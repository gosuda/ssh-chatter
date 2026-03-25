static int host_telnet_open_socket(host_t *host)
{
    if (host == nullptr || host->telnet.port[0] == '\0') {
        return -1;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    const char *bind_addr = host->telnet.bind_address[0] != '\0'
                                ? host->telnet.bind_address
                                : nullptr;
    struct addrinfo *result = nullptr;
    int rc = getaddrinfo(bind_addr, host->telnet.port, &hints, &result);
    if (rc != 0) {
        printf("[telnet] failed to resolve %s:%s (%s)\n",
               bind_addr != nullptr ? bind_addr : "*", host->telnet.port,
               gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = result; ai != nullptr; ai = ai->ai_next) {
        int candidate = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (candidate < 0) {
            continue;
        }

        int enable = 1;
        setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, &enable,
                   sizeof(enable));

        if (bind(candidate, ai->ai_addr, ai->ai_addrlen) != 0) {
            close(candidate);
            continue;
        }

        if (listen(candidate, 16) != 0) {
            close(candidate);
            continue;
        }

        fd = candidate;
        break;
    }

    freeaddrinfo(result);
    return fd;
}

static void *host_telnet_thread(void *arg)
{
    host_t *host = (host_t *)arg;
    if (host == nullptr) {
        return nullptr;
    }

    sshc_epoch_thread_enter();
    atomic_store(&host->telnet.running, true);
    struct timespec last_idle_check = session_now_monotonic();

    while (!atomic_load(&host->telnet.stop) &&
           (host->shutdown_flag == nullptr || *host->shutdown_flag == 0)) {
        host_idle_state_maintenance(host, &last_idle_check);
        if (host->telnet.fd < 0) {
            int fd = host_telnet_open_socket(host);
            if (fd < 0) {
                struct timespec backoff = {.tv_sec = 1, .tv_nsec = 0};
                host_sleep_uninterruptible(&backoff);
                continue;
            }

            host->telnet.fd = fd;
            const char *display_addr = host->telnet.bind_address[0] != '\0'
                                           ? host->telnet.bind_address
                                           : "*";
            printf("[telnet] listening on %s:%s\n", display_addr,
                   host->telnet.port);
        }

        struct sockaddr_storage addr;
        socklen_t addr_len = sizeof(addr);
        int client_fd =
            accept(host->telnet.fd, (struct sockaddr *)&addr, &addr_len);
        if (client_fd < 0) {
            int accept_error = errno;
            if (accept_error == EINTR) {
                continue;
            }
            if (atomic_load(&host->telnet.stop) ||
                (host->shutdown_flag != nullptr && *host->shutdown_flag != 0)) {
                break;
            }

            char message[256];
            const char *system_message = strerror(accept_error);
            if (system_message != nullptr && system_message[0] != '\0') {
                snprintf(message, sizeof(message), "accept failed: %s",
                         system_message);
            } else {
                snprintf(message, sizeof(message), "accept failed (code %d)",
                         accept_error);
            }
            humanized_log_error("telnet", message, accept_error);

            bool fatal_socket_error = false;
            bool should_backoff = true;
            switch (accept_error) {
            case EINTR:
                continue;
            case EAGAIN:
#ifdef EWOULDBLOCK
#if EWOULDBLOCK != EAGAIN
            case EWOULDBLOCK:
#endif
#endif
            case ECONNABORTED:
            case ECONNRESET:
            case ETIMEDOUT:
#ifdef ENOTCONN
            case ENOTCONN:
#endif
#ifdef EHOSTUNREACH
            case EHOSTUNREACH:
#endif
#ifdef ENETDOWN
            case ENETDOWN:
#endif
#ifdef ENETRESET
            case ENETRESET:
#endif
#ifdef ENETUNREACH
            case ENETUNREACH:
#endif
            case EPIPE:
#ifdef EPROTO
            case EPROTO:
#endif
                break;
            case EBADF:
            case ENOTSOCK:
            case EINVAL:
            case EMFILE:
            case ENFILE:
#ifdef ENOBUFS
            case ENOBUFS:
#endif
#ifdef ENOMEM
            case ENOMEM:
#endif
            default:
                fatal_socket_error = true;
                should_backoff = false;
                break;
            }

            if (fatal_socket_error) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);

                double elapsed_seconds = 0.0;
                if (host->telnet.last_error_time.tv_sec != 0 ||
                    host->telnet.last_error_time.tv_nsec != 0L) {
                    struct timespec elapsed =
                        timespec_diff(&now, &host->telnet.last_error_time);
                    elapsed_seconds = (double)elapsed.tv_sec +
                                      (double)elapsed.tv_nsec / 1000000000.0;
                    if (elapsed_seconds >= TELNET_STABLE_RESET_SECONDS &&
                        host->telnet.restart_attempts > 0U) {
                        printf("[telnet] listener stable for %.3f seconds; "
                               "clearing "
                               "restart backoff\n",
                               elapsed_seconds);
                        host->telnet.restart_attempts = 0U;
                    }
                }

                host->telnet.last_error_time = now;
                host->telnet.restart_attempts += 1U;

                if (host->telnet.fd >= 0) {
                    close(host->telnet.fd);
                    host->telnet.fd = -1;
                }

                if (elapsed_seconds > 0.0) {
                    printf(
                        "[telnet] last fatal error occurred %.3f seconds ago\n",
                        elapsed_seconds);
                }

                printf("[telnet] restarting listener after socket error "
                       "(attempt %u)\n",
                       host->telnet.restart_attempts);

                struct timespec restart_delay = {
                    .tv_sec =
                        host->telnet.restart_attempts < 5U
                            ? 1L
                            : (host->telnet.restart_attempts < 10U ? 5L : 30L),
                    .tv_nsec = 0L,
                };
                host_sleep_uninterruptible(&restart_delay);
                continue;
            }

            if (should_backoff) {
                struct timespec retry_delay = {
                    .tv_sec = 0,
                    .tv_nsec = 200000000L,
                };
                host_sleep_uninterruptible(&retry_delay);
            }
            continue;
        }

        if (atomic_load(&host->telnet.stop) ||
            (host->shutdown_flag != nullptr && *host->shutdown_flag != 0)) {
            close(client_fd);
            break;
        }

        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        char peer_address[NI_MAXHOST];
        host_format_sockaddr((struct sockaddr *)&addr, addr_len, peer_address,
                             sizeof(peer_address));
        if (peer_address[0] == '\0') {
            snprintf(peer_address, sizeof(peer_address), "%s", "unknown");
        }

        printf("[telnet] accepted client from %s\n", peer_address);

        session_ctx_t *ctx = session_create();
        if (ctx == nullptr) {
            humanized_log_error("telnet", "failed to allocate session context",
                                ENOMEM);
            close(client_fd);
            continue;
        }
        ctx->ops = &telnet_session_ops;
        ctx->transport_kind = SESSION_TRANSPORT_TELNET;
        ctx->telnet_fd = client_fd;
        ctx->telnet_negotiated = false;
        ctx->telnet_eof = false;
        ctx->telnet_pending_valid = false;
        pthread_mutexattr_t lock_attr;
        pthread_mutexattr_init(&lock_attr);
        pthread_mutexattr_settype(&lock_attr, PTHREAD_MUTEX_RECURSIVE);
        int mutex_error = pthread_mutex_init(&ctx->output_lock, &lock_attr);
        pthread_mutexattr_destroy(&lock_attr);
        if (mutex_error != 0) {
            humanized_log_error("telnet",
                                "failed to initialise session output lock",
                                mutex_error);
            session_destroy(ctx);
            continue;
        }
        ctx->output_lock_initialized = true;
        ctx->owner = host;
        if (ttak_mutex_init(&ctx->channel_mutex) == 0) {
            ctx->channel_mutex_initialized = true;
        } else {
            humanized_log_error("session", "failed to initialize channel mutex",
                                errno != 0 ? errno : ENOMEM);
        }
        ctx->auth = (auth_profile_t){0};
        snprintf(ctx->client_ip, sizeof(ctx->client_ip), "%.*s",
                 (int)sizeof(ctx->client_ip) - 1, peer_address);
        ctx->input_mode = SESSION_INPUT_MODE_CHAT;

        bool geo_language_enabled =
            atomic_load(&ctx->owner->geo_language_enabled);
        session_ui_language_t provider_language = SESSION_UI_LANGUAGE_COUNT;
        char provider_label[SSH_CHATTER_PROVIDER_LABEL_LEN];
        bool provider_detected =
            geo_language_enabled &&
            session_detect_provider_ip(ctx->client_ip, provider_label,
                                       sizeof(provider_label));
        if (provider_detected &&
            host_provider_language_preference(host, provider_label,
                                              &provider_language)) {
            ctx->ui_language = provider_language;
            ctx->active_codepage =
                session_codepage_for_language(provider_language);
        } else if (geo_language_enabled) {
            session_ui_language_t geo_language =
                session_client_geo_language(ctx);
            if (geo_language != SESSION_UI_LANGUAGE_COUNT) {
                ctx->ui_language = geo_language;
                ctx->active_codepage =
                    session_codepage_for_language(geo_language);
            } else {
                ctx->ui_language = SESSION_UI_LANGUAGE_EN;
                ctx->active_codepage =
                    session_codepage_for_language(SESSION_UI_LANGUAGE_EN);
            }
        } else {
            ctx->ui_language = SESSION_UI_LANGUAGE_EN;
            ctx->active_codepage =
                session_codepage_for_language(SESSION_UI_LANGUAGE_EN);
        }

        /* Favor CP437-style output for legacy telnet clients */
        ctx->prefer_cp437_output = true;

        ttak_mutex_lock(&host->lock);
        ++host->connection_count;
        ctx->user.is_operator = false;
        ctx->user.is_lan_operator = false;
        ttak_mutex_unlock(&host->lock);

        pthread_t thread_id;
        if (pthread_create(&thread_id, nullptr, session_thread, ctx) != 0) {
            humanized_log_error("telnet", "failed to spawn session thread",
                                errno);
            session_destroy(ctx);
            continue;
        }

        pthread_detach(thread_id);
    }

    int listener_fd = host->telnet.fd;
    host->telnet.fd = -1;
    if (listener_fd >= 0) {
        close(listener_fd);
    }

    atomic_store(&host->telnet.running, false);
    sshc_epoch_thread_exit();
    return nullptr;
}

static bool host_telnet_listener_start(host_t *host, const char *bind_addr,
                                       const char *port)
{
    if (host == nullptr || port == nullptr || port[0] == '\0') {
        return false;
    }

    if (host->telnet.thread_initialized) {
        const bool same_port =
            strncmp(host->telnet.port, port, sizeof(host->telnet.port)) == 0;
        bool same_bind = false;
        if (bind_addr == nullptr || bind_addr[0] == '\0') {
            same_bind = host->telnet.bind_address[0] == '\0';
        } else {
            same_bind = strncmp(host->telnet.bind_address, bind_addr,
                                sizeof(host->telnet.bind_address)) == 0;
        }

        if (same_port && same_bind && atomic_load(&host->telnet.running)) {
            const char *display_addr = host->telnet.bind_address[0] != '\0'
                                           ? host->telnet.bind_address
                                           : "*";
            printf("[telnet] listener already active on %s:%s\n", display_addr,
                   host->telnet.port);
            return true;
        }

        host_telnet_listener_stop(host);
    }

    if (bind_addr != nullptr && bind_addr[0] != '\0') {
        snprintf(host->telnet.bind_address, sizeof(host->telnet.bind_address),
                 "%s", bind_addr);
    } else {
        host->telnet.bind_address[0] = '\0';
    }
    snprintf(host->telnet.port, sizeof(host->telnet.port), "%s", port);
    host->telnet.enabled = true;
    host->telnet.fd = -1;
    host->telnet.restart_attempts = 0U;
    host->telnet.last_error_time.tv_sec = 0;
    host->telnet.last_error_time.tv_nsec = 0L;
    atomic_store(&host->telnet.stop, false);

    if (pthread_create(&host->telnet.thread, nullptr, host_telnet_thread,
                       host) != 0) {
        humanized_log_error("telnet", "failed to start telnet listener", errno);
        host->telnet.enabled = false;
        return false;
    }

    host->telnet.thread_initialized = true;
    return true;
}

static void host_telnet_listener_stop(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (!host->telnet.thread_initialized) {
        host->telnet.enabled = false;
        host->telnet.fd = -1;
        host->telnet.bind_address[0] = '\0';
        host->telnet.port[0] = '\0';
        atomic_store(&host->telnet.running, false);
        atomic_store(&host->telnet.stop, false);
        return;
    }

    const char *display_addr =
        host->telnet.bind_address[0] != '\0' ? host->telnet.bind_address : "*";
    printf("[telnet] stopping listener on %s:%s\n", display_addr,
           host->telnet.port);

    atomic_store(&host->telnet.stop, true);
    if (host->telnet.fd >= 0) {
        shutdown(host->telnet.fd, SHUT_RDWR);
    }

    int join_result = pthread_join(host->telnet.thread, nullptr);
    if (join_result != 0) {
        humanized_log_error("telnet", "failed to join telnet listener",
                            join_result);
    }

    host->telnet.thread_initialized = false;
    host->telnet.enabled = false;
    atomic_store(&host->telnet.running, false);
    atomic_store(&host->telnet.stop, false);

    if (host->telnet.fd >= 0) {
        close(host->telnet.fd);
        host->telnet.fd = -1;
    }

    host->telnet.bind_address[0] = '\0';
    host->telnet.port[0] = '\0';
}

static bool session_attempt_handshake_restart(session_ctx_t *ctx,
                                              unsigned int *attempts)
{
    if (ctx == nullptr || attempts == nullptr) {
        return false;
    }

    if (!session_transport_active(ctx)) {
        return false;
    }

    if (*attempts >= SSH_CHATTER_HANDSHAKE_RETRY_LIMIT) {
        return false;
    }

    ++(*attempts);
    printf("[session] retrying handshake (attempt %u/%u)\n", *attempts,
           SSH_CHATTER_HANDSHAKE_RETRY_LIMIT);
    session_reset_for_retry(ctx);
    session_apply_theme_defaults(ctx);
    struct timespec backoff = {
        .tv_sec = 0,
        .tv_nsec = 200000000L,
    };
    host_sleep_uninterruptible(&backoff);
    return true;
}

static void session_cleanup(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    sshc_memory_defer_gc_registration_end();

    if (ctx->user_data_loaded) {
        (void)session_user_data_commit(ctx);
    }

    session_translation_worker_shutdown(ctx);

    /* Release per-session RSS snapshot cache if the user disconnects while
     * browsing feeds. */
    session_rss_clear(ctx);

    /* Ensure multiplayer slot references are detached before the session
     * object is reclaimed so stale pointers do not remain in host state. */
    if (ctx->owner != nullptr) {
        host_t *host = ctx->owner;
        for (size_t idx = 0U; idx < SSH_CHATTER_OTHELLO_MAX_SLOTS; ++idx) {
            bool should_release = false;
            session_ctx_t *opponent = nullptr;
            othello_game_state_t snapshot = {0};
            unsigned opponent_index = 0U;
            bool had_snapshot = false;

            ttak_mutex_lock(&host->lock);
            othello_multiplayer_slot_t *slot = &host->othello_games[idx];
            if (slot->in_use &&
                (slot->players[0] == ctx || slot->players[1] == ctx)) {
                if (slot->active) {
                    had_snapshot = true;
                    snapshot = slot->state;
                    if (slot->players[0] == ctx) {
                        opponent = slot->players[1];
                        opponent_index = 1U;
                    } else {
                        opponent = slot->players[0];
                        opponent_index = 0U;
                    }
                }
                should_release = true;
                host_othello_release_slot_locked(host, slot);
            }
            ttak_mutex_unlock(&host->lock);

            if (!should_release) {
                continue;
            }

            if (opponent != nullptr) {
                if (had_snapshot) {
                    session_game_othello_sync_player_from_snapshot(
                        opponent, &snapshot, opponent_index, -1, false);
                    opponent->game.othello.game_over = true;
                }
                session_game_suspend(opponent,
                                     "Opponent disconnected. Game ended.");
            }
        }
    }

    if (ctx->display_model_initialized) {
        display_model_destroy(&ctx->display_model);
        ctx->display_model_initialized = false;
    }
    session_asciiart_buffer_release(ctx);
    session_bbs_workspace_release(ctx);
    session_bbs_view_notice_release(ctx);
    session_tetris_buffers_release(ctx);
    session_game_release_tetris(ctx);
    session_game_release_saved_tetris(ctx);
    session_safe_free((void **)&ctx->scrollback_buffer);
    ctx->scrollback_buffer_capacity = 0U;

    if (ctx->transport_kind == SESSION_TRANSPORT_SSH &&
        ctx->channel != nullptr) {
        ssh_channel_request_send_exit_status(ctx->channel, ctx->exit_status);
    }
    session_close_channel(ctx);
}

static void session_epoch_free(void *ptr)
{
    session_ctx_t *ctx = (session_ctx_t *)ptr;
    if (ctx == nullptr) {
        return;
    }

    if (ctx->channel_mutex_initialized) {
        ttak_mutex_destroy(&ctx->channel_mutex);
        ctx->channel_mutex_initialized = false;
    }

    if (ctx->output_lock_initialized) {
        /*
         * Do not destroy output_lock here.
         *
         * Broadcast paths take room-member snapshots and may still attempt to
         * lock this mutex briefly after a session begins teardown. Destroying
         * the mutex in that window can trigger glibc aborts in
         * __pthread_mutex_lock_full (lock-after-destroy UB).
         *
         * The mutex storage is embedded in session_ctx_t, so skipping destroy
         * does not leak heap memory.
         */
        ctx->output_lock_initialized = false;
    }

    if (ctx->session != nullptr) {
        ssh_disconnect(ctx->session);
        ssh_free(ctx->session);
        ctx->session = nullptr;
    }

    if (ctx->memory_context != nullptr) {
        sshc_memory_context_destroy(ctx->memory_context);
        ctx->memory_context = nullptr;
    }

    if (ctx->session_owner != nullptr) {
        ttak_owner_destroy(ctx->session_owner);
        ctx->session_owner = nullptr;
    }

    sshc_gc_free(ctx);
}

static void session_destroy(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    session_cleanup(ctx);
    if (ctx->memory_context != nullptr) {
        /*
         * Explicitly reset per-session allocations before EBR-retiring the
         * session object. This keeps join/leave bursts from accumulating large
         * deferred heaps while waiting for epoch reclamation.
         */
        sshc_memory_context_reset(ctx->memory_context);
    }
    session_manual_gc_tick(ctx);
    if (ctx->owner != nullptr) {
        host_manual_gc_tick(ctx->owner);
    }
#if defined(__GLIBC__)
    (void)malloc_trim(0);
#endif
    sshc_epoch_retire_with(ctx, session_epoch_free);
}

session_ctx_t *host_session_create_for_testing(host_t *host,
                                               const char *username,
                                               const char *ip, bool is_operator)
{
    if (host == nullptr) {
        return nullptr;
    }

    session_ctx_t *ctx = session_create();
    if (ctx == nullptr) {
        return nullptr;
    }

    ctx->owner = host;
    ctx->ops = &ssh_session_ops;
    ctx->transport_kind = SESSION_TRANSPORT_SSH;
    ctx->telnet_fd = -1;
    ctx->input_mode = SESSION_INPUT_MODE_CHAT;
    ctx->has_joined_room = true;
    ctx->user.is_authenticated = true;
    ctx->user.is_operator = is_operator;
    ctx->ui_language = SESSION_UI_LANGUAGE_EN;

    const char *resolved_name =
        (username != nullptr && username[0] != '\0') ? username : "tester";
    snprintf(ctx->user.name, sizeof(ctx->user.name), "%s", resolved_name);

    if (ip != nullptr && ip[0] != '\0') {
        snprintf(ctx->client_ip, sizeof(ctx->client_ip), "%s", ip);
    } else {
        snprintf(ctx->client_ip, sizeof(ctx->client_ip), "%s", "127.0.0.1");
    }

    pthread_mutexattr_t lock_attr;
    pthread_mutexattr_init(&lock_attr);
    pthread_mutexattr_settype(&lock_attr, PTHREAD_MUTEX_RECURSIVE);
    if (pthread_mutex_init(&ctx->output_lock, &lock_attr) != 0) {
        pthread_mutexattr_destroy(&lock_attr);
        session_destroy(ctx);
        return nullptr;
    }
    pthread_mutexattr_destroy(&lock_attr);
    ctx->output_lock_initialized = true;

    if (ttak_mutex_init(&ctx->channel_mutex) != 0) {
        session_destroy(ctx);
        return nullptr;
    }
    ctx->channel_mutex_initialized = true;

    session_apply_theme_defaults(ctx);

    return ctx;
}

void host_session_destroy_for_testing(session_ctx_t *ctx)
{
    session_destroy(ctx);
}

static void *session_thread(void *arg)
{
    session_ctx_t *ctx = (session_ctx_t *)arg;
    if (ctx == nullptr) {
        return nullptr;
    }

    sshc_epoch_thread_enter();

    sshc_memory_context_t *memory_scope = session_memory_scope_push(ctx);
    sshc_memory_defer_gc_registration_begin();

#define SESSION_THREAD_RETURN(value)                                           \
    do {                                                                       \
        if (memory_scope != nullptr) {                                         \
            session_memory_scope_pop(memory_scope);                            \
        }                                                                      \
        sshc_epoch_thread_exit();                                              \
        return (value);                                                        \
    } while (0)

#define SESSION_THREAD_ERROR_EXIT()                                            \
    do {                                                                       \
        session_destroy(ctx);                                                  \
        SESSION_THREAD_RETURN(nullptr);                                        \
    } while (0)

    ctx->exit_status = EXIT_FAILURE;
    session_apply_theme_defaults(ctx);

    bool authenticated = false;
    unsigned int handshake_retries = 0U;
    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        session_telnet_initialize(ctx);
        session_telnet_capture_startup_metadata(ctx);
        authenticated = true;
    }

    while (ctx->transport_kind == SESSION_TRANSPORT_SSH) {
        if (!authenticated) {
            if (session_authenticate(ctx) != 0) {
                humanized_log_error("session", "authentication failed", EACCES);
                SESSION_THREAD_ERROR_EXIT();
            }
            authenticated = true;
            ctx->user.is_authenticated = true;
            if (ctx->password_not_set) {
                session_send_raw_text(
                    ctx, "Welcome! Your account has no password set. Please "
                         "use the /password command to set one.\r\n");
            }
        }

        if (session_accept_channel(ctx) != 0) {
            humanized_log_error("session", "failed to open channel", EIO);
            if (session_attempt_handshake_restart(ctx, &handshake_retries)) {
                continue;
            }
            SESSION_THREAD_ERROR_EXIT();
        }

        int shell_result = session_prepare_shell(ctx);
        if (shell_result < 0) {
            humanized_log_error("session", "shell negotiation failed", EPROTO);
            if (session_attempt_handshake_restart(ctx, &handshake_retries)) {
                continue;
            }
            SESSION_THREAD_ERROR_EXIT();
        } else if (shell_result > 0) {
            SESSION_THREAD_ERROR_EXIT();
        }

        break;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_SSH) {
        session_install_channel_callbacks(ctx);
        session_render_prelogin_banner(ctx);
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        if (!session_telnet_prompt_unicode_check(ctx)) {
            session_send_system_line(
                ctx, "Well, you didn't answer. Proceeding with Unicode...");
        }
    }

    session_assign_lan_privileges(ctx);
    session_apply_granted_privileges(ctx);
    session_apply_saved_preferences(ctx);

    char preferred_nickname_raw[SSH_CHATTER_USERNAME_LEN] = {0};
    char preferred_nickname[SSH_CHATTER_USERNAME_LEN] = {0};
    if (ctx->user.is_authenticated && ctx->user_data_loaded &&
        ctx->user_data.preferred_nickname[0] != '\0') {
        snprintf(preferred_nickname_raw, sizeof(preferred_nickname_raw), "%s",
                 ctx->user_data.preferred_nickname);
        snprintf(preferred_nickname, sizeof(preferred_nickname), "%s",
                 preferred_nickname_raw);
        if (!user_data_strip_ansi_sequences(preferred_nickname_raw,
                                            preferred_nickname,
                                            sizeof(preferred_nickname))) {
            snprintf(preferred_nickname, sizeof(preferred_nickname), "%s",
                     preferred_nickname_raw);
        }

        trim_whitespace_inplace(preferred_nickname_raw);
        trim_whitespace_inplace(preferred_nickname);
        if (preferred_nickname_raw[0] == '\0' &&
            preferred_nickname[0] != '\0') {
            snprintf(preferred_nickname_raw, sizeof(preferred_nickname_raw),
                     "%s", preferred_nickname);
        }

        const char *nick_to_apply = preferred_nickname_raw[0] != '\0'
                                        ? preferred_nickname_raw
                                        : preferred_nickname;

        if (nick_to_apply[0] != '\0' &&
            strcasecmp(ctx->user.name, nick_to_apply) != 0) {
            char nick_command[SSH_CHATTER_MAX_INPUT_LEN];
            snprintf(nick_command, sizeof(nick_command), "/nick %s",
                     nick_to_apply);
            if (ctx->ops != nullptr && ctx->ops->dispatch_command != nullptr) {
                ctx->ops->dispatch_command(ctx, nick_command);
            }
        }
    }

    bool captcha_enabled = false;
    if (ctx->owner != nullptr) {
        captcha_enabled = atomic_load(&ctx->owner->captcha_enabled);
    }
    const bool captcha_exempt = session_is_captcha_exempt(ctx);
    if (captcha_enabled && !captcha_exempt && !session_run_captcha(ctx)) {
        SESSION_THREAD_ERROR_EXIT();
    }

    if (host_is_ip_banned(ctx->owner, ctx->client_ip)) {
        session_send_system_line(ctx, "You are banned from this server.");
        SESSION_THREAD_ERROR_EXIT();
    }

    const bool banned_username =
        host_is_username_banned(ctx->owner, ctx->user.name);
    const bool system_reserved_username =
        host_is_system_reserved_username(ctx->owner, ctx->user.name);
    const bool lan_operator_reserved_username =
        host_is_lan_operator_username(ctx->owner, ctx->user.name);
    session_ctx_t *existing =
        chat_room_find_user(&ctx->owner->room, ctx->user.name);

    // Check if same IP and nickname - allow reconnection by kicking existing session
    bool should_kick_existing = false;
    if (existing != nullptr && !banned_username && !system_reserved_username &&
        !lan_operator_reserved_username) {
        if (strcmp(existing->client_ip, ctx->client_ip) == 0) {
            should_kick_existing = true;
            printf("[reconnect] same IP+nickname detected: %s from %s, kicking "
                   "existing session\n",
                   ctx->user.name, ctx->client_ip);
        }
    }

    if (should_kick_existing) {
        // Kick the existing session
        char kick_message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(kick_message, sizeof(kick_message),
                 "You have been disconnected because a new connection from the "
                 "same IP is joining.");
        session_force_disconnect(existing, kick_message);

        // Wait a moment for the disconnect to process
        struct timespec disconnect_delay = {.tv_sec = 0,
                                            .tv_nsec = 100000000L}; // 100ms
        nanosleep(&disconnect_delay, nullptr);

        // Clear the existing reference as it's being disconnected
        existing = nullptr;
    }

    if (banned_username || system_reserved_username ||
        (lan_operator_reserved_username && !ctx->user.is_lan_operator) ||
        existing != nullptr || strnlen(ctx->user.name, 64) < 2) {
        ctx->username_conflict = true;

        if (banned_username) {
            printf("[reject] banned nickname attempted: %s\n", ctx->user.name);
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message),
                     "The nickname '%s' is blocked for bot detection.",
                     ctx->user.name);
            session_send_system_line(ctx, message);
            session_send_system_line(
                ctx,
                "Reconnect with a different nickname before logging in again.");
            session_send_system_line(ctx, "Type /exit to quit.");
        } else if (system_reserved_username) {
            printf("[reject] system reserved username requested: %s\n",
                   ctx->user.name);
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message),
                     "The username '%s' is reserved for system use.",
                     ctx->user.name);
            session_send_system_line(ctx, message);
            session_send_system_line(
                ctx, "Reconnect with a different username by running: ssh "
                     "newname@<server> (or ssh -l newname <server>).");
            session_send_system_line(ctx, "Type /exit to quit.");
        } else if (lan_operator_reserved_username &&
                   !ctx->user.is_lan_operator) {
            printf("[reject] LAN operator reserved username requested: %s\n",
                   ctx->user.name);
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message),
                     "The username '%s' is reserved for LAN operators.",
                     ctx->user.name);
            session_send_system_line(ctx, message);
            session_send_system_line(
                ctx, "Reconnect with a different username by running: ssh "
                     "newname@<server> (or ssh -l newname <server>).");
            session_send_system_line(ctx, "Type /exit to quit.");
        } else {
            printf("[reject] username in use: %s\n", ctx->user.name);
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message),
                     "The username '%s' is already in use.", ctx->user.name);
            session_send_system_line(ctx, message);
            session_send_system_line(
                ctx, "Reconnect with a different username by running: ssh "
                     "newname@<server> (or ssh -l newname <server>).");
            session_send_system_line(ctx, "Type /exit to quit.");
        }
        session_force_disconnect(ctx, "Disconnecting...");
        SESSION_THREAD_ERROR_EXIT();
    } else {
        host_join_attempt_result_t join_result = host_register_join_attempt(
            ctx->owner, ctx->user.name, ctx->client_ip);
        if (join_result == HOST_JOIN_ATTEMPT_BAN) {
            session_send_system_line(
                ctx, "Rapid reconnect detected. You have been banned.");
            SESSION_THREAD_ERROR_EXIT();
        }
        if (join_result == HOST_JOIN_ATTEMPT_KICK) {
            session_send_system_line(
                ctx, "Rapid reconnect detected. You have been kicked.");
            SESSION_THREAD_ERROR_EXIT();
        }

        session_send_system_line(ctx, "Wait for a moment...");
        session_send_system_line(
            ctx, "Note: I recommend UTF-8 for elegant multilingual support.");
        session_send_system_line(
            ctx, "~~~ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZ~~~");
    /* Duplicate the MOTD string because strtok_r modifies the source string */
        char *motd_copy = sshc_strdup(ctx->owner->motd);
        if (motd_copy != NULL) {
            char *saveptr;
            char *line = strtok_r(motd_copy, "\n", &saveptr);
        
            while (line != NULL) {
                session_send_system_line(ctx, line);
                line = strtok_r(NULL, "\n", &saveptr);
            }
        
            /* Free the allocated copy after processing */
            sshc_gc_free(motd_copy);
        }
        session_send_system_line(
            ctx, "For TELNET users: type /motd and follow the guide.");
        session_render_banner(ctx);
        struct timespec wait_time = {0, 0};
        size_t progress = host_prepare_join_delay(ctx->owner, &wait_time);
        if (progress == 0U) {
            progress = 1U;
        }
        if (progress > SSH_CHATTER_JOIN_BAR_MAX) {
            progress = SSH_CHATTER_JOIN_BAR_MAX;
        }
        char loading_line[SSH_CHATTER_MESSAGE_LIMIT];
        size_t written = 0U;
        for (size_t idx = 0;
             idx < progress && written + 1U < sizeof(loading_line); ++idx) {
            loading_line[written++] = '=';
        }
        if (written + 1U < sizeof(loading_line)) {
            loading_line[written++] = '>';
        }
        loading_line[written] = '\0';
        session_send_system_line(ctx, loading_line);
        if (wait_time.tv_sec != 0 || wait_time.tv_nsec != 0) {
            host_sleep_uninterruptible(&wait_time);
        }
        chat_room_add(&ctx->owner->room, ctx);
        session_manual_gc_tick(ctx);
        host_manual_gc_tick(ctx->owner);
        host_reload_cached_state(ctx->owner);
        ctx->has_joined_room = true;
        printf("[join] %s\n", ctx->user.name);

        ctx->history_scroll_position = 0U;
        host_refresh_motd(ctx->owner);
        const session_ui_locale_t *locale = session_ui_get_locale(ctx);
        const char *prefix = session_command_prefix(ctx);

        session_send_system_line(
            ctx,
            "Chat history starts hidden. Press the UpArrow/DownArrow keys to "
            "load older messages when you need them.");
        if (locale->help_scroll_hint != nullptr &&
            locale->help_scroll_hint[0] != '\0') {
            session_send_system_line(ctx, locale->help_scroll_hint);
        }

        if (locale->welcome_help_hint != nullptr &&
            locale->welcome_help_hint[0] != '\0') {
            const char *args[] = {prefix};
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            session_format_template(locale->welcome_help_hint, args,
                                    sizeof(args) / sizeof(args[0]), message,
                                    sizeof(message));
            session_send_system_line(ctx, message);
        }

        if (locale->help_hint_extra != nullptr &&
            locale->help_hint_extra[0] != '\0') {
            const char *args[] = {prefix};
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            session_format_template(locale->help_hint_extra, args,
                                    sizeof(args) / sizeof(args[0]), message,
                                    sizeof(message));
            session_send_system_line(ctx, message);
        }

        if (locale->mode_usage != nullptr && locale->mode_usage[0] != '\0') {
            const char *args[] = {prefix};
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            session_format_template(locale->mode_usage, args,
                                    sizeof(args) / sizeof(args[0]), message,
                                    sizeof(message));
            session_send_system_line(ctx, message);
        }

        if (locale->mode_explain_command != nullptr &&
            locale->mode_explain_command[0] != '\0') {
            const char *args[] = {prefix};
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            session_format_template(locale->mode_explain_command, args,
                                    sizeof(args) / sizeof(args[0]), message,
                                    sizeof(message));
            session_send_system_line(ctx, message);
        }

        char bbs_hint[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(bbs_hint, sizeof(bbs_hint),
                 "This room is alive. Use %sbbs list to browse posts.", prefix);
        session_send_system_line(ctx, bbs_hint);

        char slow_contact[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(slow_contact, sizeof(slow_contact),
                 "Replies may be slow. [MORSE] feed is OFF by default; %smorse on to "
                 "enable and %smorse-chat <text> to send Morse.",
                 prefix, prefix);
        session_send_system_line(ctx, slow_contact);

        // Add retro command hint
        {
            char retro_hint[SSH_CHATTER_MESSAGE_LIMIT];
            const char *retro_msg = nullptr;
            switch (ctx->ui_language) {
            case SESSION_UI_LANGUAGE_KO:
                retro_msg = "레트로 터미널 인코딩: %sretro on "
                            "[ko|en|jp|zh|ru|de|fr|pl]";
                break;
            case SESSION_UI_LANGUAGE_JP:
                retro_msg = "レトロターミナルエンコーディング: %sretro on "
                            "[ko|en|jp|zh|ru|de|fr|pl]";
                break;
            case SESSION_UI_LANGUAGE_ZH:
                retro_msg =
                    "复古终端编码: %sretro on [ko|en|jp|zh|ru|de|fr|pl]";
                break;
            case SESSION_UI_LANGUAGE_RU:
                retro_msg = "Ретро кодировка терминала: %sretro on "
                            "[ko|en|jp|zh|ru|de|fr|pl]";
                break;
            case SESSION_UI_LANGUAGE_DE:
                retro_msg = "Retro-Terminal-Codierung: %sretro on "
                            "[ko|en|jp|zh|ru|de|fr|pl]";
                break;
            case SESSION_UI_LANGUAGE_FR:
                retro_msg = "Encodage de terminal rétro: %sretro on "
                            "[ko|en|jp|zh|ru|de|fr|pl]";
                break;
            case SESSION_UI_LANGUAGE_PL:
                retro_msg = "Kodowanie terminala retro: %sretro on "
                            "[ko|en|jp|zh|ru|de|fr|pl]";
                break;
            case SESSION_UI_LANGUAGE_EN:
            default:
                retro_msg = "Retro terminal encoding: %sretro on "
                            "[ko|en|jp|zh|ru|de|fr|pl]";
                break;
            }
            const char *args[] = {prefix};
            session_format_template(retro_msg, args,
                                    sizeof(args) / sizeof(args[0]), retro_hint,
                                    sizeof(retro_hint));
            session_send_system_line(ctx, retro_hint);
            session_send_system_line(ctx, "NOTE: UTF-8 is recommended to see Korean RSS without dirty broken letters");
        }

        char join_message[SSH_CHATTER_MESSAGE_LIMIT];
        if (ctx->user_data_loaded &&
            strnlen(ctx->user_data.preferred_nickname,
                    sizeof(ctx->user_data.preferred_nickname)) != 0U) {
            snprintf(join_message, sizeof(join_message),
                     "%s%s*%s [%.*s] has joined the chat", ANSI_RESET, ANSI_BRIGHT_RED, ANSI_RESET, (int)sizeof(ctx->user_data.preferred_nickname), ctx->user_data.preferred_nickname);
        } else {
            snprintf(join_message, sizeof(join_message),
                     "%s%s*%s [%.*s] has joined the chat", ANSI_RESET, ANSI_BRIGHT_RED, ANSI_RESET, (int)sizeof(ctx->user.name), ctx->user.name);
        }
        host_history_record_system(ctx->owner, join_message, nullptr);
        chat_room_broadcast(&ctx->owner->room, join_message, nullptr);
    }

    session_clear_input_without_prompt(ctx);
    session_render_prompt(ctx, true);

    char buffer[SSH_CHATTER_MAX_INPUT_LEN];
    const int poll_timeout_ms = 100;
    while (!ctx->should_exit) {
        struct timespec lifetime_now = session_now_monotonic();
        if (session_enforce_lifetime(ctx, &lifetime_now)) {
            break;
        }
        session_translation_flush_ready(ctx);

        if (ctx->game.active && ctx->game.type == SESSION_GAME_TETRIS) {
            session_game_tetris_process_timeout(ctx);
        }

        int read_result =
            session_transport_read(ctx, buffer, sizeof(buffer) - 1U, 200);
        if (read_result == SSH_AGAIN) {
            continue;
        }
        if (read_result == SSH_ERROR) {
            if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
                break;
            }
            read_result = session_channel_read_poll(
                ctx, buffer, sizeof(buffer) - 1U, poll_timeout_ms);
            if (read_result == SESSION_CHANNEL_TIMEOUT) {
                ctx->channel_error_retries = 0U;
                continue;
            }

            if (read_result == SSH_ERROR) {
                const char *error_message = ssh_get_error(ctx->session);
                bool unexpected_bytes_error = false;
                if (error_message != nullptr && error_message[0] != '\0') {
                    unexpected_bytes_error =
                        strstr(error_message,
                               "unexpected bytes remain after decoding") !=
                        nullptr;
                }

                if (unexpected_bytes_error) {
                    const char *username =
                        ctx->user.name[0] != '\0' ? ctx->user.name : "unknown";
                    printf("[session] channel decode error for %s: %s\n",
                           username, error_message);
                    break;
                }

                bool remote_disconnect = !session_transport_is_open(ctx) ||
                                         session_transport_is_eof(ctx);
                if (!remote_disconnect && error_message != nullptr &&
                    error_message[0] != '\0') {
                    static const char *const kNetworkDisconnectTokens[] = {
                        "ssh_msg_disconnect", "disconnected by",
                        "connection reset",   "connection closed",
                        "broken pipe",        "socket error",
                    };
                    for (size_t token_idx = 0;
                         token_idx < (sizeof(kNetworkDisconnectTokens) /
                                      sizeof(kNetworkDisconnectTokens[0]));
                         ++token_idx) {
                        if (string_contains_case_insensitive(
                                error_message,
                                kNetworkDisconnectTokens[token_idx])) {
                            remote_disconnect = true;
                            break;
                        }
                    }
                }

                if (remote_disconnect) {
                    const char *username =
                        ctx->user.name[0] != '\0' ? ctx->user.name : "unknown";
                    const char *message =
                        (error_message != nullptr && error_message[0] != '\0')
                            ? error_message
                            : "connection closed";
                    printf("[session] channel closed for %s: %s\n", username,
                           message);
                    break;
                }

                if (ctx->has_joined_room &&
                    ctx->channel_error_retries <
                        SSH_CHATTER_CHANNEL_RECOVERY_LIMIT) {
                    ctx->channel_error_retries += 1U;
                    if (error_message == nullptr || error_message[0] == '\0') {
                        error_message = "unknown channel error";
                    }
                    printf("[session] channel read error for %s (attempt "
                           "%u/%u): %s\n",
                           ctx->user.name, ctx->channel_error_retries,
                           SSH_CHATTER_CHANNEL_RECOVERY_LIMIT, error_message);
                    struct timespec retry_delay = {
                        .tv_sec = 0,
                        .tv_nsec = SSH_CHATTER_CHANNEL_RECOVERY_DELAY_NS,
                    };
                    host_sleep_uninterruptible(&retry_delay);
                    continue;
                }

                if (ctx->has_joined_room) {
                    if (error_message == nullptr || error_message[0] == '\0') {
                        error_message = "unknown channel error";
                    }
                    printf("[session] channel read failure for %s after %u "
                           "retries: %s\n",
                           ctx->user.name, ctx->channel_error_retries,
                           error_message);
                }
                break;
            }
            continue;
        }

        if (read_result == 0) {
            if (!session_transport_is_open(ctx) ||
                session_transport_is_eof(ctx)) {
                break;
            }
            if (ctx->game.active && ctx->game.type == SESSION_GAME_TETRIS) {
                session_game_tetris_process_timeout(ctx);
            }
            continue;
        }

        ctx->channel_error_retries = 0U;

        if (read_result == 0) {
            break;
        }
        if (read_result < 0) {
            continue;
        }

        if (read_result > 0) {
            session_mark_activity(ctx);
        }

        for (int idx = 0; idx < read_result; ++idx) {
            const char ch = buffer[idx];

            if (ctx->game.active && ctx->game.type == SESSION_GAME_TETRIS) {
                if (session_game_tetris_process_raw_input(ctx, ch)) {
                    continue;
                }
            }

            if (session_consume_escape_sequence(ctx, ch)) {
                continue;
            }

            if (ch == 0x01) {
                if (ctx->bbs_post_pending || ctx->asciiart_pending) {
                    ctx->input_buffer[ctx->input_length] = '\0';
                    session_apply_background_fill(ctx);
                    if (ctx->bbs_post_pending) {
                        if (ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART) {
                            session_asciiart_cancel(ctx,
                                                    "ASCII art draft canceled.");
                        } else {
                            const char *cancel_notice =
                                (ctx->editor_mode ==
                                 SESSION_EDITOR_MODE_BBS_EDIT)
                                    ? "BBS edit canceled."
                                    : "BBS draft canceled.";
                            session_bbs_reset_pending_post(ctx);
                            session_send_system_line(ctx, cancel_notice);
                        }
                    } else {
                        session_asciiart_cancel(ctx,
                                                "ASCII art draft canceled.");
                    }
                    session_clear_input_without_prompt(ctx);
                    if (ctx->should_exit) {
                        break;
                    }
                    session_render_prompt(ctx, false);
                }
                continue;
            }

            if (ctx->transport_kind == SESSION_TRANSPORT_TELNET && ch == 0x00) {
                // Ignore stray NUL padding bytes from telnet clients so they do
                // not trigger unintended disconnects.
                continue;
            }

            if (ch == 0x03) {
                ctx->input_buffer[ctx->input_length] = '\0';
                session_apply_background_fill(ctx);
                session_handle_exit(ctx);
                session_clear_input_without_prompt(ctx);
                if (ctx->should_exit) {
                    break;
                }
                session_render_prompt(ctx, false);
                continue;
            }

            if (ch == 0x1a) {
                ctx->input_buffer[ctx->input_length] = '\0';
                session_apply_background_fill(ctx);
                if (ctx->in_rss_mode) {
                    session_rss_exit(ctx, nullptr);
                    session_clear_input_without_prompt(ctx);
                    if (ctx->should_exit) {
                        break;
                    }
                    session_render_prompt(ctx, false);
                } else if (ctx->game.active) {
                    bool handled = false;
                    if (ctx->game.type == SESSION_GAME_OTHELLO &&
                        ctx->game.othello.multiplayer) {
                        handled = session_game_othello_handle_forced_exit(ctx);
                    }
                    if (!handled) {
                        session_game_suspend(ctx, "Game suspended.");
                    }
                    session_clear_input_without_prompt(ctx);
                    if (ctx->should_exit) {
                        break;
                    }
                    session_render_prompt(ctx, false);
                } else {
                    session_handle_exit(ctx);
                    session_clear_input_without_prompt(ctx);
                    if (ctx->should_exit) {
                        break;
                    }
                    session_render_prompt(ctx, false);
                }
                continue;
            }

            if (ch == 0x06) {
                if (ctx->bbs_post_pending) {
                    ctx->bbs_search_active = true;
                    ctx->bbs_search_restore_line = ctx->pending_bbs_cursor_line;
                    ctx->bbs_search_restore_editing =
                        ctx->pending_bbs_editing_line;
                    ctx->bbs_search_restore_scroll =
                        ctx->bbs_editor_scroll_offset;
                    session_clear_input_without_prompt(ctx);
                    session_bbs_render_editor(
                        ctx, "Search keyword: type text and press Enter.");
                }
                continue;
            }

            if (ch == 0x0f) {
                if (ctx->bbs_post_pending) {
                    ctx->input_buffer[ctx->input_length] = '\0';
                    char status[SSH_CHATTER_MESSAGE_LIMIT];
                    status[0] = '\0';
                    size_t insert_index = ctx->pending_bbs_cursor_line;
                    if (ctx->pending_bbs_editing_line) {
                        insert_index = ctx->pending_bbs_cursor_line;
                    }
                    if (!session_bbs_insert_line(ctx, insert_index,
                                                 ctx->input_buffer, status,
                                                 sizeof(status))) {
                        if (status[0] == '\0') {
                            snprintf(status, sizeof(status),
                                     "Unable to insert the line.");
                        }
                    } else {
                        session_bbs_set_cursor(ctx, insert_index + 1U, false);
                        if (status[0] == '\0') {
                            snprintf(status, sizeof(status),
                                     "Inserted line at %zu.", insert_index + 1U);
                        }
                    }
                    ctx->bbs_line_edit_mode = false;
                    session_clear_input_without_prompt(ctx);
                    session_bbs_render_editor(ctx, status);
                }
                continue;
            }

            if (ch == 0x11) {
                if (ctx->bbs_post_pending) {
                    session_bbs_recalculate_line_count(ctx);
                    size_t line_count = ctx->pending_bbs_line_count;
                    char status[SSH_CHATTER_MESSAGE_LIMIT];
                    status[0] = '\0';
                    if (line_count == 0U) {
                        snprintf(status, sizeof(status),
                                 "No lines available to select.");
                    } else {
                        size_t anchor = ctx->pending_bbs_cursor_line;
                        if (anchor >= line_count) {
                            anchor = line_count - 1U;
                        }
                        ctx->bbs_editor_selection_start = anchor;
                        ctx->bbs_editor_selection_start_set = true;
                        ctx->bbs_editor_selection_end_set = false;
                        snprintf(status, sizeof(status),
                                 "Selection start set to line %zu.",
                                 anchor + 1U);
                    }
                    ctx->bbs_line_edit_mode = false;
                    session_bbs_render_editor(ctx, status);
                }
                continue;
            }

            if (ch == 0x12) {
                if (ctx->bbs_post_pending) {
                    session_bbs_recalculate_line_count(ctx);
                    size_t line_count = ctx->pending_bbs_line_count;
                    char status[SSH_CHATTER_MESSAGE_LIMIT];
                    status[0] = '\0';
                    if (!ctx->bbs_editor_selection_start_set || line_count == 0U) {
                        snprintf(status, sizeof(status),
                                 "Selection start not set.");
                    } else {
                        size_t end = ctx->pending_bbs_cursor_line;
                        if (end >= line_count) {
                            end = line_count - 1U;
                        }
                        ctx->bbs_editor_selection_end = end;
                        ctx->bbs_editor_selection_end_set = true;
                        if (session_bbs_copy_line_range(
                                ctx, ctx->bbs_editor_selection_start,
                                ctx->bbs_editor_selection_end,
                                ctx->bbs_editor_clipboard,
                                SSH_CHATTER_BBS_BODY_LEN,
                                &ctx->bbs_editor_clipboard_lines)) {
                            ctx->bbs_editor_clipboard_length =
                                strlen(ctx->bbs_editor_clipboard);
                            if (session_bbs_remove_line_range(
                                    ctx, ctx->bbs_editor_selection_start,
                                    ctx->bbs_editor_selection_end, status,
                                    sizeof(status))) {
                                session_bbs_recalculate_line_count(ctx);
                                size_t new_count =
                                    ctx->pending_bbs_line_count;
                                size_t target =
                                    ctx->bbs_editor_selection_start;
                                if (target > new_count) {
                                    target = new_count;
                                }
                                session_bbs_set_cursor(ctx, target, false);
                                snprintf(status, sizeof(status),
                                         "Cut lines %zu-%zu.",
                                         ctx->bbs_editor_selection_start + 1U,
                                         ctx->bbs_editor_selection_end + 1U);
                            }
                        } else {
                            snprintf(status, sizeof(status),
                                     "Failed to copy selected lines.");
                        }
                        ctx->bbs_editor_selection_start_set = false;
                        ctx->bbs_editor_selection_end_set = false;
                    }
                    ctx->bbs_line_edit_mode = false;
                    session_clear_input_without_prompt(ctx);
                    session_bbs_render_editor(ctx, status);
                }
                continue;
            }

            if (ch == 0x14) {
                if (ctx->bbs_post_pending) {
                    char status[SSH_CHATTER_MESSAGE_LIMIT];
                    status[0] = '\0';
                    if (!session_bbs_insert_clipboard(ctx, status,
                                                      sizeof(status))) {
                        if (status[0] == '\0') {
                            snprintf(status, sizeof(status),
                                     "Unable to paste clipboard.");
                        }
                    } else if (status[0] == '\0') {
                        snprintf(status, sizeof(status), "Pasted clipboard.");
                    }
                    ctx->bbs_line_edit_mode = false;
                    session_clear_input_without_prompt(ctx);
                    session_bbs_render_editor(ctx, status);
                }
                continue;
            }

            if (ch == 0x0c) {
                if (ctx->bbs_post_pending && ctx->bbs_line_edit_mode) {
                    ctx->input_buffer[ctx->input_length] = '\0';
                    char status[SSH_CHATTER_MESSAGE_LIMIT];
                    status[0] = '\0';
                    size_t target = ctx->bbs_line_edit_target;
                    if (ctx->pending_bbs_editing_line) {
                        target = ctx->pending_bbs_cursor_line;
                    }
                    if (!session_bbs_replace_line(ctx, target,
                                                  ctx->input_buffer, status,
                                                  sizeof(status))) {
                        if (status[0] == '\0') {
                            snprintf(status, sizeof(status),
                                     "Unable to update that line.");
                        }
                    }
                    ctx->bbs_line_edit_mode = false;
                    session_clear_input_without_prompt(ctx);
                    session_bbs_render_editor(ctx, status);
                } else if (!ctx->bbs_post_pending && !ctx->asciiart_pending &&
                           !ctx->game.active && !ctx->in_rss_mode) {
                    // Ctrl+L in normal chat mode: clean screen redraw
                    // without disconnecting/reconnecting.
                    session_flag_should_sink(ctx);
                }
                continue;
            }

            if (ch == 0x13) {
                if (ctx->bbs_post_pending || ctx->asciiart_pending) {
                    ctx->input_buffer[ctx->input_length] = '\0';
                    bool had_body = ctx->input_length > 0U;
                    if (had_body) {
                        session_local_echo_char(ctx, '\n');
                        session_history_record(ctx, ctx->input_buffer);
                        if (ctx->bbs_post_pending) {
                            session_bbs_capture_body_text(ctx,
                                                          ctx->input_buffer);
                        } else {
                            session_asciiart_capture_text(ctx,
                                                          ctx->input_buffer);
                        }
                    } else {
                        session_local_echo_char(ctx, '\n');
                    }

                    const char *terminator =
                        ctx->bbs_post_pending
                            ? session_editor_terminator(ctx)
                            : session_asciiart_terminator(ctx);
                    for (const char *cursor = terminator; *cursor != '\0';
                         ++cursor) {
                        session_local_echo_char(ctx, *cursor);
                    }
                    session_local_echo_char(ctx, '\n');
                    session_history_record(ctx, terminator);
                    if (ctx->bbs_post_pending) {
                        session_bbs_capture_body_line(ctx, terminator);
                    } else {
                        session_asciiart_capture_line(ctx, terminator);
                    }
                    session_clear_input_without_prompt(ctx);
                    if (ctx->should_exit) {
                        break;
                    }
                    session_render_prompt(ctx, false);
                } else if (ctx->game.active &&
                           ctx->game.type == SESSION_GAME_ALPHA) {
                    session_game_alpha_manual_save(ctx);
                }
                continue;
            }

            if (ch == '\r' && ctx->bbs_post_pending &&
                ctx->pending_bbs_editing_line && ctx->input_length == 0U &&
                !ctx->bbs_line_edit_mode && !ctx->bracket_paste_active) {
                session_bbs_recalculate_line_count(ctx);
                char status[SSH_CHATTER_MESSAGE_LIMIT];
                status[0] = '\0';
                if (ctx->pending_bbs_line_count == 0U) {
                    snprintf(status, sizeof(status),
                             "No lines available to edit.");
                } else {
                    size_t target = ctx->pending_bbs_cursor_line;
                    if (target >= ctx->pending_bbs_line_count) {
                        target = ctx->pending_bbs_line_count - 1U;
                    }
                    session_bbs_set_cursor(ctx, target, true);
                    ctx->bbs_line_edit_mode = true;
                    ctx->bbs_line_edit_target = target;
                    snprintf(status, sizeof(status),
                             "Line edit mode for line %zu. Ctrl+L to apply.",
                             target + 1U);
                }
                session_bbs_render_editor(ctx, status);
                continue;
            }

            if (ch == '\r' || ch == '\n') {
                session_apply_background_fill(ctx);
                const bool composing_draft =
                    ctx->bbs_post_pending || ctx->asciiart_pending;
                if (ctx->bbs_search_active) {
                    ctx->input_buffer[ctx->input_length] = '\0';
                    char status[SSH_CHATTER_MESSAGE_LIMIT];
                    status[0] = '\0';
                    session_bbs_search_keyword(ctx, ctx->input_buffer, status,
                                               sizeof(status));
                    ctx->bbs_search_active = false;
                    session_clear_input_without_prompt(ctx);
                    session_bbs_render_editor(ctx, status);
                    continue;
                }
                if (ctx->input_length > 0U) {
                    ctx->input_buffer[ctx->input_length] = '\0';
                    session_history_record(ctx, ctx->input_buffer);
                    session_process_line(ctx, ctx->input_buffer);
                } else if (composing_draft) {
                    session_process_line(ctx, "");
                }
                session_clear_input_without_prompt(ctx);
                if (ctx->should_exit) {
                    break;
                }
                if (!ctx->bracket_paste_active) {
                    session_render_prompt(ctx, false);
                }
                continue;
            }

            if (ch == '\b' || ch == 0x7f) {
                ctx->input_history_position = -1;
                session_scrollback_reset_position(ctx);
                session_local_backspace(ctx);
                /* Reset multi-byte buffer on backspace */
                ctx->multibyte_input_length = 0U;
                continue;
            }

            if (ch == '\t') {
                if (session_try_command_completion(ctx)) {
                    continue;
                }
                if (ctx->input_length + 1U < sizeof(ctx->input_buffer)) {
                    ctx->input_history_position = -1;
                    session_scrollback_reset_position(ctx);
                    ctx->input_buffer[ctx->input_length++] = ' ';
                    session_local_echo_char(ctx, ' ');
                }
                continue;
            }

            if ((unsigned char)ch < 0x20U) {
                continue;
            }

            char encoded[8];
            size_t encoded_len = 0U;
            if (ctx->cp437_input_enabled) {
                encoded_len = session_codepage_byte_to_utf8(
                    ctx->active_codepage, &ctx->codepage_ctx, (unsigned char)ch,
                    encoded, sizeof(encoded));
                if (encoded_len == 0U) {
                    encoded[0] = '?';
                    encoded_len = 1U;
                }
            } else {
                encoded[0] = ch;
                encoded_len = 1U;
            }

            if (ctx->input_length + encoded_len >= sizeof(ctx->input_buffer)) {
                ctx->input_buffer[sizeof(ctx->input_buffer) - 1U] = '\0';
                session_history_record(ctx, ctx->input_buffer);
                session_process_line(ctx, ctx->input_buffer);
                session_clear_input_without_prompt(ctx);
                if (ctx->should_exit) {
                    break;
                }
                session_render_prompt(ctx, false);
            }

            if (ctx->input_length + encoded_len < sizeof(ctx->input_buffer)) {
                ctx->input_history_position = -1;
                session_scrollback_reset_position(ctx);
                memcpy(&ctx->input_buffer[ctx->input_length], encoded,
                       encoded_len);
                ctx->input_length += encoded_len;
                ctx->input_buffer[ctx->input_length] = '\0';
                for (size_t echo_idx = 0U; echo_idx < encoded_len; ++echo_idx) {
                    session_local_echo_char(ctx, encoded[echo_idx]);
                }
            }
        }

        if (ctx->should_exit) {
            break;
        }
    }

    session_translation_flush_ready(ctx);

    if (!ctx->should_exit && ctx->input_length > 0U) {
        ctx->input_buffer[ctx->input_length] = '\0';
        session_history_record(ctx, ctx->input_buffer);
        session_process_line(ctx, ctx->input_buffer);
        session_clear_input(ctx);
    }

    if (ctx->has_joined_room) {
        printf("[part] %s\n", ctx->user.name);
        char part_message[SSH_CHATTER_MESSAGE_LIMIT];
        if(strnlen(ctx->user_data.preferred_nickname, 256) != 0) {
            snprintf(part_message, sizeof(part_message), "%s%s*%s [%s] has left the chat",
                     ANSI_RESET, ANSI_BRIGHT_BLUE, ANSI_RESET, ctx->user_data.preferred_nickname);
        } else {
            snprintf(part_message, sizeof(part_message), "%s%s*%s [%s] has left the chat",
                     ANSI_RESET, ANSI_BRIGHT_BLUE, ANSI_RESET, ctx->user.name);
        }
        host_history_record_system(ctx->owner, part_message, nullptr);
        chat_room_broadcast(&ctx->owner->room, part_message, nullptr);
        chat_room_remove(&ctx->owner->room, ctx);
        session_manual_gc_tick(ctx);
        host_manual_gc_tick(ctx->owner);

        /* Allow in-flight broadcasts that already captured this session in
         * their snapshot to finish writing before we destroy the channel
         * and mutexes.  Without this pause, a concurrent broadcast thread
         * could dereference the freed channel or a destroyed mutex. */
        struct timespec drain_delay = {.tv_sec = 0, .tv_nsec = 50000000L};
        nanosleep(&drain_delay, nullptr);
    }

    session_destroy(ctx);

    SESSION_THREAD_RETURN(nullptr);

#undef SESSION_THREAD_RETURN
}

void host_init(host_t *host, auth_profile_t *auth)
{
    if (host == nullptr) {
        return;
    }

    sshc_memory_context_t *memory_scope =
        sshc_memory_context_push(host->memory_context);

    translator_global_init();

    memset(&host->eliza_worker, 0, sizeof(host->eliza_worker));
    if (!host_eliza_worker_init(host)) {
        printf("[eliza] asynchronous intervention worker unavailable; "
               "interventions disabled.\n");
    }

    if (!host_moderation_init(host)) {
        printf("[security] moderation worker unavailable; using synchronous "
               "checks\n");
    }

    chat_room_init(&host->room);
    host->idle_state_pending = false;
    host->last_room_empty_time = session_now_monotonic();
    host->listener.handle = nullptr;
    host->listener.inplace_recoveries = 0U;
    host->listener.restart_attempts = 0U;
    host->listener.accept_error_streak = 0U;
    host->listener.last_error_time.tv_sec = 0;
    host->listener.last_error_time.tv_nsec = 0L;
    host->telnet.enabled = false;
    host->telnet.fd = -1;
    host->telnet.thread_initialized = false;
    atomic_store(&host->telnet.running, false);
    atomic_store(&host->telnet.stop, false);
    host->telnet.restart_attempts = 0U;
    host->telnet.last_error_time.tv_sec = 0;
    host->telnet.last_error_time.tv_nsec = 0L;
    host->telnet.bind_address[0] = '\0';
    host->telnet.port[0] = '\0';
    host->json_api.enabled = false;
    host->json_api.fd = -1;
    host->json_api.thread_initialized = false;
    atomic_store(&host->json_api.running, false);
    atomic_store(&host->json_api.stop, false);
    host->json_api.restart_attempts = 0U;
    host->json_api.last_error_time.tv_sec = 0;
    host->json_api.last_error_time.tv_nsec = 0L;
    host->json_api.bind_address[0] = '\0';
    host->json_api.port[0] = '\0';
    
    const char *env_secret = getenv("JWT_SECRET");
    if (env_secret && env_secret[0] != '\0') {
        snprintf(host->jwt_secret, sizeof(host->jwt_secret), "%s", env_secret);
    } else {
        snprintf(host->jwt_secret, sizeof(host->jwt_secret), "ssh-chatter-secret-key-change-me");
    }

    host->auth = auth;
    host->clients = nullptr;
    host->web_client = nullptr;
    host->morse_client = nullptr;
    host->security_layer_initialized =
        security_layer_init(&host->security_layer);
    if (!host->security_layer_initialized) {
        humanized_log_error("security",
                            "failed to initialise layered message encryption",
                            errno != 0 ? errno : EIO);
    }
    atomic_store(&host->geo_language_enabled, false);
    host_load_lan_operator_credentials(host);
    const palette_descriptor_t *default_palette =
        palette_find_descriptor("blackpink");
    if (default_palette != nullptr) {
        host_apply_palette_descriptor(host, default_palette);
    } else {
        static char userColor[32];
        static char highlight[32];
        static char bgColor[32];
        static char fgColor[32];
        static char hlColor[32];

        ansi_256(userColor, sizeof(userColor), MONOKAI_BLUE_USER_COLOR);
        ansi_bg_256(highlight, sizeof(highlight), 0);
        ansi_bg_256(bgColor, sizeof(bgColor), MONOKAI_BLUE_BACKGROUND_COLOR);
        ansi_256(fgColor, sizeof(fgColor), MONOKAI_BLUE_FOREGROUND_COLOR);
        ansi_bg_256(hlColor, sizeof(hlColor), MONOKAI_BLUE_HIGHLIGHT_COLOR);

        host->user_theme.userColor = userColor;
        host->user_theme.highlight = highlight;
        host->user_theme.isBold = false;
        host->system_theme.backgroundColor = bgColor;
        host->system_theme.foregroundColor = fgColor;
        host->system_theme.highlightColor = hlColor;
        host->system_theme.isBold = true;
        snprintf(host->default_user_color_name,
                 sizeof(host->default_user_color_name), "%s", "green");
        snprintf(host->default_user_highlight_name,
                 sizeof(host->default_user_highlight_name), "%s", "default");
        snprintf(host->default_system_fg_name,
                 sizeof(host->default_system_fg_name), "%s", "white");
        snprintf(host->default_system_bg_name,
                 sizeof(host->default_system_bg_name), "%s", "blue");
        snprintf(host->default_system_highlight_name,
                 sizeof(host->default_system_highlight_name), "%s", "yellow");
    }
    host->ban_count = 0U;
    memset(host->bans, 0, sizeof(host->bans));
    memset(host->replies, 0, sizeof(host->replies));
    host->reply_count = 0U;
    host->next_reply_id = 1U;
    memset(host->eliza_memory, 0, sizeof(host->eliza_memory));
    host->eliza_memory_count = 0U;
    host->eliza_memory_next_id = 1U;
    host->version_ip_ban_rules = nullptr;
    host->version_ip_ban_rule_count = 0U;
    host->version_ip_ban_rule_capacity = 0U;
    snprintf(host->version, sizeof(host->version),
             "ssh-chatter (C, rolling release)");
    snprintf(host->motd_base, sizeof(host->motd_base),
             "Welcome to ssh-chat!\n"
             "- Be polite to each other\n"
             "- fun fact: this server is written in pure c.\n"
             "============================================\n"
             " _      ____  ____  _____ ____  _        ____  _ \n"
             "/ \\__/|/  _ \\/  _ \\/  __//  __\\/ \\  /|  /   _\\/ \\\n"
             "| |\\/||| / \\|| | \\||  \\  |  \\/|| |\\ ||  |  /  | |\n"
             "| |  ||| \\_/|| |_/||  /_ |    /| | \\||  |  \\__\\_/\n"
             "\\_/  \\|\\____/\\____/\\____\\\\_/\\_\\\\_/  \\|  \\____/(_)\n"
             "                                                 \n"
             "============================================\n");
    snprintf(host->motd, sizeof(host->motd), "%s", host->motd_base);
    host->motd_path[0] = '\0';
    host->motd_has_file = false;
    host->motd_last_modified.tv_sec = 0;
    host->motd_last_modified.tv_nsec = 0L;
    host->welcome_banner[0] = '\0';
    host->welcome_banner_loaded = false;

    host->translation_quota_exhausted = false;
    host->connection_count = 0U;
    host->history = nullptr;
    host->history_count = 0U;
    host->history_capacity = 0U;
    host->history_cache_loaded = false;
    host->next_message_id = 1U;
    memset(host->preferences, 0, sizeof(host->preferences));
    host->preference_count = 0U;
    host->state_file_path[0] = '\0';
    host_state_resolve_path(host);
    host->sync_state_file_path[0] = '\0';
    host_sync_state_resolve_path(host);
    host->bbs_state_file_path[0] = '\0';
    host_bbs_resolve_path(host);
    host->vote_state_file_path[0] = '\0';
    host_vote_resolve_path(host);
    host->ban_state_file_path[0] = '\0';
    host_ban_resolve_path(host);
    host->reply_state_file_path[0] = '\0';
    host_reply_state_resolve_path(host);
    host->ui_lang_state_file_path[0] = '\0';
    host_ui_language_state_resolve_path(host);
    host->pw_auth_file_path[0] = '\0';
    host_pw_auth_resolve_path(host);
    host->alpha_landers_file_path[0] = '\0';
    host_alpha_landers_resolve_path(host);
    snprintf(host->user_data_root, sizeof(host->user_data_root), "%s",
             "/var/lib/mailbox");
    host->user_data_ready = user_data_ensure_root(host->user_data_root);
    if (ttak_mutex_init(&host->user_data_lock) == 0) {
        host->user_data_lock_initialized = true;
    } else {
        humanized_log_error("mailbox", "failed to initialise mailbox lock",
                            errno != 0 ? errno : ENOMEM);
        host->user_data_lock_initialized = false;
        host->user_data_ready = false;
    }
    if (ttak_mutex_init(&host->alpha_landers_lock) == 0) {
        host->alpha_landers_lock_initialized = true;
    } else {
        humanized_log_error("alpha", "failed to initialise alpha landers lock",
                            errno != 0 ? errno : ENOMEM);
        host->alpha_landers_lock_initialized = false;
    }
    host->file_storage_ready = host_file_storage_init(host);
    host->rss_state_file_path[0] = '\0';
    host_rss_resolve_path(host);
    host->eliza_memory_file_path[0] = '\0';
    host_eliza_memory_resolve_path(host);
    host->eliza_state_file_path[0] = '\0';
    host_eliza_state_resolve_path(host);
    host->bbs_watchdog_thread_initialized = false;
    atomic_store(&host->bbs_watchdog_thread_running, false);
    atomic_store(&host->bbs_watchdog_thread_stop, false);
    host->bbs_watchdog_last_run.tv_sec = 0;
    host->bbs_watchdog_last_run.tv_nsec = 0;
    host->rss_thread_initialized = false;
    atomic_store(&host->rss_thread_running, false);
    atomic_store(&host->rss_thread_stop, false);
    host->rss_last_run.tv_sec = 0;
    host->rss_last_run.tv_nsec = 0L;
    atomic_store(&host->rss_manual_refresh_running, false);
    host->rss_refresh_lock_initialized = false;
    if (ttak_mutex_init(&host->rss_refresh_lock) == 0) {
        host->rss_refresh_lock_initialized = true;
    } else {
        humanized_log_error("rss", "failed to initialise refresh lock",
                            errno != 0 ? errno : ENOMEM);
    }
    host->archive_thread_initialized = false;
    atomic_store(&host->archive_thread_running, false);
    atomic_store(&host->archive_thread_stop, false);
    host->archive_last_run.tv_sec = 0;
    host->archive_last_run.tv_nsec = 0L;
    host_security_configure(host);
    host_version_ip_rules_init(host);
    memset(host->protected_ips, 0, sizeof(host->protected_ips));
    host->protected_ip_count = 0U;
    ttak_mutex_init(&host->lock);
    host_protected_ips_bootstrap(host);
    poll_state_reset(&host->poll);
    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_NAMED_POLLS; ++idx) {
        named_poll_reset(&host->named_polls[idx]);
    }
    host->named_poll_count = 0U;
    host->bbs_cache_loaded = false;
    if (!host_bbs_acquire_storage(host)) {
        host->bbs_post_capacity = 0U;
    }
    host->bbs_post_count = 0U;
    host->next_bbs_id = 1U;
    for (size_t idx = 0U; idx < SSH_CHATTER_RSS_MAX_FEEDS; ++idx) {
        host_clear_rss_feed(&host->rss_feeds[idx]);
    }
    host->rss_feed_count = 0U;
    for (size_t idx = 0U; idx < SSH_CHATTER_OTHELLO_MAX_SLOTS; ++idx) {
        othello_multiplayer_slot_t *slot = &host->othello_games[idx];
        slot->in_use = false;
        slot->active = false;
        slot->awaiting_second_player = false;
        slot->slot_id = (uint16_t)(idx + 1U);
        slot->owner[0] = '\0';
        slot->players[0] = nullptr;
        slot->players[1] = nullptr;
        slot->state = (othello_game_state_t){0};
        slot->state.slot_index = -1;
    }
    host->random_seeded = false;
    memset(host->operator_grants, 0, sizeof(host->operator_grants));
    host->operator_grant_count = 0U;
    host->reserved_nicknames = nullptr;
    host->reserved_nicknames_len = 0U;
    host->reserved_nicknames_capacity = 0U;
    host->next_join_ready_time = (struct timespec){0, 0};
    host->join_throttle_initialised = false;
    host->join_progress_length = 0U;
    host->join_activity = nullptr;
    host->join_activity_count = 0U;
    host->join_activity_capacity = 0U;
    host->connection_guard = nullptr;
    host->connection_guard_count = 0U;
    host->connection_guard_capacity = 0U;
    host->health_guard.consecutive_errors = 0U;
    host->health_guard.last_error_time.tv_sec = 0;
    host->health_guard.last_error_time.tv_nsec = 0L;
    atomic_store(&host->captcha_enabled, false);
    host->captcha_nonce = 0U;
    host->has_last_captcha = false;
    host->last_captcha_question[0] = '\0';
    host->last_captcha_answer[0] = '\0';
    host->last_captcha_generated.tv_sec = 0;
    host->last_captcha_generated.tv_nsec = 0L;
    host->reserved_nicknames =
        (char(*)[SSH_CHATTER_USERNAME_LEN])sshc_gc_calloc(
            SSH_CHATTER_MAX_RESERVED_NAMES,
            sizeof(host->reserved_nicknames[0]));
    if (host->reserved_nicknames != nullptr) {
        host->reserved_nicknames_capacity = SSH_CHATTER_MAX_RESERVED_NAMES;
    } else {
        host->reserved_nicknames_capacity = 0U;
        humanized_log_error("host", "failed to allocate reserved nicknames",
                            ENOMEM);
    }
    if (ttak_mutex_init(&host->nickname_reserve_lock) != 0) {
        humanized_log_error("host", "failed to initialise nickname lock",
                            errno != 0 ? errno : ENOMEM);
    }
    atomic_store(&host->eliza_enabled, false);
    atomic_store(&host->eliza_announced, false);
    host->eliza_last_action.tv_sec = 0;
    host->eliza_last_action.tv_nsec = 0L;
    atomic_store(&host->ai_chat_enabled, false);
    host->ai_chat_last_reply.tv_sec = 0;
    host->ai_chat_last_reply.tv_nsec = 0L;
    host->ai_chat_model[0] = '\0';
    memset(host->ai_chat_memory, 0, sizeof(host->ai_chat_memory));
    host->ai_chat_memory_count = 0U;
    const char *env_ollama_model = getenv("CHATTER_OLLAMA_MODEL");
    if (env_ollama_model != nullptr && env_ollama_model[0] != '\0') {
        snprintf(host->ai_chat_model, sizeof(host->ai_chat_model), "%s",
                 env_ollama_model);
    }

    (void)host_try_load_motd_from_path(host, "/etc/ssh-chatter/motd");

    host_state_load(host);
    host->history_cache_loaded =
        host->history != nullptr && host->history_capacity > 0U;
    host_ui_language_state_load(host);
    host_vote_state_load(host);
    host_bbs_state_load(host);
    host->bbs_cache_loaded = host_bbs_storage_ready(host);
    host_ban_state_load(host);
    host_reply_state_load(host);
    host_rss_state_load(host);
    host_eliza_memory_load(host);
    host_eliza_state_load(host);

    host_user_data_bootstrap(host);

    host_refresh_motd(host);

    host->clients = client_manager_create(host);
    if (host->clients == nullptr) {
        humanized_log_error("host", "failed to create client manager", ENOMEM);
    } else {
        host->web_client = webssh_client_create(host, host->clients);
        if (host->web_client == nullptr) {
            humanized_log_error("host", "failed to initialise webssh client",
                                ENOMEM);
        }

    }
    if (host->morse_client == nullptr) {
        host->morse_client = morse_client_create(host);
        if (host->morse_client == nullptr) {
            printf("[morse] relay inactive; connection will not start.\n");
        }
    }
    host_bbs_start_watchdog(host);
    host_rss_start_backend(host);
    host_archive_start_backend(host);
    sshc_memory_context_pop(memory_scope);
}

static void host_build_birthday_notice_locked(host_t *host, char *line,
                                              size_t length)
{
    if (line == nullptr || length == 0U) {
        return;
    }

    line[0] = '\0';

    if (host == nullptr) {
        return;
    }

    time_t now = time(nullptr);
    if (now == (time_t)-1) {
        return;
    }

    struct tm local_now;
    if (localtime_r(&now, &local_now) == nullptr) {
        return;
    }

    struct tm today_tm = local_now;
    today_tm.tm_hour = 0;
    today_tm.tm_min = 0;
    today_tm.tm_sec = 0;
    today_tm.tm_isdst = -1;
    time_t today = mktime(&today_tm);
    if (today == (time_t)-1) {
        today = now;
    }

    char names[SSH_CHATTER_MESSAGE_LIMIT];
    names[0] = '\0';
    size_t name_count = 0U;

    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_PREFERENCES; ++idx) {
        const user_preference_t *pref = &host->preferences[idx];
        if (!pref->in_use || !pref->has_birthday) {
            continue;
        }
        if (pref->username[0] == '\0' || pref->birthday[0] == '\0') {
            continue;
        }

        int month = 0;
        int day = 0;
        if (sscanf(pref->birthday, "%*d-%d-%d", &month, &day) != 2) {
            continue;
        }
        if (month < 1 || month > 12 || day < 1 || day > 31) {
            continue;
        }

        int use_day = day;
        int use_month = month;
        const int current_year = local_now.tm_year + 1900;
        if (use_month == 2 && use_day == 29 &&
            !host_is_leap_year(current_year)) {
            use_day = 28;
        }

        struct tm birthday_tm = today_tm;
        birthday_tm.tm_year = local_now.tm_year;
        birthday_tm.tm_mon = use_month - 1;
        birthday_tm.tm_mday = use_day;
        birthday_tm.tm_hour = 0;
        birthday_tm.tm_min = 0;
        birthday_tm.tm_sec = 0;
        birthday_tm.tm_isdst = -1;
        time_t birthday_time = mktime(&birthday_tm);
        if (birthday_time == (time_t)-1) {
            continue;
        }

        time_t diff = today - birthday_time;
        if (diff < 0) {
            birthday_tm.tm_year -= 1;
            birthday_tm.tm_isdst = -1;
            birthday_time = mktime(&birthday_tm);
            if (birthday_time == (time_t)-1) {
                continue;
            }
            diff = today - birthday_time;
        }

        if (diff < 0 || diff >= (time_t)SSH_CHATTER_BIRTHDAY_WINDOW_SECONDS) {
            continue;
        }

        size_t current_len = strnlen(names, sizeof(names));
        const size_t name_len = strnlen(pref->username, sizeof(pref->username));
        if (name_len == 0U) {
            continue;
        }

        if (current_len > 0U) {
            if (current_len + 2U >= sizeof(names)) {
                continue;
            }
            names[current_len++] = ',';
            names[current_len++] = ' ';
            names[current_len] = '\0';
        }

        if (name_len >= sizeof(names) - current_len) {
            continue;
        }

        memcpy(names + current_len, pref->username, name_len);
        current_len += name_len;
        names[current_len] = '\0';
        ++name_count;
    }

    if (name_count == 0U) {
        return;
    }

    snprintf(line, length, "Happy birthday to %s!\n", names);
}

static void host_refresh_motd_locked(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    char birthday_line[SSH_CHATTER_MESSAGE_LIMIT];
    host_build_birthday_notice_locked(host, birthday_line,
                                      sizeof(birthday_line));

    if (birthday_line[0] != '\0') {
        snprintf(host->motd, sizeof(host->motd), "%s%s", birthday_line,
                 host->motd_base);
    } else {
        snprintf(host->motd, sizeof(host->motd), "%s", host->motd_base);
    }
}

static void host_refresh_motd(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    host_maybe_reload_motd_from_file(host);

    ttak_mutex_lock(&host->lock);
    host_refresh_motd_locked(host);
    ttak_mutex_unlock(&host->lock);
}

static bool host_try_load_motd_from_path(host_t *host, const char *path)
{
    if (host == nullptr || path == nullptr || path[0] == '\0') {
        return false;
    }

    FILE *motd_file = fopen(path, "r");
    if (motd_file == nullptr) {
        return false;
    }

    struct stat info;
    bool have_info = false;
    struct timespec modified = {0, 0};
    if (fstat(fileno(motd_file), &info) == 0) {
        modified = host_stat_mtime(&info);
        if (modified.tv_sec != 0 || modified.tv_nsec != 0) {
            have_info = true;
        }
    }

    char motd_buffer[sizeof(host->motd)];
    motd_buffer[0] = '\0';
    size_t current_motd_len = 0U;
    char line_buffer[SSH_CHATTER_MESSAGE_LIMIT];

    while (fgets(line_buffer, sizeof(line_buffer), motd_file) != nullptr) {
        size_t line_len = strlen(line_buffer);
        while (line_len > 0U && (line_buffer[line_len - 1U] == '\n' ||
                                 line_buffer[line_len - 1U] == '\r')) {
            line_buffer[--line_len] = '\0';
        }

        // Prepend [200~ and append [201~ for bracketed paste mode
        const char *start_paste = "\033[200~";
        const char *end_paste = "\033[201~";
        size_t start_paste_len = strlen(start_paste);
        size_t end_paste_len = strlen(end_paste);

        size_t required_len =
            start_paste_len + line_len + end_paste_len + 1U; // +1 for newline

        if (current_motd_len + required_len + 1U >=
            sizeof(motd_buffer)) { // +1 for null terminator
            // MOTD buffer is full, stop reading
            break;
        }

        // Append start_paste
        memcpy(motd_buffer + current_motd_len, start_paste, start_paste_len);
        current_motd_len += start_paste_len;

        // Append line content
        memcpy(motd_buffer + current_motd_len, line_buffer, line_len);
        current_motd_len += line_len;

        // Append end_paste
        memcpy(motd_buffer + current_motd_len, end_paste, end_paste_len);
        current_motd_len += end_paste_len;

        // Append newline
        motd_buffer[current_motd_len++] = '\n';
        motd_buffer[current_motd_len] = '\0';
    }

    if (fclose(motd_file) != 0) {
        const int close_error = errno;
        humanized_log_error("host", "failed to close motd file", close_error);
    }

    ttak_mutex_lock(&host->lock);
    snprintf(host->motd_base, sizeof(host->motd_base), "%s", motd_buffer);
    snprintf(host->motd_path, sizeof(host->motd_path), "%s", path);
    host->motd_has_file = true;
    if (have_info) {
        host->motd_last_modified = modified;
    } else {
        host->motd_last_modified.tv_sec = 0;
        host->motd_last_modified.tv_nsec = 0L;
    }
    host_refresh_motd_locked(host);
    ttak_mutex_unlock(&host->lock);
    return true;
}

void host_set_motd(host_t *host, const char *motd)
{
    sshc_memory_context_t *memory_scope = nullptr;
    if (host != nullptr) {
        memory_scope = sshc_memory_context_push(host->memory_context);
    }
    if (host == nullptr || motd == nullptr) {
        goto exit_host_set_motd;
    }

    char motd_path[PATH_MAX];
    motd_path[0] = '\0';
    snprintf(motd_path, sizeof(motd_path), "%s", motd);
    trim_whitespace_inplace(motd_path);

    const size_t max_paths = 2U;
    const char *paths_to_try[2] = {nullptr, nullptr};
    size_t path_count = 0U;

    char expanded_path[PATH_MAX];
    expanded_path[0] = '\0';
    if (motd_path[0] == '~') {
        const char *home = getenv("HOME");
        if (home != nullptr && home[0] != '\0' &&
            (motd_path[1] == '\0' || motd_path[1] == '/')) {
            const int written = snprintf(expanded_path, sizeof(expanded_path),
                                         "%s%s", home, motd_path + 1);
            if (written > 0 && (size_t)written < sizeof(expanded_path) &&
                path_count < max_paths) {
                paths_to_try[path_count++] = expanded_path;
            }
        }
    }

    if (motd_path[0] != '\0') {
        if (path_count < max_paths) {
            paths_to_try[path_count++] = motd_path;
        }
    }

    for (size_t idx = 0U; idx < path_count; ++idx) {
        if (paths_to_try[idx] != nullptr &&
            host_try_load_motd_from_path(host, paths_to_try[idx])) {
            goto exit_host_set_motd;
        }
    }

    char normalized[sizeof(host->motd)];
    snprintf(normalized, sizeof(normalized), "%s", motd);
    session_normalize_newlines(normalized);

    ttak_mutex_lock(&host->lock);
    if (motd_path[0] != '\0') {
        snprintf(host->motd_path, sizeof(host->motd_path), "%s", motd_path);
    } else {
        host->motd_path[0] = '\0';
    }
    host->motd_has_file = false;
    host->motd_last_modified.tv_sec = 0;
    host->motd_last_modified.tv_nsec = 0L;
    snprintf(host->motd_base, sizeof(host->motd_base), "%s", normalized);
    host_refresh_motd_locked(host);
    ttak_mutex_unlock(&host->lock);

exit_host_set_motd:
    if (memory_scope != nullptr) {
        sshc_memory_context_pop(memory_scope);
    }
}

static bool host_prepare_chat_entry(host_t *host, const char *username,
                                    const char *message, const char *color_name,
                                    const char *highlight_name, bool is_bold,
                                    chat_history_entry_t *entry)
{
    if (host == nullptr || username == nullptr || username[0] == '\0' ||
        message == nullptr || message[0] == '\0' || entry == nullptr) {
        return false;
    }

    memset(entry, 0, sizeof(*entry));
    entry->is_user_message = true;
    snprintf(entry->username, sizeof(entry->username), "%s", username);
    snprintf(entry->message, sizeof(entry->message), "%s", message);
    entry->attachment_type = CHAT_ATTACHMENT_NONE;
    entry->user_is_bold = is_bold;
    time_t now = time(nullptr);
    if (now != (time_t)-1) {
        entry->created_at = now;
    }

    const char *color_label = (color_name != nullptr && color_name[0] != '\0')
                                  ? color_name
                                  : host->default_user_color_name;
    snprintf(entry->user_color_name, sizeof(entry->user_color_name), "%s",
             color_label);
    const char *highlight_label =
        (highlight_name != nullptr && highlight_name[0] != '\0')
            ? highlight_name
            : host->default_user_highlight_name;
    snprintf(entry->user_highlight_name, sizeof(entry->user_highlight_name),
             "%s", highlight_label);

    const char *color_code = lookup_color_code(
        USER_COLOR_MAP, sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
        color_label);
    const char *highlight_code = lookup_color_code(
        HIGHLIGHT_COLOR_MAP,
        sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
        highlight_label);

    const char *effective_color =
        color_code != nullptr ? color_code : host->user_theme.userColor;
    const char *effective_highlight =
        highlight_code != nullptr ? highlight_code : host->user_theme.highlight;

    if (effective_color != nullptr) {
        snprintf(entry->user_color_code, sizeof(entry->user_color_code), "%s",
                 effective_color);
    } else {
        entry->user_color_code[0] = '\0';
    }

    if (effective_highlight != nullptr) {
        snprintf(entry->user_highlight_code, sizeof(entry->user_highlight_code),
                 "%s", effective_highlight);
    } else {
        entry->user_highlight_code[0] = '\0';
    }

    return true;
}

void host_append_sync_log(host_t *host, const char *source, const char *message)
{
    if (host == nullptr || source == nullptr || source[0] == '\0' ||
        message == nullptr || message[0] == '\0') {
        return;
    }

    if (host->sync_state_file_path[0] == '\0') {
        return;
    }

    FILE *fp = fopen(host->sync_state_file_path, "a");
    if (fp == nullptr) {
        humanized_log_error("sync", "failed to open sync state file",
                            errno != 0 ? errno : EIO);
        return;
    }

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    fprintf(fp, "%ld|%s|%s\n", (long)now.tv_sec, source, message);
    fclose(fp);
}

static const char *host_ai_chat_default_model(void)
{
    return "gemma2:2b";
}

static bool host_ai_chat_message_mentions_bot(const char *text)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    return string_contains_case_insensitive(text, "ai-eliza");
}

static bool host_ai_chat_should_respond(const chat_history_entry_t *entry)
{
    if (entry == nullptr || !entry->is_user_message) {
        return false;
    }
    if (entry->message[0] == '\0' || entry->message[0] == '/') {
        return false;
    }
    if (strncasecmp(entry->username, "ai-eliza",
                    SSH_CHATTER_USERNAME_LEN) == 0) {
        return false;
    }
    return host_ai_chat_message_mentions_bot(entry->message);
}

static void host_ai_chat_snapshot_state(host_t *host, char *model,
                                        size_t model_len,
                                        struct timespec *last_reply)
{
    if (host == nullptr) {
        if (model != nullptr && model_len > 0U) {
            snprintf(model, model_len, "%s", host_ai_chat_default_model());
        }
        if (last_reply != nullptr) {
            last_reply->tv_sec = 0;
            last_reply->tv_nsec = 0L;
        }
        return;
    }

    ttak_mutex_lock(&host->lock);
    if (model != nullptr && model_len > 0U) {
        const char *source = host->ai_chat_model[0] != '\0'
                                 ? host->ai_chat_model
                                 : host_ai_chat_default_model();
        snprintf(model, model_len, "%s", source);
    }
    if (last_reply != nullptr) {
        *last_reply = host->ai_chat_last_reply;
    }
    ttak_mutex_unlock(&host->lock);
}

static void host_ai_chat_update_last_reply(host_t *host,
                                           const struct timespec *now)
{
    if (host == nullptr || now == nullptr) {
        return;
    }
    ttak_mutex_lock(&host->lock);
    host->ai_chat_last_reply = *now;
    ttak_mutex_unlock(&host->lock);
}

static size_t host_ai_chat_memory_collect_tokens(const char *prompt,
                                                 char tokens[][32],
                                                 size_t max_tokens)
{
    if (prompt == nullptr || tokens == nullptr || max_tokens == 0U) {
        return 0U;
    }

    size_t count = 0U;
    size_t length = strlen(prompt);
    size_t idx = 0U;
    while (idx < length && count < max_tokens) {
        while (idx < length && isspace((unsigned char)prompt[idx])) {
            ++idx;
        }
        if (idx >= length) {
            break;
        }

        size_t token_idx = 0U;
        char buffer[32];
        while (idx < length && !isspace((unsigned char)prompt[idx])) {
            unsigned char ch = (unsigned char)prompt[idx];
            if (token_idx + 1U < sizeof(buffer)) {
                buffer[token_idx++] =
                    (ch < 0x80U) ? (char)tolower(ch) : (char)ch;
            }
            ++idx;
        }
        buffer[token_idx] = '\0';

        if (token_idx == 0U) {
            continue;
        }
        if (token_idx < 3U && (unsigned char)buffer[0] < 0x80U) {
            continue;
        }

        bool duplicate = false;
        for (size_t existing = 0U; existing < count; ++existing) {
            if (strcmp(tokens[existing], buffer) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        snprintf(tokens[count], 32U, "%s", buffer);
        ++count;
    }

    return count;
}

static void host_ai_chat_memory_prepare_preview(const char *source, char *dest,
                                                size_t dest_length)
{
    if (dest == nullptr || dest_length == 0U) {
        return;
    }

    dest[0] = '\0';
    if (source == nullptr || source[0] == '\0') {
        return;
    }

    size_t copy_length = strnlen(source, dest_length);
    bool truncated = false;
    if (copy_length >= dest_length) {
        copy_length = dest_length - 1U;
        truncated = true;
    }

    memcpy(dest, source, copy_length);
    dest[copy_length] = '\0';
    trim_whitespace_inplace(dest);

    if (truncated && dest_length > 4U) {
        size_t length = strnlen(dest, dest_length);
        if (length + 3U < dest_length) {
            dest[length++] = '.';
            dest[length++] = '.';
            dest[length++] = '.';
            dest[length] = '\0';
        }
    }
}

static size_t host_ai_chat_memory_collect_context(host_t *host,
                                                  const char *prompt,
                                                  char *context,
                                                  size_t context_length)
{
    if (context == nullptr || context_length == 0U) {
        return 0U;
    }

    context[0] = '\0';
    if (host == nullptr || prompt == nullptr) {
        return 0U;
    }

    ai_chat_memory_entry_t snapshot[SSH_CHATTER_AI_MEMORY_MAX];
    size_t snapshot_count = 0U;

    ttak_mutex_lock(&host->lock);
    snapshot_count = host->ai_chat_memory_count;
    if (snapshot_count > SSH_CHATTER_AI_MEMORY_MAX) {
        snapshot_count = SSH_CHATTER_AI_MEMORY_MAX;
    }
    if (snapshot_count > 0U) {
        memcpy(snapshot, host->ai_chat_memory,
               snapshot_count * sizeof(snapshot[0]));
    }
    ttak_mutex_unlock(&host->lock);

    if (snapshot_count == 0U) {
        return 0U;
    }

    char tokens[SSH_CHATTER_AI_MEMORY_TOKEN_LIMIT][32];
    size_t token_count = host_ai_chat_memory_collect_tokens(
        prompt, tokens, SSH_CHATTER_AI_MEMORY_TOKEN_LIMIT);

    size_t best_indices[SSH_CHATTER_AI_MEMORY_CONTEXT_LIMIT] = {0U};
    size_t best_scores[SSH_CHATTER_AI_MEMORY_CONTEXT_LIMIT] = {0U};
    size_t best_count = 0U;

    for (size_t idx = 0U; idx < snapshot_count; ++idx) {
        const ai_chat_memory_entry_t *entry = &snapshot[idx];
        size_t score = 0U;
        if (token_count > 0U) {
            for (size_t token_idx = 0U; token_idx < token_count; ++token_idx) {
                if (tokens[token_idx][0] == '\0') {
                    continue;
                }
                if (string_contains_case_insensitive(entry->prompt,
                                                     tokens[token_idx]) ||
                    string_contains_case_insensitive(entry->reply,
                                                     tokens[token_idx])) {
                    ++score;
                }
            }
            if (score == 0U) {
                continue;
            }
        }

        size_t recency_bonus = snapshot_count - idx;
        if (recency_bonus > 4U) {
            recency_bonus = 4U;
        }
        score += recency_bonus;

        size_t insert_pos = best_count;
        if (best_count < SSH_CHATTER_AI_MEMORY_CONTEXT_LIMIT) {
            ++best_count;
        } else if (score <=
                   best_scores[SSH_CHATTER_AI_MEMORY_CONTEXT_LIMIT - 1U]) {
            continue;
        } else {
            insert_pos = SSH_CHATTER_AI_MEMORY_CONTEXT_LIMIT - 1U;
        }

        while (insert_pos > 0U && score > best_scores[insert_pos - 1U]) {
            if (insert_pos < SSH_CHATTER_AI_MEMORY_CONTEXT_LIMIT) {
                best_scores[insert_pos] = best_scores[insert_pos - 1U];
                best_indices[insert_pos] = best_indices[insert_pos - 1U];
            }
            --insert_pos;
        }

        best_scores[insert_pos] = score;
        best_indices[insert_pos] = idx;
    }

    if (best_count == 0U && token_count == 0U) {
        size_t fallback = snapshot_count < SSH_CHATTER_AI_MEMORY_CONTEXT_LIMIT
                              ? snapshot_count
                              : SSH_CHATTER_AI_MEMORY_CONTEXT_LIMIT;
        for (size_t idx = 0U; idx < fallback; ++idx) {
            best_indices[idx] = snapshot_count - idx - 1U;
        }
        best_count = fallback;
    }

    if (best_count == 0U) {
        return 0U;
    }

    size_t offset = 0U;
    for (size_t idx = 0U; idx < best_count; ++idx) {
        const ai_chat_memory_entry_t *entry = &snapshot[best_indices[idx]];
        char time_buffer[32];
        time_buffer[0] = '\0';
        if (entry->stored_at != 0) {
            struct tm tm_value;
            if (localtime_r(&entry->stored_at, &tm_value) != nullptr) {
                strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M",
                         &tm_value);
            }
        }
        if (time_buffer[0] == '\0') {
            snprintf(time_buffer, sizeof(time_buffer), "-");
        }

        char prompt_preview[SSH_CHATTER_AI_MEMORY_PREVIEW_LEN];
        char reply_preview[SSH_CHATTER_AI_MEMORY_PREVIEW_LEN];
        host_ai_chat_memory_prepare_preview(entry->prompt, prompt_preview,
                                            sizeof(prompt_preview));
        host_ai_chat_memory_prepare_preview(entry->reply, reply_preview,
                                            sizeof(reply_preview));

        char block[SSH_CHATTER_AI_MEMORY_PREVIEW_LEN * 4U];
        int written = snprintf(
            block, sizeof(block),
            "%s- [%s] %s said: %s\n  ai-eliza replied: %s",
            idx == 0U ? "" : "\n", time_buffer,
            entry->username[0] != '\0' ? entry->username : "user",
            prompt_preview[0] != '\0' ? prompt_preview : "(empty)",
            reply_preview[0] != '\0' ? reply_preview : "(empty)");
        if (written < 0) {
            continue;
        }

        size_t block_len = (size_t)written;
        if (block_len >= sizeof(block)) {
            block_len = sizeof(block) - 1U;
            block[block_len] = '\0';
        }

        if (offset + block_len >= context_length) {
            size_t available =
                (offset < context_length) ? context_length - offset - 1U : 0U;
            if (available > 0U) {
                memcpy(context + offset, block, available);
                offset += available;
                context[offset] = '\0';
            }
            break;
        }

        memcpy(context + offset, block, block_len);
        offset += block_len;
        context[offset] = '\0';
    }

    return best_count;
}

static void host_ai_chat_memory_store(host_t *host, const char *username,
                                      const char *prompt, const char *reply)
{
    if (host == nullptr || prompt == nullptr || reply == nullptr) {
        return;
    }

    char clean_prompt[SSH_CHATTER_MESSAGE_LIMIT];
    char clean_reply[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(clean_prompt, sizeof(clean_prompt), "%s", prompt);
    snprintf(clean_reply, sizeof(clean_reply), "%s", reply);
    trim_whitespace_inplace(clean_prompt);
    trim_whitespace_inplace(clean_reply);

    char clean_username[SSH_CHATTER_USERNAME_LEN];
    if (username != nullptr && username[0] != '\0') {
        snprintf(clean_username, sizeof(clean_username), "%s", username);
    } else {
        snprintf(clean_username, sizeof(clean_username), "%s", "user");
    }

    ttak_mutex_lock(&host->lock);
    if (host->ai_chat_memory_count >= SSH_CHATTER_AI_MEMORY_MAX) {
        memmove(host->ai_chat_memory, host->ai_chat_memory + 1,
                (SSH_CHATTER_AI_MEMORY_MAX - 1U) *
                    sizeof(host->ai_chat_memory[0]));
        host->ai_chat_memory_count = SSH_CHATTER_AI_MEMORY_MAX - 1U;
    }

    ai_chat_memory_entry_t *entry =
        &host->ai_chat_memory[host->ai_chat_memory_count++];
    entry->stored_at = time(nullptr);
    snprintf(entry->username, sizeof(entry->username), "%s", clean_username);
    snprintf(entry->prompt, sizeof(entry->prompt), "%s", clean_prompt);
    snprintf(entry->reply, sizeof(entry->reply), "%s", clean_reply);
    ttak_mutex_unlock(&host->lock);
}

static void host_ai_chat_consider_reply(host_t *host,
                                        const chat_history_entry_t *entry)
{
    if (host == nullptr || entry == nullptr) {
        return;
    }
    if (!atomic_load(&host->ai_chat_enabled)) {
        return;
    }
    if (!host_ai_chat_should_respond(entry)) {
        return;
    }

    struct timespec now = session_now_monotonic();
    struct timespec last_reply = {0, 0};
    char model[sizeof(host->ai_chat_model)];
    host_ai_chat_snapshot_state(host, model, sizeof(model), &last_reply);

    double cooldown = session_timespec_elapsed_seconds(&now, &last_reply);
    if (cooldown < 3.0) {
        return;
    }

    char prompt[SSH_CHATTER_MESSAGE_LIMIT * 2U];
    char context[SSH_CHATTER_AI_MEMORY_CONTEXT_BUFFER];
    char username_snippet[SSH_CHATTER_USERNAME_LEN];
    char message_snippet[SSH_CHATTER_AI_PROMPT_MESSAGE_MAX];
    host_ai_chat_copy_limited(username_snippet, sizeof(username_snippet),
                              entry->username, SSH_CHATTER_AI_PROMPT_USERNAME_MAX);
    host_ai_chat_copy_limited(message_snippet, sizeof(message_snippet),
                              entry->message,
                              SSH_CHATTER_AI_PROMPT_MESSAGE_MAX - 1U);
    size_t context_matches = host_ai_chat_memory_collect_context(
        host, entry->message, context, sizeof(context));
    if (context_matches > 0U && context[0] != '\0') {
        char context_snippet[SSH_CHATTER_AI_PROMPT_CONTEXT_MAX];
        host_ai_chat_copy_limited(context_snippet, sizeof(context_snippet),
                                  context,
                                  SSH_CHATTER_AI_PROMPT_CONTEXT_MAX - 1U);
        snprintf(prompt, sizeof(prompt),
                 "Memory context:\n%s\n\nUser %s says: %s\n"
                 "Respond as ai-eliza, a friendly retro terminal chatter "
                 "focused on light conversation. Keep replies under three "
                 "sentences and avoid moderation or BBS topics.",
                 context_snippet, username_snippet, message_snippet);
    } else {
        snprintf(prompt, sizeof(prompt),
                 "User %s says: %s\n"
                 "Respond as ai-eliza, a friendly retro terminal chatter "
                 "focused on light conversation. Keep replies under three "
                 "sentences and avoid moderation or BBS topics.",
                 username_snippet, message_snippet);
    }

    char reply[SSH_CHATTER_MESSAGE_LIMIT];
    const char *default_model = host_ai_chat_default_model();
    bool success =
        translator_ollama_smalltalk(prompt, model, reply, sizeof(reply));
    if (!success) {
        const char *error = translator_last_error();
        bool attempted_custom =
            strcasecmp(model, default_model) != 0;
        if (attempted_custom) {
            printf("[ai-chat] model '%s' failed (%s); falling back to '%s'\n",
                   model,
                   (error != nullptr && error[0] != '\0') ? error
                                                          : "unknown error",
                   default_model);
            ttak_mutex_lock(&host->lock);
            snprintf(host->ai_chat_model, sizeof(host->ai_chat_model), "%s",
                     default_model);
            ttak_mutex_unlock(&host->lock);
            success = translator_ollama_smalltalk(prompt, default_model, reply,
                                                  sizeof(reply));
        } else {
            if (error != nullptr && error[0] != '\0') {
                printf("[ai-chat] small-talk request failed: %s\n", error);
            } else {
                printf("[ai-chat] small-talk request failed.\n");
            }
            return;
        }
    }

    if (!success || reply[0] == '\0') {
        return;
    }

    if (!host_post_client_message(host, "ai-eliza", reply, nullptr, nullptr,
                                  false)) {
        return;
    }

    host_ai_chat_memory_store(host, entry->username, entry->message, reply);
    host_ai_chat_update_last_reply(host, &now);
}

static bool host_ai_chat_enable(host_t *host)
{
    if (host == nullptr) {
        return false;
    }
    bool was_enabled = atomic_exchange(&host->ai_chat_enabled, true);
    if (was_enabled) {
        return false;
    }

    struct timespec reset_ts = {.tv_sec = 0, .tv_nsec = 0};
    host_ai_chat_update_last_reply(host, &reset_ts);

    host_history_record_system(
        host, "* [ai-eliza] is now available for small talk.", nullptr);
    host_post_client_message(
        host, "ai-eliza",
        "Hi! Mention me with \"ai-eliza\" if you want to chat.",
        nullptr, nullptr, false);
    return true;
}

static bool host_ai_chat_disable(host_t *host)
{
    if (host == nullptr) {
        return false;
    }
    bool was_enabled = atomic_exchange(&host->ai_chat_enabled, false);
    if (!was_enabled) {
        return false;
    }

    host_history_record_system(host,
                               "* [ai-eliza] has signed off for now.", nullptr);
    host_post_client_message(host, "ai-eliza",
                             "I'm heading out. Ping me later!", nullptr, nullptr,
                             false);
    return true;
}

bool host_post_client_message(host_t *host, const char *username,
                              const char *message, const char *color_name,
                              const char *highlight_name, bool is_bold)
{
    bool success = false;
    sshc_memory_context_t *memory_scope = nullptr;
    if (host != nullptr) {
        memory_scope = sshc_memory_context_push(host->memory_context);
    }
    chat_history_entry_t entry = {0};
    if (!host_prepare_chat_entry(host, username, message, color_name,
                                 highlight_name, is_bold, &entry)) {
        goto exit_host_post_client_message;
    }

    chat_history_entry_t stored = {0};
    if (!host_history_commit_entry(host, &entry, &stored)) {
        goto exit_host_post_client_message;
    }

    chat_room_broadcast_entry(&host->room, &stored, nullptr);
    host_notify_external_clients(host, &stored);
    success = true;
    if (success) {
        host_ai_chat_consider_reply(host, &stored);
    }

exit_host_post_client_message:
    if (memory_scope != nullptr) {
        sshc_memory_context_pop(memory_scope);
    }
    return success;
}

bool host_post_ephemeral_message(host_t *host, const char *username,
                                 const char *message, const char *color_name,
                                 const char *highlight_name, bool is_bold)
{
    bool success = false;
    sshc_memory_context_t *memory_scope = nullptr;
    if (host != nullptr) {
        memory_scope = sshc_memory_context_push(host->memory_context);
    }
    chat_history_entry_t entry = {0};
    if (!host_prepare_chat_entry(host, username, message, color_name,
                                 highlight_name, is_bold, &entry)) {
        goto exit_host_post_client_message;
    }

    chat_room_broadcast_entry(&host->room, &entry, nullptr);
    host_notify_external_clients(host, &entry);
    success = true;

exit_host_post_client_message:
    if (memory_scope != nullptr) {
        sshc_memory_context_pop(memory_scope);
    }
    return success;
}

bool host_snapshot_last_captcha(host_t *host, char *question,
                                size_t question_length, char *answer,
                                size_t answer_length,
                                struct timespec *timestamp)
{
    if (host == nullptr) {
        return false;
    }

    ttak_mutex_lock(&host->lock);
    bool has_captcha = host->has_last_captcha;
    if (has_captcha) {
        if (question != nullptr && question_length > 0U) {
            snprintf(question, question_length, "%s",
                     host->last_captcha_question);
        }
        if (answer != nullptr && answer_length > 0U) {
            snprintf(answer, answer_length, "%s", host->last_captcha_answer);
        }
        if (timestamp != nullptr) {
            *timestamp = host->last_captcha_generated;
        }
    } else {
        if (question != nullptr && question_length > 0U) {
            question[0] = '\0';
        }
        if (answer != nullptr && answer_length > 0U) {
            answer[0] = '\0';
        }
        if (timestamp != nullptr) {
            timestamp->tv_sec = 0;
            timestamp->tv_nsec = 0L;
        }
    }
    ttak_mutex_unlock(&host->lock);
    return has_captcha;
}

static void host_sleep_after_error(host_t *host)
{
    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);

    unsigned int streak = 1U;
    if (host != nullptr) {
        if (host->health_guard.last_error_time.tv_sec != 0 ||
            host->health_guard.last_error_time.tv_nsec != 0) {
            struct timespec diff =
                timespec_diff(&now, &host->health_guard.last_error_time);
            long long diff_ns = timespec_to_ns(&diff);
            if (diff_ns <= SSH_CHATTER_ERROR_BACKOFF_STABLE_NS &&
                host->health_guard.consecutive_errors < UINT_MAX) {
                streak = host->health_guard.consecutive_errors + 1U;
            }
        }

        host->health_guard.consecutive_errors = streak;
        host->health_guard.last_error_time = now;
    }

    long long multiplier = (long long)streak;
    if (multiplier <= 0) {
        multiplier = 1LL;
    }

    const long long max_multiplier =
        SSH_CHATTER_ERROR_BACKOFF_MAX_NS / SSH_CHATTER_ERROR_BACKOFF_BASE_NS;
    if (multiplier > max_multiplier) {
        multiplier = max_multiplier;
    }

    long long delay_ns = SSH_CHATTER_ERROR_BACKOFF_BASE_NS * multiplier;
    if (delay_ns > SSH_CHATTER_ERROR_BACKOFF_MAX_NS) {
        delay_ns = SSH_CHATTER_ERROR_BACKOFF_MAX_NS;
    }
    if (delay_ns < SSH_CHATTER_ERROR_BACKOFF_BASE_NS) {
        delay_ns = SSH_CHATTER_ERROR_BACKOFF_BASE_NS;
    }

    struct timespec delay = {
        .tv_sec = (time_t)(delay_ns / 1000000000LL),
        .tv_nsec = (long)(delay_ns % 1000000000LL),
    };
    host_sleep_uninterruptible(&delay);
}

static void host_shutdown_internal(host_t *host, bool send_sigterm)
{
    if (host == nullptr) {
        return;
    }

    if (send_sigterm) {
        // Terminate all child processes in the same process group
        kill(0, SIGTERM);
    }

    sshc_memory_context_t *memory_scope = nullptr;
    if (host->memory_context != nullptr) {
        memory_scope = sshc_memory_context_push(host->memory_context);
    }

    host_eliza_worker_shutdown(host);
    host_moderation_shutdown(host);

    host_telnet_listener_stop(host);
    host_json_api_listener_stop(host);

    if (host->rss_thread_initialized) {
        atomic_store(&host->rss_thread_stop, true);
        pthread_join(host->rss_thread, nullptr);
        host->rss_thread_initialized = false;
        atomic_store(&host->rss_thread_running, false);
    }

    while (atomic_load(&host->rss_manual_refresh_running)) {
        struct timespec wait = {
            .tv_sec = 0,
            .tv_nsec = 50 * 1000 * 1000L,
        };
        host_sleep_uninterruptible(&wait);
    }

    if (host->archive_thread_initialized) {
        atomic_store(&host->archive_thread_stop, true);
        pthread_join(host->archive_thread, nullptr);
        host->archive_thread_initialized = false;
        atomic_store(&host->archive_thread_running, false);
    }


    if (host->bbs_watchdog_thread_initialized) {
        atomic_store(&host->bbs_watchdog_thread_stop, true);
        pthread_join(host->bbs_watchdog_thread, nullptr);
        host->bbs_watchdog_thread_initialized = false;
        atomic_store(&host->bbs_watchdog_thread_running, false);
    }

    if (host->morse_client != nullptr) {
        morse_client_destroy(host->morse_client);
        host->morse_client = nullptr;
    }
    if (host->web_client != nullptr) {
        webssh_client_destroy(host->web_client);
        host->web_client = nullptr;
    }
    if (host->clients != nullptr) {
        client_manager_destroy(host->clients);
        host->clients = nullptr;
    }
    chat_history_entry_t *history_buffer = nullptr;
    join_activity_entry_t *join_buffer = nullptr;
    ttak_mutex_lock(&host->lock);
    history_buffer = host->history;
    host->history = nullptr;
    host->history_capacity = 0U;
    host->history_count = 0U;
    join_buffer = host->join_activity;
    host->join_activity = nullptr;
    host->join_activity_capacity = 0U;
    host->join_activity_count = 0U;
    ttak_mutex_unlock(&host->lock);
    if (history_buffer != nullptr) {
        sshc_gc_free(history_buffer);
    }
    if (join_buffer != nullptr) {
        sshc_gc_free(join_buffer);
    }
    sshc_gc_free(host->connection_guard);
    host->connection_guard = nullptr;
    host->connection_guard_capacity = 0U;
    host->connection_guard_count = 0U;
    host->listener.accept_error_streak = 0U;
    host->health_guard.consecutive_errors = 0U;
    host->health_guard.last_error_time.tv_sec = 0;
    host->health_guard.last_error_time.tv_nsec = 0L;
    session_ctx_t **room_members = nullptr;
    ttak_mutex_lock(&host->room.lock);
    room_members = host->room.members;
    host->room.members = nullptr;
    host->room.member_capacity = 0U;
    host->room.member_count = 0U;
    ttak_mutex_unlock(&host->room.lock);
    if (room_members != nullptr) {
        sshc_gc_free(room_members);
    }
    if (host->user_data_lock_initialized) {
        ttak_mutex_destroy(&host->user_data_lock);
        host->user_data_lock_initialized = false;
    }
    if (host->alpha_landers_lock_initialized) {
        ttak_mutex_destroy(&host->alpha_landers_lock);
        host->alpha_landers_lock_initialized = false;
    }
    if (host->security_layer_initialized) {
        security_layer_free(&host->security_layer);
        host->security_layer_initialized = false;
    }

    if (host->rss_refresh_lock_initialized) {
        ttak_mutex_destroy(&host->rss_refresh_lock);
        host->rss_refresh_lock_initialized = false;
    }

    ttak_mutex_destroy(&host->room.lock);
    ttak_mutex_destroy(&host->lock);

    if (memory_scope != nullptr) {
        sshc_memory_context_pop(memory_scope);
    }
}

void host_shutdown(host_t *host)
{
    host_shutdown_internal(host, true);
}

void host_shutdown_for_testing(host_t *host)
{
    host_shutdown_internal(host, false);
}

int host_serve(host_t *host, const char *bind_addr, const char *port,
               const char *key_directory, const char *telnet_bind_addr,
               const char *telnet_port, const char *json_bind_addr,
               const char *json_port)
{
    if (host == nullptr) {
        return -1;
    }

    const char *address =
        (bind_addr != nullptr && bind_addr[0] != '\0') ? bind_addr : "0.0.0.0";
    const char *bind_port =
        (port != nullptr && port[0] != '\0') ? port : "2222";
    const char *telnet_bind = nullptr;
    if (telnet_bind_addr != nullptr) {
        telnet_bind = telnet_bind_addr;
    } else if (bind_addr != nullptr && bind_addr[0] != '\0') {
        telnet_bind = bind_addr;
    } else {
        telnet_bind = address;
    }
    if (telnet_port != nullptr && telnet_port[0] != '\0') {
        if (!host_telnet_listener_start(host, telnet_bind, telnet_port)) {
            const char *display_addr =
                (telnet_bind != nullptr && telnet_bind[0] != '\0') ? telnet_bind
                                                                   : "*";
            printf("[telnet] telnet listener unavailable on %s:%s\n",
                   display_addr, telnet_port);
        }
    } else {
        host_telnet_listener_stop(host);
    }
    const char *json_bind = nullptr;
    if (json_bind_addr != nullptr) {
        json_bind = json_bind_addr;
    } else if (bind_addr != nullptr && bind_addr[0] != '\0') {
        json_bind = bind_addr;
    } else {
        json_bind = address;
    }
    if (json_port != nullptr && json_port[0] != '\0') {
        if (!host_json_api_listener_start(host, json_bind, json_port)) {
            const char *display_addr =
                (json_bind != nullptr && json_bind[0] != '\0') ? json_bind : "*";
            printf("[json-api] listener unavailable on %s:%s\n", display_addr,
                   json_port);
        }
    } else {
        host_json_api_listener_stop(host);
    }
    host_register_protected_bind_address(host, address);
    host_register_protected_bind_address(host, telnet_bind);
    host_register_protected_bind_address(host, json_bind);
    const bool key_dir_specified =
        key_directory != nullptr && key_directory[0] != '\0';
    const host_key_definition_t host_key_definitions[] = {
        {"ssh-ed25519", "ssh_host_ed25519_key", SSH_BIND_OPTIONS_IMPORT_KEY,
         true},
        {"ecdsa-sha2-nistp256", "ssh_host_ecdsa_key", SSH_BIND_OPTIONS_ECDSAKEY,
         false},
        {"ssh-rsa", "ssh_host_rsa_key", SSH_BIND_OPTIONS_RSAKEY, false},
    };
    const size_t host_key_count =
        sizeof(host_key_definitions) / sizeof(host_key_definitions[0]);

    while (host->shutdown_flag == nullptr || *host->shutdown_flag == 0) {
        ssh_bind bind_handle = ssh_bind_new();
        if (bind_handle == nullptr) {
            humanized_log_error("host", "failed to allocate ssh_bind", ENOMEM);
            host_sleep_after_error(host);
            continue;
        }

        ssh_key imported_keys[host_key_count];
        memset(imported_keys, 0, sizeof(imported_keys));

        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_BINDADDR, address);
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_BINDPORT_STR,
                             bind_port);

        bool fatal_key_error = false;
        bool key_loaded = false;
        char preferred_algorithm[64];
        preferred_algorithm[0] = '\0';
        char algorithm_buffer[256];
        algorithm_buffer[0] = '\0';
        size_t algorithm_length = 0U;

        for (size_t idx = 0; idx < host_key_count; ++idx) {
            const host_key_definition_t *definition =
                &host_key_definitions[idx];
            char key_path[PATH_MAX];
            bool path_valid = false;
            char custom_candidate[PATH_MAX];
            bool attempted_custom = false;

            if (key_dir_specified) {
                attempted_custom = true;
                if (!host_join_key_path(key_directory, definition->filename,
                                        custom_candidate,
                                        sizeof(custom_candidate))) {
                    humanized_log_error("host",
                                        "host key directory path is too long",
                                        ENAMETOOLONG);
                    fatal_key_error = true;
                    break;
                }
                if (access(custom_candidate, R_OK) == 0) {
                    snprintf(key_path, sizeof(key_path), "%s",
                             custom_candidate);
                    path_valid = true;
                }
            }

            if (!path_valid) {
                if (access(definition->filename, R_OK) == 0) {
                    snprintf(key_path, sizeof(key_path), "%s",
                             definition->filename);
                    path_valid = true;
                } else {
                    char fallback_path[PATH_MAX];
                    const int written =
                        snprintf(fallback_path, sizeof(fallback_path),
                                 "/etc/ssh/%s", definition->filename);
                    if (written >= 0 &&
                        (size_t)written < sizeof(fallback_path) &&
                        access(fallback_path, R_OK) == 0) {
                        snprintf(key_path, sizeof(key_path), "%s",
                                 fallback_path);
                        path_valid = true;
                    }
                }
            }

            if (!path_valid) {
                if (attempted_custom) {
                    printf(
                        "[listener] %s host key not found at %s (skipping)\n",
                        definition->algorithm, custom_candidate);
                }
                continue;
            }

            if (!host_bind_load_key(bind_handle, definition, key_path,
                                    &imported_keys[idx])) {
                continue;
            }

            if (!key_loaded) {
                snprintf(preferred_algorithm, sizeof(preferred_algorithm), "%s",
                         definition->algorithm);
                key_loaded = true;
            }

            host_bind_append_algorithm(
                algorithm_buffer, sizeof(algorithm_buffer), &algorithm_length,
                definition->algorithm);
        }

        if (fatal_key_error) {
            ssh_bind_free(bind_handle);
            host_sleep_after_error(host);
            continue;
        }

        if (algorithm_length == 0U) {
            humanized_log_error("host", "no host keys configured", 0);
            ssh_bind_free(bind_handle);
            host_sleep_after_error(host);
            continue;
        }

        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_HOSTKEY_ALGORITHMS,
                             algorithm_buffer);
        if (preferred_algorithm[0] != '\0') {
            ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_HOSTKEY,
                                 preferred_algorithm);
        }

        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_KEY_EXCHANGE,
                             SSH_CHATTER_SUPPORTED_KEX_ALGORITHMS);
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_CIPHERS_C_S,
                             SSH_CHATTER_STRONG_CIPHERS);
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_CIPHERS_S_C,
                             SSH_CHATTER_STRONG_CIPHERS);
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_HMAC_C_S,
                             SSH_CHATTER_STRONG_MACS);
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_HMAC_S_C,
                             SSH_CHATTER_STRONG_MACS);
#ifdef SSH_BIND_OPTIONS_COMPRESSION_C_S
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_COMPRESSION_C_S,
                             SSH_CHATTER_SECURE_COMPRESSION);
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_COMPRESSION_S_C,
                             SSH_CHATTER_SECURE_COMPRESSION);
#endif

        if (ssh_bind_listen(bind_handle) < 0) {
            humanized_log_error("host", ssh_get_error(bind_handle), EIO);
            ssh_bind_free(bind_handle);
            host_sleep_after_error(host);
            continue;
        }

        host->listener.handle = bind_handle;
        host->listener.accept_error_streak = 0U;
        host->listener.last_error_time.tv_sec = 0;
        host->listener.last_error_time.tv_nsec = 0L;
        printf("[listener] listening on %s:%s\n", address, bind_port);
        host_error_guard_register_success(host);

        // Retrieve the bind socket fd for poll()-based accept gating
        socket_t bind_fd = ssh_bind_get_fd(bind_handle);
        unsigned int idle_poll_cycles = 0U;
        struct timespec last_gc_run = {0};
        clock_gettime(CLOCK_MONOTONIC, &last_gc_run);
        struct timespec last_idle_check = last_gc_run;

        bool restart_listener = false;
        while (!restart_listener &&
               (host->shutdown_flag == nullptr || *host->shutdown_flag == 0)) {

            // Gate accept() with poll() so we never block indefinitely.
            // This lets us check shutdown flags and run health probes.
            if (bind_fd >= 0) {
                struct pollfd pfd;
                pfd.fd = bind_fd;
                pfd.events = POLLIN;
                pfd.revents = 0;

                int poll_rc =
                    poll(&pfd, 1, SSH_CHATTER_ACCEPT_POLL_TIMEOUT_MS);
                if (poll_rc < 0) {
                    if (errno == EINTR) {
                        host_gc_cycle(host, &last_gc_run);
                        continue;
                    }
                    // poll() failed on the bind socket -- treat as fatal
                    printf("[listener] poll() on bind socket failed: %s\n",
                           strerror(errno));
                    restart_listener = true;
                    break;
                }
                if (poll_rc == 0) {
                    // Timeout: no incoming connection yet
                    host_gc_cycle(host, &last_gc_run);
                    ++idle_poll_cycles;
                    if (idle_poll_cycles >=
                        SSH_CHATTER_ACCEPT_HEALTH_CHECK_POLLS) {
                        // Verify bind socket is still healthy
                        idle_poll_cycles = 0U;
                        int sock_err = 0;
                        socklen_t err_len = sizeof(sock_err);
                        if (getsockopt(bind_fd, SOL_SOCKET, SO_ERROR,
                                       &sock_err, &err_len) < 0 ||
                            sock_err != 0) {
                            printf("[listener] bind socket health check "
                                   "failed (error=%d), restarting\n",
                                   sock_err);
                            restart_listener = true;
                            break;
                        }
                    }
                    continue;
                }
                if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                    printf("[listener] bind socket reported error "
                           "(revents=0x%x), restarting\n",
                           (unsigned)pfd.revents);
                    restart_listener = true;
                    break;
                }
                idle_poll_cycles = 0U;
            }

            host_gc_cycle(host, &last_gc_run);
            host_idle_state_maintenance(host, &last_idle_check);

            ssh_session session = ssh_new();
            if (session == nullptr) {
                humanized_log_error("host", "failed to allocate session",
                                    ENOMEM);
                continue;
            }

            if (ssh_bind_accept(bind_handle, session) == SSH_ERROR) {
                int accept_error = errno;
                const char *bind_error = ssh_get_error(bind_handle);
                const bool bind_error_present =
                    bind_error != nullptr && bind_error[0] != '\0';

                if (accept_error == 0) {
                    if (host->listener.accept_error_streak < UINT_MAX) {
                        host->listener.accept_error_streak += 1U;
                    }
                } else {
                    host->listener.accept_error_streak = 0U;
                }

                printf("[listener] accept failed, error=%d, shutdown_flag=%p "
                       "value=%d\n",
                       accept_error, (void *)host->shutdown_flag,
                       (host->shutdown_flag != nullptr ? *host->shutdown_flag
                                                       : -1));
                fflush(stdout);

                if (accept_error != 0) {
                    char log_message[512];
                    const char *system_message = strerror(accept_error);

                    if (system_message != nullptr &&
                        system_message[0] != '\0') {
                        if (bind_error_present &&
                            !string_contains_case_insensitive(bind_error,
                                                              system_message)) {
                            snprintf(log_message, sizeof(log_message),
                                     "Socket error: %s (%s)", system_message,
                                     bind_error);
                        } else {
                            snprintf(log_message, sizeof(log_message),
                                     "Socket error: %s", system_message);
                        }
                    } else if (bind_error_present) {
                        snprintf(log_message, sizeof(log_message),
                                 "Socket error (code %d): %s", accept_error,
                                 bind_error);
                    } else {
                        snprintf(log_message, sizeof(log_message),
                                 "Socket error (code %d)", accept_error);
                    }

                    humanized_log_error("host", log_message, accept_error);
                } else if (bind_error != nullptr && bind_error[0] != '\0') {
                    humanized_log_error("host", bind_error, EIO);
                } else {
                    humanized_log_error("host", "Socket accept failed", EIO);
                }

                bool fatal_socket_error = false;
                bool should_backoff_after_socket_error = false;
                bool force_restart_after_empty_errno = false;

                if (accept_error != 0) {
                    should_backoff_after_socket_error = true;
                    switch (accept_error) {
                    case EAGAIN:
#ifdef EWOULDBLOCK
#if EWOULDBLOCK != EAGAIN
                    case EWOULDBLOCK:
#endif
#endif
                    case EINTR:
                    case ECONNRESET:
                    case ECONNABORTED:
                    case ETIMEDOUT:
                    case ENOTCONN:
                    case EPIPE:
#ifdef EPROTO
                    case EPROTO:
#endif
                        should_backoff_after_socket_error = false;
                        break;
                    case EBADF:
                    case ENOTSOCK:
                    case EINVAL:
                        fatal_socket_error = true;
                        break;
                    case EMFILE:
                    case ENFILE:
                    case ENOBUFS:
#ifdef ENOMEM
                    case ENOMEM:
#endif
#ifdef ENOSR
                    case ENOSR:
#endif
                        // Resource exhaustion: treat as transient with backoff
                        // instead of fatal restart.  Restarting the listener
                        // while FDs are exhausted will just fail again.
                        should_backoff_after_socket_error = true;
                        break;
                    default:
                        break;
                    }
                } else {
                    should_backoff_after_socket_error = true;
                    if (host->listener.accept_error_streak >= 3U) {
                        fatal_socket_error = true;
                        force_restart_after_empty_errno = true;
                    }
                }

                ssh_free(session);
                if (fatal_socket_error && !force_restart_after_empty_errno) {
                    if (bind_error_present &&
                        string_contains_case_insensitive(bind_error, "kex")) {
                        fatal_socket_error = false;
                    } else if (bind_error_present) {
                        fatal_socket_error = false;
                    }
                }

                if (fatal_socket_error) {
                    if (force_restart_after_empty_errno) {
                        printf("[listener] forcing restart after %u consecutive "
                               "accept failures without errno\n",
                               host->listener.accept_error_streak);
                    }
                    host->listener.accept_error_streak = 0U;
                    clock_gettime(CLOCK_MONOTONIC,
                                  &host->listener.last_error_time);
                    if (host_listener_attempt_recover(host, bind_handle,
                                                      address, bind_port)) {
                        continue;
                    }
                    host->listener.restart_attempts += 1U;
                    printf("[listener] scheduling full listener restart after "
                           "socket "
                           "error (attempt %u)\n",
                           host->listener.restart_attempts);
                    restart_listener = true;
                    break;
                }

                if (should_backoff_after_socket_error) {
                    // Use longer backoff for resource exhaustion errors
                    long backoff_ns = 200000000L; // 200ms default
                    if (accept_error == EMFILE || accept_error == ENFILE ||
                        accept_error == ENOBUFS) {
                        backoff_ns = 1000000000L; // 1s for FD/buffer exhaustion
                    }
                    struct timespec retry_delay = {
                        .tv_sec = backoff_ns / 1000000000L,
                        .tv_nsec = backoff_ns % 1000000000L,
                    };
                    host_sleep_uninterruptible(&retry_delay);
                }

                // Check shutdown flag after non-fatal errors (e.g., EINTR from signal)
                if (host->shutdown_flag != nullptr &&
                    *host->shutdown_flag != 0) {
                    printf(
                        "[listener] detected shutdown flag after socket error, "
                        "breaking from accept loop\n");
                    restart_listener = true;
                    break;
                }

                continue;
            }

            session_configure_tcp_keepalive(session);
            session_configure_ssh_options(session);

            hostkey_probe_result_t hostkey_probe =
                session_probe_client_hostkey_algorithms(
                    session, SSH_CHATTER_REQUIRED_HOSTKEY_ALGORITHMS,
                    SSH_CHATTER_REQUIRED_HOSTKEY_ALGORITHMS_COUNT);
            if (hostkey_probe.status == HOSTKEY_SUPPORT_REJECTED) {
                char peer_address[NI_MAXHOST];
                session_describe_peer(session, peer_address,
                                      sizeof(peer_address));
                if (peer_address[0] == '\0') {
                    strncpy(peer_address, "unknown", sizeof(peer_address) - 1U);
                    peer_address[sizeof(peer_address) - 1U] = '\0';
                }

                if (hostkey_probe.offered_algorithms[0] != '\0') {
                    printf("[reject] client %s does not accept one of [%s] "
                           "host keys "
                           "(client offered: %s)\n",
                           peer_address,
                           SSH_CHATTER_REQUIRED_HOSTKEY_ALGORITHMS_DISPLAY,
                           hostkey_probe.offered_algorithms);
                } else {
                    printf("[reject] client %s does not accept one of [%s] "
                           "host keys\n",
                           peer_address,
                           SSH_CHATTER_REQUIRED_HOSTKEY_ALGORITHMS_DISPLAY);
                }

                ssh_disconnect(session);
                ssh_free(session);
                continue;
            }

            if (ssh_handle_key_exchange(session) != SSH_OK) {
                humanized_log_error("host", ssh_get_error(session), EPROTO);
                ssh_disconnect(session);
                ssh_free(session);
                continue;
            }

            char peer_address[NI_MAXHOST];
            session_describe_peer(session, peer_address, sizeof(peer_address));
            if (peer_address[0] == '\0') {
                strncpy(peer_address, "unknown", sizeof(peer_address) - 1U);
                peer_address[sizeof(peer_address) - 1U] = '\0';
            }

            connection_guard_result_t guard =
                host_connection_guard_register(host, peer_address);
            if (guard.blocked) {
                struct timespec now_block = {0, 0};
                clock_gettime(CLOCK_MONOTONIC, &now_block);
                double wait_seconds = 0.0;
                if (timespec_compare(&guard.blocked_until, &now_block) > 0) {
                    struct timespec remaining =
                        timespec_diff(&guard.blocked_until, &now_block);
                    wait_seconds = (double)remaining.tv_sec +
                                   (double)remaining.tv_nsec / 1000000000.0;
                }
                printf(
                    "[throttle] rate-limited connection from %s (attempts=%zu, "
                    "penalty=%.2f seconds)\n",
                    peer_address, guard.attempt_count, wait_seconds);
                ssh_disconnect(session);
                ssh_free(session);
                continue;
            }

            printf("[connect] accepted client from %s\n", peer_address);
            host->listener.accept_error_streak = 0U;

            const char *client_banner = ssh_get_clientbanner(session);
            const version_ip_ban_rule_t *matched_rule = nullptr;
            if (host_version_ip_should_ban(host, client_banner, peer_address,
                                           &matched_rule)) {
                const char *version_display =
                    (client_banner != nullptr && client_banner[0] != '\0')
                        ? client_banner
                        : "unknown";
                const char *pattern_display =
                    (matched_rule != nullptr &&
                     matched_rule->original_pattern[0] != '\0')
                        ? matched_rule->original_pattern
                        : "policy";
                const char *cidr_display = (matched_rule != nullptr &&
                                            matched_rule->cidr_text[0] != '\0')
                                               ? matched_rule->cidr_text
                                               : "unknown range";
                const char *note_display =
                    (matched_rule != nullptr && matched_rule->note[0] != '\0')
                        ? matched_rule->note
                        : "version/IP policy";
                printf(
                    "[reject] %s disconnected for client version '%s' (%s in "
                    "%s; %s)\n",
                    peer_address, version_display, pattern_display,
                    cidr_display, note_display);
                ssh_disconnect(session);
                ssh_free(session);
                continue;
            }

            session_ctx_t *ctx = session_create();
            if (ctx == nullptr) {
                humanized_log_error(
                    "host", "failed to allocate session context", ENOMEM);
                ssh_disconnect(session);
                ssh_free(session);
                continue;
            }
            ctx->ops = &ssh_session_ops;

            ctx->session = session;
            ctx->channel = nullptr;
            ctx->transport_kind = SESSION_TRANSPORT_SSH;
            ctx->telnet_fd = -1;
            ctx->telnet_eof = false;
            ctx->telnet_pending_valid = false;
            pthread_mutexattr_t lock_attr;
            pthread_mutexattr_init(&lock_attr);
            pthread_mutexattr_settype(&lock_attr, PTHREAD_MUTEX_RECURSIVE);
            int mutex_error = pthread_mutex_init(&ctx->output_lock, &lock_attr);
            pthread_mutexattr_destroy(&lock_attr);
            if (mutex_error != 0) {
                humanized_log_error("host",
                                    "failed to initialise session output lock",
                                    mutex_error);
                session_destroy(ctx);
                continue;
            }
            ctx->output_lock_initialized = true;
            ctx->owner = host;
            if (ttak_mutex_init(&ctx->channel_mutex) == 0) {
                ctx->channel_mutex_initialized = true;
            } else {
                humanized_log_error("session",
                                    "failed to initialize channel mutex",
                                    errno != 0 ? errno : ENOMEM);
            }
            ctx->auth = (auth_profile_t){0};
            snprintf(ctx->client_ip, sizeof(ctx->client_ip), "%.*s",
                     (int)sizeof(ctx->client_ip) - 1, peer_address);
            ctx->input_mode = SESSION_INPUT_MODE_CHAT;

            bool geo_language_enabled =
                atomic_load(&ctx->owner->geo_language_enabled);
            session_ui_language_t provider_language = SESSION_UI_LANGUAGE_COUNT;
            char provider_label[SSH_CHATTER_PROVIDER_LABEL_LEN];
            bool provider_detected =
                geo_language_enabled &&
                session_detect_provider_ip(ctx->client_ip, provider_label,
                                           sizeof(provider_label));
            if (provider_detected &&
                host_provider_language_preference(host, provider_label,
                                                  &provider_language)) {
                ctx->ui_language = provider_language;
                ctx->active_codepage =
                    session_codepage_for_language(provider_language);
            } else if (geo_language_enabled) {
                session_ui_language_t geo_language =
                    session_client_geo_language(ctx);
                if (geo_language != SESSION_UI_LANGUAGE_COUNT) {
                    ctx->ui_language = geo_language;
                    ctx->active_codepage =
                        session_codepage_for_language(geo_language);
                } else {
                    ctx->ui_language = SESSION_UI_LANGUAGE_EN;
                    ctx->active_codepage =
                        session_codepage_for_language(SESSION_UI_LANGUAGE_EN);
                }
            } else {
                ctx->ui_language = SESSION_UI_LANGUAGE_EN;
                ctx->active_codepage =
                    session_codepage_for_language(SESSION_UI_LANGUAGE_EN);
            }
            if (client_banner != nullptr && client_banner[0] != '\0') {
                snprintf(ctx->client_banner, sizeof(ctx->client_banner), "%s",
                         client_banner);
            }
            session_refresh_output_encoding(ctx);

            ttak_mutex_lock(&host->lock);
            ++host->connection_count;
            ctx->user.is_operator = false;
            ctx->user.is_lan_operator = false;
            ttak_mutex_unlock(&host->lock);

            pthread_t thread_id;
            if (pthread_create(&thread_id, nullptr, session_thread, ctx) != 0) {
                humanized_log_error("host", "failed to spawn session thread",
                                    errno);
                session_destroy(ctx);
                continue;
            }

            pthread_detach(thread_id);
            host_error_guard_register_success(host);

        }

        ssh_bind_free(bind_handle);
        host->listener.handle = nullptr;

        // Check for shutdown signal before deciding to restart
        if (host->shutdown_flag != nullptr && *host->shutdown_flag != 0) {
            printf(
                "[listener] shutdown signal received, exiting listener loop\n");
            break;
        }

        if (!restart_listener) {
            host_sleep_after_error(host);
            continue;
        }

        struct timespec backoff = {
            .tv_sec = 1,
            .tv_nsec = 0,
        };
        host_sleep_uninterruptible(&backoff);

        if (host->listener.restart_attempts > 0U) {
            printf("[listener] attempting full listener restart after socket "
                   "error "
                   "(attempt %u)\n",
                   host->listener.restart_attempts);
        } else {
            printf("[listener] attempting full listener restart after socket "
                   "error\n");
        }

        if (host->listener.last_error_time.tv_sec != 0 ||
            host->listener.last_error_time.tv_nsec != 0L) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            struct timespec elapsed =
                timespec_diff(&now, &host->listener.last_error_time);
            double elapsed_seconds =
                (double)elapsed.tv_sec + (double)elapsed.tv_nsec / 1000000000.0;
            printf("[listener] last fatal error occurred %.3f seconds ago\n",
                   elapsed_seconds);
        }
    }

    return 0;
}

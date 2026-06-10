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

static void session_log_encoding_state(const char *phase,
                                       const session_ctx_t *ctx)
{
    if (phase == nullptr || ctx == nullptr) {
        return;
    }

    printf("[encoding-debug] phase=%s transport_kind=%d prefer_utf16_output=%d "
           "output_kind=%d\n",
           phase, (int)ctx->transport_kind, (int)ctx->prefer_utf16_output,
           (int)ctx->output_kind);
}

uint64_t host_allocate_session_id(host_t *host)
{
    if (host == nullptr) {
        return 0U;
    }

    return atomic_fetch_add(&host->next_session_id, 1U);
}

static void host_telnet_configure_client_socket(int client_fd)
{
    if (client_fd < 0) {
        return;
    }

    int enable = 1;
    (void)setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &enable,
                     sizeof(enable));
    (void)setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &enable,
                     sizeof(enable));

#if defined(TCP_KEEPIDLE)
    int keep_idle_seconds = 30;
    (void)setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPIDLE,
                     &keep_idle_seconds, sizeof(keep_idle_seconds));
#endif

#if defined(TCP_KEEPINTVL)
    int keep_interval_seconds = 10;
    (void)setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPINTVL,
                     &keep_interval_seconds, sizeof(keep_interval_seconds));
#endif

#if defined(TCP_KEEPCNT)
    int keep_probe_count = 3;
    (void)setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPCNT, &keep_probe_count,
                     sizeof(keep_probe_count));
#endif

#if defined(TCP_USER_TIMEOUT)
    int user_timeout_ms = 60000;
    (void)setsockopt(client_fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout_ms,
                     sizeof(user_timeout_ms));
#endif
}

static bool session_runtime_bind(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return false;
    }
    if (ctx->session_data != nullptr && ctx->session_id != 0U) {
        return true;
    }

    session_runtime_data_t *runtime =
        (session_runtime_data_t *)sshc_gc_calloc(1U, sizeof(*runtime));
    if (runtime == nullptr) {
        return false;
    }

    const uint64_t session_id = host_allocate_session_id(ctx->owner);
    if (session_id == 0U) {
        sshc_gc_free(runtime);
        return false;
    }

    runtime->session_id = session_id;
    atomic_init(&runtime->active, true);
    runtime->ctx = ctx;
    ctx->session_id = session_id;
    ctx->session_data = (void *)runtime;
    printf("[session] created session_id=%llu\n",
           (unsigned long long)session_id);
    return true;
}

static void session_runtime_unbind(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->session_data == nullptr) {
        return;
    }

    session_runtime_data_t *runtime =
        (session_runtime_data_t *)ctx->session_data;
    printf("[session] releasing session_id=%llu\n",
           (unsigned long long)runtime->session_id);
    atomic_store(&runtime->active, false);
    runtime->ctx = nullptr;
    ctx->session_data = nullptr;
    ctx->session_id = 0U;
    sshc_gc_free(runtime);
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

        host_telnet_configure_client_socket(client_fd);

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
        session_log_encoding_state("transport_setup", ctx);

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
        if (host->connection_count == 1) {
            sshc_memory_context_set_gc_aggressive(host->memory_context);
        }
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

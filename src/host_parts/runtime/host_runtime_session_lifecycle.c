
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

#define SESSION_EBR_DRAIN_PASSES 3U
#define SESSION_GC_ROTATE_PASSES 2U
#define SESSION_MANUAL_GC_WAIT_PASSES 2U
#define SESSION_MANUAL_GC_WAIT_NS 5000000L

static void session_drain_reclamation(sshc_memory_context_t *memory_context,
                                      host_t *owner)
{
    for (unsigned int pass = 0U; pass < SESSION_GC_ROTATE_PASSES; ++pass) {
        if (memory_context != nullptr) {
            sshc_memory_context_epoch_gc_rotate(memory_context);
        }
        sshc_epoch_reclaim();
    }

    if (owner != nullptr) {
        for (unsigned int pass = 0U; pass < SESSION_MANUAL_GC_WAIT_PASSES;
             ++pass) {
            host_manual_gc_tick(owner);
            struct timespec pause = {
                .tv_sec = 0,
                .tv_nsec = SESSION_MANUAL_GC_WAIT_NS,
            };
            host_sleep_uninterruptible(&pause);
        }
    }

    for (unsigned int pass = 0U; pass < SESSION_EBR_DRAIN_PASSES; ++pass) {
        sshc_epoch_reclaim();
    }
}

static void session_release_interaction_state(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    /*
     * Reset only the active indices and lengths instead of zeroing entire
     * embedded buffers.  This avoids cache-thrashing memsets on multi-kilobyte
     * arrays while still making the session state appear empty.  The buffers
     * themselves will be zeroed when the session context is recreated via
     * sshc_gc_calloc.
     */
    ctx->input_length = 0U;
    ctx->input_history_count = 0U;
    ctx->input_history_position = -1;
    ctx->input_escape_length = 0U;
    ctx->multibyte_input_length = 0U;
    ctx->output_buffer_length = 0U;
    ctx->has_last_output_line = false;
    ctx->realtime_recent_count = 0U;
    ctx->realtime_recent_start = 0U;
    ctx->realtime_line_count = 0U;
}

static void session_release_transport_state(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_SSH &&
        ctx->channel != nullptr) {
        ssh_channel_request_send_exit_status(ctx->channel, ctx->exit_status);
    }
    session_close_channel(ctx);

    if (ctx->session != nullptr) {
        ssh_disconnect(ctx->session);
        ssh_free(ctx->session);
        ctx->session = nullptr;
    }

    session_safe_free((void **)&ctx->session_data);
}

static void session_detach_external_state(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    sshc_memory_defer_gc_registration_end();

    if (ctx->user_data_loaded) {
        (void)session_user_data_commit(ctx);
    }

    session_translation_worker_shutdown(ctx);

    /* Ensure multiplayer slot references are detached before the session
     * object is reclaimed so stale pointers do not remain in host state. */
    if (ctx->owner != nullptr) {
        host_t *host = ctx->owner;

        ttak_mutex_lock(&host->lock);
        if (host->connection_count > 0U) {
            --host->connection_count;
        }
        ttak_mutex_unlock(&host->lock);

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
                    if (opponent->game.othello != nullptr) {
                        opponent->game.othello->game_over = true;
                    }
                }
                session_game_suspend(opponent,
                                     "Opponent disconnected. Game ended.");
            }
        }
    }

    session_release_transport_state(ctx);
}

static void session_cleanup(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    /* Release per-session RSS snapshot cache after the session has already
     * stopped claiming runtime resources. */
    session_rss_clear(ctx);

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
    session_game_release_liar(ctx);
    session_game_release_saved_liar(ctx);
    session_game_release_alpha(ctx);
    session_game_release_saved_alpha(ctx);
    session_game_release_othello(ctx);
    session_game_release_saved_othello(ctx);
    session_game_release_gonu(ctx);
    session_game_release_saved_gonu(ctx);
    session_safe_free((void **)&ctx->scrollback_buffer);
    ctx->scrollback_buffer_capacity = 0U;
    session_release_interaction_state(ctx);
}

static void session_epoch_free(void *ptr)
{
    session_ctx_t *ctx = (session_ctx_t *)ptr;
    if (ctx == nullptr) {
        return;
    }

    session_cleanup(ctx);

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

    session_runtime_unbind(ctx);
    session_detach_external_state(ctx);
    session_drain_reclamation(ctx->memory_context, ctx->owner);
#if defined(__GLIBC__)
    (void)malloc_trim(0);
#endif
    /*
     * Destroy the session object immediately instead of deferring through
     * EBR.  By the time we reach here the session has already been removed
     * from the room and room_snapshot_refs has dropped to zero, so no
     * other thread can observe this pointer.  This makes session teardown
     * fully deterministic.
     */
    session_epoch_free(ctx);
    session_drain_reclamation(nullptr, ctx->owner);
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
    session_log_encoding_state("transport_setup", ctx);

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
    if (!session_runtime_bind(ctx)) {
        session_destroy(ctx);
        return nullptr;
    }

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
    if (!session_runtime_bind(ctx)) {
        SESSION_THREAD_ERROR_EXIT();
    }
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
            session_configure_tcp_keepalive(ctx->session);
            session_configure_ssh_options(ctx->session);

            hostkey_probe_result_t hostkey_probe =
                session_probe_client_hostkey_algorithms(
                    ctx->session, SSH_CHATTER_REQUIRED_HOSTKEY_ALGORITHMS,
                    SSH_CHATTER_REQUIRED_HOSTKEY_ALGORITHMS_COUNT);
            if (hostkey_probe.status == HOSTKEY_SUPPORT_REJECTED) {
                if (hostkey_probe.offered_algorithms[0] != '\0') {
                    printf("[reject] client %s does not accept one of [%s] "
                           "host keys (client offered: %s)\n",
                           ctx->client_ip,
                           SSH_CHATTER_REQUIRED_HOSTKEY_ALGORITHMS_DISPLAY,
                           hostkey_probe.offered_algorithms);
                } else {
                    printf("[reject] client %s does not accept one of [%s] "
                           "host keys\n",
                           ctx->client_ip,
                           SSH_CHATTER_REQUIRED_HOSTKEY_ALGORITHMS_DISPLAY);
                }
                SESSION_THREAD_ERROR_EXIT();
            }

            if (ssh_handle_key_exchange(ctx->session) != SSH_OK) {
                humanized_log_error("session", ssh_get_error(ctx->session),
                                    EPROTO);
                SESSION_THREAD_ERROR_EXIT();
            }

            const char *client_banner = ssh_get_clientbanner(ctx->session);
            const version_ip_ban_rule_t *matched_rule = nullptr;
            if (host_version_ip_should_ban(ctx->owner, client_banner,
                                           ctx->client_ip, &matched_rule)) {
                const char *version_display =
                    (client_banner != nullptr && client_banner[0] != '\0')
                        ? client_banner
                        : "unknown";
                const char *pattern_display =
                    (matched_rule != nullptr &&
                     matched_rule->original_pattern[0] != '\0')
                        ? matched_rule->original_pattern
                        : "policy";
                const char *cidr_display =
                    (matched_rule != nullptr &&
                     matched_rule->cidr_text[0] != '\0')
                        ? matched_rule->cidr_text
                        : "unknown range";
                const char *note_display =
                    (matched_rule != nullptr && matched_rule->note[0] != '\0')
                        ? matched_rule->note
                        : "version/IP policy";
                printf("[reject] %s disconnected for client version '%s' (%s in "
                       "%s; %s)\n",
                       ctx->client_ip, version_display, pattern_display,
                       cidr_display, note_display);
                SESSION_THREAD_ERROR_EXIT();
            }

            if (client_banner != nullptr && client_banner[0] != '\0') {
                snprintf(ctx->client_banner, sizeof(ctx->client_banner), "%s",
                         client_banner);
            }
            session_refresh_output_encoding(ctx);

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

        const bool preferred_has_ansi =
            strchr(preferred_nickname_raw, '\x1b') != nullptr;
        const char *nick_to_apply =
            (preferred_has_ansi && preferred_nickname[0] != '\0')
                ? preferred_nickname
                : (preferred_nickname_raw[0] != '\0' ? preferred_nickname_raw
                                                     : preferred_nickname);

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
        char join_name[SSH_CHATTER_USERNAME_LEN];
        const char *source_name =
            (ctx->user_data_loaded &&
             ctx->user_data.preferred_nickname[0] != '\0')
                ? ctx->user_data.preferred_nickname
                : ctx->user.name;
        snprintf(join_name, sizeof(join_name), "%s", source_name);
        snprintf(join_message, sizeof(join_message),
                 "%s%s*%s [%s] has joined the chat", ANSI_RESET,
                 ANSI_BRIGHT_RED, ANSI_RESET, join_name);
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
                if (ctx->wall_active) {
                    session_wall_exit(ctx, "Exited graffiti wall.");
                    if (ctx->should_exit) {
                        break;
                    }
                    session_render_prompt(ctx, false);
                } else if (ctx->bbs_post_pending || ctx->asciiart_pending) {
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
                        ctx->game.othello != nullptr &&
                        ctx->game.othello->multiplayer) {
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
                !ctx->bracket_paste_active) {
                session_process_line(ctx, "");
                session_clear_input_without_prompt(ctx);
                if (ctx->should_exit) {
                    break;
                }
                session_render_prompt(ctx, false);
                continue;
            }

            if (ch == '\r' || ch == '\n') {
                session_apply_background_fill(ctx);
                const bool composing_draft =
                    ctx->bbs_post_pending || ctx->asciiart_pending;
                if (ctx->wall_active) {
                    ctx->input_buffer[ctx->input_length] = '\0';
                    session_wall_process_line(ctx, ctx->input_buffer,
                                              ctx->input_length);
                    session_clear_input_without_prompt(ctx);
                    if (ctx->should_exit) {
                        break;
                    }
                    if (!ctx->wall_active && !ctx->bracket_paste_active) {
                        session_render_prompt(ctx, false);
                    }
                    continue;
                }
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
                if (ctx->wall_active && ctx->wall_command_mode) {
                    if (ctx->input_length > 0U) {
                        ctx->input_length -= 1U;
                        ctx->input_buffer[ctx->input_length] = '\0';
                    }
                    session_wall_render(ctx, nullptr);
                } else if (ctx->bbs_post_pending &&
                           ctx->pending_bbs_editing_line &&
                    !ctx->bbs_search_active) {
                    session_local_backspace(ctx);
                    session_bbs_render_editor(ctx, nullptr);
                } else {
                    session_local_backspace(ctx);
                }
                /* Reset multi-byte buffer on backspace */
                ctx->multibyte_input_length = 0U;
                continue;
            }

            if (ch == '\t') {
                if (ctx->wall_active && ctx->wall_command_mode) {
                    if (ctx->input_length + 1U < sizeof(ctx->input_buffer)) {
                        ctx->input_buffer[ctx->input_length++] = ' ';
                        ctx->input_buffer[ctx->input_length] = '\0';
                    }
                    session_wall_render(ctx, nullptr);
                    continue;
                }
                if (session_try_command_completion(ctx)) {
                    continue;
                }
                if (ctx->input_length + 1U < sizeof(ctx->input_buffer)) {
                    ctx->input_history_position = -1;
                    session_scrollback_reset_position(ctx);
                    ctx->input_buffer[ctx->input_length++] = ' ';
                    if (ctx->bbs_post_pending && ctx->pending_bbs_editing_line &&
                        !ctx->bbs_search_active) {
                        ctx->input_buffer[ctx->input_length] = '\0';
                        session_bbs_render_editor(ctx, nullptr);
                    } else {
                        session_local_echo_char(ctx, ' ');
                    }
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

            if (ctx->wall_active) {
                if (!ctx->wall_command_mode) {
                    if (encoded_len == 1U && encoded[0] == ':') {
                        ctx->wall_command_mode = true;
                        session_clear_input_without_prompt(ctx);
                        session_wall_render(ctx, nullptr);
                        continue;
                    }
                    if (encoded_len == 1U &&
                        (unsigned char)encoded[0] >= 0x20U &&
                        (unsigned char)encoded[0] <= 0x7eU) {
                        char wall_input[2] = {encoded[0], '\0'};
                        session_wall_process_line(ctx, wall_input, 1U);
                    }
                    continue;
                }

                if (ctx->input_length + encoded_len < sizeof(ctx->input_buffer)) {
                    memcpy(&ctx->input_buffer[ctx->input_length], encoded,
                           encoded_len);
                    ctx->input_length += encoded_len;
                    ctx->input_buffer[ctx->input_length] = '\0';
                }
                session_wall_render(ctx, nullptr);
                continue;
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
                if (ctx->bbs_post_pending && ctx->pending_bbs_editing_line &&
                    !ctx->bbs_search_active) {
                    session_bbs_render_editor(ctx, nullptr);
                } else {
                    for (size_t echo_idx = 0U; echo_idx < encoded_len;
                         ++echo_idx) {
                        session_local_echo_char(ctx, encoded[echo_idx]);
                    }
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
        if (ctx->user_data.preferred_nickname[0] != '\0') {
            snprintf(part_message, sizeof(part_message), "%s%s*%s [%s] has left the chat",
                     ANSI_RESET, ANSI_BRIGHT_BLUE, ANSI_RESET, ctx->user_data.preferred_nickname);
        } else {
            snprintf(part_message, sizeof(part_message), "%s%s*%s [%s] has left the chat",
                     ANSI_RESET, ANSI_BRIGHT_BLUE, ANSI_RESET, ctx->user.name);
        }
        host_history_record_system(ctx->owner, part_message, nullptr);
        chat_room_broadcast(&ctx->owner->room, part_message, nullptr);
        atomic_store(&ctx->room_snapshot_retired, true);
        chat_room_remove(&ctx->owner->room, ctx);
        session_manual_gc_tick(ctx);
        host_manual_gc_tick(ctx->owner);

        struct timespec drain_started = {0};
        bool drain_started_valid =
            (clock_gettime(CLOCK_MONOTONIC, &drain_started) == 0);
        bool drain_logged = false;
        while (atomic_load(&ctx->room_snapshot_refs) > 0U) {
            struct timespec drain_delay = {.tv_sec = 0, .tv_nsec = 10000000L};
            nanosleep(&drain_delay, nullptr);
            if (drain_started_valid && !drain_logged) {
                struct timespec now = {0};
                if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
                    time_t elapsed_sec = now.tv_sec - drain_started.tv_sec;
                    if (elapsed_sec >= 5) {
                        printf("[session] waiting for broadcast drain (%u refs) "
                               "for %s\n",
                               atomic_load(&ctx->room_snapshot_refs),
                               ctx->user.name[0] != '\0' ? ctx->user.name
                                                         : "unknown");
                        drain_logged = true;
                    }
                }
            }
        }
    }

    if (ctx->owner != nullptr) {
        host_nickname_claim_release(ctx->owner, ctx, nullptr);
    }

    session_destroy(ctx);

    SESSION_THREAD_RETURN(nullptr);

#undef SESSION_THREAD_RETURN
}

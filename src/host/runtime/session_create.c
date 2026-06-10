
void session_mark_activity(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    struct timespec now = session_now_monotonic();
    ctx->lifetime_last_activity = now;
    ctx->lifetime_has_activity = true;
    ctx->lifetime_decay_active = false;

    if (ctx->lifetime_units < SESSION_LIFETIME_MAX_UNITS) {
        uint32_t next =
            ctx->lifetime_units + SESSION_LIFETIME_ACTIVITY_BONUS;
        ctx->lifetime_units =
            next > SESSION_LIFETIME_MAX_UNITS ? SESSION_LIFETIME_MAX_UNITS
                                              : next;
    }
}

bool session_enforce_lifetime(session_ctx_t *ctx,
                              const struct timespec *now)
{
    if (ctx == nullptr || now == nullptr) {
        return false;
    }

    if (!ctx->lifetime_has_activity) {
        return false;
    }

    double idle_seconds =
        session_timespec_elapsed_seconds(now, &ctx->lifetime_last_activity);
    if (idle_seconds < SESSION_LIFETIME_INACTIVE_THRESHOLD) {
        ctx->lifetime_decay_active = false;
        return false;
    }

    if (!ctx->lifetime_decay_active) {
        ctx->lifetime_decay_reference = ctx->lifetime_last_activity;
        session_timespec_add_seconds(&ctx->lifetime_decay_reference,
                                     SESSION_LIFETIME_INACTIVE_THRESHOLD);
        ctx->lifetime_decay_active = true;
    }

    double since_decay =
        session_timespec_elapsed_seconds(now, &ctx->lifetime_decay_reference);
    if (since_decay < SESSION_LIFETIME_DECAY_INTERVAL) {
        return false;
    }

    size_t intervals =
        (size_t)(since_decay / SESSION_LIFETIME_DECAY_INTERVAL);
    session_timespec_add_seconds(
        &ctx->lifetime_decay_reference,
        (time_t)(intervals * SESSION_LIFETIME_DECAY_INTERVAL));

    for (size_t idx = 0U; idx < intervals; ++idx) {
        if (ctx->lifetime_units > SESSION_LIFETIME_MIN_UNITS) {
            ctx->lifetime_units /= 2U;
            if (ctx->lifetime_units < SESSION_LIFETIME_MIN_UNITS) {
                ctx->lifetime_units = SESSION_LIFETIME_MIN_UNITS;
            }
        }
    }

    if (ctx->lifetime_units <= SESSION_LIFETIME_MIN_UNITS) {
        session_force_disconnect(ctx, "Session removed after inactivity.");
        return true;
    }

    return false;
}

static session_ctx_t *session_create(void)
{
    // Initially allocate session context in the current (likely global) scope
    session_ctx_t *ctx =
        (session_ctx_t *)sshc_gc_calloc(1U, sizeof(session_ctx_t));

    if (ctx != nullptr) {
        // Create a dedicated memory context for this session
        /* Session max lifetime ≈ 64 units × 300 s + 1200 s idle = 20400 s */
        ctx->memory_context = sshc_memory_context_create("session",
                                                           TT_SECOND(20400));
        if (ctx->memory_context == nullptr) {
            sshc_gc_free(ctx);
            return nullptr;
        }
        sshc_memory_context_set_gc_aggressive(ctx->memory_context);

        // Initialize session-specific owner for strict isolation
        ctx->session_owner = ttak_owner_create(TTAK_OWNER_STRICT_ISOLATION);
        if (ctx->session_owner == nullptr) {
            sshc_memory_context_destroy(ctx->memory_context);
            sshc_gc_free(ctx);
            return nullptr;
        }

        // Push session context so subsequent allocations happen in this scope
        sshc_memory_context_t *session_scope =
            sshc_memory_context_push(ctx->memory_context);

        ctx->user.is_authenticated = false;
        ctx->session_id = 0U;
        ctx->session_data = nullptr;
        ctx->prefer_utf16_output = false;
        ctx->newline_mode = SESSION_NEWLINE_MODE_AUTO;
        ctx->active_codepage = SESSION_CODEPAGE_CP437; /* Default to CP437 */
        ctx->morse_feed_enabled = false;
        ctx->exit_notice_sent = false;
        ctx->wall_brush_char = '#';
        snprintf(ctx->wall_brush_color_name,
                 sizeof(ctx->wall_brush_color_name), "%s", "white");
        atomic_init(&ctx->room_snapshot_retired, false);
        atomic_init(&ctx->room_snapshot_refs, 0U);
        ctx->has_last_output_line = false;
        ctx->disable_output_dedup = false;
        ctx->lifetime_units = SESSION_LIFETIME_INITIAL_UNITS;
        ctx->lifetime_last_activity = session_now_monotonic();
        ctx->lifetime_decay_reference = ctx->lifetime_last_activity;
        ctx->lifetime_has_activity = true;
        ctx->lifetime_decay_active = false;

        printf("[encoding-debug] phase=ctx_init transport_kind=%d "
               "prefer_utf16_output=%d output_kind=%d\n",
               (int)ctx->transport_kind, (int)ctx->prefer_utf16_output,
               (int)ctx->output_kind);

        if (display_model_init(&ctx->display_model, 256U)) {
            ctx->display_model_initialized = true;
        }

        bool ascii_ready = session_asciiart_buffer_acquire(ctx);
        if (!ascii_ready) {
            session_asciiart_buffer_release(ctx);

            sshc_memory_context_pop(session_scope);
            if (ctx->session_owner != nullptr) {
                ttak_owner_destroy(ctx->session_owner);
            }
            sshc_memory_context_destroy(ctx->memory_context);
            sshc_gc_free(ctx);
            return nullptr;
        }

        sshc_memory_context_pop(session_scope);
    }
    return ctx;
}

static void session_configure_tcp_keepalive(ssh_session session)
{
    if (session == nullptr) {
        return;
    }

    const int socket_fd = ssh_get_fd(session);
    if (socket_fd < 0) {
        return;
    }

    int enabled = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE, &enabled,
                   sizeof(enabled)) < 0) {
        fprintf(stderr, "[session] setsockopt SO_KEEPALIVE failed\n");
    }

    /* Disable Nagle's algorithm so small interactive packets (e.g.
     * single-character echo) are sent immediately without waiting for
     * ACK coalescing.  This removes the typing-delay perceived by users. */
    if (setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &enabled,
                   sizeof(enabled)) < 0) {
        fprintf(stderr, "[session] setsockopt TCP_NODELAY failed\n");
    }

#ifdef TCP_KEEPIDLE
    {
        int idle_seconds = SSH_CHATTER_TCP_KEEPALIVE_IDLE;
        if (setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle_seconds,
                       sizeof(idle_seconds)) < 0) {
            fprintf(stderr, "[session] setsockopt TCP_KEEPIDLE failed\n");
        }
    }
#endif

#ifdef TCP_KEEPALIVE
    {
        int idle_seconds = SSH_CHATTER_TCP_KEEPALIVE_IDLE;
        if (setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle_seconds,
                       sizeof(idle_seconds)) < 0) {
            fprintf(stderr, "[session] setsockopt TCP_KEEPALIVE failed\n");
        }
    }
#endif

#ifdef TCP_KEEPINTVL
    {
        int interval_seconds = SSH_CHATTER_TCP_KEEPALIVE_INTERVAL;
        if (setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval_seconds,
                       sizeof(interval_seconds)) < 0) {
            fprintf(stderr, "[session] setsockopt TCP_KEEPINTVL failed\n");
        }
    }
#endif

#ifdef TCP_KEEPCNT
    {
        int keepalive_probes = SSH_CHATTER_TCP_KEEPALIVE_COUNT;
        if (setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPCNT, &keepalive_probes,
                       sizeof(keepalive_probes)) < 0) {
            fprintf(stderr, "[session] setsockopt TCP_KEEPCNT failed\n");
        }
    }
#endif

#ifdef TCP_USER_TIMEOUT
    {
        int user_timeout_ms = SSH_CHATTER_TCP_USER_TIMEOUT_MS;
        if (setsockopt(socket_fd, IPPROTO_TCP, TCP_USER_TIMEOUT,
                       &user_timeout_ms, sizeof(user_timeout_ms)) < 0) {
            fprintf(stderr, "[session] setsockopt TCP_USER_TIMEOUT failed\n");
        }
    }
#endif

    // Enable TCP_NODELAY for low-latency interactive sessions
    if (setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &enabled,
                   sizeof(enabled)) < 0) {
        fprintf(stderr, "[session] setsockopt TCP_NODELAY failed\n");
    }
}

// Configure SSH-level session options after accept (timeout, nodelay, rekey).
static void session_configure_ssh_options(ssh_session session)
{
    if (session == nullptr) {
        return;
    }

    // Set SSH operation timeout (matches openssh LoginGraceTime behavior)
    long timeout_seconds = SSH_CHATTER_SSH_TIMEOUT_SECONDS;
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout_seconds);

    // Enable TCP_NODELAY at SSH layer as well
    int nodelay = 1;
    ssh_options_set(session, SSH_OPTIONS_NODELAY, &nodelay);
}

static const char *session_cp437_scope_label(session_cp437_scope_t cp437_scope)
{
    switch (cp437_scope) {
    case SESSION_CP437_SCOPE_SYSTEM_ONLY:
        return "system-only";
    case SESSION_CP437_SCOPE_CHAT_ONLY:
        return "chat-only";
    case SESSION_CP437_SCOPE_ALL:
    default:
        return "all output";
    }
}

static bool session_cp437_scope_parse(const char *token,
                                      session_cp437_scope_t *out_scope)
{
    if (token == nullptr || out_scope == nullptr || token[0] == '\0') {
        return false;
    }

    if (strcasecmp(token, "system") == 0 ||
        strcasecmp(token, "system-only") == 0 ||
        strcasecmp(token, "system_only") == 0) {
        *out_scope = SESSION_CP437_SCOPE_SYSTEM_ONLY;
        return true;
    }

    if (strcasecmp(token, "chat") == 0 || strcasecmp(token, "chat-only") == 0 ||
        strcasecmp(token, "chat_only") == 0) {
        *out_scope = SESSION_CP437_SCOPE_CHAT_ONLY;
        return true;
    }

    if (strcasecmp(token, "all") == 0 || strcasecmp(token, "both") == 0) {
        *out_scope = SESSION_CP437_SCOPE_ALL;
        return true;
    }

    return false;

static void session_handle_morse(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    static const char *kUsage = "Usage: /morse <on <filter>|off|status>";

    char working[SSH_CHATTER_MAX_INPUT_LEN];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
        trim_whitespace_inplace(working);
    }

    char *command = strtok(working, " \t");
    if (command == nullptr || strcasecmp(command, "status") == 0) {
        char status[SSH_CHATTER_MESSAGE_LIMIT];
        if (ctx->morse_feed_enabled) {
            snprintf(status, sizeof(status),
                     "Morse feed is ON. Filter: '%s'. /morse off to mute.",
                     ctx->morse_filter);
        } else {
            snprintf(status, sizeof(status),
                     "Morse feed is OFF. /morse on <filter> to resume.");
        }
        session_send_system_line(ctx, status);
        return;
    }

    if (strcasecmp(command, "on") == 0) {
        char *filter = strtok(NULL, "");
        if (filter == NULL || filter[0] == '\0') {
            session_send_system_line(ctx, kUsage);
            return;
        }
        trim_whitespace_inplace(filter);
        if (strlen(filter) >= sizeof(ctx->morse_filter)) {
            session_send_system_line(ctx, "Filter is too long.");
            return;
        }
        strncpy(ctx->morse_filter, filter, sizeof(ctx->morse_filter) - 1);
        ctx->morse_filter[sizeof(ctx->morse_filter) - 1] = '\0';
        ctx->morse_feed_enabled = true;
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message),
                 "Morse relay enabled with filter '%s'. Use /morse off to silence.",
                 ctx->morse_filter);
        session_send_system_line(ctx, message);
        return;
    }

    if (strcasecmp(command, "off") == 0) {
        ctx->morse_feed_enabled = false;
        ctx->morse_filter[0] = '\0';
        session_send_system_line(ctx,
                                 "Morse relay disabled for this session.");
        return;
    }

    session_send_system_line(ctx, kUsage);
}

static void session_handle_morse_chat(session_ctx_t *ctx,
                                      const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    static const char *kUsage = "Usage: /morse-chat <text>";

    if (arguments == nullptr || arguments[0] == '\0') {
        session_send_system_line(ctx, kUsage);
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, kUsage);
        return;
    }

    morse_client_t *client = ctx->owner->morse_client;
    if (client == nullptr || !morse_client_connected(client)) {
        session_send_system_line(ctx,
                                 "Morse relay is not connected. Please try "
                                 "again shortly.");
        return;
    }

    if (!morse_client_send(client, working)) {
        session_send_system_line(ctx, "Unable to send Morse chat right now.");
        return;
    }

    session_send_system_line(ctx, "[MORSE] message transmitted.");
}

static bool host_asciiart_cooldown_active(host_t *host, const char *ip,
                                          const struct timespec *now,
                                          long *remaining_seconds)
{
    if (host == nullptr || ip == nullptr || ip[0] == '\0') {
        if (remaining_seconds != nullptr) {
            *remaining_seconds = 0L;
        }
        return false;
    }

    struct timespec current = {0, 0};
    if (now != nullptr) {
        current = *now;
    } else if (clock_gettime(CLOCK_MONOTONIC, &current) != 0) {
        current.tv_sec = time(nullptr);
        current.tv_nsec = 0L;
    }

    bool active = false;
    long remaining = 0L;

    ttak_mutex_lock(&host->lock);
    join_activity_entry_t *entry = host_find_join_activity_locked(host, ip);
    if (entry != nullptr && entry->asciiart_has_cooldown) {
        struct timespec expiry = entry->last_asciiart_post;
        expiry.tv_sec += SSH_CHATTER_ASCIIART_COOLDOWN_SECONDS;
        if (timespec_compare(&current, &expiry) >= 0) {
            entry->asciiart_has_cooldown = false;
        } else {
            active = true;
            struct timespec diff = timespec_diff(&expiry, &current);
            remaining = diff.tv_sec;
            if (diff.tv_nsec > 0L) {
                ++remaining;
            }
            if (remaining < 0L) {
                remaining = 0L;
            }
        }
    }
    ttak_mutex_unlock(&host->lock);

    if (remaining_seconds != nullptr) {
        *remaining_seconds = active ? remaining : 0L;
    }

    return active;
}

static void host_asciiart_register_post(host_t *host, const char *ip,
                                        const struct timespec *when)
{
    if (host == nullptr || ip == nullptr || ip[0] == '\0' || when == nullptr) {
        return;
    }

    ttak_mutex_lock(&host->lock);
    join_activity_entry_t *entry = host_ensure_join_activity_locked(host, ip);
    if (entry != nullptr) {
        entry->last_asciiart_post = *when;
        entry->asciiart_has_cooldown = true;
    }
    ttak_mutex_unlock(&host->lock);
}

static void session_asciiart_reset(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    ctx->asciiart_pending = false;
    ctx->asciiart_target = SESSION_ASCIIART_TARGET_NONE;
    if (ctx->asciiart_buffer != nullptr) {
        ctx->asciiart_buffer[0] = '\0';
    }
    ctx->asciiart_length = 0U;
    ctx->asciiart_line_count = 0U;
}

static bool session_asciiart_cooldown_active(session_ctx_t *ctx,
                                             struct timespec *now,
                                             long *remaining_seconds)
{
    if (ctx == nullptr) {
        return false;
    }

    struct timespec current;
    if (clock_gettime(CLOCK_MONOTONIC, &current) != 0) {
        current.tv_sec = time(nullptr);
        current.tv_nsec = 0L;
    }

    if (now != nullptr) {
        *now = current;
    }

    long session_remaining = 0L;
    bool session_active = false;
    if (ctx->asciiart_has_cooldown) {
        struct timespec expiry = ctx->last_asciiart_post;
        expiry.tv_sec += SSH_CHATTER_ASCIIART_COOLDOWN_SECONDS;
        if (timespec_compare(&current, &expiry) >= 0) {
            ctx->asciiart_has_cooldown = false;
        } else {
            session_active = true;
            struct timespec diff = timespec_diff(&expiry, &current);
            session_remaining = diff.tv_sec;
            if (diff.tv_nsec > 0L) {
                ++session_remaining;
            }
            if (session_remaining < 0L) {
                session_remaining = 0L;
            }
        }
    }

    long ip_remaining = 0L;
    bool ip_active = host_asciiart_cooldown_active(ctx->owner, ctx->client_ip,
                                                   &current, &ip_remaining);

    if (!session_active && !ip_active) {
        if (remaining_seconds != nullptr) {
            *remaining_seconds = 0L;
        }
        return false;
    }

    long max_remaining = session_active ? session_remaining : 0L;
    if (ip_active && ip_remaining > max_remaining) {
        max_remaining = ip_remaining;
    }

    if (remaining_seconds != nullptr) {
        *remaining_seconds = max_remaining;
    }

    return true;
}

static void session_asciiart_begin(session_ctx_t *ctx,
                                   session_asciiart_target_t target)
{
    if (ctx == nullptr || target == SESSION_ASCIIART_TARGET_NONE) {
        return;
    }

    if (ctx->bbs_post_pending) {
        if (ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART) {
            const char *terminator = session_editor_terminator(ctx);
            char notice[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(notice, sizeof(notice),
                     "You are already composing ASCII art. Finish it with %s.",
                     terminator);
            session_send_system_line(ctx, notice);
        } else {
            session_send_system_line(
                ctx, "Finish your BBS draft before starting ASCII art.");
        }
        return;
    }

    if (ctx->asciiart_pending) {
        const char *terminator = session_asciiart_terminator(ctx);
        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice),
                 "You are already composing ASCII art. Finish it with %s.",
                 terminator);
        session_send_system_line(ctx, notice);
        return;
    }

    if (target == SESSION_ASCIIART_TARGET_CHAT) {
        struct timespec now;
        long remaining = 0L;
        if (session_asciiart_cooldown_active(ctx, &now, &remaining)) {
            if (remaining < 1L) {
                remaining = 1L;
            }
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message),
                     "You can share another ASCII art in %ld second%s.",
                     remaining, remaining == 1L ? "" : "s");
            session_send_system_line(ctx, message);
            return;
        }
    }

    session_asciiart_reset(ctx);
    ctx->asciiart_pending = true;
    ctx->asciiart_target = target;

    session_bbs_reset_pending_post(ctx);
    if (!session_asciiart_buffer_acquire(ctx) ||
        !session_bbs_workspace_acquire(ctx)) {
        session_send_system_line(
            ctx, "ASCII art editor is unavailable right now.");
        session_asciiart_reset(ctx);
        return;
    }
    ctx->editor_mode = SESSION_EDITOR_MODE_ASCIIART;
    ctx->bbs_post_pending = true;

    size_t ascii_bytes = (size_t)SSH_CHATTER_ASCIIART_BUFFER_LEN;
    char status[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(status, sizeof(status),
             "ASCII art composer ready (max %u lines, up to %zu bytes, "
             "10-minute cooldown per IP).",
             SSH_CHATTER_ASCIIART_MAX_LINES, ascii_bytes);

    session_bbs_render_editor(ctx, status);
}

static void session_asciiart_import_from_editor(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    if (!session_bbs_workspace_acquire(ctx) ||
        !session_asciiart_buffer_acquire(ctx)) {
        session_send_system_line(
            ctx, "ASCII art workspace is unavailable right now.");
        return;
    }

    session_bbs_recalculate_line_count(ctx);

    size_t copy_len = ctx->pending_bbs_body_length;
    if (copy_len >= SSH_CHATTER_ASCIIART_BUFFER_LEN) {
        copy_len = SSH_CHATTER_ASCIIART_BUFFER_LEN - 1U;
    }

    if (copy_len > 0U) {
        memcpy(ctx->asciiart_buffer, ctx->pending_bbs_body, copy_len);
    }
    ctx->asciiart_buffer[copy_len] = '\0';
    ctx->asciiart_length = copy_len;
    ctx->asciiart_line_count = ctx->pending_bbs_line_count;
    if (ctx->asciiart_line_count > SSH_CHATTER_ASCIIART_MAX_LINES) {
        ctx->asciiart_line_count = SSH_CHATTER_ASCIIART_MAX_LINES;
    }
}

static void session_asciiart_commit(session_ctx_t *ctx)
{
    if (ctx == nullptr || !ctx->asciiart_pending) {
        return;
    }

    if (!session_asciiart_buffer_acquire(ctx)) {
        session_asciiart_reset(ctx);
        return;
    }

    if (ctx->asciiart_length == 0U) {
        session_asciiart_cancel(ctx, "ASCII art draft discarded.");
        return;
    }

    if (ctx->owner == nullptr) {
        session_asciiart_reset(ctx);
        return;
    }

    if (session_security_check_text(ctx, "ASCII art", ctx->asciiart_buffer,
                                    ctx->asciiart_length,
                                    false) != HOST_SECURITY_SCAN_CLEAN) {
        session_asciiart_reset(ctx);
        return;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        now.tv_sec = time(nullptr);
        now.tv_nsec = 0L;
    }

    ctx->last_asciiart_post = now;
    ctx->asciiart_has_cooldown = true;
    host_asciiart_register_post(ctx->owner, ctx->client_ip, &now);

    chat_history_entry_t entry = {0};
    if (!host_history_record_user(ctx->owner, ctx, ctx->asciiart_buffer, true,
                                  &entry)) {
        session_asciiart_reset(ctx);
        return;
    }

    session_send_history_entry(ctx, &entry);
    chat_room_broadcast_entry(&ctx->owner->room, &entry, ctx);
    host_notify_external_clients(ctx->owner, &entry);

    ctx->last_message_time = now;
    ctx->has_last_message_time = true;

    session_asciiart_reset(ctx);
}

static void session_asciiart_cancel(session_ctx_t *ctx, const char *reason)
{
    if (ctx == nullptr || !ctx->asciiart_pending) {
        return;
    }

    const bool used_editor = ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART;
    session_asciiart_reset(ctx);
    if (used_editor) {
        session_bbs_reset_pending_post(ctx);
    }
    if (reason != nullptr && reason[0] != '\0') {
        session_send_system_line(ctx, reason);
    }
}

static bool session_asciiart_capture_continue(const session_ctx_t *ctx)
{
    return ctx != nullptr && ctx->asciiart_pending;
}

static bool session_bbs_capture_continue(const session_ctx_t *ctx)
{
    return ctx != nullptr && ctx->bbs_post_pending;
}

static void session_capture_multiline_text(
    session_ctx_t *ctx, const char *text, session_text_line_consumer_t consumer,
    session_text_continue_predicate_t should_continue)
{
    if (ctx == nullptr || text == nullptr || consumer == nullptr ||
        should_continue == nullptr) {
        return;
    }

    char line[SSH_CHATTER_MAX_INPUT_LEN];
    size_t line_length = 0U;
    bool emitted = false;

    const char *cursor = text;
    while (*cursor != '\0') {
        char ch = *cursor++;
        if (ch == '\r') {
            if (*cursor == '\n') {
                ++cursor;
            }
            line[line_length] = '\0';
            consumer(ctx, line);
            emitted = true;
            line_length = 0U;
            if (!should_continue(ctx)) {
                return;
            }
            continue;
        }

        if (ch == '\n') {
            line[line_length] = '\0';
            consumer(ctx, line);
            emitted = true;
            line_length = 0U;
            if (!should_continue(ctx)) {
                return;
            }
            continue;
        }

        if (line_length + 1U < sizeof(line)) {
            line[line_length++] = ch;
        }
    }

    if (line_length > 0U || !emitted) {
        line[line_length] = '\0';
        consumer(ctx, line);
    }
}

static void session_asciiart_capture_text(session_ctx_t *ctx, const char *text)
{
    if (ctx == nullptr || !ctx->asciiart_pending || text == nullptr) {
        return;
    }

    session_capture_multiline_text(ctx, text, session_asciiart_capture_line,
                                   session_asciiart_capture_continue);
}

static void session_asciiart_capture_line(session_ctx_t *ctx, const char *line)
{
    if (ctx == nullptr || !ctx->asciiart_pending) {
        return;
    }

    if (!session_asciiart_buffer_acquire(ctx)) {
        session_send_system_line(ctx, "ASCII art buffer unavailable.");
        return;
    }

    char trimmed[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(trimmed, sizeof(trimmed), "%s", line != nullptr ? line : "");
    trim_whitespace_inplace(trimmed);
    if (session_asciiart_matches_terminator(trimmed)) {
        session_asciiart_commit(ctx);
        return;
    }

    if (ctx->asciiart_line_count >= SSH_CHATTER_ASCIIART_MAX_LINES) {
        session_send_system_line(
            ctx, "ASCII art line limit reached. Use the terminator to finish.");
        return;
    }

    if (line == nullptr) {
        line = "";
    }

    const char *full_message =
        "ASCII art buffer is full. Additional text ignored.";
    const char *truncate_message =
        "Line truncated to fit within the ASCII art size limit.";

    size_t buffer_capacity = SSH_CHATTER_ASCIIART_BUFFER_LEN;

    if (ctx->asciiart_length >= buffer_capacity - 1U) {
        session_send_system_line(ctx, full_message);
        return;
    }

    size_t available = buffer_capacity - ctx->asciiart_length - 1U;
    const size_t newline_cost = ctx->asciiart_length > 0U ? 1U : 0U;
    if (available < newline_cost) {
        session_send_system_line(ctx, full_message);
        return;
    }

    size_t line_length = strlen(line);
    size_t max_line_length =
        (available > newline_cost) ? (available - newline_cost) : 0U;
    if (line_length > max_line_length) {
        line_length = max_line_length;
        session_send_system_line(ctx, truncate_message);
    }

    if (ctx->asciiart_length > 0U) {
        ctx->asciiart_buffer[ctx->asciiart_length++] = '\n';
    }

    if (line_length > 0U) {
        memcpy(ctx->asciiart_buffer + ctx->asciiart_length, line, line_length);
        ctx->asciiart_length += line_length;
    }

    ctx->asciiart_buffer[ctx->asciiart_length] = '\0';
    ctx->asciiart_line_count += 1U;
}

//

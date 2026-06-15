static void session_channel_write_line_ending(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    bool prefer_crlf = false;
    switch (ctx->newline_mode) {
    case SESSION_NEWLINE_MODE_CRLF:
        prefer_crlf = true;
        break;
    case SESSION_NEWLINE_MODE_LF:
        prefer_crlf = false;
        break;
    case SESSION_NEWLINE_MODE_AUTO:
    default:
        prefer_crlf = (ctx->os_name[0] != '\0' &&
                       strcasecmp(ctx->os_name, "windows") == 0);
        break;
    }

    if (prefer_crlf) {
        session_channel_write(ctx, "\r\n", 2U);
    } else {
        session_channel_write(ctx, "\n", 1U);
    }
}

static void session_render_banner_text(session_ctx_t *ctx, const char *banner)
{
    if (ctx == nullptr || banner == nullptr) {
        return;
    }

    bool locked = session_output_lock(ctx);

    bool is_ans = false;
    if (ctx->owner != nullptr && ctx->owner->welcome_banner_loaded) {
        if (strcmp(banner, ctx->owner->welcome_banner) == 0) {
            is_ans = ctx->owner->welcome_banner_is_ans;
        }
    }

    const char *cursor = banner;
    while (true) {
        const char *newline = strchr(cursor, '\n');
        size_t length = newline != nullptr
                            ? (size_t)(newline - cursor)
                            : strnlen(cursor, SSH_CHATTER_MESSAGE_LIMIT);
        while (length > 0U && cursor[length - 1U] == '\r') {
            --length;
        }

        if (is_ans) {
            static const char cursor_home[] = "\033[H";
            session_channel_write(ctx, cursor_home, sizeof(cursor_home) - 1U);

            int state = 0;
            char write_buf[1024];
            size_t write_idx = 0;

            for (size_t i = 0; i < length; ++i) {
                unsigned char b = (unsigned char)cursor[i];
                if (state == 0) {
                    if (b == 0x1b) {
                        state = 1;
                        write_buf[write_idx++] = (char)b;
                    } else if (b >= 0x80) {
                        char utf8_bytes[4];
                        size_t utf8_len = session_cp437_byte_to_utf8(b, utf8_bytes, sizeof(utf8_bytes));
                        for (size_t j = 0; j < utf8_len; ++j) {
                            write_buf[write_idx++] = utf8_bytes[j];
                            if (write_idx >= sizeof(write_buf) - 8) {
                                session_channel_write(ctx, write_buf, write_idx);
                                write_idx = 0;
                            }
                        }
                    } else {
                        write_buf[write_idx++] = (char)b;
                    }
                } else if (state == 1) {
                    if (b == '[') {
                        state = 2;
                        write_buf[write_idx++] = (char)b;
                    } else {
                        state = 0;
                        write_buf[write_idx++] = (char)b;
                    }
                } else if (state == 2) {
                    write_buf[write_idx++] = (char)b;
                    if (b >= 0x40 && b <= 0x7E) {
                        state = 0;
                    }
                }

                if (write_idx >= sizeof(write_buf) - 8) {
                    session_channel_write(ctx, write_buf, write_idx);
                    write_idx = 0;
                }
            }

            if (write_idx > 0) {
                session_channel_write(ctx, write_buf, write_idx);
            }

            session_channel_write_line_ending(ctx);
            session_note_output_lines(ctx, 1U);
        } else {
            session_fill_line_with_theme(ctx);
            static const char column_reset[] = "\033[1G";
            session_channel_write(ctx, column_reset, sizeof(column_reset) - 1U);
            if (length > 0U) {
                session_channel_write(ctx, cursor, length);
            }
            session_channel_write_line_ending(ctx);
            session_note_output_lines(ctx, 1U);
        }

        if (newline == nullptr) {
            break;
        }

        cursor = newline + 1;
        if (*cursor == '\0') {
            break;
        }
    }

    if (locked) {
        session_output_unlock(ctx);
    }
}

void host_set_welcome_banner(host_t *host, const char *banner, bool is_ans)
{
    if (host == nullptr) {
        return;
    }

    if (banner == nullptr || banner[0] == '\0') {
        host->welcome_banner[0] = '\0';
        host->welcome_banner_loaded = false;
        host->welcome_banner_is_ans = false;
        return;
    }

    snprintf(host->welcome_banner, sizeof(host->welcome_banner), "%s", banner);
    host->welcome_banner_loaded = true;
    host->welcome_banner_is_ans = is_ans;
}

static void session_game_show_camouflage(session_ctx_t *ctx);

#define SESSION_REALTIME_CLEAR_INTERVAL 100U
#define SESSION_REALTIME_RECENT_LIMIT SSH_CHATTER_REALTIME_RECENT_LIMIT

static void session_realtime_refresh(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    // When the display model is active, skip the incremental clear+redraw
    // cycle that can erase visible history and show only a few recent lines.
    // The full-frame redraw via session_process_pending_sink handles this.
    if (ctx->display_model_initialized) {
        ctx->realtime_line_count = 0U;
        return;
    }

    const bool previous_capture = ctx->capture_realtime_output;
    ctx->capture_realtime_output = false;

    // Bundle the clear and redraw into a single buffered write so the
    // terminal receives them atomically, preventing the visible blank
    // flash that causes screen flickering.
    const bool buffering_started = !ctx->output_buffering_enabled;
    if (buffering_started) {
        session_output_buffer_start(ctx);
    }

    session_clear_screen(ctx);

    size_t lines_to_show = ctx->realtime_recent_count;
    if (lines_to_show > SESSION_REALTIME_RECENT_LIMIT) {
        lines_to_show = SESSION_REALTIME_RECENT_LIMIT;
    }

    for (size_t idx = 0U; idx < lines_to_show; ++idx) {
        size_t slot =
            (ctx->realtime_recent_start + idx) % SESSION_REALTIME_RECENT_LIMIT;
        session_send_plain_line(ctx, ctx->realtime_recent_lines[slot]);
    }

    if (buffering_started) {
        session_output_buffer_stop(ctx);
    }

    ctx->capture_realtime_output = previous_capture;
    ctx->realtime_line_count = 0U;

    if (ctx->history_scroll_position == 0U && !ctx->bracket_paste_active) {
        session_refresh_input_line(ctx);
    }
}

static void session_realtime_record_line(session_ctx_t *ctx, const char *line)
{
    if (ctx == nullptr || line == nullptr || !ctx->capture_realtime_output ||
        ctx->history_scroll_position > 0U) {
        return;
    }

    size_t insert_pos = 0U;
    if (ctx->realtime_recent_count < SESSION_REALTIME_RECENT_LIMIT) {
        insert_pos = ctx->realtime_recent_count;
        ++ctx->realtime_recent_count;
    } else {
        ctx->realtime_recent_start =
            (ctx->realtime_recent_start + 1U) % SESSION_REALTIME_RECENT_LIMIT;
        insert_pos =
            (ctx->realtime_recent_start + ctx->realtime_recent_count - 1U) %
            SESSION_REALTIME_RECENT_LIMIT;
    }

    size_t line_length = strnlen(line, SSH_CHATTER_MESSAGE_LIMIT - 1U);
    if (line_length >= sizeof(ctx->realtime_recent_lines[insert_pos])) {
        line_length = sizeof(ctx->realtime_recent_lines[insert_pos]) - 1U;
    }
    memcpy(ctx->realtime_recent_lines[insert_pos], line, line_length);
    ctx->realtime_recent_lines[insert_pos][line_length] = '\0';

    if (ctx->realtime_line_count < SIZE_MAX) {
        ++ctx->realtime_line_count;
    }
}


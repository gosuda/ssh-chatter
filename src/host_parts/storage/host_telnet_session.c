static const char SESSION_COLUMN_RESET[] = "\033[1G";

static void session_fill_line_with_theme(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    const char *bg = ctx->system_bg_code != nullptr ? ctx->system_bg_code : "";
    const size_t bg_len = strlen(bg);

    unsigned int width = ctx->terminal_width > 0U ? ctx->terminal_width : 80U;
    if (width > SSH_CHATTER_MESSAGE_LIMIT) {
        width = SSH_CHATTER_MESSAGE_LIMIT;
    }

    session_channel_write(ctx, SESSION_COLUMN_RESET,
                          sizeof(SESSION_COLUMN_RESET) - 1U);

    if (bg_len > 0U) {
        session_channel_write(ctx, bg, bg_len);
    }

    if (width > 0U) {
        char spaces[64];
        memset(spaces, ' ', sizeof(spaces));
        unsigned int remaining = width;
        while (remaining > 0U) {
            size_t chunk =
                remaining < sizeof(spaces) ? remaining : sizeof(spaces);
            session_channel_write(ctx, spaces, chunk);
            remaining -= (unsigned int)chunk;
        }
    }

    session_channel_write(ctx, SESSION_COLUMN_RESET,
                          sizeof(SESSION_COLUMN_RESET) - 1U);

    if (bg_len > 0U) {
        session_channel_write(ctx, bg, bg_len);
    }
}

static void session_apply_background_fill(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    bool locked = session_output_lock(ctx);
    session_fill_line_with_theme(ctx);

    if (locked) {
        session_output_unlock(ctx);
    }
}

static size_t session_append_fragment(char *dest, size_t dest_size,
                                      size_t offset, const char *fragment);

static bool session_sequence_resets_theme(const char *sequence_start,
                                          size_t sequence_length,
                                          bool *reset_fg, bool *reset_bg,
                                          bool *reset_bold)
{
    if (sequence_start == nullptr || sequence_length == 0U) {
        return false;
    }

    if (sequence_start[0] != '\033' || sequence_length < 2U) {
        return false;
    }

    const char final_char = sequence_start[sequence_length - 1U];
    if (final_char != 'm') {
        return false;
    }

    if (reset_fg != nullptr) {
        *reset_fg = false;
    }
    if (reset_bg != nullptr) {
        *reset_bg = false;
    }
    if (reset_bold != nullptr) {
        *reset_bold = false;
    }

    const char *params_start = sequence_start + 2U;
    const char *params_end = sequence_start + sequence_length - 1U;
    bool reset_all = false;

    if (params_start >= params_end) {
        reset_all = true;
    }

    while (!reset_all && params_start < params_end) {
        if (*params_start == ';') {
            ++params_start;
            continue;
        }

        char *parse_end = nullptr;
        long value = strtol(params_start, &parse_end, 10);
        if (parse_end == params_start) {
            reset_all = true;
            break;
        }

        if (value == 0L) {
            reset_all = true;
            break;
        }
        if (value == 39L && reset_fg != nullptr) {
            *reset_fg = true;
        }
        if (value == 49L && reset_bg != nullptr) {
            *reset_bg = true;
        }
        if ((value == 21L || value == 22L) && reset_bold != nullptr) {
            *reset_bold = true;
        }

        params_start = parse_end;
    }

    if (reset_all) {
        if (reset_fg != nullptr) {
            *reset_fg = true;
        }
        if (reset_bg != nullptr) {
            *reset_bg = true;
        }
        if (reset_bold != nullptr) {
            *reset_bold = true;
        }
    }

    return reset_all || (reset_fg != nullptr && *reset_fg) ||
           (reset_bg != nullptr && *reset_bg) ||
           (reset_bold != nullptr && *reset_bold);
}

static size_t session_prepare_themed_output(session_ctx_t *ctx,
                                            const char *render_source,
                                            char *dest, size_t dest_size)
{
    if (dest == nullptr || dest_size == 0U) {
        return 0U;
    }

    dest[0] = '\0';

    const char *bg = ctx->system_bg_code != nullptr ? ctx->system_bg_code : "";
    const char *fg = ctx->system_fg_code != nullptr ? ctx->system_fg_code : "";
    const char *bold = ctx->system_is_bold ? ANSI_BOLD : "";

    size_t offset = 0U;
    offset = session_append_fragment(dest, dest_size, offset, bg);
    offset = session_append_fragment(dest, dest_size, offset, fg);
    offset = session_append_fragment(dest, dest_size, offset, bold);

    const char *cursor = render_source;
    while (cursor != nullptr && *cursor != '\0') {
        if ((size_t)offset >= dest_size - 1U) {
            break;
        }

        if (*cursor == '\033' && cursor[1] == '[') {
            const char *sequence_start = cursor;
            cursor += 2;
            while (*cursor != '\0' && (*cursor < '@' || *cursor > '~')) {
                ++cursor;
            }
            if (*cursor == '\0') {
                break;
            }
            ++cursor;
            size_t sequence_length = (size_t)(cursor - sequence_start);
            if (sequence_length >= dest_size - offset) {
                sequence_length = dest_size - offset - 1U;
            }
            memcpy(dest + offset, sequence_start, sequence_length);
            offset += sequence_length;
            dest[offset] = '\0';

            bool reset_fg = false;
            bool reset_bg = false;
            bool reset_bold = false;
            if (session_sequence_resets_theme(sequence_start, sequence_length,
                                              &reset_fg, &reset_bg,
                                              &reset_bold)) {
                if (reset_bg) {
                    offset =
                        session_append_fragment(dest, dest_size, offset, bg);
                }
                if (reset_fg) {
                    offset =
                        session_append_fragment(dest, dest_size, offset, fg);
                }
                if (ctx->system_is_bold && reset_bold) {
                    offset = session_append_fragment(dest, dest_size, offset,
                                                     ANSI_BOLD);
                }
            }

            continue;
        }

        dest[offset++] = *cursor++;
        dest[offset] = '\0';
    }

    return offset;
}

static void session_write_rendered_line(session_ctx_t *ctx,
                                        const char *render_source)
{
    if (ctx == nullptr || render_source == nullptr ||
        !session_transport_active(ctx)) {
        return;
    }

    bool locked = session_output_lock(ctx);
    session_fill_line_with_theme(ctx);

    char buffer[SSH_CHATTER_MESSAGE_LIMIT * 4U];
    size_t offset = session_prepare_themed_output(ctx, render_source, buffer,
                                                  sizeof(buffer));

    if (offset > 0U) {
        session_channel_write(ctx, buffer, offset);
    }

    session_channel_write(ctx, "\r\n", 2U);
    session_note_output_lines(ctx, 1U);

    if (locked) {
        session_output_unlock(ctx);
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        session_channel_flush(ctx);
    }
}

static void session_send_caption_line(session_ctx_t *ctx, const char *message)
{
    if (ctx == nullptr || message == nullptr ||
        !session_transport_active(ctx)) {
        return;
    }

    bool locked = session_output_lock(ctx);
    session_channel_write(ctx, SESSION_COLUMN_RESET,
                          sizeof(SESSION_COLUMN_RESET) - 1U);
    session_channel_write(ctx, ANSI_INSERT_LINE, sizeof(ANSI_INSERT_LINE) - 1U);

    session_output_kind_t previous_kind =
        session_output_set_kind(ctx, SESSION_OUTPUT_KIND_SYSTEM);
    session_write_rendered_line(ctx, message);
    session_output_restore_kind(ctx, previous_kind);

    if (locked) {
        session_output_unlock(ctx);
    }
}

static void session_render_caption_with_offset(session_ctx_t *ctx,
                                               const char *message,
                                               size_t move_up)
{
    if (ctx == nullptr || message == nullptr ||
        !session_transport_active(ctx)) {
        return;
    }

    if (move_up == 0U) {
        session_send_caption_line(ctx, message);
        return;
    }

    bool locked = session_output_lock(ctx);
    session_channel_write(ctx, "\033[s", 3U);

    char command[32];
    int written = snprintf(command, sizeof(command), "\033[%zuA", move_up);
    if (written > 0 && (size_t)written < sizeof(command)) {
        session_channel_write(ctx, command, (size_t)written);
    }

    session_channel_write(ctx, SESSION_COLUMN_RESET,
                          sizeof(SESSION_COLUMN_RESET) - 1U);
    session_output_kind_t previous_kind =
        session_output_set_kind(ctx, SESSION_OUTPUT_KIND_SYSTEM);
    session_write_rendered_line(ctx, message);
    session_output_restore_kind(ctx, previous_kind);
    session_channel_write(ctx, "\033[u", 3U);

    if (locked) {
        session_output_unlock(ctx);
    }
}

static void session_telnet_send_option(session_ctx_t *ctx,
                                       unsigned char command,
                                       unsigned char option)
{
    if (ctx == nullptr || ctx->telnet_fd < 0) {
        return;
    }

    unsigned char payload[3] = {TELNET_IAC, command, option};
    send(ctx->telnet_fd, payload, sizeof(payload), MSG_NOSIGNAL);
}

static void session_telnet_request_terminal_type(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->telnet_fd < 0 ||
        ctx->telnet_terminal_type_requested) {
        return;
    }

    unsigned char payload[] = {
        TELNET_IAC, TELNET_CMD_SB, TELNET_OPT_TERMINAL_TYPE,
        1U,         TELNET_IAC,    TELNET_CMD_SE};
    send(ctx->telnet_fd, payload, sizeof(payload), MSG_NOSIGNAL);
    ctx->telnet_terminal_type_requested = true;
}

static void session_telnet_handle_option(session_ctx_t *ctx,
                                         unsigned char command,
                                         unsigned char option)
{
    if (ctx == nullptr) {
        return;
    }

    switch (command) {
    case TELNET_CMD_DO:
        if (option == TELNET_OPT_BINARY) {
            session_telnet_send_option(ctx, TELNET_CMD_WILL, option);
        } else if (option == TELNET_OPT_SUPPRESS_GO_AHEAD ||
                   option == TELNET_OPT_ECHO) {
            session_telnet_send_option(ctx, TELNET_CMD_WILL, option);
        } else if (option == TELNET_OPT_TERMINAL_TYPE) {
            session_telnet_send_option(ctx, TELNET_CMD_WONT, option);
        } else {
            session_telnet_send_option(ctx, TELNET_CMD_WONT, option);
        }
        break;
    case TELNET_CMD_DONT:
        session_telnet_send_option(ctx, TELNET_CMD_WONT, option);
        break;
    case TELNET_CMD_WILL:
        if (option == TELNET_OPT_BINARY ||
            option == TELNET_OPT_SUPPRESS_GO_AHEAD) {
            session_telnet_send_option(ctx, TELNET_CMD_DO, option);
        } else if (option == TELNET_OPT_TERMINAL_TYPE) {
            session_telnet_send_option(ctx, TELNET_CMD_DO, option);
            session_telnet_request_terminal_type(ctx);
        } else if (option == TELNET_OPT_NAWS) {
            session_telnet_send_option(ctx, TELNET_CMD_DO, option);
        } else {
            session_telnet_send_option(ctx, TELNET_CMD_DONT, option);
        }
        break;
    case TELNET_CMD_WONT:
        session_telnet_send_option(ctx, TELNET_CMD_DONT, option);
        break;
    default:
        break;
    }
}

static void session_telnet_initialize(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->telnet_fd < 0 || ctx->telnet_negotiated) {
        return;
    }

    session_telnet_send_option(ctx, TELNET_CMD_WILL, TELNET_OPT_BINARY);
    session_telnet_send_option(ctx, TELNET_CMD_DO, TELNET_OPT_BINARY);
    session_telnet_send_option(ctx, TELNET_CMD_WILL, TELNET_OPT_ECHO);
    session_telnet_send_option(ctx, TELNET_CMD_WILL,
                               TELNET_OPT_SUPPRESS_GO_AHEAD);
    session_telnet_send_option(ctx, TELNET_CMD_DO,
                               TELNET_OPT_SUPPRESS_GO_AHEAD);
    session_telnet_send_option(ctx, TELNET_CMD_DONT, TELNET_OPT_LINEMODE);
    session_telnet_send_option(ctx, TELNET_CMD_WONT, TELNET_OPT_STATUS);
    session_telnet_send_option(ctx, TELNET_CMD_DO, TELNET_OPT_TERMINAL_TYPE);
    session_telnet_send_option(ctx, TELNET_CMD_WONT, TELNET_OPT_TERMINAL_SPEED);
    session_telnet_send_option(ctx, TELNET_CMD_DO, TELNET_OPT_NAWS);

    ctx->telnet_negotiated = true;
}

static int session_telnet_read_byte(session_ctx_t *ctx, unsigned char *out,
                                    int timeout_ms)
{
    if (ctx == nullptr || out == nullptr || ctx->telnet_fd < 0) {
        return SSH_ERROR;
    }

    if (ctx->telnet_pending_valid) {
        ctx->telnet_pending_valid = false;
        *out = (unsigned char)ctx->telnet_pending_char;
        return 1;
    }

    for (;;) {
        struct pollfd pfd = {
            .fd = ctx->telnet_fd,
            .events = POLLIN,
            .revents = 0,
        };

        int poll_result = poll(&pfd, 1, timeout_ms);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SSH_ERROR;
        }
        if (poll_result == 0) {
            return SSH_AGAIN;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            ctx->telnet_eof = true;
            return 0;
        }

        unsigned char byte = 0U;
        ssize_t read_result = recv(ctx->telnet_fd, &byte, 1, 0);
        if (read_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return SSH_ERROR;
        }
        if (read_result == 0) {
            ctx->telnet_eof = true;
            return 0;
        }

        if (byte == TELNET_IAC) {
            unsigned char command = 0U;
            ssize_t command_result = recv(ctx->telnet_fd, &command, 1, 0);
            if (command_result <= 0) {
                if (command_result < 0 && errno == EINTR) {
                    continue;
                }
                ctx->telnet_eof = (command_result == 0);
                return ctx->telnet_eof ? 0 : SSH_ERROR;
            }

            if (command == TELNET_IAC) {
                *out = TELNET_IAC;
                return 1;
            }

            if (command == TELNET_CMD_DO || command == TELNET_CMD_DONT ||
                command == TELNET_CMD_WILL || command == TELNET_CMD_WONT) {
                unsigned char option = 0U;
                ssize_t option_result = recv(ctx->telnet_fd, &option, 1, 0);
                if (option_result <= 0) {
                    if (option_result < 0 && errno == EINTR) {
                        continue;
                    }
                    ctx->telnet_eof = (option_result == 0);
                    return ctx->telnet_eof ? 0 : SSH_ERROR;
                }
                session_telnet_handle_option(ctx, command, option);
                continue;
            }

            if (command == TELNET_CMD_SB) {
                unsigned char option = 0U;
                ssize_t option_result = recv(ctx->telnet_fd, &option, 1, 0);
                if (option_result <= 0) {
                    if (option_result < 0 && errno == EINTR) {
                        continue;
                    }
                    ctx->telnet_eof = (option_result == 0);
                    return ctx->telnet_eof ? 0 : SSH_ERROR;
                }

                if (option == TELNET_OPT_TERMINAL_TYPE) {
                    unsigned char qualifier = 0U;
                    ssize_t qual_result =
                        recv(ctx->telnet_fd, &qualifier, 1, 0);
                    if (qual_result <= 0) {
                        if (qual_result < 0 && errno == EINTR) {
                            continue;
                        }
                        ctx->telnet_eof = (qual_result == 0);
                        return ctx->telnet_eof ? 0 : SSH_ERROR;
                    }

                    char type_buffer[SSH_CHATTER_TERMINAL_TYPE_LEN];
                    size_t type_len = 0U;
                    bool finished = false;

                    while (!finished) {
                        unsigned char chunk = 0U;
                        ssize_t chunk_result =
                            recv(ctx->telnet_fd, &chunk, 1, 0);
                        if (chunk_result < 0) {
                            if (errno == EINTR) {
                                continue;
                            }
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                continue;
                            }
                            return SSH_ERROR;
                        }
                        if (chunk_result == 0) {
                            ctx->telnet_eof = true;
                            return 0;
                        }

                        if (chunk == TELNET_IAC) {
                            unsigned char next = 0U;
                            ssize_t next_result =
                                recv(ctx->telnet_fd, &next, 1, 0);
                            if (next_result < 0) {
                                if (errno == EINTR) {
                                    continue;
                                }
                                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                    continue;
                                }
                                return SSH_ERROR;
                            }
                            if (next_result == 0) {
                                ctx->telnet_eof = true;
                                return 0;
                            }

                            if (next == TELNET_CMD_SE) {
                                finished = true;
                                break;
                            }
                            if (next == TELNET_IAC) {
                                if (type_len + 1U < sizeof(type_buffer)) {
                                    type_buffer[type_len++] = (char)TELNET_IAC;
                                }
                            }
                            continue;
                        }

                        if (type_len + 1U < sizeof(type_buffer)) {
                            type_buffer[type_len++] = (char)chunk;
                        }
                    }

                    if (type_len < sizeof(type_buffer)) {
                        type_buffer[type_len] = '\0';
                    } else {
                        type_buffer[sizeof(type_buffer) - 1U] = '\0';
                    }

                    if (qualifier == 0U) {
                        trim_whitespace_inplace(type_buffer);
                        if (type_buffer[0] != '\0') {
                            for (size_t idx = 0U; type_buffer[idx] != '\0';
                                 ++idx) {
                                type_buffer[idx] = (char)toupper(
                                    (unsigned char)type_buffer[idx]);
                            }
                            snprintf(ctx->terminal_type,
                                     sizeof(ctx->terminal_type), "%s",
                                     type_buffer);
                            session_refresh_output_encoding(ctx);
                        }
                    }
                } else if (option == TELNET_OPT_NAWS) {
                    /* RFC 1073: NAWS sends 4 data bytes followed by IAC SE.
                     * Format: <width-hi> <width-lo> <height-hi> <height-lo>
                     * Any 0xFF data byte is doubled (escaped as IAC IAC). */
                    unsigned char naws_data[4];
                    size_t naws_pos = 0U;
                    bool naws_done = false;
                    unsigned char prev_byte = 0U;

                    while (!naws_done) {
                        unsigned char nb = 0U;
                        ssize_t nr = recv(ctx->telnet_fd, &nb, 1, 0);
                        if (nr < 0) {
                            if (errno == EINTR) {
                                continue;
                            }
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                continue;
                            }
                            return SSH_ERROR;
                        }
                        if (nr == 0) {
                            ctx->telnet_eof = true;
                            return 0;
                        }

                        if (prev_byte == TELNET_IAC) {
                            prev_byte = 0U;
                            if (nb == TELNET_CMD_SE) {
                                naws_done = true;
                                break;
                            }
                            if (nb == TELNET_IAC && naws_pos < 4U) {
                                naws_data[naws_pos++] = TELNET_IAC;
                            }
                            continue;
                        }

                        if (nb == TELNET_IAC) {
                            prev_byte = TELNET_IAC;
                            continue;
                        }

                        if (naws_pos < 4U) {
                            naws_data[naws_pos++] = nb;
                        }
                    }

                    if (naws_pos == 4U) {
                        unsigned int width =
                            ((unsigned int)naws_data[0] << 8) |
                            (unsigned int)naws_data[1];
                        unsigned int height =
                            ((unsigned int)naws_data[2] << 8) |
                            (unsigned int)naws_data[3];
                        if (width > 0U && width <= (unsigned)SSH_CHATTER_MESSAGE_LIMIT) {
                            ctx->terminal_width = width;
                        }
                        if (height > 0U) {
                            ctx->terminal_height = height;
                        }
                    }
                } else {
                    unsigned char prev = 0U;
                    for (;;) {
                        unsigned char chunk = 0U;
                        ssize_t chunk_result =
                            recv(ctx->telnet_fd, &chunk, 1, 0);
                        if (chunk_result < 0) {
                            if (errno == EINTR) {
                                continue;
                            }
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                continue;
                            }
                            return SSH_ERROR;
                        }
                        if (chunk_result == 0) {
                            ctx->telnet_eof = true;
                            return 0;
                        }
                        if (prev == TELNET_IAC && chunk == TELNET_CMD_SE) {
                            break;
                        }
                        prev = (chunk == TELNET_IAC) ? TELNET_IAC : 0U;
                    }
                }
                continue;
            }

            if (command == TELNET_CMD_NOP || command == TELNET_CMD_DM ||
                command == TELNET_CMD_BREAK) {
                continue;
            }

            continue;
        }

        if (byte == '\r') {
            for (;;) {
                unsigned char next = 0U;
                ssize_t next_result =
                    recv(ctx->telnet_fd, &next, 1, MSG_PEEK | MSG_DONTWAIT);
                if (next_result < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    return SSH_ERROR;
                }
                if (next_result == 0) {
                    ctx->telnet_eof = true;
                    break;
                }

                if (next == '\n' || next == '\0') {
                    recv(ctx->telnet_fd, &next, 1, 0);
                } else {
                    recv(ctx->telnet_fd, &next, 1, 0);
                    ctx->telnet_pending_char = (int)next;
                    ctx->telnet_pending_valid = true;
                }
                break;
            }

            ctx->telnet_consume_next_lf = true;
            *out = '\r';
            return 1;
        }

        *out = byte;
        return 1;
    }
}

static bool session_telnet_collect_line(session_ctx_t *ctx, char *buffer,
                                        size_t length, bool mask_input)
{
    if (ctx == nullptr || buffer == nullptr || length == 0U) {
        return false;
    }

    size_t written = 0U;
    bool ignore_next_newline = ctx->telnet_consume_next_lf;
    unsigned char glyph_sizes[SSH_CHATTER_MESSAGE_LIMIT];
    size_t glyph_count = 0U;
    unsigned int idle_time_ms = 0U;
    bool idle_warning_sent = false;

    while (!ctx->should_exit) {
        unsigned char byte = 0U;
        int read_result =
            session_telnet_read_byte(ctx, &byte, SESSION_TELNET_INPUT_POLL_MS);
        if (read_result == SSH_AGAIN) {
            idle_time_ms += SESSION_TELNET_INPUT_POLL_MS;
            if (!idle_warning_sent &&
                idle_time_ms >= SESSION_TELNET_IDLE_WARNING_MS) {
                session_send_system_line(
                    ctx,
                    "Still waiting for input... type /exit to disconnect.");
                idle_warning_sent = true;
            }
            if (idle_time_ms >= SESSION_TELNET_IDLE_TIMEOUT_MS) {
                session_send_system_line(
                    ctx,
                    "No response detected, closing the telnet session.");
                ctx->should_exit = true;
                return false;
            }
            continue;
        }
        if (read_result <= 0) {
            ctx->should_exit = true;
            return false;
        }
        idle_time_ms = 0U;
        idle_warning_sent = false;

        if (ignore_next_newline) {
            if (byte == '\n' || byte == '\0') {
                ignore_next_newline = false;
                continue;
            }
            ignore_next_newline = false;
        }

        if (byte == '\0') {
            // Some telnet clients emit NUL padding bytes (e.g. CR NUL) when
            // sending user input. Treat them as no-ops so clients like IcyTerm
            // stay connected.
            continue;
        }

        if (byte == '\r' || byte == '\n') {
            session_channel_write(ctx, "\r\n", 2U);
            if (byte == '\r') {
                ignore_next_newline = true;
            }
            break;
        }

        if (byte == 0x7FU || byte == '\b') {
            if (glyph_count > 0U) {
                size_t remove = glyph_sizes[--glyph_count];
                if (remove > written) {
                    written = 0U;
                } else {
                    written -= remove;
                }
                buffer[written] = '\0';
                session_channel_write(ctx, "\b \b", 3U);
            }
            /* Also reset multi-byte buffer on backspace */
            ctx->multibyte_input_length = 0U;
            continue;
        }

        if (byte == 0x03U || byte == 0x04U) {
            ctx->should_exit = true;
            return false;
        }

        if (byte < 0x20U) {
            continue;
        }

        char encoded[8];
        size_t encoded_len = 0U;

        if (ctx->cp437_input_enabled) {
            encoded_len = session_codepage_byte_to_utf8(
                ctx->active_codepage, &ctx->codepage_ctx, byte, encoded,
                sizeof(encoded));
            if (encoded_len == 0U) {
                encoded[0] = '?';
                encoded_len = 1U;
            }
        } else {
            /* UTF-8 mode - pass through */
            encoded[0] = (char)byte;
            encoded_len = 1U;
        }

        if (written + encoded_len >= length) {
            const char bell = '\a';
            session_channel_write(ctx, &bell, 1U);
            continue;
        }

        if (glyph_count < sizeof(glyph_sizes)) {
            glyph_sizes[glyph_count++] = (unsigned char)encoded_len;
        }

        memcpy(&buffer[written], encoded, encoded_len);
        written += encoded_len;
        buffer[written] = '\0';

        if (mask_input) {
            const char mask = '*';
            session_channel_write(ctx, &mask, 1U);
        } else {
            session_channel_write(ctx, encoded, encoded_len);
        }
    }

    buffer[written] = '\0';
    ctx->telnet_consume_next_lf = ignore_next_newline;
    return !ctx->should_exit;
}

static int session_pw_auth_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool session_pw_auth_decode_hex(const char *hex, uint8_t *out,
                                       size_t out_len)
{
    if (hex == nullptr || out == nullptr || out_len == 0U) {
        return false;
    }

    size_t hex_len = strlen(hex);
    if (hex_len != out_len * 2U) {
        return false;
    }

    for (size_t idx = 0U; idx < out_len; ++idx) {
        int hi = session_pw_auth_hex_value(hex[idx * 2U]);
        int lo = session_pw_auth_hex_value(hex[idx * 2U + 1U]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[idx] = (uint8_t)((hi << 4U) | lo);
    }

    return true;
}

static bool session_telnet_pw_auth_lookup(host_t *host, const char *username,
                                          uint8_t *salt_out, size_t salt_len,
                                          uint8_t *hash_out, size_t hash_len)
{
    if (host == nullptr || username == nullptr || username[0] == '\0' ||
        salt_out == nullptr || hash_out == nullptr || salt_len == 0U ||
        hash_len == 0U) {
        return false;
    }

    if (host->pw_auth_file_path[0] == '\0') {
        return false;
    }

    FILE *fp = fopen(host->pw_auth_file_path, "rb");
    if (fp == nullptr) {
        return false;
    }

    char line[SSH_CHATTER_MESSAGE_LIMIT];
    bool found = false;

    while (fgets(line, sizeof(line), fp) != nullptr) {
        size_t length = strcspn(line, "\r\n");
        line[length] = '\0';

        char *first = strchr(line, ':');
        if (first == nullptr) {
            continue;
        }
        char *second = strchr(first + 1, ':');
        if (second == nullptr) {
            continue;
        }

        *first = '\0';
        *second = '\0';

        const char *name = line;
        const char *salt_hex = first + 1;
        const char *hash_hex = second + 1;

        if (name[0] == '\0') {
            continue;
        }

        if (strcasecmp(name, username) != 0) {
            continue;
        }

        uint8_t decoded_salt[SECURITY_LAYER_SALT_LEN];
        uint8_t decoded_hash[SECURITY_LAYER_HASH_LEN];

        if (!session_pw_auth_decode_hex(salt_hex, decoded_salt,
                                        sizeof(decoded_salt)) ||
            !session_pw_auth_decode_hex(hash_hex, decoded_hash,
                                        sizeof(decoded_hash))) {
            continue;
        }

        size_t salt_copy =
            salt_len < sizeof(decoded_salt) ? salt_len : sizeof(decoded_salt);
        size_t hash_copy =
            hash_len < sizeof(decoded_hash) ? hash_len : sizeof(decoded_hash);
        if (salt_copy > 0U) {
            memset(salt_out, 0, salt_len);
            memcpy(salt_out, decoded_salt, salt_copy);
        }
        if (hash_copy > 0U) {
            memset(hash_out, 0, hash_len);
            memcpy(hash_out, decoded_hash, hash_copy);
        }
        found = true;
    }

    int read_error = ferror(fp);
    fclose(fp);

    if (read_error != 0) {
        return false;
    }

    return found;
}

static bool session_telnet_pw_auth_verify(host_t *host, const char *username,
                                          const char *password)
{
    if (host == nullptr || username == nullptr || username[0] == '\0' ||
        password == nullptr) {
        return false;
    }

    uint8_t salt[SECURITY_LAYER_SALT_LEN];
    uint8_t expected[SECURITY_LAYER_HASH_LEN];
    if (!session_telnet_pw_auth_lookup(host, username, salt, sizeof(salt),
                                       expected, sizeof(expected))) {
        return false;
    }

    uint8_t computed[SECURITY_LAYER_HASH_LEN];
    security_layer_hash_password(password, salt, computed);
    return memcmp(computed, expected, sizeof(expected)) == 0;
}

static bool session_telnet_pw_auth_exists(host_t *host, const char *username)
{
    uint8_t salt[SECURITY_LAYER_SALT_LEN];
    uint8_t hash[SECURITY_LAYER_HASH_LEN];
    return session_telnet_pw_auth_lookup(host, username, salt, sizeof(salt),
                                         hash, sizeof(hash));
}

static bool session_telnet_prompt_unicode_check(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return false;
    }

    char resp[2];
    memset(resp, 0, 2);
    unsigned int attempts = 0U;
    const unsigned int max_attempts = 5U;
    bool prefer_unicode = true;

    while (!ctx->should_exit && attempts < max_attempts) {
        session_send_plain_line(ctx, "Are you using Unicode terminal? <Y/N>");
        session_send_plain_line(ctx,
                                "If you are using SyncTerm/other Retro Terms,");
        session_send_plain_line(ctx, "Type N");
        session_channel_write(ctx, "> ", 2U);

        if (!session_telnet_collect_line(ctx, resp, sizeof(resp), false)) {
            return false;
        }

        trim_whitespace_inplace(resp);

        if (resp[0] == '\0') {
            resp[0] = 'N';
        }

        if (is_pure_ascii(resp)) {
            to_lowercase(resp);
        } else {
            session_send_system_line(ctx, "Type Y/N. No other response.");
            ++attempts;
            continue;
        }

        prefer_unicode = (resp[0] != 'n');
        break;
    }

    if (attempts >= max_attempts && !ctx->should_exit) {
        session_send_system_line(
            ctx, "No clear answer, defaulting to Unicode output.");
        prefer_unicode = true;
    }

    if (prefer_unicode) {
        session_handle_retro(ctx, "off");
    } else {
        session_handle_retro(ctx, "on");
        session_handle_set_ui_lang(ctx, "en");
    }

    return session_telnet_login_prompt(ctx);
}

bool session_telnet_login_prompt(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return false;
    }

    char id_buffer[SSH_CHATTER_USERNAME_LEN];
    memset(id_buffer, 0, sizeof(id_buffer));

    while (!ctx->should_exit) {
        session_send_system_line(ctx, "Enter ID (nickname required):");
        session_channel_write(ctx, "> ", 2U);

        char input_line[SSH_CHATTER_MESSAGE_LIMIT];
        if (!session_telnet_collect_line(ctx, input_line, sizeof(input_line),
                                         false)) {
            return false;
        }

        trim_whitespace_inplace(input_line);

        snprintf(id_buffer, sizeof(id_buffer), "%.*s",
                 SSH_CHATTER_USERNAME_LEN - 1, input_line);
        trim_whitespace_inplace(id_buffer);
        if (id_buffer[0] == '\0') {
            session_send_system_line(ctx, "A nickname is required to proceed.");
            continue;
        }

        char provided_password[128];
        provided_password[0] = '\0';

        user_data_record_t user_data;
        memset(&user_data, 0, sizeof(user_data));
        const char *ip_for_lookup =
            (ctx->client_ip[0] != '\0') ? ctx->client_ip : nullptr;
        bool user_data_loaded = false;
        bool user_data_has_password = false;

        if (host_user_data_load_existing(ctx->owner, id_buffer, ip_for_lookup,
                                         &user_data, false)) {
            user_data_loaded = true;
            user_data_has_password = !security_layer_is_zero_hash(
                user_data.password_hash, sizeof(user_data.password_hash));
        }

        if (!user_data_has_password) {
            user_data_record_t fallback_data;
            if (host_user_data_load_existing(ctx->owner, id_buffer, nullptr,
                                             &fallback_data, false)) {
                bool fallback_has_password = !security_layer_is_zero_hash(
                    fallback_data.password_hash,
                    sizeof(fallback_data.password_hash));
                if (!user_data_loaded || fallback_has_password) {
                    user_data = fallback_data;
                    user_data_loaded = true;
                }
                if (fallback_has_password) {
                    user_data_has_password = true;
                }
            }
        }

        lan_operator_credential_t *lan_credential = nullptr;
        if (ctx->owner != nullptr) {
            lan_credential =
                host_find_lan_operator_credential(ctx->owner, id_buffer);
            if (lan_credential != nullptr &&
                !session_is_lan_client(ctx->client_ip)) {
                session_send_system_line(
                    ctx, "That nickname is reserved for LAN operators.");
                continue;
            }
        }

        bool pw_auth_available =
            session_telnet_pw_auth_exists(ctx->owner, id_buffer);
        bool requires_password = user_data_has_password || pw_auth_available;

        if (lan_credential != nullptr) {
            requires_password = true;
        }

        char confirmation_prompt[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(confirmation_prompt, sizeof(confirmation_prompt),
                 "Are you sure with a name <%s>? <Y/N>", id_buffer);
        session_send_system_line(ctx, confirmation_prompt);
        session_channel_write(ctx, "> ", 2U);

        char confirmation_response[8];
        if (!session_telnet_collect_line(ctx, confirmation_response,
                                         sizeof(confirmation_response),
                                         false)) {
            return false;
        }

        trim_whitespace_inplace(confirmation_response);
        if (confirmation_response[0] == '\0') {
            session_send_system_line(ctx, "Nickname confirmation is required.");
            continue;
        }

        if (!is_pure_ascii(confirmation_response)) {
            session_send_system_line(ctx, "Please answer Y or N.");
            continue;
        }

        to_lowercase(confirmation_response);
        if (confirmation_response[0] != 'y') {
            session_send_system_line(ctx, "Let's pick a nickname again.");
            continue;
        }

        const char *password_to_check = nullptr;
        if (provided_password[0] != '\0') {
            password_to_check = provided_password;
        }

        if (!requires_password) {
            snprintf(ctx->user.name, sizeof(ctx->user.name), "%s", id_buffer);
            ctx->user.is_authenticated = true;
            session_send_system_line(ctx, "Login successful.");
            return true;
        }

        if (password_to_check == nullptr) {
            session_send_system_line(ctx, "Password required. Enter password:");
            session_channel_write(ctx, "> ", 2U);

            char password_buffer[128];
            if (!session_telnet_collect_line(ctx, password_buffer,
                                             sizeof(password_buffer), true)) {
                return false;
            }
            if (password_buffer[0] == '\0') {
                session_send_system_line(ctx, "Password required.");
                continue;
            }
            // Copy into provided_password for later comparisons and make that
            // the canonical pointer for verification. The local
            // password_buffer will go out of scope after this block, so
            // retaining its address would leave password_to_check referencing
            // invalid stack memory.
            snprintf(provided_password, sizeof(provided_password), "%s",
                     password_buffer);
            password_to_check = provided_password;
        }

        if (lan_credential != nullptr) {
            if (lan_credential->password[0] != '\0' &&
                strcmp(lan_credential->password, password_to_check) == 0) {
                snprintf(ctx->user.name, sizeof(ctx->user.name), "%s",
                         lan_credential->nickname);
                ctx->lan_operator_credentials_valid = true;
                ctx->user.is_lan_operator = true;
                ctx->user.is_authenticated = true;
                (void)host_user_data_load_existing(ctx->owner, ctx->user.name,
                                                   ctx->client_ip, &user_data,
                                                   true);
                session_send_system_line(ctx, "Login successful.");
                return true;
            }

            session_send_system_line(ctx, "Invalid password.");
            continue;
        }

        bool authenticated = false;
        if (user_data_loaded && user_data_has_password) {
            uint8_t hashed_password[SECURITY_LAYER_HASH_LEN];
            security_layer_hash_password(
                password_to_check, user_data.password_salt, hashed_password);
            authenticated = memcmp(hashed_password, user_data.password_hash,
                                   sizeof(hashed_password)) == 0;
        }

        if (!authenticated && pw_auth_available) {
            authenticated = session_telnet_pw_auth_verify(ctx->owner, id_buffer,
                                                          password_to_check);

            if (authenticated) {
                (void)host_user_data_load_existing(
                    ctx->owner, id_buffer, ctx->client_ip, &user_data, true);
            }
        }

        if (authenticated) {
            snprintf(ctx->user.name, sizeof(ctx->user.name), "%s", id_buffer);
            ctx->user.is_authenticated = true;
            session_send_system_line(ctx, "Login successful.");
            return true;
        }

        session_send_system_line(ctx, "Invalid password.");
    }

    return false;
}

static int session_transport_read(session_ctx_t *ctx, void *buffer,
                                  size_t length, int timeout_ms)
{
    if (ctx == nullptr || buffer == nullptr || length == 0U) {
        return SSH_ERROR;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        unsigned char *output = (unsigned char *)buffer;
        size_t produced = 0U;

        while (produced < length) {
            unsigned char byte = 0U;
            int read_result = session_telnet_read_byte(ctx, &byte, timeout_ms);
            if (read_result == SSH_AGAIN) {
                if (produced > 0U) {
                    return (int)produced;
                }
                return SSH_AGAIN;
            }
            if (read_result <= 0) {
                if (produced > 0U) {
                    return (int)produced;
                }
                return read_result;
            }

            output[produced++] = byte;
            if (timeout_ms >= 0) {
                break;
            }
        }

        return (int)produced;
    }

    const uint32_t chunk =
        (length > UINT32_MAX) ? UINT32_MAX : (uint32_t)length;

    if (ctx->channel == nullptr) {
        return SSH_ERROR;
    }

    if (!ssh_channel_is_open(ctx->channel) || ssh_channel_is_eof(ctx->channel)) {
        return SSH_ERROR;
    }

    if (timeout_ms >= 0) {
        int total = 0;
        int result = 0;
        do {
            result = ssh_channel_read_timeout(
                ctx->channel, (char *)buffer + total, chunk - (uint32_t)total,
                0, total > 0 ? 0 : timeout_ms);
            if (result > 0) {
                total += result;
            }
        } while (result > 0 && total < (int)chunk);
        return total > 0 ? total : result;
    }

    return ssh_channel_read(ctx->channel, buffer, chunk, 0);
}

static bool session_is_first_message_bot_probe(const session_ctx_t *ctx,
                                               const char *message)
{
    if (ctx == nullptr || message == nullptr) {
        return false;
    }

    if (ctx->chat_message_count > 0U) {
        return false;
    }

    if (strncmp(ctx->user.name, message, 64) == 0) {
        return true;
    }

    if (strncmp(ctx->user.name, "Host:", 5) == 0 &&
        strncmp(message, "User-Agent: ", 12) == 0) {
        return true;
    }

   static const char *kTelnetBotFirstMessages[] = {"enable", "nconnect"};
    
   char normalized[SSH_CHATTER_MESSAGE_LIMIT];
   snprintf(normalized, sizeof(normalized), "%s", message);
   trim_whitespace_inplace(normalized);
   
   if (normalized[0] == '\0') {
       return false;
   }
   
   const size_t pattern_count =
       sizeof(kTelnetBotFirstMessages) / sizeof(kTelnetBotFirstMessages[0]);
   
   for (size_t idx = 0U; idx < pattern_count; ++idx) {
       if (strcasecmp(normalized, kTelnetBotFirstMessages[idx]) == 0) {
           return true;
       }
   }

   return false;
}

static void session_deliver_outgoing_message(session_ctx_t *ctx,
                                             const char *message,
                                             bool clear_prompt_text)
{
    if (ctx == nullptr || ctx->owner == nullptr || message == nullptr) {
        return;
    }

    const bool was_scrolled_back = ctx->history_scroll_position > 0U;
    if (was_scrolled_back) {
        session_clear_screen(ctx);
        session_scrollback_reset_position(ctx);
        session_flag_should_sink(ctx);
    }

    // Trim whitespace and check if message is empty
    char trimmed[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(trimmed, sizeof(trimmed), "%s", message);
    trim_whitespace_inplace(trimmed);

    if (trimmed[0] == '\0') {
        // Don't send empty messages
        return;
    }

    if (session_is_first_message_bot_probe(ctx, trimmed)) {
        session_send_system_line(
            ctx,
            "Connection closed: first message matched a telnet bot command.");
        session_force_disconnect(
            ctx, "Disconnected for suspected telnet bot command.");
        return;
    }

    chat_history_entry_t entry = {0};
    if (!host_history_record_user(ctx->owner, ctx, trimmed, false, &entry)) {
        return;
    }

    if (ctx->chat_message_count < SIZE_MAX) {
        ttak_mutex_lock(&ctx->chat_message_count_mutex);
        ctx->chat_message_count += 1U;
        ttak_mutex_unlock(&ctx->chat_message_count_mutex);
    }

    session_scrollback_reset_position(ctx);

    // Append the sender's own message immediately so local UX matches how
    // remote messages are shown in real time.
    if (ctx->history_scroll_position == 0U) {
        char line[SSH_CHATTER_MESSAGE_LIMIT * 2U];
        chat_history_entry_format_user_line(&entry, line, sizeof(line), true);
        session_send_plain_line(ctx, line);

        if (entry.attachment_type != CHAT_ATTACHMENT_NONE &&
            entry.attachment_target[0] != '\0') {
            const char *label = chat_attachment_type_label(entry.attachment_type);
            char attachment_line[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(attachment_line, sizeof(attachment_line),
                     "    (%s)" ANSI_RESET " %s", label,
                     entry.attachment_target);
            session_send_plain_line(ctx, attachment_line);

            if (entry.attachment_caption[0] != '\0') {
                char caption_line[SSH_CHATTER_MESSAGE_LIMIT];
                snprintf(caption_line, sizeof(caption_line), "    \342\206\263 %s",
                         entry.attachment_caption);
                session_send_plain_line(ctx, caption_line);
            }
        }
    }

    if (ctx->history_scroll_position == 0U && !ctx->bracket_paste_active) {
        if (clear_prompt_text) {
            ctx->input_length = 0U;
            ctx->input_buffer[0] = '\0';
        }
    }
    chat_room_broadcast_entry(&ctx->owner->room, &entry, ctx);
    host_notify_external_clients(ctx->owner, &entry);
    session_clear_pending_sink(ctx);

    (void)host_eliza_intervene(ctx, trimmed, nullptr, false);

    size_t message_length = strnlen(trimmed, SSH_CHATTER_MESSAGE_LIMIT);

    if (!host_moderation_queue_chat(ctx, trimmed, message_length)) {
        (void)session_security_check_text(ctx, "chat message", trimmed,
                                          message_length, true);
    }
}

// session_send_line writes a single line while preserving the session's
// background color even when individual strings reset their ANSI attributes by
// clearing the row with the palette tint before printing.
static void session_send_line(session_ctx_t *ctx, const char *message)
{
    if (ctx == nullptr || !session_transport_active(ctx) ||
        message == nullptr) {
        return;
    }

    session_output_kind_t previous_kind =
        session_output_set_kind(ctx, SESSION_OUTPUT_KIND_CHAT);

    char buffer[SSH_CHATTER_MESSAGE_LIMIT];
    memset(buffer, 0, sizeof(buffer));
    strncpy(buffer, message, SSH_CHATTER_MESSAGE_LIMIT);
    buffer[SSH_CHATTER_MESSAGE_LIMIT - 1] = '\0';

    char stripped[SSH_CHATTER_MESSAGE_LIMIT];
    bool suppress_translation = translation_strip_no_translate_prefix(
        buffer, stripped, sizeof(stripped));
    const char *render_text = suppress_translation ? stripped : buffer;

    session_write_rendered_line(ctx, render_text);

    bool at_latest = (ctx->history_scroll_position == 0U);
    size_t placeholder_lines = 0U;
    const bool scope_allows_translation =
        (!translator_should_limit_to_chat_bbs() ||
         ctx->translation_manual_scope_override);
    const bool translation_ready =
        scope_allows_translation && !suppress_translation &&
        !ctx->translation_suppress_output && ctx->translation_enabled &&
        ctx->output_translation_enabled &&
        ctx->output_translation_language[0] != '\0' && render_text[0] != '\0';
    if (translation_ready && at_latest && !ctx->in_bbs_mode &&
        !ctx->in_rss_mode) {
        size_t spacing = ctx->translation_caption_spacing;
        if (spacing > 8U) {
            spacing = 8U;
        }
        placeholder_lines = spacing;
    }

    if (translation_ready && session_translation_queue_caption(
                                 ctx, render_text, placeholder_lines)) {
        if (placeholder_lines > 0U) {
            session_translation_reserve_placeholders(ctx, placeholder_lines);
        }
    }

    session_translation_flush_ready(ctx);

    session_output_restore_kind(ctx, previous_kind);
}

static size_t session_append_fragment(char *dest, size_t dest_size,
                                      size_t offset, const char *fragment)
{
    if (dest == nullptr || dest_size == 0U) {
        return offset;
    }

    if (offset >= dest_size) {
        return dest_size > 0U ? dest_size - 1U : offset;
    }

    if (fragment == nullptr) {
        dest[offset] = '\0';
        return offset;
    }

    const size_t fragment_len = strlen(fragment);
    if (fragment_len == 0U) {
        return offset;
    }

    if (offset >= dest_size - 1U) {
        dest[dest_size - 1U] = '\0';
        return dest_size - 1U;
    }

    size_t available = dest_size - offset - 1U;
    if (fragment_len < available) {
        memcpy(dest + offset, fragment, fragment_len);
        offset += fragment_len;
    } else {
        memcpy(dest + offset, fragment, available);
        offset += available;
    }

    dest[offset] = '\0';
    return offset;
}

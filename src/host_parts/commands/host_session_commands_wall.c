#ifndef ANSI_REVERSE
#define ANSI_REVERSE "\033[7m"
#endif

static const char *session_wall_lookup_color_code(const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        return nullptr;
    }

    return lookup_color_code(USER_COLOR_MAP,
                             sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
                             name);
}

static void session_wall_copy_snapshot(host_t *host,
                                       ascii_pixel_t snapshot
                                           [SSH_CHATTER_WALL_HEIGHT]
                                           [SSH_CHATTER_WALL_WIDTH])
{
    if (host == nullptr || snapshot == nullptr) {
        return;
    }

    ttak_mutex_lock(&host->lock);
    memcpy(snapshot, host->wall, sizeof(host->wall));
    ttak_mutex_unlock(&host->lock);
}

static void session_wall_render_row(
    const ascii_pixel_t row[SSH_CHATTER_WALL_WIDTH], size_t cursor_x,
    bool highlight_cursor, char *buffer, size_t length)
{
    if (row == nullptr || buffer == nullptr || length == 0U) {
        return;
    }

    size_t offset = 0U;
    buffer[0] = '\0';

    for (size_t x = 0U; x < SSH_CHATTER_WALL_WIDTH; ++x) {
        const ascii_pixel_t *pixel = &row[x];
        unsigned char ch = (unsigned char)pixel->ch;
        char render = (ch >= 0x20U && ch <= 0x7eU) ? (char)ch : ' ';
        const char *color =
            (render != ' ') ? session_wall_lookup_color_code(pixel->color_name)
                            : nullptr;
        const bool is_cursor = highlight_cursor && x == cursor_x;

        int written = 0;
        if (is_cursor && color != nullptr) {
            written = snprintf(buffer + offset, length - offset, ANSI_REVERSE
                               "%s%c" ANSI_RESET, color, render);
        } else if (is_cursor) {
            written = snprintf(buffer + offset, length - offset, ANSI_REVERSE
                               "%c" ANSI_RESET, render);
        } else if (color != nullptr) {
            written = snprintf(buffer + offset, length - offset, "%s%c"
                               ANSI_RESET, color, render);
        } else {
            written = snprintf(buffer + offset, length - offset, "%c", render);
        }

        if (written < 0 || (size_t)written >= length - offset) {
            break;
        }
        offset += (size_t)written;
    }
}

static void session_wall_show(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    ascii_pixel_t snapshot[SSH_CHATTER_WALL_HEIGHT][SSH_CHATTER_WALL_WIDTH];
    session_wall_copy_snapshot(ctx->owner, snapshot);

    session_send_plain_line(ctx, "ASCII graffiti wall (80x24)");
    for (size_t y = 0U; y < SSH_CHATTER_WALL_HEIGHT; ++y) {
        char row[SSH_CHATTER_MESSAGE_LIMIT];
        session_wall_render_row(snapshot[y], 0U, false, row, sizeof(row));
        session_send_plain_line(ctx, row);
    }
}

static void session_wall_move_cursor(session_ctx_t *ctx, int dx, int dy)
{
    if (ctx == nullptr) {
        return;
    }

    int next_x = (int)ctx->wall_cursor_x + dx;
    int next_y = (int)ctx->wall_cursor_y + dy;

    if (next_x < 0) {
        next_x = 0;
    }
    if (next_x >= SSH_CHATTER_WALL_WIDTH) {
        next_x = SSH_CHATTER_WALL_WIDTH - 1;
    }
    if (next_y < 0) {
        next_y = 0;
    }
    if (next_y >= SSH_CHATTER_WALL_HEIGHT) {
        next_y = SSH_CHATTER_WALL_HEIGHT - 1;
    }

    ctx->wall_cursor_x = (uint8_t)next_x;
    ctx->wall_cursor_y = (uint8_t)next_y;
}

static int64_t session_wall_now_ns(void)
{
    struct timespec now = {0, 0};
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        now.tv_sec = time(nullptr);
        now.tv_nsec = 0L;
    }

    return ((int64_t)now.tv_sec * 1000000000LL) + (int64_t)now.tv_nsec;
}

static void session_wall_broadcast_refresh(host_t *host,
                                           const session_ctx_t *origin)
{
    if (host == nullptr) {
        return;
    }

    session_ctx_t **targets = nullptr;
    size_t target_count = 0U;

    ttak_mutex_lock(&host->room.lock);
    if (host->room.member_count > 0U) {
        targets = sshc_gc_calloc(host->room.member_count, sizeof(*targets));
        if (targets != nullptr) {
            for (size_t idx = 0U; idx < host->room.member_count; ++idx) {
                session_ctx_t *member = host->room.members[idx];
                if (member == nullptr || !member->wall_active) {
                    continue;
                }
                if (atomic_load(&member->room_snapshot_retired)) {
                    continue;
                }
                atomic_fetch_add(&member->room_snapshot_refs, 1U);
                if (atomic_load(&member->room_snapshot_retired)) {
                    atomic_fetch_sub(&member->room_snapshot_refs, 1U);
                    continue;
                }
                targets[target_count++] = member;
            }
        }
    }
    ttak_mutex_unlock(&host->room.lock);

    for (size_t idx = 0U; idx < target_count; ++idx) {
        session_ctx_t *target = targets[idx];
        if (target == nullptr) {
            continue;
        }
        session_wall_render(target, target == origin ? "Wall updated."
                                                     : "Wall updated by another user.");
        atomic_fetch_sub(&target->room_snapshot_refs, 1U);
    }
}

static void session_wall_draw_current(session_ctx_t *ctx, char ch)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if ((unsigned char)ch < 0x20U || (unsigned char)ch > 0x7eU) {
        return;
    }

    const int64_t updated_at_ns = session_wall_now_ns();

    ttak_mutex_lock(&ctx->owner->lock);
    ascii_pixel_t *pixel =
        &ctx->owner->wall[ctx->wall_cursor_y][ctx->wall_cursor_x];
    if (updated_at_ns >= pixel->updated_at_ns) {
        pixel->ch = ch;
        if (ch == ' ') {
            snprintf(pixel->color_name, sizeof(pixel->color_name), "%s",
                     "default");
        } else {
            snprintf(pixel->color_name, sizeof(pixel->color_name), "%s",
                     ctx->wall_brush_color_name);
        }
        pixel->updated_at_ns = updated_at_ns;
    }
    host_wall_state_save_locked(ctx->owner);
    ttak_mutex_unlock(&ctx->owner->lock);

    ctx->wall_brush_char = ch;
    if (ctx->wall_cursor_x + 1U < SSH_CHATTER_WALL_WIDTH) {
        ctx->wall_cursor_x += 1U;
    }

    session_wall_broadcast_refresh(ctx->owner, ctx);
}

void session_wall_render(session_ctx_t *ctx, const char *status)
{
    if (ctx == nullptr || ctx->owner == nullptr || !ctx->wall_active ||
        !session_transport_active(ctx)) {
        return;
    }

    ascii_pixel_t snapshot[SSH_CHATTER_WALL_HEIGHT][SSH_CHATTER_WALL_WIDTH];
    session_wall_copy_snapshot(ctx->owner, snapshot);

    bool locked = session_output_lock(ctx);
    session_output_buffer_start(ctx);

    session_channel_write(ctx, "\033[H\033[2J", 7U);

    char line[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(line, sizeof(line),
             "ASCII WALL 80x24  cursor=(%u,%u)  brush='%c'  color=%s",
             (unsigned int)ctx->wall_cursor_x + 1U,
             (unsigned int)ctx->wall_cursor_y + 1U,
             (ctx->wall_brush_char >= 0x20 && ctx->wall_brush_char <= 0x7e)
                 ? ctx->wall_brush_char
                 : '#',
             ctx->wall_brush_color_name[0] != '\0' ? ctx->wall_brush_color_name
                                                   : "white");
    session_channel_write(ctx, line, strlen(line));
    session_channel_write(ctx, "\r\n", 2U);

    if (ctx->wall_command_mode) {
        snprintf(line, sizeof(line),
                 "Command mode: /wall exit | color <name|xterm:N> | goto X Y");
    } else {
        snprintf(line, sizeof(line),
                 "Arrows move. Printable ASCII paints. Space erases. ':' enters command mode.");
    }
    session_channel_write(ctx, line, strlen(line));
    session_channel_write(ctx, "\r\n", 2U);

    for (size_t y = 0U; y < SSH_CHATTER_WALL_HEIGHT; ++y) {
        char row[SSH_CHATTER_MESSAGE_LIMIT];
        session_wall_render_row(snapshot[y], ctx->wall_cursor_x,
                                y == ctx->wall_cursor_y, row, sizeof(row));
        session_channel_write(ctx, row, strlen(row));
        session_channel_write(ctx, "\r\n", 2U);
    }

    if (ctx->wall_command_mode) {
        snprintf(line, sizeof(line), ":%s", ctx->input_buffer);
    } else if (status != nullptr && status[0] != '\0') {
        snprintf(line, sizeof(line), "%s", status);
    } else {
        snprintf(line, sizeof(line),
                 "Use /wall show outside the editor for a static dump.");
    }
    session_channel_write(ctx, line, strlen(line));
    session_channel_write(ctx, "\r\n", 2U);

    session_output_buffer_stop(ctx);
    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        session_channel_flush(ctx);
    }

    if (locked) {
        session_output_unlock(ctx);
    }
}

void session_wall_exit(session_ctx_t *ctx, const char *status)
{
    if (ctx == nullptr) {
        return;
    }

    ctx->wall_active = false;
    ctx->wall_command_mode = false;
    session_clear_input_without_prompt(ctx);

    if (status != nullptr && status[0] != '\0') {
        session_send_system_line(ctx, status);
    }
}

static bool session_wall_handle_command(session_ctx_t *ctx, const char *line)
{
    if (ctx == nullptr || line == nullptr) {
        return false;
    }

    char working[SSH_CHATTER_MAX_INPUT_LEN];
    snprintf(working, sizeof(working), "%s", line);
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        ctx->wall_command_mode = false;
        session_wall_render(ctx, "Returned to paint mode.");
        return true;
    }

    if (strcasecmp(working, "/wall exit") == 0 || strcasecmp(working, "exit") == 0) {
        session_wall_exit(ctx, "Exited graffiti wall.");
        return true;
    }

    if (strcasecmp(working, "/wall show") == 0 || strcasecmp(working, "show") == 0) {
        ctx->wall_command_mode = false;
        session_wall_render(ctx, "Wall refreshed.");
        return true;
    }

    if (strncasecmp(working, "color ", 6) == 0) {
        char *value = working + 6;
        trim_whitespace_inplace(value);
        if (value[0] == '\0' || session_wall_lookup_color_code(value) == nullptr) {
            session_wall_render(ctx, "Unknown color. Use ANSI names or xterm:N.");
            return true;
        }
        snprintf(ctx->wall_brush_color_name, sizeof(ctx->wall_brush_color_name),
                 "%s", value);
        ctx->wall_command_mode = false;
        session_wall_render(ctx, "Brush color updated.");
        return true;
    }

    if (strncasecmp(working, "goto ", 5) == 0) {
        unsigned int x = 0U;
        unsigned int y = 0U;
        if (sscanf(working + 5, "%u %u", &x, &y) != 2U || x == 0U || y == 0U ||
            x > SSH_CHATTER_WALL_WIDTH || y > SSH_CHATTER_WALL_HEIGHT) {
            session_wall_render(ctx, "Usage: goto X Y within 1..80 and 1..24.");
            return true;
        }
        ctx->wall_cursor_x = (uint8_t)(x - 1U);
        ctx->wall_cursor_y = (uint8_t)(y - 1U);
        ctx->wall_command_mode = false;
        session_wall_render(ctx, "Cursor moved.");
        return true;
    }

    session_wall_render(ctx, "Unknown wall command.");
    return true;
}

bool session_wall_process_line(session_ctx_t *ctx, const char *line,
                               size_t length)
{
    if (ctx == nullptr || !ctx->wall_active) {
        return false;
    }

    if (ctx->wall_command_mode) {
        return session_wall_handle_command(ctx, line != nullptr ? line : "");
    }

    if (line != nullptr && length == 1U) {
        unsigned char ch = (unsigned char)line[0];
        if (ch >= 0x20U && ch <= 0x7eU) {
            session_wall_draw_current(ctx, (char)ch);
            session_wall_render(ctx, nullptr);
            return true;
        }
    }

    session_wall_render(ctx, nullptr);
    return true;
}

bool session_wall_process_escape(session_ctx_t *ctx, const char *sequence,
                                 size_t length)
{
    if (ctx == nullptr || !ctx->wall_active || sequence == nullptr ||
        length < 2U) {
        return false;
    }

    if (ctx->wall_command_mode) {
        return true;
    }

    int dx = 0;
    int dy = 0;

    if (length == 3U &&
        ((sequence[1] == '[') || (sequence[1] == 'O'))) {
        switch (sequence[2]) {
        case 'A':
            dy = -1;
            break;
        case 'B':
            dy = 1;
            break;
        case 'C':
            dx = 1;
            break;
        case 'D':
            dx = -1;
            break;
        default:
            break;
        }
    }

    if (dx == 0 && dy == 0) {
        return true;
    }

    session_wall_move_cursor(ctx, dx, dy);
    session_wall_render(ctx, nullptr);
    return true;
}

void session_handle_wall(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    char working[SSH_CHATTER_MAX_INPUT_LEN];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
        trim_whitespace_inplace(working);
    }

    if (working[0] == '\0' || strcasecmp(working, "show") == 0) {
        session_wall_show(ctx);
        return;
    }

    if (strcasecmp(working, "enter") == 0) {
        if (ctx->wall_active) {
            session_wall_render(ctx, "Already in the wall editor.");
            return;
        }
        ctx->wall_active = true;
        ctx->wall_command_mode = false;
        session_clear_input_without_prompt(ctx);
        session_wall_render(ctx, "Entered graffiti wall.");
        return;
    }

    if (strcasecmp(working, "exit") == 0) {
        if (!ctx->wall_active) {
            session_send_system_line(ctx, "Wall editor is not active.");
            return;
        }
        session_wall_exit(ctx, "Exited graffiti wall.");
        return;
    }

    session_send_system_line(ctx, "Usage: /wall <show|enter|exit>");
}

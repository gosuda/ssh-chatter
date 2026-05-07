static size_t session_visible_history_lines(const session_ctx_t *ctx)
{
    return session_scrollback_line_capacity(ctx);
}

static void session_history_record(session_ctx_t *ctx, const char *line)
{
    if (ctx == nullptr || line == nullptr) {
        return;
    }

    bool has_visible = false;
    for (const char *cursor = line; *cursor != '\0'; ++cursor) {
        if (!isspace((unsigned char)*cursor)) {
            has_visible = true;
            break;
        }
    }

    if (!has_visible) {
        ctx->input_history_position = -1;
        return;
    }

    const char *trimmed = line;
    while (*trimmed == ' ' || *trimmed == '\t') {
        ++trimmed;
    }

    bool is_command = false;
    if (*trimmed != '\0') {
        if (*trimmed == '/') {
            is_command = true;
        } else if (ctx->input_mode == SESSION_INPUT_MODE_COMMAND) {
            is_command = true;
        }
    }

    if (ctx->input_history_count > 0U) {
        const size_t last_index = ctx->input_history_count - 1U;
        if (strncmp(ctx->input_history[last_index], line,
                    sizeof(ctx->input_history[last_index])) == 0) {
            ctx->input_history_position = -1;
            return;
        }
    }

    if (ctx->input_history_count < SSH_CHATTER_INPUT_HISTORY_LIMIT) {
        snprintf(ctx->input_history[ctx->input_history_count],
                 sizeof(ctx->input_history[0]), "%s", line);
        ctx->input_history_is_command[ctx->input_history_count] = is_command;
        ++ctx->input_history_count;
    } else {
        memmove(ctx->input_history, ctx->input_history + 1,
                sizeof(ctx->input_history) - sizeof(ctx->input_history[0]));
        memmove(ctx->input_history_is_command,
                ctx->input_history_is_command + 1,
                (SSH_CHATTER_INPUT_HISTORY_LIMIT - 1U) *
                    sizeof(ctx->input_history_is_command[0]));
        snprintf(ctx->input_history[SSH_CHATTER_INPUT_HISTORY_LIMIT - 1U],
                 sizeof(ctx->input_history[0]), "%s", line);
        ctx->input_history_is_command[SSH_CHATTER_INPUT_HISTORY_LIMIT - 1U] =
            is_command;
    }

    ctx->input_history_position = -1;
    session_scrollback_reset_position(ctx);
}

static void session_history_navigate(session_ctx_t *ctx, int direction)
{
    if (ctx == nullptr || direction == 0) {
        return;
    }

    // If scrollback is active (scrolled back), clear it before navigating command history
    // This prevents blank lines from appearing when switching from scrollback to command history
    bool was_scrolled_back = (ctx->history_scroll_position > 0U);

    session_scrollback_reset_position(ctx);

    // Clear the current line to remove any scrollback content
    if (was_scrolled_back) {
        const char clear_sequence[] = "\r" ANSI_CLEAR_LINE;
        session_channel_write(ctx, clear_sequence, sizeof(clear_sequence) - 1U);
    }

    if (ctx->input_history_count == 0U) {
        ctx->input_history_position = (int)ctx->input_history_count;
        session_set_input_text(ctx, "");
        return;
    }

    int position = ctx->input_history_position;
    if (position < 0 || position > (int)ctx->input_history_count) {
        position = (int)ctx->input_history_count;
    }

    position += direction;
    if (position < 0) {
        position = 0;
    }
    if (position > (int)ctx->input_history_count) {
        position = (int)ctx->input_history_count;
    }

    ctx->input_history_position = position;

    if (position == (int)ctx->input_history_count) {
        session_set_input_text(ctx, "");
    } else {
        session_set_input_text(ctx, ctx->input_history[position]);
    }
}

void session_scrollback_navigate(session_ctx_t *ctx, int direction,
                                 size_t step)
{
    if (ctx == nullptr || ctx->owner == nullptr ||
        !session_transport_active(ctx) || direction == 0) {
        return;
    }
    if ((direction < 0 && ctx->history_latest_notified) ||
        (direction > 0 && ctx->history_oldest_notified)) {
        return;
    }

    size_t total = host_history_total(ctx->owner);
    if (total == 0U) {
        session_send_system_line(ctx, "No chat history available yet.");
        return;
    }

    bool suppress_translation = translator_should_skip_scrollback_translation();
    bool previous_translation_suppress = ctx->translation_suppress_output;
    if (suppress_translation) {
        ctx->translation_suppress_output = true;
    }

    const bool buffering_started = !ctx->output_buffering_enabled;
    if (buffering_started) {
        session_output_buffer_start(ctx);
    }

    size_t buffer_capacity = 0U;
    chat_history_entry_t *buffer = nullptr;
    bool reached_oldest = false;

    size_t scroll_step = (step == 0) ? session_visible_history_lines(ctx) : step;
    if (scroll_step == 0U) {
        scroll_step = 1U;
    }

    size_t max_position = 0U;
    if (total > scroll_step) {
        max_position = total - scroll_step;
    }
    if (ctx->history_scroll_position > max_position) {
        ctx->history_scroll_position = max_position;
    }
    size_t position = ctx->history_scroll_position;
    size_t new_position = position;
    if (direction > 0) {
        size_t current_newest_visible = 0U;
        if (position < total) {
            current_newest_visible = total - 1U - position;
        }

        size_t current_chunk = scroll_step;
        if (current_chunk > current_newest_visible + 1U) {
            current_chunk = current_newest_visible + 1U;
        }
        if (current_chunk == 0U) {
            current_chunk = 1U;
        }

        const size_t current_oldest_visible =
            (current_newest_visible + 1U > current_chunk)
                ? (current_newest_visible + 1U - current_chunk)
                : 0U;

        if (current_oldest_visible == 0U) {
            reached_oldest = true;
        } else if (new_position < max_position) {
            size_t advance = scroll_step;
            if (advance > max_position - new_position) {
                advance = max_position - new_position;
            }
            if (advance == 0U) {
                reached_oldest = true;
            } else {
                new_position += advance;
                if (new_position == max_position) {
                    reached_oldest = true;
                }
            }
        } else {
            reached_oldest = true;
        }
    } else if (direction < 0) {
        if (new_position > 0U) {
            size_t retreat = scroll_step;
            if (retreat > new_position) {
                retreat = new_position;
            }
            new_position -= retreat;
        }
    }

    bool at_boundary = (new_position == position);
    ctx->history_scroll_position = new_position;

    bool at_latest = (ctx->history_scroll_position == 0U);
    bool at_oldest = (ctx->history_scroll_position == max_position);

    // Set no_update flag when scrolling away from latest messages
    if (!at_latest) {
        ctx->no_update = true;
        ctx->history_latest_notified = false;
        // Synchronize the display model: switch to manual scroll with a
        // stable anchor based on the oldest visible message so that new
        // incoming messages do not jump the view back to the tail.
        if (ctx->display_model_initialized) {
            const size_t nv = total - 1U - new_position;
            size_t cs = scroll_step;
            if (cs > nv + 1U) cs = nv + 1U;
            if (cs == 0U) cs = 1U;
            const size_t ov = (nv + 1U > cs) ? (nv + 1U - cs) : 0U;
            chat_history_entry_t anchor_buf;
            if (host_history_copy_range(ctx->owner, ov, &anchor_buf, 1U) == 1U &&
                anchor_buf.message_id > 0U) {
                ctx->display_model.view.mode = VIEW_MANUAL_SCROLL;
                ctx->display_model.view.anchor.message_id = anchor_buf.message_id;
                ctx->display_model.view.anchor.subline_index = 0U;
            }
        }
    } else {
        // Clear no_update flag when back at latest
        ctx->no_update = false;
        if (ctx->display_model_initialized) {
            display_model_follow_tail(&ctx->display_model);
        }
    }

    if (!at_oldest) {
        ctx->history_oldest_notified = false;
    }

    if (direction < 0 && at_boundary && new_position == 0U) {
        if (!ctx->history_latest_notified) {
            ctx->history_latest_notified = true;
        }
        session_render_prompt(ctx, false);
        session_process_pending_sink(ctx);
        ctx->scrollback_rendered_lines = 0U;
        goto cleanup;
    }

    session_scrollback_prepare_display(ctx);

    const char clear_sequence[] = "\r" ANSI_CLEAR_LINE;
    session_channel_write(ctx, clear_sequence, sizeof(clear_sequence) - 1U);

    const size_t newest_visible = total - 1U - new_position;
    size_t chunk = scroll_step;
    if (chunk > newest_visible + 1U) {
        chunk = newest_visible + 1U;
    }
    if (chunk == 0U) {
        chunk = 1U;
    }

    const size_t oldest_visible =
        (newest_visible + 1U > chunk) ? (newest_visible + 1U - chunk) : 0U;

    if (direction > 0 && reached_oldest) {
        if (!ctx->history_oldest_notified) {
            ctx->history_oldest_notified = true;
        }
    }

    char header[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(header, sizeof(header), "Scrollback (%zu-%zu of %zu)",
             oldest_visible + 1U, newest_visible + 1U, total);
    session_send_system_line(ctx, header);

    if (ctx->display_model_initialized) {
        ctx->display_model.line_count = 0U;
    }

    buffer = session_scrollback_reserve_buffer(ctx, chunk, &buffer_capacity);
    if (buffer == nullptr) {
        goto cleanup;
    }

    size_t request = chunk;
    if (request > buffer_capacity) {
        request = buffer_capacity;
    }
    size_t copied =
        host_history_copy_range(ctx->owner, oldest_visible, buffer, request);
    if (copied == 0U) {
        session_send_system_line(ctx, "Unable to read chat history right now.");
        ctx->history_scroll_position = (total > 0U) ? max_position : 0U;
        ctx->history_latest_notified = false;
        ctx->history_oldest_notified = false;
        ctx->scrollback_rendered_lines = 0U;
        session_render_prompt(ctx, false);
        goto cleanup;
    }

    for (size_t idx = 0; idx < copied; ++idx) {
        session_send_history_entry(ctx, &buffer[idx]);
    }

    if (ctx->display_model_initialized) {
        ctx->scrollback_rendered_lines = ctx->display_model.line_count + 1U;
    } else {
        ctx->scrollback_rendered_lines = copied + 1U;
    }

    if (direction < 0 && new_position == 0U) {
        if (!ctx->history_latest_notified) {
            ctx->history_latest_notified = true;
        }
    }

    session_render_prompt(ctx, false);
    session_process_pending_sink(ctx);

cleanup:
    if (buffering_started) {
        session_output_buffer_stop(ctx);
    }
    if (suppress_translation) {
        ctx->translation_suppress_output = previous_translation_suppress;
    }
}

static void session_scrollback_navigate_line(session_ctx_t *ctx, int direction)
{
    if (ctx == nullptr || ctx->owner == nullptr ||
        !session_transport_active(ctx) || direction == 0) {
        return;
    }
    if ((direction < 0 && ctx->history_latest_notified) ||
        (direction > 0 && ctx->history_oldest_notified)) {
        return;
    }

    size_t total = host_history_total(ctx->owner);
    if (total == 0U) {
        return;
    }

    bool suppress_translation = translator_should_skip_scrollback_translation();
    bool previous_translation_suppress = ctx->translation_suppress_output;
    if (suppress_translation) {
        ctx->translation_suppress_output = true;
    }

    const bool buffering_started = !ctx->output_buffering_enabled;
    if (buffering_started) {
        session_output_buffer_start(ctx);
    }

    size_t visible_lines = session_visible_history_lines(ctx);
    if (visible_lines == 0U) {
        visible_lines = 1U;
    }

    size_t max_position = 0U;
    if (total > visible_lines) {
        max_position = total - visible_lines;
    }
    if (ctx->history_scroll_position > max_position) {
        ctx->history_scroll_position = max_position;
    }
    size_t position = ctx->history_scroll_position;
    size_t new_position = position;
    size_t buffer_capacity = 0U;
    chat_history_entry_t *buffer = nullptr;

    // Scroll by exactly 1 line
    if (direction > 0) {
        // Scroll toward older messages
        if (new_position < max_position) {
            new_position += 1U;
        }
    } else if (direction < 0) {
        // Scroll toward newer messages
        if (new_position > 0U) {
            new_position -= 1U;
        }
    }

    bool at_boundary = (new_position == position);
    ctx->history_scroll_position = new_position;

    bool at_latest = (ctx->history_scroll_position == 0U);
    bool at_oldest = (ctx->history_scroll_position == max_position);

    // Set no_update flag when scrolling away from latest messages
    if (!at_latest) {
        ctx->no_update = true;
        ctx->history_latest_notified = false;
        // Keep display model in manual scroll so new messages don't jump
        // the view to tail while the user is reading back-history.
        if (ctx->display_model_initialized) {
            const size_t nv = total - 1U - new_position;
            size_t cs = visible_lines;
            if (cs > nv + 1U) cs = nv + 1U;
            if (cs == 0U) cs = 1U;
            const size_t ov = (nv + 1U > cs) ? (nv + 1U - cs) : 0U;
            chat_history_entry_t anchor_buf;
            if (host_history_copy_range(ctx->owner, ov, &anchor_buf, 1U) == 1U &&
                anchor_buf.message_id > 0U) {
                ctx->display_model.view.mode = VIEW_MANUAL_SCROLL;
                ctx->display_model.view.anchor.message_id = anchor_buf.message_id;
                ctx->display_model.view.anchor.subline_index = 0U;
            }
        }
    } else {
        // Clear no_update flag when back at latest
        ctx->no_update = false;
        if (ctx->display_model_initialized) {
            display_model_follow_tail(&ctx->display_model);
        }
    }

    if (!at_oldest) {
        ctx->history_oldest_notified = false;
    }

    if (direction < 0 && at_boundary && new_position == 0U) {
        if (!ctx->history_latest_notified) {
            ctx->history_latest_notified = true;
        }
        session_render_prompt(ctx, false);
        session_process_pending_sink(ctx);
        ctx->scrollback_rendered_lines = 0U;
        goto cleanup;
    }

    session_scrollback_prepare_display(ctx);

    const char clear_sequence[] = "\r" ANSI_CLEAR_LINE;
    session_channel_write(ctx, clear_sequence, sizeof(clear_sequence) - 1U);

    // Calculate sliding window - always show the configured message chunk
    size_t newest_visible = total - 1U - new_position;
    size_t chunk = visible_lines;
    if (chunk > newest_visible + 1U) {
        chunk = newest_visible + 1U;
    }
    if (chunk == 0U) {
        chunk = 1U;
    }

    const size_t oldest_visible =
        (newest_visible + 1U > chunk) ? (newest_visible + 1U - chunk) : 0U;

    buffer = session_scrollback_reserve_buffer(ctx, chunk, &buffer_capacity);
    if (buffer == nullptr) {
        goto cleanup;
    }

    size_t request = chunk;
    if (request > buffer_capacity) {
        request = buffer_capacity;
    }
    size_t copied =
        host_history_copy_range(ctx->owner, oldest_visible, buffer, request);
    if (copied == 0U) {
        ctx->history_scroll_position = (total > 0U) ? max_position : 0U;
        ctx->history_latest_notified = false;
        ctx->scrollback_rendered_lines = 0U;
        session_render_prompt(ctx, false);
        goto cleanup;
    }

    // Show header indicating the message range being displayed
    char header[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(header, sizeof(header), "Scrollback (%zu-%zu of %zu)",
             oldest_visible + 1U, newest_visible + 1U, total);
    session_send_system_line(ctx, header);

    if (ctx->display_model_initialized) {
        ctx->display_model.line_count = 0U;
    }

    for (size_t idx = 0; idx < copied; ++idx) {
        session_send_history_entry(ctx, &buffer[idx]);
    }

    if (ctx->display_model_initialized) {
        ctx->scrollback_rendered_lines = ctx->display_model.line_count + 1U;
    } else {
        ctx->scrollback_rendered_lines = copied + 1U;
    }

    if (direction < 0 && new_position == 0U) {
        if (!ctx->history_latest_notified) {
            ctx->history_latest_notified = true;
        }
    }
    if (direction > 0 && new_position == max_position) {
        if (!ctx->history_oldest_notified) {
            ctx->history_oldest_notified = true;
        }
    }

    session_render_prompt(ctx, false);
    session_process_pending_sink(ctx);

cleanup:
    if (buffering_started) {
        session_output_buffer_stop(ctx);
    }
    if (suppress_translation) {
        ctx->translation_suppress_output = previous_translation_suppress;
    }
}

static bool session_consume_escape_sequence(session_ctx_t *ctx, char ch)
{
    if (ctx == nullptr) {
        return false;
    }

    if (!ctx->input_escape_active) {
        if (ch == 0x1b) {
            ctx->input_escape_active = true;
            ctx->input_escape_length = 0U;
            if (ctx->input_escape_length < sizeof(ctx->input_escape_buffer)) {
                ctx->input_escape_buffer[ctx->input_escape_length++] = ch;
            }
            return true;
        }
        return false;
    }

    if (ctx->input_escape_length < sizeof(ctx->input_escape_buffer)) {
        ctx->input_escape_buffer[ctx->input_escape_length++] = ch;
    }

    const char *sequence = ctx->input_escape_buffer;
    const size_t length = ctx->input_escape_length;

    if (length == 1U) {
        return true;
    }

    if (length == 2U) {
        if (sequence[1] == '[') {
            return true;
        }
        if (sequence[1] == 'k') {
            if (ctx->bbs_post_pending) {
                session_bbs_move_cursor(ctx, -1);
            } else {
                if (ctx->bbs_view_active && session_bbs_scroll(ctx, 1, 1U)) {
                    ctx->input_escape_active = false;
                    ctx->input_escape_length = 0U;
                    return true;
                }
                session_history_navigate(ctx, -1);
            }
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
        if (sequence[1] == 'j') {
            if (ctx->bbs_post_pending) {
                session_bbs_move_cursor(ctx, 1);
            } else {
                if (ctx->bbs_view_active && session_bbs_scroll(ctx, -1, 1U)) {
                    ctx->input_escape_active = false;
                    ctx->input_escape_length = 0U;
                    return true;
                }
                session_history_navigate(ctx, 1);
            }
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
        if ((sequence[1] == 'l' || sequence[1] == 'L') && ctx->game.active &&
            ctx->game.type == SESSION_GAME_ALPHA) {
            session_game_alpha_manual_lock(ctx);
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
    }

    if (length == 3U && sequence[1] == '[') {
        if (session_wall_process_escape(ctx, sequence, length)) {
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
        int dx = 0;
        int dy = 0;
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
        if ((dx != 0 || dy != 0) &&
            session_game_alpha_handle_arrow(ctx, dx, dy)) {
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
        if (sequence[2] == 'A') {
            if (ctx->bbs_post_pending) {
                session_bbs_move_cursor(ctx, -1);
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->bbs_view_active && session_bbs_scroll(ctx, 1, 1U)) {
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->in_rss_mode && session_rss_move(ctx, -1)) {
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->input_mode == SESSION_INPUT_MODE_COMMAND) {
                session_history_navigate(ctx, -1);
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            session_scrollback_navigate_line(ctx, 1);
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
        if (sequence[2] == 'B') {
            if (ctx->bbs_post_pending) {
                session_bbs_move_cursor(ctx, 1);
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->bbs_view_active && session_bbs_scroll(ctx, -1, 1U)) {
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->in_rss_mode && session_rss_move(ctx, 1)) {
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->input_mode == SESSION_INPUT_MODE_COMMAND) {
                session_history_navigate(ctx, 1);
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            session_scrollback_navigate_line(ctx, -1);
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
    }

    if (length == 3U && sequence[1] == 'O') {
        if (session_wall_process_escape(ctx, sequence, length)) {
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
        int dx = 0;
        int dy = 0;
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
        if ((dx != 0 || dy != 0) &&
            session_game_alpha_handle_arrow(ctx, dx, dy)) {
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
        if (sequence[2] == 'A') {
            if (ctx->bbs_post_pending) {
                session_bbs_move_cursor(ctx, -1);
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->bbs_view_active && session_bbs_scroll(ctx, 1, 1U)) {
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->in_rss_mode && session_rss_move(ctx, -1)) {
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->input_mode == SESSION_INPUT_MODE_COMMAND) {
                session_history_navigate(ctx, -1);
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            session_scrollback_navigate_line(ctx, 1);
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
        if (sequence[2] == 'B') {
            if (ctx->bbs_post_pending) {
                session_bbs_move_cursor(ctx, 1);
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->bbs_view_active && session_bbs_scroll(ctx, -1, 1U)) {
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->in_rss_mode && session_rss_move(ctx, 1)) {
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->input_mode == SESSION_INPUT_MODE_COMMAND) {
                session_history_navigate(ctx, 1);
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            session_scrollback_navigate_line(ctx, -1);
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
    }

    if (length == 4U && sequence[1] == '[' && sequence[3] == '~') {
        if (sequence[2] == '5') {
            if (ctx->bbs_view_active && session_bbs_scroll(ctx, 1, 0U)) {
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            // Treat PageUp/PageDown as single-page navigation based on the
            // current viewport height instead of a fixed minimum history size.
            session_scrollback_navigate(ctx, 1, 0U);
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
        if (sequence[2] == '6') {
            if (ctx->bbs_view_active && session_bbs_scroll(ctx, -1, 0U)) {
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            // Treat PageUp/PageDown as single-page navigation based on the
            // current viewport height instead of a fixed minimum history size.
            session_scrollback_navigate(ctx, -1, 0U);
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
    }

    const bool bracket_sequence = (length >= 2U && sequence[1] == '[');
    if (bracket_sequence) {
        const char final = sequence[length - 1U];
        if (final != '~' &&
            !(length == 3U && isalpha((unsigned char)sequence[2]))) {
            return true;
        }
        if (final == '~') {
            if (length >= 5U && strncmp(&sequence[2], "200", 3) == 0) {
                ctx->bracket_paste_active = true;
            } else if (length >= 5U && strncmp(&sequence[2], "201", 3) == 0) {
                ctx->bracket_paste_active = false;
                session_refresh_input_line(ctx);
            }
        }
    }

    ctx->input_escape_active = false;
    ctx->input_escape_length = 0U;
    if (bracket_sequence) {
        return true;
    }
    return ch == 0x1b;
}

static void session_send_private_message_line(session_ctx_t *ctx,
                                              const session_ctx_t *color_source,
                                              const char *label,
                                              const char *message)
{
    if (ctx == nullptr || !session_transport_active(ctx) ||
        color_source == nullptr || label == nullptr || message == nullptr) {
        return;
    }

    const char *highlight = color_source->user_highlight_code[0] != '\0'
                                ? color_source->user_highlight_code
                                : "";
    const char *color = color_source->user_color_code[0] != '\0'
                            ? color_source->user_color_code
                            : "";
    const char *bold = color_source->user_is_bold ? ANSI_BOLD : "";

    char line[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(line, sizeof(line), "%s%s%s[%s]%s %s", highlight, bold, color,
             label, ANSI_RESET, message);
    session_send_line(ctx, line);

    if (ctx != color_source && ctx->history_scroll_position == 0U) {
        session_refresh_input_line(ctx);
    }
}

// Helper function to send a message line-by-line with cursor reset for each line
static void session_send_multiline_message(session_ctx_t *ctx,
                                           const char *message)
{
    if (ctx == nullptr || message == nullptr ||
        !session_transport_active(ctx)) {
        return;
    }

    // Make a copy of the message since we'll be modifying it
    size_t message_len = strlen(message);
    if (message_len == 0U) {
        return;
    }

    char *message_copy = sshc_gc_calloc(1U, message_len + 1U);
    if (message_copy == nullptr) {
        // If allocation fails, split inline without copying
        // This is a fallback path that still preserves the line-by-line behavior
        const char *line_start = message;
        const char *newline_pos = nullptr;

        while ((newline_pos = strchr(line_start, '\n')) != nullptr) {
            // Calculate line length
            size_t line_len = (size_t)(newline_pos - line_start);

            // Create a temporary buffer for this line (reserve 1 byte for null terminator)
            char line_buffer[SSH_CHATTER_MESSAGE_LIMIT];
            const size_t max_line_len = sizeof(line_buffer) - 1U;
            if (line_len > max_line_len) {
                line_len = max_line_len;
            }

            memcpy(line_buffer, line_start, line_len);
            line_buffer[line_len] = '\0';

            session_send_plain_line(ctx, line_buffer);

            // Move to the next line
            line_start = newline_pos + 1;
        }

        // Send any remaining text after the last newline
        if (line_start[0] != '\0') {
            session_send_plain_line(ctx, line_start);
        }
        return;
    }

    memcpy(message_copy, message, message_len);

    // Split by newlines and send each line individually
    char *line_start = message_copy;
    char *newline_pos = nullptr;

    while ((newline_pos = strchr(line_start, '\n')) != nullptr) {
        // Temporarily null-terminate at the newline
        *newline_pos = '\0';

        // Send this line with cursor reset
        session_send_plain_line(ctx, line_start);

        // Move to the next line
        line_start = newline_pos + 1;
    }

    // Send any remaining text after the last newline
    if (line_start[0] != '\0') {
        session_send_plain_line(ctx, line_start);
    }

    sshc_gc_free(message_copy);
}

static void session_render_history_entry(session_ctx_t *ctx,
                                         const chat_history_entry_t *entry,
                                         bool emit_output)
{
    if (ctx == nullptr || !session_transport_active(ctx) || entry == nullptr) {
        return;
    }

    if (chat_history_entry_is_empty(entry)) {
        return;
    }

    session_output_kind_t previous_kind =
        session_output_set_kind(ctx, SESSION_OUTPUT_KIND_CHAT);

    if (session_should_hide_entry(ctx, entry)) {
        session_output_restore_kind(ctx, previous_kind);
        return;
    }

    if (entry->is_user_message) {
        char formatted[SSH_CHATTER_MESSAGE_LIMIT * 2U];
        formatted[0] = '\0';

        const char *highlight = (entry->user_highlight_code[0] != '\0')
                                    ? entry->user_highlight_code
                                    : "";
        const char *color =
            (entry->user_color_code[0] != '\0') ? entry->user_color_code : "";
        const char *bold = entry->user_is_bold ? ANSI_BOLD : "";

        const bool has_custom_codes =
            (color[0] != '\0') || (highlight[0] != '\0');

        char name_block[SSH_CHATTER_MESSAGE_LIMIT];
        const char *id_display = "-";
        char id_label[32];
        if (entry->message_id > 0U &&
            host_compact_id_encode(entry->message_id, id_label,
                                   sizeof(id_label))) {
            id_display = id_label;
        }
        const char *display_name = chat_history_entry_display_name(entry);
        if (has_custom_codes) {
            snprintf(name_block, sizeof(name_block), ANSI_CYAN "[%s]" ANSI_RESET " <%s%s%s%s%s>",
                     id_display, highlight, color, bold, display_name,
                     ANSI_RESET);
        } else {
            snprintf(name_block, sizeof(name_block), "%s%s%s " ANSI_CYAN "[%s]" ANSI_RESET " <%s>%s",
                     highlight, bold, color, id_display, display_name,
                     ANSI_RESET);
        }
        strncat(formatted, name_block,
                sizeof(formatted) - strlen(formatted) - 1U);

        if (entry->message[0] != '\0') {
            const bool multiline = strchr(entry->message, '\n') != nullptr;
            if (multiline) {
                // For multiline messages, send the username first, then each line separately
                strncat(formatted, " ",
                        sizeof(formatted) - strlen(formatted) - 1U);
                if (ctx->display_model_initialized) {
                    unsigned int width = (ctx->terminal_width > 0U) ? ctx->terminal_width : 80U;
                    display_model_append_message(&ctx->display_model, entry->message_id, formatted, width);
                    display_model_append_message(&ctx->display_model, entry->message_id, entry->message, width);
                }
                if (emit_output) {
                    session_send_plain_line(ctx, formatted);
                    session_send_multiline_message(ctx, entry->message);
                }
            } else {
                // For single-line messages, send as before
                strncat(formatted, " ",
                        sizeof(formatted) - strlen(formatted) - 1U);
                strncat(formatted, entry->message,
                        sizeof(formatted) - strlen(formatted) - 1U);
                if (ctx->display_model_initialized) {
                    unsigned int width = (ctx->terminal_width > 0U) ? ctx->terminal_width : 80U;
                    display_model_append_message(&ctx->display_model, entry->message_id, formatted, width);
                }
                if (emit_output) {
                    session_send_plain_line(ctx, formatted);
                }
            }
        } else {
            if (ctx->display_model_initialized) {
                unsigned int width = (ctx->terminal_width > 0U) ? ctx->terminal_width : 80U;
                display_model_append_message(&ctx->display_model, entry->message_id, formatted, width);
            }
            if (emit_output) {
                session_send_plain_line(ctx, formatted);
            }
        }

        // Display attachment URL if present, similar to reply format
        if (emit_output && entry->attachment_type != CHAT_ATTACHMENT_NONE &&
            entry->attachment_target[0] != '\0') {
            const char *label =
                chat_attachment_type_label(entry->attachment_type);
            char attachment_line[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(attachment_line, sizeof(attachment_line),
                     "    (%s)" ANSI_RESET " %s", label,
                     entry->attachment_target);
            session_send_plain_line(ctx, attachment_line);

            if (entry->attachment_caption[0] != '\0') {
                char caption_line[SSH_CHATTER_MESSAGE_LIMIT];
                snprintf(caption_line, sizeof(caption_line),
                         "    -> %s", entry->attachment_caption);
                session_send_plain_line(ctx, caption_line);
            }
        }

        char reaction_summary[SSH_CHATTER_MESSAGE_LIMIT];
        if (chat_history_entry_build_reaction_summary(entry, reaction_summary,
                                                      sizeof(reaction_summary))) {
            char reaction_line[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(reaction_line, sizeof(reaction_line), "    - %s",
                     reaction_summary);
            if (ctx->display_model_initialized) {
                unsigned int width =
                    (ctx->terminal_width > 0U) ? ctx->terminal_width : 80U;
                display_model_append_message(&ctx->display_model,
                                             entry->message_id, reaction_line,
                                             width);
            }
            if (emit_output) {
                session_send_plain_line(ctx, reaction_line);
            }
        }

        session_output_restore_kind(ctx, previous_kind);
        return;
    }

    // For non-user messages, check if multiline and send accordingly
    const bool multiline = strchr(entry->message, '\n') != nullptr;
    if (multiline) {
        if (ctx->display_model_initialized) {
            unsigned int width = (ctx->terminal_width > 0U) ? ctx->terminal_width : 80U;
            display_model_append_message(&ctx->display_model, entry->message_id, entry->message, width);
        }
        if (emit_output) {
            session_send_multiline_message(ctx, entry->message);
        }
    } else {
        if (ctx->display_model_initialized) {
            unsigned int width = (ctx->terminal_width > 0U) ? ctx->terminal_width : 80U;
            display_model_append_message(&ctx->display_model, entry->message_id, entry->message, width);
        }
        if (emit_output) {
            session_send_plain_line(ctx, entry->message);
        }
    }

    session_output_restore_kind(ctx, previous_kind);
}

static void session_send_history_entry(session_ctx_t *ctx,
                                       const chat_history_entry_t *entry)
{
    session_render_history_entry(ctx, entry, true);
}

// Present a summary of a poll, optionally showing the label used for named polls.
static void session_send_poll_summary_generic(session_ctx_t *ctx,
                                              const poll_state_t *poll,
                                              const char *label)
{
    if (ctx == nullptr || poll == nullptr) {
        return;
    }

    if (!poll->active || poll->option_count == 0U) {
        if (label == nullptr) {
            session_send_system_line(ctx, "No active poll right now.");
        } else {
            char message[128];
            snprintf(message, sizeof(message), "Poll '%s' is not active.",
                     label);
            session_send_system_line(ctx, message);
        }
        return;
    }

    char header[SSH_CHATTER_MESSAGE_LIMIT];
    enum {
        SESSION_POLL_HEADER_QUESTION_PREC = SSH_CHATTER_MESSAGE_LIMIT / 2,
        SESSION_POLL_HEADER_LABEL_PREC = SSH_CHATTER_POLL_LABEL_LEN - 1,
        SESSION_POLL_OPTION_TEXT_PREC = SSH_CHATTER_MESSAGE_LIMIT / 2
    };
    const char *mode_suffix = poll->allow_multiple ? " (multiple choice)" : "";
    if (label == nullptr) {
        snprintf(header, sizeof(header), "Poll #%" PRIu64 ": %.*s%s", poll->id,
                 SESSION_POLL_HEADER_QUESTION_PREC, poll->question,
                 mode_suffix);
    } else {
        snprintf(header, sizeof(header), "Poll [%.*s] #%" PRIu64 ": %.*s%s",
                 SESSION_POLL_HEADER_LABEL_PREC, label, poll->id,
                 SESSION_POLL_HEADER_QUESTION_PREC, poll->question,
                 mode_suffix);
    }
    session_send_system_line(ctx, header);

    for (size_t idx = 0U; idx < poll->option_count; ++idx) {
        char option_line[SSH_CHATTER_MESSAGE_LIMIT];
        uint32_t votes = poll->options[idx].votes;
        if (label == nullptr) {
            snprintf(option_line, sizeof(option_line),
                     "  /%zu - %.*s (%u vote%s)", idx + 1U,
                     SESSION_POLL_OPTION_TEXT_PREC, poll->options[idx].text,
                     votes, votes == 1U ? "" : "s");
        } else {
            snprintf(option_line, sizeof(option_line),
                     "  /%zu %.*s - %.*s (%u vote%s)", idx + 1U,
                     SESSION_POLL_HEADER_LABEL_PREC, label,
                     SESSION_POLL_OPTION_TEXT_PREC, poll->options[idx].text,
                     votes, votes == 1U ? "" : "s");
        }
        session_send_system_line(ctx, option_line);
    }

    if (label == nullptr) {
        if (poll->allow_multiple) {
            session_send_system_line(
                ctx, "Vote with /1 through /5 (multiple selections allowed).");
        } else {
            session_send_system_line(ctx, "Vote with /1 through /5.");
        }
    } else {
        char footer[192];
        if (poll->allow_multiple) {
            snprintf(footer, sizeof(footer),
                     "Vote with /1 %s through /%zu %s (multiple selections "
                     "allowed).",
                     label, poll->option_count, label);
        } else {
            snprintf(footer, sizeof(footer), "Vote with /1 %s through /%zu %s.",
                     label, poll->option_count, label);
        }
        session_send_system_line(ctx, footer);
    }
}

// Gather the main poll and any named polls and present summaries to the caller.
static __attribute__((unused)) void
session_send_poll_summary(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    host_t *host = ctx->owner;
    poll_state_t main_snapshot = {0};
    named_poll_state_t named_snapshot[SSH_CHATTER_MAX_NAMED_POLLS];
    size_t named_count = 0U;

    ttak_mutex_lock(&host->lock);
    main_snapshot = host->poll;
    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_NAMED_POLLS; ++idx) {
        if (host->named_polls[idx].label[0] == '\0') {
            continue;
        }
        named_snapshot[named_count++] = host->named_polls[idx];
        if (named_count >= SSH_CHATTER_MAX_NAMED_POLLS) {
            break;
        }
    }
    ttak_mutex_unlock(&host->lock);

    session_send_poll_summary_generic(ctx, &main_snapshot, nullptr);

    size_t active_named = 0U;
    for (size_t idx = 0U; idx < named_count; ++idx) {
        if (named_snapshot[idx].poll.active &&
            named_snapshot[idx].poll.option_count > 0U) {
            if (active_named == 0U) {
                session_send_system_line(ctx, "Active named polls:");
            }
            session_send_poll_summary_generic(ctx, &named_snapshot[idx].poll,
                                              named_snapshot[idx].label);
            ++active_named;
        }
    }

    if (active_named == 0U) {
        session_send_system_line(
            ctx, "No active named polls. Use /vote <label> "
                 "<question>|<option1>|<option2> or /vote-single for a "
                 "single-choice poll.");
    }
}

// Provide a lightweight overview of every named poll regardless of status.
static __attribute__((unused)) void session_list_named_polls(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    host_t *host = ctx->owner;
    named_poll_state_t snapshot[SSH_CHATTER_MAX_NAMED_POLLS];
    size_t count = 0U;

    ttak_mutex_lock(&host->lock);
    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_NAMED_POLLS; ++idx) {
        if (host->named_polls[idx].label[0] == '\0') {
            continue;
        }
        snapshot[count++] = host->named_polls[idx];
        if (count >= SSH_CHATTER_MAX_NAMED_POLLS) {
            break;
        }
    }
    ttak_mutex_unlock(&host->lock);

    if (count == 0U) {
        session_send_system_line(
            ctx, "No named polls exist. Start one with /vote <label> "
                 "<question>|<option1>|<option2> or /vote-single "
                 "for single-choice voting.");
        return;
    }

    session_send_system_line(ctx, "Named polls overview:");
    for (size_t idx = 0U; idx < count; ++idx) {
        const named_poll_state_t *entry = &snapshot[idx];
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        const char *status = entry->poll.active ? "active" : "inactive";
        const char *mode =
            entry->poll.allow_multiple ? "multiple choice" : "single choice";
        snprintf(line, sizeof(line), "- [%s] %s (options: %zu, %s, %s)",
                 entry->label, entry->poll.question, entry->poll.option_count,
                 status, mode);
        session_send_system_line(ctx, line);
    }
}

static bool
chat_history_entry_build_reaction_summary(const chat_history_entry_t *entry,
                                          char *buffer, size_t length)
{
    if (entry == nullptr || buffer == nullptr || length == 0U) {
        return false;
    }

    buffer[0] = '\0';
    bool any = false;
    size_t offset = 0U;

    for (size_t idx = 0U; idx < SSH_CHATTER_REACTION_KIND_COUNT; ++idx) {
        uint32_t count = entry->reaction_counts[idx];
        if (count == 0U) {
            continue;
        }

        const reaction_descriptor_t *descriptor = &REACTION_DEFINITIONS[idx];
        char chunk[64];
        snprintf(chunk, sizeof(chunk), "[%s: %u]", descriptor->label, count);

        size_t chunk_len = strlen(chunk);
        if (chunk_len + 1U >= length - offset) {
            break;
        }

        if (any) {
            buffer[offset++] = ' ';
        }
        memcpy(buffer + offset, chunk, chunk_len);
        offset += chunk_len;
        buffer[offset] = '\0';
        any = true;
    }

    return any;
}

static const char *chat_attachment_type_label(chat_attachment_type_t type)
{
    switch (type) {
    case CHAT_ATTACHMENT_IMAGE:
        return "image";
    case CHAT_ATTACHMENT_VIDEO:
        return "video";
    case CHAT_ATTACHMENT_AUDIO:
        return "audio";
    case CHAT_ATTACHMENT_FILE:
        return "file";
    case CHAT_ATTACHMENT_NONE:
    default:
        return "attachment";
    }
}

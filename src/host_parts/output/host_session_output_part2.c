static inline bool session_is_slash_compatible_char(char ch)
{
    switch ((int)ch) {
    case (int)'/':
    case (int)'.':
    case (int)'_':
    case (int)'@':
    case (int)'$':
    case (int)'*':
    case (int)'-':
    case (int)'#':
    case (int)'>':
        return true;
    default:
        return false;
    }
}

static bool session_try_command_completion(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }

    if (ctx->input_length == 0U) {
        return false;
    }

    size_t first_visible = 0U;
    while (first_visible < ctx->input_length &&
           isspace((unsigned char)ctx->input_buffer[first_visible])) {
        ++first_visible;
    }
    if (first_visible >= ctx->input_length) {
        return false;
    }

    // SLASH_COMPATIBLE: Check for slash or slash-compatible characters
    const bool has_slash_compatible =
        session_is_slash_compatible_char(ctx->input_buffer[first_visible]);
    if (!has_slash_compatible &&
        ctx->input_mode != SESSION_INPUT_MODE_COMMAND) {
        return false;
    }

    size_t command_start = first_visible + (has_slash_compatible ? 1U : 0U);
    if (command_start > ctx->input_length) {
        command_start = ctx->input_length;
    }

    size_t command_end = command_start;
    while (command_end < ctx->input_length &&
           !isspace((unsigned char)ctx->input_buffer[command_end])) {
        ++command_end;
    }

    const size_t token_len = command_end - command_start;
    char prefix[SSH_CHATTER_MAX_INPUT_LEN];
    size_t copy_len =
        token_len < sizeof(prefix) - 1U ? token_len : sizeof(prefix) - 1U;
    if (copy_len > 0U) {
        memcpy(prefix, &ctx->input_buffer[command_start], copy_len);
    }
    prefix[copy_len] = '\0';

    const size_t prefix_len = strlen(prefix);
    const char *matches[SSH_CHATTER_COMMAND_COUNT];
    size_t match_count = 0U;
    for (size_t idx = 0U; idx < SSH_CHATTER_COMMAND_COUNT; ++idx) {
        const char *candidate = kSessionCommandNames[idx];
        if (prefix_len == 0U ||
            strncasecmp(candidate, prefix, prefix_len) == 0) {
            matches[match_count++] = candidate;
        }
    }

    session_command_collect_localized_matches(
        ctx, prefix, matches, &match_count,
        sizeof(matches) / sizeof(matches[0]));

    if (match_count == 0U) {
        if (session_transport_active(ctx)) {
            const char bell = '\a';
            session_channel_write(ctx, &bell, 1U);
        }
        session_refresh_input_line(ctx);
        return true;
    }

    char updated[SSH_CHATTER_MAX_INPUT_LEN];
    size_t updated_len = 0U;
    const size_t prefix_copy_len =
        command_start < sizeof(updated) ? command_start : sizeof(updated) - 1U;
    if (prefix_copy_len > 0U) {
        memcpy(updated, ctx->input_buffer, prefix_copy_len);
        updated_len = prefix_copy_len;
    }

    if (match_count == 1U) {
        const char *completion = matches[0];
        size_t completion_len = strlen(completion);
        if (updated_len + completion_len >= sizeof(updated)) {
            completion_len = sizeof(updated) - 1U - updated_len;
        }
        memcpy(&updated[updated_len], completion, completion_len);
        updated_len += completion_len;

        size_t suffix_len = ctx->input_length - command_end;
        if (suffix_len > 0U) {
            size_t copy_suffix = suffix_len;
            if (updated_len + copy_suffix >= sizeof(updated)) {
                copy_suffix = sizeof(updated) - 1U - updated_len;
            }
            memcpy(&updated[updated_len], &ctx->input_buffer[command_end],
                   copy_suffix);
            updated_len += copy_suffix;
        } else if (updated_len + 1U < sizeof(updated)) {
            updated[updated_len++] = ' ';
        }

        updated[updated_len] = '\0';
        session_set_input_text(ctx, updated);
        ctx->input_history_position = -1;
        session_scrollback_reset_position(ctx);
        return true;
    }

    size_t common_len = strlen(matches[0]);
    for (size_t idx = 1U; idx < match_count && common_len > 0U; ++idx) {
        const char *candidate = matches[idx];
        size_t candidate_len = strlen(candidate);
        if (candidate_len < common_len) {
            common_len = candidate_len;
        }
        size_t compare_len = common_len;
        size_t match_prefix = 0U;
        for (; match_prefix < compare_len; ++match_prefix) {
            unsigned char lhs =
                (unsigned char)tolower((unsigned char)matches[0][match_prefix]);
            unsigned char rhs =
                (unsigned char)tolower((unsigned char)candidate[match_prefix]);
            if (lhs != rhs) {
                break;
            }
        }
        common_len = match_prefix;
    }

    if (common_len > prefix_len) {
        size_t completion_len = common_len;
        if (updated_len + completion_len >= sizeof(updated)) {
            completion_len = sizeof(updated) - 1U - updated_len;
        }
        memcpy(&updated[updated_len], matches[0], completion_len);
        updated_len += completion_len;

        size_t suffix_len = ctx->input_length - command_end;
        if (suffix_len > 0U) {
            size_t copy_suffix = suffix_len;
            if (updated_len + copy_suffix >= sizeof(updated)) {
                copy_suffix = sizeof(updated) - 1U - updated_len;
            }
            memcpy(&updated[updated_len], &ctx->input_buffer[command_end],
                   copy_suffix);
            updated_len += copy_suffix;
        }

        updated[updated_len] = '\0';
        session_set_input_text(ctx, updated);
        ctx->input_history_position = -1;
        session_scrollback_reset_position(ctx);
        return true;
    }

    session_send_system_line(ctx, "Possible commands:");
    char line[SSH_CHATTER_MESSAGE_LIMIT];
    size_t offset = 0U;
    for (size_t idx = 0U; idx < match_count; ++idx) {
        char entry[64];
        snprintf(entry, sizeof(entry), "/%s", matches[idx]);
        size_t entry_len = strlen(entry);
        if (offset != 0U) {
            if (offset + 1U >= sizeof(line)) {
                line[offset] = '\0';
                session_send_system_line(ctx, line);
                offset = 0U;
            }
            line[offset++] = ' ';
        }
        if (entry_len >= sizeof(line)) {
            session_send_system_line(ctx, entry);
            offset = 0U;
            continue;
        }
        if (offset + entry_len >= sizeof(line)) {
            line[offset] = '\0';
            session_send_system_line(ctx, line);
            offset = 0U;
        }
        memcpy(&line[offset], entry, entry_len);
        offset += entry_len;
    }
    if (offset > 0U) {
        line[offset] = '\0';
        session_send_system_line(ctx, line);
    }
    session_refresh_input_line(ctx);
    return true;
}

/**
 * @desc Reset a session's scrollback to the latest message and clear any
 *       scrollback flags so real-time output resumes.
 * @param ctx Session context to reset.
 * @return None.
 */
void session_scrollback_reset_position(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    // Reset scrollback counters so the latest history is shown.
    ctx->history_scroll_position = 0U;
    ctx->history_latest_notified = false;
    ctx->history_oldest_notified = false;
    ctx->scrollback_rendered_lines = 0U;
    // Clear no_update flag when returning to latest messages
    ctx->no_update = false;

    // Return display model to tail-follow mode
    if (ctx->display_model_initialized) {
        display_model_follow_tail(&ctx->display_model);
    }

    // If a sink flag is pending, synchronize the latest chat chunk now
    session_process_pending_sink(ctx);
}

/**
 * @desc Synchronize pending chat history to a session that is not scrolled
 *       back, emitting the most recent chunk without forcing a redraw when
 *       the session is paused in scrollback.
 * @param ctx Session context to process pending sink state for.
 * @return None.
 */
void session_process_pending_sink(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr || !ctx->pending_should_sink) {
        return;
    }

    if (ctx->history_scroll_position > 0U) {
        // Do not force-render history when the user is scrolled back.
        return;
    }

    display_line_t previous_visible[SSH_CHATTER_SCROLLBACK_MAX_CHUNK];
    session_screen_line_t previous_lines[SSH_CHATTER_SCROLLBACK_MAX_CHUNK];
    size_t previous_count = 0U;
    const unsigned int viewport_height =
        (unsigned int)session_scrollback_line_capacity(ctx);
    const bool try_incremental =
        ctx->display_model_initialized &&
        display_model_is_following_tail(&ctx->display_model);

    if (try_incremental) {
        previous_count = session_capture_visible_display_lines(
            &ctx->display_model, viewport_height, previous_visible,
            SSH_CHATTER_SCROLLBACK_MAX_CHUNK);
        previous_count = session_describe_display_lines(
            previous_visible, previous_count, previous_lines,
            SSH_CHATTER_SCROLLBACK_MAX_CHUNK);
        ctx->display_model.line_count = 0U;
        ctx->display_model.dirty = true;
    }

    // Compute the newest chunk to deliver.
    size_t total = host_history_total(ctx->owner);
    if (total == 0U) {
        ctx->pending_should_sink = false;
        return;
    }

    size_t chunk = session_scrollback_line_capacity(ctx);
    if (chunk > total) {
        chunk = total;
    }

    size_t start_index = (total > 0U && total > chunk) ? (total - chunk) : 0U;
    size_t buffer_capacity = 0U;
    chat_history_entry_t *buffer =
        session_scrollback_reserve_buffer(ctx, chunk, &buffer_capacity);
    if (buffer == nullptr || buffer_capacity < chunk) {
        return;
    }
    size_t copied =
        host_history_copy_range(ctx->owner, start_index, buffer, chunk);
    if (copied == 0U) {
        return;
    }

    ctx->pending_should_sink = false;

    const bool buffering_started = !ctx->output_buffering_enabled;
    if (buffering_started) {
        session_output_buffer_start(ctx);
    }

    const size_t buffer_mark = ctx->output_buffer_length;
    ctx->output_lines_since_prompt = 0U;
    ctx->prompt_needs_padding = false;

    // Disable the incremental realtime capture during the full-frame
    // redraw to prevent session_realtime_refresh from firing mid-render
    // and clearing the screen.
    const bool prev_capture = ctx->capture_realtime_output;
    ctx->capture_realtime_output = false;

    for (size_t idx = 0; idx < copied; ++idx) {
        // Rebuild the visible model without emitting output first so an
        // incremental patch can replace the previous frame atomically.
        session_render_history_entry(ctx, &buffer[idx], false);
    }

    bool used_incremental_redraw = false;
    if (try_incremental && previous_count > 0U) {
        display_visible_frame_t current_frame;
        session_screen_line_t current_lines[SSH_CHATTER_SCROLLBACK_MAX_CHUNK];
        display_model_compute_visible(&ctx->display_model, viewport_height,
                                      &current_frame);
        size_t current_count = session_describe_visible_frame(
            &current_frame, current_lines, SSH_CHATTER_SCROLLBACK_MAX_CHUNK);
        if (current_count > 0U) {
            ctx->output_buffer_length = buffer_mark;
            used_incremental_redraw = session_render_incremental_lines(
                ctx, previous_lines, previous_count, current_lines, current_count,
                true);
        }
    }

    if (!used_incremental_redraw) {
        /*
         * Avoid full-screen clear fallback for both SSH and TELNET.
         * When incremental diffing is unavailable, append only the newest
         * history line so sink updates do not trigger full-frame flicker.
         */
        ctx->output_buffer_length = buffer_mark;
        session_send_history_entry(ctx, &buffer[copied - 1U]);
    }

    if (ctx->display_model_initialized) {
        ctx->scrollback_rendered_lines = ctx->display_model.line_count;
    } else {
        ctx->scrollback_rendered_lines = copied;
    }

    ctx->capture_realtime_output = prev_capture;
    // Reset the realtime line counter so the next batch of live messages
    // does not immediately trigger session_realtime_refresh.
    ctx->realtime_line_count = 0U;

    if (buffering_started) {
        session_output_buffer_stop(ctx);
    }

    // Ensure the input line is visible after the sink update.
    session_refresh_input_line(ctx);
}

/**
 * @desc Mark a session as needing to sink the latest chat chunk and
 *       immediately attempt to deliver it when the session is at the
 *       newest scrollback position.
 * @param ctx Session context to mark and process.
 * @return None.
 */
void session_flag_should_sink(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    ctx->pending_should_sink = true;
    session_process_pending_sink(ctx);
}

/**
 * @desc Mark a session as needing to sink the latest chat chunk without
 *       immediately rendering it (defer processing until the session is
 *       at the newest position).
 * @param ctx Session context to mark as pending.
 * @return None.
 */
void session_mark_should_sink(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    ctx->pending_should_sink = true;
}

/**
 * @desc Clear any pending sink state for a session that already received
 *       real-time output, avoiding redundant history rendering later.
 * @param ctx Session context to clear.
 * @return None.
 */
void session_clear_pending_sink(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    ctx->pending_should_sink = false;
}

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
        max_position = total - step;
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

    if (direction > 0 && at_boundary && new_position == max_position) {
        if (!ctx->history_oldest_notified) {
            ctx->history_oldest_notified = true;
        }
        session_render_prompt(ctx, false);
        session_process_pending_sink(ctx);
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

    if (direction > 0 && at_boundary && new_position == max_position) {
        if (!ctx->history_oldest_notified) {
            ctx->history_oldest_notified = true;
        }
        session_render_prompt(ctx, false);
        session_process_pending_sink(ctx);
        goto cleanup;
    }

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

    if (direction > 0 && at_boundary && new_position == max_position) {
        if (!ctx->history_oldest_notified) {
            ctx->history_oldest_notified = true;
        }
        // No message for oldest by default, just block
        session_render_prompt(ctx, false);
        session_process_pending_sink(ctx);
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
            session_scrollback_navigate(ctx, 1, 100);
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
            session_scrollback_navigate(ctx, -1, 100);
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
        snprintf(chunk, sizeof(chunk), "%s x%u", descriptor->icon, count);

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

static bool session_handle_service_request(ssh_message message)
{
    if (message == nullptr) {
        return false;
    }

    const char *service = ssh_message_service_service(message);
    if (service == nullptr) {
        return false;
    }

    if (strcmp(service, "ssh-userauth") == 0 ||
        strcmp(service, "ssh-connection") == 0) {
        ssh_message_service_reply_success(message);
        return true;
    }

    return false;
}

bool is_nullarray(uint8_t *arr, size_t len)
{
    uint8_t zeros[len];
    memset(zeros, 0, len);
    return memcmp(arr, zeros, len) == 0;
}

static int session_authenticate(session_ctx_t *ctx)
{
    ssh_message message = nullptr;
    bool authenticated = false;
    if (ctx != nullptr) {
        ctx->lan_operator_credentials_valid = false;
    }

    // Declare credential here to ensure it's in scope for all uses
    lan_operator_credential_t *credential = nullptr;

    while (!authenticated &&
           (message = ssh_message_get(ctx->session)) != nullptr) {
        const int message_type = ssh_message_type(message);
        switch (message_type) {
        case SSH_REQUEST_SERVICE:
            if (!session_handle_service_request(message)) {
                ssh_message_reply_default(message);
            }
            break;
        case SSH_REQUEST_AUTH: {
            const char *username = ssh_message_auth_user(message);
            if (username != nullptr && username[0] != '\0') {
                char cleaned_username[SSH_CHATTER_USERNAME_LEN];
                if (!user_data_strip_ansi_sequences(username, cleaned_username,
                                                    sizeof(cleaned_username)) ||
                    cleaned_username[0] == '\0') {
                    snprintf(cleaned_username, sizeof(cleaned_username), "%.*s",
                             SSH_CHATTER_USERNAME_LEN - 1, username);
                }

                snprintf(ctx->user.name, sizeof(ctx->user.name), "%s",
                         cleaned_username);
            }

            // Load user data
            bool loaded_by_username = false;
            ttak_mutex_lock(&ctx->owner->user_data_lock);
            if (user_data_load(ctx->owner->user_data_root, ctx->user.name, NULL,
                               &ctx->user_data)) {
                if (!security_layer_is_zero_hash(
                        ctx->user_data.password_hash,
                        sizeof(ctx->user_data.password_hash))) {
                    loaded_by_username = true;
                }
            }

            if (!loaded_by_username) {
                user_data_ensure_exists(ctx->owner->user_data_root,
                                        ctx->user.name, ctx->client_ip,
                                        &ctx->user_data);
            }
            ttak_mutex_unlock(&ctx->owner->user_data_lock);

            // Check if a password is set for this user
            bool password_is_set = !security_layer_is_zero_hash(
                ctx->user_data.password_hash,
                sizeof(ctx->user_data.password_hash));

            // Handle LAN operator authentication
            bool reserved_name = false;
            // credential variable is already declared at the beginning of the function
            if (ctx->owner != nullptr) {
                credential = host_find_lan_operator_credential(ctx->owner,
                                                               ctx->user.name);
                reserved_name = credential != nullptr;
            }

            if (reserved_name) {
                if (!session_is_lan_client(ctx->client_ip)) {
                    ssh_message_auth_set_methods(message,
                                                 SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(message);
                    break; // Break from switch, continue while loop
                }

                const int auth_method = ssh_message_subtype(message);
                if (auth_method != SSH_AUTH_METHOD_PASSWORD) {
                    ssh_message_auth_set_methods(message,
                                                 SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(message);
                    break; // Break from switch, continue while loop
                }

                const char *password = ssh_message_auth_password(message);
                if (credential == nullptr || password == nullptr ||
                    credential->password[0] == '\0' ||
                    strcmp(credential->password, password) != 0) {
                    ssh_message_auth_set_methods(message,
                                                 SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(message);
                    break; // Break from switch, continue while loop
                }
                ctx->lan_operator_credentials_valid = true;
                snprintf(ctx->user.name, sizeof(ctx->user.name), "%s",
                         credential->nickname);
                ssh_message_auth_reply_success(message, 0);
                authenticated = true; // This will break the while loop
                break;                // Break from switch
            }

            // Regular user authentication
            if (!password_is_set) {
                // No password set, allow login but flag for password creation
                ctx->password_not_set = true;
                ssh_message_auth_reply_success(message, 0);
                authenticated = true; // This will break the while loop
                break;                // Break from switch
            } else {
                // Password is set, require password authentication
                const int auth_method = ssh_message_subtype(message);
                if (auth_method != SSH_AUTH_METHOD_PASSWORD) {
                    ssh_message_auth_set_methods(message,
                                                 SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(message);
                    break; // Break from switch, continue while loop
                }

                const char *password = ssh_message_auth_password(message);
                if (password == nullptr) {
                    ssh_message_auth_set_methods(message,
                                                 SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(message);
                    break; // Break from switch, continue while loop
                }

                uint8_t provided_password_hash[32];
                security_layer_hash_password(password,
                                             ctx->user_data.password_salt,
                                             provided_password_hash);

                if (memcmp(provided_password_hash, ctx->user_data.password_hash,
                           sizeof(provided_password_hash)) == 0) {
                    ssh_message_auth_reply_success(message, 0);
                    authenticated = true; // This will break the while loop
                    break;                // Break from switch
                } else {
                    ssh_message_auth_set_methods(message,
                                                 SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(message);
                    break; // Break from switch, continue while loop
                }
            }
            break;
        }
        case SSH_CHANNEL_REQUEST_WINDOW_CHANGE:
            ssh_message_channel_request_reply_success(message);
            break;
        case SSH_CHANNEL_REQUEST_SHELL:
            ssh_message_channel_request_reply_success(message);
            break;
        default:
            ssh_message_reply_default(message);
            break;
        }
        ssh_message_free(message);
    }

    return authenticated ? 0 : -1;
}

static int session_accept_channel(session_ctx_t *ctx)
{
    ssh_message message = nullptr;

    while ((message = ssh_message_get(ctx->session)) != nullptr) {
        const int message_type = ssh_message_type(message);
        if (message_type == SSH_REQUEST_SERVICE) {
            if (!session_handle_service_request(message)) {
                ssh_message_reply_default(message);
            }
            ssh_message_free(message);
            continue;
        }

        if (message_type == SSH_REQUEST_CHANNEL_OPEN &&
            ssh_message_subtype(message) == SSH_CHANNEL_SESSION) {
            ssh_channel channel =
                ssh_message_channel_request_open_reply_accept(message);
            if (channel == nullptr) {
                accept_channel_fn_t accept_channel =
                    resolve_accept_channel_fn();
                if (accept_channel != nullptr) {
                    channel = ssh_channel_new(ctx->session);
                    if (channel != nullptr) {
                        if (accept_channel(message, channel) != SSH_OK) {
                            ssh_channel_free(channel);
                            channel = nullptr;
                        }
                    }
                }
            }

            if (channel != nullptr) {
                ctx->channel = channel;
                ssh_message_free(message);
                break;
            }

            ssh_message_reply_default(message);
            ssh_message_free(message);
            continue;
        }

        ssh_message_reply_default(message);
        ssh_message_free(message);
    }

    return session_transport_active(ctx) ? 0 : -1;
}

static int session_on_window_change(ssh_session session, ssh_channel channel,
                                    int width, int height, int pxwidth,
                                    int pwheight, void *userdata)
{
    (void)session;
    (void)channel;
    (void)pxwidth;
    (void)pwheight;

    session_runtime_data_t *runtime = (session_runtime_data_t *)userdata;
    if (runtime == nullptr || !atomic_load(&runtime->active)) {
        return -1;
    }

    session_ctx_t *ctx = runtime->ctx;
    if (ctx == nullptr) {
        return -1;
    }

    if (width > 0 && width <= SSH_CHATTER_MESSAGE_LIMIT) {
        ctx->terminal_width = (unsigned int)width;
    }
    if (height > 0 && height <= SSH_CHATTER_MESSAGE_LIMIT) {
        ctx->terminal_height = (unsigned int)height;
    }

    // Trigger a clean screen redraw with the new dimensions.
    ctx->pending_should_sink = true;

    return 0;
}

static void session_install_channel_callbacks(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->channel == nullptr || ctx->channel_cb_installed ||
        ctx->session_data == nullptr) {
        return;
    }

    memset(&ctx->channel_cb, 0, sizeof(ctx->channel_cb));
    ctx->channel_cb.size = sizeof(ctx->channel_cb);
    ctx->channel_cb.userdata = ctx->session_data;
    ctx->channel_cb.channel_pty_window_change_function =
        session_on_window_change;
    ssh_callbacks_init(&ctx->channel_cb);
    if (ssh_set_channel_callbacks(ctx->channel, &ctx->channel_cb) == SSH_OK) {
        ctx->channel_cb_installed = true;
    }
}

static int session_prepare_shell(session_ctx_t *ctx)
{
    ssh_message message = nullptr;
    bool shell_ready = false;

    while (!shell_ready &&
           (message = ssh_message_get(ctx->session)) != nullptr) {
        if (ssh_message_type(message) == SSH_REQUEST_CHANNEL) {
            const int subtype = ssh_message_subtype(message);
            if (subtype == SSH_CHANNEL_REQUEST_PTY ||
                subtype == SSH_CHANNEL_REQUEST_SHELL ||
                subtype == SSH_CHANNEL_REQUEST_EXEC ||
                subtype == SSH_CHANNEL_REQUEST_SUBSYSTEM) {
                if (subtype == SSH_CHANNEL_REQUEST_PTY) {
                    const int raw_width =
                        ssh_message_channel_request_pty_width(message);
                    const int raw_height =
                        ssh_message_channel_request_pty_height(message);
                    int width = raw_width;
                    int height = raw_height;
                    if (width > 0) {
                        if (width > SSH_CHATTER_MESSAGE_LIMIT) {
                            width = SSH_CHATTER_MESSAGE_LIMIT;
                        }
                        ctx->terminal_width = width > 0 ? (unsigned)width: 0;
                    }
                    if (height > 0) {
                        ctx->terminal_height = height > 0 ? (unsigned)height: 0;
                    }
                }
                if (subtype == SSH_CHANNEL_REQUEST_EXEC) {
                    const char *command =
                        ssh_message_channel_request_command(message);
                    ssh_message_channel_request_reply_success(message);
                    if (command != nullptr &&
                        strncmp(command, "scp", 3) == 0 &&
                        ctx->owner != nullptr &&
                        ctx->owner->file_storage_ready) {
                        int result =
                            file_transfer_handle_scp_exec(ctx, command);
                        ctx->exit_status =
                            (result == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
                        ssh_channel_request_send_exit_status(
                            ctx->channel, ctx->exit_status);
                        ssh_channel_send_eof(ctx->channel);
                        ssh_channel_close(ctx->channel);
                        ssh_message_free(message);
                        return 1;
                    }
                } else if (subtype == SSH_CHANNEL_REQUEST_SUBSYSTEM) {
                    const char *subsystem =
                        ssh_message_channel_request_subsystem(message);
                    if (subsystem != nullptr &&
                        strcmp(subsystem, "sftp") == 0) {
                        humanized_log_error("session", "SFTP subsystem requested but not yet implemented (modern scp defaults to SFTP)", 0);
                        ssh_message_channel_request_reply_success(message);
                        int result = file_transfer_handle_sftp(ctx);
                        ctx->exit_status =
                            (result == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
                        ssh_channel_request_send_exit_status(
                            ctx->channel, ctx->exit_status);
                        ssh_channel_send_eof(ctx->channel);
                        ssh_channel_close(ctx->channel);
                        ssh_message_free(message);
                        return 1;
                    } else {
                        ssh_message_reply_default(message);
                    }
                } else {
                    ssh_message_channel_request_reply_success(message);
                }
                if (subtype == SSH_CHANNEL_REQUEST_SHELL) {
                    shell_ready = true;
                }
            } else {
                ssh_message_reply_default(message);
            }
        } else {
            ssh_message_reply_default(message);
        }
        ssh_message_free(message);
    }

    return shell_ready ? 0 : -1;
}

static const char *
session_captcha_question_for_language(const captcha_prompt_t *prompt,
                                      captcha_language_t language)
{
    if (prompt == nullptr) {
        return nullptr;
    }

    switch (language) {
    case CAPTCHA_LANGUAGE_EN:
        return prompt->question_en;
    case CAPTCHA_LANGUAGE_ZH:
        return prompt->question_zh;
    case CAPTCHA_LANGUAGE_RU:
        return prompt->question_ru;
    case CAPTCHA_LANGUAGE_KO:
    default:
        return prompt->question_ko;
    }
}

static const char *
session_captcha_label_for_language(captcha_language_t language)
{
    switch (language) {
    case CAPTCHA_LANGUAGE_EN:
        return "Captcha: ";
    case CAPTCHA_LANGUAGE_ZH:
        return "驗證碼: ";
    case CAPTCHA_LANGUAGE_RU:
        return "Капча: ";
    case CAPTCHA_LANGUAGE_KO:
    default:
        return "캡챠: ";
    }
}

static captcha_language_t
session_captcha_language_from_ui(session_ui_language_t language)
{
    switch (language) {
    case SESSION_UI_LANGUAGE_EN:
        return CAPTCHA_LANGUAGE_EN;
    case SESSION_UI_LANGUAGE_ZH:
        return CAPTCHA_LANGUAGE_ZH;
    case SESSION_UI_LANGUAGE_RU:
        return CAPTCHA_LANGUAGE_RU;
    case SESSION_UI_LANGUAGE_JP:
        return CAPTCHA_LANGUAGE_EN;
    case SESSION_UI_LANGUAGE_KO:
    default:
        return CAPTCHA_LANGUAGE_KO;
    }
}

static captcha_language_t
session_captcha_primary_language(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return CAPTCHA_LANGUAGE_KO;
    }

    bool geo_language_enabled =
        ctx->owner != nullptr ? atomic_load(&ctx->owner->geo_language_enabled)
                              : true;

    session_ui_language_t preferred = session_ui_language_current(ctx);
    captcha_language_t preferred_language =
        session_captcha_language_from_ui(preferred);
    if (preferred != SESSION_UI_LANGUAGE_KO ||
        preferred_language != CAPTCHA_LANGUAGE_KO) {
        return preferred_language;
    }

    if (geo_language_enabled) {
        session_ui_language_t geo_language = session_client_geo_language(ctx);
        if (geo_language != SESSION_UI_LANGUAGE_COUNT) {
            return session_captcha_language_from_ui(geo_language);
        }

        char label[64];
        if (session_detect_provider_ip(ctx->client_ip, label, sizeof(label))) {
            if (string_contains_case_insensitive(label, "Chinese")) {
                return CAPTCHA_LANGUAGE_ZH;
            }
            if (string_contains_case_insensitive(label, "Russian")) {
                return CAPTCHA_LANGUAGE_RU;
            }
            if (string_contains_case_insensitive(label, "Korean")) {
                return CAPTCHA_LANGUAGE_KO;
            }
        }
    }

    return CAPTCHA_LANGUAGE_EN;
}

static bool session_captcha_add_language(captcha_language_t *order,
                                         size_t capacity, size_t *count,
                                         bool used[],
                                         captcha_language_t language)
{
    if (order == nullptr || count == nullptr || used == nullptr) {
        return false;
    }

    size_t index = (size_t)language;
    if (index >= CAPTCHA_LANGUAGE_COUNT) {
        return false;
    }

    if (used[index] || *count >= capacity) {
        return false;
    }

    order[*count] = language;
    used[index] = true;
    ++(*count);
    return true;
}

static size_t session_collect_captcha_languages(const session_ctx_t *ctx,
                                                captcha_language_t *order,
                                                size_t capacity)
{
    if (order == nullptr || capacity == 0U) {
        return 0U;
    }

    bool used[CAPTCHA_LANGUAGE_COUNT] = {false};
    size_t count = 0U;

    captcha_language_t primary = session_captcha_primary_language(ctx);
    session_captcha_add_language(order, capacity, &count, used, primary);

    if (ctx != nullptr) {
        captcha_language_t user_pref =
            session_captcha_language_from_ui(session_ui_language_current(ctx));
        session_captcha_add_language(order, capacity, &count, used, user_pref);

        char label[64];
        if (session_detect_provider_ip(ctx->client_ip, label, sizeof(label))) {
            if (string_contains_case_insensitive(label, "Chinese")) {
                session_captcha_add_language(order, capacity, &count, used,
                                             CAPTCHA_LANGUAGE_ZH);
            }
            if (string_contains_case_insensitive(label, "Russian")) {
                session_captcha_add_language(order, capacity, &count, used,
                                             CAPTCHA_LANGUAGE_RU);
            }
            if (string_contains_case_insensitive(label, "Korean")) {
                session_captcha_add_language(order, capacity, &count, used,
                                             CAPTCHA_LANGUAGE_KO);
            }
        }
    }

    session_captcha_add_language(order, capacity, &count, used,
                                 CAPTCHA_LANGUAGE_EN);
    session_captcha_add_language(order, capacity, &count, used,
                                 CAPTCHA_LANGUAGE_KO);

    static const captcha_language_t kFallbackOrder[] = {
        CAPTCHA_LANGUAGE_ZH,
        CAPTCHA_LANGUAGE_RU,
        CAPTCHA_LANGUAGE_EN,
        CAPTCHA_LANGUAGE_KO,
    };

    for (size_t idx = 0U;
         idx < sizeof(kFallbackOrder) / sizeof(kFallbackOrder[0]); ++idx) {
        session_captcha_add_language(order, capacity, &count, used,
                                     kFallbackOrder[idx]);
    }

    return count;
}

static void session_send_captcha_prompt(session_ctx_t *ctx,
                                        const captcha_prompt_t *prompt,
                                        const captcha_language_t *order,
                                        size_t count)
{
    if (ctx == nullptr || prompt == nullptr || order == nullptr ||
        count == 0U) {
        return;
    }

    for (size_t idx = 0U; idx < count; ++idx) {
        captcha_language_t language = order[idx];
        const char *label = session_captcha_label_for_language(language);
        const char *question =
            session_captcha_question_for_language(prompt, language);
        if (label == nullptr || question == nullptr || question[0] == '\0') {
            continue;
        }

        char line[sizeof(prompt->question_en) + 32];
        int written = snprintf(line, sizeof(line), "%s%s", label, question);
        if (written <= 0) {
            continue;
        }

        session_send_system_line(ctx, line);
    }
}

static void host_update_last_captcha_prompt(host_t *host,
                                            const captcha_prompt_t *prompt,
                                            const captcha_language_t *order,
                                            size_t count)
{
    if (host == nullptr || prompt == nullptr) {
        return;
    }

    static const captcha_language_t kDefaultOrder[] = {
        CAPTCHA_LANGUAGE_KO,
        CAPTCHA_LANGUAGE_EN,
        CAPTCHA_LANGUAGE_ZH,
        CAPTCHA_LANGUAGE_RU,
    };

    const captcha_language_t *languages = order;
    size_t language_count = count;
    if (languages == nullptr || language_count == 0U) {
        languages = kDefaultOrder;
        language_count = sizeof(kDefaultOrder) / sizeof(kDefaultOrder[0]);
    }

    char combined_question[sizeof(prompt->question_en) +
                           sizeof(prompt->question_ko) +
                           sizeof(prompt->question_ru) +
                           sizeof(prompt->question_zh) + 64];
    combined_question[0] = '\0';
    size_t combined_length = 0U;

    for (size_t idx = 0U; idx < language_count; ++idx) {
        const char *label = session_captcha_label_for_language(languages[idx]);
        const char *question =
            session_captcha_question_for_language(prompt, languages[idx]);
        if (label == nullptr || question == nullptr || question[0] == '\0') {
            continue;
        }

        char line[sizeof(prompt->question_en) + 32];
        int written = snprintf(line, sizeof(line), "%s%s", label, question);
        if (written <= 0) {
            continue;
        }

        size_t line_length = (size_t)written;
        if (combined_length > 0U &&
            combined_length + 1U < sizeof(combined_question)) {
            combined_question[combined_length++] = '\n';
        }

        if (combined_length >= sizeof(combined_question)) {
            break;
        }

        size_t available = sizeof(combined_question) - combined_length;
        if (available == 0U) {
            break;
        }

        if (line_length >= available) {
            line_length = available - 1U;
        }

        memcpy(combined_question + combined_length, line, line_length);
        combined_length += line_length;
        combined_question[combined_length] = '\0';
    }

    ttak_mutex_lock(&host->lock);
    snprintf(host->last_captcha_question, sizeof(host->last_captcha_question),
             "%s", combined_question);
    snprintf(host->last_captcha_answer, sizeof(host->last_captcha_answer), "%s",
             prompt->answer);
    host->has_last_captcha = host->last_captcha_question[0] != '\0' &&
                             host->last_captcha_answer[0] != '\0';
    if (host->has_last_captcha) {
        if (clock_gettime(CLOCK_REALTIME, &host->last_captcha_generated) != 0) {
            host->last_captcha_generated.tv_sec = time(nullptr);
            host->last_captcha_generated.tv_nsec = 0L;
        }
    } else {
        host->last_captcha_generated.tv_sec = 0;
        host->last_captcha_generated.tv_nsec = 0L;
    }
    ttak_mutex_unlock(&host->lock);
}

static bool session_run_captcha(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return false;
    }

    captcha_prompt_t prompt;
    session_build_captcha_prompt(ctx, &prompt);
    captcha_language_t languages[CAPTCHA_LANGUAGE_COUNT];
    size_t language_count = session_collect_captcha_languages(
        ctx, languages, sizeof(languages) / sizeof(languages[0]));
    if (language_count == 0U) {
        languages[0] = CAPTCHA_LANGUAGE_KO;
        language_count = 1U;
    }

    host_update_last_captcha_prompt(ctx->owner, &prompt, languages,
                                    language_count);

    bool include_chinese = false;
    for (size_t idx = 0U; idx < language_count; ++idx) {
        if (languages[idx] == CAPTCHA_LANGUAGE_ZH) {
            include_chinese = true;
            break;
        }
    }

    session_send_system_line(
        ctx, "For Windows users: CHANGE TERMINAL ENCODING TO UTF-8");
    if (include_chinese) {
        session_send_system_line(
            ctx, "INFO: Chinese question is in Traditional one to "
                 "cover regions those are NOT Mainland China.");
    }
    session_send_system_line(
        ctx, "Before entering the room, solve this small puzzle.");
    session_send_captcha_prompt(ctx, &prompt, languages, language_count);
    session_send_system_line(ctx, "Type your answer and press Enter:");

    char answer[sizeof(prompt.answer)];
    size_t length = 0U;
    while (length + 1U < sizeof(answer)) {
        char ch = '\0';
        const int read_result = session_transport_read(ctx, &ch, 1, -1);
        if (read_result <= 0) {
            return false;
        }

        if (ch == '\r' || ch == '\n') {
            session_local_echo_char(ctx, '\n');
            break;
        }

        if (ch == '\b' || (unsigned char)ch == 0x7fU) {
            if (length > 0U) {
                --length;
                session_send_raw_text(ctx, "\b \b");
            }
            continue;
        }

        if ((unsigned char)ch < 0x20U) {
            continue;
        }

        answer[length++] = ch;
        session_local_echo_char(ctx, ch);
    }
    answer[length] = '\0';
    trim_whitespace_inplace(answer);

    if (answer[0] == '\0') {
        session_send_system_line(ctx, "Captcha answer missing. Disconnecting.");
        return false;
    }

    if (strcasecmp(prompt.answer, "dog") == 0 && strcmp(answer, "개") == 0) {
        snprintf(answer, sizeof(answer), "%s", "dog");
    }

    if (strcasecmp(answer, prompt.answer) == 0) {
        session_send_system_line(ctx, "Captcha solved. Welcome aboard!");
        return true;
    }

    session_send_system_line(ctx, "Captcha failed. Disconnecting.");
    return false;
}

static bool session_is_captcha_exempt(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }

    if (ctx->user.name[0] == '\0') {
        return false;
    }

    char lowered[sizeof(ctx->user.name)];
    size_t idx = 0U;
    for (; idx + 1U < sizeof(lowered) && ctx->user.name[idx] != '\0'; ++idx) {
        lowered[idx] = (char)tolower((unsigned char)ctx->user.name[idx]);
    }
    if (idx < sizeof(lowered)) {
        lowered[idx] = '\0';
    } else {
        lowered[sizeof(lowered) - 1U] = '\0';
    }

    return strcmp(lowered, "gpt") == 0;
}

static void session_print_help(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    const session_ui_locale_t *locale = session_ui_get_locale(ctx);
    const char *prefix = session_command_prefix(ctx);

    char help_buffer[SSH_CHATTER_MESSAGE_LIMIT *
                     32]; // A large enough buffer for help messages
    help_buffer[0] = '\0';

    if (locale->help_title != nullptr && locale->help_title[0] != '\0') {
        session_send_system_line(ctx, locale->help_title);
    }

    session_format_help_entries_to_buffer(ctx, kSessionHelpEssential,
                                          sizeof(kSessionHelpEssential) /
                                              sizeof(kSessionHelpEssential[0]),
                                          help_buffer, sizeof(help_buffer));
    session_send_raw_text(ctx, help_buffer);

    if (locale->help_hint_extra != nullptr &&
        locale->help_hint_extra[0] != '\0') {
        const char *args[] = {prefix};
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        session_format_template(locale->help_hint_extra, args,
                                sizeof(args) / sizeof(args[0]), line,
                                sizeof(line));
        session_send_system_line(ctx, line);
    }

    if (locale->help_scroll_hint != nullptr &&
        locale->help_scroll_hint[0] != '\0') {
        session_send_system_line(ctx, locale->help_scroll_hint);
    }

    if (locale->help_regular_hint != nullptr &&
        locale->help_regular_hint[0] != '\0') {
        session_send_system_line(ctx, locale->help_regular_hint);
    }
}

static bool session_line_is_exit_command(const char *line)
{
    if (line == nullptr) {
        return false;
    }

    if (strncmp(line, "/exit", 5) != 0) {
        return false;
    }

    const char trailing = line[5];
    if (trailing == '\0') {
        return true;
    }

    if (!isspace((unsigned char)trailing)) {
        return false;
    }

    for (size_t idx = 6U; line[idx] != '\0'; ++idx) {
        if (!isspace((unsigned char)line[idx])) {
            return false;
        }
    }

    return true;
}

static void session_handle_username_conflict_input(session_ctx_t *ctx,
                                                   const char *line)
{
    if (ctx == nullptr) {
        return;
    }

    if (session_line_is_exit_command(line)) {
        if (ctx->ops != nullptr && ctx->ops->handle_exit != nullptr) {
            ctx->ops->handle_exit(ctx);
        }
        return;
    }

    char reminder[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(reminder, sizeof(reminder), "The username '%s' is already in use.",
             ctx->user.name);
    session_send_system_line(ctx, reminder);
    session_send_system_line(
        ctx, "Reconnect with a different username by running: ssh "
             "newname@<server> (or ssh -l newname <server>)");
    session_send_system_line(ctx, "Type /exit to quit.");
}

static bool session_prepare_slash_command(const char *input, char *output,
                                          size_t length)
{
    if (input == nullptr || output == nullptr || length == 0U) {
        return false;
    }

    const unsigned char *start = (const unsigned char *)input;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        ++start;
    }

    if (*start != '/') {
        return false;
    }

    const unsigned char *end = start;
    while (*end != '\0') {
        ++end;
    }
    while (end > start && isspace((unsigned char)*(end - 1U))) {
        --end;
    }

    size_t copy_len = (size_t)(end - start);
    if (copy_len == 0U) {
        return false;
    }
    if (copy_len >= length) {
        copy_len = length - 1U;
    }

    memcpy(output, start, copy_len);
    output[copy_len] = '\0';
    return true;
}

static bool session_acquire_cpu_slot(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return true;
    }

    host_t *host = ctx->owner;
    for (;;) {
        bool acquired = false;
        ttak_mutex_lock(&host->lock);
        if (host->cpu_slot_in_use < host->cpu_slot_limit) {
            size_t slot_index = host->cpu_slot_in_use;
            host->cpu_slot_in_use++;
            if (slot_index < 64U) {
                host->cpu_slot_mask |= (1ULL << slot_index);
            }
            acquired = true;
        } else {
            host->cpu_slot_waiting++;
        }
        ttak_mutex_unlock(&host->lock);

        if (acquired) {
            return true;
        }

        session_send_system_line(
            ctx,
            "Server CPU slots are full. Queued and waiting for available slot...");
        const struct timespec wait_time = {.tv_sec = 0, .tv_nsec = 20000000L};
        host_sleep_uninterruptible(&wait_time);

        ttak_mutex_lock(&host->lock);
        if (host->cpu_slot_waiting > 0U) {
            host->cpu_slot_waiting--;
        }
        ttak_mutex_unlock(&host->lock);
    }
}

static void session_release_cpu_slot(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    host_t *host = ctx->owner;
    ttak_mutex_lock(&host->lock);
    if (host->cpu_slot_in_use > 0U) {
        size_t slot_index = host->cpu_slot_in_use - 1U;
        host->cpu_slot_in_use--;
        if (slot_index < 64U) {
            host->cpu_slot_mask &= ~(1ULL << slot_index);
        }
    }
    ttak_mutex_unlock(&host->lock);
}

static void session_process_line(session_ctx_t *ctx, const char *line)
{
    if (ctx == nullptr || line == nullptr) {
        return;
    }

    char normalized[SSH_CHATTER_MAX_INPUT_LEN];
    snprintf(normalized, sizeof(normalized), "%s", line);
    session_normalize_newlines(normalized);

    const bool composing_draft = ctx->bbs_post_pending || ctx->asciiart_pending;

    if (!composing_draft) {
        switch ((int)normalized[0]) {
        // SLASH_COMPATIBLE: slash compatible chars.
        case (int)'.':
        case (int)'_':
        case (int)'@':
        case (int)'$':
        case (int)'*':
        case (int)'-':
        case (int)'#':
        case (int)'>':
            normalized[0] = '/';
            break;
        default:
        }
    }

    if (ctx->bbs_post_pending) {
        session_bbs_capture_body_text(ctx, normalized);
        return;
    }

    if (ctx->asciiart_pending) {
        session_asciiart_capture_text(ctx, normalized);
        return;
    }

    char command_line[SSH_CHATTER_MAX_INPUT_LEN];

    if (normalized[0] == '\0') {
        return;
    }

    if (!session_acquire_cpu_slot(ctx)) {
        return;
    }

    if (ctx->game.active) {
        if (strcmp(normalized, "/suspend!") == 0) {
            session_game_suspend(ctx, "Game suspended.");
            session_release_cpu_slot(ctx);
            return;
        }

        if (normalized[0] == 't' && normalized[1] == '\0') {
            if (ctx->game.is_camouflaged) {
                ctx->game.is_camouflaged = false;
                if (ctx->game.type == SESSION_GAME_TETRIS) {
                    if (ctx->game.tetris != nullptr &&
                        ctx->game.saved_tetris_state != nullptr) {
                        *ctx->game.tetris = *ctx->game.saved_tetris_state;
                    }
                    ctx->game.tetris->gravity_timer_initialized = false;
                    ctx->game.tetris->gravity_timer_accumulator_ns = 0U;
                    session_game_tetris_render(ctx);
                } else if (ctx->game.type == SESSION_GAME_LIARGAME) {
                    ctx->game.liar = ctx->game.saved_liar_state;
                    session_game_liar_present_round(ctx);
                } else if (ctx->game.type == SESSION_GAME_ALPHA) {
                    ctx->game.alpha = ctx->game.saved_alpha_state;
                    session_game_alpha_present_stage(ctx);
                } else if (ctx->game.type == SESSION_GAME_OTHELLO) {
                    ctx->game.othello = ctx->game.saved_othello_state;
                    session_game_othello_render(ctx);
                    session_game_othello_prepare_next_turn(ctx);
                }
            } else {
                ctx->game.is_camouflaged = true;
                if (ctx->game.type == SESSION_GAME_TETRIS) {
                    if (ctx->game.tetris != nullptr &&
                        ctx->game.saved_tetris_state != nullptr) {
                        *ctx->game.saved_tetris_state = *ctx->game.tetris;
                        ctx->game.saved_tetris_state->gravity_timer_initialized =
                            false;
                        ctx->game.saved_tetris_state
                            ->gravity_timer_accumulator_ns = 0U;
                    }
                    ctx->game.tetris->gravity_timer_initialized = false;
                    ctx->game.tetris->gravity_timer_accumulator_ns = 0U;
                } else if (ctx->game.type == SESSION_GAME_LIARGAME) {
                    ctx->game.saved_liar_state = ctx->game.liar;
                } else if (ctx->game.type == SESSION_GAME_ALPHA) {
                    ctx->game.saved_alpha_state = ctx->game.alpha;
                } else if (ctx->game.type == SESSION_GAME_OTHELLO) {
                    ctx->game.saved_othello_state = ctx->game.othello;
                }
                session_game_show_camouflage(ctx);
            }
            session_release_cpu_slot(ctx);
            return;
        }

        if (normalized[0] == '/') {
            session_send_system_line(
                ctx, "Finish the current game with /suspend! first.");
            session_release_cpu_slot(ctx);
            return;
        }

        if (ctx->game.type == SESSION_GAME_TETRIS) {
            session_game_tetris_handle_line(ctx, normalized);
        } else if (ctx->game.type == SESSION_GAME_LIARGAME) {
            session_game_liar_handle_line(ctx, normalized);
        } else if (ctx->game.type == SESSION_GAME_ALPHA) {
            session_game_alpha_handle_line(ctx, normalized);
        } else if (ctx->game.type == SESSION_GAME_OTHELLO) {
            session_game_othello_handle_line(ctx, normalized);
        } else if (ctx->game.type == SESSION_GAME_GONU) {
            session_game_gonu_handle_input(ctx, normalized);
        }
        session_release_cpu_slot(ctx);
        return;
    }

    if (ctx->in_rss_mode) {
        if (strcmp(normalized, "/exit") == 0) {
            session_rss_exit(ctx, nullptr);
        } else {
            const char *rss_args = nullptr;
            if (session_parse_command(normalized, "/rss", &rss_args)) {
                session_rss_exit(ctx, nullptr);
                session_handle_rss(ctx, rss_args);
            } else {
                session_send_system_line(
                    ctx, "RSS reader active. Use /rss exit Ctrl+Z, or "
                         "Terminate to return to chat.");
            }
        }
        session_release_cpu_slot(ctx);
        return;
    }

    bool translation_bypass = false;
    char bypass_buffer[SSH_CHATTER_MAX_INPUT_LEN];
    if (translation_strip_no_translate_prefix(normalized, bypass_buffer,
                                              sizeof(bypass_buffer))) {
        translation_bypass = true;
        snprintf(normalized, sizeof(normalized), "%s", bypass_buffer);
    }

    if (normalized[0] == '\0') {
        return;
    }

    const struct timespec tiny_delay = {.tv_sec = 0, .tv_nsec = 5000000L};
    host_sleep_uninterruptible(&tiny_delay);

    if (ctx->username_conflict) {
        session_handle_username_conflict_input(ctx, normalized);
        session_release_cpu_slot(ctx);
        return;
    }

    if (ctx->ops == nullptr || ctx->ops->dispatch_command == nullptr) {
        session_release_cpu_slot(ctx);
        return;
    }

    if (!translation_bypass) {
        if (session_prepare_slash_command(normalized, command_line,
                                          sizeof(command_line))) {
            if (session_try_localized_command_forward(ctx, command_line)) {
                session_release_cpu_slot(ctx);
                return;
            }
            ctx->ops->dispatch_command(ctx, command_line);
            session_release_cpu_slot(ctx);
            return;
        }
    }

    if (!translation_bypass && normalized[0] == '/') {
        if (session_try_localized_command_forward(ctx, normalized)) {
            session_release_cpu_slot(ctx);
            return;
        }
        ctx->ops->dispatch_command(ctx, normalized);
        session_release_cpu_slot(ctx);
        return;
    }

    const char *trimmed = normalized;
    while (*trimmed == ' ' || *trimmed == '\t') {
        ++trimmed;
    }

    if (!translation_bypass && ctx->input_mode == SESSION_INPUT_MODE_COMMAND &&
        *trimmed != '\0') {
        const char *command_text = trimmed;
        char command_buffer[SSH_CHATTER_MAX_INPUT_LEN];
        if (command_text[0] != '/') {
            command_buffer[0] = '/';
            size_t command_len =
                strnlen(command_text, sizeof(command_buffer) - 2U);
            memcpy(&command_buffer[1], command_text, command_len);
            command_buffer[command_len + 1U] = '\0';
            command_text = command_buffer;
        }
        if (session_prepare_slash_command(command_text, command_buffer,
                                          sizeof(command_buffer))) {
            ctx->ops->dispatch_command(ctx, command_buffer);
        } else {
            ctx->ops->dispatch_command(ctx, command_text);
        }
        session_release_cpu_slot(ctx);
        return;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        now.tv_sec = time(nullptr);
        now.tv_nsec = 0L;
    }

    const bool asciiart_active = ctx->asciiart_pending;
    bool ascii_profile_command = asciiart_active;
    if (!ascii_profile_command && normalized[0] == '/') {
        const char *command_args = nullptr;
        if (session_parse_command(normalized, "/asciiart", &command_args)) {
            ascii_profile_command = true;
        }
    }

    const bool translation_throttle =
        ctx->translation_enabled && ctx->input_translation_enabled &&
        ctx->input_translation_language[0] != '\0';
    const bool chat_throttle = ctx->input_mode == SESSION_INPUT_MODE_CHAT;
    if ((translation_throttle || chat_throttle) && ctx->has_last_message_time) {
        time_t sec_delta = now.tv_sec - ctx->last_message_time.tv_sec;
        long nsec_delta = now.tv_nsec - ctx->last_message_time.tv_nsec;
        if (nsec_delta < 0L) {
            --sec_delta;
            nsec_delta += 1000000000L;
        }
        if (translation_throttle &&
            (sec_delta < 0 || (sec_delta == 0 && nsec_delta < 1000000000L))) {
            session_send_system_line(ctx, "Please wait at least one second "
                                          "before sending another message.");
            session_release_cpu_slot(ctx);
            return;
        }
        if (!translation_throttle && chat_throttle && !ascii_profile_command &&
            !ctx->bracket_paste_active &&
            (sec_delta < 0 || (sec_delta == 0 && nsec_delta < 300000000L))) {
            session_send_system_line(ctx,
                                     "Please wait at least 300 milliseconds "
                                     "before sending another chat message.");
            session_release_cpu_slot(ctx);
            return;
        }
    }

    ctx->last_message_time = now;
    ctx->has_last_message_time = true;

    if (!translation_bypass && ctx->translation_enabled &&
        ctx->input_translation_enabled &&
        ctx->input_translation_language[0] != '\0') {
        if (session_translation_queue_input(ctx, normalized)) {
            session_release_cpu_slot(ctx);
            return;
        }
        session_send_system_line(
            ctx, "Translation unavailable; sending your original message.");
    }

    printf("[%s] %s\n", ctx->user.name, normalized);
    session_deliver_outgoing_message(ctx, normalized, true);
    session_release_cpu_slot(ctx);
}

void host_session_process_line_for_testing(session_ctx_t *ctx, const char *line)
{
    session_process_line(ctx, line);
}

static void session_handle_kick(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->user.is_operator) {
        session_send_system_line(ctx, "You are not allowed to kick users.");
        return;
    }

    if (arguments == nullptr || *arguments == '\0') {
        session_send_system_line(ctx, "Usage: /kick <username>");
        return;
    }

    char target_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(target_name, sizeof(target_name), "%s", arguments);
    trim_whitespace_inplace(target_name);

    if (target_name[0] == '\0') {
        session_send_system_line(ctx, "Usage: /kick <username>");
        return;
    }

    session_ctx_t *target = chat_room_find_user(&ctx->owner->room, target_name);
    if (target == nullptr) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "User '%s' is not connected.",
                 target_name);
        session_send_system_line(ctx, message);
        return;
    }

    if (target == ctx) {
        session_send_system_line(ctx, "You cannot kick yourself.");
        return;
    }

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice), "* [%s] has been kicked by [%s]",
             target->user.name, ctx->user.name);
    host_history_record_system(ctx->owner, notice, nullptr);
    chat_room_broadcast(&ctx->owner->room, notice, nullptr);

    const bool target_active = session_transport_active(target);
    if (!target_active || (target->transport_kind == SESSION_TRANSPORT_SSH &&
                           target->session == nullptr)) {
        target->should_exit = true;
        target->has_joined_room = false;
        chat_room_remove(&ctx->owner->room, target);
        session_send_system_line(ctx, "User removed from the chat.");
    } else {
        session_send_system_line(target,
                                 "You have been kicked by an operator.");
        target->should_exit = true;
        session_transport_request_close(target);
        target->has_joined_room = false;
        chat_room_remove(&ctx->owner->room, target);
        session_send_system_line(ctx, "User removed from the chat.");
    }

    printf("[kick] %s kicked %s\n", ctx->user.name, target->user.name);
}

static void session_handle_ban_name(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (!ctx->user.is_operator) {
        session_send_system_line(ctx, "You are not allowed to ban nicknames.");
        return;
    }

    if (ctx->owner == nullptr) {
        session_send_system_line(ctx, "Host unavailable.");
        return;
    }

    if (arguments == nullptr || *arguments == '\0') {
        session_send_system_line(ctx, "Usage: /banname <nickname>");
        return;
    }

    char target_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(target_name, sizeof(target_name), "%s", arguments);
    trim_whitespace_inplace(target_name);

    if (target_name[0] == '\0') {
        session_send_system_line(ctx, "Usage: /banname <nickname>");
        return;
    }

    for (size_t idx = 0U; target_name[idx] != '\0'; ++idx) {
        const unsigned char ch = (unsigned char)target_name[idx];
        if (ch <= 0x1FU || ch == 0x7FU || ch == ' ' || ch == '\t') {
            session_send_system_line(
                ctx,
                "Nicknames may not include control characters or whitespace.");
            return;
        }
    }

    if (host_is_username_banned(ctx->owner, target_name)) {
        session_send_system_line(
            ctx, "That nickname is already blocked for bot detection.");
        return;
    }

    if (!host_add_ban_entry(ctx->owner, target_name, "")) {
        session_send_system_line(ctx, "Unable to add ban entry (list full?).");
        return;
    }

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice),
             "* Nickname '%s' blocked for bot detection by [%s]", target_name,
             ctx->user.name);
    host_history_record_system(ctx->owner, notice, nullptr);
    chat_room_broadcast(&ctx->owner->room, notice, nullptr);
    session_send_system_line(ctx, "Nickname ban applied.");
    printf("[banname] %s banned nickname %s\n", ctx->user.name, target_name);

    session_ctx_t *active = chat_room_find_user(&ctx->owner->room, target_name);
    if (active != nullptr) {
        session_send_system_line(
            active, "Your nickname is now blocked for bot detection. "
                    "Use /nick <name> to change immediately.");
    }
}

static void session_handle_ban(session_ctx_t *ctx, const char *arguments)
{
    if (!ctx->user.is_operator) {
        session_send_system_line(ctx, "You are not allowed to ban users.");
        return;
    }

    if (arguments == nullptr || *arguments == '\0') {
        session_send_system_line(ctx, "Usage: /ban <username>");
        return;
    }

    char target_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(target_name, sizeof(target_name), "%s", arguments);
    trim_whitespace_inplace(target_name);

    if (target_name[0] == '\0') {
        session_send_system_line(ctx, "Usage: /ban <username>");
        return;
    }

    session_ctx_t *target = chat_room_find_user(&ctx->owner->room, target_name);
    if (target == nullptr) {
        bool valid_ip = false;
        unsigned char inet_buffer[sizeof(struct in6_addr)];
        if (inet_pton(AF_INET, target_name, inet_buffer) == 1 ||
            inet_pton(AF_INET6, target_name, inet_buffer) == 1) {
            valid_ip = true;
        }

        bool valid_cidr = false;
        if (!valid_ip && strchr(target_name, '/') != nullptr) {
            uint32_t ipv4_network = 0U;
            uint32_t ipv4_mask = 0U;
            struct in6_addr ipv6_network;
            struct in6_addr ipv6_mask;
            memset(&ipv6_network, 0, sizeof(ipv6_network));
            memset(&ipv6_mask, 0, sizeof(ipv6_mask));
            valid_cidr =
                host_parse_ipv4_cidr(target_name, &ipv4_network, &ipv4_mask) ||
                host_parse_ipv6_cidr(target_name, &ipv6_network, &ipv6_mask);
        }

        if (valid_ip || valid_cidr) {
            if (host_add_ban_entry(ctx->owner, "", target_name)) {
                char notice[SSH_CHATTER_MESSAGE_LIMIT];
                const char *label = valid_cidr ? "CIDR" : "IP";
                snprintf(notice, sizeof(notice), "%s '%s' has been banned.",
                         label, target_name);
                session_send_system_line(ctx, notice);
            } else {
                session_send_system_line(
                    ctx, "Unable to add ban entry (list full?).");
            }
        } else {
            char not_found[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(not_found, sizeof(not_found),
                     "User '%s' is not connected.", target_name);
            session_send_system_line(ctx, not_found);
        }
        return;
    }

    if (target->user.is_lan_operator) {
        session_send_system_line(ctx, "LAN operators cannot be banned.");
        return;
    }

    const char *target_ip =
        target->client_ip[0] != '\0' ? target->client_ip : "";
    if (!host_add_ban_entry(ctx->owner, target->user.name, target_ip)) {
        session_send_system_line(ctx, "Unable to add ban entry (list full?).");
        return;
    }

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice), "* [%s] has been banned by [%s]",
             target->user.name, ctx->user.name);
    host_history_record_system(ctx->owner, notice, nullptr);
    chat_room_broadcast(&ctx->owner->room, notice, nullptr);
    session_send_system_line(ctx, "Ban applied.");
    printf("[ban] %s banned %s (%s)\n", ctx->user.name, target->user.name,
           target_ip[0] != '\0' ? target_ip : "unknown");

    if (session_transport_active(target)) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "You have been banned by [%s].",
                 ctx->user.name);
        session_send_system_line(target, message);
        target->should_exit = true;
        session_transport_request_close(target);
    }
}

static void session_handle_ban_list(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (!ctx->user.is_operator) {
        session_send_system_line(ctx,
                                 "You are not allowed to view the ban list.");
        return;
    }

    if (arguments != nullptr) {
        while (*arguments != '\0' && isspace((unsigned char)*arguments)) {
            ++arguments;
        }
        if (*arguments != '\0') {
            session_send_system_line(ctx, "Usage: /banlist");
            return;
        }
    }

    host_t *host = ctx->owner;
    if (host == nullptr) {
        session_send_system_line(ctx, "Host unavailable.");
        return;
    }

    typedef struct ban_snapshot {
        char username[SSH_CHATTER_USERNAME_LEN];
        char ip[SSH_CHATTER_IP_LEN];
    } ban_snapshot_t;

    ban_snapshot_t entries[SSH_CHATTER_MAX_BANS];
    size_t entry_count = 0U;

    ttak_mutex_lock(&host->lock);
    entry_count = host->ban_count;
    if (entry_count > SSH_CHATTER_MAX_BANS) {
        entry_count = SSH_CHATTER_MAX_BANS;
    }
    for (size_t idx = 0U; idx < entry_count; ++idx) {
        snprintf(entries[idx].username, sizeof(entries[idx].username), "%s",
                 host->bans[idx].username);
        snprintf(entries[idx].ip, sizeof(entries[idx].ip), "%s",
                 host->bans[idx].ip);
    }
    ttak_mutex_unlock(&host->lock);

    if (entry_count == 0U) {
        session_send_system_line(ctx, "No active bans.");
        return;
    }

    session_send_system_line(ctx, "Active bans:");
    enum {
        SESSION_BAN_USERNAME_PREC = SSH_CHATTER_USERNAME_LEN - 1,
        SESSION_BAN_IP_PREC = SSH_CHATTER_IP_LEN - 1
    };
    for (size_t idx = 0U; idx < entry_count; ++idx) {
        const char *username = entries[idx].username;
        const char *ip = entries[idx].ip;
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        if (username[0] != '\0' && ip[0] != '\0') {
            snprintf(message, sizeof(message),
                     "%zu. user: %.*s, ip: %.*s", idx + 1U,
                     SESSION_BAN_USERNAME_PREC, username,
                     SESSION_BAN_IP_PREC, ip);
        } else if (username[0] != '\0') {
            snprintf(message, sizeof(message), "%zu. user: %.*s", idx + 1U,
                     SESSION_BAN_USERNAME_PREC, username);
        } else if (ip[0] != '\0') {
            snprintf(message, sizeof(message), "%zu. ip: %.*s", idx + 1U,
                     SESSION_BAN_IP_PREC, ip);
        } else {
            snprintf(message, sizeof(message), "%zu. <empty>", idx + 1U);
        }
        session_send_system_line(ctx, message);
    }
}

static void session_handle_getaddr(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(ctx,
                                 "You are not allowed to run that command.");
        return;
    }

    if (arguments == nullptr || *arguments == '\0') {
        session_send_system_line(ctx, "Usage: /getaddr <username>");
        return;
    }

    char target_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(target_name, sizeof(target_name), "%s", arguments);
    trim_whitespace_inplace(target_name);

    if (target_name[0] == '\0') {
        session_send_system_line(ctx, "Usage: /getaddr <username>");
        return;
    }

    host_t *host = ctx->owner;
    if (host == nullptr) {
        session_send_system_line(ctx, "Host unavailable.");
        return;
    }

    char ip[SSH_CHATTER_IP_LEN];
    if (!host_lookup_last_ip(host, target_name, ip, sizeof(ip)) ||
        ip[0] == '\0') {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "No recorded address for '%s'.",
                 target_name);
        session_send_system_line(ctx, message);
        return;
    }

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(message, sizeof(message), "Last known address for '%s': %s",
             target_name, ip);
    session_send_system_line(ctx, message);
}

static void session_handle_poke(session_ctx_t *ctx, const char *arguments)
{
    if (arguments == nullptr || *arguments == '\0') {
        session_send_system_line(ctx, "Usage: /poke <username>");
        return;
    }

    session_ctx_t *target = chat_room_find_user(&ctx->owner->room, arguments);
    if (target == nullptr) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "User '%s' is not connected.",
                 arguments);
        session_send_system_line(ctx, message);
        return;
    }

    printf("[poke] %s pokes %s\n", ctx->user.name, target->user.name);
    session_channel_write(target, "\a", 1U);
    session_send_system_line(ctx, "Poke sent.");
}

static void session_handle_block(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    static const char *kUsage =
        "Usage: /block <username|ip|list|confirm <username> <only|ip>>";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/block", kUsage, usage, sizeof(usage));

    if (arguments == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    if (strcasecmp(working, "list") == 0) {
        session_blocklist_show(ctx);
        return;
    }

    if (strncasecmp(working, "confirm", 7) == 0 &&
        (working[7] == '\0' || isspace((unsigned char)working[7]))) {
        char *cursor = working + 7;
        while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
            ++cursor;
        }

        if (*cursor == '\0') {
            session_send_system_line(ctx, usage);
            return;
        }

        char username[SSH_CHATTER_USERNAME_LEN];
        size_t name_len = 0U;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor) &&
               name_len + 1U < sizeof(username)) {
            username[name_len++] = *cursor++;
        }
        username[name_len] = '\0';

        while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
            ++cursor;
        }

        if (*cursor == '\0') {
            session_send_system_line(ctx, usage);
            return;
        }

        char mode[16];
        size_t mode_len = 0U;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor) &&
               mode_len + 1U < sizeof(mode)) {
            mode[mode_len++] = *cursor++;
        }
        mode[mode_len] = '\0';

        if (!ctx->block_pending.active) {
            session_send_system_line(
                ctx, "No provider block is awaiting confirmation.");
            return;
        }

        if (strncmp(ctx->block_pending.username, username,
                    SSH_CHATTER_USERNAME_LEN) != 0) {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message),
                     "Pending block is for [%s], not [%s].",
                     ctx->block_pending.username, username);
            session_send_system_line(ctx, message);
            return;
        }

        bool block_ip = false;
        if (strcasecmp(mode, "ip") == 0 || strcasecmp(mode, "all") == 0 ||
            strcasecmp(mode, "full") == 0) {
            block_ip = true;
        } else if (strcasecmp(mode, "only") == 0 ||
                   strcasecmp(mode, "user") == 0 ||
                   strcasecmp(mode, "name") == 0) {
            block_ip = false;
        } else {
            session_send_system_line(ctx, usage);
            return;
        }

        bool already_present = false;
        if (!session_blocklist_add(ctx, ctx->block_pending.ip,
                                   ctx->block_pending.username, block_ip,
                                   &already_present)) {
            if (already_present) {
                session_send_system_line(ctx,
                                         "That target is already blocked.");
            } else {
                session_send_system_line(
                    ctx, "Unable to add block entry (limit reached?).");
            }
        } else {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            if (block_ip) {
                snprintf(message, sizeof(message),
                         "Blocking all users from %.63s.",
                         ctx->block_pending.ip);
            } else {
                snprintf(message, sizeof(message),
                         "Blocking [%.23s] only (IP %.63s).",
                         ctx->block_pending.username, ctx->block_pending.ip);
            }
            session_send_system_line(ctx, message);
        }

        ctx->block_pending.active = false;
        ctx->block_pending.username[0] = '\0';
        ctx->block_pending.ip[0] = '\0';
        ctx->block_pending.provider_label[0] = '\0';
        return;
    }

    unsigned char inet_buffer[sizeof(struct in6_addr)];
    if (inet_pton(AF_INET, working, inet_buffer) == 1 ||
        inet_pton(AF_INET6, working, inet_buffer) == 1) {
        bool already_present = false;
        char label[64];
        bool provider =
            session_detect_provider_ip(working, label, sizeof(label));
        if (provider && label[0] != '\0') {
            char warning[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(
                warning, sizeof(warning),
                "Error: You cannot ban a country."
                "%.256s is flagged as %.63s; other people may also be hidden.",
                working, label);
            session_send_system_line(ctx, warning);
            return;
        }
        if (!session_blocklist_add(ctx, working, "", true, &already_present)) {
            if (already_present) {
                session_send_system_line(ctx, "That IP is already blocked.");
            } else {
                session_send_system_line(
                    ctx, "Unable to add block entry (limit reached?).");
            }
        } else {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message),
                     "Blocking all users from %.256s.", working);
            session_send_system_line(ctx, message);
        }
        return;
    }

    if (ctx->owner == nullptr) {
        session_send_system_line(ctx, "Block list unavailable right now.");
        return;
    }

    session_ctx_t *target = chat_room_find_user(&ctx->owner->room, working);
    if (target == nullptr) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "User '%.256s' is not connected.",
                 working);
        session_send_system_line(ctx, message);
        return;
    }

    if (target == ctx) {
        session_send_system_line(ctx, "You do not need to block yourself.");
        return;
    }

    if (target->client_ip[0] == '\0') {
        session_send_system_line(
            ctx, "Unable to identify that user's IP address right now.");
        return;
    }

    char label[64];
    if (session_detect_provider_ip(target->client_ip, label, sizeof(label))) {
        memset(&ctx->block_pending, 0, sizeof(ctx->block_pending));
        ctx->block_pending.active = true;
        snprintf(ctx->block_pending.username,
                 sizeof(ctx->block_pending.username), "%s", target->user.name);
        snprintf(ctx->block_pending.ip, sizeof(ctx->block_pending.ip), "%s",
                 target->client_ip);
        snprintf(ctx->block_pending.provider_label,
                 sizeof(ctx->block_pending.provider_label), "%.31s", label);

        char warning[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(warning, sizeof(warning), "%.63s appears to belong to %.63s.",
                 target->client_ip, label);
        session_send_system_line(ctx, warning);

        char prompt[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(prompt, sizeof(prompt),
                 "Use /block confirm %.23s only to hide just [%.23s] or /block "
                 "confirm %.23s ip to hide everyone from that IP.",
                 target->user.name, target->user.name, target->user.name);
        session_send_system_line(ctx, prompt);
        return;
    }

    bool already_present = false;
    if (!session_blocklist_add(ctx, target->client_ip, target->user.name, true,
                               &already_present)) {
        if (already_present) {
            session_send_system_line(ctx, "That address is already blocked.");
        } else {
            session_send_system_line(
                ctx, "Unable to add block entry (limit reached?).");
        }
    } else {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message),
                 "Blocking all users from %.63s (triggered by [%.23s]).",
                 target->client_ip, target->user.name);
        session_send_system_line(ctx, message);
    }
}

static void session_handle_unblock(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    static const char *kUsage = "Usage: /unblock <username|ip|all>";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/unblock", kUsage, usage, sizeof(usage));

    if (arguments == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    if (strcasecmp(working, "all") == 0) {
        size_t removed = 0U;
        for (size_t idx = 0U; idx < SSH_CHATTER_MAX_BLOCKED; ++idx) {
            if (ctx->block_entries[idx].in_use) {
                memset(&ctx->block_entries[idx], 0,
                       sizeof(ctx->block_entries[idx]));
                ++removed;
            }
        }
        ctx->block_entry_count = 0U;
        ctx->block_pending.active = false;
        ctx->block_pending.username[0] = '\0';
        ctx->block_pending.ip[0] = '\0';
        ctx->block_pending.provider_label[0] = '\0';

        if (removed == 0U) {
            session_send_system_line(ctx, "No blocked entries to remove.");
        } else {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message), "Removed %zu blocked entr%s.",
                     removed, removed == 1U ? "y" : "ies");
            session_send_system_line(ctx, message);
        }
        return;
    }

    if (session_blocklist_remove(ctx, working)) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "Removed block for %.256s.",
                 working);
        session_send_system_line(ctx, message);
    } else {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "No block entry matched '%.256s'.",
                 working);
        session_send_system_line(ctx, message);
    }
}

static void session_handle_pm(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    static const char *kUsage = "Usage: /pm <username> <message>";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/pm", kUsage, usage, sizeof(usage));

    if (ctx->owner == nullptr) {
        session_send_system_line(ctx,
                                 "Private messages are unavailable right now.");
        return;
    }

    if (arguments == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    char *cursor = working;
    while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
        ++cursor;
    }

    if (*cursor == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    *cursor = '\0';
    char *message = cursor + 1;
    while (*message != '\0' && isspace((unsigned char)*message)) {
        ++message;
    }

    if (*message == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    char target_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(target_name, sizeof(target_name), "%.*s",
             (int)sizeof(target_name) - 1, working);

    session_ctx_t *target = chat_room_find_user(&ctx->owner->room, target_name);
    if (target == nullptr) {
        char not_found[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(not_found, sizeof(not_found), "User '%s' is not connected.",
                 target_name);
        session_send_system_line(ctx, not_found);
        return;
    }

    char prepared[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(prepared, sizeof(prepared), "%s", message);

    char stripped[SSH_CHATTER_MESSAGE_LIMIT];
    bool translation_bypass = translation_strip_no_translate_prefix(
        prepared, stripped, sizeof(stripped));
    const char *deliver_body = translation_bypass ? stripped : prepared;

    const char *target_display = target->user.name;
    printf("[pm] %s -> %s: %s\n", ctx->user.name, target_display, deliver_body);

    char to_target_label[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(to_target_label, sizeof(to_target_label), "%s -> you",
             ctx->user.name);

    char to_sender_label[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(to_sender_label, sizeof(to_sender_label), "you -> %s",
             target_display);

    bool attempt_translation = (target != nullptr) && !translation_bypass &&
                               ctx->translation_enabled &&
                               ctx->input_translation_enabled &&
                               ctx->input_translation_language[0] != '\0';

    if (attempt_translation) {
        if (session_translation_queue_private_message(ctx, target,
                                                      deliver_body)) {
            return;
        }
        session_send_system_line(
            ctx, "Translation unavailable; sending your original message.");
    }

    session_send_private_message_line(target, ctx, to_target_label,
                                      deliver_body);
    session_send_private_message_line(ctx, ctx, to_sender_label, deliver_body);
}

static bool username_contains(const char *username, const char *needle)
{
    if (username == nullptr || needle == nullptr) {
        return false;
    }

    const size_t needle_len = strlen(needle);
    if (needle_len == 0U) {
        return false;
    }

    const size_t name_len = strlen(username);
    if (needle_len > name_len) {
        return false;
    }

    for (size_t offset = 0U; offset + needle_len <= name_len; ++offset) {
        bool match = true;
        for (size_t idx = 0U; idx < needle_len; ++idx) {
            const unsigned char user_ch = (unsigned char)username[offset + idx];
            const unsigned char needle_ch = (unsigned char)needle[idx];
            if (tolower(user_ch) != tolower(needle_ch)) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }

    return false;
}

static void session_handle_search(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (ctx->owner == nullptr) {
        session_send_system_line(ctx, "Search is unavailable at the moment.");
        return;
    }

    if (arguments == nullptr) {
        session_send_system_line(ctx, "Usage: /search <text>");
        return;
    }

    char query[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(query, sizeof(query), "%s", arguments);
    trim_whitespace_inplace(query);

    if (query[0] == '\0') {
        session_send_system_line(ctx, "Usage: /search <text>");
        return;
    }

    char listing[SSH_CHATTER_MESSAGE_LIMIT];
    listing[0] = '\0';
    size_t match_count = 0U;

    ttak_mutex_lock(&ctx->owner->room.lock);
    for (size_t idx = 0U; idx < ctx->owner->room.member_count; ++idx) {
        session_ctx_t *member = ctx->owner->room.members[idx];
        if (member == nullptr) {
            continue;
        }
        if (!username_contains(member->user.name, query)) {
            continue;
        }

        char name[SSH_CHATTER_USERNAME_LEN];
        snprintf(name, sizeof(name), "%s", member->user.name);
        size_t current_len = strnlen(listing, sizeof(listing));
        size_t name_len = strnlen(name, sizeof(name));
        size_t prefix_len = (match_count == 0U) ? 0U : 2U;

        if (current_len + prefix_len + name_len >= sizeof(listing)) {
            continue;
        }

        if (match_count > 0U) {
            listing[current_len++] = ',';
            listing[current_len++] = ' ';
        }
        memcpy(listing + current_len, name, name_len);
        listing[current_len + name_len] = '\0';
        ++match_count;
    }
    ttak_mutex_unlock(&ctx->owner->room.lock);

    if (match_count == 0U) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        char display_query[64];
        size_t copy_len = strnlen(query, sizeof(display_query) - 1U);
        memcpy(display_query, query, copy_len);
        display_query[copy_len] = '\0';
        snprintf(message, sizeof(message), "No users matching '%s'.",
                 display_query);
        session_send_system_line(ctx, message);
        return;
    }

    char header[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(header, sizeof(header), "Matching users (%zu):", match_count);
    session_send_system_line(ctx, header);
    session_send_system_line(ctx, listing);
}

static bool session_output_os_is_windows(const session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->os_name[0] == '\0') {
        return false;
    }

    return strcasecmp(ctx->os_name, "windows") == 0;
}

static bool session_output_prefers_crlf(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }

    switch (ctx->newline_mode) {
    case SESSION_NEWLINE_MODE_CRLF:
        return true;
    case SESSION_NEWLINE_MODE_LF:
        return false;
    case SESSION_NEWLINE_MODE_AUTO:
    default:
        return session_output_os_is_windows(ctx);
    }
}

static unsigned char *
session_normalize_output_newlines(const session_ctx_t *ctx, const void *data,
                                  size_t length, size_t *out_length)
{
    if (out_length == nullptr || data == nullptr || length == 0U) {
        return nullptr;
    }

    *out_length = length;
    const unsigned char *input = (const unsigned char *)data;
    const bool prefer_crlf = session_output_prefers_crlf(ctx);

    bool needs_change = false;
    for (size_t idx = 0U; idx < length; ++idx) {
        unsigned char ch = input[idx];
        if (prefer_crlf) {
            if (ch == '\n' && (idx == 0U || input[idx - 1U] != '\r')) {
                needs_change = true;
                break;
            }
        } else {
            if (ch == '\r') {
                needs_change = true;
                break;
            }
        }
    }

    if (!needs_change) {
        return nullptr;
    }

    unsigned char *buffer = (unsigned char *)sshc_gc_malloc(length * 2U + 1U);
    if (buffer == nullptr) {
        return nullptr;
    }

    size_t out = 0U;
    for (size_t idx = 0U; idx < length; ++idx) {
        unsigned char ch = input[idx];
        if (prefer_crlf) {
            if (ch == '\n' && (idx == 0U || input[idx - 1U] != '\r')) {
                buffer[out++] = '\r';
                buffer[out++] = '\n';
            } else {
                buffer[out++] = ch;
            }
            continue;
        }

        if (ch == '\r') {
            if (idx + 1U < length && input[idx + 1U] == '\n') {
                buffer[out++] = '\n';
                ++idx;
            } else {
                buffer[out++] = '\n';
            }
            continue;
        }
        buffer[out++] = ch;
    }

    *out_length = out;
    return buffer;
}

void session_channel_write(session_ctx_t *ctx, const void *data, size_t length)
{
    if (ctx == nullptr || data == nullptr || length == 0U || ctx->should_exit ||
        !session_transport_active(ctx)) {
        return;
    }

    // If output buffering is enabled, append to buffer instead of writing directly
    if (ctx->output_buffering_enabled) {
        session_output_buffer_append(ctx, data, length);
        return;
    }

    bool locked = session_output_lock(ctx);

    bool success = true;
    bool channel_mutex_locked = false;
    if (ctx->channel_mutex_initialized) {
        int lock_result = ttak_mutex_lock(&ctx->channel_mutex);
        if (lock_result == 0) {
            channel_mutex_locked = true;
        } else {
            humanized_log_error("session", "failed to lock channel mutex",
                                lock_result);
        }
    }

    const bool use_retro_output =
        session_output_should_use_retro_encoding(ctx, ctx->output_kind);

    const void *write_data = data;
    size_t write_length = length;

    size_t newline_normalized_length = 0U;
    unsigned char *newline_normalized = session_normalize_output_newlines(
        ctx, write_data, write_length, &newline_normalized_length);
    if (newline_normalized != nullptr) {
        write_data = newline_normalized;
        write_length = newline_normalized_length;
    }

    size_t nul_count = 0U;
    const unsigned char *inspect = (const unsigned char *)write_data;
    for (size_t idx = 0U; idx < write_length; ++idx) {
        if (inspect[idx] == '\0') {
            ++nul_count;
        }
    }

    printf("[encoding-debug] phase=write_dispatch transport_kind=%d "
           "prefer_utf16_output=%d prefer_cp437_output=%d "
           "use_retro_output=%d output_kind=%d active_codepage=%d "
           "length=%zu nul_count=%zu\n",
           (int)ctx->transport_kind, (int)ctx->prefer_utf16_output,
           (int)ctx->prefer_cp437_output, (int)use_retro_output,
           (int)ctx->output_kind, (int)ctx->active_codepage, write_length,
           nul_count);

    unsigned char *sanitized = nullptr;
    if (!ctx->prefer_utf16_output && nul_count > 0U) {
        sanitized = (unsigned char *)sshc_gc_malloc(write_length);
        if (sanitized != nullptr) {
            size_t out = 0U;
            for (size_t idx = 0U; idx < write_length; ++idx) {
                if (inspect[idx] != '\0') {
                    sanitized[out++] = inspect[idx];
                }
            }
            write_data = sanitized;
            write_length = out;
        }
    }

    bool prefer_utf8_for_hybrid = false;
    if (ctx->hybrid_output_mode && use_retro_output &&
        ctx->output_kind != SESSION_OUTPUT_KIND_SYSTEM) {
        prefer_utf8_for_hybrid =
            session_output_requires_utf8((const char *)write_data, write_length);
    }

    if (use_retro_output && !prefer_utf8_for_hybrid) {
        /* Use the generic codepage conversion with the active codepage */
        success = session_channel_write_codepage(
            ctx, (const char *)write_data, write_length, ctx->active_codepage);
    } else if (ctx->prefer_utf16_output) {
        success =
            session_channel_write_utf16(ctx, (const char *)write_data, write_length);
    } else {
        success = session_channel_write_all(ctx, write_data, write_length);
    }

    if (sanitized != nullptr) {
        sshc_gc_free(sanitized);
    }
    if (newline_normalized != nullptr) {
        sshc_gc_free(newline_normalized);
    }

    if (channel_mutex_locked) {
        int unlock_result = ttak_mutex_unlock(&ctx->channel_mutex);
        if (unlock_result != 0) {
            humanized_log_error("session", "failed to unlock channel mutex",
                                unlock_result);
        }
    }

    if (!success) {
        ctx->should_exit = true;
    }

    if (locked) {
        session_output_unlock(ctx);
    }
}

static void session_handle_chat_lookup(session_ctx_t *ctx,
                                       const char *arguments)
{
    static const char *kUsage = "Usage: /chat <message-id>";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/chat", kUsage, usage, sizeof(usage));

    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (arguments == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    char working[64];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    uint64_t message_id = 0U;
    if (!host_compact_id_decode(working, &message_id) || message_id == 0U) {
        session_send_system_line(ctx, usage);
        return;
    }

    chat_history_entry_t entry = {0};
    if (!host_history_find_entry_by_id(ctx->owner, message_id, &entry)) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        char label[32];
        if (!host_compact_id_encode(message_id, label, sizeof(label))) {
            snprintf(label, sizeof(label), "%" PRIu64, message_id);
        }
        snprintf(message, sizeof(message), "Message #%s was not found.", label);
        session_send_system_line(ctx, message);
        return;
    }

    char header[SSH_CHATTER_MESSAGE_LIMIT];
    char header_label[32];
    if (!host_compact_id_encode(message_id, header_label,
                                sizeof(header_label))) {
        snprintf(header_label, sizeof(header_label), "%" PRIu64, message_id);
    }
    snprintf(header, sizeof(header), "Message #%s:", header_label);
    session_send_system_line(ctx, header);
    session_send_history_entry(ctx, &entry);
    session_send_reply_tree(ctx, entry.message_id, 0U, 1U);
}

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

    if (ctx->history_scroll_position > 0U ||
        (ctx->display_model_initialized && !display_model_is_following_tail(&ctx->display_model))) {
        // Do not force-render history when the user is scrolled back.
        return;
    }

    display_line_t previous_visible[SSH_CHATTER_SCROLLBACK_MAX_CHUNK];
    session_screen_line_t previous_lines[SSH_CHATTER_SCROLLBACK_MAX_CHUNK];
    size_t previous_count = 0U;
    const unsigned int viewport_height =
        (unsigned int)session_scrollback_line_capacity(ctx);
    const bool try_incremental = false;

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
        ctx->last_sink_history_total = 0U;
        ctx->pending_should_sink = false;
        return;
    }

    size_t visible_messages = session_scrollback_line_capacity(ctx);
    if (visible_messages == 0U) {
        visible_messages = 1U;
    }
    size_t start_index = (total > visible_messages) ? (total - visible_messages)
                                                    : 0U;
    size_t chunk = total - start_index;
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
        display_model_release_visible(&current_frame);
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
         * When incremental diffing is unavailable, append each newly
         * committed history line with the same renderer used by scrollback.
         */
        ctx->output_buffer_length = buffer_mark;
        for (size_t idx = 0U; idx < copied; ++idx) {
            session_send_history_entry(ctx, &buffer[idx]);
        }
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
    ctx->last_sink_history_total = total;

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

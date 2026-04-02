static void session_enable_alternate_screen(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    // Switch to alternate screen buffer and disable scrolling
    // \033[?1049h = enable alternate screen buffer (saves current screen)
    // \033[2J = clear screen
    // \033[H = move cursor to home position
    static const char kEnableAltScreen[] = "\033[?1049h\033[2J\033[H";
    session_channel_write(ctx, kEnableAltScreen, sizeof(kEnableAltScreen) - 1U);
}

static void session_disable_alternate_screen(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    // Switch back to main screen buffer (restores previous screen)
    // \033[?1049l = disable alternate screen buffer
    static const char kDisableAltScreen[] = "\033[?1049l";
    session_channel_write(ctx, kDisableAltScreen,
                          sizeof(kDisableAltScreen) - 1U);
}

static void session_clear_screen(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    static const char kClearSequence[] = "\033[2J\033[H";
    session_channel_write(ctx, kClearSequence, sizeof(kClearSequence) - 1U);

    ctx->output_lines_since_prompt = 0U;
    ctx->prompt_needs_padding = false;

    session_game_handle_screen_cleared(ctx);
}

static void session_bbs_prepare_canvas(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    session_clear_screen(ctx);
    session_apply_background_fill(ctx);
}

static void session_bbs_render_post(session_ctx_t *ctx, const bbs_post_t *post,
                                    const char *notice, bool reset_scroll)
{
    if (ctx == nullptr || post == nullptr) {
        return;
    }

    // Start buffering to send entire post in one flush
    session_output_buffer_start(ctx);

    session_bbs_prepare_canvas(ctx);

    if (reset_scroll) {
        ctx->bbs_view_scroll_offset = 0U;
    }

    char title_line[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(title_line, sizeof(title_line), "#%" PRIu64 ": %s", post->id,
             post->title);

    char author_line[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(author_line, sizeof(author_line), "Author: %s", post->author);

    char created_line[SSH_CHATTER_MESSAGE_LIMIT];
    time_t created = (time_t)post->created_at;
    struct tm created_tm;
    localtime_r(&created, &created_tm);
    strftime(created_line, sizeof(created_line), "Created: %Y-%m-%d %H:%M:%S",
             &created_tm);

    char bumped_line[SSH_CHATTER_MESSAGE_LIMIT];
    time_t bumped = (time_t)post->bumped_at;
    struct tm bumped_tm;
    localtime_r(&bumped, &bumped_tm);
    strftime(bumped_line, sizeof(bumped_line),
             "Last activity: %Y-%m-%d %H:%M:%S", &bumped_tm);

    session_send_plain_line(ctx, title_line);
    session_send_plain_line(ctx, author_line);
    session_send_plain_line(ctx, created_line);
    session_send_plain_line(ctx, bumped_line);
    session_render_separator(ctx, "{Body}");

    // Send body line by line (with BBS color markup expansion)
    session_send_bbs_body_text(ctx, post->body);

    // Send comments if any
    if (post->comment_count > 0U) {
        session_send_plain_line(ctx, ""); // Empty line for spacing
        session_render_separator(ctx, "Comments");
        for (size_t idx = 0U; idx < post->comment_count; ++idx) {
            const bbs_comment_t *comment = &post->comments[idx];
            char comment_author_line[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(comment_author_line, sizeof(comment_author_line), "Comment by: %s", comment->author);

            char comment_created_line[SSH_CHATTER_MESSAGE_LIMIT];
            time_t comment_created = (time_t)comment->created_at;
            struct tm comment_created_tm;
            localtime_r(&comment_created, &comment_created_tm);
            strftime(comment_created_line, sizeof(comment_created_line), "At: %Y-%m-%d %H:%M:%S", &comment_created_tm);

            session_send_system_line(ctx, comment_author_line);
            session_send_system_line(ctx, comment_created_line);
            session_send_bbs_body_text(ctx, comment->text);
            session_send_plain_line(ctx, ""); // Empty line for spacing between comments
        }
    }

    if (notice != nullptr && notice[0] != '\0') {
        session_send_system_line(ctx, notice);
    }

    session_render_prompt(ctx, true);

    // Flush all buffered output at once
    session_output_buffer_stop(ctx);
}

static void session_bbs_recalculate_line_count(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    size_t count = 0U;
    if (ctx->pending_bbs_body_length > 0U) {
        count = 1U;
        for (size_t idx = 0U; idx < ctx->pending_bbs_body_length; ++idx) {
            if (ctx->pending_bbs_body[idx] == '\n') {
                ++count;
            }
        }
    }

    ctx->pending_bbs_line_count = count;
    if (ctx->pending_bbs_cursor_line > count) {
        ctx->pending_bbs_cursor_line = count;
        ctx->pending_bbs_editing_line = false;
    }
}

static bool session_bbs_get_line_range(const session_ctx_t *ctx,
                                       size_t line_index, size_t *start,
                                       size_t *length)
{
    if (ctx == nullptr || start == nullptr || length == nullptr) {
        return false;
    }

    if (line_index >= ctx->pending_bbs_line_count) {
        return false;
    }

    size_t offset = 0U;
    size_t current = 0U;
    while (current < line_index && offset < ctx->pending_bbs_body_length) {
        const char *newline = memchr(ctx->pending_bbs_body + offset, '\n',
                                     ctx->pending_bbs_body_length - offset);
        if (newline == nullptr) {
            return false;
        }
        offset = (size_t)(newline - ctx->pending_bbs_body) + 1U;
        ++current;
    }

    if (offset > ctx->pending_bbs_body_length) {
        return false;
    }

    size_t end = ctx->pending_bbs_body_length;
    const char *newline = memchr(ctx->pending_bbs_body + offset, '\n',
                                 ctx->pending_bbs_body_length - offset);
    if (newline != nullptr) {
        end = (size_t)(newline - ctx->pending_bbs_body);
    }

    *start = offset;
    *length = end - offset;
    return true;
}

static void session_bbs_copy_line(const session_ctx_t *ctx, size_t line_index,
                                  char *buffer, size_t length)
{
    if (buffer == nullptr || length == 0U) {
        return;
    }

    buffer[0] = '\0';
    size_t start = 0U;
    size_t line_length = 0U;
    if (!session_bbs_get_line_range(ctx, line_index, &start, &line_length)) {
        return;
    }

    if (line_length >= length) {
        line_length = length - 1U;
    }

    if (line_length > 0U) {
        memcpy(buffer, ctx->pending_bbs_body + start, line_length);
    }
    buffer[line_length] = '\0';
}

static size_t session_editor_body_capacity(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return 0U;
    }

    if (ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART) {
        return SSH_CHATTER_ASCIIART_BUFFER_LEN;
    }

    return SSH_CHATTER_BBS_BODY_LEN;
}

static size_t session_editor_max_lines(const session_ctx_t *ctx)
{
    if (ctx != nullptr) {
        if (ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART) {
            return SSH_CHATTER_ASCIIART_MAX_LINES;
        }
        if (ctx->editor_mode == SESSION_EDITOR_MODE_BBS_CREATE ||
            ctx->editor_mode == SESSION_EDITOR_MODE_BBS_EDIT) {
            return SSH_CHATTER_BBS_MAX_LINES;
        }
    }

    return SIZE_MAX;
}

static size_t session_bbs_editor_window(const session_ctx_t *ctx,
                                        bool ascii_mode)
{
    size_t window = SSH_CHATTER_BBS_VIEW_WINDOW;
    if (window == 0U) {
        window = 1U;
    }

    if (ctx == nullptr || ctx->terminal_height == 0U) {
        return window;
    }

    size_t reserved = ascii_mode ? 10U : 11U;
    reserved += 1U; // Prompt line.

    if (ctx->terminal_height <= reserved) {
        return 1U;
    }

    size_t available = ctx->terminal_height - reserved;
    if (available == 0U) {
        available = 1U;
    }

    if (available < window) {
        window = available;
    }

    return window > 0U ? window : 1U;
}

static void session_bbs_set_cursor(session_ctx_t *ctx, size_t target,
                                   bool editing)
{
    if (ctx == nullptr) {
        return;
    }

    session_bbs_recalculate_line_count(ctx);
    size_t line_count = ctx->pending_bbs_line_count;

    if (target > line_count) {
        target = line_count;
    }

    if (editing && target >= line_count) {
        editing = false;
        target = line_count;
    }

    ctx->pending_bbs_cursor_line = target;
    ctx->pending_bbs_editing_line = editing;

    if (editing && target < line_count) {
        char line_buffer[SSH_CHATTER_MAX_INPUT_LEN];
        session_bbs_copy_line(ctx, target, line_buffer, sizeof(line_buffer));
        session_set_input_text(ctx, line_buffer);
        return;
    }

    session_set_input_text(ctx, "");
}

static void session_bbs_adjust_editor_scroll(session_ctx_t *ctx,
                                             size_t line_count, size_t window)
{
    if (ctx == nullptr) {
        return;
    }

    if (window == 0U) {
        window = 1U;
    }

    bool include_insertion_line =
        !ctx->pending_bbs_editing_line &&
        ctx->pending_bbs_cursor_line >= line_count;
    size_t effective_line_count = line_count;
    if (include_insertion_line && line_count < SIZE_MAX) {
        ++effective_line_count;
    }

    if (effective_line_count <= window) {
        ctx->bbs_editor_scroll_offset = 0U;
        return;
    }

    size_t max_offset = effective_line_count - window;
    size_t offset = ctx->bbs_editor_scroll_offset;
    size_t cursor = ctx->pending_bbs_cursor_line;

    if (cursor >= effective_line_count && effective_line_count > 0U) {
        cursor = effective_line_count - 1U;
    }

    if (cursor < offset) {
        offset = cursor;
    } else if (cursor >= offset + window) {
        offset = cursor - window + 1U;
    }

    if (offset > max_offset) {
        offset = max_offset;
    }

    ctx->bbs_editor_scroll_offset = offset;
}

static bool session_bbs_append_line(session_ctx_t *ctx, const char *line,
                                    char *status, size_t status_length)
{
    if (ctx == nullptr) {
        return false;
    }

    const bool ascii_mode = ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART;
    if (status != nullptr && status_length > 0U) {
        status[0] = '\0';
    }

    if (line == nullptr) {
        line = "";
    }

    const char *length_limit_message =
        ascii_mode ? "ASCII art buffer is full. Additional text ignored."
                   : "Post body length limit reached. Additional text ignored.";
    const char *line_limit_message =
        ascii_mode ? "ASCII art line limit reached. Additional text ignored."
                   : "Post line limit reached. Additional text ignored.";
    const char *line_truncated_message =
        ascii_mode ? "Line truncated to fit within the ASCII art size limit."
                   : "Line truncated to fit within the post size limit.";

    size_t capacity = session_editor_body_capacity(ctx);
    if (capacity == 0U) {
        if (status != nullptr && status_length > 0U) {
            snprintf(status, status_length, "%s", length_limit_message);
        }
        return false;
    }

    if (ctx->pending_bbs_body_length >= capacity - 1U) {
        if (status != nullptr && status_length > 0U) {
            snprintf(status, status_length, "%s", length_limit_message);
        }
        return false;
    }

    session_bbs_recalculate_line_count(ctx);
    size_t max_lines = session_editor_max_lines(ctx);
    if (ctx->pending_bbs_line_count >= max_lines) {
        if (status != nullptr && status_length > 0U) {
            snprintf(status, status_length, "%s", line_limit_message);
        }
        return false;
    }

    size_t available = capacity - ctx->pending_bbs_body_length - 1U;
    if (available == 0U) {
        if (status != nullptr && status_length > 0U) {
            snprintf(status, status_length, "%s", length_limit_message);
        }
        return false;
    }

    bool needs_newline = ctx->pending_bbs_body_length > 0U;
    if (needs_newline) {
        if (available == 0U) {
            if (status != nullptr && status_length > 0U) {
                snprintf(status, status_length, "%s", length_limit_message);
            }
            return false;
        }
        ctx->pending_bbs_body[ctx->pending_bbs_body_length++] = '\n';
        --available;
    }

    size_t line_length = strlen(line);
    if (line_length > available) {
        line_length = available;
        if (status != nullptr && status_length > 0U) {
            snprintf(status, status_length, "%s", line_truncated_message);
        }
    }

    if (line_length > 0U) {
        memcpy(ctx->pending_bbs_body + ctx->pending_bbs_body_length, line,
               line_length);
        ctx->pending_bbs_body_length += line_length;
    }

    ctx->pending_bbs_body[ctx->pending_bbs_body_length] = '\0';
    session_bbs_recalculate_line_count(ctx);
    session_bbs_set_cursor(ctx, ctx->pending_bbs_line_count, false);
    return true;
}

static bool session_bbs_insert_line(session_ctx_t *ctx, size_t line_index,
                                    const char *line, char *status,
                                    size_t status_length)
{
    if (ctx == nullptr) {
        return false;
    }

    const bool ascii_mode = ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART;
    if (status != nullptr && status_length > 0U) {
        status[0] = '\0';
    }

    if (line == nullptr) {
        line = "";
    }

    const char *length_limit_message =
        ascii_mode ? "ASCII art buffer is full. Additional text ignored."
                   : "Post body length limit reached. Additional text ignored.";
    const char *line_limit_message =
        ascii_mode ? "ASCII art line limit reached. Additional text ignored."
                   : "Post line limit reached. Additional text ignored.";
    const char *line_truncated_message =
        ascii_mode ? "Line truncated to fit within the ASCII art size limit."
                   : "Line truncated to fit within the post size limit.";

    session_bbs_recalculate_line_count(ctx);
    size_t line_count = ctx->pending_bbs_line_count;
    size_t max_lines = session_editor_max_lines(ctx);
    if (line_count >= max_lines) {
        if (status != nullptr && status_length > 0U) {
            snprintf(status, status_length, "%s", line_limit_message);
        }
        return false;
    }

    if (line_index > line_count) {
        line_index = line_count;
    }

    size_t capacity = session_editor_body_capacity(ctx);
    if (capacity == 0U) {
        if (status != nullptr && status_length > 0U) {
            snprintf(status, status_length, "%s", length_limit_message);
        }
        return false;
    }

    size_t current_length = ctx->pending_bbs_body_length;
    size_t available = capacity - current_length - 1U;
    if (available == 0U) {
        if (status != nullptr && status_length > 0U) {
            snprintf(status, status_length, "%s", length_limit_message);
        }
        return false;
    }

    size_t insert_offset = current_length;
    if (line_index < line_count) {
        size_t start = 0U;
        size_t length = 0U;
        if (!session_bbs_get_line_range(ctx, line_index, &start, &length)) {
            if (status != nullptr && status_length > 0U) {
                snprintf(status, status_length,
                         "Unable to locate the selected line.");
            }
            return false;
        }
        insert_offset = start;
    }

    bool insert_before_existing = line_index < line_count;
    bool insert_needs_prefix = (line_index == line_count && current_length > 0U);
    bool insert_needs_suffix = insert_before_existing;

    size_t line_length = strlen(line);
    size_t required = line_length +
                      (insert_needs_prefix ? 1U : 0U) +
                      (insert_needs_suffix ? 1U : 0U);
    if (required > available) {
        size_t overhead =
            (insert_needs_prefix ? 1U : 0U) + (insert_needs_suffix ? 1U : 0U);
        if (available <= overhead) {
            if (status != nullptr && status_length > 0U) {
                snprintf(status, status_length, "%s", length_limit_message);
            }
            return false;
        }
        line_length = available - overhead;
        if (status != nullptr && status_length > 0U) {
            snprintf(status, status_length, "%s", line_truncated_message);
        }
    }

    size_t insert_length = line_length +
                           (insert_needs_prefix ? 1U : 0U) +
                           (insert_needs_suffix ? 1U : 0U);
    size_t tail_bytes = current_length - insert_offset + 1U;
    memmove(ctx->pending_bbs_body + insert_offset + insert_length,
            ctx->pending_bbs_body + insert_offset, tail_bytes);

    size_t write_offset = insert_offset;
    if (insert_needs_prefix) {
        ctx->pending_bbs_body[write_offset++] = '\n';
    }
    if (line_length > 0U) {
        memcpy(ctx->pending_bbs_body + write_offset, line, line_length);
        write_offset += line_length;
    }
    if (insert_needs_suffix) {
        ctx->pending_bbs_body[write_offset++] = '\n';
    }

    ctx->pending_bbs_body_length = current_length + insert_length;
    ctx->pending_bbs_body[ctx->pending_bbs_body_length] = '\0';
    session_bbs_recalculate_line_count(ctx);
    return true;
}

/* Commit the current input buffer to the body for the cursor line without
 * changing the cursor position.  Used by navigation to auto-save inline edits. */
static void session_bbs_commit_edit_in_place(session_ctx_t *ctx)
{
    if (ctx == nullptr || !ctx->pending_bbs_editing_line) {
        return;
    }

    session_bbs_recalculate_line_count(ctx);
    const size_t line_index = ctx->pending_bbs_cursor_line;
    if (line_index >= ctx->pending_bbs_line_count) {
        return;
    }

    ctx->input_buffer[ctx->input_length] = '\0';

    size_t start = 0U;
    size_t old_length = 0U;
    if (!session_bbs_get_line_range(ctx, line_index, &start, &old_length)) {
        return;
    }

    size_t capacity = session_editor_body_capacity(ctx);
    if (capacity > 0U) {
        --capacity;
    }
    const size_t current_length = ctx->pending_bbs_body_length;
    const size_t base_length = current_length - old_length;
    const size_t max_allowed =
        (capacity > base_length) ? (capacity - base_length) : 0U;

    size_t new_length = ctx->input_length;
    if (new_length > max_allowed) {
        new_length = max_allowed;
    }

    const size_t tail_offset = start + old_length;
    const size_t tail_bytes = current_length - tail_offset + 1U;

    if (new_length > old_length) {
        memmove(ctx->pending_bbs_body + tail_offset + (new_length - old_length),
                ctx->pending_bbs_body + tail_offset, tail_bytes);
    } else if (old_length > new_length) {
        /* new_tail_offset: adjusted destination after line shrinkage */
        const size_t new_tail_offset = tail_offset - (old_length - new_length);
        memmove(ctx->pending_bbs_body + new_tail_offset,
                ctx->pending_bbs_body + tail_offset, tail_bytes);
    }

    if (new_length > 0U) {
        memcpy(ctx->pending_bbs_body + start, ctx->input_buffer, new_length);
    }

    ctx->pending_bbs_body_length = base_length + new_length;
    ctx->pending_bbs_body[ctx->pending_bbs_body_length] = '\0';
    session_bbs_recalculate_line_count(ctx);
}

static bool session_bbs_replace_line(session_ctx_t *ctx, size_t line_index,
                                     const char *line, char *status,
                                     size_t status_length)
{
    if (ctx == nullptr || line == nullptr) {
        return false;
    }

    const bool ascii_mode = ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART;
    if (status != nullptr && status_length > 0U) {
        status[0] = '\0';
    }

    session_bbs_recalculate_line_count(ctx);
    if (line_index >= ctx->pending_bbs_line_count) {
        if (status != nullptr && status_length > 0U) {
            snprintf(status, status_length,
                     "Unable to locate the selected line.");
        }
        return false;
    }

    size_t start = 0U;
    size_t old_length = 0U;
    if (!session_bbs_get_line_range(ctx, line_index, &start, &old_length)) {
        if (status != nullptr && status_length > 0U) {
            snprintf(status, status_length,
                     "Unable to locate the selected line.");
        }
        return false;
    }

    size_t current_length = ctx->pending_bbs_body_length;
    size_t capacity = session_editor_body_capacity(ctx);
    if (capacity == 0U) {
        return false;
    }
    if (capacity > 0U) {
        --capacity;
    }
    size_t base_length = current_length - old_length;
    size_t max_allowed =
        (capacity > base_length) ? (capacity - base_length) : 0U;

    size_t new_length = strlen(line);
    if (new_length > max_allowed) {
        new_length = max_allowed;
        if (status != nullptr && status_length > 0U) {
            const char *line_truncated_message =
                ascii_mode
                    ? "Line truncated to fit within the ASCII art size limit."
                    : "Line truncated to fit within the post size limit.";
            snprintf(status, status_length, "%s", line_truncated_message);
        }
    }

    size_t tail_offset = start + old_length;
    size_t tail_bytes = current_length - tail_offset + 1U;

    if (new_length > old_length) {
        size_t shift = new_length - old_length;
        memmove(ctx->pending_bbs_body + tail_offset + shift,
                ctx->pending_bbs_body + tail_offset, tail_bytes);
    } else if (old_length > new_length) {
        size_t shift = old_length - new_length;
        memmove(ctx->pending_bbs_body + tail_offset - shift,
                ctx->pending_bbs_body + tail_offset, tail_bytes);
        tail_offset -= shift;
    }

    if (new_length > 0U) {
        memcpy(ctx->pending_bbs_body + start, line, new_length);
    }

    ctx->pending_bbs_body_length = base_length + new_length;
    ctx->pending_bbs_body[ctx->pending_bbs_body_length] = '\0';

    session_bbs_recalculate_line_count(ctx);
    size_t updated_count = ctx->pending_bbs_line_count;
    if (line_index + 1U <= updated_count) {
        session_bbs_set_cursor(ctx, line_index + 1U, false);
    } else {
        session_bbs_set_cursor(ctx, updated_count, false);
    }
    return true;
}

static bool session_bbs_copy_line_range(const session_ctx_t *ctx, size_t start,
                                        size_t end, char *buffer,
                                        size_t length, size_t *lines_out)
{
    if (buffer == nullptr || length == 0U || ctx == nullptr) {
        return false;
    }

    buffer[0] = '\0';
    if (lines_out != nullptr) {
        *lines_out = 0U;
    }

    session_ctx_t *mutable_ctx = (session_ctx_t *)ctx;
    session_bbs_recalculate_line_count(mutable_ctx);
    size_t line_count = ctx->pending_bbs_line_count;
    if (line_count == 0U || start >= line_count || end >= line_count) {
        return false;
    }

    if (start > end) {
        size_t tmp = start;
        start = end;
        end = tmp;
    }

    size_t write_offset = 0U;
    size_t copied_lines = 0U;
    for (size_t idx = start; idx <= end; ++idx) {
        char line_buffer[SSH_CHATTER_MESSAGE_LIMIT];
        session_bbs_copy_line(ctx, idx, line_buffer, sizeof(line_buffer));
        size_t line_len = strlen(line_buffer);
        size_t needed = line_len + (copied_lines > 0U ? 1U : 0U);
        if (write_offset + needed >= length) {
            break;
        }
        if (copied_lines > 0U) {
            buffer[write_offset++] = '\n';
        }
        if (line_len > 0U) {
            memcpy(buffer + write_offset, line_buffer, line_len);
            write_offset += line_len;
        }
        ++copied_lines;
    }
    buffer[write_offset] = '\0';
    if (lines_out != nullptr) {
        *lines_out = copied_lines;
    }
    return copied_lines > 0U;
}

static bool session_bbs_remove_line_range(session_ctx_t *ctx, size_t start,
                                          size_t end, char *status,
                                          size_t status_length)
{
    if (ctx == nullptr) {
        return false;
    }

    if (status != nullptr && status_length > 0U) {
        status[0] = '\0';
    }

    session_bbs_recalculate_line_count(ctx);
    size_t line_count = ctx->pending_bbs_line_count;
    if (line_count == 0U || start >= line_count || end >= line_count) {
        if (status != nullptr && status_length > 0U) {
            snprintf(status, status_length, "No lines selected.");
        }
        return false;
    }

    if (start > end) {
        size_t tmp = start;
        start = end;
        end = tmp;
    }

    size_t start_offset = 0U;
    size_t start_length = 0U;
    if (!session_bbs_get_line_range(ctx, start, &start_offset, &start_length)) {
        return false;
    }
    size_t end_offset = 0U;
    size_t end_length = 0U;
    if (!session_bbs_get_line_range(ctx, end, &end_offset, &end_length)) {
        return false;
    }

    size_t remove_start = start_offset;
    size_t remove_end = end_offset + end_length;
    if (end < line_count - 1U) {
        remove_end += 1U;
    } else if (start > 0U) {
        remove_start -= 1U;
    }

    size_t current_length = ctx->pending_bbs_body_length;
    if (remove_end > current_length) {
        remove_end = current_length;
    }

    size_t tail_bytes = current_length - remove_end + 1U;
    memmove(ctx->pending_bbs_body + remove_start,
            ctx->pending_bbs_body + remove_end, tail_bytes);

    ctx->pending_bbs_body_length =
        current_length - (remove_end - remove_start);
    ctx->pending_bbs_body[ctx->pending_bbs_body_length] = '\0';
    session_bbs_recalculate_line_count(ctx);
    return true;
}

static bool session_bbs_insert_clipboard(session_ctx_t *ctx, char *status,
                                         size_t status_length)
{
    if (ctx == nullptr || ctx->bbs_editor_clipboard_length == 0U) {
        if (status != nullptr && status_length > 0U) {
            snprintf(status, status_length, "Clipboard is empty.");
        }
        return false;
    }

    session_bbs_recalculate_line_count(ctx);
    size_t line_count = ctx->pending_bbs_line_count;
    size_t insert_index = ctx->pending_bbs_cursor_line;
    if (insert_index > line_count) {
        insert_index = line_count;
    }

    char clipboard_copy[SSH_CHATTER_BBS_BODY_LEN];
    snprintf(clipboard_copy, sizeof(clipboard_copy), "%s",
             ctx->bbs_editor_clipboard);

    char *cursor = clipboard_copy;
    bool inserted_any = false;
    while (cursor != nullptr && *cursor != '\0') {
        char *newline = strchr(cursor, '\n');
        if (newline != nullptr) {
            *newline = '\0';
        }
        if (!session_bbs_insert_line(ctx, insert_index, cursor, status,
                                     status_length)) {
            return inserted_any;
        }
        inserted_any = true;
        ++insert_index;
        if (newline == nullptr) {
            break;
        }
        cursor = newline + 1;
    }

    session_bbs_set_cursor(ctx, insert_index, false);
    return inserted_any;
}

static bool session_bbs_search_keyword(session_ctx_t *ctx, const char *keyword,
                                       char *status, size_t status_length)
{
    if (ctx == nullptr) {
        return false;
    }

    if (status != nullptr && status_length > 0U) {
        status[0] = '\0';
    }

    if (keyword == nullptr || keyword[0] == '\0') {
        if (status != nullptr && status_length > 0U) {
            snprintf(status, status_length, "Search canceled.");
        }
        session_bbs_set_cursor(ctx, ctx->bbs_search_restore_line,
                               ctx->bbs_search_restore_editing);
        ctx->bbs_editor_scroll_offset = ctx->bbs_search_restore_scroll;
        return false;
    }

    session_bbs_recalculate_line_count(ctx);
    size_t line_count = ctx->pending_bbs_line_count;
    if (line_count == 0U) {
        if (status != nullptr && status_length > 0U) {
            snprintf(status, status_length, "No lines to search.");
        }
        session_bbs_set_cursor(ctx, ctx->bbs_search_restore_line,
                               ctx->bbs_search_restore_editing);
        ctx->bbs_editor_scroll_offset = ctx->bbs_search_restore_scroll;
        return false;
    }

    size_t start_line = ctx->pending_bbs_cursor_line;
    if (start_line >= line_count) {
        start_line = 0U;
    }

    for (size_t pass = 0U; pass < 2U; ++pass) {
        size_t idx = (pass == 0U) ? start_line : 0U;
        size_t limit = (pass == 0U) ? line_count : start_line;
        for (; idx < limit; ++idx) {
            char line_buffer[SSH_CHATTER_MESSAGE_LIMIT];
            session_bbs_copy_line(ctx, idx, line_buffer,
                                  sizeof(line_buffer));
            if (strstr(line_buffer, keyword) != nullptr) {
                session_bbs_set_cursor(ctx, idx, true);
                if (status != nullptr && status_length > 0U) {
                    snprintf(status, status_length,
                             "Found \"%s\" on line %zu.", keyword, idx + 1U);
                }
                return true;
            }
        }
    }

    session_bbs_set_cursor(ctx, ctx->bbs_search_restore_line,
                           ctx->bbs_search_restore_editing);
    ctx->bbs_editor_scroll_offset = ctx->bbs_search_restore_scroll;
    if (status != nullptr && status_length > 0U) {
        snprintf(status, status_length, "No match for \"%s\".", keyword);
    }
    return false;
}

static void session_bbs_render_editor(session_ctx_t *ctx, const char *status)
{
    if (ctx == nullptr) {
        return;
    }

    // Start buffering to send entire editor screen in one flush
    session_output_buffer_start(ctx);

    const bool ascii_mode = ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART;
    const bool editing_post = ctx->editor_mode == SESSION_EDITOR_MODE_BBS_EDIT;

    ctx->bbs_view_active = false;
    ctx->bbs_view_post_id = 0U;
    ctx->bbs_rendering_editor = true;

    session_bbs_prepare_canvas(ctx);
    session_bbs_recalculate_line_count(ctx);
    size_t line_count = ctx->pending_bbs_line_count;
    size_t window = session_bbs_editor_window(ctx, ascii_mode);
    session_bbs_adjust_editor_scroll(ctx, line_count, window);

    // Send title line
    if (ascii_mode) {
        char title_line[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(title_line, sizeof(title_line),
                 "ASCII art draft (%zu/%u lines)", line_count,
                 (unsigned int)SSH_CHATTER_ASCIIART_MAX_LINES);
        session_send_plain_line(ctx, title_line);
    } else {
        char title_line[SSH_CHATTER_MESSAGE_LIMIT];
        if (editing_post) {
            if (ctx->pending_bbs_edit_id != 0U) {
                snprintf(title_line, sizeof(title_line),
                         "Editing post #%" PRIu64 " '%s'",
                         ctx->pending_bbs_edit_id, ctx->pending_bbs_title);
            } else {
                snprintf(title_line, sizeof(title_line), "Editing '%s'",
                         ctx->pending_bbs_title);
            }
        } else {
            snprintf(title_line, sizeof(title_line), "Composing '%s'",
                     ctx->pending_bbs_title);
        }
        session_send_plain_line(ctx, title_line);

        // Send tags line
        char tag_buffer[SSH_CHATTER_BBS_MAX_TAGS *
                        (SSH_CHATTER_BBS_TAG_LEN + 2U)];
        tag_buffer[0] = '\0';
        size_t offset = 0U;
        for (size_t idx = 0U; idx < ctx->pending_bbs_tag_count; ++idx) {
            size_t remaining = sizeof(tag_buffer) - offset;
            if (remaining == 0U) {
                break;
            }
            int written =
                snprintf(tag_buffer + offset, remaining, "%s%s",
                         idx > 0U ? "," : "", ctx->pending_bbs_tags[idx]);
            if (written < 0) {
                break;
            }
            if ((size_t)written >= remaining) {
                offset = sizeof(tag_buffer) - 1U;
                break;
            }
            offset += (size_t)written;
        }

        char tags_line[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(tags_line, sizeof(tags_line), "Tags: %s",
                 tag_buffer[0] != '\0' ? tag_buffer : "(none)");
        session_send_plain_line(ctx, tags_line);
    }

    // Send divider
    session_render_separator(ctx, "{Body}");

    // Send body lines individually
    if (line_count == 0U) {
        session_send_plain_line(ctx, "> ");
    } else {
        size_t start = ctx->bbs_editor_scroll_offset;
        size_t end = start + window;
        if (end > line_count) {
            end = line_count;
        }
        size_t insertion_index = ctx->pending_bbs_cursor_line;
        if (insertion_index > line_count) {
            insertion_index = line_count;
        }
        size_t selection_start = ctx->bbs_editor_selection_start;
        size_t selection_end = ctx->bbs_editor_selection_end;
        if (selection_start > selection_end) {
            size_t tmp = selection_start;
            selection_start = selection_end;
            selection_end = tmp;
        }
        bool selection_active = ctx->bbs_editor_selection_start_set &&
                                ctx->bbs_editor_selection_end_set;

        /* BBS_EDITOR_LINE_PREC: reserve 4 bytes for "> " (2), "_" (1), NUL (1) */
        enum { BBS_EDITOR_LINE_PREC = SSH_CHATTER_MESSAGE_LIMIT - 4 };
        for (size_t idx = start; idx < end; ++idx) {
            char line_buffer[SSH_CHATTER_MESSAGE_LIMIT];
            session_bbs_copy_line(ctx, idx, line_buffer, sizeof(line_buffer));
            bool cursor_selected = ctx->pending_bbs_editing_line &&
                                   ctx->pending_bbs_cursor_line == idx;
            bool cursor_highlight =
                !ctx->pending_bbs_editing_line && insertion_index == idx;
            bool range_selected =
                selection_active && idx >= selection_start &&
                idx <= selection_end;
            const char *prefix = cursor_selected ? "> "
                                  : cursor_highlight ? "> "
                                  : range_selected ? "* "
                                                   : "  ";
            char display[SSH_CHATTER_MESSAGE_LIMIT];
            if (cursor_selected) {
                /* Real-time inline edit: show the live input buffer so the
                 * user sees every keystroke; '_' marks the insertion point. */
                snprintf(display, sizeof(display), "> %.*s_",
                         BBS_EDITOR_LINE_PREC, ctx->input_buffer);
            } else if (line_buffer[0] == '\0') {
                snprintf(display, sizeof(display), "%s", prefix);
            } else {
                snprintf(display, sizeof(display), "%s%.*s", prefix,
                         BBS_EDITOR_LINE_PREC, line_buffer);
            }
            session_send_plain_line(ctx, display);
        }
        if (insertion_index >= end && insertion_index == line_count) {
            if (ctx->pending_bbs_editing_line) {
                char display[SSH_CHATTER_MESSAGE_LIMIT];
                snprintf(display, sizeof(display), "> %.*s_",
                         BBS_EDITOR_LINE_PREC, ctx->input_buffer);
                session_send_plain_line(ctx, display);
            } else {
                session_send_plain_line(ctx, "> ");
            }
        }
    }

    // Send end divider
    session_render_separator(ctx, "{End}");

    // Send remaining bytes info
    size_t capacity = session_editor_body_capacity(ctx);
    size_t remaining = 0U;
    if (capacity > ctx->pending_bbs_body_length) {
        remaining = capacity - ctx->pending_bbs_body_length - 1U;
    }
    char remaining_line[64];
    snprintf(remaining_line, sizeof(remaining_line), "Remaining bytes: %zu",
             remaining);
    session_send_plain_line(ctx, remaining_line);

    // Send hints
    const char *terminator = session_editor_terminator(ctx);
    char shortcut_hint[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(shortcut_hint, sizeof(shortcut_hint),
             "Ctrl+S(or Save) inserts %s. Ctrl+A(or Abort) cancels the draft.",
             terminator);
    session_send_plain_line(ctx, shortcut_hint);
    session_send_plain_line(ctx, "Ctrl+O inserts the current input at the "
                                 "cursor. Ctrl+F searches within the draft.");
    session_send_plain_line(ctx, "Ctrl+M starts explicit line edit mode; "
                                 "Ctrl+L applies the edited line.");
    session_send_plain_line(ctx, "Ctrl+1 marks a selection start, Ctrl+2 cuts "
                                 "to a selection end, Ctrl+3 pastes.");
    session_send_plain_line(ctx, "Searching returns to the editor and moves the "
                                 "cursor to the first match.");
    session_send_plain_line(ctx, "Up/Down arrows move between lines; edits are "
                                 "saved automatically on navigation.");

    char publish_hint[SSH_CHATTER_MESSAGE_LIMIT];
    if (ascii_mode) {
        snprintf(publish_hint, sizeof(publish_hint),
                 "Typing %s on its own line will finish the artwork.",
                 terminator);
    } else if (editing_post) {
        snprintf(publish_hint, sizeof(publish_hint),
                 "Typing %s on its own line will update the post.", terminator);
    } else {
        snprintf(publish_hint, sizeof(publish_hint),
                 "Typing %s on its own line will publish the post.",
                 terminator);
    }
    session_send_plain_line(ctx, publish_hint);

    // Send status if any
    if (status != nullptr && status[0] != '\0') {
        char working[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(working, sizeof(working), "%s", status);
        char *cursor = working;
        while (cursor != nullptr && *cursor != '\0') {
            char *newline = strchr(cursor, '\n');
            if (newline != nullptr) {
                *newline = '\0';
            }
            if (*cursor != '\0') {
                session_send_plain_line(ctx, cursor);
            }
            if (newline == nullptr) {
                break;
            }
            cursor = newline + 1;
        }
    }

    session_render_prompt(ctx, false);

    // Flush all buffered output at once
    session_output_buffer_stop(ctx);

    ctx->bbs_rendering_editor = false;
}

static void session_bbs_move_cursor(session_ctx_t *ctx, int direction)
{
    if (ctx == nullptr || direction == 0) {
        return;
    }
    ctx->bbs_line_edit_mode = false;

    /* Auto-save the current inline edit before moving the cursor. */
    if (ctx->pending_bbs_editing_line) {
        session_bbs_commit_edit_in_place(ctx);
    }

    session_bbs_recalculate_line_count(ctx);
    size_t line_count = ctx->pending_bbs_line_count;

    if (line_count == 0U) {
        session_bbs_set_cursor(ctx, 0U, false);
        session_bbs_render_editor(ctx, nullptr);
        return;
    }

    size_t target = ctx->pending_bbs_cursor_line;
    bool editing = ctx->pending_bbs_editing_line;
    if (target > line_count) {
        target = line_count;
    }

    if (direction < 0) {
        if (editing) {
            if (target > 0U) {
                --target;
            }
        } else if (line_count > 0U) {
            if (target > line_count) {
                target = line_count;
            }
            if (target > 0U) {
                --target;
            } else {
                target = 0U;
            }
            editing = true;
        }
    } else {
        if (editing) {
            if (target + 1U < line_count) {
                ++target;
            } else {
                target = line_count;
                editing = false;
            }
        } else if (target < line_count) {
            editing = true;
        }
    }

    char status[64];
    status[0] = '\0';

    session_bbs_set_cursor(ctx, target, editing);
    if (editing && target < line_count) {
        snprintf(status, sizeof(status), "Editing line %zu of %zu.",
                 target + 1U, line_count);
    } else {
        snprintf(status, sizeof(status), "Editing new line %zu.",
                 line_count + 1U);
    }

    session_bbs_render_editor(ctx, status);
}

void session_render_banner_ascii(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    const char *banner_text = "Welcome to CHATTER!";
    if (ctx->owner != nullptr && ctx->owner->welcome_banner_loaded &&
        ctx->owner->welcome_banner[0] != '\0') {
        banner_text = ctx->owner->welcome_banner;
    }

    session_render_banner_text(ctx, banner_text);
}

static void session_render_prelogin_banner(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->prelogin_banner_rendered) {
        return;
    }

    session_apply_background_fill(ctx);

    session_render_banner_ascii(ctx);

    session_send_plain_line(ctx, "Connection established.");
    session_send_plain_line(
        ctx, "[0] <sysop> TELNET connection is unstable. Please use SSH");
    session_send_plain_line(ctx,
                            "Authenticate or choose a nickname to continue.");
    session_send_plain_line(ctx, "/retro on for CP-437 DOS compatibility.");

    ctx->prelogin_banner_rendered = true;
}

static void session_render_banner(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    session_apply_background_fill(ctx);

    bool show_graphics = !ctx->prelogin_banner_rendered;
    if (show_graphics) {
        session_render_banner_ascii(ctx);
    }

    session_render_separator(ctx, "Chatroom");
}

static __attribute__((unused)) void
session_pad_prompt_to_terminal(session_ctx_t *ctx, bool include_separator)
{
    if (ctx == nullptr || !ctx->prompt_needs_padding) {
        return;
    }

    unsigned int height = ctx->terminal_height;
    if (height == 0U) {
        return;
    }

    unsigned int reserved = 1U + (include_separator ? 1U : 0U);
    if (height <= reserved) {
        return;
    }

    unsigned int max_content = height - reserved;
    unsigned int used = ctx->output_lines_since_prompt;
    if (used >= max_content) {
        ctx->output_lines_since_prompt = max_content;
        return;
    }

    unsigned int blanks = max_content - used;
    for (unsigned int idx = 0U; idx < blanks; ++idx) {
        session_fill_line_with_theme(ctx);
        session_channel_write_line_ending(ctx);
    }

    session_note_output_lines(ctx, blanks);
}

static __attribute__((unused)) void session_fill_prompt_line(session_ctx_t *ctx)
{
    const char *bg = ctx->system_bg_code != nullptr ? ctx->system_bg_code : "";
    const size_t bg_len = strlen(bg);
    unsigned int width = ctx->terminal_width > 0U ? ctx->terminal_width : 80U;
    if (width > SSH_CHATTER_MESSAGE_LIMIT) {
        width = SSH_CHATTER_MESSAGE_LIMIT;
    }

    static const char column_reset[] = "\033[1G";
    session_channel_write(ctx, column_reset, sizeof(column_reset) - 1U);
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

    session_channel_write(ctx, column_reset, sizeof(column_reset) - 1U);
    if (bg_len > 0U) {
        session_channel_write(ctx, bg, bg_len);
    }
}

static void session_render_prompt_internal(session_ctx_t *ctx,
                                           bool include_separator,
                                           bool fill_line)
{
    (void)include_separator;
    (void)fill_line;

    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    ctx->output_lines_since_prompt = 0U;
    ctx->prompt_needs_padding = false;
}

static void session_render_prompt(session_ctx_t *ctx, bool include_separator)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    bool locked = session_output_lock(ctx);
    session_render_prompt_internal(ctx, include_separator, true);
    if (locked) {
        session_output_unlock(ctx);
    }
}

static void session_refresh_input_line(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    bool locked = session_output_lock(ctx);
    session_render_prompt_internal(ctx, false, true);

    // For telnet, ensure the prompt is immediately visible by flushing the socket
    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        session_channel_flush(ctx);
    }

    fflush(stdout);

    if (locked) {
        session_output_unlock(ctx);
    }
}

static void session_set_input_text(session_ctx_t *ctx, const char *text)
{
    if (ctx == nullptr) {
        return;
    }

    ctx->input_length = 0U;
    memset(ctx->input_buffer, 0, sizeof(ctx->input_buffer));

    if (text != nullptr && text[0] != '\0') {
        const size_t len = strnlen(text, sizeof(ctx->input_buffer) - 1U);
        memcpy(ctx->input_buffer, text, len);
        ctx->input_buffer[len] = '\0';
        ctx->input_length = len;
    }

    session_refresh_input_line(ctx);
}

static void session_local_echo_char(session_ctx_t *ctx, char ch)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    if (ch == '\r' || ch == '\n') {
        session_channel_write_line_ending(ctx);
        return;
    }

    session_channel_write(ctx, &ch, 1U);
}

static size_t session_utf8_prev_char_len(const char *buffer, size_t length)
{
    if (buffer == nullptr || length == 0U) {
        return 0U;
    }

    size_t idx = length;
    while (idx > 0U) {
        --idx;
        const unsigned char byte = (unsigned char)buffer[idx];
        if ((byte & 0xC0U) != 0x80U) {
            const size_t seq_len = length - idx;
            size_t expected = 1U;
            if ((byte & 0x80U) == 0U) {
                expected = 1U;
            } else if ((byte & 0xE0U) == 0xC0U) {
                expected = 2U;
            } else if ((byte & 0xF0U) == 0xE0U) {
                expected = 3U;
            } else if ((byte & 0xF8U) == 0xF0U) {
                expected = 4U;
            } else {
                expected = 1U;
            }

            if (seq_len < expected) {
                return seq_len;
            }
            return expected;
        }
    }

    return 1U;
}

static int session_utf8_char_width(const char *bytes, size_t length)
{
    if (bytes == nullptr || length == 0U) {
        return 0;
    }

    mbstate_t state;
    memset(&state, 0, sizeof(state));

    wchar_t wc;
    const size_t result = mbrtowc(&wc, bytes, length, &state);
    if (result == (size_t)-1 || result == (size_t)-2) {
        return 1;
    }

    const int width = wcwidth(wc);
    if (width < 0) {
        return 1;
    }

    return width;
}

static void session_local_backspace(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx) ||
        ctx->input_length == 0U) {
        return;
    }

    const size_t char_len =
        session_utf8_prev_char_len(ctx->input_buffer, ctx->input_length);
    if (char_len == 0U || char_len > ctx->input_length) {
        return;
    }

    const size_t char_start = ctx->input_length - char_len;
    const int display_width =
        session_utf8_char_width(&ctx->input_buffer[char_start], char_len);

    ctx->input_length = char_start;
    ctx->input_buffer[ctx->input_length] = '\0';

    const int width = display_width > 0 ? display_width : 1;
    const char sequence[] = "\b \b";
    for (int idx = 0; idx < width; ++idx) {
        session_channel_write(ctx, sequence, sizeof(sequence) - 1U);
    }
}

static void session_clear_input_base(session_ctx_t *ctx, bool render_prompt)
{
    if (ctx == nullptr) {
        return;
    }

    ctx->input_length = 0U;
    memset(ctx->input_buffer, 0, sizeof(ctx->input_buffer));
    ctx->input_history_position = -1;
    ctx->input_escape_active = false;
    ctx->input_escape_length = 0U;

    if (ctx->bracket_paste_active) {
        return;
    }

    if (render_prompt) {
        session_refresh_input_line(ctx);
        return;
    }

    if (!session_transport_active(ctx)) {
        return;
    }

    bool locked = session_output_lock(ctx);
    const char *bg = ctx->system_bg_code != nullptr ? ctx->system_bg_code : "";
    if (bg[0] != '\0') {
        session_channel_write(ctx, bg, strlen(bg));
    }

    static const char clear_sequence[] = "\r" ANSI_CLEAR_LINE;
    session_channel_write(ctx, clear_sequence, sizeof(clear_sequence) - 1U);

    if (bg[0] != '\0') {
        session_channel_write(ctx, bg, strlen(bg));
    }

    if (locked) {
        session_output_unlock(ctx);
    }
}

static void session_clear_input(session_ctx_t *ctx)
{
    session_clear_input_base(ctx, true);
}

static void session_clear_input_without_prompt(session_ctx_t *ctx)
{
    session_clear_input_base(ctx, false);
}

// SLASH_COMPATIBLE: Helper function to check if a character is slash-compatible

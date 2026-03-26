/**
 * @file host_session_output.c
 * @desc File-level documentation for host_session_output.c, describing its role
 *       in the SSH-Chatter server and providing a consistent header
 *       comment format across C sources.
 * @return None.
 */

#include "../host_internal.h"
#include "ssh_chatter/user_data.h"
#include "ssh_chatter/security_layer.h"
#include "ssh_chatter/palettes.h"

// Session output, history delivery, and client-facing helpers.

#define SESSION_DEFAULT_TERMINAL_HEIGHT 24U

void session_process_pending_sink(session_ctx_t *ctx);
void session_flag_should_sink(session_ctx_t *ctx);
static void session_render_history_entry(session_ctx_t *ctx,
                                         const chat_history_entry_t *entry,
                                         bool emit_output);

typedef struct session_screen_line {
    const char *text;
    size_t length;
} session_screen_line_t;

static size_t session_skip_ansi_sequence(const char *text, size_t length)
{
    if (text == nullptr || length == 0U || text[0] != '\x1b') {
        return 0U;
    }

    size_t idx = 1U;
    if (idx < length && text[idx] == '[') {
        ++idx;
        while (idx < length) {
            unsigned char ch = (unsigned char)text[idx];
            ++idx;
            if (ch >= '@' && ch <= '~') {
                break;
            }
        }
        return idx;
    }

    if (idx < length && text[idx] == ']') {
        ++idx;
        while (idx < length) {
            unsigned char ch = (unsigned char)text[idx];
            ++idx;
            if (ch == '\a') {
                break;
            }
            if (ch == '\x1b') {
                ++idx;
                break;
            }
        }
        return idx;
    }

    while (idx < length) {
        unsigned char ch = (unsigned char)text[idx];
        ++idx;
        if (ch >= '@' && ch <= '~') {
            break;
        }
    }
    return idx;
}

static size_t session_count_visible_columns(const char *text, size_t length)
{
    if (text == nullptr || length == 0U) {
        return 0U;
    }

    size_t columns = 0U;
    size_t idx = 0U;
    while (idx < length) {
        unsigned char ch = (unsigned char)text[idx];
        if (ch == '\x1b') {
            size_t consumed = session_skip_ansi_sequence(text + idx, length - idx);
            if (consumed == 0U) {
                ++idx;
            } else {
                idx += consumed;
            }
            continue;
        }

        if (ch < 0x20U) {
            ++idx;
            continue;
        }

        if ((ch & 0x80U) == 0U) {
            ++columns;
            ++idx;
            continue;
        }

        size_t advance = 1U;
        if ((ch & 0xE0U) == 0xC0U && idx + 1U < length) {
            advance = 2U;
        } else if ((ch & 0xF0U) == 0xE0U && idx + 2U < length) {
            advance = 3U;
        } else if ((ch & 0xF8U) == 0xF0U && idx + 3U < length) {
            advance = 4U;
        }
        idx += advance;
        ++columns;
    }

    return columns;
}

static size_t session_estimate_line_rows(const session_screen_line_t *line,
                                         unsigned int terminal_width)
{
    if (terminal_width == 0U) {
        terminal_width = 80U;
    }
    if (terminal_width == 0U) {
        terminal_width = 1U;
    }

    if (line == nullptr || line->text == nullptr) {
        return 1U;
    }

    size_t visible = session_count_visible_columns(line->text, line->length);
    if (visible == 0U) {
        return 1U;
    }

    size_t rows = (visible + terminal_width - 1U) / terminal_width;
    return rows > 0U ? rows : 1U;
}

static size_t session_build_row_offsets(const session_screen_line_t *lines,
                                        size_t count, unsigned int terminal_width,
                                        size_t *offsets, size_t offsets_capacity)
{
    if (offsets == nullptr || offsets_capacity == 0U) {
        return 0U;
    }

    offsets[0] = 0U;
    if (lines == nullptr || count == 0U) {
        return 0U;
    }

    if (count + 1U > offsets_capacity) {
        count = offsets_capacity - 1U;
    }

    size_t total = 0U;
    for (size_t idx = 0U; idx < count; ++idx) {
        size_t rows = session_estimate_line_rows(&lines[idx], terminal_width);
        total += rows;
        offsets[idx + 1U] = total;
    }
    return total;
}

void session_note_output_lines(session_ctx_t *ctx, size_t line_count)
{
    if (ctx == nullptr || line_count == 0U) {
        return;
    }

    size_t total =
        (size_t)ctx->output_lines_since_prompt + line_count;
    unsigned int clamp =
        (ctx->terminal_height > 0U) ? ctx->terminal_height : UINT_MAX;

    if (total > clamp) {
        total = clamp;
    }
    if (total > UINT_MAX) {
        total = UINT_MAX;
    }

    ctx->output_lines_since_prompt = (unsigned int)total;
    ctx->prompt_needs_padding = true;
}

static size_t session_scrollback_line_capacity(const session_ctx_t *ctx)
{
    size_t target = SSH_CHATTER_SCROLLBACK_CHUNK;

    if (ctx != nullptr) {
        unsigned int height = ctx->terminal_height;
        if (height > 0U) {
            const unsigned int reserved_lines = 2U;
            if (height > reserved_lines) {
                target = (size_t)(height - reserved_lines);
            } else {
                target = 1U;
            }
        }
    }

    if (target > SSH_CHATTER_SCROLLBACK_MAX_CHUNK) {
        target = SSH_CHATTER_SCROLLBACK_MAX_CHUNK;
    }
    if (target == 0U) {
        target = 1U;
    }

    return target;
}

static chat_history_entry_t *
session_scrollback_reserve_buffer(session_ctx_t *ctx, size_t minimum_capacity,
                                  size_t *out_capacity)
{
    if (ctx == nullptr) {
        if (out_capacity != nullptr) {
            *out_capacity = 0U;
        }
        return nullptr;
    }

    size_t target = session_scrollback_line_capacity(ctx);
    if (target < minimum_capacity) {
        target = minimum_capacity;
    }
    if (target == 0U) {
        target = 1U;
    }

    if (ctx->scrollback_buffer != nullptr &&
        ctx->scrollback_buffer_capacity >= target) {
        if (out_capacity != nullptr) {
            *out_capacity = ctx->scrollback_buffer_capacity;
        }
        return ctx->scrollback_buffer;
    }

    chat_history_entry_t *fresh = (chat_history_entry_t *)sshc_gc_calloc(
        target, sizeof(chat_history_entry_t));
    if (fresh == nullptr) {
        if (out_capacity != nullptr) {
            *out_capacity = 0U;
        }
        return nullptr;
    }

    if (ctx->scrollback_buffer != nullptr) {
        sshc_gc_free(ctx->scrollback_buffer);
    }
    ctx->scrollback_buffer = fresh;
    ctx->scrollback_buffer_capacity = target;
    if (out_capacity != nullptr) {
        *out_capacity = target;
    }
    return ctx->scrollback_buffer;
}

static size_t session_capture_visible_display_lines(
    display_model_t *model, unsigned int viewport_height,
    display_line_t *snapshot, size_t snapshot_capacity)
{
    if (model == nullptr || snapshot == nullptr || snapshot_capacity == 0U) {
        return 0U;
    }

    display_visible_frame_t frame;
    display_model_compute_visible(model, viewport_height, &frame);
    size_t count = frame.count;
    if (count > snapshot_capacity) {
        count = snapshot_capacity;
    }

    for (size_t idx = 0U; idx < count; ++idx) {
        snapshot[idx] = frame.lines[idx];
    }

    return count;
}

static size_t session_describe_display_lines(const display_line_t *lines,
                                             size_t count,
                                             session_screen_line_t *described,
                                             size_t described_capacity)
{
    if (lines == nullptr || described == nullptr || described_capacity == 0U) {
        return 0U;
    }

    if (count > described_capacity) {
        count = described_capacity;
    }

    for (size_t idx = 0U; idx < count; ++idx) {
        described[idx].text = lines[idx].text;
        described[idx].length =
            strnlen(lines[idx].text, sizeof(lines[idx].text));
    }

    return count;
}

static size_t session_describe_visible_frame(
    const display_visible_frame_t *frame, session_screen_line_t *described,
    size_t described_capacity)
{
    if (frame == nullptr || described == nullptr || described_capacity == 0U) {
        return 0U;
    }

    size_t count = frame->count;
    if (count > described_capacity) {
        count = described_capacity;
    }

    for (size_t idx = 0U; idx < count; ++idx) {
        described[idx].text = frame->lines[idx].text;
        described[idx].length =
            strnlen(frame->lines[idx].text, sizeof(frame->lines[idx].text));
    }

    return count;
}

static __attribute__((unused)) size_t
session_describe_buffer_lines(const char *buffer,
                              session_screen_line_t *described,
                              size_t described_capacity)
{
    if (buffer == nullptr || described == nullptr || described_capacity == 0U) {
        return 0U;
    }

    size_t count = 0U;
    const char *cursor = buffer;
    while (*cursor != '\0' && count < described_capacity) {
        const char *line_start = cursor;
        while (*cursor != '\0' && *cursor != '\n') {
            ++cursor;
        }

        size_t line_length = (size_t)(cursor - line_start);
        if (line_length > 0U && line_start[line_length - 1U] == '\r') {
            --line_length;
        }

        described[count].text = line_start;
        described[count].length = line_length;
        ++count;

        if (*cursor == '\n') {
            ++cursor;
        }
    }

    return count;
}

static bool session_screen_line_matches(const session_screen_line_t *lhs,
                                        const session_screen_line_t *rhs)
{
    if (lhs == nullptr || rhs == nullptr) {
        return false;
    }

    if (lhs->length != rhs->length) {
        return false;
    }

    if (lhs->length == 0U) {
        return true;
    }

    return memcmp(lhs->text, rhs->text, lhs->length) == 0;
}

static void session_write_vertical_cursor_move(session_ctx_t *ctx, size_t lines,
                                               char direction)
{
    if (ctx == nullptr || lines == 0U) {
        return;
    }

    char sequence[32];
    int written =
        snprintf(sequence, sizeof(sequence), "\033[%zu%c", lines, direction);
    if (written > 0) {
        session_channel_write(ctx, sequence, (size_t)written);
    }
}

static void session_write_scroll_up(session_ctx_t *ctx, size_t lines)
{
    if (ctx == nullptr || lines == 0U) {
        return;
    }

    char sequence[32];
    int written = snprintf(sequence, sizeof(sequence), "\033[%zuS", lines);
    if (written > 0 && written < (int)sizeof(sequence)) {
        session_channel_write(ctx, sequence, (size_t)written);
    }
}

static size_t session_detect_incremental_scroll_shift(
    const session_screen_line_t *old_lines, size_t old_count,
    const session_screen_line_t *new_lines, size_t new_count)
{
    if (old_lines == nullptr || new_lines == nullptr || old_count == 0U ||
        new_count == 0U || old_count != new_count) {
        return 0U;
    }

    bool any_difference = false;
    for (size_t idx = 0U; idx < old_count; ++idx) {
        if (!session_screen_line_matches(&old_lines[idx], &new_lines[idx])) {
            any_difference = true;
            break;
        }
    }
    if (!any_difference) {
        return 0U;
    }

    for (size_t shift = 1U; shift < old_count; ++shift) {
        const size_t overlap = old_count - shift;
        bool matches = true;
        for (size_t idx = 0U; idx < overlap; ++idx) {
            if (!session_screen_line_matches(&old_lines[shift + idx],
                                             &new_lines[idx])) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return shift;
        }
    }

    return 0U;
}

static bool session_render_incremental_lines(session_ctx_t *ctx,
                                             const session_screen_line_t *old_lines,
                                             size_t old_count,
                                             const session_screen_line_t *new_lines,
                                             size_t new_count,
                                             bool clear_following_line)
{
    if (ctx == nullptr || old_lines == nullptr || new_lines == nullptr ||
        old_count == 0U) {
        return false;
    }

    static const char kHideCursor[] = "\033[?25l";
    static const char kShowCursor[] = "\033[?25h";
    static const char kClearLine[] = "\r" ANSI_CLEAR_LINE;
    unsigned int terminal_width =
        (ctx->terminal_width > 0U) ? ctx->terminal_width : 80U;
    if (terminal_width == 0U) {
        terminal_width = 80U;
    }

    size_t old_row_offsets[SSH_CHATTER_SCROLLBACK_MAX_CHUNK + 1];
    size_t new_row_offsets[SSH_CHATTER_SCROLLBACK_MAX_CHUNK + 1];
    const size_t old_offset_capacity =
        sizeof(old_row_offsets) / sizeof(old_row_offsets[0]);
    const size_t new_offset_capacity =
        sizeof(new_row_offsets) / sizeof(new_row_offsets[0]);
    size_t total_old_rows = session_build_row_offsets(
        old_lines, old_count, terminal_width, old_row_offsets,
        old_offset_capacity);
    size_t total_new_rows = session_build_row_offsets(
        new_lines, new_count, terminal_width, new_row_offsets,
        new_offset_capacity);

    session_channel_write(ctx, kHideCursor, sizeof(kHideCursor) - 1U);
    session_channel_write(ctx, kClearLine, sizeof(kClearLine) - 1U);
    session_write_vertical_cursor_move(ctx, total_old_rows, 'A');

    const size_t scroll_shift = session_detect_incremental_scroll_shift(
        old_lines, old_count, new_lines, new_count);
    const size_t redraw_start =
        (scroll_shift > 0U && scroll_shift < old_count) ? (old_count - scroll_shift) : 0U;
    if (scroll_shift > 0U) {
        size_t limited_shift = scroll_shift < old_count ? scroll_shift : old_count;
        size_t scroll_rows =
            limited_shift < old_offset_capacity ? old_row_offsets[limited_shift]
                                                : total_old_rows;
        if (scroll_rows > 0U) {
            session_write_scroll_up(ctx, scroll_rows);
        }
    }

    const size_t max_lines = old_count > new_count ? old_count : new_count;
    size_t current_row = 0U;
    bool cursor_positioned = false;

    for (size_t idx = redraw_start; idx < max_lines; ++idx) {
        const bool old_present = idx < old_count;
        const bool new_present = idx < new_count;
        const bool changed =
            !old_present || !new_present ||
            !session_screen_line_matches(&old_lines[idx], &new_lines[idx]);
        if (!changed) {
            continue;
        }

        size_t target_row =
            (old_present && idx < old_offset_capacity) ? old_row_offsets[idx]
                                                       : total_old_rows;
        if (!cursor_positioned) {
            session_write_vertical_cursor_move(ctx, target_row, 'B');
            current_row = target_row;
            cursor_positioned = true;
        } else if (target_row > current_row) {
            session_write_vertical_cursor_move(ctx, target_row - current_row, 'B');
            current_row = target_row;
        }

        session_channel_write(ctx, kClearLine, sizeof(kClearLine) - 1U);
        session_fill_line_with_theme(ctx);
        if (new_present && new_lines[idx].length > 0U) {
            char themed_line[SSH_CHATTER_MESSAGE_LIMIT * 4U];
            char raw_line[SSH_CHATTER_MESSAGE_LIMIT];
            size_t copy_len = new_lines[idx].length;
            if (copy_len >= sizeof(raw_line)) {
                copy_len = sizeof(raw_line) - 1U;
            }
            memcpy(raw_line, new_lines[idx].text, copy_len);
            raw_line[copy_len] = '\0';
            size_t themed_len = session_prepare_themed_output(
                ctx, raw_line, themed_line, sizeof(themed_line));
            if (themed_len > 0U) {
                session_channel_write(ctx, themed_line, themed_len);
            }
        }
    }

    size_t footer_row = total_new_rows > 0U ? total_new_rows : total_old_rows;
    if (!cursor_positioned) {
        session_write_vertical_cursor_move(ctx, footer_row, 'B');
    } else if (footer_row >= current_row) {
        session_write_vertical_cursor_move(ctx, footer_row - current_row, 'B');
    }

    if (clear_following_line) {
        session_channel_write(ctx, kClearLine, sizeof(kClearLine) - 1U);
    }
    session_channel_write(ctx, kShowCursor, sizeof(kShowCursor) - 1U);
    return true;
}

static void session_scrollback_prepare_display(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    const char clear_sequence[] = "\r" ANSI_CLEAR_LINE;
    session_channel_write(ctx, clear_sequence, sizeof(clear_sequence) - 1U);

    // On the first scrollback entry scrollback_rendered_lines is 0, so use the
    // terminal height to jump up far enough to cover the full visible area and
    // produce a clean slide-view page.  On subsequent navigations the exact
    // number of lines that were rendered last time is used instead.
    size_t rendered = ctx->scrollback_rendered_lines;
    if (rendered == 0U) {
        unsigned int height = ctx->terminal_height > 0U ? ctx->terminal_height : SESSION_DEFAULT_TERMINAL_HEIGHT;
        rendered = (size_t)height;
    }

    char move_up[32];
    int move_up_len = snprintf(move_up, sizeof(move_up), "\033[%zuA", rendered);
    if (move_up_len > 0) {
        session_channel_write(ctx, move_up, (size_t)move_up_len);
    }

    // Clear from the repositioned cursor to the end of the screen so the
    // incoming scrollback chunk fills the terminal without leftover lines.
    static const char clear_to_end[] = "\033[J";
    session_channel_write(ctx, clear_to_end, sizeof(clear_to_end) - 1U);
}

static void session_render_banner_text(session_ctx_t *ctx, const char *banner)
{
    if (ctx == nullptr || banner == nullptr) {
        return;
    }

    bool locked = session_output_lock(ctx);

    const char *cursor = banner;
    while (true) {
        const char *newline = strchr(cursor, '\n');
        size_t length = newline != nullptr
                            ? (size_t)(newline - cursor)
                            : strnlen(cursor, SSH_CHATTER_MESSAGE_LIMIT);
        while (length > 0U && cursor[length - 1U] == '\r') {
            --length;
        }

        session_fill_line_with_theme(ctx);
        static const char column_reset[] = "\033[1G";
        session_channel_write(ctx, column_reset, sizeof(column_reset) - 1U);
        if (length > 0U) {
            session_channel_write(ctx, cursor, length);
        }
        session_channel_write(ctx, "\r\n", 2U);
        session_note_output_lines(ctx, 1U);

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

void host_set_welcome_banner(host_t *host, const char *banner)
{
    if (host == nullptr) {
        return;
    }

    if (banner == nullptr || banner[0] == '\0') {
        host->welcome_banner[0] = '\0';
        host->welcome_banner_loaded = false;
        return;
    }

    snprintf(host->welcome_banner, sizeof(host->welcome_banner), "%s", banner);
    host->welcome_banner_loaded = true;
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
    ctx->realtime_line_count = ctx->realtime_recent_count;

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

static void session_send_plain_line(session_ctx_t *ctx, const char *message)
{
    if (ctx == nullptr || !session_transport_active(ctx) ||
        message == nullptr) {
        return;
    }

    // Prevent re-output of the last processed line
    if (!ctx->disable_output_dedup && ctx->has_last_output_line &&
        strncmp(ctx->last_output_line, message, SSH_CHATTER_MESSAGE_LIMIT) == 0) {
        return;
    }

    static const char kCaptionPrefix[] = "    ->";
    if (strncmp(message, kCaptionPrefix, sizeof(kCaptionPrefix) - 1U) == 0) {
        session_send_caption_line(ctx, message);
        // Update last output line after sending
        size_t msg_len = strnlen(message, SSH_CHATTER_MESSAGE_LIMIT - 1U);
        if (msg_len >= sizeof(ctx->last_output_line)) {
            msg_len = sizeof(ctx->last_output_line) - 1U;
        }
        memcpy(ctx->last_output_line, message, msg_len);
        ctx->last_output_line[msg_len] = '\0';
        ctx->has_last_output_line = true;
        return;
    }

    session_write_rendered_line(ctx, message);
    session_realtime_record_line(ctx, message);

    if (ctx->capture_realtime_output &&
        ctx->realtime_line_count >= SESSION_REALTIME_CLEAR_INTERVAL) {
        session_realtime_refresh(ctx);
    }

    // Update last output line after sending
    size_t msg_len = strnlen(message, SSH_CHATTER_MESSAGE_LIMIT - 1U);
    if (msg_len >= sizeof(ctx->last_output_line)) {
        msg_len = sizeof(ctx->last_output_line) - 1U;
    }
    memcpy(ctx->last_output_line, message, msg_len);
    ctx->last_output_line[msg_len] = '\0';
    ctx->has_last_output_line = true;
}

static void session_send_reply_tree(session_ctx_t *ctx,
                                    uint64_t parent_message_id,
                                    uint64_t parent_reply_id, size_t depth)
{
    if (ctx == nullptr || ctx->owner == nullptr || parent_message_id == 0U) {
        return;
    }

    if (depth > 32U) {
        return;
    }

    host_t *host = ctx->owner;

    size_t match_count = 0U;
    ttak_mutex_lock(&host->lock);
    for (size_t idx = 0U; idx < host->reply_count; ++idx) {
        const chat_reply_entry_t *candidate = &host->replies[idx];
        if (!candidate->in_use) {
            continue;
        }
        if (candidate->parent_message_id == parent_message_id &&
            candidate->parent_reply_id == parent_reply_id) {
            ++match_count;
        }
    }

    if (match_count == 0U) {
        ttak_mutex_unlock(&host->lock);
        return;
    }

    chat_reply_entry_t *snapshot = sshc_gc_calloc(match_count, sizeof(*snapshot));
    if (snapshot == nullptr) {
        ttak_mutex_unlock(&host->lock);
        return;
    }

    size_t copy_idx = 0U;
    for (size_t idx = 0U; idx < host->reply_count && copy_idx < match_count;
         ++idx) {
        const chat_reply_entry_t *candidate = &host->replies[idx];
        if (!candidate->in_use) {
            continue;
        }
        if (candidate->parent_message_id == parent_message_id &&
            candidate->parent_reply_id == parent_reply_id) {
            snapshot[copy_idx++] = *candidate;
        }
    }
    ttak_mutex_unlock(&host->lock);

    for (size_t idx = 0U; idx < copy_idx; ++idx) {
        const chat_reply_entry_t *reply = &snapshot[idx];

        size_t indent_len = depth * 4U;
        char indent[128];
        if (indent_len >= sizeof(indent)) {
            indent_len = sizeof(indent) - 1U;
        }
        memset(indent, ' ', indent_len);
        indent[indent_len] = '\0';

        char reply_label[32];
        const char *reply_display = "?";
        if (host_compact_id_encode(reply->reply_id, reply_label,
                                   sizeof(reply_label))) {
            reply_display = reply_label;
        }

        char line[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(line, sizeof(line), "%s(r#%s) %s", indent, reply_display,
                 reply->message);
        session_send_plain_line(ctx, line);

        session_send_reply_tree(ctx, parent_message_id, reply->reply_id,
                                depth + 1U);
    }

    sshc_gc_free(snapshot);
}

static bool host_lookup_member_ip(host_t *host, const char *username, char *ip,
                                  size_t length)
{
    if (host == nullptr || username == nullptr || ip == nullptr ||
        length == 0U) {
        return false;
    }

    session_ctx_t *member = chat_room_find_user(&host->room, username);
    if (member == nullptr || member->client_ip[0] == '\0') {
        return false;
    }

    snprintf(ip, length, "\%s", member->client_ip);
    return true;
}

static void session_telnet_capture_startup_metadata(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->telnet_fd < 0) {
        return;
    }

    while (!ctx->telnet_eof) {
        struct pollfd pfd = {
            .fd = ctx->telnet_fd,
            .events = POLLIN,
            .revents = 0,
        };

        int poll_result = poll(&pfd, 1, 0);
        if (poll_result <= 0 || (pfd.revents & POLLIN) == 0) {
            break;
        }

        unsigned char byte = 0U;
        int read_result = session_telnet_read_byte(ctx, &byte, 0);
        if (read_result == SSH_AGAIN) {
            break;
        }
        if (read_result <= 0) {
            break;
        }

        if (byte != 0U) {
            if (!ctx->telnet_pending_valid) {
                ctx->telnet_pending_char = (int)byte;
                ctx->telnet_pending_valid = true;
            }
            break;
        }
    }
}

typedef struct provider_prefix {
    const char *prefix;
    const char *label;
} provider_prefix_t;

static const provider_prefix_t kProviderPrefixes[] = {
    {"39.7.", "Korean ISP"},
    {"58.120.", "Korean ISP"},
    {"59.0.", "Korean ISP"},
    {"61.32.", "Korean ISP"},
    {"211.36.", "Korean ISP"},
    {"218.144.", "Korean ISP"},
    {"220.149.", "Korean ISP"},
    {"73.", "US ISP"},
    {"96.", "US ISP"},
    {"107.", "US ISP"},
    {"174.", "US ISP"},
    {"2600:", "US ISP"},
    {"2604:", "US ISP"},
    {"2605:", "US ISP"},
    {"2607:", "US ISP"},
    {"2609:", "US ISP"},
    {"1.0.", "Japanese ISP"},
    {"106.130.", "Japanese ISP"},
    {"118.103.", "Japanese ISP"},
    {"133.", "Japanese ISP"},
    {"153.", "Japanese ISP"},
    {"60.62.", "Japanese ISP"},
    {"61.22.", "Japanese ISP"},
    {"61.46.", "Japanese ISP"},
    {"220.100", "Japanese ISP"},
    {"202.162.128.", "Japanese ISP"},
    {"202.140.240.", "Japanese ISP"},
    {"110.172", "Japanese ISP"},
    {"2400:", "Japanese ISP"},
    {"2404:", "Japanese ISP"},
    {"2406:", "Japanese ISP"},
    {"2408:", "Japanese ISP"},
    {"24.114.", "Canadian ISP"},
    {"142.", "Canadian ISP"},
    {"2603:", "Canadian ISP"},
    {"185.", "EU ISP"},
    {"195.", "EU ISP"},
    {"2a00:", "EU ISP"},
    {"2a02:", "EU ISP"},
    {"2a03:", "EU ISP"},
    {"2a09:", "EU ISP"},
    {"5.18.", "Russian ISP"},
    {"37.", "Russian ISP"},
    {"91.", "Russian ISP"},
    {"36.", "Chinese ISP"},
    {"42.", "Chinese ISP"},
    {"139.", "Chinese ISP"},
    {"2408:", "Chinese ISP"},
    {"2409:", "Chinese ISP"},
    {"49.", "Indian ISP"},
    {"103.", "Indian ISP"},
    {"106.", "Indian ISP"},
    {"2405:", "Indian ISP"},
    {"2406:", "Indian ISP"},
    {"100.64.", "Carrier-grade NAT"}};

bool is_pure_ascii(const char *str)
{
    size_t len = strlen(str);
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)str[i] > 127) {
            return false;
        }
    }
    return true;
}

int count_unicode_points(const char *str, utf8_code_count_t **counts_out,
                         size_t *unique_count_out)
{
    size_t len = strnlen(str, SSH_CHATTER_MESSAGE_LIMIT);
    size_t unique_count = 0;
    size_t capacity = 100;
    utf8_code_count_t *counts =
        (utf8_code_count_t *)sshc_gc_calloc(capacity, sizeof(utf8_code_count_t));

    if (!counts)
        return -1;

    size_t total_points = 0;
    for (size_t i = 0; i < len;) {
        unsigned int code_point = 0;
        size_t bytes_read = 0;

        unsigned char c = (unsigned char)str[i];
        if ((c & 0x80) == 0) {
            code_point = c;
            bytes_read = 1;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 < len)
                code_point = ((c & 0x1F) << 6) | (str[i + 1] & 0x3F);
            bytes_read = 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 < len)
                code_point = ((c & 0x0F) << 12) | ((str[i + 1] & 0x3F) << 6) |
                             (str[i + 2] & 0x3F);
            bytes_read = 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 < len)
                code_point = ((c & 0x07) << 18) | ((str[i + 1] & 0x3F) << 12) |
                             ((str[i + 2] & 0x3F) << 6) | (str[i + 3] & 0x3F);
            bytes_read = 4;
        } else {
            code_point = c;
            bytes_read = 1;
        }

        i += bytes_read;
        total_points++;

        int found = 0;
        for (size_t j = 0; j < unique_count; j++) {
            if (counts[j].code_point == code_point) {
                counts[j].count++;
                found = 1;
                break;
            }
        }

        if (!found) {
            if (unique_count >= capacity) {
                capacity *= 2;
                utf8_code_count_t *new_counts = (utf8_code_count_t *)sshc_gc_realloc(
                    counts, capacity * sizeof(utf8_code_count_t));
                if (!new_counts) {
                    sshc_gc_free(counts);
                    return -1;
                }
                counts = new_counts;
            }
            counts[unique_count].code_point = code_point;
            counts[unique_count].count = 1;
            unique_count++;
        }
    }

    *counts_out = counts;
    *unique_count_out = unique_count;
    return (int)total_points;
}

double calculate_chi_squared(const char *str)
{
    utf8_code_count_t *counts = nullptr;
    size_t unique_count = 0;
    int N = count_unicode_points(str, &counts, &unique_count);

    if (N <= 0 || !counts || unique_count == 0) {
        if (counts)
            sshc_gc_free(counts);
        return 0.0;
    }

    double E_i = (double)N / (double)unique_count;

    double chi_squared = 0.0;
    for (size_t i = 0; i < unique_count; i++) {
        double O_i = (double)counts[i].count;
        double diff = O_i - E_i;

        if (E_i > 0) {
            chi_squared += (diff * diff) / E_i;
        }
    }

    sshc_gc_free(counts);
    return chi_squared;
}

bool is_string_random(const char *str)
{
    static const double CRITICAL_THRESHOLD = 25.0;

    double chi_squared = calculate_chi_squared(str);

    if (strlen(str) > 20) {
        return chi_squared < CRITICAL_THRESHOLD;
    } else {
        return chi_squared < 10.0;
    }
}

bool session_detect_provider_ip(const char *ip, char *label, size_t length)
{
    if (label != nullptr && length > 0U) {
        label[0] = '\0';
    }

    if (ip == nullptr || ip[0] == '\0' || label == nullptr || length == 0U) {
        return false;
    }

    for (size_t idx = 0U;
         idx < sizeof(kProviderPrefixes) / sizeof(kProviderPrefixes[0]);
         ++idx) {
        const provider_prefix_t *entry = &kProviderPrefixes[idx];
        size_t prefix_len = strlen(entry->prefix);
        if (strncasecmp(ip, entry->prefix, prefix_len) == 0) {
            snprintf(label, length, "%s", entry->label);
            return true;
        }
    }

    return false;
}

static const struct {
    const char *label;
    session_ui_language_t language;
} kProviderLanguageMapping[] = {
    {"Korean ISP", SESSION_UI_LANGUAGE_KO},
    {"US ISP", SESSION_UI_LANGUAGE_EN},
    {"Canadian ISP", SESSION_UI_LANGUAGE_EN},
    {"EU ISP", SESSION_UI_LANGUAGE_EN},
    {"Russian ISP", SESSION_UI_LANGUAGE_RU},
    {"Chinese ISP", SESSION_UI_LANGUAGE_ZH},
    {"Indian ISP", SESSION_UI_LANGUAGE_EN},
    {"Japanese ISP", SESSION_UI_LANGUAGE_JP},
};

static session_ui_language_t
session_client_geo_language(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return SESSION_UI_LANGUAGE_COUNT;
    }

    if (ctx->owner != nullptr &&
        !atomic_load(&ctx->owner->geo_language_enabled)) {
        return SESSION_UI_LANGUAGE_COUNT;
    }

    char label[64];
    if (!session_detect_provider_ip(ctx->client_ip, label, sizeof(label))) {
        return SESSION_UI_LANGUAGE_COUNT;
    }

    for (size_t idx = 0U; idx < sizeof(kProviderLanguageMapping) /
                                    sizeof(kProviderLanguageMapping[0]);
         ++idx) {
        if (strcasecmp(label, kProviderLanguageMapping[idx].label) == 0) {
            return kProviderLanguageMapping[idx].language;
        }
    }

    return SESSION_UI_LANGUAGE_COUNT;
}

static bool session_blocklist_add(session_ctx_t *ctx, const char *ip,
                                  const char *username, bool ip_wide,
                                  bool *already_present)
{
    if (ctx == nullptr) {
        if (already_present != nullptr) {
            *already_present = false;
        }
        return false;
    }

    if (already_present != nullptr) {
        *already_present = false;
    }

    char normalized_ip[SSH_CHATTER_IP_LEN] = {0};
    char normalized_user[SSH_CHATTER_USERNAME_LEN] = {0};

    if (ip != nullptr && ip[0] != '\0') {
        snprintf(normalized_ip, sizeof(normalized_ip), "%s", ip);
    }

    if (username != nullptr && username[0] != '\0') {
        snprintf(normalized_user, sizeof(normalized_user), "%s", username);
    }

    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_BLOCKED; ++idx) {
        session_block_entry_t *entry = &ctx->block_entries[idx];
        if (!entry->in_use) {
            continue;
        }

        if (ip_wide) {
            if (normalized_ip[0] != '\0' &&
                strncmp(entry->ip, normalized_ip, SSH_CHATTER_IP_LEN) == 0) {
                if (already_present != nullptr) {
                    *already_present = true;
                }
                return false;
            }
        } else {
            if (normalized_user[0] != '\0' &&
                strncmp(entry->username, normalized_user,
                        SSH_CHATTER_USERNAME_LEN) == 0 &&
                !entry->ip_wide) {
                if (already_present != nullptr) {
                    *already_present = true;
                }
                return false;
            }
        }
    }

    size_t free_index = SSH_CHATTER_MAX_BLOCKED;
    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_BLOCKED; ++idx) {
        if (!ctx->block_entries[idx].in_use) {
            free_index = idx;
            break;
        }
    }

    if (free_index >= SSH_CHATTER_MAX_BLOCKED) {
        return false;
    }

    session_block_entry_t *slot = &ctx->block_entries[free_index];
    memset(slot, 0, sizeof(*slot));
    slot->in_use = true;
    slot->ip_wide = ip_wide;
    if (normalized_ip[0] != '\0') {
        snprintf(slot->ip, sizeof(slot->ip), "%s", normalized_ip);
    }
    if (normalized_user[0] != '\0') {
        snprintf(slot->username, sizeof(slot->username), "%s", normalized_user);
    }

    if (ctx->block_entry_count < SSH_CHATTER_MAX_BLOCKED) {
        ctx->block_entry_count += 1U;
    }

    return true;
}

static bool session_blocklist_remove(session_ctx_t *ctx, const char *token)
{
    if (ctx == nullptr || token == nullptr || token[0] == '\0') {
        return false;
    }

    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_BLOCKED; ++idx) {
        session_block_entry_t *entry = &ctx->block_entries[idx];
        if (!entry->in_use) {
            continue;
        }

        if ((entry->ip[0] != '\0' &&
             strncmp(entry->ip, token, SSH_CHATTER_IP_LEN) == 0) ||
            (entry->username[0] != '\0' &&
             strncmp(entry->username, token, SSH_CHATTER_USERNAME_LEN) == 0)) {
            memset(entry, 0, sizeof(*entry));
            if (ctx->block_entry_count > 0U) {
                ctx->block_entry_count -= 1U;
            }
            return true;
        }
    }

    return false;
}

static void session_blocklist_show(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    if (ctx->block_entry_count == 0U) {
        session_send_system_line(ctx, "No blocked users or IPs.");
        return;
    }

    session_send_system_line(ctx, "Blocked targets:");
    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_BLOCKED; ++idx) {
        const session_block_entry_t *entry = &ctx->block_entries[idx];
        if (!entry->in_use) {
            continue;
        }

        char line[SSH_CHATTER_MESSAGE_LIMIT];
        if (entry->ip_wide && entry->ip[0] != '\0') {
            if (entry->username[0] != '\0') {
                snprintf(line, sizeof(line),
                         "- %s (all users from this IP, originally [%s])",
                         entry->ip, entry->username);
            } else {
                snprintf(line, sizeof(line), "- %s (all users from this IP)",
                         entry->ip);
            }
        } else if (entry->username[0] != '\0') {
            if (entry->ip[0] != '\0') {
                snprintf(line, sizeof(line), "- [%s] (only this user, IP %s)",
                         entry->username, entry->ip);
            } else {
                snprintf(line, sizeof(line), "- [%s]", entry->username);
            }
        } else {
            snprintf(line, sizeof(line), "- entry #%zu", idx + 1U);
        }
        session_send_system_line(ctx, line);
    }
}

static bool session_message_contains_breaking(const char *message)
{
    if (message == nullptr) {
        return false;
    }

    if (strstr(message, SSH_CHATTER_RSS_BREAKING_PREFIX) != nullptr) {
        return true;
    }

    if (strcasestr(message, "breaking news") != nullptr ||
        strcasestr(message, "breaking:") != nullptr ||
        strcasestr(message, "urgent") != nullptr ||
        strcasestr(message, "alert") != nullptr) {
        return true;
    }

    if (strstr(message, "속보") != nullptr ||
        strstr(message, "速報") != nullptr) {
        return true;
    }

    return false;
}

static bool session_bbs_should_defer_breaking(session_ctx_t *ctx,
                                              const char *message)
{
    if (ctx == nullptr || message == nullptr) {
        return false;
    }

    if (!ctx->breaking_alerts_enabled) {
        return false;
    }

    if (!ctx->bbs_post_pending || ctx->bbs_rendering_editor) {
        return false;
    }

    return session_message_contains_breaking(message);
}

static size_t session_find_wrap_position(const char *text, size_t start,
                                         size_t max_width)
{
    if (text == nullptr || max_width == 0U) {
        return start;
    }

    size_t pos = start;
    size_t visible_chars = 0U;
    size_t last_space_pos = start;
    size_t last_space_visible = 0U;
    bool in_escape = false;

    while (text[pos] != '\0' && visible_chars < max_width) {
        if (text[pos] == '\033') {
            // Start of ANSI escape sequence
            in_escape = true;
            ++pos;
            continue;
        }

        if (in_escape) {
            // Skip escape sequence characters
            if ((text[pos] >= 'A' && text[pos] <= 'Z') ||
                (text[pos] >= 'a' && text[pos] <= 'z')) {
                in_escape = false;
            }
            ++pos;
            continue;
        }

        // Regular character
        if (text[pos] == ' ' || text[pos] == '\t') {
            last_space_pos = pos;
            last_space_visible = visible_chars;
        }

        ++visible_chars;
        ++pos;
    }

    // If we haven't exceeded max_width, return the current position
    if (text[pos] == '\0' || visible_chars < max_width) {
        return pos;
    }

    // If we found a space within reasonable distance, break there
    if (last_space_visible > 0U &&
        (visible_chars - last_space_visible) < (max_width / 3)) {
        return last_space_pos + 1; // Skip the space itself
    }

    // Otherwise, hard break at max_width
    return pos;
}

static void session_bbs_format_breaking_notice_wrapped(
    const char *message, char lines[][SSH_CHATTER_MESSAGE_LIMIT],
    size_t max_lines, size_t *line_count)
{
    if (message == nullptr || lines == nullptr || max_lines == 0U ||
        line_count == nullptr) {
        if (line_count != nullptr) {
            *line_count = 0U;
        }
        return;
    }

    *line_count = 0U;

    // Maximum visible characters per line (accounting for terminal width)
    const size_t kMaxVisibleWidth = 48U;

    // ANSI codes to apply
    const char *kPrefix =
        "\r\033[2G" ANSI_BG_BRIGHT_BLUE ANSI_BRIGHT_MAGENTA ANSI_BOLD;
    const char *kSuffix = "\033[K" ANSI_RESET;

    size_t pos = 0U;
    while (message[pos] != '\0' && *line_count < max_lines) {
        // Skip leading whitespace
        while (message[pos] == ' ' || message[pos] == '\t') {
            ++pos;
        }

        if (message[pos] == '\0') {
            break;
        }

        // Find where to wrap this line
        size_t next_pos =
            session_find_wrap_position(message, pos, kMaxVisibleWidth);

        // Extract the segment
        size_t segment_len = next_pos - pos;
        char segment[SSH_CHATTER_MESSAGE_LIMIT];
        if (segment_len >= sizeof(segment)) {
            segment_len = sizeof(segment) - 1U;
        }
        memcpy(segment, message + pos, segment_len);
        segment[segment_len] = '\0';

        // Remove trailing whitespace
        while (segment_len > 0U && (segment[segment_len - 1U] == ' ' ||
                                    segment[segment_len - 1U] == '\t')) {
            segment[--segment_len] = '\0';
        }

        // Format the line with ANSI codes
        size_t offset = 0U;
        offset = session_append_fragment(
            lines[*line_count], SSH_CHATTER_MESSAGE_LIMIT, offset, kPrefix);
        offset = session_append_fragment(
            lines[*line_count], SSH_CHATTER_MESSAGE_LIMIT, offset, segment);
        session_append_fragment(lines[*line_count], SSH_CHATTER_MESSAGE_LIMIT,
                                offset, kSuffix);

        (*line_count)++;
        pos = next_pos;
    }
}

static void session_bbs_format_breaking_notice(const char *message, char *out,
                                               size_t length)
{
    if (out == nullptr || length == 0U) {
        return;
    }

    out[0] = '\0';

    if (message == nullptr) {
        return;
    }

    size_t offset = 0U;
    offset = session_append_fragment(out, length, offset, "\r\033[2G");
    offset = session_append_fragment(out, length, offset, ANSI_BG_BRIGHT_BLUE);
    offset = session_append_fragment(out, length, offset, ANSI_BRIGHT_MAGENTA);
    offset = session_append_fragment(out, length, offset, ANSI_BOLD);
    offset = session_append_fragment(out, length, offset, message);
    offset = session_append_fragment(out, length, offset, "\033[K");
    session_append_fragment(out, length, offset, ANSI_RESET);
}

static void session_bbs_buffer_breaking_notice(session_ctx_t *ctx,
                                               const char *message)
{
    if (ctx == nullptr || message == nullptr) {
        return;
    }

    if (!ctx->breaking_alerts_enabled) {
        return;
    }

    const size_t kWrapThreshold = 76U;
    char formatted_lines[4][SSH_CHATTER_MESSAGE_LIMIT];
    size_t formatted_count = 0U;
    if (strlen(message) > kWrapThreshold) {
        session_bbs_format_breaking_notice_wrapped(
            message, formatted_lines,
            sizeof(formatted_lines) / sizeof(formatted_lines[0]),
            &formatted_count);
    } else {
        session_bbs_format_breaking_notice(
            message, formatted_lines[0], sizeof(formatted_lines[0]));
        formatted_count = 1U;
    }

    if (formatted_count == 0U) {
        session_bbs_format_breaking_notice(
            message, formatted_lines[0], sizeof(formatted_lines[0]));
        formatted_count = 1U;
    }

    session_send_plain_line(ctx, "");
    for (size_t idx = 0U; idx < formatted_count; ++idx) {
        session_send_plain_line(ctx, formatted_lines[idx]);
    }
    session_send_plain_line(ctx, "");
    session_bbs_render_editor(ctx, nullptr);
}

static bool session_should_hide_entry(session_ctx_t *ctx,
                                      const chat_history_entry_t *entry)
{
    if (ctx == nullptr || entry == nullptr) {
        return false;
    }

    if (!entry->is_user_message) {
        if (!ctx->breaking_alerts_enabled &&
            session_message_contains_breaking(entry->message)) {
            return true;
        }
        return false;
    }

    if (ctx->block_entry_count == 0U) {
        return false;
    }

    if (strncmp(entry->username, ctx->user.name, SSH_CHATTER_USERNAME_LEN) ==
        0) {
        return false;
    }

    char entry_ip[SSH_CHATTER_IP_LEN] = {0};
    if (ctx->owner != nullptr) {
        host_lookup_member_ip(ctx->owner, entry->username, entry_ip,
                              sizeof(entry_ip));
    }

    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_BLOCKED; ++idx) {
        const session_block_entry_t *block = &ctx->block_entries[idx];
        if (!block->in_use) {
            continue;
        }

        bool ip_match = false;
        bool user_match = false;

        if (block->ip[0] != '\0' && entry_ip[0] != '\0' &&
            strncmp(block->ip, entry_ip, SSH_CHATTER_IP_LEN) == 0) {
            ip_match = true;
        }

        if (block->username[0] != '\0' &&
            strncmp(block->username, entry->username,
                    SSH_CHATTER_USERNAME_LEN) == 0) {
            user_match = true;
        }

        if (block->ip_wide) {
            if (ip_match) {
                return true;
            }
            if (!ip_match && entry_ip[0] == '\0' && user_match) {
                return true;
            }
        } else {
            if (user_match) {
                return true;
            }
        }
    }

    return false;
}

// this displays a message to a chatting room.
void session_send_system_line(session_ctx_t *ctx, const char *message)
{
    if (ctx == nullptr || !session_transport_active(ctx) ||
        message == nullptr) {
        return;
    }

    session_output_kind_t previous_kind =
        session_output_set_kind(ctx, SESSION_OUTPUT_KIND_SYSTEM);

    if (session_bbs_should_defer_breaking(ctx, message)) {
        session_bbs_buffer_breaking_notice(ctx, message);
        session_output_restore_kind(ctx, previous_kind);
        return;
    }

    if (ctx->game
            .active) { // If game is active, send raw text without [system] prefix
        session_send_raw_text(ctx, message);
        session_output_restore_kind(ctx, previous_kind);
        return;
    }

    session_send_plain_line(ctx, message);

    session_output_restore_kind(ctx, previous_kind);
}

void session_send_raw_text(session_ctx_t *ctx, const char *text)
{
    if (ctx == nullptr || !session_transport_active(ctx) || text == nullptr) {
        return;
    }

    const char *cursor = text;
    while (*cursor != '\0') {
        const char *newline = strchr(cursor, '\n');
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        if (newline == nullptr) {
            snprintf(line, sizeof(line), "%.*s",
                     (int)(sizeof(line) - 1U), cursor);
            session_send_plain_line(ctx, line);
            break;
        }

        size_t length = (size_t)(newline - cursor);
        if (length >= sizeof(line)) {
            length = sizeof(line) - 1U;
        }
        memcpy(line, cursor, length);
        line[length] = '\0';
        session_send_plain_line(ctx, line);

        cursor = newline + 1;
        if (*cursor == '\r') {
            ++cursor;
        }
        if (*cursor == '\0') {
            session_send_plain_line(ctx, "");
        }
    }
}

/* Expand BBS color markup: (#RRGGBB)text(#end)
 * Renders the enclosed text with a bright white background and a 24-bit
 * foreground color. Any other text is passed through unchanged.
 * out is NUL-terminated and will not exceed out_size bytes. */
static void session_bbs_expand_color_markup(const char *text, char *out,
                                             size_t out_size)
{
    if (text == nullptr || out == nullptr || out_size == 0U) {
        if (out != nullptr && out_size > 0U) {
            out[0] = '\0';
        }
        return;
    }
    out[0] = '\0';

    size_t pos = 0U;
    const char *cursor = text;

    while (*cursor != '\0' && pos + 1U < out_size) {
        /* Check for (#end) - case-insensitive, 6 chars */
        if (cursor[0] == '(' && cursor[1] == '#' &&
            (cursor[2] == 'e' || cursor[2] == 'E') &&
            (cursor[3] == 'n' || cursor[3] == 'N') &&
            (cursor[4] == 'd' || cursor[4] == 'D') &&
            cursor[5] == ')') {
            static const char kReset[] = "\033[0m";
            const size_t seq_len = sizeof(kReset) - 1U;
            if (pos + seq_len + 1U <= out_size) {
                memcpy(out + pos, kReset, seq_len);
                pos += seq_len;
            }
            cursor += 6;
            continue;
        }

        /* Check for (#RRGGBB) - exactly 9 characters */
        if (cursor[0] == '(' && cursor[1] == '#' && cursor[8] == ')') {
            bool valid = true;
            for (int hex_idx = 2; hex_idx < 8; ++hex_idx) {
                if (!isxdigit((unsigned char)cursor[hex_idx])) {
                    valid = false;
                    break;
                }
            }
            if (valid) {
                unsigned int r = 0U, g = 0U, b = 0U;
                if (sscanf(cursor + 2, "%02x%02x%02x", &r, &g, &b) == 3) {
                    /* Bright white bg (\033[107m) + 24-bit fg */
                    char ansi_seq[48];
                    int written = snprintf(ansi_seq, sizeof(ansi_seq),
                                           "\033[107m\033[38;2;%u;%u;%um",
                                           r, g, b);
                    if (written > 0 &&
                        pos + (size_t)written + 1U <= out_size) {
                        memcpy(out + pos, ansi_seq, (size_t)written);
                        pos += (size_t)written;
                    }
                    cursor += 9;
                    continue;
                }
            }
        }

        out[pos++] = *cursor++;
    }

    out[pos] = '\0';
}

/* Like session_send_raw_text but expands BBS color markup on each line. */
static void session_send_bbs_body_text(session_ctx_t *ctx, const char *text)
{
    if (ctx == nullptr || !session_transport_active(ctx) || text == nullptr) {
        return;
    }

    const char *cursor = text;
    while (*cursor != '\0') {
        const char *newline = strchr(cursor, '\n');
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        char expanded[SSH_CHATTER_MESSAGE_LIMIT * 4U];
        if (newline == nullptr) {
            snprintf(line, sizeof(line), "%.*s",
                     (int)(sizeof(line) - 1U), cursor);
            session_bbs_expand_color_markup(line, expanded, sizeof(expanded));
            session_send_plain_line(ctx, expanded);
            break;
        }

        size_t length = (size_t)(newline - cursor);
        if (length >= sizeof(line)) {
            length = sizeof(line) - 1U;
        }
        memcpy(line, cursor, length);
        line[length] = '\0';
        session_bbs_expand_color_markup(line, expanded, sizeof(expanded));
        session_send_plain_line(ctx, expanded);

        cursor = newline + 1;
        if (*cursor == '\r') {
            ++cursor;
        }
        if (*cursor == '\0') {
            session_send_plain_line(ctx, "");
        }
    }
}

static void session_format_separator_line(session_ctx_t *ctx, const char *label,
                                          char *out, size_t length)
{
    if (out == nullptr || length == 0U) {
        return;
    }

    out[0] = '\0';

    if (ctx == nullptr || label == nullptr) {
        return;
    }

    const char *fg = ctx->system_fg_code != nullptr ? ctx->system_fg_code : "";
    const char *hl =
        ctx->system_highlight_code != nullptr ? ctx->system_highlight_code : "";
    const char *bold = ctx->system_is_bold ? ANSI_BOLD : "";

    size_t total_width = ctx->terminal_width > 0U ? ctx->terminal_width : 80U;
    if (total_width > SSH_CHATTER_MESSAGE_LIMIT) {
        total_width = SSH_CHATTER_MESSAGE_LIMIT;
    }
    char label_block[96];
    snprintf(label_block, sizeof(label_block), " %s ", label);
    size_t label_len = strnlen(label_block, sizeof(label_block) - 1U);
    if (label_len > total_width) {
        label_len = total_width;
        label_block[label_len] = '\0';
    }

    size_t dash_total = total_width > label_len ? total_width - label_len : 0U;
    size_t left = dash_total / 2U;
    size_t right = dash_total - left;

    enum { SESSION_SEPARATOR_BODY_LEN = SSH_CHATTER_MESSAGE_LIMIT - 16 };
    char body[SESSION_SEPARATOR_BODY_LEN];
    size_t max_body = sizeof(body);
    if (length > 0U && length < max_body) {
        max_body = length;
    }
    size_t offset = 0U;
    for (size_t idx = 0U; idx < left && offset + 1U < max_body; ++idx) {
        body[offset++] = '-';
    }
    if (offset + 1U < max_body) {
        size_t copy_limit = max_body - offset - 1U;
        size_t copy_len = label_len < copy_limit ? label_len : copy_limit;
        if (copy_len > 0U) {
            memcpy(body + offset, label_block, copy_len);
            offset += copy_len;
        }
    }
    for (size_t idx = 0U; idx < right && offset + 1U < max_body; ++idx) {
        body[offset++] = '-';
    }
    body[offset < max_body ? offset : max_body - 1U] = '\0';

    snprintf(out, length, "%s%s%s%.*s%s", hl, fg, bold,
             SESSION_SEPARATOR_BODY_LEN - 1, body, ANSI_RESET);
}

static void session_render_separator(session_ctx_t *ctx, const char *label)
{
    if (ctx == nullptr || label == nullptr) {
        return;
    }

    char line[SSH_CHATTER_MESSAGE_LIMIT];
    session_format_separator_line(ctx, label, line, sizeof(line));
    if (line[0] != '\0') {
        session_send_line(ctx, line);
    }
}

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
        session_channel_write(ctx, "\r\n", 2U);
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
        session_channel_write(ctx, "\r\n", 2U);
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

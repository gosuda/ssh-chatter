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

    return session_scrollback_buffer_acquire(ctx, target, out_capacity);
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

    display_model_release_visible(&frame);

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

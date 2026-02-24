/**
 * @file display_model.c
 * @desc Implementation of the display model pipeline for the terminal chat UI.
 *       Converts append-only message log into wrapped display lines, manages
 *       view state with a stable anchor, and provides full-frame redraws.
 */

#include "ssh_chatter/display_model.h"

#include <stdlib.h>
#include <string.h>

/* ---- Internal helpers ---- */

/**
 * @desc Count the display width of a UTF-8 character starting at *p.
 *       Returns the number of columns consumed and advances *p past the char.
 *       ANSI escape sequences (\033[...m) consume 0 columns.
 */
static unsigned int utf8_char_width(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;

    /* Skip ANSI escape sequences (CSI: ESC [ ... final byte) */
    if (s[0] == 0x1B && s[1] == '[') {
        s += 2;
        while (*s != '\0' && *s < 0x40) {
            ++s;
        }
        if (*s != '\0') {
            ++s; /* skip final byte */
        }
        *p = (const char *)s;
        return 0U;
    }

    /* ASCII */
    if (s[0] < 0x80) {
        *p = (const char *)(s + 1);
        return (s[0] >= 0x20) ? 1U : 0U; /* control chars are zero-width */
    }

    /* Multi-byte UTF-8: determine byte length and approximate display width.
     * CJK characters (U+1100..U+FFDC, U+20000..U+2FFFD, etc.) are typically
     * double-width. For simplicity we treat 3-byte and 4-byte sequences as
     * potentially double-width if they fall in common CJK ranges. */
    unsigned int byte_len = 1U;
    unsigned int display_cols = 1U;

    if ((s[0] & 0xE0) == 0xC0) {
        byte_len = 2U;
        display_cols = 1U;
    } else if ((s[0] & 0xF0) == 0xE0) {
        byte_len = 3U;
        /* Decode code point to determine width */
        unsigned int cp = ((unsigned int)(s[0] & 0x0F) << 12) |
                          ((unsigned int)(s[1] & 0x3F) << 6) |
                          (unsigned int)(s[2] & 0x3F);
        /* Common double-width ranges */
        if ((cp >= 0x1100 && cp <= 0x115F) ||
            cp == 0x2329 || cp == 0x232A ||
            (cp >= 0x2E80 && cp <= 0x303E) ||
            (cp >= 0x3040 && cp <= 0xA4CF) ||
            (cp >= 0xAC00 && cp <= 0xD7A3) ||
            (cp >= 0xF900 && cp <= 0xFAFF) ||
            (cp >= 0xFE10 && cp <= 0xFE6F) ||
            (cp >= 0xFF01 && cp <= 0xFF60) ||
            (cp >= 0xFFE0 && cp <= 0xFFE6)) {
            display_cols = 2U;
        }
    } else if ((s[0] & 0xF8) == 0xF0) {
        byte_len = 4U;
        /* Most 4-byte characters (supplementary planes) including CJK ext. */
        unsigned int cp = ((unsigned int)(s[0] & 0x07) << 18) |
                          ((unsigned int)(s[1] & 0x3F) << 12) |
                          ((unsigned int)(s[2] & 0x3F) << 6) |
                          (unsigned int)(s[3] & 0x3F);
        if (cp >= 0x20000 && cp <= 0x2FFFD) {
            display_cols = 2U;
        }
    }

    /* Validate continuation bytes */
    for (unsigned int i = 1U; i < byte_len && s[i] != '\0'; ++i) {
        if ((s[i] & 0xC0) != 0x80) {
            byte_len = i;
            break;
        }
    }

    *p = (const char *)(s + byte_len);
    return display_cols;
}

/**
 * @desc Ensure the model has room for at least needed_capacity display lines.
 */
static bool display_model_ensure_capacity(display_model_t *model,
                                          size_t needed_capacity)
{
    if (model->line_capacity >= needed_capacity) {
        return true;
    }

    size_t new_cap = model->line_capacity;
    if (new_cap == 0U) {
        new_cap = 64U;
    }
    while (new_cap < needed_capacity) {
        if (new_cap > SIZE_MAX / 2U) {
            return false;
        }
        new_cap *= 2U;
    }

    display_line_t *new_lines =
        (display_line_t *)realloc(model->lines, new_cap * sizeof(display_line_t));
    if (new_lines == NULL) {
        return false;
    }

    model->lines = new_lines;
    model->line_capacity = new_cap;
    return true;
}

/* ---- Public API ---- */

bool display_model_init(display_model_t *model, size_t initial_capacity)
{
    if (model == NULL) {
        return false;
    }

    memset(model, 0, sizeof(*model));
    model->view.mode = VIEW_FOLLOW_TAIL;

    if (initial_capacity == 0U) {
        initial_capacity = 64U;
    }

    model->lines =
        (display_line_t *)calloc(initial_capacity, sizeof(display_line_t));
    if (model->lines == NULL) {
        return false;
    }

    model->line_capacity = initial_capacity;
    return true;
}

void display_model_destroy(display_model_t *model)
{
    if (model == NULL) {
        return;
    }

    free(model->lines);
    memset(model, 0, sizeof(*model));
}

size_t display_model_wrap_line(const char *text, unsigned int width,
                               uint64_t message_id,
                               display_line_t *out, size_t out_capacity)
{
    if (text == NULL || width == 0U || out == NULL || out_capacity == 0U) {
        return 0U;
    }

    size_t produced = 0U;
    const char *cursor = text;
    uint16_t subline = 0U;

    while (*cursor != '\0' && produced < out_capacity) {
        display_line_t *line = &out[produced];
        line->message_id = message_id;
        line->subline_index = subline;

        unsigned int col = 0U;
        size_t out_pos = 0U;
        const char *line_start = cursor;

        /* Handle explicit newlines in messages */
        while (*cursor != '\0' && *cursor != '\n') {
            const char *prev = cursor;
            unsigned int cw = utf8_char_width(&cursor);

            if (col + cw > width && col > 0U) {
                /* Wrap: revert this character and start a new display line */
                cursor = prev;
                break;
            }

            size_t byte_len = (size_t)(cursor - prev);
            if (out_pos + byte_len < DISPLAY_LINE_TEXT_MAX - 1U) {
                memcpy(line->text + out_pos, prev, byte_len);
                out_pos += byte_len;
            }
            col += cw;
        }

        line->text[out_pos] = '\0';

        /* Skip newline if that's why we stopped */
        if (*cursor == '\n') {
            ++cursor;
        }

        ++produced;
        ++subline;
        (void)line_start;
    }

    /* If text was empty, produce one empty line */
    if (produced == 0U && out_capacity > 0U) {
        out[0].message_id = message_id;
        out[0].subline_index = 0;
        out[0].text[0] = '\0';
        produced = 1U;
    }

    return produced;
}

bool display_model_recompute_layout(display_model_t *model,
                                    const uint64_t *message_ids,
                                    const char *const *texts,
                                    size_t count,
                                    unsigned int width)
{
    if (model == NULL) {
        return false;
    }

    model->line_count = 0U;
    model->layout_width = width;

    if (count == 0U || message_ids == NULL || texts == NULL || width == 0U) {
        model->dirty = false;
        return true;
    }

    /* Temporary buffer for wrapping a single message */
    display_line_t tmp[256];

    for (size_t i = 0U; i < count; ++i) {
        size_t wrapped = display_model_wrap_line(
            texts[i], width, message_ids[i], tmp, 256U);

        if (!display_model_ensure_capacity(model,
                                           model->line_count + wrapped)) {
            return false;
        }

        memcpy(model->lines + model->line_count, tmp,
               wrapped * sizeof(display_line_t));
        model->line_count += wrapped;
    }

    model->dirty = false;
    return true;
}

bool display_model_append_message(display_model_t *model,
                                  uint64_t message_id,
                                  const char *text,
                                  unsigned int width)
{
    if (model == NULL || text == NULL || width == 0U) {
        return false;
    }

    display_line_t tmp[256];
    size_t wrapped = display_model_wrap_line(text, width, message_id, tmp, 256U);

    if (!display_model_ensure_capacity(model, model->line_count + wrapped)) {
        return false;
    }

    memcpy(model->lines + model->line_count, tmp,
           wrapped * sizeof(display_line_t));
    model->line_count += wrapped;
    model->layout_width = width;

    return true;
}

size_t display_model_resolve_anchor(const display_model_t *model,
                                    const display_view_anchor_t *anchor)
{
    if (model == NULL || anchor == NULL || model->line_count == 0U) {
        return SIZE_MAX;
    }

    /* Search for exact match */
    for (size_t i = 0U; i < model->line_count; ++i) {
        if (model->lines[i].message_id == anchor->message_id &&
            model->lines[i].subline_index == anchor->subline_index) {
            return i;
        }
    }

    /* If exact subline not found, find the first subline of this message */
    for (size_t i = 0U; i < model->line_count; ++i) {
        if (model->lines[i].message_id == anchor->message_id) {
            return i;
        }
    }

    /* Anchor message was deleted or evicted: find the closest message_id */
    uint64_t target = anchor->message_id;
    for (size_t i = 0U; i < model->line_count; ++i) {
        if (model->lines[i].message_id >= target) {
            return i;
        }
    }

    /* All messages are older than anchor: clamp to end */
    return model->line_count - 1U;
}

void display_model_compute_visible(const display_model_t *model,
                                   unsigned int viewport_height,
                                   display_visible_frame_t *frame)
{
    if (model == NULL || frame == NULL) {
        if (frame != NULL) {
            memset(frame, 0, sizeof(*frame));
        }
        return;
    }

    memset(frame, 0, sizeof(*frame));

    if (model->line_count == 0U || viewport_height == 0U) {
        frame->at_tail = true;
        frame->at_head = true;
        return;
    }

    size_t vp = (size_t)viewport_height;
    size_t total = model->line_count;

    if (model->view.mode == VIEW_FOLLOW_TAIL) {
        /* Show the last vp lines */
        size_t start = (total > vp) ? (total - vp) : 0U;
        size_t count = total - start;

        frame->lines = model->lines + start;
        frame->count = count;
        frame->first_global_index = start;
        frame->at_tail = true;
        frame->at_head = (start == 0U);
    } else {
        /* Manual scroll: resolve anchor to find the top line */
        size_t anchor_idx = display_model_resolve_anchor(
            model, &model->view.anchor);

        if (anchor_idx == SIZE_MAX) {
            anchor_idx = (total > vp) ? (total - vp) : 0U;
        }

        /* Clamp so we don't go past the end */
        if (anchor_idx + vp > total) {
            anchor_idx = (total > vp) ? (total - vp) : 0U;
        }

        size_t count = total - anchor_idx;
        if (count > vp) {
            count = vp;
        }

        frame->lines = model->lines + anchor_idx;
        frame->count = count;
        frame->first_global_index = anchor_idx;
        frame->at_tail = (anchor_idx + count >= total);
        frame->at_head = (anchor_idx == 0U);
    }
}

void display_model_scroll_up(display_model_t *model, size_t lines)
{
    if (model == NULL || model->line_count == 0U || lines == 0U) {
        return;
    }

    size_t current_idx;

    if (model->view.mode == VIEW_FOLLOW_TAIL) {
        /* Switch from tail to manual scroll. Anchor at current top. */
        model->view.mode = VIEW_MANUAL_SCROLL;
        /* Set anchor to the line that would be the top of the current view.
         * Since we don't know viewport_height here, we anchor at the last
         * line and the caller should recompute visible. For now, anchor at
         * the end of the model. */
        if (model->line_count > 0U) {
            current_idx = model->line_count - 1U;
        } else {
            current_idx = 0U;
        }
    } else {
        current_idx = display_model_resolve_anchor(
            model, &model->view.anchor);
        if (current_idx == SIZE_MAX) {
            current_idx = model->line_count - 1U;
        }
    }

    /* Move up */
    if (current_idx >= lines) {
        current_idx -= lines;
    } else {
        current_idx = 0U;
    }

    model->view.anchor.message_id = model->lines[current_idx].message_id;
    model->view.anchor.subline_index =
        model->lines[current_idx].subline_index;
}

void display_model_scroll_down(display_model_t *model, size_t lines)
{
    if (model == NULL || model->line_count == 0U || lines == 0U) {
        return;
    }

    if (model->view.mode == VIEW_FOLLOW_TAIL) {
        return; /* Already at the bottom */
    }

    size_t current_idx = display_model_resolve_anchor(
        model, &model->view.anchor);
    if (current_idx == SIZE_MAX) {
        display_model_follow_tail(model);
        return;
    }

    current_idx += lines;
    if (current_idx >= model->line_count) {
        /* Reached the end: switch back to tail-follow */
        display_model_follow_tail(model);
        return;
    }

    model->view.anchor.message_id = model->lines[current_idx].message_id;
    model->view.anchor.subline_index =
        model->lines[current_idx].subline_index;
}

void display_model_follow_tail(display_model_t *model)
{
    if (model == NULL) {
        return;
    }

    model->view.mode = VIEW_FOLLOW_TAIL;
    model->view.anchor.message_id = 0U;
    model->view.anchor.subline_index = 0U;
}

bool display_model_is_following_tail(const display_model_t *model)
{
    if (model == NULL) {
        return true;
    }

    return model->view.mode == VIEW_FOLLOW_TAIL;
}

size_t display_model_total_lines(const display_model_t *model)
{
    if (model == NULL) {
        return 0U;
    }

    return model->line_count;
}

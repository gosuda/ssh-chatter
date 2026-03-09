/**
 * @file display_model.h
 * @desc Display model for the terminal BBS/chat UI. Provides a pipeline
 *       that converts an append-only message log into display lines,
 *       manages view state (tail-follow vs manual scroll with stable
 *       anchor), and exposes visible lines for efficient redraws.
 *
 *       Invariants:
 *       - Messages are stored as an append-only log (single source of truth).
 *       - Rendering consumes visible display lines, using incremental patching
 *         when possible and full redraw as a fallback.
 *       - Wrapping/layout produces display-lines; scrolling operates on them.
 *       - View state uses a stable anchor (message_id, subline_index).
 */

#ifndef SSH_CHATTER_DISPLAY_MODEL_H
#define SSH_CHATTER_DISPLAY_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Maximum length of a single display line text. */
#define DISPLAY_LINE_TEXT_MAX 4096

/**
 * @desc A single rendered display line, linked back to its source message.
 */
typedef struct display_line {
    uint64_t message_id;
    uint16_t subline_index;
    char text[DISPLAY_LINE_TEXT_MAX];
} display_line_t;

/**
 * @desc View mode: either auto-scrolling to the newest content, or manually
 *       scrolled to a stable anchor position.
 */
typedef enum {
    VIEW_FOLLOW_TAIL,
    VIEW_MANUAL_SCROLL
} display_view_mode_t;

/**
 * @desc Stable anchor identifying a position in the display line array.
 *       Used by manual scroll to avoid jumping when new messages arrive.
 */
typedef struct display_view_anchor {
    uint64_t message_id;
    uint16_t subline_index;
} display_view_anchor_t;

/**
 * @desc View state controlling what portion of the display lines is visible.
 */
typedef struct display_view_state {
    display_view_mode_t mode;
    display_view_anchor_t anchor;
} display_view_state_t;

/**
 * @desc Display model: manages layout (message wrapping) and view state.
 *       The renderer reads an immutable snapshot of visible lines from this.
 */
typedef struct display_model {
    display_line_t *lines;
    size_t line_count;
    size_t line_capacity;
    unsigned int layout_width;
    bool dirty;
    display_view_state_t view;
} display_model_t;

/**
 * @desc Visible frame: the slice of display lines that should be rendered.
 *       Produced by display_model_compute_visible().
 */
typedef struct display_visible_frame {
    const display_line_t *lines;
    size_t count;
    size_t first_global_index;
    bool at_tail;
    bool at_head;
} display_visible_frame_t;

/* ---- Lifecycle ---- */

/**
 * @desc Initialize a display model, allocating internal storage.
 * @param model Model to initialize.
 * @param initial_capacity Initial number of display lines to allocate.
 * @return true on success, false on allocation failure.
 */
bool display_model_init(display_model_t *model, size_t initial_capacity);

/**
 * @desc Free all resources held by a display model.
 * @param model Model to destroy.
 */
void display_model_destroy(display_model_t *model);

/* ---- Layout ---- */

/**
 * @desc Recompute display lines from formatted message strings.
 *       Each message is wrapped to the given terminal width.
 *       message_ids, texts, and count describe the append-only log.
 * @param model      Display model to update.
 * @param message_ids Array of message IDs (one per message).
 * @param texts      Array of pre-formatted message strings (one per message).
 * @param count      Number of messages.
 * @param width      Terminal width for wrapping.
 * @return true on success, false on allocation failure.
 */
bool display_model_recompute_layout(display_model_t *model,
                                    const uint64_t *message_ids,
                                    const char *const *texts,
                                    size_t count,
                                    unsigned int width);

/**
 * @desc Append a single new message to the layout without full recomputation.
 *       Used for incremental updates when a new message arrives.
 * @param model      Display model to update.
 * @param message_id Message ID of the new message.
 * @param text       Pre-formatted message string.
 * @param width      Terminal width for wrapping.
 * @return true on success, false on allocation failure.
 */
bool display_model_append_message(display_model_t *model,
                                  uint64_t message_id,
                                  const char *text,
                                  unsigned int width);

/* ---- View / Scroll ---- */

/**
 * @desc Compute the visible frame of display lines for a given viewport.
 * @param model           Display model to read from.
 * @param viewport_height Number of lines the terminal can show.
 * @param frame           Output: visible frame descriptor.
 */
void display_model_compute_visible(const display_model_t *model,
                                   unsigned int viewport_height,
                                   display_visible_frame_t *frame);

/**
 * @desc Scroll up (toward older messages) by a given number of display lines.
 * @param model Display model.
 * @param lines Number of display lines to scroll up.
 */
void display_model_scroll_up(display_model_t *model, size_t lines);

/**
 * @desc Scroll down (toward newer messages) by a given number of display lines.
 * @param model Display model.
 * @param lines Number of display lines to scroll down.
 */
void display_model_scroll_down(display_model_t *model, size_t lines);

/**
 * @desc Switch to tail-follow mode: the view auto-scrolls to the newest lines.
 * @param model Display model.
 */
void display_model_follow_tail(display_model_t *model);

/**
 * @desc Check if the model is currently in tail-follow mode.
 * @param model Display model.
 * @return true if tail-following.
 */
bool display_model_is_following_tail(const display_model_t *model);

/**
 * @desc Resolve an anchor to a global display line index.
 *       Returns SIZE_MAX if the anchor is not found.
 * @param model  Display model.
 * @param anchor Anchor to resolve.
 * @return Global display line index, or SIZE_MAX.
 */
size_t display_model_resolve_anchor(const display_model_t *model,
                                    const display_view_anchor_t *anchor);

/**
 * @desc Get the total number of display lines in the model.
 * @param model Display model.
 * @return Total display line count.
 */
size_t display_model_total_lines(const display_model_t *model);

/* ---- Utility ---- */

/**
 * @desc Wrap a single message string into display lines at a given width.
 *       Handles multi-byte UTF-8 correctly. Does not modify the model.
 * @param text         Input message string.
 * @param width        Terminal width (columns).
 * @param message_id   Message ID to tag each output line with.
 * @param out          Output array of display lines.
 * @param out_capacity Maximum number of display lines to produce.
 * @return Number of display lines produced.
 */
size_t display_model_wrap_line(const char *text, unsigned int width,
                               uint64_t message_id,
                               display_line_t *out, size_t out_capacity);

#endif /* SSH_CHATTER_DISPLAY_MODEL_H */

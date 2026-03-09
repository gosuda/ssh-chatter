/**
 * @file display_model_test.c
 * @desc Regression tests for the display model pipeline.
 *       Exercises wrapping, layout, view state, scrolling and tail-follow.
 *
 *       Tests assert:
 *       - After replay, visible buffer contains more than just the last message.
 *       - Scrolling up does not jump to tail on new messages.
 *       - Tail-follow shows the latest lines reliably.
 *       - UTF-8 and long-line wrapping produce correct display lines.
 *       - Resize (re-layout with new width) preserves anchor.
 */

#include "ssh_chatter/display_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ---- Helpers ---- */

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d): %s\n", __func__, __LINE__, msg); \
        ++g_tests_failed; \
        return; \
    } \
} while (0)

#define TEST_ASSERT_EQ(a, b, msg) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "  FAIL: %s (line %d): %s (got %zu, expected %zu)\n", \
                __func__, __LINE__, msg, (size_t)(a), (size_t)(b)); \
        ++g_tests_failed; \
        return; \
    } \
} while (0)

/* ---- Test: basic init/destroy ---- */

static void test_init_destroy(void)
{
    display_model_t model;
    bool ok = display_model_init(&model, 16);
    TEST_ASSERT(ok, "init should succeed");
    TEST_ASSERT_EQ(model.line_count, 0U, "line_count should be 0");
    TEST_ASSERT(model.view.mode == VIEW_FOLLOW_TAIL, "should start in tail mode");
    display_model_destroy(&model);
    ++g_tests_passed;
}

/* ---- Test: wrap single short line ---- */

static void test_wrap_short_line(void)
{
    display_line_t out[8];
    size_t n = display_model_wrap_line("hello world", 80, 1, out, 8);
    TEST_ASSERT_EQ(n, 1U, "short line should produce 1 display line");
    TEST_ASSERT(strcmp(out[0].text, "hello world") == 0,
                "text should match");
    TEST_ASSERT_EQ(out[0].message_id, 1U, "message_id should be 1");
    TEST_ASSERT_EQ(out[0].subline_index, 0U, "subline should be 0");
    ++g_tests_passed;
}

/* ---- Test: wrap long line ---- */

static void test_wrap_long_line(void)
{
    /* Create a 100-char ASCII string, wrap at width 40 */
    char text[101];
    for (int i = 0; i < 100; ++i) text[i] = 'A' + (char)(i % 26);
    text[100] = '\0';

    display_line_t out[16];
    size_t n = display_model_wrap_line(text, 40, 42, out, 16);
    TEST_ASSERT(n >= 3, "100 chars at width 40 should wrap to >= 3 lines");

    /* All lines should reference the same message */
    for (size_t i = 0; i < n; ++i) {
        TEST_ASSERT_EQ(out[i].message_id, 42U, "message_id should be 42");
        TEST_ASSERT_EQ(out[i].subline_index, (uint16_t)i, "subline should match");
    }

    /* Concatenation of all line texts should equal the original */
    char reconstructed[512] = {0};
    for (size_t i = 0; i < n; ++i) {
        strncat(reconstructed, out[i].text,
                sizeof(reconstructed) - strlen(reconstructed) - 1);
    }
    TEST_ASSERT(strcmp(reconstructed, text) == 0,
                "concatenated lines should match original");
    ++g_tests_passed;
}

/* ---- Test: wrap with explicit newlines ---- */

static void test_wrap_newlines(void)
{
    const char *text = "line one\nline two\nline three";
    display_line_t out[8];
    size_t n = display_model_wrap_line(text, 80, 10, out, 8);
    TEST_ASSERT_EQ(n, 3U, "3 newline-separated lines");
    TEST_ASSERT(strcmp(out[0].text, "line one") == 0, "line 1");
    TEST_ASSERT(strcmp(out[1].text, "line two") == 0, "line 2");
    TEST_ASSERT(strcmp(out[2].text, "line three") == 0, "line 3");
    ++g_tests_passed;
}

/* ---- Test: wrap UTF-8 multibyte ---- */

static void test_wrap_utf8(void)
{
    /* Each CJK character is 3 bytes and 2 columns wide.
     * 5 characters = 10 columns. With width=6, should wrap. */
    const char *text = "\xe4\xb8\x80\xe4\xba\x8c\xe4\xb8\x89\xe5\x9b\x9b\xe4\xba\x94";
    display_line_t out[8];
    size_t n = display_model_wrap_line(text, 6, 99, out, 8);
    TEST_ASSERT(n >= 2, "CJK at width 6 should wrap");
    ++g_tests_passed;
}

/* ---- Test: ANSI escape sequences don't consume columns ---- */

static void test_wrap_ansi_escapes(void)
{
    /* "\033[31mRED\033[0m" = the word "RED" in red */
    const char *text = "\033[31mRED\033[0m";
    display_line_t out[4];
    size_t n = display_model_wrap_line(text, 10, 1, out, 4);
    TEST_ASSERT_EQ(n, 1U, "ANSI colored text should not wrap at width 10");
    TEST_ASSERT(strcmp(out[0].text, text) == 0, "text preserved");
    ++g_tests_passed;
}

/* ---- Test: recompute layout with multiple messages ---- */

static void test_recompute_layout(void)
{
    display_model_t model;
    display_model_init(&model, 16);

    const char *texts[] = { "msg one", "msg two", "msg three" };
    uint64_t ids[] = { 1, 2, 3 };

    bool ok = display_model_recompute_layout(&model, ids, texts, 3, 80);
    TEST_ASSERT(ok, "layout should succeed");
    TEST_ASSERT_EQ(model.line_count, 3U, "3 short messages = 3 display lines");

    display_model_destroy(&model);
    ++g_tests_passed;
}

/* ---- Test: tail-follow shows latest lines ---- */

static void test_tail_follow(void)
{
    display_model_t model;
    display_model_init(&model, 64);

    /* Add 20 messages */
    uint64_t ids[20];
    const char *texts[20];
    char buf[20][32];
    for (int i = 0; i < 20; ++i) {
        ids[i] = (uint64_t)(i + 1);
        snprintf(buf[i], sizeof(buf[i]), "message %d", i + 1);
        texts[i] = buf[i];
    }

    display_model_recompute_layout(&model, ids, texts, 20, 80);
    TEST_ASSERT(display_model_is_following_tail(&model),
                "should be in tail mode");

    /* With viewport of 5 lines, should see messages 16-20 */
    display_visible_frame_t frame;
    display_model_compute_visible(&model, 5, &frame);

    TEST_ASSERT_EQ(frame.count, 5U, "visible should be 5 lines");
    TEST_ASSERT(frame.at_tail, "should be at tail");
    TEST_ASSERT(strcmp(frame.lines[4].text, "message 20") == 0,
                "last visible should be message 20");
    TEST_ASSERT(strcmp(frame.lines[0].text, "message 16") == 0,
                "first visible should be message 16");

    display_model_destroy(&model);
    ++g_tests_passed;
}

/* ---- Test: after replay, visible buffer has more than just the last message ---- */

static void test_replay_visible_buffer(void)
{
    display_model_t model;
    display_model_init(&model, 256);

    /* Simulate a burst of 100 messages + some join/leave events */
    const int total_msgs = 110;
    uint64_t ids[110];
    const char *texts[110];
    char buf[110][64];

    for (int i = 0; i < total_msgs; ++i) {
        ids[i] = (uint64_t)(i + 1);
        if (i % 20 == 0) {
            snprintf(buf[i], sizeof(buf[i]), "[user%d joined]", i / 20);
        } else if (i % 20 == 19) {
            snprintf(buf[i], sizeof(buf[i]), "[user%d left]", i / 20);
        } else {
            snprintf(buf[i], sizeof(buf[i]), "chat message number %d", i);
        }
        texts[i] = buf[i];
    }

    display_model_recompute_layout(&model, ids, texts, (size_t)total_msgs, 80);

    /* With a 24-line viewport, we should see the last 24 messages */
    display_visible_frame_t frame;
    display_model_compute_visible(&model, 24, &frame);

    TEST_ASSERT(frame.count > 1U,
                "visible buffer must contain more than just the last message");
    TEST_ASSERT_EQ(frame.count, 24U, "should see 24 lines");
    TEST_ASSERT(frame.at_tail, "should be at tail");

    /* The last line should be the last message */
    TEST_ASSERT(frame.lines[frame.count - 1].message_id == (uint64_t)total_msgs,
                "last visible should be the last message");

    display_model_destroy(&model);
    ++g_tests_passed;
}

/* ---- Test: scroll up does not jump to tail on new messages ---- */

static void test_scroll_up_stable(void)
{
    display_model_t model;
    display_model_init(&model, 128);

    /* Add 50 messages */
    uint64_t ids[50];
    const char *texts[50];
    char buf[50][32];
    for (int i = 0; i < 50; ++i) {
        ids[i] = (uint64_t)(i + 1);
        snprintf(buf[i], sizeof(buf[i]), "msg %d", i + 1);
        texts[i] = buf[i];
    }

    display_model_recompute_layout(&model, ids, texts, 50, 80);

    /* Scroll up by 10 lines */
    display_model_scroll_up(&model, 10);
    TEST_ASSERT(!display_model_is_following_tail(&model),
                "should be in manual scroll mode after scroll up");

    /* Record the anchor */
    display_view_anchor_t saved_anchor = model.view.anchor;

    /* Simulate new messages arriving (append 5 more) */
    for (int i = 0; i < 5; ++i) {
        char msg[32];
        snprintf(msg, sizeof(msg), "new msg %d", 51 + i);
        display_model_append_message(&model, (uint64_t)(51 + i), msg, 80);
    }

    /* The view should still be in manual scroll with the same anchor */
    TEST_ASSERT(!display_model_is_following_tail(&model),
                "new messages should not switch to tail mode");
    TEST_ASSERT(model.view.anchor.message_id == saved_anchor.message_id,
                "anchor message_id should be stable after new messages");
    TEST_ASSERT(model.view.anchor.subline_index == saved_anchor.subline_index,
                "anchor subline should be stable after new messages");

    /* Compute visible: should still show the same anchored region */
    display_visible_frame_t frame;
    display_model_compute_visible(&model, 10, &frame);
    TEST_ASSERT(!frame.at_tail, "should NOT be at tail");
    TEST_ASSERT(frame.count > 0U, "should have visible lines");

    display_model_destroy(&model);
    ++g_tests_passed;
}

/* ---- Test: scroll down past end returns to tail ---- */

static void test_scroll_down_to_tail(void)
{
    display_model_t model;
    display_model_init(&model, 64);

    uint64_t ids[10];
    const char *texts[10];
    char buf[10][16];
    for (int i = 0; i < 10; ++i) {
        ids[i] = (uint64_t)(i + 1);
        snprintf(buf[i], sizeof(buf[i]), "m%d", i + 1);
        texts[i] = buf[i];
    }

    display_model_recompute_layout(&model, ids, texts, 10, 80);

    /* Scroll up then scroll down past end */
    display_model_scroll_up(&model, 5);
    TEST_ASSERT(!display_model_is_following_tail(&model), "manual scroll");

    display_model_scroll_down(&model, 100);
    TEST_ASSERT(display_model_is_following_tail(&model),
                "scrolling down past end should return to tail");

    display_model_destroy(&model);
    ++g_tests_passed;
}

/* ---- Test: resize re-layout preserves approximate position ---- */

static void test_resize_relayout(void)
{
    display_model_t model;
    display_model_init(&model, 128);

    /* Add messages with varying lengths */
    const char *texts[] = {
        "short",
        "this is a medium length message that should wrap at narrow widths easily",
        "tiny"
    };
    uint64_t ids[] = { 1, 2, 3 };

    /* Layout at width 80 */
    display_model_recompute_layout(&model, ids, texts, 3, 80);
    size_t lines_at_80 = model.line_count;

    /* Scroll up to anchor on message 2 */
    display_model_scroll_up(&model, 1);
    uint64_t anchor_msg = model.view.anchor.message_id;

    /* Re-layout at width 40 (narrower) */
    display_model_recompute_layout(&model, ids, texts, 3, 40);
    size_t lines_at_40 = model.line_count;

    TEST_ASSERT(lines_at_40 >= lines_at_80,
                "narrower width should produce same or more lines");

    /* Anchor should still resolve to message 2's line */
    size_t resolved = display_model_resolve_anchor(&model, &model.view.anchor);
    TEST_ASSERT(resolved != SIZE_MAX, "anchor should still resolve");
    TEST_ASSERT(model.lines[resolved].message_id == anchor_msg,
                "resolved anchor should point to same message");

    display_model_destroy(&model);
    ++g_tests_passed;
}

/* ---- Test: append_message keeps layout consistent ---- */

static void test_append_message(void)
{
    display_model_t model;
    display_model_init(&model, 64);

    /* Start with 3 messages */
    const char *texts[] = { "first", "second", "third" };
    uint64_t ids[] = { 1, 2, 3 };
    display_model_recompute_layout(&model, ids, texts, 3, 80);
    TEST_ASSERT_EQ(model.line_count, 3U, "3 messages");

    /* Append one more */
    display_model_append_message(&model, 4, "fourth", 80);
    TEST_ASSERT_EQ(model.line_count, 4U, "4 messages after append");
    TEST_ASSERT(strcmp(model.lines[3].text, "fourth") == 0,
                "appended message text");

    display_model_destroy(&model);
    ++g_tests_passed;
}

/* ---- Test: tail append shifts visible window by one line ---- */

static void test_tail_append_shifts_visible_window(void)
{
    display_model_t model;
    display_model_init(&model, 64);

    uint64_t ids[6];
    const char *texts[6];
    char buf[6][16];
    for (int i = 0; i < 6; ++i) {
        ids[i] = (uint64_t)(i + 1);
        snprintf(buf[i], sizeof(buf[i]), "msg %d", i + 1);
        texts[i] = buf[i];
    }

    display_model_recompute_layout(&model, ids, texts, 5, 80);

    display_visible_frame_t before;
    display_model_compute_visible(&model, 3, &before);
    TEST_ASSERT_EQ(before.count, 3U, "initial visible frame should use viewport");
    TEST_ASSERT(strcmp(before.lines[0].text, "msg 3") == 0,
                "viewport should start at msg 3");
    TEST_ASSERT(strcmp(before.lines[2].text, "msg 5") == 0,
                "viewport should end at msg 5");

    display_model_append_message(&model, ids[5], texts[5], 80);

    display_visible_frame_t after;
    display_model_compute_visible(&model, 3, &after);
    TEST_ASSERT_EQ(after.count, 3U, "visible frame size should remain stable");
    TEST_ASSERT(strcmp(after.lines[0].text, "msg 4") == 0,
                "tail append should shift the first visible line");
    TEST_ASSERT(strcmp(after.lines[2].text, "msg 6") == 0,
                "tail append should place new message at the bottom");

    display_model_destroy(&model);
    ++g_tests_passed;
}

/* ---- Test: empty model visible frame ---- */

static void test_empty_model(void)
{
    display_model_t model;
    display_model_init(&model, 8);

    display_visible_frame_t frame;
    display_model_compute_visible(&model, 24, &frame);
    TEST_ASSERT_EQ(frame.count, 0U, "empty model has 0 visible lines");
    TEST_ASSERT(frame.at_tail, "empty model is at tail");
    TEST_ASSERT(frame.at_head, "empty model is at head");

    display_model_destroy(&model);
    ++g_tests_passed;
}

/* ---- Test: long wrapping messages don't lose content ---- */

static void test_long_wrap_content_preserved(void)
{
    /* Create a very long message (800 chars), wrap at width 20 */
    char text[801];
    for (int i = 0; i < 800; ++i) text[i] = '0' + (char)(i % 10);
    text[800] = '\0';

    display_line_t out[128];
    size_t n = display_model_wrap_line(text, 20, 1, out, 128);
    TEST_ASSERT_EQ(n, 40U, "800 chars at width 20 = 40 lines");

    /* Verify all content preserved */
    char reconstructed[1024] = {0};
    for (size_t i = 0; i < n; ++i) {
        strncat(reconstructed, out[i].text,
                sizeof(reconstructed) - strlen(reconstructed) - 1);
    }
    TEST_ASSERT(strcmp(reconstructed, text) == 0, "content preserved");

    ++g_tests_passed;
}

/* ---- Test: burst scenario (join/leave + 100 msgs + UTF-8 + long wrap + resize) ---- */

static void test_burst_replay(void)
{
    display_model_t model;
    display_model_init(&model, 512);

    /* Build a mix of messages simulating the bug scenario */
    const int N = 120;
    uint64_t ids[120];
    const char *texts[120];
    char buf[120][256];

    for (int i = 0; i < N; ++i) {
        ids[i] = (uint64_t)(i + 1);

        if (i < 5) {
            /* Join burst */
            snprintf(buf[i], sizeof(buf[i]), "[user%d joined from 192.168.1.%d]", i, i);
        } else if (i >= 115) {
            /* Leave burst */
            snprintf(buf[i], sizeof(buf[i]), "[user%d left]", i - 115);
        } else if (i % 10 == 0) {
            /* UTF-8 message: CJK */
            snprintf(buf[i], sizeof(buf[i]),
                     "\xe4\xb8\x80\xe4\xba\x8c\xe4\xb8\x89 chat msg %d "
                     "\xe5\x9b\x9b\xe4\xba\x94\xe5\x85\xad", i);
        } else if (i % 15 == 0) {
            /* Long message that wraps */
            memset(buf[i], 'X', 200);
            buf[i][200] = '\0';
        } else {
            snprintf(buf[i], sizeof(buf[i]), "regular chat message number %d here", i);
        }
        texts[i] = buf[i];
    }

    /* Layout at width 80 */
    bool ok = display_model_recompute_layout(&model, ids, texts, (size_t)N, 80);
    TEST_ASSERT(ok, "layout should succeed");

    /* Visible frame should have many lines, not just the last message */
    display_visible_frame_t frame;
    display_model_compute_visible(&model, 24, &frame);
    TEST_ASSERT(frame.count > 1, "visible must have more than 1 line");
    TEST_ASSERT(frame.at_tail, "should be at tail");

    /* Scroll up */
    display_model_scroll_up(&model, 20);
    display_view_anchor_t anchor_before = model.view.anchor;

    /* Simulate resize: re-layout at width 40 */
    ok = display_model_recompute_layout(&model, ids, texts, (size_t)N, 40);
    TEST_ASSERT(ok, "relayout at 40 should succeed");

    /* Anchor should still resolve */
    size_t idx = display_model_resolve_anchor(&model, &anchor_before);
    TEST_ASSERT(idx != SIZE_MAX, "anchor should resolve after resize");

    /* Visible frame should still work */
    display_model_compute_visible(&model, 24, &frame);
    TEST_ASSERT(frame.count > 0, "should have visible lines after resize");

    display_model_destroy(&model);
    ++g_tests_passed;
}

/* ---- Main ---- */

int main(void)
{
    printf("Running display model tests...\n\n");

    test_init_destroy();
    test_wrap_short_line();
    test_wrap_long_line();
    test_wrap_newlines();
    test_wrap_utf8();
    test_wrap_ansi_escapes();
    test_recompute_layout();
    test_tail_follow();
    test_replay_visible_buffer();
    test_scroll_up_stable();
    test_scroll_down_to_tail();
    test_resize_relayout();
    test_append_message();
    test_tail_append_shifts_visible_window();
    test_empty_model();
    test_long_wrap_content_preserved();
    test_burst_replay();

    printf("\n%d passed, %d failed\n", g_tests_passed, g_tests_failed);

    return g_tests_failed > 0 ? 1 : 0;
}

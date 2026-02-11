/**
 * @file host_bbs_and_games.c
 * @desc File-level documentation for host_bbs_and_games.c, describing its role
 *       in the SSH-Chatter server and providing a consistent header
 *       comment format across C sources.
 * @return None.
 */

// BBS workflows plus interactive mini-games.
#include "host_internal.h"

static int session_game_random_range(session_ctx_t *ctx, int max);
static session_ctx_t *chat_room_find_user(chat_room_t *room,
                                          const char *username);

// Handle the /bbs command entry point.
static void session_handle_bbs(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (arguments == nullptr || *arguments == '\0') {
        session_bbs_show_dashboard(ctx);
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);
    if (working[0] == '\0') {
        session_bbs_show_dashboard(ctx);
        return;
    }

    char *command = working;
    char *rest = nullptr;
    for (char *cursor = working; *cursor != '\0'; ++cursor) {
        if (isspace((unsigned char)*cursor)) {
            *cursor = '\0';
            rest = cursor + 1;
            break;
        }
    }
    if (rest != nullptr) {
        trim_whitespace_inplace(rest);
    }

    if (strcmp(command, "exit") == 0) {
        ctx->in_bbs_mode = false;
        ctx->bbs_view_active = false;
        ctx->bbs_view_post_id = 0U;
        session_send_system_line(ctx, "Exited BBS mode.");
        return;
    }

    ctx->in_bbs_mode = true;

    const char *canonical_command =
        session_bbs_subcommand_canonicalize(ctx, command);
    if (canonical_command == nullptr) {
        session_send_system_line(
            ctx, "Unknown /bbs subcommand. Try /bbs for usage.");
        return;
    }

    if (strcmp(canonical_command, "list") == 0) {
        session_bbs_prepare_canvas(ctx);
        session_bbs_list(ctx);
    } else if (strcmp(canonical_command, "read") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_bbs_send_usage(ctx, "read", "<id>");
            return;
        }
        uint64_t id = (uint64_t)strtoull(rest, nullptr, 10);
        session_bbs_read(ctx, id);
    } else if (strcmp(canonical_command, "topic") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_bbs_send_usage(ctx, "topic", "read <tag>");
            return;
        }

        char topic_full[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(topic_full, sizeof(topic_full), "%s", rest);
        trim_whitespace_inplace(topic_full);
        if (topic_full[0] == '\0') {
            session_bbs_send_usage(ctx, "topic", "read <tag>");
            return;
        }

        char topic_args[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(topic_args, sizeof(topic_args), "%s", topic_full);

        char action_token[32];
        size_t action_len = 0U;
        char *cursor = topic_args;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
            if (action_len + 1U < sizeof(action_token)) {
                action_token[action_len++] = *cursor;
            }
            ++cursor;
        }
        action_token[action_len] = '\0';

        char *remaining = nullptr;
        if (*cursor != '\0') {
            *cursor = '\0';
            remaining = cursor + 1;
            trim_whitespace_inplace(remaining);
        }

        if (action_token[0] != '\0') {
            const char *canonical_action =
                session_bbs_subcommand_canonicalize(ctx, action_token);
            if (canonical_action != nullptr &&
                strcmp(canonical_action, "read") == 0) {
                if (remaining == nullptr || remaining[0] == '\0') {
                    session_bbs_send_usage(ctx, "topic", "read <tag>");
                    return;
                }
                session_bbs_prepare_canvas(ctx);
                session_bbs_list_topic(ctx, remaining);
                return;
            }
        }

        session_bbs_prepare_canvas(ctx);
        session_bbs_list_topic(ctx, topic_full);
    } else if (strcmp(canonical_command, "post") == 0) {
        session_bbs_begin_post(ctx, rest);
    } else if (strcmp(canonical_command, "edit") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_bbs_send_usage(ctx, "edit", "<id>");
            return;
        }
        uint64_t id = (uint64_t)strtoull(rest, nullptr, 10);
        session_bbs_begin_edit(ctx, id);
    } else if (strcmp(canonical_command, "comment") == 0) {
        session_bbs_add_comment(ctx, rest);
    } else if (strcmp(canonical_command, "regen") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_bbs_send_usage(ctx, "regen", "<id>");
            return;
        }
        uint64_t id = (uint64_t)strtoull(rest, nullptr, 10);
        session_bbs_regen_post(ctx, id);
    } else if (strcmp(canonical_command, "delete") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_bbs_send_usage(ctx, "delete", "<id>");
            return;
        }
        uint64_t id = (uint64_t)strtoull(rest, nullptr, 10);
        session_bbs_delete(ctx, id);
    } else {
        session_send_system_line(
            ctx, "Unknown /bbs subcommand. Try /bbs for usage.");
    }
}

static void session_game_seed_rng(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    if (ctx->game.rng_seeded) {
        return;
    }

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        ts.tv_sec = time(nullptr);
        ts.tv_nsec = 0L;
    }

    uint64_t seed = ((uint64_t)ts.tv_sec << 32) ^ (uint64_t)ts.tv_nsec ^
                    (uintptr_t)ctx ^ (uintptr_t)ctx->owner;
    if (seed == 0U) {
        seed = UINT64_C(0x9E3779B97F4A7C15);
    }
    ctx->game.rng_state = seed;
    ctx->game.rng_seeded = true;
}

static uint32_t session_game_random(session_ctx_t *ctx)
{
    session_game_seed_rng(ctx);
    uint64_t x = ctx->game.rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    ctx->game.rng_state = x;
    uint64_t result = x * UINT64_C(2685821657736338717);
    return (uint32_t)(result >> 32);
}

static int session_game_random_range(session_ctx_t *ctx, int max)
{
    if (max <= 0) {
        return 0;
    }
    return (int)(session_game_random(ctx) % (uint32_t)max);
}

static void session_game_tetris_reset(tetris_game_state_t *state)
{
    if (state == nullptr) {
        return;
    }

    memset(state->board, 0, sizeof(state->board));
    state->current_piece = -1;
    state->rotation = 0;
    state->row = 0;
    state->column = 0;
    state->next_piece = 0;
    state->score = 0U;
    state->lines_cleared = 0U;
    state->game_over = false;
    state->bag_index = 0U;
    for (size_t idx = 0U; idx < 7U; ++idx) {
        state->bag[idx] = (int)idx;
    }
    state->gravity_counter = 0U;
    state->gravity_rate = SSH_CHATTER_TETRIS_GRAVITY_RATE;
    state->gravity_timer_initialized = false;
    state->gravity_timer_last.tv_sec = 0;
    state->gravity_timer_last.tv_nsec = 0;
    state->gravity_timer_accumulator_ns = 0U;
    state->round = 1U;
    state->next_round_line_goal = SSH_CHATTER_TETRIS_LINES_PER_ROUND;
    session_game_tetris_apply_round_settings(state);
    state->input_escape_active = false;
    state->input_escape_length = 0U;
    memset(state->input_escape_buffer, 0, sizeof(state->input_escape_buffer));
}

static void session_game_tetris_apply_round_settings(tetris_game_state_t *state)
{
    if (state == nullptr) {
        return;
    }

    if (state->round == 0U) {
        state->round = 1U;
    }

    unsigned reduction = state->round > 0U ? state->round - 1U : 0U;
    unsigned base_threshold = SSH_CHATTER_TETRIS_GRAVITY_THRESHOLD;
    unsigned threshold = base_threshold;
    if (reduction >= base_threshold) {
        threshold = 1U;
    } else {
        threshold = base_threshold - reduction;
    }

    if (threshold == 0U) {
        threshold = 1U;
    }

    state->gravity_threshold = threshold;
    state->gravity_counter = 0U;
    state->gravity_timer_initialized = false;
    state->gravity_timer_last.tv_sec = 0;
    state->gravity_timer_last.tv_nsec = 0;
    state->gravity_timer_accumulator_ns = 0U;
}

static void session_game_tetris_fill_bag(session_ctx_t *ctx)
{
    tetris_game_state_t *state = session_game_ensure_tetris(ctx);
    if (state == nullptr) {
        return;
    }
    for (size_t idx = 0U; idx < 7U; ++idx) {
        state->bag[idx] = (int)idx;
    }
    for (int idx = 6; idx > 0; --idx) {
        int swap_index = session_game_random_range(ctx, idx + 1);
        int temp = state->bag[idx];
        state->bag[idx] = state->bag[swap_index];
        state->bag[swap_index] = temp;
    }
    state->bag_index = 0U;
}

static int session_game_tetris_take_piece(session_ctx_t *ctx)
{
    tetris_game_state_t *state = session_game_ensure_tetris(ctx);
    if (state == nullptr) {
        return 0;
    }
    if (state->bag_index >= 7U) {
        session_game_tetris_fill_bag(ctx);
    }
    return state->bag[state->bag_index++];
}

static bool session_game_tetris_cell_occupied(int piece, int rotation, int row,
                                              int column)
{
    if (piece < 0 || piece >= 7) {
        return false;
    }
    rotation = rotation & 3;
    if (row < 0 || row >= SSH_CHATTER_TETROMINO_SIZE || column < 0 ||
        column >= SSH_CHATTER_TETROMINO_SIZE) {
        return false;
    }
    const char *shape = TETROMINO_SHAPES[piece][rotation];
    char value = shape[row * SSH_CHATTER_TETROMINO_SIZE + column];
    return value != '.' && value != '\0';
}

static bool session_game_tetris_position_valid(const tetris_game_state_t *state,
                                               int piece, int rotation, int row,
                                               int column)
{
    if (state == nullptr) {
        return false;
    }
    for (int r = 0; r < SSH_CHATTER_TETROMINO_SIZE; ++r) {
        for (int c = 0; c < SSH_CHATTER_TETROMINO_SIZE; ++c) {
            if (!session_game_tetris_cell_occupied(piece, rotation, r, c)) {
                continue;
            }
            int board_row = row + r;
            int board_col = column + c;
            if (board_col < 0 || board_col >= SSH_CHATTER_TETRIS_WIDTH) {
                return false;
            }
            if (board_row >= SSH_CHATTER_TETRIS_HEIGHT) {
                return false;
            }
            if (board_row < 0) {
                continue;
            }
            if (state->board[board_row][board_col] != 0) {
                return false;
            }
        }
    }
    return true;
}

static bool session_game_tetris_spawn_piece(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }
    tetris_game_state_t *state = session_game_ensure_tetris(ctx);
    if (state == nullptr) {
        return false;
    }
    state->current_piece = state->next_piece;
    state->rotation = 0;
    state->row = 0;
    state->column = (SSH_CHATTER_TETRIS_WIDTH / 2) - 2;
    state->gravity_counter = 0U;
    state->gravity_timer_initialized = false;
    state->gravity_timer_accumulator_ns = 0U;
    state->input_escape_active = false;
    state->input_escape_length = 0U;
    state->next_piece = session_game_tetris_take_piece(ctx);
    if (!session_game_tetris_position_valid(state, state->current_piece,
                                            state->rotation, state->row,
                                            state->column)) {
        state->game_over = true;
        return false;
    }
    return true;
}

static bool session_game_tetris_move(session_ctx_t *ctx, int drow, int dcol)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_TETRIS) {
        return false;
    }
    tetris_game_state_t *state = session_game_ensure_tetris(ctx);
    if (state == nullptr) {
        return false;
    }
    if (state->current_piece < 0) {
        return false;
    }
    int new_row = state->row + drow;
    int new_col = state->column + dcol;
    if (!session_game_tetris_position_valid(
            state, state->current_piece, state->rotation, new_row, new_col)) {
        return false;
    }
    state->row = new_row;
    state->column = new_col;
    return true;
}

static bool session_game_tetris_soft_drop(session_ctx_t *ctx)
{
    if (session_game_tetris_move(ctx, 1, 0)) {
        return true;
    }
    session_game_tetris_lock_piece(ctx);
    return false;
}

static bool session_game_tetris_apply_gravity(session_ctx_t *ctx,
                                              unsigned ticks)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_TETRIS ||
        !ctx->game.active) {
        return false;
    }

    tetris_game_state_t *state = session_game_ensure_tetris(ctx);
    if (state == nullptr) {
        return false;
    }
    if (state->game_over || ticks == 0U) {
        return false;
    }

    if (state->gravity_threshold == 0U) {
        state->gravity_threshold = SSH_CHATTER_TETRIS_GRAVITY_THRESHOLD;
    }

    bool moved = false;
    state->gravity_counter += ticks;
    while (state->gravity_counter >= state->gravity_threshold) {
        if (!session_game_tetris_soft_drop(ctx)) {
            state->gravity_counter = 0U;
            break;
        }
        moved = true;
        state->gravity_counter -= state->gravity_threshold;
        if (state->game_over) {
            break;
        }
    }
    return moved;
}

typedef enum {
    TETRIS_INPUT_NONE = 0,
    TETRIS_INPUT_MOVE_LEFT,
    TETRIS_INPUT_MOVE_RIGHT,
    TETRIS_INPUT_ROTATE,
    TETRIS_INPUT_SOFT_DROP,
    TETRIS_INPUT_HARD_DROP,
} tetris_input_action_t;

static bool session_game_tetris_update_timer(session_ctx_t *ctx,
                                             bool accelerate)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_TETRIS ||
        !ctx->game.active) {
        return false;
    }

    tetris_game_state_t *state = session_game_ensure_tetris(ctx);
    if (state == nullptr) {
        return false;
    }
    if (state->game_over) {
        return false;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        now.tv_sec = time(nullptr);
        now.tv_nsec = 0L;
    }

    if (!state->gravity_timer_initialized) {
        state->gravity_timer_last = now;
        state->gravity_timer_initialized = true;
    } else {
        struct timespec last = state->gravity_timer_last;
        state->gravity_timer_last = now;

        time_t sec_delta = now.tv_sec - last.tv_sec;
        long nsec_delta = now.tv_nsec - last.tv_nsec;
        if (nsec_delta < 0L) {
            --sec_delta;
            nsec_delta += 1000000000L;
        }

        if (sec_delta > 0 || nsec_delta > 0L) {
            uint64_t elapsed_ns =
                (uint64_t)sec_delta * 1000000000ULL + (uint64_t)nsec_delta;
            state->gravity_timer_accumulator_ns += elapsed_ns;
        }
    }

    unsigned ticks = 0U;
    while (state->gravity_timer_accumulator_ns >=
           SSH_CHATTER_TETRIS_GRAVITY_INTERVAL_NS) {
        state->gravity_timer_accumulator_ns -=
            SSH_CHATTER_TETRIS_GRAVITY_INTERVAL_NS;
        ticks += state->gravity_rate;
    }

    if (accelerate) {
        ticks += state->gravity_rate + state->gravity_threshold;
    }

    if (ticks == 0U) {
        return false;
    }

    return session_game_tetris_apply_gravity(ctx, ticks);
}

static bool session_game_tetris_process_timeout(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_TETRIS ||
        !ctx->game.active) {
        return false;
    }

    if (ctx->game.is_camouflaged) {
        tetris_game_state_t *tetris = session_game_ensure_tetris(ctx);
        if (tetris != nullptr) {
            tetris->gravity_timer_initialized = false;
            tetris->gravity_timer_accumulator_ns = 0U;
        }
        return false;
    }

    bool redraw = session_game_tetris_update_timer(ctx, false);
    tetris_game_state_t *state = session_game_ensure_tetris(ctx);
    if (state != nullptr && state->game_over) {
        session_game_suspend(ctx, "Game over!");
        return true;
    }

    if (redraw) {
        session_game_tetris_render(ctx);
    }
    return redraw;
}

static bool session_game_tetris_process_action(session_ctx_t *ctx,
                                               int action_value)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_TETRIS ||
        !ctx->game.active) {
        return false;
    }

    if (ctx->game.is_camouflaged) {
        return true;
    }

    tetris_input_action_t action = (tetris_input_action_t)action_value;
    if (action == TETRIS_INPUT_NONE) {
        return false;
    }

    tetris_game_state_t *state = ctx->game.tetris;
    if (state->game_over) {
        session_game_suspend(ctx, "Game over!");
        return true;
    }

    bool redraw = session_game_tetris_update_timer(ctx, false);
    if (state->game_over) {
        session_game_suspend(ctx, "Game over!");
        return true;
    }

    bool accelerate = false;
    bool manual_drop = false;

    switch (action) {
    case TETRIS_INPUT_MOVE_LEFT:
        if (session_game_tetris_move(ctx, 0, -1)) {
            redraw = true;
        }
        break;
    case TETRIS_INPUT_MOVE_RIGHT:
        if (session_game_tetris_move(ctx, 0, 1)) {
            redraw = true;
        }
        break;
    case TETRIS_INPUT_ROTATE:
        if (session_game_tetris_rotate(ctx)) {
            redraw = true;
        }
        break;
    case TETRIS_INPUT_SOFT_DROP:
        accelerate = true;
        break;
    case TETRIS_INPUT_HARD_DROP:
        while (session_game_tetris_soft_drop(ctx)) {
            redraw = true;
        }
        manual_drop = true;
        break;
    case TETRIS_INPUT_NONE:
    default:
        break;
    }

    if (state->game_over) {
        session_game_suspend(ctx, "Game over!");
        return true;
    }

    if (accelerate) {
        if (session_game_tetris_update_timer(ctx, true)) {
            redraw = true;
        }
    } else if (!manual_drop) {
        if (session_game_tetris_update_timer(ctx, false)) {
            redraw = true;
        }
    }

    if (state->game_over) {
        session_game_suspend(ctx, "Game over!");
        return true;
    }

    if (redraw) {
        session_game_tetris_render(ctx);
    }

    return true;
}

static bool session_game_tetris_process_raw_input(session_ctx_t *ctx, char ch)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_TETRIS ||
        !ctx->game.active) {
        return false;
    }

    tetris_game_state_t *state = ctx->game.tetris;
    const bool camouflaged = ctx->game.is_camouflaged;

    if (ch == 0x01 || ch == 0x03 || ch == 0x1a || ch == 0x13) {
        return false;
    }

    if (state->input_escape_active) {
        if (state->input_escape_length < sizeof(state->input_escape_buffer)) {
            state->input_escape_buffer[state->input_escape_length++] = ch;
        }

        if (state->input_escape_length == 2U &&
            state->input_escape_buffer[1] == '[') {
            return true;
        }

        if (state->input_escape_length >= 3U &&
            state->input_escape_buffer[1] == '[') {
            char final =
                state->input_escape_buffer[state->input_escape_length - 1U];
            tetris_input_action_t action = TETRIS_INPUT_NONE;
            if (final == 'A') {
                action = TETRIS_INPUT_ROTATE;
            } else if (final == 'B') {
                action = TETRIS_INPUT_SOFT_DROP;
            } else if (final == 'C') {
                action = TETRIS_INPUT_MOVE_RIGHT;
            } else if (final == 'D') {
                action = TETRIS_INPUT_MOVE_LEFT;
            }
            state->input_escape_active = false;
            state->input_escape_length = 0U;
            if (action != TETRIS_INPUT_NONE && !camouflaged) {
                session_game_tetris_process_action(ctx, action);
            }
            return true;
        }

        state->input_escape_active = false;
        state->input_escape_length = 0U;
        return true;
    }

    if (ch == 0x1b) {
        state->input_escape_active = true;
        state->input_escape_length = 0U;
        if (state->input_escape_length < sizeof(state->input_escape_buffer)) {
            state->input_escape_buffer[state->input_escape_length++] = ch;
        } else {
            state->input_escape_active = false;
            state->input_escape_length = 0U;
        }
        return true;
    }

    if (ch == '\r' || ch == '\n') {
        return true;
    }

    if (ch == 0x12) {
        if (camouflaged) {
            return true;
        }
        session_game_tetris_process_action(ctx, TETRIS_INPUT_ROTATE);
        return true;
    }

    unsigned char lowered = (unsigned char)ch;
    if (lowered >= 'A' && lowered <= 'Z') {
        lowered = (unsigned char)tolower(lowered);
    }

    if (lowered == 't') {
        if (ctx->game.is_camouflaged) {
            ctx->game.is_camouflaged = false;
            if (ctx->game.tetris != nullptr &&
                ctx->game.saved_tetris_state != nullptr) {
                *ctx->game.tetris = *ctx->game.saved_tetris_state;
            }
            ctx->game.tetris->gravity_timer_initialized = false;
            ctx->game.tetris->gravity_timer_accumulator_ns = 0U;
            // Re-enable alternate screen buffer when returning to game
            session_enable_alternate_screen(ctx);
            session_game_tetris_render(ctx);
        } else {
            ctx->game.is_camouflaged = true;
            if (ctx->game.tetris != nullptr &&
                ctx->game.saved_tetris_state != nullptr) {
                *ctx->game.saved_tetris_state = *ctx->game.tetris;
                ctx->game.saved_tetris_state->gravity_timer_initialized =
                    false;
                ctx->game.saved_tetris_state->gravity_timer_accumulator_ns =
                    0U;
            }
            ctx->game.tetris->gravity_timer_initialized = false;
            ctx->game.tetris->gravity_timer_accumulator_ns = 0U;
            // Disable alternate screen buffer when showing camouflage
            session_disable_alternate_screen(ctx);
            session_game_show_camouflage(ctx);
        }
        return true;
    }

    if (camouflaged) {
        return true;
    }

    switch (lowered) {
    case 'a':
        session_game_tetris_process_action(ctx, TETRIS_INPUT_MOVE_LEFT);
        return true;
    case 'd':
        session_game_tetris_process_action(ctx, TETRIS_INPUT_MOVE_RIGHT);
        return true;
    case 'w':
        session_game_tetris_process_action(ctx, TETRIS_INPUT_ROTATE);
        return true;
    case 's':
        session_game_tetris_process_action(ctx, TETRIS_INPUT_SOFT_DROP);
        return true;
    case ' ':
        session_game_tetris_process_action(ctx, TETRIS_INPUT_HARD_DROP);
        return true;
    default:
        break;
    }

    if ((unsigned char)ch < 0x20U) {
        return false;
    }

    return true;
}

static bool session_game_tetris_rotate(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_TETRIS) {
        return false;
    }
    tetris_game_state_t *state = ctx->game.tetris;
    if (state->current_piece < 0) {
        return false;
    }
    int new_rotation = (state->rotation + 1) & 3;
    if (!session_game_tetris_position_valid(state, state->current_piece,
                                            new_rotation, state->row,
                                            state->column)) {
        return false;
    }
    state->rotation = new_rotation;
    return true;
}

static void session_game_tetris_clear_lines(session_ctx_t *ctx,
                                            unsigned *cleared)
{
    if (ctx == nullptr) {
        if (cleared != nullptr) {
            *cleared = 0U;
        }
        return;
    }

    tetris_game_state_t *state = ctx->game.tetris;
    unsigned removed = 0U;
    for (int row = 0; row < SSH_CHATTER_TETRIS_HEIGHT; ++row) {
        bool full = true;
        for (int col = 0; col < SSH_CHATTER_TETRIS_WIDTH; ++col) {
            if (state->board[row][col] == 0) {
                full = false;
                break;
            }
        }
        if (!full) {
            continue;
        }
        ++removed;
        for (int move_row = row; move_row > 0; --move_row) {
            for (int move_col = 0; move_col < SSH_CHATTER_TETRIS_WIDTH;
                 ++move_col) {
                state->board[move_row][move_col] =
                    state->board[move_row - 1][move_col];
            }
        }
        for (int move_col = 0; move_col < SSH_CHATTER_TETRIS_WIDTH;
             ++move_col) {
            state->board[0][move_col] = 0;
        }
    }
    if (cleared != nullptr) {
        *cleared = removed;
    }
}

static void session_game_tetris_handle_round_progress(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_TETRIS) {
        return;
    }

    tetris_game_state_t *state = ctx->game.tetris;
    while (state->round < SSH_CHATTER_TETRIS_MAX_ROUNDS &&
           state->lines_cleared >= state->next_round_line_goal) {
        state->round += 1U;
        state->next_round_line_goal += SSH_CHATTER_TETRIS_LINES_PER_ROUND;
        session_game_tetris_apply_round_settings(state);

        char announcement[SSH_CHATTER_MESSAGE_LIMIT];
        if (state->round >= SSH_CHATTER_TETRIS_MAX_ROUNDS) {
            snprintf(announcement, sizeof(announcement),
                     "Round %u reached! Gravity is at maximum speed.",
                     state->round);
        } else {
            snprintf(announcement, sizeof(announcement),
                     "Round %u reached! Blocks will fall faster.",
                     state->round);
        }
        bool previous_translation_suppress = ctx->translation_suppress_output;
        ctx->translation_suppress_output = true;
        session_send_system_line(ctx, announcement);
        ctx->translation_suppress_output = previous_translation_suppress;
    }
}

static void session_game_tetris_lock_piece(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_TETRIS) {
        return;
    }

    tetris_game_state_t *state = ctx->game.tetris;
    if (state->current_piece < 0) {
        return;
    }

    for (int r = 0; r < SSH_CHATTER_TETROMINO_SIZE; ++r) {
        for (int c = 0; c < SSH_CHATTER_TETROMINO_SIZE; ++c) {
            if (!session_game_tetris_cell_occupied(state->current_piece,
                                                   state->rotation, r, c)) {
                continue;
            }
            int board_row = state->row + r;
            int board_col = state->column + c;
            if (board_row < 0 || board_row >= SSH_CHATTER_TETRIS_HEIGHT ||
                board_col < 0 || board_col >= SSH_CHATTER_TETRIS_WIDTH) {
                continue;
            }
            state->board[board_row][board_col] = state->current_piece + 1;
        }
    }

    unsigned cleared = 0U;
    session_game_tetris_clear_lines(ctx, &cleared);
    if (cleared > 0U) {
        state->lines_cleared += cleared;
        state->score += cleared * 10U;
        session_game_tetris_handle_round_progress(ctx);
    }

    if (!session_game_tetris_spawn_piece(ctx)) {
        state->game_over = true;
    }
}

static const char *session_game_camouflage_lexer(const char *language)
{
    if (language == nullptr) {
        return nullptr;
    }
    if (strcmp(language, "c") == 0) {
        return "c";
    }
    if (strcmp(language, "cpp") == 0) {
        return "cpp";
    }
    if (strcmp(language, "java") == 0) {
        return "java";
    }
    if (strcmp(language, "go") == 0) {
        return "go";
    }
    if (strcmp(language, "js") == 0) {
        return "javascript";
    }
    if (strcmp(language, "ts") == 0) {
        return "typescript";
    }
    if (strcmp(language, "rust") == 0) {
        return "rust";
    }
    return nullptr;
}

static bool session_game_camouflage_highlight(session_ctx_t *ctx,
                                              const char *lexer,
                                              const char *file_path)
{
    if (ctx == nullptr || lexer == nullptr || file_path == nullptr) {
        return false;
    }

    char command[512];
    int written =
        snprintf(command, sizeof(command),
                 "pygmentize -f terminal256 -O stripnl=False -l %s %s", lexer,
                 file_path);
    if (written <= 0 || (size_t)written >= sizeof(command)) {
        return false;
    }

    errno = 0;
    FILE *pipe = popen(command, "r");
    if (pipe == nullptr) {
        if (errno == ENOENT) {
            session_send_system_line(ctx, "Install python3-pygments to enable "
                                          "camouflage syntax highlighting.");
        }
        return false;
    }

    char line_buffer[SSH_CHATTER_MESSAGE_LIMIT];
    bool produced_output = false;
    while (fgets(line_buffer, sizeof(line_buffer), pipe) != nullptr) {
        size_t len = strlen(line_buffer);
        if (len > 0U && line_buffer[len - 1U] == '\n') {
            line_buffer[len - 1U] = '\0';
        }
        session_send_system_line(ctx, line_buffer);
        produced_output = true;
    }

    int status = pclose(pipe);
    if (status != 0 && !produced_output) {
        return false;
    }

    return produced_output;
}

static void session_game_show_camouflage(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    session_clear_screen(ctx);
    session_apply_background_fill(ctx);

    char file_path_buffer[256];
    snprintf(file_path_buffer, sizeof(file_path_buffer),
             "/var/lib/ssh-chatter/%s.txt",
             ctx->game.chosen_camouflage_language);

    FILE *probe = fopen(file_path_buffer, "r");
    if (probe == nullptr) {
        session_send_system_line(
            ctx, "Error: Could not load camouflage code snippet.");
        return;
    }
    fclose(probe);

    bool highlighted = false;
    const char *lexer =
        session_game_camouflage_lexer(ctx->game.chosen_camouflage_language);
    if (lexer != nullptr) {
        highlighted =
            session_game_camouflage_highlight(ctx, lexer, file_path_buffer);
    }

    if (!highlighted) {
        FILE *fp = fopen(file_path_buffer, "r");
        if (fp == nullptr) {
            session_send_system_line(
                ctx, "Error: Could not load camouflage code snippet.");
            session_render_prompt(ctx, false);
            return;
        }

        char line_buffer[SSH_CHATTER_MESSAGE_LIMIT];
        while (fgets(line_buffer, sizeof(line_buffer), fp) != nullptr) {
            size_t len = strlen(line_buffer);
            if (len > 0U && line_buffer[len - 1U] == '\n') {
                line_buffer[len - 1U] = '\0';
            }
            session_send_system_line(ctx, line_buffer);
        }

        fclose(fp);
    }

    session_render_prompt(ctx, false);
}

static const char *TETROMINO_COLOR_CODES[7] = {
    ANSI_BRIGHT_CYAN,  ANSI_BLUE,          ANSI_BRIGHT_YELLOW,
    ANSI_YELLOW,       ANSI_BRIGHT_GREEN,  ANSI_BRIGHT_MAGENTA,
    ANSI_BRIGHT_RED};

static void session_game_tetris_render(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_TETRIS) {
        return;
    }

    bool previous_translation_suppress = ctx->translation_suppress_output;
    ctx->translation_suppress_output = true;
    bool previous_dedup_state = ctx->disable_output_dedup;
    ctx->disable_output_dedup = true;

    tetris_game_state_t *state = ctx->game.tetris;
    if (state == nullptr) {
        return;
    }

    char *buffer = ctx->tetris_screen_buffer;
    size_t offset = 0;

    // Clear screen and move cursor to home position
    offset += (size_t)snprintf(buffer + offset,
                               SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE - offset,
                               "\033[H\033[2J");

    offset += (size_t)snprintf(
        buffer + offset, SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE - offset, "\n");
    offset += (size_t)snprintf(buffer + offset,
                               SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE - offset,
                               "%s--- Tetris ---%s\n", ANSI_BRIGHT_CYAN,
                               ANSI_RESET);

    char header[SSH_CHATTER_MESSAGE_LIMIT];
    char next_char = TETROMINO_DISPLAY_CHARS[state->next_piece % 7];
    if (state->round < SSH_CHATTER_TETRIS_MAX_ROUNDS) {
        unsigned lines_remaining = 0U;
        if (state->next_round_line_goal > state->lines_cleared) {
            lines_remaining =
                state->next_round_line_goal - state->lines_cleared;
        }
        snprintf(header, sizeof(header),
                 "%sScore: %u%s   %sLines: %u%s   %sRound: %u/%u (next in %u)%s   "
                 "%sNext: %c%s",
                 ANSI_BRIGHT_YELLOW, state->score, ANSI_RESET,
                 ANSI_BRIGHT_GREEN, state->lines_cleared, ANSI_RESET,
                 ANSI_BRIGHT_CYAN, state->round, SSH_CHATTER_TETRIS_MAX_ROUNDS,
                 lines_remaining, ANSI_RESET, ANSI_BRIGHT_MAGENTA, next_char,
                 ANSI_RESET);
    } else {
        snprintf(header, sizeof(header),
                 "%sScore: %u%s   %sLines: %u%s   %sRound: %u/%u (max speed)%s   "
                 "%sNext: %c%s",
                 ANSI_BRIGHT_YELLOW, state->score, ANSI_RESET,
                 ANSI_BRIGHT_GREEN, state->lines_cleared, ANSI_RESET,
                 ANSI_BRIGHT_CYAN, state->round, SSH_CHATTER_TETRIS_MAX_ROUNDS,
                 ANSI_RESET, ANSI_BRIGHT_MAGENTA, next_char, ANSI_RESET);
    }

    char border[SSH_CHATTER_TETRIS_WIDTH + 3];
    border[0] = '+';
    for (int col = 0; col < SSH_CHATTER_TETRIS_WIDTH; ++col) {
        border[col + 1] = '-';
    }
    border[SSH_CHATTER_TETRIS_WIDTH + 1] = '+';
    border[SSH_CHATTER_TETRIS_WIDTH + 2] = '\0';
    offset += (size_t)snprintf(buffer + offset,
                               SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE - offset,
                               "%s%s%s\n", ANSI_BRIGHT_BLACK, border,
                               ANSI_RESET);

    for (int row = 0; row < SSH_CHATTER_TETRIS_HEIGHT; ++row) {
        char line_buffer[SSH_CHATTER_TETRIS_WIDTH * 8 + 32];
        size_t line_offset = 0U;
        line_offset += (size_t)snprintf(line_buffer + line_offset,
                                        sizeof(line_buffer) - line_offset,
                                        "%s|%s", ANSI_BRIGHT_BLACK,
                                        ANSI_RESET);
        for (int col = 0; col < SSH_CHATTER_TETRIS_WIDTH; ++col) {
            char cell = ' ';
            const char *color = "";
            if (state->board[row][col] != 0) {
                int index = state->board[row][col] - 1;
                if (index < 0 || index >= 7) {
                    index = 0;
                }
                cell = TETROMINO_DISPLAY_CHARS[index];
                color = TETROMINO_COLOR_CODES[index];
            } else if (!state->game_over && state->current_piece >= 0) {
                int local_row = row - state->row;
                int local_col = col - state->column;
                if (local_row >= 0 && local_row < SSH_CHATTER_TETROMINO_SIZE &&
                    local_col >= 0 && local_col < SSH_CHATTER_TETROMINO_SIZE &&
                    session_game_tetris_cell_occupied(state->current_piece,
                                                      state->rotation,
                                                      local_row, local_col)) {
                    cell = TETROMINO_DISPLAY_CHARS[state->current_piece];
                    color =
                        TETROMINO_COLOR_CODES[state->current_piece % 7];
                }
            }
            if (color[0] != '\0') {
                line_offset += (size_t)snprintf(
                    line_buffer + line_offset, sizeof(line_buffer) - line_offset,
                    "%s%c%s", color, cell, ANSI_RESET);
            } else {
                line_buffer[line_offset++] = cell;
                line_buffer[line_offset] = '\0';
            }
        }
        line_offset += (size_t)snprintf(line_buffer + line_offset,
                                        sizeof(line_buffer) - line_offset,
                                        "%s|%s", ANSI_BRIGHT_BLACK,
                                        ANSI_RESET);
        offset += (size_t)snprintf(
            buffer + offset, SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE - offset,
            "%s\n", line_buffer);
    }

    offset += (size_t)snprintf(buffer + offset,
                               SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE - offset,
                               "%s%s%s\n", ANSI_BRIGHT_BLACK, border,
                               ANSI_RESET);
    offset += (size_t)snprintf(buffer + offset,
                               SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE - offset,
                               "%s\n", header);
    offset += (size_t)snprintf(buffer + offset,
                               SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE - offset,
                               "%sControls:%s left, right, down, Ctrl+R or up: "
                               "rotate, drop. Blank line = down.\n",
                               ANSI_BRIGHT_CYAN, ANSI_RESET);

    // Only send if the buffer has changed
    if (strcmp(ctx->tetris_screen_buffer, ctx->tetris_prev_screen_buffer) !=
        0) {
        // Enable output buffering to send entire screen in one flush
        session_output_buffer_start(ctx);
        session_send_raw_text(ctx, ctx->tetris_screen_buffer);
        session_output_buffer_stop(ctx);

        strncpy(ctx->tetris_prev_screen_buffer, ctx->tetris_screen_buffer,
                SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE);
        ctx->tetris_prev_screen_buffer[SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE -
                                       1] = '\0';
    }

    ctx->translation_suppress_output = previous_translation_suppress;
    ctx->disable_output_dedup = previous_dedup_state;
}

static void session_game_tetris_handle_line(session_ctx_t *ctx,
                                            const char *line)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_TETRIS ||
        !ctx->game.active) {
        return;
    }

    tetris_game_state_t *state = ctx->game.tetris;
    if (state->game_over) {
        session_game_suspend(ctx, "Game over!");
        return;
    }

    bool previous_translation_suppress = ctx->translation_suppress_output;
    ctx->translation_suppress_output = true;

    char command[32];
    if (line == nullptr) {
        command[0] = '\0';
    } else {
        size_t copy_len = strnlen(line, sizeof(command) - 1U);
        memcpy(command, line, copy_len);
        command[copy_len] = '\0';
    }
    trim_whitespace_inplace(command);
    for (size_t idx = 0U; command[idx] != '\0'; ++idx) {
        command[idx] = (char)tolower((unsigned char)command[idx]);
    }

    if (command[0] == '\0') {
        session_game_tetris_process_timeout(ctx);
        goto cleanup;
    }

    if (strcmp(command, "help") == 0) {
        session_send_system_line(
            ctx, "Tetris controls: WASD or arrow keys move (W/Up "
                 "rotate, S/Down soft drop, A/Left, D/Right),"
                 " space for a hard drop, and Ctrl+R also "
                 "rotates. Ctrl+Z or /suspend! exits.");
        goto cleanup;
    }

    if (strcmp(command, "drop") == 0) {
        session_game_tetris_process_action(ctx, TETRIS_INPUT_HARD_DROP);
        goto cleanup;
    }

    session_send_system_line(
        ctx,
        "Use WASD or the arrow keys for control. Type help for a summary.");

cleanup:
    ctx->translation_suppress_output = previous_translation_suppress;
}

static void session_game_start_tetris(session_ctx_t *ctx)
{
    session_send_system_line(
        ctx,
        "CHOOSE YOUR LOCKSCREEN LANGUAGE TO HIDE THE SCREEN ON YOUR OFFICE! "
        "(c, cpp, java, go, js, ts, rust)");
    char language_choice[16];
    size_t length = 0U;
    while (length + 1U < sizeof(language_choice)) {
        char ch = '\0';
        const int read_result = session_transport_read(ctx, &ch, 1, -1);
        if (read_result <= 0) {
            ctx->game.active = false;
            ctx->game.type = SESSION_GAME_NONE;
            return;
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

        language_choice[length++] = ch;
        session_local_echo_char(ctx, ch);
    }
    language_choice[length] = '\0';
    trim_whitespace_inplace(language_choice);
    for (size_t idx = 0U; language_choice[idx] != '\0'; ++idx) {
        language_choice[idx] =
            (char)tolower((unsigned char)language_choice[idx]);
    }

    if (language_choice[0] == '\0') {
        session_send_system_line(ctx, "No language chosen. Defaulting to C.");
        snprintf(ctx->game.chosen_camouflage_language,
                 sizeof(ctx->game.chosen_camouflage_language), "c");
    } else if (strcmp(language_choice, "c") == 0 ||
               strcmp(language_choice, "cpp") == 0 ||
               strcmp(language_choice, "java") == 0 ||
               strcmp(language_choice, "go") == 0 ||
               strcmp(language_choice, "js") == 0 ||
               strcmp(language_choice, "ts") == 0 ||
               strcmp(language_choice, "rust") == 0) {
        snprintf(ctx->game.chosen_camouflage_language,
                 sizeof(ctx->game.chosen_camouflage_language), "%s",
                 language_choice);
    } else {
        session_send_system_line(ctx, "Invalid language. Defaulting to C.");
        snprintf(ctx->game.chosen_camouflage_language,
                 sizeof(ctx->game.chosen_camouflage_language), "c");
    }

    session_game_tetris_reset(ctx->game.tetris);
    session_game_seed_rng(ctx);
    session_game_tetris_fill_bag(ctx);
    ctx->game.tetris->next_piece = session_game_tetris_take_piece(ctx);
    ctx->game.type = SESSION_GAME_TETRIS;
    ctx->game.active = true;
    ctx->game.tetris->game_over = false;
    bool previous_translation_suppress = ctx->translation_suppress_output;
    if (!session_game_tetris_spawn_piece(ctx)) {
        session_send_system_line(ctx, "Unable to start Tetris right now.");
        ctx->game.active = false;
        ctx->game.type = SESSION_GAME_NONE;
        return;
    }

    memset(ctx->tetris_screen_buffer, 0, SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE);
    memset(ctx->tetris_prev_screen_buffer, 0,
           SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE);

    session_send_system_line(ctx,
                             "Tetris started. Pieces fall on their own - use "
                             "WASD or arrow keys to move, Ctrl+R or Up to "
                             "rotate, Down to soft drop, Space to hard "
                             "drop. Blank line = soft drop.");
    char round_message[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(round_message, sizeof(round_message),
             "Round 1/%u: Clear %u lines to reach the next round.",
             SSH_CHATTER_TETRIS_MAX_ROUNDS, SSH_CHATTER_TETRIS_LINES_PER_ROUND);
    session_send_system_line(ctx, round_message);
    session_game_tetris_render(ctx);

    ctx->translation_suppress_output = previous_translation_suppress;
}

static void session_game_start_liargame(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    // Ask for camouflage language first
    session_send_system_line(
        ctx,
        "CHOOSE YOUR LOCKSCREEN LANGUAGE TO HIDE THE SCREEN ON YOUR OFFICE! "
        "(c, cpp, java, go, js, ts, rust)");
    char language_choice[16];
    size_t length = 0U;
    while (length + 1U < sizeof(language_choice)) {
        char ch = '\0';
        const int read_result = session_transport_read(ctx, &ch, 1, -1);
        if (read_result <= 0) {
            return;
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

        language_choice[length++] = ch;
        session_local_echo_char(ctx, ch);
    }
    language_choice[length] = '\0';
    trim_whitespace_inplace(language_choice);
    for (size_t idx = 0U; language_choice[idx] != '\0'; ++idx) {
        language_choice[idx] =
            (char)tolower((unsigned char)language_choice[idx]);
    }

    if (language_choice[0] == '\0') {
        session_send_system_line(ctx, "No language chosen. Defaulting to C.");
        snprintf(ctx->game.chosen_camouflage_language,
                 sizeof(ctx->game.chosen_camouflage_language), "c");
    } else if (strcmp(language_choice, "c") == 0 ||
               strcmp(language_choice, "cpp") == 0 ||
               strcmp(language_choice, "java") == 0 ||
               strcmp(language_choice, "go") == 0 ||
               strcmp(language_choice, "js") == 0 ||
               strcmp(language_choice, "ts") == 0 ||
               strcmp(language_choice, "rust") == 0) {
        snprintf(ctx->game.chosen_camouflage_language,
                 sizeof(ctx->game.chosen_camouflage_language), "%s",
                 language_choice);
    } else {
        session_send_system_line(ctx, "Invalid language. Defaulting to C.");
        snprintf(ctx->game.chosen_camouflage_language,
                 sizeof(ctx->game.chosen_camouflage_language), "c");
    }

    ctx->game.type = SESSION_GAME_LIARGAME;
    ctx->game.active = true;
    ctx->game.is_camouflaged = false;
    ctx->game.liar.round_number = 0U;
    ctx->game.liar.score = 0U;
    ctx->game.liar.awaiting_guess = false;
    session_send_system_line(ctx, "");
    session_send_system_line(ctx, "Liar Game started. Guess which statement is "
                                  "the lie by typing 1, 2, or 3.");
    session_game_liar_present_round(ctx);
}

static void session_game_liar_present_round(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_LIARGAME ||
        !ctx->game.active) {
        return;
    }

    size_t prompt_count = sizeof(LIAR_PROMPTS) / sizeof(LIAR_PROMPTS[0]);
    if (prompt_count == 0U) {
        session_game_suspend(ctx, "No prompts available for the liar game.");
        return;
    }

    unsigned index =
        (unsigned)session_game_random_range(ctx, (int)prompt_count);
    ctx->game.liar.current_prompt_index = index;
    ctx->game.liar.liar_index = LIAR_PROMPTS[index].liar_index % 3U;
    ctx->game.liar.round_number += 1U;
    ctx->game.liar.awaiting_guess = true;

    session_render_separator(ctx, "Liar Game");
    char header[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(header, sizeof(header), "Round %u - which statement is the lie?",
             ctx->game.liar.round_number);
    session_send_system_line(ctx, header);

    const liar_prompt_t *prompt = &LIAR_PROMPTS[index];
    for (int i = 0; i < 3; ++i) {
        char line_buffer[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(line_buffer, sizeof(line_buffer), "%d. %s", i + 1,
                 prompt->statements[i]);
        session_send_system_line(ctx, line_buffer);
    }
    session_send_system_line(
        ctx, "Enter 1, 2, or 3 to choose. Type 'help' for options.");
}

static void session_game_liar_handle_line(session_ctx_t *ctx, const char *line)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_LIARGAME ||
        !ctx->game.active) {
        return;
    }

    liar_game_state_t *state = &ctx->game.liar;
    char command[32];
    if (line == nullptr) {
        command[0] = '\0';
    } else {
        size_t copy_len = strnlen(line, sizeof(command) - 1U);
        memcpy(command, line, copy_len);
        command[copy_len] = '\0';
    }
    trim_whitespace_inplace(command);
    for (size_t idx = 0U; command[idx] != '\0'; ++idx) {
        command[idx] = (char)tolower((unsigned char)command[idx]);
    }

    // Handle camouflage toggle with 't' command
    if (strcmp(command, "t") == 0) {
        if (ctx->game.is_camouflaged) {
            ctx->game.is_camouflaged = false;
            ctx->game.saved_liar_state = ctx->game.liar;
            session_game_liar_present_round(ctx);
        } else {
            ctx->game.is_camouflaged = true;
            ctx->game.saved_liar_state = ctx->game.liar;
            session_game_show_camouflage(ctx);
        }
        return;
    }

    if (strcmp(command, "help") == 0) {
        session_send_system_line(
            ctx, "Type 1, 2, or 3 to guess the lie. /suspend! exits the game.");
        return;
    }

    if (command[0] == '\0') {
        session_send_system_line(ctx,
                                 "Pick a statement number between 1 and 3.");
        return;
    }

    if (!state->awaiting_guess) {
        session_game_liar_present_round(ctx);
        return;
    }

    char *endptr = nullptr;
    long value = strtol(command, &endptr, 10);
    if (endptr == command || value < 1L || value > 3L) {
        session_send_system_line(ctx,
                                 "Please enter 1, 2, or 3 to choose the lie.");
        return;
    }

    unsigned guess = (unsigned)(value - 1L);
    const liar_prompt_t *prompt = &LIAR_PROMPTS[state->current_prompt_index];
    if (guess == state->liar_index) {
        ++state->score;
        session_send_system_line(ctx, "Correct! That statement was the lie.");
    } else {
        char reveal[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(reveal, sizeof(reveal), "Nope! The lie was #%u: %s",
                 state->liar_index + 1U, prompt->statements[state->liar_index]);
        session_send_system_line(ctx, reveal);
    }

    state->awaiting_guess = false;
    session_game_liar_present_round(ctx);
}

static const int kOthelloDirections[8][2] = {
    {-1, -1}, {-1, 0}, {-1, 1}, {0, -1}, {0, 1}, {1, -1}, {1, 0}, {1, 1},
};

typedef struct othello_move {
    int row;
    int col;
    int flipped;
} othello_move_t;

static bool session_game_othello_in_bounds(int row, int col)
{
    return row >= 0 && row < SSH_CHATTER_OTHELLO_BOARD_SIZE && col >= 0 &&
           col < SSH_CHATTER_OTHELLO_BOARD_SIZE;
}

static int
session_game_othello_direction_count(const othello_game_state_t *state, int row,
                                     int col, int drow, int dcol,
                                     othello_cell_type_t player)
{
    if (state == nullptr) {
        return 0;
    }

    othello_cell_type_t opponent =
        (player == OTHELLO_CELL_RED) ? OTHELLO_CELL_GREEN : OTHELLO_CELL_RED;
    int count = 0;
    int r = row + drow;
    int c = col + dcol;

    while (session_game_othello_in_bounds(r, c)) {
        uint8_t cell = state->board[r][c];
        if (cell == (uint8_t)opponent) {
            ++count;
            r += drow;
            c += dcol;
            continue;
        }

        if (cell == (uint8_t)player && count > 0) {
            return count;
        }

        break;
    }

    return 0;
}

static int session_game_othello_count_flips(const othello_game_state_t *state,
                                            int row, int col,
                                            othello_cell_type_t player)
{
    if (state == nullptr || !session_game_othello_in_bounds(row, col)) {
        return 0;
    }

    if (state->board[row][col] != OTHELLO_CELL_EMPTY) {
        return 0;
    }

    int total = 0;
    for (size_t idx = 0U;
         idx < sizeof(kOthelloDirections) / sizeof(kOthelloDirections[0]);
         ++idx) {
        total += session_game_othello_direction_count(
            state, row, col, kOthelloDirections[idx][0],
            kOthelloDirections[idx][1], player);
    }

    return total;
}

static void session_game_othello_count_scores(const othello_game_state_t *state,
                                              unsigned *red, unsigned *green)
{
    if (red != nullptr) {
        *red = 0U;
    }
    if (green != nullptr) {
        *green = 0U;
    }

    if (state == nullptr) {
        return;
    }

    unsigned red_count = 0U;
    unsigned green_count = 0U;
    for (int row = 0; row < SSH_CHATTER_OTHELLO_BOARD_SIZE; ++row) {
        for (int col = 0; col < SSH_CHATTER_OTHELLO_BOARD_SIZE; ++col) {
            if (state->board[row][col] == OTHELLO_CELL_RED) {
                ++red_count;
            } else if (state->board[row][col] == OTHELLO_CELL_GREEN) {
                ++green_count;
            }
        }
    }

    if (red != nullptr) {
        *red = red_count;
    }
    if (green != nullptr) {
        *green = green_count;
    }
}

static void session_game_othello_reset_state(othello_game_state_t *state)
{
    if (state == nullptr) {
        return;
    }

    memset(state, 0, sizeof(*state));
    for (int row = 0; row < SSH_CHATTER_OTHELLO_BOARD_SIZE; ++row) {
        for (int col = 0; col < SSH_CHATTER_OTHELLO_BOARD_SIZE; ++col) {
            state->board[row][col] = OTHELLO_CELL_EMPTY;
        }
    }

    int mid = SSH_CHATTER_OTHELLO_BOARD_SIZE / 2;
    state->board[mid - 1][mid - 1] = OTHELLO_CELL_GREEN;
    state->board[mid][mid] = OTHELLO_CELL_GREEN;
    state->board[mid - 1][mid] = OTHELLO_CELL_RED;
    state->board[mid][mid - 1] = OTHELLO_CELL_RED;
    state->player_turn = true;
    state->game_over = false;
    state->consecutive_passes = 0U;
    state->last_player_row = -1;
    state->last_player_col = -1;
    state->last_ai_row = -1;
    state->last_ai_col = -1;
    state->awaiting_mode_selection = false;
    state->awaiting_difficulty_selection = false;
    state->difficulty_level = 5U; // Default to hardest
    state->multiplayer = false;
    state->awaiting_opponent = false;
    state->slot_index = -1;
    state->player_number = 0U;
    session_game_othello_count_scores(state, &state->red_score,
                                      &state->green_score);
}

static void session_game_othello_copy_core(othello_game_state_t *dest,
                                           const othello_game_state_t *src)
{
    if (dest == nullptr || src == nullptr) {
        return;
    }

    memcpy(dest->board, src->board, sizeof(dest->board));
    dest->player_turn = src->player_turn;
    dest->game_over = src->game_over;
    dest->consecutive_passes = src->consecutive_passes;
    dest->red_score = src->red_score;
    dest->green_score = src->green_score;
    dest->last_player_row = src->last_player_row;
    dest->last_player_col = src->last_player_col;
    dest->last_ai_row = src->last_ai_row;
    dest->last_ai_col = src->last_ai_col;
}

static void session_game_othello_sync_player_from_snapshot(
    session_ctx_t *player, const othello_game_state_t *snapshot,
    unsigned player_index, int slot_index, bool awaiting_opponent)
{
    if (player == nullptr || snapshot == nullptr) {
        return;
    }

    player->game.active = true;
    player->game.type = SESSION_GAME_OTHELLO;
    player->game.is_camouflaged = false;

    othello_game_state_t *state = &player->game.othello;
    session_game_othello_copy_core(state, snapshot);
    state->awaiting_mode_selection = false;
    state->multiplayer = true;
    state->awaiting_opponent = awaiting_opponent;
    state->slot_index = slot_index;
    state->player_number = player_index + 1U;
    state->player_turn =
        (player_index == 0U) ? snapshot->player_turn : !snapshot->player_turn;
    state->game_over = snapshot->game_over;
}

static othello_multiplayer_slot_t *host_othello_slot_by_id_locked(host_t *host,
                                                                  int slot_id)
{
    if (host == nullptr || slot_id <= 0 ||
        slot_id > (int)SSH_CHATTER_OTHELLO_MAX_SLOTS) {
        return nullptr;
    }

    return &host->othello_games[(size_t)(slot_id - 1)];
}

static othello_multiplayer_slot_t *
host_othello_allocate_slot_locked(host_t *host, const char *owner_name)
{
    if (host == nullptr) {
        return nullptr;
    }

    for (size_t idx = 0U; idx < SSH_CHATTER_OTHELLO_MAX_SLOTS; ++idx) {
        othello_multiplayer_slot_t *slot = &host->othello_games[idx];
        if (slot->in_use) {
            continue;
        }

        slot->in_use = true;
        slot->active = false;
        slot->awaiting_second_player = true;
        slot->owner[0] = '\0';
        if (owner_name != nullptr) {
            snprintf(slot->owner, sizeof(slot->owner), "%s", owner_name);
        }
        slot->players[0] = nullptr;
        slot->players[1] = nullptr;
        session_game_othello_reset_state(&slot->state);
        slot->state.awaiting_mode_selection = false;
        slot->state.multiplayer = true;
        slot->state.awaiting_opponent = true;
        slot->state.slot_index = (int)slot->slot_id;
        slot->state.player_number = 0U;
        return slot;
    }

    return nullptr;
}

static void host_othello_release_slot_locked(host_t *host,
                                             othello_multiplayer_slot_t *slot)
{
    if (host == nullptr || slot == nullptr) {
        return;
    }

    slot->in_use = false;
    slot->active = false;
    slot->awaiting_second_player = false;
    slot->owner[0] = '\0';
    slot->players[0] = nullptr;
    slot->players[1] = nullptr;
    session_game_othello_reset_state(&slot->state);
}

static void session_game_othello_finish_multiplayer(
    host_t *host, othello_multiplayer_slot_t *slot, const char *reason_p1,
    const char *reason_p2)
{
    if (host == nullptr || slot == nullptr) {
        return;
    }

    session_ctx_t *players[2] = {nullptr, nullptr};
    othello_game_state_t snapshot = {0};

    ttak_mutex_lock(&host->lock);
    if (slot->in_use) {
        snapshot = slot->state;
        players[0] = slot->players[0];
        players[1] = slot->players[1];
        host_othello_release_slot_locked(host, slot);
    }
    ttak_mutex_unlock(&host->lock);

    const char *reasons[2] = {reason_p1, reason_p2};

    for (unsigned idx = 0U; idx < 2U; ++idx) {
        session_ctx_t *player = players[idx];
        if (player == nullptr) {
            continue;
        }
        session_game_othello_sync_player_from_snapshot(player, &snapshot, idx,
                                                       -1, false);
        player->game.othello.game_over = true;
        session_game_suspend(player, reasons[idx]);
    }
}

static void session_game_othello_apply_move(othello_game_state_t *state,
                                            int row, int col,
                                            othello_cell_type_t player)
{
    if (state == nullptr || !session_game_othello_in_bounds(row, col)) {
        return;
    }

    state->board[row][col] = (uint8_t)player;
    for (size_t idx = 0U;
         idx < sizeof(kOthelloDirections) / sizeof(kOthelloDirections[0]);
         ++idx) {
        int drow = kOthelloDirections[idx][0];
        int dcol = kOthelloDirections[idx][1];
        int count = session_game_othello_direction_count(state, row, col, drow,
                                                         dcol, player);
        if (count <= 0) {
            continue;
        }

        int r = row + drow;
        int c = col + dcol;
        for (int step = 0; step < count && session_game_othello_in_bounds(r, c);
             ++step) {
            state->board[r][c] = (uint8_t)player;
            r += drow;
            c += dcol;
        }
    }

    session_game_othello_count_scores(state, &state->red_score,
                                      &state->green_score);
}

static unsigned
session_game_othello_collect_moves(const othello_game_state_t *state,
                                   othello_cell_type_t player,
                                   othello_move_t *moves, unsigned max_moves)
{
    if (state == nullptr) {
        return 0U;
    }

    unsigned count = 0U;
    for (int row = 0; row < SSH_CHATTER_OTHELLO_BOARD_SIZE; ++row) {
        for (int col = 0; col < SSH_CHATTER_OTHELLO_BOARD_SIZE; ++col) {
            int flipped =
                session_game_othello_count_flips(state, row, col, player);
            if (flipped <= 0) {
                continue;
            }

            if (moves != nullptr && count < max_moves) {
                moves[count].row = row;
                moves[count].col = col;
                moves[count].flipped = flipped;
            }
            ++count;
        }
    }

    return count;
}

static void session_game_othello_format_coordinate(int row, int col,
                                                   char *buffer, size_t length)
{
    if (buffer == nullptr || length == 0U) {
        return;
    }

    if (!session_game_othello_in_bounds(row, col)) {
        snprintf(buffer, length, "-");
        return;
    }

    char file = (char)('a' + col);
    char rank = (char)('1' + row);
    snprintf(buffer, length, "%c%c", file, rank);
}

static void session_game_othello_handle_line_multiplayer(session_ctx_t *ctx,
                                                         const char *working)
{
    if (ctx == nullptr || working == nullptr || ctx->owner == nullptr) {
        return;
    }

    othello_game_state_t *session_state = &ctx->game.othello;
    host_t *host = ctx->owner;

    if (session_state->awaiting_opponent) {
        if (strcmp(working, "quit") == 0 || strcmp(working, "resign") == 0 ||
            strcmp(working, "exit") == 0) {
            if (session_state->slot_index > 0) {
                ttak_mutex_lock(&host->lock);
                othello_multiplayer_slot_t *slot =
                    host_othello_slot_by_id_locked(host,
                                                   session_state->slot_index);
                if (slot != nullptr && slot->in_use && !slot->active &&
                    slot->players[0] == ctx) {
                    host_othello_release_slot_locked(host, slot);
                }
                ttak_mutex_unlock(&host->lock);
            }
            session_state->multiplayer = false;
            session_state->awaiting_mode_selection = false;
            session_state->awaiting_opponent = false;
            session_state->slot_index = -1;
            session_state->player_number = 0U;
            session_game_suspend(ctx, "Multiplayer matchmaking cancelled.");
        } else {
            session_send_system_line(ctx,
                                     "Waiting for an opponent. Others can join "
                                     "with /othello accept <game-id>.");
        }
        return;
    }

    if (session_state->slot_index <= 0) {
        session_send_system_line(
            ctx, "This multiplayer game is no longer available.");
        session_state->multiplayer = false;
        session_game_suspend(ctx, "Game suspended.");
        return;
    }

    ttak_mutex_lock(&host->lock);
    othello_multiplayer_slot_t *slot =
        host_othello_slot_by_id_locked(host, session_state->slot_index);
    if (slot == nullptr || !slot->in_use) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "This multiplayer game has ended.");
        session_state->multiplayer = false;
        session_game_suspend(ctx, "Game suspended.");
        return;
    }

    unsigned player_index = 2U;
    if (slot->players[0] == ctx) {
        player_index = 0U;
    } else if (slot->players[1] == ctx) {
        player_index = 1U;
    }

    if (player_index >= 2U) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "You are no longer part of this game.");
        session_state->multiplayer = false;
        session_game_suspend(ctx, "Game suspended.");
        return;
    }

    if (!slot->active) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(
            ctx, "Waiting for another player to accept the game.");
        return;
    }

    if (slot->state.game_over) {
        ttak_mutex_unlock(&host->lock);
        session_game_othello_finish_multiplayer(host, slot, nullptr, nullptr);
        return;
    }

    othello_cell_type_t my_color =
        (player_index == 0U) ? OTHELLO_CELL_RED : OTHELLO_CELL_GREEN;
    othello_cell_type_t opponent_color =
        (player_index == 0U) ? OTHELLO_CELL_GREEN : OTHELLO_CELL_RED;

    bool my_turn = (player_index == 0U) ? slot->state.player_turn
                                        : !slot->state.player_turn;
    if (!my_turn) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "Please wait for your turn.");
        return;
    }

    session_ctx_t *players[2] = {slot->players[0], slot->players[1]};
    session_ctx_t *opponent = players[1U - player_index];
    int slot_id = (int)slot->slot_id;

    if (strcmp(working, "pass") == 0) {
        unsigned my_moves = session_game_othello_collect_moves(
            &slot->state, my_color, nullptr, 0U);
        if (my_moves > 0U) {
            ttak_mutex_unlock(&host->lock);
            session_send_system_line(ctx,
                                     "You still have legal moves available.");
            return;
        }

        slot->state.consecutive_passes++;
        if (player_index == 0U) {
            slot->state.last_player_row = -1;
            slot->state.last_player_col = -1;
        } else {
            slot->state.last_ai_row = -1;
            slot->state.last_ai_col = -1;
        }

        bool finish = false;
        if (slot->state.consecutive_passes >= 2U) {
            slot->state.game_over = true;
            session_game_othello_count_scores(
                &slot->state, &slot->state.red_score, &slot->state.green_score);
            finish = true;
        } else {
            slot->state.player_turn = (player_index == 0U) ? false : true;
        }

        othello_game_state_t snapshot = slot->state;
        ttak_mutex_unlock(&host->lock);

        if (finish) {
            session_game_othello_finish_multiplayer(host, slot,
                                                    "No more moves available.",
                                                    "No more moves available.");
            return;
        }

        session_game_othello_sync_player_from_snapshot(players[0], &snapshot,
                                                       0U, slot_id, false);
        session_game_othello_sync_player_from_snapshot(players[1], &snapshot,
                                                       1U, slot_id, false);

        session_send_system_line(ctx, "You pass your turn.");
        if (opponent != nullptr) {
            char notice[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(notice, sizeof(notice), "%s (%s) passes their turn.",
                     player_index == 0U ? "1P" : "2P", ctx->user.name);
            session_send_system_line(opponent, notice);
        }

        if (players[player_index] != nullptr) {
            session_game_othello_prepare_next_turn(players[player_index]);
        }
        if (opponent != nullptr) {
            session_game_othello_prepare_next_turn(opponent);
        }
        return;
    }

    if (strcmp(working, "resign") == 0 || strcmp(working, "quit") == 0) {
        slot->state.game_over = true;
        session_game_othello_count_scores(&slot->state, &slot->state.red_score,
                                          &slot->state.green_score);
        ttak_mutex_unlock(&host->lock);

        char self_reason[64];
        char opp_reason[64];
        snprintf(self_reason, sizeof(self_reason), "You resigned.");
        if (opponent != nullptr) {
            snprintf(opp_reason, sizeof(opp_reason), "%s resigned.",
                     ctx->user.name);
        } else {
            snprintf(opp_reason, sizeof(opp_reason), "Opponent resigned.");
        }
        session_game_othello_finish_multiplayer(host, slot, self_reason,
                                                opp_reason);
        return;
    }

    int row = -1;
    int col = -1;
    if (isalpha((unsigned char)working[0]) &&
        isdigit((unsigned char)working[1])) {
        col = working[0] - 'a';
        row = working[1] - '1';
    } else if (isdigit((unsigned char)working[0]) &&
               isalpha((unsigned char)working[1])) {
        row = working[0] - '1';
        col = working[1] - 'a';
    }

    if (!session_game_othello_in_bounds(row, col)) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "Invalid move. Use coordinates like d3.");
        return;
    }

    int flips =
        session_game_othello_count_flips(&slot->state, row, col, my_color);
    if (flips <= 0) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "That square is not a legal move.");
        return;
    }

    session_game_othello_apply_move(&slot->state, row, col, my_color);
    if (player_index == 0U) {
        slot->state.last_player_row = row;
        slot->state.last_player_col = col;
    } else {
        slot->state.last_ai_row = row;
        slot->state.last_ai_col = col;
    }
    slot->state.consecutive_passes = 0U;

    unsigned opponent_moves = session_game_othello_collect_moves(
        &slot->state, opponent_color, nullptr, 0U);
    unsigned my_future_moves =
        session_game_othello_collect_moves(&slot->state, my_color, nullptr, 0U);

    bool opponent_forced_pass = false;
    bool finish = false;
    if (opponent_moves == 0U) {
        opponent_forced_pass = true;
        slot->state.consecutive_passes++;
        if (slot->state.consecutive_passes >= 2U || my_future_moves == 0U) {
            slot->state.game_over = true;
            session_game_othello_count_scores(
                &slot->state, &slot->state.red_score, &slot->state.green_score);
            finish = true;
        } else {
            slot->state.player_turn = (player_index == 0U) ? true : false;
        }
    } else {
        slot->state.player_turn = (player_index == 0U) ? false : true;
    }

    char coord[8];
    session_game_othello_format_coordinate(row, col, coord, sizeof(coord));

    othello_game_state_t snapshot = slot->state;
    ttak_mutex_unlock(&host->lock);

    if (finish) {
        session_game_othello_finish_multiplayer(
            host, slot, "No more moves available.", "No more moves available.");
        return;
    }

    session_game_othello_sync_player_from_snapshot(players[0], &snapshot, 0U,
                                                   slot_id, false);
    session_game_othello_sync_player_from_snapshot(players[1], &snapshot, 1U,
                                                   slot_id, false);

    if (players[player_index] != nullptr) {
        char self_message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(self_message, sizeof(self_message), "You place a piece at %s.",
                 coord);
        session_send_system_line(players[player_index], self_message);
    }
    if (opponent != nullptr) {
        char other_message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(other_message, sizeof(other_message),
                 "%s (%s) places a piece at %s.",
                 player_index == 0U ? "1P" : "2P", ctx->user.name, coord);
        session_send_system_line(opponent, other_message);
    }

    if (opponent_forced_pass) {
        if (players[player_index] != nullptr) {
            session_send_system_line(players[player_index],
                                     "Opponent has no moves and must pass.");
        }
        if (opponent != nullptr) {
            session_send_system_line(opponent,
                                     "You have no legal moves and must pass.");
        }
    }

    if (players[player_index] != nullptr) {
        session_game_othello_render(players[player_index]);
        session_game_othello_prepare_next_turn(players[player_index]);
    }
    if (opponent != nullptr) {
        session_game_othello_render(opponent);
        session_game_othello_prepare_next_turn(opponent);
    }
}

bool session_game_othello_handle_forced_exit(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return false;
    }

    if (!ctx->game.active || ctx->game.type != SESSION_GAME_OTHELLO) {
        return false;
    }

    othello_game_state_t *state = &ctx->game.othello;
    if (!state->multiplayer || state->slot_index <= 0) {
        return false;
    }

    host_t *host = ctx->owner;
    ttak_mutex_lock(&host->lock);
    othello_multiplayer_slot_t *slot =
        host_othello_slot_by_id_locked(host, state->slot_index);
    if (slot == nullptr || !slot->in_use || !slot->active) {
        ttak_mutex_unlock(&host->lock);
        return false;
    }

    bool is_player_one = slot->players[0] == ctx;
    bool is_player_two = slot->players[1] == ctx;
    session_ctx_t *opponent = nullptr;
    if (is_player_one) {
        opponent = slot->players[1];
    } else if (is_player_two) {
        opponent = slot->players[0];
    } else {
        ttak_mutex_unlock(&host->lock);
        return false;
    }

    slot->state.game_over = true;
    session_game_othello_count_scores(&slot->state, &slot->state.red_score,
                                      &slot->state.green_score);
    ttak_mutex_unlock(&host->lock);

    const char *resigner_name =
        (ctx->user.name[0] != '\0') ? ctx->user.name : "Opponent";

    char reason_p1[64];
    char reason_p2[64];
    if (is_player_one) {
        snprintf(reason_p1, sizeof(reason_p1), "You resigned.");
        if (opponent != nullptr) {
            snprintf(reason_p2, sizeof(reason_p2), "%s resigned.",
                     resigner_name);
        } else {
            snprintf(reason_p2, sizeof(reason_p2), "Opponent resigned.");
        }
    } else {
        if (opponent != nullptr) {
            snprintf(reason_p1, sizeof(reason_p1), "%s resigned.",
                     resigner_name);
        } else {
            snprintf(reason_p1, sizeof(reason_p1), "Opponent resigned.");
        }
        snprintf(reason_p2, sizeof(reason_p2), "You resigned.");
    }

    session_game_othello_finish_multiplayer(host, slot, reason_p1, reason_p2);
    return true;
}

static void session_game_othello_render(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_OTHELLO) {
        return;
    }

    othello_game_state_t *state = &ctx->game.othello;
    session_game_othello_count_scores(state, &state->red_score,
                                      &state->green_score);

    bool previous_translation = ctx->translation_suppress_output;
    ctx->translation_suppress_output = true;

    // Start buffering to send entire game board in one flush
    session_output_buffer_start(ctx);

    session_render_separator(ctx, "Othello");
    session_send_system_line(ctx, "    a b c d e f g h");
    for (int row = 0; row < SSH_CHATTER_OTHELLO_BOARD_SIZE; ++row) {
        char line[256];
        int offset = snprintf(line, sizeof(line), " %d ", row + 1);
        for (int col = 0; col < SSH_CHATTER_OTHELLO_BOARD_SIZE; ++col) {
            const char *symbol = ".";
            if (state->board[row][col] == OTHELLO_CELL_RED) {
                symbol = "\033[31mo\033[0m";
            } else if (state->board[row][col] == OTHELLO_CELL_GREEN) {
                symbol = "\033[32mo\033[0m";
            }

            offset += snprintf(line + offset, sizeof(line) - (size_t)offset,
                               "%s ", symbol);
            if (offset >= (int)sizeof(line)) {
                break;
            }
        }
        snprintf(line + (size_t)offset, sizeof(line) - (size_t)offset, "%d",
                 row + 1);
        session_send_raw_text(ctx, line);
    }
    session_send_system_line(ctx, "    a b c d e f g h");

    const char *red_label = "Red";
    const char *green_label = "Green";
    if (state->multiplayer) {
        bool is_player_one = ctx->game.othello.player_number == 1U;
        bool is_player_two = ctx->game.othello.player_number == 2U;
        red_label = is_player_one ? "1P (You)" : "1P";
        green_label = is_player_two ? "2P (You)" : "2P";
    }

    char score_line[128];
    snprintf(score_line, sizeof(score_line), "Score - %s %u : %s %u", red_label,
             state->red_score, green_label, state->green_score);
    session_send_system_line(ctx, score_line);

    char red_coord[8];
    char green_coord[8];
    session_game_othello_format_coordinate(state->last_player_row,
                                           state->last_player_col, red_coord,
                                           sizeof(red_coord));
    session_game_othello_format_coordinate(state->last_ai_row,
                                           state->last_ai_col, green_coord,
                                           sizeof(green_coord));

    if (state->last_player_row >= 0 || state->last_ai_row >= 0) {
        char last_line[128];
        if (state->multiplayer) {
            snprintf(last_line, sizeof(last_line),
                     "Last moves - 1P: %s  2P: %s", red_coord, green_coord);
        } else {
            snprintf(last_line, sizeof(last_line),
                     "Last moves - Red: %s  Green: %s", red_coord, green_coord);
        }
        session_send_system_line(ctx, last_line);
    }

    // Flush all buffered output at once
    session_output_buffer_stop(ctx);

    ctx->translation_suppress_output = previous_translation;
}

static void session_game_othello_finish(session_ctx_t *ctx, const char *reason)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_OTHELLO) {
        return;
    }

    othello_game_state_t *state = &ctx->game.othello;
    if (!state->game_over) {
        session_game_othello_count_scores(state, &state->red_score,
                                          &state->green_score);
        state->game_over = true;
    }

    const char *message =
        (reason != nullptr && reason[0] != '\0') ? reason : "Othello finished.";

    if (state->multiplayer) {
        host_t *host = ctx->owner;
        othello_multiplayer_slot_t *slot = nullptr;
        if (host != nullptr && state->slot_index > 0) {
            ttak_mutex_lock(&host->lock);
            slot = host_othello_slot_by_id_locked(host, state->slot_index);
            ttak_mutex_unlock(&host->lock);
        }

        if (host != nullptr && slot != nullptr) {
            const char *outcome =
                (reason != nullptr && reason[0] != '\0') ? reason : nullptr;
            session_game_othello_finish_multiplayer(host, slot, outcome,
                                                    outcome);
        } else {
            session_game_suspend(ctx, message);
        }
        return;
    }

    session_game_suspend(ctx, message);
}

static void session_game_othello_handle_ai_turn(session_ctx_t *ctx);

static void session_game_othello_prepare_next_turn(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_OTHELLO) {
        return;
    }

    othello_game_state_t *state = &ctx->game.othello;
    if (state->game_over) {
        return;
    }

    if (state->multiplayer) {
        if (state->awaiting_opponent) {
            session_send_system_line(
                ctx, "Waiting for an opponent to join this game.");
            return;
        }

        if (ctx->game.othello.player_number == 0U) {
            session_send_system_line(ctx,
                                     "Waiting for multiplayer assignment.");
            return;
        }

        if (state->player_turn) {
            session_send_system_line(ctx,
                                     "Enter your move (e.g., d3). Type 'pass' "
                                     "if no moves.");
        } else {
            if (ctx->game.othello.player_number == 1U) {
                session_send_system_line(ctx, "Waiting for 2P's move.");
            } else {
                session_send_system_line(ctx, "Waiting for 1P's move.");
            }
        }
        return;
    }

    unsigned player_moves = session_game_othello_collect_moves(
        state, OTHELLO_CELL_RED, nullptr, 0U);
    unsigned ai_moves = session_game_othello_collect_moves(
        state, OTHELLO_CELL_GREEN, nullptr, 0U);

    if (player_moves == 0U && ai_moves == 0U) {
        session_game_othello_finish(ctx, "No more moves available.");
        return;
    }

    if (player_moves == 0U) {
        state->consecutive_passes++;
        if (state->consecutive_passes >= 2U) {
            session_game_othello_finish(ctx, "No more moves available.");
            return;
        }

        session_send_system_line(ctx, "You have no legal moves and must pass.");
        state->player_turn = false;
        session_game_othello_handle_ai_turn(ctx);
        return;
    }

    if (ai_moves == 0U) {
        state->consecutive_passes++;
        if (state->consecutive_passes >= 2U) {
            session_game_othello_finish(ctx, "No more moves available.");
            return;
        }

        session_send_system_line(ctx, "Green has no legal moves and passes.");
        state->player_turn = true;
        session_send_system_line(
            ctx, "Enter your move (e.g., d3). Type 'pass' if no moves.");
        return;
    }

    state->player_turn = true;
    session_send_system_line(
        ctx, "Enter your move (e.g., d3). Type 'pass' if no moves.");
}

static void session_game_othello_handle_ai_turn(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_OTHELLO) {
        return;
    }

    othello_game_state_t *state = &ctx->game.othello;
    if (state->multiplayer) {
        return;
    }
    if (state->game_over) {
        return;
    }

    othello_move_t moves[SSH_CHATTER_OTHELLO_MAX_MOVES];
    unsigned move_count = session_game_othello_collect_moves(
        state, OTHELLO_CELL_GREEN, moves, SSH_CHATTER_OTHELLO_MAX_MOVES);

    if (move_count == 0U) {
        state->consecutive_passes++;
        session_send_system_line(ctx, "Green has no legal moves and passes.");
        if (state->consecutive_passes >= 2U) {
            session_game_othello_finish(ctx, "No more moves available.");
            return;
        }

        state->player_turn = true;
        session_game_othello_prepare_next_turn(ctx);
        return;
    }

    session_game_seed_rng(ctx);

    othello_move_t chosen;
    unsigned difficulty = state->difficulty_level;

    // Difficulty-based AI decision making
    if (difficulty == 1U) {
        // Level 1: Completely random moves
        size_t random_idx =
            (size_t)session_game_random_range(ctx, (int)move_count);
        chosen = moves[random_idx];
    } else {
        // Find best moves for other difficulty levels
        int best_flips = -1;
        size_t best_indexes[SSH_CHATTER_OTHELLO_MAX_MOVES];
        size_t best_count = 0U;
        for (unsigned idx = 0U; idx < move_count; ++idx) {
            if (moves[idx].flipped > best_flips) {
                best_flips = moves[idx].flipped;
                best_indexes[0] = idx;
                best_count = 1U;
            } else if (moves[idx].flipped == best_flips &&
                       best_count < SSH_CHATTER_OTHELLO_MAX_MOVES) {
                best_indexes[best_count++] = idx;
            }
        }

        // Decide whether to make a good move or random move based on difficulty
        bool make_good_move = true;
        if (difficulty == 2U) {
            // Level 2: 60% good moves, 40% random
            make_good_move = (session_game_random_range(ctx, 100) < 60);
        } else if (difficulty == 3U) {
            // Level 3: 80% good moves, 20% random
            make_good_move = (session_game_random_range(ctx, 100) < 80);
        } else if (difficulty == 4U) {
            // Level 4: 95% good moves, 5% random
            make_good_move = (session_game_random_range(ctx, 100) < 95);
        }
        // Level 5: Always good moves (make_good_move stays true)

        if (make_good_move) {
            size_t choice_index = 0U;
            if (best_count > 1U) {
                choice_index =
                    (size_t)session_game_random_range(ctx, (int)best_count);
            }
            chosen = moves[best_indexes[choice_index]];
        } else {
            // Make a random move
            size_t random_idx =
                (size_t)session_game_random_range(ctx, (int)move_count);
            chosen = moves[random_idx];
        }
    }

    session_game_othello_apply_move(state, chosen.row, chosen.col,
                                    OTHELLO_CELL_GREEN);
    state->last_ai_row = chosen.row;
    state->last_ai_col = chosen.col;
    state->consecutive_passes = 0U;
    state->player_turn = true;

    char coord[8];
    session_game_othello_format_coordinate(chosen.row, chosen.col, coord,
                                           sizeof(coord));
    char move_message[64];
    snprintf(move_message, sizeof(move_message), "Green plays %s.", coord);

    session_game_othello_render(ctx);
    session_send_system_line(ctx, move_message);

    session_game_othello_prepare_next_turn(ctx);
}

static void session_game_othello_handle_line(session_ctx_t *ctx,
                                             const char *line)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_OTHELLO ||
        line == nullptr) {
        return;
    }

    othello_game_state_t *state = &ctx->game.othello;

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", line);
    trim_whitespace_inplace(working);
    if (working[0] == '\0') {
        return;
    }

    for (size_t idx = 0U; working[idx] != '\0'; ++idx) {
        if (isalpha((unsigned char)working[idx])) {
            working[idx] = (char)tolower((unsigned char)working[idx]);
        }
    }

    // Handle camouflage toggle with 't' command
    if (strcmp(working, "t") == 0) {
        if (ctx->game.is_camouflaged) {
            ctx->game.is_camouflaged = false;
            ctx->game.saved_othello_state = ctx->game.othello;
            session_game_othello_render(ctx);
        } else {
            ctx->game.is_camouflaged = true;
            ctx->game.saved_othello_state = ctx->game.othello;
            session_game_show_camouflage(ctx);
        }
        return;
    }

    if (state->awaiting_mode_selection) {
        if (strcmp(working, "single") == 0 || strcmp(working, "s") == 0) {
            state->awaiting_mode_selection = false;
            state->awaiting_difficulty_selection = true;
            session_send_system_line(ctx, "");
            session_send_system_line(ctx, "Choose difficulty level (1-5):");
            session_send_system_line(ctx, "  1 - Very Easy (random moves)");
            session_send_system_line(ctx, "  2 - Easy (60% good moves)");
            session_send_system_line(ctx, "  3 - Medium (80% good moves)");
            session_send_system_line(ctx, "  4 - Hard (95% good moves)");
            session_send_system_line(ctx, "  5 - Expert (always best moves)");
            session_send_system_line(ctx, "Type a number 1-5:");
        } else if (strcmp(working, "multi") == 0 || strcmp(working, "m") == 0) {
            if (ctx->owner == nullptr) {
                session_send_system_line(
                    ctx, "Multiplayer mode is unavailable right now.");
                return;
            }

            size_t member_count = 0U;
            ttak_mutex_lock(&ctx->owner->room.lock);
            member_count = ctx->owner->room.member_count;
            ttak_mutex_unlock(&ctx->owner->room.lock);

            if (member_count < 2U) {
                session_send_system_line(
                    ctx, "At least two connected users are required for "
                         "multiplayer Othello.");
                return;
            }

            othello_multiplayer_slot_t *slot = nullptr;
            int slot_id = -1;
            ttak_mutex_lock(&ctx->owner->lock);
            slot =
                host_othello_allocate_slot_locked(ctx->owner, ctx->user.name);
            if (slot != nullptr) {
                slot->players[0] = ctx;
                slot_id = (int)slot->slot_id;
            }
            ttak_mutex_unlock(&ctx->owner->lock);

            if (slot == nullptr) {
                session_send_system_line(
                    ctx, "All multiplayer Othello slots are currently in use.");
                return;
            }

            state->awaiting_mode_selection = false;
            state->multiplayer = true;
            state->awaiting_opponent = true;
            state->slot_index = slot_id;
            state->player_number = 0U;
            state->player_turn = false;
            session_game_othello_copy_core(state, &slot->state);

            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message),
                     "Waiting for an opponent. Share game #%d so another "
                     "player can /othello accept it.",
                     slot_id);
            session_send_system_line(ctx, message);
            session_game_othello_render(ctx);
            session_game_othello_prepare_next_turn(ctx);
        } else if (strcmp(working, "exit") == 0 ||
                   strcmp(working, "quit") == 0) {
            session_game_suspend(ctx, "Game cancelled.");
        } else {
            session_send_system_line(
                ctx, "Type 'single' or 'multi' to choose how to play.");
        }
        return;
    }

    if (state->awaiting_difficulty_selection) {
        int difficulty = atoi(working);
        if (difficulty < 1 || difficulty > 5) {
            session_send_system_line(
                ctx,
                "Invalid difficulty. Please enter a number between 1 and 5.");
            return;
        }

        session_game_othello_reset_state(state);
        state->difficulty_level = (unsigned)difficulty;
        state->player_number = 1U;
        state->awaiting_difficulty_selection = false;

        char msg[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(msg, sizeof(msg), "Difficulty set to %d. Good luck!",
                 difficulty);
        session_send_system_line(ctx, msg);
        session_send_system_line(ctx, "");

        session_game_othello_render(ctx);
        session_send_system_line(ctx, "You are Red (\033[31mo\033[0m). Green "
                                      "(\033[32mo\033[0m) will respond after "
                                      "your move.");
        session_game_othello_prepare_next_turn(ctx);
        return;
    }

    if (state->multiplayer) {
        session_game_othello_handle_line_multiplayer(ctx, working);
        return;
    }

    if (state->game_over) {
        session_send_system_line(ctx,
                                 "The game is over. Use /game to start again.");
        return;
    }

    if (!state->player_turn) {
        session_send_system_line(ctx, "Please wait for your turn.");
        return;
    }

    unsigned player_moves = session_game_othello_collect_moves(
        state, OTHELLO_CELL_RED, nullptr, 0U);
    if (strcmp(working, "pass") == 0) {
        if (player_moves > 0U) {
            session_send_system_line(ctx,
                                     "You still have legal moves available.");
            return;
        }

        state->consecutive_passes++;
        session_send_system_line(ctx, "You pass your turn.");
        if (state->consecutive_passes >= 2U) {
            session_game_othello_finish(ctx, "No more moves available.");
            return;
        }

        state->player_turn = false;
        session_game_othello_handle_ai_turn(ctx);
        return;
    }

    if (strcmp(working, "resign") == 0 || strcmp(working, "quit") == 0) {
        session_game_othello_count_scores(state, &state->red_score,
                                          &state->green_score);
        session_game_othello_finish(ctx, "You resigned.");
        return;
    }

    int row = -1;
    int col = -1;
    if (isalpha((unsigned char)working[0]) &&
        isdigit((unsigned char)working[1])) {
        col = working[0] - 'a';
        row = working[1] - '1';
    } else if (isdigit((unsigned char)working[0]) &&
               isalpha((unsigned char)working[1])) {
        row = working[0] - '1';
        col = working[1] - 'a';
    }

    if (!session_game_othello_in_bounds(row, col)) {
        session_send_system_line(ctx, "Invalid move. Use coordinates like d3.");
        return;
    }

    int flips =
        session_game_othello_count_flips(state, row, col, OTHELLO_CELL_RED);
    if (flips <= 0) {
        session_send_system_line(ctx, "That square is not a legal move.");
        return;
    }

    session_game_othello_apply_move(state, row, col, OTHELLO_CELL_RED);
    state->last_player_row = row;
    state->last_player_col = col;
    state->player_turn = false;
    state->consecutive_passes = 0U;

    session_game_othello_render(ctx);

    session_game_othello_handle_ai_turn(ctx);
}

static void session_game_start_othello(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    // Ask for camouflage language first
    session_send_system_line(
        ctx,
        "CHOOSE YOUR LOCKSCREEN LANGUAGE TO HIDE THE SCREEN ON YOUR OFFICE! "
        "(c, cpp, java, go, js, ts, rust)");
    char language_choice[16];
    size_t length = 0U;
    while (length + 1U < sizeof(language_choice)) {
        char ch = '\0';
        const int read_result = session_transport_read(ctx, &ch, 1, -1);
        if (read_result <= 0) {
            return;
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

        language_choice[length++] = ch;
        session_local_echo_char(ctx, ch);
    }
    language_choice[length] = '\0';
    trim_whitespace_inplace(language_choice);
    for (size_t idx = 0U; language_choice[idx] != '\0'; ++idx) {
        language_choice[idx] =
            (char)tolower((unsigned char)language_choice[idx]);
    }

    if (language_choice[0] == '\0') {
        session_send_system_line(ctx, "No language chosen. Defaulting to C.");
        snprintf(ctx->game.chosen_camouflage_language,
                 sizeof(ctx->game.chosen_camouflage_language), "c");
    } else if (strcmp(language_choice, "c") == 0 ||
               strcmp(language_choice, "cpp") == 0 ||
               strcmp(language_choice, "java") == 0 ||
               strcmp(language_choice, "go") == 0 ||
               strcmp(language_choice, "js") == 0 ||
               strcmp(language_choice, "ts") == 0 ||
               strcmp(language_choice, "rust") == 0) {
        snprintf(ctx->game.chosen_camouflage_language,
                 sizeof(ctx->game.chosen_camouflage_language), "%s",
                 language_choice);
    } else {
        session_send_system_line(ctx, "Invalid language. Defaulting to C.");
        snprintf(ctx->game.chosen_camouflage_language,
                 sizeof(ctx->game.chosen_camouflage_language), "c");
    }

    ctx->game.active = true;
    ctx->game.type = SESSION_GAME_OTHELLO;
    ctx->game.is_camouflaged = false;
    session_game_seed_rng(ctx);
    session_game_othello_reset_state(&ctx->game.othello);
    ctx->game.othello.awaiting_mode_selection = true;
    ctx->game.othello.player_turn = false;
    session_send_system_line(ctx, "");
    session_send_system_line(
        ctx,
        "Choose Othello mode: type 'single' to play the AI or 'multi' to wait "
        "for another player. Type 'exit' to cancel.");
}

static void session_othello_list_games(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    int ids[SSH_CHATTER_OTHELLO_MAX_SLOTS];
    char owners[SSH_CHATTER_OTHELLO_MAX_SLOTS][SSH_CHATTER_USERNAME_LEN];
    size_t count = 0U;

    ttak_mutex_lock(&ctx->owner->lock);
    for (size_t idx = 0U; idx < SSH_CHATTER_OTHELLO_MAX_SLOTS; ++idx) {
        othello_multiplayer_slot_t *slot = &ctx->owner->othello_games[idx];
        if (!slot->in_use || slot->active || !slot->awaiting_second_player) {
            continue;
        }
        if (count < SSH_CHATTER_OTHELLO_MAX_SLOTS) {
            ids[count] = slot->slot_id;
            if (slot->owner[0] != '\0') {
                snprintf(owners[count], sizeof(owners[count]), "%s",
                         slot->owner);
            } else {
                owners[count][0] = '\0';
            }
            ++count;
        }
    }
    ttak_mutex_unlock(&ctx->owner->lock);

    if (count == 0U) {
        session_send_system_line(
            ctx, "No open multiplayer Othello games right now.");
        return;
    }

    session_send_system_line(ctx, "Open multiplayer Othello games:");
    for (size_t idx = 0U; idx < count; ++idx) {
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        if (owners[idx][0] != '\0') {
            snprintf(line, sizeof(line), "  #%d - host: %s", ids[idx],
                     owners[idx]);
        } else {
            snprintf(line, sizeof(line), "  #%d - host: unknown", ids[idx]);
        }
        session_send_system_line(ctx, line);
    }
}

static void session_othello_accept_game(session_ctx_t *ctx, unsigned slot_id)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (slot_id == 0U) {
        session_send_system_line(ctx, "Provide a valid game number.");
        return;
    }

    if (ctx->game.active) {
        session_send_system_line(
            ctx, "Finish your current game before accepting another match.");
        return;
    }

    host_t *host = ctx->owner;
    ttak_mutex_lock(&host->lock);
    othello_multiplayer_slot_t *slot =
        host_othello_slot_by_id_locked(host, (int)slot_id);
    if (slot == nullptr || !slot->in_use || slot->active ||
        !slot->awaiting_second_player) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "That game is not available.");
        return;
    }

    if (slot->players[0] == ctx) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "You cannot accept your own game.");
        return;
    }

    session_ctx_t *creator = slot->players[0];
    if (creator == nullptr || creator->owner != host ||
        creator->game.type != SESSION_GAME_OTHELLO ||
        !creator->game.othello.multiplayer) {
        host_othello_release_slot_locked(host, slot);
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "That game is no longer available.");
        return;
    }

    session_ctx_t *player_one = creator;
    session_ctx_t *player_two = ctx;
    if (session_game_random_range(creator, 2) == 1) {
        player_one = ctx;
        player_two = creator;
    }

    session_game_othello_reset_state(&slot->state);
    slot->state.multiplayer = true;
    slot->state.awaiting_mode_selection = false;
    slot->state.awaiting_opponent = false;
    slot->state.slot_index = slot->slot_id;
    slot->state.player_turn = true;
    slot->state.player_number = 0U;

    slot->players[0] = player_one;
    slot->players[1] = player_two;
    slot->awaiting_second_player = false;
    slot->active = true;

    othello_game_state_t snapshot = slot->state;
    ttak_mutex_unlock(&host->lock);

    session_game_seed_rng(player_two);

    session_game_othello_sync_player_from_snapshot(player_one, &snapshot, 0U,
                                                   slot->slot_id, false);
    session_game_othello_sync_player_from_snapshot(player_two, &snapshot, 1U,
                                                   slot->slot_id, false);

    char announcement[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(announcement, sizeof(announcement),
             "Multiplayer Othello game #%u ready: %s vs %s.", slot_id,
             player_one->user.name, player_two->user.name);
    session_send_system_line(player_one, announcement);
    session_send_system_line(player_two, announcement);

    session_send_system_line(player_one,
                             "You are 1P (Red). Enter your move to begin.");
    session_send_system_line(player_two,
                             "You are 2P (Green). Wait for 1P's move.");

    session_game_othello_render(player_one);
    session_game_othello_render(player_two);
    session_game_othello_prepare_next_turn(player_one);
    session_game_othello_prepare_next_turn(player_two);
}

static void session_handle_othello_command(session_ctx_t *ctx,
                                           const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    const char *usage = "Usage: /othello <list|accept <game-id>>";

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

    char *command = working;
    char *rest = nullptr;
    for (char *cursor = working; *cursor != '\0'; ++cursor) {
        if (isspace((unsigned char)*cursor)) {
            *cursor = '\0';
            rest = cursor + 1;
            break;
        }
    }
    if (rest != nullptr) {
        trim_whitespace_inplace(rest);
    }

    for (size_t idx = 0U; command[idx] != '\0'; ++idx) {
        command[idx] = (char)tolower((unsigned char)command[idx]);
    }

    if (strcmp(command, "list") == 0) {
        if (rest != nullptr && rest[0] != '\0') {
            session_send_system_line(
                ctx, "Usage: /othello list (no extra arguments).");
            return;
        }
        session_othello_list_games(ctx);
        return;
    }

    if (strcmp(command, "accept") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_send_system_line(ctx, "Usage: /othello accept <game-id>");
            return;
        }

        char *endptr = nullptr;
        unsigned long parsed = strtoul(rest, &endptr, 10);
        if (endptr == rest || (endptr != nullptr && *endptr != '\0') ||
            parsed == 0UL || parsed > SSH_CHATTER_OTHELLO_MAX_SLOTS) {
            session_send_system_line(ctx, "Provide a valid game number.");
            return;
        }

        session_othello_accept_game(ctx, (unsigned)parsed);
        return;
    }

    session_send_system_line(ctx, usage);
}

static void
session_game_alpha_add_gravity_source(alpha_centauri_game_state_t *state, int x,
                                      int y, double mu, int influence_radius,
                                      char symbol, const char *name)
{
    if (state == nullptr ||
        state->gravity_source_count >= ALPHA_MAX_GRAVITY_SOURCES) {
        return;
    }

    if (x < 0) {
        x = 0;
    } else if (x >= ALPHA_NAV_WIDTH) {
        x = ALPHA_NAV_WIDTH - 1;
    }

    if (y < 0) {
        y = 0;
    } else if (y >= ALPHA_NAV_HEIGHT) {
        y = ALPHA_NAV_HEIGHT - 1;
    }

    alpha_gravity_source_t *source =
        &state->gravity_sources[state->gravity_source_count++];
    source->x = x;
    source->y = y;
    source->mu = mu >= 0.0 ? mu : 0.0;
    source->influence_radius = influence_radius > 0 ? influence_radius : 0;
    source->symbol = symbol;
    if (name != nullptr) {
        snprintf(source->name, sizeof(source->name), "%s", name);
    } else {
        source->name[0] = '\0';
    }
}

static void session_game_alpha_configure_gravity(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_ALPHA) {
        return;
    }

    alpha_centauri_game_state_t *state = &ctx->game.alpha;

    for (unsigned idx = 0U; idx < ALPHA_MAX_GRAVITY_SOURCES; ++idx) {
        state->gravity_sources[idx] = (alpha_gravity_source_t){0};
    }
    state->gravity_source_count = 0U;

    double stage_multiplier = 1.0 + (double)state->stage * 0.45;
    if (state->stage >= 4U) {
        stage_multiplier += (double)state->waypoint_index * 0.35;
        if (state->awaiting_flag) {
            stage_multiplier += 0.75;
        }
    }
    const char *hole_name =
        state->stage >= 3 ? "Proxima Abyss" : "Core Singularity";
    unsigned special_sources = 0U;
    if (state->stage == 4U) {
        if (!state->eva_ready || state->final_waypoint.symbol == '\0') {
            session_game_alpha_plan_waypoints(ctx);
        }
        if (!state->eva_ready) {
            special_sources += state->waypoint_count;
        }
        if (state->final_waypoint.symbol != '\0') {
            ++special_sources;
        }
    }
    double hole_mu = ALPHA_BLACK_HOLE_MU *
                     session_game_alpha_random_double(ctx, stage_multiplier,
                                                      stage_multiplier + 1.0);
    session_game_alpha_place_random_source(ctx, state, ALPHA_NAV_MARGIN,
                                           hole_mu, ALPHA_NAV_MARGIN * 3, 'B',
                                           hole_name);

    int star_count = 2 + (int)state->stage;
    if (state->stage >= 2U) {
        star_count += 1;
    }
    int planet_count = 1 + (int)((state->stage + 1U) / 2U);
    int debris_count = 1 + (int)state->stage * 2;
    if (state->stage >= 3U) {
        debris_count += 1;
    }
    if (state->stage >= 4U) {
        planet_count = 0;
        debris_count += (int)state->waypoint_index;
        if (state->awaiting_flag) {
            debris_count += 2;
        }
    }

    int available_slots =
        (int)ALPHA_MAX_GRAVITY_SOURCES - 1 - (int)special_sources;
    if (available_slots < 0) {
        available_slots = 0;
    }
    if (star_count > available_slots) {
        star_count = available_slots;
    }
    available_slots -= star_count;
    if (available_slots < 0) {
        available_slots = 0;
    }
    if (planet_count > available_slots) {
        planet_count = available_slots;
    }
    available_slots -= planet_count;
    if (available_slots < 0) {
        available_slots = 0;
    }
    if (debris_count > available_slots) {
        debris_count = available_slots;
    }

    for (int idx = 0; idx < star_count; ++idx) {
        const char *name = kAlphaStarCatalog[session_game_random_range(
            ctx, (int)ALPHA_STAR_CATALOG_COUNT)];
        double mu = ALPHA_STAR_MU *
                    session_game_alpha_random_double(
                        ctx, stage_multiplier * 0.7, stage_multiplier * 1.4);
        session_game_alpha_place_random_source(ctx, state, ALPHA_NAV_MARGIN / 2,
                                               mu, ALPHA_NAV_MARGIN * 2, 'S',
                                               name);
    }

    for (int idx = 0; idx < planet_count; ++idx) {
        const char *name = kAlphaPlanetCatalog[session_game_random_range(
            ctx, (int)ALPHA_PLANET_CATALOG_COUNT)];
        double mu = ALPHA_PLANET_MU *
                    session_game_alpha_random_double(
                        ctx, stage_multiplier * 0.8, stage_multiplier * 1.6);
        session_game_alpha_place_random_source(ctx, state, ALPHA_NAV_MARGIN / 2,
                                               mu, ALPHA_NAV_MARGIN * 2, 'P',
                                               name);
    }

    for (int idx = 0; idx < debris_count; ++idx) {
        const char *name = kAlphaDebrisCatalog[session_game_random_range(
            ctx, (int)ALPHA_DEBRIS_CATALOG_COUNT)];
        double mu = ALPHA_DEBRIS_MU *
                    session_game_alpha_random_double(ctx, 0.7, 2.1) *
                    stage_multiplier;
        session_game_alpha_place_random_source(ctx, state, ALPHA_NAV_MARGIN / 3,
                                               mu, ALPHA_NAV_MARGIN, 'D', name);
    }

    if (state->stage == 4U) {
        if (!state->eva_ready) {
            for (unsigned idx = 0U; idx < state->waypoint_count; ++idx) {
                const alpha_waypoint_t *waypoint = &state->waypoints[idx];
                session_game_alpha_add_gravity_source(
                    state, waypoint->x, waypoint->y, ALPHA_PLANET_MU,
                    ALPHA_NAV_MARGIN * 2, waypoint->symbol, waypoint->name);
            }
        }
        if (state->final_waypoint.symbol != '\0') {
            session_game_alpha_add_gravity_source(
                state, state->final_waypoint.x, state->final_waypoint.y,
                ALPHA_PLANET_MU, ALPHA_NAV_MARGIN * 2,
                state->final_waypoint.symbol, state->final_waypoint.name);
        }
    }
}

static void session_game_alpha_apply_gravity(alpha_centauri_game_state_t *state)
{
    if (state == nullptr || state->gravity_source_count == 0U) {
        return;
    }

    double fx = state->nav_fx;
    double fy = state->nav_fy;
    double ax = 0.0;
    double ay = 0.0;

    for (unsigned idx = 0U; idx < state->gravity_source_count; ++idx) {
        const alpha_gravity_source_t *source = &state->gravity_sources[idx];
        if (source->mu <= 0.0) {
            continue;
        }

        double dx = (double)source->x - fx;
        double dy = (double)source->y - fy;
        double distance_sq = (dx * dx) + (dy * dy);
        double distance = sqrt(distance_sq);
        if (distance < ALPHA_GRAVITY_MIN_DISTANCE) {
            distance = ALPHA_GRAVITY_MIN_DISTANCE;
        }

        double radius = source->influence_radius > 0
                            ? (double)source->influence_radius
                            : (double)ALPHA_NAV_MARGIN;
        double attenuation = 1.0;
        if (radius > 0.0) {
            double normalized = distance / radius;
            if (normalized > 1.0) {
                attenuation = 1.0 / (normalized * normalized);
            }
        }

        double force = (source->mu * attenuation) / (distance * distance);
        if (force <= 0.0) {
            continue;
        }

        ax += force * (dx / distance);
        ay += force * (dy / distance);
    }

    double accel_magnitude = hypot(ax, ay);
    if (accel_magnitude > ALPHA_GRAVITY_MAX_ACCEL && accel_magnitude > 0.0) {
        double accel_scale = ALPHA_GRAVITY_MAX_ACCEL / accel_magnitude;
        ax *= accel_scale;
        ay *= accel_scale;
    }

    state->nav_vx = (state->nav_vx + ax) * ALPHA_GRAVITY_DAMPING;
    state->nav_vy = (state->nav_vy + ay) * ALPHA_GRAVITY_DAMPING;

    double speed = hypot(state->nav_vx, state->nav_vy);
    if (speed > ALPHA_NAV_MAX_SPEED && speed > 0.0) {
        double speed_scale = ALPHA_NAV_MAX_SPEED / speed;
        state->nav_vx *= speed_scale;
        state->nav_vy *= speed_scale;
    }

    state->nav_fx += state->nav_vx;
    state->nav_fy += state->nav_vy;

    double max_x = (double)(ALPHA_NAV_WIDTH - 1);
    double max_y = (double)(ALPHA_NAV_HEIGHT - 1);

    if (state->nav_fx < 0.0) {
        state->nav_fx = 0.0;
        state->nav_vx = 0.0;
    } else if (state->nav_fx > max_x) {
        state->nav_fx = max_x;
        state->nav_vx = 0.0;
    }

    if (state->nav_fy < 0.0) {
        state->nav_fy = 0.0;
        state->nav_vy = 0.0;
    } else if (state->nav_fy > max_y) {
        state->nav_fy = max_y;
        state->nav_vy = 0.0;
    }

    long rounded_x = lround(state->nav_fx);
    long rounded_y = lround(state->nav_fy);
    if (rounded_x < 0) {
        rounded_x = 0;
    } else if (rounded_x > (long)(ALPHA_NAV_WIDTH - 1)) {
        rounded_x = (long)(ALPHA_NAV_WIDTH - 1);
    }
    if (rounded_y < 0) {
        rounded_y = 0;
    } else if (rounded_y > (long)(ALPHA_NAV_HEIGHT - 1)) {
        rounded_y = (long)(ALPHA_NAV_HEIGHT - 1);
    }

    state->nav_x = (int)rounded_x;
    state->nav_y = (int)rounded_y;
}

static void session_game_alpha_prepare_navigation(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_ALPHA) {
        return;
    }

    alpha_centauri_game_state_t *state = &ctx->game.alpha;
    int safe_margin = ALPHA_NAV_MARGIN;

    unsigned stage_level = state->stage;
    if (stage_level > 0U) {
        int shrink = (int)stage_level;
        if (stage_level >= 2U) {
            shrink += 1;
        }
        if (stage_level >= 3U) {
            shrink += 1;
        }
        if (stage_level >= 4U) {
            shrink += (int)state->waypoint_index;
            if (state->awaiting_flag) {
                shrink += 2;
            }
        }
        safe_margin -= shrink;
        if (safe_margin < 2) {
            safe_margin = 2;
        }
    }

    state->nav_stable_ticks = 0U;
    state->nav_required_ticks = 1U;
    state->nav_vx = 0.0;
    state->nav_vy = 0.0;

    switch (state->stage) {
    case 0:
        state->nav_x = session_game_alpha_random_with_margin(
            ctx, ALPHA_NAV_WIDTH, safe_margin);
        state->nav_y = ALPHA_NAV_HEIGHT - 1 -
                       session_game_random_range(ctx, safe_margin + 4);
        if (state->nav_y < safe_margin) {
            state->nav_y = safe_margin;
        }
        state->nav_target_x = session_game_alpha_random_with_margin(
            ctx, ALPHA_NAV_WIDTH, safe_margin);
        state->nav_target_y = session_game_random_range(ctx, safe_margin + 4);
        state->nav_required_ticks = 1U;
        break;
    case 1:
        state->nav_x =
            session_game_random_range(ctx, (ALPHA_NAV_WIDTH / 2)) + safe_margin;
        if (state->nav_x >= ALPHA_NAV_WIDTH) {
            state->nav_x = ALPHA_NAV_WIDTH - 1;
        }
        state->nav_y = session_game_alpha_random_with_margin(
            ctx, ALPHA_NAV_HEIGHT, safe_margin);
        state->nav_target_x = ALPHA_NAV_WIDTH - 1 -
                              session_game_random_range(ctx, safe_margin + 5);
        state->nav_target_y = session_game_alpha_random_with_margin(
            ctx, ALPHA_NAV_HEIGHT, safe_margin);
        state->nav_required_ticks = 1U;
        break;
    case 2:
        state->nav_x = ALPHA_NAV_WIDTH - 1 -
                       session_game_random_range(ctx, safe_margin + 5);
        state->nav_y = session_game_alpha_random_with_margin(
            ctx, ALPHA_NAV_HEIGHT, safe_margin);
        state->nav_target_x = session_game_random_range(ctx, safe_margin + 5);
        state->nav_target_y = session_game_alpha_random_with_margin(
            ctx, ALPHA_NAV_HEIGHT, safe_margin);
        state->nav_required_ticks = 1U;
        break;
    case 3:
        state->nav_x = session_game_alpha_random_with_margin(
            ctx, ALPHA_NAV_WIDTH, safe_margin);
        state->nav_y = session_game_random_range(ctx, safe_margin + 5);
        state->nav_target_x = session_game_alpha_random_with_margin(
            ctx, ALPHA_NAV_WIDTH, safe_margin);
        state->nav_target_y = ALPHA_NAV_HEIGHT - 1 -
                              session_game_random_range(ctx, safe_margin + 5);
        state->nav_required_ticks = 1U;
        break;
    case 4:
        if (!state->eva_ready) {
            session_game_alpha_plan_waypoints(ctx);
            if (state->waypoint_count == 0U) {
                state->nav_target_x = session_game_alpha_random_with_margin(
                    ctx, ALPHA_NAV_WIDTH, safe_margin);
                state->nav_target_y =
                    session_game_random_range(ctx, safe_margin + 5);
            } else {
                if (state->waypoint_index >= state->waypoint_count) {
                    state->waypoint_index = state->waypoint_count - 1U;
                }
                const alpha_waypoint_t *waypoint =
                    &state->waypoints[state->waypoint_index];
                state->nav_target_x = waypoint->x;
                state->nav_target_y = waypoint->y;
            }
            state->nav_x = session_game_alpha_random_with_margin(
                ctx, ALPHA_NAV_WIDTH, safe_margin);
            state->nav_y = session_game_random_range(ctx, safe_margin + 5);
            state->nav_required_ticks = 1U;
        } else if (state->awaiting_flag) {
            if (state->final_waypoint.symbol == '\0') {
                session_game_alpha_plan_waypoints(ctx);
            }
            state->nav_x = session_game_alpha_random_with_margin(
                ctx, ALPHA_NAV_WIDTH, safe_margin);
            state->nav_y = session_game_alpha_random_with_margin(
                ctx, ALPHA_NAV_HEIGHT, safe_margin);
            state->nav_target_x = state->final_waypoint.x;
            state->nav_target_y = state->final_waypoint.y;
            state->nav_required_ticks = 1U;
        } else {
            state->nav_x = session_game_alpha_random_with_margin(
                ctx, ALPHA_NAV_WIDTH, safe_margin);
            state->nav_y = ALPHA_NAV_HEIGHT - 1 -
                           session_game_random_range(ctx, safe_margin + 3);
            state->nav_target_x = session_game_alpha_random_with_margin(
                ctx, ALPHA_NAV_WIDTH, safe_margin);
            state->nav_target_y =
                session_game_random_range(ctx, safe_margin + 3);
            state->nav_required_ticks = 1U;
        }
        break;
    default:
        state->nav_x = session_game_alpha_random_with_margin(
            ctx, ALPHA_NAV_WIDTH, safe_margin);
        state->nav_y = session_game_alpha_random_with_margin(
            ctx, ALPHA_NAV_HEIGHT, safe_margin);
        state->nav_target_x = session_game_alpha_random_with_margin(
            ctx, ALPHA_NAV_WIDTH, safe_margin);
        state->nav_target_y = session_game_alpha_random_with_margin(
            ctx, ALPHA_NAV_HEIGHT, safe_margin);
        break;
    }

    if (state->nav_target_x < 0) {
        state->nav_target_x = 0;
    } else if (state->nav_target_x >= ALPHA_NAV_WIDTH) {
        state->nav_target_x = ALPHA_NAV_WIDTH - 1;
    }
    if (state->nav_target_y < 0) {
        state->nav_target_y = 0;
    } else if (state->nav_target_y >= ALPHA_NAV_HEIGHT) {
        state->nav_target_y = ALPHA_NAV_HEIGHT - 1;
    }

    if (state->nav_x < 0) {
        state->nav_x = 0;
    } else if (state->nav_x >= ALPHA_NAV_WIDTH) {
        state->nav_x = ALPHA_NAV_WIDTH - 1;
    }
    if (state->nav_y < 0) {
        state->nav_y = 0;
    } else if (state->nav_y >= ALPHA_NAV_HEIGHT) {
        state->nav_y = ALPHA_NAV_HEIGHT - 1;
    }

    if (state->nav_x == state->nav_target_x &&
        state->nav_y == state->nav_target_y) {
        if (state->stage == 4U) {
            state->nav_x =
                (state->nav_target_x + ALPHA_NAV_MARGIN) % ALPHA_NAV_WIDTH;
            state->nav_y =
                (state->nav_target_y + ALPHA_NAV_MARGIN) % ALPHA_NAV_HEIGHT;
            state->nav_fx = (double)state->nav_x;
            state->nav_fy = (double)state->nav_y;
        } else {
            state->nav_target_x =
                (state->nav_target_x + (ALPHA_NAV_WIDTH / 2)) % ALPHA_NAV_WIDTH;
            state->nav_target_y =
                (state->nav_target_y + (ALPHA_NAV_HEIGHT / 2)) %
                ALPHA_NAV_HEIGHT;
        }
    }

    state->nav_fx = (double)state->nav_x;
    state->nav_fy = (double)state->nav_y;

    session_game_alpha_configure_gravity(ctx);
}

static void session_game_alpha_reroll_navigation(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_ALPHA ||
        !ctx->game.active) {
        return;
    }

    session_send_system_line(
        ctx, "Mission control: Recomputing the navigation solution...");
    session_game_alpha_prepare_navigation(ctx);
    session_game_alpha_sync_to_save(ctx);
    session_game_alpha_present_stage(ctx);
}

static void session_game_alpha_reset(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    alpha_centauri_game_state_t *state = &ctx->game.alpha;
    *state = (alpha_centauri_game_state_t){0};
    state->stage = 0U;
    state->velocity_fraction_c = 0.0;
    state->distance_travelled_ly = 0.0;
    state->distance_remaining_ly = ALPHA_TOTAL_DISTANCE_LY;
    state->fuel_percent = 100.0;
    state->oxygen_days = 730.0;
    state->mission_time_years = 0.0;
    state->radiation_msv = 0.0;
    state->active = false;
    state->eva_ready = false;
    state->awaiting_flag = false;
    session_game_alpha_prepare_navigation(ctx);
}

static void session_game_alpha_sync_from_save(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    alpha_centauri_game_state_t *state = &ctx->game.alpha;
    session_game_alpha_reset(ctx);

    if (!session_user_data_load(ctx)) {
        return;
    }

    const alpha_centauri_save_t *save = &ctx->user_data.alpha;
    if (!save->active) {
        return;
    }

    state->active = true;
    state->stage = save->stage <= 4U ? save->stage : 0U;
    state->eva_ready = save->eva_ready != 0U;
    state->awaiting_flag = save->awaiting_flag != 0U;
    state->velocity_fraction_c = save->velocity_fraction_c;
    state->distance_travelled_ly = save->distance_travelled_ly;
    state->distance_remaining_ly = save->distance_remaining_ly;
    if (state->distance_remaining_ly < 0.0) {
        state->distance_remaining_ly = 0.0;
    }
    state->fuel_percent = save->fuel_percent;
    state->oxygen_days = save->oxygen_days;
    state->mission_time_years = save->mission_time_years;
    state->radiation_msv = save->radiation_msv;
    session_game_alpha_prepare_navigation(ctx);
}

static void session_game_alpha_sync_to_save(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    if (!session_user_data_load(ctx)) {
        return;
    }

    alpha_centauri_save_t *save = &ctx->user_data.alpha;
    const alpha_centauri_game_state_t *state = &ctx->game.alpha;
    save->active = state->active ? 1U : 0U;
    save->stage = (uint8_t)(state->stage <= 4U ? state->stage : 0U);
    save->eva_ready = state->eva_ready ? 1U : 0U;
    save->awaiting_flag = state->awaiting_flag ? 1U : 0U;
    save->velocity_fraction_c = state->velocity_fraction_c;
    save->distance_travelled_ly = state->distance_travelled_ly;
    save->distance_remaining_ly = state->distance_remaining_ly;
    save->fuel_percent = state->fuel_percent;
    save->oxygen_days = state->oxygen_days;
    save->mission_time_years = state->mission_time_years;
    save->radiation_msv = state->radiation_msv;
    session_user_data_commit(ctx);
}

static void session_game_alpha_report_state(session_ctx_t *ctx,
                                            const char *label)
{
    if (ctx == nullptr) {
        return;
    }

    const alpha_centauri_game_state_t *state = &ctx->game.alpha;
    bool previous_translation = ctx->translation_suppress_output;
    ctx->translation_suppress_output = true;

    if (label != nullptr && label[0] != '\0') {
        session_send_system_line(ctx, label);
    }

    double velocity_kms =
        state->velocity_fraction_c * ALPHA_SPEED_OF_LIGHT_MPS / 1000.0;
    double distance_au = state->distance_remaining_ly * ALPHA_LY_TO_AU;

    char line[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(
        line, sizeof(line),
        "Velocity: %.2f%% c (%.0f km/s) | Fuel %.1f%% | Radiation %.1f mSv",
        state->velocity_fraction_c * 100.0, velocity_kms, state->fuel_percent,
        state->radiation_msv);
    session_send_system_line(ctx, line);

    snprintf(line, sizeof(line),
             "Distance remaining: %.2f ly (%.0f AU) | Oxygen %.0f days | "
             "Mission clock %.2f years",
             state->distance_remaining_ly, distance_au, state->oxygen_days,
             state->mission_time_years);
    session_send_system_line(ctx, line);

    ctx->translation_suppress_output = previous_translation;
}

static const char *
session_game_alpha_phase_label(const alpha_centauri_game_state_t *state)
{
    if (state == nullptr) {
        return "Guidance";
    }

    switch (state->stage) {
    case 0:
        return "Launch corridor beacon";
    case 1:
        return "Barycenter alignment";
    case 2:
        return "Turnover marker";
    case 3:
        return "Retro burn beacon";
    case 4:
        if (!state->eva_ready) {
            return "Deorbit corridor";
        }
        if (state->awaiting_flag) {
            return "Landing beacon";
        }
        return "Orbit standby";
    default:
        break;
    }
    return "Guidance";
}

static void session_game_alpha_render_navigation(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_ALPHA ||
        !ctx->game.active) {
        return;
    }

    const alpha_centauri_game_state_t *state = &ctx->game.alpha;
    const char *phase_label = session_game_alpha_phase_label(state);

    char header[SSH_CHATTER_MESSAGE_LIMIT];
    bool contact = (state->nav_x == state->nav_target_x &&
                    state->nav_y == state->nav_target_y);
    const char *status =
        contact ? "beacon contact achieved" : "tracking beacon";
    snprintf(header, sizeof(header),
             "Guidance: %s (%s - reach '+' to advance automatically)",
             phase_label, status);

    char border[ALPHA_NAV_WIDTH + 3];
    border[0] = '+';
    for (int idx = 0; idx < ALPHA_NAV_WIDTH; ++idx) {
        border[idx + 1] = '-';
    }
    border[ALPHA_NAV_WIDTH + 1] = '+';
    border[ALPHA_NAV_WIDTH + 2] = '\0';
    session_send_system_line(ctx, border);

    for (int y = 0; y < ALPHA_NAV_HEIGHT; ++y) {
        char row[ALPHA_NAV_WIDTH + 1];
        for (int x = 0; x < ALPHA_NAV_WIDTH; ++x) {
            row[x] = '.';
        }

        for (unsigned idx = 0U; idx < state->gravity_source_count; ++idx) {
            const alpha_gravity_source_t *source = &state->gravity_sources[idx];
            if (source->x >= 0 && source->x < ALPHA_NAV_WIDTH &&
                source->y == y) {
                char symbol = source->symbol != '\0' ? source->symbol : 'G';
                row[source->x] = symbol;
            }
        }

        if (state->nav_target_x >= 0 && state->nav_target_x < ALPHA_NAV_WIDTH &&
            state->nav_target_y >= 0 &&
            state->nav_target_y < ALPHA_NAV_HEIGHT &&
            y == state->nav_target_y) {
            row[state->nav_target_x] = '+';
        }

        if (state->nav_x >= 0 && state->nav_x < ALPHA_NAV_WIDTH &&
            state->nav_y >= 0 && state->nav_y < ALPHA_NAV_HEIGHT &&
            y == state->nav_y) {
            if (state->nav_target_x == state->nav_x &&
                state->nav_target_y == state->nav_y) {
                row[state->nav_x] = '*';
            } else {
                row[state->nav_x] = '@';
            }
        }

        char line[ALPHA_NAV_WIDTH + 4];
        line[0] = '|';
        for (int x = 0; x < ALPHA_NAV_WIDTH; ++x) {
            line[x + 1] = row[x];
        }
        line[ALPHA_NAV_WIDTH + 1] = '|';
        line[ALPHA_NAV_WIDTH + 2] = '\0';
        session_send_system_line(ctx, line);
    }

    session_send_system_line(ctx, border);
    session_send_system_line(ctx, header);

    if (state->gravity_source_count > 0U) {
        char gravity_line[SSH_CHATTER_MESSAGE_LIMIT];
        int written =
            snprintf(gravity_line, sizeof(gravity_line), "Gravity wells: ");
        size_t offset = 0U;
        if (written >= 0) {
            offset = (size_t)written;
            if (offset >= sizeof(gravity_line)) {
                offset = sizeof(gravity_line) - 1U;
            }
        } else {
            gravity_line[0] = '\0';
        }

        for (unsigned idx = 0U; idx < state->gravity_source_count &&
                                offset < sizeof(gravity_line) - 1U;
             ++idx) {
            const alpha_gravity_source_t *source = &state->gravity_sources[idx];
            const char *name =
                source->name[0] != '\0' ? source->name : "Gravity Source";
            char symbol = source->symbol != '\0' ? source->symbol : 'G';
            written = snprintf(gravity_line + offset,
                               sizeof(gravity_line) - offset, "%s%c=%s(μ=%.2e)",
                               idx == 0U ? "" : ", ", symbol, name, source->mu);
            if (written < 0) {
                break;
            }
            if ((size_t)written >= sizeof(gravity_line) - offset) {
                offset = sizeof(gravity_line) - 1U;
                break;
            }
            offset += (size_t)written;
        }

        gravity_line[sizeof(gravity_line) - 1U] = '\0';
        session_send_system_line(ctx, gravity_line);
    }
}

static void session_game_alpha_refresh_navigation(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_ALPHA ||
        !ctx->game.active) {
        return;
    }

    bool previous_translation = ctx->translation_suppress_output;
    ctx->translation_suppress_output = true;
    session_game_alpha_render_navigation(ctx);
    session_game_alpha_report_state(ctx, "Current status:");
    ctx->translation_suppress_output = previous_translation;
}

static void session_game_alpha_plan_waypoints(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_ALPHA) {
        return;
    }

    alpha_centauri_game_state_t *state = &ctx->game.alpha;
    if (state->stage != 4U) {
        state->waypoint_count = 0U;
        state->waypoint_index = 0U;
        state->final_waypoint = (alpha_waypoint_t){0};
        return;
    }

    size_t name_count =
        sizeof(kAlphaWaystationNames) / sizeof(kAlphaWaystationNames[0]);

    if (!state->eva_ready && state->waypoint_count == 0U) {
        unsigned desired = ALPHA_MIN_WAYPOINTS;
        if (desired > ALPHA_MAX_WAYPOINTS) {
            desired = ALPHA_MAX_WAYPOINTS;
        }
        state->waypoint_count = desired;
        state->waypoint_index = 0U;

        for (unsigned idx = 0U; idx < state->waypoint_count; ++idx) {
            int x = 0;
            int y = 0;
            bool placed = false;
            for (unsigned attempt = 0U; attempt < 96U && !placed; ++attempt) {
                x = session_game_alpha_random_with_margin(ctx, ALPHA_NAV_WIDTH,
                                                          ALPHA_NAV_MARGIN);
                y = session_game_alpha_random_with_margin(ctx, ALPHA_NAV_HEIGHT,
                                                          ALPHA_NAV_MARGIN);
                bool conflict = false;
                for (unsigned prior = 0U; prior < idx; ++prior) {
                    const alpha_waypoint_t *existing = &state->waypoints[prior];
                    if (existing->x == x && existing->y == y) {
                        conflict = true;
                        break;
                    }
                }
                if (!conflict) {
                    placed = true;
                }
            }
            if (!placed) {
                x = session_game_alpha_random_with_margin(ctx, ALPHA_NAV_WIDTH,
                                                          ALPHA_NAV_MARGIN);
                y = session_game_alpha_random_with_margin(ctx, ALPHA_NAV_HEIGHT,
                                                          ALPHA_NAV_MARGIN);
            }

            alpha_waypoint_t *waypoint = &state->waypoints[idx];
            waypoint->x = x;
            waypoint->y = y;
            waypoint->symbol = (char)('1' + (idx % 9));
            waypoint->visited = false;
            const char *name = name_count > 0
                                   ? kAlphaWaystationNames[idx % name_count]
                                   : "Waystation";
            snprintf(waypoint->name, sizeof(waypoint->name), "%s", name);
        }
    } else if (!state->eva_ready) {
        for (unsigned idx = 0U; idx < state->waypoint_count; ++idx) {
            alpha_waypoint_t *waypoint = &state->waypoints[idx];
            if (waypoint->symbol == '\0') {
                waypoint->symbol = (char)('1' + (idx % 9));
            }
        }
    }

    if (state->final_waypoint.symbol == '\0') {
        int x = session_game_alpha_random_with_margin(ctx, ALPHA_NAV_WIDTH,
                                                      ALPHA_NAV_MARGIN);
        int y = ALPHA_NAV_HEIGHT - 1 -
                session_game_random_range(ctx, ALPHA_NAV_MARGIN + 4);
        for (unsigned attempt = 0U; attempt < 96U; ++attempt) {
            bool conflict = false;
            for (unsigned idx = 0U; idx < state->waypoint_count; ++idx) {
                const alpha_waypoint_t *waypoint = &state->waypoints[idx];
                if (waypoint->x == x && waypoint->y == y) {
                    conflict = true;
                    break;
                }
            }
            if (!conflict) {
                break;
            }
            x = session_game_alpha_random_with_margin(ctx, ALPHA_NAV_WIDTH,
                                                      ALPHA_NAV_MARGIN);
            y = ALPHA_NAV_HEIGHT - 1 -
                session_game_random_range(ctx, ALPHA_NAV_MARGIN + 4);
        }

        state->final_waypoint.x = x;
        state->final_waypoint.y = y;
        state->final_waypoint.symbol = 'P';
        state->final_waypoint.visited = false;
        snprintf(state->final_waypoint.name, sizeof(state->final_waypoint.name),
                 "%s", "Proxima Landing");
    }
}

static void session_game_alpha_present_waypoints(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_ALPHA) {
        return;
    }

    alpha_centauri_game_state_t *state = &ctx->game.alpha;
    if (state->stage != 4U) {
        return;
    }

    if (!state->eva_ready) {
        if (state->waypoint_count == 0U) {
            session_send_system_line(ctx,
                                     "Waystation manifest pending - reroll if "
                                     "the corridor looks blocked.");
            return;
        }

        session_send_system_line(ctx, "Waystation manifest:");
        for (unsigned idx = 0U; idx < state->waypoint_count; ++idx) {
            const alpha_waypoint_t *waypoint = &state->waypoints[idx];
            char line[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(
                line, sizeof(line), "  [%c] %c - %s%s",
                waypoint->visited ? 'x' : ' ', waypoint->symbol, waypoint->name,
                idx == state->waypoint_index ? " ← current objective" : "");
            session_send_system_line(ctx, line);
        }

        char landing[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(
            landing, sizeof(landing),
            "Final descent: P - %s unlocks after the last waystation. Touch "
            "down to finish or press Alt+L if you"
            " prefer a manual confirmation.",
            state->final_waypoint.name[0] != '\0' ? state->final_waypoint.name
                                                  : "Proxima Landing");
        session_send_system_line(ctx, landing);
        return;
    }

    if (state->awaiting_flag) {
        char landing[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(landing, sizeof(landing),
                 "Final target: P - %s. Touch down to plant automatically, or "
                 "press Alt+L/type 'plant flag' to finish.",
                 state->final_waypoint.name[0] != '\0'
                     ? state->final_waypoint.name
                     : "Proxima Landing");
        session_send_system_line(ctx, landing);
    }
}

static void session_game_alpha_complete_waypoint(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_ALPHA) {
        return;
    }

    alpha_centauri_game_state_t *state = &ctx->game.alpha;
    if (state->stage != 4U || state->eva_ready) {
        return;
    }

    if (state->waypoint_index >= state->waypoint_count) {
        session_game_alpha_execute_eva(ctx);
        return;
    }

    alpha_waypoint_t *current = &state->waypoints[state->waypoint_index];
    current->visited = true;
    ++state->waypoint_index;
    state->nav_stable_ticks = 0U;

    if (state->waypoint_index >= state->waypoint_count) {
        session_send_system_line(
            ctx, "Waystations secured. Setting the descent beacon...");
        state->waypoint_index = state->waypoint_count;
        session_game_alpha_execute_eva(ctx);
        return;
    }

    const alpha_waypoint_t *next = &state->waypoints[state->waypoint_index];
    state->nav_target_x = next->x;
    state->nav_target_y = next->y;
    state->nav_required_ticks = 1U;
    state->nav_fx = (double)state->nav_x;
    state->nav_fy = (double)state->nav_y;
    state->nav_vx = 0.0;
    state->nav_vy = 0.0;

    session_game_alpha_configure_gravity(ctx);

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(message, sizeof(message), "Next stop %u/%u - marker %c (%s).",
             state->waypoint_index + 1U, state->waypoint_count, next->symbol,
             next->name);
    session_send_system_line(ctx, message);
    session_game_alpha_refresh_navigation(ctx);
}

static void session_game_alpha_present_stage(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_ALPHA ||
        !ctx->game.active) {
        return;
    }

    alpha_centauri_game_state_t *state = &ctx->game.alpha;
    bool previous_translation = ctx->translation_suppress_output;
    ctx->translation_suppress_output = true;

    session_render_separator(ctx, "Alpha Centauri Expedition");
    session_game_alpha_render_navigation(ctx);

    char stage_line[SSH_CHATTER_MESSAGE_LIMIT];
    switch (state->stage) {
    case 0:
        snprintf(
            stage_line, sizeof(stage_line),
            "Stage 0 - Launch stack ready. Ride the ascent beacon; contact "
            "ignites the antimatter booster automatically.");
        session_send_system_line(ctx, stage_line);
        break;
    case 1:
        snprintf(
            stage_line, sizeof(stage_line),
            "Stage 1 - Mid-course trim. Touch the barycenter beacon to bank "
            "the correction burn; manual lock is optional.");
        session_send_system_line(ctx, stage_line);
        break;
    case 2:
        snprintf(stage_line, sizeof(stage_line),
                 "Stage 2 - Turnover. Settle on the retrograde marker to flip "
                 "into braking attitude automatically.");
        session_send_system_line(ctx, stage_line);
        break;
    case 3:
        snprintf(stage_line, sizeof(stage_line),
                 "Stage 3 - Braking burn. Drop onto the braking beacon and the "
                 "burn locks the moment you make contact.");
        session_send_system_line(ctx, stage_line);
        break;
    case 4:
        if (!state->eva_ready) {
            unsigned remaining = 0U;
            if (state->waypoint_count > state->waypoint_index) {
                remaining = state->waypoint_count - state->waypoint_index;
            }
            snprintf(stage_line, sizeof(stage_line),
                     "Stage 4 - High orbit over Proxima b. Visit the numbered "
                     "waystations; each beacon contact auto-logs"
                     " the stop. %u stop(s) remain before descent.",
                     remaining);
            session_send_system_line(ctx, stage_line);
        } else if (state->awaiting_flag) {
            snprintf(stage_line, sizeof(stage_line),
                     "Stage 4 - Surface EVA. Touch marker %c (%s) to plant "
                     "\"Immigrants' "
                     "Flag\" automatically, or press"
                     " Alt+L/type 'plant flag' for manual confirmation.",
                     state->final_waypoint.symbol != '\0'
                         ? state->final_waypoint.symbol
                         : 'P',
                     state->final_waypoint.name[0] != '\0'
                         ? state->final_waypoint.name
                         : "Proxima Landing");
            session_send_system_line(ctx, stage_line);
        } else {
            session_send_system_line(
                ctx,
                "Stage 4 - Mission reset. Realign with the beacons for another "
                "run or exit with /suspend!.");
        }
        session_game_alpha_present_waypoints(ctx);
        break;
    default:
        session_send_system_line(ctx, "Awaiting next burn sequence.");
        break;
    }

    if (state->stage == 4U) {
        session_send_system_line(
            ctx, "Route markers: 1–9 mark required waystations; P "
                 "marks the Proxima landing zone.");
        session_send_system_line(ctx,
                                 "Gravitational pulls: B=black hole, S=star, "
                                 "D=debris - each mass tugs with its own μ.");
    } else {
        session_send_system_line(
            ctx,
            "Gravitational pulls: B=black hole, S=star, P=planet, D=debris - "
            "each mass tugs with its own μ.");
    }
    if (state->stage == 4U) {
        session_send_system_line(ctx,
                                 "Legend: @ craft, + beacon, * beacon contact, "
                                 "digits=waystations, P final landing, B black"
                                 " hole, S star, D debris.");
    } else {
        session_send_system_line(
            ctx, "Legend: @ craft, + beacon, * beacon contact, B "
                 "black hole, S star, P planet, D debris.");
    }
    session_send_system_line(ctx, "Navigation grid spans 60×60 sectors; each "
                                  "maneuver reshuffles the gravity field.");
    session_send_system_line(ctx, "Use arrow keys to nudge the craft; touching "
                                  "the beacon advances immediately.");
    session_send_system_line(ctx, "Alt+L records a manual confirmation; press "
                                  "Ctrl+S anytime to save the mission log.");
    session_send_system_line(ctx,
                             "Each stage intensifies the gravity field, so "
                             "later maneuvers demand tighter control.");
    session_send_system_line(ctx,
                             "Stuck? Type 'reset' to reroll the field with "
                             "a fresh gravimetric solution.");
    session_game_alpha_report_state(ctx, "Current status:");
    ctx->translation_suppress_output = previous_translation;
}

static void session_game_alpha_log_completion(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    alpha_centauri_game_state_t *state = &ctx->game.alpha;
    state->velocity_fraction_c = 0.0;
    state->distance_travelled_ly = ALPHA_TOTAL_DISTANCE_LY;
    state->distance_remaining_ly = 0.0;
    if (state->fuel_percent > 5.0) {
        state->fuel_percent = 5.0;
    }
    if (state->oxygen_days > 20.0) {
        state->oxygen_days -= 20.0;
    } else {
        state->oxygen_days = 0.0;
    }
    state->mission_time_years += 0.05;
    state->radiation_msv += 5.0;
    state->eva_ready = true;
    state->awaiting_flag = false;

    double total_years = state->mission_time_years;
    double total_radiation = state->radiation_msv;

    time_t now = time(nullptr);
    uint64_t landing_timestamp = 0U;
    if (now != (time_t)-1) {
        landing_timestamp = (uint64_t)now;
    }
    bool recorded = false;
    uint32_t updated_flag_count = 0U;

    if (session_user_data_load(ctx)) {
        ctx->user_data.flag_count += 1U;
        uint64_t timestamp = landing_timestamp;
        if (ctx->user_data.flag_history_count < USER_DATA_FLAG_HISTORY_LIMIT) {
            ctx->user_data.flag_history[ctx->user_data.flag_history_count++] =
                timestamp;
        } else {
            for (size_t idx = 1U; idx < USER_DATA_FLAG_HISTORY_LIMIT; ++idx) {
                ctx->user_data.flag_history[idx - 1U] =
                    ctx->user_data.flag_history[idx];
            }
            ctx->user_data.flag_history[USER_DATA_FLAG_HISTORY_LIMIT - 1U] =
                timestamp;
        }
        recorded = true;
        updated_flag_count = ctx->user_data.flag_count;
    }

    bool previous_translation = ctx->translation_suppress_output;
    ctx->translation_suppress_output = true;

    char success[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(success, sizeof(success),
             "Mission complete! \"Immigrants' Flag\" is registered for %s. "
             "Flight time %.2f years, exposure %.1f mSv.",
             ctx->user.name, total_years, total_radiation);
    session_send_system_line(ctx, success);

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice),
             "* [alpha-centauri] Immigrants' Flag planted by %s.",
             ctx->user.name);
    host_history_record_system(ctx->owner, notice, nullptr);
    chat_room_broadcast(&ctx->owner->room, notice, nullptr);

    ctx->translation_suppress_output = previous_translation;

    if (recorded) {
        host_alpha_landers_record(ctx->owner, ctx->user.name,
                                  updated_flag_count, landing_timestamp);
    }

    session_game_alpha_reset(ctx);
    state->active = true;
    session_game_alpha_sync_to_save(ctx);
    session_game_alpha_present_stage(ctx);
}

static void session_game_alpha_execute_ignite(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_ALPHA ||
        ctx->game.alpha.stage != 0U) {
        return;
    }

    alpha_centauri_game_state_t *state = &ctx->game.alpha;
    state->stage = 1U;
    state->active = true;
    state->velocity_fraction_c = 0.04;
    state->distance_travelled_ly = 0.05;
    state->distance_remaining_ly =
        ALPHA_TOTAL_DISTANCE_LY - state->distance_travelled_ly;
    state->fuel_percent = 82.0;
    if (state->oxygen_days > 10.0) {
        state->oxygen_days -= 10.0;
    }
    state->mission_time_years += 0.02;
    state->radiation_msv += 12.0;
    session_game_alpha_prepare_navigation(ctx);
    session_game_alpha_sync_to_save(ctx);
    session_game_alpha_present_stage(ctx);
}

static void session_game_alpha_execute_trim(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_ALPHA ||
        ctx->game.alpha.stage != 1U) {
        return;
    }

    alpha_centauri_game_state_t *state = &ctx->game.alpha;
    state->stage = 2U;
    state->velocity_fraction_c = 0.18;
    state->distance_travelled_ly = 1.90;
    state->distance_remaining_ly =
        ALPHA_TOTAL_DISTANCE_LY - state->distance_travelled_ly;
    state->fuel_percent = 58.0;
    if (state->oxygen_days > 110.0) {
        state->oxygen_days -= 110.0;
    } else {
        state->oxygen_days = 0.0;
    }
    state->mission_time_years += 0.55;
    state->radiation_msv += 28.0;
    session_game_alpha_prepare_navigation(ctx);
    session_game_alpha_sync_to_save(ctx);
    session_game_alpha_present_stage(ctx);
}

static void session_game_alpha_execute_flip(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_ALPHA ||
        ctx->game.alpha.stage != 2U) {
        return;
    }

    alpha_centauri_game_state_t *state = &ctx->game.alpha;
    state->stage = 3U;
    state->distance_travelled_ly = 3.60;
    state->distance_remaining_ly =
        ALPHA_TOTAL_DISTANCE_LY - state->distance_travelled_ly;
    state->fuel_percent = 45.0;
    if (state->oxygen_days > 220.0) {
        state->oxygen_days -= 220.0;
    } else {
        state->oxygen_days = 0.0;
    }
    state->mission_time_years += 1.80;
    state->radiation_msv += 18.0;
    session_game_alpha_prepare_navigation(ctx);
    session_game_alpha_sync_to_save(ctx);
    session_game_alpha_present_stage(ctx);
}

static void session_game_alpha_execute_retro(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_ALPHA ||
        ctx->game.alpha.stage != 3U) {
        return;
    }

    alpha_centauri_game_state_t *state = &ctx->game.alpha;
    state->stage = 4U;
    state->velocity_fraction_c = 0.01;
    state->distance_travelled_ly = 4.22;
    state->distance_remaining_ly =
        ALPHA_TOTAL_DISTANCE_LY - state->distance_travelled_ly;
    state->fuel_percent = 18.0;
    if (state->oxygen_days > 150.0) {
        state->oxygen_days -= 150.0;
    } else {
        state->oxygen_days = 0.0;
    }
    state->mission_time_years += 1.20;
    state->radiation_msv += 12.0;
    state->eva_ready = false;
    state->awaiting_flag = false;
    state->waypoint_index = 0U;
    state->waypoint_count = 0U;
    state->final_waypoint = (alpha_waypoint_t){0};
    session_game_alpha_plan_waypoints(ctx);
    session_game_alpha_prepare_navigation(ctx);
    session_game_alpha_sync_to_save(ctx);
    session_game_alpha_present_stage(ctx);
}

static void session_game_alpha_execute_eva(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_ALPHA ||
        ctx->game.alpha.stage != 4U || ctx->game.alpha.eva_ready) {
        return;
    }

    alpha_centauri_game_state_t *state = &ctx->game.alpha;
    state->eva_ready = true;
    state->awaiting_flag = true;
    state->waypoint_index = state->waypoint_count;
    if (state->oxygen_days > 30.0) {
        state->oxygen_days -= 30.0;
    } else {
        state->oxygen_days = 0.0;
    }
    state->mission_time_years += 0.05;
    state->radiation_msv += 6.0;
    session_game_alpha_prepare_navigation(ctx);
    session_game_alpha_sync_to_save(ctx);
    session_game_alpha_present_stage(ctx);
}

static bool session_game_alpha_attempt_completion(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_ALPHA) {
        return false;
    }

    alpha_centauri_game_state_t *state = &ctx->game.alpha;
    if (state->nav_x != state->nav_target_x ||
        state->nav_y != state->nav_target_y) {
        return false;
    }

    if (state->stage == 0U) {
        session_game_alpha_execute_ignite(ctx);
        return true;
    }
    if (state->stage == 1U) {
        session_game_alpha_execute_trim(ctx);
        return true;
    }
    if (state->stage == 2U) {
        session_game_alpha_execute_flip(ctx);
        return true;
    }
    if (state->stage == 3U) {
        session_game_alpha_execute_retro(ctx);
        return true;
    }
    if (state->stage == 4U) {
        if (!state->eva_ready) {
            session_game_alpha_complete_waypoint(ctx);
            return true;
        }
        if (state->awaiting_flag) {
            state->final_waypoint.visited = true;
            session_game_alpha_log_completion(ctx);
            return true;
        }
    }

    return false;
}

static void session_game_alpha_manual_lock(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_ALPHA ||
        !ctx->game.active) {
        return;
    }

    alpha_centauri_game_state_t *state = &ctx->game.alpha;
    if (state->nav_x != state->nav_target_x ||
        state->nav_y != state->nav_target_y) {
        session_send_system_line(
            ctx, "Lock failed: align with the beacon before pressing Alt+L.");
        session_game_alpha_refresh_navigation(ctx);
        return;
    }

    if (!session_game_alpha_attempt_completion(ctx)) {
        session_send_system_line(
            ctx, "Beacon contact logged; mission control is standing by.");
        session_game_alpha_refresh_navigation(ctx);
    }
}

static void session_game_alpha_manual_save(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_ALPHA) {
        return;
    }

    session_game_alpha_sync_to_save(ctx);
    session_send_system_line(ctx,
                             "Mission log saved. Touch the beacon to "
                             "advance or press Alt+L to confirm manually.");
}

static bool session_game_alpha_handle_arrow(session_ctx_t *ctx, int dx, int dy)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_ALPHA ||
        !ctx->game.active) {
        return false;
    }

    if (dx == 0 && dy == 0) {
        return false;
    }

    alpha_centauri_game_state_t *state = &ctx->game.alpha;

    state->nav_vx += (double)dx * ALPHA_THRUST_DELTA;
    state->nav_vy += (double)dy * ALPHA_THRUST_DELTA;
    state->nav_fx += (double)dx * ALPHA_THRUST_POSITION_STEP;
    state->nav_fy += (double)dy * ALPHA_THRUST_POSITION_STEP;

    double max_x = (double)(ALPHA_NAV_WIDTH - 1);
    double max_y = (double)(ALPHA_NAV_HEIGHT - 1);
    if (state->nav_fx < 0.0) {
        state->nav_fx = 0.0;
        state->nav_vx = 0.0;
    } else if (state->nav_fx > max_x) {
        state->nav_fx = max_x;
        state->nav_vx = 0.0;
    }
    if (state->nav_fy < 0.0) {
        state->nav_fy = 0.0;
        state->nav_vy = 0.0;
    } else if (state->nav_fy > max_y) {
        state->nav_fy = max_y;
        state->nav_vy = 0.0;
    }

    state->nav_x = (int)lround(state->nav_fx);
    state->nav_y = (int)lround(state->nav_fy);
    if (state->nav_x < 0) {
        state->nav_x = 0;
    } else if (state->nav_x >= ALPHA_NAV_WIDTH) {
        state->nav_x = ALPHA_NAV_WIDTH - 1;
    }
    if (state->nav_y < 0) {
        state->nav_y = 0;
    } else if (state->nav_y >= ALPHA_NAV_HEIGHT) {
        state->nav_y = ALPHA_NAV_HEIGHT - 1;
    }

    session_game_alpha_apply_gravity(state);

    if (state->nav_x == state->nav_target_x &&
        state->nav_y == state->nav_target_y) {
        state->nav_stable_ticks = 1U;
    } else {
        state->nav_stable_ticks = 0U;
    }

    bool completed = session_game_alpha_attempt_completion(ctx);
    if (!completed) {
        session_game_alpha_refresh_navigation(ctx);
    }

    return true;
}

static void session_game_alpha_handle_line(session_ctx_t *ctx, const char *line)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_ALPHA ||
        !ctx->game.active) {
        return;
    }

    alpha_centauri_game_state_t *state = &ctx->game.alpha;
    char command[SSH_CHATTER_MAX_INPUT_LEN];
    if (line == nullptr) {
        command[0] = '\0';
    } else {
        snprintf(command, sizeof(command), "%s", line);
    }
    trim_whitespace_inplace(command);

    if (command[0] == '\0') {
        session_game_alpha_refresh_navigation(ctx);
        return;
    }

    // Handle camouflage toggle with 't' command
    if (strcasecmp(command, "t") == 0) {
        if (ctx->game.is_camouflaged) {
            ctx->game.is_camouflaged = false;
            ctx->game.saved_alpha_state = ctx->game.alpha;
            session_game_alpha_refresh_navigation(ctx);
        } else {
            ctx->game.is_camouflaged = true;
            ctx->game.saved_alpha_state = ctx->game.alpha;
            session_game_show_camouflage(ctx);
        }
        return;
    }

    if (strcasecmp(command, "lock") == 0 ||
        strcasecmp(command, "align lock") == 0) {
        session_game_alpha_manual_lock(ctx);
        return;
    }

    if (strcasecmp(command, "save") == 0 || strcasecmp(command, "log") == 0) {
        session_game_alpha_manual_save(ctx);
        return;
    }

    if (strcasecmp(command, "reset") == 0 ||
        strcasecmp(command, "reroll") == 0 ||
        strcasecmp(command, "rescan") == 0) {
        session_game_alpha_reroll_navigation(ctx);
        return;
    }

    if (state->stage == 0U) {
        if (strcasecmp(command, "ignite") == 0 ||
            strcasecmp(command, "launch") == 0) {
            session_game_alpha_execute_ignite(ctx);
        } else {
            session_send_system_line(ctx, "Line up with the ascent beacon "
                                          "using arrow keys or type 'ignite'.");
            session_game_alpha_refresh_navigation(ctx);
        }
        return;
    }

    if (state->stage == 1U) {
        if (strcasecmp(command, "trim") == 0 ||
            strcasecmp(command, "align") == 0) {
            session_game_alpha_execute_trim(ctx);
        } else {
            session_send_system_line(
                ctx,
                "Touch the barycenter beacon with arrow keys or type 'trim'.");
            session_game_alpha_refresh_navigation(ctx);
        }
        return;
    }

    if (state->stage == 2U) {
        if (strcasecmp(command, "flip") == 0 ||
            strcasecmp(command, "turnover") == 0) {
            session_game_alpha_execute_flip(ctx);
        } else {
            session_send_system_line(ctx,
                                     "Rotate into retrograde by touching the "
                                     "marker with arrow keys or type 'flip'.");
            session_game_alpha_refresh_navigation(ctx);
        }
        return;
    }

    if (state->stage == 3U) {
        if (strcasecmp(command, "retro") == 0 ||
            strcasecmp(command, "brake") == 0) {
            session_game_alpha_execute_retro(ctx);
        } else {
            session_send_system_line(ctx, "Drop onto the braking beacon with "
                                          "arrow keys or type 'retro'.");
            session_game_alpha_refresh_navigation(ctx);
        }
        return;
    }

    if (state->stage == 4U) {
        if (!state->eva_ready) {
            if (state->waypoint_index < state->waypoint_count) {
                const alpha_waypoint_t *target =
                    &state->waypoints[state->waypoint_index];
                char message[SSH_CHATTER_MESSAGE_LIMIT];
                snprintf(
                    message, sizeof(message),
                    "Route checkpoint %u/%u - touch marker %c (%s) to proceed "
                    "automatically. Alt+L remains available for"
                    " manual control.",
                    state->waypoint_index + 1U, state->waypoint_count,
                    target->symbol, target->name);
                session_send_system_line(ctx, message);
                session_game_alpha_refresh_navigation(ctx);
            } else {
                session_send_system_line(
                    ctx, "Waystations cleared. Touch the descent "
                         "beacon or press Alt+L to trigger EVA.");
                session_game_alpha_refresh_navigation(ctx);
            }
        } else if (state->awaiting_flag) {
            if (strcasecmp(command, "plant") == 0 ||
                strcasecmp(command, "plant flag") == 0 ||
                strcasecmp(command, "flag") == 0) {
                session_game_alpha_log_completion(ctx);
            } else {
                char message[SSH_CHATTER_MESSAGE_LIMIT];
                snprintf(message, sizeof(message),
                         "Touch marker %c (%s) to finish automatically, or "
                         "press Alt+L/type "
                         "'plant flag' to plant manually.",
                         state->final_waypoint.symbol != '\0'
                             ? state->final_waypoint.symbol
                             : 'P',
                         state->final_waypoint.name[0] != '\0'
                             ? state->final_waypoint.name
                             : "Proxima Landing");
                session_send_system_line(ctx, message);
                session_game_alpha_refresh_navigation(ctx);
            }
        } else {
            session_send_system_line(
                ctx, "Launch again with 'ignite' or exit with /suspend!.");
            session_game_alpha_refresh_navigation(ctx);
        }
        return;
    }

    session_send_system_line(ctx, "Hold position for the next maneuver.");
    session_game_alpha_refresh_navigation(ctx);
}

static void session_game_start_alpha(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    if (!session_user_data_load(ctx)) {
        session_send_system_line(
            ctx, "Profile storage unavailable; cannot start the mission.");
        return;
    }

    // Ask for camouflage language first
    session_send_system_line(
        ctx,
        "CHOOSE YOUR LOCKSCREEN LANGUAGE TO HIDE THE SCREEN ON YOUR OFFICE! "
        "(c, cpp, java, go, js, ts, rust)");
    char language_choice[16];
    size_t length = 0U;
    while (length + 1U < sizeof(language_choice)) {
        char ch = '\0';
        const int read_result = session_transport_read(ctx, &ch, 1, -1);
        if (read_result <= 0) {
            return;
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

        language_choice[length++] = ch;
        session_local_echo_char(ctx, ch);
    }
    language_choice[length] = '\0';
    trim_whitespace_inplace(language_choice);
    for (size_t idx = 0U; language_choice[idx] != '\0'; ++idx) {
        language_choice[idx] =
            (char)tolower((unsigned char)language_choice[idx]);
    }

    if (language_choice[0] == '\0') {
        session_send_system_line(ctx, "No language chosen. Defaulting to C.");
        snprintf(ctx->game.chosen_camouflage_language,
                 sizeof(ctx->game.chosen_camouflage_language), "c");
    } else if (strcmp(language_choice, "c") == 0 ||
               strcmp(language_choice, "cpp") == 0 ||
               strcmp(language_choice, "java") == 0 ||
               strcmp(language_choice, "go") == 0 ||
               strcmp(language_choice, "js") == 0 ||
               strcmp(language_choice, "ts") == 0 ||
               strcmp(language_choice, "rust") == 0) {
        snprintf(ctx->game.chosen_camouflage_language,
                 sizeof(ctx->game.chosen_camouflage_language), "%s",
                 language_choice);
    } else {
        session_send_system_line(ctx, "Invalid language. Defaulting to C.");
        snprintf(ctx->game.chosen_camouflage_language,
                 sizeof(ctx->game.chosen_camouflage_language), "c");
    }

    session_game_alpha_sync_from_save(ctx);
    alpha_centauri_game_state_t *state = &ctx->game.alpha;
    ctx->game.type = SESSION_GAME_ALPHA;
    ctx->game.active = true;
    ctx->game.is_camouflaged = false;
    state->active = true;

    session_send_system_line(ctx, "");
    if (state->stage == 0U) {
        session_send_system_line(
            ctx,
            "Mission control: Alpha Centauri expedition primed. Complete each "
            "maneuver to reach Proxima b.");
    } else {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message),
                 "Mission control: Resuming expedition at stage %u.",
                 state->stage);
        session_send_system_line(ctx, message);
    }

    session_game_alpha_sync_to_save(ctx);
    session_game_alpha_present_stage(ctx);
}

// Gonu (Korean traditional board game) implementation
static void session_game_gonu_reset(gonu_game_state_t *state,
                                    gonu_variant_t variant)
{
    if (state == nullptr) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->variant = variant;
    state->placement_phase = true;
    state->movement_started = false;
    state->player_turn = true;
    state->player_pieces = 0U;
    state->ai_pieces = 0U;
    state->selected_row = -1;
    state->selected_col = -1;
    state->piece_selected = false;
    state->awaiting_variant_selection = false;
    state->awaiting_mode_selection = false;
    state->awaiting_difficulty_selection = false;
    state->difficulty_level = 5U; // Default to hardest
    state->multiplayer = false;
    state->awaiting_opponent = false;
    state->slot_index = -1;
    state->player_number = 0U;
}

typedef struct gonu_board_pattern {
    bool valid[GONU_BOARD_SIZE][GONU_BOARD_SIZE];
    uint8_t neighbors[GONU_BOARD_SIZE][GONU_BOARD_SIZE];
} gonu_board_pattern_t;

static gonu_board_pattern_t gonu_patterns[GONU_VARIANT_JANGGI_STAR + 1U];
static bool gonu_patterns_initialized = false;

enum {
    GONU_DIR_N = 1U << 0,
    GONU_DIR_NE = 1U << 1,
    GONU_DIR_E = 1U << 2,
    GONU_DIR_SE = 1U << 3,
    GONU_DIR_S = 1U << 4,
    GONU_DIR_SW = 1U << 5,
    GONU_DIR_W = 1U << 6,
    GONU_DIR_NW = 1U << 7,
};

static bool session_game_gonu_in_bounds(int row, int col)
{
    return row >= 0 && row < GONU_BOARD_SIZE && col >= 0 &&
           col < GONU_BOARD_SIZE;
}

static uint8_t session_game_gonu_dir_to_bit(int dr, int dc)
{
    if (dr == -1 && dc == 0) {
        return GONU_DIR_N;
    }
    if (dr == -1 && dc == 1) {
        return GONU_DIR_NE;
    }
    if (dr == 0 && dc == 1) {
        return GONU_DIR_E;
    }
    if (dr == 1 && dc == 1) {
        return GONU_DIR_SE;
    }
    if (dr == 1 && dc == 0) {
        return GONU_DIR_S;
    }
    if (dr == 1 && dc == -1) {
        return GONU_DIR_SW;
    }
    if (dr == 0 && dc == -1) {
        return GONU_DIR_W;
    }
    if (dr == -1 && dc == -1) {
        return GONU_DIR_NW;
    }
    return 0U;
}

static void session_game_gonu_connect_cells(gonu_board_pattern_t *pattern,
                                            int r1, int c1, int r2, int c2)
{
    if (pattern == nullptr) {
        return;
    }

    const int dr = r2 - r1;
    const int dc = c2 - c1;
    if (dr < -1 || dr > 1 || dc < -1 || dc > 1 || (dr == 0 && dc == 0)) {
        return;
    }

    const uint8_t forward = session_game_gonu_dir_to_bit(dr, dc);
    const uint8_t backward = session_game_gonu_dir_to_bit(-dr, -dc);
    if (forward == 0U || backward == 0U) {
        return;
    }

    pattern->neighbors[r1][c1] |= forward;
    pattern->neighbors[r2][c2] |= backward;
}

static void session_game_gonu_add_line(gonu_board_pattern_t *pattern, int r1,
                                       int c1, int r2, int c2)
{
    if (pattern == nullptr) {
        return;
    }
    if (!session_game_gonu_in_bounds(r1, c1) ||
        !session_game_gonu_in_bounds(r2, c2)) {
        return;
    }

    const int dr = (r2 > r1) ? 1 : (r2 < r1 ? -1 : 0);
    const int dc = (c2 > c1) ? 1 : (c2 < c1 ? -1 : 0);
    if (dr == 0 && dc == 0) {
        return;
    }
    if (!(dr == 0 || dc == 0 || abs(dr) == abs(dc))) {
        return;
    }

    int row = r1;
    int col = c1;
    bool first = true;
    int prev_row = r1;
    int prev_col = c1;

    while (true) {
        pattern->valid[row][col] = true;
        if (!first) {
            session_game_gonu_connect_cells(pattern, prev_row, prev_col, row,
                                            col);
        }

        if (row == r2 && col == c2) {
            break;
        }

        prev_row = row;
        prev_col = col;
        row += dr;
        col += dc;

        if (!session_game_gonu_in_bounds(row, col)) {
            break;
        }
        first = false;
    }
}

static void session_game_gonu_add_diamond(gonu_board_pattern_t *pattern,
                                          int center, int radius)
{
    session_game_gonu_add_line(pattern, center, center - radius,
                               center - radius, center);
    session_game_gonu_add_line(pattern, center - radius, center, center,
                               center + radius);
    session_game_gonu_add_line(pattern, center, center + radius,
                               center + radius, center);
    session_game_gonu_add_line(pattern, center + radius, center, center,
                               center - radius);
}

static void session_game_gonu_build_patterns(void)
{
    if (gonu_patterns_initialized) {
        return;
    }

    const int center = GONU_BOARD_SIZE / 2;
    for (size_t idx = 0U;
         idx < sizeof(gonu_patterns) / sizeof(gonu_patterns[0]); ++idx) {
        memset(&gonu_patterns[idx], 0, sizeof(gonu_patterns[idx]));
    }

    // Hobak-gonu: layered diamonds with cross beams for a tight pumpkin lattice
    gonu_board_pattern_t *hobak = &gonu_patterns[GONU_VARIANT_HOBAK];
    session_game_gonu_add_diamond(hobak, center, 2);
    session_game_gonu_add_diamond(hobak, center, 1);
    session_game_gonu_add_line(hobak, center, 0, center, GONU_BOARD_SIZE - 1);
    session_game_gonu_add_line(hobak, 0, center, GONU_BOARD_SIZE - 1, center);
    session_game_gonu_add_line(hobak, 1, 1, GONU_BOARD_SIZE - 2, 1);
    session_game_gonu_add_line(hobak, 1, GONU_BOARD_SIZE - 2,
                               GONU_BOARD_SIZE - 2, GONU_BOARD_SIZE - 2);
    session_game_gonu_add_line(hobak, 0, 0, GONU_BOARD_SIZE - 1,
                               GONU_BOARD_SIZE - 1);
    session_game_gonu_add_line(hobak, 0, GONU_BOARD_SIZE - 1,
                               GONU_BOARD_SIZE - 1, 0);

    // Bakwi-gonu: concentric squares with spokes and diagonals on a compact wheel
    gonu_board_pattern_t *bakwi = &gonu_patterns[GONU_VARIANT_BAKWI];
    session_game_gonu_add_line(bakwi, 0, 0, 0, GONU_BOARD_SIZE - 1);
    session_game_gonu_add_line(bakwi, 0, GONU_BOARD_SIZE - 1,
                               GONU_BOARD_SIZE - 1, GONU_BOARD_SIZE - 1);
    session_game_gonu_add_line(bakwi, GONU_BOARD_SIZE - 1, GONU_BOARD_SIZE - 1,
                               GONU_BOARD_SIZE - 1, 0);
    session_game_gonu_add_line(bakwi, GONU_BOARD_SIZE - 1, 0, 0, 0);
    session_game_gonu_add_line(bakwi, 1, 1, 1, GONU_BOARD_SIZE - 2);
    session_game_gonu_add_line(bakwi, 1, GONU_BOARD_SIZE - 2,
                               GONU_BOARD_SIZE - 2, GONU_BOARD_SIZE - 2);
    session_game_gonu_add_line(bakwi, GONU_BOARD_SIZE - 2, GONU_BOARD_SIZE - 2,
                               GONU_BOARD_SIZE - 2, 1);
    session_game_gonu_add_line(bakwi, GONU_BOARD_SIZE - 2, 1, 1, 1);
    session_game_gonu_add_line(bakwi, center, 0, center, GONU_BOARD_SIZE - 1);
    session_game_gonu_add_line(bakwi, 0, center, GONU_BOARD_SIZE - 1, center);
    session_game_gonu_add_line(bakwi, 0, 0, GONU_BOARD_SIZE - 1,
                               GONU_BOARD_SIZE - 1);
    session_game_gonu_add_line(bakwi, 0, GONU_BOARD_SIZE - 1,
                               GONU_BOARD_SIZE - 1, 0);

    // Umul-gonu:井 grid thickened with inner walls and cross-cut diagonals
    gonu_board_pattern_t *umul = &gonu_patterns[GONU_VARIANT_UMUL];
    session_game_gonu_add_line(umul, 0, 0, 0, GONU_BOARD_SIZE - 1);
    session_game_gonu_add_line(umul, GONU_BOARD_SIZE - 1, 0,
                               GONU_BOARD_SIZE - 1, GONU_BOARD_SIZE - 1);
    session_game_gonu_add_line(umul, 0, 0, GONU_BOARD_SIZE - 1, 0);
    session_game_gonu_add_line(umul, 0, GONU_BOARD_SIZE - 1,
                               GONU_BOARD_SIZE - 1, GONU_BOARD_SIZE - 1);
    session_game_gonu_add_line(umul, center, 0, center, GONU_BOARD_SIZE - 1);
    session_game_gonu_add_line(umul, 0, center, GONU_BOARD_SIZE - 1, center);
    session_game_gonu_add_line(umul, 1, 1, 1, GONU_BOARD_SIZE - 2);
    session_game_gonu_add_line(umul, GONU_BOARD_SIZE - 2, 1,
                               GONU_BOARD_SIZE - 2, GONU_BOARD_SIZE - 2);
    session_game_gonu_add_line(umul, 1, 1, GONU_BOARD_SIZE - 2, 1);
    session_game_gonu_add_line(umul, 1, GONU_BOARD_SIZE - 2,
                               GONU_BOARD_SIZE - 2, GONU_BOARD_SIZE - 2);
    session_game_gonu_add_line(umul, 0, 0, GONU_BOARD_SIZE - 1,
                               GONU_BOARD_SIZE - 1);
    session_game_gonu_add_line(umul, 0, GONU_BOARD_SIZE - 1,
                               GONU_BOARD_SIZE - 1, 0);

    // Janggi star palace: layered palace boxes with long star diagonals
    gonu_board_pattern_t *janggi = &gonu_patterns[GONU_VARIANT_JANGGI_STAR];
    session_game_gonu_add_line(janggi, 0, 0, 0, GONU_BOARD_SIZE - 1);
    session_game_gonu_add_line(janggi, 0, GONU_BOARD_SIZE - 1,
                               GONU_BOARD_SIZE - 1, GONU_BOARD_SIZE - 1);
    session_game_gonu_add_line(janggi, GONU_BOARD_SIZE - 1, GONU_BOARD_SIZE - 1,
                               GONU_BOARD_SIZE - 1, 0);
    session_game_gonu_add_line(janggi, GONU_BOARD_SIZE - 1, 0, 0, 0);
    session_game_gonu_add_line(janggi, 1, 1, 1, GONU_BOARD_SIZE - 2);
    session_game_gonu_add_line(janggi, 1, GONU_BOARD_SIZE - 2,
                               GONU_BOARD_SIZE - 2, GONU_BOARD_SIZE - 2);
    session_game_gonu_add_line(janggi, GONU_BOARD_SIZE - 2, GONU_BOARD_SIZE - 2,
                               GONU_BOARD_SIZE - 2, 1);
    session_game_gonu_add_line(janggi, GONU_BOARD_SIZE - 2, 1, 1, 1);
    session_game_gonu_add_line(janggi, center, 0, center, GONU_BOARD_SIZE - 1);
    session_game_gonu_add_line(janggi, 0, center, GONU_BOARD_SIZE - 1, center);
    session_game_gonu_add_line(janggi, 1, center, GONU_BOARD_SIZE - 2, center);
    session_game_gonu_add_line(janggi, center, 1, center, GONU_BOARD_SIZE - 2);
    session_game_gonu_add_line(janggi, 0, 0, GONU_BOARD_SIZE - 1,
                               GONU_BOARD_SIZE - 1);
    session_game_gonu_add_line(janggi, 0, GONU_BOARD_SIZE - 1,
                               GONU_BOARD_SIZE - 1, 0);
    session_game_gonu_add_line(janggi, 1, GONU_BOARD_SIZE - 2,
                               GONU_BOARD_SIZE - 2, 1);
    session_game_gonu_add_line(janggi, 1, 1, GONU_BOARD_SIZE - 2,
                               GONU_BOARD_SIZE - 2);
    session_game_gonu_add_diamond(janggi, center, 1);
    session_game_gonu_add_diamond(janggi, center, 2);

    gonu_patterns_initialized = true;
}

static bool session_game_gonu_is_valid_position(gonu_variant_t variant, int row,
                                                int col)
{
    session_game_gonu_build_patterns();
    if (variant < 0 || variant > GONU_VARIANT_JANGGI_STAR) {
        return false;
    }
    if (!session_game_gonu_in_bounds(row, col)) {
        return false;
    }

    return gonu_patterns[variant].valid[row][col];
}

static bool session_game_gonu_is_connected(gonu_variant_t variant, int r1,
                                           int c1, int r2, int c2)
{
    session_game_gonu_build_patterns();
    if (!session_game_gonu_is_valid_position(variant, r1, c1) ||
        !session_game_gonu_is_valid_position(variant, r2, c2)) {
        return false;
    }

    const int dr = r2 - r1;
    const int dc = c2 - c1;
    if (dr < -1 || dr > 1 || dc < -1 || dc > 1 || (dr == 0 && dc == 0)) {
        return false;
    }

    const uint8_t mask = session_game_gonu_dir_to_bit(dr, dc);
    return (gonu_patterns[variant].neighbors[r1][c1] & mask) != 0U;
}

static bool session_game_gonu_check_win(const gonu_game_state_t *state,
                                        gonu_cell_t player)
{
    // Traditional rule: mills only count after placement is finished and
    // movement begins, so do not award an immediate win during the opening.
    if (state->placement_phase || !state->movement_started) {
        return false;
    }

    // Look for any three-in-a-line sequence across allowed connections.
    const int dirs[][2] = {
        {-1, -1}, {-1, 0}, {-1, 1}, {0, -1}, {0, 1}, {1, -1}, {1, 0}, {1, 1},
    };

    for (int row = 0; row < GONU_BOARD_SIZE; ++row) {
        for (int col = 0; col < GONU_BOARD_SIZE; ++col) {
            if (state->board[row][col] != player ||
                !session_game_gonu_is_valid_position(state->variant, row,
                                                     col)) {
                continue;
            }

            for (size_t d = 0U; d < sizeof(dirs) / sizeof(dirs[0]); ++d) {
                int dr = dirs[d][0];
                int dc = dirs[d][1];
                if (dr == 0 && dc == 0) {
                    continue;
                }

                // Ensure we only count each line once by requiring the previous cell
                // in the opposite direction is not part of the same contiguous line.
                int prev_row = row - dr;
                int prev_col = col - dc;
                if (session_game_gonu_is_connected(state->variant, prev_row,
                                                   prev_col, row, col) &&
                    state->board[prev_row][prev_col] == player) {
                    continue;
                }

                int r2 = row + dr;
                int c2 = col + dc;
                int r3 = row + 2 * dr;
                int c3 = col + 2 * dc;

                if (session_game_gonu_is_connected(state->variant, row, col, r2,
                                                   c2) &&
                    session_game_gonu_is_connected(state->variant, r2, c2, r3,
                                                   c3) &&
                    state->board[r2][c2] == player &&
                    state->board[r3][c3] == player) {
                    return true;
                }
            }
        }
    }

    return false;
}

static int session_game_gonu_count_adjacent(const gonu_game_state_t *state,
                                            int row, int col,
                                            gonu_cell_t player)
{
    if (state == nullptr) {
        return 0;
    }

    int count = 0;
    for (int dr = -1; dr <= 1; ++dr) {
        for (int dc = -1; dc <= 1; ++dc) {
            if (dr == 0 && dc == 0) {
                continue;
            }

            int nr = row + dr;
            int nc = col + dc;
            if (session_game_gonu_is_connected(state->variant, row, col, nr,
                                               nc) &&
                state->board[nr][nc] == player) {
                ++count;
            }
        }
    }

    return count;
}

static int session_game_gonu_positional_score(const gonu_game_state_t *state,
                                              int row, int col,
                                              gonu_cell_t focus)
{
    const int center = GONU_BOARD_SIZE / 2;
    const int manhattan = abs(center - row) + abs(center - col);
    int score = (GONU_BOARD_SIZE * 2) - (manhattan * 2);

    score += session_game_gonu_count_adjacent(state, row, col, focus) * 4;
    score += session_game_gonu_count_adjacent(
                 state, row, col,
                 focus == GONU_CELL_AI ? GONU_CELL_PLAYER : GONU_CELL_AI) *
             3;
    return score;
}

static bool session_game_gonu_has_winning_move(gonu_game_state_t *state,
                                               gonu_cell_t player)
{
    if (state == nullptr || state->placement_phase) {
        return false;
    }

    bool restore_started = state->movement_started;
    state->movement_started = true;

    for (int row = 0; row < GONU_BOARD_SIZE; ++row) {
        for (int col = 0; col < GONU_BOARD_SIZE; ++col) {
            if (state->board[row][col] != player) {
                continue;
            }

            for (int dr = -1; dr <= 1; ++dr) {
                for (int dc = -1; dc <= 1; ++dc) {
                    int nr = row + dr;
                    int nc = col + dc;
                    if (!session_game_gonu_is_connected(state->variant, row,
                                                        col, nr, nc) ||
                        state->board[nr][nc] != GONU_CELL_EMPTY) {
                        continue;
                    }

                    state->board[row][col] = GONU_CELL_EMPTY;
                    state->board[nr][nc] = player;
                    bool win = session_game_gonu_check_win(state, player);
                    state->board[row][col] = player;
                    state->board[nr][nc] = GONU_CELL_EMPTY;

                    if (win) {
                        state->movement_started = restore_started;
                        return true;
                    }
                }
            }
        }
    }

    state->movement_started = restore_started;
    return false;
}

static void session_game_gonu_render(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->game.type != SESSION_GAME_GONU) {
        return;
    }

    gonu_game_state_t *state = &ctx->game.gonu;

    session_send_system_line(ctx, "");
    const char *variant_name = "Gonu";
    switch (state->variant) {
    case GONU_VARIANT_HOBAK:
        variant_name = "Hobak-gonu (Pumpkin)";
        break;
    case GONU_VARIANT_BAKWI:
        variant_name = "Bakwi-gonu (Wheel)";
        break;
    case GONU_VARIANT_UMUL:
        variant_name = "Umul-gonu (Well)";
        break;
    case GONU_VARIANT_JANGGI_STAR:
        variant_name = "Janggi-gonu (Star Palace)";
        break;
    }

    char title[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(title, sizeof(title), "=== %s ===", variant_name);
    session_send_system_line(ctx, title);

    // Use lowercase markers for Bakwi-gonu (wheel) variant
    bool use_lowercase = (state->variant == GONU_VARIANT_BAKWI);

    // Render column headers
    char header[SSH_CHATTER_MESSAGE_LIMIT] = {0};
    size_t header_offset = 0U;
    header_offset += (size_t)snprintf(header + header_offset,
                                      sizeof(header) - header_offset, "  ");
    for (int col = 0; col < GONU_BOARD_SIZE; ++col) {
        header_offset += (size_t)snprintf(header + header_offset,
                                          sizeof(header) - header_offset,
                                          " %c  ", 'A' + col);
    }
    session_send_system_line(ctx, header);

    // Render board with row numbers
    for (int row = 0; row < GONU_BOARD_SIZE; ++row) {
        char line[SSH_CHATTER_MESSAGE_LIMIT] = {0};
        size_t offset = 0U;

        // Add row number
        offset +=
            (size_t)snprintf(line + offset, sizeof(line) - offset, "%d ", row);

        for (int col = 0; col < GONU_BOARD_SIZE; ++col) {
            if (session_game_gonu_is_valid_position(state->variant, row, col)) {
                char cell = '_'; // Use underscore for empty valid positions
                if (state->board[row][col] == GONU_CELL_PLAYER) {
                    cell = use_lowercase ? 'q' : 'Q'; // Player marker
                } else if (state->board[row][col] == GONU_CELL_AI) {
                    cell = use_lowercase ? 'x' : 'X'; // AI marker
                }

                // Highlight selected piece
                if (state->piece_selected && state->selected_row == row &&
                    state->selected_col == col) {
                    offset += (size_t)snprintf(
                        line + offset, sizeof(line) - offset, "[%c] ", cell);
                } else {
                    offset += (size_t)snprintf(
                        line + offset, sizeof(line) - offset, " %c  ", cell);
                }
            } else {
                offset += (size_t)snprintf(line + offset, sizeof(line) - offset,
                                           "    ");
            }
        }
        session_send_system_line(ctx, line);
    }

    session_send_system_line(ctx, "");

    if (state->placement_phase) {
        char msg[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(msg, sizeof(msg), "Placement phase - Player: %u/3, AI: %u/3",
                 state->player_pieces, state->ai_pieces);
        session_send_system_line(ctx, msg);
    }

    if (state->player_turn && !state->game_over) {
        session_send_system_line(
            ctx,
            "Your turn! Enter position as 'row,col' (e.g., '2,C' or '2,2')");
    }
}

static void session_game_gonu_ai_move(session_ctx_t *ctx)
{
    gonu_game_state_t *state = &ctx->game.gonu;

    if (state->multiplayer) {
        return; // No AI in multiplayer
    }

    session_game_seed_rng(ctx);
    unsigned difficulty = state->difficulty_level;
    const bool player_has_immediate_win =
        session_game_gonu_has_winning_move(state, GONU_CELL_PLAYER);

    // Collect all possible moves
    typedef struct {
        int from_row;
        int from_col;
        int to_row;
        int to_col;
        bool is_placement;
        int score;
    } gonu_ai_move_t;

    gonu_ai_move_t possible_moves[128];
    int move_count = 0;

    if (state->placement_phase && state->ai_pieces < 3U) {
        // Find all empty valid positions for placement
        for (int row = 0; row < GONU_BOARD_SIZE; ++row) {
            for (int col = 0; col < GONU_BOARD_SIZE; ++col) {
                if (session_game_gonu_is_valid_position(state->variant, row,
                                                        col) &&
                    state->board[row][col] == GONU_CELL_EMPTY) {
                    possible_moves[move_count].is_placement = true;
                    possible_moves[move_count].to_row = row;
                    possible_moves[move_count].to_col = col;

                    int score = session_game_gonu_positional_score(
                        state, row, col, GONU_CELL_AI);
                    possible_moves[move_count].score = score;
                    move_count++;

                    if (move_count >= 128)
                        break;
                }
            }
            if (move_count >= 128)
                break;
        }
    } else {
        // Movement phase: Find all possible moves
        for (int row = 0; row < GONU_BOARD_SIZE; ++row) {
            for (int col = 0; col < GONU_BOARD_SIZE; ++col) {
                if (state->board[row][col] == GONU_CELL_AI) {
                    // Try to move to adjacent positions
                    for (int dr = -1; dr <= 1; ++dr) {
                        for (int dc = -1; dc <= 1; ++dc) {
                            int new_row = row + dr;
                            int new_col = col + dc;

                            if (session_game_gonu_is_connected(
                                    state->variant, row, col, new_row,
                                    new_col) &&
                                state->board[new_row][new_col] ==
                                    GONU_CELL_EMPTY) {
                                possible_moves[move_count].is_placement = false;
                                possible_moves[move_count].from_row = row;
                                possible_moves[move_count].from_col = col;
                                possible_moves[move_count].to_row = new_row;
                                possible_moves[move_count].to_col = new_col;

                                bool restore_started = state->movement_started;
                                state->board[row][col] = GONU_CELL_EMPTY;
                                state->board[new_row][new_col] = GONU_CELL_AI;
                                state->movement_started = true;

                                const bool ai_wins =
                                    session_game_gonu_check_win(state,
                                                                GONU_CELL_AI);
                                const bool player_can_win_after =
                                    session_game_gonu_has_winning_move(
                                        state, GONU_CELL_PLAYER);

                                int score = session_game_gonu_positional_score(
                                    state, new_row, new_col, GONU_CELL_AI);
                                score += session_game_gonu_count_adjacent(
                                    state, row, col, GONU_CELL_AI);
                                if (ai_wins) {
                                    score += 500;
                                } else if (player_has_immediate_win &&
                                           !player_can_win_after) {
                                    score += 150;
                                }
                                if (player_can_win_after) {
                                    score -= 120;
                                }

                                state->board[row][col] = GONU_CELL_AI;
                                state->board[new_row][new_col] =
                                    GONU_CELL_EMPTY;
                                state->movement_started = restore_started;

                                possible_moves[move_count].score = score;
                                move_count++;

                                if (move_count >= 128)
                                    break;
                            }
                        }
                        if (move_count >= 128)
                            break;
                    }
                }
                if (move_count >= 128)
                    break;
            }
            if (move_count >= 128)
                break;
        }
    }

    if (move_count == 0) {
        return; // No moves available
    }

    // Choose move based on difficulty
    gonu_ai_move_t chosen_move;

    if (difficulty == 1U) {
        // Level 1: Completely random
        int idx = session_game_random_range(ctx, move_count);
        chosen_move = possible_moves[idx];
    } else {
        // Find best moves
        int best_score = -1;
        int best_indexes[128];
        int best_count = 0;

        for (int i = 0; i < move_count; ++i) {
            if (possible_moves[i].score > best_score) {
                best_score = possible_moves[i].score;
                best_indexes[0] = i;
                best_count = 1;
            } else if (possible_moves[i].score == best_score &&
                       best_count < 128) {
                best_indexes[best_count++] = i;
            }
        }

        // Decide whether to make good move or random based on difficulty
        bool make_good_move = true;
        if (difficulty == 2U) {
            make_good_move = (session_game_random_range(ctx, 100) < 50);
        } else if (difficulty == 3U) {
            make_good_move = (session_game_random_range(ctx, 100) < 70);
        } else if (difficulty == 4U) {
            make_good_move = (session_game_random_range(ctx, 100) < 90);
        }
        // Level 5: Always good moves

        if (make_good_move) {
            int idx = session_game_random_range(ctx, best_count);
            chosen_move = possible_moves[best_indexes[idx]];
        } else {
            int idx = session_game_random_range(ctx, move_count);
            chosen_move = possible_moves[idx];
        }
    }

    // Execute the move
    if (chosen_move.is_placement) {
        state->board[chosen_move.to_row][chosen_move.to_col] = GONU_CELL_AI;
        state->ai_pieces++;
        char msg[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(msg, sizeof(msg), "AI placed piece at %d,%d",
                 chosen_move.to_row, chosen_move.to_col);
        session_send_system_line(ctx, msg);

        if (state->ai_pieces == 3U && state->player_pieces == 3U) {
            state->placement_phase = false;
            session_send_system_line(
                ctx, "Placement complete! Now move your pieces.");
        }
    } else {
        state->board[chosen_move.from_row][chosen_move.from_col] =
            GONU_CELL_EMPTY;
        state->board[chosen_move.to_row][chosen_move.to_col] = GONU_CELL_AI;
        state->movement_started = true;
        char msg[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(msg, sizeof(msg), "AI moved from %d,%c to %d,%c",
                 chosen_move.from_row, 'A' + chosen_move.from_col,
                 chosen_move.to_row, 'A' + chosen_move.to_col);
        session_send_system_line(ctx, msg);
    }
}

static bool session_game_gonu_handle_input(session_ctx_t *ctx,
                                           const char *input)
{
    if (ctx == nullptr || input == nullptr ||
        ctx->game.type != SESSION_GAME_GONU) {
        return false;
    }

    gonu_game_state_t *state = &ctx->game.gonu;

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", input);
    trim_whitespace_inplace(working);
    for (size_t idx = 0U; working[idx] != '\0'; ++idx) {
        working[idx] = (char)tolower((unsigned char)working[idx]);
    }

    // Handle mode selection
    if (state->awaiting_mode_selection) {
        if (strcmp(working, "single") == 0 || strcmp(working, "s") == 0) {
            state->awaiting_mode_selection = false;
            state->awaiting_difficulty_selection = true;
            state->multiplayer = false;
            session_send_system_line(ctx, "");
            session_send_system_line(ctx, "Choose difficulty level (1-5):");
            session_send_system_line(ctx, "  1 - Very Easy (random moves)");
            session_send_system_line(ctx, "  2 - Easy (50% good moves)");
            session_send_system_line(ctx, "  3 - Medium (70% good moves)");
            session_send_system_line(ctx, "  4 - Hard (90% good moves)");
            session_send_system_line(ctx, "  5 - Expert (always best moves, "
                                          "but not perfect to keep it fair)");
            session_send_system_line(ctx, "Type a number 1-5:");
        } else if (strcmp(working, "multi") == 0 || strcmp(working, "m") == 0) {
            session_send_system_line(
                ctx, "Multiplayer mode for Gonu is coming soon!");
            session_send_system_line(ctx,
                                     "For now, please choose 'single' mode.");
        } else if (strcmp(working, "exit") == 0 ||
                   strcmp(working, "quit") == 0) {
            session_game_suspend(ctx, "Game cancelled.");
        } else {
            session_send_system_line(
                ctx, "Type 'single' or 'multi' to choose how to play.");
        }
        return true;
    }

    // Handle difficulty selection
    if (state->awaiting_difficulty_selection) {
        int difficulty = atoi(working);
        if (difficulty < 1 || difficulty > 5) {
            session_send_system_line(
                ctx,
                "Invalid difficulty. Please enter a number between 1 and 5.");
            return true;
        }

        state->difficulty_level = (unsigned)difficulty;
        state->awaiting_difficulty_selection = false;
        state->player_turn = true;

        char msg[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(msg, sizeof(msg), "Difficulty set to %d. Good luck!",
                 difficulty);
        session_send_system_line(ctx, msg);
        session_send_system_line(ctx, "");
        session_send_system_line(ctx,
                                 "Gonu started! Place your 3 pieces first.");
        session_send_system_line(ctx,
                                 "Note: Three-in-a-row only wins after all "
                                 "pieces are placed and movement begins.");
        session_send_system_line(
            ctx, "_ = empty position, Q = your piece, X = AI piece");
        char coord_hint[SSH_CHATTER_MESSAGE_LIMIT];
        const char max_letter = (char)('A' + (GONU_BOARD_SIZE - 1));
        snprintf(coord_hint, sizeof(coord_hint),
                 "Use column letters (A-%c) or numbers (0-%d): e.g., '2,%c' or "
                 "'2,2'",
                 max_letter, GONU_BOARD_SIZE - 1, max_letter);
        session_send_system_line(ctx, coord_hint);
        session_send_system_line(ctx, "Press 't' to toggle camouflage screen.");
        session_game_gonu_render(ctx);
        return true;
    }

    if (state->game_over || !state->player_turn) {
        return true;
    }

    // Parse input as "row,col" where col can be a letter (A-G) or number (0-6)
    int row = -1, col = -1;

    // Try to find the comma
    const char *comma = strchr(input, ',');
    if (comma == nullptr) {
        session_send_system_line(
            ctx, "Invalid input. Use format: row,col (e.g., '2,C' or '2,2')");
        return true;
    }

    // Parse row (before comma)
    char row_str[16];
    size_t row_len = (size_t)(comma - input);
    if (row_len >= sizeof(row_str)) {
        session_send_system_line(ctx, "Invalid row value.");
        return true;
    }
    memcpy(row_str, input, row_len);
    row_str[row_len] = '\0';
    trim_whitespace_inplace(row_str);
    row = atoi(row_str);

    // Parse column (after comma)
    char col_str[16];
    snprintf(col_str, sizeof(col_str), "%s", comma + 1);
    trim_whitespace_inplace(col_str);

    // Check if column is a letter (A-G, depending on board size) or number
    const char max_col_letter = (char)('A' + (GONU_BOARD_SIZE - 1));
    if (strlen(col_str) == 1 &&
        ((col_str[0] >= 'A' && col_str[0] <= max_col_letter) ||
         (col_str[0] >= 'a' &&
          col_str[0] <= (char)tolower((unsigned char)max_col_letter)))) {
        // Convert letter to column number
        char letter = (char)toupper((unsigned char)col_str[0]);
        col = letter - 'A';
    } else {
        // Try to parse as number
        col = atoi(col_str);
    }

    if (row < 0 || row >= GONU_BOARD_SIZE || col < 0 ||
        col >= GONU_BOARD_SIZE) {
        session_send_system_line(ctx, "Invalid row or column value.");
        return true;
    }

    if (!session_game_gonu_is_valid_position(state->variant, row, col)) {
        session_send_system_line(ctx,
                                 "Invalid position for this board layout.");
        return true;
    }

    if (state->placement_phase) {
        // Placement phase
        if (state->board[row][col] != GONU_CELL_EMPTY) {
            session_send_system_line(ctx, "Position already occupied.");
            return true;
        }

        state->board[row][col] = GONU_CELL_PLAYER;
        state->player_pieces++;

        if (state->player_pieces == 3U && state->ai_pieces == 3U) {
            state->placement_phase = false;
            session_send_system_line(
                ctx, "Placement complete! Now move your pieces.");
        }
    } else {
        // Movement phase
        if (!state->piece_selected) {
            // Select piece to move
            if (state->board[row][col] != GONU_CELL_PLAYER) {
                session_send_system_line(ctx, "Select your own piece first.");
                return true;
            }
            state->selected_row = row;
            state->selected_col = col;
            state->piece_selected = true;
            session_send_system_line(
                ctx, "Piece selected. Enter destination position.");
            session_game_gonu_render(ctx);
            return true;
        } else {
            // Move selected piece
            if (state->board[row][col] != GONU_CELL_EMPTY) {
                session_send_system_line(ctx, "Destination must be empty.");
                state->piece_selected = false;
                return true;
            }

            if (!session_game_gonu_is_connected(
                    state->variant, state->selected_row, state->selected_col,
                    row, col)) {
                session_send_system_line(ctx,
                                         "Cannot move there - not connected.");
                state->piece_selected = false;
                return true;
            }

            // Execute move
            state->board[state->selected_row][state->selected_col] =
                GONU_CELL_EMPTY;
            state->board[row][col] = GONU_CELL_PLAYER;
            state->movement_started = true;
            state->piece_selected = false;
        }
    }

    // Check for win
    if (session_game_gonu_check_win(state, GONU_CELL_PLAYER)) {
        state->game_over = true;
        session_game_gonu_render(ctx);
        session_send_system_line(ctx, "Congratulations! You won!");
        session_game_suspend(ctx, "");
        return true;
    }

    // AI turn
    state->player_turn = false;
    session_game_gonu_ai_move(ctx);

    if (session_game_gonu_check_win(state, GONU_CELL_AI)) {
        state->game_over = true;
        session_game_gonu_render(ctx);
        session_send_system_line(ctx, "AI wins! Better luck next time.");
        session_game_suspend(ctx, "");
        return true;
    }

    state->player_turn = true;
    session_game_gonu_render(ctx);
    return true;
}

static void session_game_start_gonu(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    // Ask for camouflage language first
    session_send_system_line(
        ctx,
        "CHOOSE YOUR LOCKSCREEN LANGUAGE TO HIDE THE SCREEN ON YOUR OFFICE! "
        "(c, cpp, java, go, js, ts, rust)");
    char language_choice[16];
    size_t length = 0U;
    while (length + 1U < sizeof(language_choice)) {
        char ch = '\0';
        const int read_result = session_transport_read(ctx, &ch, 1, -1);
        if (read_result <= 0) {
            return;
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

        language_choice[length++] = ch;
        session_local_echo_char(ctx, ch);
    }
    language_choice[length] = '\0';
    trim_whitespace_inplace(language_choice);
    for (size_t idx = 0U; language_choice[idx] != '\0'; ++idx) {
        language_choice[idx] =
            (char)tolower((unsigned char)language_choice[idx]);
    }

    if (language_choice[0] == '\0') {
        session_send_system_line(ctx, "No language chosen. Defaulting to C.");
        snprintf(ctx->game.chosen_camouflage_language,
                 sizeof(ctx->game.chosen_camouflage_language), "c");
    } else if (strcmp(language_choice, "c") == 0 ||
               strcmp(language_choice, "cpp") == 0 ||
               strcmp(language_choice, "java") == 0 ||
               strcmp(language_choice, "go") == 0 ||
               strcmp(language_choice, "js") == 0 ||
               strcmp(language_choice, "ts") == 0 ||
               strcmp(language_choice, "rust") == 0) {
        snprintf(ctx->game.chosen_camouflage_language,
                 sizeof(ctx->game.chosen_camouflage_language), "%s",
                 language_choice);
    } else {
        session_send_system_line(ctx, "Invalid language. Defaulting to C.");
        snprintf(ctx->game.chosen_camouflage_language,
                 sizeof(ctx->game.chosen_camouflage_language), "c");
    }

    // Now ask for game variant
    session_send_system_line(ctx, "");
    session_send_system_line(ctx, "Choose Gonu variant:");
    session_send_system_line(
        ctx, "1. Hobak-gonu (Pumpkin) - layered diamonds from the classic map");
    session_send_system_line(
        ctx, "2. Bakwi-gonu (Wheel) - triple-ring wheel with spokes");
    session_send_system_line(
        ctx, "3. Umul-gonu (Well) - thickened井 grid with diagonals");
    session_send_system_line(
        ctx, "4. Janggi-gonu (Star Palace) - extended palace star lattice");
    session_send_system_line(ctx, "Enter 1, 2, 3, or 4 (default 4):");

    char variant_choice[16];
    length = 0U;
    while (length + 1U < sizeof(variant_choice)) {
        char ch = '\0';
        const int read_result = session_transport_read(ctx, &ch, 1, -1);
        if (read_result <= 0) {
            return;
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

        variant_choice[length++] = ch;
        session_local_echo_char(ctx, ch);
    }
    variant_choice[length] = '\0';
    trim_whitespace_inplace(variant_choice);

    gonu_variant_t variant = GONU_VARIANT_JANGGI_STAR;
    if (strcmp(variant_choice, "1") == 0) {
        variant = GONU_VARIANT_HOBAK;
    } else if (strcmp(variant_choice, "2") == 0) {
        variant = GONU_VARIANT_BAKWI;
    } else if (strcmp(variant_choice, "3") == 0) {
        variant = GONU_VARIANT_UMUL;
    }

    session_game_gonu_reset(&ctx->game.gonu, variant);
    ctx->game.type = SESSION_GAME_GONU;
    ctx->game.active = true;
    ctx->game.gonu.awaiting_mode_selection = true;

    session_send_system_line(ctx, "");
    session_send_system_line(ctx, "Choose Gonu mode: type 'single' to play "
                                  "against AI or 'multi' for multiplayer.");
}

static void session_handle_game(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (ctx->game.active) {
        session_send_system_line(
            ctx, "Finish the current game with /suspend! first.");
        return;
    }

    if (arguments == nullptr) {
        session_send_system_line(
            ctx, "Usage: /game <tetris|liargame|alpha|othello|gonu>");
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);
    if (working[0] == '\0') {
        session_send_system_line(
            ctx, "Usage: /game <tetris|liargame|alpha|othello|gonu>");
        return;
    }

    for (size_t idx = 0U; working[idx] != '\0'; ++idx) {
        working[idx] = (char)tolower((unsigned char)working[idx]);
    }

    if (strcmp(working, "tetris") == 0) {
        session_game_start_tetris(ctx);
    } else if (strcmp(working, "liargame") == 0) {
        session_game_start_liargame(ctx);
    } else if (strcmp(working, "alpha") == 0 ||
               strcmp(working, "alphacentauri") == 0) {
        session_game_start_alpha(ctx);
    } else if (strcmp(working, "othello") == 0) {
        session_game_start_othello(ctx);
    } else if (strcmp(working, "gonu") == 0) {
        session_game_start_gonu(ctx);
    } else {
        session_send_system_line(ctx,
                                 "Unknown game. Available options: "
                                 "tetris, liargame, alpha, othello, gonu.");
    }
}

static void session_game_suspend(session_ctx_t *ctx, const char *reason)
{
    if (ctx == nullptr) {
        return;
    }

    if (!ctx->game.active) {
        if (reason != nullptr && reason[0] != '\0') {
            session_send_system_line(ctx, reason);
        } else {
            session_send_system_line(ctx,
                                     "There is no active game to suspend.");
        }
        return;
    }

    // Restore normal screen buffer when exiting game mode
    if (ctx->game.type == SESSION_GAME_TETRIS) {
        session_disable_alternate_screen(ctx);
    }

    if (reason != nullptr && reason[0] != '\0') {
        session_send_system_line(ctx, reason);
    }

    if (ctx->game.type == SESSION_GAME_TETRIS) {
        char summary[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(summary, sizeof(summary),
                 "Tetris final score: %u (lines cleared: %u).",
                 ctx->game.tetris->score, ctx->game.tetris->lines_cleared);
        session_send_system_line(ctx, summary);
        session_game_tetris_reset(ctx->game.tetris);
    } else if (ctx->game.type == SESSION_GAME_LIARGAME) {
        char summary[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(summary, sizeof(summary),
                 "Liar Game rounds played: %u, score: %u.",
                 ctx->game.liar.round_number, ctx->game.liar.score);
        session_send_system_line(ctx, summary);
        ctx->game.liar.awaiting_guess = false;
        ctx->game.liar.round_number = 0U;
        ctx->game.liar.score = 0U;
    } else if (ctx->game.type == SESSION_GAME_ALPHA) {
        char summary[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(
            summary, sizeof(summary),
            "Alpha Centauri mission paused at stage %u with %.2f ly remaining.",
            ctx->game.alpha.stage, ctx->game.alpha.distance_remaining_ly);
        session_send_system_line(ctx, summary);
        session_game_alpha_reset(ctx);
        session_game_alpha_sync_to_save(ctx);
    } else if (ctx->game.type == SESSION_GAME_OTHELLO) {
        unsigned red = ctx->game.othello.red_score;
        unsigned green = ctx->game.othello.green_score;
        if (red == 0U && green == 0U) {
            session_game_othello_count_scores(&ctx->game.othello, &red, &green);
        }

        const char *outcome = "It's a draw.";
        if (red > green) {
            outcome = "Red wins!";
        } else if (green > red) {
            outcome = "Green wins!";
        }

        char summary[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(summary, sizeof(summary),
                 "Othello final score: Red %u vs Green %u - %s", red, green,
                 outcome);
        session_send_system_line(ctx, summary);
        session_game_othello_reset_state(&ctx->game.othello);
    }

    ctx->game.active = false;
    ctx->game.type = SESSION_GAME_NONE;
}

static int session_channel_read_poll(session_ctx_t *ctx, char *buffer,
                                     size_t length, int timeout_ms)
{
    if (ctx == nullptr || buffer == nullptr || length == 0U ||
        !session_transport_active(ctx)) {
        return SSH_ERROR;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        int result = session_transport_read(ctx, buffer, length, timeout_ms);
        if (result == SSH_AGAIN) {
            return SESSION_CHANNEL_TIMEOUT;
        }
        return result;
    }

    int fd = ssh_get_fd(ctx->session);
    if (fd < 0) {
        return session_transport_read(ctx, buffer, length, -1);
    }

    int val = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) < 0) {
        fprintf(stderr, "[session] setsockopt SO_KEEPALIVE failed");
    }

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    for (;;) {
        int poll_result = poll(&pfd, 1, timeout_ms);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SSH_ERROR;
        }
        if (poll_result == 0) {
            return SESSION_CHANNEL_TIMEOUT;
        }
        break;
    }

    if ((pfd.revents & POLLIN) == 0) {
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return 0;
        }
        return SESSION_CHANNEL_TIMEOUT;
    }

    return session_transport_read(ctx, buffer, length, -1);
}

static bool session_parse_color_arguments(char *working, char **tokens,
                                          size_t max_tokens,
                                          size_t *token_count)
{
    if (working == nullptr || tokens == nullptr || token_count == nullptr) {
        return false;
    }

    *token_count = 0U;
    bool extra_tokens = false;
    char *cursor = working;
    while (cursor != nullptr) {
        char *next = strchr(cursor, ';');
        if (next != nullptr) {
            *next = '\0';
        }

        trim_whitespace_inplace(cursor);
        if (cursor[0] == '\0') {
            return false;
        }

        if (*token_count < max_tokens) {
            tokens[*token_count] = cursor;
            ++(*token_count);
        } else if (cursor[0] != '\0') {
            extra_tokens = true;
        }

        if (next == nullptr) {
            break;
        }

        cursor = next + 1;
        if (cursor[0] == '\0') {
            extra_tokens = true;
            break;
        }
    }

    return !extra_tokens;
}

#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>

static bool session_valid_ansi_256_sequence(const char *sequence)
{
    if (sequence == NULL) {
        return false;
    }

    size_t length = strnlen(sequence, SSH_CHATTER_COLOR_CODE_LEN);
    if (length >= SSH_CHATTER_COLOR_CODE_LEN) {
        return false;
    }

    size_t idx = 0;
    bool found_escape = false;
    while (idx < length) {
        if (sequence[idx] != '\x1b') {
            idx++;
            continue;
        }

        found_escape = true;

        if (idx + 1 >= length || sequence[idx + 1] != '[') {
            return false;
        }

        size_t end = idx + 2;
        bool found_terminator = false;

        while (end < length) {
            char ch = sequence[end];

            if (ch == 'm') {
                found_terminator = true;
                break;
            }

            if (!isdigit((unsigned char)ch) && ch != ';') {
                return false;
            }
            end++;
        }

        if (!found_terminator) {
            return false;
        }

        size_t param_len = end - (idx + 2);

        if (param_len > 64) {
            return false;
        }

        if (param_len > 0) {
            char buffer[65];
            memcpy(buffer, sequence + idx + 2, param_len);
            buffer[param_len] = '\0';

            char *fg_ptr = strstr(buffer, "38;5;");
            char *bg_ptr = strstr(buffer, "48;5;");
            char *fg_truecolor_ptr = strstr(buffer, "38;2;");
            char *bg_truecolor_ptr = strstr(buffer, "48;2;");
            char *target = (fg_ptr) ? fg_ptr : bg_ptr;
            char *truecolor_target =
                (fg_truecolor_ptr) ? fg_truecolor_ptr : bg_truecolor_ptr;

            if (target != NULL) {
                target += 5;

                char *endptr = NULL;
                long val = strtol(target, &endptr, 10);
                if (val < 0 || val > 255 || target == endptr) {
                    return false;
                }
            }

            if (truecolor_target != NULL) {
                truecolor_target += 5;
                for (int component = 0; component < 3; ++component) {
                    char *endptr = NULL;
                    long val = strtol(truecolor_target, &endptr, 10);
                    if (val < 0 || val > 255 || truecolor_target == endptr) {
                        return false;
                    }

                    if (component < 2) {
                        if (endptr == NULL || *endptr != ';') {
                            return false;
                        }
                        truecolor_target = endptr + 1;
                    }
                }
            }
        }
        idx = end + 1;
    }
    return found_escape;
}

static bool session_translate_escape_sequences(const char *input, char *output,
                                               size_t output_size)
{
    if (input == NULL || output == NULL || output_size == 0U) {
        return false;
    }

    const char *cursor = input;
    char *out = output;
    char *out_limit = output + output_size - 1U;

    while (*cursor != '\0') {
        if (out >= out_limit) {
            return false;
        }

        if (*cursor == '\\') {
            if (strncmp(cursor, "\\033", 4U) == 0 ||
                strncmp(cursor, "\\x1b", 4U) == 0 ||
                strncmp(cursor, "\\x1B", 4U) == 0) {
                *out++ = '\x1b';
                cursor += 4U;
                continue;
            }

            if (strncmp(cursor, "\\u001b", 6U) == 0 ||
                strncmp(cursor, "\\u001B", 6U) == 0) {
                *out++ = '\x1b';
                cursor += 6U;
                continue;
            }

            if (cursor[1] == 'e' || cursor[1] == 'E') {
                *out++ = '\x1b';
                cursor += 2U;
                continue;
            }
        }

        *out++ = *cursor++;
    }

    *out = '\0';
    return true;
}

static void session_handle_color(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    static const char *kColorUsage =
        "Usage: /color (text;highlight[;bold]) or /color advanced <ansi>";

    if (arguments == nullptr) {
        session_send_system_line(ctx, kColorUsage);
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, kColorUsage);
        return;
    }

    bool had_parentheses = false;
    if (working[0] == '(') {
        had_parentheses = true;
        memmove(working, working + 1, strlen(working));
        trim_whitespace_inplace(working);
    }

    if (had_parentheses) {
        size_t len = strlen(working);
        if (len == 0U || working[len - 1U] != ')') {
            session_send_system_line(ctx,
                                     "Usage: /color (text;highlight[;bold])");
            return;
        }
        working[len - 1U] = '\0';
        trim_whitespace_inplace(working);
    }

    if (working[0] == '\0') {
        session_send_system_line(ctx, kColorUsage);
        return;
    }

    if (strncasecmp(working, "advanced", strlen("advanced")) == 0) {
        const char *raw_code = working + strlen("advanced");
        while (raw_code[0] != '\0' && isspace((unsigned char)raw_code[0])) {
            ++raw_code;
        }

        if (raw_code[0] == '\0') {
            session_send_system_line(ctx, kColorUsage);
            return;
        }

        char translated_name[SSH_CHATTER_USERNAME_LEN];
        if (!session_translate_escape_sequences(raw_code, translated_name,
                                                sizeof(translated_name)) ||
            !session_valid_ansi_256_sequence(translated_name)) {
            session_send_system_line(ctx, "Invalid ANSI/256 expression. Use "
                                          "sequences like \\x1b[38;5;196m.");
            return;
        }

        if (!ctx->user_data_loaded) {
            (void)session_user_data_load(ctx);
        }

        snprintf(ctx->user_data.preferred_nickname,
                 sizeof(ctx->user_data.preferred_nickname), "%s",
                 translated_name);

        if (ctx->owner != nullptr && ctx->owner->user_data_root[0] != '\0' &&
            ctx->user_data.username[0] != '\0') {
            user_data_save(ctx->owner->user_data_root, &ctx->user_data,
                           ctx->client_ip);
        }

        ctx->user_color_code = "";
        ctx->user_highlight_code = "";
        ctx->user_highlight_code_buffer[0] = '\0';
        ctx->user_color_code_buffer[0] = '\0';
        ctx->user_is_bold = false;

        session_send_system_line(
            ctx, "Display name updated with advanced ANSI formatting.");

        char preview[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(preview, sizeof(preview), "%s%s", translated_name, ANSI_RESET);
        session_send_line(ctx, preview);

        return;
    }

    if (working[0] == '\0') {
        session_send_system_line(ctx, kColorUsage);
        return;
    }

    char *tokens[3] = {0};
    size_t token_count = 0U;
    if (!session_parse_color_arguments(working, tokens, 3U, &token_count) ||
        token_count < 2U) {
        session_send_system_line(ctx, kColorUsage);
        return;
    }

    const char *text_code = lookup_color_code(
        USER_COLOR_MAP, sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
        tokens[0]);
    if (text_code == nullptr) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "Unknown text color '%s'.",
                 tokens[0]);
        session_send_system_line(ctx, message);
        return;
    }

    const char *highlight_code = lookup_color_code(
        HIGHLIGHT_COLOR_MAP,
        sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
        tokens[1]);
    if (highlight_code == nullptr) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "Unknown highlight color '%s'.",
                 tokens[1]);
        session_send_system_line(ctx, message);
        return;
    }

    bool is_bold = false;
    if (token_count == 3U) {
        if (!parse_bool_token(tokens[2], &is_bold)) {
            session_send_system_line(
                ctx,
                "The third value must describe bold (ex: bold, true, normal).");
            return;
        }
    }

    ctx->user_color_code = text_code;
    ctx->user_highlight_code = highlight_code;
    ctx->user_is_bold = is_bold;
    snprintf(ctx->user_color_name, sizeof(ctx->user_color_name), "%s",
             tokens[0]);
    snprintf(ctx->user_highlight_name, sizeof(ctx->user_highlight_name), "%s",
             tokens[1]);

    char info[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(info, sizeof(info),
             "Handle colors updated: text=%s highlight=%s bold=%s", tokens[0],
             tokens[1], is_bold ? "on" : "off");
    session_send_system_line(ctx, info);

    const char *bold_code = is_bold ? ANSI_BOLD : "";
    char preview[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(preview, sizeof(preview), "%s%s%s[%s] preview%s", highlight_code,
             bold_code, text_code, ctx->user.name, ANSI_RESET);
    session_send_line(ctx, preview);

    if (ctx->owner != nullptr) {
        host_store_user_theme(ctx->owner, ctx);
    }
}

static void session_handle_motd(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    host_refresh_motd(ctx->owner);

    ttak_mutex_lock(&ctx->owner->lock);
    const char *motd_to_display = ctx->owner->motd;
    ttak_mutex_unlock(&ctx->owner->lock);

    if (motd_to_display[0] != '\0') {
        session_send_raw_text(ctx, motd_to_display);
    } else {
        session_send_system_line(ctx, "No message of the day configured.");
    }
}

static void session_handle_system_color(session_ctx_t *ctx,
                                        const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    static const char *kUsage =
        "Usage: /systemcolor (fg;background[;highlight][;bold]) or "
        "/systemcolor reset - third value may be highlight or "
        "bold.";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/systemcolor", kUsage, usage,
                                 sizeof(usage));

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

    bool had_parentheses = false;
    if (working[0] == '(') {
        had_parentheses = true;
        memmove(working, working + 1, strlen(working));
        trim_whitespace_inplace(working);
    }

    if (had_parentheses) {
        size_t len = strlen(working);
        if (len == 0U || working[len - 1U] != ')') {
            session_send_system_line(ctx, usage);
            return;
        }
        working[len - 1U] = '\0';
        trim_whitespace_inplace(working);
    }

    if (working[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    if (strcasecmp(working, "reset") == 0) {
        session_apply_system_theme_defaults(ctx);
        session_send_system_line(ctx, "System colors reset to defaults.");
        session_render_separator(ctx, "Chatroom");
        session_render_prompt(ctx, true);
        if (ctx->owner != nullptr) {
            host_store_system_theme(ctx->owner, ctx);
        }
        return;
    }

    char *tokens[4] = {0};
    size_t token_count = 0U;
    if (!session_parse_color_arguments(working, tokens, 4U, &token_count) ||
        token_count < 2U) {
        session_send_system_line(ctx, usage);
        return;
    }

    const char *fg_code = lookup_color_code(
        USER_COLOR_MAP, sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
        tokens[0]);
    if (fg_code == nullptr) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "Unknown foreground color '%s'.",
                 tokens[0]);
        session_send_system_line(ctx, message);
        return;
    }

    const char *bg_code = lookup_color_code(HIGHLIGHT_COLOR_MAP,
                                            sizeof(HIGHLIGHT_COLOR_MAP) /
                                                sizeof(HIGHLIGHT_COLOR_MAP[0]),
                                            tokens[1]);
    if (bg_code == nullptr) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "Unknown background color '%s'.",
                 tokens[1]);
        session_send_system_line(ctx, message);
        return;
    }

    const char *highlight_code = ctx->system_highlight_code;
    bool highlight_updated = false;
    bool is_bold = ctx->system_is_bold;
    if (token_count >= 3U) {
        bool bool_value = false;
        if (parse_bool_token(tokens[2], &bool_value)) {
            if (token_count > 3U) {
                session_send_system_line(ctx, usage);
                return;
            }
            is_bold = bool_value;
        } else {
            highlight_code = lookup_color_code(
                HIGHLIGHT_COLOR_MAP,
                sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
                tokens[2]);
            if (highlight_code == nullptr) {
                char message[SSH_CHATTER_MESSAGE_LIMIT];
                snprintf(message, sizeof(message),
                         "Unknown highlight color '%s'.", tokens[2]);
                session_send_system_line(ctx, message);
                return;
            }
            highlight_updated = true;

            if (token_count == 4U) {
                if (!parse_bool_token(tokens[3], &bool_value)) {
                    session_send_system_line(ctx,
                                             "The last value must describe "
                                             "bold (ex: bold, true, normal).");
                    return;
                }
                is_bold = bool_value;
            }
        }
    }

    ctx->system_fg_code = fg_code;
    ctx->system_bg_code = bg_code;
    ctx->system_highlight_code = highlight_code;
    ctx->system_is_bold = is_bold;
    snprintf(ctx->system_fg_name, sizeof(ctx->system_fg_name), "%s", tokens[0]);
    snprintf(ctx->system_bg_name, sizeof(ctx->system_bg_name), "%s", tokens[1]);
    if (highlight_updated) {
        snprintf(ctx->system_highlight_name, sizeof(ctx->system_highlight_name),
                 "%s", tokens[2]);
    }

    session_force_dark_mode_foreground(ctx);
    session_apply_background_fill(ctx);

    session_send_system_line(ctx, "System colors updated.");
    session_render_separator(ctx, "Chatroom");
    session_render_prompt(ctx, true);
    if (ctx->owner != nullptr) {
        host_store_system_theme(ctx->owner, ctx);
    }
}

static void session_handle_set_trans_lang(session_ctx_t *ctx,
                                          const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    char working[SSH_CHATTER_LANG_NAME_LEN];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
    }
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, "Usage: /set-trans-lang <language|off>");
        return;
    }

    if (session_argument_is_disable(working)) {
        ctx->output_translation_enabled = false;
        ctx->output_translation_language[0] = '\0';
        session_translation_clear_queue(ctx);
        session_send_system_line(ctx, "Terminal translation disabled.");
        if (ctx->owner != nullptr) {
            host_store_translation_preferences(ctx->owner, ctx);
        }
        return;
    }

    if (session_language_equals(ctx->output_translation_language, working)) {
        snprintf(ctx->output_translation_language,
                 sizeof(ctx->output_translation_language), "%s", working);
        ctx->output_translation_enabled = true;
        session_translation_clear_queue(ctx);

        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message),
                 "Terminal output will continue to be translated to %s.",
                 ctx->output_translation_language);
        session_send_system_line(ctx, message);
        if (!ctx->translation_enabled) {
            session_send_system_line(ctx, "Translation is currently disabled; "
                                          "enable it with /translate on.");
        }
        if (ctx->owner != nullptr) {
            host_store_translation_preferences(ctx->owner, ctx);
        }
        return;
    }

    char preview[SSH_CHATTER_MESSAGE_LIMIT];
    char detected[SSH_CHATTER_LANG_NAME_LEN];
    if (!translator_translate("Terminal messages will be translated for you.",
                              working, preview, sizeof(preview), detected,
                              sizeof(detected))) {
        const char *error = translator_last_error();
        if (error != nullptr && *error != '\0') {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message), "Translation service error: %s",
                     error);
            session_send_system_line(ctx, message);
        } else {
            session_send_system_line(ctx, "Failed to reach the translation "
                                          "service. Please try again later.");
        }
        return;
    }

    snprintf(ctx->output_translation_language,
             sizeof(ctx->output_translation_language), "%s", working);
    ctx->output_translation_enabled = true;
    session_translation_clear_queue(ctx);

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    int preview_limit = (int)(sizeof(message) / 2);
    if (preview_limit <= 0) {
        preview_limit = (int)sizeof(message) - 1;
    }
    int detected_limit = (int)sizeof(detected) - 1;
    if (detected_limit <= 0) {
        detected_limit = (int)sizeof(detected);
    }
    if (detected[0] != '\0') {
        snprintf(message, sizeof(message),
                 "Terminal output will be translated to %s. Sample: %.*s "
                 "(detected: %.*s).",
                 ctx->output_translation_language, preview_limit, preview,
                 detected_limit, detected);
    } else {
        snprintf(message, sizeof(message),
                 "Terminal output will be translated to %s. Sample: %.*s.",
                 ctx->output_translation_language, preview_limit, preview);
    }
    session_send_system_line(ctx, message);
    if (!ctx->translation_enabled) {
        session_send_system_line(
            ctx,
            "Translation is currently disabled; enable it with /translate on.");
    }
    if (ctx->owner != nullptr) {
        host_store_translation_preferences(ctx->owner, ctx);
    }
}

static void session_handle_set_target_lang(session_ctx_t *ctx,
                                           const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    char working[SSH_CHATTER_LANG_NAME_LEN];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
    }
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, "Usage: /set-target-lang <language|off>");
        return;
    }

    if (session_argument_is_disable(working)) {
        ctx->input_translation_enabled = false;
        ctx->input_translation_language[0] = '\0';
        ctx->last_detected_input_language[0] = '\0';
        session_send_system_line(ctx, "Outgoing message translation disabled.");
        if (ctx->owner != nullptr) {
            host_store_translation_preferences(ctx->owner, ctx);
        }
        return;
    }

    if (session_language_equals(ctx->input_translation_language, working)) {
        snprintf(ctx->input_translation_language,
                 sizeof(ctx->input_translation_language), "%s", working);
        ctx->input_translation_enabled = true;
        ctx->last_detected_input_language[0] = '\0';

        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message),
                 "Outgoing messages will continue to be translated to %s.",
                 ctx->input_translation_language);
        session_send_system_line(ctx, message);
        if (!ctx->translation_enabled) {
            session_send_system_line(ctx, "Translation is currently disabled; "
                                          "enable it with /translate on.");
        }
        if (ctx->owner != nullptr) {
            host_store_translation_preferences(ctx->owner, ctx);
        }
        return;
    }

    char preview[SSH_CHATTER_MESSAGE_LIMIT];
    char detected[SSH_CHATTER_LANG_NAME_LEN];
    if (!translator_translate(
            "Your messages will be translated before broadcasting.", working,
            preview, sizeof(preview), detected, sizeof(detected))) {
        const char *error = translator_last_error();
        if (error != nullptr && *error != '\0') {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message), "Translation service error: %s",
                     error);
            session_send_system_line(ctx, message);
        } else {
            session_send_system_line(ctx, "Failed to reach the translation "
                                          "service. Please try again later.");
        }
        return;
    }

    snprintf(ctx->input_translation_language,
             sizeof(ctx->input_translation_language), "%s", working);
    ctx->input_translation_enabled = true;
    ctx->last_detected_input_language[0] = '\0';

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    int preview_limit = (int)(sizeof(message) / 2);
    if (preview_limit <= 0) {
        preview_limit = (int)sizeof(message) - 1;
    }
    int detected_limit = (int)sizeof(detected) - 1;
    if (detected_limit <= 0) {
        detected_limit = (int)sizeof(detected);
    }
    if (detected[0] != '\0') {
        snprintf(message, sizeof(message),
                 "Outgoing messages will be translated to %s. Sample: %.*s "
                 "(detected: %.*s).",
                 ctx->input_translation_language, preview_limit, preview,
                 detected_limit, detected);
    } else {
        snprintf(message, sizeof(message),
                 "Outgoing messages will be translated to %s. Sample: %.*s.",
                 ctx->input_translation_language, preview_limit, preview);
    }
    session_send_system_line(ctx, message);
    if (!ctx->translation_enabled) {
        session_send_system_line(
            ctx,
            "Translation is currently disabled; enable it with /translate on.");
    }
    if (ctx->owner != nullptr) {
        host_store_translation_preferences(ctx->owner, ctx);
    }
}

static void session_handle_chat_spacing(session_ctx_t *ctx,
                                        const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    const session_ui_locale_t *locale = session_ui_get_locale(ctx);
    const char *prefix = session_command_prefix(ctx);
    const char *usage_format = (locale->chat_spacing_usage != nullptr &&
                                locale->chat_spacing_usage[0] != '\0')
                                   ? locale->chat_spacing_usage
                                   : "Usage: %schat-spacing <0-5>";

    char working[16];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
    }
    trim_whitespace_inplace(working);

    char usage_message[SSH_CHATTER_MESSAGE_LIMIT];
    const char *usage_args[] = {prefix};

    if (working[0] == '\0') {
        session_format_template(usage_format, usage_args,
                                sizeof(usage_args) / sizeof(usage_args[0]),
                                usage_message, sizeof(usage_message));
        session_send_system_line(ctx, usage_message);
        return;
    }

    char *endptr = nullptr;
    long value = strtol(working, &endptr, 10);
    if (endptr == working || (endptr != nullptr && *endptr != '\0') ||
        value < 0L || value > 5L) {
        session_format_template(usage_format, usage_args,
                                sizeof(usage_args) / sizeof(usage_args[0]),
                                usage_message, sizeof(usage_message));
        session_send_system_line(ctx, usage_message);
        return;
    }

    ctx->translation_caption_spacing = (size_t)value;

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    if (value == 0L) {
        const char *format =
            (locale->chat_spacing_immediate != nullptr &&
             locale->chat_spacing_immediate[0] != '\0')
                ? locale->chat_spacing_immediate
                : "Translation captions will appear immediately "
                  "without reserving extra blank lines.";
        session_format_template(format, nullptr, 0U, message, sizeof(message));
    } else if (value == 1L) {
        const char *format =
            (locale->chat_spacing_single != nullptr &&
             locale->chat_spacing_single[0] != '\0')
                ? locale->chat_spacing_single
                : "Translation captions will reserve 1 blank line "
                  "before appearing in chat threads.";
        session_format_template(format, nullptr, 0U, message, sizeof(message));
    } else {
        const char *format = (locale->chat_spacing_multiple != nullptr &&
                              locale->chat_spacing_multiple[0] != '\0')
                                 ? locale->chat_spacing_multiple
                                 : "Translation captions will reserve %s blank "
                                   "lines before appearing in chat threads.";
        char count[16];
        snprintf(count, sizeof(count), "%ld", value);
        const char *args[] = {count};
        session_format_template(format, args, sizeof(args) / sizeof(args[0]),
                                message, sizeof(message));
    }
    session_send_system_line(ctx, message);

    if (ctx->owner != nullptr) {
        host_store_chat_spacing(ctx->owner, ctx);
    }
}

static void session_handle_set_ui_lang(session_ctx_t *ctx,
                                       const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    const session_ui_locale_t *locale = session_ui_get_locale(ctx);
    const char *prefix = session_command_prefix(ctx);

    char token[SSH_CHATTER_LANG_NAME_LEN];
    const char *cursor = session_consume_token(arguments, token, sizeof(token));

    bool extra_tokens = cursor != nullptr && *cursor != '\0';
    if (token[0] == '\0' || extra_tokens) {
        const char *format = (locale->set_ui_lang_usage != nullptr &&
                              locale->set_ui_lang_usage[0] != '\0')
                                 ? locale->set_ui_lang_usage
                                 : "Usage: %sset-ui-lang <ko|en|jp|zh|ru>";
        const char *args[] = {prefix};
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        session_format_template(format, args, sizeof(args) / sizeof(args[0]),
                                message, sizeof(message));
        session_send_system_line(ctx, message);
        return;
    }

    session_ui_language_t language = session_ui_language_from_code(token);
    if (language == SESSION_UI_LANGUAGE_COUNT) {
        const char *format =
            (locale->set_ui_lang_invalid != nullptr &&
             locale->set_ui_lang_invalid[0] != '\0')
                ? locale->set_ui_lang_invalid
                : "Unsupported language. Use one of: ko, en, jp, zh, ru.";
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        session_format_template(format, nullptr, 0U, message, sizeof(message));
        session_send_system_line(ctx, message);
        return;
    }

    ctx->ui_language = language;
    const session_ui_locale_t *updated_locale = session_ui_get_locale(ctx);
    const char *language_name =
        session_ui_language_name(language, ctx->ui_language);
    const char *format =
        (updated_locale->set_ui_lang_success != nullptr &&
         updated_locale->set_ui_lang_success[0] != '\0')
            ? updated_locale->set_ui_lang_success
            : "UI language set to %s. Use %shelp to review commands.";
    const char *updated_prefix = session_command_prefix(ctx);

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    const char *args[] = {language_name != nullptr ? language_name : "-",
                          updated_prefix};
    session_format_template(format, args, sizeof(args) / sizeof(args[0]),
                            message, sizeof(message));
    session_send_system_line(ctx, message);

    if (ctx->owner != nullptr) {
        host_store_ui_language(ctx->owner, ctx);
    }
}

static void session_handle_mode(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    const session_ui_locale_t *locale = session_ui_get_locale(ctx);
    const char *prefix = session_command_prefix(ctx);
    const char *chat_label = (locale->mode_label_chat != nullptr &&
                              locale->mode_label_chat[0] != '\0')
                                 ? locale->mode_label_chat
                                 : "chat";
    const char *command_label = (locale->mode_label_command != nullptr &&
                                 locale->mode_label_command[0] != '\0')
                                    ? locale->mode_label_command
                                    : "command";

    char working[32];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
    }
    trim_whitespace_inplace(working);

    const char *status_format = (locale->mode_status_format != nullptr &&
                                 locale->mode_status_format[0] != '\0')
                                    ? locale->mode_status_format
                                    : "Current input mode: %s.";
    const char *explain_chat =
        (locale->mode_explain_chat != nullptr &&
         locale->mode_explain_chat[0] != '\0')
            ? locale->mode_explain_chat
            : "Chat mode: send messages normally. Prefix commands with %s.";
    const char *explain_command =
        (locale->mode_explain_command != nullptr &&
         locale->mode_explain_command[0] != '\0')
            ? locale->mode_explain_command
            : "Command mode: enter commands without %s, use UpArrow/DownArrow "
              "for history and Tab for completion.";
    const char *already_chat =
        (locale->mode_already_chat != nullptr &&
         locale->mode_already_chat[0] != '\0')
            ? locale->mode_already_chat
            : "Already in chat mode. Commands require the %s prefix.";
    const char *already_command =
        (locale->mode_already_command != nullptr &&
         locale->mode_already_command[0] != '\0')
            ? locale->mode_already_command
            : "Command mode already active. Enter commands without %s.";
    const char *enabled_chat =
        (locale->mode_enabled_chat != nullptr &&
         locale->mode_enabled_chat[0] != '\0')
            ? locale->mode_enabled_chat
            : "Chat mode enabled. Commands once again require the %s prefix.";
    const char *enabled_command =
        (locale->mode_enabled_command != nullptr &&
         locale->mode_enabled_command[0] != '\0')
            ? locale->mode_enabled_command
            : "Command mode enabled. Enter commands without %s; use "
              "UpArrow/DownArrow for history and Tab for completion.";
    const char *usage_format =
        (locale->mode_usage != nullptr && locale->mode_usage[0] != '\0')
            ? locale->mode_usage
            : "Usage: %smode <chat|command|toggle>";

    if (working[0] == '\0') {
        const char *current_label =
            (ctx->input_mode == SESSION_INPUT_MODE_COMMAND) ? command_label
                                                            : chat_label;
        char status_line[128];
        const char *status_args[] = {current_label};
        session_format_template(status_format, status_args,
                                sizeof(status_args) / sizeof(status_args[0]),
                                status_line, sizeof(status_line));
        session_send_system_line(ctx, status_line);

        const char *explain = (ctx->input_mode == SESSION_INPUT_MODE_COMMAND)
                                  ? explain_command
                                  : explain_chat;
        char explain_line[SSH_CHATTER_MESSAGE_LIMIT];
        const char *explain_args[] = {prefix};
        session_format_template(explain, explain_args,
                                sizeof(explain_args) / sizeof(explain_args[0]),
                                explain_line, sizeof(explain_line));
        session_send_system_line(ctx, explain_line);
        return;
    }

    const bool matches_chat = (strcasecmp(working, "chat") == 0) ||
                              (chat_label != nullptr && chat_label[0] != '\0' &&
                               strcmp(working, chat_label) == 0);
    if (matches_chat) {
        if (ctx->input_mode == SESSION_INPUT_MODE_CHAT) {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            const char *args[] = {prefix};
            session_format_template(already_chat, args,
                                    sizeof(args) / sizeof(args[0]), message,
                                    sizeof(message));
            session_send_system_line(ctx, message);
            return;
        }
        ctx->input_mode = SESSION_INPUT_MODE_CHAT;
        session_refresh_input_line(ctx);
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        const char *args[] = {prefix};
        session_format_template(enabled_chat, args,
                                sizeof(args) / sizeof(args[0]), message,
                                sizeof(message));
        session_send_system_line(ctx, message);
        return;
    }

    const bool matches_command =
        (strcasecmp(working, "command") == 0) ||
        (command_label != nullptr && command_label[0] != '\0' &&
         strcmp(working, command_label) == 0);
    if (matches_command) {
        if (ctx->input_mode == SESSION_INPUT_MODE_COMMAND) {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            const char *args[] = {prefix};
            session_format_template(already_command, args,
                                    sizeof(args) / sizeof(args[0]), message,
                                    sizeof(message));
            session_send_system_line(ctx, message);
            return;
        }
        ctx->input_mode = SESSION_INPUT_MODE_COMMAND;
        session_refresh_input_line(ctx);
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        const char *args[] = {prefix};
        session_format_template(enabled_command, args,
                                sizeof(args) / sizeof(args[0]), message,
                                sizeof(message));
        session_send_system_line(ctx, message);
        return;
    }

    if (strcasecmp(working, "toggle") == 0) {
        ctx->input_mode = (ctx->input_mode == SESSION_INPUT_MODE_COMMAND)
                              ? SESSION_INPUT_MODE_CHAT
                              : SESSION_INPUT_MODE_COMMAND;
        session_refresh_input_line(ctx);
        const char *format = (ctx->input_mode == SESSION_INPUT_MODE_COMMAND)
                                 ? enabled_command
                                 : enabled_chat;
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        const char *args[] = {prefix};
        session_format_template(format, args, sizeof(args) / sizeof(args[0]),
                                message, sizeof(message));
        session_send_system_line(ctx, message);
        return;
    }

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    const char *args[] = {prefix};
    session_format_template(usage_format, args, sizeof(args) / sizeof(args[0]),
                            message, sizeof(message));
    session_send_system_line(ctx, message);
}

static void session_handle_history(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    session_clear_screen(ctx);

    size_t command_indices[SSH_CHATTER_INPUT_HISTORY_LIMIT];
    size_t command_count = 0U;
    for (size_t idx = 0U; idx < ctx->input_history_count; ++idx) {
        if (ctx->input_history_is_command[idx]) {
            command_indices[command_count++] = idx;
        }
    }

    if (command_count == 0U) {
        session_send_system_line(ctx, "No command history recorded yet.");
        return;
    }

    size_t limit = command_count;
    if (arguments != nullptr) {
        char working[32];
        snprintf(working, sizeof(working), "%s", arguments);
        trim_whitespace_inplace(working);
        if (working[0] != '\0') {
            char *end = nullptr;
            errno = 0;
            long requested = strtol(working, &end, 10);
            if (errno != 0 || end == working || *end != '\0' ||
                requested <= 0) {
                session_send_system_line(ctx, "Usage: /history [count]");
                return;
            }
            if ((size_t)requested < limit) {
                limit = (size_t)requested;
            }
        }
    }

    session_send_system_line(ctx, "Command history (newest first):");

    for (size_t displayed = 0U; displayed < limit; ++displayed) {
        size_t source_index = command_indices[command_count - 1U - displayed];
        const char *entry = ctx->input_history[source_index];
        if (entry == nullptr || entry[0] == '\0') {
            continue;
        }
        char normalized[SSH_CHATTER_MAX_INPUT_LEN];
        normalized[0] = '\0';
        const char *prefix = session_command_prefix(ctx);
        const char *display_prefix =
            (prefix != nullptr && prefix[0] != '\0') ? prefix : "/";
        size_t prefix_len = strlen(display_prefix);
        bool has_prefix = false;
        if (prefix_len > 0U) {
            has_prefix = strncmp(entry, display_prefix, prefix_len) == 0;
        } else {
            has_prefix = entry[0] == '/';
        }

        if (!has_prefix) {
            snprintf(normalized, sizeof(normalized), "%s%s", display_prefix,
                     entry);
        } else {
            snprintf(normalized, sizeof(normalized), "%s", entry);
        }

        char line[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(line, sizeof(line), "%2zu. %s", displayed + 1U, normalized);
        session_send_system_line(ctx, line);
    }

    session_refresh_input_line(ctx);
}

typedef struct session_weather_buffer {
    char *data;
    size_t length;
} session_weather_buffer_t;

static size_t session_weather_write_callback(void *contents, size_t size,
                                             size_t nmemb, void *userp)
{
    session_weather_buffer_t *buffer = (session_weather_buffer_t *)userp;
    const size_t total = size * nmemb;
    if (buffer == nullptr || total == 0U) {
        return 0U;
    }

    char *resized = sshc_gc_realloc(buffer->data, buffer->length + total + 1U);
    if (resized == nullptr) {
        return 0U;
    }

    buffer->data = resized;
    memcpy(buffer->data + buffer->length, contents, total);
    buffer->length += total;
    buffer->data[buffer->length] = '\0';
    return total;
}

static bool session_fetch_weather_summary(const char *region, const char *city,
                                          char *summary, size_t summary_len)
{
    if (region == nullptr || city == nullptr || summary == nullptr ||
        summary_len == 0U) {
        return false;
    }

    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        return false;
    }

    bool success = false;
    session_weather_buffer_t buffer = {0};
    char query[128];
    snprintf(query, sizeof(query), "%s %s", region, city);

    char *escaped = curl_easy_escape(curl, query, 0);
    if (escaped == nullptr) {
        goto cleanup;
    }

    char url[512];
    static const char *kFormat = "%25l:%20%25C,%20%25t";
    int written = snprintf(url, sizeof(url), "https://wttr.in/%s?format=%s",
                           escaped, kFormat);
    curl_free(escaped);
    if (written < 0 || (size_t)written >= sizeof(url)) {
        goto cleanup;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                     session_weather_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

    CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        goto cleanup;
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status < 200L || status >= 300L || buffer.data == nullptr) {
        goto cleanup;
    }

    char *trimmed = buffer.data;
    while (*trimmed != '\0' && isspace((unsigned char)*trimmed)) {
        ++trimmed;
    }
    size_t end = strlen(trimmed);
    while (end > 0U && isspace((unsigned char)trimmed[end - 1U])) {
        trimmed[--end] = '\0';
    }

    if (trimmed[0] == '\0') {
        goto cleanup;
    }

    snprintf(summary, summary_len, "%s", trimmed);
    success = true;

cleanup:
    curl_easy_cleanup(curl);
    return success;
}

static void session_handle_status(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    static const char *kUsage = "Usage: /status <message|off>";
    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/status", kUsage, usage, sizeof(usage));

    if (arguments == nullptr || *arguments == '\0') {
        if (ctx->status_message[0] == '\0') {
            session_send_system_line(ctx, "You do not have a status message.");
        } else {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message), "Your status: %s",
                     ctx->status_message);
            session_send_system_line(ctx, message);
        }
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    if (strcasecmp(working, "off") == 0 || strcasecmp(working, "clear") == 0) {
        ctx->status_message[0] = '\0';
        session_send_system_line(ctx, "Status message cleared.");
        return;
    }

    char cleaned[SSH_CHATTER_STATUS_LEN];
    if (!user_data_strip_ansi_sequences(working, cleaned, sizeof(cleaned)) ||
        cleaned[0] == '\0') {
        snprintf(cleaned, sizeof(cleaned), "%.*s", SSH_CHATTER_STATUS_LEN - 1,
                 working);
    }
    trim_whitespace_inplace(cleaned);

    if (cleaned[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    bool truncated = strnlen(working, sizeof(working)) >=
                     (SSH_CHATTER_STATUS_LEN - 1U);
    snprintf(ctx->status_message, sizeof(ctx->status_message), "%s", cleaned);

    if (truncated) {
        session_send_system_line(ctx,
                                 "Status message set (truncated to fit).");
    } else {
        session_send_system_line(ctx, "Status message set.");
    }
}

static void session_handle_showstatus(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    static const char *kUsage = "Usage: /showstatus <nickname>";
    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/showstatus", kUsage, usage,
                                 sizeof(usage));

    if (arguments == nullptr || arguments[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    char target_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(target_name, sizeof(target_name), "%s", arguments);
    trim_whitespace_inplace(target_name);

    if (target_name[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    if (ctx->owner == nullptr) {
        session_send_system_line(ctx, "User status lookup is unavailable.");
        return;
    }

    session_ctx_t *target =
        chat_room_find_user(&ctx->owner->room, target_name);
    if (target == nullptr) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "User '%s' is not connected.",
                 target_name);
        session_send_system_line(ctx, message);
        return;
    }

    if (target->status_message[0] == '\0') {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message),
                 "No status message set for '%s'.", target->user.name);
        session_send_system_line(ctx, message);
        return;
    }

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(message, sizeof(message), "Status for %s: %s", target->user.name,
             target->status_message);
    session_send_system_line(ctx, message);
}

static void session_handle_weather(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    static const char *kUsage = "Usage: /weather <region> <city>";
    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/weather", kUsage, usage, sizeof(usage));
    if (arguments == nullptr || *arguments == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    const char *cursor = arguments;
    while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
        ++cursor;
    }

    if (*cursor == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    size_t region_len = (size_t)(cursor - arguments);
    char region[64];
    if (region_len >= sizeof(region)) {
        session_send_system_line(ctx, "Region name is too long.");
        return;
    }
    memcpy(region, arguments, region_len);
    region[region_len] = '\0';
    trim_whitespace_inplace(region);

    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        ++cursor;
    }

    if (*cursor == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    char city[64];
    snprintf(city, sizeof(city), "%s", cursor);
    trim_whitespace_inplace(city);

    if (region[0] == '\0' || city[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    char summary[256];
    if (!session_fetch_weather_summary(region, city, summary,
                                       sizeof(summary))) {
        session_send_system_line(
            ctx,
            "Failed to fetch weather information. Please try again later.");
        return;
    }

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(message, sizeof(message), "%s", summary);
    session_send_system_line(ctx, message);
}

static void session_handle_translate(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    char working[16];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
    }
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, "Usage: /translate <on|off>");
        return;
    }

    if (session_argument_is_disable(working)) {
        ctx->translation_enabled = false;
        ctx->translation_quota_notified = false;
        session_translation_clear_queue(ctx);
        session_send_system_line(ctx,
                                 "Translation disabled. New messages will be "
                                 "delivered without translation.");
        if (ctx->owner != nullptr) {
            host_store_translation_preferences(ctx->owner, ctx);
        }
        return;
    }

    bool enabled = false;
    if (!parse_bool_token(working, &enabled)) {
        if (strcasecmp(working, "enable") == 0 ||
            strcasecmp(working, "enabled") == 0) {
            enabled = true;
        } else {
            session_send_system_line(ctx, "Usage: /translate <on|off>");
            return;
        }
    }

    ctx->translation_enabled = enabled;
    ctx->translation_quota_notified = false;
    if (enabled) {
        session_send_system_line(ctx,
                                 "Translation enabled. Configure directions "
                                 "with /set-trans-lang or /set-target-lang.");
    } else {
        session_translation_clear_queue(ctx);
        session_send_system_line(ctx,
                                 "Translation disabled. New messages will be "
                                 "delivered without translation.");
    }
    if (ctx->owner != nullptr) {
        host_store_translation_preferences(ctx->owner, ctx);
    }
}

static void session_handle_breaking_alerts(session_ctx_t *ctx,
                                           const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    char working[32];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
    }
    trim_whitespace_inplace(working);

    const char *prefix = session_command_prefix(ctx);
    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(usage, sizeof(usage), "Usage: %sbreaking <on|off|toggle>", prefix);

    if (working[0] == '\0') {
        char status[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(status, sizeof(status), "Breaking alerts are currently %s.",
                 ctx->breaking_alerts_enabled ? "ON" : "OFF");
        session_send_system_line(ctx, status);
        session_send_system_line(ctx, usage);
        return;
    }

    bool desired_state = ctx->breaking_alerts_enabled;
    bool recognized = false;

    if (session_argument_is_disable(working)) {
        desired_state = false;
        recognized = true;
    } else if (session_argument_is_enable(working)) {
        desired_state = true;
        recognized = true;
    } else if (strcasecmp(working, "toggle") == 0) {
        desired_state = !ctx->breaking_alerts_enabled;
        recognized = true;
    }

    if (!recognized) {
        session_send_system_line(ctx, usage);
        return;
    }

    ctx->breaking_alerts_enabled = desired_state;
    session_send_system_line(
        ctx, desired_state ? "Breaking alerts enabled."
                           : "Breaking alerts disabled.");

    if (ctx->owner != nullptr) {
        host_store_breaking_alerts(ctx->owner, ctx);
    }
}

static void session_translate_scope_send_usage(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    const char *prefix = session_command_prefix(ctx);
    char *usage_format_head = "Usage: ";
    char *usage_format_tail = "translate-scope <chat|chat-nohistory|all>";

    switch (ctx->ui_language) {
    case SESSION_UI_LANGUAGE_KO:
        usage_format_head = "사용법: ";
        usage_format_tail = "번역범위 <채팅|채팅기록없음|모두>";
        break;
    case SESSION_UI_LANGUAGE_JP:
        usage_format_head = "使い方: ";
        usage_format_tail = "翻訳範囲 <チャット|チャット履歴なし|すべて>";
        break;
    case SESSION_UI_LANGUAGE_ZH:
        usage_format_head = "用法：";
        usage_format_tail = "翻译范围 <聊天|无聊天记录|全部>";
        break;
    case SESSION_UI_LANGUAGE_RU:
        usage_format_head = "Использование: ";
        usage_format_tail = "область-перевода <чат|чат-без-истории|все>";
        break;
    default:
        break;
    }

    char usage_line[SSH_CHATTER_MESSAGE_LIMIT];
    strncat(usage_line, usage_format_head,
            strnlen(usage_format_head, SSH_CHATTER_MESSAGE_LIMIT));

    size_t prefix_len = strnlen(prefix, SSH_CHATTER_MESSAGE_LIMIT);
    if (prefix_len > 0)
        strncat(usage_line, prefix, prefix_len);
    strncat(usage_line, usage_format_tail,
            strnlen(usage_format_tail, SSH_CHATTER_MESSAGE_LIMIT));
    session_send_system_line(ctx, usage_line);
}

static void session_handle_translate_scope(session_ctx_t *ctx,
                                           const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(
            ctx, "Only operators may manage translation scope.");
        return;
    }

    char token[64];
    token[0] = '\0';
    if (arguments != nullptr) {
        const char *cursor = arguments;
        while (*cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }

        size_t length = 0U;
        while (cursor[length] != '\0' &&
               !isspace((unsigned char)cursor[length]) &&
               length + 1U < sizeof(token)) {
            token[length] = cursor[length];
            ++length;
        }
        token[length] = '\0';
    }

    if (token[0] == '\0') {
        const bool limited = translator_should_limit_to_chat_bbs();
        const bool forced = translator_is_ollama_only();
        const bool manual = translator_is_manual_chat_bbs_only();
        const bool skip_scrollback = translator_is_manual_skip_scrollback();

        char status[SSH_CHATTER_MESSAGE_LIMIT];
        if (limited) {
            if (skip_scrollback) {
                snprintf(status, sizeof(status),
                         "Translation scope is currently limited to chat "
                         "messages and "
                         "BBS posts. Scrollback translation is disabled.");
            } else {
                snprintf(status, sizeof(status),
                         "Translation scope is currently limited to chat "
                         "messages and "
                         "BBS posts.");
            }
        } else {
            snprintf(
                status, sizeof(status),
                "Translation scope currently includes system output and bulk "
                "messages.");
        }
        session_send_system_line(ctx, status);

        if (forced) {
            session_send_system_line(
                ctx, "Gemini translation is unavailable; Ollama "
                     "fallback enforces chat/BBS-only scope.");
        } else if (manual) {
            if (skip_scrollback) {
                session_send_system_line(
                    ctx, "Chat/BBS-only scope is enabled manually. "
                         "Scrollback translation is suppressed.");
            } else {
                session_send_system_line(
                    ctx, "Chat/BBS-only scope is enabled manually.");
            }
        }

        session_translate_scope_send_usage(ctx);
        return;
    }

    bool limit_chat_scope = false;
    bool limit_chat_nohistory_scope = false;
    bool restore_full_scope = false;

    if (strcasecmp(token, "chat") == 0 || strcasecmp(token, "limit") == 0 ||
        strcasecmp(token, "on") == 0) {
        limit_chat_scope = true;
    }
    if (strcasecmp(token, "chat-nohistory") == 0 ||
        strcasecmp(token, "chat_nohistory") == 0 ||
        strcasecmp(token, "chat-nohist") == 0) {
        limit_chat_nohistory_scope = true;
    }
    if (strcasecmp(token, "all") == 0 || strcasecmp(token, "full") == 0 ||
        strcasecmp(token, "off") == 0) {
        restore_full_scope = true;
    }

    if (!limit_chat_scope && !limit_chat_nohistory_scope &&
        !restore_full_scope) {
        if (strcmp(token, "채팅") == 0 || strcmp(token, "チャット") == 0 ||
            strcmp(token, "聊天") == 0 || strcmp(token, "чат") == 0) {
            limit_chat_scope = true;
        } else if (strcmp(token, "채팅기록없음") == 0 ||
                   strcmp(token, "チャット履歴なし") == 0 ||
                   strcmp(token, "无聊天记录") == 0 ||
                   strcmp(token, "чат-без-истории") == 0) {
            limit_chat_nohistory_scope = true;
        } else if (strcmp(token, "모두") == 0 || strcmp(token, "すべて") == 0 ||
                   strcmp(token, "全部") == 0 || strcmp(token, "все") == 0 ||
                   strcmp(token, "всё") == 0) {
            restore_full_scope = true;
        }
    }

    if (limit_chat_scope) {
        if (translator_is_manual_chat_bbs_only() &&
            !translator_is_manual_skip_scrollback()) {
            session_send_system_line(ctx,
                                     "Translation scope is already limited to "
                                     "chat messages and BBS posts.");
            return;
        }

        translator_set_manual_chat_bbs_only(true);
        translator_set_manual_skip_scrollback(false);
        session_send_system_line(
            ctx, "Translation scope limited to chat messages and BBS posts.");

        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice),
                 "* [%s] limited translation scope to chat and BBS posts.",
                 ctx->user.name);
        host_history_record_system(ctx->owner, notice, nullptr);
        chat_room_broadcast(&ctx->owner->room, notice, nullptr);
        return;
    }

    if (limit_chat_nohistory_scope) {
        if (translator_is_manual_chat_bbs_only() &&
            translator_is_manual_skip_scrollback()) {
            session_send_system_line(
                ctx,
                "Translation scope is already limited to chat/BBS posts with "
                "scrollback translation disabled.");
            return;
        }

        translator_set_manual_chat_bbs_only(true);
        translator_set_manual_skip_scrollback(true);
        session_send_system_line(
            ctx, "Translation scope limited to chat messages and "
                 "BBS posts. Scrollback translation is disabled.");

        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(
            notice, sizeof(notice),
            "* [%s] limited translation scope to chat/BBS posts and disabled "
            "scrollback translation.",
            ctx->user.name);
        host_history_record_system(ctx->owner, notice, nullptr);
        chat_room_broadcast(&ctx->owner->room, notice, nullptr);
        return;
    }

    if (restore_full_scope) {
        if (translator_is_ollama_only()) {
            session_send_system_line(
                ctx,
                "Full translation scope cannot be restored while Gemini is "
                "unavailable."
                " Ollama-only mode restricts translation to chat and BBS "
                "posts.");
            return;
        }

        if (!translator_is_manual_chat_bbs_only()) {
            session_send_system_line(ctx, "Translation scope already includes "
                                          "system output and bulk messages.");
            return;
        }

        translator_set_manual_chat_bbs_only(false);
        translator_set_manual_skip_scrollback(false);
        session_send_system_line(
            ctx,
            "Full translation scope restored. System output and bulk messages "
            "are eligible for translation.");

        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice),
                 "* [%s] restored full translation scope for translations.",
                 ctx->user.name);
        host_history_record_system(ctx->owner, notice, nullptr);
        chat_room_broadcast(&ctx->owner->room, notice, nullptr);
        return;
    }

    session_translate_scope_send_usage(ctx);
}

static void session_handle_gemini(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(
            ctx, "Only operators may manage Gemini translation.");
        return;
    }

    const char *cursor = arguments;
    while (cursor != nullptr && (*cursor == ' ' || *cursor == '\t')) {
        ++cursor;
    }

    char token[16];
    token[0] = '\0';
    if (cursor != nullptr && *cursor != '\0') {
        size_t length = 0U;
        while (cursor[length] != '\0' &&
               !isspace((unsigned char)cursor[length]) &&
               length + 1U < sizeof(token)) {
            token[length] = cursor[length];
            ++length;
        }
        token[length] = '\0';
    }

    if (token[0] == '\0') {
        bool enabled = translator_is_gemini_enabled();
        bool manual = translator_is_gemini_manually_disabled();
        struct timespec remaining = {0, 0};
        bool cooldown_active = translator_gemini_backoff_remaining(&remaining);

        char status_line[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(status_line, sizeof(status_line),
                 "Gemini translation is currently %s.",
                 enabled ? "enabled" : "disabled");
        session_send_system_line(ctx, status_line);

        if (manual) {
            session_send_system_line(ctx, "Gemini usage is manually disabled. "
                                          "Use /gemini on to re-enable it.");
        }

        if (cooldown_active) {
            long long seconds = remaining.tv_sec;
            if (remaining.tv_nsec > 0L) {
                ++seconds;
            }
            long long hours = seconds / 3600LL;
            long long minutes = (seconds % 3600LL) / 60LL;
            long long secs = seconds % 60LL;

            char cooldown_line[SSH_CHATTER_MESSAGE_LIMIT];
            if (hours > 0) {
                snprintf(cooldown_line, sizeof(cooldown_line),
                         "Automatic Gemini cooldown ends in %lldh %lldm %llds.",
                         hours, minutes, secs);
            } else if (minutes > 0) {
                snprintf(cooldown_line, sizeof(cooldown_line),
                         "Automatic Gemini cooldown ends in %lldm %llds.",
                         minutes, secs);
            } else {
                snprintf(cooldown_line, sizeof(cooldown_line),
                         "Automatic Gemini cooldown ends in %lld seconds.",
                         secs > 0 ? secs : 1LL);
            }
            session_send_system_line(ctx, cooldown_line);
        }

        session_send_system_line(ctx, "Usage: /gemini <on|off>");
        session_send_system_line(
            ctx,
            "Use /gemini-unfreeze to clear the automatic cooldown manually.");
        return;
    }

    bool requested_enable = false;
    bool recognized = false;
    if (session_argument_is_disable(token)) {
        recognized = true;
        requested_enable = false;
    } else {
        recognized = parse_bool_token(token, &requested_enable);
    }

    if (!recognized) {
        session_send_system_line(ctx, "Usage: /gemini <on|off>");
        return;
    }

    if (requested_enable) {
        translator_set_gemini_enabled(true);
        session_send_system_line(
            ctx,
            "Gemini translation enabled. Ollama fallback remains available.");

        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice),
                 "* [%s] enabled Gemini translation; Ollama fallback remains "
                 "available.",
                 ctx->user.name);
        host_history_record_system(ctx->owner, notice, nullptr);
        chat_room_broadcast(&ctx->owner->room, notice, nullptr);
        return;
    }

    translator_set_gemini_enabled(false);
    session_send_system_line(
        ctx, "Gemini translation disabled. Using Ollama gemma2:2b only.");
    session_send_system_line(ctx, "While Gemini is off, only chat messages and "
                                  "BBS posts will be translated.");

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice),
             "* [%s] disabled Gemini translation; using Ollama fallback only "
             "(chat and BBS posts).",
             ctx->user.name);
    host_history_record_system(ctx->owner, notice, nullptr);
    chat_room_broadcast(&ctx->owner->room, notice, nullptr);
    return;
}

static void session_handle_captcha(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(
            ctx, "Only operators may control captcha requirements.");
        return;
    }

    char token[16];
    if (arguments != nullptr) {
        snprintf(token, sizeof(token), "%s", arguments);
        trim_whitespace_inplace(token);
    } else {
        token[0] = '\0';
    }

    host_t *host = ctx->owner;
    if (token[0] == '\0') {
        bool enabled = atomic_load(&host->captcha_enabled);
        char status[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(status, sizeof(status), "Captcha is currently %s.",
                 enabled ? "enabled" : "disabled");
        session_send_system_line(ctx, status);
        session_send_system_line(ctx, "Usage: /captcha <on|off>");
        return;
    }

    bool requested_enable = false;
    bool recognized = false;
    if (session_argument_is_disable(token)) {
        recognized = true;
        requested_enable = false;
    } else {
        recognized = parse_bool_token(token, &requested_enable);
    }

    if (!recognized) {
        session_send_system_line(ctx, "Usage: /captcha <on|off>");
        return;
    }

    if (requested_enable) {
        bool was_enabled = atomic_exchange(&host->captcha_enabled, true);
        if (was_enabled) {
            session_send_system_line(ctx, "Captcha is already enabled.");
        } else {
            session_send_system_line(
                ctx, "Captcha enabled. New connections must solve the puzzle.");
            char notice[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(notice, sizeof(notice),
                     "* [%s] enabled captcha for new connections.",
                     ctx->user.name);
            host_history_record_system(host, notice, nullptr);
            chat_room_broadcast(&host->room, notice, nullptr);
            ttak_mutex_lock(&host->lock);
            host_state_save_locked(host);
            ttak_mutex_unlock(&host->lock);
        }
        return;
    }

    bool was_enabled = atomic_exchange(&host->captcha_enabled, false);
    if (!was_enabled) {
        session_send_system_line(ctx, "Captcha is already disabled.");
    } else {
        session_send_system_line(
            ctx, "Captcha disabled. New connections will skip the puzzle.");
        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice),
                 "* [%s] disabled captcha for new connections.",
                 ctx->user.name);
        host_history_record_system(host, notice, nullptr);
        chat_room_broadcast(&host->room, notice, nullptr);
        ttak_mutex_lock(&host->lock);
        host_state_save_locked(host);
        ttak_mutex_unlock(&host->lock);
    }
    return;
}

static void session_handle_geo_language(session_ctx_t *ctx,
                                        const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(ctx,
                                 "Only operators may control geo language.");
        return;
    }

    char token[32];
    if (arguments != nullptr) {
        snprintf(token, sizeof(token), "%s", arguments);
        trim_whitespace_inplace(token);
    } else {
        token[0] = '\0';
    }

    host_t *host = ctx->owner;
    if (token[0] == '\0') {
        bool enabled = atomic_load(&host->geo_language_enabled);
        char status[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(status, sizeof(status),
                 "Geo-IP UI language detection is currently %s.",
                 enabled ? "enabled" : "disabled");
        session_send_system_line(ctx, status);
        session_send_system_line(ctx, "Usage: /geo <on|off>");
        return;
    }

    bool requested_enable = false;
    bool recognized = false;
    if (session_argument_is_disable(token)) {
        recognized = true;
        requested_enable = false;
    } else {
        recognized = parse_bool_token(token, &requested_enable);
    }

    if (!recognized) {
        session_send_system_line(ctx, "Usage: /geo <on|off>");
        return;
    }

    bool was_enabled =
        atomic_exchange(&host->geo_language_enabled, requested_enable);
    if (requested_enable) {
        if (was_enabled) {
            session_send_system_line(ctx, "Geo-IP UI language is already on.");
            return;
        }

        session_send_system_line(
            ctx, "Geo-IP UI language detection enabled for new connections.");
        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice),
                 "* [%s] enabled Geo-IP UI language detection.",
                 ctx->user.name);
        host_history_record_system(host, notice, nullptr);
        chat_room_broadcast(&host->room, notice, nullptr);
        ttak_mutex_lock(&host->lock);
        host_state_save_locked(host);
        ttak_mutex_unlock(&host->lock);
        return;
    }

    if (!was_enabled) {
        session_send_system_line(ctx,
                                 "Geo-IP UI language is already disabled.");
        return;
    }

    session_send_system_line(
        ctx, "Geo-IP UI language detection disabled; defaulting to English.");
    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice),
             "* [%s] disabled Geo-IP UI language detection (English default).",
             ctx->user.name);
    host_history_record_system(host, notice, nullptr);
    chat_room_broadcast(&host->room, notice, nullptr);
    ttak_mutex_lock(&host->lock);
    host_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);
}

static void session_handle_eliza(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(ctx, "Only operators may control eliza.");
        return;
    }

    char token[32];
    if (arguments != nullptr) {
        snprintf(token, sizeof(token), "%s", arguments);
        trim_whitespace_inplace(token);
    } else {
        token[0] = '\0';
    }

    if (token[0] == '\0') {
        bool enabled = atomic_load(&ctx->owner->eliza_enabled);
        char status[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(status, sizeof(status), "eliza is currently %s.",
                 enabled ? "enabled" : "disabled");
        session_send_system_line(ctx, status);
        session_send_system_line(ctx, "Usage: /eliza <on|off>");
        return;
    }

    bool requested_enable = false;
    bool recognized = false;
    if (session_argument_is_disable(token)) {
        recognized = true;
        requested_enable = false;
    } else {
        recognized = parse_bool_token(token, &requested_enable);
    }

    if (!recognized) {
        session_send_system_line(ctx, "Usage: /eliza <on|off>");
        return;
    }

    if (requested_enable) {
        if (host_eliza_enable(ctx->owner)) {
            session_send_system_line(ctx,
                                     "eliza enabled. She will now mingle with "
                                     "the room and watch for severe issues.");
        } else {
            session_send_system_line(ctx, "eliza is already active.");
        }
        return;
    }

    if (host_eliza_disable(ctx->owner)) {
        session_send_system_line(ctx, "eliza disabled.");
    } else {
        session_send_system_line(ctx, "eliza is already inactive.");
    }
    return;
}

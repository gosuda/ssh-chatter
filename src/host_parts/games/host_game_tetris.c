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
        session_game_toggle_camouflage(ctx);
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
        /* The rows above shifted down into position `row`.
         * Decrement so the for-loop's ++row re-checks the same row. */
        --row;
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

    // Clear scrollback buffer and visible screen so the game state is hidden
    // before rendering camouflage output.
    static const char kClearScrollback[] = "\033[3J";
    session_channel_write(ctx, kClearScrollback, sizeof(kClearScrollback) - 1U);

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

#define SSH_CHATTER_TETRIS_FRAME_BOARD_FIRST_ROW 4U
#define SSH_CHATTER_TETRIS_FRAME_BOARD_FIRST_COL 2U
#define SSH_CHATTER_TETRIS_FRAME_BOTTOM_BORDER_ROW \
    (SSH_CHATTER_TETRIS_FRAME_BOARD_FIRST_ROW + SSH_CHATTER_TETRIS_HEIGHT)
#define SSH_CHATTER_TETRIS_FRAME_HEADER_ROW \
    (SSH_CHATTER_TETRIS_FRAME_BOTTOM_BORDER_ROW + 1U)
#define SSH_CHATTER_TETRIS_FRAME_CONTROLS_ROW \
    (SSH_CHATTER_TETRIS_FRAME_BOTTOM_BORDER_ROW + 2U)

static void session_game_tetris_snapshot_hud(session_ctx_t *ctx,
                                             const tetris_game_state_t *state)
{
    if (ctx == nullptr || state == nullptr) {
        return;
    }

    ctx->game.tetris_prev_score = state->score;
    ctx->game.tetris_prev_lines_cleared = state->lines_cleared;
    ctx->game.tetris_prev_round = state->round;
    ctx->game.tetris_prev_next_piece = state->next_piece;
    ctx->game.tetris_prev_game_over = state->game_over;
    ctx->game.tetris_prev_hud_valid = true;
}

static bool session_game_tetris_hud_changed(session_ctx_t *ctx,
                                            const tetris_game_state_t *state)
{
    if (ctx == nullptr || state == nullptr || !ctx->game.tetris_prev_hud_valid) {
        return true;
    }

    return ctx->game.tetris_prev_score != state->score ||
           ctx->game.tetris_prev_lines_cleared != state->lines_cleared ||
           ctx->game.tetris_prev_round != state->round ||
           ctx->game.tetris_prev_next_piece != state->next_piece ||
           ctx->game.tetris_prev_game_over != state->game_over;
}

static void session_game_tetris_write_themed_at(session_ctx_t *ctx,
                                                unsigned int row,
                                                unsigned int column,
                                                const char *render_source)
{
    if (ctx == nullptr || render_source == nullptr) {
        return;
    }

    char move[32];
    int written =
        snprintf(move, sizeof(move), "\033[%u;%uH", row, column);
    if (written > 0 && (size_t)written < sizeof(move)) {
        session_channel_write(ctx, move, (size_t)written);
    }

    char themed[SSH_CHATTER_MESSAGE_LIMIT * 4U];
    size_t themed_len = session_prepare_themed_output(
        ctx, render_source, themed, sizeof(themed));
    if (themed_len > 0U) {
        session_channel_write(ctx, themed, themed_len);
    }
}

static void session_game_tetris_build_board_line(const tetris_game_state_t *state,
                                                 int row,
                                                 char *line_buffer,
                                                 size_t line_cap)
{
    if (state == nullptr || line_buffer == nullptr || line_cap == 0U) {
        return;
    }

    size_t line_offset = 0U;
    line_offset +=
        (size_t)snprintf(line_buffer + line_offset, line_cap - line_offset,
                         "%s|%s", ANSI_BRIGHT_BLACK, ANSI_RESET);
    for (int col = 0; col < SSH_CHATTER_TETRIS_WIDTH; ++col) {
        if (line_offset >= line_cap - 1U) {
            break;
        }
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
                                                  state->rotation, local_row,
                                                  local_col)) {
                cell = TETROMINO_DISPLAY_CHARS[state->current_piece];
                color = TETROMINO_COLOR_CODES[state->current_piece % 7];
            }
        }
        if (color[0] != '\0') {
            line_offset += (size_t)snprintf(line_buffer + line_offset,
                                            line_cap - line_offset, "%s%c%s",
                                            color, cell, ANSI_RESET);
        } else if (line_offset + 1U < line_cap) {
            line_buffer[line_offset++] = cell;
            line_buffer[line_offset] = '\0';
        }
    }
    if (line_offset < line_cap) {
        (void)snprintf(line_buffer + line_offset, line_cap - line_offset,
                       "%s|%s", ANSI_BRIGHT_BLACK, ANSI_RESET);
    }
}

static void session_game_tetris_clear_and_fill_line(session_ctx_t *ctx,
                                                     unsigned int row)
{
    if (ctx == nullptr) {
        return;
    }

    char move[32];
    int written = snprintf(move, sizeof(move), "\033[%u;%uH", row, 1U);
    if (written > 0 && (size_t)written < sizeof(move)) {
        session_channel_write(ctx, move, (size_t)written);
    }

    static const char kClearLine[] = "\r" ANSI_CLEAR_LINE;
    session_channel_write(ctx, kClearLine, sizeof(kClearLine) - 1U);
    session_fill_line_with_theme(ctx);
}

static bool session_game_tetris_capture_visible_cells(session_ctx_t *ctx,
                                                       uint8_t out_cells
                                                            [SSH_CHATTER_TETRIS_HEIGHT]
                                                            [SSH_CHATTER_TETRIS_WIDTH])
{
    if (ctx == nullptr || out_cells == nullptr || ctx->game.tetris == nullptr) {
        return false;
    }

    const tetris_game_state_t *state = ctx->game.tetris;
    for (int row = 0; row < SSH_CHATTER_TETRIS_HEIGHT; ++row) {
        for (int col = 0; col < SSH_CHATTER_TETRIS_WIDTH; ++col) {
            uint8_t cell = 0U;
            if (state->board[row][col] != 0) {
                int index = state->board[row][col] - 1;
                if (index < 0 || index >= 7) {
                    index = 0;
                }
                cell = (uint8_t)(index + 1);
            } else if (!state->game_over && state->current_piece >= 0) {
                int local_row = row - state->row;
                int local_col = col - state->column;
                if (local_row >= 0 && local_row < SSH_CHATTER_TETROMINO_SIZE &&
                    local_col >= 0 && local_col < SSH_CHATTER_TETROMINO_SIZE &&
                    session_game_tetris_cell_occupied(state->current_piece,
                                                      state->rotation, local_row,
                                                      local_col)) {
                    cell = (uint8_t)(state->current_piece + 1);
                }
            }
            out_cells[row][col] = cell;
        }
    }

    return true;
}

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
    const char controls_line[] =
        ANSI_BRIGHT_CYAN "Controls:" ANSI_RESET
        " left, right, down, Ctrl+R or up: rotate, drop. Blank line = down.";

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
    char border_render[64];
    snprintf(border_render, sizeof(border_render), "%s%s%s", ANSI_BRIGHT_BLACK,
             border, ANSI_RESET);
    offset += (size_t)snprintf(buffer + offset,
                               SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE - offset,
                               "%s\n", border_render);

    for (int row = 0; row < SSH_CHATTER_TETRIS_HEIGHT; ++row) {
        /* Worst case: border(10) + 15 cells * (color5 + char1 + reset4) + border(10) + NUL(1) = 171 bytes */
        char line_buffer[SSH_CHATTER_TETRIS_WIDTH * 12 + 16];
        session_game_tetris_build_board_line(state, row, line_buffer,
                                             sizeof(line_buffer));
        offset += (size_t)snprintf(
            buffer + offset, SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE - offset,
            "%s\n", line_buffer);
    }

    offset += (size_t)snprintf(buffer + offset,
                               SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE - offset,
                               "%s\n", border_render);
    offset += (size_t)snprintf(buffer + offset,
                               SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE - offset,
                               "%s\n", header);
    offset += (size_t)snprintf(buffer + offset,
                               SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE - offset,
                               "%s\n", controls_line);

    // Force a full clear+redraw for the very first render (game entry or
    // locker return), then switch to deterministic cell-level updates.
    const bool force_full_redraw = (ctx->game.tetris_render_count < 1U);
    uint8_t visible_cells[SSH_CHATTER_TETRIS_HEIGHT][SSH_CHATTER_TETRIS_WIDTH];
    bool has_visible_cells =
        session_game_tetris_capture_visible_cells(ctx, visible_cells);
    bool board_changed = true;
    if (!force_full_redraw && has_visible_cells &&
        ctx->game.tetris_prev_cells_valid) {
        board_changed = memcmp(visible_cells, ctx->game.tetris_prev_cells,
                               sizeof(visible_cells)) != 0;
    }
    const bool hud_changed = session_game_tetris_hud_changed(ctx, state);
    if (!force_full_redraw && !board_changed && !hud_changed) {
        ctx->translation_suppress_output = previous_translation_suppress;
        ctx->disable_output_dedup = previous_dedup_state;
        return;
    }

    // Enable output buffering to send each frame atomically.
    session_output_buffer_start(ctx);
    static const char kHideCursor[] = "\033[?25l";
    static const char kShowCursor[] = "\033[?25h";
    session_channel_write(ctx, kHideCursor, sizeof(kHideCursor) - 1U);

    if (force_full_redraw) {
        static const char kHomeAndClear[] = "\033[H\033[2J";
        session_channel_write(ctx, kHomeAndClear, sizeof(kHomeAndClear) - 1U);
        session_send_raw_text(ctx, ctx->tetris_screen_buffer);
    } else {
        session_game_tetris_clear_and_fill_line(
            ctx, SSH_CHATTER_TETRIS_FRAME_BOARD_FIRST_ROW - 1U);
        session_game_tetris_write_themed_at(
            ctx, SSH_CHATTER_TETRIS_FRAME_BOARD_FIRST_ROW - 1U, 1U,
            border_render);

        for (int row = 0; row < SSH_CHATTER_TETRIS_HEIGHT; ++row) {
            const unsigned int frame_row =
                SSH_CHATTER_TETRIS_FRAME_BOARD_FIRST_ROW + (unsigned int)row;
            char line_buffer[SSH_CHATTER_TETRIS_WIDTH * 12 + 16];
            session_game_tetris_build_board_line(state, row, line_buffer,
                                                 sizeof(line_buffer));
            session_game_tetris_clear_and_fill_line(ctx, frame_row);
            session_game_tetris_write_themed_at(ctx, frame_row, 1U, line_buffer);
        }

        session_game_tetris_clear_and_fill_line(
            ctx, SSH_CHATTER_TETRIS_FRAME_BOTTOM_BORDER_ROW);
        session_game_tetris_write_themed_at(
            ctx, SSH_CHATTER_TETRIS_FRAME_BOTTOM_BORDER_ROW, 1U, border_render);

        session_game_tetris_clear_and_fill_line(ctx,
                                                SSH_CHATTER_TETRIS_FRAME_HEADER_ROW);
        session_game_tetris_write_themed_at(ctx,
                                            SSH_CHATTER_TETRIS_FRAME_HEADER_ROW,
                                            1U, header);

        session_game_tetris_clear_and_fill_line(
            ctx, SSH_CHATTER_TETRIS_FRAME_CONTROLS_ROW);
        session_game_tetris_write_themed_at(
            ctx, SSH_CHATTER_TETRIS_FRAME_CONTROLS_ROW, 1U, controls_line);
    }
    char footer_move[32];
    int footer_move_written = snprintf(
        footer_move, sizeof(footer_move), "\033[%u;%uH",
        SSH_CHATTER_TETRIS_FRAME_CONTROLS_ROW + 1U, 1U);
    if (footer_move_written > 0 &&
        (size_t)footer_move_written < sizeof(footer_move)) {
        session_channel_write(ctx, footer_move, (size_t)footer_move_written);
    }
    session_channel_write(ctx, kShowCursor, sizeof(kShowCursor) - 1U);
    session_output_buffer_stop(ctx);

    strncpy(ctx->tetris_prev_screen_buffer, ctx->tetris_screen_buffer,
            SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE);
    ctx->tetris_prev_screen_buffer[SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE - 1] =
        '\0';

    if (ctx->game.tetris_render_count < 1U) {
        ctx->game.tetris_render_count++;
    }
    if (has_visible_cells) {
        memcpy(ctx->game.tetris_prev_cells, visible_cells, sizeof(visible_cells));
        ctx->game.tetris_prev_cells_valid = true;
    } else {
        ctx->game.tetris_prev_cells_valid = false;
    }
    session_game_tetris_snapshot_hud(ctx, state);

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
    if (ctx == nullptr) {
        return;
    }
    if (!session_tetris_buffers_acquire(ctx) ||
        session_game_ensure_tetris(ctx) == nullptr ||
        session_game_ensure_saved_tetris(ctx) == nullptr) {
        session_send_system_line(ctx, "Unable to allocate Tetris resources.");
        return;
    }

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
    ctx->game.tetris_render_count = 0U;
    memset(ctx->game.tetris_prev_cells, 0, sizeof(ctx->game.tetris_prev_cells));
    ctx->game.tetris_prev_cells_valid = false;

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

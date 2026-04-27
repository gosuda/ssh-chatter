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

    // Umul-gonu: well grid thickened with inner walls and cross-cut diagonals
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

    // Handle camouflage toggle with 't' command
    if (strcmp(working, "t") == 0) {
        session_game_toggle_camouflage(ctx);
        return true;
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
        ctx, "3. Umul-gonu (Well) - thickened well grid with diagonals");
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
    session_manual_gc_tick(ctx);
    host_manual_gc_tick(ctx->owner);
}

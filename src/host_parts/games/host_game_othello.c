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
        slot_id > (int)host->othello_slot_limit) {
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

    for (size_t idx = 0U; idx < host->othello_slot_limit; ++idx) {
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
        if (idx < 64U) {
            host->othello_slot_mask |= (1ULL << idx);
        }
        return slot;
    }

    return nullptr;
}

static bool host_othello_queue_push_locked(host_t *host, session_ctx_t *ctx)
{
    if (host == nullptr || ctx == nullptr ||
        host->othello_wait_queue_count >= SSH_CHATTER_OTHELLO_MAX_WAIT_QUEUE) {
        return false;
    }
    for (size_t idx = 0U; idx < host->othello_wait_queue_count; ++idx) {
        size_t pos =
            (host->othello_wait_queue_head + idx) % SSH_CHATTER_OTHELLO_MAX_WAIT_QUEUE;
        if (host->othello_wait_queue[pos] == ctx) {
            return true;
        }
    }
    host->othello_wait_queue[host->othello_wait_queue_tail] = ctx;
    host->othello_wait_queue_tail =
        (host->othello_wait_queue_tail + 1U) % SSH_CHATTER_OTHELLO_MAX_WAIT_QUEUE;
    host->othello_wait_queue_count++;
    return true;
}

static void host_othello_try_promote_queue_locked(host_t *host)
{
    if (host == nullptr || host->othello_wait_queue_count == 0U) {
        return;
    }

    while (host->othello_wait_queue_count > 0U) {
        session_ctx_t *queued = host->othello_wait_queue[host->othello_wait_queue_head];
        host->othello_wait_queue[host->othello_wait_queue_head] = nullptr;
        host->othello_wait_queue_head =
            (host->othello_wait_queue_head + 1U) % SSH_CHATTER_OTHELLO_MAX_WAIT_QUEUE;
        host->othello_wait_queue_count--;

        if (queued == nullptr || queued->owner != host || queued->game.active) {
            continue;
        }

        othello_multiplayer_slot_t *slot =
            host_othello_allocate_slot_locked(host, queued->user.name);
        if (slot == nullptr) {
            break;
        }
        slot->players[0] = queued;
        queued->othello_slot_queued = false;
        queued->game.active = true;
        queued->game.type = SESSION_GAME_OTHELLO;
        queued->game.is_camouflaged = false;
        othello_game_state_t *state = &queued->game.othello;
        session_game_othello_copy_core(state, &slot->state);
        state->awaiting_mode_selection = false;
        state->multiplayer = true;
        state->awaiting_opponent = true;
        state->slot_index = (int)slot->slot_id;
        state->player_number = 0U;
        state->player_turn = false;
        state->game_over = false;

        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message),
                 "Queued match activated. You are now waiting in game #%u.",
                 slot->slot_id);
        session_send_system_line(queued, message);
        session_game_othello_render(queued);
        session_game_othello_prepare_next_turn(queued);
        break;
    }
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
    if (slot->slot_id > 0U && slot->slot_id <= 64U) {
        host->othello_slot_mask &= ~(1ULL << (slot->slot_id - 1U));
    }
    host_othello_try_promote_queue_locked(host);
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
        int opp_precision = 0;
        const size_t opp_suffix_len = sizeof(" resigned.") - 1U;
        if (sizeof(opp_reason) > opp_suffix_len + 1U) {
            opp_precision = (int)(sizeof(opp_reason) - opp_suffix_len - 1U);
        }
        snprintf(self_reason, sizeof(self_reason), "You resigned.");
        if (opponent != nullptr) {
            snprintf(opp_reason, sizeof(opp_reason), "%.*s resigned.",
                     opp_precision, ctx->user.name);
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
    const size_t reason_suffix_len = sizeof(" resigned.") - 1U;
    size_t reason_name_limit = 0U;
    if (sizeof(reason_p1) > reason_suffix_len + 1U) {
        reason_name_limit = sizeof(reason_p1) - reason_suffix_len - 1U;
    }
    const int reason_precision = (int)reason_name_limit;
    if (is_player_one) {
        snprintf(reason_p1, sizeof(reason_p1), "You resigned.");
        if (opponent != nullptr) {
            snprintf(reason_p2, sizeof(reason_p2), "%.*s resigned.",
                     reason_precision, resigner_name);
        } else {
            snprintf(reason_p2, sizeof(reason_p2), "Opponent resigned.");
        }
    } else {
        if (opponent != nullptr) {
            snprintf(reason_p1, sizeof(reason_p1), "%.*s resigned.",
                     reason_precision, resigner_name);
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
            session_clear_screen(ctx);
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
            bool queued_for_slot = false;
            ttak_mutex_lock(&ctx->owner->lock);
            slot =
                host_othello_allocate_slot_locked(ctx->owner, ctx->user.name);
            if (slot != nullptr) {
                slot->players[0] = ctx;
                slot_id = (int)slot->slot_id;
            } else if (ctx->owner->othello_slot_mask ==
                       ((ctx->owner->othello_slot_side_n >= 64U)
                            ? UINT64_MAX
                            : ((1ULL << ctx->owner->othello_slot_side_n) - 1ULL))) {
                queued_for_slot = host_othello_queue_push_locked(ctx->owner, ctx);
                ctx->othello_slot_queued = queued_for_slot;
            }
            ttak_mutex_unlock(&ctx->owner->lock);

            if (slot == nullptr) {
                if (queued_for_slot) {
                    session_send_system_line(
                        ctx,
                        "All multiplayer Othello slots are currently full. "
                        "You have been queued and will be activated automatically.");
                } else {
                    session_send_system_line(
                        ctx, "All multiplayer Othello slots are currently in use.");
                }
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
    size_t in_use_count = 0U;
    for (size_t idx = 0U; idx < ctx->owner->othello_slot_limit; ++idx) {
        othello_multiplayer_slot_t *slot = &ctx->owner->othello_games[idx];
        if (slot->in_use) {
            in_use_count++;
        }
        if (!slot->in_use || slot->active || !slot->awaiting_second_player) {
            continue;
        }
        if (count < ctx->owner->othello_slot_limit) {
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

    char status_line[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(status_line, sizeof(status_line),
             "OLS status: O=%zu L=%zu S=%zu, n=%zu, square=%zu, bitmask=0x%016llX",
             in_use_count, ctx->owner->othello_slot_limit,
             ctx->owner->othello_wait_queue_count, ctx->owner->othello_slot_side_n,
             ctx->owner->othello_slot_limit,
             (unsigned long long)ctx->owner->othello_slot_mask);
    session_send_system_line(ctx, status_line);

    if (count == 0U) {
        session_send_system_line(
            ctx, "No open multiplayer Othello games right now.");
        return;
    }

    session_send_system_line(ctx, "Open multiplayer Othello games:");
    for (size_t idx = 0U; idx < count; ++idx) {
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        if (owners[idx][0] != '\0') {
            snprintf(line, sizeof(line), "  #%d - host: %.*s", ids[idx],
                     SSH_CHATTER_USERNAME_LEN - 1, owners[idx]);
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
            parsed == 0UL ||
            (ctx->owner != nullptr && parsed > ctx->owner->othello_slot_limit)) {
            session_send_system_line(ctx, "Provide a valid game number.");
            return;
        }

        session_othello_accept_game(ctx, (unsigned)parsed);
        return;
    }

    session_send_system_line(ctx, usage);
}

static void

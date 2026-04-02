void session_game_handle_screen_cleared(session_ctx_t *ctx)
{
    if (ctx == nullptr || !ctx->game.active) {
        return;
    }

    if (ctx->game.type != SESSION_GAME_TETRIS) {
        return;
    }

    if (ctx->tetris_screen_buffer == nullptr ||
        ctx->tetris_prev_screen_buffer == nullptr ||
        ctx->game.tetris == nullptr) {
        return;
    }

    // Force the next render to redraw the entire frame.
    ctx->tetris_prev_screen_buffer[0] = '\0';
    ctx->game.tetris_prev_cells_valid = false;
    session_game_tetris_render(ctx);
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


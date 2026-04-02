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
                               sizeof(gravity_line) - offset, "%s%c=%s(u=%.2e)",
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
                idx == state->waypoint_index ? " <- current objective" : "");
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
            ctx, "Route markers: 1-9 mark required waystations; P "
                 "marks the Proxima landing zone.");
        session_send_system_line(ctx,
                                 "Gravitational pulls: B=black hole, S=star, "
                                 "D=debris - each mass tugs with its own u.");
    } else {
        session_send_system_line(
            ctx,
            "Gravitational pulls: B=black hole, S=star, P=planet, D=debris - "
            "each mass tugs with its own u.");
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
    session_send_system_line(ctx, "Navigation grid spans 60x60 sectors; each "
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


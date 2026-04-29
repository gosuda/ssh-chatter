static void host_fix_overlapping_bbs_rss_paths(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (host->bbs_state_file_path[0] == '\0' ||
        host->rss_state_file_path[0] == '\0') {
        return;
    }

    if (strcmp(host->bbs_state_file_path, host->rss_state_file_path) != 0) {
        return;
    }

    const char *fallback = "rss_state.dat";
    int written = snprintf(host->rss_state_file_path,
                           sizeof(host->rss_state_file_path), "%s", fallback);
    if (written < 0 || (size_t)written >= sizeof(host->rss_state_file_path)) {
        host->rss_state_file_path[0] = '\0';
        humanized_log_error("rss",
                            "rss state file path is too long after overlap fix",
                            ENAMETOOLONG);
        return;
    }

    printf("[rss] CHATTER_RSS_FILE matched CHATTER_BBS_FILE. "
           "Using '%s' to keep storage separated.\n",
           host->rss_state_file_path);
}

void host_init(host_t *host, auth_profile_t *auth)
{
    if (host == nullptr) {
        return;
    }

    sshc_memory_context_t *memory_scope =
        sshc_memory_context_push(host->memory_context);

    translator_global_init();

    memset(&host->eliza_worker, 0, sizeof(host->eliza_worker));
    if (!host_eliza_worker_init(host)) {
        printf("[eliza] asynchronous intervention worker unavailable; "
               "interventions disabled.\n");
    }

    if (!host_moderation_init(host)) {
        printf("[security] moderation worker unavailable; using synchronous "
               "checks\n");
    }

    chat_room_init(&host->room);
    atomic_init(&host->next_session_id, 1U);
    host->idle_state_pending = false;
    host->last_room_empty_time = session_now_monotonic();
    host->listener.handle = nullptr;
    host->listener.inplace_recoveries = 0U;
    host->listener.restart_attempts = 0U;
    host->listener.accept_error_streak = 0U;
    host->listener.last_error_time.tv_sec = 0;
    host->listener.last_error_time.tv_nsec = 0L;
    host->telnet.enabled = false;
    host->telnet.fd = -1;
    host->telnet.thread_initialized = false;
    atomic_store(&host->telnet.running, false);
    atomic_store(&host->telnet.stop, false);
    host->telnet.restart_attempts = 0U;
    host->telnet.last_error_time.tv_sec = 0;
    host->telnet.last_error_time.tv_nsec = 0L;
    host->telnet.bind_address[0] = '\0';
    host->telnet.port[0] = '\0';
    host->json_api.enabled = false;
    host->json_api.fd = -1;
    host->json_api.thread_initialized = false;
    atomic_store(&host->json_api.running, false);
    atomic_store(&host->json_api.stop, false);
    host->json_api.restart_attempts = 0U;
    host->json_api.last_error_time.tv_sec = 0;
    host->json_api.last_error_time.tv_nsec = 0L;
    host->json_api.bind_address[0] = '\0';
    host->json_api.port[0] = '\0';
    
    const char *env_secret = getenv("JWT_SECRET");
    if (env_secret && env_secret[0] != '\0') {
        snprintf(host->jwt_secret, sizeof(host->jwt_secret), "%s", env_secret);
    } else {
        snprintf(host->jwt_secret, sizeof(host->jwt_secret), "ssh-chatter-secret-key-change-me");
    }

    host->auth = auth;
    host->clients = nullptr;
    host->web_client = nullptr;
    host->morse_client = nullptr;
    host->security_layer_initialized =
        security_layer_init(&host->security_layer);
    if (!host->security_layer_initialized) {
        humanized_log_error("security",
                            "failed to initialise layered message encryption",
                            errno != 0 ? errno : EIO);
    }
    atomic_store(&host->geo_language_enabled, false);
    host_load_lan_operator_credentials(host);
    const palette_descriptor_t *default_palette =
        palette_find_descriptor("blackpink");
    if (default_palette != nullptr) {
        host_apply_palette_descriptor(host, default_palette);
    } else {
        static char userColor[32];
        static char highlight[32];
        static char bgColor[32];
        static char fgColor[32];
        static char hlColor[32];

        ansi_256(userColor, sizeof(userColor), MONOKAI_BLUE_USER_COLOR);
        ansi_bg_256(highlight, sizeof(highlight), 0);
        ansi_bg_256(bgColor, sizeof(bgColor), MONOKAI_BLUE_BACKGROUND_COLOR);
        ansi_256(fgColor, sizeof(fgColor), MONOKAI_BLUE_FOREGROUND_COLOR);
        ansi_bg_256(hlColor, sizeof(hlColor), MONOKAI_BLUE_HIGHLIGHT_COLOR);

        host->user_theme.userColor = userColor;
        host->user_theme.highlight = highlight;
        host->user_theme.isBold = false;
        host->system_theme.backgroundColor = bgColor;
        host->system_theme.foregroundColor = fgColor;
        host->system_theme.highlightColor = hlColor;
        host->system_theme.isBold = true;
        snprintf(host->default_user_color_name,
                 sizeof(host->default_user_color_name), "%s", "green");
        snprintf(host->default_user_highlight_name,
                 sizeof(host->default_user_highlight_name), "%s", "default");
        snprintf(host->default_system_fg_name,
                 sizeof(host->default_system_fg_name), "%s", "white");
        snprintf(host->default_system_bg_name,
                 sizeof(host->default_system_bg_name), "%s", "blue");
        snprintf(host->default_system_highlight_name,
                 sizeof(host->default_system_highlight_name), "%s", "yellow");
    }
    host->ban_count = 0U;
    memset(host->bans, 0, sizeof(host->bans));
    memset(host->replies, 0, sizeof(host->replies));
    host->reply_count = 0U;
    host->next_reply_id = 1U;
    memset(host->eliza_memory, 0, sizeof(host->eliza_memory));
    host->eliza_memory_count = 0U;
    host->eliza_memory_next_id = 1U;
    host->version_ip_ban_rules = nullptr;
    host->version_ip_ban_rule_count = 0U;
    host->version_ip_ban_rule_capacity = 0U;
    snprintf(host->version, sizeof(host->version),
             "ssh-chatter (C, rolling release)");
    snprintf(host->motd_base, sizeof(host->motd_base),
             "Welcome to ssh-chat!\n"
             "- Be polite to each other\n"
             "- fun fact: this server is written in pure c.\n"
             "============================================\n"
             " _      ____  ____  _____ ____  _        ____  _ \n"
             "/ \\__/|/  _ \\/  _ \\/  __//  __\\/ \\  /|  /   _\\/ \\\n"
             "| |\\/||| / \\|| | \\||  \\  |  \\/|| |\\ ||  |  /  | |\n"
             "| |  ||| \\_/|| |_/||  /_ |    /| | \\||  |  \\__\\_/\n"
             "\\_/  \\|\\____/\\____/\\____\\\\_/\\_\\\\_/  \\|  \\____/(_)\n"
             "                                                 \n"
             "============================================\n");
    snprintf(host->motd, sizeof(host->motd), "%s", host->motd_base);
    host->motd_path[0] = '\0';
    host->motd_has_file = false;
    host->motd_last_modified.tv_sec = 0;
    host->motd_last_modified.tv_nsec = 0L;
    host->welcome_banner[0] = '\0';
    host->welcome_banner_loaded = false;

    host->translation_quota_exhausted = false;
    host->connection_count = 0U;
    host->history = nullptr;
    host->history_count = 0U;
    host->history_capacity = 0U;
    host->history_cache_loaded = false;
    host->next_message_id = 1U;
    memset(host->preferences, 0, sizeof(host->preferences));
    host->preference_count = 0U;
    host->state_file_path[0] = '\0';
    host_state_resolve_path(host);
    host->sync_state_file_path[0] = '\0';
    host_sync_state_resolve_path(host);
    host->wall_state_file_path[0] = '\0';
    host_wall_resolve_path(host);
    host->bbs_state_file_path[0] = '\0';
    host_bbs_resolve_path(host);
    host->vote_state_file_path[0] = '\0';
    host_vote_resolve_path(host);
    host->ban_state_file_path[0] = '\0';
    host_ban_resolve_path(host);
    host->reply_state_file_path[0] = '\0';
    host_reply_state_resolve_path(host);
    host->ui_lang_state_file_path[0] = '\0';
    host_ui_language_state_resolve_path(host);
    host->pw_auth_file_path[0] = '\0';
    host_pw_auth_resolve_path(host);
    host->alpha_landers_file_path[0] = '\0';
    host_alpha_landers_resolve_path(host);
    snprintf(host->user_data_root, sizeof(host->user_data_root), "%s",
             "/var/lib/mailbox");
    host->user_data_ready = user_data_ensure_root(host->user_data_root);
    if (ttak_mutex_init(&host->user_data_lock) == 0) {
        host->user_data_lock_initialized = true;
    } else {
        humanized_log_error("mailbox", "failed to initialise mailbox lock",
                            errno != 0 ? errno : ENOMEM);
        host->user_data_lock_initialized = false;
        host->user_data_ready = false;
    }
    if (ttak_mutex_init(&host->alpha_landers_lock) == 0) {
        host->alpha_landers_lock_initialized = true;
    } else {
        humanized_log_error("alpha", "failed to initialise alpha landers lock",
                            errno != 0 ? errno : ENOMEM);
        host->alpha_landers_lock_initialized = false;
    }
    host->file_storage_ready = host_file_storage_init(host);
    host->rss_state_file_path[0] = '\0';
    host_rss_resolve_path(host);
    host_fix_overlapping_bbs_rss_paths(host);
    host->eliza_memory_file_path[0] = '\0';
    host_eliza_memory_resolve_path(host);
    host->eliza_state_file_path[0] = '\0';
    host_eliza_state_resolve_path(host);
    host->bbs_watchdog_thread_initialized = false;
    atomic_store(&host->bbs_watchdog_thread_running, false);
    atomic_store(&host->bbs_watchdog_thread_stop, false);
    host->bbs_watchdog_last_run.tv_sec = 0;
    host->bbs_watchdog_last_run.tv_nsec = 0;
    host->rss_thread_initialized = false;
    atomic_store(&host->rss_thread_running, false);
    atomic_store(&host->rss_thread_stop, false);
    host->rss_last_run.tv_sec = 0;
    host->rss_last_run.tv_nsec = 0L;
    atomic_store(&host->rss_manual_refresh_running, false);
    host->rss_refresh_lock_initialized = false;
    if (ttak_mutex_init(&host->rss_refresh_lock) == 0) {
        host->rss_refresh_lock_initialized = true;
    } else {
        humanized_log_error("rss", "failed to initialise refresh lock",
                            errno != 0 ? errno : ENOMEM);
    }
    host->archive_thread_initialized = false;
    atomic_store(&host->archive_thread_running, false);
    atomic_store(&host->archive_thread_stop, false);
    host->archive_last_run.tv_sec = 0;
    host->archive_last_run.tv_nsec = 0L;
    host_security_configure(host);
    host_version_ip_rules_init(host);
    memset(host->protected_ips, 0, sizeof(host->protected_ips));
    host->protected_ip_count = 0U;
    ttak_mutex_init(&host->lock);
    host_wall_reset_locked(host);
    host_protected_ips_bootstrap(host);
    poll_state_reset(&host->poll);
    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_NAMED_POLLS; ++idx) {
        named_poll_reset(&host->named_polls[idx]);
    }
    host->named_poll_count = 0U;
    host->bbs_cache_loaded = false;
    if (!host_bbs_acquire_storage(host)) {
        host->bbs_post_capacity = 0U;
    }
    host->bbs_post_count = 0U;
    host->next_bbs_id = 1U;
    for (size_t idx = 0U; idx < SSH_CHATTER_RSS_MAX_FEEDS; ++idx) {
        host_clear_rss_feed(&host->rss_feeds[idx]);
    }
    host->rss_feed_count = 0U;
    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_count < 1L) {
        cpu_count = 2L;
    }
    size_t slot_side_n = (size_t)(cpu_count / 2L);
    if (slot_side_n < 1U) {
        slot_side_n = 1U;
    }
    if (slot_side_n > 32U) {
        slot_side_n = 32U;
    }
    /*
     * Keep process queue concurrency very low so command-processing CPU
     * utilization stays near 10% of total host capacity.
     */
    size_t cpu_slot_limit = 1U;
    if (cpu_count >= 10L) {
        cpu_slot_limit = (size_t)(cpu_count / 10L);
    }
    if (cpu_slot_limit > 64U) {
        cpu_slot_limit = 64U;
    }

    host->cpu_slot_side_n = cpu_slot_limit;
    host->cpu_slot_limit = cpu_slot_limit;
    host->cpu_slot_in_use = 0U;
    host->cpu_slot_waiting = 0U;
    host->cpu_slot_mask = 0ULL;
    host->othello_slot_side_n = slot_side_n;
    host->othello_slot_limit = slot_side_n * slot_side_n;
    if (host->othello_slot_limit > SSH_CHATTER_OTHELLO_MAX_SLOTS) {
        host->othello_slot_limit = SSH_CHATTER_OTHELLO_MAX_SLOTS;
    }
    host->othello_slot_mask = 0ULL;
    memset(host->othello_wait_queue, 0, sizeof(host->othello_wait_queue));
    host->othello_wait_queue_head = 0U;
    host->othello_wait_queue_tail = 0U;
    host->othello_wait_queue_count = 0U;
    for (size_t idx = 0U; idx < SSH_CHATTER_OTHELLO_MAX_SLOTS; ++idx) {
        othello_multiplayer_slot_t *slot = &host->othello_games[idx];
        slot->in_use = false;
        slot->active = false;
        slot->awaiting_second_player = false;
        slot->slot_id = (uint16_t)(idx + 1U);
        slot->owner[0] = '\0';
        slot->players[0] = nullptr;
        slot->players[1] = nullptr;
        slot->state = (othello_game_state_t){0};
        slot->state.slot_index = -1;
    }
    host->random_seeded = false;
    memset(host->operator_grants, 0, sizeof(host->operator_grants));
    host->operator_grant_count = 0U;
    host->reserved_nicknames = nullptr;
    host->reserved_nicknames_len = 0U;
    host->reserved_nicknames_capacity = 0U;
    host->nickname_claim_pool = nullptr;
    memset(host->nickname_claims, 0, sizeof(host->nickname_claims));
    host->nickname_claim_count = 0U;
    host->next_join_ready_time = (struct timespec){0, 0};
    host->join_throttle_initialised = false;
    host->join_progress_length = 0U;
    host->join_activity = nullptr;
    host->join_activity_count = 0U;
    host->join_activity_capacity = 0U;
    host->connection_guard = nullptr;
    host->connection_guard_count = 0U;
    host->connection_guard_capacity = 0U;
    host->health_guard.consecutive_errors = 0U;
    host->health_guard.last_error_time.tv_sec = 0;
    host->health_guard.last_error_time.tv_nsec = 0L;
    host->force_restart_requested = false;
    atomic_store(&host->captcha_enabled, false);
    host->captcha_nonce = 0U;
    host->has_last_captcha = false;
    host->last_captcha_question[0] = '\0';
    host->last_captcha_answer[0] = '\0';
    host->last_captcha_generated.tv_sec = 0;
    host->last_captcha_generated.tv_nsec = 0L;
    host->reserved_nicknames_capacity = 0U;
    if (ttak_mutex_init(&host->nickname_reserve_lock) != 0) {
        humanized_log_error("host", "failed to initialise nickname lock",
                            errno != 0 ? errno : ENOMEM);
    }
    host->nickname_claim_pool = ttak_object_pool_create(
        SSH_CHATTER_MAX_NICKNAME_CLAIMS, sizeof(nickname_claim_t));
    if (host->nickname_claim_pool == nullptr) {
        humanized_log_error("host", "failed to create nickname claim pool",
                            errno != 0 ? errno : ENOMEM);
    }
    atomic_store(&host->eliza_enabled, false);
    atomic_store(&host->eliza_announced, false);
    host->eliza_last_action.tv_sec = 0;
    host->eliza_last_action.tv_nsec = 0L;
    atomic_store(&host->ai_chat_enabled, true);
    host->ai_chat_use_gemini = false;
    host->ai_chat_last_reply.tv_sec = 0;
    host->ai_chat_last_reply.tv_nsec = 0L;
    host->ai_chat_model[0] = '\0';
    memset(host->ai_chat_memory, 0, sizeof(host->ai_chat_memory));
    host->ai_chat_memory_count = 0U;
    (void)host_try_load_motd_from_path(host, "/etc/ssh-chatter/motd");

    host_state_load(host);
    host->history_cache_loaded =
        host->history != nullptr && host->history_capacity > 0U;
    host_wall_state_load(host);
    host_ui_language_state_load(host);
    host_vote_state_load(host);
    host_ban_state_load(host);
    host_reply_state_load(host);
    host_rss_state_load(host);
    host_eliza_memory_load(host);
    host_eliza_state_load(host);

    host_user_data_bootstrap(host);

    host_refresh_motd(host);

    host->clients = client_manager_create(host);
    if (host->clients == nullptr) {
        humanized_log_error("host", "failed to create client manager", ENOMEM);
    } else {
        host->web_client = webssh_client_create(host, host->clients);
        if (host->web_client == nullptr) {
            humanized_log_error("host", "failed to initialise webssh client",
                                ENOMEM);
        }

    }
    if (host->morse_client == nullptr) {
        host->morse_client = morse_client_create(host);
        if (host->morse_client == nullptr) {
            printf("[morse] relay inactive; connection will not start.\n");
        }
    }
    host_bbs_start_watchdog(host);
    host_rss_start_backend(host);
    host_archive_start_backend(host);
    sshc_memory_context_pop(memory_scope);
}

static void host_build_birthday_notice_locked(host_t *host, char *line,
                                              size_t length)
{
    if (line == nullptr || length == 0U) {
        return;
    }

    line[0] = '\0';

    if (host == nullptr) {
        return;
    }

    time_t now = time(nullptr);
    if (now == (time_t)-1) {
        return;
    }

    struct tm local_now;
    if (localtime_r(&now, &local_now) == nullptr) {
        return;
    }

    struct tm today_tm = local_now;
    today_tm.tm_hour = 0;
    today_tm.tm_min = 0;
    today_tm.tm_sec = 0;
    today_tm.tm_isdst = -1;
    time_t today = mktime(&today_tm);
    if (today == (time_t)-1) {
        today = now;
    }

    char names[SSH_CHATTER_MESSAGE_LIMIT];
    names[0] = '\0';
    size_t name_count = 0U;

    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_PREFERENCES; ++idx) {
        const user_preference_t *pref = &host->preferences[idx];
        if (!pref->in_use || !pref->has_birthday) {
            continue;
        }
        if (pref->username[0] == '\0' || pref->birthday[0] == '\0') {
            continue;
        }

        int month = 0;
        int day = 0;
        if (sscanf(pref->birthday, "%*d-%d-%d", &month, &day) != 2) {
            continue;
        }
        if (month < 1 || month > 12 || day < 1 || day > 31) {
            continue;
        }

        int use_day = day;
        int use_month = month;
        const int current_year = local_now.tm_year + 1900;
        if (use_month == 2 && use_day == 29 &&
            !host_is_leap_year(current_year)) {
            use_day = 28;
        }

        struct tm birthday_tm = today_tm;
        birthday_tm.tm_year = local_now.tm_year;
        birthday_tm.tm_mon = use_month - 1;
        birthday_tm.tm_mday = use_day;
        birthday_tm.tm_hour = 0;
        birthday_tm.tm_min = 0;
        birthday_tm.tm_sec = 0;
        birthday_tm.tm_isdst = -1;
        time_t birthday_time = mktime(&birthday_tm);
        if (birthday_time == (time_t)-1) {
            continue;
        }

        time_t diff = today - birthday_time;
        if (diff < 0) {
            birthday_tm.tm_year -= 1;
            birthday_tm.tm_isdst = -1;
            birthday_time = mktime(&birthday_tm);
            if (birthday_time == (time_t)-1) {
                continue;
            }
            diff = today - birthday_time;
        }

        if (diff < 0 || diff >= (time_t)SSH_CHATTER_BIRTHDAY_WINDOW_SECONDS) {
            continue;
        }

        size_t current_len = strnlen(names, sizeof(names));
        const size_t name_len = strnlen(pref->username, sizeof(pref->username));
        if (name_len == 0U) {
            continue;
        }

        if (current_len > 0U) {
            if (current_len + 2U >= sizeof(names)) {
                continue;
            }
            names[current_len++] = ',';
            names[current_len++] = ' ';
            names[current_len] = '\0';
        }

        if (name_len >= sizeof(names) - current_len) {
            continue;
        }

        memcpy(names + current_len, pref->username, name_len);
        current_len += name_len;
        names[current_len] = '\0';
        ++name_count;
    }

    if (name_count == 0U) {
        return;
    }

    snprintf(line, length, "Happy birthday to %s!\n", names);
}

static void host_refresh_motd_locked(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    char birthday_line[SSH_CHATTER_MESSAGE_LIMIT];
    host_build_birthday_notice_locked(host, birthday_line,
                                      sizeof(birthday_line));

    if (birthday_line[0] != '\0') {
        snprintf(host->motd, sizeof(host->motd), "%s%s", birthday_line,
                 host->motd_base);
    } else {
        snprintf(host->motd, sizeof(host->motd), "%s", host->motd_base);
    }
}

static void host_refresh_motd(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    host_maybe_reload_motd_from_file(host);

    ttak_mutex_lock(&host->lock);
    host_refresh_motd_locked(host);
    ttak_mutex_unlock(&host->lock);
}

static bool host_try_load_motd_from_path(host_t *host, const char *path)
{
    if (host == nullptr || path == nullptr || path[0] == '\0') {
        return false;
    }

    FILE *motd_file = fopen(path, "r");
    if (motd_file == nullptr) {
        return false;
    }

    struct stat info;
    bool have_info = false;
    struct timespec modified = {0, 0};
    if (fstat(fileno(motd_file), &info) == 0) {
        modified = host_stat_mtime(&info);
        if (modified.tv_sec != 0 || modified.tv_nsec != 0) {
            have_info = true;
        }
    }

    char motd_buffer[sizeof(host->motd)];
    motd_buffer[0] = '\0';
    size_t current_motd_len = 0U;
    char line_buffer[SSH_CHATTER_MESSAGE_LIMIT];

    while (fgets(line_buffer, sizeof(line_buffer), motd_file) != nullptr) {
        size_t line_len = strlen(line_buffer);
        while (line_len > 0U && (line_buffer[line_len - 1U] == '\n' ||
                                 line_buffer[line_len - 1U] == '\r')) {
            line_buffer[--line_len] = '\0';
        }

        // Prepend [200~ and append [201~ for bracketed paste mode
        const char *start_paste = "\033[200~";
        const char *end_paste = "\033[201~";
        size_t start_paste_len = strlen(start_paste);
        size_t end_paste_len = strlen(end_paste);

        size_t required_len =
            start_paste_len + line_len + end_paste_len + 1U; // +1 for newline

        if (current_motd_len + required_len + 1U >=
            sizeof(motd_buffer)) { // +1 for null terminator
            // MOTD buffer is full, stop reading
            break;
        }

        // Append start_paste
        memcpy(motd_buffer + current_motd_len, start_paste, start_paste_len);
        current_motd_len += start_paste_len;

        // Append line content
        memcpy(motd_buffer + current_motd_len, line_buffer, line_len);
        current_motd_len += line_len;

        // Append end_paste
        memcpy(motd_buffer + current_motd_len, end_paste, end_paste_len);
        current_motd_len += end_paste_len;

        // Append newline
        motd_buffer[current_motd_len++] = '\n';
        motd_buffer[current_motd_len] = '\0';
    }

    if (fclose(motd_file) != 0) {
        const int close_error = errno;
        humanized_log_error("host", "failed to close motd file", close_error);
    }

    ttak_mutex_lock(&host->lock);
    snprintf(host->motd_base, sizeof(host->motd_base), "%s", motd_buffer);
    snprintf(host->motd_path, sizeof(host->motd_path), "%s", path);
    host->motd_has_file = true;
    if (have_info) {
        host->motd_last_modified = modified;
    } else {
        host->motd_last_modified.tv_sec = 0;
        host->motd_last_modified.tv_nsec = 0L;
    }
    host_refresh_motd_locked(host);
    ttak_mutex_unlock(&host->lock);
    return true;
}

void host_set_motd(host_t *host, const char *motd)
{
    sshc_memory_context_t *memory_scope = nullptr;
    if (host != nullptr) {
        memory_scope = sshc_memory_context_push(host->memory_context);
    }
    if (host == nullptr || motd == nullptr) {
        goto exit_host_set_motd;
    }

    char motd_path[PATH_MAX];
    motd_path[0] = '\0';
    snprintf(motd_path, sizeof(motd_path), "%s", motd);
    trim_whitespace_inplace(motd_path);

    const size_t max_paths = 2U;
    const char *paths_to_try[2] = {nullptr, nullptr};
    size_t path_count = 0U;

    char expanded_path[PATH_MAX];
    expanded_path[0] = '\0';
    if (motd_path[0] == '~') {
        const char *home = getenv("HOME");
        if (home != nullptr && home[0] != '\0' &&
            (motd_path[1] == '\0' || motd_path[1] == '/')) {
            const int written = snprintf(expanded_path, sizeof(expanded_path),
                                         "%s%s", home, motd_path + 1);
            if (written > 0 && (size_t)written < sizeof(expanded_path) &&
                path_count < max_paths) {
                paths_to_try[path_count++] = expanded_path;
            }
        }
    }

    if (motd_path[0] != '\0') {
        if (path_count < max_paths) {
            paths_to_try[path_count++] = motd_path;
        }
    }

    for (size_t idx = 0U; idx < path_count; ++idx) {
        if (paths_to_try[idx] != nullptr &&
            host_try_load_motd_from_path(host, paths_to_try[idx])) {
            goto exit_host_set_motd;
        }
    }

    char normalized[sizeof(host->motd)];
    snprintf(normalized, sizeof(normalized), "%s", motd);

    ttak_mutex_lock(&host->lock);
    if (motd_path[0] != '\0') {
        snprintf(host->motd_path, sizeof(host->motd_path), "%s", motd_path);
    } else {
        host->motd_path[0] = '\0';
    }
    host->motd_has_file = false;
    host->motd_last_modified.tv_sec = 0;
    host->motd_last_modified.tv_nsec = 0L;
    snprintf(host->motd_base, sizeof(host->motd_base), "%s", normalized);
    host_refresh_motd_locked(host);
    ttak_mutex_unlock(&host->lock);

exit_host_set_motd:
    if (memory_scope != nullptr) {
        sshc_memory_context_pop(memory_scope);
    }
}

static bool host_prepare_chat_entry(host_t *host, const char *username,
                                    const char *message, const char *color_name,
                                    const char *highlight_name, bool is_bold,
                                    chat_history_entry_t *entry)
{
    if (host == nullptr || username == nullptr || username[0] == '\0' ||
        message == nullptr || message[0] == '\0' || entry == nullptr) {
        return false;
    }

    memset(entry, 0, sizeof(*entry));
    entry->is_user_message = true;
    snprintf(entry->username, sizeof(entry->username), "%s", username);
    snprintf(entry->message, sizeof(entry->message), "%s", message);
    entry->attachment_type = CHAT_ATTACHMENT_NONE;
    entry->user_is_bold = is_bold;
    time_t now = time(nullptr);
    if (now != (time_t)-1) {
        entry->created_at = now;
    }

    const char *color_label = (color_name != nullptr && color_name[0] != '\0')
                                  ? color_name
                                  : host->default_user_color_name;
    snprintf(entry->user_color_name, sizeof(entry->user_color_name), "%s",
             color_label);
    const char *highlight_label =
        (highlight_name != nullptr && highlight_name[0] != '\0')
            ? highlight_name
            : host->default_user_highlight_name;
    snprintf(entry->user_highlight_name, sizeof(entry->user_highlight_name),
             "%s", highlight_label);

    const char *color_code = lookup_color_code(
        USER_COLOR_MAP, sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
        color_label);
    const char *highlight_code = lookup_color_code(
        HIGHLIGHT_COLOR_MAP,
        sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
        highlight_label);

    const char *effective_color =
        color_code != nullptr ? color_code : host->user_theme.userColor;
    const char *effective_highlight =
        highlight_code != nullptr ? highlight_code : host->user_theme.highlight;

    if (effective_color != nullptr) {
        snprintf(entry->user_color_code, sizeof(entry->user_color_code), "%s",
                 effective_color);
    } else {
        entry->user_color_code[0] = '\0';
    }

    if (effective_highlight != nullptr) {
        snprintf(entry->user_highlight_code, sizeof(entry->user_highlight_code),
                 "%s", effective_highlight);
    } else {
        entry->user_highlight_code[0] = '\0';
    }

    return true;
}

void host_append_sync_log(host_t *host, const char *source, const char *message)
{
    if (host == nullptr || source == nullptr || source[0] == '\0' ||
        message == nullptr || message[0] == '\0') {
        return;
    }

    if (host->sync_state_file_path[0] == '\0') {
        return;
    }

    FILE *fp = fopen(host->sync_state_file_path, "a");
    if (fp == nullptr) {
        humanized_log_error("sync", "failed to open sync state file",
                            errno != 0 ? errno : EIO);
        return;
    }

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    fprintf(fp, "%ld|%s|%s\n", (long)now.tv_sec, source, message);
    fclose(fp);
}

static const char *host_ai_chat_default_model(void)
{
    return "gemma2:2b";
}

static const char *host_ai_chat_default_gemini_model(void)
{
    return "gemini-2.5-flash-lite";
}

static const char *host_ai_chat_skip_token(void)
{
    return "cucumber-ballet-fly-tetromino";
}

static bool host_ai_chat_message_mentions(const char *text, const char *needle)
{
    if (text == nullptr || text[0] == '\0' || needle == nullptr ||
        needle[0] == '\0') {
        return false;
    }
    return string_contains_case_insensitive(text, needle);
}

static bool host_ai_chat_reply_is_skip_token(const char *reply)
{
    if (reply == nullptr || reply[0] == '\0') {
        return false;
    }

    char trimmed[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(trimmed, sizeof(trimmed), "%s", reply);
    trim_whitespace_inplace(trimmed);
    return strcasecmp(trimmed, host_ai_chat_skip_token()) == 0;
}

static bool host_ai_chat_message_looks_korean(const char *text)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }

    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor != '\0') {
        if ((cursor[0] & 0xF0U) == 0xE0U && cursor[1] != '\0' &&
            cursor[2] != '\0') {
            const uint32_t cp = ((uint32_t)(cursor[0] & 0x0FU) << 12U) |
                                ((uint32_t)(cursor[1] & 0x3FU) << 6U) |
                                (uint32_t)(cursor[2] & 0x3FU);
            if ((cp >= 0x1100U && cp <= 0x11FFU) ||
                (cp >= 0x3130U && cp <= 0x318FU) ||
                (cp >= 0xAC00U && cp <= 0xD7AFU)) {
                return true;
            }
            cursor += 3;
            continue;
        }

        if ((*cursor & 0xE0U) == 0xC0U && cursor[1] != '\0') {
            cursor += 2;
            continue;
        }
        if ((*cursor & 0xF8U) == 0xF0U && cursor[1] != '\0' &&
            cursor[2] != '\0' && cursor[3] != '\0') {
            cursor += 4;
            continue;
        }
        ++cursor;
    }

    return false;
}

typedef enum ai_chat_bot_persona {
    AI_CHAT_BOT_NONE = 0,
    AI_CHAT_BOT_KAKA,
    AI_CHAT_BOT_DADA,
} ai_chat_bot_persona_t;

static ai_chat_bot_persona_t host_ai_chat_choose_persona(
    const chat_history_entry_t *entry)
{
    if (entry == nullptr) {
        return AI_CHAT_BOT_NONE;
    }

    if (host_ai_chat_message_mentions(entry->message, "kaka") ||
        host_ai_chat_message_mentions(entry->message, "카카")) {
        return AI_CHAT_BOT_KAKA;
    }
    if (host_ai_chat_message_mentions(entry->message, "dada") ||
        host_ai_chat_message_mentions(entry->message, "다다")) {
        return AI_CHAT_BOT_DADA;
    }

    uint32_t hash = 2166136261U;
    for (const unsigned char *cur = (const unsigned char *)entry->username;
         *cur != '\0'; ++cur) {
        hash ^= (uint32_t)(*cur);
        hash *= 16777619U;
    }
    for (const unsigned char *cur = (const unsigned char *)entry->message;
         *cur != '\0'; ++cur) {
        hash ^= (uint32_t)(*cur);
        hash *= 16777619U;
    }

    if ((hash % 4U) != 0U) {
        return AI_CHAT_BOT_NONE;
    }
    return ((hash % 2U) == 0U) ? AI_CHAT_BOT_KAKA : AI_CHAT_BOT_DADA;
}

static bool host_ai_chat_should_respond(const chat_history_entry_t *entry)
{
    if (entry == nullptr || !entry->is_user_message) {
        return false;
    }
    if (entry->message[0] == '\0' || entry->message[0] == '/') {
        return false;
    }
    if (strncasecmp(entry->username, "ai-eliza", SSH_CHATTER_USERNAME_LEN) ==
            0 ||
        strncasecmp(entry->username, "kaka", SSH_CHATTER_USERNAME_LEN) == 0 ||
        strncasecmp(entry->username, "dada", SSH_CHATTER_USERNAME_LEN) == 0) {
        return false;
    }
    return host_ai_chat_choose_persona(entry) != AI_CHAT_BOT_NONE;
}

static void host_ai_chat_snapshot_state(host_t *host, char *model,
                                        size_t model_len, bool *use_gemini,
                                        struct timespec *last_reply)
{
    if (host == nullptr) {
        if (model != nullptr && model_len > 0U) {
            snprintf(model, model_len, "%s", host_ai_chat_default_model());
        }
        if (use_gemini != nullptr) {
            *use_gemini = false;
        }
        if (last_reply != nullptr) {
            last_reply->tv_sec = 0;
            last_reply->tv_nsec = 0L;
        }
        return;
    }

    ttak_mutex_lock(&host->lock);
    if (model != nullptr && model_len > 0U) {
        snprintf(model, model_len, "%s",
                 host->ai_chat_use_gemini
                     ? host_ai_chat_default_gemini_model()
                     : host_ai_chat_default_model());
    }
    if (use_gemini != nullptr) {
        *use_gemini = host->ai_chat_use_gemini;
    }
    if (last_reply != nullptr) {
        *last_reply = host->ai_chat_last_reply;
    }
    ttak_mutex_unlock(&host->lock);
}

static void host_ai_chat_update_last_reply(host_t *host,
                                           const struct timespec *now)
{
    if (host == nullptr || now == nullptr) {
        return;
    }
    ttak_mutex_lock(&host->lock);
    host->ai_chat_last_reply = *now;
    ttak_mutex_unlock(&host->lock);
}

static bool host_ai_member_is_enabled(host_t *host)
{
    if (host == nullptr) {
        return false;
    }
    return atomic_load(&host->ai_chat_enabled);
}

static void host_ai_member_set_enabled(host_t *host, bool enabled)
{
    if (host == nullptr) {
        return;
    }

    atomic_store(&host->ai_chat_enabled, enabled);

    if (enabled) {
        struct timespec now = session_now_monotonic();
        host_ai_chat_update_last_reply(host, &now);
    }
}

static size_t host_ai_chat_memory_collect_tokens(const char *prompt,
                                                 char tokens[][32],
                                                 size_t max_tokens)
{
    if (prompt == nullptr || tokens == nullptr || max_tokens == 0U) {
        return 0U;
    }

    size_t count = 0U;
    size_t length = strlen(prompt);
    size_t idx = 0U;
    while (idx < length && count < max_tokens) {
        while (idx < length && isspace((unsigned char)prompt[idx])) {
            ++idx;
        }
        if (idx >= length) {
            break;
        }

        size_t token_idx = 0U;
        char buffer[32];
        while (idx < length && !isspace((unsigned char)prompt[idx])) {
            unsigned char ch = (unsigned char)prompt[idx];
            if (token_idx + 1U < sizeof(buffer)) {
                buffer[token_idx++] =
                    (ch < 0x80U) ? (char)tolower(ch) : (char)ch;
            }
            ++idx;
        }
        buffer[token_idx] = '\0';

        if (token_idx == 0U) {
            continue;
        }
        if (token_idx < 3U && (unsigned char)buffer[0] < 0x80U) {
            continue;
        }

        bool duplicate = false;
        for (size_t existing = 0U; existing < count; ++existing) {
            if (strcmp(tokens[existing], buffer) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        snprintf(tokens[count], 32U, "%s", buffer);
        ++count;
    }

    return count;
}

static void host_ai_chat_memory_prepare_preview(const char *source, char *dest,
                                                size_t dest_length)
{
    if (dest == nullptr || dest_length == 0U) {
        return;
    }

    dest[0] = '\0';
    if (source == nullptr || source[0] == '\0') {
        return;
    }

    size_t copy_length = strnlen(source, dest_length);
    bool truncated = false;
    if (copy_length >= dest_length) {
        copy_length = dest_length - 1U;
        truncated = true;
    }

    memcpy(dest, source, copy_length);
    dest[copy_length] = '\0';
    trim_whitespace_inplace(dest);

    if (truncated && dest_length > 4U) {
        size_t length = strnlen(dest, dest_length);
        if (length + 3U < dest_length) {
            dest[length++] = '.';
            dest[length++] = '.';
            dest[length++] = '.';
            dest[length] = '\0';
        }
    }
}

static size_t host_ai_chat_memory_collect_context(host_t *host,
                                                  const char *prompt,
                                                  char *context,
                                                  size_t context_length)
{
    if (context == nullptr || context_length == 0U) {
        return 0U;
    }

    context[0] = '\0';
    if (host == nullptr || prompt == nullptr) {
        return 0U;
    }

    ai_chat_memory_entry_t snapshot[SSH_CHATTER_AI_MEMORY_MAX];
    size_t snapshot_count = 0U;

    ttak_mutex_lock(&host->lock);
    snapshot_count = host->ai_chat_memory_count;
    if (snapshot_count > SSH_CHATTER_AI_MEMORY_MAX) {
        snapshot_count = SSH_CHATTER_AI_MEMORY_MAX;
    }
    if (snapshot_count > 0U) {
        memcpy(snapshot, host->ai_chat_memory,
               snapshot_count * sizeof(snapshot[0]));
    }
    ttak_mutex_unlock(&host->lock);

    if (snapshot_count == 0U) {
        return 0U;
    }

    char tokens[SSH_CHATTER_AI_MEMORY_TOKEN_LIMIT][32];
    size_t token_count = host_ai_chat_memory_collect_tokens(
        prompt, tokens, SSH_CHATTER_AI_MEMORY_TOKEN_LIMIT);

    size_t best_indices[SSH_CHATTER_AI_MEMORY_CONTEXT_LIMIT] = {0U};
    size_t best_scores[SSH_CHATTER_AI_MEMORY_CONTEXT_LIMIT] = {0U};
    size_t best_count = 0U;

    for (size_t idx = 0U; idx < snapshot_count; ++idx) {
        const ai_chat_memory_entry_t *entry = &snapshot[idx];
        size_t score = 0U;
        if (token_count > 0U) {
            for (size_t token_idx = 0U; token_idx < token_count; ++token_idx) {
                if (tokens[token_idx][0] == '\0') {
                    continue;
                }
                if (string_contains_case_insensitive(entry->prompt,
                                                     tokens[token_idx]) ||
                    string_contains_case_insensitive(entry->reply,
                                                     tokens[token_idx])) {
                    ++score;
                }
            }
            if (score == 0U) {
                continue;
            }
        }

        size_t recency_bonus = snapshot_count - idx;
        if (recency_bonus > 4U) {
            recency_bonus = 4U;
        }
        score += recency_bonus;

        size_t insert_pos = best_count;
        if (best_count < SSH_CHATTER_AI_MEMORY_CONTEXT_LIMIT) {
            ++best_count;
        } else if (score <=
                   best_scores[SSH_CHATTER_AI_MEMORY_CONTEXT_LIMIT - 1U]) {
            continue;
        } else {
            insert_pos = SSH_CHATTER_AI_MEMORY_CONTEXT_LIMIT - 1U;
        }

        while (insert_pos > 0U && score > best_scores[insert_pos - 1U]) {
            if (insert_pos < SSH_CHATTER_AI_MEMORY_CONTEXT_LIMIT) {
                best_scores[insert_pos] = best_scores[insert_pos - 1U];
                best_indices[insert_pos] = best_indices[insert_pos - 1U];
            }
            --insert_pos;
        }

        best_scores[insert_pos] = score;
        best_indices[insert_pos] = idx;
    }

    if (best_count == 0U && token_count == 0U) {
        size_t fallback = snapshot_count < SSH_CHATTER_AI_MEMORY_CONTEXT_LIMIT
                              ? snapshot_count
                              : SSH_CHATTER_AI_MEMORY_CONTEXT_LIMIT;
        for (size_t idx = 0U; idx < fallback; ++idx) {
            best_indices[idx] = snapshot_count - idx - 1U;
        }
        best_count = fallback;
    }

    if (best_count == 0U) {
        return 0U;
    }

    size_t offset = 0U;
    for (size_t idx = 0U; idx < best_count; ++idx) {
        const ai_chat_memory_entry_t *entry = &snapshot[best_indices[idx]];
        char time_buffer[32];
        time_buffer[0] = '\0';
        if (entry->stored_at != 0) {
            struct tm tm_value;
            if (localtime_r(&entry->stored_at, &tm_value) != nullptr) {
                strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M",
                         &tm_value);
            }
        }
        if (time_buffer[0] == '\0') {
            snprintf(time_buffer, sizeof(time_buffer), "-");
        }

        char prompt_preview[SSH_CHATTER_AI_MEMORY_PREVIEW_LEN];
        char reply_preview[SSH_CHATTER_AI_MEMORY_PREVIEW_LEN];
        host_ai_chat_memory_prepare_preview(entry->prompt, prompt_preview,
                                            sizeof(prompt_preview));
        host_ai_chat_memory_prepare_preview(entry->reply, reply_preview,
                                            sizeof(reply_preview));

        char block[SSH_CHATTER_AI_MEMORY_PREVIEW_LEN * 4U];
        int written = snprintf(
            block, sizeof(block),
            "%s- [%s] %s said: %s\n  ai-eliza replied: %s",
            idx == 0U ? "" : "\n", time_buffer,
            entry->username[0] != '\0' ? entry->username : "user",
            prompt_preview[0] != '\0' ? prompt_preview : "(empty)",
            reply_preview[0] != '\0' ? reply_preview : "(empty)");
        if (written < 0) {
            continue;
        }

        size_t block_len = (size_t)written;
        if (block_len >= sizeof(block)) {
            block_len = sizeof(block) - 1U;
            block[block_len] = '\0';
        }

        if (offset + block_len >= context_length) {
            size_t available =
                (offset < context_length) ? context_length - offset - 1U : 0U;
            if (available > 0U) {
                memcpy(context + offset, block, available);
                offset += available;
                context[offset] = '\0';
            }
            break;
        }

        memcpy(context + offset, block, block_len);
        offset += block_len;
        context[offset] = '\0';
    }

    return best_count;
}

static void host_ai_chat_memory_store(host_t *host, const char *username,
                                      const char *prompt, const char *reply)
{
    if (host == nullptr || prompt == nullptr || reply == nullptr) {
        return;
    }

    char clean_prompt[SSH_CHATTER_MESSAGE_LIMIT];
    char clean_reply[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(clean_prompt, sizeof(clean_prompt), "%s", prompt);
    snprintf(clean_reply, sizeof(clean_reply), "%s", reply);
    trim_whitespace_inplace(clean_prompt);
    trim_whitespace_inplace(clean_reply);

    char clean_username[SSH_CHATTER_USERNAME_LEN];
    if (username != nullptr && username[0] != '\0') {
        snprintf(clean_username, sizeof(clean_username), "%s", username);
    } else {
        snprintf(clean_username, sizeof(clean_username), "%s", "user");
    }

    ttak_mutex_lock(&host->lock);
    if (host->ai_chat_memory_count >= SSH_CHATTER_AI_MEMORY_MAX) {
        memmove(host->ai_chat_memory, host->ai_chat_memory + 1,
                (SSH_CHATTER_AI_MEMORY_MAX - 1U) *
                    sizeof(host->ai_chat_memory[0]));
        host->ai_chat_memory_count = SSH_CHATTER_AI_MEMORY_MAX - 1U;
    }

    ai_chat_memory_entry_t *entry =
        &host->ai_chat_memory[host->ai_chat_memory_count++];
    entry->stored_at = time(nullptr);
    snprintf(entry->username, sizeof(entry->username), "%s", clean_username);
    snprintf(entry->prompt, sizeof(entry->prompt), "%s", clean_prompt);
    snprintf(entry->reply, sizeof(entry->reply), "%s", clean_reply);
    ttak_mutex_unlock(&host->lock);
}

static void host_ai_chat_consider_reply(host_t *host,
                                        const chat_history_entry_t *entry)
{
    if (host == nullptr || entry == nullptr) {
        return;
    }
    if (!host_ai_member_is_enabled(host)) {
        return;
    }
    if (!host_ai_chat_should_respond(entry)) {
        return;
    }

    struct timespec now = session_now_monotonic();
    struct timespec last_reply = {0, 0};
    char model[64];
    bool use_gemini = false;
    host_ai_chat_snapshot_state(host, model, sizeof(model), &use_gemini,
                                &last_reply);

    double cooldown = session_timespec_elapsed_seconds(&now, &last_reply);
    if (cooldown < 3.0) {
        return;
    }

    char prompt[SSH_CHATTER_MESSAGE_LIMIT * 2U];
    char context[SSH_CHATTER_AI_MEMORY_CONTEXT_BUFFER];
    char username_snippet[SSH_CHATTER_USERNAME_LEN];
    char message_snippet[SSH_CHATTER_AI_PROMPT_MESSAGE_MAX];
    host_ai_chat_copy_limited(username_snippet, sizeof(username_snippet),
                              entry->username, SSH_CHATTER_AI_PROMPT_USERNAME_MAX);
    host_ai_chat_copy_limited(message_snippet, sizeof(message_snippet),
                              entry->message,
                              SSH_CHATTER_AI_PROMPT_MESSAGE_MAX - 1U);
    size_t context_matches = host_ai_chat_memory_collect_context(
        host, entry->message, context, sizeof(context));
    ai_chat_bot_persona_t persona = host_ai_chat_choose_persona(entry);
    if (persona == AI_CHAT_BOT_NONE) {
        return;
    }

    const bool korean = host_ai_chat_message_looks_korean(entry->message);
    const char *persona_name =
        (persona == AI_CHAT_BOT_KAKA) ? "kaka" : "dada";
    const char *tone_instruction = nullptr;
    if (persona == AI_CHAT_BOT_KAKA) {
        tone_instruction =
            "Respond as kaka, a slightly cheerful and playful chat "
            "participant. Keep it short and natural.";
    } else {
        tone_instruction =
            "Respond as dada, a calm-but-absurd jokester who sounds a little "
            "childish. Keep it short and natural.";
    }
    const char *language_instruction =
        korean
            ? "The user is speaking Korean. Reply in Korean."
            : "The user is speaking English. Reply in English.";

    if (context_matches > 0U && context[0] != '\0') {
        char context_snippet[SSH_CHATTER_AI_PROMPT_CONTEXT_MAX];
        host_ai_chat_copy_limited(context_snippet, sizeof(context_snippet),
                                  context,
                                  SSH_CHATTER_AI_PROMPT_CONTEXT_MAX - 1U);
        snprintf(prompt, sizeof(prompt),
                 "Memory context:\n%s\n\nUser %s says: %s\n"
                 "%s %s Keep replies under three sentences and avoid "
                 "moderation or BBS topics. If you decide this message does "
                 "not need a reply, output exactly: "
                 "%s",
                 context_snippet, username_snippet, message_snippet,
                 tone_instruction, language_instruction,
                 host_ai_chat_skip_token());
    } else {
        snprintf(prompt, sizeof(prompt),
                 "User %s says: %s\n"
                 "%s %s Keep replies under three sentences and avoid "
                 "moderation or BBS topics. If you decide this message does "
                 "not need a reply, output exactly: "
                 "%s",
                 username_snippet, message_snippet, tone_instruction,
                 language_instruction, host_ai_chat_skip_token());
    }

    char reply[SSH_CHATTER_MESSAGE_LIMIT];
    bool success = use_gemini
                       ? translator_gemini_smalltalk(
                             prompt, model, reply, sizeof(reply))
                       : translator_ollama_smalltalk(
                             prompt, model, reply, sizeof(reply));
    if (!success) {
        const char *error = translator_last_error();
        if (error != nullptr && error[0] != '\0') {
            printf("[ai-chat] small-talk request failed: %s\n", error);
        } else {
            printf("[ai-chat] small-talk request failed.\n");
        }
        return;
    }

    if (!success || reply[0] == '\0') {
        return;
    }
    if (host_ai_chat_reply_is_skip_token(reply)) {
        return;
    }

    if (!host_post_client_message(host, persona_name, reply, nullptr, nullptr,
                                  false)) {
        return;
    }

    host_ai_chat_memory_store(host, entry->username, entry->message, reply);
    host_ai_chat_update_last_reply(host, &now);
}

bool host_post_client_message(host_t *host, const char *username,
                              const char *message, const char *color_name,
                              const char *highlight_name, bool is_bold)
{
    bool success = false;
    sshc_memory_context_t *memory_scope = nullptr;
    if (host != nullptr) {
        memory_scope = sshc_memory_context_push(host->memory_context);
    }
    chat_history_entry_t entry = {0};
    if (!host_prepare_chat_entry(host, username, message, color_name,
                                 highlight_name, is_bold, &entry)) {
        goto exit_host_post_client_message;
    }

    chat_history_entry_t stored = {0};
    if (!host_history_commit_entry(host, &entry, &stored)) {
        goto exit_host_post_client_message;
    }

    chat_room_broadcast_entry(&host->room, &stored, nullptr);
    host_notify_external_clients(host, &stored);
    success = true;
    if (success) {
        host_ai_chat_consider_reply(host, &stored);
    }

exit_host_post_client_message:
    if (memory_scope != nullptr) {
        sshc_memory_context_pop(memory_scope);
    }
    return success;
}

bool host_post_ephemeral_message(host_t *host, const char *username,
                                 const char *message, const char *color_name,
                                 const char *highlight_name, bool is_bold)
{
    bool success = false;
    sshc_memory_context_t *memory_scope = nullptr;
    if (host != nullptr) {
        memory_scope = sshc_memory_context_push(host->memory_context);
    }
    chat_history_entry_t entry = {0};
    if (!host_prepare_chat_entry(host, username, message, color_name,
                                 highlight_name, is_bold, &entry)) {
        goto exit_host_post_client_message;
    }

    chat_room_broadcast_entry(&host->room, &entry, nullptr);
    host_notify_external_clients(host, &entry);
    success = true;

exit_host_post_client_message:
    if (memory_scope != nullptr) {
        sshc_memory_context_pop(memory_scope);
    }
    return success;
}

bool host_snapshot_last_captcha(host_t *host, char *question,
                                size_t question_length, char *answer,
                                size_t answer_length,
                                struct timespec *timestamp)
{
    if (host == nullptr) {
        return false;
    }

    ttak_mutex_lock(&host->lock);
    bool has_captcha = host->has_last_captcha;
    if (has_captcha) {
        if (question != nullptr && question_length > 0U) {
            snprintf(question, question_length, "%s",
                     host->last_captcha_question);
        }
        if (answer != nullptr && answer_length > 0U) {
            snprintf(answer, answer_length, "%s", host->last_captcha_answer);
        }
        if (timestamp != nullptr) {
            *timestamp = host->last_captcha_generated;
        }
    } else {
        if (question != nullptr && question_length > 0U) {
            question[0] = '\0';
        }
        if (answer != nullptr && answer_length > 0U) {
            answer[0] = '\0';
        }
        if (timestamp != nullptr) {
            timestamp->tv_sec = 0;
            timestamp->tv_nsec = 0L;
        }
    }
    ttak_mutex_unlock(&host->lock);
    return has_captcha;
}

static void host_sleep_after_error(host_t *host)
{
    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);

    unsigned int streak = 1U;
    if (host != nullptr) {
        if (host->health_guard.last_error_time.tv_sec != 0 ||
            host->health_guard.last_error_time.tv_nsec != 0) {
            struct timespec diff =
                timespec_diff(&now, &host->health_guard.last_error_time);
            long long diff_ns = timespec_to_ns(&diff);
            if (diff_ns <= SSH_CHATTER_ERROR_BACKOFF_STABLE_NS &&
                host->health_guard.consecutive_errors < UINT_MAX) {
                streak = host->health_guard.consecutive_errors + 1U;
            }
        }

        host->health_guard.consecutive_errors = streak;
        host->health_guard.last_error_time = now;
    }

    long long multiplier = (long long)streak;
    if (multiplier <= 0) {
        multiplier = 1LL;
    }

    const long long max_multiplier =
        SSH_CHATTER_ERROR_BACKOFF_MAX_NS / SSH_CHATTER_ERROR_BACKOFF_BASE_NS;
    if (multiplier > max_multiplier) {
        multiplier = max_multiplier;
    }

    long long delay_ns = SSH_CHATTER_ERROR_BACKOFF_BASE_NS * multiplier;
    if (delay_ns > SSH_CHATTER_ERROR_BACKOFF_MAX_NS) {
        delay_ns = SSH_CHATTER_ERROR_BACKOFF_MAX_NS;
    }
    if (delay_ns < SSH_CHATTER_ERROR_BACKOFF_BASE_NS) {
        delay_ns = SSH_CHATTER_ERROR_BACKOFF_BASE_NS;
    }

    struct timespec delay = {
        .tv_sec = (time_t)(delay_ns / 1000000000LL),
        .tv_nsec = (long)(delay_ns % 1000000000LL),
    };
    host_sleep_uninterruptible(&delay);
}

static void host_shutdown_internal(host_t *host, bool send_sigterm)
{
    if (host == nullptr) {
        return;
    }

    if (send_sigterm) {
        // Terminate all child processes in the same process group
        kill(0, SIGTERM);
    }

    sshc_memory_context_t *memory_scope = nullptr;
    if (host->memory_context != nullptr) {
        memory_scope = sshc_memory_context_push(host->memory_context);
    }

    host_eliza_worker_shutdown(host);
    host_moderation_shutdown(host);

    host_telnet_listener_stop(host);
    host_json_api_listener_stop(host);

    if (host->rss_thread_initialized) {
        atomic_store(&host->rss_thread_stop, true);
        pthread_join(host->rss_thread, nullptr);
        host->rss_thread_initialized = false;
        atomic_store(&host->rss_thread_running, false);
    }

    while (atomic_load(&host->rss_manual_refresh_running)) {
        struct timespec wait = {
            .tv_sec = 0,
            .tv_nsec = 50 * 1000 * 1000L,
        };
        host_sleep_uninterruptible(&wait);
    }

    if (host->archive_thread_initialized) {
        atomic_store(&host->archive_thread_stop, true);
        pthread_join(host->archive_thread, nullptr);
        host->archive_thread_initialized = false;
        atomic_store(&host->archive_thread_running, false);
    }


    if (host->bbs_watchdog_thread_initialized) {
        atomic_store(&host->bbs_watchdog_thread_stop, true);
        pthread_join(host->bbs_watchdog_thread, nullptr);
        host->bbs_watchdog_thread_initialized = false;
        atomic_store(&host->bbs_watchdog_thread_running, false);
    }

    if (host->morse_client != nullptr) {
        morse_client_destroy(host->morse_client);
        host->morse_client = nullptr;
    }
    if (host->web_client != nullptr) {
        webssh_client_destroy(host->web_client);
        host->web_client = nullptr;
    }
    if (host->clients != nullptr) {
        client_manager_destroy(host->clients);
        host->clients = nullptr;
    }
    chat_history_entry_t *history_buffer = nullptr;
    join_activity_entry_t *join_buffer = nullptr;
    ttak_mutex_lock(&host->lock);
    history_buffer = host->history;
    host->history = nullptr;
    host->history_capacity = 0U;
    host->history_count = 0U;
    join_buffer = host->join_activity;
    host->join_activity = nullptr;
    host->join_activity_capacity = 0U;
    host->join_activity_count = 0U;
    ttak_mutex_unlock(&host->lock);
    if (history_buffer != nullptr) {
        sshc_gc_free(history_buffer);
    }
    if (join_buffer != nullptr) {
        sshc_gc_free(join_buffer);
    }
    sshc_gc_free(host->connection_guard);
    host->connection_guard = nullptr;
    host->connection_guard_capacity = 0U;
    host->connection_guard_count = 0U;
    host->listener.accept_error_streak = 0U;
    host->health_guard.consecutive_errors = 0U;
    host->health_guard.last_error_time.tv_sec = 0;
    host->health_guard.last_error_time.tv_nsec = 0L;
    session_ctx_t **room_members = nullptr;
    ttak_mutex_lock(&host->room.lock);
    room_members = host->room.members;
    host->room.members = nullptr;
    host->room.member_capacity = 0U;
    host->room.member_count = 0U;
    ttak_mutex_unlock(&host->room.lock);
    if (room_members != nullptr) {
        sshc_gc_free(room_members);
    }
    if (host->user_data_lock_initialized) {
        ttak_mutex_destroy(&host->user_data_lock);
        host->user_data_lock_initialized = false;
    }
    if (host->alpha_landers_lock_initialized) {
        ttak_mutex_destroy(&host->alpha_landers_lock);
        host->alpha_landers_lock_initialized = false;
    }
    if (host->security_layer_initialized) {
        security_layer_free(&host->security_layer);
        host->security_layer_initialized = false;
    }

    if (host->rss_refresh_lock_initialized) {
        ttak_mutex_destroy(&host->rss_refresh_lock);
        host->rss_refresh_lock_initialized = false;
    }

    if (host->nickname_claim_pool != nullptr) {
        ttak_object_pool_destroy(host->nickname_claim_pool);
        host->nickname_claim_pool = nullptr;
    }
    memset(host->nickname_claims, 0, sizeof(host->nickname_claims));
    host->nickname_claim_count = 0U;

    ttak_mutex_destroy(&host->room.lock);
    ttak_mutex_destroy(&host->lock);
    ttak_mutex_destroy(&host->nickname_reserve_lock);

    if (memory_scope != nullptr) {
        sshc_memory_context_pop(memory_scope);
    }
}

void host_shutdown(host_t *host)
{
    host_shutdown_internal(host, true);
}

void host_shutdown_for_testing(host_t *host)
{
    host_shutdown_internal(host, false);
}

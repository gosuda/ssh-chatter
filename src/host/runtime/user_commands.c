
static void session_handle_nick(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (arguments == nullptr || *arguments == '\0') {
        session_send_system_line(ctx, "Usage: /nick <name>");
        return;
    }

    char new_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(new_name, sizeof(new_name), "%s", arguments);
    trim_whitespace_inplace(new_name);

    if (new_name[0] == '\0') {
        session_send_system_line(ctx, "Usage: /nick <name>");
        return;
    }

    if (ctx->user.is_lan_operator && strcmp(new_name, ctx->user.name) != 0) {
        session_send_system_line(ctx, "LAN operator nicknames are fixed.");
        return;
    }

    mbstate_t state;
    memset(&state, 0, sizeof(state));
    const char *cursor = new_name;
    while (*cursor != '\0') {
        wchar_t wc;
        size_t consumed = mbrtowc(&wc, cursor, (size_t)-1, &state);
        if (consumed == (size_t)-1 || consumed == (size_t)-2) {
            // Invalid UTF-8 sequence
            session_send_system_line(
                ctx, "Names may not contain invalid UTF-8 sequences.");
            return;
        }
        if (consumed ==
            0) { // Should not happen with valid UTF-8, but as a safeguard
            cursor++;
            continue;
        }

        // Check if the character is a control character, DEL, space, or tab
        if (wc < 0x20 || wc == 0x7F || wc == L' ' || wc == L'\t') {
            session_send_system_line(ctx, "Names may not include control "
                                          "characters, DEL, space, or tab.");
            return;
        }
        cursor += consumed;
    }

    if (host_is_username_banned(ctx->owner, new_name)) {
        session_send_system_line(
            ctx, "That nickname is blocked for bot detection. Choose another.");
        return;
    }

    if (host_username_reserved(ctx->owner, new_name) &&
        !ctx->user.is_lan_operator) {
        session_send_system_line(ctx,
                                 "That name is reserved for LAN operators.");
        return;
    }

    if (!ctx->user_data_loaded) {
        (void)session_user_data_load(ctx);
    }

    bool owns_requested_name = strcasecmp(ctx->user.name, new_name) == 0;
    if (!owns_requested_name && ctx->user_data_loaded) {
        owns_requested_name =
            strcasecmp(ctx->user_data.username, new_name) == 0;
    }

    if (!owns_requested_name && host_username_has_password(ctx->owner, new_name) &&
        !host_nickname_claim_can_use(ctx->owner, ctx, new_name)) {
        session_send_system_line(
            ctx, "That nickname is password-protected. Log in as that user.");
        return;
    }

    session_ctx_t *existing = chat_room_find_user(&ctx->owner->room, new_name);
    if (existing != nullptr && existing != ctx) {
        session_send_system_line(ctx, "That name is already taken.");
        return;
    }

    char old_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(old_name, sizeof(old_name), "%s", ctx->user.name);
    host_nickname_claim_release(ctx->owner, ctx, old_name);
    snprintf(ctx->user.name, sizeof(ctx->user.name), "%s", new_name);

    if (ctx->user_data_loaded) {
        snprintf(ctx->user_data.preferred_nickname,
                 sizeof(ctx->user_data.preferred_nickname), "%s", new_name);
        if (ctx->owner != NULL && ctx->owner->user_data_root[0] != '\0') {
            user_data_save(ctx->owner->user_data_root, &ctx->user_data,
                           ctx->client_ip);
        }
    }

    char announcement[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(announcement, sizeof(announcement), "* [%s] is now known as [%s]",
             old_name, ctx->user.name);
    host_history_record_system(ctx->owner, announcement, NULL);
    chat_room_broadcast(&ctx->owner->room, announcement, NULL);
    session_apply_saved_preferences(ctx);
    session_send_system_line(ctx, "Display name updated.");
}

static void session_force_disconnect(session_ctx_t *ctx, const char *reason)
{
    if (ctx == nullptr) {
        return;
    }

    if (!ctx->exit_notice_sent && reason != nullptr && reason[0] != '\0') {
        session_send_system_line(ctx, reason);
        ctx->exit_notice_sent = true;
    }

    ctx->should_exit = true;
    ctx->exit_status = EXIT_FAILURE;

    if (ctx->translation_mutex_initialized) {
        ttak_mutex_lock(&ctx->translation_mutex);
        ctx->translation_thread_stop = true;
        ttak_cond_broadcast(&ctx->translation_cond);
        ttak_mutex_unlock(&ctx->translation_mutex);
    }
    session_translation_clear_queue(ctx);

    session_transport_request_close(ctx);
}

static void session_handle_exit(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    session_force_disconnect(ctx, "Disconnecting... bye!");
    ctx->exit_status = EXIT_SUCCESS;
}

static void session_handle_pardon(session_ctx_t *ctx, const char *arguments)
{
    if (!ctx->user.is_operator) {
        session_send_system_line(ctx, "You are not allowed to pardon users.");
        return;
    }

    if (arguments == nullptr || *arguments == '\0') {
        session_send_system_line(ctx, "Usage: /pardon <user|ip>");
        return;
    }

    char token[SSH_CHATTER_IP_LEN];
    snprintf(token, sizeof(token), "%s", arguments);
    trim_whitespace_inplace(token);

    if (token[0] == '\0') {
        session_send_system_line(ctx, "Usage: /pardon <user|ip>");
        return;
    }

    if (host_remove_ban_entry(ctx->owner, token)) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "Ban lifted for '%s'.", token);
        session_send_system_line(ctx, message);
    } else {
        session_send_system_line(ctx, "No matching ban found.");
    }
}

static session_ctx_t *chat_room_find_user(chat_room_t *room,
                                          const char *username)
{
    if (room == nullptr || username == nullptr) {
        return nullptr;
    }

    session_ctx_t *result = nullptr;
    ttak_mutex_lock(&room->lock);
    for (size_t idx = 0; idx < room->member_count; ++idx) {
        session_ctx_t *member = room->members[idx];
        if (member == nullptr) {
            continue;
        }

        if (strncmp(member->user.name, username, SSH_CHATTER_USERNAME_LEN) ==
            0) {
            result = member;
            break;
        }
    }
    ttak_mutex_unlock(&room->lock);

    return result;
}

static bool host_username_reserved(host_t *host, const char *username)
{
    if (host == nullptr || username == nullptr || username[0] == '\0') {
        return false;
    }

    // Check if it's a LAN operator username
    if (host_is_lan_operator_username(host, username)) {
        return true;
    }

    return false;
}

static join_activity_entry_t *host_find_join_activity_locked(host_t *host,
                                                             const char *ip)
{
    if (host == nullptr || ip == nullptr) {
        return nullptr;
    }

    for (size_t idx = 0; idx < host->join_activity_count; ++idx) {
        join_activity_entry_t *entry = &host->join_activity[idx];
        if (strncmp(entry->ip, ip, SSH_CHATTER_IP_LEN) == 0) {
            return entry;
        }
    }

    return nullptr;
}

static void
host_prune_join_activity_locked(host_t *host,
                                const struct timespec *reference_time)
{
    if (host == nullptr) {
        return;
    }

    sshc_memory_context_t *memory_scope = host_memory_scope_push(host);

    if (host->join_activity == nullptr || host->join_activity_count == 0U) {
        host_memory_scope_pop(memory_scope);
        return;
    }

    struct timespec now = {0, 0};
    if (reference_time != nullptr &&
        (reference_time->tv_sec != 0 || reference_time->tv_nsec != 0)) {
        now = *reference_time;
    } else {
        clock_gettime(CLOCK_MONOTONIC, &now);
    }

    size_t idx = 0U;
    while (idx < host->join_activity_count) {
        join_activity_entry_t *entry = &host->join_activity[idx];
        bool remove_entry = false;

        if (entry->last_attempt.tv_sec == 0 &&
            entry->last_attempt.tv_nsec == 0) {
            remove_entry = true;
        } else {
            struct timespec age = timespec_diff(&now, &entry->last_attempt);
            long long age_ns = timespec_to_ns(&age);
            if (age_ns >= SSH_CHATTER_JOIN_ACTIVITY_RETENTION_NS) {
                remove_entry = true;
            } else if (entry->asciiart_has_cooldown) {
                struct timespec ascii_age =
                    timespec_diff(&now, &entry->last_asciiart_post);
                long long ascii_age_ns = timespec_to_ns(&ascii_age);
                if (ascii_age_ns >= SSH_CHATTER_JOIN_ACTIVITY_RETENTION_NS) {
                    entry->asciiart_has_cooldown = false;
                }
            }
        }

        if (remove_entry) {
            size_t last_index = host->join_activity_count - 1U;
            if (idx != last_index) {
                host->join_activity[idx] = host->join_activity[last_index];
            }
            memset(&host->join_activity[last_index], 0,
                   sizeof(host->join_activity[last_index]));
            host->join_activity_count = last_index;
            continue;
        }

        ++idx;
    }

    if (host->join_activity_count == 0U) {
        sshc_gc_free(host->join_activity);
        host->join_activity = nullptr;
        host->join_activity_capacity = 0U;
    } else if (host->join_activity_capacity > 8U &&
               host->join_activity_count <= host->join_activity_capacity / 2U) {
        size_t new_capacity = host->join_activity_capacity / 2U;
        if (new_capacity < 8U) {
            new_capacity = 8U;
        }
        if (new_capacity < host->join_activity_count) {
            new_capacity = host->join_activity_count;
        }
        join_activity_entry_t *resized = (join_activity_entry_t *)sshc_gc_realloc(
            host->join_activity, new_capacity * sizeof(*resized));
        if (resized != nullptr) {
            host->join_activity = resized;
            host->join_activity_capacity = new_capacity;
        }
    }

    host_memory_scope_pop(memory_scope);
}

static join_activity_entry_t *host_ensure_join_activity_locked(host_t *host,
                                                               const char *ip)
{
    if (host == nullptr || ip == nullptr || ip[0] == '\0') {
        return nullptr;
    }

    sshc_memory_context_t *memory_scope = host_memory_scope_push(host);

    join_activity_entry_t *entry = host_find_join_activity_locked(host, ip);
    if (entry != nullptr) {
        host_memory_scope_pop(memory_scope);
        return entry;
    }

    if (host->join_activity_count >= host->join_activity_capacity) {
        size_t new_capacity = host->join_activity_capacity > 0U
                                  ? host->join_activity_capacity * 2U
                                  : 8U;
        join_activity_entry_t *resized = sshc_gc_realloc(
            host->join_activity, new_capacity * sizeof(join_activity_entry_t));
        if (resized == nullptr) {
            host_memory_scope_pop(memory_scope);
            return nullptr;
        }
        host->join_activity = resized;
        host->join_activity_capacity = new_capacity;
    }

    entry = &host->join_activity[host->join_activity_count++];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->ip, sizeof(entry->ip), "%s", ip);
    host_memory_scope_pop(memory_scope);
    return entry;
}

static size_t host_prepare_join_delay(host_t *host,
                                      struct timespec *wait_duration)
{
    struct timespec wait = {0, 0};
    if (host == nullptr) {
        if (wait_duration != nullptr) {
            *wait_duration = wait;
        }
        return 1U;
    }

    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);

    ttak_mutex_lock(&host->lock);
    if (!host->join_throttle_initialised) {
        host->next_join_ready_time = now;
        host->join_throttle_initialised = true;
        host->join_progress_length = 0U;
    }

    if (timespec_compare(&now, &host->next_join_ready_time) < 0) {
        wait = timespec_diff(&host->next_join_ready_time, &now);
    }

    struct timespec base = now;
    if (timespec_compare(&host->next_join_ready_time, &now) > 0) {
        base = host->next_join_ready_time;
    }
    host->next_join_ready_time = timespec_add_ms(&base, 100);
    host->join_progress_length =
        (host->join_progress_length % SSH_CHATTER_JOIN_BAR_MAX) + 1U;
    size_t progress = host->join_progress_length;
    ttak_mutex_unlock(&host->lock);

    if (wait_duration != nullptr) {
        *wait_duration = wait;
    }
    return progress;
}

static host_join_attempt_result_t
host_register_join_attempt(host_t *host, const char *username, const char *ip)
{
    if (host == nullptr || ip == nullptr || ip[0] == '\0') {
        return HOST_JOIN_ATTEMPT_OK;
    }

    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);

    bool exempt_ip = false;
    bool kick_ip = false;

    ttak_mutex_lock(&host->lock);
    host_prune_join_activity_locked(host, &now);
    join_activity_entry_t *entry = host_ensure_join_activity_locked(host, ip);
    if (entry == nullptr) {
        ttak_mutex_unlock(&host->lock);
        return HOST_JOIN_ATTEMPT_OK;
    }

    struct timespec diff = timespec_diff(&now, &entry->last_attempt);
    const long long diff_ns =
        (long long)diff.tv_sec * 1000000000LL + (long long)diff.tv_nsec;
    const bool has_prior_attempt =
        (entry->last_attempt.tv_sec != 0 || entry->last_attempt.tv_nsec != 0);
    const bool within_window =
        has_prior_attempt && diff_ns <= SSH_CHATTER_JOIN_RAPID_WINDOW_NS;

    if (within_window) {
        entry->rapid_attempts += 1U;
    } else {
        entry->rapid_attempts = 1U;
    }

    if (username != nullptr && username[0] != '\0') {
        if (within_window && strncmp(entry->last_username, username,
                                     SSH_CHATTER_USERNAME_LEN) == 0) {
            entry->same_name_attempts += 1U;
        } else {
            entry->same_name_attempts = 1U;
            snprintf(entry->last_username, sizeof(entry->last_username), "%s",
                     username);
        }
    } else {
        entry->same_name_attempts =
            within_window ? entry->same_name_attempts + 1U : 1U;
    }

    const bool has_join_window = entry->join_window_start.tv_sec != 0 ||
                                 entry->join_window_start.tv_nsec != 0;
    if (!has_join_window) {
        entry->join_window_start = now;
        entry->join_window_attempts = 1U;
    } else {
        struct timespec window_diff =
            timespec_diff(&now, &entry->join_window_start);
        const long long window_ns =
            (long long)window_diff.tv_sec * 1000000000LL +
            (long long)window_diff.tv_nsec;
        if (window_ns > SSH_CHATTER_JOIN_KICK_WINDOW_NS) {
            entry->join_window_start = now;
            entry->join_window_attempts = 1U;
        } else {
            if (entry->join_window_attempts < SIZE_MAX) {
                entry->join_window_attempts += 1U;
            }
        }
    }

    entry->last_attempt = now;

    if (host_ip_has_grant_locked(host, ip)) {
        exempt_ip = true;
    }

    if (!exempt_ip &&
        entry->join_window_attempts >= SSH_CHATTER_JOIN_KICK_THRESHOLD) {
        kick_ip = true;
    }
    ttak_mutex_unlock(&host->lock);

    if (!exempt_ip && kick_ip) {
        printf("[auto-kick] %s exceeded join limit\n", ip);
        return HOST_JOIN_ATTEMPT_KICK;
    }

    /* Register this successful join attempt in the connection guard. */
    (void)host_connection_guard_register(host, ip);

    return HOST_JOIN_ATTEMPT_OK;
}

static bool host_register_suspicious_activity(host_t *host,
                                              const char *username,
                                              const char *ip,
                                              size_t *attempts_out)
{
    if (host == nullptr || ip == nullptr || ip[0] == '\0') {
        if (attempts_out != nullptr) {
            *attempts_out = 0U;
        }
        return false;
    }

    (void)username;

    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);

    size_t attempts = 0U;
    ttak_mutex_lock(&host->lock);
    join_activity_entry_t *entry = host_ensure_join_activity_locked(host, ip);
    if (entry != nullptr) {
        if (entry->last_suspicious.tv_sec != 0 ||
            entry->last_suspicious.tv_nsec != 0) {
            struct timespec diff = timespec_diff(&now, &entry->last_suspicious);
            long long diff_ns =
                (long long)diff.tv_sec * 1000000000LL + (long long)diff.tv_nsec;
            if (diff_ns > SSH_CHATTER_SUSPICIOUS_EVENT_WINDOW_NS) {
                entry->suspicious_events = 0U;
            }
        }

        if (entry->suspicious_events < SSH_CHATTER_SUSPICIOUS_EVENT_THRESHOLD) {
            entry->suspicious_events += 1U;
        } else {
            entry->suspicious_events = SSH_CHATTER_SUSPICIOUS_EVENT_THRESHOLD;
        }
        entry->last_suspicious = now;
        attempts = entry->suspicious_events;
    }
    ttak_mutex_unlock(&host->lock);

    if (attempts_out != nullptr) {
        *attempts_out = attempts;
    }

    return false;
}

static bool host_is_ip_banned(host_t *host, const char *ip)
{
    if (host == nullptr || ip == nullptr || ip[0] == '\0') {
        return false;
    }

    bool banned = false;
    ttak_mutex_lock(&host->lock);
    if (host_is_protected_ip_unlocked(host, ip)) {
        ttak_mutex_unlock(&host->lock);
        return false;
    }
    host_bans_ensure(host);
    for (size_t idx = 0; idx < host->ban_count; ++idx) {
        const char *ban_ip = host->bans[idx].ip;
        if (ban_ip[0] == '\0') {
            continue;
        }

        if (host_is_protected_ip_unlocked(host, ban_ip)) {
            continue;
        }

        if (strchr(ban_ip, '/') != nullptr) {
            if (host_cidr_contains_ip(ban_ip, ip)) {
                banned = true;
                break;
            }
            continue;
        }

        if (strncmp(ban_ip, ip, SSH_CHATTER_IP_LEN) == 0) {
            banned = true;
            break;
        }
    }
    ttak_mutex_unlock(&host->lock);

    return banned;
}

static bool host_is_username_banned(host_t *host, const char *username)
{
    if (host == nullptr || username == nullptr || username[0] == '\0') {
        return false;
    }

    bool banned = false;
    ttak_mutex_lock(&host->lock);
    host_bans_ensure(host);
    for (size_t idx = 0; idx < host->ban_count; ++idx) {
        if (strncmp(host->bans[idx].username, username,
                    SSH_CHATTER_USERNAME_LEN) == 0) {
            banned = true;
            break;
        }
    }
    ttak_mutex_unlock(&host->lock);

    return banned;
}

static bool host_add_ban_entry(host_t *host, const char *username,
                               const char *ip)
{
    if (host == nullptr) {
        return false;
    }

    bool added = false;
    ttak_mutex_lock(&host->lock);
    host_bans_ensure(host);
    host_protected_ips_ensure(host);
    if (host->ban_count >= SSH_CHATTER_MAX_BANS) {
        ttak_mutex_unlock(&host->lock);
        return false;
    }

    if (ip != nullptr && ip[0] != '\0' &&
        host_is_protected_ip_unlocked(host, ip)) {
        ttak_mutex_unlock(&host->lock);
        return true;
    }
    if (ip != nullptr && ip[0] != '\0' && strchr(ip, '/') != nullptr) {
        for (size_t idx = 0; idx < host->protected_ip_count &&
                             idx < SSH_CHATTER_MAX_PROTECTED_IPS;
             ++idx) {
            if (host_cidr_contains_ip(ip, host->protected_ips[idx])) {
                ttak_mutex_unlock(&host->lock);
                return true;
            }
        }
    }

    for (size_t idx = 0; idx < host->ban_count; ++idx) {
        const bool username_match =
            (username != nullptr && username[0] != '\0' &&
             strncmp(host->bans[idx].username, username,
                     SSH_CHATTER_USERNAME_LEN) == 0);
        const bool ip_match =
            (ip != nullptr && ip[0] != '\0' &&
             strncmp(host->bans[idx].ip, ip, SSH_CHATTER_IP_LEN) == 0);
        if (username_match || ip_match) {
            ttak_mutex_unlock(&host->lock);
            return true;
        }
    }

    strncpy(host->bans[host->ban_count].username,
            username != nullptr ? username : "", SSH_CHATTER_USERNAME_LEN - 1U);
    host->bans[host->ban_count].username[SSH_CHATTER_USERNAME_LEN - 1U] = '\0';
    strncpy(host->bans[host->ban_count].ip, ip != nullptr ? ip : "",
            SSH_CHATTER_IP_LEN - 1U);
    host->bans[host->ban_count].ip[SSH_CHATTER_IP_LEN - 1U] = '\0';
    ++host->ban_count;
    added = true;

    host_ban_state_save_locked(host);

    ttak_mutex_unlock(&host->lock);
    return added;
}

static bool host_remove_ban_entry(host_t *host, const char *token)
{
    if (host == nullptr || token == nullptr || token[0] == '\0') {
        return false;
    }

    bool removed = false;
    ttak_mutex_lock(&host->lock);
    host_bans_ensure(host);
    for (size_t idx = 0; idx < host->ban_count; ++idx) {
        if (strncmp(host->bans[idx].username, token,
                    SSH_CHATTER_USERNAME_LEN) == 0 ||
            strncmp(host->bans[idx].ip, token, SSH_CHATTER_IP_LEN) == 0) {
            for (size_t shift = idx; shift + 1U < host->ban_count; ++shift) {
                host->bans[shift] = host->bans[shift + 1U];
            }
            memset(&host->bans[host->ban_count - 1U], 0,
                   sizeof(host->bans[host->ban_count - 1U]));
            --host->ban_count;
            removed = true;
            host_ban_state_save_locked(host);
            break;
        }
    }
    ttak_mutex_unlock(&host->lock);

    return removed;
}

static bool
session_parse_localized_command(session_ctx_t *ctx,
                                const session_command_alias_t *alias,
                                const char *line, const char **arguments)
{
    if (alias == nullptr) {
        return false;
    }

    if (session_parse_command(line, alias->canonical, arguments)) {
        return true;
    }

    session_ui_language_t language = session_ui_language_current(ctx);
    const char *preferred = session_command_alias_for_language(alias, language);
    if (preferred != nullptr && strcmp(preferred, alias->canonical) != 0) {
        if (session_parse_command(line, preferred, arguments)) {
            return true;
        }
    }

    for (size_t idx = 0; idx < SESSION_UI_LANGUAGE_COUNT; ++idx) {
        const char *localized = alias->localized[idx];
        if (localized == nullptr || localized[0] == '\0' ||
            strcmp(localized, alias->canonical) == 0) {
            continue;
        }
        if (session_parse_command(line, localized, arguments)) {
            return true;
        }
    }

    return false;
}

static bool session_parse_command(const char *line, const char *command,
                                  const char **arguments)
{
    if (line == nullptr || command == nullptr) {
        return false;
    }
    size_t command_len = strlen(command);

    if (strncmp(line, command, command_len) == 0) {
        const char boundary = line[command_len];
        if (boundary != '\0' && boundary != ' ' && boundary != '\t') {
            return false;
        }

        const char *args = line + command_len;

        while (*args == ' ' || *args == '\t') {
            ++args;
        }

        *arguments = args;
        return true;
    }
    return false;
}

static void session_handle_set_lf(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (arguments == nullptr || arguments[0] == '\0') {
        session_send_system_line(ctx, "Usage: /set-lf <auto|lf|crlf>");
        return;
    }

    char mode[16];
    const char *unused = session_consume_token(arguments, mode, sizeof(mode));
    (void)unused;

    if (strcasecmp(mode, "auto") == 0) {
        ctx->newline_mode = SESSION_NEWLINE_MODE_AUTO;
        session_send_system_line(ctx, "Line ending mode set to auto.");
        return;
    }
    if (strcasecmp(mode, "lf") == 0) {
        ctx->newline_mode = SESSION_NEWLINE_MODE_LF;
        session_send_system_line(ctx, "Line ending mode set to LF.");
        return;
    }
    if (strcasecmp(mode, "crlf") == 0) {
        ctx->newline_mode = SESSION_NEWLINE_MODE_CRLF;
        session_send_system_line(ctx, "Line ending mode set to CRLF.");
        return;
    }
}

static void session_handle_ddial(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    ddial_relay_t *relay = &ctx->owner->ddial_relay;

    if (arguments == nullptr || arguments[0] == '\0') {
        char status[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(status, sizeof(status),
                 "DDial relay: %s | Host: %s:%d",
                 relay->enabled && relay->connected ? "\033[1;32mconnected\033[0m"
                 : relay->enabled ? "\033[1;33mconnecting\033[0m"
                 : "\033[1;31moffline\033[0m",
                 relay->host[0] != '\0' ? relay->host : "(none)",
                 relay->port);
        session_send_system_line(ctx, status);
        session_send_system_line(
            ctx, "Usage: /ddial <connect <host> <port> [key]|disconnect|status>");
        return;
    }

    char action[32];
    const char *rest = session_consume_token(arguments, action, sizeof(action));

    if (strcasecmp(action, "status") == 0) {
        char status[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(status, sizeof(status),
                 "DDial relay: %s | Host: %s:%d",
                 relay->enabled && relay->connected
                     ? "\033[1;32mconnected\033[0m"
                     : relay->enabled ? "\033[1;33mconnecting\033[0m"
                                      : "\033[1;31moffline\033[0m",
                 relay->host[0] != '\0' ? relay->host : "(none)",
                 relay->port);
        session_send_system_line(ctx, status);
        return;
    }

    if (strcasecmp(action, "disconnect") == 0) {
        host_ddial_client_disconnect(ctx->owner);
        session_send_system_line(ctx, "DDial relay disconnected.");
        return;
    }

    if (strcasecmp(action, "connect") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_send_system_line(
                ctx, "Usage: /ddial connect <host> <port> [key]");
            return;
        }

        char host_str[256];
        char port_str[16];
        const char *remaining =
            session_consume_token(rest, host_str, sizeof(host_str));
        remaining =
            session_consume_token(remaining, port_str, sizeof(port_str));

        if (host_str[0] == '\0' || port_str[0] == '\0') {
            session_send_system_line(
                ctx, "Usage: /ddial connect <host> <port> [key]");
            return;
        }

        char *endptr;
        long port_long = strtol(port_str, &endptr, 10);
        if (*endptr != '\0' || port_long <= 0 || port_long > 65535) {
            session_send_system_line(
                ctx, "Invalid port number. Port must be between 1 and 65535.");
            return;
        }

        const char *key = nullptr;
        if (remaining != nullptr && remaining[0] != '\0') {
            key = remaining;
            while (*key == ' ' || *key == '\t') {
                ++key;
            }
        }

        if (host_ddial_client_configure(ctx->owner, host_str, (int)port_long,
                                        key)) {
            session_send_system_line(
                ctx, "DDial relay configured and connecting...");
        } else {
            session_send_system_line(
                ctx, "Failed to configure DDial relay.");
        }
        return;
    }

    session_send_system_line(ctx, "Unknown /ddial subcommand.");
    session_send_system_line(
        ctx, "Usage: /ddial <connect <host> <port> [key]|disconnect|status>");
}

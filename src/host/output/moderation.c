}

static void session_handle_kick(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->user.is_operator) {
        session_send_system_line(
            ctx, session_command_localize(
                     ctx, "You are not allowed to kick users."));
        return;
    }

    if (arguments == nullptr || *arguments == '\0') {
        session_send_system_line(
            ctx, session_command_localize(ctx, "Usage: /kick <username>"));
        return;
    }

    char target_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(target_name, sizeof(target_name), "%s", arguments);
    trim_whitespace_inplace(target_name);

    if (target_name[0] == '\0') {
        session_send_system_line(
            ctx, session_command_localize(ctx, "Usage: /kick <username>"));
        return;
    }

    session_ctx_t *target =
        chat_room_find_user_ref(&ctx->owner->room, target_name);
    if (target == nullptr) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        session_command_snprintf(ctx, message, sizeof(message),
                                 "User '%s' is not connected.", target_name);
        session_send_system_line(ctx, message);
        return;
    }

    if (target == ctx) {
        session_send_system_line(
            ctx, session_command_localize(ctx, "You cannot kick yourself."));
        chat_room_release_user_ref(target);
        return;
    }

    char kicked_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(kicked_name, sizeof(kicked_name), "%s", target->user.name);

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice), "* [%s] has been kicked by [%s]",
             kicked_name, ctx->user.name);
    host_history_record_system(ctx->owner, notice, nullptr);
    chat_room_broadcast(&ctx->owner->room, notice, nullptr);

    if (session_transport_active(target)) {
        session_send_system_line(
            target, session_command_localize(
                        target, "You have been kicked by an operator."));
        session_transport_request_close(target);
    }
    target->should_exit = true;
    chat_room_release_user_ref(target);
    session_send_system_line(
        ctx, session_command_localize(ctx, "User removed from the chat."));

    printf("[kick] %s kicked %s\n", ctx->user.name, kicked_name);
}

static void session_handle_ban_name(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (!ctx->user.is_operator) {
        session_send_system_line(
            ctx, session_command_localize(
                     ctx, "You are not allowed to ban nicknames."));
        return;
    }

    if (ctx->owner == nullptr) {
        session_send_system_line(
            ctx, session_command_localize(ctx, "Host unavailable."));
        return;
    }

    if (arguments == nullptr || *arguments == '\0') {
        session_send_system_line(
            ctx, session_command_localize(ctx, "Usage: /banname <nickname>"));
        return;
    }

    char target_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(target_name, sizeof(target_name), "%s", arguments);
    trim_whitespace_inplace(target_name);

    if (target_name[0] == '\0') {
        session_send_system_line(
            ctx, session_command_localize(ctx, "Usage: /banname <nickname>"));
        return;
    }

    for (size_t idx = 0U; target_name[idx] != '\0'; ++idx) {
        const unsigned char ch = (unsigned char)target_name[idx];
        if (ch <= 0x1FU || ch == 0x7FU || ch == ' ' || ch == '\t') {
            session_send_system_line(
                ctx, session_command_localize(
                         ctx, "Nicknames may not include control characters or "
                              "whitespace."));
            return;
        }
    }

    if (host_is_username_banned(ctx->owner, target_name)) {
        session_send_system_line(
            ctx,
            session_command_localize(
                ctx, "That nickname is already blocked for bot detection."));
        return;
    }

    if (!host_add_ban_entry(ctx->owner, target_name, "")) {
        session_send_system_line(
            ctx, session_command_localize(
                     ctx, "Unable to add ban entry (list full?)."));
        return;
    }

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice),
             "* Nickname '%s' blocked for bot detection by [%s]", target_name,
             ctx->user.name);
    host_history_record_system(ctx->owner, notice, nullptr);
    chat_room_broadcast(&ctx->owner->room, notice, nullptr);
    session_send_system_line(
        ctx, session_command_localize(ctx, "Nickname ban applied."));
    printf("[banname] %s banned nickname %s\n", ctx->user.name, target_name);

    session_ctx_t *active = chat_room_find_user(&ctx->owner->room, target_name);
    if (active != nullptr) {
        session_send_system_line(
            active, session_command_localize(
                        active, "Your nickname is now blocked for bot "
                                "detection. Use /nick <name> to change "
                                "immediately."));
    }
}

static void session_handle_ban(session_ctx_t *ctx, const char *arguments)
{
    if (!ctx->user.is_operator) {
        session_send_system_line(
            ctx,
            session_command_localize(ctx, "You are not allowed to ban users."));
        return;
    }

    if (arguments == nullptr || *arguments == '\0') {
        session_send_system_line(
            ctx, session_command_localize(ctx, "Usage: /ban <username>"));
        return;
    }

    char target_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(target_name, sizeof(target_name), "%s", arguments);
    trim_whitespace_inplace(target_name);

    if (target_name[0] == '\0') {
        session_send_system_line(
            ctx, session_command_localize(ctx, "Usage: /ban <username>"));
        return;
    }

    session_ctx_t *target = chat_room_find_user(&ctx->owner->room, target_name);
    if (target == nullptr) {
        bool valid_ip = false;
        unsigned char inet_buffer[sizeof(struct in6_addr)];
        if (inet_pton(AF_INET, target_name, inet_buffer) == 1 ||
            inet_pton(AF_INET6, target_name, inet_buffer) == 1) {
            valid_ip = true;
        }

        bool valid_cidr = false;
        if (!valid_ip && strchr(target_name, '/') != nullptr) {
            uint32_t ipv4_network = 0U;
            uint32_t ipv4_mask = 0U;
            struct in6_addr ipv6_network;
            struct in6_addr ipv6_mask;
            memset(&ipv6_network, 0, sizeof(ipv6_network));
            memset(&ipv6_mask, 0, sizeof(ipv6_mask));
            valid_cidr =
                host_parse_ipv4_cidr(target_name, &ipv4_network, &ipv4_mask) ||
                host_parse_ipv6_cidr(target_name, &ipv6_network, &ipv6_mask);
        }

        if (valid_ip || valid_cidr) {
            if (host_add_ban_entry(ctx->owner, "", target_name)) {
                char notice[SSH_CHATTER_MESSAGE_LIMIT];
                const char *label = valid_cidr ? "CIDR" : "IP";
                session_command_snprintf(ctx, notice, sizeof(notice),
                                         "%s '%s' has been banned.", label,
                                         target_name);
                session_send_system_line(ctx, notice);
            } else {
                session_send_system_line(
                    ctx, session_command_localize(
                             ctx, "Unable to add ban entry (list full?)."));
            }
        } else {
            char not_found[SSH_CHATTER_MESSAGE_LIMIT];
            session_command_snprintf(ctx, not_found, sizeof(not_found),
                                     "User '%s' is not connected.",
                                     target_name);
            session_send_system_line(ctx, not_found);
        }
        return;
    }

    if (target->user.is_lan_operator) {
        session_send_system_line(
            ctx,
            session_command_localize(ctx, "LAN operators cannot be banned."));
        return;
    }

    const char *target_ip =
        target->client_ip[0] != '\0' ? target->client_ip : "";
    if (!host_add_ban_entry(ctx->owner, target->user.name, target_ip)) {
        session_send_system_line(
            ctx, session_command_localize(
                     ctx, "Unable to add ban entry (list full?)."));
        return;
    }

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice), "* [%s] has been banned by [%s]",
             target->user.name, ctx->user.name);
    host_history_record_system(ctx->owner, notice, nullptr);
    chat_room_broadcast(&ctx->owner->room, notice, nullptr);
    session_send_system_line(ctx,
                             session_command_localize(ctx, "Ban applied."));
    printf("[ban] %s banned %s (%s)\n", ctx->user.name, target->user.name,
           target_ip[0] != '\0' ? target_ip : "unknown");

    if (session_transport_active(target)) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        session_command_snprintf(target, message, sizeof(message),
                                 "You have been banned by [%s].",
                                 ctx->user.name);
        session_send_system_line(target, message);
        target->should_exit = true;
        session_transport_request_close(target);
    }
}

static void session_handle_ban_list(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (!ctx->user.is_operator) {
        session_send_system_line(
            ctx, session_command_localize(
                     ctx, "You are not allowed to view the ban list."));
        return;
    }

    if (arguments != nullptr) {
        while (*arguments != '\0' && isspace((unsigned char)*arguments)) {
            ++arguments;
        }
        if (*arguments != '\0') {
            session_send_system_line(
                ctx, session_command_localize(ctx, "Usage: /banlist"));
            return;
        }
    }

    host_t *host = ctx->owner;
    if (host == nullptr) {
        session_send_system_line(
            ctx, session_command_localize(ctx, "Host unavailable."));
        return;
    }

    typedef struct ban_snapshot {
        char username[SSH_CHATTER_USERNAME_LEN];
        char ip[SSH_CHATTER_IP_LEN];
    } ban_snapshot_t;

    ban_snapshot_t entries[SSH_CHATTER_MAX_BANS];
    size_t entry_count = 0U;

    ttak_mutex_lock(&host->lock);
    entry_count = host->ban_count;
    if (entry_count > SSH_CHATTER_MAX_BANS) {
        entry_count = SSH_CHATTER_MAX_BANS;
    }
    for (size_t idx = 0U; idx < entry_count; ++idx) {
        snprintf(entries[idx].username, sizeof(entries[idx].username), "%s",
                 host->bans[idx].username);
        snprintf(entries[idx].ip, sizeof(entries[idx].ip), "%s",
                 host->bans[idx].ip);
    }
    ttak_mutex_unlock(&host->lock);

    if (entry_count == 0U) {
        session_send_system_line(
            ctx, session_command_localize(ctx, "No active bans."));
        return;
    }

    session_send_system_line(ctx,
                             session_command_localize(ctx, "Active bans:"));
    enum {
        SESSION_BAN_USERNAME_PREC = SSH_CHATTER_USERNAME_LEN - 1,
        SESSION_BAN_IP_PREC = SSH_CHATTER_IP_LEN - 1
    };
    for (size_t idx = 0U; idx < entry_count; ++idx) {
        const char *username = entries[idx].username;
        const char *ip = entries[idx].ip;
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        if (username[0] != '\0' && ip[0] != '\0') {
            session_command_snprintf(ctx, message, sizeof(message),
                                     "%zu. user: %.*s, ip: %.*s", idx + 1U,
                                     SESSION_BAN_USERNAME_PREC, username,
                                     SESSION_BAN_IP_PREC, ip);
        } else if (username[0] != '\0') {
            session_command_snprintf(ctx, message, sizeof(message),
                                     "%zu. user: %.*s", idx + 1U,
                                     SESSION_BAN_USERNAME_PREC, username);
        } else if (ip[0] != '\0') {
            session_command_snprintf(ctx, message, sizeof(message),
                                     "%zu. ip: %.*s", idx + 1U,
                                     SESSION_BAN_IP_PREC, ip);
        } else {
            session_command_snprintf(ctx, message, sizeof(message),
                                     "%zu. <empty>", idx + 1U);
        }
        session_send_system_line(ctx, message);
    }
}

static void session_handle_getaddr(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(
            ctx, session_command_localize(
                     ctx, "You are not allowed to run that command."));
        return;
    }

    if (arguments == nullptr || *arguments == '\0') {
        session_send_system_line(
            ctx, session_command_localize(ctx, "Usage: /getaddr <username>"));
        return;
    }

    char target_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(target_name, sizeof(target_name), "%s", arguments);
    trim_whitespace_inplace(target_name);

    if (target_name[0] == '\0') {
        session_send_system_line(
            ctx, session_command_localize(ctx, "Usage: /getaddr <username>"));
        return;
    }

    host_t *host = ctx->owner;
    if (host == nullptr) {
        session_send_system_line(
            ctx, session_command_localize(ctx, "Host unavailable."));
        return;
    }

    char ip[SSH_CHATTER_IP_LEN];
    if (!host_lookup_last_ip(host, target_name, ip, sizeof(ip)) ||
        ip[0] == '\0') {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        session_command_snprintf(ctx, message, sizeof(message),
                                 "No recorded address for '%s'.", target_name);
        session_send_system_line(ctx, message);
        return;
    }

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_snprintf(ctx, message, sizeof(message),
                             "Last known address for '%s': %s", target_name,
                             ip);
    session_send_system_line(ctx, message);
}

static void session_handle_poke(session_ctx_t *ctx, const char *arguments)
{
    if (arguments == nullptr || *arguments == '\0') {
        session_send_system_line(
            ctx, session_command_localize(ctx, "Usage: /poke <username>"));
        return;
    }

    session_ctx_t *target = chat_room_find_user(&ctx->owner->room, arguments);
    if (target == nullptr) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        session_command_snprintf(ctx, message, sizeof(message),
                                 "User '%s' is not connected.", arguments);
        session_send_system_line(ctx, message);
        return;
    }

    printf("[poke] %s pokes %s\n", ctx->user.name, target->user.name);
    session_channel_write(target, "\a", 1U);
    session_send_system_line(ctx, session_command_localize(ctx, "Poke sent."));
}

// Localized usage for /pm; the "/pm" token is later swapped for the alias.
static const char *session_pm_usage_template(session_ctx_t *ctx)
{
    switch (session_ui_language_current(ctx)) {
    case SESSION_UI_LANGUAGE_KO:
        return "사용법: /pm <사용자>|<메시지>";
    case SESSION_UI_LANGUAGE_JP:
        return "使い方: /pm <ユーザー>|<メッセージ>";
    case SESSION_UI_LANGUAGE_ZH:
        return "用法: /pm <用户>|<消息>";
    case SESSION_UI_LANGUAGE_RU:
        return "Использование: /pm <пользователь>|<сообщение>";
    case SESSION_UI_LANGUAGE_DE:
        return "Nutzung: /pm <Benutzer>|<Nachricht>";
    case SESSION_UI_LANGUAGE_FR:
        return "Utilisation: /pm <utilisateur>|<message>";
    case SESSION_UI_LANGUAGE_PL:
        return "Użycie: /pm <użytkownik>|<wiadomość>";
    default:
        return "Usage: /pm <username>|<message>";
    }
}

// Localized usage for /block; the "/block" token is later swapped for the
// alias.
static const char *session_block_usage_template(session_ctx_t *ctx)
{
    switch (session_ui_language_current(ctx)) {
    case SESSION_UI_LANGUAGE_KO:
        return "사용법: /block <사용자|IP|list|confirm <사용자>|<only|ip>>";
    case SESSION_UI_LANGUAGE_JP:
        return "使い方: /block <ユーザー|IP|list|confirm <ユーザー>|<only|ip>>";
    case SESSION_UI_LANGUAGE_ZH:
        return "用法: /block <用户|IP|list|confirm <用户>|<only|ip>>";
    case SESSION_UI_LANGUAGE_RU:
        return "Использование: /block <пользователь|IP|list|confirm "
               "<пользователь>|<only|ip>>";
    case SESSION_UI_LANGUAGE_DE:
        return "Nutzung: /block <Benutzer|IP|list|confirm "
               "<Benutzer>|<only|ip>>";
    case SESSION_UI_LANGUAGE_FR:
        return "Utilisation: /block <utilisateur|IP|list|confirm "
               "<utilisateur>|<only|ip>>";
    case SESSION_UI_LANGUAGE_PL:
        return "Użycie: /block <użytkownik|IP|list|confirm "
               "<użytkownik>|<only|ip>>";
    default:
        return "Usage: /block <username|ip|list|confirm <username>|<only|ip>>";
    }
}

// Explains how to confirm a block on a shared-provider IP.
static void session_block_format_confirm_prompt(session_ctx_t *ctx,
                                                const char *name, char *buffer,
                                                size_t length)
{
    switch (session_ui_language_current(ctx)) {
    case SESSION_UI_LANGUAGE_KO:
        snprintf(buffer, length,
                 "[%.23s]만 숨기려면 /block confirm %.23s|only, 해당 IP의 "
                 "모든 사용자를 숨기려면 /block confirm %.23s|ip 를 "
                 "입력하세요.",
                 name, name, name);
        break;
    case SESSION_UI_LANGUAGE_JP:
        snprintf(buffer, length,
                 "[%.23s]だけを非表示にするには /block confirm %.23s|only、"
                 "そのIPの全員を非表示にするには /block confirm %.23s|ip "
                 "と入力してください。",
                 name, name, name);
        break;
    case SESSION_UI_LANGUAGE_ZH:
        snprintf(buffer, length,
                 "仅屏蔽 [%.23s] 请输入 /block confirm %.23s|only，屏蔽该 IP "
                 "的所有人请输入 /block confirm %.23s|ip。",
                 name, name, name);
        break;
    case SESSION_UI_LANGUAGE_RU:
        snprintf(buffer, length,
                 "Чтобы скрыть только [%.23s], введите /block confirm "
                 "%.23s|only, а чтобы скрыть всех с этого IP — /block "
                 "confirm %.23s|ip.",
                 name, name, name);
        break;
    case SESSION_UI_LANGUAGE_DE:
        snprintf(buffer, length,
                 "Nur [%.23s] ausblenden: /block confirm %.23s|only; alle "
                 "von dieser IP ausblenden: /block confirm %.23s|ip.",
                 name, name, name);
        break;
    case SESSION_UI_LANGUAGE_FR:
        snprintf(buffer, length,
                 "Pour masquer seulement [%.23s] : /block confirm %.23s|only ; "
                 "pour masquer tout le monde depuis cette IP : /block "
                 "confirm %.23s|ip.",
                 name, name, name);
        break;
    case SESSION_UI_LANGUAGE_PL:
        snprintf(buffer, length,
                 "Aby ukryć tylko [%.23s], wpisz /block confirm %.23s|only; "
                 "aby ukryć wszystkich z tego IP: /block confirm %.23s|ip.",
                 name, name, name);
        break;
    default:
        snprintf(buffer, length,
                 "Use /block confirm %.23s|only to hide just [%.23s] or /block "
                 "confirm %.23s|ip to hide everyone from that IP.",
                 name, name, name);
        break;
    }
}

static void session_handle_block(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(
        ctx, "/block", session_block_usage_template(ctx), usage, sizeof(usage));

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

    if (strcasecmp(working, "list") == 0) {
        session_blocklist_show(ctx);
        return;
    }

    if (strncasecmp(working, "confirm", 7) == 0 &&
        (working[7] == '\0' || isspace((unsigned char)working[7]))) {
        char *cursor = working + 7;
        while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
            ++cursor;
        }

        if (*cursor == '\0') {
            session_send_system_line(ctx, usage);
            return;
        }

        // "confirm <username>|<mode>"; whitespace split kept as a fallback.
        char *name_end = strrchr(cursor, '|');
        if (name_end == nullptr) {
            name_end = cursor;
            while (*name_end != '\0' && !isspace((unsigned char)*name_end)) {
                ++name_end;
            }
        }
        char username[SSH_CHATTER_USERNAME_LEN];
        size_t name_len = (size_t)(name_end - cursor);
        if (name_len >= sizeof(username)) {
            name_len = sizeof(username) - 1U;
        }
        memcpy(username, cursor, name_len);
        username[name_len] = '\0';
        trim_whitespace_inplace(username);
        cursor = name_end;
        if (*cursor == '|') {
            ++cursor;
        }

        while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
            ++cursor;
        }

        if (*cursor == '\0') {
            session_send_system_line(ctx, usage);
            return;
        }

        char mode[16];
        size_t mode_len = 0U;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor) &&
               mode_len + 1U < sizeof(mode)) {
            mode[mode_len++] = *cursor++;
        }
        mode[mode_len] = '\0';

        if (!ctx->block_pending.active) {
            session_send_system_line(
                ctx, session_command_localize(
                         ctx, "No provider block is awaiting confirmation."));
            return;
        }

        if (strncmp(ctx->block_pending.username, username,
                    SSH_CHATTER_USERNAME_LEN) != 0) {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            session_command_snprintf(ctx, message, sizeof(message),
                                     "Pending block is for [%s], not [%s].",
                                     ctx->block_pending.username, username);
            session_send_system_line(ctx, message);
            return;
        }

        bool block_ip = false;
        if (strcasecmp(mode, "ip") == 0 || strcasecmp(mode, "all") == 0 ||
            strcasecmp(mode, "full") == 0) {
            block_ip = true;
        } else if (strcasecmp(mode, "only") == 0 ||
                   strcasecmp(mode, "user") == 0 ||
                   strcasecmp(mode, "name") == 0) {
            block_ip = false;
        } else {
            session_send_system_line(ctx, usage);
            return;
        }

        bool already_present = false;
        if (!session_blocklist_add(ctx, ctx->block_pending.ip,
                                   ctx->block_pending.username, block_ip,
                                   &already_present)) {
            if (already_present) {
                session_send_system_line(
                    ctx, session_command_localize(
                             ctx, "That target is already blocked."));
            } else {
                session_send_system_line(
                    ctx,
                    session_command_localize(
                        ctx, "Unable to add block entry (limit reached?)."));
            }
        } else {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            if (block_ip) {
                session_command_snprintf(ctx, message, sizeof(message),
                                         "Blocking all users from %.63s.",
                                         ctx->block_pending.ip);
            } else {
                session_command_snprintf(ctx, message, sizeof(message),
                                         "Blocking [%.23s] only (IP %.63s).",
                                         ctx->block_pending.username,
                                         ctx->block_pending.ip);
            }
            session_send_system_line(ctx, message);
        }

        ctx->block_pending.active = false;
        ctx->block_pending.username[0] = '\0';
        ctx->block_pending.ip[0] = '\0';
        ctx->block_pending.provider_label[0] = '\0';
        return;
    }

    unsigned char inet_buffer[sizeof(struct in6_addr)];
    if (inet_pton(AF_INET, working, inet_buffer) == 1 ||
        inet_pton(AF_INET6, working, inet_buffer) == 1) {
        bool already_present = false;
        char label[64];
        bool provider =
            session_detect_provider_ip(working, label, sizeof(label));
        if (provider && label[0] != '\0') {
            char warning[SSH_CHATTER_MESSAGE_LIMIT];
            session_command_snprintf(
                ctx, warning, sizeof(warning),
                "Error: You cannot ban a country. "
                "%.256s is flagged as %.63s; other people may also be hidden.",
                working, label);
            session_send_system_line(ctx, warning);
            return;
        }
        if (!session_blocklist_add(ctx, working, "", true, &already_present)) {
            if (already_present) {
                session_send_system_line(
                    ctx, session_command_localize(
                             ctx, "That IP is already blocked."));
            } else {
                session_send_system_line(
                    ctx,
                    session_command_localize(
                        ctx, "Unable to add block entry (limit reached?)."));
            }
        } else {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            session_command_snprintf(ctx, message, sizeof(message),
                                     "Blocking all users from %.256s.",
                                     working);
            session_send_system_line(ctx, message);
        }
        return;
    }

    if (ctx->owner == nullptr) {
        session_send_system_line(
            ctx,
            session_command_localize(ctx, "Block list unavailable right now."));
        return;
    }

    session_ctx_t *target = chat_room_find_user(&ctx->owner->room, working);
    if (target == nullptr) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        session_command_snprintf(ctx, message, sizeof(message),
                                 "User '%.256s' is not connected.", working);
        session_send_system_line(ctx, message);
        return;
    }

    if (target == ctx) {
        session_send_system_line(
            ctx, session_command_localize(
                     ctx, "You do not need to block yourself."));
        return;
    }

    if (target->client_ip[0] == '\0') {
        session_send_system_line(
            ctx,
            session_command_localize(
                ctx, "Unable to identify that user's IP address right now."));
        return;
    }

    char label[64];
    if (session_detect_provider_ip(target->client_ip, label, sizeof(label))) {
        memset(&ctx->block_pending, 0, sizeof(ctx->block_pending));
        ctx->block_pending.active = true;
        snprintf(ctx->block_pending.username,
                 sizeof(ctx->block_pending.username), "%s", target->user.name);
        snprintf(ctx->block_pending.ip, sizeof(ctx->block_pending.ip), "%s",
                 target->client_ip);
        snprintf(ctx->block_pending.provider_label,
                 sizeof(ctx->block_pending.provider_label), "%.31s", label);

        char warning[SSH_CHATTER_MESSAGE_LIMIT];
        session_command_snprintf(ctx, warning, sizeof(warning),
                                 "%.63s appears to belong to %.63s.",
                                 target->client_ip, label);
        session_send_system_line(ctx, warning);

        char prompt[SSH_CHATTER_MESSAGE_LIMIT];
        session_block_format_confirm_prompt(ctx, target->user.name, prompt,
                                            sizeof(prompt));
        session_send_system_line(ctx, prompt);
        return;
    }

    bool already_present = false;
    if (!session_blocklist_add(ctx, target->client_ip, target->user.name, true,
                               &already_present)) {
        if (already_present) {
            session_send_system_line(
                ctx, session_command_localize(
                         ctx, "That address is already blocked."));
        } else {
            session_send_system_line(
                ctx, session_command_localize(
                         ctx, "Unable to add block entry (limit reached?)."));
        }
    } else {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        session_command_snprintf(
            ctx, message, sizeof(message),
            "Blocking all users from %.63s (triggered by [%.23s]).",
            target->client_ip, target->user.name);
        session_send_system_line(ctx, message);
    }
}

static void session_handle_unblock(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    static const char *kUsage = "Usage: /unblock <username|ip|all>";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/unblock",
                                 session_command_localize(ctx, kUsage), usage,
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

    if (strcasecmp(working, "all") == 0) {
        size_t removed = 0U;
        for (size_t idx = 0U; idx < SSH_CHATTER_MAX_BLOCKED; ++idx) {
            if (ctx->block_entries[idx].in_use) {
                memset(&ctx->block_entries[idx], 0,
                       sizeof(ctx->block_entries[idx]));
                ++removed;
            }
        }
        ctx->block_entry_count = 0U;
        ctx->block_pending.active = false;
        ctx->block_pending.username[0] = '\0';
        ctx->block_pending.ip[0] = '\0';
        ctx->block_pending.provider_label[0] = '\0';

        if (removed == 0U) {
            session_send_system_line(
                ctx,
                session_command_localize(ctx, "No blocked entries to remove."));
        } else {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            if (removed == 1U) {
                snprintf(
                    message, sizeof(message), "%s",
                    session_command_localize(ctx, "Removed 1 blocked entry."));
            } else {
                session_command_snprintf(ctx, message, sizeof(message),
                                         "Removed %zu blocked entries.",
                                         removed);
            }
            session_send_system_line(ctx, message);
        }
        return;
    }

    if (session_blocklist_remove(ctx, working)) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        session_command_snprintf(ctx, message, sizeof(message),
                                 "Removed block for %.256s.", working);
        session_send_system_line(ctx, message);
    } else {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        session_command_snprintf(ctx, message, sizeof(message),
                                 "No block entry matched '%.256s'.", working);
        session_send_system_line(ctx, message);
    }
}

static void session_handle_pm(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/pm", session_pm_usage_template(ctx),
                                 usage, sizeof(usage));

    if (ctx->owner == nullptr) {
        session_send_system_line(
            ctx, session_command_localize(
                     ctx, "Private messages are unavailable right now."));
        return;
    }

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

    // Names may contain spaces, so '|' separates the name from the message.
    // Without '|', fall back to splitting at the first whitespace.
    char *cursor = strchr(working, '|');
    if (cursor == nullptr) {
        cursor = working;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
            ++cursor;
        }
    }

    if (*cursor == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    *cursor = '\0';
    char *message = cursor + 1;
    trim_whitespace_inplace(working);
    while (*message != '\0' && isspace((unsigned char)*message)) {
        ++message;
    }

    if (working[0] == '\0' || *message == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    char target_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(target_name, sizeof(target_name), "%.*s",
             (int)sizeof(target_name) - 1, working);

    session_ctx_t *target = chat_room_find_user(&ctx->owner->room, target_name);
    if (target == nullptr) {
        char not_found[SSH_CHATTER_MESSAGE_LIMIT];
        session_command_snprintf(ctx, not_found, sizeof(not_found),
                                 "User '%s' is not connected.", target_name);
        session_send_system_line(ctx, not_found);
        return;
    }

    char prepared[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(prepared, sizeof(prepared), "%s", message);

    char stripped[SSH_CHATTER_MESSAGE_LIMIT];
    bool translation_bypass = translation_strip_no_translate_prefix(
        prepared, stripped, sizeof(stripped));
    const char *deliver_body = translation_bypass ? stripped : prepared;

    const char *target_display = target->user.name;
    printf("[pm] %s -> %s: %s\n", ctx->user.name, target_display, deliver_body);

    char to_target_label[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_snprintf(target, to_target_label, sizeof(to_target_label),
                             "%s -> you", ctx->user.name);

    char to_sender_label[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_snprintf(ctx, to_sender_label, sizeof(to_sender_label),
                             "you -> %s", target_display);

    bool attempt_translation = (target != nullptr) && !translation_bypass &&
                               ctx->translation_enabled &&
                               ctx->input_translation_enabled &&
                               ctx->input_translation_language[0] != '\0';

    if (attempt_translation) {
        if (session_translation_queue_private_message(ctx, target,
                                                      deliver_body)) {
            return;
        }
        session_send_system_line(
            ctx,
            session_command_localize(
                ctx,
                "Translation unavailable; sending your original message."));
    }

    session_send_private_message_line(target, ctx, to_target_label,
                                      deliver_body);
    session_send_private_message_line(ctx, ctx, to_sender_label, deliver_body);
}

static bool username_contains(const char *username, const char *needle)
{
    if (username == nullptr || needle == nullptr) {
        return false;
    }

    const size_t needle_len = strlen(needle);
    if (needle_len == 0U) {
        return false;
    }

    const size_t name_len = strlen(username);
    if (needle_len > name_len) {
        return false;
    }

    for (size_t offset = 0U; offset + needle_len <= name_len; ++offset) {
        bool match = true;
        for (size_t idx = 0U; idx < needle_len; ++idx) {
            const unsigned char user_ch = (unsigned char)username[offset + idx];
            const unsigned char needle_ch = (unsigned char)needle[idx];
            if (tolower(user_ch) != tolower(needle_ch)) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }

    return false;
}

static void session_handle_search(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (ctx->owner == nullptr) {
        session_send_system_line(ctx, "Search is unavailable at the moment.");
        return;
    }

    if (arguments == nullptr) {
        session_send_system_line(ctx, "Usage: /search <text>");
        return;
    }

    char query[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(query, sizeof(query), "%s", arguments);
    trim_whitespace_inplace(query);

    if (query[0] == '\0') {
        session_send_system_line(ctx, "Usage: /search <text>");
        return;
    }

    char listing[SSH_CHATTER_MESSAGE_LIMIT];
    listing[0] = '\0';
    size_t match_count = 0U;

    ttak_mutex_lock(&ctx->owner->room.lock);
    for (size_t idx = 0U; idx < ctx->owner->room.member_count; ++idx) {
        session_ctx_t *member = ctx->owner->room.members[idx];
        if (member == nullptr) {
            continue;
        }
        if (!username_contains(member->user.name, query)) {
            continue;
        }

        char name[SSH_CHATTER_USERNAME_LEN];
        snprintf(name, sizeof(name), "%s", member->user.name);
        size_t current_len = strnlen(listing, sizeof(listing));
        size_t name_len = strnlen(name, sizeof(name));
        size_t prefix_len = (match_count == 0U) ? 0U : 2U;

        if (current_len + prefix_len + name_len >= sizeof(listing)) {
            continue;
        }

        if (match_count > 0U) {
            listing[current_len++] = ',';
            listing[current_len++] = ' ';
        }
        memcpy(listing + current_len, name, name_len);
        listing[current_len + name_len] = '\0';
        ++match_count;
    }
    ttak_mutex_unlock(&ctx->owner->room.lock);

    if (match_count == 0U) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        char display_query[64];
        size_t copy_len = strnlen(query, sizeof(display_query) - 1U);
        memcpy(display_query, query, copy_len);
        display_query[copy_len] = '\0';
        snprintf(message, sizeof(message), "No users matching '%s'.",
                 display_query);
        session_send_system_line(ctx, message);
        return;
    }

    char header[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(header, sizeof(header), "Matching users (%zu):", match_count);
    session_send_system_line(ctx, header);
    session_send_system_line(ctx, listing);
}

void session_channel_write(session_ctx_t *ctx, const void *data, size_t length)
{
    if (ctx == nullptr || data == nullptr || length == 0U || ctx->should_exit ||
        !session_transport_active(ctx)) {
        return;
    }

    // If output buffering is enabled, append to buffer instead of writing directly
    if (ctx->output_buffering_enabled) {
        session_output_buffer_append(ctx, data, length);
        return;
    }

    bool locked = session_output_lock(ctx);

    bool success = true;
    bool channel_mutex_locked = false;
    if (ctx->channel_mutex_initialized) {
        int lock_result = ttak_mutex_lock(&ctx->channel_mutex);
        if (lock_result == 0) {
            channel_mutex_locked = true;
        } else {
            humanized_log_error("session", "failed to lock channel mutex",
                                lock_result);
        }
    }

    const bool use_retro_output =
        session_output_should_use_retro_encoding(ctx, ctx->output_kind);

    const void *write_data = data;
    size_t write_length = length;

    size_t nul_count = 0U;
    const unsigned char *inspect = (const unsigned char *)write_data;
    for (size_t idx = 0U; idx < write_length; ++idx) {
        if (inspect[idx] == '\0') {
            ++nul_count;
        }
    }

    unsigned char *sanitized = nullptr;
    if (!ctx->prefer_utf16_output && nul_count > 0U) {
        sanitized = (unsigned char *)sshc_gc_malloc(write_length);
        if (sanitized != nullptr) {
            size_t out = 0U;
            for (size_t idx = 0U; idx < write_length; ++idx) {
                if (inspect[idx] != '\0') {
                    sanitized[out++] = inspect[idx];
                }
            }
            write_data = sanitized;
            write_length = out;
        }
    }

    bool prefer_utf8_for_hybrid = false;
    if (ctx->hybrid_output_mode && use_retro_output &&
        ctx->output_kind != SESSION_OUTPUT_KIND_SYSTEM) {
        prefer_utf8_for_hybrid =
            session_output_requires_utf8((const char *)write_data, write_length);
    }

    if (use_retro_output && !prefer_utf8_for_hybrid) {
        /* Use the generic codepage conversion with the active codepage */
        success = session_channel_write_codepage(
            ctx, (const char *)write_data, write_length, ctx->active_codepage);
    } else if (ctx->prefer_utf16_output) {
        success =
            session_channel_write_utf16(ctx, (const char *)write_data, write_length);
    } else {
        success = session_channel_write_all(ctx, write_data, write_length);
    }

    if (sanitized != nullptr) {
        sshc_gc_free(sanitized);
    }
    if (channel_mutex_locked) {
        int unlock_result = ttak_mutex_unlock(&ctx->channel_mutex);
        if (unlock_result != 0) {
            humanized_log_error("session", "failed to unlock channel mutex",
                                unlock_result);
        }
    }

    if (!success) {
        ctx->should_exit = true;
    }

    if (locked) {
        session_output_unlock(ctx);
    }
}

static void session_handle_chat_lookup(session_ctx_t *ctx,
                                       const char *arguments)
{
    static const char *kUsage = "Usage: /chat <message-id>";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/chat", kUsage, usage, sizeof(usage));

    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (arguments == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    char working[64];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    uint64_t message_id = 0U;
    if (!host_compact_id_decode(working, &message_id) || message_id == 0U) {
        session_send_system_line(ctx, usage);
        return;
    }

    chat_history_entry_t entry = {0};
    if (!host_history_find_entry_by_id(ctx->owner, message_id, &entry)) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        char label[32];
        if (!host_compact_id_encode(message_id, label, sizeof(label))) {
            snprintf(label, sizeof(label), "%" PRIu64, message_id);
        }
        snprintf(message, sizeof(message), "Message #%s was not found.", label);
        session_send_system_line(ctx, message);
        return;
    }

    char header[SSH_CHATTER_MESSAGE_LIMIT];
    char header_label[32];
    if (!host_compact_id_encode(message_id, header_label,
                                sizeof(header_label))) {
        snprintf(header_label, sizeof(header_label), "%" PRIu64, message_id);
    }
    snprintf(header, sizeof(header), "Message #%s:", header_label);
    session_send_system_line(ctx, header);
    session_send_history_entry(ctx, &entry);
    session_send_reply_tree(ctx, entry.message_id, 0U, 1U);
}

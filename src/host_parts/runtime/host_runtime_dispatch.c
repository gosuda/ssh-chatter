    session_send_system_line(ctx, "Usage: /set-lf <auto|lf|crlf>");
}

static void session_dispatch_command(session_ctx_t *ctx, const char *line)
{
    if (ctx == nullptr || line == nullptr) {
        return;
    }

    const char *args = nullptr;
    const char *effective_line = line;

    // Handle double slash commands by effectively removing the first slash
    if (line != nullptr && line[0] == '/' && line[1] == '/') {
        effective_line = line + 1;
    }

    if (session_parse_command_any(ctx, "/help", effective_line, &args)) {
        session_print_help(ctx);
        return;
    }

    else if (session_parse_command_any(ctx, "/advanced", effective_line,
                                       &args)) {
        session_handle_advanced(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/sync-trigger", effective_line,
                                       &args)) {
        return;
    }

    else if (session_parse_command_any(ctx, "/filestore", effective_line,
                                       &args)) {
        session_handle_filestore(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/filestore-upload",
                                       effective_line, &args)) {
        session_handle_filestore_upload(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/filestore-download",
                                       effective_line, &args)) {
        session_handle_filestore_download(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/grant", effective_line, &args)) {
        session_handle_grant(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/shell", effective_line, &args)) {
        session_handle_shell(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/kick", effective_line, &args)) {
        session_handle_kick(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/set-sync-url", effective_line,
                                       &args)) {
        if (args == nullptr || *args == '\0') {
            session_send_system_line(ctx, "Usage: /set-sync-url <host> <port>");
            return;
        }

        char host_str[256];
        char port_str[16];
        const char *remaining_args =
            session_consume_token(args, host_str, sizeof(host_str));
        remaining_args =
            session_consume_token(remaining_args, port_str, sizeof(port_str));

        if (host_str[0] == '\0' || port_str[0] == '\0') {
            session_send_system_line(ctx, "Usage: /set-sync-url <host> <port>");
            return;
        }

        char *endptr;
        long port_long = strtol(port_str, &endptr, 10);
        if (*endptr != '\0' || port_long <= 0 || port_long > 65535) {
            session_send_system_line(
                ctx, "Invalid port number. Port must be between 1 and 65535.");
            return;
        }

        // For now, using default username and password. This can be extended later.
        ssh_chatter_sync_set_connection_details(host_str, (int)port_long,
                                                "chatter_sync", "password");
        ssh_chatter_sync_manual_trigger();
        session_send_system_line(
            ctx, "SSH sync URL updated. Attempting to reconnect...");
        return;
    }

    else if (session_parse_command_any(ctx, "/set-sync-url", effective_line,
                                       &args)) {
        if (args == nullptr || *args == '\0') {
            session_send_system_line(ctx, "Usage: /set-sync-url <host> <port>");
            return;
        }

        char host_str[256];
        char port_str[16];
        const char *remaining_args =
            session_consume_token(args, host_str, sizeof(host_str));
        remaining_args =
            session_consume_token(remaining_args, port_str, sizeof(port_str));

        if (host_str[0] == '\0' || port_str[0] == '\0') {
            session_send_system_line(ctx, "Usage: /set-sync-url <host> <port>");
            return;
        }

        char *endptr;
        long port_long = strtol(port_str, &endptr, 10);
        if (*endptr != '\0' || port_long <= 0 || port_long > 65535) {
            session_send_system_line(
                ctx, "Invalid port number. Port must be between 1 and 65535.");
            return;
        }

        // For now, using default username and password. This can be extended later.
        ssh_chatter_sync_set_connection_details(host_str, (int)port_long,
                                                "chatter_sync", "password");
        ssh_chatter_sync_manual_trigger();
        session_send_system_line(
            ctx, "SSH sync URL updated. Attempting to reconnect...");
        return;
    }

    else if (session_parse_command_any(ctx, "/history", effective_line,
                                       &args)) {
        session_handle_history(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/exit", effective_line, &args)) {
        if (ctx->ops != nullptr && ctx->ops->handle_exit != nullptr) {
            ctx->ops->handle_exit(ctx);
        }
        return;
    }

    else if (session_parse_command_any(ctx, "/nick", effective_line, &args)) {
        if (ctx->ops != nullptr && ctx->ops->handle_nick != nullptr) {
            ctx->ops->handle_nick(ctx, args);
        } else {
            session_send_system_line(ctx, "Usage: /nick <name>");
        }
        return;
    }

    else if (session_parse_command_any(ctx, "/pm", effective_line, &args)) {
        session_handle_pm(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/asciiart", effective_line,
                                       &args)) {
        if (*args != '\0') {
            session_send_system_line(ctx, "Usage: /asciiart");
        } else {
            session_asciiart_begin(ctx, SESSION_ASCIIART_TARGET_CHAT);
        }
        return;
    }

    else if (session_parse_command_any(ctx, "/wall", effective_line, &args)) {
        session_handle_wall(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/motd", effective_line, &args)) {
        if (*args != '\0') {
            session_send_system_line(ctx, "Usage: /motd");
        } else {
            session_handle_motd(ctx);
        }
        return;
    }

    else if (session_parse_command_any(ctx, "/status", effective_line, &args)) {
        session_handle_status(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/showstatus", effective_line,
                                       &args)) {
        session_handle_showstatus(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/users", effective_line, &args)) {
        if (*args != '\0') {
            session_send_system_line(ctx, "Usage: /users");
        } else {
            session_handle_usercount(ctx);
        }
        return;
    }

    else if (session_parse_command_any(ctx, "/search", effective_line, &args)) {
        session_handle_search(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/chat", effective_line, &args)) {
        session_handle_chat_lookup(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/reply", effective_line, &args)) {
        session_handle_reply(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/image", effective_line, &args)) {
        session_handle_image(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/video", effective_line, &args)) {
        session_handle_video(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/audio", effective_line, &args)) {
        session_handle_audio(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/files", effective_line, &args)) {
        session_handle_files(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/mail", effective_line, &args)) {
        session_handle_mail(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/game", effective_line, &args)) {
        session_handle_game(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/othello", effective_line,
                                       &args)) {
        session_handle_othello_command(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/banlist", effective_line,
                                       &args)) {
        session_handle_ban_list(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/banname", effective_line,
                                       &args)) {
        session_handle_ban_name(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/ban", effective_line, &args)) {
        session_handle_ban(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/delete-msg", effective_line,
                                         &args)) {
        session_handle_delete_message(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/block", effective_line,
                                         &args)) {
        session_handle_block(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/unblock", effective_line,
                                         &args)) {
        session_handle_unblock(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/pardon", effective_line, &args)) {
        session_handle_pardon(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/poke", effective_line, &args)) {
        session_handle_poke(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/color", effective_line, &args)) {
        session_handle_color(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/systemcolor", effective_line,
                                       &args)) {
        session_handle_system_color(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/set-trans-lang", effective_line,
                                         &args)) {
        session_handle_set_trans_lang(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/set-target-lang",
                                         effective_line, &args)) {
        session_handle_set_target_lang(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/set-ui-lang", effective_line,
                                         &args)) {
        session_handle_set_ui_lang(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/set-lf", effective_line,
                                         &args)) {
        session_handle_set_lf(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/weather", effective_line,
                                         &args)) {
        session_handle_weather(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/translate", effective_line,
                                         &args)) {
        session_handle_translate(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/translate-scope",
                                         effective_line, &args)) {
        session_handle_translate_scope(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/gemini-unfreeze",
                                         effective_line, &args)) {
        session_handle_gemini_unfreeze(ctx);
        return;
    } else if (session_parse_command_any(ctx, "/gemini", effective_line,
                                         &args)) {
        session_handle_gemini(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/captcha", effective_line,
                                         &args)) {
        session_handle_captcha(ctx, args);
        return;
    } else if (session_parse_command(effective_line, "/geo", &args)) {
        session_handle_geo_language(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/eliza", effective_line,
                                         &args)) {
        session_handle_eliza(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/ai-member", effective_line,
                                         &args)) {
        session_handle_ai_member(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/ollama-model", effective_line,
                                         &args)) {
        session_handle_ollama_model(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/breaking", effective_line,
                                         &args)) {
        session_handle_breaking_alerts(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/morse", effective_line,
                                         &args)) {
        session_handle_morse(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/morse-chat", effective_line,
                                         &args)) {
        session_handle_morse_chat(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/morse-reply", effective_line,
                                         &args)) {
        session_handle_morse_chat(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/chat-spacing", effective_line,
                                         &args)) {
        session_handle_chat_spacing(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/retro", effective_line, &args)) {
        session_handle_retro(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/hybrid", effective_line,
                                         &args)) {
        session_handle_hybrid(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/iyagi", effective_line,
                                         &args)) {
        session_handle_iyagi(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/mode", effective_line, &args)) {
        if (ctx->ops != nullptr && ctx->ops->handle_mode != nullptr) {
            ctx->ops->handle_mode(ctx, args);
        } else {
            session_send_system_line(ctx, "Usage: /mode <chat|command|toggle>");
        }
        return;
    } else if (session_parse_command_any(ctx, "/palette", effective_line,
                                         &args)) {
        session_handle_palette(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/suspend!", effective_line,
                                         &args)) {
        if (ctx->game.active) {
            session_game_suspend(ctx, "Game suspended.");
        } else {
            session_game_suspend(ctx, nullptr);
        }
        return;
    } else if (session_parse_command_any(ctx, "/today", effective_line,
                                         &args)) {
        if (*args != '\0') {
            session_send_system_line(ctx, "Usage: /today");
        } else {
            session_handle_today(ctx);
        }
        return;
    } else if (session_parse_command_any(ctx, "/date", effective_line, &args)) {
        session_handle_date(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/os", effective_line, &args)) {
        session_handle_os(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/getos", effective_line,
                                         &args)) {
        session_handle_getos(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/getaddr", effective_line,
                                         &args)) {
        session_handle_getaddr(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/birthday", effective_line,
                                         &args)) {
        session_handle_birthday(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/password", effective_line,
                                         &args) ||
               session_parse_command_any(ctx, "/setpw", effective_line,
                                         &args)) {
        session_handle_setpw(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/delpw", effective_line,
                                         &args)) {
        session_handle_delpw(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/grant", effective_line,
                                         &args)) {
        session_handle_grant(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/shell", effective_line,
                                         &args)) {
        session_handle_shell(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/revoke", effective_line,
                                         &args)) {
        session_handle_revoke(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/pair", effective_line, &args)) {
        if (*args != '\0') {
            session_send_system_line(ctx, "Usage: /pair");
        } else {
            session_handle_pair(ctx);
        }
        return;
    } else if (session_parse_command_any(ctx, "/connected", effective_line,
                                         &args)) {
        if (*args != '\0') {
            session_send_system_line(ctx, "Usage: /connected");
        } else {
            session_handle_connected(ctx);
        }
        return;
    } else if (session_parse_command_any(ctx, "/alpha-centauri-landers",
                                         effective_line, &args)) {
        if (*args != '\0') {
            session_send_system_line(ctx, "Usage: /alpha-centauri-landers");
        } else {
            session_handle_alpha_centauri_landers(ctx);
        }
        return;
    } else if (session_parse_command_any(ctx, "/poll", effective_line, &args)) {
        session_handle_poll(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/vote-single", effective_line,
                                         &args)) {
        if (*args == '\0') {
            session_handle_vote_command(ctx, nullptr, false);
        } else {
            session_handle_vote_command(ctx, args, false);
        }
        return;
    } else if (session_parse_command_any(ctx, "/vote", effective_line, &args)) {
        if (*args == '\0') {
            session_handle_vote_command(ctx, nullptr, true);
        } else {
            session_handle_vote_command(ctx, args, true);
        }
        return;
    } else if (session_parse_command_any(ctx, "/elect", effective_line,
                                         &args)) {
        if (*args == '\0') {
            session_handle_elect_command(ctx, nullptr);
        } else {
            session_handle_elect_command(ctx, args);
        }
        return;
    } else if (session_parse_command(effective_line, "/rss", &args)) {
        session_handle_rss(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/bbs", effective_line, &args)) {
        session_handle_bbs(ctx, (args != nullptr && args[0] != '\0') ? args
                                                                     : nullptr);
        return;
    }

    else if (session_parse_command_any(ctx, "/resetpw", effective_line,
                                       &args)) {
        session_handle_resetpw(ctx, args);
        return;
    }

    else if (effective_line[0] == '/') {
        if (isdigit((unsigned char)effective_line[1])) {
            char *endptr = nullptr;
            unsigned long vote_index = strtoul(effective_line + 1, &endptr, 10);
            const unsigned long max_vote = sizeof(ctx->owner->poll.options) /
                                           sizeof(ctx->owner->poll.options[0]);
            if (vote_index >= 1UL && vote_index <= max_vote) {
                while (endptr != nullptr &&
                       (*endptr == ' ' || *endptr == '\t')) {
                    ++endptr;
                }
                if (endptr == nullptr || *endptr == '\0') {
                    session_handle_vote(ctx, (size_t)(vote_index - 1UL));
                    return;
                } else {
                    while (*endptr == ' ' || *endptr == '\t') {
                        ++endptr;
                    }
                    if (*endptr != '\0') {
                        char label[SSH_CHATTER_POLL_LABEL_LEN];
                        size_t label_len = 0U;
                        while (*endptr != '\0' &&
                               !isspace((unsigned char)*endptr)) {
                            if (label_len + 1U >= sizeof(label)) {
                                label_len = 0U;
                                break;
                            }
                            label[label_len++] = *endptr++;
                        }
                        label[label_len] = '\0';
                        if (label_len > 0U) {
                            session_handle_named_vote(
                                ctx, (size_t)(vote_index - 1UL), label);
                            return;
                        }
                    }
                }
            }
        }
        for (size_t idx = 0U; idx < SSH_CHATTER_REACTION_KIND_COUNT; ++idx) {
            const reaction_descriptor_t *descriptor =
                &REACTION_DEFINITIONS[idx];
            char canonical[32];
            int written = snprintf(canonical, sizeof(canonical), "/%s",
                                   descriptor->command);
            if (written < 0 || (size_t)written >= sizeof(canonical)) {
                continue;
            }

            const char *arguments = nullptr;
            if (!session_parse_command_any(ctx, canonical, effective_line,
                                           &arguments)) {
                continue;
            }

            session_handle_reaction(ctx, idx, arguments);
            return;
        }
    }

    const session_ui_locale_t *locale = session_ui_get_locale(ctx);
    const char *format = (locale->unknown_command != nullptr &&
                          locale->unknown_command[0] != '\0')
                             ? locale->unknown_command
                             : "Unknown command. Type %shelp for help.";
    const char *prefix = session_command_prefix(ctx);
    const char *prefix_args[] = {prefix};
    char message[SSH_CHATTER_MESSAGE_LIMIT];
    session_format_template(format, prefix_args,
                            sizeof(prefix_args) / sizeof(prefix_args[0]),
                            message, sizeof(message));
    session_send_system_line(ctx, message);
}

void trim_whitespace_inplace(char *text)
{
    if (text == nullptr) {
        return;
    }

    char *start = text;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        ++start;
    }

    char *end = text + strlen(text);
    while (end > start && isspace((unsigned char)*(end - 1))) {
        --end;
    }

    const size_t length = (size_t)(end - start);
    if (start != text && length > 0U) {
        memmove(text, start, length);
    }
    text[length] = '\0';
}

static const char *session_consume_token(const char *input, char *token,
                                         size_t length)
{
    if (token == nullptr || length == 0U) {
        return input;
    }

    token[0] = '\0';
    if (input == nullptr) {
        return nullptr;
    }

    while (*input == ' ' || *input == '\t') {
        ++input;
    }

    size_t out_idx = 0U;
    while (*input != '\0' && !isspace((unsigned char)*input)) {
        if (out_idx + 1U < length) {
            token[out_idx++] = *input;
        }
        ++input;
    }
    token[out_idx] = '\0';

    while (*input == ' ' || *input == '\t') {
        ++input;
    }

    return input;
}

static bool session_user_data_available(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return false;
    }

    if (!ctx->owner->user_data_ready) {
        return false;
    }

    if (ctx->user.name[0] == '\0') {
        return false;
    }

    return true;
}

bool host_user_data_load_existing(host_t *host, const char *username,
                                  const char *ip, user_data_record_t *record,
                                  bool create_if_missing)
{
    if (host == nullptr || username == nullptr || username[0] == '\0') {
        return false;
    }

    if (!host->user_data_ready) {
        return false;
    }

    bool success = false;
    if (host->user_data_lock_initialized) {
        ttak_mutex_lock(&host->user_data_lock);
    }

    if (create_if_missing) {
        success =
            user_data_ensure_exists(host->user_data_root, username, ip, record);
    } else {
        success = user_data_load(host->user_data_root, username, ip, record);
    }

    if (host->user_data_lock_initialized) {
        ttak_mutex_unlock(&host->user_data_lock);
    }

    return success;
}

static bool host_lookup_last_ip(host_t *host, const char *username, char *ip,
                                size_t length)
{
    if (ip != nullptr && length > 0U) {
        ip[0] = '\0';
    }

    if (host == nullptr || username == nullptr || username[0] == '\0' ||
        ip == nullptr || length == 0U) {
        return false;
    }

    if (host_lookup_member_ip(host, username, ip, length)) {
        return true;
    }

    user_data_record_t record;
    if (!host_user_data_load_existing(host, username, nullptr, &record,
                                      false)) {
        return false;
    }

    if (record.last_ip[0] == '\0') {
        return false;
    }

    snprintf(ip, length, "%s", record.last_ip);
    return true;
}

static bool host_user_data_send_mail(host_t *host, const char *recipient,
                                     const char *recipient_ip,
                                     const char *sender, const char *message,
                                     char *error, size_t error_length)
{
    if (error != nullptr && error_length > 0U) {
        error[0] = '\0';
    }

    if (host == nullptr || recipient == nullptr || recipient[0] == '\0' ||
        message == nullptr || message[0] == '\0') {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length, "%s", "Invalid mailbox parameters.");
        }
        return false;
    }

    if (!host->user_data_ready) {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length, "%s", "Mailbox storage unavailable.");
        }
        return false;
    }

    char resolved_ip[SSH_CHATTER_IP_LEN];
    resolved_ip[0] = '\0';
    if (recipient_ip != nullptr && recipient_ip[0] != '\0') {
        snprintf(resolved_ip, sizeof(resolved_ip), "%s", recipient_ip);
    }

    const bool target_is_lan_ops =
        host_is_lan_operator_username(host, recipient);
    if (target_is_lan_ops) {
        session_ctx_t *target_session =
            chat_room_find_user(&host->room, recipient);
        if (target_session == nullptr ||
            !target_session->user.is_lan_operator) {
            if (error != nullptr && error_length > 0U) {
                snprintf(error, error_length, "%s",
                         "LAN operator mailbox is unavailable.");
            }
            return false;
        }
        snprintf(resolved_ip, sizeof(resolved_ip), "%s",
                 target_session->client_ip);
    }

    if (resolved_ip[0] == '\0') {
        session_ctx_t *target_session =
            chat_room_find_user(&host->room, recipient);
        if (target_session != nullptr) {
            snprintf(resolved_ip, sizeof(resolved_ip), "%s",
                     target_session->client_ip);
        }
    }

    if (resolved_ip[0] == '\0') {
        if (error != nullptr && error_length > 0U) {
            snprintf(
                error, error_length, "%s",
                "Provide the recipient's IP (name@ip) when they are offline.");
        }
        return false;
    }

    user_data_record_t record;
    if (!host_user_data_load_existing(host, recipient, resolved_ip, &record,
                                      true)) {
        if (error != nullptr && error_length > 0U) {
            const int mailbox_name_precision =
                (int)(SSH_CHATTER_USERNAME_LEN / 4U);
            snprintf(error, error_length, "Unable to open mailbox for %.*s.",
                     mailbox_name_precision, recipient);
        }
        return false;
    }

    if (record.mailbox_count >= USER_DATA_MAILBOX_LIMIT) {
        for (size_t idx = 1U; idx < USER_DATA_MAILBOX_LIMIT; ++idx) {
            record.mailbox[idx - 1U] = record.mailbox[idx];
        }
        record.mailbox_count = USER_DATA_MAILBOX_LIMIT - 1U;
    }

    user_data_mail_entry_t *entry = &record.mailbox[record.mailbox_count++];
    time_t now = time(nullptr);
    if (now == (time_t)-1) {
        now = 0;
    }
    entry->timestamp = (uint64_t)now;
    if (sender != nullptr && sender[0] != '\0') {
        snprintf(entry->sender, sizeof(entry->sender), "%s", sender);
    } else {
        snprintf(entry->sender, sizeof(entry->sender), "%s", "system");
    }
    snprintf(entry->message, sizeof(entry->message), "%s", message);
    record.last_updated = (uint64_t)now;
    snprintf(record.last_ip, sizeof(record.last_ip), "%s", resolved_ip);

    bool success;
    if (host->user_data_lock_initialized) {
        ttak_mutex_lock(&host->user_data_lock);
    }
    success = user_data_save(host->user_data_root, &record, resolved_ip);
    if (host->user_data_lock_initialized) {
        ttak_mutex_unlock(&host->user_data_lock);
    }

    if (!success) {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length, "%s",
                     "Failed to write mailbox file.");
        }
        humanized_log_error("mailbox", "failed to persist mailbox entry",
                            errno != 0 ? errno : EIO);
        return false;
    }

    return true;
}

static void rss_trim_whitespace(char *text)
{
    trim_whitespace_inplace(text);
}

static void rss_strip_html(char *text)
{
    if (text == nullptr) {
        return;
    }

    size_t read = 0U;
    size_t write = 0U;
    bool in_tag = false;
    while (text[read] != '\0') {
        char ch = text[read++];
        if (ch == '<') {
            in_tag = true;
            continue;
        }
        if (in_tag) {
            if (ch == '>') {
                in_tag = false;
            }
            continue;
        }
        text[write++] = ch;
    }
    text[write] = '\0';
}

static void rss_decode_entities(char *text)
{
    if (text == nullptr) {
        return;
    }

    char *src = text;
    char *dst = text;
    while (*src != '\0') {
        if (*src == '&') {
            if (strncmp(src, "&amp;", 5) == 0) {
                *dst++ = '&';
                src += 5;
                continue;
            }
            if (strncmp(src, "&lt;", 4) == 0) {
                *dst++ = '<';
                src += 4;
                continue;
            }
            if (strncmp(src, "&gt;", 4) == 0) {
                *dst++ = '>';
                src += 4;
                continue;
            }
            if (strncmp(src, "&quot;", 6) == 0) {
                *dst++ = '\"';
                src += 6;
                continue;
            }
            if (strncmp(src, "&#39;", 5) == 0) {
                *dst++ = '\'';
                src += 5;
                continue;
            }
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

// Reset a poll structure to a neutral inactive state.
static void poll_state_reset(poll_state_t *poll)
{
    if (poll == nullptr) {
        return;
    }

    poll->active = false;
    poll->option_count = 0U;
    poll->question[0] = '\0';
    poll->allow_multiple = false;
    for (size_t idx = 0U;
         idx < sizeof(poll->options) / sizeof(poll->options[0]); ++idx) {
        poll->options[idx].text[0] = '\0';
        poll->options[idx].votes = 0U;
    }
}

// Reset a named poll entry including its label and voter tracking list.
static void named_poll_reset(named_poll_state_t *poll)
{
    if (poll == nullptr) {
        return;
    }

    poll_state_reset(&poll->poll);
    poll->label[0] = '\0';
    poll->owner[0] = '\0';
    poll->voter_count = 0U;
    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_NAMED_VOTERS; ++idx) {
        poll->voters[idx].username[0] = '\0';
        poll->voters[idx].choice = -1;
        poll->voters[idx].choices_mask = 0U;
    }
}

// Look up a named poll by its label while the host lock is already held.
static named_poll_state_t *host_find_named_poll_locked(host_t *host,
                                                       const char *label)
{
    if (host == nullptr || label == nullptr || label[0] == '\0') {
        return nullptr;
    }

    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_NAMED_POLLS; ++idx) {
        named_poll_state_t *entry = &host->named_polls[idx];
        if (entry->label[0] == '\0') {
            continue;
        }
        if (strcasecmp(entry->label, label) == 0) {
            return entry;
        }
    }

    return nullptr;
}

// Either fetch an existing named poll or initialise a new slot for the provided label.
static __attribute__((unused)) named_poll_state_t *
host_ensure_named_poll_locked(host_t *host, const char *label)
{
    if (host == nullptr || label == nullptr || label[0] == '\0') {
        return nullptr;
    }

    named_poll_state_t *existing = host_find_named_poll_locked(host, label);
    if (existing != nullptr) {
        return existing;
    }

    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_NAMED_POLLS; ++idx) {
        named_poll_state_t *entry = &host->named_polls[idx];
        if (entry->label[0] != '\0') {
            continue;
        }
        named_poll_reset(entry);
        snprintf(entry->label, sizeof(entry->label), "%s", label);
        return entry;
    }

    return nullptr;
}

// Recompute how many named polls are active so list summaries remain accurate.
static void host_recount_named_polls_locked(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    size_t count = 0U;
    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_NAMED_POLLS; ++idx) {
        if (host->named_polls[idx].label[0] != '\0' &&
            host->named_polls[idx].poll.active) {
            ++count;
        }
    }
    host->named_poll_count = count;
}

// Ensure poll labels remain short and shell-friendly.
static __attribute__((unused)) bool poll_label_is_valid(const char *label)
{
    if (label == nullptr || label[0] == '\0') {
        return false;
    }

    for (size_t idx = 0U; label[idx] != '\0'; ++idx) {
        char ch = label[idx];
        if (!(isalnum((unsigned char)ch) || ch == '_' || ch == '-')) {
            return false;
        }
    }
    return true;
}

static void session_normalize_newlines(char *text)
{
    if (text == nullptr) {
        return;
    }

    size_t read_idx = 0U;
    size_t write_idx = 0U;
    while (text[read_idx] != '\0') {
        char ch = text[read_idx++];
        if (ch == '\r') {
            if (text[read_idx] == '\n') {
                ++read_idx;
            }
            text[write_idx++] = '\n';
        } else {
            text[write_idx++] = ch;
        }
    }

    text[write_idx] = '\0';
}

static bool timezone_sanitize_identifier(const char *input, char *output,
                                         size_t length)
{
    if (input == nullptr || output == nullptr || length == 0U) {
        return false;
    }

    size_t out_idx = 0U;
    bool last_was_slash = true;

    for (size_t idx = 0U; input[idx] != '\0'; ++idx) {
        unsigned char ch = (unsigned char)input[idx];
        if (isspace(ch)) {
            return false;
        }

        if (ch == '/') {
            if (last_was_slash) {
                return false;
            }
            if (out_idx + 1U >= length) {
                return false;
            }
            output[out_idx++] = '/';
            last_was_slash = true;
            continue;
        }

        if (!(isalnum(ch) || ch == '_' || ch == '-' || ch == '+' ||
              ch == '.')) {
            return false;
        }

        if (out_idx + 1U >= length) {
            return false;
        }
        output[out_idx++] = (char)ch;
        last_was_slash = false;
    }

    if (out_idx == 0U || last_was_slash) {
        return false;
    }

    output[out_idx] = '\0';

    if (output[0] == '/' || strstr(output, "..") != nullptr) {
        return false;
    }

    return true;
}

static bool timezone_resolve_identifier(const char *input, char *resolved,
                                        size_t length)
{
    if (input == nullptr || input[0] == '\0' || resolved == nullptr ||
        length == 0U) {
        return false;
    }

    static const char kTimezoneDir[] = "/usr/share/zoneinfo";

    char full_path[PATH_MAX];
    int full_written =
        snprintf(full_path, sizeof(full_path), "%s/%s", kTimezoneDir, input);
    if (full_written >= 0 && (size_t)full_written < sizeof(full_path) &&
        access(full_path, R_OK) == 0) {
        int copy_written = snprintf(resolved, length, "%s", input);
        return copy_written >= 0 && (size_t)copy_written < length;
    }

    char working[PATH_MAX];
    int working_written = snprintf(working, sizeof(working), "%s", input);
    if (working_written < 0 || (size_t)working_written >= sizeof(working)) {
        return false;
    }

    char accumulated[PATH_MAX];
    accumulated[0] = '\0';
    size_t accumulated_len = 0U;
    char current_dir[PATH_MAX];
    int dir_written =
        snprintf(current_dir, sizeof(current_dir), "%s", kTimezoneDir);
    if (dir_written < 0 || (size_t)dir_written >= sizeof(current_dir)) {
        return false;
    }

    char *saveptr = nullptr;
    char *segment = strtok_r(working, "/", &saveptr);
    if (segment == nullptr) {
        return false;
    }

    while (segment != nullptr) {
        DIR *dir = opendir(current_dir);
        if (dir == nullptr) {
            return false;
        }

        bool found = false;
        char matched[NAME_MAX + 1];
        matched[0] = '\0';
        struct dirent *entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') {
                if (entry->d_name[1] == '\0') {
                    continue;
                }
                if (entry->d_name[1] == '.' && entry->d_name[2] == '\0') {
                    continue;
                }
            }

            if (strcasecmp(entry->d_name, segment) == 0) {
                found = true;
                snprintf(matched, sizeof(matched), "%s", entry->d_name);
                break;
            }
        }
        closedir(dir);

        if (!found) {
            return false;
        }

        if (accumulated_len > 0U) {
            if (accumulated_len + 1U >= sizeof(accumulated)) {
                return false;
            }
            accumulated[accumulated_len++] = '/';
        }

        size_t match_len = strlen(matched);
        if (accumulated_len + match_len >= sizeof(accumulated)) {
            return false;
        }
        memcpy(accumulated + accumulated_len, matched, match_len);
        accumulated_len += match_len;
        accumulated[accumulated_len] = '\0';

        dir_written = snprintf(current_dir, sizeof(current_dir), "%s/%s",
                               kTimezoneDir, accumulated);
        if (dir_written < 0 || (size_t)dir_written >= sizeof(current_dir)) {
            return false;
        }

        segment = strtok_r(nullptr, "/", &saveptr);
    }

    if (accumulated_len == 0U) {
        return false;
    }

    full_written = snprintf(full_path, sizeof(full_path), "%s/%s", kTimezoneDir,
                            accumulated);
    if (full_written < 0 || (size_t)full_written >= sizeof(full_path)) {
        return false;
    }

    if (access(full_path, R_OK) != 0) {
        return false;
    }

    int copy_written = snprintf(resolved, length, "%s", accumulated);
    return copy_written >= 0 && (size_t)copy_written < length;
}

static const os_descriptor_t *session_lookup_os_descriptor(const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        return nullptr;
    }

    for (size_t idx = 0U; idx < sizeof(OS_CATALOG) / sizeof(OS_CATALOG[0]);
         ++idx) {
        if (strcasecmp(OS_CATALOG[idx].name, name) == 0) {
            return &OS_CATALOG[idx];
        }
    }

    return nullptr;
}

static const char *lookup_color_code(const color_entry_t *entries,
                                     size_t entry_count, const char *name)
{
    if (entries == nullptr || name == nullptr) {
        return nullptr;
    }

    for (size_t idx = 0; idx < entry_count; ++idx) {
        if (strcasecmp(entries[idx].name, name) == 0) {
            return entries[idx].code;
        }
    }

#if defined(__STDC_NO_THREADS__)
    static char fg_cache[8][16];
    static size_t fg_cache_index = 0U;
    static char bg_cache[8][16];
    static size_t bg_cache_index = 0U;
#else
    static _Thread_local char fg_cache[8][16];
    static _Thread_local size_t fg_cache_index = 0U;
    static _Thread_local char bg_cache[8][16];
    static _Thread_local size_t bg_cache_index = 0U;
#endif

    if (strncasecmp(name, "xterm:", 6) == 0) {
        const char *digits = name + 6;
        char *endptr = nullptr;
        unsigned long code = strtoul(digits, &endptr, 10);
        if (endptr != nullptr && *endptr == '\0' && code <= 255U) {
            char(*slot)[16] = &fg_cache[fg_cache_index];
            fg_cache_index = (fg_cache_index + 1U) %
                             (sizeof(fg_cache) / sizeof(fg_cache[0]));
            ansi_256(*slot, sizeof(fg_cache[0]), (unsigned int)code);
            return *slot;
        }
    }

    if (strncasecmp(name, "xterm-bg:", 9) == 0) {
        const char *digits = name + 9;
        char *endptr = nullptr;
        unsigned long code = strtoul(digits, &endptr, 10);
        if (endptr != nullptr && *endptr == '\0' && code <= 255U) {
            char(*slot)[16] = &bg_cache[bg_cache_index];
            bg_cache_index = (bg_cache_index + 1U) %
                             (sizeof(bg_cache) / sizeof(bg_cache[0]));
            ansi_bg_256(*slot, sizeof(bg_cache[0]), (unsigned int)code);
            return *slot;
        }
    }

    return nullptr;
}

static const palette_descriptor_t *palette_find_descriptor(const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        return nullptr;
    }

    for (size_t idx = 0U;
         idx < sizeof(PALETTE_DEFINITIONS) / sizeof(PALETTE_DEFINITIONS[0]);
         ++idx) {
        if (strcasecmp(PALETTE_DEFINITIONS[idx].id, name) == 0) {
            return &PALETTE_DEFINITIONS[idx];
        }
    }

    return nullptr;
}

static bool palette_apply_to_session(session_ctx_t *ctx,
                                     const palette_descriptor_t *descriptor)
{
    if (ctx == nullptr || descriptor == nullptr) {
        return false;
    }

    const char *user_color_code = lookup_color_code(
        USER_COLOR_MAP, sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
        descriptor->user_color_name);
    const char *user_highlight_code = lookup_color_code(
        HIGHLIGHT_COLOR_MAP,
        sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
        descriptor->user_highlight_name);
    const char *system_fg_code = lookup_color_code(
        USER_COLOR_MAP, sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
        descriptor->system_fg_name);
    const char *system_bg_code = lookup_color_code(
        HIGHLIGHT_COLOR_MAP,
        sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
        descriptor->system_bg_name);
    const char *system_highlight_code = lookup_color_code(
        HIGHLIGHT_COLOR_MAP,
        sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
        descriptor->system_highlight_name);

    if (user_color_code == nullptr || user_highlight_code == nullptr ||
        system_fg_code == nullptr || system_bg_code == nullptr ||
        system_highlight_code == nullptr) {
        return false;
    }

    snprintf(ctx->user_color_code, sizeof(ctx->user_color_code), "%s",
             user_color_code);
    snprintf(ctx->user_highlight_code, sizeof(ctx->user_highlight_code), "%s",
             user_highlight_code);
    ctx->user_is_bold = descriptor->user_is_bold;
    snprintf(ctx->user_color_name, sizeof(ctx->user_color_name), "%s",
             descriptor->user_color_name);
    snprintf(ctx->user_highlight_name, sizeof(ctx->user_highlight_name), "%s",
             descriptor->user_highlight_name);

    ctx->system_fg_code = system_fg_code;
    ctx->system_bg_code = system_bg_code;
    ctx->system_highlight_code = system_highlight_code;
    ctx->system_is_bold = descriptor->system_is_bold;
    snprintf(ctx->system_fg_name, sizeof(ctx->system_fg_name), "%s",
             descriptor->system_fg_name);
    snprintf(ctx->system_bg_name, sizeof(ctx->system_bg_name), "%s",
             descriptor->system_bg_name);
    snprintf(ctx->system_highlight_name, sizeof(ctx->system_highlight_name),
             "%s", descriptor->system_highlight_name);

    session_force_dark_mode_foreground(ctx);

    return true;
}

static void
host_apply_palette_descriptor(host_t *host,
                              const palette_descriptor_t *descriptor)
{
    if (host == nullptr || descriptor == nullptr) {
        return;
    }

    const char *user_color_code = lookup_color_code(
        USER_COLOR_MAP, sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
        descriptor->user_color_name);
    const char *user_highlight_code = lookup_color_code(
        HIGHLIGHT_COLOR_MAP,
        sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
        descriptor->user_highlight_name);
    const char *system_fg_code = lookup_color_code(
        USER_COLOR_MAP, sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
        descriptor->system_fg_name);
    const char *system_bg_code = lookup_color_code(
        HIGHLIGHT_COLOR_MAP,
        sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
        descriptor->system_bg_name);
    const char *system_highlight_code = lookup_color_code(
        HIGHLIGHT_COLOR_MAP,
        sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
        descriptor->system_highlight_name);

    if (user_color_code == nullptr) {
        user_color_code = ANSI_GREEN;
    }
    if (user_highlight_code == nullptr) {
        user_highlight_code = ANSI_BG_DEFAULT;
    }
    if (system_fg_code == nullptr) {
        system_fg_code = ANSI_WHITE;
    }
    if (system_bg_code == nullptr) {
        system_bg_code = ANSI_BG_BLUE;
    }
    if (system_highlight_code == nullptr) {
        system_highlight_code = ANSI_BG_YELLOW;
    }

    host->user_theme.userColor = user_color_code;
    host->user_theme.highlight = user_highlight_code;
    host->user_theme.isBold = descriptor->user_is_bold;
    host->system_theme.foregroundColor = system_fg_code;
    host->system_theme.backgroundColor = system_bg_code;
    host->system_theme.highlightColor = system_highlight_code;
    host->system_theme.isBold = descriptor->system_is_bold;

    snprintf(host->default_user_color_name,
             sizeof(host->default_user_color_name), "%s",
             descriptor->user_color_name);
    snprintf(host->default_user_highlight_name,
             sizeof(host->default_user_highlight_name), "%s",
             descriptor->user_highlight_name);
    snprintf(host->default_system_fg_name, sizeof(host->default_system_fg_name),
             "%s", descriptor->system_fg_name);
    snprintf(host->default_system_bg_name, sizeof(host->default_system_bg_name),
             "%s", descriptor->system_bg_name);
    snprintf(host->default_system_highlight_name,
             sizeof(host->default_system_highlight_name), "%s",
             descriptor->system_highlight_name);
}

static bool parse_bool_token(const char *token, bool *value)
{
    if (token == nullptr || value == nullptr) {
        return false;
    }

    if (strcasecmp(token, "true") == 0 || strcasecmp(token, "yes") == 0 ||
        strcasecmp(token, "on") == 0 || strcasecmp(token, "bold") == 0 ||
        strcmp(token, "켜기") == 0 || strcmp(token, "オン") == 0 ||
        strcmp(token, "开") == 0 || strcmp(token, "вкл") == 0) {
        *value = true;
        return true;
    }

    if (strcasecmp(token, "false") == 0 || strcasecmp(token, "no") == 0 ||
        strcasecmp(token, "off") == 0 || strcasecmp(token, "normal") == 0 ||
        strcmp(token, "끄기") == 0 || strcmp(token, "オフ") == 0 ||
        strcmp(token, "关") == 0 || strcmp(token, "выкл") == 0) {
        *value = false;
        return true;
    }

    return false;
}

static bool session_transport_active(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        return ctx->telnet_fd >= 0 && !ctx->telnet_eof;
    }

    return ctx->channel != nullptr;
}

static bool session_transport_is_open(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        return ctx->telnet_fd >= 0 && !ctx->telnet_eof;
    }

    return ctx->channel != nullptr && ssh_channel_is_open(ctx->channel);
}

static bool session_transport_is_eof(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return true;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        return ctx->telnet_eof || ctx->telnet_fd < 0;
    }

    return ctx->channel == nullptr || ssh_channel_is_eof(ctx->channel);
}

static void session_transport_request_close(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        if (ctx->telnet_fd >= 0) {
            shutdown(ctx->telnet_fd, SHUT_RDWR);
        }
        ctx->telnet_eof = true;
        return;
    }

    if (ctx->channel != nullptr) {
        ssh_channel_send_eof(ctx->channel);
        ssh_channel_close(ctx->channel);
    }
}

static void session_close_channel(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        if (ctx->telnet_fd >= 0) {
            shutdown(ctx->telnet_fd, SHUT_RDWR);
            close(ctx->telnet_fd);
            ctx->telnet_fd = -1;
        }
        ctx->telnet_eof = true;
        return;
    }

    if (ctx->channel == nullptr) {
        return;
    }

    /* Nullify the channel pointer first so that concurrent readers
     * (e.g. in-flight broadcast threads) see NULL via
     * session_transport_active() and bail out before we free the
     * underlying libssh object. */
    ssh_channel old_channel = ctx->channel;
    ctx->channel = nullptr;

    ssh_channel_send_eof(old_channel);
    ssh_channel_close(old_channel);
    ssh_channel_free(old_channel);
}

static void session_reset_for_retry(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    session_close_channel(ctx);
    ctx->should_exit = false;
    ctx->exit_notice_sent = false;
    ctx->username_conflict = false;
    ctx->has_joined_room = false;
    ctx->prelogin_banner_rendered = false;
    ctx->input_length = 0U;
    ctx->input_buffer[0] = '\0';
    ctx->input_escape_active = false;
    ctx->input_escape_length = 0U;
    ctx->input_escape_buffer[0] = '\0';
    ctx->multibyte_input_length = 0U;
    memset(ctx->multibyte_input_buffer, 0, sizeof(ctx->multibyte_input_buffer));
    ctx->wall_active = false;
    ctx->wall_command_mode = false;
    ctx->wall_cursor_x = 0U;
    ctx->wall_cursor_y = 0U;
    ctx->wall_brush_char = '#';
    snprintf(ctx->wall_brush_color_name, sizeof(ctx->wall_brush_color_name),
             "%s", "white");
    ctx->bbs_post_pending = false;
    ctx->pending_bbs_body_length = 0U;
    ctx->pending_bbs_tag_count = 0U;
    if (ctx->pending_bbs_title != nullptr) {
        ctx->pending_bbs_title[0] = '\0';
    }
    if (ctx->pending_bbs_body != nullptr) {
        ctx->pending_bbs_body[0] = '\0';
    }
    if (ctx->pending_bbs_tags != nullptr) {
        memset(ctx->pending_bbs_tags, 0,
               sizeof(*ctx->pending_bbs_tags) * SSH_CHATTER_BBS_MAX_TAGS);
    }
    ctx->bbs_view_active = false;
    ctx->bbs_view_post_id = 0U;
    ctx->bbs_view_scroll_offset = 0U;
    ctx->bbs_view_total_lines = 0U;
    ctx->bbs_view_notice_pending = false;
    if (ctx->bbs_view_notice != nullptr) {
        ctx->bbs_view_notice[0] = '\0';
    }
    ctx->bbs_rendering_editor = false;
    ctx->telnet_terminal_type_requested = false;
    ctx->terminal_type[0] = '\0';
    ctx->prefer_cp437_output = false;
    ctx->cp437_output_scope = SESSION_CP437_SCOPE_ALL;
    ctx->output_kind = SESSION_OUTPUT_KIND_SYSTEM;
    ctx->cp437_override = SESSION_CP437_OVERRIDE_NONE;
    ctx->cp437_input_enabled = false;
    session_asciiart_reset(ctx);
    ctx->asciiart_has_cooldown = false;
    ctx->last_asciiart_post.tv_sec = 0;
    ctx->last_asciiart_post.tv_nsec = 0;
    session_game_tetris_reset(ctx->game.tetris);
    ctx->game.liar.awaiting_guess = false;
    ctx->game.liar.round_number = 0U;
    ctx->game.liar.score = 0U;
    ctx->game.othello = (othello_game_state_t){0};
    ctx->game.saved_othello_state = (othello_game_state_t){0};
    ctx->game.active = false;
    ctx->game.type = SESSION_GAME_NONE;
    ctx->game.rng_seeded = false;
    ctx->game.rng_state = 0U;
    ctx->game.alpha = (alpha_centauri_game_state_t){0};
    ctx->input_history_count = 0U;
    memset(ctx->input_history_is_command, 0,
           sizeof(ctx->input_history_is_command));
    ctx->input_history_position = -1;
    session_scrollback_reset_position(ctx);
    ctx->has_last_message_time = false;
    ctx->last_message_time.tv_sec = 0;
    ctx->last_message_time.tv_nsec = 0;
    ctx->user_data_loaded = false;
    memset(&ctx->user_data, 0, sizeof(ctx->user_data));
    session_refresh_output_encoding(ctx);
}

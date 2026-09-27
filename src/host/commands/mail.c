static void session_mail_render_inbox(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    if (!session_user_data_load(ctx)) {
        session_send_system_line(
            ctx,
            session_command_localize(ctx, "Mailbox storage is unavailable."));
        return;
    }

    if (ctx->user_data.mailbox_count == 0U) {
        session_send_system_line(ctx, "Your mailbox is empty.");
        return;
    }

    char header[128];
    snprintf(header, sizeof(header), "Mailbox (%u message%s):",
             (unsigned int)ctx->user_data.mailbox_count,
             ctx->user_data.mailbox_count == 1U ? "" : "s");
    session_send_system_line(ctx, header);

    for (size_t idx = 0U; idx < ctx->user_data.mailbox_count; ++idx) {
        const user_data_mail_entry_t *entry = &ctx->user_data.mailbox[idx];
        time_t stamp = (time_t)entry->timestamp;
        struct tm when;
        char stamp_text[32];
        if (stamp != 0 && localtime_r(&stamp, &when) != nullptr) {
            if (strftime(stamp_text, sizeof(stamp_text), "%Y-%m-%d %H:%M",
                         &when) == 0U) {
                snprintf(stamp_text, sizeof(stamp_text), "%s",
                         "(time unknown)");
            }
        } else {
            snprintf(stamp_text, sizeof(stamp_text), "%s", "(time unknown)");
        }

        char body[USER_DATA_MAILBOX_MESSAGE_LEN];
        snprintf(body, sizeof(body), "%s", entry->message);
        for (size_t pos = 0U; body[pos] != '\0'; ++pos) {
            unsigned char ch = (unsigned char)body[pos];
            if (ch < ' ' && ch != '\n' && ch != '\t') {
                body[pos] = ' ';
            }
            if (body[pos] == '\n') {
                body[pos] = ' ';
            }
        }

        char line[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(line, sizeof(line), "[%s] %s: %s", stamp_text,
                 entry->sender[0] != '\0' ? entry->sender : "(unknown)", body);
        session_send_system_line(ctx, line);
    }
}

// Sends the localized /mail usage; "/mail" is swapped for the alias.
static void session_mail_send_usage(session_ctx_t *ctx, bool send_form)
{
    const char *tmpl = nullptr;
    switch (session_ui_language_current(ctx)) {
    case SESSION_UI_LANGUAGE_KO:
        tmpl = send_form ? "사용법: /mail [send] <사용자[@IP]>|<메시지>"
                         : "사용법: /mail [inbox|send <사용자>|<메시지>|clear]";
        break;
    case SESSION_UI_LANGUAGE_JP:
        tmpl = send_form
                   ? "使い方: /mail [send] <ユーザー[@IP]>|<メッセージ>"
                   : "使い方: /mail [inbox|send <ユーザー>|<メッセージ>|clear]";
        break;
    case SESSION_UI_LANGUAGE_ZH:
        tmpl = send_form ? "用法: /mail [send] <用户[@IP]>|<消息>"
                         : "用法: /mail [inbox|send <用户>|<消息>|clear]";
        break;
    case SESSION_UI_LANGUAGE_RU:
        tmpl = send_form ? "Использование: /mail [send] "
                           "<пользователь[@IP]>|<сообщение>"
                         : "Использование: /mail [inbox|send "
                           "<пользователь>|<сообщение>|clear]";
        break;
    case SESSION_UI_LANGUAGE_DE:
        tmpl = send_form
                   ? "Nutzung: /mail [send] <Benutzer[@IP]>|<Nachricht>"
                   : "Nutzung: /mail [inbox|send <Benutzer>|<Nachricht>|clear]";
        break;
    case SESSION_UI_LANGUAGE_FR:
        tmpl = send_form ? "Utilisation: /mail [send] "
                           "<utilisateur[@IP]>|<message>"
                         : "Utilisation: /mail [inbox|send "
                           "<utilisateur>|<message>|clear]";
        break;
    case SESSION_UI_LANGUAGE_PL:
        tmpl = send_form ? "Użycie: /mail [send] <użytkownik[@IP]>|<wiadomość>"
                         : "Użycie: /mail [inbox|send "
                           "<użytkownik>|<wiadomość>|clear]";
        break;
    default:
        tmpl = send_form ? "Usage: /mail [send] <user[@ip]>|<message>"
                         : "Usage: /mail [inbox|send <user>|<message>|clear]";
        break;
    }

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/mail", tmpl, usage, sizeof(usage));
    session_send_system_line(ctx, usage);
}

static void session_handle_mail(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!session_user_data_available(ctx) && !ctx->owner->user_data_ready) {
        session_send_system_line(
            ctx,
            session_command_localize(ctx, "Mailbox storage is unavailable."));
        return;
    }

    const char *cursor = arguments != nullptr ? arguments : "";
    char command[16];
    cursor = session_consume_token(cursor, command, sizeof(command));

    if (command[0] == '\0' || strcasecmp(command, "inbox") == 0) {
        session_mail_render_inbox(ctx);
        return;
    }

    if (strcasecmp(command, "clear") == 0) {
        if (!session_user_data_load(ctx)) {
            session_send_system_line(
                ctx, session_command_localize(
                         ctx, "Mailbox storage is unavailable."));
            return;
        }

        ctx->user_data.mailbox_count = 0U;
        memset(ctx->user_data.mailbox, 0, sizeof(ctx->user_data.mailbox));
        if (session_user_data_commit(ctx)) {
            session_send_system_line(
                ctx, session_command_localize(ctx, "Mailbox cleared."));
        } else {
            session_send_system_line(
                ctx,
                session_command_localize(ctx, "Failed to update mailbox."));
        }
        return;
    }

    char target_token[SSH_CHATTER_USERNAME_LEN + SSH_CHATTER_IP_LEN];
    const char *msg_cursor = nullptr;

    // Names may contain spaces, so '|' separates the recipient from the
    // message. Without '|', fall back to whitespace-separated tokens.
    const char *rest = arguments != nullptr ? arguments : "";
    while (*rest != '\0' && isspace((unsigned char)*rest)) {
        ++rest;
    }
    if (strcasecmp(command, "send") == 0) {
        rest = cursor;
        while (*rest != '\0' && isspace((unsigned char)*rest)) {
            ++rest;
        }
    }

    const char *separator = strchr(rest, '|');
    if (separator != nullptr) {
        size_t token_len = (size_t)(separator - rest);
        if (token_len >= sizeof(target_token)) {
            token_len = sizeof(target_token) - 1U;
        }
        memcpy(target_token, rest, token_len);
        target_token[token_len] = '\0';
        trim_whitespace_inplace(target_token);
        msg_cursor = separator + 1;
    } else if (strcasecmp(command, "send") == 0) {
        cursor = session_consume_token(cursor, target_token, sizeof(target_token));
        msg_cursor = cursor;
    } else {
        snprintf(target_token, sizeof(target_token), "%s", command);
        msg_cursor = cursor;
    }
    while (msg_cursor != nullptr && *msg_cursor != '\0' &&
           isspace((unsigned char)*msg_cursor)) {
        ++msg_cursor;
    }

    if (target_token[0] == '\0' || msg_cursor == nullptr || msg_cursor[0] == '\0') {
        session_mail_send_usage(ctx, true);
        return;
    }

    char target[SSH_CHATTER_USERNAME_LEN];
    char target_ip[SSH_CHATTER_IP_LEN];
    target_ip[0] = '\0';
    const char *at = strchr(target_token, '@');
    if (at != nullptr) {
        size_t name_len = (size_t)(at - target_token);
        if (name_len == 0U || name_len >= sizeof(target)) {
            session_send_system_line(
                ctx,
                session_command_localize(ctx, "Invalid mailbox recipient."));
            return;
        }
        memcpy(target, target_token, name_len);
        target[name_len] = '\0';
        const char *ip_part = at + 1;
        if (ip_part[0] != '\0') {
            if (strlen(ip_part) >= sizeof(target_ip)) {
                session_send_system_line(
                    ctx,
                    session_command_localize(ctx, "Recipient IP is too long."));
                return;
            }
            snprintf(target_ip, sizeof(target_ip), "%s", ip_part);
        }
    } else {
        snprintf(target, sizeof(target), "%s", target_token);
    }

    if (target[0] == '\0') {
        session_send_system_line(
            ctx, session_command_localize(ctx, "Invalid mailbox recipient."));
        return;
    }

    char message[USER_DATA_MAILBOX_MESSAGE_LEN];
    snprintf(message, sizeof(message), "%s", msg_cursor);
        trim_whitespace_inplace(message);
        if (message[0] == '\0') {
            session_send_system_line(
                ctx, session_command_localize(
                         ctx, "Mailbox message cannot be empty."));
            return;
        }

        char error[128];
        bool to_ddial = false;
        if (!host_mail_send(ctx->owner, ctx->user.name, target, target_ip,
                            msg_cursor, &to_ddial, error, sizeof(error))) {
            static const char kOpenPrefix[] = "Unable to open mailbox for ";
            if (strncmp(error, kOpenPrefix, sizeof(kOpenPrefix) - 1U) == 0) {
                session_command_snprintf(ctx, error, sizeof(error),
                                         "Unable to open mailbox for %s.",
                                         target);
                session_send_system_line(ctx, error);
            } else {
                session_send_system_line(
                    ctx, session_command_localize(
                             ctx, error[0] != '\0'
                                      ? error
                                      : "Unable to deliver mailbox message."));
            }
            return;
        }
        if (to_ddial) {
            char confirmation[SSH_CHATTER_MESSAGE_LIMIT];
            session_command_snprintf(ctx, confirmation, sizeof(confirmation),
                                     "Delivered mailbox message to %s (DDial).",
                                     target);
            session_send_system_line(ctx, confirmation);
            return;
        }

        if (strcasecmp(target, ctx->user.name) == 0) {
            (void)session_user_data_load(ctx);
        }

        char confirmation[SSH_CHATTER_MESSAGE_LIMIT];
        session_command_snprintf(ctx, confirmation, sizeof(confirmation),
                                 "Delivered mailbox message to %s.", target);
        session_send_system_line(ctx, confirmation);
}

static void session_handle_reaction(session_ctx_t *ctx, size_t reaction_index,
                                    const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr ||
        reaction_index >= SSH_CHATTER_REACTION_KIND_COUNT) {
        return;
    }

    const reaction_descriptor_t *descriptor =
        &REACTION_DEFINITIONS[reaction_index];

    char usage[64];
    char canonical[32];
    int written =
        snprintf(canonical, sizeof(canonical), "/%s", descriptor->command);
    if (written < 0 || (size_t)written >= sizeof(canonical)) {
        canonical[0] = '\0';
    }
    const char *command_label =
        canonical[0] != '\0'
            ? session_command_alias_preferred_by_canonical(ctx, canonical)
            : nullptr;
    if (command_label == nullptr || command_label[0] == '\0') {
        command_label = canonical;
    }

    snprintf(usage, sizeof(usage), "Usage: %s <message-id>",
             command_label != nullptr ? command_label : "");

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

    chat_history_entry_t updated = {0};
    if (!host_history_apply_reaction(ctx->owner, message_id, reaction_index,
                                     &updated)) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        char label[32];
        if (!host_compact_id_encode(message_id, label, sizeof(label))) {
            snprintf(label, sizeof(label), "%" PRIu64, message_id);
        }
        snprintf(message, sizeof(message),
                 "Message #%s was not found or cannot be reacted to.", label);
        session_send_system_line(ctx, message);
        return;
    }

    char confirmation[SSH_CHATTER_MESSAGE_LIMIT];
    char label[32];
    if (!host_compact_id_encode(message_id, label, sizeof(label))) {
        snprintf(label, sizeof(label), "%" PRIu64, message_id);
    }
    snprintf(confirmation, sizeof(confirmation), "Added %s %s to message #%s.",
             descriptor->icon, descriptor->label, label);
    session_send_system_line(ctx, confirmation);
    chat_room_broadcast_reaction_update(ctx->owner, &updated);
}


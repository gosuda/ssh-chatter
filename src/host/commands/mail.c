static void session_mail_render_inbox(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    if (!session_user_data_load(ctx)) {
        session_send_system_line(ctx, "Mailbox storage is unavailable.");
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

static void session_handle_mail(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!session_user_data_available(ctx) && !ctx->owner->user_data_ready) {
        session_send_system_line(ctx, "Mailbox storage is unavailable.");
        return;
    }

    const char *cursor = arguments != nullptr ? arguments : "";
    char command[16];
    cursor = session_consume_token(cursor, command, sizeof(command));

    if (command[0] == '\0' || strcasecmp(command, "inbox") == 0) {
        session_mail_render_inbox(ctx);
        return;
    }

    if (strcasecmp(command, "send") == 0) {
        char target_token[SSH_CHATTER_USERNAME_LEN + SSH_CHATTER_IP_LEN];
        cursor =
            session_consume_token(cursor, target_token, sizeof(target_token));
        if (target_token[0] == '\0' || cursor == nullptr || cursor[0] == '\0') {
            session_send_system_line(ctx,
                                     "Usage: /mail send <user[@ip]> <message>");
            return;
        }

        char target[SSH_CHATTER_USERNAME_LEN];
        char target_ip[SSH_CHATTER_IP_LEN];
        target_ip[0] = '\0';
        const char *at = strchr(target_token, '@');
        if (at != nullptr) {
            size_t name_len = (size_t)(at - target_token);
            if (name_len == 0U || name_len >= sizeof(target)) {
                session_send_system_line(ctx, "Invalid mailbox recipient.");
                return;
            }
            memcpy(target, target_token, name_len);
            target[name_len] = '\0';
            const char *ip_part = at + 1;
            if (ip_part[0] != '\0') {
                if (strlen(ip_part) >= sizeof(target_ip)) {
                    session_send_system_line(ctx, "Recipient IP is too long.");
                    return;
                }
                snprintf(target_ip, sizeof(target_ip), "%s", ip_part);
            }
        } else {
            snprintf(target, sizeof(target), "%s", target_token);
        }

        if (target[0] == '\0') {
            session_send_system_line(ctx, "Invalid mailbox recipient.");
            return;
        }

        char message[USER_DATA_MAILBOX_MESSAGE_LEN];
        snprintf(message, sizeof(message), "%s", cursor);
        trim_whitespace_inplace(message);
        if (message[0] == '\0') {
            session_send_system_line(ctx, "Mailbox message cannot be empty.");
            return;
        }

        char error[128];
        if (!host_user_data_send_mail(
                ctx->owner, target, target_ip[0] != '\0' ? target_ip : nullptr,
                ctx->user.name, message, error, sizeof(error))) {
            if (error[0] != '\0') {
                session_send_system_line(ctx, error);
            } else {
                session_send_system_line(ctx,
                                         "Unable to deliver mailbox message.");
            }
            return;
        }

        if (strcasecmp(target, ctx->user.name) == 0) {
            (void)session_user_data_load(ctx);
        }

        char confirmation[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(confirmation, sizeof(confirmation),
                 "Delivered mailbox message to %s.", target);
        session_send_system_line(ctx, confirmation);
        return;
    }

    if (strcasecmp(command, "clear") == 0) {
        if (!session_user_data_load(ctx)) {
            session_send_system_line(ctx, "Mailbox storage is unavailable.");
            return;
        }

        ctx->user_data.mailbox_count = 0U;
        memset(ctx->user_data.mailbox, 0, sizeof(ctx->user_data.mailbox));
        if (session_user_data_commit(ctx)) {
            session_send_system_line(ctx, "Mailbox cleared.");
        } else {
            session_send_system_line(ctx, "Failed to update mailbox.");
        }
        return;
    }

    session_send_system_line(
        ctx, "Usage: /mail [inbox|send <user> <message>|clear]");
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


/**
 * @file host_session_commands.c
 * @desc File-level documentation for host_session_commands.c, describing its role
 *       in the SSH-Chatter server and providing a consistent header
 *       comment format across C sources.
 * @return None.
 */

#include "host_internal.h"
#include "ssh_chatter/security_layer.h"
// Command handlers for chat interactions, media, and user utilities.

static void session_handle_reply(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(usage, sizeof(usage), "Usage: %s <message-id|r<reply-id>> <text>",
             session_command_alias_preferred_by_canonical(ctx, "/reply"));

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

    char *saveptr = nullptr;
    char *target = strtok_r(working, " \t", &saveptr);
    if (target == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    char *text = nullptr;
    if (saveptr != nullptr) {
        text = saveptr;
        while (*text == ' ' || *text == '\t') {
            ++text;
        }
    }

    if (text == nullptr || *text == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    bool targeting_reply = false;
    if (*target == '#') {
        ++target;
    }
    if (*target == 'r' || *target == 'R') {
        targeting_reply = true;
        ++target;
    }

    if (*target == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    uint64_t identifier = 0U;
    if (!host_compact_id_decode(target, &identifier) || identifier == 0U) {
        session_send_system_line(ctx, usage);
        return;
    }

    chat_reply_entry_t parent_reply = {0};
    uint64_t parent_reply_id = 0U;
    uint64_t parent_message_id = 0U;

    if (targeting_reply) {
        if (!host_replies_find_entry_by_id(ctx->owner, identifier,
                                           &parent_reply)) {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            char label[32];
            if (!host_compact_id_encode(identifier, label, sizeof(label))) {
                snprintf(label, sizeof(label), "%" PRIu64, identifier);
            }
            snprintf(message, sizeof(message), "Reply r#%s was not found.",
                     label);
            session_send_system_line(ctx, message);
            return;
        }
        parent_message_id = parent_reply.parent_message_id;
        parent_reply_id = parent_reply.reply_id;
    } else {
        chat_history_entry_t parent_entry = {0};
        if (host_history_find_entry_by_id(ctx->owner, identifier,
                                          &parent_entry)) {
            parent_message_id = parent_entry.message_id;
        } else if (host_replies_find_entry_by_id(ctx->owner, identifier,
                                                 &parent_reply)) {
            parent_message_id = parent_reply.parent_message_id;
            parent_reply_id = parent_reply.reply_id;
        } else {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            char label[32];
            if (!host_compact_id_encode(identifier, label, sizeof(label))) {
                snprintf(label, sizeof(label), "%" PRIu64, identifier);
            }
            snprintf(message, sizeof(message),
                     "Message or reply #%s was not found.", label);
            session_send_system_line(ctx, message);
            return;
        }
    }

    if (parent_message_id == 0U) {
        session_send_system_line(ctx, "Unable to determine reply target.");
        return;
    }

    char normalized[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(normalized, sizeof(normalized), "%s", text);
    session_normalize_newlines(normalized);
    trim_whitespace_inplace(normalized);
    for (size_t idx = 0U; normalized[idx] != '\0'; ++idx) {
        if (normalized[idx] == '\n') {
            normalized[idx] = ' ';
        }
    }
    trim_whitespace_inplace(normalized);

    if (normalized[0] == '\0') {
        session_send_system_line(ctx, "Reply text cannot be empty.");
        return;
    }

    chat_reply_entry_t entry = {0};
    entry.parent_message_id = parent_message_id;
    entry.parent_reply_id = parent_reply_id;
    entry.created_at = time(nullptr);
    snprintf(entry.username, sizeof(entry.username), "%s", ctx->user.name);
    snprintf(entry.message, sizeof(entry.message), "%s", normalized);

    chat_reply_entry_t stored = {0};
    if (!host_replies_commit_entry(ctx->owner, &entry, &stored)) {
        session_send_system_line(ctx, "Unable to record reply.");
        return;
    }

    // Ensure the sender leaves scrollback so the reply appears immediately.
    session_scrollback_reset_position(ctx);

    // Add reply to chat history
    const char *target_prefix = (stored.parent_reply_id == 0U) ? "#" : "r#";
    uint64_t target_id = (stored.parent_reply_id == 0U)
                             ? stored.parent_message_id
                             : stored.parent_reply_id;

    char reply_label[32];
    if (!host_compact_id_encode(stored.reply_id, reply_label,
                                sizeof(reply_label))) {
        snprintf(reply_label, sizeof(reply_label), "%" PRIu64, stored.reply_id);
    }

    char target_label[32];
    if (!host_compact_id_encode(target_id, target_label,
                                sizeof(target_label))) {
        snprintf(target_label, sizeof(target_label), "%" PRIu64, target_id);
    }

    char reply_message[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(reply_message, sizeof(reply_message), "->[r#%s %s%s] %s: %s",
             reply_label, target_prefix, target_label, stored.username,
             stored.message);

    chat_history_entry_t reply_entry = {0};
    if (!host_history_record_system(ctx->owner, reply_message, &reply_entry)) {
        session_send_system_line(ctx, "Unable to broadcast reply.");
        return;
    }

    // Show the reply to the sender immediately (just like regular chat messages)
    session_send_history_entry(ctx, &reply_entry);

    // Broadcast the reply entry to all users so it appears in chat buffer
    chat_room_broadcast_entry(&ctx->owner->room, &reply_entry, ctx);
}

static void session_handle_filestore(session_ctx_t *ctx, const char *arguments)
{
    (void)arguments;
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    char listing[SSH_CHATTER_MESSAGE_LIMIT];
    if (!host_file_storage_list(ctx->owner, listing, sizeof(listing))) {
        session_send_system_line(ctx, listing);
    } else {
        session_send_system_line(ctx, listing);
    }

    session_send_system_line(
        ctx, "SSH/SCP users: scp <file> user@host:/name | TELNET users: "
             "/filestore-upload [/<path>] or /filestore-download <name>");
}

static void session_handle_filestore_upload(session_ctx_t *ctx,
                                            const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (ctx->transport_kind != SESSION_TRANSPORT_TELNET) {
        session_send_system_line(
            ctx, "Uploads over SSH should use scp: scp file "
                 "user@host:/name");
        return;
    }

    bool has_target = false;
    char resolved[PATH_MAX];
    char display[PATH_MAX];
    resolved[0] = '\0';
    display[0] = '\0';

    if (arguments != nullptr) {
        char working[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(working, sizeof(working), "%s", arguments);
        trim_whitespace_inplace(working);
        if (working[0] != '\0') {
            if (!file_transfer_resolve_path(ctx->owner, working, resolved,
                                            sizeof(resolved), display,
                                            sizeof(display))) {
                session_send_system_line(
                    ctx,
                    "Invalid destination. Use paths under / without ..");
                return;
            }
            has_target = true;
        }
    }

    if (has_target) {
        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice),
                 "* Ready to receive file into %s. Start ZMODEM SEND now.",
                 display);
        session_send_system_line(ctx, notice);
    } else {
        session_send_system_line(
            ctx, "* Ready to receive file. Start ZMODEM SEND now.");
    }

    if (!file_transfer_telnet_receive(ctx, has_target ? resolved : nullptr)) {
        session_send_system_line(ctx, "Upload failed.");
        return;
    }

    if (has_target) {
        char success[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(success, sizeof(success), "* Upload stored at %s.", display);
        session_send_system_line(ctx, success);
    } else {
        session_send_system_line(ctx, "* Upload complete.");
    }
}

static void session_handle_filestore_download(session_ctx_t *ctx,
                                              const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (ctx->transport_kind != SESSION_TRANSPORT_TELNET) {
        session_send_system_line(
            ctx, "Downloads over SSH should use scp: scp "
                 "user@host:/name ./");
        return;
    }

    if (arguments == nullptr) {
        session_send_system_line(ctx, "Usage: /filestore-download <name>");
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);
    if (working[0] == '\0') {
        session_send_system_line(ctx, "Usage: /filestore-download <name>");
        return;
    }

    file_transfer_telnet_send(ctx, working);
}

static void session_handle_image(session_ctx_t *ctx, const char *arguments)
{
    static const char *kUsage = "Usage: /image <url> [caption]";
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/image", kUsage, usage, sizeof(usage));

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

    char *saveptr = nullptr;
    char *url = strtok_r(working, " \t", &saveptr);
    if (url == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    char *caption = nullptr;
    if (saveptr != nullptr) {
        caption = saveptr;
        while (*caption == ' ' || *caption == '\t') {
            ++caption;
        }
        if (*caption == '\0') {
            caption = nullptr;
        }
    }

    if (strnlen(url, SSH_CHATTER_ATTACHMENT_TARGET_LEN) >=
        SSH_CHATTER_ATTACHMENT_TARGET_LEN) {
        session_send_system_line(ctx, "Image URL is too long.");
        return;
    }

    chat_history_entry_t entry;
    chat_history_entry_prepare_user(&entry, ctx, "shared an image", false);
    entry.attachment_type = CHAT_ATTACHMENT_IMAGE;
    snprintf(entry.attachment_target, sizeof(entry.attachment_target), "%s",
             url);
    if (caption != nullptr) {
        trim_whitespace_inplace(caption);
        snprintf(entry.attachment_caption, sizeof(entry.attachment_caption),
                 "%s", caption);
    }

    chat_history_entry_t stored = {0};
    if (!host_history_commit_entry(ctx->owner, &entry, &stored)) {
        session_send_system_line(ctx, "Unable to record image message.");
        return;
    }

    session_send_history_entry(ctx, &stored);
    chat_room_broadcast_entry(&ctx->owner->room, &stored, ctx);
    host_notify_external_clients(ctx->owner, &stored);
}

static void session_handle_video(session_ctx_t *ctx, const char *arguments)
{
    static const char *kUsage = "Usage: /video <url> [caption]";
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/video", kUsage, usage, sizeof(usage));

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

    char *saveptr = nullptr;
    char *url = strtok_r(working, " \t", &saveptr);
    if (url == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    char *caption = nullptr;
    if (saveptr != nullptr) {
        caption = saveptr;
        while (*caption == ' ' || *caption == '\t') {
            ++caption;
        }
        if (*caption == '\0') {
            caption = nullptr;
        }
    }

    if (strnlen(url, SSH_CHATTER_ATTACHMENT_TARGET_LEN) >=
        SSH_CHATTER_ATTACHMENT_TARGET_LEN) {
        session_send_system_line(ctx, "Video link is too long.");
        return;
    }

    chat_history_entry_t entry;
    chat_history_entry_prepare_user(&entry, ctx, "shared a video", false);
    entry.attachment_type = CHAT_ATTACHMENT_VIDEO;
    snprintf(entry.attachment_target, sizeof(entry.attachment_target), "%s",
             url);
    if (caption != nullptr) {
        trim_whitespace_inplace(caption);
        snprintf(entry.attachment_caption, sizeof(entry.attachment_caption),
                 "%s", caption);
    }

    chat_history_entry_t stored = {0};
    if (!host_history_commit_entry(ctx->owner, &entry, &stored)) {
        session_send_system_line(ctx, "Unable to record video message.");
        return;
    }

    session_send_history_entry(ctx, &stored);
    chat_room_broadcast_entry(&ctx->owner->room, &stored, ctx);
    host_notify_external_clients(ctx->owner, &stored);
}

static void session_handle_audio(session_ctx_t *ctx, const char *arguments)
{
    static const char *kUsage = "Usage: /audio <url> [caption]";
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/audio", kUsage, usage, sizeof(usage));

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

    char *saveptr = nullptr;
    char *url = strtok_r(working, " \t", &saveptr);
    if (url == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    char *caption = nullptr;
    if (saveptr != nullptr) {
        caption = saveptr;
        while (*caption == ' ' || *caption == '\t') {
            ++caption;
        }
        if (*caption == '\0') {
            caption = nullptr;
        }
    }

    if (strnlen(url, SSH_CHATTER_ATTACHMENT_TARGET_LEN) >=
        SSH_CHATTER_ATTACHMENT_TARGET_LEN) {
        session_send_system_line(ctx, "Audio link is too long.");
        return;
    }

    chat_history_entry_t entry;
    chat_history_entry_prepare_user(&entry, ctx, "shared an audio clip", false);
    entry.attachment_type = CHAT_ATTACHMENT_AUDIO;
    snprintf(entry.attachment_target, sizeof(entry.attachment_target), "%s",
             url);
    if (caption != nullptr) {
        trim_whitespace_inplace(caption);
        snprintf(entry.attachment_caption, sizeof(entry.attachment_caption),
                 "%s", caption);
    }

    chat_history_entry_t stored = {0};
    if (!host_history_commit_entry(ctx->owner, &entry, &stored)) {
        session_send_system_line(ctx, "Unable to record audio message.");
        return;
    }

    session_send_history_entry(ctx, &stored);
    chat_room_broadcast_entry(&ctx->owner->room, &stored, ctx);
    host_notify_external_clients(ctx->owner, &stored);
}

static void session_handle_files(session_ctx_t *ctx, const char *arguments)
{
    static const char *kUsage = "Usage: /files <url> [caption]";
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/files", kUsage, usage, sizeof(usage));

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

    char *saveptr = nullptr;
    char *url = strtok_r(working, " \t", &saveptr);
    if (url == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    char *caption = nullptr;
    if (saveptr != nullptr) {
        caption = saveptr;
        while (*caption == ' ' || *caption == '\t') {
            ++caption;
        }
        if (*caption == '\0') {
            caption = nullptr;
        }
    }

    if (strnlen(url, SSH_CHATTER_ATTACHMENT_TARGET_LEN) >=
        SSH_CHATTER_ATTACHMENT_TARGET_LEN) {
        session_send_system_line(ctx, "File link is too long.");
        return;
    }

    chat_history_entry_t entry;
    chat_history_entry_prepare_user(&entry, ctx, "shared a file", false);
    entry.attachment_type = CHAT_ATTACHMENT_FILE;
    snprintf(entry.attachment_target, sizeof(entry.attachment_target), "%s",
             url);
    if (caption != nullptr) {
        trim_whitespace_inplace(caption);
        snprintf(entry.attachment_caption, sizeof(entry.attachment_caption),
                 "%s", caption);
    }

    chat_history_entry_t stored = {0};
    if (!host_history_commit_entry(ctx->owner, &entry, &stored)) {
        session_send_system_line(ctx, "Unable to record file message.");
        return;
    }

    session_send_history_entry(ctx, &stored);
    chat_room_broadcast_entry(&ctx->owner->room, &stored, ctx);
    host_notify_external_clients(ctx->owner, &stored);
}

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
    host_notify_external_clients(ctx->owner, &updated);
}

static void session_handle_usercount(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    size_t count = 0U;
    ttak_mutex_lock(&ctx->owner->room.lock);
    count = ctx->owner->room.member_count;
    ttak_mutex_unlock(&ctx->owner->room.lock);

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(message, sizeof(message),
             "There %s currently %zu user%s connected.",
             count == 1U ? "is" : "are", count, count == 1U ? "" : "s");

    host_history_record_system(ctx->owner, message, nullptr);
    session_send_system_line(ctx, message);
}

static void session_handle_today(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    session_send_system_line(ctx, "Today's function has been retired.");
}

static void session_handle_date(session_ctx_t *ctx, const char *arguments)
{
    static const char *kUsage = "Usage: /date <Area/Location>";

    if (ctx == nullptr) {
        return;
    }

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/date", kUsage, usage, sizeof(usage));

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

    char sanitized[PATH_MAX];
    if (!timezone_sanitize_identifier(working, sanitized, sizeof(sanitized))) {
        session_send_system_line(ctx,
                                 "Timezone names may only include letters, "
                                 "numbers, '/', '_', '-', '+', or '.'.");
        return;
    }

    char resolved[PATH_MAX];
    if (!timezone_resolve_identifier(sanitized, resolved, sizeof(resolved))) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "Unknown timezone '%.128s'.",
                 working);
        session_send_system_line(ctx, message);
        return;
    }

    const char *previous_tz = getenv("TZ");
    char previous_copy[PATH_MAX];
    bool had_previous = false;
    if (previous_tz != nullptr) {
        int prev_written =
            snprintf(previous_copy, sizeof(previous_copy), "%s", previous_tz);
        if (prev_written >= 0 && (size_t)prev_written < sizeof(previous_copy)) {
            had_previous = true;
        }
    }

    bool tz_applied = false;

    if (setenv("TZ", resolved, 1) != 0) {
        session_send_system_line(ctx, "Unable to adjust timezone right now.");
        return;
    }

    tzset();
    tz_applied = true;

    time_t now = time(nullptr);
    if (now == (time_t)-1) {
        session_send_system_line(ctx, "Unable to determine current time.");
        goto cleanup;
    }

    struct tm tm_now;
#if defined(_POSIX_THREAD_SAFE_FUNCTIONS)
    if (localtime_r(&now, &tm_now) == nullptr) {
        session_send_system_line(ctx,
                                 "Unable to compute the requested local time.");
        goto cleanup;
    }
#else
    struct tm *tmp = localtime(&now);
    if (tmp == nullptr) {
        session_send_system_line(ctx,
                                 "Unable to compute the requested local time.");
        goto cleanup;
    }
    tm_now = *tmp;
#endif

    char formatted[128];
    if (strftime(formatted, sizeof(formatted), "%Y-%m-%d %H:%M:%S %Z (UTC%z)",
                 &tm_now) == 0) {
        session_send_system_line(ctx, "Unable to format the requested time.");
        goto cleanup;
    }

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(message, sizeof(message), "%.128s -> %s", resolved, formatted);
    session_send_system_line(ctx, message);

cleanup:
    if (tz_applied) {
        if (had_previous) {
            setenv("TZ", previous_copy, 1);
        } else {
            unsetenv("TZ");
        }
        tzset();
    }
}

static void session_handle_os(session_ctx_t *ctx, const char *arguments)
{
    static const char *kUsage =
        "Usage: /os "
        "<windows|macos|linux|freebsd|ios|android|watchos|solaris|openbsd|"
        "netbsd|dragonflybsd|reactos|tyzen|kdos|pcdos|msdos|drdos|bsd|haiku|"
        "zealos|templeos>";
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/os", kUsage, usage, sizeof(usage));

    if (arguments == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    char working[SSH_CHATTER_OS_NAME_LEN];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);
    if (working[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    for (size_t idx = 0U; working[idx] != '\0'; ++idx) {
        working[idx] = (char)tolower((unsigned char)working[idx]);
    }

    const os_descriptor_t *descriptor = session_lookup_os_descriptor(working);
    if (descriptor == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    snprintf(ctx->os_name, sizeof(ctx->os_name), "%s", descriptor->name);
    host_store_user_os(ctx->owner, ctx);
    session_refresh_output_encoding(ctx);

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(message, sizeof(message), "Recorded your operating system as %s.",
             descriptor->display);
    session_send_system_line(ctx, message);
}

static void session_handle_getos(session_ctx_t *ctx, const char *arguments)
{
    static const char *kUsage = "Usage: /getos <username>";
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/getos", kUsage, usage, sizeof(usage));

    if (arguments == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    char target[SSH_CHATTER_USERNAME_LEN];
    snprintf(target, sizeof(target), "%s", arguments);
    trim_whitespace_inplace(target);
    if (target[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    char os_buffer[SSH_CHATTER_OS_NAME_LEN];
    if (!host_lookup_user_os(ctx->owner, target, os_buffer,
                             sizeof(os_buffer))) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message),
                 "No operating system is recorded for %s.", target);
        session_send_system_line(ctx, message);
        return;
    }

    const os_descriptor_t *descriptor = session_lookup_os_descriptor(os_buffer);
    const char *display =
        descriptor != nullptr ? descriptor->display : os_buffer;

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(message, sizeof(message), "%s reports using %s.", target, display);
    session_send_system_line(ctx, message);
}

static void session_handle_pair(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    session_send_system_line(ctx, "Pair matches have been discontinued.");
}

static void session_handle_connected(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    char buffer[SSH_CHATTER_MESSAGE_LIMIT];
    size_t offset = 0U;
    size_t count = 0U;

    ttak_mutex_lock(&ctx->owner->room.lock);
    for (size_t idx = 0U; idx < ctx->owner->room.member_count; ++idx) {
        session_ctx_t *member = ctx->owner->room.members[idx];
        if (member == nullptr) {
            continue;
        }

        const size_t prefix = count == 0U ? 0U : 2U;
        size_t name_len = strnlen(member->user.name, sizeof(member->user.name));
        if (offset + prefix + name_len >= sizeof(buffer)) {
            break;
        }
        if (count > 0U) {
            buffer[offset++] = ',';
            buffer[offset++] = ' ';
        }
        memcpy(buffer + offset, member->user.name, name_len);
        offset += name_len;
        buffer[offset] = '\0';
        ++count;
    }
    ttak_mutex_unlock(&ctx->owner->room.lock);

    char header[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(header, sizeof(header), "Connected users (%zu):", count);
    session_send_system_line(ctx, header);
    if (count > 0U) {
        session_send_system_line(ctx, buffer);
    }
}

static void session_handle_alpha_centauri_landers(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    alpha_lander_entry_t entries[ALPHA_LANDERS_MAX_RECORDS];
    size_t entry_count = 0U;

    if (!host_alpha_landers_snapshot(ctx->owner, entries,
                                     ALPHA_LANDERS_MAX_RECORDS, &entry_count)) {
        session_send_system_line(
            ctx, "Unable to inspect landing records right now.");
        return;
    }

    session_send_system_line(
        ctx, "Alpha Centauri Landers -- Immigrants' Flag Hall of Fame:");

    if (entry_count == 0U) {
        session_send_system_line(ctx, "No landings logged yet. Finish the "
                                      "expedition to claim the first flag!");
        return;
    }

    qsort(entries, entry_count, sizeof(entries[0]), alpha_lander_entry_compare);

    size_t display_count = entry_count < ALPHA_LANDERS_DISPLAY_LIMIT
                               ? entry_count
                               : ALPHA_LANDERS_DISPLAY_LIMIT;
    for (size_t idx = 0U; idx < display_count; ++idx) {
        const alpha_lander_entry_t *lander = &entries[idx];
        char when[64];
        when[0] = '\0';
        if (lander->last_flag_timestamp != 0U) {
            time_t when_time = (time_t)lander->last_flag_timestamp;
            struct tm tm_buf;
            if (gmtime_r(&when_time, &tm_buf) != nullptr) {
                strftime(when, sizeof(when), "%Y-%m-%d %H:%M UTC", &tm_buf);
            }
        }
        if (when[0] == '\0') {
            snprintf(when, sizeof(when), "unknown");
        }

        char line[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(line, sizeof(line),
                 "#%zu %s -- flags planted: %u (last landing %s)", idx + 1U,
                 lander->username, lander->flag_count, when);
        session_send_system_line(ctx, line);
    }

    if (entry_count > display_count) {
        size_t remaining = entry_count - display_count;
        char summary[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(summary, sizeof(summary),
                 "...and %zu more landers recorded in the archives.", remaining);
        session_send_system_line(ctx, summary);
    }
}

static bool session_parse_birthday(const char *input, char *normalized,
                                   size_t length)
{
    if (input == nullptr || normalized == nullptr || length < 11U) {
        return false;
    }

    char working[32];
    snprintf(working, sizeof(working), "%s", input);
    trim_whitespace_inplace(working);

    if (strlen(working) != 10U || working[4] != '-' || working[7] != '-') {
        return false;
    }

    for (size_t idx = 0U; idx < 10U; ++idx) {
        if (idx == 4U || idx == 7U) {
            continue;
        }
        if (!isdigit((unsigned char)working[idx])) {
            return false;
        }
    }

    int year = atoi(working);
    int month = atoi(working + 5);
    int day = atoi(working + 8);

    if (year < 1900 || year > 9999 || month < 1 || month > 12 || day < 1) {
        return false;
    }

    static const int days_in_month[] = {31, 28, 31, 30, 31, 30,
                                        31, 31, 30, 31, 30, 31};
    int max_day = days_in_month[month - 1];
    bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    if (month == 2 && leap) {
        max_day = 29;
    }
    if (day > max_day) {
        return false;
    }

    char formatted[16];
    int written = snprintf(formatted, sizeof(formatted), "%04d-%02d-%02d", year,
                           month, day);
    if (written <= 0 || written >= (int)sizeof(formatted)) {
        return false;
    }
    if ((size_t)(written + 1) > length) {
        return false;
    }
    snprintf(normalized, length, "%s", formatted);
    return true;
}

static void session_handle_birthday(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (arguments == nullptr) {
        session_send_system_line(ctx, "Usage: /birthday YYYY-MM-DD");
        return;
    }

    char normalized[16];
    if (!session_parse_birthday(arguments, normalized, sizeof(normalized))) {
        session_send_system_line(ctx,
                                 "Invalid date. Use /birthday YYYY-MM-DD.");
        return;
    }

    ctx->has_birthday = true;
    snprintf(ctx->birthday, sizeof(ctx->birthday), "%s", normalized);
    host_store_birthday(ctx->owner, ctx, normalized);

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(message, sizeof(message), "Birthday recorded as %s.", normalized);
    session_send_system_line(ctx, message);
}

static void session_pw_auth_hex_encode(const uint8_t *input, size_t length,
                                       char *output, size_t output_length)
{
    if (output == nullptr || output_length == 0U) {
        return;
    }

    if (input == nullptr || length == 0U) {
        output[0] = '\0';
        return;
    }

    static const char kHexDigits[] = "0123456789abcdef";
    size_t offset = 0U;
    for (size_t idx = 0U; idx < length && offset + 2U < output_length; ++idx) {
        uint8_t value = input[idx];
        output[offset++] = kHexDigits[(value >> 4U) & 0x0FU];
        output[offset++] = kHexDigits[value & 0x0FU];
    }

    if (offset >= output_length) {
        offset = output_length - 1U;
    }
    output[offset] = '\0';
}

static bool session_pw_auth_format_line(const char *username,
                                        const uint8_t *salt, size_t salt_length,
                                        const uint8_t *hash, size_t hash_length,
                                        char *buffer, size_t buffer_length)
{
    if (username == nullptr || buffer == nullptr || buffer_length == 0U) {
        return false;
    }

    char salt_hex[64];
    char hash_hex[128];
    session_pw_auth_hex_encode(salt, salt_length, salt_hex, sizeof(salt_hex));
    session_pw_auth_hex_encode(hash, hash_length, hash_hex, sizeof(hash_hex));

    int written = snprintf(buffer, buffer_length, "%s:%s:%s", username,
                           salt_hex, hash_hex);
    return written >= 0 && (size_t)written < buffer_length;
}

static bool session_pw_auth_update(host_t *host, const char *username,
                                   const uint8_t *salt, size_t salt_length,
                                   const uint8_t *hash, size_t hash_length,
                                   bool has_password)
{
    if (host == nullptr || username == nullptr || username[0] == '\0') {
        return false;
    }

    if (host->pw_auth_file_path[0] == '\0') {
        return false;
    }

    if (!host_ensure_private_data_path(host, host->pw_auth_file_path, true)) {
        return false;
    }

    char temp_path[PATH_MAX];
    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp",
                           host->pw_auth_file_path);
    if (written < 0 || (size_t)written >= sizeof(temp_path)) {
        return false;
    }

    FILE *output = fopen(temp_path, "wb");
    if (output == nullptr) {
        return false;
    }

    bool success = true;
    bool replaced = false;
    FILE *input = fopen(host->pw_auth_file_path, "rb");
    if (input != nullptr) {
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        while (success && fgets(line, sizeof(line), input) != nullptr) {
            size_t length = strcspn(line, "\r\n");
            line[length] = '\0';

            char existing_name[SSH_CHATTER_USERNAME_LEN];
            size_t separator = strcspn(line, ":");
            if (separator >= length) {
                if (fprintf(output, "%s\n", line) < 0) {
                    success = false;
                }
                continue;
            }

            size_t copy_length = separator;
            if (copy_length >= sizeof(existing_name)) {
                copy_length = sizeof(existing_name) - 1U;
            }
            memcpy(existing_name, line, copy_length);
            existing_name[copy_length] = '\0';

            if (strcasecmp(existing_name, username) == 0) {
                if (has_password && !replaced) {
                    char formatted[256];
                    if (!session_pw_auth_format_line(
                            username, salt, salt_length, hash, hash_length,
                            formatted, sizeof(formatted)) ||
                        fprintf(output, "%s\n", formatted) < 0) {
                        success = false;
                    }
                    replaced = true;
                }
                continue;
            }

            if (fprintf(output, "%s\n", line) < 0) {
                success = false;
            }
        }

        if (input != nullptr) {
            int read_error = ferror(input);
            if (fclose(input) != 0) {
                success = false;
            }
            if (read_error != 0) {
                success = false;
            }
        }
    } else if (errno != ENOENT) {
        success = false;
    }

    if (success && has_password && !replaced) {
        char formatted[256];
        if (!session_pw_auth_format_line(username, salt, salt_length, hash,
                                         hash_length, formatted,
                                         sizeof(formatted)) ||
            fprintf(output, "%s\n", formatted) < 0) {
            success = false;
        }
    }

    if (success && fflush(output) != 0) {
        success = false;
    }

    if (success) {
        int fd = fileno(output);
        if (fd >= 0 && fsync(fd) != 0) {
            success = false;
        }
    }

    if (fclose(output) != 0) {
        success = false;
    }

    if (!success) {
        unlink(temp_path);
        return false;
    }

    if (rename(temp_path, host->pw_auth_file_path) != 0) {
        unlink(temp_path);
        return false;
    }

    if (chmod(host->pw_auth_file_path, S_IRUSR | S_IWUSR) != 0) {
        return false;
    }

    return true;
}

static void session_handle_setpw(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (arguments != nullptr && strlen(arguments) > 128) {
        session_send_system_line(ctx,
                                 "Password is too long (max 128 characters).");
        return;
    }

    if (!session_user_data_load(ctx)) {
        session_send_system_line(ctx, "Unable to load user data.");
        return;
    }

    if (arguments == nullptr || arguments[0] == '\0') {
        memset(ctx->user_data.password_salt, 0,
               sizeof(ctx->user_data.password_salt));
        memset(ctx->user_data.password_hash, 0,
               sizeof(ctx->user_data.password_hash));

        if (session_user_data_commit(ctx)) {
            session_send_system_line(ctx, "Password removed.");
            if (!session_pw_auth_update(ctx->owner, ctx->user.name, nullptr, 0U,
                                        nullptr, 0U, false)) {
                session_send_system_line(
                    ctx, "Warning: unable to update pw_auth.dat.");
            }
        } else {
            session_send_system_line(ctx, "Failed to remove password.");
        }
        return;
    }

    ttak_mutex_lock(&ctx->nickname_reserve_lock);
    strncpy(ctx->reserved_nicknames[ctx->reserved_nicknames_len],
            ctx->user.name, SSH_CHATTER_USERNAME_LEN);
    ctx->reserved_nicknames_len++;
    ttak_mutex_unlock(&ctx->nickname_reserve_lock);
    security_layer_generate_salt(ctx->user_data.password_salt);
    security_layer_hash_password(arguments, ctx->user_data.password_salt,
                                 ctx->user_data.password_hash);

    if (session_user_data_commit(ctx)) {
        session_send_system_line(ctx, "Password set successfully.");
        if (!session_pw_auth_update(
                ctx->owner, ctx->user.name, ctx->user_data.password_salt,
                sizeof(ctx->user_data.password_salt),
                ctx->user_data.password_hash,
                sizeof(ctx->user_data.password_hash), true)) {
            session_send_system_line(ctx,
                                     "Warning: unable to update pw_auth.dat.");
        }

        // Add user's nickname to reserved list if password was set
        if (ctx->owner != nullptr && ctx->owner->reserved_nicknames != nullptr &&
            ctx->owner->reserved_nicknames_len <
                ctx->owner->reserved_nicknames_capacity) {
            ttak_mutex_lock(&ctx->owner->nickname_reserve_lock);
            // Check if nickname is already reserved to avoid duplicates
            bool already_reserved = false;
            for (size_t i = 0; i < ctx->owner->reserved_nicknames_len; ++i) {
                if (strncmp(ctx->owner->reserved_nicknames[i], ctx->user.name,
                            SSH_CHATTER_USERNAME_LEN) == 0) {
                    already_reserved = true;
                    break;
                }
            }
            if (!already_reserved) {
                strncpy(ctx->owner->reserved_nicknames
                            [ctx->owner->reserved_nicknames_len],
                        ctx->user.name, SSH_CHATTER_USERNAME_LEN);
                ctx->owner->reserved_nicknames_len++;
            }
            ttak_mutex_unlock(&ctx->owner->nickname_reserve_lock);
        }

    } else {
        session_send_system_line(ctx, "Failed to save password.");
    }
}

static void session_handle_delpw(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    char target_user[SSH_CHATTER_USERNAME_LEN];
    bool self_delete = (arguments == nullptr || arguments[0] == '\0' ||
                        strcasecmp(arguments, ctx->user.name) == 0);

    if (self_delete) {
        snprintf(target_user, sizeof(target_user), "%s", ctx->user.name);
    } else {
        snprintf(target_user, sizeof(target_user), "%s", arguments);
        trim_whitespace_inplace(target_user);
    }

    if (!self_delete && !ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(
            ctx, "Only operators may remove other users' passwords.");
        return;
    }

    user_data_record_t user_data;
    bool data_loaded = false;
    if (self_delete) {
        if (session_user_data_load(ctx)) {
            user_data = ctx->user_data;
            data_loaded = true;
        }
    } else {
        data_loaded = host_user_data_load_existing(ctx->owner, target_user,
                                                   nullptr, &user_data, false);
    }

    if (!data_loaded) {
        if (self_delete) {
            session_send_system_line(ctx, "Unable to load your user data.");
        } else {
            session_send_system_line(ctx, "User not found.");
        }
        return;
    }

    bool was_set = false;
    for (size_t i = 0; i < sizeof(user_data.password_hash); ++i) {
        if (user_data.password_hash[i] != 0) {
            was_set = true;
            break;
        }
    }

    if (!was_set) {
        if (self_delete) {
            session_send_system_line(ctx, "You do not have a password set.");
        } else {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message),
                     "User %s does not have a password set.", target_user);
            session_send_system_line(ctx, message);
        }
        return;
    }

    memset(user_data.password_salt, 0, sizeof(user_data.password_salt));
    memset(user_data.password_hash, 0, sizeof(user_data.password_hash));

    bool success;
    if (self_delete) {
        ctx->user_data = user_data;
        success = session_user_data_commit(ctx);
    } else {
        success = user_data_save(ctx->owner->user_data_root, &user_data,
                                 user_data.last_ip);
    }

    if (success) {
        if (self_delete) {
            session_send_system_line(ctx, "Your password has been removed.");
        } else {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message),
                     "Password for %s has been removed.", target_user);
            session_send_system_line(ctx, message);
        }

        // Remove from reserved nicknames if password was deleted
        if (ctx->owner != nullptr && ctx->owner->reserved_nicknames != nullptr &&
            ctx->owner->reserved_nicknames_len > 0U) {
            ttak_mutex_lock(&ctx->owner->nickname_reserve_lock);
            for (size_t i = 0; i < ctx->owner->reserved_nicknames_len; ++i) {
                if (strncmp(ctx->owner->reserved_nicknames[i], target_user,
                            SSH_CHATTER_USERNAME_LEN) == 0) {
                    // Shift elements to the left to fill the gap
                    memmove(&ctx->owner->reserved_nicknames[i],
                            &ctx->owner->reserved_nicknames[i + 1],
                            (ctx->owner->reserved_nicknames_len - i - 1) *
                                sizeof(ctx->owner->reserved_nicknames[0]));
                    // Decrement the count
                    ctx->owner->reserved_nicknames_len--;
                    // Zero out the last used slot to prevent stale data
                    memset(&ctx->owner->reserved_nicknames
                                [ctx->owner->reserved_nicknames_len],
                           0, sizeof(ctx->owner->reserved_nicknames[0]));
                    break; // Found and removed, exit loop
                }
            }
            ttak_mutex_unlock(&ctx->owner->nickname_reserve_lock);
        }

        if (!session_pw_auth_update(ctx->owner, target_user, nullptr, 0U,
                                    nullptr, 0U, false)) {
            session_send_system_line(ctx,
                                     "Warning: unable to update pw_auth.dat.");
        }

    } else {
        session_send_system_line(ctx, "Failed to remove password.");
    }
}

static void session_handle_resetpw(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    // Only operators can reset passwords
    if (!ctx->user.is_operator) {
        session_send_system_line(ctx, "Only operators can reset passwords.");
        return;
    }

    if (arguments == nullptr || arguments[0] == '\0') {
        session_send_system_line(ctx, "Usage: /resetpw <nickname>");
        return;
    }

    char target_nickname[SSH_CHATTER_USERNAME_LEN];
    snprintf(target_nickname, sizeof(target_nickname), "%s", arguments);
    trim_whitespace_inplace(target_nickname);

    if (target_nickname[0] == '\0') {
        session_send_system_line(ctx, "Usage: /resetpw <nickname>");
        return;
    }

    // Load the target user's data
    user_data_record_t user_data;
    // We need to find the user's IP to load their data correctly if they are offline.
    // First, try to find the user in the current session list.
    session_ctx_t *target_session =
        chat_room_find_user(&ctx->owner->room, target_nickname);
    const char *target_ip = nullptr;

    if (target_session != nullptr) {
        target_ip = target_session->client_ip;
    } else {
        // If offline, try to find their last known IP from user data
        char last_ip[SSH_CHATTER_IP_LEN];
        if (host_lookup_last_ip(ctx->owner, target_nickname, last_ip,
                                sizeof(last_ip))) {
            target_ip = last_ip;
        }
    }

    if (target_ip == nullptr || target_ip[0] == '\0') {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message),
                 "Could not find IP for user '%s'. Cannot reset password.",
                 target_nickname);
        session_send_system_line(ctx, message);
        return;
    }

    if (!host_user_data_load_existing(ctx->owner, target_nickname, target_ip,
                                      &user_data, false)) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "Failed to load data for user '%s'.",
                 target_nickname);
        session_send_system_line(ctx, message);
        return;
    }

    // Clear the password salt and hash
    memset(user_data.password_salt, 0, sizeof(user_data.password_salt));
    memset(user_data.password_hash, 0, sizeof(user_data.password_hash));

    // Save the modified user data
    bool success =
        user_data_save(ctx->owner->user_data_root, &user_data, target_ip);

    if (success) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "Password for '%s' has been reset.",
                 target_nickname);
        session_send_system_line(ctx, message);

        if (!session_pw_auth_update(ctx->owner, target_nickname, nullptr, 0U,
                                    nullptr, 0U, false)) {
            session_send_system_line(ctx,
                                     "Warning: unable to update pw_auth.dat.");
        }

        // If the user is currently online, notify them or clear their session password state
        if (target_session != nullptr) {
            // Ideally, we'd also clear the password state in the session_ctx if it's cached,
            // but for now, just notifying the operator is sufficient.
            // A more robust solution might involve a signal to the target_session.
        }
    } else {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "Failed to reset password for '%s'.",
                 target_nickname);
        session_send_system_line(ctx, message);
    }
}
static void session_handle_revoke(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->user.is_lan_operator) {
        session_send_system_line(
            ctx, "Only LAN administrators may revoke operator privileges.");
        return;
    }

    if (arguments == nullptr) {
        session_send_system_line(ctx, "Usage: /revoke <ip-address>");
        return;
    }

    char ip[SSH_CHATTER_IP_LEN];
    snprintf(ip, sizeof(ip), "%s", arguments);
    trim_whitespace_inplace(ip);
    if (ip[0] == '\0') {
        session_send_system_line(ctx, "Usage: /revoke <ip-address>");
        return;
    }

    unsigned char buf[sizeof(struct in6_addr)];
    if (inet_pton(AF_INET, ip, buf) != 1 && inet_pton(AF_INET6, ip, buf) != 1) {
        session_send_system_line(ctx, "Provide a valid IPv4 or IPv6 address.");
        return;
    }

    bool removed = false;
    ttak_mutex_lock(&ctx->owner->lock);
    removed = host_remove_operator_grant_locked(ctx->owner, ip);
    if (removed) {
        host_state_save_locked(ctx->owner);
    }
    ttak_mutex_unlock(&ctx->owner->lock);

    if (!removed) {
        session_send_system_line(ctx,
                                 "No stored grant exists for that IP address.");
        return;
    }

    host_revoke_grant_from_ip(ctx->owner, ip);

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(message, sizeof(message), "Operator privileges revoked for %s.",
             ip);
    session_send_system_line(ctx, message);
}

static void session_handle_delete_message(session_ctx_t *ctx,
                                          const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    static const char *kUsage = "Usage: /delete-msg <id|start-end>";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/delete-msg", kUsage, usage,
                                 sizeof(usage));
    const bool is_operator = ctx->user.is_operator || ctx->user.is_lan_operator;

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

    uint64_t start_id = 0U;
    uint64_t end_id = 0U;
    char *dash = strchr(working, '-');
    if (dash != nullptr) {
        *dash = '\0';
        char *end_token = dash + 1;
        trim_whitespace_inplace(working);
        trim_whitespace_inplace(end_token);
        if (working[0] == '\0' || end_token[0] == '\0') {
            session_send_system_line(ctx, usage);
            return;
        }

        if (!host_compact_id_decode(working, &start_id) || start_id == 0U) {
            session_send_system_line(ctx, usage);
            return;
        }
        if (!host_compact_id_decode(end_token, &end_id) || end_id == 0U) {
            session_send_system_line(ctx, usage);
            return;
        }
        if (start_id > end_id) {
            session_send_system_line(ctx, "Start identifier must be less than "
                                          "or equal to the end identifier.");
            return;
        }
    } else {
        if (!host_compact_id_decode(working, &start_id) || start_id == 0U) {
            session_send_system_line(ctx, usage);
            return;
        }
        end_id = start_id;
    }

    if (!is_operator) {
        if (dash != nullptr) {
            session_send_system_line(
                ctx, "You may only delete your own single message.");
            return;
        }

        chat_history_entry_t entry = {0};
        if (!host_history_find_entry_by_id(ctx->owner, start_id, &entry)) {
            session_send_system_line(
                ctx, "No chat messages matched that identifier.");
            return;
        }

        const bool name_match = strcasecmp(entry.username, ctx->user.name) == 0;
        const bool ip_match = entry.user_ip[0] != '\0' &&
                              ctx->client_ip[0] != '\0' &&
                              strcmp(entry.user_ip, ctx->client_ip) == 0;
        if (!(name_match && ip_match)) {
            session_send_system_line(
                ctx, "You can only delete your own messages from this IP.");
            return;
        }
    }

    uint64_t first_removed = 0U;
    uint64_t last_removed = 0U;
    size_t replies_removed = 0U;
    size_t removed =
        host_history_delete_range(ctx->owner, start_id, end_id, &first_removed,
                                  &last_removed, &replies_removed);
    if (removed == 0U) {
        session_send_system_line(ctx,
                                 "No chat messages matched that identifier.");
        return;
    }

    char range_label[64];
    char first_label[32];
    if (!host_compact_id_encode(first_removed, first_label,
                                sizeof(first_label))) {
        snprintf(first_label, sizeof(first_label), "%" PRIu64, first_removed);
    }

    if (last_removed != 0U && last_removed != first_removed) {
        char last_label[32];
        if (!host_compact_id_encode(last_removed, last_label,
                                    sizeof(last_label))) {
            snprintf(last_label, sizeof(last_label), "%" PRIu64, last_removed);
        }
        snprintf(range_label, sizeof(range_label), "#%s-#%s", first_label,
                 last_label);
    } else {
        snprintf(range_label, sizeof(range_label), "#%s", first_label);
    }

    char reply_note[64];
    if (replies_removed > 0U) {
        snprintf(reply_note, sizeof(reply_note), " (%zu repl%s removed)",
                 replies_removed, replies_removed == 1U ? "y" : "ies");
    } else {
        reply_note[0] = '\0';
    }

    char acknowledgement[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(acknowledgement, sizeof(acknowledgement),
             "Removed %zu message%s (%s)%s.", removed, removed == 1U ? "" : "s",
             range_label, reply_note);
    session_send_system_line(ctx, acknowledgement);

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice), "* [%s] removed %s %s%s.", ctx->user.name,
             removed == 1U ? "message" : "messages", range_label, reply_note);
    host_history_record_system(ctx->owner, notice, nullptr);
    chat_room_broadcast(&ctx->owner->room, notice, nullptr);
}

static bool session_poll_parse_fields(const char *input, char *question,
                                      size_t question_length,
                                      char options[][SSH_CHATTER_MESSAGE_LIMIT],
                                      size_t max_options, size_t *option_count,
                                      char *error, size_t error_length)
{
    if (question != nullptr && question_length > 0U) {
        question[0] = '\0';
    }
    if (option_count != nullptr) {
        *option_count = 0U;
    }
    if (error != nullptr && error_length > 0U) {
        error[0] = '\0';
    }
    if (input == nullptr || question == nullptr || question_length == 0U ||
        options == nullptr || max_options == 0U || option_count == nullptr) {
        return false;
    }

    char working[SSH_CHATTER_MAX_INPUT_LEN];
    snprintf(working, sizeof(working), "%s", input);

    char *saveptr = nullptr;
    char *token = strtok_r(working, "|", &saveptr);
    if (token == nullptr) {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length,
                     "Provide a question and at least two options separated "
                     "by '|'.");
        }
        return false;
    }

    trim_whitespace_inplace(token);
    if (token[0] == '\0') {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length,
                     "Provide a question and at least two options separated "
                     "by '|'.");
        }
        return false;
    }
    snprintf(question, question_length, "%s", token);

    size_t count = 0U;
    while ((token = strtok_r(nullptr, "|", &saveptr)) != nullptr) {
        if (count >= max_options) {
            if (error != nullptr && error_length > 0U) {
                snprintf(error, error_length,
                         "You may only provide up to %zu options.",
                         max_options);
            }
            return false;
        }
        trim_whitespace_inplace(token);
        if (token[0] == '\0') {
            if (error != nullptr && error_length > 0U) {
                snprintf(error, error_length,
                         "Poll options may not be empty.");
            }
            return false;
        }
        snprintf(options[count], SSH_CHATTER_MESSAGE_LIMIT, "%s", token);
        ++count;
    }

    if (count < 2U) {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length,
                     "Provide at least two options separated by '|'.");
        }
        return false;
    }

    *option_count = count;
    return true;
}

static void session_handle_poll(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    static const char *kUsage =
        "Usage: /poll [close|<question>|<option1>|<option2>|<option3>|...]";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/poll", kUsage, usage, sizeof(usage));

    char working[SSH_CHATTER_MAX_INPUT_LEN];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
    }
    trim_whitespace_inplace(working);

    if (working[0] == '\0' || strcasecmp(working, "list") == 0 ||
        strcasecmp(working, "status") == 0) {
        session_send_poll_summary(ctx);
        return;
    }

    if (strcasecmp(working, "close") == 0 || strcasecmp(working, "end") == 0 ||
        strcasecmp(working, "stop") == 0 || strcasecmp(working, "off") == 0) {
        if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
            session_send_system_line(
                ctx, "Only operators may close global polls.");
            return;
        }

        bool was_active = false;
        ttak_mutex_lock(&ctx->owner->lock);
        if (ctx->owner->poll.active) {
            ctx->owner->poll.active = false;
            was_active = true;
            host_vote_state_save_locked(ctx->owner);
        }
        ttak_mutex_unlock(&ctx->owner->lock);

        if (!was_active) {
            session_send_system_line(ctx, "No active poll to close.");
            return;
        }

        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice), "* [%s] closed the main poll.",
                 ctx->user.name);
        host_history_record_system(ctx->owner, notice, nullptr);
        chat_room_broadcast(&ctx->owner->room, notice, nullptr);
        session_send_system_line(ctx, "Poll closed.");
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(ctx,
                                 "Only operators may start global polls.");
        return;
    }

    char question[SSH_CHATTER_MESSAGE_LIMIT];
    char options[5][SSH_CHATTER_MESSAGE_LIMIT];
    size_t option_count = 0U;
    char error[128];
    if (!session_poll_parse_fields(working, question, sizeof(question), options,
                                   sizeof(options) / sizeof(options[0]),
                                   &option_count, error, sizeof(error))) {
        if (error[0] != '\0') {
            session_send_system_line(ctx, error);
        } else {
            session_send_system_line(ctx, usage);
        }
        return;
    }

    poll_state_t snapshot = {0};
    ttak_mutex_lock(&ctx->owner->lock);
    uint64_t next_id = ctx->owner->poll.id + 1U;
    poll_state_reset(&ctx->owner->poll);
    ctx->owner->poll.active = true;
    ctx->owner->poll.allow_multiple = false;
    ctx->owner->poll.id = next_id == 0U ? 1U : next_id;
    ctx->owner->poll.option_count = option_count;
    snprintf(ctx->owner->poll.question, sizeof(ctx->owner->poll.question),
             "%s", question);
    for (size_t idx = 0U; idx < option_count; ++idx) {
        snprintf(ctx->owner->poll.options[idx].text,
                 sizeof(ctx->owner->poll.options[idx].text), "%s",
                 options[idx]);
        ctx->owner->poll.options[idx].votes = 0U;
    }
    host_vote_state_save_locked(ctx->owner);
    snapshot = ctx->owner->poll;
    ttak_mutex_unlock(&ctx->owner->lock);

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice), "* [%s] started a poll: %s",
             ctx->user.name, question);
    host_history_record_system(ctx->owner, notice, nullptr);
    chat_room_broadcast(&ctx->owner->room, notice, nullptr);
    session_send_poll_summary_generic(ctx, &snapshot, nullptr);
}

static void session_handle_vote(session_ctx_t *ctx, size_t option_index)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    char response[SSH_CHATTER_MESSAGE_LIMIT];
    response[0] = '\0';

    ttak_mutex_lock(&ctx->owner->lock);
    poll_state_t *poll = &ctx->owner->poll;
    if (!poll->active || poll->option_count == 0U) {
        ttak_mutex_unlock(&ctx->owner->lock);
        session_send_system_line(ctx, "No active poll right now.");
        return;
    }

    if (option_index >= poll->option_count) {
        ttak_mutex_unlock(&ctx->owner->lock);
        session_send_system_line(ctx, "That poll option does not exist.");
        return;
    }

    user_preference_t *pref =
        host_ensure_preference_locked(ctx->owner, ctx->user.name,
                                      ctx->client_ip);
    if (pref == nullptr) {
        ttak_mutex_unlock(&ctx->owner->lock);
        session_send_system_line(ctx, "Unable to record your vote.");
        return;
    }

    if (pref->last_poll_id != poll->id) {
        pref->last_poll_id = poll->id;
        pref->last_poll_choice = -1;
    }

    if (poll->allow_multiple) {
        uint32_t mask =
            pref->last_poll_choice < 0 ? 0U : (uint32_t)pref->last_poll_choice;
        uint32_t bit = 1U << option_index;
        if ((mask & bit) != 0U) {
            mask &= ~bit;
            if (poll->options[option_index].votes > 0U) {
                poll->options[option_index].votes--;
            }
            snprintf(response, sizeof(response),
                     "Removed your vote for option %zu.", option_index + 1U);
        } else {
            mask |= bit;
            poll->options[option_index].votes++;
            snprintf(response, sizeof(response),
                     "Vote recorded for option %zu.", option_index + 1U);
        }
        pref->last_poll_choice =
            mask == 0U ? -1 : (int32_t)mask;
    } else {
        int32_t previous = pref->last_poll_choice;
        if (previous == (int32_t)option_index) {
            ttak_mutex_unlock(&ctx->owner->lock);
            session_send_system_line(ctx,
                                     "You have already voted for that option.");
            return;
        }
        if (previous >= 0 && (size_t)previous < poll->option_count &&
            poll->options[previous].votes > 0U) {
            poll->options[previous].votes--;
        }
        poll->options[option_index].votes++;
        pref->last_poll_choice = (int32_t)option_index;
        snprintf(response, sizeof(response), "Vote recorded for option %zu.",
                 option_index + 1U);
    }

    pref->last_poll_id = poll->id;
    host_vote_state_save_locked(ctx->owner);
    host_state_save_locked(ctx->owner);
    ttak_mutex_unlock(&ctx->owner->lock);

    if (response[0] != '\0') {
        session_send_system_line(ctx, response);
    }
}

// Record a vote in a named poll, ensuring a user can move their vote between options.
static void session_handle_named_vote(session_ctx_t *ctx, size_t option_index,
                                      const char *label)
{
    if (ctx == nullptr || ctx->owner == nullptr || label == nullptr ||
        label[0] == '\0') {
        return;
    }

    char normalized_label[SSH_CHATTER_POLL_LABEL_LEN];
    snprintf(normalized_label, sizeof(normalized_label), "%s", label);
    trim_whitespace_inplace(normalized_label);
    if (!poll_label_is_valid(normalized_label)) {
        session_send_system_line(
            ctx, "Poll labels may only contain letters, numbers, '-' or '_'.");
        return;
    }

    char response[SSH_CHATTER_MESSAGE_LIMIT];
    response[0] = '\0';

    ttak_mutex_lock(&ctx->owner->lock);
    named_poll_state_t *poll =
        host_find_named_poll_locked(ctx->owner, normalized_label);
    if (poll == nullptr || !poll->poll.active ||
        poll->poll.option_count == 0U) {
        ttak_mutex_unlock(&ctx->owner->lock);
        session_send_system_line(ctx, "That poll is not active.");
        return;
    }

    if (option_index >= poll->poll.option_count) {
        ttak_mutex_unlock(&ctx->owner->lock);
        session_send_system_line(ctx, "That poll option does not exist.");
        return;
    }

    int voter_index = -1;
    for (size_t idx = 0U; idx < poll->voter_count; ++idx) {
        if (strcasecmp(poll->voters[idx].username, ctx->user.name) == 0) {
            voter_index = (int)idx;
            break;
        }
    }

    if (poll->poll.allow_multiple) {
        uint32_t bit = 1U << option_index;
        if (voter_index >= 0) {
            uint32_t mask = poll->voters[voter_index].choices_mask;
            if ((mask & bit) != 0U) {
                mask &= ~bit;
                if (poll->poll.options[option_index].votes > 0U) {
                    poll->poll.options[option_index].votes--;
                }
                if (mask == 0U) {
                    size_t remove_index = (size_t)voter_index;
                    if (remove_index + 1U < poll->voter_count) {
                        memmove(&poll->voters[remove_index],
                                &poll->voters[remove_index + 1U],
                                (poll->voter_count - remove_index - 1U) *
                                    sizeof(poll->voters[0]));
                    }
                    poll->voter_count--;
                    if (poll->voter_count < SSH_CHATTER_MAX_NAMED_VOTERS) {
                        memset(&poll->voters[poll->voter_count], 0,
                               sizeof(poll->voters[0]));
                    }
                } else {
                    poll->voters[voter_index].choices_mask = mask;
                    poll->voters[voter_index].choice = (int)option_index;
                }
                snprintf(response, sizeof(response),
                         "Removed your vote for option %zu.",
                         option_index + 1U);
            } else {
                mask |= bit;
                poll->poll.options[option_index].votes++;
                poll->voters[voter_index].choices_mask = mask;
                poll->voters[voter_index].choice = (int)option_index;
                snprintf(response, sizeof(response),
                         "Vote recorded for option %zu.", option_index + 1U);
            }
        } else {
            if (poll->voter_count >= SSH_CHATTER_MAX_NAMED_VOTERS) {
                ttak_mutex_unlock(&ctx->owner->lock);
                session_send_system_line(ctx,
                                         "That poll has reached "
                                         "its voter limit.");
                return;
            }
            poll->poll.options[option_index].votes++;
            named_poll_state_t *target = poll;
            size_t insert_at = target->voter_count++;
            snprintf(target->voters[insert_at].username,
                     sizeof(target->voters[insert_at].username), "%s",
                     ctx->user.name);
            target->voters[insert_at].choice = (int)option_index;
            target->voters[insert_at].choices_mask = bit;
            snprintf(response, sizeof(response),
                     "Vote recorded for option %zu.", option_index + 1U);
        }
    } else {
        if (voter_index >= 0) {
            int previous = poll->voters[voter_index].choice;
            if (previous == (int)option_index) {
                ttak_mutex_unlock(&ctx->owner->lock);
                session_send_system_line(
                    ctx, "You have already voted for that option.");
                return;
            }
            if (previous >= 0 &&
                (size_t)previous < poll->poll.option_count &&
                poll->poll.options[previous].votes > 0U) {
                poll->poll.options[previous].votes--;
            }
            poll->poll.options[option_index].votes++;
            poll->voters[voter_index].choice = (int)option_index;
            poll->voters[voter_index].choices_mask = 0U;
            snprintf(response, sizeof(response),
                     "Vote recorded for option %zu.", option_index + 1U);
        } else {
            if (poll->voter_count >= SSH_CHATTER_MAX_NAMED_VOTERS) {
                ttak_mutex_unlock(&ctx->owner->lock);
                session_send_system_line(ctx,
                                         "That poll has reached "
                                         "its voter limit.");
                return;
            }
            poll->poll.options[option_index].votes++;
            size_t insert_at = poll->voter_count++;
            snprintf(poll->voters[insert_at].username,
                     sizeof(poll->voters[insert_at].username), "%s",
                     ctx->user.name);
            poll->voters[insert_at].choice = (int)option_index;
            poll->voters[insert_at].choices_mask = 0U;
            snprintf(response, sizeof(response),
                     "Vote recorded for option %zu.", option_index + 1U);
        }
    }

    host_vote_state_save_locked(ctx->owner);
    ttak_mutex_unlock(&ctx->owner->lock);

    if (response[0] != '\0') {
        session_send_system_line(ctx, response);
    }
}

// Allow voting in a named poll by specifying the label and desired choice directly.
static void session_handle_elect_command(session_ctx_t *ctx,
                                         const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    static const char *kUsage = "Usage: /elect <label> <choice>";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/elect", kUsage, usage, sizeof(usage));

    char working[SSH_CHATTER_MAX_INPUT_LEN];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
    }
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    char *saveptr = nullptr;
    char *label = strtok_r(working, " \t", &saveptr);
    char *choice_text = strtok_r(nullptr, " \t", &saveptr);
    if (label == nullptr || choice_text == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    trim_whitespace_inplace(label);
    if (!poll_label_is_valid(label)) {
        session_send_system_line(
            ctx, "Poll labels may only contain letters, numbers, '-' or '_'.");
        return;
    }

    char *endptr = nullptr;
    unsigned long choice = strtoul(choice_text, &endptr, 10);
    if (endptr == choice_text || choice == 0U) {
        session_send_system_line(ctx, usage);
        return;
    }

    session_handle_named_vote(ctx, (size_t)(choice - 1U), label);
}

// Parse the /vote command to manage named polls, including listing, creation, and closure.
static void session_handle_vote_command(session_ctx_t *ctx,
                                        const char *arguments,
                                        bool allow_multiple)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    const char *canonical = allow_multiple ? "/vote" : "/vote-single";
    const char *kUsage =
        "Usage: /vote <label> [close|<question>|<option1>|<option2>|...]";
    const char *kUsageSingle =
        "Usage: /vote-single <label> [close|<question>|<option1>|<option2>|...]";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, canonical,
                                 allow_multiple ? kUsage : kUsageSingle, usage,
                                 sizeof(usage));

    char working[SSH_CHATTER_MAX_INPUT_LEN];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
    }
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_list_named_polls(ctx);
        return;
    }

    char *saveptr = nullptr;
    char *label = strtok_r(working, " \t", &saveptr);
    if (label == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    if (strcasecmp(label, "list") == 0) {
        session_list_named_polls(ctx);
        return;
    }

    trim_whitespace_inplace(label);
    if (!poll_label_is_valid(label)) {
        session_send_system_line(
            ctx, "Poll labels may only contain letters, numbers, '-' or '_'.");
        return;
    }

    char remainder[SSH_CHATTER_MAX_INPUT_LEN];
    if (saveptr == nullptr) {
        remainder[0] = '\0';
    } else {
        snprintf(remainder, sizeof(remainder), "%s", saveptr);
    }
    trim_whitespace_inplace(remainder);

    if (remainder[0] == '\0') {
        named_poll_state_t snapshot = {0};
        bool found = false;
        ttak_mutex_lock(&ctx->owner->lock);
        named_poll_state_t *poll =
            host_find_named_poll_locked(ctx->owner, label);
        if (poll != nullptr) {
            snapshot = *poll;
            found = true;
        }
        ttak_mutex_unlock(&ctx->owner->lock);

        if (!found) {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message), "No poll found for label '%s'.",
                     label);
            session_send_system_line(ctx, message);
            return;
        }

        session_send_poll_summary_generic(ctx, &snapshot.poll, snapshot.label);
        return;
    }

    if (strcasecmp(remainder, "close") == 0 ||
        strcasecmp(remainder, "end") == 0 ||
        strcasecmp(remainder, "stop") == 0 ||
        strcasecmp(remainder, "off") == 0) {
        bool closed = false;
        bool allowed = false;
        bool found = false;
        ttak_mutex_lock(&ctx->owner->lock);
        named_poll_state_t *poll =
            host_find_named_poll_locked(ctx->owner, label);
        if (poll != nullptr) {
            found = true;
            allowed = ctx->user.is_operator || ctx->user.is_lan_operator ||
                      strcasecmp(poll->owner, ctx->user.name) == 0;
            if (allowed && poll->poll.active) {
                poll->poll.active = false;
                closed = true;
                host_recount_named_polls_locked(ctx->owner);
                host_vote_state_save_locked(ctx->owner);
            }
        }
        ttak_mutex_unlock(&ctx->owner->lock);

        if (!found) {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message), "No poll found for label '%s'.",
                     label);
            session_send_system_line(ctx, message);
            return;
        }

        if (!allowed) {
            session_send_system_line(
                ctx,
                "Only the poll owner or an operator may close this poll.");
            return;
        }

        if (!closed) {
            session_send_system_line(ctx, "That poll is not active.");
            return;
        }

        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice), "* [%s] closed poll [%s].",
                 ctx->user.name, label);
        host_history_record_system(ctx->owner, notice, nullptr);
        chat_room_broadcast(&ctx->owner->room, notice, nullptr);
        session_send_system_line(ctx, "Poll closed.");
        return;
    }

    char question[SSH_CHATTER_MESSAGE_LIMIT];
    char options[5][SSH_CHATTER_MESSAGE_LIMIT];
    size_t option_count = 0U;
    char error[128];
    if (!session_poll_parse_fields(remainder, question, sizeof(question),
                                   options,
                                   sizeof(options) / sizeof(options[0]),
                                   &option_count, error, sizeof(error))) {
        if (error[0] != '\0') {
            session_send_system_line(ctx, error);
        } else {
            session_send_system_line(ctx, usage);
        }
        return;
    }

    named_poll_state_t snapshot = {0};
    bool created = false;
    bool allowed = true;
    ttak_mutex_lock(&ctx->owner->lock);
    named_poll_state_t *poll =
        host_ensure_named_poll_locked(ctx->owner, label);
    if (poll == nullptr) {
        allowed = false;
    } else if (poll->poll.active &&
               !(ctx->user.is_operator || ctx->user.is_lan_operator ||
                 strcasecmp(poll->owner, ctx->user.name) == 0)) {
        allowed = false;
    } else {
        uint64_t next_id = poll->poll.id + 1U;
        char saved_label[SSH_CHATTER_POLL_LABEL_LEN];
        snprintf(saved_label, sizeof(saved_label), "%s", label);
        named_poll_reset(poll);
        snprintf(poll->label, sizeof(poll->label), "%s", saved_label);
        snprintf(poll->owner, sizeof(poll->owner), "%s", ctx->user.name);
        poll->poll.active = true;
        poll->poll.allow_multiple = allow_multiple;
        poll->poll.id = next_id == 0U ? 1U : next_id;
        poll->poll.option_count = option_count;
        snprintf(poll->poll.question, sizeof(poll->poll.question), "%s",
                 question);
        for (size_t idx = 0U; idx < option_count; ++idx) {
            snprintf(poll->poll.options[idx].text,
                     sizeof(poll->poll.options[idx].text), "%s",
                     options[idx]);
            poll->poll.options[idx].votes = 0U;
        }
        poll->voter_count = 0U;
        host_recount_named_polls_locked(ctx->owner);
        host_vote_state_save_locked(ctx->owner);
        snapshot = *poll;
        created = true;
    }
    ttak_mutex_unlock(&ctx->owner->lock);

    if (!allowed) {
        session_send_system_line(ctx,
                                 "Unable to start that poll. Another active "
                                 "poll owns the label or the poll limit has "
                                 "been reached.");
        return;
    }

    if (created) {
        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice), "* [%s] started poll [%s]: %s",
                 ctx->user.name, label, question);
        host_history_record_system(ctx->owner, notice, nullptr);
        chat_room_broadcast(&ctx->owner->room, notice, nullptr);
        session_send_poll_summary_generic(ctx, &snapshot.poll, snapshot.label);
    }
}

static void __attribute__((unused))
session_handle_gameopt(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    static const char *kUsage = "Usage: /gameopt <reset>";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/gameopt", kUsage, usage, sizeof(usage));

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

    if (strcasecmp(working, "reset") == 0) {
        snprintf(ctx->game.chosen_camouflage_language,
                 sizeof(ctx->game.chosen_camouflage_language), "c");
        if (ctx->owner != nullptr) {
            ttak_mutex_lock(&ctx->owner->lock);
            user_preference_t *pref =
                host_ensure_preference_locked(ctx->owner, ctx->user.name, "");
            if (pref != nullptr) {
                snprintf(pref->camouflage_language,
                         sizeof(pref->camouflage_language), "c");
                host_state_save_locked(ctx->owner);
            }
            ttak_mutex_unlock(&ctx->owner->lock);
        }
        session_send_system_line(
            ctx, "Game options reset. Camouflage language set to default (C).");
        return;
    }

    session_send_system_line(ctx, usage);
}

static void session_handle_advanced(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    bool is_operator = ctx->user.is_operator || ctx->user.is_lan_operator;

    char delegated_buffer[SSH_CHATTER_MESSAGE_LIMIT];
    if (arguments != nullptr) {
        snprintf(delegated_buffer, sizeof(delegated_buffer), "%s", arguments);
        trim_whitespace_inplace(delegated_buffer);

        if (delegated_buffer[0] != '\0') {
            char token[64];
            const char *remaining =
                session_consume_token(delegated_buffer, token, sizeof(token));
            if (strcasecmp(token, "telnet-server") == 0) {
                char forwarded[SSH_CHATTER_MESSAGE_LIMIT];
                if (remaining != nullptr) {
                    snprintf(forwarded, sizeof(forwarded), "%s", remaining);
                    trim_whitespace_inplace(forwarded);
                } else {
                    forwarded[0] = '\0';
                }

                session_send_system_line(
                    ctx, "Tip: use /telnet-server directly "
                         "for Telnet/Fidonet integration controls.");

                return;
            }

            session_send_system_line(
                ctx,
                "Unknown advanced topic. Showing available commands instead.");
        }
    }

    const session_ui_locale_t *locale = session_ui_get_locale(ctx);
    const char *prefix = session_command_prefix(ctx);
    char help_buffer[SSH_CHATTER_MESSAGE_LIMIT * 32];

    if (locale != nullptr && locale->help_extra_title != nullptr &&
        locale->help_extra_title[0] != '\0') {
        session_send_system_line(ctx, locale->help_extra_title);
    }

    help_buffer[0] = '\0';
    session_format_help_entries_to_buffer(ctx, kSessionHelpExtended,
                                          sizeof(kSessionHelpExtended) /
                                              sizeof(kSessionHelpExtended[0]),
                                          help_buffer, sizeof(help_buffer));
    session_send_raw_text(ctx, help_buffer);

    if (locale != nullptr && locale->help_extra_hint != nullptr &&
        locale->help_extra_hint[0] != '\0') {
        const char *args[] = {prefix};
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        session_format_template(locale->help_extra_hint, args,
                                sizeof(args) / sizeof(args[0]), line,
                                sizeof(line));
        session_send_system_line(ctx, line);
    }

    if (is_operator) {
        if (locale != nullptr && locale->help_operator_title != nullptr &&
            locale->help_operator_title[0] != '\0') {
            session_send_system_line(ctx, locale->help_operator_title);
        }

        help_buffer[0] = '\0';
        session_format_help_entries_to_buffer(
            ctx, kSessionHelpOperator,
            sizeof(kSessionHelpOperator) / sizeof(kSessionHelpOperator[0]),
            help_buffer, sizeof(help_buffer));
        session_send_raw_text(ctx, help_buffer);

    } else {
        session_send_system_line(
            ctx,
            "Operator-only integrations are hidden. Request access if needed.");
    }
}

// Format a timestamp for BBS displays in a compact form.
static void bbs_format_time(time_t value, char *buffer, size_t length)
{
    if (buffer == nullptr || length == 0U) {
        return;
    }
    struct tm tm_value;
    if (localtime_r(&value, &tm_value) == nullptr) {
        snprintf(buffer, length, "-");
        return;
    }
    strftime(buffer, length, "%Y-%m-%d %H:%M", &tm_value);
}

// Return a post by identifier while the host lock is held.
static bbs_post_t *host_find_bbs_post_locked(host_t *host, uint64_t id)
{
    if (host == nullptr || id == 0U) {
        return nullptr;
    }
    for (size_t idx = 0U; idx < SSH_CHATTER_BBS_MAX_POSTS; ++idx) {
        if (!host->bbs_posts[idx].in_use) {
            continue;
        }
        if (host->bbs_posts[idx].id == id) {
            return &host->bbs_posts[idx];
        }
    }
    return nullptr;
}

// Allocate a new post slot, returning nullptr if capacity has been reached.
static bbs_post_t *host_allocate_bbs_post_locked(host_t *host)
{
    if (host == nullptr) {
        return nullptr;
    }
    for (size_t idx = 0U; idx < SSH_CHATTER_BBS_MAX_POSTS; ++idx) {
        if (host->bbs_posts[idx].in_use) {
            continue;
        }
        bbs_post_t *post = &host->bbs_posts[idx];
        post->in_use = true;
        post->id = host->next_bbs_id++;
        post->tag_count = 0U;
        post->comment_count = 0U;
        post->created_at = time(nullptr);
        post->bumped_at = post->created_at;
        post->title[0] = '\0';
        post->body[0] = '\0';
        post->author[0] = '\0';
        for (size_t tag = 0U; tag < SSH_CHATTER_BBS_MAX_TAGS; ++tag) {
            post->tags[tag][0] = '\0';
        }
        for (size_t comment = 0U; comment < SSH_CHATTER_BBS_MAX_COMMENTS;
             ++comment) {
            post->comments[comment].author[0] = '\0';
            post->comments[comment].text[0] = '\0';
            post->comments[comment].created_at = 0;
        }
        if (host->bbs_post_count < SSH_CHATTER_BBS_MAX_POSTS) {
            host->bbs_post_count += 1U;
        }
        return post;
    }
    return nullptr;
}

static void host_reset_bbs_post(bbs_post_t *post)
{
    if (post == nullptr) {
        return;
    }

    post->in_use = false;
    post->id = 0U;
    post->author[0] = '\0';
    post->title[0] = '\0';
    post->body[0] = '\0';
    post->tag_count = 0U;
    post->created_at = 0;
    post->bumped_at = 0;
    post->comment_count = 0U;
    for (size_t tag = 0U; tag < SSH_CHATTER_BBS_MAX_TAGS; ++tag) {
        post->tags[tag][0] = '\0';
    }
    for (size_t comment = 0U; comment < SSH_CHATTER_BBS_MAX_COMMENTS;
         ++comment) {
        post->comments[comment].author[0] = '\0';
        post->comments[comment].text[0] = '\0';
        post->comments[comment].created_at = 0;
    }
}

static void host_clear_bbs_post_locked(host_t *host, bbs_post_t *post)
{
    if (host == nullptr || post == nullptr) {
        return;
    }

    host_reset_bbs_post(post);

    size_t write_index = 0U;
    for (size_t idx = 0U; idx < SSH_CHATTER_BBS_MAX_POSTS; ++idx) {
        if (!host->bbs_posts[idx].in_use) {
            continue;
        }

        if (write_index != idx) {
            host->bbs_posts[write_index] = host->bbs_posts[idx];
        }

        ++write_index;
    }

    for (size_t idx = write_index; idx < SSH_CHATTER_BBS_MAX_POSTS; ++idx) {
        host_reset_bbs_post(&host->bbs_posts[idx]);
    }

    host->bbs_post_count = write_index;
}

// Render an ASCII framed view of a post, including metadata and comments.

static bool session_bbs_refresh_view(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr || !ctx->bbs_view_active ||
        ctx->bbs_view_post_id == 0U) {
        return false;
    }

    host_t *host = ctx->owner;
    ttak_mutex_lock(&host->lock);
    bbs_post_t *post = host_find_bbs_post_locked(host, ctx->bbs_view_post_id);
    bbs_post_t snapshot = {0};
    if (post != nullptr) {
        snapshot = *post;
    }
    ttak_mutex_unlock(&host->lock);

    if (post == nullptr || !snapshot.in_use) {
        ctx->bbs_view_active = false;
        ctx->bbs_view_post_id = 0U;
        ctx->bbs_view_total_lines = 0U;
        ctx->bbs_view_scroll_offset = 0U;
        session_send_system_line(ctx, "That post is no longer available.");
        return false;
    }

    session_bbs_render_post(ctx, &snapshot, nullptr, false);
    return true;
}

static bool session_bbs_scroll(session_ctx_t *ctx, int direction, size_t step)
{
    if (ctx == nullptr || ctx->owner == nullptr || !ctx->bbs_view_active ||
        direction == 0) {
        return false;
    }

    size_t window = SSH_CHATTER_BBS_VIEW_WINDOW;
    if (window == 0U) {
        window = 1U;
    }

    size_t total = ctx->bbs_view_total_lines;
    if (total <= window) {
        if (direction > 0) {
            session_send_system_line(ctx,
                                     "Already viewing the top of this post.");
        } else if (direction < 0) {
            session_send_system_line(ctx,
                                     "Already viewing the end of this post.");
        }
        return true;
    }

    size_t max_offset = total - window;
    size_t offset = ctx->bbs_view_scroll_offset;
    size_t effective_step = step;
    if (effective_step == 0U) {
        effective_step = window;
    }
    if (effective_step == 0U) {
        effective_step = 1U;
    }

    size_t new_offset = offset;
    if (direction > 0) {
        if (offset == 0U) {
            session_send_system_line(ctx,
                                     "Already viewing the top of this post.");
            return true;
        }
        if (effective_step > offset) {
            effective_step = offset;
        }
        if (effective_step == 0U) {
            effective_step = 1U;
        }
        new_offset = offset - effective_step;
    } else if (direction < 0) {
        if (offset >= max_offset) {
            session_send_system_line(ctx,
                                     "Already viewing the end of this post.");
            return true;
        }
        size_t advance = effective_step;
        if (advance > max_offset - offset) {
            advance = max_offset - offset;
        }
        if (advance == 0U) {
            advance = 1U;
        }
        new_offset = offset + advance;
    }

    if (new_offset == offset) {
        if (direction > 0) {
            session_send_system_line(ctx,
                                     "Already viewing the top of this post.");
        } else if (direction < 0) {
            session_send_system_line(ctx,
                                     "Already viewing the end of this post.");
        }
        return true;
    }

    ctx->bbs_view_scroll_offset = new_offset;
    return session_bbs_refresh_view(ctx);
}

// Show the BBS dashboard and mark the session as being in BBS mode.
static void session_bbs_show_dashboard(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    ctx->in_bbs_mode = true;
    ctx->bbs_view_active = false;
    ctx->bbs_view_post_id = 0U;
    session_bbs_prepare_canvas(ctx);
    session_render_separator(ctx, "BBS Dashboard");
    session_send_system_line(
        ctx, "Commands: list, read <id>, topic read <tag>, post <title> "
             "[tags...], comment <id>|<text>, regen <id>, delete <id>, exit");
    session_bbs_list(ctx);
}

// List posts sorted by most recent activity.
static void session_bbs_list(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    bool previous_override = session_translation_push_scope_override(ctx);
    typedef struct bbs_listing {
        uint64_t id;
        char title[SSH_CHATTER_BBS_TITLE_LEN];
        char author[SSH_CHATTER_USERNAME_LEN];
        char tags[SSH_CHATTER_BBS_MAX_TAGS][SSH_CHATTER_BBS_TAG_LEN];
        size_t tag_count;
        time_t created_at;
        time_t bumped_at;
    } bbs_listing_t;

    bbs_listing_t listings[SSH_CHATTER_BBS_MAX_POSTS];
    size_t count = 0U;

    host_t *host = ctx->owner;
    ttak_mutex_lock(&host->lock);
    for (size_t idx = 0U; idx < SSH_CHATTER_BBS_MAX_POSTS; ++idx) {
        const bbs_post_t *post = &host->bbs_posts[idx];
        if (!post->in_use) {
            continue;
        }
        listings[count].id = post->id;
        snprintf(listings[count].title, sizeof(listings[count].title), "%s",
                 post->title);
        snprintf(listings[count].author, sizeof(listings[count].author), "%s",
                 post->author);
        listings[count].tag_count = post->tag_count;
        for (size_t tag = 0U;
             tag < post->tag_count && tag < SSH_CHATTER_BBS_MAX_TAGS; ++tag) {
            snprintf(listings[count].tags[tag],
                     sizeof(listings[count].tags[tag]), "%s", post->tags[tag]);
        }
        listings[count].created_at = post->created_at;
        listings[count].bumped_at = post->bumped_at;
        ++count;
        if (count >= SSH_CHATTER_BBS_MAX_POSTS) {
            break;
        }
    }
    ttak_mutex_unlock(&host->lock);

    if (count == 0U) {
        char empty_hint[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(
            empty_hint, sizeof(empty_hint),
            "The bulletin board is empty. Use /bbs post <title> [tags...] to "
            "write something. Finish drafts with %s.",
            session_bbs_terminator(ctx));
        session_send_system_line(ctx, empty_hint);
        session_translation_pop_scope_override(ctx, previous_override);
        return;
    }

    for (size_t outer = 1U; outer < count; ++outer) {
        bbs_listing_t key = listings[outer];
        size_t position = outer;
        while (position > 0U &&
               listings[position - 1U].bumped_at < key.bumped_at) {
            listings[position] = listings[position - 1U];
            --position;
        }
        listings[position] = key;
    }

    ctx->bbs_view_active = false;
    ctx->bbs_view_post_id = 0U;

    typedef struct bbs_topic_group {
        char name[SSH_CHATTER_BBS_TAG_LEN];
        size_t indexes[SSH_CHATTER_BBS_MAX_POSTS];
        size_t count;
    } bbs_topic_group_t;

    bbs_topic_group_t topics[SSH_CHATTER_BBS_MAX_POSTS];
    size_t topic_count = 0U;
    memset(topics, 0, sizeof(topics));

    for (size_t idx = 0U; idx < count; ++idx) {
        const char *topic_name = (listings[idx].tag_count > 0U)
                                     ? listings[idx].tags[0]
                                     : SSH_CHATTER_BBS_DEFAULT_TAG;
        size_t match = topic_count;
        for (size_t topic_idx = 0U; topic_idx < topic_count; ++topic_idx) {
            if (strcasecmp(topics[topic_idx].name, topic_name) == 0) {
                match = topic_idx;
                break;
            }
        }
        if (match == topic_count) {
            if (topic_count >= SSH_CHATTER_BBS_MAX_POSTS) {
                continue;
            }
            snprintf(topics[match].name, sizeof(topics[match].name), "%s",
                     topic_name);
            topics[match].count = 0U;
            ++topic_count;
        }
        if (topics[match].count < SSH_CHATTER_BBS_MAX_POSTS) {
            topics[match].indexes[topics[match].count++] = idx;
        }
    }

    for (size_t outer = 1U; outer < topic_count; ++outer) {
        bbs_topic_group_t key = topics[outer];
        size_t position = outer;
        while (position > 0U &&
               strcasecmp(topics[position - 1U].name, key.name) > 0) {
            topics[position] = topics[position - 1U];
            --position;
        }
        topics[position] = key;
    }

    session_render_separator(ctx, "BBS Posts by Topic");
    for (size_t topic_idx = 0U; topic_idx < topic_count; ++topic_idx) {
        char section_label[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(section_label, sizeof(section_label), "Topic: %s",
                 topics[topic_idx].name);
        session_render_separator(ctx, section_label);

        for (size_t entry_idx = 0U; entry_idx < topics[topic_idx].count;
             ++entry_idx) {
            size_t listing_index = topics[topic_idx].indexes[entry_idx];
            const bbs_listing_t *entry = &listings[listing_index];
            char created_buffer[32];
            bbs_format_time(entry->bumped_at, created_buffer,
                            sizeof(created_buffer));
            char line[SSH_CHATTER_MESSAGE_LIMIT];
            int title_preview =
                (int)strnlen(entry->title, sizeof(entry->title));
            if (title_preview > 80) {
                title_preview = 80;
            }
            if (entry->tag_count == 0U) {
                snprintf(line, sizeof(line), "#%" PRIu64 " [%s] %.*s|(no tags)",
                         entry->id, created_buffer, title_preview,
                         entry->title);
            } else {
                char tag_buffer[SSH_CHATTER_MESSAGE_LIMIT];
                size_t buffer_offset = 0U;
                tag_buffer[0] = '\0';
                for (size_t tag = 0U; tag < entry->tag_count; ++tag) {
                    size_t len = strlen(entry->tags[tag]);
                    if (buffer_offset + len + 2U >= sizeof(tag_buffer)) {
                        break;
                    }
                    if (tag > 0U) {
                        tag_buffer[buffer_offset++] = ',';
                    }
                    memcpy(tag_buffer + buffer_offset, entry->tags[tag], len);
                    buffer_offset += len;
                    tag_buffer[buffer_offset] = '\0';
                }
                int tags_preview = (int)strnlen(tag_buffer, sizeof(tag_buffer));
                if (tags_preview > 80) {
                    tags_preview = 80;
                }
                snprintf(line, sizeof(line), "#%" PRIu64 " [%s] %.*s|%.*s",
                         entry->id, created_buffer, title_preview, entry->title,
                         tags_preview, tag_buffer);
            }
            session_send_system_line(ctx, line);
        }
    }

    session_render_separator(ctx, "End");
    session_translation_pop_scope_override(ctx, previous_override);
}

static void session_bbs_list_topic(session_ctx_t *ctx, const char *topic)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    char working_topic[SSH_CHATTER_BBS_TAG_LEN];
    if (topic != nullptr) {
        snprintf(working_topic, sizeof(working_topic), "%s", topic);
    } else {
        working_topic[0] = '\0';
    }
    trim_whitespace_inplace(working_topic);

    if (working_topic[0] == '\0') {
        session_send_system_line(ctx, "Specify a topic to read.");
        return;
    }

    bool previous_override = session_translation_push_scope_override(ctx);

    typedef struct bbs_listing {
        uint64_t id;
        char title[SSH_CHATTER_BBS_TITLE_LEN];
        char author[SSH_CHATTER_USERNAME_LEN];
        char tags[SSH_CHATTER_BBS_MAX_TAGS][SSH_CHATTER_BBS_TAG_LEN];
        size_t tag_count;
        time_t created_at;
        time_t bumped_at;
    } bbs_listing_t;

    bbs_listing_t listings[SSH_CHATTER_BBS_MAX_POSTS];
    size_t count = 0U;

    host_t *host = ctx->owner;
    ttak_mutex_lock(&host->lock);
    for (size_t idx = 0U; idx < SSH_CHATTER_BBS_MAX_POSTS; ++idx) {
        const bbs_post_t *post = &host->bbs_posts[idx];
        if (!post->in_use) {
            continue;
        }
        listings[count].id = post->id;
        snprintf(listings[count].title, sizeof(listings[count].title), "%s",
                 post->title);
        snprintf(listings[count].author, sizeof(listings[count].author), "%s",
                 post->author);
        listings[count].tag_count = post->tag_count;
        for (size_t tag_idx = 0U;
             tag_idx < post->tag_count && tag_idx < SSH_CHATTER_BBS_MAX_TAGS;
             ++tag_idx) {
            snprintf(listings[count].tags[tag_idx],
                     sizeof(listings[count].tags[tag_idx]), "%s",
                     post->tags[tag_idx]);
        }
        listings[count].created_at = post->created_at;
        listings[count].bumped_at = post->bumped_at;
        ++count;
        if (count >= SSH_CHATTER_BBS_MAX_POSTS) {
            break;
        }
    }
    ttak_mutex_unlock(&host->lock);

    if (count == 0U) {
        session_send_system_line(ctx, "The bulletin board is empty.");
        session_translation_pop_scope_override(ctx, previous_override);
        return;
    }

    for (size_t outer = 1U; outer < count; ++outer) {
        bbs_listing_t key = listings[outer];
        size_t position = outer;
        while (position > 0U &&
               listings[position - 1U].bumped_at < key.bumped_at) {
            listings[position] = listings[position - 1U];
            --position;
        }
        listings[position] = key;
    }

    ctx->bbs_view_active = false;
    ctx->bbs_view_post_id = 0U;

    char section_label[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(section_label, sizeof(section_label), "BBS Topic: %s",
             working_topic);
    session_render_separator(ctx, section_label);

    bool found = false;
    for (size_t idx = 0U; idx < count; ++idx) {
        const bbs_listing_t *entry = &listings[idx];
        const char *entry_topic = (entry->tag_count > 0U)
                                      ? entry->tags[0]
                                      : SSH_CHATTER_BBS_DEFAULT_TAG;
        if (strcasecmp(entry_topic, working_topic) != 0) {
            continue;
        }

        char created_buffer[32];
        bbs_format_time(entry->bumped_at, created_buffer,
                        sizeof(created_buffer));

        char line[SSH_CHATTER_MESSAGE_LIMIT];
        int title_preview = (int)strnlen(entry->title, sizeof(entry->title));
        if (title_preview > 80) {
            title_preview = 80;
        }

        if (entry->tag_count <= 1U) {
            snprintf(line, sizeof(line), "#%" PRIu64 " [%s] %.*s", entry->id,
                     created_buffer, title_preview, entry->title);
        } else {
            char tag_buffer[SSH_CHATTER_MESSAGE_LIMIT];
            size_t buffer_offset = 0U;
            tag_buffer[0] = '\0';
            for (size_t tag_idx = 0U; tag_idx < entry->tag_count; ++tag_idx) {
                const char *tag_value = entry->tags[tag_idx];
                if (tag_value[0] == '\0') {
                    continue;
                }
                size_t len = strlen(tag_value);
                if (buffer_offset + len + 2U >= sizeof(tag_buffer)) {
                    break;
                }
                if (buffer_offset > 0U) {
                    tag_buffer[buffer_offset++] = ',';
                }
                memcpy(tag_buffer + buffer_offset, tag_value, len);
                buffer_offset += len;
                tag_buffer[buffer_offset] = '\0';
            }
            int tags_preview = (int)strnlen(tag_buffer, sizeof(tag_buffer));
            if (tags_preview > 80) {
                tags_preview = 80;
            }
            snprintf(line, sizeof(line), "#%" PRIu64 " [%s] %.*s|%.*s",
                     entry->id, created_buffer, title_preview, entry->title,
                     tags_preview, tag_buffer);
        }

        session_send_system_line(ctx, line);
        found = true;
    }

    if (!found) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "No posts found for topic '%s'.",
                 working_topic);
        session_send_system_line(ctx, message);
    }

    session_render_separator(ctx, "End");
    session_translation_pop_scope_override(ctx, previous_override);
}

// Display a single post to the user.
static void session_bbs_read(session_ctx_t *ctx, uint64_t id)
{
    if (ctx == nullptr || ctx->owner == nullptr || id == 0U) {
        return;
    }

    host_t *host = ctx->owner;
    ttak_mutex_lock(&host->lock);
    bbs_post_t *post = host_find_bbs_post_locked(host, id);
    bbs_post_t snapshot = {0};
    if (post != nullptr) {
        snapshot = *post;
    }
    ttak_mutex_unlock(&host->lock);

    if (post == nullptr || !snapshot.in_use) {
        session_send_system_line(ctx, "No post exists with that identifier.");
        return;
    }

    session_bbs_render_post(ctx, &snapshot, nullptr, true);
}

// Create a new post using the provided argument format.
static bool session_bbs_is_admin_only_tag(const char *tag)
{
    if (tag == nullptr || tag[0] == '\0') {
        return false;
    }

    if (strcasecmp(tag, "manual") == 0 || strcasecmp(tag, "notice") == 0) {
        return true;
    }

    if (strcmp(tag, "설명서") == 0 || strcmp(tag, "공지") == 0) {
        return true;
    }

    return false;
}

static void session_bbs_compact_preview(const char *input, char *output,
                                        size_t length)
{
    if (output == nullptr || length == 0U) {
        return;
    }
    output[0] = '\0';
    if (input == nullptr) {
        return;
    }

    size_t out_idx = 0U;
    bool last_space = true;
    bool truncated = false;
    const unsigned char *cursor = (const unsigned char *)input;

    while (*cursor != '\0') {
        unsigned char ch = *cursor++;
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            ch = ' ';
        }
        if (ch < 32U) {
            continue;
        }
        if (ch == ' ') {
            if (last_space) {
                continue;
            }
            last_space = true;
        } else {
            last_space = false;
        }

        if (out_idx + 1U >= length) {
            truncated = true;
            break;
        }

        output[out_idx++] = (char)ch;
    }

    if (last_space && out_idx > 0U) {
        --out_idx;
    }

    if (truncated && out_idx + 3U < length) {
        output[out_idx++] = '.';
        output[out_idx++] = '.';
        output[out_idx++] = '.';
    }

    output[out_idx] = '\0';
}

static void session_bbs_announce_post(host_t *host, const bbs_post_t *post)
{
    if (host == nullptr || post == nullptr) {
        return;
    }

    char author[SSH_CHATTER_USERNAME_LEN];
    snprintf(author, sizeof(author), "%s", post->author);
    trim_whitespace_inplace(author);

    char title[SSH_CHATTER_BBS_TITLE_LEN];
    snprintf(title, sizeof(title), "%s", post->title);
    trim_whitespace_inplace(title);

    char preview[128];
    session_bbs_compact_preview(post->body, preview, sizeof(preview));

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    if (preview[0] != '\0') {
        snprintf(notice, sizeof(notice), "* [bbs] #%llu %s posted \"%s\" --%s",
                 (unsigned long long)post->id,
                 author[0] != '\0' ? author : "unknown",
                 title[0] != '\0' ? title : "(untitled)", preview);
    } else {
        snprintf(notice, sizeof(notice), "* [bbs] #%llu %s posted \"%s\"",
                 (unsigned long long)post->id,
                 author[0] != '\0' ? author : "unknown",
                 title[0] != '\0' ? title : "(untitled)");
    }

    host_history_record_system(host, notice, nullptr);
}

static void session_bbs_announce_comment(host_t *host, const bbs_post_t *post,
                                         const bbs_comment_t *comment)
{
    if (host == nullptr || post == nullptr || comment == nullptr) {
        return;
    }

    char author[SSH_CHATTER_USERNAME_LEN];
    snprintf(author, sizeof(author), "%s", comment->author);
    trim_whitespace_inplace(author);

    char title[SSH_CHATTER_BBS_TITLE_LEN];
    snprintf(title, sizeof(title), "%s", post->title);
    trim_whitespace_inplace(title);

    char preview[128];
    session_bbs_compact_preview(comment->text, preview, sizeof(preview));

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    if (preview[0] != '\0') {
        snprintf(notice, sizeof(notice),
                 "* [bbs] #%llu %s commented on \"%s\": %s",
                 (unsigned long long)post->id,
                 author[0] != '\0' ? author : "unknown",
                 title[0] != '\0' ? title : "(untitled)", preview);
    } else {
        snprintf(notice, sizeof(notice), "* [bbs] #%llu %s commented on \"%s\"",
                 (unsigned long long)post->id,
                 author[0] != '\0' ? author : "unknown",
                 title[0] != '\0' ? title : "(untitled)");
    }

    host_history_record_system(host, notice, nullptr);
}

static void session_bbs_reset_pending_post(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    ctx->bbs_post_pending = false;
    ctx->editor_mode = SESSION_EDITOR_MODE_NONE;
    ctx->pending_bbs_edit_id = 0U;
    ctx->pending_bbs_body_length = 0U;
    ctx->pending_bbs_tag_count = 0U;
    ctx->pending_bbs_line_count = 0U;
    ctx->pending_bbs_cursor_line = 0U;
    ctx->pending_bbs_editing_line = false;
    ctx->bbs_editor_scroll_offset = 0U;
    ctx->bbs_editor_selection_start = 0U;
    ctx->bbs_editor_selection_start_set = false;
    ctx->bbs_editor_selection_end = 0U;
    ctx->bbs_editor_selection_end_set = false;
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
    if (ctx->bbs_editor_clipboard != nullptr) {
        ctx->bbs_editor_clipboard[0] = '\0';
    }
    ctx->bbs_editor_clipboard_length = 0U;
    ctx->bbs_editor_clipboard_lines = 0U;
    ctx->bbs_line_edit_mode = false;
    ctx->bbs_line_edit_target = 0U;
    ctx->bbs_search_active = false;
    ctx->bbs_search_restore_line = 0U;
    ctx->bbs_search_restore_editing = false;
    ctx->bbs_search_restore_scroll = 0U;
    ctx->bbs_rendering_editor = false;
}

static void session_bbs_commit_pending_post(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    if (!ctx->bbs_post_pending) {
        return;
    }

    if (ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART) {
        session_asciiart_import_from_editor(ctx);
        if (ctx->asciiart_length == 0U) {
            session_asciiart_cancel(ctx, "ASCII art draft discarded.");
            session_bbs_reset_pending_post(ctx);
            return;
        }

        session_asciiart_commit(ctx);
        session_bbs_reset_pending_post(ctx);
        return;
    }

    if (ctx->pending_bbs_body_length == 0U) {
        session_send_system_line(ctx, "Post body was empty. Draft discarded.");
        session_bbs_reset_pending_post(ctx);
        return;
    }

    if (session_security_check_text(ctx, "BBS post", ctx->pending_bbs_body,
                                    ctx->pending_bbs_body_length,
                                    false) != HOST_SECURITY_SCAN_CLEAN) {
        session_bbs_reset_pending_post(ctx);
        return;
    }

    host_t *host = ctx->owner;
    if (host == nullptr) {
        session_bbs_reset_pending_post(ctx);
        return;
    }

    ttak_mutex_lock(&host->lock);
    bbs_post_t snapshot = {0};
    if (ctx->editor_mode == SESSION_EDITOR_MODE_BBS_EDIT) {
        uint64_t edit_id = ctx->pending_bbs_edit_id;
        bbs_post_t *post = host_find_bbs_post_locked(host, edit_id);
        if (post == nullptr || !post->in_use) {
            ttak_mutex_unlock(&host->lock);
            session_send_system_line(
                ctx, "No post exists with that identifier anymore.");
            session_bbs_reset_pending_post(ctx);
            return;
        }

        bool can_edit = (strncmp(post->author, ctx->user.name,
                                 SSH_CHATTER_USERNAME_LEN) == 0) ||
                        ctx->user.is_operator || ctx->user.is_lan_operator;
        if (!can_edit) {
            ttak_mutex_unlock(&host->lock);
            session_send_system_line(
                ctx, "Only the author or an operator may edit this post.");
            session_bbs_reset_pending_post(ctx);
            return;
        }

        snprintf(post->title, sizeof(post->title), "%s",
                 ctx->pending_bbs_title);
        memcpy(post->body, ctx->pending_bbs_body, ctx->pending_bbs_body_length);
        post->body[ctx->pending_bbs_body_length] = '\0';
        host_strip_column_reset(post->title);
        host_strip_column_reset(post->body);
        post->tag_count = ctx->pending_bbs_tag_count;
        for (size_t idx = 0U; idx < post->tag_count; ++idx) {
            snprintf(post->tags[idx], sizeof(post->tags[idx]), "%s",
                     ctx->pending_bbs_tags[idx]);
            host_strip_column_reset(post->tags[idx]);
        }

        post->bumped_at = time(nullptr);
        snapshot = *post;
        host_bbs_state_save_locked(host);
        ttak_mutex_unlock(&host->lock);

        session_bbs_reset_pending_post(ctx);
        session_bbs_render_post(ctx, &snapshot, "Post updated.", false);
        return;
    }

    bbs_post_t *post = host_allocate_bbs_post_locked(host);
    if (post == nullptr) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "The bulletin board is full right now.");
        return;
    }

    snprintf(post->author, sizeof(post->author), "%s", ctx->user.name);
    snprintf(post->title, sizeof(post->title), "%s", ctx->pending_bbs_title);
    memcpy(post->body, ctx->pending_bbs_body, ctx->pending_bbs_body_length);
    post->body[ctx->pending_bbs_body_length] = '\0';
    host_strip_column_reset(post->author);
    host_strip_column_reset(post->title);
    host_strip_column_reset(post->body);
    post->tag_count = ctx->pending_bbs_tag_count;
    for (size_t idx = 0U; idx < post->tag_count; ++idx) {
        snprintf(post->tags[idx], sizeof(post->tags[idx]), "%s",
                 ctx->pending_bbs_tags[idx]);
        host_strip_column_reset(post->tags[idx]);
    }

    snapshot = *post;
    host_bbs_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);

    session_bbs_reset_pending_post(ctx);

    session_bbs_announce_post(ctx->owner, &snapshot);
    session_bbs_render_post(ctx, &snapshot, "Post created.", true);
}

static void session_bbs_begin_post(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (ctx->bbs_post_pending) {
        const char *terminator = session_editor_terminator(ctx);
        char warning[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(warning, sizeof(warning),
                 "You are already composing a post. Finish it with %s.",
                 terminator);
        session_send_system_line(ctx, warning);
        return;
    }

    ctx->bbs_view_active = false;
    ctx->bbs_view_post_id = 0U;

    if (ctx->owner == nullptr) {
        session_send_system_line(
            ctx, "The bulletin board is unavailable right now.");
        return;
    }

    session_bbs_reset_pending_post(ctx);
    if (!session_bbs_workspace_acquire(ctx)) {
        session_send_system_line(ctx, "Unable to allocate editor workspace.");
        return;
    }

    if (arguments == nullptr) {
        session_bbs_send_usage(ctx, "post", "<title>[|tags...]");
        session_send_system_line(
            ctx, "Use | to separate tags when the title has spaces.");
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);
    if (working[0] == '\0') {
        session_bbs_send_usage(ctx, "post", "<title>[|tags...]");
        session_send_system_line(
            ctx, "Use | to separate tags when the title has spaces.");
        return;
    }

    char title[SSH_CHATTER_BBS_TITLE_LEN];
    title[0] = '\0';
    char *tag_cursor = nullptr;
    char *separator = strchr(working, '|');
    if (separator != nullptr) {
        *separator = '\0';
        char *title_part = working;
        char *tags_part = separator + 1;
        trim_whitespace_inplace(title_part);
        trim_whitespace_inplace(tags_part);
        size_t title_len = strnlen(title_part, sizeof(title));
        if (title_len > 1U &&
            (title_part[0] == '\"' || title_part[0] == '\'') &&
            title_part[title_len - 1U] == title_part[0]) {
            title_part[title_len - 1U] = '\0';
            ++title_part;
            trim_whitespace_inplace(title_part);
        }
        size_t copy_len = strnlen(title_part, sizeof(title) - 1U);
        memcpy(title, title_part, copy_len);
        title[copy_len] = '\0';
        tag_cursor = tags_part;
    } else {
        char *cursor = working;
        if (*cursor == '\"' || *cursor == '\'') {
            char quote = *cursor++;
            char *closing = strchr(cursor, quote);
            if (closing == nullptr) {
                session_send_system_line(
                    ctx, "Missing closing quote for the title.");
                return;
            }
            size_t copy_len = (size_t)(closing - cursor);
            if (copy_len >= sizeof(title)) {
                copy_len = sizeof(title) - 1U;
            }
            memcpy(title, cursor, copy_len);
            title[copy_len] = '\0';
            cursor = closing + 1;
        } else {
            char *space = cursor;
            while (*space != '\0' && !isspace((unsigned char)*space)) {
                ++space;
            }
            size_t copy_len = (size_t)(space - cursor);
            if (copy_len >= sizeof(title)) {
                copy_len = sizeof(title) - 1U;
            }
            memcpy(title, cursor, copy_len);
            title[copy_len] = '\0';
            cursor = space;
        }

        trim_whitespace_inplace(cursor);
        tag_cursor = cursor;
    }

    if (title[0] == '\0') {
        session_send_system_line(ctx, "A title is required to create a post.");
        return;
    }

    size_t tag_count = 0U;
    bool discarded_tags = false;
    bool default_tag_applied = false;
    while (tag_cursor != nullptr && *tag_cursor != '\0') {
        while (isspace((unsigned char)*tag_cursor)) {
            ++tag_cursor;
        }
        if (*tag_cursor == '\0') {
            break;
        }
        char *end = tag_cursor;
        while (*end != '\0' && !isspace((unsigned char)*end)) {
            ++end;
        }
        size_t length = (size_t)(end - tag_cursor);
        if (length > 0U) {
            if (tag_count < SSH_CHATTER_BBS_MAX_TAGS) {
                if (length >= SSH_CHATTER_BBS_TAG_LEN) {
                    length = SSH_CHATTER_BBS_TAG_LEN - 1U;
                }
                char tag_value[SSH_CHATTER_BBS_TAG_LEN];
                memcpy(tag_value, tag_cursor, length);
                tag_value[length] = '\0';
                if (!ctx->user.is_operator &&
                    session_bbs_is_admin_only_tag(tag_value)) {
                    char warning[SSH_CHATTER_MESSAGE_LIMIT];
                    snprintf(warning, sizeof(warning),
                             "The '%s' tag is reserved for administrators.",
                             tag_value);
                    session_send_system_line(ctx, warning);
                    return;
                }
                snprintf(ctx->pending_bbs_tags[tag_count],
                         SSH_CHATTER_BBS_TAG_LEN, "%s",
                         tag_value);
                ++tag_count;
            } else {
                discarded_tags = true;
            }
        }
        tag_cursor = end;
    }

    if (tag_count == 0U) {
        snprintf(ctx->pending_bbs_tags[0], SSH_CHATTER_BBS_TAG_LEN,
                 "%s", SSH_CHATTER_BBS_DEFAULT_TAG);
        tag_count = 1U;
        default_tag_applied = true;
    }

    snprintf(ctx->pending_bbs_title, SSH_CHATTER_BBS_TITLE_LEN, "%s", title);
    ctx->pending_bbs_tag_count = tag_count;
    ctx->pending_bbs_body[0] = '\0';
    ctx->pending_bbs_body_length = 0U;
    ctx->bbs_post_pending = true;
    ctx->editor_mode = SESSION_EDITOR_MODE_BBS_CREATE;
    ctx->pending_bbs_edit_id = 0U;

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    notice[0] = '\0';
    if (default_tag_applied) {
        snprintf(notice, sizeof(notice),
                 "No tags provided; default tag '%s' applied.",
                 SSH_CHATTER_BBS_DEFAULT_TAG);
    }
    if (discarded_tags) {
        if (notice[0] != '\0') {
            strncat(notice, "\n", sizeof(notice) - strlen(notice) - 1U);
        }
        strncat(notice,
                "Only the first four tags were kept. Extra tags were ignored.",
                sizeof(notice) - strlen(notice) - 1U);
    }

    session_bbs_render_editor(ctx, notice[0] != '\0' ? notice : nullptr);
}

static void session_bbs_capture_body_text(session_ctx_t *ctx, const char *text)
{
    if (ctx == nullptr || !ctx->bbs_post_pending || text == nullptr) {
        return;
    }

    session_capture_multiline_text(ctx, text, session_bbs_capture_body_line,
                                   session_bbs_capture_continue);
}

static void session_bbs_capture_body_line(session_ctx_t *ctx, const char *line)
{
    if (ctx == nullptr || !ctx->bbs_post_pending) {
        return;
    }

    char trimmed[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(trimmed, sizeof(trimmed), "%s", line != nullptr ? line : "");
    trim_whitespace_inplace(trimmed);
    if (session_editor_matches_terminator(ctx, trimmed)) {
        session_bbs_commit_pending_post(ctx);
        return;
    }

    if (line == nullptr) {
        line = "";
    }

    char status[SSH_CHATTER_MESSAGE_LIMIT];
    status[0] = '\0';

    session_bbs_recalculate_line_count(ctx);
    bool editing_line =
        ctx->pending_bbs_editing_line &&
        ctx->pending_bbs_cursor_line < ctx->pending_bbs_line_count;
    bool inserting_line =
        !editing_line &&
        ctx->pending_bbs_cursor_line < ctx->pending_bbs_line_count;

    bool updated = false;
    if (editing_line) {
        updated = session_bbs_replace_line(ctx, ctx->pending_bbs_cursor_line,
                                           line, status, sizeof(status));
        ctx->bbs_line_edit_mode = false;
    } else if (inserting_line) {
        updated = session_bbs_insert_line(ctx, ctx->pending_bbs_cursor_line,
                                          line, status, sizeof(status));
        if (updated) {
            session_bbs_set_cursor(ctx, ctx->pending_bbs_cursor_line + 1U,
                                   false);
        }
    } else {
        updated = session_bbs_append_line(ctx, line, status, sizeof(status));
    }

    if (!updated && status[0] == '\0') {
        snprintf(status, sizeof(status),
                 "Unable to update the draft right now.");
    }
    if (updated) {
        ctx->bbs_line_edit_mode = false;
    }

    session_bbs_render_editor(ctx, status[0] != '\0' ? status : nullptr);
}

static void session_bbs_begin_edit(session_ctx_t *ctx, uint64_t id)
{
    if (ctx == nullptr || id == 0U) {
        session_send_system_line(ctx, "Invalid post identifier.");
        return;
    }

    if (ctx->bbs_post_pending) {
        const char *terminator = session_editor_terminator(ctx);
        char warning[SSH_CHATTER_MESSAGE_LIMIT];
        if (ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART) {
            snprintf(warning, sizeof(warning),
                     "You are already composing ASCII art. Finish it with %s.",
                     terminator);
        } else {
            snprintf(warning, sizeof(warning),
                     "You are already composing a post. Finish it with %s.",
                     terminator);
        }
        session_send_system_line(ctx, warning);
        return;
    }

    if (ctx->owner == nullptr) {
        session_send_system_line(
            ctx, "The bulletin board is unavailable right now.");
        return;
    }

    host_t *host = ctx->owner;
    ttak_mutex_lock(&host->lock);
    bbs_post_t *post = host_find_bbs_post_locked(host, id);
    bbs_post_t snapshot = {0};
    if (post != nullptr && post->in_use) {
        snapshot = *post;
    }
    ttak_mutex_unlock(&host->lock);

    if (post == nullptr || !snapshot.in_use) {
        session_send_system_line(ctx, "No post exists with that identifier.");
        return;
    }

    bool can_edit = (strncmp(snapshot.author, ctx->user.name,
                             SSH_CHATTER_USERNAME_LEN) == 0) ||
                    ctx->user.is_operator || ctx->user.is_lan_operator;
    if (!can_edit) {
        session_send_system_line(
            ctx, "Only the author or an operator may edit this post.");
        return;
    }

    session_bbs_reset_pending_post(ctx);
    if (!session_bbs_workspace_acquire(ctx)) {
        session_send_system_line(ctx, "Unable to allocate editor workspace.");
        return;
    }
    ctx->bbs_post_pending = true;
    ctx->editor_mode = SESSION_EDITOR_MODE_BBS_EDIT;
    ctx->pending_bbs_edit_id = id;

    snprintf(ctx->pending_bbs_title, SSH_CHATTER_BBS_TITLE_LEN, "%s",
             snapshot.title);

    size_t body_len =
        strnlen(snapshot.body, SSH_CHATTER_BBS_BODY_LEN - 1U);
    memcpy(ctx->pending_bbs_body, snapshot.body, body_len);
    ctx->pending_bbs_body[body_len] = '\0';
    ctx->pending_bbs_body_length = body_len;

    ctx->pending_bbs_tag_count = snapshot.tag_count;
    if (ctx->pending_bbs_tag_count > SSH_CHATTER_BBS_MAX_TAGS) {
        ctx->pending_bbs_tag_count = SSH_CHATTER_BBS_MAX_TAGS;
    }
    for (size_t idx = 0U; idx < ctx->pending_bbs_tag_count; ++idx) {
        snprintf(ctx->pending_bbs_tags[idx], SSH_CHATTER_BBS_TAG_LEN,
                 "%s", snapshot.tags[idx]);
    }

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice),
             "Editing post #%" PRIu64 ". Finish with %s to save changes.", id,
             session_bbs_terminator(ctx));
    session_bbs_render_editor(ctx, notice);
}

// Append a comment to a post.
static void session_bbs_add_comment(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr || arguments == nullptr) {
        session_bbs_send_usage(ctx, "comment", "<id>|<text>");
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);
    if (working[0] == '\0') {
        session_bbs_send_usage(ctx, "comment", "<id>|<text>");
        return;
    }

    char *separator = strchr(working, '|');
    if (separator == nullptr) {
        session_bbs_send_usage(ctx, "comment", "<id>|<text>");
        return;
    }
    *separator = '\0';
    char *id_text = working;
    char *comment_text = separator + 1;
    trim_whitespace_inplace(id_text);
    trim_whitespace_inplace(comment_text);

    if (id_text[0] == '\0' || comment_text[0] == '\0') {
        session_bbs_send_usage(ctx, "comment", "<id>|<text>");
        return;
    }

    uint64_t id = (uint64_t)strtoull(id_text, nullptr, 10);
    if (id == 0U) {
        session_send_system_line(ctx, "Invalid post identifier.");
        return;
    }

    size_t comment_scan_length =
        strnlen(comment_text, SSH_CHATTER_BBS_COMMENT_LEN);
    if (session_security_check_text(ctx, "BBS comment", comment_text,
                                    comment_scan_length,
                                    false) != HOST_SECURITY_SCAN_CLEAN) {
        return;
    }

    host_t *host = ctx->owner;
    ttak_mutex_lock(&host->lock);
    bbs_post_t *post = host_find_bbs_post_locked(host, id);
    if (post == nullptr || !post->in_use) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "No post exists with that identifier.");
        return;
    }
    if (post->comment_count >= SSH_CHATTER_BBS_MAX_COMMENTS) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx,
                                 "This post has reached the comment limit.");
        return;
    }

    size_t comment_index = post->comment_count;
    bbs_comment_t *comment = &post->comments[comment_index];
    post->comment_count++;
    snprintf(comment->author, sizeof(comment->author), "%s", ctx->user.name);
    size_t comment_len =
        strnlen(comment_text, SSH_CHATTER_BBS_COMMENT_LEN - 1U);
    memcpy(comment->text, comment_text, comment_len);
    comment->text[comment_len] = '\0';
    host_strip_column_reset(comment->author);
    host_strip_column_reset(comment->text);
    comment->created_at = time(nullptr);
    post->bumped_at = comment->created_at;
    bbs_post_t snapshot = *post;
    host_bbs_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);

    if (comment_index < snapshot.comment_count) {
        session_bbs_announce_comment(ctx->owner, &snapshot,
                                     &snapshot.comments[comment_index]);
    }
    session_bbs_render_post(ctx, &snapshot, "Comment added.", false);
}

static void session_bbs_delete(session_ctx_t *ctx, uint64_t id)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (id == 0U) {
        session_send_system_line(ctx, "Invalid post identifier.");
        return;
    }

    host_t *host = ctx->owner;
    ttak_mutex_lock(&host->lock);
    bbs_post_t *post = host_find_bbs_post_locked(host, id);
    if (post == nullptr || !post->in_use) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "No post exists with that identifier.");
        return;
    }

    bool can_delete = (strncmp(post->author, ctx->user.name,
                               SSH_CHATTER_USERNAME_LEN) == 0) ||
                      ctx->user.is_operator || ctx->user.is_lan_operator;
    if (!can_delete) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(
            ctx, "Only the author or an operator may delete this post.");
        return;
    }

    host_clear_bbs_post_locked(host, post);
    host_bbs_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);

    session_send_system_line(ctx, "Post deleted.");
}

// Bump a post to the top of the list by refreshing its activity time.
static void session_bbs_regen_post(session_ctx_t *ctx, uint64_t id)
{
    if (ctx == nullptr || ctx->owner == nullptr || id == 0U) {
        return;
    }

    host_t *host = ctx->owner;
    ttak_mutex_lock(&host->lock);
    bbs_post_t *post = host_find_bbs_post_locked(host, id);
    if (post == nullptr || !post->in_use) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "No post exists with that identifier.");
        return;
    }

    post->bumped_at = time(nullptr);
    bbs_post_t snapshot = *post;
    host_bbs_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);

    session_bbs_render_post(ctx, &snapshot, "Post bumped to the top.", false);
}

static void session_rss_clear(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    memset(&ctx->rss_view, 0, sizeof(ctx->rss_view));
    ctx->in_rss_mode = false;
}

static void session_rss_exit(session_ctx_t *ctx, const char *reason)
{
    if (ctx == nullptr) {
        return;
    }

    const bool was_active = ctx->in_rss_mode;
    session_rss_clear(ctx);

    if (reason != nullptr && reason[0] != '\0') {
        session_send_system_line(ctx, reason);
    } else if (was_active) {
        session_send_system_line(ctx, "RSS reader closed.");
    }

    if (was_active) {
        session_render_prompt(ctx, false);
    }
}

static void session_rss_show_current(session_ctx_t *ctx)
{
    if (ctx == nullptr || !ctx->rss_view.active ||
        ctx->rss_view.item_count == 0U) {
        return;
    }

    if (ctx->rss_view.cursor >= ctx->rss_view.item_count) {
        ctx->rss_view.cursor = ctx->rss_view.item_count - 1U;
    }

    const rss_session_item_t *item = &ctx->rss_view.items[ctx->rss_view.cursor];

    char header[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(header, sizeof(header), "Feed %s (%zu/%zu)", ctx->rss_view.tag,
             ctx->rss_view.cursor + 1U, ctx->rss_view.item_count);
    session_render_separator(ctx, header);

    char line[SSH_CHATTER_MESSAGE_LIMIT];
    if (item->title[0] != '\0') {
        snprintf(line, sizeof(line), "Title : %s", item->title);
    } else {
        snprintf(line, sizeof(line), "Title : (untitled)");
    }
    session_send_system_line(ctx, line);

    if (item->link[0] != '\0') {
        snprintf(line, sizeof(line), "Link  : %s", item->link);
    } else {
        snprintf(line, sizeof(line), "Link  : (none)");
    }
    session_send_system_line(ctx, line);

    if (item->summary[0] != '\0') {
        session_send_system_line(ctx, "Summary:");
        char working[SSH_CHATTER_RSS_SUMMARY_LEN];
        snprintf(working, sizeof(working), "%s", item->summary);
        char *saveptr = nullptr;
        char *fragment = strtok_r(working, "\r\n", &saveptr);
        while (fragment != nullptr) {
            rss_trim_whitespace(fragment);
            if (fragment[0] != '\0') {
                snprintf(line, sizeof(line), "  %s", fragment);
                session_send_system_line(ctx, line);
            }
            fragment = strtok_r(nullptr, "\r\n", &saveptr);
        }
    } else {
        session_send_system_line(ctx, "Summary: (none)");
    }
}

static void session_rss_begin(session_ctx_t *ctx, const char *tag,
                              const rss_session_item_t *items, size_t count)
{
    if (ctx == nullptr || tag == nullptr || tag[0] == '\0' ||
        items == nullptr || count == 0U) {
        return;
    }

    session_rss_clear(ctx);

    if (count > SSH_CHATTER_RSS_MAX_ITEMS) {
        count = SSH_CHATTER_RSS_MAX_ITEMS;
    }

    ctx->rss_view.active = true;
    ctx->rss_view.item_count = count;
    ctx->rss_view.cursor = 0U;
    snprintf(ctx->rss_view.tag, sizeof(ctx->rss_view.tag), "%s", tag);
    for (size_t idx = 0U; idx < count; ++idx) {
        ctx->rss_view.items[idx] = items[idx];
    }
    ctx->in_rss_mode = true;

    char intro[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(
        intro, sizeof(intro),
        "Browsing feed '%s'. Use Up/Down arrows to navigate. Type /exit or "
        "press Ctrl+Z to return.",
        ctx->rss_view.tag);
    session_render_separator(ctx, "RSS Reader");
    session_send_system_line(ctx, intro);
    session_rss_show_current(ctx);
}

static bool session_rss_move(session_ctx_t *ctx, int delta)
{
    if (ctx == nullptr || !ctx->rss_view.active ||
        ctx->rss_view.item_count == 0U || delta == 0) {
        return false;
    }

    size_t current = ctx->rss_view.cursor;
    size_t next = current;

    if (delta > 0) {
        if (next + 1U < ctx->rss_view.item_count) {
            next += 1U;
        }
    } else {
        if (next > 0U) {
            next -= 1U;
        }
    }

    if (next == current) {
        return false;
    }

    ctx->rss_view.cursor = next;
    session_rss_show_current(ctx);
    return true;
}

static void session_rss_list(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    rss_feed_t snapshot[SSH_CHATTER_RSS_MAX_FEEDS];
    size_t count = 0U;

    ttak_mutex_lock(&ctx->owner->lock);
    for (size_t idx = 0U; idx < SSH_CHATTER_RSS_MAX_FEEDS; ++idx) {
        if (!ctx->owner->rss_feeds[idx].in_use) {
            continue;
        }
        snapshot[count++] = ctx->owner->rss_feeds[idx];
        if (count >= SSH_CHATTER_RSS_MAX_FEEDS) {
            break;
        }
    }
    ttak_mutex_unlock(&ctx->owner->lock);

    session_render_separator(ctx, "RSS Feeds");
    if (count == 0U) {
        session_send_system_line(ctx,
                                 "No RSS feeds registered. Operators can add "
                                 "one with /rss add <url> <tag>.");
        return;
    }

    for (size_t idx = 0U; idx < count; ++idx) {
        const rss_feed_t *entry = &snapshot[idx];
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        if (entry->last_title[0] != '\0') {
            char preview[72];
            snprintf(preview, sizeof(preview), "%.64s", entry->last_title);
            snprintf(line, sizeof(line), "[%s] %s (last: %s)", entry->tag,
                     entry->url, preview);
        } else {
            snprintf(line, sizeof(line), "[%s] %s", entry->tag, entry->url);
        }
        session_send_system_line(ctx, line);
    }
}

static void session_rss_read(session_ctx_t *ctx, const char *tag)
{
    if (ctx == nullptr || ctx->owner == nullptr || tag == nullptr ||
        tag[0] == '\0') {
        session_send_system_line(ctx, "Usage: /rss read <tag>");
        return;
    }

    char working[SSH_CHATTER_RSS_TAG_LEN];
    snprintf(working, sizeof(working), "%s", tag);
    rss_trim_whitespace(working);
    if (!rss_tag_is_valid(working)) {
        session_send_system_line(
            ctx, "Tags may only contain letters, numbers, '-', '_' or '.'.");
        return;
    }

    rss_feed_t feed_snapshot = {0};
    ttak_mutex_lock(&ctx->owner->lock);
    rss_feed_t *entry = host_find_rss_feed_locked(ctx->owner, working);
    if (entry != nullptr) {
        feed_snapshot = *entry;
    }
    ttak_mutex_unlock(&ctx->owner->lock);

    if (feed_snapshot.tag[0] == '\0') {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "No RSS feed found for tag '%s'.",
                 working);
        session_send_system_line(ctx, message);
        return;
    }

    rss_session_item_t items[SSH_CHATTER_RSS_MAX_ITEMS];
    size_t item_count = 0U;
    if (!host_rss_fetch_items(&feed_snapshot, items, SSH_CHATTER_RSS_MAX_ITEMS,
                              &item_count)) {
        session_send_system_line(ctx,
                                 "Failed to fetch RSS feed. Try again later.");
        return;
    }

    if (item_count == 0U) {
        session_send_system_line(
            ctx, "The feed does not contain any recent entries.");
        return;
    }

    time_t now = time(nullptr);
    ttak_mutex_lock(&ctx->owner->lock);
    entry = host_find_rss_feed_locked(ctx->owner, working);
    if (entry != nullptr) {
        entry->last_checked = now;
        snprintf(entry->last_title, sizeof(entry->last_title), "%s",
                 items[0].title);
        snprintf(entry->last_link, sizeof(entry->last_link), "%s",
                 items[0].link);
        host_rss_state_save_locked(ctx->owner);
    }
    ttak_mutex_unlock(&ctx->owner->lock);

    session_rss_begin(ctx, feed_snapshot.tag, items, item_count);
}

static void session_handle_rss(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    static const char *kUsage =
        "Usage: /rss <add <url> <tag>|del <tag>|read <tag>|list>";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/rss", kUsage, usage, sizeof(usage));

    char working[SSH_CHATTER_MAX_INPUT_LEN];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
    }
    rss_trim_whitespace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    char *saveptr = nullptr;
    char *command = strtok_r(working, " \t", &saveptr);
    if (command == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    if (strcasecmp(command, "list") == 0) {
        session_rss_list(ctx);
        return;
    }

    if (strcasecmp(command, "add") == 0 || strcasecmp(command, "추가") == 0) {
        if (!ctx->user.is_operator) {
            session_send_system_line(ctx, "Only operators may add RSS feeds.");
            return;
        }

        char *url = strtok_r(nullptr, " \t", &saveptr);
        char *tag = strtok_r(nullptr, " \t", &saveptr);
        if (url == nullptr || tag == nullptr) {
            session_send_system_line(ctx, "Usage: /rss add <url> <tag>");
            return;
        }

        rss_trim_whitespace(url);
        rss_trim_whitespace(tag);
        if (url[0] == '\0' || tag[0] == '\0') {
            session_send_system_line(ctx, "Usage: /rss add <url> <tag>");
            return;
        }

        char error[128];
        if (host_rss_add_feed(ctx->owner, url, tag, error, sizeof(error))) {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message),
                     "RSS feed '%s' registered as '%s'.", url, tag);
            session_send_system_line(ctx, message);
            host_rss_start_backend(ctx->owner);
        } else {
            if (error[0] == '\0') {
                snprintf(error, sizeof(error), "Failed to add RSS feed.");
            }
            session_send_system_line(ctx, error);
        }
        return;
    }

    if (strcasecmp(command, "del") == 0 || strcasecmp(command, "삭제") == 0) {
        if (!ctx->user.is_operator) {
            session_send_system_line(ctx,
                                     "Only operators may delete RSS feeds.");
            return;
        }

        char *tag = strtok_r(nullptr, " \t", &saveptr);
        if (tag == nullptr) {
            session_send_system_line(ctx, "Usage: /rss del <tag>");
            return;
        }

        rss_trim_whitespace(tag);
        if (tag[0] == '\0') {
            session_send_system_line(ctx, "Usage: /rss del <tag>");
            return;
        }

        char error[128];
        if (host_rss_remove_feed(ctx->owner, tag, error, sizeof(error))) {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message), "RSS feed '%s' deleted.", tag);
            session_send_system_line(ctx, message);
        } else {
            if (error[0] == '\0') {
                snprintf(error, sizeof(error), "Failed to delete RSS feed.");
            }
            session_send_system_line(ctx, error);
        }
        return;
    }

    if (strcasecmp(command, "read") == 0) {
        char *tag = strtok_r(nullptr, " \t", &saveptr);
        if (tag == nullptr) {
            session_send_system_line(ctx, "Usage: /rss read <tag>");
            return;
        }
        session_rss_read(ctx, tag);
        return;
    }

    session_send_system_line(ctx, usage);
}

static void session_handle_morse(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    static const char *kUsage = "Usage: /morse <on <filter>|off|status>";

    char working[SSH_CHATTER_MAX_INPUT_LEN];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
        trim_whitespace_inplace(working);
    }

    char *command = strtok(working, " \t");
    if (command == nullptr || strcasecmp(command, "status") == 0) {
        char status[SSH_CHATTER_MESSAGE_LIMIT];
        if (ctx->morse_feed_enabled) {
            snprintf(status, sizeof(status),
                     "Morse feed is ON. Filter: '%s'. /morse off to mute.",
                     ctx->morse_filter);
        } else {
            snprintf(status, sizeof(status),
                     "Morse feed is OFF. /morse on <filter> to resume.");
        }
        session_send_system_line(ctx, status);
        return;
    }

    if (strcasecmp(command, "on") == 0) {
        char *filter = strtok(NULL, "");
        if (filter == NULL || filter[0] == '\0') {
            session_send_system_line(ctx, kUsage);
            return;
        }
        trim_whitespace_inplace(filter);
        if (strlen(filter) >= sizeof(ctx->morse_filter)) {
            session_send_system_line(ctx, "Filter is too long.");
            return;
        }
        strncpy(ctx->morse_filter, filter, sizeof(ctx->morse_filter) - 1);
        ctx->morse_filter[sizeof(ctx->morse_filter) - 1] = '\0';
        ctx->morse_feed_enabled = true;
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message),
                 "Morse relay enabled with filter '%s'. Use /morse off to silence.",
                 ctx->morse_filter);
        session_send_system_line(ctx, message);
        return;
    }

    if (strcasecmp(command, "off") == 0) {
        ctx->morse_feed_enabled = false;
        ctx->morse_filter[0] = '\0';
        session_send_system_line(ctx,
                                 "Morse relay disabled for this session.");
        return;
    }

    session_send_system_line(ctx, kUsage);
}

static void session_handle_morse_chat(session_ctx_t *ctx,
                                      const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    static const char *kUsage = "Usage: /morse-chat <text>";

    if (arguments == nullptr || arguments[0] == '\0') {
        session_send_system_line(ctx, kUsage);
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, kUsage);
        return;
    }

    morse_client_t *client = ctx->owner->morse_client;
    if (client == nullptr || !morse_client_connected(client)) {
        session_send_system_line(ctx,
                                 "Morse relay is not connected. Please try "
                                 "again shortly.");
        return;
    }

    if (!morse_client_send(client, working)) {
        session_send_system_line(ctx, "Unable to send Morse chat right now.");
        return;
    }

    session_send_system_line(ctx, "[MORSE] message transmitted.");
}

static bool host_asciiart_cooldown_active(host_t *host, const char *ip,
                                          const struct timespec *now,
                                          long *remaining_seconds)
{
    if (host == nullptr || ip == nullptr || ip[0] == '\0') {
        if (remaining_seconds != nullptr) {
            *remaining_seconds = 0L;
        }
        return false;
    }

    struct timespec current = {0, 0};
    if (now != nullptr) {
        current = *now;
    } else if (clock_gettime(CLOCK_MONOTONIC, &current) != 0) {
        current.tv_sec = time(nullptr);
        current.tv_nsec = 0L;
    }

    bool active = false;
    long remaining = 0L;

    ttak_mutex_lock(&host->lock);
    join_activity_entry_t *entry = host_find_join_activity_locked(host, ip);
    if (entry != nullptr && entry->asciiart_has_cooldown) {
        struct timespec expiry = entry->last_asciiart_post;
        expiry.tv_sec += SSH_CHATTER_ASCIIART_COOLDOWN_SECONDS;
        if (timespec_compare(&current, &expiry) >= 0) {
            entry->asciiart_has_cooldown = false;
        } else {
            active = true;
            struct timespec diff = timespec_diff(&expiry, &current);
            remaining = diff.tv_sec;
            if (diff.tv_nsec > 0L) {
                ++remaining;
            }
            if (remaining < 0L) {
                remaining = 0L;
            }
        }
    }
    ttak_mutex_unlock(&host->lock);

    if (remaining_seconds != nullptr) {
        *remaining_seconds = active ? remaining : 0L;
    }

    return active;
}

static void host_asciiart_register_post(host_t *host, const char *ip,
                                        const struct timespec *when)
{
    if (host == nullptr || ip == nullptr || ip[0] == '\0' || when == nullptr) {
        return;
    }

    ttak_mutex_lock(&host->lock);
    join_activity_entry_t *entry = host_ensure_join_activity_locked(host, ip);
    if (entry != nullptr) {
        entry->last_asciiart_post = *when;
        entry->asciiart_has_cooldown = true;
    }
    ttak_mutex_unlock(&host->lock);
}

static void session_asciiart_reset(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    ctx->asciiart_pending = false;
    ctx->asciiart_target = SESSION_ASCIIART_TARGET_NONE;
    if (ctx->asciiart_buffer != nullptr) {
        ctx->asciiart_buffer[0] = '\0';
    }
    ctx->asciiart_length = 0U;
    ctx->asciiart_line_count = 0U;
}

static bool session_asciiart_cooldown_active(session_ctx_t *ctx,
                                             struct timespec *now,
                                             long *remaining_seconds)
{
    if (ctx == nullptr) {
        return false;
    }

    struct timespec current;
    if (clock_gettime(CLOCK_MONOTONIC, &current) != 0) {
        current.tv_sec = time(nullptr);
        current.tv_nsec = 0L;
    }

    if (now != nullptr) {
        *now = current;
    }

    long session_remaining = 0L;
    bool session_active = false;
    if (ctx->asciiart_has_cooldown) {
        struct timespec expiry = ctx->last_asciiart_post;
        expiry.tv_sec += SSH_CHATTER_ASCIIART_COOLDOWN_SECONDS;
        if (timespec_compare(&current, &expiry) >= 0) {
            ctx->asciiart_has_cooldown = false;
        } else {
            session_active = true;
            struct timespec diff = timespec_diff(&expiry, &current);
            session_remaining = diff.tv_sec;
            if (diff.tv_nsec > 0L) {
                ++session_remaining;
            }
            if (session_remaining < 0L) {
                session_remaining = 0L;
            }
        }
    }

    long ip_remaining = 0L;
    bool ip_active = host_asciiart_cooldown_active(ctx->owner, ctx->client_ip,
                                                   &current, &ip_remaining);

    if (!session_active && !ip_active) {
        if (remaining_seconds != nullptr) {
            *remaining_seconds = 0L;
        }
        return false;
    }

    long max_remaining = session_active ? session_remaining : 0L;
    if (ip_active && ip_remaining > max_remaining) {
        max_remaining = ip_remaining;
    }

    if (remaining_seconds != nullptr) {
        *remaining_seconds = max_remaining;
    }

    return true;
}

static void session_asciiart_begin(session_ctx_t *ctx,
                                   session_asciiart_target_t target)
{
    if (ctx == nullptr || target == SESSION_ASCIIART_TARGET_NONE) {
        return;
    }

    if (ctx->bbs_post_pending) {
        if (ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART) {
            const char *terminator = session_editor_terminator(ctx);
            char notice[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(notice, sizeof(notice),
                     "You are already composing ASCII art. Finish it with %s.",
                     terminator);
            session_send_system_line(ctx, notice);
        } else {
            session_send_system_line(
                ctx, "Finish your BBS draft before starting ASCII art.");
        }
        return;
    }

    if (ctx->asciiart_pending) {
        const char *terminator = session_asciiart_terminator(ctx);
        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice),
                 "You are already composing ASCII art. Finish it with %s.",
                 terminator);
        session_send_system_line(ctx, notice);
        return;
    }

    if (target == SESSION_ASCIIART_TARGET_CHAT) {
        struct timespec now;
        long remaining = 0L;
        if (session_asciiart_cooldown_active(ctx, &now, &remaining)) {
            if (remaining < 1L) {
                remaining = 1L;
            }
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message),
                     "You can share another ASCII art in %ld second%s.",
                     remaining, remaining == 1L ? "" : "s");
            session_send_system_line(ctx, message);
            return;
        }
    }

    session_asciiart_reset(ctx);
    ctx->asciiart_pending = true;
    ctx->asciiart_target = target;

    session_bbs_reset_pending_post(ctx);
    if (!session_asciiart_buffer_acquire(ctx) ||
        !session_bbs_workspace_acquire(ctx)) {
        session_send_system_line(
            ctx, "ASCII art editor is unavailable right now.");
        session_asciiart_reset(ctx);
        return;
    }
    ctx->editor_mode = SESSION_EDITOR_MODE_ASCIIART;
    ctx->bbs_post_pending = true;

    size_t ascii_bytes = (size_t)SSH_CHATTER_ASCIIART_BUFFER_LEN;
    char status[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(status, sizeof(status),
             "ASCII art composer ready (max %u lines, up to %zu bytes, "
             "10-minute cooldown per IP).",
             SSH_CHATTER_ASCIIART_MAX_LINES, ascii_bytes);

    session_bbs_render_editor(ctx, status);
}

static void session_asciiart_import_from_editor(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    if (!session_bbs_workspace_acquire(ctx) ||
        !session_asciiart_buffer_acquire(ctx)) {
        session_send_system_line(
            ctx, "ASCII art workspace is unavailable right now.");
        return;
    }

    session_bbs_recalculate_line_count(ctx);

    size_t copy_len = ctx->pending_bbs_body_length;
    if (copy_len >= SSH_CHATTER_ASCIIART_BUFFER_LEN) {
        copy_len = SSH_CHATTER_ASCIIART_BUFFER_LEN - 1U;
    }

    if (copy_len > 0U) {
        memcpy(ctx->asciiart_buffer, ctx->pending_bbs_body, copy_len);
    }
    ctx->asciiart_buffer[copy_len] = '\0';
    ctx->asciiart_length = copy_len;
    ctx->asciiart_line_count = ctx->pending_bbs_line_count;
    if (ctx->asciiart_line_count > SSH_CHATTER_ASCIIART_MAX_LINES) {
        ctx->asciiart_line_count = SSH_CHATTER_ASCIIART_MAX_LINES;
    }
}

static void session_asciiart_commit(session_ctx_t *ctx)
{
    if (ctx == nullptr || !ctx->asciiart_pending) {
        return;
    }

    if (!session_asciiart_buffer_acquire(ctx)) {
        session_asciiart_reset(ctx);
        return;
    }

    if (ctx->asciiart_length == 0U) {
        session_asciiart_cancel(ctx, "ASCII art draft discarded.");
        return;
    }

    if (ctx->owner == nullptr) {
        session_asciiart_reset(ctx);
        return;
    }

    if (session_security_check_text(ctx, "ASCII art", ctx->asciiart_buffer,
                                    ctx->asciiart_length,
                                    false) != HOST_SECURITY_SCAN_CLEAN) {
        session_asciiart_reset(ctx);
        return;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        now.tv_sec = time(nullptr);
        now.tv_nsec = 0L;
    }

    ctx->last_asciiart_post = now;
    ctx->asciiart_has_cooldown = true;
    host_asciiart_register_post(ctx->owner, ctx->client_ip, &now);

    chat_history_entry_t entry = {0};
    if (!host_history_record_user(ctx->owner, ctx, ctx->asciiart_buffer, true,
                                  &entry)) {
        session_asciiart_reset(ctx);
        return;
    }

    session_send_history_entry(ctx, &entry);
    chat_room_broadcast_entry(&ctx->owner->room, &entry, ctx);
    host_notify_external_clients(ctx->owner, &entry);

    ctx->last_message_time = now;
    ctx->has_last_message_time = true;

    session_asciiart_reset(ctx);
}

static void session_asciiart_cancel(session_ctx_t *ctx, const char *reason)
{
    if (ctx == nullptr || !ctx->asciiart_pending) {
        return;
    }

    const bool used_editor = ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART;
    session_asciiart_reset(ctx);
    if (used_editor) {
        session_bbs_reset_pending_post(ctx);
    }
    if (reason != nullptr && reason[0] != '\0') {
        session_send_system_line(ctx, reason);
    }
}

static bool session_asciiart_capture_continue(const session_ctx_t *ctx)
{
    return ctx != nullptr && ctx->asciiart_pending;
}

static bool session_bbs_capture_continue(const session_ctx_t *ctx)
{
    return ctx != nullptr && ctx->bbs_post_pending;
}

static void session_capture_multiline_text(
    session_ctx_t *ctx, const char *text, session_text_line_consumer_t consumer,
    session_text_continue_predicate_t should_continue)
{
    if (ctx == nullptr || text == nullptr || consumer == nullptr ||
        should_continue == nullptr) {
        return;
    }

    char line[SSH_CHATTER_MAX_INPUT_LEN];
    size_t line_length = 0U;
    bool emitted = false;

    const char *cursor = text;
    while (*cursor != '\0') {
        char ch = *cursor++;
        if (ch == '\r') {
            if (*cursor == '\n') {
                ++cursor;
            }
            line[line_length] = '\0';
            consumer(ctx, line);
            emitted = true;
            line_length = 0U;
            if (!should_continue(ctx)) {
                return;
            }
            continue;
        }

        if (ch == '\n') {
            line[line_length] = '\0';
            consumer(ctx, line);
            emitted = true;
            line_length = 0U;
            if (!should_continue(ctx)) {
                return;
            }
            continue;
        }

        if (line_length + 1U < sizeof(line)) {
            line[line_length++] = ch;
        }
    }

    if (line_length > 0U || !emitted) {
        line[line_length] = '\0';
        consumer(ctx, line);
    }
}

static void session_asciiart_capture_text(session_ctx_t *ctx, const char *text)
{
    if (ctx == nullptr || !ctx->asciiart_pending || text == nullptr) {
        return;
    }

    session_capture_multiline_text(ctx, text, session_asciiart_capture_line,
                                   session_asciiart_capture_continue);
}

static void session_asciiart_capture_line(session_ctx_t *ctx, const char *line)
{
    if (ctx == nullptr || !ctx->asciiart_pending) {
        return;
    }

    if (!session_asciiart_buffer_acquire(ctx)) {
        session_send_system_line(ctx, "ASCII art buffer unavailable.");
        return;
    }

    char trimmed[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(trimmed, sizeof(trimmed), "%s", line != nullptr ? line : "");
    trim_whitespace_inplace(trimmed);
    if (session_asciiart_matches_terminator(trimmed)) {
        session_asciiart_commit(ctx);
        return;
    }

    if (ctx->asciiart_line_count >= SSH_CHATTER_ASCIIART_MAX_LINES) {
        session_send_system_line(
            ctx, "ASCII art line limit reached. Use the terminator to finish.");
        return;
    }

    if (line == nullptr) {
        line = "";
    }

    const char *full_message =
        "ASCII art buffer is full. Additional text ignored.";
    const char *truncate_message =
        "Line truncated to fit within the ASCII art size limit.";

    size_t buffer_capacity = SSH_CHATTER_ASCIIART_BUFFER_LEN;

    if (ctx->asciiart_length >= buffer_capacity - 1U) {
        session_send_system_line(ctx, full_message);
        return;
    }

    size_t available = buffer_capacity - ctx->asciiart_length - 1U;
    const size_t newline_cost = ctx->asciiart_length > 0U ? 1U : 0U;
    if (available < newline_cost) {
        session_send_system_line(ctx, full_message);
        return;
    }

    size_t line_length = strlen(line);
    size_t max_line_length =
        (available > newline_cost) ? (available - newline_cost) : 0U;
    if (line_length > max_line_length) {
        line_length = max_line_length;
        session_send_system_line(ctx, truncate_message);
    }

    if (ctx->asciiart_length > 0U) {
        ctx->asciiart_buffer[ctx->asciiart_length++] = '\n';
    }

    if (line_length > 0U) {
        memcpy(ctx->asciiart_buffer + ctx->asciiart_length, line, line_length);
        ctx->asciiart_length += line_length;
    }

    ctx->asciiart_buffer[ctx->asciiart_length] = '\0';
    ctx->asciiart_line_count += 1U;
}

//

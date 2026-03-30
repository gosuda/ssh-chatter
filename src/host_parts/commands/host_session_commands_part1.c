/**
 * @file host_session_commands.c
 * @desc File-level documentation for host_session_commands.c, describing its role
 *       in the SSH-Chatter server and providing a consistent header
 *       comment format across C sources.
 * @return None.
 */

#include "../host_internal.h"
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
    // Only perform the full clear+sink cycle when actually scrolled back,
    // matching the pattern used for regular chat messages to avoid an
    // unnecessary duplicate full-frame redraw that can overwhelm the
    // SSH channel write buffer.
    const bool was_scrolled_back = ctx->history_scroll_position > 0U;
    if (was_scrolled_back) {
        session_scrollback_reset_position(ctx);
    }

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
    enum {
        SESSION_REPLY_USERNAME_PREC = SSH_CHATTER_USERNAME_LEN - 1,
        SESSION_REPLY_MESSAGE_PREC = SSH_CHATTER_MESSAGE_LIMIT / 2
    };
    snprintf(reply_message, sizeof(reply_message),
             "->[r#%s %s%s] %.*s: %.*s", reply_label, target_prefix, target_label,
             SESSION_REPLY_USERNAME_PREC, stored.username,
             SESSION_REPLY_MESSAGE_PREC, stored.message);

    chat_history_entry_t reply_entry = {0};
    if (!host_history_record_system(ctx->owner, reply_message, &reply_entry)) {
        session_send_system_line(ctx, "Unable to broadcast reply.");
        return;
    }

    // Show the reply to the sender immediately (just like regular chat messages)
    session_send_history_entry(ctx, &reply_entry);

    if (ctx->history_scroll_position == 0U && !ctx->bracket_paste_active) {
        session_refresh_input_line(ctx);
    }

    // Broadcast the reply entry to all users so it appears in chat buffer
    chat_room_broadcast_entry(&ctx->owner->room, &reply_entry, ctx);

    // Force-sync the sender's screen after broadcasting.  The broadcast
    // marks all room members (including the sender) with a pending sink
    // flag but skips the sender in the delivery loop, leaving the flag
    // unprocessed until the next keystroke.  Processing it here ensures
    // the sender's viewport immediately reflects all recent messages.
    session_process_pending_sink(ctx);
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

    if (ctx->newline_mode == SESSION_NEWLINE_MODE_AUTO) {
        const bool prefers_crlf = (strcasecmp(descriptor->name, "windows") == 0);
        char newline_notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(newline_notice, sizeof(newline_notice),
                 "Line ending default is now %s (auto mode). Use /set-lf "
                 "<auto|lf|crlf> to override.",
                 prefers_crlf ? "CRLF" : "LF");
        session_send_system_line(ctx, newline_notice);
    }
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
                 "#%zu %.*s -- flags planted: %u (last landing %.*s)",
                 idx + 1U, SSH_CHATTER_USERNAME_LEN - 1, lander->username,
                 lander->flag_count, (int)(sizeof(when) - 1U), when);
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

static void session_handle_shell(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(
            ctx, "Only granted operators may use /shell.");
        return;
    }

    if (arguments != nullptr) {
        char trimmed[32];
        snprintf(trimmed, sizeof(trimmed), "%s", arguments);
        trim_whitespace_inplace(trimmed);
        if (trimmed[0] != '\0') {
            session_send_system_line(ctx, "Usage: /shell");
            return;
        }
    }

    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
        session_send_system_line(ctx, "Failed to start shell.");
        if (stdin_pipe[0] >= 0) {
            close(stdin_pipe[0]);
        }
        if (stdin_pipe[1] >= 0) {
            close(stdin_pipe[1]);
        }
        if (stdout_pipe[0] >= 0) {
            close(stdout_pipe[0]);
        }
        if (stdout_pipe[1] >= 0) {
            close(stdout_pipe[1]);
        }
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        session_send_system_line(ctx, "Failed to start shell.");
        return;
    }

    if (pid == 0) {
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stdout_pipe[1], STDERR_FILENO);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        execl("/bin/sh", "sh", "-i", (char *)nullptr);
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    session_send_system_line(
        ctx, "Launching /bin/sh. Type 'exit' to return to SSH-Chatter.");

    bool input_open = true;
    bool output_open = true;
    while (input_open || output_open) {
        struct pollfd child_out = {
            .fd = stdout_pipe[0],
            .events = output_open ? POLLIN : 0,
            .revents = 0,
        };

        if (output_open) {
            int poll_result = poll(&child_out, 1, 10);
            if (poll_result > 0 && (child_out.revents & POLLIN)) {
                char buffer[1024];
                ssize_t read_len = read(stdout_pipe[0], buffer, sizeof(buffer));
                if (read_len > 0) {
                    session_channel_write(ctx, buffer, (size_t)read_len);
                } else {
                    output_open = false;
                    close(stdout_pipe[0]);
                    stdout_pipe[0] = -1;
                }
            } else if (poll_result < 0 && errno != EINTR) {
                output_open = false;
                close(stdout_pipe[0]);
                stdout_pipe[0] = -1;
            } else if (child_out.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                output_open = false;
                close(stdout_pipe[0]);
                stdout_pipe[0] = -1;
            }
        }

        if (input_open) {
            if (!session_transport_is_open(ctx) || session_transport_is_eof(ctx)) {
                input_open = false;
                close(stdin_pipe[1]);
                stdin_pipe[1] = -1;
                continue;
            }

            char input[512];
            int received = session_transport_read(ctx, input, sizeof(input), 10);
            if (received > 0) {
                ssize_t sent = write(stdin_pipe[1], input, (size_t)received);
                if (sent != received) {
                    input_open = false;
                    close(stdin_pipe[1]);
                    stdin_pipe[1] = -1;
                }
            } else if (received == SSH_AGAIN) {
                // no input available yet
            } else if (received < 0 || session_transport_is_eof(ctx) ||
                       !session_transport_is_open(ctx)) {
                input_open = false;
                close(stdin_pipe[1]);
                stdin_pipe[1] = -1;
            }
        }

        int status = 0;
        pid_t wait_result = waitpid(pid, &status, WNOHANG);
        if (wait_result == pid) {
            output_open = false;
            input_open = false;
            if (stdout_pipe[0] >= 0) {
                close(stdout_pipe[0]);
                stdout_pipe[0] = -1;
            }
            if (stdin_pipe[1] >= 0) {
                close(stdin_pipe[1]);
                stdin_pipe[1] = -1;
            }
            break;
        }
    }

    int status = 0;
    (void)waitpid(pid, &status, 0);
    session_send_system_line(ctx, "Shell session ended.");
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
    const int range_pair_precision =
        (int)((sizeof(range_label) > 3U)
                  ? ((sizeof(range_label) - 3U) / 2U)
                  : (sizeof(range_label) - 1U));
    const int range_single_precision =
        (int)((sizeof(range_label) > 2U) ? (sizeof(range_label) - 2U)
                                         : (sizeof(range_label) - 1U));
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
        snprintf(range_label, sizeof(range_label), "#%.*s-#%.*s",
                 range_pair_precision, first_label, range_pair_precision,
                 last_label);
    } else {
        snprintf(range_label, sizeof(range_label), "#%.*s",
                 range_single_precision, first_label);
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
    enum {
        SESSION_POLL_TEXT_PREC = SSH_CHATTER_MESSAGE_LIMIT - 1,
        SESSION_POLL_NOTICE_USER_PREC = SSH_CHATTER_USERNAME_LEN - 1,
        SESSION_POLL_NOTICE_QUESTION_PREC = SSH_CHATTER_MESSAGE_LIMIT / 2
    };
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
             "%.*s", SESSION_POLL_TEXT_PREC, question);
    for (size_t idx = 0U; idx < option_count; ++idx) {
        snprintf(ctx->owner->poll.options[idx].text,
                 sizeof(ctx->owner->poll.options[idx].text), "%.*s",
                 SESSION_POLL_TEXT_PREC, options[idx]);
        ctx->owner->poll.options[idx].votes = 0U;
    }
    host_vote_state_save_locked(ctx->owner);
    snapshot = ctx->owner->poll;
    ttak_mutex_unlock(&ctx->owner->lock);

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice), "* [%.*s] started a poll: %.*s",
             SESSION_POLL_NOTICE_USER_PREC, ctx->user.name,
             SESSION_POLL_NOTICE_QUESTION_PREC, question);
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

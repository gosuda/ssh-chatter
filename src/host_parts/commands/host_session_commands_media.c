/**
 * @file host_session_commands.c
 * @desc File-level documentation for host_session_commands.c, describing its role
 *       in the SSH-Chatter server and providing a consistent header
 *       comment format across C sources.
 * @return None.
 */

#include "../host_internal.h"
#include "ssh_chatter/security_layer.h"
#include <pty.h>
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
        ctx, "SSH/SCP users: scp <file> user@host:/name (root is CHATTER_FILESTORE_PATH) | TELNET users: "
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

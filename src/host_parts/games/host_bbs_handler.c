/**
 * @file host_bbs_and_games.c
 * @desc File-level documentation for host_bbs_and_games.c, describing its role
 *       in the SSH-Chatter server and providing a consistent header
 *       comment format across C sources.
 * @return None.
 */

// BBS workflows plus interactive mini-games.
#include "../host_internal.h"

static int session_game_random_range(session_ctx_t *ctx, int max);
static session_ctx_t *chat_room_find_user(chat_room_t *room,
                                          const char *username);

// Handle the /bbs command entry point.
static void session_handle_bbs(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (arguments == nullptr || *arguments == '\0') {
        session_bbs_show_dashboard(ctx);
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);
    if (working[0] == '\0') {
        session_bbs_show_dashboard(ctx);
        return;
    }

    char *command = working;
    char *rest = nullptr;
    for (char *cursor = working; *cursor != '\0'; ++cursor) {
        if (isspace((unsigned char)*cursor)) {
            *cursor = '\0';
            rest = cursor + 1;
            break;
        }
    }
    if (rest != nullptr) {
        trim_whitespace_inplace(rest);
    }

    if (strcmp(command, "exit") == 0) {
        ctx->in_bbs_mode = false;
        ctx->bbs_post_pending = false;
        ctx->bbs_view_active = false;
        ctx->bbs_view_post_id = 0U;
        ctx->bbs_view_scroll_offset = 0U;
        ctx->bbs_view_total_lines = 0U;
        ctx->bbs_rendering_editor = false;
        session_bbs_workspace_release(ctx);
        session_send_system_line(ctx, "Exited BBS mode.");
        return;
    }

    ctx->in_bbs_mode = true;

    const char *canonical_command =
        session_bbs_subcommand_canonicalize(ctx, command);
    if (canonical_command == nullptr) {
        session_send_system_line(
            ctx, "Unknown /bbs subcommand. Try /bbs for usage.");
        return;
    }

    if (strcmp(canonical_command, "list") == 0) {
        session_bbs_prepare_canvas(ctx);
        session_bbs_list(ctx);
    } else if (strcmp(canonical_command, "read") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_bbs_send_usage(ctx, "read", "<id>");
            return;
        }
        uint64_t id = (uint64_t)strtoull(rest, nullptr, 10);
        session_bbs_read(ctx, id);
    } else if (strcmp(canonical_command, "topic") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_bbs_send_usage(ctx, "topic", "read <tag>");
            return;
        }

        char topic_full[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(topic_full, sizeof(topic_full), "%s", rest);
        trim_whitespace_inplace(topic_full);
        if (topic_full[0] == '\0') {
            session_bbs_send_usage(ctx, "topic", "read <tag>");
            return;
        }

        char topic_args[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(topic_args, sizeof(topic_args), "%s", topic_full);

        char action_token[32];
        size_t action_len = 0U;
        char *cursor = topic_args;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
            if (action_len + 1U < sizeof(action_token)) {
                action_token[action_len++] = *cursor;
            }
            ++cursor;
        }
        action_token[action_len] = '\0';

        char *remaining = nullptr;
        if (*cursor != '\0') {
            *cursor = '\0';
            remaining = cursor + 1;
            trim_whitespace_inplace(remaining);
        }

        if (action_token[0] != '\0') {
            const char *canonical_action =
                session_bbs_subcommand_canonicalize(ctx, action_token);
            if (canonical_action != nullptr &&
                strcmp(canonical_action, "read") == 0) {
                if (remaining == nullptr || remaining[0] == '\0') {
                    session_bbs_send_usage(ctx, "topic", "read <tag>");
                    return;
                }
                session_bbs_prepare_canvas(ctx);
                session_bbs_list_topic(ctx, remaining);
                return;
            }
        }

        session_bbs_prepare_canvas(ctx);
        session_bbs_list_topic(ctx, topic_full);
    } else if (strcmp(canonical_command, "post") == 0) {
        session_bbs_begin_post(ctx, rest);
    } else if (strcmp(canonical_command, "edit") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_bbs_send_usage(ctx, "edit", "<id>");
            return;
        }
        uint64_t id = (uint64_t)strtoull(rest, nullptr, 10);
        session_bbs_begin_edit(ctx, id);
    } else if (strcmp(canonical_command, "comment") == 0) {
        session_bbs_add_comment(ctx, rest);
    } else if (strcmp(canonical_command, "regen") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_bbs_send_usage(ctx, "regen", "<id>");
            return;
        }
        uint64_t id = (uint64_t)strtoull(rest, nullptr, 10);
        session_bbs_regen_post(ctx, id);
    } else if (strcmp(canonical_command, "delete") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_bbs_send_usage(ctx, "delete", "<id>");
            return;
        }
        uint64_t id = (uint64_t)strtoull(rest, nullptr, 10);
        session_bbs_delete(ctx, id);
    } else if (strcmp(canonical_command, "door") == 0) {
        /* `/bbs door` lists doors; `/bbs door <name>` launches one. */
        const char *door_name = (rest != nullptr && rest[0] != '\0') ? rest
                                                                     : nullptr;
        session_bbs_door_run(ctx, door_name);
    } else {
        session_send_system_line(
            ctx, "Unknown /bbs subcommand. Try /bbs for usage.");
    }
}

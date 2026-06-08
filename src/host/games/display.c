
static int session_channel_read_poll(session_ctx_t *ctx, char *buffer,
                                     size_t length, int timeout_ms)
{
    if (ctx == nullptr || buffer == nullptr || length == 0U ||
        !session_transport_active(ctx)) {
        return SSH_ERROR;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        int result = session_transport_read(ctx, buffer, length, timeout_ms);
        if (result == SSH_AGAIN) {
            return SESSION_CHANNEL_TIMEOUT;
        }
        return result;
    }

    int fd = ssh_get_fd(ctx->session);
    if (fd < 0) {
        return session_transport_read(ctx, buffer, length, -1);
    }

    int val = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) < 0) {
        fprintf(stderr, "[session] setsockopt SO_KEEPALIVE failed");
    }

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    for (;;) {
        int poll_result = poll(&pfd, 1, timeout_ms);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SSH_ERROR;
        }
        if (poll_result == 0) {
            return SESSION_CHANNEL_TIMEOUT;
        }
        break;
    }

    if ((pfd.revents & POLLIN) == 0) {
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return 0;
        }
        return SESSION_CHANNEL_TIMEOUT;
    }

    return session_transport_read(ctx, buffer, length, -1);
}

static bool session_parse_color_arguments(char *working, char **tokens,
                                          size_t max_tokens,
                                          size_t *token_count)
{
    if (working == nullptr || tokens == nullptr || token_count == nullptr) {
        return false;
    }

    *token_count = 0U;
    bool extra_tokens = false;
    char *cursor = working;
    while (cursor != nullptr) {
        char *next = strchr(cursor, ';');
        if (next != nullptr) {
            *next = '\0';
        }

        trim_whitespace_inplace(cursor);
        if (cursor[0] == '\0') {
            return false;
        }

        if (*token_count < max_tokens) {
            tokens[*token_count] = cursor;
            ++(*token_count);
        } else if (cursor[0] != '\0') {
            extra_tokens = true;
        }

        if (next == nullptr) {
            break;
        }

        cursor = next + 1;
        if (cursor[0] == '\0') {
            extra_tokens = true;
            break;
        }
    }

    return !extra_tokens;
}

#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>

#include "ssh_chatter/abstract_byte_buffer.h"

static bool session_valid_ansi_256_sequence(const char *sequence)
{
    if (sequence == NULL) {
        return false;
    }

    size_t length = strnlen(sequence, SSH_CHATTER_COLOR_CODE_LEN);
    if (length >= SSH_CHATTER_COLOR_CODE_LEN) {
        return false;
    }

    size_t idx = 0;
    bool found_escape = false;
    while (idx < length) {
        if (sequence[idx] != '\x1b') {
            idx++;
            continue;
        }

        found_escape = true;

        if (idx + 1 >= length || sequence[idx + 1] != '[') {
            return false;
        }

        size_t end = idx + 2;
        bool found_terminator = false;

        while (end < length) {
            char ch = sequence[end];

            if (ch == 'm') {
                found_terminator = true;
                break;
            }

            if (!isdigit((unsigned char)ch) && ch != ';') {
                return false;
            }
            end++;
        }

        if (!found_terminator) {
            return false;
        }

        size_t param_len = end - (idx + 2);

        if (param_len > 64) {
            return false;
        }

        if (param_len > 0) {
            char buffer[65];
            memcpy(buffer, sequence + idx + 2, param_len);
            buffer[param_len] = '\0';

            char *fg_ptr = strstr(buffer, "38;5;");
            char *bg_ptr = strstr(buffer, "48;5;");
            char *fg_truecolor_ptr = strstr(buffer, "38;2;");
            char *bg_truecolor_ptr = strstr(buffer, "48;2;");
            char *target = (fg_ptr) ? fg_ptr : bg_ptr;
            char *truecolor_target =
                (fg_truecolor_ptr) ? fg_truecolor_ptr : bg_truecolor_ptr;

            if (target != NULL) {
                target += 5;

                char *endptr = NULL;
                long val = strtol(target, &endptr, 10);
                if (val < 0 || val > 255 || target == endptr) {
                    return false;
                }
            }

            if (truecolor_target != NULL) {
                truecolor_target += 5;
                for (int component = 0; component < 3; ++component) {
                    char *endptr = NULL;
                    long val = strtol(truecolor_target, &endptr, 10);
                    if (val < 0 || val > 255 || truecolor_target == endptr) {
                        return false;
                    }

                    if (component < 2) {
                        if (endptr == NULL || *endptr != ';') {
                            return false;
                        }
                        truecolor_target = endptr + 1;
                    }
                }
            }
        }
        idx = end + 1;
    }
    return found_escape;
}

static bool session_translate_escape_sequences(const char *input, char *output,
                                               size_t output_size)
{
    if (input == NULL || output == NULL || output_size == 0U) {
        return false;
    }

    const char *cursor = input;
    char *out = output;
    char *out_limit = output + output_size - 1U;

    while (*cursor != '\0') {
        if (out >= out_limit) {
            return false;
        }

        if (*cursor == '\\') {
            if (strncmp(cursor, "\\033", 4U) == 0 ||
                strncmp(cursor, "\\x1b", 4U) == 0 ||
                strncmp(cursor, "\\x1B", 4U) == 0) {
                *out++ = '\x1b';
                cursor += 4U;
                continue;
            }

            if (strncmp(cursor, "\\u001b", 6U) == 0 ||
                strncmp(cursor, "\\u001B", 6U) == 0) {
                *out++ = '\x1b';
                cursor += 6U;
                continue;
            }

            if (cursor[1] == 'e' || cursor[1] == 'E') {
                *out++ = '\x1b';
                cursor += 2U;
                continue;
            }
        }

        *out++ = *cursor++;
    }

    *out = '\0';
    return true;
}

static void session_handle_color(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    static const char *kColorUsage =
        "Usage: /color (text;highlight[;bold]) or /color advanced <ansi>";

    if (arguments == nullptr) {
        session_send_system_line(ctx, kColorUsage);
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, kColorUsage);
        return;
    }

    bool had_parentheses = false;
    if (working[0] == '(') {
        had_parentheses = true;
        memmove(working, working + 1, strlen(working));
        trim_whitespace_inplace(working);
    }

    if (had_parentheses) {
        size_t len = strlen(working);
        if (len == 0U || working[len - 1U] != ')') {
            session_send_system_line(ctx,
                                     "Usage: /color (text;highlight[;bold])");
            return;
        }
        working[len - 1U] = '\0';
        trim_whitespace_inplace(working);
    }

    if (working[0] == '\0') {
        session_send_system_line(ctx, kColorUsage);
        return;
    }

    if (strncasecmp(working, "advanced", strlen("advanced")) == 0) {
        const char *raw_code = working + strlen("advanced");
        while (raw_code[0] != '\0' && isspace((unsigned char)raw_code[0])) {
            ++raw_code;
        }

        if (raw_code[0] == '\0') {
            session_send_system_line(ctx, kColorUsage);
            return;
        }

        char translated_name[SSH_CHATTER_USERNAME_LEN];
        if (!session_translate_escape_sequences(raw_code, translated_name,
                                                sizeof(translated_name)) ||
            !session_valid_ansi_256_sequence(translated_name)) {
            session_send_system_line(ctx, "Invalid ANSI/256 expression. Use "
                                          "sequences like \\x1b[38;5;196m.");
            return;
        }

        if (!ctx->user_data_loaded) {
            (void)session_user_data_load(ctx);
        }

        snprintf(ctx->user_data.preferred_nickname,
                 sizeof(ctx->user_data.preferred_nickname), "%s",
                 translated_name);

        if (ctx->owner != nullptr && ctx->owner->user_data_root[0] != '\0' &&
            ctx->user_data.username[0] != '\0') {
            user_data_save(ctx->owner->user_data_root, &ctx->user_data,
                           ctx->client_ip);
        }

        ctx->user_color_code[0] = '\0';
        ctx->user_highlight_code[0] = '\0';
        ctx->user_is_bold = false;

        session_send_system_line(
            ctx, "Display name updated with advanced ANSI formatting.");

        char preview[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(preview, sizeof(preview), "%s%s", translated_name, ANSI_RESET);
        session_send_line(ctx, preview);

        return;
    }

    if (working[0] == '\0') {
        session_send_system_line(ctx, kColorUsage);
        return;
    }

    char *tokens[3] = {0};
    size_t token_count = 0U;
    if (!session_parse_color_arguments(working, tokens, 3U, &token_count) ||
        token_count < 2U) {
        session_send_system_line(ctx, kColorUsage);
        return;
    }

    const char *text_code = lookup_color_code(
        USER_COLOR_MAP, sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
        tokens[0]);
    if (text_code == nullptr) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "Unknown text color '%s'.",
                 tokens[0]);
        session_send_system_line(ctx, message);
        return;
    }

    const char *highlight_code = lookup_color_code(
        HIGHLIGHT_COLOR_MAP,
        sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
        tokens[1]);
    if (highlight_code == nullptr) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "Unknown highlight color '%s'.",
                 tokens[1]);
        session_send_system_line(ctx, message);
        return;
    }

    bool is_bold = false;
    if (token_count == 3U) {
        if (!parse_bool_token(tokens[2], &is_bold)) {
            session_send_system_line(
                ctx,
                "The third value must describe bold (ex: bold, true, normal).");
            return;
        }
    }

    snprintf(ctx->user_color_code, sizeof(ctx->user_color_code), "%s",
             text_code);
    snprintf(ctx->user_highlight_code, sizeof(ctx->user_highlight_code), "%s",
             highlight_code);
    ctx->user_is_bold = is_bold;
    snprintf(ctx->user_color_name, sizeof(ctx->user_color_name), "%s",
             tokens[0]);
    snprintf(ctx->user_highlight_name, sizeof(ctx->user_highlight_name), "%s",
             tokens[1]);

    char info[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(info, sizeof(info),
             "Handle colors updated: text=%s highlight=%s bold=%s", tokens[0],
             tokens[1], is_bold ? "on" : "off");
    session_send_system_line(ctx, info);

    const char *bold_code = is_bold ? ANSI_BOLD : "";
    char preview[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(preview, sizeof(preview), "%s%s%s[%s] preview%s", highlight_code,
             bold_code, text_code, ctx->user.name, ANSI_RESET);
    session_send_line(ctx, preview);

    if (ctx->owner != nullptr) {
        host_store_user_theme(ctx->owner, ctx);
    }
}

static bool session_fixnick_extract_plain_name(const char *source,
                                               char *plain_name,
                                               size_t plain_name_len)
{
    if (plain_name == nullptr || plain_name_len == 0U) {
        return false;
    }

    plain_name[0] = '\0';
    if (source == nullptr || source[0] == '\0') {
        return false;
    }

    if (!user_data_strip_ansi_sequences(source, plain_name, plain_name_len)) {
        return false;
    }

    trim_whitespace_inplace(plain_name);
    return plain_name[0] != '\0';
}

static void session_fixnick_clear_user_theme_state(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    ctx->user_color_code[0] = '\0';
    ctx->user_highlight_code[0] = '\0';
    ctx->user_color_name[0] = '\0';
    ctx->user_highlight_name[0] = '\0';
    ctx->user_is_bold = false;

    if (ctx->user_data_loaded) {
        ctx->user_data.has_user_theme = 0U;
        ctx->user_data.user_is_bold = 0U;
        ctx->user_data.user_color_code[0] = '\0';
        ctx->user_data.user_highlight_code[0] = '\0';
        ctx->user_data.user_color_name[0] = '\0';
        ctx->user_data.user_highlight_name[0] = '\0';
    }

    if (ctx->owner != nullptr) {
        ttak_mutex_lock(&ctx->owner->lock);
        user_preference_t *pref =
            host_find_preference_locked(ctx->owner, ctx->user.name, "");
        if (pref != nullptr) {
            pref->has_user_theme = false;
            pref->user_is_bold = false;
            pref->user_color_code[0] = '\0';
            pref->user_highlight_code[0] = '\0';
            pref->user_color_name[0] = '\0';
            pref->user_highlight_name[0] = '\0';
        }
        host_state_save_locked(ctx->owner);
        ttak_mutex_unlock(&ctx->owner->lock);
    }
}

static void session_handle_fixnick(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (arguments != nullptr && arguments[0] != '\0') {
        session_send_system_line(ctx, "Usage: /fixnick");
        return;
    }

    if (!session_user_data_load(ctx)) {
        session_send_system_line(ctx, "Unable to load user data.");
        return;
    }

    if (!user_data_has_password(&ctx->user_data) ||
        !user_data_reserved_nickname_is_ip_wide(&ctx->user_data)) {
        session_send_system_line(
            ctx, "/fixnick is only available for IP-wide reserved nicknames.");
        return;
    }

    char fixed_nickname[SSH_CHATTER_USERNAME_LEN];
    char visible_name[SSH_CHATTER_USERNAME_LEN];
    fixed_nickname[0] = '\0';
    visible_name[0] = '\0';

    const char *preferred = ctx->user_data.preferred_nickname;
    if (preferred[0] != '\0' && session_valid_ansi_256_sequence(preferred)) {
        snprintf(fixed_nickname, sizeof(fixed_nickname), "%s", preferred);
        if (!session_fixnick_extract_plain_name(preferred, visible_name,
                                                sizeof(visible_name))) {
            snprintf(visible_name, sizeof(visible_name), "%s", ctx->user.name);
        }
    } else {
        const char *source_name =
            preferred[0] != '\0' ? preferred : ctx->user.name;
        if (!session_fixnick_extract_plain_name(source_name, visible_name,
                                                sizeof(visible_name))) {
            snprintf(visible_name, sizeof(visible_name), "%s", ctx->user.name);
            trim_whitespace_inplace(visible_name);
        }

        if (ctx->user_color_code[0] == '\0' &&
            ctx->user_highlight_code[0] == '\0' && !ctx->user_is_bold) {
            session_send_system_line(
                ctx, "No nickname color state is active. Use /color first.");
            return;
        }

        snprintf(fixed_nickname, sizeof(fixed_nickname), "%s%s%s%s%s",
                 ctx->user_highlight_code, ctx->user_color_code,
                 ctx->user_is_bold ? ANSI_BOLD : "", visible_name,
                 ANSI_RESET);
    }

    trim_whitespace_inplace(visible_name);
    if (visible_name[0] == '\0') {
        session_send_system_line(ctx, "Unable to derive a valid nickname.");
        return;
    }

    snprintf(ctx->user_data.preferred_nickname,
             sizeof(ctx->user_data.preferred_nickname), "%s", fixed_nickname);

    user_data_set_fixnick_enabled(&ctx->user_data, true);

    session_fixnick_clear_user_theme_state(ctx);

    if (!session_user_data_commit(ctx)) {
        session_send_system_line(ctx, "Failed to persist fixed nickname.");
        return;
    }

    if (ctx->owner != nullptr && ctx->owner->pw_auth_file_path[0] != '\0') {
        (void)session_pw_auth_update(
            ctx->owner, ctx->user.name, ctx->user_data.password_salt,
            sizeof(ctx->user_data.password_salt), ctx->user_data.password_hash,
            sizeof(ctx->user_data.password_hash),
            user_data_reserved_nickname_is_ip_wide(&ctx->user_data), true,
            ctx->client_ip, true);
    }

    session_send_system_line(
        ctx, "Nickname colors fixed and stored for all IP addresses.");

    char preview[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(preview, sizeof(preview), "%s%s", fixed_nickname, ANSI_RESET);
    session_send_line(ctx, preview);
}

static void session_handle_motd(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    host_refresh_motd(ctx->owner);

    ttak_mutex_lock(&ctx->owner->lock);
    const char *motd_to_display = ctx->owner->motd;
    ttak_mutex_unlock(&ctx->owner->lock);

    if (motd_to_display[0] != '\0') {
        session_send_raw_text(ctx, motd_to_display);
    } else {
        session_send_system_line(ctx, "No message of the day configured.");
    }
}

static void session_handle_system_color(session_ctx_t *ctx,
                                        const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    static const char *kUsage =
        "Usage: /systemcolor (fg;background[;highlight][;bold]) or "
        "/systemcolor reset - third value may be highlight or "
        "bold.";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/systemcolor", kUsage, usage,
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

    bool had_parentheses = false;
    if (working[0] == '(') {
        had_parentheses = true;
        memmove(working, working + 1, strlen(working));
        trim_whitespace_inplace(working);
    }

    if (had_parentheses) {
        size_t len = strlen(working);
        if (len == 0U || working[len - 1U] != ')') {
            session_send_system_line(ctx, usage);
            return;
        }
        working[len - 1U] = '\0';
        trim_whitespace_inplace(working);
    }

    if (working[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    if (strcasecmp(working, "reset") == 0) {
        session_apply_system_theme_defaults(ctx);
        session_send_system_line(ctx, "System colors reset to defaults.");
        session_render_separator(ctx, "Chatroom");
        session_render_prompt(ctx, true);
        if (ctx->owner != nullptr) {
            host_store_system_theme(ctx->owner, ctx);
        }
        return;
    }

    char *tokens[4] = {0};
    size_t token_count = 0U;
    if (!session_parse_color_arguments(working, tokens, 4U, &token_count) ||
        token_count < 2U) {
        session_send_system_line(ctx, usage);
        return;
    }

    const char *fg_code = lookup_color_code(
        USER_COLOR_MAP, sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
        tokens[0]);
    if (fg_code == nullptr) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "Unknown foreground color '%s'.",
                 tokens[0]);
        session_send_system_line(ctx, message);
        return;
    }

    const char *bg_code = lookup_color_code(HIGHLIGHT_COLOR_MAP,
                                            sizeof(HIGHLIGHT_COLOR_MAP) /
                                                sizeof(HIGHLIGHT_COLOR_MAP[0]),
                                            tokens[1]);
    if (bg_code == nullptr) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "Unknown background color '%s'.",
                 tokens[1]);
        session_send_system_line(ctx, message);
        return;
    }

    const char *highlight_code = ctx->system_highlight_code;
    bool highlight_updated = false;
    bool is_bold = ctx->system_is_bold;
    if (token_count >= 3U) {
        bool bool_value = false;
        if (parse_bool_token(tokens[2], &bool_value)) {
            if (token_count > 3U) {
                session_send_system_line(ctx, usage);
                return;
            }
            is_bold = bool_value;
        } else {
            highlight_code = lookup_color_code(
                HIGHLIGHT_COLOR_MAP,
                sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
                tokens[2]);
            if (highlight_code == nullptr) {
                char message[SSH_CHATTER_MESSAGE_LIMIT];
                snprintf(message, sizeof(message),
                         "Unknown highlight color '%s'.", tokens[2]);
                session_send_system_line(ctx, message);
                return;
            }
            highlight_updated = true;

            if (token_count == 4U) {
                if (!parse_bool_token(tokens[3], &bool_value)) {
                    session_send_system_line(ctx,
                                             "The last value must describe "
                                             "bold (ex: bold, true, normal).");
                    return;
                }
                is_bold = bool_value;
            }
        }
    }

    ctx->system_fg_code = fg_code;
    ctx->system_bg_code = bg_code;
    ctx->system_highlight_code = highlight_code;
    ctx->system_is_bold = is_bold;
    snprintf(ctx->system_fg_name, sizeof(ctx->system_fg_name), "%s", tokens[0]);
    snprintf(ctx->system_bg_name, sizeof(ctx->system_bg_name), "%s", tokens[1]);
    if (highlight_updated) {
        snprintf(ctx->system_highlight_name, sizeof(ctx->system_highlight_name),
                 "%s", tokens[2]);
    }

    session_force_dark_mode_foreground(ctx);
    session_apply_background_fill(ctx);

    session_send_system_line(ctx, "System colors updated.");
    session_render_separator(ctx, "Chatroom");
    session_render_prompt(ctx, true);
    if (ctx->owner != nullptr) {
        host_store_system_theme(ctx->owner, ctx);
    }
}

static void session_handle_set_trans_lang(session_ctx_t *ctx,
                                          const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    char working[SSH_CHATTER_LANG_NAME_LEN];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
    }
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, "Usage: /set-trans-lang <language|off>");
        return;
    }

    if (session_argument_is_disable(working)) {
        ctx->output_translation_enabled = false;
        ctx->output_translation_language[0] = '\0';
        session_translation_clear_queue(ctx);
        session_send_system_line(ctx, "Terminal translation disabled.");
        if (ctx->owner != nullptr) {
            host_store_translation_preferences(ctx->owner, ctx);
        }
        return;
    }

    if (session_language_equals(ctx->output_translation_language, working)) {
        snprintf(ctx->output_translation_language,
                 sizeof(ctx->output_translation_language), "%s", working);
        ctx->output_translation_enabled = true;
        session_translation_clear_queue(ctx);

        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message),
                 "Terminal output will continue to be translated to %s.",
                 ctx->output_translation_language);
        session_send_system_line(ctx, message);
        if (!ctx->translation_enabled) {
            session_send_system_line(ctx, "Translation is currently disabled; "
                                          "enable it with /translate on.");
        }
        if (ctx->owner != nullptr) {
            host_store_translation_preferences(ctx->owner, ctx);
        }
        return;
    }

    char preview[SSH_CHATTER_MESSAGE_LIMIT];
    char detected[SSH_CHATTER_LANG_NAME_LEN];
    if (!translator_translate("Terminal messages will be translated for you.",
                              working, preview, sizeof(preview), detected,
                              sizeof(detected))) {
        const char *error = translator_last_error();
        if (error != nullptr && *error != '\0') {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message), "Translation service error: %s",
                     error);
            session_send_system_line(ctx, message);
        } else {
            session_send_system_line(ctx, "Failed to reach the translation "
                                          "service. Please try again later.");
        }
        return;
    }

    snprintf(ctx->output_translation_language,
             sizeof(ctx->output_translation_language), "%s", working);
    ctx->output_translation_enabled = true;
    session_translation_clear_queue(ctx);

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    int preview_limit = (int)(sizeof(message) / 2);
    if (preview_limit <= 0) {
        preview_limit = (int)sizeof(message) - 1;
    }
    int detected_limit = (int)sizeof(detected) - 1;
    if (detected_limit <= 0) {
        detected_limit = (int)sizeof(detected);
    }
    if (detected[0] != '\0') {
        snprintf(message, sizeof(message),
                 "Terminal output will be translated to %s. Sample: %.*s "
                 "(detected: %.*s).",
                 ctx->output_translation_language, preview_limit, preview,
                 detected_limit, detected);
    } else {
        snprintf(message, sizeof(message),
                 "Terminal output will be translated to %s. Sample: %.*s.",
                 ctx->output_translation_language, preview_limit, preview);
    }
    session_send_system_line(ctx, message);
    if (!ctx->translation_enabled) {
        session_send_system_line(
            ctx,
            "Translation is currently disabled; enable it with /translate on.");
    }
    if (ctx->owner != nullptr) {
        host_store_translation_preferences(ctx->owner, ctx);
    }
}

static void session_handle_set_target_lang(session_ctx_t *ctx,
                                           const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    char working[SSH_CHATTER_LANG_NAME_LEN];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
    }
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, "Usage: /set-target-lang <language|off>");
        return;
    }

    if (session_argument_is_disable(working)) {
        ctx->input_translation_enabled = false;
        ctx->input_translation_language[0] = '\0';
        ctx->last_detected_input_language[0] = '\0';
        session_send_system_line(ctx, "Outgoing message translation disabled.");
        if (ctx->owner != nullptr) {
            host_store_translation_preferences(ctx->owner, ctx);
        }
        return;
    }

    if (session_language_equals(ctx->input_translation_language, working)) {
        snprintf(ctx->input_translation_language,
                 sizeof(ctx->input_translation_language), "%s", working);
        ctx->input_translation_enabled = true;
        ctx->last_detected_input_language[0] = '\0';

        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message),
                 "Outgoing messages will continue to be translated to %s.",
                 ctx->input_translation_language);
        session_send_system_line(ctx, message);
        if (!ctx->translation_enabled) {
            session_send_system_line(ctx, "Translation is currently disabled; "
                                          "enable it with /translate on.");
        }
        if (ctx->owner != nullptr) {
            host_store_translation_preferences(ctx->owner, ctx);
        }
        return;
    }

    char preview[SSH_CHATTER_MESSAGE_LIMIT];
    char detected[SSH_CHATTER_LANG_NAME_LEN];
    if (!translator_translate(
            "Your messages will be translated before broadcasting.", working,
            preview, sizeof(preview), detected, sizeof(detected))) {
        const char *error = translator_last_error();
        if (error != nullptr && *error != '\0') {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message), "Translation service error: %s",
                     error);
            session_send_system_line(ctx, message);
        } else {
            session_send_system_line(ctx, "Failed to reach the translation "
                                          "service. Please try again later.");
        }
        return;
    }

    snprintf(ctx->input_translation_language,
             sizeof(ctx->input_translation_language), "%s", working);
    ctx->input_translation_enabled = true;
    ctx->last_detected_input_language[0] = '\0';

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    int preview_limit = (int)(sizeof(message) / 2);
    if (preview_limit <= 0) {
        preview_limit = (int)sizeof(message) - 1;
    }
    int detected_limit = (int)sizeof(detected) - 1;
    if (detected_limit <= 0) {
        detected_limit = (int)sizeof(detected);
    }
    if (detected[0] != '\0') {
        snprintf(message, sizeof(message),
                 "Outgoing messages will be translated to %s. Sample: %.*s "
                 "(detected: %.*s).",
                 ctx->input_translation_language, preview_limit, preview,
                 detected_limit, detected);
    } else {
        snprintf(message, sizeof(message),
                 "Outgoing messages will be translated to %s. Sample: %.*s.",
                 ctx->input_translation_language, preview_limit, preview);
    }
    session_send_system_line(ctx, message);
    if (!ctx->translation_enabled) {
        session_send_system_line(
            ctx,
            "Translation is currently disabled; enable it with /translate on.");
    }
    if (ctx->owner != nullptr) {
        host_store_translation_preferences(ctx->owner, ctx);
    }
}

static void session_handle_chat_spacing(session_ctx_t *ctx,
                                        const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    const session_ui_locale_t *locale = session_ui_get_locale(ctx);
    const char *prefix = session_command_prefix(ctx);
    const char *usage_format = (locale->chat_spacing_usage != nullptr &&
                                locale->chat_spacing_usage[0] != '\0')
                                   ? locale->chat_spacing_usage
                                   : "Usage: %schat-spacing <0-5>";

    char working[16];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
    }
    trim_whitespace_inplace(working);

    char usage_message[SSH_CHATTER_MESSAGE_LIMIT];
    const char *usage_args[] = {prefix};

    if (working[0] == '\0') {
        session_format_template(usage_format, usage_args,
                                sizeof(usage_args) / sizeof(usage_args[0]),
                                usage_message, sizeof(usage_message));
        session_send_system_line(ctx, usage_message);
        return;
    }

    char *endptr = nullptr;
    long value = strtol(working, &endptr, 10);
    if (endptr == working || (endptr != nullptr && *endptr != '\0') ||
        value < 0L || value > 5L) {
        session_format_template(usage_format, usage_args,
                                sizeof(usage_args) / sizeof(usage_args[0]),
                                usage_message, sizeof(usage_message));
        session_send_system_line(ctx, usage_message);
        return;
    }

    ctx->translation_caption_spacing = (size_t)value;

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    if (value == 0L) {
        const char *format =
            (locale->chat_spacing_immediate != nullptr &&
             locale->chat_spacing_immediate[0] != '\0')
                ? locale->chat_spacing_immediate
                : "Translation captions will appear immediately "
                  "without reserving extra blank lines.";
        session_format_template(format, nullptr, 0U, message, sizeof(message));
    } else if (value == 1L) {
        const char *format =
            (locale->chat_spacing_single != nullptr &&
             locale->chat_spacing_single[0] != '\0')
                ? locale->chat_spacing_single
                : "Translation captions will reserve 1 blank line "
                  "before appearing in chat threads.";
        session_format_template(format, nullptr, 0U, message, sizeof(message));
    } else {
        const char *format = (locale->chat_spacing_multiple != nullptr &&
                              locale->chat_spacing_multiple[0] != '\0')
                                 ? locale->chat_spacing_multiple
                                 : "Translation captions will reserve %s blank "
                                   "lines before appearing in chat threads.";
        char count[16];
        snprintf(count, sizeof(count), "%ld", value);
        const char *args[] = {count};
        session_format_template(format, args, sizeof(args) / sizeof(args[0]),
                                message, sizeof(message));
    }
    session_send_system_line(ctx, message);

    if (ctx->owner != nullptr) {
        host_store_chat_spacing(ctx->owner, ctx);
    }
}

static void session_handle_set_ui_lang(session_ctx_t *ctx,
                                       const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    const session_ui_locale_t *locale = session_ui_get_locale(ctx);
    const char *prefix = session_command_prefix(ctx);

    char token[SSH_CHATTER_LANG_NAME_LEN];
    const char *cursor = session_consume_token(arguments, token, sizeof(token));

    bool extra_tokens = cursor != nullptr && *cursor != '\0';
    if (token[0] == '\0' || extra_tokens) {
        const char *format = (locale->set_ui_lang_usage != nullptr &&
                              locale->set_ui_lang_usage[0] != '\0')
                                 ? locale->set_ui_lang_usage
                                 : "Usage: %sset-ui-lang <ko|en|jp|zh|ru|de|fr|pl>";
        const char *args[] = {prefix};
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        session_format_template(format, args, sizeof(args) / sizeof(args[0]),
                                message, sizeof(message));
        session_send_system_line(ctx, message);
        return;
    }

    if (strcasecmp(token, "unicode-all") == 0) {
        ctx->unicode_all_mode = true;
        session_send_system_line(ctx, "Unicode-all mode enabled. All languages are now visible.");
        return;
    }

    session_ui_language_t language = session_ui_language_from_code(token);
    if (language == SESSION_UI_LANGUAGE_COUNT) {
        const char *format =
            (locale->set_ui_lang_invalid != nullptr &&
             locale->set_ui_lang_invalid[0] != '\0')
                ? locale->set_ui_lang_invalid
                : "Unsupported language. Use one of: ko, en, jp, zh, ru, de, fr, pl.";
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        session_format_template(format, nullptr, 0U, message, sizeof(message));
        session_send_system_line(ctx, message);
        return;
    }

    ctx->ui_language = language;
    ctx->unicode_all_mode = false;
    const session_ui_locale_t *updated_locale = session_ui_get_locale(ctx);
    const char *language_name =
        session_ui_language_name(language, ctx->ui_language);
    const char *format =
        (updated_locale->set_ui_lang_success != nullptr &&
         updated_locale->set_ui_lang_success[0] != '\0')
            ? updated_locale->set_ui_lang_success
            : "UI language set to %s. Use %shelp to review commands.";
    const char *updated_prefix = session_command_prefix(ctx);

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    const char *args[] = {language_name != nullptr ? language_name : "-",
                          updated_prefix};
    session_format_template(format, args, sizeof(args) / sizeof(args[0]),
                            message, sizeof(message));
    session_send_system_line(ctx, message);

    if (ctx->owner != nullptr) {
        host_store_ui_language(ctx->owner, ctx);
    }
}

static void session_handle_mode(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    const session_ui_locale_t *locale = session_ui_get_locale(ctx);
    const char *prefix = session_command_prefix(ctx);
    const char *chat_label = (locale->mode_label_chat != nullptr &&
                              locale->mode_label_chat[0] != '\0')
                                 ? locale->mode_label_chat
                                 : "chat";
    const char *command_label = (locale->mode_label_command != nullptr &&
                                 locale->mode_label_command[0] != '\0')
                                    ? locale->mode_label_command
                                    : "command";

    char working[32];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
    }
    trim_whitespace_inplace(working);

    const char *status_format = (locale->mode_status_format != nullptr &&
                                 locale->mode_status_format[0] != '\0')
                                    ? locale->mode_status_format
                                    : "Current input mode: %s.";
    const char *explain_chat =
        (locale->mode_explain_chat != nullptr &&
         locale->mode_explain_chat[0] != '\0')
            ? locale->mode_explain_chat
            : "Chat mode: send messages normally. Prefix commands with %s.";
    const char *explain_command =
        (locale->mode_explain_command != nullptr &&
         locale->mode_explain_command[0] != '\0')
            ? locale->mode_explain_command
            : "Command mode: enter commands without %s, use UpArrow/DownArrow "
              "for history and Tab for completion.";
    const char *already_chat =
        (locale->mode_already_chat != nullptr &&
         locale->mode_already_chat[0] != '\0')
            ? locale->mode_already_chat
            : "Already in chat mode. Commands require the %s prefix.";
    const char *already_command =
        (locale->mode_already_command != nullptr &&
         locale->mode_already_command[0] != '\0')
            ? locale->mode_already_command
            : "Command mode already active. Enter commands without %s.";
    const char *enabled_chat =
        (locale->mode_enabled_chat != nullptr &&
         locale->mode_enabled_chat[0] != '\0')
            ? locale->mode_enabled_chat
            : "Chat mode enabled. Commands once again require the %s prefix.";
    const char *enabled_command =
        (locale->mode_enabled_command != nullptr &&
         locale->mode_enabled_command[0] != '\0')
            ? locale->mode_enabled_command
            : "Command mode enabled. Enter commands without %s; use "
              "UpArrow/DownArrow for history and Tab for completion.";
    const char *usage_format =
        (locale->mode_usage != nullptr && locale->mode_usage[0] != '\0')
            ? locale->mode_usage
            : "Usage: %smode <chat|command|toggle>";

    if (working[0] == '\0') {
        const char *current_label =
            (ctx->input_mode == SESSION_INPUT_MODE_COMMAND) ? command_label
                                                            : chat_label;
        char status_line[128];
        const char *status_args[] = {current_label};
        session_format_template(status_format, status_args,
                                sizeof(status_args) / sizeof(status_args[0]),
                                status_line, sizeof(status_line));
        session_send_system_line(ctx, status_line);

        const char *explain = (ctx->input_mode == SESSION_INPUT_MODE_COMMAND)
                                  ? explain_command
                                  : explain_chat;
        char explain_line[SSH_CHATTER_MESSAGE_LIMIT];
        const char *explain_args[] = {prefix};
        session_format_template(explain, explain_args,
                                sizeof(explain_args) / sizeof(explain_args[0]),
                                explain_line, sizeof(explain_line));
        session_send_system_line(ctx, explain_line);
        return;
    }

    const bool matches_chat = (strcasecmp(working, "chat") == 0) ||
                              (chat_label != nullptr && chat_label[0] != '\0' &&
                               strcmp(working, chat_label) == 0);
    if (matches_chat) {
        if (ctx->input_mode == SESSION_INPUT_MODE_CHAT) {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            const char *args[] = {prefix};
            session_format_template(already_chat, args,
                                    sizeof(args) / sizeof(args[0]), message,
                                    sizeof(message));
            session_send_system_line(ctx, message);
            return;
        }
        ctx->input_mode = SESSION_INPUT_MODE_CHAT;
        session_refresh_input_line(ctx);
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        const char *args[] = {prefix};
        session_format_template(enabled_chat, args,
                                sizeof(args) / sizeof(args[0]), message,
                                sizeof(message));
        session_send_system_line(ctx, message);
        return;
    }

    const bool matches_command =
        (strcasecmp(working, "command") == 0) ||
        (command_label != nullptr && command_label[0] != '\0' &&
         strcmp(working, command_label) == 0);
    if (matches_command) {
        if (ctx->input_mode == SESSION_INPUT_MODE_COMMAND) {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            const char *args[] = {prefix};
            session_format_template(already_command, args,
                                    sizeof(args) / sizeof(args[0]), message,
                                    sizeof(message));
            session_send_system_line(ctx, message);
            return;
        }
        ctx->input_mode = SESSION_INPUT_MODE_COMMAND;
        session_refresh_input_line(ctx);
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        const char *args[] = {prefix};
        session_format_template(enabled_command, args,
                                sizeof(args) / sizeof(args[0]), message,
                                sizeof(message));
        session_send_system_line(ctx, message);
        return;
    }

    if (strcasecmp(working, "toggle") == 0) {
        ctx->input_mode = (ctx->input_mode == SESSION_INPUT_MODE_COMMAND)
                              ? SESSION_INPUT_MODE_CHAT
                              : SESSION_INPUT_MODE_COMMAND;
        session_refresh_input_line(ctx);
        const char *format = (ctx->input_mode == SESSION_INPUT_MODE_COMMAND)
                                 ? enabled_command
                                 : enabled_chat;
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        const char *args[] = {prefix};
        session_format_template(format, args, sizeof(args) / sizeof(args[0]),
                                message, sizeof(message));
        session_send_system_line(ctx, message);
        return;
    }

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    const char *args[] = {prefix};
    session_format_template(usage_format, args, sizeof(args) / sizeof(args[0]),
                            message, sizeof(message));
    session_send_system_line(ctx, message);
}

static void session_handle_history(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    session_clear_screen(ctx);

    size_t command_indices[SSH_CHATTER_INPUT_HISTORY_LIMIT];
    size_t command_count = 0U;
    for (size_t idx = 0U; idx < ctx->input_history_count; ++idx) {
        if (ctx->input_history_is_command[idx]) {
            command_indices[command_count++] = idx;
        }
    }

    if (command_count == 0U) {
        session_send_system_line(ctx, "No command history recorded yet.");
        return;
    }

    size_t limit = command_count;
    if (arguments != nullptr) {
        char working[32];
        snprintf(working, sizeof(working), "%s", arguments);
        trim_whitespace_inplace(working);
        if (working[0] != '\0') {
            char *end = nullptr;
            errno = 0;
            long requested = strtol(working, &end, 10);
            if (errno != 0 || end == working || *end != '\0' ||
                requested <= 0) {
                session_send_system_line(ctx, "Usage: /history [count]");
                return;
            }
            if ((size_t)requested < limit) {
                limit = (size_t)requested;
            }
        }
    }

    session_send_system_line(ctx, "Command history (newest first):");

    for (size_t displayed = 0U; displayed < limit; ++displayed) {
        size_t source_index = command_indices[command_count - 1U - displayed];
        const char *entry = ctx->input_history[source_index];
        if (entry == nullptr || entry[0] == '\0') {
            continue;
        }
        char normalized[SSH_CHATTER_MAX_INPUT_LEN];
        normalized[0] = '\0';
        const char *prefix = session_command_prefix(ctx);
        const char *display_prefix =
            (prefix != nullptr && prefix[0] != '\0') ? prefix : "/";
        size_t prefix_len = strlen(display_prefix);
        bool has_prefix = false;
        if (prefix_len > 0U) {
            has_prefix = strncmp(entry, display_prefix, prefix_len) == 0;
        } else {
            has_prefix = entry[0] == '/';
        }

        if (!has_prefix) {
            snprintf(normalized, sizeof(normalized), "%s%s", display_prefix,
                     entry);
        } else {
            snprintf(normalized, sizeof(normalized), "%s", entry);
        }

        char line[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(line, sizeof(line), "%2zu. %s", displayed + 1U, normalized);
        session_send_system_line(ctx, line);
    }

    session_refresh_input_line(ctx);
}

typedef struct session_weather_buffer {
    sshc_abstract_byte_buffer_t bytes;
} session_weather_buffer_t;

static size_t session_weather_write_callback(void *contents, size_t size,
                                             size_t nmemb, void *userp)
{
    session_weather_buffer_t *buffer = (session_weather_buffer_t *)userp;
    const size_t total = size * nmemb;
    if (buffer == nullptr || total == 0U) {
        return 0U;
    }

    return sshc_abstract_byte_buffer_append(&buffer->bytes, contents, total)
               ? total
               : 0U;
}

static bool session_fetch_weather_summary(const char *city,
                                          char *summary, size_t summary_len)
{
    if (city == nullptr || summary == nullptr ||
        summary_len == 0U) {
        return false;
    }

    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        return false;
    }

    bool success = false;
    session_weather_buffer_t buffer = {0};
    sshc_abstract_byte_buffer_init(&buffer.bytes);
    char query[128];
    snprintf(query, sizeof(query), "%s", city);

    char *escaped = curl_easy_escape(curl, query, 0);
    if (escaped == nullptr) {
        goto cleanup;
    }

    char url[512];
    static const char *kFormat = "%25l:%20%25C,%20%25t";
    int written = snprintf(url, sizeof(url), "https://wttr.in/%s?format=%s",
                           escaped, kFormat);
    curl_free(escaped);
    if (written < 0 || (size_t)written >= sizeof(url)) {
        goto cleanup;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                     session_weather_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

    CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        goto cleanup;
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status < 200L || status >= 300L || buffer.bytes.storage == nullptr) {
        goto cleanup;
    }

    sshc_abstract_byte_buffer_view_t buffer_view = {0};
    if (!sshc_abstract_byte_buffer_map_cstr(&buffer.bytes, &buffer_view)) {
        goto cleanup;
    }

    char *trimmed = buffer_view.data;
    while (*trimmed != '\0' && isspace((unsigned char)*trimmed)) {
        ++trimmed;
    }
    size_t end = strlen(trimmed);
    while (end > 0U && isspace((unsigned char)trimmed[end - 1U])) {
        trimmed[--end] = '\0';
    }

    if (trimmed[0] == '\0') {
        sshc_abstract_byte_buffer_unmap(&buffer_view);
        goto cleanup;
    }

    snprintf(summary, summary_len, "%s", trimmed);
    success = true;
    sshc_abstract_byte_buffer_unmap(&buffer_view);

cleanup:
    sshc_abstract_byte_buffer_free(&buffer.bytes);
    curl_easy_cleanup(curl);
    return success;
}

static void session_handle_status(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    static const char *kUsage = "Usage: /status <message|off>";
    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/status", kUsage, usage, sizeof(usage));

    if (arguments == nullptr || *arguments == '\0') {
        if (ctx->status_message[0] == '\0') {
            session_send_system_line(ctx, "You do not have a status message.");
        } else {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message), "Your status: %s",
                     ctx->status_message);
            session_send_system_line(ctx, message);
        }
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    if (strcasecmp(working, "off") == 0 || strcasecmp(working, "clear") == 0) {
        ctx->status_message[0] = '\0';
        session_send_system_line(ctx, "Status message cleared.");
        return;
    }

    char cleaned[SSH_CHATTER_STATUS_LEN];
    if (!user_data_strip_ansi_sequences(working, cleaned, sizeof(cleaned)) ||
        cleaned[0] == '\0') {
        snprintf(cleaned, sizeof(cleaned), "%.*s", SSH_CHATTER_STATUS_LEN - 1,
                 working);
    }
    trim_whitespace_inplace(cleaned);

    if (cleaned[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    bool truncated = strnlen(working, sizeof(working)) >=
                     (SSH_CHATTER_STATUS_LEN - 1U);
    snprintf(ctx->status_message, sizeof(ctx->status_message), "%s", cleaned);

    if (truncated) {
        session_send_system_line(ctx,
                                 "Status message set (truncated to fit).");
    } else {
        session_send_system_line(ctx, "Status message set.");
    }
}

static void session_handle_showstatus(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    static const char *kUsage = "Usage: /showstatus <nickname>";
    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/showstatus", kUsage, usage,
                                 sizeof(usage));

    if (arguments == nullptr || arguments[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    char target_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(target_name, sizeof(target_name), "%s", arguments);
    trim_whitespace_inplace(target_name);

    if (target_name[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    if (ctx->owner == nullptr) {
        session_send_system_line(ctx, "User status lookup is unavailable.");
        return;
    }

    session_ctx_t *target =
        chat_room_find_user(&ctx->owner->room, target_name);
    if (target == nullptr) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "User '%s' is not connected.",
                 target_name);
        session_send_system_line(ctx, message);
        return;
    }

    if (target->status_message[0] == '\0') {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message),
                 "No status message set for '%s'.", target->user.name);
        session_send_system_line(ctx, message);
        return;
    }

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(message, sizeof(message), "Status for %s: %s", target->user.name,
             target->status_message);
    session_send_system_line(ctx, message);
}

static void session_handle_weather(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    static const char *kUsage = "Usage: /weather <city>";
    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/weather", kUsage, usage, sizeof(usage));
    if (arguments == nullptr || *arguments == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    const char *cursor = arguments;

    char city[64];
    snprintf(city, sizeof(city), "%s", cursor);
    trim_whitespace_inplace(city);

    for(unsigned i = 0; i < strnlen(city, 63); i++) {
        if(isspace(city[i])) {
            city[i] = '+';
        }
    }

    char summary[256];
    if (!session_fetch_weather_summary(city, summary,
                                       sizeof(summary))) {
        session_send_system_line(
            ctx,
            "Failed to fetch weather information. Please try again later.");
        return;
    }

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(message, sizeof(message), "%s", summary);
    session_send_system_line(ctx, message);
}

static void session_handle_translate(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    char working[16];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
    }
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, "Usage: /translate <on|off>");
        return;
    }

    if (session_argument_is_disable(working)) {
        ctx->translation_enabled = false;
        ctx->translation_quota_notified = false;
        session_translation_clear_queue(ctx);
        session_send_system_line(ctx,
                                 "Translation disabled. New messages will be "
                                 "delivered without translation.");
        if (ctx->owner != nullptr) {
            host_store_translation_preferences(ctx->owner, ctx);
        }
        return;
    }

    bool enabled = false;
    if (!parse_bool_token(working, &enabled)) {
        if (strcasecmp(working, "enable") == 0 ||
            strcasecmp(working, "enabled") == 0) {
            enabled = true;
        } else {
            session_send_system_line(ctx, "Usage: /translate <on|off>");
            return;
        }
    }

    ctx->translation_enabled = enabled;
    ctx->translation_quota_notified = false;
    if (enabled) {
        session_send_system_line(ctx,
                                 "Translation enabled. Configure directions "
                                 "with /set-trans-lang or /set-target-lang.");
    } else {
        session_translation_clear_queue(ctx);
        session_send_system_line(ctx,
                                 "Translation disabled. New messages will be "
                                 "delivered without translation.");
    }
    if (ctx->owner != nullptr) {
        host_store_translation_preferences(ctx->owner, ctx);
    }
}

static void session_handle_breaking_alerts(session_ctx_t *ctx,
                                           const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    char working[32];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
    }
    trim_whitespace_inplace(working);

    const char *prefix = session_command_prefix(ctx);
    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(usage, sizeof(usage), "Usage: %sbreaking <on|off|toggle>", prefix);

    if (working[0] == '\0') {
        char status[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(status, sizeof(status), "Breaking alerts are currently %s.",
                 ctx->breaking_alerts_enabled ? "ON" : "OFF");
        session_send_system_line(ctx, status);
        session_send_system_line(ctx, usage);
        return;
    }

    bool desired_state = ctx->breaking_alerts_enabled;
    bool recognized = false;

    if (session_argument_is_disable(working)) {
        desired_state = false;
        recognized = true;
    } else if (session_argument_is_enable(working)) {
        desired_state = true;
        recognized = true;
    } else if (strcasecmp(working, "toggle") == 0) {
        desired_state = !ctx->breaking_alerts_enabled;
        recognized = true;
    }

    if (!recognized) {
        session_send_system_line(ctx, usage);
        return;
    }

    ctx->breaking_alerts_enabled = desired_state;
    session_send_system_line(
        ctx, desired_state ? "Breaking alerts enabled."
                           : "Breaking alerts disabled.");

    if (ctx->owner != nullptr) {
        host_store_breaking_alerts(ctx->owner, ctx);
    }
}

static void session_translate_scope_send_usage(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    const char *prefix = session_command_prefix(ctx);
    char *usage_format_head = "Usage: ";
    char *usage_format_tail = "translate-scope <chat|chat-nohistory|all>";

    switch (ctx->ui_language) {
    case SESSION_UI_LANGUAGE_KO:
        usage_format_head = "사용법: ";
        usage_format_tail = "번역범위 <채팅|채팅기록없음|모두>";
        break;
    case SESSION_UI_LANGUAGE_JP:
        usage_format_head = "使い方: ";
        usage_format_tail = "翻訳範囲 <チャット|チャット履歴なし|すべて>";
        break;
    case SESSION_UI_LANGUAGE_ZH:
        usage_format_head = "用法：";
        usage_format_tail = "翻译范围 <聊天|无聊天记录|全部>";
        break;
    case SESSION_UI_LANGUAGE_RU:
        usage_format_head = "Использование: ";
        usage_format_tail = "область-перевода <чат|чат-без-истории|все>";
        break;
    default:
        break;
    }

    char usage_line[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(usage_line, sizeof(usage_line), "%s%s%s",
             usage_format_head, prefix, usage_format_tail);
    session_send_system_line(ctx, usage_line);
}

static void session_handle_translate_scope(session_ctx_t *ctx,
                                           const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(
            ctx, "Only operators may manage translation scope.");
        return;
    }

    char token[64];
    token[0] = '\0';
    if (arguments != nullptr) {
        const char *cursor = arguments;
        while (*cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }

        size_t length = 0U;
        while (cursor[length] != '\0' &&
               !isspace((unsigned char)cursor[length]) &&
               length + 1U < sizeof(token)) {
            token[length] = cursor[length];
            ++length;
        }
        token[length] = '\0';
    }

    if (token[0] == '\0') {
        const bool limited = translator_should_limit_to_chat_bbs();
        const bool forced = translator_is_ollama_only();
        const bool manual = translator_is_manual_chat_bbs_only();
        const bool skip_scrollback = translator_is_manual_skip_scrollback();

        char status[SSH_CHATTER_MESSAGE_LIMIT];
        if (limited) {
            if (skip_scrollback) {
                snprintf(status, sizeof(status),
                         "Translation scope is currently limited to chat "
                         "messages and "
                         "BBS posts. Scrollback translation is disabled.");
            } else {
                snprintf(status, sizeof(status),
                         "Translation scope is currently limited to chat "
                         "messages and "
                         "BBS posts.");
            }
        } else {
            snprintf(
                status, sizeof(status),
                "Translation scope currently includes system output and bulk "
                "messages.");
        }
        session_send_system_line(ctx, status);

        if (forced) {
            session_send_system_line(
                ctx, "Gemini translation is unavailable; Ollama "
                     "fallback enforces chat/BBS-only scope.");
        } else if (manual) {
            if (skip_scrollback) {
                session_send_system_line(
                    ctx, "Chat/BBS-only scope is enabled manually. "
                         "Scrollback translation is suppressed.");
            } else {
                session_send_system_line(
                    ctx, "Chat/BBS-only scope is enabled manually.");
            }
        }

        session_translate_scope_send_usage(ctx);
        return;
    }

    bool limit_chat_scope = false;
    bool limit_chat_nohistory_scope = false;
    bool restore_full_scope = false;

    if (strcasecmp(token, "chat") == 0 || strcasecmp(token, "limit") == 0 ||
        strcasecmp(token, "on") == 0) {
        limit_chat_scope = true;
    }
    if (strcasecmp(token, "chat-nohistory") == 0 ||
        strcasecmp(token, "chat_nohistory") == 0 ||
        strcasecmp(token, "chat-nohist") == 0) {
        limit_chat_nohistory_scope = true;
    }
    if (strcasecmp(token, "all") == 0 || strcasecmp(token, "full") == 0 ||
        strcasecmp(token, "off") == 0) {
        restore_full_scope = true;
    }

    if (!limit_chat_scope && !limit_chat_nohistory_scope &&
        !restore_full_scope) {
        if (strcmp(token, "채팅") == 0 || strcmp(token, "チャット") == 0 ||
            strcmp(token, "聊天") == 0 || strcmp(token, "чат") == 0) {
            limit_chat_scope = true;
        } else if (strcmp(token, "채팅기록없음") == 0 ||
                   strcmp(token, "チャット履歴なし") == 0 ||
                   strcmp(token, "无聊天记录") == 0 ||
                   strcmp(token, "чат-без-истории") == 0) {
            limit_chat_nohistory_scope = true;
        } else if (strcmp(token, "모두") == 0 || strcmp(token, "すべて") == 0 ||
                   strcmp(token, "全部") == 0 || strcmp(token, "все") == 0 ||
                   strcmp(token, "всё") == 0) {
            restore_full_scope = true;
        }
    }

    if (limit_chat_scope) {
        if (translator_is_manual_chat_bbs_only() &&
            !translator_is_manual_skip_scrollback()) {
            session_send_system_line(ctx,
                                     "Translation scope is already limited to "
                                     "chat messages and BBS posts.");
            return;
        }

        translator_set_manual_chat_bbs_only(true);
        translator_set_manual_skip_scrollback(false);
        session_send_system_line(
            ctx, "Translation scope limited to chat messages and BBS posts.");

        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice),
                 "* [%s] limited translation scope to chat and BBS posts.",
                 ctx->user.name);
        host_history_record_system(ctx->owner, notice, nullptr);
        chat_room_broadcast(&ctx->owner->room, notice, nullptr);
        return;
    }

    if (limit_chat_nohistory_scope) {
        if (translator_is_manual_chat_bbs_only() &&
            translator_is_manual_skip_scrollback()) {
            session_send_system_line(
                ctx,
                "Translation scope is already limited to chat/BBS posts with "
                "scrollback translation disabled.");
            return;
        }

        translator_set_manual_chat_bbs_only(true);
        translator_set_manual_skip_scrollback(true);
        session_send_system_line(
            ctx, "Translation scope limited to chat messages and "
                 "BBS posts. Scrollback translation is disabled.");

        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(
            notice, sizeof(notice),
            "* [%s] limited translation scope to chat/BBS posts and disabled "
            "scrollback translation.",
            ctx->user.name);
        host_history_record_system(ctx->owner, notice, nullptr);
        chat_room_broadcast(&ctx->owner->room, notice, nullptr);
        return;
    }

    if (restore_full_scope) {
        if (translator_is_ollama_only()) {
            session_send_system_line(
                ctx,
                "Full translation scope cannot be restored while Gemini is "
                "unavailable."
                " Ollama-only mode restricts translation to chat and BBS "
                "posts.");
            return;
        }

        if (!translator_is_manual_chat_bbs_only()) {
            session_send_system_line(ctx, "Translation scope already includes "
                                          "system output and bulk messages.");
            return;
        }

        translator_set_manual_chat_bbs_only(false);
        translator_set_manual_skip_scrollback(false);
        session_send_system_line(
            ctx,
            "Full translation scope restored. System output and bulk messages "
            "are eligible for translation.");

        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice),
                 "* [%s] restored full translation scope for translations.",
                 ctx->user.name);
        host_history_record_system(ctx->owner, notice, nullptr);
        chat_room_broadcast(&ctx->owner->room, notice, nullptr);
        return;
    }

    session_translate_scope_send_usage(ctx);
}

static void session_handle_gemini(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(
            ctx, "Only operators may manage Gemini translation.");
        return;
    }

    const char *cursor = arguments;
    while (cursor != nullptr && (*cursor == ' ' || *cursor == '\t')) {
        ++cursor;
    }

    char token[16];
    token[0] = '\0';
    if (cursor != nullptr && *cursor != '\0') {
        size_t length = 0U;
        while (cursor[length] != '\0' &&
               !isspace((unsigned char)cursor[length]) &&
               length + 1U < sizeof(token)) {
            token[length] = cursor[length];
            ++length;
        }
        token[length] = '\0';
    }

    if (token[0] == '\0') {
        bool enabled = translator_is_gemini_enabled();
        bool manual = translator_is_gemini_manually_disabled();
        struct timespec remaining = {0, 0};
        bool cooldown_active = translator_gemini_backoff_remaining(&remaining);

        char status_line[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(status_line, sizeof(status_line),
                 "Gemini translation is currently %s.",
                 enabled ? "enabled" : "disabled");
        session_send_system_line(ctx, status_line);

        if (manual) {
            session_send_system_line(ctx, "Gemini usage is manually disabled. "
                                          "Use /gemini on to re-enable it.");
        }

        if (cooldown_active) {
            long long seconds = remaining.tv_sec;
            if (remaining.tv_nsec > 0L) {
                ++seconds;
            }
            long long hours = seconds / 3600LL;
            long long minutes = (seconds % 3600LL) / 60LL;
            long long secs = seconds % 60LL;

            char cooldown_line[SSH_CHATTER_MESSAGE_LIMIT];
            if (hours > 0) {
                snprintf(cooldown_line, sizeof(cooldown_line),
                         "Automatic Gemini cooldown ends in %lldh %lldm %llds.",
                         hours, minutes, secs);
            } else if (minutes > 0) {
                snprintf(cooldown_line, sizeof(cooldown_line),
                         "Automatic Gemini cooldown ends in %lldm %llds.",
                         minutes, secs);
            } else {
                snprintf(cooldown_line, sizeof(cooldown_line),
                         "Automatic Gemini cooldown ends in %lld seconds.",
                         secs > 0 ? secs : 1LL);
            }
            session_send_system_line(ctx, cooldown_line);
        }

        session_send_system_line(ctx, "Usage: /gemini <on|off>");
        session_send_system_line(
            ctx,
            "Use /gemini-unfreeze to clear the automatic cooldown manually.");
        return;
    }

    bool requested_enable = false;
    bool recognized = false;
    if (session_argument_is_disable(token)) {
        recognized = true;
        requested_enable = false;
    } else {
        recognized = parse_bool_token(token, &requested_enable);
    }

    if (!recognized) {
        session_send_system_line(ctx, "Usage: /gemini <on|off>");
        return;
    }

    if (requested_enable) {
        translator_set_gemini_enabled(true);
        session_send_system_line(
            ctx,
            "Gemini translation enabled. Ollama fallback remains available.");

        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice),
                 "* [%s] enabled Gemini translation; Ollama fallback remains "
                 "available.",
                 ctx->user.name);
        host_history_record_system(ctx->owner, notice, nullptr);
        chat_room_broadcast(&ctx->owner->room, notice, nullptr);
        return;
    }

    translator_set_gemini_enabled(false);
    session_send_system_line(
        ctx, "Gemini translation disabled. Using Ollama gemma2:2b only.");
    session_send_system_line(ctx, "While Gemini is off, only chat messages and "
                                  "BBS posts will be translated.");

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice),
             "* [%s] disabled Gemini translation; using Ollama fallback only "
             "(chat and BBS posts).",
             ctx->user.name);
    host_history_record_system(ctx->owner, notice, nullptr);
    chat_room_broadcast(&ctx->owner->room, notice, nullptr);
    return;
}

static void session_handle_captcha(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(
            ctx, "Only operators may control captcha requirements.");
        return;
    }

    char token[16];
    if (arguments != nullptr) {
        snprintf(token, sizeof(token), "%s", arguments);
        trim_whitespace_inplace(token);
    } else {
        token[0] = '\0';
    }

    host_t *host = ctx->owner;
    if (token[0] == '\0') {
        bool enabled = atomic_load(&host->captcha_enabled);
        char status[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(status, sizeof(status), "Captcha is currently %s.",
                 enabled ? "enabled" : "disabled");
        session_send_system_line(ctx, status);
        session_send_system_line(ctx, "Usage: /captcha <on|off>");
        return;
    }

    bool requested_enable = false;
    bool recognized = false;
    if (session_argument_is_disable(token)) {
        recognized = true;
        requested_enable = false;
    } else {
        recognized = parse_bool_token(token, &requested_enable);
    }

    if (!recognized) {
        session_send_system_line(ctx, "Usage: /captcha <on|off>");
        return;
    }

    if (requested_enable) {
        bool was_enabled = atomic_exchange(&host->captcha_enabled, true);
        if (was_enabled) {
            session_send_system_line(ctx, "Captcha is already enabled.");
        } else {
            session_send_system_line(
                ctx, "Captcha enabled. New connections must solve the puzzle.");
            char notice[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(notice, sizeof(notice),
                     "* [%s] enabled captcha for new connections.",
                     ctx->user.name);
            host_history_record_system(host, notice, nullptr);
            chat_room_broadcast(&host->room, notice, nullptr);
            ttak_mutex_lock(&host->lock);
            host_state_save_locked(host);
            ttak_mutex_unlock(&host->lock);
        }
        return;
    }

    bool was_enabled = atomic_exchange(&host->captcha_enabled, false);
    if (!was_enabled) {
        session_send_system_line(ctx, "Captcha is already disabled.");
    } else {
        session_send_system_line(
            ctx, "Captcha disabled. New connections will skip the puzzle.");
        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice),
                 "* [%s] disabled captcha for new connections.",
                 ctx->user.name);
        host_history_record_system(host, notice, nullptr);
        chat_room_broadcast(&host->room, notice, nullptr);
        ttak_mutex_lock(&host->lock);
        host_state_save_locked(host);
        ttak_mutex_unlock(&host->lock);
    }
    return;
}

static void session_handle_geo_language(session_ctx_t *ctx,
                                        const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(ctx,
                                 "Only operators may control geo language.");
        return;
    }

    char token[32];
    if (arguments != nullptr) {
        snprintf(token, sizeof(token), "%s", arguments);
        trim_whitespace_inplace(token);
    } else {
        token[0] = '\0';
    }

    host_t *host = ctx->owner;
    if (token[0] == '\0') {
        bool enabled = atomic_load(&host->geo_language_enabled);
        char status[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(status, sizeof(status),
                 "Geo-IP UI language detection is currently %s.",
                 enabled ? "enabled" : "disabled");
        session_send_system_line(ctx, status);
        session_send_system_line(ctx, "Usage: /geo <on|off>");
        return;
    }

    bool requested_enable = false;
    bool recognized = false;
    if (session_argument_is_disable(token)) {
        recognized = true;
        requested_enable = false;
    } else {
        recognized = parse_bool_token(token, &requested_enable);
    }

    if (!recognized) {
        session_send_system_line(ctx, "Usage: /geo <on|off>");
        return;
    }

    bool was_enabled =
        atomic_exchange(&host->geo_language_enabled, requested_enable);
    if (requested_enable) {
        if (was_enabled) {
            session_send_system_line(ctx, "Geo-IP UI language is already on.");
            return;
        }

        session_send_system_line(
            ctx, "Geo-IP UI language detection enabled for new connections.");
        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice),
                 "* [%s] enabled Geo-IP UI language detection.",
                 ctx->user.name);
        host_history_record_system(host, notice, nullptr);
        chat_room_broadcast(&host->room, notice, nullptr);
        ttak_mutex_lock(&host->lock);
        host_state_save_locked(host);
        ttak_mutex_unlock(&host->lock);
        return;
    }

    if (!was_enabled) {
        session_send_system_line(ctx,
                                 "Geo-IP UI language is already disabled.");
        return;
    }

    session_send_system_line(
        ctx, "Geo-IP UI language detection disabled; defaulting to English.");
    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice),
             "* [%s] disabled Geo-IP UI language detection (English default).",
             ctx->user.name);
    host_history_record_system(host, notice, nullptr);
    chat_room_broadcast(&host->room, notice, nullptr);
    ttak_mutex_lock(&host->lock);
    host_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);
}

static void session_handle_eliza(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(ctx, "Only operators may control eliza.");
        return;
    }

    char token[32];
    if (arguments != nullptr) {
        snprintf(token, sizeof(token), "%s", arguments);
        trim_whitespace_inplace(token);
    } else {
        token[0] = '\0';
    }

    if (token[0] == '\0') {
        bool enabled = atomic_load(&ctx->owner->eliza_enabled);
        char status[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(status, sizeof(status), "eliza is currently %s.",
                 enabled ? "enabled" : "disabled");
        session_send_system_line(ctx, status);
        session_send_system_line(ctx, "Usage: /eliza <on|off>");
        return;
    }

    bool requested_enable = false;
    bool recognized = false;
    if (session_argument_is_disable(token)) {
        recognized = true;
        requested_enable = false;
    } else {
        recognized = parse_bool_token(token, &requested_enable);
    }

    if (!recognized) {
        session_send_system_line(ctx, "Usage: /eliza <on|off>");
        return;
    }

    if (requested_enable) {
        if (host_eliza_enable(ctx->owner)) {
            session_send_system_line(ctx,
                                     "eliza enabled. She will now mingle with "
                                     "the room and watch for severe issues.");
        } else {
            session_send_system_line(ctx, "eliza is already active.");
        }
        return;
    }

    if (host_eliza_disable(ctx->owner)) {
        session_send_system_line(ctx, "eliza disabled.");
    } else {
        session_send_system_line(ctx, "eliza is already inactive.");
    }
    return;
}

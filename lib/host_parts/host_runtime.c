// Remaining operator commands, ban management, and host lifecycle entry points.
#include "host_internal.h"

#define TELNET_STABLE_RESET_SECONDS 10.0

static session_ctx_t *session_create(void)
{
#if defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC
    session_ctx_t *ctx = (session_ctx_t *)GC_CALLOC(1U, sizeof(session_ctx_t));
#else
    session_ctx_t *ctx = (session_ctx_t *)calloc(1U, sizeof(session_ctx_t));
#endif
    if (ctx != NULL) {
        ctx->user.is_authenticated = false;
    }
    return ctx;
}

static void session_destroy(session_ctx_t *ctx);

static size_t session_encode_utf8_codepoint(uint32_t codepoint, char *output,
                                            size_t capacity)
{
    if (output == NULL || capacity == 0U) {
        return 0U;
    }

    if (codepoint <= 0x7FU) {
        if (capacity < 1U) {
            return 0U;
        }
        output[0] = (char)codepoint;
        return 1U;
    }

    if (codepoint <= 0x7FFU) {
        if (capacity < 2U) {
            return 0U;
        }
        output[0] = (char)(0xC0U | ((codepoint >> 6U) & 0x1FU));
        output[1] = (char)(0x80U | (codepoint & 0x3FU));
        return 2U;
    }

    if (codepoint <= 0xFFFFU) {
        if (capacity < 3U) {
            return 0U;
        }
        output[0] = (char)(0xE0U | ((codepoint >> 12U) & 0x0FU));
        output[1] = (char)(0x80U | ((codepoint >> 6U) & 0x3FU));
        output[2] = (char)(0x80U | (codepoint & 0x3FU));
        return 3U;
    }

    if (codepoint <= 0x10FFFFU) {
        if (capacity < 4U) {
            return 0U;
        }
        output[0] = (char)(0xF0U | ((codepoint >> 18U) & 0x07U));
        output[1] = (char)(0x80U | ((codepoint >> 12U) & 0x3FU));
        output[2] = (char)(0x80U | ((codepoint >> 6U) & 0x3FU));
        output[3] = (char)(0x80U | (codepoint & 0x3FU));
        return 4U;
    }

    if (capacity < 1U) {
        return 0U;
    }
    output[0] = '?';
    return 1U;
}

size_t session_cp437_byte_to_utf8(unsigned char byte, char *output,
                                  size_t capacity)
{
    if (output == NULL || capacity == 0U) {
        return 0U;
    }

    if (byte < 0x80U) {
        output[0] = (char)byte;
        return 1U;
    }

    static const uint16_t kCp437ToUnicode[128] = {
        0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA,
        0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6,
        0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC,
        0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA,
        0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC,
        0x00A1, 0x00AB, 0x00BB, 0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561,
        0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B,
        0x2510, 0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
        0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567, 0x2568,
        0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518,
        0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580, 0x03B1, 0x00DF, 0x0393,
        0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4,
        0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320,
        0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2,
        0x25A0, 0x00A0,
    };

    const uint32_t codepoint = kCp437ToUnicode[byte - 0x80U];
    size_t produced =
        session_encode_utf8_codepoint(codepoint, output, capacity);
    if (produced == 0U) {
        output[0] = '?';
        return 1U;
    }

    return produced;
}

bool host_user_data_load_existing(host_t *host, const char *username,
                                  const char *ip, user_data_record_t *record,
                                  bool create_if_missing);

static void session_handle_gemini_unfreeze(session_ctx_t *ctx)
{
    if (ctx == NULL || ctx->owner == NULL) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(
            ctx, "Only operators may manage Gemini translation.");
        return;
    }

    struct timespec remaining = {0, 0};
    bool cooldown_active = translator_gemini_backoff_remaining(&remaining);
    translator_clear_gemini_backoff();

    if (cooldown_active) {
        session_send_system_line(ctx, "Automatic Gemini cooldown cleared. "
                                      "Translations may resume immediately.");

        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice),
                 "* [%s] cleared the automatic Gemini cooldown.",
                 ctx->user.name);
        host_history_record_system(ctx->owner, notice, NULL);
        chat_room_broadcast(&ctx->owner->room, notice, NULL);
    } else {
        session_send_system_line(ctx,
                                 "No automatic Gemini cooldown was active.");
    }
}

static void session_handle_palette(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == NULL) {
        return;
    }

    if (arguments == NULL) {
        session_send_system_line(ctx,
                                 "Usage: /palette <name> (try /palette list)");
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);

    if (working[0] == '\0' || strcasecmp(working, "list") == 0) {
        session_send_system_line(ctx, "Available palettes:");

        for (size_t idx = 0U;
             idx < sizeof(PALETTE_DEFINITIONS) / sizeof(PALETTE_DEFINITIONS[0]);
             ++idx) {
            const palette_descriptor_t *descriptor = &PALETTE_DEFINITIONS[idx];
            const char *name = descriptor->name->en;
            const char *description = descriptor->description->en;
            switch (session_ui_language_current(ctx)) {
            case SESSION_UI_LANGUAGE_KO:
                name = descriptor->name->ko;
                description = descriptor->description->ko;
                break;
            case SESSION_UI_LANGUAGE_JP:
                name = descriptor->name->jp;
                description = descriptor->description->jp;
                break;
            case SESSION_UI_LANGUAGE_ZH:
                name = descriptor->name->zh;
                description = descriptor->description->zh;
                break;
            case SESSION_UI_LANGUAGE_RU:
                name = descriptor->name->ru;
                description = descriptor->description->ru;
                break;
            default:
                break;
            }
            char palette_line[SSH_CHATTER_MESSAGE_LIMIT];
            static char palette_line_group[SSH_CHATTER_MESSAGE_LIMIT * 16];
            if (descriptor->is_256_color) {
                snprintf(palette_line, sizeof(palette_line),
                         "\033[1G  %s\033[1G\n   - %s (256-color)\n", name,
                         description);
            } else {
                snprintf(palette_line, sizeof(palette_line),
                         "\033[1G  %s\033[1G\n   - %s\n", name, description);
            }
            strncat(palette_line_group, palette_line,
                    strnlen(palette_line, SSH_CHATTER_MESSAGE_LIMIT));
            strncat(palette_line_group, "\n", 1);
            if ((idx + 1) % 16 == 0) {
                session_send_system_line(ctx, palette_line_group);
                memset(palette_line_group, 0,
                       sizeof(SSH_CHATTER_MESSAGE_LIMIT * 16));
            }
        }
        session_send_system_line(ctx, "Apply a palette with /palette <name>.");
        return;
    }

    const palette_descriptor_t *descriptor = palette_find_descriptor(working);
    if (descriptor == NULL) {
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(line, sizeof(line),
                 "Unknown palette '%.32s'. Use /palette list to see options.",
                 working);
        session_send_system_line(ctx, line);
        return;
    }

    if (!palette_apply_to_session(ctx, descriptor)) {
        session_send_system_line(ctx,
                                 "Unable to apply that palette right now.");
        return;
    }

    session_apply_background_fill(ctx);

    char info[SSH_CHATTER_MESSAGE_LIMIT];
    const char *name = descriptor->name->en;
    const char *description = descriptor->description->en;
    switch (session_ui_language_current(ctx)) {
    case SESSION_UI_LANGUAGE_KO:
        name = descriptor->name->ko;
        description = descriptor->description->ko;
        break;
    case SESSION_UI_LANGUAGE_JP:
        name = descriptor->name->jp;
        description = descriptor->description->jp;
        break;
    case SESSION_UI_LANGUAGE_ZH:
        name = descriptor->name->zh;
        description = descriptor->description->zh;
        break;
    case SESSION_UI_LANGUAGE_RU:
        name = descriptor->name->ru;
        description = descriptor->description->ru;
        break;
    default:
        break;
    }
    snprintf(info, sizeof(info), "Palette '%s' applied - %s", name,
             description);
    session_send_system_line(ctx, info);
    session_render_separator(ctx, "Chatroom");
    session_render_prompt(ctx, true);

    if (ctx->owner != NULL) {
        host_store_user_theme(ctx->owner, ctx);
        host_store_system_theme(ctx->owner, ctx);
    }
}

void session_handle_retro(session_ctx_t *ctx, const char *arguments)
{
    static const char *kUsage = "Usage: /retro <on|off|auto|status>";

    if (ctx == NULL) {
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    if (arguments != NULL) {
        snprintf(working, sizeof(working), "%s", arguments);
        trim_whitespace_inplace(working);
    } else {
        working[0] = '\0';
    }

    if (working[0] == '\0' || strcasecmp(working, "status") == 0) {
        const char *mode = "automatic";
        if (ctx->cp437_override == SESSION_CP437_OVERRIDE_FORCE_ON) {
            mode = "forced on";
        } else if (ctx->cp437_override == SESSION_CP437_OVERRIDE_FORCE_OFF) {
            mode = "forced off";
        }

        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message),
                 "Retro encoding mode: %s (input: %s, output: %s).", mode,
                 ctx->cp437_input_enabled ? "CP437" : "UTF-8",
                 ctx->prefer_cp437_output ? "CP437" : "UTF-8");
        session_send_system_line(ctx, message);
        session_send_system_line(
            ctx, "Toggle with /retro on, /retro off, or /retro auto.");
        return;
    }

    if (strcasecmp(working, "on") == 0) {
        ctx->cp437_override = SESSION_CP437_OVERRIDE_FORCE_ON;
        session_refresh_output_encoding(ctx);
        session_send_system_line(
            ctx,
            "Retro encoding enabled. CP437 input and output are forced on.");
        return;
    }

    if (strcasecmp(working, "off") == 0) {
        ctx->cp437_override = SESSION_CP437_OVERRIDE_FORCE_OFF;
        session_refresh_output_encoding(ctx);
        session_send_system_line(
            ctx,
            "Retro encoding disabled. CP437 input and output are forced off.");
        return;
    }

    if (strcasecmp(working, "auto") == 0) {
        ctx->cp437_override = SESSION_CP437_OVERRIDE_NONE;
        session_refresh_output_encoding(ctx);
        session_send_system_line(
            ctx, "Retro encoding returned to automatic detection.");
        return;
    }

    session_send_system_line(ctx, kUsage);
}

static bool find_reserved_names(session_ctx_t *ctx, const char *nick)
{
    if (ctx == NULL || ctx->owner == NULL || nick == NULL || nick[0] == '\0') {
        return false;
    }

    bool found = false;
    pthread_mutex_lock(&ctx->owner->nickname_reserve_lock);
    for (size_t i = 0; i < ctx->owner->reserved_nicknames_len; ++i) {
        if (strcasecmp(nick, ctx->owner->reserved_nicknames[i]) == 0) {
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&ctx->owner->nickname_reserve_lock);
    return found;
}

bool host_username_has_password(host_t *host, const char *nick)
{
    if (host == NULL || nick == NULL || nick[0] == '\0') {
        return false;
    }

    user_data_record_t record;
    if (host_user_data_load_existing(host, nick, NULL, &record, false) &&
        !security_layer_is_zero_hash(record.password_hash,
                                     sizeof(record.password_hash))) {
        return true;
    }

    if (host->pw_auth_file_path[0] == '\0') {
        return false;
    }

    FILE *fp = fopen(host->pw_auth_file_path, "rb");
    if (fp == NULL) {
        return false;
    }

    bool protected_name = false;
    char line[SSH_CHATTER_MESSAGE_LIMIT];
    while (!protected_name && fgets(line, sizeof(line), fp) != NULL) {
        size_t length = strcspn(line, "\r\n");
        line[length] = '\0';

        char *first_separator = strchr(line, ':');
        if (first_separator == NULL) {
            continue;
        }

        size_t name_length = (size_t)(first_separator - line);
        if (name_length == 0U) {
            continue;
        }

        char existing[SSH_CHATTER_USERNAME_LEN];
        if (name_length >= sizeof(existing)) {
            name_length = sizeof(existing) - 1U;
        }

        memcpy(existing, line, name_length);
        existing[name_length] = '\0';

        if (strcasecmp(existing, nick) == 0) {
            protected_name = true;
        }
    }

    int read_error = ferror(fp);
    fclose(fp);

    if (read_error != 0) {
        return false;
    }

    return protected_name;
}

static void session_handle_nick(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == NULL) {
        return;
    }

    if (arguments == NULL || *arguments == '\0') {
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

    if (find_reserved_names(ctx, new_name)) {
        session_send_system_line(ctx, "That name is reserved.");
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

    if (!owns_requested_name &&
        host_username_has_password(ctx->owner, new_name)) {
        session_send_system_line(
            ctx, "That nickname is password-protected. Log in as that user.");
        return;
    }

    session_ctx_t *existing = chat_room_find_user(&ctx->owner->room, new_name);
    if (existing != NULL && existing != ctx) {
        session_send_system_line(ctx, "That name is already taken.");
        return;
    }

    char old_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(old_name, sizeof(old_name), "%s", ctx->user.name);
    snprintf(ctx->user.name, sizeof(ctx->user.name), "%s", new_name);

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
    if (ctx == NULL) {
        return;
    }

    if (reason != NULL && reason[0] != '\0') {
        session_send_system_line(ctx, reason);
    }

    ctx->should_exit = true;
    ctx->exit_status = EXIT_FAILURE;

    if (ctx->translation_mutex_initialized) {
        pthread_mutex_lock(&ctx->translation_mutex);
        ctx->translation_thread_stop = true;
        pthread_cond_broadcast(&ctx->translation_cond);
        pthread_mutex_unlock(&ctx->translation_mutex);
    }
    session_translation_clear_queue(ctx);

    session_transport_request_close(ctx);
}

static void session_handle_exit(session_ctx_t *ctx)
{
    if (ctx == NULL) {
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

    if (arguments == NULL || *arguments == '\0') {
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
    if (room == NULL || username == NULL) {
        return NULL;
    }

    session_ctx_t *result = NULL;
    pthread_mutex_lock(&room->lock);
    for (size_t idx = 0; idx < room->member_count; ++idx) {
        session_ctx_t *member = room->members[idx];
        if (member == NULL) {
            continue;
        }

        if (strncmp(member->user.name, username, SSH_CHATTER_USERNAME_LEN) ==
            0) {
            result = member;
            break;
        }
    }
    pthread_mutex_unlock(&room->lock);

    return result;
}

static bool host_username_reserved(host_t *host, const char *username)
{
    if (host == NULL || username == NULL || username[0] == '\0') {
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
    if (host == NULL || ip == NULL) {
        return NULL;
    }

    for (size_t idx = 0; idx < host->join_activity_count; ++idx) {
        join_activity_entry_t *entry = &host->join_activity[idx];
        if (strncmp(entry->ip, ip, SSH_CHATTER_IP_LEN) == 0) {
            return entry;
        }
    }

    return NULL;
}

static void
host_prune_join_activity_locked(host_t *host,
                                const struct timespec *reference_time)
{
    if (host == NULL || host->join_activity == NULL ||
        host->join_activity_count == 0U) {
        return;
    }

    struct timespec now = {0, 0};
    if (reference_time != NULL &&
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
        free(host->join_activity);
        host->join_activity = NULL;
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
        join_activity_entry_t *resized = (join_activity_entry_t *)realloc(
            host->join_activity, new_capacity * sizeof(*resized));
        if (resized != NULL) {
            host->join_activity = resized;
            host->join_activity_capacity = new_capacity;
        }
    }
}

static join_activity_entry_t *host_ensure_join_activity_locked(host_t *host,
                                                               const char *ip)
{
    if (host == NULL || ip == NULL || ip[0] == '\0') {
        return NULL;
    }

    join_activity_entry_t *entry = host_find_join_activity_locked(host, ip);
    if (entry != NULL) {
        return entry;
    }

    if (host->join_activity_count >= host->join_activity_capacity) {
        size_t new_capacity = host->join_activity_capacity > 0U
                                  ? host->join_activity_capacity * 2U
                                  : 8U;
        join_activity_entry_t *resized = realloc(
            host->join_activity, new_capacity * sizeof(join_activity_entry_t));
        if (resized == NULL) {
            return NULL;
        }
        host->join_activity = resized;
        host->join_activity_capacity = new_capacity;
    }

    entry = &host->join_activity[host->join_activity_count++];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->ip, sizeof(entry->ip), "%s", ip);
    return entry;
}

static size_t host_prepare_join_delay(host_t *host,
                                      struct timespec *wait_duration)
{
    struct timespec wait = {0, 0};
    if (host == NULL) {
        if (wait_duration != NULL) {
            *wait_duration = wait;
        }
        return 1U;
    }

    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);

    pthread_mutex_lock(&host->lock);
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
    pthread_mutex_unlock(&host->lock);

    if (wait_duration != NULL) {
        *wait_duration = wait;
    }
    return progress;
}

static host_join_attempt_result_t
host_register_join_attempt(host_t *host, const char *username, const char *ip)
{
    if (host == NULL || ip == NULL || ip[0] == '\0') {
        return HOST_JOIN_ATTEMPT_OK;
    }

    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);

    bool ban_ip = false;
    bool ban_same_name = false;
    bool exempt_ip = false;
    bool kick_ip = false;

    pthread_mutex_lock(&host->lock);
    host_prune_join_activity_locked(host, &now);
    join_activity_entry_t *entry = host_ensure_join_activity_locked(host, ip);
    if (entry == NULL) {
        pthread_mutex_unlock(&host->lock);
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

    if (username != NULL && username[0] != '\0') {
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

    if (!exempt_ip && within_window &&
        entry->rapid_attempts >= SSH_CHATTER_JOIN_IP_THRESHOLD) {
        ban_ip = true;
    }
    if (within_window &&
        entry->same_name_attempts >= SSH_CHATTER_JOIN_NAME_THRESHOLD) {
        ban_same_name = true;
    }
    if (!exempt_ip &&
        entry->join_window_attempts >= SSH_CHATTER_JOIN_KICK_THRESHOLD) {
        kick_ip = true;
    }
    pthread_mutex_unlock(&host->lock);

    if (!exempt_ip && (ban_ip || ban_same_name)) {
        const char *ban_user =
            (ban_same_name && username != NULL && username[0] != '\0')
                ? username
                : "";
        (void)host_add_ban_entry(host, ban_user, ip);

        if (ban_ip && ban_same_name) {
            printf(
                "[auto-ban] %s (%s) banned for rapid reconnects and repeated "
                "username attempts\n",
                ip, ban_user[0] != '\0' ? ban_user : "unknown");
        } else if (ban_ip) {
            printf("[auto-ban] %s banned for rapid reconnects\n", ip);
        } else {
            printf("[auto-ban] %s (%s) banned for repeated username attempts\n",
                   ip, ban_user[0] != '\0' ? ban_user : "unknown");
        }
        return HOST_JOIN_ATTEMPT_BAN;
    }

    if (!exempt_ip && kick_ip) {
        printf("[auto-kick] %s exceeded join limit\n", ip);
        return HOST_JOIN_ATTEMPT_KICK;
    }

    return HOST_JOIN_ATTEMPT_OK;
}

static bool host_register_suspicious_activity(host_t *host,
                                              const char *username,
                                              const char *ip,
                                              size_t *attempts_out)
{
    if (host == NULL || ip == NULL || ip[0] == '\0') {
        if (attempts_out != NULL) {
            *attempts_out = 0U;
        }
        return false;
    }

    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);

    size_t attempts = 0U;
    pthread_mutex_lock(&host->lock);
    join_activity_entry_t *entry = host_ensure_join_activity_locked(host, ip);
    if (entry != NULL) {
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
    pthread_mutex_unlock(&host->lock);

    if (attempts_out != NULL) {
        *attempts_out = attempts;
    }

    if (attempts >= SSH_CHATTER_SUSPICIOUS_EVENT_THRESHOLD) {
        const char *ban_user =
            (username != NULL && username[0] != '\0') ? username : "";
        (void)host_add_ban_entry(host, ban_user, ip);
        return true;
    }

    return false;
}

static bool host_is_ip_banned(host_t *host, const char *ip)
{
    if (host == NULL || ip == NULL || ip[0] == '\0') {
        return false;
    }

    bool banned = false;
    pthread_mutex_lock(&host->lock);
    if (host_is_protected_ip_unlocked(host, ip)) {
        pthread_mutex_unlock(&host->lock);
        return false;
    }
    for (size_t idx = 0; idx < host->ban_count; ++idx) {
        const char *ban_ip = host->bans[idx].ip;
        if (ban_ip[0] == '\0') {
            continue;
        }

        if (host_is_protected_ip_unlocked(host, ban_ip)) {
            continue;
        }

        if (strchr(ban_ip, '/') != NULL) {
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
    pthread_mutex_unlock(&host->lock);

    return banned;
}

static bool host_is_username_banned(host_t *host, const char *username)
{
    if (host == NULL || username == NULL || username[0] == '\0') {
        return false;
    }

    bool banned = false;
    pthread_mutex_lock(&host->lock);
    for (size_t idx = 0; idx < host->ban_count; ++idx) {
        if (strncmp(host->bans[idx].username, username,
                    SSH_CHATTER_USERNAME_LEN) == 0) {
            banned = true;
            break;
        }
    }
    pthread_mutex_unlock(&host->lock);

    return banned;
}

static bool host_add_ban_entry(host_t *host, const char *username,
                               const char *ip)
{
    if (host == NULL) {
        return false;
    }

    bool added = false;
    pthread_mutex_lock(&host->lock);
    if (host->ban_count >= SSH_CHATTER_MAX_BANS) {
        pthread_mutex_unlock(&host->lock);
        return false;
    }

    if (ip != NULL && ip[0] != '\0' &&
        host_is_protected_ip_unlocked(host, ip)) {
        pthread_mutex_unlock(&host->lock);
        return true;
    }
    if (ip != NULL && ip[0] != '\0' && strchr(ip, '/') != NULL) {
        for (size_t idx = 0; idx < host->protected_ip_count &&
                             idx < SSH_CHATTER_MAX_PROTECTED_IPS;
             ++idx) {
            if (host_cidr_contains_ip(ip, host->protected_ips[idx])) {
                pthread_mutex_unlock(&host->lock);
                return true;
            }
        }
    }

    for (size_t idx = 0; idx < host->ban_count; ++idx) {
        const bool username_match = (username != NULL && username[0] != '\0' &&
                                     strncmp(host->bans[idx].username, username,
                                             SSH_CHATTER_USERNAME_LEN) == 0);
        const bool ip_match =
            (ip != NULL && ip[0] != '\0' &&
             strncmp(host->bans[idx].ip, ip, SSH_CHATTER_IP_LEN) == 0);
        if (username_match || ip_match) {
            pthread_mutex_unlock(&host->lock);
            return true;
        }
    }

    strncpy(host->bans[host->ban_count].username,
            username != NULL ? username : "", SSH_CHATTER_USERNAME_LEN - 1U);
    host->bans[host->ban_count].username[SSH_CHATTER_USERNAME_LEN - 1U] = '\0';
    strncpy(host->bans[host->ban_count].ip, ip != NULL ? ip : "",
            SSH_CHATTER_IP_LEN - 1U);
    host->bans[host->ban_count].ip[SSH_CHATTER_IP_LEN - 1U] = '\0';
    ++host->ban_count;
    added = true;

    host_ban_state_save_locked(host);

    pthread_mutex_unlock(&host->lock);
    return added;
}

static bool host_remove_ban_entry(host_t *host, const char *token)
{
    if (host == NULL || token == NULL || token[0] == '\0') {
        return false;
    }

    bool removed = false;
    pthread_mutex_lock(&host->lock);
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
    pthread_mutex_unlock(&host->lock);

    return removed;
}

static bool
session_parse_localized_command(session_ctx_t *ctx,
                                const session_command_alias_t *alias,
                                const char *line, const char **arguments)
{
    if (alias == NULL) {
        return false;
    }

    if (session_parse_command(line, alias->canonical, arguments)) {
        return true;
    }

    session_ui_language_t language = session_ui_language_current(ctx);
    const char *preferred = session_command_alias_for_language(alias, language);
    if (preferred != NULL && strcmp(preferred, alias->canonical) != 0) {
        if (session_parse_command(line, preferred, arguments)) {
            return true;
        }
    }

    for (size_t idx = 0; idx < SESSION_UI_LANGUAGE_COUNT; ++idx) {
        const char *localized = alias->localized[idx];
        if (localized == NULL || localized[0] == '\0' ||
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
    if (line == NULL || command == NULL) {
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

static void session_dispatch_command(session_ctx_t *ctx, const char *line)
{
    const char *args = NULL;
    const char *effective_line = line;

    // Handle double slash commands by effectively removing the first slash
    if (line != NULL && line[0] == '/' && line[1] == '/') {
        effective_line = line + 1;
    }

    if (session_parse_command_any(ctx, "/help", effective_line, &args)) {
        session_print_help(ctx);
        return;
    }

    else if (session_parse_command_any(ctx, "/ssh-chat-server", effective_line,
                                       &args)) {
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

    else if (session_parse_command_any(ctx, "/grant", effective_line, &args)) {
        session_handle_grant(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/kick", effective_line, &args)) {
        session_handle_kick(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/set-sync-url", effective_line,
                                       &args)) {
        if (args == NULL || *args == '\0') {
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
        if (args == NULL || *args == '\0') {
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
        ctx->ops->handle_exit(ctx);
        return;
    }

    else if (session_parse_command_any(ctx, "/nick", effective_line, &args)) {
        ctx->ops->handle_nick(ctx, args);
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

    else if (session_parse_command_any(ctx, "/profilepic", effective_line,
                                       &args)) {
        session_handle_profile_picture(ctx, args);
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
    } else if (session_parse_command_any(ctx, "/eliza", effective_line,
                                         &args)) {
        session_handle_eliza(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/breaking", effective_line,
                                         &args)) {
        session_handle_breaking_alerts(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/chat-spacing", effective_line,
                                         &args)) {
        session_handle_chat_spacing(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/retro", effective_line, &args)) {
        session_handle_retro(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/mode", effective_line, &args)) {
        ctx->ops->handle_mode(ctx, args);
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
            session_game_suspend(ctx, NULL);
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
    } else if (session_parse_command_any(ctx, "/mrcserver", effective_line,
                                         &args)) {
        session_handle_mrcserver(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/birthday", effective_line,
                                         &args)) {
        session_handle_birthday(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/soulmate", effective_line,
                                         &args)) {
        if (*args != '\0') {
            session_send_system_line(ctx, "Usage: /soulmate");
        } else {
            session_handle_soulmate(ctx);
        }
        return;
    } else if (session_parse_command_any(ctx, "/setpw", effective_line,
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
            session_handle_vote_command(ctx, NULL, false);
        } else {
            session_handle_vote_command(ctx, args, false);
        }
        return;
    } else if (session_parse_command_any(ctx, "/vote", effective_line, &args)) {
        if (*args == '\0') {
            session_handle_vote_command(ctx, NULL, true);
        } else {
            session_handle_vote_command(ctx, args, true);
        }
        return;
    } else if (session_parse_command_any(ctx, "/elect", effective_line,
                                         &args)) {
        if (*args == '\0') {
            session_handle_elect_command(ctx, NULL);
        } else {
            session_handle_elect_command(ctx, args);
        }
        return;
    } else if (session_parse_command(effective_line, "/rss", &args)) {
        session_handle_rss(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/bbs", effective_line, &args)) {
        session_handle_bbs(ctx,
                           (args != NULL && args[0] != '\0') ? args : NULL);
        return;
    }

    else if (session_parse_command_any(ctx, "/resetpw", effective_line,
                                       &args)) {
        session_handle_resetpw(ctx, args);
        return;
    }

    else if (effective_line[0] == '/') {
        if (isdigit((unsigned char)effective_line[1])) {
            char *endptr = NULL;
            unsigned long vote_index = strtoul(effective_line + 1, &endptr, 10);
            const unsigned long max_vote = sizeof(ctx->owner->poll.options) /
                                           sizeof(ctx->owner->poll.options[0]);
            if (vote_index >= 1UL && vote_index <= max_vote) {
                while (endptr != NULL && (*endptr == ' ' || *endptr == '\t')) {
                    ++endptr;
                }
                if (endptr == NULL || *endptr == '\0') {
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

            const char *arguments = NULL;
            if (!session_parse_command_any(ctx, canonical, effective_line,
                                           &arguments)) {
                continue;
            }

            session_handle_reaction(ctx, idx, arguments);
            return;
        }
    }

    const session_ui_locale_t *locale = session_ui_get_locale(ctx);
    const char *format =
        (locale->unknown_command != NULL && locale->unknown_command[0] != '\0')
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
    if (text == NULL) {
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
    if (token == NULL || length == 0U) {
        return input;
    }

    token[0] = '\0';
    if (input == NULL) {
        return NULL;
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
    if (ctx == NULL || ctx->owner == NULL) {
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
    if (host == NULL || username == NULL || username[0] == '\0') {
        return false;
    }

    if (!host->user_data_ready) {
        return false;
    }

    bool success = false;
    if (host->user_data_lock_initialized) {
        pthread_mutex_lock(&host->user_data_lock);
    }

    if (create_if_missing) {
        success =
            user_data_ensure_exists(host->user_data_root, username, ip, record);
    } else {
        success = user_data_load(host->user_data_root, username, ip, record);
    }

    if (host->user_data_lock_initialized) {
        pthread_mutex_unlock(&host->user_data_lock);
    }

    return success;
}

static bool host_lookup_last_ip(host_t *host, const char *username, char *ip,
                                size_t length)
{
    if (ip != NULL && length > 0U) {
        ip[0] = '\0';
    }

    if (host == NULL || username == NULL || username[0] == '\0' || ip == NULL ||
        length == 0U) {
        return false;
    }

    if (host_lookup_member_ip(host, username, ip, length)) {
        return true;
    }

    user_data_record_t record;
    if (!host_user_data_load_existing(host, username, NULL, &record, false)) {
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
    if (error != NULL && error_length > 0U) {
        error[0] = '\0';
    }

    if (host == NULL || recipient == NULL || recipient[0] == '\0' ||
        message == NULL || message[0] == '\0') {
        if (error != NULL && error_length > 0U) {
            snprintf(error, error_length, "%s", "Invalid mailbox parameters.");
        }
        return false;
    }

    if (!host->user_data_ready) {
        if (error != NULL && error_length > 0U) {
            snprintf(error, error_length, "%s", "Mailbox storage unavailable.");
        }
        return false;
    }

    char resolved_ip[SSH_CHATTER_IP_LEN];
    resolved_ip[0] = '\0';
    if (recipient_ip != NULL && recipient_ip[0] != '\0') {
        snprintf(resolved_ip, sizeof(resolved_ip), "%s", recipient_ip);
    }

    const bool target_is_lan_ops =
        host_is_lan_operator_username(host, recipient);
    if (target_is_lan_ops) {
        session_ctx_t *target_session =
            chat_room_find_user(&host->room, recipient);
        if (target_session == NULL || !target_session->user.is_lan_operator) {
            if (error != NULL && error_length > 0U) {
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
        if (target_session != NULL) {
            snprintf(resolved_ip, sizeof(resolved_ip), "%s",
                     target_session->client_ip);
        }
    }

    if (resolved_ip[0] == '\0') {
        if (error != NULL && error_length > 0U) {
            snprintf(
                error, error_length, "%s",
                "Provide the recipient's IP (name@ip) when they are offline.");
        }
        return false;
    }

    user_data_record_t record;
    if (!host_user_data_load_existing(host, recipient, resolved_ip, &record,
                                      true)) {
        if (error != NULL && error_length > 0U) {
            snprintf(error, error_length, "Unable to open mailbox for %s.",
                     recipient);
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
    time_t now = time(NULL);
    if (now == (time_t)-1) {
        now = 0;
    }
    entry->timestamp = (uint64_t)now;
    if (sender != NULL && sender[0] != '\0') {
        snprintf(entry->sender, sizeof(entry->sender), "%s", sender);
    } else {
        snprintf(entry->sender, sizeof(entry->sender), "%s", "system");
    }
    snprintf(entry->message, sizeof(entry->message), "%s", message);
    record.last_updated = (uint64_t)now;
    snprintf(record.last_ip, sizeof(record.last_ip), "%s", resolved_ip);

    bool success;
    if (host->user_data_lock_initialized) {
        pthread_mutex_lock(&host->user_data_lock);
    }
    success = user_data_save(host->user_data_root, &record, resolved_ip);
    if (host->user_data_lock_initialized) {
        pthread_mutex_unlock(&host->user_data_lock);
    }

    if (!success) {
        if (error != NULL && error_length > 0U) {
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
    if (text == NULL) {
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
    if (text == NULL) {
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

static bool rss_tag_is_valid(const char *tag)
{
    if (tag == NULL || tag[0] == '\0') {
        return false;
    }

    for (const char *cursor = tag; *cursor != '\0'; ++cursor) {
        const char ch = *cursor;
        if (!(isalnum((unsigned char)ch) || ch == '-' || ch == '_' ||
              ch == '.')) {
            return false;
        }
    }
    return true;
}

// Reset a poll structure to a neutral inactive state.
static void poll_state_reset(poll_state_t *poll)
{
    if (poll == NULL) {
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
    if (poll == NULL) {
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
    if (host == NULL || label == NULL || label[0] == '\0') {
        return NULL;
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

    return NULL;
}

// Either fetch an existing named poll or initialise a new slot for the provided label.
static named_poll_state_t *host_ensure_named_poll_locked(host_t *host,
                                                         const char *label)
{
    if (host == NULL || label == NULL || label[0] == '\0') {
        return NULL;
    }

    named_poll_state_t *existing = host_find_named_poll_locked(host, label);
    if (existing != NULL) {
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

    return NULL;
}

// Recompute how many named polls are active so list summaries remain accurate.
static void host_recount_named_polls_locked(host_t *host)
{
    if (host == NULL) {
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
static bool poll_label_is_valid(const char *label)
{
    if (label == NULL || label[0] == '\0') {
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
    if (text == NULL) {
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
    if (input == NULL || output == NULL || length == 0U) {
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

    if (output[0] == '/' || strstr(output, "..") != NULL) {
        return false;
    }

    return true;
}

static bool timezone_resolve_identifier(const char *input, char *resolved,
                                        size_t length)
{
    if (input == NULL || input[0] == '\0' || resolved == NULL || length == 0U) {
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

    char *saveptr = NULL;
    char *segment = strtok_r(working, "/", &saveptr);
    if (segment == NULL) {
        return false;
    }

    while (segment != NULL) {
        DIR *dir = opendir(current_dir);
        if (dir == NULL) {
            return false;
        }

        bool found = false;
        char matched[NAME_MAX + 1];
        matched[0] = '\0';
        struct dirent *entry = NULL;
        while ((entry = readdir(dir)) != NULL) {
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

        segment = strtok_r(NULL, "/", &saveptr);
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
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }

    for (size_t idx = 0U; idx < sizeof(OS_CATALOG) / sizeof(OS_CATALOG[0]);
         ++idx) {
        if (strcasecmp(OS_CATALOG[idx].name, name) == 0) {
            return &OS_CATALOG[idx];
        }
    }

    return NULL;
}

static const char *lookup_color_code(const color_entry_t *entries,
                                     size_t entry_count, const char *name)
{
    if (entries == NULL || name == NULL) {
        return NULL;
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
        char *endptr = NULL;
        unsigned long code = strtoul(digits, &endptr, 10);
        if (endptr != NULL && *endptr == '\0' && code <= 255U) {
            char(*slot)[16] = &fg_cache[fg_cache_index];
            fg_cache_index = (fg_cache_index + 1U) %
                             (sizeof(fg_cache) / sizeof(fg_cache[0]));
            ansi_256(*slot, sizeof(fg_cache[0]), (unsigned int)code);
            return *slot;
        }
    }

    if (strncasecmp(name, "xterm-bg:", 9) == 0) {
        const char *digits = name + 9;
        char *endptr = NULL;
        unsigned long code = strtoul(digits, &endptr, 10);
        if (endptr != NULL && *endptr == '\0' && code <= 255U) {
            char(*slot)[16] = &bg_cache[bg_cache_index];
            bg_cache_index = (bg_cache_index + 1U) %
                             (sizeof(bg_cache) / sizeof(bg_cache[0]));
            ansi_bg_256(*slot, sizeof(bg_cache[0]), (unsigned int)code);
            return *slot;
        }
    }

    return NULL;
}

static const palette_descriptor_t *palette_find_descriptor(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }

    for (size_t idx = 0U;
         idx < sizeof(PALETTE_DEFINITIONS) / sizeof(PALETTE_DEFINITIONS[0]);
         ++idx) {
        if (strcasecmp(PALETTE_DEFINITIONS[idx].id, name) == 0) {
            return &PALETTE_DEFINITIONS[idx];
        }
    }

    return NULL;
}

static bool palette_apply_to_session(session_ctx_t *ctx,
                                     const palette_descriptor_t *descriptor)
{
    if (ctx == NULL || descriptor == NULL) {
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

    if (user_color_code == NULL || user_highlight_code == NULL ||
        system_fg_code == NULL || system_bg_code == NULL ||
        system_highlight_code == NULL) {
        return false;
    }

    ctx->user_color_code = user_color_code;
    ctx->user_highlight_code = user_highlight_code;
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
    if (host == NULL || descriptor == NULL) {
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

    if (user_color_code == NULL) {
        user_color_code = ANSI_GREEN;
    }
    if (user_highlight_code == NULL) {
        user_highlight_code = ANSI_BG_DEFAULT;
    }
    if (system_fg_code == NULL) {
        system_fg_code = ANSI_WHITE;
    }
    if (system_bg_code == NULL) {
        system_bg_code = ANSI_BG_BLUE;
    }
    if (system_highlight_code == NULL) {
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
    if (token == NULL || value == NULL) {
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
    if (ctx == NULL) {
        return false;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        return ctx->telnet_fd >= 0 && !ctx->telnet_eof;
    }

    return ctx->channel != NULL;
}

static bool session_transport_is_open(const session_ctx_t *ctx)
{
    if (ctx == NULL) {
        return false;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        return ctx->telnet_fd >= 0 && !ctx->telnet_eof;
    }

    return ctx->channel != NULL && ssh_channel_is_open(ctx->channel);
}

static bool session_transport_is_eof(const session_ctx_t *ctx)
{
    if (ctx == NULL) {
        return true;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        return ctx->telnet_eof || ctx->telnet_fd < 0;
    }

    return ctx->channel == NULL || ssh_channel_is_eof(ctx->channel);
}

static void session_transport_request_close(session_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        if (ctx->telnet_fd >= 0) {
            shutdown(ctx->telnet_fd, SHUT_RDWR);
        }
        ctx->telnet_eof = true;
        return;
    }

    if (ctx->channel != NULL) {
        ssh_channel_send_eof(ctx->channel);
        ssh_channel_close(ctx->channel);
    }
}

static void session_close_channel(session_ctx_t *ctx)
{
    if (ctx == NULL) {
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

    if (ctx->channel == NULL) {
        return;
    }

    ssh_channel_send_eof(ctx->channel);
    ssh_channel_close(ctx->channel);
    ssh_channel_free(ctx->channel);
    ctx->channel = NULL;
}

static void session_reset_for_retry(session_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    session_close_channel(ctx);
    ctx->should_exit = false;
    ctx->username_conflict = false;
    ctx->has_joined_room = false;
    ctx->prelogin_banner_rendered = false;
    ctx->input_length = 0U;
    ctx->input_buffer[0] = '\0';
    ctx->input_escape_active = false;
    ctx->input_escape_length = 0U;
    ctx->input_escape_buffer[0] = '\0';
    ctx->bbs_post_pending = false;
    ctx->pending_bbs_title[0] = '\0';
    ctx->pending_bbs_body[0] = '\0';
    ctx->pending_bbs_body_length = 0U;
    ctx->pending_bbs_tag_count = 0U;
    memset(ctx->pending_bbs_tags, 0, sizeof(ctx->pending_bbs_tags));
    ctx->bbs_view_active = false;
    ctx->bbs_view_post_id = 0U;
    ctx->bbs_view_scroll_offset = 0U;
    ctx->bbs_view_total_lines = 0U;
    ctx->bbs_view_notice_pending = false;
    ctx->bbs_view_notice[0] = '\0';
    ctx->bbs_rendering_editor = false;
    ctx->bbs_breaking_count = 0U;
    memset(ctx->bbs_breaking_messages, 0, sizeof(ctx->bbs_breaking_messages));
    ctx->telnet_terminal_type_requested = false;
    ctx->terminal_type[0] = '\0';
    ctx->prefer_cp437_output = false;
    ctx->cp437_override = SESSION_CP437_OVERRIDE_NONE;
    ctx->cp437_input_enabled = false;
    session_asciiart_reset(ctx);
    ctx->asciiart_has_cooldown = false;
    ctx->last_asciiart_post.tv_sec = 0;
    ctx->last_asciiart_post.tv_nsec = 0;
    session_game_tetris_reset(&ctx->game.tetris);
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

static int host_telnet_open_socket(host_t *host)
{
    if (host == NULL || host->telnet.port[0] == '\0') {
        return -1;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    const char *bind_addr =
        host->telnet.bind_address[0] != '\0' ? host->telnet.bind_address : NULL;
    struct addrinfo *result = NULL;
    int rc = getaddrinfo(bind_addr, host->telnet.port, &hints, &result);
    if (rc != 0) {
        printf("[telnet] failed to resolve %s:%s (%s)\n",
               bind_addr != NULL ? bind_addr : "*", host->telnet.port,
               gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = result; ai != NULL; ai = ai->ai_next) {
        int candidate = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (candidate < 0) {
            continue;
        }

        int enable = 1;
        setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, &enable,
                   sizeof(enable));

        if (bind(candidate, ai->ai_addr, ai->ai_addrlen) != 0) {
            close(candidate);
            continue;
        }

        if (listen(candidate, 16) != 0) {
            close(candidate);
            continue;
        }

        fd = candidate;
        break;
    }

    freeaddrinfo(result);
    return fd;
}

static void *host_telnet_thread(void *arg)
{
    host_t *host = (host_t *)arg;
    if (host == NULL) {
        return NULL;
    }

    atomic_store(&host->telnet.running, true);

    while (!atomic_load(&host->telnet.stop) &&
           (host->shutdown_flag == NULL || *host->shutdown_flag == 0)) {
        if (host->telnet.fd < 0) {
            int fd = host_telnet_open_socket(host);
            if (fd < 0) {
                struct timespec backoff = {.tv_sec = 1, .tv_nsec = 0};
                host_sleep_uninterruptible(&backoff);
                continue;
            }

            host->telnet.fd = fd;
            const char *display_addr = host->telnet.bind_address[0] != '\0'
                                           ? host->telnet.bind_address
                                           : "*";
            printf("[telnet] listening on %s:%s\n", display_addr,
                   host->telnet.port);
        }

        struct sockaddr_storage addr;
        socklen_t addr_len = sizeof(addr);
        int client_fd =
            accept(host->telnet.fd, (struct sockaddr *)&addr, &addr_len);
        if (client_fd < 0) {
            int accept_error = errno;
            if (accept_error == EINTR) {
                continue;
            }
            if (atomic_load(&host->telnet.stop) ||
                (host->shutdown_flag != NULL && *host->shutdown_flag != 0)) {
                break;
            }

            char message[256];
            const char *system_message = strerror(accept_error);
            if (system_message != NULL && system_message[0] != '\0') {
                snprintf(message, sizeof(message), "accept failed: %s",
                         system_message);
            } else {
                snprintf(message, sizeof(message), "accept failed (code %d)",
                         accept_error);
            }
            humanized_log_error("telnet", message, accept_error);

            bool fatal_socket_error = false;
            bool should_backoff = true;
            switch (accept_error) {
            case EINTR:
                continue;
            case EAGAIN:
#ifdef EWOULDBLOCK
#if EWOULDBLOCK != EAGAIN
            case EWOULDBLOCK:
#endif
#endif
            case ECONNABORTED:
            case ECONNRESET:
            case ETIMEDOUT:
#ifdef ENOTCONN
            case ENOTCONN:
#endif
#ifdef EHOSTUNREACH
            case EHOSTUNREACH:
#endif
#ifdef ENETDOWN
            case ENETDOWN:
#endif
#ifdef ENETRESET
            case ENETRESET:
#endif
#ifdef ENETUNREACH
            case ENETUNREACH:
#endif
            case EPIPE:
#ifdef EPROTO
            case EPROTO:
#endif
                break;
            case EBADF:
            case ENOTSOCK:
            case EINVAL:
            case EMFILE:
            case ENFILE:
#ifdef ENOBUFS
            case ENOBUFS:
#endif
#ifdef ENOMEM
            case ENOMEM:
#endif
            default:
                fatal_socket_error = true;
                should_backoff = false;
                break;
            }

            if (fatal_socket_error) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);

                double elapsed_seconds = 0.0;
                if (host->telnet.last_error_time.tv_sec != 0 ||
                    host->telnet.last_error_time.tv_nsec != 0L) {
                    struct timespec elapsed =
                        timespec_diff(&now, &host->telnet.last_error_time);
                    elapsed_seconds = (double)elapsed.tv_sec +
                                      (double)elapsed.tv_nsec / 1000000000.0;
                    if (elapsed_seconds >= TELNET_STABLE_RESET_SECONDS &&
                        host->telnet.restart_attempts > 0U) {
                        printf("[telnet] listener stable for %.3f seconds; "
                               "clearing "
                               "restart backoff\n",
                               elapsed_seconds);
                        host->telnet.restart_attempts = 0U;
                    }
                }

                host->telnet.last_error_time = now;
                host->telnet.restart_attempts += 1U;

                if (host->telnet.fd >= 0) {
                    close(host->telnet.fd);
                    host->telnet.fd = -1;
                }

                if (elapsed_seconds > 0.0) {
                    printf(
                        "[telnet] last fatal error occurred %.3f seconds ago\n",
                        elapsed_seconds);
                }

                printf("[telnet] restarting listener after socket error "
                       "(attempt %u)\n",
                       host->telnet.restart_attempts);

                struct timespec restart_delay = {
                    .tv_sec =
                        host->telnet.restart_attempts < 5U
                            ? 1L
                            : (host->telnet.restart_attempts < 10U ? 5L : 30L),
                    .tv_nsec = 0L,
                };
                host_sleep_uninterruptible(&restart_delay);
                continue;
            }

            if (should_backoff) {
                struct timespec retry_delay = {
                    .tv_sec = 0,
                    .tv_nsec = 200000000L,
                };
                host_sleep_uninterruptible(&retry_delay);
            }
            continue;
        }

        if (atomic_load(&host->telnet.stop) ||
            (host->shutdown_flag != NULL && *host->shutdown_flag != 0)) {
            close(client_fd);
            break;
        }

        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        char peer_address[NI_MAXHOST];
        host_format_sockaddr((struct sockaddr *)&addr, addr_len, peer_address,
                             sizeof(peer_address));
        if (peer_address[0] == '\0') {
            snprintf(peer_address, sizeof(peer_address), "%s", "unknown");
        }

        printf("[telnet] accepted client from %s\n", peer_address);

        session_ctx_t *ctx = session_create();
        if (ctx == NULL) {
            humanized_log_error("telnet", "failed to allocate session context",
                                ENOMEM);
            close(client_fd);
            continue;
        }
        ctx->ops = &telnet_session_ops;
        ctx->transport_kind = SESSION_TRANSPORT_TELNET;
        ctx->telnet_fd = client_fd;
        ctx->telnet_negotiated = false;
        ctx->telnet_eof = false;
        ctx->telnet_pending_valid = false;
        pthread_mutexattr_t lock_attr;
        pthread_mutexattr_init(&lock_attr);
        pthread_mutexattr_settype(&lock_attr, PTHREAD_MUTEX_RECURSIVE);
        int mutex_error = pthread_mutex_init(&ctx->output_lock, &lock_attr);
        pthread_mutexattr_destroy(&lock_attr);
        if (mutex_error != 0) {
            humanized_log_error("telnet",
                                "failed to initialise session output lock",
                                mutex_error);
            session_destroy(ctx);
            continue;
        }
        ctx->output_lock_initialized = true;
        ctx->owner = host;
        if (pthread_mutex_init(&ctx->channel_mutex, NULL) == 0) {
            ctx->channel_mutex_initialized = true;
        } else {
            humanized_log_error("session", "failed to initialize channel mutex",
                                errno != 0 ? errno : ENOMEM);
        }
        ctx->auth = (auth_profile_t){0};
        snprintf(ctx->client_ip, sizeof(ctx->client_ip), "%.*s",
                 (int)sizeof(ctx->client_ip) - 1, peer_address);
        ctx->input_mode = SESSION_INPUT_MODE_CHAT;

        session_ui_language_t geo_language = session_client_geo_language(ctx);
        if (geo_language != SESSION_UI_LANGUAGE_COUNT) {
            ctx->ui_language = geo_language;
        } else {
            ctx->ui_language = SESSION_UI_LANGUAGE_KO;
        }

        pthread_mutex_lock(&host->lock);
        ++host->connection_count;
        snprintf(ctx->user.name, sizeof(ctx->user.name), "Guest%zu",
                 host->connection_count);
        ctx->user.is_operator = false;
        ctx->user.is_lan_operator = false;
        pthread_mutex_unlock(&host->lock);

        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, session_thread, ctx) != 0) {
            humanized_log_error("telnet", "failed to spawn session thread",
                                errno);
            session_destroy(ctx);
            continue;
        }

        pthread_detach(thread_id);
    }

    int listener_fd = host->telnet.fd;
    host->telnet.fd = -1;
    if (listener_fd >= 0) {
        close(listener_fd);
    }

    atomic_store(&host->telnet.running, false);
    return NULL;
}

static bool host_telnet_listener_start(host_t *host, const char *bind_addr,
                                       const char *port)
{
    if (host == NULL || port == NULL || port[0] == '\0') {
        return false;
    }

    if (host->telnet.thread_initialized) {
        const bool same_port =
            strncmp(host->telnet.port, port, sizeof(host->telnet.port)) == 0;
        bool same_bind = false;
        if (bind_addr == NULL || bind_addr[0] == '\0') {
            same_bind = host->telnet.bind_address[0] == '\0';
        } else {
            same_bind = strncmp(host->telnet.bind_address, bind_addr,
                                sizeof(host->telnet.bind_address)) == 0;
        }

        if (same_port && same_bind && atomic_load(&host->telnet.running)) {
            const char *display_addr = host->telnet.bind_address[0] != '\0'
                                           ? host->telnet.bind_address
                                           : "*";
            printf("[telnet] listener already active on %s:%s\n", display_addr,
                   host->telnet.port);
            return true;
        }

        host_telnet_listener_stop(host);
    }

    if (bind_addr != NULL && bind_addr[0] != '\0') {
        snprintf(host->telnet.bind_address, sizeof(host->telnet.bind_address),
                 "%s", bind_addr);
    } else {
        host->telnet.bind_address[0] = '\0';
    }
    snprintf(host->telnet.port, sizeof(host->telnet.port), "%s", port);
    host->telnet.enabled = true;
    host->telnet.fd = -1;
    host->telnet.restart_attempts = 0U;
    host->telnet.last_error_time.tv_sec = 0;
    host->telnet.last_error_time.tv_nsec = 0L;
    atomic_store(&host->telnet.stop, false);

    if (pthread_create(&host->telnet.thread, NULL, host_telnet_thread, host) !=
        0) {
        humanized_log_error("telnet", "failed to start telnet listener", errno);
        host->telnet.enabled = false;
        return false;
    }

    host->telnet.thread_initialized = true;
    return true;
}

static void host_telnet_listener_stop(host_t *host)
{
    if (host == NULL) {
        return;
    }

    if (!host->telnet.thread_initialized) {
        host->telnet.enabled = false;
        host->telnet.fd = -1;
        host->telnet.bind_address[0] = '\0';
        host->telnet.port[0] = '\0';
        atomic_store(&host->telnet.running, false);
        atomic_store(&host->telnet.stop, false);
        return;
    }

    const char *display_addr =
        host->telnet.bind_address[0] != '\0' ? host->telnet.bind_address : "*";
    printf("[telnet] stopping listener on %s:%s\n", display_addr,
           host->telnet.port);

    atomic_store(&host->telnet.stop, true);
    if (host->telnet.fd >= 0) {
        shutdown(host->telnet.fd, SHUT_RDWR);
    }

    int join_result = pthread_join(host->telnet.thread, NULL);
    if (join_result != 0) {
        humanized_log_error("telnet", "failed to join telnet listener",
                            join_result);
    }

    host->telnet.thread_initialized = false;
    host->telnet.enabled = false;
    atomic_store(&host->telnet.running, false);
    atomic_store(&host->telnet.stop, false);

    if (host->telnet.fd >= 0) {
        close(host->telnet.fd);
        host->telnet.fd = -1;
    }

    host->telnet.bind_address[0] = '\0';
    host->telnet.port[0] = '\0';
}

static bool session_attempt_handshake_restart(session_ctx_t *ctx,
                                              unsigned int *attempts)
{
    if (ctx == NULL || attempts == NULL) {
        return false;
    }

    if (!session_transport_active(ctx)) {
        return false;
    }

    if (*attempts >= SSH_CHATTER_HANDSHAKE_RETRY_LIMIT) {
        return false;
    }

    ++(*attempts);
    printf("[session] retrying handshake (attempt %u/%u)\n", *attempts,
           SSH_CHATTER_HANDSHAKE_RETRY_LIMIT);
    session_reset_for_retry(ctx);
    session_apply_theme_defaults(ctx);
    struct timespec backoff = {
        .tv_sec = 0,
        .tv_nsec = 200000000L,
    };
    host_sleep_uninterruptible(&backoff);
    return true;
}

static void session_cleanup(session_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->user_data_loaded) {
        (void)session_user_data_commit(ctx);
    }

    session_translation_worker_shutdown(ctx);
    if (ctx->channel_mutex_initialized) {
        pthread_mutex_destroy(&ctx->channel_mutex);
        ctx->channel_mutex_initialized = false;
    }
    if (ctx->transport_kind == SESSION_TRANSPORT_SSH && ctx->channel != NULL) {
        ssh_channel_request_send_exit_status(ctx->channel, ctx->exit_status);
    }
    session_close_channel(ctx);

    if (ctx->output_lock_initialized) {
        pthread_mutex_destroy(&ctx->output_lock);
        ctx->output_lock_initialized = false;
    }

    if (ctx->session != NULL) {
        ssh_disconnect(ctx->session);
        ssh_free(ctx->session);
        ctx->session = NULL;
    }
}

static void session_destroy(session_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    session_cleanup(ctx);

#if !(defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC)
    free(ctx);
#endif
}

session_ctx_t *host_session_create_for_testing(host_t *host,
                                               const char *username,
                                               const char *ip, bool is_operator)
{
    if (host == NULL) {
        return NULL;
    }

    session_ctx_t *ctx = session_create();
    if (ctx == NULL) {
        return NULL;
    }

    ctx->owner = host;
    ctx->ops = &ssh_session_ops;
    ctx->transport_kind = SESSION_TRANSPORT_SSH;
    ctx->telnet_fd = -1;
    ctx->input_mode = SESSION_INPUT_MODE_CHAT;
    ctx->has_joined_room = true;
    ctx->user.is_authenticated = true;
    ctx->user.is_operator = is_operator;
    ctx->ui_language = SESSION_UI_LANGUAGE_EN;

    const char *resolved_name =
        (username != NULL && username[0] != '\0') ? username : "tester";
    snprintf(ctx->user.name, sizeof(ctx->user.name), "%s", resolved_name);

    if (ip != NULL && ip[0] != '\0') {
        snprintf(ctx->client_ip, sizeof(ctx->client_ip), "%s", ip);
    } else {
        snprintf(ctx->client_ip, sizeof(ctx->client_ip), "%s", "127.0.0.1");
    }

    pthread_mutexattr_t lock_attr;
    pthread_mutexattr_init(&lock_attr);
    pthread_mutexattr_settype(&lock_attr, PTHREAD_MUTEX_RECURSIVE);
    if (pthread_mutex_init(&ctx->output_lock, &lock_attr) != 0) {
        pthread_mutexattr_destroy(&lock_attr);
        session_destroy(ctx);
        return NULL;
    }
    pthread_mutexattr_destroy(&lock_attr);
    ctx->output_lock_initialized = true;

    if (pthread_mutex_init(&ctx->channel_mutex, NULL) != 0) {
        session_destroy(ctx);
        return NULL;
    }
    ctx->channel_mutex_initialized = true;

    session_apply_theme_defaults(ctx);

    return ctx;
}

void host_session_destroy_for_testing(session_ctx_t *ctx)
{
    session_destroy(ctx);
}

static void *session_thread(void *arg)
{
    session_ctx_t *ctx = (session_ctx_t *)arg;
    if (ctx == NULL) {
        return NULL;
    }

    sshc_memory_context_t *memory_scope = NULL;
    if (ctx->owner != NULL) {
        memory_scope = sshc_memory_context_push(ctx->owner->memory_context);
    }

#define SESSION_THREAD_RETURN(value)                                           \
    do {                                                                       \
        if (memory_scope != NULL) {                                            \
            sshc_memory_context_pop(memory_scope);                             \
        }                                                                      \
        return (value);                                                        \
    } while (0)

    ctx->exit_status = EXIT_FAILURE;
    session_apply_theme_defaults(ctx);

    bool authenticated = false;
    unsigned int handshake_retries = 0U;
    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        session_telnet_initialize(ctx);
        session_telnet_capture_startup_metadata(ctx);
        authenticated = true;
    }

    while (ctx->transport_kind == SESSION_TRANSPORT_SSH) {
        if (!authenticated) {
            if (session_authenticate(ctx) != 0) {
                humanized_log_error("session", "authentication failed", EACCES);
                session_destroy(ctx);
                SESSION_THREAD_RETURN(NULL);
            }
            authenticated = true;
            ctx->user.is_authenticated = true;
            if (ctx->password_not_set) {
                session_send_raw_text(
                    ctx, "Welcome! Your account has no password set. Please "
                         "use the /password command to set one.\r\n");
            }
        }

        if (session_accept_channel(ctx) != 0) {
            humanized_log_error("session", "failed to open channel", EIO);
            if (session_attempt_handshake_restart(ctx, &handshake_retries)) {
                continue;
            }
            session_destroy(ctx);
            SESSION_THREAD_RETURN(NULL);
        }

        if (session_prepare_shell(ctx) != 0) {
            humanized_log_error("session", "shell negotiation failed", EPROTO);
            if (session_attempt_handshake_restart(ctx, &handshake_retries)) {
                continue;
            }
            session_destroy(ctx);
            SESSION_THREAD_RETURN(NULL);
        }

        break;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_SSH) {
        session_render_prelogin_banner(ctx);
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        if (!session_telnet_prompt_unicode_check(ctx)) {
            session_send_system_line(
                ctx, "Well, you didn't answer. Proceeding with Unicode...");
        }
    }

    session_assign_lan_privileges(ctx);
    session_apply_granted_privileges(ctx);
    session_apply_saved_preferences(ctx);

    // Auto-nick for pure ASCII names if a preferred nickname is available
    if (ctx->user.is_authenticated && is_pure_ascii(ctx->user.name) &&
        ctx->user_data_loaded && ctx->user_data.preferred_nickname[0] != '\0' &&
        strcasecmp(ctx->user.name, ctx->user_data.preferred_nickname) != 0) {
        char nick_command[SSH_CHATTER_MAX_INPUT_LEN];
        snprintf(nick_command, sizeof(nick_command), "/nick %s",
                 ctx->user_data.preferred_nickname);
        ctx->ops->dispatch_command(ctx, nick_command);
    }

    bool captcha_enabled = false;
    if (ctx->owner != NULL) {
        captcha_enabled = atomic_load(&ctx->owner->captcha_enabled);
    }
    const bool captcha_exempt = session_is_captcha_exempt(ctx);
    if (captcha_enabled && !captcha_exempt && !session_run_captcha(ctx)) {
        session_destroy(ctx);
        SESSION_THREAD_RETURN(NULL);
    }

    if (host_is_ip_banned(ctx->owner, ctx->client_ip)) {
        session_send_system_line(ctx, "You are banned from this server.");
        session_destroy(ctx);
        SESSION_THREAD_RETURN(NULL);
    }

    const bool banned_username =
        host_is_username_banned(ctx->owner, ctx->user.name);
    const bool system_reserved_username =
        host_is_system_reserved_username(ctx->owner, ctx->user.name);
    const bool lan_operator_reserved_username =
        host_is_lan_operator_username(ctx->owner, ctx->user.name);
    session_ctx_t *existing =
        chat_room_find_user(&ctx->owner->room, ctx->user.name);

    // Check if same IP and nickname - allow reconnection by kicking existing session
    bool should_kick_existing = false;
    if (existing != NULL && !banned_username && !system_reserved_username &&
        !lan_operator_reserved_username) {
        if (strcmp(existing->client_ip, ctx->client_ip) == 0) {
            should_kick_existing = true;
            printf("[reconnect] same IP+nickname detected: %s from %s, kicking "
                   "existing session\n",
                   ctx->user.name, ctx->client_ip);
        }
    }

    if (should_kick_existing) {
        // Kick the existing session
        char kick_message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(kick_message, sizeof(kick_message),
                 "You have been disconnected because a new connection from the "
                 "same IP is joining.");
        session_force_disconnect(existing, kick_message);

        // Wait a moment for the disconnect to process
        struct timespec disconnect_delay = {.tv_sec = 0,
                                            .tv_nsec = 100000000L}; // 100ms
        nanosleep(&disconnect_delay, NULL);

        // Clear the existing reference as it's being disconnected
        existing = NULL;
    }

    if (banned_username || system_reserved_username ||
        (lan_operator_reserved_username && !ctx->user.is_lan_operator) ||
        existing != NULL) {
        ctx->username_conflict = true;

        if (banned_username) {
            printf("[reject] banned nickname attempted: %s\n", ctx->user.name);
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message),
                     "The nickname '%s' is blocked for bot detection.",
                     ctx->user.name);
            session_send_system_line(ctx, message);
            session_send_system_line(
                ctx,
                "Reconnect with a different nickname before logging in again.");
            session_send_system_line(ctx, "Type /exit to quit.");
        } else if (system_reserved_username) {
            printf("[reject] system reserved username requested: %s\n",
                   ctx->user.name);
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message),
                     "The username '%s' is reserved for system use.",
                     ctx->user.name);
            session_send_system_line(ctx, message);
            session_send_system_line(
                ctx, "Reconnect with a different username by running: ssh "
                     "newname@<server> (or ssh -l newname <server>).");
            session_send_system_line(ctx, "Type /exit to quit.");
        } else if (lan_operator_reserved_username &&
                   !ctx->user.is_lan_operator) {
            printf("[reject] LAN operator reserved username requested: %s\n",
                   ctx->user.name);
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message),
                     "The username '%s' is reserved for LAN operators.",
                     ctx->user.name);
            session_send_system_line(ctx, message);
            session_send_system_line(
                ctx, "Reconnect with a different username by running: ssh "
                     "newname@<server> (or ssh -l newname <server>).");
            session_send_system_line(ctx, "Type /exit to quit.");
        } else {
            printf("[reject] username in use: %s\n", ctx->user.name);
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message),
                     "The username '%s' is already in use.", ctx->user.name);
            session_send_system_line(ctx, message);
            session_send_system_line(
                ctx, "Reconnect with a different username by running: ssh "
                     "newname@<server> (or ssh -l newname <server>).");
            session_send_system_line(ctx, "Type /exit to quit.");
        }
    } else {
        host_join_attempt_result_t join_result = host_register_join_attempt(
            ctx->owner, ctx->user.name, ctx->client_ip);
        if (join_result == HOST_JOIN_ATTEMPT_BAN) {
            session_send_system_line(
                ctx, "Rapid reconnect detected. You have been banned.");
            session_destroy(ctx);
            SESSION_THREAD_RETURN(NULL);
        }
        if (join_result == HOST_JOIN_ATTEMPT_KICK) {
            session_send_system_line(
                ctx, "Rapid reconnect detected. You have been kicked.");
            session_destroy(ctx);
            SESSION_THREAD_RETURN(NULL);
        }

        session_send_system_line(ctx, "Wait for a moment...");
        session_send_system_line(
            ctx, "For TELNET users: type /motd and follow the guide.");
        session_render_banner(ctx);
        struct timespec wait_time = {0, 0};
        size_t progress = host_prepare_join_delay(ctx->owner, &wait_time);
        if (progress == 0U) {
            progress = 1U;
        }
        if (progress > SSH_CHATTER_JOIN_BAR_MAX) {
            progress = SSH_CHATTER_JOIN_BAR_MAX;
        }
        char loading_line[SSH_CHATTER_MESSAGE_LIMIT];
        size_t written = 0U;
        for (size_t idx = 0;
             idx < progress && written + 1U < sizeof(loading_line); ++idx) {
            loading_line[written++] = '=';
        }
        if (written + 1U < sizeof(loading_line)) {
            loading_line[written++] = '>';
        }
        loading_line[written] = '\0';
        session_send_system_line(ctx, loading_line);
        if (wait_time.tv_sec != 0 || wait_time.tv_nsec != 0) {
            host_sleep_uninterruptible(&wait_time);
        }
        chat_room_add(&ctx->owner->room, ctx);
        ctx->has_joined_room = true;
        printf("[join] %s\n", ctx->user.name);

        ctx->history_scroll_position = 0U;
        host_refresh_motd(ctx->owner);
        const session_ui_locale_t *locale = session_ui_get_locale(ctx);
        const char *prefix = session_command_prefix(ctx);

        session_send_system_line(
            ctx,
            "Chat history starts hidden. Press the UpArrow/DownArrow keys to "
            "load older messages when you need them.");
        if (locale->help_scroll_hint != NULL &&
            locale->help_scroll_hint[0] != '\0') {
            session_send_system_line(ctx, locale->help_scroll_hint);
        }

        if (locale->welcome_help_hint != NULL &&
            locale->welcome_help_hint[0] != '\0') {
            const char *args[] = {prefix};
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            session_format_template(locale->welcome_help_hint, args,
                                    sizeof(args) / sizeof(args[0]), message,
                                    sizeof(message));
            session_send_system_line(ctx, message);
        }

        if (locale->help_hint_extra != NULL &&
            locale->help_hint_extra[0] != '\0') {
            const char *args[] = {prefix};
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            session_format_template(locale->help_hint_extra, args,
                                    sizeof(args) / sizeof(args[0]), message,
                                    sizeof(message));
            session_send_system_line(ctx, message);
        }

        if (locale->mode_usage != NULL && locale->mode_usage[0] != '\0') {
            const char *args[] = {prefix};
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            session_format_template(locale->mode_usage, args,
                                    sizeof(args) / sizeof(args[0]), message,
                                    sizeof(message));
            session_send_system_line(ctx, message);
        }

        if (locale->mode_explain_command != NULL &&
            locale->mode_explain_command[0] != '\0') {
            const char *args[] = {prefix};
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            session_format_template(locale->mode_explain_command, args,
                                    sizeof(args) / sizeof(args[0]), message,
                                    sizeof(message));
            session_send_system_line(ctx, message);
        }

        char join_message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(join_message, sizeof(join_message),
                 "* [%s] has joined the chat", ctx->user.name);
        host_history_record_system(ctx->owner, join_message, NULL);
        chat_room_broadcast(&ctx->owner->room, join_message, NULL);
    }

    session_clear_input_without_prompt(ctx);
    session_render_prompt(ctx, true);

    char buffer[SSH_CHATTER_MAX_INPUT_LEN];
    const int poll_timeout_ms = 100;
    while (!ctx->should_exit) {
        session_translation_flush_ready(ctx);

        int read_result =
            session_transport_read(ctx, buffer, sizeof(buffer) - 1U, 200);
        if (read_result == SSH_AGAIN) {
            if (ctx->game.active && ctx->game.type == SESSION_GAME_TETRIS) {
                session_game_tetris_process_timeout(ctx);
            }
            continue;
        }
        if (read_result == SSH_ERROR) {
            if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
                break;
            }
            read_result = session_channel_read_poll(
                ctx, buffer, sizeof(buffer) - 1U, poll_timeout_ms);
            if (read_result == SESSION_CHANNEL_TIMEOUT) {
                ctx->channel_error_retries = 0U;
                if (ctx->game.active && ctx->game.type == SESSION_GAME_TETRIS) {
                    session_game_tetris_process_timeout(ctx);
                }
                continue;
            }

            if (read_result == SSH_ERROR) {
                const char *error_message = ssh_get_error(ctx->session);
                bool unexpected_bytes_error = false;
                if (error_message != NULL && error_message[0] != '\0') {
                    unexpected_bytes_error =
                        strstr(error_message,
                               "unexpected bytes remain after decoding") !=
                        NULL;
                }

                if (unexpected_bytes_error) {
                    const char *username =
                        ctx->user.name[0] != '\0' ? ctx->user.name : "unknown";
                    printf("[session] channel decode error for %s: %s\n",
                           username, error_message);
                    break;
                }

                bool remote_disconnect = !session_transport_is_open(ctx) ||
                                         session_transport_is_eof(ctx);
                if (!remote_disconnect && error_message != NULL &&
                    error_message[0] != '\0') {
                    static const char *const kNetworkDisconnectTokens[] = {
                        "ssh_msg_disconnect", "disconnected by",
                        "connection reset",   "connection closed",
                        "broken pipe",        "socket error",
                    };
                    for (size_t token_idx = 0;
                         token_idx < (sizeof(kNetworkDisconnectTokens) /
                                      sizeof(kNetworkDisconnectTokens[0]));
                         ++token_idx) {
                        if (string_contains_case_insensitive(
                                error_message,
                                kNetworkDisconnectTokens[token_idx])) {
                            remote_disconnect = true;
                            break;
                        }
                    }
                }

                if (remote_disconnect) {
                    const char *username =
                        ctx->user.name[0] != '\0' ? ctx->user.name : "unknown";
                    const char *message =
                        (error_message != NULL && error_message[0] != '\0')
                            ? error_message
                            : "connection closed";
                    printf("[session] channel closed for %s: %s\n", username,
                           message);
                    break;
                }

                if (ctx->has_joined_room &&
                    ctx->channel_error_retries <
                        SSH_CHATTER_CHANNEL_RECOVERY_LIMIT) {
                    ctx->channel_error_retries += 1U;
                    if (error_message == NULL || error_message[0] == '\0') {
                        error_message = "unknown channel error";
                    }
                    printf("[session] channel read error for %s (attempt "
                           "%u/%u): %s\n",
                           ctx->user.name, ctx->channel_error_retries,
                           SSH_CHATTER_CHANNEL_RECOVERY_LIMIT, error_message);
                    struct timespec retry_delay = {
                        .tv_sec = 0,
                        .tv_nsec = SSH_CHATTER_CHANNEL_RECOVERY_DELAY_NS,
                    };
                    host_sleep_uninterruptible(&retry_delay);
                    continue;
                }

                if (ctx->has_joined_room) {
                    if (error_message == NULL || error_message[0] == '\0') {
                        error_message = "unknown channel error";
                    }
                    printf("[session] channel read failure for %s after %u "
                           "retries: %s\n",
                           ctx->user.name, ctx->channel_error_retries,
                           error_message);
                }
                break;
            }
            continue;
        }

        if (read_result == 0) {
            if (!session_transport_is_open(ctx) ||
                session_transport_is_eof(ctx)) {
                break;
            }
            if (ctx->game.active && ctx->game.type == SESSION_GAME_TETRIS) {
                session_game_tetris_process_timeout(ctx);
            }
            continue;
        }

        ctx->channel_error_retries = 0U;

        if (read_result == 0) {
            break;
        }
        if (read_result < 0) {
            continue;
        }

        for (int idx = 0; idx < read_result; ++idx) {
            const char ch = buffer[idx];

            if (ctx->game.active && ctx->game.type == SESSION_GAME_TETRIS) {
                if (session_game_tetris_process_raw_input(ctx, ch)) {
                    continue;
                }
            }

            if (session_consume_escape_sequence(ctx, ch)) {
                continue;
            }

            if (ch == 0x01) {
                if (ctx->bbs_post_pending || ctx->asciiart_pending) {
                    ctx->input_buffer[ctx->input_length] = '\0';
                    session_apply_background_fill(ctx);
                    if (ctx->bbs_post_pending) {
                        if (ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART) {
                            const char *cancel_message =
                                (ctx->asciiart_target ==
                                 SESSION_ASCIIART_TARGET_PROFILE_PICTURE)
                                    ? "Profile picture draft canceled."
                                    : "ASCII art draft canceled.";
                            session_asciiart_cancel(ctx, cancel_message);
                        } else {
                            const char *cancel_notice =
                                (ctx->editor_mode ==
                                 SESSION_EDITOR_MODE_BBS_EDIT)
                                    ? "BBS edit canceled."
                                    : "BBS draft canceled.";
                            session_bbs_reset_pending_post(ctx);
                            session_send_system_line(ctx, cancel_notice);
                        }
                    } else {
                        const char *cancel_message =
                            (ctx->asciiart_target ==
                             SESSION_ASCIIART_TARGET_PROFILE_PICTURE)
                                ? "Profile picture draft canceled."
                                : "ASCII art draft canceled.";
                        session_asciiart_cancel(ctx, cancel_message);
                    }
                    session_clear_input_without_prompt(ctx);
                    if (ctx->should_exit) {
                        break;
                    }
                    session_render_prompt(ctx, false);
                }
                continue;
            }

            if (ctx->transport_kind == SESSION_TRANSPORT_TELNET && ch == 0x00) {
                // Ignore stray NUL padding bytes from telnet clients so they do
                // not trigger unintended disconnects.
                continue;
            }

            if (ch == 0x03) {
                ctx->input_buffer[ctx->input_length] = '\0';
                session_apply_background_fill(ctx);
                session_handle_exit(ctx);
                session_clear_input_without_prompt(ctx);
                if (ctx->should_exit) {
                    break;
                }
                session_render_prompt(ctx, false);
                continue;
            }

            if (ch == 0x1a) {
                ctx->input_buffer[ctx->input_length] = '\0';
                session_apply_background_fill(ctx);
                if (ctx->in_rss_mode) {
                    session_rss_exit(ctx, NULL);
                    session_clear_input_without_prompt(ctx);
                    if (ctx->should_exit) {
                        break;
                    }
                    session_render_prompt(ctx, false);
                } else if (ctx->game.active) {
                    bool handled = false;
                    if (ctx->game.type == SESSION_GAME_OTHELLO &&
                        ctx->game.othello.multiplayer) {
                        handled = session_game_othello_handle_forced_exit(ctx);
                    }
                    if (!handled) {
                        session_game_suspend(ctx, "Game suspended.");
                    }
                    session_clear_input_without_prompt(ctx);
                    if (ctx->should_exit) {
                        break;
                    }
                    session_render_prompt(ctx, false);
                } else {
                    session_handle_exit(ctx);
                    session_clear_input_without_prompt(ctx);
                    if (ctx->should_exit) {
                        break;
                    }
                    session_render_prompt(ctx, false);
                }
                continue;
            }

            if (ch == 0x13) {
                if (ctx->bbs_post_pending || ctx->asciiart_pending) {
                    ctx->input_buffer[ctx->input_length] = '\0';
                    bool had_body = ctx->input_length > 0U;
                    if (had_body) {
                        session_local_echo_char(ctx, '\n');
                        session_history_record(ctx, ctx->input_buffer);
                        if (ctx->bbs_post_pending) {
                            session_bbs_capture_body_text(ctx,
                                                          ctx->input_buffer);
                        } else {
                            session_asciiart_capture_text(ctx,
                                                          ctx->input_buffer);
                        }
                    } else {
                        session_local_echo_char(ctx, '\n');
                    }

                    const char *terminator =
                        ctx->bbs_post_pending
                            ? session_editor_terminator(ctx)
                            : session_asciiart_terminator(ctx);
                    for (const char *cursor = terminator; *cursor != '\0';
                         ++cursor) {
                        session_local_echo_char(ctx, *cursor);
                    }
                    session_local_echo_char(ctx, '\n');
                    session_history_record(ctx, terminator);
                    if (ctx->bbs_post_pending) {
                        session_bbs_capture_body_line(ctx, terminator);
                    } else {
                        session_asciiart_capture_line(ctx, terminator);
                    }
                    session_clear_input_without_prompt(ctx);
                    if (ctx->should_exit) {
                        break;
                    }
                    session_render_prompt(ctx, false);
                } else if (ctx->game.active &&
                           ctx->game.type == SESSION_GAME_ALPHA) {
                    session_game_alpha_manual_save(ctx);
                }
                continue;
            }

            if (ch == '\r' || ch == '\n') {
                session_apply_background_fill(ctx);
                const bool composing_draft =
                    ctx->bbs_post_pending || ctx->asciiart_pending;
                if (ctx->input_length > 0U) {
                    ctx->input_buffer[ctx->input_length] = '\0';
                    session_history_record(ctx, ctx->input_buffer);
                    session_process_line(ctx, ctx->input_buffer);
                } else if (composing_draft) {
                    session_process_line(ctx, "");
                }
                session_clear_input_without_prompt(ctx);
                if (ctx->should_exit) {
                    break;
                }
                if (!ctx->bracket_paste_active) {
                    session_render_prompt(ctx, false);
                }
                continue;
            }

            if (ch == '\b' || ch == 0x7f) {
                ctx->input_history_position = -1;
                session_scrollback_reset_position(ctx);
                session_local_backspace(ctx);
                continue;
            }

            if (ch == '\t') {
                if (session_try_command_completion(ctx)) {
                    continue;
                }
                if (ctx->input_length + 1U < sizeof(ctx->input_buffer)) {
                    ctx->input_history_position = -1;
                    session_scrollback_reset_position(ctx);
                    ctx->input_buffer[ctx->input_length++] = ' ';
                    session_local_echo_char(ctx, ' ');
                }
                continue;
            }

            if ((unsigned char)ch < 0x20U) {
                continue;
            }

            char encoded[4];
            size_t encoded_len = 1U;
            if (ctx->cp437_input_enabled) {
                encoded_len = session_cp437_byte_to_utf8(
                    (unsigned char)ch, encoded, sizeof(encoded));
                if (encoded_len == 0U) {
                    encoded[0] = '?';
                    encoded_len = 1U;
                }
            } else {
                encoded[0] = ch;
            }

            if (ctx->input_length + encoded_len >= sizeof(ctx->input_buffer)) {
                ctx->input_buffer[sizeof(ctx->input_buffer) - 1U] = '\0';
                session_history_record(ctx, ctx->input_buffer);
                session_process_line(ctx, ctx->input_buffer);
                session_clear_input_without_prompt(ctx);
                if (ctx->should_exit) {
                    break;
                }
                session_render_prompt(ctx, false);
            }

            if (ctx->input_length + encoded_len < sizeof(ctx->input_buffer)) {
                ctx->input_history_position = -1;
                session_scrollback_reset_position(ctx);
                memcpy(&ctx->input_buffer[ctx->input_length], encoded,
                       encoded_len);
                ctx->input_length += encoded_len;
                ctx->input_buffer[ctx->input_length] = '\0';
                for (size_t echo_idx = 0U; echo_idx < encoded_len; ++echo_idx) {
                    session_local_echo_char(ctx, encoded[echo_idx]);
                }
            }
        }

        if (ctx->should_exit) {
            break;
        }
    }

    session_translation_flush_ready(ctx);

    if (!ctx->should_exit && ctx->input_length > 0U) {
        ctx->input_buffer[ctx->input_length] = '\0';
        session_history_record(ctx, ctx->input_buffer);
        session_process_line(ctx, ctx->input_buffer);
        session_clear_input(ctx);
    }

    if (ctx->has_joined_room) {
        printf("[part] %s\n", ctx->user.name);
        char part_message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(part_message, sizeof(part_message), "* [%s] has left the chat",
                 ctx->user.name);
        host_history_record_system(ctx->owner, part_message, NULL);
        chat_room_broadcast(&ctx->owner->room, part_message, NULL);
        chat_room_remove(&ctx->owner->room, ctx);
    }

    session_destroy(ctx);

    SESSION_THREAD_RETURN(NULL);

#undef SESSION_THREAD_RETURN
}

void host_init(host_t *host, auth_profile_t *auth)
{
    if (host == NULL) {
        return;
    }

    sshc_memory_context_t *memory_scope =
        sshc_memory_context_push(host->memory_context);

    translator_global_init();

    memset(&host->eliza_worker, 0, sizeof(host->eliza_worker));
    if (!host_eliza_worker_init(host)) {
        printf("[eliza] asynchronous intervention worker unavailable; "
               "interventions disabled.\n");
    }

    if (!host_moderation_init(host)) {
        printf("[security] moderation worker unavailable; using synchronous "
               "checks\n");
    }

    chat_room_init(&host->room);
    host->listener.handle = NULL;
    host->listener.inplace_recoveries = 0U;
    host->listener.restart_attempts = 0U;
    host->listener.last_error_time.tv_sec = 0;
    host->listener.last_error_time.tv_nsec = 0L;
    host->telnet.enabled = false;
    host->telnet.fd = -1;
    host->telnet.thread_initialized = false;
    atomic_store(&host->telnet.running, false);
    atomic_store(&host->telnet.stop, false);
    host->telnet.restart_attempts = 0U;
    host->telnet.last_error_time.tv_sec = 0;
    host->telnet.last_error_time.tv_nsec = 0L;
    host->telnet.bind_address[0] = '\0';
    host->telnet.port[0] = '\0';
    host->auth = auth;
    host->clients = NULL;
    host->web_client = NULL;
    host->matrix_client = NULL;
    host->security_layer_initialized =
        security_layer_init(&host->security_layer);
    if (!host->security_layer_initialized) {
        humanized_log_error("security",
                            "failed to initialise layered message encryption",
                            errno != 0 ? errno : EIO);
    }
    host_load_lan_operator_credentials(host);
    const palette_descriptor_t *default_palette =
        palette_find_descriptor("clean");
    if (default_palette != NULL) {
        host_apply_palette_descriptor(host, default_palette);
    } else {
        static char userColor[32];
        static char highlight[32];
        static char bgColor[32];
        static char fgColor[32];
        static char hlColor[32];

        ansi_256(userColor, sizeof(userColor), MONOKAI_BLUE_USER_COLOR);
        ansi_bg_256(highlight, sizeof(highlight), 0);
        ansi_bg_256(bgColor, sizeof(bgColor), MONOKAI_BLUE_BACKGROUND_COLOR);
        ansi_256(fgColor, sizeof(fgColor), MONOKAI_BLUE_FOREGROUND_COLOR);
        ansi_bg_256(hlColor, sizeof(hlColor), MONOKAI_BLUE_HIGHLIGHT_COLOR);

        host->user_theme.userColor = userColor;
        host->user_theme.highlight = highlight;
        host->user_theme.isBold = false;
        host->system_theme.backgroundColor = bgColor;
        host->system_theme.foregroundColor = fgColor;
        host->system_theme.highlightColor = hlColor;
        host->system_theme.isBold = true;
        snprintf(host->default_user_color_name,
                 sizeof(host->default_user_color_name), "%s", "green");
        snprintf(host->default_user_highlight_name,
                 sizeof(host->default_user_highlight_name), "%s", "default");
        snprintf(host->default_system_fg_name,
                 sizeof(host->default_system_fg_name), "%s", "white");
        snprintf(host->default_system_bg_name,
                 sizeof(host->default_system_bg_name), "%s", "blue");
        snprintf(host->default_system_highlight_name,
                 sizeof(host->default_system_highlight_name), "%s", "yellow");
    }
    host->ban_count = 0U;
    memset(host->bans, 0, sizeof(host->bans));
    memset(host->replies, 0, sizeof(host->replies));
    host->reply_count = 0U;
    host->next_reply_id = 1U;
    memset(host->eliza_memory, 0, sizeof(host->eliza_memory));
    host->eliza_memory_count = 0U;
    host->eliza_memory_next_id = 1U;
    snprintf(host->version, sizeof(host->version),
             "ssh-chatter (C, rolling release)");
    snprintf(host->motd_base, sizeof(host->motd_base),
             "Welcome to ssh-chat!\n"
             "- Be polite to each other\n"
             "- fun fact: this server is written in pure c.\n"
             "============================================\n"
             " _      ____  ____  _____ ____  _        ____  _ \n"
             "/ \\__/|/  _ \\/  _ \\/  __//  __\\/ \\  /|  /   _\\/ \\\n"
             "| |\\/||| / \\|| | \\||  \\  |  \\/|| |\\ ||  |  /  | |\n"
             "| |  ||| \\_/|| |_/||  /_ |    /| | \\||  |  \\__\\_/\n"
             "\\_/  \\|\\____/\\____/\\____\\\\_/\\_\\\\_/  \\|  \\____/(_)\n"
             "                                                 \n"
             "============================================\n");
    snprintf(host->motd, sizeof(host->motd), "%s", host->motd_base);
    host->motd_path[0] = '\0';
    host->motd_has_file = false;
    host->motd_last_modified.tv_sec = 0;
    host->motd_last_modified.tv_nsec = 0L;
    host->welcome_banner[0] = '\0';
    host->welcome_banner_loaded = false;

    host->translation_quota_exhausted = false;
    host->connection_count = 0U;
    host->history = NULL;
    host->history_count = 0U;
    host->history_capacity = 0U;
    host->next_message_id = 1U;
    memset(host->preferences, 0, sizeof(host->preferences));
    host->preference_count = 0U;
    host->state_file_path[0] = '\0';
    host_state_resolve_path(host);
    host->bbs_state_file_path[0] = '\0';
    host_bbs_resolve_path(host);
    host->vote_state_file_path[0] = '\0';
    host_vote_resolve_path(host);
    host->ban_state_file_path[0] = '\0';
    host_ban_resolve_path(host);
    host->reply_state_file_path[0] = '\0';
    host_reply_state_resolve_path(host);
    host->pw_auth_file_path[0] = '\0';
    host_pw_auth_resolve_path(host);
    host->alpha_landers_file_path[0] = '\0';
    host_alpha_landers_resolve_path(host);
    snprintf(host->user_data_root, sizeof(host->user_data_root), "%s",
             "/var/lib/mailbox");
    host->user_data_ready = user_data_ensure_root(host->user_data_root);
    if (pthread_mutex_init(&host->user_data_lock, NULL) == 0) {
        host->user_data_lock_initialized = true;
    } else {
        humanized_log_error("mailbox", "failed to initialise mailbox lock",
                            errno != 0 ? errno : ENOMEM);
        host->user_data_lock_initialized = false;
        host->user_data_ready = false;
    }
    if (pthread_mutex_init(&host->alpha_landers_lock, NULL) == 0) {
        host->alpha_landers_lock_initialized = true;
    } else {
        humanized_log_error("alpha", "failed to initialise alpha landers lock",
                            errno != 0 ? errno : ENOMEM);
        host->alpha_landers_lock_initialized = false;
    }
    host->rss_state_file_path[0] = '\0';
    host_rss_resolve_path(host);
    host->eliza_memory_file_path[0] = '\0';
    host_eliza_memory_resolve_path(host);
    host->eliza_state_file_path[0] = '\0';
    host_eliza_state_resolve_path(host);
    host->security_clamav_thread_initialized = false;
    atomic_store(&host->security_clamav_thread_running, false);
    atomic_store(&host->security_clamav_thread_stop, false);
    host->security_clamav_last_run.tv_sec = 0;
    host->security_clamav_last_run.tv_nsec = 0;
    host->bbs_watchdog_thread_initialized = false;
    atomic_store(&host->bbs_watchdog_thread_running, false);
    atomic_store(&host->bbs_watchdog_thread_stop, false);
    host->bbs_watchdog_last_run.tv_sec = 0;
    host->bbs_watchdog_last_run.tv_nsec = 0;
    host->rss_thread_initialized = false;
    atomic_store(&host->rss_thread_running, false);
    atomic_store(&host->rss_thread_stop, false);
    host->rss_last_run.tv_sec = 0;
    host->rss_last_run.tv_nsec = 0L;
    host_security_configure(host);
    host_version_ip_rules_init(host);
    memset(host->protected_ips, 0, sizeof(host->protected_ips));
    host->protected_ip_count = 0U;
    pthread_mutex_init(&host->lock, NULL);
    host_protected_ips_bootstrap(host);
    poll_state_reset(&host->poll);
    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_NAMED_POLLS; ++idx) {
        named_poll_reset(&host->named_polls[idx]);
    }
    host->named_poll_count = 0U;
    for (size_t idx = 0U; idx < SSH_CHATTER_BBS_MAX_POSTS; ++idx) {
        host->bbs_posts[idx].in_use = false;
        host->bbs_posts[idx].id = 0U;
        host->bbs_posts[idx].author[0] = '\0';
        host->bbs_posts[idx].title[0] = '\0';
        host->bbs_posts[idx].body[0] = '\0';
        host->bbs_posts[idx].tag_count = 0U;
        host->bbs_posts[idx].created_at = 0;
        host->bbs_posts[idx].bumped_at = 0;
        host->bbs_posts[idx].comment_count = 0U;
        for (size_t comment = 0U; comment < SSH_CHATTER_BBS_MAX_COMMENTS;
             ++comment) {
            host->bbs_posts[idx].comments[comment].author[0] = '\0';
            host->bbs_posts[idx].comments[comment].text[0] = '\0';
            host->bbs_posts[idx].comments[comment].created_at = 0;
        }
    }
    host->bbs_post_count = 0U;
    host->next_bbs_id = 1U;
    for (size_t idx = 0U; idx < SSH_CHATTER_RSS_MAX_FEEDS; ++idx) {
        host_clear_rss_feed(&host->rss_feeds[idx]);
    }
    host->rss_feed_count = 0U;
    for (size_t idx = 0U; idx < SSH_CHATTER_OTHELLO_MAX_SLOTS; ++idx) {
        othello_multiplayer_slot_t *slot = &host->othello_games[idx];
        slot->in_use = false;
        slot->active = false;
        slot->awaiting_second_player = false;
        slot->slot_id = (uint16_t)(idx + 1U);
        slot->owner[0] = '\0';
        slot->players[0] = NULL;
        slot->players[1] = NULL;
        slot->state = (othello_game_state_t){0};
        slot->state.slot_index = -1;
    }
    host->random_seeded = false;
    memset(host->operator_grants, 0, sizeof(host->operator_grants));
    host->operator_grant_count = 0U;
    host->next_join_ready_time = (struct timespec){0, 0};
    host->join_throttle_initialised = false;
    host->join_progress_length = 0U;
    host->join_activity = NULL;
    host->join_activity_count = 0U;
    host->join_activity_capacity = 0U;
    host->connection_guard = NULL;
    host->connection_guard_count = 0U;
    host->connection_guard_capacity = 0U;
    host->health_guard.consecutive_errors = 0U;
    host->health_guard.last_error_time.tv_sec = 0;
    host->health_guard.last_error_time.tv_nsec = 0L;
    atomic_store(&host->captcha_enabled, false);
    host->captcha_nonce = 0U;
    host->has_last_captcha = false;
    host->last_captcha_question[0] = '\0';
    host->last_captcha_answer[0] = '\0';
    host->last_captcha_generated.tv_sec = 0;
    host->last_captcha_generated.tv_nsec = 0L;
    atomic_store(&host->eliza_enabled, false);
    atomic_store(&host->eliza_announced, false);
    host->eliza_last_action.tv_sec = 0;
    host->eliza_last_action.tv_nsec = 0L;

    (void)host_try_load_motd_from_path(host, "/etc/ssh-chatter/motd");

    host_state_load(host);
    host_vote_state_load(host);
    host_bbs_state_load(host);
    host_ban_state_load(host);
    host_reply_state_load(host);
    host_rss_state_load(host);
    host_eliza_memory_load(host);
    host_eliza_state_load(host);

    host_user_data_bootstrap(host);

    host_refresh_motd(host);

    host->clients = client_manager_create(host);
    if (host->clients == NULL) {
        humanized_log_error("host", "failed to create client manager", ENOMEM);
    } else {
        host->web_client = webssh_client_create(host, host->clients);
        if (host->web_client == NULL) {
            humanized_log_error("host", "failed to initialise webssh client",
                                ENOMEM);
        }

        if (host->security_layer_initialized) {
            host->matrix_client = matrix_client_create(host, host->clients,
                                                       &host->security_layer);
            if (host->matrix_client == NULL) {
                humanized_log_error("matrix",
                                    "matrix backend inactive; check "
                                    "CHATTER_MATRIX_* configuration",
                                    EINVAL);
            }
            
            host->mrc_client = mrc_client_create(host);
            if (host->mrc_client == NULL) {
                humanized_log_error("mrc",
                                    "MRC relay inactive; check "
                                    "CHATTER_MRC_* configuration",
                                    EINVAL);
            }
        }
    }
    host_security_start_clamav_backend(host);
    host_bbs_start_watchdog(host);
    host_rss_start_backend(host);
    sshc_memory_context_pop(memory_scope);
}

static void host_build_birthday_notice_locked(host_t *host, char *line,
                                              size_t length)
{
    if (line == NULL || length == 0U) {
        return;
    }

    line[0] = '\0';

    if (host == NULL) {
        return;
    }

    time_t now = time(NULL);
    if (now == (time_t)-1) {
        return;
    }

    struct tm local_now;
    if (localtime_r(&now, &local_now) == NULL) {
        return;
    }

    struct tm today_tm = local_now;
    today_tm.tm_hour = 0;
    today_tm.tm_min = 0;
    today_tm.tm_sec = 0;
    today_tm.tm_isdst = -1;
    time_t today = mktime(&today_tm);
    if (today == (time_t)-1) {
        today = now;
    }

    char names[SSH_CHATTER_MESSAGE_LIMIT];
    names[0] = '\0';
    size_t name_count = 0U;

    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_PREFERENCES; ++idx) {
        const user_preference_t *pref = &host->preferences[idx];
        if (!pref->in_use || !pref->has_birthday) {
            continue;
        }
        if (pref->username[0] == '\0' || pref->birthday[0] == '\0') {
            continue;
        }

        int month = 0;
        int day = 0;
        if (sscanf(pref->birthday, "%*d-%d-%d", &month, &day) != 2) {
            continue;
        }
        if (month < 1 || month > 12 || day < 1 || day > 31) {
            continue;
        }

        int use_day = day;
        int use_month = month;
        const int current_year = local_now.tm_year + 1900;
        if (use_month == 2 && use_day == 29 &&
            !host_is_leap_year(current_year)) {
            use_day = 28;
        }

        struct tm birthday_tm = today_tm;
        birthday_tm.tm_year = local_now.tm_year;
        birthday_tm.tm_mon = use_month - 1;
        birthday_tm.tm_mday = use_day;
        birthday_tm.tm_hour = 0;
        birthday_tm.tm_min = 0;
        birthday_tm.tm_sec = 0;
        birthday_tm.tm_isdst = -1;
        time_t birthday_time = mktime(&birthday_tm);
        if (birthday_time == (time_t)-1) {
            continue;
        }

        time_t diff = today - birthday_time;
        if (diff < 0) {
            birthday_tm.tm_year -= 1;
            birthday_tm.tm_isdst = -1;
            birthday_time = mktime(&birthday_tm);
            if (birthday_time == (time_t)-1) {
                continue;
            }
            diff = today - birthday_time;
        }

        if (diff < 0 || diff >= (time_t)SSH_CHATTER_BIRTHDAY_WINDOW_SECONDS) {
            continue;
        }

        size_t current_len = strnlen(names, sizeof(names));
        const size_t name_len = strnlen(pref->username, sizeof(pref->username));
        if (name_len == 0U) {
            continue;
        }

        if (current_len > 0U) {
            if (current_len + 2U >= sizeof(names)) {
                continue;
            }
            names[current_len++] = ',';
            names[current_len++] = ' ';
            names[current_len] = '\0';
        }

        if (name_len >= sizeof(names) - current_len) {
            continue;
        }

        memcpy(names + current_len, pref->username, name_len);
        current_len += name_len;
        names[current_len] = '\0';
        ++name_count;
    }

    if (name_count == 0U) {
        return;
    }

    snprintf(line, length, "Happy birthday to %s!\n", names);
}

static void host_refresh_motd_locked(host_t *host)
{
    if (host == NULL) {
        return;
    }

    char birthday_line[SSH_CHATTER_MESSAGE_LIMIT];
    host_build_birthday_notice_locked(host, birthday_line,
                                      sizeof(birthday_line));

    if (birthday_line[0] != '\0') {
        snprintf(host->motd, sizeof(host->motd), "%s%s", birthday_line,
                 host->motd_base);
    } else {
        snprintf(host->motd, sizeof(host->motd), "%s", host->motd_base);
    }
}

static void host_refresh_motd(host_t *host)
{
    if (host == NULL) {
        return;
    }

    host_maybe_reload_motd_from_file(host);

    pthread_mutex_lock(&host->lock);
    host_refresh_motd_locked(host);
    pthread_mutex_unlock(&host->lock);
}

static bool host_try_load_motd_from_path(host_t *host, const char *path)
{
    if (host == NULL || path == NULL || path[0] == '\0') {
        return false;
    }

    FILE *motd_file = fopen(path, "r");
    if (motd_file == NULL) {
        return false;
    }

    struct stat info;
    bool have_info = false;
    struct timespec modified = {0, 0};
    if (fstat(fileno(motd_file), &info) == 0) {
        modified = host_stat_mtime(&info);
        if (modified.tv_sec != 0 || modified.tv_nsec != 0) {
            have_info = true;
        }
    }

    char motd_buffer[sizeof(host->motd)];
    motd_buffer[0] = '\0';
    size_t current_motd_len = 0U;
    char line_buffer[SSH_CHATTER_MESSAGE_LIMIT];

    while (fgets(line_buffer, sizeof(line_buffer), motd_file) != NULL) {
        size_t line_len = strlen(line_buffer);
        while (line_len > 0U && (line_buffer[line_len - 1U] == '\n' ||
                                 line_buffer[line_len - 1U] == '\r')) {
            line_buffer[--line_len] = '\0';
        }

        // Prepend [200~ and append [201~ for bracketed paste mode
        const char *start_paste = "\033[200~";
        const char *end_paste = "\033[201~";
        size_t start_paste_len = strlen(start_paste);
        size_t end_paste_len = strlen(end_paste);

        size_t required_len =
            start_paste_len + line_len + end_paste_len + 1U; // +1 for newline

        if (current_motd_len + required_len + 1U >=
            sizeof(motd_buffer)) { // +1 for null terminator
            // MOTD buffer is full, stop reading
            break;
        }

        // Append start_paste
        memcpy(motd_buffer + current_motd_len, start_paste, start_paste_len);
        current_motd_len += start_paste_len;

        // Append line content
        memcpy(motd_buffer + current_motd_len, line_buffer, line_len);
        current_motd_len += line_len;

        // Append end_paste
        memcpy(motd_buffer + current_motd_len, end_paste, end_paste_len);
        current_motd_len += end_paste_len;

        // Append newline
        motd_buffer[current_motd_len++] = '\n';
        motd_buffer[current_motd_len] = '\0';
    }

    if (fclose(motd_file) != 0) {
        const int close_error = errno;
        humanized_log_error("host", "failed to close motd file", close_error);
    }

    pthread_mutex_lock(&host->lock);
    snprintf(host->motd_base, sizeof(host->motd_base), "%s", motd_buffer);
    snprintf(host->motd_path, sizeof(host->motd_path), "%s", path);
    host->motd_has_file = true;
    if (have_info) {
        host->motd_last_modified = modified;
    } else {
        host->motd_last_modified.tv_sec = 0;
        host->motd_last_modified.tv_nsec = 0L;
    }
    host_refresh_motd_locked(host);
    pthread_mutex_unlock(&host->lock);
    return true;
}

void host_set_motd(host_t *host, const char *motd)
{
    sshc_memory_context_t *memory_scope = NULL;
    if (host != NULL) {
        memory_scope = sshc_memory_context_push(host->memory_context);
    }
    if (host == NULL || motd == NULL) {
        goto exit_host_set_motd;
    }

    char motd_path[PATH_MAX];
    motd_path[0] = '\0';
    snprintf(motd_path, sizeof(motd_path), "%s", motd);
    trim_whitespace_inplace(motd_path);

    const size_t max_paths = 2U;
    const char *paths_to_try[2] = {NULL, NULL};
    size_t path_count = 0U;

    char expanded_path[PATH_MAX];
    expanded_path[0] = '\0';
    if (motd_path[0] == '~') {
        const char *home = getenv("HOME");
        if (home != NULL && home[0] != '\0' &&
            (motd_path[1] == '\0' || motd_path[1] == '/')) {
            const int written = snprintf(expanded_path, sizeof(expanded_path),
                                         "%s%s", home, motd_path + 1);
            if (written > 0 && (size_t)written < sizeof(expanded_path) &&
                path_count < max_paths) {
                paths_to_try[path_count++] = expanded_path;
            }
        }
    }

    if (motd_path[0] != '\0') {
        if (path_count < max_paths) {
            paths_to_try[path_count++] = motd_path;
        }
    }

    for (size_t idx = 0U; idx < path_count; ++idx) {
        if (paths_to_try[idx] != NULL &&
            host_try_load_motd_from_path(host, paths_to_try[idx])) {
            goto exit_host_set_motd;
        }
    }

    char normalized[sizeof(host->motd)];
    snprintf(normalized, sizeof(normalized), "%s", motd);
    session_normalize_newlines(normalized);

    pthread_mutex_lock(&host->lock);
    if (motd_path[0] != '\0') {
        snprintf(host->motd_path, sizeof(host->motd_path), "%s", motd_path);
    } else {
        host->motd_path[0] = '\0';
    }
    host->motd_has_file = false;
    host->motd_last_modified.tv_sec = 0;
    host->motd_last_modified.tv_nsec = 0L;
    snprintf(host->motd_base, sizeof(host->motd_base), "%s", normalized);
    host_refresh_motd_locked(host);
    pthread_mutex_unlock(&host->lock);

exit_host_set_motd:
    if (memory_scope != NULL) {
        sshc_memory_context_pop(memory_scope);
    }
}

bool host_post_client_message(host_t *host, const char *username,
                              const char *message, const char *color_name,
                              const char *highlight_name, bool is_bold)
{
    bool success = false;
    sshc_memory_context_t *memory_scope = NULL;
    if (host != NULL) {
        memory_scope = sshc_memory_context_push(host->memory_context);
    }
    if (host == NULL || username == NULL || username[0] == '\0' ||
        message == NULL) {
        goto exit_host_post_client_message;
    }

    chat_history_entry_t entry = {0};
    entry.is_user_message = true;
    snprintf(entry.username, sizeof(entry.username), "%s", username);
    snprintf(entry.message, sizeof(entry.message), "%s", message);
    entry.attachment_type = CHAT_ATTACHMENT_NONE;
    entry.user_is_bold = is_bold;
    time_t now = time(NULL);
    if (now != (time_t)-1) {
        entry.created_at = now;
    }

    const char *color_label = (color_name != NULL && color_name[0] != '\0')
                                  ? color_name
                                  : host->default_user_color_name;
    snprintf(entry.user_color_name, sizeof(entry.user_color_name), "%s",
             color_label);
    const char *highlight_label =
        (highlight_name != NULL && highlight_name[0] != '\0')
            ? highlight_name
            : host->default_user_highlight_name;
    snprintf(entry.user_highlight_name, sizeof(entry.user_highlight_name), "%s",
             highlight_label);

    const char *color_code = lookup_color_code(
        USER_COLOR_MAP, sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
        color_label);
    const char *highlight_code = lookup_color_code(
        HIGHLIGHT_COLOR_MAP,
        sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
        highlight_label);
    entry.user_color_code =
        color_code != NULL ? color_code : host->user_theme.userColor;
    entry.user_highlight_code =
        highlight_code != NULL ? highlight_code : host->user_theme.highlight;

    chat_history_entry_t stored = {0};
    if (!host_history_commit_entry(host, &entry, &stored)) {
        goto exit_host_post_client_message;
    }

    chat_room_broadcast_entry(&host->room, &stored, NULL);
    host_notify_external_clients(host, &stored);
    success = true;

exit_host_post_client_message:
    if (memory_scope != NULL) {
        sshc_memory_context_pop(memory_scope);
    }
    return success;
}

bool host_snapshot_last_captcha(host_t *host, char *question,
                                size_t question_length, char *answer,
                                size_t answer_length,
                                struct timespec *timestamp)
{
    if (host == NULL) {
        return false;
    }

    pthread_mutex_lock(&host->lock);
    bool has_captcha = host->has_last_captcha;
    if (has_captcha) {
        if (question != NULL && question_length > 0U) {
            snprintf(question, question_length, "%s",
                     host->last_captcha_question);
        }
        if (answer != NULL && answer_length > 0U) {
            snprintf(answer, answer_length, "%s", host->last_captcha_answer);
        }
        if (timestamp != NULL) {
            *timestamp = host->last_captcha_generated;
        }
    } else {
        if (question != NULL && question_length > 0U) {
            question[0] = '\0';
        }
        if (answer != NULL && answer_length > 0U) {
            answer[0] = '\0';
        }
        if (timestamp != NULL) {
            timestamp->tv_sec = 0;
            timestamp->tv_nsec = 0L;
        }
    }
    pthread_mutex_unlock(&host->lock);
    return has_captcha;
}

static void host_sleep_after_error(host_t *host)
{
    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);

    unsigned int streak = 1U;
    if (host != NULL) {
        if (host->health_guard.last_error_time.tv_sec != 0 ||
            host->health_guard.last_error_time.tv_nsec != 0) {
            struct timespec diff =
                timespec_diff(&now, &host->health_guard.last_error_time);
            long long diff_ns = timespec_to_ns(&diff);
            if (diff_ns <= SSH_CHATTER_ERROR_BACKOFF_STABLE_NS &&
                host->health_guard.consecutive_errors < UINT_MAX) {
                streak = host->health_guard.consecutive_errors + 1U;
            }
        }

        host->health_guard.consecutive_errors = streak;
        host->health_guard.last_error_time = now;
    }

    long long multiplier = (long long)streak;
    if (multiplier <= 0) {
        multiplier = 1LL;
    }

    const long long max_multiplier =
        SSH_CHATTER_ERROR_BACKOFF_MAX_NS / SSH_CHATTER_ERROR_BACKOFF_BASE_NS;
    if (multiplier > max_multiplier) {
        multiplier = max_multiplier;
    }

    long long delay_ns = SSH_CHATTER_ERROR_BACKOFF_BASE_NS * multiplier;
    if (delay_ns > SSH_CHATTER_ERROR_BACKOFF_MAX_NS) {
        delay_ns = SSH_CHATTER_ERROR_BACKOFF_MAX_NS;
    }
    if (delay_ns < SSH_CHATTER_ERROR_BACKOFF_BASE_NS) {
        delay_ns = SSH_CHATTER_ERROR_BACKOFF_BASE_NS;
    }

    struct timespec delay = {
        .tv_sec = (time_t)(delay_ns / 1000000000LL),
        .tv_nsec = (long)(delay_ns % 1000000000LL),
    };
    host_sleep_uninterruptible(&delay);
}

static void host_shutdown_internal(host_t *host, bool send_sigterm)
{
    if (host == NULL) {
        return;
    }

    if (send_sigterm) {
        // Terminate all child processes in the same process group
        kill(0, SIGTERM);
    }

    sshc_memory_context_t *memory_scope = NULL;
    if (host->memory_context != NULL) {
        memory_scope = sshc_memory_context_push(host->memory_context);
    }

    host_eliza_worker_shutdown(host);
    host_moderation_shutdown(host);

    host_telnet_listener_stop(host);

    if (host->rss_thread_initialized) {
        atomic_store(&host->rss_thread_stop, true);
        pthread_join(host->rss_thread, NULL);
        host->rss_thread_initialized = false;
        atomic_store(&host->rss_thread_running, false);
    }

    if (host->security_clamav_thread_initialized) {
        atomic_store(&host->security_clamav_thread_stop, true);
        pthread_join(host->security_clamav_thread, NULL);
        host->security_clamav_thread_initialized = false;
        atomic_store(&host->security_clamav_thread_running, false);
    }

    if (host->bbs_watchdog_thread_initialized) {
        atomic_store(&host->bbs_watchdog_thread_stop, true);
        pthread_join(host->bbs_watchdog_thread, NULL);
        host->bbs_watchdog_thread_initialized = false;
        atomic_store(&host->bbs_watchdog_thread_running, false);
    }

    if (host->matrix_client != NULL) {
        matrix_client_destroy(host->matrix_client);
        host->matrix_client = NULL;
    }
    if (host->mrc_client != NULL) {
        mrc_client_destroy(host->mrc_client);
        host->mrc_client = NULL;
    }
    if (host->web_client != NULL) {
        webssh_client_destroy(host->web_client);
        host->web_client = NULL;
    }
    if (host->clients != NULL) {
        client_manager_destroy(host->clients);
        host->clients = NULL;
    }
    chat_history_entry_t *history_buffer = NULL;
    join_activity_entry_t *join_buffer = NULL;
    pthread_mutex_lock(&host->lock);
    history_buffer = host->history;
    host->history = NULL;
    host->history_capacity = 0U;
    host->history_count = 0U;
    join_buffer = host->join_activity;
    host->join_activity = NULL;
    host->join_activity_capacity = 0U;
    host->join_activity_count = 0U;
    pthread_mutex_unlock(&host->lock);
    if (history_buffer != NULL) {
        free(history_buffer);
    }
    if (join_buffer != NULL) {
        free(join_buffer);
    }
    free(host->connection_guard);
    host->connection_guard = NULL;
    host->connection_guard_capacity = 0U;
    host->connection_guard_count = 0U;
    host->health_guard.consecutive_errors = 0U;
    host->health_guard.last_error_time.tv_sec = 0;
    host->health_guard.last_error_time.tv_nsec = 0L;
    session_ctx_t **room_members = NULL;
    pthread_mutex_lock(&host->room.lock);
    room_members = host->room.members;
    host->room.members = NULL;
    host->room.member_capacity = 0U;
    host->room.member_count = 0U;
    pthread_mutex_unlock(&host->room.lock);
    if (room_members != NULL) {
        free(room_members);
    }
    if (host->user_data_lock_initialized) {
        pthread_mutex_destroy(&host->user_data_lock);
        host->user_data_lock_initialized = false;
    }
    if (host->alpha_landers_lock_initialized) {
        pthread_mutex_destroy(&host->alpha_landers_lock);
        host->alpha_landers_lock_initialized = false;
    }
    if (host->security_layer_initialized) {
        security_layer_free(&host->security_layer);
        host->security_layer_initialized = false;
    }

    if (memory_scope != NULL) {
        sshc_memory_context_pop(memory_scope);
    }
}

void host_shutdown(host_t *host)
{
    host_shutdown_internal(host, true);
}

void host_shutdown_for_testing(host_t *host)
{
    host_shutdown_internal(host, false);
}

int host_serve(host_t *host, const char *bind_addr, const char *port,
               const char *key_directory, const char *telnet_bind_addr,
               const char *telnet_port)
{
    if (host == NULL) {
        return -1;
    }

    const char *address =
        (bind_addr != NULL && bind_addr[0] != '\0') ? bind_addr : "0.0.0.0";
    const char *bind_port = (port != NULL && port[0] != '\0') ? port : "2222";
    const char *telnet_bind = NULL;
    if (telnet_bind_addr != NULL) {
        telnet_bind = telnet_bind_addr;
    } else if (bind_addr != NULL && bind_addr[0] != '\0') {
        telnet_bind = bind_addr;
    } else {
        telnet_bind = address;
    }
    if (telnet_port != NULL && telnet_port[0] != '\0') {
        if (!host_telnet_listener_start(host, telnet_bind, telnet_port)) {
            const char *display_addr =
                (telnet_bind != NULL && telnet_bind[0] != '\0') ? telnet_bind
                                                                : "*";
            printf("[telnet] telnet listener unavailable on %s:%s\n",
                   display_addr, telnet_port);
        }
    } else {
        host_telnet_listener_stop(host);
    }
    host_register_protected_bind_address(host, address);
    host_register_protected_bind_address(host, telnet_bind);
    const bool key_dir_specified =
        key_directory != NULL && key_directory[0] != '\0';
    const host_key_definition_t host_key_definitions[] = {
        {"ssh-ed25519", "ssh_host_ed25519_key", SSH_BIND_OPTIONS_IMPORT_KEY,
         true},
        {"ecdsa-sha2-nistp256", "ssh_host_ecdsa_key", SSH_BIND_OPTIONS_ECDSAKEY,
         false},
        {"ssh-rsa", "ssh_host_rsa_key", SSH_BIND_OPTIONS_RSAKEY, false},
    };
    const size_t host_key_count =
        sizeof(host_key_definitions) / sizeof(host_key_definitions[0]);

    while (host->shutdown_flag == NULL || *host->shutdown_flag == 0) {
        ssh_bind bind_handle = ssh_bind_new();
        if (bind_handle == NULL) {
            humanized_log_error("host", "failed to allocate ssh_bind", ENOMEM);
            host_sleep_after_error(host);
            continue;
        }

        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_BINDADDR, address);
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_BINDPORT_STR,
                             bind_port);

        bool fatal_key_error = false;
        bool key_loaded = false;
        char preferred_algorithm[64];
        preferred_algorithm[0] = '\0';
        char algorithm_buffer[256];
        algorithm_buffer[0] = '\0';
        size_t algorithm_length = 0U;

        for (size_t idx = 0; idx < host_key_count; ++idx) {
            const host_key_definition_t *definition =
                &host_key_definitions[idx];
            char key_path[PATH_MAX];
            bool path_valid = false;
            char custom_candidate[PATH_MAX];
            bool attempted_custom = false;

            if (key_dir_specified) {
                attempted_custom = true;
                if (!host_join_key_path(key_directory, definition->filename,
                                        custom_candidate,
                                        sizeof(custom_candidate))) {
                    humanized_log_error("host",
                                        "host key directory path is too long",
                                        ENAMETOOLONG);
                    fatal_key_error = true;
                    break;
                }
                if (access(custom_candidate, R_OK) == 0) {
                    snprintf(key_path, sizeof(key_path), "%s",
                             custom_candidate);
                    path_valid = true;
                }
            }

            if (!path_valid) {
                if (access(definition->filename, R_OK) == 0) {
                    snprintf(key_path, sizeof(key_path), "%s",
                             definition->filename);
                    path_valid = true;
                } else {
                    char fallback_path[PATH_MAX];
                    const int written =
                        snprintf(fallback_path, sizeof(fallback_path),
                                 "/etc/ssh/%s", definition->filename);
                    if (written >= 0 &&
                        (size_t)written < sizeof(fallback_path) &&
                        access(fallback_path, R_OK) == 0) {
                        snprintf(key_path, sizeof(key_path), "%s",
                                 fallback_path);
                        path_valid = true;
                    }
                }
            }

            if (!path_valid) {
                if (attempted_custom) {
                    printf(
                        "[listener] %s host key not found at %s (skipping)\n",
                        definition->algorithm, custom_candidate);
                }
                continue;
            }

            if (!host_bind_load_key(bind_handle, definition, key_path)) {
                continue;
            }

            if (!key_loaded) {
                snprintf(preferred_algorithm, sizeof(preferred_algorithm), "%s",
                         definition->algorithm);
                key_loaded = true;
            }

            host_bind_append_algorithm(
                algorithm_buffer, sizeof(algorithm_buffer), &algorithm_length,
                definition->algorithm);
        }

        if (fatal_key_error) {
            ssh_bind_free(bind_handle);
            host_sleep_after_error(host);
            continue;
        }

        if (algorithm_length == 0U) {
            humanized_log_error("host", "no host keys configured", 0);
            ssh_bind_free(bind_handle);
            host_sleep_after_error(host);
            continue;
        }

        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_HOSTKEY_ALGORITHMS,
                             algorithm_buffer);
        if (preferred_algorithm[0] != '\0') {
            ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_HOSTKEY,
                                 preferred_algorithm);
        }

        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_KEY_EXCHANGE,
                             SSH_CHATTER_SUPPORTED_KEX_ALGORITHMS);
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_CIPHERS_C_S,
                             SSH_CHATTER_STRONG_CIPHERS);
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_CIPHERS_S_C,
                             SSH_CHATTER_STRONG_CIPHERS);
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_HMAC_C_S,
                             SSH_CHATTER_STRONG_MACS);
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_HMAC_S_C,
                             SSH_CHATTER_STRONG_MACS);
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_COMPRESSION_C_S,
                             SSH_CHATTER_SECURE_COMPRESSION);
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_COMPRESSION_S_C,
                             SSH_CHATTER_SECURE_COMPRESSION);

        if (ssh_bind_listen(bind_handle) < 0) {
            humanized_log_error("host", ssh_get_error(bind_handle), EIO);
            ssh_bind_free(bind_handle);
            host_sleep_after_error(host);
            continue;
        }

        host->listener.handle = bind_handle;
        host->listener.last_error_time.tv_sec = 0;
        host->listener.last_error_time.tv_nsec = 0L;
        printf("[listener] listening on %s:%s\n", address, bind_port);
        host_error_guard_register_success(host);

        bool restart_listener = false;
        while (!restart_listener &&
               (host->shutdown_flag == NULL || *host->shutdown_flag == 0)) {
            ssh_session session = ssh_new();
            if (session == NULL) {
                humanized_log_error("host", "failed to allocate session",
                                    ENOMEM);
                continue;
            }

            if (ssh_bind_accept(bind_handle, session) == SSH_ERROR) {
                const int accept_error = errno;
                const char *bind_error = ssh_get_error(bind_handle);

                printf(
                    "[listener] accept failed, error=%d, shutdown_flag=%p "
                    "value=%d\n",
                    accept_error, (void *)host->shutdown_flag,
                    (host->shutdown_flag != NULL ? *host->shutdown_flag : -1));
                fflush(stdout);

                if (accept_error != 0) {
                    char log_message[512];
                    const char *system_message = strerror(accept_error);

                    if (system_message != NULL && system_message[0] != '\0') {
                        if (bind_error != NULL && bind_error[0] != '\0' &&
                            !string_contains_case_insensitive(bind_error,
                                                              system_message)) {
                            snprintf(log_message, sizeof(log_message),
                                     "Socket error: %s (%s)", system_message,
                                     bind_error);
                        } else {
                            snprintf(log_message, sizeof(log_message),
                                     "Socket error: %s", system_message);
                        }
                    } else if (bind_error != NULL && bind_error[0] != '\0') {
                        snprintf(log_message, sizeof(log_message),
                                 "Socket error (code %d): %s", accept_error,
                                 bind_error);
                    } else {
                        snprintf(log_message, sizeof(log_message),
                                 "Socket error (code %d)", accept_error);
                    }

                    humanized_log_error("host", log_message, accept_error);
                } else if (bind_error != NULL && bind_error[0] != '\0') {
                    humanized_log_error("host", bind_error, EIO);
                } else {
                    humanized_log_error("host", "Socket accept failed", EIO);
                }

                bool fatal_socket_error = false;
                bool should_backoff_after_socket_error = false;

                if (accept_error != 0) {
                    should_backoff_after_socket_error = true;
                    switch (accept_error) {
                    case EAGAIN:
#ifdef EWOULDBLOCK
#if EWOULDBLOCK != EAGAIN
                    case EWOULDBLOCK:
#endif
#endif
                    case EINTR:
                    case ECONNRESET:
                    case ECONNABORTED:
                    case ETIMEDOUT:
                    case ENOTCONN:
                    case EPIPE:
#ifdef EPROTO
                    case EPROTO:
#endif
                        should_backoff_after_socket_error = false;
                        break;
                    case EBADF:
                    case ENOTSOCK:
                    case EINVAL:
                        fatal_socket_error = true;
                        break;
                    case EMFILE:
                    case ENFILE:
                    case ENOBUFS:
#ifdef ENOMEM
                    case ENOMEM:
#endif
#ifdef ENOSR
                    case ENOSR:
#endif
                        fatal_socket_error = true;
                        should_backoff_after_socket_error = false;
                        break;
                    default:
                        break;
                    }
                }

                ssh_free(session);
                if ((fatal_socket_error && bind_error != NULL) ||
                    string_contains_case_insensitive(bind_error, "kex")) {
                    fatal_socket_error = false;
                }

                if (fatal_socket_error) {
                    clock_gettime(CLOCK_MONOTONIC,
                                  &host->listener.last_error_time);
                    if (host_listener_attempt_recover(host, bind_handle,
                                                      address, bind_port)) {
                        continue;
                    }
                    host->listener.restart_attempts += 1U;
                    printf("[listener] scheduling full listener restart after "
                           "socket "
                           "error (attempt %u)\n",
                           host->listener.restart_attempts);
                    restart_listener = true;
                    break;
                }

                if (should_backoff_after_socket_error) {
                    struct timespec retry_delay = {
                        .tv_sec = 0,
                        .tv_nsec = 200000000L,
                    };
                    host_sleep_uninterruptible(&retry_delay);
                }

                // Check shutdown flag after non-fatal errors (e.g., EINTR from signal)
                if (host->shutdown_flag != NULL && *host->shutdown_flag != 0) {
                    printf(
                        "[listener] detected shutdown flag after socket error, "
                        "breaking from accept loop\n");
                    restart_listener = true;
                    break;
                }

                continue;
            }

            hostkey_probe_result_t hostkey_probe =
                session_probe_client_hostkey_algorithms(
                    session, SSH_CHATTER_REQUIRED_HOSTKEY_ALGORITHMS,
                    SSH_CHATTER_REQUIRED_HOSTKEY_ALGORITHMS_COUNT);
            if (hostkey_probe.status == HOSTKEY_SUPPORT_REJECTED) {
                char peer_address[NI_MAXHOST];
                session_describe_peer(session, peer_address,
                                      sizeof(peer_address));
                if (peer_address[0] == '\0') {
                    strncpy(peer_address, "unknown", sizeof(peer_address) - 1U);
                    peer_address[sizeof(peer_address) - 1U] = '\0';
                }

                if (hostkey_probe.offered_algorithms[0] != '\0') {
                    printf("[reject] client %s does not accept one of [%s] "
                           "host keys "
                           "(client offered: %s)\n",
                           peer_address,
                           SSH_CHATTER_REQUIRED_HOSTKEY_ALGORITHMS_DISPLAY,
                           hostkey_probe.offered_algorithms);
                } else {
                    printf("[reject] client %s does not accept one of [%s] "
                           "host keys\n",
                           peer_address,
                           SSH_CHATTER_REQUIRED_HOSTKEY_ALGORITHMS_DISPLAY);
                }

                ssh_disconnect(session);
                ssh_free(session);
                continue;
            }

            if (ssh_handle_key_exchange(session) != SSH_OK) {
                humanized_log_error("host", ssh_get_error(session), EPROTO);
                ssh_disconnect(session);
                ssh_free(session);
                continue;
            }

            char peer_address[NI_MAXHOST];
            session_describe_peer(session, peer_address, sizeof(peer_address));
            if (peer_address[0] == '\0') {
                strncpy(peer_address, "unknown", sizeof(peer_address) - 1U);
                peer_address[sizeof(peer_address) - 1U] = '\0';
            }

            connection_guard_result_t guard =
                host_connection_guard_register(host, peer_address);
            if (guard.blocked) {
                struct timespec now_block = {0, 0};
                clock_gettime(CLOCK_MONOTONIC, &now_block);
                double wait_seconds = 0.0;
                if (timespec_compare(&guard.blocked_until, &now_block) > 0) {
                    struct timespec remaining =
                        timespec_diff(&guard.blocked_until, &now_block);
                    wait_seconds = (double)remaining.tv_sec +
                                   (double)remaining.tv_nsec / 1000000000.0;
                }
                printf(
                    "[throttle] rate-limited connection from %s (attempts=%zu, "
                    "penalty=%.2f seconds)\n",
                    peer_address, guard.attempt_count, wait_seconds);
                ssh_disconnect(session);
                ssh_free(session);
                if (guard.escalate_ban &&
                    host_add_ban_entry(host, "", peer_address)) {
                    printf("[auto-ban] %s banned after repeated connection "
                           "flooding\n",
                           peer_address);
                }
                continue;
            }

            printf("[connect] accepted client from %s\n", peer_address);

            const char *client_banner = ssh_get_clientbanner(session);
            const version_ip_ban_rule_t *matched_rule = NULL;
            if (host_version_ip_should_ban(host, client_banner, peer_address,
                                           &matched_rule)) {
                const char *version_display =
                    (client_banner != NULL && client_banner[0] != '\0')
                        ? client_banner
                        : "unknown";
                const char *pattern_display =
                    (matched_rule != NULL &&
                     matched_rule->original_pattern[0] != '\0')
                        ? matched_rule->original_pattern
                        : "policy";
                const char *cidr_display =
                    (matched_rule != NULL && matched_rule->cidr_text[0] != '\0')
                        ? matched_rule->cidr_text
                        : "unknown range";
                const char *note_display =
                    (matched_rule != NULL && matched_rule->note[0] != '\0')
                        ? matched_rule->note
                        : "version/IP policy";
                printf("[auto-ban] %s banned for client version '%s' (%s in "
                       "%s; %s)\n",
                       peer_address, version_display, pattern_display,
                       cidr_display, note_display);
                (void)host_add_ban_entry(host, "", peer_address);
                ssh_disconnect(session);
                ssh_free(session);
                continue;
            }

            if (guard.escalate_ban &&
                host_add_ban_entry(host, "", peer_address)) {
                printf(
                    "[auto-ban] %s banned after repeated connection flooding\n",
                    peer_address);
            }

            session_ctx_t *ctx = session_create();
            if (ctx == NULL) {
                humanized_log_error(
                    "host", "failed to allocate session context", ENOMEM);
                ssh_disconnect(session);
                ssh_free(session);
                continue;
            }
            ctx->ops = &ssh_session_ops;

            ctx->session = session;
            ctx->channel = NULL;
            ctx->transport_kind = SESSION_TRANSPORT_SSH;
            ctx->telnet_fd = -1;
            ctx->telnet_eof = false;
            ctx->telnet_pending_valid = false;
            pthread_mutexattr_t lock_attr;
            pthread_mutexattr_init(&lock_attr);
            pthread_mutexattr_settype(&lock_attr, PTHREAD_MUTEX_RECURSIVE);
            int mutex_error = pthread_mutex_init(&ctx->output_lock, &lock_attr);
            pthread_mutexattr_destroy(&lock_attr);
            if (mutex_error != 0) {
                humanized_log_error("host",
                                    "failed to initialise session output lock",
                                    mutex_error);
                session_destroy(ctx);
                continue;
            }
            ctx->output_lock_initialized = true;
            ctx->owner = host;
            if (pthread_mutex_init(&ctx->channel_mutex, NULL) == 0) {
                ctx->channel_mutex_initialized = true;
            } else {
                humanized_log_error("session",
                                    "failed to initialize channel mutex",
                                    errno != 0 ? errno : ENOMEM);
            }
            ctx->auth = (auth_profile_t){0};
            snprintf(ctx->client_ip, sizeof(ctx->client_ip), "%.*s",
                     (int)sizeof(ctx->client_ip) - 1, peer_address);
            ctx->input_mode = SESSION_INPUT_MODE_CHAT;

            session_ui_language_t geo_language =
                session_client_geo_language(ctx);
            if (geo_language != SESSION_UI_LANGUAGE_COUNT) {
                ctx->ui_language = geo_language;
            } else {
                ctx->ui_language = SESSION_UI_LANGUAGE_KO;
            }
            if (client_banner != NULL && client_banner[0] != '\0') {
                snprintf(ctx->client_banner, sizeof(ctx->client_banner), "%s",
                         client_banner);
            }
            session_refresh_output_encoding(ctx);

            pthread_mutex_lock(&host->lock);
            ++host->connection_count;
            snprintf(ctx->user.name, sizeof(ctx->user.name), "Guest%zu",
                     host->connection_count);
            ctx->user.is_operator = false;
            ctx->user.is_lan_operator = false;
            pthread_mutex_unlock(&host->lock);

            pthread_t thread_id;
            if (pthread_create(&thread_id, NULL, session_thread, ctx) != 0) {
                humanized_log_error("host", "failed to spawn session thread",
                                    errno);
                session_destroy(ctx);
                continue;
            }

            pthread_detach(thread_id);
            host_error_guard_register_success(host);
        }

        ssh_bind_free(bind_handle);
        host->listener.handle = NULL;

        // Check for shutdown signal before deciding to restart
        if (host->shutdown_flag != NULL && *host->shutdown_flag != 0) {
            printf(
                "[listener] shutdown signal received, exiting listener loop\n");
            break;
        }

        if (!restart_listener) {
            host_sleep_after_error(host);
            continue;
        }

        struct timespec backoff = {
            .tv_sec = 1,
            .tv_nsec = 0,
        };
        host_sleep_uninterruptible(&backoff);

        if (host->listener.restart_attempts > 0U) {
            printf("[listener] attempting full listener restart after socket "
                   "error "
                   "(attempt %u)\n",
                   host->listener.restart_attempts);
        } else {
            printf("[listener] attempting full listener restart after socket "
                   "error\n");
        }

        if (host->listener.last_error_time.tv_sec != 0 ||
            host->listener.last_error_time.tv_nsec != 0L) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            struct timespec elapsed =
                timespec_diff(&now, &host->listener.last_error_time);
            double elapsed_seconds =
                (double)elapsed.tv_sec + (double)elapsed.tv_nsec / 1000000000.0;
            printf("[listener] last fatal error occurred %.3f seconds ago\n",
                   elapsed_seconds);
        }
    }

    return 0;
}

}

void session_handle_hybrid(session_ctx_t *ctx, const char *arguments)
{
    static const char *kUsage = "Usage: /hybrid <on|off|status>";

    if (ctx == nullptr) {
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    if (arguments != nullptr) {
        snprintf(working, sizeof(working), "%s", arguments);
        trim_whitespace_inplace(working);
    } else {
        working[0] = '\0';
    }

    if (working[0] == '\0' || strcasecmp(working, "status") == 0) {
        session_send_system_line(
            ctx,
            ctx->hybrid_output_mode
                ? "Hybrid encoding detection is enabled. Mixed content will"
                  " stay UTF-8 while retro-safe system output uses legacy"
                  " encoding."
                : "Hybrid encoding detection is disabled. Output encoding"
                  " follows the retro scope as-is.");
        return;
    }

    if (strcasecmp(working, "on") == 0) {
        ctx->hybrid_output_mode = true;
        session_send_system_line(
            ctx, "Hybrid encoding detection enabled for retro output.");
        return;
    }

    if (strcasecmp(working, "off") == 0) {
        ctx->hybrid_output_mode = false;
        session_send_system_line(ctx, "Hybrid encoding detection disabled.");
        return;
    }

    session_send_system_line(ctx, kUsage);
}

void session_handle_iyagi(session_ctx_t *ctx, const char *arguments)
{
    static const char *kUsage = "Usage: /iyagi <on|off|status>";

    if (ctx == nullptr) {
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    if (arguments != nullptr) {
        snprintf(working, sizeof(working), "%s", arguments);
        trim_whitespace_inplace(working);
    } else {
        working[0] = '\0';
    }

    if (working[0] == '\0' || strcasecmp(working, "status") == 0) {
        const bool iyagi_active =
            ctx->cp437_override == SESSION_CP437_OVERRIDE_FORCE_ON &&
            ctx->cp437_output_scope == SESSION_CP437_SCOPE_SYSTEM_ONLY &&
            !ctx->cp437_input_enabled && ctx->hybrid_output_mode &&
            ctx->active_codepage == SESSION_CODEPAGE_CP949;

        char status[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(status, sizeof(status),
                 "IYAGI profile: %s (ui scope: %s, keyboard: %s, codepage: %s, "
                 "hybrid: %s).",
                 iyagi_active ? "enabled" : "disabled",
                 session_cp437_scope_label(ctx->cp437_output_scope),
                 ctx->cp437_input_enabled ? "legacy" : "modern UTF-8",
                 session_codepage_name(ctx->active_codepage),
                 ctx->hybrid_output_mode ? "enabled" : "disabled");
        session_send_system_line(ctx, status);
        session_send_system_line(
            ctx, "Windows 11 + legacy CP949 app profile: system UI renders in "
                 "legacy codepage, chat stays UTF-8.");
        return;
    }

    if (strcasecmp(working, "on") == 0) {
        ctx->cp437_output_scope = SESSION_CP437_SCOPE_SYSTEM_ONLY;
        ctx->cp437_override = SESSION_CP437_OVERRIDE_FORCE_ON;
        ctx->cp437_input_enabled = false;
        ctx->hybrid_output_mode = true;
        ctx->active_codepage = SESSION_CODEPAGE_CP949;
        ctx->ui_language = SESSION_UI_LANGUAGE_KO;
        if (ctx->owner != nullptr) {
            host_store_ui_language(ctx->owner, ctx);
        }
        session_refresh_output_encoding(ctx);
        session_send_system_line(
            ctx, "IYAGI mode enabled (Windows 11 legacy CP949 profile).");
        return;
    }

    if (strcasecmp(working, "off") == 0) {
        ctx->cp437_output_scope = SESSION_CP437_SCOPE_ALL;
        ctx->cp437_override = SESSION_CP437_OVERRIDE_NONE;
        ctx->cp437_input_enabled = false;
        ctx->hybrid_output_mode = false;
        session_refresh_output_encoding(ctx);
        session_send_system_line(ctx, "IYAGI mode disabled.");
        return;
    }

    session_send_system_line(ctx, kUsage);
}

static bool
host_provider_language_preference(host_t *host, const char *provider_label,
                                  session_ui_language_t *out_language)
{
    if (host == nullptr || provider_label == nullptr ||
        provider_label[0] == '\0' || out_language == nullptr) {
        return false;
    }

    size_t counts[SESSION_UI_LANGUAGE_COUNT] = {0};
    size_t total = 0U;

    ttak_mutex_lock(&host->lock);
    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_PREFERENCES; ++idx) {
        const user_preference_t *pref = &host->preferences[idx];
        if (!pref->in_use || pref->ui_language[0] == '\0') {
            continue;
        }

        char resolved_label[SSH_CHATTER_PROVIDER_LABEL_LEN] = {0};
        if (pref->provider_label[0] != '\0') {
            snprintf(resolved_label, sizeof(resolved_label), "%s",
                     pref->provider_label);
        } else if (pref->ip[0] != '\0') {
            (void)session_detect_provider_ip(pref->ip, resolved_label,
                                             sizeof(resolved_label));
        }

        if (resolved_label[0] == '\0' ||
            strcasecmp(resolved_label, provider_label) != 0) {
            continue;
        }

        session_ui_language_t lang =
            session_ui_language_from_code(pref->ui_language);
        if (lang == SESSION_UI_LANGUAGE_COUNT) {
            continue;
        }

        ++counts[(size_t)lang];
        ++total;
    }
    ttak_mutex_unlock(&host->lock);

    if (total < 4U) {
        return false;
    }

    size_t best_count = 0U;
    session_ui_language_t best_language = SESSION_UI_LANGUAGE_COUNT;
    for (size_t idx = 0U; idx < SESSION_UI_LANGUAGE_COUNT; ++idx) {
        if (counts[idx] > best_count) {
            best_count = counts[idx];
            best_language = (session_ui_language_t)idx;
        }
    }

    if (best_language == SESSION_UI_LANGUAGE_COUNT) {
        return false;
    }

    *out_language = best_language;
    return true;
}

static void host_sync_state_resolve_path(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    const char *sync_path = getenv("CHATTER_SYNC_STATE_FILE");
    if (sync_path == nullptr || sync_path[0] == '\0') {
        sync_path = "sync_chatter_state.dat";
    }

    int written = snprintf(host->sync_state_file_path,
                           sizeof(host->sync_state_file_path), "%s", sync_path);
    if (written < 0 || (size_t)written >= sizeof(host->sync_state_file_path)) {
        humanized_log_error("host", "sync state file path is too long",
                            ENAMETOOLONG);
        host->sync_state_file_path[0] = '\0';
    }
}

static void session_destroy(session_ctx_t *ctx);

static size_t session_encode_utf8_codepoint(uint32_t codepoint, char *output,
                                            size_t capacity)
{
    if (output == nullptr || capacity == 0U) {
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
    if (output == nullptr || capacity == 0U) {
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
    if (ctx == nullptr || ctx->owner == nullptr) {
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
        host_history_record_system(ctx->owner, notice, nullptr);
        chat_room_broadcast(&ctx->owner->room, notice, nullptr);
    } else {
        session_send_system_line(ctx,
                                 "No automatic Gemini cooldown was active.");
    }
}

static void session_handle_palette(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (arguments == nullptr) {
        session_send_system_line(ctx,
                                 "Usage: /palette <name> (try /palette list)");
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);

    if (working[0] == '\0' || strcasecmp(working, "list") == 0) {
        session_send_system_line(ctx, "Available palettes:");

        /* +1 ensures there is always room for the null terminator after 16
         * entries each up to SSH_CHATTER_MESSAGE_LIMIT bytes long. */
        static char palette_line_group[SSH_CHATTER_MESSAGE_LIMIT * 16 + 1U];
        /* Reset the static buffer at the start of each listing call so that
         * stale data from a previous invocation cannot bleed through. */
        size_t group_len = 0U;
        palette_line_group[0] = '\0';

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
            if (descriptor->is_256_color) {
                snprintf(palette_line, sizeof(palette_line),
                         "\033[1G  %s\033[1G\n   - %s (256-color)\n", name,
                         description);
            } else {
                snprintf(palette_line, sizeof(palette_line),
                         "\033[1G  %s\033[1G\n   - %s\n", name, description);
            }
            /* Use offset-tracked snprintf to avoid strncat truncation warnings
             * and ensure the group buffer is never overflowed. */
            size_t remaining = sizeof(palette_line_group) - group_len;
            if (remaining > 1U) {
                int written = snprintf(palette_line_group + group_len,
                                       remaining, "%s\n", palette_line);
                if (written > 0) {
                    group_len += (size_t)written < remaining
                                     ? (size_t)written
                                     : remaining - 1U;
                }
            }
            if ((idx + 1) % 16 == 0) {
                session_send_system_line(ctx, palette_line_group);
                /* Use sizeof(palette_line_group) — NOT sizeof(integer expression) —
                 * to zero the full buffer, then reset the offset. */
                memset(palette_line_group, 0, sizeof(palette_line_group));
                group_len = 0U;
            }
        }
        /* Flush any remaining entries that did not fill a full 16-entry batch. */
        if (group_len > 0U) {
            session_send_system_line(ctx, palette_line_group);
            palette_line_group[0] = '\0';
            group_len = 0U;
        }
        (void)group_len;
        session_send_system_line(ctx, "Apply a palette with /palette <name>.");
        return;
    }

    const palette_descriptor_t *descriptor = palette_find_descriptor(working);
    if (descriptor == nullptr) {
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

    if (ctx->owner != nullptr) {
        host_store_user_theme(ctx->owner, ctx);
        host_store_system_theme(ctx->owner, ctx);
    }
}

void session_handle_retro(session_ctx_t *ctx, const char *arguments)
{
    static const char *kUsage =
        "Usage: /retro <on [ko|en|jp|zh|ru|de|fr|pl] [system|chat|all]|off|"
        "auto|status|keyboard <on|off|status|cp437|cp949|cp932|cp936|cp1251|"
        "cp850|cp852>|ui <system|chat|all|status>>";

    if (ctx == nullptr) {
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    if (arguments != nullptr) {
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

        const char *codepage_name = session_codepage_name(ctx->active_codepage);
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        char output_description[64];
        if (ctx->prefer_cp437_output) {
            switch (ctx->cp437_output_scope) {
            case SESSION_CP437_SCOPE_SYSTEM_ONLY:
                snprintf(output_description, sizeof(output_description),
                         "legacy (system-only)");
                break;
            case SESSION_CP437_SCOPE_CHAT_ONLY:
                snprintf(output_description, sizeof(output_description),
                         "legacy (chat-only)");
                break;
            case SESSION_CP437_SCOPE_ALL:
            default:
                snprintf(output_description, sizeof(output_description),
                         "legacy");
                break;
            }
        } else {
            snprintf(output_description, sizeof(output_description),
                     "UTF-8 only");
        }

        const char *scope_label =
            session_cp437_scope_label(ctx->cp437_output_scope);
        snprintf(message, sizeof(message),
                 "Retro encoding mode: %s (scope: %s, codepage: %s, input: %s, "
                 "output: %s).",
                 mode, scope_label, codepage_name,
                 ctx->cp437_input_enabled ? "legacy" : "UTF-8",
                 output_description);
        session_send_system_line(ctx, message);
        snprintf(message, sizeof(message), "Hybrid detection: %s.",
                 ctx->hybrid_output_mode ? "enabled" : "disabled");
        session_send_system_line(ctx, message);
        session_send_system_line(
            ctx, "Toggle with /retro on [lang], /retro off, or /retro auto.");
        session_send_system_line(
            ctx, "Supported languages: ko, en, jp, zh, ru, de, fr, pl.");
        if (ctx->cp437_override == SESSION_CP437_OVERRIDE_NONE) {
            session_send_system_line(
                ctx, "Hybrid retro/Unicode auto-conversion is active for all "
                     "languages when automatic detection is enabled.");
        }
        return;
    }

    if (strncasecmp(working, "keyboard", 8) == 0 &&
        (working[8] == '\0' || isspace((unsigned char)working[8]) != 0)) {
        const char *arg = working + 8;
        while (*arg != '\0' && isspace((unsigned char)*arg) != 0) {
            ++arg;
        }

        session_codepage_t requested_codepage = ctx->active_codepage;
        bool has_codepage = false;
        if (strcasecmp(arg, "cp437") == 0) {
            requested_codepage = SESSION_CODEPAGE_CP437;
            has_codepage = true;
        } else if (strcasecmp(arg, "cp949") == 0) {
            requested_codepage = SESSION_CODEPAGE_CP949;
            has_codepage = true;
        } else if (strcasecmp(arg, "cp932") == 0) {
            requested_codepage = SESSION_CODEPAGE_CP932;
            has_codepage = true;
        } else if (strcasecmp(arg, "cp936") == 0) {
            requested_codepage = SESSION_CODEPAGE_CP936;
            has_codepage = true;
        } else if (strcasecmp(arg, "cp1251") == 0) {
            requested_codepage = SESSION_CODEPAGE_CP1251;
            has_codepage = true;
        } else if (strcasecmp(arg, "cp850") == 0) {
            requested_codepage = SESSION_CODEPAGE_CP850;
            has_codepage = true;
        } else if (strcasecmp(arg, "cp852") == 0) {
            requested_codepage = SESSION_CODEPAGE_CP852;
            has_codepage = true;
        }

        if (*arg == '\0' || strcasecmp(arg, "status") == 0) {
            char status[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(status, sizeof(status),
                     "Retro keyboard is %s (%s).",
                     ctx->cp437_input_enabled ? "enabled" : "disabled",
                     session_codepage_name(ctx->active_codepage));
            session_send_system_line(
                ctx, status);
            return;
        }

        if (strcasecmp(arg, "on") == 0) {
            ctx->cp437_input_enabled = true;
            char status[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(status, sizeof(status),
                     "Retro keyboard enabled (%s input).",
                     session_codepage_name(ctx->active_codepage));
            session_send_system_line(
                ctx, status);
            return;
        }

        if (strcasecmp(arg, "off") == 0) {
            ctx->cp437_input_enabled = false;
            session_send_system_line(
                ctx, "Retro keyboard disabled (modern UTF-8 input).");
            return;
        }

        if (has_codepage) {
            ctx->active_codepage = requested_codepage;
            ctx->cp437_input_enabled = true;
            char status[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(status, sizeof(status),
                     "Retro keyboard enabled with %s input.",
                     session_codepage_name(requested_codepage));
            session_send_system_line(ctx, status);
            return;
        }

        session_send_system_line(
            ctx, "Usage: /retro keyboard <on|off|status|cp437|cp949|cp932|"
                 "cp936|cp1251|cp850|cp852>");
        return;
    }

    if (strncasecmp(working, "ui", 2) == 0 &&
        (working[2] == '\0' || isspace((unsigned char)working[2]) != 0)) {
        const char *arg = working + 2;
        while (*arg != '\0' && isspace((unsigned char)*arg) != 0) {
            ++arg;
        }

        if (*arg == '\0' || strcasecmp(arg, "status") == 0) {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message),
                     "Retro UI scope is %s. Hybrid mode is %s.",
                     session_cp437_scope_label(ctx->cp437_output_scope),
                     ctx->hybrid_output_mode ? "enabled" : "disabled");
            session_send_system_line(ctx, message);
            return;
        }

        if (strcasecmp(arg, "hybrid") == 0) {
            ctx->hybrid_output_mode = true;
            session_send_system_line(ctx,
                                     "Retro UI hybrid mode enabled.");
            return;
        }
        if (strcasecmp(arg, "utf8") == 0 || strcasecmp(arg, "modern") == 0) {
            ctx->hybrid_output_mode = false;
            session_send_system_line(ctx,
                                     "Retro UI hybrid mode disabled.");
            return;
        }
        if (strcasecmp(arg, "jp") == 0 || strcasecmp(arg, "japanese") == 0) {
            ctx->ui_language = SESSION_UI_LANGUAGE_JP;
            ctx->active_codepage = session_codepage_for_language(ctx->ui_language);
            if (ctx->owner != nullptr) {
                host_store_ui_language(ctx->owner, ctx);
            }
            session_send_system_line(ctx,
                                     "Retro UI language set to Japanese mode.");
            return;
        }

        session_cp437_scope_t parsed_scope = ctx->cp437_output_scope;
        if (!session_cp437_scope_parse(arg, &parsed_scope)) {
            session_send_system_line(
                ctx,
                "Usage: /retro ui <system|chat|all|status|hybrid|utf8|jp>");
            return;
        }

        ctx->cp437_output_scope = parsed_scope;
        session_refresh_output_encoding(ctx);
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "Retro UI scope set to %s.",
                 session_cp437_scope_label(parsed_scope));
        session_send_system_line(ctx, message);
        return;
    }

    // Handle "on" with optional language parameter
    if (strncasecmp(working, "on", 2) == 0) {
        const char *lang_arg = working + 2;
        while (*lang_arg == ' ' || *lang_arg == '\t') {
            ++lang_arg;
        }

        char first_token[16] = {0};
        char second_token[16] = {0};
        int token_count =
            sscanf(lang_arg, "%15s %15s", first_token, second_token);

        const char *lang_token = nullptr;
        const char *scope_token = nullptr;
        session_cp437_scope_t requested_scope = ctx->cp437_output_scope;

        if (token_count >= 1) {
            session_ui_language_t token_language =
                session_ui_language_from_code(first_token);
            if (token_language != SESSION_UI_LANGUAGE_COUNT ||
                strcasecmp(first_token, "en") == 0) {
                lang_token = first_token;
                if (token_count >= 2) {
                    scope_token = second_token;
                }
            } else {
                scope_token = first_token;
                if (token_count >= 2) {
                    lang_token = second_token;
                }
            }
        }

        // If language is specified, set UI language
        if (lang_token != nullptr && lang_token[0] != '\0') {
            // Try to parse language code
            session_ui_language_t new_lang =
                session_ui_language_from_code(lang_token);
            if (new_lang != SESSION_UI_LANGUAGE_EN ||
                strcasecmp(lang_token, "en") == 0) {
                ctx->ui_language = new_lang;
                /* Set the appropriate code page for the language */
                ctx->active_codepage = session_codepage_for_language(new_lang);
                if (ctx->owner != nullptr) {
                    host_store_ui_language(ctx->owner, ctx);
                }
            }
        } else {
            /* No language specified, use code page for current UI language */
            ctx->active_codepage =
                session_codepage_for_language(ctx->ui_language);
        }

        if (scope_token != nullptr && scope_token[0] != '\0') {
            session_cp437_scope_t parsed_scope = requested_scope;
            if (!session_cp437_scope_parse(scope_token, &parsed_scope)) {
                session_send_system_line(
                    ctx, "Scope must be one of: all, system, or chat.");
                return;
            }
            requested_scope = parsed_scope;
        }

        ctx->cp437_output_scope = requested_scope;

        ctx->cp437_override = SESSION_CP437_OVERRIDE_FORCE_ON;
        ctx->hybrid_output_mode = true;
        session_refresh_output_encoding(ctx);

        char message[SSH_CHATTER_MESSAGE_LIMIT];
        const char *codepage_name = session_codepage_name(ctx->active_codepage);
        const char *scope_label = session_cp437_scope_label(requested_scope);
        if (lang_token != nullptr && lang_token[0] != '\0') {
            snprintf(message, sizeof(message),
                     "Retro encoding enabled with language %s (%s) for %s. "
                     "Legacy code page input and output are forced on.",
                     lang_token, codepage_name, scope_label);
        } else {
            snprintf(message, sizeof(message),
                     "Retro encoding enabled (%s) for %s. "
                     "Legacy code page input and output are forced on.",
                     codepage_name, scope_label);
        }
        session_send_system_line(ctx, message);
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

static void session_handle_ai_chat(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(ctx,
                                 "Only operators may control ai-eliza.");
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
        bool enabled = atomic_load(&ctx->owner->ai_chat_enabled);
        char status[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(status, sizeof(status), "ai-eliza is currently %s.",
                 enabled ? "enabled" : "disabled");
        session_send_system_line(ctx, status);
        session_send_system_line(ctx, "Usage: /ai-chat <on|off>");
        session_send_system_line(
            ctx, "When enabled, mention \"ai-eliza\" in chat to start a "
                 "conversation.");
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
        session_send_system_line(ctx, "Usage: /ai-chat <on|off>");
        return;
    }

    if (requested_enable) {
        if (host_ai_chat_enable(ctx->owner)) {
            session_send_system_line(ctx,
                                     "ai-eliza is now active for casual chat.");
        } else {
            session_send_system_line(ctx, "ai-eliza is already chatting.");
        }
        return;
    }

    if (host_ai_chat_disable(ctx->owner)) {
        session_send_system_line(ctx, "ai-eliza has been muted.");
    } else {
        session_send_system_line(ctx, "ai-eliza is already inactive.");
    }
}

static void session_handle_ollama_model(session_ctx_t *ctx,
                                        const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(
            ctx, "Only operators may configure the Ollama model.");
        return;
    }

    char working[sizeof(ctx->owner->ai_chat_model)];
    if (arguments != nullptr) {
        snprintf(working, sizeof(working), "%s", arguments);
        trim_whitespace_inplace(working);
    } else {
        working[0] = '\0';
    }

    host_t *host = ctx->owner;
    if (working[0] == '\0') {
        char model[sizeof(host->ai_chat_model)];
        host_ai_chat_snapshot_state(host, model, sizeof(model), nullptr);
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "Current Ollama model: %s%s.",
                 model,
                 (strcasecmp(model, host_ai_chat_default_model()) == 0)
                     ? " (default)"
                     : "");
        session_send_system_line(ctx, message);
        session_send_system_line(ctx, "Usage: /ollama-model <model_name>");
        return;
    }

    size_t length = strlen(working);
    if (length >= sizeof(host->ai_chat_model)) {
        session_send_system_line(ctx, "Model name is too long.");
        return;
    }

    ttak_mutex_lock(&host->lock);
    snprintf(host->ai_chat_model, sizeof(host->ai_chat_model), "%s", working);
    ttak_mutex_unlock(&host->lock);

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(message, sizeof(message),
             "Ollama model updated to '%s'. ai-eliza will use it on the next "
             "reply.",
             working);
    session_send_system_line(ctx, message);
}

static bool find_reserved_names(session_ctx_t *ctx, const char *nick)
{
    if (ctx == nullptr || ctx->owner == nullptr || nick == nullptr ||
        nick[0] == '\0') {
        return false;
    }

    bool found = false;
    ttak_mutex_lock(&ctx->owner->nickname_reserve_lock);
    if (ctx->owner->reserved_nicknames == nullptr) {
        ttak_mutex_unlock(&ctx->owner->nickname_reserve_lock);
        return false;
    }
    for (size_t i = 0; i < ctx->owner->reserved_nicknames_len; ++i) {
        if (strcasecmp(nick, ctx->owner->reserved_nicknames[i]) == 0) {
            found = true;
            break;
        }
    }
    ttak_mutex_unlock(&ctx->owner->nickname_reserve_lock);
    return found;
}

bool host_username_has_password(host_t *host, const char *nick)
{
    if (host == nullptr || nick == nullptr || nick[0] == '\0') {
        return false;
    }

    user_data_record_t record;
    if (host_user_data_load_existing(host, nick, nullptr, &record, false) &&
        !security_layer_is_zero_hash(record.password_hash,
                                     sizeof(record.password_hash))) {
        return true;
    }

    if (host->pw_auth_file_path[0] == '\0') {
        return false;
    }

    FILE *fp = fopen(host->pw_auth_file_path, "rb");
    if (fp == nullptr) {
        return false;
    }

    bool protected_name = false;
    char line[SSH_CHATTER_MESSAGE_LIMIT];
    while (!protected_name && fgets(line, sizeof(line), fp) != nullptr) {
        size_t length = strcspn(line, "\r\n");
        line[length] = '\0';

        char *first_separator = strchr(line, ':');
        if (first_separator == nullptr) {
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

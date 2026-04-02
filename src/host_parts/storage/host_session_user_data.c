static void session_refresh_output_encoding(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    /*
     * UTF-16 output is reserved for explicit local console backends only.
     * Network transports (SSH/TELNET) must always remain byte-oriented.
     */
    bool use_utf16 = false;

    const bool previous_cp437 = ctx->prefer_cp437_output;
    const bool previous_cp437_input = ctx->cp437_input_enabled;
    const bool has_client_identity =
        (ctx->terminal_type[0] != '\0') || (ctx->client_banner[0] != '\0');

    bool use_cp437 = session_detect_retro_client(ctx);

    if (!has_client_identity &&
        ctx->cp437_override == SESSION_CP437_OVERRIDE_NONE && previous_cp437) {
        use_cp437 = true;
        ctx->cp437_input_enabled = previous_cp437_input;
    }

    if (ctx->cp437_override == SESSION_CP437_OVERRIDE_FORCE_ON) {
        use_cp437 = true;
    } else if (ctx->cp437_override == SESSION_CP437_OVERRIDE_FORCE_OFF) {
        use_cp437 = false;
    }

    ctx->prefer_cp437_output = use_cp437;

    if (use_cp437 != previous_cp437) {
        const char *subject =
            ctx->user.name[0] != '\0' ? ctx->user.name : ctx->client_ip;
        if (subject == nullptr || subject[0] == '\0') {
            subject = "unknown";
        }

        if (use_cp437) {
            if (ctx->cp437_override == SESSION_CP437_OVERRIDE_FORCE_ON) {
                printf("[retro] manually enabling CP437 for %s via /retro "
                       "command\n",
                       subject);
            } else {
                const char *marker = ctx->retro_client_marker[0] != '\0'
                                         ? ctx->retro_client_marker
                                         : "retro client";
                if (ctx->telnet_identity[0] != '\0') {
                    printf("[retro] enabling CP437 output for %s via %s (%s)\n",
                           subject, marker, ctx->telnet_identity);
                } else {
                    printf("[retro] enabling CP437 output for %s via %s\n",
                           subject, marker);
                }
            }
        } else {
            if (ctx->cp437_override == SESSION_CP437_OVERRIDE_FORCE_OFF) {
                printf("[retro] manually disabling CP437 for %s via /retro "
                       "command\n",
                       subject);
            } else {
                printf("[retro] CP437 output disabled for %s\n", subject);
            }
        }

        if (use_cp437 && ctx->prelogin_banner_rendered) {
            session_render_banner_ascii(ctx);
        }
    }

    if (ctx->cp437_override == SESSION_CP437_OVERRIDE_FORCE_ON) {
        ctx->cp437_input_enabled = true;
    } else if (ctx->cp437_override == SESSION_CP437_OVERRIDE_FORCE_OFF) {
        ctx->cp437_input_enabled = false;
    }

    ctx->prefer_utf16_output = (!use_cp437) && use_utf16;
}

static bool session_detect_retro_client(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }

    ctx->retro_client_marker[0] = '\0';

    typedef struct retro_marker {
        const char *marker;
        const char *label;
    } retro_marker_t;

    static const retro_marker_t kRetroMarkers[] = {
        {"ftelnet", "fTelnet"},      {"htmlterm", "HTMLTerm"},
        {"syncterm", "SyncTERM"},    {"netrunner", "NetRunner"},
        {"netfury", "NetFury"},      {"qodem", "Qodem"},
        {"mtelnet", "MTelnet"},      {"etherterm", "EtherTerm"},
        {"mysticbbs", "Mystic BBS"}, {"ansi-bbs", "ANSI-BBS"},
        {"pc-ansi", "PC-ANSI"},      {"cp-437", "CP437 terminal"},
        {"cp437", "CP437 terminal"}, {"avatar", "AVATAR terminal"},
        {"ripterm", "RIPTerm"},      {"ansiart", "ANSI art terminal"},
        {"ansi", "ANSI terminal"},   {"icyterm", "IcyTerm"},
    };

    const char *sources[] = {
        ctx->terminal_type,
        ctx->client_banner,
    };

    const char *label = nullptr;
    const char *identity_label = nullptr;
    bool detected = false;
    bool saerom_client = false;

    for (size_t source_idx = 0U;
         source_idx < sizeof(sources) / sizeof(sources[0]) && !detected;
         ++source_idx) {
        const char *candidate = sources[source_idx];
        if (candidate == nullptr || candidate[0] == '\0') {
            continue;
        }
        for (size_t marker_idx = 0U;
             marker_idx < sizeof(kRetroMarkers) / sizeof(kRetroMarkers[0]);
             ++marker_idx) {
            if (string_contains_case_insensitive(
                    candidate, kRetroMarkers[marker_idx].marker)) {
                label = kRetroMarkers[marker_idx].label;
                identity_label = label;
                detected = true;
                if (string_contains_case_insensitive(candidate, "saerom") ||
                    string_contains_case_insensitive(candidate, "dataman")) {
                    saerom_client = true;
                }
                break;
            }
        }
    }

    if (!detected && ctx->terminal_type[0] != '\0') {
        const char *type = ctx->terminal_type;
        if (string_contains_case_insensitive(type, "syncterm")) {
            label = "SyncTERM";
            identity_label = label;
            detected = true;
        } else if (string_contains_case_insensitive(type, "saerom") ||
                   string_contains_case_insensitive(type, "dataman")) {
            label = "Saerom DataMan";
            identity_label = label;
            detected = true;
            saerom_client = true;
        } else if (string_contains_token_case_insensitive(type, "ANSI-BBS")) {
            label = "ANSI-BBS terminal";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(type, "PC-ANSI")) {
            label = "PC-ANSI terminal";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(type, "CP-437") ||
                   string_contains_token_case_insensitive(type, "CP437")) {
            label = "CP437 terminal";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(type,
                                                          "IBMGRAPHICS") ||
                   string_contains_token_case_insensitive(type, "IBM-ASCII") ||
                   string_contains_token_case_insensitive(type, "IBMPC")) {
            label = "IBM PC terminal";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(type, "AVATAR")) {
            label = "AVATAR terminal";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(type, "RIPTERM")) {
            label = "RIPTerm terminal";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(type, "PETSCII") ||
                   string_contains_token_case_insensitive(type, "ATASCII")) {
            label = "8-bit art terminal";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(type, "DOS")) {
            label = "DOS ANSI terminal";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(type, "BBS")) {
            label = "BBS terminal";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(type, "ANSI")) {
            label = "ANSI terminal";
            identity_label = label;
            detected = true;
        }
    }

    if (!detected && ctx->client_banner[0] != '\0') {
        const char *banner = ctx->client_banner;
        if (string_contains_case_insensitive(banner, "syncterm")) {
            label = "SyncTERM";
            identity_label = label;
            detected = true;
        } else if (string_contains_case_insensitive(banner, "saerom") ||
                   string_contains_case_insensitive(banner, "dataman")) {
            label = "Saerom DataMan";
            identity_label = label;
            detected = true;
            saerom_client = true;
        } else if (string_contains_token_case_insensitive(banner, "ANSI-BBS") ||
                   string_contains_token_case_insensitive(banner, "PC-ANSI")) {
            label = "ANSI-BBS banner";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(banner, "BBS")) {
            label = "BBS banner";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(banner, "ANSI")) {
            label = "ANSI banner";
            identity_label = label;
            detected = true;
        }
    }

    if (!detected && ctx->os_name[0] != '\0') {
        static const char *const kDosFamilies[] = {"msdos", "drdos", "pcdos",
                                                   "kdos"};
        for (size_t idx = 0U;
             idx < sizeof(kDosFamilies) / sizeof(kDosFamilies[0]); ++idx) {
            if (strcasecmp(ctx->os_name, kDosFamilies[idx]) == 0) {
                label = "DOS OS";
                identity_label = nullptr;
                detected = true;
                break;
            }
        }
    }

    if (detected) {
        const char *display =
            (label != nullptr && label[0] != '\0') ? label : "Retro terminal";
        snprintf(ctx->retro_client_marker, sizeof(ctx->retro_client_marker),
                 "%s", display);

        if (saerom_client) {
            ctx->cp437_output_scope = SESSION_CP437_SCOPE_SYSTEM_ONLY;
            ctx->hybrid_output_mode = true;
        }
    }

    session_format_telnet_identity(ctx, detected ? identity_label : nullptr);

    ctx->cp437_input_enabled = saerom_client ? false : detected;

    return detected;
}

static void session_apply_user_data_theme(session_ctx_t *ctx,
                                          const user_data_record_t *record)
{
    if (ctx == nullptr || record == nullptr || !record->has_user_theme) {
        return;
    }

    const char *color_code = nullptr;
    const char *highlight_code = nullptr;

    if (record->user_color_code[0] != '\0') {
        color_code = record->user_color_code;
    } else if (record->user_color_name[0] != '\0') {
        color_code = lookup_color_code(
            USER_COLOR_MAP, sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
            record->user_color_name);
    }

    if (record->user_highlight_code[0] != '\0') {
        highlight_code = record->user_highlight_code;
    } else if (record->user_highlight_name[0] != '\0') {
        highlight_code = lookup_color_code(HIGHLIGHT_COLOR_MAP,
                                           sizeof(HIGHLIGHT_COLOR_MAP) /
                                               sizeof(HIGHLIGHT_COLOR_MAP[0]),
                                           record->user_highlight_name);
    }

    if (color_code != nullptr && highlight_code != nullptr) {
        snprintf(ctx->user_color_code, sizeof(ctx->user_color_code), "%s",
                 color_code);
        snprintf(ctx->user_highlight_code, sizeof(ctx->user_highlight_code),
                 "%s", highlight_code);
        ctx->user_is_bold = record->user_is_bold != 0U;
        snprintf(ctx->user_color_name, sizeof(ctx->user_color_name), "%s",
                 record->user_color_name);
        snprintf(ctx->user_highlight_name, sizeof(ctx->user_highlight_name),
                 "%s", record->user_highlight_name);
    }
}

static void session_apply_saved_preferences(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    host_t *host = ctx->owner;
    const bool user_data_loaded = session_user_data_load(ctx);
    const user_data_record_t *user_record =
        user_data_loaded ? &ctx->user_data : nullptr;
    user_preference_t base_snapshot = (user_preference_t){0};
    user_preference_t ip_snapshot = (user_preference_t){0};
    bool has_base_snapshot = false;
    bool has_ip_snapshot = false;
    bool user_theme_applied = false;

    ttak_mutex_lock(&host->lock);
    user_preference_t *pref =
        host_find_preference_locked(host, ctx->user.name, "");
    if (pref != nullptr) {
        base_snapshot = *pref;
        has_base_snapshot = true;
    }
    if (ctx->client_ip[0] != '\0') {
        user_preference_t *ip_pref =
            host_find_preference_locked(host, ctx->user.name, ctx->client_ip);
        if (ip_pref != nullptr && (pref == nullptr || ip_pref != pref)) {
            ip_snapshot = *ip_pref;
            has_ip_snapshot = true;
        }
    }
    ttak_mutex_unlock(&host->lock);

    session_ui_language_t previous_language = ctx->ui_language;
    session_cp437_override_t previous_cp437_override = ctx->cp437_override;
    bool previous_cp437_output = ctx->prefer_cp437_output;
    bool previous_cp437_input = ctx->cp437_input_enabled;

    ctx->prefer_utf16_output = false;

    ctx->translation_caption_spacing = 0U;
    ctx->translation_enabled = false;
    ctx->output_translation_enabled = false;
    ctx->output_translation_language[0] = '\0';
    ctx->input_translation_enabled = false;
    ctx->input_translation_language[0] = '\0';
    ctx->last_detected_input_language[0] = '\0';
    ctx->breaking_alerts_enabled = false;

    if (has_base_snapshot) {
        if (base_snapshot.ui_language[0] != '\0') {
            session_ui_language_t saved_language =
                session_ui_language_from_code(base_snapshot.ui_language);
            if (saved_language != SESSION_UI_LANGUAGE_COUNT) {
                ctx->ui_language = saved_language;
            }
        }

        if (ctx->ui_language == SESSION_UI_LANGUAGE_COUNT) {
            ctx->ui_language = previous_language;
        }

        if (base_snapshot.has_user_theme) {
            const bool has_custom_color =
                base_snapshot.user_color_code[0] != '\0';
            const bool has_custom_highlight =
                base_snapshot.user_highlight_code[0] != '\0';

            const char *color_code = nullptr;
            if (has_custom_color) {
                color_code = base_snapshot.user_color_code;
            } else {
                color_code = lookup_color_code(USER_COLOR_MAP,
                                               sizeof(USER_COLOR_MAP) /
                                                   sizeof(USER_COLOR_MAP[0]),
                                               base_snapshot.user_color_name);
            }

            const char *highlight_code = nullptr;
            if (has_custom_highlight) {
                highlight_code = base_snapshot.user_highlight_code;
            } else {
                highlight_code =
                    lookup_color_code(HIGHLIGHT_COLOR_MAP,
                                      sizeof(HIGHLIGHT_COLOR_MAP) /
                                          sizeof(HIGHLIGHT_COLOR_MAP[0]),
                                      base_snapshot.user_highlight_name);
            }

            if (color_code != nullptr && highlight_code != nullptr) {
                snprintf(ctx->user_color_code, sizeof(ctx->user_color_code),
                         "%s", color_code);
                snprintf(ctx->user_highlight_code,
                         sizeof(ctx->user_highlight_code), "%s",
                         highlight_code);
                ctx->user_is_bold = base_snapshot.user_is_bold;
                snprintf(ctx->user_color_name, sizeof(ctx->user_color_name),
                         "%s", base_snapshot.user_color_name);
                snprintf(ctx->user_highlight_name,
                         sizeof(ctx->user_highlight_name), "%s",
                         base_snapshot.user_highlight_name);
                user_theme_applied = true;
            }
        }

        if (base_snapshot.has_system_theme) {
            const char *fg_code = lookup_color_code(
                USER_COLOR_MAP,
                sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
                base_snapshot.system_fg_name);
            const char *bg_code = lookup_color_code(
                HIGHLIGHT_COLOR_MAP,
                sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
                base_snapshot.system_bg_name);
            if (fg_code != nullptr && bg_code != nullptr) {
                const char *highlight_code = ctx->system_highlight_code;
                if (base_snapshot.system_highlight_name[0] != '\0') {
                    const char *candidate =
                        lookup_color_code(HIGHLIGHT_COLOR_MAP,
                                          sizeof(HIGHLIGHT_COLOR_MAP) /
                                              sizeof(HIGHLIGHT_COLOR_MAP[0]),
                                          base_snapshot.system_highlight_name);
                    if (candidate != nullptr) {
                        highlight_code = candidate;
                    }
                }

                ctx->system_fg_code = fg_code;
                ctx->system_bg_code = bg_code;
                ctx->system_highlight_code = highlight_code;
                ctx->system_is_bold = base_snapshot.system_is_bold;
                snprintf(ctx->system_fg_name, sizeof(ctx->system_fg_name), "%s",
                         base_snapshot.system_fg_name);
                snprintf(ctx->system_bg_name, sizeof(ctx->system_bg_name), "%s",
                         base_snapshot.system_bg_name);
                if (base_snapshot.system_highlight_name[0] != '\0') {
                    snprintf(ctx->system_highlight_name,
                             sizeof(ctx->system_highlight_name), "%s",
                             base_snapshot.system_highlight_name);
                }
            }
        }

        if (base_snapshot.os_name[0] != '\0') {
            snprintf(ctx->os_name, sizeof(ctx->os_name), "%s",
                     base_snapshot.os_name);
        }
        ctx->daily_year = base_snapshot.daily_year;
        ctx->daily_yday = base_snapshot.daily_yday;
        if (base_snapshot.daily_function[0] != '\0') {
            snprintf(ctx->daily_function, sizeof(ctx->daily_function), "%s",
                     base_snapshot.daily_function);
        }
        ctx->has_birthday = base_snapshot.has_birthday;
        if (ctx->has_birthday) {
            snprintf(ctx->birthday, sizeof(ctx->birthday), "%s",
                     base_snapshot.birthday);
        } else {
            ctx->birthday[0] = '\0';
        }

        ctx->translation_caption_spacing =
            base_snapshot.translation_caption_spacing;
        if (ctx->translation_caption_spacing > 8U) {
            ctx->translation_caption_spacing = 8U;
        }

        if (base_snapshot.translation_master_explicit) {
            ctx->translation_enabled = base_snapshot.translation_master_enabled;
        }

        ctx->output_translation_enabled =
            base_snapshot.output_translation_enabled;
        snprintf(ctx->output_translation_language,
                 sizeof(ctx->output_translation_language), "%s",
                 base_snapshot.output_translation_language);
        ctx->input_translation_enabled =
            base_snapshot.input_translation_enabled;
        snprintf(ctx->input_translation_language,
                 sizeof(ctx->input_translation_language), "%s",
                 base_snapshot.input_translation_language);
        ctx->breaking_alerts_enabled = base_snapshot.breaking_alerts_enabled;
        snprintf(ctx->game.chosen_camouflage_language,
                 sizeof(ctx->game.chosen_camouflage_language), "%s",
                 base_snapshot.camouflage_language);
    }

    if (!user_theme_applied && user_record != nullptr) {
        session_apply_user_data_theme(ctx, user_record);
    }

    if (has_ip_snapshot && ip_snapshot.ui_language[0] != '\0') {
        session_ui_language_t saved_language =
            session_ui_language_from_code(ip_snapshot.ui_language);
        if (saved_language != SESSION_UI_LANGUAGE_COUNT) {
            ctx->ui_language = saved_language;
        }
    }

    if (!has_base_snapshot && !has_ip_snapshot) {
        ctx->ui_language = previous_language;
    } else if (ctx->ui_language == SESSION_UI_LANGUAGE_COUNT) {
        ctx->ui_language = previous_language;
    }

    ctx->cp437_override = previous_cp437_override;
    ctx->prefer_cp437_output = previous_cp437_output;
    ctx->cp437_input_enabled = previous_cp437_input;

    session_refresh_output_encoding(ctx);

    if (!user_data_loaded) {
        (void)session_user_data_load(ctx);
    }
    session_force_dark_mode_foreground(ctx);
}

bool session_user_data_load(session_ctx_t *ctx)

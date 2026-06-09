    if (entry_value->raw_username[0] == '\0') {
        snprintf(entry_value->raw_username, sizeof(entry_value->raw_username),
                 "%s", entry_value->username);
    }

    return true;
}
static bool host_state_load_history_entries(FILE *fp, host_t *host,
                                            uint32_t version,
                                            uint32_t history_count)
{
    if (host == nullptr) {
        return false;
    }

    size_t cache_limit = SSH_CHATTER_HISTORY_CACHE_LIMIT;
    size_t keep_start = 0U;
    size_t keep_count = history_count;
    if (cache_limit > 0U && keep_count > cache_limit) {
        keep_start = keep_count - cache_limit;
        keep_count = cache_limit;
    }

    if (keep_count > 0U && !host_history_reserve_locked(host, keep_count)) {
        host->history_count = 0U;
        return false;
    }

    host->history_count = 0U;
    host->history_start_index = 0U;

    size_t total_kept = 0U;

    for (uint32_t idx = 0; idx < history_count; ++idx) {
        chat_history_entry_t entry_value = {0};
        if (!host_state_read_history_entry_from_stream(fp, version,
                                                       &entry_value)) {
            return false;
        }

        if (!host_history_normalize_entry(host, &entry_value)) {
            continue;
        }

        ++total_kept;

        if (cache_limit > 0U && host->history_count == cache_limit) {
            memmove(host->history, host->history + 1,
                    (cache_limit - 1U) * sizeof(host->history[0]));
            host->history[cache_limit - 1U] = entry_value;
            host->history_start_index += 1U;
            continue;
        }

        if ((uint32_t)idx >= keep_start) {
            size_t target_index = host->history_count;
            if (target_index < host->history_capacity) {
                host->history[target_index] = entry_value;
                ++host->history_count;
            }
        }
    }

    if (cache_limit > 0U) {
        size_t expected_start =
            (total_kept > host->history_count) ? (total_kept - host->history_count)
                                               : 0U;
        host->history_start_index = expected_start;
    }

    host->history_total = total_kept;
    return true;
}

static bool host_state_read_preference_entry(FILE *fp, uint32_t version,
                                             host_state_preference_entry_t *out)
{
    if (fp == nullptr || out == nullptr) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    if (version >= 14U) {
        if (fread(out, sizeof(*out), 1U, fp) != 1U) {
            return false;
        }
        return true;
    }

    if (version >= 12U) {
        const size_t legacy_prefix_size =
            offsetof(host_state_preference_entry_t, user_color_code);
        const size_t legacy_suffix_offset =
            offsetof(host_state_preference_entry_t, user_color_name);
        const size_t legacy_suffix_size = sizeof(*out) - legacy_suffix_offset;

        if (fread(out, legacy_prefix_size, 1U, fp) != 1U) {
            return false;
        }

        out->user_color_code[0] = '\0';
        out->user_highlight_code[0] = '\0';

        if (fread(((uint8_t *)out) + legacy_suffix_offset, legacy_suffix_size,
                  1U, fp) != 1U) {
            return false;
        }

        return true;
    }

    if (version >= 10U) {
        host_state_preference_entry_v9_t legacy9 = {0};
        if (fread(&legacy9, sizeof(legacy9), 1U, fp) != 1U) {
            return false;
        }
        memcpy(out, &legacy9, sizeof(legacy9));
        out->ip[0] = '\0';
        out->user_color_code[0] = '\0';
        out->user_highlight_code[0] = '\0';
        out->provider_label[0] = '\0';
        memset(out->reserved2, 0, sizeof(out->reserved2));
        return true;
    }

    if (version >= 9U) {
        host_state_preference_entry_v8_t legacy8 = {0};
        if (fread(&legacy8, sizeof(legacy8), 1U, fp) != 1U) {
            return false;
        }
        out->has_user_theme = legacy8.has_user_theme;
        out->has_system_theme = legacy8.has_system_theme;
        out->user_is_bold = legacy8.user_is_bold;
        out->system_is_bold = legacy8.system_is_bold;
        snprintf(out->username, sizeof(out->username), "%s", legacy8.username);
        snprintf(out->user_color_name, sizeof(out->user_color_name), "%s",
                 legacy8.user_color_name);
        snprintf(out->user_highlight_name, sizeof(out->user_highlight_name),
                 "%s", legacy8.user_highlight_name);
        snprintf(out->system_fg_name, sizeof(out->system_fg_name), "%s",
                 legacy8.system_fg_name);
        snprintf(out->system_bg_name, sizeof(out->system_bg_name), "%s",
                 legacy8.system_bg_name);
        snprintf(out->system_highlight_name, sizeof(out->system_highlight_name),
                 "%s", legacy8.system_highlight_name);
        snprintf(out->os_name, sizeof(out->os_name), "%s", legacy8.os_name);
        out->daily_year = legacy8.daily_year;
        out->daily_yday = legacy8.daily_yday;
        snprintf(out->daily_function, sizeof(out->daily_function), "%s",
                 legacy8.daily_function);
        out->last_poll_id = legacy8.last_poll_id;
        out->last_poll_choice = legacy8.last_poll_choice;
        out->has_birthday = legacy8.has_birthday;
        out->translation_caption_spacing = legacy8.translation_caption_spacing;
        out->translation_enabled = legacy8.translation_enabled;
        out->output_translation_enabled = legacy8.output_translation_enabled;
        out->input_translation_enabled = legacy8.input_translation_enabled;
        out->translation_master_explicit = legacy8.translation_master_explicit;
        snprintf(out->birthday, sizeof(out->birthday), "%s", legacy8.birthday);
        snprintf(out->output_translation_language,
                 sizeof(out->output_translation_language), "%s",
                 legacy8.output_translation_language);
        snprintf(out->input_translation_language,
                 sizeof(out->input_translation_language), "%s",
                 legacy8.input_translation_language);
        snprintf(out->ui_language, sizeof(out->ui_language), "%s",
                 legacy8.ui_language);
        out->breaking_alerts_enabled = 0U;
        memset(out->reserved2, 0, sizeof(out->reserved2));
        return true;
    }

    if (version >= 7U) {
        host_state_preference_entry_v7_t legacy7 = {0};
        if (fread(&legacy7, sizeof(legacy7), 1U, fp) != 1U) {
            return false;
        }
        out->has_user_theme = legacy7.has_user_theme;
        out->has_system_theme = legacy7.has_system_theme;
        out->user_is_bold = legacy7.user_is_bold;
        out->system_is_bold = legacy7.system_is_bold;
        snprintf(out->username, sizeof(out->username), "%s", legacy7.username);
        snprintf(out->user_color_name, sizeof(out->user_color_name), "%s",
                 legacy7.user_color_name);
        snprintf(out->user_highlight_name, sizeof(out->user_highlight_name),
                 "%s", legacy7.user_highlight_name);
        snprintf(out->system_fg_name, sizeof(out->system_fg_name), "%s",
                 legacy7.system_fg_name);
        snprintf(out->system_bg_name, sizeof(out->system_bg_name), "%s",
                 legacy7.system_bg_name);
        snprintf(out->system_highlight_name, sizeof(out->system_highlight_name),
                 "%s", legacy7.system_highlight_name);
        snprintf(out->os_name, sizeof(out->os_name), "%s", legacy7.os_name);
        out->daily_year = legacy7.daily_year;
        out->daily_yday = legacy7.daily_yday;
        snprintf(out->daily_function, sizeof(out->daily_function), "%s",
                 legacy7.daily_function);
        out->last_poll_id = legacy7.last_poll_id;
        out->last_poll_choice = legacy7.last_poll_choice;
        out->has_birthday = legacy7.has_birthday;
        out->translation_caption_spacing = legacy7.translation_caption_spacing;
        out->translation_enabled = legacy7.translation_enabled;
        out->output_translation_enabled = legacy7.output_translation_enabled;
        out->input_translation_enabled = legacy7.input_translation_enabled;
        out->translation_master_explicit = legacy7.translation_master_explicit;
        snprintf(out->birthday, sizeof(out->birthday), "%s", legacy7.birthday);
        snprintf(out->output_translation_language,
                 sizeof(out->output_translation_language), "%s",
                 legacy7.output_translation_language);
        snprintf(out->input_translation_language,
                 sizeof(out->input_translation_language), "%s",
                 legacy7.input_translation_language);
        out->ui_language[0] = '\0';
        out->breaking_alerts_enabled = 0U;
        memset(out->reserved2, 0, sizeof(out->reserved2));
        return true;
    }

    if (version == 6U) {
        host_state_preference_entry_v6_t legacy6 = {0};
        if (fread(&legacy6, sizeof(legacy6), 1U, fp) != 1U) {
            return false;
        }
        out->has_user_theme = legacy6.has_user_theme;
        out->has_system_theme = legacy6.has_system_theme;
        out->user_is_bold = legacy6.user_is_bold;
        out->system_is_bold = legacy6.system_is_bold;
        snprintf(out->username, sizeof(out->username), "%s", legacy6.username);
        snprintf(out->user_color_name, sizeof(out->user_color_name), "%s",
                 legacy6.user_color_name);
        snprintf(out->user_highlight_name, sizeof(out->user_highlight_name),
                 "%s", legacy6.user_highlight_name);
        snprintf(out->system_fg_name, sizeof(out->system_fg_name), "%s",
                 legacy6.system_fg_name);
        snprintf(out->system_bg_name, sizeof(out->system_bg_name), "%s",
                 legacy6.system_bg_name);
        snprintf(out->system_highlight_name, sizeof(out->system_highlight_name),
                 "%s", legacy6.system_highlight_name);
        snprintf(out->os_name, sizeof(out->os_name), "%s", legacy6.os_name);
        out->daily_year = legacy6.daily_year;
        out->daily_yday = legacy6.daily_yday;
        snprintf(out->daily_function, sizeof(out->daily_function), "%s",
                 legacy6.daily_function);
        out->last_poll_id = legacy6.last_poll_id;
        out->last_poll_choice = legacy6.last_poll_choice;
        out->has_birthday = legacy6.has_birthday;
        out->translation_caption_spacing = legacy6.translation_caption_spacing;
        out->translation_enabled = legacy6.translation_enabled;
        out->output_translation_enabled = legacy6.output_translation_enabled;
        out->input_translation_enabled = legacy6.input_translation_enabled;
        out->translation_master_explicit = legacy6.translation_enabled;
        snprintf(out->birthday, sizeof(out->birthday), "%s", legacy6.birthday);
        snprintf(out->output_translation_language,
                 sizeof(out->output_translation_language), "%s",
                 legacy6.output_translation_language);
        snprintf(out->input_translation_language,
                 sizeof(out->input_translation_language), "%s",
                 legacy6.input_translation_language);
        out->ui_language[0] = '\0';
        out->breaking_alerts_enabled = 0U;
        memset(out->reserved2, 0, sizeof(out->reserved2));
        return true;
    }

    if (version == 5U) {
        host_state_preference_entry_v5_t legacy5 = {0};
        if (fread(&legacy5, sizeof(legacy5), 1U, fp) != 1U) {
            return false;
        }
        out->has_user_theme = legacy5.has_user_theme;
        out->has_system_theme = legacy5.has_system_theme;
        out->user_is_bold = legacy5.user_is_bold;
        out->system_is_bold = legacy5.system_is_bold;
        snprintf(out->username, sizeof(out->username), "%s", legacy5.username);
        snprintf(out->user_color_name, sizeof(out->user_color_name), "%s",
                 legacy5.user_color_name);
        snprintf(out->user_highlight_name, sizeof(out->user_highlight_name),
                 "%s", legacy5.user_highlight_name);
        snprintf(out->system_fg_name, sizeof(out->system_fg_name), "%s",
                 legacy5.system_fg_name);
        snprintf(out->system_bg_name, sizeof(out->system_bg_name), "%s",
                 legacy5.system_bg_name);
        snprintf(out->system_highlight_name, sizeof(out->system_highlight_name),
                 "%s", legacy5.system_highlight_name);
        snprintf(out->os_name, sizeof(out->os_name), "%s", legacy5.os_name);
        out->daily_year = legacy5.daily_year;
        out->daily_yday = legacy5.daily_yday;
        snprintf(out->daily_function, sizeof(out->daily_function), "%s",
                 legacy5.daily_function);
        out->last_poll_id = legacy5.last_poll_id;
        out->last_poll_choice = legacy5.last_poll_choice;
        out->has_birthday = legacy5.has_birthday;
        out->translation_caption_spacing = legacy5.reserved[0];
        out->translation_enabled = 0U;
        out->output_translation_enabled = 0U;
        out->input_translation_enabled = 0U;
        out->translation_master_explicit = 0U;
        snprintf(out->birthday, sizeof(out->birthday), "%s", legacy5.birthday);
        out->output_translation_language[0] = '\0';
        out->input_translation_language[0] = '\0';
        out->ui_language[0] = '\0';
        out->breaking_alerts_enabled = 0U;
        memset(out->reserved2, 0, sizeof(out->reserved2));
        return true;
    }

    if (version == 4U) {
        host_state_preference_entry_v4_t legacy4 = {0};
        if (fread(&legacy4, sizeof(legacy4), 1U, fp) != 1U) {
            return false;
        }
        out->has_user_theme = legacy4.has_user_theme;
        out->has_system_theme = legacy4.has_system_theme;
        out->user_is_bold = legacy4.user_is_bold;
        out->system_is_bold = legacy4.system_is_bold;
        snprintf(out->username, sizeof(out->username), "%s", legacy4.username);
        snprintf(out->user_color_name, sizeof(out->user_color_name), "%s",
                 legacy4.user_color_name);
        snprintf(out->user_highlight_name, sizeof(out->user_highlight_name),
                 "%s", legacy4.user_highlight_name);
        snprintf(out->system_fg_name, sizeof(out->system_fg_name), "%s",
                 legacy4.system_fg_name);
        snprintf(out->system_bg_name, sizeof(out->system_bg_name), "%s",
                 legacy4.system_bg_name);
        snprintf(out->system_highlight_name, sizeof(out->system_highlight_name),
                 "%s", legacy4.system_highlight_name);
        snprintf(out->os_name, sizeof(out->os_name), "%s", legacy4.os_name);
        out->daily_year = legacy4.daily_year;
        out->daily_yday = legacy4.daily_yday;
        snprintf(out->daily_function, sizeof(out->daily_function), "%s",
                 legacy4.daily_function);
        out->last_poll_id = legacy4.last_poll_id;
        out->last_poll_choice = legacy4.last_poll_choice;
        out->has_birthday = 0U;
        out->translation_caption_spacing = 0U;
        out->translation_enabled = 0U;
        out->output_translation_enabled = 0U;
        out->input_translation_enabled = 0U;
        out->translation_master_explicit = 0U;
        out->birthday[0] = '\0';
        out->output_translation_language[0] = '\0';
        out->input_translation_language[0] = '\0';
        out->ui_language[0] = '\0';
        out->breaking_alerts_enabled = 0U;
        memset(out->reserved2, 0, sizeof(out->reserved2));
        return true;
    }

    host_state_preference_entry_v3_t legacy = {0};
    if (fread(&legacy, sizeof(legacy), 1U, fp) != 1U) {
        return false;
    }
    out->has_user_theme = legacy.has_user_theme;
    out->has_system_theme = legacy.has_system_theme;
    out->user_is_bold = legacy.user_is_bold;
    out->system_is_bold = legacy.system_is_bold;
    snprintf(out->username, sizeof(out->username), "%s", legacy.username);
    snprintf(out->user_color_name, sizeof(out->user_color_name), "%s",
             legacy.user_color_name);
    snprintf(out->user_highlight_name, sizeof(out->user_highlight_name), "%s",
             legacy.user_highlight_name);
    snprintf(out->system_fg_name, sizeof(out->system_fg_name), "%s",
             legacy.system_fg_name);
    snprintf(out->system_bg_name, sizeof(out->system_bg_name), "%s",
             legacy.system_bg_name);
    snprintf(out->system_highlight_name, sizeof(out->system_highlight_name),
             "%s", legacy.system_highlight_name);
    out->os_name[0] = '\0';
    out->daily_year = 0;
    out->daily_yday = 0;
    out->daily_function[0] = '\0';
    out->last_poll_id = 0U;
    out->last_poll_choice = -1;
    out->has_birthday = 0U;
    out->translation_caption_spacing = 0U;
    out->translation_enabled = 0U;
    out->output_translation_enabled = 0U;
    out->input_translation_enabled = 0U;
    out->translation_master_explicit = 0U;
    out->birthday[0] = '\0';
    out->output_translation_language[0] = '\0';
    out->input_translation_language[0] = '\0';
    out->ui_language[0] = '\0';
    out->breaking_alerts_enabled = 0U;
    memset(out->reserved2, 0, sizeof(out->reserved2));
    return true;
}

static void host_state_apply_preference_entry(
    host_t *host, const host_state_preference_entry_t *serialized)
{
    if (host == nullptr || serialized == nullptr) {
        return;
    }

    if (host->preference_count >= SSH_CHATTER_MAX_PREFERENCES) {
        return;
    }

    user_preference_t *pref = &host->preferences[host->preference_count];
    memset(pref, 0, sizeof(*pref));
    pref->in_use = true;
    pref->has_user_theme = serialized->has_user_theme != 0U;
    pref->has_system_theme = serialized->has_system_theme != 0U;
    pref->user_is_bold = serialized->user_is_bold != 0U;
    pref->system_is_bold = serialized->system_is_bold != 0U;
    snprintf(pref->username, sizeof(pref->username), "%s",
             serialized->username);
    snprintf(pref->ip, sizeof(pref->ip), "%s", serialized->ip);
    snprintf(pref->user_color_code, sizeof(pref->user_color_code), "%s",
             serialized->user_color_code);
    snprintf(pref->user_highlight_code, sizeof(pref->user_highlight_code), "%s",
             serialized->user_highlight_code);
    snprintf(pref->user_color_name, sizeof(pref->user_color_name), "%s",
             serialized->user_color_name);
    snprintf(pref->user_highlight_name, sizeof(pref->user_highlight_name), "%s",
             serialized->user_highlight_name);
    snprintf(pref->system_fg_name, sizeof(pref->system_fg_name), "%s",
             serialized->system_fg_name);
    snprintf(pref->system_bg_name, sizeof(pref->system_bg_name), "%s",
             serialized->system_bg_name);
    snprintf(pref->system_highlight_name, sizeof(pref->system_highlight_name),
             "%s", serialized->system_highlight_name);
    snprintf(pref->os_name, sizeof(pref->os_name), "%s", serialized->os_name);
    pref->daily_year = serialized->daily_year;
    pref->daily_yday = serialized->daily_yday;
    snprintf(pref->daily_function, sizeof(pref->daily_function), "%s",
             serialized->daily_function);
    pref->last_poll_id = serialized->last_poll_id;
    pref->last_poll_choice = serialized->last_poll_choice;
    pref->has_birthday = serialized->has_birthday != 0U;
    snprintf(pref->birthday, sizeof(pref->birthday), "%s",
             serialized->birthday);
    pref->translation_caption_spacing = serialized->translation_caption_spacing;
    pref->translation_master_enabled = serialized->translation_enabled != 0U;
    pref->translation_master_explicit =
        serialized->translation_master_explicit != 0U;
    pref->output_translation_enabled =
        serialized->output_translation_enabled != 0U;
    pref->input_translation_enabled =
        serialized->input_translation_enabled != 0U;
    snprintf(pref->output_translation_language,
             sizeof(pref->output_translation_language), "%s",
             serialized->output_translation_language);
    snprintf(pref->input_translation_language,
             sizeof(pref->input_translation_language), "%s",
             serialized->input_translation_language);
    pref->breaking_alerts_enabled = serialized->breaking_alerts_enabled != 0U;
    snprintf(pref->provider_label, sizeof(pref->provider_label), "%s",
             serialized->provider_label);
    ++host->preference_count;
}

static bool host_state_load_preferences(FILE *fp, host_t *host,
                                        uint32_t version,
                                        uint32_t preference_count)
{
    if (host == nullptr) {
        return false;
    }

    host_preferences_ensure(host);
    memset(host->preferences, 0,
           SSH_CHATTER_MAX_PREFERENCES * sizeof(user_preference_t));
    host->preference_count = 0U;

    for (uint32_t idx = 0; idx < preference_count; ++idx) {
        host_state_preference_entry_t serialized = {0};
        if (!host_state_read_preference_entry(fp, version, &serialized)) {
            return false;
        }

        host_state_apply_preference_entry(host, &serialized);
    }

    return true;
}

static bool host_state_load_grants(FILE *fp, host_t *host, uint32_t grant_count)
{
    if (fp == nullptr || host == nullptr) {
        return false;
    }

    memset(host->operator_grants, 0, HOST_GRANTS_CLEAR_SIZE);
    host->operator_grant_count = 0U;

    for (uint32_t idx = 0; idx < grant_count; ++idx) {
        host_state_grant_entry_t serialized = {0};
        if (fread(&serialized, sizeof(serialized), 1U, fp) != 1U) {
            return false;
        }
        if (serialized.ip[0] == '\0') {
            continue;
        }
        if (host->operator_grant_count >= SSH_CHATTER_MAX_GRANTS) {
            continue;
        }
        snprintf(host->operator_grants[host->operator_grant_count].ip,
                 sizeof(host->operator_grants[host->operator_grant_count].ip),
                 "%s", serialized.ip);
        ++host->operator_grant_count;
    }

    return true;
}

static void host_state_reset_loaded_data(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (host->history != nullptr && host->history_capacity > 0U) {
        memset(host->history, 0,
               host->history_capacity * sizeof(chat_history_entry_t));
    }
    host->history_count = 0U;
    host->preference_count = 0U;
    host_preferences_ensure(host);
    memset(host->preferences, 0,
           SSH_CHATTER_MAX_PREFERENCES * sizeof(user_preference_t));
}

static uint64_t host_state_normalize_next_message_id(uint64_t requested,
                                                     size_t history_total)
{
    uint64_t minimum_allowed = (uint64_t)history_total + 1U;
    if (requested < minimum_allowed) {
        return minimum_allowed;
    }
    return requested;
}

static void host_state_load(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (host->state_file_path[0] == '\0') {
        return;
    }

    FILE *fp = fopen(host->state_file_path, "rb");
    if (fp == nullptr) {
        return;
    }

    host_state_header_v1_t base_header = {0};
    if (!host_state_read_base_header(fp, &base_header)) {
        fclose(fp);
        return;
    }

    uint32_t version = base_header.version;
    uint32_t history_count = base_header.history_count;
    uint32_t preference_count = base_header.preference_count;
    uint64_t next_message_id = 1U;
    uint32_t grant_count = 0U;
    uint8_t captcha_enabled_raw = 0U;
    uint8_t geo_language_enabled_raw = 0U;

    if (!host_state_read_metadata(fp, version, &next_message_id, &grant_count,
                                  &captcha_enabled_raw,
                                  &geo_language_enabled_raw)) {
        fclose(fp);
        return;
    }

    if (preference_count > SSH_CHATTER_MAX_PREFERENCES) {
        preference_count = SSH_CHATTER_MAX_PREFERENCES;
    }

    ttak_mutex_lock(&host->lock);

    if (version >= 8U) {
        atomic_store(&host->captcha_enabled, captcha_enabled_raw != 0U);
    }

    if (version >= 13U) {
        atomic_store(&host->geo_language_enabled,
                     geo_language_enabled_raw != 0U);
    }

    bool success =
        host_state_load_history_entries(fp, host, version, history_count);

    if (success) {
        success =
            host_state_load_preferences(fp, host, version, preference_count);
    }

    if (success) {
        success = host_state_load_grants(fp, host, grant_count);
    }

    if (!success) {
        host_state_reset_loaded_data(host);
    }

    host->next_message_id = host_state_normalize_next_message_id(
        next_message_id == 0U ? (uint64_t)host->history_total + 1U
                              : next_message_id,
        host->history_total);

    ttak_mutex_unlock(&host->lock);
    fclose(fp);

    // Clean up messages older than 3 days after loading state
    host_history_cleanup_expired(host);
}

static void host_ui_language_state_save_locked(host_t *host)
{
    if (host == nullptr || host->ui_lang_state_file_path[0] == '\0') {
        return;
    }

    char temp_path[PATH_MAX];
    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp",
                           host->ui_lang_state_file_path);
    if (written < 0 || (size_t)written >= sizeof(temp_path)) {
        humanized_log_error("host", "ui-lang state file path is too long",
                            ENAMETOOLONG);
        return;
    }

    if (!host_ensure_private_data_path(host, host->ui_lang_state_file_path,
                                       true)) {
        return;
    }

    FILE *fp = fopen(temp_path, "wb");
    if (fp == nullptr) {
        humanized_log_error("host", "failed to open ui-lang state file", errno);
        return;
    }

    size_t entry_count = 0U;
    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_PREFERENCES; ++idx) {
        const user_preference_t *pref = &host->preferences[idx];
        if (pref->in_use && pref->ui_language[0] != '\0') {
            ++entry_count;
        }
    }

    ui_lang_state_header_t header = {0};
    header.magic = UI_LANG_STATE_MAGIC;
    header.version = UI_LANG_STATE_VERSION;
    header.entry_count = (uint32_t)entry_count;

    bool success = fwrite(&header, sizeof(header), 1U, fp) == 1U;

    for (size_t idx = 0U; success && idx < SSH_CHATTER_MAX_PREFERENCES; ++idx) {
        const user_preference_t *pref = &host->preferences[idx];
        if (!pref->in_use || pref->ui_language[0] == '\0') {
            continue;
        }

        ui_lang_state_entry_t entry = {0};
        snprintf(entry.username, sizeof(entry.username), "%s", pref->username);
        snprintf(entry.ip, sizeof(entry.ip), "%s", pref->ip);
        snprintf(entry.ui_language, sizeof(entry.ui_language), "%s",
                 pref->ui_language);

        if (fwrite(&entry, sizeof(entry), 1U, fp) != 1U) {
            success = false;
            break;
        }
    }

    if (success && fflush(fp) != 0) {
        success = false;
    }

    if (success) {
        int fd = fileno(fp);
        if (fd >= 0 && fsync(fd) != 0) {
            success = false;
        }
    }

    fclose(fp);

    if (!success) {
        unlink(temp_path);
        return;
    }

    if (rename(temp_path, host->ui_lang_state_file_path) != 0) {
        humanized_log_error("host", "failed to commit ui-lang state", errno);
        unlink(temp_path);
        return;
    }

    if (chmod(host->ui_lang_state_file_path, S_IRUSR | S_IWUSR) != 0) {
        humanized_log_error("host", "failed to secure ui-lang state file",
                            errno);
    }
}

static void host_ui_language_state_load(host_t *host)
{
    if (host == nullptr || host->ui_lang_state_file_path[0] == '\0') {
        return;
    }

    if (!host_ensure_private_data_path(host, host->ui_lang_state_file_path,
                                       false)) {
        return;
    }

    FILE *fp = fopen(host->ui_lang_state_file_path, "rb");
    if (fp == nullptr) {
        return;
    }

    ui_lang_state_header_t header = {0};
    if (fread(&header, sizeof(header), 1U, fp) != 1U) {
        fclose(fp);
        return;
    }

    if (header.magic != UI_LANG_STATE_MAGIC || header.version == 0U ||
        header.version > UI_LANG_STATE_VERSION) {
        fclose(fp);
        return;
    }

    uint32_t entry_count = header.entry_count;
    if (entry_count > SSH_CHATTER_MAX_PREFERENCES) {
        entry_count = SSH_CHATTER_MAX_PREFERENCES;
    }

    ttak_mutex_lock(&host->lock);
    for (uint32_t idx = 0U; idx < entry_count; ++idx) {
        ui_lang_state_entry_t entry = {0};
        if (fread(&entry, sizeof(entry), 1U, fp) != 1U) {
            break;
        }

        entry.username[sizeof(entry.username) - 1U] = '\0';
        entry.ip[sizeof(entry.ip) - 1U] = '\0';
        entry.ui_language[sizeof(entry.ui_language) - 1U] = '\0';

        if (entry.username[0] == '\0' || entry.ui_language[0] == '\0') {
            continue;
        }

        user_preference_t *pref =
            host_ensure_preference_locked(host, entry.username, entry.ip);
        if (pref == nullptr) {
            continue;
        }

        if (entry.ip[0] != '\0' && pref->ip[0] == '\0') {
            snprintf(pref->ip, sizeof(pref->ip), "%s", entry.ip);
        }

        if (pref->ui_language[0] == '\0') {
            snprintf(pref->ui_language, sizeof(pref->ui_language), "%s",
                     entry.ui_language);
        }
    }
    ttak_mutex_unlock(&host->lock);

    fclose(fp);
}

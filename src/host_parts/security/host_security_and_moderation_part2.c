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

    memset(host->preferences, 0, sizeof(host->preferences));
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
    memset(host->preferences, 0, sizeof(host->preferences));
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

static const uint32_t RSS_STATE_MAGIC = 0x52535331U; /* 'RSS1' */
static const uint32_t RSS_STATE_VERSION = 2U;

typedef struct rss_state_header {
    uint32_t magic;
    uint32_t version;
    uint32_t feed_count;
    uint32_t reserved;
} rss_state_header_t;

typedef struct rss_state_entry_legacy_v1 {
    char tag[SSH_CHATTER_RSS_TAG_LEN];
    char url[SSH_CHATTER_RSS_URL_LEN];
    char last_item_key[SSH_CHATTER_RSS_ITEM_KEY_LEN];
} rss_state_entry_legacy_v1_t;

typedef struct rss_state_entry {
    char tag[SSH_CHATTER_RSS_TAG_LEN];
    char url[SSH_CHATTER_RSS_URL_LEN];
    char last_item_key[SSH_CHATTER_RSS_ITEM_KEY_LEN];
    uint8_t window_id;
    uint8_t reserved[3];
    uint32_t item_count;
    rss_session_item_t items[SSH_CHATTER_RSS_MAX_ITEMS];
} rss_state_entry_t;

static uint8_t host_rss_current_window_id(void)
{
    time_t now = time(nullptr);
    if (now == (time_t)-1) {
        return 0U;
    }

    time_t adjusted = now - (time_t)(3 * 60 * 60);
    struct tm utc_snapshot;
    if (gmtime_r(&adjusted, &utc_snapshot) == nullptr) {
        return 0U;
    }

    switch (utc_snapshot.tm_wday) {
    case 1: /* Monday */
    case 2: /* Tuesday */
        return 0U;
    case 3: /* Wednesday */
    case 4: /* Thursday */
        return 1U;
    default: /* Friday, Saturday, Sunday */
        return 2U;
    }
}

static void host_rss_reset_feed_window_locked(rss_feed_t *feed,
                                              uint8_t window_id)
{
    if (feed == nullptr) {
        return;
    }
    feed->window_id = window_id;
    feed->stored_item_count = 0U;
    memset(feed->stored_items, 0, sizeof(feed->stored_items));
}

static bool host_rss_handle_window_rollover_locked(host_t *host,
                                                   uint8_t new_window_id)
{
    if (host == nullptr) {
        return false;
    }

    if (host->rss_current_window_id == new_window_id) {
        return false;
    }

    bool cleared = false;
    for (size_t idx = 0U; idx < SSH_CHATTER_RSS_MAX_FEEDS; ++idx) {
        rss_feed_t *entry = &host->rss_feeds[idx];
        if (!entry->in_use) {
            entry->window_id = new_window_id;
            entry->stored_item_count = 0U;
            continue;
        }

        bool had_items = entry->stored_item_count > 0U;
        if (had_items || entry->window_id != new_window_id) {
            host_rss_reset_feed_window_locked(entry, new_window_id);
            if (had_items) {
                cleared = true;
            }
        } else {
            entry->window_id = new_window_id;
        }
    }

    host->rss_current_window_id = new_window_id;
    return cleared;
}

static bool host_rss_store_item_locked(rss_feed_t *entry,
                                       const rss_session_item_t *item)
{
    if (entry == nullptr || item == nullptr) {
        return false;
    }

    const char *candidate = item->id;
    if (candidate[0] == '\0') {
        candidate = item->link;
    }
    if (candidate[0] == '\0') {
        candidate = item->title;
    }

    if (candidate[0] != '\0') {
        for (size_t idx = 0U; idx < entry->stored_item_count; ++idx) {
            const rss_session_item_t *existing = &entry->stored_items[idx];
            const char *existing_key = existing->id;
            if (existing_key[0] == '\0') {
                existing_key = existing->link;
            }
            if (existing_key[0] == '\0') {
                existing_key = existing->title;
            }
            if (existing_key[0] != '\0' &&
                strcmp(existing_key, candidate) == 0) {
                return false;
            }
        }
    }

    size_t preserved =
        entry->stored_item_count >= SSH_CHATTER_RSS_MAX_ITEMS
            ? SSH_CHATTER_RSS_MAX_ITEMS - 1U
            : entry->stored_item_count;
    if (preserved > 0U) {
        memmove(&entry->stored_items[1], &entry->stored_items[0],
                preserved * sizeof(entry->stored_items[0]));
    }
    entry->stored_items[0] = *item;
    if (entry->stored_item_count < SSH_CHATTER_RSS_MAX_ITEMS) {
        ++entry->stored_item_count;
    }
    return true;
}

static void host_clear_rss_feed(rss_feed_t *feed)
{
    if (feed == nullptr) {
        return;
    }

    memset(feed, 0, sizeof(*feed));
}

static void host_rss_recount_locked(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    size_t count = 0U;
    for (size_t idx = 0U; idx < SSH_CHATTER_RSS_MAX_FEEDS; ++idx) {
        if (host->rss_feeds[idx].in_use) {
            ++count;
        }
    }
    host->rss_feed_count = count;
}

static rss_feed_t *host_find_rss_feed_locked(host_t *host, const char *tag)
{
    if (host == nullptr || tag == nullptr || tag[0] == '\0') {
        return nullptr;
    }

    for (size_t idx = 0U; idx < SSH_CHATTER_RSS_MAX_FEEDS; ++idx) {
        rss_feed_t *entry = &host->rss_feeds[idx];
        if (!entry->in_use) {
            continue;
        }
        if (strcasecmp(entry->tag, tag) == 0) {
            return entry;
        }
    }
    return nullptr;
}

static bool rss_tag_is_valid(const char *tag)
{
    if (tag == nullptr || tag[0] == '\0') {
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

static void host_rss_state_save_locked(host_t *host);
static uint8_t host_rss_current_window_id(void);
static void host_rss_reset_feed_window_locked(rss_feed_t *feed,
                                              uint8_t window_id);
static bool host_rss_handle_window_rollover_locked(host_t *host,
                                                   uint8_t new_window_id);
static bool host_rss_store_item_locked(rss_feed_t *entry,
                                       const rss_session_item_t *item);
static rss_feed_t *host_find_rss_feed_locked(host_t *host, const char *tag);
static void host_clear_rss_feed(rss_feed_t *feed);
static void host_rss_recount_locked(host_t *host);
static bool host_rss_fetch_items(const rss_feed_t *feed,
                                 rss_session_item_t *items, size_t max_items,
                                 size_t *out_count);

static bool host_rss_add_feed(host_t *host, const char *url, const char *tag,
                              char *error, size_t error_length)
{
    if (error != nullptr && error_length > 0U) {
        error[0] = '\0';
    }

    if (host == nullptr || url == nullptr || url[0] == '\0' || tag == nullptr ||
        tag[0] == '\0') {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length, "Invalid RSS feed details.");
        }
        return false;
    }

    ttak_mutex_lock(&host->lock);

    bool success = false;

    if (!rss_tag_is_valid(tag)) {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length,
                     "Tag may only contain letters, numbers, '-', '_' or '.'.");
        }
        goto cleanup;
    }

    if (host->rss_feed_count >= SSH_CHATTER_RSS_MAX_FEEDS) {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length, "Maximum RSS feed capacity reached.");
        }
        goto cleanup;
    }

    if (host_find_rss_feed_locked(host, tag) != nullptr) {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length,
                     "Tag '%s' is already assigned to another feed.", tag);
        }
        goto cleanup;
    }

    for (size_t idx = 0U; idx < SSH_CHATTER_RSS_MAX_FEEDS; ++idx) {
        rss_feed_t *entry = &host->rss_feeds[idx];
        if (!entry->in_use) {
            continue;
        }
        if (strcasecmp(entry->url, url) == 0) {
            if (error != nullptr && error_length > 0U) {
                snprintf(error, error_length,
                         "Feed '%s' is already registered as '%s'.", url,
                         entry->tag);
            }
            goto cleanup;
        }
    }

    rss_feed_t *slot = nullptr;
    for (size_t idx = 0U; idx < SSH_CHATTER_RSS_MAX_FEEDS; ++idx) {
        if (!host->rss_feeds[idx].in_use) {
            slot = &host->rss_feeds[idx];
            break;
        }
    }

    if (slot == nullptr) {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length, "Unable to allocate RSS feed slot.");
        }
        goto cleanup;
    }

    host_clear_rss_feed(slot);
    slot->in_use = true;
    snprintf(slot->tag, sizeof(slot->tag), "%s", tag);
    snprintf(slot->url, sizeof(slot->url), "%s", url);
    slot->last_item_key[0] = '\0';
    slot->last_title[0] = '\0';
    slot->last_link[0] = '\0';
    slot->last_checked = 0;

    host_rss_recount_locked(host);
    host_rss_state_save_locked(host);
    success = true;

cleanup:
    ttak_mutex_unlock(&host->lock);
    return success;
}

static bool host_rss_rename_feed(host_t *host, const char *old_tag,
                                 const char *new_tag, char *error,
                                 size_t error_length)
{
    if (error != nullptr && error_length > 0U) {
        error[0] = '\0';
    }

    if (host == nullptr || old_tag == nullptr || old_tag[0] == '\0' ||
        new_tag == nullptr || new_tag[0] == '\0') {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length, "Invalid RSS feed tag.");
        }
        return false;
    }

    ttak_mutex_lock(&host->lock);

    bool success = false;

    if (!rss_tag_is_valid(old_tag) || !rss_tag_is_valid(new_tag)) {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length,
                     "Tag may only contain letters, numbers, '-', '_' or '.'.");
        }
        goto cleanup;
    }

    rss_feed_t *entry = host_find_rss_feed_locked(host, old_tag);
    if (entry == nullptr) {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length, "No RSS feed found for tag '%s'.",
                     old_tag);
        }
        goto cleanup;
    }

    if (strcasecmp(old_tag, new_tag) != 0 &&
        host_find_rss_feed_locked(host, new_tag) != nullptr) {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length,
                     "Tag '%s' is already assigned to another feed.", new_tag);
        }
        goto cleanup;
    }

    snprintf(entry->tag, sizeof(entry->tag), "%s", new_tag);
    host_rss_state_save_locked(host);
    success = true;

cleanup:
    ttak_mutex_unlock(&host->lock);
    return success;
}

static bool host_rss_remove_feed(host_t *host, const char *tag, char *error,
                                 size_t error_length)
{
    if (error != nullptr && error_length > 0U) {
        error[0] = '\0';
    }

    if (host == nullptr || tag == nullptr || tag[0] == '\0') {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length, "Invalid RSS feed tag.");
        }
        return false;
    }

    ttak_mutex_lock(&host->lock);

    bool success = false;

    if (!rss_tag_is_valid(tag)) {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length,
                     "Tag may only contain letters, numbers, '-', '_' or '.'.");
        }
        goto cleanup;
    }

    rss_feed_t *entry = host_find_rss_feed_locked(host, tag);
    if (entry == nullptr) {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length, "No RSS feed found for tag '%s'.",
                     tag);
        }
        goto cleanup;
    }

    host_clear_rss_feed(entry);
    host_rss_recount_locked(host);
    host_rss_state_save_locked(host);
    success = true;

cleanup:
    ttak_mutex_unlock(&host->lock);
    return success;
}

static void host_rss_resolve_path(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    const char *rss_path = getenv("CHATTER_RSS_FILE");
    if (rss_path == nullptr || rss_path[0] == '\0') {
        rss_path = "rss_state.dat";
    }

    int written = snprintf(host->rss_state_file_path,
                           sizeof(host->rss_state_file_path), "%s", rss_path);
    if (written < 0 || (size_t)written >= sizeof(host->rss_state_file_path)) {
        humanized_log_error("host", "rss state file path is too long",
                            ENAMETOOLONG);
        host->rss_state_file_path[0] = '\0';
    }
}

static void host_rss_state_save_locked(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (host->rss_state_file_path[0] == '\0') {
        return;
    }

    if (!host_ensure_private_data_path(host, host->rss_state_file_path, true)) {
        return;
    }

    char temp_path[PATH_MAX];
    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp",
                           host->rss_state_file_path);
    if (written < 0 || (size_t)written >= sizeof(temp_path)) {
        humanized_log_error("host", "rss state file path is too long",
                            ENAMETOOLONG);
        return;
    }

    int temp_fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW,
                       S_IRUSR | S_IWUSR);
    if (temp_fd < 0) {
        humanized_log_error("host", "failed to open rss state file",
                            errno != 0 ? errno : EIO);
        return;
    }

    FILE *fp = fdopen(temp_fd, "wb");
    if (fp == nullptr) {
        int saved_errno = errno;
        close(temp_fd);
        unlink(temp_path);
        humanized_log_error("host", "failed to wrap rss state descriptor",
                            saved_errno != 0 ? saved_errno : EIO);
        return;
    }

    uint32_t feed_count = 0U;
    for (size_t idx = 0U; idx < SSH_CHATTER_RSS_MAX_FEEDS; ++idx) {
        if (host->rss_feeds[idx].in_use) {
            ++feed_count;
        }
    }

    rss_state_header_t header = {0};
    header.magic = RSS_STATE_MAGIC;
    header.version = RSS_STATE_VERSION;
    header.feed_count = feed_count;

    bool success = fwrite(&header, sizeof(header), 1U, fp) == 1U;

        for (size_t idx = 0U; success && idx < SSH_CHATTER_RSS_MAX_FEEDS; ++idx) {
            const rss_feed_t *entry = &host->rss_feeds[idx];
            if (!entry->in_use) {
                continue;
            }
    
            rss_state_entry_t *record =
                (rss_state_entry_t *)ttak_mem_alloc(sizeof(rss_state_entry_t),
                                                    __TTAK_UNSAFE_MEM_FOREVER__,
                                                    ttak_get_tick_count());
            if (record == nullptr) {
                success = false;
                break;
            }
            memset(record, 0, sizeof(rss_state_entry_t));
    
            snprintf(record->tag, sizeof(record->tag), "%s", entry->tag);
            snprintf(record->url, sizeof(record->url), "%s", entry->url);
            snprintf(record->last_item_key, sizeof(record->last_item_key), "%s",
                     entry->last_item_key);
            record->window_id = entry->window_id;
            record->item_count = (uint32_t)entry->stored_item_count;
    
            size_t items_to_copy = entry->stored_item_count;
            if (items_to_copy > SSH_CHATTER_RSS_MAX_ITEMS) {
                items_to_copy = SSH_CHATTER_RSS_MAX_ITEMS;
            }
    
            if (items_to_copy > 0U) {
                memcpy(record->items, entry->stored_items,
                       items_to_copy * sizeof(rss_session_item_t));
            }
    
            if (fwrite(record, sizeof(rss_state_entry_t), 1U, fp) != 1U) {
                success = false;
                ttak_mem_free(record);
                break;
            }
    
            ttak_mem_free(record);
        }

    if (success && fflush(fp) != 0) {
        success = false;
    }

    if (success) {
        int descriptor = fileno(fp);
        if (descriptor >= 0 && fsync(descriptor) != 0) {
            success = false;
        }
    }

    if (fclose(fp) != 0) {
        success = false;
    }

    if (!success) {
        humanized_log_error("host", "failed to write rss state file",
                            errno != 0 ? errno : EIO);
        unlink(temp_path);
        return;
    }

    if (chmod(temp_path, S_IRUSR | S_IWUSR) != 0) {
        humanized_log_error("host",
                            "failed to tighten temporary rss state permissions",
                            errno != 0 ? errno : EACCES);
        unlink(temp_path);
        return;
    }

    if (rename(temp_path, host->rss_state_file_path) != 0) {
        humanized_log_error("host", "failed to update rss state file",
                            errno != 0 ? errno : EIO);
        unlink(temp_path);
    } else if (chmod(host->rss_state_file_path, S_IRUSR | S_IWUSR) != 0) {
        humanized_log_error("host", "failed to tighten rss state permissions",
                            errno != 0 ? errno : EACCES);
    }
}

static void host_rss_state_load(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (host->rss_state_file_path[0] == '\0') {
        return;
    }

    if (!host_ensure_private_data_path(host, host->rss_state_file_path,
                                       false)) {
        return;
    }

    FILE *fp = fopen(host->rss_state_file_path, "rb");
    if (fp == nullptr) {
        return;
    }

    rss_state_header_t header = {0};
    if (fread(&header, sizeof(header), 1U, fp) != 1U) {
        fclose(fp);
        return;
    }

    if (header.magic != RSS_STATE_MAGIC || header.version == 0U ||
        header.version > RSS_STATE_VERSION) {
        fclose(fp);
        return;
    }

    ttak_mutex_lock(&host->lock);

    for (size_t idx = 0U; idx < SSH_CHATTER_RSS_MAX_FEEDS; ++idx) {
        host_clear_rss_feed(&host->rss_feeds[idx]);
    }
    host->rss_feed_count = 0U;

    bool success = true;
    for (uint32_t idx = 0U; idx < header.feed_count; ++idx) {
        rss_state_entry_t *record =
            (rss_state_entry_t *)ttak_mem_alloc(sizeof(rss_state_entry_t),
                                                __TTAK_UNSAFE_MEM_FOREVER__,
                                                ttak_get_tick_count());
        if (record == nullptr) {
            success = false;
            break;
        }
        memset(record, 0, sizeof(rss_state_entry_t));

        if (header.version == 1U) {
            rss_state_entry_legacy_v1_t v1_record = {0};
            if (fread(&v1_record, sizeof(v1_record), 1U, fp) != 1U) {
                success = false;
                ttak_mem_free(record);
                break;
            }
            snprintf(record->tag, sizeof(record->tag), "%s", v1_record.tag);
            snprintf(record->url, sizeof(record->url), "%s", v1_record.url);
            snprintf(record->last_item_key, sizeof(record->last_item_key), "%s",
                     v1_record.last_item_key);
            record->window_id = host_rss_current_window_id();
            record->item_count = 0U;
        } else {
            if (fread(record, sizeof(rss_state_entry_t), 1U, fp) != 1U) {
                success = false;
                ttak_mem_free(record);
                break;
            }
        }

        rss_trim_whitespace(record->tag);
        rss_trim_whitespace(record->url);
        rss_trim_whitespace(record->last_item_key);

        if (!rss_tag_is_valid(record->tag) || record->url[0] == '\0') {
            ttak_mem_free(record);
            continue;
        }

        rss_feed_t *slot = nullptr;
        for (size_t pos = 0U; pos < SSH_CHATTER_RSS_MAX_FEEDS; ++pos) {
            if (!host->rss_feeds[pos].in_use) {
                slot = &host->rss_feeds[pos];
                break;
            }
        }

        if (slot == nullptr) {
            ttak_mem_free(record);
            continue;
        }

        host_clear_rss_feed(slot);
        slot->in_use = true;
        snprintf(slot->tag, sizeof(slot->tag), "%s", record->tag);
        snprintf(slot->url, sizeof(slot->url), "%s", record->url);
        snprintf(slot->last_item_key, sizeof(slot->last_item_key), "%s",
                 record->last_item_key);
        slot->last_checked = 0;
        slot->window_id = record->window_id;
        slot->stored_item_count = (size_t)record->item_count;

        size_t items_to_copy = slot->stored_item_count;
        if (items_to_copy > SSH_CHATTER_RSS_MAX_ITEMS) {
            items_to_copy = SSH_CHATTER_RSS_MAX_ITEMS;
        }

        if (items_to_copy > 0U) {
            memcpy(slot->stored_items, record->items,
                   items_to_copy * sizeof(rss_session_item_t));
        }
        ttak_mem_free(record);
    }

    if (success) {
        host_rss_recount_locked(host);
    } else {
        for (size_t idx = 0U; idx < SSH_CHATTER_RSS_MAX_FEEDS; ++idx) {
            host_clear_rss_feed(&host->rss_feeds[idx]);
        }
        host->rss_feed_count = 0U;
    }

    ttak_mutex_unlock(&host->lock);
    fclose(fp);
}

typedef struct host_rss_buffer {
    char *data;
    size_t length;
} host_rss_buffer_t;

static size_t host_rss_write_callback(void *contents, size_t size, size_t nmemb,
                                      void *userp)
{
    host_rss_buffer_t *buffer = (host_rss_buffer_t *)userp;
    const size_t total = size * nmemb;
    if (buffer == nullptr || total == 0U) {
        return 0U;
    }

    char *resized = ttak_mem_realloc(buffer->data, buffer->length + total + 1U,
                                     __TTAK_UNSAFE_MEM_FOREVER__,
                                     ttak_get_tick_count());
    if (resized == nullptr) {
        return 0U;
    }

    buffer->data = resized;
    memcpy(buffer->data + buffer->length, contents, total);
    buffer->length += total;
    buffer->data[buffer->length] = '\0';
    return total;
}

static bool host_rss_download(const char *url, char **payload, size_t *length)
{
    if (payload != nullptr) {
        *payload = nullptr;
    }
    if (length != nullptr) {
        *length = 0U;
    }

    if (url == nullptr || url[0] == '\0') {
        return false;
    }

    bool success = false;
    for (unsigned int attempt = 0U;
         attempt < SSH_CHATTER_RSS_DOWNLOAD_ATTEMPTS && !success; ++attempt) {
        CURL *curl = curl_easy_init();
        if (curl == nullptr) {
            break;
        }

        host_rss_buffer_t buffer = {0};
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                         (long)SSH_CHATTER_RSS_TRANSFER_TIMEOUT_MS);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                         (long)SSH_CHATTER_RSS_CONNECT_TIMEOUT_MS);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, SSH_CHATTER_RSS_USER_AGENT);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, host_rss_write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

        CURLcode result = curl_easy_perform(curl);
        if (result == CURLE_OK) {
            long status = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            if (status >= 200L && status < 300L && buffer.data != nullptr) {
                if (payload != nullptr) {
                    *payload = buffer.data;
                }
                if (length != nullptr) {
                    *length = buffer.length;
                }
                buffer.data = nullptr;
                success = true;
            }
        }

        if (!success && buffer.data != nullptr) {
            ttak_mem_free(buffer.data);
        }

        curl_easy_cleanup(curl);
    }

    return success;
}

static bool host_rss_extract_tag(const char *block, const char *tag, char *out,
                                 size_t out_len)
{
    if (block == nullptr || tag == nullptr || out == nullptr || out_len == 0U) {
        return false;
    }

    char open_pattern[32];
    char close_pattern[32];
    int open_written = snprintf(open_pattern, sizeof(open_pattern), "<%s", tag);
    int close_written =
        snprintf(close_pattern, sizeof(close_pattern), "</%s>", tag);
    if (open_written < 0 || (size_t)open_written >= sizeof(open_pattern) ||
        close_written < 0 || (size_t)close_written >= sizeof(close_pattern)) {
        return false;
    }

    const char *start = strcasestr(block, open_pattern);
    if (start == nullptr) {
        return false;
    }

    const char *content = strchr(start, '>');
    if (content == nullptr) {
        return false;
    }
    ++content;

    const char *end = strcasestr(content, close_pattern);
    if (end == nullptr) {
        return false;
    }

    size_t length = (size_t)(end - content);
    if (length >= out_len) {
        length = out_len - 1U;
    }
    memcpy(out, content, length);
    out[length] = '\0';
    return true;
}

static bool host_rss_extract_atom_link(const char *block, char *out,
                                       size_t out_len)
{
    if (block == nullptr || out == nullptr || out_len == 0U) {
        return false;
    }

    const char *cursor = block;
    while ((cursor = strcasestr(cursor, "<link")) != nullptr) {
        const char *close = strchr(cursor, '>');
        if (close == nullptr) {
            return false;
        }

        const char *href = strcasestr(cursor, "href=");
        if (href == nullptr || href > close) {
            cursor = close + 1;
            continue;
        }

        href += 5; // skip href=
        char quote = *href;
        if (quote != '\"' && quote != '\'') {
            cursor = close + 1;
            continue;
        }
        ++href;

        const char *end = strchr(href, quote);
        if (end == nullptr || end > close) {
            cursor = close + 1;
            continue;
        }

        size_t length = (size_t)(end - href);
        if (length >= out_len) {
            length = out_len - 1U;
        }
        memcpy(out, href, length);
        out[length] = '\0';
        rss_trim_whitespace(out);
        return out[0] != '\0';
    }

    return false;
}

static size_t host_rss_parse_items(const char *payload,
                                   rss_session_item_t *items, size_t max_items)
{
    if (payload == nullptr || items == nullptr || max_items == 0U) {
        return 0U;
    }

    for (size_t idx = 0U; idx < max_items; ++idx) {
        memset(&items[idx], 0, sizeof(items[idx]));
    }

    size_t count = 0U;
    const char *cursor = payload;
    while (*cursor != '\0' && count < max_items) {
        const char *item_start = strcasestr(cursor, "<item");
        const char *entry_start = strcasestr(cursor, "<entry");
        const char *start = nullptr;
        const char *close_tag = nullptr;
        bool is_atom = false;

        if (item_start == nullptr && entry_start == nullptr) {
            break;
        }

        if (item_start != nullptr &&
            (entry_start == nullptr || item_start < entry_start)) {
            start = item_start;
            close_tag = "</item>";
        } else {
            start = entry_start;
            close_tag = "</entry>";
            is_atom = true;
        }

        const char *end = strcasestr(start, close_tag);
        if (end == nullptr) {
            break;
        }
        end += strlen(close_tag);

        size_t block_len = (size_t)(end - start);
        char *block = (char *)ttak_mem_alloc(block_len + 1U,
                                             __TTAK_UNSAFE_MEM_FOREVER__,
                                             ttak_get_tick_count());
        if (block == nullptr) {
            break;
        }
        memcpy(block, start, block_len);
        block[block_len] = '\0';

        char title[SSH_CHATTER_RSS_TITLE_LEN] = {0};
        char link[SSH_CHATTER_RSS_LINK_LEN] = {0};
        char summary[SSH_CHATTER_RSS_SUMMARY_LEN] = {0};
        char guid[SSH_CHATTER_RSS_ITEM_KEY_LEN] = {0};

        bool have_title =
            host_rss_extract_tag(block, "title", title, sizeof(title));
        bool have_link =
            host_rss_extract_tag(block, "link", link, sizeof(link));
        if (!have_link) {
            have_link = host_rss_extract_atom_link(block, link, sizeof(link));
        }
        bool have_guid = false;
        if (is_atom) {
            have_guid = host_rss_extract_tag(block, "id", guid, sizeof(guid));
        } else {
            have_guid = host_rss_extract_tag(block, "guid", guid, sizeof(guid));
        }
        bool have_summary = host_rss_extract_tag(block, "description", summary,
                                                 sizeof(summary));
        if (!have_summary) {
            have_summary = host_rss_extract_tag(block, "summary", summary,
                                                sizeof(summary));
        }
        if (!have_summary) {
            have_summary = host_rss_extract_tag(block, "content", summary,
                                                sizeof(summary));
        }

        rss_trim_whitespace(title);
        rss_trim_whitespace(link);
        rss_trim_whitespace(guid);
        rss_trim_whitespace(summary);
        rss_strip_html(summary);
        rss_decode_entities(title);
        rss_decode_entities(link);
        rss_decode_entities(guid);
        rss_decode_entities(summary);

        rss_session_item_t *item = &items[count];
        if (have_title) {
            snprintf(item->title, sizeof(item->title), "%s", title);
        }
        if (have_link) {
            snprintf(item->link, sizeof(item->link), "%s", link);
        }
        if (have_summary) {
            snprintf(item->summary, sizeof(item->summary), "%s", summary);
        }

        if (have_guid) {
            snprintf(item->id, sizeof(item->id), "%s", guid);
        } else if (have_link) {
            snprintf(item->id, sizeof(item->id), "%s", link);
        } else if (have_title) {
            snprintf(item->id, sizeof(item->id), "%s", title);
        }

        ++count;
        ttak_mem_free(block);
        cursor = end;
    }

    return count;
}

static bool host_rss_fetch_items(const rss_feed_t *feed,
                                 rss_session_item_t *items, size_t max_items,
                                 size_t *out_count)
{
    if (out_count != nullptr) {
        *out_count = 0U;
    }

    if (feed == nullptr || items == nullptr || max_items == 0U) {
        return false;
    }

    char *payload = nullptr;
    size_t length = 0U;
    if (!host_rss_download(feed->url, &payload, &length)) {
        return false;
    }

    size_t count = host_rss_parse_items(payload, items, max_items);
    if (out_count != nullptr) {
        *out_count = count;
    }

    if (payload != nullptr) {
        ttak_mem_free(payload);
    }

    return true;
}

static bool host_rss_should_broadcast_breaking(const rss_session_item_t *item)
{
    if (item == nullptr) {
        return false;
    }

    const char *fields[] = {item->title, item->summary, item->link};
    for (size_t field_index = 0U;
         field_index < sizeof(fields) / sizeof(fields[0]); ++field_index) {
        const char *field = fields[field_index];
        if (field == nullptr || field[0] == '\0') {
            continue;
        }

        if (strncasecmp(field, "[breaking", 9) == 0) {
            return true;
        }
        if (strcasestr(field, "breaking news") != nullptr ||
            strcasestr(field, "breaking:") != nullptr ||
            strcasestr(field, "breaking ") != nullptr) {
            return true;
        }
        if (strcasestr(field, "urgent") != nullptr ||
            strcasestr(field, "alert") != nullptr) {
            return true;
        }
        if (strstr(field, "속보") != nullptr ||
            strstr(field, "速報") != nullptr) {
            return true;
        }
    }

    return false;
}

static size_t host_rss_refresh_cycle(host_t *host, bool abort_on_stop)
{
    if (host == nullptr) {
        return 0U;
    }

    bool refresh_lock_held = false;
    if (host->rss_refresh_lock_initialized) {
        if (ttak_mutex_lock(&host->rss_refresh_lock) == 0) {
            refresh_lock_held = true;
        } else {
            printf("[rss] failed to acquire refresh lock\n");
        }
    }

    // Window check
    uint8_t current_window = host_rss_current_window_id();
    ttak_mutex_lock(&host->lock);
    if (host_rss_handle_window_rollover_locked(host, current_window)) {
        host_rss_state_save_locked(host);
    }

    rss_feed_t *feed_snapshots = (rss_feed_t *)ttak_mem_alloc(
        sizeof(rss_feed_t) * SSH_CHATTER_RSS_MAX_FEEDS,
        __TTAK_UNSAFE_MEM_FOREVER__, ttak_get_tick_count());
    size_t snapshot_count = 0U;

    if (feed_snapshots != nullptr) {
        memset(feed_snapshots, 0, sizeof(rss_feed_t) * SSH_CHATTER_RSS_MAX_FEEDS);
        for (size_t idx = 0U; idx < SSH_CHATTER_RSS_MAX_FEEDS; ++idx) {
            if (!host->rss_feeds[idx].in_use) {
                continue;
            }
            feed_snapshots[snapshot_count++] = host->rss_feeds[idx];
        }
    }
    ttak_mutex_unlock(&host->lock);

    if (snapshot_count > 0U && feed_snapshots != nullptr) {
        for (size_t snapshot_index = 0U; snapshot_index < snapshot_count;
             ++snapshot_index) {
            if (abort_on_stop && atomic_load(&host->rss_thread_stop)) {
                break;
            }
            const rss_feed_t *feed_snapshot = &feed_snapshots[snapshot_index];

            rss_session_item_t *items = (rss_session_item_t *)ttak_mem_alloc(
                sizeof(rss_session_item_t) * SSH_CHATTER_RSS_MAX_ITEMS,
                __TTAK_UNSAFE_MEM_FOREVER__, ttak_get_tick_count());
            if (items == nullptr) {
                continue;
            }
            memset(items, 0,
                   sizeof(rss_session_item_t) * SSH_CHATTER_RSS_MAX_ITEMS);

            size_t item_count = 0U;
            if (!host_rss_fetch_items(feed_snapshot, items,
                                      SSH_CHATTER_RSS_MAX_ITEMS, &item_count)) {
                printf("[rss] failed to refresh feed '%s' (%s)\n",
                       feed_snapshot->tag, feed_snapshot->url);
                ttak_mem_free(items);
                continue;
            }

            size_t new_item_count = 0U;
            if (item_count > 0U) {
                if (feed_snapshot->last_item_key[0] == '\0') {
                    new_item_count = item_count; // First time? Let's take them all.
                } else {
                    bool found_marker = false;
                    for (size_t idx = 0U; idx < item_count; ++idx) {
                        if (items[idx].id[0] == '\0' &&
                            feed_snapshot->last_item_key[0] == '\0') {
                            continue;
                        }
                        if (strcmp(items[idx].id,
                                   feed_snapshot->last_item_key) == 0) {
                            new_item_count = idx;
                            found_marker = true;
                            break;
                        }
                    }
                    if (!found_marker) {
                        new_item_count = item_count;
                    }
                }
            }

            bool feed_active = false;
            bool state_changed = false;
            time_t now = time(nullptr);
            size_t items_to_store = new_item_count;
            if (items_to_store > item_count) {
                items_to_store = item_count;
            }

            ttak_mutex_lock(&host->lock);
            rss_feed_t *entry =
                host_find_rss_feed_locked(host, feed_snapshot->tag);
            if (entry != nullptr && entry->in_use) {
                feed_active = true;
                entry->last_checked = now;

                // Store only the newly discovered items at the front
                for (size_t idx = items_to_store; idx > 0U; --idx) {
                    if (host_rss_store_item_locked(entry, &items[idx - 1U])) {
                        state_changed = true;
                    }
                }

                if (item_count > 0U) {
                    const rss_session_item_t *latest = &items[0U];
                    char new_key[SSH_CHATTER_RSS_ITEM_KEY_LEN];
                    new_key[0] = '\0';
                    if (latest->id[0] != '\0') {
                        snprintf(new_key, sizeof(new_key), "%s", latest->id);
                    } else if (latest->link[0] != '\0') {
                        snprintf(new_key, sizeof(new_key), "%s",
                                 latest->link);
                    } else if (latest->title[0] != '\0') {
                        snprintf(new_key, sizeof(new_key), "%s",
                                 latest->title);
                    }

                    if (new_key[0] != '\0' &&
                        strcmp(entry->last_item_key, new_key) != 0) {
                        snprintf(entry->last_item_key,
                                 sizeof(entry->last_item_key), "%s", new_key);
                        state_changed = true;
                    }

                    if (latest->title[0] != '\0') {
                        snprintf(entry->last_title, sizeof(entry->last_title),
                                 "%s", latest->title);
                    } else {
                        entry->last_title[0] = '\0';
                    }

                    if (latest->link[0] != '\0') {
                        snprintf(entry->last_link, sizeof(entry->last_link), "%s",
                                 latest->link);
                    } else {
                        entry->last_link[0] = '\0';
                    }
                }

                if (state_changed) {
                    host_rss_state_save_locked(host);
                }
            }
            ttak_mutex_unlock(&host->lock);

            if (!feed_active || new_item_count == 0U) {
                ttak_mem_free(items);
                continue;
            }

            for (size_t idx = new_item_count; idx > 0U; --idx) {
                if (abort_on_stop && atomic_load(&host->rss_thread_stop)) {
                    break;
                }
                const rss_session_item_t *item = &items[idx - 1U];
                if (!host_rss_should_broadcast_breaking(item)) {
                    continue;
                }

                char headline[SSH_CHATTER_RSS_TITLE_LEN];
                if (item->title[0] != '\0') {
                    snprintf(headline, sizeof(headline), "%s", item->title);
                } else if (item->summary[0] != '\0') {
                    snprintf(headline, sizeof(headline), "%s", item->summary);
                } else if (item->link[0] != '\0') {
                    snprintf(headline, sizeof(headline), "%s", item->link);
                } else {
                    snprintf(headline, sizeof(headline), "%s", "New update");
                }

                rss_trim_whitespace(headline);
                for (size_t pos = 0U; headline[pos] != '\0'; ++pos) {
                    if (headline[pos] == '\r' || headline[pos] == '\n' ||
                        headline[pos] == '\t') {
                        headline[pos] = ' ';
                    }
                }
                rss_trim_whitespace(headline);
                if (headline[0] == '\0') {
                    snprintf(headline, sizeof(headline), "%s", "New update");
                }

                char notice[SSH_CHATTER_MESSAGE_LIMIT];
                if (item->link[0] != '\0') {
                    char clean_link[SSH_CHATTER_RSS_LINK_LEN];
                    snprintf(clean_link, sizeof(clean_link), "%s", item->link);
                    rss_trim_whitespace(clean_link);
                    for (size_t pos = 0U; clean_link[pos] != '\0'; ++pos) {
                        if (clean_link[pos] == '\r' || clean_link[pos] == '\n' ||
                            clean_link[pos] == '\t') {
                            clean_link[pos] = ' ';
                        }
                    }
                    rss_trim_whitespace(clean_link);
                    snprintf(notice, sizeof(notice), "* %s [%s] %s - %s",
                             SSH_CHATTER_RSS_BREAKING_PREFIX,
                             feed_snapshot->tag, headline, clean_link);
                } else {
                    snprintf(notice, sizeof(notice), "* %s [%s] %s",
                             SSH_CHATTER_RSS_BREAKING_PREFIX,
                             feed_snapshot->tag, headline);
                }

                printf("%s\n", notice);
                // Iterate through all active sessions and send the notice only to those with breaking_alerts_enabled
                ttak_mutex_lock(&host->room.lock);
                for (size_t i = 0; i < host->room.member_count; ++i) {
                    session_ctx_t *member = host->room.members[i];
                    if (member != nullptr && member->breaking_alerts_enabled) {
                        session_send_system_line(member, notice);
                    }
                }
                ttak_mutex_unlock(&host->room.lock);
            }
            ttak_mem_free(items);
        }
    }

    if (feed_snapshots != nullptr) {
        ttak_mem_free(feed_snapshots);
    }

    struct timespec mark;
    if (clock_gettime(CLOCK_MONOTONIC, &mark) == 0) {
        host->rss_last_run = mark;
    } else {
        host->rss_last_run.tv_sec = time(nullptr);
        host->rss_last_run.tv_nsec = 0L;
    }

    if (refresh_lock_held) {
        ttak_mutex_unlock(&host->rss_refresh_lock);
    }

    return snapshot_count;
}

static bool host_rss_refresh_now(host_t *host)
{
    return host_rss_refresh_cycle(host, false) > 0U;
}

typedef struct host_rss_refresh_async_request {
    host_t *host;
} host_rss_refresh_async_request_t;

static void *host_rss_manual_refresh_worker(void *arg)
{
    host_rss_refresh_async_request_t *request =
        (host_rss_refresh_async_request_t *)arg;
    if (request == nullptr) {
        return nullptr;
    }

    host_t *host = request->host;
    sshc_gc_free(request);
    if (host == nullptr) {
        return nullptr;
    }

    sshc_epoch_thread_enter();
    host_rss_refresh_now(host);
    sshc_epoch_thread_exit();

    atomic_store(&host->rss_manual_refresh_running, false);
    return nullptr;
}

static bool host_rss_schedule_manual_refresh(host_t *host)
{
    if (host == nullptr) {
        return false;
    }

    bool expected = false;
    if (!atomic_compare_exchange_strong(&host->rss_manual_refresh_running,
                                        &expected, true)) {
        return false;
    }

    host_rss_refresh_async_request_t *request =
        (host_rss_refresh_async_request_t *)sshc_gc_calloc(
            1U, sizeof(*request));
    if (request == nullptr) {
        atomic_store(&host->rss_manual_refresh_running, false);
        return false;
    }
    request->host = host;

    pthread_t worker;
    int error =
        pthread_create(&worker, nullptr, host_rss_manual_refresh_worker,
                       request);
    if (error != 0) {
        printf("[rss] failed to start manual refresh worker: %s\n",
               strerror(error));
        sshc_gc_free(request);
        atomic_store(&host->rss_manual_refresh_running, false);
        return false;
    }

    pthread_detach(worker);
    return true;
}

static void *host_rss_backend(void *arg)
{
    host_t *host = (host_t *)arg;
    if (host == nullptr) {
        return nullptr;
    }

    atomic_store(&host->rss_thread_running, true);
    printf("[rss] backend thread started (interval: %u seconds)\n",
           (unsigned int)SSH_CHATTER_RSS_REFRESH_SECONDS);

    while (!atomic_load(&host->rss_thread_stop)) {
        size_t snapshot_count = host_rss_refresh_cycle(host, true);

        unsigned int remaining = snapshot_count > 0U
                                     ? SSH_CHATTER_RSS_REFRESH_SECONDS
                                     : SSH_CHATTER_RSS_SLEEP_CHUNK_SECONDS;
        while (remaining > 0U && !atomic_load(&host->rss_thread_stop)) {
            unsigned int chunk = remaining > SSH_CHATTER_RSS_SLEEP_CHUNK_SECONDS
                                     ? SSH_CHATTER_RSS_SLEEP_CHUNK_SECONDS
                                     : remaining;
            struct timespec pause = {
                .tv_sec = (time_t)chunk,
                .tv_nsec = 0L,
            };
            host_sleep_uninterruptible(&pause);
            if (remaining <= chunk) {
                remaining = 0U;
            } else {
                remaining -= chunk;
            }
        }
    }

    atomic_store(&host->rss_thread_running, false);
    printf("[rss] backend thread stopped\n");
    return nullptr;
}

static void host_rss_start_backend(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    ttak_mutex_lock(&host->lock);
    host->rss_current_window_id = host_rss_current_window_id();
    bool has_feeds = host->rss_feed_count > 0U;
    ttak_mutex_unlock(&host->lock);

    if (!has_feeds) {
        return;
    }

    if (host->rss_thread_initialized) {
        return;
    }

    atomic_store(&host->rss_thread_stop, false);
    atomic_store(&host->rss_thread_running, false);

    int error =
        pthread_create(&host->rss_thread, nullptr, host_rss_backend, host);
    if (error != 0) {
        printf("[rss] failed to start backend thread: %s\n", strerror(error));
        return;
    }

    host->rss_thread_initialized = true;
}

#define CHAT_ARCHIVE_INTERVAL_SECONDS (14U * 24U * 60U * 60U)

static bool host_archive_format_timestamp(char *buffer, size_t length)
{
    if (buffer == nullptr || length == 0U) {
        return false;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return false;
    }

    time_t adjusted = now.tv_sec + (9 * 60 * 60);
    struct tm utc9;
    if (gmtime_r(&adjusted, &utc9) == nullptr) {
        return false;
    }

    const char *ampm = utc9.tm_hour >= 12 ? "PM" : "AM";
    int written = snprintf(buffer, length,
                           "%04d-%02d-%02d-%02d-%02d%s-UTC+9.log",
                           utc9.tm_year + 1900, utc9.tm_mon + 1, utc9.tm_mday,
                           utc9.tm_hour, utc9.tm_min, ampm);
    return written > 0 && (size_t)written < length;
}

static bool host_archive_copy_file(const char *source, const char *dest)
{
    if (source == nullptr || dest == nullptr) {
        return false;
    }

    FILE *in = fopen(source, "rb");
    if (in == nullptr) {
        return false;
    }

    FILE *out = fopen(dest, "wb");
    if (out == nullptr) {
        fclose(in);
        return false;
    }

    char buffer[4096];
    size_t read_bytes = 0U;
    bool success = true;
    while ((read_bytes = fread(buffer, 1U, sizeof(buffer), in)) > 0U) {
        if (fwrite(buffer, 1U, read_bytes, out) != read_bytes) {
            success = false;
            break;
        }
    }

    fclose(in);
    fclose(out);
    return success;
}

static bool host_archive_run_command(const char *workdir, const char *command)
{
    if (command == nullptr || command[0] == '\0') {
        return false;
    }

    char wrapped[PATH_MAX * 2];
    if (workdir != nullptr && workdir[0] != '\0') {
        int written = snprintf(wrapped, sizeof(wrapped), "cd \"%s\" && %s",
                               workdir, command);
        if (written <= 0 || (size_t)written >= sizeof(wrapped)) {
            return false;
        }
    } else {
        int written = snprintf(wrapped, sizeof(wrapped), "%s", command);
        if (written <= 0 || (size_t)written >= sizeof(wrapped)) {
            return false;
        }
    }

    int rc = system(wrapped);
    return rc == 0;
}

static bool host_archive_push_snapshot(host_t *host)
{
    const char *remote = getenv("CHAT_ARCHIVE_URL");
    if (remote == nullptr || remote[0] == '\0') {
        return false;
    }

    if (host == nullptr || host->state_file_path[0] == '\0') {
        return false;
    }

    if (access(host->state_file_path, R_OK) != 0) {
        return false;
    }

    char timestamp[128];
    if (!host_archive_format_timestamp(timestamp, sizeof(timestamp))) {
        return false;
    }

    char base_dir[] = "/tmp/sshchatter_archiveXXXXXX";
    if (mkdtemp(base_dir) == nullptr) {
        return false;
    }

    char repo_dir[PATH_MAX];
    snprintf(repo_dir, sizeof(repo_dir), "%s/repo", base_dir);

    const char *token = getenv("GH_TOKEN");
    char auth_remote[PATH_MAX];
    const char *clone_target = remote;
    if (token != nullptr && token[0] != '\0' &&
        strncasecmp(remote, "https://", strlen("https://")) == 0) {
        const char *host_part = remote + strlen("https://");
        snprintf(auth_remote, sizeof(auth_remote), "https://x-access-token:%s@%s",
                 token, host_part);
        clone_target = auth_remote;
    }

    char clone_cmd[PATH_MAX * 2];
    size_t component_limit =
        (sizeof(clone_cmd) / 2U > 64U) ? (sizeof(clone_cmd) / 2U - 64U)
                                       : (sizeof(clone_cmd) / 2U);
    int component_precision = (int)component_limit;
    snprintf(clone_cmd, sizeof(clone_cmd),
             "GIT_TERMINAL_PROMPT=0 git clone --depth 1 %.*s %.*s",
             component_precision, clone_target, component_precision, repo_dir);

    bool success = host_archive_run_command(nullptr, clone_cmd);
    if (!success) {
        char cleanup_cmd[PATH_MAX * 2];
        snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf \"%s\"", base_dir);
        (void)host_archive_run_command(nullptr, cleanup_cmd);
        return false;
    }

    char destination[PATH_MAX];
    size_t dest_repo_limit =
        (sizeof(destination) > sizeof(timestamp) + 2U)
            ? (sizeof(destination) - sizeof(timestamp) - 2U)
            : (sizeof(destination) / 2U);
    int dest_repo_precision = (int)dest_repo_limit;
    int dest_time_precision = (int)(sizeof(timestamp) - 1U);
    snprintf(destination, sizeof(destination), "%.*s/%.*s", dest_repo_precision,
             repo_dir, dest_time_precision, timestamp);
    success = host_archive_copy_file(host->state_file_path, destination);
    if (!success) {
        char cleanup_cmd[PATH_MAX * 2];
        snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf \"%s\"", base_dir);
        (void)host_archive_run_command(nullptr, cleanup_cmd);
        return false;
    }

    char add_cmd[PATH_MAX * 2];
    snprintf(add_cmd, sizeof(add_cmd), "git -C %s add %s", repo_dir, timestamp);
    char commit_cmd[PATH_MAX * 2];
    snprintf(commit_cmd, sizeof(commit_cmd), "git -C %s commit -m \"Archive %s\"",
             repo_dir, timestamp);
    char push_cmd[PATH_MAX * 2];
    snprintf(push_cmd, sizeof(push_cmd), "git -C %s push origin HEAD", repo_dir);

    success = host_archive_run_command(nullptr, add_cmd) &&
              host_archive_run_command(nullptr, commit_cmd) &&
              host_archive_run_command(nullptr, push_cmd);

    if (success) {
        remove(host->state_file_path);
    }

    char cleanup_cmd[PATH_MAX * 2];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf \"%s\"", base_dir);
    (void)host_archive_run_command(nullptr, cleanup_cmd);
    return success;
}

static void *host_archive_backend(void *arg)
{
    host_t *host = (host_t *)arg;
    if (host == nullptr) {
        return nullptr;
    }

    sshc_epoch_thread_enter();
    atomic_store(&host->archive_thread_running, true);

    while (!atomic_load(&host->archive_thread_stop)) {
        (void)host_archive_push_snapshot(host);

        struct timespec mark;
        if (clock_gettime(CLOCK_MONOTONIC, &mark) == 0) {
            host->archive_last_run = mark;
        } else {
            host->archive_last_run.tv_sec = time(nullptr);
            host->archive_last_run.tv_nsec = 0L;
        }

        unsigned int remaining = CHAT_ARCHIVE_INTERVAL_SECONDS;
        while (remaining > 0U && !atomic_load(&host->archive_thread_stop)) {
            unsigned int chunk = remaining > 60U ? 60U : remaining;
            struct timespec pause = {
                .tv_sec = (time_t)chunk,
                .tv_nsec = 0L,
            };
            host_sleep_uninterruptible(&pause);
            if (remaining <= chunk) {
                remaining = 0U;
            } else {
                remaining -= chunk;
            }
        }
    }

    atomic_store(&host->archive_thread_running, false);
    sshc_epoch_thread_exit();
    return nullptr;
}

void host_archive_start_backend(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (host->archive_thread_initialized) {
        return;
    }

    const char *remote = getenv("CHAT_ARCHIVE_URL");
    if (remote == nullptr || remote[0] == '\0') {
        return;
    }

    atomic_store(&host->archive_thread_stop, false);
    atomic_store(&host->archive_thread_running, false);

    int error =
        pthread_create(&host->archive_thread, nullptr, host_archive_backend, host);
    if (error != 0) {
        printf("[archive] failed to start thread: %s\n", strerror(error));
        return;
    }

    host->archive_thread_initialized = true;
}

static void host_vote_state_load(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (host->vote_state_file_path[0] == '\0') {
        return;
    }

    FILE *fp = fopen(host->vote_state_file_path, "rb");
    if (fp == nullptr) {
        return;
    }

    vote_state_header_t header = {0};
    if (fread(&header, sizeof(header), 1U, fp) != 1U) {
        fclose(fp);
        return;
    }

    if (header.magic != VOTE_STATE_MAGIC) {
        fclose(fp);
        return;
    }

    if (header.version == 0U || header.version > VOTE_STATE_VERSION) {
        fclose(fp);
        return;
    }

    ttak_mutex_lock(&host->lock);

    poll_state_reset(&host->poll);
    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_NAMED_POLLS; ++idx) {
        named_poll_reset(&host->named_polls[idx]);
    }
    host->named_poll_count = 0U;

    bool success = true;

    vote_state_poll_entry_t main_entry = {0};
    if (fread(&main_entry, sizeof(main_entry), 1U, fp) != 1U) {
        success = false;
    } else {
        vote_state_import_poll_entry(&main_entry, &host->poll);
    }

    for (uint32_t idx = 0U; success && idx < header.named_count; ++idx) {
        vote_state_named_entry_t entry = {0};
        if (fread(&entry, sizeof(entry), 1U, fp) != 1U) {
            success = false;
            break;
        }

        if (idx >= SSH_CHATTER_MAX_NAMED_POLLS) {
            continue;
        }

        named_poll_state_t *poll = &host->named_polls[idx];
        vote_state_import_poll_entry(&entry.poll, &poll->poll);
        snprintf(poll->label, sizeof(poll->label), "%s", entry.label);
        snprintf(poll->owner, sizeof(poll->owner), "%s", entry.owner);
        poll->voter_count = entry.voter_count;
        if (poll->voter_count > SSH_CHATTER_MAX_NAMED_VOTERS) {
            poll->voter_count = SSH_CHATTER_MAX_NAMED_VOTERS;
        }
        for (size_t voter = 0U; voter < SSH_CHATTER_MAX_NAMED_VOTERS; ++voter) {
            snprintf(poll->voters[voter].username,
                     sizeof(poll->voters[voter].username), "%s",
                     entry.voters[voter].username);
            poll->voters[voter].choice = entry.voters[voter].choice;
            poll->voters[voter].choices_mask = entry.voters[voter].choices_mask;
        }
    }

    if (success) {
        host_recount_named_polls_locked(host);
    } else {
        poll_state_reset(&host->poll);
        for (size_t idx = 0U; idx < SSH_CHATTER_MAX_NAMED_POLLS; ++idx) {
            named_poll_reset(&host->named_polls[idx]);
        }
        host->named_poll_count = 0U;
    }

    ttak_mutex_unlock(&host->lock);
    fclose(fp);
}

static void host_ban_state_load(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (host->ban_state_file_path[0] == '\0') {
        return;
    }

    FILE *fp = fopen(host->ban_state_file_path, "rb");
    if (fp == nullptr) {
        return;
    }

    ban_state_header_t header = {0};
    if (fread(&header, sizeof(header), 1U, fp) != 1U) {
        fclose(fp);
        return;
    }

    if (header.magic != BAN_STATE_MAGIC || header.version == 0U ||
        header.version > BAN_STATE_VERSION) {
        fclose(fp);
        return;
    }

    uint32_t entry_count = header.entry_count;
    ban_state_entry_t *entries = nullptr;
    if (entry_count > 0U) {
        entries = sshc_gc_calloc(entry_count, sizeof(*entries));
        if (entries == nullptr) {
            fclose(fp);
            humanized_log_error("host", "failed to allocate ban state buffer",
                                ENOMEM);
            return;
        }
    }

    bool success = true;
    int read_error = 0;
    for (uint32_t idx = 0U; idx < entry_count; ++idx) {
        if (fread(&entries[idx], sizeof(entries[idx]), 1U, fp) != 1U) {
            success = false;
            if (errno != 0) {
                read_error = errno;
            }
            break;
        }
    }

    fclose(fp);

    if (!success) {
        humanized_log_error("host", "failed to read ban state file",
                            read_error != 0 ? read_error : EIO);
        sshc_gc_free(entries);
        return;
    }

    ttak_mutex_lock(&host->lock);
    memset(host->bans, 0, sizeof(host->bans));
    host->ban_count = 0U;
    for (uint32_t idx = 0U; idx < entry_count; ++idx) {
        if (host->ban_count >= SSH_CHATTER_MAX_BANS) {
            break;
        }
        if (entries[idx].ip[0] != '\0' &&
            host_is_protected_ip_unlocked(host, entries[idx].ip)) {
            continue;
        }
        if (entries[idx].ip[0] != '\0' &&
            strchr(entries[idx].ip, '/') != nullptr) {
            bool intersects_protected = false;
            for (size_t protected_idx = 0;
                 protected_idx < host->protected_ip_count &&
                 protected_idx < SSH_CHATTER_MAX_PROTECTED_IPS;
                 ++protected_idx) {
                if (host_cidr_contains_ip(entries[idx].ip,
                                          host->protected_ips[protected_idx])) {
                    intersects_protected = true;
                    break;
                }
            }
            if (intersects_protected) {
                continue;
            }
        }
        snprintf(host->bans[host->ban_count].username,
                 sizeof(host->bans[host->ban_count].username), "%s",
                 entries[idx].username);
        snprintf(host->bans[host->ban_count].ip,
                 sizeof(host->bans[host->ban_count].ip), "%s", entries[idx].ip);
        ++host->ban_count;
    }
    ttak_mutex_unlock(&host->lock);
    sshc_gc_free(entries);
}

static void host_reply_state_load(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (host->reply_state_file_path[0] == '\0') {
        return;
    }

    FILE *fp = fopen(host->reply_state_file_path, "rb");
    if (fp == nullptr) {
        return;
    }

    reply_state_header_t header = {0};
    if (fread(&header, sizeof(header), 1U, fp) != 1U) {
        fclose(fp);
        return;
    }

    if (header.magic != REPLY_STATE_MAGIC || header.version == 0U ||
        header.version > REPLY_STATE_VERSION) {
        fclose(fp);
        return;
    }

    uint32_t entry_count = header.entry_count;
    reply_state_entry_t *entries = nullptr;
    if (entry_count > 0U) {
        entries = sshc_gc_calloc(entry_count, sizeof(*entries));
        if (entries == nullptr) {
            fclose(fp);
            humanized_log_error("host", "failed to allocate reply state buffer",
                                ENOMEM);
            return;
        }
    }

    bool success = true;
    int read_error = 0;
    for (uint32_t idx = 0U; idx < entry_count; ++idx) {
        if (fread(&entries[idx], sizeof(entries[idx]), 1U, fp) != 1U) {
            success = false;
            if (errno != 0) {
                read_error = errno;
            }
            break;
        }
    }

    fclose(fp);

    if (!success) {
        humanized_log_error("host", "failed to read reply state file",
                            read_error != 0 ? read_error : EIO);
        return;
    }

    ttak_mutex_lock(&host->lock);
    memset(host->replies, 0, sizeof(host->replies));
    host->reply_count = 0U;
    host->next_reply_id =
        header.next_reply_id != 0U ? header.next_reply_id : 1U;
    uint64_t max_reply_id = 0U;

    for (uint32_t idx = 0U; idx < entry_count; ++idx) {
        if (host->reply_count >= SSH_CHATTER_MAX_REPLIES) {
            if (entries[idx].reply_id > max_reply_id) {
                max_reply_id = entries[idx].reply_id;
            }
            continue;
        }

        chat_reply_entry_t *slot = &host->replies[host->reply_count];
        memset(slot, 0, sizeof(*slot));
        slot->in_use = true;
        slot->reply_id = entries[idx].reply_id != 0U
                             ? entries[idx].reply_id
                             : (uint64_t)(host->reply_count + 1U);
        if (slot->reply_id > max_reply_id) {
            max_reply_id = slot->reply_id;
        }
        slot->parent_message_id = entries[idx].parent_message_id;
        slot->parent_reply_id = entries[idx].parent_reply_id;
        slot->created_at = (time_t)entries[idx].created_at;
        snprintf(slot->username, sizeof(slot->username), "%s",
                 entries[idx].username);
        snprintf(slot->message, sizeof(slot->message), "%s",
                 entries[idx].message);
        ++host->reply_count;
    }

    if (host->next_reply_id <= max_reply_id) {
        if (max_reply_id == UINT64_MAX) {
            host->next_reply_id = UINT64_MAX;
        } else {
            host->next_reply_id = max_reply_id + 1U;
        }
    }

    if (host->next_reply_id == 0U) {
        host->next_reply_id = (uint64_t)host->reply_count + 1U;
    }

    ttak_mutex_unlock(&host->lock);
}

static void host_eliza_memory_resolve_path(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    const char *memory_path = getenv("CHATTER_ELIZA_MEMORY_FILE");
    if (memory_path == nullptr || memory_path[0] == '\0') {
        memory_path = "eliza_memory.dat";
    }

    int written =
        snprintf(host->eliza_memory_file_path,
                 sizeof(host->eliza_memory_file_path), "%s", memory_path);
    if (written < 0 ||
        (size_t)written >= sizeof(host->eliza_memory_file_path)) {
        humanized_log_error("host", "eliza memory file path is too long",
                            ENAMETOOLONG);
        host->eliza_memory_file_path[0] = '\0';
    }
}

static void host_eliza_memory_save_locked(host_t *host)
{
    if (host == nullptr || host->eliza_memory_file_path[0] == '\0') {
        return;
    }

    if (!host_ensure_private_data_path(host, host->eliza_memory_file_path,
                                       true)) {
        return;
    }

    char temp_path[PATH_MAX];
    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp",
                           host->eliza_memory_file_path);
    if (written < 0 || (size_t)written >= sizeof(temp_path)) {
        humanized_log_error("host", "eliza memory path is too long",
                            ENAMETOOLONG);
        return;
    }

    FILE *fp = fopen(temp_path, "wb");
    if (fp == nullptr) {
        humanized_log_error("host", "failed to open eliza memory file",
                            errno != 0 ? errno : EIO);
        return;
    }

    size_t stored = host->eliza_memory_count;
    if (stored > SSH_CHATTER_ELIZA_MEMORY_MAX) {
        stored = SSH_CHATTER_ELIZA_MEMORY_MAX;
    }

    eliza_memory_header_t header = {0};
    header.magic = ELIZA_MEMORY_MAGIC;
    header.version = ELIZA_MEMORY_VERSION;
    header.entry_count = (uint32_t)stored;
    header.next_id = host->eliza_memory_next_id;

    bool success = fwrite(&header, sizeof(header), 1U, fp) == 1U;
    int write_error = 0;
    if (!success && errno != 0) {
        write_error = errno;
    }

    for (size_t idx = 0U; success && idx < stored; ++idx) {
        const eliza_memory_entry_t *entry = &host->eliza_memory[idx];
        eliza_memory_entry_serialized_t serialized = {0};
        serialized.id = entry->id;
        serialized.stored_at = (int64_t)entry->stored_at;
        snprintf(serialized.prompt, sizeof(serialized.prompt), "%s",
                 entry->prompt);
        snprintf(serialized.reply, sizeof(serialized.reply), "%s",
                 entry->reply);
        if (fwrite(&serialized, sizeof(serialized), 1U, fp) != 1U) {
            success = false;
            if (errno != 0) {
                write_error = errno;
            }
            break;
        }
    }

    if (success && fflush(fp) != 0) {
        success = false;
        if (errno != 0) {
            write_error = errno;
        }
    }

    if (success) {
        int fd = fileno(fp);
        if (fd >= 0 && fsync(fd) != 0) {
            success = false;
            if (errno != 0) {
                write_error = errno;
            }
        }
    }

    if (fclose(fp) != 0) {
        if (success && errno != 0) {
            write_error = errno;
        }
        success = false;
    }

    if (!success) {
        unlink(temp_path);
        humanized_log_error("host", "failed to write eliza memory file",
                            write_error != 0 ? write_error : EIO);
        return;
    }

    if (rename(temp_path, host->eliza_memory_file_path) != 0) {
        int rename_error = errno != 0 ? errno : EIO;
        unlink(temp_path);
        humanized_log_error("host", "failed to install eliza memory file",
                            rename_error);
        return;
    }

    if (chmod(host->eliza_memory_file_path, S_IRUSR | S_IWUSR) != 0) {
        humanized_log_error("host", "failed to set eliza memory permissions",
                            errno != 0 ? errno : EACCES);
    }
}

static void host_eliza_memory_load(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (host->eliza_memory_file_path[0] == '\0') {
        return;
    }

    if (!host_ensure_private_data_path(host, host->eliza_memory_file_path,
                                       false)) {
        return;
    }

    FILE *fp = fopen(host->eliza_memory_file_path, "rb");
    if (fp == nullptr) {
        return;
    }

    eliza_memory_header_t header = {0};
    if (fread(&header, sizeof(header), 1U, fp) != 1U) {
        fclose(fp);
        return;
    }

    if (header.magic != ELIZA_MEMORY_MAGIC || header.version == 0U ||
        header.version > ELIZA_MEMORY_VERSION) {
        fclose(fp);
        return;
    }

    uint32_t entry_count = header.entry_count;
    eliza_memory_entry_serialized_t *entries = nullptr;
    if (entry_count > 0U) {
        entries = sshc_gc_calloc(entry_count, sizeof(*entries));
        if (entries == nullptr) {
            fclose(fp);
            humanized_log_error(
                "host", "failed to allocate eliza memory buffer", ENOMEM);
            return;
        }
    }

    bool success = true;
    int read_error = 0;
    for (uint32_t idx = 0U; idx < entry_count; ++idx) {
        if (fread(&entries[idx], sizeof(entries[idx]), 1U, fp) != 1U) {
            success = false;
            if (errno != 0) {
                read_error = errno;
            }
            break;
        }
    }

    fclose(fp);

    if (!success) {
        humanized_log_error("host", "failed to read eliza memory file",
                            read_error != 0 ? read_error : EIO);
        return;
    }

    ttak_mutex_lock(&host->lock);
    memset(host->eliza_memory, 0, sizeof(host->eliza_memory));
    host->eliza_memory_count = 0U;
    host->eliza_memory_next_id = header.next_id != 0U ? header.next_id : 1U;

    uint64_t max_id = 0U;
    for (uint32_t idx = 0U; idx < entry_count; ++idx) {
        uint64_t entry_id =
            entries[idx].id != 0U ? entries[idx].id : (uint64_t)(idx + 1U);
        if (idx < SSH_CHATTER_ELIZA_MEMORY_MAX) {
            eliza_memory_entry_t *slot =
                &host->eliza_memory[host->eliza_memory_count++];
            slot->id = entry_id;
            slot->stored_at = (time_t)entries[idx].stored_at;
            snprintf(slot->prompt, sizeof(slot->prompt), "%s",
                     entries[idx].prompt);
            snprintf(slot->reply, sizeof(slot->reply), "%s",
                     entries[idx].reply);
        }
        if (entry_id > max_id) {
            max_id = entry_id;
        }
    }

    if (max_id >= host->eliza_memory_next_id) {
        host->eliza_memory_next_id =
            (max_id == UINT64_MAX) ? UINT64_MAX : max_id + 1U;
    }
    if (host->eliza_memory_next_id == 0U) {
        host->eliza_memory_next_id = (uint64_t)host->eliza_memory_count + 1U;
    }

    ttak_mutex_unlock(&host->lock);
}

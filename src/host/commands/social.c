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
                                        bool ip_wide, bool fixnick,
                                        const char *owner_ip,
                                        char *buffer, size_t buffer_length)
{
    if (username == nullptr || buffer == nullptr || buffer_length == 0U) {
        return false;
    }

    char salt_hex[64];
    char hash_hex[128];
    session_pw_auth_hex_encode(salt, salt_length, salt_hex, sizeof(salt_hex));
    session_pw_auth_hex_encode(hash, hash_length, hash_hex, sizeof(hash_hex));

    int written = snprintf(buffer, buffer_length, "%s:%s:%s:%d:%d:%s", username,
                           salt_hex, hash_hex, ip_wide ? 1 : 0,
                           fixnick ? 1 : 0, owner_ip != nullptr ? owner_ip : "");
    return written >= 0 && (size_t)written < buffer_length;
}

static bool session_pw_auth_update(host_t *host, const char *username,
                                   const uint8_t *salt, size_t salt_length,
                                   const uint8_t *hash, size_t hash_length,
                                   bool ip_wide, bool fixnick,
                                   const char *owner_ip,
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
                            ip_wide, fixnick, owner_ip, formatted,
                            sizeof(formatted)) ||
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
                                         hash_length, ip_wide, fixnick,
                                         owner_ip, formatted,
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

static void session_parse_password_arguments(const char *arguments,
                                             char *password,
                                             size_t password_len,
                                             bool *ip_wide,
                                             bool *ip_wide_explicit)
{
    if (password != nullptr && password_len > 0U) {
        password[0] = '\0';
    }
    if (ip_wide != nullptr) {
        *ip_wide = false;
    }
    if (ip_wide_explicit != nullptr) {
        *ip_wide_explicit = false;
    }
    if (arguments == nullptr || password == nullptr || password_len == 0U) {
        return;
    }

    snprintf(password, password_len, "%s", arguments);
    trim_whitespace_inplace(password);
    if (password[0] == '\0') {
        return;
    }

    char *marker = strstr(password, " ip-wide ");
    if (marker == nullptr) {
        return;
    }

    char value[16];
    snprintf(value, sizeof(value), "%s", marker + strlen(" ip-wide "));
    trim_whitespace_inplace(value);

    bool parsed = false;
    bool parsed_value = false;
    if (value[0] != '\0') {
        parsed = parse_bool_token(value, &parsed_value);
        if (!parsed && session_argument_is_disable(value)) {
            parsed = true;
            parsed_value = false;
        }
    }

    if (!parsed) {
        return;
    }

    *marker = '\0';
    trim_whitespace_inplace(password);
    if (ip_wide != nullptr) {
        *ip_wide = parsed_value;
    }
    if (ip_wide_explicit != nullptr) {
        *ip_wide_explicit = true;
    }
}

static void session_handle_setpw(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    char parsed_password[256];
    bool ip_wide = false;
    bool ip_wide_explicit = false;
    session_parse_password_arguments(arguments, parsed_password,
                                     sizeof(parsed_password), &ip_wide,
                                     &ip_wide_explicit);

    if (parsed_password[0] != '\0' && strlen(parsed_password) > 128) {
        session_send_system_line(ctx,
                                 "Password is too long (max 128 characters).");
        return;
    }

    if (!session_user_data_load(ctx)) {
        session_send_system_line(ctx, "Unable to load user data.");
        return;
    }

    if (parsed_password[0] == '\0') {
        memset(ctx->user_data.password_salt, 0,
               sizeof(ctx->user_data.password_salt));
        memset(ctx->user_data.password_hash, 0,
               sizeof(ctx->user_data.password_hash));
        user_data_set_reserved_nickname_ip_wide(&ctx->user_data, false);

        if (session_user_data_commit(ctx)) {
            session_send_system_line(ctx, "Password removed.");
            host_nickname_claim_remove(ctx->owner, ctx->user.name);
            if (!session_pw_auth_update(ctx->owner, ctx->user.name, nullptr, 0U,
                                        nullptr, 0U, false, false, nullptr,
                                        false)) {
                session_send_system_line(
                    ctx, "Warning: unable to update pw_auth.dat.");
            }
        } else {
            session_send_system_line(ctx, "Failed to remove password.");
        }
        return;
    }

    security_layer_generate_salt(ctx->user_data.password_salt);
    security_layer_hash_password(parsed_password, ctx->user_data.password_salt,
                                 ctx->user_data.password_hash);
    user_data_set_reserved_nickname_ip_wide(
        &ctx->user_data, ip_wide_explicit && ip_wide);

    if (session_user_data_commit(ctx)) {
        session_send_system_line(ctx, "Password set successfully.");
        if (!session_pw_auth_update(
                ctx->owner, ctx->user.name, ctx->user_data.password_salt,
                sizeof(ctx->user_data.password_salt),
                ctx->user_data.password_hash,
                sizeof(ctx->user_data.password_hash),
                ip_wide_explicit ? ip_wide : false,
                user_data_fixnick_enabled(&ctx->user_data), ctx->client_ip,
                true)) {
            session_send_system_line(ctx,
                                     "Warning: unable to update pw_auth.dat.");
        }
        if (!host_nickname_claim_upsert(
                ctx->owner, ctx, ctx->user.name, ctx->user_data.password_salt,
                ctx->user_data.password_hash,
                ip_wide_explicit ? ip_wide : false,
                user_data_fixnick_enabled(&ctx->user_data))) {
            session_send_system_line(
                ctx, "Warning: unable to create runtime nickname claim.");
        }
        session_send_system_line(
            ctx, (ip_wide_explicit && ip_wide)
                     ? "Nickname claim enabled for this session (IP-wide)."
                     : "Nickname claim enabled for this session.");

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
    user_data_set_reserved_nickname_ip_wide(&user_data, false);

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

        host_nickname_claim_remove(ctx->owner, target_user);

        if (!session_pw_auth_update(ctx->owner, target_user, nullptr, 0U,
                                    nullptr, 0U, false, false, nullptr,
                                    false)) {
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
        char last_ip[SSH_CHATTER_IP_LEN] = {0};
        if (host_lookup_last_ip(ctx->owner, target_nickname, last_ip,
                                sizeof(last_ip))) {
            last_ip[sizeof(last_ip) - 1U] = '\0';
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
    user_data_set_reserved_nickname_ip_wide(&user_data, false);

    // Save the modified user data
    bool success =
        user_data_save(ctx->owner->user_data_root, &user_data, target_ip);

    if (success) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "Password for '%s' has been reset.",
                 target_nickname);
        session_send_system_line(ctx, message);

        if (!session_pw_auth_update(ctx->owner, target_nickname, nullptr, 0U,
                                    nullptr, 0U, false, false, nullptr,
                                    false)) {
            session_send_system_line(ctx,
                                     "Warning: unable to update pw_auth.dat.");
        }
        host_nickname_claim_remove(ctx->owner, target_nickname);

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

    bool allow_dangerous_management = false;
    const char *dangerous_management_env =
        getenv("SSH_CHATTER_ALLOW_DANGEROUS_MANAGEMENT");
    if (dangerous_management_env != nullptr) {
        if (strcasecmp(dangerous_management_env, "1") == 0 ||
            strcasecmp(dangerous_management_env, "true") == 0 ||
            strcasecmp(dangerous_management_env, "yes") == 0 ||
            strcasecmp(dangerous_management_env, "on") == 0) {
            allow_dangerous_management = true;
        }
    }

    int pty_master = -1;
    pid_t pid = forkpty(&pty_master, nullptr, nullptr, nullptr);
    if (pid < 0) {
        session_send_system_line(ctx, "Failed to start shell.");
        return;
    }

    if (pid == 0) {
        if (allow_dangerous_management) {
            execlp("sudo", "sudo", "-s", (char *)nullptr);
        }
        execl("/bin/sh", "sh", "-i", (char *)nullptr);
        _exit(127);
    }

    if (allow_dangerous_management) {
        session_send_system_line(
            ctx,
            "Launching sudo shell (SSH_CHATTER_ALLOW_DANGEROUS_MANAGEMENT=true). Type 'exit' to return.");
    } else {
        session_send_system_line(
            ctx, "Launching /bin/sh. Type 'exit' to return to SSH-Chatter.");
    }

    bool input_open = true;
    bool output_open = true;
    while (input_open || output_open) {
        struct pollfd child_out = {
            .fd = pty_master,
            .events = output_open ? POLLIN : 0,
            .revents = 0,
        };

        if (output_open) {
            int poll_result = poll(&child_out, 1, 10);
            if (poll_result > 0 && (child_out.revents & POLLIN)) {
                char buffer[1024];
                ssize_t read_len = read(pty_master, buffer, sizeof(buffer));
                if (read_len > 0) {
                    session_channel_write(ctx, buffer, (size_t)read_len);
                } else {
                    output_open = false;
                }
            } else if (poll_result < 0 && errno != EINTR) {
                output_open = false;
            } else if (child_out.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                output_open = false;
            }
        }

        if (input_open) {
            if (!session_transport_is_open(ctx) || session_transport_is_eof(ctx)) {
                input_open = false;
                continue;
            }

            char input[512];
            int received = session_transport_read(ctx, input, sizeof(input), 10);
            if (received > 0) {
                ssize_t sent = write(pty_master, input, (size_t)received);
                if (sent != received) {
                    input_open = false;
                }
            } else if (received == SSH_AGAIN) {
                // no input available yet
            } else if (received < 0 || session_transport_is_eof(ctx) ||
                       !session_transport_is_open(ctx)) {
                input_open = false;
            }
        }

        int status = 0;
        pid_t wait_result = waitpid(pid, &status, WNOHANG);
        if (wait_result == pid) {
            output_open = false;
            input_open = false;
            break;
        }
    }

    if (pty_master >= 0) {
        close(pty_master);
        pty_master = -1;
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
        char current_topology[SSH_CHATTER_TOPOLOGY_LEN];
        session_build_network_topology_key(ctx, current_topology,
                                           sizeof(current_topology));
        const bool ip_match = entry.user_ip[0] != '\0' &&
                              ctx->client_ip[0] != '\0' &&
                              strcmp(entry.user_ip, ctx->client_ip) == 0;
        const bool topology_match =
            entry.user_topology[0] != '\0' && current_topology[0] != '\0' &&
            strcmp(entry.user_topology, current_topology) == 0;
        if (!(name_match && (ip_match || topology_match))) {
            session_send_system_line(
                ctx, "You can only delete your own messages from the same "
                     "connection topology.");
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

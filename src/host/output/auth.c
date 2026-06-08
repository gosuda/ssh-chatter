static bool session_handle_service_request(ssh_message message)
{
    if (message == nullptr) {
        return false;
    }

    const char *service = ssh_message_service_service(message);
    if (service == nullptr) {
        return false;
    }

    if (strcmp(service, "ssh-userauth") == 0 ||
        strcmp(service, "ssh-connection") == 0) {
        ssh_message_service_reply_success(message);
        return true;
    }

    return false;
}

bool is_nullarray(uint8_t *arr, size_t len)
{
    uint8_t zeros[len];
    memset(zeros, 0, len);
    return memcmp(arr, zeros, len) == 0;
}

static int session_authenticate(session_ctx_t *ctx)
{
    ssh_message message = nullptr;
    bool authenticated = false;
    if (ctx != nullptr) {
        ctx->lan_operator_credentials_valid = false;
    }

    // Declare credential here to ensure it's in scope for all uses
    lan_operator_credential_t *credential = nullptr;

    while (!authenticated &&
           (message = ssh_message_get(ctx->session)) != nullptr) {
        const int message_type = ssh_message_type(message);
        switch (message_type) {
        case SSH_REQUEST_SERVICE:
            if (!session_handle_service_request(message)) {
                ssh_message_reply_default(message);
            }
            break;
        case SSH_REQUEST_AUTH: {
            const char *username = ssh_message_auth_user(message);
            if (username != nullptr && username[0] != '\0') {
                char cleaned_username[SSH_CHATTER_USERNAME_LEN];
                if (!user_data_strip_ansi_sequences(username, cleaned_username,
                                                    sizeof(cleaned_username)) ||
                    cleaned_username[0] == '\0') {
                    snprintf(cleaned_username, sizeof(cleaned_username), "%.*s",
                             SSH_CHATTER_USERNAME_LEN - 1, username);
                }

                snprintf(ctx->user.name, sizeof(ctx->user.name), "%s",
                         cleaned_username);
            }

            // Load user data
            bool loaded_by_username = false;
            ttak_mutex_lock(&ctx->owner->user_data_lock);
            if (user_data_load(ctx->owner->user_data_root, ctx->user.name, NULL,
                               &ctx->user_data)) {
                if (!security_layer_is_zero_hash(
                        ctx->user_data.password_hash,
                        sizeof(ctx->user_data.password_hash))) {
                    loaded_by_username = true;
                }
            }

            if (!loaded_by_username) {
                user_data_ensure_exists(ctx->owner->user_data_root,
                                        ctx->user.name, ctx->client_ip,
                                        &ctx->user_data);
            }
            ttak_mutex_unlock(&ctx->owner->user_data_lock);

            // Check if a password is set for this user
            bool password_is_set = !security_layer_is_zero_hash(
                ctx->user_data.password_hash,
                sizeof(ctx->user_data.password_hash));

            // Handle LAN operator authentication
            bool reserved_name = false;
            // credential variable is already declared at the beginning of the function
            if (ctx->owner != nullptr) {
                credential = host_find_lan_operator_credential(ctx->owner,
                                                               ctx->user.name);
                reserved_name = credential != nullptr;
            }

            if (reserved_name) {
                if (!session_is_lan_client(ctx->client_ip)) {
                    ssh_message_auth_set_methods(message,
                                                 SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(message);
                    break; // Break from switch, continue while loop
                }

                const int auth_method = ssh_message_subtype(message);
                if (auth_method != SSH_AUTH_METHOD_PASSWORD) {
                    ssh_message_auth_set_methods(message,
                                                 SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(message);
                    break; // Break from switch, continue while loop
                }

                const char *password = ssh_message_auth_password(message);
                if (credential == nullptr || password == nullptr ||
                    credential->password[0] == '\0' ||
                    strcmp(credential->password, password) != 0) {
                    ssh_message_auth_set_methods(message,
                                                 SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(message);
                    break; // Break from switch, continue while loop
                }
                ctx->lan_operator_credentials_valid = true;
                snprintf(ctx->user.name, sizeof(ctx->user.name), "%s",
                         credential->nickname);
                ssh_message_auth_reply_success(message, 0);
                authenticated = true; // This will break the while loop
                break;                // Break from switch
            }

            // Regular user authentication
            if (!password_is_set) {
                // No password set, allow login but flag for password creation
                ctx->password_not_set = true;
                ssh_message_auth_reply_success(message, 0);
                authenticated = true; // This will break the while loop
                break;                // Break from switch
            } else {
                // Password is set, require password authentication
                const int auth_method = ssh_message_subtype(message);
                if (auth_method != SSH_AUTH_METHOD_PASSWORD) {
                    ssh_message_auth_set_methods(message,
                                                 SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(message);
                    break; // Break from switch, continue while loop
                }

                const char *password = ssh_message_auth_password(message);
                if (password == nullptr) {
                    ssh_message_auth_set_methods(message,
                                                 SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(message);
                    break; // Break from switch, continue while loop
                }

                uint8_t provided_password_hash[32];
                security_layer_hash_password(password,
                                             ctx->user_data.password_salt,
                                             provided_password_hash);

                if (memcmp(provided_password_hash, ctx->user_data.password_hash,
                           sizeof(provided_password_hash)) == 0) {
                    ssh_message_auth_reply_success(message, 0);
                    authenticated = true; // This will break the while loop
                    break;                // Break from switch
                } else {
                    ssh_message_auth_set_methods(message,
                                                 SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(message);
                    break; // Break from switch, continue while loop
                }
            }
            break;
        }
        case SSH_CHANNEL_REQUEST_WINDOW_CHANGE:
            ssh_message_channel_request_reply_success(message);
            break;
        case SSH_CHANNEL_REQUEST_SHELL:
            ssh_message_channel_request_reply_success(message);
            break;
        default:
            ssh_message_reply_default(message);
            break;
        }
        ssh_message_free(message);
    }

    return authenticated ? 0 : -1;
}

static int session_accept_channel(session_ctx_t *ctx)
{
    ssh_message message = nullptr;

    while ((message = ssh_message_get(ctx->session)) != nullptr) {
        const int message_type = ssh_message_type(message);
        if (message_type == SSH_REQUEST_SERVICE) {
            if (!session_handle_service_request(message)) {
                ssh_message_reply_default(message);
            }
            ssh_message_free(message);
            continue;
        }

        if (message_type == SSH_REQUEST_CHANNEL_OPEN &&
            ssh_message_subtype(message) == SSH_CHANNEL_SESSION) {
            ssh_channel channel =
                ssh_message_channel_request_open_reply_accept(message);
            if (channel == nullptr) {
                accept_channel_fn_t accept_channel =
                    resolve_accept_channel_fn();
                if (accept_channel != nullptr) {
                    channel = ssh_channel_new(ctx->session);
                    if (channel != nullptr) {
                        if (accept_channel(message, channel) != SSH_OK) {
                            ssh_channel_free(channel);
                            channel = nullptr;
                        }
                    }
                }
            }

            if (channel != nullptr) {
                ctx->channel = channel;
                ssh_message_free(message);
                break;
            }

            ssh_message_reply_default(message);
            ssh_message_free(message);
            continue;
        }

        ssh_message_reply_default(message);
        ssh_message_free(message);
    }

    return session_transport_active(ctx) ? 0 : -1;
}

static int session_on_window_change(ssh_session session, ssh_channel channel,
                                    int width, int height, int pxwidth,
                                    int pwheight, void *userdata)
{
    (void)session;
    (void)channel;
    (void)pxwidth;
    (void)pwheight;

    session_runtime_data_t *runtime = (session_runtime_data_t *)userdata;
    if (runtime == nullptr || !atomic_load(&runtime->active)) {
        return -1;
    }

    session_ctx_t *ctx = runtime->ctx;
    if (ctx == nullptr) {
        return -1;
    }

    if (width > 0 && width <= SSH_CHATTER_MESSAGE_LIMIT) {
        ctx->terminal_width = (unsigned int)width;
    }
    if (height > 0 && height <= SSH_CHATTER_MESSAGE_LIMIT) {
        ctx->terminal_height = (unsigned int)height;
    }

    // Trigger a clean screen redraw with the new dimensions.
    ctx->pending_should_sink = true;

    return 0;
}

static void session_install_channel_callbacks(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->channel == nullptr || ctx->channel_cb_installed ||
        ctx->session_data == nullptr) {
        return;
    }

    memset(&ctx->channel_cb, 0, sizeof(ctx->channel_cb));
    ctx->channel_cb.size = sizeof(ctx->channel_cb);
    ctx->channel_cb.userdata = ctx->session_data;
    ctx->channel_cb.channel_pty_window_change_function =
        session_on_window_change;
    ssh_callbacks_init(&ctx->channel_cb);
    if (ssh_set_channel_callbacks(ctx->channel, &ctx->channel_cb) == SSH_OK) {
        ctx->channel_cb_installed = true;
    }
}

static int session_prepare_shell(session_ctx_t *ctx)
{
    ssh_message message = nullptr;
    bool shell_ready = false;

    while (!shell_ready &&
           (message = ssh_message_get(ctx->session)) != nullptr) {
        if (ssh_message_type(message) == SSH_REQUEST_CHANNEL) {
            const int subtype = ssh_message_subtype(message);
            if (subtype == SSH_CHANNEL_REQUEST_PTY ||
                subtype == SSH_CHANNEL_REQUEST_SHELL ||
                subtype == SSH_CHANNEL_REQUEST_EXEC ||
                subtype == SSH_CHANNEL_REQUEST_SUBSYSTEM) {
                if (subtype == SSH_CHANNEL_REQUEST_PTY) {
                    const int raw_width =
                        ssh_message_channel_request_pty_width(message);
                    const int raw_height =
                        ssh_message_channel_request_pty_height(message);
                    int width = raw_width;
                    int height = raw_height;
                    if (width > 0) {
                        if (width > SSH_CHATTER_MESSAGE_LIMIT) {
                            width = SSH_CHATTER_MESSAGE_LIMIT;
                        }
                        ctx->terminal_width = width > 0 ? (unsigned)width: 0;
                    }
                    if (height > 0) {
                        ctx->terminal_height = height > 0 ? (unsigned)height: 0;
                    }
                }
                if (subtype == SSH_CHANNEL_REQUEST_EXEC) {
                    const char *command =
                        ssh_message_channel_request_command(message);
                    ssh_message_channel_request_reply_success(message);
                    if (command != nullptr &&
                        strncmp(command, "scp", 3) == 0 &&
                        ctx->owner != nullptr &&
                        ctx->owner->file_storage_ready) {
                        int result =
                            file_transfer_handle_scp_exec(ctx, command);
                        ctx->exit_status =
                            (result == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
                        ssh_channel_request_send_exit_status(
                            ctx->channel, ctx->exit_status);
                        ssh_channel_send_eof(ctx->channel);
                        ssh_channel_close(ctx->channel);
                        ssh_message_free(message);
                        return 1;
                    }
                } else if (subtype == SSH_CHANNEL_REQUEST_SUBSYSTEM) {
                    const char *subsystem =
                        ssh_message_channel_request_subsystem(message);
                    if (subsystem != nullptr &&
                        strcmp(subsystem, "sftp") == 0) {
                        humanized_log_error("session",
                                            "SFTP subsystem requested.",
                                            0);
                        ssh_message_channel_request_reply_success(message);
                        int result = file_transfer_handle_sftp(ctx);
                        ctx->exit_status =
                            (result == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
                        ssh_channel_request_send_exit_status(
                            ctx->channel, ctx->exit_status);
                        ssh_channel_send_eof(ctx->channel);
                        ssh_channel_close(ctx->channel);
                        ssh_message_free(message);
                        return 1;
                    } else {
                        ssh_message_reply_default(message);
                    }
                } else {
                    ssh_message_channel_request_reply_success(message);
                }
                if (subtype == SSH_CHANNEL_REQUEST_SHELL) {
                    shell_ready = true;
                }
            } else {
                ssh_message_reply_default(message);
            }
        } else {
            ssh_message_reply_default(message);
        }
        ssh_message_free(message);
    }

    return shell_ready ? 0 : -1;
}

static const char *
session_captcha_question_for_language(const captcha_prompt_t *prompt,
                                      captcha_language_t language)
{
    if (prompt == nullptr) {
        return nullptr;
    }

    switch (language) {
    case CAPTCHA_LANGUAGE_EN:
        return prompt->question_en;
    case CAPTCHA_LANGUAGE_ZH:
        return prompt->question_zh;
    case CAPTCHA_LANGUAGE_RU:
        return prompt->question_ru;
    case CAPTCHA_LANGUAGE_KO:
    default:
        return prompt->question_ko;
    }
}

static const char *
session_captcha_label_for_language(captcha_language_t language)
{
    switch (language) {
    case CAPTCHA_LANGUAGE_EN:
        return "Captcha: ";
    case CAPTCHA_LANGUAGE_ZH:
        return "驗證碼: ";
    case CAPTCHA_LANGUAGE_RU:
        return "Капча: ";
    case CAPTCHA_LANGUAGE_KO:
    default:
        return "캡챠: ";
    }
}

static captcha_language_t
session_captcha_language_from_ui(session_ui_language_t language)
{
    switch (language) {
    case SESSION_UI_LANGUAGE_EN:
        return CAPTCHA_LANGUAGE_EN;
    case SESSION_UI_LANGUAGE_ZH:
        return CAPTCHA_LANGUAGE_ZH;
    case SESSION_UI_LANGUAGE_RU:
        return CAPTCHA_LANGUAGE_RU;
    case SESSION_UI_LANGUAGE_JP:
        return CAPTCHA_LANGUAGE_EN;
    case SESSION_UI_LANGUAGE_KO:
    default:
        return CAPTCHA_LANGUAGE_KO;
    }
}

static captcha_language_t
session_captcha_primary_language(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return CAPTCHA_LANGUAGE_KO;
    }

    bool geo_language_enabled =
        ctx->owner != nullptr ? atomic_load(&ctx->owner->geo_language_enabled)
                              : true;

    session_ui_language_t preferred = session_ui_language_current(ctx);
    captcha_language_t preferred_language =
        session_captcha_language_from_ui(preferred);
    if (preferred != SESSION_UI_LANGUAGE_KO ||
        preferred_language != CAPTCHA_LANGUAGE_KO) {
        return preferred_language;
    }

    if (geo_language_enabled) {
        session_ui_language_t geo_language = session_client_geo_language(ctx);
        if (geo_language != SESSION_UI_LANGUAGE_COUNT) {
            return session_captcha_language_from_ui(geo_language);
        }

        char label[64];
        if (session_detect_provider_ip(ctx->client_ip, label, sizeof(label))) {
            if (string_contains_case_insensitive(label, "Chinese")) {
                return CAPTCHA_LANGUAGE_ZH;
            }
            if (string_contains_case_insensitive(label, "Russian")) {
                return CAPTCHA_LANGUAGE_RU;
            }
            if (string_contains_case_insensitive(label, "Korean")) {
                return CAPTCHA_LANGUAGE_KO;
            }
        }
    }

    return CAPTCHA_LANGUAGE_EN;
}

static bool session_captcha_add_language(captcha_language_t *order,
                                         size_t capacity, size_t *count,
                                         bool used[],
                                         captcha_language_t language)
{
    if (order == nullptr || count == nullptr || used == nullptr) {
        return false;
    }

    size_t index = (size_t)language;
    if (index >= CAPTCHA_LANGUAGE_COUNT) {
        return false;
    }

    if (used[index] || *count >= capacity) {
        return false;
    }

    order[*count] = language;
    used[index] = true;
    ++(*count);
    return true;
}

static size_t session_collect_captcha_languages(const session_ctx_t *ctx,
                                                captcha_language_t *order,
                                                size_t capacity)
{
    if (order == nullptr || capacity == 0U) {
        return 0U;
    }

    bool used[CAPTCHA_LANGUAGE_COUNT] = {false};
    size_t count = 0U;

    captcha_language_t primary = session_captcha_primary_language(ctx);
    session_captcha_add_language(order, capacity, &count, used, primary);

    if (ctx != nullptr) {
        captcha_language_t user_pref =
            session_captcha_language_from_ui(session_ui_language_current(ctx));
        session_captcha_add_language(order, capacity, &count, used, user_pref);

        char label[64];
        if (session_detect_provider_ip(ctx->client_ip, label, sizeof(label))) {
            if (string_contains_case_insensitive(label, "Chinese")) {
                session_captcha_add_language(order, capacity, &count, used,
                                             CAPTCHA_LANGUAGE_ZH);
            }
            if (string_contains_case_insensitive(label, "Russian")) {
                session_captcha_add_language(order, capacity, &count, used,
                                             CAPTCHA_LANGUAGE_RU);
            }
            if (string_contains_case_insensitive(label, "Korean")) {
                session_captcha_add_language(order, capacity, &count, used,
                                             CAPTCHA_LANGUAGE_KO);
            }
        }
    }

    session_captcha_add_language(order, capacity, &count, used,
                                 CAPTCHA_LANGUAGE_EN);
    session_captcha_add_language(order, capacity, &count, used,
                                 CAPTCHA_LANGUAGE_KO);

    static const captcha_language_t kFallbackOrder[] = {
        CAPTCHA_LANGUAGE_ZH,
        CAPTCHA_LANGUAGE_RU,
        CAPTCHA_LANGUAGE_EN,
        CAPTCHA_LANGUAGE_KO,
    };

    for (size_t idx = 0U;
         idx < sizeof(kFallbackOrder) / sizeof(kFallbackOrder[0]); ++idx) {
        session_captcha_add_language(order, capacity, &count, used,
                                     kFallbackOrder[idx]);
    }

    return count;
}

static void session_send_captcha_prompt(session_ctx_t *ctx,
                                        const captcha_prompt_t *prompt,
                                        const captcha_language_t *order,
                                        size_t count)
{
    if (ctx == nullptr || prompt == nullptr || order == nullptr ||
        count == 0U) {
        return;
    }

    for (size_t idx = 0U; idx < count; ++idx) {
        captcha_language_t language = order[idx];
        const char *label = session_captcha_label_for_language(language);
        const char *question =
            session_captcha_question_for_language(prompt, language);
        if (label == nullptr || question == nullptr || question[0] == '\0') {
            continue;
        }

        char line[sizeof(prompt->question_en) + 32];
        int written = snprintf(line, sizeof(line), "%s%s", label, question);
        if (written <= 0) {
            continue;
        }

        session_send_system_line(ctx, line);
    }
}

static void host_update_last_captcha_prompt(host_t *host,
                                            const captcha_prompt_t *prompt,
                                            const captcha_language_t *order,
                                            size_t count)
{
    if (host == nullptr || prompt == nullptr) {
        return;
    }

    static const captcha_language_t kDefaultOrder[] = {
        CAPTCHA_LANGUAGE_KO,
        CAPTCHA_LANGUAGE_EN,
        CAPTCHA_LANGUAGE_ZH,
        CAPTCHA_LANGUAGE_RU,
    };

    const captcha_language_t *languages = order;
    size_t language_count = count;
    if (languages == nullptr || language_count == 0U) {
        languages = kDefaultOrder;
        language_count = sizeof(kDefaultOrder) / sizeof(kDefaultOrder[0]);
    }

    char combined_question[sizeof(prompt->question_en) +
                           sizeof(prompt->question_ko) +
                           sizeof(prompt->question_ru) +
                           sizeof(prompt->question_zh) + 64];
    combined_question[0] = '\0';
    size_t combined_length = 0U;

    for (size_t idx = 0U; idx < language_count; ++idx) {
        const char *label = session_captcha_label_for_language(languages[idx]);
        const char *question =
            session_captcha_question_for_language(prompt, languages[idx]);
        if (label == nullptr || question == nullptr || question[0] == '\0') {
            continue;
        }

        char line[sizeof(prompt->question_en) + 32];
        int written = snprintf(line, sizeof(line), "%s%s", label, question);
        if (written <= 0) {
            continue;
        }

        size_t line_length = (size_t)written;
        if (combined_length > 0U &&
            combined_length + 1U < sizeof(combined_question)) {
            combined_question[combined_length++] = '\n';
        }

        if (combined_length >= sizeof(combined_question)) {
            break;
        }

        size_t available = sizeof(combined_question) - combined_length;
        if (available == 0U) {
            break;
        }

        if (line_length >= available) {
            line_length = available - 1U;
        }

        memcpy(combined_question + combined_length, line, line_length);
        combined_length += line_length;
        combined_question[combined_length] = '\0';
    }

    ttak_mutex_lock(&host->lock);
    snprintf(host->last_captcha_question, sizeof(host->last_captcha_question),
             "%s", combined_question);
    snprintf(host->last_captcha_answer, sizeof(host->last_captcha_answer), "%s",
             prompt->answer);
    host->has_last_captcha = host->last_captcha_question[0] != '\0' &&
                             host->last_captcha_answer[0] != '\0';
    if (host->has_last_captcha) {
        if (clock_gettime(CLOCK_REALTIME, &host->last_captcha_generated) != 0) {
            host->last_captcha_generated.tv_sec = time(nullptr);
            host->last_captcha_generated.tv_nsec = 0L;
        }
    } else {
        host->last_captcha_generated.tv_sec = 0;
        host->last_captcha_generated.tv_nsec = 0L;
    }
    ttak_mutex_unlock(&host->lock);
}

static bool session_run_captcha(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return false;
    }

    captcha_prompt_t prompt;
    session_build_captcha_prompt(ctx, &prompt);
    captcha_language_t languages[CAPTCHA_LANGUAGE_COUNT];
    size_t language_count = session_collect_captcha_languages(
        ctx, languages, sizeof(languages) / sizeof(languages[0]));
    if (language_count == 0U) {
        languages[0] = CAPTCHA_LANGUAGE_KO;
        language_count = 1U;
    }

    host_update_last_captcha_prompt(ctx->owner, &prompt, languages,
                                    language_count);

    bool include_chinese = false;
    for (size_t idx = 0U; idx < language_count; ++idx) {
        if (languages[idx] == CAPTCHA_LANGUAGE_ZH) {
            include_chinese = true;
            break;
        }
    }

    session_send_system_line(
        ctx, "For Windows users: CHANGE TERMINAL ENCODING TO UTF-8");
    if (include_chinese) {
        session_send_system_line(
            ctx, "INFO: Chinese question is in Traditional one to "
                 "cover regions those are NOT Mainland China.");
    }
    session_send_system_line(
        ctx, "Before entering the room, solve this small puzzle.");
    session_send_captcha_prompt(ctx, &prompt, languages, language_count);
    session_send_system_line(ctx, "Type your answer and press Enter:");

    char answer[sizeof(prompt.answer)];
    size_t length = 0U;
    while (length + 1U < sizeof(answer)) {
        char ch = '\0';
        const int read_result = session_transport_read(ctx, &ch, 1, -1);
        if (read_result <= 0) {
            return false;
        }

        if (ch == '\r' || ch == '\n') {
            session_local_echo_char(ctx, '\n');
            break;
        }

        if (ch == '\b' || (unsigned char)ch == 0x7fU) {
            if (length > 0U) {
                --length;
                session_send_raw_text(ctx, "\b \b");
            }
            continue;
        }

        if ((unsigned char)ch < 0x20U) {
            continue;
        }

        answer[length++] = ch;
        session_local_echo_char(ctx, ch);
    }
    answer[length] = '\0';
    trim_whitespace_inplace(answer);

    if (answer[0] == '\0') {
        session_send_system_line(ctx, "Captcha answer missing. Disconnecting.");
        return false;
    }

    if (strcasecmp(prompt.answer, "dog") == 0 && strcmp(answer, "개") == 0) {
        snprintf(answer, sizeof(answer), "%s", "dog");
    }

    if (strcasecmp(answer, prompt.answer) == 0) {
        session_send_system_line(ctx, "Captcha solved. Welcome aboard!");
        return true;
    }

    session_send_system_line(ctx, "Captcha failed. Disconnecting.");
    return false;
}

static bool session_is_captcha_exempt(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }

    if (ctx->user.name[0] == '\0') {
        return false;
    }

    char lowered[sizeof(ctx->user.name)];
    size_t idx = 0U;
    for (; idx + 1U < sizeof(lowered) && ctx->user.name[idx] != '\0'; ++idx) {
        lowered[idx] = (char)tolower((unsigned char)ctx->user.name[idx]);
    }
    if (idx < sizeof(lowered)) {
        lowered[idx] = '\0';
    } else {
        lowered[sizeof(lowered) - 1U] = '\0';
    }

    return strcmp(lowered, "gpt") == 0;
}

static void session_print_help(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    const session_ui_locale_t *locale = session_ui_get_locale(ctx);
    const char *prefix = session_command_prefix(ctx);

    char help_buffer[SSH_CHATTER_MESSAGE_LIMIT *
                     32]; // A large enough buffer for help messages
    help_buffer[0] = '\0';

    if (locale->help_title != nullptr && locale->help_title[0] != '\0') {
        session_send_system_line(ctx, locale->help_title);
    }

    session_format_help_entries_to_buffer(ctx, kSessionHelpEssential,
                                          sizeof(kSessionHelpEssential) /
                                              sizeof(kSessionHelpEssential[0]),
                                          help_buffer, sizeof(help_buffer));
    session_send_raw_text(ctx, help_buffer);

    if (locale->help_hint_extra != nullptr &&
        locale->help_hint_extra[0] != '\0') {
        const char *args[] = {prefix};
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        session_format_template(locale->help_hint_extra, args,
                                sizeof(args) / sizeof(args[0]), line,
                                sizeof(line));
        session_send_system_line(ctx, line);
    }

    if (locale->help_scroll_hint != nullptr &&
        locale->help_scroll_hint[0] != '\0') {
        session_send_system_line(ctx, locale->help_scroll_hint);
    }

    if (locale->help_regular_hint != nullptr &&
        locale->help_regular_hint[0] != '\0') {
        session_send_system_line(ctx, locale->help_regular_hint);
    }
}

static bool session_line_is_exit_command(const char *line)
{
    if (line == nullptr) {
        return false;
    }

    if (strncmp(line, "/exit", 5) != 0) {
        return false;
    }

    const char trailing = line[5];
    if (trailing == '\0') {
        return true;
    }

    if (!isspace((unsigned char)trailing)) {
        return false;
    }

    for (size_t idx = 6U; line[idx] != '\0'; ++idx) {
        if (!isspace((unsigned char)line[idx])) {
            return false;
        }
    }

    return true;
}

static void session_handle_username_conflict_input(session_ctx_t *ctx,
                                                   const char *line)
{
    if (ctx == nullptr) {
        return;
    }

    if (session_line_is_exit_command(line)) {
        if (ctx->ops != nullptr && ctx->ops->handle_exit != nullptr) {
            ctx->ops->handle_exit(ctx);
        }
        return;
    }

    char reminder[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(reminder, sizeof(reminder), "The username '%s' is already in use.",
             ctx->user.name);
    session_send_system_line(ctx, reminder);
    session_send_system_line(
        ctx, "Reconnect with a different username by running: ssh "
             "newname@<server> (or ssh -l newname <server>)");
    session_send_system_line(ctx, "Type /exit to quit.");
}

static bool session_prepare_slash_command(const char *input, char *output,
                                          size_t length)
{
    if (input == nullptr || output == nullptr || length == 0U) {
        return false;
    }

    const unsigned char *start = (const unsigned char *)input;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        ++start;
    }

    if (*start != '/') {
        return false;
    }

    const unsigned char *end = start;
    while (*end != '\0') {
        ++end;
    }
    while (end > start && isspace((unsigned char)*(end - 1U))) {
        --end;
    }

    size_t copy_len = (size_t)(end - start);
    if (copy_len == 0U) {
        return false;
    }
    if (copy_len >= length) {
        copy_len = length - 1U;
    }

    memcpy(output, start, copy_len);
    output[copy_len] = '\0';
    return true;
}

static bool session_acquire_cpu_slot(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return true;
    }

    host_t *host = ctx->owner;
    for (;;) {
        bool acquired = false;
        ttak_mutex_lock(&host->lock);
        if (host->cpu_slot_in_use < host->cpu_slot_limit) {
            size_t slot_index = host->cpu_slot_in_use;
            host->cpu_slot_in_use++;
            if (slot_index < 64U) {
                host->cpu_slot_mask |= (1ULL << slot_index);
            }
            acquired = true;
        } else {
            host->cpu_slot_waiting++;
        }
        ttak_mutex_unlock(&host->lock);

        if (acquired) {
            return true;
        }

        session_send_system_line(
            ctx,
            "Server CPU slots are full. Queued and waiting for available slot...");
        const struct timespec wait_time = {.tv_sec = 0, .tv_nsec = 20000000L};
        host_sleep_uninterruptible(&wait_time);

        ttak_mutex_lock(&host->lock);
        if (host->cpu_slot_waiting > 0U) {
            host->cpu_slot_waiting--;
        }
        ttak_mutex_unlock(&host->lock);
    }
}

static void session_release_cpu_slot(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    host_t *host = ctx->owner;
    ttak_mutex_lock(&host->lock);
    if (host->cpu_slot_in_use > 0U) {
        size_t slot_index = host->cpu_slot_in_use - 1U;
        host->cpu_slot_in_use--;
        if (slot_index < 64U) {
            host->cpu_slot_mask &= ~(1ULL << slot_index);
        }
    }
    ttak_mutex_unlock(&host->lock);
}

static const char *session_bbs_subcommand_canonicalize(const session_ctx_t *ctx, const char *command);
static void session_bbs_show_dashboard(session_ctx_t *ctx);
static bool session_bbs_refresh_view(session_ctx_t *ctx);

static const char *bbs_consume_first_word(const char *input, char *token, size_t length)
{
    if (input == nullptr || token == nullptr || length == 0U) {
        return nullptr;
    }
    token[0] = '\0';
    const char *cursor = input;
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    size_t out_idx = 0U;
    while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
        if (out_idx + 1U < length) {
            token[out_idx++] = *cursor;
        }
        ++cursor;
    }
    token[out_idx] = '\0';
    return cursor;
}

static void session_process_line(session_ctx_t *ctx, const char *line)
{
    if (ctx == nullptr || line == nullptr) {
        return;
    }

    char normalized[SSH_CHATTER_MAX_INPUT_LEN];
    snprintf(normalized, sizeof(normalized), "%s", line);

    const bool composing_draft = ctx->bbs_post_pending || ctx->asciiart_pending;

    if (!composing_draft) {
        switch ((int)normalized[0]) {
        // SLASH_COMPATIBLE: slash compatible chars.
        case (int)'.':
        case (int)'_':
        case (int)'@':
        case (int)'$':
        case (int)'*':
        case (int)'-':
        case (int)'#':
        case (int)'>':
            normalized[0] = '/';
            break;
        default:
        }
    }

    if (ctx->bbs_post_pending) {
        session_bbs_capture_body_text(ctx, normalized);
        return;
    }

    if (ctx->asciiart_pending) {
        session_asciiart_capture_text(ctx, normalized);
        return;
    }

    char command_line[SSH_CHATTER_MAX_INPUT_LEN];

    if (ctx->in_bbs_mode && normalized[0] == '\0') {
        if (!session_acquire_cpu_slot(ctx)) {
            return;
        }
        session_bbs_show_dashboard(ctx);
        session_release_cpu_slot(ctx);
        return;
    }

    if (normalized[0] == '\0') {
        return;
    }

    if (!session_acquire_cpu_slot(ctx)) {
        return;
    }

    if (ctx->in_bbs_mode) {
        char command_word[128];
        bbs_consume_first_word(normalized, command_word, sizeof(command_word));
        const char *canonical = session_bbs_subcommand_canonicalize(ctx, command_word);

        if (canonical != nullptr) {
            char bbs_forwarded[SSH_CHATTER_MAX_INPUT_LEN];
            snprintf(bbs_forwarded, sizeof(bbs_forwarded), "/bbs %s", normalized);
            ctx->ops->dispatch_command(ctx, bbs_forwarded);
            session_release_cpu_slot(ctx);
            return;
        }

        if (normalized[0] != '/') {
            session_send_system_line(ctx, "--------------------------------------------------");
            session_send_system_line(ctx, "[BBS] \033[1;33mClassic BBS Mode is active.\033[0m Chat messages cannot be sent here.");
            session_send_system_line(ctx, " - Write a Post:  Type '\033[1;32mpost <title>\033[0m' to start a draft.");
            session_send_system_line(ctx, " - Add a Comment: Type '\033[1;32mcomment <id>|<text>\033[0m' to reply.");
            session_send_system_line(ctx, " - Read a Post:   Type '\033[1;32mread <id>\033[0m' to view content.");
            session_send_system_line(ctx, " - Leave BBS:     Type '\033[1;31mexit\033[0m' to return to general chat.");
            session_send_system_line(ctx, "--------------------------------------------------");
            session_release_cpu_slot(ctx);
            return;
        }
    }

    if (ctx->game.active) {
        if (strcmp(normalized, "/suspend!") == 0) {
            session_game_suspend(ctx, "Game suspended.");
            session_release_cpu_slot(ctx);
            return;
        }

        if (normalized[0] == 't' && normalized[1] == '\0') {
            session_game_toggle_camouflage(ctx);
            session_release_cpu_slot(ctx);
            return;
        }

        if (normalized[0] == '/') {
            session_send_system_line(
                ctx, "Finish the current game with /suspend! first.");
            session_release_cpu_slot(ctx);
            return;
        }

        if (ctx->game.type == SESSION_GAME_TETRIS) {
            session_game_tetris_handle_line(ctx, normalized);
        } else if (ctx->game.type == SESSION_GAME_LIARGAME) {
            session_game_liar_handle_line(ctx, normalized);
        } else if (ctx->game.type == SESSION_GAME_ALPHA) {
            session_game_alpha_handle_line(ctx, normalized);
        } else if (ctx->game.type == SESSION_GAME_OTHELLO) {
            session_game_othello_handle_line(ctx, normalized);
        } else if (ctx->game.type == SESSION_GAME_GONU) {
            session_game_gonu_handle_input(ctx, normalized);
        }
        session_release_cpu_slot(ctx);
        return;
    }

    if (ctx->in_rss_mode) {
        if (strcmp(normalized, "/exit") == 0) {
            session_rss_exit(ctx, nullptr);
        } else {
            const char *rss_args = nullptr;
            if (session_parse_command(normalized, "/rss", &rss_args)) {
                session_rss_exit(ctx, nullptr);
                session_handle_rss(ctx, rss_args);
            } else {
                session_send_system_line(
                    ctx, "RSS reader active. Use /rss exit Ctrl+Z, or "
                         "Terminate to return to chat.");
            }
        }
        session_release_cpu_slot(ctx);
        return;
    }

    bool translation_bypass = false;
    char bypass_buffer[SSH_CHATTER_MAX_INPUT_LEN];
    if (translation_strip_no_translate_prefix(normalized, bypass_buffer,
                                              sizeof(bypass_buffer))) {
        translation_bypass = true;
        snprintf(normalized, sizeof(normalized), "%s", bypass_buffer);
    }

    if (normalized[0] == '\0') {
        return;
    }

    const struct timespec tiny_delay = {.tv_sec = 0, .tv_nsec = 5000000L};
    host_sleep_uninterruptible(&tiny_delay);

    if (ctx->username_conflict) {
        session_handle_username_conflict_input(ctx, normalized);
        session_release_cpu_slot(ctx);
        return;
    }

    if (ctx->ops == nullptr || ctx->ops->dispatch_command == nullptr) {
        session_release_cpu_slot(ctx);
        return;
    }

    if (!translation_bypass) {
        if (session_prepare_slash_command(normalized, command_line,
                                          sizeof(command_line))) {
            if (session_try_localized_command_forward(ctx, command_line)) {
                session_release_cpu_slot(ctx);
                return;
            }
            ctx->ops->dispatch_command(ctx, command_line);
            session_release_cpu_slot(ctx);
            return;
        }
    }

    if (!translation_bypass && normalized[0] == '/') {
        if (session_try_localized_command_forward(ctx, normalized)) {
            session_release_cpu_slot(ctx);
            return;
        }
        ctx->ops->dispatch_command(ctx, normalized);
        session_release_cpu_slot(ctx);
        return;
    }

    const char *trimmed = normalized;
    while (*trimmed == ' ' || *trimmed == '\t') {
        ++trimmed;
    }

    if (!translation_bypass && ctx->input_mode == SESSION_INPUT_MODE_COMMAND &&
        *trimmed != '\0') {
        const char *command_text = trimmed;
        char command_buffer[SSH_CHATTER_MAX_INPUT_LEN];
        if (command_text[0] != '/') {
            command_buffer[0] = '/';
            size_t command_len =
                strnlen(command_text, sizeof(command_buffer) - 2U);
            memcpy(&command_buffer[1], command_text, command_len);
            command_buffer[command_len + 1U] = '\0';
            command_text = command_buffer;
        }
        if (session_prepare_slash_command(command_text, command_buffer,
                                          sizeof(command_buffer))) {
            ctx->ops->dispatch_command(ctx, command_buffer);
        } else {
            ctx->ops->dispatch_command(ctx, command_text);
        }
        session_release_cpu_slot(ctx);
        return;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        now.tv_sec = time(nullptr);
        now.tv_nsec = 0L;
    }

    const bool asciiart_active = ctx->asciiart_pending;
    bool ascii_profile_command = asciiart_active;
    if (!ascii_profile_command && normalized[0] == '/') {
        const char *command_args = nullptr;
        if (session_parse_command(normalized, "/asciiart", &command_args)) {
            ascii_profile_command = true;
        }
    }

    const bool translation_throttle =
        ctx->translation_enabled && ctx->input_translation_enabled &&
        ctx->input_translation_language[0] != '\0';
    const bool chat_throttle = ctx->input_mode == SESSION_INPUT_MODE_CHAT;
    if ((translation_throttle || chat_throttle) && ctx->has_last_message_time) {
        time_t sec_delta = now.tv_sec - ctx->last_message_time.tv_sec;
        long nsec_delta = now.tv_nsec - ctx->last_message_time.tv_nsec;
        if (nsec_delta < 0L) {
            --sec_delta;
            nsec_delta += 1000000000L;
        }
        if (translation_throttle &&
            (sec_delta < 0 || (sec_delta == 0 && nsec_delta < 1000000000L))) {
            session_send_system_line(ctx, "Please wait at least one second "
                                          "before sending another message.");
            session_release_cpu_slot(ctx);
            return;
        }
        if (!translation_throttle && chat_throttle && !ascii_profile_command &&
            !ctx->bracket_paste_active &&
            (sec_delta < 0 || (sec_delta == 0 && nsec_delta < 300000000L))) {
            session_send_system_line(ctx,
                                     "Please wait at least 300 milliseconds "
                                     "before sending another chat message.");
            session_release_cpu_slot(ctx);
            return;
        }
    }

    ctx->last_message_time = now;
    ctx->has_last_message_time = true;

    if (!translation_bypass && ctx->translation_enabled &&
        ctx->input_translation_enabled &&
        ctx->input_translation_language[0] != '\0') {
        if (session_translation_queue_input(ctx, normalized)) {
            session_release_cpu_slot(ctx);
            return;
        }
        session_send_system_line(
            ctx, "Translation unavailable; sending your original message.");
    }

    printf("[%s] %s\n", ctx->user.name, normalized);
    session_deliver_outgoing_message(ctx, normalized, true);
    session_release_cpu_slot(ctx);
}

void host_session_process_line_for_testing(session_ctx_t *ctx, const char *line)
{
    session_process_line(ctx, line);

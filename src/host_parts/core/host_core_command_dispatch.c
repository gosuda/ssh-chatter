session_ui_language_t session_ui_language_from_code(const char *code)
{
    if (code == nullptr || code[0] == '\0') {
        return SESSION_UI_LANGUAGE_COUNT;
    }

    for (size_t idx = 0; idx < SESSION_UI_LANGUAGE_COUNT; ++idx) {
        if (strcasecmp(code, kSessionUiLanguageCodes[idx]) == 0) {
            return (session_ui_language_t)idx;
        }
    }

    return SESSION_UI_LANGUAGE_COUNT;
}

static const char *session_ui_language_code(session_ui_language_t language)
{
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }
    return kSessionUiLanguageCodes[language];
}

static const char *session_ui_language_name(session_ui_language_t language,
                                            session_ui_language_t locale)
{
    if (locale < 0 || locale >= SESSION_UI_LANGUAGE_COUNT) {
        locale = SESSION_UI_LANGUAGE_KO;
    }
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }
    return kSessionUiLanguageNames[locale][language];
}

static const session_ui_locale_t *
session_ui_get_locale(const session_ctx_t *ctx)
{
    session_ui_language_t language = SESSION_UI_LANGUAGE_KO;
    if (ctx != nullptr) {
        language = ctx->ui_language;
    }
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }

    for (size_t idx = 0; idx < SESSION_UI_LANGUAGE_COUNT; ++idx) {
        if (kSessionUiLocales[idx].language == language) {
            return &kSessionUiLocales[idx];
        }
    }

    return &kSessionUiLocales[SESSION_UI_LANGUAGE_KO];
}

static void session_dispatch_command(session_ctx_t *ctx, const char *line);
static void session_handle_mode(session_ctx_t *ctx, const char *arguments);

static const char *session_command_prefix(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return "/";
    }
    return ctx->input_mode == SESSION_INPUT_MODE_COMMAND ? "" : "/";
}

static bool session_try_localized_command_forward(session_ctx_t *ctx,
                                                  const char *line)
{
    if (ctx == nullptr || line == nullptr || ctx->ops == nullptr ||
        ctx->ops->dispatch_command == nullptr || ctx->ops->handle_mode == nullptr) {
        return false;
    }

    const session_ui_locale_t *locale = session_ui_get_locale(ctx);
    const char *command_label =
        (locale != nullptr && locale->mode_label_command != nullptr &&
         locale->mode_label_command[0] != '\0')
            ? locale->mode_label_command
            : "command";

    char localized_prefix[128];
    int prefix_len = snprintf(localized_prefix, sizeof(localized_prefix), "/%s",
                              command_label);
    if (prefix_len <= 0 || (size_t)prefix_len >= sizeof(localized_prefix)) {
        return false;
    }

    if (strncmp(line, localized_prefix, (size_t)prefix_len) != 0) {
        return false;
    }

    const char *remainder = line + prefix_len;
    while (*remainder == ' ' || *remainder == '\t') {
        ++remainder;
    }

    if (*remainder == '\0') {
        ctx->ops->handle_mode(ctx, command_label);
        return true;
    }

    if (*remainder == '/') {
        ctx->ops->dispatch_command(ctx, remainder);
        return true;
    }

    char forwarded[SSH_CHATTER_MAX_INPUT_LEN];
    forwarded[0] = '/';
    size_t copy_len = strnlen(remainder, sizeof(forwarded) - 2U);
    memcpy(&forwarded[1], remainder, copy_len);
    forwarded[copy_len + 1U] = '\0';
    ctx->ops->dispatch_command(ctx, forwarded);
    return true;
}

static session_ui_language_t
session_ui_language_current(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return SESSION_UI_LANGUAGE_KO;
    }
    session_ui_language_t language = ctx->ui_language;
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }
    return language;
}

static const char *
session_asciiart_terminator_for_language(session_ui_language_t language)
{
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }
    const char *terminator = kSessionAsciiartTerminators[language];
    if (terminator != nullptr && terminator[0] != '\0') {
        return terminator;
    }
    const char *fallback = kSessionAsciiartTerminators[SESSION_UI_LANGUAGE_KO];
    return (fallback != nullptr && fallback[0] != '\0')
               ? fallback
               : SSH_CHATTER_ASCIIART_TERMINATOR_EN;
}

static const char *
session_bbs_terminator_for_language(session_ui_language_t language)
{
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }
    const char *terminator = kSessionBbsTerminators[language];
    if (terminator != nullptr && terminator[0] != '\0') {
        return terminator;
    }
    const char *fallback = kSessionBbsTerminators[SESSION_UI_LANGUAGE_KO];
    return (fallback != nullptr && fallback[0] != '\0')
               ? fallback
               : SSH_CHATTER_BBS_TERMINATOR_EN;
}

static const char *session_asciiart_terminator(const session_ctx_t *ctx)
{
    return session_asciiart_terminator_for_language(
        session_ui_language_current(ctx));
}

static const char *session_bbs_terminator(const session_ctx_t *ctx)
{
    return session_bbs_terminator_for_language(
        session_ui_language_current(ctx));
}

static const char *session_editor_terminator(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return SSH_CHATTER_BBS_TERMINATOR_EN;
    }

    if (ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART) {
        return session_asciiart_terminator(ctx);
    }

    return session_bbs_terminator(ctx);
}

static bool session_asciiart_matches_terminator(const char *line)
{
    if (line == nullptr) {
        return false;
    }
    for (size_t idx = 0; idx < SESSION_UI_LANGUAGE_COUNT; ++idx) {
        const char *terminator = session_asciiart_terminator_for_language(
            (session_ui_language_t)idx);
        if (terminator != nullptr && strcmp(line, terminator) == 0) {
            return true;
        }
    }
    return false;
}

static bool session_bbs_matches_terminator(const char *line)
{
    if (line == nullptr) {
        return false;
    }
    for (size_t idx = 0; idx < SESSION_UI_LANGUAGE_COUNT; ++idx) {
        const char *terminator =
            session_bbs_terminator_for_language((session_ui_language_t)idx);
        if (terminator != nullptr && strcmp(line, terminator) == 0) {
            return true;
        }
    }
    return false;
}

static const char *
session_command_alias_preferred_by_canonical(const session_ctx_t *ctx,
                                             const char *canonical);

static const session_bbs_subcommand_alias_t *
session_bbs_subcommand_lookup(const char *canonical)
{
    if (canonical == nullptr || canonical[0] == '\0') {
        return nullptr;
    }
    for (size_t idx = 0; idx < kSessionBbsSubcommandCount; ++idx) {
        if (strcmp(kSessionBbsSubcommands[idx].canonical, canonical) == 0) {
            return &kSessionBbsSubcommands[idx];
        }
    }
    return nullptr;
}

static const char *
session_bbs_subcommand_localized(const session_bbs_subcommand_alias_t *alias,
                                 session_ui_language_t language)
{
    if (alias == nullptr) {
        return nullptr;
    }
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }
    const char *localized = alias->localized[language];
    if (localized != nullptr && localized[0] != '\0') {
        return localized;
    }
    return nullptr;
}

static const char *session_bbs_subcommand_preferred(const session_ctx_t *ctx,
                                                    const char *canonical)
{
    const session_bbs_subcommand_alias_t *alias =
        session_bbs_subcommand_lookup(canonical);
    if (alias == nullptr) {
        return canonical;
    }
    const char *localized = session_bbs_subcommand_localized(
        alias, session_ui_language_current(ctx));
    if (localized != nullptr) {
        return localized;
    }
    return alias->canonical;
}

static const char *session_bbs_subcommand_canonicalize(const session_ctx_t *ctx,
                                                       const char *command)
{
    if (command == nullptr || command[0] == '\0') {
        return nullptr;
    }

    for (size_t idx = 0; idx < kSessionBbsSubcommandCount; ++idx) {
        if (strcmp(command, kSessionBbsSubcommands[idx].canonical) == 0) {
            return kSessionBbsSubcommands[idx].canonical;
        }
    }

    session_ui_language_t language = session_ui_language_current(ctx);
    for (size_t idx = 0; idx < kSessionBbsSubcommandCount; ++idx) {
        const char *localized = session_bbs_subcommand_localized(
            &kSessionBbsSubcommands[idx], language);
        if (localized != nullptr && strcmp(command, localized) == 0) {
            return kSessionBbsSubcommands[idx].canonical;
        }
    }

    for (size_t idx = 0; idx < kSessionBbsSubcommandCount; ++idx) {
        for (size_t lang = 0; lang < SESSION_UI_LANGUAGE_COUNT; ++lang) {
            const char *localized = session_bbs_subcommand_localized(
                &kSessionBbsSubcommands[idx], (session_ui_language_t)lang);
            if (localized != nullptr && strcmp(command, localized) == 0) {
                return kSessionBbsSubcommands[idx].canonical;
            }
        }
    }

    return nullptr;
}

static void session_bbs_format_usage(session_ctx_t *ctx, const char *canonical,
                                     const char *arguments, char *buffer,
                                     size_t length)
{
    if (buffer == nullptr || length == 0U) {
        return;
    }

    buffer[0] = '\0';
    const char *bbs_command =
        session_command_alias_preferred_by_canonical(ctx, "/bbs");
    if (bbs_command == nullptr || bbs_command[0] == '\0') {
        bbs_command = "/bbs";
    }

    const char *subcommand =
        canonical != nullptr ? session_bbs_subcommand_preferred(ctx, canonical)
                             : nullptr;
    if (subcommand == nullptr || subcommand[0] == '\0') {
        subcommand = canonical != nullptr ? canonical : "";
    }

    const char *args = arguments != nullptr ? arguments : "";
    const char *separator = args[0] != '\0' ? " " : "";

    snprintf(buffer, length, "Usage: %s %s%s%s", bbs_command, subcommand,
             separator, args);
}

static void session_bbs_send_usage(session_ctx_t *ctx, const char *canonical,
                                   const char *arguments)
{
    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_bbs_format_usage(ctx, canonical, arguments, usage, sizeof(usage));
    session_send_system_line(ctx, usage);
}

static const session_command_alias_t *
session_command_alias_lookup(const char *canonical)
{
    if (canonical == nullptr || canonical[0] == '\0') {
        return nullptr;
    }
    for (size_t idx = 0; idx < kSessionCommandAliasCount; ++idx) {
        if (strcmp(kSessionCommandAliases[idx].canonical, canonical) == 0) {
            return &kSessionCommandAliases[idx];
        }
    }
    return nullptr;
}

static const char *
session_command_alias_for_language(const session_command_alias_t *alias,
                                   session_ui_language_t language)
{
    if (alias == nullptr) {
        return nullptr;
    }
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }
    const char *localized = alias->localized[language];
    if (localized != nullptr && localized[0] != '\0') {
        return localized;
    }
    return alias->canonical;
}

static const char *
session_command_alias_preferred(const session_ctx_t *ctx,
                                const session_command_alias_t *alias)
{
    return session_command_alias_for_language(alias,
                                              session_ui_language_current(ctx));
}

static const char *
session_command_alias_preferred_by_canonical(const session_ctx_t *ctx,
                                             const char *canonical)
{
    const session_command_alias_t *alias =
        session_command_alias_lookup(canonical);
    if (alias == nullptr) {
        return canonical;
    }
    return session_command_alias_preferred(ctx, alias);
}

static bool session_parse_command(const char *line, const char *command,
                                  const char **arguments);
static bool
session_parse_localized_command(session_ctx_t *ctx,
                                const session_command_alias_t *alias,
                                const char *line, const char **arguments);

static bool session_parse_command_any(session_ctx_t *ctx, const char *canonical,
                                      const char *line, const char **arguments)
{
    if (canonical == nullptr) {
        return false;
    }
    const session_command_alias_t *alias =
        session_command_alias_lookup(canonical);
    if (alias != nullptr) {
        return session_parse_localized_command(ctx, alias, line, arguments);
    }
    return session_parse_command(line, canonical, arguments);
}

static void session_command_collect_localized_matches(session_ctx_t *ctx,
                                                      const char *prefix,
                                                      const char **matches,
                                                      size_t *match_count,
                                                      size_t max_count)
{
    if (ctx == nullptr || matches == nullptr || match_count == nullptr) {
        return;
    }

    size_t prefix_len = prefix != nullptr ? strlen(prefix) : 0U;

    for (size_t idx = 0; idx < kSessionCommandAliasCount; ++idx) {
        const session_command_alias_t *alias = &kSessionCommandAliases[idx];
        const char *localized = session_command_alias_preferred(ctx, alias);
        if (localized == nullptr || localized[0] == '\0') {
            continue;
        }
        if (strcmp(localized, alias->canonical) == 0) {
            continue;
        }

        const char *name = localized[0] == '/' ? localized + 1 : localized;
        if (name[0] == '\0') {
            continue;
        }

        if (prefix_len > 0U && strncasecmp(name, prefix, prefix_len) != 0) {
            continue;
        }

        bool duplicate = false;
        for (size_t existing = 0; existing < *match_count; ++existing) {
            if (strcmp(matches[existing], name) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        if (*match_count >= max_count) {
            break;
        }
        matches[(*match_count)++] = name;
    }
}

static void session_command_format_usage(session_ctx_t *ctx,
                                         const char *canonical,
                                         const char *fallback, char *buffer,
                                         size_t length)
{
    if (buffer == nullptr || length == 0U) {
        return;
    }

    buffer[0] = '\0';
    if (fallback == nullptr) {
        return;
    }

    if (canonical == nullptr || canonical[0] == '\0') {
        snprintf(buffer, length, "%s", fallback);
        return;
    }

    const char *alias =
        session_command_alias_preferred_by_canonical(ctx, canonical);
    if (alias == nullptr || alias[0] == '\0') {
        alias = canonical;
    }

    const char *prefix = session_command_prefix(ctx);
    if (prefix == nullptr) {
        prefix = "";
    }

    const char *alias_body = alias;
    if (alias_body != nullptr && alias_body[0] == '/') {
        ++alias_body;
    }

    const char *canonical_body = canonical;
    if (canonical_body[0] == '/') {
        ++canonical_body;
    }

    if (alias_body == nullptr || alias_body[0] == '\0') {
        alias_body = canonical_body;
    }

    char replacement[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(replacement, sizeof(replacement), "%s%s", prefix,
             alias_body != nullptr ? alias_body : "");

    const char *source = fallback;
    size_t canonical_len = strlen(canonical);
    size_t out_index = 0U;
    bool replaced = false;

    for (size_t idx = 0U; source[idx] != '\0' && out_index + 1U < length;) {
        if (canonical_len > 0U &&
            strncmp(source + idx, canonical, canonical_len) == 0) {
            size_t repl_len = strnlen(replacement, length - out_index - 1U);
            memcpy(buffer + out_index, replacement, repl_len);
            out_index += repl_len;
            idx += canonical_len;
            replaced = true;
            continue;
        }
        buffer[out_index++] = source[idx++];
    }
    buffer[out_index] = '\0';

    if (replaced) {
        return;
    }

    size_t body_len = canonical_body != nullptr ? strlen(canonical_body) : 0U;
    if (body_len == 0U) {
        snprintf(buffer, length, "%s", fallback);
        return;
    }

    out_index = 0U;
    bool body_replaced = false;
    for (size_t idx = 0U; source[idx] != '\0' && out_index + 1U < length;) {
        if (strncmp(source + idx, canonical_body, body_len) == 0) {
            size_t repl_len = strnlen(replacement, length - out_index - 1U);
            memcpy(buffer + out_index, replacement, repl_len);
            out_index += repl_len;
            idx += body_len;
            body_replaced = true;
            continue;
        }
        buffer[out_index++] = source[idx++];
    }
    buffer[out_index] = '\0';

    if (!body_replaced) {
        snprintf(buffer, length, "%s", fallback);
    }
}


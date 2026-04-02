static void __attribute__((unused))
session_handle_gameopt(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    static const char *kUsage = "Usage: /gameopt <reset>";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/gameopt", kUsage, usage, sizeof(usage));

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

    if (strcasecmp(working, "reset") == 0) {
        snprintf(ctx->game.chosen_camouflage_language,
                 sizeof(ctx->game.chosen_camouflage_language), "c");
        if (ctx->owner != nullptr) {
            ttak_mutex_lock(&ctx->owner->lock);
            user_preference_t *pref =
                host_ensure_preference_locked(ctx->owner, ctx->user.name, "");
            if (pref != nullptr) {
                snprintf(pref->camouflage_language,
                         sizeof(pref->camouflage_language), "c");
                host_state_save_locked(ctx->owner);
            }
            ttak_mutex_unlock(&ctx->owner->lock);
        }
        session_send_system_line(
            ctx, "Game options reset. Camouflage language set to default (C).");
        return;
    }

    session_send_system_line(ctx, usage);
}

static void session_handle_advanced(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    bool is_full_operator = ctx->user.is_operator;
    bool is_lan_operator = ctx->user.is_lan_operator;

    char delegated_buffer[SSH_CHATTER_MESSAGE_LIMIT];
    if (arguments != nullptr) {
        snprintf(delegated_buffer, sizeof(delegated_buffer), "%s", arguments);
        trim_whitespace_inplace(delegated_buffer);

        if (delegated_buffer[0] != '\0') {
            char token[64];
            const char *remaining =
                session_consume_token(delegated_buffer, token, sizeof(token));
            if (strcasecmp(token, "telnet-server") == 0) {
                char forwarded[SSH_CHATTER_MESSAGE_LIMIT];
                if (remaining != nullptr) {
                    snprintf(forwarded, sizeof(forwarded), "%s", remaining);
                    trim_whitespace_inplace(forwarded);
                } else {
                    forwarded[0] = '\0';
                }

                session_send_system_line(
                    ctx, "Tip: use /telnet-server directly "
                         "for Telnet/Fidonet integration controls.");

                return;
            }

            session_send_system_line(
                ctx,
                "Unknown advanced topic. Showing available commands instead.");
        }
    }

    const session_ui_locale_t *locale = session_ui_get_locale(ctx);
    const char *prefix = session_command_prefix(ctx);
    char help_buffer[SSH_CHATTER_MESSAGE_LIMIT * 32];

    if (locale != nullptr && locale->help_extra_title != nullptr &&
        locale->help_extra_title[0] != '\0') {
        session_send_system_line(ctx, locale->help_extra_title);
    }

    help_buffer[0] = '\0';
    session_format_help_entries_to_buffer(ctx, kSessionHelpExtended,
                                          sizeof(kSessionHelpExtended) /
                                              sizeof(kSessionHelpExtended[0]),
                                          help_buffer, sizeof(help_buffer));
    session_send_raw_text(ctx, help_buffer);

    if (locale != nullptr && locale->help_extra_hint != nullptr &&
        locale->help_extra_hint[0] != '\0') {
        const char *args[] = {prefix};
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        session_format_template(locale->help_extra_hint, args,
                                sizeof(args) / sizeof(args[0]), line,
                                sizeof(line));
        session_send_system_line(ctx, line);
    }

    if (is_full_operator) {
        if (locale != nullptr && locale->help_operator_title != nullptr &&
            locale->help_operator_title[0] != '\0') {
            session_send_system_line(ctx, locale->help_operator_title);
        }

        help_buffer[0] = '\0';
        session_format_help_entries_to_buffer(
            ctx, kSessionHelpOperator,
            sizeof(kSessionHelpOperator) / sizeof(kSessionHelpOperator[0]),
            help_buffer, sizeof(help_buffer));
        session_send_raw_text(ctx, help_buffer);
    } else if (is_lan_operator) {
        session_send_system_line(
            ctx, "LAN operator mode active: full administrator commands are "
                 "hidden.");
    } else {
        session_send_system_line(
            ctx,
            "Operator-only integrations are hidden. Request access if needed.");
    }
}

// Format a timestamp for BBS displays in a compact form.

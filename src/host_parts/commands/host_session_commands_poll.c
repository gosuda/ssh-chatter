static bool session_poll_parse_fields(const char *input, char *question,
                                      size_t question_length,
                                      char options[][SSH_CHATTER_MESSAGE_LIMIT],
                                      size_t max_options, size_t *option_count,
                                      char *error, size_t error_length)
{
    if (question != nullptr && question_length > 0U) {
        question[0] = '\0';
    }
    if (option_count != nullptr) {
        *option_count = 0U;
    }
    if (error != nullptr && error_length > 0U) {
        error[0] = '\0';
    }
    if (input == nullptr || question == nullptr || question_length == 0U ||
        options == nullptr || max_options == 0U || option_count == nullptr) {
        return false;
    }

    char working[SSH_CHATTER_MAX_INPUT_LEN];
    snprintf(working, sizeof(working), "%s", input);

    char *saveptr = nullptr;
    char *token = strtok_r(working, "|", &saveptr);
    if (token == nullptr) {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length,
                     "Provide a question and at least two options separated "
                     "by '|'.");
        }
        return false;
    }

    trim_whitespace_inplace(token);
    if (token[0] == '\0') {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length,
                     "Provide a question and at least two options separated "
                     "by '|'.");
        }
        return false;
    }
    snprintf(question, question_length, "%s", token);

    size_t count = 0U;
    while ((token = strtok_r(nullptr, "|", &saveptr)) != nullptr) {
        if (count >= max_options) {
            if (error != nullptr && error_length > 0U) {
                snprintf(error, error_length,
                         "You may only provide up to %zu options.",
                         max_options);
            }
            return false;
        }
        trim_whitespace_inplace(token);
        if (token[0] == '\0') {
            if (error != nullptr && error_length > 0U) {
                snprintf(error, error_length,
                         "Poll options may not be empty.");
            }
            return false;
        }
        snprintf(options[count], SSH_CHATTER_MESSAGE_LIMIT, "%s", token);
        ++count;
    }

    if (count < 2U) {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length,
                     "Provide at least two options separated by '|'.");
        }
        return false;
    }

    *option_count = count;
    return true;
}

static void session_handle_poll(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    static const char *kUsage =
        "Usage: /poll [close|<question>|<option1>|<option2>|<option3>|...]";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/poll", kUsage, usage, sizeof(usage));

    char working[SSH_CHATTER_MAX_INPUT_LEN];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
    }
    trim_whitespace_inplace(working);

    if (working[0] == '\0' || strcasecmp(working, "list") == 0 ||
        strcasecmp(working, "status") == 0) {
        session_send_poll_summary(ctx);
        return;
    }

    if (strcasecmp(working, "close") == 0 || strcasecmp(working, "end") == 0 ||
        strcasecmp(working, "stop") == 0 || strcasecmp(working, "off") == 0) {
        if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
            session_send_system_line(
                ctx, "Only operators may close global polls.");
            return;
        }

        bool was_active = false;
        ttak_mutex_lock(&ctx->owner->lock);
        if (ctx->owner->poll.active) {
            ctx->owner->poll.active = false;
            was_active = true;
            host_vote_state_save_locked(ctx->owner);
        }
        ttak_mutex_unlock(&ctx->owner->lock);

        if (!was_active) {
            session_send_system_line(ctx, "No active poll to close.");
            return;
        }

        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice), "* [%s] closed the main poll.",
                 ctx->user.name);
        host_history_record_system(ctx->owner, notice, nullptr);
        chat_room_broadcast(&ctx->owner->room, notice, nullptr);
        session_send_system_line(ctx, "Poll closed.");
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(ctx,
                                 "Only operators may start global polls.");
        return;
    }

    char question[SSH_CHATTER_MESSAGE_LIMIT];
    char options[5][SSH_CHATTER_MESSAGE_LIMIT];
    enum {
        SESSION_POLL_TEXT_PREC = SSH_CHATTER_MESSAGE_LIMIT - 1,
        SESSION_POLL_NOTICE_USER_PREC = SSH_CHATTER_USERNAME_LEN - 1,
        SESSION_POLL_NOTICE_QUESTION_PREC = SSH_CHATTER_MESSAGE_LIMIT / 2
    };
    size_t option_count = 0U;
    char error[128];
    if (!session_poll_parse_fields(working, question, sizeof(question), options,
                                   sizeof(options) / sizeof(options[0]),
                                   &option_count, error, sizeof(error))) {
        if (error[0] != '\0') {
            session_send_system_line(ctx, error);
        } else {
            session_send_system_line(ctx, usage);
        }
        return;
    }

    poll_state_t snapshot = {0};
    ttak_mutex_lock(&ctx->owner->lock);
    uint64_t next_id = ctx->owner->poll.id + 1U;
    poll_state_reset(&ctx->owner->poll);
    ctx->owner->poll.active = true;
    ctx->owner->poll.allow_multiple = false;
    ctx->owner->poll.id = next_id == 0U ? 1U : next_id;
    ctx->owner->poll.option_count = option_count;
    snprintf(ctx->owner->poll.question, sizeof(ctx->owner->poll.question),
             "%.*s", SESSION_POLL_TEXT_PREC, question);
    for (size_t idx = 0U; idx < option_count; ++idx) {
        snprintf(ctx->owner->poll.options[idx].text,
                 sizeof(ctx->owner->poll.options[idx].text), "%.*s",
                 SESSION_POLL_TEXT_PREC, options[idx]);
        ctx->owner->poll.options[idx].votes = 0U;
    }
    host_vote_state_save_locked(ctx->owner);
    snapshot = ctx->owner->poll;
    ttak_mutex_unlock(&ctx->owner->lock);

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice), "* [%.*s] started a poll: %.*s",
             SESSION_POLL_NOTICE_USER_PREC, ctx->user.name,
             SESSION_POLL_NOTICE_QUESTION_PREC, question);
    host_history_record_system(ctx->owner, notice, nullptr);
    chat_room_broadcast(&ctx->owner->room, notice, nullptr);
    session_send_poll_summary_generic(ctx, &snapshot, nullptr);
}

static void session_handle_vote(session_ctx_t *ctx, size_t option_index)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    char response[SSH_CHATTER_MESSAGE_LIMIT];
    response[0] = '\0';

    ttak_mutex_lock(&ctx->owner->lock);
    poll_state_t *poll = &ctx->owner->poll;
    if (!poll->active || poll->option_count == 0U) {
        ttak_mutex_unlock(&ctx->owner->lock);
        session_send_system_line(ctx, "No active poll right now.");
        return;
    }

    if (option_index >= poll->option_count) {
        ttak_mutex_unlock(&ctx->owner->lock);
        session_send_system_line(ctx, "That poll option does not exist.");
        return;
    }

    user_preference_t *pref =
        host_ensure_preference_locked(ctx->owner, ctx->user.name,
                                      ctx->client_ip);
    if (pref == nullptr) {
        ttak_mutex_unlock(&ctx->owner->lock);
        session_send_system_line(ctx, "Unable to record your vote.");
        return;
    }

    if (pref->last_poll_id != poll->id) {
        pref->last_poll_id = poll->id;
        pref->last_poll_choice = -1;
    }

    if (poll->allow_multiple) {
        uint32_t mask =
            pref->last_poll_choice < 0 ? 0U : (uint32_t)pref->last_poll_choice;
        uint32_t bit = 1U << option_index;
        if ((mask & bit) != 0U) {
            mask &= ~bit;
            if (poll->options[option_index].votes > 0U) {
                poll->options[option_index].votes--;
            }
            snprintf(response, sizeof(response),
                     "Removed your vote for option %zu.", option_index + 1U);
        } else {
            mask |= bit;
            poll->options[option_index].votes++;
            snprintf(response, sizeof(response),
                     "Vote recorded for option %zu.", option_index + 1U);
        }
        pref->last_poll_choice =
            mask == 0U ? -1 : (int32_t)mask;
    } else {
        int32_t previous = pref->last_poll_choice;
        if (previous == (int32_t)option_index) {
            ttak_mutex_unlock(&ctx->owner->lock);
            session_send_system_line(ctx,
                                     "You have already voted for that option.");
            return;
        }
        if (previous >= 0 && (size_t)previous < poll->option_count &&
            poll->options[previous].votes > 0U) {
            poll->options[previous].votes--;
        }
        poll->options[option_index].votes++;
        pref->last_poll_choice = (int32_t)option_index;
        snprintf(response, sizeof(response), "Vote recorded for option %zu.",
                 option_index + 1U);
    }

    pref->last_poll_id = poll->id;
    host_vote_state_save_locked(ctx->owner);
    host_state_save_locked(ctx->owner);
    ttak_mutex_unlock(&ctx->owner->lock);

    if (response[0] != '\0') {
        session_send_system_line(ctx, response);
    }
}

// Record a vote in a named poll, ensuring a user can move their vote between options.
static void session_handle_named_vote(session_ctx_t *ctx, size_t option_index,
                                      const char *label)
{
    if (ctx == nullptr || ctx->owner == nullptr || label == nullptr ||
        label[0] == '\0') {
        return;
    }

    char normalized_label[SSH_CHATTER_POLL_LABEL_LEN];
    snprintf(normalized_label, sizeof(normalized_label), "%s", label);
    trim_whitespace_inplace(normalized_label);
    if (!poll_label_is_valid(normalized_label)) {
        session_send_system_line(
            ctx, "Poll labels may only contain letters, numbers, '-' or '_'.");
        return;
    }

    char response[SSH_CHATTER_MESSAGE_LIMIT];
    response[0] = '\0';

    ttak_mutex_lock(&ctx->owner->lock);
    named_poll_state_t *poll =
        host_find_named_poll_locked(ctx->owner, normalized_label);
    if (poll == nullptr || !poll->poll.active ||
        poll->poll.option_count == 0U) {
        ttak_mutex_unlock(&ctx->owner->lock);
        session_send_system_line(ctx, "That poll is not active.");
        return;
    }

    if (option_index >= poll->poll.option_count) {
        ttak_mutex_unlock(&ctx->owner->lock);
        session_send_system_line(ctx, "That poll option does not exist.");
        return;
    }

    int voter_index = -1;
    for (size_t idx = 0U; idx < poll->voter_count; ++idx) {
        if (strcasecmp(poll->voters[idx].username, ctx->user.name) == 0) {
            voter_index = (int)idx;
            break;
        }
    }

    if (poll->poll.allow_multiple) {
        uint32_t bit = 1U << option_index;
        if (voter_index >= 0) {
            uint32_t mask = poll->voters[voter_index].choices_mask;
            if ((mask & bit) != 0U) {
                mask &= ~bit;
                if (poll->poll.options[option_index].votes > 0U) {
                    poll->poll.options[option_index].votes--;
                }
                if (mask == 0U) {
                    size_t remove_index = (size_t)voter_index;
                    if (remove_index + 1U < poll->voter_count) {
                        memmove(&poll->voters[remove_index],
                                &poll->voters[remove_index + 1U],
                                (poll->voter_count - remove_index - 1U) *
                                    sizeof(poll->voters[0]));
                    }
                    poll->voter_count--;
                    if (poll->voter_count < SSH_CHATTER_MAX_NAMED_VOTERS) {
                        memset(&poll->voters[poll->voter_count], 0,
                               sizeof(poll->voters[0]));
                    }
                } else {
                    poll->voters[voter_index].choices_mask = mask;
                    poll->voters[voter_index].choice = (int)option_index;
                }
                snprintf(response, sizeof(response),
                         "Removed your vote for option %zu.",
                         option_index + 1U);
            } else {
                mask |= bit;
                poll->poll.options[option_index].votes++;
                poll->voters[voter_index].choices_mask = mask;
                poll->voters[voter_index].choice = (int)option_index;
                snprintf(response, sizeof(response),
                         "Vote recorded for option %zu.", option_index + 1U);
            }
        } else {
            if (poll->voter_count >= SSH_CHATTER_MAX_NAMED_VOTERS) {
                ttak_mutex_unlock(&ctx->owner->lock);
                session_send_system_line(ctx,
                                         "That poll has reached "
                                         "its voter limit.");
                return;
            }
            poll->poll.options[option_index].votes++;
            named_poll_state_t *target = poll;
            size_t insert_at = target->voter_count++;
            snprintf(target->voters[insert_at].username,
                     sizeof(target->voters[insert_at].username), "%s",
                     ctx->user.name);
            target->voters[insert_at].choice = (int)option_index;
            target->voters[insert_at].choices_mask = bit;
            snprintf(response, sizeof(response),
                     "Vote recorded for option %zu.", option_index + 1U);
        }
    } else {
        if (voter_index >= 0) {
            int previous = poll->voters[voter_index].choice;
            if (previous == (int)option_index) {
                ttak_mutex_unlock(&ctx->owner->lock);
                session_send_system_line(
                    ctx, "You have already voted for that option.");
                return;
            }
            if (previous >= 0 &&
                (size_t)previous < poll->poll.option_count &&
                poll->poll.options[previous].votes > 0U) {
                poll->poll.options[previous].votes--;
            }
            poll->poll.options[option_index].votes++;
            poll->voters[voter_index].choice = (int)option_index;
            poll->voters[voter_index].choices_mask = 0U;
            snprintf(response, sizeof(response),
                     "Vote recorded for option %zu.", option_index + 1U);
        } else {
            if (poll->voter_count >= SSH_CHATTER_MAX_NAMED_VOTERS) {
                ttak_mutex_unlock(&ctx->owner->lock);
                session_send_system_line(ctx,
                                         "That poll has reached "
                                         "its voter limit.");
                return;
            }
            poll->poll.options[option_index].votes++;
            size_t insert_at = poll->voter_count++;
            snprintf(poll->voters[insert_at].username,
                     sizeof(poll->voters[insert_at].username), "%s",
                     ctx->user.name);
            poll->voters[insert_at].choice = (int)option_index;
            poll->voters[insert_at].choices_mask = 0U;
            snprintf(response, sizeof(response),
                     "Vote recorded for option %zu.", option_index + 1U);
        }
    }

    host_vote_state_save_locked(ctx->owner);
    ttak_mutex_unlock(&ctx->owner->lock);

    if (response[0] != '\0') {
        session_send_system_line(ctx, response);
    }
}

// Allow voting in a named poll by specifying the label and desired choice directly.
static void session_handle_elect_command(session_ctx_t *ctx,
                                         const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    static const char *kUsage = "Usage: /elect <label> <choice>";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/elect", kUsage, usage, sizeof(usage));

    char working[SSH_CHATTER_MAX_INPUT_LEN];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
    }
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    char *saveptr = nullptr;
    char *label = strtok_r(working, " \t", &saveptr);
    char *choice_text = strtok_r(nullptr, " \t", &saveptr);
    if (label == nullptr || choice_text == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    trim_whitespace_inplace(label);
    if (!poll_label_is_valid(label)) {
        session_send_system_line(
            ctx, "Poll labels may only contain letters, numbers, '-' or '_'.");
        return;
    }

    char *endptr = nullptr;
    unsigned long choice = strtoul(choice_text, &endptr, 10);
    if (endptr == choice_text || choice == 0U) {
        session_send_system_line(ctx, usage);
        return;
    }

    session_handle_named_vote(ctx, (size_t)(choice - 1U), label);
}

// Parse the /vote command to manage named polls, including listing, creation, and closure.
static void session_handle_vote_command(session_ctx_t *ctx,
                                        const char *arguments,
                                        bool allow_multiple)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    const char *canonical = allow_multiple ? "/vote" : "/vote-single";
    const char *kUsage =
        "Usage: /vote <label> [close|<question>|<option1>|<option2>|...]";
    const char *kUsageSingle =
        "Usage: /vote-single <label> [close|<question>|<option1>|<option2>|...]";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, canonical,
                                 allow_multiple ? kUsage : kUsageSingle, usage,
                                 sizeof(usage));

    char working[SSH_CHATTER_MAX_INPUT_LEN];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
    }
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_list_named_polls(ctx);
        return;
    }

    char *saveptr = nullptr;
    char *label = strtok_r(working, " \t", &saveptr);
    if (label == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    if (strcasecmp(label, "list") == 0) {
        session_list_named_polls(ctx);
        return;
    }

    trim_whitespace_inplace(label);
    if (!poll_label_is_valid(label)) {
        session_send_system_line(
            ctx, "Poll labels may only contain letters, numbers, '-' or '_'.");
        return;
    }

    char remainder[SSH_CHATTER_MAX_INPUT_LEN];
    if (saveptr == nullptr) {
        remainder[0] = '\0';
    } else {
        snprintf(remainder, sizeof(remainder), "%s", saveptr);
    }
    trim_whitespace_inplace(remainder);

    if (remainder[0] == '\0') {
        named_poll_state_t snapshot = {0};
        bool found = false;
        ttak_mutex_lock(&ctx->owner->lock);
        named_poll_state_t *poll =
            host_find_named_poll_locked(ctx->owner, label);
        if (poll != nullptr) {
            snapshot = *poll;
            found = true;
        }
        ttak_mutex_unlock(&ctx->owner->lock);

        if (!found) {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message), "No poll found for label '%s'.",
                     label);
            session_send_system_line(ctx, message);
            return;
        }

        session_send_poll_summary_generic(ctx, &snapshot.poll, snapshot.label);
        return;
    }

    if (strcasecmp(remainder, "close") == 0 ||
        strcasecmp(remainder, "end") == 0 ||
        strcasecmp(remainder, "stop") == 0 ||
        strcasecmp(remainder, "off") == 0) {
        bool closed = false;
        bool allowed = false;
        bool found = false;
        ttak_mutex_lock(&ctx->owner->lock);
        named_poll_state_t *poll =
            host_find_named_poll_locked(ctx->owner, label);
        if (poll != nullptr) {
            found = true;
            allowed = ctx->user.is_operator || ctx->user.is_lan_operator ||
                      strcasecmp(poll->owner, ctx->user.name) == 0;
            if (allowed && poll->poll.active) {
                poll->poll.active = false;
                closed = true;
                host_recount_named_polls_locked(ctx->owner);
                host_vote_state_save_locked(ctx->owner);
            }
        }
        ttak_mutex_unlock(&ctx->owner->lock);

        if (!found) {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message), "No poll found for label '%s'.",
                     label);
            session_send_system_line(ctx, message);
            return;
        }

        if (!allowed) {
            session_send_system_line(
                ctx,
                "Only the poll owner or an operator may close this poll.");
            return;
        }

        if (!closed) {
            session_send_system_line(ctx, "That poll is not active.");
            return;
        }

        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice), "* [%s] closed poll [%s].",
                 ctx->user.name, label);
        host_history_record_system(ctx->owner, notice, nullptr);
        chat_room_broadcast(&ctx->owner->room, notice, nullptr);
        session_send_system_line(ctx, "Poll closed.");
        return;
    }

    char question[SSH_CHATTER_MESSAGE_LIMIT];
    char options[5][SSH_CHATTER_MESSAGE_LIMIT];
    enum { SESSION_NAMED_POLL_TEXT_PREC = SSH_CHATTER_MESSAGE_LIMIT - 1 };
    size_t option_count = 0U;
    char error[128];
    if (!session_poll_parse_fields(remainder, question, sizeof(question),
                                   options,
                                   sizeof(options) / sizeof(options[0]),
                                   &option_count, error, sizeof(error))) {
        if (error[0] != '\0') {
            session_send_system_line(ctx, error);
        } else {
            session_send_system_line(ctx, usage);
        }
        return;
    }

    named_poll_state_t snapshot = {0};
    bool created = false;
    bool allowed = true;
    ttak_mutex_lock(&ctx->owner->lock);
    named_poll_state_t *poll =
        host_ensure_named_poll_locked(ctx->owner, label);
    if (poll == nullptr) {
        allowed = false;
    } else if (poll->poll.active &&
               !(ctx->user.is_operator || ctx->user.is_lan_operator ||
                 strcasecmp(poll->owner, ctx->user.name) == 0)) {
        allowed = false;
    } else {
        uint64_t next_id = poll->poll.id + 1U;
        char saved_label[SSH_CHATTER_POLL_LABEL_LEN];
        snprintf(saved_label, sizeof(saved_label), "%s", label);
        named_poll_reset(poll);
        snprintf(poll->label, sizeof(poll->label), "%s", saved_label);
        snprintf(poll->owner, sizeof(poll->owner), "%s", ctx->user.name);
        poll->poll.active = true;
        poll->poll.allow_multiple = allow_multiple;
        poll->poll.id = next_id == 0U ? 1U : next_id;
        poll->poll.option_count = option_count;
        snprintf(poll->poll.question, sizeof(poll->poll.question), "%.*s",
                 SESSION_NAMED_POLL_TEXT_PREC, question);
        for (size_t idx = 0U; idx < option_count; ++idx) {
            snprintf(poll->poll.options[idx].text,
                     sizeof(poll->poll.options[idx].text), "%.*s",
                     SESSION_NAMED_POLL_TEXT_PREC, options[idx]);
            poll->poll.options[idx].votes = 0U;
        }
        poll->voter_count = 0U;
        host_recount_named_polls_locked(ctx->owner);
        host_vote_state_save_locked(ctx->owner);
        snapshot = *poll;
        created = true;
    }
    ttak_mutex_unlock(&ctx->owner->lock);

    if (!allowed) {
        session_send_system_line(ctx,
                                 "Unable to start that poll. Another active "
                                 "poll owns the label or the poll limit has "
                                 "been reached.");
        return;
    }

    if (created) {
        enum {
            SESSION_VOTE_NOTICE_USER_PREC = SSH_CHATTER_USERNAME_LEN - 1,
            SESSION_VOTE_NOTICE_LABEL_PREC = SSH_CHATTER_POLL_LABEL_LEN - 1,
            SESSION_VOTE_NOTICE_QUESTION_PREC = SSH_CHATTER_MESSAGE_LIMIT / 2
        };
        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice), "* [%.*s] started poll [%.*s]: %.*s",
                 SESSION_VOTE_NOTICE_USER_PREC, ctx->user.name,
                 SESSION_VOTE_NOTICE_LABEL_PREC, label,
                 SESSION_VOTE_NOTICE_QUESTION_PREC, question);
        host_history_record_system(ctx->owner, notice, nullptr);
        chat_room_broadcast(&ctx->owner->room, notice, nullptr);
        session_send_poll_summary_generic(ctx, &snapshot.poll, snapshot.label);
    }
}


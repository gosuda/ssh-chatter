{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return false;
    }

    if (ctx->user_data_loaded) {
        return true;
    }

    // Use a placeholder IP for loading if ctx->client_ip is not available or empty
    const char *ip_to_use =
        ctx->client_ip[0] != '\0' ? ctx->client_ip : nullptr;

    if (!user_data_load(ctx->owner->user_data_root, ctx->user.name, ip_to_use,
                        &ctx->user_data)) {
        // If loading fails, try to ensure it exists (create new)
        if (!user_data_ensure_exists(ctx->owner->user_data_root, ctx->user.name,
                                     ip_to_use, &ctx->user_data)) {
            return false;
        }
    }

    ctx->user_data_loaded = true;
    return true;
}

bool session_user_data_commit(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr || !ctx->user_data_loaded) {
        return false;
    }

    // Use a placeholder IP for saving if ctx->client_ip is not available or empty
    const char *ip_to_use =
        ctx->client_ip[0] != '\0' ? ctx->client_ip : nullptr;

    return user_data_save(ctx->owner->user_data_root, &ctx->user_data,
                          ip_to_use);
}

static bool session_argument_is_disable(const char *token)
{
    if (token == nullptr) {
        return false;
    }

    if (strcasecmp(token, "off") == 0 || strcasecmp(token, "none") == 0 ||
        strcasecmp(token, "disable") == 0 || strcasecmp(token, "stop") == 0) {
        return true;
    }

    return strcmp(token, "끄기") == 0 || strcmp(token, "オフ") == 0 ||
           strcmp(token, "关") == 0 || strcmp(token, "выкл") == 0;
}

static bool session_argument_is_enable(const char *token)
{
    if (token == nullptr) {
        return false;
    }

    if (strcasecmp(token, "on") == 0 || strcasecmp(token, "enable") == 0 ||
        strcasecmp(token, "start") == 0 || strcasecmp(token, "show") == 0) {
        return true;
    }

    return strcmp(token, "켜기") == 0 || strcmp(token, "オン") == 0 ||
           strcmp(token, "开") == 0 || strcmp(token, "开启") == 0 ||
           strcmp(token, "表示") == 0 || strcmp(token, "вкл") == 0;
}

static void session_language_normalize(const char *input, char *normalized,
                                       size_t length)
{
    if (normalized == nullptr || length == 0U) {
        return;
    }

    normalized[0] = '\0';
    if (input == nullptr) {
        return;
    }

    size_t out_idx = 0U;
    for (size_t idx = 0U; input[idx] != '\0'; ++idx) {
        unsigned char ch = (unsigned char)input[idx];
        if (isspace(ch)) {
            continue;
        }

        char lowered = (char)tolower(ch);
        if (lowered == '_') {
            lowered = '-';
        }

        if (out_idx + 1U >= length) {
            break;
        }

        normalized[out_idx++] = lowered;
    }

    if (out_idx < length) {
        normalized[out_idx] = '\0';
    } else {
        normalized[length - 1U] = '\0';
    }
}

static bool session_language_equals(const char *lhs, const char *rhs)
{
    if (lhs == nullptr || rhs == nullptr) {
        return false;
    }

    char normalized_lhs[SSH_CHATTER_LANG_NAME_LEN];
    char normalized_rhs[SSH_CHATTER_LANG_NAME_LEN];
    session_language_normalize(lhs, normalized_lhs, sizeof(normalized_lhs));
    session_language_normalize(rhs, normalized_rhs, sizeof(normalized_rhs));

    return strcmp(normalized_lhs, normalized_rhs) == 0;
}

typedef enum translation_job_type {
    TRANSLATION_JOB_CAPTION = 0,
    TRANSLATION_JOB_INPUT,
    TRANSLATION_JOB_PRIVATE_MESSAGE,
} translation_job_type_t;

typedef struct translation_job {
    translation_job_type_t type;
    char target_language[SSH_CHATTER_LANG_NAME_LEN];
    size_t placeholder_lines;
    struct translation_job *next;
    union {
        struct {
            char sanitized[SSH_CHATTER_TRANSLATION_WORKING_LEN];
            translation_placeholder_t
                placeholders[SSH_CHATTER_MAX_TRANSLATION_PLACEHOLDERS];
            size_t placeholder_count;
        } caption;
        struct {
            char original[SSH_CHATTER_TRANSLATION_WORKING_LEN];
        } input;
        struct {
            char original[SSH_CHATTER_TRANSLATION_WORKING_LEN];
            char target_name[SSH_CHATTER_USERNAME_LEN];
            char to_target_label[SSH_CHATTER_MESSAGE_LIMIT];
            char to_sender_label[SSH_CHATTER_MESSAGE_LIMIT];
        } pm;
    } data;
} translation_job_t;

typedef struct translation_result {
    translation_job_type_t type;
    bool success;
    size_t placeholder_lines;
    char translated[SSH_CHATTER_TRANSLATION_WORKING_LEN];
    char detected_language[SSH_CHATTER_LANG_NAME_LEN];
    char original[SSH_CHATTER_TRANSLATION_WORKING_LEN];
    char error_message[128];
    char pm_target_name[SSH_CHATTER_USERNAME_LEN];
    char pm_to_target_label[SSH_CHATTER_MESSAGE_LIMIT];
    char pm_to_sender_label[SSH_CHATTER_MESSAGE_LIMIT];
    struct translation_result *next;
} translation_result_t;

static translation_job_t *session_translation_job_alloc(void)
{
    translation_job_t *job = (translation_job_t *)sshc_gc_malloc(sizeof(*job));
    if (job != nullptr) {
        memset(job, 0, sizeof(*job));
    }
    return job;
}

static translation_result_t *session_translation_result_alloc(void)
{
    translation_result_t *result =
        (translation_result_t *)sshc_gc_malloc(sizeof(*result));
    if (result != nullptr) {
        memset(result, 0, sizeof(*result));
    }
    return result;
}

static bool session_translation_worker_ensure(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }

    if (!ctx->translation_mutex_initialized) {
        if (ttak_mutex_init(&ctx->translation_mutex) != 0) {
            return false;
        }
        ctx->translation_mutex_initialized = true;
    }

    if (!ctx->translation_cond_initialized) {
        if (ttak_cond_init(&ctx->translation_cond) != 0) {
            ttak_mutex_destroy(&ctx->translation_mutex);
            ctx->translation_mutex_initialized = false;
            return false;
        }
        ctx->translation_cond_initialized = true;
    }

    if (!ctx->translation_thread_started) {
        ctx->translation_thread_stop = false;
        if (pthread_create(&ctx->translation_thread, nullptr,
                           session_translation_worker, ctx) != 0) {
            ttak_cond_destroy(&ctx->translation_cond);
            ctx->translation_cond_initialized = false;
            ttak_mutex_destroy(&ctx->translation_mutex);
            ctx->translation_mutex_initialized = false;
            return false;
        }
        ctx->translation_thread_started = true;
    }

    return true;
}

static void session_translation_clear_queue(session_ctx_t *ctx)
{
    if (ctx == nullptr || !ctx->translation_mutex_initialized) {
        return;
    }

    translation_job_t *pending = nullptr;
    translation_result_t *ready = nullptr;

    ttak_mutex_lock(&ctx->translation_mutex);
    pending = ctx->translation_pending_head;
    ctx->translation_pending_head = nullptr;
    ctx->translation_pending_tail = nullptr;
    ready = ctx->translation_ready_head;
    ctx->translation_ready_head = nullptr;
    ctx->translation_ready_tail = nullptr;
    ttak_mutex_unlock(&ctx->translation_mutex);

    while (pending != nullptr) {
        translation_job_t *next = pending->next;
        sshc_gc_free(pending);
        pending = next;
    }

    while (ready != nullptr) {
        translation_result_t *next = ready->next;
        sshc_gc_free(ready);
        ready = next;
    }

    ctx->translation_placeholder_active_lines = 0U;
}

static bool session_translation_queue_caption(session_ctx_t *ctx,
                                              const char *message,
                                              size_t placeholder_lines)
{
    if (ctx == nullptr || message == nullptr) {
        return false;
    }

    char stripped[SSH_CHATTER_TRANSLATION_WORKING_LEN];
    if (translation_strip_no_translate_prefix(message, stripped,
                                              sizeof(stripped))) {
        return false;
    }

    if (!ctx->translation_enabled || !ctx->output_translation_enabled ||
        ctx->output_translation_language[0] == '\0' || message[0] == '\0') {
        return false;
    }

    if (!session_translation_worker_ensure(ctx)) {
        return false;
    }

    translation_job_t *job = session_translation_job_alloc();
    if (job == nullptr) {
        return false;
    }

    size_t placeholder_count = 0U;
    if (!translation_prepare_text(message, job->data.caption.sanitized,
                                  sizeof(job->data.caption.sanitized),
                                  job->data.caption.placeholders,
                                  &placeholder_count)) {
        return false;
    }

    if (job->data.caption.sanitized[0] == '\0') {
        return false;
    }

    job->type = TRANSLATION_JOB_CAPTION;
    job->data.caption.placeholder_count = placeholder_count;
    job->placeholder_lines = placeholder_lines;
    snprintf(job->target_language, sizeof(job->target_language), "%s",
             ctx->output_translation_language);

    ttak_mutex_lock(&ctx->translation_mutex);
    job->next = nullptr;
    if (ctx->translation_pending_tail != nullptr) {
        ctx->translation_pending_tail->next = job;
    } else {
        ctx->translation_pending_head = job;
    }
    ctx->translation_pending_tail = job;
    ttak_cond_signal(&ctx->translation_cond);
    ttak_mutex_unlock(&ctx->translation_mutex);

    return true;
}

static void session_translation_reserve_placeholders(session_ctx_t *ctx,
                                                     size_t placeholder_lines)
{
    if (ctx == nullptr || !session_transport_active(ctx) ||
        placeholder_lines == 0U) {
        return;
    }

    session_output_kind_t previous_kind =
        session_output_set_kind(ctx, SESSION_OUTPUT_KIND_CHAT);

    for (size_t idx = 0U; idx < placeholder_lines; ++idx) {
        session_write_rendered_line(ctx, "");
    }

    if (SIZE_MAX - ctx->translation_placeholder_active_lines <
        placeholder_lines) {
        ctx->translation_placeholder_active_lines = SIZE_MAX;
    } else {
        ctx->translation_placeholder_active_lines += placeholder_lines;
    }

    if (ctx->history_scroll_position == 0U) {
        session_refresh_input_line(ctx);
    }

    session_output_restore_kind(ctx, previous_kind);
}

static bool session_translation_push_scope_override(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }

    bool previous = ctx->translation_manual_scope_override;
    ctx->translation_manual_scope_override = true;
    return previous;
}

static void session_translation_pop_scope_override(session_ctx_t *ctx,
                                                   bool previous)
{
    if (ctx == nullptr) {
        return;
    }

    ctx->translation_manual_scope_override = previous;
}

static bool session_translation_queue_private_message(session_ctx_t *ctx,
                                                      session_ctx_t *target,
                                                      const char *message)
{
    if (ctx == nullptr || target == nullptr || message == nullptr) {
        return false;
    }

    if (!ctx->translation_enabled || !ctx->input_translation_enabled ||
        ctx->input_translation_language[0] == '\0' || message[0] == '\0') {
        return false;
    }

    if (!session_translation_worker_ensure(ctx)) {
        return false;
    }

    translation_job_t *job = session_translation_job_alloc();
    if (job == nullptr) {
        return false;
    }

    job->type = TRANSLATION_JOB_PRIVATE_MESSAGE;
    job->placeholder_lines = 0U;
    snprintf(job->target_language, sizeof(job->target_language), "%s",
             ctx->input_translation_language);
    snprintf(job->data.pm.original, sizeof(job->data.pm.original), "%s",
             message);
    snprintf(job->data.pm.target_name, sizeof(job->data.pm.target_name), "%s",
             target->user.name);
    snprintf(job->data.pm.to_target_label, sizeof(job->data.pm.to_target_label),
             "%s -> you", ctx->user.name);
    snprintf(job->data.pm.to_sender_label, sizeof(job->data.pm.to_sender_label),
             "you -> %s", target->user.name);

    ttak_mutex_lock(&ctx->translation_mutex);
    job->next = nullptr;
    if (ctx->translation_pending_tail != nullptr) {
        ctx->translation_pending_tail->next = job;
    } else {
        ctx->translation_pending_head = job;
    }
    ctx->translation_pending_tail = job;
    ttak_cond_signal(&ctx->translation_cond);
    ttak_mutex_unlock(&ctx->translation_mutex);

    return true;
}

static bool session_translation_queue_input(session_ctx_t *ctx,
                                            const char *text)
{
    if (ctx == nullptr || text == nullptr || text[0] == '\0') {
        return false;
    }

    if (!ctx->translation_enabled || !ctx->input_translation_enabled ||
        ctx->input_translation_language[0] == '\0') {
        return false;
    }

    if (!session_translation_worker_ensure(ctx)) {
        return false;
    }

    translation_job_t *job = session_translation_job_alloc();
    if (job == nullptr) {
        return false;
    }

    job->type = TRANSLATION_JOB_INPUT;
    job->placeholder_lines = 0U;
    snprintf(job->target_language, sizeof(job->target_language), "%s",
             ctx->input_translation_language);
    snprintf(job->data.input.original, sizeof(job->data.input.original), "%s",
             text);

    ttak_mutex_lock(&ctx->translation_mutex);
    job->next = nullptr;
    if (ctx->translation_pending_tail != nullptr) {
        ctx->translation_pending_tail->next = job;
    } else {
        ctx->translation_pending_head = job;
    }
    ctx->translation_pending_tail = job;
    ttak_cond_signal(&ctx->translation_cond);
    ttak_mutex_unlock(&ctx->translation_mutex);

    return true;
}

static void session_translation_normalize_output(char *text)
{
    if (text == nullptr) {
        return;
    }

    size_t length = strlen(text);
    size_t idx = 0U;
    while (idx < length) {
        char ch = text[idx];
        if ((ch == 'u' || ch == 'U') && idx + 4U < length &&
            text[idx + 1U] == '0' && text[idx + 2U] == '0' &&
            text[idx + 3U] == '3' &&
            (text[idx + 4U] == 'c' || text[idx + 4U] == 'C' ||
             text[idx + 4U] == 'e' || text[idx + 4U] == 'E')) {
            char replacement =
                (text[idx + 4U] == 'c' || text[idx + 4U] == 'C') ? '<' : '>';
            size_t remove_start = idx;
            if (remove_start > 0U && text[remove_start - 1U] == '\\') {
                --remove_start;
            }

            size_t remove_end = idx + 5U;
            size_t removed = remove_end - remove_start;
            text[remove_start] = replacement;
            memmove(text + remove_start + 1U, text + remove_end,
                    length - remove_end + 1U);
            length -= (removed - 1U);
            idx = remove_start + 1U;
            continue;
        }

        ++idx;
    }
}

static bool host_motd_contains_translation_notice(const char *motd_text)
{
    if (motd_text == nullptr) {
        return false;
    }

    const size_t notice_length = strlen(kTranslationQuotaNotice);
    const char *cursor = motd_text;
    while (*cursor != '\0') {
        size_t skip = host_column_reset_sequence_length(cursor);
        if (skip > 0U) {
            cursor += skip;
            continue;
        }
        if (*cursor == '\r' || *cursor == '\n') {
            ++cursor;
            continue;
        }
        if (strncmp(cursor, kTranslationQuotaNotice, notice_length) == 0) {
            return true;
        }
        while (*cursor != '\0' && *cursor != '\n') {
            ++cursor;
        }
    }

    return false;
}

static void host_prepend_translation_notice_in_memory(host_t *host,
                                                      const char *existing_motd)
{
    if (host == nullptr) {
        return;
    }

    char updated[sizeof(host->motd)];
    if (existing_motd != nullptr && existing_motd[0] != '\0') {
        snprintf(updated, sizeof(updated), "%s\n\n%s", kTranslationQuotaNotice,
                 existing_motd);
    } else {
        snprintf(updated, sizeof(updated), "%s\n", kTranslationQuotaNotice);
    }

    ttak_mutex_lock(&host->lock);
    snprintf(host->motd_base, sizeof(host->motd_base), "%s", updated);
    host_refresh_motd_locked(host);
    ttak_mutex_unlock(&host->lock);
}

static void host_handle_translation_quota_exhausted(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    bool already_marked = false;
    char motd_path[PATH_MAX];
    motd_path[0] = '\0';
    char motd_snapshot[sizeof(host->motd_base)];
    motd_snapshot[0] = '\0';

    ttak_mutex_lock(&host->lock);
    if (host->translation_quota_exhausted) {
        already_marked = true;
    } else {
        host->translation_quota_exhausted = true;
        if (host->motd_has_file && host->motd_path[0] != '\0') {
            snprintf(motd_path, sizeof(motd_path), "%s", host->motd_path);
        }
        snprintf(motd_snapshot, sizeof(motd_snapshot), "%s", host->motd_base);
    }
    ttak_mutex_unlock(&host->lock);

    if (already_marked) {
        return;
    }

    if (motd_path[0] == '\0') {
        if (host_motd_contains_translation_notice(motd_snapshot)) {
            host_refresh_motd(host);
            return;
        }
        host_prepend_translation_notice_in_memory(host, motd_snapshot);
        return;
    }

    char existing[8192];
    existing[0] = '\0';
    size_t existing_len = 0U;

    FILE *motd_file = fopen(motd_path, "rb");
    if (motd_file != nullptr) {
        existing_len = fread(existing, 1U, sizeof(existing) - 1U, motd_file);
        if (ferror(motd_file)) {
            const int read_error = errno;
            humanized_log_error("host", "failed to read motd file", read_error);
            existing_len = 0U;
            existing[0] = '\0';
        }
        existing[existing_len] = '\0';
        if (fclose(motd_file) != 0) {
            const int close_error = errno;
            humanized_log_error("host", "failed to close motd file",
                                close_error);
        }
    } else {
        host_prepend_translation_notice_in_memory(host, motd_snapshot);
        return;
    }

    const char *existing_start = existing;
    while (*existing_start == '\n' || *existing_start == '\r') {
        ++existing_start;
    }

    if (strncmp(existing_start, kTranslationQuotaNotice,
                strlen(kTranslationQuotaNotice)) == 0) {
        (void)host_try_load_motd_from_path(host, motd_path);
        return;
    }

    FILE *out = fopen(motd_path, "wb");
    if (out == nullptr) {
        const int write_error = errno != 0 ? errno : EIO;
        humanized_log_error("host", "failed to update motd file", write_error);
        host_prepend_translation_notice_in_memory(host, motd_snapshot);
        return;
    }

    (void)fprintf(out, "%s\n", kTranslationQuotaNotice);
    if (existing[0] != '\0') {
        fputc('\n', out);
        (void)fwrite(existing, 1U, existing_len, out);
    }

    if (fclose(out) != 0) {
        const int close_error = errno;
        humanized_log_error("host", "failed to close motd file", close_error);
    }

    (void)host_try_load_motd_from_path(host, motd_path);
}

static void session_handle_translation_quota_exhausted(session_ctx_t *ctx,
                                                       const char *error_detail)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    host_handle_translation_quota_exhausted(ctx->owner);

    const bool was_enabled = ctx->translation_enabled ||
                             ctx->output_translation_enabled ||
                             ctx->input_translation_enabled;
    ctx->translation_enabled = false;
    ctx->output_translation_enabled = false;
    ctx->input_translation_enabled = false;

    if (was_enabled) {
        host_store_translation_preferences(ctx->owner, ctx);
    }

    if (!ctx->translation_quota_notified) {
        char message[256];
        if (error_detail != nullptr && error_detail[0] != '\0') {
            (void)snprintf(message, sizeof(message), "%s (%s)",
                           kTranslationQuotaSystemMessage, error_detail);
        } else {
            (void)snprintf(message, sizeof(message), "%s",
                           kTranslationQuotaSystemMessage);
        }
        session_send_system_line(ctx, message);
        ctx->translation_quota_notified = true;
    }
}

static void session_translation_flush_ready(session_ctx_t *ctx)
{
    if (ctx == nullptr || !ctx->translation_mutex_initialized) {
        return;
    }

    translation_result_t *ready = nullptr;

    ttak_mutex_lock(&ctx->translation_mutex);
    ready = ctx->translation_ready_head;
    ctx->translation_ready_head = nullptr;
    ctx->translation_ready_tail = nullptr;
    ttak_mutex_unlock(&ctx->translation_mutex);

    if (ready == nullptr) {
        return;
    }

    const bool translation_active = ctx->translation_enabled &&
                                    ctx->output_translation_enabled &&
                                    ctx->output_translation_language[0] != '\0';

    bool refreshed = false;
    while (ready != nullptr) {
        translation_result_t *next = ready->next;
        if (ready->type == TRANSLATION_JOB_INPUT) {
            if (ready->success) {
                if (ready->detected_language[0] != '\0') {
                    snprintf(ctx->last_detected_input_language,
                             sizeof(ctx->last_detected_input_language), "%s",
                             ready->detected_language);
                }
                session_deliver_outgoing_message(ctx, ready->translated, false);
            } else {
                const char *error_message =
                    ready->error_message[0] != '\0'
                        ? ready->error_message
                        : "Translation failed; sending your original message.";
                session_send_system_line(ctx, error_message);
                session_deliver_outgoing_message(ctx, ready->original, false);
            }
            refreshed = true;
            ready = next;
            continue;
        }

        if (ready->type == TRANSLATION_JOB_PRIVATE_MESSAGE) {
            session_ctx_t *target = nullptr;
            if (ctx->owner != nullptr && ready->pm_target_name[0] != '\0') {
                target = chat_room_find_user(&ctx->owner->room,
                                             ready->pm_target_name);
            }

            if (ready->success) {
                if (target != nullptr) {
                    session_send_private_message_line(target, ctx,
                                                      ready->pm_to_target_label,
                                                      ready->translated);
                } else if (ready->pm_target_name[0] != '\0') {
                    char notice[SSH_CHATTER_MESSAGE_LIMIT];
                    snprintf(notice, sizeof(notice),
                             "User '%s' disconnected before your private "
                             "message was "
                             "delivered.",
                             ready->pm_target_name);
                    session_send_system_line(ctx, notice);
                }
                session_send_private_message_line(
                    ctx, ctx, ready->pm_to_sender_label, ready->translated);
            } else {
                const char *error_message =
                    ready->error_message[0] != '\0'
                        ? ready->error_message
                        : "Translation failed; sending your original message.";
                session_send_system_line(ctx, error_message);
                if (target != nullptr) {
                    session_send_private_message_line(target, ctx,
                                                      ready->pm_to_target_label,
                                                      ready->original);
                } else if (ready->pm_target_name[0] != '\0') {
                    char notice[SSH_CHATTER_MESSAGE_LIMIT];
                    snprintf(notice, sizeof(notice),
                             "User '%s' disconnected before your private "
                             "message was "
                             "delivered.",
                             ready->pm_target_name);
                    session_send_system_line(ctx, notice);
                }
                session_send_private_message_line(
                    ctx, ctx, ready->pm_to_sender_label, ready->original);
            }

            refreshed = true;
            ready = next;
            continue;
        }

        size_t placeholder_lines = ready->placeholder_lines;
        size_t move_up = 0U;
        if (placeholder_lines > 0U &&
            ctx->translation_placeholder_active_lines >= placeholder_lines) {
            size_t remaining_after =
                ctx->translation_placeholder_active_lines - placeholder_lines;
            move_up = remaining_after + 1U;
        }

        if (translation_active) {
            const char *body = ready->translated;
            if (body[0] == '\0') {
                body = "translation unavailable.";
            }

            const char *line_cursor = body;
            size_t line_index = 0U;
            while (line_cursor != nullptr) {
                const char *line_end = strchr(line_cursor, '\n');
                size_t line_length = (line_end != nullptr)
                                         ? (size_t)(line_end - line_cursor)
                                         : strlen(line_cursor);
                if (line_length >= SSH_CHATTER_TRANSLATION_WORKING_LEN) {
                    line_length = SSH_CHATTER_TRANSLATION_WORKING_LEN - 1U;
                }

                char line_fragment[SSH_CHATTER_TRANSLATION_WORKING_LEN];
                memcpy(line_fragment, line_cursor, line_length);
                line_fragment[line_length] = '\0';

                char annotated[SSH_CHATTER_TRANSLATION_WORKING_LEN + 64U];
                snprintf(annotated, sizeof(annotated), "    \342\206\263 %s",
                         line_fragment);
                session_render_caption_with_offset(
                    ctx, annotated, line_index == 0U ? move_up : 0U);
                refreshed = true;

                if (line_end == nullptr) {
                    break;
                }

                line_cursor = line_end + 1;
                ++line_index;
            }
        }

        if (placeholder_lines > 0U) {
            if (ctx->translation_placeholder_active_lines >=
                placeholder_lines) {
                ctx->translation_placeholder_active_lines -= placeholder_lines;
            } else {
                ctx->translation_placeholder_active_lines = 0U;
            }
        }

        ready = next;
    }

    if (refreshed && ctx->history_scroll_position == 0U) {
        session_refresh_input_line(ctx);
    }
}

static void session_translation_worker_shutdown(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    if (ctx->translation_mutex_initialized) {
        ttak_mutex_lock(&ctx->translation_mutex);
        if (ctx->translation_thread_started) {
            ctx->translation_thread_stop = true;
            ttak_cond_broadcast(&ctx->translation_cond);
            ttak_mutex_unlock(&ctx->translation_mutex);
            pthread_join(ctx->translation_thread, nullptr);
            ctx->translation_thread_started = false;
        } else {
            ttak_mutex_unlock(&ctx->translation_mutex);
        }
    }

    session_translation_clear_queue(ctx);

    if (ctx->translation_cond_initialized) {
        ttak_cond_destroy(&ctx->translation_cond);
        ctx->translation_cond_initialized = false;
    }
    if (ctx->translation_mutex_initialized) {
        ttak_mutex_destroy(&ctx->translation_mutex);
        ctx->translation_mutex_initialized = false;
    }

    ctx->translation_thread_stop = false;
}

static void session_translation_publish_result(
    session_ctx_t *ctx, const translation_job_t *job, const char *payload,
    const char *detected_language, const char *error_message, bool success)
{
    if (ctx == nullptr || job == nullptr) {
        return;
    }

    translation_result_t *result = session_translation_result_alloc();
    if (result == nullptr) {
        return;
    }

    result->type = job->type;
    result->success = success;
    result->placeholder_lines = job->placeholder_lines;

    if (job->type == TRANSLATION_JOB_INPUT) {
        snprintf(result->original, sizeof(result->original), "%s",
                 job->data.input.original);
        if (payload != nullptr) {
            snprintf(result->translated, sizeof(result->translated), "%s",
                     payload);
        } else {
            result->translated[0] = '\0';
        }
        if (detected_language != nullptr) {
            snprintf(result->detected_language,
                     sizeof(result->detected_language), "%s",
                     detected_language);
        } else {
            result->detected_language[0] = '\0';
        }
        if (error_message != nullptr) {
            snprintf(result->error_message, sizeof(result->error_message), "%s",
                     error_message);
        } else {
            result->error_message[0] = '\0';
        }
        session_translation_normalize_output(result->translated);
    } else if (job->type == TRANSLATION_JOB_PRIVATE_MESSAGE) {
        snprintf(result->original, sizeof(result->original), "%s",
                 job->data.pm.original);
        if (payload != nullptr) {
            snprintf(result->translated, sizeof(result->translated), "%s",
                     payload);
        } else {
            result->translated[0] = '\0';
        }
        if (error_message != nullptr) {
            snprintf(result->error_message, sizeof(result->error_message), "%s",
                     error_message);
        } else {
            result->error_message[0] = '\0';
        }
        result->detected_language[0] = '\0';
        snprintf(result->pm_target_name, sizeof(result->pm_target_name), "%s",
                 job->data.pm.target_name);
        snprintf(result->pm_to_target_label, sizeof(result->pm_to_target_label),
                 "%s", job->data.pm.to_target_label);
        snprintf(result->pm_to_sender_label, sizeof(result->pm_to_sender_label),
                 "%s", job->data.pm.to_sender_label);
        session_translation_normalize_output(result->translated);
    } else {
        const char *message = payload;
        if (message == nullptr || message[0] == '\0') {
            if (success) {
                message = "";
            } else {
                message = "[!] translation unavailable.";
            }
        }

        snprintf(result->translated, sizeof(result->translated), "%s", message);
        session_translation_normalize_output(result->translated);
        result->detected_language[0] = '\0';
        result->error_message[0] = '\0';
        result->original[0] = '\0';
    }

    ttak_mutex_lock(&ctx->translation_mutex);
    result->next = nullptr;
    if (ctx->translation_ready_tail != nullptr) {
        ctx->translation_ready_tail->next = result;
    } else {
        ctx->translation_ready_head = result;
    }
    ctx->translation_ready_tail = result;
    ttak_mutex_unlock(&ctx->translation_mutex);
}

static void session_translation_process_single_job(session_ctx_t *ctx,
                                                   translation_job_t *job)
{
    if (ctx == nullptr || job == nullptr) {
        return;
    }

    if (ctx->translation_thread_stop) {
        return;
    }

    if (job->type == TRANSLATION_JOB_INPUT ||
        job->type == TRANSLATION_JOB_PRIVATE_MESSAGE) {
        char translated_body[SSH_CHATTER_TRANSLATION_WORKING_LEN];
        char detected_language[SSH_CHATTER_LANG_NAME_LEN];
        translated_body[0] = '\0';
        detected_language[0] = '\0';

        const bool is_private_message =
            (job->type == TRANSLATION_JOB_PRIVATE_MESSAGE);
        const char *source_text = is_private_message ? job->data.pm.original
                                                     : job->data.input.original;
        char *detected_target =
            is_private_message ? nullptr : detected_language;
        size_t detected_length =
            is_private_message ? 0U : sizeof(detected_language);

        if (translator_translate_with_cancel(
                source_text, job->target_language, translated_body,
                sizeof(translated_body), detected_target, detected_length,
                &ctx->translation_thread_stop)) {
            if (ctx->translation_thread_stop) {
                return;
            }
            session_translation_publish_result(
                ctx, job, translated_body,
                is_private_message ? nullptr : detected_language, nullptr,
                true);
        } else {
            const char *error = translator_last_error();
            char message[128];
            const bool quota_failure = translator_last_error_was_quota();
            if (ctx->translation_thread_stop) {
                return;
            }
            if (quota_failure) {
                if (error != nullptr && error[0] != '\0') {
                    snprintf(message, sizeof(message),
                             "[!] translation unavailable (quota exhausted: %s); "
                             "sending "
                             "your original message.",
                             error);
                } else {
                    snprintf(message, sizeof(message),
                             "[!] translation unavailable (quota exhausted); "
                             "sending your "
                             "original message.");
                }
                session_handle_translation_quota_exhausted(ctx, error);
            } else if (error != nullptr && error[0] != '\0') {
                snprintf(
                    message, sizeof(message),
                    "Translation failed (%s); sending your original message.",
                    error);
            } else {
                snprintf(message, sizeof(message),
                         "Translation failed; sending your original message.");
            }
            if (ctx->translation_thread_stop) {
                return;
            }
            session_translation_publish_result(ctx, job, nullptr, nullptr,
                                               message, false);
        }
        return;
    }

    char translated_body[SSH_CHATTER_TRANSLATION_WORKING_LEN];
    char restored[SSH_CHATTER_TRANSLATION_WORKING_LEN];
    translated_body[0] = '\0';
    restored[0] = '\0';

    bool success = false;
    char failure_message[128];
    failure_message[0] = '\0';
    const int max_attempts = 3;
    for (int attempt = 0; attempt < max_attempts && !success; ++attempt) {
        translated_body[0] = '\0';

        if (ctx->translation_thread_stop) {
            return;
        }

        if (!translator_translate_with_cancel(
                job->data.caption.sanitized, job->target_language,
                translated_body, sizeof(translated_body), nullptr, 0U,
                &ctx->translation_thread_stop)) {
            const char *error = translator_last_error();
            const bool quota_failure = translator_last_error_was_quota();
            if (ctx->translation_thread_stop) {
                return;
            }
            if (quota_failure) {
                if (error != nullptr && error[0] != '\0') {
                    snprintf(failure_message, sizeof(failure_message),
                             "[!] translation unavailable (quota exhausted: %s)",
                             error);
                } else {
                    snprintf(failure_message, sizeof(failure_message),
                             "[!] translation unavailable (quota exhausted).");
                }
                session_handle_translation_quota_exhausted(ctx, error);
                break;
            }

            if (error != nullptr && error[0] != '\0') {
                snprintf(failure_message, sizeof(failure_message),
                         "[!] translation failed: %s", error);
            } else {
                snprintf(failure_message, sizeof(failure_message),
                         "[!] translation failed.");
            }

            if (attempt + 1 < max_attempts) {
                struct timespec retry_delay = {.tv_sec = 1, .tv_nsec = 0L};
                host_sleep_uninterruptible(&retry_delay);
            }
            continue;
        }

        if (!translation_restore_text(translated_body, restored,
                                      sizeof(restored),
                                      job->data.caption.placeholders,
                                      job->data.caption.placeholder_count)) {
            snprintf(failure_message, sizeof(failure_message),
                     "[!] translation post-processing failed.");
            break;
        }

        success = true;
        failure_message[0] = '\0';
    }

    if (!success && failure_message[0] == '\0') {
        snprintf(failure_message, sizeof(failure_message),
                 "[!] translation unavailable.");
    }

    if (ctx->translation_thread_stop) {
        return;
    }

    if (success) {
        session_translation_publish_result(ctx, job, restored, nullptr, nullptr,
                                           true);
    } else {
        session_translation_publish_result(ctx, job, failure_message, nullptr,
                                           nullptr, false);
    }
}

static bool session_translation_process_batch(session_ctx_t *ctx,
                                              translation_job_t **jobs,
                                              size_t job_count)
{
    if (ctx == nullptr || jobs == nullptr || job_count == 0U) {
        return false;
    }

    if (jobs[0] == nullptr || jobs[0]->type != TRANSLATION_JOB_CAPTION) {
        return false;
    }

    if (ctx->translation_thread_stop) {
        for (size_t idx = 0U; idx < job_count; ++idx) {
            if (jobs[idx] != nullptr) {
                jobs[idx] = nullptr;
            }
        }
        return true;
    }

    char *combined =
        sshc_gc_calloc(SSH_CHATTER_TRANSLATION_BATCH_BUFFER, sizeof(char));
    char *translated =
        sshc_gc_calloc(SSH_CHATTER_TRANSLATION_BATCH_BUFFER, sizeof(char));
    if (combined == nullptr || translated == nullptr) {
        return false;
    }

    size_t offset = 0U;
    for (size_t idx = 0U; idx < job_count; ++idx) {
        if (ctx->translation_thread_stop) {
            for (size_t release = idx; release < job_count; ++release) {
                if (jobs[release] != nullptr) {
                    jobs[release] = nullptr;
                }
            }
            return true;
        }
        if (jobs[idx] == nullptr ||
            jobs[idx]->type != TRANSLATION_JOB_CAPTION) {
            return false;
        }

        char marker[32];
        int marker_len =
            snprintf(marker, sizeof(marker), "[[SEG%02zu]]\n", idx);
        if (marker_len < 0) {
            return false;
        }

        size_t marker_size = (size_t)marker_len;
        size_t text_len = strlen(jobs[idx]->data.caption.sanitized);
        if (offset + marker_size + text_len + 1U >
            SSH_CHATTER_TRANSLATION_BATCH_BUFFER) {
            return false;
        }

        memcpy(combined + offset, marker, marker_size);
        offset += marker_size;
        memcpy(combined + offset, jobs[idx]->data.caption.sanitized, text_len);
        offset += text_len;
        combined[offset++] = '\n';
    }
    combined[offset] = '\0';

    if (!translator_translate_with_cancel(
            combined, jobs[0]->target_language, translated,
            SSH_CHATTER_TRANSLATION_BATCH_BUFFER, nullptr, 0U,
            &ctx->translation_thread_stop)) {
        if (ctx->translation_thread_stop) {
            for (size_t idx = 0U; idx < job_count; ++idx) {
                if (jobs[idx] != nullptr) {
                    jobs[idx] = nullptr;
                }
            }
            return true;
        }
        return false;
    }

    if (ctx->translation_thread_stop) {
        for (size_t idx = 0U; idx < job_count; ++idx) {
            if (jobs[idx] != nullptr) {
                jobs[idx] = nullptr;
            }
        }
        return true;
    }

    char *segment_starts[SSH_CHATTER_TRANSLATION_BATCH_MAX] = {0};
    char *segment_ends[SSH_CHATTER_TRANSLATION_BATCH_MAX] = {0};

    char *search_cursor = translated;
    for (size_t idx = 0U; idx < job_count; ++idx) {
        char marker[32];
        int marker_len = snprintf(marker, sizeof(marker), "[[SEG%02zu]]", idx);
        if (marker_len < 0) {
            return false;
        }

        char *marker_pos = strstr(search_cursor, marker);
        if (marker_pos == nullptr) {
            return false;
        }

        char *start = marker_pos + (size_t)marker_len;
        while (*start == '\r' || *start == '\n') {
            ++start;
        }

        segment_starts[idx] = start;
        search_cursor = start;
    }

    for (size_t idx = 0U; idx + 1U < job_count; ++idx) {
        char marker[32];
        int marker_len =
            snprintf(marker, sizeof(marker), "[[SEG%02zu]]", idx + 1U);
        if (marker_len < 0) {
            return false;
        }

        char *next_pos = strstr(segment_starts[idx], marker);
        if (next_pos == nullptr) {
            return false;
        }

        char *end = next_pos;
        while (end > segment_starts[idx] &&
               (end[-1] == '\r' || end[-1] == '\n')) {
            --end;
        }
        segment_ends[idx] = end;
    }

    char *last_end = translated + strlen(translated);
    while (last_end > segment_starts[job_count - 1U] &&
           (last_end[-1] == '\r' || last_end[-1] == '\n')) {
        --last_end;
    }
    segment_ends[job_count - 1U] = last_end;

    char restored_segments[SSH_CHATTER_TRANSLATION_BATCH_MAX]
                          [SSH_CHATTER_TRANSLATION_WORKING_LEN];
    for (size_t idx = 0U; idx < job_count; ++idx) {
        if (segment_starts[idx] == nullptr || segment_ends[idx] == nullptr ||
            segment_ends[idx] < segment_starts[idx]) {
            return false;
        }

        size_t segment_len = (size_t)(segment_ends[idx] - segment_starts[idx]);
        if (segment_len + 1U > SSH_CHATTER_TRANSLATION_WORKING_LEN) {
            return false;
        }

        char segment_buffer[SSH_CHATTER_TRANSLATION_WORKING_LEN];
        memcpy(segment_buffer, segment_starts[idx], segment_len);
        segment_buffer[segment_len] = '\0';

        if (!translation_restore_text(
                segment_buffer, restored_segments[idx],
                sizeof(restored_segments[idx]),
                jobs[idx]->data.caption.placeholders,
                jobs[idx]->data.caption.placeholder_count)) {
            return false;
        }
    }

    if (ctx->translation_thread_stop) {
        for (size_t idx = 0U; idx < job_count; ++idx) {
            if (jobs[idx] != nullptr) {
                jobs[idx] = nullptr;
            }
        }
        return true;
    }

    for (size_t idx = 0U; idx < job_count; ++idx) {
        session_translation_publish_result(
            ctx, jobs[idx], restored_segments[idx], nullptr, nullptr, true);
    }

    return true;
}

static void *session_translation_worker(void *arg)
{
    session_ctx_t *ctx = (session_ctx_t *)arg;
    if (ctx == nullptr) {
        return nullptr;
    }

    sshc_memory_context_t *memory_scope = nullptr;
    if (ctx->owner != nullptr) {
        memory_scope = sshc_memory_context_push(ctx->owner->memory_context);
    }

    for (;;) {
        translation_job_t *batch[SSH_CHATTER_TRANSLATION_BATCH_MAX] = {0};
        size_t batch_count = 0U;

        ttak_mutex_lock(&ctx->translation_mutex);
        while (!ctx->translation_thread_stop &&
               ctx->translation_pending_head == nullptr) {
            ttak_cond_wait(&ctx->translation_cond, &ctx->translation_mutex);
        }

        if (ctx->translation_thread_stop) {
            ttak_mutex_unlock(&ctx->translation_mutex);
            break;
        }

        translation_job_t *job = ctx->translation_pending_head;
        if (job != nullptr) {
            ctx->translation_pending_head = job->next;
            if (ctx->translation_pending_head == nullptr) {
                ctx->translation_pending_tail = nullptr;
            }
            job->next = nullptr;
            batch[batch_count++] = job;
        }
        ttak_mutex_unlock(&ctx->translation_mutex);

        if (batch_count == 0U) {
            continue;
        }

        if (batch[0]->type != TRANSLATION_JOB_CAPTION) {
            session_translation_process_single_job(ctx, batch[0]);
            continue;
        }

        size_t estimate = strlen(batch[0]->data.caption.sanitized) +
                          SSH_CHATTER_TRANSLATION_SEGMENT_GUARD;

        if (batch_count == 1U) {
            bool delay_needed = false;
            ttak_mutex_lock(&ctx->translation_mutex);
            if (!ctx->translation_thread_stop &&
                ctx->translation_pending_head == nullptr) {
                delay_needed = true;
            }
            ttak_mutex_unlock(&ctx->translation_mutex);

            if (delay_needed) {
                struct timespec aggregation_delay = {
                    .tv_sec = 0,
                    .tv_nsec = SSH_CHATTER_TRANSLATION_BATCH_DELAY_NS};
                host_sleep_uninterruptible(&aggregation_delay);
            }
        }

        ttak_mutex_lock(&ctx->translation_mutex);
        while (batch_count < SSH_CHATTER_TRANSLATION_BATCH_MAX &&
               ctx->translation_pending_head != nullptr) {
            translation_job_t *candidate = ctx->translation_pending_head;
            if (candidate == nullptr) {
                break;
            }

            if (candidate->type != TRANSLATION_JOB_CAPTION) {
                break;
            }

            if (strcmp(candidate->target_language, batch[0]->target_language) !=
                0) {
                break;
            }

            size_t candidate_len = strlen(candidate->data.caption.sanitized) +
                                   SSH_CHATTER_TRANSLATION_SEGMENT_GUARD;
            if (estimate + candidate_len >=
                SSH_CHATTER_TRANSLATION_BATCH_BUFFER) {
                break;
            }

            ctx->translation_pending_head = candidate->next;
            if (ctx->translation_pending_head == nullptr) {
                ctx->translation_pending_tail = nullptr;
            }
            candidate->next = nullptr;
            batch[batch_count++] = candidate;
            estimate += candidate_len;
        }
        ttak_mutex_unlock(&ctx->translation_mutex);

        bool processed = false;
        if (batch_count > 1U) {
            processed =
                session_translation_process_batch(ctx, batch, batch_count);
        }

        if (!processed) {
            for (size_t idx = 0U; idx < batch_count; ++idx) {
                session_translation_process_single_job(ctx, batch[idx]);
            }
        }
    }

    if (memory_scope != nullptr) {
        sshc_memory_context_pop(memory_scope);
    }
    return nullptr;
}


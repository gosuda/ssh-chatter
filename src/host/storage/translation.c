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
    TRANSLATION_JOB_CHAT,
    TRANSLATION_JOB_BBS_POST,
} translation_job_type_t;

typedef struct translation_job {
    translation_job_type_t type;
    char target_language[SSH_CHATTER_LANG_NAME_LEN];
    size_t placeholder_lines;
    uint64_t generation;
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
        struct {
            uint64_t message_id;
            bool backfill;
            char name[SSH_CHATTER_USERNAME_LEN];
            char text[SSH_CHATTER_MESSAGE_LIMIT];
        } chat;
        struct {
            uint64_t post_id;
            uint64_t fingerprint;
            char **segments; /* title, body chunks, then one per comment */
            size_t segment_count;
            size_t body_chunks;
        } bbs;
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
    uint64_t generation;
    uint64_t message_id;
    bool backfill;
    char chat_name[SSH_CHATTER_USERNAME_LEN];
    uint64_t post_id;
    uint64_t fingerprint;
    /* BBS results only: the array and every string in it are owned by this
     * node until session_translation_apply_bbs_result moves them into the
     * BBS cache; whoever owns them last frees them (see reset_cache). */
    char **segments;
    size_t segment_count;
    size_t body_chunks;
    struct translation_result *next;
} translation_result_t;

#ifndef SSH_CHATTER_TRANSLATION_BACKFILL_MESSAGES
#define SSH_CHATTER_TRANSLATION_BACKFILL_MESSAGES 100U
#endif

/* One request carries the backfill plus whatever arrived meanwhile. */
#ifndef SSH_CHATTER_TRANSLATION_CHAT_BATCH_MAX
#define SSH_CHATTER_TRANSLATION_CHAT_BATCH_MAX 128U
#endif

#ifndef SSH_CHATTER_TRANSLATION_CHAT_BATCH_BYTES
#define SSH_CHATTER_TRANSLATION_CHAT_BATCH_BYTES (64U * 1024U)
#endif

#ifndef SSH_CHATTER_TRANSLATION_CHAT_CACHE_SIZE
#define SSH_CHATTER_TRANSLATION_CHAT_CACHE_SIZE 512U
#endif

#ifndef SSH_CHATTER_TRANSLATION_BBS_CHUNK_BYTES
#define SSH_CHATTER_TRANSLATION_BBS_CHUNK_BYTES 1800U
#endif

typedef struct translation_chat_cache_entry {
    uint64_t message_id;
    bool pending;
    bool failed;
    char *text;
} translation_chat_cache_entry_t;

typedef struct translation_chat_cache {
    size_t next_slot;
    translation_chat_cache_entry_t
        entries[SSH_CHATTER_TRANSLATION_CHAT_CACHE_SIZE];
} translation_chat_cache_t;

typedef struct translation_bbs_cache {
    uint64_t post_id;
    uint64_t fingerprint;
    bool pending;
    bool failed;
    char **segments;
    size_t segment_count;
    size_t body_chunks;
} translation_bbs_cache_t;

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

// Free a BBS segment list: every string in it plus the array itself.
static void session_translation_free_bbs_segments(char **segments,
                                                  size_t segment_count)
{
    if (segments == nullptr) {
        return;
    }
    for (size_t idx = 0U; idx < segment_count; ++idx) {
        sshc_gc_free(segments[idx]);
    }
    sshc_gc_free(segments);
}

// Free a result node that was never handed to an apply function, together
// with any heap data it still owns. Once apply_bbs_result has run, the node
// is bare (its segments moved to the cache or were freed) and only the node
// itself may be freed.
static void session_translation_free_result(translation_result_t *result)
{
    if (result == nullptr) {
        return;
    }
    session_translation_free_bbs_segments(result->segments,
                                          result->segment_count);
    sshc_gc_free(result);
}

// Free a processed translation job once the worker is done with it. Only BBS
// jobs carry heap data beyond the node: their source segment strings and the
// array holding them. Results keep copies of what they need (process_bbs_job
// transfers ownership of a string rather than borrowing it when a copy
// fails), so freeing the job's segments here cannot double-free.
static void session_translation_free_job(translation_job_t *job)
{
    if (job == nullptr) {
        return;
    }
    if (job->type == TRANSLATION_JOB_BBS_POST) {
        session_translation_free_bbs_segments(job->data.bbs.segments,
                                              job->data.bbs.segment_count);
    }
    sshc_gc_free(job);
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

// Forget every cached chat/BBS translation and make results of jobs that
// are still in flight stale, so a language change never shows old output.
static void session_translation_reset_cache(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    ++ctx->translation_generation;
    ctx->translation_backfill_done = false;

    translation_chat_cache_t *chat_cache = ctx->translation_chat_cache;
    if (chat_cache != nullptr) {
        for (size_t idx = 0U; idx < SSH_CHATTER_TRANSLATION_CHAT_CACHE_SIZE;
             ++idx) {
            sshc_gc_free(chat_cache->entries[idx].text);
        }
        sshc_gc_free(chat_cache);
    }
    ctx->translation_chat_cache = nullptr;

    translation_bbs_cache_t *bbs_cache = ctx->translation_bbs_cache;
    if (bbs_cache != nullptr) {
        session_translation_free_bbs_segments(bbs_cache->segments,
                                              bbs_cache->segment_count);
        sshc_gc_free(bbs_cache);
    }
    ctx->translation_bbs_cache = nullptr;
}

static void session_translation_clear_queue(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    if (ctx->translation_mutex_initialized) {
        ttak_mutex_lock(&ctx->translation_mutex);
        session_translation_reset_cache(ctx);
        ttak_mutex_unlock(&ctx->translation_mutex);
    } else {
        session_translation_reset_cache(ctx);
        return;
    }

    translation_job_t *pending = nullptr;
    translation_result_t *ready = nullptr;
    translation_result_t *batch_ready = nullptr;

    ttak_mutex_lock(&ctx->translation_mutex);
    pending = ctx->translation_pending_head;
    ctx->translation_pending_head = nullptr;
    ctx->translation_pending_tail = nullptr;
    ready = ctx->translation_ready_head;
    ctx->translation_ready_head = nullptr;
    ctx->translation_ready_tail = nullptr;
    batch_ready = ctx->translation_batch_ready_head;
    ctx->translation_batch_ready_head = nullptr;
    ctx->translation_batch_ready_tail = nullptr;
    ttak_mutex_unlock(&ctx->translation_mutex);

    while (pending != nullptr) {
        translation_job_t *next = pending->next;
        if (pending->type == TRANSLATION_JOB_BBS_POST) {
            session_translation_free_bbs_segments(pending->data.bbs.segments,
                                                  pending->data.bbs
                                                      .segment_count);
        }
        sshc_gc_free(pending);
        pending = next;
    }

    while (ready != nullptr) {
        translation_result_t *next = ready->next;
        session_translation_free_result(ready);
        ready = next;
    }

    while (batch_ready != nullptr) {
        translation_result_t *next = batch_ready->next;
        session_translation_free_result(batch_ready);
        batch_ready = next;
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
        sshc_gc_free(job);
        return false;
    }

    if (job->data.caption.sanitized[0] == '\0') {
        sshc_gc_free(job);
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

static const char *session_translation_pending_text(session_ui_language_t lang, bool is_pm)
{
    switch (lang) {
    case SESSION_UI_LANGUAGE_KO:
        return is_pm ? "[귓속말 번역 중...]" : "[메시지 번역 중...]";
    case SESSION_UI_LANGUAGE_JP:
        return is_pm ? "[プライベートメッセージ翻訳中...]" : "[メッセージ翻訳中...]";
    case SESSION_UI_LANGUAGE_ZH:
        return is_pm ? "[正在翻译私信...]" : "[正在翻译消息...]";
    case SESSION_UI_LANGUAGE_RU:
        return is_pm ? "[Перевод личного сообщения...]" : "[Перевод сообщения...]";
    case SESSION_UI_LANGUAGE_DE:
        return is_pm ? "[Private Nachricht wird übersetzt...]" : "[Nachricht wird übersetzt...]";
    case SESSION_UI_LANGUAGE_FR:
        return is_pm ? "[Traduction du message privé...]" : "[Traduction du message...]";
    case SESSION_UI_LANGUAGE_PL:
        return is_pm ? "[Tłumaczenie prywatnej wiadomości...]" : "[Tłumaczenie wiadomości...]";
    default:
        return is_pm ? "[Translating private message...]" : "[Translating message...]";
    }
}

static const char *session_translation_bbs_pending_text(session_ui_language_t lang)
{
    switch (lang) {
    case SESSION_UI_LANGUAGE_KO:
        return "[게시글 번역 중...]";
    case SESSION_UI_LANGUAGE_JP:
        return "[投稿を翻訳中...]";
    case SESSION_UI_LANGUAGE_ZH:
        return "[正在翻译帖子...]";
    case SESSION_UI_LANGUAGE_RU:
        return "[Перевод публикации...]";
    case SESSION_UI_LANGUAGE_DE:
        return "[Beitrag wird übersetzt...]";
    case SESSION_UI_LANGUAGE_FR:
        return "[Traduction de la publication...]";
    case SESSION_UI_LANGUAGE_PL:
        return "[Tłumaczenie posta...]";
    default:
        return "[Translating post...]";
    }
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
    session_command_snprintf(target, job->data.pm.to_target_label,
                             sizeof(job->data.pm.to_target_label), "%s -> you",
                             ctx->user.name);
    session_command_snprintf(ctx, job->data.pm.to_sender_label,
                             sizeof(job->data.pm.to_sender_label), "you -> %s",
                             target->user.name);

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

    session_send_system_line(ctx, session_translation_pending_text(ctx->ui_language, true));
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

    session_send_system_line(ctx, session_translation_pending_text(ctx->ui_language, false));
    return true;
}

static bool session_translation_output_active(const session_ctx_t *ctx)
{
    return ctx != nullptr && ctx->translation_enabled &&
           ctx->output_translation_enabled &&
           ctx->output_translation_language[0] != '\0';
}

static char *session_translation_strndup(const char *text, size_t length)
{
    char *copy = (char *)sshc_gc_malloc(length + 1U);
    if (copy != nullptr) {
        memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}

typedef struct translation_job_list {
    translation_job_t *head;
    translation_job_t *tail;
} translation_job_list_t;

static void session_translation_job_list_append(translation_job_list_t *list,
                                                translation_job_t *job)
{
    job->next = nullptr;
    if (list->tail != nullptr) {
        list->tail->next = job;
    } else {
        list->head = job;
    }
    list->tail = job;
}

// Hand a list of jobs to the worker in one step, so it sees all of them
// when it assembles its next batch.
static void session_translation_enqueue_list(session_ctx_t *ctx,
                                             translation_job_list_t *list)
{
    if (list->head == nullptr) {
        return;
    }

    ttak_mutex_lock(&ctx->translation_mutex);
    for (translation_job_t *job = list->head; job != nullptr; job = job->next) {
        job->generation = ctx->translation_generation;
    }
    if (ctx->translation_pending_tail != nullptr) {
        ctx->translation_pending_tail->next = list->head;
    } else {
        ctx->translation_pending_head = list->head;
    }
    ctx->translation_pending_tail = list->tail;
    ttak_cond_signal(&ctx->translation_cond);
    ttak_mutex_unlock(&ctx->translation_mutex);
    list->head = nullptr;
    list->tail = nullptr;
}

static void session_translation_enqueue(session_ctx_t *ctx,
                                        translation_job_t *job)
{
    translation_job_list_t list = {0};
    session_translation_job_list_append(&list, job);
    session_translation_enqueue_list(ctx, &list);
}

// Caller holds translation_mutex (or owns ctx exclusively).
static translation_chat_cache_entry_t *
session_translation_chat_cache_find_locked(session_ctx_t *ctx,
                                           uint64_t message_id)
{
    translation_chat_cache_t *cache = ctx->translation_chat_cache;
    if (cache == nullptr || message_id == 0U) {
        return nullptr;
    }

    for (size_t idx = 0U; idx < SSH_CHATTER_TRANSLATION_CHAT_CACHE_SIZE;
         ++idx) {
        if (cache->entries[idx].message_id == message_id) {
            return &cache->entries[idx];
        }
    }
    return nullptr;
}

static translation_chat_cache_entry_t *
session_translation_chat_cache_claim_locked(session_ctx_t *ctx,
                                            uint64_t message_id)
{
    translation_chat_cache_entry_t *slot =
        session_translation_chat_cache_find_locked(ctx, message_id);
    if (slot != nullptr) {
        return slot;
    }

    if (ctx->translation_chat_cache == nullptr) {
        ctx->translation_chat_cache = (translation_chat_cache_t *)
            sshc_gc_calloc(1U, sizeof(translation_chat_cache_t));
        if (ctx->translation_chat_cache == nullptr) {
            return nullptr;
        }
    }

    translation_chat_cache_t *cache = ctx->translation_chat_cache;
    slot = &cache->entries[cache->next_slot];
    cache->next_slot =
        (cache->next_slot + 1U) % SSH_CHATTER_TRANSLATION_CHAT_CACHE_SIZE;
    sshc_gc_free(slot->text);
    memset(slot, 0, sizeof(*slot));
    slot->message_id = message_id;
    return slot;
}

// Resolve what the reader actually sees for a history entry and decide
// whether it is a chat line worth translating for them.
static bool session_translation_chat_candidate(
    session_ctx_t *ctx, const chat_history_entry_t *entry,
    chat_history_entry_t *scratch, const chat_history_entry_t **view)
{
    if (ctx == nullptr || entry == nullptr || scratch == nullptr ||
        view == nullptr || chat_history_entry_is_empty(entry)) {
        return false;
    }

    if (!host_ddial_display_view(entry, scratch, view)) {
        return false;
    }

    const chat_history_entry_t *shown = *view;
    if (!shown->is_user_message || shown->message_id == 0U ||
        shown->message[0] == '\0') {
        return false;
    }

    // The reader's own lines are already in the reader's language.
    if (strcmp(chat_history_entry_display_name(shown), ctx->user.name) == 0) {
        return false;
    }

    if (shown->message[0] == '@') {
        char stripped[SSH_CHATTER_MESSAGE_LIMIT];
        if (translation_strip_no_translate_prefix(shown->message, stripped,
                                                  sizeof(stripped))) {
            return false;
        }
    }

    return !session_should_hide_entry(ctx, shown);
}

// Queue one incoming chat line for the batched chat translator. Lines already
// translated, in flight or failed for the current language are skipped. With
// a collect list the job is gathered there instead of being queued.
static bool session_translation_queue_chat_entry(
    session_ctx_t *ctx, const chat_history_entry_t *entry, bool backfill,
    translation_job_list_t *collect)
{
    if (!session_translation_output_active(ctx) ||
        ctx->translation_suppress_output) {
        return false;
    }

    chat_history_entry_t scratch;
    const chat_history_entry_t *view = entry;
    if (!session_translation_chat_candidate(ctx, entry, &scratch, &view)) {
        return false;
    }

    if (!session_translation_worker_ensure(ctx)) {
        return false;
    }

    ttak_mutex_lock(&ctx->translation_mutex);
    translation_chat_cache_entry_t *slot =
        session_translation_chat_cache_find_locked(ctx, view->message_id);
    if (slot != nullptr &&
        (slot->pending || slot->failed || slot->text != nullptr)) {
        ttak_mutex_unlock(&ctx->translation_mutex);
        return false;
    }
    slot = session_translation_chat_cache_claim_locked(ctx, view->message_id);
    if (slot != nullptr) {
        slot->pending = true;
    }
    ttak_mutex_unlock(&ctx->translation_mutex);
    if (slot == nullptr) {
        return false;
    }

    translation_job_t *job = session_translation_job_alloc();
    if (job == nullptr) {
        return false;
    }

    job->type = TRANSLATION_JOB_CHAT;
    snprintf(job->target_language, sizeof(job->target_language), "%s",
             ctx->output_translation_language);
    job->data.chat.message_id = view->message_id;
    job->data.chat.backfill = backfill;
    snprintf(job->data.chat.name, sizeof(job->data.chat.name), "%s",
             chat_history_entry_display_name(view));
    snprintf(job->data.chat.text, sizeof(job->data.chat.text), "%s",
             view->message);

    if (collect != nullptr) {
        session_translation_job_list_append(collect, job);
    } else {
        session_translation_enqueue(ctx, job);
    }
    return true;
}

// When translation turns on (or the language changes) queue the last
// SSH_CHATTER_TRANSLATION_BACKFILL_MESSAGES chat lines; the worker sends them
// together with anything that arrives meanwhile as one batch.
static void session_translation_backfill(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr ||
        ctx->translation_backfill_done ||
        !session_translation_output_active(ctx)) {
        return;
    }
    ctx->translation_backfill_done = true;

    const size_t total = host_history_total(ctx->owner);
    if (total == 0U) {
        return;
    }

    enum { kChunk = 32, kScanLimit = 1000 };
    chat_history_entry_t *buffer = (chat_history_entry_t *)sshc_gc_calloc(
        kChunk, sizeof(chat_history_entry_t));
    if (buffer == nullptr) {
        return;
    }

    // Walk backwards to find where the last N translatable lines begin.
    size_t start = total;
    size_t found = 0U;
    size_t scanned = 0U;
    chat_history_entry_t scratch;
    const chat_history_entry_t *view = nullptr;
    while (start > 0U && found < SSH_CHATTER_TRANSLATION_BACKFILL_MESSAGES &&
           scanned < kScanLimit) {
        size_t chunk = start >= (size_t)kChunk ? (size_t)kChunk : start;
        size_t copied =
            host_history_copy_range(ctx->owner, start - chunk, buffer, chunk);
        if (copied < chunk) {
            break;
        }
        size_t idx = chunk;
        while (idx > 0U && found < SSH_CHATTER_TRANSLATION_BACKFILL_MESSAGES) {
            --idx;
            ++scanned;
            if (session_translation_chat_candidate(ctx, &buffer[idx],
                                                   &scratch, &view)) {
                ++found;
            }
        }
        start = start - chunk + idx;
    }

    translation_job_list_t backlog = {0};
    for (size_t pos = start; pos < total; pos += (size_t)kChunk) {
        size_t chunk = total - pos;
        if (chunk > (size_t)kChunk) {
            chunk = (size_t)kChunk;
        }
        size_t copied = host_history_copy_range(ctx->owner, pos, buffer, chunk);
        for (size_t idx = 0U; idx < copied; ++idx) {
            (void)session_translation_queue_chat_entry(ctx, &buffer[idx],
                                                       true, &backlog);
        }
    }
    session_translation_enqueue_list(ctx, &backlog);
}

static uint64_t session_translation_fnv1a(uint64_t hash, const char *text);

static uint64_t session_translation_output_hash(const session_ctx_t *ctx)
{
    if (!ctx->has_last_output_line) {
        return 0U;
    }
    return session_translation_fnv1a(1469598103934665603ULL,
                                     ctx->last_output_line);
}

// Called after a chat line was written: remember it so its translation can
// be placed directly beneath it when nothing else was printed in between.
static void session_translation_note_chat_line(session_ctx_t *ctx,
                                               const chat_history_entry_t *entry)
{
    if (!session_translation_queue_chat_entry(ctx, entry, false, nullptr)) {
        return;
    }
    ctx->translation_tail_message_id = entry->message_id;
    ctx->translation_tail_output_hash = session_translation_output_hash(ctx);
}

// Copy the cached translation of a chat line, if one exists.
static bool session_translation_chat_caption(session_ctx_t *ctx,
                                             uint64_t message_id, char *buffer,
                                             size_t length)
{
    if (!session_translation_output_active(ctx) || buffer == nullptr ||
        length == 0U || !ctx->translation_mutex_initialized) {
        return false;
    }

    bool found = false;
    ttak_mutex_lock(&ctx->translation_mutex);
    translation_chat_cache_entry_t *slot =
        session_translation_chat_cache_find_locked(ctx, message_id);
    if (slot != nullptr && slot->text != nullptr) {
        snprintf(buffer, length, "%s", slot->text);
        found = true;
    }
    ttak_mutex_unlock(&ctx->translation_mutex);
    return found;
}

// Emit a translated chat line as caption rows ("    ↳ ..."), keeping later
// lines of a multi-line translation indented under the first.
static void session_translation_emit_caption(session_ctx_t *ctx,
                                             const char *prefix,
                                             const char *text,
                                             bool add_to_model,
                                             uint64_t message_id, bool emit)
{
    if (ctx == nullptr || text == nullptr) {
        return;
    }

    const char *cursor = text;
    bool first = true;
    while (cursor != nullptr) {
        const char *newline = strchr(cursor, '\n');
        size_t length =
            newline != nullptr ? (size_t)(newline - cursor) : strlen(cursor);
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(line, sizeof(line), "%s%s%.*s" ANSI_RESET,
                 first ? "    \342\206\263 " : "      ",
                 first && prefix != nullptr ? prefix : "", (int)length,
                 cursor);
        if (add_to_model && ctx->display_model_initialized) {
            unsigned int width =
                (ctx->terminal_width > 0U) ? ctx->terminal_width : 80U;
            display_model_append_message(&ctx->display_model, message_id,
                                         line, width);
        }
        if (emit) {
            session_send_plain_line(ctx, line);
        }
        first = false;
        cursor = newline != nullptr ? newline + 1 : nullptr;
    }
}

static uint64_t session_translation_fnv1a(uint64_t hash, const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text;
         p != nullptr && *p != '\0'; ++p) {
        hash ^= *p;
        hash *= 1099511628211ULL;
    }
    hash ^= 0xffU;
    hash *= 1099511628211ULL;
    return hash;
}

static uint64_t session_translation_bbs_fingerprint(const bbs_post_t *post)
{
    uint64_t hash = 1469598103934665603ULL;
    hash = session_translation_fnv1a(hash, post->title);
    hash = session_translation_fnv1a(hash, post->body);
    for (size_t idx = 0U; idx < post->comment_count; ++idx) {
        hash = session_translation_fnv1a(hash, post->comments[idx].text);
    }
    return hash ^ (uint64_t)post->comment_count;
}

// The finished translation of this exact post revision, or nullptr.
static const translation_bbs_cache_t *
session_translation_bbs_ready(session_ctx_t *ctx, const bbs_post_t *post)
{
    if (!session_translation_output_active(ctx) || post == nullptr) {
        return nullptr;
    }

    const translation_bbs_cache_t *cache = ctx->translation_bbs_cache;
    if (cache == nullptr || cache->pending || cache->failed ||
        cache->segments == nullptr || cache->post_id != post->id ||
        cache->fingerprint != session_translation_bbs_fingerprint(post) ||
        cache->segment_count != 1U + cache->body_chunks + post->comment_count) {
        return nullptr;
    }
    return cache;
}

// Split a post body into newline-aligned chunks the translator can handle.
static size_t session_translation_split_body(const char *body, char **out,
                                             size_t capacity)
{
    size_t count = 0U;
    size_t length = strlen(body);
    size_t pos = 0U;
    while (pos < length && count < capacity) {
        size_t end = length;
        size_t next = length;
        if (length - pos > SSH_CHATTER_TRANSLATION_BBS_CHUNK_BYTES) {
            end = pos + SSH_CHATTER_TRANSLATION_BBS_CHUNK_BYTES;
            size_t cut = end;
            while (cut > pos && body[cut] != '\n') {
                --cut;
            }
            if (cut > pos) {
                end = cut;
                next = cut + 1U;
            } else {
                while (end > pos &&
                       ((unsigned char)body[end] & 0xC0U) == 0x80U) {
                    --end;
                }
                next = end;
            }
        }
        out[count] = session_translation_strndup(body + pos, end - pos);
        if (out[count] == nullptr) {
            break;
        }
        ++count;
        pos = next;
    }
    return count;
}

// Queue the title, body and comments of a post as one batch. Returns true
// when a new request went out (the caller shows a "translating" notice).
static bool session_translation_queue_bbs_post(session_ctx_t *ctx,
                                               const bbs_post_t *post)
{
    if (!session_translation_output_active(ctx) || post == nullptr) {
        return false;
    }

    const uint64_t fingerprint = session_translation_bbs_fingerprint(post);
    const translation_bbs_cache_t *existing = ctx->translation_bbs_cache;
    if (existing != nullptr && existing->post_id == post->id &&
        existing->fingerprint == fingerprint) {
        return false;
    }

    if (!session_translation_worker_ensure(ctx)) {
        return false;
    }

    const size_t body_capacity =
        2U * (strlen(post->body) / SSH_CHATTER_TRANSLATION_BBS_CHUNK_BYTES) +
        2U;
    const size_t capacity = 1U + body_capacity + post->comment_count;
    char **segments = (char **)sshc_gc_calloc(capacity, sizeof(char *));
    translation_bbs_cache_t *cache = (translation_bbs_cache_t *)sshc_gc_calloc(
        1U, sizeof(translation_bbs_cache_t));
    translation_job_t *job = session_translation_job_alloc();
    if (segments == nullptr || cache == nullptr || job == nullptr) {
        sshc_gc_free(segments);
        sshc_gc_free(cache);
        sshc_gc_free(job);
        return false;
    }

    size_t count = 0U;
    segments[count++] =
        session_translation_strndup(post->title, strlen(post->title));
    size_t body_chunks = session_translation_split_body(
        post->body, &segments[count], body_capacity);
    count += body_chunks;
    for (size_t idx = 0U; idx < post->comment_count; ++idx) {
        const char *text = post->comments[idx].text;
        segments[count++] = session_translation_strndup(text, strlen(text));
    }
    for (size_t idx = 0U; idx < count; ++idx) {
        if (segments[idx] == nullptr) {
            session_translation_free_bbs_segments(segments, count);
            sshc_gc_free(cache);
            sshc_gc_free(job);
            return false;
        }
    }

    // Drop the previous post's cache (and any translation it held) now that
    // this request is going out; reset_cache no longer sees it.
    if (ctx->translation_bbs_cache != nullptr) {
        session_translation_free_bbs_segments(
            ctx->translation_bbs_cache->segments,
            ctx->translation_bbs_cache->segment_count);
        sshc_gc_free(ctx->translation_bbs_cache);
    }

    cache->post_id = post->id;
    cache->fingerprint = fingerprint;
    cache->pending = true;
    ctx->translation_bbs_cache = cache;

    job->type = TRANSLATION_JOB_BBS_POST;
    snprintf(job->target_language, sizeof(job->target_language), "%s",
             ctx->output_translation_language);
    job->data.bbs.post_id = post->id;
    job->data.bbs.fingerprint = fingerprint;
    job->data.bbs.segments = segments;
    job->data.bbs.segment_count = count;
    job->data.bbs.body_chunks = body_chunks;

    session_translation_enqueue(ctx, job);
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

// True when chat translations may be written to the screen right now.
static bool session_translation_chat_visible(const session_ctx_t *ctx)
{
    return ctx->history_scroll_position == 0U &&
           ctx->editor_mode == SESSION_EDITOR_MODE_NONE &&
           !ctx->bbs_rendering_editor && !session_in_protected_section(ctx) &&
           (!ctx->display_model_initialized ||
            display_model_is_following_tail(&ctx->display_model));
}

// Repaint the chat tail so the freshly translated backlog shows inline.
static void session_translation_repaint_chat(session_ctx_t *ctx)
{
    session_clear_screen(ctx);
    session_apply_background_fill(ctx);
    ctx->pending_should_sink = true;
    session_process_pending_sink(ctx);
}

static void session_translation_apply_chat_result(session_ctx_t *ctx,
                                                  const translation_result_t *ready,
                                                  bool show_live,
                                                  bool *refreshed)
{
    char *text = nullptr;
    if (ready->success) {
        text = session_translation_strndup(ready->translated,
                                           strlen(ready->translated));
    }

    ttak_mutex_lock(&ctx->translation_mutex);
    translation_chat_cache_entry_t *slot =
        session_translation_chat_cache_find_locked(ctx, ready->message_id);
    if (slot != nullptr) {
        slot->pending = false;
        slot->text = text;
        slot->failed = (text == nullptr);
    }
    ttak_mutex_unlock(&ctx->translation_mutex);

    if (!show_live || text == nullptr || slot == nullptr) {
        return;
    }

    // Right under its line the caption needs no label; anywhere else it says
    // which line it belongs to.
    const bool adjacent =
        ready->message_id == ctx->translation_tail_message_id &&
        session_translation_output_hash(ctx) ==
            ctx->translation_tail_output_hash;
    char prefix[SSH_CHATTER_USERNAME_LEN + 64U];
    prefix[0] = '\0';
    if (!adjacent) {
        char id_label[32];
        if (!host_compact_id_encode(ready->message_id, id_label,
                                    sizeof(id_label))) {
            snprintf(id_label, sizeof(id_label), "-");
        }
        snprintf(prefix, sizeof(prefix), ANSI_CYAN "[%s]" ANSI_RESET " <%s> ",
                 id_label, ready->chat_name);
    }
    session_translation_emit_caption(ctx, prefix, text, false, 0U, true);
    ctx->translation_tail_message_id = 0U;
    *refreshed = true;
}

static void session_translation_apply_bbs_result(session_ctx_t *ctx,
                                                 const translation_result_t *ready)
{
    translation_bbs_cache_t *cache = ctx->translation_bbs_cache;
    // A result only feeds the cache when it belongs to the current request:
    // reset_cache bumps the generation and drops the cache together, so a
    // stale result can at most coincidentally match the new cache's post.
    const bool cache_matches =
        cache != nullptr && cache->post_id == ready->post_id &&
        cache->fingerprint == ready->fingerprint &&
        ready->generation == ctx->translation_generation;

    if (cache_matches) {
        cache->pending = false;
        cache->failed = !ready->success;
        if (ready->success) {
            // Ownership of the segment strings and their array moves from
            // the result node to the cache; reset_cache releases them.
            cache->segments = ready->segments;
            cache->segment_count = ready->segment_count;
            cache->body_chunks = ready->body_chunks;
        } else {
            session_translation_free_bbs_segments(ready->segments,
                                                  ready->segment_count);
        }
    } else {
        // Stale or superseded result: the node still owns its segments.
        session_translation_free_bbs_segments(ready->segments,
                                              ready->segment_count);
    }

    if (!cache_matches || !ctx->bbs_view_active ||
        ctx->bbs_view_post_id != ready->post_id ||
        ctx->editor_mode != SESSION_EDITOR_MODE_NONE ||
        ctx->bbs_rendering_editor) {
        return;
    }

    if (ready->success) {
        (void)session_bbs_refresh_view(ctx);
    } else {
        session_send_system_line(ctx, ready->error_message);
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
            sshc_gc_free(ready);
            ready = next;
            continue;
        }

        if (ready->type == TRANSLATION_JOB_PRIVATE_MESSAGE) {
            session_ctx_t *target = nullptr;
            if (ctx->owner != nullptr && ready->pm_target_name[0] != '\0') {
                target = chat_room_find_user_ref(&ctx->owner->room,
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

            if (target != nullptr) {
                chat_room_release_user_ref(target);
            }
            refreshed = true;
            sshc_gc_free(ready);
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

        sshc_gc_free(ready);
        ready = next;
    }

    if (refreshed && ctx->history_scroll_position == 0U) {
        session_refresh_input_line(ctx);
    }
}

// Session-loop tick for batched chat/BBS translation: start the backlog
// batch when translation turns on and show whatever came back.
static void session_translation_flush_batches(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    session_translation_backfill(ctx);

    if (!ctx->translation_mutex_initialized) {
        return;
    }

    ttak_mutex_lock(&ctx->translation_mutex);
    translation_result_t *ready = ctx->translation_batch_ready_head;
    ctx->translation_batch_ready_head = nullptr;
    ctx->translation_batch_ready_tail = nullptr;
    ttak_mutex_unlock(&ctx->translation_mutex);

    if (ready == nullptr) {
        return;
    }

    // A translated backlog is shown by repainting the chat tail, which also
    // covers live lines from the same batch; otherwise live lines get their
    // translation appended as they come back.
    const bool chat_visible = session_translation_output_active(ctx) &&
                              session_translation_chat_visible(ctx);
    bool backfill_ready = false;
    for (const translation_result_t *scan = ready; scan != nullptr;
         scan = scan->next) {
        if (scan->type == TRANSLATION_JOB_CHAT && scan->backfill &&
            scan->success &&
            scan->generation == ctx->translation_generation) {
            backfill_ready = true;
        }
    }

    bool refreshed = false;
    while (ready != nullptr) {
        translation_result_t *next = ready->next;
        if (ready->type == TRANSLATION_JOB_CHAT) {
            if (ready->generation == ctx->translation_generation) {
                session_translation_apply_chat_result(
                    ctx, ready, chat_visible && !backfill_ready, &refreshed);
            }
        } else if (ready->type == TRANSLATION_JOB_BBS_POST) {
            // Keeps or frees the segments; stale results are dropped here.
            session_translation_apply_bbs_result(ctx, ready);
        }
        sshc_gc_free(ready);
        ready = next;
    }

    if (backfill_ready && chat_visible) {
        session_translation_repaint_chat(ctx);
    } else if (refreshed) {
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

    // The worker frees the batch jobs once this returns, on every path.
    if (ctx->translation_thread_stop) {
        return true;
    }

    char *combined =
        sshc_gc_calloc(SSH_CHATTER_TRANSLATION_BATCH_BUFFER, sizeof(char));
    char *translated =
        sshc_gc_calloc(SSH_CHATTER_TRANSLATION_BATCH_BUFFER, sizeof(char));
    if (combined == nullptr || translated == nullptr) {
        sshc_gc_free(combined);
        sshc_gc_free(translated);
        return false;
    }

    // False means "not handled, fall back to per-job translation"; the
    // abandoned_jobs flag instead reports a stop request: nothing is
    // published, and the caller frees the jobs after this returns.
    bool handled = true;
    bool abandoned_jobs = false;

    size_t offset = 0U;
    for (size_t idx = 0U; idx < job_count; ++idx) {
        if (ctx->translation_thread_stop) {
            abandoned_jobs = true;
            goto done;
        }
        if (jobs[idx] == nullptr ||
            jobs[idx]->type != TRANSLATION_JOB_CAPTION) {
            handled = false;
            goto done;
        }

        char marker[32];
        int marker_len =
            snprintf(marker, sizeof(marker), "[[SEG%02zu]]\n", idx);
        if (marker_len < 0) {
            handled = false;
            goto done;
        }

        size_t marker_size = (size_t)marker_len;
        size_t text_len = strlen(jobs[idx]->data.caption.sanitized);
        if (offset + marker_size + text_len + 1U >
            SSH_CHATTER_TRANSLATION_BATCH_BUFFER) {
            handled = false;
            goto done;
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
            abandoned_jobs = true;
            goto done;
        }
        handled = false;
        goto done;
    }

    if (ctx->translation_thread_stop) {
        abandoned_jobs = true;
        goto done;
    }

    char *segment_starts[SSH_CHATTER_TRANSLATION_BATCH_MAX] = {0};
    char *segment_ends[SSH_CHATTER_TRANSLATION_BATCH_MAX] = {0};

    char *search_cursor = translated;
    for (size_t idx = 0U; idx < job_count; ++idx) {
        char marker[32];
        int marker_len = snprintf(marker, sizeof(marker), "[[SEG%02zu]]", idx);
        if (marker_len < 0) {
            handled = false;
            goto done;
        }

        char *marker_pos = strstr(search_cursor, marker);
        if (marker_pos == nullptr) {
            handled = false;
            goto done;
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
            handled = false;
            goto done;
        }

        char *next_pos = strstr(segment_starts[idx], marker);
        if (next_pos == nullptr) {
            handled = false;
            goto done;
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
            handled = false;
            goto done;
        }

        size_t segment_len = (size_t)(segment_ends[idx] - segment_starts[idx]);
        if (segment_len + 1U > SSH_CHATTER_TRANSLATION_WORKING_LEN) {
            handled = false;
            goto done;
        }

        char segment_buffer[SSH_CHATTER_TRANSLATION_WORKING_LEN];
        memcpy(segment_buffer, segment_starts[idx], segment_len);
        segment_buffer[segment_len] = '\0';

        if (!translation_restore_text(
                segment_buffer, restored_segments[idx],
                sizeof(restored_segments[idx]),
                jobs[idx]->data.caption.placeholders,
                jobs[idx]->data.caption.placeholder_count)) {
            handled = false;
            goto done;
        }
    }

    if (ctx->translation_thread_stop) {
        abandoned_jobs = true;
        goto done;
    }

    for (size_t idx = 0U; idx < job_count; ++idx) {
        session_translation_publish_result(
            ctx, jobs[idx], restored_segments[idx], nullptr, nullptr, true);
    }

done:
    sshc_gc_free(combined);
    sshc_gc_free(translated);
    return handled || abandoned_jobs;
}

typedef struct translation_segment {
    const char *source;
    char *sanitized;
    translation_placeholder_t
        placeholders[SSH_CHATTER_MAX_TRANSLATION_PLACEHOLDERS];
    size_t placeholder_count;
    char *output;
} translation_segment_t;

// Chat and BBS batch results have their own queue: they may repaint the
// screen, so only the session loop drains them, never a nested output call.
static void session_translation_push_results(session_ctx_t *ctx,
                                             translation_result_t *head,
                                             translation_result_t *tail)
{
    ttak_mutex_lock(&ctx->translation_mutex);
    tail->next = nullptr;
    if (ctx->translation_batch_ready_tail != nullptr) {
        ctx->translation_batch_ready_tail->next = head;
    } else {
        ctx->translation_batch_ready_head = head;
    }
    ctx->translation_batch_ready_tail = tail;
    ttak_mutex_unlock(&ctx->translation_mutex);
}

static void session_translation_push_result(session_ctx_t *ctx,
                                            translation_result_t *result)
{
    session_translation_push_results(ctx, result, result);
}

// Hide ANSI sequences behind [[ANSIn]] tokens; text with more sequences than
// placeholders is sent with the sequences dropped instead.
static bool session_translation_segment_prepare(translation_segment_t *segment)
{
    const size_t length = strlen(segment->source);
    const size_t capacity = length * 5U + 64U;
    segment->sanitized = (char *)sshc_gc_malloc(capacity);
    if (segment->sanitized == nullptr) {
        return false;
    }

    if (translation_prepare_text(segment->source, segment->sanitized,
                                 capacity, segment->placeholders,
                                 &segment->placeholder_count)) {
        return true;
    }

    size_t out = 0U;
    for (size_t idx = 0U; segment->source[idx] != '\0';) {
        if (segment->source[idx] == '\033') {
            ++idx;
            if (segment->source[idx] == '[') {
                ++idx;
                while (segment->source[idx] != '\0') {
                    unsigned char ch = (unsigned char)segment->source[idx++];
                    if (ch >= '@' && ch <= '~') {
                        break;
                    }
                }
            } else if (segment->source[idx] != '\0') {
                ++idx;
            }
            continue;
        }
        segment->sanitized[out++] = segment->source[idx++];
    }
    segment->sanitized[out] = '\0';
    segment->placeholder_count = 0U;
    return true;
}

static bool session_translation_segment_restore(translation_segment_t *segment,
                                                const char *translated,
                                                size_t length)
{
    char *raw = session_translation_strndup(translated, length);
    size_t capacity = length + 1U +
                      segment->placeholder_count *
                          SSH_CHATTER_PLACEHOLDER_SEQUENCE_LEN;
    char *restored = (char *)sshc_gc_malloc(capacity);
    if (raw == nullptr || restored == nullptr) {
        sshc_gc_free(raw);
        sshc_gc_free(restored);
        return false;
    }

    if (!translation_restore_text(raw, restored, capacity,
                                  segment->placeholders,
                                  segment->placeholder_count)) {
        sshc_gc_free(raw);
        sshc_gc_free(restored);
        return false;
    }
    // raw was only a mutable copy for the restore; the result never aliases
    // it, so it can go once the text has been copied out.
    sshc_gc_free(raw);
    session_translation_normalize_output(restored);
    segment->output = restored;
    return true;
}

typedef struct translation_group_state {
    bool abort;
    unsigned int consecutive_failures;
} translation_group_state_t;

// Give up on the rest of the batch when the provider is out of quota, the
// session is closing, or requests keep failing (the provider is down).
static void session_translation_note_failure(session_ctx_t *ctx,
                                             translation_group_state_t *state)
{
    if (ctx->translation_thread_stop) {
        state->abort = true;
        return;
    }
    if (translator_last_error_was_quota()) {
        session_handle_translation_quota_exhausted(ctx,
                                                   translator_last_error());
        state->abort = true;
        return;
    }
    if (++state->consecutive_failures >= 3U) {
        state->abort = true;
    }
}

// Translate a group of segments in one request, delimited by [[SEGnnn]]
// markers. Segments the reply lost are retried; a failed request is split in
// halves so one bad line or an oversized reply cannot sink the whole batch.
static void session_translation_translate_group(session_ctx_t *ctx,
                                                const char *language,
                                                translation_segment_t **group,
                                                size_t count,
                                                translation_group_state_t *state)
{
    if (count == 0U || state->abort || ctx->translation_thread_stop) {
        return;
    }

    if (count == 1U) {
        translation_segment_t *segment = group[0];
        size_t capacity = strlen(segment->sanitized) * 4U + 1024U;
        char *translated = (char *)sshc_gc_malloc(capacity);
        if (translated == nullptr) {
            return;
        }
        if (!translator_translate_with_cancel(segment->sanitized, language,
                                              translated, capacity, nullptr,
                                              0U,
                                              &ctx->translation_thread_stop)) {
            sshc_gc_free(translated);
            session_translation_note_failure(ctx, state);
            return;
        }
        state->consecutive_failures = 0U;
        (void)session_translation_segment_restore(segment, translated,
                                                  strlen(translated));
        sshc_gc_free(translated);
        return;
    }

    size_t combined_len = 1U;
    for (size_t idx = 0U; idx < count; ++idx) {
        combined_len += strlen(group[idx]->sanitized) + 16U;
    }
    char *combined = (char *)sshc_gc_malloc(combined_len);
    size_t capacity = combined_len * 4U + 4096U;
    char *translated = (char *)sshc_gc_malloc(capacity);
    if (combined == nullptr || translated == nullptr) {
        sshc_gc_free(combined);
        sshc_gc_free(translated);
        return;
    }

    size_t offset = 0U;
    for (size_t idx = 0U; idx < count; ++idx) {
        int written = snprintf(combined + offset, combined_len - offset,
                               "[[SEG%03zu]]\n%s\n", idx, group[idx]->sanitized);
        if (written < 0 || (size_t)written >= combined_len - offset) {
            sshc_gc_free(combined);
            sshc_gc_free(translated);
            return;
        }
        offset += (size_t)written;
    }

    const size_t half = count / 2U;
    if (!translator_translate_with_cancel(combined, language, translated,
                                          capacity, nullptr, 0U,
                                          &ctx->translation_thread_stop)) {
        sshc_gc_free(combined);
        sshc_gc_free(translated);
        session_translation_note_failure(ctx, state);
        session_translation_translate_group(ctx, language, group, half, state);
        session_translation_translate_group(ctx, language, group + half,
                                            count - half, state);
        return;
    }
    state->consecutive_failures = 0U;

    translation_segment_t **missing = (translation_segment_t **)sshc_gc_calloc(
        count, sizeof(translation_segment_t *));
    if (missing == nullptr) {
        sshc_gc_free(combined);
        sshc_gc_free(translated);
        return;
    }
    size_t missing_count = 0U;
    for (size_t idx = 0U; idx < count; ++idx) {
        char marker[24];
        int marker_len = snprintf(marker, sizeof(marker), "[[SEG%03zu]]", idx);
        const char *start = strstr(translated, marker);
        if (marker_len <= 0 || start == nullptr) {
            missing[missing_count++] = group[idx];
            continue;
        }
        start += marker_len;
        while (*start == '\r' || *start == '\n' || *start == ' ') {
            ++start;
        }
        const char *end = strstr(start, "[[SEG");
        if (end == nullptr) {
            end = start + strlen(start);
        }
        while (end > start && (end[-1] == '\r' || end[-1] == '\n' ||
                               end[-1] == ' ')) {
            --end;
        }
        if (!session_translation_segment_restore(group[idx], start,
                                                 (size_t)(end - start))) {
            missing[missing_count++] = group[idx];
        }
    }

    if (missing_count == count) {
        session_translation_translate_group(ctx, language, group, half, state);
        session_translation_translate_group(ctx, language, group + half,
                                            count - half, state);
    } else if (missing_count > 0U) {
        session_translation_translate_group(ctx, language, missing,
                                            missing_count, state);
    }

    sshc_gc_free(missing);
    sshc_gc_free(combined);
    sshc_gc_free(translated);
}

// Translate every segment, packing as many as fit the request budget into
// each call. Returns how many segments ended up with a translation.
static size_t session_translation_translate_segments(
    session_ctx_t *ctx, const char *language, translation_segment_t *segments,
    size_t count)
{
    translation_segment_t **group = (translation_segment_t **)sshc_gc_calloc(
        count + 1U, sizeof(translation_segment_t *));
    if (group == nullptr) {
        return 0U;
    }

    translation_group_state_t state = {0};
    size_t group_count = 0U;
    size_t group_bytes = 0U;
    for (size_t idx = 0U; idx <= count && !state.abort; ++idx) {
        size_t bytes = 0U;
        if (idx < count) {
            if (!session_translation_segment_prepare(&segments[idx])) {
                continue;
            }
            bytes = strlen(segments[idx].sanitized);
            if (bytes == 0U) {
                segments[idx].output = session_translation_strndup("", 0U);
                continue;
            }
        }
        if (group_count > 0U &&
            (idx == count ||
             group_bytes + bytes > SSH_CHATTER_TRANSLATION_CHAT_BATCH_BYTES)) {
            session_translation_translate_group(ctx, language, group,
                                                group_count, &state);
            group_count = 0U;
            group_bytes = 0U;
        }
        if (idx < count) {
            group[group_count++] = &segments[idx];
            group_bytes += bytes;
        }
    }

    size_t translated = 0U;
    for (size_t idx = 0U; idx < count; ++idx) {
        if (segments[idx].output != nullptr) {
            ++translated;
        }
    }
    // group only borrows pointers into the caller's segments array.
    sshc_gc_free(group);
    return translated;
}

static void session_translation_process_chat_batch(session_ctx_t *ctx,
                                                   translation_job_t **jobs,
                                                   size_t job_count)
{
    translation_segment_t *segments = (translation_segment_t *)sshc_gc_calloc(
        job_count, sizeof(translation_segment_t));
    if (segments == nullptr) {
        return;
    }
    for (size_t idx = 0U; idx < job_count; ++idx) {
        segments[idx].source = jobs[idx]->data.chat.text;
    }

    (void)session_translation_translate_segments(
        ctx, jobs[0]->target_language, segments, job_count);

    if (!ctx->translation_thread_stop) {
        // Hand the whole batch over at once so the session sees it in one
        // flush. Results copy the text out, so the segment buffers can be
        // released once this loop finishes.
        translation_result_t *head = nullptr;
        translation_result_t *tail = nullptr;
        for (size_t idx = 0U; idx < job_count; ++idx) {
            translation_result_t *result = session_translation_result_alloc();
            if (result == nullptr) {
                continue;
            }
            result->type = TRANSLATION_JOB_CHAT;
            result->generation = jobs[idx]->generation;
            result->message_id = jobs[idx]->data.chat.message_id;
            result->backfill = jobs[idx]->data.chat.backfill;
            snprintf(result->chat_name, sizeof(result->chat_name), "%s",
                     jobs[idx]->data.chat.name);
            result->success = segments[idx].output != nullptr;
            if (result->success) {
                snprintf(result->translated, sizeof(result->translated), "%s",
                         segments[idx].output);
            }
            if (tail != nullptr) {
                tail->next = result;
            } else {
                head = result;
            }
            tail = result;
        }
        if (head != nullptr) {
            session_translation_push_results(ctx, head, tail);
        }
    }

    for (size_t idx = 0U; idx < job_count; ++idx) {
        sshc_gc_free(segments[idx].sanitized);
        sshc_gc_free(segments[idx].output);
    }
    sshc_gc_free(segments);
}

static void session_translation_process_bbs_job(session_ctx_t *ctx,
                                                translation_job_t *job)
{
    const size_t count = job->data.bbs.segment_count;
    translation_segment_t *segments = (translation_segment_t *)sshc_gc_calloc(
        count, sizeof(translation_segment_t));
    char **output = (char **)sshc_gc_calloc(count, sizeof(char *));
    if (segments == nullptr || output == nullptr) {
        sshc_gc_free(segments);
        sshc_gc_free(output);
        return;
    }
    for (size_t idx = 0U; idx < count; ++idx) {
        segments[idx].source = job->data.bbs.segments[idx];
    }

    size_t translated = session_translation_translate_segments(
        ctx, job->target_language, segments, count);

    if (!ctx->translation_thread_stop) {
        // Anything the translator could not handle is shown as written. The
        // result must own every string it carries (the cache frees the list
        // wholesale later), so untranslated segments are copied rather than
        // borrowed from the job. A copy failure transfers the string to the
        // result instead: the job forgets it, so it is freed exactly once.
        for (size_t idx = 0U; idx < count; ++idx) {
            if (segments[idx].output != nullptr) {
                output[idx] = segments[idx].output;
                segments[idx].output = nullptr;
            } else {
                output[idx] = session_translation_strndup(
                    job->data.bbs.segments[idx],
                    strlen(job->data.bbs.segments[idx]));
                if (output[idx] == nullptr) {
                    output[idx] = job->data.bbs.segments[idx];
                    job->data.bbs.segments[idx] = nullptr;
                }
            }
        }

        translation_result_t *result = session_translation_result_alloc();
        if (result != nullptr) {
            result->type = TRANSLATION_JOB_BBS_POST;
            result->generation = job->generation;
            result->success = translated > 0U;
            result->post_id = job->data.bbs.post_id;
            result->fingerprint = job->data.bbs.fingerprint;
            result->segments = output;
            result->segment_count = count;
            result->body_chunks = job->data.bbs.body_chunks;
            if (!result->success) {
                const char *error = translator_last_error();
                snprintf(result->error_message, sizeof(result->error_message),
                         "[!] post translation failed%s%s",
                         error != nullptr && error[0] != '\0' ? ": " : ".",
                         error != nullptr ? error : "");
            }
            session_translation_push_result(ctx, result);
            output = nullptr;
        }
    }

    for (size_t idx = 0U; idx < count; ++idx) {
        sshc_gc_free(segments[idx].sanitized);
        sshc_gc_free(segments[idx].output);
    }
    sshc_gc_free(segments);
    // Leftovers only: a pushed result took its segments with it.
    session_translation_free_bbs_segments(output, count);
}

// Take the first chat job plus every chat job queued behind it (the backfill
// and whatever arrived meanwhile) and translate them as one batch.
static void session_translation_collect_chat_batch(session_ctx_t *ctx,
                                                   translation_job_t *first)
{
    translation_job_t **jobs = (translation_job_t **)sshc_gc_calloc(
        SSH_CHATTER_TRANSLATION_CHAT_BATCH_MAX, sizeof(translation_job_t *));
    if (jobs == nullptr) {
        session_translation_free_job(first);
        return;
    }
    size_t count = 0U;
    jobs[count++] = first;
    size_t bytes = strlen(first->data.chat.text);

    bool idle = false;
    ttak_mutex_lock(&ctx->translation_mutex);
    idle = !ctx->translation_thread_stop &&
           ctx->translation_pending_head == nullptr;
    ttak_mutex_unlock(&ctx->translation_mutex);
    if (idle) {
        // Give messages arriving right now a moment to join this request.
        struct timespec aggregation_delay = {
            .tv_sec = 0, .tv_nsec = SSH_CHATTER_TRANSLATION_BATCH_DELAY_NS};
        host_sleep_uninterruptible(&aggregation_delay);
    }

    ttak_mutex_lock(&ctx->translation_mutex);
    while (count < SSH_CHATTER_TRANSLATION_CHAT_BATCH_MAX &&
           ctx->translation_pending_head != nullptr) {
        translation_job_t *candidate = ctx->translation_pending_head;
        if (candidate->type != TRANSLATION_JOB_CHAT ||
            strcmp(candidate->target_language, first->target_language) != 0) {
            break;
        }
        size_t candidate_bytes = strlen(candidate->data.chat.text);
        if (bytes + candidate_bytes > SSH_CHATTER_TRANSLATION_CHAT_BATCH_BYTES) {
            break;
        }
        ctx->translation_pending_head = candidate->next;
        if (ctx->translation_pending_head == nullptr) {
            ctx->translation_pending_tail = nullptr;
        }
        candidate->next = nullptr;
        jobs[count++] = candidate;
        bytes += candidate_bytes;
    }
    ttak_mutex_unlock(&ctx->translation_mutex);

    session_translation_process_chat_batch(ctx, jobs, count);

    // Every job the batch collected (first included) was removed from the
    // pending list under translation_mutex above and is referenced nowhere
    // else; the results copied out what they needed.
    for (size_t idx = 0U; idx < count; ++idx) {
        session_translation_free_job(jobs[idx]);
    }
    sshc_gc_free(jobs);
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

        if (batch[0]->type == TRANSLATION_JOB_BBS_POST) {
            session_translation_process_bbs_job(ctx, batch[0]);
            session_translation_free_job(batch[0]);
            continue;
        }

        if (batch[0]->type == TRANSLATION_JOB_CHAT) {
            // collect_chat_batch frees first and everything it collects.
            session_translation_collect_chat_batch(ctx, batch[0]);
            continue;
        }

        if (batch[0]->type != TRANSLATION_JOB_CAPTION) {
            session_translation_process_single_job(ctx, batch[0]);
            session_translation_free_job(batch[0]);
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

        // The batch jobs left the pending list under translation_mutex, so
        // the worker is their sole owner: free them on every path, including
        // the thread-stop abandons inside process_batch (which publishes
        // nothing but leaves the array entries in place for this cleanup).
        for (size_t idx = 0U; idx < batch_count; ++idx) {
            session_translation_free_job(batch[idx]);
        }
    }

    if (memory_scope != nullptr) {
        sshc_memory_context_pop(memory_scope);
    }
    return nullptr;
}


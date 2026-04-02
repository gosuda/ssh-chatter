static bool host_eliza_enable(host_t *host)
{
    if (host == nullptr) {
        return false;
    }

    bool changed = false;
    bool announce = false;

    ttak_mutex_lock(&host->lock);
    if (!atomic_load(&host->eliza_enabled)) {
        atomic_store(&host->eliza_enabled, true);
        changed = true;
    }
    if (!atomic_load(&host->eliza_announced)) {
        atomic_store(&host->eliza_announced, true);
        announce = true;
    }
    if (changed) {
        host_eliza_state_save_locked(host);
    }
    ttak_mutex_unlock(&host->lock);

    if (announce) {
        host_eliza_announce_join(host);
    }

    return changed;
}

static bool host_eliza_disable(host_t *host)
{
    if (host == nullptr) {
        return false;
    }

    bool changed = false;
    bool announce_depart = false;

    ttak_mutex_lock(&host->lock);
    if (atomic_load(&host->eliza_enabled)) {
        changed = true;
    }
    atomic_store(&host->eliza_enabled, false);
    if (atomic_load(&host->eliza_announced)) {
        announce_depart = true;
    }
    atomic_store(&host->eliza_announced, false);
    if (changed) {
        host_eliza_state_save_locked(host);
    }
    ttak_mutex_unlock(&host->lock);

    if (announce_depart) {
        host_eliza_announce_depart(host);
    }

    return changed;
}

static void host_eliza_announce_join(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    host_history_record_system(host, "* [eliza] has joined the chat", nullptr);
    host_eliza_say(host,
                   "Hey everyone, I'm eliza. Just another chatter keeping "
                   "an eye on things.");
}

static void host_eliza_announce_depart(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    host_eliza_say(host, "I'm heading out. Stay safe!");
    host_history_record_system(host, "* [eliza] has left the chat", nullptr);
}

static void host_eliza_say(host_t *host, const char *message)
{
    if (host == nullptr || message == nullptr || message[0] == '\0') {
        return;
    }

    if (!host_post_client_message(host, "eliza", message, nullptr, nullptr,
                                  false)) {
        printf("[eliza] failed to deliver message: %s\n", message);
    }
}

static __attribute__((unused)) void
host_eliza_prepare_private_reply(const char *message, char *reply,
                                 size_t reply_length)
{
    if (reply == nullptr || reply_length == 0U) {
        return;
    }

    reply[0] = '\0';

    if (message == nullptr) {
        snprintf(reply, reply_length,
                 "I'm listening. Let me know what's going on.");
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", message);
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        snprintf(reply, reply_length,
                 "I'm here if you want to talk about anything.");
        return;
    }

    if (translator_eliza_respond(working, reply, reply_length)) {
        trim_whitespace_inplace(reply);
        if (reply[0] != '\0') {
            return;
        }
    } else {
        const char *error = translator_last_error();
        if (error != nullptr && error[0] != '\0') {
            printf("[eliza] AI backend error: %s\n", error);
        }
    }

    const bool says_hello =
        string_contains_case_insensitive(working, "hello") ||
        string_contains_case_insensitive(working, "hi") ||
        string_contains_case_insensitive(working, "안녕");
    const bool asks_help = string_contains_case_insensitive(working, "help") ||
                           string_contains_case_insensitive(working, "도와");
    const bool expresses_thanks =
        string_contains_case_insensitive(working, "thank") ||
        string_contains_case_insensitive(working, "고마");
    const bool asks_question = strchr(working, '?') != nullptr;

    if (says_hello) {
        snprintf(reply, reply_length,
                 "Hi there! I'm here if you need anything.");
        return;
    }

    if (expresses_thanks) {
        snprintf(reply, reply_length,
                 "You're welcome. I'm glad to help keep things calm.");
        return;
    }

    if (asks_help) {
        snprintf(reply, reply_length,
                 "Tell me what's happening and I'll see how I can help.");
        return;
    }

    if (asks_question) {
        snprintf(reply, reply_length,
                 "That's a thoughtful question. What do you think about it?");
        return;
    }

    snprintf(reply, reply_length,
             "I'm listening. Share anything that's on your mind.");
}

static __attribute__((unused)) bool
host_eliza_content_is_severe(const char *text)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }

    char formatted_prompt[SSH_CHATTER_MESSAGE_LIMIT];
    char reply[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(formatted_prompt, SSH_CHATTER_MESSAGE_LIMIT,
             "You are a modbot of this mesage. If you think that this message "
             "is containing"
             "detailed crime plan that must happen, or illegally made child "
             "abuse video so"
             "sysops must take heavy legal responsibility from this,"
             "You should say \"Melon.\""
             "If you think that this is okay(or you are unsure)"
             "You should say \"Pear.\""
             "Here's the message: %s",
             text);
    if (!translator_eliza_respond(formatted_prompt, reply, sizeof(reply))) {
        if (string_contains_case_insensitive(text, "melon")) {
            return true;
        }
    }
    if (string_contains_case_insensitive(text, "child")) {
        if (string_contains_case_insensitive(text, "exploitation") ||
            string_contains_case_insensitive(text, "abuse") ||
            string_contains_case_insensitive(text, "porn")) {
            if (string_contains_case_insensitive(text, "http"))
                return true;
        }
    }

    if (string_contains_case_insensitive(text, "아청물") &&
        string_contains_case_insensitive(text, "http"))
        return true;
    if (string_contains_case_insensitive(text, "아동")) {
        if (string_contains_case_insensitive(text, "초딩") ||
            string_contains_case_insensitive(text, "중딩")) {
            if (string_contains_case_insensitive(text, "http")) {
                return true;
            }
        }
    }

    return false;
}

typedef struct host_eliza_intervene_task {
    struct host_eliza_intervene_task *next;
    session_ctx_t *ctx;
    bool from_filter;
    bool allocated_with_gc;
    char reason[SSH_CHATTER_MESSAGE_LIMIT];
} host_eliza_intervene_task_t;

static void host_eliza_task_free(host_eliza_intervene_task_t *task)
{
    (void)task;
}

static bool host_eliza_worker_init(host_t *host)
{
    if (host == nullptr) {
        return false;
    }

    host_eliza_worker_state_t *worker = &host->eliza_worker;
    if (worker->thread_started) {
        return true;
    }

    worker->head = nullptr;
    worker->tail = nullptr;
    worker->mutex_initialized = false;
    worker->cond_initialized = false;
    worker->thread_started = false;
    atomic_store(&worker->stop, false);
    atomic_store(&worker->active, false);

    if (ttak_mutex_init(&worker->mutex) != 0) {
        return false;
    }
    worker->mutex_initialized = true;

    if (ttak_cond_init(&worker->cond) != 0) {
        ttak_mutex_destroy(&worker->mutex);
        worker->mutex_initialized = false;
        return false;
    }
    worker->cond_initialized = true;

    if (pthread_create(&worker->thread, nullptr, host_eliza_worker_thread,
                       host) != 0) {
        ttak_cond_destroy(&worker->cond);
        worker->cond_initialized = false;
        ttak_mutex_destroy(&worker->mutex);
        worker->mutex_initialized = false;
        return false;
    }

    worker->thread_started = true;
    return true;
}

static void host_eliza_worker_shutdown(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    host_eliza_worker_state_t *worker = &host->eliza_worker;

    if (worker->mutex_initialized) {
        ttak_mutex_lock(&worker->mutex);
        atomic_store(&worker->stop, true);
        ttak_cond_broadcast(&worker->cond);
        ttak_mutex_unlock(&worker->mutex);
    } else {
        atomic_store(&worker->stop, true);
    }

    if (worker->thread_started) {
        pthread_join(worker->thread, nullptr);
        worker->thread_started = false;
    }

    if (worker->mutex_initialized) {
        ttak_mutex_destroy(&worker->mutex);
        worker->mutex_initialized = false;
    }

    if (worker->cond_initialized) {
        ttak_cond_destroy(&worker->cond);
        worker->cond_initialized = false;
    }

    host_eliza_intervene_task_t *task = worker->head;
    while (task != nullptr) {
        host_eliza_intervene_task_t *next = task->next;
        host_eliza_task_free(task);
        task = next;
    }

    worker->head = nullptr;
    worker->tail = nullptr;
    atomic_store(&worker->active, false);
    atomic_store(&worker->stop, false);
}

static __attribute__((unused)) bool
host_eliza_worker_enqueue(host_t *host, host_eliza_intervene_task_t *task)
{
    if (host == nullptr || task == nullptr) {
        return false;
    }

    host_eliza_worker_state_t *worker = &host->eliza_worker;
    if (!worker->mutex_initialized || !worker->cond_initialized ||
        !worker->thread_started) {
        return false;
    }

    task->next = nullptr;

    ttak_mutex_lock(&worker->mutex);
    if (atomic_load(&worker->stop)) {
        ttak_mutex_unlock(&worker->mutex);
        return false;
    }

    if (worker->tail == nullptr) {
        worker->head = task;
        worker->tail = task;
    } else {
        worker->tail->next = task;
        worker->tail = task;
    }

    ttak_cond_signal(&worker->cond);
    ttak_mutex_unlock(&worker->mutex);
    return true;
}

static void *host_eliza_worker_thread(void *arg)
{
    host_t *host = (host_t *)arg;
    if (host == nullptr) {
        return nullptr;
    }

    sshc_epoch_thread_enter();
    sshc_memory_context_t *memory_scope =
        sshc_memory_context_push(host->memory_context);

    host_eliza_worker_state_t *worker = &host->eliza_worker;
    atomic_store(&worker->active, true);

    while (true) {
        ttak_mutex_lock(&worker->mutex);
        while (!atomic_load(&worker->stop) && worker->head == nullptr) {
            ttak_cond_wait(&worker->cond, &worker->mutex);
        }

        if (worker->head == nullptr && atomic_load(&worker->stop)) {
            ttak_mutex_unlock(&worker->mutex);
            break;
        }

        host_eliza_intervene_task_t *task = worker->head;
        if (task != nullptr) {
            worker->head = task->next;
            if (worker->head == nullptr) {
                worker->tail = nullptr;
            }
        }
        ttak_mutex_unlock(&worker->mutex);

        if (task == nullptr) {
            continue;
        }

        const char *reason = (task->reason[0] != '\0') ? task->reason : nullptr;
        host_eliza_intervene_execute(task->ctx, reason, task->from_filter);
        host_eliza_task_free(task);
    }

    atomic_store(&worker->active, false);
    sshc_memory_context_pop(memory_scope);
    sshc_epoch_thread_exit();
    return nullptr;
}

static bool host_eliza_intervene(session_ctx_t *ctx, const char *content,
                                 const char *reason, bool from_filter)
{
    (void)ctx;
    (void)content;
    (void)reason;
    (void)from_filter;
    return false;
}

static void host_eliza_intervene_execute(session_ctx_t *ctx, const char *reason,
                                         bool from_filter)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    host_t *host = ctx->owner;
    if (!atomic_load(&host->eliza_enabled)) {
        return;
    }

    if (ctx->should_exit) {
        return;
    }

    if (!atomic_load(&host->eliza_announced)) {
        bool announce = false;
        ttak_mutex_lock(&host->lock);
        if (!atomic_load(&host->eliza_announced)) {
            atomic_store(&host->eliza_announced, true);
            announce = true;
        }
        ttak_mutex_unlock(&host->lock);
        if (announce) {
            host_eliza_announce_join(host);
        }
    }

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(message, sizeof(message),
             "%s, that crosses a legal line. You're out of here.",
             ctx->user.name);
    host_eliza_say(host, message);

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice),
             "* [eliza] removed [%s] for severe content.", ctx->user.name);
    host_history_record_system(host, notice, nullptr);

    clock_gettime(CLOCK_MONOTONIC, &host->eliza_last_action);
    if (from_filter && reason != nullptr && reason[0] != '\0') {
        printf("[eliza] removing %s (%s) after filter flag: %s\n",
               ctx->user.name, ctx->client_ip, reason);
    } else {
        printf("[eliza] removing %s (%s) after manual keyword flag\n",
               ctx->user.name, ctx->client_ip);
    }

    session_force_disconnect(
        ctx, "You have been removed by eliza for severe content.");
}

static host_security_scan_result_t
session_security_check_text(session_ctx_t *ctx, const char *category,
                            const char *content, size_t length, bool post_send)
{
    if (ctx == nullptr || ctx->owner == nullptr || content == nullptr ||
        length == 0U) {
        return HOST_SECURITY_SCAN_CLEAN;
    }

    char diagnostic[256];
    host_security_scan_result_t scan_result = host_security_scan_payload(
        ctx->owner, category, content, length, diagnostic, sizeof(diagnostic));

    if (scan_result == HOST_SECURITY_SCAN_CLEAN) {
        return HOST_SECURITY_SCAN_CLEAN;
    }

    if (scan_result == HOST_SECURITY_SCAN_BLOCKED) {
        host_security_process_blocked(ctx->owner, category, diagnostic,
                                      ctx->user.name, ctx->client_ip, ctx,
                                      post_send, content);
        return HOST_SECURITY_SCAN_BLOCKED;
    }

    const char *error = translator_last_error();
    if (diagnostic[0] == '\0' && error != nullptr && error[0] != '\0') {
        snprintf(diagnostic, sizeof(diagnostic), "%s", error);
    }

    host_security_process_error(ctx->owner, category, diagnostic,
                                ctx->user.name, ctx->client_ip, ctx, post_send);
    return scan_result;
}

static void host_state_resolve_path(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    const char *state_path = getenv("CHATTER_STATE_FILE");
    if (state_path == nullptr || state_path[0] == '\0') {
        state_path = "chatter_state.dat";
    }

    int written = snprintf(host->state_file_path, sizeof(host->state_file_path),
                           "%s", state_path);
    if (written < 0 || (size_t)written >= sizeof(host->state_file_path)) {
        humanized_log_error("host", "state file path is too long",
                            ENAMETOOLONG);
        host->state_file_path[0] = '\0';
    }
}

static void host_vote_resolve_path(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    const char *vote_path = getenv("CHATTER_VOTE_FILE");
    if (vote_path == nullptr || vote_path[0] == '\0') {
        vote_path = "vote_state.dat";
    }

    int written = snprintf(host->vote_state_file_path,
                           sizeof(host->vote_state_file_path), "%s", vote_path);
    if (written < 0 || (size_t)written >= sizeof(host->vote_state_file_path)) {
        humanized_log_error("host", "vote state file path is too long",
                            ENAMETOOLONG);
        host->vote_state_file_path[0] = '\0';
    }
}

static void host_ban_resolve_path(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    const char *ban_path = getenv("CHATTER_BAN_FILE");
    if (ban_path == nullptr || ban_path[0] == '\0') {
        ban_path = "ban_state.dat";
    }

    int written = snprintf(host->ban_state_file_path,
                           sizeof(host->ban_state_file_path), "%s", ban_path);
    if (written < 0 || (size_t)written >= sizeof(host->ban_state_file_path)) {
        humanized_log_error("host", "ban state file path is too long",
                            ENAMETOOLONG);
        host->ban_state_file_path[0] = '\0';
    }
}

static void host_reply_state_resolve_path(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    const char *reply_path = getenv("CHATTER_REPLY_FILE");
    if (reply_path == nullptr || reply_path[0] == '\0') {
        reply_path = "reply_state.dat";
    }

    int written =
        snprintf(host->reply_state_file_path,
                 sizeof(host->reply_state_file_path), "%s", reply_path);
    if (written < 0 || (size_t)written >= sizeof(host->reply_state_file_path)) {
        humanized_log_error("host", "reply state file path is too long",
                            ENAMETOOLONG);
        host->reply_state_file_path[0] = '\0';
    }
}

static void host_ui_language_state_resolve_path(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    const char *ui_lang_path = getenv("CHATTER_UI_LANG_FILE");
    if (ui_lang_path == nullptr || ui_lang_path[0] == '\0') {
        ui_lang_path = "ui_lang_state.dat";
    }

    int written =
        snprintf(host->ui_lang_state_file_path,
                 sizeof(host->ui_lang_state_file_path), "%s", ui_lang_path);
    if (written < 0 ||
        (size_t)written >= sizeof(host->ui_lang_state_file_path)) {
        humanized_log_error("host", "ui-lang state file path is too long",
                            ENAMETOOLONG);
        host->ui_lang_state_file_path[0] = '\0';
    }
}

static void host_pw_auth_resolve_path(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    const char *pw_path = getenv("CHATTER_PW_AUTH_FILE");
    if (pw_path == nullptr || pw_path[0] == '\0') {
        pw_path = "pw_auth.dat";
    }

    int written = snprintf(host->pw_auth_file_path,
                           sizeof(host->pw_auth_file_path), "%s", pw_path);
    if (written < 0 || (size_t)written >= sizeof(host->pw_auth_file_path)) {
        humanized_log_error("host", "pw auth file path is too long",
                            ENAMETOOLONG);
        host->pw_auth_file_path[0] = '\0';
    }
}

static void host_alpha_landers_resolve_path(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    const char *landers_path = getenv("CHATTER_ALPHA_LANDERS_FILE");
    if (landers_path == nullptr || landers_path[0] == '\0') {
        landers_path = "alpha_landers.dat";
    }

    int written =
        snprintf(host->alpha_landers_file_path,
                 sizeof(host->alpha_landers_file_path), "%s", landers_path);
    if (written < 0 ||
        (size_t)written >= sizeof(host->alpha_landers_file_path)) {
        humanized_log_error("host", "alpha landers file path is too long",
                            ENAMETOOLONG);
        host->alpha_landers_file_path[0] = '\0';
    }
}

static bool host_alpha_landers_load_locked(host_t *host,
                                           alpha_lander_entry_t *entries,
                                           size_t capacity, size_t *entry_count)
{
    if (entry_count != nullptr) {
        *entry_count = 0U;
    }
    if (host == nullptr || entries == nullptr || capacity == 0U ||
        entry_count == nullptr) {
        errno = EINVAL;
        return false;
    }

    if (host->alpha_landers_file_path[0] == '\0') {
        errno = ENOENT;
        return false;
    }

    memset(entries, 0, sizeof(entries[0]) * capacity);

    FILE *fp = fopen(host->alpha_landers_file_path, "rb");
    if (fp == nullptr) {
        if (errno == ENOENT) {
            return true;
        }
        return false;
    }

    alpha_landers_file_header_t header = {0};
    if (fread(&header, sizeof(header), 1U, fp) != 1U) {
        if (errno == 0) {
            errno = EIO;
        }
        fclose(fp);
        return false;
    }

    if (header.magic != ALPHA_LANDERS_STATE_MAGIC || header.version == 0U ||
        header.version > ALPHA_LANDERS_STATE_VERSION) {
        errno = EINVAL;
        fclose(fp);
        return false;
    }

    size_t total = header.entry_count;
    size_t stored = 0U;
    for (size_t idx = 0U; idx < total; ++idx) {
        alpha_landers_file_entry_t raw = {0};
        if (fread(&raw, sizeof(raw), 1U, fp) != 1U) {
            if (errno == 0) {
                errno = EIO;
            }
            fclose(fp);
            return false;
        }
        if (stored < capacity) {
            alpha_lander_entry_t *dest = &entries[stored++];
            memset(dest->username, 0, sizeof(dest->username));
            memcpy(dest->username, raw.username, sizeof(raw.username));
            dest->username[sizeof(dest->username) - 1U] = '\0';
            dest->flag_count = raw.flag_count;
            dest->last_flag_timestamp = raw.last_flag_timestamp;
        }
    }

    if (entry_count != nullptr) {
        *entry_count = stored;
    }

    fclose(fp);
    return true;
}

static bool host_alpha_landers_save_locked(host_t *host,
                                           const alpha_lander_entry_t *entries,
                                           size_t entry_count)
{
    if (host == nullptr || entries == nullptr) {
        errno = EINVAL;
        return false;
    }

    if (host->alpha_landers_file_path[0] == '\0') {
        errno = ENOENT;
        return false;
    }

    size_t count = entry_count;
    if (count > ALPHA_LANDERS_MAX_RECORDS) {
        count = ALPHA_LANDERS_MAX_RECORDS;
    }

    char temp_path[PATH_MAX];
    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp",
                           host->alpha_landers_file_path);
    if (written < 0 || (size_t)written >= sizeof(temp_path)) {
        humanized_log_error("alpha", "alpha landers file path is too long",
                            ENAMETOOLONG);
        return false;
    }

    FILE *fp = fopen(temp_path, "wb");
    if (fp == nullptr) {
        humanized_log_error("alpha", "failed to open alpha landers file",
                            errno != 0 ? errno : EIO);
        return false;
    }

    alpha_landers_file_header_t header = {0};
    header.magic = ALPHA_LANDERS_STATE_MAGIC;
    header.version = ALPHA_LANDERS_STATE_VERSION;
    header.entry_count = (uint32_t)count;

    bool success = fwrite(&header, sizeof(header), 1U, fp) == 1U;
    int write_error = 0;
    if (!success && errno != 0) {
        write_error = errno;
    }

    for (size_t idx = 0U; success && idx < count; ++idx) {
        alpha_landers_file_entry_t raw = {0};
        snprintf(raw.username, sizeof(raw.username), "%.*s",
                 SSH_CHATTER_USERNAME_LEN - 1, entries[idx].username);
        raw.flag_count = entries[idx].flag_count;
        raw.last_flag_timestamp = entries[idx].last_flag_timestamp;
        if (fwrite(&raw, sizeof(raw), 1U, fp) != 1U) {
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
        success = false;
        if (errno != 0) {
            write_error = errno;
        }
    }

    if (!success) {
        humanized_log_error("alpha", "failed to write alpha landers file",
                            write_error != 0 ? write_error : EIO);
        unlink(temp_path);
        return false;
    }

    if (rename(temp_path, host->alpha_landers_file_path) != 0) {
        humanized_log_error("alpha", "failed to update alpha landers file",
                            errno != 0 ? errno : EIO);
        unlink(temp_path);
        return false;
    }

    return true;
}

static bool host_alpha_landers_snapshot(host_t *host,
                                        alpha_lander_entry_t *entries,
                                        size_t capacity, size_t *entry_count)
{
    if (entry_count != nullptr) {
        *entry_count = 0U;
    }
    if (host == nullptr || entries == nullptr || capacity == 0U ||
        entry_count == nullptr) {
        errno = EINVAL;
        return false;
    }

    if (host->alpha_landers_lock_initialized) {
        ttak_mutex_lock(&host->alpha_landers_lock);
        bool success = host_alpha_landers_load_locked(host, entries, capacity,
                                                      entry_count);
        ttak_mutex_unlock(&host->alpha_landers_lock);
        return success;
    }

    return host_alpha_landers_load_locked(host, entries, capacity, entry_count);
}

static void host_alpha_landers_record(host_t *host, const char *username,
                                      uint32_t flag_count, uint64_t timestamp)
{
    if (host == nullptr || username == nullptr || username[0] == '\0' ||
        flag_count == 0U) {
        return;
    }

    if (host->alpha_landers_file_path[0] == '\0') {
        return;
    }

    alpha_lander_entry_t entries[ALPHA_LANDERS_MAX_RECORDS];
    size_t entry_count = 0U;

    bool locked = false;
    if (host->alpha_landers_lock_initialized) {
        ttak_mutex_lock(&host->alpha_landers_lock);
        locked = true;
    }

    bool loaded = host_alpha_landers_load_locked(
        host, entries, ALPHA_LANDERS_MAX_RECORDS, &entry_count);
    if (!loaded) {
        if (locked) {
            ttak_mutex_unlock(&host->alpha_landers_lock);
        }
        humanized_log_error("alpha", "failed to load alpha landers file",
                            errno != 0 ? errno : EIO);
        return;
    }

    bool found = false;
    for (size_t idx = 0U; idx < entry_count; ++idx) {
        alpha_lander_entry_t *entry = &entries[idx];
        if (strcasecmp(entry->username, username) == 0) {
            found = true;
            if (flag_count > entry->flag_count) {
                entry->flag_count = flag_count;
            }
            if (timestamp != 0U || entry->last_flag_timestamp == 0U) {
                entry->last_flag_timestamp = timestamp;
            }
            break;
        }
    }

    if (!found) {
        alpha_lander_entry_t candidate = {0};
        snprintf(candidate.username, sizeof(candidate.username), "%s",
                 username);
        candidate.flag_count = flag_count;
        candidate.last_flag_timestamp = timestamp;

        if (entry_count < ALPHA_LANDERS_MAX_RECORDS) {
            entries[entry_count++] = candidate;
        } else {
            size_t worst = 0U;
            for (size_t idx = 1U; idx < entry_count; ++idx) {
                if (alpha_lander_entry_compare(&entries[idx], &entries[worst]) >
                    0) {
                    worst = idx;
                }
            }
            if (alpha_lander_entry_compare(&candidate, &entries[worst]) < 0) {
                entries[worst] = candidate;
            }
        }
    }

    (void)host_alpha_landers_save_locked(host, entries, entry_count);

    if (locked) {
        ttak_mutex_unlock(&host->alpha_landers_lock);
    }
}

static bool host_user_data_bootstrap_username_is_valid(const char *username)
{
    if (username == nullptr) {
        return false;
    }

    const char *cursor = username;
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        ++cursor;
    }

    if (*cursor == '\0') {
        return false;
    }

    char sanitized[SSH_CHATTER_USERNAME_LEN * 2U];
    if (!user_data_sanitize_username(username, sanitized, sizeof(sanitized))) {
        return false;
    }

    return sanitized[0] != '\0';
}

static void host_user_data_bootstrap_visit(host_t *host, const char *username)
{
    if (host == nullptr) {
        return;
    }

    if (!host_user_data_bootstrap_username_is_valid(username)) {
        return;
    }

    (void)host_user_data_load_existing(host, username, nullptr, nullptr, true);
}

static void host_user_data_bootstrap(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (!host->user_data_ready) {
        if (user_data_ensure_root(host->user_data_root)) {
            host->user_data_ready = true;
        } else {
            humanized_log_error("mailbox",
                                "failed to prepare mailbox directory",
                                errno != 0 ? errno : EIO);
            return;
        }
    }

    if (!host->user_data_lock_initialized) {
        if (ttak_mutex_init(&host->user_data_lock) != 0) {
            humanized_log_error("mailbox", "failed to initialise mailbox lock",
                                errno != 0 ? errno : ENOMEM);
            host->user_data_lock_initialized = false;
            host->user_data_ready = false;
            return;
        }
        host->user_data_lock_initialized = true;
    }

    if (!host->user_data_ready) {
        return;
    }

    if (host->history != nullptr) {
        for (size_t idx = 0U; idx < host->history_count; ++idx) {
            const chat_history_entry_t *entry = &host->history[idx];
            if (!entry->is_user_message) {
                continue;
            }
            host_user_data_bootstrap_visit(host, entry->username);
        }
    }

    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_PREFERENCES; ++idx) {
        const user_preference_t *pref = &host->preferences[idx];
        if (!pref->in_use || pref->username[0] == '\0') {
            continue;
        }
        host_user_data_bootstrap_visit(host, pref->username);
    }

    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_REPLIES; ++idx) {
        const chat_reply_entry_t *reply = &host->replies[idx];
        if (!reply->in_use) {
            continue;
        }
        host_user_data_bootstrap_visit(host, reply->username);
    }

    for (size_t idx = 0U; idx < host->ban_count && idx < SSH_CHATTER_MAX_BANS;
         ++idx) {
        host_user_data_bootstrap_visit(host, host->bans[idx].username);
    }

    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_NAMED_POLLS; ++idx) {
        const named_poll_state_t *poll = &host->named_polls[idx];
        if (poll->label[0] == '\0') {
            continue;
        }
        host_user_data_bootstrap_visit(host, poll->owner);
        size_t voter_count = poll->voter_count;
        if (voter_count > SSH_CHATTER_MAX_NAMED_VOTERS) {
            voter_count = SSH_CHATTER_MAX_NAMED_VOTERS;
        }
        for (size_t voter = 0U; voter < voter_count; ++voter) {
            host_user_data_bootstrap_visit(host, poll->voters[voter].username);
        }
    }

    if (!host_bbs_storage_ready(host)) {
        return;
    }

    size_t capacity = host_bbs_loop_limit(host);
    for (size_t idx = 0U; idx < capacity; ++idx) {
        const bbs_post_t *post = &host->bbs_posts[idx];
        if (!post->in_use) {
            continue;
        }
        host_user_data_bootstrap_visit(host, post->author);
        size_t comment_count = post->comment_count;
        if (comment_count > SSH_CHATTER_BBS_MAX_COMMENTS) {
            comment_count = SSH_CHATTER_BBS_MAX_COMMENTS;
        }
        for (size_t comment = 0U; comment < comment_count; ++comment) {
            host_user_data_bootstrap_visit(host,
                                           post->comments[comment].author);
        }
    }
}

static void host_eliza_state_resolve_path(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    const char *state_path = getenv("CHATTER_ELIZA_STATE_FILE");
    char fallback_path[PATH_MAX];
    fallback_path[0] = '\0';
    if (state_path == nullptr || state_path[0] == '\0') {
        state_path = "eliza_state.dat";
        if (host->eliza_memory_file_path[0] != '\0') {
            char memory_parent_buffer[PATH_MAX];
            snprintf(memory_parent_buffer, sizeof(memory_parent_buffer), "%s",
                     host->eliza_memory_file_path);
            char *memory_parent = dirname(memory_parent_buffer);
            if (memory_parent != nullptr && memory_parent[0] != '\0' &&
                strcmp(memory_parent, ".") != 0) {
                int derived_written =
                    snprintf(fallback_path, sizeof(fallback_path), "%s/%s",
                             memory_parent, "eliza_state.dat");
                if (derived_written >= 0 &&
                    (size_t)derived_written < sizeof(fallback_path)) {
                    state_path = fallback_path;
                }
            }
        }
    }

    int written =
        snprintf(host->eliza_state_file_path,
                 sizeof(host->eliza_state_file_path), "%s", state_path);
    if (written < 0 || (size_t)written >= sizeof(host->eliza_state_file_path)) {
        humanized_log_error("host", "eliza state file path is too long",
                            ENAMETOOLONG);
        host->eliza_state_file_path[0] = '\0';
    }
}

static void host_state_save_locked(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (host->state_file_path[0] == '\0') {
        return;
    }

    if (!host_ensure_private_data_path(host, host->state_file_path, true)) {
        return;
    }

    char temp_path[PATH_MAX];
    int written =
        snprintf(temp_path, sizeof(temp_path), "%s.tmp", host->state_file_path);
    if (written < 0 || (size_t)written >= sizeof(temp_path)) {
        humanized_log_error("host", "state file path is too long",
                            ENAMETOOLONG);
        return;
    }

    FILE *fp = fopen(temp_path, "wb");
    if (fp == nullptr) {
        humanized_log_error("host", "failed to open state file", errno);
        return;
    }

    size_t preference_count = 0U;
    for (size_t idx = 0; idx < SSH_CHATTER_MAX_PREFERENCES; ++idx) {
        if (host->preferences[idx].in_use) {
            ++preference_count;
        }
    }

    const chat_history_entry_t *history_entries = host->history_override;
    size_t override_total = host->history_override_count;
    size_t history_entry_count = override_total;
    bool using_override =
        (history_entries != nullptr) || (host->history_override_count == 0U &&
                                         host->history_override != nullptr);
    if (!using_override) {
        history_entry_count = host->history_total;
    }

    time_t cutoff = 0;
    if (HOST_HISTORY_RETENTION_SECONDS > 0) {
        time_t now = time(nullptr);
        if (now != (time_t)-1 && now > HOST_HISTORY_RETENTION_SECONDS) {
            cutoff = now - HOST_HISTORY_RETENTION_SECONDS;
        }
    }

    size_t original_older_count = host->history_start_index;
    size_t persisted_older_count = original_older_count;
    bool prune_success = true;

    if (cutoff > 0) {
        if (using_override) {
            history_entry_count = 0U;
            if (history_entries != nullptr) {
                for (size_t idx = 0U; idx < override_total; ++idx) {
                    if (!chat_history_entry_is_expired(&history_entries[idx],
                                                       cutoff)) {
                        ++history_entry_count;
                    }
                }
            }
        } else {
            size_t removed_current =
                host_history_drop_expired_locked(host, cutoff);
            if (removed_current > 0U) {
                if (host->history_total >= removed_current) {
                    host->history_total -= removed_current;
                } else {
                    host->history_total = host->history_count;
                }
            }

            persisted_older_count = 0U;
            if (original_older_count > 0U) {
                FILE *input = nullptr;
                uint32_t version = 0U;
                uint32_t file_history_count = 0U;
                if (host_state_stream_open(host->state_file_path, &input,
                                           &version, &file_history_count)) {
                    size_t limit = original_older_count;
                    if (limit > (size_t)file_history_count) {
                        limit = (size_t)file_history_count;
                    }
                    for (size_t idx = 0U; idx < limit; ++idx) {
                        chat_history_entry_t entry_value = {0};
                        if (!host_state_read_history_entry(input, version,
                                                           &entry_value)) {
                            prune_success = false;
                            break;
                        }
                        if (!chat_history_entry_is_expired(&entry_value,
                                                           cutoff)) {
                            ++persisted_older_count;
                        }
                    }
                    fclose(input);
                } else {
                    prune_success = false;
                }
            }

            history_entry_count = persisted_older_count + host->history_count;
            host->history_start_index = persisted_older_count;
            host->history_total = history_entry_count;
        }
    } else if (!using_override) {
        history_entry_count = host->history_start_index + host->history_count;
        host->history_total = history_entry_count;
    }

    if (using_override) {
        host->history_total = history_entry_count;
        host->history_start_index = 0U;
    }

    host_state_header_t header = {0};
    header.base.magic = HOST_STATE_MAGIC;
    header.base.version = HOST_STATE_VERSION;
    header.base.history_count = (uint32_t)history_entry_count;
    header.base.preference_count = (uint32_t)preference_count;
    header.legacy_sound_count = 0U;
    header.grant_count = (uint32_t)host->operator_grant_count;
    header.next_message_id = host->next_message_id;
    header.captcha_enabled = atomic_load(&host->captcha_enabled) ? 1U : 0U;
    header.geo_language_enabled =
        atomic_load(&host->geo_language_enabled) ? 1U : 0U;
    memset(header.reserved, 0, sizeof(header.reserved));

    bool success = fwrite(&header, sizeof(header), 1U, fp) == 1U;

    if (!using_override && host->history_count > 0U &&
        host->history == nullptr) {
        success = false;
    }

    if (success) {
        if (using_override) {
            if (history_entries != nullptr) {
                for (size_t idx = 0U; success && idx < override_total; ++idx) {
                    if (cutoff > 0 && chat_history_entry_is_expired(
                                          &history_entries[idx], cutoff)) {
                        continue;
                    }
                    if (!host_state_write_history_entry(
                            fp, &history_entries[idx])) {
                        success = false;
                    }
                }
            }
        } else {
            if (original_older_count > 0U) {
                if (prune_success) {
                    FILE *input = nullptr;
                    uint32_t version = 0U;
                    uint32_t file_history_count = 0U;
                    if (host_state_stream_open(host->state_file_path, &input,
                                               &version, &file_history_count)) {
                        size_t limit = original_older_count;
                        if (limit > (size_t)file_history_count) {
                            limit = (size_t)file_history_count;
                        }
                        for (size_t idx = 0U; success && idx < limit; ++idx) {
                            chat_history_entry_t entry_value = {0};
                            if (!host_state_read_history_entry(input, version,
                                                               &entry_value)) {
                                success = false;
                                break;
                            }
                            if (cutoff > 0 && chat_history_entry_is_expired(
                                                  &entry_value, cutoff)) {
                                continue;
                            }
                            if (!host_state_write_history_entry(fp,
                                                                &entry_value)) {
                                success = false;
                                break;
                            }
                        }
                        fclose(input);
                    } else {
                        success = false;
                    }
                } else {
                    success = false;
                }
            }

            for (size_t idx = 0U; success && idx < host->history_count; ++idx) {
                const chat_history_entry_t *entry = &host->history[idx];
                if (cutoff > 0 &&
                    chat_history_entry_is_expired(entry, cutoff)) {
                    continue;
                }
                if (!host_state_write_history_entry(fp, entry)) {
                    success = false;
                }
            }
        }
    }

    if (!prune_success) {
        success = false;
    }

    for (size_t idx = 0; success && idx < SSH_CHATTER_MAX_PREFERENCES; ++idx) {
        const user_preference_t *pref = &host->preferences[idx];
        if (!pref->in_use) {
            continue;
        }

        host_state_preference_entry_t serialized = {0};
        serialized.has_user_theme = pref->has_user_theme ? 1U : 0U;
        serialized.has_system_theme = pref->has_system_theme ? 1U : 0U;
        serialized.user_is_bold = pref->user_is_bold ? 1U : 0U;
        serialized.system_is_bold = pref->system_is_bold ? 1U : 0U;
        snprintf(serialized.username, sizeof(serialized.username), "%s",
                 pref->username);
        snprintf(serialized.ip, sizeof(serialized.ip), "%s", pref->ip);
        snprintf(serialized.user_color_code, sizeof(serialized.user_color_code),
                 "%s", pref->user_color_code);
        snprintf(serialized.user_highlight_code,
                 sizeof(serialized.user_highlight_code), "%s",
                 pref->user_highlight_code);
        snprintf(serialized.user_color_name, sizeof(serialized.user_color_name),
                 "%s", pref->user_color_name);
        snprintf(serialized.user_highlight_name,
                 sizeof(serialized.user_highlight_name), "%s",
                 pref->user_highlight_name);
        snprintf(serialized.system_fg_name, sizeof(serialized.system_fg_name),
                 "%s", pref->system_fg_name);
        snprintf(serialized.system_bg_name, sizeof(serialized.system_bg_name),
                 "%s", pref->system_bg_name);
        snprintf(serialized.system_highlight_name,
                 sizeof(serialized.system_highlight_name), "%s",
                 pref->system_highlight_name);
        snprintf(serialized.os_name, sizeof(serialized.os_name), "%s",
                 pref->os_name);
        serialized.daily_year = pref->daily_year;
        serialized.daily_yday = pref->daily_yday;
        snprintf(serialized.daily_function, sizeof(serialized.daily_function),
                 "%s", pref->daily_function);
        serialized.last_poll_id = pref->last_poll_id;
        serialized.last_poll_choice = pref->last_poll_choice;
        serialized.has_birthday = pref->has_birthday ? 1U : 0U;
        serialized.translation_caption_spacing =
            pref->translation_caption_spacing;
        serialized.translation_enabled =
            pref->translation_master_enabled ? 1U : 0U;
        serialized.output_translation_enabled =
            pref->output_translation_enabled ? 1U : 0U;
        serialized.input_translation_enabled =
            pref->input_translation_enabled ? 1U : 0U;
        serialized.translation_master_explicit =
            pref->translation_master_explicit ? 1U : 0U;
        memset(serialized.reserved, 0, sizeof(serialized.reserved));
        serialized.breaking_alerts_enabled =
            pref->breaking_alerts_enabled ? 1U : 0U;
        memset(serialized.reserved2, 0, sizeof(serialized.reserved2));
        snprintf(serialized.birthday, sizeof(serialized.birthday), "%s",
                 pref->birthday);
        snprintf(serialized.output_translation_language,
                 sizeof(serialized.output_translation_language), "%s",
                 pref->output_translation_language);
        snprintf(serialized.input_translation_language,
                 sizeof(serialized.input_translation_language), "%s",
                 pref->input_translation_language);
        snprintf(serialized.ui_language, sizeof(serialized.ui_language), "%s",
                 pref->ui_language);
        snprintf(serialized.provider_label, sizeof(serialized.provider_label),
                 "%s", pref->provider_label);

        if (fwrite(&serialized, sizeof(serialized), 1U, fp) != 1U) {
            success = false;
            break;
        }
    }

    for (size_t idx = 0; success && idx < host->operator_grant_count; ++idx) {
        host_state_grant_entry_t grant = {0};
        snprintf(grant.ip, sizeof(grant.ip), "%s",
                 host->operator_grants[idx].ip);
        if (fwrite(&grant, sizeof(grant), 1U, fp) != 1U) {
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

    if (fclose(fp) != 0) {
        success = false;
    }

    if (!success) {
        humanized_log_error("host", "failed to write state file", errno);
        unlink(temp_path);
        return;
    }

    if (rename(temp_path, host->state_file_path) != 0) {
        humanized_log_error("host", "failed to update state file", errno);
        unlink(temp_path);
    }
}

static void host_eliza_state_save_locked(host_t *host)
{
    if (host == nullptr || host->eliza_state_file_path[0] == '\0') {
        return;
    }

    if (!host_ensure_private_data_path(host, host->eliza_state_file_path,
                                       true)) {
        return;
    }

    char temp_path[PATH_MAX];
    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp",
                           host->eliza_state_file_path);
    if (written < 0 || (size_t)written >= sizeof(temp_path)) {
        humanized_log_error("host", "eliza state path is too long",
                            ENAMETOOLONG);
        return;
    }

    FILE *fp = fopen(temp_path, "wb");
    if (fp == nullptr) {
        humanized_log_error("host", "failed to open eliza state file",
                            errno != 0 ? errno : EIO);
        return;
    }

    eliza_state_record_t record = {0};
    record.magic = ELIZA_STATE_MAGIC;
    record.version = ELIZA_STATE_VERSION;
    record.enabled = atomic_load(&host->eliza_enabled) ? 1U : 0U;

    bool success = fwrite(&record, sizeof(record), 1U, fp) == 1U;
    int write_error = 0;
    if (!success && errno != 0) {
        write_error = errno;
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
        humanized_log_error("host", "failed to write eliza state file",
                            write_error != 0 ? write_error : EIO);
        return;
    }

    if (rename(temp_path, host->eliza_state_file_path) != 0) {
        int rename_error = errno != 0 ? errno : EIO;
        unlink(temp_path);
        humanized_log_error("host", "failed to update eliza state file",
                            rename_error);
        return;
    }

    if (chmod(host->eliza_state_file_path, S_IRUSR | S_IWUSR) != 0) {
        humanized_log_error("host", "failed to set eliza state permissions",
                            errno != 0 ? errno : EACCES);
    }
}

static void host_eliza_state_load(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (host->eliza_state_file_path[0] == '\0') {
        return;
    }

    if (!host_ensure_private_data_path(host, host->eliza_state_file_path,
                                       false)) {
        return;
    }

    FILE *fp = fopen(host->eliza_state_file_path, "rb");
    if (fp == nullptr) {
        return;
    }

    eliza_state_record_t record = {0};
    if (fread(&record, sizeof(record), 1U, fp) != 1U) {
        fclose(fp);
        return;
    }

    fclose(fp);

    if (record.magic != ELIZA_STATE_MAGIC || record.version == 0U ||
        record.version > ELIZA_STATE_VERSION) {
        return;
    }

    if (record.enabled != 0U) {
        (void)host_eliza_enable(host);
    } else {
        ttak_mutex_lock(&host->lock);
        atomic_store(&host->eliza_enabled, false);
        atomic_store(&host->eliza_announced, false);
        ttak_mutex_unlock(&host->lock);
    }
}

static void host_ban_state_save_locked(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (host->ban_state_file_path[0] == '\0') {
        return;
    }

    char temp_path[PATH_MAX];
    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp",
                           host->ban_state_file_path);
    if (written < 0 || (size_t)written >= sizeof(temp_path)) {
        humanized_log_error("host", "ban state file path is too long",
                            ENAMETOOLONG);
        return;
    }

    FILE *fp = fopen(temp_path, "wb");
    if (fp == nullptr) {
        humanized_log_error("host", "failed to open ban state file", errno);
        return;
    }

    ban_state_header_t header = {0};
    header.magic = BAN_STATE_MAGIC;
    header.version = BAN_STATE_VERSION;
    header.entry_count = (uint32_t)host->ban_count;

    bool success = fwrite(&header, sizeof(header), 1U, fp) == 1U;
    int write_error = 0;
    if (!success && errno != 0) {
        write_error = errno;
    }

    for (size_t idx = 0U; success && idx < host->ban_count; ++idx) {
        ban_state_entry_t entry = {0};
        snprintf(entry.username, sizeof(entry.username), "%s",
                 host->bans[idx].username);
        snprintf(entry.ip, sizeof(entry.ip), "%s", host->bans[idx].ip);
        if (fwrite(&entry, sizeof(entry), 1U, fp) != 1U) {
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
        success = false;
        if (errno != 0) {
            write_error = errno;
        }
    }

    if (!success) {
        humanized_log_error("host", "failed to write ban state file",
                            write_error != 0 ? write_error : EIO);
        unlink(temp_path);
        return;
    }

    if (rename(temp_path, host->ban_state_file_path) != 0) {
        humanized_log_error("host", "failed to update ban state file", errno);
        unlink(temp_path);
    }
}

static void host_reply_state_save_locked(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (host->reply_state_file_path[0] == '\0') {
        return;
    }

    char temp_path[PATH_MAX];
    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp",
                           host->reply_state_file_path);
    if (written < 0 || (size_t)written >= sizeof(temp_path)) {
        humanized_log_error("host", "reply state file path is too long",
                            ENAMETOOLONG);
        return;
    }

    FILE *fp = fopen(temp_path, "wb");
    if (fp == nullptr) {
        humanized_log_error("host", "failed to open reply state file", errno);
        return;
    }

    size_t stored_count = 0U;
    for (size_t idx = 0U; idx < host->reply_count; ++idx) {
        if (host->replies[idx].in_use) {
            ++stored_count;
        }
    }

    reply_state_header_t header = {0};
    header.magic = REPLY_STATE_MAGIC;
    header.version = REPLY_STATE_VERSION;
    header.entry_count = (uint32_t)stored_count;
    header.next_reply_id = host->next_reply_id;

    bool success = fwrite(&header, sizeof(header), 1U, fp) == 1U;
    int write_error = 0;
    if (!success && errno != 0) {
        write_error = errno;
    }

    for (size_t idx = 0U; success && idx < host->reply_count; ++idx) {
        const chat_reply_entry_t *reply = &host->replies[idx];
        if (!reply->in_use) {
            continue;
        }

        reply_state_entry_t serialized = {0};
        serialized.reply_id = reply->reply_id;
        serialized.parent_message_id = reply->parent_message_id;
        serialized.parent_reply_id = reply->parent_reply_id;
        serialized.created_at = (int64_t)reply->created_at;
        snprintf(serialized.username, sizeof(serialized.username), "%s",
                 reply->username);
        snprintf(serialized.message, sizeof(serialized.message), "%s",
                 reply->message);

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
        success = false;
        if (errno != 0) {
            write_error = errno;
        }
    }

    if (!success) {
        humanized_log_error("host", "failed to write reply state file",
                            write_error != 0 ? write_error : EIO);
        unlink(temp_path);
        return;
    }

    if (rename(temp_path, host->reply_state_file_path) != 0) {
        humanized_log_error("host", "failed to update reply state file", errno);
        unlink(temp_path);
    }
}

static void vote_state_export_poll_entry(const poll_state_t *source,
                                         vote_state_poll_entry_t *dest)
{
    if (dest == nullptr) {
        return;
    }

    memset(dest, 0, sizeof(*dest));
    if (source == nullptr) {
        return;
    }

    dest->active = source->active ? 1U : 0U;
    dest->allow_multiple = source->allow_multiple ? 1U : 0U;
    dest->id = source->id;
    dest->option_count = (uint32_t)source->option_count;
    if (dest->option_count > 5U) {
        dest->option_count = 5U;
    }
    snprintf(dest->question, sizeof(dest->question), "%s", source->question);
    for (size_t idx = 0U; idx < 5U; ++idx) {
        snprintf(dest->options[idx].text, sizeof(dest->options[idx].text), "%s",
                 source->options[idx].text);
        dest->options[idx].votes = source->options[idx].votes;
    }
}

static void vote_state_import_poll_entry(const vote_state_poll_entry_t *source,
                                         poll_state_t *dest)
{
    if (dest == nullptr) {
        return;
    }

    poll_state_reset(dest);
    if (source == nullptr) {
        return;
    }

    dest->active = source->active != 0U;
    dest->allow_multiple = source->allow_multiple != 0U;
    dest->id = source->id;
    size_t option_count = source->option_count;
    if (option_count > 5U) {
        option_count = 5U;
    }
    dest->option_count = option_count;
    snprintf(dest->question, sizeof(dest->question), "%s", source->question);
    for (size_t idx = 0U; idx < option_count; ++idx) {
        snprintf(dest->options[idx].text, sizeof(dest->options[idx].text), "%s",
                 source->options[idx].text);
        dest->options[idx].votes = source->options[idx].votes;
    }
    for (size_t idx = option_count; idx < 5U; ++idx) {
        dest->options[idx].text[0] = '\0';
        dest->options[idx].votes = 0U;
    }
}

static __attribute__((unused)) void host_vote_state_save_locked(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (host->vote_state_file_path[0] == '\0') {
        return;
    }

    char temp_path[PATH_MAX];
    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp",
                           host->vote_state_file_path);
    if (written < 0 || (size_t)written >= sizeof(temp_path)) {
        humanized_log_error("host", "vote state file path is too long",
                            ENAMETOOLONG);
        return;
    }

    FILE *fp = fopen(temp_path, "wb");
    if (fp == nullptr) {
        humanized_log_error("host", "failed to open vote state file", errno);
        return;
    }

    uint32_t named_count = 0U;
    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_NAMED_POLLS; ++idx) {
        if (host->named_polls[idx].label[0] != '\0') {
            ++named_count;
        }
    }

    vote_state_header_t header = {0};
    header.magic = VOTE_STATE_MAGIC;
    header.version = VOTE_STATE_VERSION;
    header.named_count = named_count;

    bool success = fwrite(&header, sizeof(header), 1U, fp) == 1U;
    int write_error = 0;
    if (!success && errno != 0) {
        write_error = errno;
    }

    vote_state_poll_entry_t main_entry = {0};
    vote_state_export_poll_entry(&host->poll, &main_entry);
    if (success) {
        success = fwrite(&main_entry, sizeof(main_entry), 1U, fp) == 1U;
        if (!success && errno != 0) {
            write_error = errno;
        }
    }

    for (size_t idx = 0U; success && idx < SSH_CHATTER_MAX_NAMED_POLLS; ++idx) {
        const named_poll_state_t *poll = &host->named_polls[idx];
        if (poll->label[0] == '\0') {
            continue;
        }

        vote_state_named_entry_t entry = {0};
        vote_state_export_poll_entry(&poll->poll, &entry.poll);
        snprintf(entry.label, sizeof(entry.label), "%s", poll->label);
        snprintf(entry.owner, sizeof(entry.owner), "%s", poll->owner);
        entry.voter_count = (uint32_t)poll->voter_count;
        if (entry.voter_count > SSH_CHATTER_MAX_NAMED_VOTERS) {
            entry.voter_count = SSH_CHATTER_MAX_NAMED_VOTERS;
        }
        for (size_t voter = 0U; voter < SSH_CHATTER_MAX_NAMED_VOTERS; ++voter) {
            snprintf(entry.voters[voter].username,
                     sizeof(entry.voters[voter].username), "%s",
                     poll->voters[voter].username);
            entry.voters[voter].choice = poll->voters[voter].choice;
            entry.voters[voter].choices_mask = poll->voters[voter].choices_mask;
        }

        if (fwrite(&entry, sizeof(entry), 1U, fp) != 1U) {
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
        success = false;
        if (errno != 0) {
            write_error = errno;
        }
    }

    if (!success) {
        humanized_log_error("host", "failed to write vote state file",
                            write_error != 0 ? write_error : EIO);
        unlink(temp_path);
        return;
    }

    if (rename(temp_path, host->vote_state_file_path) != 0) {
        humanized_log_error("host", "failed to update vote state file", errno);
        unlink(temp_path);
    }
}

static bool host_state_read_base_header(FILE *fp,
                                        host_state_header_v1_t *base_header)
{
    if (fp == nullptr || base_header == nullptr) {
        return false;
    }

    if (fread(base_header, sizeof(*base_header), 1U, fp) != 1U) {
        return false;
    }

    if (base_header->magic != HOST_STATE_MAGIC) {
        return false;
    }

    if (base_header->version == 0U ||
        base_header->version > HOST_STATE_VERSION) {
        return false;
    }

    return true;
}

static bool host_state_read_metadata(FILE *fp, uint32_t version,
                                     uint64_t *next_message_id,
                                     uint32_t *grant_count,
                                     uint8_t *captcha_enabled_raw,
                                     uint8_t *geo_language_enabled_raw)
{
    if (fp == nullptr || next_message_id == nullptr || grant_count == nullptr ||
        captcha_enabled_raw == nullptr || geo_language_enabled_raw == nullptr) {
        return false;
    }

    *next_message_id = 1U;
    *grant_count = 0U;
    *captcha_enabled_raw = 0U;
    *geo_language_enabled_raw = 0U;

    if (version >= 2U) {
        uint32_t sound_count_raw = 0U;
        uint32_t grant_count_raw = 0U;
        uint64_t next_id_raw = 0U;
        if (fread(&sound_count_raw, sizeof(sound_count_raw), 1U, fp) != 1U ||
            fread(&grant_count_raw, sizeof(grant_count_raw), 1U, fp) != 1U ||
            fread(&next_id_raw, sizeof(next_id_raw), 1U, fp) != 1U) {
            return false;
        }
        *next_message_id = next_id_raw;
        if (version >= 5U) {
            *grant_count = grant_count_raw;
        }
    }

    if (version >= 8U) {
        uint8_t reserved_bytes[7] = {0};
        if (fread(captcha_enabled_raw, sizeof(*captcha_enabled_raw), 1U, fp) !=
                1U ||
            fread(reserved_bytes, sizeof(reserved_bytes), 1U, fp) != 1U) {
            return false;
        }

        if (version >= 13U) {
            *geo_language_enabled_raw = reserved_bytes[0];
        }
    }

    return true;
}

static bool
host_state_read_history_entry_from_stream(FILE *fp, uint32_t version,
                                          chat_history_entry_t *entry_value)
{
    if (fp == nullptr || entry_value == nullptr) {
        return false;
    }

    memset(entry_value, 0, sizeof(*entry_value));

    if (version != HOST_STATE_VERSION) {
        return false;
    }

    host_state_history_entry_t serialized = {0};
    if (fread(&serialized, sizeof(serialized), 1U, fp) != 1U) {
        return false;
    }

    entry_value->is_user_message = serialized.is_user_message != 0U;
    entry_value->user_is_bold = serialized.user_is_bold != 0U;
    snprintf(entry_value->username, sizeof(entry_value->username), "%s",
             serialized.username);
    snprintf(entry_value->raw_username, sizeof(entry_value->raw_username), "%s",
             serialized.raw_username);
    snprintf(entry_value->message, sizeof(entry_value->message), "%s",
             serialized.message);
    snprintf(entry_value->user_color_name, sizeof(entry_value->user_color_name),
             "%s", serialized.user_color_name);
    snprintf(entry_value->user_highlight_name,
             sizeof(entry_value->user_highlight_name), "%s",
             serialized.user_highlight_name);
    entry_value->message_id = serialized.message_id;
    if (serialized.attachment_type > CHAT_ATTACHMENT_FILE) {
        entry_value->attachment_type = CHAT_ATTACHMENT_NONE;
    } else {
        entry_value->attachment_type =
            (chat_attachment_type_t)serialized.attachment_type;
    }
    entry_value->created_at = (time_t)serialized.created_at;
    entry_value->preserve_whitespace = serialized.reserved[0] != 0U;
    snprintf(entry_value->attachment_target,
             sizeof(entry_value->attachment_target), "%s",
             serialized.attachment_target);
    snprintf(entry_value->attachment_caption,
             sizeof(entry_value->attachment_caption), "%s",
             serialized.attachment_caption);
    host_state_assign_color_codes(entry_value, serialized.user_color_code,
                                  serialized.user_highlight_code);
    memcpy(entry_value->reaction_counts, serialized.reaction_counts,
           sizeof(entry_value->reaction_counts));

static bool host_moderation_write_all(int fd, const void *buffer, size_t length)
{
    if (fd < 0 || buffer == nullptr) {
        return false;
    }

    const unsigned char *data = (const unsigned char *)buffer;
    size_t written = 0U;
    while (written < length) {
        ssize_t result = write(fd, data + written, length - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        written += (size_t)result;
    }

    return true;
}

static bool host_moderation_read_all(int fd, void *buffer, size_t length)
{
    if (fd < 0 || buffer == nullptr) {
        return false;
    }

    unsigned char *data = (unsigned char *)buffer;
    size_t read_total = 0U;
    while (read_total < length) {
        ssize_t result = read(fd, data + read_total, length - read_total);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        read_total += (size_t)result;
    }

    return true;
}

static void host_moderation_worker_loop(int request_fd, int response_fd)
{
    if (request_fd < 0 || response_fd < 0) {
        _exit(HOST_MODERATION_WORKER_EXIT_CODE);
    }

    translator_global_init();

    while (true) {
        host_moderation_ipc_request_t request;
        if (!host_moderation_read_all(request_fd, &request, sizeof(request))) {
            break;
        }

        if (request.category_length == 0U && request.content_length == 0U &&
            request.task_id == 0U) {
            break;
        }

        if (request.category_length >= HOST_MODERATION_CATEGORY_LEN) {
            request.category_length = HOST_MODERATION_CATEGORY_LEN - 1U;
        }
        if (request.content_length >= HOST_MODERATION_SNIPPET_LEN) {
            request.content_length = HOST_MODERATION_SNIPPET_LEN - 1U;
        }

        char category[HOST_MODERATION_CATEGORY_LEN];
        memset(category, 0, sizeof(category));
        if (!host_moderation_read_all(request_fd, category,
                                      request.category_length)) {
            break;
        }
        category[request.category_length] = '\0';

        char content[HOST_MODERATION_SNIPPET_LEN];
        memset(content, 0, sizeof(content));
        if (!host_moderation_read_all(request_fd, content,
                                      request.content_length)) {
            break;
        }
        content[request.content_length] = '\0';

        bool blocked = false;
        char reason[256];
        reason[0] = '\0';
        bool success = translator_moderate_text(category, content, &blocked,
                                                reason, sizeof(reason));

        host_moderation_ipc_response_t response;
        memset(&response, 0, sizeof(response));
        response.task_id = request.task_id;

        char message[256];
        message[0] = '\0';
        size_t message_length = 0U;

        if (!success) {
            response.result = HOST_SECURITY_SCAN_ERROR;
            response.disable_filter = 1U;
            const char *error = translator_last_error();
            if (error != nullptr && error[0] != '\0') {
                message_length = strnlen(error, sizeof(message) - 1U);
                memcpy(message, error, message_length);
            } else {
                const char *fallback = "moderation unavailable";
                message_length = strnlen(fallback, sizeof(message) - 1U);
                memcpy(message, fallback, message_length);
            }
            message[message_length] = '\0';
        } else if (blocked) {
            response.result = HOST_SECURITY_SCAN_BLOCKED;
            if (reason[0] != '\0') {
                message_length = strnlen(reason, sizeof(message) - 1U);
                memcpy(message, reason, message_length);
                message[message_length] = '\0';
            }
        } else {
            response.result = HOST_SECURITY_SCAN_CLEAN;
        }

        response.message_length = (uint32_t)message_length;

        if (!host_moderation_write_all(response_fd, &response,
                                       sizeof(response))) {
            break;
        }

        if (message_length > 0U) {
            if (!host_moderation_write_all(response_fd, message,
                                           message_length)) {
                break;
            }
        }
    }

    _exit(HOST_MODERATION_WORKER_EXIT_CODE);
}

static void host_moderation_backoff(unsigned int attempts)
{
    struct timespec delay = {
        .tv_sec = (attempts < 3U) ? 1L : ((attempts < 6U) ? 5L : 30L),
        .tv_nsec = 0L,
    };
    host_sleep_uninterruptible(&delay);
}

static void host_moderation_close_worker(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (host->moderation.request_fd >= 0) {
        close(host->moderation.request_fd);
        host->moderation.request_fd = -1;
    }
    if (host->moderation.response_fd >= 0) {
        close(host->moderation.response_fd);
        host->moderation.response_fd = -1;
    }

    if (host->moderation.worker_pid > 0) {
        int status = 0;
        pid_t result = waitpid(host->moderation.worker_pid, &status, WNOHANG);
        if (result == 0) {
            (void)kill(host->moderation.worker_pid, SIGTERM);
            (void)waitpid(host->moderation.worker_pid, &status, 0);
        }
        host->moderation.worker_pid = -1;
    }
}

static bool host_moderation_spawn_worker(host_t *host)
{
    if (host == nullptr) {
        return false;
    }

    int request_pipe[2] = {-1, -1};
    int response_pipe[2] = {-1, -1};

    if (pipe(request_pipe) != 0) {
        return false;
    }
    if (pipe(response_pipe) != 0) {
        close(request_pipe[0]);
        close(request_pipe[1]);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(request_pipe[0]);
        close(request_pipe[1]);
        close(response_pipe[0]);
        close(response_pipe[1]);
        return false;
    }

    if (pid == 0) {
        close(request_pipe[1]);
        close(response_pipe[0]);
        host_moderation_worker_loop(request_pipe[0], response_pipe[1]);
    }

    close(request_pipe[0]);
    close(response_pipe[1]);

    host->moderation.worker_pid = pid;
    host->moderation.request_fd = request_pipe[1];
    host->moderation.response_fd = response_pipe[0];

    if (clock_gettime(CLOCK_MONOTONIC, &host->moderation.worker_start_time) !=
        0) {
        host->moderation.worker_start_time.tv_sec = 0;
        host->moderation.worker_start_time.tv_nsec = 0;
    }

    return true;
}

static bool host_moderation_recover_worker(host_t *host, const char *diagnostic)
{
    if (host == nullptr) {
        return false;
    }

    const char *reason = (diagnostic != nullptr && diagnostic[0] != '\0')
                             ? diagnostic
                             : "moderation worker failure";

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
        double runtime =
            host_elapsed_seconds(&host->moderation.worker_start_time, &now);
        if (runtime >= HOST_MODERATION_WORKER_STABLE_SECONDS &&
            host->moderation.restart_attempts > 0U) {
            host->moderation.restart_attempts = 0U;
        }
    } else {
        host->moderation.restart_attempts = 0U;
    }

    unsigned int attempt = host->moderation.restart_attempts + 1U;

    char detail[256];
    snprintf(detail, sizeof(detail), "moderation worker panic (%s)", reason);
    humanized_log_error("moderation", detail, EIO);
    printf("[moderation] worker panic (%s); scheduling restart attempt %u\n",
           reason, attempt);

    host_moderation_close_worker(host);
    host_moderation_flush_pending(host, reason);

    if (attempt > HOST_MODERATION_MAX_RESTART_ATTEMPTS) {
        humanized_log_error(
            "moderation",
            "too many moderation worker panics; disabling moderation filter",
            EIO);
        ttak_mutex_lock(&host->moderation.mutex);
        host->moderation.active = false;
        host->moderation.stop = true;
        ttak_cond_broadcast(&host->moderation.cond);
        ttak_mutex_unlock(&host->moderation.mutex);
        atomic_store(&host->security_filter_enabled, false);
        return false;
    }

    host_moderation_backoff(attempt);

    if (!host_moderation_spawn_worker(host)) {
        humanized_log_error("moderation", "failed to restart moderation worker",
                            EIO);
        ttak_mutex_lock(&host->moderation.mutex);
        host->moderation.active = false;
        host->moderation.stop = true;
        ttak_cond_broadcast(&host->moderation.cond);
        ttak_mutex_unlock(&host->moderation.mutex);
        atomic_store(&host->security_filter_enabled, false);
        return false;
    }

    host->moderation.restart_attempts = attempt;

    ttak_mutex_lock(&host->moderation.mutex);
    host->moderation.active = true;
    host->moderation.stop = false;
    ttak_cond_broadcast(&host->moderation.cond);
    ttak_mutex_unlock(&host->moderation.mutex);

    printf("[moderation] worker recovered after panic (attempt %u)\n", attempt);
    return true;
}

static void
host_moderation_apply_result(host_t *host, host_moderation_task_t *task,
                             const host_moderation_ipc_response_t *response,
                             const char *message)
{
    if (host == nullptr || task == nullptr || response == nullptr) {
        return;
    }

    /*
     * Moderation tasks run asynchronously. Avoid keeping/using room member
     * pointers here because the target session may have disconnected before the
     * worker processes the task.
     */
    session_ctx_t *session = nullptr;

    if (response->disable_filter != 0U) {
        const char *reason = (message != nullptr && message[0] != '\0')
                                 ? message
                                 : "moderation pipeline unavailable";
        host_security_disable_filter(host, reason);
    }

    switch (response->result) {
    case HOST_SECURITY_SCAN_CLEAN:
        break;
    case HOST_SECURITY_SCAN_BLOCKED:
        host_security_process_blocked(host, task->category, message,
                                      task->username, task->client_ip, session,
                                      task->post_send, task->message);
        break;
    case HOST_SECURITY_SCAN_ERROR:
    default:
        host_security_process_error(host, task->category, message,
                                    task->username, task->client_ip, session,
                                    task->post_send);
        break;
    }
}

static void host_moderation_handle_failure(host_t *host,
                                           host_moderation_task_t *task,
                                           const char *diagnostic)
{
    if (host == nullptr || task == nullptr) {
        return;
    }

    const char *message = (diagnostic != nullptr && diagnostic[0] != '\0')
                              ? diagnostic
                              : "moderation pipeline unavailable";
    host_security_disable_filter(host, message);

    session_ctx_t *session = nullptr;
    host_security_process_error(host, task->category, message, task->username,
                                task->client_ip, session, task->post_send);
}

static void host_moderation_task_free(host_moderation_task_t *task)
{
    if (task == nullptr) {
        return;
    }
    sshc_gc_free(task);
}

static void host_moderation_flush_pending(host_t *host, const char *diagnostic)
{
    if (host == nullptr) {
        return;
    }

    host_moderation_task_t *task = nullptr;

    if (host->moderation.mutex_initialized) {
        ttak_mutex_lock(&host->moderation.mutex);
        task = host->moderation.head;
        host->moderation.head = nullptr;
        host->moderation.tail = nullptr;
        ttak_mutex_unlock(&host->moderation.mutex);
    }

    const char *message = (diagnostic != nullptr && diagnostic[0] != '\0')
                              ? diagnostic
                              : "moderation unavailable";

    while (task != nullptr) {
        host_moderation_task_t *next = task->next;
        session_ctx_t *session = nullptr;
        host_security_process_error(host, task->category, message,
                                    task->username, task->client_ip, session,
                                    task->post_send);
        host_moderation_task_free(task);
        task = next;
    }
}

static void *host_moderation_thread(void *arg)
{
    host_t *host = (host_t *)arg;
    if (host == nullptr) {
        return nullptr;
    }

    sshc_epoch_thread_enter();
    sshc_memory_context_t *memory_scope =
        sshc_memory_context_push(host->memory_context);

    const char *failure_reason = nullptr;

    while (true) {
        ttak_mutex_lock(&host->moderation.mutex);
        while (!host->moderation.stop && host->moderation.head == nullptr &&
               host->moderation.active) {
            ttak_cond_wait(&host->moderation.cond, &host->moderation.mutex);
        }

        if (!host->moderation.active ||
            (host->moderation.stop && host->moderation.head == nullptr)) {
            ttak_mutex_unlock(&host->moderation.mutex);
            break;
        }

        host_moderation_task_t *task = host->moderation.head;
        if (task != nullptr) {
            host->moderation.head = task->next;
            if (host->moderation.head == nullptr) {
                host->moderation.tail = nullptr;
            }
        }
        ttak_mutex_unlock(&host->moderation.mutex);

        if (task == nullptr) {
            continue;
        }

        host_moderation_ipc_request_t request;
        memset(&request, 0, sizeof(request));
        request.task_id = task->task_id;
        request.category_length = (uint32_t)strnlen(
            task->category, HOST_MODERATION_CATEGORY_LEN - 1U);
        request.content_length = (uint32_t)task->snippet_length;

        bool success = true;
        if (!host_moderation_write_all(host->moderation.request_fd, &request,
                                       sizeof(request)) ||
            (request.category_length > 0U &&
             !host_moderation_write_all(host->moderation.request_fd,
                                        task->category,
                                        request.category_length)) ||
            (request.content_length > 0U &&
             !host_moderation_write_all(host->moderation.request_fd,
                                        task->snippet,
                                        request.content_length))) {
            success = false;
        }

        if (!success) {
            failure_reason = "moderation worker unavailable";
            host_moderation_handle_failure(host, task, failure_reason);
            host_moderation_task_free(task);
            bool recovered =
                host_moderation_recover_worker(host, failure_reason);
            if (!recovered) {
                break;
            }
            failure_reason = nullptr;
            continue;
        }

        host_moderation_ipc_response_t response;
        if (!host_moderation_read_all(host->moderation.response_fd, &response,
                                      sizeof(response))) {
            failure_reason = "moderation worker unavailable";
            host_moderation_handle_failure(host, task, failure_reason);
            host_moderation_task_free(task);
            bool recovered =
                host_moderation_recover_worker(host, failure_reason);
            if (!recovered) {
                break;
            }
            failure_reason = nullptr;
            continue;
        }

        size_t message_length = response.message_length;
        char *message = nullptr;

        if (message_length > 0U) {
            message = (char *)sshc_gc_malloc(message_length + 1U);
            if (message == nullptr) {
                char *discard = (char *)sshc_gc_malloc(message_length);
                if (discard != nullptr) {
                    (void)host_moderation_read_all(host->moderation.response_fd,
                                                   discard, message_length);
                    sshc_gc_free(discard);
                }
                failure_reason = "moderation worker unavailable";
                host_moderation_handle_failure(host, task, failure_reason);
                host_moderation_task_free(task);
                bool recovered =
                    host_moderation_recover_worker(host, failure_reason);
                if (!recovered) {
                    break;
                }
                failure_reason = nullptr;
                continue;
            }

            if (!host_moderation_read_all(host->moderation.response_fd, message,
                                          message_length)) {
                failure_reason = "moderation worker unavailable";
                host_moderation_handle_failure(host, task, failure_reason);
                sshc_gc_free(message);
                host_moderation_task_free(task);
                bool recovered =
                    host_moderation_recover_worker(host, failure_reason);
                if (!recovered) {
                    break;
                }
                failure_reason = nullptr;
                continue;
            }
            message[message_length] = '\0';
        }

        const char *message_text = (message != nullptr) ? message : "";
        host_moderation_apply_result(host, task, &response, message_text);
        if (message != nullptr) {
            sshc_gc_free(message);
        }
        host_moderation_task_free(task);
        failure_reason = nullptr;
    }

    host_moderation_flush_pending(host, failure_reason);
    sshc_memory_context_pop(memory_scope);
    sshc_epoch_thread_exit();
    return nullptr;
}

static bool host_moderation_init(host_t *host)
{
    if (host == nullptr) {
        return false;
    }

    host->moderation.active = false;
    host->moderation.stop = false;
    host->moderation.head = nullptr;
    host->moderation.tail = nullptr;
    host->moderation.next_task_id = 1U;
    host->moderation.request_fd = -1;
    host->moderation.response_fd = -1;
    host->moderation.worker_pid = -1;
    host->moderation.thread_started = false;
    host->moderation.mutex_initialized = false;
    host->moderation.cond_initialized = false;

    if (ttak_mutex_init(&host->moderation.mutex) != 0) {
        return false;
    }
    host->moderation.mutex_initialized = true;

    if (ttak_cond_init(&host->moderation.cond) != 0) {
        ttak_mutex_destroy(&host->moderation.mutex);
        host->moderation.mutex_initialized = false;
        return false;
    }
    host->moderation.cond_initialized = true;

    host->moderation.restart_attempts = 0U;
    host->moderation.worker_start_time.tv_sec = 0;
    host->moderation.worker_start_time.tv_nsec = 0;

    if (!host_moderation_spawn_worker(host)) {
        host_moderation_shutdown(host);
        return false;
    }

    host->moderation.active = true;
    host->moderation.stop = false;

    if (pthread_create(&host->moderation.thread, nullptr,
                       host_moderation_thread, host) != 0) {
        host->moderation.active = false;
        host->moderation.stop = true;
        host_moderation_shutdown(host);
        return false;
    }

    host->moderation.thread_started = true;
    return true;
}

static void host_moderation_shutdown(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (!host->moderation.active && !host->moderation.thread_started) {
        if (host->moderation.mutex_initialized) {
            ttak_mutex_destroy(&host->moderation.mutex);
            host->moderation.mutex_initialized = false;
        }
        if (host->moderation.cond_initialized) {
            ttak_cond_destroy(&host->moderation.cond);
            host->moderation.cond_initialized = false;
        }
        return;
    }

    if (host->moderation.mutex_initialized) {
        ttak_mutex_lock(&host->moderation.mutex);
        host->moderation.stop = true;
        ttak_cond_broadcast(&host->moderation.cond);
        ttak_mutex_unlock(&host->moderation.mutex);
    }

    if (host->moderation.thread_started) {
        pthread_join(host->moderation.thread, nullptr);
        host->moderation.thread_started = false;
    }

    host_moderation_close_worker(host);
    host->moderation.restart_attempts = 0U;
    host->moderation.worker_start_time.tv_sec = 0;
    host->moderation.worker_start_time.tv_nsec = 0;

    host_moderation_flush_pending(host, nullptr);

    if (host->moderation.mutex_initialized) {
        ttak_mutex_destroy(&host->moderation.mutex);
        host->moderation.mutex_initialized = false;
    }
    if (host->moderation.cond_initialized) {
        ttak_cond_destroy(&host->moderation.cond);
        host->moderation.cond_initialized = false;
    }

    host->moderation.active = false;
}

static bool host_moderation_queue_chat(session_ctx_t *ctx, const char *message,
                                       size_t length)
{
    if (ctx == nullptr || ctx->owner == nullptr || message == nullptr ||
        length == 0U) {
        return false;
    }

    host_t *host = ctx->owner;
    if (!host->moderation.active || host->moderation.request_fd < 0 ||
        host->moderation.response_fd < 0) {
        return false;
    }

    if (!atomic_load(&host->security_filter_enabled)) {
        return false;
    }

    bool ai_active = atomic_load(&host->security_ai_enabled);
    if (!ai_active) {
        atomic_store(&host->security_filter_enabled, false);
        return false;
    }

    if (!atomic_load(&host->eliza_enabled)) {
        return false;
    }

    host_moderation_task_t *task =
        (host_moderation_task_t *)sshc_gc_malloc(sizeof(*task));
    if (task == nullptr) {
        return false;
    }

    memset(task, 0, sizeof(*task));
    snprintf(task->username, sizeof(task->username), "%s", ctx->user.name);
    snprintf(task->client_ip, sizeof(task->client_ip), "%s", ctx->client_ip);
    snprintf(task->category, sizeof(task->category), "%s", "chat message");

    size_t effective_length = strnlen(message, SSH_CHATTER_MESSAGE_LIMIT - 1U);
    if (effective_length > length) {
        effective_length = length;
    }

    task->snippet_length = effective_length;
    if (task->snippet_length >= HOST_MODERATION_SNIPPET_LEN) {
        task->snippet_length = HOST_MODERATION_SNIPPET_LEN - 1U;
    }
    memcpy(task->snippet, message, task->snippet_length);
    for (size_t idx = 0U; idx < task->snippet_length; ++idx) {
        unsigned char ch = (unsigned char)task->snippet[idx];
        if (ch == '\0') {
            task->snippet_length = idx;
            break;
        }
        if (ch < 0x20U && ch != '\n' && ch != '\r' && ch != '\t') {
            task->snippet[idx] = ' ';
        }
    }
    task->snippet[task->snippet_length] = '\0';

    size_t message_copy = effective_length;
    if (message_copy >= sizeof(task->message)) {
        message_copy = sizeof(task->message) - 1U;
    }
    memcpy(task->message, message, message_copy);
    task->message[message_copy] = '\0';
    task->post_send = true;

    ttak_mutex_lock(&host->moderation.mutex);
    if (!host->moderation.active || host->moderation.stop) {
        ttak_mutex_unlock(&host->moderation.mutex);
        host_moderation_task_free(task);
        return false;
    }

    task->task_id = host->moderation.next_task_id++;
    task->next = nullptr;
    if (host->moderation.tail == nullptr) {
        host->moderation.head = task;
        host->moderation.tail = task;
    } else {
        host->moderation.tail->next = task;
        host->moderation.tail = task;
    }
    ttak_cond_signal(&host->moderation.cond);
    ttak_mutex_unlock(&host->moderation.mutex);

    return true;
}


/**
 * @file host_security_and_moderation.c
 * @desc File-level documentation for host_security_and_moderation.c, describing its role
 *       in the SSH-Chatter server and providing a consistent header
 *       comment format across C sources.
 * @return None.
 */

// Host security pipeline, moderation workers, and persistence utilities.
#include "host_internal.h"

static const uint32_t UI_LANG_STATE_MAGIC = 0x55494c47U; /* 'UILG' */
static const uint32_t UI_LANG_STATE_VERSION = 1U;

typedef struct ui_lang_state_header {
    uint32_t magic;
    uint32_t version;
    uint32_t entry_count;
    uint32_t reserved;
} ui_lang_state_header_t;

typedef struct ui_lang_state_entry {
    char username[SSH_CHATTER_USERNAME_LEN];
    char ip[SSH_CHATTER_IP_LEN];
    char ui_language[SSH_CHATTER_LANG_NAME_LEN];
} ui_lang_state_entry_t;

static void host_security_reset_diagnostic(char *diagnostic,
                                           size_t diagnostic_length)
{
    if (diagnostic != nullptr && diagnostic_length > 0U) {
        diagnostic[0] = '\0';
    }
}

static bool host_security_scan_input_invalid(host_t *host, const char *payload,
                                             size_t length)
{
    return (host == nullptr || payload == nullptr || length == 0U);
}

static bool host_security_moderation_available(host_t *host)
{
    if (!atomic_load(&host->security_filter_enabled)) {
        return false;
    }

    bool ai_active = atomic_load(&host->security_ai_enabled);

    if (!ai_active) {
        atomic_store(&host->security_filter_enabled, false);
        return false;
    }

    return atomic_load(&host->eliza_enabled);
}

static const time_t HOST_HISTORY_RETENTION_SECONDS = 14 * 24 * 60 * 60;

static bool chat_history_entry_is_expired(const chat_history_entry_t *entry,
                                          time_t cutoff)
{
    if (entry == nullptr || cutoff <= 0) {
        return false;
    }

    if (entry->created_at == 0) {
        return false;
    }

    return entry->created_at < cutoff;
}

static size_t host_history_drop_expired_locked(host_t *host, time_t cutoff)
{
    if (host == nullptr || cutoff <= 0 || host->history == nullptr ||
        host->history_count == 0U) {
        return 0U;
    }

    size_t write_idx = 0U;
    size_t removed = 0U;
    for (size_t idx = 0U; idx < host->history_count; ++idx) {
        chat_history_entry_t *entry = &host->history[idx];
        if (chat_history_entry_is_expired(entry, cutoff)) {
            ++removed;
            continue;
        }
        if (write_idx != idx) {
            host->history[write_idx] = *entry;
        }
        ++write_idx;
    }

    if (removed > 0U) {
        for (size_t idx = write_idx; idx < host->history_count; ++idx) {
            memset(&host->history[idx], 0, sizeof(host->history[idx]));
        }
        host->history_count = write_idx;
    }

    return removed;
}

static size_t host_security_copy_sanitized_snippet(char *snippet,
                                                   size_t snippet_size,
                                                   const char *payload,
                                                   size_t length)
{
    size_t copy_length = length;
    if (copy_length >= snippet_size) {
        copy_length = snippet_size - 1U;
    }

    memcpy(snippet, payload, copy_length);
    for (size_t idx = 0U; idx < copy_length; ++idx) {
        unsigned char ch = (unsigned char)snippet[idx];
        if (ch == '\0') {
            copy_length = idx;
            break;
        }
        if (ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') {
            snippet[idx] = ' ';
        }
    }
    snippet[copy_length] = '\0';
    return copy_length;
}

static host_security_scan_result_t host_security_handle_moderation_failure(
    host_t *host, const char *error, char *diagnostic, size_t diagnostic_length)
{
    if (diagnostic != nullptr && diagnostic_length > 0U) {
        if (error != nullptr && error[0] != '\0') {
            snprintf(diagnostic, diagnostic_length, "%s", error);
        } else {
            snprintf(diagnostic, diagnostic_length, "%s",
                     "moderation unavailable");
        }
    }
    host_security_disable_filter(host, "moderation pipeline unavailable");
    return HOST_SECURITY_SCAN_ERROR;
}

static host_security_scan_result_t
host_security_finalize_scan(bool blocked, const char *reason, char *diagnostic,
                            size_t diagnostic_length)
{
    if (!blocked) {
        host_security_reset_diagnostic(diagnostic, diagnostic_length);
        return HOST_SECURITY_SCAN_CLEAN;
    }

    if (diagnostic != nullptr && diagnostic_length > 0U) {
        if (reason != nullptr && reason[0] != '\0') {
            snprintf(diagnostic, diagnostic_length, "%s", reason);
        } else {
            snprintf(diagnostic, diagnostic_length, "%s",
                     "potential intrusion attempt");
        }
    }

    return HOST_SECURITY_SCAN_BLOCKED;
}

static host_security_scan_result_t
host_security_scan_payload(host_t *host, const char *category,
                           const char *payload, size_t length, char *diagnostic,
                           size_t diagnostic_length)
{
    host_security_reset_diagnostic(diagnostic, diagnostic_length);

    if (host_security_scan_input_invalid(host, payload, length)) {
        return HOST_SECURITY_SCAN_CLEAN;
    }

    if (!host_security_moderation_available(host)) {
        return HOST_SECURITY_SCAN_CLEAN;
    }

    char snippet[1024];
    (void)host_security_copy_sanitized_snippet(snippet, sizeof(snippet),
                                               payload, length);

    bool blocked = false;
    char reason[256];
    reason[0] = '\0';

    bool success = translator_moderate_text(category, snippet, &blocked, reason,
                                            sizeof(reason));
    if (!success) {
        const char *error = translator_last_error();
        return host_security_handle_moderation_failure(host, error, diagnostic,
                                                       diagnostic_length);
    }

    return host_security_finalize_scan(blocked, reason, diagnostic,
                                       diagnostic_length);
}

typedef struct host_security_blocked_identity_s {
    const char *label;
    const char *name;
    char resolved_ip[SSH_CHATTER_IP_LEN];
    const char *address;
    const char *register_ip;
} host_security_blocked_identity_t;

static void host_security_blocked_identity_set_label_name(
    host_security_blocked_identity_t *identity, const char *category,
    const char *username)
{
    identity->label =
        (category != nullptr && category[0] != '\0') ? category : "submission";
    identity->name =
        (username != nullptr && username[0] != '\0') ? username : "unknown";
}

static void host_security_blocked_identity_resolve_ip(
    host_security_blocked_identity_t *identity, host_t *host,
    const char *username, const char *ip)
{
    identity->resolved_ip[0] = '\0';

    if (ip != nullptr && ip[0] != '\0' &&
        strncmp(ip, "unknown", SSH_CHATTER_IP_LEN) != 0) {
        snprintf(identity->resolved_ip, sizeof(identity->resolved_ip), "%s",
                 ip);
        return;
    }

    if (host != nullptr && username != nullptr && username[0] != '\0') {
        host_lookup_last_ip(host, username, identity->resolved_ip,
                            sizeof(identity->resolved_ip));
    }
}

static void host_security_blocked_identity_choose_addresses(
    host_security_blocked_identity_t *identity, const char *ip)
{
    if (identity->resolved_ip[0] != '\0') {
        identity->address = identity->resolved_ip;
        identity->register_ip = identity->resolved_ip;
        return;
    }

    if (ip != nullptr && ip[0] != '\0') {
        identity->address = ip;
        if (strncmp(ip, "unknown", SSH_CHATTER_IP_LEN) != 0) {
            identity->register_ip = ip;
            return;
        }
    }

    identity->address = "unknown";
    identity->register_ip = nullptr;
}

static void
host_security_blocked_identity_init(host_security_blocked_identity_t *identity,
                                    host_t *host, const char *category,
                                    const char *username, const char *ip)
{
    host_security_blocked_identity_set_label_name(identity, category, username);
    host_security_blocked_identity_resolve_ip(identity, host, username, ip);
    host_security_blocked_identity_choose_addresses(identity, ip);
}

static const char *host_security_select_diagnostic(const char *diagnostic,
                                                   char *buffer,
                                                   size_t buffer_length)
{
    if (diagnostic != nullptr && diagnostic[0] != '\0') {
        return diagnostic;
    }

    snprintf(buffer, buffer_length, "%s", "suspected intrusion content");
    return buffer;
}

static void
host_security_log_blocked(const host_security_blocked_identity_t *id,
                          const char *diagnostic)
{
    printf("[security] blocked %s from %s: %s\n", id->label, id->name,
           diagnostic);
}

static void host_security_notify_session_blocked(session_ctx_t *session,
                                                 const char *label,
                                                 const char *diagnostic,
                                                 bool post_send)
{
    if (session == nullptr) {
        return;
    }

    char message[512];
    if (post_send) {
        snprintf(message, sizeof(message),
                 "Security filter flagged your %s after delivery: %s", label,
                 diagnostic);
    } else {
        snprintf(message, sizeof(message),
                 "Security filter rejected your %s: %s", label, diagnostic);
    }
    session_send_system_line(session, message);
}

static void host_security_handle_suspicious_activity(
    host_t *host, const host_security_blocked_identity_t *identity,
    session_ctx_t *session)
{
    size_t attempts = 0U;

    if (host != nullptr) {
        const char *register_ip =
            (identity->register_ip != nullptr) ? identity->register_ip : "";
        host_register_suspicious_activity(host, identity->name, register_ip,
                                          &attempts);
    }

    if (attempts > 0U) {
        printf("[security] suspicious payload counter for %s (%s): %zu/%u\n",
               identity->name, identity->address, attempts,
               (unsigned int)SSH_CHATTER_SUSPICIOUS_EVENT_THRESHOLD);
    }

    if (attempts > 0U && session != nullptr) {
        char warning[256];
        snprintf(warning, sizeof(warning),
                 "Suspicious activity detected (%zu/%u).", attempts,
                 (unsigned int)SSH_CHATTER_SUSPICIOUS_EVENT_THRESHOLD);
        session_send_system_line(session, warning);
    }

    if (attempts >= SSH_CHATTER_SUSPICIOUS_EVENT_THRESHOLD) {
        printf("[security] suspicious payload threshold reached for %s (%s)\n",
               identity->name, identity->address);
    }
}

static void host_security_apply_eliza_intervention(session_ctx_t *session,
                                                   const char *content,
                                                   const char *diagnostic)
{
    if (session != nullptr) {
        (void)host_eliza_intervene(session, content, diagnostic, true);
    }
}

static void host_security_process_blocked(host_t *host, const char *category,
                                          const char *diagnostic,
                                          const char *username, const char *ip,
                                          session_ctx_t *session,
                                          bool post_send, const char *content)
{
    host_security_blocked_identity_t identity;
    host_security_blocked_identity_init(&identity, host, category, username,
                                        ip);

    char diagnostic_buffer[256];
    const char *use_diagnostic = host_security_select_diagnostic(
        diagnostic, diagnostic_buffer, sizeof(diagnostic_buffer));

    host_security_log_blocked(&identity, use_diagnostic);
    host_security_notify_session_blocked(session, identity.label,
                                         use_diagnostic, post_send);
    host_security_handle_suspicious_activity(host, &identity, session);
    host_security_apply_eliza_intervention(session, content, use_diagnostic);
}

static void host_security_process_error(host_t *host, const char *category,
                                        const char *diagnostic,
                                        const char *username, const char *ip,
                                        session_ctx_t *session, bool post_send)
{
    (void)host;
    (void)ip;

    const char *label =
        (category != nullptr && category[0] != '\0') ? category : "submission";
    const char *name =
        (username != nullptr && username[0] != '\0') ? username : "unknown";

    if (diagnostic != nullptr && diagnostic[0] != '\0') {
        printf("[security] unable to moderate %s from %s: %s\n", label, name,
               diagnostic);
    } else {
        printf("[security] unable to moderate %s from %s\n", label, name);
    }

    if (session == nullptr) {
        return;
    }

    char message[512];
    if (diagnostic != nullptr && diagnostic[0] != '\0') {
        if (post_send) {
            snprintf(message, sizeof(message),
                     "Security filter could not validate your %s after "
                     "delivery (%s).",
                     label, diagnostic);
        } else {
            snprintf(
                message, sizeof(message),
                "Security filter is unavailable (%s). Please try again later.",
                diagnostic);
        }
    } else {
        if (post_send) {
            snprintf(message, sizeof(message), "%s",
                     "Security filter could not validate your submission after "
                     "delivery. Please try again later.");
        } else {
            snprintf(
                message, sizeof(message), "%s",
                "Security filter could not validate your submission. Please "
                "try again later.");
        }
    }

    session_send_system_line(session, message);
}

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

    session_ctx_t *session = chat_room_find_user(&host->room, task->username);

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

    session_ctx_t *session = chat_room_find_user(&host->room, task->username);
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
        session_ctx_t *session =
            chat_room_find_user(&host->room, task->username);
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
        snprintf(raw.username, sizeof(raw.username), "%s",
                 entries[idx].username);
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

    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        return false;
    }

    host_rss_buffer_t buffer = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, SSH_CHATTER_RSS_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, host_rss_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

    bool success = false;
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
            for (size_t snapshot_index = 0U;
                 snapshot_index < snapshot_count &&
                 !atomic_load(&host->rss_thread_stop);
                 ++snapshot_index) {
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
                                          SSH_CHATTER_RSS_MAX_ITEMS,
                                          &item_count)) {
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

                ttak_mutex_lock(&host->lock);
                rss_feed_t *entry =
                    host_find_rss_feed_locked(host, feed_snapshot->tag);
                if (entry != nullptr && entry->in_use) {
                    feed_active = true;
                    entry->last_checked = now;

                    // Store new items in the sliding window
                    for (size_t idx = item_count; idx > 0U; --idx) {
                        if (host_rss_store_item_locked(entry,
                                                       &items[idx - 1U])) {
                            state_changed = true;
                        }
                    }

                    if (item_count > 0U) {
                        const rss_session_item_t *latest = &items[0U];
                        char new_key[SSH_CHATTER_RSS_ITEM_KEY_LEN];
                        new_key[0] = '\0';
                        if (latest->id[0] != '\0') {
                            snprintf(new_key, sizeof(new_key), "%s",
                                     latest->id);
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
                                     sizeof(entry->last_item_key), "%s",
                                     new_key);
                            state_changed = true;
                        }

                        if (latest->title[0] != '\0') {
                            snprintf(entry->last_title,
                                     sizeof(entry->last_title), "%s",
                                     latest->title);
                        } else {
                            entry->last_title[0] = '\0';
                        }

                        if (latest->link[0] != '\0') {
                            snprintf(entry->last_link, sizeof(entry->last_link),
                                     "%s", latest->link);
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
                    continue;
                }

                for (size_t idx = new_item_count;
                     idx > 0U && !atomic_load(&host->rss_thread_stop); --idx) {
                    const rss_session_item_t *item = &items[idx - 1U];
                    if (!host_rss_should_broadcast_breaking(item)) {
                        continue;
                    }

                    char headline[SSH_CHATTER_RSS_TITLE_LEN];
                    if (item->title[0] != '\0') {
                        snprintf(headline, sizeof(headline), "%s", item->title);
                    } else if (item->summary[0] != '\0') {
                        snprintf(headline, sizeof(headline), "%s",
                                 item->summary);
                    } else if (item->link[0] != '\0') {
                        snprintf(headline, sizeof(headline), "%s", item->link);
                    } else {
                        snprintf(headline, sizeof(headline), "%s",
                                 "New update");
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
                        snprintf(headline, sizeof(headline), "%s",
                                 "New update");
                    }

                    char notice[SSH_CHATTER_MESSAGE_LIMIT];
                    if (item->link[0] != '\0') {
                        char clean_link[SSH_CHATTER_RSS_LINK_LEN];
                        snprintf(clean_link, sizeof(clean_link), "%s",
                                 item->link);
                        rss_trim_whitespace(clean_link);
                        for (size_t pos = 0U; clean_link[pos] != '\0'; ++pos) {
                            if (clean_link[pos] == '\r' ||
                                clean_link[pos] == '\n' ||
                                clean_link[pos] == '\t') {
                                clean_link[pos] = ' ';
                            }
                        }
                        rss_trim_whitespace(clean_link);
                        snprintf(notice, sizeof(notice),
                                 "* %s [%s] %s - %s",
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
                        if (member != nullptr &&
                            member->breaking_alerts_enabled) {
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
    snprintf(clone_cmd, sizeof(clone_cmd),
             "GIT_TERMINAL_PROMPT=0 git clone --depth 1 %s %s", clone_target,
             repo_dir);

    bool success = host_archive_run_command(nullptr, clone_cmd);
    if (!success) {
        char cleanup_cmd[PATH_MAX * 2];
        snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf \"%s\"", base_dir);
        (void)host_archive_run_command(nullptr, cleanup_cmd);
        return false;
    }

    char destination[PATH_MAX];
    snprintf(destination, sizeof(destination), "%s/%s", repo_dir, timestamp);
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

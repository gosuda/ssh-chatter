// Connection setup, listener management, and transport helpers.
#include "host_internal.h"

static void session_format_telnet_identity(session_ctx_t *ctx,
                                           const char *primary_label)
{
    if (ctx == nullptr) {
        return;
    }

    ctx->telnet_identity[0] = '\0';
    if (ctx->transport_kind != SESSION_TRANSPORT_TELNET) {
        return;
    }

    const char *label = primary_label;
    char snippet[SSH_CHATTER_TERMINAL_TYPE_LEN];
    snippet[0] = '\0';

    if (label == nullptr || label[0] == '\0') {
        if (ctx->terminal_type[0] != '\0') {
            label = ctx->terminal_type;
        } else if (ctx->client_banner[0] != '\0') {
            session_extract_banner_token(ctx->client_banner, snippet,
                                         sizeof(snippet));
            if (snippet[0] != '\0') {
                label = snippet;
            }
        }
    }

    if (label == nullptr || label[0] == '\0') {
        label = "unknown";
    }

    snprintf(ctx->telnet_identity, sizeof(ctx->telnet_identity), "telnet/%s",
             label);
}

bool host_compact_id_encode(uint64_t id, char *buffer, size_t length)
{
    if (buffer == nullptr || length == 0U) {
        return false;
    }

    buffer[0] = '\0';
    if (id == 0U) {
        return false;
    }

    uint64_t base_value = ((id - 1U) % 9999U) + 1U;
    uint64_t suffix_index = (id - 1U) / 9999U;

    int written = snprintf(buffer, length, "%" PRIu64, base_value);
    if (written < 0 || (size_t)written >= length) {
        buffer[0] = '\0';
        return false;
    }

    size_t offset = (size_t)written;
    if (suffix_index == 0U) {
        return true;
    }

    char suffix[32];
    size_t suffix_len = 0U;
    while (suffix_index > 0U) {
        suffix_index -= 1U;
        if (suffix_len + 1U >= sizeof(suffix)) {
            buffer[0] = '\0';
            return false;
        }
        suffix[suffix_len++] = (char)('a' + (suffix_index % 26U));
        suffix_index /= 26U;
    }

    if (offset + suffix_len >= length) {
        buffer[0] = '\0';
        return false;
    }

    for (size_t idx = 0U; idx < suffix_len; ++idx) {
        buffer[offset + idx] = suffix[suffix_len - idx - 1U];
    }
    buffer[offset + suffix_len] = '\0';
    return true;
}

bool host_compact_id_decode(const char *text, uint64_t *id_out)
{
    if (text == nullptr || id_out == nullptr) {
        return false;
    }

    size_t position = 0U;
    while (text[position] != '\0' &&
           isspace((unsigned char)text[position]) != 0) {
        ++position;
    }

    uint64_t base_value = 0U;
    bool saw_digit = false;
    while (text[position] != '\0') {
        unsigned char ch = (unsigned char)text[position];
        if (!isdigit(ch)) {
            break;
        }
        saw_digit = true;
        base_value = base_value * 10U + (uint64_t)(ch - '0');
        if (base_value > 9999U) {
            return false;
        }
        ++position;
    }

    if (!saw_digit || base_value == 0U) {
        return false;
    }

    uint64_t suffix_value = 0U;
    while (text[position] != '\0') {
        unsigned char ch = (unsigned char)text[position];
        if (isspace(ch) != 0) {
            break;
        }
        if (!isalpha(ch)) {
            return false;
        }
        unsigned int alpha_index = (unsigned int)(tolower((int)ch) - 'a');
        if (alpha_index >= 26U) {
            return false;
        }
        uint64_t addend = (uint64_t)(alpha_index + 1U);
        if (suffix_value > (UINT64_MAX - addend) / 26U) {
            return false;
        }
        suffix_value = suffix_value * 26U + addend;
        ++position;
    }

    while (text[position] != '\0') {
        if (isspace((unsigned char)text[position]) == 0) {
            return false;
        }
        ++position;
    }

    if (suffix_value > 0U) {
        uint64_t product = suffix_value * 9999U;
        if (product > UINT64_MAX - base_value) {
            return false;
        }
        *id_out = product + base_value;
    } else {
        *id_out = base_value;
    }

    return true;
}

static bool host_is_leap_year(int year)
{
    if (year <= 0) {
        return false;
    }

    if ((year % 4) != 0) {
        return false;
    }
    if ((year % 100) != 0) {
        return true;
    }
    return (year % 400) == 0;
}

static struct timespec timespec_diff(const struct timespec *end,
                                     const struct timespec *start)
{
    struct timespec result = {0, 0};
    if (end == nullptr || start == nullptr) {
        return result;
    }

    time_t sec = end->tv_sec - start->tv_sec;
    long nsec = end->tv_nsec - start->tv_nsec;
    if (nsec < 0) {
        --sec;
        nsec += 1000000000L;
    }
    if (sec < 0) {
        sec = 0;
        nsec = 0;
    }
    result.tv_sec = sec;
    result.tv_nsec = nsec;
    return result;
}

static long long timespec_to_ns(const struct timespec *value)
{
    if (value == nullptr) {
        return 0LL;
    }

    return (long long)value->tv_sec * 1000000000LL + (long long)value->tv_nsec;
}

static bool host_listener_attempt_recover(host_t *host, ssh_bind bind_handle,
                                          const char *address,
                                          const char *bind_port)
{
    if (host == nullptr || bind_handle == nullptr) {
        return false;
    }

    printf(
        "[listener] attempting in-place recovery on %s:%s after socket error\n",
        address, bind_port);
    ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_BINDADDR, address);
    ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_BINDPORT_STR, bind_port);
    if (ssh_bind_listen(bind_handle) == SSH_OK) {
        host->listener.inplace_recoveries += 1U;
        printf("[listener] listener recovered without restart (total in-place "
               "recoveries: %u)\n",
               host->listener.inplace_recoveries);
        return true;
    }

    const char *error_message = ssh_get_error(bind_handle);
    if (error_message == nullptr || error_message[0] == '\0') {
        error_message = "unknown error";
    }
    printf("[listener] in-place recovery failed: %s\n", error_message);
    return false;
}

static bool host_join_key_path(const char *directory, const char *filename,
                               char *buffer, size_t buffer_len)
{
    if (directory == nullptr || filename == nullptr || buffer == nullptr ||
        buffer_len == 0U) {
        return false;
    }

    const size_t dir_len = strlen(directory);
    const bool needs_separator = dir_len > 0U && directory[dir_len - 1U] != '/';
    const int written = snprintf(buffer, buffer_len, "%s%s%s", directory,
                                 needs_separator ? "/" : "", filename);
    if (written < 0 || (size_t)written >= buffer_len) {
        return false;
    }

    return true;
}

static bool host_bind_algorithm_is_rsa(const char *algorithm)
{
    return algorithm != nullptr && strcmp(algorithm, "ssh-rsa") == 0;
}

static void host_bind_append_single_algorithm(char *buffer, size_t buffer_len,
                                              size_t *current_len,
                                              const char *algorithm)
{
    if (buffer == nullptr || current_len == nullptr || algorithm == nullptr ||
        algorithm[0] == '\0' || buffer_len == 0U) {
        return;
    }

    const size_t usable_length = buffer_len - 1U;
    if (*current_len > usable_length) {
        *current_len = usable_length;
        buffer[usable_length] = '\0';
        return;
    }

    if (*current_len > 0U) {
        if (*current_len >= usable_length) {
            buffer[usable_length] = '\0';
            return;
        }
        buffer[*current_len] = ',';
        ++(*current_len);
    }

    size_t remaining = usable_length - *current_len;
    if (remaining == 0U) {
        buffer[*current_len] = '\0';
        return;
    }

    size_t algorithm_length = strlen(algorithm);
    if (algorithm_length > remaining) {
        algorithm_length = remaining;
    }

    memcpy(buffer + *current_len, algorithm, algorithm_length);
    *current_len += algorithm_length;
    buffer[*current_len] = '\0';
}

static void host_bind_append_algorithm(char *buffer, size_t buffer_len,
                                       size_t *current_len,
                                       const char *algorithm)
{
    if (buffer == nullptr || current_len == nullptr || algorithm == nullptr ||
        algorithm[0] == '\0' || buffer_len == 0U) {
        return;
    }

    if (host_bind_algorithm_is_rsa(algorithm)) {
        host_bind_append_single_algorithm(buffer, buffer_len, current_len,
                                          "rsa-sha2-512");
        host_bind_append_single_algorithm(buffer, buffer_len, current_len,
                                          "rsa-sha2-256");
    }

    host_bind_append_single_algorithm(buffer, buffer_len, current_len,
                                      algorithm);
}

static bool host_bind_import_key(ssh_bind bind_handle, const char *algorithm,
                                 const char *key_path, ssh_key *retained_key)
{
    if (bind_handle == nullptr || algorithm == nullptr || key_path == nullptr) {
        return false;
    }

    ssh_key imported_key = nullptr;
    if (ssh_pki_import_privkey_file(key_path, nullptr, nullptr, nullptr,
                                    &imported_key) != SSH_OK ||
        imported_key == nullptr) {
        char message[256];
        snprintf(message, sizeof(message), "failed to import %s host key",
                 algorithm);
        humanized_log_error("host", message, errno != 0 ? errno : EIO);
        if (imported_key != nullptr) {
            ssh_key_free(imported_key);
        }
        return false;
    }

    errno = 0;
    const int import_result = ssh_bind_options_set(
        bind_handle, SSH_BIND_OPTIONS_IMPORT_KEY, imported_key);
    if (import_result != SSH_OK) {
        const char *error_message = ssh_get_error(bind_handle);
        char message[256];
        snprintf(message, sizeof(message), "failed to register %s host key",
                 algorithm);
        humanized_log_error("host",
                            error_message != nullptr ? error_message : message,
                            errno != 0 ? errno : EIO);
        ssh_key_free(imported_key);
        return false;
    }

    if (retained_key != nullptr) {
        *retained_key = imported_key;
    }

    return true;
}

static bool host_bind_load_key(ssh_bind bind_handle,
                               const host_key_definition_t *definition,
                               const char *key_path, ssh_key *retained_key)
{
    if (bind_handle == nullptr || definition == nullptr ||
        key_path == nullptr) {
        return false;
    }

    bool require_import = definition->requires_import;
    if (!require_import) {
        errno = 0;
        const int set_result =
            ssh_bind_options_set(bind_handle, definition->option, key_path);
        if (set_result == SSH_OK) {
            return true;
        }

        const char *error_message = ssh_get_error(bind_handle);
        const bool unsupported_option =
            (error_message != nullptr &&
             strstr(error_message, "Unknown ssh option") != nullptr) ||
            errno == ENOTSUP;
        if (!unsupported_option) {
            char message[256];
            snprintf(message, sizeof(message), "failed to load %s host key",
                     definition->algorithm);
            humanized_log_error(
                "host", error_message != nullptr ? error_message : message,
                errno != 0 ? errno : EIO);
            return false;
        }
        require_import = true;
    }

    if (require_import && !definition->requires_import) {
        printf(
            "[listener] importing %s host key due to limited libssh support\n",
            definition->algorithm);
    }

    return host_bind_import_key(bind_handle, definition->algorithm, key_path,
                                retained_key);
}

static struct timespec timespec_add_ns(const struct timespec *start,
                                       long long nanoseconds)
{
    struct timespec result = {0, 0};
    if (start != nullptr) {
        result = *start;
    }

    if (nanoseconds < 0) {
        return result;
    }

    result.tv_sec += (time_t)(nanoseconds / 1000000000LL);
    result.tv_nsec += (long)(nanoseconds % 1000000000LL);
    if (result.tv_nsec >= 1000000000L) {
        result.tv_sec += result.tv_nsec / 1000000000L;
        result.tv_nsec %= 1000000000L;
    }
    return result;
}

static struct timespec timespec_add_ms(const struct timespec *start,
                                       long milliseconds)
{
    struct timespec result = {0, 0};
    if (start != nullptr) {
        result = *start;
    }

    long seconds = milliseconds / 1000L;
    long remaining_ms = milliseconds % 1000L;
    result.tv_sec += seconds;
    result.tv_nsec += remaining_ms * 1000000L;
    if (result.tv_nsec >= 1000000000L) {
        result.tv_sec += result.tv_nsec / 1000000000L;
        result.tv_nsec %= 1000000000L;
    }
    return result;
}

static int timespec_compare(const struct timespec *lhs,
                            const struct timespec *rhs)
{
    if (lhs == nullptr || rhs == nullptr) {
        return 0;
    }
    if (lhs->tv_sec < rhs->tv_sec) {
        return -1;
    }
    if (lhs->tv_sec > rhs->tv_sec) {
        return 1;
    }
    if (lhs->tv_nsec < rhs->tv_nsec) {
        return -1;
    }
    if (lhs->tv_nsec > rhs->tv_nsec) {
        return 1;
    }
    return 0;
}

typedef struct connection_guard_result {
    bool blocked;
    bool escalate_ban;
    struct timespec blocked_until;
    size_t attempt_count;
    unsigned int block_count;
} connection_guard_result_t;

static void host_connection_guard_prune_locked(host_t *host,
                                               const struct timespec *now)
{
    if (host == nullptr || now == nullptr ||
        host->connection_guard_count == 0U) {
        return;
    }

    size_t write_idx = 0U;
    const size_t original_count = host->connection_guard_count;
    for (size_t idx = 0U; idx < original_count; ++idx) {
        connection_guard_entry_t *entry = &host->connection_guard[idx];
        if (entry->ip[0] == '\0') {
            continue;
        }

        if (entry->last_seen.tv_sec != 0 || entry->last_seen.tv_nsec != 0) {
            struct timespec age = timespec_diff(now, &entry->last_seen);
            long long age_ns = timespec_to_ns(&age);
            if (age_ns > SSH_CHATTER_CONNECTION_GUARD_RETENTION_NS) {
                continue;
            }
        }

        if (write_idx != idx) {
            host->connection_guard[write_idx] = *entry;
        }
        ++write_idx;
    }

    if (write_idx < original_count) {
        size_t cleared = original_count - write_idx;
        memset(&host->connection_guard[write_idx], 0,
               cleared * sizeof(host->connection_guard[write_idx]));
    }
    host->connection_guard_count = write_idx;
}

static connection_guard_entry_t *
host_find_connection_guard_locked(host_t *host, const char *ip)
{
    if (host == nullptr || ip == nullptr) {
        return nullptr;
    }

    for (size_t idx = 0U; idx < host->connection_guard_count; ++idx) {
        connection_guard_entry_t *entry = &host->connection_guard[idx];
        if (strncmp(entry->ip, ip, SSH_CHATTER_IP_LEN) == 0) {
            return entry;
        }
    }

    return nullptr;
}

static connection_guard_entry_t *
host_ensure_connection_guard_locked(host_t *host, const char *ip)
{
    if (host == nullptr || ip == nullptr || ip[0] == '\0') {
        return nullptr;
    }

    connection_guard_entry_t *entry =
        host_find_connection_guard_locked(host, ip);
    if (entry != nullptr) {
        return entry;
    }

    if (host->connection_guard_count >= host->connection_guard_capacity) {
        size_t new_capacity = host->connection_guard_capacity > 0U
                                  ? host->connection_guard_capacity * 2U
                                  : 16U;
        connection_guard_entry_t *resized =
            GC_REALLOC(host->connection_guard,
                       new_capacity * sizeof(connection_guard_entry_t));
        if (resized == nullptr) {
            return nullptr;
        }
        host->connection_guard = resized;
        memset(&host->connection_guard[host->connection_guard_capacity], 0,
               (new_capacity - host->connection_guard_capacity) *
                   sizeof(connection_guard_entry_t));
        host->connection_guard_capacity = new_capacity;
    }

    entry = &host->connection_guard[host->connection_guard_count++];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->ip, sizeof(entry->ip), "%s", ip);
    return entry;
}

static connection_guard_result_t host_connection_guard_register(host_t *host,
                                                                const char *ip)
{
    connection_guard_result_t result = {0};
    if (host == nullptr || ip == nullptr || ip[0] == '\0') {
        return result;
    }

    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);

    pthread_mutex_lock(&host->lock);
    host_connection_guard_prune_locked(host, &now);
    connection_guard_entry_t *entry =
        host_ensure_connection_guard_locked(host, ip);
    if (entry == nullptr) {
        pthread_mutex_unlock(&host->lock);
        return result;
    }

    entry->last_seen = now;

    if (entry->blocked_until.tv_sec != 0 || entry->blocked_until.tv_nsec != 0) {
        if (timespec_compare(&entry->blocked_until, &now) > 0) {
            result.blocked = true;
            result.blocked_until = entry->blocked_until;
            result.block_count = entry->block_count;
            result.attempt_count = entry->attempts;
            pthread_mutex_unlock(&host->lock);
            return result;
        }
        entry->blocked_until.tv_sec = 0;
        entry->blocked_until.tv_nsec = 0L;
    }

    if (entry->window_start.tv_sec == 0 && entry->window_start.tv_nsec == 0) {
        entry->window_start = now;
        entry->attempts = 0U;
    } else {
        struct timespec diff = timespec_diff(&now, &entry->window_start);
        long long window_ns = timespec_to_ns(&diff);
        if (window_ns > SSH_CHATTER_CONNECTION_GUARD_WINDOW_NS) {
            entry->window_start = now;
            entry->attempts = 0U;
            if (window_ns > SSH_CHATTER_CONNECTION_GUARD_RETENTION_NS) {
                entry->block_count = 0U;
            }
        }
    }

    if (entry->attempts < SIZE_MAX) {
        entry->attempts += 1U;
    }
    result.attempt_count = entry->attempts;
    result.block_count = entry->block_count;

    if (entry->attempts >= SSH_CHATTER_CONNECTION_GUARD_THRESHOLD) {
        size_t attempt_snapshot = entry->attempts;
        if (entry->block_count < UINT_MAX) {
            entry->block_count += 1U;
        }
        long long penalty_ns =
            SSH_CHATTER_CONNECTION_GUARD_BLOCK_BASE_NS +
            (long long)(entry->block_count > 0U ? entry->block_count - 1U
                                                : 0U) *
                SSH_CHATTER_CONNECTION_GUARD_BLOCK_STEP_NS;
        if (penalty_ns > SSH_CHATTER_CONNECTION_GUARD_BLOCK_MAX_NS) {
            penalty_ns = SSH_CHATTER_CONNECTION_GUARD_BLOCK_MAX_NS;
        }
        entry->blocked_until = timespec_add_ns(&now, penalty_ns);
        entry->window_start = now;
        entry->attempts = 0U;

        result.blocked = true;
        result.blocked_until = entry->blocked_until;
        result.block_count = entry->block_count;
        result.attempt_count = attempt_snapshot;
        if (entry->block_count >= SSH_CHATTER_CONNECTION_GUARD_BAN_THRESHOLD) {
            result.escalate_ban = true;
        }
    }

    pthread_mutex_unlock(&host->lock);
    return result;
}

static void host_error_guard_register_success(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    host->health_guard.consecutive_errors = 0U;
    host->health_guard.last_error_time.tv_sec = 0;
    host->health_guard.last_error_time.tv_nsec = 0L;
}

static bool host_try_load_motd_from_path(host_t *host, const char *path);

static struct timespec host_stat_mtime(const struct stat *info)
{
    struct timespec result = {0, 0};
    if (info == nullptr) {
        return result;
    }

#if defined(__APPLE__)
    result.tv_sec = info->st_mtimespec.tv_sec;
    result.tv_nsec = info->st_mtimespec.tv_nsec;
#elif defined(_BSD_SOURCE) || defined(_SVID_SOURCE) || defined(__USE_XOPEN2K8)
    result.tv_sec = info->st_mtim.tv_sec;
    result.tv_nsec = info->st_mtim.tv_nsec;
#else
    result.tv_sec = info->st_mtime;
    result.tv_nsec = 0;
#endif

    if (result.tv_sec < 0) {
        result.tv_sec = 0;
    }
    if (result.tv_nsec < 0) {
        result.tv_nsec = 0;
    }
    return result;
}

static void host_maybe_reload_motd_from_file(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    char stored_path[PATH_MAX];
    stored_path[0] = '\0';
    struct timespec last_loaded = {0, 0};
    bool had_file = false;

    pthread_mutex_lock(&host->lock);
    if (host->motd_path[0] != '\0') {
        snprintf(stored_path, sizeof(stored_path), "%s", host->motd_path);
        last_loaded = host->motd_last_modified;
        had_file = host->motd_has_file;
    }
    pthread_mutex_unlock(&host->lock);

    if (stored_path[0] == '\0') {
        return;
    }

    char resolved_path[PATH_MAX];
    resolved_path[0] = '\0';

    if (stored_path[0] == '~' &&
        (stored_path[1] == '\0' || stored_path[1] == '/')) {
        const char *home = getenv("HOME");
        if (home != nullptr && home[0] != '\0') {
            int expanded = snprintf(resolved_path, sizeof(resolved_path),
                                    "%s%s", home, stored_path + 1);
            if (expanded <= 0 || (size_t)expanded >= sizeof(resolved_path)) {
                resolved_path[0] = '\0';
            }
        }
    }

    const char *path_to_try =
        resolved_path[0] != '\0' ? resolved_path : stored_path;

    struct stat file_info;
    if (stat(path_to_try, &file_info) != 0 || !S_ISREG(file_info.st_mode)) {
        if (!had_file) {
            (void)host_try_load_motd_from_path(host, path_to_try);
        }
        if (had_file) {
            pthread_mutex_lock(&host->lock);
            if (host->motd_has_file && strncmp(host->motd_path, stored_path,
                                               sizeof(host->motd_path)) == 0) {
                host->motd_has_file = false;
                host->motd_last_modified.tv_sec = 0;
                host->motd_last_modified.tv_nsec = 0L;
            }
            pthread_mutex_unlock(&host->lock);
        }
        return;
    }

    struct timespec modified = host_stat_mtime(&file_info);

    if (had_file && modified.tv_sec == last_loaded.tv_sec &&
        modified.tv_nsec == last_loaded.tv_nsec) {
        return;
    }

    (void)host_try_load_motd_from_path(host, path_to_try);
}

static unsigned session_simple_hash(const char *text)
{
    unsigned hash = 5381U;
    if (text == nullptr) {
        return hash;
    }

    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor != '\0'; ++cursor) {
        hash = (hash * 33U) ^ *cursor;
    }
    return hash;
}

static void session_build_captcha_prompt(session_ctx_t *ctx,
                                         captcha_prompt_t *prompt)
{
    if (prompt == nullptr) {
        return;
    }

    memset(prompt, 0, sizeof(*prompt));

    unsigned basis =
        session_simple_hash(ctx != nullptr ? ctx->user.name : "user");
    basis ^= session_simple_hash(ctx != nullptr ? ctx->client_ip : "ip");

    unsigned entropy = 0U;
    struct timespec now = {0, 0};
    if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
        uint64_t now_sec = (uint64_t)now.tv_sec;
        entropy ^= (unsigned)now_sec;
        entropy ^= (unsigned)(now_sec >> 32);
        entropy ^= (unsigned)now.tv_nsec;
    } else {
        uint64_t fallback = (uint64_t)time(nullptr);
        entropy ^= (unsigned)fallback;
        entropy ^= (unsigned)(fallback >> 32);
    }

    host_t *host = (ctx != nullptr) ? ctx->owner : nullptr;
    if (host != nullptr) {
        pthread_mutex_lock(&host->lock);
        uint64_t nonce = ++host->captcha_nonce;
        pthread_mutex_unlock(&host->lock);
        entropy ^= (unsigned)nonce;
        entropy ^= (unsigned)(nonce >> 32);
    }

    basis ^= entropy;

    const unsigned variant_seed = basis ^ (basis >> 16U) ^ (entropy << 1U);
    unsigned prng_state = variant_seed | 1U;

    session_fill_digit_sum_prompt(prompt, &prng_state);
}

typedef struct {
    const char *name;
    const char *code;
} color_entry_t;

static const color_entry_t USER_COLOR_MAP[] = {
    {"black", ANSI_BLACK},
    {"red", ANSI_RED},
    {"green", ANSI_GREEN},
    {"yellow", ANSI_YELLOW},
    {"blue", ANSI_BLUE},
    {"magenta", ANSI_MAGENTA},
    {"cyan", ANSI_CYAN},
    {"white", ANSI_WHITE},
    {"default", ANSI_DEFAULT},

    {"검정", ANSI_BLACK},
    {"검은색", ANSI_BLACK},
    {"黒", ANSI_BLACK},
    {"黑", ANSI_BLACK},
    {"黑色", ANSI_BLACK},
    {"черный", ANSI_BLACK},
    {"чёрный", ANSI_BLACK},

    {"빨강", ANSI_RED},
    {"빨간색", ANSI_RED},
    {"赤", ANSI_RED},
    {"红", ANSI_RED},
    {"红色", ANSI_RED},
    {"красный", ANSI_RED},

    {"초록", ANSI_GREEN},
    {"초록색", ANSI_GREEN},
    {"緑", ANSI_GREEN},
    {"绿", ANSI_GREEN},
    {"绿色", ANSI_GREEN},
    {"зелёный", ANSI_GREEN},
    {"зеленый", ANSI_GREEN},

    {"노랑", ANSI_YELLOW},
    {"노란색", ANSI_YELLOW},
    {"黄色", ANSI_YELLOW},
    {"黄", ANSI_YELLOW},
    {"黄色い", ANSI_YELLOW},
    {"黃色", ANSI_YELLOW},
    {"жёлтый", ANSI_YELLOW},
    {"желтый", ANSI_YELLOW},

    {"파랑", ANSI_BLUE},
    {"파란색", ANSI_BLUE},
    {"青", ANSI_BLUE},
    {"青色", ANSI_BLUE},
    {"蓝", ANSI_BLUE},
    {"蓝色", ANSI_BLUE},
    {"синий", ANSI_BLUE},

    {"마젠타", ANSI_MAGENTA},
    {"자주", ANSI_MAGENTA},
    {"보라", ANSI_MAGENTA},
    {"보라색", ANSI_MAGENTA},
    {"マゼンタ", ANSI_MAGENTA},
    {"紫", ANSI_MAGENTA},
    {"洋红", ANSI_MAGENTA},
    {"品红", ANSI_MAGENTA},
    {"紫色", ANSI_MAGENTA},
    {"пурпурный", ANSI_MAGENTA},
    {"фиолетовый", ANSI_MAGENTA},

    {"시안", ANSI_CYAN},
    {"청록", ANSI_CYAN},
    {"하늘", ANSI_CYAN},
    {"하늘색", ANSI_CYAN},
    {"シアン", ANSI_CYAN},
    {"水色", ANSI_CYAN},
    {"青绿", ANSI_CYAN},
    {"青色", ANSI_CYAN},
    {"青綠", ANSI_CYAN},
    {"青藍", ANSI_CYAN},
    {"青蓝", ANSI_CYAN},
    {"циан", ANSI_CYAN},
    {"бирюзовый", ANSI_CYAN},
    {"голубой", ANSI_CYAN},

    {"하양", ANSI_WHITE},
    {"흰색", ANSI_WHITE},
    {"白", ANSI_WHITE},
    {"白色", ANSI_WHITE},
    {"белый", ANSI_WHITE},

    {"기본", ANSI_DEFAULT},
    {"기본값", ANSI_DEFAULT},
    {"デフォルト", ANSI_DEFAULT},
    {"既定", ANSI_DEFAULT},
    {"默认", ANSI_DEFAULT},
    {"默認", ANSI_DEFAULT},
    {"по умолчанию", ANSI_DEFAULT},

    {"bright-black", ANSI_BRIGHT_BLACK},
    {"bright-red", ANSI_BRIGHT_RED},
    {"bright-green", ANSI_BRIGHT_GREEN},
    {"bright-yellow", ANSI_BRIGHT_YELLOW},
    {"bright-blue", ANSI_BRIGHT_BLUE},
    {"bright-magenta", ANSI_BRIGHT_MAGENTA},
    {"bright-cyan", ANSI_BRIGHT_CYAN},
    {"bright-white", ANSI_BRIGHT_WHITE},

    {"밝은검정", ANSI_BRIGHT_BLACK},
    {"회색", ANSI_BRIGHT_BLACK},
    {"明るい黒", ANSI_BRIGHT_BLACK},
    {"グレー", ANSI_BRIGHT_BLACK},
    {"灰色", ANSI_BRIGHT_BLACK},
    {"серый", ANSI_BRIGHT_BLACK},

    {"밝은빨강", ANSI_BRIGHT_RED},
    {"밝은빨간색", ANSI_BRIGHT_RED},
    {"明るい赤", ANSI_BRIGHT_RED},
    {"亮红", ANSI_BRIGHT_RED},
    {"亮红色", ANSI_BRIGHT_RED},
    {"ярко-красный", ANSI_BRIGHT_RED},

    {"밝은초록", ANSI_BRIGHT_GREEN},
    {"밝은초록색", ANSI_BRIGHT_GREEN},
    {"明るい緑", ANSI_BRIGHT_GREEN},
    {"亮绿", ANSI_BRIGHT_GREEN},
    {"亮绿色", ANSI_BRIGHT_GREEN},
    {"ярко-зелёный", ANSI_BRIGHT_GREEN},
    {"ярко-зеленый", ANSI_BRIGHT_GREEN},

    {"밝은노랑", ANSI_BRIGHT_YELLOW},
    {"밝은노란색", ANSI_BRIGHT_YELLOW},
    {"明るい黄", ANSI_BRIGHT_YELLOW},
    {"亮黄", ANSI_BRIGHT_YELLOW},
    {"亮黄色", ANSI_BRIGHT_YELLOW},
    {"ярко-жёлтый", ANSI_BRIGHT_YELLOW},
    {"ярко-желтый", ANSI_BRIGHT_YELLOW},

    {"밝은파랑", ANSI_BRIGHT_BLUE},
    {"밝은파란색", ANSI_BRIGHT_BLUE},
    {"明るい青", ANSI_BRIGHT_BLUE},
    {"亮蓝", ANSI_BRIGHT_BLUE},
    {"亮蓝色", ANSI_BRIGHT_BLUE},
    {"ярко-синий", ANSI_BRIGHT_BLUE},

    {"밝은마젠타", ANSI_BRIGHT_MAGENTA},
    {"밝은자주", ANSI_BRIGHT_MAGENTA},
    {"밝은보라", ANSI_BRIGHT_MAGENTA},
    {"밝은보라색", ANSI_BRIGHT_MAGENTA},
    {"明るいマゼンタ", ANSI_BRIGHT_MAGENTA},
    {"明るい紫", ANSI_BRIGHT_MAGENTA},
    {"亮洋红", ANSI_BRIGHT_MAGENTA},
    {"亮品红", ANSI_BRIGHT_MAGENTA},
    {"亮紫色", ANSI_BRIGHT_MAGENTA},
    {"ярко-пурпурный", ANSI_BRIGHT_MAGENTA},
    {"ярко-фиолетовый", ANSI_BRIGHT_MAGENTA},

    {"밝은시안", ANSI_BRIGHT_CYAN},
    {"밝은청록", ANSI_BRIGHT_CYAN},
    {"밝은하늘", ANSI_BRIGHT_CYAN},
    {"밝은하늘색", ANSI_BRIGHT_CYAN},
    {"明るいシアン", ANSI_BRIGHT_CYAN},
    {"明るい水色", ANSI_BRIGHT_CYAN},
    {"亮青色", ANSI_BRIGHT_CYAN},
    {"亮青绿", ANSI_BRIGHT_CYAN},
    {"亮青綠", ANSI_BRIGHT_CYAN},
    {"亮青藍", ANSI_BRIGHT_CYAN},
    {"亮青蓝", ANSI_BRIGHT_CYAN},
    {"ярко-циан", ANSI_BRIGHT_CYAN},
    {"ярко-бирюзовый", ANSI_BRIGHT_CYAN},
    {"ярко-голубой", ANSI_BRIGHT_CYAN},

    {"밝은하양", ANSI_BRIGHT_WHITE},
    {"밝은흰색", ANSI_BRIGHT_WHITE},
    {"明るい白", ANSI_BRIGHT_WHITE},
    {"亮白", ANSI_BRIGHT_WHITE},
    {"亮白色", ANSI_BRIGHT_WHITE},
    {"ярко-белый", ANSI_BRIGHT_WHITE},
};

static const color_entry_t HIGHLIGHT_COLOR_MAP[] = {
    {"black", ANSI_BG_BLACK},
    {"red", ANSI_BG_RED},
    {"green", ANSI_BG_GREEN},
    {"yellow", ANSI_BG_YELLOW},
    {"blue", ANSI_BG_BLUE},
    {"magenta", ANSI_BG_MAGENTA},
    {"cyan", ANSI_BG_CYAN},
    {"white", ANSI_BG_WHITE},
    {"default", ANSI_BG_DEFAULT},

    {"검정", ANSI_BG_BLACK},
    {"검은색", ANSI_BG_BLACK},
    {"黒", ANSI_BG_BLACK},
    {"黑", ANSI_BG_BLACK},
    {"黑色", ANSI_BG_BLACK},
    {"черный", ANSI_BG_BLACK},
    {"чёрный", ANSI_BG_BLACK},

    {"빨강", ANSI_BG_RED},
    {"빨간색", ANSI_BG_RED},
    {"赤", ANSI_BG_RED},
    {"红", ANSI_BG_RED},
    {"红色", ANSI_BG_RED},
    {"красный", ANSI_BG_RED},

    {"초록", ANSI_BG_GREEN},
    {"초록색", ANSI_BG_GREEN},
    {"緑", ANSI_BG_GREEN},
    {"绿", ANSI_BG_GREEN},
    {"绿色", ANSI_BG_GREEN},
    {"зелёный", ANSI_BG_GREEN},
    {"зеленый", ANSI_BG_GREEN},

    {"노랑", ANSI_BG_YELLOW},
    {"노란색", ANSI_BG_YELLOW},
    {"黄色", ANSI_BG_YELLOW},
    {"黄", ANSI_BG_YELLOW},
    {"黄色い", ANSI_BG_YELLOW},
    {"黃色", ANSI_BG_YELLOW},
    {"жёлтый", ANSI_BG_YELLOW},
    {"желтый", ANSI_BG_YELLOW},

    {"파랑", ANSI_BG_BLUE},
    {"파란색", ANSI_BG_BLUE},
    {"青", ANSI_BG_BLUE},
    {"青色", ANSI_BG_BLUE},
    {"蓝", ANSI_BG_BLUE},
    {"蓝色", ANSI_BG_BLUE},
    {"синий", ANSI_BG_BLUE},

    {"마젠타", ANSI_BG_MAGENTA},
    {"자주", ANSI_BG_MAGENTA},
    {"보라", ANSI_BG_MAGENTA},
    {"보라색", ANSI_BG_MAGENTA},
    {"マゼンタ", ANSI_BG_MAGENTA},
    {"紫", ANSI_BG_MAGENTA},
    {"洋红", ANSI_BG_MAGENTA},
    {"品红", ANSI_BG_MAGENTA},
    {"紫色", ANSI_BG_MAGENTA},
    {"пурпурный", ANSI_BG_MAGENTA},
    {"фиолетовый", ANSI_BG_MAGENTA},

    {"시안", ANSI_BG_CYAN},
    {"청록", ANSI_BG_CYAN},
    {"하늘", ANSI_BG_CYAN},
    {"하늘색", ANSI_BG_CYAN},
    {"シアン", ANSI_BG_CYAN},
    {"水色", ANSI_BG_CYAN},
    {"青绿", ANSI_BG_CYAN},
    {"青色", ANSI_BG_CYAN},
    {"青綠", ANSI_BG_CYAN},
    {"青藍", ANSI_BG_CYAN},
    {"青蓝", ANSI_BG_CYAN},
    {"циан", ANSI_BG_CYAN},
    {"бирюзовый", ANSI_BG_CYAN},
    {"голубой", ANSI_BG_CYAN},

    {"하양", ANSI_BG_WHITE},
    {"흰색", ANSI_BG_WHITE},
    {"白", ANSI_BG_WHITE},
    {"白色", ANSI_BG_WHITE},
    {"белый", ANSI_BG_WHITE},

    {"기본", ANSI_BG_DEFAULT},
    {"기본값", ANSI_BG_DEFAULT},
    {"デフォルト", ANSI_BG_DEFAULT},
    {"既定", ANSI_BG_DEFAULT},
    {"默认", ANSI_BG_DEFAULT},
    {"默認", ANSI_BG_DEFAULT},
    {"по умолчанию", ANSI_BG_DEFAULT},

    {"bright-black", ANSI_BG_BRIGHT_BLACK},
    {"bright-red", ANSI_BG_BRIGHT_RED},
    {"bright-green", ANSI_BG_BRIGHT_GREEN},
    {"bright-yellow", ANSI_BG_BRIGHT_YELLOW},
    {"bright-blue", ANSI_BG_BRIGHT_BLUE},
    {"bright-magenta", ANSI_BG_BRIGHT_MAGENTA},
    {"bright-cyan", ANSI_BG_BRIGHT_CYAN},
    {"bright-white", ANSI_BG_BRIGHT_WHITE},

    {"밝은검정", ANSI_BG_BRIGHT_BLACK},
    {"회색", ANSI_BG_BRIGHT_BLACK},
    {"明るい黒", ANSI_BG_BRIGHT_BLACK},
    {"グレー", ANSI_BG_BRIGHT_BLACK},
    {"灰色", ANSI_BG_BRIGHT_BLACK},
    {"серый", ANSI_BG_BRIGHT_BLACK},

    {"밝은빨강", ANSI_BG_BRIGHT_RED},
    {"밝은빨간색", ANSI_BG_BRIGHT_RED},
    {"明るい赤", ANSI_BG_BRIGHT_RED},
    {"亮红", ANSI_BG_BRIGHT_RED},
    {"亮红色", ANSI_BG_BRIGHT_RED},
    {"ярко-красный", ANSI_BG_BRIGHT_RED},

    {"밝은초록", ANSI_BG_BRIGHT_GREEN},
    {"밝은초록색", ANSI_BG_BRIGHT_GREEN},
    {"明るい緑", ANSI_BG_BRIGHT_GREEN},
    {"亮绿", ANSI_BG_BRIGHT_GREEN},
    {"亮绿色", ANSI_BG_BRIGHT_GREEN},
    {"ярко-зелёный", ANSI_BG_BRIGHT_GREEN},
    {"ярко-зеленый", ANSI_BG_BRIGHT_GREEN},

    {"밝은노랑", ANSI_BG_BRIGHT_YELLOW},
    {"밝은노란색", ANSI_BG_BRIGHT_YELLOW},
    {"明るい黄", ANSI_BG_BRIGHT_YELLOW},
    {"亮黄", ANSI_BG_BRIGHT_YELLOW},
    {"亮黄色", ANSI_BG_BRIGHT_YELLOW},
    {"ярко-жёлтый", ANSI_BG_BRIGHT_YELLOW},
    {"ярко-желтый", ANSI_BG_BRIGHT_YELLOW},

    {"밝은파랑", ANSI_BG_BRIGHT_BLUE},
    {"밝은파란색", ANSI_BG_BRIGHT_BLUE},
    {"明るい青", ANSI_BG_BRIGHT_BLUE},
    {"亮蓝", ANSI_BG_BRIGHT_BLUE},
    {"亮蓝色", ANSI_BG_BRIGHT_BLUE},
    {"ярко-синий", ANSI_BG_BRIGHT_BLUE},

    {"밝은마젠타", ANSI_BG_BRIGHT_MAGENTA},
    {"밝은자주", ANSI_BG_BRIGHT_MAGENTA},
    {"밝은보라", ANSI_BG_BRIGHT_MAGENTA},
    {"밝은보라색", ANSI_BG_BRIGHT_MAGENTA},
    {"明るいマゼンタ", ANSI_BG_BRIGHT_MAGENTA},
    {"明るい紫", ANSI_BG_BRIGHT_MAGENTA},
    {"亮洋红", ANSI_BG_BRIGHT_MAGENTA},
    {"亮品红", ANSI_BG_BRIGHT_MAGENTA},
    {"亮紫色", ANSI_BG_BRIGHT_MAGENTA},
    {"ярко-пурпурный", ANSI_BG_BRIGHT_MAGENTA},
    {"ярко-фиолетовый", ANSI_BG_BRIGHT_MAGENTA},

    {"밝은시안", ANSI_BG_BRIGHT_CYAN},
    {"밝은청록", ANSI_BG_BRIGHT_CYAN},
    {"밝은하늘", ANSI_BG_BRIGHT_CYAN},
    {"밝은하늘색", ANSI_BG_BRIGHT_CYAN},
    {"明るいシアン", ANSI_BG_BRIGHT_CYAN},
    {"明るい水色", ANSI_BG_BRIGHT_CYAN},
    {"亮青色", ANSI_BG_BRIGHT_CYAN},
    {"亮青绿", ANSI_BG_BRIGHT_CYAN},
    {"亮青綠", ANSI_BG_BRIGHT_CYAN},
    {"亮青藍", ANSI_BG_BRIGHT_CYAN},
    {"亮青蓝", ANSI_BG_BRIGHT_CYAN},
    {"ярко-циан", ANSI_BG_BRIGHT_CYAN},
    {"ярко-бирюзовый", ANSI_BG_BRIGHT_CYAN},
    {"ярко-голубой", ANSI_BG_BRIGHT_CYAN},

    {"밝은하양", ANSI_BG_BRIGHT_WHITE},
    {"밝은흰색", ANSI_BG_BRIGHT_WHITE},
    {"明るい白", ANSI_BG_BRIGHT_WHITE},
    {"亮白", ANSI_BG_BRIGHT_WHITE},
    {"亮白色", ANSI_BG_BRIGHT_WHITE},
    {"ярко-белый", ANSI_BG_BRIGHT_WHITE},
};

#include "ssh_chatter/l10n.h"

typedef struct palette_descriptor {
    const char *id;
    const l10n_string_t *name;
    const l10n_string_t *description;
    const char *user_color_name;
    const char *user_highlight_name;
    bool user_is_bold;
    const char *system_fg_name;
    const char *system_bg_name;
    const char *system_highlight_name;
    bool system_is_bold;
    bool is_256_color;
} palette_descriptor_t;

#define L10N_ALL(str)                                                          \
    {                                                                          \
        (str), (str), (str), (str), (str), (str)                               \
    }

static const l10n_string_t PALETTE_NAME_MONOKAI = L10N_ALL("monokai");
static const l10n_string_t PALETTE_DESC_MONOKAI =
    L10N_ALL("A dark theme with vibrant accent colors, inspired by the "
             "Monokai color scheme.");

static const l10n_string_t PALETTE_NAME_WINDOWS = L10N_ALL("windows");
static const l10n_string_t PALETTE_DESC_WINDOWS =
    L10N_ALL("High contrast palette reminiscent of Windows");
static const l10n_string_t PALETTE_NAME_GNU_LINUX = L10N_ALL("gnu-linux");
static const l10n_string_t PALETTE_DESC_GNU_LINUX = L10N_ALL(
    "Modern, elegant, and free. the universal rhythm of your workflow.");
static const l10n_string_t PALETTE_NAME_MACOS = L10N_ALL("macos");
static const l10n_string_t PALETTE_DESC_MACOS =
    L10N_ALL("Precision in silence. Minimalist contemporary unix.");
static const l10n_string_t PALETTE_NAME_FREEBSD = L10N_ALL("freebsd");
static const l10n_string_t PALETTE_DESC_FREEBSD =
    L10N_ALL("Rigid and graceful BSD. The biggest 'True UNIX'");
static const l10n_string_t PALETTE_NAME_SOLARIS = L10N_ALL("solaris");
static const l10n_string_t PALETTE_DESC_SOLARIS =
    L10N_ALL("Ancient sun of enterprise UNIX: Sun, Machine, nostalgia.");
static const l10n_string_t PALETTE_NAME_OPENBSD_FORTRESS =
    L10N_ALL("openbsd-fortress");
static const l10n_string_t PALETTE_DESC_OPENBSD_FORTRESS = L10N_ALL(
    "Security through simplicity. calm blue walls over disciplined darkness.");
static const l10n_string_t PALETTE_NAME_NETBSD_UNIVERSAL =
    L10N_ALL("netbsd-universal");
static const l10n_string_t PALETTE_DESC_NETBSD_UNIVERSAL =
    L10N_ALL("Runs on anything. Maybe your fridge, too?");
static const l10n_string_t PALETTE_NAME_MOE = L10N_ALL("moe");
static const l10n_string_t PALETTE_DESC_MOE =
    L10N_ALL("Soft magenta accents with playful highlights");
static const l10n_string_t PALETTE_NAME_NEON_GENESIS_EVANGELION =
    L10N_ALL("neon-genesis-evangelion");
static const l10n_string_t PALETTE_DESC_NEON_GENESIS_EVANGELION =
    L10N_ALL("Sho-nen yo Shin-wa ni nare--");
static const l10n_string_t PALETTE_NAME_MEGAMI = L10N_ALL("megami");
static const l10n_string_t PALETTE_DESC_MEGAMI =
    L10N_ALL("Japanese anime goddess cliché");
static const l10n_string_t PALETTE_NAME_CLEAN = L10N_ALL("clean");
static const l10n_string_t PALETTE_DESC_CLEAN =
    L10N_ALL("Balanced neutral palette");
static const l10n_string_t PALETTE_NAME_ADWAITA = L10N_ALL("adwaita");
static const l10n_string_t PALETTE_DESC_ADWAITA =
    L10N_ALL("Bright background inspired by GNOME Adwaita");
static const l10n_string_t PALETTE_NAME_80SHACKER = L10N_ALL("80shacker");
static const l10n_string_t PALETTE_DESC_80SHACKER =
    L10N_ALL("Bright monochrome green inspired by old CRT");
static const l10n_string_t PALETTE_NAME_PLATO = L10N_ALL("plato");
static const l10n_string_t PALETTE_DESC_PLATO =
    L10N_ALL("Bright monochrome yellow inspired by old Amber CRT");
static const l10n_string_t PALETTE_NAME_ATARIST = L10N_ALL("atarist");
static const l10n_string_t PALETTE_DESC_ATARIST =
    L10N_ALL("Sharp paper-white monochrome for high-res work");
static const l10n_string_t PALETTE_NAME_WIN95BSOD = L10N_ALL("win95bsod");
static const l10n_string_t PALETTE_DESC_WIN95BSOD =
    L10N_ALL("High-contrast blue screen of death style");
static const l10n_string_t PALETTE_NAME_CHN_HANZI = L10N_ALL("chn-hanzi");
static const l10n_string_t PALETTE_DESC_CHN_HANZI =
    L10N_ALL("Bright cyan high-clarity Chinese text terminal");
static const l10n_string_t PALETTE_NAME_USA_FLAG = L10N_ALL("usa-flag");
static const l10n_string_t PALETTE_DESC_USA_FLAG =
    L10N_ALL("Flag blue base with red/white highlights");
static const l10n_string_t PALETTE_NAME_JPN_FLAG = L10N_ALL("jpn-flag");
static const l10n_string_t PALETTE_DESC_JPN_FLAG =
    L10N_ALL("Minimalist white with rising sun red accent");
static const l10n_string_t PALETTE_NAME_CHN_FLAG = L10N_ALL("chn-flag");
static const l10n_string_t PALETTE_DESC_CHN_FLAG =
    L10N_ALL("Star-red background with lucky yellow text");
static const l10n_string_t PALETTE_NAME_RUS_FLAG = L10N_ALL("rus-flag");
static const l10n_string_t PALETTE_DESC_RUS_FLAG =
    L10N_ALL("Tricolor base with strong red emphasis");
static const l10n_string_t PALETTE_NAME_DE_FLAG = L10N_ALL("de-flag");
static const l10n_string_t PALETTE_DESC_DE_FLAG =
    L10N_ALL("Tricolor base with strong red/yellow emphasis");
static const l10n_string_t PALETTE_NAME_HOLY_LIGHT = L10N_ALL("holy-light");
static const l10n_string_t PALETTE_DESC_HOLY_LIGHT =
    L10N_ALL("Christian sacred light on pure white/blue base");
static const l10n_string_t PALETTE_NAME_ISLAM = L10N_ALL("islam");
static const l10n_string_t PALETTE_DESC_ISLAM =
    L10N_ALL("Iconic color of muslim, white/green base");
static const l10n_string_t PALETTE_NAME_DHARMA_OCHRE = L10N_ALL("dharma-ochre");
static const l10n_string_t PALETTE_DESC_DHARMA_OCHRE =
    L10N_ALL("Ochre robes of enlightenment and vitality");
static const l10n_string_t PALETTE_NAME_YIN_YANG = L10N_ALL("yin-yang");
static const l10n_string_t PALETTE_DESC_YIN_YANG =
    L10N_ALL("Balance of Black and White with Jade accent");
static const l10n_string_t PALETTE_NAME_SOVIET_COLD = L10N_ALL("soviet-cold");
static const l10n_string_t PALETTE_DESC_SOVIET_COLD =
    L10N_ALL("Cold blue/white terminal for scientific systems");
static const l10n_string_t PALETTE_NAME_HI_TEL = L10N_ALL("hi-tel");
static const l10n_string_t PALETTE_DESC_HI_TEL =
    L10N_ALL("1990s Korean BBS blue background and text style");
static const l10n_string_t PALETTE_NAME_BLACKPINK = L10N_ALL("blackpink");
static const l10n_string_t PALETTE_DESC_BLACKPINK = L10N_ALL(
    "K-Pop inspired palette with a black background and pink accents.");
static const l10n_string_t PALETTE_NAME_AMIGA_CLI = L10N_ALL("amiga-cli");
static const l10n_string_t PALETTE_DESC_AMIGA_CLI =
    L10N_ALL("AmigaOS style with cyan/blue");
static const l10n_string_t PALETTE_NAME_JPN_PC98 = L10N_ALL("jpn-pc98");
static const l10n_string_t PALETTE_DESC_JPN_PC98 =
    L10N_ALL("NEC PC-9801 subtle, earthy low-res tones");
static const l10n_string_t PALETTE_NAME_DEEP_BLUE = L10N_ALL("deep-blue");
static const l10n_string_t PALETTE_DESC_DEEP_BLUE =
    L10N_ALL("IBM Supercomputer monitoring interface style");
static const l10n_string_t PALETTE_NAME_KOREA = L10N_ALL("korea");
static const l10n_string_t PALETTE_DESC_KOREA =
    L10N_ALL("Taegeuk-gi inspired black base with red and blue accents");
static const l10n_string_t PALETTE_NAME_NEO_SEOUL = L10N_ALL("neo-seoul");
static const l10n_string_t PALETTE_DESC_NEO_SEOUL =
    L10N_ALL("Neon skyline of Gangnam and Hongdae: glowing magenta and cyan "
             "lights on dark asphalt");
static const l10n_string_t PALETTE_NAME_INCHEON_INDUSTRIAL =
    L10N_ALL("incheon-industrial");
static const l10n_string_t PALETTE_DESC_INCHEON_INDUSTRIAL =
    L10N_ALL("Metallic cranes and sodium streetlights of Incheon docks");
static const l10n_string_t PALETTE_NAME_GYEONGGI_MODERN =
    L10N_ALL("gyeonggi-modern");
static const l10n_string_t PALETTE_DESC_GYEONGGI_MODERN = L10N_ALL(
    "Suburban calm of modern Korea. asphalt gray and warm window light");
static const l10n_string_t PALETTE_NAME_KOREAN_PALACE =
    L10N_ALL("korean-palace");
static const l10n_string_t PALETTE_DESC_KOREAN_PALACE =
    L10N_ALL("Royal dancheong harmony: jade green, vermilion red, and gold "
             "over black lacquer");
static const l10n_string_t PALETTE_NAME_GYEONGSANGBUKDO =
    L10N_ALL("gyeongsangbukdo");
static const l10n_string_t PALETTE_DESC_GYEONGSANGBUKDO =
    L10N_ALL("Stoic mountains and agricultural spirit. stone, pine, and the "
             "quiet gold of temples");
static const l10n_string_t PALETTE_NAME_DAEGU_SUMMER = L10N_ALL("daegu-summer");
static const l10n_string_t PALETTE_DESC_DAEGU_SUMMER =
    L10N_ALL("The biggest, the hottest of north gyeongsang: Blazing "
             "red-orange heat and festival gold under night sky");
static const l10n_string_t PALETTE_NAME_GYEONGJU_HERITAGE =
    L10N_ALL("gyeongju-heritage");
static const l10n_string_t PALETTE_DESC_GYEONGJU_HERITAGE =
    L10N_ALL("Eternal relics and golden crowns: moonlit stone and ancient "
             "buddhism with blue flag of shilla military force");
static const l10n_string_t PALETTE_NAME_KANGWON_WINTER =
    L10N_ALL("kangwon-winter");
static const l10n_string_t PALETTE_DESC_KANGWON_WINTER =
    L10N_ALL("Cold white peaks and blue shadows of Gangwon’s frozen dawn");
static const l10n_string_t PALETTE_NAME_ULSAN_STEEL = L10N_ALL("ulsan-steel");
static const l10n_string_t PALETTE_DESC_ULSAN_STEEL =
    L10N_ALL("Molten metal glow inside heavy industry furnace halls");
static const l10n_string_t PALETTE_NAME_JEOLLA_SEASIDE =
    L10N_ALL("jeolla-seaside");
static const l10n_string_t PALETTE_DESC_JEOLLA_SEASIDE =
    L10N_ALL("Quiet sea and horizon light of Mokpo and Yeosu nights");
static const l10n_string_t PALETTE_NAME_GWANGJU_BIENNALE =
    L10N_ALL("gwangju-biennale");
static const l10n_string_t PALETTE_DESC_GWANGJU_BIENNALE =
    L10N_ALL("Experimental art city with a heritage of democracy: violet "
             "neon and philosophical blue");
static const l10n_string_t PALETTE_NAME_JEONJU_HANOK = L10N_ALL("jeonju-hanok");
static const l10n_string_t PALETTE_DESC_JEONJU_HANOK =
    L10N_ALL("The symbol of north jeolla. warm roofs and calm golden light");
static const l10n_string_t PALETTE_NAME_DAEJEON_TECH = L10N_ALL("daejeon-tech");
static const l10n_string_t PALETTE_DESC_DAEJEON_TECH = L10N_ALL(
    "Futuristic research district glow: clean LED light on steel gray night");
static const l10n_string_t PALETTE_NAME_SEJONG_NIGHT = L10N_ALL("sejong-night");
static const l10n_string_t PALETTE_DESC_SEJONG_NIGHT =
    L10N_ALL("Balanced dark-blue administration city under cool LED light");
static const l10n_string_t PALETTE_NAME_CHEONGJU_INTELLECT =
    L10N_ALL("cheongju-intellect");
static const l10n_string_t PALETTE_DESC_CHEONGJU_INTELLECT =
    L10N_ALL("Scholarly ink and soft dawn over hills: serene blue clarity");
static const l10n_string_t PALETTE_NAME_CHUNGCHEONG_FIELD =
    L10N_ALL("chungcheong-field");
static const l10n_string_t PALETTE_DESC_CHUNGCHEONG_FIELD =
    L10N_ALL("Muted greens and dust gold of inland farmlands");
static const l10n_string_t PALETTE_NAME_JEJU_ROCK = L10N_ALL("jeju-rock");
static const l10n_string_t PALETTE_DESC_JEJU_ROCK =
    L10N_ALL("Volcanic basalt, moss green, and deep sea mist of Jeju Island");
static const l10n_string_t PALETTE_NAME_GYEONGSANGNAMDO =
    L10N_ALL("gyeongsangnamdo");
static const l10n_string_t PALETTE_DESC_GYEONGSANGNAMDO = L10N_ALL(
    "Sea breeze and industry — blue steel, orange dusk, and vibrant harbors");
static const l10n_string_t PALETTE_NAME_BUSAN_HARBOR = L10N_ALL("busan-harbor");
static const l10n_string_t PALETTE_DESC_BUSAN_HARBOR =
    L10N_ALL("Night harbor lights and steel-blue waters of Busan Port");
static const l10n_string_t PALETTE_NAME_HAN = L10N_ALL("han");
static const l10n_string_t PALETTE_DESC_HAN = L10N_ALL(
    "Deep unresolved sorrow and austere beauty pale blue and gray layers");
static const l10n_string_t PALETTE_NAME_JEONG = L10N_ALL("jeong");
static const l10n_string_t PALETTE_DESC_JEONG =
    L10N_ALL("Warm emotional bonds and communal comfort soft red and gold "
             "glow on darkness");
static const l10n_string_t PALETTE_NAME_HEUNG = L10N_ALL("heung");
static const l10n_string_t PALETTE_DESC_HEUNG =
    L10N_ALL("Joyful energy and dynamic spirit: brilliant magenta and "
             "yellow over black");
static const l10n_string_t PALETTE_NAME_NUNCHI = L10N_ALL("nunchi");
static const l10n_string_t PALETTE_DESC_NUNCHI =
    L10N_ALL("Subtle perception and quiet adaptation: dim neutral tones "
             "with cyan glints");
static const l10n_string_t PALETTE_NAME_PCBANG_NIGHT = L10N_ALL("pcbang-night");
static const l10n_string_t PALETTE_DESC_PCBANG_NIGHT =
    L10N_ALL("Late-night gaming neon: cold blue LEDs, energy drink, and so on");
static const l10n_string_t PALETTE_NAME_ALCOHOL = L10N_ALL("alcohol");
static const l10n_string_t PALETTE_DESC_ALCOHOL = L10N_ALL(
    "Soju nights and neon haze: industrial green bottles and pink laughter");
static const l10n_string_t PALETTE_NAME_KOREAN_HARDCORE =
    L10N_ALL("korean-hardcore");
static const l10n_string_t PALETTE_DESC_KOREAN_HARDCORE = L10N_ALL(
    "I don't wanna die yet! neon blood and cold steel over asphalt black");
static const l10n_string_t PALETTE_NAME_KOREAN_NATIONALISTS =
    L10N_ALL("korean-nationalists");
static const l10n_string_t PALETTE_DESC_KOREAN_NATIONALISTS =
    L10N_ALL("Slightly exclusive types. you know the kind.");
static const l10n_string_t PALETTE_NAME_MEDIEVAL_KOREA =
    L10N_ALL("medieval-korea");
static const l10n_string_t PALETTE_DESC_MEDIEVAL_KOREA =
    L10N_ALL("Celadon grace and temple gold over aged ink-black lacquer");
static const l10n_string_t PALETTE_NAME_STONEAGE_KOREA =
    L10N_ALL("stoneage-korea");
static const l10n_string_t PALETTE_DESC_STONEAGE_KOREA =
    L10N_ALL("Primitive contrast of pale clothing and ground stone tools - "
             "raw earth and silence");
static const l10n_string_t PALETTE_NAME_FLAME_AND_BLOOD =
    L10N_ALL("flame-and-blood");
static const l10n_string_t PALETTE_DESC_FLAME_AND_BLOOD = L10N_ALL(
    "An East Asian war of 1592–1598. A great conflict akin to a world war, "
    "where flame met blood and nothing could be forsaken.");
static const l10n_string_t PALETTE_NAME_KOREAN_WAR = L10N_ALL("korean-war");
static const l10n_string_t PALETTE_DESC_KOREAN_WAR = L10N_ALL(
    "The Korean War: an unforgettable sorrow beneath ash, blood, and snow.");
static const l10n_string_t PALETTE_NAME_INDEPENDENCE_SPIRIT =
    L10N_ALL("independence-spirit");
static const l10n_string_t PALETTE_DESC_INDEPENDENCE_SPIRIT =
    L10N_ALL("The spirit of independence. A soul that we must remember.");

static const l10n_string_t PALETTE_NAME_USA_FLAG_256 = L10N_ALL("usa-flag-256");
static const l10n_string_t PALETTE_DESC_USA_FLAG_256 = L10N_ALL(
    "256-color reinterpretation of Flag blue base with red/white highlights");
static const l10n_string_t PALETTE_NAME_JPN_FLAG_256 = L10N_ALL("jpn-flag-256");
static const l10n_string_t PALETTE_DESC_JPN_FLAG_256 =
    L10N_ALL("256-color reinterpretation of Minimalist white with rising "
             "sun red accent");
static const l10n_string_t PALETTE_NAME_CHN_FLAG_256 = L10N_ALL("chn-flag-256");
static const l10n_string_t PALETTE_DESC_CHN_FLAG_256 = L10N_ALL(
    "256-color reinterpretation of Star-red background with lucky yellow text");
static const l10n_string_t PALETTE_NAME_RUS_FLAG_256 = L10N_ALL("rus-flag-256");
static const l10n_string_t PALETTE_DESC_RUS_FLAG_256 = L10N_ALL(
    "256-color reinterpretation of Tricolor base with strong red emphasis");
static const l10n_string_t PALETTE_NAME_DE_FLAG_256 = L10N_ALL("de-flag-256");
static const l10n_string_t PALETTE_DESC_DE_FLAG_256 =
    L10N_ALL("256-color reinterpretation of Tricolor base with strong "
             "red/yellow emphasis");
static const l10n_string_t PALETTE_NAME_HOLY_LIGHT_256 =
    L10N_ALL("holy-light-256");
static const l10n_string_t PALETTE_DESC_HOLY_LIGHT_256 =
    L10N_ALL("256-color reinterpretation of Christian sacred light on pure "
             "white/blue base");
static const l10n_string_t PALETTE_NAME_ISLAM_256 = L10N_ALL("islam-256");
static const l10n_string_t PALETTE_DESC_ISLAM_256 = L10N_ALL(
    "256-color reinterpretation of Iconic color of muslim, white/green base");
static const l10n_string_t PALETTE_NAME_DHARMA_OCHRE_256 =
    L10N_ALL("dharma-ochre-256");
static const l10n_string_t PALETTE_DESC_DHARMA_OCHRE_256 = L10N_ALL(
    "256-color reinterpretation of Ochre robes of enlightenment and vitality");
static const l10n_string_t PALETTE_NAME_YIN_YANG_256 = L10N_ALL("yin-yang-256");
static const l10n_string_t PALETTE_DESC_YIN_YANG_256 =
    L10N_ALL("256-color reinterpretation of Balance of Black and White with "
             "Jade accent");
static const l10n_string_t PALETTE_NAME_SOVIET_COLD_256 =
    L10N_ALL("soviet-cold-256");
static const l10n_string_t PALETTE_DESC_SOVIET_COLD_256 =
    L10N_ALL("256-color reinterpretation of Cold blue/white terminal for "
             "scientific systems");
static const l10n_string_t PALETTE_NAME_HI_TEL_256 = L10N_ALL("hi-tel-256");
static const l10n_string_t PALETTE_DESC_HI_TEL_256 =
    L10N_ALL("256-color reinterpretation of 1990s Korean BBS blue "
             "background and text style");
static const l10n_string_t PALETTE_NAME_AMIGA_CLI_256 =
    L10N_ALL("amiga-cli-256");
static const l10n_string_t PALETTE_DESC_AMIGA_CLI_256 =
    L10N_ALL("256-color reinterpretation of AmigaOS style with cyan/blue");
static const l10n_string_t PALETTE_NAME_JPN_PC98_256 = L10N_ALL("jpn-pc98-256");
static const l10n_string_t PALETTE_DESC_JPN_PC98_256 = L10N_ALL(
    "256-color reinterpretation of NEC PC-9801 subtle, earthy low-res tones");
static const l10n_string_t PALETTE_NAME_DEEP_BLUE_256 =
    L10N_ALL("deep-blue-256");
static const l10n_string_t PALETTE_DESC_DEEP_BLUE_256 =
    L10N_ALL("256-color reinterpretation of IBM Supercomputer monitoring "
             "interface style");
static const l10n_string_t PALETTE_NAME_KOREA_256 = L10N_ALL("korea-256");
static const l10n_string_t PALETTE_DESC_KOREA_256 =
    L10N_ALL("256-color reinterpretation of Taegeuk-gi inspired black base "
             "with red and blue accents");
static const l10n_string_t PALETTE_NAME_NEO_SEOUL_256 =
    L10N_ALL("neo-seoul-256");
static const l10n_string_t PALETTE_DESC_NEO_SEOUL_256 =
    L10N_ALL("256-color reinterpretation of Neon skyline of Gangnam and "
             "Hongdae: glowing magenta and cyan lights on dark asphalt");
static const l10n_string_t PALETTE_NAME_INCHEON_INDUSTRIAL_256 =
    L10N_ALL("incheon-industrial-256");
static const l10n_string_t PALETTE_DESC_INCHEON_INDUSTRIAL_256 =
    L10N_ALL("256-color reinterpretation of Metallic cranes and sodium "
             "streetlights of Incheon docks");
static const l10n_string_t PALETTE_NAME_GYEONGGI_MODERN_256 =
    L10N_ALL("gyeonggi-modern-256");
static const l10n_string_t PALETTE_DESC_GYEONGGI_MODERN_256 =
    L10N_ALL("256-color reinterpretation of Suburban calm of modern Korea. "
             "asphalt gray and warm window light");
static const l10n_string_t PALETTE_NAME_KOREAN_PALACE_256 =
    L10N_ALL("korean-palace-256");
static const l10n_string_t PALETTE_DESC_KOREAN_PALACE_256 =
    L10N_ALL("256-color reinterpretation of Royal dancheong harmony: jade "
             "green, vermilion red, and gold over black lacquer");
static const l10n_string_t PALETTE_NAME_GYEONGSANGBUKDO_256 =
    L10N_ALL("gyeongsangbukdo-256");
static const l10n_string_t PALETTE_DESC_GYEONGSANGBUKDO_256 = L10N_ALL(
    "256-color reinterpretation of Stoic mountains and agricultural spirit. "
    "stone, pine, and the quiet gold of temples");
static const l10n_string_t PALETTE_NAME_DAEGU_SUMMER_256 =
    L10N_ALL("daegu-summer-256");
static const l10n_string_t PALETTE_DESC_DAEGU_SUMMER_256 = L10N_ALL(
    "256-color reinterpretation of The biggest, the hottest of north "
    "gyeongsang: Blazing red-orange heat and festival gold under night sky");
static const l10n_string_t PALETTE_NAME_GYEONGJU_HERITAGE_256 =
    L10N_ALL("gyeongju-heritage-256");
static const l10n_string_t PALETTE_DESC_GYEONGJU_HERITAGE_256 = L10N_ALL(
    "256-color reinterpretation of Eternal relics and golden crowns: moonlit "
    "stone and ancient buddhism with blue flag of shilla military force");
static const l10n_string_t PALETTE_NAME_KANGWON_WINTER_256 =
    L10N_ALL("kangwon-winter-256");
static const l10n_string_t PALETTE_DESC_KANGWON_WINTER_256 =
    L10N_ALL("256-color reinterpretation of Cold white peaks and blue "
             "shadows of Gangwon’s frozen dawn");
static const l10n_string_t PALETTE_NAME_ULSAN_STEEL_256 =
    L10N_ALL("ulsan-steel-256");
static const l10n_string_t PALETTE_DESC_ULSAN_STEEL_256 =
    L10N_ALL("256-color reinterpretation of Molten metal glow inside heavy "
             "industry furnace halls");
static const l10n_string_t PALETTE_NAME_JEOLLA_SEASIDE_256 =
    L10N_ALL("jeolla-seaside-256");
static const l10n_string_t PALETTE_DESC_JEOLLA_SEASIDE_256 =
    L10N_ALL("256-color reinterpretation of Quiet sea and horizon light of "
             "Mokpo and Yeosu nights");
static const l10n_string_t PALETTE_NAME_GWANGJU_BIENNALE_256 =
    L10N_ALL("gwangju-biennale-256");
static const l10n_string_t PALETTE_DESC_GWANGJU_BIENNALE_256 =
    L10N_ALL("256-color reinterpretation of Experimental art city with a "
             "heritage of democracy: violet neon and philosophical blue");
static const l10n_string_t PALETTE_NAME_JEONJU_HANOK_256 =
    L10N_ALL("jeonju-hanok-256");
static const l10n_string_t PALETTE_DESC_JEONJU_HANOK_256 =
    L10N_ALL("256-color reinterpretation of The symbol of north jeolla. "
             "warm roofs and calm golden light");
static const l10n_string_t PALETTE_NAME_DAEJEON_TECH_256 =
    L10N_ALL("daejeon-tech-256");
static const l10n_string_t PALETTE_DESC_DAEJEON_TECH_256 =
    L10N_ALL("256-color reinterpretation of Futuristic research district "
             "glow: clean LED light on steel gray night");
static const l10n_string_t PALETTE_NAME_SEJONG_NIGHT_256 =
    L10N_ALL("sejong-night-256");
static const l10n_string_t PALETTE_DESC_SEJONG_NIGHT_256 =
    L10N_ALL("256-color reinterpretation of Balanced dark-blue "
             "administration city under cool LED light");
static const l10n_string_t PALETTE_NAME_CHEONGJU_INTELLECT_256 =
    L10N_ALL("cheongju-intellect-256");
static const l10n_string_t PALETTE_DESC_CHEONGJU_INTELLECT_256 =
    L10N_ALL("256-color reinterpretation of Scholarly ink and soft dawn "
             "over hills: serene blue clarity");
static const l10n_string_t PALETTE_NAME_CHUNGCHEONG_FIELD_256 =
    L10N_ALL("chungcheong-field-256");
static const l10n_string_t PALETTE_DESC_CHUNGCHEONG_FIELD_256 =
    L10N_ALL("256-color reinterpretation of Muted greens and dust gold of "
             "inland farmlands");
static const l10n_string_t PALETTE_NAME_JEJU_ROCK_256 =
    L10N_ALL("jeju-rock-256");
static const l10n_string_t PALETTE_DESC_JEJU_ROCK_256 =
    L10N_ALL("256-color reinterpretation of Volcanic basalt, moss green, "
             "and deep sea mist of Jeju Island");
static const l10n_string_t PALETTE_NAME_GYEONGSANGNAMDO_256 =
    L10N_ALL("gyeongsangnamdo-256");
static const l10n_string_t PALETTE_DESC_GYEONGSANGNAMDO_256 =
    L10N_ALL("256-color reinterpretation of Sea breeze and industry — blue "
             "steel, orange dusk, and vibrant harbors");
static const l10n_string_t PALETTE_NAME_BUSAN_HARBOR_256 =
    L10N_ALL("busan-harbor-256");
static const l10n_string_t PALETTE_DESC_BUSAN_HARBOR_256 =
    L10N_ALL("256-color reinterpretation of Night harbor lights and "
             "steel-blue waters of Busan Port");
static const l10n_string_t PALETTE_NAME_HAN_256 = L10N_ALL("han-256");
static const l10n_string_t PALETTE_DESC_HAN_256 =
    L10N_ALL("256-color reinterpretation of Deep unresolved sorrow and "
             "austere beauty pale blue and gray layers");
static const l10n_string_t PALETTE_NAME_JEONG_256 = L10N_ALL("jeong-256");
static const l10n_string_t PALETTE_DESC_JEONG_256 =
    L10N_ALL("256-color reinterpretation of Warm emotional bonds and "
             "communal comfort soft red and gold glow on darkness");
static const l10n_string_t PALETTE_NAME_HEUNG_256 = L10N_ALL("heung-256");
static const l10n_string_t PALETTE_DESC_HEUNG_256 =
    L10N_ALL("256-color reinterpretation of Joyful energy and dynamic "
             "spirit: brilliant magenta and yellow over black");
static const l10n_string_t PALETTE_NAME_NUNCHI_256 = L10N_ALL("nunchi-256");
static const l10n_string_t PALETTE_DESC_NUNCHI_256 =
    L10N_ALL("256-color reinterpretation of Subtle perception and quiet "
             "adaptation: dim neutral tones with blue glints");
static const l10n_string_t PALETTE_NAME_PCBANG_NIGHT_256 =
    L10N_ALL("pcbang-night-256");
static const l10n_string_t PALETTE_DESC_PCBANG_NIGHT_256 =
    L10N_ALL("256-color reinterpretation of Late-night gaming neon: cold "
             "blue LEDs, energy drink, and so on");
static const l10n_string_t PALETTE_NAME_ALCOHOL_256 = L10N_ALL("alcohol-256");
static const l10n_string_t PALETTE_DESC_ALCOHOL_256 =
    L10N_ALL("256-color reinterpretation of Soju nights and neon haze: "
             "industrial green bottles and pink laughter");
static const l10n_string_t PALETTE_NAME_KOREAN_HARDCORE_256 =
    L10N_ALL("korean-hardcore-256");
static const l10n_string_t PALETTE_DESC_KOREAN_HARDCORE_256 =
    L10N_ALL("256-color reinterpretation of I don't wanna die yet! neon "
             "blood and cold steel over asphalt black");
static const l10n_string_t PALETTE_NAME_KOREAN_NATIONALISTS_256 =
    L10N_ALL("korean-nationalists-256");
static const l10n_string_t PALETTE_DESC_KOREAN_NATIONALISTS_256 =
    L10N_ALL("256-color reinterpretation of Slightly exclusive types. you "
             "know the kind.");
static const l10n_string_t PALETTE_NAME_MEDIEVAL_KOREA_256 =
    L10N_ALL("medieval-korea-256");
static const l10n_string_t PALETTE_DESC_MEDIEVAL_KOREA_256 =
    L10N_ALL("256-color reinterpretation of Celadon grace and temple gold "
             "over aged ink-black lacquer");
static const l10n_string_t PALETTE_NAME_STONEAGE_KOREA_256 =
    L10N_ALL("stoneage-korea-256");
static const l10n_string_t PALETTE_DESC_STONEAGE_KOREA_256 =
    L10N_ALL("256-color reinterpretation of Primitive contrast of pale "
             "clothing and ground stone tools - raw earth and silence");
static const l10n_string_t PALETTE_NAME_FLAME_AND_BLOOD_256 =
    L10N_ALL("flame-and-blood-256");
static const l10n_string_t PALETTE_DESC_FLAME_AND_BLOOD_256 =
    L10N_ALL("256-color reinterpretation of An East Asian war of 1592–1598. "
             "A great conflict akin to a world war, where flame met blood "
             "and nothing could be forsaken.");
static const l10n_string_t PALETTE_NAME_KOREAN_WAR_256 =
    L10N_ALL("korean-war-256");
static const l10n_string_t PALETTE_DESC_KOREAN_WAR_256 =
    L10N_ALL("256-color reinterpretation of The Korean War: an "
             "unforgettable sorrow beneath ash, blood, and snow.");
static const l10n_string_t PALETTE_NAME_INDEPENDENCE_SPIRIT_256 =
    L10N_ALL("independence-spirit-256");
static const l10n_string_t PALETTE_DESC_INDEPENDENCE_SPIRIT_256 =
    L10N_ALL("256-color reinterpretation of The spirit of independence. A "
             "soul that we must remember.");

static const l10n_string_t PALETTE_NAME_FRENCH_ROCOCO_256 =
    L10N_ALL("french-rococo-256");
static const l10n_string_t PALETTE_DESC_FRENCH_ROCOCO_256 =
    L10N_ALL("Dramatic Silence—Rameau to Couperin");

static const l10n_string_t PALETTE_NAME_POLISH_CATHOLIC_256 =
    L10N_ALL("polish-catholic-256");
static const l10n_string_t PALETTE_DESC_POLISH_CATHOLIC_256 =
    L10N_ALL("Sacred Echoes—Chant to Solidarity");

static const l10n_string_t PALETTE_NAME_GERMAN_INDUSTRY_256 =
    L10N_ALL("german-industry-256");
static const l10n_string_t PALETTE_DESC_GERMAN_INDUSTRY_256 =
    L10N_ALL("Iron Will & Innovation—Ruhr to Raumfahrt");

static const l10n_string_t PALETTE_NAME_LEIPZIG_BLACK_256 =
    L10N_ALL("leipzig-black-256");
static const l10n_string_t PALETTE_DESC_LEIPZIG_BLACK_256 =
    L10N_ALL("Timeless Trendsetter—J.S Bach To Graffiti");

static const l10n_string_t PALETTE_NAME_CHOPIN_MAZURKA_256 =
    L10N_ALL("chopin-mazurka-256");
static const l10n_string_t PALETTE_DESC_CHOPIN_MAZURKA_256 =
    L10N_ALL("Poetic Melancholy—Salon to Soul");

static const palette_descriptor_t PALETTE_DEFINITIONS[] = {
    {"windows", &PALETTE_NAME_WINDOWS, &PALETTE_DESC_WINDOWS, "cyan", "blue",
     true, "white", "blue", "yellow", true, false},
    {"gnu-linux", &PALETTE_NAME_GNU_LINUX, &PALETTE_DESC_GNU_LINUX,
     "bright-green", "black", true, "blue", "black", "bright-yellow", true,
     false},
    {"macos", &PALETTE_NAME_MACOS, &PALETTE_DESC_MACOS, "bright-white", "black",
     false, "bright-blue", "black", "white", false, false},
    {"freebsd", &PALETTE_NAME_FREEBSD, &PALETTE_DESC_FREEBSD, "bright-red",
     "black", false, "red", "black", "bright-white", false, false},
    {"solaris", &PALETTE_NAME_SOLARIS, &PALETTE_DESC_SOLARIS, "bright-yellow",
     "black", true, "bright-red", "black", "bright-white", true, false},
    {"openbsd-fortress", &PALETTE_NAME_OPENBSD_FORTRESS,
     &PALETTE_DESC_OPENBSD_FORTRESS, "bright-blue", "black", false,
     "bright-white", "black", "cyan", false, false},
    {"netbsd-universal", &PALETTE_NAME_NETBSD_UNIVERSAL,
     &PALETTE_DESC_NETBSD_UNIVERSAL, "bright-cyan", "black", false,
     "bright-white", "black", "bright-yellow", false, false},
    {"moe", &PALETTE_NAME_MOE, &PALETTE_DESC_MOE, "bright-magenta", "white",
     true, "white", "magenta", "cyan", true, false},
    {"neon-genesis-evangelion", &PALETTE_NAME_NEON_GENESIS_EVANGELION,
     &PALETTE_DESC_NEON_GENESIS_EVANGELION, "bright-red", "white", true,
     "white", "magenta", "blue", true, false},
    {"megami", &PALETTE_NAME_MEGAMI, &PALETTE_DESC_MEGAMI, "bright-white",
     "black", false, "bright-yellow", "blue", "cyan", false, false},
    {"clean", &PALETTE_NAME_CLEAN, &PALETTE_DESC_CLEAN, "default", "default",
     false, "white", "default", "default", false, false},
    {"adwaita", &PALETTE_NAME_ADWAITA, &PALETTE_DESC_ADWAITA, "blue", "default",
     false, "blue", "bright-white", "white", true, false},
    {"80shacker", &PALETTE_NAME_80SHACKER, &PALETTE_DESC_80SHACKER,
     "bright-green", "default", true, "bright-green", "default", "default",
     true, false},
    {"plato", &PALETTE_NAME_PLATO, &PALETTE_DESC_PLATO, "yellow", "default",
     false, "yellow", "default", "default", false, false},
    {"atarist", &PALETTE_NAME_ATARIST, &PALETTE_DESC_ATARIST, "bright-white",
     "black", true, "bright-white", "black", "black", false, false},
    {"win95bsod", &PALETTE_NAME_WIN95BSOD, &PALETTE_DESC_WIN95BSOD,
     "bright-white", "blue", true, "bright-white", "blue", "cyan", true, false},
    {"chn-hanzi", &PALETTE_NAME_CHN_HANZI, &PALETTE_DESC_CHN_HANZI,
     "bright-cyan", "black", true, "white", "black", "cyan", true, false},
    {"usa-flag", &PALETTE_NAME_USA_FLAG, &PALETTE_DESC_USA_FLAG, "bright-white",
     "blue", true, "red", "blue", "bright-white", true, false},
    {"jpn-flag", &PALETTE_NAME_JPN_FLAG, &PALETTE_DESC_JPN_FLAG, "bright-white",
     "black", false, "red", "black", "black", true, false},
    {"chn-flag", &PALETTE_NAME_CHN_FLAG, &PALETTE_DESC_CHN_FLAG,
     "bright-yellow", "red", true, "white", "red", "white", true, false},
    {"rus-flag", &PALETTE_NAME_RUS_FLAG, &PALETTE_DESC_RUS_FLAG, "bright-white",
     "blue", true, "red", "blue", "bright-white", true, false},
    {"de-flag", &PALETTE_NAME_DE_FLAG, &PALETTE_DESC_DE_FLAG, "bright-black",
     "black", true, "yellow", "black", "red", true, false},
    {"holy-light", &PALETTE_NAME_HOLY_LIGHT, &PALETTE_DESC_HOLY_LIGHT,
     "bright-white", "blue", false, "blue", "black", "yellow", true, false},
    {"islam", &PALETTE_NAME_ISLAM, &PALETTE_DESC_ISLAM, "bright-white", "green",
     false, "green", "black", "bright-white", true, false},
    {"dharma-ochre", &PALETTE_NAME_DHARMA_OCHRE, &PALETTE_DESC_DHARMA_OCHRE,
     "yellow", "black", true, "red", "black", "yellow", true, false},
    {"yin-yang", &PALETTE_NAME_YIN_YANG, &PALETTE_DESC_YIN_YANG, "white",
     "black", false, "green", "black", "white", false, false},
    {"soviet-cold", &PALETTE_NAME_SOVIET_COLD, &PALETTE_DESC_SOVIET_COLD,
     "white", "blue", false, "white", "blue", "blue", true, false},
    {"soviet-cold-256", &PALETTE_NAME_SOVIET_COLD_256,
     &PALETTE_DESC_SOVIET_COLD_256, "xterm:231", "xterm-bg:19", false,
     "xterm:231", "xterm-bg:19", "xterm-bg:19", true, true},
    {"hi-tel", &PALETTE_NAME_HI_TEL, &PALETTE_DESC_HI_TEL, "bright-white",
     "blue", true, "bright-white", "blue", "magenta", true, false},
    {"blackpink", &PALETTE_NAME_BLACKPINK, &PALETTE_DESC_BLACKPINK,
     "bright-white", "blue", false, "bright-white", "black", "magenta", false,
     false},
    {"amiga-cli", &PALETTE_NAME_AMIGA_CLI, &PALETTE_DESC_AMIGA_CLI, "cyan",
     "blue", true, "cyan", "blue", "blue", true, false},
    {"jpn-pc98", &PALETTE_NAME_JPN_PC98, &PALETTE_DESC_JPN_PC98, "yellow",
     "black", false, "red", "black", "yellow", false, false},
    {"deep-blue", &PALETTE_NAME_DEEP_BLUE, &PALETTE_DESC_DEEP_BLUE, "white",
     "blue", true, "cyan", "blue", "white", true, false},
    {"korea", &PALETTE_NAME_KOREA, &PALETTE_DESC_KOREA, "bright-blue", "blue",
     true, "bright-white", "blue", "red", true, false},
    {"neo-seoul", &PALETTE_NAME_NEO_SEOUL, &PALETTE_DESC_NEO_SEOUL,
     "bright-magenta", "black", true, "bright-cyan", "black", "cyan", true,
     false},
    {"incheon-industrial", &PALETTE_NAME_INCHEON_INDUSTRIAL,
     &PALETTE_DESC_INCHEON_INDUSTRIAL, "bright-yellow", "black", true,
     "bright-yellow", "black", "bright-red", true, false},
    {"gyeonggi-modern", &PALETTE_NAME_GYEONGGI_MODERN,
     &PALETTE_DESC_GYEONGGI_MODERN, "bright-white", "black", false,
     "bright-yellow", "black", "bright-cyan", false, false},
    {"korean-palace", &PALETTE_NAME_KOREAN_PALACE, &PALETTE_DESC_KOREAN_PALACE,
     "bright-yellow", "black", true, "red", "black", "green", false, false},
    {"gyeongsangbukdo", &PALETTE_NAME_GYEONGSANGBUKDO,
     &PALETTE_DESC_GYEONGSANGBUKDO, "bright-yellow", "black", false,
     "bright-green", "black", "bright-white", false, false},
    {"daegu-summer", &PALETTE_NAME_DAEGU_SUMMER, &PALETTE_DESC_DAEGU_SUMMER,
     "bright-red", "black", true, "bright-yellow", "black", "yellow", true,
     false},
    {"gyeongju-heritage", &PALETTE_NAME_GYEONGJU_HERITAGE,
     &PALETTE_DESC_GYEONGJU_HERITAGE, "bright-white", "black", false,
     "bright-yellow", "black", "blue", false, false},
    {"kangwon-winter", &PALETTE_NAME_KANGWON_WINTER,
     &PALETTE_DESC_KANGWON_WINTER, "bright-white", "blue", true, "bright-cyan",
     "blue", "white", true, false},
    {"ulsan-steel", &PALETTE_NAME_ULSAN_STEEL, &PALETTE_DESC_ULSAN_STEEL,
     "bright-red", "black", true, "bright-yellow", "black", "red", true, false},
    {"jeolla-seaside", &PALETTE_NAME_JEOLLA_SEASIDE,
     &PALETTE_DESC_JEOLLA_SEASIDE, "bright-cyan", "black", false, "cyan",
     "black", "bright-blue", true, false},
    {"gwangju-biennale", &PALETTE_NAME_GWANGJU_BIENNALE,
     &PALETTE_DESC_GWANGJU_BIENNALE, "bright-magenta", "black", true,
     "bright-blue", "black", "magenta", true, false},
    {"jeonju-hanok", &PALETTE_NAME_JEONJU_HANOK, &PALETTE_DESC_JEONJU_HANOK,
     "bright-yellow", "black", false, "yellow", "black", "bright-white", false,
     false},
    {"daejeon-tech", &PALETTE_NAME_DAEJEON_TECH, &PALETTE_DESC_DAEJEON_TECH,
     "white", "black", true, "white", "black", "bright-green", true, false},
    {"sejong-night", &PALETTE_NAME_SEJONG_NIGHT, &PALETTE_DESC_SEJONG_NIGHT,
     "bright-white", "blue", true, "bright-cyan", "blue", "white", true, false},
    {"cheongju-intellect", &PALETTE_NAME_CHEONGJU_INTELLECT,
     &PALETTE_DESC_CHEONGJU_INTELLECT, "bright-cyan", "black", false,
     "bright-white", "black", "cyan", false, false},
    {"chungcheong-field", &PALETTE_NAME_CHUNGCHEONG_FIELD,
     &PALETTE_DESC_CHUNGCHEONG_FIELD, "yellow", "black", false, "green",
     "black", "yellow", false, false},
    {"jeju-rock", &PALETTE_NAME_JEJU_ROCK, &PALETTE_DESC_JEJU_ROCK,
     "bright-green", "black", false, "bright-cyan", "black", "green", false,
     false},
    {"gyeongsangnamdo", &PALETTE_NAME_GYEONGSANGNAMDO,
     &PALETTE_DESC_GYEONGSANGNAMDO, "bright-blue", "black", true,
     "bright-yellow", "black", "bright-cyan", true, false},
    {"busan-harbor", &PALETTE_NAME_BUSAN_HARBOR, &PALETTE_DESC_BUSAN_HARBOR,
     "bright-blue", "black", true, "cyan", "black", "bright-blue", true, false},
    {"han", &PALETTE_NAME_HAN, &PALETTE_DESC_HAN, "bright-cyan", "blue", false,
     "white", "blue", "bright-white", false, false},
    {"jeong", &PALETTE_NAME_JEONG, &PALETTE_DESC_JEONG, "bright-red", "black",
     true, "black", "black", "bright-yellow", true, false},
    {"heung", &PALETTE_NAME_HEUNG, &PALETTE_DESC_HEUNG, "bright-magenta",
     "black", true, "bright-yellow", "black", "magenta", true, false},
    {"nunchi", &PALETTE_NAME_NUNCHI, &PALETTE_DESC_NUNCHI, "white", "black",
     false, "bright-cyan", "black", "cyan", false, false},
    {"pcbang-night", &PALETTE_NAME_PCBANG_NIGHT, &PALETTE_DESC_PCBANG_NIGHT,
     "bright-cyan", "black", true, "bright-red", "black", "bright-blue", true,
     false},
    {"alcohol", &PALETTE_NAME_ALCOHOL, &PALETTE_DESC_ALCOHOL, "bright-green",
     "black", true, "bright-magenta", "black", "green", true, false},
    {"korean-hardcore", &PALETTE_NAME_KOREAN_HARDCORE,
     &PALETTE_DESC_KOREAN_HARDCORE, "bright-red", "black", true, "bright-blue",
     "black", "bright-red", true, false},
    {"korean-nationalists", &PALETTE_NAME_KOREAN_NATIONALISTS,
     &PALETTE_DESC_KOREAN_NATIONALISTS, "bright-green", "black", true,
     "bright-blue", "black", "bright-cyan", true, false},
    {"medieval-korea", &PALETTE_NAME_MEDIEVAL_KOREA,
     &PALETTE_DESC_MEDIEVAL_KOREA, "bright-cyan", "black", false,
     "bright-yellow", "black", "cyan", false, false},
    {"stoneage-korea", &PALETTE_NAME_STONEAGE_KOREA,
     &PALETTE_DESC_STONEAGE_KOREA, "bright-white", "black", false,
     "bright-yellow", "black", "white", false, false},
    {"flame-and-blood", &PALETTE_NAME_FLAME_AND_BLOOD,
     &PALETTE_DESC_FLAME_AND_BLOOD, "bright-blue", "black", true,
     "bright-yellow", "black", "red", true, false},
    {"korean-war", &PALETTE_NAME_KOREAN_WAR, &PALETTE_DESC_KOREAN_WAR,
     "bright-white", "black", false, "bright-red", "black", "white", false,
     false},
    {"independence-spirit", &PALETTE_NAME_INDEPENDENCE_SPIRIT,
     &PALETTE_DESC_INDEPENDENCE_SPIRIT, "bright-red", "black", true, "blue",
     "black", "bright-yellow", true, false},
    {"usa-flag-256", &PALETTE_NAME_USA_FLAG_256, &PALETTE_DESC_USA_FLAG_256,
     "xterm:231", "xterm-bg:20", true, "xterm:196", "xterm-bg:20",
     "xterm-bg:231", true, true},
    {"jpn-flag-256", &PALETTE_NAME_JPN_FLAG_256, &PALETTE_DESC_JPN_FLAG_256,
     "xterm:231", "xterm-bg:16", false, "xterm:196", "xterm-bg:16",
     "xterm-bg:231", true, true},
    {"chn-flag-256", &PALETTE_NAME_CHN_FLAG_256, &PALETTE_DESC_CHN_FLAG_256,
     "xterm:229", "xterm-bg:196", true, "xterm:15", "xterm-bg:196",
     "xterm-bg:15", true, true},
    {"rus-flag-256", &PALETTE_NAME_RUS_FLAG_256, &PALETTE_DESC_RUS_FLAG_256,
     "xterm:231", "xterm-bg:20", true, "xterm:196", "xterm-bg:20",
     "xterm-bg:231", true, true},
    {"de-flag-256", &PALETTE_NAME_DE_FLAG_256, &PALETTE_DESC_DE_FLAG_256,
     "xterm:238", "xterm-bg:16", true, "xterm:226", "xterm-bg:16",
     "xterm-bg:196", true, true},
    {"holy-light-256", &PALETTE_NAME_HOLY_LIGHT_256,
     &PALETTE_DESC_HOLY_LIGHT_256, "xterm:231", "xterm-bg:20", false,
     "xterm:20", "xterm-bg:16", "xterm-bg:226", true, true},
    {"islam-256", &PALETTE_NAME_ISLAM_256, &PALETTE_DESC_ISLAM_256, "xterm:231",
     "xterm-bg:34", false, "xterm:34", "xterm-bg:16", "xterm-bg:231", true,
     true},
    {"dharma-ochre-256", &PALETTE_NAME_DHARMA_OCHRE_256,
     &PALETTE_DESC_DHARMA_OCHRE_256, "xterm:226", "xterm-bg:16", true,
     "xterm:196", "xterm-bg:16", "xterm-bg:226", true, true},
    {"yin-yang-256", &PALETTE_NAME_YIN_YANG_256, &PALETTE_DESC_YIN_YANG_256,
     "xterm:15", "xterm-bg:16", false, "xterm:34", "xterm-bg:16", "xterm-bg:15",
     false, true},
    {"soviet-cold-256", &PALETTE_NAME_SOVIET_COLD_256,
     &PALETTE_DESC_SOVIET_COLD_256, "xterm:15", "xterm-bg:19", false,
     "xterm:15", "xterm-bg:19", "xterm-bg:19", true, true},
    {"hi-tel-256", &PALETTE_NAME_HI_TEL_256, &PALETTE_DESC_HI_TEL_256,
     "xterm:231", "xterm-bg:20", true, "xterm:231", "xterm-bg:20",
     "xterm-bg:165", true, true},
    {"amiga-cli-256", &PALETTE_NAME_AMIGA_CLI_256, &PALETTE_DESC_AMIGA_CLI_256,
     "xterm:51", "xterm-bg:20", true, "xterm:51", "xterm-bg:20", "xterm-bg:231",
     true, true},
    {"jpn-pc98-256", &PALETTE_NAME_JPN_PC98_256, &PALETTE_DESC_JPN_PC98_256,
     "xterm:226", "xterm-bg:16", false, "xterm:196", "xterm-bg:16",
     "xterm-bg:226", false, true},
    {"deep-blue-256", &PALETTE_NAME_DEEP_BLUE_256, &PALETTE_DESC_DEEP_BLUE_256,
     "xterm:15", "xterm-bg:20", true, "xterm:51", "xterm-bg:20", "xterm-bg:15",
     true, true},
    {"korea-256", &PALETTE_NAME_KOREA_256, &PALETTE_DESC_KOREA_256, "xterm:81",
     "xterm-bg:20", true, "xterm:231", "xterm-bg:20", "xterm-bg:196", true,
     true},
    {"neo-seoul-256", &PALETTE_NAME_NEO_SEOUL_256, &PALETTE_DESC_NEO_SEOUL_256,
     "xterm:207", "xterm-bg:16", true, "xterm:117", "xterm-bg:16",
     "xterm-bg:229", true, true},
    {"incheon-industrial-256", &PALETTE_NAME_INCHEON_INDUSTRIAL_256,
     &PALETTE_DESC_INCHEON_INDUSTRIAL_256, "xterm:229", "xterm-bg:16", true,
     "xterm:229", "xterm-bg:16", "xterm-bg:220", true, true},
    {"gyeonggi-modern-256", &PALETTE_NAME_GYEONGGI_MODERN_256,
     &PALETTE_DESC_GYEONGGI_MODERN_256, "xterm:231", "xterm-bg:16", false,
     "xterm:229", "xterm-bg:16", "xterm-bg:72", false, true},
    {"korean-palace-256", &PALETTE_NAME_KOREAN_PALACE_256,
     &PALETTE_DESC_KOREAN_PALACE_256, "xterm:229", "xterm-bg:16", true,
     "xterm:131", "xterm-bg:16", "xterm-bg:65", false, true},
    {"gyeongsangbukdo-256", &PALETTE_NAME_GYEONGSANGBUKDO_256,
     &PALETTE_DESC_GYEONGSANGBUKDO_256, "xterm:229", "xterm-bg:16", false,
     "xterm:118", "xterm-bg:16", "xterm-bg:231", false, true},
    {"daegu-summer-256", &PALETTE_NAME_DAEGU_SUMMER_256,
     &PALETTE_DESC_DAEGU_SUMMER_256, "xterm:203", "xterm-bg:16", true,
     "xterm:229", "xterm-bg:16", "xterm-bg:220", true, true},
    {"gyeongju-heritage-256", &PALETTE_NAME_GYEONGJU_HERITAGE_256,
     &PALETTE_DESC_GYEONGJU_HERITAGE_256, "xterm:231", "xterm-bg:16", false,
     "xterm:229", "xterm-bg:16", "xterm-bg:20", false, true},
    {"kangwon-winter-256", &PALETTE_NAME_KANGWON_WINTER_256,
     &PALETTE_DESC_KANGWON_WINTER_256, "xterm:231", "xterm-bg:20", true,
     "xterm:123", "xterm-bg:20", "xterm-bg:15", true, true},
    {"ulsan-steel-256", &PALETTE_NAME_ULSAN_STEEL_256,
     &PALETTE_DESC_ULSAN_STEEL_256, "xterm:203", "xterm-bg:16", true,
     "xterm:229", "xterm-bg:16", "xterm-bg:208", true, true},
    {"jeolla-seaside-256", &PALETTE_NAME_JEOLLA_SEASIDE_256,
     &PALETTE_DESC_JEOLLA_SEASIDE_256, "xterm:95", "xterm-bg:16", false,
     "xterm:51", "xterm-bg:16", "xterm-bg:95", true, true},
    {"gwangju-biennale-256", &PALETTE_NAME_GWANGJU_BIENNALE_256,
     &PALETTE_DESC_GWANGJU_BIENNALE_256, "xterm:207", "xterm-bg:16", true,
     "xterm:81", "xterm-bg:16", "xterm-bg:165", true, true},
    {"jeonju-hanok-256", &PALETTE_NAME_JEONJU_HANOK_256,
     &PALETTE_DESC_JEONJU_HANOK_256, "xterm:231", "xterm-bg:16", false,
     "xterm:226", "xterm-bg:16", "xterm-bg:1", false, true},
    {"daejeon-tech-256", &PALETTE_NAME_DAEJEON_TECH_256,
     &PALETTE_DESC_DAEJEON_TECH_256, "xterm:15", "xterm-bg:16", true,
     "xterm:15", "xterm-bg:16", "xterm-bg:64", true, true},
    {"sejong-night-256", &PALETTE_NAME_SEJONG_NIGHT_256,
     &PALETTE_DESC_SEJONG_NIGHT_256, "xterm:231", "xterm-bg:20", true,
     "xterm:90", "xterm-bg:20", "xterm-bg:15", true, true},
    {"cheongju-intellect-256", &PALETTE_NAME_CHEONGJU_INTELLECT_256,
     &PALETTE_DESC_CHEONGJU_INTELLECT_256, "xterm:123", "xterm-bg:16", false,
     "xterm:178", "xterm-bg:16", "xterm-bg:123", false, true},
    {"chungcheong-field-256", &PALETTE_NAME_CHUNGCHEONG_FIELD_256,
     &PALETTE_DESC_CHUNGCHEONG_FIELD_256, "xterm:226", "xterm-bg:16", false,
     "xterm:34", "xterm-bg:16", "xterm-bg:226", false, true},
    {"jeju-rock-256", &PALETTE_NAME_JEJU_ROCK_256, &PALETTE_DESC_JEJU_ROCK_256,
     "xterm:118", "xterm-bg:16", false, "xterm:123", "xterm-bg:16",
     "xterm-bg:34", false, true},
    {"gyeongsangnamdo-256", &PALETTE_NAME_GYEONGSANGNAMDO_256,
     &PALETTE_DESC_GYEONGSANGNAMDO_256, "xterm:81", "xterm-bg:16", true,
     "xterm:229", "xterm-bg:16", "xterm-bg:123", true, true},
    {"busan-harbor-256", &PALETTE_NAME_BUSAN_HARBOR_256,
     &PALETTE_DESC_BUSAN_HARBOR_256, "xterm:81", "xterm-bg:16", true,
     "xterm:255", "xterm-bg:16", "xterm-bg:81", true, true},
    {"han-256", &PALETTE_NAME_HAN_256, &PALETTE_DESC_HAN_256, "xterm:123",
     "xterm-bg:75", false, "xterm:246", "xterm-bg:20", "xterm-bg:231", false,
     true},
    {"jeong-256", &PALETTE_NAME_JEONG_256, &PALETTE_DESC_JEONG_256, "xterm:203",
     "xterm-bg:16", true, "xterm:203", "xterm-bg:16", "xterm-bg:220", true,
     true},
    {"heung-256", &PALETTE_NAME_HEUNG_256, &PALETTE_DESC_HEUNG_256, "xterm:207",
     "xterm-bg:16", true, "xterm:229", "xterm-bg:16", "xterm-bg:165", true,
     true},
    {"nunchi-256", &PALETTE_NAME_NUNCHI_256, &PALETTE_DESC_NUNCHI_256,
     "xterm:15", "xterm-bg:16", false, "xterm:105", "xterm-bg:16",
     "xterm-bg:15", false, true},
    {"pcbang-night-256", &PALETTE_NAME_PCBANG_NIGHT_256,
     &PALETTE_DESC_PCBANG_NIGHT_256, "xterm:123", "xterm-bg:16", true,
     "xterm:203", "xterm-bg:16", "xterm-bg:220", true, true},
    {"alcohol-256", &PALETTE_NAME_ALCOHOL_256, &PALETTE_DESC_ALCOHOL_256,
     "xterm:118", "xterm-bg:16", true, "xterm:207", "xterm-bg:16",
     "xterm-bg:34", true, true},
    {"korean-hardcore-256", &PALETTE_NAME_KOREAN_HARDCORE_256,
     &PALETTE_DESC_KOREAN_HARDCORE_256, "xterm:203", "xterm-bg:16", true,
     "xterm:81", "xterm-bg:16", "xterm-bg:220", true, true},
    {"korean-nationalists-256", &PALETTE_NAME_KOREAN_NATIONALISTS_256,
     &PALETTE_DESC_KOREAN_NATIONALISTS_256, "xterm:118", "xterm-bg:16", true,
     "xterm:81", "xterm-bg:16", "xterm-bg:123", true, true},
    {"medieval-korea-256", &PALETTE_NAME_MEDIEVAL_KOREA_256,
     &PALETTE_DESC_MEDIEVAL_KOREA_256, "xterm:123", "xterm-bg:16", false,
     "xterm:229", "xterm-bg:16", "xterm-bg:123", false, true},
    {"stoneage-korea-256", &PALETTE_NAME_STONEAGE_KOREA_256,
     &PALETTE_DESC_STONEAGE_KOREA_256, "xterm:231", "xterm-bg:16", false,
     "xterm:235", "xterm-bg:229", "xterm-bg:229", false, true},
    {"flame-and-blood-256", &PALETTE_NAME_FLAME_AND_BLOOD_256,
     &PALETTE_DESC_FLAME_AND_BLOOD_256, "xterm:81", "xterm-bg:16", true,
     "xterm:229", "xterm-bg:16", "xterm-bg:196", true, true},
    {"korean-war-256", &PALETTE_NAME_KOREAN_WAR_256,
     &PALETTE_DESC_KOREAN_WAR_256, "xterm:231", "xterm-bg:16", false,
     "xterm:203", "xterm-bg:16", "xterm-bg:15", false, true},
    {"independence-spirit-256", &PALETTE_NAME_INDEPENDENCE_SPIRIT_256,
     &PALETTE_DESC_INDEPENDENCE_SPIRIT_256, "xterm:203", "xterm-bg:16", true,
     "xterm:20", "xterm-bg:16", "xterm-bg:229", true, true},
    {"monokai", &PALETTE_NAME_MONOKAI, &PALETTE_DESC_MONOKAI, "xterm:118",
     "xterm-bg:239", false, "xterm:255", "xterm-bg:235", "xterm-bg:239", false,
     true},
    {"french-rococo-256", &PALETTE_NAME_FRENCH_ROCOCO_256,
     &PALETTE_DESC_FRENCH_ROCOCO_256, "xterm:218", "xterm-bg:229", false,
     "xterm:222", "xterm-bg:231", "xterm-bg:147", false, true},
    {"polish-catholic-256", &PALETTE_NAME_POLISH_CATHOLIC_256,
     &PALETTE_DESC_POLISH_CATHOLIC_256, "xterm:226", "xterm-bg:21", true,
     "xterm:231", "xterm-bg:16", "xterm-bg:196", true, true},
    {"german-industry-256", &PALETTE_NAME_GERMAN_INDUSTRY_256,
     &PALETTE_DESC_GERMAN_INDUSTRY_256, "xterm:208", "xterm-bg:238", true,
     "xterm:252", "xterm-bg:235", "xterm-bg:220", true, true},
    {"leipzig-black-256", &PALETTE_NAME_LEIPZIG_BLACK_256,
     &PALETTE_DESC_LEIPZIG_BLACK_256, "xterm:250", "xterm-bg:232", false,
     "xterm:255", "xterm-bg:16", "xterm-bg:240", false, true},
    {"chopin-mazurka-256", &PALETTE_NAME_CHOPIN_MAZURKA_256,
     &PALETTE_DESC_CHOPIN_MAZURKA_256, "xterm:111", "xterm-bg:16", false,
     "xterm:253", "xterm-bg:233", "xterm-bg:176", false, true},
};

typedef int (*accept_channel_fn_t)(ssh_message, ssh_channel);

#if defined(__GNUC__)
extern int ssh_message_channel_request_open_reply_accept_channel(
    ssh_message message, ssh_channel channel) __attribute__((weak));
#endif

static void resolve_accept_channel_once(void);
static accept_channel_fn_t g_accept_channel_fn = nullptr;
static pthread_once_t g_accept_channel_once = PTHREAD_ONCE_INIT;

static accept_channel_fn_t resolve_accept_channel_fn(void)
{
    pthread_once(&g_accept_channel_once, resolve_accept_channel_once);
    return g_accept_channel_fn;
}

static void resolve_accept_channel_once(void)
{
#if defined(__GNUC__)
    if (ssh_message_channel_request_open_reply_accept_channel != nullptr) {
        g_accept_channel_fn =
            ssh_message_channel_request_open_reply_accept_channel;
        return;
    }
#endif

    static const char *kSymbol =
        "ssh_message_channel_request_open_reply_accept_channel";

#if defined(RTLD_DEFAULT)
    g_accept_channel_fn = (accept_channel_fn_t)dlsym(RTLD_DEFAULT, kSymbol);
    if (g_accept_channel_fn != nullptr) {
        return;
    }
#endif

    const char *candidates[] = {"libssh.so.4", "libssh.so", "libssh.dylib"};
    for (size_t idx = 0; idx < sizeof(candidates) / sizeof(candidates[0]);
         ++idx) {
        const char *name = candidates[idx];
        void *handle = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
        if (handle == nullptr) {
            handle = dlopen(name, RTLD_LAZY);
        }
        if (handle == nullptr) {
            continue;
        }

        g_accept_channel_fn = (accept_channel_fn_t)dlsym(handle, kSymbol);
        if (g_accept_channel_fn != nullptr) {
            return;
        }
    }
}

void trim_whitespace_inplace(char *text);
static const char *lookup_color_code(const color_entry_t *entries,
                                     size_t entry_count, const char *name);
static bool parse_bool_token(const char *token, bool *value);
static bool session_transport_active(const session_ctx_t *ctx);
static void session_transport_request_close(session_ctx_t *ctx);
void session_channel_write(session_ctx_t *ctx, const void *data,
                                  size_t length);
static bool session_channel_write_cp437(session_ctx_t *ctx, const char *data,
                                        size_t length);
static bool session_channel_write_utf16(session_ctx_t *ctx, const char *data,
                                        size_t length);
static bool session_channel_write_utf16_segment(session_ctx_t *ctx,
                                                const char *data,
                                                size_t length);
static size_t session_utf8_decode_codepoint(const unsigned char *data,
                                            size_t length, uint32_t *codepoint);
static bool session_utf8_to_utf16le(const char *input, size_t length,
                                    unsigned char *output, size_t capacity,
                                    size_t *produced);
static bool session_channel_write_all(session_ctx_t *ctx, const void *data,
                                      size_t length);
static bool session_output_lock(session_ctx_t *ctx);
static void session_output_unlock(session_ctx_t *ctx);
static bool session_channel_wait_writable(session_ctx_t *ctx, int timeout_ms);
static void session_channel_log_write_failure(session_ctx_t *ctx,
                                              const char *reason);
static void session_channel_flush(session_ctx_t *ctx);
static void session_output_buffer_flush(session_ctx_t *ctx);
static int session_transport_read(session_ctx_t *ctx, void *buffer,
                                  size_t length, int timeout_ms);
static bool session_transport_is_open(const session_ctx_t *ctx);
static bool session_transport_is_eof(const session_ctx_t *ctx);
static void session_apply_background_fill(session_ctx_t *ctx);
static void session_write_rendered_line(session_ctx_t *ctx,
                                        const char *render_source);
static void session_send_caption_line(session_ctx_t *ctx, const char *message);
static void session_render_caption_with_offset(session_ctx_t *ctx,
                                               const char *message,
                                               size_t move_up);
static void session_send_line(session_ctx_t *ctx, const char *message);
static void session_send_plain_line(session_ctx_t *ctx, const char *message);
static void session_send_multiline_message(session_ctx_t *ctx,
                                           const char *message);
static void session_send_system_line(session_ctx_t *ctx, const char *message);
void session_send_raw_text(session_ctx_t *ctx, const char *text);

static void session_render_banner(session_ctx_t *ctx);
static const char *session_editor_terminator(const session_ctx_t *ctx);
static bool session_editor_matches_terminator(const session_ctx_t *ctx,
                                              const char *line);
static size_t session_editor_body_capacity(const session_ctx_t *ctx);
static size_t session_editor_max_lines(const session_ctx_t *ctx);
static void session_format_separator_line(session_ctx_t *ctx, const char *label,
                                          char *out, size_t length);
static void session_render_separator(session_ctx_t *ctx, const char *label);
static void session_clear_screen(session_ctx_t *ctx);
static void session_bbs_prepare_canvas(session_ctx_t *ctx);
static void session_bbs_render_editor(session_ctx_t *ctx, const char *status);
static void session_bbs_recalculate_line_count(session_ctx_t *ctx);
static bool session_bbs_get_line_range(const session_ctx_t *ctx,
                                       size_t line_index, size_t *start,
                                       size_t *length);
static void session_bbs_copy_line(const session_ctx_t *ctx, size_t line_index,
                                  char *buffer, size_t length);
static bool session_bbs_append_line(session_ctx_t *ctx, const char *line,
                                    char *status, size_t status_length);
static bool session_bbs_replace_line(session_ctx_t *ctx, size_t line_index,
                                     const char *line, char *status,
                                     size_t status_length);
static void session_bbs_move_cursor(session_ctx_t *ctx, int direction);
static bool session_bbs_is_admin_only_tag(const char *tag);
static void session_bbs_buffer_breaking_notice(session_ctx_t *ctx,
                                               const char *message);
static bool session_bbs_should_defer_breaking(session_ctx_t *ctx,
                                              const char *message);
static void session_render_prompt(session_ctx_t *ctx, bool include_separator);
static void session_refresh_input_line(session_ctx_t *ctx);
static void session_set_input_text(session_ctx_t *ctx, const char *text);
static void session_local_echo_char(session_ctx_t *ctx, char ch);
static void session_local_backspace(session_ctx_t *ctx);
static void session_clear_input(session_ctx_t *ctx);
static void session_clear_input_without_prompt(session_ctx_t *ctx);
static bool session_try_command_completion(session_ctx_t *ctx);
static bool session_consume_escape_sequence(session_ctx_t *ctx, char ch);
static session_ctx_t *session_create(void);
static void session_destroy(session_ctx_t *ctx);
static void session_cleanup(session_ctx_t *ctx);
static void *session_thread(void *arg);
static void host_telnet_listener_stop(host_t *host);
static void session_refresh_output_encoding(session_ctx_t *ctx);
static bool session_detect_retro_client(session_ctx_t *ctx);
static void session_telnet_request_terminal_type(session_ctx_t *ctx);
static void session_telnet_capture_startup_metadata(session_ctx_t *ctx);
static void session_history_record(session_ctx_t *ctx, const char *line);
static void session_history_navigate(session_ctx_t *ctx, int direction);
void session_scrollback_reset_position(session_ctx_t *ctx);
void session_scrollback_navigate(session_ctx_t *ctx, int direction,
                                 size_t step);
static bool session_try_localized_command_forward(session_ctx_t *ctx,
                                                  const char *line);
static const char *session_display_name(const session_ctx_t *ctx);
static void chat_history_entry_prepare_user(chat_history_entry_t *entry,
                                            const session_ctx_t *from,
                                            const char *message,
                                            bool preserve_whitespace);
static bool host_history_record_user(host_t *host, const session_ctx_t *from,
                                     const char *message,
                                     bool preserve_whitespace,
                                     chat_history_entry_t *stored_entry);
static bool host_history_commit_entry(host_t *host, chat_history_entry_t *entry,
                                      chat_history_entry_t *stored_entry);
static void host_notify_external_clients(host_t *host,
                                         const chat_history_entry_t *entry);
static bool host_history_append_locked(host_t *host,
                                       const chat_history_entry_t *entry);
static bool host_history_reserve_locked(host_t *host, size_t min_capacity);
static size_t host_history_total(host_t *host);
static size_t host_history_copy_range(host_t *host, size_t start_index,
                                      chat_history_entry_t *buffer,
                                      size_t capacity);
static bool host_history_find_entry_by_id(host_t *host, uint64_t message_id,
                                          chat_history_entry_t *entry);
static size_t host_history_delete_range(host_t *host, uint64_t start_id,
                                        uint64_t end_id,
                                        uint64_t *first_removed,
                                        uint64_t *last_removed,
                                        size_t *replies_removed);
static void chat_room_broadcast_should_sink(chat_room_t *room);
static void chat_room_broadcast_entry(chat_room_t *room,
                                      const chat_history_entry_t *entry,
                                      const session_ctx_t *from);
static void chat_room_broadcast(chat_room_t *room, const char *message,
                                const session_ctx_t *from);
static void chat_room_broadcast_caption(chat_room_t *room, const char *message);
static bool host_history_apply_reaction(host_t *host, uint64_t message_id,
                                        size_t reaction_index,
                                        chat_history_entry_t *updated_entry);
static bool
chat_history_entry_build_reaction_summary(const chat_history_entry_t *entry,
                                          char *buffer, size_t length);
static void host_ban_resolve_path(host_t *host);
static void host_ban_state_save_locked(host_t *host);
static void host_ban_state_load(host_t *host);
static void host_reply_state_resolve_path(host_t *host);
static void host_reply_state_save_locked(host_t *host);
static void host_reply_state_load(host_t *host);
static bool host_replies_find_entry_by_id(host_t *host, uint64_t reply_id,
                                          chat_reply_entry_t *entry);
static bool host_replies_commit_entry(host_t *host, chat_reply_entry_t *entry,
                                      chat_reply_entry_t *stored_entry);
static void session_send_reply_tree(session_ctx_t *ctx,
                                    uint64_t parent_message_id,
                                    uint64_t parent_reply_id, size_t depth);

static const char *session_display_name(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return "";
    }

    if (ctx->user_data_loaded && ctx->user_data.preferred_nickname[0] != '\0') {
        return ctx->user_data.preferred_nickname;
    }

    if (ctx->user.name[0] != '\0') {
        return ctx->user.name;
    }

    return "";
}

// static void host_broadcast_reply(host_t *host, const chat_reply_entry_t *entry);
static void session_send_private_message_line(session_ctx_t *ctx,
                                              const session_ctx_t *color_source,
                                              const char *label,
                                              const char *message);
static session_ctx_t *chat_room_find_user(chat_room_t *room,
                                          const char *username);
static bool host_is_ip_banned(host_t *host, const char *ip);
static bool host_is_username_banned(host_t *host, const char *username);
static bool host_add_ban_entry(host_t *host, const char *username,
                               const char *ip);
static bool host_remove_ban_entry(host_t *host, const char *token);
static join_activity_entry_t *host_find_join_activity_locked(host_t *host,
                                                             const char *ip);
static join_activity_entry_t *host_ensure_join_activity_locked(host_t *host,
                                                               const char *ip);
static bool host_register_suspicious_activity(host_t *host,
                                              const char *username,
                                              const char *ip,
                                              size_t *attempts_out);
static bool session_is_private_ipv4(const unsigned char octets[4]);
bool session_is_lan_client(const char *ip);
static void session_assign_lan_privileges(session_ctx_t *ctx);
static void session_apply_granted_privileges(session_ctx_t *ctx);
static void session_apply_theme_defaults(session_ctx_t *ctx);
static void session_apply_system_theme_defaults(session_ctx_t *ctx);
static void session_force_dark_mode_foreground(session_ctx_t *ctx);
static void session_apply_saved_preferences(session_ctx_t *ctx);
static void session_dispatch_command(session_ctx_t *ctx, const char *line);
static void session_handle_exit(session_ctx_t *ctx);
static void session_force_disconnect(session_ctx_t *ctx, const char *reason);
static void session_handle_nick(session_ctx_t *ctx, const char *arguments);
static bool host_lookup_member_ip(host_t *host, const char *username, char *ip,
                                  size_t length);
static bool host_lookup_last_ip(host_t *host, const char *username, char *ip,
                                size_t length);
static bool session_should_hide_entry(session_ctx_t *ctx,
                                      const chat_history_entry_t *entry);
static bool session_blocklist_add(session_ctx_t *ctx, const char *ip,
                                  const char *username, bool ip_wide,
                                  bool *already_present);
static bool session_blocklist_remove(session_ctx_t *ctx, const char *token);
static void session_blocklist_show(session_ctx_t *ctx);
static void session_handle_reply(session_ctx_t *ctx, const char *arguments);
static void session_handle_block(session_ctx_t *ctx, const char *arguments);
static void session_handle_unblock(session_ctx_t *ctx, const char *arguments);
static void session_handle_pm(session_ctx_t *ctx, const char *arguments);
static void session_handle_motd(session_ctx_t *ctx);
static void session_handle_system_color(session_ctx_t *ctx,
                                        const char *arguments);
static void session_handle_palette(session_ctx_t *ctx, const char *arguments);
static void session_handle_translate(session_ctx_t *ctx, const char *arguments);
static void session_handle_translate_scope(session_ctx_t *ctx,
                                           const char *arguments);
static void session_handle_gemini(session_ctx_t *ctx, const char *arguments);
static void session_handle_captcha(session_ctx_t *ctx, const char *arguments);
static void session_handle_set_trans_lang(session_ctx_t *ctx,
                                          const char *arguments);
static void session_handle_set_target_lang(session_ctx_t *ctx,
                                           const char *arguments);
static void session_handle_chat_spacing(session_ctx_t *ctx,
                                        const char *arguments);
static void session_handle_mode(session_ctx_t *ctx, const char *arguments);
static void session_handle_history(session_ctx_t *ctx, const char *arguments);
static void session_handle_eliza(session_ctx_t *ctx, const char *arguments);
static void session_handle_status(session_ctx_t *ctx, const char *arguments);
static void session_handle_showstatus(session_ctx_t *ctx,
                                      const char *arguments);
static void session_handle_weather(session_ctx_t *ctx, const char *arguments);
static void session_handle_pardon(session_ctx_t *ctx, const char *arguments);
static void session_handle_ban_name(session_ctx_t *ctx, const char *arguments);
static void session_handle_ban_list(session_ctx_t *ctx, const char *arguments);
static bool host_add_operator_grant_locked(host_t *host, const char *ip);
static void host_apply_grant_to_ip(host_t *host, const char *ip);
static void host_state_save_locked(host_t *host);
static void session_handle_grant(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(
            ctx, "Only operators may grant operator privileges.");
        return;
    }

    if (arguments == nullptr) {
        session_send_system_line(ctx, "Usage: /grant <ip-address>");
        return;
    }

    char ip[SSH_CHATTER_IP_LEN];
    snprintf(ip, sizeof(ip), "%s", arguments);
    trim_whitespace_inplace(ip);
    if (ip[0] == '\0') {
        session_send_system_line(ctx, "Usage: /grant <ip-address>");
        return;
    }

    unsigned char buf[sizeof(struct in6_addr)];
    if (inet_pton(AF_INET, ip, buf) != 1 && inet_pton(AF_INET6, ip, buf) != 1) {
        session_send_system_line(ctx, "Provide a valid IPv4 or IPv6 address.");
        return;
    }

    pthread_mutex_lock(&ctx->owner->lock);
    bool added = host_add_operator_grant_locked(ctx->owner, ip);
    if (added) {
        host_state_save_locked(ctx->owner);
    }
    pthread_mutex_unlock(&ctx->owner->lock);

    if (!added) {
        session_send_system_line(ctx, "That IP address already has a grant.");
        return;
    }

    host_apply_grant_to_ip(ctx->owner, ip);

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(message, sizeof(message), "Operator privileges granted to %s.",
             ip);
    session_send_system_line(ctx, message);
}

static void session_handle_grant(session_ctx_t *ctx, const char *arguments);
static void session_handle_kick(session_ctx_t *ctx, const char *arguments);
static void session_handle_usercount(session_ctx_t *ctx);
static bool host_username_reserved(host_t *host, const char *username);
static void session_handle_search(session_ctx_t *ctx, const char *arguments);
static void session_handle_chat_lookup(session_ctx_t *ctx,
                                       const char *arguments);
static void session_handle_image(session_ctx_t *ctx, const char *arguments);
static void session_handle_video(session_ctx_t *ctx, const char *arguments);
static void session_handle_audio(session_ctx_t *ctx, const char *arguments);
static void session_handle_files(session_ctx_t *ctx, const char *arguments);
static void session_handle_reaction(session_ctx_t *ctx, size_t reaction_index,
                                    const char *arguments);
static void session_handle_gameopt(session_ctx_t *ctx, const char *arguments);
static void session_handle_othello_command(session_ctx_t *ctx,
                                           const char *arguments);
static void session_handle_mail(session_ctx_t *ctx, const char *arguments);
static void session_handle_profile_picture(session_ctx_t *ctx,
                                           const char *arguments);
static void session_handle_today(session_ctx_t *ctx);
static void session_handle_date(session_ctx_t *ctx, const char *arguments);
static void session_handle_os(session_ctx_t *ctx, const char *arguments);
static void session_handle_getos(session_ctx_t *ctx, const char *arguments);
static void session_handle_getaddr(session_ctx_t *ctx, const char *arguments);
static void session_handle_pair(session_ctx_t *ctx);
static void session_handle_connected(session_ctx_t *ctx);
static bool session_parse_birthday(const char *input, char *normalized,
                                   size_t length);
static void session_handle_birthday(session_ctx_t *ctx, const char *arguments);
static void session_handle_soulmate(session_ctx_t *ctx);
static void session_handle_setpw(session_ctx_t *ctx, const char *arguments);
static void session_handle_delpw(session_ctx_t *ctx, const char *arguments);
static void session_handle_revoke(session_ctx_t *ctx, const char *arguments);
static void session_handle_delete_message(session_ctx_t *ctx,
                                          const char *arguments);
static void session_normalize_newlines(char *text);
static bool timezone_sanitize_identifier(const char *input, char *output,
                                         size_t length);
static bool timezone_resolve_identifier(const char *input, char *resolved,
                                        size_t length);
static const palette_descriptor_t *palette_find_descriptor(const char *name);
static bool palette_apply_to_session(session_ctx_t *ctx,
                                     const palette_descriptor_t *descriptor);
static void session_translation_flush_ready(session_ctx_t *ctx);
static bool session_translation_queue_caption(session_ctx_t *ctx,
                                              const char *message,
                                              size_t placeholder_lines);
static void session_translation_reserve_placeholders(session_ctx_t *ctx,
                                                     size_t placeholder_lines);
static void session_translation_clear_queue(session_ctx_t *ctx);
static bool session_translation_worker_ensure(session_ctx_t *ctx);
static void session_translation_worker_shutdown(session_ctx_t *ctx);
static void *session_translation_worker(void *arg);

static bool session_translation_queue_private_message(session_ctx_t *ctx,
                                                      session_ctx_t *target,
                                                      const char *message);
static void session_translation_normalize_output(char *text);
static void host_handle_translation_quota_exhausted(host_t *host);
static void
session_handle_translation_quota_exhausted(session_ctx_t *ctx,
                                           const char *error_detail);
static bool session_argument_is_disable(const char *token);
static bool session_argument_is_enable(const char *token);
static void session_language_normalize(const char *input, char *normalized,
                                       size_t length);
static bool session_language_equals(const char *lhs, const char *rhs);

static const char *session_ui_language_code(session_ui_language_t language);
static const char *session_ui_language_name(session_ui_language_t language,
                                            session_ui_language_t locale);
static const session_ui_locale_t *
session_ui_get_locale(const session_ctx_t *ctx);
static const char *session_command_prefix(const session_ctx_t *ctx);
static int session_utf8_display_width(const char *text);
static void session_format_help_line(session_ctx_t *ctx,
                                     const session_help_entry_t *entry,
                                     const char *description, char *buffer,
                                     size_t length);
static bool session_fetch_weather_summary(const char *region, const char *city,
                                          char *summary, size_t summary_len);
static void session_handle_poll(session_ctx_t *ctx, const char *arguments);
static void session_handle_vote(session_ctx_t *ctx, size_t option_index);
static void session_handle_named_vote(session_ctx_t *ctx, size_t option_index,
                                      const char *label);
static void session_handle_elect_command(session_ctx_t *ctx,
                                         const char *arguments);
static void session_handle_vote_command(session_ctx_t *ctx,
                                        const char *arguments,
                                        bool allow_multiple);
static void session_handle_alpha_centauri_landers(session_ctx_t *ctx);
static void session_format_help_entries_to_buffer(
    session_ctx_t *ctx, const session_help_entry_t *entries, size_t count,
    char *buffer, size_t buffer_length);
static void session_print_help(session_ctx_t *ctx);
static void session_handle_set_ui_lang(session_ctx_t *ctx,
                                       const char *arguments);
static bool session_line_is_exit_command(const char *line);
static void session_handle_username_conflict_input(session_ctx_t *ctx,
                                                   const char *line);
static const char *session_consume_token(const char *input, char *token,
                                         size_t length);
static bool session_user_data_available(session_ctx_t *ctx);
static bool session_user_data_load(session_ctx_t *ctx);
static bool session_user_data_commit(session_ctx_t *ctx);
static bool host_user_data_send_mail(host_t *host, const char *recipient,
                                     const char *recipient_ip,
                                     const char *sender, const char *message,
                                     char *error, size_t error_length);
bool host_user_data_load_existing(host_t *host, const char *username,
                                  const char *ip, user_data_record_t *record,
                                  bool create_if_missing);
static void host_user_data_bootstrap(host_t *host);
static bool session_parse_color_arguments(char *working, char **tokens,
                                          size_t max_tokens,
                                          size_t *token_count);
static size_t session_utf8_prev_char_len(const char *buffer, size_t length);
static int session_utf8_char_width(const char *bytes, size_t length);
static bool host_history_record_system(host_t *host, const char *message,
                                       chat_history_entry_t *stored_entry);
static void host_history_cleanup_expired(host_t *host);
static void session_send_history_entry(session_ctx_t *ctx,
                                       const chat_history_entry_t *entry);
static void session_deliver_outgoing_message(session_ctx_t *ctx,
                                             const char *message,
                                             bool clear_prompt_text);
static void
chat_room_broadcast_reaction_update(host_t *host,
                                    const chat_history_entry_t *entry);
static user_preference_t *
host_find_preference_locked(host_t *host, const char *username, const char *ip);
static user_preference_t *host_ensure_preference_locked(host_t *host,
                                                        const char *username,
                                                        const char *ip);
static void host_store_user_theme(host_t *host, session_ctx_t *ctx);
static size_t host_prepare_join_delay(host_t *host,
                                      struct timespec *wait_duration);
static host_join_attempt_result_t
host_register_join_attempt(host_t *host, const char *username, const char *ip);
static bool session_run_captcha(session_ctx_t *ctx);
static bool session_is_captcha_exempt(const session_ctx_t *ctx);
static void host_store_system_theme(host_t *host, const session_ctx_t *ctx);
static void host_store_user_os(host_t *host, const session_ctx_t *ctx);
static void host_store_birthday(host_t *host, const session_ctx_t *ctx,
                                const char *birthday);
static void host_store_chat_spacing(host_t *host, const session_ctx_t *ctx);
static void host_store_translation_preferences(host_t *host,
                                               const session_ctx_t *ctx);

static bool host_ip_has_grant_locked(host_t *host, const char *ip);
static bool host_ip_has_grant(host_t *host, const char *ip);
static bool host_remove_operator_grant_locked(host_t *host, const char *ip);
static void host_refresh_motd_locked(host_t *host);
static void host_refresh_motd(host_t *host);
static void host_build_birthday_notice_locked(host_t *host, char *line,
                                              size_t length);
static bool host_is_leap_year(int year);
static void host_revoke_grant_from_ip(host_t *host, const char *ip);
static bool host_history_normalize_entry(host_t *host,
                                         chat_history_entry_t *entry);
static const char *chat_attachment_type_label(chat_attachment_type_t type);
static void host_state_resolve_path(host_t *host);
static void host_state_load(host_t *host);
static void host_state_save_locked(host_t *host);
static void host_ui_language_state_resolve_path(host_t *host);
static void host_ui_language_state_load(host_t *host);
static void host_ui_language_state_save_locked(host_t *host);
static void host_eliza_state_resolve_path(host_t *host);
static void host_eliza_state_load(host_t *host);
static void host_eliza_state_save_locked(host_t *host);
static void host_eliza_memory_resolve_path(host_t *host);
static void host_eliza_memory_load(host_t *host);
static void host_eliza_memory_save_locked(host_t *host);
static void host_eliza_memory_store(host_t *host, const char *prompt,
                                    const char *reply);
static size_t host_eliza_memory_collect_context(host_t *host,
                                                const char *prompt,
                                                char *context,
                                                size_t context_length);
static void host_eliza_history_normalize_line(char *text);
static size_t host_eliza_history_collect_context(host_t *host, char *context,
                                                 size_t context_length);
static void host_eliza_prepare_preview(const char *source, char *dest,
                                       size_t dest_length);
static size_t host_eliza_bbs_collect_context(host_t *host, char *context,
                                             size_t context_length);
static size_t host_eliza_memory_collect_tokens(const char *prompt,
                                               char tokens[][32],
                                               size_t max_tokens);
static void host_bbs_resolve_path(host_t *host);
static void host_bbs_state_load(host_t *host);
static void host_bbs_state_save_locked(host_t *host);
static void host_bbs_start_watchdog(host_t *host);
static void *host_bbs_watchdog_thread(void *arg);
static void host_bbs_watchdog_scan(host_t *host);
static void host_security_configure(host_t *host);
static bool host_ensure_private_data_path(host_t *host, const char *path,
                                          bool create_directories);
static void host_security_compact_whitespace(char *text);
static bool host_security_execute_clamav_backend(host_t *host, char *notice,
                                                 size_t notice_length);
static void *host_security_clamav_backend(void *arg);
static void host_security_start_clamav_backend(host_t *host);
static void host_security_disable_filter(host_t *host, const char *reason);
static void host_security_disable_clamav(host_t *host, const char *reason);
static host_security_scan_result_t
host_security_scan_payload(host_t *host, const char *category,
                           const char *payload, size_t length, char *diagnostic,
                           size_t diagnostic_length);
static void host_security_process_blocked(host_t *host, const char *category,
                                          const char *diagnostic,
                                          const char *username, const char *ip,
                                          session_ctx_t *session,
                                          bool post_send, const char *content);
static void host_security_process_error(host_t *host, const char *category,
                                        const char *diagnostic,
                                        const char *username, const char *ip,
                                        session_ctx_t *session, bool post_send);
static bool host_moderation_init(host_t *host);
static void host_moderation_shutdown(host_t *host);
static void host_moderation_backoff(unsigned int attempts);
static bool host_moderation_spawn_worker(host_t *host);
static void host_moderation_close_worker(host_t *host);
static bool host_moderation_recover_worker(host_t *host,
                                           const char *diagnostic);
static bool host_moderation_queue_chat(session_ctx_t *ctx, const char *message,
                                       size_t length);
static void *host_moderation_thread(void *arg);
static bool host_moderation_write_all(int fd, const void *buffer,
                                      size_t length);
static bool host_moderation_read_all(int fd, void *buffer, size_t length);
static void host_moderation_worker_loop(int request_fd, int response_fd);
static void host_moderation_handle_failure(host_t *host,
                                           host_moderation_task_t *task,
                                           const char *diagnostic);
static void
host_moderation_apply_result(host_t *host, host_moderation_task_t *task,
                             const host_moderation_ipc_response_t *response,
                             const char *message);
static void host_moderation_flush_pending(host_t *host, const char *diagnostic);
static double host_elapsed_seconds(const struct timespec *start,
                                   const struct timespec *end);
static bool host_eliza_enable(host_t *host);
static bool host_eliza_disable(host_t *host);
static void host_eliza_announce_join(host_t *host);
static void host_eliza_announce_depart(host_t *host);
static void host_eliza_say(host_t *host, const char *message);
static void host_eliza_handle_private_message(session_ctx_t *ctx,
                                              const char *message);
static void host_eliza_prepare_private_reply(const char *message, char *reply,
                                             size_t reply_length);
static bool host_eliza_content_is_severe(const char *text);
static bool host_eliza_worker_init(host_t *host);
static void host_eliza_worker_shutdown(host_t *host);
static bool host_eliza_worker_enqueue(host_t *host,
                                      host_eliza_intervene_task_t *task);
static void *host_eliza_worker_thread(void *arg);
static bool host_eliza_intervene(session_ctx_t *ctx, const char *content,
                                 const char *reason, bool from_filter);
static void host_eliza_intervene_execute(session_ctx_t *ctx, const char *reason,
                                         bool from_filter);
static host_security_scan_result_t
session_security_check_text(session_ctx_t *ctx, const char *category,
                            const char *content, size_t length, bool post_send);
static void host_vote_resolve_path(host_t *host);
static void host_vote_state_load(host_t *host);
static void host_vote_state_save_locked(host_t *host);
static bool host_try_load_motd_from_path(host_t *host, const char *path);
static bool username_contains(const char *username, const char *needle);
static void
host_apply_palette_descriptor(host_t *host,
                              const palette_descriptor_t *descriptor);
static bool host_lookup_user_os(host_t *host, const char *username,
                                char *buffer, size_t length);
static void session_send_poll_summary(session_ctx_t *ctx);
static void session_send_poll_summary_generic(session_ctx_t *ctx,
                                              const poll_state_t *poll,
                                              const char *label);
static void session_list_named_polls(session_ctx_t *ctx);
static void session_handle_bbs(session_ctx_t *ctx, const char *arguments);
static void poll_state_reset(poll_state_t *poll);
static void named_poll_reset(named_poll_state_t *poll);
static named_poll_state_t *host_find_named_poll_locked(host_t *host,
                                                       const char *label);
static named_poll_state_t *host_ensure_named_poll_locked(host_t *host,
                                                         const char *label);
static void host_recount_named_polls_locked(host_t *host);
static bool poll_label_is_valid(const char *label);
static void session_bbs_show_dashboard(session_ctx_t *ctx);
static void session_bbs_list(session_ctx_t *ctx);
static void session_bbs_list_topic(session_ctx_t *ctx, const char *topic);
static void session_bbs_read(session_ctx_t *ctx, uint64_t id);
static void session_bbs_begin_post(session_ctx_t *ctx, const char *arguments);
static void session_bbs_begin_edit(session_ctx_t *ctx, uint64_t id);
static void session_bbs_capture_body_text(session_ctx_t *ctx, const char *text);
static void session_bbs_capture_body_line(session_ctx_t *ctx, const char *line);
static bool session_bbs_capture_continue(const session_ctx_t *ctx);
static void session_bbs_add_comment(session_ctx_t *ctx, const char *arguments);
static void session_bbs_regen_post(session_ctx_t *ctx, uint64_t id);
static void session_bbs_delete(session_ctx_t *ctx, uint64_t id);
static void session_bbs_reset_pending_post(session_ctx_t *ctx);
static bbs_post_t *host_find_bbs_post_locked(host_t *host, uint64_t id);
static bbs_post_t *host_allocate_bbs_post_locked(host_t *host);
static void host_clear_bbs_post_locked(host_t *host, bbs_post_t *post);

bool host_user_data_load_existing(host_t *host, const char *username,
                                  const char *ip, user_data_record_t *record,
                                  bool create_if_missing);

static bool session_bbs_scroll(session_ctx_t *ctx, int direction, size_t step);
static bool session_bbs_refresh_view(session_ctx_t *ctx);
static void session_handle_rss(session_ctx_t *ctx, const char *arguments);
static void session_rss_list(session_ctx_t *ctx);
static void session_rss_read(session_ctx_t *ctx, const char *tag);
static void session_rss_begin(session_ctx_t *ctx, const char *tag,
                              const rss_session_item_t *items, size_t count);
static void session_rss_show_current(session_ctx_t *ctx);
static bool session_rss_move(session_ctx_t *ctx, int delta);
static void session_rss_exit(session_ctx_t *ctx, const char *reason);
static void session_rss_clear(session_ctx_t *ctx);
static bool session_parse_command(const char *line, const char *command,
                                  const char **arguments);
static bool
session_parse_localized_command(session_ctx_t *ctx,
                                const session_command_alias_t *alias,
                                const char *line, const char **arguments);
static void rss_strip_html(char *text);
static void rss_decode_entities(char *text);
static void rss_trim_whitespace(char *text);
static bool rss_tag_is_valid(const char *tag);
static rss_feed_t *host_find_rss_feed_locked(host_t *host, const char *tag);
static void host_clear_rss_feed(rss_feed_t *feed);
static void host_rss_recount_locked(host_t *host);
static bool host_rss_add_feed(host_t *host, const char *url, const char *tag,
                              char *error, size_t error_length);
static bool host_rss_remove_feed(host_t *host, const char *tag, char *error,
                                 size_t error_length);
static void host_rss_resolve_path(host_t *host);
static void host_rss_state_load(host_t *host);
static void host_rss_state_save_locked(host_t *host);
static size_t host_rss_write_callback(void *contents, size_t size, size_t nmemb,
                                      void *userp);
static bool host_rss_download(const char *url, char **payload, size_t *length);
static bool host_rss_extract_tag(const char *block, const char *tag, char *out,
                                 size_t out_len);
static bool host_rss_extract_atom_link(const char *block, char *out,
                                       size_t out_len);
static size_t host_rss_parse_items(const char *payload,
                                   rss_session_item_t *items, size_t max_items);
static bool host_rss_fetch_items(const rss_feed_t *feed,
                                 rss_session_item_t *items, size_t max_items,
                                 size_t *out_count);
static void host_rss_start_backend(host_t *host);
static void *host_rss_backend(void *arg);
static bool host_rss_should_broadcast_breaking(const rss_session_item_t *item);
static bool host_asciiart_cooldown_active(host_t *host, const char *ip,
                                          const struct timespec *now,
                                          long *remaining_seconds);
static void host_asciiart_register_post(host_t *host, const char *ip,
                                        const struct timespec *when);
static bool session_asciiart_cooldown_active(session_ctx_t *ctx,
                                             struct timespec *now,
                                             long *remaining_seconds);
static void session_asciiart_reset(session_ctx_t *ctx);
static void session_asciiart_begin(session_ctx_t *ctx,
                                   session_asciiart_target_t target);
static void session_asciiart_import_from_editor(session_ctx_t *ctx);
static void session_asciiart_capture_text(session_ctx_t *ctx, const char *text);
static void session_asciiart_capture_line(session_ctx_t *ctx, const char *line);
static void session_asciiart_commit(session_ctx_t *ctx);
static void session_asciiart_cancel(session_ctx_t *ctx, const char *reason);
typedef void (*session_text_line_consumer_t)(session_ctx_t *, const char *);
typedef bool (*session_text_continue_predicate_t)(const session_ctx_t *);
static void session_capture_multiline_text(
    session_ctx_t *ctx, const char *text, session_text_line_consumer_t consumer,
    session_text_continue_predicate_t should_continue);
static bool session_asciiart_capture_continue(const session_ctx_t *ctx);
static void session_handle_game(session_ctx_t *ctx, const char *arguments);
static void session_game_suspend(session_ctx_t *ctx, const char *reason);
static int session_channel_read_poll(session_ctx_t *ctx, char *buffer,
                                     size_t length, int timeout_ms);
static void session_game_seed_rng(session_ctx_t *ctx);
static uint32_t session_game_random(session_ctx_t *ctx);
static int session_game_random_range(session_ctx_t *ctx, int max);
static void session_game_start_tetris(session_ctx_t *ctx);
static void session_game_tetris_reset(tetris_game_state_t *state);
static void
session_game_tetris_apply_round_settings(tetris_game_state_t *state);
static void session_game_tetris_handle_round_progress(session_ctx_t *ctx);
static void session_game_tetris_fill_bag(session_ctx_t *ctx);
static int session_game_tetris_take_piece(session_ctx_t *ctx);
static bool session_game_tetris_spawn_piece(session_ctx_t *ctx);
static bool session_game_tetris_cell_occupied(int piece, int rotation, int row,
                                              int column);
static bool session_game_tetris_position_valid(const tetris_game_state_t *state,
                                               int piece, int rotation, int row,
                                               int column);
static bool session_game_tetris_move(session_ctx_t *ctx, int drow, int dcol);
static bool session_game_tetris_soft_drop(session_ctx_t *ctx);
static bool session_game_tetris_rotate(session_ctx_t *ctx);
static bool session_game_tetris_apply_gravity(session_ctx_t *ctx,
                                              unsigned ticks);
static bool session_game_tetris_update_timer(session_ctx_t *ctx,
                                             bool accelerate);
static bool session_game_tetris_process_timeout(session_ctx_t *ctx);
static bool session_game_tetris_process_action(session_ctx_t *ctx, int action);
static bool session_game_tetris_process_raw_input(session_ctx_t *ctx, char ch);
static void session_game_tetris_lock_piece(session_ctx_t *ctx);
static void session_game_tetris_clear_lines(session_ctx_t *ctx,
                                            unsigned *cleared);
static void session_game_tetris_render(session_ctx_t *ctx);
static void session_game_tetris_handle_line(session_ctx_t *ctx,
                                            const char *line);
static void session_game_start_liargame(session_ctx_t *ctx);
static void session_game_liar_present_round(session_ctx_t *ctx);
static void session_game_liar_handle_line(session_ctx_t *ctx, const char *line);
static bool session_game_gonu_handle_input(session_ctx_t *ctx,
                                           const char *input);
static void session_game_start_alpha(session_ctx_t *ctx);
static void session_game_alpha_reset(session_ctx_t *ctx);
static void session_game_alpha_prepare_navigation(session_ctx_t *ctx);
static void session_game_alpha_reroll_navigation(session_ctx_t *ctx);
static void
session_game_alpha_add_gravity_source(alpha_centauri_game_state_t *state, int x,
                                      int y, double mu, int influence_radius,
                                      char symbol, const char *name);
static void session_game_start_othello(session_ctx_t *ctx);
static void session_game_othello_handle_line(session_ctx_t *ctx,
                                             const char *line);
static void session_game_othello_prepare_next_turn(session_ctx_t *ctx);
static void session_game_othello_finish(session_ctx_t *ctx, const char *reason);
static void session_game_othello_render(session_ctx_t *ctx);
static void session_game_othello_count_scores(const othello_game_state_t *state,
                                              unsigned *red, unsigned *green);
static void session_game_othello_reset_state(othello_game_state_t *state);
static void session_game_othello_handle_ai_turn(session_ctx_t *ctx);
static void session_game_alpha_configure_gravity(session_ctx_t *ctx);
static void
session_game_alpha_apply_gravity(alpha_centauri_game_state_t *state);
static const char *const kAlphaStarCatalog[] = {
    "Midway Star",   "Binary Torch", "Turnover Sun",
    "Arrival Flare", "Relay Star",   "Shepherd Star",
};

static const char *const kAlphaPlanetCatalog[] = {
    "Departure World", "Drift Planet", "Relay Outpost",
    "Approach World",  "Proxima b",    "Immigrants' Harbor",
};

static const char *const kAlphaDebrisCatalog[] = {
    "Comet Trail", "Asteroid Swarm", "Ice Shard", "Dust Ribbon", "Sail Wreck",
};

#define ALPHA_STAR_CATALOG_COUNT                                               \
    (sizeof(kAlphaStarCatalog) / sizeof(kAlphaStarCatalog[0]))
#define ALPHA_PLANET_CATALOG_COUNT                                             \
    (sizeof(kAlphaPlanetCatalog) / sizeof(kAlphaPlanetCatalog[0]))
#define ALPHA_DEBRIS_CATALOG_COUNT                                             \
    (sizeof(kAlphaDebrisCatalog) / sizeof(kAlphaDebrisCatalog[0]))

static bool
session_game_alpha_position_occupied(const alpha_centauri_game_state_t *state,
                                     int x, int y)
{
    if (state == nullptr) {
        return true;
    }
    if (state->nav_x == x && state->nav_y == y) {
        return true;
    }
    if (state->nav_target_x == x && state->nav_target_y == y) {
        return true;
    }
    for (unsigned idx = 0U; idx < state->gravity_source_count; ++idx) {
        const alpha_gravity_source_t *existing = &state->gravity_sources[idx];
        if (existing->x == x && existing->y == y) {
            return true;
        }
    }
    if (state->stage == 4U) {
        if (!state->eva_ready) {
            for (unsigned idx = 0U; idx < state->waypoint_count; ++idx) {
                const alpha_waypoint_t *waypoint = &state->waypoints[idx];
                if (waypoint->x == x && waypoint->y == y) {
                    return true;
                }
            }
        }
        if (state->final_waypoint.symbol != '\0' &&
            state->final_waypoint.x == x && state->final_waypoint.y == y) {
            return true;
        }
    }
    return false;
}

static void session_game_alpha_place_random_source(
    session_ctx_t *ctx, alpha_centauri_game_state_t *state, int margin,
    double mu, int radius, char symbol, const char *name)
{
    if (ctx == nullptr || state == nullptr) {
        return;
    }

    int attempts = 0;
    int min_margin = margin >= 0 ? margin : 0;
    int usable_width = ALPHA_NAV_WIDTH - (min_margin * 2);
    int usable_height = ALPHA_NAV_HEIGHT - (min_margin * 2);
    if (usable_width <= 0) {
        usable_width = ALPHA_NAV_WIDTH;
        min_margin = 0;
    }
    if (usable_height <= 0) {
        usable_height = ALPHA_NAV_HEIGHT;
        min_margin = 0;
    }

    while (attempts < 128) {
        int x = min_margin + session_game_random_range(ctx, usable_width);
        int y = min_margin + session_game_random_range(ctx, usable_height);
        if (!session_game_alpha_position_occupied(state, x, y)) {
            session_game_alpha_add_gravity_source(state, x, y, mu, radius,
                                                  symbol, name);
            return;
        }
        ++attempts;
    }

    int fallback_x = min_margin < ALPHA_NAV_WIDTH ? min_margin : 0;
    int fallback_y = min_margin < ALPHA_NAV_HEIGHT ? min_margin : 0;
    session_game_alpha_add_gravity_source(state, fallback_x, fallback_y, mu,
                                          radius, symbol, name);
}

static double session_game_alpha_random_double(session_ctx_t *ctx,
                                               double min_value,
                                               double max_value)
{
    if (ctx == nullptr) {
        return min_value;
    }
    if (max_value <= min_value) {
        return min_value;
    }
    double fraction = (double)session_game_random(ctx) / (double)UINT32_MAX;
    if (fraction < 0.0) {
        fraction = 0.0;
    } else if (fraction > 1.0) {
        fraction = 1.0;
    }
    return min_value + (max_value - min_value) * fraction;
}

static int session_game_alpha_random_with_margin(session_ctx_t *ctx, int extent,
                                                 int margin)
{
    if (extent <= 0) {
        return 0;
    }
    int safe_margin = margin;
    if (safe_margin < 0) {
        safe_margin = 0;
    }
    int usable = extent - (safe_margin * 2);
    if (usable <= 0) {
        usable = extent;
        safe_margin = 0;
    }
    return safe_margin + session_game_random_range(ctx, usable);
}
static void session_game_alpha_sync_from_save(session_ctx_t *ctx);
static void session_game_alpha_sync_to_save(session_ctx_t *ctx);
static void session_game_alpha_present_stage(session_ctx_t *ctx);
static void session_game_alpha_handle_line(session_ctx_t *ctx,
                                           const char *line);
static void session_game_alpha_log_completion(session_ctx_t *ctx);
static void session_game_alpha_render_navigation(session_ctx_t *ctx);
static void session_game_alpha_refresh_navigation(session_ctx_t *ctx);
static void session_game_alpha_plan_waypoints(session_ctx_t *ctx);
static void session_game_alpha_present_waypoints(session_ctx_t *ctx);
static void session_game_alpha_complete_waypoint(session_ctx_t *ctx);
static bool session_game_alpha_handle_arrow(session_ctx_t *ctx, int dx, int dy);
static bool session_game_alpha_attempt_completion(session_ctx_t *ctx);
static void session_game_alpha_execute_ignite(session_ctx_t *ctx);
static void session_game_alpha_execute_trim(session_ctx_t *ctx);
static void session_game_alpha_execute_flip(session_ctx_t *ctx);
static void session_game_alpha_execute_retro(session_ctx_t *ctx);
static void session_game_alpha_execute_eva(session_ctx_t *ctx);
static void session_game_alpha_manual_lock(session_ctx_t *ctx);
static void session_game_alpha_manual_save(session_ctx_t *ctx);
static void host_update_last_captcha_prompt(host_t *host,
                                            const captcha_prompt_t *prompt,
                                            const captcha_language_t *order,
                                            size_t count);

typedef struct liar_prompt {
    const char *statements[3];
    unsigned liar_index;
} liar_prompt_t;

static const liar_prompt_t LIAR_PROMPTS[] = {
    {{"I have contributed code to an open source project.",
      "I once replaced an entire server rack solo.",
      "I prefer mechanical keyboards with clicky switches."},
     1U},
    {{"I have memorized pi to 200 digits.",
      "I used to write BASIC games in middle school.",
      "I cannot solve a Rubik's Cube."},
     0U},
    {{"I drink my coffee without sugar.",
      "I debug using `printf` more than any other tool.",
      "I have never broken a build."},
     2U},
    {{"I run Linux on my primary laptop.",
      "I have camped overnight for a console launch.",
      "I have attended a demoparty."},
     1U},
    {{"I know how to solder surface-mount components.",
      "I have written an emulator in C.", "I have a pet snake named Segfault."},
     2U},
    {{"I play at least one rhythm game competitively.",
      "I once deployed to production from my phone.",
      "I have built a keyboard from scratch."},
     1U},
};

static const char *const TETROMINO_SHAPES[7][4] = {
    {
        "...."
        "####"
        "...."
        "....",
        "..#."
        "..#."
        "..#."
        "..#.",
        "...."
        "####"
        "...."
        "....",
        "..#."
        "..#."
        "..#."
        "..#.",
    },
    {
        "#..."
        "###."
        "...."
        "....",
        ".##."
        ".#.."
        ".#.."
        "....",
        "...."
        "###."
        "..#."
        "....",
        ".#.."
        ".#.."
        "##.."
        "....",
    },
    {
        "..#."
        "###."
        "...."
        "....",
        ".#.."
        ".#.."
        ".##."
        "....",
        "...."
        "###."
        "#..."
        "....",
        "##.."
        ".#.."
        ".#.."
        "....",
    },
    {
        ".##."
        ".##."
        "...."
        "....",
        ".##."
        ".##."
        "...."
        "....",
        ".##."
        ".##."
        "...."
        "....",
        ".##."
        ".##."
        "...."
        "....",
    },
    {
        ".##."
        "##.."
        "...."
        "....",
        ".#.."
        ".##."
        "..#."
        "....",
        ".##."
        "##.."
        "...."
        "....",
        ".#.."
        ".##."
        "..#."
        "....",
    },
    {
        ".#.."
        "###."
        "...."
        "....",
        ".#.."
        ".##."
        ".#.."
        "....",
        "...."
        "###."
        ".#.."
        "....",
        ".#.."
        "##.."
        ".#.."
        "....",
    },
    {
        "##.."
        ".##."
        "...."
        "....",
        "..#."
        ".##."
        ".#.."
        "....",
        "##.."
        ".##."
        "...."
        "....",
        "..#."
        ".##."
        ".#.."
        "....",
    },
};

static const char TETROMINO_DISPLAY_CHARS[7] = {'I', 'J', 'L', 'O',
                                                'S', 'T', 'Z'};

static const uint32_t HOST_STATE_MAGIC = 0x53484354U; /* 'SHCT' */
static const uint32_t HOST_STATE_VERSION = 16U;
static const uint32_t ELIZA_STATE_MAGIC = 0x454c5354U; /* 'ELST' */
static const uint32_t ELIZA_STATE_VERSION = 1U;

typedef struct eliza_memory_header {
    uint32_t magic;
    uint32_t version;
    uint32_t entry_count;
    uint32_t reserved;
    uint64_t next_id;
} eliza_memory_header_t;

typedef struct eliza_memory_entry_serialized {
    uint64_t id;
    int64_t stored_at;
    char prompt[SSH_CHATTER_MESSAGE_LIMIT];
    char reply[SSH_CHATTER_MESSAGE_LIMIT];
} eliza_memory_entry_serialized_t;

typedef struct eliza_state_record {
    uint32_t magic;
    uint32_t version;
    uint8_t enabled;
    uint8_t reserved[7];
} eliza_state_record_t;

typedef struct host_state_header_v1 {
    uint32_t magic;
    uint32_t version;
    uint32_t history_count;
    uint32_t preference_count;
} host_state_header_v1_t;

typedef struct host_state_header {
    host_state_header_v1_t base;
    uint32_t legacy_sound_count;
    uint32_t grant_count;
    uint64_t next_message_id;
    uint8_t captcha_enabled;
    uint8_t geo_language_enabled;
    uint8_t reserved[6];
} host_state_header_t;

typedef struct host_state_history_entry {
    uint8_t is_user_message;
    uint8_t user_is_bold;
    char username[SSH_CHATTER_USERNAME_LEN];
    char message[SSH_CHATTER_MESSAGE_LIMIT];
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char raw_username[SSH_CHATTER_USERNAME_LEN];
    uint64_t message_id;
    int64_t created_at;
    uint8_t attachment_type;
    uint8_t reserved[7];
    char attachment_target[SSH_CHATTER_ATTACHMENT_TARGET_LEN];
    char attachment_caption[SSH_CHATTER_ATTACHMENT_CAPTION_LEN];
    char user_color_code[SSH_CHATTER_COLOR_CODE_LEN];
    char user_highlight_code[SSH_CHATTER_COLOR_CODE_LEN];
    uint32_t reaction_counts[SSH_CHATTER_REACTION_KIND_COUNT];
} host_state_history_entry_t;

static void host_state_assign_color_codes(chat_history_entry_t *entry,
                                          const char *color_code,
                                          const char *highlight_code)
{
    if (entry == nullptr) {
        return;
    }

    entry->user_color_code = nullptr;
    entry->user_highlight_code = nullptr;

    if (color_code != nullptr && color_code[0] != '\0') {
        size_t length = strnlen(color_code, SSH_CHATTER_COLOR_CODE_LEN - 1U);
        char *copy = GC_MALLOC(length + 1U);
        if (copy != nullptr) {
            memcpy(copy, color_code, length);
            copy[length] = '\0';
            entry->user_color_code = copy;
        }
    }

    if (highlight_code != nullptr && highlight_code[0] != '\0') {
        size_t length =
            strnlen(highlight_code, SSH_CHATTER_COLOR_CODE_LEN - 1U);
        char *copy = GC_MALLOC(length + 1U);
        if (copy != nullptr) {
            memcpy(copy, highlight_code, length);
            copy[length] = '\0';
            entry->user_highlight_code = copy;
        }
    }
}

typedef struct host_state_preference_entry_v3 {
    uint8_t has_user_theme;
    uint8_t has_system_theme;
    uint8_t user_is_bold;
    uint8_t system_is_bold;
    char username[SSH_CHATTER_USERNAME_LEN];
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_fg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_bg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
} host_state_preference_entry_v3_t;

typedef struct host_state_preference_entry_v4 {
    uint8_t has_user_theme;
    uint8_t has_system_theme;
    uint8_t user_is_bold;
    uint8_t system_is_bold;
    char username[SSH_CHATTER_USERNAME_LEN];
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_fg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_bg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char os_name[SSH_CHATTER_OS_NAME_LEN];
    int32_t daily_year;
    int32_t daily_yday;
    char daily_function[64];
    uint64_t last_poll_id;
    int32_t last_poll_choice;
} host_state_preference_entry_v4_t;

typedef struct host_state_preference_entry_v5 {
    uint8_t has_user_theme;
    uint8_t has_system_theme;
    uint8_t user_is_bold;
    uint8_t system_is_bold;
    char username[SSH_CHATTER_USERNAME_LEN];
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_fg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_bg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char os_name[SSH_CHATTER_OS_NAME_LEN];
    int32_t daily_year;
    int32_t daily_yday;
    char daily_function[64];
    uint64_t last_poll_id;
    int32_t last_poll_choice;
    uint8_t has_birthday;
    uint8_t reserved[3];
    char birthday[16];
} host_state_preference_entry_v5_t;

typedef struct host_state_preference_entry_v6 {
    uint8_t has_user_theme;
    uint8_t has_system_theme;
    uint8_t user_is_bold;
    uint8_t system_is_bold;
    char username[SSH_CHATTER_USERNAME_LEN];
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_fg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_bg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char os_name[SSH_CHATTER_OS_NAME_LEN];
    int32_t daily_year;
    int32_t daily_yday;
    char daily_function[64];
    uint64_t last_poll_id;
    int32_t last_poll_choice;
    uint8_t has_birthday;
    uint8_t translation_caption_spacing;
    uint8_t translation_enabled;
    uint8_t output_translation_enabled;
    uint8_t input_translation_enabled;
    uint8_t reserved[3];
    char birthday[16];
    char output_translation_language[SSH_CHATTER_LANG_NAME_LEN];
    char input_translation_language[SSH_CHATTER_LANG_NAME_LEN];
} host_state_preference_entry_v6_t;

typedef struct host_state_preference_entry_v7 {
    uint8_t has_user_theme;
    uint8_t has_system_theme;
    uint8_t user_is_bold;
    uint8_t system_is_bold;
    char username[SSH_CHATTER_USERNAME_LEN];
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_fg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_bg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char os_name[SSH_CHATTER_OS_NAME_LEN];
    int32_t daily_year;
    int32_t daily_yday;
    char daily_function[64];
    uint64_t last_poll_id;
    int32_t last_poll_choice;
    uint8_t has_birthday;
    uint8_t translation_caption_spacing;
    uint8_t translation_enabled;
    uint8_t output_translation_enabled;
    uint8_t input_translation_enabled;
    uint8_t translation_master_explicit;
    uint8_t reserved[2];
    char birthday[16];
    char output_translation_language[SSH_CHATTER_LANG_NAME_LEN];
    char input_translation_language[SSH_CHATTER_LANG_NAME_LEN];
} host_state_preference_entry_v7_t;

typedef struct host_state_preference_entry_v8 {
    uint8_t has_user_theme;
    uint8_t has_system_theme;
    uint8_t user_is_bold;
    uint8_t system_is_bold;
    char username[SSH_CHATTER_USERNAME_LEN];
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_fg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_bg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char os_name[SSH_CHATTER_OS_NAME_LEN];
    int32_t daily_year;
    int32_t daily_yday;
    char daily_function[64];
    uint64_t last_poll_id;
    int32_t last_poll_choice;
    uint8_t has_birthday;
    uint8_t translation_caption_spacing;
    uint8_t translation_enabled;
    uint8_t output_translation_enabled;
    uint8_t input_translation_enabled;
    uint8_t translation_master_explicit;
    uint8_t reserved[2];
    char birthday[16];
    char output_translation_language[SSH_CHATTER_LANG_NAME_LEN];
    char input_translation_language[SSH_CHATTER_LANG_NAME_LEN];
    char ui_language[SSH_CHATTER_LANG_NAME_LEN];
} host_state_preference_entry_v8_t;

typedef struct host_state_preference_entry_v9 {
    uint8_t has_user_theme;
    uint8_t has_system_theme;
    uint8_t user_is_bold;
    uint8_t system_is_bold;
    char username[SSH_CHATTER_USERNAME_LEN];
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_fg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_bg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char os_name[SSH_CHATTER_OS_NAME_LEN];
    int32_t daily_year;
    int32_t daily_yday;
    char daily_function[64];
    uint64_t last_poll_id;
    int32_t last_poll_choice;
    uint8_t has_birthday;
    uint8_t translation_caption_spacing;
    uint8_t translation_enabled;
    uint8_t output_translation_enabled;
    uint8_t input_translation_enabled;
    uint8_t translation_master_explicit;
    uint8_t reserved[2];
    char birthday[16];
    char output_translation_language[SSH_CHATTER_LANG_NAME_LEN];
    char input_translation_language[SSH_CHATTER_LANG_NAME_LEN];
    char ui_language[SSH_CHATTER_LANG_NAME_LEN];
    uint8_t breaking_alerts_enabled;
    uint8_t reserved2[7];
} host_state_preference_entry_v9_t;

typedef struct host_state_preference_entry {
    uint8_t has_user_theme;
    uint8_t has_system_theme;
    uint8_t user_is_bold;
    uint8_t system_is_bold;
    char username[SSH_CHATTER_USERNAME_LEN];
    char ip[SSH_CHATTER_IP_LEN];
    char user_color_code[SSH_CHATTER_COLOR_CODE_LEN];
    char user_highlight_code[SSH_CHATTER_COLOR_CODE_LEN];
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_fg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_bg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char os_name[SSH_CHATTER_OS_NAME_LEN];
    int32_t daily_year;
    int32_t daily_yday;
    char daily_function[64];
    uint64_t last_poll_id;
    int32_t last_poll_choice;
    uint8_t has_birthday;
    uint8_t translation_caption_spacing;
    uint8_t translation_enabled;
    uint8_t output_translation_enabled;
    uint8_t input_translation_enabled;
    uint8_t translation_master_explicit;
    uint8_t reserved[2];
    char birthday[16];
    char output_translation_language[SSH_CHATTER_LANG_NAME_LEN];
    char input_translation_language[SSH_CHATTER_LANG_NAME_LEN];
    char ui_language[SSH_CHATTER_LANG_NAME_LEN];
    uint8_t breaking_alerts_enabled;
    char provider_label[SSH_CHATTER_PROVIDER_LABEL_LEN];
    uint8_t reserved2[7];
} host_state_preference_entry_t;

static const uint32_t BAN_STATE_MAGIC = 0x5348424eU; /* 'SHBN' */
static const uint32_t BAN_STATE_VERSION = 1U;

typedef struct ban_state_header {
    uint32_t magic;
    uint32_t version;
    uint32_t entry_count;
} ban_state_header_t;

typedef struct ban_state_entry {
    char username[SSH_CHATTER_USERNAME_LEN];
    char ip[SSH_CHATTER_IP_LEN];
} ban_state_entry_t;

static const uint32_t REPLY_STATE_MAGIC = 0x53485250U; /* 'SHRP' */
static const uint32_t REPLY_STATE_VERSION = 1U;

typedef struct reply_state_header {
    uint32_t magic;
    uint32_t version;
    uint32_t entry_count;
    uint64_t next_reply_id;
} reply_state_header_t;

typedef struct reply_state_entry {
    uint64_t reply_id;
    uint64_t parent_message_id;
    uint64_t parent_reply_id;
    int64_t created_at;
    char username[SSH_CHATTER_USERNAME_LEN];
    char message[SSH_CHATTER_MESSAGE_LIMIT];
} reply_state_entry_t;

typedef struct host_state_grant_entry {
    char ip[SSH_CHATTER_IP_LEN];
} host_state_grant_entry_t;

static const uint32_t BBS_STATE_MAGIC = 0x42425331U; /* 'BBS1' */
static const uint32_t BBS_STATE_VERSION = 4U;

#define SSH_CHATTER_BBS_TITLE_LEN_V1 96U
#define SSH_CHATTER_BBS_BODY_LEN_V1 2048U
#define SSH_CHATTER_BBS_BODY_LEN_V2 10240U
#define SSH_CHATTER_BBS_BODY_LEN_V3 20480U

typedef struct bbs_state_header {
    uint32_t magic;
    uint32_t version;
    uint32_t post_count;
    uint32_t reserved;
    uint64_t next_id;
} bbs_state_header_t;

typedef struct bbs_state_comment_entry {
    char author[SSH_CHATTER_USERNAME_LEN];
    char text[SSH_CHATTER_BBS_COMMENT_LEN];
    int64_t created_at;
} bbs_state_comment_entry_t;

typedef struct bbs_state_post_entry {
    uint64_t id;
    int64_t created_at;
    int64_t bumped_at;
    uint32_t tag_count;
    uint32_t comment_count;
    char author[SSH_CHATTER_USERNAME_LEN];
    char title[SSH_CHATTER_BBS_TITLE_LEN];
    char body[SSH_CHATTER_BBS_BODY_LEN];
    char tags[SSH_CHATTER_BBS_MAX_TAGS][SSH_CHATTER_BBS_TAG_LEN];
    bbs_state_comment_entry_t comments[SSH_CHATTER_BBS_MAX_COMMENTS];
} bbs_state_post_entry_t;

typedef struct bbs_state_post_entry_v1 {
    uint64_t id;
    int64_t created_at;
    int64_t bumped_at;
    uint32_t tag_count;
    uint32_t comment_count;
    char author[SSH_CHATTER_USERNAME_LEN];
    char title[SSH_CHATTER_BBS_TITLE_LEN_V1];
    char body[SSH_CHATTER_BBS_BODY_LEN_V1];
    char tags[SSH_CHATTER_BBS_MAX_TAGS][SSH_CHATTER_BBS_TAG_LEN];
    bbs_state_comment_entry_t comments[SSH_CHATTER_BBS_MAX_COMMENTS];
} bbs_state_post_entry_v1_t;

typedef struct bbs_state_post_entry_v2 {
    uint64_t id;
    int64_t created_at;
    int64_t bumped_at;
    uint32_t tag_count;
    uint32_t comment_count;
    char author[SSH_CHATTER_USERNAME_LEN];
    char title[SSH_CHATTER_BBS_TITLE_LEN];
    char body[SSH_CHATTER_BBS_BODY_LEN_V2];
    char tags[SSH_CHATTER_BBS_MAX_TAGS][SSH_CHATTER_BBS_TAG_LEN];
    bbs_state_comment_entry_t comments[SSH_CHATTER_BBS_MAX_COMMENTS];
} bbs_state_post_entry_v2_t;

typedef struct bbs_state_post_entry_v3 {
    uint64_t id;
    int64_t created_at;
    int64_t bumped_at;
    uint32_t tag_count;
    uint32_t comment_count;
    char author[SSH_CHATTER_USERNAME_LEN];
    char title[SSH_CHATTER_BBS_TITLE_LEN];
    char body[SSH_CHATTER_BBS_BODY_LEN_V3];
    char tags[SSH_CHATTER_BBS_MAX_TAGS][SSH_CHATTER_BBS_TAG_LEN];
    bbs_state_comment_entry_t comments[SSH_CHATTER_BBS_MAX_COMMENTS];
} bbs_state_post_entry_v3_t;

static const uint32_t RSS_STATE_MAGIC = 0x52535331U; /* 'RSS1' */
static const uint32_t RSS_STATE_VERSION = 1U;

typedef struct rss_state_header {
    uint32_t magic;
    uint32_t version;
    uint32_t feed_count;
    uint32_t reserved;
} rss_state_header_t;

typedef struct rss_state_entry {
    char tag[SSH_CHATTER_RSS_TAG_LEN];
    char url[SSH_CHATTER_RSS_URL_LEN];
    char last_item_key[SSH_CHATTER_RSS_ITEM_KEY_LEN];
} rss_state_entry_t;

static const uint32_t ALPHA_LANDERS_STATE_MAGIC = 0x464C4147U; /* 'FLAG' */
static const uint32_t ALPHA_LANDERS_STATE_VERSION = 1U;

typedef struct alpha_landers_file_header {
    uint32_t magic;
    uint32_t version;
    uint32_t entry_count;
    uint32_t reserved;
} alpha_landers_file_header_t;

typedef struct alpha_landers_file_entry {
    char username[SSH_CHATTER_USERNAME_LEN];
    uint32_t flag_count;
    uint64_t last_flag_timestamp;
    uint32_t reserved;
} alpha_landers_file_entry_t;

static const uint32_t VOTE_STATE_MAGIC = 0x564F5445U; /* 'VOTE' */
static const uint32_t VOTE_STATE_VERSION = 1U;

typedef struct vote_state_header {
    uint32_t magic;
    uint32_t version;
    uint32_t named_count;
    uint32_t reserved;
} vote_state_header_t;

typedef struct vote_state_poll_option_entry {
    char text[SSH_CHATTER_MESSAGE_LIMIT];
    uint32_t votes;
} vote_state_poll_option_entry_t;

typedef struct vote_state_poll_entry {
    uint8_t active;
    uint8_t allow_multiple;
    uint8_t reserved[6];
    uint64_t id;
    uint32_t option_count;
    uint32_t reserved2;
    char question[SSH_CHATTER_MESSAGE_LIMIT];
    vote_state_poll_option_entry_t options[5];
} vote_state_poll_entry_t;

typedef struct vote_state_named_voter_entry {
    char username[SSH_CHATTER_USERNAME_LEN];
    int32_t choice;
    uint32_t choices_mask;
} vote_state_named_voter_entry_t;

typedef struct vote_state_named_entry {
    vote_state_poll_entry_t poll;
    char label[SSH_CHATTER_POLL_LABEL_LEN];
    char owner[SSH_CHATTER_USERNAME_LEN];
    uint32_t voter_count;
    uint32_t reserved;
    vote_state_named_voter_entry_t voters[SSH_CHATTER_MAX_NAMED_VOTERS];
} vote_state_named_entry_t;

typedef struct reaction_descriptor {
    const char *command;
    const char *label;
    const char *icon;
} reaction_descriptor_t;

static const reaction_descriptor_t
    REACTION_DEFINITIONS[SSH_CHATTER_REACTION_KIND_COUNT] = {
        {"good", "good", "b"},         {"sad", "sad", ":("},
        {"cool", "cool", "(ツ)!"},     {"angry", "angry", ":/"},
        {"checked", "checked", "[v]"}, {"love", "love", "<3"},
        {"wtf", "wtf", "凸_(ツ)"},
};

typedef struct os_descriptor {
    const char *name;
    const char *display;
} os_descriptor_t;

static const os_descriptor_t OS_CATALOG[] = {{"windows", "Windows"},
                                             {"macos", "macOS"},
                                             {"linux", "Linux"},
                                             {"freebsd", "FreeBSD"},
                                             {"ios", "iOS"},
                                             {"android", "Android"},
                                             {"watchos", "watchOS"},
                                             {"solaris", "Solaris"},
                                             {"openbsd", "OpenBSD"},
                                             {"netbsd", "NetBSD"},
                                             {"dragonflybsd", "DragonFlyBSD"},
                                             {"reactos", "ReactOS"},
                                             {"tizen", "Tizen"},
                                             {"bsd", "BSD"},
                                             {"msdos", "MS-DOS"},
                                             {"drdos", "DR-DOS"},
                                             {"kdos", "K-DOS"},
                                             {"templeos", "TempleOS"},
                                             {"zealos", "ZealOS"},
                                             {"haiku", "Haiku"},
                                             {"pcdos", "PC-DOS"}};

static const os_descriptor_t *session_lookup_os_descriptor(const char *name);

// random pool for daily functions
static const char *DAILY_FUNCTIONS[] __attribute__((unused)) = {
    "sin",     "cos",    "tan",    "sqrt",           "log",
    "exp",     "printf", "malloc", "free",           "memcpy",
    "strncpy", "qsort",  "fopen",  "close",          "select",
    "poll",    "fork",   "exec",   "pthread_create", "strtok"};

static bool chat_room_ensure_capacity(chat_room_t *room, size_t required)
{ // chat members should be within capacity
    if (room == nullptr) {
        return false;
    }

    if (required <= room->member_capacity) {
        return true;
    }

    size_t new_capacity =
        room->member_capacity == 0U ? 8U : room->member_capacity;
    while (new_capacity < required) {
        if (new_capacity > SIZE_MAX / 2U) {
            new_capacity = required;
            break;
        }
        new_capacity *= 2U;
    }

    session_ctx_t **resized =
        GC_REALLOC(room->members, new_capacity * sizeof(*resized));
    if (resized == nullptr) {
        return false;
    }

    for (size_t idx = room->member_capacity; idx < new_capacity; ++idx) {
        resized[idx] = nullptr;
    }

    room->members = resized;
    room->member_capacity = new_capacity;
    return true;
}

static void chat_room_init(chat_room_t *room)
{
    if (room == nullptr) {
        return;
    }
    pthread_mutex_init(&room->lock, nullptr);
    room->members = nullptr;
    room->member_count = 0U;
    room->member_capacity = 0U;
}

static void session_describe_peer(ssh_session session, char *buffer, size_t len)
{
    if (buffer == nullptr || len == 0U) {
        return;
    }

    buffer[0] = '\0';
    if (session == nullptr) {
        return;
    }

    const int socket_fd = ssh_get_fd(session);
    if (socket_fd < 0) {
        return;
    }

    int val = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) < 0) {
        fprintf(stderr, "[session] setsockopt SO_KEEPALIVE failed");
    }

    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(socket_fd, (struct sockaddr *)&addr, &addr_len) != 0) {
        return;
    }

    char host[NI_MAXHOST];
    if (getnameinfo((struct sockaddr *)&addr, addr_len, host, sizeof(host),
                    nullptr, 0, NI_NUMERICHOST) != 0) {
        return;
    }

    snprintf(buffer, len, "%s", host);
}

static void host_format_sockaddr(const struct sockaddr *addr, socklen_t len,
                                 char *buffer, size_t size)
{
    if (buffer == nullptr || size == 0U) {
        return;
    }

    buffer[0] = '\0';
    if (addr == nullptr) {
        return;
    }

    socklen_t host_len = (socklen_t)(size > (size_t)UINT_MAX ? UINT_MAX : size);
    if (host_len == 0) {
        return;
    }

    if (getnameinfo(addr, len, buffer, host_len, nullptr, 0, NI_NUMERICHOST) !=
        0) {
        buffer[0] = '\0';
    }
}

typedef enum {
    HOSTKEY_SUPPORT_UNKNOWN = 0,
    HOSTKEY_SUPPORT_ACCEPTED,
    HOSTKEY_SUPPORT_REJECTED, // unlisted auth key should be rejected.
} hostkey_support_status_t;

typedef struct {
    hostkey_support_status_t status;
    char offered_algorithms[256];
} hostkey_probe_result_t;

static bool hostkey_list_contains(const unsigned char *data, size_t data_len,
                                  const char *needle, size_t needle_len)
{
    if (data == nullptr || needle == nullptr || needle_len == 0U) {
        return false;
    }

    size_t position = 0U;
    while (position < data_len) {
        size_t token_end = position;
        while (token_end < data_len && data[token_end] != ',') {
            ++token_end;
        }

        const size_t token_length = token_end - position;
        if (token_length == needle_len &&
            memcmp(data + position, needle, needle_len) == 0) {
            return true;
        }

        if (token_end >= data_len) {
            break;
        }

        position = token_end + 1U;
    }

    return false;
}

static hostkey_probe_result_t
session_probe_client_hostkey_algorithms(ssh_session session,
                                        const char *const *required_algorithms,
                                        size_t required_algorithm_count)
{
    hostkey_probe_result_t result;
    result.status = HOSTKEY_SUPPORT_UNKNOWN;
    result.offered_algorithms[0] = '\0';

    if (session == nullptr || required_algorithms == nullptr ||
        required_algorithm_count == 0U) {
        return result;
    }

    for (size_t i = 0; i < required_algorithm_count; ++i) {
        if (required_algorithms[i] == nullptr ||
            required_algorithms[i][0] == '\0') {
            return result;
        }
    }

    const int socket_fd = ssh_get_fd(session);
    if (socket_fd < 0) {
        return result;
    }

    int val = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) < 0) {
        fprintf(stderr, "[session] setsockopt SO_KEEPALIVE failed");
    }

    const size_t max_buffer_size = 65536U;
    size_t buffer_size = 16384U;
    unsigned char *buffer = (unsigned char *)GC_MALLOC(buffer_size);
    if (buffer == nullptr) {
        return result;
    }

    unsigned int attempts = 0U;
    const unsigned int max_attempts = 5U;

    while (attempts < max_attempts) {
        struct pollfd poll_fd;
        poll_fd.fd = socket_fd;
        poll_fd.events = POLLIN;
        poll_fd.revents = 0;

        int poll_result = poll(&poll_fd, 1, 1000);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (poll_result == 0) {
            ++attempts;
            continue;
        }

        if ((poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            break;
        }

        ssize_t peeked =
            recv(socket_fd, buffer, buffer_size, MSG_PEEK | MSG_DONTWAIT);
        if (peeked < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN
#ifdef EWOULDBLOCK
                || errno == EWOULDBLOCK
#endif
            ) {
                ++attempts;
                continue;
            }
            break;
        }

        if (peeked == 0) {
            break;
        }

        size_t available = (size_t)peeked;
        unsigned char *newline = memchr(buffer, '\n', available);
        if (newline == nullptr) {
            if (available == buffer_size && buffer_size < max_buffer_size) {
                size_t new_size = buffer_size * 2U;
                if (new_size > max_buffer_size) {
                    new_size = max_buffer_size;
                }
                unsigned char *resized = GC_REALLOC(buffer, new_size);
                if (resized != nullptr) {
                    buffer = resized;
                    buffer_size = new_size;
                    continue;
                }
            }
            ++attempts;
            continue;
        }

        size_t payload_offset = (size_t)(newline - buffer) + 1U;
        while (payload_offset < available && (buffer[payload_offset] == '\r' ||
                                              buffer[payload_offset] == '\n')) {
            ++payload_offset;
        }

        if (available <= payload_offset || available - payload_offset < 5U) {
            ++attempts;
            continue;
        }

        const unsigned char *packet = buffer + payload_offset;
        uint32_t packet_length =
            ((uint32_t)packet[0] << 24) | ((uint32_t)packet[1] << 16) |
            ((uint32_t)packet[2] << 8) | (uint32_t)packet[3];
        if (packet_length == 0U) {
            break;
        }

        size_t total_packet_size = 4U + (size_t)packet_length;
        if (total_packet_size > available - payload_offset) {
            if (payload_offset + total_packet_size > buffer_size &&
                buffer_size < max_buffer_size) {
                size_t new_size = buffer_size;
                while (new_size < payload_offset + total_packet_size &&
                       new_size < max_buffer_size) {
                    new_size *= 2U;
                    if (new_size > max_buffer_size) {
                        new_size = max_buffer_size;
                    }
                }
                if (new_size > buffer_size) {
                    unsigned char *resized = GC_REALLOC(buffer, new_size);
                    if (resized != nullptr) {
                        buffer = resized;
                        buffer_size = new_size;
                        continue;
                    }
                }
            }
            ++attempts;
            continue;
        }

        unsigned int padding_length = packet[4];
        if ((size_t)padding_length + 1U > packet_length) {
            break;
        }

        size_t payload_length =
            (size_t)packet_length - (size_t)padding_length - 1U;
        if (payload_length < 17U) {
            break;
        }

        const unsigned char *payload = packet + 5;
        if (payload[0] != 20U) {
            break;
        }

        const unsigned char *cursor = payload + 17U;
        size_t remaining = payload_length - 17U;
        if (remaining < 4U) {
            break;
        }

        uint32_t kex_names_len =
            ((uint32_t)cursor[0] << 24) | ((uint32_t)cursor[1] << 16) |
            ((uint32_t)cursor[2] << 8) | (uint32_t)cursor[3];
        cursor += 4U;
        if ((size_t)kex_names_len > remaining - 4U) {
            break;
        }

        cursor += (size_t)kex_names_len;
        remaining -= 4U + (size_t)kex_names_len;
        if (remaining < 4U) {
            break;
        }

        uint32_t hostkey_names_len =
            ((uint32_t)cursor[0] << 24) | ((uint32_t)cursor[1] << 16) |
            ((uint32_t)cursor[2] << 8) | (uint32_t)cursor[3];
        cursor += 4U;
        if ((size_t)hostkey_names_len > remaining - 4U) {
            break;
        }

        size_t hostkey_len = (size_t)hostkey_names_len;
        const unsigned char *hostkey_data = cursor;

        size_t copy_length = hostkey_len;
        if (copy_length >= sizeof(result.offered_algorithms)) {
            copy_length = sizeof(result.offered_algorithms) - 1U;
        }
        memcpy(result.offered_algorithms, hostkey_data, copy_length);
        result.offered_algorithms[copy_length] = '\0';

        if (hostkey_len == 0U) {
            result.status = HOSTKEY_SUPPORT_REJECTED;
        } else {
            bool supported = false;
            for (size_t i = 0; i < required_algorithm_count; ++i) {
                const char *algorithm = required_algorithms[i];
                const size_t required_length = strlen(algorithm);
                if (required_length == 0U) {
                    continue;
                }

                if (hostkey_list_contains(hostkey_data, hostkey_len, algorithm,
                                          required_length)) {
                    supported = true;
                    break;
                }
            }

            if (supported) {
                result.status = HOSTKEY_SUPPORT_ACCEPTED;
            } else {
                result.status = HOSTKEY_SUPPORT_REJECTED;
            }
        }

        GC_FREE(buffer);
        return result;
    }

    GC_FREE(buffer);
    return result;
}

static bool session_is_private_ipv4(const unsigned char octets[4])
{
    if (octets == nullptr) {
        return false;
    }

    if (octets[0] == 10U || octets[0] == 127U) {
        return true;
    }

    if (octets[0] == 172U && octets[1] >= 16U && octets[1] <= 31U) {
        return true;
    }

    if ((octets[0] == 192U && octets[1] == 168U) ||
        (octets[0] == 169U && octets[1] == 254U)) {
        return true;
    }

    return false;
}

bool session_is_lan_client(const char *ip)
{
    if (ip == nullptr || ip[0] == '\0') {
        return false;
    }

    struct in_addr addr4;
    if (inet_pton(AF_INET, ip, &addr4) == 1) {
        unsigned char octets[4];
        memcpy(octets, &addr4.s_addr, sizeof(octets));
        return session_is_private_ipv4(octets);
    }

    struct in6_addr addr6;
    if (inet_pton(AF_INET6, ip, &addr6) != 1) {
        return false;
    }

    if (IN6_IS_ADDR_LOOPBACK(&addr6) || IN6_IS_ADDR_LINKLOCAL(&addr6)) {
        return true;
    }

    if (IN6_IS_ADDR_V4MAPPED(&addr6)) {
        return session_is_private_ipv4(&addr6.s6_addr[12]);
    }

    const unsigned char first_byte = addr6.s6_addr[0];
    if ((first_byte & 0xfeU) == 0xfcU) { // fc00::/7 unique local
        return true;
    }

    return false;
}

static void session_assign_lan_privileges(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->lan_operator_credentials_valid) {
        ctx->user.is_lan_operator = false;
        return;
    }

    if (!session_is_lan_client(ctx->client_ip)) {
        ctx->lan_operator_credentials_valid = false;
        ctx->user.is_lan_operator = false;
        return;
    }

    if (!host_is_lan_operator_username(ctx->owner, ctx->user.name)) {
        ctx->lan_operator_credentials_valid = false;
        ctx->user.is_lan_operator = false;
        return;
    }

    ctx->user.is_operator = true;
    ctx->auth.is_operator = true;
    ctx->user.is_lan_operator = true;
}

static void session_apply_granted_privileges(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (host_ip_has_grant(ctx->owner, ctx->client_ip)) {
        ctx->user.is_operator = true;
        ctx->auth.is_operator = true;
    }
}

static void chat_room_add(chat_room_t *room, session_ctx_t *session)
{
    if (room == nullptr || session == nullptr) {
        return;
    }

    pthread_mutex_lock(&room->lock);
    for (size_t idx = 0; idx < room->member_count; ++idx) {
        if (room->members[idx] == session) {
            pthread_mutex_unlock(&room->lock);
            return;
        }
    }
    if (chat_room_ensure_capacity(room, room->member_count + 1U)) {
        room->members[room->member_count++] = session;
    } else {
        humanized_log_error("chat-room", "failed to grow member list", ENOMEM);
    }
    pthread_mutex_unlock(&room->lock);
}

static void chat_room_remove(chat_room_t *room, const session_ctx_t *session)
{
    if (room == nullptr || session == nullptr) {
        return;
    }

    pthread_mutex_lock(&room->lock);
    for (size_t idx = 0; idx < room->member_count; ++idx) {
        if (room->members[idx] == session) {
            for (size_t shift = idx; shift + 1U < room->member_count; ++shift) {
                room->members[shift] = room->members[shift + 1U];
            }
            room->members[room->member_count - 1U] = nullptr;
            room->member_count--;
            break;
        }
    }
    pthread_mutex_unlock(&room->lock);
}

static void chat_room_broadcast_should_sink(chat_room_t *room)
{
    if (room == nullptr) {
        return;
    }

    session_ctx_t **targets = nullptr;
    size_t target_count = 0U;

    pthread_mutex_lock(&room->lock);
    size_t expected_targets = room->member_count;
    if (expected_targets > 0U) {
        targets = GC_MALLOC(expected_targets * sizeof(*targets));
        if (targets != nullptr) {
            memset(targets, 0, expected_targets * sizeof(*targets));
            for (size_t idx = 0; idx < room->member_count; ++idx) {
                session_ctx_t *member = room->members[idx];
                if (member == nullptr || member->channel == nullptr) {
                    continue;
                }
                targets[target_count++] = member;
            }
        }
    }
    pthread_mutex_unlock(&room->lock);

    if (targets == nullptr && target_count == 0U) {
        return;
    }

    for (size_t idx = 0; idx < target_count; ++idx) {
        session_flag_should_sink(targets[idx]);
    }

    GC_FREE(targets);
}

static void chat_room_broadcast(chat_room_t *room, const char *message,
                                const session_ctx_t *from)
{
    if (room == nullptr || message == nullptr) {
        return;
    }

    session_ctx_t **targets = nullptr;
    size_t target_count = 0U;
    size_t expected_targets = 0U;

    pthread_mutex_lock(&room->lock);
    expected_targets = room->member_count;
    if (expected_targets > 0U) {
        targets = GC_MALLOC(expected_targets * sizeof(*targets));
        if (targets != nullptr) {
            memset(targets, 0, expected_targets * sizeof(*targets));
            for (size_t idx = 0; idx < room->member_count; ++idx) {
                session_ctx_t *member = room->members[idx];
                if (member == nullptr || !session_transport_active(member)) {
                    continue;
                }
                if (from != nullptr && member == from) {
                    continue;
                }
                // Skip users who have the no_update flag set (scrolled back in history)
                if (member->no_update &&
                    member->history_scroll_position == 0U) {
                    // If the user has already returned to the newest message,
                    // clear the freeze flag so chat output resumes.
                    member->no_update = false;
                }
                if (member->no_update &&
                    member->transport_kind != SESSION_TRANSPORT_TELNET) {
                    continue;
                }
                targets[target_count++] = member;
            }
        }
    }
    pthread_mutex_unlock(&room->lock);

    if (targets == nullptr && expected_targets > 0U) {
        humanized_log_error("chat-room", "failed to allocate broadcast buffer",
                            ENOMEM);
        return;
    }

    // For real-time broadcast: format and send directly without history lookup
    for (size_t idx = 0; idx < target_count; ++idx) {
        session_ctx_t *member = targets[idx];

        // Flush and disable buffering to ensure immediate message delivery
        // This is critical for telnet sessions where buffered writes can hide
        // new messages until another action flushes the buffer
        session_output_buffer_flush(member);
        member->output_buffering_enabled = false;
        member->output_buffer_length = 0U;

        // For telnet, clear the current input line first before displaying the message
        // This prevents the old prompt from remaining visible above the new message
        if (member->transport_kind == SESSION_TRANSPORT_TELNET) {
            // Move to column 1 and clear the line
            static const char clear_line[] = "\033[1G\033[K";
            session_channel_write(member, clear_line, sizeof(clear_line) - 1U);
        }

        if (from != nullptr) {
            // Format message directly for real-time delivery
            char formatted[SSH_CHATTER_MESSAGE_LIMIT * 2U];
            const char *color =
                from->user_color_code != nullptr ? from->user_color_code : "";
            const char *highlight = from->user_highlight_code != nullptr
                                        ? from->user_highlight_code
                                        : "";
            const char *bold = from->user_is_bold ? ANSI_BOLD : "";

            const bool has_custom_codes =
                (color[0] != '\0') || (highlight[0] != '\0');

            if (has_custom_codes) {
                snprintf(formatted, sizeof(formatted), "[-] <%s%s%s%s%s> %s",
                         highlight, color, bold, from->user.name, ANSI_RESET,
                         message);
            } else {
                snprintf(formatted, sizeof(formatted), "%s%s [-] <%s>%s %s",
                         color, bold, from->user.name, ANSI_RESET, message);
            }

            session_send_plain_line(member, formatted);
        } else {
            session_send_system_line(member, message);
        }

        // Flush the channel to ensure immediate delivery
        session_channel_flush(member);

        // For telnet, always refresh input line to ensure messages are visible
        // For SSH, only refresh when at bottom of history
        if (member->transport_kind == SESSION_TRANSPORT_TELNET ||
            member->history_scroll_position == 0U) {
            session_refresh_input_line(member);
        }
    }

    GC_FREE(targets);
}

static void chat_room_broadcast_caption(chat_room_t *room, const char *message)
{
    if (room == nullptr || message == nullptr) {
        return;
    }

    session_ctx_t **targets = nullptr;
    size_t target_count = 0U;
    size_t expected_targets = 0U;

    pthread_mutex_lock(&room->lock);
    expected_targets = room->member_count;
    if (expected_targets > 0U) {
        targets = GC_MALLOC(expected_targets * sizeof(*targets));
        if (targets != nullptr) {
            memset(targets, 0, expected_targets * sizeof(*targets));
            for (size_t idx = 0; idx < room->member_count; ++idx) {
                session_ctx_t *member = room->members[idx];
                if (member == nullptr || !session_transport_active(member)) {
                    continue;
                }
                // Skip users who have the no_update flag set (scrolled back in history)
                if (member->no_update &&
                    member->history_scroll_position == 0U) {
                    // If the user has already returned to the newest message,
                    // clear the freeze flag so chat output resumes.
                    member->no_update = false;
                }
                if (member->no_update &&
                    member->transport_kind != SESSION_TRANSPORT_TELNET) {
                    continue;
                }
                targets[target_count++] = member;
            }
        }
    }
    pthread_mutex_unlock(&room->lock);

    if (targets == nullptr && expected_targets > 0U) {
        humanized_log_error("chat-room", "failed to allocate broadcast buffer",
                            ENOMEM);
        return;
    }

    for (size_t idx = 0; idx < target_count; ++idx) {
        session_ctx_t *member = targets[idx];

        // Flush and disable buffering to ensure immediate message delivery
        // This is critical for telnet sessions where buffered writes can hide
        // new messages until another action flushes the buffer
        session_output_buffer_flush(member);
        member->output_buffering_enabled = false;
        member->output_buffer_length = 0U;

        // For telnet, clear the current input line first before displaying the message
        if (member->transport_kind == SESSION_TRANSPORT_TELNET) {
            static const char clear_line[] = "\033[1G\033[K";
            session_channel_write(member, clear_line, sizeof(clear_line) - 1U);
        }

        session_send_caption_line(member, message);

        // Flush the channel to ensure immediate delivery
        session_channel_flush(member);

        // For telnet, always refresh input line to ensure messages are visible
        // For SSH, only refresh when at bottom of history
        if (member->transport_kind == SESSION_TRANSPORT_TELNET ||
            member->history_scroll_position == 0U) {
            session_refresh_input_line(member);
        }
    }

    // printf("\033[1G[broadcast caption] %s\n", message);

    GC_FREE(targets);
}

static void chat_room_broadcast_entry(chat_room_t *room,
                                      const chat_history_entry_t *entry,
                                      const session_ctx_t *from)
{
    if (room == nullptr || entry == nullptr) {
        return;
    }

    // Broadcast sink flag so listeners can pull the latest chat chunk when appropriate
    chat_room_broadcast_should_sink(room);

    session_ctx_t **targets = nullptr;
    size_t target_count = 0U;
    size_t expected_targets = 0U;
    session_ctx_t **sink_targets = nullptr;
    size_t sink_count = 0U;

    pthread_mutex_lock(&room->lock);
    expected_targets = room->member_count;
    if (expected_targets > 0U) {
        targets = GC_MALLOC(expected_targets * sizeof(*targets));
        if (targets != nullptr) {
            memset(targets, 0, expected_targets * sizeof(*targets));
            for (size_t idx = 0; idx < room->member_count; ++idx) {
                session_ctx_t *member = room->members[idx];
                if (member == nullptr || !session_transport_active(member)) {
                    continue;
                }
                if (from != nullptr && member == from) {
                    continue;
                }
                // Skip users who have the no_update flag set (scrolled back in history)
                if (member->no_update &&
                    member->history_scroll_position == 0U) {
                    // If the user has already returned to the newest message,
                    // clear the freeze flag so chat output resumes.
                    member->no_update = false;
                }
                if (member->no_update &&
                    member->transport_kind != SESSION_TRANSPORT_TELNET) {
                    sink_targets[sink_count++] = member;
                    continue;
                }
                targets[target_count++] = member;
            }
        }
    }
    pthread_mutex_unlock(&room->lock);

    if (sink_targets != nullptr && sink_count > 0U) {
        for (size_t idx = 0; idx < sink_count; ++idx) {
            session_flag_should_sink(sink_targets[idx]);
        }
    }

    if (targets == nullptr && expected_targets > 0U) {
        humanized_log_error(
            "chat-room", "failed to allocate entry broadcast buffer", ENOMEM);
        GC_FREE(sink_targets);
        return;
    }

    // For real-time broadcast: format and send directly without history lookup
    for (size_t idx = 0; idx < target_count; ++idx) {
        session_ctx_t *member = targets[idx];

        // Flush and disable buffering to ensure immediate message delivery
        // This is critical for telnet sessions where buffered writes can hide
        // new messages until another action flushes the buffer
        session_output_buffer_flush(member);
        member->output_buffering_enabled = false;
        member->output_buffer_length = 0U;

        // For telnet, clear the current input line first before displaying the message
        if (member->transport_kind == SESSION_TRANSPORT_TELNET) {
            static const char clear_line[] = "\033[1G\033[K";
            session_channel_write(member, clear_line, sizeof(clear_line) - 1U);
        }

        bool previous_capture = member->capture_realtime_output;
        member->capture_realtime_output =
            (member->history_scroll_position == 0U) && !member->no_update;

        if (entry->is_user_message) {
            // Format user message directly, handling multi-line content to avoid
            // inserting unintended blank lines.
            char id_label[32] = "-";
            if (entry->message_id > 0U) {
                host_compact_id_encode(entry->message_id, id_label,
                                       sizeof(id_label));
            }

            // Send attachment if present
            if (entry->attachment_type != CHAT_ATTACHMENT_NONE &&
                entry->attachment_target[0] != '\0') {
                const char *label =
                    chat_attachment_type_label(entry->attachment_type);
                char attachment_line[SSH_CHATTER_MESSAGE_LIMIT];
                snprintf(attachment_line, sizeof(attachment_line),
                         "    (%s)" ANSI_RESET " %s", label,
                         entry->attachment_target);
                session_send_plain_line(member, attachment_line);

                if (entry->attachment_caption[0] != '\0') {
                    char caption_line[SSH_CHATTER_MESSAGE_LIMIT];
                    snprintf(caption_line, sizeof(caption_line),
                             "    \342\206\263 %s", entry->attachment_caption);
                    session_send_plain_line(member, caption_line);
                }
            }
        } else {
            // System message
            session_send_plain_line(member, entry->message);
        }

        // Flush the channel to ensure immediate delivery
        session_channel_flush(member);

        // Auto-scroll all users to the latest message when new content arrives
        // This ensures everyone sees new messages immediately, unless they're actively
        // scrolled back viewing history
        if (member->history_scroll_position > 0U) {
            // Reset scroll position to show the latest messages
            session_scrollback_reset_position(member);
        }

        // Refresh input line to display the message and prompt
        // For telnet, always refresh to ensure messages are visible
        // For SSH, refresh after auto-scrolling to latest
        if (member->transport_kind == SESSION_TRANSPORT_TELNET ||
            member->history_scroll_position == 0U) {
            session_refresh_input_line(member);

            // For telnet, flush again after refreshing the input line to ensure
            // the prompt and any typed text are immediately visible along with the message
            if (member->transport_kind == SESSION_TRANSPORT_TELNET) {
                session_channel_flush(member);
            }
        }

        member->capture_realtime_output = previous_capture;
    }

    GC_FREE(targets);
    GC_FREE(sink_targets);
}

static void
chat_room_broadcast_reaction_update(host_t *host,
                                    const chat_history_entry_t *entry)
{
    if (host == nullptr || entry == nullptr) {
        return;
    }

    char summary[SSH_CHATTER_MESSAGE_LIMIT];
    if (!chat_history_entry_build_reaction_summary(entry, summary,
                                                   sizeof(summary))) {
        return;
    }

    char line[SSH_CHATTER_MESSAGE_LIMIT];
    if (entry->message_id > 0U) {
        char label[32];
        if (!host_compact_id_encode(entry->message_id, label, sizeof(label))) {
            snprintf(label, sizeof(label), "%" PRIu64, entry->message_id);
        }
        snprintf(line, sizeof(line), "    ->[#%s] reactions: %s", label,
                 summary);
    } else {
        snprintf(line, sizeof(line), "    ->reactions: %s", summary);
    }

    chat_room_broadcast_caption(&host->room, line);
}

static bool host_history_reserve_locked(host_t *host, size_t min_capacity)
{
    if (host == nullptr) {
        return false;
    }

    if (SSH_CHATTER_HISTORY_CACHE_LIMIT > 0U &&
        min_capacity > SSH_CHATTER_HISTORY_CACHE_LIMIT) {
        min_capacity = SSH_CHATTER_HISTORY_CACHE_LIMIT;
    }

    if (min_capacity <= host->history_capacity) {
        return true;
    }

    if (min_capacity > SIZE_MAX / sizeof(chat_history_entry_t)) {
        humanized_log_error("host-history",
                            "history buffer too large to allocate", ENOMEM);
        return false;
    }

    size_t new_capacity =
        host->history_capacity > 0U ? host->history_capacity : 64U;
    if (new_capacity == 0U) {
        new_capacity = 64U;
    }

    while (new_capacity < min_capacity) {
        if (new_capacity > SIZE_MAX / 2U) {
            new_capacity = min_capacity;
            break;
        }
        size_t doubled = new_capacity * 2U;
        if (doubled < new_capacity ||
            doubled > SIZE_MAX / sizeof(chat_history_entry_t)) {
            new_capacity = min_capacity;
            break;
        }
        new_capacity = doubled;
    }

    size_t bytes = new_capacity * sizeof(chat_history_entry_t);
    chat_history_entry_t *resized = GC_REALLOC(host->history, bytes);
    if (resized == nullptr) {
        humanized_log_error("host-history",
                            "failed to grow chat history buffer",
                            errno != 0 ? errno : ENOMEM);
        return false;
    }

    if (new_capacity > host->history_capacity) {
        size_t old_capacity = host->history_capacity;
        size_t added = new_capacity - old_capacity;
        memset(resized + old_capacity, 0, added * sizeof(chat_history_entry_t));
    }

    host->history = resized;
    host->history_capacity = new_capacity;
    return true;
}

static bool host_history_append_locked(host_t *host,
                                       const chat_history_entry_t *entry)
{
    if (host == nullptr || entry == nullptr) {
        return false;
    }

    size_t cache_limit = SSH_CHATTER_HISTORY_CACHE_LIMIT;
    if (cache_limit == 0U) {
        cache_limit = host->history_count + 1U;
    }

    size_t desired_capacity = host->history_count + 1U;
    if (cache_limit > 0U && desired_capacity > cache_limit) {
        desired_capacity = cache_limit;
    }

    if (!host_history_reserve_locked(host, desired_capacity)) {
        return false;
    }

    if (cache_limit == 0U || host->history_count < cache_limit) {
        host->history[host->history_count++] = *entry;
    } else if (host->history_count > 0U) {
        memmove(host->history, host->history + 1,
                (host->history_count - 1U) * sizeof(host->history[0]));
        host->history[host->history_count - 1U] = *entry;
        host->history_start_index += 1U;
    } else {
        host->history[0] = *entry;
        host->history_count = 1U;
    }

    host->history_total += 1U;
    if (host->history_total < host->history_count) {
        host->history_total = host->history_count;
    }

    if (cache_limit > 0U) {
        size_t expected_start =
            (host->history_total > host->history_count)
                ? (host->history_total - host->history_count)
                : 0U;
        host->history_start_index = expected_start;
    }

    host_state_save_locked(host);
    return true;
}

static size_t host_history_total(host_t *host)
{
    if (host == nullptr) {
        return 0U;
    }

    size_t count = 0U;
    pthread_mutex_lock(&host->lock);
    count = host->history_total;
    pthread_mutex_unlock(&host->lock);
    return count;
}

static bool host_state_read_history_entry(FILE *fp, uint32_t version,
                                          chat_history_entry_t *entry)
{
    if (fp == nullptr || entry == nullptr) {
        return false;
    }

    memset(entry, 0, sizeof(*entry));

    if (version != HOST_STATE_VERSION) {
        return false;
    }

    host_state_history_entry_t serialized = {0};
    if (fread(&serialized, sizeof(serialized), 1U, fp) != 1U) {
        return false;
    }

    entry->is_user_message = serialized.is_user_message != 0U;
    entry->user_is_bold = serialized.user_is_bold != 0U;
    snprintf(entry->username, sizeof(entry->username), "%s",
             serialized.username);
    snprintf(entry->raw_username, sizeof(entry->raw_username), "%s",
             serialized.raw_username);
    snprintf(entry->message, sizeof(entry->message), "%s", serialized.message);
    snprintf(entry->user_color_name, sizeof(entry->user_color_name), "%s",
             serialized.user_color_name);
    snprintf(entry->user_highlight_name, sizeof(entry->user_highlight_name),
             "%s", serialized.user_highlight_name);
    entry->message_id = serialized.message_id;
    if (serialized.attachment_type > CHAT_ATTACHMENT_FILE) {
        entry->attachment_type = CHAT_ATTACHMENT_NONE;
    } else {
        entry->attachment_type =
            (chat_attachment_type_t)serialized.attachment_type;
    }
    entry->created_at = (time_t)serialized.created_at;
    entry->preserve_whitespace = serialized.reserved[0] != 0U;
    snprintf(entry->attachment_target, sizeof(entry->attachment_target), "%s",
             serialized.attachment_target);
    snprintf(entry->attachment_caption, sizeof(entry->attachment_caption), "%s",
             serialized.attachment_caption);
    host_state_assign_color_codes(entry, serialized.user_color_code,
                                  serialized.user_highlight_code);
    memcpy(entry->reaction_counts, serialized.reaction_counts,
           sizeof(entry->reaction_counts));
    if (entry->raw_username[0] == '\0') {
        snprintf(entry->raw_username, sizeof(entry->raw_username), "%s",
                 entry->username);
    }
    return true;
}
static bool host_state_write_history_entry(FILE *fp,
                                           const chat_history_entry_t *entry)
{
    if (fp == nullptr || entry == nullptr) {
        return false;
    }

    host_state_history_entry_t serialized = {0};
    serialized.is_user_message = entry->is_user_message ? 1U : 0U;
    serialized.user_is_bold = entry->user_is_bold ? 1U : 0U;
    snprintf(serialized.username, sizeof(serialized.username), "%s",
             entry->username);
    snprintf(serialized.raw_username, sizeof(serialized.raw_username), "%s",
             entry->raw_username);
    snprintf(serialized.message, sizeof(serialized.message), "%s",
             entry->message);
    snprintf(serialized.user_color_name, sizeof(serialized.user_color_name),
             "%s", entry->user_color_name);
    snprintf(serialized.user_highlight_name,
             sizeof(serialized.user_highlight_name), "%s",
             entry->user_highlight_name);
    serialized.message_id = entry->message_id;
    serialized.created_at = (int64_t)entry->created_at;
    serialized.attachment_type = (uint8_t)entry->attachment_type;
    snprintf(serialized.attachment_target, sizeof(serialized.attachment_target),
             "%s", entry->attachment_target);
    snprintf(serialized.attachment_caption,
             sizeof(serialized.attachment_caption), "%s",
             entry->attachment_caption);
    snprintf(serialized.user_color_code, sizeof(serialized.user_color_code),
             "%s",
             entry->user_color_code != nullptr ? entry->user_color_code : "");
    snprintf(serialized.user_highlight_code,
             sizeof(serialized.user_highlight_code), "%s",
             entry->user_highlight_code != nullptr ? entry->user_highlight_code
                                                   : "");
    memcpy(serialized.reaction_counts, entry->reaction_counts,
           sizeof(serialized.reaction_counts));
    memset(serialized.reserved, 0, sizeof(serialized.reserved));
    serialized.reserved[0] = entry->preserve_whitespace ? 1U : 0U;

    return fwrite(&serialized, sizeof(serialized), 1U, fp) == 1U;
}
static bool host_state_stream_open(const char *path, FILE **out_fp,
                                   uint32_t *version, uint32_t *history_count)
{
    if (path == nullptr || path[0] == '\0' || out_fp == nullptr ||
        version == nullptr || history_count == nullptr) {
        return false;
    }

    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        return false;
    }

    host_state_header_v1_t base_header = {0};
    if (fread(&base_header, sizeof(base_header), 1U, fp) != 1U) {
        fclose(fp);
        return false;
    }

    if (base_header.magic != HOST_STATE_MAGIC) {
        fclose(fp);
        return false;
    }

    uint32_t file_version = base_header.version;
    if (file_version != HOST_STATE_VERSION) {
        fclose(fp);
        return false;
    }

    if (file_version >= 2U) {
        uint32_t sound_count_raw = 0U;
        uint32_t grant_count_raw = 0U;
        uint64_t next_id_raw = 0U;
        if (fread(&sound_count_raw, sizeof(sound_count_raw), 1U, fp) != 1U ||
            fread(&grant_count_raw, sizeof(grant_count_raw), 1U, fp) != 1U ||
            fread(&next_id_raw, sizeof(next_id_raw), 1U, fp) != 1U) {
            fclose(fp);
            return false;
        }
    }

    if (file_version >= 8U) {
        uint8_t captcha_enabled_raw = 0U;
        uint8_t reserved_bytes[7];
        if (fread(&captcha_enabled_raw, sizeof(captcha_enabled_raw), 1U, fp) !=
                1U ||
            fread(reserved_bytes, sizeof(reserved_bytes), 1U, fp) != 1U) {
            fclose(fp);
            return false;
        }
    }

    *version = file_version;
    *history_count = base_header.history_count;
    *out_fp = fp;
    return true;
}

static size_t host_state_history_entry_stride(uint32_t version)
{
    if (version == HOST_STATE_VERSION) {
        return sizeof(host_state_history_entry_t);
    }
    return 0U;
}

static size_t host_state_read_history_range(const char *path, size_t start,
                                            chat_history_entry_t *buffer,
                                            size_t count)
{
    if (buffer == nullptr || count == 0U) {
        return 0U;
    }

    FILE *fp = nullptr;
    uint32_t version = 0U;
    uint32_t total = 0U;
    if (!host_state_stream_open(path, &fp, &version, &total)) {
        return 0U;
    }

    size_t produced = 0U;
    if ((size_t)total <= start) {
        fclose(fp);
        return 0U;
    }

    size_t stride = host_state_history_entry_stride(version);
    if (stride == 0U) {
        fclose(fp);
        return 0U;
    }

    if (start > 0U) {
        bool skip_ok = false;
        if (stride <= SIZE_MAX / start) {
            off_t offset = (off_t)(stride * start);
            if (offset >= 0 && fseeko(fp, offset, SEEK_CUR) == 0) {
                skip_ok = true;
            }
        }

        if (!skip_ok) {
            size_t remaining = start;
            while (remaining > 0U) {
                chat_history_entry_t discarded = {0};
                if (!host_state_read_history_entry(fp, version, &discarded)) {
                    fclose(fp);
                    return 0U;
                }
                --remaining;
            }
        }
    }

    while (produced < count) {
        chat_history_entry_t entry = {0};
        if (!host_state_read_history_entry(fp, version, &entry)) {
            break;
        }
        buffer[produced++] = entry;
    }

    fclose(fp);
    return produced;
}

static size_t host_history_copy_range(host_t *host, size_t start_index,
                                      chat_history_entry_t *buffer,
                                      size_t capacity)
{
    if (host == nullptr || buffer == nullptr || capacity == 0U) {
        return 0U;
    }

    chat_history_entry_t *cached_copy = nullptr;
    size_t cached_count = 0U;
    size_t cached_offset = 0U;
    size_t before_cache = 0U;
    size_t total_available = 0U;
    char state_path[PATH_MAX];
    state_path[0] = '\0';

    pthread_mutex_lock(&host->lock);
    size_t total = host->history_total;
    if (start_index >= total) {
        pthread_mutex_unlock(&host->lock);
        return 0U;
    }

    size_t cache_start = host->history_start_index;
    size_t cache_count = host->history_count;
    size_t cache_end = cache_start + cache_count;

    total_available = total - start_index;
    if (total_available > capacity) {
        total_available = capacity;
    }

    if (start_index < cache_start) {
        before_cache = cache_start - start_index;
        if (before_cache > total_available) {
            before_cache = total_available;
        }
        if (before_cache > 0U && host->state_file_path[0] != '\0') {
            snprintf(state_path, sizeof(state_path), "%s",
                     host->state_file_path);
        }
    }

    size_t cache_portion = 0U;
    size_t cache_begin_index = start_index + before_cache;
    if (total_available > before_cache && cache_begin_index < cache_end &&
        host->history != nullptr) {
        cache_portion = total_available - before_cache;
        size_t available_cache = cache_end - cache_begin_index;
        if (cache_portion > available_cache) {
            cache_portion = available_cache;
        }
    }

    if (cache_portion > 0U) {
        cached_copy = (chat_history_entry_t *)GC_MALLOC(cache_portion *
                                                        sizeof(*cached_copy));
        if (cached_copy == nullptr) {
            pthread_mutex_unlock(&host->lock);
            return 0U;
        }
        cached_offset = cache_begin_index - cache_start;
        for (size_t idx = 0U; idx < cache_portion; ++idx) {
            cached_copy[idx] = host->history[cached_offset + idx];
        }
        cached_count = cache_portion;
    }

    pthread_mutex_unlock(&host->lock);

    size_t produced = 0U;

    if (before_cache > 0U) {
        if (state_path[0] == '\0') {
            GC_FREE(cached_copy);
            return 0U;
        }
        size_t fetched = host_state_read_history_range(state_path, start_index,
                                                       buffer, before_cache);
        if (fetched < before_cache) {
            GC_FREE(cached_copy);
            return fetched;
        }
        produced += fetched;
    }

    if (cached_count > 0U && cached_copy != nullptr) {
        memcpy(buffer + produced, cached_copy,
               cached_count * sizeof(*cached_copy));
        produced += cached_count;
    }

    GC_FREE(cached_copy);
    return produced;
}

static bool host_history_find_entry_by_id(host_t *host, uint64_t message_id,
                                          chat_history_entry_t *entry)
{
    if (host == nullptr || entry == nullptr || message_id == 0U) {
        return false;
    }

    bool found = false;
    size_t older_count = 0U;
    char state_path[PATH_MAX];
    state_path[0] = '\0';
    uint32_t file_version = 0U;
    uint32_t file_history_count = 0U;

    pthread_mutex_lock(&host->lock);
    if (host->history != nullptr) {
        for (size_t idx = 0U; idx < host->history_count; ++idx) {
            const chat_history_entry_t *candidate = &host->history[idx];
            if (candidate->message_id != message_id) {
                continue;
            }

            *entry = *candidate;
            found = true;
            break;
        }
    }
    if (!found) {
        older_count = host->history_start_index;
        if (older_count > 0U && host->state_file_path[0] != '\0') {
            snprintf(state_path, sizeof(state_path), "%s",
                     host->state_file_path);
        }
    }
    pthread_mutex_unlock(&host->lock);

    if (found) {
        return true;
    }

    if (state_path[0] != '\0') {
        FILE *fp = nullptr;
        if (host_state_stream_open(state_path, &fp, &file_version,
                                   &file_history_count)) {
            size_t limit = older_count;
            if (limit > (size_t)file_history_count) {
                limit = (size_t)file_history_count;
            }
            for (size_t idx = 0U; idx < limit; ++idx) {
                chat_history_entry_t candidate = {0};
                if (!host_state_read_history_entry(fp, file_version,
                                                   &candidate)) {
                    break;
                }
                if (candidate.message_id != message_id) {
                    continue;
                }
                *entry = candidate;
                found = true;
                break;
            }
            fclose(fp);
        }
    }

    return found;
}

static size_t host_history_delete_range(host_t *host, uint64_t start_id,
                                        uint64_t end_id,
                                        uint64_t *first_removed,
                                        uint64_t *last_removed,
                                        size_t *replies_removed)
{
    if (first_removed != nullptr) {
        *first_removed = 0U;
    }
    if (last_removed != nullptr) {
        *last_removed = 0U;
    }
    if (replies_removed != nullptr) {
        *replies_removed = 0U;
    }

    if (host == nullptr || start_id == 0U || end_id == 0U ||
        start_id > end_id) {
        return 0U;
    }

    size_t removed = 0U;
    size_t reply_removed = 0U;
    uint64_t local_first = 0U;
    uint64_t local_last = 0U;

    pthread_mutex_lock(&host->lock);

    chat_history_entry_t *entries = nullptr;
    size_t entry_count = 0U;
    bool history_loaded = false;

    if (host->state_file_path[0] != '\0') {
        FILE *fp = nullptr;
        uint32_t version = 0U;
        uint32_t file_history_count = 0U;
        if (host_state_stream_open(host->state_file_path, &fp, &version,
                                   &file_history_count)) {
            entry_count = (size_t)file_history_count;
            if (entry_count > 0U) {
                entries = (chat_history_entry_t *)GC_MALLOC(entry_count *
                                                            sizeof(*entries));
                if (entries != nullptr) {
                    history_loaded = true;
                    for (size_t idx = 0U; idx < entry_count; ++idx) {
                        if (!host_state_read_history_entry(fp, version,
                                                           &entries[idx])) {
                            history_loaded = false;
                            break;
                        }
                    }
                }
            } else {
                history_loaded = true;
            }
            fclose(fp);
        }
    }

    if (!history_loaded) {
        entry_count = host->history_count;
        if (entry_count > 0U) {
            entries = (chat_history_entry_t *)GC_MALLOC(entry_count *
                                                        sizeof(*entries));
            if (entries == nullptr) {
                pthread_mutex_unlock(&host->lock);
                return 0U;
            }
            for (size_t idx = 0U; idx < entry_count; ++idx) {
                entries[idx] = host->history[idx];
            }
        }
        history_loaded = true;
    }

    if (!history_loaded || (entries == nullptr && entry_count == 0U)) {
        pthread_mutex_unlock(&host->lock);
        GC_FREE(entries);
        return 0U;
    }

    size_t write_index = 0U;
    for (size_t idx = 0U; idx < entry_count; ++idx) {
        chat_history_entry_t *entry = &entries[idx];
        const bool drop = entry->is_user_message &&
                          entry->message_id >= start_id &&
                          entry->message_id <= end_id;
        if (drop) {
            if (local_first == 0U || entry->message_id < local_first) {
                local_first = entry->message_id;
            }
            if (entry->message_id > local_last) {
                local_last = entry->message_id;
            }
            ++removed;
            continue;
        }

        if (write_index != idx) {
            entries[write_index] = *entry;
        }
        ++write_index;
    }

    entry_count = write_index;

    if (removed == 0U) {
        pthread_mutex_unlock(&host->lock);
        GC_FREE(entries);
        return 0U;
    }

    size_t cache_limit = SSH_CHATTER_HISTORY_CACHE_LIMIT;
    size_t new_cache_count = entry_count;
    if (cache_limit > 0U && new_cache_count > cache_limit) {
        new_cache_count = cache_limit;
    }
    size_t new_cache_start =
        (entry_count > new_cache_count) ? (entry_count - new_cache_count) : 0U;

    if (!host_history_reserve_locked(host, new_cache_count)) {
        pthread_mutex_unlock(&host->lock);
        GC_FREE(entries);
        return 0U;
    }

    for (size_t idx = 0U; idx < new_cache_count; ++idx) {
        host->history[idx] = entries[new_cache_start + idx];
    }
    if (host->history_count > new_cache_count) {
        for (size_t idx = new_cache_count; idx < host->history_count; ++idx) {
            memset(&host->history[idx], 0, sizeof(host->history[idx]));
        }
    }
    host->history_count = new_cache_count;
    host->history_start_index = new_cache_start;
    host->history_total = entry_count;

    uint64_t max_message_id = 0U;
    for (size_t idx = 0U; idx < entry_count; ++idx) {
        if (entries[idx].message_id > max_message_id) {
            max_message_id = entries[idx].message_id;
        }
    }
    if (max_message_id == 0U) {
        host->next_message_id = 1U;
    } else if (host->next_message_id <= max_message_id) {
        host->next_message_id = max_message_id + 1U;
    }

    if (host->reply_count > 0U) {
        size_t reply_write = 0U;
        for (size_t idx = 0U; idx < host->reply_count; ++idx) {
            chat_reply_entry_t *entry = &host->replies[idx];
            if (!entry->in_use) {
                continue;
            }

            const bool drop = entry->parent_message_id >= start_id &&
                              entry->parent_message_id <= end_id;
            if (drop) {
                ++reply_removed;
                continue;
            }

            if (reply_write != idx) {
                host->replies[reply_write] = *entry;
            }
            ++reply_write;
        }

        if (reply_removed > 0U) {
            for (size_t idx = reply_write; idx < host->reply_count; ++idx) {
                memset(&host->replies[idx], 0, sizeof(host->replies[idx]));
            }
            host->reply_count = reply_write;

            uint64_t max_reply_id = 0U;
            for (size_t idx = 0U; idx < host->reply_count; ++idx) {
                const chat_reply_entry_t *entry = &host->replies[idx];
                if (!entry->in_use) {
                    continue;
                }
                if (entry->reply_id > max_reply_id) {
                    max_reply_id = entry->reply_id;
                }
            }

            if (max_reply_id == 0U) {
                host->next_reply_id =
                    host->reply_count == 0U ? 1U : host->next_reply_id;
            } else if (host->next_reply_id <= max_reply_id) {
                host->next_reply_id = (max_reply_id == UINT64_MAX)
                                          ? UINT64_MAX
                                          : max_reply_id + 1U;
            }

            host_reply_state_save_locked(host);
        }
    }

    host->history_override = entries;
    host->history_override_count = entry_count;
    host_state_save_locked(host);
    host->history_override = nullptr;
    host->history_override_count = 0U;

    pthread_mutex_unlock(&host->lock);

    if (first_removed != nullptr) {
        *first_removed = local_first;
    }
    if (last_removed != nullptr) {
        *last_removed = local_last;
    }
    if (replies_removed != nullptr) {
        *replies_removed = reply_removed;
    }

    GC_FREE(entries);
    return removed;
}

static bool host_replies_find_entry_by_id(host_t *host, uint64_t reply_id,
                                          chat_reply_entry_t *entry)
{
    if (host == nullptr || entry == nullptr || reply_id == 0U) {
        return false;
    }

    bool found = false;

    pthread_mutex_lock(&host->lock);
    for (size_t idx = 0U; idx < host->reply_count; ++idx) {
        const chat_reply_entry_t *candidate = &host->replies[idx];
        if (!candidate->in_use) {
            continue;
        }
        if (candidate->reply_id != reply_id) {
            continue;
        }

        *entry = *candidate;
        found = true;
        break;
    }
    pthread_mutex_unlock(&host->lock);

    return found;
}

static void chat_history_entry_prepare_user(chat_history_entry_t *entry,
                                            const session_ctx_t *from,
                                            const char *message,
                                            bool preserve_whitespace)
{
    if (entry == nullptr || from == nullptr) {
        return;
    }

    memset(entry, 0, sizeof(*entry));
    entry->is_user_message = true;
    entry->preserve_whitespace = preserve_whitespace;
    time_t now = time(nullptr);
    if (now != (time_t)-1) {
        entry->created_at = now;
    }
    if (message != nullptr) {
        snprintf(entry->message, sizeof(entry->message), "%s", message);
    }
    const char *username = session_display_name(from);
    const char *raw_username = (from->user_data_loaded &&
                                from->user_data.preferred_nickname[0] != '\0')
                                   ? from->user_data.preferred_nickname
                                   : from->user.name;
    if (raw_username == nullptr || raw_username[0] == '\0') {
        raw_username = username;
    }

    snprintf(entry->username, sizeof(entry->username), "%s", username);
    snprintf(entry->raw_username, sizeof(entry->raw_username), "%s",
             raw_username);
    snprintf(entry->user_ip, sizeof(entry->user_ip), "%s", from->client_ip);
    if (from->user_color_code != nullptr && from->user_color_code[0] != '\0') {
        size_t color_len = strlen(from->user_color_code);
        char *color_copy = GC_MALLOC(color_len + 1U);
        if (color_copy != nullptr) {
            memcpy(color_copy, from->user_color_code, color_len + 1U);
            entry->user_color_code = color_copy;
        }
    } else {
        entry->user_color_code = nullptr;
    }

    if (from->user_highlight_code != nullptr &&
        from->user_highlight_code[0] != '\0') {
        size_t highlight_len = strlen(from->user_highlight_code);
        char *highlight_copy = GC_MALLOC(highlight_len + 1U);
        if (highlight_copy != nullptr) {
            memcpy(highlight_copy, from->user_highlight_code,
                   highlight_len + 1U);
            entry->user_highlight_code = highlight_copy;
        }
    } else {
        entry->user_highlight_code = nullptr;
    }
    entry->user_is_bold = from->user_is_bold;
    snprintf(entry->user_color_name, sizeof(entry->user_color_name), "%s",
             from->user_color_name);
    snprintf(entry->user_highlight_name, sizeof(entry->user_highlight_name),
             "%s", from->user_highlight_name);
    entry->attachment_type = CHAT_ATTACHMENT_NONE;
    entry->message_id = 0U;
}

static bool host_history_commit_entry(host_t *host, chat_history_entry_t *entry,
                                      chat_history_entry_t *stored_entry)
{
    if (host == nullptr || entry == nullptr) {
        return false;
    }

    if (entry->created_at == 0) {
        time_t now = time(nullptr);
        if (now != (time_t)-1) {
            entry->created_at = now;
        }
    }

    if (!host_history_normalize_entry(host, entry)) {
        return false;
    }

    pthread_mutex_lock(&host->lock);
    if (entry->is_user_message) {
        if (host->next_message_id == 0U) {
            host->next_message_id = 1U;
        }
        entry->message_id = host->next_message_id++;
    } else {
        entry->message_id = 0U;
    }

    if (!host_history_append_locked(host, entry)) {
        pthread_mutex_unlock(&host->lock);
        return false;
    }

    if (stored_entry != nullptr) {
        *stored_entry = *entry;
    }

    pthread_mutex_unlock(&host->lock);
    return true;
}

static bool host_replies_commit_entry(host_t *host, chat_reply_entry_t *entry,
                                      chat_reply_entry_t *stored_entry)
{
    if (host == nullptr || entry == nullptr) {
        return false;
    }

    bool committed = false;

    pthread_mutex_lock(&host->lock);
    if (host->reply_count >= SSH_CHATTER_MAX_REPLIES) {
        pthread_mutex_unlock(&host->lock);
        return false;
    }

    uint64_t assigned_id = host->next_reply_id;
    if (assigned_id == 0U || assigned_id == UINT64_MAX) {
        assigned_id = (uint64_t)host->reply_count + 1U;
    }

    entry->reply_id = assigned_id;
    if (assigned_id < UINT64_MAX) {
        host->next_reply_id = assigned_id + 1U;
    } else {
        host->next_reply_id = assigned_id;
    }

    entry->in_use = true;

    size_t slot = host->reply_count;
    host->replies[slot] = *entry;
    host->reply_count = slot + 1U;

    host_reply_state_save_locked(host);

    if (stored_entry != nullptr) {
        *stored_entry = host->replies[slot];
    }

    committed = true;

    pthread_mutex_unlock(&host->lock);
    return committed;
}

static void host_notify_external_clients(host_t *host,
                                         const chat_history_entry_t *entry)
{
    if (host == nullptr || entry == nullptr) {
        return;
    }
    if (host->clients == nullptr) {
        return;
    }
    client_manager_notify_history(host->clients, entry);
}

static bool host_history_record_user(host_t *host, const session_ctx_t *from,
                                     const char *message,
                                     bool preserve_whitespace,
                                     chat_history_entry_t *stored_entry)
{
    if (host == nullptr || from == nullptr || message == nullptr ||
        message[0] == '\0') {
        return false;
    }

    chat_history_entry_t entry;
    chat_history_entry_prepare_user(&entry, from, message, preserve_whitespace);
    return host_history_commit_entry(host, &entry, stored_entry);
}

static bool host_history_record_system(host_t *host, const char *message,
                                       chat_history_entry_t *stored_entry)
{
    if (host == nullptr || message == nullptr || message[0] == '\0') {
        return false;
    }

    chat_history_entry_t entry = {0};
    entry.is_user_message = false;
    snprintf(entry.message, sizeof(entry.message), "%s", message);
    entry.user_color_name[0] = '\0';
    entry.user_highlight_name[0] = '\0';
    entry.attachment_type = CHAT_ATTACHMENT_NONE;
    entry.message_id = 0U;
    time_t now = time(nullptr);
    if (now != (time_t)-1) {
        entry.created_at = now;
    }

    if (!host_history_commit_entry(host, &entry, stored_entry)) {
        return false;
    }

    chat_history_entry_t notification_entry;
    if (stored_entry != nullptr) {
        notification_entry = *stored_entry;
    } else {
        notification_entry = entry;
    }
    host_notify_external_clients(host, &notification_entry);
    return true;
}

static void host_history_cleanup_expired(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    // Get current UTC time
    time_t now = time(nullptr);
    if (now == (time_t)-1) {
        return;
    }

    // Calculate expiration threshold: 3 days ago
    const time_t expiration_threshold = now - (3 * 24 * 60 * 60);

    pthread_mutex_lock(&host->lock);

    if (host->history == nullptr || host->history_count == 0U) {
        pthread_mutex_unlock(&host->lock);
        return;
    }

    // Count how many messages to keep
    size_t write_idx = 0U;
    size_t removed_count = 0U;

    for (size_t idx = 0U; idx < host->history_count; ++idx) {
        chat_history_entry_t *entry = &host->history[idx];

        // Keep messages that are newer than the threshold
        if (entry->created_at >= expiration_threshold) {
            if (write_idx != idx) {
                host->history[write_idx] = host->history[idx];
            }
            write_idx++;
        } else {
            removed_count++;
        }
    }

    // Update the count
    if (removed_count > 0U) {
        host->history_count = write_idx;
        // Also update history_total to reflect the removal
        if (host->history_total > removed_count) {
            host->history_total -= removed_count;
        } else {
            host->history_total = 0U;
        }
    }

    pthread_mutex_unlock(&host->lock);
}

static bool host_history_apply_reaction(host_t *host, uint64_t message_id,
                                        size_t reaction_index,
                                        chat_history_entry_t *updated_entry)
{
    if (host == nullptr || message_id == 0U ||
        reaction_index >= SSH_CHATTER_REACTION_KIND_COUNT) {
        return false;
    }

    bool applied = false;

    pthread_mutex_lock(&host->lock);
    if (host->history == nullptr) {
        pthread_mutex_unlock(&host->lock);
        return false;
    }
    for (size_t idx = 0U; idx < host->history_count; ++idx) {
        chat_history_entry_t *entry = &host->history[idx];
        if (!entry->is_user_message) {
            continue;
        }
        if (entry->message_id != message_id) {
            continue;
        }

        if (entry->reaction_counts[reaction_index] < UINT32_MAX) {
            entry->reaction_counts[reaction_index] += 1U;
        }

        if (updated_entry != nullptr) {
            *updated_entry = *entry;
        }

        host_state_save_locked(host);
        applied = true;
        break;
    }
    pthread_mutex_unlock(&host->lock);

    return applied;
}

static void session_apply_theme_defaults(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    host_t *host = ctx->owner;

    ctx->user_color_code = host->user_theme.userColor;
    ctx->user_highlight_code = host->user_theme.highlight;
    ctx->user_is_bold = host->user_theme.isBold;
    snprintf(ctx->user_color_name, sizeof(ctx->user_color_name), "%s",
             host->default_user_color_name);
    snprintf(ctx->user_highlight_name, sizeof(ctx->user_highlight_name), "%s",
             host->default_user_highlight_name);

    session_apply_system_theme_defaults(ctx);
}

static void session_apply_system_theme_defaults(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    host_t *host = ctx->owner;

    ctx->system_fg_code = host->system_theme.foregroundColor;
    ctx->system_bg_code = host->system_theme.backgroundColor;
    ctx->system_highlight_code = host->system_theme.highlightColor;
    ctx->system_is_bold = host->system_theme.isBold;
    snprintf(ctx->system_fg_name, sizeof(ctx->system_fg_name), "%s",
             host->default_system_fg_name);
    snprintf(ctx->system_bg_name, sizeof(ctx->system_bg_name), "%s",
             host->default_system_bg_name);
    snprintf(ctx->system_highlight_name, sizeof(ctx->system_highlight_name),
             "%s", host->default_system_highlight_name);
    session_force_dark_mode_foreground(ctx);
}

static void session_force_dark_mode_foreground(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    const bool has_name = ctx->system_fg_name[0] != '\0';
    const bool name_is_default =
        has_name && strcasecmp(ctx->system_fg_name, "default") == 0;
    const bool missing_name = !has_name;
    const bool code_is_default = ctx->system_fg_code == nullptr ||
                                 strcmp(ctx->system_fg_code, ANSI_DEFAULT) == 0;

    if (!missing_name && !name_is_default && !code_is_default) {
        return;
    }

    ctx->system_fg_code = ANSI_WHITE;
    snprintf(ctx->system_fg_name, sizeof(ctx->system_fg_name), "%s", "white");
}

static user_preference_t *
host_find_preference_locked(host_t *host, const char *username, const char *ip)
{
    if (host == nullptr || username == nullptr || username[0] == '\0') {
        return nullptr;
    }

    user_preference_t *fallback = nullptr;

    for (size_t idx = 0; idx < SSH_CHATTER_MAX_PREFERENCES; ++idx) {
        user_preference_t *pref = &host->preferences[idx];
        if (!pref->in_use) {
            continue;
        }

        if (strncmp(pref->username, username, SSH_CHATTER_USERNAME_LEN) != 0) {
            continue;
        }

        const bool pref_has_ip = pref->ip[0] != '\0';
        const bool target_has_ip = ip != nullptr && ip[0] != '\0';

        if (pref_has_ip && target_has_ip &&
            strncmp(pref->ip, ip, SSH_CHATTER_IP_LEN) == 0) {
            return pref;
        }

        if (!target_has_ip && !pref_has_ip) {
            return pref;
        }

        if (fallback == nullptr) {
            fallback = pref;
        }
    }

    return fallback;
}

static user_preference_t *host_ensure_preference_locked(host_t *host,
                                                        const char *username,
                                                        const char *ip)
{
    if (host == nullptr || username == nullptr || username[0] == '\0') {
        return nullptr;
    }

    user_preference_t *existing =
        host_find_preference_locked(host, username, ip);
    const bool target_has_ip = ip != nullptr && ip[0] != '\0';
    if (existing != nullptr && (!target_has_ip || existing->ip[0] != '\0')) {
        return existing;
    }

    for (size_t idx = 0; idx < SSH_CHATTER_MAX_PREFERENCES; ++idx) {
        user_preference_t *pref = &host->preferences[idx];
        if (pref->in_use) {
            continue;
        }

        memset(pref, 0, sizeof(*pref));
        pref->in_use = true;
        pref->last_poll_choice = -1;
        snprintf(pref->camouflage_language, sizeof(pref->camouflage_language),
                 "c");
        snprintf(pref->username, sizeof(pref->username), "%s", username);
        if (target_has_ip) {
            snprintf(pref->ip, sizeof(pref->ip), "%s", ip);
        }
        if (host->preference_count < SSH_CHATTER_MAX_PREFERENCES) {
            ++host->preference_count;
        }
        return pref;
    }

    return existing;
}

static void host_store_user_theme(host_t *host, session_ctx_t *ctx)
{
    if (host == nullptr || ctx == nullptr) {
        return;
    }

    pthread_mutex_lock(&host->lock);
    user_preference_t *pref =
        host_ensure_preference_locked(host, ctx->user.name, "");
    if (pref != nullptr) {
        pref->has_user_theme = true;
        snprintf(pref->user_color_code, sizeof(pref->user_color_code), "%s",
                 ctx->user_color_code != nullptr ? ctx->user_color_code : "");
        snprintf(pref->user_highlight_code, sizeof(pref->user_highlight_code),
                 "%s",
                 ctx->user_highlight_code != nullptr ? ctx->user_highlight_code
                                                     : "");
        snprintf(pref->user_color_name, sizeof(pref->user_color_name), "%s",
                 ctx->user_color_name);
        snprintf(pref->user_highlight_name, sizeof(pref->user_highlight_name),
                 "%s", ctx->user_highlight_name);
        pref->user_is_bold = ctx->user_is_bold;
    }
    if (ctx->user_data_loaded) {
        ctx->user_data.has_user_theme = 1U;
        ctx->user_data.user_is_bold = ctx->user_is_bold ? 1U : 0U;
        snprintf(ctx->user_data.user_color_code,
                 sizeof(ctx->user_data.user_color_code), "%s",
                 ctx->user_color_code != nullptr ? ctx->user_color_code : "");
        snprintf(ctx->user_data.user_highlight_code,
                 sizeof(ctx->user_data.user_highlight_code), "%s",
                 ctx->user_highlight_code != nullptr ? ctx->user_highlight_code
                                                     : "");
        snprintf(ctx->user_data.user_color_name,
                 sizeof(ctx->user_data.user_color_name), "%s",
                 ctx->user_color_name);
        snprintf(ctx->user_data.user_highlight_name,
                 sizeof(ctx->user_data.user_highlight_name), "%s",
                 ctx->user_highlight_name);
        (void)session_user_data_commit(ctx);
    }
    host_state_save_locked(host);
    pthread_mutex_unlock(&host->lock);
}

static void host_store_system_theme(host_t *host, const session_ctx_t *ctx)
{
    if (host == nullptr || ctx == nullptr) {
        return;
    }

    pthread_mutex_lock(&host->lock);
    user_preference_t *pref =
        host_ensure_preference_locked(host, ctx->user.name, "");
    if (pref != nullptr) {
        pref->has_system_theme = true;
        snprintf(pref->system_fg_name, sizeof(pref->system_fg_name), "%s",
                 ctx->system_fg_name);
        snprintf(pref->system_bg_name, sizeof(pref->system_bg_name), "%s",
                 ctx->system_bg_name);
        snprintf(pref->system_highlight_name,
                 sizeof(pref->system_highlight_name), "%s",
                 ctx->system_highlight_name);
        pref->system_is_bold = ctx->system_is_bold;
    }
    host_state_save_locked(host);
    pthread_mutex_unlock(&host->lock);
}

static void host_store_user_os(host_t *host, const session_ctx_t *ctx)
{
    if (host == nullptr || ctx == nullptr) {
        return;
    }

    pthread_mutex_lock(&host->lock);
    user_preference_t *pref =
        host_ensure_preference_locked(host, ctx->user.name, "");
    if (pref != nullptr) {
        snprintf(pref->os_name, sizeof(pref->os_name), "%s", ctx->os_name);
    }
    host_state_save_locked(host);
    pthread_mutex_unlock(&host->lock);
}

static void host_store_birthday(host_t *host, const session_ctx_t *ctx,
                                const char *birthday)
{
    if (host == nullptr || ctx == nullptr || birthday == nullptr) {
        return;
    }

    pthread_mutex_lock(&host->lock);
    user_preference_t *pref =
        host_ensure_preference_locked(host, ctx->user.name, "");
    if (pref != nullptr) {
        pref->has_birthday = true;
        snprintf(pref->birthday, sizeof(pref->birthday), "%s", birthday);
    }
    host_state_save_locked(host);
    host_refresh_motd_locked(host);
    pthread_mutex_unlock(&host->lock);
}

static void host_store_chat_spacing(host_t *host, const session_ctx_t *ctx)
{
    if (host == nullptr || ctx == nullptr) {
        return;
    }

    pthread_mutex_lock(&host->lock);
    user_preference_t *pref =
        host_ensure_preference_locked(host, ctx->user.name, "");
    if (pref != nullptr) {
        if (ctx->translation_caption_spacing > UINT8_MAX) {
            pref->translation_caption_spacing = UINT8_MAX;
        } else {
            pref->translation_caption_spacing =
                (uint8_t)ctx->translation_caption_spacing;
        }
    }
    host_state_save_locked(host);
    pthread_mutex_unlock(&host->lock);
}

static void host_store_translation_preferences(host_t *host,
                                               const session_ctx_t *ctx)
{
    if (host == nullptr || ctx == nullptr) {
        return;
    }

    pthread_mutex_lock(&host->lock);
    user_preference_t *pref =
        host_ensure_preference_locked(host, ctx->user.name, "");
    if (pref != nullptr) {
        pref->translation_master_enabled = ctx->translation_enabled;
        pref->translation_master_explicit = true;
        pref->output_translation_enabled = ctx->output_translation_enabled;
        pref->input_translation_enabled = ctx->input_translation_enabled;
        snprintf(pref->output_translation_language,
                 sizeof(pref->output_translation_language), "%s",
                 ctx->output_translation_language);
        snprintf(pref->input_translation_language,
                 sizeof(pref->input_translation_language), "%s",
                 ctx->input_translation_language);
    }
    host_state_save_locked(host);
    pthread_mutex_unlock(&host->lock);
}

static void host_store_breaking_alerts(host_t *host, const session_ctx_t *ctx)
{
    if (host == nullptr || ctx == nullptr) {
        return;
    }

    pthread_mutex_lock(&host->lock);
    user_preference_t *pref =
        host_ensure_preference_locked(host, ctx->user.name, "");
    if (pref != nullptr) {
        pref->breaking_alerts_enabled = ctx->breaking_alerts_enabled;
    }
    host_state_save_locked(host);
    pthread_mutex_unlock(&host->lock);
}

void host_store_ui_language(host_t *host, const session_ctx_t *ctx)
{
    if (host == nullptr || ctx == nullptr) {
        return;
    }

    pthread_mutex_lock(&host->lock);
    user_preference_t *pref =
        host_ensure_preference_locked(host, ctx->user.name, ctx->client_ip);
    if (pref != nullptr) {
        const char *code = session_ui_language_code(ctx->ui_language);
        snprintf(pref->ui_language, sizeof(pref->ui_language), "%s", code);
        if (ctx->client_ip[0] != '\0') {
            snprintf(pref->ip, sizeof(pref->ip), "%s", ctx->client_ip);
        }
        char label[SSH_CHATTER_PROVIDER_LABEL_LEN];
        if (session_detect_provider_ip(ctx->client_ip, label, sizeof(label))) {
            snprintf(pref->provider_label, sizeof(pref->provider_label), "%s",
                     label);
        } else {
            pref->provider_label[0] = '\0';
        }
    }
    host_state_save_locked(host);
    host_ui_language_state_save_locked(host);
    pthread_mutex_unlock(&host->lock);
}

static bool host_ip_has_grant_locked(host_t *host, const char *ip)
{
    if (host == nullptr || ip == nullptr || ip[0] == '\0') {
        return false;
    }

    for (size_t idx = 0U; idx < host->operator_grant_count; ++idx) {
        if (strncmp(host->operator_grants[idx].ip, ip, SSH_CHATTER_IP_LEN) ==
            0) {
            return true;
        }
    }

    return false;
}

static bool host_add_operator_grant_locked(host_t *host, const char *ip)
{
    if (host == nullptr || ip == nullptr || ip[0] == '\0') {
        return false;
    }

    if (host_ip_has_grant_locked(host, ip)) {
        return true;
    }

    if (host->operator_grant_count >= SSH_CHATTER_MAX_GRANTS) {
        return false;
    }

    snprintf(host->operator_grants[host->operator_grant_count].ip,
             sizeof(host->operator_grants[host->operator_grant_count].ip), "%s",
             ip);
    ++host->operator_grant_count;
    return true;
}

static bool host_ip_has_grant(host_t *host, const char *ip)
{
    if (host == nullptr || ip == nullptr || ip[0] == '\0') {
        return false;
    }

    bool result = false;
    pthread_mutex_lock(&host->lock);
    result = host_ip_has_grant_locked(host, ip);
    pthread_mutex_unlock(&host->lock);
    return result;
}

static void host_apply_grant_to_ip(host_t *host, const char *ip)
{
    if (host == nullptr || ip == nullptr || ip[0] == '\0') {
        return;
    }

    session_ctx_t **matches = nullptr;
    size_t match_count = 0U;

    pthread_mutex_lock(&host->room.lock);
    if (host->room.member_count > 0U) {
        matches = GC_CALLOC(host->room.member_count, sizeof(*matches));
        if (matches != nullptr) {
            for (size_t idx = 0U; idx < host->room.member_count; ++idx) {
                session_ctx_t *member = host->room.members[idx];
                if (member == nullptr) {
                    continue;
                }
                if (strncmp(member->client_ip, ip, SSH_CHATTER_IP_LEN) != 0) {
                    continue;
                }
                member->user.is_operator = true;
                member->auth.is_operator = true;
                matches[match_count++] = member;
            }
        }
    }
    pthread_mutex_unlock(&host->room.lock);

    if (matches == nullptr) {
        return;
    }

    for (size_t idx = 0U; idx < match_count; ++idx) {
        session_ctx_t *member = matches[idx];
        session_send_system_line(
            member, "Operator privileges granted for your IP address.");
    }
}

static bool host_remove_operator_grant_locked(host_t *host, const char *ip)
{
    if (host == nullptr || ip == nullptr || ip[0] == '\0') {
        return false;
    }

    for (size_t idx = 0U; idx < host->operator_grant_count; ++idx) {
        if (strncmp(host->operator_grants[idx].ip, ip, SSH_CHATTER_IP_LEN) !=
            0) {
            continue;
        }

        for (size_t shift = idx; shift + 1U < host->operator_grant_count;
             ++shift) {
            host->operator_grants[shift] = host->operator_grants[shift + 1U];
        }
        memset(&host->operator_grants[host->operator_grant_count - 1U], 0,
               sizeof(host->operator_grants[host->operator_grant_count - 1U]));
        --host->operator_grant_count;
        return true;
    }

    return false;
}

static void host_revoke_grant_from_ip(host_t *host, const char *ip)
{
    if (host == nullptr || ip == nullptr || ip[0] == '\0') {
        return;
    }

    session_ctx_t **matches = nullptr;
    size_t match_count = 0U;

    pthread_mutex_lock(&host->room.lock);
    if (host->room.member_count > 0U) {
        session_ctx_t **allocated =
            GC_CALLOC(host->room.member_count, sizeof(*allocated));
        if (allocated != nullptr) {
            matches = allocated;
        }

        for (size_t idx = 0U; idx < host->room.member_count; ++idx) {
            session_ctx_t *member = host->room.members[idx];
            if (member == nullptr) {
                continue;
            }
            if (strncmp(member->client_ip, ip, SSH_CHATTER_IP_LEN) != 0) {
                continue;
            }
            if (member->user.is_lan_operator) {
                continue;
            }

            member->user.is_operator = false;
            member->auth.is_operator = false;

            if (matches != nullptr) {
                matches[match_count++] = member;
            }
        }
    }
    pthread_mutex_unlock(&host->room.lock);

    if (matches == nullptr) {
        return;
    }

    for (size_t idx = 0U; idx < match_count; ++idx) {
        session_ctx_t *member = matches[idx];
        if (member == nullptr) {
            continue;
        }
        session_send_system_line(
            member, "Operator privileges revoked for your IP address.");
    }
}

static bool host_lookup_user_os(host_t *host, const char *username,
                                char *buffer, size_t length)
{
    if (host == nullptr || username == nullptr || buffer == nullptr ||
        length == 0U) {
        return false;
    }

    bool found = false;

    pthread_mutex_lock(&host->lock);
    user_preference_t *pref = host_find_preference_locked(host, username, "");
    if (pref != nullptr && pref->os_name[0] != '\0') {
        snprintf(buffer, length, "%s", pref->os_name);
        found = true;
    }
    pthread_mutex_unlock(&host->lock);

    if (found) {
        return true;
    }

    session_ctx_t *session = chat_room_find_user(&host->room, username);
    if (session != nullptr && session->os_name[0] != '\0') {
        snprintf(buffer, length, "%s", session->os_name);
        return true;
    }

    return false;
}

static void host_history_strip_empty_lines(chat_history_entry_t *entry)
{
    if (entry == nullptr) {
        return;
    }

    char cleaned[sizeof(entry->message)] = {0};
    size_t write_idx = 0U;
    const char *cursor = entry->message;

    while (*cursor != '\0') {
        const char *line_end = strchr(cursor, '\n');
        size_t line_len =
            (line_end != nullptr) ? (size_t)(line_end - cursor) : strlen(cursor);

        bool has_content = false;
        for (size_t idx = 0U; idx < line_len; ++idx) {
            if (!isspace((unsigned char)cursor[idx])) {
                has_content = true;
                break;
            }
        }

        if (has_content) {
            size_t copy_len = line_len;
            if (write_idx + copy_len >= sizeof(cleaned)) {
                copy_len = sizeof(cleaned) - 1U - write_idx;
            }

            memcpy(cleaned + write_idx, cursor, copy_len);
            write_idx += copy_len;

            if (line_end != nullptr && write_idx + 1U < sizeof(cleaned)) {
                cleaned[write_idx++] = '\n';
            }
        }

        if (line_end == nullptr) {
            break;
        }
        cursor = line_end + 1;
    }

    cleaned[write_idx] = '\0';
    snprintf(entry->message, sizeof(entry->message), "%s", cleaned);
}

static bool host_history_has_content(const chat_history_entry_t *entry)
{
    if (entry == nullptr) {
        return false;
    }

    if (entry->message[0] != '\0') {
        return true;
    }

    if (entry->attachment_type != CHAT_ATTACHMENT_NONE &&
        entry->attachment_target[0] != '\0') {
        return true;
    }

    return false;
}

static bool host_history_normalize_entry(host_t *host,
                                         chat_history_entry_t *entry)
{
    if (host == nullptr || entry == nullptr) {
        return false;
    }

    host_history_strip_empty_lines(entry);

    if (!host_history_has_content(entry)) {
        return false;
    }


    if (!entry->is_user_message) {
        entry->user_color_code = nullptr;
        entry->user_highlight_code = nullptr;
        entry->user_is_bold = false;
        entry->user_color_name[0] = '\0';
        entry->user_highlight_name[0] = '\0';
        return true;
    }

    const bool has_color_code =
        entry->user_color_code != nullptr && entry->user_color_code[0] != '\0';
    const bool has_highlight_code = entry->user_highlight_code != nullptr &&
                                    entry->user_highlight_code[0] != '\0';

    if (!has_color_code) {
        const char *color_code = lookup_color_code(
            USER_COLOR_MAP, sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
            entry->user_color_name);
        if (color_code == nullptr) {
            color_code = host->user_theme.userColor;
            snprintf(entry->user_color_name, sizeof(entry->user_color_name),
                     "%s", host->default_user_color_name);
        }
        entry->user_color_code = color_code;
    }

    if (!has_highlight_code) {
        const char *highlight_code = lookup_color_code(
            HIGHLIGHT_COLOR_MAP,
            sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
            entry->user_highlight_name);
        if (highlight_code == nullptr) {
            highlight_code = host->user_theme.highlight;
            snprintf(entry->user_highlight_name,
                     sizeof(entry->user_highlight_name), "%s",
                     host->default_user_highlight_name);
        }
        entry->user_highlight_code = highlight_code;
    }

    return true;
}

static void host_security_configure(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    atomic_store(&host->security_filter_enabled, false);
    atomic_store(&host->security_filter_failure_logged, false);
    atomic_store(&host->security_ai_enabled, false);
    atomic_store(&host->security_clamav_enabled, false);
    atomic_store(&host->security_clamav_failure_logged, false);
    host->security_clamav_command[0] = '\0';

    const char *toggle = getenv("CHATTER_SECURITY_FILTER");
    if (toggle != nullptr && toggle[0] != '\0') {
        if (strcasecmp(toggle, "0") == 0 || strcasecmp(toggle, "false") == 0 ||
            strcasecmp(toggle, "off") == 0) {
            return;
        }
    }

    bool pipeline_enabled = false;

    const char *clamav_toggle = getenv("CHATTER_CLAMAV");
    bool clamav_disabled = false;
    if (clamav_toggle != nullptr && clamav_toggle[0] != '\0') {
        if (strcasecmp(clamav_toggle, "0") == 0 ||
            strcasecmp(clamav_toggle, "false") == 0 ||
            strcasecmp(clamav_toggle, "off") == 0) {
            clamav_disabled = true;
        }
    }

    if (!clamav_disabled) {
        const char *command = getenv("CHATTER_CLAMAV_COMMAND");
        if (command == nullptr || command[0] == '\0') {
            command = "clamscan --no-summary --stdout .";
        }

        size_t command_length = strlen(command);
        if (command_length < sizeof(host->security_clamav_command)) {
            snprintf(host->security_clamav_command,
                     sizeof(host->security_clamav_command), "%s", command);
            atomic_store(&host->security_clamav_enabled, true);
            pipeline_enabled = true;
        }
    }

    bool ai_requested = false;
    const char *ai_toggle = getenv("CHATTER_SECURITY_AI");
    if (ai_toggle != nullptr && ai_toggle[0] != '\0') {
        if (!(strcasecmp(ai_toggle, "0") == 0 ||
              strcasecmp(ai_toggle, "false") == 0 ||
              strcasecmp(ai_toggle, "off") == 0)) {
            ai_requested = true;
        }
    }

    if (ai_requested) {
        bool has_gemini = false;
        const char *gemini_key = getenv("GEMINI_API_KEY");
        if (gemini_key != nullptr && gemini_key[0] != '\0') {
            has_gemini = true;
        }

        atomic_store(&host->security_ai_enabled, true);
        pipeline_enabled = true;

        const char *message = has_gemini
                                  ? "[security] AI payload moderation enabled "
                                    "(Gemini primary, Ollama fallback)"
                                  : "[security] AI payload moderation enabled "
                                    "(Ollama fallback only)";

        printf("%s\n", message);
    } else {
        printf("[security] AI payload moderation disabled (set "
               "CHATTER_SECURITY_AI=on to enable)\n");
    }

    if (pipeline_enabled) {
        atomic_store(&host->security_filter_enabled, true);
    }
}

static void host_security_disable_filter(host_t *host, const char *reason)
{
    if (host == nullptr) {
        return;
    }

    if (!atomic_exchange(&host->security_ai_enabled, false)) {
        return;
    }

    if (reason == nullptr || reason[0] == '\0') {
        reason = "moderation failure";
    }

    if (!atomic_exchange(&host->security_filter_failure_logged, true)) {
        printf("[security] disabling payload moderation: %s\n", reason);
    }

    if (!atomic_load(&host->security_clamav_enabled)) {
        atomic_store(&host->security_filter_enabled, false);
    }
}

static void host_security_disable_clamav(host_t *host, const char *reason)
{
    if (host == nullptr) {
        return;
    }

    if (!atomic_exchange(&host->security_clamav_enabled, false)) {
        return;
    }

    if (reason == nullptr || reason[0] == '\0') {
        reason = "ClamAV failure";
    }

    if (!atomic_exchange(&host->security_clamav_failure_logged, true)) {
        printf("[security] disabling ClamAV scanning: %s\n", reason);
    }

    if (!atomic_load(&host->security_ai_enabled)) {
        atomic_store(&host->security_filter_enabled, false);
    }
}

static void host_security_compact_whitespace(char *text)
{
    if (text == nullptr) {
        return;
    }

    size_t read_index = 0U;
    size_t write_index = 0U;
    bool previous_was_space = false;

    while (text[read_index] != '\0') {
        unsigned char ch = (unsigned char)text[read_index++];
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            ch = ' ';
        } else if (ch < 0x20U || ch == 0x7FU) {
            ch = ' ';
        }

        if (ch == ' ') {
            if (previous_was_space) {
                continue;
            }
            previous_was_space = true;
            text[write_index++] = ' ';
        } else {
            previous_was_space = false;
            text[write_index++] = (char)ch;
        }
    }

    if (write_index > 0U && text[write_index - 1U] == ' ') {
        --write_index;
    }

    text[write_index] = '\0';
}

static double host_elapsed_seconds(const struct timespec *start,
                                   const struct timespec *end)
{
    double sec = (double)end->tv_sec - (double)start->tv_sec;
    double nsec_to_sec =
        ((double)end->tv_nsec - (double)start->tv_nsec) / 1000000000.0;

    return sec + nsec_to_sec;
}

static bool host_security_execute_clamav_backend(host_t *host, char *notice,
                                                 size_t notice_length)
{
    if (notice != nullptr && notice_length > 0U) {
        notice[0] = '\0';
    }

    if (host == nullptr || notice == nullptr || notice_length == 0U) {
        return false;
    }

    if (!atomic_load(&host->security_clamav_enabled)) {
        return false;
    }

    if (host->security_clamav_command[0] == '\0') {
        return false;
    }

    struct timespec start = {0, 0};
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        start.tv_sec = 0;
        start.tv_nsec = 0;
    }

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        int error_code = errno;
        char reason[128];
        snprintf(reason, sizeof(reason), "%s", strerror(error_code));
        snprintf(
            notice, notice_length,
            "* [security] Scheduled ClamAV scan failed to create pipe (%s).",
            reason);
        host_security_disable_clamav(host, reason);
        return true;
    }

    pid_t pid = fork();
    if (pid == -1) {
        int error_code = errno;
        char reason[128];
        snprintf(reason, sizeof(reason), "%s", strerror(error_code));
        snprintf(notice, notice_length,
                 "* [security] Scheduled ClamAV scan fork() failed (%s).",
                 reason);
        host_security_disable_clamav(host, reason);
        int saved_errno = errno;
        int close_result = 0;
        do {
            close_result = close(pipefd[0]);
        } while (close_result != 0 && errno == EINTR);
        if (close_result != 0) {
            errno = saved_errno;
        }
        saved_errno = errno;
        do {
            close_result = close(pipefd[1]);
        } while (close_result != 0 && errno == EINTR);
        if (close_result != 0) {
            errno = saved_errno;
        }
        return true;
    }

    if (pid == 0) {
        // child process: redirect stdout/stderr to pipe
        int saved_errno = errno;
        int close_result = 0;
        do {
            close_result = close(pipefd[0]);
        } while (close_result != 0 && errno == EINTR);
        if (close_result != 0) {
            fprintf(stderr, "[security] close() failed in child: %s\n",
                    strerror(errno));
            _exit(126);
        }
        errno = saved_errno;

        if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
            fprintf(stderr, "[security] dup2() to STDOUT failed: %s\n",
                    strerror(errno));
            _exit(126);
        }
        if (dup2(pipefd[1], STDERR_FILENO) == -1) {
            fprintf(stderr, "[security] dup2() to STDERR failed: %s\n",
                    strerror(errno));
            _exit(126);
        }

        do {
            close_result = close(pipefd[1]);
        } while (close_result != 0 && errno == EINTR);
        if (close_result != 0) {
            fprintf(stderr, "[security] close() on pipe failed: %s\n",
                    strerror(errno));
            _exit(126);
        }

        // execute clamscan without shell parsing issues
        const char *argv[] = {"sh", "-c", host->security_clamav_command,
                              nullptr};
        execvp(argv[0], (char *const *)argv);

        // only runs if exec failed
        fprintf(stderr, "[security] execvp() failed: %s\n", strerror(errno));
        _exit(127);
    }

    // parent process: turn pipefd[0] into a FILE* for compatibility
    int saved_errno = errno;
    int close_result = 0;
    do {
        close_result = close(pipefd[1]);
    } while (close_result != 0 && errno == EINTR);
    if (close_result != 0) {
        int error_code = errno;
        errno = saved_errno;
        char reason[128];
        snprintf(reason, sizeof(reason), "%s", strerror(error_code));
        snprintf(notice, notice_length,
                 "* [security] Scheduled ClamAV scan close() failed (%s).",
                 reason);
        host_security_disable_clamav(host, reason);
        do {
            close_result = close(pipefd[0]);
        } while (close_result != 0 && errno == EINTR);
        return true;
    }
    errno = saved_errno;
    FILE *pipe = fdopen(pipefd[0], "r");
    if (!pipe) {
        int error_code = errno;
        char reason[128];
        snprintf(reason, sizeof(reason), "%s", strerror(error_code));
        snprintf(notice, notice_length,
                 "* [security] Scheduled ClamAV scan fdopen() failed (%s).",
                 reason);
        host_security_disable_clamav(host, reason);
        close(pipefd[0]);
        return true;
    }

    char output[SSH_CHATTER_CLAMAV_OUTPUT_LIMIT];
    output[0] = '\0';
    size_t output_length = 0U;

    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        size_t chunk = strlen(buffer);
        if (chunk == 0U) {
            continue;
        }
        if (output_length + chunk >= sizeof(output)) {
            chunk = sizeof(output) - output_length - 1U;
        }
        if (chunk == 0U) {
            break;
        }
        memcpy(output + output_length, buffer, chunk);
        output_length += chunk;
        output[output_length] = '\0';
    }

    int status = -1;
    do {
        errno = 0;
        status = fclose(pipe);
    } while (status == -1 && errno == EINTR);
    struct timespec end = {0, 0};
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        end.tv_sec = 0;
        end.tv_nsec = 0;
    }
    host->security_clamav_last_run = end;
    struct timespec elapsed = timespec_diff(&end, &start);
    double seconds =
        (double)elapsed.tv_sec + (double)elapsed.tv_nsec / 1000000000.0;

    host_security_compact_whitespace(output);

    if (status == -1) {
        int error_code = errno;
        if (error_code != 0) {
            snprintf(
                notice, notice_length,
                "* [security] Scheduled ClamAV scan failed (unable to retrieve "
                "status: %s).",
                strerror(error_code));
        } else {
            snprintf(
                notice, notice_length,
                "* [security] Scheduled ClamAV scan failed (unable to retrieve "
                "status).");
        }
        host_security_disable_clamav(
            host, "unable to retrieve scheduled ClamAV status");
        return true;
    }

    if (!WIFEXITED(status)) {
        snprintf(notice, notice_length,
                 "* [security] Scheduled ClamAV scan terminated unexpectedly.");
        host_security_disable_clamav(
            host, "scheduled ClamAV scan terminated unexpectedly");
        return true;
    }

    int exit_code = WEXITSTATUS(status);
    if (exit_code == 0) {
        return true;
    }

    if (exit_code == 1) {
        if (output[0] != '\0') {
            snprintf(
                notice, notice_length,
                "* [security] Scheduled ClamAV scan finished in %.1fs (issues "
                "found): %s",
                seconds, output);
        } else {
            snprintf(
                notice, notice_length,
                "* [security] Scheduled ClamAV scan finished in %.1fs (issues "
                "found).",
                seconds);
        }
        return true;
    }

    if (output[0] != '\0') {
        snprintf(notice, notice_length,
                 "* [security] Scheduled ClamAV scan failed in %.1fs (exit "
                 "code %d): %s",
                 seconds, exit_code, output);
    } else {
        snprintf(notice, notice_length,
                 "* [security] Scheduled ClamAV scan failed in %.1fs (exit "
                 "code %d).",
                 seconds, exit_code);
    }
    host_security_disable_clamav(host,
                                 "scheduled ClamAV scan returned an error");
    return true;
}

static void *host_security_clamav_backend(void *arg)
{
    host_t *host = (host_t *)arg;
    if (host == nullptr) {
        return nullptr;
    }

    sshc_memory_context_t *memory_scope =
        sshc_memory_context_push(host->memory_context);

    atomic_store(&host->security_clamav_thread_running, true);
    printf("[security] scheduled ClamAV backend thread started (interval: %u "
           "seconds)\n",
           (unsigned int)SSH_CHATTER_CLAMAV_SCAN_INTERVAL_SECONDS);

    while (!atomic_load(&host->security_clamav_thread_stop)) {
        if (atomic_load(&host->security_clamav_enabled) &&
            host->security_clamav_command[0] != '\0') {
            char notice[SSH_CHATTER_MESSAGE_LIMIT];
            if (host_security_execute_clamav_backend(host, notice,
                                                     sizeof(notice)) &&
                notice[0] != '\0') {
                printf("%s\n", notice);
                host_history_record_system(host, notice, nullptr);
                chat_room_broadcast(&host->room, notice, nullptr);
            }
        }

        unsigned int remaining = SSH_CHATTER_CLAMAV_SCAN_INTERVAL_SECONDS;
        while (remaining > 0U &&
               !atomic_load(&host->security_clamav_thread_stop)) {
            unsigned int chunk =
                remaining > SSH_CHATTER_CLAMAV_SLEEP_CHUNK_SECONDS
                    ? SSH_CHATTER_CLAMAV_SLEEP_CHUNK_SECONDS
                    : remaining;
            struct timespec pause_duration = {
                .tv_sec = (time_t)chunk,
                .tv_nsec = 0,
            };
            host_sleep_uninterruptible(&pause_duration);
            if (remaining < chunk) {
                remaining = 0U;
            } else {
                remaining -= chunk;
            }
        }
    }

    atomic_store(&host->security_clamav_thread_running, false);
    printf("[security] scheduled ClamAV backend thread stopped\n");
    sshc_memory_context_pop(memory_scope);
    return nullptr;
}

static void host_security_start_clamav_backend(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (host->security_clamav_thread_initialized) {
        return;
    }

    if (!atomic_load(&host->security_clamav_enabled)) {
        return;
    }

    if (host->security_clamav_command[0] == '\0') {
        return;
    }

    atomic_store(&host->security_clamav_thread_stop, false);
    atomic_store(&host->security_clamav_thread_running, false);

    int error = pthread_create(&host->security_clamav_thread, nullptr,
                               host_security_clamav_backend, host);
    if (error != 0) {
        printf("[security] failed to start ClamAV backend thread: %s\n",
               strerror(error));
        return;
    }

    host->security_clamav_thread_initialized = true;
}

static bool host_ensure_private_data_path(host_t *host, const char *path,
                                          bool create_directories)
{
    (void)host;
    if (path == nullptr || path[0] == '\0') {
        return false;
    }

    char parent_buffer[PATH_MAX];
    snprintf(parent_buffer, sizeof(parent_buffer), "%s", path);
    char *parent_dir = dirname(parent_buffer);
    if (parent_dir == nullptr || parent_dir[0] == '\0') {
        parent_dir = ".";
    }

    char parent_path[PATH_MAX];
    snprintf(parent_path, sizeof(parent_path), "%s", parent_dir);

    struct stat dir_stat;
    if (stat(parent_path, &dir_stat) != 0) {
        if (!(create_directories && errno == ENOENT)) {
            humanized_log_error("host", "failed to inspect data directory",
                                errno != 0 ? errno : EIO);
            return false;
        }

        if (mkdir(parent_path, 0750) != 0 && errno != EEXIST) {
            humanized_log_error("host", "failed to create data directory",
                                errno != 0 ? errno : EIO);
            return false;
        }

        if (stat(parent_path, &dir_stat) != 0) {
            humanized_log_error("host", "failed to inspect data directory",
                                errno != 0 ? errno : EIO);
            return false;
        }
    }

    if (!S_ISDIR(dir_stat.st_mode)) {
        humanized_log_error("host", "data path parent is not a directory",
                            ENOTDIR);
        return false;
    }

    mode_t insecure_bits = dir_stat.st_mode & (S_IWOTH | S_IWGRP);
    bool is_dot = strcmp(parent_path, ".") == 0;
    bool is_root = strcmp(parent_path, "/") == 0;
    if (insecure_bits != 0U) {
        if (!is_dot && !is_root) {
            mode_t tightened = dir_stat.st_mode & (mode_t) ~(S_IWOTH | S_IWGRP);
            if (chmod(parent_path, tightened) != 0) {
                humanized_log_error(
                    "host", "failed to tighten data directory permissions",
                    errno != 0 ? errno : EACCES);
                return false;
            }
        } else {
            humanized_log_error(
                "host", "data directory permissions are too loose", EACCES);
            return false;
        }
    }

    struct stat file_stat;
    if (lstat(path, &file_stat) == 0) {
        if (!S_ISREG(file_stat.st_mode)) {
            humanized_log_error(
                "host", "bbs state path does not reference a regular file",
                EINVAL);
            return false;
        }

        if ((file_stat.st_mode & (S_IWOTH | S_IWGRP)) != 0U) {
            if (chmod(path, S_IRUSR | S_IWUSR) != 0) {
                humanized_log_error("host",
                                    "failed to tighten bbs state permissions",
                                    errno != 0 ? errno : EACCES);
                return false;
            }
        }

        if (file_stat.st_uid != geteuid()) {
            humanized_log_error("host", "bbs state file ownership mismatch",
                                EPERM);
            return false;
        }
    } else if (errno != ENOENT) {
        humanized_log_error("host", "failed to inspect bbs state path",
                            errno != 0 ? errno : EIO);
        return false;
    }

    return true;
}

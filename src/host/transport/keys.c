/**
 * @file host_transport.c
 * @desc File-level documentation for host_transport.c, describing its role
 *       in the SSH-Chatter server and providing a consistent header
 *       comment format across C sources.
 * @return None.
 */

// Connection setup, listener management, and transport helpers.
#include "../internal.h"

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

static void session_channel_log_write_failure(session_ctx_t *ctx,
                                              const char *reason)
{
    if (ctx == nullptr) {
        return;
    }

    if (reason == nullptr || reason[0] == '\0') {
        reason = "transport write failure";
    }

    const char *username =
        ctx->user.name[0] != '\0' ? ctx->user.name : "unknown";
    printf("[session] transport write failure for %s: %s\n", username, reason);
}

static bool session_telnet_write_block(session_ctx_t *ctx,
                                       const unsigned char *data, size_t length)
{
    if (ctx == nullptr || data == nullptr || length == 0U ||
        ctx->telnet_fd < 0) {
        return true;
    }

    while (length > 0U) {
        size_t chunk = length;
        if (chunk > SSH_CHATTER_CHANNEL_WRITE_CHUNK) {
            chunk = SSH_CHATTER_CHANNEL_WRITE_CHUNK;
        }

        /* Expand data for telnet IAC escaping (IAC byte must be doubled) */
        unsigned char buffer[SSH_CHATTER_CHANNEL_WRITE_CHUNK * 2U];
        size_t expanded = 0U;
        for (size_t idx = 0U; idx < chunk; ++idx) {
            unsigned char byte = data[idx];
            buffer[expanded++] = byte;
            if (byte == TELNET_IAC) {
                buffer[expanded++] = TELNET_IAC;
            }
        }

        /* Write entire expanded buffer with proper error handling */
        size_t offset = 0U;
        unsigned int retry_count = 0U;
        const unsigned int max_retries = 3U;

        while (offset < expanded) {
            ssize_t written = send(ctx->telnet_fd, buffer + offset,
                                   expanded - offset, MSG_NOSIGNAL);
            if (written < 0) {
                if (errno == EINTR) {
                    /* Interrupted system call - retry immediately */
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* Socket buffer full - wait and retry */
                    if (retry_count++ >= max_retries) {
                        return false; /* Too many retries */
                    }
                    struct timespec backoff = {
                        .tv_sec = 0,
                        .tv_nsec = 10000000, /* 10ms backoff */
                    };
                    nanosleep(&backoff, NULL);
                    continue;
                }
                /* Other errors are fatal */
                return false;
            }

            if (written == 0) {
                /* Connection closed */
                return false;
            }

            offset += (size_t)written;
            retry_count = 0U; /* Reset retry counter on successful write */
        }

        data += chunk;
        length -= chunk;
    }

    /* Ensure data is pushed to network layer (strong sync point) */
    /* Note: TCP_NODELAY should be set on socket for immediate send */
    int flags = 0;
    socklen_t flags_len = sizeof(flags);
    if (getsockopt(ctx->telnet_fd, IPPROTO_TCP, TCP_NODELAY, &flags,
                   &flags_len) == 0) {
        if (flags == 0) {
            /* TCP_NODELAY not set - force flush with empty MSG_OOB as sync marker */
            /* This ensures message boundaries are preserved */
            (void)send(ctx->telnet_fd, "", 0, MSG_NOSIGNAL);
        }
    }

    return true;
}

#define SSH_CHATTER_MAX_WRITE_TIMEOUT_MS 100

static bool session_channel_wait_writable(session_ctx_t *ctx, int timeout_ms)
{
    if (ctx == nullptr) {
        return false;
    }

    if (timeout_ms > SSH_CHATTER_MAX_WRITE_TIMEOUT_MS) {
        timeout_ms = SSH_CHATTER_MAX_WRITE_TIMEOUT_MS;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        if (ctx->telnet_fd < 0) {
            return false;
        }

        struct pollfd pfd = {
            .fd = ctx->telnet_fd,
            .events = POLLOUT,
            .revents = 0,
        };

        for (;;) {
            int result = poll(&pfd, 1, timeout_ms);
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (result == 0) {
                return false;
            }
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                return false;
            }
            if (pfd.revents & POLLOUT) {
                return true;
            }
            return false;
        }
    }

    if (ctx->session == nullptr) {
        return false;
    }

    int fd = ssh_get_fd(ctx->session);
    if (fd < 0) {
        struct timespec backoff = {
            .tv_sec = 0,
            .tv_nsec = SSH_CHATTER_CHANNEL_WRITE_BACKOFF_NS,
        };
        host_sleep_uninterruptible(&backoff);
        return true;
    }

    int val = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) < 0) {
        fprintf(stderr, "[session] setsockopt SO_KEEPALIVE failed");
    }

    struct pollfd pfd = {
        .fd = fd,
        .events = POLLOUT,
        .revents = 0,
    };

    if (timeout_ms > SSH_CHATTER_MAX_WRITE_TIMEOUT_MS) {
        timeout_ms = SSH_CHATTER_MAX_WRITE_TIMEOUT_MS;
    }

    for (;;) {
        int result = poll(&pfd, 1, timeout_ms);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return false;
        }
        if (pfd.revents & POLLOUT) {
            return true;
        }
        return false;
    }
}

static bool session_channel_write_all(session_ctx_t *ctx, const void *data,
                                      size_t length)
{
    if (ctx == nullptr || data == nullptr || length == 0U ||
        !session_transport_active(ctx)) {
        return true;
    }

    const unsigned char *cursor = (const unsigned char *)data;
    size_t remaining = length;
    unsigned int stalled = 0U;

    while (remaining > 0U) {
        if (!session_channel_wait_writable(
                ctx, SSH_CHATTER_CHANNEL_WRITE_TIMEOUT_MS)) {
            if (++stalled >= SSH_CHATTER_CHANNEL_WRITE_MAX_STALLS) {
                session_channel_log_write_failure(ctx, "write timed out");
                return false;
            }
            continue;
        }

        stalled = 0U;

        size_t chunk = remaining;
        if (chunk > SSH_CHATTER_CHANNEL_WRITE_CHUNK) {
            chunk = SSH_CHATTER_CHANNEL_WRITE_CHUNK;
        }

        if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
            if (!session_telnet_write_block(ctx, cursor, chunk)) {
                session_channel_log_write_failure(ctx, "telnet write error");
                return false;
            }
            cursor += chunk;
            remaining -= chunk;
            continue;
        }

        ssize_t written =
            ssh_channel_write(ctx->channel, cursor, (uint32_t)chunk);
        if (written == SSH_ERROR) {
            const char *error = ssh_get_error(ctx->session);
            session_channel_log_write_failure(
                ctx, (error != nullptr && error[0] != '\0')
                         ? error
                         : "channel write error");
            return false;
        }

        if (written == 0) {
            if (ssh_channel_is_eof(ctx->channel) ||
                !ssh_channel_is_open(ctx->channel)) {
                session_channel_log_write_failure(
                    ctx, "channel closed during write");
                return false;
            }

            if (++stalled >= SSH_CHATTER_CHANNEL_WRITE_MAX_STALLS) {
                session_channel_log_write_failure(ctx, "channel write stalled");
                return false;
            }

            continue;
        }

        cursor += written;
        remaining -= (size_t)written;
    }

    return true;
}

typedef struct cp437_replacement {
    uint32_t codepoint;
    const char *replacement;
} cp437_replacement_t;

static const cp437_replacement_t kCp437AsciiReplacements[] = {
    {0x00A0U, " "},   {0x2013U, "-"},  {0x2014U, "-"},  {0x2015U, "-"},
    {0x2018U, "'"},   {0x2019U, "'"},  {0x201CU, "\""}, {0x201DU, "\""},
    {0x2026U, "..."}, {0x2190U, "<-"}, {0x2191U, "^"},  {0x2192U, "->"},
    {0x2193U, "v"},   {0x21B3U, "->"},
};

static const char *session_cp437_ascii_replacement(uint32_t codepoint)
{
    for (size_t idx = 0U; idx < sizeof(kCp437AsciiReplacements) /
                                    sizeof(kCp437AsciiReplacements[0]);
         ++idx) {
        if (kCp437AsciiReplacements[idx].codepoint == codepoint) {
            return kCp437AsciiReplacements[idx].replacement;
        }
    }
    return nullptr;
}

static char *session_cp437_normalize_utf8(const char *data, size_t length,
                                          size_t *normalized_length)
{
    if (data == nullptr || length == 0U) {
        if (normalized_length != nullptr) {
            *normalized_length = length;
        }
        return nullptr;
    }

    size_t capacity = length + 16U;
    char *buffer = (char *)sshc_gc_malloc(capacity);
    if (buffer == nullptr) {
        if (normalized_length != nullptr) {
            *normalized_length = length;
        }
        return nullptr;
    }

    size_t output = 0U;
    bool modified = false;
    const unsigned char *cursor = (const unsigned char *)data;
    size_t remaining = length;

    while (remaining > 0U) {
        uint32_t codepoint = 0U;
        size_t consumed =
            session_utf8_decode_codepoint(cursor, remaining, &codepoint);
        if (consumed == 0U) {
            codepoint = (uint32_t)(*cursor);
            consumed = 1U;
        }

        const char *replacement = session_cp437_ascii_replacement(codepoint);
        if (replacement != nullptr) {
            modified = true;
            size_t rep_len = strlen(replacement);
            size_t needed = output + rep_len + 1U;
            if (needed > capacity) {
                size_t new_capacity = capacity * 2U;
                if (new_capacity < needed) {
                    new_capacity = needed + 16U;
                }
                char *resized = (char *)sshc_gc_realloc(buffer, new_capacity);
                if (resized == nullptr) {
                    sshc_gc_free(buffer);
                    if (normalized_length != nullptr) {
                        *normalized_length = length;
                    }
                    return nullptr;
                }
                buffer = resized;
                capacity = new_capacity;
            }
            memcpy(buffer + output, replacement, rep_len);
            output += rep_len;
        } else {
            size_t needed = output + consumed + 1U;
            if (needed > capacity) {
                size_t new_capacity = capacity * 2U;
                if (new_capacity < needed) {
                    new_capacity = needed + 16U;
                }
                char *resized = (char *)sshc_gc_realloc(buffer, new_capacity);
                if (resized == nullptr) {
                    sshc_gc_free(buffer);
                    if (normalized_length != nullptr) {
                        *normalized_length = length;
                    }
                    return nullptr;
                }
                buffer = resized;
                capacity = new_capacity;
            }
            memcpy(buffer + output, cursor, consumed);
            output += consumed;
        }

        cursor += consumed;
        remaining -= consumed;
    }

    if (!modified) {
        sshc_gc_free(buffer);
        if (normalized_length != nullptr) {
            *normalized_length = length;
        }
        return nullptr;
    }

    buffer[output] = '\0';
    if (normalized_length != nullptr) {
        *normalized_length = output;
    }
    return buffer;
}

static bool __attribute__((unused))
session_channel_write_cp437(session_ctx_t *ctx, const char *data, size_t length)
{
    if (ctx == nullptr || data == nullptr || length == 0U) {
        return true;
    }

    iconv_t descriptor = iconv_open("CP437//TRANSLIT", "UTF-8");
    if (descriptor == (iconv_t)(-1)) {
        return session_channel_write_all(ctx, data, length);
    }

    size_t capacity = (length > 0U ? length : 1U) * 4U + 16U;
    char *buffer = (char *)sshc_gc_malloc(capacity);
    if (buffer == nullptr) {
        iconv_close(descriptor);
        return session_channel_write_all(ctx, data, length);
    }

    size_t normalized_length = 0U;
    char *normalized =
        session_cp437_normalize_utf8(data, length, &normalized_length);

    const char *input_cursor = normalized != nullptr ? normalized : data;
    size_t input_remaining = normalized != nullptr ? normalized_length : length;
    char *output_cursor = buffer;
    size_t output_remaining = capacity;

    bool fallback_to_plaintext = false;

    while (input_remaining > 0U) {
        size_t result =
            iconv(descriptor, (char **)&input_cursor, &input_remaining,
                  &output_cursor, &output_remaining);
        if (result == (size_t)-1) {
            if (errno == E2BIG) {
                size_t produced = capacity - output_remaining;
                size_t new_capacity = capacity * 2U;
                if (new_capacity <= capacity) {
                    new_capacity = capacity + length + 32U;
                }
                char *resized = (char *)sshc_gc_realloc(buffer, new_capacity);
                if (resized == nullptr) {
                    fallback_to_plaintext = true;
                    goto cleanup;
                }
                buffer = resized;
                output_cursor = buffer + produced;
                output_remaining = new_capacity - produced;
                capacity = new_capacity;
                continue;
            }
            if (errno == EILSEQ || errno == EINVAL) {
                ++input_cursor;
                --input_remaining;
                if (output_remaining == 0U) {
                    size_t produced = capacity - output_remaining;
                    size_t new_capacity = capacity * 2U;
                    if (new_capacity <= capacity) {
                        new_capacity = capacity + length + 32U;
                    }
                    char *resized = (char *)sshc_gc_realloc(buffer, new_capacity);
                    if (resized == nullptr) {
                        fallback_to_plaintext = true;
                        goto cleanup;
                    }
                    buffer = resized;
                    output_cursor = buffer + produced;
                    output_remaining = new_capacity - produced;
                    capacity = new_capacity;
                }
                *output_cursor++ = '?';
                output_remaining -= 1U;
                continue;
            }
            fallback_to_plaintext = true;
            goto cleanup;
        }
    }

cleanup:
    iconv_close(descriptor);
    bool success = false;
    if (fallback_to_plaintext) {
        const char *fallback_data = normalized != nullptr ? normalized : data;
        size_t fallback_length =
            normalized != nullptr ? normalized_length : length;
        success =
            session_channel_write_all(ctx, fallback_data, fallback_length);
    } else {
        size_t produced = capacity - output_remaining;
        success = session_channel_write_all(ctx, buffer, produced);
    }
    sshc_gc_free(buffer);
    if (normalized != nullptr) {
        sshc_gc_free(normalized);
    }
    return success;
}

static bool session_text_has_unicode(const char *data, size_t length)
{
    if (data == nullptr || length == 0U) {
        return false;
    }

    size_t idx = 0U;
    while (idx < length) {
        unsigned char byte = (unsigned char)data[idx];
        if (byte < 0x80U) {
            ++idx;
            continue;
        }

        /* Basic UTF-8 sequence validation to avoid false positives */
        if ((byte & 0xE0U) == 0xC0U && idx + 1U < length &&
            ((unsigned char)data[idx + 1U] & 0xC0U) == 0x80U) {
            return true;
        }
        if ((byte & 0xF0U) == 0xE0U && idx + 2U < length &&
            ((unsigned char)data[idx + 1U] & 0xC0U) == 0x80U &&
            ((unsigned char)data[idx + 2U] & 0xC0U) == 0x80U) {
            return true;
        }
        if ((byte & 0xF8U) == 0xF0U && idx + 3U < length &&
            ((unsigned char)data[idx + 1U] & 0xC0U) == 0x80U &&
            ((unsigned char)data[idx + 2U] & 0xC0U) == 0x80U &&
            ((unsigned char)data[idx + 3U] & 0xC0U) == 0x80U) {
            return true;
        }

        /* Any other high-bit byte is treated as non-ASCII content */
        return true;
    }

    return false;
}

static bool session_channel_write_codepage(session_ctx_t *ctx, const char *data,
                                           size_t length,
                                           session_codepage_t codepage)
{
    if (ctx == nullptr || data == nullptr || length == 0U) {
        return true;
    }

    /* For UTF-8 or unknown codepages, just pass through */
    if (codepage == SESSION_CODEPAGE_UTF8) {
        return session_channel_write_all(ctx, data, length);
    }

    const bool hybrid_auto =
        ctx != nullptr && ctx->cp437_override == SESSION_CP437_OVERRIDE_NONE;
    const bool contains_unicode = session_text_has_unicode(data, length);

    if (hybrid_auto && contains_unicode && !ctx->prefer_cp437_output) {
        /* Hybrid mode: leave Unicode intact when retro is not preferred */
        return session_channel_write_all(ctx, data, length);
    }

    const char *iconv_name = session_codepage_iconv_name(codepage);
    if (iconv_name == nullptr) {
        return session_channel_write_all(ctx, data, length);
    }

    iconv_t descriptor = iconv_open(iconv_name, "UTF-8");
    if (descriptor == (iconv_t)(-1)) {
        return session_channel_write_all(ctx, data, length);
    }

    size_t capacity = (length > 0U ? length : 1U) * 4U + 16U;
    char *buffer = (char *)sshc_gc_malloc(capacity);
    if (buffer == nullptr) {
        iconv_close(descriptor);
        return session_channel_write_all(ctx, data, length);
    }

    /* For CP437, apply normalization. For others, use data as-is */
    size_t normalized_length = 0U;
    char *normalized = nullptr;
    if (codepage == SESSION_CODEPAGE_CP437) {
        normalized =
            session_cp437_normalize_utf8(data, length, &normalized_length);
    }

    const char *input_cursor = normalized != nullptr ? normalized : data;
    size_t input_remaining = normalized != nullptr ? normalized_length : length;
    char *output_cursor = buffer;
    size_t output_remaining = capacity;

    bool fallback_to_plaintext = false;

    while (input_remaining > 0U) {
        size_t result =
            iconv(descriptor, (char **)&input_cursor, &input_remaining,
                  &output_cursor, &output_remaining);
        if (result == (size_t)-1) {
            if (errno == E2BIG) {
                size_t produced = capacity - output_remaining;
                size_t new_capacity = capacity * 2U;
                if (new_capacity <= capacity) {
                    new_capacity = capacity + length + 32U;
                }
                char *resized = (char *)sshc_gc_realloc(buffer, new_capacity);
                if (resized == nullptr) {
                    fallback_to_plaintext = true;
                    goto cleanup;
                }
                buffer = resized;
                output_cursor = buffer + produced;
                output_remaining = new_capacity - produced;
                capacity = new_capacity;
                continue;
            }
            if (errno == EILSEQ || errno == EINVAL) {
                ++input_cursor;
                --input_remaining;
                if (output_remaining == 0U) {
                    size_t produced = capacity - output_remaining;
                    size_t new_capacity = capacity * 2U;
                    if (new_capacity <= capacity) {
                        new_capacity = capacity + length + 32U;
                    }
                    char *resized = (char *)sshc_gc_realloc(buffer, new_capacity);
                    if (resized == nullptr) {
                        fallback_to_plaintext = true;
                        goto cleanup;
                    }
                    buffer = resized;
                    output_cursor = buffer + produced;
                    output_remaining = new_capacity - produced;
                    capacity = new_capacity;
                }
                *output_cursor++ = '?';
                output_remaining -= 1U;
                continue;
            }
            fallback_to_plaintext = true;
            goto cleanup;
        }
    }

cleanup:
    iconv_close(descriptor);
    bool success = false;
    if (fallback_to_plaintext) {
        const char *fallback_data = normalized != nullptr ? normalized : data;
        size_t fallback_length =
            normalized != nullptr ? normalized_length : length;
        success =
            session_channel_write_all(ctx, fallback_data, fallback_length);
    } else {
        size_t produced = capacity - output_remaining;
        success = session_channel_write_all(ctx, buffer, produced);
    }
    sshc_gc_free(buffer);
    if (normalized != nullptr) {
        sshc_gc_free(normalized);
    }
    return success;
}

static bool session_output_lock(session_ctx_t *ctx)
{
    if (ctx == nullptr || !ctx->output_lock_initialized) {
        return false;
    }

    int error = ttak_mutex_lock(&ctx->output_lock);
    if (error != 0) {
        printf("[session] failed to lock output for %s: %s\n",
               (ctx->user.name[0] != '\0') ? ctx->user.name : "unknown",
               strerror(error));
        return false;
    }

    return true;
}

static void session_output_unlock(session_ctx_t *ctx)
{
    if (ctx == nullptr || !ctx->output_lock_initialized) {
        return;
    }

    int error = ttak_mutex_unlock(&ctx->output_lock);
    if (error != 0) {
        printf("[session] failed to unlock output for %s: %s\n",
               (ctx->user.name[0] != '\0') ? ctx->user.name : "unknown",
               strerror(error));
    }
}

// Output buffering functions to prevent flickering
static void session_output_buffer_start(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    ctx->output_buffering_enabled = true;
    ctx->output_buffer_length = 0U;
}

static void session_output_buffer_clear(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    ctx->output_buffer_length = 0U;
}

static void session_output_buffer_flush(session_ctx_t *ctx)
{
    if (ctx == nullptr || !ctx->output_buffering_enabled) {
        return;
    }

    bool locked = session_output_lock(ctx);
    if (ctx->output_buffer_length > 0U) {
        // Temporarily disable buffering to avoid infinite recursion
        ctx->output_buffering_enabled = false;
        session_channel_write(ctx, ctx->output_buffer,
                              ctx->output_buffer_length);
        ctx->output_buffer_length = 0U;
        ctx->output_buffering_enabled = true;
    }
    if (locked) {
        session_output_unlock(ctx);
    }
}

static void session_output_buffer_stop(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    session_output_buffer_flush(ctx);
    session_output_buffer_clear(ctx);
    ctx->output_buffering_enabled = false;
}

static bool session_output_buffer_append(session_ctx_t *ctx, const void *data,
                                         size_t length)
{
    if (ctx == nullptr || data == nullptr || length == 0U) {
        return false;
    }

    // Check if buffer has enough space
    if (ctx->output_buffer_length + length > SSH_CHATTER_OUTPUT_BUFFER_SIZE) {
        // Buffer is full, flush it first
        session_output_buffer_flush(ctx);
        session_output_buffer_clear(ctx);

        // If still not enough space after flush, this write is too large
        if (length > SSH_CHATTER_OUTPUT_BUFFER_SIZE) {
            // Write directly without buffering
            ctx->output_buffering_enabled = false;
            session_channel_write(ctx, data, length);
            ctx->output_buffering_enabled = true;
            return true;
        }
    }

    // Append data to buffer
    memcpy(ctx->output_buffer + ctx->output_buffer_length, data, length);
    ctx->output_buffer_length += length;
    return true;
}

static bool session_output_requires_utf8(const char *data, size_t length)
{
    if (data == nullptr || length == 0U) {
        return false;
    }

    mbstate_t state;
    memset(&state, 0, sizeof(state));

    const char *cursor = data;
    size_t remaining = length;
    while (remaining > 0U) {
        wchar_t wc;
        size_t consumed = mbrtowc(&wc, cursor, remaining, &state);
        if (consumed == (size_t)-2 || consumed == (size_t)-1) {
            return true;
        }

        if (consumed == 0U) {
            ++cursor;
            --remaining;
            continue;
        }

        if (consumed > 1U) {
            return true;
        }

        cursor += consumed;
        remaining -= consumed;
    }

    return false;
}



static void session_channel_flush(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    // For Telnet connections, explicitly force a socket flush so chat messages
    // are synchronized immediately (without waiting for subsequent input)
    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        // Flush any pending buffered output first
        session_output_buffer_flush(ctx);

        // Confirm the socket is writable and nudge the TCP stack so any queued
        // bytes are delivered even if the client is idle
        struct pollfd pfd = {
            .fd = ctx->telnet_fd,
            .events = POLLOUT,
            .revents = 0,
        };

        if (poll(&pfd, 1, SSH_CHATTER_CHANNEL_WRITE_TIMEOUT_MS) > 0 &&
            (pfd.revents & POLLOUT) != 0) {
            (void)send(ctx->telnet_fd, "", 0, MSG_NOSIGNAL);
        }
        return;
    }

    // For SSH, flush any pending data in libssh's buffers
    if (ctx->session != nullptr) {
        // Use a short timeout (50ms) to avoid blocking
        ssh_blocking_flush(ctx->session, 50);
    }
}

static bool session_channel_write_utf16(session_ctx_t *ctx, const char *data,
                                        size_t length)
{
    if (ctx == nullptr || data == nullptr) {
        return true;
    }

    size_t idx = 0U;
    while (idx < length) {
        unsigned char byte = (unsigned char)data[idx];
        if (byte == '\033') {
            size_t start = idx++;
            if (idx < length) {
                unsigned char next = (unsigned char)data[idx];
                if (next == '[') {
                    ++idx;
                    while (idx < length) {
                        unsigned char ch = (unsigned char)data[idx++];
                        if (ch >= '@' && ch <= '~') {
                            break;
                        }
                    }
                } else if (next == ']') {
                    ++idx;
                    while (idx < length) {
                        unsigned char ch = (unsigned char)data[idx++];
                        if (ch == '\a') {
                            break;
                        }
                        if (ch == '\033' && idx < length) {
                            unsigned char terminator = (unsigned char)data[idx];
                            if (terminator == '\\') {
                                ++idx;
                                break;
                            }
                        }
                    }
                }
            }

            if (!session_channel_write_all(ctx, data + start, idx - start)) {
                return false;
            }
            continue;
        }

        if (byte < 0x20U || byte == 0x7FU) {
            if (!session_channel_write_all(ctx, data + idx, 1U)) {
                return false;
            }
            ++idx;
            continue;
        }

        size_t start = idx;
        while (idx < length) {
            unsigned char ch = (unsigned char)data[idx];
            if (ch == '\033' || ch < 0x20U || ch == 0x7FU) {
                break;
            }
            ++idx;
        }

        if (!session_channel_write_utf16_segment(ctx, data + start,
                                                 idx - start)) {
            return false;
        }
    }

    return true;
}

static bool session_channel_write_utf16_segment(session_ctx_t *ctx,
                                                const char *data, size_t length)
{
    if (ctx == nullptr || data == nullptr || length == 0U) {
        return true;
    }

    size_t max_output = length * 4U;
    if (max_output == 0U) {
        return true;
    }

    unsigned char stack_buffer[512];
    unsigned char *buffer = nullptr;
    bool use_stack = max_output <= sizeof(stack_buffer);
    if (use_stack) {
        buffer = stack_buffer;
    } else {
        buffer = (unsigned char *)sshc_gc_malloc(max_output);
        if (buffer == nullptr) {
            return session_channel_write_all(ctx, data, length);
        }
    }

    size_t produced = 0U;
    bool encoded =
        session_utf8_to_utf16le(data, length, buffer, max_output, &produced);
    bool result = false;
    if (encoded) {
        result = session_channel_write_all(ctx, buffer, produced);
    } else {
        result = session_channel_write_all(ctx, data, length);
    }

    if (!use_stack) {
        sshc_gc_free(buffer);
    }

    return result;
}

static size_t session_utf8_decode_codepoint(const unsigned char *data,
                                            size_t length, uint32_t *codepoint)
{
    if (data == nullptr || length == 0U || codepoint == nullptr) {
        return 0U;
    }

    unsigned char b0 = data[0];
    if (b0 < 0x80U) {
        *codepoint = b0;
        return 1U;
    }

    if ((b0 & 0xE0U) == 0xC0U) {
        if (length < 2U) {
            return 0U;
        }
        unsigned char b1 = data[1];
        if ((b1 & 0xC0U) != 0x80U) {
            return 0U;
        }
        uint32_t value =
            ((uint32_t)(b0 & 0x1FU) << 6U) | (uint32_t)(b1 & 0x3FU);
        if (value < 0x80U) {
            return 0U;
        }
        *codepoint = value;
        return 2U;
    }

    if ((b0 & 0xF0U) == 0xE0U) {
        if (length < 3U) {
            return 0U;
        }
        unsigned char b1 = data[1];
        unsigned char b2 = data[2];
        if ((b1 & 0xC0U) != 0x80U || (b2 & 0xC0U) != 0x80U) {
            return 0U;
        }
        uint32_t value = ((uint32_t)(b0 & 0x0FU) << 12U) |
                         ((uint32_t)(b1 & 0x3FU) << 6U) |
                         (uint32_t)(b2 & 0x3FU);
        if (value < 0x800U || (value >= 0xD800U && value <= 0xDFFFU)) {
            return 0U;
        }
        *codepoint = value;
        return 3U;
    }

    if ((b0 & 0xF8U) == 0xF0U) {
        if (length < 4U) {
            return 0U;
        }
        unsigned char b1 = data[1];
        unsigned char b2 = data[2];
        unsigned char b3 = data[3];
        if ((b1 & 0xC0U) != 0x80U || (b2 & 0xC0U) != 0x80U ||
            (b3 & 0xC0U) != 0x80U) {
            return 0U;
        }
        uint32_t value =
            ((uint32_t)(b0 & 0x07U) << 18U) | ((uint32_t)(b1 & 0x3FU) << 12U) |
            ((uint32_t)(b2 & 0x3FU) << 6U) | (uint32_t)(b3 & 0x3FU);
        if (value < 0x10000U || value > 0x10FFFFU) {
            return 0U;
        }
        *codepoint = value;
        return 4U;
    }

    return 0U;
}

static bool session_utf8_to_utf16le(const char *input, size_t length,
                                    unsigned char *output, size_t capacity,
                                    size_t *produced)
{
    if (input == nullptr || output == nullptr) {
        return false;
    }

    size_t out_idx = 0U;
    size_t idx = 0U;
    while (idx < length) {
        uint32_t codepoint = 0U;
        size_t consumed = session_utf8_decode_codepoint(
            (const unsigned char *)input + idx, length - idx, &codepoint);
        if (consumed == 0U) {
            codepoint = 0xFFFD;
            consumed = 1U;
        }
        idx += consumed;

        if (codepoint <= 0xFFFFU) {
            if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
                codepoint = 0xFFFD;
            }
            if (out_idx + 2U > capacity) {
                return false;
            }
            output[out_idx++] = (unsigned char)(codepoint & 0xFFU);
            output[out_idx++] = (unsigned char)((codepoint >> 8U) & 0xFFU);
            continue;
        }

        uint32_t adjusted = codepoint - 0x10000U;
        uint16_t high = (uint16_t)(0xD800U | ((adjusted >> 10U) & 0x3FFU));
        uint16_t low = (uint16_t)(0xDC00U | (adjusted & 0x3FFU));
        if (out_idx + 4U > capacity) {
            return false;
        }
        output[out_idx++] = (unsigned char)(high & 0xFFU);
        output[out_idx++] = (unsigned char)((high >> 8U) & 0xFFU);
        output[out_idx++] = (unsigned char)(low & 0xFFU);
        output[out_idx++] = (unsigned char)((low >> 8U) & 0xFFU);
    }

    if (produced != nullptr) {
        *produced = out_idx;
    }
    return true;
}


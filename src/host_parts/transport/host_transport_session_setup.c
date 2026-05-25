
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
    unsigned char *buffer = (unsigned char *)sshc_gc_malloc(buffer_size);
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
                unsigned char *resized = sshc_gc_realloc(buffer, new_size);
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
                    unsigned char *resized = sshc_gc_realloc(buffer, new_size);
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

        sshc_gc_free(buffer);
        return result;
    }

    sshc_gc_free(buffer);
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

/**
 * @desc Add a session to the chat room member list if not already present.
 * @param room Chat room to update.
 * @param session Session to add to the room.
 * @return None.
 */
static void chat_room_add(chat_room_t *room, session_ctx_t *session)
{
    if (room == nullptr || session == nullptr) {
        return;
    }

    ttak_mutex_lock(&room->lock);
    for (size_t idx = 0; idx < room->member_count; ++idx) {
        if (room->members[idx] == session) {
            ttak_mutex_unlock(&room->lock);
            return;
        }
    }
    if (chat_room_ensure_capacity(room, room->member_count + 1U)) {
        room->members[room->member_count++] = session;
    } else {
        humanized_log_error("chat-room", "failed to grow member list", ENOMEM);
    }
    ttak_mutex_unlock(&room->lock);
}

/**
 * @desc Remove a session from the chat room member list.
 * @param room Chat room to update.
 * @param session Session to remove from the room.
 * @return None.
 */
static void chat_room_remove(chat_room_t *room, const session_ctx_t *session)
{
    if (room == nullptr || session == nullptr) {
        return;
    }

    ttak_mutex_lock(&room->lock);
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
    ttak_mutex_unlock(&room->lock);
}

/**
 * @desc Mark all connected room members as needing a history sink without
 *       forcing an immediate redraw; delivery happens when sessions are at
 *       the newest position.
 * @param room Chat room whose members should be flagged.
 * @return None.
 */
static void chat_room_broadcast_should_sink(chat_room_t *room)
{
    if (room == nullptr) {
        return;
    }

    session_ctx_t **targets = nullptr;
    size_t target_count = 0U;

    ttak_mutex_lock(&room->lock);
    size_t expected_targets = room->member_count;
    if (expected_targets > 0U) {
        targets = sshc_gc_malloc(expected_targets * sizeof(*targets));
        if (targets != nullptr) {
            memset(targets, 0, expected_targets * sizeof(*targets));
            for (size_t idx = 0; idx < room->member_count; ++idx) {
                session_ctx_t *member = room->members[idx];
                if (member == nullptr || !session_transport_active(member)) {
                    continue;
                }
                targets[target_count++] = member;
            }
        }
    }
    ttak_mutex_unlock(&room->lock);

    if (targets == nullptr && target_count == 0U) {
        return;
    }

    for (size_t idx = 0; idx < target_count; ++idx) {
        session_mark_should_sink(targets[idx]);
    }

    sshc_gc_free(targets);
}

/**
 * @desc Broadcast a system or user line to all room members, skipping
 *       sessions frozen in scrollback to preserve their view.
 * @param room Chat room to broadcast into.
 * @param message Message text to emit.
 * @param from Optional sender; when null, treat as system output.
 * @return None.
 */
static void chat_room_broadcast(chat_room_t *room, const char *message,
                                const session_ctx_t *from)
{
    if (room == nullptr || message == nullptr) {
        return;
    }

    session_ctx_t **targets = nullptr;
    session_ctx_t *stack_targets[64];
    size_t target_count = 0U;
    size_t expected_targets = 0U;

    ttak_mutex_lock(&room->lock);
    expected_targets = room->member_count;
    if (expected_targets > 0U) {
        if (expected_targets <= sizeof(stack_targets) / sizeof(stack_targets[0])) {
            targets = stack_targets;
        } else {
            targets = sshc_gc_malloc(expected_targets * sizeof(*targets));
        }
        if (targets != nullptr) {
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
                if (member->no_update) {
                    continue;
                }
                if (atomic_load(&member->room_snapshot_retired)) {
                    continue;
                }
                atomic_fetch_add(&member->room_snapshot_refs, 1U);
                if (atomic_load(&member->room_snapshot_retired)) {
                    atomic_fetch_sub(&member->room_snapshot_refs, 1U);
                    continue;
                }
                targets[target_count++] = member;
            }
        }
    }
    ttak_mutex_unlock(&room->lock);

    // For real-time broadcast: format and send directly without history lookup
    for (size_t idx = 0; idx < target_count; ++idx) {
        session_ctx_t *member = targets[idx];
        if (member == nullptr) {
            continue;
        }

        bool locked = session_output_lock(member);

        // For telnet, or SSH sessions using the display model: trigger an
        // incremental history-scroll redraw so the conversation scrolls up
        // by one line and the new message appears at the bottom, rather than
        // the message simply being appended as a standalone line.
        if (member->transport_kind == SESSION_TRANSPORT_TELNET ||
            member->display_model_initialized) {
            session_flag_should_sink(member);
            session_channel_flush(member);
            if (locked) {
                session_output_unlock(member);
            }
            atomic_fetch_sub(&member->room_snapshot_refs, 1U);
            continue;
        }

        // --- SSH path (no display model) ---
        session_output_buffer_flush(member);
        member->output_buffering_enabled = false;
        member->output_buffer_length = 0U;

        if (from != nullptr) {
            char formatted[SSH_CHATTER_MESSAGE_LIMIT * 2U];
            const char *color =
                from->user_color_code[0] != '\0' ? from->user_color_code : "";
            const char *highlight = from->user_highlight_code[0] != '\0'
                                        ? from->user_highlight_code
                                        : "";
            const char *bold = from->user_is_bold ? ANSI_BOLD : "";

            const bool has_custom_codes =
                (color[0] != '\0') || (highlight[0] != '\0');

            if (has_custom_codes) {
                snprintf(formatted, sizeof(formatted),
                         ANSI_CYAN "[-]" ANSI_RESET " <%s%s%s%s%s> %s",
                         highlight, color, bold, from->user.name, ANSI_RESET,
                         message);
            } else {
                snprintf(formatted, sizeof(formatted),
                         "%s%s " ANSI_CYAN "[-]" ANSI_RESET " <%s>%s %s",
                         color, bold, from->user.name, ANSI_RESET, message);
            }

            session_send_plain_line(member, formatted);
        } else {
            session_send_system_line(member, message);
        }

        session_channel_flush(member);

        if (member->history_scroll_position == 0U) {
            session_clear_pending_sink(member);
        }

        if (member->history_scroll_position == 0U) {
            member->prompt_needs_padding = false;
            member->output_lines_since_prompt = 0U;
            session_refresh_input_line(member);
        }

        if (locked) {
            session_output_unlock(member);
        }
        atomic_fetch_sub(&member->room_snapshot_refs, 1U);
    }

    if (targets != stack_targets) {
        sshc_gc_free(targets);
    }
}

/**
 * @desc Broadcast a caption-style line to all room members, respecting
 *       scrollback freeze and flushing telnet output as needed.
 * @param room Chat room to broadcast into.
 * @param message Caption text to emit.
 * @return None.
 */
static void chat_room_broadcast_caption(chat_room_t *room, const char *message)
{
    if (room == nullptr || message == nullptr) {
        return;
    }

    session_ctx_t **targets = nullptr;
    size_t target_count = 0U;
    size_t expected_targets = 0U;

    ttak_mutex_lock(&room->lock);
    expected_targets = room->member_count;
    if (expected_targets > 0U) {
        targets = sshc_gc_malloc(expected_targets * sizeof(*targets));
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
                if (member->no_update) {
                    continue;
                }
                if (atomic_load(&member->room_snapshot_retired)) {
                    continue;
                }
                atomic_fetch_add(&member->room_snapshot_refs, 1U);
                if (atomic_load(&member->room_snapshot_retired)) {
                    atomic_fetch_sub(&member->room_snapshot_refs, 1U);
                    continue;
                }
                targets[target_count++] = member;
            }
        }
    }
    ttak_mutex_unlock(&room->lock);

    if (targets == nullptr && expected_targets > 0U) {
        humanized_log_error("chat-room", "failed to allocate broadcast buffer",
                            ENOMEM);
        return;
    }

    for (size_t idx = 0; idx < target_count; ++idx) {
        session_ctx_t *member = targets[idx];
        if (member == nullptr) {
            continue;
        }

        // Append the caption as a regular line at the bottom for every
        // transport. A full sink redraw cannot reliably surface a reaction
        // notification when the reacted-to message is outside the current
        // viewport, so the caption is emitted directly and the next sink
        // (triggered by any future chat message) will re-render the inline
        // reaction count from the updated history.
        session_output_buffer_flush(member);
        member->output_buffering_enabled = false;
        member->output_buffer_length = 0U;

        session_send_caption_line(member, message);

        session_channel_flush(member);

        if (member->history_scroll_position == 0U) {
            session_clear_pending_sink(member);
            member->prompt_needs_padding = false;
            member->output_lines_since_prompt = 0U;
            session_refresh_input_line(member);
        }
        atomic_fetch_sub(&member->room_snapshot_refs, 1U);
    }

    sshc_gc_free(targets);
}

/**
 * @desc Broadcast a stored chat history entry to all room members, marking
 *       pending sink updates for scrolled-back sessions while keeping
 *       real-time output for active viewers.
 * @param room Chat room to broadcast into.
 * @param entry Stored chat history entry to display.
 * @param from Optional sender to exclude from the broadcast.
 * @return None.
 */
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
    session_ctx_t *stack_targets[64];
    size_t target_count = 0U;
    size_t expected_targets = 0U;
    session_ctx_t **sink_targets = nullptr;
    session_ctx_t *stack_sink_targets[64];
    size_t sink_count = 0U;

    ttak_mutex_lock(&room->lock);
    expected_targets = room->member_count;
    if (expected_targets > 0U) {
        if (expected_targets <= sizeof(stack_targets) / sizeof(stack_targets[0])) {
            targets = stack_targets;
            sink_targets = stack_sink_targets;
        } else {
            targets = sshc_gc_malloc(expected_targets * sizeof(*targets));
            sink_targets = sshc_gc_malloc(expected_targets * sizeof(*sink_targets));
        }
        if (targets != nullptr) {
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
                if (member->no_update) {
                    if (sink_targets != nullptr) {
                        if (atomic_load(&member->room_snapshot_retired)) {
                            continue;
                        }
                        atomic_fetch_add(&member->room_snapshot_refs, 1U);
                        if (atomic_load(&member->room_snapshot_retired)) {
                            atomic_fetch_sub(&member->room_snapshot_refs, 1U);
                            continue;
                        }
                        sink_targets[sink_count++] = member;
                    }
                    continue;
                }
                if (atomic_load(&member->room_snapshot_retired)) {
                    continue;
                }
                atomic_fetch_add(&member->room_snapshot_refs, 1U);
                if (atomic_load(&member->room_snapshot_retired)) {
                    atomic_fetch_sub(&member->room_snapshot_refs, 1U);
                    continue;
                }
                targets[target_count++] = member;
            }
        }
    }
    ttak_mutex_unlock(&room->lock);

    if (sink_targets != nullptr && sink_count > 0U) {
        for (size_t idx = 0; idx < sink_count; ++idx) {
            session_ctx_t *member = sink_targets[idx];
            if (member != nullptr) {
                session_flag_should_sink(member);
                atomic_fetch_sub(&member->room_snapshot_refs, 1U);
            }
        }
    }

    if (targets == nullptr && expected_targets > 0U) {
        humanized_log_error(
            "chat-room", "failed to allocate entry broadcast buffer", ENOMEM);
        if (sink_targets != stack_sink_targets) {
            sshc_gc_free(sink_targets);
        }
        return;
    }

    // For real-time broadcast: format and send directly without history lookup
    for (size_t idx = 0; idx < target_count; ++idx) {
        session_ctx_t *member = targets[idx];
        if (member == nullptr) {
            continue;
        }

        // For telnet, or SSH sessions using the display model: trigger an
        // incremental history-scroll redraw so the conversation scrolls up
        // by one line and the new message appears at the bottom, rather than
        // the new entry being appended as a standalone line.
        if (member->transport_kind == SESSION_TRANSPORT_TELNET ||
            member->display_model_initialized) {
            session_flag_should_sink(member);
            if (member->history_scroll_position == 0U && !member->no_update) {
                session_process_pending_sink(member);
            }
            session_channel_flush(member);
            atomic_fetch_sub(&member->room_snapshot_refs, 1U);
            continue;
        }

        // --- SSH path (no display model) ---
        session_output_buffer_flush(member);
        member->output_buffering_enabled = false;
        member->output_buffer_length = 0U;

        bool previous_capture = member->capture_realtime_output;
        member->capture_realtime_output =
            (member->history_scroll_position == 0U) && !member->no_update;

        if (entry->is_user_message) {
            char line[SSH_CHATTER_MESSAGE_LIMIT * 2U];
            chat_history_entry_format_user_line(entry, line, sizeof(line),
                                                true);
            session_send_plain_line(member, line);

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
            session_send_plain_line(member, entry->message);
        }

        session_channel_flush(member);

        if (member->history_scroll_position == 0U) {
            session_clear_pending_sink(member);
        }

        if (member->history_scroll_position == 0U) {
            member->prompt_needs_padding = false;
            member->output_lines_since_prompt = 0U;
            session_refresh_input_line(member);
        }

        member->capture_realtime_output = previous_capture;
        atomic_fetch_sub(&member->room_snapshot_refs, 1U);
    }

    if (targets != stack_targets) {
        sshc_gc_free(targets);
    }
    if (sink_targets != stack_sink_targets) {
        sshc_gc_free(sink_targets);
    }
}

static void
chat_room_broadcast_reaction_update(host_t *host,
                                    const chat_history_entry_t *entry)
{
    if (host == nullptr || entry == nullptr) {
        return;
    }

    char summary[SSH_CHATTER_MESSAGE_LIMIT];
    if (!chat_history_entry_build_reaction_summary(
            entry, summary, sizeof(summary), SESSION_UI_LANGUAGE_EN)) {
        return;
    }

    char line[SSH_CHATTER_MESSAGE_LIMIT];
    if (entry->message_id > 0U) {
        char label[32];
        if (!host_compact_id_encode(entry->message_id, label, sizeof(label))) {
            snprintf(label, sizeof(label), "%" PRIu64, entry->message_id);
        }
        snprintf(line, sizeof(line), "    ->[#%s] - %s", label, summary);
    } else {
        snprintf(line, sizeof(line), "    - %s", summary);
    }

    chat_room_broadcast_caption(&host->room, line);
}

static bool host_history_reserve_locked(host_t *host, size_t min_capacity)
{
    if (host == nullptr) {
        return false;
    }

    sshc_memory_context_t *memory_scope = host_memory_scope_push(host);
    bool success = false;

    if (SSH_CHATTER_HISTORY_CACHE_LIMIT > 0U &&
        min_capacity > SSH_CHATTER_HISTORY_CACHE_LIMIT) {
        min_capacity = SSH_CHATTER_HISTORY_CACHE_LIMIT;
    }

    if (min_capacity <= host->history_capacity) {
        success = true;
        goto cleanup;
    }

    if (min_capacity > SIZE_MAX / sizeof(chat_history_entry_t)) {
        humanized_log_error("host-history",
                            "history buffer too large to allocate", ENOMEM);
        goto cleanup;
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
    chat_history_entry_t *resized = sshc_gc_realloc(host->history, bytes);
    if (resized == nullptr) {
        humanized_log_error("host-history",
                            "failed to grow chat history buffer",
                            errno != 0 ? errno : ENOMEM);
        goto cleanup;
    }

    if (new_capacity > host->history_capacity) {
        size_t old_capacity = host->history_capacity;
        size_t added = new_capacity - old_capacity;
        memset(resized + old_capacity, 0, added * sizeof(chat_history_entry_t));
    }

    host->history = resized;
    host->history_capacity = new_capacity;
    success = true;

cleanup:
    host_memory_scope_pop(memory_scope);
    return success;
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
    ttak_mutex_lock(&host->lock);
    count = host->history_total;
    ttak_mutex_unlock(&host->lock);
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
             "%s", entry->user_color_code);
    snprintf(serialized.user_highlight_code,
             sizeof(serialized.user_highlight_code), "%s",
             entry->user_highlight_code);
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

    ttak_mutex_lock(&host->lock);
    size_t total = host->history_total;
    if (start_index >= total) {
        ttak_mutex_unlock(&host->lock);
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
        cached_copy = (chat_history_entry_t *)sshc_gc_malloc(cache_portion *
                                                        sizeof(*cached_copy));
        if (cached_copy == nullptr) {
            ttak_mutex_unlock(&host->lock);
            return 0U;
        }
        cached_offset = cache_begin_index - cache_start;
        for (size_t idx = 0U; idx < cache_portion; ++idx) {
            cached_copy[idx] = host->history[cached_offset + idx];
        }
        cached_count = cache_portion;
    }

    ttak_mutex_unlock(&host->lock);

    size_t produced = 0U;

    if (before_cache > 0U) {
        if (state_path[0] == '\0') {
            sshc_gc_free(cached_copy);
            return 0U;
        }
        size_t fetched = host_state_read_history_range(state_path, start_index,
                                                       buffer, before_cache);
        if (fetched < before_cache) {
            sshc_gc_free(cached_copy);
            return fetched;
        }
        produced += fetched;
    }

    if (cached_count > 0U && cached_copy != nullptr) {
        memcpy(buffer + produced, cached_copy,
               cached_count * sizeof(*cached_copy));
        produced += cached_count;
    }

    sshc_gc_free(cached_copy);
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

    ttak_mutex_lock(&host->lock);
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
    ttak_mutex_unlock(&host->lock);

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

    ttak_mutex_lock(&host->lock);

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
                entries = (chat_history_entry_t *)sshc_gc_malloc(entry_count *
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
            entries = (chat_history_entry_t *)sshc_gc_malloc(entry_count *
                                                        sizeof(*entries));
            if (entries == nullptr) {
                ttak_mutex_unlock(&host->lock);
                return 0U;
            }
            for (size_t idx = 0U; idx < entry_count; ++idx) {
                entries[idx] = host->history[idx];
            }
        }
        history_loaded = true;
    }

    if (!history_loaded || (entries == nullptr && entry_count == 0U)) {
        ttak_mutex_unlock(&host->lock);
        sshc_gc_free(entries);
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
        ttak_mutex_unlock(&host->lock);
        sshc_gc_free(entries);
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
        ttak_mutex_unlock(&host->lock);
        sshc_gc_free(entries);
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

    ttak_mutex_unlock(&host->lock);

    if (first_removed != nullptr) {
        *first_removed = local_first;
    }
    if (last_removed != nullptr) {
        *last_removed = local_last;
    }
    if (replies_removed != nullptr) {
        *replies_removed = reply_removed;
    }

    sshc_gc_free(entries);
    return removed;
}

static bool host_replies_find_entry_by_id(host_t *host, uint64_t reply_id,
                                          chat_reply_entry_t *entry)
{
    if (host == nullptr || entry == nullptr || reply_id == 0U) {
        return false;
    }

    bool found = false;

    ttak_mutex_lock(&host->lock);
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
    ttak_mutex_unlock(&host->lock);

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
    session_build_network_topology_key(from, entry->user_topology,
                                       sizeof(entry->user_topology));
    if (from->user_color_code[0] != '\0') {
        snprintf(entry->user_color_code, sizeof(entry->user_color_code), "%s",
                 from->user_color_code);
    } else {
        entry->user_color_code[0] = '\0';
    }

    if (from->user_highlight_code[0] != '\0') {
        snprintf(entry->user_highlight_code, sizeof(entry->user_highlight_code),
                 "%s", from->user_highlight_code);
    } else {
        entry->user_highlight_code[0] = '\0';
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

    ttak_mutex_lock(&host->lock);
    if (entry->is_user_message) {
        if (host->next_message_id == 0U) {
            host->next_message_id = 1U;
        }
        entry->message_id = host->next_message_id++;
    } else {
        entry->message_id = 0U;
    }

    if (!host_history_append_locked(host, entry)) {
        ttak_mutex_unlock(&host->lock);
        return false;
    }

    if (stored_entry != nullptr) {
        *stored_entry = *entry;
    }

    ttak_mutex_unlock(&host->lock);
    return true;
}

static bool host_replies_commit_entry(host_t *host, chat_reply_entry_t *entry,
                                      chat_reply_entry_t *stored_entry)
{
    if (host == nullptr || entry == nullptr) {
        return false;
    }

    bool committed = false;

    ttak_mutex_lock(&host->lock);
    if (host->reply_count >= SSH_CHATTER_MAX_REPLIES) {
        ttak_mutex_unlock(&host->lock);
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

    ttak_mutex_unlock(&host->lock);
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

    ttak_mutex_lock(&host->lock);

    if (host->history == nullptr || host->history_count == 0U) {
        ttak_mutex_unlock(&host->lock);
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

    ttak_mutex_unlock(&host->lock);
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

    ttak_mutex_lock(&host->lock);
    if (host->history == nullptr) {
        ttak_mutex_unlock(&host->lock);
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
    ttak_mutex_unlock(&host->lock);

    return applied;
}

static void session_apply_theme_defaults(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    host_t *host = ctx->owner;

    if (host->user_theme.userColor != nullptr) {
        snprintf(ctx->user_color_code, sizeof(ctx->user_color_code), "%s",
                 host->user_theme.userColor);
    } else {
        ctx->user_color_code[0] = '\0';
    }

    if (host->user_theme.highlight != nullptr) {
        snprintf(ctx->user_highlight_code, sizeof(ctx->user_highlight_code),
                 "%s", host->user_theme.highlight);
    } else {
        ctx->user_highlight_code[0] = '\0';
    }
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

    ttak_mutex_lock(&host->lock);
    user_preference_t *pref =
        host_ensure_preference_locked(host, ctx->user.name, "");
    if (pref != nullptr) {
        pref->has_user_theme = true;
        snprintf(pref->user_color_code, sizeof(pref->user_color_code), "%s",
                 ctx->user_color_code);
        snprintf(pref->user_highlight_code, sizeof(pref->user_highlight_code),
                 "%s",
                 ctx->user_highlight_code);
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
                 ctx->user_color_code);
        snprintf(ctx->user_data.user_highlight_code,
                 sizeof(ctx->user_data.user_highlight_code), "%s",
                 ctx->user_highlight_code);
        snprintf(ctx->user_data.user_color_name,
                 sizeof(ctx->user_data.user_color_name), "%s",
                 ctx->user_color_name);
        snprintf(ctx->user_data.user_highlight_name,
                 sizeof(ctx->user_data.user_highlight_name), "%s",
                 ctx->user_highlight_name);
        (void)session_user_data_commit(ctx);
    }
    host_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);
}

static void host_store_system_theme(host_t *host, const session_ctx_t *ctx)
{
    if (host == nullptr || ctx == nullptr) {
        return;
    }

    ttak_mutex_lock(&host->lock);
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
    ttak_mutex_unlock(&host->lock);
}

static void host_store_user_os(host_t *host, const session_ctx_t *ctx)
{
    if (host == nullptr || ctx == nullptr) {
        return;
    }

    ttak_mutex_lock(&host->lock);
    user_preference_t *pref =
        host_ensure_preference_locked(host, ctx->user.name, "");
    if (pref != nullptr) {
        snprintf(pref->os_name, sizeof(pref->os_name), "%s", ctx->os_name);
    }
    host_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);
}

static void host_store_birthday(host_t *host, const session_ctx_t *ctx,
                                const char *birthday)
{
    if (host == nullptr || ctx == nullptr || birthday == nullptr) {
        return;
    }

    ttak_mutex_lock(&host->lock);
    user_preference_t *pref =
        host_ensure_preference_locked(host, ctx->user.name, "");
    if (pref != nullptr) {
        pref->has_birthday = true;
        snprintf(pref->birthday, sizeof(pref->birthday), "%s", birthday);
    }
    host_state_save_locked(host);
    host_refresh_motd_locked(host);
    ttak_mutex_unlock(&host->lock);
}

static void host_store_chat_spacing(host_t *host, const session_ctx_t *ctx)
{
    if (host == nullptr || ctx == nullptr) {
        return;
    }

    ttak_mutex_lock(&host->lock);
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
    ttak_mutex_unlock(&host->lock);
}

static void host_store_translation_preferences(host_t *host,
                                               const session_ctx_t *ctx)
{
    if (host == nullptr || ctx == nullptr) {
        return;
    }

    ttak_mutex_lock(&host->lock);
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
    ttak_mutex_unlock(&host->lock);
}

static void host_store_breaking_alerts(host_t *host, const session_ctx_t *ctx)
{
    if (host == nullptr || ctx == nullptr) {
        return;
    }

    ttak_mutex_lock(&host->lock);
    user_preference_t *pref =
        host_ensure_preference_locked(host, ctx->user.name, "");
    if (pref != nullptr) {
        pref->breaking_alerts_enabled = ctx->breaking_alerts_enabled;
    }
    host_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);
}

void host_store_ui_language(host_t *host, const session_ctx_t *ctx)
{
    if (host == nullptr || ctx == nullptr) {
        return;
    }

    ttak_mutex_lock(&host->lock);
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
    ttak_mutex_unlock(&host->lock);
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
    ttak_mutex_lock(&host->lock);
    result = host_ip_has_grant_locked(host, ip);
    ttak_mutex_unlock(&host->lock);
    return result;
}

static void host_apply_grant_to_ip(host_t *host, const char *ip)
{
    if (host == nullptr || ip == nullptr || ip[0] == '\0') {
        return;
    }

    session_ctx_t **matches = nullptr;
    size_t match_count = 0U;

    ttak_mutex_lock(&host->room.lock);
    if (host->room.member_count > 0U) {
        matches = sshc_gc_calloc(host->room.member_count, sizeof(*matches));
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
    ttak_mutex_unlock(&host->room.lock);

    if (matches == nullptr) {
        return;
    }

    for (size_t idx = 0U; idx < match_count; ++idx) {
        session_ctx_t *member = matches[idx];
        session_send_system_line(
            member, "Operator privileges granted for your IP address.");
    }

    sshc_gc_free(matches);
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

    ttak_mutex_lock(&host->room.lock);
    if (host->room.member_count > 0U) {
        session_ctx_t **allocated =
            sshc_gc_calloc(host->room.member_count, sizeof(*allocated));
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
    ttak_mutex_unlock(&host->room.lock);

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

    sshc_gc_free(matches);
}

static bool host_lookup_user_os(host_t *host, const char *username,
                                char *buffer, size_t length)
{
    if (host == nullptr || username == nullptr || buffer == nullptr ||
        length == 0U) {
        return false;
    }

    bool found = false;

    ttak_mutex_lock(&host->lock);
    user_preference_t *pref = host_find_preference_locked(host, username, "");
    if (pref != nullptr && pref->os_name[0] != '\0') {
        snprintf(buffer, length, "%s", pref->os_name);
        found = true;
    }
    ttak_mutex_unlock(&host->lock);

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
        entry->user_color_code[0] = '\0';
        entry->user_highlight_code[0] = '\0';
        entry->user_is_bold = false;
        entry->user_color_name[0] = '\0';
        entry->user_highlight_name[0] = '\0';
        return true;
    }

    const bool has_color_code =
        entry->user_color_code[0] != '\0';
    const bool has_highlight_code = entry->user_highlight_code[0] != '\0';

    if (!has_color_code) {
        const char *color_code = lookup_color_code(
            USER_COLOR_MAP, sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
            entry->user_color_name);
        if (color_code == nullptr) {
            color_code = host->user_theme.userColor;
            snprintf(entry->user_color_name, sizeof(entry->user_color_name),
                     "%s", host->default_user_color_name);
        }
        if (color_code != nullptr) {
            snprintf(entry->user_color_code, sizeof(entry->user_color_code),
                     "%s", color_code);
        }
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
        if (highlight_code != nullptr) {
            snprintf(entry->user_highlight_code,
                     sizeof(entry->user_highlight_code), "%s", highlight_code);
        }
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

    const char *toggle = getenv("CHATTER_SECURITY_FILTER");
    if (toggle != nullptr && toggle[0] != '\0') {
        if (strcasecmp(toggle, "0") == 0 || strcasecmp(toggle, "false") == 0 ||
            strcasecmp(toggle, "off") == 0) {
            return;
        }
    }

    bool pipeline_enabled = false;

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

    atomic_store(&host->security_filter_enabled, false);
}

static double host_elapsed_seconds(const struct timespec *start,
                                   const struct timespec *end)
{
    if (start == nullptr || end == nullptr) {
        return 0.0;
    }

    double sec = (double)end->tv_sec - (double)start->tv_sec;
    double nsec_to_sec =
        ((double)end->tv_nsec - (double)start->tv_nsec) / 1000000000.0;

    return sec + nsec_to_sec;
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
    int state_fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (state_fd >= 0) {
        if (fstat(state_fd, &file_stat) != 0) {
            int saved_errno = errno;
            close(state_fd);
            humanized_log_error("host", "failed to inspect bbs state path",
                                saved_errno != 0 ? saved_errno : EIO);
            return false;
        }

        if (!S_ISREG(file_stat.st_mode)) {
            close(state_fd);
            humanized_log_error(
                "host", "bbs state path does not reference a regular file",
                EINVAL);
            return false;
        }

        if ((file_stat.st_mode & (S_IWOTH | S_IWGRP)) != 0U) {
            if (fchmod(state_fd, S_IRUSR | S_IWUSR) != 0) {
                int saved_errno = errno;
                close(state_fd);
                humanized_log_error("host",
                                    "failed to tighten bbs state permissions",
                                    saved_errno != 0 ? saved_errno : EACCES);
                return false;
            }
        }

        if (file_stat.st_uid != geteuid()) {
            close(state_fd);
            humanized_log_error("host", "bbs state file ownership mismatch",
                                EPERM);
            return false;
        }

        close(state_fd);
    } else if (errno != ENOENT) {
        humanized_log_error("host", "failed to inspect bbs state path",
                            errno != 0 ? errno : EIO);
        return false;
    }

    return true;
}

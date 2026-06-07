static void session_send_plain_line(session_ctx_t *ctx, const char *message)
{
    if (ctx == nullptr || !session_transport_active(ctx) ||
        message == nullptr) {
        return;
    }

    // Prevent re-output of the last processed line
    if (!ctx->disable_output_dedup && ctx->has_last_output_line &&
        strncmp(ctx->last_output_line, message, SSH_CHATTER_MESSAGE_LIMIT) == 0) {
        return;
    }

    static const char kCaptionPrefix[] = "    ->";
    if (strncmp(message, kCaptionPrefix, sizeof(kCaptionPrefix) - 1U) == 0) {
        session_send_caption_line(ctx, message);
        // Update last output line after sending
        size_t msg_len = strnlen(message, SSH_CHATTER_MESSAGE_LIMIT - 1U);
        if (msg_len >= sizeof(ctx->last_output_line)) {
            msg_len = sizeof(ctx->last_output_line) - 1U;
        }
        memcpy(ctx->last_output_line, message, msg_len);
        ctx->last_output_line[msg_len] = '\0';
        ctx->has_last_output_line = true;
        return;
    }

    session_write_rendered_line(ctx, message);
    session_realtime_record_line(ctx, message);

    if (ctx->capture_realtime_output &&
        ctx->realtime_line_count >= SESSION_REALTIME_CLEAR_INTERVAL) {
        session_realtime_refresh(ctx);
    }

    // Update last output line after sending
    size_t msg_len = strnlen(message, SSH_CHATTER_MESSAGE_LIMIT - 1U);
    if (msg_len >= sizeof(ctx->last_output_line)) {
        msg_len = sizeof(ctx->last_output_line) - 1U;
    }
    memcpy(ctx->last_output_line, message, msg_len);
    ctx->last_output_line[msg_len] = '\0';
    ctx->has_last_output_line = true;
}

static void session_send_reply_tree(session_ctx_t *ctx,
                                    uint64_t parent_message_id,
                                    uint64_t parent_reply_id, size_t depth)
{
    if (ctx == nullptr || ctx->owner == nullptr || parent_message_id == 0U) {
        return;
    }

    if (depth > 32U) {
        return;
    }

    host_t *host = ctx->owner;

    size_t match_count = 0U;
    ttak_mutex_lock(&host->lock);
    for (size_t idx = 0U; idx < host->reply_count; ++idx) {
        const chat_reply_entry_t *candidate = &host->replies[idx];
        if (!candidate->in_use) {
            continue;
        }
        if (candidate->parent_message_id == parent_message_id &&
            candidate->parent_reply_id == parent_reply_id) {
            ++match_count;
        }
    }

    if (match_count == 0U) {
        ttak_mutex_unlock(&host->lock);
        return;
    }

    chat_reply_entry_t *snapshot = sshc_gc_calloc(match_count, sizeof(*snapshot));
    if (snapshot == nullptr) {
        ttak_mutex_unlock(&host->lock);
        return;
    }

    size_t copy_idx = 0U;
    for (size_t idx = 0U; idx < host->reply_count && copy_idx < match_count;
         ++idx) {
        const chat_reply_entry_t *candidate = &host->replies[idx];
        if (!candidate->in_use) {
            continue;
        }
        if (candidate->parent_message_id == parent_message_id &&
            candidate->parent_reply_id == parent_reply_id) {
            snapshot[copy_idx++] = *candidate;
        }
    }
    ttak_mutex_unlock(&host->lock);

    for (size_t idx = 0U; idx < copy_idx; ++idx) {
        const chat_reply_entry_t *reply = &snapshot[idx];

        size_t indent_len = depth * 4U;
        char indent[128];
        if (indent_len >= sizeof(indent)) {
            indent_len = sizeof(indent) - 1U;
        }
        memset(indent, ' ', indent_len);
        indent[indent_len] = '\0';

        char reply_label[32];
        const char *reply_display = "?";
        if (host_compact_id_encode(reply->reply_id, reply_label,
                                   sizeof(reply_label))) {
            reply_display = reply_label;
        }

        char line[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(line, sizeof(line), "%s(r#%s) %s", indent, reply_display,
                 reply->message);
        session_send_plain_line(ctx, line);

        session_send_reply_tree(ctx, parent_message_id, reply->reply_id,
                                depth + 1U);
    }

    sshc_gc_free(snapshot);
}

static bool host_lookup_member_ip(host_t *host, const char *username, char *ip,
                                  size_t length)
{
    if (host == nullptr || username == nullptr || ip == nullptr ||
        length == 0U) {
        return false;
    }

    session_ctx_t *member = chat_room_find_user(&host->room, username);
    if (member == nullptr || member->client_ip[0] == '\0') {
        return false;
    }

    snprintf(ip, length, "\%s", member->client_ip);
    return true;
}

static void session_telnet_capture_startup_metadata(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->telnet_fd < 0) {
        return;
    }

    while (!ctx->telnet_eof) {
        struct pollfd pfd = {
            .fd = ctx->telnet_fd,
            .events = POLLIN,
            .revents = 0,
        };

        int poll_result = poll(&pfd, 1, 0);
        if (poll_result <= 0 || (pfd.revents & POLLIN) == 0) {
            break;
        }

        unsigned char byte = 0U;
        int read_result = session_telnet_read_byte(ctx, &byte, 0);
        if (read_result == SSH_AGAIN) {
            break;
        }
        if (read_result <= 0) {
            break;
        }

        if (byte != 0U) {
            if (!ctx->telnet_pending_valid) {
                ctx->telnet_pending_char = (int)byte;
                ctx->telnet_pending_valid = true;
            }
            break;
        }
    }
}

typedef struct provider_prefix {
    const char *prefix;
    const char *label;
} provider_prefix_t;

static const provider_prefix_t kProviderPrefixes[] = {
    {"39.7.", "Korean ISP"},
    {"58.120.", "Korean ISP"},
    {"59.0.", "Korean ISP"},
    {"61.32.", "Korean ISP"},
    {"211.36.", "Korean ISP"},
    {"218.144.", "Korean ISP"},
    {"220.149.", "Korean ISP"},
    {"73.", "US ISP"},
    {"96.", "US ISP"},
    {"107.", "US ISP"},
    {"174.", "US ISP"},
    {"2600:", "US ISP"},
    {"2604:", "US ISP"},
    {"2605:", "US ISP"},
    {"2607:", "US ISP"},
    {"2609:", "US ISP"},
    {"1.0.", "Japanese ISP"},
    {"106.130.", "Japanese ISP"},
    {"118.103.", "Japanese ISP"},
    {"133.", "Japanese ISP"},
    {"153.", "Japanese ISP"},
    {"60.62.", "Japanese ISP"},
    {"61.22.", "Japanese ISP"},
    {"61.46.", "Japanese ISP"},
    {"220.100", "Japanese ISP"},
    {"202.162.128.", "Japanese ISP"},
    {"202.140.240.", "Japanese ISP"},
    {"110.172", "Japanese ISP"},
    {"2400:", "Japanese ISP"},
    {"2404:", "Japanese ISP"},
    {"2406:", "Japanese ISP"},
    {"2408:", "Japanese ISP"},
    {"24.114.", "Canadian ISP"},
    {"142.", "Canadian ISP"},
    {"2603:", "Canadian ISP"},
    {"185.", "EU ISP"},
    {"195.", "EU ISP"},
    {"2a00:", "EU ISP"},
    {"2a02:", "EU ISP"},
    {"2a03:", "EU ISP"},
    {"2a09:", "EU ISP"},
    {"5.18.", "Russian ISP"},
    {"37.", "Russian ISP"},
    {"91.", "Russian ISP"},
    {"36.", "Chinese ISP"},
    {"42.", "Chinese ISP"},
    {"139.", "Chinese ISP"},
    {"2408:", "Chinese ISP"},
    {"2409:", "Chinese ISP"},
    {"49.", "Indian ISP"},
    {"103.", "Indian ISP"},
    {"106.", "Indian ISP"},
    {"2405:", "Indian ISP"},
    {"2406:", "Indian ISP"},
    {"100.64.", "Carrier-grade NAT"}};

bool is_pure_ascii(const char *str)
{
    for (const unsigned char *p = (const unsigned char *)str; *p != '\0'; ++p) {
        if (*p > 127) {
            return false;
        }
    }
    return true;
}

static int uint32_t_cmp(const void *a, const void *b)
{
    uint32_t va = *(const uint32_t *)a;
    uint32_t vb = *(const uint32_t *)b;
    return (va > vb) - (va < vb);
}

int count_unicode_points(const char *str, utf8_code_count_t **counts_out,
                         size_t *unique_count_out)
{
    size_t len = strnlen(str, SSH_CHATTER_MESSAGE_LIMIT);
    uint32_t points[SSH_CHATTER_MESSAGE_LIMIT];
    size_t total = 0;

    for (size_t i = 0; i < len;) {
        unsigned int code_point = 0;
        size_t bytes_read = 0;

        unsigned char c = (unsigned char)str[i];
        if ((c & 0x80) == 0) {
            code_point = c;
            bytes_read = 1;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 < len)
                code_point = ((c & 0x1F) << 6) | (str[i + 1] & 0x3F);
            bytes_read = 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 < len)
                code_point = ((c & 0x0F) << 12) | ((str[i + 1] & 0x3F) << 6) |
                             (str[i + 2] & 0x3F);
            bytes_read = 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 < len)
                code_point = ((c & 0x07) << 18) | ((str[i + 1] & 0x3F) << 12) |
                             ((str[i + 2] & 0x3F) << 6) | (str[i + 3] & 0x3F);
            bytes_read = 4;
        } else {
            code_point = c;
            bytes_read = 1;
        }

        i += bytes_read;
        points[total++] = code_point;
    }

    if (total == 0) {
        *counts_out = nullptr;
        *unique_count_out = 0;
        return 0;
    }

    qsort(points, total, sizeof(points[0]), uint32_t_cmp);

    size_t unique = 1;
    for (size_t i = 1; i < total; ++i) {
        if (points[i] != points[i - 1])
            unique++;
    }

    utf8_code_count_t *counts =
        (utf8_code_count_t *)sshc_gc_calloc(unique, sizeof(*counts));
    if (!counts)
        return -1;

    size_t idx = 0;
    counts[idx].code_point = points[0];
    counts[idx].count = 1;
    for (size_t i = 1; i < total; ++i) {
        if (points[i] == counts[idx].code_point) {
            counts[idx].count++;
        } else {
            idx++;
            counts[idx].code_point = points[i];
            counts[idx].count = 1;
        }
    }

    *counts_out = counts;
    *unique_count_out = unique;
    return (int)total;
}

double calculate_chi_squared(const char *str)
{
    utf8_code_count_t *counts = nullptr;
    size_t unique_count = 0;
    int N = count_unicode_points(str, &counts, &unique_count);

    if (N <= 0 || !counts || unique_count == 0) {
        if (counts)
            sshc_gc_free(counts);
        return 0.0;
    }

    double E_i = (double)N / (double)unique_count;

    double chi_squared = 0.0;
    for (size_t i = 0; i < unique_count; i++) {
        double O_i = (double)counts[i].count;
        double diff = O_i - E_i;

        if (E_i > 0) {
            chi_squared += (diff * diff) / E_i;
        }
    }

    sshc_gc_free(counts);
    return chi_squared;
}

bool is_string_random(const char *str)
{
    static const double CRITICAL_THRESHOLD = 25.0;

    double chi_squared = calculate_chi_squared(str);

    if (strlen(str) > 20) {
        return chi_squared < CRITICAL_THRESHOLD;
    } else {
        return chi_squared < 10.0;
    }
}

bool session_detect_provider_ip(const char *ip, char *label, size_t length)
{
    if (label != nullptr && length > 0U) {
        label[0] = '\0';
    }

    if (ip == nullptr || ip[0] == '\0' || label == nullptr || length == 0U) {
        return false;
    }

    for (size_t idx = 0U;
         idx < sizeof(kProviderPrefixes) / sizeof(kProviderPrefixes[0]);
         ++idx) {
        const provider_prefix_t *entry = &kProviderPrefixes[idx];
        size_t prefix_len = strlen(entry->prefix);
        if (strncasecmp(ip, entry->prefix, prefix_len) == 0) {
            snprintf(label, length, "%s", entry->label);
            return true;
        }
    }

    return false;
}

static const struct {
    const char *label;
    session_ui_language_t language;
} kProviderLanguageMapping[] = {
    {"Korean ISP", SESSION_UI_LANGUAGE_KO},
    {"US ISP", SESSION_UI_LANGUAGE_EN},
    {"Canadian ISP", SESSION_UI_LANGUAGE_EN},
    {"EU ISP", SESSION_UI_LANGUAGE_EN},
    {"Russian ISP", SESSION_UI_LANGUAGE_RU},
    {"Chinese ISP", SESSION_UI_LANGUAGE_ZH},
    {"Indian ISP", SESSION_UI_LANGUAGE_EN},
    {"Japanese ISP", SESSION_UI_LANGUAGE_JP},
};

static session_ui_language_t
session_client_geo_language(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return SESSION_UI_LANGUAGE_COUNT;
    }

    if (ctx->owner != nullptr &&
        !atomic_load(&ctx->owner->geo_language_enabled)) {
        return SESSION_UI_LANGUAGE_COUNT;
    }

    char label[64];
    if (!session_detect_provider_ip(ctx->client_ip, label, sizeof(label))) {
        return SESSION_UI_LANGUAGE_COUNT;
    }

    for (size_t idx = 0U; idx < sizeof(kProviderLanguageMapping) /
                                    sizeof(kProviderLanguageMapping[0]);
         ++idx) {
        if (strcasecmp(label, kProviderLanguageMapping[idx].label) == 0) {
            return kProviderLanguageMapping[idx].language;
        }
    }

    return SESSION_UI_LANGUAGE_COUNT;
}

static bool session_blocklist_add(session_ctx_t *ctx, const char *ip,
                                  const char *username, bool ip_wide,
                                  bool *already_present)
{
    if (ctx == nullptr) {
        if (already_present != nullptr) {
            *already_present = false;
        }
        return false;
    }

    if (already_present != nullptr) {
        *already_present = false;
    }

    char normalized_ip[SSH_CHATTER_IP_LEN] = {0};
    char normalized_user[SSH_CHATTER_USERNAME_LEN] = {0};

    if (ip != nullptr && ip[0] != '\0') {
        snprintf(normalized_ip, sizeof(normalized_ip), "%s", ip);
    }

    if (username != nullptr && username[0] != '\0') {
        snprintf(normalized_user, sizeof(normalized_user), "%s", username);
    }

    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_BLOCKED; ++idx) {
        session_block_entry_t *entry = &ctx->block_entries[idx];
        if (!entry->in_use) {
            continue;
        }

        if (ip_wide) {
            if (normalized_ip[0] != '\0' &&
                strncmp(entry->ip, normalized_ip, SSH_CHATTER_IP_LEN) == 0) {
                if (already_present != nullptr) {
                    *already_present = true;
                }
                return false;
            }
        } else {
            if (normalized_user[0] != '\0' &&
                strncmp(entry->username, normalized_user,
                        SSH_CHATTER_USERNAME_LEN) == 0 &&
                !entry->ip_wide) {
                if (already_present != nullptr) {
                    *already_present = true;
                }
                return false;
            }
        }
    }

    size_t free_index = SSH_CHATTER_MAX_BLOCKED;
    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_BLOCKED; ++idx) {
        if (!ctx->block_entries[idx].in_use) {
            free_index = idx;
            break;
        }
    }

    if (free_index >= SSH_CHATTER_MAX_BLOCKED) {
        return false;
    }

    session_block_entry_t *slot = &ctx->block_entries[free_index];
    memset(slot, 0, sizeof(*slot));
    slot->in_use = true;
    slot->ip_wide = ip_wide;
    if (normalized_ip[0] != '\0') {
        snprintf(slot->ip, sizeof(slot->ip), "%s", normalized_ip);
    }
    if (normalized_user[0] != '\0') {
        snprintf(slot->username, sizeof(slot->username), "%s", normalized_user);
    }

    if (ctx->block_entry_count < SSH_CHATTER_MAX_BLOCKED) {
        ctx->block_entry_count += 1U;
    }

    return true;
}

static bool session_blocklist_remove(session_ctx_t *ctx, const char *token)
{
    if (ctx == nullptr || token == nullptr || token[0] == '\0') {
        return false;
    }

    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_BLOCKED; ++idx) {
        session_block_entry_t *entry = &ctx->block_entries[idx];
        if (!entry->in_use) {
            continue;
        }

        if ((entry->ip[0] != '\0' &&
             strncmp(entry->ip, token, SSH_CHATTER_IP_LEN) == 0) ||
            (entry->username[0] != '\0' &&
             strncmp(entry->username, token, SSH_CHATTER_USERNAME_LEN) == 0)) {
            memset(entry, 0, sizeof(*entry));
            if (ctx->block_entry_count > 0U) {
                ctx->block_entry_count -= 1U;
            }
            return true;
        }
    }

    return false;
}

static void session_blocklist_show(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    if (ctx->block_entry_count == 0U) {
        session_send_system_line(ctx, "No blocked users or IPs.");
        return;
    }

    session_send_system_line(ctx, "Blocked targets:");
    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_BLOCKED; ++idx) {
        const session_block_entry_t *entry = &ctx->block_entries[idx];
        if (!entry->in_use) {
            continue;
        }

        char line[SSH_CHATTER_MESSAGE_LIMIT];
        if (entry->ip_wide && entry->ip[0] != '\0') {
            if (entry->username[0] != '\0') {
                snprintf(line, sizeof(line),
                         "- %s (all users from this IP, originally [%s])",
                         entry->ip, entry->username);
            } else {
                snprintf(line, sizeof(line), "- %s (all users from this IP)",
                         entry->ip);
            }
        } else if (entry->username[0] != '\0') {
            if (entry->ip[0] != '\0') {
                snprintf(line, sizeof(line), "- [%s] (only this user, IP %s)",
                         entry->username, entry->ip);
            } else {
                snprintf(line, sizeof(line), "- [%s]", entry->username);
            }
        } else {
            snprintf(line, sizeof(line), "- entry #%zu", idx + 1U);
        }
        session_send_system_line(ctx, line);
    }
}

static bool session_message_contains_breaking(const char *message)
{
    if (message == nullptr) {
        return false;
    }

    if (strstr(message, SSH_CHATTER_RSS_BREAKING_PREFIX) != nullptr) {
        return true;
    }

    if (strcasestr(message, "breaking news") != nullptr ||
        strcasestr(message, "breaking:") != nullptr ||
        strcasestr(message, "urgent") != nullptr ||
        strcasestr(message, "alert") != nullptr) {
        return true;
    }

    if (strstr(message, "속보") != nullptr ||
        strstr(message, "速報") != nullptr) {
        return true;
    }

    return false;
}

static bool session_bbs_should_defer_breaking(session_ctx_t *ctx,
                                              const char *message)
{
    if (ctx == nullptr || message == nullptr) {
        return false;
    }

    if (!ctx->breaking_alerts_enabled) {
        return false;
    }

    if (!ctx->bbs_post_pending || ctx->bbs_rendering_editor) {
        return false;
    }

    return session_message_contains_breaking(message);
}

static size_t session_find_wrap_position(const char *text, size_t start,
                                         size_t max_width)
{
    if (text == nullptr || max_width == 0U) {
        return start;
    }

    size_t pos = start;
    size_t visible_chars = 0U;
    size_t last_space_pos = start;
    size_t last_space_visible = 0U;
    bool in_escape = false;

    while (text[pos] != '\0' && visible_chars < max_width) {
        if (text[pos] == '\033') {
            // Start of ANSI escape sequence
            in_escape = true;
            ++pos;
            continue;
        }

        if (in_escape) {
            // Skip escape sequence characters
            if ((text[pos] >= 'A' && text[pos] <= 'Z') ||
                (text[pos] >= 'a' && text[pos] <= 'z')) {
                in_escape = false;
            }
            ++pos;
            continue;
        }

        // Regular character
        if (text[pos] == ' ' || text[pos] == '\t') {
            last_space_pos = pos;
            last_space_visible = visible_chars;
        }

        ++visible_chars;
        ++pos;
    }

    // If we haven't exceeded max_width, return the current position
    if (text[pos] == '\0' || visible_chars < max_width) {
        return pos;
    }

    // If we found a space within reasonable distance, break there
    if (last_space_visible > 0U &&
        (visible_chars - last_space_visible) < (max_width / 3)) {
        return last_space_pos + 1; // Skip the space itself
    }

    // Otherwise, hard break at max_width
    return pos;
}

static void session_bbs_format_breaking_notice_wrapped(
    const char *message, char lines[][SSH_CHATTER_MESSAGE_LIMIT],
    size_t max_lines, size_t *line_count)
{
    if (message == nullptr || lines == nullptr || max_lines == 0U ||
        line_count == nullptr) {
        if (line_count != nullptr) {
            *line_count = 0U;
        }
        return;
    }

    *line_count = 0U;

    // Maximum visible characters per line (accounting for terminal width)
    const size_t kMaxVisibleWidth = 48U;

    // ANSI codes to apply
    const char *kPrefix =
        "\r\033[2G" ANSI_BG_BRIGHT_BLUE ANSI_BRIGHT_MAGENTA ANSI_BOLD;
    const char *kSuffix = "\033[K" ANSI_RESET;

    size_t pos = 0U;
    while (message[pos] != '\0' && *line_count < max_lines) {
        // Skip leading whitespace
        while (message[pos] == ' ' || message[pos] == '\t') {
            ++pos;
        }

        if (message[pos] == '\0') {
            break;
        }

        // Find where to wrap this line
        size_t next_pos =
            session_find_wrap_position(message, pos, kMaxVisibleWidth);

        // Extract the segment
        size_t segment_len = next_pos - pos;
        char segment[SSH_CHATTER_MESSAGE_LIMIT];
        if (segment_len >= sizeof(segment)) {
            segment_len = sizeof(segment) - 1U;
        }
        memcpy(segment, message + pos, segment_len);
        segment[segment_len] = '\0';

        // Remove trailing whitespace
        while (segment_len > 0U && (segment[segment_len - 1U] == ' ' ||
                                    segment[segment_len - 1U] == '\t')) {
            segment[--segment_len] = '\0';
        }

        // Format the line with ANSI codes
        size_t offset = 0U;
        offset = session_append_fragment(
            lines[*line_count], SSH_CHATTER_MESSAGE_LIMIT, offset, kPrefix);
        offset = session_append_fragment(
            lines[*line_count], SSH_CHATTER_MESSAGE_LIMIT, offset, segment);
        session_append_fragment(lines[*line_count], SSH_CHATTER_MESSAGE_LIMIT,
                                offset, kSuffix);

        (*line_count)++;
        pos = next_pos;
    }
}

static void session_bbs_format_breaking_notice(const char *message, char *out,
                                               size_t length)
{
    if (out == nullptr || length == 0U) {
        return;
    }

    out[0] = '\0';

    if (message == nullptr) {
        return;
    }

    size_t offset = 0U;
    offset = session_append_fragment(out, length, offset, "\r\033[2G");
    offset = session_append_fragment(out, length, offset, ANSI_BG_BRIGHT_BLUE);
    offset = session_append_fragment(out, length, offset, ANSI_BRIGHT_MAGENTA);
    offset = session_append_fragment(out, length, offset, ANSI_BOLD);
    offset = session_append_fragment(out, length, offset, message);
    offset = session_append_fragment(out, length, offset, "\033[K");
    session_append_fragment(out, length, offset, ANSI_RESET);
}

static void session_bbs_buffer_breaking_notice(session_ctx_t *ctx,
                                               const char *message)
{
    if (ctx == nullptr || message == nullptr) {
        return;
    }

    if (!ctx->breaking_alerts_enabled) {
        return;
    }

    const size_t kWrapThreshold = 76U;
    char formatted_lines[4][SSH_CHATTER_MESSAGE_LIMIT];
    size_t formatted_count = 0U;
    if (strlen(message) > kWrapThreshold) {
        session_bbs_format_breaking_notice_wrapped(
            message, formatted_lines,
            sizeof(formatted_lines) / sizeof(formatted_lines[0]),
            &formatted_count);
    } else {
        session_bbs_format_breaking_notice(
            message, formatted_lines[0], sizeof(formatted_lines[0]));
        formatted_count = 1U;
    }

    if (formatted_count == 0U) {
        session_bbs_format_breaking_notice(
            message, formatted_lines[0], sizeof(formatted_lines[0]));
        formatted_count = 1U;
    }

    session_send_plain_line(ctx, "");
    for (size_t idx = 0U; idx < formatted_count; ++idx) {
        session_send_plain_line(ctx, formatted_lines[idx]);
    }
    session_send_plain_line(ctx, "");
    session_bbs_render_editor(ctx, nullptr);
}

static bool session_should_hide_entry(session_ctx_t *ctx,
                                      const chat_history_entry_t *entry)
{
    if (ctx == nullptr || entry == nullptr) {
        return false;
    }

    if (!entry->is_user_message) {
        if (!ctx->breaking_alerts_enabled &&
            session_message_contains_breaking(entry->message)) {
            return true;
        }
        return false;
    }

    if (ctx->block_entry_count == 0U) {
        return false;
    }

    if (strncmp(entry->username, ctx->user.name, SSH_CHATTER_USERNAME_LEN) ==
        0) {
        return false;
    }

    char entry_ip[SSH_CHATTER_IP_LEN] = {0};
    if (ctx->owner != nullptr) {
        host_lookup_member_ip(ctx->owner, entry->username, entry_ip,
                              sizeof(entry_ip));
    }

    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_BLOCKED; ++idx) {
        const session_block_entry_t *block = &ctx->block_entries[idx];
        if (!block->in_use) {
            continue;
        }

        bool ip_match = false;
        bool user_match = false;

        if (block->ip[0] != '\0' && entry_ip[0] != '\0' &&
            strncmp(block->ip, entry_ip, SSH_CHATTER_IP_LEN) == 0) {
            ip_match = true;
        }

        if (block->username[0] != '\0' &&
            strncmp(block->username, entry->username,
                    SSH_CHATTER_USERNAME_LEN) == 0) {
            user_match = true;
        }

        if (block->ip_wide) {
            if (ip_match) {
                return true;
            }
            if (!ip_match && entry_ip[0] == '\0' && user_match) {
                return true;
            }
        } else {
            if (user_match) {
                return true;
            }
        }
    }

    return false;
}

// this displays a message to a chatting room.
void session_send_system_line(session_ctx_t *ctx, const char *message)
{
    if (ctx == nullptr || !session_transport_active(ctx) ||
        message == nullptr) {
        return;
    }

    session_output_kind_t previous_kind =
        session_output_set_kind(ctx, SESSION_OUTPUT_KIND_SYSTEM);

    if (session_bbs_should_defer_breaking(ctx, message)) {
        session_bbs_buffer_breaking_notice(ctx, message);
        session_output_restore_kind(ctx, previous_kind);
        return;
    }

    if (ctx->game
            .active) { // If game is active, send raw text without [system] prefix
        session_send_raw_text(ctx, message);
        session_output_restore_kind(ctx, previous_kind);
        return;
    }

    session_send_plain_line(ctx, message);

    session_output_restore_kind(ctx, previous_kind);
}

void session_send_raw_text(session_ctx_t *ctx, const char *text)
{
    if (ctx == nullptr || !session_transport_active(ctx) || text == nullptr) {
        return;
    }

    const char *cursor = text;
    while (*cursor != '\0') {
        const char *newline = strchr(cursor, '\n');
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        if (newline == nullptr) {
            snprintf(line, sizeof(line), "%.*s",
                     (int)(sizeof(line) - 1U), cursor);
            session_send_plain_line(ctx, line);
            break;
        }

        size_t length = (size_t)(newline - cursor);
        if (length >= sizeof(line)) {
            length = sizeof(line) - 1U;
        }
        memcpy(line, cursor, length);
        line[length] = '\0';
        session_send_plain_line(ctx, line);

        cursor = newline + 1;
        if (*cursor == '\r') {
            ++cursor;
        }
        if (*cursor == '\0') {
            session_send_plain_line(ctx, "");
        }
    }
}

/* Expand BBS color markup: (#RRGGBB)text(#end)
 * Renders the enclosed text with a bright white background and a 24-bit
 * foreground color. Any other text is passed through unchanged.
 * out is NUL-terminated and will not exceed out_size bytes. */
static void session_bbs_expand_color_markup(const char *text, char *out,
                                             size_t out_size)
{
    if (text == nullptr || out == nullptr || out_size == 0U) {
        if (out != nullptr && out_size > 0U) {
            out[0] = '\0';
        }
        return;
    }
    out[0] = '\0';

    size_t pos = 0U;
    const char *cursor = text;

    while (*cursor != '\0' && pos + 1U < out_size) {
        /* Check for (#end) - case-insensitive, 6 chars */
        if (cursor[0] == '(' && cursor[1] == '#' &&
            (cursor[2] == 'e' || cursor[2] == 'E') &&
            (cursor[3] == 'n' || cursor[3] == 'N') &&
            (cursor[4] == 'd' || cursor[4] == 'D') &&
            cursor[5] == ')') {
            static const char kReset[] = "\033[0m";
            const size_t seq_len = sizeof(kReset) - 1U;
            if (pos + seq_len + 1U <= out_size) {
                memcpy(out + pos, kReset, seq_len);
                pos += seq_len;
            }
            cursor += 6;
            continue;
        }

        /* Check for (#RRGGBB) - exactly 9 characters */
        if (cursor[0] == '(' && cursor[1] == '#' && cursor[8] == ')') {
            bool valid = true;
            for (int hex_idx = 2; hex_idx < 8; ++hex_idx) {
                if (!isxdigit((unsigned char)cursor[hex_idx])) {
                    valid = false;
                    break;
                }
            }
            if (valid) {
                unsigned int r = 0U, g = 0U, b = 0U;
                if (sscanf(cursor + 2, "%02x%02x%02x", &r, &g, &b) == 3) {
                    /* Bright white bg (\033[107m) + 24-bit fg */
                    char ansi_seq[48];
                    int written = snprintf(ansi_seq, sizeof(ansi_seq),
                                           "\033[107m\033[38;2;%u;%u;%um",
                                           r, g, b);
                    if (written > 0 &&
                        pos + (size_t)written + 1U <= out_size) {
                        memcpy(out + pos, ansi_seq, (size_t)written);
                        pos += (size_t)written;
                    }
                    cursor += 9;
                    continue;
                }
            }
        }

        out[pos++] = *cursor++;
    }

    out[pos] = '\0';
}

/* Like session_send_raw_text but expands BBS color markup on each line. */
static void session_send_bbs_body_text(session_ctx_t *ctx, const char *text)
{
    if (ctx == nullptr || !session_transport_active(ctx) || text == nullptr) {
        return;
    }

    const char *cursor = text;
    while (*cursor != '\0') {
        const char *newline = strchr(cursor, '\n');
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        char expanded[SSH_CHATTER_MESSAGE_LIMIT * 4U];
        if (newline == nullptr) {
            snprintf(line, sizeof(line), "%.*s",
                     (int)(sizeof(line) - 1U), cursor);
            session_bbs_expand_color_markup(line, expanded, sizeof(expanded));
            session_send_plain_line(ctx, expanded);
            break;
        }

        size_t length = (size_t)(newline - cursor);
        if (length >= sizeof(line)) {
            length = sizeof(line) - 1U;
        }
        memcpy(line, cursor, length);
        line[length] = '\0';
        session_bbs_expand_color_markup(line, expanded, sizeof(expanded));
        session_send_plain_line(ctx, expanded);

        cursor = newline + 1;
        if (*cursor == '\r') {
            ++cursor;
        }
        if (*cursor == '\0') {
            session_send_plain_line(ctx, "");
        }
    }
}

static void session_format_separator_line(session_ctx_t *ctx, const char *label,
                                          char *out, size_t length)
{
    if (out == nullptr || length == 0U) {
        return;
    }

    out[0] = '\0';

    if (ctx == nullptr || label == nullptr) {
        return;
    }

    const char *fg = ctx->system_fg_code != nullptr ? ctx->system_fg_code : "";
    const char *hl =
        ctx->system_highlight_code != nullptr ? ctx->system_highlight_code : "";
    const char *bold = ctx->system_is_bold ? ANSI_BOLD : "";

    size_t total_width = ctx->terminal_width > 0U ? ctx->terminal_width : 80U;
    if (total_width > SSH_CHATTER_MESSAGE_LIMIT) {
        total_width = SSH_CHATTER_MESSAGE_LIMIT;
    }
    char label_block[96];
    snprintf(label_block, sizeof(label_block), " %s ", label);
    size_t label_len = strnlen(label_block, sizeof(label_block) - 1U);
    if (label_len > total_width) {
        label_len = total_width;
        label_block[label_len] = '\0';
    }

    size_t dash_total = total_width > label_len ? total_width - label_len : 0U;
    size_t left = dash_total / 2U;
    size_t right = dash_total - left;

    enum { SESSION_SEPARATOR_BODY_LEN = SSH_CHATTER_MESSAGE_LIMIT - 16 };
    char body[SESSION_SEPARATOR_BODY_LEN];
    size_t max_body = sizeof(body);
    if (length > 0U && length < max_body) {
        max_body = length;
    }
    size_t offset = 0U;
    for (size_t idx = 0U; idx < left && offset + 1U < max_body; ++idx) {
        body[offset++] = '-';
    }
    if (offset + 1U < max_body) {
        size_t copy_limit = max_body - offset - 1U;
        size_t copy_len = label_len < copy_limit ? label_len : copy_limit;
        if (copy_len > 0U) {
            memcpy(body + offset, label_block, copy_len);
            offset += copy_len;
        }
    }
    for (size_t idx = 0U; idx < right && offset + 1U < max_body; ++idx) {
        body[offset++] = '-';
    }
    body[offset < max_body ? offset : max_body - 1U] = '\0';

    snprintf(out, length, "%s%s%s%.*s%s", hl, fg, bold,
             SESSION_SEPARATOR_BODY_LEN - 1, body, ANSI_RESET);
}

static void session_render_separator(session_ctx_t *ctx, const char *label)
{
    if (ctx == nullptr || label == nullptr) {
        return;
    }

    char line[SSH_CHATTER_MESSAGE_LIMIT];
    session_format_separator_line(ctx, label, line, sizeof(line));
    if (line[0] != '\0') {
        session_send_line(ctx, line);
    }
}


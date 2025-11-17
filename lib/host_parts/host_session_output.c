#include "host_internal.h"
#include "../headers/user_data.h"
#include "../headers/security_layer.h"

// Session output, history delivery, and client-facing helpers.

static void session_render_banner_text(session_ctx_t *ctx, const char *banner)
{
    if (ctx == nullptr || banner == nullptr) {
        return;
    }

    bool locked = session_output_lock(ctx);

    const char *cursor = banner;
    while (true) {
        const char *newline = strchr(cursor, '\n');
        size_t length = newline != nullptr
                            ? (size_t)(newline - cursor)
                            : strnlen(cursor, SSH_CHATTER_MESSAGE_LIMIT);
        while (length > 0U && cursor[length - 1U] == '\r') {
            --length;
        }

        session_fill_line_with_theme(ctx);
        static const char column_reset[] = "\033[1G";
        session_channel_write(ctx, column_reset, sizeof(column_reset) - 1U);
        if (length > 0U) {
            session_channel_write(ctx, cursor, length);
        }
        session_channel_write(ctx, ANSI_RESET, sizeof(ANSI_RESET) - 1U);
        session_channel_write(ctx, "\r\n", 2U);

        if (newline == nullptr) {
            break;
        }

        cursor = newline + 1;
        if (*cursor == '\0') {
            break;
        }
    }

    if (locked) {
        session_output_unlock(ctx);
    }
}

void host_set_welcome_banner(host_t *host, const char *banner)
{
    if (host == nullptr) {
        return;
    }

    if (banner == nullptr || banner[0] == '\0') {
        host->welcome_banner[0] = '\0';
        host->welcome_banner_loaded = false;
        return;
    }

    snprintf(host->welcome_banner, sizeof(host->welcome_banner), "%s", banner);
    host->welcome_banner_loaded = true;
}

static void session_game_show_camouflage(session_ctx_t *ctx);

static void session_send_plain_line(session_ctx_t *ctx, const char *message)
{
    if (ctx == nullptr || !session_transport_active(ctx) || message == nullptr) {
        return;
    }

    static const char kCaptionPrefix[] = "    \342\206\263";
    if (strncmp(message, kCaptionPrefix, sizeof(kCaptionPrefix) - 1U) == 0) {
        session_send_caption_line(ctx, message);
        return;
    }

    session_write_rendered_line(ctx, message);
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
    pthread_mutex_lock(&host->lock);
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
        pthread_mutex_unlock(&host->lock);
        return;
    }

    chat_reply_entry_t *snapshot = GC_CALLOC(match_count, sizeof(*snapshot));
    if (snapshot == nullptr) {
        pthread_mutex_unlock(&host->lock);
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
    pthread_mutex_unlock(&host->lock);

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
}

static bool host_lookup_member_ip(host_t *host, const char *username, char *ip,
                                  size_t length)
{
    if (host == nullptr || username == nullptr || ip == nullptr || length == 0U) {
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

static void session_copy_lowercase(const char *source, char *dest,
                                   size_t dest_len)
{
    if (dest == nullptr || dest_len == 0U) {
        return;
    }

    if (source == nullptr) {
        dest[0] = '\0';
        return;
    }

    size_t write = 0U;
    while (source[write] != '\0' && write + 1U < dest_len) {
        dest[write] = (char)tolower((unsigned char)source[write]);
        ++write;
    }
    dest[write] = '\0';
}

bool is_pure_ascii(const char *str)
{
    size_t len = strlen(str);
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)str[i] > 127) {
            return false;
        }
    }
    return true;
}

int count_unicode_points(const char *str, utf8_code_count_t **counts_out,
                         size_t *unique_count_out)
{
    size_t len = strnlen(str, SSH_CHATTER_MESSAGE_LIMIT);
    size_t unique_count = 0;
    size_t capacity = 100;
    utf8_code_count_t *counts =
        (utf8_code_count_t *)calloc(capacity, sizeof(utf8_code_count_t));

    if (!counts)
        return -1;

    size_t total_points = 0;
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
        total_points++;

        int found = 0;
        for (size_t j = 0; j < unique_count; j++) {
            if (counts[j].code_point == code_point) {
                counts[j].count++;
                found = 1;
                break;
            }
        }

        if (!found) {
            if (unique_count >= capacity) {
                capacity *= 2;
                utf8_code_count_t *new_counts = (utf8_code_count_t *)GC_REALLOC(
                    counts, capacity * sizeof(utf8_code_count_t));
                if (!new_counts) {
                    GC_FREE(counts);
                    return -1;
                }
                counts = new_counts;
            }
            counts[unique_count].code_point = code_point;
            counts[unique_count].count = 1;
            unique_count++;
        }
    }

    *counts_out = counts;
    *unique_count_out = unique_count;
    return (int)total_points;
}

double calculate_chi_squared(const char *str)
{
    utf8_code_count_t *counts = nullptr;
    size_t unique_count = 0;
    int N = count_unicode_points(str, &counts, &unique_count);

    if (N <= 0 || !counts || unique_count == 0) {
        if (counts)
            GC_FREE(counts);
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

    GC_FREE(counts);
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

static bool session_token_is_suspicious_command(const char *token)
{
    if (token == nullptr || token[0] == '\0') {
        return false;
    }

    static const char *const kExactCommands[] = {
        "ls",        "pwd",     "whoami",  "uname",    "id",       "ps",
        "cd",        "rm",      "chmod",   "chown",    "apt",      "apt-get",
        "aptitude",  "yum",     "dnf",     "apk",      "pacman",   "brew",
        "curl",      "wget",    "scp",     "ssh",      "sudo",     "service",
        "systemctl", "kill",    "tar",     "zip",      "unzip",    "nc",
        "netcat",    "bash",    "sh",      "shell",    "enable",   "system",
        "nconnect",  "busybox", "python",  "python2",  "python3",  "perl",
        "php",       "ruby",    "node",    "npm",      "npx",      "pip",
        "pip3",      "docker",  "kubectl", "ifconfig", "ipconfig", "powershell",
        "cmd",       "dir",     "tftp",    "ftpget",   "admin",    "bash",
        "su",        "root"};

    if (!is_pure_ascii(token) ||
        strnlen(token, SSH_CHATTER_MESSAGE_LIMIT) < 8) {
        return false;
    }

    if (is_string_random(token)) {
        return true;
    }

    for (size_t idx = 0U;
         idx < sizeof(kExactCommands) / sizeof(kExactCommands[0]); ++idx) {
        if (strcmp(token, kExactCommands[idx]) == 0) {
            return true;
        }
    }

    // Check for file path patterns like "./script" or "/bin/sh"
    // but NOT single-word commands like "/help"
    if (token[0] == '.' && token[1] == '/') {
        return true;
    }
    if (token[0] == '/') {
        // Check if it looks like a file path (contains another slash or 
        // is suspiciously long for a command name)
        const char *second_slash = strchr(token + 1, '/');
        if (second_slash != nullptr) {
            return true;
        }
        // Also flag if it's too long to be a legitimate command
        // (most commands are < 20 chars, file paths are often longer)
        size_t len = strlen(token);
        if (len > 30) {
            return true;
        }
    }

    if (strncmp(token, "python", 6) == 0) {
        char next = token[6];
        if (next == '\0' || next == '.' || (next >= '0' && next <= '9')) {
            return true;
        }
    }

    if (strncmp(token, "pip", 3) == 0) {
        char next = token[3];
        if (next == '\0' || next == '3' || next == '.') {
            return true;
        }
    }

    if (strncmp(token, "node", 4) == 0) {
        char next = token[4];
        if (next == '\0' || next == '.' || (next >= '0' && next <= '9')) {
            return true;
        }
    }

    if (strncmp(token, "npm", 3) == 0) {
        char next = token[3];
        if (next == '\0' || next == '-' || (next >= '0' && next <= '9')) {
            return true;
        }
    }

    return false;
}

static bool session_line_contains_shell_operator(const char *text)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }

    if (strstr(text, "&&") != nullptr || strstr(text, "||") != nullptr ||
        strstr(text, "$(") != nullptr || strchr(text, '`') != nullptr) {
        return true;
    }

    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        char ch = *cursor;
        if (ch == ';') {
            char prev = (cursor > text) ? cursor[-1] : '\0';
            char next = cursor[1];
            bool prev_sep = prev == '\0' || isspace((unsigned char)prev) ||
                            prev == ';' || prev == '&' || prev == '|';
            bool next_sep = next == '\0' || isspace((unsigned char)next) ||
                            next == '&' || next == '|' || next == ';';
            if (prev_sep || next_sep) {
                return true;
            }
        } else if (ch == '|') {
            char prev = (cursor > text) ? cursor[-1] : '\0';
            char next = cursor[1];
            if (next == '|' || isspace((unsigned char)next) ||
                isspace((unsigned char)prev)) {
                return true;
            }
        } else if (ch == '>') {
            char prev = (cursor > text) ? cursor[-1] : '\0';
            char next = cursor[1];
            if (next == '>' || next == '&' || isspace((unsigned char)next) ||
                isspace((unsigned char)prev)) {
                return true;
            }
        } else if (ch == '<') {
            char prev = (cursor > text) ? cursor[-1] : '\0';
            char next = cursor[1];
            if (next == '<' || isspace((unsigned char)next) ||
                isspace((unsigned char)prev)) {
                return true;
            }
        }
    }

    return false;
}

static bool session_line_contains_suspicious_keyword(const char *text)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }

    static const char *const kKeywords[] = {
        "sudo ", "rm -",    "chmod ", "chown ", "wget ",
        "curl ", "apt-get", " apt ",  "dnf ",   "yum ",
        "scp ",  "ssh ",    "enable", "system", "nconnect"};

    for (size_t idx = 0U; idx < sizeof(kKeywords) / sizeof(kKeywords[0]);
         ++idx) {
        const char *keyword = kKeywords[idx];
        const char *match = text;
        while ((match = strstr(match, keyword)) != nullptr) {
            char before = (match == text) ? ' ' : match[-1];
            if (isspace((unsigned char)before) || before == ';' ||
                before == '|' || before == '&') {
                return true;
            }
            match += strlen(keyword);
        }
    }

    return false;
}

static bool session_line_matches_telnet_bot_pattern(const char *text)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }

    static const char *const kPatterns[] = {
        "busybox",   " cd /tmp",     " cd /var/run", " cd /var/tmp",
        " cd /tmp/", "wget http",    "curl http",    "tftp ",
        "ftpget ",   "/bin/busybox", "/bin/sh",      "/sbin/ifconfig",
        "chmod 777", ">/dev/null",   "killall",      "pkill",
        "udpflood",  "tcpflood",     "nconnect"};

    for (size_t idx = 0U; idx < sizeof(kPatterns) / sizeof(kPatterns[0]);
         ++idx) {
        if (strstr(text, kPatterns[idx]) != nullptr) {
            return true;
        }
    }

    return false;
}

static bool session_first_message_is_suspicious(session_ctx_t *ctx,
                                                const char *message)
{
    if (ctx == nullptr || message == nullptr) {
        return false;
    }

    if (!ctx->has_joined_room || ctx->chat_message_count > 0U) {
        return false;
    }

    char working[SSH_CHATTER_MAX_INPUT_LEN];
    snprintf(working, sizeof(working), "%s", message);
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        return false;
    }

    char lower_message[sizeof(working)];
    session_copy_lowercase(working, lower_message, sizeof(lower_message));

    char username_lower[SSH_CHATTER_USERNAME_LEN];
    session_copy_lowercase(ctx->user.name, username_lower,
                           sizeof(username_lower));
    if (strcmp(lower_message, username_lower) == 0) {
        return true;
    }

    if (username_lower[0] != '\0') {
        const size_t username_len = strlen(username_lower);
        size_t remaining = 0U;
        bool found_username = false;
        const char *cursor = lower_message;
        while (*cursor != '\0') {
            if (strncmp(cursor, username_lower, username_len) == 0) {
                cursor += username_len;
                found_username = true;
                continue;
            }
            if (!isspace((unsigned char)*cursor)) {
                ++remaining;
            }
            ++cursor;
        }
        if (found_username && remaining <= 3U) {
            return true;
        }
    }

    const char *cursor = working;
    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }

    char token[SSH_CHATTER_MAX_INPUT_LEN];
    size_t token_len = 0U;
    while (*cursor != '\0' && !isspace((unsigned char)*cursor) &&
           *cursor != ';' && *cursor != '&' && *cursor != '|' &&
           *cursor != '>' && *cursor != '<') {
        if (token_len + 1U >= sizeof(token)) {
            break;
        }
        token[token_len++] = (char)tolower((unsigned char)*cursor);
        ++cursor;
    }
    token[token_len] = '\0';

    if (session_token_is_suspicious_command(token)) {
        return true;
    }

    if (session_line_contains_shell_operator(lower_message)) {
        return true;
    }

    if (session_line_contains_suspicious_keyword(lower_message)) {
        return true;
    }

    if (session_line_matches_telnet_bot_pattern(lower_message)) {
        return true;
    }

    return false;
}

static void session_handle_suspicious_first_message(session_ctx_t *ctx,
                                                    const char *message)
{
    if (ctx == nullptr) {
        return;
    }

    char trimmed[SSH_CHATTER_MAX_INPUT_LEN];
    snprintf(trimmed, sizeof(trimmed), "%s", message != nullptr ? message : "");
    trim_whitespace_inplace(trimmed);

    const char *ip = nullptr;
    if (ctx->client_ip[0] != '\0' &&
        strncmp(ctx->client_ip, "unknown", SSH_CHATTER_IP_LEN) != 0) {
        ip = ctx->client_ip;
    }

    const char *address = (ip != nullptr) ? ip : "unknown";
    printf("\[security] suspicious first message from %s (%s): %s\n",
           ctx->user.name, address, trimmed[0] != '\0' ? trimmed : "(empty)");

    host_t *host = ctx->owner;
    if (host != nullptr) {
        (void)host_history_remove_join_entry(host, ctx->user.name);
        const char *ban_ip = (ip != nullptr) ? ip : "";
        if (host_add_ban_entry(host, ctx->user.name, ban_ip)) {
            printf("\[security] permanently banned %s (%s) for "
                   "suspicious first "
                   "message\n",
                   ctx->user.name, address);
        } else {
            printf("[security] unable to record permanent ban for %s "
                   "(%s)\n",
                   ctx->user.name, address);
        }

        const char *admin_user = getenv("ADMIN1");
        if (admin_user != nullptr && admin_user[0] != '\0') {
            char admin_ip[SSH_CHATTER_IP_LEN];
            admin_ip[0] = '\0';
            if (!host_lookup_member_ip(host, admin_user, admin_ip,
                                       sizeof(admin_ip))) {
                (void)host_lookup_last_ip(host, admin_user, admin_ip,
                                          sizeof(admin_ip));
            }

            char notification[USER_DATA_MAILBOX_MESSAGE_LEN];
            snprintf(notification, sizeof(notification),
                     "Suspicious first message ban: [%s] from %s sent '%s'.",
                     ctx->user.name, address,
                     trimmed[0] != '\0' ? trimmed : "(empty)");

            char mail_error[128];
            if (!host_user_data_send_mail(
                    host, admin_user, admin_ip[0] != '\0' ? admin_ip : nullptr,
                    "system", notification, mail_error, sizeof(mail_error))) {
                if (mail_error[0] != '\0') {
                    printf("[mail] failed to notify %s about "
                           "suspicious first "
                           "message: %s\n",
                           admin_user, mail_error);
                } else {
                    printf("[mail] failed to notify %s about "
                           "suspicious first "
                           "message\n",
                           admin_user);
                }
            } else {
                printf("[mail] notified %s about suspicious first "
                       "message from "
                       "%s\n",
                       admin_user, ctx->user.name);
            }
        }
    }

    ctx->user_data_loaded = false;
    memset(&ctx->user_data, 0, sizeof(ctx->user_data));

    session_force_disconnect(ctx, "Suspicious command activity detected. You "
                                  "have been permanently banned.");
}

static bool session_detect_provider_ip(const char *ip, char *label,
                                       size_t length)
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

    if (strstr(message, "속보") != nullptr || strstr(message, "速報") != nullptr) {
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
    const size_t kMaxVisibleWidth = 76U;

    // ANSI codes to apply
    const char *kPrefix = ANSI_BG_BRIGHT_BLUE ANSI_BRIGHT_MAGENTA ANSI_BOLD;
    const char *kSuffix = ANSI_RESET;

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
    offset = session_append_fragment(out, length, offset, ANSI_BG_BRIGHT_BLUE);
    offset = session_append_fragment(out, length, offset, ANSI_BRIGHT_MAGENTA);
    offset = session_append_fragment(out, length, offset, ANSI_BOLD);
    offset = session_append_fragment(out, length, offset, message);
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

    // Check message length to decide if wrapping is needed
    size_t msg_len = strlen(message);
    const size_t kWrapThreshold = 76U; // Wrap if message is longer than this

    if (msg_len > kWrapThreshold) {
        // Use wrapped formatting for long messages
        char wrapped_lines[4][SSH_CHATTER_MESSAGE_LIMIT];
        size_t wrapped_count = 0U;
        session_bbs_format_breaking_notice_wrapped(message, wrapped_lines, 4U,
                                                   &wrapped_count);

        size_t limit = SSH_CHATTER_BBS_BREAKING_MAX;
        if (limit == 0U) {
            return;
        }

        // Add each wrapped line as a separate breaking message
        for (size_t i = 0U; i < wrapped_count; ++i) {
            if (ctx->bbs_breaking_count < limit) {
                snprintf(
                    ctx->bbs_breaking_messages[ctx->bbs_breaking_count],
                    sizeof(ctx->bbs_breaking_messages[ctx->bbs_breaking_count]),
                    "%s", wrapped_lines[i]);
                ctx->bbs_breaking_count += 1U;
            } else {
                // Shift messages up and add new one at the end
                for (size_t idx = 1U; idx < limit; ++idx) {
                    snprintf(ctx->bbs_breaking_messages[idx - 1U],
                             sizeof(ctx->bbs_breaking_messages[idx - 1U]), "%s",
                             ctx->bbs_breaking_messages[idx]);
                }
                snprintf(ctx->bbs_breaking_messages[limit - 1U],
                         sizeof(ctx->bbs_breaking_messages[limit - 1U]), "%s",
                         wrapped_lines[i]);
            }
        }
    } else {
        // Use original formatting for short messages
        char formatted[SSH_CHATTER_MESSAGE_LIMIT];
        session_bbs_format_breaking_notice(message, formatted,
                                           sizeof(formatted));

        size_t limit = SSH_CHATTER_BBS_BREAKING_MAX;
        if (limit == 0U) {
            return;
        }

        if (ctx->bbs_breaking_count < limit) {
            snprintf(
                ctx->bbs_breaking_messages[ctx->bbs_breaking_count],
                sizeof(ctx->bbs_breaking_messages[ctx->bbs_breaking_count]),
                "%s", formatted);
            ctx->bbs_breaking_count += 1U;
        } else {
            for (size_t idx = 1U; idx < limit; ++idx) {
                snprintf(ctx->bbs_breaking_messages[idx - 1U],
                         sizeof(ctx->bbs_breaking_messages[idx - 1U]), "%s",
                         ctx->bbs_breaking_messages[idx]);
            }
            snprintf(ctx->bbs_breaking_messages[limit - 1U],
                     sizeof(ctx->bbs_breaking_messages[limit - 1U]), "%s",
                     formatted);
        }
    }

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
static void session_send_system_line(session_ctx_t *ctx, const char *message)
{
    if (ctx == nullptr || !session_transport_active(ctx) || message == nullptr) {
        return;
    }

    if (session_bbs_should_defer_breaking(ctx, message)) {
        session_bbs_buffer_breaking_notice(ctx, message);
        return;
    }

    if (ctx->game
            .active) { // If game is active, send raw text without [system] prefix
        session_send_raw_text(ctx, message);
        return;
    }

    char prefixed_message[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(prefixed_message, sizeof(prefixed_message), "+ %s", message);

    session_send_plain_line(ctx, prefixed_message);
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
            snprintf(line, sizeof(line), "%s", cursor);
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

    const size_t total_width = 80U;
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

    char body[128];
    size_t offset = 0U;
    for (size_t idx = 0U; idx < left && offset + 1U < sizeof(body); ++idx) {
        body[offset++] = '-';
    }
    if (offset + label_len < sizeof(body)) {
        memcpy(body + offset, label_block, label_len);
        offset += label_len;
    }
    for (size_t idx = 0U; idx < right && offset < sizeof(body); ++idx) {
        body[offset++] = '-';
    }
    body[offset] = '\0';

    snprintf(out, length, "%s%s%s%s%s", hl, fg, bold, body, ANSI_RESET);
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

static void session_enable_alternate_screen(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    // Switch to alternate screen buffer and disable scrolling
    // \033[?1049h = enable alternate screen buffer (saves current screen)
    // \033[2J = clear screen
    // \033[H = move cursor to home position
    static const char kEnableAltScreen[] = "\033[?1049h\033[2J\033[H";
    session_channel_write(ctx, kEnableAltScreen, sizeof(kEnableAltScreen) - 1U);
}

static void session_disable_alternate_screen(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    // Switch back to main screen buffer (restores previous screen)
    // \033[?1049l = disable alternate screen buffer
    static const char kDisableAltScreen[] = "\033[?1049l";
    session_channel_write(ctx, kDisableAltScreen,
                          sizeof(kDisableAltScreen) - 1U);
}

static void session_clear_screen(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    static const char kClearSequence[] = "\033[2J\033[H";
    session_channel_write(ctx, kClearSequence, sizeof(kClearSequence) - 1U);
}

static void session_bbs_prepare_canvas(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    session_clear_screen(ctx);
    session_apply_background_fill(ctx);
}

static void session_bbs_render_post(session_ctx_t *ctx, const bbs_post_t *post,
                                    const char *notice, bool reset_scroll)
{
    if (ctx == nullptr || post == nullptr) {
        return;
    }

    // Start buffering to send entire post in one flush
    session_output_buffer_start(ctx);

    session_bbs_prepare_canvas(ctx);

    if (reset_scroll) {
        ctx->bbs_view_scroll_offset = 0U;
    }

    char title_line[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(title_line, sizeof(title_line), "#%" PRIu64 ": %s", post->id,
             post->title);

    char author_line[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(author_line, sizeof(author_line), "Author: %s", post->author);

    char created_line[SSH_CHATTER_MESSAGE_LIMIT];
    time_t created = (time_t)post->created_at;
    struct tm created_tm;
    localtime_r(&created, &created_tm);
    strftime(created_line, sizeof(created_line), "Created: %Y-%m-%d %H:%M:%S",
             &created_tm);

    char bumped_line[SSH_CHATTER_MESSAGE_LIMIT];
    time_t bumped = (time_t)post->bumped_at;
    struct tm bumped_tm;
    localtime_r(&bumped, &bumped_tm);
    strftime(bumped_line, sizeof(bumped_line),
             "Last activity: %Y-%m-%d %H:%M:%S", &bumped_tm);

    session_send_plain_line(ctx, title_line);
    session_send_plain_line(ctx, author_line);
    session_send_plain_line(ctx, created_line);
    session_send_plain_line(ctx, bumped_line);
    session_send_plain_line(ctx, SSH_CHATTER_BBS_EDITOR_BODY_DIVIDER);

    // Send body line by line
    session_send_raw_text(ctx, post->body);

    if (notice != nullptr && notice[0] != '\0') {
        session_send_system_line(ctx, notice);
    }

    session_render_prompt(ctx, true);

    // Flush all buffered output at once
    session_output_buffer_stop(ctx);
}

static void session_bbs_recalculate_line_count(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    size_t count = 0U;
    if (ctx->pending_bbs_body_length > 0U) {
        count = 1U;
        for (size_t idx = 0U; idx < ctx->pending_bbs_body_length; ++idx) {
            if (ctx->pending_bbs_body[idx] == '\n') {
                ++count;
            }
        }
    }

    ctx->pending_bbs_line_count = count;
    if (ctx->pending_bbs_cursor_line > count) {
        ctx->pending_bbs_cursor_line = count;
        ctx->pending_bbs_editing_line = false;
    }
}

static bool session_bbs_get_line_range(const session_ctx_t *ctx,
                                       size_t line_index, size_t *start,
                                       size_t *length)
{
    if (ctx == nullptr || start == nullptr || length == nullptr) {
        return false;
    }

    if (line_index >= ctx->pending_bbs_line_count) {
        return false;
    }

    size_t offset = 0U;
    size_t current = 0U;
    while (current < line_index && offset < ctx->pending_bbs_body_length) {
        const char *newline = memchr(ctx->pending_bbs_body + offset, '\n',
                                     ctx->pending_bbs_body_length - offset);
        if (newline == nullptr) {
            return false;
        }
        offset = (size_t)(newline - ctx->pending_bbs_body) + 1U;
        ++current;
    }

    if (offset > ctx->pending_bbs_body_length) {
        return false;
    }

    size_t end = ctx->pending_bbs_body_length;
    const char *newline = memchr(ctx->pending_bbs_body + offset, '\n',
                                 ctx->pending_bbs_body_length - offset);
    if (newline != nullptr) {
        end = (size_t)(newline - ctx->pending_bbs_body);
    }

    *start = offset;
    *length = end - offset;
    return true;
}

static void session_bbs_copy_line(const session_ctx_t *ctx, size_t line_index,
                                  char *buffer, size_t length)
{
    if (buffer == nullptr || length == 0U) {
        return;
    }

    buffer[0] = '\0';
    size_t start = 0U;
    size_t line_length = 0U;
    if (!session_bbs_get_line_range(ctx, line_index, &start, &line_length)) {
        return;
    }

    if (line_length >= length) {
        line_length = length - 1U;
    }

    if (line_length > 0U) {
        memcpy(buffer, ctx->pending_bbs_body + start, line_length);
    }
    buffer[line_length] = '\0';
}

static size_t session_editor_body_capacity(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return 0U;
    }

    if (ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART) {
        if (ctx->asciiart_target == SESSION_ASCIIART_TARGET_PROFILE_PICTURE &&
            USER_DATA_PROFILE_PICTURE_LEN < SSH_CHATTER_ASCIIART_BUFFER_LEN) {
            return USER_DATA_PROFILE_PICTURE_LEN;
        }
        return SSH_CHATTER_ASCIIART_BUFFER_LEN;
    }

    return sizeof(ctx->pending_bbs_body);
}

static size_t session_editor_max_lines(const session_ctx_t *ctx)
{
    if (ctx != nullptr) {
        if (ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART) {
            return SSH_CHATTER_ASCIIART_MAX_LINES;
        }
        if (ctx->editor_mode == SESSION_EDITOR_MODE_BBS_CREATE ||
            ctx->editor_mode == SESSION_EDITOR_MODE_BBS_EDIT) {
            return SSH_CHATTER_BBS_MAX_LINES;
        }
    }

    return SIZE_MAX;
}

static bool session_bbs_append_line(session_ctx_t *ctx, const char *line,
                                    char *status, size_t status_length)
{
    if (ctx == nullptr) {
        return false;
    }

    const bool ascii_mode = ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART;
    if (status != nullptr && status_length > 0U) {
        status[0] = '\0';
    }

    if (line == nullptr) {
        line = "";
    }

    const char *length_limit_message =
        ascii_mode ? "ASCII art buffer is full. Additional text ignored."
                   : "Post body length limit reached. Additional text ignored.";
    const char *line_limit_message =
        ascii_mode ? "ASCII art line limit reached. Additional text ignored."
                   : "Post line limit reached. Additional text ignored.";
    const char *line_truncated_message =
        ascii_mode ? "Line truncated to fit within the ASCII art size limit."
                   : "Line truncated to fit within the post size limit.";

    size_t capacity = session_editor_body_capacity(ctx);
    if (capacity == 0U) {
        if (status != nullptr && status_length > 0U) {
            snprintf(status, status_length, "%s", length_limit_message);
        }
        return false;
    }

    if (ctx->pending_bbs_body_length >= capacity - 1U) {
        if (status != nullptr && status_length > 0U) {
            snprintf(status, status_length, "%s", length_limit_message);
        }
        return false;
    }

    session_bbs_recalculate_line_count(ctx);
    size_t max_lines = session_editor_max_lines(ctx);
    if (ctx->pending_bbs_line_count >= max_lines) {
        if (status != nullptr && status_length > 0U) {
            snprintf(status, status_length, "%s", line_limit_message);
        }
        return false;
    }

    size_t available = capacity - ctx->pending_bbs_body_length - 1U;
    if (available == 0U) {
        if (status != nullptr && status_length > 0U) {
            snprintf(status, status_length, "%s", length_limit_message);
        }
        return false;
    }

    bool needs_newline = ctx->pending_bbs_body_length > 0U;
    if (needs_newline) {
        if (available == 0U) {
            if (status != nullptr && status_length > 0U) {
                snprintf(status, status_length, "%s", length_limit_message);
            }
            return false;
        }
        ctx->pending_bbs_body[ctx->pending_bbs_body_length++] = '\n';
        --available;
    }

    size_t line_length = strlen(line);
    if (line_length > available) {
        line_length = available;
        if (status != nullptr && status_length > 0U) {
            snprintf(status, status_length, "%s", line_truncated_message);
        }
    }

    if (line_length > 0U) {
        memcpy(ctx->pending_bbs_body + ctx->pending_bbs_body_length, line,
               line_length);
        ctx->pending_bbs_body_length += line_length;
    }

    ctx->pending_bbs_body[ctx->pending_bbs_body_length] = '\0';
    session_bbs_recalculate_line_count(ctx);
    ctx->pending_bbs_cursor_line = ctx->pending_bbs_line_count;
    ctx->pending_bbs_editing_line = false;
    return true;
}

static bool session_bbs_replace_line(session_ctx_t *ctx, size_t line_index,
                                     const char *line, char *status,
                                     size_t status_length)
{
    if (ctx == nullptr || line == nullptr) {
        return false;
    }

    const bool ascii_mode = ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART;
    if (status != nullptr && status_length > 0U) {
        status[0] = '\0';
    }

    session_bbs_recalculate_line_count(ctx);
    if (line_index >= ctx->pending_bbs_line_count) {
        if (status != nullptr && status_length > 0U) {
            snprintf(status, status_length,
                     "Unable to locate the selected line.");
        }
        return false;
    }

    size_t start = 0U;
    size_t old_length = 0U;
    if (!session_bbs_get_line_range(ctx, line_index, &start, &old_length)) {
        if (status != nullptr && status_length > 0U) {
            snprintf(status, status_length,
                     "Unable to locate the selected line.");
        }
        return false;
    }

    size_t current_length = ctx->pending_bbs_body_length;
    size_t capacity = session_editor_body_capacity(ctx);
    if (capacity == 0U) {
        return false;
    }
    if (capacity > 0U) {
        --capacity;
    }
    size_t base_length = current_length - old_length;
    size_t max_allowed =
        (capacity > base_length) ? (capacity - base_length) : 0U;

    size_t new_length = strlen(line);
    if (new_length > max_allowed) {
        new_length = max_allowed;
        if (status != nullptr && status_length > 0U) {
            const char *line_truncated_message =
                ascii_mode
                    ? "Line truncated to fit within the ASCII art size limit."
                    : "Line truncated to fit within the post size limit.";
            snprintf(status, status_length, "%s", line_truncated_message);
        }
    }

    size_t tail_offset = start + old_length;
    size_t tail_bytes = current_length - tail_offset + 1U;

    if (new_length > old_length) {
        size_t shift = new_length - old_length;
        memmove(ctx->pending_bbs_body + tail_offset + shift,
                ctx->pending_bbs_body + tail_offset, tail_bytes);
    } else if (old_length > new_length) {
        size_t shift = old_length - new_length;
        memmove(ctx->pending_bbs_body + tail_offset - shift,
                ctx->pending_bbs_body + tail_offset, tail_bytes);
        tail_offset -= shift;
    }

    if (new_length > 0U) {
        memcpy(ctx->pending_bbs_body + start, line, new_length);
    }

    ctx->pending_bbs_body_length = base_length + new_length;
    ctx->pending_bbs_body[ctx->pending_bbs_body_length] = '\0';

    session_bbs_recalculate_line_count(ctx);
    size_t updated_count = ctx->pending_bbs_line_count;
    if (line_index + 1U <= updated_count) {
        ctx->pending_bbs_cursor_line = line_index + 1U;
    } else {
        ctx->pending_bbs_cursor_line = updated_count;
    }
    ctx->pending_bbs_editing_line = false;
    return true;
}

static void session_bbs_render_editor(session_ctx_t *ctx, const char *status)
{
    if (ctx == nullptr) {
        return;
    }

    // Start buffering to send entire editor screen in one flush
    session_output_buffer_start(ctx);

    const bool ascii_mode = ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART;
    const bool editing_post = ctx->editor_mode == SESSION_EDITOR_MODE_BBS_EDIT;

    ctx->bbs_view_active = false;
    ctx->bbs_view_post_id = 0U;
    ctx->bbs_rendering_editor = true;

    session_bbs_prepare_canvas(ctx);
    session_bbs_recalculate_line_count(ctx);
    size_t line_count = ctx->pending_bbs_line_count;

    // Send title line
    if (ascii_mode) {
        const char *target_label =
            (ctx->asciiart_target == SESSION_ASCIIART_TARGET_PROFILE_PICTURE)
                ? "profile picture"
                : "chat";
        char title_line[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(title_line, sizeof(title_line),
                 "ASCII art %s draft (%zu/%u lines)", target_label, line_count,
                 (unsigned int)SSH_CHATTER_ASCIIART_MAX_LINES);
        session_send_plain_line(ctx, title_line);
    } else {
        char title_line[SSH_CHATTER_MESSAGE_LIMIT];
        if (editing_post) {
            if (ctx->pending_bbs_edit_id != 0U) {
                snprintf(title_line, sizeof(title_line),
                         "Editing post #%" PRIu64 " '%s'",
                         ctx->pending_bbs_edit_id, ctx->pending_bbs_title);
            } else {
                snprintf(title_line, sizeof(title_line), "Editing '%s'",
                         ctx->pending_bbs_title);
            }
        } else {
            snprintf(title_line, sizeof(title_line), "Composing '%s'",
                     ctx->pending_bbs_title);
        }
        session_send_plain_line(ctx, title_line);

        // Send tags line
        char tag_buffer[SSH_CHATTER_BBS_MAX_TAGS *
                        (SSH_CHATTER_BBS_TAG_LEN + 2U)];
        tag_buffer[0] = '\0';
        size_t offset = 0U;
        for (size_t idx = 0U; idx < ctx->pending_bbs_tag_count; ++idx) {
            size_t remaining = sizeof(tag_buffer) - offset;
            if (remaining == 0U) {
                break;
            }
            int written =
                snprintf(tag_buffer + offset, remaining, "%s%s",
                         idx > 0U ? "," : "", ctx->pending_bbs_tags[idx]);
            if (written < 0) {
                break;
            }
            if ((size_t)written >= remaining) {
                offset = sizeof(tag_buffer) - 1U;
                break;
            }
            offset += (size_t)written;
        }

        char tags_line[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(tags_line, sizeof(tags_line), "Tags: %s",
                 tag_buffer[0] != '\0' ? tag_buffer : "(none)");
        session_send_plain_line(ctx, tags_line);
    }

    // Send divider
    session_send_plain_line(ctx, SSH_CHATTER_BBS_EDITOR_BODY_DIVIDER);

    // Send body lines individually
    if (line_count == 0U) {
        const char *prefix = ctx->pending_bbs_editing_line ? "> " : "> ";
        session_send_plain_line(ctx, prefix);
    } else {
        for (size_t idx = 0U; idx < line_count; ++idx) {
            char line_buffer[SSH_CHATTER_MESSAGE_LIMIT];
            session_bbs_copy_line(ctx, idx, line_buffer, sizeof(line_buffer));
            bool selected = ctx->pending_bbs_editing_line &&
                            ctx->pending_bbs_cursor_line == idx;
            const char *prefix = selected ? "> " : "  ";
            char display[SSH_CHATTER_MESSAGE_LIMIT];
            if (line_buffer[0] == '\0') {
                snprintf(display, sizeof(display), "%s", prefix);
            } else {
                snprintf(display, sizeof(display), "%s%s", prefix, line_buffer);
            }
            session_send_plain_line(ctx, display);
        }
        if (!ctx->pending_bbs_editing_line) {
            session_send_plain_line(ctx, "> ");
        }
    }

    // Send end divider
    session_send_plain_line(ctx, SSH_CHATTER_BBS_EDITOR_END_DIVIDER);

    // Send remaining bytes info
    size_t capacity = session_editor_body_capacity(ctx);
    size_t remaining = 0U;
    if (capacity > ctx->pending_bbs_body_length) {
        remaining = capacity - ctx->pending_bbs_body_length - 1U;
    }
    char remaining_line[64];
    snprintf(remaining_line, sizeof(remaining_line), "Remaining bytes: %zu",
             remaining);
    session_send_plain_line(ctx, remaining_line);

    // Send hints
    const char *terminator = session_editor_terminator(ctx);
    char shortcut_hint[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(shortcut_hint, sizeof(shortcut_hint),
             "Ctrl+S(or Save) inserts %s. Ctrl+A(or Abort) cancels the draft.",
             terminator);
    session_send_plain_line(ctx, shortcut_hint);
    session_send_plain_line(ctx, "Use Up/Down arrows to revisit a saved line "
                                 "and press Enter to store changes.");

    char publish_hint[SSH_CHATTER_MESSAGE_LIMIT];
    if (ascii_mode) {
        snprintf(publish_hint, sizeof(publish_hint),
                 "Typing %s on its own line will finish the artwork.",
                 terminator);
    } else if (editing_post) {
        snprintf(publish_hint, sizeof(publish_hint),
                 "Typing %s on its own line will update the post.", terminator);
    } else {
        snprintf(publish_hint, sizeof(publish_hint),
                 "Typing %s on its own line will publish the post.",
                 terminator);
    }
    session_send_plain_line(ctx, publish_hint);

    // Send breaking updates if any
    if (ctx->breaking_alerts_enabled && ctx->bbs_breaking_count > 0U &&
        !ascii_mode) {
        session_send_plain_line(ctx, "");
        session_send_plain_line(ctx, "Breaking updates:");
        for (size_t idx = 0U; idx < ctx->bbs_breaking_count; ++idx) {
            session_send_plain_line(ctx, ctx->bbs_breaking_messages[idx]);
        }
    }

    // Send status if any
    if (status != nullptr && status[0] != '\0') {
        char working[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(working, sizeof(working), "%s", status);
        char *cursor = working;
        while (cursor != nullptr && *cursor != '\0') {
            char *newline = strchr(cursor, '\n');
            if (newline != nullptr) {
                *newline = '\0';
            }
            if (*cursor != '\0') {
                session_send_plain_line(ctx, cursor);
            }
            if (newline == nullptr) {
                break;
            }
            cursor = newline + 1;
        }
    }

    session_render_prompt(ctx, false);

    // Flush all buffered output at once
    session_output_buffer_stop(ctx);

    ctx->bbs_rendering_editor = false;
}

static void session_bbs_move_cursor(session_ctx_t *ctx, int direction)
{
    if (ctx == nullptr || direction == 0) {
        return;
    }

    session_bbs_recalculate_line_count(ctx);
    size_t line_count = ctx->pending_bbs_line_count;

    if (line_count == 0U) {
        ctx->pending_bbs_cursor_line = 0U;
        ctx->pending_bbs_editing_line = false;
        session_set_input_text(ctx, "");
        session_bbs_render_editor(ctx, nullptr);
        return;
    }

    size_t target = ctx->pending_bbs_cursor_line;
    bool editing = ctx->pending_bbs_editing_line;
    if (target > line_count) {
        target = line_count;
    }

    if (direction < 0) {
        if (!editing) {
            target = line_count - 1U;
            editing = true;
        } else {
            if (target > 0U) {
                --target;
            }
        }
    } else {
        if (editing) {
            if (target + 1U < line_count) {
                ++target;
            } else {
                target = line_count;
                editing = false;
            }
        }
    }

    ctx->pending_bbs_cursor_line = target;
    ctx->pending_bbs_editing_line = editing;

    char status[64];
    status[0] = '\0';

    if (editing && target < line_count) {
        char line_buffer[SSH_CHATTER_MAX_INPUT_LEN];
        session_bbs_copy_line(ctx, target, line_buffer, sizeof(line_buffer));
        session_set_input_text(ctx, line_buffer);
        snprintf(status, sizeof(status), "Editing line %zu of %zu.",
                 target + 1U, line_count);
    } else {
        session_set_input_text(ctx, "");
        snprintf(status, sizeof(status), "Editing new line %zu.",
                 line_count + 1U);
    }

    session_bbs_render_editor(ctx, status);
}

void session_render_banner_ascii(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    const char *banner_text = "Welcome to CHATTER!";
    if (ctx->owner != nullptr && ctx->owner->welcome_banner_loaded &&
        ctx->owner->welcome_banner[0] != '\0') {
        banner_text = ctx->owner->welcome_banner;
    }

    session_render_banner_text(ctx, banner_text);
}

static void session_render_prelogin_banner(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->prelogin_banner_rendered) {
        return;
    }

    session_apply_background_fill(ctx);

    session_render_banner_ascii(ctx);

    session_send_plain_line(ctx, "Connection established.");
    session_send_plain_line(ctx,
                            "Authenticate or choose a nickname to continue.");
    session_send_plain_line(ctx, "/retro on for CP-437 DOS compatibility.");

    ctx->prelogin_banner_rendered = true;
}

static void session_render_banner(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    session_apply_background_fill(ctx);

    bool show_graphics = !ctx->prelogin_banner_rendered;
    if (show_graphics) {
        session_render_banner_ascii(ctx);
    }

    session_render_separator(ctx, "Chatroom");
}

static void session_fill_prompt_line(session_ctx_t *ctx)
{
    const char *bg = ctx->system_bg_code != nullptr ? ctx->system_bg_code : "";
    const size_t bg_len = strlen(bg);
    unsigned int width = ctx->terminal_width > 0U ? ctx->terminal_width : 80U;
    if (width > SSH_CHATTER_MESSAGE_LIMIT) {
        width = SSH_CHATTER_MESSAGE_LIMIT;
    }

    static const char column_reset[] = "\033[1G";
    session_channel_write(ctx, column_reset, sizeof(column_reset) - 1U);
    if (bg_len > 0U) {
        session_channel_write(ctx, bg, bg_len);
    }

    if (width > 0U) {
        char spaces[64];
        memset(spaces, ' ', sizeof(spaces));
        unsigned int remaining = width;
        while (remaining > 0U) {
            size_t chunk =
                remaining < sizeof(spaces) ? remaining : sizeof(spaces);
            session_channel_write(ctx, spaces, chunk);
            remaining -= (unsigned int)chunk;
        }
    }

    session_channel_write(ctx, column_reset, sizeof(column_reset) - 1U);
    if (bg_len > 0U) {
        session_channel_write(ctx, bg, bg_len);
    }
}

static void session_render_prompt_internal(session_ctx_t *ctx,
                                           bool include_separator,
                                           bool fill_line)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    if (include_separator) {
        session_render_separator(ctx, "Input");
    }

    if (fill_line) {
        session_fill_prompt_line(ctx);
    }

    const char *fg = ctx->system_fg_code != nullptr ? ctx->system_fg_code : "";
    const char *bold = ctx->system_is_bold ? ANSI_BOLD : "";
    const char *bg = ctx->system_bg_code != nullptr ? ctx->system_bg_code : "";
    const char *mode_prompt = "> ";

    char prompt[128];
    size_t offset = 0U;
    offset = session_append_fragment(prompt, sizeof(prompt), offset,
                                     "\033[38;5;117m");
    offset = session_append_fragment(prompt, sizeof(prompt), offset, bold);
    offset =
        session_append_fragment(prompt, sizeof(prompt), offset, mode_prompt);
    offset =
        session_append_fragment(prompt, sizeof(prompt), offset, ANSI_RESET);
    if (bg[0] != '\0') {
        offset = session_append_fragment(prompt, sizeof(prompt), offset, bg);
    }
    if (fg[0] != '\0') {
        offset = session_append_fragment(prompt, sizeof(prompt), offset, fg);
    }
    if (bold[0] != '\0') {
        offset = session_append_fragment(prompt, sizeof(prompt), offset, bold);
    }

    session_channel_write(ctx, prompt, offset);
    if (ctx->input_length > 0U) {
        session_channel_write(ctx, ctx->input_buffer, ctx->input_length);
    }
}

static void session_render_prompt(session_ctx_t *ctx, bool include_separator)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    bool locked = session_output_lock(ctx);
    session_render_prompt_internal(ctx, include_separator, true);
    if (locked) {
        session_output_unlock(ctx);
    }
}

static void session_refresh_input_line(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    bool locked = session_output_lock(ctx);
    session_render_prompt_internal(ctx, false, true);
    fflush(stdout);

    if (locked) {
        session_output_unlock(ctx);
    }
}

static void session_set_input_text(session_ctx_t *ctx, const char *text)
{
    if (ctx == nullptr) {
        return;
    }

    ctx->input_length = 0U;
    memset(ctx->input_buffer, 0, sizeof(ctx->input_buffer));

    if (text != nullptr && text[0] != '\0') {
        const size_t len = strnlen(text, sizeof(ctx->input_buffer) - 1U);
        memcpy(ctx->input_buffer, text, len);
        ctx->input_buffer[len] = '\0';
        ctx->input_length = len;
    }

    session_refresh_input_line(ctx);
}

static void session_local_echo_char(session_ctx_t *ctx, char ch)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    if (ch == '\r' || ch == '\n') {
        session_channel_write(ctx, "\r\n", 2U);
        return;
    }

    session_channel_write(ctx, &ch, 1U);
}

static size_t session_utf8_prev_char_len(const char *buffer, size_t length)
{
    if (buffer == nullptr || length == 0U) {
        return 0U;
    }

    size_t idx = length;
    while (idx > 0U) {
        --idx;
        const unsigned char byte = (unsigned char)buffer[idx];
        if ((byte & 0xC0U) != 0x80U) {
            const size_t seq_len = length - idx;
            size_t expected = 1U;
            if ((byte & 0x80U) == 0U) {
                expected = 1U;
            } else if ((byte & 0xE0U) == 0xC0U) {
                expected = 2U;
            } else if ((byte & 0xF0U) == 0xE0U) {
                expected = 3U;
            } else if ((byte & 0xF8U) == 0xF0U) {
                expected = 4U;
            } else {
                expected = 1U;
            }

            if (seq_len < expected) {
                return seq_len;
            }
            return expected;
        }
    }

    return 1U;
}

static int session_utf8_char_width(const char *bytes, size_t length)
{
    if (bytes == nullptr || length == 0U) {
        return 0;
    }

    mbstate_t state;
    memset(&state, 0, sizeof(state));

    wchar_t wc;
    const size_t result = mbrtowc(&wc, bytes, length, &state);
    if (result == (size_t)-1 || result == (size_t)-2) {
        return 1;
    }

    const int width = wcwidth(wc);
    if (width < 0) {
        return 1;
    }

    return width;
}

static void session_local_backspace(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx) ||
        ctx->input_length == 0U) {
        return;
    }

    const size_t char_len =
        session_utf8_prev_char_len(ctx->input_buffer, ctx->input_length);
    if (char_len == 0U || char_len > ctx->input_length) {
        return;
    }

    const size_t char_start = ctx->input_length - char_len;
    const int display_width =
        session_utf8_char_width(&ctx->input_buffer[char_start], char_len);

    ctx->input_length = char_start;
    ctx->input_buffer[ctx->input_length] = '\0';

    const int width = display_width > 0 ? display_width : 1;
    const char sequence[] = "\b \b";
    for (int idx = 0; idx < width; ++idx) {
        session_channel_write(ctx, sequence, sizeof(sequence) - 1U);
    }
}

static void session_clear_input_base(session_ctx_t *ctx, bool render_prompt)
{
    if (ctx == nullptr) {
        return;
    }

    ctx->input_length = 0U;
    memset(ctx->input_buffer, 0, sizeof(ctx->input_buffer));
    ctx->input_history_position = -1;
    ctx->input_escape_active = false;
    ctx->input_escape_length = 0U;

    if (ctx->bracket_paste_active) {
        return;
    }

    if (render_prompt) {
        session_refresh_input_line(ctx);
        return;
    }

    if (!session_transport_active(ctx)) {
        return;
    }

    bool locked = session_output_lock(ctx);
    const char *bg = ctx->system_bg_code != nullptr ? ctx->system_bg_code : "";
    if (bg[0] != '\0') {
        session_channel_write(ctx, bg, strlen(bg));
    }

    static const char clear_sequence[] = "\r" ANSI_CLEAR_LINE;
    session_channel_write(ctx, clear_sequence, sizeof(clear_sequence) - 1U);

    if (bg[0] != '\0') {
        session_channel_write(ctx, bg, strlen(bg));
    }

    if (locked) {
        session_output_unlock(ctx);
    }
}

static void session_clear_input(session_ctx_t *ctx)
{
    session_clear_input_base(ctx, true);
}

static void session_clear_input_without_prompt(session_ctx_t *ctx)
{
    session_clear_input_base(ctx, false);
}

// SLASH_COMPATIBLE: Helper function to check if a character is slash-compatible
static inline bool session_is_slash_compatible_char(char ch)
{
    switch ((int)ch) {
    case (int)'/':
    case (int)'.':
    case (int)'_':
    case (int)'@':
    case (int)'$':
    case (int)'*':
    case (int)'-':
    case (int)'#':
    case (int)'>':
        return true;
    default:
        return false;
    }
}

static bool session_try_command_completion(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }

    if (ctx->input_length == 0U) {
        return false;
    }

    size_t first_visible = 0U;
    while (first_visible < ctx->input_length &&
           isspace((unsigned char)ctx->input_buffer[first_visible])) {
        ++first_visible;
    }
    if (first_visible >= ctx->input_length) {
        return false;
    }

    // SLASH_COMPATIBLE: Check for slash or slash-compatible characters
    const bool has_slash_compatible =
        session_is_slash_compatible_char(ctx->input_buffer[first_visible]);
    if (!has_slash_compatible && ctx->input_mode != SESSION_INPUT_MODE_COMMAND) {
        return false;
    }

    size_t command_start = first_visible + (has_slash_compatible ? 1U : 0U);
    if (command_start > ctx->input_length) {
        command_start = ctx->input_length;
    }

    size_t command_end = command_start;
    while (command_end < ctx->input_length &&
           !isspace((unsigned char)ctx->input_buffer[command_end])) {
        ++command_end;
    }

    const size_t token_len = command_end - command_start;
    char prefix[SSH_CHATTER_MAX_INPUT_LEN];
    size_t copy_len =
        token_len < sizeof(prefix) - 1U ? token_len : sizeof(prefix) - 1U;
    if (copy_len > 0U) {
        memcpy(prefix, &ctx->input_buffer[command_start], copy_len);
    }
    prefix[copy_len] = '\0';

    const size_t prefix_len = strlen(prefix);
    const char *matches[SSH_CHATTER_COMMAND_COUNT];
    size_t match_count = 0U;
    for (size_t idx = 0U; idx < SSH_CHATTER_COMMAND_COUNT; ++idx) {
        const char *candidate = kSessionCommandNames[idx];
        if (prefix_len == 0U ||
            strncasecmp(candidate, prefix, prefix_len) == 0) {
            matches[match_count++] = candidate;
        }
    }

    session_command_collect_localized_matches(
        ctx, prefix, matches, &match_count,
        sizeof(matches) / sizeof(matches[0]));

    if (match_count == 0U) {
        if (session_transport_active(ctx)) {
            const char bell = '\a';
            session_channel_write(ctx, &bell, 1U);
        }
        session_refresh_input_line(ctx);
        return true;
    }

    char updated[SSH_CHATTER_MAX_INPUT_LEN];
    size_t updated_len = 0U;
    const size_t prefix_copy_len =
        command_start < sizeof(updated) ? command_start : sizeof(updated) - 1U;
    if (prefix_copy_len > 0U) {
        memcpy(updated, ctx->input_buffer, prefix_copy_len);
        updated_len = prefix_copy_len;
    }

    if (match_count == 1U) {
        const char *completion = matches[0];
        size_t completion_len = strlen(completion);
        if (updated_len + completion_len >= sizeof(updated)) {
            completion_len = sizeof(updated) - 1U - updated_len;
        }
        memcpy(&updated[updated_len], completion, completion_len);
        updated_len += completion_len;

        size_t suffix_len = ctx->input_length - command_end;
        if (suffix_len > 0U) {
            size_t copy_suffix = suffix_len;
            if (updated_len + copy_suffix >= sizeof(updated)) {
                copy_suffix = sizeof(updated) - 1U - updated_len;
            }
            memcpy(&updated[updated_len], &ctx->input_buffer[command_end],
                   copy_suffix);
            updated_len += copy_suffix;
        } else if (updated_len + 1U < sizeof(updated)) {
            updated[updated_len++] = ' ';
        }

        updated[updated_len] = '\0';
        session_set_input_text(ctx, updated);
        ctx->input_history_position = -1;
        session_scrollback_reset_position(ctx);
        return true;
    }

    size_t common_len = strlen(matches[0]);
    for (size_t idx = 1U; idx < match_count && common_len > 0U; ++idx) {
        const char *candidate = matches[idx];
        size_t candidate_len = strlen(candidate);
        if (candidate_len < common_len) {
            common_len = candidate_len;
        }
        size_t compare_len = common_len;
        size_t match_prefix = 0U;
        for (; match_prefix < compare_len; ++match_prefix) {
            unsigned char lhs =
                (unsigned char)tolower((unsigned char)matches[0][match_prefix]);
            unsigned char rhs =
                (unsigned char)tolower((unsigned char)candidate[match_prefix]);
            if (lhs != rhs) {
                break;
            }
        }
        common_len = match_prefix;
    }

    if (common_len > prefix_len) {
        size_t completion_len = common_len;
        if (updated_len + completion_len >= sizeof(updated)) {
            completion_len = sizeof(updated) - 1U - updated_len;
        }
        memcpy(&updated[updated_len], matches[0], completion_len);
        updated_len += completion_len;

        size_t suffix_len = ctx->input_length - command_end;
        if (suffix_len > 0U) {
            size_t copy_suffix = suffix_len;
            if (updated_len + copy_suffix >= sizeof(updated)) {
                copy_suffix = sizeof(updated) - 1U - updated_len;
            }
            memcpy(&updated[updated_len], &ctx->input_buffer[command_end],
                   copy_suffix);
            updated_len += copy_suffix;
        }

        updated[updated_len] = '\0';
        session_set_input_text(ctx, updated);
        ctx->input_history_position = -1;
        session_scrollback_reset_position(ctx);
        return true;
    }

    session_send_system_line(ctx, "Possible commands:");
    char line[SSH_CHATTER_MESSAGE_LIMIT];
    size_t offset = 0U;
    for (size_t idx = 0U; idx < match_count; ++idx) {
        char entry[64];
        snprintf(entry, sizeof(entry), "/%s", matches[idx]);
        size_t entry_len = strlen(entry);
        if (offset != 0U) {
            if (offset + 1U >= sizeof(line)) {
                line[offset] = '\0';
                session_send_system_line(ctx, line);
                offset = 0U;
            }
            line[offset++] = ' ';
        }
        if (entry_len >= sizeof(line)) {
            session_send_system_line(ctx, entry);
            offset = 0U;
            continue;
        }
        if (offset + entry_len >= sizeof(line)) {
            line[offset] = '\0';
            session_send_system_line(ctx, line);
            offset = 0U;
        }
        memcpy(&line[offset], entry, entry_len);
        offset += entry_len;
    }
    if (offset > 0U) {
        line[offset] = '\0';
        session_send_system_line(ctx, line);
    }
    session_refresh_input_line(ctx);
    return true;
}

void session_scrollback_reset_position(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    ctx->history_scroll_position = 0U;
    ctx->history_latest_notified = false;
    ctx->history_oldest_notified = false;
}

static void session_history_record(session_ctx_t *ctx, const char *line)
{
    if (ctx == nullptr || line == nullptr) {
        return;
    }

    bool has_visible = false;
    for (const char *cursor = line; *cursor != '\0'; ++cursor) {
        if (!isspace((unsigned char)*cursor)) {
            has_visible = true;
            break;
        }
    }

    if (!has_visible) {
        ctx->input_history_position = -1;
        return;
    }

    const char *trimmed = line;
    while (*trimmed == ' ' || *trimmed == '\t') {
        ++trimmed;
    }

    bool is_command = false;
    if (*trimmed != '\0') {
        if (*trimmed == '/') {
            is_command = true;
        } else if (ctx->input_mode == SESSION_INPUT_MODE_COMMAND) {
            is_command = true;
        }
    }

    if (ctx->input_history_count > 0U) {
        const size_t last_index = ctx->input_history_count - 1U;
        if (strncmp(ctx->input_history[last_index], line,
                    sizeof(ctx->input_history[last_index])) == 0) {
            ctx->input_history_position = -1;
            return;
        }
    }

    if (ctx->input_history_count < SSH_CHATTER_INPUT_HISTORY_LIMIT) {
        snprintf(ctx->input_history[ctx->input_history_count],
                 sizeof(ctx->input_history[0]), "%s", line);
        ctx->input_history_is_command[ctx->input_history_count] = is_command;
        ++ctx->input_history_count;
    } else {
        memmove(ctx->input_history, ctx->input_history + 1,
                sizeof(ctx->input_history) - sizeof(ctx->input_history[0]));
        memmove(ctx->input_history_is_command,
                ctx->input_history_is_command + 1,
                (SSH_CHATTER_INPUT_HISTORY_LIMIT - 1U) *
                    sizeof(ctx->input_history_is_command[0]));
        snprintf(ctx->input_history[SSH_CHATTER_INPUT_HISTORY_LIMIT - 1U],
                 sizeof(ctx->input_history[0]), "%s", line);
        ctx->input_history_is_command[SSH_CHATTER_INPUT_HISTORY_LIMIT - 1U] =
            is_command;
    }

    ctx->input_history_position = -1;
    session_scrollback_reset_position(ctx);
}

static void session_history_navigate(session_ctx_t *ctx, int direction)
{
    if (ctx == nullptr || direction == 0) {
        return;
    }

    // If scrollback is active (scrolled back), clear it before navigating command history
    // This prevents blank lines from appearing when switching from scrollback to command history
    bool was_scrolled_back = (ctx->history_scroll_position > 0U);
    
    session_scrollback_reset_position(ctx);

    // Clear the current line to remove any scrollback content
    if (was_scrolled_back) {
        const char clear_sequence[] = "\r" ANSI_CLEAR_LINE;
        session_channel_write(ctx, clear_sequence, sizeof(clear_sequence) - 1U);
    }

    if (ctx->input_history_count == 0U) {
        ctx->input_history_position = (int)ctx->input_history_count;
        session_set_input_text(ctx, "");
        return;
    }

    int position = ctx->input_history_position;
    if (position < 0 || position > (int)ctx->input_history_count) {
        position = (int)ctx->input_history_count;
    }

    position += direction;
    if (position < 0) {
        position = 0;
    }
    if (position > (int)ctx->input_history_count) {
        position = (int)ctx->input_history_count;
    }

    ctx->input_history_position = position;

    if (position == (int)ctx->input_history_count) {
        session_set_input_text(ctx, "");
    } else {
        session_set_input_text(ctx, ctx->input_history[position]);
    }
}

void session_scrollback_navigate(session_ctx_t *ctx, int direction)
{
    if (ctx == nullptr || ctx->owner == nullptr || !session_transport_active(ctx) ||
        direction == 0) {
        return;
    }

    size_t total = host_history_total(ctx->owner);
    if (total == 0U) {
        session_send_system_line(ctx, "No chat history available yet.");
        return;
    }

    bool suppress_translation = translator_should_skip_scrollback_translation();
    bool previous_translation_suppress = ctx->translation_suppress_output;
    if (suppress_translation) {
        ctx->translation_suppress_output = true;
    }

    const size_t step =
        SSH_CHATTER_SCROLLBACK_CHUNK > 0 ? SSH_CHATTER_SCROLLBACK_CHUNK : 1U;
    if (ctx->history_scroll_position >= total) {
        ctx->history_scroll_position = total > 0U ? total - 1U : 0U;
    }
    size_t position = ctx->history_scroll_position;
    size_t new_position = position;
    bool reached_oldest = false;

    const size_t max_position = total > 0U ? total - 1U : 0U;

    if (direction > 0) {
        size_t current_newest_visible = 0U;
        if (position < total) {
            current_newest_visible = total - 1U - position;
        }

        size_t current_chunk = step;
        if (current_chunk > current_newest_visible + 1U) {
            current_chunk = current_newest_visible + 1U;
        }
        if (current_chunk == 0U) {
            current_chunk = 1U;
        }

        const size_t current_oldest_visible =
            (current_newest_visible + 1U > current_chunk)
                ? (current_newest_visible + 1U - current_chunk)
                : 0U;

        if (current_oldest_visible == 0U) {
            reached_oldest = true;
        } else if (new_position < max_position) {
            size_t advance = step;
            if (advance > max_position - new_position) {
                advance = max_position - new_position;
            }
            if (advance == 0U) {
                reached_oldest = true;
            } else {
                new_position += advance;
            }
        } else {
            reached_oldest = true;
        }
    } else if (direction < 0) {
        if (new_position > 0U) {
            size_t retreat = step;
            if (retreat > new_position) {
                retreat = new_position;
            }
            new_position -= retreat;
        }
    }

    bool at_boundary = (new_position == position);
    ctx->history_scroll_position = new_position;

    bool at_latest = (ctx->history_scroll_position == 0U);
    bool at_oldest =
        (ctx->history_scroll_position == max_position && total > 0U);

    if (!at_latest) {
        ctx->history_latest_notified = false;
    }
    if (!at_oldest) {
        ctx->history_oldest_notified = false;
    }

    const char clear_sequence[] = "\r" ANSI_CLEAR_LINE;
    session_channel_write(ctx, clear_sequence, sizeof(clear_sequence) - 1U);

    if (direction < 0 && at_boundary && new_position == 0U) {
        if (!ctx->history_latest_notified) {
            ctx->history_latest_notified = true;
        }
        session_render_prompt(ctx, false);
        goto cleanup;
    }

    const size_t newest_visible = total - 1U - new_position;
    size_t chunk = step;
    if (chunk > newest_visible + 1U) {
        chunk = newest_visible + 1U;
    }
    if (chunk == 0U) {
        chunk = 1U;
    }

    const size_t oldest_visible =
        (newest_visible + 1U > chunk) ? (newest_visible + 1U - chunk) : 0U;

    if (direction > 0 &&
        (reached_oldest || (at_boundary && new_position == max_position))) {
        if (!ctx->history_oldest_notified) {
            ctx->history_oldest_notified = true;
        }
    }

    char header[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(header, sizeof(header), "Scrollback (%zu-%zu of %zu)",
             oldest_visible + 1U, newest_visible + 1U, total);
    session_send_system_line(ctx, header);

    chat_history_entry_t buffer[SSH_CHATTER_SCROLLBACK_CHUNK];
    size_t request = chunk;
    if (request > SSH_CHATTER_SCROLLBACK_CHUNK) {
        request = SSH_CHATTER_SCROLLBACK_CHUNK;
    }
    size_t copied =
        host_history_copy_range(ctx->owner, oldest_visible, buffer, request);
    if (copied == 0U) {
        session_send_system_line(ctx, "Unable to read chat history right now.");
        ctx->history_scroll_position = (total > 0U) ? max_position : 0U;
        ctx->history_latest_notified = false;
        ctx->history_oldest_notified = false;
        session_render_prompt(ctx, false);
        goto cleanup;
    }

    for (size_t idx = 0; idx < copied; ++idx) {
        session_send_history_entry(ctx, &buffer[idx]);
    }

    if (direction < 0 && new_position == 0U) {
        if (!ctx->history_latest_notified) {
            session_send_system_line(ctx, "End of scrollback.");
            ctx->history_latest_notified = true;
        }
    }

    session_render_prompt(ctx, false);

cleanup:
    if (suppress_translation) {
        ctx->translation_suppress_output = previous_translation_suppress;
    }
}

static void session_scrollback_navigate_line(session_ctx_t *ctx, int direction)
{
    if (ctx == nullptr || ctx->owner == nullptr || !session_transport_active(ctx) ||
        direction == 0) {
        return;
    }

    size_t total = host_history_total(ctx->owner);
    if (total == 0U) {
        return;
    }

    bool suppress_translation = translator_should_skip_scrollback_translation();
    bool previous_translation_suppress = ctx->translation_suppress_output;
    if (suppress_translation) {
        ctx->translation_suppress_output = true;
    }

    if (ctx->history_scroll_position >= total) {
        ctx->history_scroll_position = total > 0U ? total - 1U : 0U;
    }
    size_t position = ctx->history_scroll_position;
    size_t new_position = position;

    const size_t max_position = total > 0U ? total - 1U : 0U;

    // Scroll by exactly 1 line
    if (direction > 0) {
        // Scroll toward older messages
        if (new_position < max_position) {
            new_position += 1U;
        }
    } else if (direction < 0) {
        // Scroll toward newer messages
        if (new_position > 0U) {
            new_position -= 1U;
        }
    }

    bool at_boundary = (new_position == position);
    ctx->history_scroll_position = new_position;

    bool at_latest = (ctx->history_scroll_position == 0U);

    if (!at_latest) {
        ctx->history_latest_notified = false;
    }

    const char clear_sequence[] = "\r" ANSI_CLEAR_LINE;
    session_channel_write(ctx, clear_sequence, sizeof(clear_sequence) - 1U);

    if (direction < 0 && at_boundary && new_position == 0U) {
        session_render_prompt(ctx, false);
        goto cleanup;
    }

    // Calculate sliding window - always show 100 messages (MESSAGE CHUNK)
    size_t visible_lines = SSH_CHATTER_SCROLLBACK_CHUNK; // Always show 100 messages
    size_t newest_visible = total - 1U - new_position;
    size_t chunk = visible_lines;
    if (chunk > newest_visible + 1U) {
        chunk = newest_visible + 1U;
    }
    if (chunk == 0U) {
        chunk = 1U;
    }

    const size_t oldest_visible =
        (newest_visible + 1U > chunk) ? (newest_visible + 1U - chunk) : 0U;

    chat_history_entry_t buffer[SSH_CHATTER_SCROLLBACK_CHUNK];
    size_t request = chunk;
    if (request > SSH_CHATTER_SCROLLBACK_CHUNK) {
        request = SSH_CHATTER_SCROLLBACK_CHUNK;
    }
    size_t copied =
        host_history_copy_range(ctx->owner, oldest_visible, buffer, request);
    if (copied == 0U) {
        ctx->history_scroll_position = (total > 0U) ? max_position : 0U;
        ctx->history_latest_notified = false;
        session_render_prompt(ctx, false);
        goto cleanup;
    }

    // Clear screen before displaying messages (as per requirement)
    session_clear_screen(ctx);

    // Show header indicating the message range being displayed
    char header[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(header, sizeof(header), "Scrollback (%zu-%zu of %zu)",
             oldest_visible + 1U, newest_visible + 1U, total);
    session_send_system_line(ctx, header);

    for (size_t idx = 0; idx < copied; ++idx) {
        session_send_history_entry(ctx, &buffer[idx]);
    }

    session_render_prompt(ctx, false);

cleanup:
    if (suppress_translation) {
        ctx->translation_suppress_output = previous_translation_suppress;
    }
}

static bool session_consume_escape_sequence(session_ctx_t *ctx, char ch)
{
    if (ctx == nullptr) {
        return false;
    }

    if (!ctx->input_escape_active) {
        if (ch == 0x1b) {
            ctx->input_escape_active = true;
            ctx->input_escape_length = 0U;
            if (ctx->input_escape_length < sizeof(ctx->input_escape_buffer)) {
                ctx->input_escape_buffer[ctx->input_escape_length++] = ch;
            }
            return true;
        }
        return false;
    }

    if (ctx->input_escape_length < sizeof(ctx->input_escape_buffer)) {
        ctx->input_escape_buffer[ctx->input_escape_length++] = ch;
    }

    const char *sequence = ctx->input_escape_buffer;
    const size_t length = ctx->input_escape_length;

    if (length == 1U) {
        return true;
    }

    if (length == 2U) {
        if (sequence[1] == '[') {
            return true;
        }
        if (sequence[1] == 'k') {
            if (ctx->bbs_post_pending) {
                session_bbs_move_cursor(ctx, -1);
            } else {
                if (ctx->bbs_view_active && session_bbs_scroll(ctx, 1, 1U)) {
                    ctx->input_escape_active = false;
                    ctx->input_escape_length = 0U;
                    return true;
                }
                session_history_navigate(ctx, -1);
            }
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
        if (sequence[1] == 'j') {
            if (ctx->bbs_post_pending) {
                session_bbs_move_cursor(ctx, 1);
            } else {
                if (ctx->bbs_view_active && session_bbs_scroll(ctx, -1, 1U)) {
                    ctx->input_escape_active = false;
                    ctx->input_escape_length = 0U;
                    return true;
                }
                session_history_navigate(ctx, 1);
            }
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
        if ((sequence[1] == 'l' || sequence[1] == 'L') && ctx->game.active &&
            ctx->game.type == SESSION_GAME_ALPHA) {
            session_game_alpha_manual_lock(ctx);
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
    }

    if (length == 3U && sequence[1] == '[') {
        int dx = 0;
        int dy = 0;
        switch (sequence[2]) {
        case 'A':
            dy = -1;
            break;
        case 'B':
            dy = 1;
            break;
        case 'C':
            dx = 1;
            break;
        case 'D':
            dx = -1;
            break;
        default:
            break;
        }
        if ((dx != 0 || dy != 0) &&
            session_game_alpha_handle_arrow(ctx, dx, dy)) {
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
        if (sequence[2] == 'A') {
            if (ctx->bbs_post_pending) {
                session_bbs_move_cursor(ctx, -1);
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->bbs_view_active && session_bbs_scroll(ctx, 1, 1U)) {
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->in_rss_mode && session_rss_move(ctx, -1)) {
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->input_mode == SESSION_INPUT_MODE_COMMAND) {
                session_history_navigate(ctx, -1);
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            session_scrollback_navigate_line(ctx, 1);
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
        if (sequence[2] == 'B') {
            if (ctx->bbs_post_pending) {
                session_bbs_move_cursor(ctx, 1);
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->bbs_view_active && session_bbs_scroll(ctx, -1, 1U)) {
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->in_rss_mode && session_rss_move(ctx, 1)) {
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->input_mode == SESSION_INPUT_MODE_COMMAND) {
                session_history_navigate(ctx, 1);
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            session_scrollback_navigate_line(ctx, -1);
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
    }

    if (length == 3U && sequence[1] == 'O') {
        int dx = 0;
        int dy = 0;
        switch (sequence[2]) {
        case 'A':
            dy = -1;
            break;
        case 'B':
            dy = 1;
            break;
        case 'C':
            dx = 1;
            break;
        case 'D':
            dx = -1;
            break;
        default:
            break;
        }
        if ((dx != 0 || dy != 0) &&
            session_game_alpha_handle_arrow(ctx, dx, dy)) {
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
        if (sequence[2] == 'A') {
            if (ctx->bbs_post_pending) {
                session_bbs_move_cursor(ctx, -1);
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->bbs_view_active && session_bbs_scroll(ctx, 1, 1U)) {
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->in_rss_mode && session_rss_move(ctx, -1)) {
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->input_mode == SESSION_INPUT_MODE_COMMAND) {
                session_history_navigate(ctx, -1);
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            session_scrollback_navigate_line(ctx, 1);
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
        if (sequence[2] == 'B') {
            if (ctx->bbs_post_pending) {
                session_bbs_move_cursor(ctx, 1);
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->bbs_view_active && session_bbs_scroll(ctx, -1, 1U)) {
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->in_rss_mode && session_rss_move(ctx, 1)) {
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            if (ctx->input_mode == SESSION_INPUT_MODE_COMMAND) {
                session_history_navigate(ctx, 1);
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            session_scrollback_navigate_line(ctx, -1);
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
    }

    if (length == 4U && sequence[1] == '[' && sequence[3] == '~') {
        if (sequence[2] == '5') {
            if (ctx->bbs_view_active && session_bbs_scroll(ctx, 1, 0U)) {
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            session_scrollback_navigate(ctx, 1);
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
        if (sequence[2] == '6') {
            if (ctx->bbs_view_active && session_bbs_scroll(ctx, -1, 0U)) {
                ctx->input_escape_active = false;
                ctx->input_escape_length = 0U;
                return true;
            }
            session_scrollback_navigate(ctx, -1);
            ctx->input_escape_active = false;
            ctx->input_escape_length = 0U;
            return true;
        }
    }

    const bool bracket_sequence = (length >= 2U && sequence[1] == '[');
    if (bracket_sequence) {
        const char final = sequence[length - 1U];
        if (final != '~' &&
            !(length == 3U && isalpha((unsigned char)sequence[2]))) {
            return true;
        }
        if (final == '~') {
            if (length >= 5U && strncmp(&sequence[2], "200", 3) == 0) {
                ctx->bracket_paste_active = true;
            } else if (length >= 5U && strncmp(&sequence[2], "201", 3) == 0) {
                ctx->bracket_paste_active = false;
                session_refresh_input_line(ctx);
            }
        }
    }

    ctx->input_escape_active = false;
    ctx->input_escape_length = 0U;
    if (bracket_sequence) {
        return true;
    }
    return ch == 0x1b;
}

static void session_send_private_message_line(session_ctx_t *ctx,
                                              const session_ctx_t *color_source,
                                              const char *label,
                                              const char *message)
{
    if (ctx == nullptr || !session_transport_active(ctx) || color_source == nullptr ||
        label == nullptr || message == nullptr) {
        return;
    }

    const char *highlight = color_source->user_highlight_code != nullptr
                                ? color_source->user_highlight_code
                                : "";
    const char *color = color_source->user_color_code != nullptr
                            ? color_source->user_color_code
                            : "";
    const char *bold = color_source->user_is_bold ? ANSI_BOLD : "";

    char line[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(line, sizeof(line), "%s%s%s[%s]%s %s", highlight, bold, color,
             label, ANSI_RESET, message);
    session_send_line(ctx, line);

    if (ctx != color_source && ctx->history_scroll_position == 0U) {
        session_refresh_input_line(ctx);
    }
}

// Helper function to send a message line-by-line with cursor reset for each line
static void session_send_multiline_message(session_ctx_t *ctx,
                                           const char *message)
{
    if (ctx == nullptr || message == nullptr || !session_transport_active(ctx)) {
        return;
    }

    // Make a copy of the message since we'll be modifying it
    size_t message_len = strlen(message);
    if (message_len == 0U) {
        return;
    }

    char *message_copy = GC_CALLOC(1U, message_len + 1U);
    if (message_copy == nullptr) {
        // If allocation fails, split inline without copying
        // This is a fallback path that still preserves the line-by-line behavior
        const char *line_start = message;
        const char *newline_pos = nullptr;

        while ((newline_pos = strchr(line_start, '\n')) != nullptr) {
            // Calculate line length
            size_t line_len = (size_t)(newline_pos - line_start);

            // Create a temporary buffer for this line (reserve 1 byte for null terminator)
            char line_buffer[SSH_CHATTER_MESSAGE_LIMIT];
            const size_t max_line_len = sizeof(line_buffer) - 1U;
            if (line_len > max_line_len) {
                line_len = max_line_len;
            }

            memcpy(line_buffer, line_start, line_len);
            line_buffer[line_len] = '\0';

            session_send_plain_line(ctx, line_buffer);

            // Move to the next line
            line_start = newline_pos + 1;
        }

        // Send any remaining text after the last newline
        if (line_start[0] != '\0') {
            session_send_plain_line(ctx, line_start);
        }
        return;
    }

    memcpy(message_copy, message, message_len);

    // Split by newlines and send each line individually
    char *line_start = message_copy;
    char *newline_pos = nullptr;

    while ((newline_pos = strchr(line_start, '\n')) != nullptr) {
        // Temporarily null-terminate at the newline
        *newline_pos = '\0';

        // Send this line with cursor reset
        session_send_plain_line(ctx, line_start);

        // Move to the next line
        line_start = newline_pos + 1;
    }

    // Send any remaining text after the last newline
    if (line_start[0] != '\0') {
        session_send_plain_line(ctx, line_start);
    }
}

static void session_send_history_entry(session_ctx_t *ctx,
                                       const chat_history_entry_t *entry)
{
    if (ctx == nullptr || !session_transport_active(ctx) || entry == nullptr) {
        return;
    }

    if (session_should_hide_entry(ctx, entry)) {
        return;
    }

    if (entry->is_user_message) {
        char formatted[SSH_CHATTER_MESSAGE_LIMIT * 2U];
        formatted[0] = '\0';

        const char *color =
            (entry->user_color_code != nullptr) ? entry->user_color_code : "";
        const char *bold = entry->user_is_bold ? ANSI_BOLD : "";

        char name_block[SSH_CHATTER_MESSAGE_LIMIT];
        const char *id_display = "-";
        char id_label[32];
        if (entry->message_id > 0U &&
            host_compact_id_encode(entry->message_id, id_label,
                                   sizeof(id_label))) {
            id_display = id_label;
        }
        snprintf(name_block, sizeof(name_block), "%s%s [%s] <%s>%s", color, bold,
                 id_display, entry->username, ANSI_RESET);
        strncat(formatted, name_block,
                sizeof(formatted) - strlen(formatted) - 1U);

        if (entry->message[0] != '\0') {
            const bool multiline = strchr(entry->message, '\n') != nullptr;
            if (multiline) {
                // For multiline messages, send the username first, then each line separately
                strncat(formatted, " ",
                        sizeof(formatted) - strlen(formatted) - 1U);
                session_send_plain_line(ctx, formatted);
                session_send_multiline_message(ctx, entry->message);
            } else {
                // For single-line messages, send as before
                strncat(formatted, " ",
                        sizeof(formatted) - strlen(formatted) - 1U);
                strncat(formatted, entry->message,
                        sizeof(formatted) - strlen(formatted) - 1U);
                session_send_plain_line(ctx, formatted);
            }
        } else {
            session_send_plain_line(ctx, formatted);
        }

        // Display attachment URL if present, similar to reply format
        if (entry->attachment_type != CHAT_ATTACHMENT_NONE &&
            entry->attachment_target[0] != '\0') {
            const char *label =
                chat_attachment_type_label(entry->attachment_type);
            char attachment_line[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(attachment_line, sizeof(attachment_line), "    (%s)" ANSI_RESET " %s",
                     label, entry->attachment_target);
            session_send_plain_line(ctx, attachment_line);

            if (entry->attachment_caption[0] != '\0') {
                char caption_line[SSH_CHATTER_MESSAGE_LIMIT];
                snprintf(caption_line, sizeof(caption_line), "    \342\206\263 %s",
                         entry->attachment_caption);
                session_send_plain_line(ctx, caption_line);
            }
        }

        return;
    }

    // For non-user messages, check if multiline and send accordingly
    const bool multiline = strchr(entry->message, '\n') != nullptr;
    if (multiline) {
        session_send_multiline_message(ctx, entry->message);
    } else {
        session_send_plain_line(ctx, entry->message);
    }
}

// Present a summary of a poll, optionally showing the label used for named polls.
static void session_send_poll_summary_generic(session_ctx_t *ctx,
                                              const poll_state_t *poll,
                                              const char *label)
{
    if (ctx == nullptr || poll == nullptr) {
        return;
    }

    if (!poll->active || poll->option_count == 0U) {
        if (label == nullptr) {
            session_send_system_line(ctx, "No active poll right now.");
        } else {
            char message[128];
            snprintf(message, sizeof(message), "Poll '%s' is not active.",
                     label);
            session_send_system_line(ctx, message);
        }
        return;
    }

    char header[SSH_CHATTER_MESSAGE_LIMIT];
    const char *mode_suffix = poll->allow_multiple ? " (multiple choice)" : "";
    if (label == nullptr) {
        snprintf(header, sizeof(header), "Poll #%" PRIu64 ": %s%s", poll->id,
                 poll->question, mode_suffix);
    } else {
        snprintf(header, sizeof(header), "Poll [%s] #%" PRIu64 ": %s%s", label,
                 poll->id, poll->question, mode_suffix);
    }
    session_send_system_line(ctx, header);

    for (size_t idx = 0U; idx < poll->option_count; ++idx) {
        char option_line[SSH_CHATTER_MESSAGE_LIMIT];
        uint32_t votes = poll->options[idx].votes;
        if (label == nullptr) {
            snprintf(option_line, sizeof(option_line),
                     "  /%zu - %s (%u vote%s)", idx + 1U,
                     poll->options[idx].text, votes, votes == 1U ? "" : "s");
        } else {
            snprintf(option_line, sizeof(option_line),
                     "  /%zu %s - %s (%u vote%s)", idx + 1U, label,
                     poll->options[idx].text, votes, votes == 1U ? "" : "s");
        }
        session_send_system_line(ctx, option_line);
    }

    if (label == nullptr) {
        if (poll->allow_multiple) {
            session_send_system_line(
                ctx, "Vote with /1 through /5 (multiple selections allowed).");
        } else {
            session_send_system_line(ctx, "Vote with /1 through /5.");
        }
    } else {
        char footer[192];
        if (poll->allow_multiple) {
            snprintf(footer, sizeof(footer),
                     "Vote with /1 %s through /%zu %s (multiple selections "
                     "allowed).",
                     label, poll->option_count, label);
        } else {
            snprintf(footer, sizeof(footer), "Vote with /1 %s through /%zu %s.",
                     label, poll->option_count, label);
        }
        session_send_system_line(ctx, footer);
    }
}

// Gather the main poll and any named polls and present summaries to the caller.
static void session_send_poll_summary(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    host_t *host = ctx->owner;
    poll_state_t main_snapshot = {0};
    named_poll_state_t named_snapshot[SSH_CHATTER_MAX_NAMED_POLLS];
    size_t named_count = 0U;

    pthread_mutex_lock(&host->lock);
    main_snapshot = host->poll;
    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_NAMED_POLLS; ++idx) {
        if (host->named_polls[idx].label[0] == '\0') {
            continue;
        }
        named_snapshot[named_count++] = host->named_polls[idx];
        if (named_count >= SSH_CHATTER_MAX_NAMED_POLLS) {
            break;
        }
    }
    pthread_mutex_unlock(&host->lock);

    session_send_poll_summary_generic(ctx, &main_snapshot, nullptr);

    size_t active_named = 0U;
    for (size_t idx = 0U; idx < named_count; ++idx) {
        if (named_snapshot[idx].poll.active &&
            named_snapshot[idx].poll.option_count > 0U) {
            if (active_named == 0U) {
                session_send_system_line(ctx, "Active named polls:");
            }
            session_send_poll_summary_generic(ctx, &named_snapshot[idx].poll,
                                              named_snapshot[idx].label);
            ++active_named;
        }
    }

    if (active_named == 0U) {
        session_send_system_line(
            ctx, "No active named polls. Use /vote <label> "
                 "<question>|<option1>|<option2> or /vote-single for a "
                 "single-choice poll.");
    }
}

// Provide a lightweight overview of every named poll regardless of status.
static void session_list_named_polls(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    host_t *host = ctx->owner;
    named_poll_state_t snapshot[SSH_CHATTER_MAX_NAMED_POLLS];
    size_t count = 0U;

    pthread_mutex_lock(&host->lock);
    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_NAMED_POLLS; ++idx) {
        if (host->named_polls[idx].label[0] == '\0') {
            continue;
        }
        snapshot[count++] = host->named_polls[idx];
        if (count >= SSH_CHATTER_MAX_NAMED_POLLS) {
            break;
        }
    }
    pthread_mutex_unlock(&host->lock);

    if (count == 0U) {
        session_send_system_line(
            ctx, "No named polls exist. Start one with /vote <label> "
                 "<question>|<option1>|<option2> or /vote-single "
                 "for single-choice voting.");
        return;
    }

    session_send_system_line(ctx, "Named polls overview:");
    for (size_t idx = 0U; idx < count; ++idx) {
        const named_poll_state_t *entry = &snapshot[idx];
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        const char *status = entry->poll.active ? "active" : "inactive";
        const char *mode =
            entry->poll.allow_multiple ? "multiple choice" : "single choice";
        snprintf(line, sizeof(line), "- [%s] %s (options: %zu, %s, %s)",
                 entry->label, entry->poll.question, entry->poll.option_count,
                 status, mode);
        session_send_system_line(ctx, line);
    }
}

static bool
chat_history_entry_build_reaction_summary(const chat_history_entry_t *entry,
                                          char *buffer, size_t length)
{
    if (entry == nullptr || buffer == nullptr || length == 0U) {
        return false;
    }

    buffer[0] = '\0';
    bool any = false;
    size_t offset = 0U;

    for (size_t idx = 0U; idx < SSH_CHATTER_REACTION_KIND_COUNT; ++idx) {
        uint32_t count = entry->reaction_counts[idx];
        if (count == 0U) {
            continue;
        }

        const reaction_descriptor_t *descriptor = &REACTION_DEFINITIONS[idx];
        char chunk[64];
        snprintf(chunk, sizeof(chunk), "%s x%u", descriptor->icon, count);

        size_t chunk_len = strlen(chunk);
        if (chunk_len + 1U >= length - offset) {
            break;
        }

        if (any) {
            buffer[offset++] = ' ';
        }
        memcpy(buffer + offset, chunk, chunk_len);
        offset += chunk_len;
        buffer[offset] = '\0';
        any = true;
    }

    return any;
}

static const char *chat_attachment_type_label(chat_attachment_type_t type)
{
    switch (type) {
    case CHAT_ATTACHMENT_IMAGE:
        return "image";
    case CHAT_ATTACHMENT_VIDEO:
        return "video";
    case CHAT_ATTACHMENT_AUDIO:
        return "audio";
    case CHAT_ATTACHMENT_FILE:
        return "file";
    case CHAT_ATTACHMENT_NONE:
    default:
        return "attachment";
    }
}

static bool session_handle_service_request(ssh_message message)
{
    if (message == nullptr) {
        return false;
    }

    const char *service = ssh_message_service_service(message);
    if (service == nullptr) {
        return false;
    }

    if (strcmp(service, "ssh-userauth") == 0 ||
        strcmp(service, "ssh-connection") == 0) {
        ssh_message_service_reply_success(message);
        return true;
    }

    return false;
}

bool is_nullarray(uint8_t *arr, size_t len)
{
    uint8_t zeros[len];
    memset(zeros, 0, len);
    return memcmp(arr, zeros, len) == 0;
}

static int session_authenticate(session_ctx_t *ctx)
{
    ssh_message message = nullptr;
    bool authenticated = false;
    if (ctx != nullptr) {
        ctx->lan_operator_credentials_valid = false;
    }

    // Declare credential here to ensure it's in scope for all uses
    lan_operator_credential_t *credential = nullptr;

    while (!authenticated &&
           (message = ssh_message_get(ctx->session)) != nullptr) {
        const int message_type = ssh_message_type(message);
        switch (message_type) {
        case SSH_REQUEST_SERVICE:
            if (!session_handle_service_request(message)) {
                ssh_message_reply_default(message);
            }
            break;
        case SSH_REQUEST_AUTH: {
            const char *username = ssh_message_auth_user(message);
            if (username != nullptr && username[0] != '\0') {
                snprintf(ctx->user.name, sizeof(ctx->user.name), "%.*s",
                         SSH_CHATTER_USERNAME_LEN - 1, username);
            }

            // Load user data
            pthread_mutex_lock(&ctx->owner->user_data_lock);
            user_data_ensure_exists(ctx->owner->user_data_root, ctx->user.name,
                                    ctx->client_ip, &ctx->user_data);
            pthread_mutex_unlock(&ctx->owner->user_data_lock);

            // Check if a password is set for this user
            bool password_is_set = !security_layer_is_zero_hash(
                ctx->user_data.password_hash,
                sizeof(ctx->user_data.password_hash));
            password_is_set =
                is_nullarray((uint8_t *)ctx->user_data.password_hash, 32)
                    ? false
                    : true;

            // Handle LAN operator authentication
            bool reserved_name = false;
            // credential variable is already declared at the beginning of the function
            if (ctx->owner != nullptr) {
                credential = host_find_lan_operator_credential(ctx->owner,
                                                               ctx->user.name);
                reserved_name = credential != nullptr;
            }

            if (reserved_name) {
                if (!session_is_lan_client(ctx->client_ip)) {
                    ssh_message_auth_set_methods(message,
                                                 SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(message);
                    break; // Break from switch, continue while loop
                }

                const int auth_method = ssh_message_subtype(message);
                if (auth_method != SSH_AUTH_METHOD_PASSWORD) {
                    ssh_message_auth_set_methods(message,
                                                 SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(message);
                    break; // Break from switch, continue while loop
                }

                const char *password = ssh_message_auth_password(message);
                if (credential == nullptr || password == nullptr ||
                    credential->password[0] == '\0' ||
                    strcmp(credential->password, password) != 0) {
                    ssh_message_auth_set_methods(message,
                                                 SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(message);
                    break; // Break from switch, continue while loop
                }
                ctx->lan_operator_credentials_valid = true;
                snprintf(ctx->user.name, sizeof(ctx->user.name), "%s",
                         credential->nickname);
                ssh_message_auth_reply_success(message, 0);
                authenticated = true; // This will break the while loop
                break;                // Break from switch
            }

            // Regular user authentication
            if (!password_is_set) {
                // No password set, allow login but flag for password creation
                ctx->password_not_set = true;
                ssh_message_auth_reply_success(message, 0);
                authenticated = true; // This will break the while loop
                break;                // Break from switch
            } else {
                // Password is set, require password authentication
                const int auth_method = ssh_message_subtype(message);
                if (auth_method != SSH_AUTH_METHOD_PASSWORD) {
                    ssh_message_auth_set_methods(message,
                                                 SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(message);
                    break; // Break from switch, continue while loop
                }

                const char *password = ssh_message_auth_password(message);
                if (password == nullptr) {
                    ssh_message_auth_set_methods(message,
                                                 SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(message);
                    break; // Break from switch, continue while loop
                }

                uint8_t provided_password_hash[32];
                security_layer_hash_password(password,
                                             ctx->user_data.password_salt,
                                             provided_password_hash);

                if (memcmp(provided_password_hash, ctx->user_data.password_hash,
                           sizeof(provided_password_hash)) == 0) {
                    ssh_message_auth_reply_success(message, 0);
                    authenticated = true; // This will break the while loop
                    break;                // Break from switch
                } else {
                    ssh_message_auth_set_methods(message,
                                                 SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(message);
                    break; // Break from switch, continue while loop
                }
            }
        }
        default:
            ssh_message_reply_default(message);
            break;
        }
        ssh_message_free(message);
    }

    return authenticated ? 0 : -1;
}

static int session_accept_channel(session_ctx_t *ctx)
{
    ssh_message message = nullptr;

    while ((message = ssh_message_get(ctx->session)) != nullptr) {
        const int message_type = ssh_message_type(message);
        if (message_type == SSH_REQUEST_SERVICE) {
            if (!session_handle_service_request(message)) {
                ssh_message_reply_default(message);
            }
            ssh_message_free(message);
            continue;
        }

        if (message_type == SSH_REQUEST_CHANNEL_OPEN &&
            ssh_message_subtype(message) == SSH_CHANNEL_SESSION) {
            ssh_channel channel =
                ssh_message_channel_request_open_reply_accept(message);
            if (channel == nullptr) {
                accept_channel_fn_t accept_channel =
                    resolve_accept_channel_fn();
                if (accept_channel != nullptr) {
                    channel = ssh_channel_new(ctx->session);
                    if (channel != nullptr) {
                        if (accept_channel(message, channel) != SSH_OK) {
                            ssh_channel_free(channel);
                            channel = nullptr;
                        }
                    }
                }
            }

            if (channel != nullptr) {
                ctx->channel = channel;
                ssh_message_free(message);
                break;
            }

            ssh_message_reply_default(message);
            ssh_message_free(message);
            continue;
        }

        ssh_message_reply_default(message);
        ssh_message_free(message);
    }

    return session_transport_active(ctx) ? 0 : -1;
}

static int session_prepare_shell(session_ctx_t *ctx)
{
    ssh_message message = nullptr;
    bool shell_ready = false;

    while (!shell_ready && (message = ssh_message_get(ctx->session)) != nullptr) {
        if (ssh_message_type(message) == SSH_REQUEST_CHANNEL) {
            const int subtype = ssh_message_subtype(message);
            if (subtype == SSH_CHANNEL_REQUEST_PTY ||
                subtype == SSH_CHANNEL_REQUEST_SHELL) {
                if (subtype == SSH_CHANNEL_REQUEST_PTY) {
                    unsigned int width =
                        ssh_message_channel_request_pty_width(message);
                    unsigned int height =
                        ssh_message_channel_request_pty_height(message);
                    if (width > 0U) {
                        if (width > SSH_CHATTER_MESSAGE_LIMIT) {
                            width = SSH_CHATTER_MESSAGE_LIMIT;
                        }
                        ctx->terminal_width = width;
                    }
                    if (height > 0U) {
                        ctx->terminal_height = height;
                    }
                }
                ssh_message_channel_request_reply_success(message);
                if (subtype == SSH_CHANNEL_REQUEST_SHELL) {
                    shell_ready = true;
                }
            } else {
                ssh_message_reply_default(message);
            }
        } else {
            ssh_message_reply_default(message);
        }
        ssh_message_free(message);
    }

    return shell_ready ? 0 : -1;
}

static const char *
session_captcha_question_for_language(const captcha_prompt_t *prompt,
                                      captcha_language_t language)
{
    if (prompt == nullptr) {
        return nullptr;
    }

    switch (language) {
    case CAPTCHA_LANGUAGE_EN:
        return prompt->question_en;
    case CAPTCHA_LANGUAGE_ZH:
        return prompt->question_zh;
    case CAPTCHA_LANGUAGE_RU:
        return prompt->question_ru;
    case CAPTCHA_LANGUAGE_KO:
    default:
        return prompt->question_ko;
    }
}

static const char *
session_captcha_label_for_language(captcha_language_t language)
{
    switch (language) {
    case CAPTCHA_LANGUAGE_EN:
        return "Captcha: ";
    case CAPTCHA_LANGUAGE_ZH:
        return "驗證碼: ";
    case CAPTCHA_LANGUAGE_RU:
        return "Капча: ";
    case CAPTCHA_LANGUAGE_KO:
    default:
        return "캡챠: ";
    }
}

static captcha_language_t
session_captcha_language_from_ui(session_ui_language_t language)
{
    switch (language) {
    case SESSION_UI_LANGUAGE_EN:
        return CAPTCHA_LANGUAGE_EN;
    case SESSION_UI_LANGUAGE_ZH:
        return CAPTCHA_LANGUAGE_ZH;
    case SESSION_UI_LANGUAGE_RU:
        return CAPTCHA_LANGUAGE_RU;
    case SESSION_UI_LANGUAGE_JP:
        return CAPTCHA_LANGUAGE_EN;
    case SESSION_UI_LANGUAGE_KO:
    default:
        return CAPTCHA_LANGUAGE_KO;
    }
}

static captcha_language_t
session_captcha_primary_language(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return CAPTCHA_LANGUAGE_KO;
    }

    session_ui_language_t preferred = session_ui_language_current(ctx);
    captcha_language_t preferred_language =
        session_captcha_language_from_ui(preferred);
    if (preferred != SESSION_UI_LANGUAGE_KO ||
        preferred_language != CAPTCHA_LANGUAGE_KO) {
        return preferred_language;
    }

    session_ui_language_t geo_language = session_client_geo_language(ctx);
    if (geo_language != SESSION_UI_LANGUAGE_COUNT) {
        return session_captcha_language_from_ui(geo_language);
    }

    char label[64];
    if (session_detect_provider_ip(ctx->client_ip, label, sizeof(label))) {
        if (string_contains_case_insensitive(label, "Chinese")) {
            return CAPTCHA_LANGUAGE_ZH;
        }
        if (string_contains_case_insensitive(label, "Russian")) {
            return CAPTCHA_LANGUAGE_RU;
        }
        if (string_contains_case_insensitive(label, "Korean")) {
            return CAPTCHA_LANGUAGE_KO;
        }
    }

    return CAPTCHA_LANGUAGE_EN;
}

static bool session_captcha_add_language(captcha_language_t *order,
                                         size_t capacity, size_t *count,
                                         bool used[],
                                         captcha_language_t language)
{
    if (order == nullptr || count == nullptr || used == nullptr) {
        return false;
    }

    size_t index = (size_t)language;
    if (index >= CAPTCHA_LANGUAGE_COUNT) {
        return false;
    }

    if (used[index] || *count >= capacity) {
        return false;
    }

    order[*count] = language;
    used[index] = true;
    ++(*count);
    return true;
}

static size_t session_collect_captcha_languages(const session_ctx_t *ctx,
                                                captcha_language_t *order,
                                                size_t capacity)
{
    if (order == nullptr || capacity == 0U) {
        return 0U;
    }

    bool used[CAPTCHA_LANGUAGE_COUNT] = {false};
    size_t count = 0U;

    captcha_language_t primary = session_captcha_primary_language(ctx);
    session_captcha_add_language(order, capacity, &count, used, primary);

    if (ctx != nullptr) {
        captcha_language_t user_pref =
            session_captcha_language_from_ui(session_ui_language_current(ctx));
        session_captcha_add_language(order, capacity, &count, used, user_pref);

        char label[64];
        if (session_detect_provider_ip(ctx->client_ip, label, sizeof(label))) {
            if (string_contains_case_insensitive(label, "Chinese")) {
                session_captcha_add_language(order, capacity, &count, used,
                                             CAPTCHA_LANGUAGE_ZH);
            }
            if (string_contains_case_insensitive(label, "Russian")) {
                session_captcha_add_language(order, capacity, &count, used,
                                             CAPTCHA_LANGUAGE_RU);
            }
            if (string_contains_case_insensitive(label, "Korean")) {
                session_captcha_add_language(order, capacity, &count, used,
                                             CAPTCHA_LANGUAGE_KO);
            }
        }
    }

    session_captcha_add_language(order, capacity, &count, used,
                                 CAPTCHA_LANGUAGE_EN);
    session_captcha_add_language(order, capacity, &count, used,
                                 CAPTCHA_LANGUAGE_KO);

    static const captcha_language_t kFallbackOrder[] = {
        CAPTCHA_LANGUAGE_ZH,
        CAPTCHA_LANGUAGE_RU,
        CAPTCHA_LANGUAGE_EN,
        CAPTCHA_LANGUAGE_KO,
    };

    for (size_t idx = 0U;
         idx < sizeof(kFallbackOrder) / sizeof(kFallbackOrder[0]); ++idx) {
        session_captcha_add_language(order, capacity, &count, used,
                                     kFallbackOrder[idx]);
    }

    return count;
}

static void session_send_captcha_prompt(session_ctx_t *ctx,
                                        const captcha_prompt_t *prompt,
                                        const captcha_language_t *order,
                                        size_t count)
{
    if (ctx == nullptr || prompt == nullptr || order == nullptr || count == 0U) {
        return;
    }

    for (size_t idx = 0U; idx < count; ++idx) {
        captcha_language_t language = order[idx];
        const char *label = session_captcha_label_for_language(language);
        const char *question =
            session_captcha_question_for_language(prompt, language);
        if (label == nullptr || question == nullptr || question[0] == '\0') {
            continue;
        }

        char line[sizeof(prompt->question_en) + 32];
        int written = snprintf(line, sizeof(line), "%s%s", label, question);
        if (written <= 0) {
            continue;
        }

        session_send_system_line(ctx, line);
    }
}

static void host_update_last_captcha_prompt(host_t *host,
                                            const captcha_prompt_t *prompt,
                                            const captcha_language_t *order,
                                            size_t count)
{
    if (host == nullptr || prompt == nullptr) {
        return;
    }

    static const captcha_language_t kDefaultOrder[] = {
        CAPTCHA_LANGUAGE_KO,
        CAPTCHA_LANGUAGE_EN,
        CAPTCHA_LANGUAGE_ZH,
        CAPTCHA_LANGUAGE_RU,
    };

    const captcha_language_t *languages = order;
    size_t language_count = count;
    if (languages == nullptr || language_count == 0U) {
        languages = kDefaultOrder;
        language_count = sizeof(kDefaultOrder) / sizeof(kDefaultOrder[0]);
    }

    char combined_question[sizeof(prompt->question_en) +
                           sizeof(prompt->question_ko) +
                           sizeof(prompt->question_ru) +
                           sizeof(prompt->question_zh) + 64];
    combined_question[0] = '\0';
    size_t combined_length = 0U;

    for (size_t idx = 0U; idx < language_count; ++idx) {
        const char *label = session_captcha_label_for_language(languages[idx]);
        const char *question =
            session_captcha_question_for_language(prompt, languages[idx]);
        if (label == nullptr || question == nullptr || question[0] == '\0') {
            continue;
        }

        char line[sizeof(prompt->question_en) + 32];
        int written = snprintf(line, sizeof(line), "%s%s", label, question);
        if (written <= 0) {
            continue;
        }

        size_t line_length = (size_t)written;
        if (combined_length > 0U &&
            combined_length + 1U < sizeof(combined_question)) {
            combined_question[combined_length++] = '\n';
        }

        if (combined_length >= sizeof(combined_question)) {
            break;
        }

        size_t available = sizeof(combined_question) - combined_length;
        if (available == 0U) {
            break;
        }

        if (line_length >= available) {
            line_length = available - 1U;
        }

        memcpy(combined_question + combined_length, line, line_length);
        combined_length += line_length;
        combined_question[combined_length] = '\0';
    }

    pthread_mutex_lock(&host->lock);
    snprintf(host->last_captcha_question, sizeof(host->last_captcha_question),
             "%s", combined_question);
    snprintf(host->last_captcha_answer, sizeof(host->last_captcha_answer), "%s",
             prompt->answer);
    host->has_last_captcha = host->last_captcha_question[0] != '\0' &&
                             host->last_captcha_answer[0] != '\0';
    if (host->has_last_captcha) {
        if (clock_gettime(CLOCK_REALTIME, &host->last_captcha_generated) != 0) {
            host->last_captcha_generated.tv_sec = time(nullptr);
            host->last_captcha_generated.tv_nsec = 0L;
        }
    } else {
        host->last_captcha_generated.tv_sec = 0;
        host->last_captcha_generated.tv_nsec = 0L;
    }
    pthread_mutex_unlock(&host->lock);
}

static bool session_run_captcha(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return false;
    }

    captcha_prompt_t prompt;
    session_build_captcha_prompt(ctx, &prompt);
    captcha_language_t languages[CAPTCHA_LANGUAGE_COUNT];
    size_t language_count = session_collect_captcha_languages(
        ctx, languages, sizeof(languages) / sizeof(languages[0]));
    if (language_count == 0U) {
        languages[0] = CAPTCHA_LANGUAGE_KO;
        language_count = 1U;
    }

    host_update_last_captcha_prompt(ctx->owner, &prompt, languages,
                                    language_count);

    bool include_chinese = false;
    for (size_t idx = 0U; idx < language_count; ++idx) {
        if (languages[idx] == CAPTCHA_LANGUAGE_ZH) {
            include_chinese = true;
            break;
        }
    }

    session_send_system_line(
        ctx, "For Windows users: CHANGE TERMINAL ENCODING TO UTF-8");
    if (include_chinese) {
        session_send_system_line(
            ctx, "INFO: Chinese question is in Traditional one to "
                 "cover regions those are NOT Mainland China.");
    }
    session_send_system_line(
        ctx, "Before entering the room, solve this small puzzle.");
    session_send_captcha_prompt(ctx, &prompt, languages, language_count);
    session_send_system_line(ctx, "Type your answer and press Enter:");

    char answer[sizeof(prompt.answer)];
    size_t length = 0U;
    while (length + 1U < sizeof(answer)) {
        char ch = '\0';
        const int read_result = session_transport_read(ctx, &ch, 1, -1);
        if (read_result <= 0) {
            return false;
        }

        if (ch == '\r' || ch == '\n') {
            session_local_echo_char(ctx, '\n');
            break;
        }

        if (ch == '\b' || (unsigned char)ch == 0x7fU) {
            if (length > 0U) {
                --length;
                session_send_raw_text(ctx, "\b \b");
            }
            continue;
        }

        if ((unsigned char)ch < 0x20U) {
            continue;
        }

        answer[length++] = ch;
        session_local_echo_char(ctx, ch);
    }
    answer[length] = '\0';
    trim_whitespace_inplace(answer);

    if (answer[0] == '\0') {
        session_send_system_line(ctx, "Captcha answer missing. Disconnecting.");
        return false;
    }

    if (strcasecmp(prompt.answer, "dog") == 0 && strcmp(answer, "개") == 0) {
        snprintf(answer, sizeof(answer), "%s", "dog");
    }

    if (strcasecmp(answer, prompt.answer) == 0) {
        session_send_system_line(ctx, "Captcha solved. Welcome aboard!");
        return true;
    }

    session_send_system_line(ctx, "Captcha failed. Disconnecting.");
    return false;
}

static bool session_is_captcha_exempt(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }

    if (ctx->user.name[0] == '\0') {
        return false;
    }

    char lowered[sizeof(ctx->user.name)];
    size_t idx = 0U;
    for (; idx + 1U < sizeof(lowered) && ctx->user.name[idx] != '\0'; ++idx) {
        lowered[idx] = (char)tolower((unsigned char)ctx->user.name[idx]);
    }
    if (idx < sizeof(lowered)) {
        lowered[idx] = '\0';
    } else {
        lowered[sizeof(lowered) - 1U] = '\0';
    }

    return strcmp(lowered, "gpt") == 0;
}

static void session_print_help(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    const session_ui_locale_t *locale = session_ui_get_locale(ctx);
    const char *prefix = session_command_prefix(ctx);

    char help_buffer[SSH_CHATTER_MESSAGE_LIMIT *
                     32]; // A large enough buffer for help messages
    help_buffer[0] = '\0';

    if (locale->help_title != nullptr && locale->help_title[0] != '\0') {
        session_send_system_line(ctx, locale->help_title);
    }

    session_format_help_entries_to_buffer(ctx, kSessionHelpEssential,
                                          sizeof(kSessionHelpEssential) /
                                              sizeof(kSessionHelpEssential[0]),
                                          help_buffer, sizeof(help_buffer));
    session_send_raw_text(ctx, help_buffer);

    if (locale->help_hint_extra != nullptr && locale->help_hint_extra[0] != '\0') {
        const char *args[] = {prefix};
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        session_format_template(locale->help_hint_extra, args,
                                sizeof(args) / sizeof(args[0]), line,
                                sizeof(line));
        session_send_system_line(ctx, line);
    }

    if (locale->help_scroll_hint != nullptr &&
        locale->help_scroll_hint[0] != '\0') {
        session_send_system_line(ctx, locale->help_scroll_hint);
    }

    if (locale->help_regular_hint != nullptr &&
        locale->help_regular_hint[0] != '\0') {
        session_send_system_line(ctx, locale->help_regular_hint);
    }
}

static bool session_line_is_exit_command(const char *line)
{
    if (line == nullptr) {
        return false;
    }

    if (strncmp(line, "/exit", 5) != 0) {
        return false;
    }

    const char trailing = line[5];
    if (trailing == '\0') {
        return true;
    }

    if (!isspace((unsigned char)trailing)) {
        return false;
    }

    for (size_t idx = 6U; line[idx] != '\0'; ++idx) {
        if (!isspace((unsigned char)line[idx])) {
            return false;
        }
    }

    return true;
}

static void session_handle_username_conflict_input(session_ctx_t *ctx,
                                                   const char *line)
{
    if (ctx == nullptr) {
        return;
    }

    if (session_line_is_exit_command(line)) {
        ctx->ops->handle_exit(ctx);
        return;
    }

    char reminder[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(reminder, sizeof(reminder), "The username '%s' is already in use.",
             ctx->user.name);
    session_send_system_line(ctx, reminder);
    session_send_system_line(
        ctx, "Reconnect with a different username by running: ssh "
             "newname@<server> (or ssh -l newname <server>)");
    session_send_system_line(ctx, "Type /exit to quit.");
}

static bool session_prepare_slash_command(const char *input, char *output,
                                          size_t length)
{
    if (input == nullptr || output == nullptr || length == 0U) {
        return false;
    }

    const unsigned char *start = (const unsigned char *)input;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        ++start;
    }

    if (*start != '/') {
        return false;
    }

    const unsigned char *end = start;
    while (*end != '\0') {
        ++end;
    }
    while (end > start && isspace((unsigned char)*(end - 1U))) {
        --end;
    }

    size_t copy_len = (size_t)(end - start);
    if (copy_len == 0U) {
        return false;
    }
    if (copy_len >= length) {
        copy_len = length - 1U;
    }

    memcpy(output, start, copy_len);
    output[copy_len] = '\0';
    return true;
}

static void session_process_line(session_ctx_t *ctx, const char *line)
{
    if (ctx == nullptr || line == nullptr) {
        return;
    }

    char normalized[SSH_CHATTER_MAX_INPUT_LEN];
    snprintf(normalized, sizeof(normalized), "%s", line);
    session_normalize_newlines(normalized);

    switch ((int)normalized[0]) {
    // SLASH_COMPATIBLE: slash compatible chars.
    case (int)'.':
    case (int)'_':
    case (int)'@':
    case (int)'$':
    case (int)'*':
    case (int)'-':
    case (int)'#':
    case (int)'>':
        normalized[0] = '/';
        break;
    default:
    }

    if (ctx->bbs_post_pending) {
        session_bbs_capture_body_text(ctx, normalized);
        return;
    }

    if (ctx->asciiart_pending) {
        session_asciiart_capture_text(ctx, normalized);
        return;
    }

    char command_line[SSH_CHATTER_MAX_INPUT_LEN];

    if (normalized[0] == '\0') {
        return;
    }

    if (ctx->game.active) {
        if (strcmp(normalized, "/suspend!") == 0) {
            session_game_suspend(ctx, "Game suspended.");
            return;
        }

        if (normalized[0] == 't' && normalized[1] == '\0') {
            if (ctx->game.is_camouflaged) {
                ctx->game.is_camouflaged = false;
                if (ctx->game.type == SESSION_GAME_TETRIS) {
                    ctx->game.tetris = ctx->game.saved_tetris_state;
                    ctx->game.tetris.gravity_timer_initialized = false;
                    ctx->game.tetris.gravity_timer_accumulator_ns = 0U;
                    session_game_tetris_render(ctx);
                } else if (ctx->game.type == SESSION_GAME_LIARGAME) {
                    ctx->game.liar = ctx->game.saved_liar_state;
                    session_game_liar_present_round(ctx);
                } else if (ctx->game.type == SESSION_GAME_ALPHA) {
                    ctx->game.alpha = ctx->game.saved_alpha_state;
                    session_game_alpha_present_stage(ctx);
                } else if (ctx->game.type == SESSION_GAME_OTHELLO) {
                    ctx->game.othello = ctx->game.saved_othello_state;
                    session_game_othello_render(ctx);
                    session_game_othello_prepare_next_turn(ctx);
                }
            } else {
                ctx->game.is_camouflaged = true;
                if (ctx->game.type == SESSION_GAME_TETRIS) {
                    ctx->game.saved_tetris_state = ctx->game.tetris;
                    ctx->game.saved_tetris_state.gravity_timer_initialized =
                        false;
                    ctx->game.saved_tetris_state.gravity_timer_accumulator_ns =
                        0U;
                    ctx->game.tetris.gravity_timer_initialized = false;
                    ctx->game.tetris.gravity_timer_accumulator_ns = 0U;
                } else if (ctx->game.type == SESSION_GAME_LIARGAME) {
                    ctx->game.saved_liar_state = ctx->game.liar;
                } else if (ctx->game.type == SESSION_GAME_ALPHA) {
                    ctx->game.saved_alpha_state = ctx->game.alpha;
                } else if (ctx->game.type == SESSION_GAME_OTHELLO) {
                    ctx->game.saved_othello_state = ctx->game.othello;
                }
                session_game_show_camouflage(ctx);
            }
            return;
        }

        if (normalized[0] == '/') {
            session_send_system_line(
                ctx, "Finish the current game with /suspend! first.");
            return;
        }

        if (ctx->game.type == SESSION_GAME_TETRIS) {
            session_game_tetris_handle_line(ctx, normalized);
        } else if (ctx->game.type == SESSION_GAME_LIARGAME) {
            session_game_liar_handle_line(ctx, normalized);
        } else if (ctx->game.type == SESSION_GAME_ALPHA) {
            session_game_alpha_handle_line(ctx, normalized);
        } else if (ctx->game.type == SESSION_GAME_OTHELLO) {
            session_game_othello_handle_line(ctx, normalized);
        }
        return;
    }

    if (ctx->in_rss_mode) {
        if (strcmp(normalized, "/exit") == 0) {
            session_rss_exit(ctx, nullptr);
        } else {
            const char *rss_args = nullptr;
            if (session_parse_command(normalized, "/rss", &rss_args)) {
                session_rss_exit(ctx, nullptr);
                session_handle_rss(ctx, rss_args);
            } else {
                session_send_system_line(
                    ctx, "RSS reader active. Use /rss exit Ctrl+Z, or "
                         "Terminate to return to chat.");
            }
        }
        return;
    }

    bool translation_bypass = false;
    char bypass_buffer[SSH_CHATTER_MAX_INPUT_LEN];
    if (translation_strip_no_translate_prefix(normalized, bypass_buffer,
                                              sizeof(bypass_buffer))) {
        translation_bypass = true;
        snprintf(normalized, sizeof(normalized), "%s", bypass_buffer);
    }

    if (normalized[0] == '\0') {
        return;
    }

    const struct timespec tiny_delay = {.tv_sec = 0, .tv_nsec = 5000000L};
    host_sleep_uninterruptible(&tiny_delay);

    if (ctx->username_conflict) {
        session_handle_username_conflict_input(ctx, normalized);
        return;
    }

    if (!translation_bypass) {
        if (session_prepare_slash_command(normalized, command_line,
                                          sizeof(command_line))) {
            if (session_try_localized_command_forward(ctx, command_line)) {
                return;
            }
            ctx->ops->dispatch_command(ctx, command_line);
            return;
        }
    }

    if (!translation_bypass && normalized[0] == '/') {
        if (session_try_localized_command_forward(ctx, normalized)) {
            return;
        }
        ctx->ops->dispatch_command(ctx, normalized);
        return;
    }

    const char *trimmed = normalized;
    while (*trimmed == ' ' || *trimmed == '\t') {
        ++trimmed;
    }

    if (!translation_bypass && ctx->input_mode == SESSION_INPUT_MODE_COMMAND &&
        *trimmed != '\0') {
        const char *command_text = trimmed;
        char command_buffer[SSH_CHATTER_MAX_INPUT_LEN];
        if (command_text[0] != '/') {
            command_buffer[0] = '/';
            size_t command_len =
                strnlen(command_text, sizeof(command_buffer) - 2U);
            memcpy(&command_buffer[1], command_text, command_len);
            command_buffer[command_len + 1U] = '\0';
            command_text = command_buffer;
        }
        if (session_prepare_slash_command(command_text, command_buffer,
                                          sizeof(command_buffer))) {
            ctx->ops->dispatch_command(ctx, command_buffer);
        } else {
            ctx->ops->dispatch_command(ctx, command_text);
        }
        return;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        now.tv_sec = time(nullptr);
        now.tv_nsec = 0L;
    }

    const bool asciiart_active = ctx->asciiart_pending;
    bool ascii_profile_command = asciiart_active;
    if (!ascii_profile_command && normalized[0] == '/') {
        const char *command_args = nullptr;
        if (session_parse_command(normalized, "/asciiart", &command_args) ||
            session_parse_command(normalized, "/profilepic", &command_args)) {
            ascii_profile_command = true;
        }
    }

    const bool translation_throttle =
        ctx->translation_enabled && ctx->input_translation_enabled &&
        ctx->input_translation_language[0] != '\0';
    const bool chat_throttle = ctx->input_mode == SESSION_INPUT_MODE_CHAT;
    if ((translation_throttle || chat_throttle) && ctx->has_last_message_time) {
        time_t sec_delta = now.tv_sec - ctx->last_message_time.tv_sec;
        long nsec_delta = now.tv_nsec - ctx->last_message_time.tv_nsec;
        if (nsec_delta < 0L) {
            --sec_delta;
            nsec_delta += 1000000000L;
        }
        if (translation_throttle &&
            (sec_delta < 0 || (sec_delta == 0 && nsec_delta < 1000000000L))) {
            session_send_system_line(ctx, "Please wait at least one second "
                                          "before sending another message.");
            return;
        }
        if (!translation_throttle && chat_throttle && !ascii_profile_command &&
            !ctx->bracket_paste_active &&
            (sec_delta < 0 || (sec_delta == 0 && nsec_delta < 300000000L))) {
            session_send_system_line(ctx,
                                     "Please wait at least 300 milliseconds "
                                     "before sending another chat message.");
            return;
        }
    }

    ctx->last_message_time = now;
    ctx->has_last_message_time = true;

    if (session_first_message_is_suspicious(ctx, normalized)) {
        session_handle_suspicious_first_message(ctx, normalized);
        return;
    }
    if (!translation_bypass && ctx->translation_enabled &&
        ctx->input_translation_enabled &&
        ctx->input_translation_language[0] != '\0') {
        if (session_translation_queue_input(ctx, normalized)) {
            return;
        }
        session_send_system_line(
            ctx, "Translation unavailable; sending your original message.");
    }

    printf("[%s] %s\n", ctx->user.name, normalized);
    session_deliver_outgoing_message(ctx, normalized, true);
}

void host_session_process_line_for_testing(session_ctx_t *ctx, const char *line)
{
    session_process_line(ctx, line);
}

static void session_handle_kick(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->user.is_operator) {
        session_send_system_line(ctx, "You are not allowed to kick users.");
        return;
    }

    if (arguments == nullptr || *arguments == '\0') {
        session_send_system_line(ctx, "Usage: /kick <username>");
        return;
    }

    char target_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(target_name, sizeof(target_name), "%s", arguments);
    trim_whitespace_inplace(target_name);

    if (target_name[0] == '\0') {
        session_send_system_line(ctx, "Usage: /kick <username>");
        return;
    }

    session_ctx_t *target = chat_room_find_user(&ctx->owner->room, target_name);
    if (target == nullptr) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "User '%s' is not connected.",
                 target_name);
        session_send_system_line(ctx, message);
        return;
    }

    if (target == ctx) {
        session_send_system_line(ctx, "You cannot kick yourself.");
        return;
    }

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice), "* [%s] has been kicked by [%s]",
             target->user.name, ctx->user.name);
    host_history_record_system(ctx->owner, notice, nullptr);
    chat_room_broadcast(&ctx->owner->room, notice, nullptr);

    const bool target_active = session_transport_active(target);
    if (!target_active || (target->transport_kind == SESSION_TRANSPORT_SSH &&
                           target->session == nullptr)) {
        target->should_exit = true;
        target->has_joined_room = false;
        chat_room_remove(&ctx->owner->room, target);
        session_send_system_line(ctx, "User removed from the chat.");
    } else {
        session_send_system_line(target,
                                 "You have been kicked by an operator.");
        target->should_exit = true;
        session_transport_request_close(target);
        target->has_joined_room = false;
        chat_room_remove(&ctx->owner->room, target);
        session_send_system_line(ctx, "User removed from the chat.");
    }

    printf("[kick] %s kicked %s\n", ctx->user.name, target->user.name);
}

static void session_handle_ban_name(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (!ctx->user.is_operator) {
        session_send_system_line(ctx, "You are not allowed to ban nicknames.");
        return;
    }

    if (ctx->owner == nullptr) {
        session_send_system_line(ctx, "Host unavailable.");
        return;
    }

    if (arguments == nullptr || *arguments == '\0') {
        session_send_system_line(ctx, "Usage: /banname <nickname>");
        return;
    }

    char target_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(target_name, sizeof(target_name), "%s", arguments);
    trim_whitespace_inplace(target_name);

    if (target_name[0] == '\0') {
        session_send_system_line(ctx, "Usage: /banname <nickname>");
        return;
    }

    for (size_t idx = 0U; target_name[idx] != '\0'; ++idx) {
        const unsigned char ch = (unsigned char)target_name[idx];
        if (ch <= 0x1FU || ch == 0x7FU || ch == ' ' || ch == '\t') {
            session_send_system_line(
                ctx,
                "Nicknames may not include control characters or whitespace.");
            return;
        }
    }

    if (host_is_username_banned(ctx->owner, target_name)) {
        session_send_system_line(
            ctx, "That nickname is already blocked for bot detection.");
        return;
    }

    if (!host_add_ban_entry(ctx->owner, target_name, "")) {
        session_send_system_line(ctx, "Unable to add ban entry (list full?).");
        return;
    }

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice),
             "* Nickname '%s' blocked for bot detection by [%s]", target_name,
             ctx->user.name);
    host_history_record_system(ctx->owner, notice, nullptr);
    chat_room_broadcast(&ctx->owner->room, notice, nullptr);
    session_send_system_line(ctx, "Nickname ban applied.");
    printf("[banname] %s banned nickname %s\n", ctx->user.name, target_name);

    session_ctx_t *active = chat_room_find_user(&ctx->owner->room, target_name);
    if (active != nullptr) {
        session_send_system_line(
            active, "Your nickname is now blocked for bot detection. "
                    "Use /nick <name> to change immediately.");
    }
}

static void session_handle_ban(session_ctx_t *ctx, const char *arguments)
{
    if (!ctx->user.is_operator) {
        session_send_system_line(ctx, "You are not allowed to ban users.");
        return;
    }

    if (arguments == nullptr || *arguments == '\0') {
        session_send_system_line(ctx, "Usage: /ban <username>");
        return;
    }

    char target_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(target_name, sizeof(target_name), "%s", arguments);
    trim_whitespace_inplace(target_name);

    if (target_name[0] == '\0') {
        session_send_system_line(ctx, "Usage: /ban <username>");
        return;
    }

    session_ctx_t *target = chat_room_find_user(&ctx->owner->room, target_name);
    if (target == nullptr) {
        bool valid_ip = false;
        unsigned char inet_buffer[sizeof(struct in6_addr)];
        if (inet_pton(AF_INET, target_name, inet_buffer) == 1 ||
            inet_pton(AF_INET6, target_name, inet_buffer) == 1) {
            valid_ip = true;
        }

        bool valid_cidr = false;
        if (!valid_ip && strchr(target_name, '/') != nullptr) {
            uint32_t ipv4_network = 0U;
            uint32_t ipv4_mask = 0U;
            struct in6_addr ipv6_network;
            struct in6_addr ipv6_mask;
            memset(&ipv6_network, 0, sizeof(ipv6_network));
            memset(&ipv6_mask, 0, sizeof(ipv6_mask));
            valid_cidr =
                host_parse_ipv4_cidr(target_name, &ipv4_network, &ipv4_mask) ||
                host_parse_ipv6_cidr(target_name, &ipv6_network, &ipv6_mask);
        }

        if (valid_ip || valid_cidr) {
            if (host_add_ban_entry(ctx->owner, "", target_name)) {
                char notice[SSH_CHATTER_MESSAGE_LIMIT];
                const char *label = valid_cidr ? "CIDR" : "IP";
                snprintf(notice, sizeof(notice), "%s '%s' has been banned.",
                         label, target_name);
                session_send_system_line(ctx, notice);
            } else {
                session_send_system_line(
                    ctx, "Unable to add ban entry (list full?).");
            }
        } else {
            char not_found[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(not_found, sizeof(not_found),
                     "User '%s' is not connected.", target_name);
            session_send_system_line(ctx, not_found);
        }
        return;
    }

    if (target->user.is_lan_operator) {
        session_send_system_line(ctx, "LAN operators cannot be banned.");
        return;
    }

    const char *target_ip =
        target->client_ip[0] != '\0' ? target->client_ip : "";
    if (!host_add_ban_entry(ctx->owner, target->user.name, target_ip)) {
        session_send_system_line(ctx, "Unable to add ban entry (list full?).");
        return;
    }

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice), "* [%s] has been banned by [%s]",
             target->user.name, ctx->user.name);
    host_history_record_system(ctx->owner, notice, nullptr);
    chat_room_broadcast(&ctx->owner->room, notice, nullptr);
    session_send_system_line(ctx, "Ban applied.");
    printf("[ban] %s banned %s (%s)\n", ctx->user.name, target->user.name,
           target_ip[0] != '\0' ? target_ip : "unknown");

    if (session_transport_active(target)) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "You have been banned by [%s].",
                 ctx->user.name);
        session_send_system_line(target, message);
        target->should_exit = true;
        session_transport_request_close(target);
    }
}

static void session_handle_ban_list(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (!ctx->user.is_operator) {
        session_send_system_line(ctx,
                                 "You are not allowed to view the ban list.");
        return;
    }

    if (arguments != nullptr) {
        while (*arguments != '\0' && isspace((unsigned char)*arguments)) {
            ++arguments;
        }
        if (*arguments != '\0') {
            session_send_system_line(ctx, "Usage: /banlist");
            return;
        }
    }

    host_t *host = ctx->owner;
    if (host == nullptr) {
        session_send_system_line(ctx, "Host unavailable.");
        return;
    }

    typedef struct ban_snapshot {
        char username[SSH_CHATTER_USERNAME_LEN];
        char ip[SSH_CHATTER_IP_LEN];
    } ban_snapshot_t;

    ban_snapshot_t entries[SSH_CHATTER_MAX_BANS];
    size_t entry_count = 0U;

    pthread_mutex_lock(&host->lock);
    entry_count = host->ban_count;
    if (entry_count > SSH_CHATTER_MAX_BANS) {
        entry_count = SSH_CHATTER_MAX_BANS;
    }
    for (size_t idx = 0U; idx < entry_count; ++idx) {
        snprintf(entries[idx].username, sizeof(entries[idx].username), "%s",
                 host->bans[idx].username);
        snprintf(entries[idx].ip, sizeof(entries[idx].ip), "%s",
                 host->bans[idx].ip);
    }
    pthread_mutex_unlock(&host->lock);

    if (entry_count == 0U) {
        session_send_system_line(ctx, "No active bans.");
        return;
    }

    session_send_system_line(ctx, "Active bans:");
    for (size_t idx = 0U; idx < entry_count; ++idx) {
        const char *username = entries[idx].username;
        const char *ip = entries[idx].ip;
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        if (username[0] != '\0' && ip[0] != '\0') {
            snprintf(message, sizeof(message), "%zu. user: %s, ip: %s",
                     idx + 1U, username, ip);
        } else if (username[0] != '\0') {
            snprintf(message, sizeof(message), "%zu. user: %s", idx + 1U,
                     username);
        } else if (ip[0] != '\0') {
            snprintf(message, sizeof(message), "%zu. ip: %s", idx + 1U, ip);
        } else {
            snprintf(message, sizeof(message), "%zu. <empty>", idx + 1U);
        }
        session_send_system_line(ctx, message);
    }
}

static void session_handle_getaddr(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(ctx,
                                 "You are not allowed to run that command.");
        return;
    }

    if (arguments == nullptr || *arguments == '\0') {
        session_send_system_line(ctx, "Usage: /getaddr <username>");
        return;
    }

    char target_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(target_name, sizeof(target_name), "%s", arguments);
    trim_whitespace_inplace(target_name);

    if (target_name[0] == '\0') {
        session_send_system_line(ctx, "Usage: /getaddr <username>");
        return;
    }

    host_t *host = ctx->owner;
    if (host == nullptr) {
        session_send_system_line(ctx, "Host unavailable.");
        return;
    }

    char ip[SSH_CHATTER_IP_LEN];
    if (!host_lookup_last_ip(host, target_name, ip, sizeof(ip)) ||
        ip[0] == '\0') {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "No recorded address for '%s'.",
                 target_name);
        session_send_system_line(ctx, message);
        return;
    }

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(message, sizeof(message), "Last known address for '%s': %s",
             target_name, ip);
    session_send_system_line(ctx, message);
}

static void session_handle_ircserver(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(ctx,
                                 "You are not allowed to run that command.");
        return;
    }

    host_t *host = ctx->owner;
    if (host == nullptr) {
        session_send_system_line(ctx, "Host unavailable.");
        return;
    }

    if (host->irc_client == nullptr) {
        session_send_system_line(
            ctx,
            "IRC relay is not configured. Set CHATTER_IRC_SERVER, "
            "CHATTER_IRC_PORT, and CHATTER_IRC_CHANNEL environment variables.");
        return;
    }

    static const char *kUsage =
        "Usage: /ircserver status|reconnect|disconnect";
    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/ircserver", kUsage, usage,
                                 sizeof(usage));

    if (arguments == nullptr || *arguments == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    char command[64];
    snprintf(command, sizeof(command), "%s", arguments);
    trim_whitespace_inplace(command);

    if (strcmp(command, "status") == 0) {
        const char *status = irc_client_get_status(host->irc_client);
        bool connected = irc_client_is_connected(host->irc_client);
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "IRC Relay Status: %s (%s)", status,
                 connected ? "connected" : "disconnected");
        session_send_system_line(ctx, message);
    } else if (strcmp(command, "reconnect") == 0) {
        session_send_system_line(ctx, "Attempting to reconnect to IRC server...");
        if (irc_client_reconnect(host->irc_client)) {
            session_send_system_line(ctx, "IRC reconnection initiated.");
        } else {
            session_send_system_line(ctx, "Failed to reconnect to IRC server.");
        }
    } else if (strcmp(command, "disconnect") == 0) {
        irc_client_disconnect(host->irc_client);
        session_send_system_line(ctx, "Disconnected from IRC server.");
    } else {
        session_send_system_line(ctx, usage);
    }
}

static void session_handle_poke(session_ctx_t *ctx, const char *arguments)
{
    if (arguments == nullptr || *arguments == '\0') {
        session_send_system_line(ctx, "Usage: /poke <username>");
        return;
    }

    session_ctx_t *target = chat_room_find_user(&ctx->owner->room, arguments);
    if (target == nullptr) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "User '%s' is not connected.",
                 arguments);
        session_send_system_line(ctx, message);
        return;
    }

    printf("[poke] %s pokes %s\n", ctx->user.name, target->user.name);
    session_channel_write(target, "\a", 1U);
    session_send_system_line(ctx, "Poke sent.");
}

static void session_handle_block(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    static const char *kUsage =
        "Usage: /block <username|ip|list|confirm <username> <only|ip>>";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/block", kUsage, usage, sizeof(usage));

    if (arguments == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    if (strcasecmp(working, "list") == 0) {
        session_blocklist_show(ctx);
        return;
    }

    if (strncasecmp(working, "confirm", 7) == 0 &&
        (working[7] == '\0' || isspace((unsigned char)working[7]))) {
        char *cursor = working + 7;
        while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
            ++cursor;
        }

        if (*cursor == '\0') {
            session_send_system_line(ctx, usage);
            return;
        }

        char username[SSH_CHATTER_USERNAME_LEN];
        size_t name_len = 0U;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor) &&
               name_len + 1U < sizeof(username)) {
            username[name_len++] = *cursor++;
        }
        username[name_len] = '\0';

        while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
            ++cursor;
        }

        if (*cursor == '\0') {
            session_send_system_line(ctx, usage);
            return;
        }

        char mode[16];
        size_t mode_len = 0U;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor) &&
               mode_len + 1U < sizeof(mode)) {
            mode[mode_len++] = *cursor++;
        }
        mode[mode_len] = '\0';

        if (!ctx->block_pending.active) {
            session_send_system_line(
                ctx, "No provider block is awaiting confirmation.");
            return;
        }

        if (strncmp(ctx->block_pending.username, username,
                    SSH_CHATTER_USERNAME_LEN) != 0) {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message),
                     "Pending block is for [%s], not [%s].",
                     ctx->block_pending.username, username);
            session_send_system_line(ctx, message);
            return;
        }

        bool block_ip = false;
        if (strcasecmp(mode, "ip") == 0 || strcasecmp(mode, "all") == 0 ||
            strcasecmp(mode, "full") == 0) {
            block_ip = true;
        } else if (strcasecmp(mode, "only") == 0 ||
                   strcasecmp(mode, "user") == 0 ||
                   strcasecmp(mode, "name") == 0) {
            block_ip = false;
        } else {
            session_send_system_line(ctx, usage);
            return;
        }

        bool already_present = false;
        if (!session_blocklist_add(ctx, ctx->block_pending.ip,
                                   ctx->block_pending.username, block_ip,
                                   &already_present)) {
            if (already_present) {
                session_send_system_line(ctx,
                                         "That target is already blocked.");
            } else {
                session_send_system_line(
                    ctx, "Unable to add block entry (limit reached?).");
            }
        } else {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            if (block_ip) {
                snprintf(message, sizeof(message),
                         "Blocking all users from %.63s.",
                         ctx->block_pending.ip);
            } else {
                snprintf(message, sizeof(message),
                         "Blocking [%.23s] only (IP %.63s).",
                         ctx->block_pending.username, ctx->block_pending.ip);
            }
            session_send_system_line(ctx, message);
        }

        ctx->block_pending.active = false;
        ctx->block_pending.username[0] = '\0';
        ctx->block_pending.ip[0] = '\0';
        ctx->block_pending.provider_label[0] = '\0';
        return;
    }

    unsigned char inet_buffer[sizeof(struct in6_addr)];
    if (inet_pton(AF_INET, working, inet_buffer) == 1 ||
        inet_pton(AF_INET6, working, inet_buffer) == 1) {
        bool already_present = false;
        char label[64];
        bool provider =
            session_detect_provider_ip(working, label, sizeof(label));
        if (provider && label[0] != '\0') {
            char warning[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(
                warning, sizeof(warning),
                "Error: You cannot ban a country."
                "%.256s is flagged as %.63s; other people may also be hidden.",
                working, label);
            session_send_system_line(ctx, warning);
            return;
        }
        if (!session_blocklist_add(ctx, working, "", true, &already_present)) {
            if (already_present) {
                session_send_system_line(ctx, "That IP is already blocked.");
            } else {
                session_send_system_line(
                    ctx, "Unable to add block entry (limit reached?).");
            }
        } else {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message),
                     "Blocking all users from %.256s.", working);
            session_send_system_line(ctx, message);
        }
        return;
    }

    if (ctx->owner == nullptr) {
        session_send_system_line(ctx, "Block list unavailable right now.");
        return;
    }

    session_ctx_t *target = chat_room_find_user(&ctx->owner->room, working);
    if (target == nullptr) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "User '%.256s' is not connected.",
                 working);
        session_send_system_line(ctx, message);
        return;
    }

    if (target == ctx) {
        session_send_system_line(ctx, "You do not need to block yourself.");
        return;
    }

    if (target->client_ip[0] == '\0') {
        session_send_system_line(
            ctx, "Unable to identify that user's IP address right now.");
        return;
    }

    char label[64];
    if (session_detect_provider_ip(target->client_ip, label, sizeof(label))) {
        memset(&ctx->block_pending, 0, sizeof(ctx->block_pending));
        ctx->block_pending.active = true;
        snprintf(ctx->block_pending.username,
                 sizeof(ctx->block_pending.username), "%s", target->user.name);
        snprintf(ctx->block_pending.ip, sizeof(ctx->block_pending.ip), "%s",
                 target->client_ip);
        snprintf(ctx->block_pending.provider_label,
                 sizeof(ctx->block_pending.provider_label), "%.31s", label);

        char warning[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(warning, sizeof(warning), "%.63s appears to belong to %.63s.",
                 target->client_ip, label);
        session_send_system_line(ctx, warning);

        char prompt[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(prompt, sizeof(prompt),
                 "Use /block confirm %.23s only to hide just [%.23s] or /block "
                 "confirm %.23s ip to hide everyone from that IP.",
                 target->user.name, target->user.name, target->user.name);
        session_send_system_line(ctx, prompt);
        return;
    }

    bool already_present = false;
    if (!session_blocklist_add(ctx, target->client_ip, target->user.name, true,
                               &already_present)) {
        if (already_present) {
            session_send_system_line(ctx, "That address is already blocked.");
        } else {
            session_send_system_line(
                ctx, "Unable to add block entry (limit reached?).");
        }
    } else {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message),
                 "Blocking all users from %.63s (triggered by [%.23s]).",
                 target->client_ip, target->user.name);
        session_send_system_line(ctx, message);
    }
}

static void session_handle_unblock(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    static const char *kUsage = "Usage: /unblock <username|ip|all>";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/unblock", kUsage, usage, sizeof(usage));

    if (arguments == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    if (strcasecmp(working, "all") == 0) {
        size_t removed = 0U;
        for (size_t idx = 0U; idx < SSH_CHATTER_MAX_BLOCKED; ++idx) {
            if (ctx->block_entries[idx].in_use) {
                memset(&ctx->block_entries[idx], 0,
                       sizeof(ctx->block_entries[idx]));
                ++removed;
            }
        }
        ctx->block_entry_count = 0U;
        ctx->block_pending.active = false;
        ctx->block_pending.username[0] = '\0';
        ctx->block_pending.ip[0] = '\0';
        ctx->block_pending.provider_label[0] = '\0';

        if (removed == 0U) {
            session_send_system_line(ctx, "No blocked entries to remove.");
        } else {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message), "Removed %zu blocked entr%s.",
                     removed, removed == 1U ? "y" : "ies");
            session_send_system_line(ctx, message);
        }
        return;
    }

    if (session_blocklist_remove(ctx, working)) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "Removed block for %.256s.",
                 working);
        session_send_system_line(ctx, message);
    } else {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "No block entry matched '%.256s'.",
                 working);
        session_send_system_line(ctx, message);
    }
}

static void session_handle_pm(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    static const char *kUsage = "Usage: /pm <username> <message>";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/pm", kUsage, usage, sizeof(usage));

    if (ctx->owner == nullptr) {
        session_send_system_line(ctx,
                                 "Private messages are unavailable right now.");
        return;
    }

    if (arguments == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    char *cursor = working;
    while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
        ++cursor;
    }

    if (*cursor == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    *cursor = '\0';
    char *message = cursor + 1;
    while (*message != '\0' && isspace((unsigned char)*message)) {
        ++message;
    }

    if (*message == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    char target_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(target_name, sizeof(target_name), "%.*s",
             (int)sizeof(target_name) - 1, working);

    session_ctx_t *target = chat_room_find_user(&ctx->owner->room, target_name);
    const bool target_is_eliza = strcasecmp(target_name, "eliza") == 0;
    const bool eliza_active =
        target_is_eliza && atomic_load(&ctx->owner->eliza_enabled);

    if (target == nullptr) {
        if (target_is_eliza) {
            if (!eliza_active) {
                session_send_system_line(ctx, "eliza isn't around right now.");
                return;
            }
            session_send_system_line(ctx, "Connecting you with eliza...");
        } else {
            char not_found[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(not_found, sizeof(not_found),
                     "User '%s' is not connected.", target_name);
            session_send_system_line(ctx, not_found);
            return;
        }
    }

    char prepared[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(prepared, sizeof(prepared), "%s", message);

    char stripped[SSH_CHATTER_MESSAGE_LIMIT];
    bool translation_bypass = translation_strip_no_translate_prefix(
        prepared, stripped, sizeof(stripped));
    const char *deliver_body = translation_bypass ? stripped : prepared;

    const char *target_display =
        target != nullptr ? target->user.name : target_name;
    printf("[pm] %s -> %s: %s\n", ctx->user.name, target_display, deliver_body);

    char to_target_label[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(to_target_label, sizeof(to_target_label), "%s -> you",
             ctx->user.name);

    char to_sender_label[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(to_sender_label, sizeof(to_sender_label), "you -> %s",
             target_display);

    bool attempt_translation = (target != nullptr) && !translation_bypass &&
                               ctx->translation_enabled &&
                               ctx->input_translation_enabled &&
                               ctx->input_translation_language[0] != '\0';

    if (attempt_translation) {
        if (session_translation_queue_private_message(ctx, target,
                                                      deliver_body)) {
            return;
        }
        session_send_system_line(
            ctx, "Translation unavailable; sending your original message.");
    }

    if (target != nullptr) {
        session_send_private_message_line(target, ctx, to_target_label,
                                          deliver_body);
        session_send_private_message_line(ctx, ctx, to_sender_label,
                                          deliver_body);
        return;
    }

    session_send_private_message_line(ctx, ctx, to_sender_label, deliver_body);
    host_eliza_handle_private_message(ctx, deliver_body);
}

static bool username_contains(const char *username, const char *needle)
{
    if (username == nullptr || needle == nullptr) {
        return false;
    }

    const size_t needle_len = strlen(needle);
    if (needle_len == 0U) {
        return false;
    }

    const size_t name_len = strlen(username);
    if (needle_len > name_len) {
        return false;
    }

    for (size_t offset = 0U; offset + needle_len <= name_len; ++offset) {
        bool match = true;
        for (size_t idx = 0U; idx < needle_len; ++idx) {
            const unsigned char user_ch = (unsigned char)username[offset + idx];
            const unsigned char needle_ch = (unsigned char)needle[idx];
            if (tolower(user_ch) != tolower(needle_ch)) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }

    return false;
}

static void session_handle_search(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (ctx->owner == nullptr) {
        session_send_system_line(ctx, "Search is unavailable at the moment.");
        return;
    }

    if (arguments == nullptr) {
        session_send_system_line(ctx, "Usage: /search <text>");
        return;
    }

    char query[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(query, sizeof(query), "%s", arguments);
    trim_whitespace_inplace(query);

    if (query[0] == '\0') {
        session_send_system_line(ctx, "Usage: /search <text>");
        return;
    }

    char listing[SSH_CHATTER_MESSAGE_LIMIT];
    listing[0] = '\0';
    size_t match_count = 0U;

    pthread_mutex_lock(&ctx->owner->room.lock);
    for (size_t idx = 0U; idx < ctx->owner->room.member_count; ++idx) {
        session_ctx_t *member = ctx->owner->room.members[idx];
        if (member == nullptr) {
            continue;
        }
        if (!username_contains(member->user.name, query)) {
            continue;
        }

        char name[SSH_CHATTER_USERNAME_LEN];
        snprintf(name, sizeof(name), "%s", member->user.name);
        size_t current_len = strnlen(listing, sizeof(listing));
        size_t name_len = strnlen(name, sizeof(name));
        size_t prefix_len = (match_count == 0U) ? 0U : 2U;

        if (current_len + prefix_len + name_len >= sizeof(listing)) {
            continue;
        }

        if (match_count > 0U) {
            listing[current_len++] = ',';
            listing[current_len++] = ' ';
        }
        memcpy(listing + current_len, name, name_len);
        listing[current_len + name_len] = '\0';
        ++match_count;
    }
    pthread_mutex_unlock(&ctx->owner->room.lock);

    if (match_count == 0U) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        char display_query[64];
        size_t copy_len = strnlen(query, sizeof(display_query) - 1U);
        memcpy(display_query, query, copy_len);
        display_query[copy_len] = '\0';
        snprintf(message, sizeof(message), "No users matching '%s'.",
                 display_query);
        session_send_system_line(ctx, message);
        return;
    }

    char header[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(header, sizeof(header), "Matching users (%zu):", match_count);
    session_send_system_line(ctx, header);
    session_send_system_line(ctx, listing);
}

static void session_handle_chat_lookup(session_ctx_t *ctx,
                                       const char *arguments)
{
    static const char *kUsage = "Usage: /chat <message-id>";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/chat", kUsage, usage, sizeof(usage));

    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (arguments == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    char working[64];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    uint64_t message_id = 0U;
    if (!host_compact_id_decode(working, &message_id) || message_id == 0U) {
        session_send_system_line(ctx, usage);
        return;
    }

    chat_history_entry_t entry = {0};
    if (!host_history_find_entry_by_id(ctx->owner, message_id, &entry)) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        char label[32];
        if (!host_compact_id_encode(message_id, label, sizeof(label))) {
            snprintf(label, sizeof(label), "%" PRIu64, message_id);
        }
        snprintf(message, sizeof(message), "Message #%s was not found.",
                 label);
        session_send_system_line(ctx, message);
        return;
    }

    char header[SSH_CHATTER_MESSAGE_LIMIT];
    char header_label[32];
    if (!host_compact_id_encode(message_id, header_label,
                                sizeof(header_label))) {
        snprintf(header_label, sizeof(header_label), "%" PRIu64, message_id);
    }
    snprintf(header, sizeof(header), "Message #%s:", header_label);
    session_send_system_line(ctx, header);
    session_send_history_entry(ctx, &entry);
    session_send_reply_tree(ctx, entry.message_id, 0U, 1U);
}

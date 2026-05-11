/**
 * @file host_core.c
 * @desc File-level documentation for host_core.c, describing its role
 *       in the SSH-Chatter server and providing a consistent header
 *       comment format across C sources.
 * @return None.
 */

#include "../host_internal.h"

static bool host_is_system_reserved_username(host_t *host, const char *username)
{
    if (host == nullptr || username == nullptr || username[0] == '\0') {
        return false;
    }

    // List of system-reserved usernames that even authenticated users cannot use
    static const char *const system_reserved_names[] = {
        "Guest",   "Admin",       "Operator", "System", "Root", "Moderator",
        "Chatter", "ssh-chatter", "bot",      "Bot",    "BOT",
        "kaka",    "dada",
    };
    static const size_t system_reserved_names_count =
        sizeof(system_reserved_names) / sizeof(system_reserved_names[0]);

    for (size_t i = 0; i < system_reserved_names_count; ++i) {
        if (strcasecmp(username, system_reserved_names[i]) == 0) {
            return true;
        }
    }

    if (host->ai_persona_a_name[0] != '\0' &&
        strcasecmp(username, host->ai_persona_a_name) == 0) {
        return true;
    }
    if (host->ai_persona_b_name[0] != '\0' &&
        strcasecmp(username, host->ai_persona_b_name) == 0) {
        return true;
    }
    return false;
}

static void session_dispatch_command(session_ctx_t *ctx, const char *line);
static void session_handle_mode(session_ctx_t *ctx, const char *arguments);
static void session_handle_nick(session_ctx_t *ctx, const char *arguments);
static void session_handle_exit(session_ctx_t *ctx);

static const session_ops_t ssh_session_ops = {
    .dispatch_command = session_dispatch_command,
    .handle_mode = session_handle_mode,
    .handle_nick = session_handle_nick,
    .handle_exit = session_handle_exit,
};

static const session_ops_t telnet_session_ops = {
    .dispatch_command = session_dispatch_command,
    .handle_mode = session_handle_mode,
    .handle_nick = session_handle_nick,
    .handle_exit = session_handle_exit,
};

#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif

#ifndef RTLD_LOCAL
#define RTLD_LOCAL 0
#endif

#define ANSI_CLEAR_LINE "\033[2K"
#define ANSI_INSERT_LINE "\033[1L"

#define SSH_CHATTER_MESSAGE_BOX_MAX_LINES 32U
#define SSH_CHATTER_MESSAGE_BOX_PADDING 2U
#define SSH_CHATTER_IMAGE_PREVIEW_WIDTH 48U
#define SSH_CHATTER_IMAGE_PREVIEW_HEIGHT 48U
#define SSH_CHATTER_IMAGE_PREVIEW_LINE_LEN 128U
#define SSH_CHATTER_BBS_DEFAULT_TAG "general"
#define SSH_CHATTER_ASCIIART_TERMINATOR_EN ">/__ARTWORK_END>"
#define SSH_CHATTER_BBS_TERMINATOR_EN ">/__BBS_END>"
#define SSH_CHATTER_RSS_REFRESH_SECONDS 300U
#define SSH_CHATTER_RSS_SLEEP_CHUNK_SECONDS 5U
#define SSH_CHATTER_RSS_DOWNLOAD_ATTEMPTS 10U
#define SSH_CHATTER_RSS_CONNECT_TIMEOUT_MS 5000L
#define SSH_CHATTER_RSS_TRANSFER_TIMEOUT_MS 10000L
#define SSH_CHATTER_RSS_USER_AGENT "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
#define SSH_CHATTER_RSS_BREAKING_PREFIX "[BREAKING NEWS]"
#define SSH_CHATTER_TETROMINO_SIZE 4
#define SSH_CHATTER_HANDSHAKE_RETRY_LIMIT 2U
#define SSH_CHATTER_REQUIRED_HOSTKEY_ALGORITHMS_DISPLAY                        \
    "rsa-sha2-512, rsa-sha2-256, ssh-rsa, ssh-ed25519, ecdsa-sha2-nistp256"
// KEX algorithms matching modern openssh-server defaults
#define SSH_CHATTER_SUPPORTED_KEX_ALGORITHMS                                   \
    "curve25519-sha256,curve25519-sha256@libssh.org,"                          \
    "ecdh-sha2-nistp256,ecdh-sha2-nistp384,ecdh-sha2-nistp521,"               \
    "diffie-hellman-group-exchange-sha256,"                                    \
    "diffie-hellman-group16-sha512,diffie-hellman-group18-sha512,"             \
    "diffie-hellman-group14-sha256,diffie-hellman-group14-sha1"
// Ciphers matching modern openssh-server defaults (strongest first)
#define SSH_CHATTER_STRONG_CIPHERS                                             \
    "chacha20-poly1305@openssh.com,"                                           \
    "aes256-gcm@openssh.com,aes128-gcm@openssh.com,"                          \
    "aes256-ctr,aes192-ctr,aes128-ctr"
// MACs matching modern openssh-server defaults (ETM variants preferred)
#define SSH_CHATTER_STRONG_MACS                                                \
    "hmac-sha2-256-etm@openssh.com,hmac-sha2-512-etm@openssh.com,"            \
    "hmac-sha2-256,hmac-sha2-512"
#define SSH_CHATTER_SECURE_COMPRESSION "none"
#define SSH_CHATTER_BIRTHDAY_WINDOW_SECONDS (7 * 24 * 60 * 60)

#define ALPHA_LANDERS_MAX_RECORDS 256U
#define ALPHA_LANDERS_DISPLAY_LIMIT 10U

typedef struct alpha_lander_entry {
    char username[SSH_CHATTER_USERNAME_LEN];
    uint32_t flag_count;
    uint64_t last_flag_timestamp;
} alpha_lander_entry_t;

static int alpha_lander_entry_compare(const void *lhs, const void *rhs)
{
    const alpha_lander_entry_t *left = (const alpha_lander_entry_t *)lhs;
    const alpha_lander_entry_t *right = (const alpha_lander_entry_t *)rhs;

    if (left->flag_count < right->flag_count) {
        return 1;
    }
    if (left->flag_count > right->flag_count) {
        return -1;
    }
    if (left->last_flag_timestamp < right->last_flag_timestamp) {
        return 1;
    }
    if (left->last_flag_timestamp > right->last_flag_timestamp) {
        return -1;
    }
    return strcasecmp(left->username, right->username);
}

static const char *const SSH_CHATTER_REQUIRED_HOSTKEY_ALGORITHMS[] = {
    "rsa-sha2-512", "rsa-sha2-256",        "ssh-rsa",
    "ssh-ed25519",  "ecdsa-sha2-nistp256",
};

typedef struct host_key_definition {
    const char *algorithm;
    const char *filename;
    enum ssh_bind_options_e option;
    bool requires_import;
} host_key_definition_t;

static const size_t SSH_CHATTER_REQUIRED_HOSTKEY_ALGORITHMS_COUNT =
    sizeof(SSH_CHATTER_REQUIRED_HOSTKEY_ALGORITHMS) /
    sizeof(SSH_CHATTER_REQUIRED_HOSTKEY_ALGORITHMS[0]);
#define SESSION_CHANNEL_TIMEOUT (-2)

typedef struct lan_operator_env_pair {
    const char *name_var;
    const char *password_var;
} lan_operator_env_pair_t;

static const lan_operator_env_pair_t LAN_OPERATOR_ENV_PAIRS[] = {
    {"ADMIN1", "ADMIN1PW"}, {"ADMIN2", "ADMIN2PW"}, {"ADMIN3", "ADMIN3PW"},
    {"ADMIN4", "ADMIN4PW"}, {"BACKUP", "BACKUPPW"},
};

static const size_t LAN_OPERATOR_ENV_PAIR_COUNT =
    sizeof(LAN_OPERATOR_ENV_PAIRS) / sizeof(LAN_OPERATOR_ENV_PAIRS[0]);

static void host_sleep_uninterruptible(const struct timespec *duration)
{
    if (duration == nullptr) {
        return;
    }

    struct timespec request = *duration;
    struct timespec remaining = {0};

    while (nanosleep(&request, &remaining) != 0) {
        if (errno != EINTR) {
            break;
        }
        request = remaining;
    }
}

static bool host_address_is_wildcard(const char *address)
{
    if (address == nullptr) {
        return true;
    }

    if (address[0] == '\0') {
        return true;
    }

    if (strcmp(address, "*") == 0 || strcmp(address, "0.0.0.0") == 0 ||
        strcmp(address, "::") == 0 || strcmp(address, "::0") == 0) {
        return true;
    }

    bool all_zero = true;
    for (const char *cursor = address; *cursor != '\0'; ++cursor) {
        if (*cursor == ':' || *cursor == '.') {
            continue;
        }
        if (*cursor != '0') {
            all_zero = false;
            break;
        }
    }

    return all_zero;
}

static bool host_is_protected_ip_unlocked(const host_t *host, const char *ip)
{
    if (host == nullptr || ip == nullptr || ip[0] == '\0') {
        return false;
    }

    for (size_t idx = 0;
         idx < host->protected_ip_count && idx < SSH_CHATTER_MAX_PROTECTED_IPS;
         ++idx) {
        if (strncmp(host->protected_ips[idx], ip, SSH_CHATTER_IP_LEN) == 0) {
            return true;
        }
    }

    return false;
}

static bool host_protected_ip_add_unlocked(host_t *host, const char *ip)
{
    if (host == nullptr || ip == nullptr) {
        return false;
    }

    char normalized[SSH_CHATTER_IP_LEN];
    size_t length = strnlen(ip, sizeof(normalized));
    size_t start = 0U;
    while (start < length && isspace((unsigned char)ip[start]) != 0) {
        ++start;
    }
    size_t end = length;
    while (end > start && isspace((unsigned char)ip[end - 1U]) != 0) {
        --end;
    }

    if (end <= start) {
        return false;
    }

    size_t normalized_length = end - start;
    if (normalized_length >= sizeof(normalized)) {
        normalized_length = sizeof(normalized) - 1U;
    }
    memcpy(normalized, ip + start, normalized_length);
    normalized[normalized_length] = '\0';

    if (host_address_is_wildcard(normalized)) {
        return false;
    }

    if (host_is_protected_ip_unlocked(host, normalized)) {
        return true;
    }

    if (host->protected_ip_count >= SSH_CHATTER_MAX_PROTECTED_IPS) {
        return false;
    }

    snprintf(host->protected_ips[host->protected_ip_count], SSH_CHATTER_IP_LEN,
             "%s", normalized);
    ++host->protected_ip_count;
    return true;
}

static bool host_protected_ip_add(host_t *host, const char *ip)
{
    if (host == nullptr || ip == nullptr) {
        return false;
    }

    bool added = false;
    ttak_mutex_lock(&host->lock);
    added = host_protected_ip_add_unlocked(host, ip);
    ttak_mutex_unlock(&host->lock);
    return added;
}

static void host_protected_ips_load_from_env(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    const char *env = getenv("CHATTER_PROTECTED_IPS");
    if (env == nullptr || env[0] == '\0') {
        return;
    }

    size_t env_length = strlen(env);
    char *copy = (char *)sshc_gc_malloc(env_length + 1U);
    if (copy == nullptr) {
        humanized_log_error("host", "failed to allocate protected ip buffer",
                            errno != 0 ? errno : ENOMEM);
        return;
    }
    memcpy(copy, env, env_length + 1U);

    char *save_ptr = nullptr;
    for (char *token = strtok_r(copy, ",", &save_ptr); token != nullptr;
         token = strtok_r(nullptr, ",", &save_ptr)) {
        char working[SSH_CHATTER_IP_LEN];
        size_t token_length = strnlen(token, sizeof(working));
        if (token_length >= sizeof(working)) {
            token_length = sizeof(working) - 1U;
        }
        memcpy(working, token, token_length);
        working[token_length] = '\0';

        // Trim leading and trailing whitespace inside the buffer before adding it.
        size_t local_length = strnlen(working, sizeof(working));
        size_t local_start = 0U;
        while (local_start < local_length &&
               isspace((unsigned char)working[local_start]) != 0) {
            ++local_start;
        }
        size_t local_end = local_length;
        while (local_end > local_start &&
               isspace((unsigned char)working[local_end - 1U]) != 0) {
            --local_end;
        }
        if (local_end <= local_start) {
            continue;
        }
        size_t trimmed_length = local_end - local_start;
        if (trimmed_length >= sizeof(working)) {
            trimmed_length = sizeof(working) - 1U;
        }
        memmove(working, working + local_start, trimmed_length);
        working[trimmed_length] = '\0';

        if (working[0] == '\0') {
            continue;
        }

        (void)host_protected_ip_add(host, working);
    }

    sshc_gc_free(copy);
}

static void host_protected_ips_bootstrap(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    const char *defaults[] = {"127.0.0.1", "::1", "192.168.0.1"};

    ttak_mutex_lock(&host->lock);
    for (size_t idx = 0; idx < sizeof(defaults) / sizeof(defaults[0]); ++idx) {
        (void)host_protected_ip_add_unlocked(host, defaults[idx]);
    }
    ttak_mutex_unlock(&host->lock);

    host_protected_ips_load_from_env(host);
}

static void host_register_protected_bind_address(host_t *host,
                                                 const char *address)
{
    if (host == nullptr || address == nullptr || address[0] == '\0') {
        return;
    }

    if (host_address_is_wildcard(address)) {
        return;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo *result = nullptr;
    int rc = getaddrinfo(address, nullptr, &hints, &result);
    if (rc != 0 || result == nullptr) {
        (void)host_protected_ip_add(host, address);
        if (result != nullptr) {
            freeaddrinfo(result);
        }
        return;
    }

    for (struct addrinfo *entry = result; entry != nullptr;
         entry = entry->ai_next) {
        char ip_buffer[SSH_CHATTER_IP_LEN];
        void *addr_ptr = nullptr;
        int family = entry->ai_family;
        if (family == AF_INET) {
            struct sockaddr_in *in4 = (struct sockaddr_in *)entry->ai_addr;
            addr_ptr = &in4->sin_addr;
        } else if (family == AF_INET6) {
            struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)entry->ai_addr;
            addr_ptr = &in6->sin6_addr;
        } else {
            continue;
        }

        if (inet_ntop(family, addr_ptr, ip_buffer, sizeof(ip_buffer)) ==
            nullptr) {
            continue;
        }

        (void)host_protected_ip_add(host, ip_buffer);
    }

    freeaddrinfo(result);
}

static void host_clear_lan_operator_credentials(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    host->lan_ops.count = 0U;
    memset(host->lan_ops.entries, 0, sizeof(host->lan_ops.entries));
}

lan_operator_credential_t *
host_find_lan_operator_credential(host_t *host, const char *username)
{
    if (host == nullptr || username == nullptr || username[0] == '\0') {
        return nullptr;
    }

    size_t limit = host->lan_ops.count;
    if (limit > SSH_CHATTER_MAX_LAN_OPERATORS) {
        limit = SSH_CHATTER_MAX_LAN_OPERATORS;
    }

    for (size_t idx = 0U; idx < limit; ++idx) {
        lan_operator_credential_t *credential = &host->lan_ops.entries[idx];
        if (!credential->active) {
            continue;
        }
        if (strcasecmp(credential->nickname, username) == 0) {
            return credential;
        }
    }

    return nullptr;
}

static bool host_is_lan_operator_username(host_t *host, const char *username)
{
    return host_find_lan_operator_credential(host, username) != nullptr;
}

static void host_load_lan_operator_credentials(host_t *host)
{
    host_clear_lan_operator_credentials(host);
    if (host == nullptr) {
        return;
    }

    for (size_t idx = 0U; idx < LAN_OPERATOR_ENV_PAIR_COUNT; ++idx) {
        const lan_operator_env_pair_t *pair = &LAN_OPERATOR_ENV_PAIRS[idx];
        const char *username = getenv(pair->name_var);
        const char *password = getenv(pair->password_var);
        if (username == nullptr || username[0] == '\0') {
            continue;
        }
        if (password == nullptr || password[0] == '\0') {
            continue;
        }

        lan_operator_credential_t *existing =
            host_find_lan_operator_credential(host, username);
        if (existing != nullptr) {
            existing->active = true;
            snprintf(existing->nickname, sizeof(existing->nickname), "%s",
                     username);
            snprintf(existing->password, sizeof(existing->password), "%s",
                     password);
            continue;
        }

        if (host->lan_ops.count >= SSH_CHATTER_MAX_LAN_OPERATORS) {
            continue;
        }

        lan_operator_credential_t *credential =
            &host->lan_ops.entries[host->lan_ops.count++];
        memset(credential, 0, sizeof(*credential));
        credential->active = true;
        snprintf(credential->nickname, sizeof(credential->nickname), "%s",
                 username);
        snprintf(credential->password, sizeof(credential->password), "%s",
                 password);
    }
}

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
    };
    static const size_t system_reserved_names_count =
        sizeof(system_reserved_names) / sizeof(system_reserved_names[0]);

    for (size_t i = 0; i < system_reserved_names_count; ++i) {
        if (strcasecmp(username, system_reserved_names[i]) == 0) {
            return true;
        }
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

typedef enum host_join_attempt_result {
    HOST_JOIN_ATTEMPT_OK = 0,
    HOST_JOIN_ATTEMPT_KICK,
    HOST_JOIN_ATTEMPT_BAN,
} host_join_attempt_result_t;
#define HOST_MODERATION_CATEGORY_LEN 64U
#define HOST_MODERATION_SNIPPET_LEN 1024U
#define HOST_MODERATION_WORKER_EXIT_CODE 0
#define HOST_MODERATION_WORKER_STABLE_SECONDS 30.0
#define HOST_MODERATION_MAX_RESTART_ATTEMPTS 5U

typedef struct host_moderation_task {
    struct host_moderation_task *next;
    uint64_t task_id;
    char username[SSH_CHATTER_USERNAME_LEN];
    char client_ip[SSH_CHATTER_IP_LEN];
    char category[HOST_MODERATION_CATEGORY_LEN];
    char snippet[HOST_MODERATION_SNIPPET_LEN];
    size_t snippet_length;
    char message[SSH_CHATTER_MESSAGE_LIMIT];
    bool post_send;
} host_moderation_task_t;

typedef struct {
    uint64_t task_id;
    uint32_t category_length;
    uint32_t content_length;
} host_moderation_ipc_request_t;

typedef struct {
    uint64_t task_id;
    int32_t result;
    uint32_t message_length;
    uint32_t disable_filter;
} host_moderation_ipc_response_t;
#define SSH_CHATTER_CHANNEL_RECOVERY_LIMIT ((unsigned int)INT_MAX)
#define SSH_CHATTER_CHANNEL_RECOVERY_DELAY_NS 200000000L
#define SSH_CHATTER_CHANNEL_WRITE_TIMEOUT_MS 200
#define SSH_CHATTER_CHANNEL_WRITE_MAX_STALLS 30U
#define SSH_CHATTER_CHANNEL_WRITE_CHUNK 1024U
#define SSH_CHATTER_CHANNEL_WRITE_BACKOFF_NS 20000000L
#define SSH_CHATTER_TRANSLATION_SEGMENT_GUARD 32U
#define SSH_CHATTER_TRANSLATION_BATCH_DELAY_NS 150000000L
#define SSH_CHATTER_JOIN_RAPID_WINDOW_NS 60000000000LL
#define SSH_CHATTER_JOIN_IP_THRESHOLD 6U
#define SSH_CHATTER_JOIN_NAME_THRESHOLD 6U
#define SSH_CHATTER_JOIN_KICK_WINDOW_NS 60000000000LL
#define SSH_CHATTER_JOIN_KICK_THRESHOLD 20U
#define SSH_CHATTER_JOIN_ACTIVITY_RETENTION_NS (300LL * 1000000000LL)
#define SSH_CHATTER_CONNECTION_GUARD_WINDOW_NS 5000000000LL
#define SSH_CHATTER_CONNECTION_GUARD_THRESHOLD 8U
#define SSH_CHATTER_CONNECTION_GUARD_BLOCK_BASE_NS 2000000000LL
#define SSH_CHATTER_CONNECTION_GUARD_BLOCK_STEP_NS 2000000000LL
#define SSH_CHATTER_CONNECTION_GUARD_BLOCK_MAX_NS 60000000000LL
#define SSH_CHATTER_CONNECTION_GUARD_RETENTION_NS 300000000000LL
#define SSH_CHATTER_CONNECTION_GUARD_BAN_THRESHOLD 5U
#define SSH_CHATTER_ERROR_BACKOFF_BASE_NS 1000000000LL
#define SSH_CHATTER_ERROR_BACKOFF_MAX_NS 15000000000LL
#define SSH_CHATTER_ERROR_BACKOFF_STABLE_NS 10000000000LL
#define SSH_CHATTER_SUSPICIOUS_EVENT_WINDOW_NS 300000000000LL
#define SSH_CHATTER_SUSPICIOUS_EVENT_THRESHOLD 2U
#define SSH_CHATTER_BBS_WATCHDOG_SLEEP_SECONDS 5U
#define SSH_CHATTER_BBS_REVIEW_INTERVAL_SECONDS 120U
#define ELIZA_MEMORY_MAGIC 0x454C5A41U
#define ELIZA_MEMORY_VERSION 1U
#define SSH_CHATTER_ELIZA_CONTEXT_LIMIT 3U
#define SSH_CHATTER_ELIZA_CONTEXT_BUFFER (SSH_CHATTER_MESSAGE_LIMIT * 4U)
#define SSH_CHATTER_ELIZA_HISTORY_LIMIT 6U
#define SSH_CHATTER_ELIZA_HISTORY_WINDOW 12U
#define SSH_CHATTER_ELIZA_BBS_CONTEXT_LIMIT 3U
#define SSH_CHATTER_ELIZA_BBS_PREVIEW_LEN 160U
#define SSH_CHATTER_ELIZA_PROMPT_BUFFER                                        \
    ((SSH_CHATTER_ELIZA_CONTEXT_BUFFER * 2U) + (SSH_CHATTER_MESSAGE_LIMIT * 3U))
#define SSH_CHATTER_ELIZA_TOKEN_LIMIT 16U

static size_t host_column_reset_sequence_length(const char *text);
static void host_strip_column_reset(char *text);

#define ALPHA_TOTAL_DISTANCE_LY 4.24
#define ALPHA_LY_TO_KM 9460730472580.8
#define ALPHA_LY_TO_AU 63241.077
#define ALPHA_SPEED_OF_LIGHT_MPS 299792458.0
#define ALPHA_NAV_WIDTH 42
#define ALPHA_NAV_HEIGHT 42
#define ALPHA_NAV_MARGIN 6
#define ALPHA_THRUST_DELTA 0.45
#define ALPHA_THRUST_POSITION_STEP 0.5
#define ALPHA_GRAVITY_DAMPING 0.97
#define ALPHA_GRAVITY_MIN_DISTANCE 2.5
#define ALPHA_GRAVITY_MAX_ACCEL 0.30
#define ALPHA_NAV_MAX_SPEED 1.20
#define ALPHA_BLACK_HOLE_MU 1800.0
#define ALPHA_STAR_MU 360.0
#define ALPHA_PLANET_MU 65.0
#define ALPHA_DEBRIS_MU 12.0
#define ALPHA_MIN_WAYPOINTS 3U

static const char *const kAlphaWaystationNames[] = {
    "Relay Lyra",
    "Depot Carina",
    "Refuel Vesper",
    "Outpost Helion",
};

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#define VERSION_IP_RULE_SEPARATOR ",;\n"

typedef struct version_ip_ban_seed {
    const char *pattern;
    const char *cidr;
    const char *note;
} version_ip_ban_seed_t;

static const version_ip_ban_seed_t kVersionIpBanSeeds[] = {
    {"SSH-2.0-Go*", "34.80.0.0/12", ""},
    {"SSH-2.0-Go*", "34.96.0.0/14", ""},
    {"SSH-2.0-paramiko_*", "52.78.0.0/15", ""},
    {"SSH-2.0-AsyncSSH_*", "20.214.0.0/15", ""},
    {"SSH-2.0-libssh_0.4*", "0.0.0.0/0", ""},
    {"SSH-2.0-libssh_0.5*", "0.0.0.0/0", ""},
    {"SSH-2.0-libssh_0.6*", "0.0.0.0/0", ""},
    {"SSH-2.0-libssh_0.7*", "0.0.0.0/0", ""},
    {"SSH-2.0-libssh_0.8*", "0.0.0.0/0", ""},
    {"SSH-2.0-libssh_0.9*", "0.0.0.0/0", ""},
    {"SSH-2.0-paramiko*", "3.0.0.0/8", ""},
    {"SSH-2.0-paramiko_1.*", "0.0.0.0/0", ""},
    {"SSH-2.0-paramiko_2.*", "44.0.0.0/8", ""},
    {"SSH-2.0-JSch*", "35.0.0.0/8", ""},
    {"SSH-2.0-JSch_0.1.55", "0.0.0.0/0", ""},
    {"SSH-2.0-JSch_0.1.53", "0.0.0.0/0", ""},
    {"SSH-2.0-JSch_0.1.54", "0.0.0.0/0", ""},
    {"SSH-2.0-PuTTY*", "23.0.0.0/8", ""},
    {"SSH-2.0-PuTTY_Release_0.6*", "52.0.0.0/8", ""},
    {"SSH-2.0-PuTTY_Release_0.7*", "104.0.0.0/8", ""},
    {"SSH-2.0-masscan*", "0.0.0.0/0", ""},
    {"SSH-2.0-Nmap*", "0.0.0.0/0", ""},
    {"SSH-2.0-Go-SSH*", "0.0.0.0/0", ""},
    {"SSH-2.0-Python-requests*", "0.0.0.0/0", ""},
    {"SSH-2.0-PHPSecLib*", "0.0.0.0/0", ""},
    {"SSH-2.0-Dropbear*", "0.0.0.0/0", ""},
    {"SSH-2.0-BlackRose*", "0.0.0.0/0", ""},
    {"SSH-2.0-PumaBot*", "0.0.0.0/0", ""},
    {"SSH-2.0-SSH-Client-v1.*", "0.0.0.0/0", ""},
    {"SSH-2.0-sshj*", "0.0.0.0/0", ""},
    {"SSH-2.0-Java*", "13.0.0.0/8", ""},
    {"SSH-2.0-Python*", "104.0.0.0/8", ""},
    {"SSH-2.0-Curl*", "172.0.0.0/8", ""},
    {"SSH-2.0-Ruby*", "18.0.0.0/8", ""},
    {"SSH-2.0-SSH*", "134.0.0.0/8", ""},
    {"SSH-1.99*", "0.0.0.0/0", ""},
    {"", "0.0.0.0/0", ""},
    {"SSH-2.0-Go*", "147.0.0.0/8", ""},
    {"SSH-2.0-Python*", "13.0.0.0/8", ""},
    {"SSH-2.0-libssh*", "192.0.0.0/8", ""},
    {"SSH-2.0-paramiko*", "172.0.0.0/8", ""},
    {"SSH-2.0-JSch*", "10.0.0.0/8", ""},
    {"SSH-2.0-Curl*", "103.0.0.0/8", ""},
    {"SSH-2.0-Go*", "167.0.0.0/8", ""},
    {"SSH-2.0-paramiko*", "69.0.0.0/8", ""},
    {"SSH-2.0-libssh*", "173.0.0.0/8", ""},
    {"SSH-2.0-JSch*", "216.0.0.0/8", ""},
    {"SSH-2.0-Python*", "185.0.0.0/8", ""},
    {"SSH-2.0-SSH-2.0-libssh*", "0.0.0.0/0", ""},
    {"SSH-2.0-paramiko_paramiko*", "0.0.0.0/0", ""},
    {"SSH-2.0-Jakarta*", "0.0.0.0/0", ""},
    {"SSH-2.0-ssh.net*", "0.0.0.0/0", ""},
    {"SSH-2.0-Mina*", "0.0.0.0/0", ""},
    {"SSH-2.0-Tectia*", "0.0.0.0/0", ""},
    {"SSH-2.0-Ganymed*", "0.0.0.0/0", ""},
    {"SSH-2.0-NSSHS*", "0.0.0.0/0", ""},
    {"SSH-2.0-F-Secure*", "0.0.0.0/0", ""},
    {"SSH-2.0-SecureCRT*", "0.0.0.0/0", ""},
    {"SSH-2.0-Unknown*", "0.0.0.0/0", ""},
    {"SSH-2.0-SshTunnel*", "0.0.0.0/0", ""},
    {"SSH-2.0-paramiko*", "199.0.0.0/8", ""},
    {"SSH-2.0-paramiko*", "207.0.0.0/8", ""},
    {"SSH-2.0-paramiko*", "85.0.0.0/8", ""},
    {"SSH-2.0-Python*", "199.0.0.0/8", ""},
    {"SSH-2.0-Go*", "199.0.0.0/8", ""},
    {"SSH-2.0-Java*", "199.0.0.0/8", ""},
    {"SSH-2.0-Go*", "85.0.0.0/8", ""},
    {"SSH-2.0-Python*", "85.0.0.0/8", ""},
};
static char *host_trim_whitespace(char *text)
{
    if (text == nullptr) {
        return nullptr;
    }

    while (*text != '\0' && isspace((unsigned char)*text)) {
        ++text;
    }

    if (*text == '\0') {
        return text;
    }

    char *end = text + strlen(text) - 1U;
    while (end > text && isspace((unsigned char)*end)) {
        *end = '\0';
        --end;
    }

    return text;
}

static bool host_version_ip_parse_pattern(const char *pattern, char *normalized,
                                          size_t normalized_len, char *original,
                                          size_t original_len,
                                          version_pattern_match_t *mode)
{
    if (normalized == nullptr || original == nullptr || mode == nullptr) {
        return false;
    }

    char working[SSH_CHATTER_VERSION_PATTERN_LEN];
    int written = snprintf(working, sizeof(working), "%s",
                           pattern != nullptr ? pattern : "");
    if (written < 0 || (size_t)written >= sizeof(working)) {
        return false;
    }

    char *trimmed = host_trim_whitespace(working);
    if (trimmed == nullptr) {
        return false;
    }

    char trimmed_copy[SSH_CHATTER_VERSION_PATTERN_LEN];
    int copy_written =
        snprintf(trimmed_copy, sizeof(trimmed_copy), "%s", trimmed);
    if (copy_written < 0 || (size_t)copy_written >= sizeof(trimmed_copy)) {
        return false;
    }

    if (trimmed[0] == '\0') {
        return false;
    }

    size_t length = strlen(trimmed);
    bool leading_star = trimmed[0] == '*';
    bool trailing_star = (length > 0U && trimmed[length - 1U] == '*');

    if (leading_star && trailing_star && length == 1U) {
        if (original_len > 0U) {
            size_t orig_len = (size_t)copy_written;
            if (orig_len >= original_len) {
                orig_len = original_len - 1U;
            }
            memcpy(original, trimmed_copy, orig_len);
            original[orig_len] = '\0';
        }
        if (normalized_len > 0U) {
            normalized[0] = '\0';
        }
        *mode = VERSION_PATTERN_MATCH_ANY;
        return true;
    }

    if (trailing_star) {
        trimmed[length - 1U] = '\0';
        --length;
    }

    if (leading_star) {
        ++trimmed;
        length = strlen(trimmed);
    }

    if (length == 0U) {
        if (original_len > 0U) {
            size_t orig_len = (size_t)copy_written;
            if (orig_len >= original_len) {
                orig_len = original_len - 1U;
            }
            memcpy(original, trimmed_copy, orig_len);
            original[orig_len] = '\0';
        }
        if (normalized_len > 0U) {
            normalized[0] = '\0';
        }
        *mode = VERSION_PATTERN_MATCH_ANY;
        return true;
    }

    for (size_t idx = 0U; idx < length; ++idx) {
        if (trimmed[idx] == '*') {
            return false;
        }
    }

    if (normalized_len == 0U) {
        return false;
    }

    size_t normalized_length = length;
    if (normalized_length >= normalized_len) {
        normalized_length = normalized_len - 1U;
    }
    memcpy(normalized, trimmed, normalized_length);
    normalized[normalized_length] = '\0';

    if (original_len > 0U) {
        size_t orig_len = (size_t)copy_written;
        if (orig_len >= original_len) {
            orig_len = original_len - 1U;
        }
        memcpy(original, trimmed_copy, orig_len);
        original[orig_len] = '\0';
    }

    if (leading_star && trailing_star) {
        *mode = VERSION_PATTERN_MATCH_SUBSTRING;
    } else if (leading_star) {
        *mode = VERSION_PATTERN_MATCH_SUFFIX;
    } else if (trailing_star) {
        *mode = VERSION_PATTERN_MATCH_PREFIX;
    } else {
        *mode = VERSION_PATTERN_MATCH_EXACT;
    }

    return true;
}

static bool host_parse_ipv4_cidr(const char *cidr, uint32_t *network_out,
                                 uint32_t *mask_out)
{
    if (cidr == nullptr) {
        return false;
    }

    char buffer[SSH_CHATTER_CIDR_TEXT_LEN];
    int written = snprintf(buffer, sizeof(buffer), "%s", cidr);
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        return false;
    }

    char *slash = strchr(buffer, '/');
    if (slash == nullptr) {
        return false;
    }
    *slash = '\0';

    char *prefix_str = slash + 1;
    char *prefix_end = nullptr;
    long prefix_long = strtol(prefix_str, &prefix_end, 10);
    if (prefix_str == prefix_end || prefix_long < 0L || prefix_long > 32L) {
        return false;
    }

    struct in_addr address = {0};
    if (inet_pton(AF_INET, buffer, &address) != 1) {
        return false;
    }

    uint32_t prefix = (uint32_t)prefix_long;
    uint32_t mask =
        prefix == 0U ? 0U : (uint32_t)(0xFFFFFFFFu << (32U - prefix));
    uint32_t network = ntohl(address.s_addr) & mask;

    if (network_out != nullptr) {
        *network_out = network;
    }
    if (mask_out != nullptr) {
        *mask_out = mask;
    }

    return true;
}

static bool host_parse_ipv6_cidr(const char *cidr, struct in6_addr *network_out,
                                 struct in6_addr *mask_out)
{
    if (cidr == nullptr) {
        return false;
    }

    char buffer[SSH_CHATTER_CIDR_TEXT_LEN];
    int written = snprintf(buffer, sizeof(buffer), "%s", cidr);
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        return false;
    }

    char *slash = strchr(buffer, '/');
    if (slash == nullptr) {
        return false;
    }
    *slash = '\0';

    char *prefix_str = slash + 1;
    char *prefix_end = nullptr;
    long prefix_long = strtol(prefix_str, &prefix_end, 10);
    if (prefix_str == prefix_end || prefix_long < 0L || prefix_long > 128L) {
        return false;
    }

    struct in6_addr address;
    memset(&address, 0, sizeof(address));
    if (inet_pton(AF_INET6, buffer, &address) != 1) {
        return false;
    }

    struct in6_addr mask;
    memset(&mask, 0, sizeof(mask));
    int remaining = (int)prefix_long;
    for (size_t idx = 0U; idx < sizeof(mask.s6_addr); ++idx) {
        uint8_t value = 0U;
        if (remaining >= 8) {
            value = 0xFFU;
            remaining -= 8;
        } else if (remaining > 0) {
            value = (uint8_t)((uint32_t)0xFFU << (unsigned int)(8 - remaining));
            remaining = 0;
        }
        mask.s6_addr[idx] = value;
    }

    struct in6_addr network;
    memset(&network, 0, sizeof(network));
    for (size_t idx = 0U; idx < sizeof(network.s6_addr); ++idx) {
        network.s6_addr[idx] = address.s6_addr[idx] & mask.s6_addr[idx];
    }

    if (network_out != nullptr) {
        *network_out = network;
    }
    if (mask_out != nullptr) {
        *mask_out = mask;
    }

    return true;
}

static bool host_cidr_contains_ip(const char *cidr_text, const char *ip)
{
    if (cidr_text == nullptr || ip == nullptr || ip[0] == '\0') {
        return false;
    }

    uint32_t ipv4_network = 0U;
    uint32_t ipv4_mask = 0U;
    if (host_parse_ipv4_cidr(cidr_text, &ipv4_network, &ipv4_mask)) {
        struct in_addr address = {0};
        if (inet_pton(AF_INET, ip, &address) != 1) {
            return false;
        }

        uint32_t ip_value = ntohl(address.s_addr);
        return (ip_value & ipv4_mask) == ipv4_network;
    }

    struct in6_addr ipv6_network;
    struct in6_addr ipv6_mask;
    memset(&ipv6_network, 0, sizeof(ipv6_network));
    memset(&ipv6_mask, 0, sizeof(ipv6_mask));

    if (host_parse_ipv6_cidr(cidr_text, &ipv6_network, &ipv6_mask)) {
        struct in6_addr address6;
        memset(&address6, 0, sizeof(address6));
        if (inet_pton(AF_INET6, ip, &address6) != 1) {
            return false;
        }

        for (size_t idx = 0U; idx < sizeof(address6.s6_addr); ++idx) {
            if ((address6.s6_addr[idx] & ipv6_mask.s6_addr[idx]) !=
                ipv6_network.s6_addr[idx]) {
                return false;
            }
        }

        return true;
    }

    return false;
}

static void host_version_ip_rule_release(version_ip_ban_rule_t *rule)
{
    if (rule == nullptr) {
        return;
    }

    if (rule->original_pattern != nullptr) {
        sshc_gc_free(rule->original_pattern);
        rule->original_pattern = nullptr;
    }
    if (rule->normalized_pattern != nullptr) {
        sshc_gc_free(rule->normalized_pattern);
        rule->normalized_pattern = nullptr;
    }
    rule->in_use = false;
}

static bool host_version_ip_rules_reserve(host_t *host, size_t min_capacity)
{
    if (host == nullptr) {
        return false;
    }

    sshc_memory_context_t *memory_scope = host_memory_scope_push(host);
    bool success = false;

    if (min_capacity <= host->version_ip_ban_rule_capacity &&
        host->version_ip_ban_rules != nullptr) {
        success = true;
        goto cleanup;
    }

    size_t new_capacity = host->version_ip_ban_rule_capacity > 0U
                              ? host->version_ip_ban_rule_capacity
                              : 16U;
    while (new_capacity < min_capacity &&
           new_capacity < SSH_CHATTER_MAX_VERSION_IP_BANS) {
        new_capacity *= 2U;
    }
    if (new_capacity > SSH_CHATTER_MAX_VERSION_IP_BANS) {
        new_capacity = SSH_CHATTER_MAX_VERSION_IP_BANS;
    }
    if (new_capacity < min_capacity) {
        goto cleanup;
    }

    version_ip_ban_rule_t *buffer =
        sshc_gc_calloc(new_capacity, sizeof(*buffer));
    if (buffer == nullptr) {
        goto cleanup;
    }

    if (host->version_ip_ban_rules != nullptr &&
        host->version_ip_ban_rule_count > 0U) {
        memcpy(buffer, host->version_ip_ban_rules,
               host->version_ip_ban_rule_count * sizeof(*buffer));
        sshc_gc_free(host->version_ip_ban_rules);
    } else if (host->version_ip_ban_rules != nullptr) {
        sshc_gc_free(host->version_ip_ban_rules);
    }

    host->version_ip_ban_rules = buffer;
    host->version_ip_ban_rule_capacity = new_capacity;
    success = true;

cleanup:
    host_memory_scope_pop(memory_scope);
    return success;
}

static bool host_version_ip_rules_prepare(host_t *host)
{
    if (host == nullptr) {
        return false;
    }

    if (host->version_ip_ban_rules == nullptr ||
        host->version_ip_ban_rule_capacity == 0U) {
        if (!host_version_ip_rules_reserve(host, 16U)) {
            return false;
        }
    } else {
        for (size_t idx = 0U; idx < host->version_ip_ban_rule_capacity;
             ++idx) {
            host_version_ip_rule_release(&host->version_ip_ban_rules[idx]);
            memset(&host->version_ip_ban_rules[idx], 0,
                   sizeof(host->version_ip_ban_rules[idx]));
        }
    }

    host->version_ip_ban_rule_count = 0U;
    return true;
}

static bool host_version_ip_rule_matches(const version_ip_ban_rule_t *rule,
                                         const char *version, const char *ip)
{
    if (rule == nullptr || !rule->in_use || ip == nullptr || ip[0] == '\0') {
        return false;
    }

    bool version_match = false;
    const char *normalized = rule->normalized_pattern;
    switch (rule->match_mode) {
    case VERSION_PATTERN_MATCH_ANY:
        version_match = true;
        break;
    case VERSION_PATTERN_MATCH_EXACT:
        if (version != nullptr && normalized != nullptr) {
            version_match = strcmp(version, normalized) == 0;
        }
        break;
    case VERSION_PATTERN_MATCH_PREFIX:
        if (version != nullptr && normalized != nullptr) {
            size_t prefix_len =
                strnlen(normalized, SSH_CHATTER_VERSION_PATTERN_LEN);
            version_match =
                (prefix_len > 0U && strncmp(version, normalized, prefix_len) ==
                                        0);
        }
        break;
    case VERSION_PATTERN_MATCH_SUFFIX:
        if (version != nullptr && normalized != nullptr) {
            size_t suffix_len =
                strnlen(normalized, SSH_CHATTER_VERSION_PATTERN_LEN);
            size_t version_len = strlen(version);
            if (suffix_len > 0U && version_len >= suffix_len) {
                version_match = strncmp(version + (version_len - suffix_len),
                                        normalized, suffix_len) == 0;
            }
        }
        break;
    case VERSION_PATTERN_MATCH_SUBSTRING:
        if (version != nullptr && normalized != nullptr &&
            normalized[0] != '\0') {
            version_match = strstr(version, normalized) != nullptr;
        }
        break;
    default:
        version_match = false;
        break;
    }

    if (!version_match) {
        return false;
    }

    if (!rule->is_ipv6) {
        struct in_addr address = {0};
        if (inet_pton(AF_INET, ip, &address) != 1) {
            return false;
        }
        uint32_t ip_value = ntohl(address.s_addr);
        return (ip_value & rule->ipv4_mask) == rule->ipv4_network;
    }

    struct in6_addr address6;
    memset(&address6, 0, sizeof(address6));
    if (inet_pton(AF_INET6, ip, &address6) != 1) {
        return false;
    }

    for (size_t idx = 0U; idx < sizeof(address6.s6_addr); ++idx) {
        if ((address6.s6_addr[idx] & rule->ipv6_mask.s6_addr[idx]) !=
            rule->ipv6_network.s6_addr[idx]) {
            return false;
        }
    }

    return true;
}

static bool host_version_ip_rule_add(host_t *host, const char *pattern,
                                     const char *cidr, const char *note)
{
    if (host == nullptr || pattern == nullptr || cidr == nullptr) {
        return false;
    }

    if (host->version_ip_ban_rule_count >= SSH_CHATTER_MAX_VERSION_IP_BANS) {
        printf(
            "[security] version/IP rule capacity reached; skipping %s @ %s\n",
            pattern, cidr);
        return false;
    }

    if (!host_version_ip_rules_reserve(
            host, host->version_ip_ban_rule_count + 1U)) {
        printf("[security] unable to grow version/IP rule table; skipping %s @ "
               "%s\n",
               pattern, cidr);
        return false;
    }

    char normalized[SSH_CHATTER_VERSION_PATTERN_LEN];
    char original[SSH_CHATTER_VERSION_PATTERN_LEN];
    version_pattern_match_t match_mode = VERSION_PATTERN_MATCH_EXACT;
    if (!host_version_ip_parse_pattern(pattern, normalized, sizeof(normalized),
                                       original, sizeof(original),
                                       &match_mode)) {
        printf("[security] invalid version pattern '%s'\n", pattern);
        return false;
    }

    char cidr_copy[SSH_CHATTER_CIDR_TEXT_LEN];
    int cidr_written = snprintf(cidr_copy, sizeof(cidr_copy), "%s", cidr);
    if (cidr_written < 0 || (size_t)cidr_written >= sizeof(cidr_copy)) {
        printf("[security] invalid CIDR '%s'\n", cidr);
        return false;
    }

    char *cidr_trimmed = host_trim_whitespace(cidr_copy);
    if (cidr_trimmed == nullptr || cidr_trimmed[0] == '\0') {
        printf("[security] CIDR '%s' empty after trimming\n", cidr);
        return false;
    }

    uint32_t ipv4_network = 0U;
    uint32_t ipv4_mask = 0U;
    struct in6_addr ipv6_network;
    struct in6_addr ipv6_mask;
    memset(&ipv6_network, 0, sizeof(ipv6_network));
    memset(&ipv6_mask, 0, sizeof(ipv6_mask));
    bool is_ipv6 = false;

    if (host_parse_ipv4_cidr(cidr_trimmed, &ipv4_network, &ipv4_mask)) {
        is_ipv6 = false;
    } else if (host_parse_ipv6_cidr(cidr_trimmed, &ipv6_network, &ipv6_mask)) {
        is_ipv6 = true;
    } else {
        printf("[security] failed to parse CIDR '%s'\n", cidr_trimmed);
        return false;
    }

    for (size_t idx = 0U; idx < host->version_ip_ban_rule_count; ++idx) {
        version_ip_ban_rule_t *existing = &host->version_ip_ban_rules[idx];
        if (!existing->in_use) {
            continue;
        }
        if (existing->match_mode != match_mode) {
            continue;
        }
        if (existing->normalized_pattern == nullptr ||
            strncmp(existing->normalized_pattern, normalized,
                    SSH_CHATTER_VERSION_PATTERN_LEN) != 0) {
            continue;
        }
        if (existing->is_ipv6 != is_ipv6) {
            continue;
        }
        bool network_match = false;
        if (!is_ipv6) {
            network_match = (existing->ipv4_network == ipv4_network &&
                             existing->ipv4_mask == ipv4_mask);
        } else {
            network_match =
                (memcmp(existing->ipv6_network.s6_addr, ipv6_network.s6_addr,
                        sizeof(ipv6_network.s6_addr)) == 0) &&
                (memcmp(existing->ipv6_mask.s6_addr, ipv6_mask.s6_addr,
                        sizeof(ipv6_mask.s6_addr)) == 0);
        }
        if (network_match) {
            if (note != nullptr && note[0] != '\0') {
                size_t note_len =
                    strnlen(note, SSH_CHATTER_VERSION_NOTE_LEN - 1U);
                memcpy(existing->note, note, note_len);
                existing->note[note_len] = '\0';
            }
            return true;
        }
    }

    version_ip_ban_rule_t *rule =
        &host->version_ip_ban_rules[host->version_ip_ban_rule_count];
    host_version_ip_rule_release(rule);
    memset(rule, 0, sizeof(*rule));
    rule->in_use = true;
    rule->match_mode = match_mode;
    char *original_copy = sshc_strdup(original);
    char *normalized_copy = sshc_strdup(normalized);
    if (original_copy == nullptr || normalized_copy == nullptr) {
        sshc_gc_free(original_copy);
        sshc_gc_free(normalized_copy);
        printf("[security] unable to allocate memory for version/IP rule %s @ "
               "%s\n",
               original, cidr_trimmed);
        return false;
    }
    rule->original_pattern = original_copy;
    rule->normalized_pattern = normalized_copy;
    snprintf(rule->cidr_text, sizeof(rule->cidr_text), "%s", cidr_trimmed);
    rule->is_ipv6 = is_ipv6;
    if (!is_ipv6) {
        rule->ipv4_network = ipv4_network;
        rule->ipv4_mask = ipv4_mask;
    } else {
        rule->ipv6_network = ipv6_network;
        rule->ipv6_mask = ipv6_mask;
    }

    if (note != nullptr && note[0] != '\0') {
        size_t note_len = strnlen(note, SSH_CHATTER_VERSION_NOTE_LEN - 1U);
        memcpy(rule->note, note, note_len);
        rule->note[note_len] = '\0';
    } else {
        rule->note[0] = '\0';
    }

    host->version_ip_ban_rule_count += 1U;

    const char *note_display =
        rule->note[0] != '\0' ? rule->note : "version/IP policy";
    const char *pattern_display =
        (rule->original_pattern != nullptr && rule->original_pattern[0] != '\0')
            ? rule->original_pattern
            : "*";
    printf("[security] loaded version/IP ban rule: %s @ %s (%s)\n",
           pattern_display, rule->cidr_text, note_display);

    return true;
}

static void host_version_ip_rules_load_env(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    const char *env = getenv("CHATTER_VERSION_IP_BANS");
    if (env == nullptr || env[0] == '\0') {
        return;
    }

    char *copy = sshc_strdup(env);
    if (copy == nullptr) {
        return;
    }

    char *context = nullptr;
    for (char *token = strtok_r(copy, VERSION_IP_RULE_SEPARATOR, &context);
         token != nullptr;
         token = strtok_r(nullptr, VERSION_IP_RULE_SEPARATOR, &context)) {
        char *trimmed = host_trim_whitespace(token);
        if (trimmed == nullptr || trimmed[0] == '\0') {
            continue;
        }

        char *note_part = nullptr;
        char *hash = strchr(trimmed, '#');
        if (hash != nullptr) {
            *hash = '\0';
            note_part = host_trim_whitespace(hash + 1);
        }

        char *separator = strchr(trimmed, '@');
        if (separator == nullptr) {
            printf("[security] ignoring malformed version/IP rule '%s'\n",
                   trimmed);
            continue;
        }

        *separator = '\0';
        char *pattern = host_trim_whitespace(trimmed);
        char *cidr = host_trim_whitespace(separator + 1);
        if (pattern == nullptr || cidr == nullptr || pattern[0] == '\0' ||
            cidr[0] == '\0') {
            printf("[security] ignoring malformed version/IP rule entry\n");
            continue;
        }

        const char *note = (note_part != nullptr && note_part[0] != '\0')
                               ? note_part
                               : "custom rule";
        if (!host_version_ip_rule_add(host, pattern, cidr, note)) {
            printf("[security] failed to register version/IP rule from "
                   "environment: "
                   "%s @ %s\n",
                   pattern, cidr);
        }
    }

    sshc_gc_free(copy);
}

static bool
host_version_ip_should_ban(host_t *host, const char *version, const char *ip,
                           const version_ip_ban_rule_t **matched_rule)
{
    if (matched_rule != nullptr) {
        *matched_rule = nullptr;
    }

    if (host == nullptr || ip == nullptr || ip[0] == '\0') {
        return false;
    }

    for (size_t idx = 0U; idx < host->version_ip_ban_rule_count; ++idx) {
        const version_ip_ban_rule_t *rule = &host->version_ip_ban_rules[idx];
        if (!rule->in_use) {
            continue;
        }
        if (host_version_ip_rule_matches(rule, version, ip)) {
            if (matched_rule != nullptr) {
                *matched_rule = rule;
            }
            return true;
        }
    }

    return false;
}

static void host_version_ip_rules_init(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (!host_version_ip_rules_prepare(host)) {
        printf("[security] unable to allocate version/IP ban rule table\n");
        return;
    }

    for (size_t idx = 0U;
         idx < (sizeof(kVersionIpBanSeeds) / sizeof(kVersionIpBanSeeds[0]));
         ++idx) {
        const version_ip_ban_seed_t *seed = &kVersionIpBanSeeds[idx];
        (void)host_version_ip_rule_add(host, seed->pattern, seed->cidr,
                                       seed->note);
    }

    host_version_ip_rules_load_env(host);
}

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

static const char kTranslationQuotaNotice[] =
    "[!] Translation quota exhausted. Translation features are temporarily "
    "disabled.";
static const char kTranslationQuotaSystemMessage[] =
    "Translation quota exhausted. Translation has been disabled. Try again "
    "later.";

static const char *const kSessionCommandNames[] = {
    "asciiart",
    "audio",
    "ban",
    "banlist",
    "bbs",
    "birthday",
    "block",
    "breaking",
    "captcha",
    "chat",
    "chat-spacing",
    "color",
    "connected",
    "date",
    "delete-msg",
    "elect",
    "eliza",
    "eliza-chat",
    "exit",
    "files",
    "game",
    "gemini",
    "gemini-unfreeze",
    "getos",
    "grant",
    "help",
    "history",
    "advanced",
    "image",
    "kick",
    "mode",
    "motd",
    "nick",
    "os",
    "pair",
    "palette",
    "pardon",
    "pm",
    "poke",
    "poll",
    "reply",
    "revoke",
    "rss",
    "search",
    "setpw",
    "shell",
    "set-target-lang",
    "set-trans-lang",
    "set-ui-lang",
    "showstatus",
    "status",
    "suspend!",
    "sync-trigger",
    "systemcolor",
    "today",
    "translate",
    "translate-scope",
    "unblock",
    "users",
    "video",
    "vote",
    "vote-single",
    "weather",
    "gameopt",
};
#define SSH_CHATTER_COMMAND_COUNT                                              \
    (sizeof(kSessionCommandNames) / sizeof(kSessionCommandNames[0]))

typedef enum session_help_entry_kind {
    SESSION_HELP_ENTRY_COMMAND = 0,
    SESSION_HELP_ENTRY_FORMATTED,
    SESSION_HELP_ENTRY_TEXT,
} session_help_entry_kind_t;

#define SESSION_HELP_TEMPLATE_ARG_LIMIT 8U

typedef enum session_help_template_arg_kind {
    SESSION_HELP_TEMPLATE_ARG_PREFIX = 0,
    SESSION_HELP_TEMPLATE_ARG_ASCIIART_TERMINATOR,
    SESSION_HELP_TEMPLATE_ARG_BBS_TERMINATOR,
    SESSION_HELP_TEMPLATE_ARG_COMMAND_REPLY,
    SESSION_HELP_TEMPLATE_ARG_COMMAND_GOOD,
    SESSION_HELP_TEMPLATE_ARG_COMMAND_SAD,
    SESSION_HELP_TEMPLATE_ARG_COMMAND_WTF,
    SESSION_HELP_TEMPLATE_ARG_COMMAND_COOL,
    SESSION_HELP_TEMPLATE_ARG_COMMAND_ANGRY,
    SESSION_HELP_TEMPLATE_ARG_COMMAND_CHECKED,
    SESSION_HELP_TEMPLATE_ARG_COMMAND_LOVE,
    SESSION_HELP_TEMPLATE_ARG_COMMAND_BBS,
} session_help_template_arg_kind_t;

typedef struct session_help_entry {
    session_help_entry_kind_t kind;
    const char *label;
    const char *description[SESSION_UI_LANGUAGE_COUNT];
    const char *label_translations[SESSION_UI_LANGUAGE_COUNT];
    size_t label_arg_count;
    session_help_template_arg_kind_t
        label_args[SESSION_HELP_TEMPLATE_ARG_LIMIT];
    size_t description_arg_count;
    session_help_template_arg_kind_t
        description_args[SESSION_HELP_TEMPLATE_ARG_LIMIT];
} session_help_entry_t;

typedef struct session_command_alias {
    const char *canonical;
    const char *localized[SESSION_UI_LANGUAGE_COUNT];
} session_command_alias_t;

typedef struct session_ui_locale {
    session_ui_language_t language;
    const char *code;
    const char *help_title;
    const char *help_hint_extra;
    const char *help_scroll_hint;
    const char *help_regular_hint;
    const char *help_extra_title;
    const char *help_extra_hint;
    const char *help_operator_title;
    const char *welcome_help_hint;
    const char *welcome_motd_hint;
    const char *welcome_history_hint;
    const char *chat_spacing_usage;
    const char *chat_spacing_immediate;
    const char *chat_spacing_single;
    const char *chat_spacing_multiple;
    const char *set_ui_lang_usage;
    const char *set_ui_lang_success;
    const char *set_ui_lang_invalid;
    const char *mode_status_format;
    const char *mode_label_chat;
    const char *mode_label_command;
    const char *mode_explain_chat;
    const char *mode_explain_command;
    const char *mode_already_chat;
    const char *mode_already_command;
    const char *mode_enabled_chat;
    const char *mode_enabled_command;
    const char *mode_usage;
    const char *unknown_command;
    const char *help_morse;
} session_ui_locale_t;

static const char *const kSessionUiLanguageCodes[SESSION_UI_LANGUAGE_COUNT] = {
    "en", "ko", "jp", "zh", "ru", "de", "fr", "pl",
};

static const char *const kSessionUiLanguageNames
    [SESSION_UI_LANGUAGE_COUNT][SESSION_UI_LANGUAGE_COUNT] = {
        [SESSION_UI_LANGUAGE_EN] = {"English", "Korean", "Japanese", "Chinese",
                                    "Russian", "German", "French", "Polish"},
        [SESSION_UI_LANGUAGE_KO] = {"영어", "한국어", "일본어", "중국어",
                                    "러시아어", "독일어", "프랑스어",
                                    "폴란드어"},
        [SESSION_UI_LANGUAGE_JP] = {"英語", "韓国語", "日本語", "中国語",
                                    "ロシア語", "ドイツ語", "フランス語",
                                    "ポーランド語"},
        [SESSION_UI_LANGUAGE_ZH] = {"英语", "韩语", "日语", "中文", "俄语",
                                    "德语", "法语", "波兰语"},
        [SESSION_UI_LANGUAGE_RU] = {"английский", "корейский", "японский",
                                    "китайский", "русский", "немецкий",
                                    "французский", "польский"},
        [SESSION_UI_LANGUAGE_DE] = {"Englisch", "Koreanisch", "Japanisch",
                                    "Chinesisch", "Russisch", "Deutsch",
                                    "Französisch", "Polnisch"},
        [SESSION_UI_LANGUAGE_FR] = {"Anglais", "Coréen", "Japonais", "Chinois",
                                    "Russe", "Allemand", "Français",
                                    "Polonais"},
        [SESSION_UI_LANGUAGE_PL] = {"Angielski", "Koreański", "Japoński",
                                    "Chiński", "Rosyjski", "Niemiecki",
                                    "Francuski", "Polski"},
};

static const session_ui_locale_t kSessionUiLocales[SESSION_UI_LANGUAGE_COUNT] =
    {
        {
            .language = SESSION_UI_LANGUAGE_EN,
            .code = "en",
            .help_title = "Essential commands:",
            .help_hint_extra = "See %sadvanced for optional commands.",
            .help_scroll_hint =
                "Use Up/Down arrows to scroll chat or command history.",
            .help_regular_hint = "Regular messages are shared with everyone.",
            .help_extra_title = "Extended commands:",
            .help_extra_hint = "Return to %shelp for essentials.",
            .help_operator_title = "Operator commands:",
            .welcome_help_hint = "Use %shelp to view the manual.",
            .welcome_motd_hint = "Use %smotd to read the information.",
            .welcome_history_hint = "Previous messages are hidden. Use Up/Down "
                                    "arrows to browse older chat.",
            .chat_spacing_usage = "Usage: %schat-spacing <0-5>",
            .chat_spacing_immediate = "Translation captions will appear "
                                      "immediately without reserving "
                                      "extra blank lines.",
            .chat_spacing_single = "Translation captions will reserve 1 blank "
                                   "line before appearing in chat threads.",
            .chat_spacing_multiple =
                "Translation captions will reserve %s blank lines before "
                "appearing in chat threads.",
            .set_ui_lang_usage =
                "Usage: %sset-ui-lang <ko|en|jp|zh|ru|de|fr|pl>",
            .set_ui_lang_success =
                "UI language set to %s. Use %shelp to review commands.",
            .set_ui_lang_invalid = "Unsupported language. Use one of: ko, en, "
                                   "jp, zh, ru, de, fr, pl.",
            .mode_status_format = "Current input mode: %s.",
            .mode_label_chat = "chat",
            .mode_label_command = "command",
            .mode_explain_chat =
                "Chat mode: send messages normally. Prefix commands with %s.",
            .mode_explain_command =
                "Command mode: type commands without a prefix, use "
                "UpArrow/DownArrow for history, Tab for completion.",
            .mode_already_chat =
                "Already in chat mode. Commands require the %s prefix.",
            .mode_already_command =
                "Command mode already active. Enter commands without a prefix, "
                "use UpArrow/DownArrow for history, Tab to autocomplete.",
            .mode_enabled_chat =
                "Chat mode enabled. Commands once again require the %s prefix.",
            .mode_enabled_command =
                "Command mode enabled. Enter commands without a prefix; use "
                "UpArrow/DownArrow for history and Tab for completion.",
            .mode_usage = "Usage: %smode <chat|command|toggle>",
            .unknown_command = "Unknown command. Type %shelp for help.",
        },
        {
            .language = SESSION_UI_LANGUAGE_KO,
            .code = "ko",
            .help_title = "필수 명령:",
            .help_hint_extra =
                "%sadvanced에서 선택 및 운영자 명령을 확인합니다.",
            .help_scroll_hint =
                "위/아래 화살표로 채팅이나 명령 기록을 살펴볼 수 있습니다.",
            .help_regular_hint = "일반 메시지는 모두에게 공유됩니다.",
            .help_extra_title = "확장 명령:",
            .help_extra_hint = "핵심 목록은 %shelp에서 다시 볼 수 있습니다.",
            .help_operator_title = "운영자 명령:",
            .welcome_help_hint = "%shelp 명령으로 도움말을 확인하세요.",
            .welcome_motd_hint = "%smotd 명령으로 안내를 읽을 수 있습니다.",
            .welcome_history_hint = "이전 메시지는 숨겨져 있습니다. 위/아래 "
                                    "화살표로 지난 채팅을 살펴보세요.",
            .chat_spacing_usage = "사용법: %schat-spacing <0-5>",
            .chat_spacing_immediate =
                "번역 자막이 빈 줄을 예약하지 않고 즉시 표시됩니다.",
            .chat_spacing_single =
                "번역 자막이 표시되기 전에 빈 줄 1줄을 예약합니다.",
            .chat_spacing_multiple =
                "번역 자막이 표시되기 전에 빈 줄 %s줄을 예약합니다.",
            .set_ui_lang_usage =
                "사용법: %sset-ui-lang <ko|en|jp|zh|ru|de|fr|pl>",
            .set_ui_lang_success = "UI 언어를 %s로 설정했습니다. %shelp "
                                   "명령으로 목록을 다시 확인하세요.",
            .set_ui_lang_invalid =
                "지원하지 않는 언어입니다. ko, en, jp, zh, ru, de, fr, pl"
                "ru 중에서 선택하세요.",
            .mode_status_format = "현재 입력 모드: %s.",
            .mode_label_chat = "채팅",
            .mode_label_command = "명령",
            .mode_explain_chat = "채팅 모드: 일반 메시지를 보내고, 명령은 %s "
                                 "접두사를 붙여 입력하세요.",
            .mode_explain_command = "명령 모드: 접두사 없이 명령을 입력하고, "
                                    "위/아래 화살표로 기록을 "
                                    "탐색하며 Tab으로 자동완성하세요.",
            .mode_already_chat =
                "이미 채팅 모드입니다. 명령은 %s 접두사가 필요합니다.",
            .mode_already_command =
                "이미 명령 모드입니다. 접두사 없이 입력하고, 위/아래 화살표와 "
                "Tab을 활용하세요.",
            .mode_enabled_chat = "채팅 모드가 활성화되었습니다. 명령은 다시 %s "
                                 "접두사가 필요합니다.",
            .mode_enabled_command =
                "명령 모드가 활성화되었습니다. 접두사 없이 입력하고, 위/아래 "
                "화살표와 Tab을 사용하세요.",
            .mode_usage = "사용법: %smode <chat|command|toggle>",
            .unknown_command =
                "알 수 없는 명령입니다. 도움말은 %shelp에서 확인하세요.",
        },
        {
            .language = SESSION_UI_LANGUAGE_JP,
            .code = "jp",
            .help_title = "基本コマンド:",
            .help_hint_extra = "追加コマンドは %sadvanced で確認できます。",
            .help_scroll_hint =
                "上下の矢印でチャットやコマンド履歴をたどれます。",
            .help_regular_hint = "通常のメッセージは全員に共有されます。",
            .help_extra_title = "拡張コマンド:",
            .help_extra_hint = "基本一覧に戻るには %shelp を実行してください。",
            .help_operator_title = "オペレーター用コマンド:",
            .welcome_help_hint = "%shelp でヘルプを表示できます。",
            .welcome_motd_hint = "%smotd でお知らせを確認できます。",
            .welcome_history_hint = "以前のメッセージは非表示です。上下の矢印で"
                                    "過去のチャットを確認できます。",
            .chat_spacing_usage = "使い方: %schat-spacing <0-5>",
            .chat_spacing_immediate =
                "翻訳字幕は空行を確保せずすぐに表示されます。",
            .chat_spacing_single = "翻訳字幕は表示前に空行を 1 行確保します。",
            .chat_spacing_multiple =
                "翻訳字幕は表示前に空行を %s 行確保します。",
            .set_ui_lang_usage =
                "使い方: %sset-ui-lang <ko|en|jp|zh|ru|de|fr|pl>",
            .set_ui_lang_success = "UI 言語を %s に設定しました。%shelp "
                                   "でコマンドを再確認してください。",
            .set_ui_lang_invalid =
                "対応していない言語です。ko, en, jp, zh, ru, de, fr, pl"
                "から選んでください。",
            .mode_status_format = "現在の入力モード: %s。",
            .mode_label_chat = "チャット",
            .mode_label_command = "コマンド",
            .mode_explain_chat =
                "チャットモード: 通常通りメッセージを送り、コマンドは %s "
                "を付けて入力します。",
            .mode_explain_command =
                "コマンドモード: 接頭辞なしで入力し、上下矢印で履歴を、Tab "
                "で補完を利用できます。",
            .mode_already_chat =
                "すでにチャットモードです。コマンドには %s を付けてください。",
            .mode_already_command = "すでにコマンドモードです。接頭辞なしで入力"
                                    "し、上下矢印と Tab を使ってください。",
            .mode_enabled_chat = "チャットモードを有効にしました。コマンドには"
                                 "再び %s が必要です。",
            .mode_enabled_command = "コマンドモードを有効にしました。接頭辞なし"
                                    "で入力し、上下矢印と "
                                    "Tab を使ってください。",
            .mode_usage = "使い方: %smode <chat|command|toggle>",
            .unknown_command =
                "不明なコマンドです。%shelp で確認してください。",
        },
        {
            .language = SESSION_UI_LANGUAGE_ZH,
            .code = "zh",
            .help_title = "核心命令：",
            .help_hint_extra = "更多命令请查看 %sadvanced。",
            .help_scroll_hint = "使用上下方向键查看聊天或命令历史。",
            .help_regular_hint = "普通消息会分享给所有人。",
            .help_extra_title = "扩展命令：",
            .help_extra_hint = "返回核心列表请使用 %shelp。",
            .help_operator_title = "管理员命令：",
            .welcome_help_hint = "使用 %shelp 查看帮助。",
            .welcome_motd_hint = "使用 %smotd 阅读公告。",
            .welcome_history_hint =
                "之前的消息已隐藏。使用上下方向键查看较早的聊天。",
            .chat_spacing_usage = "用法：%schat-spacing <0-5>",
            .chat_spacing_immediate = "翻译字幕会立即显示，不再预留空行。",
            .chat_spacing_single = "翻译字幕在显示前会预留 1 行空白。",
            .chat_spacing_multiple = "翻译字幕在显示前会预留 %s 行空白。",
            .set_ui_lang_usage =
                "用法：%sset-ui-lang <ko|en|jp|zh|ru|de|fr|pl>",
            .set_ui_lang_success =
                "界面语言已切换为 %s。可用 %shelp 重新查看命令。",
            .set_ui_lang_invalid =
                "不支持的语言，请选择 ko、en、jp、zh、ru、de、fr、pl。",
            .mode_status_format = "当前输入模式：%s。",
            .mode_label_chat = "聊天",
            .mode_label_command = "命令",
            .mode_explain_chat = "聊天模式：正常发送消息，命令需加上 %s 前缀。",
            .mode_explain_command = "命令模式：直接输入命令，不需要前缀；用上下"
                                    "方向键查看历史，Tab 自动补全。",
            .mode_already_chat = "已经是聊天模式。命令需要 %s 前缀。",
            .mode_already_command =
                "已经是命令模式。无需前缀，使用上下方向键和 Tab。",
            .mode_enabled_chat = "聊天模式已启用。命令重新需要 %s 前缀。",
            .mode_enabled_command =
                "命令模式已启用。无需前缀，可用上下方向键和 Tab。",
            .mode_usage = "用法：%smode <chat|command|toggle>",
            .unknown_command = "未知命令。请使用 %shelp 查看帮助。",
        },
        {
            .language = SESSION_UI_LANGUAGE_RU,
            .code = "ru",
            .help_title = "Основные команды:",
            .help_hint_extra = "Дополнительные команды смотрите в %sadvanced.",
            .help_scroll_hint =
                "Стрелки вверх/вниз листают чат или историю команд.",
            .help_regular_hint = "Обычные сообщения видны всем.",
            .help_extra_title = "Дополнительные команды:",
            .help_extra_hint = "К основному списку вернёт %shelp.",
            .help_operator_title = "Команды оператора:",
            .welcome_help_hint = "Команду %shelp используйте для справки.",
            .welcome_motd_hint = "%smotd покажет объявление.",
            .welcome_history_hint =
                "Предыдущие сообщения скрыты. Используйте стрелки вверх/вниз, "
                "чтобы просмотреть старый чат.",
            .chat_spacing_usage = "Использование: %schat-spacing <0-5>",
            .chat_spacing_immediate = "Подписи перевода будут появляться "
                                      "сразу, без запасных пустых строк.",
            .chat_spacing_single =
                "Подписи перевода перед выводом резервируют 1 пустую строку.",
            .chat_spacing_multiple =
                "Подписи перевода перед выводом резервируют %s пустых строк.",
            .set_ui_lang_usage =
                "Использование: %sset-ui-lang <ko|en|jp|zh|ru|de|fr|pl>",
            .set_ui_lang_success = "Язык интерфейса переключён на %s. Команды "
                                   "можно пересмотреть через %shelp.",
            .set_ui_lang_invalid = "Этот язык не поддерживается. Выберите один "
                                   "из: ko, en, jp, zh, ru, de, fr, pl.",
            .mode_status_format = "Текущий режим ввода: %s.",
            .mode_label_chat = "чат",
            .mode_label_command = "команды",
            .mode_explain_chat = "Режим чата: отправляйте сообщения, а команды "
                                 "вводите с префиксом %s.",
            .mode_explain_command =
                "Режим команд: вводите без префикса, используйте стрелки "
                "вверх/вниз для истории и Tab для автодополнения.",
            .mode_already_chat =
                "Вы уже в режиме чата. Командам нужен префикс %s.",
            .mode_already_command = "Режим команд уже активен. Вводите без "
                                    "префикса, пользуйтесь стрелками и Tab.",
            .mode_enabled_chat =
                "Включён режим чата. Командам снова требуется префикс %s.",
            .mode_enabled_command = "Включён режим команд. Вводите без "
                                    "префикса, применяйте стрелки и Tab.",
            .mode_usage = "Использование: %smode <chat|command|toggle>",
            .unknown_command = "Неизвестная команда. Подсказка — %shelp.",
        },
        {
            .language = SESSION_UI_LANGUAGE_DE,
            .code = "de",
            .help_title = "Wichtige Befehle:",
            .help_hint_extra =
                "%sadvanced für zusätzliche und Operator-Befehle.",
            .help_scroll_hint =
                "Auf-/Ab-Pfeile zum Scrollen von Chat oder Befehlsverlauf.",
            .help_regular_hint = "Normal Nachrichten werden mit allen geteilt.",
            .help_extra_title = "Erweiterte Befehle:",
            .help_extra_hint = "Zurück zu %shelp für die Grundlagen.",
            .help_operator_title = "Operator-Befehle:",
            .welcome_help_hint = "Benutzen Sie %shelp für das Handbuch.",
            .welcome_motd_hint = "Benutzen Sie %smotd für die Informationen.",
            .welcome_history_hint =
                "Vorherige Nachrichten sind verborgen. Auf-/Ab-Pfeile für "
                "älteren Chat.",
            .chat_spacing_usage = "Nutzung: %schat-spacing <0-5>",
            .chat_spacing_immediate =
                "Übersetzungsunterschriften erscheinen "
                "sofort ohne Reservierung von Leerzeilen.",
            .chat_spacing_single =
                "Übersetzungsunterschriften reservieren 1 Leerzeile vor "
                "Erscheinen in Chat-Threads.",
            .chat_spacing_multiple =
                "Übersetzungsunterschriften reservieren %s Leerzeilen vor "
                "Erscheinen in Chat-Threads.",
            .set_ui_lang_usage =
                "Nutzung: %sset-ui-lang <ko|en|jp|zh|ru|de|fr|pl>",
            .set_ui_lang_success =
                "UI-Sprache auf %s gesetzt. %shelp zeigt Befehle.",
            .set_ui_lang_invalid =
                "Nicht unterstützte Sprache. Wählen Sie: ko, "
                "en, jp, zh, ru, de, fr, pl.",
            .mode_status_format = "Aktueller Eingabemodus: %s.",
            .mode_label_chat = "Chat",
            .mode_label_command = "Befehl",
            .mode_explain_chat =
                "Chat-Modus: Nachrichten normal senden. Befehle mit %s "
                "prefixen.",
            .mode_explain_command = "Befehlsmodus: Befehle ohne Präfix "
                                    "eingeben, Auf-/Ab-Pfeile für "
                                    "Verlauf, Tab für Vervollständigung.",
            .mode_already_chat =
                "Bereits im Chat-Modus. Befehle benötigen das %s Präfix.",
            .mode_already_command =
                "Befehlsmodus bereits aktiv. Befehle ohne Präfix, "
                "Auf-/Ab-Pfeile für Verlauf, Tab für Vervollständigung.",
            .mode_enabled_chat =
                "Chat-Modus aktiviert. Befehle benötigen wieder das %s Präfix.",
            .mode_enabled_command =
                "Befehlsmodus aktiviert. Befehle ohne Präfix, Auf-/Ab-Pfeile "
                "und Tab verfügbar.",
            .mode_usage = "Nutzung: %smode <chat|command|toggle>",
            .unknown_command =
                "Unbekannter Befehl. Tippen Sie %shelp für Hilfe.",
        },
        {
            .language = SESSION_UI_LANGUAGE_FR,
            .code = "fr",
            .help_title = "Commandes essentielles:",
            .help_hint_extra =
                "%sadvanced pour les commandes supplémentaires et d'opérateur.",
            .help_scroll_hint =
                "Flèches Haut/Bas pour faire défiler le chat ou l'historique.",
            .help_regular_hint =
                "Les messages normaux sont partagés avec tous.",
            .help_extra_title = "Commandes étendues:",
            .help_extra_hint = "Retour à %shelp pour l'essentiel.",
            .help_operator_title = "Commandes d'opérateur:",
            .welcome_help_hint = "Utilisez %shelp pour afficher le manuel.",
            .welcome_motd_hint = "Utilisez %smotd pour lire les informations.",
            .welcome_history_hint =
                "Messages précédents sont cachés. Flèches Haut/Bas pour "
                "parcourir l'ancien chat.",
            .chat_spacing_usage = "Utilisation: %schat-spacing <0-5>",
            .chat_spacing_immediate = "Les sous-titres de traduction "
                                      "apparaissent immédiatement sans "
                                      "réserver de lignes vides.",
            .chat_spacing_single =
                "Les sous-titres de traduction réserveront 1 ligne vide avant "
                "d'apparaître dans les fils de discussion.",
            .chat_spacing_multiple =
                "Les sous-titres de traduction réserveront %s lignes vides "
                "avant d'apparaître dans les fils de discussion.",
            .set_ui_lang_usage =
                "Utilisation: %sset-ui-lang <ko|en|jp|zh|ru|de|fr|pl>",
            .set_ui_lang_success = "Langue de l'interface définie sur %s. "
                                   "Utilisez %shelp pour revoir les commandes.",
            .set_ui_lang_invalid = "Langue non prise en charge. Utilisez l'une "
                                   "des: ko, en, jp, zh, ru, de, fr, pl.",
            .mode_status_format = "Mode d'entrée actuel: %s.",
            .mode_label_chat = "chat",
            .mode_label_command = "commande",
            .mode_explain_chat =
                "Mode chat: envoyez des messages normalement. Préfixez les "
                "commandes avec %s.",
            .mode_explain_command =
                "Mode commande: tapez des commandes sans préfixe, flèches "
                "Haut/Bas pour l'historique, Tab pour la complétion.",
            .mode_already_chat =
                "Déjà en mode chat. Les commandes nécessitent le préfixe %s.",
            .mode_already_command =
                "Mode commande déjà actif. Entrez les commandes sans préfixe, "
                "flèches Haut/Bas pour l'historique, Tab pour compléter.",
            .mode_enabled_chat =
                "Mode chat activé. Les commandes nécessitent à "
                "nouveau le préfixe %s.",
            .mode_enabled_command =
                "Mode commande activé. Entrez les commandes sans préfixe, "
                "flèches Haut/Bas et Tab disponibles.",
            .mode_usage = "Utilisation: %smode <chat|command|toggle>",
            .unknown_command = "Commande inconnue. Tapez %shelp pour l'aide.",
        },
        {
            .language = SESSION_UI_LANGUAGE_PL,
            .code = "pl",
            .help_title = "Podstawowe polecenia:",
            .help_hint_extra =
                "%sadvanced dla dodatkowych poleceń i poleceń operatora.",
            .help_scroll_hint =
                "Strzałki Góra/Dół do przewijania czatu lub historii poleceń.",
            .help_regular_hint = "Zwykłe wiadomości są udostępniane wszystkim.",
            .help_extra_title = "Rozszerzone polecenia:",
            .help_extra_hint = "Wróć do %shelp dla podstaw.",
            .help_operator_title = "Polecenia operatora:",
            .welcome_help_hint = "Użyj %shelp aby zobaczyć instrukcję.",
            .welcome_motd_hint = "Użyj %smotd aby przeczytać informacje.",
            .welcome_history_hint =
                "Poprzednie wiadomości są ukryte. Strzałki Góra/Dół do "
                "przewijania starszego czatu.",
            .chat_spacing_usage = "Użycie: %schat-spacing <0-5>",
            .chat_spacing_immediate =
                "Napisy tłumaczeń pojawiają się "
                "natychmiast bez rezerwowania pustych linii.",
            .chat_spacing_single =
                "Napisy tłumaczeń rezerwują 1 pustą linię przed "
                "pojawieniem się w wątkach czatu.",
            .chat_spacing_multiple =
                "Napisy tłumaczeń rezerwują %s pustych linii przed "
                "pojawieniem się w wątkach czatu.",
            .set_ui_lang_usage =
                "Użycie: %sset-ui-lang <ko|en|jp|zh|ru|de|fr|pl>",
            .set_ui_lang_success = "Język interfejsu ustawiony na %s. "
                                   "Użyj %shelp aby zobaczyć polecenia.",
            .set_ui_lang_invalid = "Nieobsługiwany język. Użyj jednego z: ko, "
                                   "en, jp, zh, ru, de, fr, pl.",
            .mode_status_format = "Aktualny tryb wprowadzania: %s.",
            .mode_label_chat = "czat",
            .mode_label_command = "polecenie",
            .mode_explain_chat =
                "Tryb czatu: wysyłaj wiadomości normalnie. Dodaj prefiks %s do "
                "poleceń.",
            .mode_explain_command =
                "Tryb poleceń: wpisuj polecenia bez prefiksu, strzałki "
                "Góra/Dół dla historii, Tab dla uzupełniania.",
            .mode_already_chat =
                "Już w trybie czatu. Polecenia wymagają prefiksu %s.",
            .mode_already_command =
                "Tryb poleceń już aktywny. Wpisuj polecenia bez prefiksu, "
                "strzałki Góra/Dół dla historii, Tab dla uzupełniania.",
            .mode_enabled_chat = "Tryb czatu włączony. Polecenia ponownie "
                                 "wymagają prefiksu %s.",
            .mode_enabled_command =
                "Tryb poleceń włączony. Wpisuj polecenia bez prefiksu, "
                "strzałki Góra/Dół i Tab dostępne.",
            .mode_usage = "Użycie: %smode <chat|command|toggle>",
            .unknown_command =
                "Nieznane polecenie. Wpisz %shelp aby uzyskać pomoc.",
        },
};

static const char
    *const kSessionAsciiartTerminators[SESSION_UI_LANGUAGE_COUNT] = {
        [SESSION_UI_LANGUAGE_EN] = SSH_CHATTER_ASCIIART_TERMINATOR_EN,
        [SESSION_UI_LANGUAGE_KO] = ">/__그림_끝>",
        [SESSION_UI_LANGUAGE_JP] = ">/__アート_終了>",
        [SESSION_UI_LANGUAGE_ZH] = ">/__图像_结束>",
        [SESSION_UI_LANGUAGE_RU] = ">/__АРТ_КОНЕЦ>",
        [SESSION_UI_LANGUAGE_DE] = ">/__KUNST_ENDE>",
        [SESSION_UI_LANGUAGE_FR] = ">/__ART_FIN>",
        [SESSION_UI_LANGUAGE_PL] = ">/__SZTUKA_KONIEC>",
};

static const char *const kSessionBbsTerminators[SESSION_UI_LANGUAGE_COUNT] = {
    [SESSION_UI_LANGUAGE_EN] = SSH_CHATTER_BBS_TERMINATOR_EN,
    [SESSION_UI_LANGUAGE_KO] = ">/__게시판_끝>",
    [SESSION_UI_LANGUAGE_JP] = ">/__掲示板_終了>",
    [SESSION_UI_LANGUAGE_ZH] = ">/__公告板_结束>",
    [SESSION_UI_LANGUAGE_RU] = ">/__ДОСКА_КОНЕЦ>",
    [SESSION_UI_LANGUAGE_DE] = ">/__BBS_ENDE>",
    [SESSION_UI_LANGUAGE_FR] = ">/__BBS_FIN>",
    [SESSION_UI_LANGUAGE_PL] = ">/__BBS_KONIEC>",
};

static const session_command_alias_t kSessionCommandAliases[] = {
    {
        .canonical = "/reply",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/답장",
                [SESSION_UI_LANGUAGE_JP] = "/返信",
                [SESSION_UI_LANGUAGE_ZH] = "/回复",
                [SESSION_UI_LANGUAGE_RU] = "/ответ",
            },
    },
    {
        .canonical = "/nick",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/닉",
                [SESSION_UI_LANGUAGE_JP] = "/ニックネ",
                [SESSION_UI_LANGUAGE_ZH] = "/昵称",
                [SESSION_UI_LANGUAGE_RU] = "/ник",
            },
    },
    {
        .canonical = "/good",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/좋아요",
                [SESSION_UI_LANGUAGE_JP] = "/いいね",
                [SESSION_UI_LANGUAGE_ZH] = "/点赞",
                [SESSION_UI_LANGUAGE_RU] = "/класс",
            },
    },
    {
        .canonical = "/sad",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/슬퍼요",
                [SESSION_UI_LANGUAGE_JP] = "/かなしい",
                [SESSION_UI_LANGUAGE_ZH] = "/难过",
                [SESSION_UI_LANGUAGE_RU] = "/грусть",
            },
    },
    {
        .canonical = "/wtf",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/어쩌라고",
                [SESSION_UI_LANGUAGE_JP] = "/なんだと",
                [SESSION_UI_LANGUAGE_ZH] = "/搞什么",
                [SESSION_UI_LANGUAGE_RU] = "/чтоэто",
            },
    },
    {
        .canonical = "/cool",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/멋져요",
                [SESSION_UI_LANGUAGE_JP] = "/クール",
                [SESSION_UI_LANGUAGE_ZH] = "/酷",
                [SESSION_UI_LANGUAGE_RU] = "/круто",
            },
    },
    {
        .canonical = "/angry",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/화나요",
                [SESSION_UI_LANGUAGE_JP] = "/怒り",
                [SESSION_UI_LANGUAGE_ZH] = "/生气",
                [SESSION_UI_LANGUAGE_RU] = "/злой",
            },
    },
    {
        .canonical = "/checked",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/확인",
                [SESSION_UI_LANGUAGE_JP] = "/確認済み",
                [SESSION_UI_LANGUAGE_ZH] = "/已检查",
                [SESSION_UI_LANGUAGE_RU] = "/проверено",
            },
    },
    {
        .canonical = "/love",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/사랑해요",
                [SESSION_UI_LANGUAGE_JP] = "/愛",
                [SESSION_UI_LANGUAGE_ZH] = "/爱",
                [SESSION_UI_LANGUAGE_RU] = "/любовь",
            },
    },
    {
        .canonical = "/bbs",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/게시판",
                [SESSION_UI_LANGUAGE_JP] = "/掲示板",
                [SESSION_UI_LANGUAGE_ZH] = "/公告板",
                [SESSION_UI_LANGUAGE_RU] = "/доска",
            },
    },
    {
        .canonical = "/vote",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/투표",
            },
    },
    {
        .canonical = "/poll",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/전역투표",
            },
    },
    {
        .canonical = "/vote-single",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/단일투표",
            },
    },
    {
        .canonical = "/elect",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/선택",
            },
    },
    {
        .canonical = "/ban",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/밴",
                [SESSION_UI_LANGUAGE_JP] = "/バン",
                [SESSION_UI_LANGUAGE_ZH] = "/封禁",
                [SESSION_UI_LANGUAGE_RU] = "/бан",
            },
    },
    {
        .canonical = "/banlist",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/밴목록",
                [SESSION_UI_LANGUAGE_JP] = "/バンリスト",
                [SESSION_UI_LANGUAGE_ZH] = "/封禁列表",
                [SESSION_UI_LANGUAGE_RU] = "/список-банов",
            },
    },
    {
        .canonical = "/banname",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/닉차단",
                [SESSION_UI_LANGUAGE_JP] = "/ニックネ禁止",
                [SESSION_UI_LANGUAGE_ZH] = "/屏蔽昵称",
                [SESSION_UI_LANGUAGE_RU] = "/забанить-ник",
            },
    },
    {
        .canonical = "/delete-msg",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/메시지삭제",
            },
    },
    {
        .canonical = "/search",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/검색",
                [SESSION_UI_LANGUAGE_JP] = "/検索",
                [SESSION_UI_LANGUAGE_ZH] = "/搜索",
                [SESSION_UI_LANGUAGE_RU] = "/поиск",
            },
    },
    {
        .canonical = "/image",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/이미지",
                [SESSION_UI_LANGUAGE_JP] = "/画像",
                [SESSION_UI_LANGUAGE_ZH] = "/图片",
                [SESSION_UI_LANGUAGE_RU] = "/изображение",
            },
    },
    {
        .canonical = "/video",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/영상",
                [SESSION_UI_LANGUAGE_JP] = "/動画",
                [SESSION_UI_LANGUAGE_ZH] = "/视频",
                [SESSION_UI_LANGUAGE_RU] = "/видео",
            },
    },
    {
        .canonical = "/audio",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/오디오",
                [SESSION_UI_LANGUAGE_JP] = "/音声",
                [SESSION_UI_LANGUAGE_ZH] = "/音频",
                [SESSION_UI_LANGUAGE_RU] = "/аудио",
            },
    },
    {
        .canonical = "/files",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/파일",
                [SESSION_UI_LANGUAGE_JP] = "/ファイル",
                [SESSION_UI_LANGUAGE_ZH] = "/文件",
                [SESSION_UI_LANGUAGE_RU] = "/файлы",
            },
    },
    {
        .canonical = "/filestore",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/filestore",
                [SESSION_UI_LANGUAGE_JP] = "/filestore",
                [SESSION_UI_LANGUAGE_ZH] = "/filestore",
                [SESSION_UI_LANGUAGE_RU] = "/filestore",
            },
    },
    {
        .canonical = "/filestore-upload",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/filestore-upload",
                [SESSION_UI_LANGUAGE_JP] = "/filestore-upload",
                [SESSION_UI_LANGUAGE_ZH] = "/filestore-upload",
                [SESSION_UI_LANGUAGE_RU] = "/filestore-upload",
            },
    },
    {
        .canonical = "/filestore-download",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/filestore-download",
                [SESSION_UI_LANGUAGE_JP] = "/filestore-download",
                [SESSION_UI_LANGUAGE_ZH] = "/filestore-download",
                [SESSION_UI_LANGUAGE_RU] = "/filestore-download",
            },
    },
    {
        .canonical = "/mail",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/메일",
                [SESSION_UI_LANGUAGE_JP] = "/メール",
                [SESSION_UI_LANGUAGE_ZH] = "/邮件",
                [SESSION_UI_LANGUAGE_RU] = "/почта",
            },
    },
    {
        .canonical = "/asciiart",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/아스키아트",
                [SESSION_UI_LANGUAGE_JP] = "/アスキーアート",
                [SESSION_UI_LANGUAGE_ZH] = "/艺术",
                [SESSION_UI_LANGUAGE_RU] = "/ASCIIарт",
            },
    },
    {
        .canonical = "/game",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/게임",
                [SESSION_UI_LANGUAGE_JP] = "/ゲーム",
                [SESSION_UI_LANGUAGE_ZH] = "/游戏",
                [SESSION_UI_LANGUAGE_RU] = "/игра",
            },
    },
    {
        .canonical = "/gemini",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/제미니",
                [SESSION_UI_LANGUAGE_JP] = "/ジェミニ",
                [SESSION_UI_LANGUAGE_ZH] = "/Gemini",
                [SESSION_UI_LANGUAGE_RU] = "/джемини",
            },
    },
    {
        .canonical = "/gemini-unfreeze",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/제미니-해제",
                [SESSION_UI_LANGUAGE_JP] = "/ジェミニ-解除",
                [SESSION_UI_LANGUAGE_ZH] = "/Gemini-解冻",
                [SESSION_UI_LANGUAGE_RU] = "/джемини-разморозить",
            },
    },
    {
        .canonical = "/color",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/색상",
                [SESSION_UI_LANGUAGE_JP] = "/色",
                [SESSION_UI_LANGUAGE_ZH] = "/颜色",
                [SESSION_UI_LANGUAGE_RU] = "/цвет",
            },
    },
    {
        .canonical = "/captcha",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/캡차",
                [SESSION_UI_LANGUAGE_JP] = "/キャプチャ",
                [SESSION_UI_LANGUAGE_ZH] = "/验证码",
                [SESSION_UI_LANGUAGE_RU] = "/капча",
            },
    },
    {
        .canonical = "/systemcolor",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/시스템색상",
                [SESSION_UI_LANGUAGE_JP] = "/システムカラー",
                [SESSION_UI_LANGUAGE_ZH] = "/系统颜色",
                [SESSION_UI_LANGUAGE_RU] = "/системныйцвет",
            },
    },
    {
        .canonical = "/set-trans-lang",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/번역언어설정",
                [SESSION_UI_LANGUAGE_JP] = "/翻訳言語設定",
                [SESSION_UI_LANGUAGE_ZH] = "/设置翻译语言",
                [SESSION_UI_LANGUAGE_RU] = "/установить-язык-перевода",
            },
    },
    {
        .canonical = "/set-target-lang",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/대상언어설정",
                [SESSION_UI_LANGUAGE_JP] = "/ターゲット言語設定",
                [SESSION_UI_LANGUAGE_ZH] = "/设置目标语言",
                [SESSION_UI_LANGUAGE_RU] = "/установить-целевой-язык",
            },
    },
    {
        .canonical = "/set-ui-lang",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/UI언어설정",
                [SESSION_UI_LANGUAGE_JP] = "/UI言語設定",
                [SESSION_UI_LANGUAGE_ZH] = "/界面语言设置",
                [SESSION_UI_LANGUAGE_RU] = "/язык-интерфейса",
            },
    },
    {
        .canonical = "/grant",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/권한부여",
            },
    },
    {
        .canonical = "/shell",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/쉘",
            },
    },
    {
        .canonical = "/revoke",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/권한해제",
            },
    },
    {
        .canonical = "/kick",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/강퇴",
                [SESSION_UI_LANGUAGE_JP] = "/キック",
                [SESSION_UI_LANGUAGE_ZH] = "/踢出",
                [SESSION_UI_LANGUAGE_RU] = "/кик",
            },
    },
    {
        .canonical = "/poke",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/호출",
                [SESSION_UI_LANGUAGE_JP] = "/つつく",
                [SESSION_UI_LANGUAGE_ZH] = "/戳",
                [SESSION_UI_LANGUAGE_RU] = "/пинг",
            },
    },
    {
        .canonical = "/weather",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/날씨",
                [SESSION_UI_LANGUAGE_JP] = "/天気",
                [SESSION_UI_LANGUAGE_ZH] = "/天气",
                [SESSION_UI_LANGUAGE_RU] = "/погода",
            },
    },
    {
        .canonical = "/translate",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/번역",
                [SESSION_UI_LANGUAGE_JP] = "/翻訳",
                [SESSION_UI_LANGUAGE_ZH] = "/翻译",
                [SESSION_UI_LANGUAGE_RU] = "/перевод",
            },
    },
    {
        .canonical = "/translate-scope",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/번역범위",
                [SESSION_UI_LANGUAGE_JP] = "/翻訳範囲",
                [SESSION_UI_LANGUAGE_ZH] = "/翻译范围",
                [SESSION_UI_LANGUAGE_RU] = "/область-перевода",
            },
    },
    {
        .canonical = "/chat-spacing",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/채팅간격",
                [SESSION_UI_LANGUAGE_JP] = "/チャット間隔",
                [SESSION_UI_LANGUAGE_ZH] = "/聊天间距",
                [SESSION_UI_LANGUAGE_RU] = "/интервал-чата",
            },
    },
    {
        .canonical = "/palette",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/팔레트",
                [SESSION_UI_LANGUAGE_JP] = "/パレット",
                [SESSION_UI_LANGUAGE_ZH] = "/调色板",
                [SESSION_UI_LANGUAGE_RU] = "/палитра",
            },
    },
    {
        .canonical = "/today",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/오늘",
                [SESSION_UI_LANGUAGE_JP] = "/今日",
                [SESSION_UI_LANGUAGE_ZH] = "/今日",
                [SESSION_UI_LANGUAGE_RU] = "/сегодня",
            },
    },
    {
        .canonical = "/date",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/날짜",
                [SESSION_UI_LANGUAGE_JP] = "/日付",
                [SESSION_UI_LANGUAGE_ZH] = "/日期",
                [SESSION_UI_LANGUAGE_RU] = "/дата",
            },
    },
    {
        .canonical = "/os",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/운영체제",
                [SESSION_UI_LANGUAGE_JP] = "/OS",
                [SESSION_UI_LANGUAGE_ZH] = "/操作系统",
                [SESSION_UI_LANGUAGE_RU] = "/ОС",
            },
    },
    {
        .canonical = "/getos",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/운영체제확인",
                [SESSION_UI_LANGUAGE_JP] = "/OS取得",
                [SESSION_UI_LANGUAGE_ZH] = "/获取操作系统",
                [SESSION_UI_LANGUAGE_RU] = "/получить-ОС",
            },
    },
    {
        .canonical = "/birthday",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/생일",
                [SESSION_UI_LANGUAGE_JP] = "/誕生日",
                [SESSION_UI_LANGUAGE_ZH] = "/生日",
                [SESSION_UI_LANGUAGE_RU] = "/деньрождения",
            },
    },
    {
        .canonical = "/pair",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/짝",
                [SESSION_UI_LANGUAGE_JP] = "/ペア",
                [SESSION_UI_LANGUAGE_ZH] = "/配对",
                [SESSION_UI_LANGUAGE_RU] = "/пара",
            },
    },
    {
        .canonical = "/connected",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/접속자",
                [SESSION_UI_LANGUAGE_JP] = "/接続中",
                [SESSION_UI_LANGUAGE_ZH] = "/在线",
                [SESSION_UI_LANGUAGE_RU] = "/подключенные",
            },
    },
    {
        .canonical = "/users",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/접속자수",
                [SESSION_UI_LANGUAGE_JP] = "/ユーザー数",
                [SESSION_UI_LANGUAGE_ZH] = "/用户数量",
                [SESSION_UI_LANGUAGE_RU] = "/пользователи",
            },
    },
    {
        .canonical = "/pardon",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/사면",
                [SESSION_UI_LANGUAGE_JP] = "/許し",
                [SESSION_UI_LANGUAGE_ZH] = "/赦免",
                [SESSION_UI_LANGUAGE_RU] = "/помиловать",
            },
    },
    {
        .canonical = "/eliza",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/엘리자",
                [SESSION_UI_LANGUAGE_JP] = "/エリザ",
                [SESSION_UI_LANGUAGE_ZH] = "/伊丽莎",
                [SESSION_UI_LANGUAGE_RU] = "/элиза",
            },
    },
    {
        .canonical = "/ai-chat",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/ai-채팅",
                [SESSION_UI_LANGUAGE_JP] = "/aiチャット",
                [SESSION_UI_LANGUAGE_ZH] = "/ai聊天",
                [SESSION_UI_LANGUAGE_RU] = "/ai-чат",
            },
    },
    {
        .canonical = "/ollama-model",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/ollama-모델",
                [SESSION_UI_LANGUAGE_JP] = "/ollamaモデル",
                [SESSION_UI_LANGUAGE_ZH] = "/ollama模型",
                [SESSION_UI_LANGUAGE_RU] = "/ollama-модель",
            },
    },
    {
        .canonical = "/getaddr",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/주소확인",
                [SESSION_UI_LANGUAGE_JP] = "/アドレス取得",
                [SESSION_UI_LANGUAGE_ZH] = "/获取地址",
                [SESSION_UI_LANGUAGE_RU] = "/получить-адрес",
            },
    },
    {
        .canonical = "/alpha-centauri-landers",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/알파센타우리착륙자",
            },
    },
    {
        .canonical = "/block",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/차단",
            },
    },
    {
        .canonical = "/advanced",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/advanced",
            },
    },
    {
        .canonical = "/sync-trigger",
    },
    {
        .canonical = "/set-sync-url",
    },
    {
        .canonical = "/setpw",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/비밀번호설정",
                [SESSION_UI_LANGUAGE_JP] = "/パスワード設定",
                [SESSION_UI_LANGUAGE_ZH] = "/设置密码",
                [SESSION_UI_LANGUAGE_RU] = "/установить-пароль",
            },
    },
};

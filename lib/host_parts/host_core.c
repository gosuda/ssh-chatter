#include "host_internal.h"

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
#define SSH_CHATTER_BBS_EDITOR_BODY_DIVIDER "----------Body---------------"
#define SSH_CHATTER_BBS_EDITOR_END_DIVIDER "----------End-----------------"
#define SSH_CHATTER_RSS_REFRESH_SECONDS 180U
#define SSH_CHATTER_RSS_SLEEP_CHUNK_SECONDS 5U
#define SSH_CHATTER_RSS_USER_AGENT "ssh-chatter/rss"
#define SSH_CHATTER_RSS_BREAKING_PREFIX "[BREAKING NEWS]"
#define SSH_CHATTER_TETROMINO_SIZE 4
#define SSH_CHATTER_HANDSHAKE_RETRY_LIMIT ((unsigned int)INT_MAX)
#define SSH_CHATTER_REQUIRED_HOSTKEY_ALGORITHMS_DISPLAY                        \
    "rsa-sha2-512, rsa-sha2-256, ssh-rsa, ssh-ed25519, ecdsa-sha2-nistp256"
#define SSH_CHATTER_SUPPORTED_KEX_ALGORITHMS                                   \
    "curve25519-sha256,curve25519-sha256@libssh.org,ecdh-sha2-nistp256,ecdh-"  \
    "sha2-nistp384,"                                                           \
    "ecdh-sha2-nistp521,diffie-hellman-group-exchange-sha256,diffie-hellman-"  \
    "group14-sha256,"                                                          \
    "diffie-hellman-group14-sha1"
#define SSH_CHATTER_STRONG_CIPHERS                                             \
    "chacha20-poly1305@openssh.com,aes256-gcm@openssh.com,aes256-ctr"
#define SSH_CHATTER_STRONG_MACS "hmac-sha2-512,hmac-sha2-256"
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
    ssh_bind_options_e option;
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
    pthread_mutex_lock(&host->lock);
    added = host_protected_ip_add_unlocked(host, ip);
    pthread_mutex_unlock(&host->lock);
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
    char *copy = (char *)GC_MALLOC(env_length + 1U);
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

    GC_FREE(copy);
}

static void host_protected_ips_bootstrap(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    const char *defaults[] = {"127.0.0.1", "::1", "192.168.0.1"};

    pthread_mutex_lock(&host->lock);
    for (size_t idx = 0; idx < sizeof(defaults) / sizeof(defaults[0]); ++idx) {
        (void)host_protected_ip_add_unlocked(host, defaults[idx]);
    }
    pthread_mutex_unlock(&host->lock);

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
#define SSH_CHATTER_CLAMAV_SCAN_INTERVAL_SECONDS (5 * 60 * 60)
#define SSH_CHATTER_CLAMAV_SLEEP_CHUNK_SECONDS 30U
#define SSH_CHATTER_BBS_WATCHDOG_SLEEP_SECONDS 5U
#define SSH_CHATTER_CLAMAV_OUTPUT_LIMIT 512U
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

#define TELNET_IAC 255
#define TELNET_CMD_SE 240
#define TELNET_CMD_NOP 241
#define TELNET_CMD_DM 242
#define TELNET_CMD_BREAK 243
#define TELNET_CMD_WILL 251
#define TELNET_CMD_WONT 252
#define TELNET_CMD_DO 253
#define TELNET_CMD_DONT 254
#define TELNET_CMD_SB 250
#define TELNET_OPT_BINARY 0
#define TELNET_OPT_ECHO 1
#define TELNET_OPT_SUPPRESS_GO_AHEAD 3
#define TELNET_OPT_STATUS 5
#define TELNET_OPT_TERMINAL_TYPE 24
#define TELNET_OPT_NAWS 31
#define TELNET_OPT_TERMINAL_SPEED 32
#define TELNET_OPT_LINEMODE 34

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
    {"SSH-2.0-libssh*", "54.0.0.0/7", ""},
    {"SSH-2.0-libssh2*", "40.0.0.0/6", ""},
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
    {"SSH-2.0-", "0.0.0.0/0", ""},
    {"", "0.0.0.0/0", ""},
    {"SSH-2.0-OpenSSH_3.*", "52.0.0.0/8", ""},
    {"SSH-2.0-OpenSSH_4.*", "20.0.0.0/8", ""},
    {"SSH-2.0-OpenSSH_5.*", "34.0.0.0/8", ""},
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
    {"SSH-2.0-libssh*", "139.59.0.0/16", ""},
    {"SSH-2.0-libssh*", "178.62.0.0/16", ""},
    {"SSH-2.0-libssh*", "144.76.0.0/16", ""},
    {"SSH-2.0-libssh*", "199.0.0.0/8", ""},
    {"SSH-2.0-libssh*", "207.0.0.0/8", ""},
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

static bool host_version_ip_rule_matches(const version_ip_ban_rule_t *rule,
                                         const char *version, const char *ip)
{
    if (rule == nullptr || !rule->in_use || ip == nullptr || ip[0] == '\0') {
        return false;
    }

    bool version_match = false;
    switch (rule->match_mode) {
    case VERSION_PATTERN_MATCH_ANY:
        version_match = true;
        break;
    case VERSION_PATTERN_MATCH_EXACT:
        if (version != nullptr) {
            version_match = strcmp(version, rule->normalized_pattern) == 0;
        }
        break;
    case VERSION_PATTERN_MATCH_PREFIX:
        if (version != nullptr) {
            size_t prefix_len = strnlen(rule->normalized_pattern,
                                        sizeof(rule->normalized_pattern));
            version_match =
                (prefix_len > 0U &&
                 strncmp(version, rule->normalized_pattern, prefix_len) == 0);
        }
        break;
    case VERSION_PATTERN_MATCH_SUFFIX:
        if (version != nullptr) {
            size_t suffix_len = strnlen(rule->normalized_pattern,
                                        sizeof(rule->normalized_pattern));
            size_t version_len = strlen(version);
            if (suffix_len > 0U && version_len >= suffix_len) {
                version_match =
                    strncmp(version + (version_len - suffix_len),
                            rule->normalized_pattern, suffix_len) == 0;
            }
        }
        break;
    case VERSION_PATTERN_MATCH_SUBSTRING:
        if (version != nullptr && rule->normalized_pattern[0] != '\0') {
            version_match =
                strstr(version, rule->normalized_pattern) != nullptr;
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
        if (strncmp(existing->normalized_pattern, normalized,
                    sizeof(existing->normalized_pattern)) != 0) {
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
    memset(rule, 0, sizeof(*rule));
    rule->in_use = true;
    rule->match_mode = match_mode;
    snprintf(rule->original_pattern, sizeof(rule->original_pattern), "%s",
             original);
    snprintf(rule->normalized_pattern, sizeof(rule->normalized_pattern), "%s",
             normalized);
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
        rule->original_pattern[0] != '\0' ? rule->original_pattern : "*";
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

    char *copy = strdup(env);
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

    GC_FREE(copy);
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

    host->version_ip_ban_rule_count = 0U;
    memset(host->version_ip_ban_rules, 0, sizeof(host->version_ip_ban_rules));

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
    "⚠️ Translation quota exhausted. Translation features are temporarily "
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
    "ssh-chat-server",
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
    "set-target-lang",
    "set-trans-lang",
    "set-ui-lang",
    "showstatus",
    "soulmate",
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
            .set_ui_lang_usage = "Usage: %sset-ui-lang <ko|en|jp|zh|ru|de|fr|pl>",
            .set_ui_lang_success =
                "UI language set to %s. Use %shelp to review commands.",
            .set_ui_lang_invalid =
                "Unsupported language. Use one of: ko, en, jp, zh, ru, de, fr, pl.",
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
            .set_ui_lang_usage = "사용법: %sset-ui-lang <ko|en|jp|zh|ru|de|fr|pl>",
            .set_ui_lang_success = "UI 언어를 %s로 설정했습니다. %shelp "
                                   "명령으로 목록을 다시 확인하세요.",
            .set_ui_lang_invalid = "지원하지 않는 언어입니다. ko, en, jp, zh, ru, de, fr, pl"
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
            .set_ui_lang_usage = "使い方: %sset-ui-lang <ko|en|jp|zh|ru|de|fr|pl>",
            .set_ui_lang_success = "UI 言語を %s に設定しました。%shelp "
                                   "でコマンドを再確認してください。",
            .set_ui_lang_invalid = "対応していない言語です。ko, en, jp, zh, ru, de, fr, pl"
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
            .set_ui_lang_usage = "用法：%sset-ui-lang <ko|en|jp|zh|ru|de|fr|pl>",
            .set_ui_lang_success =
                "界面语言已切换为 %s。可用 %shelp 重新查看命令。",
            .set_ui_lang_invalid = "不支持的语言，请选择 ko、en、jp、zh、ru、de、fr、pl。",
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
        .canonical = "/profilepic",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/프로필사진",
                [SESSION_UI_LANGUAGE_JP] = "/プロフィール写真",
                [SESSION_UI_LANGUAGE_ZH] = "/头像",
                [SESSION_UI_LANGUAGE_RU] = "/аватар",
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
        .canonical = "/soulmate",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/영혼의단짝",
                [SESSION_UI_LANGUAGE_JP] = "/ソウルメイト",
                [SESSION_UI_LANGUAGE_ZH] = "/灵魂伴侣",
                [SESSION_UI_LANGUAGE_RU] = "/родственнаядуша",
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

static const size_t kSessionCommandAliasCount =
    sizeof(kSessionCommandAliases) / sizeof(kSessionCommandAliases[0]);

typedef struct session_bbs_subcommand_alias {
    const char *canonical;
    const char *localized[SESSION_UI_LANGUAGE_COUNT];
} session_bbs_subcommand_alias_t;

static const session_bbs_subcommand_alias_t kSessionBbsSubcommands[] = {
    {
        .canonical = "list",
        .localized =
            {
                [SESSION_UI_LANGUAGE_EN] = "list",
                [SESSION_UI_LANGUAGE_KO] = "목록",
                [SESSION_UI_LANGUAGE_JP] = "一覧",
                [SESSION_UI_LANGUAGE_ZH] = "列表",
                [SESSION_UI_LANGUAGE_RU] = "список",
            },
    },
    {
        .canonical = "read",
        .localized =
            {
                [SESSION_UI_LANGUAGE_EN] = "read",
                [SESSION_UI_LANGUAGE_KO] = "읽기",
                [SESSION_UI_LANGUAGE_JP] = "閲覧",
                [SESSION_UI_LANGUAGE_ZH] = "阅读",
                [SESSION_UI_LANGUAGE_RU] = "читать",
            },
    },
    {
        .canonical = "topic",
        .localized =
            {
                [SESSION_UI_LANGUAGE_EN] = "topic",
                [SESSION_UI_LANGUAGE_KO] = "주제",
                [SESSION_UI_LANGUAGE_JP] = "トピック",
                [SESSION_UI_LANGUAGE_ZH] = "主题",
                [SESSION_UI_LANGUAGE_RU] = "тема",
            },
    },
    {
        .canonical = "post",
        .localized =
            {
                [SESSION_UI_LANGUAGE_EN] = "post",
                [SESSION_UI_LANGUAGE_KO] = "게시",
                [SESSION_UI_LANGUAGE_JP] = "投稿",
                [SESSION_UI_LANGUAGE_ZH] = "发布",
                [SESSION_UI_LANGUAGE_RU] = "пост",
            },
    },
    {
        .canonical = "edit",
        .localized =
            {
                [SESSION_UI_LANGUAGE_EN] = "edit",
                [SESSION_UI_LANGUAGE_KO] = "수정",
                [SESSION_UI_LANGUAGE_JP] = "編集",
                [SESSION_UI_LANGUAGE_ZH] = "编辑",
                [SESSION_UI_LANGUAGE_RU] = "редакт",
            },
    },
    {
        .canonical = "comment",
        .localized =
            {
                [SESSION_UI_LANGUAGE_EN] = "comment",
                [SESSION_UI_LANGUAGE_KO] = "댓글",
                [SESSION_UI_LANGUAGE_JP] = "コメント",
                [SESSION_UI_LANGUAGE_ZH] = "评论",
                [SESSION_UI_LANGUAGE_RU] = "коммент",
            },
    },
    {
        .canonical = "regen",
        .localized =
            {
                [SESSION_UI_LANGUAGE_EN] = "regen",
                [SESSION_UI_LANGUAGE_KO] = "갱신",
                [SESSION_UI_LANGUAGE_JP] = "再掲",
                [SESSION_UI_LANGUAGE_ZH] = "置顶",
                [SESSION_UI_LANGUAGE_RU] = "поднять",
            },
    },
    {
        .canonical = "delete",
        .localized =
            {
                [SESSION_UI_LANGUAGE_EN] = "delete",
                [SESSION_UI_LANGUAGE_KO] = "삭제",
                [SESSION_UI_LANGUAGE_JP] = "削除",
                [SESSION_UI_LANGUAGE_ZH] = "删除",
                [SESSION_UI_LANGUAGE_RU] = "удалить",
            },
    },
};

static const size_t kSessionBbsSubcommandCount =
    sizeof(kSessionBbsSubcommands) / sizeof(kSessionBbsSubcommands[0]);

static const session_help_entry_t kSessionHelpEssential[] = {
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "help",
        .description =
            {
                "Show essential chat and BBS commands.",
                "채팅과 게시판에 필요한 핵심 명령을 보여줍니다.",
                "チャットと掲示板で必要な基本コマンドを表示します。",
                "显示聊天与公告板所需的核心命令。",
                "Показать основные команды для чата и доски объявлений.",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "advanced",
        .description =
            {
                "List optional and operator commands.",
                "선택 및 운영자 명령을 확인합니다.",
                "補助および運営向けコマンドを一覧表示します。",
                "列出可选命令和管理员命令。",
                "Показать дополнительные и операторские команды.",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "exit",
        .description =
            {
                "Leave the chat.",
                "채팅방을 나갑니다.",
                "チャットを退出します。",
                "离开聊天。",
                "Выйти из чата.",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "nick <name>",
        .description =
            {
                "Change your display name.",
                "표시 이름을 변경합니다.",
                "表示名を変更します。",
                "更改显示名称。",
                "Изменить отображаемое имя.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "닉 <이름>",
                [SESSION_UI_LANGUAGE_JP] = "/ニックネ <ネーム>",
                [SESSION_UI_LANGUAGE_ZH] = "/昵称 <网名>",
                [SESSION_UI_LANGUAGE_RU] = "/ник <имя>",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "setpw <password>",
        .description =
            {
                "Set a password for your nickname.",
                "닉네임에 비밀번호를 설정합니다.",
                "ニックネームにパスワードを設定します。",
                "为你的昵称设置密码。",
                "Установить пароль для вашего ника.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "비밀번호설정 <비밀번호>",
                [SESSION_UI_LANGUAGE_JP] = "パスワード設定 <パスワード>",
                [SESSION_UI_LANGUAGE_ZH] = "设置密码 <密码>",
                [SESSION_UI_LANGUAGE_RU] = "установить-пароль <пароль>",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "delpw [username]",
        .description =
            {
                "Remove your nickname's password, or another user's (operator "
                "only).",
                "닉네임 비밀번호를 제거하거나, 다른 사용자의 비밀번호를 "
                "제거합니다 (운영자 전용).",
                "ニックネームのパスワードを削除、または他のユーザーのパスワード"
                "を削除します（オペレーター専用）。",
                "删除你的昵称密码，或删除其他用户的密码（仅限管理员）。",
                "Удалить пароль вашего ника, или другого пользователя (только "
                "оператор).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "비밀번호삭제 [사용자이름]",
                [SESSION_UI_LANGUAGE_JP] = "パスワード削除 [ユーザー名]",
                [SESSION_UI_LANGUAGE_ZH] = "删除密码 [用户名]",
                [SESSION_UI_LANGUAGE_RU] = "удалить-пароль [имяпользователя]",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "pm <username> <message>",
        .description =
            {
                "Send a private message.",
                "개인 메시지를 보냅니다.",
                "プライベートメッセージを送信します。",
                "发送私信。",
                "Отправить личное сообщение.",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "motd",
        .description =
            {
                "View the message of the day.",
                "공지(MOTD)를 확인합니다.",
                "MOTD（お知らせ）を表示します。",
                "查看每日公告 (MOTD)。",
                "Показать сообщение дня.",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "history [count]",
        .description =
            {
                "Review your recent commands.",
                "최근 실행한 명령을 확인합니다.",
                "最近実行したコマンドを確認します。",
                "查看最近执行的命令。",
                "Просмотреть недавно выполненные команды.",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "status <message|clear>",
        .description =
            {
                "Set your profile status.",
                "프로필 상태를 설정합니다.",
                "プロフィールステータスを設定します。",
                "设置个人状态。",
                "Установить статус профиля.",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "showstatus <username>",
        .description =
            {
                "View someone else's status.",
                "다른 사용자의 상태를 봅니다.",
                "他のユーザーのステータスを確認します。",
                "查看他人的状态。",
                "Посмотреть статус другого пользователя.",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "users",
        .description =
            {
                "Announce the number of connected users.",
                "현재 접속자 수를 알려줍니다.",
                "接続中のユーザー数を知らせます。",
                "公布当前在线人数。",
                "Сообщить количество подключённых пользователей.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "접속자수",
                [SESSION_UI_LANGUAGE_JP] = "/ユーザー数",
                [SESSION_UI_LANGUAGE_ZH] = "/用户数量",
                [SESSION_UI_LANGUAGE_RU] = "/пользователи",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "chat <message-id>",
        .description =
            {
                "Show a past message by ID.",
                "ID로 이전 메시지를 보여줍니다.",
                "ID を指定して過去のメッセージを表示します。",
                "按编号查看历史消息。",
                "Показать прошлое сообщение по идентификатору.",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "reply <message-id|r<reply-id>> <text>",
        .description =
            {
                "Reply to a message or reply.",
                "메시지 또는 답글에 답장합니다.",
                "メッセージまたは返信に返信します。",
                "回复消息或回复链。",
                "Ответить на сообщение или ответ.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "답장 <메시지ID|r<답글ID>> <내용>",
                [SESSION_UI_LANGUAGE_JP] =
                    "返信 <メッセージID|r<返信ID>> <本文>",
                [SESSION_UI_LANGUAGE_ZH] = "回复 <消息ID|r<回复ID>> <内容>",
                [SESSION_UI_LANGUAGE_RU] =
                    "ответ <id сообщения|r<id ответа>> <текст>",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "bbs [list|read|post|comment|regen|delete]",
        .description =
            {
                "Open the bulletin board system (finish %s to post).",
                "게시판을 엽니다 (%s 로 입력을 마칩니다).",
                "掲示板を開きます（投稿は %s で終了）。",
                "打开公告板系统（以 %s 结束提交）。",
                "Открыть доску объявлений (завершайте ввод строкой %s).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] =
                    "게시판 [목록|읽기|게시|댓글|갱신|삭제]",
                [SESSION_UI_LANGUAGE_JP] =
                    "掲示板 [一覧|閲覧|投稿|コメント|再掲|削除]",
                [SESSION_UI_LANGUAGE_ZH] =
                    "公告板 [列表|阅读|发布|评论|置顶|删除]",
                [SESSION_UI_LANGUAGE_RU] =
                    "доска [список|читать|пост|коммент|поднять|удалить]",
            },
        .description_arg_count = 1,
        .description_args = {SESSION_HELP_TEMPLATE_ARG_BBS_TERMINATOR},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "bbs topic read <tag>",
        .description =
            {
                "Show posts under a specific topic.",
                "특정 주제의 게시물을 확인합니다.",
                "特定のトピックに属する投稿を表示します。",
                "查看特定主题下的帖子。",
                "Показать записи выбранной темы.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "게시판 주제 읽기 <태그>",
                [SESSION_UI_LANGUAGE_JP] = "掲示板 トピック 閲覧 <タグ>",
                [SESSION_UI_LANGUAGE_ZH] = "公告板 主题 阅读 <标签>",
                [SESSION_UI_LANGUAGE_RU] = "доска тема читать <тег>",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "game tetris",
        .description =
            {
                "Start a game of Tetris.",
                "테트리스 게임을 시작합니다.",
                "テトリスゲームを開始します。",
                "开始俄罗斯方块游戏。",
                "Начать игру в Тетрис.",
                "Ein Tetris-Spiel starten.",
                "Commencer un jeu de Tetris.",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "game othello",
        .description =
            {
                "Start a game of Othello (Reversi). Place discs with row/col "
                "(e.g., d4).",
                "오셀로(리버시) 게임을 시작합니다. 행/열로 디스크를 배치합니다 "
                "(예: d4).",
                "オセロ（リバーシ）ゲームを開始します。行/"
                "列でディスクを配置します（例：d4）。",
                "开始黑白棋（翻转棋）游戏。用行/列放置棋子（例如：d4）。",
                "Начать игру в Отелло (Реверси). Размещайте диски "
                "строкой/столбцом (например, d4).",
                "Ein Othello (Reversi)-Spiel starten. Platziere Scheiben mit "
                "Zeile/Spalte (z.B. d4).",
                "Commencer un jeu d'Othello (Reversi). Placer des disques avec "
                "ligne/col (ex : d4).",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "mode <chat|command|toggle>",
        .description =
            {
                "Switch between chat mode and command mode.",
                "채팅 모드와 명령 모드를 전환합니다.",
                "チャットモードとコマンドモードを切り替えます。",
                "在聊天模式和命令模式之间切换。",
                "Переключить режим чата и режим команд.",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "set-ui-lang <ko|en|jp|zh|ru|de|fr|pl>",
        .description =
            {
                "Change the interface language.",
                "인터페이스 언어를 변경합니다.",
                "インターフェース言語を変更します。",
                "更改界面语言。",
                "Изменить язык интерфейса.",
                "Schnittstellensprache ändern.",
                "Changer la langue de l'interface.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "UI언어설정 <ko|en|jp|zh|ru|de|fr|pl>",
                [SESSION_UI_LANGUAGE_JP] = "UI言語設定 <ko|en|jp|zh|ru|de|fr|pl>",
                [SESSION_UI_LANGUAGE_ZH] =
                    "设置界面语言 <ko|en|jp|zh|ru|de|fr|pl>",
                [SESSION_UI_LANGUAGE_RU] =
                    "установить-язык-интерфейса <ko|en|jp|zh|ru|de|fr|pl>",
                [SESSION_UI_LANGUAGE_DE] =
                    "ui-sprache-setzen <ko|en|jp|zh|ru|de|fr|pl>",
                [SESSION_UI_LANGUAGE_FR] =
                    "définir-langue-interface <ko|en|jp|zh|ru|de|fr|pl>",
            },
    },
};

static const session_help_entry_t kSessionHelpExtended[] = {
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "search <text>",
        .description =
            {
                "Search for users whose name matches the text.",
                "이름에 텍스트가 포함된 사용자를 찾습니다.",
                "名前にテキストが含まれるユーザーを検索します。",
                "按名称中包含的文字搜索用户。",
                "Найти пользователей, чьи имена содержат указанный текст.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "검색 <텍스트>",
                [SESSION_UI_LANGUAGE_JP] = "検索 <テキスト>",
                [SESSION_UI_LANGUAGE_ZH] = "搜索 <文本>",
                [SESSION_UI_LANGUAGE_RU] = "поиск <текст>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "image <url> [caption]",
        .description =
            {
                "Share an image link.",
                "이미지 링크를 공유합니다.",
                "画像リンクを共有します。",
                "分享图片链接。",
                "Поделиться ссылкой на изображение.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "이미지 <url> [캡션]",
                [SESSION_UI_LANGUAGE_JP] = "画像 <url> [キャプション]",
                [SESSION_UI_LANGUAGE_ZH] = "图片 <url> [标题]",
                [SESSION_UI_LANGUAGE_RU] = "изображение <url> [подпись]",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "video <url> [caption]",
        .description =
            {
                "Share a video link.",
                "영상 링크를 공유합니다.",
                "動画リンクを共有します。",
                "分享视频链接。",
                "Поделиться ссылкой на видео.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "영상 <url> [캡션]",
                [SESSION_UI_LANGUAGE_JP] = "動画 <url> [キャプション]",
                [SESSION_UI_LANGUAGE_ZH] = "视频 <url> [标题]",
                [SESSION_UI_LANGUAGE_RU] = "видео <url> [подпись]",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "audio <url> [caption]",
        .description =
            {
                "Share an audio clip link.",
                "오디오 링크를 공유합니다.",
                "音声クリップのリンクを共有します。",
                "分享音频链接。",
                "Поделиться ссылкой на аудио.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "오디오 <url> [캡션]",
                [SESSION_UI_LANGUAGE_JP] = "音声 <url> [キャプション]",
                [SESSION_UI_LANGUAGE_ZH] = "音频 <url> [标题]",
                [SESSION_UI_LANGUAGE_RU] = "аудио <url> [подпись]",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "files <url> [caption]",
        .description =
            {
                "Share a downloadable file.",
                "다운로드 가능한 파일을 공유합니다.",
                "ダウンロード可能なファイルを共有します。",
                "分享可下载的文件。",
                "Поделиться загружаемым файлом.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "파일 <url> [캡션]",
                [SESSION_UI_LANGUAGE_JP] = "ファイル <url> [キャプション]",
                [SESSION_UI_LANGUAGE_ZH] = "文件 <url> [标题]",
                [SESSION_UI_LANGUAGE_RU] = "файлы <url> [подпись]",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },

    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "mail [inbox|send <user> <message>|clear]",
        .description =
            {
                "Manage your mailbox.",
                "사서함을 관리합니다.",
                "メールボックスを管理します。",
                "管理你的邮箱。",
                "Управлять почтовым ящиком.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] =
                    "메일 [받은편지함|보내기 <사용자> <메시지>|지우기]",
                [SESSION_UI_LANGUAGE_JP] =
                    "メール [受信箱|送信 <ユーザー> <メッセージ>|クリア]",
                [SESSION_UI_LANGUAGE_ZH] =
                    "邮件 [收件箱|发送 <用户> <消息>|清除]",
                [SESSION_UI_LANGUAGE_RU] =
                    "почта [входящие|отправить <пользователь> "
                    "<сообщение>|очистить]",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "profilepic",
        .description =
            {
                "Open the ASCII art profile picture composer.",
                "ASCII 아트 프로필 편집기를 엽니다.",
                "ASCII アートのプロフィール作成ツールを開きます。",
                "打开 ASCII 头像编辑器。",
                "Открыть редактор ASCII-аватаров.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "프로필사진",
                [SESSION_UI_LANGUAGE_JP] = "プロフィール写真",
                [SESSION_UI_LANGUAGE_ZH] = "头像",
                [SESSION_UI_LANGUAGE_RU] = "аватар",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "asciiart",
        .description =
            {
                "Open the ASCII art composer (max 128 lines, 1/10 min per IP).",
                "ASCII 아트 작성기를 엽니다 (최대 128줄, IP당 10분에 1회).",
                "ASCII "
                "アート作成ツールを開きます（最大128行、IPごと10分に1回）。",
                "打开 ASCII 艺术编辑器（最多128行，每个 IP 10 分钟一次）。",
                "Открыть редактор ASCII-арта (до 128 строк, раз в 10 минут на "
                "IP).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "아스키아트",
                [SESSION_UI_LANGUAGE_JP] = "アスキーアート",
                [SESSION_UI_LANGUAGE_ZH] = "艺术",
                [SESSION_UI_LANGUAGE_RU] = "ASCIIарт",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "color (text;highlight[;bold])",
        .description =
            {
                "Style your handle.",
                "사용자 이름 색상을 꾸밉니다.",
                "ハンドル名の配色を設定します。",
                "设置昵称的配色。",
                "Настроить оформление вашего ника.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "색상 (텍스트;하이라이트[;굵게])",
                [SESSION_UI_LANGUAGE_JP] = "色 (テキスト;ハイライト[;太字])",
                [SESSION_UI_LANGUAGE_ZH] = "颜色 (文本;高亮[;粗体])",
                [SESSION_UI_LANGUAGE_RU] = "цвет (текст;выделение[;жирный])",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "systemcolor (fg;background[;highlight][;bold])",
        .description =
            {
                "Customize interface colors (reset with %ssystemcolor reset).",
                "인터페이스 색상을 조정합니다 (%ssystemcolor reset으로 "
                "초기화).",
                "インターフェースの色を調整します（%ssystemcolor reset "
                "で初期化）。",
                "自定义界面颜色（用 %ssystemcolor reset 重置）。",
                "Настроить цвета интерфейса (сброс — %ssystemcolor reset).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] =
                    "시스템색상 (전경;배경[;하이라이트][;굵게])",
                [SESSION_UI_LANGUAGE_JP] =
                    "システムカラー (前景色;背景色[;ハイライト][;太字])",
                [SESSION_UI_LANGUAGE_ZH] = "系统颜色 (前景;背景[;高亮][;粗体])",
                [SESSION_UI_LANGUAGE_RU] =
                    "системныйцвет (передний;фон[;выделение][;жирный])",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "set-trans-lang <language|off>",
        .description =
            {
                "Translate terminal output to a language.",
                "터미널 출력 번역 대상 언어를 지정합니다.",
                "端末出力の翻訳先を指定します。",
                "设置终端输出的翻译语言。",
                "Задать язык для перевода терминальных сообщений.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "번역언어설정 <언어|끄기>",
                [SESSION_UI_LANGUAGE_JP] = "翻訳言語設定 <言語|オフ>",
                [SESSION_UI_LANGUAGE_ZH] = "设置翻译语言 <语言|关闭>",
                [SESSION_UI_LANGUAGE_RU] =
                    "установить-язык-перевода <язык|выкл>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "set-target-lang <language|off>",
        .description =
            {
                "Translate your outgoing messages.",
                "내보내는 메시지를 번역합니다.",
                "自分の送信メッセージを翻訳します。",
                "翻译你发送的消息。",
                "Переводить исходящие сообщения.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "대상언어설정 <언어|끄기>",
                [SESSION_UI_LANGUAGE_JP] = "ターゲット言語設定 <言語|オフ>",
                [SESSION_UI_LANGUAGE_ZH] = "设置目标语言 <语言|关闭>",
                [SESSION_UI_LANGUAGE_RU] =
                    "установить-целевой-язык <язык|выкл>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "weather <region> <city>",
        .description =
            {
                "Show weather for a region and city.",
                "지역과 도시의 날씨를 보여줍니다.",
                "地域と都市の天気を表示します。",
                "显示指定地区和城市的天气。",
                "Показать погоду для региона и города.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "날씨 <지역> <도시>",
                [SESSION_UI_LANGUAGE_JP] = "天気 <地域> <都市>",
                [SESSION_UI_LANGUAGE_ZH] = "天气 <地区> <城市>",
                [SESSION_UI_LANGUAGE_RU] = "погода <регион> <город>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "translate <on|off>",
        .description =
            {
                "Enable or disable translation after configuring languages.",
                "언어를 설정한 후 번역 기능을 켜거나 끕니다.",
                "言語設定後に翻訳機能を有効/無効にします。",
                "在设定语言后开启或关闭翻译。",
                "Включить или отключить перевод после настройки языков.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "번역 <켜기|끄기>",
                [SESSION_UI_LANGUAGE_JP] = "翻訳 <オン|オフ>",
                [SESSION_UI_LANGUAGE_ZH] = "翻译 <开|关>",
                [SESSION_UI_LANGUAGE_RU] = "перевод <вкл|выкл>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "breaking <on|off>",
        .description =
            {
                "Choose whether blue-and-magenta breaking alerts are shown.",
                "파랑 배경 마젠타 속보 알림 표시 여부를 선택합니다.",
                "青地にマゼンタの速報通知を表示するか選択します。",
                "选择是否显示蓝底洋红色的快讯提示。",
                "Выбрать показ синих оповещений о срочных новостях.",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "eliza-chat <message>",
        .description =
            {
                "Chat with the shared Eliza persona.",
                "공유된 엘리자 페르소나와 대화합니다.",
                "共有のエリザ人格と会話します。",
                "与共享的 Eliza 人格聊天。",
                "Пообщаться с общей персоной Элиза.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "엘리자채팅 <메시지>",
                [SESSION_UI_LANGUAGE_JP] = "エリザチャット <メッセージ>",
                [SESSION_UI_LANGUAGE_ZH] = "伊丽莎聊天 <消息>",
                [SESSION_UI_LANGUAGE_RU] = "элиза-чат <сообщение>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "chat-spacing <0-5>",
        .description =
            {
                "Reserve blank lines before translated captions in chat.",
                "번역 자막 앞에 공백 줄을 예약합니다.",
                "翻訳キャプション前に空行を確保します。",
                "在聊天翻译字幕前预留空行。",
                "Резервировать пустые строки перед переводами в чате.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "채팅간격 <0-5>",
                [SESSION_UI_LANGUAGE_JP] = "チャット間隔 <0-5>",
                [SESSION_UI_LANGUAGE_ZH] = "聊天间距 <0-5>",
                [SESSION_UI_LANGUAGE_RU] = "интервал-чата <0-5>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "palette <name>",
        .description =
            {
                "Apply a predefined interface palette (%spalette list).",
                "미리 정의된 팔레트를 적용합니다 (%spalette list 참고).",
                "定義済みの配色を適用します（%spalette list を参照）。",
                "应用预设的界面配色（参见 %spalette list）。",
                "Применить готовую палитру интерфейса (см. %spalette list).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "팔레트 <이름>",
                [SESSION_UI_LANGUAGE_JP] = "パレット <名前>",
                [SESSION_UI_LANGUAGE_ZH] = "调色板 <名称>",
                [SESSION_UI_LANGUAGE_RU] = "палитра <имя>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "today",
        .description =
            {
                "Discover today's function (once per day).",
                "오늘의 기능을 확인합니다 (하루 1회).",
                "本日の機能を確認します（1日1回）。",
                "查看今日功能（每天一次）。",
                "Узнать сегодняшнюю функцию (раз в день).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "오늘",
                [SESSION_UI_LANGUAGE_JP] = "今日",
                [SESSION_UI_LANGUAGE_ZH] = "今日",
                [SESSION_UI_LANGUAGE_RU] = "сегодня",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "date <timezone>",
        .description =
            {
                "View the server time in another timezone.",
                "다른 시간대의 서버 시간을 확인합니다.",
                "別のタイムゾーンでサーバー時刻を表示します。",
                "查看其他时区的服务器时间。",
                "Показать серверное время в другом часовом поясе.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "날짜 <시간대>",
                [SESSION_UI_LANGUAGE_JP] = "日付 <タイムゾーン>",
                [SESSION_UI_LANGUAGE_ZH] = "日期 <时区>",
                [SESSION_UI_LANGUAGE_RU] = "дата <часовойпояс>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "os <name>",
        .description =
            {
                "Record the operating system you use.",
                "사용 중인 운영체제를 기록합니다.",
                "使用中のOSを記録します。",
                "记录你使用的操作系统。",
                "Сохранить информацию о вашей ОС.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "운영체제 <이름>",
                [SESSION_UI_LANGUAGE_JP] = "OS <名前>",
                [SESSION_UI_LANGUAGE_ZH] = "操作系统 <名称>",
                [SESSION_UI_LANGUAGE_RU] = "ОС <имя>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "getos <username>",
        .description =
            {
                "Look up someone else's recorded operating system.",
                "다른 사용자가 기록한 운영체제를 확인합니다.",
                "他のユーザーが登録したOSを確認します。",
                "查看他人记录的操作系统。",
                "Посмотреть, какую ОС указал другой пользователь.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "운영체제확인 <사용자이름>",
                [SESSION_UI_LANGUAGE_JP] = "OS取得 <ユーザー名>",
                [SESSION_UI_LANGUAGE_ZH] = "获取操作系统 <用户名>",
                [SESSION_UI_LANGUAGE_RU] = "получить-ОС <имяпользователя>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "birthday YYYY-MM-DD",
        .description =
            {
                "Register your birthday.",
                "생일을 등록합니다.",
                "誕生日を登録します。",
                "登记你的生日。",
                "Зарегистрировать дату рождения.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "생일 YYYY-MM-DD",
                [SESSION_UI_LANGUAGE_JP] = "誕生日 YYYY-MM-DD",
                [SESSION_UI_LANGUAGE_ZH] = "生日 YYYY-MM-DD",
                [SESSION_UI_LANGUAGE_RU] = "деньрождения ГГГГ-ММ-ДД",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "soulmate",
        .description =
            {
                "List users sharing your birthday.",
                "생일이 같은 사용자를 나열합니다.",
                "同じ誕生日のユーザーを一覧表示します。",
                "列出与你同生日的用户。",
                "Показать пользователей с той же датой рождения.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "영혼의단짝",
                [SESSION_UI_LANGUAGE_JP] = "ソウルメイト",
                [SESSION_UI_LANGUAGE_ZH] = "灵魂伴侣",
                [SESSION_UI_LANGUAGE_RU] = "родственнаядуша",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "pair",
        .description =
            {
                "List users sharing your recorded OS.",
                "등록한 OS가 같은 사용자를 나열합니다.",
                "同じOSを登録したユーザーを表示します。",
                "列出记录的操作系统相同的用户。",
                "Показать пользователей с той же записанной ОС.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "짝",
                [SESSION_UI_LANGUAGE_JP] = "ペア",
                [SESSION_UI_LANGUAGE_ZH] = "配对",
                [SESSION_UI_LANGUAGE_RU] = "пара",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "connected",
        .description =
            {
                "Privately list everyone connected.",
                "현재 접속 중인 사용자 목록을 비공개로 확인합니다.",
                "接続中のユーザーを自分だけに一覧表示します。",
                "私下查看所有在线用户。",
                "Получить приватный список всех подключённых.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "접속자",
                [SESSION_UI_LANGUAGE_JP] = "接続中",
                [SESSION_UI_LANGUAGE_ZH] = "在线",
                [SESSION_UI_LANGUAGE_RU] = "подключенные",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "alpha-centauri-landers",
        .description =
            {
                "View the Immigrants' Flag hall of fame.",
                "Immigrants' Flag 명예의 전당을 확인합니다.",
                "Immigrants' Flag 殿堂を表示します。",
                "查看 Immigrants' Flag 名人堂。",
                "Открыть зал славы Immigrants' Flag.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "알파센타우리착륙자",
                [SESSION_UI_LANGUAGE_JP] = "アルファケンタウリ着陸者",
                [SESSION_UI_LANGUAGE_ZH] = "半人马座阿尔法星登陆者",
                [SESSION_UI_LANGUAGE_RU] = "альфа-центавра-посадочные",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "poll <question>|<option...>",
        .description =
            {
                "Start or view a quick poll.",
                "빠른 투표를 시작하거나 확인합니다.",
                "簡易投票を開始または表示します。",
                "发起或查看快速投票。",
                "Создать или просмотреть быстрый опрос.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "전역투표 <질문>|<옵션...>",
                [SESSION_UI_LANGUAGE_JP] = "投票 <質問>|<選択肢...>",
                [SESSION_UI_LANGUAGE_ZH] = "投票 <问题>|<选项...>",
                [SESSION_UI_LANGUAGE_RU] = "опрос <вопрос>|<вариант...>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "vote <label> <question>|<option...>",
        .description =
            {
                "Start or inspect a named multiple-choice poll (close with "
                "%svote @close <label>).",
                "이름 있는 다중 선택 투표를 시작하거나 확인합니다 (%svote "
                "@close <label> 로 종료).",
                "名前付きの複数選択投票を開始/確認します（終了は %svote @close "
                "<label>）。",
                "发起或查看命名的多选投票（用 %svote @close <label> 结束）。",
                "Создать или просмотреть именованный многовариантный опрос "
                "(закрытие — %svote @close <label>).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "투표 <라벨> <질문>|<옵션...>",
                [SESSION_UI_LANGUAGE_JP] = "投票 <ラベル> <質問>|<選択肢...>",
                [SESSION_UI_LANGUAGE_ZH] = "投票 <标签> <问题>|<选项...>",
                [SESSION_UI_LANGUAGE_RU] =
                    "голосовать <метка> <вопрос>|<вариант...>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "vote-single <label> <question>|<option...>",
        .description =
            {
                "Start or inspect a named single-choice poll.",
                "이름 있는 단일 선택 투표를 시작하거나 확인합니다.",
                "名前付き単一選択投票を開始/確認します。",
                "发起或查看命名的单选投票。",
                "Создать или просмотреть именованный одно вариантный опрос.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "단일투표 <라벨> <질문>|<옵션...>",
                [SESSION_UI_LANGUAGE_JP] =
                    "単一投票 <ラベル> <質問>|<選択肢...>",
                [SESSION_UI_LANGUAGE_ZH] = "单选投票 <标签> <问题>|<选项...>",
                [SESSION_UI_LANGUAGE_RU] =
                    "голосовать-один <метка> <вопрос>|<вариант...>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "elect <label> <choice>",
        .description =
            {
                "Vote in a named poll by label.",
                "라벨로 지정된 투표에 참여합니다.",
                "ラベルを指定して名前付き投票に投票します。",
                "按标签在命名投票中投票。",
                "Проголосовать в именованном опросе по метке.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "선택 <라벨> <선택>",
                [SESSION_UI_LANGUAGE_JP] = "選択 <ラベル> <選択肢>",
                [SESSION_UI_LANGUAGE_ZH] = "选择 <标签> <选项>",
                [SESSION_UI_LANGUAGE_RU] = "выбрать <метка> <выбор>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "poke <username>",
        .description =
            {
                "Send a bell to call a user.",
                "사용자를 호출하는 종소리를 보냅니다.",
                "ユーザーを呼び出すベルを送ります。",
                "向用户发送提醒铃声。",
                "Отправить звуковой сигнал пользователю.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "호출 <사용자이름>",
                [SESSION_UI_LANGUAGE_JP] = "つつく <ユーザー名>",
                [SESSION_UI_LANGUAGE_ZH] = "戳 <用户名>",
                [SESSION_UI_LANGUAGE_RU] = "пинг <имяпользователя>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "block <user|ip>",
        .description =
            {
                "Hide messages from a user or IP locally (%sblock list to "
                "review).",
                "사용자 또는 IP의 메시지를 차단합니다 (%sblock list로 확인).",
                "ユーザーやIPのメッセージをローカルで非表示にします（確認は "
                "%sblock list）。",
                "本地屏蔽某用户或 IP 的消息（用 %sblock list 查看）。",
                "Скрыть сообщения пользователя или IP локально (проверка через "
                "%sblock list).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "차단 <사용자|IP>",
                [SESSION_UI_LANGUAGE_JP] = "ブロック <ユーザー|IP>",
                [SESSION_UI_LANGUAGE_ZH] = "屏蔽 <用户|IP>",
                [SESSION_UI_LANGUAGE_RU] = "заблокировать <пользователь|IP>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "unblock <target|all>",
        .description =
            {
                "Remove a local block entry.",
                "로컬 차단을 해제합니다.",
                "ローカルのブロックを解除します。",
                "解除本地屏蔽。",
                "Удалить локальную блокировку.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "차단해제 <대상|모두>",
                [SESSION_UI_LANGUAGE_JP] = "ブロック解除 <ターゲット|すべて>",
                [SESSION_UI_LANGUAGE_ZH] = "解除屏蔽 <目标|全部>",
                [SESSION_UI_LANGUAGE_RU] = "разблокировать <цель|все>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_FORMATTED,
        .label = "%sgood|%ssad|%scool|%sangry|%schecked|%slove|%swtf <id>",
        .description =
            {
                "React to a message by number.",
                "번호로 메시지에 반응합니다.",
                "番号を指定してメッセージにリアクションします。",
                "按编号为消息添加表情反应。",
                "Реагировать на сообщение по номеру.",
            },
        .label_arg_count = 7U,
        .label_args =
            {
                SESSION_HELP_TEMPLATE_ARG_PREFIX,
                SESSION_HELP_TEMPLATE_ARG_PREFIX,
                SESSION_HELP_TEMPLATE_ARG_PREFIX,
                SESSION_HELP_TEMPLATE_ARG_PREFIX,
                SESSION_HELP_TEMPLATE_ARG_PREFIX,
                SESSION_HELP_TEMPLATE_ARG_PREFIX,
                SESSION_HELP_TEMPLATE_ARG_PREFIX,
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "좋아요|/슬퍼요|/멋져요|/화나요|/"
                                           "확인|/사랑해요|/어쩌라고 <id>",
                [SESSION_UI_LANGUAGE_JP] = "いいね|/かなしい|/クール|/怒り|/"
                                           "確認済み|/愛|/なんだと <id>",
                [SESSION_UI_LANGUAGE_ZH] =
                    "点赞|/难过|/酷|/生气|/已检查|/爱|/搞什么 <id>",
                [SESSION_UI_LANGUAGE_RU] = "класс|/грусть|/круто|/злой|/"
                                           "проверено|/любовь|/чтоэто <id>",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_FORMATTED,
        .label = "%s1 .. %s5",
        .description =
            {
                "Vote for an option in the active poll.",
                "진행 중인 투표에서 항목에 투표합니다.",
                "実施中の投票で候補に投票します。",
                "为当前投票的选项投票。",
                "Проголосовать за вариант в активном опросе.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "/1 .. /5",
                [SESSION_UI_LANGUAGE_JP] = "/1 .. /5",
                [SESSION_UI_LANGUAGE_ZH] = "/1 .. /5",
                [SESSION_UI_LANGUAGE_RU] = "/1 .. /5",
            },
        .label_arg_count = 2U,
        .label_args =
            {
                SESSION_HELP_TEMPLATE_ARG_PREFIX,
                SESSION_HELP_TEMPLATE_ARG_PREFIX,
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "rss list",
        .description =
            {
                "List saved RSS feeds.",
                "저장된 RSS 피드를 나열합니다.",
                "保存された RSS フィードを一覧表示します。",
                "列出已保存的 RSS 源。",
                "Показать сохранённые RSS-ленты.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "rss 목록",
                [SESSION_UI_LANGUAGE_JP] = "rss 一覧",
                [SESSION_UI_LANGUAGE_ZH] = "rss 列表",
                [SESSION_UI_LANGUAGE_RU] = "rss список",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "rss read <tag>",
        .description =
            {
                "Open a saved feed in the inline reader.",
                "저장된 피드를 인라인 리더로 엽니다.",
                "保存されたフィードをインラインリーダーで開きます。",
                "在内嵌阅读器中打开已保存的源。",
                "Открыть сохранённую ленту во встроенном ридере.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "rss 읽기 <태그>",
                [SESSION_UI_LANGUAGE_JP] = "rss 読む <タグ>",
                [SESSION_UI_LANGUAGE_ZH] = "rss 阅读 <标签>",
                [SESSION_UI_LANGUAGE_RU] = "rss читать <тег>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "retro <on [ko|en|jp|zh|ru|de|fr|pl]|off|auto|status>",
        .description =
            {
                "Toggle retro CP437 terminal encoding with optional language.",
                "선택적 언어와 함께 레트로 CP437 터미널 인코딩을 전환합니다.",
                "オプションの言語でレトロ CP437 "
                "ターミナルエンコーディングを切り替えます。",
                "使用可选语言切换复古 CP437 终端编码。",
                "Переключить ретро CP437 кодировку терминала с необязательным "
                "языком.",
                "Retro CP437-Terminalcodierung mit optionaler Sprache "
                "umschalten.",
                "Basculer l'encodage de terminal rétro CP437 avec une langue "
                "optionnelle.",
                "Przełącz kodowanie terminala retro CP437 z opcjonalnym "
                "językiem.",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "suspend!",
        .description =
            {
                "Suspend the active game (Ctrl+Z while playing).",
                "진행 중인 게임을 일시 중단합니다 (플레이 중 Ctrl+Z).",
                "進行中のゲームを一時停止します（プレイ中に Ctrl+Z）。",
                "暂停正在进行的游戏（游戏中按 Ctrl+Z）。",
                "Приостановить активную игру (Ctrl+Z во время игры).",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
};

static const session_help_entry_t kSessionHelpOperator[] = {
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "translate-scope <chat|chat-nohistory|all>",
        .description =
            {
                "Limit translation scope (operators only).",
                "번역 범위를 제한합니다 (운영자 전용).",
                "翻訳対象範囲を制限します（オペレーター専用）。",
                "限制翻译范围（仅限管理员）。",
                "Ограничить область перевода (только для операторов).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "번역범위 <채팅|채팅기록없음|모두>",
                [SESSION_UI_LANGUAGE_JP] =
                    "翻訳範囲 <チャット|チャット履歴なし|すべて>",
                [SESSION_UI_LANGUAGE_ZH] = "翻译范围 <聊天|无聊天记录|全部>",
                [SESSION_UI_LANGUAGE_RU] =
                    "область-перевода <чат|чат-без-истории|все>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "ssh-chat-server "
                 "<set|port|credentials|enable|disable|status|trigger|stop>",
        .description =
            {
                "Configure the ssh-chat integration (operator only).",
                "ssh-chat 연동을 설정합니다 (운영자 전용).",
                "ssh-chat 連携を設定します (運営者専用)。",
                "配置 ssh-chat 集成（仅管理员）。",
                "Настроить интеграцию ssh-chat (только оператор).",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "gemini <on|off>",
        .description =
            {
                "Toggle Gemini provider (operator only).",
                "Gemini 제공자를 전환합니다 (운영자 전용).",
                "Gemini プロバイダーを切り替えます（オペレーター専用）。",
                "切换 Gemini 提供方（仅限管理员）。",
                "Включить/выключить провайдер Gemini (оператор).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "제미니 <켜기|끄기>",
                [SESSION_UI_LANGUAGE_JP] = "ジェミニ <オン|オフ>",
                [SESSION_UI_LANGUAGE_ZH] = "Gemini <开|关>",
                [SESSION_UI_LANGUAGE_RU] = "джемини <вкл|выкл>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "gemini-unfreeze",
        .description =
            {
                "Clear automatic Gemini cooldown (operator only).",
                "Gemini 자동 쿨다운을 해제합니다 (운영자 전용).",
                "Gemini の自動クールダウンを解除します（オペレーター専用）。",
                "清除 Gemini 自动冷却（仅限管理员）。",
                "Снять автоматическую задержку Gemini (оператор).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "제미니-해제",
                [SESSION_UI_LANGUAGE_JP] = "ジェミニ-解除",
                [SESSION_UI_LANGUAGE_ZH] = "Gemini-解冻",
                [SESSION_UI_LANGUAGE_RU] = "джемини-разморозить",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "captcha <on|off>",
        .description =
            {
                "Toggle captcha requirement (operator only).",
                "캡차 요구를 전환합니다 (운영자 전용).",
                "CAPTCHA の必須設定を切り替えます（オペレーター専用）。",
                "切换验证码要求（仅限管理员）。",
                "Включить/выключить требование капчи (оператор).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "캡차 <켜기|끄기>",
                [SESSION_UI_LANGUAGE_JP] = "キャプチャ <オン|オフ>",
                [SESSION_UI_LANGUAGE_ZH] = "验证码 <开|关>",
                [SESSION_UI_LANGUAGE_RU] = "капча <вкл|выкл>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "eliza <on|off>",
        .description =
            {
                "Toggle the Eliza moderator persona (operator only).",
                "엘리자 모더레이터를 전환합니다 (운영자 전용).",
                "Eliza モデレーターを切り替えます（オペレーター専用）。",
                "切换 Eliza 管理员人格（仅限管理员）。",
                "Включить/выключить модератора Элиза (оператор).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "엘리자 <켜기|끄기>",
                [SESSION_UI_LANGUAGE_JP] = "エリザ <オン|オフ>",
                [SESSION_UI_LANGUAGE_ZH] = "伊丽莎 <开|关>",
                [SESSION_UI_LANGUAGE_RU] = "элиза <вкл|выкл>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "grant <ip>",
        .description =
            {
                "Grant operator access to an IP (LAN only).",
                "IP에 운영자 권한을 부여합니다 (LAN 한정).",
                "IP にオペレーター権限を付与します（LAN 限定）。",
                "为 IP 授予管理员权限（仅限局域网）。",
                "Выдать операторские права IP-адресу (только LAN).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "권한부여 <IP>",
                [SESSION_UI_LANGUAGE_JP] = "許可 <IP>",
                [SESSION_UI_LANGUAGE_ZH] = "授予 <IP>",
                [SESSION_UI_LANGUAGE_RU] = "выдать <IP>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "revoke <ip>",
        .description =
            {
                "Revoke an IP's operator access (LAN top admin).",
                "IP의 운영자 권한을 회수합니다 (LAN 최고 관리자).",
                "IP のオペレーター権限を剥奪します（LAN トップ管理者）。",
                "撤销 IP 的管理员权限（局域网最高管理员）。",
                "Отозвать операторские права IP (только старший LAN-админ).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "권한해제 <IP>",
                [SESSION_UI_LANGUAGE_JP] = "取り消し <IP>",
                [SESSION_UI_LANGUAGE_ZH] = "撤销 <IP>",
                [SESSION_UI_LANGUAGE_RU] = "отозвать <IP>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "kick <username>",
        .description =
            {
                "Disconnect a user (operator only).",
                "사용자를 강제로 종료합니다 (운영자 전용).",
                "ユーザーを切断します（オペレーター専用）。",
                "断开某位用户（仅限管理员）。",
                "Отключить пользователя (operator).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "강퇴 <사용자이름>",
                [SESSION_UI_LANGUAGE_JP] = "キック <ユーザー名>",
                [SESSION_UI_LANGUAGE_ZH] = "踢出 <用户名>",
                [SESSION_UI_LANGUAGE_RU] = "кик <имяпользователя>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "ban <username>",
        .description =
            {
                "Ban a user (operator only).",
                "사용자를 차단합니다 (운영자 전용).",
                "ユーザーを追放します（オペレーター専用）。",
                "封禁用户（仅限管理员）。",
                "Забанить пользователя (оператор).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "밴 <사용자이름>",
                [SESSION_UI_LANGUAGE_JP] = "バン <ユーザー名>",
                [SESSION_UI_LANGUAGE_ZH] = "封禁 <用户名>",
                [SESSION_UI_LANGUAGE_RU] = "бан <имяпользователя>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "banname <nickname>",
        .description =
            {
                "Block a nickname (operator only).",
                "닉네임을 차단합니다 (운영자 전용).",
                "ニックネームを禁止します（オペレーター専用）。",
                "屏蔽昵称（仅限管理员）。",
                "Заблокировать ник (оператор).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "닉차단 <닉네임>",
                [SESSION_UI_LANGUAGE_JP] = "ニックネ禁止 <ニックネーム>",
                [SESSION_UI_LANGUAGE_ZH] = "屏蔽昵称 <昵称>",
                [SESSION_UI_LANGUAGE_RU] = "забанить-ник <ник>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "banlist",
        .description =
            {
                "List active bans (operator only).",
                "활성화된 차단 목록을 봅니다 (운영자 전용).",
                "現在のBANを一覧表示します（オペレーター専用）。",
                "列出当前生效的封禁（仅限管理员）。",
                "Показать активные баны (оператор).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "밴목록",
                [SESSION_UI_LANGUAGE_JP] = "バンリスト",
                [SESSION_UI_LANGUAGE_ZH] = "封禁列表",
                [SESSION_UI_LANGUAGE_RU] = "список-банов",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "delete-msg <id|start-end>",
        .description =
            {
                "Remove chat history messages (operator only).",
                "채팅 기록 메시지를 삭제합니다 (운영자 전용).",
                "チャット履歴のメッセージを削除します（オペレーター専用）。",
                "删除聊天记录中的消息（仅限管理员）。",
                "Удалить сообщения из истории чата (operator).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "메시지삭제 <ID|시작-끝>",
                [SESSION_UI_LANGUAGE_JP] = "メッセージ削除 <ID|開始-終了>",
                [SESSION_UI_LANGUAGE_ZH] = "删除消息 <ID|开始-结束>",
                [SESSION_UI_LANGUAGE_RU] =
                    "удалить-сообщение <ID|начало-конец>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "pardon <user|ip>",
        .description =
            {
                "Remove a ban (operator only).",
                "차단을 해제합니다 (운영자 전용).",
                "BAN を解除します（オペレーター専用）。",
                "解除封禁（仅限管理员）。",
                "Снять бан (operator).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "사면 <사용자|IP>",
                [SESSION_UI_LANGUAGE_JP] = "許し <ユーザー|IP>",
                [SESSION_UI_LANGUAGE_ZH] = "赦免 <用户|IP>",
                [SESSION_UI_LANGUAGE_RU] = "помиловать <пользователь|IP>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "rss add <url> <tag>",
        .description =
            {
                "Register a feed (operator only).",
                "피드를 등록합니다 (운영자 전용).",
                "フィードを登録します（オペレーター専用）。",
                "注册新的源（仅限管理员）。",
                "Добавить ленту (оператор).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "rss 추가 <url> <태그>",
                [SESSION_UI_LANGUAGE_JP] = "rss 追加 <url> <タグ>",
                [SESSION_UI_LANGUAGE_ZH] = "rss 添加 <url> <标签>",
                [SESSION_UI_LANGUAGE_RU] = "rss добавить <url> <тег>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "rss del <tag>",
        .description =
            {
                "Delete a feed (operator only).",
                "피드를 삭제합니다 (운영자 전용).",
                "フィードを削除します（オペレーター専用）。",
                "删除源（仅限管理员）。",
                "Удалить ленту (operator).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "rss 삭제 <태그>",
                [SESSION_UI_LANGUAGE_JP] = "rss 削除 <タグ>",
                [SESSION_UI_LANGUAGE_ZH] = "rss 删除 <标签>",
                [SESSION_UI_LANGUAGE_RU] = "rss удалить <тег>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "getaddr <username>",
        .description =
            {
                "Look up a user's last known address (operator only).",
                "사용자의 마지막 접속 주소를 확인합니다 (운영자 전용).",
                "ユーザーの最新の接続アドレスを確認します（オペレーター専用）"
                "。",
                "查看用户最近的连接地址（仅限管理员）。",
                "Посмотреть последний известный адрес пользователя (operator).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "주소확인 <사용자이름>",
                [SESSION_UI_LANGUAGE_JP] = "アドレス取得 <ユーザー名>",
                [SESSION_UI_LANGUAGE_ZH] = "获取地址 <用户名>",
                [SESSION_UI_LANGUAGE_RU] = "получить-адрес <имяпользователя>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "ircserver <status|reconnect|disconnect>",
        .description =
            {
                "Manage IRC relay server connection (operator only).",
                "IRC 릴레이 서버 연결을 관리합니다 (운영자 전용).",
                "IRC リレーサーバー接続を管理します（オペレーター専用）。",
                "管理 IRC 中继服务器连接（仅限管理员）。",
                "Управление подключением к IRC-серверу (оператор).",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "fidonet <status|reconnect|disconnect>",
        .description =
            {
                "Manage FidoNet/Binkp relay connection (operator only).",
                "FidoNet/Binkp 릴레이 연결을 관리합니다 (운영자 전용).",
                "FidoNet/Binkp リレー接続を管理します（オペレーター専用）。",
                "管理 FidoNet/Binkp 中继连接（仅限管理员）。",
                "Управление подключением FidoNet/Binkp (оператор).",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
};

static const int kSessionHelpLabelWidth = 26;

session_ui_language_t session_ui_language_from_code(const char *code)
{
    if (code == nullptr || code[0] == '\0') {
        return SESSION_UI_LANGUAGE_COUNT;
    }

    for (size_t idx = 0; idx < SESSION_UI_LANGUAGE_COUNT; ++idx) {
        if (strcasecmp(code, kSessionUiLanguageCodes[idx]) == 0) {
            return (session_ui_language_t)idx;
        }
    }

    return SESSION_UI_LANGUAGE_COUNT;
}

static const char *session_ui_language_code(session_ui_language_t language)
{
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }
    return kSessionUiLanguageCodes[language];
}

static const char *session_ui_language_name(session_ui_language_t language,
                                            session_ui_language_t locale)
{
    if (locale < 0 || locale >= SESSION_UI_LANGUAGE_COUNT) {
        locale = SESSION_UI_LANGUAGE_KO;
    }
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }
    return kSessionUiLanguageNames[locale][language];
}

static const session_ui_locale_t *
session_ui_get_locale(const session_ctx_t *ctx)
{
    session_ui_language_t language = SESSION_UI_LANGUAGE_KO;
    if (ctx != nullptr) {
        language = ctx->ui_language;
    }
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }

    for (size_t idx = 0; idx < SESSION_UI_LANGUAGE_COUNT; ++idx) {
        if (kSessionUiLocales[idx].language == language) {
            return &kSessionUiLocales[idx];
        }
    }

    return &kSessionUiLocales[SESSION_UI_LANGUAGE_KO];
}

static void session_dispatch_command(session_ctx_t *ctx, const char *line);
static void session_handle_mode(session_ctx_t *ctx, const char *arguments);

static const char *session_command_prefix(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return "/";
    }
    return ctx->input_mode == SESSION_INPUT_MODE_COMMAND ? "" : "/";
}

static bool session_try_localized_command_forward(session_ctx_t *ctx,
                                                  const char *line)
{
    if (ctx == nullptr || line == nullptr) {
        return false;
    }

    const session_ui_locale_t *locale = session_ui_get_locale(ctx);
    const char *command_label =
        (locale != nullptr && locale->mode_label_command != nullptr &&
         locale->mode_label_command[0] != '\0')
            ? locale->mode_label_command
            : "command";

    char localized_prefix[128];
    int prefix_len = snprintf(localized_prefix, sizeof(localized_prefix), "/%s",
                              command_label);
    if (prefix_len <= 0 || (size_t)prefix_len >= sizeof(localized_prefix)) {
        return false;
    }

    if (strncmp(line, localized_prefix, (size_t)prefix_len) != 0) {
        return false;
    }

    const char *remainder = line + prefix_len;
    while (*remainder == ' ' || *remainder == '\t') {
        ++remainder;
    }

    if (*remainder == '\0') {
        ctx->ops->handle_mode(ctx, command_label);
        return true;
    }

    if (*remainder == '/') {
        ctx->ops->dispatch_command(ctx, remainder);
        return true;
    }

    char forwarded[SSH_CHATTER_MAX_INPUT_LEN];
    forwarded[0] = '/';
    size_t copy_len = strnlen(remainder, sizeof(forwarded) - 2U);
    memcpy(&forwarded[1], remainder, copy_len);
    forwarded[copy_len + 1U] = '\0';
    ctx->ops->dispatch_command(ctx, forwarded);
    return true;
}

static session_ui_language_t
session_ui_language_current(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return SESSION_UI_LANGUAGE_KO;
    }
    session_ui_language_t language = ctx->ui_language;
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }
    return language;
}

static const char *
session_asciiart_terminator_for_language(session_ui_language_t language)
{
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }
    const char *terminator = kSessionAsciiartTerminators[language];
    if (terminator != nullptr && terminator[0] != '\0') {
        return terminator;
    }
    const char *fallback = kSessionAsciiartTerminators[SESSION_UI_LANGUAGE_KO];
    return (fallback != nullptr && fallback[0] != '\0')
               ? fallback
               : SSH_CHATTER_ASCIIART_TERMINATOR_EN;
}

static const char *
session_bbs_terminator_for_language(session_ui_language_t language)
{
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }
    const char *terminator = kSessionBbsTerminators[language];
    if (terminator != nullptr && terminator[0] != '\0') {
        return terminator;
    }
    const char *fallback = kSessionBbsTerminators[SESSION_UI_LANGUAGE_KO];
    return (fallback != nullptr && fallback[0] != '\0')
               ? fallback
               : SSH_CHATTER_BBS_TERMINATOR_EN;
}

static const char *session_asciiart_terminator(const session_ctx_t *ctx)
{
    return session_asciiart_terminator_for_language(
        session_ui_language_current(ctx));
}

static const char *session_bbs_terminator(const session_ctx_t *ctx)
{
    return session_bbs_terminator_for_language(
        session_ui_language_current(ctx));
}

static const char *session_editor_terminator(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return SSH_CHATTER_BBS_TERMINATOR_EN;
    }

    if (ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART) {
        return session_asciiart_terminator(ctx);
    }

    return session_bbs_terminator(ctx);
}

static bool session_asciiart_matches_terminator(const char *line)
{
    if (line == nullptr) {
        return false;
    }
    for (size_t idx = 0; idx < SESSION_UI_LANGUAGE_COUNT; ++idx) {
        const char *terminator = session_asciiart_terminator_for_language(
            (session_ui_language_t)idx);
        if (terminator != nullptr && strcmp(line, terminator) == 0) {
            return true;
        }
    }
    return false;
}

static bool session_bbs_matches_terminator(const char *line)
{
    if (line == nullptr) {
        return false;
    }
    for (size_t idx = 0; idx < SESSION_UI_LANGUAGE_COUNT; ++idx) {
        const char *terminator =
            session_bbs_terminator_for_language((session_ui_language_t)idx);
        if (terminator != nullptr && strcmp(line, terminator) == 0) {
            return true;
        }
    }
    return false;
}

static const char *
session_command_alias_preferred_by_canonical(const session_ctx_t *ctx,
                                             const char *canonical);
static void session_send_system_line(session_ctx_t *ctx, const char *message);

static const session_bbs_subcommand_alias_t *
session_bbs_subcommand_lookup(const char *canonical)
{
    if (canonical == nullptr || canonical[0] == '\0') {
        return nullptr;
    }
    for (size_t idx = 0; idx < kSessionBbsSubcommandCount; ++idx) {
        if (strcmp(kSessionBbsSubcommands[idx].canonical, canonical) == 0) {
            return &kSessionBbsSubcommands[idx];
        }
    }
    return nullptr;
}

static const char *
session_bbs_subcommand_localized(const session_bbs_subcommand_alias_t *alias,
                                 session_ui_language_t language)
{
    if (alias == nullptr) {
        return nullptr;
    }
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }
    const char *localized = alias->localized[language];
    if (localized != nullptr && localized[0] != '\0') {
        return localized;
    }
    return nullptr;
}

static const char *session_bbs_subcommand_preferred(const session_ctx_t *ctx,
                                                    const char *canonical)
{
    const session_bbs_subcommand_alias_t *alias =
        session_bbs_subcommand_lookup(canonical);
    if (alias == nullptr) {
        return canonical;
    }
    const char *localized = session_bbs_subcommand_localized(
        alias, session_ui_language_current(ctx));
    if (localized != nullptr) {
        return localized;
    }
    return alias->canonical;
}

static const char *session_bbs_subcommand_canonicalize(const session_ctx_t *ctx,
                                                       const char *command)
{
    if (command == nullptr || command[0] == '\0') {
        return nullptr;
    }

    for (size_t idx = 0; idx < kSessionBbsSubcommandCount; ++idx) {
        if (strcmp(command, kSessionBbsSubcommands[idx].canonical) == 0) {
            return kSessionBbsSubcommands[idx].canonical;
        }
    }

    session_ui_language_t language = session_ui_language_current(ctx);
    for (size_t idx = 0; idx < kSessionBbsSubcommandCount; ++idx) {
        const char *localized = session_bbs_subcommand_localized(
            &kSessionBbsSubcommands[idx], language);
        if (localized != nullptr && strcmp(command, localized) == 0) {
            return kSessionBbsSubcommands[idx].canonical;
        }
    }

    for (size_t idx = 0; idx < kSessionBbsSubcommandCount; ++idx) {
        for (size_t lang = 0; lang < SESSION_UI_LANGUAGE_COUNT; ++lang) {
            const char *localized = session_bbs_subcommand_localized(
                &kSessionBbsSubcommands[idx], (session_ui_language_t)lang);
            if (localized != nullptr && strcmp(command, localized) == 0) {
                return kSessionBbsSubcommands[idx].canonical;
            }
        }
    }

    return nullptr;
}

static void session_bbs_format_usage(session_ctx_t *ctx, const char *canonical,
                                     const char *arguments, char *buffer,
                                     size_t length)
{
    if (buffer == nullptr || length == 0U) {
        return;
    }

    buffer[0] = '\0';
    const char *bbs_command =
        session_command_alias_preferred_by_canonical(ctx, "/bbs");
    if (bbs_command == nullptr || bbs_command[0] == '\0') {
        bbs_command = "/bbs";
    }

    const char *subcommand =
        canonical != nullptr ? session_bbs_subcommand_preferred(ctx, canonical)
                             : nullptr;
    if (subcommand == nullptr || subcommand[0] == '\0') {
        subcommand = canonical != nullptr ? canonical : "";
    }

    const char *args = arguments != nullptr ? arguments : "";
    const char *separator = args[0] != '\0' ? " " : "";

    snprintf(buffer, length, "Usage: %s %s%s%s", bbs_command, subcommand,
             separator, args);
}

static void session_bbs_send_usage(session_ctx_t *ctx, const char *canonical,
                                   const char *arguments)
{
    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_bbs_format_usage(ctx, canonical, arguments, usage, sizeof(usage));
    session_send_system_line(ctx, usage);
}

static const session_command_alias_t *
session_command_alias_lookup(const char *canonical)
{
    if (canonical == nullptr || canonical[0] == '\0') {
        return nullptr;
    }
    for (size_t idx = 0; idx < kSessionCommandAliasCount; ++idx) {
        if (strcmp(kSessionCommandAliases[idx].canonical, canonical) == 0) {
            return &kSessionCommandAliases[idx];
        }
    }
    return nullptr;
}

static const char *
session_command_alias_for_language(const session_command_alias_t *alias,
                                   session_ui_language_t language)
{
    if (alias == nullptr) {
        return nullptr;
    }
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }
    const char *localized = alias->localized[language];
    if (localized != nullptr && localized[0] != '\0') {
        return localized;
    }
    return alias->canonical;
}

static const char *
session_command_alias_preferred(const session_ctx_t *ctx,
                                const session_command_alias_t *alias)
{
    return session_command_alias_for_language(alias,
                                              session_ui_language_current(ctx));
}

static const char *
session_command_alias_preferred_by_canonical(const session_ctx_t *ctx,
                                             const char *canonical)
{
    const session_command_alias_t *alias =
        session_command_alias_lookup(canonical);
    if (alias == nullptr) {
        return canonical;
    }
    return session_command_alias_preferred(ctx, alias);
}

static bool session_parse_command(const char *line, const char *command,
                                  const char **arguments);
static bool
session_parse_localized_command(session_ctx_t *ctx,
                                const session_command_alias_t *alias,
                                const char *line, const char **arguments);

static bool session_parse_command_any(session_ctx_t *ctx, const char *canonical,
                                      const char *line, const char **arguments)
{
    if (canonical == nullptr) {
        return false;
    }
    const session_command_alias_t *alias =
        session_command_alias_lookup(canonical);
    if (alias != nullptr) {
        return session_parse_localized_command(ctx, alias, line, arguments);
    }
    return session_parse_command(line, canonical, arguments);
}

static void session_command_collect_localized_matches(session_ctx_t *ctx,
                                                      const char *prefix,
                                                      const char **matches,
                                                      size_t *match_count,
                                                      size_t max_count)
{
    if (ctx == nullptr || matches == nullptr || match_count == nullptr) {
        return;
    }

    size_t prefix_len = prefix != nullptr ? strlen(prefix) : 0U;

    for (size_t idx = 0; idx < kSessionCommandAliasCount; ++idx) {
        const session_command_alias_t *alias = &kSessionCommandAliases[idx];
        const char *localized = session_command_alias_preferred(ctx, alias);
        if (localized == nullptr || localized[0] == '\0') {
            continue;
        }
        if (strcmp(localized, alias->canonical) == 0) {
            continue;
        }

        const char *name = localized[0] == '/' ? localized + 1 : localized;
        if (name[0] == '\0') {
            continue;
        }

        if (prefix_len > 0U && strncasecmp(name, prefix, prefix_len) != 0) {
            continue;
        }

        bool duplicate = false;
        for (size_t existing = 0; existing < *match_count; ++existing) {
            if (strcmp(matches[existing], name) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        if (*match_count >= max_count) {
            break;
        }
        matches[(*match_count)++] = name;
    }
}

static void session_command_format_usage(session_ctx_t *ctx,
                                         const char *canonical,
                                         const char *fallback, char *buffer,
                                         size_t length)
{
    if (buffer == nullptr || length == 0U) {
        return;
    }

    buffer[0] = '\0';
    if (fallback == nullptr) {
        return;
    }

    if (canonical == nullptr || canonical[0] == '\0') {
        snprintf(buffer, length, "%s", fallback);
        return;
    }

    const char *alias =
        session_command_alias_preferred_by_canonical(ctx, canonical);
    if (alias == nullptr || alias[0] == '\0') {
        alias = canonical;
    }

    const char *prefix = session_command_prefix(ctx);
    if (prefix == nullptr) {
        prefix = "";
    }

    const char *alias_body = alias;
    if (alias_body != nullptr && alias_body[0] == '/') {
        ++alias_body;
    }

    const char *canonical_body = canonical;
    if (canonical_body[0] == '/') {
        ++canonical_body;
    }

    if (alias_body == nullptr || alias_body[0] == '\0') {
        alias_body = canonical_body;
    }

    char replacement[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(replacement, sizeof(replacement), "%s%s", prefix,
             alias_body != nullptr ? alias_body : "");

    const char *source = fallback;
    size_t canonical_len = strlen(canonical);
    size_t out_index = 0U;
    bool replaced = false;

    for (size_t idx = 0U; source[idx] != '\0' && out_index + 1U < length;) {
        if (canonical_len > 0U &&
            strncmp(source + idx, canonical, canonical_len) == 0) {
            size_t repl_len = strnlen(replacement, length - out_index - 1U);
            memcpy(buffer + out_index, replacement, repl_len);
            out_index += repl_len;
            idx += canonical_len;
            replaced = true;
            continue;
        }
        buffer[out_index++] = source[idx++];
    }
    buffer[out_index] = '\0';

    if (replaced) {
        return;
    }

    size_t body_len = canonical_body != nullptr ? strlen(canonical_body) : 0U;
    if (body_len == 0U) {
        snprintf(buffer, length, "%s", fallback);
        return;
    }

    out_index = 0U;
    bool body_replaced = false;
    for (size_t idx = 0U; source[idx] != '\0' && out_index + 1U < length;) {
        if (strncmp(source + idx, canonical_body, body_len) == 0) {
            size_t repl_len = strnlen(replacement, length - out_index - 1U);
            memcpy(buffer + out_index, replacement, repl_len);
            out_index += repl_len;
            idx += body_len;
            body_replaced = true;
            continue;
        }
        buffer[out_index++] = source[idx++];
    }
    buffer[out_index] = '\0';

    if (!body_replaced) {
        snprintf(buffer, length, "%s", fallback);
    }
}

static int session_utf8_display_width(const char *text)
{
    if (text == nullptr) {
        return 0;
    }

    mbstate_t state;
    memset(&state, 0, sizeof(state));

    int width = 0;
    const char *cursor = text;
    size_t remaining = strlen(text);
    while (remaining > 0U) {
        wchar_t wc;
        size_t consumed = mbrtowc(&wc, cursor, remaining, &state);
        if (consumed == (size_t)-1 || consumed == (size_t)-2) {
            ++cursor;
            --remaining;
            memset(&state, 0, sizeof(state));
            width += 1;
            continue;
        }
        if (consumed == 0U) {
            break;
        }

        int char_width = wcwidth(wc);
        if (char_width < 0) {
            char_width = 1;
        }
        width += char_width;
        cursor += consumed;
        remaining -= consumed;
    }

    return width;
}

static void session_send_system_line(session_ctx_t *ctx, const char *message);

static void session_format_template(const char *format, const char *const *args,
                                    size_t arg_count, char *buffer,
                                    size_t length)
{
    if (buffer == nullptr || length == 0U) {
        return;
    }

    buffer[0] = '\0';
    if (format == nullptr) {
        return;
    }

    size_t out_index = 0U;
    size_t arg_index = 0U;
    for (size_t idx = 0U; format[idx] != '\0' && out_index + 1U < length;
         ++idx) {
        if (format[idx] == '%' && format[idx + 1U] == 's') {
            const char *replacement =
                (args != nullptr && arg_index < arg_count &&
                 args[arg_index] != nullptr)
                    ? args[arg_index]
                    : "";
            size_t available = length - out_index - 1U;
            size_t rep_len = strnlen(replacement, available);
            memcpy(buffer + out_index, replacement, rep_len);
            out_index += rep_len;
            ++arg_index;
            ++idx;
            continue;
        }
        if (format[idx] == '%' && format[idx + 1U] == '%') {
            buffer[out_index++] = '%';
            ++idx;
            continue;
        }

        buffer[out_index++] = format[idx];
    }

    buffer[out_index] = '\0';
}

static size_t session_help_collect_arguments(
    session_ctx_t *ctx, const session_help_template_arg_kind_t *kinds,
    size_t kind_count, const char **output, size_t capacity)
{
    if (output == nullptr || capacity == 0U) {
        return 0U;
    }

    size_t produced = 0U;
    const char *prefix = session_command_prefix(ctx);

    if (kind_count == 0U) {
        size_t repeat = capacity < 8U ? capacity : 8U;
        for (size_t idx = 0; idx < repeat; ++idx) {
            output[produced++] = prefix;
        }
        return produced;
    }

    for (size_t idx = 0; idx < kind_count && produced < capacity; ++idx) {
        const char *value = "";
        switch (kinds[idx]) {
        case SESSION_HELP_TEMPLATE_ARG_PREFIX:
            value = prefix;
            break;
        case SESSION_HELP_TEMPLATE_ARG_ASCIIART_TERMINATOR:
            value = session_asciiart_terminator(ctx);
            break;
        case SESSION_HELP_TEMPLATE_ARG_BBS_TERMINATOR:
            value = session_bbs_terminator(ctx);
            break;
        case SESSION_HELP_TEMPLATE_ARG_COMMAND_REPLY:
            value = session_command_alias_preferred_by_canonical(ctx, "/reply");
            break;
        case SESSION_HELP_TEMPLATE_ARG_COMMAND_GOOD:
            value = session_command_alias_preferred_by_canonical(ctx, "/good");
            break;
        case SESSION_HELP_TEMPLATE_ARG_COMMAND_SAD:
            value = session_command_alias_preferred_by_canonical(ctx, "/sad");
            break;
        case SESSION_HELP_TEMPLATE_ARG_COMMAND_WTF:
            value = session_command_alias_preferred_by_canonical(ctx, "/wtf");
            break;
        case SESSION_HELP_TEMPLATE_ARG_COMMAND_COOL:
            value = session_command_alias_preferred_by_canonical(ctx, "/cool");
            break;
        case SESSION_HELP_TEMPLATE_ARG_COMMAND_ANGRY:
            value = session_command_alias_preferred_by_canonical(ctx, "/angry");
            break;
        case SESSION_HELP_TEMPLATE_ARG_COMMAND_CHECKED:
            value =
                session_command_alias_preferred_by_canonical(ctx, "/checked");
            break;
        case SESSION_HELP_TEMPLATE_ARG_COMMAND_LOVE:
            value = session_command_alias_preferred_by_canonical(ctx, "/love");
            break;
        case SESSION_HELP_TEMPLATE_ARG_COMMAND_BBS:
            value = session_command_alias_preferred_by_canonical(ctx, "/bbs");
            break;
        default:
            value = prefix;
            break;
        }
        if (value == nullptr) {
            value = "";
        }
        output[produced++] = value;
    }

    return produced;
}

static void session_format_help_line(session_ctx_t *ctx,
                                     const session_help_entry_t *entry,
                                     const char *description, char *buffer,
                                     size_t length)
{
    if (ctx == nullptr || entry == nullptr || buffer == nullptr ||
        length == 0U) {
        return;
    }

    const size_t language_index = (size_t)session_ui_language_current(ctx);
    const char *label_template = entry->label;
    if (language_index < SESSION_UI_LANGUAGE_COUNT &&
        entry->label_translations[language_index] != nullptr &&
        entry->label_translations[language_index][0] != '\0') {
        label_template = entry->label_translations[language_index];
    }
    if (label_template == nullptr) {
        label_template = "";
    }

    const char *prefix = session_command_prefix(ctx);
    char label[128];
    label[0] = '\0';

    if (entry->kind == SESSION_HELP_ENTRY_COMMAND) {
        snprintf(label, sizeof(label), "%s%s", prefix, label_template);
    } else if (entry->kind == SESSION_HELP_ENTRY_FORMATTED) {
        const char *args[SESSION_HELP_TEMPLATE_ARG_LIMIT];
        size_t arg_count = session_help_collect_arguments(
            ctx, entry->label_args, entry->label_arg_count, args,
            sizeof(args) / sizeof(args[0]));
        session_format_template(label_template, args, arg_count, label,
                                sizeof(label));
    }

    if (entry->kind == SESSION_HELP_ENTRY_TEXT) {
        snprintf(buffer, length, "%s",
                 description != nullptr ? description : "");
        return;
    }

    int display_width = session_utf8_display_width(label);
    if (display_width < 0) {
        display_width = 0;
    }

    int padding = kSessionHelpLabelWidth - display_width;
    if (padding < 1) {
        padding = 1;
    }
    if (padding > 32) {
        padding = 32;
    }

    char padding_buffer[33];
    memset(padding_buffer, ' ', (size_t)padding);
    padding_buffer[padding] = '\0';

    snprintf(buffer, length, "%s%s- %s", label, padding_buffer,
             description != nullptr ? description : "");
}

static void session_format_help_entries_to_buffer(
    session_ctx_t *ctx, const session_help_entry_t *entries, size_t count,
    char *buffer, size_t buffer_length)
{
    if (ctx == nullptr || entries == nullptr || buffer == nullptr ||
        buffer_length == 0U) {
        if (buffer != nullptr && buffer_length > 0U) {
            buffer[0] = '\0';
        }
        return;
    }

    buffer[0] = '\0';
    size_t current_offset = 0U;
    const size_t language_index = (size_t)session_ui_language_current(ctx);

    for (size_t idx = 0; idx < count; ++idx) {
        const session_help_entry_t *entry = &entries[idx];
        const char *format = entry->description[language_index];
        if (format == nullptr || format[0] == '\0') {
            continue;
        }

        char description[SSH_CHATTER_MESSAGE_LIMIT];
        const char *args[SESSION_HELP_TEMPLATE_ARG_LIMIT];
        size_t arg_count = session_help_collect_arguments(
            ctx, entry->description_args, entry->description_arg_count, args,
            sizeof(args) / sizeof(args[0]));
        session_format_template(format, args, arg_count, description,
                                sizeof(description));

        char line_buffer[SSH_CHATTER_MESSAGE_LIMIT];
        if (entry->kind == SESSION_HELP_ENTRY_TEXT) {
            snprintf(line_buffer, sizeof(line_buffer), "%s", description);
        } else {
            session_format_help_line(ctx, entry, description, line_buffer,
                                     sizeof(line_buffer));
        }

        size_t line_len = strnlen(line_buffer, sizeof(line_buffer));
        if (current_offset + line_len + 1U < buffer_length) { // +1 for newline
            memcpy(buffer + current_offset, line_buffer, line_len);
            current_offset += line_len;
            buffer[current_offset++] = '\n';
        } else {
            // Buffer full, stop adding lines
            break;
        }
    }
    buffer[current_offset] = '\0';
}

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

typedef struct {
    char question_en[256];
    char question_ko[256];
    char question_ru[256];
    char question_zh[256];
    char answer[64];
} captcha_prompt_t;

typedef enum {
    CAPTCHA_LANGUAGE_KO = 0,
    CAPTCHA_LANGUAGE_EN,
    CAPTCHA_LANGUAGE_ZH,
    CAPTCHA_LANGUAGE_RU,
    CAPTCHA_LANGUAGE_COUNT,
} captcha_language_t;

typedef enum {
    HOST_SECURITY_SCAN_CLEAN = 0,
    HOST_SECURITY_SCAN_BLOCKED,
    HOST_SECURITY_SCAN_ERROR,
} host_security_scan_result_t;

static unsigned session_prng_next(unsigned *state)
{
    if (state == nullptr) {
        return 0U;
    }

    *state = (*state * 1664525U) + 1013904223U;
    return *state;
}

static void session_fill_digit_sum_prompt(captcha_prompt_t *prompt,
                                          unsigned *state)
{
    if (prompt == nullptr) {
        return;
    }

    unsigned digits_count = 3U;
    if (state != nullptr) {
        digits_count = 2U + (session_prng_next(state) % 2U);
    }
    if (digits_count < 2U) {
        digits_count = 2U;
    }
    if (digits_count > 3U) {
        digits_count = 3U;
    }

    unsigned digits[4] = {0U, 0U, 0U, 0U};
    unsigned sum = 0U;

    if (digits_count == 3U) {
        bool valid = false;
        for (unsigned attempt = 0U; attempt < 16U && !valid; ++attempt) {
            sum = 0U;
            for (unsigned idx = 0U; idx < digits_count; ++idx) {
                unsigned raw = (unsigned)((idx + 1U) % 10U);
                if (state != nullptr) {
                    raw = session_prng_next(state) % 10U;
                }
                digits[idx] = raw;
                sum += raw;
            }
            if (sum < 10U) {
                valid = true;
            }
        }

        if (!valid && sum >= 10U) {
            unsigned overflow = sum - 9U;
            for (int idx = (int)digits_count - 1; idx >= 0 && overflow > 0U;
                 --idx) {
                unsigned current = digits[(size_t)idx];
                unsigned reduction = current > overflow ? overflow : current;
                digits[(size_t)idx] = current - reduction;
                sum -= reduction;
                overflow -= reduction;
            }
            if (sum >= 10U) {
                digits[0] = 3U;
                digits[1] = 3U;
                digits[2] = 3U;
                sum = 9U;
            }
        }
    } else {
        sum = 0U;
        for (unsigned idx = 0U; idx < digits_count; ++idx) {
            unsigned raw = (unsigned)(idx + 1U);
            if (state != nullptr) {
                raw = (session_prng_next(state) % 9U) + 1U;
            } else {
                raw = (raw % 9U) + 1U;
            }
            digits[idx] = raw;
            sum += raw;
        }
    }

    char expression[64];
    expression[0] = '\0';
    size_t written = 0U;
    for (unsigned idx = 0U; idx < digits_count; ++idx) {
        int appended = 0;
        if (idx == 0U) {
            appended =
                snprintf(expression + written, sizeof(expression) - written,
                         "%u", digits[idx]);
        } else {
            appended =
                snprintf(expression + written, sizeof(expression) - written,
                         " + %u", digits[idx]);
        }
        if (appended < 0) {
            expression[sizeof(expression) - 1U] = '\0';
            break;
        }
        size_t appended_size = (size_t)appended;
        if (appended_size >= sizeof(expression) - written) {
            expression[sizeof(expression) - 1U] = '\0';
            break;
        }
        written += appended_size;
    }

    snprintf(prompt->question_en, sizeof(prompt->question_en),
             "Add the digits: %s = ?", expression);
    snprintf(prompt->question_ko, sizeof(prompt->question_ko),
             "다음 숫자들의 합은 얼마인가요? %s = ?", expression);
    snprintf(prompt->question_ru, sizeof(prompt->question_ru),
             "Чему равна сумма цифр: %s = ?", expression);
    snprintf(prompt->question_zh, sizeof(prompt->question_zh),
             "請計算以下數字的總和：%s = ?", expression);
    snprintf(prompt->answer, sizeof(prompt->answer), "%u", sum);
}

static bool string_contains_case_insensitive(const char *haystack,
                                             const char *needle)
{
    if (haystack == nullptr || needle == nullptr || *needle == '\0') {
        return false;
    }

    const size_t haystack_length = strlen(haystack);
    const size_t needle_length = strlen(needle);
    if (needle_length == 0U || haystack_length < needle_length) {
        return false;
    }

    for (size_t idx = 0; idx <= haystack_length - needle_length; ++idx) {
        size_t matched = 0U;
        while (matched < needle_length) {
            const unsigned char hay = (unsigned char)haystack[idx + matched];
            const unsigned char nee = (unsigned char)needle[matched];
            if (tolower(hay) != tolower(nee)) {
                break;
            }
            ++matched;
        }
        if (matched == needle_length) {
            return true;
        }
    }

    return false;
}

static bool session_editor_matches_terminator(const session_ctx_t *ctx,
                                              const char *line)
{
    if (ctx == nullptr) {
        return false;
    }

    if (ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART) {
        return session_asciiart_matches_terminator(line);
    }

    return session_bbs_matches_terminator(line);
}

static bool string_contains_token_case_insensitive(const char *haystack,
                                                   const char *needle)
{
    if (haystack == nullptr || needle == nullptr || *needle == '\0') {
        return false;
    }

    const size_t haystack_length = strlen(haystack);
    const size_t needle_length = strlen(needle);
    if (needle_length == 0U || haystack_length < needle_length) {
        return false;
    }

    for (size_t idx = 0; idx <= haystack_length - needle_length; ++idx) {
        size_t matched = 0U;
        while (matched < needle_length) {
            const unsigned char hay = (unsigned char)haystack[idx + matched];
            const unsigned char nee = (unsigned char)needle[matched];
            if (tolower(hay) != tolower(nee)) {
                break;
            }
            ++matched;
        }

        if (matched != needle_length) {
            continue;
        }

        const bool has_prev = idx > 0U;
        const bool has_next = (idx + needle_length) < haystack_length;
        const unsigned char prev =
            has_prev ? (unsigned char)haystack[idx - 1U] : 0U;
        const unsigned char next =
            has_next ? (unsigned char)haystack[idx + needle_length] : 0U;
        const bool prev_boundary = !has_prev || (!isalnum(prev) && prev != '_');
        const bool next_boundary = !has_next || (!isalnum(next) && next != '_');

        if (prev_boundary && next_boundary) {
            return true;
        }
    }

    return false;
}

static void session_extract_banner_token(const char *banner, char *buffer,
                                         size_t length)
{
    if (buffer == nullptr || length == 0U) {
        return;
    }

    buffer[0] = '\0';
    if (banner == nullptr || *banner == '\0') {
        return;
    }

    size_t idx = 0U;
    while (banner[idx] != '\0' && isspace((unsigned char)banner[idx])) {
        ++idx;
    }

    size_t produced = 0U;
    while (banner[idx] != '\0' && !isspace((unsigned char)banner[idx])) {
        if (produced + 1U >= length) {
            break;
        }
        buffer[produced++] = banner[idx++];
    }

    if (produced == 0U) {
        snprintf(buffer, length, "%.*s", (int)length - 1, banner);
    } else {
        buffer[produced] = '\0';
    }
}

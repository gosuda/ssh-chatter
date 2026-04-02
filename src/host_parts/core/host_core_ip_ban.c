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


/**
 * @file host_security_and_moderation.c
 * @desc File-level documentation for host_security_and_moderation.c, describing its role
 *       in the SSH-Chatter server and providing a consistent header
 *       comment format across C sources.
 * @return None.
 */

// Host security pipeline, moderation workers, and persistence utilities.
#include "../internal.h"

static const uint32_t UI_LANG_STATE_MAGIC = 0x55494c47U; /* 'UILG' */
static const uint32_t UI_LANG_STATE_VERSION = 1U;

typedef struct ui_lang_state_header {
    uint32_t magic;
    uint32_t version;
    uint32_t entry_count;
    uint32_t reserved;
} ui_lang_state_header_t;

typedef struct ui_lang_state_entry {
    char username[SSH_CHATTER_USERNAME_LEN];
    char ip[SSH_CHATTER_IP_LEN];
    char ui_language[SSH_CHATTER_LANG_NAME_LEN];
} ui_lang_state_entry_t;

static void host_security_reset_diagnostic(char *diagnostic,
                                           size_t diagnostic_length)
{
    if (diagnostic != nullptr && diagnostic_length > 0U) {
        diagnostic[0] = '\0';
    }
}

static bool host_security_scan_input_invalid(host_t *host, const char *payload,
                                             size_t length)
{
    return (host == nullptr || payload == nullptr || length == 0U);
}

static bool host_security_moderation_available(host_t *host)
{
    if (!atomic_load(&host->security_filter_enabled)) {
        return false;
    }

    bool ai_active = atomic_load(&host->security_ai_enabled);

    if (!ai_active) {
        atomic_store(&host->security_filter_enabled, false);
        return false;
    }

    return atomic_load(&host->eliza_enabled);
}

static const time_t HOST_HISTORY_RETENTION_SECONDS = 14 * 24 * 60 * 60;

static bool chat_history_entry_is_expired(const chat_history_entry_t *entry,
                                          time_t cutoff)
{
    if (entry == nullptr || cutoff <= 0) {
        return false;
    }

    if (entry->created_at == 0) {
        return false;
    }

    return entry->created_at < cutoff;
}

static size_t host_history_drop_expired_locked(host_t *host, time_t cutoff)
{
    if (host == nullptr || cutoff <= 0 || host->history == nullptr ||
        host->history_count == 0U) {
        return 0U;
    }

    size_t write_idx = 0U;
    size_t removed = 0U;
    for (size_t idx = 0U; idx < host->history_count; ++idx) {
        chat_history_entry_t *entry = &host->history[idx];
        if (chat_history_entry_is_expired(entry, cutoff)) {
            ++removed;
            continue;
        }
        if (write_idx != idx) {
            host->history[write_idx] = *entry;
        }
        ++write_idx;
    }

    if (removed > 0U) {
        for (size_t idx = write_idx; idx < host->history_count; ++idx) {
            memset(&host->history[idx], 0, sizeof(host->history[idx]));
        }
        host->history_count = write_idx;
    }

    return removed;
}

static size_t host_security_copy_sanitized_snippet(char *snippet,
                                                   size_t snippet_size,
                                                   const char *payload,
                                                   size_t length)
{
    size_t copy_length = length;
    if (copy_length >= snippet_size) {
        copy_length = snippet_size - 1U;
    }

    memcpy(snippet, payload, copy_length);
    for (size_t idx = 0U; idx < copy_length; ++idx) {
        unsigned char ch = (unsigned char)snippet[idx];
        if (ch == '\0') {
            copy_length = idx;
            break;
        }
        if (ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') {
            snippet[idx] = ' ';
        }
    }
    snippet[copy_length] = '\0';
    return copy_length;
}

static host_security_scan_result_t host_security_handle_moderation_failure(
    host_t *host, const char *error, char *diagnostic, size_t diagnostic_length)
{
    if (diagnostic != nullptr && diagnostic_length > 0U) {
        if (error != nullptr && error[0] != '\0') {
            snprintf(diagnostic, diagnostic_length, "%s", error);
        } else {
            snprintf(diagnostic, diagnostic_length, "%s",
                     "moderation unavailable");
        }
    }
    host_security_disable_filter(host, "moderation pipeline unavailable");
    return HOST_SECURITY_SCAN_ERROR;
}

static host_security_scan_result_t
host_security_finalize_scan(bool blocked, const char *reason, char *diagnostic,
                            size_t diagnostic_length)
{
    if (!blocked) {
        host_security_reset_diagnostic(diagnostic, diagnostic_length);
        return HOST_SECURITY_SCAN_CLEAN;
    }

    if (diagnostic != nullptr && diagnostic_length > 0U) {
        if (reason != nullptr && reason[0] != '\0') {
            snprintf(diagnostic, diagnostic_length, "%s", reason);
        } else {
            snprintf(diagnostic, diagnostic_length, "%s",
                     "potential intrusion attempt");
        }
    }

    return HOST_SECURITY_SCAN_BLOCKED;
}

static host_security_scan_result_t
host_security_scan_payload(host_t *host, const char *category,
                           const char *payload, size_t length, char *diagnostic,
                           size_t diagnostic_length)
{
    host_security_reset_diagnostic(diagnostic, diagnostic_length);

    if (host_security_scan_input_invalid(host, payload, length)) {
        return HOST_SECURITY_SCAN_CLEAN;
    }

    if (!host_security_moderation_available(host)) {
        return HOST_SECURITY_SCAN_CLEAN;
    }

    char snippet[1024];
    (void)host_security_copy_sanitized_snippet(snippet, sizeof(snippet),
                                               payload, length);

    bool blocked = false;
    char reason[256];
    reason[0] = '\0';

    bool success = translator_moderate_text(category, snippet, &blocked, reason,
                                            sizeof(reason));
    if (!success) {
        const char *error = translator_last_error();
        return host_security_handle_moderation_failure(host, error, diagnostic,
                                                       diagnostic_length);
    }

    return host_security_finalize_scan(blocked, reason, diagnostic,
                                       diagnostic_length);
}

typedef struct host_security_blocked_identity_s {
    const char *label;
    const char *name;
    char resolved_ip[SSH_CHATTER_IP_LEN];
    const char *address;
    const char *register_ip;
} host_security_blocked_identity_t;

static void host_security_blocked_identity_set_label_name(
    host_security_blocked_identity_t *identity, const char *category,
    const char *username)
{
    identity->label =
        (category != nullptr && category[0] != '\0') ? category : "submission";
    identity->name =
        (username != nullptr && username[0] != '\0') ? username : "unknown";
}

static void host_security_blocked_identity_resolve_ip(
    host_security_blocked_identity_t *identity, host_t *host,
    const char *username, const char *ip)
{
    identity->resolved_ip[0] = '\0';

    if (ip != nullptr && ip[0] != '\0' &&
        strncmp(ip, "unknown", SSH_CHATTER_IP_LEN) != 0) {
        snprintf(identity->resolved_ip, sizeof(identity->resolved_ip), "%s",
                 ip);
        return;
    }

    if (host != nullptr && username != nullptr && username[0] != '\0') {
        host_lookup_last_ip(host, username, identity->resolved_ip,
                            sizeof(identity->resolved_ip));
    }
}

static void host_security_blocked_identity_choose_addresses(
    host_security_blocked_identity_t *identity, const char *ip)
{
    if (identity->resolved_ip[0] != '\0') {
        identity->address = identity->resolved_ip;
        identity->register_ip = identity->resolved_ip;
        return;
    }

    if (ip != nullptr && ip[0] != '\0') {
        identity->address = ip;
        if (strncmp(ip, "unknown", SSH_CHATTER_IP_LEN) != 0) {
            identity->register_ip = ip;
            return;
        }
    }

    identity->address = "unknown";
    identity->register_ip = nullptr;
}

static void
host_security_blocked_identity_init(host_security_blocked_identity_t *identity,
                                    host_t *host, const char *category,
                                    const char *username, const char *ip)
{
    host_security_blocked_identity_set_label_name(identity, category, username);
    host_security_blocked_identity_resolve_ip(identity, host, username, ip);
    host_security_blocked_identity_choose_addresses(identity, ip);
}

static const char *host_security_select_diagnostic(const char *diagnostic,
                                                   char *buffer,
                                                   size_t buffer_length)
{
    if (diagnostic != nullptr && diagnostic[0] != '\0') {
        return diagnostic;
    }

    snprintf(buffer, buffer_length, "%s", "suspected intrusion content");
    return buffer;
}

static void
host_security_log_blocked(const host_security_blocked_identity_t *id,
                          const char *diagnostic)
{
    printf("[security] blocked %s from %s: %s\n", id->label, id->name,
           diagnostic);
}

static void host_security_notify_session_blocked(session_ctx_t *session,
                                                 const char *label,
                                                 const char *diagnostic,
                                                 bool post_send)
{
    if (session == nullptr) {
        return;
    }

    char message[512];
    if (post_send) {
        snprintf(message, sizeof(message),
                 "Security filter flagged your %s after delivery: %s", label,
                 diagnostic);
    } else {
        snprintf(message, sizeof(message),
                 "Security filter rejected your %s: %s", label, diagnostic);
    }
    session_send_system_line(session, message);
}

static void host_security_handle_suspicious_activity(
    host_t *host, const host_security_blocked_identity_t *identity,
    session_ctx_t *session)
{
    size_t attempts = 0U;

    if (host != nullptr) {
        const char *register_ip =
            (identity->register_ip != nullptr) ? identity->register_ip : "";
        host_register_suspicious_activity(host, identity->name, register_ip,
                                          &attempts);
    }

    if (attempts > 0U) {
        printf("[security] suspicious payload counter for %s (%s): %zu/%u\n",
               identity->name, identity->address, attempts,
               (unsigned int)SSH_CHATTER_SUSPICIOUS_EVENT_THRESHOLD);
    }

    if (attempts > 0U && session != nullptr) {
        char warning[256];
        snprintf(warning, sizeof(warning),
                 "Suspicious activity detected (%zu/%u).", attempts,
                 (unsigned int)SSH_CHATTER_SUSPICIOUS_EVENT_THRESHOLD);
        session_send_system_line(session, warning);
    }

    if (attempts >= SSH_CHATTER_SUSPICIOUS_EVENT_THRESHOLD) {
        printf("[security] suspicious payload threshold reached for %s (%s)\n",
               identity->name, identity->address);
    }
}

static void host_security_apply_eliza_intervention(session_ctx_t *session,
                                                   const char *content,
                                                   const char *diagnostic)
{
    if (session != nullptr) {
        (void)host_eliza_intervene(session, content, diagnostic, true);
    }
}

static void host_security_process_blocked(host_t *host, const char *category,
                                          const char *diagnostic,
                                          const char *username, const char *ip,
                                          session_ctx_t *session,
                                          bool post_send, const char *content)
{
    host_security_blocked_identity_t identity;
    host_security_blocked_identity_init(&identity, host, category, username,
                                        ip);

    char diagnostic_buffer[256];
    const char *use_diagnostic = host_security_select_diagnostic(
        diagnostic, diagnostic_buffer, sizeof(diagnostic_buffer));

    host_security_log_blocked(&identity, use_diagnostic);
    host_security_notify_session_blocked(session, identity.label,
                                         use_diagnostic, post_send);
    host_security_handle_suspicious_activity(host, &identity, session);
    host_security_apply_eliza_intervention(session, content, use_diagnostic);
}

static void host_security_process_error(host_t *host, const char *category,
                                        const char *diagnostic,
                                        const char *username, const char *ip,
                                        session_ctx_t *session, bool post_send)
{
    (void)host;
    (void)ip;

    const char *label =
        (category != nullptr && category[0] != '\0') ? category : "submission";
    const char *name =
        (username != nullptr && username[0] != '\0') ? username : "unknown";

    if (diagnostic != nullptr && diagnostic[0] != '\0') {
        printf("[security] unable to moderate %s from %s: %s\n", label, name,
               diagnostic);
    } else {
        printf("[security] unable to moderate %s from %s\n", label, name);
    }

    if (session == nullptr) {
        return;
    }

    char message[512];
    if (diagnostic != nullptr && diagnostic[0] != '\0') {
        if (post_send) {
            snprintf(message, sizeof(message),
                     "Security filter could not validate your %s after "
                     "delivery (%s).",
                     label, diagnostic);
        } else {
            snprintf(
                message, sizeof(message),
                "Security filter is unavailable (%s). Please try again later.",
                diagnostic);
        }
    } else {
        if (post_send) {
            snprintf(message, sizeof(message), "%s",
                     "Security filter could not validate your submission after "
                     "delivery. Please try again later.");
        } else {
            snprintf(
                message, sizeof(message), "%s",
                "Security filter could not validate your submission. Please "
                "try again later.");
        }
    }

    session_send_system_line(session, message);
}


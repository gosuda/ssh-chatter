#include "ssh_chatter/ddial_protocol.h"
#include "ssh_chatter/host.h"
#include "ssh_chatter/memory_manager.h"

#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct ddial_session_registry_node {
    struct ddial_session_registry_node *next;
    struct ddial_session *session;
} ddial_session_registry_node_t;

static ddial_session_registry_node_t *g_ddial_sessions = nullptr;
static pthread_mutex_t g_ddial_registry_lock = PTHREAD_MUTEX_INITIALIZER;

/* Defined in server.c but not exported in a header. */
extern void ddial_session_write_raw(struct ddial_session *sess,
                                    const char *data, size_t len);

void host_ddial_register_session(struct ddial_session *sess)
{
    if (sess == nullptr) {
        return;
    }
    ddial_session_registry_node_t *node =
        (ddial_session_registry_node_t *)sshc_gc_calloc(
            1U, sizeof(ddial_session_registry_node_t));
    if (node == nullptr) {
        return;
    }
    node->session = sess;
    pthread_mutex_lock(&g_ddial_registry_lock);
    /* DDial slots are station-local and must be stable while a session is
     * connected.  Reuse the first free slot instead of advertising every
     * local user as #1. */
    uint16_t candidate = 1U;
    for (;;) {
        /* Slots containing the digit 8 or 9 are invalid on strict ddials;
         * skip them so every session lands on a wire-legal line number. */
        if (!ddial_slot_is_valid(candidate)) {
            ++candidate;
            continue;
        }
        bool used = false;
        for (ddial_session_registry_node_t *scan = g_ddial_sessions;
             scan != nullptr; scan = scan->next) {
            if (scan->session != nullptr && scan->session->slot == candidate) {
                used = true;
                break;
            }
        }
        if (!used || candidate == UINT16_MAX) {
            break;
        }
        ++candidate;
    }
    sess->slot = candidate;
    node->next = g_ddial_sessions;
    g_ddial_sessions = node;
    pthread_mutex_unlock(&g_ddial_registry_lock);
}

bool host_ddial_send_private(host_t *host, uint16_t target_slot,
                             uint16_t from_slot, const char *from_handle,
                             const char *message)
{
    if (host == nullptr || target_slot == 0U || from_slot == 0U ||
        from_handle == nullptr || message == nullptr) {
        return false;
    }

    char formatted[SSH_CHATTER_MESSAGE_LIMIT];
    if (!ddial_format_private(formatted, sizeof(formatted), from_slot,
                              from_handle, message)) {
        return false;
    }

    bool delivered = false;
    pthread_mutex_lock(&g_ddial_registry_lock);
    for (ddial_session_registry_node_t *cur = g_ddial_sessions;
         cur != nullptr; cur = cur->next) {
        struct ddial_session *target = cur->session;
        if (target != nullptr && target->owner == host &&
            target->slot == target_slot && target->logged_in) {
            ddial_session_write_raw(target, formatted, strlen(formatted));
            delivered = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_ddial_registry_lock);
    return delivered;
}

void host_ddial_unregister_session(struct ddial_session *sess)
{
    if (sess == nullptr) {
        return;
    }
    pthread_mutex_lock(&g_ddial_registry_lock);
    ddial_session_registry_node_t **prev = &g_ddial_sessions;
    ddial_session_registry_node_t *cur = g_ddial_sessions;
    while (cur != nullptr) {
        if (cur->session == sess) {
            *prev = cur->next;
            sshc_gc_free(cur);
            break;
        }
        prev = &cur->next;
        cur = cur->next;
    }
    pthread_mutex_unlock(&g_ddial_registry_lock);
}

void host_ddial_broadcast_to_sessions(host_t *host, const char *message)
{
    if (message == nullptr || message[0] == '\0') {
        return;
    }
    size_t msg_len = strlen(message);
    pthread_mutex_lock(&g_ddial_registry_lock);
    ddial_session_registry_node_t **prev = &g_ddial_sessions;
    ddial_session_registry_node_t *cur = g_ddial_sessions;
    while (cur != nullptr) {
        bool valid = false;
        struct ddial_session *sess = cur->session;
        if (sess != nullptr) {
            if (sshc_memory_is_valid_gc_pointer(sess) &&
                sshc_pointer_check(sess, sizeof(*sess))) {
                valid = true;
            }
        }
        
        bool write_failed = false;
        if (valid) {
            SSHC_SAFE_BLOCK_BEGIN() {
                ddial_session_write_raw(sess, message, msg_len);
            } SSHC_SAFE_BLOCK_END({
                write_failed = true;
            });
        }
        
        if (!valid || write_failed) {
            printf("[ddial] Detected invalid/crashed session, purging it.\n");
            *prev = cur->next;
            ddial_session_registry_node_t *to_free = cur;
            cur = cur->next;
            if (valid) {
                SSHC_SAFE_BLOCK_BEGIN() {
                    if (sess->fd >= 0) {
                        close(sess->fd);
                        sess->fd = -1;
                    }
                    sess->should_exit = true;
                } SSHC_SAFE_BLOCK_END({
                    // ignore secondary crashes during cleanup
                });
            }
            sshc_gc_free(to_free);
        } else {
            prev = &cur->next;
            cur = cur->next;
        }
    }
    pthread_mutex_unlock(&g_ddial_registry_lock);
    (void)host;
}

void host_ddial_broadcast_system(host_t *host, const char *message)
{
    if (host == nullptr || message == nullptr || message[0] == '\0') {
        return;
    }
    char clean[SSH_CHATTER_MESSAGE_LIMIT];
    ddial_strip_ansi(message, strlen(message), clean, sizeof(clean));
    if (clean[0] == '\0') {
        return;
    }
    char formatted[SSH_CHATTER_MESSAGE_LIMIT + 16];
    int n = snprintf(formatted, sizeof(formatted), "* %s\r\n", clean);
    if (n > 0 && (size_t)n < sizeof(formatted)) {
        host_ddial_broadcast_to_sessions(host, formatted);
    }
}

/* Forward reference defined in server.c (same translation unit). */
extern void ddial_session_write_line(struct ddial_session *sess,
                                     const char *text);

/* Deliver an inbound link private message to the local DDial session that
 * owns target_slot.  The line is delivered in the local display form
 * "P#<from>[T<ch>:<handle>) <msg>".  Returns true when a local session was
 * found. */
bool host_ddial_deliver_private_line(host_t *host, uint16_t target_slot,
                                     const char *display_line)
{
    if (host == nullptr || target_slot == 0U || display_line == nullptr ||
        display_line[0] == '\0') {
        return false;
    }
    char formatted[SSH_CHATTER_MESSAGE_LIMIT + 4];
    int n = snprintf(formatted, sizeof(formatted), "%s\r\n", display_line);
    if (n <= 0 || (size_t)n >= sizeof(formatted)) {
        return false;
    }

    bool delivered = false;
    pthread_mutex_lock(&g_ddial_registry_lock);
    for (ddial_session_registry_node_t *cur = g_ddial_sessions;
         cur != nullptr; cur = cur->next) {
        struct ddial_session *sess = cur->session;
        if (sess == nullptr || sess->owner != host) {
            continue;
        }
        if (!sshc_memory_is_valid_gc_pointer(sess) ||
            !sshc_pointer_check(sess, sizeof(*sess))) {
            continue;
        }
        if (sess->slot == target_slot && sess->logged_in) {
            SSHC_SAFE_BLOCK_BEGIN() {
                ddial_session_write_raw(sess, formatted, (size_t)n);
            } SSHC_SAFE_BLOCK_END({
                /* delivery failure: leave delivered=false */
            });
            delivered = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_ddial_registry_lock);
    return delivered;
}

/* Build the "}}}-.<station>^#<slot>..." user broadcast list from the local
 * DDial session registry and send it upstream over the link.  Stations send
 * this list every 10-20 minutes so peers can track who is online. */
bool host_ddial_client_send_station_broadcast(host_t *host)
{
    if (host == nullptr) {
        return false;
    }

    const char *station_env = getenv("CHATTER_DDIAL_STATION");
    char station[DDIAL_MAX_HANDLE_LEN];
    if (station_env != nullptr && station_env[0] != '\0') {
        ddial_sanitize_handle(station_env, strlen(station_env), station,
                              sizeof(station));
    } else {
        snprintf(station, sizeof(station), "%s", "Chatter");
    }

    char list[SSH_CHATTER_MESSAGE_LIMIT];
    if (!ddial_format_broadcast_header(list, sizeof(list), station, false)) {
        return false;
    }

    size_t used = strlen(list);
    pthread_mutex_lock(&g_ddial_registry_lock);
    for (ddial_session_registry_node_t *cur = g_ddial_sessions;
         cur != nullptr; cur = cur->next) {
        struct ddial_session *sess = cur->session;
        if (sess == nullptr || sess->owner != host) {
            continue;
        }
        bool valid = sshc_memory_is_valid_gc_pointer(sess) &&
                     sshc_pointer_check(sess, sizeof(*sess));
        if (!valid || !sess->logged_in || sess->handle[0] == '\0') {
            continue;
        }
        char entry[128];
        if (!ddial_format_broadcast_entry(entry, sizeof(entry), sess->slot,
                                          sess->channel, DDIAL_TIER_PASSWORD,
                                          sess->handle, 0U, false)) {
            continue;
        }
        size_t entry_len = strlen(entry);
        if (used + entry_len + 3U > sizeof(list)) {
            break;
        }
        memcpy(list + used, entry, entry_len);
        used += entry_len;
    }
    pthread_mutex_unlock(&g_ddial_registry_lock);

    list[used++] = '\r';
    list[used++] = '\n';
    list[used] = '\0';
    return host_ddial_client_send_raw(host, list, used);
}

void host_ddial_write_who(ddial_session_t *target)
{
    if (target == nullptr) {
        return;
    }
    pthread_mutex_lock(&g_ddial_registry_lock);
    ddial_session_registry_node_t **prev = &g_ddial_sessions;
    ddial_session_registry_node_t *cur = g_ddial_sessions;
    while (cur != nullptr) {
        bool valid = false;
        struct ddial_session *sess = cur->session;
        if (sess != nullptr) {
            if (sshc_memory_is_valid_gc_pointer(sess) &&
                sshc_pointer_check(sess, sizeof(*sess))) {
                valid = true;
            }
        }
        
        bool write_failed = false;
        if (valid) {
            if (sess != target && sess->handle[0] != '\0') {
                SSHC_SAFE_BLOCK_BEGIN() {
                    ddial_session_write_line(target, sess->handle);
                } SSHC_SAFE_BLOCK_END({
                    write_failed = true;
                });
            }
        }
        
        if (!valid || write_failed) {
            printf("[ddial] Detected invalid/crashed session during who, purging it.\n");
            *prev = cur->next;
            ddial_session_registry_node_t *to_free = cur;
            cur = cur->next;
            if (valid) {
                SSHC_SAFE_BLOCK_BEGIN() {
                    if (sess->fd >= 0) {
                        close(sess->fd);
                        sess->fd = -1;
                    }
                    sess->should_exit = true;
                } SSHC_SAFE_BLOCK_END({
                    // ignore secondary crashes during cleanup
                });
            }
            sshc_gc_free(to_free);
        } else {
            prev = &cur->next;
            cur = cur->next;
        }
    }
    pthread_mutex_unlock(&g_ddial_registry_lock);
}

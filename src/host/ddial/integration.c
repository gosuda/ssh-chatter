/**
 * @file ddial_integration.c
 * @desc Integration between Chatter's internal broadcast pipeline and the
 *       Diversi Dial server sessions.
 */

#include "ssh_chatter/ddial_protocol.h"
#include "ssh_chatter/host.h"

#include <pthread.h>
#include <string.h>

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
    node->next = g_ddial_sessions;
    g_ddial_sessions = node;
    pthread_mutex_unlock(&g_ddial_registry_lock);
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
    ddial_session_registry_node_t *cur = g_ddial_sessions;
    while (cur != nullptr) {
        if (cur->session != nullptr) {
            ddial_session_write_raw(cur->session, message, msg_len);
        }
        cur = cur->next;
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

void host_ddial_write_who(ddial_session_t *target)
{
    if (target == nullptr) {
        return;
    }
    pthread_mutex_lock(&g_ddial_registry_lock);
    ddial_session_registry_node_t *cur = g_ddial_sessions;
    while (cur != nullptr) {
        if (cur->session != nullptr && cur->session != target &&
            cur->session->handle[0] != '\0') {
            ddial_session_write_line(target, cur->session->handle);
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&g_ddial_registry_lock);
}

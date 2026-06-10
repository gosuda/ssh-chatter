/**
 * @file ddial_inject.c
 * @desc Bridge Chatter-formatted messages into the internal Diversi Dial
 *       engine so they can be sent to -DT clients and upstream DDial nodes.
 */

#include "ssh_chatter/ddial_protocol.h"
#include "ssh_chatter/host.h"

#include <stdbool.h>
#include <string.h>

static _Thread_local bool g_ddial_injecting = false;

void host_ddial_inject_message(host_t *host, const char *username,
                               const char *message)
{
    if (host == nullptr || username == nullptr || message == nullptr) {
        return;
    }

    /* Avoid recursion if the message originated from the DDial engine. */
    if (host->ddial_listener.enabled == false &&
        host->ddial_relay.enabled == false) {
        return;
    }

    if (g_ddial_injecting) {
        return;
    }
    g_ddial_injecting = true;

    char formatted[SSH_CHATTER_MESSAGE_LIMIT];
    if (!ddial_format_chat(formatted, sizeof(formatted), 1U,
                           DDIAL_DEFAULT_CHANNEL, DDIAL_TIER_GUEST, username,
                           message)) {
        g_ddial_injecting = false;
        return;
    }

    /* Upstream relay. */
    host_ddial_client_send(host, username, message);

    /* Local -DT clients. */
    host_ddial_broadcast_to_sessions(host, formatted);

    g_ddial_injecting = false;
}

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

    /* DDial is a line-oriented protocol: only the first line that contains
     * the user's sentence should be relayed.  Cut at the first '\n'. */
    size_t msg_len = strlen(message);
    const char *newline = memchr(message, '\n', msg_len);
    size_t line_len =
        newline != nullptr ? (size_t)(newline - message) : msg_len;
    while (line_len > 0U && message[line_len - 1U] == '\r') {
        --line_len;
    }
    if (line_len == 0U || line_len >= SSH_CHATTER_MESSAGE_LIMIT) {
        g_ddial_injecting = false;
        return;
    }
    char single_line[SSH_CHATTER_MESSAGE_LIMIT];
    memcpy(single_line, message, line_len);
    single_line[line_len] = '\0';

    char formatted[SSH_CHATTER_MESSAGE_LIMIT];
    if (!ddial_format_chat(formatted, sizeof(formatted), 1U,
                           DDIAL_DEFAULT_CHANNEL, DDIAL_TIER_GUEST, username,
                           single_line)) {
        g_ddial_injecting = false;
        return;
    }

    /* Upstream relay. */
    host_ddial_client_send(host, username, single_line);

    /* Local -DT clients. */
    host_ddial_broadcast_to_sessions(host, formatted);

    g_ddial_injecting = false;
}

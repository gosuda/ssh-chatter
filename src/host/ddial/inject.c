/**
 * @file ddial_inject.c
 * @desc Bridge Chatter-formatted messages into the internal Diversi Dial
 *       engine for local -DT clients.  Upstream relay is handled directly
 *       from the chat broadcast path so regular text is written raw over
 *       the upstream telnet socket.
 */

#include "ssh_chatter/ddial_protocol.h"
#include "ssh_chatter/host.h"

#include <stdbool.h>
#include <string.h>

void host_ddial_inject_message(host_t *host, const char *username,
                               const char *message)
{
    if (host == nullptr || username == nullptr || message == nullptr) {
        return;
    }

    if (host->ddial_listener.enabled == false) {
        return;
    }

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
        return;
    }
    char single_line[SSH_CHATTER_MESSAGE_LIMIT];
    memcpy(single_line, message, line_len);
    single_line[line_len] = '\0';

    /* Chatter members speak on channel 1 from their chat-link line number,
     * so dial-ins can /ig, /null or /p them like anyone else. */
    uint16_t slot = host_ddial_chat_link_slot_of(host, username);
    char formatted[SSH_CHATTER_MESSAGE_LIMIT];
    if (!ddial_format_chat(formatted, sizeof(formatted), slot,
                           DDIAL_DEFAULT_CHANNEL, DDIAL_TIER_PASSWORD,
                           username, single_line)) {
        return;
    }
    formatted[strcspn(formatted, "\r\n")] = '\0';

    ddial_mv_public_t pub = {0};
    pub.channel = DDIAL_DEFAULT_CHANNEL;
    pub.from_slot = slot;
    pub.line = formatted;
    pub.plain_body = single_line;
    host_ddial_deliver_public(host, &pub);
}

#ifndef SSH_CHATTER_MORSE_CLIENT_H
#define SSH_CHATTER_MORSE_CLIENT_H

#include <stdbool.h>

#include "ssh_chatter/host.h"

typedef struct morse_client morse_client_t;

morse_client_t *morse_client_create(host_t *host);
void morse_client_destroy(morse_client_t *client);
bool morse_client_send(morse_client_t *client, const char *line);
bool morse_client_connected(const morse_client_t *client);

#endif // SSH_CHATTER_MORSE_CLIENT_H

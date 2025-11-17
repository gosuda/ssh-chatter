#ifndef SSH_CHATTER_IRC_CLIENT_H
#define SSH_CHATTER_IRC_CLIENT_H

#include <stdbool.h>

struct host;

typedef struct irc_client irc_client_t;

// Create a new IRC (Multi Relay Chat) client
irc_client_t *irc_client_create(struct host *host);

// Destroy the IRC client
void irc_client_destroy(irc_client_t *client);

// Check if IRC client is connected
bool irc_client_is_connected(irc_client_t *client);

// Get connection status message
const char *irc_client_get_status(irc_client_t *client);

// Reconnect to IRC server
bool irc_client_reconnect(irc_client_t *client);

// Disconnect from IRC server
void irc_client_disconnect(irc_client_t *client);

#endif

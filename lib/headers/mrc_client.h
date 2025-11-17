#ifndef SSH_CHATTER_MRC_CLIENT_H
#define SSH_CHATTER_MRC_CLIENT_H

#include <stdbool.h>

struct host;

typedef struct mrc_client mrc_client_t;

// Create a new MRC (Multi Relay Chat) client
mrc_client_t *mrc_client_create(struct host *host);

// Destroy the MRC client
void mrc_client_destroy(mrc_client_t *client);

// Check if MRC client is connected
bool mrc_client_is_connected(mrc_client_t *client);

// Get connection status message
const char *mrc_client_get_status(mrc_client_t *client);

// Reconnect to MRC server
bool mrc_client_reconnect(mrc_client_t *client);

// Disconnect from MRC server
void mrc_client_disconnect(mrc_client_t *client);

#endif

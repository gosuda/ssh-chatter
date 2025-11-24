#ifndef SSH_CHATTER_FIDONET_CLIENT_H
#define SSH_CHATTER_FIDONET_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

struct host;
struct client_manager;

typedef struct fidonet_client fidonet_client_t;

/**
 * Create a new FidoNet/Binkp client
 * 
 * @param host Pointer to host structure
 * @param manager Pointer to client manager
 * @return New FidoNet client instance or NULL on failure
 */
fidonet_client_t *fidonet_client_create(struct host *host,
                                        struct client_manager *manager);

/**
 * Destroy a FidoNet client
 * 
 * @param client FidoNet client to destroy
 */
void fidonet_client_destroy(fidonet_client_t *client);

/**
 * Check if FidoNet client is connected
 * 
 * @param client FidoNet client to check
 * @return true if connected, false otherwise
 */
bool fidonet_client_is_connected(fidonet_client_t *client);

/**
 * Get FidoNet client status message
 * 
 * @param client FidoNet client
 * @return Status string
 */
const char *fidonet_client_get_status(fidonet_client_t *client);

/**
 * Reconnect FidoNet client
 * 
 * @param client FidoNet client to reconnect
 * @return true if successful, false otherwise
 */
bool fidonet_client_reconnect(fidonet_client_t *client);

/**
 * Disconnect FidoNet client
 * 
 * @param client FidoNet client to disconnect
 */
void fidonet_client_disconnect(fidonet_client_t *client);

/**
 * Send a message via FidoNet
 * 
 * @param client FidoNet client
 * @param username Username of sender
 * @param message Message content
 * @return true if successful, false otherwise
 */
bool fidonet_client_send_message(fidonet_client_t *client, const char *username,
                                 const char *message);

#define FIDONET_LOG_ENTRY_LENGTH 256
#define FIDONET_LOG_CAPACITY 128
bool fidonet_client_snapshot_logs(fidonet_client_t *client,
                                  char entries[][FIDONET_LOG_ENTRY_LENGTH],
                                  size_t capacity, size_t *count);

#endif /* SSH_CHATTER_FIDONET_CLIENT_H */

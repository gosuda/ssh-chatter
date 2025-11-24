#ifndef SSH_CHATTER_WEBSSH_CLIENT_H
#define SSH_CHATTER_WEBSSH_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

struct host;
struct client_manager;
struct chat_history_entry;

typedef struct webssh_client webssh_client_t;

webssh_client_t *webssh_client_create(struct host *host,
                                      struct client_manager *manager);
void webssh_client_destroy(webssh_client_t *client);
size_t webssh_client_snapshot(webssh_client_t *client,
                              struct chat_history_entry *buffer,
                              size_t capacity);
bool webssh_client_send_message(webssh_client_t *client, const char *username,
                                const char *message);

#endif

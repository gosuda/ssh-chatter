#ifndef SSH_CHATTER_DISCORD_CLIENT_H
#define SSH_CHATTER_DISCORD_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

struct host;
struct client_manager;

typedef struct discord_client discord_client_t;

discord_client_t *discord_client_create(struct host *host,
                                        struct client_manager *manager);
void discord_client_destroy(discord_client_t *client);

bool discord_client_is_running(const discord_client_t *client);
void discord_client_status(const discord_client_t *client, char *buffer,
                           size_t length);

#endif

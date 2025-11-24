#ifndef SSH_CHATTER_CLIENT_H
#define SSH_CHATTER_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

struct host;
struct chat_history_entry;

typedef enum client_kind {
    CLIENT_KIND_UNKNOWN = 0,
    CLIENT_KIND_WEBSSH,
    CLIENT_KIND_BOT,
    CLIENT_KIND_AI,
} client_kind_t;

#define CLIENT_IDENTIFIER_LEN 64

typedef struct client_manager client_manager_t;

typedef struct client_connection {
    client_kind_t kind;
    char identifier[CLIENT_IDENTIFIER_LEN];
    bool receive_system_messages;
    bool active;
    void (*on_message)(struct client_connection *connection,
                       const struct chat_history_entry *entry);
    void (*on_detach)(struct client_connection *connection);
    void *user_data;
    client_manager_t *owner;
} client_connection_t;

client_manager_t *client_manager_create(struct host *host);
void client_manager_destroy(client_manager_t *manager);

bool client_manager_register(client_manager_t *manager,
                             client_connection_t *connection);
void client_manager_unregister(client_manager_t *manager,
                               client_connection_t *connection);

void client_manager_notify_history(client_manager_t *manager,
                                   const struct chat_history_entry *entry);

struct host *client_manager_host(client_manager_t *manager);

#endif

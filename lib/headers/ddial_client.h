#ifndef SSH_CHATTER_DDIAL_CLIENT_H
#define SSH_CHATTER_DDIAL_CLIENT_H

#include <stdbool.h>

struct host;
typedef struct client_manager client_manager_t;

typedef struct ddial_client ddial_client_t;

ddial_client_t *ddial_client_create(struct host *host, client_manager_t *manager);
void ddial_client_destroy(ddial_client_t *client);

bool ddial_client_connect(ddial_client_t *client, const char *host, const char *port);
void ddial_client_disconnect(ddial_client_t *client);

const char *ddial_client_get_status(ddial_client_t *client);
bool ddial_client_is_connected(ddial_client_t *client);

#endif

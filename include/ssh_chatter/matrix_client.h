#ifndef SSH_CHATTER_MATRIX_CLIENT_H
#define SSH_CHATTER_MATRIX_CLIENT_H

#include <stdbool.h>

struct host;
struct client_manager;
struct security_layer;

typedef struct matrix_client matrix_client_t;

matrix_client_t *matrix_client_create(struct host *host,
                                      struct client_manager *manager,
                                      struct security_layer *security);
void matrix_client_destroy(matrix_client_t *client);

#endif

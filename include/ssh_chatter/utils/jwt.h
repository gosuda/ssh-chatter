#ifndef SSH_CHATTER_JWT_H
#define SSH_CHATTER_JWT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Generates a JWT (HS256) for the given username.
// Returns a malloc'd string that must be freed by the caller.
char *jwt_generate(const char *secret, const char *username, int64_t expiry_seconds);

// Verifies a JWT (HS256) and extracts the username (sub claim).
// Returns true if valid, false otherwise.
// If valid, *username_out is set to a malloc'd string (caller must free).
bool jwt_verify(const char *secret, const char *token, char **username_out);

#endif

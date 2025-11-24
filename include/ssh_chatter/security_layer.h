#ifndef SSH_CHATTER_SECURITY_LAYER_H
#define SSH_CHATTER_SECURITY_LAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SECURITY_LAYER_LEVELS 3
#define SECURITY_LAYER_IV_LEN 12
#define SECURITY_LAYER_TAG_LEN 16
#define SECURITY_LAYER_SALT_LEN 16
#define SECURITY_LAYER_HASH_LEN 32

typedef struct security_layer {
    unsigned char master_key[32];
    unsigned char subkeys[SECURITY_LAYER_LEVELS][32];
    bool ready;
} security_layer_t;

bool security_layer_init(security_layer_t *layer);
void security_layer_free(security_layer_t *layer);

bool security_layer_encrypt_message(const security_layer_t *layer,
                                    const char *plaintext, char *out,
                                    size_t out_len);

bool security_layer_decrypt_message(const security_layer_t *layer,
                                    const char *envelope, char *plaintext,
                                    size_t plaintext_len);

void security_layer_generate_salt(uint8_t *salt);
void security_layer_hash_password(const char *password, const uint8_t *salt,
                                  uint8_t *hash_output);
bool security_layer_is_zero_hash(const uint8_t *hash, size_t len);

#endif

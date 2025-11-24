#ifndef SSH_CHATTER_TRANSLATION_HELPERS_H
#define SSH_CHATTER_TRANSLATION_HELPERS_H

#include <stdbool.h>
#include <stddef.h>
#include "host.h"

#ifndef SSH_CHATTER_MAX_TRANSLATION_PLACEHOLDERS
#define SSH_CHATTER_MAX_TRANSLATION_PLACEHOLDERS 32U
#endif

#ifndef SSH_CHATTER_PLACEHOLDER_TOKEN_LEN
#define SSH_CHATTER_PLACEHOLDER_TOKEN_LEN 16U
#endif

#ifndef SSH_CHATTER_PLACEHOLDER_SEQUENCE_LEN
#define SSH_CHATTER_PLACEHOLDER_SEQUENCE_LEN 64U
#endif

#ifndef SSH_CHATTER_TRANSLATION_WORKING_LEN
#define SSH_CHATTER_TRANSLATION_WORKING_LEN (SSH_CHATTER_MESSAGE_LIMIT * 4U)
#endif

#ifndef SSH_CHATTER_TRANSLATION_BATCH_MAX
#define SSH_CHATTER_TRANSLATION_BATCH_MAX 16U
#endif

#ifndef SSH_CHATTER_TRANSLATION_BATCH_BUFFER
#define SSH_CHATTER_TRANSLATION_BATCH_BUFFER                                   \
    (SSH_CHATTER_TRANSLATION_WORKING_LEN * SSH_CHATTER_TRANSLATION_BATCH_MAX)
#endif

#ifndef SSH_CHATTER_LANG_NAME_LEN
#define SSH_CHATTER_LANG_NAME_LEN 64
#endif

typedef struct translation_placeholder {
    char placeholder[SSH_CHATTER_PLACEHOLDER_TOKEN_LEN];
    char sequence[SSH_CHATTER_PLACEHOLDER_SEQUENCE_LEN];
} translation_placeholder_t;

bool translation_prepare_text(const char *message, char *sanitized,
                              size_t sanitized_len,
                              translation_placeholder_t *placeholders,
                              size_t *placeholder_count);

bool translation_restore_text(const char *translated, char *output,
                              size_t output_len,
                              const translation_placeholder_t *placeholders,
                              size_t placeholder_count);

bool translation_strip_no_translate_prefix(const char *message, char *stripped,
                                           size_t stripped_len);

bool is_pure_ascii(const char *str);
void to_lowercase(char *str);

#endif

#ifndef SSH_CHATTER_BACKEND_H
#define SSH_CHATTER_BACKEND_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

char *session_show_welcome_banner(const char *path);

void ssh_chatter_backend_init(void);

bool ssh_chatter_backend_translate_line(const char *message,
                                        const char *target_language,
                                        char *translated, size_t translated_len,
                                        char *detected_language,
                                        size_t detected_len);

#ifdef __cplusplus
}
#endif

#endif

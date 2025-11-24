#ifndef SSH_CHATTER_HUMANIZED_H
#define SSH_CHATTER_HUMANIZED_H

#include <errno.h>
#include <stdio.h>
#include <string.h>

static inline void humanized_log_error(const char *section, const char *message,
                                       int error_code)
{
    if (section == nullptr) {
        section = "unknown";
    }
    if (message == nullptr) {
        message = strerror(error_code != 0 ? error_code : errno);
    }
    fprintf(stderr, "[%s] %s (code: %d)\n", section, message, error_code);
}

#endif

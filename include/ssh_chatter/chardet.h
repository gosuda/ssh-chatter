#ifndef SSH_CHATTER_CHARDET_H
#define SSH_CHATTER_CHARDET_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Lightweight pure-C detector used by IYAGI/hybrid output mode.
 * Returns true when payload appears to be UTF-8 text and should be rendered
 * without retro codepage conversion.
 */
bool sshc_chardet_prefers_utf8(const char *data, size_t length);

#endif

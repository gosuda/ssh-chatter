#include "ssh_chatter/chardet.h"

static bool sshc_chardet_validate_utf8(const unsigned char *data,
                                       size_t length,
                                       size_t *out_multibyte)
{
    if (data == nullptr || length == 0U) {
        if (out_multibyte != nullptr) {
            *out_multibyte = 0U;
        }
        return true;
    }

    size_t idx = 0U;
    size_t multibyte = 0U;
    while (idx < length) {
        unsigned char lead = data[idx];
        if (lead < 0x80U) {
            ++idx;
            continue;
        }

        size_t need = 0U;
        if ((lead & 0xE0U) == 0xC0U) {
            need = 1U;
        } else if ((lead & 0xF0U) == 0xE0U) {
            need = 2U;
        } else if ((lead & 0xF8U) == 0xF0U) {
            need = 3U;
        } else {
            return false;
        }

        if (idx + need >= length) {
            return false;
        }

        for (size_t j = 1U; j <= need; ++j) {
            if ((data[idx + j] & 0xC0U) != 0x80U) {
                return false;
            }
        }

        multibyte += need + 1U;
        idx += need + 1U;
    }

    if (out_multibyte != nullptr) {
        *out_multibyte = multibyte;
    }
    return true;
}

bool sshc_chardet_prefers_utf8(const char *data, size_t length)
{
    if (data == nullptr || length == 0U) {
        return false;
    }

    const unsigned char *bytes = (const unsigned char *)data;
    size_t multibyte = 0U;
    if (!sshc_chardet_validate_utf8(bytes, length, &multibyte)) {
        return false;
    }

    if (multibyte == 0U) {
        return false;
    }

    size_t high_bytes = 0U;
    for (size_t idx = 0U; idx < length; ++idx) {
        if (bytes[idx] & 0x80U) {
            ++high_bytes;
        }
    }

    return high_bytes >= 2U;
}

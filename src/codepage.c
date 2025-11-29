#include "ssh_chatter/codepage.h"
#include "ssh_chatter/host.h"
#include <string.h>
#include <iconv.h>
#include <errno.h>

/* Forward declaration of UTF-8 encoding function from host_runtime.c */
static size_t session_encode_utf8_codepoint(uint32_t codepoint, char *output,
                                            size_t capacity)
{
    if (output == NULL || capacity == 0U) {
        return 0U;
    }

    if (codepoint <= 0x7FU) {
        if (capacity < 1U) {
            return 0U;
        }
        output[0] = (char)codepoint;
        return 1U;
    }

    if (codepoint <= 0x7FFU) {
        if (capacity < 2U) {
            return 0U;
        }
        output[0] = (char)(0xC0U | ((codepoint >> 6U) & 0x1FU));
        output[1] = (char)(0x80U | (codepoint & 0x3FU));
        return 2U;
    }

    if (codepoint <= 0xFFFFU) {
        if (capacity < 3U) {
            return 0U;
        }
        output[0] = (char)(0xE0U | ((codepoint >> 12U) & 0x0FU));
        output[1] = (char)(0x80U | ((codepoint >> 6U) & 0x3FU));
        output[2] = (char)(0x80U | (codepoint & 0x3FU));
        return 3U;
    }

    if (codepoint <= 0x10FFFFU) {
        if (capacity < 4U) {
            return 0U;
        }
        output[0] = (char)(0xF0U | ((codepoint >> 18U) & 0x07U));
        output[1] = (char)(0x80U | ((codepoint >> 12U) & 0x3FU));
        output[2] = (char)(0x80U | ((codepoint >> 6U) & 0x3FU));
        output[3] = (char)(0x80U | (codepoint & 0x3FU));
        return 4U;
    }

    if (capacity < 1U) {
        return 0U;
    }
    output[0] = '?';
    return 1U;
}

/* CP437 - IBM PC (English/Western) */
static const uint16_t kCp437ToUnicode[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA,
    0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6,
    0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC,
    0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA,
    0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC,
    0x00A1, 0x00AB, 0x00BB, 0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561,
    0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B,
    0x2510, 0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567, 0x2568,
    0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518,
    0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580, 0x03B1, 0x00DF, 0x0393,
    0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4,
    0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320,
    0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2,
    0x25A0, 0x00A0,
};

/* CP850 - Western European (DOS Latin 1) */
static const uint16_t kCp850ToUnicode[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA,
    0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6,
    0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC,
    0x00F8, 0x00A3, 0x00D8, 0x00D7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA,
    0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x00AE, 0x00AC, 0x00BD, 0x00BC,
    0x00A1, 0x00AB, 0x00BB, 0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x00C1,
    0x00C2, 0x00C0, 0x00A9, 0x2563, 0x2551, 0x2557, 0x255D, 0x00A2, 0x00A5,
    0x2510, 0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x00E3, 0x00C3,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x00A4, 0x00F0,
    0x00D0, 0x00CA, 0x00CB, 0x00C8, 0x0131, 0x00CD, 0x00CE, 0x00CF, 0x2518,
    0x250C, 0x2588, 0x2584, 0x00A6, 0x00CC, 0x2580, 0x00D3, 0x00DF, 0x00D4,
    0x00D2, 0x00F5, 0x00D5, 0x00B5, 0x00FE, 0x00DE, 0x00DA, 0x00DB, 0x00D9,
    0x00FD, 0x00DD, 0x00AF, 0x00B4, 0x00AD, 0x00B1, 0x2017, 0x00BE, 0x00B6,
    0x00A7, 0x00F7, 0x00B8, 0x00B0, 0x00A8, 0x00B7, 0x00B9, 0x00B3, 0x00B2,
    0x25A0, 0x00A0,
};

/* CP852 - Central European (DOS Latin 2) - Polish, Czech, etc. */
static const uint16_t kCp852ToUnicode[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x016F, 0x0107, 0x00E7, 0x0142,
    0x00EB, 0x0150, 0x0151, 0x00EE, 0x0179, 0x00C4, 0x0106, 0x00C9, 0x0139,
    0x013A, 0x00F4, 0x00F6, 0x013D, 0x013E, 0x015A, 0x015B, 0x00D6, 0x00DC,
    0x0164, 0x0165, 0x0141, 0x00D7, 0x010D, 0x00E1, 0x00ED, 0x00F3, 0x00FA,
    0x0104, 0x0105, 0x017D, 0x017E, 0x0118, 0x0119, 0x00AC, 0x017A, 0x010C,
    0x015F, 0x00AB, 0x00BB, 0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x00C1,
    0x00C2, 0x011A, 0x015E, 0x2563, 0x2551, 0x2557, 0x255D, 0x017B, 0x017C,
    0x2510, 0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x0102, 0x0103,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x00A4, 0x0111,
    0x0110, 0x010E, 0x00CB, 0x010F, 0x0147, 0x00CD, 0x00CE, 0x011B, 0x2518,
    0x250C, 0x2588, 0x2584, 0x0162, 0x016E, 0x2580, 0x00D3, 0x00DF, 0x00D4,
    0x0143, 0x0144, 0x0148, 0x0160, 0x0161, 0x0154, 0x00DA, 0x0155, 0x0170,
    0x00FD, 0x00DD, 0x0163, 0x00B4, 0x00AD, 0x02DD, 0x02DB, 0x02C7, 0x02D8,
    0x00A7, 0x00F7, 0x00B8, 0x00B0, 0x00A8, 0x02D9, 0x0171, 0x0158, 0x0159,
    0x25A0, 0x00A0,
};

/* CP1251 - Cyrillic (Russian) */
static const uint16_t kCp1251ToUnicode[128] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC,
    0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F, 0x0452, 0x2018,
    0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x0098, 0x2122, 0x0459,
    0x203A, 0x045A, 0x045C, 0x045B, 0x045F, 0x00A0, 0x040E, 0x045E, 0x0408,
    0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC,
    0x00AD, 0x00AE, 0x0407, 0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5,
    0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455,
    0x0457, 0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
    0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F, 0x0420,
    0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429,
    0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F, 0x0430, 0x0431, 0x0432,
    0x0433, 0x0434, 0x0435, 0x0436, 0x0437, 0x0438, 0x0439, 0x043A, 0x043B,
    0x043C, 0x043D, 0x043E, 0x043F, 0x0440, 0x0441, 0x0442, 0x0443, 0x0444,
    0x0445, 0x0446, 0x0447, 0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D,
    0x044E, 0x044F,
};

/* Placeholders for future full CP949 and CP932 implementations */

/* Placeholder for future full CP936 implementation */

static size_t session_codepage_iconv_chunk(session_codepage_t codepage,
                                           const unsigned char *bytes,
                                           size_t length, char *output,
                                           size_t capacity)
{
    if (bytes == NULL || length == 0U || output == NULL || capacity == 0U) {
        return 0U;
    }

    char utf8_buffer[16];
    size_t written = session_codepage_to_utf8(codepage, bytes, length,
                                              utf8_buffer, sizeof(utf8_buffer));
    if (written == 0U) {
        return 0U;
    }

    if (written > capacity) {
        written = capacity;
    }
    memcpy(output, utf8_buffer, written);
    return written;
}

size_t session_codepage_byte_to_utf8(session_codepage_t codepage,
                                     session_codepage_context_t *context,
                                     unsigned char byte, char *output,
                                     size_t capacity)
{
    if (output == NULL || capacity == 0U || context == NULL) {
        return 0U;
    }

    /* ASCII passthrough for all code pages */
    if (byte < 0x80U) {
        /* Reset state on ASCII byte - prevents incomplete multi-byte corruption */
        context->state = 0;
        context->lead_byte = 0;
        output[0] = (char)byte;
        return 1U;
    }

    uint32_t codepoint = 0;
    size_t produced = 0;
    bool valid_sequence = false;

    switch (codepage) {
    case SESSION_CODEPAGE_CP949: {
        const bool is_lead = (byte >= 0x81U && byte <= 0xFEU);
        const bool is_trail = (byte >= 0x41U && byte <= 0xFEU && byte != 0x7FU);

        if (context->state == 0) {
            if (is_lead) {
                context->lead_byte = byte;
                context->state = 1;
                return 0U;
            }

            unsigned char single[1] = {byte};
            produced = session_codepage_iconv_chunk(codepage, single, 1U,
                                                    output, capacity);
            if (produced == 0U && capacity > 0U) {
                output[0] = '?';
                produced = 1U;
            }
            context->state = 0;
            context->lead_byte = 0;
            return produced;
        }

        unsigned char sequence[2] = {context->lead_byte, byte};
        context->state = 0;
        context->lead_byte = 0;

        if (is_lead || !is_trail) {
            if (capacity > 0U) {
                output[0] = '?';
                return 1U;
            }
            return 0U;
        }

        produced = session_codepage_iconv_chunk(codepage, sequence, 2U, output,
                                                capacity);
        if (produced == 0U && capacity > 0U) {
            output[0] = '?';
            produced = 1U;
        }
        return produced;
    }

    case SESSION_CODEPAGE_CP932: { /* Japanese Shift-JIS */
        const bool is_lead = (byte >= 0x81U && byte <= 0x9FU) ||
                             (byte >= 0xE0U && byte <= 0xFCU);
        const bool is_trail = (byte >= 0x40U && byte <= 0xFCU && byte != 0x7FU);

        if (context->state == 0) {
            if (is_lead) {
                context->lead_byte = byte;
                context->state = 1;
                return 0U;
            }

            unsigned char single[1] = {byte};
            produced = session_codepage_iconv_chunk(codepage, single, 1U,
                                                    output, capacity);
            if (produced == 0U && capacity > 0U) {
                output[0] = '?';
                produced = 1U;
            }
            return produced;
        }

        unsigned char sequence[2] = {context->lead_byte, byte};
        context->state = 0;
        context->lead_byte = 0;

        if (!is_trail) {
            if (capacity > 0U) {
                output[0] = '?';
                produced = 1U;
            }
            return produced;
        }

        produced = session_codepage_iconv_chunk(codepage, sequence, 2U, output,
                                                capacity);
        if (produced == 0U && capacity > 0U) {
            output[0] = '?';
            produced = 1U;
        }
        return produced;
    }

    case SESSION_CODEPAGE_CP936: { /* Simplified Chinese GBK */
        const bool is_lead = (byte >= 0x81U && byte <= 0xFEU);
        const bool is_trail = (byte >= 0x40U && byte <= 0xFEU && byte != 0x7FU);

        if (context->state == 0) {
            if (is_lead) {
                context->lead_byte = byte;
                context->state = 1;
                return 0U;
            }

            unsigned char single[1] = {byte};
            produced = session_codepage_iconv_chunk(codepage, single, 1U,
                                                    output, capacity);
            if (produced == 0U && capacity > 0U) {
                output[0] = '?';
                produced = 1U;
            }
            return produced;
        }

        unsigned char sequence[2] = {context->lead_byte, byte};
        context->state = 0;
        context->lead_byte = 0;

        if (!is_trail) {
            if (capacity > 0U) {
                output[0] = '?';
                produced = 1U;
            }
            return produced;
        }

        produced = session_codepage_iconv_chunk(codepage, sequence, 2U, output,
                                                capacity);
        if (produced == 0U && capacity > 0U) {
            output[0] = '?';
            produced = 1U;
        }
        return produced;
    }

    case SESSION_CODEPAGE_CP437:
    case SESSION_CODEPAGE_CP850:
    case SESSION_CODEPAGE_CP852:
    case SESSION_CODEPAGE_CP1251: {
        const uint16_t *table = NULL;
        switch (codepage) {
        case SESSION_CODEPAGE_CP437:
            table = kCp437ToUnicode;
            break;
        case SESSION_CODEPAGE_CP850:
            table = kCp850ToUnicode;
            break;
        case SESSION_CODEPAGE_CP852:
            table = kCp852ToUnicode;
            break;
        case SESSION_CODEPAGE_CP1251:
            table = kCp1251ToUnicode;
            break;
        default:
            break; /* Should not happen */
        }

        /* Ensure state is clean for single-byte codepages */
        context->state = 0;
        context->lead_byte = 0;

        if (table != NULL) {
            /* For single-byte tables, byte-0x80 is the index */
            codepoint = table[byte - 0x80U];
            valid_sequence = true;
        } else {
            /* Fallback for unknown codepage table */
            codepoint = 0xFFFD;
            valid_sequence = true;
        }
        break;
    }

    case SESSION_CODEPAGE_UTF8:
        /* UTF-8 mode - pass through as-is
         * NOTE: The caller is responsible for UTF-8 multi-byte handling */
        output[0] = (char)byte;
        context->state = 0;
        context->lead_byte = 0;
        return 1U;

    default:
        /* Unknown or unsupported codepage */
        codepoint = 0xFFFD; /* Unicode replacement character */
        context->state = 0;
        context->lead_byte = 0;
        valid_sequence = true;
        break;
    }

    /* Convert codepoint to UTF-8 */
    if (valid_sequence && codepoint != 0) {
        produced = session_encode_utf8_codepoint(codepoint, output, capacity);
        if (produced == 0U) {
            /* UTF-8 encoding failed - output ASCII replacement */
            if (capacity >= 1U) {
                output[0] = '?';
                return 1U;
            }
            return 0U;
        }
        return produced;
    }

    /* Should not reach here, but provide fallback */
    if (capacity >= 1U) {
        output[0] = '?';
        return 1U;
    }
    return 0U;
}

session_codepage_t session_codepage_for_language(int language)
{
    switch (language) {
    case SESSION_UI_LANGUAGE_KO:
        return SESSION_CODEPAGE_CP949;
    case SESSION_UI_LANGUAGE_JP:
        return SESSION_CODEPAGE_CP932;
    case SESSION_UI_LANGUAGE_ZH:
        return SESSION_CODEPAGE_CP936;
    case SESSION_UI_LANGUAGE_RU:
        return SESSION_CODEPAGE_CP1251;
    case SESSION_UI_LANGUAGE_DE:
    case SESSION_UI_LANGUAGE_FR:
        return SESSION_CODEPAGE_CP850;
    case SESSION_UI_LANGUAGE_PL:
        return SESSION_CODEPAGE_CP852;
    case SESSION_UI_LANGUAGE_EN:
    default:
        return SESSION_CODEPAGE_CP437;
    }
}

const char *session_codepage_name(session_codepage_t codepage)
{
    switch (codepage) {
    case SESSION_CODEPAGE_UTF8:
        return "UTF-8";
    case SESSION_CODEPAGE_CP437:
        return "CP437";
    case SESSION_CODEPAGE_CP949:
        return "CP949";
    case SESSION_CODEPAGE_CP932:
        return "CP932";
    case SESSION_CODEPAGE_CP936:
        return "CP936";
    case SESSION_CODEPAGE_CP1251:
        return "CP1251";
    case SESSION_CODEPAGE_CP850:
        return "CP850";
    case SESSION_CODEPAGE_CP852:
        return "CP852";
    default:
        return "Unknown";
    }
}

const char *session_codepage_iconv_name(session_codepage_t codepage)
{
    switch (codepage) {
    case SESSION_CODEPAGE_UTF8:
        return NULL; /* No conversion needed */
    case SESSION_CODEPAGE_CP437:
        return "CP437//TRANSLIT";
    case SESSION_CODEPAGE_CP949:
        return "CP949//TRANSLIT";
    case SESSION_CODEPAGE_CP932:
        return "CP932//TRANSLIT";
    case SESSION_CODEPAGE_CP936:
        return "CP936//TRANSLIT";
    case SESSION_CODEPAGE_CP1251:
        return "CP1251//TRANSLIT";
    case SESSION_CODEPAGE_CP850:
        return "CP850//TRANSLIT";
    case SESSION_CODEPAGE_CP852:
        return "CP852//TRANSLIT";
    default:
        return NULL;
    }
}

static bool session_iconv_strip_options(const char *name, char *output,
                                        size_t length)
{
    if (name == NULL || output == NULL || length == 0U) {
        return false;
    }

    size_t copy_len = strcspn(name, "/");
    if (copy_len == 0U) {
        return false;
    }
    if (copy_len >= length) {
        copy_len = length - 1U;
    }

    memcpy(output, name, copy_len);
    output[copy_len] = '\0';
    return true;
}

static iconv_t session_iconv_open_with_fallback(const char *to,
                                                const char *from)
{
    iconv_t descriptor = iconv_open(to, from);
    if (descriptor != (iconv_t)(-1)) {
        return descriptor;
    }

    char base[64];
    if (session_iconv_strip_options(from, base, sizeof(base))) {
        descriptor = iconv_open(to, base);
    }

    return descriptor;
}

size_t session_codepage_to_utf8(session_codepage_t codepage,

                                const unsigned char *input,

                                size_t input_length,

                                char *output,

                                size_t output_capacity)
{
    session_codepage_context_t context = {0, 0};
    if (input == NULL || input_length == 0U || output == NULL ||
        output_capacity == 0U) {
        return 0U;
    }

    /* For UTF-8, just copy as-is */
    if (codepage == SESSION_CODEPAGE_UTF8) {
        size_t to_copy =
            input_length < output_capacity ? input_length : output_capacity;
        memcpy(output, input, to_copy);
        return to_copy;
    }

    const char *iconv_name = session_codepage_iconv_name(codepage);
    bool is_multibyte_codepage = (codepage == SESSION_CODEPAGE_CP949 ||
                                  codepage == SESSION_CODEPAGE_CP932 ||
                                  codepage == SESSION_CODEPAGE_CP936);

    if (iconv_name == NULL) {
        if (is_multibyte_codepage) {
            return 0U; /* No iconv name for multi-byte, conversion impossible */
        }
        /* Fall back to byte-by-byte conversion for single-byte codepages or unknown */
        if (input_length > 0U && output_capacity > 0U) {
            size_t result = session_codepage_byte_to_utf8(
                codepage, &context, input[0], output, output_capacity);
            return result;
        }
        return 0U;
    }

    iconv_t descriptor = session_iconv_open_with_fallback("UTF-8", iconv_name);
    if (descriptor == (iconv_t)(-1)) {
        if (is_multibyte_codepage) {
            return 0U; /* iconv_open failed for multi-byte, conversion impossible */
        }
        /* On error, try single-byte conversion for the first byte for single-byte codepages */
        if (input_length > 0U && output_capacity > 0U) {
            size_t result = session_codepage_byte_to_utf8(
                codepage, &context, input[0], output, output_capacity);
            return result;
        }
        return 0U;
    }

    const char *input_cursor = (const char *)input;
    size_t input_remaining = input_length;
    char *output_cursor = output;
    size_t output_remaining = output_capacity;

    size_t result = iconv(descriptor, (char **)&input_cursor, &input_remaining,
                          &output_cursor, &output_remaining);

    iconv_close(descriptor);

    if (result == (size_t)-1) {
        if (is_multibyte_codepage) {
            return 0U; /* iconv failed for multi-byte, conversion impossible */
        }
        /* On error, try single-byte conversion for the first byte for single-byte codepages */
        if (input_length > 0U && output_capacity > 0U) {
            size_t bytes = session_codepage_byte_to_utf8(
                codepage, &context, input[0], output, output_capacity);
            return bytes;
        }
        return 0U;
    }

    return output_capacity - output_remaining;
}

size_t session_utf8_to_codepage(session_codepage_t codepage, const char *input,
                                size_t input_length, char *output,
                                size_t output_capacity)
{
    if (input == NULL || input_length == 0U || output == NULL ||
        output_capacity == 0U) {
        return 0U;
    }

    /* If the target codepage is UTF-8, just copy as-is */
    if (codepage == SESSION_CODEPAGE_UTF8) {
        size_t to_copy =
            input_length < output_capacity ? input_length : output_capacity;
        memcpy(output, input, to_copy);
        return to_copy;
    }

    const char *iconv_name = session_codepage_iconv_name(codepage);

    if (iconv_name == NULL) {
        /* No iconv name for this codepage, conversion impossible */
        return 0U;
    }

    iconv_t descriptor = session_iconv_open_with_fallback(iconv_name, "UTF-8");
    if (descriptor == (iconv_t)(-1)) {
        /* iconv_open failed, conversion impossible */
        return 0U;
    }

    const char *input_cursor = input;
    size_t input_remaining = input_length;
    char *output_cursor = output;
    size_t output_remaining = output_capacity;

    size_t result = iconv(descriptor, (char **)&input_cursor, &input_remaining,
                          &output_cursor, &output_remaining);

    iconv_close(descriptor);

    if (result == (size_t)-1) {
        /* iconv failed, return 0 to indicate failure */
        return 0U;
    }

    return output_capacity - output_remaining;
}
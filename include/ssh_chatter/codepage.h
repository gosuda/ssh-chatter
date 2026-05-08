#ifndef SSH_CHATTER_CODEPAGE_H
#define SSH_CHATTER_CODEPAGE_H

#include <stddef.h>
#include <stdint.h>

/**
 * Supported code pages for retro terminal encoding
 */
typedef enum session_codepage {
    SESSION_CODEPAGE_UTF8 = 0, /* UTF-8 (default, no conversion) */
    SESSION_CODEPAGE_CP437,    /* English/Western (IBM PC) */
    SESSION_CODEPAGE_CP949,    /* Korean (Unified Hangul Code) */
    SESSION_CODEPAGE_CP932,    /* Japanese (Shift-JIS) */
    SESSION_CODEPAGE_CP936,    /* Simplified Chinese (GBK) */
    SESSION_CODEPAGE_CP1251,   /* Russian (Cyrillic) */
    SESSION_CODEPAGE_CP850,    /* Western European (DOS Latin 1) */
    SESSION_CODEPAGE_CP852,    /* Central European (DOS Latin 2) */
    SESSION_CODEPAGE_JOHAB,    /* Korean (Johab, KS X 1001-1) */
    SESSION_CODEPAGE_COUNT
} session_codepage_t;

/**
 * Convert a single byte from the specified code page to UTF-8
 * 
 * @param codepage The source code page
 * @param byte The byte to convert
 * @param output Buffer to store UTF-8 output (must be at least 4 bytes)
 * @param capacity Size of output buffer
 * @return Number of bytes written to output, or 0 on error
 */
/**
 * Context for multi-byte code page conversions
 */
typedef struct session_codepage_context {
    unsigned char
        lead_byte; /* Stores the first byte of a multi-byte sequence */
    int state; /* 0 = single-byte or no active multi-byte sequence, 1 = waiting for trail byte */
} session_codepage_context_t;

/**
 * Convert a single byte from the specified code page to UTF-8, using a context for multi-byte handling
 *
 * @param codepage The source code page
 * @param context Pointer to the codepage context (must be initialized to {0,0} for new conversions)
 * @param byte The byte to convert
 * @param output Buffer to store UTF-8 output (must be at least 4 bytes)
 * @param capacity Size of output buffer
 * @return Number of bytes written to output, or 0 on error
 */
size_t session_codepage_byte_to_utf8(session_codepage_t codepage,
                                     session_codepage_context_t *context,
                                     unsigned char byte, char *output,
                                     size_t capacity);

/**
 * Get the default code page for a UI language
 * 
 * @param language The UI language
 * @return The appropriate code page for that language
 */
session_codepage_t session_codepage_for_language(int language);

/**
 * Get the name of a code page
 * 
 * @param codepage The code page
 * @return String name of the code page
 */
const char *session_codepage_name(session_codepage_t codepage);

/**
 * Get the iconv encoding name for a code page
 * 
 * @param codepage The code page
 * @return String encoding name for use with iconv, or NULL for UTF-8
 */
const char *session_codepage_iconv_name(session_codepage_t codepage);

/**
 * Convert bytes from a code page to UTF-8 using iconv
 * This handles multi-byte sequences properly (e.g., CP949, CP932, CP936)
 * 
 * @param codepage The source code page
 * @param input Input bytes in the specified code page
 * @param input_length Number of input bytes
 * @param output Buffer to store UTF-8 output
 * @param output_capacity Size of output buffer
 * @return Number of bytes written to output, or 0 on error
 */
size_t session_codepage_to_utf8(session_codepage_t codepage,
                                const unsigned char *input, size_t input_length,
                                char *output, size_t output_capacity);

/**
 * Convert bytes from UTF-8 to a specified code page using iconv
 * This handles multi-byte sequences properly (e.g., CP949, CP932, CP936)
 * 
 * @param codepage The target code page
 * @param input Input bytes in UTF-8
 * @param input_length Number of input bytes
 * @param output Buffer to store output in the specified code page
 * @param output_capacity Size of output buffer
 * @return Number of bytes written to output, or 0 on error
 */
size_t session_utf8_to_codepage(session_codepage_t codepage, const char *input,
                                size_t input_length, char *output,
                                size_t output_capacity);

#endif /* SSH_CHATTER_CODEPAGE_H */

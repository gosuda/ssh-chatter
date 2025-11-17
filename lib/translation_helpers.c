#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L // Added for wcwidth

#include "headers/translation_helpers.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h> // For GC_MALLOC, GC_REALLOC, free
#include <string.h>
#include <wchar.h>  // For wcwidth
#include <locale.h> // For setlocale

bool translation_prepare_text(const char *message, char *sanitized,
                              size_t sanitized_len,
                              translation_placeholder_t *placeholders,
                              size_t *placeholder_count)
{
    if (message == nullptr || sanitized == nullptr || sanitized_len == 0U ||
        placeholders == nullptr || placeholder_count == nullptr) {
        return false;
    }

    size_t out_idx = 0U;
    size_t stored = 0U;

    for (size_t idx = 0U; message[idx] != '\0';) {
        unsigned char ch = (unsigned char)message[idx];
        if (ch == '\033') {
            if (stored >= SSH_CHATTER_MAX_TRANSLATION_PLACEHOLDERS) {
                return false;
            }

            size_t seq_start = idx;
            ++idx;
            if (message[idx] != '\0' && message[idx] == '[') {
                ++idx;
                while (message[idx] != '\0') {
                    unsigned char seq_char = (unsigned char)message[idx++];
                    if (seq_char >= '@' && seq_char <= '~') {
                        break;
                    }
                }
            } else if (message[idx] != '\0') {
                ++idx;
            }
            size_t seq_end = idx;
            if (seq_end <= seq_start) {
                seq_end = seq_start + 1U;
            }
            size_t sequence_len = seq_end - seq_start;
            if (sequence_len >= SSH_CHATTER_PLACEHOLDER_SEQUENCE_LEN) {
                return false;
            }

            memcpy(placeholders[stored].sequence, message + seq_start,
                   sequence_len);
            placeholders[stored].sequence[sequence_len] = '\0';
            int written = snprintf(placeholders[stored].placeholder,
                                   sizeof(placeholders[stored].placeholder),
                                   "[[ANSI%zu]]", stored);
            if (written < 0 ||
                (size_t)written >= sizeof(placeholders[stored].placeholder)) {
                return false;
            }

            if (out_idx + (size_t)written >= sanitized_len) {
                return false;
            }

            memcpy(sanitized + out_idx, placeholders[stored].placeholder,
                   (size_t)written);
            out_idx += (size_t)written;
            ++stored;
        } else {
            if (out_idx + 1U >= sanitized_len) {
                return false;
            }
            sanitized[out_idx++] = (char)ch;
            ++idx;
        }
    }

    if (out_idx >= sanitized_len) {
        sanitized[sanitized_len - 1U] = '\0';
        return false;
    }

    sanitized[out_idx] = '\0';
    *placeholder_count = stored;
    return true;
}

bool translation_restore_text(const char *translated, char *output,
                              size_t output_len,
                              const translation_placeholder_t *placeholders,
                              size_t placeholder_count)
{
    if (translated == nullptr || output == nullptr || output_len == 0U) {
        return false;
    }

    size_t out_idx = 0U;
    size_t idx = 0U;
    const size_t input_len = strlen(translated);

    while (idx < input_len) {
        bool replaced = false;
        for (size_t token = 0U; token < placeholder_count; ++token) {
            const size_t placeholder_len =
                strlen(placeholders[token].placeholder);
            if (placeholder_len == 0U) {
                continue;
            }
            if (strncmp(translated + idx, placeholders[token].placeholder,
                        placeholder_len) == 0) {
                const size_t sequence_len =
                    strlen(placeholders[token].sequence);
                if (out_idx + sequence_len >= output_len) {
                    if (output_len > 0U) {
                        output[output_len - 1U] = '\0';
                    }
                    return false;
                }
                memcpy(output + out_idx, placeholders[token].sequence,
                       sequence_len);
                out_idx += sequence_len;
                idx += placeholder_len;
                replaced = true;
                break;
            }
        }
        if (replaced) {
            continue;
        }
        if (out_idx + 1U >= output_len) {
            if (output_len > 0U) {
                output[output_len - 1U] = '\0';
            }
            return false;
        }
        output[out_idx++] = translated[idx++];
    }

    if (out_idx >= output_len) {
        output[output_len - 1U] = '\0';
    } else {
        output[out_idx] = '\0';
    }

    return true;
}

bool translation_strip_no_translate_prefix(const char *message, char *stripped,
                                           size_t stripped_len)
{
    if (stripped != nullptr && stripped_len > 0U) {
        stripped[0] = '\0';
    }

    if (message == nullptr || stripped == nullptr || stripped_len == 0U) {
        return false;
    }

    if (message[0] != '@') {
        return false;
    }

    char first = message[1];
    char second = message[2];
    if (!((first == 'n' || first == 'N') && (second == 't' || second == 'T'))) {
        return false;
    }

    char trailing = message[3];
    if (trailing != '\0' && !isspace((unsigned char)trailing)) {
        return false;
    }

    const char *body = message + 3;
    while (*body != '\0' && isspace((unsigned char)*body)) {
        ++body;
    }

    snprintf(stripped, stripped_len, "%s", body);
    return true;
}

// Helper function to get the display width of a single UTF-8 character
static int get_utf8_char_display_width(const char *s, size_t max_len,
                                       size_t *bytes_read)
{
    mbstate_t ps;
    memset(&ps, 0, sizeof(ps));
    wchar_t wc;
    size_t len = mbrtowc(&wc, s, max_len, &ps);

    if (len == (size_t)-1 || len == (size_t)-2) {
        // Invalid or incomplete character, treat as 1 byte and 1 width
        if (bytes_read)
            *bytes_read = 1;
        return 1;
    }
    if (len == 0) { // Null character
        if (bytes_read)
            *bytes_read = 0;
        return 0;
    }

    int width = wcwidth(wc);
    if (width < 0) { // Non-printable or zero-width character
        width = 0;
    }
    if (bytes_read)
        *bytes_read = len;
    return width;
}

int get_display_width(const char *text)
{
    if (text == nullptr) {
        return 0;
    }

    int width = 0;
    size_t i = 0;
    size_t len = strlen(text);

    while (i < len) {
        if (text[i] == '\033') {             // ANSI escape sequence
            i++;                             // Skip ESC
            if (i < len && text[i] == '[') { // CSI sequence
                i++;                         // Skip '['
                while (i < len && !((text[i] >= 'A' && text[i] <= 'Z') ||
                                    (text[i] >= 'a' && text[i] <= 'z'))) {
                    i++; // Skip until end of sequence character
                }
                if (i < len) {
                    i++; // Skip the final character
                }
            } else if (i < len) { // Single character escape sequence
                i++;              // Skip the character
            }
        } else {
            size_t bytes_read;
            width +=
                get_utf8_char_display_width(&text[i], len - i, &bytes_read);
            i += bytes_read;
        }
    }
    return width;
}

// Helper to check if a character is part of an ANSI escape sequence
static bool is_ansi_char(char c)
{
    return (c >= 0x40 && c <= 0x7E); // @ to ~
}

// Helper to extract ANSI escape sequence
// Returns the number of bytes read for the sequence
static size_t extract_ansi_sequence(const char *text, size_t max_len,
                                    char *buffer, size_t buffer_len)
{
    if (text == nullptr || buffer == nullptr || buffer_len == 0 || max_len == 0 ||
        text[0] != '\033') {
        if (buffer)
            buffer[0] = '\0';
        return 0;
    }

    size_t i = 0;
    size_t buf_idx = 0;

    if (buf_idx < buffer_len - 1)
        buffer[buf_idx++] = text[i]; // ESC
    i++;

    if (i < max_len && text[i] == '[') { // CSI sequence
        if (buf_idx < buffer_len - 1)
            buffer[buf_idx++] = text[i]; // '['
        i++;
        while (i < max_len && !is_ansi_char(text[i])) {
            if (buf_idx < buffer_len - 1)
                buffer[buf_idx++] = text[i];
            i++;
        }
        if (i < max_len && is_ansi_char(text[i])) {
            if (buf_idx < buffer_len - 1)
                buffer[buf_idx++] = text[i];
            i++;
        }
    } else if (i < max_len) { // Single character escape sequence
        if (buf_idx < buffer_len - 1)
            buffer[buf_idx++] = text[i];
        i++;
    }
    buffer[buf_idx] = '\0';
    return i;
}

char **wrap_text_to_width(const char *text, int max_width, size_t *line_count)
{
    if (text == nullptr || max_width <= 0 || line_count == nullptr) {
        if (line_count)
            *line_count = 0;
        return nullptr;
    }

    char **lines = nullptr;
    size_t current_line_count = 0;
    size_t lines_capacity = 8; // Initial capacity for lines array

    lines = (char **)calloc(lines_capacity, sizeof(char *));
    if (lines == nullptr) {
        *line_count = 0;
        return nullptr;
    }

    const char *current_pos = text;
    char active_ansi_codes[256] = ""; // To carry over active ANSI codes

    while (*current_pos != '\0') {
        char current_line_buffer[SSH_CHATTER_MESSAGE_LIMIT];
        size_t current_line_len = 0;
        int current_line_width = 0;
        const char *last_word_break = current_pos;
        int last_word_width = 0;
        size_t last_word_len = 0;

        // Add active ANSI codes to the beginning of the new line
        // Prepend a reset code to ensure consistent styling for each new line
        if (strlen(active_ansi_codes) > 0) {
            const char *reset_code = "\033[0m";
            size_t reset_len = strlen(reset_code);
            size_t ansi_len = strlen(active_ansi_codes);

            if (current_line_len + reset_len + ansi_len <
                sizeof(current_line_buffer)) {
                memcpy(current_line_buffer + current_line_len, reset_code,
                       reset_len);
                current_line_len += reset_len;
                memcpy(current_line_buffer + current_line_len,
                       active_ansi_codes, ansi_len);
                current_line_len += ansi_len;
            }
        }

        const char *line_start_segment = current_pos;

        while (*current_pos != '\0') {
            size_t char_bytes_read = 0;
            int char_display_width = 0;
            char ansi_seq_buffer[64];
            size_t ansi_seq_len = 0;

            if (*current_pos == '\033') {
                ansi_seq_len = extract_ansi_sequence(
                    current_pos, strlen(current_pos), ansi_seq_buffer,
                    sizeof(ansi_seq_buffer));
                if (ansi_seq_len > 0) {
                    // Append ANSI sequence to current line buffer
                    if (current_line_len + ansi_seq_len <
                        sizeof(current_line_buffer)) {
                        memcpy(current_line_buffer + current_line_len,
                               ansi_seq_buffer, ansi_seq_len);
                        current_line_len += ansi_seq_len;
                    }
                    // Update active ANSI codes
                    // This is a simplified approach. A full implementation would parse and track
                    // active SGR parameters. For now, we just append.
                    // This might lead to issues if a reset is missed or a new color overrides.
                    // For basic bubble, this might be sufficient.
                    strncat(active_ansi_codes, ansi_seq_buffer,
                            sizeof(active_ansi_codes) -
                                strlen(active_ansi_codes) - 1);
                    current_pos += ansi_seq_len;
                    continue;
                }
            }

            // Handle newline characters explicitly
            if (*current_pos == '\n') {
                current_pos++;
                break; // Force a line break
            }

            char_display_width = get_utf8_char_display_width(
                current_pos, strlen(current_pos), &char_bytes_read);

            if (current_line_width + char_display_width > max_width) {
                // If we have a word break, use it. Otherwise, break the word.
                if (last_word_break > line_start_segment) {
                    current_pos = last_word_break;
                    current_line_len = last_word_len;
                    current_line_width = last_word_width;
                }
                break; // Line is full
            }

            // Append character to current line buffer
            if (current_line_len + char_bytes_read <
                sizeof(current_line_buffer)) {
                memcpy(current_line_buffer + current_line_len, current_pos,
                       char_bytes_read);
                current_line_len += char_bytes_read;
            }
            current_line_width += char_display_width;

            // Update last word break position
            if (isspace((unsigned char)*current_pos)) {
                last_word_break = current_pos + char_bytes_read;
                last_word_len = current_line_len;
                last_word_width = current_line_width;
            }

            current_pos += char_bytes_read;
        }

        current_line_buffer[current_line_len] = '\0';

        // Trim trailing whitespace from the wrapped line
        while (
            current_line_len > 0 &&
            isspace((unsigned char)current_line_buffer[current_line_len - 1])) {
            current_line_buffer[--current_line_len] = '\0';
        }

        // Reallocate lines array if needed
        if (current_line_count >= lines_capacity) {
            lines_capacity *= 2;
            char **new_lines =
                (char **)GC_REALLOC(lines, lines_capacity * sizeof(char *));
            if (new_lines == nullptr) {
                // Free all previously allocated lines
                for (size_t i = 0; i < current_line_count; i++) {
                    GC_FREE(lines[i]);
                }
                GC_FREE(lines);
                *line_count = 0;
                return nullptr;
            }
            lines = new_lines;
        }

        lines[current_line_count] = strdup(current_line_buffer);
        if (lines[current_line_count] == nullptr) {
            // Free all previously allocated lines
            for (size_t i = 0; i < current_line_count; i++) {
                GC_FREE(lines[i]);
            }
            GC_FREE(lines);
            *line_count = 0;
            return nullptr;
        }
        current_line_count++;
    }

    *line_count = current_line_count;
    return lines;
}

void to_lowercase(char *str)
{
    if (str == nullptr) {
        return;
    }
    for (size_t i = 0; str[i] != '\0'; ++i) {
        str[i] = (char)tolower((unsigned char)str[i]);
    }
}

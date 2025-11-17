#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "headers/ssh_chatter_backend.h"
#include "headers/translation_helpers.h"
#include "headers/translator.h"

void ssh_chatter_backend_init(void)
{
    translator_global_init();
}

static void backend_copy_string(char *dest, size_t dest_len, const char *source)
{
    if (dest == nullptr || dest_len == 0U) {
        return;
    }

    if (source == nullptr) {
        dest[0] = '\0';
        return;
    }

    size_t copy_len = strlen(source);
    if (copy_len >= dest_len) {
        copy_len = dest_len - 1U;
    }

    memcpy(dest, source, copy_len);
    dest[copy_len] = '\0';
}

bool ssh_chatter_backend_translate_line(const char *message,
                                        const char *target_language,
                                        char *translated, size_t translated_len,
                                        char *detected_language,
                                        size_t detected_len)
{
    if (translated != nullptr && translated_len > 0U) {
        translated[0] = '\0';
    }
    if (detected_language != nullptr && detected_len > 0U) {
        detected_language[0] = '\0';
    }

    if (message == nullptr || target_language == nullptr ||
        translated == nullptr || translated_len == 0U) {
        return false;
    }

    char stripped[SSH_CHATTER_TRANSLATION_WORKING_LEN];
    if (translation_strip_no_translate_prefix(message, stripped,
                                              sizeof(stripped))) {
        backend_copy_string(translated, translated_len, stripped);
        if (detected_language != nullptr && detected_len > 0U) {
            detected_language[0] = '\0';
        }
        return true;
    }

    ssh_chatter_backend_init();

    translation_placeholder_t
        placeholders[SSH_CHATTER_MAX_TRANSLATION_PLACEHOLDERS];
    size_t placeholder_count = 0U;
    char sanitized[SSH_CHATTER_TRANSLATION_WORKING_LEN];
    if (!translation_prepare_text(message, sanitized, sizeof(sanitized),
                                  placeholders, &placeholder_count)) {
        return false;
    }

    if (sanitized[0] == '\0') {
        return true;
    }

    char detected_buffer[SSH_CHATTER_LANG_NAME_LEN];
    char translated_body[SSH_CHATTER_TRANSLATION_WORKING_LEN];
    if (!translator_translate(sanitized, target_language, translated_body,
                              sizeof(translated_body), detected_buffer,
                              sizeof(detected_buffer))) {
        return false;
    }

    char restored[SSH_CHATTER_TRANSLATION_WORKING_LEN];
    if (!translation_restore_text(translated_body, restored, sizeof(restored),
                                  placeholders, placeholder_count)) {
        return false;
    }

    backend_copy_string(translated, translated_len, restored);
    if (detected_language != nullptr) {
        backend_copy_string(detected_language, detected_len, detected_buffer);
    }

    return true;
}

/*
 * Reads a welcome banner from a file specified by the CHATTER_WELCOME_BANNER
 * environment variable. The banner content is returned as a dynamically allocated
 * string. The caller is responsible for freeing the returned string.
 * Returns nullptr on error or if the path is nullptr.
 */
char *session_show_welcome_banner(const char *path)
{
    if (path == nullptr) {
        return nullptr;
    }

    FILE *banner_file = fopen(path, "r");
    if (banner_file == nullptr) {
        // Error opening file, return nullptr
        return nullptr;
    }

    // Determine file size to allocate buffer
    fseek(banner_file, 0, SEEK_END);
    long file_size = ftell(banner_file);
    if (file_size < 0) {
        fclose(banner_file);
        return nullptr; // Error getting file size
    }
    rewind(banner_file);

    // Allocate memory for the banner content + null terminator
    char *banner_content = (char *)GC_MALLOC((size_t)file_size + 1);
    if (banner_content == nullptr) {
        fclose(banner_file);
        return nullptr; // Memory allocation failed
    }

    // Read file content into the buffer
    size_t bytes_read =
        fread(banner_content, 1, (size_t)file_size, banner_file);
    if (bytes_read != (size_t)file_size) {
        // Error reading file content
        GC_FREE(banner_content);
        fclose(banner_file);
        return nullptr;
    }
    banner_content[file_size] = '\0'; // Null-terminate the string

    fclose(banner_file);

    // Ensure there is at least a newline at the end if the file didn't have one
    if (file_size == 0 || banner_content[file_size - 1] != '\n') {
        char *temp = (char *)GC_REALLOC(banner_content, (size_t)file_size + 2);
        if (temp == nullptr) {
            // Realloc failed, return original content (might be missing newline)
            return banner_content;
        }
        banner_content = temp;
        banner_content[file_size] = '\n';
        banner_content[file_size + 1] = '\0';
    }

    return banner_content;
}

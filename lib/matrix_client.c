#define _POSIX_C_SOURCE 200809L

#include "headers/matrix_client.h"

#include "headers/client.h"
#include "headers/host.h"
#include "headers/humanized/humanized.h"
#include "headers/security_layer.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <openssl/crypto.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>

#define MATRIX_USER_AGENT "ssh-chatter/matrix"
#define MATRIX_SYNC_TIMEOUT_MS 20000L
#define MATRIX_SYNC_BACKOFF_SECONDS 5L
#define MATRIX_MAX_EVENT_IDS 8U

typedef struct matrix_buffer {
    char *data;
    size_t length;
} matrix_buffer_t;

struct matrix_client {
    host_t *host;
    client_manager_t *manager;
    security_layer_t *security;
    client_connection_t connection;
    pthread_mutex_t lock;
    pthread_mutex_t http_lock;
    bool lock_initialized;
    bool http_lock_initialized;
    pthread_t thread;
    bool thread_initialized;
    _Atomic bool stop;
    _Atomic bool running;
    _Atomic bool disabled;
    char homeserver[256];
    char access_token[512];
    char room_id[256];
    char device_name[128];
    char user_id[128];
    char since_token[512];
    uint64_t next_txn_id;
    char pending_skip_username[SSH_CHATTER_USERNAME_LEN];
    char pending_skip_message[SSH_CHATTER_MESSAGE_LIMIT];
    _Atomic bool skip_next_broadcast;
    char recent_event_ids[MATRIX_MAX_EVENT_IDS][128];
    size_t recent_event_count;
    size_t recent_event_head;
};

typedef struct matrix_payload {
    char username[SSH_CHATTER_USERNAME_LEN];
    char message[SSH_CHATTER_MESSAGE_LIMIT];
    bool system;
    bool from_self;
    char category[32];
} matrix_payload_t;

static bool matrix_url_is_https(const char *url)
{
    if (url == nullptr) {
        return false;
    }
    return strncasecmp(url, "https://", 8) == 0;
}

static size_t matrix_curl_write(void *contents, size_t size, size_t nmemb,
                                void *userp)
{
    size_t total = size * nmemb;
    matrix_buffer_t *buffer = (matrix_buffer_t *)userp;
    if (total == 0U) {
        return 0U;
    }

    char *new_data = (char *)GC_REALLOC(buffer->data, buffer->length + total + 1U);
    if (new_data == nullptr) {
        return 0U;
    }

    memcpy(new_data + buffer->length, contents, total);
    buffer->data = new_data;
    buffer->length += total;
    buffer->data[buffer->length] = '\0';
    return total;
}

static void matrix_buffer_free(matrix_buffer_t *buffer)
{
    if (buffer == nullptr) {
        return;
    }
    if (buffer->data != nullptr) {
        OPENSSL_cleanse(buffer->data, buffer->length);
        GC_FREE(buffer->data);
        buffer->data = nullptr;
    }
    buffer->length = 0U;
}

static void matrix_client_disable(matrix_client_t *client, const char *message,
                                  int error_code)
{
    if (client == nullptr) {
        return;
    }

    bool expected = false;
    if (!atomic_compare_exchange_strong(&client->disabled, &expected, true)) {
        return;
    }

    int log_code = (error_code != 0) ? error_code : EIO;
    if (message != nullptr && message[0] != '\0') {
        humanized_log_error("matrix", message, log_code);
    } else {
        humanized_log_error("matrix", "matrix bridge disabled after failure",
                            log_code);
    }

    atomic_store(&client->stop, true);
}

static const char *matrix_getenv(const char *name)
{
    const char *value = getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return nullptr;
    }
    return value;
}

static bool matrix_copy_trimmed(char *dest, size_t dest_len, const char *src)
{
    if (dest == nullptr || dest_len == 0U) {
        return false;
    }
    dest[0] = '\0';
    if (src == nullptr) {
        return false;
    }

    const unsigned char *start = (const unsigned char *)src;
    while (*start != '\0' && isspace(*start)) {
        ++start;
    }

    const unsigned char *end = start;
    while (*end != '\0') {
        ++end;
    }
    while (end > start && isspace(*(end - 1))) {
        --end;
    }

    size_t length = (size_t)(end - start);
    if (length >= dest_len) {
        return false;
    }

    if (length > 0U) {
        memcpy(dest, start, length);
    }
    dest[length] = '\0';
    return true;
}

static pthread_mutex_t probe_lock = PTHREAD_MUTEX_INITIALIZER;

static bool matrix_probe_homeserver(const char *homeserver)
{
    if (homeserver == nullptr || homeserver[0] == '\0') {
        return false;
    }

    pthread_mutex_lock(&probe_lock);
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        pthread_mutex_unlock(&probe_lock);
        errno = ENOMEM;
        return false;
    }

    char url[512];
    int written =
        snprintf(url, sizeof(url), "%s/_matrix/client/versions", homeserver);
    if (written < 0 || (size_t)written >= sizeof(url)) {
        curl_easy_cleanup(curl);
        pthread_mutex_unlock(&probe_lock);
        errno = ENAMETOOLONG;
        return false;
    }

    matrix_buffer_t response = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "GET");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, MATRIX_USER_AGENT);
    if (matrix_url_is_https(homeserver)) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    } else {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, matrix_curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK) {
        status = 0;
    }

    curl_easy_cleanup(curl);
    pthread_mutex_unlock(&probe_lock);
    matrix_buffer_free(&response);

    if (result != CURLE_OK || status < 200 || status >= 300) {
        return false;
    }

    return true;
}

static bool matrix_json_escape(const char *input, char *output,
                               size_t output_len)
{
    if (output == nullptr || output_len == 0U) {
        return false;
    }

    if (input == nullptr) {
        output[0] = '\0';
        return true;
    }

    size_t out_index = 0U;
    for (const unsigned char *cursor = (const unsigned char *)input;
         *cursor != '\0'; ++cursor) {
        unsigned char ch = *cursor;
        if (out_index + 6U >= output_len) {
            return false;
        }
        switch (ch) {
        case '\\':
        case '"':
            output[out_index++] = '\\';
            output[out_index++] = (char)ch;
            break;
        case '\n':
            output[out_index++] = '\\';
            output[out_index++] = 'n';
            break;
        case '\r':
            output[out_index++] = '\\';
            output[out_index++] = 'r';
            break;
        case '\t':
            output[out_index++] = '\\';
            output[out_index++] = 't';
            break;
        default:
            if (ch < 0x20U) {
                int written = snprintf(output + out_index,
                                       output_len - out_index, "\\u%04X", ch);
                if (written < 0 || (size_t)written >= output_len - out_index) {
                    return false;
                }
                out_index += (size_t)written;
            } else {
                output[out_index++] = (char)ch;
            }
            break;
        }
    }

    if (out_index >= output_len) {
        return false;
    }
    output[out_index] = '\0';
    return true;
}

static int matrix_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

static bool matrix_append_codepoint(char **output, size_t *remaining,
                                    unsigned int codepoint)
{
    char *dest = *output;
    size_t space = *remaining;
    if (codepoint <= 0x7FU) {
        if (space < 2U) {
            return false;
        }
        *dest++ = (char)codepoint;
        *remaining -= 1U;
    } else if (codepoint <= 0x7FFU) {
        if (space < 3U) {
            return false;
        }
        *dest++ = (char)(0xC0U | ((codepoint >> 6U) & 0x1FU));
        *dest++ = (char)(0x80U | (codepoint & 0x3FU));
        *remaining -= 2U;
    } else if (codepoint <= 0xFFFFU) {
        if (space < 4U) {
            return false;
        }
        *dest++ = (char)(0xE0U | ((codepoint >> 12U) & 0x0FU));
        *dest++ = (char)(0x80U | ((codepoint >> 6U) & 0x3FU));
        *dest++ = (char)(0x80U | (codepoint & 0x3FU));
        *remaining -= 3U;
    } else if (codepoint <= 0x10FFFFU) {
        if (space < 5U) {
            return false;
        }
        *dest++ = (char)(0xF0U | ((codepoint >> 18U) & 0x07U));
        *dest++ = (char)(0x80U | ((codepoint >> 12U) & 0x3FU));
        *dest++ = (char)(0x80U | ((codepoint >> 6U) & 0x3FU));
        *dest++ = (char)(0x80U | (codepoint & 0x3FU));
        *remaining -= 4U;
    } else {
        return false;
    }
    *output = dest;
    return true;
}

static bool matrix_json_decode_string(const char *input, char *output,
                                      size_t output_len, const char **end_out)
{
    if (input == nullptr || output == nullptr || output_len == 0U) {
        return false;
    }

    if (*input != '"') {
        return false;
    }

    const char *cursor = input + 1;
    char *dest = output;
    size_t remaining = output_len;

    while (*cursor != '\0') {
        if (*cursor == '"') {
            if (remaining == 0U) {
                return false;
            }
            *dest = '\0';
            if (end_out != nullptr) {
                *end_out = cursor + 1;
            }
            return true;
        }
        if (*cursor == '\\') {
            ++cursor;
            if (*cursor == '\0') {
                return false;
            }
            switch (*cursor) {
            case '"':
            case '\\':
            case '/':
                if (remaining < 2U) {
                    return false;
                }
                *dest++ = *cursor;
                --remaining;
                break;
            case 'b':
                if (remaining < 2U) {
                    return false;
                }
                *dest++ = '\b';
                --remaining;
                break;
            case 'f':
                if (remaining < 2U) {
                    return false;
                }
                *dest++ = '\f';
                --remaining;
                break;
            case 'n':
                if (remaining < 2U) {
                    return false;
                }
                *dest++ = '\n';
                --remaining;
                break;
            case 'r':
                if (remaining < 2U) {
                    return false;
                }
                *dest++ = '\r';
                --remaining;
                break;
            case 't':
                if (remaining < 2U) {
                    return false;
                }
                *dest++ = '\t';
                --remaining;
                break;
            case 'u': {
                unsigned int codepoint = 0U;
                for (int idx = 0; idx < 4; ++idx) {
                    ++cursor;
                    int value = matrix_hex_value(*cursor);
                    if (value < 0) {
                        return false;
                    }
                    codepoint = (codepoint << 4U) | (unsigned int)value;
                }
                if (!matrix_append_codepoint(&dest, &remaining, codepoint)) {
                    return false;
                }
            } break;
            default:
                return false;
            }
        } else {
            if (remaining < 2U) {
                return false;
            }
            *dest++ = *cursor;
            --remaining;
        }
        ++cursor;
    }

    return false;
}

static const char *matrix_skip_whitespace(const char *cursor)
{
    while (cursor != nullptr && (*cursor == ' ' || *cursor == '\n' ||
                              *cursor == '\r' || *cursor == '\t')) {
        ++cursor;
    }
    return cursor;
}

static bool matrix_json_extract_string(const char *json, const char *key,
                                       char *output, size_t output_len)
{
    if (json == nullptr || key == nullptr || output == nullptr || output_len == 0U) {
        return false;
    }

    const char *key_pos = strstr(json, key);
    if (key_pos == nullptr) {
        return false;
    }

    const char *colon = strchr(key_pos, ':');
    if (colon == nullptr) {
        return false;
    }
    colon = matrix_skip_whitespace(colon + 1);
    if (colon == nullptr || *colon != '"') {
        return false;
    }

    return matrix_json_decode_string(colon, output, output_len, nullptr);
}

static bool matrix_json_extract_bool(const char *json, const char *key,
                                     bool *value_out)
{
    if (json == nullptr || key == nullptr || value_out == nullptr) {
        return false;
    }

    const char *key_pos = strstr(json, key);
    if (key_pos == nullptr) {
        return false;
    }

    const char *colon = strchr(key_pos, ':');
    if (colon == nullptr) {
        return false;
    }
    colon = matrix_skip_whitespace(colon + 1);
    if (colon == nullptr) {
        return false;
    }

    if (strncmp(colon, "true", 4) == 0) {
        *value_out = true;
        return true;
    }
    if (strncmp(colon, "false", 5) == 0) {
        *value_out = false;
        return true;
    }
    return false;
}

static const char *matrix_attachment_label(chat_attachment_type_t type)
{
    switch (type) {
    case CHAT_ATTACHMENT_IMAGE:
        return "image";
    case CHAT_ATTACHMENT_VIDEO:
        return "video";
    case CHAT_ATTACHMENT_AUDIO:
        return "audio";
    case CHAT_ATTACHMENT_FILE:
        return "file";
    case CHAT_ATTACHMENT_NONE:
    default:
        return "attachment";
    }
}

static bool matrix_client_issue_request(matrix_client_t *client,
                                        const char *method, const char *url,
                                        const char *body,
                                        matrix_buffer_t *response,
                                        long *status_out)
{
    if (client == nullptr || method == nullptr || url == nullptr) {
        return false;
    }

    pthread_mutex_lock(&client->http_lock);

    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        pthread_mutex_unlock(&client->http_lock);
        errno = ENOMEM;
        return false;
    }

    struct curl_slist *headers = nullptr;
    char auth_header[640];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s",
             client->access_token);
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, MATRIX_USER_AGENT);
    if (matrix_url_is_https(url)) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    } else {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    matrix_buffer_t local_response = {0};
    if (response == nullptr) {
        response = &local_response;
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, matrix_curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);

    if (body != nullptr) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    }

    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK) {
        status = 0;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    pthread_mutex_unlock(&client->http_lock);

    if (status_out != nullptr) {
        *status_out = status;
    }

    if (result != CURLE_OK || status < 200 || status >= 300) {
        if (response == &local_response) {
            matrix_buffer_free(&local_response);
        }
        return false;
    }

    return true;
}

static bool matrix_client_send_payload(matrix_client_t *client,
                                       const char *payload)
{
    if (client == nullptr || payload == nullptr) {
        return false;
    }

    bool sent = false;
    pthread_mutex_lock(&client->http_lock);

    if (atomic_load(&client->disabled)) {
        pthread_mutex_unlock(&client->http_lock);
        return false;
    }

    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        pthread_mutex_unlock(&client->http_lock);
        errno = ENOMEM;
        return false;
    }

    char *encoded_room = curl_easy_escape(curl, client->room_id, 0);
    if (encoded_room == nullptr) {
        curl_easy_cleanup(curl);
        pthread_mutex_unlock(&client->http_lock);
        errno = ENOMEM;
        return false;
    }

    uint64_t txn_id = 0U;
    pthread_mutex_lock(&client->lock);
    txn_id = client->next_txn_id++;
    pthread_mutex_unlock(&client->lock);

    char url[512];
    snprintf(url, sizeof(url),
             "%s/_matrix/client/r0/rooms/%s/send/m.room.message/%" PRIu64,
             client->homeserver, encoded_room, txn_id);
    curl_free(encoded_room);

    matrix_buffer_t response = {0};
    long status = 0;
    sent = matrix_client_issue_request(client, "PUT", url, payload, &response,
                                       &status);
    int saved_errno = errno;
    matrix_buffer_free(&response);
    curl_easy_cleanup(curl);
    pthread_mutex_unlock(&client->http_lock);
    if (!sent) {
        char message[256];
        snprintf(message, sizeof(message),
                 "matrix send failed (HTTP %ld); matrix bridge disabled",
                 status);
        matrix_client_disable(client, message,
                              saved_errno != 0 ? saved_errno : EIO);
    }
    return sent;
}

static bool matrix_client_should_skip(matrix_client_t *client,
                                      const chat_history_entry_t *entry)
{
    if (client == nullptr || entry == nullptr) {
        return false;
    }

    if (!atomic_load(&client->skip_next_broadcast)) {
        return false;
    }

    bool should_skip = false;
    pthread_mutex_lock(&client->lock);
    if (client->pending_skip_username[0] != '\0' &&
        client->pending_skip_message[0] != '\0' &&
        strcmp(client->pending_skip_username, entry->username) == 0 &&
        strcmp(client->pending_skip_message, entry->message) == 0) {
        should_skip = true;
        client->pending_skip_username[0] = '\0';
        client->pending_skip_message[0] = '\0';
        atomic_store(&client->skip_next_broadcast, false);
    }
    pthread_mutex_unlock(&client->lock);
    return should_skip;
}

static bool matrix_entry_is_bbs_notice(const chat_history_entry_t *entry)
{
    if (entry == nullptr || entry->is_user_message) {
        return false;
    }
    const char *message = entry->message;
    if (message == nullptr) {
        return false;
    }
    return strncmp(message, "* [bbs]", 7) == 0;
}

static bool matrix_client_build_plaintext(matrix_client_t *client,
                                          const chat_history_entry_t *entry,
                                          char *plaintext, size_t plaintext_len)
{
    (void)client;
    if (entry == nullptr || plaintext == nullptr) {
        return false;
    }

    char escaped_username[SSH_CHATTER_USERNAME_LEN * 6];
    char escaped_message[SSH_CHATTER_MESSAGE_LIMIT * 6];
    if (!matrix_json_escape(entry->username, escaped_username,
                            sizeof(escaped_username))) {
        return false;
    }
    if (!matrix_json_escape(entry->message, escaped_message,
                            sizeof(escaped_message))) {
        return false;
    }

    int written =
        snprintf(plaintext, plaintext_len,
                 "{\"username\":\"%s\",\"message\":\"%s\",\"system\":%s,"
                 "\"source\":\"ssh-chatter\",\"message_id\":%" PRIu64,
                 escaped_username, escaped_message,
                 entry->is_user_message ? "false" : "true", entry->message_id);
    if (written < 0 || (size_t)written >= plaintext_len) {
        return false;
    }
    size_t offset = (size_t)written;

    if (matrix_entry_is_bbs_notice(entry)) {
        int appended = snprintf(plaintext + offset, plaintext_len - offset,
                                ",\"category\":\"bbs\"");
        if (appended < 0 || (size_t)appended >= plaintext_len - offset) {
            return false;
        }
        offset += (size_t)appended;
    }

    if (entry->attachment_type != CHAT_ATTACHMENT_NONE &&
        entry->attachment_target[0] != '\0') {
        char escaped_target[SSH_CHATTER_ATTACHMENT_TARGET_LEN * 6];
        char escaped_caption[SSH_CHATTER_ATTACHMENT_CAPTION_LEN * 6];
        if (!matrix_json_escape(entry->attachment_target, escaped_target,
                                sizeof(escaped_target))) {
            return false;
        }
        if (!matrix_json_escape(entry->attachment_caption, escaped_caption,
                                sizeof(escaped_caption))) {
            return false;
        }
        const char *label = matrix_attachment_label(entry->attachment_type);
        int appended =
            snprintf(plaintext + offset, plaintext_len - offset,
                     ",\"attachment\":{\"type\":\"%s\",\"target\":\"%"
                     "s\",\"caption\":\"%s\"}",
                     label, escaped_target, escaped_caption);
        if (appended < 0 || (size_t)appended >= plaintext_len - offset) {
            return false;
        }
        offset += (size_t)appended;
    }

    if (offset + 1U >= plaintext_len) {
        return false;
    }
    plaintext[offset++] = '}';
    plaintext[offset] = '\0';

    return true;
}

static bool matrix_client_send_entry(matrix_client_t *client,
                                     const chat_history_entry_t *entry)
{
    if (client == nullptr || entry == nullptr || client->security == nullptr) {
        return false;
    }

    if (atomic_load(&client->disabled)) {
        return false;
    }

    char plaintext[SSH_CHATTER_MESSAGE_LIMIT * 4];
    if (!matrix_client_build_plaintext(client, entry, plaintext,
                                       sizeof(plaintext))) {
        return false;
    }

    size_t encrypted_capacity = (SSH_CHATTER_MESSAGE_LIMIT * 4U) + 4096U;
    char *encrypted = (char *)GC_MALLOC(encrypted_capacity);
    if (encrypted == nullptr) {
        errno = ENOMEM;
        return false;
    }

    bool encrypted_ok = security_layer_encrypt_message(
        client->security, plaintext, encrypted, encrypted_capacity);
    OPENSSL_cleanse(plaintext, sizeof(plaintext));
    if (!encrypted_ok) {
        OPENSSL_cleanse(encrypted, encrypted_capacity);
        GC_free(encrypted);
        return false;
    }

    char escaped_body[(SSH_CHATTER_MESSAGE_LIMIT * 4U) + 4096U];
    if (!matrix_json_escape(encrypted, escaped_body, sizeof(escaped_body))) {
        OPENSSL_cleanse(encrypted, encrypted_capacity);
        GC_free(encrypted);
        return false;
    }

    char payload[(SSH_CHATTER_MESSAGE_LIMIT * 4U) + 8192U];
    int written =
        snprintf(payload, sizeof(payload),
                 "{\"msgtype\":\"m.notice\",\"body\":\"%s\"}", escaped_body);
    OPENSSL_cleanse(encrypted, encrypted_capacity);
    GC_free(encrypted);
    if (written < 0 || (size_t)written >= sizeof(payload)) {
        return false;
    }

    return matrix_client_send_payload(client, payload);
}

static void matrix_client_on_message(client_connection_t *connection,
                                     const chat_history_entry_t *entry)
{
    if (connection == nullptr || entry == nullptr || connection->user_data == nullptr) {
        return;
    }

    matrix_client_t *client = (matrix_client_t *)connection->user_data;
    if (atomic_load(&client->disabled)) {
        return;
    }
    if (entry->username[0] == '\0') {
        return;
    }

    if (entry->message[0] == '\0' &&
        entry->attachment_type == CHAT_ATTACHMENT_NONE) {
        return;
    }

    if (matrix_client_should_skip(client, entry)) {
        return;
    }

    if (!matrix_client_send_entry(client, entry)) {
        if (!atomic_load(&client->disabled)) {
            humanized_log_error("matrix", "failed to relay message to matrix",
                                errno != 0 ? errno : EIO);
        }
    }
}

static void matrix_client_on_detach(client_connection_t *connection)
{
    (void)connection;
}

static void matrix_client_record_event(matrix_client_t *client,
                                       const char *event_id)
{
    if (client == nullptr || event_id == nullptr || event_id[0] == '\0') {
        return;
    }

    pthread_mutex_lock(&client->lock);
    size_t limit = client->recent_event_count;
    if (limit > MATRIX_MAX_EVENT_IDS) {
        limit = MATRIX_MAX_EVENT_IDS;
    }
    for (size_t idx = 0U; idx < limit; ++idx) {
        size_t pos =
            (client->recent_event_head + MATRIX_MAX_EVENT_IDS - idx - 1U) %
            MATRIX_MAX_EVENT_IDS;
        if (strcmp(client->recent_event_ids[pos], event_id) == 0) {
            pthread_mutex_unlock(&client->lock);
            return;
        }
    }

    size_t slot = client->recent_event_head;
    snprintf(client->recent_event_ids[slot],
             sizeof(client->recent_event_ids[slot]), "%s", event_id);
    if (client->recent_event_count < MATRIX_MAX_EVENT_IDS) {
        client->recent_event_count++;
    }
    client->recent_event_head =
        (client->recent_event_head + 1U) % MATRIX_MAX_EVENT_IDS;
    pthread_mutex_unlock(&client->lock);
}

static bool matrix_client_event_recent(matrix_client_t *client,
                                       const char *event_id)
{
    if (client == nullptr || event_id == nullptr) {
        return true;
    }

    bool seen = false;
    pthread_mutex_lock(&client->lock);
    size_t limit = client->recent_event_count;
    if (limit > MATRIX_MAX_EVENT_IDS) {
        limit = MATRIX_MAX_EVENT_IDS;
    }
    for (size_t idx = 0U; idx < limit; ++idx) {
        size_t pos =
            (client->recent_event_head + MATRIX_MAX_EVENT_IDS - idx - 1U) %
            MATRIX_MAX_EVENT_IDS;
        if (strcmp(client->recent_event_ids[pos], event_id) == 0) {
            seen = true;
            break;
        }
    }
    pthread_mutex_unlock(&client->lock);
    return seen;
}

static bool matrix_client_parse_payload(const char *plaintext,
                                        matrix_payload_t *payload)
{
    if (plaintext == nullptr || payload == nullptr) {
        return false;
    }

    memset(payload, 0, sizeof(*payload));
    payload->system = false;
    payload->from_self = false;

    if (!matrix_json_extract_string(plaintext, "\"username\"",
                                    payload->username,
                                    sizeof(payload->username))) {
        snprintf(payload->username, sizeof(payload->username), "%s", "matrix");
    }
    (void)matrix_json_extract_string(plaintext, "\"message\"", payload->message,
                                     sizeof(payload->message));

    bool system_value = false;
    if (matrix_json_extract_bool(plaintext, "\"system\"", &system_value)) {
        payload->system = system_value;
    }

    char source[64];
    if (matrix_json_extract_string(plaintext, "\"source\"", source,
                                   sizeof(source))) {
        if (strcmp(source, "ssh-chatter") == 0) {
            payload->from_self = true;
        }
    }

    (void)matrix_json_extract_string(plaintext, "\"category\"",
                                     payload->category,
                                     sizeof(payload->category));

    return true;
}

static void matrix_client_inject_message(matrix_client_t *client,
                                         const matrix_payload_t *payload)
{
    if (client == nullptr || payload == nullptr || client->host == nullptr) {
        return;
    }

    if (payload->from_self) {
        return;
    }

    if (payload->system) {
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(line, sizeof(line), "[matrix] %s", payload->message);
        if (!host_post_client_message(client->host, "matrix-system", line, nullptr,
                                      nullptr, false)) {
            humanized_log_error("matrix", "failed to inject system notice",
                                errno != 0 ? errno : EIO);
        }
        return;
    }

    pthread_mutex_lock(&client->lock);
    snprintf(client->pending_skip_username,
             sizeof(client->pending_skip_username), "%s", payload->username);
    snprintf(client->pending_skip_message, sizeof(client->pending_skip_message),
             "%s", payload->message);
    atomic_store(&client->skip_next_broadcast, true);
    pthread_mutex_unlock(&client->lock);

    if (!host_post_client_message(client->host, payload->username,
                                  payload->message, nullptr, nullptr, false)) {
        humanized_log_error("matrix", "failed to inject bridged message",
                            errno != 0 ? errno : EIO);
        atomic_store(&client->skip_next_broadcast, false);
    }
}

static void matrix_client_handle_body(matrix_client_t *client, const char *body)
{
    if (client == nullptr || body == nullptr || client->security == nullptr) {
        return;
    }

    size_t decrypted_capacity = SSH_CHATTER_MESSAGE_LIMIT * 4U;
    char *plaintext = (char *)GC_MALLOC(decrypted_capacity);
    if (plaintext == nullptr) {
        return;
    }

    if (!security_layer_decrypt_message(client->security, body, plaintext,
                                        decrypted_capacity)) {
        OPENSSL_cleanse(plaintext, decrypted_capacity);
        GC_free(plaintext);
        return;
    }

    matrix_payload_t payload;
    if (matrix_client_parse_payload(plaintext, &payload)) {
        matrix_client_inject_message(client, &payload);
    }

    OPENSSL_cleanse(plaintext, decrypted_capacity);
    GC_free(plaintext);
}

static const char *matrix_find_matching(const char *start, char open,
                                        char close)
{
    if (start == nullptr || *start != open) {
        return nullptr;
    }

    int depth = 1;
    const char *cursor = start + 1;
    while (*cursor != '\0') {
        if (*cursor == open) {
            ++depth;
        } else if (*cursor == close) {
            --depth;
            if (depth == 0) {
                return cursor;
            }
        }
        ++cursor;
    }
    return nullptr;
}

static void matrix_client_process_event(matrix_client_t *client,
                                        const char *event_json, size_t length)
{
    if (client == nullptr || event_json == nullptr || length == 0U) {
        return;
    }

    char *buffer = (char *)GC_MALLOC(length + 1U);
    if (buffer == nullptr) {
        return;
    }
    memcpy(buffer, event_json, length);
    buffer[length] = '\0';

    if (strstr(buffer, "\"type\":\"m.room.message\"") == nullptr) {
        OPENSSL_cleanse(buffer, length);
        GC_free(buffer);
        return;
    }

    char event_id[128];
    if (!matrix_json_extract_string(buffer, "\"event_id\"", event_id,
                                    sizeof(event_id))) {
        OPENSSL_cleanse(buffer, length);
        GC_free(buffer);
        return;
    }

    if (matrix_client_event_recent(client, event_id)) {
        OPENSSL_cleanse(buffer, length);
        GC_free(buffer);
        return;
    }

    const char *content_pos = strstr(buffer, "\"body\"");
    if (content_pos == nullptr) {
        matrix_client_record_event(client, event_id);
        OPENSSL_cleanse(buffer, length);
        GC_free(buffer);
        return;
    }

    const char *colon = strchr(content_pos, ':');
    if (colon == nullptr) {
        matrix_client_record_event(client, event_id);
        OPENSSL_cleanse(buffer, length);
        GC_free(buffer);
        return;
    }
    colon = matrix_skip_whitespace(colon + 1);
    if (colon == nullptr || *colon != '"') {
        matrix_client_record_event(client, event_id);
        OPENSSL_cleanse(buffer, length);
        GC_free(buffer);
        return;
    }

    char body[SSH_CHATTER_MESSAGE_LIMIT * 4];
    if (!matrix_json_decode_string(colon, body, sizeof(body), nullptr)) {
        matrix_client_record_event(client, event_id);
        OPENSSL_cleanse(buffer, length);
        GC_free(buffer);
        return;
    }

    matrix_client_handle_body(client, body);
    matrix_client_record_event(client, event_id);
    OPENSSL_cleanse(buffer, length);
    GC_free(buffer);
}

static void matrix_client_process_sync(matrix_client_t *client,
                                       const char *json)
{
    if (client == nullptr || json == nullptr) {
        return;
    }

    char next_batch[512];
    if (matrix_json_extract_string(json, "\"next_batch\"", next_batch,
                                   sizeof(next_batch))) {
        pthread_mutex_lock(&client->lock);
        snprintf(client->since_token, sizeof(client->since_token), "%s",
                 next_batch);
        pthread_mutex_unlock(&client->lock);
    }

    char pattern[512];
    snprintf(pattern, sizeof(pattern), "\"%s\":{\"timeline\"", client->room_id);
    const char *room_block = strstr(json, pattern);
    if (room_block == nullptr) {
        return;
    }

    const char *events_pos = strstr(room_block, "\"events\":[");
    if (events_pos == nullptr) {
        return;
    }

    const char *array_start = strchr(events_pos, '[');
    if (array_start == nullptr) {
        return;
    }

    const char *array_end = matrix_find_matching(array_start, '[', ']');
    if (array_end == nullptr) {
        return;
    }

    const char *cursor = array_start + 1;
    while (cursor < array_end) {
        cursor = matrix_skip_whitespace(cursor);
        if (cursor == nullptr || cursor >= array_end) {
            break;
        }
        if (*cursor != '{') {
            ++cursor;
            continue;
        }
        const char *event_end = matrix_find_matching(cursor, '{', '}');
        if (event_end == nullptr || event_end > array_end) {
            break;
        }
        matrix_client_process_event(client, cursor,
                                    (size_t)(event_end - cursor + 1));
        cursor = event_end + 1;
    }
}

static bool matrix_client_sync(matrix_client_t *client)
{
    if (client == nullptr) {
        return false;
    }

    if (atomic_load(&client->disabled)) {
        return false;
    }

    char url[768];
    snprintf(url, sizeof(url), "%s/_matrix/client/r0/sync?timeout=%ld",
             client->homeserver, MATRIX_SYNC_TIMEOUT_MS);

    pthread_mutex_lock(&client->lock);
    char since_token[sizeof(client->since_token)];
    snprintf(since_token, sizeof(since_token), "%s", client->since_token);
    pthread_mutex_unlock(&client->lock);

    if (since_token[0] != '\0') {
        CURL *curl = curl_easy_init();
        if (curl == nullptr) {
            errno = ENOMEM;
            return false;
        }
        char *encoded = curl_easy_escape(curl, since_token, 0);
        if (encoded != nullptr) {
            strncat(url, "&since=", sizeof(url) - strlen(url) - 1U);
            strncat(url, encoded, sizeof(url) - strlen(url) - 1U);
            curl_free(encoded);
        }
        curl_easy_cleanup(curl);
    }

    matrix_buffer_t response = {0};
    long status = 0;
    bool ok = matrix_client_issue_request(client, "GET", url, nullptr, &response,
                                          &status);
    if (!ok) {
        int saved_errno = errno;
        char message[256];
        int error_code = (saved_errno != 0) ? saved_errno : EIO;
        if (status == 401) {
            snprintf(
                message, sizeof(message),
                "matrix sync authentication failed; matrix bridge disabled");
            error_code = EACCES;
        } else {
            snprintf(message, sizeof(message),
                     "matrix sync failed (HTTP %ld); matrix bridge disabled",
                     status);
        }
        matrix_buffer_free(&response);
        matrix_client_disable(client, message, error_code);
        return false;
    }

    matrix_client_process_sync(client, response.data);
    matrix_buffer_free(&response);
    return true;
}

static void *matrix_client_poll_thread(void *user_data)
{
    matrix_client_t *client = (matrix_client_t *)user_data;
    if (client == nullptr) {
        return nullptr;
    }

    if (client->host == nullptr) {
        humanized_log_error("matrix", "matrix poll thread missing host context",
                            EINVAL);
        atomic_store(&client->running, false);
        return nullptr;
    }

    sshc_memory_context_t *memory_scope =
        sshc_memory_context_push(client->host->memory_context);
    atomic_store(&client->running, true);

    while (!atomic_load(&client->stop)) {
        if (!matrix_client_sync(client)) {
            struct timespec delay = {
                .tv_sec = MATRIX_SYNC_BACKOFF_SECONDS,
                .tv_nsec = 0L,
            };
            struct timespec request = delay;
            struct timespec remaining = {0};
            while (nanosleep(&request, &remaining) != 0) {
                if (errno != EINTR) {
                    break;
                }
                request = remaining;
            }
        }
    }

    atomic_store(&client->running, false);
    if (memory_scope != nullptr) {
        sshc_memory_context_pop(memory_scope);
    }
    return nullptr;
}

matrix_client_t *matrix_client_create(host_t *host, client_manager_t *manager,
                                      security_layer_t *security)
{
    if (host == nullptr || manager == nullptr || security == nullptr ||
        !security->ready) {
        return nullptr;
    }

    const char *homeserver = matrix_getenv("CHATTER_MATRIX_HOMESERVER");
    const char *access_token = matrix_getenv("CHATTER_MATRIX_ACCESS_TOKEN");
    const char *room_id = matrix_getenv("CHATTER_MATRIX_ROOM_ID");
    if (homeserver == nullptr || access_token == nullptr || room_id == nullptr) {
        return nullptr;
    }

    char trimmed_homeserver[sizeof(((matrix_client_t *)0)->homeserver)];
    if (!matrix_copy_trimmed(trimmed_homeserver, sizeof(trimmed_homeserver),
                             homeserver) ||
        trimmed_homeserver[0] == '\0') {
        return nullptr;
    }

    const char *homeserver_value = trimmed_homeserver;
    char normalized_homeserver[sizeof(((matrix_client_t *)0)->homeserver)];
    if (strstr(trimmed_homeserver, "://") == nullptr) {
        int written =
            snprintf(normalized_homeserver, sizeof(normalized_homeserver),
                     "https://%s", trimmed_homeserver);
        if (written < 0 || (size_t)written >= sizeof(normalized_homeserver)) {
            return nullptr;
        }
        homeserver_value = normalized_homeserver;
    }

    if (!matrix_probe_homeserver(homeserver_value)) {
        return nullptr;
    }

    matrix_client_t *client =
        (matrix_client_t *)calloc(1U, sizeof(matrix_client_t));
    if (client == nullptr) {
        return nullptr;
    }

    client->host = host;
    client->manager = manager;
    client->security = security;
    snprintf(client->homeserver, sizeof(client->homeserver), "%s",
             homeserver_value);
    size_t base_len = strlen(client->homeserver);
    if (base_len > 0U && client->homeserver[base_len - 1U] == '/') {
        client->homeserver[base_len - 1U] = '\0';
    }
    if (!matrix_copy_trimmed(client->access_token, sizeof(client->access_token),
                             access_token) ||
        client->access_token[0] == '\0') {
        OPENSSL_cleanse(client->access_token, sizeof(client->access_token));
        GC_FREE(client);
        return nullptr;
    }
    if (!matrix_copy_trimmed(client->room_id, sizeof(client->room_id),
                             room_id) ||
        client->room_id[0] == '\0') {
        OPENSSL_cleanse(client->access_token, sizeof(client->access_token));
        GC_FREE(client);
        return nullptr;
    }
    const char *device = matrix_getenv("CHATTER_MATRIX_DEVICE_NAME");
    if (device != nullptr) {
        matrix_copy_trimmed(client->device_name, sizeof(client->device_name),
                            device);
    }
    if (client->device_name[0] == '\0') {
        snprintf(client->device_name, sizeof(client->device_name), "%s",
                 "ssh-chatter");
    }
    client->since_token[0] = '\0';
    client->next_txn_id = (uint64_t)time(nullptr);
    client->pending_skip_username[0] = '\0';
    client->pending_skip_message[0] = '\0';
    atomic_store(&client->skip_next_broadcast, false);
    atomic_store(&client->disabled, false);
    client->recent_event_count = 0U;
    client->recent_event_head = 0U;

    if (nullptr != 0) {
        OPENSSL_cleanse(client->access_token, sizeof(client->access_token));
        GC_FREE(client);
        return nullptr;
    }
    client->lock_initialized = true;

    if (pthread_mutex_init(&client->http_lock, nullptr) != 0) {
        pthread_mutex_destroy(&client->lock);
        client->lock_initialized = false;
        OPENSSL_cleanse(client->access_token, sizeof(client->access_token));
        GC_FREE(client);
        return nullptr;
    }
    client->http_lock_initialized = true;

    memset(&client->connection, 0, sizeof(client->connection));
    client->connection.kind = CLIENT_KIND_BOT;
    snprintf(client->connection.identifier,
             sizeof(client->connection.identifier), "%s", "matrix");
    client->connection.receive_system_messages = true;
    client->connection.on_message = matrix_client_on_message;
    client->connection.on_detach = matrix_client_on_detach;
    client->connection.user_data = client;

    if (!client_manager_register(manager, &client->connection)) {
        if (client->http_lock_initialized) {
            pthread_mutex_destroy(&client->http_lock);
            client->http_lock_initialized = false;
        }
        if (client->lock_initialized) {
            pthread_mutex_destroy(&client->lock);
            client->lock_initialized = false;
        }
        OPENSSL_cleanse(client->access_token, sizeof(client->access_token));
        GC_FREE(client);
        return nullptr;
    }

    atomic_store(&client->stop, false);
    atomic_store(&client->running, false);
    if (pthread_create(&client->thread, nullptr, matrix_client_poll_thread,
                       client) != 0) {
        client_manager_unregister(manager, &client->connection);
        if (client->http_lock_initialized) {
            pthread_mutex_destroy(&client->http_lock);
            client->http_lock_initialized = false;
        }
        if (client->lock_initialized) {
            pthread_mutex_destroy(&client->lock);
            client->lock_initialized = false;
        }
        OPENSSL_cleanse(client->access_token, sizeof(client->access_token));
        GC_FREE(client);
        return nullptr;
    }

    client->thread_initialized = true;
    printf("[matrix] matrix bridge enabled for room %s\n", client->room_id);
    return client;
}

void matrix_client_destroy(matrix_client_t *client)
{
    if (client == nullptr) {
        return;
    }

    if (client->thread_initialized) {
        atomic_store(&client->stop, true);
        pthread_join(client->thread, nullptr);
        client->thread_initialized = false;
    }

    if (client->manager != nullptr) {
        client_manager_unregister(client->manager, &client->connection);
    }

    if (client->lock_initialized) {
        pthread_mutex_destroy(&client->lock);
        client->lock_initialized = false;
    }
    if (client->http_lock_initialized) {
        pthread_mutex_destroy(&client->http_lock);
        client->http_lock_initialized = false;
    }
    OPENSSL_cleanse(client->access_token, sizeof(client->access_token));
    OPENSSL_cleanse(client->pending_skip_username,
                    sizeof(client->pending_skip_username));
    OPENSSL_cleanse(client->pending_skip_message,
                    sizeof(client->pending_skip_message));
    GC_FREE(client);
}

/**
 * @file host_json_api.c
 * @desc File-level documentation for host_json_api.c, describing its role
 *       in the SSH-Chatter server and providing a consistent header
 *       comment format across C sources.
 * @return None.
 */

// JSON line-based API for chat commands and events.
#include "host_internal.h"

typedef struct json_builder {
    char *data;
    size_t len;
    size_t cap;
} json_builder_t;

typedef struct json_api_client {
    host_t *host;
    client_manager_t *manager;
    client_connection_t connection;
    int fd;
    pthread_t thread;
    bool thread_initialized;
    _Atomic bool stop;
    pthread_mutex_t write_lock;
    bool write_lock_initialized;
    char peer_ip[SSH_CHATTER_IP_LEN];
} json_api_client_t;

typedef struct json_api_request {
    char type[32];
    char username[SSH_CHATTER_USERNAME_LEN];
    char message[SSH_CHATTER_MESSAGE_LIMIT];
    char url[SSH_CHATTER_ATTACHMENT_TARGET_LEN];
    char caption[SSH_CHATTER_ATTACHMENT_CAPTION_LEN];
    char label[SSH_CHATTER_POLL_LABEL_LEN];
    char question[SSH_CHATTER_MESSAGE_LIMIT];
    char options[5][SSH_CHATTER_MESSAGE_LIMIT];
    size_t option_count;
    char action[16];
    size_t choice;
    bool has_choice;
    bool allow_multiple;
    bool is_operator;
    char request_id[64];
    bool request_id_is_number;
    bool has_request_id;
} json_api_request_t;

static bool host_prepare_chat_entry(host_t *host, const char *username,
                                    const char *message, const char *color_name,
                                    const char *highlight_name, bool is_bold,
                                    chat_history_entry_t *entry);
static bool host_history_commit_entry(host_t *host, chat_history_entry_t *entry,
                                      chat_history_entry_t *stored_entry);
static void host_notify_external_clients(host_t *host,
                                         const chat_history_entry_t *entry);
static void chat_room_broadcast_entry(chat_room_t *room,
                                      const chat_history_entry_t *entry,
                                      const session_ctx_t *exclude);
static void chat_room_broadcast(chat_room_t *room, const char *message,
                                const session_ctx_t *exclude);
static bool host_history_record_system(host_t *host, const char *message,
                                       chat_history_entry_t *stored_entry);
static user_preference_t *host_ensure_preference_locked(host_t *host,
                                                        const char *username,
                                                        const char *ip);
static void host_vote_state_save_locked(host_t *host);
static void host_state_save_locked(host_t *host);
static named_poll_state_t *host_find_named_poll_locked(host_t *host,
                                                       const char *label);
static named_poll_state_t *host_ensure_named_poll_locked(host_t *host,
                                                         const char *label);
static void host_recount_named_polls_locked(host_t *host);
static void named_poll_reset(named_poll_state_t *poll);
static bool poll_label_is_valid(const char *label);
static void poll_state_reset(poll_state_t *poll);
static bool host_asciiart_cooldown_active(host_t *host, const char *ip,
                                          const struct timespec *now,
                                          long *remaining_seconds);
static void host_asciiart_register_post(host_t *host, const char *ip,
                                        const struct timespec *when);

static void json_api_sleep_before_restart(unsigned int attempts)
{
    struct timespec restart_delay = {
        .tv_sec = attempts < 5U ? 1L : (attempts < 10U ? 5L : 30L),
        .tv_nsec = 0L,
    };

    struct timespec request = restart_delay;
    struct timespec remaining = {0};

    while (nanosleep(&request, &remaining) != 0) {
        if (errno != EINTR) {
            break;
        }
        request = remaining;
    }
}

static void json_builder_init(json_builder_t *builder)
{
    if (builder == nullptr) {
        return;
    }
    builder->data = nullptr;
    builder->len = 0U;
    builder->cap = 0U;
}

static void json_builder_free(json_builder_t *builder)
{
    if (builder == nullptr) {
        return;
    }
    if (builder->data != nullptr) {
        free(builder->data);
    }
    builder->data = nullptr;
    builder->len = 0U;
    builder->cap = 0U;
}

static bool json_builder_reserve(json_builder_t *builder, size_t extra)
{
    if (builder == nullptr) {
        return false;
    }
    size_t required = builder->len + extra + 1U;
    if (required <= builder->cap) {
        return true;
    }
    size_t new_cap = builder->cap > 0U ? builder->cap : 256U;
    while (new_cap < required) {
        new_cap *= 2U;
    }
    char *next = (char *)realloc(builder->data, new_cap);
    if (next == nullptr) {
        return false;
    }
    builder->data = next;
    builder->cap = new_cap;
    return true;
}

static bool json_builder_append(json_builder_t *builder, const char *fmt, ...)
{
    if (builder == nullptr || fmt == nullptr) {
        return false;
    }

    va_list args;
    va_start(args, fmt);
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(nullptr, 0, fmt, args);
    va_end(args);

    if (needed < 0) {
        va_end(args_copy);
        return false;
    }

    if (!json_builder_reserve(builder, (size_t)needed)) {
        va_end(args_copy);
        return false;
    }

    vsnprintf(builder->data + builder->len, builder->cap - builder->len, fmt,
              args_copy);
    va_end(args_copy);
    builder->len += (size_t)needed;
    return true;
}

static int json_api_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool json_api_append_utf8(char **out, size_t *remaining,
                                 uint32_t codepoint)
{
    if (out == nullptr || *out == nullptr || remaining == nullptr) {
        return false;
    }

    unsigned char buffer[4];
    size_t count = 0U;

    if (codepoint <= 0x7FU) {
        buffer[0] = (unsigned char)codepoint;
        count = 1U;
    } else if (codepoint <= 0x7FFU) {
        buffer[0] = (unsigned char)(0xC0U | ((codepoint >> 6) & 0x1FU));
        buffer[1] = (unsigned char)(0x80U | (codepoint & 0x3FU));
        count = 2U;
    } else if (codepoint <= 0xFFFFU) {
        buffer[0] = (unsigned char)(0xE0U | ((codepoint >> 12) & 0x0FU));
        buffer[1] = (unsigned char)(0x80U | ((codepoint >> 6) & 0x3FU));
        buffer[2] = (unsigned char)(0x80U | (codepoint & 0x3FU));
        count = 3U;
    } else if (codepoint <= 0x10FFFFU) {
        buffer[0] = (unsigned char)(0xF0U | ((codepoint >> 18) & 0x07U));
        buffer[1] = (unsigned char)(0x80U | ((codepoint >> 12) & 0x3FU));
        buffer[2] = (unsigned char)(0x80U | ((codepoint >> 6) & 0x3FU));
        buffer[3] = (unsigned char)(0x80U | (codepoint & 0x3FU));
        count = 4U;
    } else {
        return false;
    }

    if (*remaining < count) {
        return false;
    }

    memcpy(*out, buffer, count);
    *out += count;
    *remaining -= count;
    return true;
}

static bool json_api_decode_string(const char *input, char *output,
                                   size_t output_len, const char **next)
{
    if (input == nullptr || output == nullptr || output_len == 0U) {
        return false;
    }

    if (*input != '"') {
        return false;
    }

    const char *cursor = input + 1;
    char *out_cursor = output;
    size_t remaining = output_len - 1U;

    while (*cursor != '\0') {
        if (*cursor == '"') {
            *out_cursor = '\0';
            if (next != nullptr) {
                *next = cursor + 1;
            }
            return true;
        }

        if (*cursor == '\\') {
            cursor++;
            if (*cursor == '\0') {
                return false;
            }
            switch (*cursor) {
            case '"':
                if (remaining < 1U) {
                    return false;
                }
                *out_cursor++ = '"';
                remaining--;
                break;
            case '\\':
                if (remaining < 1U) {
                    return false;
                }
                *out_cursor++ = '\\';
                remaining--;
                break;
            case '/':
                if (remaining < 1U) {
                    return false;
                }
                *out_cursor++ = '/';
                remaining--;
                break;
            case 'b':
                if (remaining < 1U) {
                    return false;
                }
                *out_cursor++ = '\b';
                remaining--;
                break;
            case 'f':
                if (remaining < 1U) {
                    return false;
                }
                *out_cursor++ = '\f';
                remaining--;
                break;
            case 'n':
                if (remaining < 1U) {
                    return false;
                }
                *out_cursor++ = '\n';
                remaining--;
                break;
            case 'r':
                if (remaining < 1U) {
                    return false;
                }
                *out_cursor++ = '\r';
                remaining--;
                break;
            case 't':
                if (remaining < 1U) {
                    return false;
                }
                *out_cursor++ = '\t';
                remaining--;
                break;
            case 'u': {
                uint32_t codepoint = 0U;
                for (int idx = 0; idx < 4; ++idx) {
                    int hex = json_api_hex_value(cursor[1 + idx]);
                    if (hex < 0) {
                        return false;
                    }
                    codepoint = (codepoint << 4) | (uint32_t)hex;
                }
                cursor += 4;

                if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                    if (cursor[1] == '\\' && cursor[2] == 'u') {
                        uint32_t low = 0U;
                        for (int idx = 0; idx < 4; ++idx) {
                            int hex = json_api_hex_value(cursor[3 + idx]);
                            if (hex < 0) {
                                return false;
                            }
                            low = (low << 4) | (uint32_t)hex;
                        }
                        if (low >= 0xDC00U && low <= 0xDFFFU) {
                            codepoint =
                                0x10000U +
                                ((codepoint - 0xD800U) << 10) +
                                (low - 0xDC00U);
                            cursor += 6;
                        }
                    }
                }

                if (!json_api_append_utf8(&out_cursor, &remaining, codepoint)) {
                    return false;
                }
                break;
            }
            default:
                return false;
            }
            cursor++;
            continue;
        }

        if ((unsigned char)*cursor < 0x20U) {
            return false;
        }

        if (remaining < 1U) {
            return false;
        }
        *out_cursor++ = *cursor++;
        remaining--;
    }

    return false;
}

static char *json_api_escape_string(const char *input)
{
    if (input == nullptr) {
        return nullptr;
    }

    size_t length = strlen(input);
    size_t max_len = length * 6U + 1U;
    char *output = (char *)malloc(max_len);
    if (output == nullptr) {
        return nullptr;
    }

    char *cursor = output;
    for (size_t idx = 0U; idx < length; ++idx) {
        unsigned char ch = (unsigned char)input[idx];
        switch (ch) {
        case '"':
            *cursor++ = '\\';
            *cursor++ = '"';
            break;
        case '\\':
            *cursor++ = '\\';
            *cursor++ = '\\';
            break;
        case '\b':
            *cursor++ = '\\';
            *cursor++ = 'b';
            break;
        case '\f':
            *cursor++ = '\\';
            *cursor++ = 'f';
            break;
        case '\n':
            *cursor++ = '\\';
            *cursor++ = 'n';
            break;
        case '\r':
            *cursor++ = '\\';
            *cursor++ = 'r';
            break;
        case '\t':
            *cursor++ = '\\';
            *cursor++ = 't';
            break;
        default:
            if (ch < 0x20U) {
                snprintf(cursor, 7, "\\u%04x", ch);
                cursor += 6;
            } else {
                *cursor++ = (char)ch;
            }
            break;
        }
    }
    *cursor = '\0';
    return output;
}

static const char *json_api_find_key(const char *json, const char *key)
{
    if (json == nullptr || key == nullptr) {
        return nullptr;
    }

    const char *key_pos = strstr(json, key);
    if (key_pos == nullptr) {
        return nullptr;
    }

    const char *colon = strchr(key_pos + strlen(key), ':');
    if (colon == nullptr) {
        return nullptr;
    }

    return colon + 1;
}

static const char *json_api_skip_whitespace(const char *cursor)
{
    while (cursor != nullptr && (*cursor == ' ' || *cursor == '\t' ||
                                 *cursor == '\r' || *cursor == '\n')) {
        cursor++;
    }
    return cursor;
}

static bool json_api_extract_string(const char *json, const char *key,
                                    char *output, size_t output_len)
{
    if (output == nullptr || output_len == 0U) {
        return false;
    }
    output[0] = '\0';

    const char *cursor = json_api_find_key(json, key);
    if (cursor == nullptr) {
        return false;
    }

    cursor = json_api_skip_whitespace(cursor);
    if (cursor == nullptr || *cursor != '"') {
        return false;
    }

    return json_api_decode_string(cursor, output, output_len, nullptr);
}

static bool json_api_extract_bool(const char *json, const char *key,
                                  bool *value_out)
{
    if (value_out == nullptr) {
        return false;
    }

    const char *cursor = json_api_find_key(json, key);
    if (cursor == nullptr) {
        return false;
    }

    cursor = json_api_skip_whitespace(cursor);
    if (cursor == nullptr) {
        return false;
    }

    if (strncmp(cursor, "true", 4) == 0) {
        *value_out = true;
        return true;
    }
    if (strncmp(cursor, "false", 5) == 0) {
        *value_out = false;
        return true;
    }

    return false;
}

static bool json_api_extract_uint(const char *json, const char *key,
                                  unsigned long *value_out)
{
    if (value_out == nullptr) {
        return false;
    }

    const char *cursor = json_api_find_key(json, key);
    if (cursor == nullptr) {
        return false;
    }

    cursor = json_api_skip_whitespace(cursor);
    if (cursor == nullptr) {
        return false;
    }

    char *endptr = nullptr;
    unsigned long value = strtoul(cursor, &endptr, 10);
    if (endptr == cursor) {
        return false;
    }

    *value_out = value;
    return true;
}

static bool json_api_extract_string_array(const char *json, const char *key,
                                          char items[][SSH_CHATTER_MESSAGE_LIMIT],
                                          size_t max_items,
                                          size_t *item_count)
{
    if (items == nullptr || max_items == 0U || item_count == nullptr) {
        return false;
    }
    *item_count = 0U;

    const char *cursor = json_api_find_key(json, key);
    if (cursor == nullptr) {
        return false;
    }

    cursor = json_api_skip_whitespace(cursor);
    if (cursor == nullptr || *cursor != '[') {
        return false;
    }
    cursor++;

    while (*cursor != '\0') {
        cursor = json_api_skip_whitespace(cursor);
        if (*cursor == ']') {
            return true;
        }

        if (*cursor != '"') {
            return false;
        }

        if (*item_count >= max_items) {
            return false;
        }

        if (!json_api_decode_string(cursor,
                                    items[*item_count],
                                    SSH_CHATTER_MESSAGE_LIMIT,
                                    &cursor)) {
            return false;
        }

        (*item_count)++;
        cursor = json_api_skip_whitespace(cursor);
        if (*cursor == ',') {
            cursor++;
            continue;
        }
        if (*cursor == ']') {
            return true;
        }
        return false;
    }

    return false;
}

static void json_api_normalize_token(char *text)
{
    if (text == nullptr) {
        return;
    }

    size_t len = strlen(text);
    for (size_t idx = 0U; idx < len; ++idx) {
        text[idx] = (char)tolower((unsigned char)text[idx]);
    }

    if (text[0] == '/') {
        memmove(text, text + 1, len);
    }
}

static bool json_api_parse_request(const char *line, json_api_request_t *request,
                                   char *error, size_t error_len)
{
    if (request == nullptr) {
        return false;
    }

    memset(request, 0, sizeof(*request));
    request->allow_multiple = true;

    if (error != nullptr && error_len > 0U) {
        error[0] = '\0';
    }

    char type[32] = "";
    if (!json_api_extract_string(line, "\"type\"", type, sizeof(type))) {
        char command[32] = "";
        if (json_api_extract_string(line, "\"command\"", command,
                                    sizeof(command))) {
            snprintf(type, sizeof(type), "%s", command);
        } else {
            if (error != nullptr && error_len > 0U) {
                snprintf(error, error_len, "Missing request type.");
            }
            return false;
        }
    }
    json_api_normalize_token(type);
    snprintf(request->type, sizeof(request->type), "%s", type);

    if (json_api_extract_string(line, "\"username\"", request->username,
                                sizeof(request->username))) {
        trim_whitespace_inplace(request->username);
    }

    (void)json_api_extract_string(line, "\"message\"", request->message,
                                  sizeof(request->message));
    (void)json_api_extract_string(line, "\"url\"", request->url,
                                  sizeof(request->url));
    (void)json_api_extract_string(line, "\"caption\"", request->caption,
                                  sizeof(request->caption));
    (void)json_api_extract_string(line, "\"label\"", request->label,
                                  sizeof(request->label));
    (void)json_api_extract_string(line, "\"question\"", request->question,
                                  sizeof(request->question));
    (void)json_api_extract_string(line, "\"action\"", request->action,
                                  sizeof(request->action));

    if (request->action[0] != '\0') {
        json_api_normalize_token(request->action);
    }

    if (json_api_extract_string_array(line, "\"options\"", request->options,
                                      sizeof(request->options) /
                                          sizeof(request->options[0]),
                                      &request->option_count)) {
        for (size_t idx = 0U; idx < request->option_count; ++idx) {
            trim_whitespace_inplace(request->options[idx]);
        }
    }

    unsigned long choice = 0UL;
    if (json_api_extract_uint(line, "\"choice\"", &choice)) {
        request->choice = (size_t)choice;
        request->has_choice = true;
    }

    bool allow_multiple = false;
    if (json_api_extract_bool(line, "\"allow_multiple\"", &allow_multiple)) {
        request->allow_multiple = allow_multiple;
    }

    bool is_operator = false;
    if (json_api_extract_bool(line, "\"is_operator\"", &is_operator) ||
        json_api_extract_bool(line, "\"operator\"", &is_operator)) {
        request->is_operator = is_operator;
    }

    if (json_api_extract_string(line, "\"id\"", request->request_id,
                                sizeof(request->request_id))) {
        request->has_request_id = true;
        request->request_id_is_number = false;
    } else {
        unsigned long request_id = 0UL;
        if (json_api_extract_uint(line, "\"id\"", &request_id)) {
            snprintf(request->request_id, sizeof(request->request_id), "%lu",
                     request_id);
            request->has_request_id = true;
            request->request_id_is_number = true;
        }
    }

    return true;
}

static bool json_api_send_line(json_api_client_t *client, const char *line)
{
    if (client == nullptr || line == nullptr || client->fd < 0) {
        return false;
    }

    if (!client->write_lock_initialized) {
        return false;
    }

    pthread_mutex_lock(&client->write_lock);
    size_t length = strlen(line);
    size_t offset = 0U;
    bool success = true;

    while (offset < length) {
        ssize_t written =
            send(client->fd, line + offset, length - offset, MSG_NOSIGNAL);
        if (written <= 0) {
            success = false;
            break;
        }
        offset += (size_t)written;
    }

    if (success) {
        const char newline = '\n';
        if (send(client->fd, &newline, 1, MSG_NOSIGNAL) != 1) {
            success = false;
        }
    }

    pthread_mutex_unlock(&client->write_lock);
    return success;
}

static void json_api_send_response(json_api_client_t *client,
                                   const json_api_request_t *request,
                                   bool ok, const char *message,
                                   const char *result_json)
{
    if (client == nullptr) {
        return;
    }

    const char *status = ok ? "ok" : "error";
    char *escaped_message = json_api_escape_string(message != nullptr
                                                       ? message
                                                       : (ok ? "ok" : "error"));
    if (escaped_message == nullptr) {
        return;
    }

    json_builder_t builder;
    json_builder_init(&builder);
    json_builder_append(&builder, "{\"type\":\"response\"");

    if (request != nullptr && request->has_request_id) {
        if (request->request_id_is_number) {
            json_builder_append(&builder, ",\"id\":%s", request->request_id);
        } else {
            json_builder_append(&builder, ",\"id\":\"%s\"",
                                request->request_id);
        }
    }

    json_builder_append(&builder, ",\"status\":\"%s\",\"message\":\"%s\"",
                        status, escaped_message);

    if (result_json != nullptr && result_json[0] != '\0') {
        json_builder_append(&builder, ",\"result\":%s", result_json);
    }

    json_builder_append(&builder, "}");

    if (builder.data != nullptr) {
        (void)json_api_send_line(client, builder.data);
    }

    json_builder_free(&builder);
    free(escaped_message);
}

static const char *json_api_attachment_type_label(chat_attachment_type_t type)
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
    default:
        return "none";
    }
}

static void json_api_on_message(client_connection_t *connection,
                                const chat_history_entry_t *entry)
{
    if (connection == nullptr || entry == nullptr || connection->user_data == nullptr) {
        return;
    }

    json_api_client_t *client = (json_api_client_t *)connection->user_data;

    char *escaped_username = json_api_escape_string(entry->username);
    char *escaped_message = json_api_escape_string(entry->message);
    char *escaped_target = json_api_escape_string(entry->attachment_target);
    char *escaped_caption = json_api_escape_string(entry->attachment_caption);

    if (escaped_username == nullptr || escaped_message == nullptr ||
        escaped_target == nullptr || escaped_caption == nullptr) {
        free(escaped_username);
        free(escaped_message);
        free(escaped_target);
        free(escaped_caption);
        return;
    }

    json_builder_t builder;
    json_builder_init(&builder);
    json_builder_append(&builder,
                        "{\"type\":\"event\",\"event\":\"message\","
                        "\"payload\":{"
                        "\"id\":%" PRIu64
                        ",\"username\":\"%s\",\"message\":\"%s\","
                        "\"created_at\":%lld,\"system\":%s,"
                        "\"preserve_whitespace\":%s,"
                        "\"attachment\":{\"type\":\"%s\","
                        "\"target\":\"%s\",\"caption\":\"%s\"}}}",
                        entry->message_id, escaped_username, escaped_message,
                        (long long)entry->created_at,
                        entry->is_user_message ? "false" : "true",
                        entry->preserve_whitespace ? "true" : "false",
                        json_api_attachment_type_label(entry->attachment_type),
                        escaped_target, escaped_caption);

    if (builder.data != nullptr) {
        if (!json_api_send_line(client, builder.data)) {
            atomic_store(&client->stop, true);
            shutdown(client->fd, SHUT_RDWR);
        }
    }

    json_builder_free(&builder);
    free(escaped_username);
    free(escaped_message);
    free(escaped_target);
    free(escaped_caption);
}

static void json_api_on_detach(client_connection_t *connection)
{
    if (connection == nullptr || connection->user_data == nullptr) {
        return;
    }

    json_api_client_t *client = (json_api_client_t *)connection->user_data;
    atomic_store(&client->stop, true);
    if (client->fd >= 0) {
        shutdown(client->fd, SHUT_RDWR);
    }
}

static bool json_api_post_attachment(host_t *host, const char *username,
                                     chat_attachment_type_t type,
                                     const char *url, const char *caption,
                                     const char *activity_message,
                                     char *error, size_t error_len)
{
    if (error != nullptr && error_len > 0U) {
        error[0] = '\0';
    }

    if (host == nullptr || username == nullptr || username[0] == '\0' ||
        url == nullptr || url[0] == '\0') {
        if (error != nullptr && error_len > 0U) {
            snprintf(error, error_len, "Missing required fields.");
        }
        return false;
    }

    if (strnlen(url, SSH_CHATTER_ATTACHMENT_TARGET_LEN) >=
        SSH_CHATTER_ATTACHMENT_TARGET_LEN) {
        if (error != nullptr && error_len > 0U) {
            snprintf(error, error_len, "URL is too long.");
        }
        return false;
    }

    chat_history_entry_t entry = {0};
    if (!host_prepare_chat_entry(host, username, activity_message, nullptr,
                                 nullptr, false, &entry)) {
        if (error != nullptr && error_len > 0U) {
            snprintf(error, error_len, "Unable to prepare entry.");
        }
        return false;
    }

    entry.attachment_type = type;
    snprintf(entry.attachment_target, sizeof(entry.attachment_target), "%s",
             url);
    if (caption != nullptr && caption[0] != '\0') {
        snprintf(entry.attachment_caption, sizeof(entry.attachment_caption),
                 "%s", caption);
    }

    chat_history_entry_t stored = {0};
    if (!host_history_commit_entry(host, &entry, &stored)) {
        if (error != nullptr && error_len > 0U) {
            snprintf(error, error_len, "Unable to record attachment.");
        }
        return false;
    }

    chat_room_broadcast_entry(&host->room, &stored, nullptr);
    host_notify_external_clients(host, &stored);
    return true;
}

static bool json_api_post_asciiart(host_t *host, const char *username,
                                   const char *art, const char *ip,
                                   char *error, size_t error_len)
{
    if (error != nullptr && error_len > 0U) {
        error[0] = '\0';
    }

    if (host == nullptr || username == nullptr || username[0] == '\0' ||
        art == nullptr || art[0] == '\0') {
        if (error != nullptr && error_len > 0U) {
            snprintf(error, error_len, "Missing ASCII art content.");
        }
        return false;
    }

    if (strnlen(art, SSH_CHATTER_MESSAGE_LIMIT) >= SSH_CHATTER_MESSAGE_LIMIT) {
        if (error != nullptr && error_len > 0U) {
            snprintf(error, error_len, "ASCII art is too long.");
        }
        return false;
    }

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    long remaining_seconds = 0L;
    if (host_asciiart_cooldown_active(host, ip, &now, &remaining_seconds)) {
        if (error != nullptr && error_len > 0U) {
            snprintf(error, error_len,
                     "ASCII art cooldown active (%ld seconds remaining).",
                     remaining_seconds);
        }
        return false;
    }

    chat_history_entry_t entry = {0};
    if (!host_prepare_chat_entry(host, username, art, nullptr, nullptr, false,
                                 &entry)) {
        if (error != nullptr && error_len > 0U) {
            snprintf(error, error_len, "Unable to prepare entry.");
        }
        return false;
    }
    entry.preserve_whitespace = true;

    chat_history_entry_t stored = {0};
    if (!host_history_commit_entry(host, &entry, &stored)) {
        if (error != nullptr && error_len > 0U) {
            snprintf(error, error_len, "Unable to record ASCII art.");
        }
        return false;
    }

    host_asciiart_register_post(host, ip, &now);
    chat_room_broadcast_entry(&host->room, &stored, nullptr);
    host_notify_external_clients(host, &stored);
    return true;
}

static char *json_api_build_poll_json(const poll_state_t *poll)
{
    if (poll == nullptr) {
        return nullptr;
    }

    char *escaped_question = json_api_escape_string(poll->question);
    if (escaped_question == nullptr) {
        return nullptr;
    }

    json_builder_t builder;
    json_builder_init(&builder);
    json_builder_append(&builder,
                        "{\"active\":%s,\"allow_multiple\":%s,"
                        "\"id\":%" PRIu64 ",\"question\":\"%s\","
                        "\"options\":[",
                        poll->active ? "true" : "false",
                        poll->allow_multiple ? "true" : "false", poll->id,
                        escaped_question);

    for (size_t idx = 0U; idx < poll->option_count; ++idx) {
        char *escaped_option = json_api_escape_string(poll->options[idx].text);
        if (escaped_option == nullptr) {
            json_builder_free(&builder);
            free(escaped_question);
            return nullptr;
        }
        json_builder_append(&builder,
                            "%s{\"index\":%zu,\"text\":\"%s\","
                            "\"votes\":%u}",
                            idx == 0U ? "" : ",", idx + 1U, escaped_option,
                            poll->options[idx].votes);
        free(escaped_option);
    }

    json_builder_append(&builder, "]}");
    free(escaped_question);

    if (builder.data == nullptr) {
        json_builder_free(&builder);
        return nullptr;
    }

    return builder.data;
}

static char *json_api_build_named_poll_json(const named_poll_state_t *poll)
{
    if (poll == nullptr) {
        return nullptr;
    }

    char *escaped_label = json_api_escape_string(poll->label);
    char *escaped_owner = json_api_escape_string(poll->owner);
    if (escaped_label == nullptr || escaped_owner == nullptr) {
        free(escaped_label);
        free(escaped_owner);
        return nullptr;
    }

    char *poll_json = json_api_build_poll_json(&poll->poll);
    if (poll_json == nullptr) {
        free(escaped_label);
        free(escaped_owner);
        return nullptr;
    }

    json_builder_t builder;
    json_builder_init(&builder);
    json_builder_append(&builder,
                        "{\"label\":\"%s\",\"owner\":\"%s\","
                        "\"poll\":%s}",
                        escaped_label, escaped_owner, poll_json);

    free(escaped_label);
    free(escaped_owner);
    free(poll_json);

    if (builder.data == nullptr) {
        json_builder_free(&builder);
        return nullptr;
    }

    return builder.data;
}

static char *json_api_build_named_poll_list(host_t *host)
{
    if (host == nullptr) {
        return nullptr;
    }

    named_poll_state_t snapshot[SSH_CHATTER_MAX_NAMED_POLLS];
    size_t count = 0U;

    pthread_mutex_lock(&host->lock);
    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_NAMED_POLLS; ++idx) {
        if (host->named_polls[idx].label[0] == '\0') {
            continue;
        }
        snapshot[count++] = host->named_polls[idx];
    }
    pthread_mutex_unlock(&host->lock);

    json_builder_t builder;
    json_builder_init(&builder);
    json_builder_append(&builder, "[");

    for (size_t idx = 0U; idx < count; ++idx) {
        char *escaped_label = json_api_escape_string(snapshot[idx].label);
        char *escaped_question =
            json_api_escape_string(snapshot[idx].poll.question);
        if (escaped_label == nullptr || escaped_question == nullptr) {
            free(escaped_label);
            free(escaped_question);
            json_builder_free(&builder);
            return nullptr;
        }
        json_builder_append(&builder,
                            "%s{\"label\":\"%s\",\"active\":%s,"
                            "\"allow_multiple\":%s,\"question\":\"%s\","
                            "\"option_count\":%zu}",
                            idx == 0U ? "" : ",", escaped_label,
                            snapshot[idx].poll.active ? "true" : "false",
                            snapshot[idx].poll.allow_multiple ? "true" :
                                                                     "false",
                            escaped_question, snapshot[idx].poll.option_count);
        free(escaped_label);
        free(escaped_question);
    }

    json_builder_append(&builder, "]");
    if (builder.data == nullptr) {
        json_builder_free(&builder);
        return nullptr;
    }

    return builder.data;
}

static bool json_api_handle_poll_request(json_api_client_t *client,
                                         const json_api_request_t *request)
{
    if (client == nullptr || request == nullptr || client->host == nullptr) {
        return false;
    }

    host_t *host = client->host;
    const char *action = request->action[0] != '\0' ? request->action : nullptr;

    if (action == nullptr || strcmp(action, "status") == 0 ||
        strcmp(action, "list") == 0) {
        poll_state_t snapshot = {0};
        pthread_mutex_lock(&host->lock);
        snapshot = host->poll;
        pthread_mutex_unlock(&host->lock);

        char *poll_json = json_api_build_poll_json(&snapshot);
        if (poll_json == nullptr) {
            json_api_send_response(client, request, false,
                                   "Unable to format poll summary.", nullptr);
            return false;
        }

        json_builder_t result;
        json_builder_init(&result);
        json_builder_append(&result, "{\"poll\":%s}", poll_json);
        json_api_send_response(client, request, true, "poll summary",
                               result.data);
        json_builder_free(&result);
        free(poll_json);
        return true;
    }

    if (strcmp(action, "close") == 0 || strcmp(action, "end") == 0 ||
        strcmp(action, "stop") == 0) {
        if (!request->is_operator) {
            json_api_send_response(client, request, false,
                                   "Only operators may close polls.", nullptr);
            return false;
        }

        bool was_active = false;
        pthread_mutex_lock(&host->lock);
        if (host->poll.active) {
            host->poll.active = false;
            was_active = true;
            host_vote_state_save_locked(host);
        }
        pthread_mutex_unlock(&host->lock);

        if (!was_active) {
            json_api_send_response(client, request, false,
                                   "No active poll to close.", nullptr);
            return false;
        }

        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice), "* [%s] closed the main poll.",
                 request->username);
        host_history_record_system(host, notice, nullptr);
        chat_room_broadcast(&host->room, notice, nullptr);
        json_api_send_response(client, request, true, "poll closed", nullptr);
        return true;
    }

    if (strcmp(action, "vote") == 0 || request->has_choice) {
        if (request->choice == 0U) {
            json_api_send_response(client, request, false,
                                   "Choice must be >= 1.", nullptr);
            return false;
        }

        char response[SSH_CHATTER_MESSAGE_LIMIT];
        response[0] = '\0';

        pthread_mutex_lock(&host->lock);
        poll_state_t *poll = &host->poll;
        if (!poll->active || poll->option_count == 0U) {
            pthread_mutex_unlock(&host->lock);
            json_api_send_response(client, request, false,
                                   "No active poll right now.", nullptr);
            return false;
        }

        size_t option_index = request->choice - 1U;
        if (option_index >= poll->option_count) {
            pthread_mutex_unlock(&host->lock);
            json_api_send_response(client, request, false,
                                   "That poll option does not exist.",
                                   nullptr);
            return false;
        }

        user_preference_t *pref = host_ensure_preference_locked(
            host, request->username, client->peer_ip);
        if (pref == nullptr) {
            pthread_mutex_unlock(&host->lock);
            json_api_send_response(client, request, false,
                                   "Unable to record your vote.", nullptr);
            return false;
        }

        if (pref->last_poll_id != poll->id) {
            pref->last_poll_id = poll->id;
            pref->last_poll_choice = -1;
        }

        if (poll->allow_multiple) {
            uint32_t mask =
                pref->last_poll_choice < 0 ? 0U
                                           : (uint32_t)pref->last_poll_choice;
            uint32_t bit = 1U << option_index;
            if ((mask & bit) != 0U) {
                mask &= ~bit;
                if (poll->options[option_index].votes > 0U) {
                    poll->options[option_index].votes--;
                }
                snprintf(response, sizeof(response),
                         "Removed your vote for option %zu.",
                         option_index + 1U);
            } else {
                mask |= bit;
                poll->options[option_index].votes++;
                snprintf(response, sizeof(response),
                         "Vote recorded for option %zu.", option_index + 1U);
            }
            pref->last_poll_choice = mask == 0U ? -1 : (int32_t)mask;
        } else {
            int32_t previous = pref->last_poll_choice;
            if (previous == (int32_t)option_index) {
                pthread_mutex_unlock(&host->lock);
                json_api_send_response(client, request, false,
                                       "You have already voted for that "
                                       "option.",
                                       nullptr);
                return false;
            }
            if (previous >= 0 && (size_t)previous < poll->option_count &&
                poll->options[previous].votes > 0U) {
                poll->options[previous].votes--;
            }
            poll->options[option_index].votes++;
            pref->last_poll_choice = (int32_t)option_index;
            snprintf(response, sizeof(response), "Vote recorded for option %zu.",
                     option_index + 1U);
        }

        pref->last_poll_id = poll->id;
        host_vote_state_save_locked(host);
        host_state_save_locked(host);
        pthread_mutex_unlock(&host->lock);

        json_api_send_response(client, request, true,
                               response[0] != '\0' ? response
                                                   : "vote recorded",
                               nullptr);
        return true;
    }

    if (!request->is_operator) {
        json_api_send_response(client, request, false,
                               "Only operators may start global polls.",
                               nullptr);
        return false;
    }

    if (request->question[0] == '\0' || request->option_count < 2U) {
        json_api_send_response(client, request, false,
                               "Provide a question and at least two options.",
                               nullptr);
        return false;
    }

    poll_state_t snapshot = {0};
    pthread_mutex_lock(&host->lock);
    uint64_t next_id = host->poll.id + 1U;
    poll_state_reset(&host->poll);
    host->poll.active = true;
    host->poll.allow_multiple = false;
    host->poll.id = next_id == 0U ? 1U : next_id;
    host->poll.option_count = request->option_count;
    snprintf(host->poll.question, sizeof(host->poll.question), "%s",
             request->question);
    for (size_t idx = 0U; idx < request->option_count; ++idx) {
        snprintf(host->poll.options[idx].text,
                 sizeof(host->poll.options[idx].text), "%s",
                 request->options[idx]);
        host->poll.options[idx].votes = 0U;
    }
    host_vote_state_save_locked(host);
    snapshot = host->poll;
    pthread_mutex_unlock(&host->lock);

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice), "* [%s] started a poll: %s",
             request->username, request->question);
    host_history_record_system(host, notice, nullptr);
    chat_room_broadcast(&host->room, notice, nullptr);

    char *poll_json = json_api_build_poll_json(&snapshot);
    if (poll_json == nullptr) {
        json_api_send_response(client, request, true, "poll started", nullptr);
        return true;
    }

    json_builder_t result;
    json_builder_init(&result);
    json_builder_append(&result, "{\"poll\":%s}", poll_json);
    json_api_send_response(client, request, true, "poll started", result.data);
    json_builder_free(&result);
    free(poll_json);
    return true;
}

static bool json_api_handle_vote_request(json_api_client_t *client,
                                         const json_api_request_t *request)
{
    if (client == nullptr || request == nullptr || client->host == nullptr) {
        return false;
    }

    host_t *host = client->host;
    const char *action = request->action[0] != '\0' ? request->action : nullptr;

    if (action == nullptr || strcmp(action, "list") == 0) {
        char *list_json = json_api_build_named_poll_list(host);
        if (list_json == nullptr) {
            json_api_send_response(client, request, false,
                                   "Unable to list polls.", nullptr);
            return false;
        }
        json_builder_t result;
        json_builder_init(&result);
        json_builder_append(&result, "{\"polls\":%s}", list_json);
        json_api_send_response(client, request, true, "poll list", result.data);
        json_builder_free(&result);
        free(list_json);
        return true;
    }

    if (request->label[0] == '\0') {
        json_api_send_response(client, request, false,
                               "Poll label is required.", nullptr);
        return false;
    }

    if (!poll_label_is_valid(request->label)) {
        json_api_send_response(client, request, false,
                               "Poll labels may only contain letters, numbers, "
                               "'-' or '_'.",
                               nullptr);
        return false;
    }

    if (strcmp(action, "status") == 0) {
        named_poll_state_t snapshot = {0};
        bool found = false;
        pthread_mutex_lock(&host->lock);
        named_poll_state_t *poll = host_find_named_poll_locked(host,
                                                               request->label);
        if (poll != nullptr) {
            snapshot = *poll;
            found = true;
        }
        pthread_mutex_unlock(&host->lock);

        if (!found) {
            json_api_send_response(client, request, false,
                                   "No poll found for that label.", nullptr);
            return false;
        }

        char *poll_json = json_api_build_named_poll_json(&snapshot);
        if (poll_json == nullptr) {
            json_api_send_response(client, request, false,
                                   "Unable to format poll.", nullptr);
            return false;
        }
        json_builder_t result;
        json_builder_init(&result);
        json_builder_append(&result, "{\"named_poll\":%s}", poll_json);
        json_api_send_response(client, request, true, "poll summary",
                               result.data);
        json_builder_free(&result);
        free(poll_json);
        return true;
    }

    if (strcmp(action, "close") == 0 || strcmp(action, "end") == 0 ||
        strcmp(action, "stop") == 0) {
        bool closed = false;
        bool allowed = false;
        bool found = false;
        pthread_mutex_lock(&host->lock);
        named_poll_state_t *poll = host_find_named_poll_locked(host,
                                                               request->label);
        if (poll != nullptr) {
            found = true;
            allowed = request->is_operator ||
                      strcasecmp(poll->owner, request->username) == 0;
            if (allowed && poll->poll.active) {
                poll->poll.active = false;
                closed = true;
                host_recount_named_polls_locked(host);
                host_vote_state_save_locked(host);
            }
        }
        pthread_mutex_unlock(&host->lock);

        if (!found) {
            json_api_send_response(client, request, false,
                                   "No poll found for that label.", nullptr);
            return false;
        }
        if (!allowed) {
            json_api_send_response(client, request, false,
                                   "Only the poll owner or an operator may "
                                   "close this poll.",
                                   nullptr);
            return false;
        }
        if (!closed) {
            json_api_send_response(client, request, false,
                                   "That poll is not active.", nullptr);
            return false;
        }

        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice), "* [%s] closed poll [%s].",
                 request->username, request->label);
        host_history_record_system(host, notice, nullptr);
        chat_room_broadcast(&host->room, notice, nullptr);
        json_api_send_response(client, request, true, "poll closed", nullptr);
        return true;
    }

    if (strcmp(action, "vote") == 0 || request->has_choice) {
        if (request->choice == 0U) {
            json_api_send_response(client, request, false,
                                   "Choice must be >= 1.", nullptr);
            return false;
        }

        char response[SSH_CHATTER_MESSAGE_LIMIT];
        response[0] = '\0';

        pthread_mutex_lock(&host->lock);
        named_poll_state_t *poll = host_find_named_poll_locked(host,
                                                               request->label);
        if (poll == nullptr || !poll->poll.active ||
            poll->poll.option_count == 0U) {
            pthread_mutex_unlock(&host->lock);
            json_api_send_response(client, request, false,
                                   "That poll is not active.", nullptr);
            return false;
        }

        size_t option_index = request->choice - 1U;
        if (option_index >= poll->poll.option_count) {
            pthread_mutex_unlock(&host->lock);
            json_api_send_response(client, request, false,
                                   "That poll option does not exist.",
                                   nullptr);
            return false;
        }

        int voter_index = -1;
        for (size_t idx = 0U; idx < poll->voter_count; ++idx) {
            if (strcasecmp(poll->voters[idx].username, request->username) ==
                0) {
                voter_index = (int)idx;
                break;
            }
        }

        if (poll->poll.allow_multiple) {
            uint32_t bit = 1U << option_index;
            if (voter_index >= 0) {
                uint32_t mask = poll->voters[voter_index].choices_mask;
                if ((mask & bit) != 0U) {
                    mask &= ~bit;
                    if (poll->poll.options[option_index].votes > 0U) {
                        poll->poll.options[option_index].votes--;
                    }
                    if (mask == 0U) {
                        size_t remove_index = (size_t)voter_index;
                        if (remove_index + 1U < poll->voter_count) {
                            memmove(&poll->voters[remove_index],
                                    &poll->voters[remove_index + 1U],
                                    (poll->voter_count - remove_index - 1U) *
                                        sizeof(poll->voters[0]));
                        }
                        poll->voter_count--;
                        if (poll->voter_count < SSH_CHATTER_MAX_NAMED_VOTERS) {
                            memset(&poll->voters[poll->voter_count], 0,
                                   sizeof(poll->voters[0]));
                        }
                    } else {
                        poll->voters[voter_index].choices_mask = mask;
                        poll->voters[voter_index].choice = (int)option_index;
                    }
                    snprintf(response, sizeof(response),
                             "Removed your vote for option %zu.",
                             option_index + 1U);
                } else {
                    mask |= bit;
                    poll->poll.options[option_index].votes++;
                    poll->voters[voter_index].choices_mask = mask;
                    poll->voters[voter_index].choice = (int)option_index;
                    snprintf(response, sizeof(response),
                             "Vote recorded for option %zu.",
                             option_index + 1U);
                }
            } else {
                if (poll->voter_count >= SSH_CHATTER_MAX_NAMED_VOTERS) {
                    pthread_mutex_unlock(&host->lock);
                    json_api_send_response(client, request, false,
                                           "That poll has reached its voter "
                                           "limit.",
                                           nullptr);
                    return false;
                }
                poll->poll.options[option_index].votes++;
                size_t insert_at = poll->voter_count++;
                snprintf(poll->voters[insert_at].username,
                         sizeof(poll->voters[insert_at].username), "%s",
                         request->username);
                poll->voters[insert_at].choice = (int)option_index;
                poll->voters[insert_at].choices_mask = bit;
                snprintf(response, sizeof(response),
                         "Vote recorded for option %zu.", option_index + 1U);
            }
        } else {
            if (voter_index >= 0) {
                int previous = poll->voters[voter_index].choice;
                if (previous == (int)option_index) {
                    pthread_mutex_unlock(&host->lock);
                    json_api_send_response(client, request, false,
                                           "You have already voted for that "
                                           "option.",
                                           nullptr);
                    return false;
                }
                if (previous >= 0 &&
                    (size_t)previous < poll->poll.option_count &&
                    poll->poll.options[previous].votes > 0U) {
                    poll->poll.options[previous].votes--;
                }
                poll->poll.options[option_index].votes++;
                poll->voters[voter_index].choice = (int)option_index;
                poll->voters[voter_index].choices_mask = 0U;
                snprintf(response, sizeof(response),
                         "Vote recorded for option %zu.", option_index + 1U);
            } else {
                if (poll->voter_count >= SSH_CHATTER_MAX_NAMED_VOTERS) {
                    pthread_mutex_unlock(&host->lock);
                    json_api_send_response(client, request, false,
                                           "That poll has reached its voter "
                                           "limit.",
                                           nullptr);
                    return false;
                }
                poll->poll.options[option_index].votes++;
                size_t insert_at = poll->voter_count++;
                snprintf(poll->voters[insert_at].username,
                         sizeof(poll->voters[insert_at].username), "%s",
                         request->username);
                poll->voters[insert_at].choice = (int)option_index;
                poll->voters[insert_at].choices_mask = 0U;
                snprintf(response, sizeof(response),
                         "Vote recorded for option %zu.", option_index + 1U);
            }
        }

        host_vote_state_save_locked(host);
        pthread_mutex_unlock(&host->lock);

        json_api_send_response(client, request, true,
                               response[0] != '\0' ? response
                                                   : "vote recorded",
                               nullptr);
        return true;
    }

    if (request->question[0] == '\0' || request->option_count < 2U) {
        json_api_send_response(client, request, false,
                               "Provide a question and at least two options.",
                               nullptr);
        return false;
    }

    named_poll_state_t snapshot = {0};
    bool created = false;
    bool allowed = true;
    pthread_mutex_lock(&host->lock);
    named_poll_state_t *poll = host_ensure_named_poll_locked(host,
                                                             request->label);
    if (poll == nullptr) {
        allowed = false;
    } else if (poll->poll.active &&
               !(request->is_operator ||
                 strcasecmp(poll->owner, request->username) == 0)) {
        allowed = false;
    } else {
        uint64_t next_id = poll->poll.id + 1U;
        char saved_label[SSH_CHATTER_POLL_LABEL_LEN];
        snprintf(saved_label, sizeof(saved_label), "%s", request->label);
        named_poll_reset(poll);
        snprintf(poll->label, sizeof(poll->label), "%s", saved_label);
        snprintf(poll->owner, sizeof(poll->owner), "%s", request->username);
        poll->poll.active = true;
        poll->poll.allow_multiple = request->allow_multiple;
        poll->poll.id = next_id == 0U ? 1U : next_id;
        poll->poll.option_count = request->option_count;
        snprintf(poll->poll.question, sizeof(poll->poll.question), "%s",
                 request->question);
        for (size_t idx = 0U; idx < request->option_count; ++idx) {
            snprintf(poll->poll.options[idx].text,
                     sizeof(poll->poll.options[idx].text), "%s",
                     request->options[idx]);
            poll->poll.options[idx].votes = 0U;
        }
        poll->voter_count = 0U;
        host_recount_named_polls_locked(host);
        host_vote_state_save_locked(host);
        snapshot = *poll;
        created = true;
    }
    pthread_mutex_unlock(&host->lock);

    if (!allowed) {
        json_api_send_response(client, request, false,
                               "Unable to start that poll. Another active poll "
                               "owns the label or the poll limit has been "
                               "reached.",
                               nullptr);
        return false;
    }

    if (created) {
        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice), "* [%s] started poll [%s]: %s",
                 request->username, request->label, request->question);
        host_history_record_system(host, notice, nullptr);
        chat_room_broadcast(&host->room, notice, nullptr);

        char *poll_json = json_api_build_named_poll_json(&snapshot);
        if (poll_json == nullptr) {
            json_api_send_response(client, request, true, "poll started",
                                   nullptr);
            return true;
        }
        json_builder_t result;
        json_builder_init(&result);
        json_builder_append(&result, "{\"named_poll\":%s}", poll_json);
        json_api_send_response(client, request, true, "poll started",
                               result.data);
        json_builder_free(&result);
        free(poll_json);
    }

    return true;
}

static void json_api_handle_request(json_api_client_t *client,
                                    const char *line)
{
    if (client == nullptr || line == nullptr) {
        return;
    }

    json_api_request_t request;
    char error[128];
    if (!json_api_parse_request(line, &request, error, sizeof(error))) {
        json_api_send_response(client, nullptr, false,
                               error[0] != '\0' ? error : "Invalid request.",
                               nullptr);
        return;
    }

    if (request.type[0] == '\0') {
        json_api_send_response(client, &request, false,
                               "Request type is required.", nullptr);
        return;
    }

    if (strcmp(request.type, "chat") == 0) {
        if (request.username[0] == '\0' || request.message[0] == '\0') {
            json_api_send_response(client, &request, false,
                                   "username and message are required.",
                                   nullptr);
            return;
        }

        if (!host_post_client_message(client->host, request.username,
                                      request.message, nullptr, nullptr,
                                      false)) {
            json_api_send_response(client, &request, false,
                                   "Unable to send message.", nullptr);
            return;
        }

        json_api_send_response(client, &request, true, "message sent", nullptr);
        return;
    }

    if (strcmp(request.type, "image") == 0 || strcmp(request.type, "video") == 0 ||
        strcmp(request.type, "audio") == 0 || strcmp(request.type, "files") == 0) {
        if (request.username[0] == '\0') {
            json_api_send_response(client, &request, false,
                                   "username is required.", nullptr);
            return;
        }

        chat_attachment_type_t attachment_type = CHAT_ATTACHMENT_NONE;
        const char *activity = "shared a file";
        if (strcmp(request.type, "image") == 0) {
            attachment_type = CHAT_ATTACHMENT_IMAGE;
            activity = "shared an image";
        } else if (strcmp(request.type, "video") == 0) {
            attachment_type = CHAT_ATTACHMENT_VIDEO;
            activity = "shared a video";
        } else if (strcmp(request.type, "audio") == 0) {
            attachment_type = CHAT_ATTACHMENT_AUDIO;
            activity = "shared an audio clip";
        } else {
            attachment_type = CHAT_ATTACHMENT_FILE;
            activity = "shared a file";
        }

        char error_message[128];
        if (!json_api_post_attachment(client->host, request.username,
                                      attachment_type, request.url,
                                      request.caption, activity,
                                      error_message, sizeof(error_message))) {
            json_api_send_response(client, &request, false,
                                   error_message[0] != '\0' ? error_message
                                                           : "Attachment failed.",
                                   nullptr);
            return;
        }

        json_api_send_response(client, &request, true, "attachment sent",
                               nullptr);
        return;
    }

    if (strcmp(request.type, "asciiart") == 0) {
        if (request.username[0] == '\0' || request.message[0] == '\0') {
            json_api_send_response(client, &request, false,
                                   "username and message are required.",
                                   nullptr);
            return;
        }

        char error_message[128];
        if (!json_api_post_asciiart(client->host, request.username,
                                    request.message, client->peer_ip,
                                    error_message,
                                    sizeof(error_message))) {
            json_api_send_response(client, &request, false,
                                   error_message[0] != '\0' ? error_message
                                                           : "ASCII art failed.",
                                   nullptr);
            return;
        }

        json_api_send_response(client, &request, true, "asciiart sent", nullptr);
        return;
    }

    if (strcmp(request.type, "poll") == 0) {
        if (request.username[0] == '\0' && request.action[0] != '\0' &&
            strcmp(request.action, "status") != 0 &&
            strcmp(request.action, "list") != 0) {
            json_api_send_response(client, &request, false,
                                   "username is required.", nullptr);
            return;
        }
        (void)json_api_handle_poll_request(client, &request);
        return;
    }

    if (strcmp(request.type, "vote") == 0) {
        if (request.username[0] == '\0' && request.action[0] != '\0' &&
            strcmp(request.action, "list") != 0) {
            json_api_send_response(client, &request, false,
                                   "username is required.", nullptr);
            return;
        }
        (void)json_api_handle_vote_request(client, &request);
        return;
    }

    json_api_send_response(client, &request, false,
                           "Unsupported request type.", nullptr);
}

static void *json_api_client_thread(void *arg)
{
    json_api_client_t *client = (json_api_client_t *)arg;
    if (client == nullptr) {
        return nullptr;
    }

    char line_buffer[65536];
    size_t line_length = 0U;
    bool overflowed = false;

    while (!atomic_load(&client->stop)) {
        char buffer[1024];
        ssize_t read_result = recv(client->fd, buffer, sizeof(buffer), 0);
        if (read_result <= 0) {
            break;
        }

        for (ssize_t idx = 0; idx < read_result; ++idx) {
            char ch = buffer[idx];
            if (ch == '\n') {
                if (!overflowed) {
                    line_buffer[line_length] = '\0';
                    if (line_length > 0U) {
                        json_api_handle_request(client, line_buffer);
                    }
                } else {
                    json_api_send_response(client, nullptr, false,
                                           "Input line too long.", nullptr);
                }
                line_length = 0U;
                overflowed = false;
                continue;
            }

            if (ch == '\r') {
                continue;
            }

            if (overflowed) {
                continue;
            }

            if (line_length + 1U >= sizeof(line_buffer)) {
                overflowed = true;
                continue;
            }

            line_buffer[line_length++] = ch;
        }
    }

    if (client->manager != nullptr) {
        client_manager_unregister(client->manager, &client->connection);
    }

    if (client->fd >= 0) {
        shutdown(client->fd, SHUT_RDWR);
        close(client->fd);
        client->fd = -1;
    }

    if (client->write_lock_initialized) {
        pthread_mutex_destroy(&client->write_lock);
        client->write_lock_initialized = false;
    }

    free(client);
    return nullptr;
}

static json_api_client_t *json_api_client_create(host_t *host,
                                                 client_manager_t *manager,
                                                 int fd,
                                                 const char *peer_ip)
{
    if (host == nullptr || manager == nullptr || fd < 0) {
        return nullptr;
    }

    json_api_client_t *client =
        (json_api_client_t *)malloc(sizeof(json_api_client_t));
    if (client == nullptr) {
        return nullptr;
    }

    memset(client, 0, sizeof(*client));
    client->host = host;
    client->manager = manager;
    client->fd = fd;
    snprintf(client->peer_ip, sizeof(client->peer_ip), "%s",
             peer_ip != nullptr ? peer_ip : "");

    if (pthread_mutex_init(&client->write_lock, nullptr) != 0) {
        free(client);
        return nullptr;
    }
    client->write_lock_initialized = true;

    memset(&client->connection, 0, sizeof(client->connection));
    client->connection.kind = CLIENT_KIND_BOT;
    snprintf(client->connection.identifier, sizeof(client->connection.identifier),
             "json-api");
    client->connection.receive_system_messages = true;
    client->connection.active = false;
    client->connection.on_message = json_api_on_message;
    client->connection.on_detach = json_api_on_detach;
    client->connection.user_data = client;
    client->connection.owner = nullptr;

    if (!client_manager_register(manager, &client->connection)) {
        pthread_mutex_destroy(&client->write_lock);
        free(client);
        return nullptr;
    }

    if (pthread_create(&client->thread, nullptr, json_api_client_thread,
                       client) != 0) {
        client_manager_unregister(manager, &client->connection);
        pthread_mutex_destroy(&client->write_lock);
        free(client);
        return nullptr;
    }

    client->thread_initialized = true;
    pthread_detach(client->thread);
    return client;
}

static int json_api_open_socket(host_t *host)
{
    if (host == nullptr || host->json_api.port[0] == '\0') {
        return -1;
    }

    const char *bind_addr = host->json_api.bind_address[0] != '\0'
                                ? host->json_api.bind_address
                                : nullptr;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo *result = nullptr;
    int rc = getaddrinfo(bind_addr, host->json_api.port, &hints, &result);
    if (rc != 0) {
        printf("[json-api] failed to resolve %s:%s (%s)\n",
               bind_addr != nullptr ? bind_addr : "*", host->json_api.port,
               gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *entry = result; entry != nullptr;
         entry = entry->ai_next) {
        fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (fd < 0) {
            continue;
        }

        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        if (bind(fd, entry->ai_addr, entry->ai_addrlen) == 0) {
            if (listen(fd, 8) == 0) {
                break;
            }
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    return fd;
}

static void *json_api_thread(void *arg)
{
    host_t *host = (host_t *)arg;
    if (host == nullptr) {
        return nullptr;
    }

    atomic_store(&host->json_api.running, true);

    while (!atomic_load(&host->json_api.stop)) {
        if (host->json_api.fd < 0) {
            int fd = json_api_open_socket(host);
            if (fd < 0) {
                host->json_api.restart_attempts += 1U;
                json_api_sleep_before_restart(host->json_api.restart_attempts);
                continue;
            }
            host->json_api.fd = fd;
            const char *display_addr = host->json_api.bind_address[0] != '\0'
                                           ? host->json_api.bind_address
                                           : "*";
            printf("[json-api] listening on %s:%s\n", display_addr,
                   host->json_api.port);
        }

        struct sockaddr_storage addr;
        socklen_t addr_len = sizeof(addr);
        int client_fd =
            accept(host->json_api.fd, (struct sockaddr *)&addr, &addr_len);
        if (client_fd < 0) {
            if (atomic_load(&host->json_api.stop)) {
                break;
            }
            humanized_log_error("json-api", "failed to accept client", errno);
            host->json_api.restart_attempts += 1U;
            close(host->json_api.fd);
            host->json_api.fd = -1;
            json_api_sleep_before_restart(host->json_api.restart_attempts);
            continue;
        }

        char peer_address[SSH_CHATTER_IP_LEN] = "";
        if (addr.ss_family == AF_INET) {
            struct sockaddr_in *in = (struct sockaddr_in *)&addr;
            inet_ntop(AF_INET, &in->sin_addr, peer_address,
                      sizeof(peer_address));
        } else if (addr.ss_family == AF_INET6) {
            struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&addr;
            inet_ntop(AF_INET6, &in6->sin6_addr, peer_address,
                      sizeof(peer_address));
        }

        if (host->clients == nullptr) {
            close(client_fd);
            continue;
        }

        json_api_client_t *client = json_api_client_create(
            host, host->clients, client_fd, peer_address);
        if (client == nullptr) {
            close(client_fd);
            continue;
        }
    }

    int listener_fd = host->json_api.fd;
    host->json_api.fd = -1;
    if (listener_fd >= 0) {
        close(listener_fd);
    }

    atomic_store(&host->json_api.running, false);
    return nullptr;
}

bool host_json_api_listener_start(host_t *host, const char *bind_addr,
                                  const char *port)
{
    if (host == nullptr || port == nullptr || port[0] == '\0') {
        return false;
    }

    if (host->json_api.thread_initialized) {
        bool same_port =
            strncmp(host->json_api.port, port, sizeof(host->json_api.port)) ==
            0;
        bool same_bind = false;
        if (bind_addr == nullptr || bind_addr[0] == '\0') {
            same_bind = host->json_api.bind_address[0] == '\0';
        } else {
            same_bind =
                strncmp(host->json_api.bind_address, bind_addr,
                        sizeof(host->json_api.bind_address)) == 0;
        }

        if (same_port && same_bind && atomic_load(&host->json_api.running)) {
            const char *display_addr = host->json_api.bind_address[0] != '\0'
                                           ? host->json_api.bind_address
                                           : "*";
            printf("[json-api] listener already active on %s:%s\n", display_addr,
                   host->json_api.port);
            return true;
        }

        host_json_api_listener_stop(host);
    }

    if (bind_addr != nullptr && bind_addr[0] != '\0') {
        snprintf(host->json_api.bind_address,
                 sizeof(host->json_api.bind_address), "%s", bind_addr);
    } else {
        host->json_api.bind_address[0] = '\0';
    }

    snprintf(host->json_api.port, sizeof(host->json_api.port), "%s", port);
    host->json_api.enabled = true;
    host->json_api.fd = -1;
    host->json_api.restart_attempts = 0U;
    host->json_api.last_error_time.tv_sec = 0;
    host->json_api.last_error_time.tv_nsec = 0L;
    atomic_store(&host->json_api.stop, false);

    if (pthread_create(&host->json_api.thread, nullptr, json_api_thread,
                       host) != 0) {
        humanized_log_error("json-api", "failed to start JSON API listener",
                            errno);
        host->json_api.enabled = false;
        return false;
    }

    host->json_api.thread_initialized = true;
    return true;
}

void host_json_api_listener_stop(host_t *host)
{
    if (host == nullptr || !host->json_api.thread_initialized) {
        if (host != nullptr) {
            host->json_api.enabled = false;
            host->json_api.fd = -1;
            host->json_api.bind_address[0] = '\0';
            host->json_api.port[0] = '\0';
            atomic_store(&host->json_api.running, false);
            atomic_store(&host->json_api.stop, false);
        }
        return;
    }

    const char *display_addr = host->json_api.bind_address[0] != '\0'
                                   ? host->json_api.bind_address
                                   : "*";
    printf("[json-api] stopping listener on %s:%s\n", display_addr,
           host->json_api.port);

    atomic_store(&host->json_api.stop, true);
    if (host->json_api.fd >= 0) {
        shutdown(host->json_api.fd, SHUT_RDWR);
    }

    int join_result = pthread_join(host->json_api.thread, nullptr);
    if (join_result != 0) {
        humanized_log_error("json-api", "failed to join JSON API listener",
                            join_result);
    }

    host->json_api.thread_initialized = false;
    host->json_api.enabled = false;
    atomic_store(&host->json_api.running, false);
    atomic_store(&host->json_api.stop, false);

    if (host->json_api.fd >= 0) {
        close(host->json_api.fd);
        host->json_api.fd = -1;
    }

    host->json_api.bind_address[0] = '\0';
    host->json_api.port[0] = '\0';
}

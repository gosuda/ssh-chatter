#define _POSIX_C_SOURCE 200809L

#include "ssh_chatter/discord_client.h"

#include "ssh_chatter/client.h"
#include "ssh_chatter/host.h"
#include "ssh_chatter/humanized/humanized.h"
#include "ssh_chatter/memory_manager.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#ifndef DISCORD_QUEUE_CAPACITY
#define DISCORD_QUEUE_CAPACITY 64U
#endif

#ifndef DISCORD_MESSAGE_LIMIT
#define DISCORD_MESSAGE_LIMIT 1800U
#endif

typedef struct discord_queue_item {
    char message[SSH_CHATTER_MESSAGE_LIMIT];
    bool is_system;
} discord_queue_item_t;

struct discord_client {
    host_t *host;
    client_manager_t *manager;
    client_connection_t connection;

    pthread_t thread;
    bool thread_initialized;
    _Atomic bool stop;
    _Atomic bool running;

    pthread_mutex_t queue_lock;
    pthread_cond_t queue_cv;
    bool queue_lock_initialized;

    discord_queue_item_t queue[DISCORD_QUEUE_CAPACITY];
    size_t queue_start;
    size_t queue_count;

    char webhook_url[512];
    char username[64];
    char avatar_url[256];
    bool include_system_messages;

    char status[256];
};

static void discord_client_set_status(discord_client_t *client,
                                      const char *status)
{
    if (client == NULL || status == NULL || !client->queue_lock_initialized) {
        return;
    }

    pthread_mutex_lock(&client->queue_lock);
    snprintf(client->status, sizeof(client->status), "%s", status);
    pthread_mutex_unlock(&client->queue_lock);
}

static size_t discord_json_escape(const char *input, char *output,
                                  size_t capacity)
{
    if (output == NULL || capacity == 0U) {
        return 0U;
    }

    size_t written = 0U;
    for (const unsigned char *cursor = (const unsigned char *)input; *cursor != '\0';
         ++cursor) {
        const unsigned char ch = *cursor;
        const char *replacement = NULL;
        switch (ch) {
        case '\\':
            replacement = "\\\\";
            break;
        case '"':
            replacement = "\\\"";
            break;
        case '\n':
            replacement = "\\n";
            break;
        case '\r':
            replacement = "\\r";
            break;
        case '\t':
            replacement = "\\t";
            break;
        default:
            break;
        }

        if (replacement != NULL) {
            size_t repl_len = strlen(replacement);
            if (written + repl_len >= capacity) {
                break;
            }
            memcpy(output + written, replacement, repl_len);
            written += repl_len;
        } else {
            if (written + 1U >= capacity) {
                break;
            }
            output[written++] = (char)ch;
        }
    }

    if (written < capacity) {
        output[written] = '\0';
    } else if (capacity > 0U) {
        output[capacity - 1U] = '\0';
    }
    return written;
}

static bool discord_client_send_webhook(discord_client_t *client,
                                        const char *content)
{
    if (client == NULL || content == NULL || content[0] == '\0') {
        return false;
    }

    char escaped[SSH_CHATTER_MESSAGE_LIMIT * 2];
    discord_json_escape(content, escaped, sizeof(escaped));

    char payload[sizeof(escaped) + 512];
    int written = snprintf(payload, sizeof(payload), "{\"content\":\"%s\"",
                          escaped);
    if (written < 0 || (size_t)written >= sizeof(payload)) {
        return false;
    }

    if (client->username[0] != '\0') {
        written += snprintf(payload + written, sizeof(payload) - (size_t)written,
                            ",\"username\":\"%s\"", client->username);
    }

    if (client->avatar_url[0] != '\0') {
        written += snprintf(payload + written, sizeof(payload) - (size_t)written,
                            ",\"avatar_url\":\"%s\"", client->avatar_url);
    }

    if (written < 0 || (size_t)written >= sizeof(payload) - 2U) {
        return false;
    }

    payload[written++] = '}';
    payload[written] = '\0';

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        return false;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, client->webhook_url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ssh-chatter/discord");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode result = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return result == CURLE_OK;
}

static bool discord_client_dequeue(discord_client_t *client,
                                   discord_queue_item_t *out)
{
    if (client == NULL || out == NULL || !client->queue_lock_initialized) {
        return false;
    }

    if (client->queue_count == 0U) {
        return false;
    }

    *out = client->queue[client->queue_start];
    client->queue_start = (client->queue_start + 1U) % DISCORD_QUEUE_CAPACITY;
    --client->queue_count;
    return true;
}

static void discord_client_enqueue(discord_client_t *client, const char *message,
                                   bool is_system)
{
    if (client == NULL || message == NULL || !client->queue_lock_initialized) {
        return;
    }

    pthread_mutex_lock(&client->queue_lock);

    if (client->queue_count >= DISCORD_QUEUE_CAPACITY) {
        client->queue_start = (client->queue_start + 1U) % DISCORD_QUEUE_CAPACITY;
        --client->queue_count;
    }

    size_t slot = (client->queue_start + client->queue_count) % DISCORD_QUEUE_CAPACITY;
    snprintf(client->queue[slot].message, sizeof(client->queue[slot].message),
             "%s", message);
    client->queue[slot].is_system = is_system;
    ++client->queue_count;

    pthread_cond_signal(&client->queue_cv);
    pthread_mutex_unlock(&client->queue_lock);
}

static void *discord_client_thread(void *arg)
{
    discord_client_t *client = (discord_client_t *)arg;
    if (client == NULL) {
        return NULL;
    }

    atomic_store(&client->running, true);
    discord_client_set_status(client, "Discord relay running");

    while (!atomic_load(&client->stop)) {
        pthread_mutex_lock(&client->queue_lock);
        while (client->queue_count == 0U && !atomic_load(&client->stop)) {
            pthread_cond_wait(&client->queue_cv, &client->queue_lock);
        }

        discord_queue_item_t item;
        bool has_item = discord_client_dequeue(client, &item);
        pthread_mutex_unlock(&client->queue_lock);

        if (atomic_load(&client->stop)) {
            break;
        }

        if (!has_item) {
            continue;
        }

        if (!discord_client_send_webhook(client, item.message)) {
            discord_client_set_status(client, "Discord relay error: send failed");
        }
    }

    atomic_store(&client->running, false);
    discord_client_set_status(client, "Discord relay stopped");
    return NULL;
}

static void discord_client_format_message(char *out, size_t length,
                                          const chat_history_entry_t *entry)
{
    if (out == NULL || length == 0U || entry == NULL) {
        return;
    }

    out[0] = '\0';
    const char *body = entry->message;
    if (body == NULL) {
        body = "";
    }

    const char *username = entry->username[0] != '\0' ? entry->username : "anon";
    if (entry->is_user_message) {
        snprintf(out, length, "[%s] %s", username, body);
    } else {
        snprintf(out, length, "%s", body);
    }

    size_t current_length = strnlen(out, length);
    if (current_length >= DISCORD_MESSAGE_LIMIT) {
        out[DISCORD_MESSAGE_LIMIT] = '\0';
    }
}

static void discord_client_on_message(client_connection_t *connection,
                                      const chat_history_entry_t *entry)
{
    if (connection == NULL || entry == NULL) {
        return;
    }

    discord_client_t *client = (discord_client_t *)connection->user_data;
    if (client == NULL || atomic_load(&client->stop)) {
        return;
    }

    if (!client->include_system_messages && !entry->is_user_message) {
        return;
    }

    char formatted[SSH_CHATTER_MESSAGE_LIMIT];
    discord_client_format_message(formatted, sizeof(formatted), entry);
    if (formatted[0] == '\0') {
        return;
    }

    discord_client_enqueue(client, formatted, !entry->is_user_message);
}

static void discord_client_on_detach(client_connection_t *connection)
{
    (void)connection;
}

discord_client_t *discord_client_create(host_t *host, client_manager_t *manager)
{
    if (host == NULL || manager == NULL) {
        return NULL;
    }

    const char *webhook = getenv("CHATTER_DISCORD_WEBHOOK_URL");
    if (webhook == NULL || webhook[0] == '\0') {
        return NULL;
    }

    discord_client_t *client =
        (discord_client_t *)GC_CALLOC(1U, sizeof(discord_client_t));
    if (client == NULL) {
        return NULL;
    }

    client->host = host;
    client->manager = manager;
    snprintf(client->webhook_url, sizeof(client->webhook_url), "%s", webhook);

    const char *username = getenv("CHATTER_DISCORD_USERNAME");
    snprintf(client->username, sizeof(client->username), "%s",
             (username != NULL && username[0] != '\0') ? username
                                                        : "ssh-chatter");

    const char *avatar = getenv("CHATTER_DISCORD_AVATAR_URL");
    if (avatar != NULL && avatar[0] != '\0') {
        snprintf(client->avatar_url, sizeof(client->avatar_url), "%s", avatar);
    }

    const char *system_flag = getenv("CHATTER_DISCORD_INCLUDE_SYSTEM");
    client->include_system_messages =
        (system_flag != NULL && strcasecmp(system_flag, "on") == 0);

    if (pthread_mutex_init(&client->queue_lock, NULL) != 0 ||
        pthread_cond_init(&client->queue_cv, NULL) != 0) {
        discord_client_destroy(client);
        return NULL;
    }
    client->queue_lock_initialized = true;

    client->connection.kind = CLIENT_KIND_BOT;
    client->connection.receive_system_messages = client->include_system_messages;
    snprintf(client->connection.identifier, sizeof(client->connection.identifier),
             "%s", "discord");
    client->connection.on_message = discord_client_on_message;
    client->connection.on_detach = discord_client_on_detach;
    client->connection.user_data = client;

    if (!client_manager_register(manager, &client->connection)) {
        humanized_log_error("discord", "failed to register discord relay", ENOMEM);
        discord_client_destroy(client);
        return NULL;
    }

    if (pthread_create(&client->thread, NULL, discord_client_thread, client) != 0) {
        humanized_log_error("discord", "failed to start discord worker", errno);
        client_manager_unregister(manager, &client->connection);
        discord_client_destroy(client);
        return NULL;
    }

    client->thread_initialized = true;
    return client;
}

void discord_client_destroy(discord_client_t *client)
{
    if (client == NULL) {
        return;
    }

    if (client->manager != NULL) {
        client_manager_unregister(client->manager, &client->connection);
    }

    if (client->queue_lock_initialized) {
        atomic_store(&client->stop, true);
        pthread_mutex_lock(&client->queue_lock);
        pthread_cond_broadcast(&client->queue_cv);
        pthread_mutex_unlock(&client->queue_lock);
    }

    if (client->thread_initialized) {
        pthread_join(client->thread, NULL);
        client->thread_initialized = false;
    }

    if (client->queue_lock_initialized) {
        pthread_cond_destroy(&client->queue_cv);
        pthread_mutex_destroy(&client->queue_lock);
        client->queue_lock_initialized = false;
    }

    GC_FREE(client);
}

bool discord_client_is_running(const discord_client_t *client)
{
    return client != NULL && atomic_load(&client->running);
}

void discord_client_status(const discord_client_t *client, char *buffer,
                           size_t length)
{
    if (buffer == NULL || length == 0U) {
        return;
    }

    buffer[0] = '\0';
    if (client == NULL) {
        snprintf(buffer, length, "%s", "Discord relay is disabled.");
        return;
    }

    snprintf(buffer, length, "%s", client->status);
}

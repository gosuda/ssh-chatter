#include "headers/webssh_client.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "headers/client.h"
#include "headers/host.h"

struct webssh_client {
    struct host *host;
    client_manager_t *manager;
    client_connection_t connection;
    pthread_mutex_t lock;
    chat_history_entry_t history[SSH_CHATTER_HISTORY_LIMIT];
    size_t history_start;
    size_t history_count;
};

static void webssh_client_on_message(client_connection_t *connection,
                                     const chat_history_entry_t *entry)
{
    if (connection == nullptr || entry == nullptr ||
        connection->user_data == nullptr) {
        return;
    }

    webssh_client_t *client = (webssh_client_t *)connection->user_data;

    pthread_mutex_lock(&client->lock);
    size_t insert_index = 0U;
    if (client->history_count < SSH_CHATTER_HISTORY_LIMIT) {
        insert_index = (client->history_start + client->history_count) %
                       SSH_CHATTER_HISTORY_LIMIT;
        client->history_count++;
    } else {
        insert_index = client->history_start;
        client->history_start =
            (client->history_start + 1U) % SSH_CHATTER_HISTORY_LIMIT;
    }
    client->history[insert_index] = *entry;
    pthread_mutex_unlock(&client->lock);
}

static void webssh_client_on_detach(client_connection_t *connection)
{
    (void)connection;
}

webssh_client_t *webssh_client_create(struct host *host,
                                      client_manager_t *manager)
{
    if (host == nullptr || manager == nullptr) {
        return nullptr;
    }

    webssh_client_t *client =
        (webssh_client_t *)calloc(1U, sizeof(webssh_client_t));
    if (client == nullptr) {
        return nullptr;
    }

    client->host = host;
    client->manager = manager;
    client->history_start = 0U;
    client->history_count = 0U;
    pthread_mutex_init(&client->lock, nullptr);
    memset(&client->connection, 0, sizeof(client->connection));
    client->connection.kind = CLIENT_KIND_WEBSSH;
    snprintf(client->connection.identifier,
             sizeof(client->connection.identifier), "%s", "webssh");
    client->connection.receive_system_messages = true;
    client->connection.active = false;
    client->connection.on_message = webssh_client_on_message;
    client->connection.on_detach = webssh_client_on_detach;
    client->connection.user_data = client;
    client->connection.owner = nullptr;

    if (!client_manager_register(manager, &client->connection)) {
        pthread_mutex_destroy(&client->lock);
        GC_FREE(client);
        return nullptr;
    }

    return client;
}

void webssh_client_destroy(webssh_client_t *client)
{
    if (client == nullptr) {
        return;
    }

    if (client->manager != nullptr) {
        client_manager_unregister(client->manager, &client->connection);
    }
    pthread_mutex_destroy(&client->lock);
    GC_FREE(client);
}

size_t webssh_client_snapshot(webssh_client_t *client,
                              chat_history_entry_t *buffer, size_t capacity)
{
    if (client == nullptr || buffer == nullptr || capacity == 0U) {
        return 0U;
    }

    pthread_mutex_lock(&client->lock);
    size_t count = client->history_count;
    if (count > capacity) {
        count = capacity;
    }
    for (size_t idx = 0U; idx < count; ++idx) {
        size_t history_index =
            (client->history_start + idx) % SSH_CHATTER_HISTORY_LIMIT;
        buffer[idx] = client->history[history_index];
    }
    pthread_mutex_unlock(&client->lock);

    return count;
}

bool webssh_client_send_message(webssh_client_t *client, const char *username,
                                const char *message)
{
    if (client == nullptr || client->host == nullptr) {
        return false;
    }
    return host_post_client_message(client->host, username, message, nullptr,
                                    nullptr, false);
}

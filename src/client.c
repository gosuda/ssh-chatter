#include "ssh_chatter/client.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "ssh_chatter/host.h"

#ifndef CLIENT_MANAGER_MAX_CONNECTIONS
#define CLIENT_MANAGER_MAX_CONNECTIONS 32U
#endif

struct client_manager {
    struct host *host;
    pthread_mutex_t lock;
    bool lock_initialized;
    client_connection_t *connections[CLIENT_MANAGER_MAX_CONNECTIONS];
    size_t connection_count;
};

client_manager_t *client_manager_create(struct host *host)
{
    client_manager_t *manager =
        (client_manager_t *)calloc(1U, sizeof(client_manager_t));
    if (manager == nullptr) {
        return nullptr;
    }

    manager->host = host;
    if (pthread_mutex_init(&manager->lock, nullptr) != 0) {
        GC_FREE(manager);
        return nullptr;
    }
    manager->lock_initialized = true;
    manager->connection_count = 0U;
    memset(manager->connections, 0, sizeof(manager->connections));
    return manager;
}

void client_manager_destroy(client_manager_t *manager)
{
    if (manager == nullptr) {
        return;
    }

    if (!manager->lock_initialized) {
        GC_FREE(manager);
        return;
    }

    pthread_mutex_lock(&manager->lock);
    client_connection_t *connections[CLIENT_MANAGER_MAX_CONNECTIONS];
    size_t connection_count = manager->connection_count;
    for (size_t idx = 0U; idx < connection_count; ++idx) {
        connections[idx] = manager->connections[idx];
        manager->connections[idx] = nullptr;
    }
    manager->connection_count = 0U;
    pthread_mutex_unlock(&manager->lock);

    for (size_t idx = 0U; idx < connection_count; ++idx) {
        client_connection_t *connection = connections[idx];
        if (connection == nullptr) {
            continue;
        }
        connection->active = false;
        connection->owner = nullptr;
        if (connection->on_detach != nullptr) {
            connection->on_detach(connection);
        }
    }

    pthread_mutex_destroy(&manager->lock);
    manager->lock_initialized = false;
    GC_FREE(manager);
}

bool client_manager_register(client_manager_t *manager,
                             client_connection_t *connection)
{
    if (manager == nullptr || !manager->lock_initialized ||
        connection == nullptr || connection->on_message == nullptr) {
        return false;
    }

    bool registered = false;
    pthread_mutex_lock(&manager->lock);
    if (connection->owner != nullptr) {
        if (connection->owner == manager) {
            registered = true;
        }
    } else if (manager->connection_count < CLIENT_MANAGER_MAX_CONNECTIONS) {
        manager->connections[manager->connection_count++] = connection;
        connection->owner = manager;
        connection->active = true;
        registered = true;
    }
    pthread_mutex_unlock(&manager->lock);

    return registered;
}

void client_manager_unregister(client_manager_t *manager,
                               client_connection_t *connection)
{
    if (manager == nullptr || !manager->lock_initialized ||
        connection == nullptr) {
        return;
    }

    bool had_entry = false;
    pthread_mutex_lock(&manager->lock);
    for (size_t idx = 0U; idx < manager->connection_count; ++idx) {
        if (manager->connections[idx] != connection) {
            continue;
        }
        for (size_t shift = idx; shift + 1U < manager->connection_count;
             ++shift) {
            manager->connections[shift] = manager->connections[shift + 1U];
        }
        manager->connections[manager->connection_count - 1U] = nullptr;
        manager->connection_count--;
        had_entry = true;
        break;
    }
    pthread_mutex_unlock(&manager->lock);

    if (had_entry) {
        connection->active = false;
        connection->owner = nullptr;
        if (connection->on_detach != nullptr) {
            connection->on_detach(connection);
        }
    }
}

void client_manager_notify_history(client_manager_t *manager,
                                   const struct chat_history_entry *entry)
{
    if (manager == nullptr || !manager->lock_initialized || entry == nullptr) {
        return;
    }

    client_connection_t *connections[CLIENT_MANAGER_MAX_CONNECTIONS];
    size_t connection_count = 0U;

    pthread_mutex_lock(&manager->lock);
    for (size_t idx = 0U; idx < manager->connection_count; ++idx) {
        client_connection_t *connection = manager->connections[idx];
        if (connection == nullptr || !connection->active) {
            continue;
        }
        if (!entry->is_user_message && !connection->receive_system_messages) {
            continue;
        }
        connections[connection_count++] = connection;
    }
    pthread_mutex_unlock(&manager->lock);

    for (size_t idx = 0U; idx < connection_count; ++idx) {
        client_connection_t *connection = connections[idx];
        if (connection->on_message != nullptr) {
            connection->on_message(connection, entry);
        }
    }
}

struct host *client_manager_host(client_manager_t *manager)
{
    if (manager == nullptr) {
        return nullptr;
    }
    return manager->host;
}

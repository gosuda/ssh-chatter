/**
 * @file client.c
 * @desc File-level documentation for client.c, describing its role
 *       in the SSH-Chatter server and providing a consistent header
 *       comment format across C sources.
 * @return None.
 */

#include "ssh_chatter/client.h"

#include <stdlib.h>
#include <string.h>

#include "ssh_chatter/host.h"

#ifndef CLIENT_MANAGER_MAX_CONNECTIONS
#define CLIENT_MANAGER_MAX_CONNECTIONS 32U
#endif

struct client_manager {
    struct host *host;
    _Atomic bool shutting_down;
    _Atomic size_t connection_count;
    _Atomic(client_connection_t *) connections[CLIENT_MANAGER_MAX_CONNECTIONS];
};

client_manager_t *client_manager_create(struct host *host)
{
    client_manager_t *manager =
        (client_manager_t *)sshc_gc_calloc(1U, sizeof(client_manager_t));
    if (manager == nullptr) {
        return nullptr;
    }

    manager->host = host;
    atomic_store_explicit(&manager->shutting_down, false, memory_order_relaxed);
    atomic_store_explicit(&manager->connection_count, 0U, memory_order_relaxed);
    for (size_t idx = 0U; idx < CLIENT_MANAGER_MAX_CONNECTIONS; ++idx) {
        atomic_store_explicit(&manager->connections[idx], nullptr,
                              memory_order_relaxed);
    }
    return manager;
}

void client_manager_destroy(client_manager_t *manager)
{
    if (manager == nullptr) {
        return;
    }

    atomic_store_explicit(&manager->shutting_down, true, memory_order_release);

    for (size_t idx = 0U; idx < CLIENT_MANAGER_MAX_CONNECTIONS; ++idx) {
        client_connection_t *connection = atomic_exchange_explicit(
            &manager->connections[idx], nullptr, memory_order_acq_rel);
        if (connection == nullptr) {
            continue;
        }
        atomic_fetch_sub_explicit(&manager->connection_count, 1U,
                                  memory_order_acq_rel);
        atomic_store_explicit(&connection->active, false, memory_order_release);
        atomic_store_explicit(&connection->owner, nullptr, memory_order_release);
        if (connection->on_detach != nullptr) {
            connection->on_detach(connection);
        }
    }

    sshc_gc_free(manager);
}

bool client_manager_register(client_manager_t *manager,
                             client_connection_t *connection)
{
    if (manager == nullptr || connection == nullptr ||
        connection->on_message == nullptr) {
        return false;
    }

    if (atomic_load_explicit(&manager->shutting_down, memory_order_acquire)) {
        return false;
    }

    client_manager_t *owner =
        atomic_load_explicit(&connection->owner, memory_order_acquire);
    if (owner != nullptr) {
        return owner == manager;
    }

    for (size_t idx = 0U; idx < CLIENT_MANAGER_MAX_CONNECTIONS; ++idx) {
        client_connection_t *expected = nullptr;
        if (!atomic_compare_exchange_strong_explicit(
                &manager->connections[idx], &expected, connection,
                memory_order_acq_rel, memory_order_acquire)) {
            continue;
        }
        atomic_store_explicit(&connection->owner, manager, memory_order_release);
        atomic_store_explicit(&connection->active, true, memory_order_release);
        atomic_fetch_add_explicit(&manager->connection_count, 1U,
                                  memory_order_acq_rel);
        return true;
    }

    return false;
}

void client_manager_unregister(client_manager_t *manager,
                               client_connection_t *connection)
{
    if (manager == nullptr || connection == nullptr) {
        return;
    }

    bool detached = false;
    for (size_t idx = 0U; idx < CLIENT_MANAGER_MAX_CONNECTIONS; ++idx) {
        client_connection_t *expected = connection;
        if (atomic_compare_exchange_strong_explicit(
                &manager->connections[idx], &expected, nullptr,
                memory_order_acq_rel, memory_order_acquire)) {
            atomic_fetch_sub_explicit(&manager->connection_count, 1U,
                                      memory_order_acq_rel);
            detached = true;
            break;
        }
    }

    if (detached) {
        atomic_store_explicit(&connection->active, false, memory_order_release);
        atomic_store_explicit(&connection->owner, nullptr, memory_order_release);
        if (connection->on_detach != nullptr) {
            connection->on_detach(connection);
        }
    }
}

void client_manager_notify_history(client_manager_t *manager,
                                   const struct chat_history_entry *entry)
{
    if (manager == nullptr || entry == nullptr) {
        return;
    }

    client_connection_t *connections[CLIENT_MANAGER_MAX_CONNECTIONS];
    size_t connection_count = 0U;

    for (size_t idx = 0U; idx < CLIENT_MANAGER_MAX_CONNECTIONS; ++idx) {
        client_connection_t *connection =
            atomic_load_explicit(&manager->connections[idx], memory_order_acquire);
        if (connection == nullptr) {
            continue;
        }
        if (!atomic_load_explicit(&connection->active, memory_order_acquire)) {
            continue;
        }
        if (!entry->is_user_message && !connection->receive_system_messages) {
            continue;
        }
        connections[connection_count++] = connection;
    }

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

#include "headers/ddial_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "headers/client.h"
#include "headers/memory_manager.h"
#include "headers/host.h"

#ifndef DDIAL_BUFFER_SIZE
#define DDIAL_BUFFER_SIZE 2048U
#endif

struct ddial_client {
    struct host *host;
    client_manager_t *manager;
    pthread_t thread;
    bool thread_initialized;
    _Atomic bool thread_stop;
    _Atomic bool thread_running;
    _Atomic bool connected;
    pthread_mutex_t lock;
    char endpoint_host[256];
    char endpoint_port[16];
    char status[256];
    int socket_fd;
};

static void ddial_client_set_status(ddial_client_t *client, const char *status)
{
    if (client == nullptr || status == nullptr) {
        return;
    }

    pthread_mutex_lock(&client->lock);
    snprintf(client->status, sizeof(client->status), "%s", status);
    pthread_mutex_unlock(&client->lock);
}

static void ddial_client_close_socket(ddial_client_t *client)
{
    if (client == nullptr) {
        return;
    }

    if (client->socket_fd >= 0) {
        close(client->socket_fd);
        client->socket_fd = -1;
    }
}

static void ddial_client_broadcast(ddial_client_t *client, const char *line)
{
    if (client == nullptr || client->host == nullptr || line == nullptr ||
        line[0] == '\0') {
        return;
    }

    host_post_ephemeral_message(client->host, "ddial", line, "cyan", nullptr,
                                true);
}

static void *ddial_client_worker(void *arg)
{
    ddial_client_t *client = (ddial_client_t *)arg;
    atomic_store(&client->thread_running, true);
    atomic_store(&client->connected, false);

    char host_copy[sizeof(client->endpoint_host)];
    char port_copy[sizeof(client->endpoint_port)];
    pthread_mutex_lock(&client->lock);
    snprintf(host_copy, sizeof(host_copy), "%s", client->endpoint_host);
    snprintf(port_copy, sizeof(port_copy), "%s", client->endpoint_port);
    pthread_mutex_unlock(&client->lock);

    struct addrinfo hints;
    struct addrinfo *result = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host_copy, port_copy, &hints, &result);
    if (rc != 0 || result == nullptr) {
        char message[256];
        snprintf(message, sizeof(message), "Resolve failed for %s:%s (%s)",
                 host_copy, port_copy, gai_strerror(rc));
        ddial_client_set_status(client, message);
        goto exit_worker;
    }

    int sock = -1;
    struct addrinfo *cursor = result;
    for (; cursor != nullptr; cursor = cursor->ai_next) {
        sock = socket(cursor->ai_family, cursor->ai_socktype, cursor->ai_protocol);
        if (sock < 0) {
            continue;
        }
        if (connect(sock, cursor->ai_addr, cursor->ai_addrlen) == 0) {
            break;
        }
        close(sock);
        sock = -1;
    }

    freeaddrinfo(result);

    if (sock < 0) {
        ddial_client_set_status(client, "Unable to connect to D-Dial endpoint");
        goto exit_worker;
    }

    client->socket_fd = sock;
    atomic_store(&client->connected, true);

    char status[256];
    snprintf(status, sizeof(status), "Connected to D-Dial at %s:%s", host_copy,
             port_copy);
    ddial_client_set_status(client, status);
    ddial_client_broadcast(client, status);

    char buffer[DDIAL_BUFFER_SIZE];
    size_t buffered = 0U;
    buffer[0] = '\0';

    while (!atomic_load(&client->thread_stop)) {
        ssize_t received = recv(sock, buffer + buffered,
                                sizeof(buffer) - buffered - 1U, 0);
        if (received > 0) {
            size_t available = (size_t)received + buffered;
            size_t start = 0U;
            for (size_t idx = 0U; idx < available; ++idx) {
                if (buffer[idx] == '\r' || buffer[idx] == '\n') {
                    buffer[idx] = '\0';
                    if (idx > start) {
                        ddial_client_broadcast(client, buffer + start);
                    }
                    while (idx + 1U < available &&
                           (buffer[idx + 1U] == '\r' ||
                            buffer[idx + 1U] == '\n')) {
                        ++idx;
                    }
                    start = idx + 1U;
                }
            }

            if (start < available) {
                buffered = available - start;
                memmove(buffer, buffer + start, buffered);
            } else {
                buffered = 0U;
            }
        } else if (received == 0) {
            ddial_client_set_status(client, "D-Dial connection closed by remote");
            break;
        } else {
            if (errno == EINTR) {
                continue;
            }
            ddial_client_set_status(client, "D-Dial connection error");
            break;
        }
    }

exit_worker:
    ddial_client_close_socket(client);
    atomic_store(&client->connected, false);
    atomic_store(&client->thread_running, false);
    return nullptr;
}

ddial_client_t *ddial_client_create(struct host *host, client_manager_t *manager)
{
    if (host == nullptr || manager == nullptr) {
        return nullptr;
    }

    ddial_client_t *client =
        (ddial_client_t *)calloc(1U, sizeof(ddial_client_t));
    if (client == nullptr) {
        return nullptr;
    }

    client->host = host;
    client->manager = manager;
    client->thread_initialized = false;
    atomic_store(&client->thread_stop, false);
    atomic_store(&client->thread_running, false);
    atomic_store(&client->connected, false);
    client->socket_fd = -1;
    pthread_mutex_init(&client->lock, nullptr);
    snprintf(client->status, sizeof(client->status), "D-Dial relay idle");
    client->endpoint_host[0] = '\0';
    client->endpoint_port[0] = '\0';

    return client;
}

void ddial_client_destroy(ddial_client_t *client)
{
    if (client == nullptr) {
        return;
    }

    ddial_client_disconnect(client);
    pthread_mutex_destroy(&client->lock);
    GC_FREE(client);
}

bool ddial_client_connect(ddial_client_t *client, const char *host,
                          const char *port)
{
    if (client == nullptr || host == nullptr || port == nullptr) {
        return false;
    }

    pthread_mutex_lock(&client->lock);
    snprintf(client->endpoint_host, sizeof(client->endpoint_host), "%s", host);
    snprintf(client->endpoint_port, sizeof(client->endpoint_port), "%s", port);
    pthread_mutex_unlock(&client->lock);

    ddial_client_disconnect(client);

    atomic_store(&client->thread_stop, false);
    if (pthread_create(&client->thread, nullptr, ddial_client_worker,
                       client) != 0) {
        ddial_client_set_status(client, "Failed to start D-Dial worker");
        return false;
    }

    client->thread_initialized = true;
    return true;
}

void ddial_client_disconnect(ddial_client_t *client)
{
    if (client == nullptr) {
        return;
    }

    atomic_store(&client->thread_stop, true);
    if (client->thread_initialized) {
        pthread_join(client->thread, nullptr);
        client->thread_initialized = false;
    }
    ddial_client_close_socket(client);
    atomic_store(&client->connected, false);
}

const char *ddial_client_get_status(ddial_client_t *client)
{
    if (client == nullptr) {
        return "D-Dial relay unavailable";
    }

    pthread_mutex_lock(&client->lock);
    const char *status = client->status;
    pthread_mutex_unlock(&client->lock);
    return status;
}

bool ddial_client_is_connected(ddial_client_t *client)
{
    if (client == nullptr) {
        return false;
    }
    return atomic_load(&client->connected);
}


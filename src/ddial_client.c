#include "ssh_chatter/ddial_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "ssh_chatter/client.h"
#include "ssh_chatter/codepage.h"
#include "ssh_chatter/memory_manager.h"
#include "ssh_chatter/host.h"

#ifndef DDIAL_BUFFER_SIZE
#define DDIAL_BUFFER_SIZE 2048U
#endif

struct ddial_client {
    struct host *host;
    client_manager_t *manager;
    client_connection_t connection;
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
    char logs[DDIAL_LOG_CAPACITY][DDIAL_LOG_ENTRY_LENGTH];
    size_t log_start;
    size_t log_count;
};

static void ddial_client_log(ddial_client_t *client, const char *format, ...);
static bool ddial_client_send_line(ddial_client_t *client, const char *line);
static void ddial_client_process_line(ddial_client_t *client, const char *line,
                                      size_t length);
static void ddial_client_broadcast(ddial_client_t *client, const char *line);
static void ddial_client_on_message(client_connection_t *connection,
                                    const chat_history_entry_t *entry);
static void ddial_client_on_detach(client_connection_t *connection);

static bool ddial_contains_multibyte(const char *line)
{
    if (line == nullptr) {
        return false;
    }

    for (const unsigned char *cursor = (const unsigned char *)line; *cursor != '\0';
         ++cursor) {
        if ((*cursor & 0x80U) != 0U) {
            if ((*cursor & 0xC0U) == 0xC0U) {
                return true;
            }
        }
    }

    return false;
}

static void ddial_client_log(ddial_client_t *client, const char *format, ...)
{
    if (client == nullptr || format == nullptr) {
        return;
    }

    va_list args;
    va_start(args, format);

    pthread_mutex_lock(&client->lock);
    size_t index = (client->log_start + client->log_count) % DDIAL_LOG_CAPACITY;
    vsnprintf(client->logs[index], sizeof(client->logs[index]), format, args);
    if (client->log_count < DDIAL_LOG_CAPACITY) {
        ++client->log_count;
    } else {
        client->log_start = (client->log_start + 1U) % DDIAL_LOG_CAPACITY;
    }
    pthread_mutex_unlock(&client->lock);

    va_end(args);
}

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

static bool ddial_client_send_line(ddial_client_t *client, const char *line)
{
    if (client == nullptr || line == nullptr || line[0] == '\0') {
        return false;
    }

    if (!atomic_load(&client->connected) || client->socket_fd < 0) {
        return false;
    }

    if (ddial_contains_multibyte(line)) {
        ddial_client_log(client, "Skipped outbound (non-CP437): %s", line);
        return false;
    }

    char encoded[DDIAL_BUFFER_SIZE];
    size_t encoded_len =
        session_utf8_to_codepage(SESSION_CODEPAGE_CP437, line, strlen(line),
                                 encoded, sizeof(encoded) - 2U);
    if (encoded_len == 0U) {
        ddial_client_log(client, "Encoding failed for outbound: %s", line);
        return false;
    }

    encoded[encoded_len++] = '\r';
    encoded[encoded_len++] = '\n';

    size_t sent_total = 0U;
    while (sent_total < encoded_len) {
        ssize_t sent =
            send(client->socket_fd, encoded + sent_total, encoded_len - sent_total,
                 0);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            ddial_client_log(client, "Send error: %s", strerror(errno));
            return false;
        }
        sent_total += (size_t)sent;
    }

    ddial_client_log(client, "Sent: %s", line);
    host_append_sync_log(client->host, "ddial", line);
    return true;
}

static void ddial_client_process_line(ddial_client_t *client, const char *line,
                                      size_t length)
{
    if (client == nullptr || line == nullptr || length == 0U) {
        return;
    }

    char utf8[SSH_CHATTER_MESSAGE_LIMIT];
    size_t produced = session_codepage_to_utf8(
        SESSION_CODEPAGE_CP437, (const unsigned char *)line, length, utf8,
        sizeof(utf8) - 1U);

    if (produced == 0U) {
        produced = (length < sizeof(utf8) - 1U) ? length : sizeof(utf8) - 1U;
        memcpy(utf8, line, produced);
    }

    utf8[produced] = '\0';
    ddial_client_log(client, "Recv: %s", utf8);

    const char *bracket = strchr(utf8, '[');
    const char *colon = (bracket != nullptr) ? strchr(bracket, ':') : nullptr;
    const char *close = (colon != nullptr) ? strchr(colon, ')') : nullptr;

    if (bracket != nullptr && colon != nullptr && close != nullptr && colon < close) {
        char nickname[SSH_CHATTER_USERNAME_LEN];
        size_t nick_len = (size_t)(close - colon - 1);
        if (nick_len >= sizeof(nickname)) {
            nick_len = sizeof(nickname) - 1U;
        }
        memcpy(nickname, colon + 1, nick_len);
        nickname[nick_len] = '\0';

        const char *message_start = close + 1;
        while (*message_start == ' ' || *message_start == ':') {
            ++message_start;
        }

        if (*message_start != '\0') {
            char formatted[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(formatted, sizeof(formatted), "%s: %s", nickname,
                     message_start);
            ddial_client_broadcast(client, formatted);
            return;
        }
    }

    ddial_client_broadcast(client, utf8);
}

static void ddial_client_broadcast(ddial_client_t *client, const char *line)
{
    if (client == nullptr || client->host == nullptr || line == nullptr ||
        line[0] == '\0') {
        return;
    }

    if (host_post_client_message(client->host, "ddial", line, nullptr, nullptr,
                                 false)) {
        host_append_sync_log(client->host, "ddial", line);
    }
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
        ddial_client_log(client, "%s", message);
        goto exit_worker;
    }

    int sock = -1;
    struct addrinfo *cursor = result;
    for (; cursor != nullptr; cursor = cursor->ai_next) {
        sock = socket(cursor->ai_family, cursor->ai_socktype, cursor->ai_protocol);
        if (sock < 0) {
            continue;
        }

        int flags = fcntl(sock, F_GETFL, 0);
        if (flags >= 0) {
            (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        }

        int connect_result = connect(sock, cursor->ai_addr, cursor->ai_addrlen);
        if (connect_result == 0) {
            if (flags >= 0) {
                (void)fcntl(sock, F_SETFL, flags);
            }
            break;
        }

        if (errno == EINPROGRESS) {
            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(sock, &write_fds);
            struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
            int ready = select(sock + 1, nullptr, &write_fds, nullptr, &timeout);
            if (ready > 0 && FD_ISSET(sock, &write_fds)) {
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 &&
                    so_error == 0) {
                    if (flags >= 0) {
                        (void)fcntl(sock, F_SETFL, flags);
                    }
                    break;
                }
            }
        }

        close(sock);
        sock = -1;
    }

    freeaddrinfo(result);

    if (sock < 0) {
        ddial_client_set_status(client, "Unable to connect to D-Dial endpoint");
        ddial_client_log(client, "Connection attempt failed for %s:%s", host_copy,
                         port_copy);
        goto exit_worker;
    }

    client->socket_fd = sock;
    atomic_store(&client->connected, true);

    char status[256];
    snprintf(status, sizeof(status), "Connected to D-Dial at %s:%s", host_copy,
             port_copy);
    ddial_client_set_status(client, status);
    ddial_client_log(client, "%s", status);
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
                        ddial_client_process_line(client, buffer + start,
                                                  idx - start);
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
            ddial_client_log(client, "Connection closed by remote");
            break;
        } else {
            if (errno == EINTR) {
                continue;
            }
            ddial_client_set_status(client, "D-Dial connection error");
            ddial_client_log(client, "Receive error: %s", strerror(errno));
            break;
        }
    }

exit_worker:
    ddial_client_close_socket(client);
    atomic_store(&client->connected, false);
    atomic_store(&client->thread_running, false);
    return nullptr;
}

static void ddial_client_on_message(client_connection_t *connection,
                                    const chat_history_entry_t *entry)
{
    if (connection == nullptr || entry == nullptr ||
        connection->user_data == nullptr) {
        return;
    }

    ddial_client_t *client = (ddial_client_t *)connection->user_data;
    if (!entry->is_user_message) {
        return;
    }

    if (strncmp(entry->username, "ddial", sizeof(entry->username)) == 0) {
        return;
    }

    if (entry->message[0] == '\0') {
        return;
    }

    char outbound[SSH_CHATTER_MESSAGE_LIMIT];
    int written = snprintf(outbound, sizeof(outbound), "%s: %s",
                           entry->username, entry->message);
    if (written <= 0) {
        return;
    }

    ddial_client_send_line(client, outbound);
}

static void ddial_client_on_detach(client_connection_t *connection)
{
    (void)connection;
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
    memset(&client->connection, 0, sizeof(client->connection));
    client->thread_initialized = false;
    atomic_store(&client->thread_stop, false);
    atomic_store(&client->thread_running, false);
    atomic_store(&client->connected, false);
    client->socket_fd = -1;
    client->log_start = 0U;
    client->log_count = 0U;
    pthread_mutex_init(&client->lock, nullptr);
    snprintf(client->status, sizeof(client->status), "D-Dial relay idle");
    client->endpoint_host[0] = '\0';
    client->endpoint_port[0] = '\0';

    client->connection.kind = CLIENT_KIND_BOT;
    snprintf(client->connection.identifier, sizeof(client->connection.identifier),
             "%s", "ddial");
    client->connection.receive_system_messages = false;
    client->connection.on_message = ddial_client_on_message;
    client->connection.on_detach = ddial_client_on_detach;
    client->connection.user_data = client;

    if (!client_manager_register(manager, &client->connection)) {
        pthread_mutex_destroy(&client->lock);
        GC_FREE(client);
        return nullptr;
    }

    return client;
}

void ddial_client_destroy(ddial_client_t *client)
{
    if (client == nullptr) {
        return;
    }

    ddial_client_disconnect(client);
    if (client->manager != nullptr) {
        client_manager_unregister(client->manager, &client->connection);
    }
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

    ddial_client_log(client, "Connect requested to %s:%s", host, port);

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

bool ddial_client_snapshot_logs(ddial_client_t *client,
                                char entries[][DDIAL_LOG_ENTRY_LENGTH],
                                size_t capacity, size_t *count)
{
    if (client == nullptr || entries == nullptr || count == nullptr ||
        capacity == 0U) {
        return false;
    }

    pthread_mutex_lock(&client->lock);
    size_t available = client->log_count;
    if (available > capacity) {
        available = capacity;
    }

    for (size_t i = 0; i < available; ++i) {
        size_t idx = (client->log_start + i) % DDIAL_LOG_CAPACITY;
        snprintf(entries[i], DDIAL_LOG_ENTRY_LENGTH, "%s", client->logs[idx]);
    }
    pthread_mutex_unlock(&client->lock);

    *count = available;
    return true;
}


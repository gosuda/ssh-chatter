#define _POSIX_C_SOURCE 200809L

#include "ssh_chatter/fidonet_client.h"
#include "ssh_chatter/host.h"
#include "ssh_chatter/humanized/humanized.h"
#include "ssh_chatter/client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Binkp protocol constants */
#define BINKP_BUFFER_SIZE 32768
#define BINKP_DEFAULT_PORT 24554
#define BINKP_RECONNECT_DELAY_SECONDS 30
#define BINKP_KEEPALIVE_INTERVAL_SECONDS 60

/* Binkp message types */
#define BINKP_BLK_DATA 0x00      /* Data block */
#define BINKP_BLK_CMD  0x80      /* Command block */

/* Binkp commands */
#define BINKP_CMD_NUL  0  /* No operation/comment */
#define BINKP_CMD_ADR  1  /* Address list */
#define BINKP_CMD_PWD  2  /* Session password */
#define BINKP_CMD_FILE 3  /* File information */
#define BINKP_CMD_OK   4  /* Password accepted */
#define BINKP_CMD_EOB  5  /* End of batch */
#define BINKP_CMD_GOT  6  /* File received */
#define BINKP_CMD_ERR  7  /* Error */
#define BINKP_CMD_BSY  8  /* Busy */
#define BINKP_CMD_GET  9  /* Get file */
#define BINKP_CMD_SKIP 10 /* Skip file */
#define BINKP_CMD_CHAT 11 /* Chat message - custom extension */

struct fidonet_client {
    host_t *host;
    client_manager_t *manager;
    client_connection_t connection;
    pthread_mutex_t lock;
    bool lock_initialized;
    pthread_t thread;
    bool thread_initialized;
    _Atomic bool stop;
    _Atomic bool running;
    _Atomic bool disabled;
    _Atomic bool connected;
    char server_host[256];
    int server_port;
    char node_address[64];      /* Our FidoNet address (e.g., "2:5030/1997") */
    char remote_address[64];    /* Remote FidoNet address */
    char session_password[128]; /* Binkp session password */
    char status_message[256];
    int socket_fd;
    time_t last_keepalive;
    bool session_established;
    char logs[FIDONET_LOG_CAPACITY][FIDONET_LOG_ENTRY_LENGTH];
    size_t log_start;
    size_t log_count;
};

static bool fidonet_contains_multibyte(const char *line)
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

static void fidonet_client_log(fidonet_client_t *client, const char *format, ...)
{
    if (client == nullptr || format == nullptr) {
        return;
    }

    va_list args;
    va_start(args, format);

    pthread_mutex_lock(&client->lock);
    size_t index = (client->log_start + client->log_count) % FIDONET_LOG_CAPACITY;
    vsnprintf(client->logs[index], sizeof(client->logs[index]), format, args);
    if (client->log_count < FIDONET_LOG_CAPACITY) {
        ++client->log_count;
    } else {
        client->log_start = (client->log_start + 1U) % FIDONET_LOG_CAPACITY;
    }
    pthread_mutex_unlock(&client->lock);

    va_end(args);
}

static const char *fidonet_getenv(const char *name)
{
    const char *value = getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return nullptr;
    }
    return value;
}

static void fidonet_set_status(fidonet_client_t *client, const char *status)
{
    if (client == nullptr || status == nullptr) {
        return;
    }
    pthread_mutex_lock(&client->lock);
    snprintf(client->status_message, sizeof(client->status_message), "%s",
             status);
    pthread_mutex_unlock(&client->lock);

    fidonet_client_log(client, "%s", status);
}

__attribute__((unused)) static void
fidonet_client_disable(fidonet_client_t *client, const char *message,
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
        humanized_log_error("fidonet", message, log_code);
    } else {
        humanized_log_error("fidonet", "FidoNet relay disabled after failure",
                            log_code);
    }

    fidonet_set_status(client, "Disabled");
    atomic_store(&client->stop, true);
}

/* Send a Binkp frame (block) */
static bool fidonet_send_frame(fidonet_client_t *client, uint8_t type,
                               const uint8_t *data, uint16_t length)
{
    if (client == nullptr || client->socket_fd < 0) {
        return false;
    }

    /* Binkp frame format: 2 bytes header + data */
    uint8_t header[2];
    header[0] = (uint8_t)((length >> 8) & 0x7F) | type;
    header[1] = (uint8_t)(length & 0xFF);

    if (send(client->socket_fd, header, 2, 0) != 2) {
        return false;
    }

    if (length > 0 && data != nullptr) {
        if (send(client->socket_fd, data, length, 0) != (ssize_t)length) {
            return false;
        }
    }

    return true;
}

/* Send a Binkp command */
static bool fidonet_send_command(fidonet_client_t *client, uint8_t cmd,
                                 const char *args)
{
    uint8_t buffer[BINKP_BUFFER_SIZE];
    uint16_t length = 1;

    buffer[0] = cmd;

    if (args != nullptr) {
        size_t args_len = strlen(args);
        if (args_len > sizeof(buffer) - 2) {
            args_len = sizeof(buffer) - 2;
        }
        memcpy(buffer + 1, args, args_len);
        length = (uint16_t)(1 + args_len);
    }

    return fidonet_send_frame(client, BINKP_BLK_CMD, buffer, length);
}

/* Send chat message via Binkp custom CHAT command */
static bool fidonet_send_chat_message(fidonet_client_t *client,
                                      const char *username, const char *message)
{
    if (client == nullptr || username == nullptr || message == nullptr) {
        return false;
    }

    if (!atomic_load(&client->connected) || !client->session_established) {
        return false;
    }

    if (fidonet_contains_multibyte(message) || fidonet_contains_multibyte(username)) {
        fidonet_client_log(client, "Skipped outbound (non-ASCII): %s: %s",
                           username, message);
        return false;
    }

    /* Format: "username: message" */
    char buffer[BINKP_BUFFER_SIZE];
    int written = snprintf(buffer, sizeof(buffer), "%s: %s", username, message);
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        return false;
    }

    bool ok = fidonet_send_command(client, BINKP_CMD_CHAT, buffer);
    if (ok) {
        fidonet_client_log(client, "Sent: %s", buffer);
        host_append_sync_log(client->host, "fidonet", buffer);
    }
    return ok;
}

/* Receive a Binkp frame */
static ssize_t fidonet_receive_frame(fidonet_client_t *client, uint8_t *type,
                                     uint8_t *data, size_t max_data)
{
    if (client == nullptr || type == nullptr || client->socket_fd < 0) {
        return -1;
    }

    /* Read 2-byte header */
    uint8_t header[2];
    ssize_t bytes = recv(client->socket_fd, header, 2, 0);
    if (bytes != 2) {
        return -1;
    }

    *type = header[0] & 0x80;
    uint16_t length = (uint16_t)(((header[0] & 0x7F) << 8) | header[1]);

    if (length == 0) {
        return 0;
    }

    if (data == nullptr || length > max_data) {
        /* Skip the data if buffer is too small */
        uint8_t temp[256];
        size_t remaining = length;
        while (remaining > 0) {
            size_t chunk = remaining > sizeof(temp) ? sizeof(temp) : remaining;
            if (recv(client->socket_fd, temp, chunk, 0) != (ssize_t)chunk) {
                return -1;
            }
            remaining -= chunk;
        }
        return 0;
    }

    bytes = recv(client->socket_fd, data, length, 0);
    if (bytes != length) {
        return -1;
    }

    return length;
}

/* Handle incoming Binkp message */
static void fidonet_handle_message(fidonet_client_t *client, uint8_t type,
                                   const uint8_t *data, size_t length)
{
    if (client == nullptr || client->host == nullptr) {
        return;
    }

    if (type == BINKP_BLK_CMD && length > 0) {
        uint8_t cmd = data[0];
        const char *args = (length > 1) ? (const char *)(data + 1) : "";

        switch (cmd) {
        case BINKP_CMD_NUL:
            /* Comment/no-op - ignore */
            break;

        case BINKP_CMD_ADR:
            /* Remote node address */
            snprintf(client->remote_address, sizeof(client->remote_address),
                     "%s", args);
            break;

        case BINKP_CMD_PWD:
            /* Remote sent password - we accept any for now */
            fidonet_send_command(client, BINKP_CMD_OK, "Session accepted");
            client->session_established = true;
            fidonet_set_status(client, "Session established");
            break;

        case BINKP_CMD_OK:
            /* Password accepted */
            client->session_established = true;
            fidonet_set_status(client, "Session established");
            break;

        case BINKP_CMD_EOB:
            /* End of batch - acknowledge */
            fidonet_send_command(client, BINKP_CMD_EOB, nullptr);
            break;

        case BINKP_CMD_ERR:
            /* Error message */
            humanized_log_error("fidonet", args, EIO);
            break;

        case BINKP_CMD_BSY:
            /* Remote is busy */
            fidonet_set_status(client, "Remote busy");
            break;

        case BINKP_CMD_CHAT: {
            /* Chat message - custom extension */
            /* Format: "username: message" */
            fidonet_client_log(client, "Recv: %s", args);
            const char *colon = strchr(args, ':');
            if (colon != nullptr) {
                size_t username_len = (size_t)(colon - args);
                char username[SSH_CHATTER_USERNAME_LEN];
                char message[SSH_CHATTER_MESSAGE_LIMIT];

                if (username_len < sizeof(username)) {
                    memcpy(username, args, username_len);
                    username[username_len] = '\0';

                    /* Skip colon and space */
                    const char *msg_start = colon + 1;
                    while (*msg_start == ' ') {
                        msg_start++;
                    }

                    snprintf(message, sizeof(message), "%s", msg_start);

                    if (host_post_client_message(client->host, username,
                                                 message, nullptr, nullptr,
                                                 false)) {
                        host_append_sync_log(client->host, "fidonet", message);
                    }
                }
            }
        } break;

        default:
            /* Unknown command - ignore */
            break;
        }
    }
}

static bool fidonet_connect_socket(fidonet_client_t *client)
{
    if (client == nullptr) {
        return false;
    }

    if (client->socket_fd >= 0) {
        close(client->socket_fd);
        client->socket_fd = -1;
    }

    struct addrinfo hints, *result, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", client->server_port);

    int ret = getaddrinfo(client->server_host, port_str, &hints, &result);
    if (ret != 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Failed to resolve FidoNet server %s: %s",
                 client->server_host, gai_strerror(ret));
        fidonet_set_status(client, msg);
        return false;
    }

    for (rp = result; rp != nullptr; rp = rp->ai_next) {
        client->socket_fd =
            socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (client->socket_fd == -1) {
            continue;
        }

        int flags = fcntl(client->socket_fd, F_GETFL, 0);
        if (flags >= 0) {
            (void)fcntl(client->socket_fd, F_SETFL, flags | O_NONBLOCK);
        }

        int connect_result = connect(client->socket_fd, rp->ai_addr, rp->ai_addrlen);
        if (connect_result == 0) {
            if (flags >= 0) {
                (void)fcntl(client->socket_fd, F_SETFL, flags);
            }
            break;
        }

        if (errno == EINPROGRESS) {
            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(client->socket_fd, &write_fds);
            struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
            int ready = select(client->socket_fd + 1, nullptr, &write_fds, nullptr,
                               &timeout);
            if (ready > 0 && FD_ISSET(client->socket_fd, &write_fds)) {
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                if (getsockopt(client->socket_fd, SOL_SOCKET, SO_ERROR, &so_error,
                               &len) == 0 &&
                    so_error == 0) {
                    if (flags >= 0) {
                        (void)fcntl(client->socket_fd, F_SETFL, flags);
                    }
                    break;
                }
            }
        }

        close(client->socket_fd);
        client->socket_fd = -1;
    }

    freeaddrinfo(result);

    if (client->socket_fd == -1) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Failed to connect to FidoNet server %s:%d",
                 client->server_host, client->server_port);
        fidonet_set_status(client, msg);
        return false;
    }

    int flag = 1;
    (void)setsockopt(client->socket_fd, IPPROTO_TCP, TCP_NODELAY, &flag,
                     sizeof(flag));
    (void)setsockopt(client->socket_fd, SOL_SOCKET, SO_KEEPALIVE, &flag,
                     sizeof(flag));

    /* Send initial handshake */
    client->session_established = false;

    fidonet_send_command(client, BINKP_CMD_NUL, "SYS ssh-chatter bridge");
    fidonet_send_command(client, BINKP_CMD_NUL, "LOC retro terminal gateway");
    fidonet_send_command(client, BINKP_CMD_NUL, "VER ssh-chatter binkp");

    /* Send our address */
    fidonet_send_command(client, BINKP_CMD_ADR, client->node_address);

    /* Send session password if configured */
    if (client->session_password[0] != '\0') {
        fidonet_send_command(client, BINKP_CMD_PWD, client->session_password);
    } else {
        /* Send empty password */
        fidonet_send_command(client, BINKP_CMD_PWD, "-");
    }

    atomic_store(&client->connected, true);
    client->last_keepalive = time(nullptr);
    fidonet_set_status(client, "Connected");

    return true;
}

static void fidonet_disconnect_socket(fidonet_client_t *client)
{
    if (client == nullptr) {
        return;
    }

    atomic_store(&client->connected, false);
    client->session_established = false;

    if (client->socket_fd >= 0) {
        /* Send EOB to signal clean disconnect */
        fidonet_send_command(client, BINKP_CMD_EOB, nullptr);
        close(client->socket_fd);
        client->socket_fd = -1;
    }

    fidonet_set_status(client, "Disconnected");
}

static void fidonet_client_on_message(client_connection_t *connection,
                                      const chat_history_entry_t *entry)
{
    if (connection == nullptr || entry == nullptr ||
        connection->user_data == nullptr) {
        return;
    }

    fidonet_client_t *client = (fidonet_client_t *)connection->user_data;
    if (atomic_load(&client->disabled)) {
        return;
    }

    if (entry->username[0] == '\0' || entry->message[0] == '\0') {
        return;
    }

    /* Don't echo back FidoNet messages */
    if (strncmp(entry->message, "[FidoNet]", 9) == 0) {
        return;
    }

    if (!fidonet_send_chat_message(client, entry->username, entry->message)) {
        /* Silently fail - don't flood logs */
    }
}

static void fidonet_client_on_detach(client_connection_t *connection)
{
    (void)connection;
}

static void *fidonet_client_thread(void *arg)
{
    fidonet_client_t *client = (fidonet_client_t *)arg;
    if (client == nullptr) {
        return nullptr;
    }

    atomic_store(&client->running, true);
    fidonet_set_status(client, "Starting");

    while (!atomic_load(&client->stop)) {
        if (!atomic_load(&client->connected)) {
            if (!fidonet_connect_socket(client)) {
                sleep(BINKP_RECONNECT_DELAY_SECONDS);
                continue;
            }
        }

        /* Send periodic keepalive */
        time_t now = time(nullptr);
        if (now - client->last_keepalive > BINKP_KEEPALIVE_INTERVAL_SECONDS) {
            fidonet_send_command(client, BINKP_CMD_NUL, "keepalive");
            client->last_keepalive = now;
        }

        /* Receive data */
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client->socket_fd, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int ret = select(client->socket_fd + 1, &read_fds, nullptr, nullptr,
                         &timeout);
        if (ret < 0) {
            fidonet_disconnect_socket(client);
            continue;
        }

        if (ret == 0) {
            continue;
        }

        /* Receive and process frame */
        uint8_t type;
        uint8_t data[BINKP_BUFFER_SIZE];
        ssize_t length =
            fidonet_receive_frame(client, &type, data, sizeof(data));
        if (length < 0) {
            fidonet_disconnect_socket(client);
            continue;
        }

        if (length > 0) {
            fidonet_handle_message(client, type, data, (size_t)length);
        }
    }

    fidonet_disconnect_socket(client);
    atomic_store(&client->running, false);
    fidonet_set_status(client, "Stopped");

    return nullptr;
}

fidonet_client_t *fidonet_client_create(host_t *host, client_manager_t *manager)
{
    if (host == nullptr || manager == nullptr) {
        return nullptr;
    }

    const char *server = fidonet_getenv("CHATTER_FIDONET_SERVER");
    const char *port_str = fidonet_getenv("CHATTER_FIDONET_PORT");
    const char *node_addr = fidonet_getenv("CHATTER_FIDONET_ADDRESS");
    const char *password = fidonet_getenv("CHATTER_FIDONET_PASSWORD");

    /* FidoNet is optional */
    if (server == nullptr || node_addr == nullptr) {
        return nullptr;
    }

    fidonet_client_t *client =
        (fidonet_client_t *)calloc(1, sizeof(fidonet_client_t));
    if (client == nullptr) {
        return nullptr;
    }

    client->host = host;
    client->manager = manager;
    client->socket_fd = -1;
    atomic_init(&client->stop, false);
    atomic_init(&client->running, false);
    atomic_init(&client->disabled, false);
    atomic_init(&client->connected, false);
    client->session_established = false;
    client->log_start = 0U;
    client->log_count = 0U;

    snprintf(client->server_host, sizeof(client->server_host), "%s", server);
    client->server_port =
        (port_str != nullptr) ? atoi(port_str) : BINKP_DEFAULT_PORT;
    snprintf(client->node_address, sizeof(client->node_address), "%s",
             node_addr);
    snprintf(client->session_password, sizeof(client->session_password), "%s",
             password != nullptr ? password : "");

    if (pthread_mutex_init(&client->lock, nullptr) != 0) {
        GC_FREE(client);
        return nullptr;
    }
    client->lock_initialized = true;

    /* Register as a bot client to receive messages */
    memset(&client->connection, 0, sizeof(client->connection));
    client->connection.kind = CLIENT_KIND_BOT;
    snprintf(client->connection.identifier,
             sizeof(client->connection.identifier), "%s", "fidonet");
    client->connection.receive_system_messages = false;
    client->connection.on_message = fidonet_client_on_message;
    client->connection.on_detach = fidonet_client_on_detach;
    client->connection.user_data = client;

    if (!client_manager_register(manager, &client->connection)) {
        pthread_mutex_destroy(&client->lock);
        GC_FREE(client);
        return nullptr;
    }

    fidonet_set_status(client, "Initializing");

    if (pthread_create(&client->thread, nullptr, fidonet_client_thread,
                       client) != 0) {
        client_manager_unregister(manager, &client->connection);
        pthread_mutex_destroy(&client->lock);
        GC_FREE(client);
        return nullptr;
    }
    client->thread_initialized = true;

    printf("[fidonet] FidoNet/Binkp relay enabled for %s at %s:%d\n",
           client->node_address, client->server_host, client->server_port);

    return client;
}

void fidonet_client_destroy(fidonet_client_t *client)
{
    if (client == nullptr) {
        return;
    }

    atomic_store(&client->stop, true);

    if (client->thread_initialized) {
        pthread_join(client->thread, nullptr);
    }

    if (client->manager != nullptr) {
        client_manager_unregister(client->manager, &client->connection);
    }

    fidonet_disconnect_socket(client);

    if (client->lock_initialized) {
        pthread_mutex_destroy(&client->lock);
    }

    GC_FREE(client);
}

bool fidonet_client_is_connected(fidonet_client_t *client)
{
    if (client == nullptr) {
        return false;
    }
    return atomic_load(&client->connected) && client->session_established;
}

const char *fidonet_client_get_status(fidonet_client_t *client)
{
    if (client == nullptr) {
        return "Not initialized";
    }
    return client->status_message;
}

bool fidonet_client_reconnect(fidonet_client_t *client)
{
    if (client == nullptr) {
        return false;
    }

    fidonet_disconnect_socket(client);
    atomic_store(&client->disabled, false);

    return fidonet_connect_socket(client);
}

void fidonet_client_disconnect(fidonet_client_t *client)
{
    if (client == nullptr) {
        return;
    }

    atomic_store(&client->disabled, true);
    fidonet_disconnect_socket(client);
}

bool fidonet_client_send_message(fidonet_client_t *client, const char *username,
                                 const char *message)
{
    if (client == nullptr) {
        return false;
    }

    return fidonet_send_chat_message(client, username, message);
}

bool fidonet_client_snapshot_logs(fidonet_client_t *client,
                                  char entries[][FIDONET_LOG_ENTRY_LENGTH],
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
        size_t idx = (client->log_start + i) % FIDONET_LOG_CAPACITY;
        snprintf(entries[i], FIDONET_LOG_ENTRY_LENGTH, "%s", client->logs[idx]);
    }
    pthread_mutex_unlock(&client->lock);

    *count = available;
    return true;
}

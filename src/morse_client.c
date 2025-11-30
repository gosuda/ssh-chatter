#include "host_parts/host_internal.h"

#include <sys/select.h>

#define MORSE_HOST "telnet.reversebeacon.net"
#define MORSE_PORT "7000"
#define MORSE_RECONNECT_SECONDS 30U
#define MORSE_SELECT_TIMEOUT_SECONDS 5U

struct morse_client {
    host_t *host;
    int socket_fd;
    pthread_t thread;
    bool thread_initialized;
    _Atomic bool thread_stop;
    _Atomic bool thread_running;
    _Atomic bool connected;
};

static void morse_client_close_socket(morse_client_t *client)
{
    if (client == nullptr) {
        return;
    }
    if (client->socket_fd >= 0) {
        close(client->socket_fd);
        client->socket_fd = -1;
    }
    atomic_store(&client->connected, false);
}

static void morse_client_collect_targets(host_t *host, session_ctx_t ***out,
                                         size_t *out_count)
{
    if (host == nullptr || out == nullptr || out_count == nullptr) {
        return;
    }

    *out = nullptr;
    *out_count = 0U;

    pthread_mutex_lock(&host->room.lock);
    size_t expected = host->room.member_count;
    if (expected > 0U) {
        session_ctx_t **targets =
            (session_ctx_t **)GC_MALLOC(expected * sizeof(*targets));
        if (targets != nullptr) {
            memset(targets, 0, expected * sizeof(*targets));
            for (size_t idx = 0; idx < host->room.member_count; ++idx) {
                session_ctx_t *member = host->room.members[idx];
                if (member == nullptr || member->should_exit ||
                    !member->morse_feed_enabled) {
                    continue;
                }
                targets[(*out_count)++] = member;
            }
            *out = targets;
        }
    }
    pthread_mutex_unlock(&host->room.lock);
}

static void morse_client_broadcast(morse_client_t *client, const char *line)
{
    if (client == nullptr || client->host == nullptr || line == nullptr) {
        return;
    }

    char formatted[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(formatted, sizeof(formatted), "[MORSE] %s", line);

    session_ctx_t **targets = nullptr;
    size_t target_count = 0U;
    morse_client_collect_targets(client->host, &targets, &target_count);

    if (targets == nullptr && target_count > 0U) {
        return;
    }

    for (size_t idx = 0; idx < target_count; ++idx) {
        session_send_raw_text(targets[idx], formatted);
    }

    GC_FREE(targets);
}

static bool morse_client_connect(morse_client_t *client)
{
    if (client == nullptr) {
        return false;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *result = nullptr;
    int rc = getaddrinfo(MORSE_HOST, MORSE_PORT, &hints, &result);
    if (rc != 0 || result == nullptr) {
        printf("[morse] DNS failure: %s\n", gai_strerror(rc));
        return false;
    }

    int fd = -1;
    for (struct addrinfo *ai = result; ai != nullptr; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);

    if (fd < 0) {
        printf("[morse] unable to connect to %s:%s\n", MORSE_HOST, MORSE_PORT);
        return false;
    }

    client->socket_fd = fd;
    atomic_store(&client->connected, true);
    printf("[morse] connected to %s:%s\n", MORSE_HOST, MORSE_PORT);
    return true;
}

static void morse_client_preview_handshake_output(morse_client_t *client)
{
    if (client == nullptr || client->socket_fd < 0) {
        return;
    }

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(client->socket_fd, &readfds);
    struct timeval tv = {
        .tv_sec = 2,
        .tv_usec = 0,
    };

    int ready = select(client->socket_fd + 1, &readfds, nullptr, nullptr, &tv);
    if (ready <= 0) {
        return;
    }

    char buffer[SSH_CHATTER_MESSAGE_LIMIT];
    ssize_t received = recv(client->socket_fd, buffer, sizeof(buffer) - 1U, 0);
    if (received <= 0) {
        return;
    }

    buffer[received] = '\0';
    char *line_start = buffer;
    char *newline = nullptr;
    while ((newline = strchr(line_start, '\n')) != nullptr) {
        *newline = '\0';
        if (newline > line_start && newline[-1] == '\r') {
            newline[-1] = '\0';
        }
        if (line_start[0] != '\0') {
            morse_client_broadcast(client, line_start);
        }
        line_start = newline + 1;
    }

    if (line_start[0] != '\0') {
        morse_client_broadcast(client, line_start);
    }
}

static void morse_client_send_handshake(morse_client_t *client)
{
    if (client == nullptr) {
        return;
    }

    if (!morse_client_send(client, "HLGUEST")) {
        printf("[morse] failed to send handshake.\n");
        return;
    }

    if (!morse_client_send(client, "CQ CQ CQ DE HLGUEST")) {
        printf("[morse] failed to send initial probe line.\n");
        return;
    }

    morse_client_preview_handshake_output(client);
}

static void morse_client_sleep_seconds(unsigned int seconds)
{
    struct timespec pause = {
        .tv_sec = (time_t)seconds,
        .tv_nsec = 0L,
    };
    while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {
    }
}

static void morse_client_handle_stream(morse_client_t *client)
{
    if (client == nullptr || client->socket_fd < 0) {
        return;
    }

    char buffer[SSH_CHATTER_MESSAGE_LIMIT];
    size_t buffered = 0U;

    while (!atomic_load(&client->thread_stop)) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(client->socket_fd, &readfds);
        struct timeval tv = {
            .tv_sec = MORSE_SELECT_TIMEOUT_SECONDS,
            .tv_usec = 0,
        };

        int ready = select(client->socket_fd + 1, &readfds, nullptr, nullptr, &tv);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            printf("[morse] select error: %s\n", strerror(errno));
            break;
        }
        if (ready == 0) {
            continue;
        }

        ssize_t received = recv(client->socket_fd, buffer + buffered,
                                sizeof(buffer) - buffered - 1U, 0);
        if (received <= 0) {
            printf("[morse] connection closed\n");
            break;
        }

        buffered += (size_t)received;
        buffer[buffered] = '\0';

        char *line_start = buffer;
        char *newline = nullptr;
        while ((newline = strchr(line_start, '\n')) != nullptr) {
            *newline = '\0';
            if (newline > line_start && newline[-1] == '\r') {
                newline[-1] = '\0';
            }
            if (line_start[0] != '\0') {
                morse_client_broadcast(client, line_start);
            }
            line_start = newline + 1;
        }

        if (line_start != buffer) {
            buffered = strnlen(line_start, sizeof(buffer) - 1U);
            memmove(buffer, line_start, buffered);
        }
        if (buffered >= sizeof(buffer) - 1U) {
            buffered = 0U;
        }
    }
}

static void *morse_client_thread(void *arg)
{
    morse_client_t *client = (morse_client_t *)arg;
    if (client == nullptr) {
        return nullptr;
    }

    atomic_store(&client->thread_running, true);
    while (!atomic_load(&client->thread_stop)) {
        if (!atomic_load(&client->connected)) {
            morse_client_close_socket(client);
            if (!morse_client_connect(client)) {
                morse_client_sleep_seconds(MORSE_RECONNECT_SECONDS);
                continue;
            }

            morse_client_send_handshake(client);
        }

        morse_client_handle_stream(client);
        morse_client_close_socket(client);
        morse_client_sleep_seconds(MORSE_RECONNECT_SECONDS);
    }

    atomic_store(&client->thread_running, false);
    return nullptr;
}

morse_client_t *morse_client_create(host_t *host)
{
    if (host == nullptr) {
        return nullptr;
    }

    morse_client_t *client = (morse_client_t *)calloc(1U, sizeof(*client));
    if (client == nullptr) {
        return nullptr;
    }

    client->host = host;
    client->socket_fd = -1;
    atomic_store(&client->thread_stop, false);
    atomic_store(&client->thread_running, false);
    atomic_store(&client->connected, false);

    int error = pthread_create(&client->thread, nullptr, morse_client_thread, client);
    if (error != 0) {
        printf("[morse] failed to start worker: %s\n", strerror(error));
        free(client);
        return nullptr;
    }

    client->thread_initialized = true;
    return client;
}

void morse_client_destroy(morse_client_t *client)
{
    if (client == nullptr) {
        return;
    }

    atomic_store(&client->thread_stop, true);
    if (client->thread_initialized) {
        pthread_join(client->thread, nullptr);
        client->thread_initialized = false;
    }
    morse_client_close_socket(client);
    free(client);
}

bool morse_client_send(morse_client_t *client, const char *line)
{
    if (client == nullptr || line == nullptr || line[0] == '\0') {
        return false;
    }
    if (!atomic_load(&client->connected) || client->socket_fd < 0) {
        return false;
    }

    char payload[SSH_CHATTER_MESSAGE_LIMIT];
    int written = snprintf(payload, sizeof(payload), "%s\r\n", line);
    if (written <= 0 || (size_t)written >= sizeof(payload)) {
        return false;
    }

    size_t total = (size_t)written;
    size_t sent_total = 0U;
    while (sent_total < total) {
        ssize_t sent = send(client->socket_fd, payload + sent_total,
                            total - sent_total, 0);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        sent_total += (size_t)sent;
    }

    return true;
}

bool morse_client_connected(const morse_client_t *client)
{
    return client != nullptr && atomic_load(&client->connected);
}

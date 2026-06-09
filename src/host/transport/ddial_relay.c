/**
 * @file ddial_relay.c
 * @desc DDial native relay: bidirectional text bridge between Chatter
 *       chat room and an upstream DDial node (e.g. magviz.ca).
 *       Modelled after jace-ddial bridge.py behaviour.
 */

#include <errno.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>

static bool ddial_relay_connect_socket(ddial_relay_t *relay)
{
    if (relay == nullptr || relay->host[0] == '\0' || relay->port <= 0) {
        return false;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    struct hostent *server = gethostbyname(relay->host);
    if (server == nullptr) {
        close(fd);
        return false;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)relay->port);
    memcpy(&addr.sin_addr.s_addr, server->h_addr, (size_t)server->h_length);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return false;
    }

    relay->upstream_fd = fd;
    relay->connected = true;
    relay->auth_sent = false;
    relay->recv_buf_len = 0U;
    return true;
}

static void ddial_relay_disconnect(ddial_relay_t *relay)
{
    if (relay == nullptr) {
        return;
    }
    ttak_mutex_lock(&relay->lock);
    if (relay->upstream_fd >= 0) {
        close(relay->upstream_fd);
        relay->upstream_fd = -1;
    }
    relay->connected = false;
    relay->auth_sent = false;
    relay->recv_buf_len = 0U;
    ttak_mutex_unlock(&relay->lock);
}

static void ddial_relay_broadcast_line(host_t *host, const char *line)
{
    if (host == nullptr || line == nullptr || line[0] == '\0') {
        return;
    }

    char prefixed[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(prefixed, sizeof(prefixed),
             "\033[1;33m[DDial]\033[0m %s", line);
    chat_room_broadcast(&host->room, prefixed, nullptr);
}

static void ddial_relay_process_buffer(host_t *host, ddial_relay_t *relay)
{
    if (host == nullptr || relay == nullptr) {
        return;
    }

    char *buf = relay->recv_buffer;
    size_t len = relay->recv_buf_len;

    for (;;) {
        char *crlf = nullptr;
        for (size_t i = 0; i + 1 < len; ++i) {
            if (buf[i] == '\r' && buf[i + 1] == '\n') {
                crlf = &buf[i];
                break;
            }
        }
        if (crlf == nullptr) {
            break;
        }

        size_t line_len = (size_t)(crlf - buf);
        if (line_len >= SSH_CHATTER_MESSAGE_LIMIT) {
            line_len = SSH_CHATTER_MESSAGE_LIMIT - 1;
        }
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        memcpy(line, buf, line_len);
        line[line_len] = '\0';

        // Strip trailing CR if any
        if (line_len > 0 && line[line_len - 1] == '\r') {
            line[line_len - 1] = '\0';
        }

        if (line[0] != '\0') {
            ddial_relay_broadcast_line(host, line);
        }

        size_t consumed = line_len + 2; // +2 for \r\n
        memmove(buf, buf + consumed, len - consumed);
        len -= consumed;
    }

    relay->recv_buf_len = len;
}

static void *ddial_relay_thread(void *arg)
{
    host_t *host = (host_t *)arg;
    if (host == nullptr) {
        return nullptr;
    }

    ddial_relay_t *relay = &host->ddial_relay;

    while (!relay->stop) {
        if (!relay->connected) {
            if (!ddial_relay_connect_socket(relay)) {
                sleep(5);
                continue;
            }

            // Send login key if configured (bridge.py style)
            if (relay->login_key[0] != '\0') {
                char key_line[128];
                snprintf(key_line, sizeof(key_line), "%s\r\n",
                         relay->login_key);
                (void)send(relay->upstream_fd, key_line, strlen(key_line),
                           MSG_NOSIGNAL);
                relay->auth_sent = true;
            }
        }

        char temp[1024];
        ssize_t n = recv(relay->upstream_fd, temp, sizeof(temp), 0);
        if (n <= 0) {
            ddial_relay_disconnect(relay);
            sleep(3);
            continue;
        }

        ttak_mutex_lock(&relay->lock);
        size_t space = sizeof(relay->recv_buffer) - relay->recv_buf_len;
        size_t to_copy = (size_t)n;
        if (to_copy > space) {
            to_copy = space;
        }
        if (to_copy > 0) {
            memcpy(relay->recv_buffer + relay->recv_buf_len, temp, to_copy);
            relay->recv_buf_len += to_copy;
        }
        ddial_relay_process_buffer(host, relay);
        ttak_mutex_unlock(&relay->lock);
    }

    ddial_relay_disconnect(relay);
    return nullptr;
}

static void host_ddial_relay_init(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    ddial_relay_t *relay = &host->ddial_relay;
    memset(relay, 0, sizeof(*relay));
    relay->upstream_fd = -1;
    relay->enabled = false;
    relay->connected = false;
    relay->stop = true;
    relay->port = 0;

    if (ttak_mutex_init(&relay->lock) == 0) {
        relay->lock_initialized = true;
    }

    const char *host_env = getenv("CHATTER_DDIAL_HOST");
    const char *port_env = getenv("CHATTER_DDIAL_PORT");
    const char *key_env = getenv("CHATTER_DDIAL_KEY");

    if (host_env != nullptr && host_env[0] != '\0' &&
        port_env != nullptr && port_env[0] != '\0') {
        snprintf(relay->host, sizeof(relay->host), "%s", host_env);
        relay->port = (int)strtol(port_env, nullptr, 10);
        if (key_env != nullptr) {
            snprintf(relay->login_key, sizeof(relay->login_key), "%s",
                     key_env);
        }
        relay->enabled = true;
    }
}

static void host_ddial_relay_start(host_t *host)
{
    if (host == nullptr) {
        return;
    }
    ddial_relay_t *relay = &host->ddial_relay;
    if (!relay->enabled || relay->thread_initialized) {
        return;
    }

    relay->stop = false;
    if (pthread_create(&relay->thread, nullptr, ddial_relay_thread, host) ==
        0) {
        relay->thread_initialized = true;
        relay->running = true;
    }
}

static void host_ddial_relay_stop(host_t *host)
{
    if (host == nullptr) {
        return;
    }
    ddial_relay_t *relay = &host->ddial_relay;
    relay->stop = true;
    if (relay->thread_initialized) {
        pthread_join(relay->thread, nullptr);
        relay->thread_initialized = false;
        relay->running = false;
    }
    ddial_relay_disconnect(relay);
}

static void host_ddial_relay_send(host_t *host, const char *handle,
                                  const char *message)
{
    if (host == nullptr || handle == nullptr || message == nullptr) {
        return;
    }
    ddial_relay_t *relay = &host->ddial_relay;
    if (!relay->enabled || !relay->connected || relay->upstream_fd < 0) {
        return;
    }

    char line[SSH_CHATTER_MESSAGE_LIMIT * 2];
    snprintf(line, sizeof(line), "[%s] %s\r\n", handle, message);

    ttak_mutex_lock(&relay->lock);
    if (relay->connected && relay->upstream_fd >= 0) {
        (void)send(relay->upstream_fd, line, strlen(line), MSG_NOSIGNAL);
    }
    ttak_mutex_unlock(&relay->lock);
}

static bool host_ddial_relay_configure(host_t *host, const char *host_str,
                                       int port, const char *key)
{
    if (host == nullptr || host_str == nullptr || host_str[0] == '\0' ||
        port <= 0) {
        return false;
    }

    host_ddial_relay_stop(host);

    ddial_relay_t *relay = &host->ddial_relay;
    snprintf(relay->host, sizeof(relay->host), "%s", host_str);
    relay->port = port;
    if (key != nullptr) {
        snprintf(relay->login_key, sizeof(relay->login_key), "%s", key);
    } else {
        relay->login_key[0] = '\0';
    }
    relay->enabled = true;

    host_ddial_relay_start(host);
    return true;
}

static void host_ddial_relay_disable(host_t *host)
{
    if (host == nullptr) {
        return;
    }
    host_ddial_relay_stop(host);
    host->ddial_relay.enabled = false;
    host->ddial_relay.host[0] = '\0';
    host->ddial_relay.port = 0;
}

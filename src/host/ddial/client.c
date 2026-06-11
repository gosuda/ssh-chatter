/**
 * @file ddial_client.c
 * @desc Upstream Diversi Dial client relay for SSH-Chatter.
 *
 * Connects to a configured upstream DDial node (e.g. magviz.ca),
 * authenticates with key/handle, and bridges messages between the
 * Chatter chat room and the upstream node using the DDial protocol.
 */

#include "ssh_chatter/ddial_protocol.h"
#include "ssh_chatter/host.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DDIAL_CLIENT_KEEPALIVE_INTERVAL_SEC 45
#define DDIAL_CLIENT_POLL_TIMEOUT_MS 5000
#define DDIAL_CLIENT_RECONNECT_BACKOFF_SEC 5
#define DDIAL_CLIENT_RECV_CHUNK_SIZE 4096
#define DDIAL_CLIENT_AUTH_TIMEOUT_SEC 10
typedef struct ddial_client {
    bool enabled;
    int upstream_fd;
    pthread_t thread;
    bool thread_initialized;
    _Atomic bool running;
    _Atomic bool stop;
    ttak_mutex_t lock;
    bool lock_initialized;
    bool connected;
    bool auth_sent;
    ddial_auth_state_t auth_state;
    struct timespec auth_deadline;
    bool locally_registered;
    char host[256];
    int port;
    char login_key[64];
    char handle[SSH_CHATTER_USERNAME_LEN];
    char recv_buffer[SSH_CHATTER_MESSAGE_LIMIT * 4];
    size_t recv_buf_len;
    struct timespec last_send_time;
    struct {
        char handle[SSH_CHATTER_USERNAME_LEN];
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        struct timespec sent_at;
    } recent_sent[DDIAL_RELAY_SENT_HISTORY];
    size_t recent_sent_index;
} ddial_client_t;

static ssize_t ddial_client_send_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0U;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

static void ddial_client_send_nop(ddial_client_t *client)
{
    if (client == nullptr || client->upstream_fd < 0) {
        return;
    }
    const unsigned char nop[2] = {0xFF, 0xF1};
    (void)ddial_client_send_all(client->upstream_fd, (const char *)nop, 2);
}

static void ddial_client_update_send_time(ddial_client_t *client)
{
    if (client == nullptr) {
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &client->last_send_time);
}

static void ddial_client_maybe_keepalive(ddial_client_t *client)
{
    if (client == nullptr || client->upstream_fd < 0 ||
        client->last_send_time.tv_sec == 0) {
        return;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return;
    }
    time_t delta = now.tv_sec - client->last_send_time.tv_sec;
    if (delta >= (time_t)DDIAL_CLIENT_KEEPALIVE_INTERVAL_SEC) {
        ddial_client_send_nop(client);
        ddial_client_update_send_time(client);
    }
}

static bool ddial_client_prompt_like(const char *buf, size_t len)
{
    if (buf == nullptr || len == 0U) {
        return false;
    }
    static const char *needles[] = {"-->", "assword", "Remote", "login:"};
    for (size_t i = 0U; i < sizeof(needles) / sizeof(needles[0]); ++i) {
        if (memmem(buf, len, needles[i], strlen(needles[i])) != nullptr) {
            return true;
        }
    }
    return false;
}

static ddial_auth_state_t ddial_client_classify_auth(const char *line)
{
    if (line == nullptr || line[0] == '\0') {
        return DDIAL_AUTH_NONE;
    }
    /* A live DDial-formatted public chat line means we are already in. */
    if (line[0] == '#' || line[0] == '[') {
        return DDIAL_AUTH_APPROVED;
    }
    /* Case-insensitive substring scan for common upstream responses. */
    static const char *approval[] = {
        "welcome",  "approved", "accepted", "logged in", "online",
        "main menu", "channels", "enter command", "ready",
    };
    static const char *rejection[] = {
        "denied",   "rejected", "invalid",  "wrong",     "failed",
        "bad",      "no access", "unauthorized", "disconnect",
        "kicked",   "banned",   "sorry",
    };
    /* Rejection overrides approval so messages like "[System] Invalid key" are
     * not mistaken for an approved chat line. */
    for (size_t i = 0U; i < sizeof(rejection) / sizeof(rejection[0]); ++i) {
        if (strcasestr(line, rejection[i]) != nullptr) {
            return DDIAL_AUTH_REJECTED;
        }
    }
    for (size_t i = 0U; i < sizeof(approval) / sizeof(approval[0]); ++i) {
        if (strcasestr(line, approval[i]) != nullptr) {
            return DDIAL_AUTH_APPROVED;
        }
    }
    return DDIAL_AUTH_NONE;
}

static bool ddial_client_register_local(host_t *host, ddial_client_t *client)
{
    if (host == nullptr || client == nullptr || client->handle[0] == '\0' ||
        client->locally_registered) {
        return client != nullptr && client->locally_registered;
    }
    user_data_record_t record = {0};
    bool ok = host_user_data_load_existing(host, client->handle, "ddial_upstream",
                                           &record, true);
    if (ok) {
        client->locally_registered = true;
        printf("[ddial] upstream approval timed out; registered '%s' locally\n",
               client->handle);
    } else {
        printf("[ddial] upstream approval timed out; local registration of '%s' "
               "failed (user_data not ready?)\n",
               client->handle);
    }
    return ok;
}

static bool ddial_client_connect_socket(ddial_client_t *client)
{
    if (client == nullptr || client->host[0] == '\0' || client->port <= 0) {
        return false;
    }

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", client->port);

    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = nullptr;
    if (getaddrinfo(client->host, port_str, &hints, &res) != 0 ||
        res == nullptr) {
        return false;
    }

    int fd = -1;
    for (struct addrinfo *rp = res; rp != nullptr; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        return false;
    }

    client->upstream_fd = fd;
    client->connected = true;
    client->auth_sent = false;
    client->recv_buf_len = 0U;
    ddial_client_update_send_time(client);
    return true;
}

static void ddial_client_disconnect(ddial_client_t *client)
{
    if (client == nullptr) {
        return;
    }
    ttak_mutex_lock(&client->lock);
    if (client->upstream_fd >= 0) {
        close(client->upstream_fd);
        client->upstream_fd = -1;
    }
    client->connected = false;
    client->auth_sent = false;
    client->auth_state = DDIAL_AUTH_NONE;
    client->auth_deadline.tv_sec = 0;
    client->auth_deadline.tv_nsec = 0;
    client->recv_buf_len = 0U;
    client->last_send_time.tv_sec = 0;
    client->last_send_time.tv_nsec = 0;
    client->recent_sent_index = 0;
    for (size_t i = 0U; i < DDIAL_RELAY_SENT_HISTORY; ++i) {
        client->recent_sent[i].sent_at.tv_sec = 0;
        client->recent_sent[i].sent_at.tv_nsec = 0;
    }
    ttak_mutex_unlock(&client->lock);
}

static bool ddial_recent_sent_contains(ddial_client_t *client,
                                         const char *handle,
                                         const char *message)
{
    if (client == nullptr || handle == nullptr || message == nullptr) {
        return false;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return false;
    }
    for (size_t i = 0U; i < DDIAL_RELAY_SENT_HISTORY; ++i) {
        if (client->recent_sent[i].sent_at.tv_sec == 0) {
            continue;
        }
        time_t delta = now.tv_sec - client->recent_sent[i].sent_at.tv_sec;
        if (delta < 0 || delta > 5) {
            continue;
        }
        if (strcasecmp(handle, client->recent_sent[i].handle) == 0 &&
            strcmp(message, client->recent_sent[i].message) == 0) {
            return true;
        }
    }
    return false;
}

static void ddial_client_broadcast_line(host_t *host, const char *line)
{
    if (host == nullptr || line == nullptr || line[0] == '\0') {
        return;
    }

    char clean[SSH_CHATTER_MESSAGE_LIMIT];
    ddial_strip_ansi(line, strlen(line), clean, sizeof(clean));
    if (clean[0] == '\0') {
        return;
    }

    ddial_client_t *client = (ddial_client_t *)&host->ddial_relay;

    const char *p = clean;
    while (*p != '\0' && isspace((unsigned char)*p)) {
        ++p;
    }

    /* Try Retro-Dial format: #number(channel:handle tier) message */
    if (*p == '#') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            ++p;
        }
        if (*p == '(' || *p == '[' || *p == '<') {
            const char *paren_end = strchr(p, ')');
            if (paren_end == nullptr) {
                paren_end = strchr(p, ']');
            }
            if (paren_end == nullptr) {
                paren_end = strchr(p, '>');
            }
            if (paren_end != nullptr && paren_end[1] == ' ') {
                size_t inner_len = (size_t)(paren_end - p - 1);
                if (inner_len > 0 && inner_len < SSH_CHATTER_MESSAGE_LIMIT) {
                    char inner[SSH_CHATTER_MESSAGE_LIMIT];
                    memcpy(inner, p + 1, inner_len);
                    inner[inner_len] = '\0';

                    /* Strip any residual ANSI from inner content. */
                    char stripped[SSH_CHATTER_MESSAGE_LIMIT];
                    ddial_strip_ansi(inner, strlen(inner), stripped,
                                     sizeof(stripped));

                    /* Skip CHn: or Tn: channel/type prefix. */
                    const char *handle_start = stripped;
                    if ((handle_start[0] == 'C' || handle_start[0] == 'c') &&
                        (handle_start[1] == 'H' || handle_start[1] == 'h')) {
                        const char *cursor = handle_start + 2;
                        while (*cursor >= '0' && *cursor <= '9') {
                            ++cursor;
                        }
                        if (*cursor == ':') {
                            handle_start = cursor + 1;
                        }
                    } else if (handle_start[0] == 'T' ||
                               handle_start[0] == 't') {
                        const char *cursor = handle_start + 1;
                        while (*cursor >= '0' && *cursor <= '9') {
                            ++cursor;
                        }
                        if (*cursor == ':') {
                            handle_start = cursor + 1;
                        }
                    }
                    while (*handle_start != '\0' &&
                           isspace((unsigned char)*handle_start)) {
                        ++handle_start;
                    }

                    /* Strip trailing tier/status symbols and whitespace. */
                    size_t hlen = strlen(handle_start);
                    while (hlen > 0 &&
                           (isspace((unsigned char)handle_start[hlen - 1]) ||
                            handle_start[hlen - 1] == '*' ||
                            handle_start[hlen - 1] == '$')) {
                        --hlen;
                    }
                    if (hlen >= SSH_CHATTER_USERNAME_LEN) {
                        hlen = SSH_CHATTER_USERNAME_LEN - 1;
                    }
                    char handle[SSH_CHATTER_USERNAME_LEN];
                    memcpy(handle, handle_start, hlen);
                    handle[hlen] = '\0';

                    if (handle[0] != '\0') {
                        const char *message = paren_end + 2;
                        if (message[0] != '\0') {
                            /* Skip our own upstream echo to prevent loops. */
                            if (ddial_recent_sent_contains(client, handle,
                                                           message)) {
                                return;
                            }
                            host_post_client_message(host, handle, message,
                                                     nullptr, nullptr, false);
                            return;
                        }
                    }
                }
            }
        }
    }

    /* Try generic [handle] message format */
    if (clean[0] == '[') {
        const char *end = strchr(clean + 1, ']');
        if (end != nullptr && end[1] == ' ') {
            size_t handle_len = (size_t)(end - clean - 1);
            if (handle_len > 0 && handle_len < SSH_CHATTER_USERNAME_LEN) {
                char handle[SSH_CHATTER_USERNAME_LEN];
                memcpy(handle, clean + 1, handle_len);
                handle[handle_len] = '\0';

                /* Strip any ANSI from handle. */
                char stripped[SSH_CHATTER_USERNAME_LEN];
                ddial_strip_ansi(handle, strlen(handle), stripped,
                                 sizeof(stripped));
                if (stripped[0] != '\0') {
                    const char *message = end + 2;
                    if (message[0] != '\0') {
                        /* Skip our own upstream echo. */
                        if (ddial_recent_sent_contains(client, stripped,
                                                       message)) {
                            return;
                        }
                        host_post_client_message(host, stripped, message,
                                                 nullptr, nullptr, false);
                        return;
                    }
                }
            }
        }
    }

    /* Fallback: broadcast as raw DDial line */
    char prefixed[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(prefixed, sizeof(prefixed), "\033[1;33m[DDial]\033[0m %s", clean);
    chat_room_broadcast(&host->room, prefixed, nullptr);
}

static void ddial_client_process_buffer(host_t *host, ddial_client_t *client,
                                        bool force_flush)
{
    if (host == nullptr || client == nullptr) {
        return;
    }

    char *buf = client->recv_buffer;
    size_t len = client->recv_buf_len;

    for (;;) {
        char *eol = nullptr;
        size_t eol_len = 0U;

        for (size_t i = 0U; i + 1U < len; ++i) {
            if (buf[i] == '\r' && buf[i + 1U] == '\n') {
                eol = &buf[i];
                eol_len = 2U;
                break;
            }
        }
        if (eol == nullptr) {
            for (size_t i = 0U; i < len; ++i) {
                if (buf[i] == '\n') {
                    eol = &buf[i];
                    eol_len = 1U;
                    break;
                }
            }
        }
        if (eol == nullptr && len > 0U && buf[len - 1U] != '\r') {
            for (size_t i = 0U; i < len; ++i) {
                if (buf[i] == '\r') {
                    eol = &buf[i];
                    eol_len = 1U;
                    break;
                }
            }
        }
        if (eol == nullptr) {
            break;
        }

        size_t line_len = (size_t)(eol - buf);
        if (line_len >= SSH_CHATTER_MESSAGE_LIMIT) {
            line_len = SSH_CHATTER_MESSAGE_LIMIT - 1U;
        }
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        memcpy(line, buf, line_len);
        line[line_len] = '\0';
        if (line_len > 0U && line[line_len - 1U] == '\r') {
            line[line_len - 1U] = '\0';
        }
        if (line[0] != '\0') {
            if (client->auth_state == DDIAL_AUTH_PENDING) {
                ddial_auth_state_t cls = ddial_client_classify_auth(line);
                if (cls == DDIAL_AUTH_APPROVED) {
                    client->auth_state = DDIAL_AUTH_APPROVED;
                    printf("[ddial] upstream approved handle '%s'\n",
                           client->handle[0] != '\0' ? client->handle
                                                       : "(unknown)");
                } else if (cls == DDIAL_AUTH_REJECTED) {
                    client->auth_state = DDIAL_AUTH_REJECTED;
                    printf("[ddial] upstream rejected handle '%s'\n",
                           client->handle[0] != '\0' ? client->handle
                                                       : "(unknown)");
                }
            }
            ddial_client_broadcast_line(host, line);
        }

        size_t consumed = line_len + eol_len;
        memmove(buf, buf + consumed, len - consumed);
        len -= consumed;
    }

    if (force_flush && len > 0U) {
        size_t line_len = len;
        if (line_len >= SSH_CHATTER_MESSAGE_LIMIT) {
            line_len = SSH_CHATTER_MESSAGE_LIMIT - 1U;
        }
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        memcpy(line, buf, line_len);
        line[line_len] = '\0';
        if (line_len > 0U && line[line_len - 1U] == '\r') {
            line[line_len - 1U] = '\0';
        }
        if (line[0] != '\0') {
            ddial_client_broadcast_line(host, line);
        }
        len = 0U;
    }

    client->recv_buf_len = len;
}

static void *ddial_client_thread(void *arg)
{
    host_t *host = (host_t *)arg;
    if (host == nullptr) {
        return nullptr;
    }

    ddial_client_t *client = (ddial_client_t *)&host->ddial_relay;

    while (!atomic_load(&client->stop)) {
        if (!client->connected) {
            if (!ddial_client_connect_socket(client)) {
                sleep(DDIAL_CLIENT_RECONNECT_BACKOFF_SEC);
                continue;
            }
            usleep(300000);
            if (client->login_key[0] != '\0') {
                char key_line[128];
                snprintf(key_line, sizeof(key_line), "%s\r\n",
                         client->login_key);
                ttak_mutex_lock(&client->lock);
                if (client->connected && client->upstream_fd >= 0) {
                    (void)ddial_client_send_all(client->upstream_fd, key_line,
                                                strlen(key_line));
                    client->auth_sent = true;
                    ddial_client_update_send_time(client);
                }
                ttak_mutex_unlock(&client->lock);
            }
            if (client->handle[0] != '\0') {
                char handle_line[SSH_CHATTER_USERNAME_LEN + 4];
                snprintf(handle_line, sizeof(handle_line), "%s\r\n",
                         client->handle);
                ttak_mutex_lock(&client->lock);
                if (client->connected && client->upstream_fd >= 0) {
                    (void)ddial_client_send_all(client->upstream_fd,
                                                handle_line,
                                                strlen(handle_line));
                    ddial_client_update_send_time(client);
                }
                ttak_mutex_unlock(&client->lock);
            }
            if (client->login_key[0] != '\0') {
                /* Approval-based DDial: wait up to 10 s for the upstream to
                 * accept the key/handle. */
                client->auth_state = DDIAL_AUTH_PENDING;
                clock_gettime(CLOCK_MONOTONIC, &client->auth_deadline);
                client->auth_deadline.tv_sec += DDIAL_CLIENT_AUTH_TIMEOUT_SEC;
            } else {
                client->auth_state = DDIAL_AUTH_APPROVED;
            }
        }

        if (client->auth_state == DDIAL_AUTH_REJECTED) {
            ddial_client_disconnect(client);
            sleep(DDIAL_CLIENT_RECONNECT_BACKOFF_SEC);
            continue;
        }

        struct pollfd pfd;
        pfd.fd = client->upstream_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int poll_rc = poll(&pfd, 1, DDIAL_CLIENT_POLL_TIMEOUT_MS);
        if (poll_rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            ddial_client_disconnect(client);
            sleep(DDIAL_CLIENT_RECONNECT_BACKOFF_SEC);
            continue;
        }
        if (poll_rc == 0) {
            if (client->auth_state == DDIAL_AUTH_PENDING) {
                struct timespec now;
                if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
                    now.tv_sec > client->auth_deadline.tv_sec) {
                    client->auth_state = DDIAL_AUTH_TIMEOUT;
                    (void)ddial_client_register_local(host, client);
                }
            }
            ddial_client_maybe_keepalive(client);
            continue;
        }
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            ddial_client_disconnect(client);
            sleep(DDIAL_CLIENT_RECONNECT_BACKOFF_SEC);
            continue;
        }
        if ((pfd.revents & POLLIN) == 0) {
            continue;
        }

        char temp[DDIAL_CLIENT_RECV_CHUNK_SIZE];
        ssize_t n = recv(client->upstream_fd, temp, sizeof(temp), 0);
        if (n <= 0) {
            ddial_client_disconnect(client);
            sleep(DDIAL_CLIENT_RECONNECT_BACKOFF_SEC);
            continue;
        }

        bool force_flush = ddial_client_prompt_like(temp, (size_t)n);
        char filtered[DDIAL_CLIENT_RECV_CHUNK_SIZE];
        size_t filtered_len = ddial_filter_telnet_iac(
            temp, (size_t)n, filtered, sizeof(filtered));

        ttak_mutex_lock(&client->lock);
        size_t space = sizeof(client->recv_buffer) - client->recv_buf_len;
        size_t to_copy = filtered_len;
        if (to_copy > space) {
            const size_t buf_cap = sizeof(client->recv_buffer);
            if (client->recv_buf_len > buf_cap / 2U) {
                size_t drop = client->recv_buf_len / 2U;
                memmove(client->recv_buffer, client->recv_buffer + drop,
                        client->recv_buf_len - drop);
                client->recv_buf_len -= drop;
                space = buf_cap - client->recv_buf_len;
            }
            if (to_copy > space) {
                to_copy = space;
            }
        }
        if (to_copy > 0U) {
            memcpy(client->recv_buffer + client->recv_buf_len, filtered,
                   to_copy);
            client->recv_buf_len += to_copy;
        }
        ddial_client_process_buffer(host, client, force_flush);
        ttak_mutex_unlock(&client->lock);
    }

    ddial_client_disconnect(client);
    return nullptr;
}

void host_ddial_init(host_t *host)
{
    if (host == nullptr) {
        return;
    }
    ddial_client_t *client = (ddial_client_t *)&host->ddial_relay;
    memset(client, 0, sizeof(*client));
    client->upstream_fd = -1;
    client->enabled = false;
    client->stop = true;
    client->port = 0;
    client->last_send_time.tv_sec = 0;
    client->last_send_time.tv_nsec = 0;
    if (ttak_mutex_init(&client->lock) == 0) {
        client->lock_initialized = true;
    }

    const char *host_env = getenv("CHATTER_DDIAL_HOST");
    const char *port_env = getenv("CHATTER_DDIAL_PORT");
    const char *key_env = getenv("CHATTER_DDIAL_KEY");
    const char *handle_env = getenv("CHATTER_DDIAL_HANDLE");

    if (host_env != nullptr && host_env[0] != '\0' && port_env != nullptr &&
        port_env[0] != '\0') {
        snprintf(client->host, sizeof(client->host), "%s", host_env);
        client->port = (int)strtol(port_env, nullptr, 10);
        if (key_env != nullptr) {
            snprintf(client->login_key, sizeof(client->login_key), "%s",
                     key_env);
        }
        if (handle_env != nullptr) {
            char clean[DDIAL_MAX_HANDLE_LEN];
            ddial_strip_ansi(handle_env, strlen(handle_env), clean,
                             sizeof(clean));
            snprintf(client->handle, sizeof(client->handle), "%s", clean);
        }
        client->enabled = true;
    }

    memset(&host->ddial_listener, 0, sizeof(host->ddial_listener));
    host->ddial_listener.fd = -1;
    atomic_store(&host->ddial_listener.running, false);
    atomic_store(&host->ddial_listener.stop, true);
}

void host_ddial_client_start(host_t *host)
{
    if (host == nullptr) {
        return;
    }
    ddial_client_t *client = (ddial_client_t *)&host->ddial_relay;
    if (!client->enabled || client->thread_initialized) {
        return;
    }
    client->stop = false;
    ddial_client_update_send_time(client);
    if (pthread_create(&client->thread, nullptr, ddial_client_thread, host) ==
        0) {
        client->thread_initialized = true;
        atomic_store(&client->running, true);
    }
}

static void host_ddial_client_stop(host_t *host)
{
    if (host == nullptr) {
        return;
    }
    ddial_client_t *client = (ddial_client_t *)&host->ddial_relay;
    atomic_store(&client->stop, true);
    if (client->thread_initialized) {
        pthread_join(client->thread, nullptr);
        client->thread_initialized = false;
        atomic_store(&client->running, false);
    }
    ddial_client_disconnect(client);
}

bool host_ddial_client_configure(host_t *host, const char *host_str, int port,
                                 const char *key)
{
    if (host == nullptr || host_str == nullptr || host_str[0] == '\0' ||
        port <= 0) {
        return false;
    }
    host_ddial_client_stop(host);
    ddial_client_t *client = (ddial_client_t *)&host->ddial_relay;
    snprintf(client->host, sizeof(client->host), "%s", host_str);
    client->port = port;
    if (key != nullptr) {
        snprintf(client->login_key, sizeof(client->login_key), "%s", key);
    } else {
        client->login_key[0] = '\0';
    }
    client->handle[0] = '\0';
    client->enabled = true;

    host_ddial_client_start(host);
    return true;
}

void host_ddial_client_disconnect(host_t *host)
{
    if (host == nullptr) {
        return;
    }
    host_ddial_client_stop(host);
    ddial_client_t *client = (ddial_client_t *)&host->ddial_relay;
    client->enabled = false;
    client->host[0] = '\0';
    client->port = 0;
}

void host_ddial_shutdown(host_t *host)
{
    if (host == nullptr) {
        return;
    }
    host_ddial_client_stop(host);
    host_ddial_listener_stop(host);
    ddial_client_t *client = (ddial_client_t *)&host->ddial_relay;
    if (client->lock_initialized) {
        /* libttak mutexes do not always have a destroy function exposed. */
        client->lock_initialized = false;
    }
}

void host_ddial_client_send(host_t *host, const char *handle,
                            const char *message)
{
    if (host == nullptr || handle == nullptr || message == nullptr) {
        return;
    }
    ddial_client_t *client = (ddial_client_t *)&host->ddial_relay;
    if (!client->enabled || !client->connected || client->upstream_fd < 0) {
        return;
    }

    char clean_handle[DDIAL_MAX_HANDLE_LEN];
    ddial_strip_ansi(handle, strlen(handle), clean_handle,
                     sizeof(clean_handle));

    char line[SSH_CHATTER_MESSAGE_LIMIT * 2];
    snprintf(line, sizeof(line), "#1(CH1:%s) %s\r\n", clean_handle, message);

    ttak_mutex_lock(&client->lock);
    if (client->connected && client->upstream_fd >= 0) {
        /* Record in recent-sent ring buffer for echo suppression. */
        size_t idx = client->recent_sent_index % DDIAL_RELAY_SENT_HISTORY;
        snprintf(client->recent_sent[idx].handle,
                 sizeof(client->recent_sent[idx].handle), "%s", clean_handle);
        snprintf(client->recent_sent[idx].message,
                 sizeof(client->recent_sent[idx].message), "%s", message);
        clock_gettime(CLOCK_MONOTONIC, &client->recent_sent[idx].sent_at);
        client->recent_sent_index++;

        (void)ddial_client_send_all(client->upstream_fd, line, strlen(line));
        ddial_client_update_send_time(client);
    }
    ttak_mutex_unlock(&client->lock);
}

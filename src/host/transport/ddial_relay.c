/**
 * @file ddial_relay.c
 * @desc DDial native relay: bidirectional text bridge between Chatter
 *       chat room and an upstream DDial node (e.g. magviz.ca).
 *       Modelled after jace-ddial bridge.py behaviour.
 *
 * Full protocol support:
 *   - ANSI escape sequence stripping so colourised Retro-Dial lines parse
 *     correctly.
 *   - Telnet IAC option negotiation filtering + minimal response (keeps
 *     port-23 telnet links alive).
 *   - poll()-based recv loop with periodic keepalive to avoid guest
 *     timeout on idle connections.
 *   - getaddrinfo() instead of deprecated gethostbyname().
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>

#define DDIAL_KEEPALIVE_INTERVAL_SEC 45
#define DDIAL_POLL_TIMEOUT_MS 5000
#define DDIAL_RECONNECT_BACKOFF_SEC 5
#define DDIAL_RECV_CHUNK_SIZE 4096

static ssize_t ddial_send_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
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

static void ddial_relay_send_nop(ddial_relay_t *relay)
{
    if (relay == nullptr || relay->upstream_fd < 0) {
        return;
    }
    /* Telnet NOP -- harmless on raw TCP too. */
    const unsigned char nop[2] = {0xFF, 0xF1};
    (void)ddial_send_all(relay->upstream_fd, (const char *)nop, 2);
}

static void ddial_relay_update_send_time(ddial_relay_t *relay)
{
    if (relay == nullptr) {
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &relay->last_send_time);
}

static void ddial_relay_maybe_keepalive(ddial_relay_t *relay)
{
    if (relay == nullptr || relay->upstream_fd < 0 ||
        relay->last_send_time.tv_sec == 0) {
        return;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return;
    }
    time_t delta = now.tv_sec - relay->last_send_time.tv_sec;
    if (delta >= (time_t)DDIAL_KEEPALIVE_INTERVAL_SEC) {
        ddial_relay_send_nop(relay);
        ddial_relay_update_send_time(relay);
    }
}

/* Strip ANSI escape sequences and other terminal control chars. */
static size_t ddial_strip_ansi(const char *src, size_t src_len,
                               char *dst, size_t dst_cap)
{
    size_t j = 0;
    for (size_t i = 0; i < src_len && j + 1 < dst_cap; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == 0x1B) {
            if (i + 1 < src_len && src[i + 1] == '[') {
                /* CSI: ESC [ params final_byte */
                i += 2;
                while (i < src_len) {
                    unsigned char p = (unsigned char)src[i];
                    if ((p >= 0x40 && p <= 0x7E)) {
                        break; /* final byte */
                    }
                    ++i;
                }
                continue;
            }
            if (i + 1 < src_len &&
                (src[i + 1] == ']' || src[i + 1] == 'P' ||
                 src[i + 1] == '_' || src[i + 1] == '^')) {
                /* OSC / DCS / APC / PM -- skip until ST (ESC \) or BEL */
                unsigned char ender = (src[i + 1] == ']') ? 0x07 : 0x1B;
                i += 2;
                while (i < src_len) {
                    if ((unsigned char)src[i] == ender) {
                        if (ender == 0x1B && i + 1 < src_len &&
                            src[i + 1] == '\\') {
                            ++i;
                        }
                        break;
                    }
                    ++i;
                }
                continue;
            }
            if (i + 1 < src_len) {
                /* Two-byte escape sequence */
                ++i;
                continue;
            }
            /* Truncated ESC at end of buffer -- drop it. */
            continue;
        }
        if (c == 0x07 || c == 0x08) {
            /* Bell or backspace */
            continue;
        }
        dst[j++] = (char)c;
    }
    dst[j] = '\0';
    return j;
}

/* Filter Telnet IAC sequences out of the data stream.  Respond to basic
 * DO/DONT/WILL/WONT so the server doesn't drop us for ignoring negotiation. */
static size_t ddial_filter_telnet_iac(ddial_relay_t *relay, const char *src,
                                      size_t src_len, char *dst,
                                      size_t dst_cap)
{
    size_t j = 0;
    for (size_t i = 0; i < src_len && j < dst_cap; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c != 0xFF) {
            dst[j++] = (char)c;
            continue;
        }
        if (i + 1 >= src_len) {
            break; /* truncated IAC */
        }
        unsigned char cmd = (unsigned char)src[i + 1];
        if (cmd == 0xFF) {
            /* IAC IAC -> literal 0xFF */
            dst[j++] = '\xFF';
            ++i;
        } else if (cmd >= 0xF0 && cmd <= 0xF9) {
            /* 2-byte command (NOP, GA, etc.) */
            ++i;
        } else if ((cmd >= 0xFB && cmd <= 0xFE) && i + 2 < src_len) {
            /* 3-byte negotiation */
            unsigned char opt = (unsigned char)src[i + 2];
            if (relay != nullptr && relay->upstream_fd >= 0) {
                unsigned char resp_cmd = 0;
                if (cmd == 0xFD) {
                    /* DO opt -> WILL for SGA (3), WONT for everything else */
                    resp_cmd = (opt == 3) ? 0xFB : 0xFC;
                } else if (cmd == 0xFE) {
                    /* DONT opt -> WONT */
                    resp_cmd = 0xFC;
                } else if (cmd == 0xFB) {
                    /* WILL opt -> DO for SGA (3), DONT for everything else */
                    resp_cmd = (opt == 3) ? 0xFD : 0xFE;
                } else if (cmd == 0xFC) {
                    /* WONT opt -> DONT */
                    resp_cmd = 0xFE;
                }
                if (resp_cmd != 0) {
                    unsigned char resp[3] = {0xFF, resp_cmd, opt};
                    (void)ddial_send_all(relay->upstream_fd,
                                         (const char *)resp, 3);
                }
            }
            i += 2;
        } else {
            /* Unrecognised 2-byte IAC sequence -- drop it. */
            ++i;
        }
    }
    return j;
}

static bool ddial_buffer_contains_prompt(const char *buf, size_t len)
{
    if (buf == nullptr || len == 0U) {
        return false;
    }
    static const char *needles[] = {"-->", "assword", "Remote"};
    for (size_t i = 0; i < sizeof(needles) / sizeof(needles[0]); ++i) {
        if (memmem(buf, len, needles[i], strlen(needles[i])) != nullptr) {
            return true;
        }
    }
    return false;
}

static bool ddial_relay_connect_socket(ddial_relay_t *relay)
{
    if (relay == nullptr || relay->host[0] == '\0' || relay->port <= 0) {
        return false;
    }

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", relay->port);

    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = nullptr;
    if (getaddrinfo(relay->host, port_str, &hints, &res) != 0 || res == nullptr) {
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

    relay->upstream_fd = fd;
    relay->connected = true;
    relay->auth_sent = false;
    relay->recv_buf_len = 0U;
    ddial_relay_update_send_time(relay);
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
    relay->last_send_time.tv_sec = 0;
    relay->last_send_time.tv_nsec = 0;
    ttak_mutex_unlock(&relay->lock);
}

static void ddial_relay_broadcast_line(host_t *host, const char *line)
{
    if (host == nullptr || line == nullptr || line[0] == '\0') {
        return;
    }

    /* Strip any residual ANSI from the line before parsing formats. */
    char clean[SSH_CHATTER_MESSAGE_LIMIT];
    ddial_strip_ansi(line, strlen(line), clean, sizeof(clean));
    if (clean[0] == '\0') {
        return;
    }

    const char *p = clean;

    /* Try Retro-Dial format: #number(channel:status) message */
    if (*p == '#') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            ++p;
        }
        if (*p == '(') {
            const char *paren_end = strchr(p, ')');
            if (paren_end != nullptr && paren_end[1] == ' ') {
                size_t handle_len = (size_t)(paren_end - clean);
                if (handle_len > 0 && handle_len < SSH_CHATTER_USERNAME_LEN) {
                    char handle[SSH_CHATTER_USERNAME_LEN];
                    memcpy(handle, clean, handle_len);
                    handle[handle_len] = '\0';
                    const char *message = paren_end + 2;
                    if (message[0] != '\0') {
                        host_post_client_message(host, handle, message, nullptr,
                                                 nullptr, false);
                        return;
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
                const char *message = end + 2;
                if (message[0] != '\0') {
                    host_post_client_message(host, handle, message, nullptr,
                                             nullptr, false);
                    return;
                }
            }
        }
    }

    /* Fallback: broadcast as raw DDial line */
    char prefixed[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(prefixed, sizeof(prefixed),
             "\033[1;33m[DDial]\033[0m %s", clean);
    chat_room_broadcast(&host->room, prefixed, nullptr);
}

static void ddial_relay_process_buffer(host_t *host, ddial_relay_t *relay,
                                       bool force_flush)
{
    if (host == nullptr || relay == nullptr) {
        return;
    }

    char *buf = relay->recv_buffer;
    size_t len = relay->recv_buf_len;

    for (;;) {
        char *eol = nullptr;
        size_t eol_len = 0;

        /* Look for \r\n first */
        for (size_t i = 0; i + 1 < len; ++i) {
            if (buf[i] == '\r' && buf[i + 1] == '\n') {
                eol = &buf[i];
                eol_len = 2;
                break;
            }
        }

        /* Then look for plain \n (bridge.py splits on \n) */
        if (eol == nullptr) {
            for (size_t i = 0; i < len; ++i) {
                if (buf[i] == '\n') {
                    eol = &buf[i];
                    eol_len = 1;
                    break;
                }
            }
        }

        /* Retro-Dial / old BBS systems sometimes use bare \r.
         * If the buffer ends with \r, defer splitting in case the next
         * chunk starts with \n (standard telnet \r\n split across reads). */
        if (eol == nullptr && len > 0 && buf[len - 1] != '\r') {
            for (size_t i = 0; i < len; ++i) {
                if (buf[i] == '\r') {
                    eol = &buf[i];
                    eol_len = 1;
                    break;
                }
            }
        }

        if (eol == nullptr) {
            break;
        }

        size_t line_len = (size_t)(eol - buf);
        if (line_len >= SSH_CHATTER_MESSAGE_LIMIT) {
            line_len = SSH_CHATTER_MESSAGE_LIMIT - 1;
        }
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        memcpy(line, buf, line_len);
        line[line_len] = '\0';

        /* Strip trailing CR if any */
        if (line_len > 0 && line[line_len - 1] == '\r') {
            line[line_len - 1] = '\0';
        }

        if (line[0] != '\0') {
            ddial_relay_broadcast_line(host, line);
        }

        size_t consumed = line_len + eol_len;
        memmove(buf, buf + consumed, len - consumed);
        len -= consumed;
    }

    /* If forced, emit the remainder as a single line even without a newline. */
    if (force_flush && len > 0) {
        size_t line_len = len;
        if (line_len >= SSH_CHATTER_MESSAGE_LIMIT) {
            line_len = SSH_CHATTER_MESSAGE_LIMIT - 1;
        }
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        memcpy(line, buf, line_len);
        line[line_len] = '\0';

        if (line_len > 0 && line[line_len - 1] == '\r') {
            line[line_len - 1] = '\0';
        }

        if (line[0] != '\0') {
            ddial_relay_broadcast_line(host, line);
        }
        len = 0;
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
                sleep(DDIAL_RECONNECT_BACKOFF_SEC);
                continue;
            }

            /* Send login key if configured (bridge.py style) */
            /* Give the server a moment to finish banner / telnet negotation
             * before blasting credentials. */
            usleep(300000);

            if (relay->login_key[0] != '\0') {
                char key_line[128];
                snprintf(key_line, sizeof(key_line), "%s\r\n",
                         relay->login_key);
                (void)ddial_send_all(relay->upstream_fd, key_line,
                                     strlen(key_line));
                relay->auth_sent = true;
                ddial_relay_update_send_time(relay);
            }

            /* Some DDial nodes expect a handle/nickname after the key. */
            if (relay->handle[0] != '\0') {
                char handle_line[SSH_CHATTER_USERNAME_LEN + 4];
                snprintf(handle_line, sizeof(handle_line), "%s\r\n",
                         relay->handle);
                (void)ddial_send_all(relay->upstream_fd, handle_line,
                                     strlen(handle_line));
                ddial_relay_update_send_time(relay);
            }
        }

        struct pollfd pfd;
        pfd.fd = relay->upstream_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int poll_rc = poll(&pfd, 1, DDIAL_POLL_TIMEOUT_MS);
        if (poll_rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            ddial_relay_disconnect(relay);
            sleep(DDIAL_RECONNECT_BACKOFF_SEC);
            continue;
        }

        if (poll_rc == 0) {
            /* Timeout -- send keepalive so guest sessions don't drop. */
            ddial_relay_maybe_keepalive(relay);
            continue;
        }

        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            ddial_relay_disconnect(relay);
            sleep(DDIAL_RECONNECT_BACKOFF_SEC);
            continue;
        }

        if ((pfd.revents & POLLIN) == 0) {
            continue;
        }

        char temp[DDIAL_RECV_CHUNK_SIZE];
        ssize_t n = recv(relay->upstream_fd, temp, sizeof(temp), 0);
        if (n <= 0) {
            ddial_relay_disconnect(relay);
            sleep(DDIAL_RECONNECT_BACKOFF_SEC);
            continue;
        }

        bool force_flush = ddial_buffer_contains_prompt(temp, (size_t)n);

        /* Filter telnet IAC sequences before buffering. */
        char filtered[DDIAL_RECV_CHUNK_SIZE];
        size_t filtered_len = ddial_filter_telnet_iac(
            relay, temp, (size_t)n, filtered, sizeof(filtered));

        ttak_mutex_lock(&relay->lock);
        size_t space = sizeof(relay->recv_buffer) - relay->recv_buf_len;
        size_t to_copy = filtered_len;

        /* Bridge.py style: if the buffer is full, drop the oldest half
         * to make room for new data. */
        if (to_copy > space) {
            const size_t buf_cap = sizeof(relay->recv_buffer);
            if (relay->recv_buf_len > buf_cap / 2) {
                size_t drop = relay->recv_buf_len / 2;
                memmove(relay->recv_buffer, relay->recv_buffer + drop,
                        relay->recv_buf_len - drop);
                relay->recv_buf_len -= drop;
                space = buf_cap - relay->recv_buf_len;
            }
            if (to_copy > space) {
                to_copy = space;
            }
        }

        if (to_copy > 0) {
            memcpy(relay->recv_buffer + relay->recv_buf_len, filtered,
                   to_copy);
            relay->recv_buf_len += to_copy;
        }
        ddial_relay_process_buffer(host, relay, force_flush);
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
    relay->last_send_time.tv_sec = 0;
    relay->last_send_time.tv_nsec = 0;

    if (ttak_mutex_init(&relay->lock) == 0) {
        relay->lock_initialized = true;
    }

    const char *host_env = getenv("CHATTER_DDIAL_HOST");
    const char *port_env = getenv("CHATTER_DDIAL_PORT");
    const char *key_env = getenv("CHATTER_DDIAL_KEY");
    const char *handle_env = getenv("CHATTER_DDIAL_HANDLE");

    if (host_env != nullptr && host_env[0] != '\0' &&
        port_env != nullptr && port_env[0] != '\0') {
        snprintf(relay->host, sizeof(relay->host), "%s", host_env);
        relay->port = (int)strtol(port_env, nullptr, 10);
        if (key_env != nullptr) {
            snprintf(relay->login_key, sizeof(relay->login_key), "%s",
                     key_env);
        }
        if (handle_env != nullptr) {
            snprintf(relay->handle, sizeof(relay->handle), "%s", handle_env);
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
    ddial_relay_update_send_time(relay);
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
        (void)ddial_send_all(relay->upstream_fd, line, strlen(line));
        ddial_relay_update_send_time(relay);
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
    relay->handle[0] = '\0';
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

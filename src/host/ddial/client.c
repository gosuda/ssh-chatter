/**
 * @file ddial_client.c
 * @desc Upstream Diversi Dial client relay for SSH-Chatter.
 *
 * Connects to a configured upstream DDial node and bridges messages between
 * the Chatter chat room and the upstream node.  Protocol reversed from
 * live traffic to hdcbbs.com:2300 (Retro-Dial).
 *
 * Wire summary observed from hdcbbs.com:2300:
 *   - Telnet option negotiation on connect (echo, suppress-go-ahead,
 *     terminal-type).  We answer DO echo, DO SGA, WILL terminal-type and
 *     reply to the terminal-type subnegotiation with "ANSI".
 *   - Server sends "Enter Password or [RETURN]: ".
 *   - Client sends the configured key or a bare CR/LF.
 *   - Server sends welcome banner and a "-->" prompt, plus a status line
 *     such as " #1(T1:?)" where the leading number is our assigned slot.
 *   - Client sets the handle with "/H<handle>\r\n".
 *   - Server echoes public chat as "#slot(Tchannel:handle) message".
 *   - Other traffic (remote chat, system messages, /SP station lists, linked
 *     station names, node lists) arrives as raw lines.
 *
 * Inbound lines are sanitized to remove terminal escape sequences, normalized
 * to a clean ddial.dat-style form, committed to the Chatter history buffer,
 * and then rendered through the normal history broadcast path.  This prevents
 * raw upstream noise from resetting the local terminal and keeps DDial traffic
 * visible in scrollback.
 */

#include "ssh_chatter/ddial_protocol.h"
#include "ssh_chatter/host.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DDIAL_CLIENT_KEEPALIVE_INTERVAL_SEC 45
#define DDIAL_CLIENT_POLL_TIMEOUT_MS 5000
#define DDIAL_CLIENT_RECONNECT_BACKOFF_SEC 5
#define DDIAL_CLIENT_RECV_CHUNK_SIZE 4096
#define DDIAL_CLIENT_AUTH_TIMEOUT_SEC 15
#define DDIAL_CLIENT_LOGIN_PROMPT_TIMEOUT_SEC 8

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
    _Atomic ddial_auth_state_t auth_state;
    struct timespec auth_deadline;
    bool locally_registered;
    char host[256];
    int port;
    char login_key[64];
    char handle[SSH_CHATTER_USERNAME_LEN];
    char recv_buffer[SSH_CHATTER_MESSAGE_LIMIT * 4];
    size_t recv_buf_len;
    struct timespec last_send_time;
    uint16_t slot;
    bool slot_known;
    unsigned int reconnect_attempts;
    struct timespec last_disconnect_time;
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
    atomic_store(&client->auth_state, DDIAL_AUTH_NONE);
    client->auth_deadline.tv_sec = 0;
    client->auth_deadline.tv_nsec = 0;
    client->recv_buf_len = 0U;
    client->last_send_time.tv_sec = 0;
    client->last_send_time.tv_nsec = 0;
    client->slot = 0U;
    client->slot_known = false;
    if (client->reconnect_attempts < 100000U) {
        client->reconnect_attempts++;
    }
    clock_gettime(CLOCK_MONOTONIC, &client->last_disconnect_time);
    ttak_mutex_unlock(&client->lock);
}

static unsigned int ddial_client_backoff_sec(ddial_client_t *client)
{
    if (client == nullptr) {
        return DDIAL_CLIENT_RECONNECT_BACKOFF_SEC;
    }
    unsigned int base = 1U << (client->reconnect_attempts > 5U
                                   ? 5U
                                   : client->reconnect_attempts);
    if (base > 30U) {
        base = 30U;
    }
    if (base < DDIAL_CLIENT_RECONNECT_BACKOFF_SEC) {
        base = DDIAL_CLIENT_RECONNECT_BACKOFF_SEC;
    }
    return base;
}

static bool ddial_client_line_looks_like_kick(const char *line)
{
    if (line == nullptr || line[0] == '\0') {
        return false;
    }
    static const char *needles[] = {
        "kicked",    "disconnected", "timeout",   "timed out",
        "bye",       "goodbye",      "see ya",    "link down",
        "booted",    "removed",      "offline",   "hangup",
        "no carrier"};
    for (size_t i = 0U; i < sizeof(needles) / sizeof(needles[0]); ++i) {
        if (strcasestr(line, needles[i]) != nullptr) {
            return true;
        }
    }
    return false;
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
    if (getaddrinfo(client->host, port_str, &hints, &res) != 0 || res == nullptr) {
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

    int enable = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));

    client->upstream_fd = fd;
    client->connected = true;
    client->auth_sent = false;
    atomic_store(&client->auth_state, DDIAL_AUTH_NONE);
    client->recv_buf_len = 0U;
    client->slot = 0U;
    client->slot_known = false;
    client->reconnect_attempts = 0U;
    client->last_disconnect_time.tv_sec = 0;
    client->last_disconnect_time.tv_nsec = 0;
    ddial_client_update_send_time(client);
    return true;
}

/* Send the initial Telnet "we are a dumb ANSI terminal" negotiation. */
static void ddial_client_send_initial_telnet(ddial_client_t *client)
{
    if (client == nullptr || client->upstream_fd < 0) {
        return;
    }
    /* DO echo(1), DO suppress-go-ahead(3), WILL terminal-type(24). */
    const unsigned char init[] = {0xFF, 0xFD, 0x01, 0xFF, 0xFD, 0x03,
                                  0xFF, 0xFB, 0x18};
    (void)ddial_client_send_all(client->upstream_fd, (const char *)init,
                                sizeof(init));
    ddial_client_update_send_time(client);
}

/* Process incoming telnet bytes, append the resulting text to
 * client->recv_buffer, and send any required IAC replies.  Returns the number
 * of source bytes consumed; incomplete trailing IAC sequences are left in the
 * kernel buffer for the next read. */
static size_t ddial_client_process_telnet(ddial_client_t *client,
                                          const unsigned char *src, size_t src_len)
{
    if (client == nullptr || src == nullptr || src_len == 0U) {
        return 0U;
    }

    unsigned char iac_buf[64];
    size_t iac_len = 0U;
    size_t i = 0U;

    while (i < src_len) {
        if (src[i] != 0xFF) {
            /* Copy plain byte into recv_buffer if space remains. */
            if (client->recv_buf_len < sizeof(client->recv_buffer)) {
                client->recv_buffer[client->recv_buf_len++] = (char)src[i];
            }
            ++i;
            continue;
        }
        if (i + 1U >= src_len) {
            break; /* IAC at end of chunk: wait for next read. */
        }
        unsigned char cmd = src[i + 1U];
        if (cmd == 0xFF) {
            if (client->recv_buf_len < sizeof(client->recv_buffer)) {
                client->recv_buffer[client->recv_buf_len++] = '\xFF';
            }
            i += 2U;
            continue;
        }

        /* Single-byte commands. */
        if (cmd >= 0xF0 && cmd <= 0xF9) {
            i += 2U;
            continue;
        }

        /* Two-byte commands WILL/WONT/DO/DONT. */
        if ((cmd >= 0xFB && cmd <= 0xFE) && i + 2U < src_len) {
            unsigned char opt = src[i + 2U];
            unsigned char reply_cmd = 0;
            switch (cmd) {
            case 0xFB: /* WILL */
                if (opt == 0x01 || opt == 0x03) {
                    reply_cmd = 0xFD; /* DO */
                } else {
                    reply_cmd = 0xFC; /* DON'T */
                }
                break;
            case 0xFC: /* WONT */
                reply_cmd = 0xFE; /* DON'T */
                break;
            case 0xFD: /* DO */
                if (opt == 0x18) { /* terminal-type */
                    reply_cmd = 0xFB; /* WILL */
                } else if (opt == 0x03) { /* suppress-go-ahead */
                    reply_cmd = 0xFB; /* WILL */
                } else {
                    reply_cmd = 0xFC; /* WONT */
                }
                break;
            case 0xFE: /* DONT */
                reply_cmd = 0xFC; /* WONT */
                break;
            }
            if (reply_cmd != 0 && iac_len + 3U <= sizeof(iac_buf)) {
                iac_buf[iac_len++] = 0xFF;
                iac_buf[iac_len++] = reply_cmd;
                iac_buf[iac_len++] = opt;
            }
            i += 3U;
            continue;
        }

        /* Subnegotiation: IAC SB ... IAC SE. */
        if (cmd == 0xFA && i + 2U < src_len) {
            unsigned char opt = src[i + 2U];
            size_t j = i + 3U;
            while (j + 1U < src_len) {
                if (src[j] == 0xFF && src[j + 1U] == 0xF0) {
                    break;
                }
                ++j;
            }
            if (j + 1U >= src_len) {
                break; /* incomplete subnegotiation */
            }

            if (opt == 0x18 && i + 3U < j && src[i + 3U] == 0x01) {
                /* Server asked for terminal type.  Reply with ANSI. */
                if (iac_len + 10U <= sizeof(iac_buf)) {
                    iac_buf[iac_len++] = 0xFF;
                    iac_buf[iac_len++] = 0xFA;
                    iac_buf[iac_len++] = 0x18;
                    iac_buf[iac_len++] = 0x00;
                    iac_buf[iac_len++] = 'A';
                    iac_buf[iac_len++] = 'N';
                    iac_buf[iac_len++] = 'S';
                    iac_buf[iac_len++] = 'I';
                    iac_buf[iac_len++] = 0xFF;
                    iac_buf[iac_len++] = 0xF0;
                }
            }
            i = j + 2U;
            continue;
        }

        /* Unknown two-byte command: skip it. */
        i += 2U;
    }

    if (iac_len > 0U && client->upstream_fd >= 0) {
        (void)ddial_client_send_all(client->upstream_fd, (const char *)iac_buf,
                                    iac_len);
    }

    return i;
}

/* Try to learn our assigned slot from status lines such as " #1(T1:?)". */
static void ddial_client_learn_slot(ddial_client_t *client, const char *line)
{
    if (client == nullptr || line == nullptr) {
        return;
    }
    const char *p = line;
    while (*p != '\0' && isspace((unsigned char)*p)) {
        ++p;
    }
    if (*p != '#') {
        return;
    }
    ++p;
    if (!isdigit((unsigned char)*p)) {
        return;
    }
    unsigned long slot = strtoul(p, nullptr, 10);
    if (slot == 0U || slot > 9999U) {
        return;
    }
    /* Make sure it looks like a status/chat line, not a random '#'. */
    const char *after_num = p;
    while (isdigit((unsigned char)*after_num)) {
        ++after_num;
    }
    if (*after_num != '(' && *after_num != '[') {
        return;
    }
    client->slot = (uint16_t)slot;
    client->slot_known = true;
}

/* Remove terminal escape sequences and control characters that could reset,
 * clear, or otherwise disturb the local terminal when rendered through the
 * Chatter history path.  Keeps tab, CR and LF intact so plain text still
 * flows; everything else below 0x20 (including BEL, BS and FF) is dropped. */
static size_t ddial_client_sanitize_line(const char *src, size_t src_len,
                                         char *dst, size_t dst_cap)
{
    if (src == nullptr || dst == nullptr || dst_cap == 0U) {
        return 0U;
    }

    size_t i = 0U;
    size_t j = 0U;
    while (i < src_len && j + 1U < dst_cap) {
        unsigned char c = (unsigned char)src[i];

        if (c == 0x1B) {
            if (i + 1U >= src_len) {
                ++i;
                continue;
            }
            unsigned char next = (unsigned char)src[i + 1U];

            /* CSI: ESC [ ... final byte @-~ */
            if (next == '[') {
                i += 2U;
                while (i < src_len) {
                    unsigned char p = (unsigned char)src[i];
                    ++i;
                    if (p >= 0x40U && p <= 0x7EU) {
                        break;
                    }
                }
                continue;
            }

            /* OSC: ESC ] ... BEL or ST (ESC \) */
            if (next == ']') {
                i += 2U;
                while (i < src_len) {
                    unsigned char p = (unsigned char)src[i];
                    ++i;
                    if (p == '\a') {
                        break;
                    }
                    if (p == 0x1B && i < src_len && src[i] == '\\') {
                        ++i;
                        break;
                    }
                }
                continue;
            }

            /* DCS / APC / PM: ESC P/_/^ ... ST */
            if (next == 'P' || next == '_' || next == '^') {
                i += 2U;
                while (i + 1U < src_len) {
                    if (src[i] == 0x1B && src[i + 1U] == '\\') {
                        i += 2U;
                        break;
                    }
                    ++i;
                }
                continue;
            }

            /* Single-byte ESC sequences (ESC c terminal reset, charset
             * shifts, etc.) -- swallow the whole two-byte sequence. */
            i += 2U;
            continue;
        }

        if (c == 0x7F) {
            ++i;
            continue;
        }

        if (c < 0x20U && c != '\t' && c != '\r' && c != '\n') {
            ++i;
            continue;
        }

        dst[j++] = (char)c;
        ++i;
    }

    dst[j] = '\0';
    return j;
}

/* Normalize a raw DDial wire line to a clean ddial.dat-style entry before
 * stuffing it into Chatter history.  Terminal noise is stripped, whitespace
 * is trimmed, and the common hdcbbs.com variants (chat, station list, linked
 * station, node list) are preserved in canonical form. */
static void ddial_client_normalize_line(const char *src, char *dst,
                                        size_t dst_cap)
{
    if (src == nullptr || dst == nullptr || dst_cap == 0U) {
        return;
    }
    dst[0] = '\0';

    char sanitized[SSH_CHATTER_MESSAGE_LIMIT];
    ddial_client_sanitize_line(src, strlen(src), sanitized,
                               sizeof(sanitized));

    char *start = sanitized;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        ++start;
    }
    size_t len = strlen(start);
    while (len > 0U && isspace((unsigned char)start[len - 1U])) {
        start[--len] = '\0';
    }
    if (len == 0U) {
        return;
    }

    /* Node list lines such as "7762-.LateNight.Detroit313" are normalized
     * to "#7762-.LateNight.Detroit313" so they look like other DDial rows. */
    const char *dash_dot = strstr(start, "-.");
    if (dash_dot != nullptr && dash_dot > start &&
        isdigit((unsigned char)start[0])) {
        bool all_digits = true;
        for (const char *q = start; q < dash_dot; ++q) {
            if (!isdigit((unsigned char)*q)) {
                all_digits = false;
                break;
            }
        }
        if (all_digits) {
            snprintf(dst, dst_cap, "#%s", start);
            return;
        }
    }

    snprintf(dst, dst_cap, "%s", start);
}

static void ddial_client_broadcast_line(host_t *host, const char *line)
{
    if (host == nullptr || line == nullptr || line[0] == '\0') {
        return;
    }

    ddial_client_t *client = (ddial_client_t *)&host->ddial_relay;

    char normalized[SSH_CHATTER_MESSAGE_LIMIT];
    ddial_client_normalize_line(line, normalized, sizeof(normalized));
    if (normalized[0] == '\0') {
        return;
    }

    /* If the server tells us we are being kicked/timed out, close the socket
     * so the main loop reconnects immediately. */
    if (ddial_client_line_looks_like_kick(normalized)) {
        ttak_mutex_lock(&client->lock);
        if (client->upstream_fd >= 0) {
            close(client->upstream_fd);
            client->upstream_fd = -1;
        }
        client->connected = false;
        if (client->reconnect_attempts < 100000U) {
            client->reconnect_attempts++;
        }
        clock_gettime(CLOCK_MONOTONIC, &client->last_disconnect_time);
        ttak_mutex_unlock(&client->lock);
    }

    /* Commit the normalized DDial line to Chatter history and render it
     * through the normal history path instead of emitting a raw system
     * broadcast that can reset the terminal. */
    chat_history_entry_t stored = {0};
    if (host_history_record_system(host, normalized, &stored)) {
        chat_room_broadcast_entry(&host->room, &stored, nullptr);
    }
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
            char normalized_line[SSH_CHATTER_MESSAGE_LIMIT];
            ddial_client_normalize_line(line, normalized_line, sizeof(normalized_line));
            if (normalized_line[0] != '\0') {
                ddial_client_learn_slot(client, normalized_line);
                ddial_client_broadcast_line(host, normalized_line);
            }
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
            char normalized_line[SSH_CHATTER_MESSAGE_LIMIT];
            ddial_client_normalize_line(line, normalized_line, sizeof(normalized_line));
            if (normalized_line[0] != '\0') {
                ddial_client_learn_slot(client, normalized_line);
                ddial_client_broadcast_line(host, normalized_line);
            }
        }
        len = 0U;
    }

    client->recv_buf_len = len;
}

static bool ddial_client_read_chunk(ddial_client_t *client, host_t *host)
{
    if (client == nullptr || host == nullptr) {
        return false;
    }

    unsigned char temp[DDIAL_CLIENT_RECV_CHUNK_SIZE];
    ssize_t n = recv(client->upstream_fd, temp, sizeof(temp), 0);
    if (n <= 0) {
        return false;
    }

    ttak_mutex_lock(&client->lock);
    size_t before = client->recv_buf_len;
    size_t consumed = ddial_client_process_telnet(client, temp, (size_t)n);

    /* If the buffer is nearly full, drop the oldest half to make room. */
    if (client->recv_buf_len > sizeof(client->recv_buffer) - DDIAL_CLIENT_RECV_CHUNK_SIZE) {
        size_t drop = client->recv_buf_len / 2U;
        memmove(client->recv_buffer, client->recv_buffer + drop,
                client->recv_buf_len - drop);
        client->recv_buf_len -= drop;
    }

    ddial_client_process_buffer(host, client, false);
    (void)consumed;
    (void)before;
    ttak_mutex_unlock(&client->lock);

    return true;
}

/* Read raw bytes from the server, process Telnet options, and append the
 * resulting text to a local scratch buffer.  Returns true once any of the
 * provided needles is found in the accumulated text.  Used only during login
 * so banners are not broadcast prematurely. */
static bool ddial_client_wait_for_any_line(ddial_client_t *client,
                                           const char *const *needles,
                                           size_t needle_count,
                                           int timeout_sec)
{
    if (client == nullptr || needles == nullptr || needle_count == 0U) {
        return false;
    }

    char scratch[SSH_CHATTER_MESSAGE_LIMIT * 4];
    size_t scratch_len = 0U;

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_sec;

    while (!atomic_load(&client->stop)) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec &&
             now.tv_nsec >= deadline.tv_nsec)) {
            return false;
        }

        int remaining_ms = (int)(deadline.tv_sec - now.tv_sec) * 1000;
        remaining_ms += (int)((deadline.tv_nsec - now.tv_nsec) / 1000000L);
        if (remaining_ms <= 0) {
            return false;
        }
        if (remaining_ms > 1000) {
            remaining_ms = 1000;
        }

        struct pollfd pfd;
        pfd.fd = client->upstream_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int rc = poll(&pfd, 1, remaining_ms);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (rc == 0) {
            continue;
        }
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return false;
        }

        unsigned char temp[DDIAL_CLIENT_RECV_CHUNK_SIZE];
        ssize_t n = recv(client->upstream_fd, temp, sizeof(temp), 0);
        if (n <= 0) {
            return false;
        }

        size_t before = client->recv_buf_len;
        (void)ddial_client_process_telnet(client, temp, (size_t)n);

        /* Copy any newly appended text into our scratch buffer. */
        size_t produced = client->recv_buf_len - before;
        if (produced > 0U) {
            size_t space = sizeof(scratch) - scratch_len;
            if (produced > space) {
                produced = space;
            }
            memcpy(scratch + scratch_len, client->recv_buffer + before, produced);
            scratch_len += produced;
            if (scratch_len >= sizeof(scratch)) {
                scratch_len = sizeof(scratch) - 1U;
            }
            scratch[scratch_len] = '\0';
        }

        for (size_t i = 0U; i < needle_count; ++i) {
            if (needles[i] != nullptr &&
                strstr(scratch, needles[i]) != nullptr) {
                return true;
            }
        }
    }
    return false;
}

static bool ddial_client_wait_for_line(ddial_client_t *client,
                                       const char *needle, int timeout_sec)
{
    if (client == nullptr || needle == nullptr) {
        return false;
    }
    return ddial_client_wait_for_any_line(client, &needle, 1U, timeout_sec);
}

static bool ddial_client_do_login(ddial_client_t *client, host_t *host)
{
    if (client == nullptr || host == nullptr) {
        return false;
    }

    ddial_client_send_initial_telnet(client);

    /* Wait for "Enter Password or [RETURN]:" prompt. */
    if (!ddial_client_wait_for_line(client, "Password or [RETURN]",
                                    DDIAL_CLIENT_LOGIN_PROMPT_TIMEOUT_SEC)) {
        printf("[ddial] login prompt not received from %s:%d\n", client->host,
               client->port);
        return false;
    }

    /* Send password or a bare newline. */
    char line[128];
    if (client->login_key[0] != '\0') {
        snprintf(line, sizeof(line), "%s\r\n", client->login_key);
    } else {
        snprintf(line, sizeof(line), "\r\n");
    }
    ttak_mutex_lock(&client->lock);
    if (client->connected && client->upstream_fd >= 0) {
        (void)ddial_client_send_all(client->upstream_fd, line, strlen(line));
        ddial_client_update_send_time(client);
    }
    ttak_mutex_unlock(&client->lock);

    /* Wait for the "-->" prompt and status line. */
    if (!ddial_client_wait_for_line(client, "-->",
                                    DDIAL_CLIENT_AUTH_TIMEOUT_SEC)) {
        printf("[ddial] command prompt not received from %s:%d\n", client->host,
               client->port);
        return false;
    }

    /* Set handle. */
    if (client->handle[0] != '\0') {
        char handle_cmd[SSH_CHATTER_USERNAME_LEN + 8];
        snprintf(handle_cmd, sizeof(handle_cmd), "/H%s\r\n", client->handle);
        ttak_mutex_lock(&client->lock);
        if (client->connected && client->upstream_fd >= 0) {
            (void)ddial_client_send_all(client->upstream_fd, handle_cmd,
                                        strlen(handle_cmd));
            ddial_client_update_send_time(client);
        }
        ttak_mutex_unlock(&client->lock);

        /* Some upstreams echo "Done" after /H, others simply return the
         * command prompt.  Accept either as confirmation that the handle
         * has been applied. */
        static const char *handle_ack_needles[] = {"Done", "-->"};
        if (!ddial_client_wait_for_any_line(
                client, handle_ack_needles,
                sizeof(handle_ack_needles) / sizeof(handle_ack_needles[0]),
                DDIAL_CLIENT_AUTH_TIMEOUT_SEC)) {
            printf("[ddial] handle set not acknowledged by %s:%d\n",
                   client->host, client->port);
            return false;
        }
    }

    atomic_store(&client->auth_state, DDIAL_AUTH_APPROVED);
    printf("[ddial] upstream %s:%d ready (handle '%s')\n", client->host,
           client->port,
           client->handle[0] != '\0' ? client->handle : "(none)");
    return true;
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
            unsigned int backoff = ddial_client_backoff_sec(client);
            if (!ddial_client_connect_socket(client)) {
                sleep(backoff);
                continue;
            }
            if (!ddial_client_do_login(client, host)) {
                ddial_client_disconnect(client);
                sleep(backoff);
                continue;
            }
            /* Flush any banners/prompts accumulated during login. */
            ttak_mutex_lock(&client->lock);
            ddial_client_process_buffer(host, client, false);
            ttak_mutex_unlock(&client->lock);
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
            sleep(ddial_client_backoff_sec(client));
            continue;
        }
        if (poll_rc == 0) {
            ddial_client_maybe_keepalive(client);
            continue;
        }
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            ddial_client_disconnect(client);
            sleep(ddial_client_backoff_sec(client));
            continue;
        }
        if ((pfd.revents & POLLIN) == 0) {
            continue;
        }

        if (!ddial_client_read_chunk(client, host)) {
            ddial_client_disconnect(client);
            sleep(ddial_client_backoff_sec(client));
            continue;
        }
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
    client->slot = 0U;
    client->slot_known = false;
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
            snprintf(client->login_key, sizeof(client->login_key), "%s", key_env);
        }
        if (handle_env != nullptr) {
            char clean[DDIAL_MAX_HANDLE_LEN];
            ddial_strip_ansi(handle_env, strlen(handle_env), clean,
                             sizeof(clean));
            snprintf(client->handle, sizeof(client->handle), "%s", clean);
        }
        if (client->handle[0] == '\0') {
            snprintf(client->handle, sizeof(client->handle), "%s", "chatter");
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
    if (pthread_create(&client->thread, nullptr, ddial_client_thread, host) == 0) {
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

void host_ddial_client_reconnect(host_t *host)
{
    if (host == nullptr) {
        return;
    }
    ddial_client_t *client = (ddial_client_t *)&host->ddial_relay;
    if (!client->enabled || client->host[0] == '\0' || client->port <= 0) {
        return;
    }
    /* Stop the current thread, reset backoff, and start fresh. */
    host_ddial_client_stop(host);
    client->reconnect_attempts = 0U;
    client->enabled = true;
    host_ddial_client_start(host);
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

/* Normalize outbound text to raw 7-bit ASCII.  DDial/Retro-Dial upstreams
 * expect plain ASCII, so strip anything that is not a printable ASCII
 * character, tab, CR, or LF.  Returns the length of the written string. */
static size_t ddial_client_normalize_outbound_text(const char *src,
                                                    size_t src_len,
                                                    char *dst,
                                                    size_t dst_cap)
{
    if (src == nullptr || dst == nullptr || dst_cap == 0U) {
        return 0U;
    }
    size_t i = 0U;
    size_t j = 0U;
    while (i < src_len && j + 1U < dst_cap) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 0x20U && c <= 0x7EU) || c == '\t' || c == '\n' ||
            c == '\r') {
            dst[j++] = (char)c;
        }
        ++i;
    }
    dst[j] = '\0';
    return j;
}

void host_ddial_client_send(host_t *host, const char *handle,
                            const char *message)
{
    (void)handle;
    if (host == nullptr || message == nullptr) {
        return;
    }
    ddial_client_t *client = (ddial_client_t *)&host->ddial_relay;

    ttak_mutex_lock(&client->lock);
    if (!client->enabled || !client->connected || client->upstream_fd < 0 ||
        atomic_load(&client->auth_state) != DDIAL_AUTH_APPROVED) {
        ttak_mutex_unlock(&client->lock);
        return;
    }

    char normalized_message[SSH_CHATTER_MESSAGE_LIMIT];
    ddial_client_normalize_outbound_text(message, strlen(message),
                                         normalized_message,
                                         sizeof(normalized_message));
    if (normalized_message[0] == '\0') {
        ttak_mutex_unlock(&client->lock);
        return;
    }

    char wire_line[SSH_CHATTER_MESSAGE_LIMIT + 4];
    int wire_len = snprintf(wire_line, sizeof(wire_line), "%s\r\n",
                            normalized_message);
    if (wire_len <= 0 || (size_t)wire_len >= sizeof(wire_line)) {
        ttak_mutex_unlock(&client->lock);
        return;
    }

    (void)ddial_client_send_all(client->upstream_fd, wire_line,
                                (size_t)wire_len);
    ddial_client_update_send_time(client);
    ttak_mutex_unlock(&client->lock);
}

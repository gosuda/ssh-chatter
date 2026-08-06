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
 *   - Client sends a bare CR/LF for guest login.
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
#include "ssh_chatter/memory_manager.h"


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
#define DDIAL_CLIENT_PREAUTH_DRAIN_MS (DDIAL_CLIENT_LOGIN_PROMPT_TIMEOUT_SEC * 1000)
#define DDIAL_CLIENT_POSTAUTH_DRAIN_MS 2000

enum {
    DDIAL_TELNET_DATA = 0,
    DDIAL_TELNET_IAC,
    DDIAL_TELNET_OPT,
    DDIAL_TELNET_SB_OPT,
    DDIAL_TELNET_SB,
    DDIAL_TELNET_SB_IAC,
};

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
    char handle[SSH_CHATTER_USERNAME_LEN];
    char key[256];
    char recv_buffer[SSH_CHATTER_MESSAGE_LIMIT * 4];
    size_t recv_buf_len;
    int telnet_state;
    unsigned char telnet_cmd;
    unsigned char telnet_sb_opt;
    unsigned char telnet_sb[64];
    size_t telnet_sb_len;
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
    client->telnet_state = DDIAL_TELNET_DATA;
    client->telnet_cmd = 0U;
    client->telnet_sb_opt = 0U;
    client->telnet_sb_len = 0U;
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
    client->telnet_state = DDIAL_TELNET_DATA;
    client->telnet_cmd = 0U;
    client->telnet_sb_opt = 0U;
    client->telnet_sb_len = 0U;
    client->slot = 0U;
    client->slot_known = false;
    client->reconnect_attempts = 0U;
    client->last_disconnect_time.tv_sec = 0;
    client->last_disconnect_time.tv_nsec = 0;
    ddial_client_update_send_time(client);
    return true;
}

/* Retro-Dial starts Telnet negotiation itself.  Stay passive here and answer
 * only the server's IAC requests from ddial_client_process_telnet(); sending
 * unsolicited options before CONNECT can make hdcbbs.com drop the socket. */
static void ddial_client_begin_telnet(ddial_client_t *client)
{
    if (client == nullptr || client->upstream_fd < 0) {
        return;
    }
    ddial_client_update_send_time(client);
}

static void ddial_client_append_telnet_reply(unsigned char *buf, size_t *len,
                                             size_t cap, unsigned char cmd,
                                             unsigned char opt)
{
    if (buf == nullptr || len == nullptr || *len + 3U > cap) {
        return;
    }
    buf[(*len)++] = 0xFF;
    buf[(*len)++] = cmd;
    buf[(*len)++] = opt;
}

static void ddial_client_append_terminal_type_reply(unsigned char *buf,
                                                    size_t *len, size_t cap)
{
    static const unsigned char reply[] = {0xFF, 0xFA, 0x18, 0x00,
                                          'A',  'N',  'S',  'I',
                                          0xFF, 0xF0};
    if (buf == nullptr || len == nullptr || *len + sizeof(reply) > cap) {
        return;
    }
    memcpy(buf + *len, reply, sizeof(reply));
    *len += sizeof(reply);
}

static void ddial_client_process_telnet_option(ddial_client_t *client,
                                               unsigned char *iac_buf,
                                               size_t *iac_len,
                                               size_t iac_cap,
                                               unsigned char cmd,
                                               unsigned char opt)
{
    (void)client;
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
    if (reply_cmd != 0) {
        ddial_client_append_telnet_reply(iac_buf, iac_len, iac_cap, reply_cmd,
                                         opt);
    }
}

/* Process incoming Telnet bytes and append resulting text to recv_buffer.
 * Telnet control sequences may be split across arbitrarily small TCP reads,
 * so parser state is stored on the client instead of assuming whole IAC
 * commands arrive in one recv(). */
static void ddial_client_process_telnet(ddial_client_t *client,
                                        const unsigned char *src,
                                        size_t src_len)
{
    if (client == nullptr || src == nullptr || src_len == 0U) {
        return;
    }

    unsigned char iac_buf[128];
    size_t iac_len = 0U;

    for (size_t i = 0U; i < src_len; ++i) {
        unsigned char byte = src[i];
        switch (client->telnet_state) {
        case DDIAL_TELNET_DATA:
            if (byte == 0xFF) {
                client->telnet_state = DDIAL_TELNET_IAC;
            } else if (client->recv_buf_len < sizeof(client->recv_buffer)) {
                client->recv_buffer[client->recv_buf_len++] = (char)byte;
            }
            break;
        case DDIAL_TELNET_IAC:
            if (byte == 0xFF) {
                if (client->recv_buf_len < sizeof(client->recv_buffer)) {
                    client->recv_buffer[client->recv_buf_len++] = '\xFF';
                }
                client->telnet_state = DDIAL_TELNET_DATA;
            } else if (byte >= 0xF0 && byte <= 0xFA) {
                if (byte == 0xFA) {
                    client->telnet_sb_len = 0U;
                    client->telnet_sb_opt = 0U;
                    client->telnet_state = DDIAL_TELNET_SB_OPT;
                } else {
                    client->telnet_state = DDIAL_TELNET_DATA;
                }
            } else if (byte >= 0xFB && byte <= 0xFE) {
                client->telnet_cmd = byte;
                client->telnet_state = DDIAL_TELNET_OPT;
            } else {
                client->telnet_state = DDIAL_TELNET_DATA;
            }
            break;
        case DDIAL_TELNET_OPT:
            ddial_client_process_telnet_option(client, iac_buf, &iac_len,
                                               sizeof(iac_buf),
                                               client->telnet_cmd, byte);
            client->telnet_state = DDIAL_TELNET_DATA;
            break;
        case DDIAL_TELNET_SB_OPT:
            client->telnet_sb_opt = byte;
            client->telnet_sb_len = 0U;
            client->telnet_state = DDIAL_TELNET_SB;
            break;
        case DDIAL_TELNET_SB:
            if (byte == 0xFF) {
                client->telnet_state = DDIAL_TELNET_SB_IAC;
            } else if (client->telnet_sb_len < sizeof(client->telnet_sb)) {
                client->telnet_sb[client->telnet_sb_len++] = byte;
            }
            break;
        case DDIAL_TELNET_SB_IAC:
            if (byte == 0xF0) {
                if (client->telnet_sb_opt == 0x18 &&
                    client->telnet_sb_len > 0U &&
                    client->telnet_sb[0] == 0x01) {
                    ddial_client_append_terminal_type_reply(
                        iac_buf, &iac_len, sizeof(iac_buf));
                }
                client->telnet_sb_len = 0U;
                client->telnet_state = DDIAL_TELNET_DATA;
            } else if (byte == 0xFF) {
                if (client->telnet_sb_len < sizeof(client->telnet_sb)) {
                    client->telnet_sb[client->telnet_sb_len++] = 0xFF;
                }
                client->telnet_state = DDIAL_TELNET_SB;
            } else {
                client->telnet_state = DDIAL_TELNET_DATA;
            }
            break;
        default:
            client->telnet_state = DDIAL_TELNET_DATA;
            break;
        }
    }

    if (iac_len > 0U && client->upstream_fd >= 0) {
        (void)ddial_client_send_all(client->upstream_fd, (const char *)iac_buf,
                                    iac_len);
    }
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

        if ((c < 0x20U && c != '\t' && c != '\r' && c != '\n') || c == 0xFF) {
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

static bool ddial_parse_incoming_chat(const char *line, char *out_handle, size_t handle_cap, const char **out_message)
{
    if (line == nullptr || line[0] != '#') {
        return false;
    }
    const char *p = line + 1;
    while (*p != '\0' && isdigit((unsigned char)*p)) {
        p++;
    }
    if (*p != '(' && *p != '[' && *p != '<') {
        return false;
    }
    p++;
    if (*p != 'T' || !isdigit((unsigned char)*(p + 1)) || *(p + 2) != ':') {
        return false;
    }
    p += 3;
    const char *handle_start = p;
    while (*p != '\0' && *p != ')' && *p != ']' && *p != '>') {
        p++;
    }
    if (*p == '\0') {
        return false;
    }
    const char *handle_end = p;
    while (handle_end > handle_start && (*(handle_end - 1) == '*' || *(handle_end - 1) == '$')) {
        handle_end--;
    }
    size_t len = (size_t)(handle_end - handle_start);
    if (len >= handle_cap) {
        len = handle_cap - 1;
    }
    memcpy(out_handle, handle_start, len);
    out_handle[len] = '\0';

    p++;
    if (*p == ' ') {
        p++;
    }
    if (out_message != nullptr) {
        *out_message = p;
    }
    return true;
}

static void host_history_delete_matching_message(host_t *host, const char *message)
{
    ttak_mutex_lock(&host->lock);
    if (host->history == nullptr || host->history_count == 0U) {
        ttak_mutex_unlock(&host->lock);
        return;
    }
    for (size_t i = host->history_count; i > 0U; --i) {
        size_t idx = i - 1U;
        chat_history_entry_t *entry = &host->history[idx];
        if (entry->is_user_message &&
            strcmp(entry->message, message) == 0) {
            if (idx + 1U < host->history_count) {
                memmove(host->history + idx, host->history + idx + 1U,
                        (host->history_count - idx - 1U) * sizeof(chat_history_entry_t));
            }
            host->history_count--;
            break;
        }
    }
    ttak_mutex_unlock(&host->lock);
}

/* Broadcast an already-normalized DDial line into the Chatter room.  This
 * function is called without client->lock held; kick/timeout handling and
 * slot learning happen during line extraction under the lock. */
static void ddial_client_broadcast_line(host_t *host, const char *line)
{
    if (host == nullptr || line == nullptr || line[0] == '\0') {
        return;
    }

    ddial_client_t *client = (ddial_client_t *)&host->ddial_relay;

    char parsed_handle[DDIAL_MAX_HANDLE_LEN];
    const char *parsed_message = nullptr;
    bool is_our_line = false;

    if (ddial_parse_incoming_chat(line, parsed_handle, sizeof(parsed_handle),
                                  &parsed_message)) {
        if (client->handle[0] != '\0' &&
            strcasecmp(parsed_handle, client->handle) == 0) {
            is_our_line = true;
        }
    }

    char display_line[SSH_CHATTER_MESSAGE_LIMIT + 128];
    if (is_our_line) {
        host_history_delete_matching_message(host, parsed_message);

        const char *color_start = "";
        const char *color_end = "";
        if (host->user_theme.userColor != nullptr &&
            host->user_theme.userColor[0] != '\0') {
            color_start = host->user_theme.userColor;
            color_end = ANSI_RESET;
        } else {
            color_start = ANSI_GREEN;
            color_end = ANSI_RESET;
        }
        snprintf(display_line, sizeof(display_line), "%s[SENT]%s %s",
                 color_start, color_end, line);
    } else {
        snprintf(display_line, sizeof(display_line), "%s", line);
    }

    /* Commit the normalized DDial line to Chatter history and render it
     * through the normal history path instead of emitting a raw system
     * broadcast that can reset the terminal. */
    chat_history_entry_t stored = {0};
    if (host_history_record_system(host, display_line, &stored)) {
        chat_room_broadcast_entry(&host->room, &stored, nullptr);
    }
}

#define DDIAL_CLIENT_MAX_EXTRACTED_LINES 64U

typedef struct {
    char line[SSH_CHATTER_MESSAGE_LIMIT];
} ddial_extracted_line_t;

/* Extract complete lines from the recv buffer.  Must be called with
 * client->lock held.  Returns the number of lines extracted into out_lines.
 * If a kick/timeout line is seen, tears down the connection under the lock
 * and sets *out_disconnected.  Slot learning is performed here so the lock
 * is not held while broadcasting to the room. */
static size_t ddial_client_extract_lines(ddial_client_t *client,
                                         ddial_extracted_line_t *out_lines,
                                         size_t max_lines,
                                         bool force_flush,
                                         bool *out_disconnected)
{
    if (client == nullptr || out_lines == nullptr || max_lines == 0U) {
        return 0U;
    }

    char *buf = client->recv_buffer;
    size_t len = client->recv_buf_len;
    size_t extracted = 0U;
    if (out_disconnected != nullptr) {
        *out_disconnected = false;
    }

    for (;;) {
        if (extracted >= max_lines) {
            break;
        }

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

        size_t consumed = line_len + eol_len;
        memmove(buf, buf + consumed, len - consumed);
        len -= consumed;

        if (line[0] == '\0') {
            continue;
        }

        char normalized_line[SSH_CHATTER_MESSAGE_LIMIT];
        ddial_client_normalize_line(line, normalized_line,
                                    sizeof(normalized_line));
        if (normalized_line[0] == '\0') {
            continue;
        }

        /* If the server tells us we are being kicked/timed out, tear down the
         * connection while still under the lock. */
        if (ddial_client_line_looks_like_kick(normalized_line)) {
            if (client->upstream_fd >= 0) {
                close(client->upstream_fd);
                client->upstream_fd = -1;
            }
            client->connected = false;
            if (client->reconnect_attempts < 100000U) {
                client->reconnect_attempts++;
            }
            clock_gettime(CLOCK_MONOTONIC, &client->last_disconnect_time);
            client->recv_buf_len = 0U;
            if (out_disconnected != nullptr) {
                *out_disconnected = true;
            }
            return 0U;
        }

        ddial_client_learn_slot(client, normalized_line);
        snprintf(out_lines[extracted].line, sizeof(out_lines[extracted].line),
                 "%s", normalized_line);
        ++extracted;
    }

    if (force_flush && len > 0U && extracted < max_lines) {
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
        len = 0U;

        if (line[0] != '\0') {
            char normalized_line[SSH_CHATTER_MESSAGE_LIMIT];
            ddial_client_normalize_line(line, normalized_line,
                                        sizeof(normalized_line));
            if (normalized_line[0] != '\0') {
                if (ddial_client_line_looks_like_kick(normalized_line)) {
                    if (client->upstream_fd >= 0) {
                        close(client->upstream_fd);
                        client->upstream_fd = -1;
                    }
                    client->connected = false;
                    if (client->reconnect_attempts < 100000U) {
                        client->reconnect_attempts++;
                    }
                    clock_gettime(CLOCK_MONOTONIC,
                                  &client->last_disconnect_time);
                    client->recv_buf_len = 0U;
                    if (out_disconnected != nullptr) {
                        *out_disconnected = true;
                    }
                    return 0U;
                }

                ddial_client_learn_slot(client, normalized_line);
                snprintf(out_lines[extracted].line,
                         sizeof(out_lines[extracted].line), "%s",
                         normalized_line);
                ++extracted;
            }
        }
    }

    client->recv_buf_len = len;
    return extracted;
}

/* Lock-protected wrapper that extracts buffered lines and then broadcasts
 * them without holding client->lock. */
static void ddial_client_flush_received_lines(host_t *host,
                                              ddial_client_t *client,
                                              bool force_flush)
{
    if (host == nullptr || client == nullptr) {
        return;
    }

    ddial_extracted_line_t lines[DDIAL_CLIENT_MAX_EXTRACTED_LINES];
    bool disconnected = false;

    ttak_mutex_lock(&client->lock);
    size_t count = ddial_client_extract_lines(client, lines,
                                              DDIAL_CLIENT_MAX_EXTRACTED_LINES,
                                              force_flush, &disconnected);
    ttak_mutex_unlock(&client->lock);

    if (disconnected) {
        return;
    }

    for (size_t i = 0U; i < count; ++i) {
        ddial_client_broadcast_line(host, lines[i].line);
    }
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

    ddial_extracted_line_t lines[DDIAL_CLIENT_MAX_EXTRACTED_LINES];
    bool disconnected = false;

    ttak_mutex_lock(&client->lock);
    ddial_client_process_telnet(client, temp, (size_t)n);

    /* If the buffer is nearly full, drop the oldest half to make room. */
    if (client->recv_buf_len > sizeof(client->recv_buffer) - DDIAL_CLIENT_RECV_CHUNK_SIZE) {
        size_t drop = client->recv_buf_len / 2U;
        memmove(client->recv_buffer, client->recv_buffer + drop,
                client->recv_buf_len - drop);
        client->recv_buf_len -= drop;
    }

    size_t count = ddial_client_extract_lines(client, lines,
                                              DDIAL_CLIENT_MAX_EXTRACTED_LINES,
                                              false, &disconnected);
    ttak_mutex_unlock(&client->lock);

    for (size_t i = 0U; i < count; ++i) {
        ddial_client_broadcast_line(host, lines[i].line);
    }

    (void)disconnected;
    return true;
}

static bool ddial_client_drain_for(ddial_client_t *client, host_t *host,
                                   int timeout_ms)
{
    if (client == nullptr || host == nullptr || client->upstream_fd < 0 ||
        timeout_ms <= 0) {
        return true;
    }

    struct timespec start;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return true;
    }

    for (;;) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            return true;
        }
        long elapsed_ms = (long)((now.tv_sec - start.tv_sec) * 1000L) +
                          (long)((now.tv_nsec - start.tv_nsec) / 1000000L);
        if (elapsed_ms >= timeout_ms) {
            return true;
        }

        int remaining_ms = timeout_ms - (int)elapsed_ms;
        int poll_ms = remaining_ms < 100 ? remaining_ms : 100;
        struct pollfd pfd = {.fd = client->upstream_fd, .events = POLLIN};
        int poll_rc = poll(&pfd, 1, poll_ms);
        if (poll_rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (poll_rc == 0) {
            continue;
        }
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return false;
        }
        if ((pfd.revents & POLLIN) != 0 &&
            !ddial_client_read_chunk(client, host)) {
            return false;
        }
    }
}

static bool ddial_client_buffer_contains_ci_locked(ddial_client_t *client,
                                                   const char *needle)
{
    if (client == nullptr || needle == nullptr || needle[0] == '\0') {
        return false;
    }
    size_t needle_len = strlen(needle);
    if (needle_len == 0U || client->recv_buf_len < needle_len) {
        return false;
    }
    for (size_t i = 0U; i + needle_len <= client->recv_buf_len; ++i) {
        size_t j = 0U;
        while (j < needle_len &&
               tolower((unsigned char)client->recv_buffer[i + j]) ==
                   tolower((unsigned char)needle[j])) {
            ++j;
        }
        if (j == needle_len) {
            return true;
        }
    }
    return false;
}

static bool ddial_client_login_prompt_seen(ddial_client_t *client)
{
    bool seen = false;
    ttak_mutex_lock(&client->lock);
    seen = ddial_client_buffer_contains_ci_locked(client, "password") ||
           ddial_client_buffer_contains_ci_locked(client, "return");
    ttak_mutex_unlock(&client->lock);
    return seen;
}

static bool ddial_client_drain_until_login_prompt(ddial_client_t *client,
                                                  host_t *host,
                                                  int timeout_ms)
{
    if (client == nullptr || host == nullptr || client->upstream_fd < 0 ||
        timeout_ms <= 0) {
        return true;
    }

    struct timespec start;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return true;
    }

    for (;;) {
        if (ddial_client_login_prompt_seen(client)) {
            return true;
        }

        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            return true;
        }
        long elapsed_ms = (long)((now.tv_sec - start.tv_sec) * 1000L) +
                          (long)((now.tv_nsec - start.tv_nsec) / 1000000L);
        if (elapsed_ms >= timeout_ms) {
            return true;
        }

        int remaining_ms = timeout_ms - (int)elapsed_ms;
        int poll_ms = remaining_ms < 100 ? remaining_ms : 100;
        struct pollfd pfd = {.fd = client->upstream_fd, .events = POLLIN};
        int poll_rc = poll(&pfd, 1, poll_ms);
        if (poll_rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (poll_rc == 0) {
            continue;
        }
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return false;
        }
        if ((pfd.revents & POLLIN) != 0 &&
            !ddial_client_read_chunk(client, host)) {
            return false;
        }
    }
}

static bool ddial_client_do_login(ddial_client_t *client, host_t *host)
{
    if (client == nullptr || host == nullptr) {
        return false;
    }

    ddial_client_begin_telnet(client);

    /* Wait for the password prompt when it arrives, then send password or RETURN.
     * On old/slow links this avoids racing input ahead of Retro-Dial's CONNECT/password state. */
    if (!ddial_client_drain_until_login_prompt(
            client, host, DDIAL_CLIENT_PREAUTH_DRAIN_MS)) {
        return false;
    }

    /* Send password if configured, or bare RETURN for guest login. */
    char line[512];
    if (client->key[0] != '\0') {
        snprintf(line, sizeof(line), "%s\r\n", client->key);
    } else {
        snprintf(line, sizeof(line), "\r\n");
    }
    ttak_mutex_lock(&client->lock);
    if (client->connected && client->upstream_fd >= 0) {
        (void)ddial_client_send_all(client->upstream_fd, line, strlen(line));
        ddial_client_update_send_time(client);
    }
    ttak_mutex_unlock(&client->lock);

    if (!ddial_client_drain_for(client, host, DDIAL_CLIENT_POSTAUTH_DRAIN_MS)) {
        return false;
    }

    /* Send handle after the guest/member login has had time to enter the chat loop. */
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

    SSHC_SAFE_BLOCK_BEGIN() {
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
                /* Flush any banners/prompts accumulated during login.
                 * Extraction happens under client->lock; broadcasting does not. */
                ddial_client_flush_received_lines(host, client, false);
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
    } SSHC_SAFE_BLOCK_END({
        printf("[ddial] SEGV/SIGBUS swallowed in ddial_client_thread\n");
    });

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
    const char *handle_env = getenv("CHATTER_DDIAL_HANDLE");
    const char *key_env = getenv("CHATTER_DDIAL_KEY");

    if (host_env != nullptr && host_env[0] != '\0' && port_env != nullptr &&
        port_env[0] != '\0') {
        snprintf(client->host, sizeof(client->host), "%s", host_env);
        client->port = (int)strtol(port_env, nullptr, 10);
        if (handle_env != nullptr) {
            char clean[DDIAL_MAX_HANDLE_LEN];
            ddial_strip_ansi(handle_env, strlen(handle_env), clean,
                             sizeof(clean));
            snprintf(client->handle, sizeof(client->handle), "%s", clean);
        }
        if (client->handle[0] == '\0') {
            snprintf(client->handle, sizeof(client->handle), "%s", "chatter");
        }
        if (key_env != nullptr && key_env[0] != '\0') {
            snprintf(client->key, sizeof(client->key), "%s", key_env);
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
                                 const char *handle, const char *key)
{
    if (host == nullptr || host_str == nullptr || host_str[0] == '\0' ||
        port <= 0) {
        return false;
    }
    host_ddial_client_stop(host);
    ddial_client_t *client = (ddial_client_t *)&host->ddial_relay;
    snprintf(client->host, sizeof(client->host), "%s", host_str);
    client->port = port;
    if (handle != nullptr && handle[0] != '\0') {
        char clean[DDIAL_MAX_HANDLE_LEN];
        ddial_strip_ansi(handle, strlen(handle), clean, sizeof(clean));
        snprintf(client->handle, sizeof(client->handle), "%s", clean);
    } else {
        snprintf(client->handle, sizeof(client->handle), "%s", "chatter");
    }
    if (key != nullptr && key[0] != '\0') {
        snprintf(client->key, sizeof(client->key), "%s", key);
    } else {
        client->key[0] = '\0';
    }
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
    if (host == nullptr || message == nullptr) {
        return;
    }
    ddial_client_t *client = (ddial_client_t *)&host->ddial_relay;

    /* Snapshot connection state briefly, then format the wire message without
     * holding client->lock so inbound traffic can still be processed. */
    ttak_mutex_lock(&client->lock);
    bool enabled = client->enabled;
    bool connected = client->connected;
    int upstream_fd = client->upstream_fd;
    ddial_auth_state_t auth_state = atomic_load(&client->auth_state);
    ttak_mutex_unlock(&client->lock);

    if (!enabled || !connected || upstream_fd < 0 ||
        auth_state != DDIAL_AUTH_APPROVED) {
        return;
    }

    char prefixed_message[SSH_CHATTER_MESSAGE_LIMIT];
    if (handle != nullptr && handle[0] != '\0') {
        char clean_handle[DDIAL_MAX_HANDLE_LEN];
        ddial_strip_ansi(handle, strlen(handle), clean_handle,
                         sizeof(clean_handle));
        snprintf(prefixed_message, sizeof(prefixed_message), "%s) %s",
                 clean_handle, message);
    } else {
        snprintf(prefixed_message, sizeof(prefixed_message), "%s", message);
    }

    char normalized_message[SSH_CHATTER_MESSAGE_LIMIT];
    ddial_client_normalize_outbound_text(prefixed_message,
                                         strlen(prefixed_message),
                                         normalized_message,
                                         sizeof(normalized_message));
    if (normalized_message[0] == '\0') {
        return;
    }

    char wire_line[SSH_CHATTER_MESSAGE_LIMIT + 4];
    int wire_len = snprintf(wire_line, sizeof(wire_line), "%s\r\n",
                            normalized_message);
    if (wire_len <= 0 || (size_t)wire_len >= sizeof(wire_line)) {
        return;
    }

    /* Hold the lock only for the actual socket write and send-time update. */
    ttak_mutex_lock(&client->lock);
    if (client->enabled && client->connected && client->upstream_fd == upstream_fd &&
        atomic_load(&client->auth_state) == DDIAL_AUTH_APPROVED) {
        (void)ddial_client_send_all(client->upstream_fd, wire_line,
                                    (size_t)wire_len);
        ddial_client_update_send_time(client);
    }
    ttak_mutex_unlock(&client->lock);
}

bool host_ddial_client_send_raw(host_t *host, const char *data,
                                size_t data_len)
{
    if (host == nullptr || data == nullptr || data_len == 0U) {
        return false;
    }

    ddial_client_t *client = (ddial_client_t *)&host->ddial_relay;
    ttak_mutex_lock(&client->lock);
    if (!client->enabled || !client->connected || client->upstream_fd < 0) {
        ttak_mutex_unlock(&client->lock);
        return false;
    }

    bool ok = ddial_client_send_all(client->upstream_fd, data, data_len) ==
              (ssize_t)data_len;
    if (ok) {
        ddial_client_update_send_time(client);
    }
    ttak_mutex_unlock(&client->lock);
    return ok;
}

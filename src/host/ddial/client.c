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
 *   - With a configured password the client waits 5 seconds after CONNECT,
 *     sends the password, and verifies the login went through; a rejected
 *     password tears the connection down instead of falling back to guest.
 *   - Without a password the client sends a bare CR/LF for guest login.
 *   - Server sends welcome banner and a "-->" prompt, plus a status line
 *     such as " #1(T1:?)" where the leading number is our assigned slot.
 *   - Client sets the handle with "/H<handle>\r\n".
 *   - Server echoes public chat as "#slot(Tchannel:handle) message".
 *   - Other traffic (remote chat, system messages, /SP station lists, linked
 *     station names, node lists) arrives as raw lines.
 *
 * Two modes (CHATTER_DDIAL_MODE):
 *   station (default)  The account is a Station Link: outbound chat uses the
 *                      full "#slot[T1:handle) msg" wire form, and login /
 *                      logout / }}} station broadcasts are sent for local
 *                      users.
 *   user ("hijack")    The account is an ordinary DDial user.  Everything we
 *                      type is shown by the remote as "#N[T1:ourhandle) ...",
 *                      so outbound chat is sent as "speaker: msg", link-only
 *                      traffic is never sent, and the remote's echo of our
 *                      own lines is dropped instead of re-entering Chatter.
 *                      Old ddials disable an account that logs in twice
 *                      (easy over telnet), so this mode also holds a
 *                      process-wide account lock, persists the last hang-up
 *                      time across restarts and waits out a re-login
 *                      cooldown, says /Q before hanging up, and stops
 *                      reconnecting if the remote reports a duplicate login.
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
#include <fcntl.h>
#include <strings.h>
#include <sys/file.h>
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
/* Password login grace period: Retro-Dial needs a moment after CONNECT
 * before its telnet negotiation and password prompt settle, so wait a fixed
 * 5 seconds before sending the configured password. */
#define DDIAL_CLIENT_AUTH_PRE_SEND_DELAY_MS 5000
/* How long to watch for a login success/failure marker after the password
 * has been sent. */
#define DDIAL_CLIENT_AUTH_RESULT_TIMEOUT_MS 10000
/* User mode: minimum quiet time between hanging up and dialing back in, so
 * the remote has dropped the old session before we log in again. */
#define DDIAL_CLIENT_USER_RELOGIN_COOLDOWN_SEC 120U
/* User mode: how long an outbound line is remembered for echo matching. */
#define DDIAL_CLIENT_ECHO_WINDOW_SEC 90

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
    /* Password login verdict: 0 = pending, 1 = success marker seen,
     * -1 = failure marker/re-prompt seen.  Only meaningful when a password
     * (key) is configured. */
    int auth_result;
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
    struct timespec last_broadcast_time;
    /* User-account ("hijack") mode, CHATTER_DDIAL_MODE=user: the relay is a
     * plain user on the remote DDial, not a Station Link.  See client.c. */
    bool user_mode;
    bool locked_out;
    int account_lock_fd;
    unsigned int relogin_cooldown_sec;
    char recent_sent[8][256];
    struct timespec recent_sent_at[8];
    size_t recent_sent_head;
} ddial_client_t;

_Static_assert(sizeof(ddial_client_t) == sizeof(ddial_relay_t),
               "ddial_client_t must mirror ddial_relay_t");

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



/* ---- user ("hijack") mode helpers ------------------------------------- */

/* True for lines that carry user-authored text (public, dual-channel,
 * link-prefixed or private chat).  Such lines must never be interpreted as
 * server status: someone typing "bye" is not a disconnect. */
static bool ddial_client_line_is_chat_content(const char *line)
{
    const char *p = line;
    /* The "-->" input prompt is not newline-terminated, so the next line
     * can arrive glued to it. */
    for (;;) {
        while (*p == ' ') {
            ++p;
        }
        if (strncmp(p, "-->", 3U) != 0) {
            break;
        }
        p += 3;
    }
    while (isdigit((unsigned char)*p)) {
        ++p; /* link prefix such as 99#2[... */
    }
    if (*p == '~') {
        ++p;
    }
    if (*p == 'P' || *p == 'p') {
        ++p;
    }
    if (*p != '#') {
        return false;
    }
    ++p;
    if (!isdigit((unsigned char)*p)) {
        return false;
    }
    while (isdigit((unsigned char)*p)) {
        ++p;
    }
    return (*p == '[' || *p == '(' || *p == '<') && strchr(p, ')') != nullptr;
}

static bool ddial_client_line_looks_like_lockout(const char *line)
{
    static const char *needles[] = {
        "already logged",  "already on line", "already online",
        "logged in twice", "account disabled", "account locked",
        "account in use",  "is in use",       "duplicate login",
    };
    for (size_t i = 0U; i < sizeof(needles) / sizeof(needles[0]); ++i) {
        if (strcasestr(line, needles[i]) != nullptr) {
            return true;
        }
    }
    return false;
}

static void ddial_client_echo_key(const char *text, char *out, size_t cap)
{
    /* Compare echoes loosely: case-folded alphanumerics only, because the
     * remote may trim, recode or re-space what we sent. */
    size_t n = 0U;
    for (const char *p = text; *p != '\0' && n + 1U < cap; ++p) {
        if (isalnum((unsigned char)*p)) {
            out[n++] = (char)tolower((unsigned char)*p);
        }
    }
    out[n] = '\0';
}

/* Must be called with client->lock held. */
static void ddial_client_remember_sent(ddial_client_t *client,
                                       const char *text)
{
    size_t slot = client->recent_sent_head % 8U;
    ddial_client_echo_key(text, client->recent_sent[slot],
                          sizeof(client->recent_sent[slot]));
    clock_gettime(CLOCK_MONOTONIC, &client->recent_sent_at[slot]);
    client->recent_sent_head = (client->recent_sent_head + 1U) % 8U;
}

/* Must be called with client->lock held.  Consumes the match. */
static bool ddial_client_is_recent_echo(ddial_client_t *client,
                                        const char *text)
{
    char key[256];
    ddial_client_echo_key(text, key, sizeof(key));
    if (key[0] == '\0') {
        return false;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (size_t i = 0U; i < 8U; ++i) {
        if (client->recent_sent[i][0] == '\0' ||
            now.tv_sec - client->recent_sent_at[i].tv_sec >
                DDIAL_CLIENT_ECHO_WINDOW_SEC) {
            continue;
        }
        if (strcmp(client->recent_sent[i], key) == 0) {
            client->recent_sent[i][0] = '\0';
            return true;
        }
    }
    return false;
}

/* The account lock file lives beside the other state files and holds
 * "active <epoch>" while connected or "closed <epoch>" after hang-up.  An
 * exclusive flock keeps a second ssh-chatter (restart overlap, a test run)
 * from dialing the same account. */
static bool ddial_client_account_lock(ddial_client_t *client, host_t *host)
{
    if (client->account_lock_fd >= 0) {
        return true;
    }
    const char *dir = getenv("CHATTER_DDIAL_LOCK_DIR");
    if (dir == nullptr || dir[0] == '\0') {
        dir = host->user_data_root[0] != '\0' ? host->user_data_root : "/tmp";
    }
    char name[300];
    size_t n = 0U;
    for (const char *p = client->host; *p != '\0' && n + 1U < 256U; ++p) {
        name[n++] = isalnum((unsigned char)*p) ? *p : '_';
    }
    name[n] = '\0';
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/ddial-account-%s-%d.lock", dir, name,
             client->port);
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        printf("[ddial] cannot open account lock %s: %s\n", path,
               strerror(errno));
        return false;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return false;
    }
    client->account_lock_fd = fd;
    return true;
}

static void ddial_client_account_mark(ddial_client_t *client, bool active)
{
    if (!client->user_mode || client->account_lock_fd < 0) {
        return;
    }
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%s %lld\n",
                       active ? "active" : "closed", (long long)time(nullptr));
    if (len > 0 && ftruncate(client->account_lock_fd, 0) == 0) {
        if (pwrite(client->account_lock_fd, buf, (size_t)len, 0) == len) {
            (void)fsync(client->account_lock_fd);
        }
    }
}

/* Seconds left before it is safe to dial in again. */
static unsigned int ddial_client_cooldown_remaining(ddial_client_t *client)
{
    if (!client->user_mode) {
        return 0U;
    }
    unsigned int cooldown = client->relogin_cooldown_sec;
    long since = -1;

    if (client->last_disconnect_time.tv_sec != 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        since = (long)(now.tv_sec - client->last_disconnect_time.tv_sec);
    }
    if (client->account_lock_fd >= 0) {
        char buf[64] = {0};
        ssize_t got = pread(client->account_lock_fd, buf, sizeof(buf) - 1U, 0);
        char state[16] = "";
        long long when = 0;
        if (got > 0 && sscanf(buf, "%15s %lld", state, &when) == 2) {
            if (strcmp(state, "active") == 0 && !client->connected) {
                /* We hold the lock, so the process that wrote "active" died
                 * while connected and the remote may still hold its
                 * session.  Start one full cooldown from now. */
                ddial_client_account_mark(client, false);
                when = (long long)time(nullptr);
            }
            long wall = (long)(time(nullptr) - (time_t)when);
            if (since < 0 || wall < since) {
                since = wall;
            }
        }
    }
    if (since < 0 || since >= (long)cooldown) {
        return 0U;
    }
    return cooldown - (unsigned int)since;
}

/* Say goodbye so the remote logs the account out instead of waiting for a
 * carrier timeout.  Must be called with client->lock held. */
static void ddial_client_graceful_quit_locked(ddial_client_t *client)
{
    if (client->user_mode && client->connected && client->upstream_fd >= 0 &&
        atomic_load(&client->auth_state) == DDIAL_AUTH_APPROVED) {
        (void)ddial_client_send_all(client->upstream_fd, "/Q\r\n", 4U);
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 300000000L};
        nanosleep(&pause, nullptr);
    }
}

/* Sleep in one-second steps so a stop request is honoured promptly. */
static void ddial_client_sleep_interruptible(ddial_client_t *client,
                                             unsigned int seconds)
{
    for (unsigned int i = 0U; i < seconds && !atomic_load(&client->stop);
         ++i) {
        sleep(1U);
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
        ddial_client_account_mark(client, false);
    }
    client->connected = false;
    client->auth_sent = false;
    atomic_store(&client->auth_state, DDIAL_AUTH_NONE);
    client->auth_result = 0;
    client->auth_deadline.tv_sec = 0;
    client->auth_deadline.tv_nsec = 0;
    client->recv_buf_len = 0U;
    client->telnet_state = DDIAL_TELNET_DATA;
    client->telnet_cmd = 0U;
    client->telnet_sb_opt = 0U;
    client->telnet_sb_len = 0U;
    client->last_send_time.tv_sec = 0;
    client->last_send_time.tv_nsec = 0;
    client->last_broadcast_time.tv_sec = 0;
    client->last_broadcast_time.tv_nsec = 0;
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
    ddial_client_account_mark(client, true);
    client->auth_sent = false;
    atomic_store(&client->auth_state, DDIAL_AUTH_NONE);
    client->auth_result = 0;
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

/* Normalize a raw DDial wire line to a clean form before stuffing it into
 * Chatter history.  Link-mode prefix bytes (}, }}, }}}) are parsed and the
 * payload is expanded (^ -> newline).  Terminal noise is stripped and
 * whitespace is trimmed.  The common hdcbbs.com variants (chat, station list,
 * linked station, node list) are preserved in canonical form. */
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

    /* Drop a "-->" prompt glued to the front of a chat line (the prompt has
     * no newline).  "-->. +^..." login notices are left alone. */
    while (strncmp(start, "-->", 3U) == 0) {
        char *after = start + 3;
        while (*after == ' ') {
            ++after;
        }
        if (*after != '#' && *after != '~' && !isdigit((unsigned char)*after)) {
            break;
        }
        start = after;
    }

    /* --- Link-mode prefix parsing ---
     * Lines received on a link connection begin with one or more '}' bytes
     * that indicate the message type:
     *   }   = member login/logoff event
     *   }}  = guest login/logoff event
     *   }}} = station broadcast (/SP list line)
     * The payload after the prefix may contain '^' as a newline substitute;
     * we expand those before storing. */
    if (start[0] == '}') {
        const char *rest = nullptr;
        ddial_link_msg_t kind = ddial_parse_link_prefix(start, &rest);

        /* Expand '^' -> ' ' for single-line display (newlines would break the
         * Chatter history format; use a space as a visual separator). */
        char expanded[SSH_CHATTER_MESSAGE_LIMIT];
        size_t exp_len = 0U;
        for (size_t i = 0U; rest[i] != '\0' && exp_len + 1U < sizeof(expanded); ++i) {
            expanded[exp_len++] = (rest[i] == '^') ? ' ' : rest[i];
        }
        expanded[exp_len] = '\0';

        /* Trim leading whitespace that often follows the prefix on the wire. */
        const char *payload = expanded;
        while (*payload != '\0' && isspace((unsigned char)*payload)) {
            ++payload;
        }

        switch (kind) {
        case DDIAL_LINK_MSG_MEMBER_EVENT:
            snprintf(dst, dst_cap, "[LINK] %s", payload);
            return;
        case DDIAL_LINK_MSG_GUEST_EVENT:
            snprintf(dst, dst_cap, "[LINK] %s", payload);
            return;
        case DDIAL_LINK_MSG_STATION_BROADCAST:
            snprintf(dst, dst_cap, "[SYSOP] %s", payload);
            return;
        default:
            break;
        }
    }

    /* Dual-channel link chat: a '~' prefix marks a message tuned to the
     * "other" channel; the remainder is a standard chat line. */
    if (start[0] == '~' && start[1] == '#') {
        snprintf(dst, dst_cap, "%s", start + 1);
        return;
    }

    /* Private message arriving over the link: "/P<slot> #<from>[T<ch>:<h>) msg".
     * Reformat to the local ddial display form "P#<from>[T<ch>:<h>) msg". */
    if (start[0] == '/' && (start[1] == 'P' || start[1] == 'p')) {
        if (start[2] == 'S' || start[2] == 's') {
            /* /PS station-to-station private message. */
            const char *msg = start + 3;
            while (*msg == ' ') {
                ++msg;
            }
            snprintf(dst, dst_cap, "[SYSOP] %s", msg);
            return;
        }
        uint16_t target_slot = 0U;
        uint16_t from_slot = 0U;
        uint8_t channel = DDIAL_DEFAULT_CHANNEL;
        char from_handle[DDIAL_MAX_HANDLE_LEN];
        const char *msg = nullptr;
        if (ddial_parse_incoming_private(start, &target_slot, &from_slot,
                                         &channel, from_handle,
                                         sizeof(from_handle), &msg)) {
            snprintf(dst, dst_cap, "P#%u[T%u:%s) %s",
                     (unsigned int)from_slot, (unsigned int)channel,
                     from_handle, msg);
            return;
        }
    }

    /* E-mail arriving over the link: "/E~SSSNNN(aaa:handle) msg". */
    if (start[0] == '/' && (start[1] == 'E' || start[1] == 'e') &&
        start[2] == '~') {
        unsigned from_station = 0U;
        unsigned from_id = 0U;
        char from_handle[DDIAL_MAX_HANDLE_LEN];
        const char *msg = nullptr;
        if (ddial_parse_incoming_email(start, &from_station, &from_id,
                                       from_handle, sizeof(from_handle),
                                       &msg)) {
            snprintf(dst, dst_cap, "[E-MAIL #%03u@%03u %s] %s", from_id,
                     from_station, from_handle, msg);
            return;
        }
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

/* Watch normalized inbound lines for login verdict markers while a password
 * login is pending.  Only used when a password (key) is configured; guest
 * logins never set auth_result.  Must be called with client->lock held. */
static void ddial_client_note_auth_marker(ddial_client_t *client,
                                          const char *line)
{
    if (client == nullptr || line == nullptr || client->key[0] == '\0' ||
        client->auth_result != 0) {
        return;
    }

    static const char *fail_needles[] = {
        /* NOTE: "PASSWORD needed for full access" is a Retro-Dial
         * informational banner, not a rejection -- never match on it. */
        "invalid password", "incorrect password",
        "wrong password",  "access denied",    "try again",
    };
    for (size_t i = 0U; i < sizeof(fail_needles) / sizeof(fail_needles[0]);
         ++i) {
        if (strcasestr(line, fail_needles[i]) != nullptr) {
            client->auth_result = -1;
            return;
        }
    }

    static const char *ok_needles[] = {
        "welcome to", "callers today", "callers total",
    };
    for (size_t i = 0U; i < sizeof(ok_needles) / sizeof(ok_needles[0]); ++i) {
        if (strcasestr(line, ok_needles[i]) != nullptr) {
            client->auth_result = 1;
            return;
        }
    }

    /* Member/guest login broadcast -- the '-->. +^...' payload arrives both as
     * a plain line (when we are a regular user) and with a '}' / '}}' prefix
     * (when we are operating as a link).  In both cases seeing '+^' means we
     * cleared authentication. */
    if (strstr(line, "+^") != nullptr || strstr(line, "->") != nullptr) {
        client->auth_result = 1;
    }
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

    if (ddial_parse_incoming_chat(line, nullptr, nullptr, nullptr, nullptr,
                                  nullptr, parsed_handle,
                                  sizeof(parsed_handle), &parsed_message)) {
        if (client->handle[0] != '\0' &&
            strcasecmp(parsed_handle, client->handle) == 0) {
            is_our_line = true;
        }
        if (client->user_mode) {
            ttak_mutex_lock(&client->lock);
            bool echo = ddial_client_is_recent_echo(client, parsed_message);
            ttak_mutex_unlock(&client->lock);
            if (is_our_line || echo) {
                /* The remote repeats what we typed as our account; the
                 * originals are already in the room and on the dial-ins. */
                return;
            }
        }
        if (!is_our_line &&
            host_ddial_handle_is_local(host, parsed_handle, false)) {
            /* Link echo of a local dial-in user's line: the Chatter room
             * already received it from the DDial listener. */
            return;
        }
    }

    if (!is_our_line) {
        host_ddial_relay_upstream_line(host, line);
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
static size_t ddial_client_extract_lines(host_t *host,
                                         ddial_client_t *client,
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
        bool chat_content = ddial_client_line_is_chat_content(normalized_line);
        bool lockout = !chat_content &&
                       ddial_client_line_looks_like_lockout(normalized_line);
        if (lockout && client->user_mode) {
            /* Dialing again would only make it worse (or disable the
             * account).  Stay down until an operator reconnects. */
            client->locked_out = true;
            printf("[ddial] upstream reports a duplicate/locked login (\"%s\"); "
                   "not reconnecting until /ddial reconnect\n",
                   normalized_line);
        }
        if (lockout ||
            (!chat_content && ddial_client_line_looks_like_kick(normalized_line))) {
            printf("[ddial] upstream hung up on us: \"%s\"\n",
                   normalized_line);
            if (client->upstream_fd >= 0) {
                close(client->upstream_fd);
                client->upstream_fd = -1;
                ddial_client_account_mark(client, false);
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

        if (!client->user_mode) {
            ddial_client_learn_slot(client, normalized_line);
        } else if (chat_content && client->handle[0] != '\0') {
            /* As a plain user every chat line starts with some "#N[", so
             * only our own echo tells us which line we are on. */
            char who[DDIAL_MAX_HANDLE_LEN];
            if (ddial_parse_incoming_chat(normalized_line, nullptr, nullptr,
                                          nullptr, nullptr, nullptr, who,
                                          sizeof(who), nullptr) &&
                strcasecmp(who, client->handle) == 0) {
                ddial_client_learn_slot(client, normalized_line);
            }
        }
        ddial_client_note_auth_marker(client, normalized_line);

        /* Route link private messages to the owning local -DT session. */
        uint16_t pm_target = 0U;
        uint16_t pm_from = 0U;
        uint8_t pm_channel = DDIAL_DEFAULT_CHANNEL;
        char pm_handle[DDIAL_MAX_HANDLE_LEN];
        const char *pm_msg = nullptr;
        if (host != nullptr &&
            ddial_parse_incoming_private(line, &pm_target, &pm_from,
                                         &pm_channel, pm_handle,
                                         sizeof(pm_handle), &pm_msg)) {
            char pm_display[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(pm_display, sizeof(pm_display), "P#%u[T%u:%s) %s",
                     (unsigned int)pm_from, (unsigned int)pm_channel,
                     pm_handle, pm_msg);
            host_ddial_deliver_private_line(host, pm_target, pm_display);
        }

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
                ddial_client_note_auth_marker(client, normalized_line);
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
    size_t count = ddial_client_extract_lines(host, client, lines,
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

    size_t count = ddial_client_extract_lines(host, client, lines,
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

/* After the password has been sent, watch inbound traffic until a login
 * verdict marker arrives, the server re-prints the password prompt (which
 * means the password was rejected), or the timeout expires.  Returns false
 * only on socket errors. */
static bool ddial_client_drain_until_auth_result(ddial_client_t *client,
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
        ttak_mutex_lock(&client->lock);
        int result = client->auth_result;
        bool reprompted =
            ddial_client_buffer_contains_ci_locked(client, "password");
        if (reprompted && result == 0) {
            client->auth_result = -1;
            result = -1;
        }
        ttak_mutex_unlock(&client->lock);
        if (result != 0) {
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

    if (client->key[0] != '\0') {
        /* Password login: wait a fixed 5 second grace period after CONNECT so
         * the upstream can finish telnet negotiation and print its password
         * prompt, then send the configured password. */
        if (!ddial_client_drain_for(client, host,
                                    DDIAL_CLIENT_AUTH_PRE_SEND_DELAY_MS)) {
            return false;
        }

        /* Drop the pending "Password:" prompt from the receive buffer so a
         * re-prompt after we send the password is recognized as a rejection. */
        char line[512];
        snprintf(line, sizeof(line), "%s\r\n", client->key);
        ttak_mutex_lock(&client->lock);
        client->recv_buf_len = 0U;
        client->auth_result = 0;
        if (client->connected && client->upstream_fd >= 0) {
            (void)ddial_client_send_all(client->upstream_fd, line,
                                        strlen(line));
            ddial_client_update_send_time(client);
        }
        ttak_mutex_unlock(&client->lock);

        /* Watch the post-auth traffic and confirm the login actually went
         * through.  A rejected password must never fall back to guest
         * login -- the connection is torn down instead. */
        if (!ddial_client_drain_until_auth_result(
                client, host, DDIAL_CLIENT_AUTH_RESULT_TIMEOUT_MS)) {
            return false;
        }

        ttak_mutex_lock(&client->lock);
        int auth_result = client->auth_result;
        ttak_mutex_unlock(&client->lock);
        if (auth_result < 0) {
            printf("[ddial] upstream %s:%d rejected the configured password; "
                   "disconnecting instead of guest login\n",
                   client->host, client->port);
            return false;
        }
        if (auth_result == 0) {
            /* Link accounts are message-in/message-out: after the password is
             * accepted the station often stays silent (no banner, no login
             * broadcast).  A wrong password re-prints the prompt, which the
             * drain loop already turned into a rejection.  Silence therefore
             * means the link is up. */
            printf("[ddial] upstream %s:%d sent no login confirmation; "
                   "assuming link access granted\n",
                   client->host, client->port);
        }
    } else {
        /* Guest login: wait for the password prompt when it arrives, then
         * send a bare RETURN.  On old/slow links this avoids racing input
         * ahead of Retro-Dial's CONNECT/password state. */
        if (!ddial_client_drain_until_login_prompt(
                client, host, DDIAL_CLIENT_PREAUTH_DRAIN_MS)) {
            return false;
        }

        ttak_mutex_lock(&client->lock);
        if (client->connected && client->upstream_fd >= 0) {
            (void)ddial_client_send_all(client->upstream_fd, "\r\n", 2U);
            ddial_client_update_send_time(client);
        }
        ttak_mutex_unlock(&client->lock);

        if (!ddial_client_drain_for(client, host,
                                    DDIAL_CLIENT_POSTAUTH_DRAIN_MS)) {
            return false;
        }
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

/* Send the }}} station user-list broadcast upstream roughly every
 * DDIAL_STATION_BROADCAST_INTERVAL_SEC seconds. */
static void ddial_client_maybe_station_broadcast(host_t *host,
                                                 ddial_client_t *client)
{
    if (host == nullptr || client == nullptr || client->upstream_fd < 0) {
        return;
    }
    if (atomic_load(&client->auth_state) != DDIAL_AUTH_APPROVED ||
        client->user_mode) {
        return;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return;
    }
    if (client->last_broadcast_time.tv_sec != 0 &&
        now.tv_sec - client->last_broadcast_time.tv_sec <
            (time_t)DDIAL_STATION_BROADCAST_INTERVAL_SEC) {
        return;
    }
    if (host_ddial_client_send_station_broadcast(host)) {
        client->last_broadcast_time = now;
    }
}

static void *ddial_client_thread(void *arg)
{
    host_t *host = (host_t *)arg;
    if (host == nullptr) {
        return nullptr;
    }

    ddial_client_t *client = (ddial_client_t *)&host->ddial_relay;

    SSHC_SAFE_BLOCK_BEGIN() {
        bool lock_warned = false;
        while (!atomic_load(&client->stop)) {
            if (!client->connected) {
                unsigned int backoff = ddial_client_backoff_sec(client);
                if (client->locked_out) {
                    ddial_client_sleep_interruptible(client, 5U);
                    continue;
                }
                if (client->user_mode) {
                    if (!ddial_client_account_lock(client, host)) {
                        if (!lock_warned) {
                            printf("[ddial] account %s:%d is held by another "
                                   "process; waiting\n",
                                   client->host, client->port);
                            lock_warned = true;
                        }
                        ddial_client_sleep_interruptible(client, 30U);
                        continue;
                    }
                    unsigned int wait = ddial_client_cooldown_remaining(client);
                    if (wait > 0U) {
                        printf("[ddial] waiting %us before dialing %s:%d again "
                               "(double-login guard)\n",
                               wait, client->host, client->port);
                        ddial_client_sleep_interruptible(client, wait);
                        continue;
                    }
                }
                if (!ddial_client_connect_socket(client)) {
                    ddial_client_sleep_interruptible(client, backoff);
                    continue;
                }
                if (!ddial_client_do_login(client, host)) {
                    ddial_client_disconnect(client);
                    ddial_client_sleep_interruptible(client, backoff);
                    continue;
                }
                /* Flush any banners/prompts accumulated during login.
                 * Extraction happens under client->lock; broadcasting does not. */
                ddial_client_flush_received_lines(host, client, false);
                /* Announce our current user list to the linked station. */
                client->last_broadcast_time.tv_sec = 0;
                client->last_broadcast_time.tv_nsec = 0;
                ddial_client_maybe_station_broadcast(host, client);
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
                ddial_client_maybe_station_broadcast(host, client);
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

    ttak_mutex_lock(&client->lock);
    ddial_client_graceful_quit_locked(client);
    ttak_mutex_unlock(&client->lock);
    ddial_client_disconnect(client);
    if (client->account_lock_fd >= 0) {
        close(client->account_lock_fd); /* releases the flock */
        client->account_lock_fd = -1;
    }
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
    const char *mode_env = getenv("CHATTER_DDIAL_MODE");
    const char *cooldown_env = getenv("CHATTER_DDIAL_RELOGIN_COOLDOWN");

    client->account_lock_fd = -1;
    client->user_mode =
        mode_env != nullptr && (strcasecmp(mode_env, "user") == 0 ||
                                strcasecmp(mode_env, "hijack") == 0);
    client->relogin_cooldown_sec = DDIAL_CLIENT_USER_RELOGIN_COOLDOWN_SEC;
    if (cooldown_env != nullptr && cooldown_env[0] != '\0') {
        client->relogin_cooldown_sec =
            (unsigned int)strtoul(cooldown_env, nullptr, 10);
    }

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
        printf("[ddial] upstream %s:%d configured in %s mode\n", client->host,
               client->port, client->user_mode ? "user" : "station");
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
    client->locked_out = false;
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
    host_ddial_client_send_channel(host, handle, DDIAL_DEFAULT_CHANNEL,
                                   message);
}

/* Link-mode public chat.  Channel 1 (the tuned link channel) goes out as
 * "#slot[T1:handle) msg"; channels 2-4 go out with the '~' dual-channel
 * prefix per the wire spec. */
void host_ddial_client_send_channel(host_t *host, const char *handle,
                                    uint8_t channel, const char *message)
{
    if (host == nullptr || message == nullptr) {
        return;
    }
    ddial_client_t *client = (ddial_client_t *)&host->ddial_relay;

    /* Snapshot connection state briefly, then format the wire message without
     * holding client->lock so inbound traffic can still be processed. */
    ttak_mutex_lock(&client->lock);
    bool enabled   = client->enabled;
    bool connected = client->connected;
    int  upstream_fd  = client->upstream_fd;
    ddial_auth_state_t auth_state = atomic_load(&client->auth_state);
    uint16_t our_slot = client->slot;
    bool     slot_known = client->slot_known;
    ttak_mutex_unlock(&client->lock);

    if (!enabled || !connected || upstream_fd < 0 ||
        auth_state != DDIAL_AUTH_APPROVED) {
        return;
    }

    /* Normalize text to 7-bit ASCII before putting it on the wire. */
    char clean_msg[SSH_CHATTER_MESSAGE_LIMIT];
    ddial_client_normalize_outbound_text(message, strlen(message),
                                         clean_msg, sizeof(clean_msg));
    if (clean_msg[0] == '\0') {
        return;
    }

    if (channel < DDIAL_MIN_CHANNEL) {
        channel = DDIAL_DEFAULT_CHANNEL;
    }
    if (channel > DDIAL_MAX_CHANNEL) {
        channel = DDIAL_MAX_CHANNEL;
    }

    char wire_line[SSH_CHATTER_MESSAGE_LIMIT + 64];
    int  wire_len;

    if (client->user_mode) {
        /* A plain user can only talk on the channel it is tuned to, and the
         * remote prefixes everything with our own "#N[T1:handle)".  Name
         * the real speaker inside the message instead. */
        if (channel != DDIAL_DEFAULT_CHANNEL) {
            return;
        }
        char text[SSH_CHATTER_MESSAGE_LIMIT];
        if (handle != nullptr && handle[0] != '\0' &&
            strcasecmp(handle, client->handle) != 0) {
            char clean_handle[DDIAL_MAX_HANDLE_LEN];
            ddial_sanitize_handle(handle, strlen(handle), clean_handle,
                                  sizeof(clean_handle));
            snprintf(text, sizeof(text), "%s: %s", clean_handle, clean_msg);
        } else {
            /* A leading '/' would be run as a command by the remote. */
            snprintf(text, sizeof(text), "%s%s",
                     clean_msg[0] == '/' ? " " : "", clean_msg);
        }
        char clean_body[DDIAL_MAX_BODY_LEN + 1U];
        ddial_clean_body(text, clean_body, sizeof(clean_body));
        wire_len = snprintf(wire_line, sizeof(wire_line), "%s\r\n",
                            clean_body);
        if (wire_len > 0 && (size_t)wire_len < sizeof(wire_line)) {
            ttak_mutex_lock(&client->lock);
            ddial_client_remember_sent(client, clean_body);
            ttak_mutex_unlock(&client->lock);
        }
    } else if (slot_known && our_slot > 0U && handle != nullptr &&
               handle[0] != '\0') {
        /* Link mode: send the full #slot[Tchan:handle) message wire form
         * (with '~' dual-channel prefix when tuned away from channel 1). */
        bool formatted;
        if (channel != DDIAL_DEFAULT_CHANNEL) {
            formatted = ddial_format_link_dual_chat(
                wire_line, sizeof(wire_line), our_slot, channel,
                DDIAL_TIER_PASSWORD, handle, clean_msg);
        } else {
            formatted = ddial_format_link_chat(
                wire_line, sizeof(wire_line), our_slot, channel,
                DDIAL_TIER_PASSWORD, handle, clean_msg);
        }
        wire_len = formatted ? (int)strlen(wire_line) : -1;
    } else if (handle != nullptr && handle[0] != '\0') {
        /* Slot not yet known: fall back to "handle) message" form. */
        char clean_handle[DDIAL_MAX_HANDLE_LEN];
        ddial_sanitize_handle(handle, strlen(handle), clean_handle,
                              sizeof(clean_handle));
        char clean_body[DDIAL_MAX_BODY_LEN + 1U];
        ddial_clean_body(clean_msg, clean_body, sizeof(clean_body));
        wire_len = snprintf(wire_line, sizeof(wire_line),
                            "%s) %s\r\n", clean_handle, clean_body);
    } else {
        char clean_body[DDIAL_MAX_BODY_LEN + 1U];
        ddial_clean_body(clean_msg, clean_body, sizeof(clean_body));
        wire_len = snprintf(wire_line, sizeof(wire_line),
                            "%s\r\n", clean_body);
    }

    if (wire_len <= 0 || (size_t)wire_len >= sizeof(wire_line)) {
        return;
    }

    /* Hold the lock only for the actual socket write and send-time update. */
    ttak_mutex_lock(&client->lock);
    if (client->enabled && client->connected &&
        client->upstream_fd == upstream_fd &&
        atomic_load(&client->auth_state) == DDIAL_AUTH_APPROVED) {
        (void)ddial_client_send_all(client->upstream_fd, wire_line,
                                    (size_t)wire_len);
        ddial_client_update_send_time(client);
    }
    ttak_mutex_unlock(&client->lock);
}

/* Send a /P private message to a remote slot over the link. */
void host_ddial_client_send_private(host_t *host, uint16_t target_slot,
                                    const char *our_handle,
                                    const char *message)
{
    if (host == nullptr || message == nullptr || target_slot == 0U) {
        return;
    }
    ddial_client_t *client = (ddial_client_t *)&host->ddial_relay;

    ttak_mutex_lock(&client->lock);
    bool enabled      = client->enabled;
    bool connected    = client->connected;
    int  upstream_fd  = client->upstream_fd;
    ddial_auth_state_t auth_state = atomic_load(&client->auth_state);
    uint16_t our_slot = client->slot;
    ttak_mutex_unlock(&client->lock);

    if (!enabled || !connected || upstream_fd < 0 ||
        auth_state != DDIAL_AUTH_APPROVED) {
        return;
    }

    char clean_msg[SSH_CHATTER_MESSAGE_LIMIT];
    ddial_client_normalize_outbound_text(message, strlen(message),
                                         clean_msg, sizeof(clean_msg));
    if (clean_msg[0] == '\0') {
        return;
    }

    char wire_line[SSH_CHATTER_MESSAGE_LIMIT + 64];
    if (client->user_mode) {
        /* Plain-user private message: "/P<line> <speaker>: <msg>". */
        char clean_body[DDIAL_MAX_BODY_LEN + 1U];
        char text[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(text, sizeof(text), "%s%s%s",
                 our_handle != nullptr ? our_handle : "",
                 our_handle != nullptr && our_handle[0] != '\0' ? ": " : "",
                 clean_msg);
        ddial_clean_body(text, clean_body, sizeof(clean_body));
        snprintf(wire_line, sizeof(wire_line), "/P%u %s\r\n",
                 (unsigned)target_slot, clean_body);
    } else if (!ddial_format_link_private(wire_line, sizeof(wire_line),
                                   target_slot, our_slot,
                                   DDIAL_DEFAULT_CHANNEL,
                                   DDIAL_TIER_PASSWORD,
                                   our_handle != nullptr ? our_handle : "",
                                   clean_msg)) {
        return;
    }

    ttak_mutex_lock(&client->lock);
    if (client->enabled && client->connected &&
        client->upstream_fd == upstream_fd &&
        atomic_load(&client->auth_state) == DDIAL_AUTH_APPROVED) {
        (void)ddial_client_send_all(client->upstream_fd, wire_line,
                                    strlen(wire_line));
        ddial_client_update_send_time(client);
    }
    ttak_mutex_unlock(&client->lock);
}

/* Announce a login/logout of one of our users to the linked station, using
 * the "}-->. +^#slot[T1:handle:#acct*" / "}}-->. -^#slot(T1:?" wire forms.
 * The slot/channel identify the user on *our* station. */
static void host_ddial_client_send_link_event(host_t *host, uint16_t slot,
                                              uint8_t channel,
                                              ddial_user_tier_t tier,
                                              const char *handle,
                                              uint16_t account, bool is_login)
{
    if (host == nullptr || handle == nullptr || handle[0] == '\0') {
        return;
    }
    ddial_client_t *client = (ddial_client_t *)&host->ddial_relay;

    ttak_mutex_lock(&client->lock);
    bool enabled      = client->enabled;
    bool connected    = client->connected;
    int  upstream_fd  = client->upstream_fd;
    ddial_auth_state_t auth_state = atomic_load(&client->auth_state);
    ttak_mutex_unlock(&client->lock);

    if (!enabled || !connected || upstream_fd < 0 ||
        auth_state != DDIAL_AUTH_APPROVED || client->user_mode) {
        return;
    }

    char wire_line[SSH_CHATTER_MESSAGE_LIMIT + 64];
    bool formatted = is_login
                         ? ddial_format_link_login(wire_line, sizeof(wire_line),
                                                   slot, channel, tier,
                                                   handle, account, false,
                                                   false)
                         : ddial_format_link_logout(wire_line, sizeof(wire_line),
                                                    slot, channel, tier,
                                                    handle, account, false,
                                                    false);
    if (!formatted) {
        return;
    }

    ttak_mutex_lock(&client->lock);
    if (client->enabled && client->connected &&
        client->upstream_fd == upstream_fd &&
        atomic_load(&client->auth_state) == DDIAL_AUTH_APPROVED) {
        (void)ddial_client_send_all(client->upstream_fd, wire_line,
                                    strlen(wire_line));
        ddial_client_update_send_time(client);
    }
    ttak_mutex_unlock(&client->lock);
}

void host_ddial_client_send_login(host_t *host, uint16_t slot,
                                  uint8_t channel, ddial_user_tier_t tier,
                                  const char *handle, uint16_t account)
{
    host_ddial_client_send_link_event(host, slot, channel, tier, handle,
                                      account, true);
}

void host_ddial_client_send_logout(host_t *host, uint16_t slot,
                                   uint8_t channel, ddial_user_tier_t tier,
                                   const char *handle, uint16_t account)
{
    host_ddial_client_send_link_event(host, slot, channel, tier, handle,
                                      account, false);
}

bool host_ddial_client_send_raw(host_t *host, const char *data,
                                size_t data_len)
{
    if (host == nullptr || data == nullptr || data_len == 0U) {
        return false;
    }

    ddial_client_t *client = (ddial_client_t *)&host->ddial_relay;
    ttak_mutex_lock(&client->lock);
    if (!client->enabled || !client->connected || client->upstream_fd < 0 ||
        atomic_load(&client->auth_state) != DDIAL_AUTH_APPROVED) {
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

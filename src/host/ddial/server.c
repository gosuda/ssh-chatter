/**
 * @file ddial_server.c
 * @desc Diversi Dial server listener for SSH-Chatter.
 *
 * Accepts raw TCP connections on the port specified by -DT and speaks
 * the Diversi Dial protocol (channels, /-commands, handle login).
 */

#include "ssh_chatter/ddial_protocol.h"
#include "ssh_chatter/host.h"
#include "ssh_chatter/memory_manager.h"


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

#define DDIAL_SESSION_INPUT_BUF 4096
#define DDIAL_SESSION_POLL_MS 100

/* Defined in integration.c (same translation unit). */
extern void host_ddial_write_who(struct ddial_session *target);

static void host_ddial_format_sockaddr(const struct sockaddr *addr,
                                       socklen_t len, char *buffer, size_t size)
{
    if (buffer == nullptr || size == 0U) {
        return;
    }
    buffer[0] = '\0';
    if (addr == nullptr) {
        return;
    }
    socklen_t host_len = (socklen_t)(size > (size_t)UINT_MAX ? UINT_MAX : size);
    if (getnameinfo(addr, len, buffer, host_len, nullptr, 0, NI_NUMERICHOST) !=
        0) {
        buffer[0] = '\0';
    }
}

typedef struct ddial_session {
    int fd;
    host_t *owner;
    uint64_t session_id;
    char client_ip[SSH_CHATTER_IP_LEN];
    char handle[DDIAL_MAX_HANDLE_LEN];
    uint8_t channel;
    bool logged_in;
    bool should_exit;
    char input_buf[DDIAL_SESSION_INPUT_BUF];
    size_t input_len;
    pthread_t thread;
    bool thread_initialized;
    ttak_mutex_t out_lock;
    bool out_lock_initialized;
    char out_buf[SSH_CHATTER_MESSAGE_LIMIT * 4];
    size_t out_len;
    struct timespec last_activity;
} ddial_session_t;

static void ddial_session_write_raw(ddial_session_t *sess, const char *data,
                                    size_t len)
{
    if (sess == nullptr || data == nullptr || len == 0U) {
        return;
    }
    ttak_mutex_lock(&sess->out_lock);
    size_t space = sizeof(sess->out_buf) - sess->out_len;
    if (len > space) {
        len = space;
    }
    if (len > 0U) {
        memcpy(sess->out_buf + sess->out_len, data, len);
        sess->out_len += len;
    }
    ttak_mutex_unlock(&sess->out_lock);
}

static void ddial_session_write_line(ddial_session_t *sess, const char *text)
{
    if (sess == nullptr || text == nullptr) {
        return;
    }
    char buf[SSH_CHATTER_MESSAGE_LIMIT + 4];
    int written = snprintf(buf, sizeof(buf), "%s\r\n", text);
    if (written > 0) {
        ddial_session_write_raw(sess, buf, (size_t)written);
    }
}

static void ddial_session_flush(ddial_session_t *sess)
{
    if (sess == nullptr || sess->fd < 0) {
        return;
    }
    ttak_mutex_lock(&sess->out_lock);
    size_t total = sess->out_len;
    size_t sent = 0U;
    while (sent < total) {
        ssize_t n = send(sess->fd, sess->out_buf + sent, total - sent,
                         MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        sent += (size_t)n;
    }
    if (sent > 0U && sess->out_len > sent) {
        memmove(sess->out_buf, sess->out_buf + sent, sess->out_len - sent);
    }
    sess->out_len -= sent;
    ttak_mutex_unlock(&sess->out_lock);
}

static void ddial_session_send_prompt(ddial_session_t *sess)
{
    if (sess == nullptr) {
        return;
    }
    char prompt[32];
    ddial_format_prompt(prompt, sizeof(prompt));
    ddial_session_write_raw(sess, prompt, strlen(prompt));
    ddial_session_flush(sess);
}

static void ddial_session_send_welcome(ddial_session_t *sess)
{
    if (sess == nullptr) {
        return;
    }
    ddial_session_write_line(sess, "Diversi Dial server powered by SSH-Chatter");
    ddial_session_write_line(
        sess, "Enter your handle (or key, then handle on next line):");
}

static bool ddial_session_set_handle(ddial_session_t *sess, const char *name)
{
    if (sess == nullptr || name == nullptr || name[0] == '\0') {
        return false;
    }
    size_t len = strlen(name);
    if (len >= sizeof(sess->handle)) {
        len = sizeof(sess->handle) - 1U;
    }
    memcpy(sess->handle, name, len);
    sess->handle[len] = '\0';
    return true;
}

static void ddial_session_do_who(ddial_session_t *sess)
{
    if (sess == nullptr || sess->owner == nullptr) {
        return;
    }
    ddial_session_write_line(sess, "Current users:");
    host_ddial_write_who(sess);
}

static void ddial_session_do_help(ddial_session_t *sess)
{
    if (sess == nullptr) {
        return;
    }
    ddial_session_write_line(sess, "Commands:");
    ddial_session_write_line(sess, "/C<message>  - chat to current channel");
    ddial_session_write_line(sess, "/P<user> <msg> - private message");
    ddial_session_write_line(sess, "/J<n>        - join channel 1-4");
    ddial_session_write_line(sess, "/W           - who's online");
    ddial_session_write_line(sess, "/Q           - quit");
}

static void ddial_session_process_line(ddial_session_t *sess, const char *line)
{
    if (sess == nullptr || line == nullptr) {
        return;
    }

    if (!sess->logged_in) {
        /* First non-empty line is treated as handle (or key if it looks like
         * one). For simplicity accept any non-empty text as handle. */
        if (ddial_session_set_handle(sess, line)) {
            sess->logged_in = true;
            sess->channel = DDIAL_DEFAULT_CHANNEL;
            char greeting[128];
            snprintf(greeting, sizeof(greeting),
                     "Welcome, %s. You are on channel %u.", sess->handle,
                     (unsigned int)sess->channel);
            ddial_session_write_line(sess, greeting);
        } else {
            ddial_session_write_line(sess, "Invalid handle. Try again.");
        }
        return;
    }

    ddial_parsed_message_t msg;
    ddial_command_t cmd = ddial_parse_command(line, strlen(line), &msg);

    switch (cmd) {
    case DDIAL_CMD_CHAT:
    case DDIAL_CMD_PRIVATE:
    case DDIAL_CMD_UNKNOWN:
        if (line[0] != '\0' && sess->owner != nullptr) {
            char formatted[SSH_CHATTER_MESSAGE_LIMIT];
            if (ddial_format_chat(formatted, sizeof(formatted), 1U,
                                  sess->channel, DDIAL_TIER_GUEST,
                                  sess->handle, line)) {
                host_ddial_broadcast_to_sessions(sess->owner, formatted);
            }
        }
        break;
    case DDIAL_CMD_JOIN:
        if (msg.channel >= DDIAL_MIN_CHANNEL &&
            msg.channel <= DDIAL_MAX_CHANNEL) {
            sess->channel = msg.channel;
            char note[64];
            snprintf(note, sizeof(note), "Joined channel %u.",
                     (unsigned int)sess->channel);
            ddial_session_write_line(sess, note);
        }
        break;
    case DDIAL_CMD_WHO:
        ddial_session_do_who(sess);
        break;
    case DDIAL_CMD_HELP:
        ddial_session_do_help(sess);
        break;
    case DDIAL_CMD_QUIT:
        sess->should_exit = true;
        break;
    case DDIAL_CMD_MAIL:
        ddial_session_write_line(
            sess, "Mail not yet implemented on DDial port.");
        break;
    case DDIAL_CMD_TIME:
    case DDIAL_CMD_USERS:
    case DDIAL_CMD_STATS:
    case DDIAL_CMD_NEWS:
    case DDIAL_CMD_INFO:
    case DDIAL_CMD_VERSION:
    case DDIAL_CMD_BELL:
    case DDIAL_CMD_DUPLEX:
    case DDIAL_CMD_LINK:
        ddial_session_write_line(sess, "Command acknowledged (stub).");
        break;
    default:
        ddial_session_write_line(
            sess, "Unknown command. Type /H for help.");
        break;
    }
}

static void ddial_session_process_input(ddial_session_t *sess)
{
    if (sess == nullptr) {
        return;
    }
    for (;;) {
        char *eol = nullptr;
        size_t eol_len = 0U;
        for (size_t i = 0U; i + 1U < sess->input_len; ++i) {
            if (sess->input_buf[i] == '\r' &&
                sess->input_buf[i + 1U] == '\n') {
                eol = &sess->input_buf[i];
                eol_len = 2U;
                break;
            }
        }
        if (eol == nullptr) {
            for (size_t i = 0U; i < sess->input_len; ++i) {
                if (sess->input_buf[i] == '\n') {
                    eol = &sess->input_buf[i];
                    eol_len = 1U;
                    break;
                }
            }
        }
        if (eol == nullptr) {
            break;
        }
        size_t line_len = (size_t)(eol - sess->input_buf);
        if (line_len >= DDIAL_MAX_LINE_LEN) {
            line_len = DDIAL_MAX_LINE_LEN - 1U;
        }
        char line[DDIAL_MAX_LINE_LEN];
        memcpy(line, sess->input_buf, line_len);
        line[line_len] = '\0';
        if (line_len > 0U && line[line_len - 1U] == '\r') {
            line[line_len - 1U] = '\0';
        }
        ddial_session_process_line(sess, line);
        size_t consumed = line_len + eol_len;
        memmove(sess->input_buf, sess->input_buf + consumed,
                sess->input_len - consumed);
        sess->input_len -= consumed;
    }
}

static void *ddial_session_thread(void *arg)
{
    ddial_session_t *sess = (ddial_session_t *)arg;
    if (sess == nullptr) {
        return nullptr;
    }

    SSHC_SAFE_BLOCK_BEGIN() {
        ddial_session_send_welcome(sess);
        ddial_session_send_prompt(sess);

        while (!sess->should_exit) {
            struct pollfd pfd;
            pfd.fd = sess->fd;
            pfd.events = POLLIN;
            pfd.revents = 0;

            int rc = poll(&pfd, 1, DDIAL_SESSION_POLL_MS);
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }

            ddial_session_flush(sess);

            if (rc == 0) {
                continue;
            }
            if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                break;
            }
            if ((pfd.revents & POLLIN) == 0) {
                continue;
            }

            char chunk[1024];
            ssize_t n = recv(sess->fd, chunk, sizeof(chunk), 0);
            if (n <= 0) {
                break;
            }

            char filtered[1024];
            size_t flen = ddial_filter_telnet_iac(chunk, (size_t)n, filtered,
                                                  sizeof(filtered));
            size_t space = sizeof(sess->input_buf) - sess->input_len;
            if (flen > space) {
                flen = space;
            }
            if (flen > 0U) {
                memcpy(sess->input_buf + sess->input_len, filtered, flen);
                sess->input_len += flen;
            }
            clock_gettime(CLOCK_MONOTONIC, &sess->last_activity);
            ddial_session_process_input(sess);
        }
    } SSHC_SAFE_BLOCK_END({
        printf("[ddial] SEGV/SIGBUS swallowed in ddial_session_thread for session_id=%lu\n",
               (unsigned long)sess->session_id);
    });

    sess->should_exit = true;
    if (sess->fd >= 0) {
        close(sess->fd);
        sess->fd = -1;
    }
    if (sess->out_lock_initialized) {
        ttak_mutex_destroy(&sess->out_lock);
        sess->out_lock_initialized = false;
    }
    host_ddial_unregister_session(sess);
    sshc_gc_free(sess);
    return nullptr;
}


static int host_ddial_open_socket(host_t *host)
{
    if (host == nullptr || host->ddial_listener.port[0] == '\0') {
        return -1;
    }

    char *endptr = nullptr;
    const char *parsed_port_str = host->ddial_listener.port;

    // Resolve environment variable if it starts with $ or contains letters
    if (parsed_port_str[0] == '$' || parsed_port_str[0] == '\\') {
        const char *var_name = parsed_port_str;
        while (*var_name == '$' || *var_name == '\\') {
            var_name++;
        }
        const char *env_val = getenv(var_name);
        if (env_val != nullptr && env_val[0] != '\0') {
            parsed_port_str = env_val;
        }
    } else if (parsed_port_str[0] >= 'a' && parsed_port_str[0] <= 'z') {
        const char *env_val = getenv(parsed_port_str);
        if (env_val != nullptr && env_val[0] != '\0') {
            parsed_port_str = env_val;
        }
    }

    long base_port = strtol(parsed_port_str, &endptr, 10);
    if (base_port <= 0 || base_port > 65535 || *endptr != '\0') {
        printf("[ddial] invalid port '%s', falling back to default port 2323\n", host->ddial_listener.port);
        base_port = 2323;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    const char *bind_addr = host->ddial_listener.bind_address[0] != '\0'
                                ? host->ddial_listener.bind_address
                                : nullptr;

    int fd = -1;
    long resolved_port = base_port;
    const long max_port = base_port + 100L;
    for (; resolved_port <= 65535L && resolved_port <= max_port;
         ++resolved_port) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%ld", resolved_port);

        struct addrinfo *result = nullptr;
        int rc = getaddrinfo(bind_addr, port_str, &hints, &result);
        if (rc != 0) {
            printf("[ddial] failed to resolve %s:%s (%s)\n",
                   bind_addr != nullptr ? bind_addr : "*", port_str,
                   gai_strerror(rc));
            continue;
        }

        bool any_bind_failed = false;
        for (struct addrinfo *ai = result; ai != nullptr; ai = ai->ai_next) {
            int candidate = socket(ai->ai_family, ai->ai_socktype,
                                   ai->ai_protocol);
            if (candidate < 0) {
                continue;
            }
            int enable = 1;
            setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, &enable,
                       sizeof(enable));
            if (bind(candidate, ai->ai_addr, ai->ai_addrlen) != 0) {
                const int bind_err = errno;
                close(candidate);
                if (bind_err == EADDRINUSE) {
                    any_bind_failed = true;
                }
                continue;
            }
            if (listen(candidate, 16) != 0) {
                close(candidate);
                continue;
            }
            fd = candidate;
            break;
        }
        freeaddrinfo(result);

        if (fd >= 0) {
            break;
        }
        if (!any_bind_failed) {
            /* bind failed for a reason other than the port being in use;
             * stop scanning. */
            break;
        }
    }

    if (fd < 0) {
        printf("[ddial] could not bind to any port in range %ld-%ld\n",
               base_port,
               (base_port + 100L > 65535L) ? 65535L : base_port + 100L);
        return -1;
    }

    if (resolved_port != base_port) {
        host->ddial_listener.port_auto_adjusted = true;
        printf("[ddial] port auto-adjusted from %s to %ld\n",
               host->ddial_listener.requested_port[0] != '\0'
                   ? host->ddial_listener.requested_port
                   : host->ddial_listener.port,
               resolved_port);
    }
    snprintf(host->ddial_listener.port, sizeof(host->ddial_listener.port),
             "%ld", resolved_port);
    return fd;
}

static void host_ddial_configure_client_socket(int client_fd)
{
    if (client_fd < 0) {
        return;
    }
    int enable = 1;
    (void)setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &enable,
                     sizeof(enable));
    (void)setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &enable,
                     sizeof(enable));
}

static void *host_ddial_listener_thread(void *arg)
{
    host_t *host = (host_t *)arg;
    if (host == nullptr) {
        return nullptr;
    }

    sshc_epoch_thread_enter();
    pthread_detach(pthread_self());
    atomic_store(&host->ddial_listener.running, true);

    SSHC_SAFE_BLOCK_BEGIN() {
        while (!atomic_load(&host->ddial_listener.stop) &&
               (host->shutdown_flag == nullptr || *host->shutdown_flag == 0)) {
            if (host->ddial_listener.fd < 0) {
                int fd = host_ddial_open_socket(host);
                if (fd < 0) {
                    struct timespec backoff = {.tv_sec = 1, .tv_nsec = 0};
                    host_sleep_uninterruptible(&backoff);
                    continue;
                }
                host->ddial_listener.fd = fd;
                const char *display_addr =
                    host->ddial_listener.bind_address[0] != '\0'
                        ? host->ddial_listener.bind_address
                        : "*";
                printf("[ddial] listening on %s:%s\n", display_addr,
                       host->ddial_listener.port);
            }

            struct sockaddr_storage addr;
            socklen_t addr_len = sizeof(addr);
            int client_fd =
                accept(host->ddial_listener.fd, (struct sockaddr *)&addr, &addr_len);
            if (client_fd < 0) {
                int err = errno;
                if (err == EINTR) {
                    continue;
                }
                if (atomic_load(&host->ddial_listener.stop) ||
                    (host->shutdown_flag != nullptr && *host->shutdown_flag != 0)) {
                    break;
                }
                char msg[256];
                snprintf(msg, sizeof(msg), "accept failed: %s", strerror(err));
                humanized_log_error("ddial", msg, err);
                struct timespec backoff = {.tv_sec = 0, .tv_nsec = 200000000L};
                host_sleep_uninterruptible(&backoff);
                continue;
            }

            if (atomic_load(&host->ddial_listener.stop) ||
                (host->shutdown_flag != nullptr && *host->shutdown_flag != 0)) {
                close(client_fd);
                break;
            }

            host_ddial_configure_client_socket(client_fd);

            char peer_address[NI_MAXHOST];
            host_ddial_format_sockaddr((struct sockaddr *)&addr, addr_len,
                                       peer_address, sizeof(peer_address));
            if (peer_address[0] == '\0') {
                snprintf(peer_address, sizeof(peer_address), "%s", "unknown");
            }
            printf("[ddial] accepted client from %s\n", peer_address);

            ddial_session_t *sess =
                (ddial_session_t *)sshc_gc_calloc(1U, sizeof(*sess));
            if (sess == nullptr) {
                humanized_log_error("ddial",
                                    "failed to allocate ddial session context",
                                    ENOMEM);
                close(client_fd);
                continue;
            }

            sess->fd = client_fd;
            sess->owner = host;
            sess->session_id = host_allocate_session_id(host);
            sess->channel = DDIAL_DEFAULT_CHANNEL;
            snprintf(sess->client_ip, sizeof(sess->client_ip), "%s", peer_address);
            if (ttak_mutex_init(&sess->out_lock) == 0) {
                sess->out_lock_initialized = true;
            }
            clock_gettime(CLOCK_MONOTONIC, &sess->last_activity);
            host_ddial_register_session(sess);

            pthread_t thread_id;
            if (pthread_create(&thread_id, nullptr, ddial_session_thread, sess) !=
                0) {
                humanized_log_error("ddial", "failed to spawn ddial session thread",
                                    errno);
                close(client_fd);
                sshc_gc_free(sess);
                continue;
            }
            sess->thread = thread_id;
            sess->thread_initialized = true;
            pthread_detach(thread_id);
        }
    } SSHC_SAFE_BLOCK_END({
        printf("[ddial] SEGV/SIGBUS swallowed in host_ddial_listener_thread\n");
    });

    int listener_fd = host->ddial_listener.fd;
    host->ddial_listener.fd = -1;
    if (listener_fd >= 0) {
        close(listener_fd);
    }
    atomic_store(&host->ddial_listener.running, false);
    sshc_epoch_thread_exit();
    return nullptr;
}


bool host_ddial_listener_start(host_t *host, const char *bind_addr,
                               const char *port)
{
    if (host == nullptr || port == nullptr || port[0] == '\0') {
        return false;
    }

    if (host->ddial_listener.thread_initialized) {
        const bool same_port =
            strncmp(host->ddial_listener.port, port,
                    sizeof(host->ddial_listener.port)) == 0;
        bool same_bind = false;
        if (bind_addr == nullptr || bind_addr[0] == '\0') {
            same_bind = host->ddial_listener.bind_address[0] == '\0';
        } else {
            same_bind = strncmp(host->ddial_listener.bind_address, bind_addr,
                                sizeof(host->ddial_listener.bind_address)) ==
                        0;
        }
        if (same_port && same_bind &&
            atomic_load(&host->ddial_listener.running)) {
            const char *display_addr =
                host->ddial_listener.bind_address[0] != '\0'
                    ? host->ddial_listener.bind_address
                    : "*";
            printf("[ddial] listener already active on %s:%s\n", display_addr,
                   host->ddial_listener.port);
            return true;
        }
        host_ddial_listener_stop(host);
    }

    if (bind_addr != nullptr && bind_addr[0] != '\0') {
        snprintf(host->ddial_listener.bind_address,
                 sizeof(host->ddial_listener.bind_address), "%s", bind_addr);
    } else {
        host->ddial_listener.bind_address[0] = '\0';
    }
    snprintf(host->ddial_listener.requested_port,
             sizeof(host->ddial_listener.requested_port), "%s", port);
    snprintf(host->ddial_listener.port, sizeof(host->ddial_listener.port), "%s",
             port);
    host->ddial_listener.port_auto_adjusted = false;
    host->ddial_listener.enabled = true;
    host->ddial_listener.fd = -1;
    host->ddial_listener.restart_attempts = 0U;
    host->ddial_listener.last_error_time.tv_sec = 0;
    host->ddial_listener.last_error_time.tv_nsec = 0L;
    atomic_store(&host->ddial_listener.stop, false);

    pthread_t thread;
    if (pthread_create(&thread, nullptr,
                       host_ddial_listener_thread, host) != 0) {
        humanized_log_error("ddial", "failed to start ddial listener", errno);
        host->ddial_listener.enabled = false;
        return false;
    }

    host->ddial_listener.thread_initialized = true;
    return true;
}

void host_ddial_listener_stop(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (!host->ddial_listener.thread_initialized) {
        host->ddial_listener.enabled = false;
        host->ddial_listener.fd = -1;
        host->ddial_listener.bind_address[0] = '\0';
        host->ddial_listener.port[0] = '\0';
        atomic_store(&host->ddial_listener.running, false);
        atomic_store(&host->ddial_listener.stop, false);
        return;
    }

    const char *display_addr =
        host->ddial_listener.bind_address[0] != '\0'
            ? host->ddial_listener.bind_address
            : "*";
    printf("[ddial] stopping listener on %s:%s\n", display_addr,
           host->ddial_listener.port);

    atomic_store(&host->ddial_listener.stop, true);
    if (host->ddial_listener.fd >= 0) {
        shutdown(host->ddial_listener.fd, SHUT_RDWR);
    }

    /* The listener thread is detached and epoch-registered.  It exits
     * on its own when it sees the stop flag or fd closure.  GC rotation
     * will reclaim host memory only after the thread has exited the epoch.
     * No pthread_join — joining a detached thread is undefined behaviour. */
    host->ddial_listener.thread_initialized = false;
    host->ddial_listener.enabled = false;
    atomic_store(&host->ddial_listener.running, false);
    atomic_store(&host->ddial_listener.stop, false);

    if (host->ddial_listener.fd >= 0) {
        close(host->ddial_listener.fd);
        host->ddial_listener.fd = -1;
    }

    host->ddial_listener.bind_address[0] = '\0';
    host->ddial_listener.port[0] = '\0';
}

void host_ddial_listener_broadcast(host_t *host, const char *text)
{
    if (host == nullptr || text == nullptr || text[0] == '\0') {
        return;
    }
    host_ddial_broadcast_to_sessions(host, text);
}

void host_ddial_notify_admin_if_port_adjusted(host_t *host, session_ctx_t *ctx)
{
    if (host == nullptr || ctx == nullptr) {
        return;
    }
    if (!host->ddial_listener.enabled || !host->ddial_listener.port_auto_adjusted) {
        return;
    }
    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        return;
    }
    char announcement[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(announcement, sizeof(announcement),
             "[DDial] listener port auto-adjusted from %s to %s",
             host->ddial_listener.requested_port[0] != '\0'
                 ? host->ddial_listener.requested_port
                 : "(unknown)",
             host->ddial_listener.port);
    chat_room_broadcast(&host->room, announcement, nullptr);
}

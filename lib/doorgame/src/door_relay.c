/**
 * @file door_relay.c
 * @desc TCP socket relay bridge for DOSBox nullmodem door games.
 *       Callback-based; no dependency on ssh-chatter internals.
 */

#define _GNU_SOURCE
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <libgen.h>
#include <limits.h>

#include "doorgame/doorgame.h"
#include "ini.h"

#ifndef nullptr
#define nullptr (void *)(NULL)
#endif

#define DOOR_RELAY_BUFFER_SIZE 4096
#define DOOR_RELAY_ACCEPT_TIMEOUT_MS 30000
#define DOOR_RELAY_POLL_TIMEOUT_MS 100
#define DOOR_RELAY_QUIT_BYTE 0x1D /* Ctrl-] */
#define DOOR_RELAY_MAX_RUNTIME_SECONDS 3600
#define DOORGAME_CHANNEL_TIMEOUT (-2)

static int door_relay_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * @desc Bind a TCP listener to 127.0.0.1 on the specified port.
 */
static int setup_door_listener(int port)
{
    if (port <= 0 || port > 65535) {
        errno = EINVAL;
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons((uint16_t)port),
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    if (listen(fd, 1) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    return fd;
}

static int door_relay_write_all(int fd, const void *data, size_t length)
{
    if (fd < 0 || data == nullptr) {
        errno = EINVAL;
        return -1;
    }

    const unsigned char *cursor = data;
    size_t remaining = length;

    while (remaining > 0U) {
        ssize_t wrote = write(fd, cursor, remaining);
        if (wrote < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct timespec nap = {
                    .tv_sec = 0,
                    .tv_nsec = 5L * 1000L * 1000L,
                };
                nanosleep(&nap, nullptr);
                continue;
            }
            return -1;
        }
        cursor += wrote;
        remaining -= (size_t)wrote;
    }

    return 0;
}

static int door_relay_accept(int listen_fd)
{
    struct sockaddr_in client_addr;
    socklen_t client_addrlen = sizeof(client_addr);

    struct pollfd accept_pfd = {
        .fd = listen_fd,
        .events = POLLIN,
        .revents = 0,
    };

    int accept_ready = poll(&accept_pfd, 1, DOOR_RELAY_ACCEPT_TIMEOUT_MS);
    if (accept_ready <= 0 || (accept_pfd.revents & POLLIN) == 0) {
        return -1;
    }

    return accept(listen_fd, (struct sockaddr *)&client_addr,
                  &client_addrlen);
}

static bool door_relay_session_loop(doorgame_session_t *s,
                                    const doorgame_session_ops_t *sops,
                                    const doorgame_host_ops_t *hops,
                                    int client_fd)
{
    if (s == nullptr || sops == nullptr || client_fd < 0) {
        return false;
    }

    if (door_relay_set_nonblocking(client_fd) < 0) {
        return false;
    }

    int nodelay = 1;
    (void)setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                     sizeof(nodelay));

    char buffer[DOOR_RELAY_BUFFER_SIZE];
    bool running = true;
    time_t started_at = time(nullptr);
    bool was_buffering =
        sops->get_buffering != nullptr ? sops->get_buffering(s) : true;

    sops->set_buffering(s, false);

    while (running) {
        struct pollfd pfd = {
            .fd = client_fd,
            .events = POLLIN,
            .revents = 0,
        };
        int poll_result = poll(&pfd, 1, DOOR_RELAY_POLL_TIMEOUT_MS);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (poll_result > 0 && (pfd.revents & POLLIN)) {
            ssize_t got = read(client_fd, buffer, sizeof(buffer));
            if (got < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    break;
                }
            } else if (got == 0) {
                break;
            } else {
                (void)sops->write_all(s, buffer, (size_t)got);
            }
        }

        if (poll_result > 0 &&
            (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            break;
        }

        int read_result = sops->read_poll(s, buffer, sizeof(buffer), 10);
        if (read_result == DOORGAME_CHANNEL_TIMEOUT) {
            /* nothing to forward */
        } else if (read_result <= 0) {
            break;
        } else {
            int escape_idx = -1;
            for (int idx = 0; idx < read_result; ++idx) {
                if ((unsigned char)buffer[idx] == DOOR_RELAY_QUIT_BYTE) {
                    escape_idx = idx;
                    break;
                }
            }
            int forward_len = (escape_idx >= 0) ? escape_idx : read_result;
            ssize_t cursor = 0;
            while (cursor < forward_len) {
                ssize_t wrote = write(client_fd, buffer + cursor,
                                      (size_t)(forward_len - cursor));
                if (wrote < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        struct timespec nap = {
                            .tv_sec = 0,
                            .tv_nsec = 1L * 1000L * 1000L,
                        };
                        nanosleep(&nap, nullptr);
                        continue;
                    }
                    forward_len = (int)cursor;
                    break;
                }
                cursor += wrote;
            }
            if (escape_idx >= 0) {
                sops->send_system_line(
                    s, "[door] escape sequence detected, ending door game.");
                break;
            }
        }

        if (started_at != (time_t)-1 &&
            time(nullptr) - started_at > DOOR_RELAY_MAX_RUNTIME_SECONDS) {
            sops->send_system_line(
                s, sops->localized(s, DOORGAME_MSG_MAX_RUNTIME));
            break;
        }

        if (hops != nullptr && hops->is_shutting_down != nullptr &&
            hops->is_shutting_down(nullptr)) {
            break;
        }
    }

    sops->set_buffering(s, was_buffering);
    return true;
}

static pid_t launch_dosbox(const char *conf_path)
{
    if (conf_path == nullptr || conf_path[0] == '\0') {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        return pid;
    }

    if (setsid() < 0) {
        _exit(127);
    }

    char conf_dir_copy[PATH_MAX];
    snprintf(conf_dir_copy, sizeof(conf_dir_copy), "%s", conf_path);
    const char *game_dir = dirname(conf_dir_copy);

    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    unsetenv("DISPLAY");

    if (game_dir != nullptr && game_dir[0] != '\0') {
        if (chdir(game_dir) != 0) {
            fprintf(stderr, "[door_relay] chdir(%s) failed: %s\n",
                    game_dir, strerror(errno));
        }
    }

    for (int fd = 3; fd < 1024; ++fd) {
        close(fd);
    }

    char *const argv[] = {
        (char *)"dosbox",
        (char *)"-conf",
        (char *)conf_path,
        (char *)"-exit",
        nullptr,
    };

    execvp("dosbox", argv);
    _exit(127);
}

#define ZMODEM_IO_CHUNK 4096
#define ZMODEM_POLL_TIMEOUT_MS 200

static bool door_relay_spawn_zmodem(int client_fd, char *const argv[],
                                    const char *working_dir,
                                    const char *label)
{
    (void)label;
    if (client_fd < 0 || argv == nullptr || argv[0] == nullptr) {
        return false;
    }

    int stdin_pipe[2];
    int stdout_pipe[2];
    if (pipe(stdin_pipe) != 0) {
        return false;
    }
    if (pipe(stdout_pipe) != 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        return false;
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return false;
    }

    if (pid == 0) {
        if (working_dir != nullptr) {
            if (chdir(working_dir) != 0) {
                fprintf(stderr, "[door_relay] chdir(%s) failed: %s\n",
                        working_dir, strerror(errno));
                _exit(127);
            }
        }
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stdout_pipe[1], STDERR_FILENO);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    bool stdin_open = true;
    bool stdout_open = true;

    while (stdin_open || stdout_open) {
        struct pollfd fds[2];
        nfds_t nfds = 0U;
        if (stdin_open) {
            fds[nfds++] =
                (struct pollfd){.fd = client_fd, .events = POLLIN};
        }
        if (stdout_open) {
            fds[nfds++] =
                (struct pollfd){.fd = stdout_pipe[0], .events = POLLIN};
        }

        int poll_result = poll(fds, nfds, ZMODEM_POLL_TIMEOUT_MS);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (poll_result == 0) {
            continue;
        }

        nfds_t index = 0U;
        if (stdin_open) {
            struct pollfd sock_pfd = fds[index++];
            if (sock_pfd.revents & POLLIN) {
                unsigned char buffer[ZMODEM_IO_CHUNK];
                ssize_t read_len = read(client_fd, buffer, sizeof(buffer));
                if (read_len <= 0) {
                    stdin_open = false;
                    shutdown(stdin_pipe[1], SHUT_WR);
                    close(stdin_pipe[1]);
                } else {
                    ssize_t written =
                        write(stdin_pipe[1], buffer, (size_t)read_len);
                    if (written != read_len) {
                        stdin_open = false;
                        shutdown(stdin_pipe[1], SHUT_WR);
                        close(stdin_pipe[1]);
                    }
                }
            } else if (sock_pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                stdin_open = false;
                shutdown(stdin_pipe[1], SHUT_WR);
                close(stdin_pipe[1]);
            }
        }

        if (stdout_open) {
            struct pollfd child_pfd = fds[index];
            if (child_pfd.revents & POLLIN) {
                unsigned char buffer[ZMODEM_IO_CHUNK];
                ssize_t read_len = read(stdout_pipe[0], buffer,
                                        sizeof(buffer));
                if (read_len <= 0) {
                    stdout_open = false;
                    close(stdout_pipe[0]);
                } else {
                    if (door_relay_write_all(client_fd, buffer,
                                             (size_t)read_len) < 0) {
                        stdout_open = false;
                        close(stdout_pipe[0]);
                    }
                }
            } else if (child_pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                stdout_open = false;
                close(stdout_pipe[0]);
            }
        }
    }

    if (stdin_open) {
        close(stdin_pipe[1]);
    }
    if (stdout_open) {
        close(stdout_pipe[0]);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return false;
    }

    return true;
}

static bool door_relay_parse_port_value(const char *value, int *port_out)
{
    if (value == nullptr || port_out == nullptr) {
        return false;
    }

    const char *bare = value;
    while (*bare == ' ' || *bare == '\t') {
        ++bare;
    }
    if (*bare >= '0' && *bare <= '9') {
        char *endptr = nullptr;
        long port = strtol(bare, &endptr, 10);
        while (*endptr == ' ' || *endptr == '\t') {
            ++endptr;
        }
        if (*endptr == '\0' && port > 0 && port <= 65535) {
            *port_out = (int)port;
            return true;
        }
    }

    const char *port_ptr = strcasestr(value, "port:");
    while (port_ptr != nullptr) {
        const bool token_start =
            port_ptr == value || port_ptr[-1] == ' ' || port_ptr[-1] == '\t';
        if (token_start) {
            const char *digits = port_ptr + 5;
            char *endptr = nullptr;
            long port = strtol(digits, &endptr, 10);
            if (endptr != digits && port > 0 && port <= 65535) {
                *port_out = (int)port;
                return true;
            }
        }
        port_ptr = strcasestr(port_ptr + 5, "port:");
    }

    const char *endpoint_ptr = strcasestr(value, "client:");
    if (endpoint_ptr == nullptr) {
        endpoint_ptr = strcasestr(value, "server:");
    }
    if (endpoint_ptr != nullptr &&
        (endpoint_ptr == value || endpoint_ptr[-1] == ' ' ||
         endpoint_ptr[-1] == '\t')) {
        const char *endpoint_end = endpoint_ptr;
        while (*endpoint_end != '\0' && *endpoint_end != ' ' &&
               *endpoint_end != '\t') {
            ++endpoint_end;
        }

        const char *last_colon = endpoint_end;
        while (last_colon > endpoint_ptr && last_colon[-1] != ':') {
            --last_colon;
        }
        if (last_colon > endpoint_ptr && last_colon < endpoint_end) {
            char *endptr = nullptr;
            long port = strtol(last_colon, &endptr, 10);
            if (endptr != last_colon && endptr <= endpoint_end && port > 0 &&
                port <= 65535) {
                *port_out = (int)port;
                return true;
            }
        }
    }

    return false;
}

static char *create_adjusted_dosbox_conf(const char *orig_conf, const char *actual_port, bool is_server)
{
    if (orig_conf == nullptr || orig_conf[0] == '\0' || actual_port == nullptr || actual_port[0] == '\0') {
        return nullptr;
    }

    FILE *in = fopen(orig_conf, "r");
    if (in == nullptr) {
        return nullptr;
    }

    char temp_path[] = "/tmp/ssh_chatter_dosbox_XXXXXX";
    int fd = mkstemp(temp_path);
    if (fd < 0) {
        fclose(in);
        return nullptr;
    }

    FILE *out = fdopen(fd, "w");
    if (out == nullptr) {
        close(fd);
        unlink(temp_path);
        fclose(in);
        return nullptr;
    }

    char line[1024];
    while (fgets(line, sizeof(line), in) != nullptr) {
        char *serial_ptr = strcasestr(line, "serial1");
        if (serial_ptr != nullptr) {
            char *eq = strchr(line, '=');
            if (eq != nullptr && eq > serial_ptr) {
                if (strcasestr(line, "nullmodem") != nullptr) {
                    char rxdelay[64] = {0};
                    char txdelay[64] = {0};
                    bool telnet_opt = false;
                    bool transparent_opt = false;

                    if (strcasestr(line, "telnet") != nullptr) {
                        telnet_opt = true;
                    }
                    if (strcasestr(line, "transparent") != nullptr) {
                        transparent_opt = true;
                    }
                    char *rx_ptr = strcasestr(line, "rxdelay:");
                    if (rx_ptr != nullptr) {
                        sscanf(rx_ptr, "rxdelay:%63s", rxdelay);
                    }
                    char *tx_ptr = strcasestr(line, "txdelay:");
                    if (tx_ptr != nullptr) {
                        sscanf(tx_ptr, "txdelay:%63s", txdelay);
                    }

                    fprintf(out, "serial1 = nullmodem");
                    if (!is_server) {
                        fprintf(out, " server:127.0.0.1");
                    }
                    fprintf(out, " port:%s", actual_port);
                    if (rxdelay[0] != '\0') {
                        fprintf(out, " rxdelay:%s", rxdelay);
                    }
                    if (txdelay[0] != '\0') {
                        fprintf(out, " txdelay:%s", txdelay);
                    }
                    if (telnet_opt) {
                        fprintf(out, " telnet");
                    }
                    if (transparent_opt) {
                        fprintf(out, " transparent");
                    }
                    fprintf(out, "\n");
                    continue;
                }
            }
        }
        fputs(line, out);
    }

    fclose(in);
    fclose(out);

    return strdup(temp_path);
}

typedef struct {
    int port;
    bool found;
    bool is_server;
} serial_parse_ctx_t;

static int door_relay_ini_handler(void* user, const char* section,
                                  const char* name, const char* value)
{
    serial_parse_ctx_t *ctx = (serial_parse_ctx_t*)user;
    if (strcasecmp(section, "serial") == 0 && strcasecmp(name, "serial1") == 0) {
        if (strcasestr(value, "nullmodem") != nullptr) {
            ctx->found = true;
            ctx->port = 23;
            ctx->is_server = true;

            if (strcasestr(value, "server:") != nullptr ||
                strcasestr(value, "client:") != nullptr) {
                ctx->is_server = false;
            }

            int parsed_port = 0;
            if (door_relay_parse_port_value(value, &parsed_port)) {
                ctx->port = parsed_port;
            }
        }
    }
    return 1;
}

static void write_door_sys(const char *game_dir, const char *username)
{
    if (game_dir == nullptr || game_dir[0] == '\0' || username == nullptr) {
        return;
    }

    char path_upper[PATH_MAX];
    char path_lower[PATH_MAX];
    snprintf(path_upper, sizeof(path_upper), "%s/DOOR.SYS", game_dir);
    snprintf(path_lower, sizeof(path_lower), "%s/door.sys", game_dir);

    FILE *f_upper = fopen(path_upper, "w");
    FILE *f_lower = fopen(path_lower, "w");
    FILE *files[2] = {f_upper, f_lower};

    for (int i = 0; i < 2; ++i) {
        FILE *f = files[i];
        if (f == nullptr) {
            continue;
        }

        fprintf(f, "COM1:\r\n");            // 1. COM Port
        fprintf(f, "115200\r\n");           // 2. Baud rate
        fprintf(f, "8\r\n");                // 3. Data bits
        fprintf(f, "1\r\n");                // 4. Node number
        fprintf(f, "3F8\r\n");              // 5. Port address
        fprintf(f, "Y\r\n");                // 6. Screen display
        fprintf(f, "Y\r\n");                // 7. Printer echo
        fprintf(f, "Y\r\n");                // 8. Page bell
        fprintf(f, "Y\r\n");                // 9. Caller alarm
        fprintf(f, "%s\r\n", username);      // 10. User full name
        fprintf(f, "Seoul, Korea\r\n");     // 11. Location
        fprintf(f, "555-1234\r\n");         // 12. Work phone
        fprintf(f, "555-5678\r\n");         // 13. Home phone
        fprintf(f, "PASSWORD\r\n");         // 14. Password
        fprintf(f, "100\r\n");              // 15. Security level
        fprintf(f, "1\r\n");                // 16. Logins
        fprintf(f, "06-15-26\r\n");         // 17. Last login date
        fprintf(f, "3600\r\n");             // 18. Seconds remaining
        fprintf(f, "60\r\n");               // 19. Minutes remaining
        fprintf(f, "GR\r\n");               // 20. Graphics mode (ANSI)
        fprintf(f, "24\r\n");               // 21. Page length
        fprintf(f, "Y\r\n");                // 22. User mode
        fprintf(f, "1\r\n");                // 23. Co-sysop command
        fprintf(f, "Y\r\n");                // 24. Sysop status
        fprintf(f, "1\r\n");                // 25. User number
        fprintf(f, "Z\r\n");                // 26. Protocol
        fprintf(f, "0\r\n");                // 27. Uploads
        fprintf(f, "0\r\n");                // 28. Downloads
        fprintf(f, "0\r\n");                // 29. Daily limit KB
        fprintf(f, "01-01-70\r\n");         // 30. Date of birth
        fprintf(f, "C:\\BBS\\DATA\r\n");    // 31. Database path
        fprintf(f, "None\r\n");             // 32. Memo
        fprintf(f, "Sysop\r\n");            // 33. Sysop name
        fprintf(f, "%s\r\n", username);      // 34. Alias
        fprintf(f, "00:00\r\n");            // 35. Event time
        fprintf(f, "Y\r\n");                // 36. Error correcting
        fprintf(f, "Y\r\n");                // 37. Session status
        fprintf(f, "21:30\r\n");            // 38. Connect time
        fprintf(f, "60\r\n");               // 39. Time remaining
        fprintf(f, "ANSI\r\n");             // 40. Emulation
        fprintf(f, "1\r\n");                // 41. Record number
        fprintf(f, "60\r\n");               // 42. Time limit
        fprintf(f, "0\r\n");                // 43. KB limit
        fprintf(f, "1\r\n");                // 44. Node count
        for (int line = 45; line <= 52; ++line) {
            fprintf(f, "0\r\n");
        }
        fclose(f);
    }
}

static void cleanup_door_sys(const char *game_dir)
{
    if (game_dir == nullptr || game_dir[0] == '\0') {
        return;
    }
    char path_upper[PATH_MAX];
    char path_lower[PATH_MAX];
    snprintf(path_upper, sizeof(path_upper), "%s/DOOR.SYS", game_dir);
    snprintf(path_lower, sizeof(path_lower), "%s/door.sys", game_dir);
    unlink(path_upper);
    unlink(path_lower);
}

bool doorgame_relay_run(doorgame_session_t *s, doorgame_host_t *h,
                        const doorgame_entry_t *entry,
                        const doorgame_session_ops_t *sops,
                        const doorgame_host_ops_t *hops)
{
    if (s == nullptr || entry == nullptr || entry->dosbox_conf[0] == '\0' ||
        sops == nullptr) {
        return false;
    }

    if (hops != nullptr && hops->max_sessions > 0U &&
        hops->active_sessions >= hops->max_sessions) {
        sops->send_system_line(s, sops->localized(s, DOORGAME_MSG_TOO_MANY));
        return false;
    }

    serial_parse_ctx_t parse_ctx = { .port = 0, .found = false, .is_server = false };
    if (ini_parse(entry->dosbox_conf, door_relay_ini_handler, &parse_ctx) < 0) {
        sops->send_system_line(s, "[door] failed to parse dosbox.conf.");
        return false;
    }
    if (!parse_ctx.found) {
        sops->send_system_line(s,
            "[door] serial1 port not found in dosbox.conf.");
        return false;
    }

    // Dynamic port adjustment to override hardcoded port:23 with the actual ddial port
    char *adjusted_conf = nullptr;
    const char *actual_port = (hops != nullptr && hops->get_ddial_port != nullptr) ? hops->get_ddial_port(h) : nullptr;
    if (actual_port != nullptr && actual_port[0] != '\0' && parse_ctx.port == 23) {
        int adjusted_port = 0;
        if (door_relay_parse_port_value(actual_port, &adjusted_port)) {
            adjusted_conf = create_adjusted_dosbox_conf(entry->dosbox_conf, actual_port, parse_ctx.is_server);
            if (adjusted_conf != nullptr) {
                parse_ctx.port = adjusted_port;
            }
        }
    }
    const char *conf_to_use = (adjusted_conf != nullptr) ? adjusted_conf : entry->dosbox_conf;

    // Generate dropfiles in the game directory so the game can read user info
    char game_dir[PATH_MAX] = {0};
    if (sops->get_username != nullptr) {
        const char *username = sops->get_username(s);
        if (username != nullptr && username[0] != '\0') {
            char conf_dir_copy[PATH_MAX];
            snprintf(conf_dir_copy, sizeof(conf_dir_copy), "%s", entry->dosbox_conf);
            char *dir = dirname(conf_dir_copy);
            if (dir != nullptr && dir[0] != '\0') {
                snprintf(game_dir, sizeof(game_dir), "%s", dir);
                write_door_sys(game_dir, username);
            }
        }
    }

    int listen_fd = -1;
    int client_fd = -1;

    if (!parse_ctx.is_server) {
        listen_fd = setup_door_listener(parse_ctx.port);
        if (listen_fd < 0) {
            sops->send_system_line(s,
                "[door] failed to bind relay listener.");
            if (adjusted_conf != nullptr) {
                unlink(adjusted_conf);
                free(adjusted_conf);
            }
            if (game_dir[0] != '\0') {
                cleanup_door_sys(game_dir);
            }
            return false;
        }
    }

    pid_t child_pid = launch_dosbox(conf_to_use);
    if (child_pid < 0) {
        if (listen_fd >= 0) {
            close(listen_fd);
        }
        if (adjusted_conf != nullptr) {
            unlink(adjusted_conf);
            free(adjusted_conf);
        }
        if (game_dir[0] != '\0') {
            cleanup_door_sys(game_dir);
        }
        sops->send_system_line(s,
            sops->localized(s, DOORGAME_MSG_FAILED_LAUNCH));
        return false;
    }

    if (hops != nullptr && hops->inc_active != nullptr) {
        hops->inc_active(h);
    }

    sops->send_system_line(s, sops->localized(s, DOORGAME_MSG_WAITING));

    if (!parse_ctx.is_server) {
        client_fd = door_relay_accept(listen_fd);
    } else {
        // Connect to DOSBox which is running as a nullmodem server
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
            .sin_port = htons((uint16_t)parse_ctx.port),
        };

        time_t start_time = time(nullptr);
        // Retry connection up to 10 seconds, check if child still runs
        while (time(nullptr) - start_time < 10) {
            int status = 0;
            if (waitpid(child_pid, &status, WNOHANG) == child_pid) {
                break;
            }

            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock >= 0) {
                int nodelay = 1;
                (void)setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
                if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                    client_fd = sock;
                    break;
                }
                close(sock);
            }

            struct timespec nap = {
                .tv_sec = 0,
                .tv_nsec = 100 * 1000 * 1000L, // 100ms
            };
            nanosleep(&nap, nullptr);
        }
    }

    if (client_fd < 0) {
        sops->send_system_line(s,
            sops->localized(s, DOORGAME_MSG_TIMEOUT));
        if (listen_fd >= 0) {
            close(listen_fd);
        }
        if (adjusted_conf != nullptr) {
            unlink(adjusted_conf);
            free(adjusted_conf);
        }
        kill(child_pid, SIGTERM);
        for (int i = 0; i < 20; ++i) {
            if (waitpid(child_pid, nullptr, WNOHANG) == child_pid) {
                break;
            }
            struct timespec nap = {
                .tv_sec = 0,
                .tv_nsec = 50 * 1000 * 1000L,
            };
            nanosleep(&nap, nullptr);
        }
        (void)waitpid(child_pid, nullptr, 0);
        if (game_dir[0] != '\0') {
            cleanup_door_sys(game_dir);
        }
        return false;
    }

    sops->send_system_line(s, sops->localized(s, DOORGAME_MSG_CONNECTED));

    (void)door_relay_session_loop(s, sops, hops, client_fd);

    if (client_fd >= 0) {
        close(client_fd);
    }
    if (listen_fd >= 0) {
        close(listen_fd);
    }

    for (int i = 0; i < 50; ++i) {
        int status = 0;
        pid_t reaped = waitpid(child_pid, &status, WNOHANG);
        if (reaped == child_pid) {
            break;
        }
        if (reaped < 0) {
            break;
        }
        struct timespec nap = {
            .tv_sec = 0,
            .tv_nsec = 100 * 1000 * 1000L,
        };
        nanosleep(&nap, nullptr);
    }
    (void)waitpid(child_pid, nullptr, 0);

    if (hops != nullptr && hops->dec_active != nullptr) {
        hops->dec_active(h);
    }

    if (adjusted_conf != nullptr) {
        unlink(adjusted_conf);
        free(adjusted_conf);
    }

    if (game_dir[0] != '\0') {
        cleanup_door_sys(game_dir);
    }

    sops->send_system_line(s, sops->localized(s, DOORGAME_MSG_SESSION_ENDED));
    return true;
}

bool door_relay_zmodem_receive(int client_fd, const char *working_dir)
{
    if (client_fd < 0 || working_dir == nullptr || working_dir[0] == '\0') {
        return false;
    }

    char *argv[] = {"rz", "-y", "-q", "--binary", "--escape", nullptr};
    return door_relay_spawn_zmodem(client_fd, argv, working_dir, "rz");
}

bool door_relay_zmodem_send(int client_fd, const char *file_path)
{
    if (client_fd < 0 || file_path == nullptr || file_path[0] == '\0') {
        return false;
    }

    char *argv[] = {"sz", "-q", "--binary", "--escape",
                    (char *)file_path, nullptr};
    return door_relay_spawn_zmodem(client_fd, argv, nullptr, "sz");
}

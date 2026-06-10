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

typedef struct {
    int port;
    bool found;
} serial_parse_ctx_t;

static int door_relay_ini_handler(void* user, const char* section,
                                  const char* name, const char* value)
{
    serial_parse_ctx_t *ctx = (serial_parse_ctx_t*)user;
    if (strcasecmp(section, "serial") == 0 && strcasecmp(name, "serial1") == 0) {
        const char *last_colon = strrchr(value, ':');
        if (last_colon != nullptr) {
            int port = atoi(last_colon + 1);
            if (port > 0 && port <= 65535) {
                ctx->port = port;
                ctx->found = true;
            }
        }
    }
    return 1;
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

    serial_parse_ctx_t parse_ctx = { .port = 0, .found = false };
    if (ini_parse(entry->dosbox_conf, door_relay_ini_handler, &parse_ctx) < 0) {
        sops->send_system_line(s, "[door] failed to parse dosbox.conf.");
        return false;
    }
    if (!parse_ctx.found) {
        sops->send_system_line(s,
            "[door] serial1 port not found in dosbox.conf.");
        return false;
    }

    int listen_fd = setup_door_listener(parse_ctx.port);
    if (listen_fd < 0) {
        sops->send_system_line(s,
            "[door] failed to bind relay listener.");
        return false;
    }

    pid_t child_pid = launch_dosbox(entry->dosbox_conf);
    if (child_pid < 0) {
        close(listen_fd);
        sops->send_system_line(s,
            sops->localized(s, DOORGAME_MSG_FAILED_LAUNCH));
        return false;
    }

    if (hops != nullptr && hops->inc_active != nullptr) {
        hops->inc_active(h);
    }

    sops->send_system_line(s, sops->localized(s, DOORGAME_MSG_WAITING));

    int client_fd = door_relay_accept(listen_fd);
    if (client_fd < 0) {
        sops->send_system_line(s,
            sops->localized(s, DOORGAME_MSG_TIMEOUT));
        close(listen_fd);
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
        return false;
    }

    sops->send_system_line(s, sops->localized(s, DOORGAME_MSG_CONNECTED));

    (void)door_relay_session_loop(s, sops, hops, client_fd);

    close(client_fd);
    close(listen_fd);

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

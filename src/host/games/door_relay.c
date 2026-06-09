/**
 * @file door_relay.c
 * @desc TCP socket relay bridge for DOSBox nullmodem door games.
 *       Binds an ephemeral loopback port, launches DOSBox configured to
 *       connect back via serial1=nullmodem, and bidirectionally relays
 *       character streams between a local fd and the accepted TCP socket
 *       using vanilla POSIX poll().
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

#include "ssh_chatter/host.h"

#ifndef nullptr
#define nullptr (void *)(NULL)
#endif

#define DOOR_RELAY_BUFFER_SIZE 4096
#define DOOR_RELAY_ACCEPT_TIMEOUT_MS 30000
#define DOOR_RELAY_POLL_TIMEOUT_MS 100

static int door_relay_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int door_relay_restore_flags(int fd, int original_flags)
{
    if (fd < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, original_flags);
}

/**
 * @desc Bind a TCP listener to 127.0.0.1 on an ephemeral port.
 * @param port  Output pointer receiving the allocated port number.
 * @return      Listening socket fd, or -1 on error (errno set).
 */
int setup_door_listener(int *port)
{
    if (port == nullptr) {
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
        .sin_port = 0,
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    socklen_t addrlen = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &addrlen) < 0) {
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

    *port = (int)ntohs(addr.sin_port);
    return fd;
}

/**
 * @desc Retry write() until the full buffer is sent or an unrecoverable
 *       error occurs.  Handles EINTR and transient EAGAIN / EWOULDBLOCK.
 * @return 0 on success, -1 on error (errno set).
 */
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

/**
 * @desc Accept a single DOSBox TCP connection and run a bidirectional
 *       relay between ssh_fd and the accepted socket.
 * @param ssh_fd     File descriptor for the terminal / SSH side.
 * @param listen_fd  Listening socket fd (created by setup_door_listener).
 */
void run_door_relay(int ssh_fd, int listen_fd)
{
    if (ssh_fd < 0 || listen_fd < 0) {
        return;
    }

    struct sockaddr_in client_addr;
    socklen_t client_addrlen = sizeof(client_addr);

    /* Wait for DOSBox to connect back. */
    struct pollfd accept_pfd = {
        .fd = listen_fd,
        .events = POLLIN,
        .revents = 0,
    };

    int accept_ready = poll(&accept_pfd, 1, DOOR_RELAY_ACCEPT_TIMEOUT_MS);
    if (accept_ready <= 0 || (accept_pfd.revents & POLLIN) == 0) {
        return;
    }

    int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr,
                           &client_addrlen);
    if (client_fd < 0) {
        return;
    }

    int ssh_flags = fcntl(ssh_fd, F_GETFL, 0);
    if (door_relay_set_nonblocking(ssh_fd) < 0 ||
        door_relay_set_nonblocking(client_fd) < 0) {
        close(client_fd);
        return;
    }

    /* Disable Nagle for low-latency character relay. */
    int nodelay = 1;
    (void)setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                     sizeof(nodelay));

    char buffer[DOOR_RELAY_BUFFER_SIZE];
    bool running = true;

    while (running) {
        struct pollfd pfds[2] = {
            {.fd = ssh_fd,    .events = POLLIN, .revents = 0},
            {.fd = client_fd, .events = POLLIN, .revents = 0},
        };

        int poll_result = poll(pfds, 2, DOOR_RELAY_POLL_TIMEOUT_MS);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        /* ssh_fd -> client_fd (user keystrokes to DOSBox serial input) */
        if (pfds[0].revents & POLLIN) {
            ssize_t got = read(ssh_fd, buffer, sizeof(buffer));
            if (got < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    break;
                }
            } else if (got == 0) {
                /* EOF — remote side closed. */
                break;
            } else {
                if (door_relay_write_all(client_fd, buffer, (size_t)got) < 0) {
                    break;
                }
            }
        }

        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            break;
        }

        /* client_fd -> ssh_fd (DOSBox ANSI output to user terminal) */
        if (pfds[1].revents & POLLIN) {
            ssize_t got = read(client_fd, buffer, sizeof(buffer));
            if (got < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    break;
                }
            } else if (got == 0) {
                /* EOF — DOSBox disconnected. */
                break;
            } else {
                if (door_relay_write_all(ssh_fd, buffer, (size_t)got) < 0) {
                    break;
                }
            }
        }

        if (pfds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            break;
        }
    }

    if (ssh_flags >= 0) {
        (void)door_relay_restore_flags(ssh_fd, ssh_flags);
    }
    close(client_fd);
}

/**
 * @desc Fork and execute DOSBox headlessly with the given configuration.
 *       The parent returns immediately; the caller is responsible for
 *       reaping the child process.
 * @param conf_path  Path to the dosbox.conf file.
 */
void launch_dosbox(const char *conf_path)
{
    if (conf_path == nullptr || conf_path[0] == '\0') {
        return;
    }

    pid_t pid = fork();
    if (pid != 0) {
        return;
    }

    if (setsid() < 0) {
        _exit(127);
    }

    /* Derive game directory from the conf path so relative mounts resolve. */
    char conf_dir_copy[PATH_MAX];
    snprintf(conf_dir_copy, sizeof(conf_dir_copy), "%s", conf_path);
    const char *game_dir = dirname(conf_dir_copy);

    /* Child: ensure headless operation. */
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    unsetenv("DISPLAY");

    if (game_dir != nullptr && game_dir[0] != '\0') {
        if (chdir(game_dir) != 0) {
            fprintf(stderr, "[door_relay] chdir(%s) failed: %s\n",
                    game_dir, strerror(errno));
        }
    }

    /* Close inherited descriptors so DOSBox cannot leak into chat sockets. */
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

/**
 * @desc Dynamically build a minimal dosbox.conf buffer for nullmodem
 *       serial relay.  Caller must free() the returned pointer.
 * @param port         The ephemeral TCP port DOSBox should connect to.
 * @param game_dir     Host path to mount as C: inside DOSBox.
 * @param game_binary  Command to run after mounting (e.g., "LORD.EXE").
 * @return             Newly-allocated configuration string, or nullptr.
 */
/**
 * @desc Inject or replace a [serial] section inside an existing dosbox.conf,
 *       write the result to a temporary file, and return its path.
 *       The caller must free() the returned pointer and unlink() the file.
 */
static char *door_relay_build_temp_conf(const char *original_conf, int port)
{
    FILE *in = fopen(original_conf, "r");
    if (in == nullptr) {
        return nullptr;
    }

    if (fseek(in, 0, SEEK_END) != 0) {
        fclose(in);
        return nullptr;
    }
    long size = ftell(in);
    if (size < 0 || fseek(in, 0, SEEK_SET) != 0) {
        fclose(in);
        return nullptr;
    }

    char *buf = nullptr;
    if (size > 0) {
        buf = (char *)malloc((size_t)size + 1U);
        if (buf == nullptr) {
            fclose(in);
            return nullptr;
        }
        size_t n = fread(buf, 1, (size_t)size, in);
        if (n != (size_t)size) {
            free(buf);
            fclose(in);
            return nullptr;
        }
        buf[size] = '\0';
    }
    fclose(in);

    char temp_path[PATH_MAX];
    snprintf(temp_path, sizeof(temp_path), "/tmp/door_relay_XXXXXX.conf");
    int fd = mkstemps(temp_path, 5);
    if (fd < 0) {
        free(buf);
        return nullptr;
    }

    FILE *out = fdopen(fd, "w");
    if (out == nullptr) {
        close(fd);
        unlink(temp_path);
        free(buf);
        return nullptr;
    }

    if (buf != nullptr) {
        char *serial_start = strstr(buf, "[serial]");
        if (serial_start != nullptr) {
            fwrite(buf, 1, (size_t)(serial_start - buf), out);
            fprintf(out, "[serial]\n");
            fprintf(out, "serial1=nullmodem client:127.0.0.1:%d\n", port);
            char *after_serial = serial_start + strlen("[serial]");
            char *next_section = strchr(after_serial, '[');
            if (next_section != nullptr) {
                fwrite(next_section, 1, strlen(next_section), out);
            }
        } else {
            fprintf(out, "[serial]\n");
            fprintf(out, "serial1=nullmodem client:127.0.0.1:%d\n\n", port);
            fprintf(out, "%s", buf);
        }
        free(buf);
    } else {
        fprintf(out, "[serial]\n");
        fprintf(out, "serial1=nullmodem client:127.0.0.1:%d\n", port);
    }

    fclose(out);
    return strdup(temp_path);
}

char *door_relay_build_dosbox_conf(int port, const char *game_dir,
                                   const char *game_binary)
{
    if (port <= 0 || port > 65535 || game_dir == nullptr ||
        game_dir[0] == '\0' || game_binary == nullptr ||
        game_binary[0] == '\0') {
        return nullptr;
    }

    static const char kTemplate[] =
        "[serial]\n"
        "serial1=nullmodem client:127.0.0.1:%d\n"
        "\n"
        "[autoexec]\n"
        "mount c \"%s\"\n"
        "c:\n"
        "%s\n";

    int need = snprintf(nullptr, 0, kTemplate, port, game_dir, game_binary);
    if (need < 0) {
        return nullptr;
    }

    size_t size = (size_t)need + 1U;
    char *buf = malloc(size);
    if (buf == nullptr) {
        return nullptr;
    }

    int wrote = snprintf(buf, size, kTemplate, port, game_dir, game_binary);
    if (wrote < 0 || wrote >= (int)size) {
        free(buf);
        return nullptr;
    }

    return buf;
}

#define ZMODEM_IO_CHUNK 4096
#define ZMODEM_POLL_TIMEOUT_MS 200

/**
 * @desc Internal helper: spawn an lrzsz helper (rz or sz) and bridge raw
 *       binary data between client_fd and the child process.
 * @param client_fd   Accepted TCP socket from DOSBox nullmodem.
 * @param argv        Null-terminated argv for execvp (e.g., {"rz", ...}).
 * @param working_dir Directory to chdir into before exec, or nullptr.
 * @param label       Human-readable label for error messages.
 * @return            true if the child exited successfully, false otherwise.
 */
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

/**
 * @desc Receive a file via ZMODEM over the TCP socket.
 *        Spawns `rz` and bridges between client_fd and the helper.
 * @param client_fd    Accepted socket from DOSBox nullmodem.
 * @param working_dir  Directory where the file will be saved.
 * @return             true on success, false on failure.
 */
/* Forward declaration from the BBS door subsystem. */
void session_send_system_line(session_ctx_t *ctx, const char *message);

/**
 * @desc High-level entry point: bind a loopback port, patch the user's
 *       dosbox.conf with a nullmodem serial section, launch DOSBox,
 *       and relay bytes between the SSH/Telnet fd and the DOSBox TCP
 *       connection.  Returns true if the relay ran, false on setup error.
 */
bool door_relay_run_session(session_ctx_t *ctx, const door_game_entry_t *entry)
{
    if (ctx == nullptr || entry == nullptr || entry->dosbox_conf[0] == '\0') {
        return false;
    }

    int port = 0;
    int listen_fd = setup_door_listener(&port);
    if (listen_fd < 0) {
        session_send_system_line(ctx,
            "[door] failed to bind relay listener.");
        return false;
    }

    char *temp_conf = door_relay_build_temp_conf(entry->dosbox_conf, port);
    if (temp_conf == nullptr) {
        close(listen_fd);
        session_send_system_line(ctx,
            "[door] failed to build temporary dosbox.conf.");
        return false;
    }

    launch_dosbox(temp_conf);

    int ssh_fd = -1;
    if (ctx->transport_kind == SESSION_TRANSPORT_SSH) {
        ssh_fd = ssh_get_fd(ctx->session);
    } else if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        ssh_fd = ctx->telnet_fd;
    }

    if (ssh_fd >= 0) {
        run_door_relay(ssh_fd, listen_fd);
    } else {
        session_send_system_line(ctx,
            "[door] unable to obtain transport fd for relay.");
    }

    close(listen_fd);
    unlink(temp_conf);
    free(temp_conf);

    /* Reap the DOSBox child launched by launch_dosbox. */
    for (int i = 0; i < 50; ++i) {
        int status = 0;
        pid_t reaped = waitpid(-1, &status, WNOHANG);
        if (reaped <= 0) {
            break;
        }
        if (reaped > 0 && !WIFEXITED(status) && !WIFSIGNALED(status)) {
            continue;
        }
        struct timespec nap = {
            .tv_sec = 0,
            .tv_nsec = 100 * 1000 * 1000L,
        };
        nanosleep(&nap, nullptr);
    }

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

/**
 * @desc Send a file via ZMODEM over the TCP socket.
 *        Spawns `sz` and bridges between client_fd and the helper.
 * @param client_fd  Accepted socket from DOSBox nullmodem.
 * @param file_path  Absolute path to the file to send.
 * @return           true on success, false on failure.
 */
bool door_relay_zmodem_send(int client_fd, const char *file_path)
{
    if (client_fd < 0 || file_path == nullptr || file_path[0] == '\0') {
        return false;
    }

    char *argv[] = {"sz", "-q", "--binary", "--escape",
                    (char *)file_path, nullptr};
    return door_relay_spawn_zmodem(client_fd, argv, nullptr, "sz");
}

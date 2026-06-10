/**
 * @file bbs_door.c
 * @desc BBS DOOR GAME runner (callback-based, no ssh-chatter internals).
 */

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <pty.h>
#include <iconv.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <libgen.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "doorgame/doorgame.h"

#ifdef DOORGAME_HAVE_UCHARDET
#include <uchardet.h>
#endif

#ifndef nullptr
#define nullptr (void *)(NULL)
#endif

#define DOORGAME_CHANNEL_TIMEOUT (-2)

static void trim_whitespace_inplace(char *str)
{
    if (str == nullptr) {
        return;
    }
    char *start = str;
    while (isspace((unsigned char)*start)) {
        ++start;
    }
    if (*start == '\0') {
        str[0] = '\0';
        return;
    }
    char *end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) {
        *end = '\0';
        --end;
    }
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

/* ---- Constants matching ssh-chatter defaults ---- */
#define DOORGAME_DOOR_IDLE_POLL_MS       100
#define DOORGAME_DOOR_MAX_RUNTIME_SECONDS 3600
#define DOORGAME_DOOR_BUFFER_SIZE        4096
#define DOORGAME_DOOR_QUIT_BYTE          0x1D /* Ctrl-] */
#define DOORGAME_DOOR_DETECT_BUFFER_BYTES 4096U
#define DOORGAME_MESSAGE_LIMIT           4096
#define DOORGAME_CHANNEL_TIMEOUT         (-2)


typedef struct host_door_runner {
    pid_t child_pid;
    int master_fd;
    time_t started_at;
    bool active;
    /* Encoding detection state for the dosbox stdout stream. We accumulate
     * up to DOORGAME_DOOR_DETECT_BUFFER_BYTES bytes before deciding which
     * Korean encoding (CP949 vs JOHAB vs UTF-8 vs none) the child is using;
     * once a decision is reached, every subsequent chunk is converted to
     * UTF-8 before being written to the SSH session. */
    unsigned char detect_buffer[DOORGAME_DOOR_DETECT_BUFFER_BYTES];
    size_t detect_length;
    bool encoding_decided;
    int detected_encoding; /* see DOOR_ENC_* below */
} host_door_runner_t;

enum {
    DOOR_ENC_UNKNOWN = 0,
    DOOR_ENC_PASSTHROUGH = 1,
    DOOR_ENC_UTF8 = 2,
    DOOR_ENC_CP949 = 3,
    DOOR_ENC_JOHAB = 4,
    DOOR_ENC_CP437 = 5,
};

/* ---- Encoding detection ----
 *
 * dosbox output is typically a mix of ANSI control bytes and either CP949,
 * JOHAB (older Korean DOS games), or already-UTF-8 (modern dosbox builds
 * with translation layers). We accumulate the first
 * DOORGAME_DOOR_DETECT_BUFFER_BYTES of stdout and run three independent
 * tests:
 *
 * 1) UTF-8 well-formedness (every multi-byte sequence's continuation bytes
 *    are 10xxxxxx, no overlong forms, no surrogate pairs in payload).
 * 2) CP949 plausibility (lead byte in 0x81–0xFE, trail in
 *    0x41–0x5A, 0x61–0x7A, 0x81–0xFE — the standard EUC-KR + extended
 *    range used in CP949).
 * 3) JOHAB plausibility (lead byte in 0x84–0xD3, trail in
 *    0x41–0x7E or 0x81–0xFE; high-bit pattern distinct from CP949).
 *
 * The decision rule:
 *   - If the buffer is fully 7-bit ASCII, mark PASSTHROUGH.
 *   - Else if the UTF-8 test passes and at least one multi-byte sequence is
 *     decoded validly, mark UTF8.
 *   - Else if the CP949 test passes more high-byte transitions than the
 *     JOHAB test, mark CP949; else if JOHAB has more, mark JOHAB.
 *   - Otherwise mark PASSTHROUGH (we don't guess wildly).
 */

static bool door_enc_byte_is_utf8_continuation(unsigned char b)
{
    return (b & 0xC0U) == 0x80U;
}

static int door_enc_score_utf8(const unsigned char *data, size_t len,
                               bool *all_ascii_out)
{
    int score = 0;
    bool all_ascii = true;
    size_t idx = 0U;
    while (idx < len) {
        unsigned char ch = data[idx];
        if (ch < 0x80U) {
            ++idx;
            continue;
        }
        all_ascii = false;
        size_t need = 0U;
        if ((ch & 0xE0U) == 0xC0U) {
            need = 1U;
        } else if ((ch & 0xF0U) == 0xE0U) {
            need = 2U;
        } else if ((ch & 0xF8U) == 0xF0U) {
            need = 3U;
        } else {
            return -1;
        }
        if (idx + need >= len) {
            /* Truncated tail — let it slide; will check next round. */
            break;
        }
        for (size_t k = 1U; k <= need; ++k) {
            if (!door_enc_byte_is_utf8_continuation(data[idx + k])) {
                return -1;
            }
        }
        score += (int)(need + 1U);
        idx += need + 1U;
    }
    if (all_ascii_out != nullptr) {
        *all_ascii_out = all_ascii;
    }
    return score;
}

static int door_enc_score_cp949(const unsigned char *data, size_t len)
{
    int score = 0;
    size_t idx = 0U;
    while (idx + 1U < len) {
        unsigned char lead = data[idx];
        if (lead < 0x80U) {
            ++idx;
            continue;
        }
        if (lead >= 0x81U && lead <= 0xFEU) {
            unsigned char trail = data[idx + 1U];
            bool trail_ok =
                (trail >= 0x41U && trail <= 0x5AU) ||
                (trail >= 0x61U && trail <= 0x7AU) ||
                (trail >= 0x81U && trail <= 0xFEU);
            if (trail_ok) {
                score += 2;
                idx += 2U;
                continue;
            }
        }
        --score; /* high byte that doesn't form a valid CP949 pair */
        ++idx;
    }
    return score;
}

static int door_enc_score_johab(const unsigned char *data, size_t len)
{
    int score = 0;
    size_t idx = 0U;
    while (idx + 1U < len) {
        unsigned char lead = data[idx];
        if (lead < 0x80U) {
            ++idx;
            continue;
        }
        if (lead >= 0x84U && lead <= 0xD3U) {
            unsigned char trail = data[idx + 1U];
            bool trail_ok =
                (trail >= 0x41U && trail <= 0x7EU) ||
                (trail >= 0x81U && trail <= 0xFEU);
            if (trail_ok) {
                score += 2;
                idx += 2U;
                continue;
            }
        }
        --score;
        ++idx;
    }
    return score;
}

static int door_enc_decide(const unsigned char *data, size_t len)
{
    if (len == 0U) {
        return DOOR_ENC_UNKNOWN;
    }

    bool all_ascii = false;
    int utf8_score = door_enc_score_utf8(data, len, &all_ascii);
    if (utf8_score >= 0 && all_ascii) {
        return DOOR_ENC_PASSTHROUGH;
    }
    if (utf8_score > 0) {
        return DOOR_ENC_UTF8;
    }

#ifdef DOORGAME_HAVE_UCHARDET
    uchardet_t ud = uchardet_new();
    if (ud != nullptr) {
        if (uchardet_handle_data(ud, (const char *)data, len) == 0) {
            uchardet_data_end(ud);
            const char *charset = uchardet_get_charset(ud);
            if (charset != nullptr && charset[0] != '\0') {
                int result = DOOR_ENC_UNKNOWN;
                if (strcasecmp(charset, "UTF-8") == 0) {
                    result = DOOR_ENC_UTF8;
                } else if (strncasecmp(charset, "EUC-KR", 6) == 0 ||
                           strncasecmp(charset, "CP949", 5) == 0 ||
                           strncasecmp(charset, "ISO-2022-KR", 11) == 0) {
                    result = DOOR_ENC_CP949;
                } else if (strncasecmp(charset, "JOHAB", 5) == 0) {
                    result = DOOR_ENC_JOHAB;
                } else if (strncasecmp(charset, "CP437", 5) == 0 ||
                           strncasecmp(charset, "IBM437", 6) == 0) {
                    result = DOOR_ENC_CP437;
                } else if (strncasecmp(charset, "ASCII", 5) == 0) {
                    result = DOOR_ENC_PASSTHROUGH;
                }
                if (result != DOOR_ENC_UNKNOWN) {
                    uchardet_delete(ud);
                    return result;
                }
            }
        }
        uchardet_delete(ud);
    }
#endif

    size_t high_byte_count = 0;
    for (size_t idx = 0; idx < len; ++idx) {
        if (data[idx] >= 0x80U) {
            high_byte_count++;
        }
    }

    int cp949_score = door_enc_score_cp949(data, len);
    int johab_score = door_enc_score_johab(data, len);

    if (cp949_score > 0 && (size_t)cp949_score > high_byte_count / 2 && cp949_score >= johab_score) {
        return DOOR_ENC_CP949;
    }
    if (johab_score > 0 && (size_t)johab_score > high_byte_count / 2 && johab_score > cp949_score) {
        return DOOR_ENC_JOHAB;
    }

    return DOOR_ENC_CP437;
}

static const char *door_enc_iconv_label(int encoding)
{
    switch (encoding) {
        case DOOR_ENC_CP949:
            return "CP949";
        case DOOR_ENC_JOHAB:
            return "JOHAB";
        case DOOR_ENC_UTF8:
            return "UTF-8";
        case DOOR_ENC_CP437:
            return "CP437";
        default:
            return nullptr;
    }
}

/* Convert `len` bytes of `src` from runner->detected_encoding into UTF-8 and
 * write to the SSH session via session_channel_write. Falls back to
 * passthrough on conversion failure. The function is best-effort: it never
 * returns an error to the caller, since the alternative is to drop the
 * door's output. */
static void door_emit_converted(doorgame_session_t *s,
                                const doorgame_session_ops_t *sops,
                                host_door_runner_t *runner,
                                const void *src, size_t len)
{
    if (s == nullptr || runner == nullptr || src == nullptr || len == 0U) {
        return;
    }

    if (runner->detected_encoding == DOOR_ENC_PASSTHROUGH ||
        runner->detected_encoding == DOOR_ENC_UTF8 ||
        runner->detected_encoding == DOOR_ENC_UNKNOWN) {
        (void)sops->write_all(s, src, len);
        return;
    }

    const char *label = door_enc_iconv_label(runner->detected_encoding);
    if (label == nullptr) {
        (void)sops->write_all(s, src, len);
        return;
    }

    iconv_t cd = iconv_open("UTF-8", label);
    if (cd == (iconv_t)-1) {
        (void)sops->write_all(s, src, len);
        return;
    }

    /* Output buffer roughly 4× input — Korean glyphs occupy 3 bytes in
     * UTF-8 so 2× CP949 → 3× UTF-8 worst case; ANSI pass-through is 1×.
     * Round up generously. */
    size_t out_capacity = len * 4U + 64U;
    char *out_buffer = (char *)sops->gc_malloc(out_capacity);
    if (out_buffer == nullptr) {
        iconv_close(cd);
        sops->write(s, src, len);
        return;
    }

    char *in_ptr = (char *)src; /* iconv signature requires non-const */
    size_t in_left = len;
    char *out_ptr = out_buffer;
    size_t out_left = out_capacity;

    while (in_left > 0U) {
        size_t rc = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
        if (rc == (size_t)-1) {
            if (errno == E2BIG) {
                /* grow output buffer */
                size_t produced = out_capacity - out_left;
                size_t new_capacity = out_capacity * 2U;
                char *grown = (char *)sops->gc_realloc(out_buffer, new_capacity);
                if (grown == nullptr) {
                    break;
                }
                out_buffer = grown;
                out_ptr = out_buffer + produced;
                out_left = new_capacity - produced;
                out_capacity = new_capacity;
                continue;
            }
            if (errno == EILSEQ || errno == EINVAL) {
                /* Skip the offending byte, emit a replacement, continue. */
                if (out_left >= 3U) {
                    out_ptr[0] = '\xEF';
                    out_ptr[1] = '\xBF';
                    out_ptr[2] = '\xBD';
                    out_ptr += 3;
                    out_left -= 3U;
                }
                if (in_left > 0U) {
                    ++in_ptr;
                    --in_left;
                }
                continue;
            }
            break;
        }
    }

    size_t produced = out_capacity - out_left;
    if (produced > 0U) {
        (void)sops->write_all(s, out_buffer, produced);
    }
    sops->gc_free(out_buffer);
    iconv_close(cd);
}

static void door_flush_detect_buffer(doorgame_session_t *s,
                                     const doorgame_session_ops_t *sops,
                                     host_door_runner_t *runner)
{
    if (s == nullptr || sops == nullptr || runner == nullptr || runner->detect_length == 0U) {
        return;
    }

    runner->detected_encoding =
        door_enc_decide(runner->detect_buffer, runner->detect_length);
    runner->encoding_decided = true;
    door_emit_converted(s, sops, runner, runner->detect_buffer,
                        runner->detect_length);
    runner->detect_length = 0U;
}

void doorgame_list(doorgame_session_t *s, doorgame_host_t *h,
                   const doorgame_session_ops_t *sops,
                   const doorgame_host_ops_t *hops)
{
    (void)h;
    if (s == nullptr || hops == nullptr) {
        return;
    }
    if (hops->entry_count == 0U) {
        sops->send_system_line(s, sops->localized(s, DOORGAME_MSG_NO_DOORS));
        return;
    }

    sops->send_system_line(s, sops->localized(s, DOORGAME_MSG_AVAILABLE));
    char buffer[DOORGAME_MESSAGE_LIMIT];
    for (size_t idx = 0U; idx < hops->entry_count; ++idx) {
        if (!hops->entries[idx].in_use) {
            continue;
        }
        const char *desc = hops->entries[idx].description;
        const char *locked_tag =
            hops->entries[idx].locked ? sops->localized(s, DOORGAME_MSG_LIST_LOCKED) : "";
        if (desc[0] == '\0') {
            snprintf(buffer, sizeof(buffer), "  %s%s",
                     hops->entries[idx].name, locked_tag);
        } else {
            snprintf(buffer, sizeof(buffer), "  %s%s — %s",
                     hops->entries[idx].name, locked_tag, desc);
        }
        sops->send_system_line(s, buffer);
    }
    sops->send_system_line(s, sops->localized(s, DOORGAME_MSG_LAUNCH_HINT));
}

static int doorgame_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void doorgame_terminate_child(host_door_runner_t *runner)
{
    if (runner == nullptr || !runner->active) {
        return;
    }
    if (runner->child_pid > 0) {
        kill(runner->child_pid, SIGTERM);
        for (int wait_idx = 0; wait_idx < 20; ++wait_idx) {
            int status = 0;
            pid_t reaped = waitpid(runner->child_pid, &status, WNOHANG);
            if (reaped == runner->child_pid || reaped < 0) {
                runner->child_pid = -1;
                break;
            }
            struct timespec naptime = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000L};
            nanosleep(&naptime, nullptr);
        }
        if (runner->child_pid > 0) {
            kill(runner->child_pid, SIGKILL);
            (void)waitpid(runner->child_pid, nullptr, 0);
            runner->child_pid = -1;
        }
    }
    if (runner->master_fd >= 0) {
        close(runner->master_fd);
        runner->master_fd = -1;
    }
    runner->active = false;
}

static bool doorgame_spawn(const char *dosbox_conf,
                                   host_door_runner_t *runner)
{
    if (dosbox_conf == nullptr || dosbox_conf[0] == '\0' || runner == nullptr) {
        return false;
    }

    struct stat conf_stat;
    if (stat(dosbox_conf, &conf_stat) != 0 || !S_ISREG(conf_stat.st_mode)) {
        return false;
    }

    int master_fd = -1;
    int slave_fd = -1;
    if (openpty(&master_fd, &slave_fd, nullptr, nullptr, nullptr) != 0) {
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(master_fd);
        close(slave_fd);
        return false;
    }

    char conf_dir_copy[PATH_MAX];
    snprintf(conf_dir_copy, sizeof(conf_dir_copy), "%s", dosbox_conf);
    const char *game_dir = dirname(conf_dir_copy);

    int debug_fd = open("/tmp/door_debug.log",
                        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);

    if (pid == 0) {
        /* Child: hook up PTY slave as stdio, drop libssh fds, exec dosbox. */
        close(master_fd);
        if (setsid() < 0) {
            _exit(127);
        }
        if (ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
            /* Non-fatal — continue without controlling terminal. */
        }
        if (dup2(slave_fd, STDIN_FILENO) < 0 ||
            dup2(slave_fd, STDOUT_FILENO) < 0 ||
            dup2(slave_fd, STDERR_FILENO) < 0) {
            _exit(127);
        }
        if (slave_fd > STDERR_FILENO) {
            close(slave_fd);
        }

        /* Redirect stderr to a persistent debug log so exec/dosbox errors
         * survive after the PTY is torn down. */
        if (debug_fd >= 0) {
            dup2(debug_fd, STDERR_FILENO);
            close(debug_fd);
        }

        /* Close any other inherited fds so dosbox cannot accidentally talk
         * to a chat socket. */
        for (int fd = STDERR_FILENO + 1; fd < 1024; ++fd) {
            close(fd);
        }

        /* Ensure dosbox runs in the directory where the conf and game files
         * live, so relative mounts / autoexec paths resolve correctly. */
        if (game_dir != nullptr && game_dir[0] != '\0') {
            if (chdir(game_dir) != 0) {
                fprintf(stderr, "[door] chdir(%s) failed: %s\n",
                        game_dir, strerror(errno));
            }
        }

        setenv("SDL_VIDEODRIVER", "dummy", 1);
        setenv("SDL_AUDIODRIVER", "dummy", 1);
        unsetenv("DISPLAY");

        const char *chosen_runner = getenv("CHATTER_DOOR_RUNNER");
        if (chosen_runner == nullptr || chosen_runner[0] == '\0') {
            chosen_runner = getenv("DOSBOX_RUNNER");
        }

        char resolved_runner[PATH_MAX];
        if (chosen_runner == nullptr || chosen_runner[0] == '\0') {
            chosen_runner = nullptr;
            const char *candidates[] = {"dosbox", "dosbox-x", "dosbox-staging"};
            const char *path_env = getenv("PATH");
            if (path_env != nullptr && path_env[0] != '\0') {
                char *path_copy = strdup(path_env);
                if (path_copy != nullptr) {
                    for (size_t i = 0U; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
                        char *saveptr = nullptr;
                        char *token = strtok_r(path_copy, ":", &saveptr);
                        bool found = false;
                        while (token != nullptr) {
                            char full_path[PATH_MAX];
                            snprintf(full_path, sizeof(full_path), "%s/%s", token, candidates[i]);
                            struct stat st;
                            if (stat(full_path, &st) == 0 && (st.st_mode & S_IXUSR) && !S_ISDIR(st.st_mode)) {
                                snprintf(resolved_runner, sizeof(resolved_runner), "%s", candidates[i]);
                                chosen_runner = resolved_runner;
                                found = true;
                                break;
                            }
                            token = strtok_r(nullptr, ":", &saveptr);
                        }
                        if (found) {
                            break;
                        }
                        strcpy(path_copy, path_env);
                    }
                    free(path_copy);
                }
            }
            if (chosen_runner == nullptr) {
                chosen_runner = "dosbox"; // default fallback
            }
        }

        char *const argv[] = {
            (char *)chosen_runner,
            (char *)"-conf",
            (char *)dosbox_conf,
            (char *)"-exit",
            nullptr,
        };
        execvp(chosen_runner, argv);
        _exit(127);
    }

    close(slave_fd);
    if (debug_fd >= 0) {
        close(debug_fd);
    }
    if (doorgame_set_nonblocking(master_fd) < 0) {
        kill(pid, SIGKILL);
        (void)waitpid(pid, nullptr, 0);
        close(master_fd);
        return false;
    }

    runner->child_pid = pid;
    runner->master_fd = master_fd;
    runner->started_at = time(nullptr);
    runner->active = true;
    return true;
}

static bool doorgame_io_loop(doorgame_session_t *s,
                                     const doorgame_session_ops_t *sops,
                                     const doorgame_host_ops_t *hops,
                                     doorgame_host_t *h,
                                     host_door_runner_t *runner)
{
    if (s == nullptr || runner == nullptr || !runner->active) {
        return false;
    }

    char buffer[DOORGAME_DOOR_BUFFER_SIZE];

    while (runner->active) {
        struct pollfd pfd = {
            .fd = runner->master_fd,
            .events = POLLIN,
            .revents = 0,
        };
        int poll_result = poll(&pfd, 1, DOORGAME_DOOR_IDLE_POLL_MS);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (poll_result > 0 && (pfd.revents & POLLIN)) {
            ssize_t got = read(runner->master_fd, buffer, sizeof(buffer));
            if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            }
            if (got == 0) {
                /* PTY master read 0: slave closed.  Do not assume the child
                 * is gone — DOSBox may have closed stdout briefly during
                 * startup.  Verify with waitpid before tearing down. */
                int verify_status = 0;
                pid_t reaped = waitpid(runner->child_pid, &verify_status,
                                       WNOHANG);
                if (reaped == runner->child_pid) {
                    runner->child_pid = -1;
                    door_flush_detect_buffer(s, sops, runner);
                    break;
                }
                /* Child still alive — give it a moment and keep polling. */
                struct timespec naptime = {
                    .tv_sec = 0,
                    .tv_nsec = 50 * 1000 * 1000L,
                };
                nanosleep(&naptime, nullptr);
                continue;
            }
            if (got > 0) {
                /* Encoding detection: pre-decision, accumulate into the
                 * detect buffer; once decided, every chunk goes through the
                 * converter. */
                if (!runner->encoding_decided) {
                    size_t previous_detect_length = runner->detect_length;
                    size_t room =
                        sizeof(runner->detect_buffer) - runner->detect_length;
                    size_t copy = ((size_t)got < room) ? (size_t)got : room;
                    if (copy > 0U) {
                        memcpy(runner->detect_buffer + runner->detect_length,
                               buffer, copy);
                        runner->detect_length += copy;
                    }
                    bool buffer_full = runner->detect_length ==
                                       sizeof(runner->detect_buffer);
                    bool seen_high_bit = false;
                    for (ssize_t k = 0; k < got; ++k) {
                        if ((unsigned char)buffer[k] >= 0x80U) {
                            seen_high_bit = true;
                            break;
                        }
                    }
                    if (!seen_high_bit && previous_detect_length == 0U) {
                        door_emit_converted(s, sops, runner, buffer, (size_t)got);
                        runner->detect_length = 0U;
                        continue;
                    }
                    bool decide_now =
                        buffer_full ||
                        (runner->detect_length >= 256U && seen_high_bit);
                    if (decide_now) {
                        runner->detected_encoding =
                            door_enc_decide(runner->detect_buffer,
                                            runner->detect_length);
                        runner->encoding_decided = true;
                        /* Flush the accumulated buffer through the converter
                         * in one shot so we don't lose pre-decision output. */
                        door_emit_converted(s, sops, runner, runner->detect_buffer,
                                            runner->detect_length);
                        runner->detect_length = 0U;
                    }
                } else {
                    door_emit_converted(s, sops, runner, buffer, (size_t)got);
                }
            }
        } else if (poll_result > 0 &&
                   (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            door_flush_detect_buffer(s, sops, runner);
            break;
        }

        /* Reap if the child has exited even when the PTY still has buffered
         * output to drain. */
        int status = 0;
        pid_t reaped = waitpid(runner->child_pid, &status, WNOHANG);
        if (reaped == runner->child_pid) {
            runner->child_pid = -1;
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                /* Drain any final output before returning. */
                for (;;) {
                    ssize_t residual =
                        read(runner->master_fd, buffer, sizeof(buffer));
                    if (residual <= 0) {
                        break;
                    }
                    if (!runner->encoding_decided) {
                        runner->detected_encoding =
                            door_enc_decide(runner->detect_buffer,
                                            runner->detect_length);
                        runner->encoding_decided = true;
                        if (runner->detect_length > 0U) {
                            door_emit_converted(s, sops, runner,
                                                runner->detect_buffer,
                                                runner->detect_length);
                            runner->detect_length = 0U;
                        }
                    }
                    door_emit_converted(s, sops, runner, buffer,
                                        (size_t)residual);
                }
                door_flush_detect_buffer(s, sops, runner);
                break;
            }
        }

        /* Optionally read from the user, with a small timeout so we keep
         * polling the master fd. */
        int read_result =
            sops->read_poll(s, buffer, sizeof(buffer), 50);
        if (read_result == DOORGAME_CHANNEL_TIMEOUT) {
            /* nothing to forward */
        } else if (read_result <= 0) {
            /* Session lost; tear down the door. */
            break;
        } else {
            /* Look for the escape byte (Ctrl-]). */
            int escape_idx = -1;
            for (int idx = 0; idx < read_result; ++idx) {
                if ((unsigned char)buffer[idx] == DOORGAME_DOOR_QUIT_BYTE) {
                    escape_idx = idx;
                    break;
                }
            }
            int forward_len = (escape_idx >= 0) ? escape_idx : read_result;
            ssize_t cursor = 0;
            while (cursor < forward_len) {
                ssize_t wrote = write(runner->master_fd, buffer + cursor,
                                      (size_t)(forward_len - cursor));
                if (wrote < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        struct timespec naptime = {
                            .tv_sec = 0,
                            .tv_nsec = 5 * 1000 * 1000L,
                        };
                        nanosleep(&naptime, nullptr);
                        continue;
                    }
                    forward_len = (int)cursor; /* stop forwarding */
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

        /* Hard runtime cap. */
        if (runner->started_at != (time_t)-1 &&
            time(nullptr) - runner->started_at >
                DOORGAME_DOOR_MAX_RUNTIME_SECONDS) {
            sops->send_system_line(
                s, sops->localized(s, DOORGAME_MSG_MAX_RUNTIME));
            break;
        }

        /* Honour a host shutdown signal so we don't pin the daemon. */
        if (hops != nullptr && hops->is_shutting_down != nullptr &&
            hops->is_shutting_down(h) != 0) {
            break;
        }
    }

    doorgame_terminate_child(runner);
    return true;
}

bool doorgame_run(doorgame_session_t *s, doorgame_host_t *h,
                  const doorgame_entry_t *entry,
                  const doorgame_session_ops_t *sops,
                  const doorgame_host_ops_t *hops)
{
    if (s == nullptr || entry == nullptr || sops == nullptr) {
        return false;
    }

    if (entry->locked && !sops->is_operator(s) && !sops->is_lan_operator(s)) {
        sops->send_system_line(
            s,
            "[door] This game is currently locked by the operator.");
        return false;
    }

    if (hops != nullptr && hops->max_sessions > 0U &&
        hops->active_sessions >= hops->max_sessions) {
        sops->send_system_line(
            s,
            "[door] too many active door sessions. Try again later.");
        return false;
    }

    /* Default to TCP-nullmodem relay path. PTY path can be forced back. */
    if (getenv("CHATTER_DOOR_USE_PTY") == nullptr) {
        extern bool doorgame_relay_run(doorgame_session_t *, doorgame_host_t *,
                                       const doorgame_entry_t *,
                                       const doorgame_session_ops_t *,
                                       const doorgame_host_ops_t *);
        return doorgame_relay_run(s, h, entry, sops, hops);
    }

    char status[DOORGAME_MESSAGE_LIMIT];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    snprintf(status, sizeof(status),
             sops->localized(s, DOORGAME_MSG_LAUNCHING),
             entry->name, DOORGAME_DOOR_MAX_RUNTIME_SECONDS);
#pragma GCC diagnostic pop
    sops->send_system_line(s, status);

    host_door_runner_t runner = {
        .child_pid = -1,
        .master_fd = -1,
        .started_at = 0,
        .active = false,
        .detect_length = 0U,
        .encoding_decided = false,
        .detected_encoding = DOOR_ENC_UNKNOWN,
    };

    bool was_buffering = true;
    sops->set_buffering(s, false);

    if (hops != nullptr && hops->inc_active != nullptr) {
        hops->inc_active(h);
    }

    if (!doorgame_spawn(entry->dosbox_conf, &runner)) {
        sops->set_buffering(s, was_buffering);
        if (hops != nullptr && hops->dec_active != nullptr) {
            hops->dec_active(h);
        }
        sops->send_system_line(s, sops->localized(s, DOORGAME_MSG_FAILED_LAUNCH));
        return false;
    }

    (void)doorgame_io_loop(s, sops, hops, h, &runner);

    sops->set_buffering(s, was_buffering);
    if (hops != nullptr && hops->dec_active != nullptr) {
        hops->dec_active(h);
    }

    time_t elapsed = time(nullptr) - runner.started_at;
    if (elapsed <= 2) {
        sops->send_system_line(s, sops->localized(s, DOORGAME_MSG_ENDED_IMMEDIATELY));
    } else {
        sops->send_system_line(s, sops->localized(s, DOORGAME_MSG_SESSION_ENDED));
    }
    return true;
}

bool doorgame_toggle_lock(doorgame_session_t *s, doorgame_host_t *h,
                          const char *name, bool is_operator,
                          const doorgame_session_ops_t *sops,
                          const doorgame_host_ops_t *hops)
{
    if (s == nullptr || hops == nullptr) {
        return false;
    }

    if (!is_operator) {
        sops->send_system_line(
            s, sops->localized(s, DOORGAME_MSG_ONLY_OPS_LOCK));
        return false;
    }

    if (name == nullptr || name[0] == '\0') {
        sops->send_system_line(s, sops->localized(s, DOORGAME_MSG_SETGAMELOCK_USAGE));
        return false;
    }

    char working[DOORGAME_NAME_LEN];
    snprintf(working, sizeof(working), "%s", name);
    trim_whitespace_inplace(working);

    doorgame_entry_t *target = nullptr;
    for (size_t i = 0U; i < hops->entry_count; ++i) {
        if (hops->entries[i].in_use &&
            strcasecmp(hops->entries[i].name, working) == 0) {
            target = &hops->entries[i];
            break;
        }
    }

    if (target == nullptr) {
        char msg[DOORGAME_MESSAGE_LIMIT];
        snprintf(msg, sizeof(msg),
                 "Unknown door game '%s'. Try `/bbs door` for the list.",
                 working);
        sops->send_system_line(s, msg);
        return false;
    }

    target->locked = !target->locked;

    if (hops->save_locks != nullptr) {
        hops->save_locks(h);
    }

    char msg[DOORGAME_MESSAGE_LIMIT];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    snprintf(msg, sizeof(msg),
             sops->localized(s, target->locked ? DOORGAME_MSG_NOW_LOCKED
                                                : DOORGAME_MSG_NOW_UNLOCKED),
             target->name);
#pragma GCC diagnostic pop
    sops->send_system_line(s, msg);
    return true;
}

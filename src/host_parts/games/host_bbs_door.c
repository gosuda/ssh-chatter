/**
 * @file host_bbs_door.c
 * @desc BBS DOOR GAME runner. Spawns a configured dosbox session and proxies
 *       stdin/stdout between the chat session and the child PTY. Doors are
 *       declared at startup via env vars `CHATTER_DOOR_<N>=name:conf[:desc]`.
 *
 *       Operator-only by default. The launch path uses execvp directly with a
 *       fixed argv — there is no shell layer and no user-controlled string
 *       reaches a shell, so users cannot inject extra arguments by naming a
 *       door creatively.
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

/* Forward declarations of helpers from sibling translation units in the
 * same aggregated TU. */
static int session_channel_read_poll(session_ctx_t *ctx, char *buffer,
                                     size_t length, int timeout_ms);
void session_channel_write(session_ctx_t *ctx, const void *data,
                           size_t length);
void session_send_system_line(session_ctx_t *ctx, const char *message);

#define SSH_CHATTER_DOOR_IDLE_POLL_MS 100
#define SSH_CHATTER_DOOR_MAX_RUNTIME_SECONDS 3600
#define SSH_CHATTER_DOOR_BUFFER_SIZE 4096
#define SSH_CHATTER_DOOR_QUIT_BYTE 0x1D /* Ctrl-] — escape from door. */
#define SSH_CHATTER_DOOR_DETECT_BUFFER_BYTES 4096U

typedef struct host_door_runner {
    pid_t child_pid;
    int master_fd;
    time_t started_at;
    bool active;
    /* Encoding detection state for the dosbox stdout stream. We accumulate
     * up to SSH_CHATTER_DOOR_DETECT_BUFFER_BYTES bytes before deciding which
     * Korean encoding (CP949 vs JOHAB vs UTF-8 vs none) the child is using;
     * once a decision is reached, every subsequent chunk is converted to
     * UTF-8 before being written to the SSH session.
     *
     * The 4 KiB scratch buffer is allocated from the host's resource manager
     * (ttak abstract backing) at spawn time and freed the moment the
     * encoding is decided — not at runner teardown — so the bytes are not
     * pinned for the rest of the door session. */
    sshc_resource_manager_t *rm;
    ttak_abstract_mem_t *detect_handle;
    ttak_abstract_map_t detect_map;
    unsigned char *detect_buffer; /* alias of detect_map.data while mapped */
    size_t detect_length;
    bool encoding_decided;
    int detected_encoding; /* see DOOR_ENC_* below */
} host_door_runner_t;

static bool door_runner_detect_acquire(host_door_runner_t *runner,
                                       sshc_resource_manager_t *rm)
{
    if (runner == nullptr) {
        return false;
    }
    runner->rm = rm;
    runner->detect_handle = nullptr;
    memset(&runner->detect_map, 0, sizeof(runner->detect_map));
    runner->detect_buffer = nullptr;
    runner->detect_length = 0U;

    if (rm == nullptr) {
        return false;
    }
    runner->detect_handle = sshc_rm_scope_alloc(
        rm, SSH_CHATTER_DOOR_DETECT_BUFFER_BYTES, "door_detect");
    if (runner->detect_handle == nullptr) {
        return false;
    }
    if (ttak_abstract_map(runner->detect_handle, 0U,
                          SSH_CHATTER_DOOR_DETECT_BUFFER_BYTES,
                          TTAK_ABSTRACT_ACCESS_WRITE,
                          &runner->detect_map) != 0) {
        sshc_rm_scope_free(rm, runner->detect_handle);
        runner->detect_handle = nullptr;
        return false;
    }
    runner->detect_buffer = (unsigned char *)runner->detect_map.data;
    return runner->detect_buffer != nullptr;
}

static void door_runner_detect_release(host_door_runner_t *runner)
{
    if (runner == nullptr) {
        return;
    }
    if (runner->detect_buffer != nullptr) {
        ttak_abstract_unmap(&runner->detect_map);
        runner->detect_buffer = nullptr;
    }
    if (runner->detect_handle != nullptr && runner->rm != nullptr) {
        sshc_rm_scope_free(runner->rm, runner->detect_handle);
    }
    runner->detect_handle = nullptr;
    runner->detect_length = 0U;
}

enum {
    DOOR_ENC_UNKNOWN = 0,
    DOOR_ENC_PASSTHROUGH = 1,
    DOOR_ENC_UTF8 = 2,
    DOOR_ENC_CP949 = 3,
    DOOR_ENC_JOHAB = 4,
};

/* ---- Encoding detection ----
 *
 * dosbox output is typically a mix of ANSI control bytes and either CP949,
 * JOHAB (older Korean DOS games), or already-UTF-8 (modern dosbox builds
 * with translation layers). We accumulate the first
 * SSH_CHATTER_DOOR_DETECT_BUFFER_BYTES of stdout and run three independent
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

    int cp949_score = door_enc_score_cp949(data, len);
    int johab_score = door_enc_score_johab(data, len);

    if (cp949_score <= 0 && johab_score <= 0) {
        return DOOR_ENC_PASSTHROUGH;
    }
    if (cp949_score >= johab_score) {
        return DOOR_ENC_CP949;
    }
    return DOOR_ENC_JOHAB;
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
        default:
            return nullptr;
    }
}

/* Convert `len` bytes of `src` from runner->detected_encoding into UTF-8 and
 * write to the SSH session via session_channel_write. Falls back to
 * passthrough on conversion failure. The function is best-effort: it never
 * returns an error to the caller, since the alternative is to drop the
 * door's output. */
static void door_emit_converted(session_ctx_t *ctx, host_door_runner_t *runner,
                                const void *src, size_t len)
{
    if (ctx == nullptr || runner == nullptr || src == nullptr || len == 0U) {
        return;
    }

    if (runner->detected_encoding == DOOR_ENC_PASSTHROUGH ||
        runner->detected_encoding == DOOR_ENC_UTF8 ||
        runner->detected_encoding == DOOR_ENC_UNKNOWN) {
        session_channel_write(ctx, src, len);
        return;
    }

    const char *label = door_enc_iconv_label(runner->detected_encoding);
    if (label == nullptr) {
        session_channel_write(ctx, src, len);
        return;
    }

    iconv_t cd = iconv_open("UTF-8", label);
    if (cd == (iconv_t)-1) {
        session_channel_write(ctx, src, len);
        return;
    }

    /* Output buffer roughly 4× input — Korean glyphs occupy 3 bytes in
     * UTF-8 so 2× CP949 → 3× UTF-8 worst case; ANSI pass-through is 1×.
     * Round up generously. */
    size_t out_capacity = len * 4U + 64U;
    char *out_buffer = (char *)sshc_gc_malloc(out_capacity);
    if (out_buffer == nullptr) {
        iconv_close(cd);
        session_channel_write(ctx, src, len);
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
                char *grown = (char *)sshc_gc_realloc(out_buffer, new_capacity);
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
        session_channel_write(ctx, out_buffer, produced);
    }
    sshc_gc_free(out_buffer);
    iconv_close(cd);
}

static void session_bbs_door_list(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }
    host_t *host = ctx->owner;
    if (host->door_game_count == 0U) {
        session_send_system_line(
            ctx,
            "No DOOR games configured. Operators: set CHATTER_DOOR_1="
            "name:dosbox_conf[:desc] (and _2, _3 ...) to register doors.");
        return;
    }

    session_send_system_line(ctx, "Available DOOR games:");
    char buffer[SSH_CHATTER_MESSAGE_LIMIT];
    for (size_t idx = 0U; idx < host->door_game_count; ++idx) {
        door_game_entry_t entry;
        if (!host_door_game_get(host, idx, &entry) || !entry.in_use) {
            continue;
        }
        if (entry.description[0] == '\0') {
            snprintf(buffer, sizeof(buffer), "  %s", entry.name);
        } else {
            snprintf(buffer, sizeof(buffer), "  %s — %s",
                     entry.name, entry.description);
        }
        session_send_system_line(ctx, buffer);
    }
    session_send_system_line(
        ctx,
        "Launch with `/bbs door <name>`. Press Ctrl-] to exit a running game.");
}

static bool session_bbs_door_caller_authorised(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }
    return ctx->user.is_operator || ctx->user.is_lan_operator;
}

static int session_bbs_door_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void session_bbs_door_terminate_child(host_door_runner_t *runner)
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

static bool session_bbs_door_spawn(const char *dosbox_conf,
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

        /* Close any other inherited fds so dosbox cannot accidentally talk
         * to a chat socket. */
        for (int fd = STDERR_FILENO + 1; fd < 1024; ++fd) {
            close(fd);
        }

        char *const argv[] = {
            (char *)"dosbox",
            (char *)"-conf",
            (char *)dosbox_conf,
            (char *)"-exit",
            nullptr,
        };
        execvp("dosbox", argv);
        _exit(127);
    }

    close(slave_fd);
    if (session_bbs_door_set_nonblocking(master_fd) < 0) {
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

static bool session_bbs_door_io_loop(session_ctx_t *ctx,
                                     host_door_runner_t *runner)
{
    if (ctx == nullptr || runner == nullptr || !runner->active) {
        return false;
    }

    char buffer[SSH_CHATTER_DOOR_BUFFER_SIZE];

    while (runner->active) {
        struct pollfd pfd = {
            .fd = runner->master_fd,
            .events = POLLIN,
            .revents = 0,
        };
        int poll_result = poll(&pfd, 1, SSH_CHATTER_DOOR_IDLE_POLL_MS);
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
                /* PTY closed — child exited. */
                break;
            }
            if (got > 0) {
                /* Encoding detection: pre-decision, accumulate into the
                 * detect buffer; once decided, every chunk goes through the
                 * converter. */
                if (!runner->encoding_decided && runner->detect_buffer != nullptr) {
                    size_t room = SSH_CHATTER_DOOR_DETECT_BUFFER_BYTES -
                                  runner->detect_length;
                    size_t copy = ((size_t)got < room) ? (size_t)got : room;
                    if (copy > 0U) {
                        memcpy(runner->detect_buffer + runner->detect_length,
                               buffer, copy);
                        runner->detect_length += copy;
                    }
                    bool buffer_full = runner->detect_length ==
                                       SSH_CHATTER_DOOR_DETECT_BUFFER_BYTES;
                    bool seen_high_bit = false;
                    for (size_t k = 0U; k < runner->detect_length; ++k) {
                        if (runner->detect_buffer[k] >= 0x80U) {
                            seen_high_bit = true;
                            break;
                        }
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
                        door_emit_converted(ctx, runner, runner->detect_buffer,
                                            runner->detect_length);
                        /* Encoding is now decided — release the 4 KiB scratch
                         * buffer immediately rather than holding it for the
                         * rest of the door session. */
                        door_runner_detect_release(runner);
                    }
                } else {
                    door_emit_converted(ctx, runner, buffer, (size_t)got);
                }
            }
        } else if (poll_result > 0 &&
                   (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            break;
        }

        /* Reap if the child has exited even when the PTY still has buffered
         * output to drain. */
        int status = 0;
        pid_t reaped = waitpid(runner->child_pid, &status, WNOHANG);
        if (reaped == runner->child_pid) {
            runner->child_pid = -1;
            /* Drain any final output before returning. */
            for (;;) {
                ssize_t residual =
                    read(runner->master_fd, buffer, sizeof(buffer));
                if (residual <= 0) {
                    break;
                }
                if (!runner->encoding_decided) {
                    runner->detected_encoding =
                        runner->detect_buffer != nullptr
                            ? door_enc_decide(runner->detect_buffer,
                                              runner->detect_length)
                            : DOOR_ENC_PASSTHROUGH;
                    runner->encoding_decided = true;
                    if (runner->detect_buffer != nullptr &&
                        runner->detect_length > 0U) {
                        door_emit_converted(ctx, runner, runner->detect_buffer,
                                            runner->detect_length);
                    }
                    door_runner_detect_release(runner);
                }
                door_emit_converted(ctx, runner, buffer, (size_t)residual);
            }
            break;
        }

        /* Optionally read from the user, with a small timeout so we keep
         * polling the master fd. */
        int read_result =
            session_channel_read_poll(ctx, buffer, sizeof(buffer), 50);
        if (read_result == SESSION_CHANNEL_TIMEOUT) {
            /* nothing to forward */
        } else if (read_result <= 0) {
            /* Session lost; tear down the door. */
            break;
        } else {
            /* Look for the escape byte (Ctrl-]). */
            int escape_idx = -1;
            for (int idx = 0; idx < read_result; ++idx) {
                if ((unsigned char)buffer[idx] == SSH_CHATTER_DOOR_QUIT_BYTE) {
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
                session_send_system_line(
                    ctx, "[door] escape sequence detected, ending door game.");
                break;
            }
        }

        /* Hard runtime cap. */
        if (runner->started_at != (time_t)-1 &&
            time(nullptr) - runner->started_at >
                SSH_CHATTER_DOOR_MAX_RUNTIME_SECONDS) {
            session_send_system_line(
                ctx, "[door] maximum runtime reached, terminating door game.");
            break;
        }

        /* Honour a host shutdown signal so we don't pin the daemon. */
        if (ctx->owner != nullptr && ctx->owner->shutdown_flag != nullptr &&
            *ctx->owner->shutdown_flag != 0) {
            break;
        }
    }

    session_bbs_door_terminate_child(runner);
    return true;
}

static void session_bbs_door_run(session_ctx_t *ctx, const char *name)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!session_bbs_door_caller_authorised(ctx)) {
        session_send_system_line(
            ctx, "Only operators may launch DOOR games on this server.");
        return;
    }

    if (name == nullptr || name[0] == '\0') {
        session_bbs_door_list(ctx);
        return;
    }

    door_game_entry_t entry;
    if (!host_door_game_lookup(ctx->owner, name, &entry)) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message),
                 "Unknown DOOR game '%s'. Try `/bbs door` for the list.",
                 name);
        session_send_system_line(ctx, message);
        return;
    }

    char status[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(status, sizeof(status),
             "[door] launching '%s' (Ctrl-] to exit, %ds max).",
             entry.name, SSH_CHATTER_DOOR_MAX_RUNTIME_SECONDS);
    session_send_system_line(ctx, status);

    host_door_runner_t runner = {
        .child_pid = -1,
        .master_fd = -1,
        .started_at = 0,
        .active = false,
        .rm = nullptr,
        .detect_handle = nullptr,
        .detect_buffer = nullptr,
        .detect_length = 0U,
        .encoding_decided = false,
        .detected_encoding = DOOR_ENC_UNKNOWN,
    };

    if (!door_runner_detect_acquire(&runner, ctx->owner->resource_manager)) {
        session_send_system_line(
            ctx, "[door] failed to allocate detection scratch buffer.");
        return;
    }

    if (!session_bbs_door_spawn(entry.dosbox_conf, &runner)) {
        door_runner_detect_release(&runner);
        session_send_system_line(
            ctx,
            "[door] failed to launch dosbox. Verify the conf path exists and "
            "dosbox is on PATH.");
        return;
    }

    (void)session_bbs_door_io_loop(ctx, &runner);
    door_runner_detect_release(&runner);
    session_send_system_line(ctx, "[door] session ended.");
}

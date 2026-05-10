/**
 * @file host_runtime.c
 * @desc File-level documentation for host_runtime.c, describing its role
 *       in the SSH-Chatter server and providing a consistent header
 *       comment format across C sources.
 * @return None.
 */

// Remaining operator commands, ban management, and host lifecycle entry points.
#include "../host_internal.h"
#if defined(__GLIBC__)
#include <malloc.h>
#endif

#define TELNET_STABLE_RESET_SECONDS 10.0
#define SSH_CHATTER_TCP_KEEPALIVE_IDLE 30
#define SSH_CHATTER_TCP_KEEPALIVE_INTERVAL 10
#define SSH_CHATTER_TCP_KEEPALIVE_COUNT 3
// Total dead-peer detection time (seconds) for TCP_USER_TIMEOUT.
// Should be >= IDLE + INTERVAL * COUNT to avoid premature drops.
#define SSH_CHATTER_TCP_USER_TIMEOUT_MS                                        \
    ((SSH_CHATTER_TCP_KEEPALIVE_IDLE +                                         \
      SSH_CHATTER_TCP_KEEPALIVE_INTERVAL * SSH_CHATTER_TCP_KEEPALIVE_COUNT) *  \
     1000)
// SSH-level operation timeout (seconds) for ssh_options_set.
#define SSH_CHATTER_SSH_TIMEOUT_SECONDS 60
// Interval between poll() wakeups in the accept loop (milliseconds).
#define SSH_CHATTER_ACCEPT_POLL_TIMEOUT_MS 2000
// Maximum consecutive poll timeouts before forcing a bind socket health check.
#define SSH_CHATTER_ACCEPT_HEALTH_CHECK_POLLS 30
#define SESSION_LIFETIME_INITIAL_UNITS 8U
#define SESSION_LIFETIME_ACTIVITY_BONUS 2U
#define SESSION_LIFETIME_MAX_UNITS 64U
#define SESSION_LIFETIME_MIN_UNITS 1U
#define SESSION_LIFETIME_INACTIVE_THRESHOLD (20 * 60)
#define SESSION_LIFETIME_DECAY_INTERVAL (5 * 60)
#define SSH_CHATTER_AI_MEMORY_CONTEXT_LIMIT 3U
#define SSH_CHATTER_AI_MEMORY_TOKEN_LIMIT 12U
#define SSH_CHATTER_AI_MEMORY_PREVIEW_LEN 160U
#define SSH_CHATTER_AI_MEMORY_CONTEXT_BUFFER SSH_CHATTER_MESSAGE_LIMIT
#define SSH_CHATTER_AI_PROMPT_CONTEXT_MAX 1536U
#define SSH_CHATTER_AI_PROMPT_MESSAGE_MAX (SSH_CHATTER_MESSAGE_LIMIT / 2U)
#define SSH_CHATTER_AI_PROMPT_USERNAME_MAX (SSH_CHATTER_USERNAME_LEN - 1U)
#define HOST_IDLE_UNLOAD_SECONDS 1
#define HOST_IDLE_CHECK_INTERVAL_NS 5000LL

static inline void session_safe_free(void **ptr)
{
    if (ptr != nullptr && *ptr != nullptr) {
        sshc_gc_free(*ptr);
        *ptr = nullptr;
    }
}

static struct timespec session_now_monotonic(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        now.tv_sec = time(nullptr);
        now.tv_nsec = 0L;
    }
    return now;
}

static double session_timespec_elapsed_seconds(const struct timespec *now,
                                               const struct timespec *then)
{
    if (now == nullptr || then == nullptr) {
        return 0.0;
    }
    double seconds = (double)(now->tv_sec - then->tv_sec);
    double nanos = (double)(now->tv_nsec - then->tv_nsec) / 1000000000.0;
    return seconds + nanos;
}

static void session_timespec_add_seconds(struct timespec *ts, time_t seconds)
{
    if (ts == nullptr) {
        return;
    }
    ts->tv_sec += seconds;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static void host_ai_chat_copy_limited(char *dest, size_t dest_len,
                                      const char *src, size_t limit)
{
    if (dest == nullptr || dest_len == 0U) {
        return;
    }
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }

    size_t max_copy = dest_len - 1U;
    if (limit < max_copy) {
        max_copy = limit;
    }

    size_t copied = strnlen(src, max_copy);
    memcpy(dest, src, copied);
    dest[copied] = '\0';
}

static inline void host_gc_cycle(host_t *host, struct timespec *last_gc_run)
{
    if (host == NULL || host->memory_context == NULL || last_gc_run == NULL) {
        return;
    }

    struct timespec now = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return;
    }

    const long elapsed_sec = now.tv_sec - last_gc_run->tv_sec;
    const long elapsed_nsec = now.tv_nsec - last_gc_run->tv_nsec;
    const long long elapsed_total_ns =
        (long long)elapsed_sec * 1000000000LL + (long long)elapsed_nsec;
    if (elapsed_total_ns < 250000000LL) {
        return;
    }

    sshc_memory_context_collect(host->memory_context, 1U);
    sshc_epoch_reclaim();
    *last_gc_run = now;

    /* Steady-state heap trim. Glibc auto-trims via M_TRIM_THRESHOLD on free,
     * but only at the top of the main arena; explicit trim walks every arena
     * so RSS doesn't drift up between session churns. */
    static _Thread_local struct timespec last_malloc_trim = {0};
    const long trim_elapsed_sec = now.tv_sec - last_malloc_trim.tv_sec;
    if (trim_elapsed_sec >= 60L) {
#if defined(__GLIBC__)
        (void)malloc_trim(0);
#endif
        last_malloc_trim = now;
    }
}

void session_manual_gc_tick(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->memory_context == nullptr) {
        return;
    }
    sshc_memory_context_collect(ctx->memory_context, 1U);
}

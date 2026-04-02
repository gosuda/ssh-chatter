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
#define SSH_CHATTER_TCP_KEEPALIVE_IDLE 60
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
#define HOST_IDLE_UNLOAD_SECONDS 0
#define HOST_IDLE_CHECK_INTERVAL_NS 0LL
#define HOST_MEMORY_PRESSURE_CHECK_INTERVAL_NS 1000000000LL
#define HOST_MEMORY_PRESSURE_DEFAULT_RSS_MB 768ULL

static pthread_once_t g_host_memory_pressure_limit_once = PTHREAD_ONCE_INIT;
static size_t g_host_memory_pressure_limit_bytes = 0U;

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

static size_t host_process_rss_bytes(void)
{
    FILE *fp = fopen("/proc/self/statm", "r");
    if (fp == nullptr) {
        return 0U;
    }

    unsigned long pages_total = 0UL;
    unsigned long pages_resident = 0UL;
    int scanned = fscanf(fp, "%lu %lu", &pages_total, &pages_resident);
    fclose(fp);
    if (scanned != 2) {
        return 0U;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return 0U;
    }

    return (size_t)pages_resident * (size_t)page_size;
}

static void host_memory_pressure_limit_bytes_init(void)
{
    const char *raw = getenv("CHATTER_MAX_RSS_MB");
    unsigned long long parsed_mb = HOST_MEMORY_PRESSURE_DEFAULT_RSS_MB;
    if (raw != nullptr && raw[0] != '\0') {
        char *end_ptr = nullptr;
        errno = 0;
        unsigned long long candidate = strtoull(raw, &end_ptr, 10);
        if (errno == 0 && end_ptr != raw &&
            (end_ptr == nullptr || *end_ptr == '\0')) {
            parsed_mb = candidate;
        }
    }
    if (parsed_mb == 0ULL ||
        parsed_mb > (unsigned long long)(SIZE_MAX / (1024ULL * 1024ULL))) {
        g_host_memory_pressure_limit_bytes = 0U;
        return;
    }
    g_host_memory_pressure_limit_bytes = (size_t)(parsed_mb * 1024ULL * 1024ULL);
}

static size_t host_memory_pressure_limit_bytes(void)
{
    pthread_once(&g_host_memory_pressure_limit_once,
                 host_memory_pressure_limit_bytes_init);
    return g_host_memory_pressure_limit_bytes;
}

static bool host_memory_pressure_restart(host_t *host,
                                         struct timespec *last_check)
{
    if (host == nullptr || host->shutdown_flag == nullptr || last_check == nullptr) {
        return false;
    }

    const size_t limit_bytes = host_memory_pressure_limit_bytes();
    if (limit_bytes == 0U) {
        return false;
    }

    struct timespec now = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return false;
    }

    const long elapsed_sec = now.tv_sec - last_check->tv_sec;
    const long elapsed_nsec = now.tv_nsec - last_check->tv_nsec;
    const long long elapsed_total_ns =
        (long long)elapsed_sec * 1000000000LL + (long long)elapsed_nsec;
    if (elapsed_total_ns < HOST_MEMORY_PRESSURE_CHECK_INTERVAL_NS) {
        return false;
    }
    *last_check = now;

    const size_t rss_bytes = host_process_rss_bytes();
    if (rss_bytes <= limit_bytes) {
        return false;
    }

    const size_t rss_mb = rss_bytes / (1024U * 1024U);
    const size_t limit_mb = limit_bytes / (1024U * 1024U);
    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice),
             "* [system] memory pressure detected (%zuMB > %zuMB). "
             "Restarting server immediately for cleanup.",
             rss_mb, limit_mb);
    host_history_record_system(host, notice, nullptr);
    chat_room_broadcast(&host->room, notice, nullptr);
    host->force_restart_requested = true;
    *host->shutdown_flag = 1;
    return true;
}

static inline bool host_gc_cycle(host_t *host, struct timespec *last_gc_run,
                                 struct timespec *last_pressure_check)
{
    if (host == NULL || host->memory_context == NULL || last_gc_run == NULL) {
        return false;
    }

    struct timespec now = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return false;
    }

    const long elapsed_sec = now.tv_sec - last_gc_run->tv_sec;
    const long elapsed_nsec = now.tv_nsec - last_gc_run->tv_nsec;
    const long long elapsed_total_ns =
        (long long)elapsed_sec * 1000000000LL + (long long)elapsed_nsec;
    if (elapsed_total_ns < 1000000000LL) {
        return host_memory_pressure_restart(host, last_pressure_check);
    }

    sshc_memory_context_epoch_gc_rotate(host->memory_context);
    sshc_epoch_reclaim();
    *last_gc_run = now;
    return host_memory_pressure_restart(host, last_pressure_check);
}

void session_manual_gc_tick(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->memory_context == nullptr) {
        return;
    }
    sshc_memory_context_epoch_gc_rotate(ctx->memory_context);
}


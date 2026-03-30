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

void host_manual_gc_tick(host_t *host)
{
    if (host == nullptr || host->memory_context == nullptr) {
        return;
    }
    sshc_memory_context_epoch_gc_rotate(host->memory_context);
    sshc_epoch_reclaim();
}

static bool host_room_has_members(host_t *host)
{
    if (host == nullptr) {
        return false;
    }
    bool has_members = false;
    ttak_mutex_lock(&host->room.lock);
    has_members = host->room.member_count > 0U;
    ttak_mutex_unlock(&host->room.lock);
    return has_members;
}

static void host_history_release_cache(host_t *host)
{
    if (host == nullptr) {
        return;
    }
    chat_history_entry_t *buffer = nullptr;
    ttak_mutex_lock(&host->lock);
    buffer = host->history;
    host->history = nullptr;
    host->history_capacity = 0U;
    host->history_count = 0U;
    host->history_start_index = host->history_total;
    host->history_cache_loaded = false;
    ttak_mutex_unlock(&host->lock);
    if (buffer != nullptr) {
        sshc_gc_free(buffer);
    }
}

static bool host_bbs_acquire_storage(host_t *host)
{
    if (host == nullptr) {
        return false;
    }
    if (host->bbs_posts != nullptr) {
        return true;
    }

    bbs_post_t *allocated = (bbs_post_t *)sshc_gc_calloc(
        SSH_CHATTER_BBS_MAX_POSTS, sizeof(host->bbs_posts[0]));
    if (allocated == nullptr) {
        humanized_log_error("bbs", "failed to allocate post cache",
                            errno != 0 ? errno : ENOMEM);
        return false;
    }

    ttak_mutex_lock(&host->lock);
    if (host->bbs_posts != nullptr) {
        ttak_mutex_unlock(&host->lock);
        sshc_gc_free(allocated);
        return true;
    }

    host->bbs_posts = allocated;
    host->bbs_post_capacity = SSH_CHATTER_BBS_MAX_POSTS;
    for (size_t idx = 0U; idx < host->bbs_post_capacity; ++idx) {
        host->bbs_posts[idx].in_use = false;
        host->bbs_posts[idx].id = 0U;
        host->bbs_posts[idx].author[0] = '\0';
        host->bbs_posts[idx].title[0] = '\0';
        host->bbs_posts[idx].body[0] = '\0';
        host->bbs_posts[idx].tag_count = 0U;
        host->bbs_posts[idx].created_at = 0;
        host->bbs_posts[idx].bumped_at = 0;
        host->bbs_posts[idx].comment_count = 0U;
        for (size_t comment = 0U; comment < SSH_CHATTER_BBS_MAX_COMMENTS;
             ++comment) {
            host->bbs_posts[idx].comments[comment].author[0] = '\0';
            host->bbs_posts[idx].comments[comment].text[0] = '\0';
            host->bbs_posts[idx].comments[comment].created_at = 0;
        }
    }
    host->bbs_cache_loaded = true;
    ttak_mutex_unlock(&host->lock);
    return true;
}

static void host_bbs_release_cache(host_t *host)
{
    if (host == nullptr) {
        return;
    }
    bbs_post_t *posts = nullptr;
    ttak_mutex_lock(&host->lock);
    posts = host->bbs_posts;
    host->bbs_posts = nullptr;
    host->bbs_post_capacity = 0U;
    host->bbs_post_count = 0U;
    host->bbs_cache_loaded = false;
    ttak_mutex_unlock(&host->lock);
    if (posts != nullptr) {
        sshc_gc_free(posts);
    }
}

static void host_reload_cached_state(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (!host->history_cache_loaded) {
        host_state_load(host);
        host->history_cache_loaded =
            host->history != nullptr && host->history_capacity > 0U;
    }

    if (!host->bbs_cache_loaded) {
        if (host_bbs_acquire_storage(host)) {
            host_bbs_state_load(host);
            host->bbs_cache_loaded = host_bbs_storage_ready(host);
        }
    }
}

static void host_idle_state_maintenance(host_t *host,
                                        struct timespec *last_idle_check)
{
    if (host == nullptr || last_idle_check == nullptr) {
        return;
    }

    struct timespec now = session_now_monotonic();
    long long elapsed_ns =
        (long long)(now.tv_sec - last_idle_check->tv_sec) * 1000000000LL +
        (long long)(now.tv_nsec - last_idle_check->tv_nsec);
    if (elapsed_ns < HOST_IDLE_CHECK_INTERVAL_NS) {
        return;
    }
    *last_idle_check = now;

    if (host_room_has_members(host)) {
        host->idle_state_pending = false;
        host->last_room_empty_time = now;
        return;
    }

    if (!host->idle_state_pending) {
        host->last_room_empty_time = now;
        host->idle_state_pending = true;
        if (HOST_IDLE_UNLOAD_SECONDS <= 0) {
            goto unload_idle_state;
        }
        return;
    }

    long long idle_ns =
        (long long)(now.tv_sec - host->last_room_empty_time.tv_sec) *
            1000000000LL +
        (long long)(now.tv_nsec - host->last_room_empty_time.tv_nsec);
    if (HOST_IDLE_UNLOAD_SECONDS > 0 &&
        idle_ns < (long long)HOST_IDLE_UNLOAD_SECONDS * 1000000000LL) {
        return;
    }

unload_idle_state:
    host_history_release_cache(host);
    host_bbs_release_cache(host);
    host_manual_gc_tick(host);
#if defined(__GLIBC__)
    (void)malloc_trim(0);
#endif
    host->idle_state_pending = false;
    host->last_room_empty_time = now;
}

static bool host_ai_chat_enable(host_t *host);
static bool host_ai_chat_disable(host_t *host);
static void host_ai_chat_consider_reply(host_t *host,
                                        const chat_history_entry_t *entry);
static void host_ai_chat_snapshot_state(host_t *host, char *model,
                                        size_t model_len,
                                        struct timespec *last_reply);
static const char *host_ai_chat_default_model(void);

bool session_bbs_workspace_acquire(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }

    if (ctx->pending_bbs_title == nullptr) {
        ctx->pending_bbs_title =
            (char *)sshc_gc_calloc(SSH_CHATTER_BBS_TITLE_LEN, sizeof(char));
    }
    if (ctx->pending_bbs_tags == nullptr) {
        ctx->pending_bbs_tags = sshc_gc_calloc(
            SSH_CHATTER_BBS_MAX_TAGS, sizeof(*ctx->pending_bbs_tags));
    }
    if (ctx->pending_bbs_body == nullptr) {
        ctx->pending_bbs_body =
            (char *)sshc_gc_calloc(SSH_CHATTER_BBS_BODY_LEN, sizeof(char));
    }
    if (ctx->bbs_editor_clipboard == nullptr) {
        ctx->bbs_editor_clipboard =
            (char *)sshc_gc_calloc(SSH_CHATTER_BBS_BODY_LEN, sizeof(char));
    }

    bool ok = ctx->pending_bbs_title != nullptr &&
              ctx->pending_bbs_tags != nullptr &&
              ctx->pending_bbs_body != nullptr &&
              ctx->bbs_editor_clipboard != nullptr;

    /* On partial failure release whatever was allocated to avoid leaks. */
    if (!ok) {
        session_bbs_workspace_release(ctx);
    }

    return ok;
}

void session_bbs_workspace_release(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    session_safe_free((void **)&ctx->pending_bbs_title);
    session_safe_free((void **)&ctx->pending_bbs_tags);
    session_safe_free((void **)&ctx->pending_bbs_body);
    session_safe_free((void **)&ctx->bbs_editor_clipboard);
}

bool session_bbs_view_notice_acquire(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }
    if (ctx->bbs_view_notice == nullptr) {
        ctx->bbs_view_notice = (char *)sshc_gc_calloc(
            SSH_CHATTER_MESSAGE_LIMIT, sizeof(char));
    }
    return ctx->bbs_view_notice != nullptr;
}

void session_bbs_view_notice_release(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    session_safe_free((void **)&ctx->bbs_view_notice);
}

bool session_asciiart_buffer_acquire(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }
    if (ctx->asciiart_buffer == nullptr) {
        ctx->asciiart_buffer = (char *)sshc_gc_calloc(
            SSH_CHATTER_ASCIIART_BUFFER_LEN, sizeof(char));
    }
    return ctx->asciiart_buffer != nullptr;
}

void session_asciiart_buffer_release(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    session_safe_free((void **)&ctx->asciiart_buffer);
}

bool session_tetris_buffers_acquire(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }
    if (ctx->tetris_screen_buffer == nullptr) {
        ctx->tetris_screen_buffer = (char *)sshc_gc_calloc(
            SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE, sizeof(char));
    }
    if (ctx->tetris_prev_screen_buffer == nullptr) {
        ctx->tetris_prev_screen_buffer = (char *)sshc_gc_calloc(
            SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE, sizeof(char));
    }
    const bool ok = ctx->tetris_screen_buffer != nullptr &&
                    ctx->tetris_prev_screen_buffer != nullptr;
    if (!ok) {
        session_tetris_buffers_release(ctx);
    }
    return ok;
}

void session_tetris_buffers_release(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    session_safe_free((void **)&ctx->tetris_screen_buffer);
    session_safe_free((void **)&ctx->tetris_prev_screen_buffer);
}

tetris_game_state_t *session_game_ensure_tetris(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return nullptr;
    }
    if (ctx->game.tetris == nullptr) {
        ctx->game.tetris =
            (tetris_game_state_t *)sshc_gc_calloc(1U, sizeof(tetris_game_state_t));
    }
    return ctx->game.tetris;
}

tetris_game_state_t *
session_game_ensure_saved_tetris(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return nullptr;
    }
    if (ctx->game.saved_tetris_state == nullptr) {
        ctx->game.saved_tetris_state =
            (tetris_game_state_t *)sshc_gc_calloc(1U, sizeof(tetris_game_state_t));
    }
    return ctx->game.saved_tetris_state;
}

void session_game_release_tetris(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    session_safe_free((void **)&ctx->game.tetris);
}

void session_game_release_saved_tetris(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    session_safe_free((void **)&ctx->game.saved_tetris_state);
}

void session_mark_activity(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    struct timespec now = session_now_monotonic();
    ctx->lifetime_last_activity = now;
    ctx->lifetime_has_activity = true;
    ctx->lifetime_decay_active = false;

    if (ctx->lifetime_units < SESSION_LIFETIME_MAX_UNITS) {
        uint32_t next =
            ctx->lifetime_units + SESSION_LIFETIME_ACTIVITY_BONUS;
        ctx->lifetime_units =
            next > SESSION_LIFETIME_MAX_UNITS ? SESSION_LIFETIME_MAX_UNITS
                                              : next;
    }
}

bool session_enforce_lifetime(session_ctx_t *ctx,
                              const struct timespec *now)
{
    if (ctx == nullptr || now == nullptr) {
        return false;
    }

    if (!ctx->lifetime_has_activity) {
        return false;
    }

    double idle_seconds =
        session_timespec_elapsed_seconds(now, &ctx->lifetime_last_activity);
    if (idle_seconds < SESSION_LIFETIME_INACTIVE_THRESHOLD) {
        ctx->lifetime_decay_active = false;
        return false;
    }

    if (!ctx->lifetime_decay_active) {
        ctx->lifetime_decay_reference = ctx->lifetime_last_activity;
        session_timespec_add_seconds(&ctx->lifetime_decay_reference,
                                     SESSION_LIFETIME_INACTIVE_THRESHOLD);
        ctx->lifetime_decay_active = true;
    }

    double since_decay =
        session_timespec_elapsed_seconds(now, &ctx->lifetime_decay_reference);
    if (since_decay < SESSION_LIFETIME_DECAY_INTERVAL) {
        return false;
    }

    size_t intervals =
        (size_t)(since_decay / SESSION_LIFETIME_DECAY_INTERVAL);
    session_timespec_add_seconds(
        &ctx->lifetime_decay_reference,
        (time_t)(intervals * SESSION_LIFETIME_DECAY_INTERVAL));

    for (size_t idx = 0U; idx < intervals; ++idx) {
        if (ctx->lifetime_units > SESSION_LIFETIME_MIN_UNITS) {
            ctx->lifetime_units /= 2U;
            if (ctx->lifetime_units < SESSION_LIFETIME_MIN_UNITS) {
                ctx->lifetime_units = SESSION_LIFETIME_MIN_UNITS;
            }
        }
    }

    if (ctx->lifetime_units <= SESSION_LIFETIME_MIN_UNITS) {
        session_force_disconnect(ctx, "Session removed after inactivity.");
        return true;
    }

    return false;
}

static session_ctx_t *session_create(void)
{
    // Initially allocate session context in the current (likely global) scope
    session_ctx_t *ctx =
        (session_ctx_t *)sshc_gc_calloc(1U, sizeof(session_ctx_t));

    if (ctx != nullptr) {
        // Create a dedicated memory context for this session
        ctx->memory_context = sshc_memory_context_create("session");
        if (ctx->memory_context == nullptr) {
            sshc_gc_free(ctx);
            return nullptr;
        }

        // Initialize session-specific owner for strict isolation
        ctx->session_owner = ttak_owner_create(TTAK_OWNER_STRICT_ISOLATION);
        if (ctx->session_owner == nullptr) {
            sshc_memory_context_destroy(ctx->memory_context);
            sshc_gc_free(ctx);
            return nullptr;
        }

        // Push session context so subsequent allocations happen in this scope
        sshc_memory_context_t *session_scope =
            sshc_memory_context_push(ctx->memory_context);

        ctx->user.is_authenticated = false;
        ctx->session_id = 0U;
        ctx->session_data = nullptr;
        ctx->prefer_utf16_output = false;
        ctx->newline_mode = SESSION_NEWLINE_MODE_AUTO;
        ctx->active_codepage = SESSION_CODEPAGE_CP437; /* Default to CP437 */
        ctx->morse_feed_enabled = false;
        ctx->exit_notice_sent = false;
        atomic_init(&ctx->room_snapshot_retired, false);
        atomic_init(&ctx->room_snapshot_refs, 0U);
        ctx->has_last_output_line = false;
        ctx->disable_output_dedup = false;
        ctx->lifetime_units = SESSION_LIFETIME_INITIAL_UNITS;
        ctx->lifetime_last_activity = session_now_monotonic();
        ctx->lifetime_decay_reference = ctx->lifetime_last_activity;
        ctx->lifetime_has_activity = true;
        ctx->lifetime_decay_active = false;

        printf("[encoding-debug] phase=ctx_init transport_kind=%d "
               "prefer_utf16_output=%d output_kind=%d\n",
               (int)ctx->transport_kind, (int)ctx->prefer_utf16_output,
               (int)ctx->output_kind);

        if (display_model_init(&ctx->display_model, 256U)) {
            ctx->display_model_initialized = true;
        }

        bool ascii_ready = session_asciiart_buffer_acquire(ctx);
        if (!ascii_ready) {
            session_asciiart_buffer_release(ctx);

            sshc_memory_context_pop(session_scope);
            if (ctx->session_owner != nullptr) {
                ttak_owner_destroy(ctx->session_owner);
            }
            sshc_memory_context_destroy(ctx->memory_context);
            sshc_gc_free(ctx);
            return nullptr;
        }

        sshc_memory_context_pop(session_scope);
    }
    return ctx;
}

static void session_configure_tcp_keepalive(ssh_session session)
{
    if (session == nullptr) {
        return;
    }

    const int socket_fd = ssh_get_fd(session);
    if (socket_fd < 0) {
        return;
    }

    int enabled = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE, &enabled,
                   sizeof(enabled)) < 0) {
        fprintf(stderr, "[session] setsockopt SO_KEEPALIVE failed\n");
    }

#ifdef TCP_KEEPIDLE
    {
        int idle_seconds = SSH_CHATTER_TCP_KEEPALIVE_IDLE;
        if (setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle_seconds,
                       sizeof(idle_seconds)) < 0) {
            fprintf(stderr, "[session] setsockopt TCP_KEEPIDLE failed\n");
        }
    }
#endif

#ifdef TCP_KEEPALIVE
    {
        int idle_seconds = SSH_CHATTER_TCP_KEEPALIVE_IDLE;
        if (setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle_seconds,
                       sizeof(idle_seconds)) < 0) {
            fprintf(stderr, "[session] setsockopt TCP_KEEPALIVE failed\n");
        }
    }
#endif

#ifdef TCP_KEEPINTVL
    {
        int interval_seconds = SSH_CHATTER_TCP_KEEPALIVE_INTERVAL;
        if (setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval_seconds,
                       sizeof(interval_seconds)) < 0) {
            fprintf(stderr, "[session] setsockopt TCP_KEEPINTVL failed\n");
        }
    }
#endif

#ifdef TCP_KEEPCNT
    {
        int keepalive_probes = SSH_CHATTER_TCP_KEEPALIVE_COUNT;
        if (setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPCNT, &keepalive_probes,
                       sizeof(keepalive_probes)) < 0) {
            fprintf(stderr, "[session] setsockopt TCP_KEEPCNT failed\n");
        }
    }
#endif

#ifdef TCP_USER_TIMEOUT
    {
        int user_timeout_ms = SSH_CHATTER_TCP_USER_TIMEOUT_MS;
        if (setsockopt(socket_fd, IPPROTO_TCP, TCP_USER_TIMEOUT,
                       &user_timeout_ms, sizeof(user_timeout_ms)) < 0) {
            fprintf(stderr, "[session] setsockopt TCP_USER_TIMEOUT failed\n");
        }
    }
#endif

    // Enable TCP_NODELAY for low-latency interactive sessions
    if (setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &enabled,
                   sizeof(enabled)) < 0) {
        fprintf(stderr, "[session] setsockopt TCP_NODELAY failed\n");
    }
}

// Configure SSH-level session options after accept (timeout, nodelay, rekey).
static void session_configure_ssh_options(ssh_session session)
{
    if (session == nullptr) {
        return;
    }

    // Set SSH operation timeout (matches openssh LoginGraceTime behavior)
    long timeout_seconds = SSH_CHATTER_SSH_TIMEOUT_SECONDS;
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout_seconds);

    // Enable TCP_NODELAY at SSH layer as well
    int nodelay = 1;
    ssh_options_set(session, SSH_OPTIONS_NODELAY, &nodelay);
}

static const char *session_cp437_scope_label(session_cp437_scope_t cp437_scope)
{
    switch (cp437_scope) {
    case SESSION_CP437_SCOPE_SYSTEM_ONLY:
        return "system-only";
    case SESSION_CP437_SCOPE_CHAT_ONLY:
        return "chat-only";
    case SESSION_CP437_SCOPE_ALL:
    default:
        return "all output";
    }
}

static bool session_cp437_scope_parse(const char *token,
                                      session_cp437_scope_t *out_scope)
{
    if (token == nullptr || out_scope == nullptr || token[0] == '\0') {
        return false;
    }

    if (strcasecmp(token, "system") == 0 ||
        strcasecmp(token, "system-only") == 0 ||
        strcasecmp(token, "system_only") == 0) {
        *out_scope = SESSION_CP437_SCOPE_SYSTEM_ONLY;
        return true;
    }

    if (strcasecmp(token, "chat") == 0 || strcasecmp(token, "chat-only") == 0 ||
        strcasecmp(token, "chat_only") == 0) {
        *out_scope = SESSION_CP437_SCOPE_CHAT_ONLY;
        return true;
    }

    if (strcasecmp(token, "all") == 0 || strcasecmp(token, "both") == 0) {
        *out_scope = SESSION_CP437_SCOPE_ALL;
        return true;
    }

    return false;
}

void session_handle_hybrid(session_ctx_t *ctx, const char *arguments)
{
    static const char *kUsage = "Usage: /hybrid <on|off|status>";

    if (ctx == nullptr) {
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    if (arguments != nullptr) {
        snprintf(working, sizeof(working), "%s", arguments);
        trim_whitespace_inplace(working);
    } else {
        working[0] = '\0';
    }

    if (working[0] == '\0' || strcasecmp(working, "status") == 0) {
        session_send_system_line(
            ctx,
            ctx->hybrid_output_mode
                ? "Hybrid encoding detection is enabled. Mixed content will"
                  " stay UTF-8 while retro-safe system output uses legacy"
                  " encoding."
                : "Hybrid encoding detection is disabled. Output encoding"
                  " follows the retro scope as-is.");
        return;
    }

    if (strcasecmp(working, "on") == 0) {
        ctx->hybrid_output_mode = true;
        session_send_system_line(
            ctx, "Hybrid encoding detection enabled for retro output.");
        return;
    }

    if (strcasecmp(working, "off") == 0) {
        ctx->hybrid_output_mode = false;
        session_send_system_line(ctx, "Hybrid encoding detection disabled.");
        return;
    }

    session_send_system_line(ctx, kUsage);
}

void session_handle_saerom(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    ctx->cp437_output_scope = SESSION_CP437_SCOPE_SYSTEM_ONLY;
    ctx->cp437_override = SESSION_CP437_OVERRIDE_FORCE_ON;
    ctx->hybrid_output_mode = true;
    session_refresh_output_encoding(ctx);

    session_send_system_line(
        ctx,
        "Saerom DataMan profile enabled: system output uses CP437 while chat"
        " stays UTF-8 with hybrid detection.");
}

static bool
host_provider_language_preference(host_t *host, const char *provider_label,
                                  session_ui_language_t *out_language)
{
    if (host == nullptr || provider_label == nullptr ||
        provider_label[0] == '\0' || out_language == nullptr) {
        return false;
    }

    size_t counts[SESSION_UI_LANGUAGE_COUNT] = {0};
    size_t total = 0U;

    ttak_mutex_lock(&host->lock);
    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_PREFERENCES; ++idx) {
        const user_preference_t *pref = &host->preferences[idx];
        if (!pref->in_use || pref->ui_language[0] == '\0') {
            continue;
        }

        char resolved_label[SSH_CHATTER_PROVIDER_LABEL_LEN] = {0};
        if (pref->provider_label[0] != '\0') {
            snprintf(resolved_label, sizeof(resolved_label), "%s",
                     pref->provider_label);
        } else if (pref->ip[0] != '\0') {
            (void)session_detect_provider_ip(pref->ip, resolved_label,
                                             sizeof(resolved_label));
        }

        if (resolved_label[0] == '\0' ||
            strcasecmp(resolved_label, provider_label) != 0) {
            continue;
        }

        session_ui_language_t lang =
            session_ui_language_from_code(pref->ui_language);
        if (lang == SESSION_UI_LANGUAGE_COUNT) {
            continue;
        }

        ++counts[(size_t)lang];
        ++total;
    }
    ttak_mutex_unlock(&host->lock);

    if (total < 4U) {
        return false;
    }

    size_t best_count = 0U;
    session_ui_language_t best_language = SESSION_UI_LANGUAGE_COUNT;
    for (size_t idx = 0U; idx < SESSION_UI_LANGUAGE_COUNT; ++idx) {
        if (counts[idx] > best_count) {
            best_count = counts[idx];
            best_language = (session_ui_language_t)idx;
        }
    }

    if (best_language == SESSION_UI_LANGUAGE_COUNT) {
        return false;
    }

    *out_language = best_language;
    return true;
}

static void host_sync_state_resolve_path(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    const char *sync_path = getenv("CHATTER_SYNC_STATE_FILE");
    if (sync_path == nullptr || sync_path[0] == '\0') {
        sync_path = "sync_chatter_state.dat";
    }

    int written = snprintf(host->sync_state_file_path,
                           sizeof(host->sync_state_file_path), "%s", sync_path);
    if (written < 0 || (size_t)written >= sizeof(host->sync_state_file_path)) {
        humanized_log_error("host", "sync state file path is too long",
                            ENAMETOOLONG);
        host->sync_state_file_path[0] = '\0';
    }
}

static void session_destroy(session_ctx_t *ctx);

static size_t session_encode_utf8_codepoint(uint32_t codepoint, char *output,
                                            size_t capacity)
{
    if (output == nullptr || capacity == 0U) {
        return 0U;
    }

    if (codepoint <= 0x7FU) {
        if (capacity < 1U) {
            return 0U;
        }
        output[0] = (char)codepoint;
        return 1U;
    }

    if (codepoint <= 0x7FFU) {
        if (capacity < 2U) {
            return 0U;
        }
        output[0] = (char)(0xC0U | ((codepoint >> 6U) & 0x1FU));
        output[1] = (char)(0x80U | (codepoint & 0x3FU));
        return 2U;
    }

    if (codepoint <= 0xFFFFU) {
        if (capacity < 3U) {
            return 0U;
        }
        output[0] = (char)(0xE0U | ((codepoint >> 12U) & 0x0FU));
        output[1] = (char)(0x80U | ((codepoint >> 6U) & 0x3FU));
        output[2] = (char)(0x80U | (codepoint & 0x3FU));
        return 3U;
    }

    if (codepoint <= 0x10FFFFU) {
        if (capacity < 4U) {
            return 0U;
        }
        output[0] = (char)(0xF0U | ((codepoint >> 18U) & 0x07U));
        output[1] = (char)(0x80U | ((codepoint >> 12U) & 0x3FU));
        output[2] = (char)(0x80U | ((codepoint >> 6U) & 0x3FU));
        output[3] = (char)(0x80U | (codepoint & 0x3FU));
        return 4U;
    }

    if (capacity < 1U) {
        return 0U;
    }
    output[0] = '?';
    return 1U;
}

size_t session_cp437_byte_to_utf8(unsigned char byte, char *output,
                                  size_t capacity)
{
    if (output == nullptr || capacity == 0U) {
        return 0U;
    }

    if (byte < 0x80U) {
        output[0] = (char)byte;
        return 1U;
    }

    static const uint16_t kCp437ToUnicode[128] = {
        0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA,
        0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6,
        0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC,
        0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA,
        0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC,
        0x00A1, 0x00AB, 0x00BB, 0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561,
        0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B,
        0x2510, 0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
        0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567, 0x2568,
        0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518,
        0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580, 0x03B1, 0x00DF, 0x0393,
        0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4,
        0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320,
        0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2,
        0x25A0, 0x00A0,
    };

    const uint32_t codepoint = kCp437ToUnicode[byte - 0x80U];
    size_t produced =
        session_encode_utf8_codepoint(codepoint, output, capacity);
    if (produced == 0U) {
        output[0] = '?';
        return 1U;
    }

    return produced;
}

bool host_user_data_load_existing(host_t *host, const char *username,
                                  const char *ip, user_data_record_t *record,
                                  bool create_if_missing);

static void session_handle_gemini_unfreeze(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(
            ctx, "Only operators may manage Gemini translation.");
        return;
    }

    struct timespec remaining = {0, 0};
    bool cooldown_active = translator_gemini_backoff_remaining(&remaining);
    translator_clear_gemini_backoff();

    if (cooldown_active) {
        session_send_system_line(ctx, "Automatic Gemini cooldown cleared. "
                                      "Translations may resume immediately.");

        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice),
                 "* [%s] cleared the automatic Gemini cooldown.",
                 ctx->user.name);
        host_history_record_system(ctx->owner, notice, nullptr);
        chat_room_broadcast(&ctx->owner->room, notice, nullptr);
    } else {
        session_send_system_line(ctx,
                                 "No automatic Gemini cooldown was active.");
    }
}

static void session_handle_palette(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (arguments == nullptr) {
        session_send_system_line(ctx,
                                 "Usage: /palette <name> (try /palette list)");
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);

    if (working[0] == '\0' || strcasecmp(working, "list") == 0) {
        session_send_system_line(ctx, "Available palettes:");

        /* +1 ensures there is always room for the null terminator after 16
         * entries each up to SSH_CHATTER_MESSAGE_LIMIT bytes long. */
        static char palette_line_group[SSH_CHATTER_MESSAGE_LIMIT * 16 + 1U];
        /* Reset the static buffer at the start of each listing call so that
         * stale data from a previous invocation cannot bleed through. */
        size_t group_len = 0U;
        palette_line_group[0] = '\0';

        for (size_t idx = 0U;
             idx < sizeof(PALETTE_DEFINITIONS) / sizeof(PALETTE_DEFINITIONS[0]);
             ++idx) {
            const palette_descriptor_t *descriptor = &PALETTE_DEFINITIONS[idx];
            const char *name = descriptor->name->en;
            const char *description = descriptor->description->en;
            switch (session_ui_language_current(ctx)) {
            case SESSION_UI_LANGUAGE_KO:
                name = descriptor->name->ko;
                description = descriptor->description->ko;
                break;
            case SESSION_UI_LANGUAGE_JP:
                name = descriptor->name->jp;
                description = descriptor->description->jp;
                break;
            case SESSION_UI_LANGUAGE_ZH:
                name = descriptor->name->zh;
                description = descriptor->description->zh;
                break;
            case SESSION_UI_LANGUAGE_RU:
                name = descriptor->name->ru;
                description = descriptor->description->ru;
                break;
            default:
                break;
            }
            char palette_line[SSH_CHATTER_MESSAGE_LIMIT];
            if (descriptor->is_256_color) {
                snprintf(palette_line, sizeof(palette_line),
                         "\033[1G  %s\033[1G\n   - %s (256-color)\n", name,
                         description);
            } else {
                snprintf(palette_line, sizeof(palette_line),
                         "\033[1G  %s\033[1G\n   - %s\n", name, description);
            }
            /* Use offset-tracked snprintf to avoid strncat truncation warnings
             * and ensure the group buffer is never overflowed. */
            size_t remaining = sizeof(palette_line_group) - group_len;
            if (remaining > 1U) {
                int written = snprintf(palette_line_group + group_len,
                                       remaining, "%s\n", palette_line);
                if (written > 0) {
                    group_len += (size_t)written < remaining
                                     ? (size_t)written
                                     : remaining - 1U;
                }
            }
            if ((idx + 1) % 16 == 0) {
                session_send_system_line(ctx, palette_line_group);
                /* Use sizeof(palette_line_group) — NOT sizeof(integer expression) —
                 * to zero the full buffer, then reset the offset. */
                memset(palette_line_group, 0, sizeof(palette_line_group));
                group_len = 0U;
            }
        }
        /* Flush any remaining entries that did not fill a full 16-entry batch. */
        if (group_len > 0U) {
            session_send_system_line(ctx, palette_line_group);
            palette_line_group[0] = '\0';
            group_len = 0U;
        }
        (void)group_len;
        session_send_system_line(ctx, "Apply a palette with /palette <name>.");
        return;
    }

    const palette_descriptor_t *descriptor = palette_find_descriptor(working);
    if (descriptor == nullptr) {
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(line, sizeof(line),
                 "Unknown palette '%.32s'. Use /palette list to see options.",
                 working);
        session_send_system_line(ctx, line);
        return;
    }

    if (!palette_apply_to_session(ctx, descriptor)) {
        session_send_system_line(ctx,
                                 "Unable to apply that palette right now.");
        return;
    }

    session_apply_background_fill(ctx);

    char info[SSH_CHATTER_MESSAGE_LIMIT];
    const char *name = descriptor->name->en;
    const char *description = descriptor->description->en;
    switch (session_ui_language_current(ctx)) {
    case SESSION_UI_LANGUAGE_KO:
        name = descriptor->name->ko;
        description = descriptor->description->ko;
        break;
    case SESSION_UI_LANGUAGE_JP:
        name = descriptor->name->jp;
        description = descriptor->description->jp;
        break;
    case SESSION_UI_LANGUAGE_ZH:
        name = descriptor->name->zh;
        description = descriptor->description->zh;
        break;
    case SESSION_UI_LANGUAGE_RU:
        name = descriptor->name->ru;
        description = descriptor->description->ru;
        break;
    default:
        break;
    }
    snprintf(info, sizeof(info), "Palette '%s' applied - %s", name,
             description);
    session_send_system_line(ctx, info);
    session_render_separator(ctx, "Chatroom");
    session_render_prompt(ctx, true);

    if (ctx->owner != nullptr) {
        host_store_user_theme(ctx->owner, ctx);
        host_store_system_theme(ctx->owner, ctx);
    }
}

void session_handle_retro(session_ctx_t *ctx, const char *arguments)
{
    static const char *kUsage = "Usage: /retro <on [ko|en|jp|zh|ru|de|fr|pl] "
                                "[system|chat|all]|off|auto|status>";

    if (ctx == nullptr) {
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    if (arguments != nullptr) {
        snprintf(working, sizeof(working), "%s", arguments);
        trim_whitespace_inplace(working);
    } else {
        working[0] = '\0';
    }

    if (working[0] == '\0' || strcasecmp(working, "status") == 0) {
        const char *mode = "automatic";
        if (ctx->cp437_override == SESSION_CP437_OVERRIDE_FORCE_ON) {
            mode = "forced on";
        } else if (ctx->cp437_override == SESSION_CP437_OVERRIDE_FORCE_OFF) {
            mode = "forced off";
        }

        const char *codepage_name = session_codepage_name(ctx->active_codepage);
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        char output_description[64];
        if (ctx->prefer_cp437_output) {
            switch (ctx->cp437_output_scope) {
            case SESSION_CP437_SCOPE_SYSTEM_ONLY:
                snprintf(output_description, sizeof(output_description),
                         "legacy (system-only)");
                break;
            case SESSION_CP437_SCOPE_CHAT_ONLY:
                snprintf(output_description, sizeof(output_description),
                         "legacy (chat-only)");
                break;
            case SESSION_CP437_SCOPE_ALL:
            default:
                snprintf(output_description, sizeof(output_description),
                         "legacy");
                break;
            }
        } else {
            snprintf(output_description, sizeof(output_description),
                     "UTF-8 only");
        }

        const char *scope_label =
            session_cp437_scope_label(ctx->cp437_output_scope);
        snprintf(message, sizeof(message),
                 "Retro encoding mode: %s (scope: %s, codepage: %s, input: %s, "
                 "output: %s).",
                 mode, scope_label, codepage_name,
                 ctx->cp437_input_enabled ? "legacy" : "UTF-8",
                 output_description);
        session_send_system_line(ctx, message);
        snprintf(message, sizeof(message), "Hybrid detection: %s.",
                 ctx->hybrid_output_mode ? "enabled" : "disabled");
        session_send_system_line(ctx, message);
        session_send_system_line(
            ctx, "Toggle with /retro on [lang], /retro off, or /retro auto.");
        session_send_system_line(
            ctx, "Supported languages: ko, en, jp, zh, ru, de, fr, pl.");
        if (ctx->cp437_override == SESSION_CP437_OVERRIDE_NONE) {
            session_send_system_line(
                ctx, "Hybrid retro/Unicode auto-conversion is active for all "
                     "languages when automatic detection is enabled.");
        }
        return;
    }

    // Handle "on" with optional language parameter
    if (strncasecmp(working, "on", 2) == 0) {
        const char *lang_arg = working + 2;
        while (*lang_arg == ' ' || *lang_arg == '\t') {
            ++lang_arg;
        }

        char first_token[16] = {0};
        char second_token[16] = {0};
        int token_count =
            sscanf(lang_arg, "%15s %15s", first_token, second_token);

        const char *lang_token = nullptr;
        const char *scope_token = nullptr;
        session_cp437_scope_t requested_scope = ctx->cp437_output_scope;

        if (token_count >= 1) {
            session_ui_language_t token_language =
                session_ui_language_from_code(first_token);
            if (token_language != SESSION_UI_LANGUAGE_COUNT ||
                strcasecmp(first_token, "en") == 0) {
                lang_token = first_token;
                if (token_count >= 2) {
                    scope_token = second_token;
                }
            } else {
                scope_token = first_token;
                if (token_count >= 2) {
                    lang_token = second_token;
                }
            }
        }

        // If language is specified, set UI language
        if (lang_token != nullptr && lang_token[0] != '\0') {
            // Try to parse language code
            session_ui_language_t new_lang =
                session_ui_language_from_code(lang_token);
            if (new_lang != SESSION_UI_LANGUAGE_EN ||
                strcasecmp(lang_token, "en") == 0) {
                ctx->ui_language = new_lang;
                /* Set the appropriate code page for the language */
                ctx->active_codepage = session_codepage_for_language(new_lang);
                if (ctx->owner != nullptr) {
                    host_store_ui_language(ctx->owner, ctx);
                }
            }
        } else {
            /* No language specified, use code page for current UI language */
            ctx->active_codepage =
                session_codepage_for_language(ctx->ui_language);
        }

        if (scope_token != nullptr && scope_token[0] != '\0') {
            session_cp437_scope_t parsed_scope = requested_scope;
            if (!session_cp437_scope_parse(scope_token, &parsed_scope)) {
                session_send_system_line(
                    ctx, "Scope must be one of: all, system, or chat.");
                return;
            }
            requested_scope = parsed_scope;
        }

        ctx->cp437_output_scope = requested_scope;

        ctx->cp437_override = SESSION_CP437_OVERRIDE_FORCE_ON;
        ctx->hybrid_output_mode = true;
        session_refresh_output_encoding(ctx);

        char message[SSH_CHATTER_MESSAGE_LIMIT];
        const char *codepage_name = session_codepage_name(ctx->active_codepage);
        const char *scope_label = session_cp437_scope_label(requested_scope);
        if (lang_token != nullptr && lang_token[0] != '\0') {
            snprintf(message, sizeof(message),
                     "Retro encoding enabled with language %s (%s) for %s. "
                     "Legacy code page input and output are forced on.",
                     lang_token, codepage_name, scope_label);
        } else {
            snprintf(message, sizeof(message),
                     "Retro encoding enabled (%s) for %s. "
                     "Legacy code page input and output are forced on.",
                     codepage_name, scope_label);
        }
        session_send_system_line(ctx, message);
        return;
    }

    if (strcasecmp(working, "off") == 0) {
        ctx->cp437_override = SESSION_CP437_OVERRIDE_FORCE_OFF;
        session_refresh_output_encoding(ctx);
        session_send_system_line(
            ctx,
            "Retro encoding disabled. CP437 input and output are forced off.");
        return;
    }

    if (strcasecmp(working, "auto") == 0) {
        ctx->cp437_override = SESSION_CP437_OVERRIDE_NONE;
        session_refresh_output_encoding(ctx);
        session_send_system_line(
            ctx, "Retro encoding returned to automatic detection.");
        return;
    }

    session_send_system_line(ctx, kUsage);
}

static void session_handle_ai_chat(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(ctx,
                                 "Only operators may control ai-eliza.");
        return;
    }

    char token[32];
    if (arguments != nullptr) {
        snprintf(token, sizeof(token), "%s", arguments);
        trim_whitespace_inplace(token);
    } else {
        token[0] = '\0';
    }

    if (token[0] == '\0') {
        bool enabled = atomic_load(&ctx->owner->ai_chat_enabled);
        char status[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(status, sizeof(status), "ai-eliza is currently %s.",
                 enabled ? "enabled" : "disabled");
        session_send_system_line(ctx, status);
        session_send_system_line(ctx, "Usage: /ai-chat <on|off>");
        session_send_system_line(
            ctx, "When enabled, mention \"ai-eliza\" in chat to start a "
                 "conversation.");
        return;
    }

    bool requested_enable = false;
    bool recognized = false;
    if (session_argument_is_disable(token)) {
        recognized = true;
        requested_enable = false;
    } else {
        recognized = parse_bool_token(token, &requested_enable);
    }

    if (!recognized) {
        session_send_system_line(ctx, "Usage: /ai-chat <on|off>");
        return;
    }

    if (requested_enable) {
        if (host_ai_chat_enable(ctx->owner)) {
            session_send_system_line(ctx,
                                     "ai-eliza is now active for casual chat.");
        } else {
            session_send_system_line(ctx, "ai-eliza is already chatting.");
        }
        return;
    }

    if (host_ai_chat_disable(ctx->owner)) {
        session_send_system_line(ctx, "ai-eliza has been muted.");
    } else {
        session_send_system_line(ctx, "ai-eliza is already inactive.");
    }
}

static void session_handle_ollama_model(session_ctx_t *ctx,
                                        const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(
            ctx, "Only operators may configure the Ollama model.");
        return;
    }

    char working[sizeof(ctx->owner->ai_chat_model)];
    if (arguments != nullptr) {
        snprintf(working, sizeof(working), "%s", arguments);
        trim_whitespace_inplace(working);
    } else {
        working[0] = '\0';
    }

    host_t *host = ctx->owner;
    if (working[0] == '\0') {
        char model[sizeof(host->ai_chat_model)];
        host_ai_chat_snapshot_state(host, model, sizeof(model), nullptr);
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "Current Ollama model: %s%s.",
                 model,
                 (strcasecmp(model, host_ai_chat_default_model()) == 0)
                     ? " (default)"
                     : "");
        session_send_system_line(ctx, message);
        session_send_system_line(ctx, "Usage: /ollama-model <model_name>");
        return;
    }

    size_t length = strlen(working);
    if (length >= sizeof(host->ai_chat_model)) {
        session_send_system_line(ctx, "Model name is too long.");
        return;
    }

    ttak_mutex_lock(&host->lock);
    snprintf(host->ai_chat_model, sizeof(host->ai_chat_model), "%s", working);
    ttak_mutex_unlock(&host->lock);

    char message[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(message, sizeof(message),
             "Ollama model updated to '%s'. ai-eliza will use it on the next "
             "reply.",
             working);
    session_send_system_line(ctx, message);
}

static bool find_reserved_names(session_ctx_t *ctx, const char *nick)
{
    if (ctx == nullptr || ctx->owner == nullptr || nick == nullptr ||
        nick[0] == '\0') {
        return false;
    }

    bool found = false;
    ttak_mutex_lock(&ctx->owner->nickname_reserve_lock);
    if (ctx->owner->reserved_nicknames == nullptr) {
        ttak_mutex_unlock(&ctx->owner->nickname_reserve_lock);
        return false;
    }
    for (size_t i = 0; i < ctx->owner->reserved_nicknames_len; ++i) {
        if (strcasecmp(nick, ctx->owner->reserved_nicknames[i]) == 0) {
            found = true;
            break;
        }
    }
    ttak_mutex_unlock(&ctx->owner->nickname_reserve_lock);
    return found;
}

bool host_username_has_password(host_t *host, const char *nick)
{
    if (host == nullptr || nick == nullptr || nick[0] == '\0') {
        return false;
    }

    user_data_record_t record;
    if (host_user_data_load_existing(host, nick, nullptr, &record, false) &&
        !security_layer_is_zero_hash(record.password_hash,
                                     sizeof(record.password_hash))) {
        return true;
    }

    if (host->pw_auth_file_path[0] == '\0') {
        return false;
    }

    FILE *fp = fopen(host->pw_auth_file_path, "rb");
    if (fp == nullptr) {
        return false;
    }

    bool protected_name = false;
    char line[SSH_CHATTER_MESSAGE_LIMIT];
    while (!protected_name && fgets(line, sizeof(line), fp) != nullptr) {
        size_t length = strcspn(line, "\r\n");
        line[length] = '\0';

        char *first_separator = strchr(line, ':');
        if (first_separator == nullptr) {
            continue;
        }

        size_t name_length = (size_t)(first_separator - line);
        if (name_length == 0U) {
            continue;
        }

        char existing[SSH_CHATTER_USERNAME_LEN];
        if (name_length >= sizeof(existing)) {
            name_length = sizeof(existing) - 1U;
        }

        memcpy(existing, line, name_length);
        existing[name_length] = '\0';

        if (strcasecmp(existing, nick) == 0) {
            protected_name = true;
        }
    }

    int read_error = ferror(fp);
    fclose(fp);

    if (read_error != 0) {
        return false;
    }

    return protected_name;
}

static void session_handle_nick(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (arguments == nullptr || *arguments == '\0') {
        session_send_system_line(ctx, "Usage: /nick <name>");
        return;
    }

    char new_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(new_name, sizeof(new_name), "%s", arguments);
    trim_whitespace_inplace(new_name);

    if (new_name[0] == '\0') {
        session_send_system_line(ctx, "Usage: /nick <name>");
        return;
    }

    if (ctx->user.is_lan_operator && strcmp(new_name, ctx->user.name) != 0) {
        session_send_system_line(ctx, "LAN operator nicknames are fixed.");
        return;
    }

    mbstate_t state;
    memset(&state, 0, sizeof(state));
    const char *cursor = new_name;
    while (*cursor != '\0') {
        wchar_t wc;
        size_t consumed = mbrtowc(&wc, cursor, (size_t)-1, &state);
        if (consumed == (size_t)-1 || consumed == (size_t)-2) {
            // Invalid UTF-8 sequence
            session_send_system_line(
                ctx, "Names may not contain invalid UTF-8 sequences.");
            return;
        }
        if (consumed ==
            0) { // Should not happen with valid UTF-8, but as a safeguard
            cursor++;
            continue;
        }

        // Check if the character is a control character, DEL, space, or tab
        if (wc < 0x20 || wc == 0x7F || wc == L' ' || wc == L'\t') {
            session_send_system_line(ctx, "Names may not include control "
                                          "characters, DEL, space, or tab.");
            return;
        }
        cursor += consumed;
    }

    if (host_is_username_banned(ctx->owner, new_name)) {
        session_send_system_line(
            ctx, "That nickname is blocked for bot detection. Choose another.");
        return;
    }

    if (find_reserved_names(ctx, new_name)) {
        session_send_system_line(ctx, "That name is reserved.");
        return;
    }

    if (host_username_reserved(ctx->owner, new_name) &&
        !ctx->user.is_lan_operator) {
        session_send_system_line(ctx,
                                 "That name is reserved for LAN operators.");
        return;
    }

    if (!ctx->user_data_loaded) {
        (void)session_user_data_load(ctx);
    }

    bool owns_requested_name = strcasecmp(ctx->user.name, new_name) == 0;
    if (!owns_requested_name && ctx->user_data_loaded) {
        owns_requested_name =
            strcasecmp(ctx->user_data.username, new_name) == 0;
    }

    if (!owns_requested_name &&
        host_username_has_password(ctx->owner, new_name)) {
        session_send_system_line(
            ctx, "That nickname is password-protected. Log in as that user.");
        return;
    }

    session_ctx_t *existing = chat_room_find_user(&ctx->owner->room, new_name);
    if (existing != nullptr && existing != ctx) {
        session_send_system_line(ctx, "That name is already taken.");
        return;
    }

    char old_name[SSH_CHATTER_USERNAME_LEN];
    snprintf(old_name, sizeof(old_name), "%s", ctx->user.name);
    snprintf(ctx->user.name, sizeof(ctx->user.name), "%s", new_name);

    if (ctx->user_data_loaded) {
        snprintf(ctx->user_data.preferred_nickname,
                 sizeof(ctx->user_data.preferred_nickname), "%s", new_name);
        if (ctx->owner != NULL && ctx->owner->user_data_root[0] != '\0') {
            user_data_save(ctx->owner->user_data_root, &ctx->user_data,
                           ctx->client_ip);
        }
    }

    char announcement[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(announcement, sizeof(announcement), "* [%s] is now known as [%s]",
             old_name, ctx->user.name);
    host_history_record_system(ctx->owner, announcement, NULL);
    chat_room_broadcast(&ctx->owner->room, announcement, NULL);
    session_apply_saved_preferences(ctx);
    session_send_system_line(ctx, "Display name updated.");
}

static void session_force_disconnect(session_ctx_t *ctx, const char *reason)
{
    if (ctx == nullptr) {
        return;
    }

    if (!ctx->exit_notice_sent && reason != nullptr && reason[0] != '\0') {
        session_send_system_line(ctx, reason);
        ctx->exit_notice_sent = true;
    }

    ctx->should_exit = true;
    ctx->exit_status = EXIT_FAILURE;

    if (ctx->translation_mutex_initialized) {
        ttak_mutex_lock(&ctx->translation_mutex);
        ctx->translation_thread_stop = true;
        ttak_cond_broadcast(&ctx->translation_cond);
        ttak_mutex_unlock(&ctx->translation_mutex);
    }
    session_translation_clear_queue(ctx);

    session_transport_request_close(ctx);
}

static void session_handle_exit(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    session_force_disconnect(ctx, "Disconnecting... bye!");
    ctx->exit_status = EXIT_SUCCESS;
}

static void session_handle_pardon(session_ctx_t *ctx, const char *arguments)
{
    if (!ctx->user.is_operator) {
        session_send_system_line(ctx, "You are not allowed to pardon users.");
        return;
    }

    if (arguments == nullptr || *arguments == '\0') {
        session_send_system_line(ctx, "Usage: /pardon <user|ip>");
        return;
    }

    char token[SSH_CHATTER_IP_LEN];
    snprintf(token, sizeof(token), "%s", arguments);
    trim_whitespace_inplace(token);

    if (token[0] == '\0') {
        session_send_system_line(ctx, "Usage: /pardon <user|ip>");
        return;
    }

    if (host_remove_ban_entry(ctx->owner, token)) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "Ban lifted for '%s'.", token);
        session_send_system_line(ctx, message);
    } else {
        session_send_system_line(ctx, "No matching ban found.");
    }
}

static session_ctx_t *chat_room_find_user(chat_room_t *room,
                                          const char *username)
{
    if (room == nullptr || username == nullptr) {
        return nullptr;
    }

    session_ctx_t *result = nullptr;
    ttak_mutex_lock(&room->lock);
    for (size_t idx = 0; idx < room->member_count; ++idx) {
        session_ctx_t *member = room->members[idx];
        if (member == nullptr) {
            continue;
        }

        if (strncmp(member->user.name, username, SSH_CHATTER_USERNAME_LEN) ==
            0) {
            result = member;
            break;
        }
    }
    ttak_mutex_unlock(&room->lock);

    return result;
}

static bool host_username_reserved(host_t *host, const char *username)
{
    if (host == nullptr || username == nullptr || username[0] == '\0') {
        return false;
    }

    // Check if it's a LAN operator username
    if (host_is_lan_operator_username(host, username)) {
        return true;
    }

    return false;
}

static join_activity_entry_t *host_find_join_activity_locked(host_t *host,
                                                             const char *ip)
{
    if (host == nullptr || ip == nullptr) {
        return nullptr;
    }

    for (size_t idx = 0; idx < host->join_activity_count; ++idx) {
        join_activity_entry_t *entry = &host->join_activity[idx];
        if (strncmp(entry->ip, ip, SSH_CHATTER_IP_LEN) == 0) {
            return entry;
        }
    }

    return nullptr;
}

static void
host_prune_join_activity_locked(host_t *host,
                                const struct timespec *reference_time)
{
    if (host == nullptr) {
        return;
    }

    sshc_memory_context_t *memory_scope = host_memory_scope_push(host);

    if (host->join_activity == nullptr || host->join_activity_count == 0U) {
        host_memory_scope_pop(memory_scope);
        return;
    }

    struct timespec now = {0, 0};
    if (reference_time != nullptr &&
        (reference_time->tv_sec != 0 || reference_time->tv_nsec != 0)) {
        now = *reference_time;
    } else {
        clock_gettime(CLOCK_MONOTONIC, &now);
    }

    size_t idx = 0U;
    while (idx < host->join_activity_count) {
        join_activity_entry_t *entry = &host->join_activity[idx];
        bool remove_entry = false;

        if (entry->last_attempt.tv_sec == 0 &&
            entry->last_attempt.tv_nsec == 0) {
            remove_entry = true;
        } else {
            struct timespec age = timespec_diff(&now, &entry->last_attempt);
            long long age_ns = timespec_to_ns(&age);
            if (age_ns >= SSH_CHATTER_JOIN_ACTIVITY_RETENTION_NS) {
                remove_entry = true;
            } else if (entry->asciiart_has_cooldown) {
                struct timespec ascii_age =
                    timespec_diff(&now, &entry->last_asciiart_post);
                long long ascii_age_ns = timespec_to_ns(&ascii_age);
                if (ascii_age_ns >= SSH_CHATTER_JOIN_ACTIVITY_RETENTION_NS) {
                    entry->asciiart_has_cooldown = false;
                }
            }
        }

        if (remove_entry) {
            size_t last_index = host->join_activity_count - 1U;
            if (idx != last_index) {
                host->join_activity[idx] = host->join_activity[last_index];
            }
            memset(&host->join_activity[last_index], 0,
                   sizeof(host->join_activity[last_index]));
            host->join_activity_count = last_index;
            continue;
        }

        ++idx;
    }

    if (host->join_activity_count == 0U) {
        sshc_gc_free(host->join_activity);
        host->join_activity = nullptr;
        host->join_activity_capacity = 0U;
    } else if (host->join_activity_capacity > 8U &&
               host->join_activity_count <= host->join_activity_capacity / 2U) {
        size_t new_capacity = host->join_activity_capacity / 2U;
        if (new_capacity < 8U) {
            new_capacity = 8U;
        }
        if (new_capacity < host->join_activity_count) {
            new_capacity = host->join_activity_count;
        }
        join_activity_entry_t *resized = (join_activity_entry_t *)sshc_gc_realloc(
            host->join_activity, new_capacity * sizeof(*resized));
        if (resized != nullptr) {
            host->join_activity = resized;
            host->join_activity_capacity = new_capacity;
        }
    }

    host_memory_scope_pop(memory_scope);
}

static join_activity_entry_t *host_ensure_join_activity_locked(host_t *host,
                                                               const char *ip)
{
    if (host == nullptr || ip == nullptr || ip[0] == '\0') {
        return nullptr;
    }

    sshc_memory_context_t *memory_scope = host_memory_scope_push(host);

    join_activity_entry_t *entry = host_find_join_activity_locked(host, ip);
    if (entry != nullptr) {
        host_memory_scope_pop(memory_scope);
        return entry;
    }

    if (host->join_activity_count >= host->join_activity_capacity) {
        size_t new_capacity = host->join_activity_capacity > 0U
                                  ? host->join_activity_capacity * 2U
                                  : 8U;
        join_activity_entry_t *resized = sshc_gc_realloc(
            host->join_activity, new_capacity * sizeof(join_activity_entry_t));
        if (resized == nullptr) {
            host_memory_scope_pop(memory_scope);
            return nullptr;
        }
        host->join_activity = resized;
        host->join_activity_capacity = new_capacity;
    }

    entry = &host->join_activity[host->join_activity_count++];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->ip, sizeof(entry->ip), "%s", ip);
    host_memory_scope_pop(memory_scope);
    return entry;
}

static size_t host_prepare_join_delay(host_t *host,
                                      struct timespec *wait_duration)
{
    struct timespec wait = {0, 0};
    if (host == nullptr) {
        if (wait_duration != nullptr) {
            *wait_duration = wait;
        }
        return 1U;
    }

    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);

    ttak_mutex_lock(&host->lock);
    if (!host->join_throttle_initialised) {
        host->next_join_ready_time = now;
        host->join_throttle_initialised = true;
        host->join_progress_length = 0U;
    }

    if (timespec_compare(&now, &host->next_join_ready_time) < 0) {
        wait = timespec_diff(&host->next_join_ready_time, &now);
    }

    struct timespec base = now;
    if (timespec_compare(&host->next_join_ready_time, &now) > 0) {
        base = host->next_join_ready_time;
    }
    host->next_join_ready_time = timespec_add_ms(&base, 100);
    host->join_progress_length =
        (host->join_progress_length % SSH_CHATTER_JOIN_BAR_MAX) + 1U;
    size_t progress = host->join_progress_length;
    ttak_mutex_unlock(&host->lock);

    if (wait_duration != nullptr) {
        *wait_duration = wait;
    }
    return progress;
}

static host_join_attempt_result_t
host_register_join_attempt(host_t *host, const char *username, const char *ip)
{
    if (host == nullptr || ip == nullptr || ip[0] == '\0') {
        return HOST_JOIN_ATTEMPT_OK;
    }

    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);

    bool exempt_ip = false;
    bool kick_ip = false;

    ttak_mutex_lock(&host->lock);
    host_prune_join_activity_locked(host, &now);
    join_activity_entry_t *entry = host_ensure_join_activity_locked(host, ip);
    if (entry == nullptr) {
        ttak_mutex_unlock(&host->lock);
        return HOST_JOIN_ATTEMPT_OK;
    }

    struct timespec diff = timespec_diff(&now, &entry->last_attempt);
    const long long diff_ns =
        (long long)diff.tv_sec * 1000000000LL + (long long)diff.tv_nsec;
    const bool has_prior_attempt =
        (entry->last_attempt.tv_sec != 0 || entry->last_attempt.tv_nsec != 0);
    const bool within_window =
        has_prior_attempt && diff_ns <= SSH_CHATTER_JOIN_RAPID_WINDOW_NS;

    if (within_window) {
        entry->rapid_attempts += 1U;
    } else {
        entry->rapid_attempts = 1U;
    }

    if (username != nullptr && username[0] != '\0') {
        if (within_window && strncmp(entry->last_username, username,
                                     SSH_CHATTER_USERNAME_LEN) == 0) {
            entry->same_name_attempts += 1U;
        } else {
            entry->same_name_attempts = 1U;
            snprintf(entry->last_username, sizeof(entry->last_username), "%s",
                     username);
        }
    } else {
        entry->same_name_attempts =
            within_window ? entry->same_name_attempts + 1U : 1U;
    }

    const bool has_join_window = entry->join_window_start.tv_sec != 0 ||
                                 entry->join_window_start.tv_nsec != 0;
    if (!has_join_window) {
        entry->join_window_start = now;
        entry->join_window_attempts = 1U;
    } else {
        struct timespec window_diff =
            timespec_diff(&now, &entry->join_window_start);
        const long long window_ns =
            (long long)window_diff.tv_sec * 1000000000LL +
            (long long)window_diff.tv_nsec;
        if (window_ns > SSH_CHATTER_JOIN_KICK_WINDOW_NS) {
            entry->join_window_start = now;
            entry->join_window_attempts = 1U;
        } else {
            if (entry->join_window_attempts < SIZE_MAX) {
                entry->join_window_attempts += 1U;
            }
        }
    }

    entry->last_attempt = now;

    if (host_ip_has_grant_locked(host, ip)) {
        exempt_ip = true;
    }

    if (!exempt_ip &&
        entry->join_window_attempts >= SSH_CHATTER_JOIN_KICK_THRESHOLD) {
        kick_ip = true;
    }
    ttak_mutex_unlock(&host->lock);

    if (!exempt_ip && kick_ip) {
        printf("[auto-kick] %s exceeded join limit\n", ip);
        return HOST_JOIN_ATTEMPT_KICK;
    }

    return HOST_JOIN_ATTEMPT_OK;
}

static bool host_register_suspicious_activity(host_t *host,
                                              const char *username,
                                              const char *ip,
                                              size_t *attempts_out)
{
    if (host == nullptr || ip == nullptr || ip[0] == '\0') {
        if (attempts_out != nullptr) {
            *attempts_out = 0U;
        }
        return false;
    }

    (void)username;

    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);

    size_t attempts = 0U;
    ttak_mutex_lock(&host->lock);
    join_activity_entry_t *entry = host_ensure_join_activity_locked(host, ip);
    if (entry != nullptr) {
        if (entry->last_suspicious.tv_sec != 0 ||
            entry->last_suspicious.tv_nsec != 0) {
            struct timespec diff = timespec_diff(&now, &entry->last_suspicious);
            long long diff_ns =
                (long long)diff.tv_sec * 1000000000LL + (long long)diff.tv_nsec;
            if (diff_ns > SSH_CHATTER_SUSPICIOUS_EVENT_WINDOW_NS) {
                entry->suspicious_events = 0U;
            }
        }

        if (entry->suspicious_events < SSH_CHATTER_SUSPICIOUS_EVENT_THRESHOLD) {
            entry->suspicious_events += 1U;
        } else {
            entry->suspicious_events = SSH_CHATTER_SUSPICIOUS_EVENT_THRESHOLD;
        }
        entry->last_suspicious = now;
        attempts = entry->suspicious_events;
    }
    ttak_mutex_unlock(&host->lock);

    if (attempts_out != nullptr) {
        *attempts_out = attempts;
    }

    return false;
}

static bool host_is_ip_banned(host_t *host, const char *ip)
{
    if (host == nullptr || ip == nullptr || ip[0] == '\0') {
        return false;
    }

    bool banned = false;
    ttak_mutex_lock(&host->lock);
    if (host_is_protected_ip_unlocked(host, ip)) {
        ttak_mutex_unlock(&host->lock);
        return false;
    }
    for (size_t idx = 0; idx < host->ban_count; ++idx) {
        const char *ban_ip = host->bans[idx].ip;
        if (ban_ip[0] == '\0') {
            continue;
        }

        if (host_is_protected_ip_unlocked(host, ban_ip)) {
            continue;
        }

        if (strchr(ban_ip, '/') != nullptr) {
            if (host_cidr_contains_ip(ban_ip, ip)) {
                banned = true;
                break;
            }
            continue;
        }

        if (strncmp(ban_ip, ip, SSH_CHATTER_IP_LEN) == 0) {
            banned = true;
            break;
        }
    }
    ttak_mutex_unlock(&host->lock);

    return banned;
}

static bool host_is_username_banned(host_t *host, const char *username)
{
    if (host == nullptr || username == nullptr || username[0] == '\0') {
        return false;
    }

    bool banned = false;
    ttak_mutex_lock(&host->lock);
    for (size_t idx = 0; idx < host->ban_count; ++idx) {
        if (strncmp(host->bans[idx].username, username,
                    SSH_CHATTER_USERNAME_LEN) == 0) {
            banned = true;
            break;
        }
    }
    ttak_mutex_unlock(&host->lock);

    return banned;
}

static bool host_add_ban_entry(host_t *host, const char *username,
                               const char *ip)
{
    if (host == nullptr) {
        return false;
    }

    bool added = false;
    ttak_mutex_lock(&host->lock);
    if (host->ban_count >= SSH_CHATTER_MAX_BANS) {
        ttak_mutex_unlock(&host->lock);
        return false;
    }

    if (ip != nullptr && ip[0] != '\0' &&
        host_is_protected_ip_unlocked(host, ip)) {
        ttak_mutex_unlock(&host->lock);
        return true;
    }
    if (ip != nullptr && ip[0] != '\0' && strchr(ip, '/') != nullptr) {
        for (size_t idx = 0; idx < host->protected_ip_count &&
                             idx < SSH_CHATTER_MAX_PROTECTED_IPS;
             ++idx) {
            if (host_cidr_contains_ip(ip, host->protected_ips[idx])) {
                ttak_mutex_unlock(&host->lock);
                return true;
            }
        }
    }

    for (size_t idx = 0; idx < host->ban_count; ++idx) {
        const bool username_match =
            (username != nullptr && username[0] != '\0' &&
             strncmp(host->bans[idx].username, username,
                     SSH_CHATTER_USERNAME_LEN) == 0);
        const bool ip_match =
            (ip != nullptr && ip[0] != '\0' &&
             strncmp(host->bans[idx].ip, ip, SSH_CHATTER_IP_LEN) == 0);
        if (username_match || ip_match) {
            ttak_mutex_unlock(&host->lock);
            return true;
        }
    }

    strncpy(host->bans[host->ban_count].username,
            username != nullptr ? username : "", SSH_CHATTER_USERNAME_LEN - 1U);
    host->bans[host->ban_count].username[SSH_CHATTER_USERNAME_LEN - 1U] = '\0';
    strncpy(host->bans[host->ban_count].ip, ip != nullptr ? ip : "",
            SSH_CHATTER_IP_LEN - 1U);
    host->bans[host->ban_count].ip[SSH_CHATTER_IP_LEN - 1U] = '\0';
    ++host->ban_count;
    added = true;

    host_ban_state_save_locked(host);

    ttak_mutex_unlock(&host->lock);
    return added;
}

static bool host_remove_ban_entry(host_t *host, const char *token)
{
    if (host == nullptr || token == nullptr || token[0] == '\0') {
        return false;
    }

    bool removed = false;
    ttak_mutex_lock(&host->lock);
    for (size_t idx = 0; idx < host->ban_count; ++idx) {
        if (strncmp(host->bans[idx].username, token,
                    SSH_CHATTER_USERNAME_LEN) == 0 ||
            strncmp(host->bans[idx].ip, token, SSH_CHATTER_IP_LEN) == 0) {
            for (size_t shift = idx; shift + 1U < host->ban_count; ++shift) {
                host->bans[shift] = host->bans[shift + 1U];
            }
            memset(&host->bans[host->ban_count - 1U], 0,
                   sizeof(host->bans[host->ban_count - 1U]));
            --host->ban_count;
            removed = true;
            host_ban_state_save_locked(host);
            break;
        }
    }
    ttak_mutex_unlock(&host->lock);

    return removed;
}

static bool
session_parse_localized_command(session_ctx_t *ctx,
                                const session_command_alias_t *alias,
                                const char *line, const char **arguments)
{
    if (alias == nullptr) {
        return false;
    }

    if (session_parse_command(line, alias->canonical, arguments)) {
        return true;
    }

    session_ui_language_t language = session_ui_language_current(ctx);
    const char *preferred = session_command_alias_for_language(alias, language);
    if (preferred != nullptr && strcmp(preferred, alias->canonical) != 0) {
        if (session_parse_command(line, preferred, arguments)) {
            return true;
        }
    }

    for (size_t idx = 0; idx < SESSION_UI_LANGUAGE_COUNT; ++idx) {
        const char *localized = alias->localized[idx];
        if (localized == nullptr || localized[0] == '\0' ||
            strcmp(localized, alias->canonical) == 0) {
            continue;
        }
        if (session_parse_command(line, localized, arguments)) {
            return true;
        }
    }

    return false;
}

static bool session_parse_command(const char *line, const char *command,
                                  const char **arguments)
{
    if (line == nullptr || command == nullptr) {
        return false;
    }
    size_t command_len = strlen(command);

    if (strncmp(line, command, command_len) == 0) {
        const char boundary = line[command_len];
        if (boundary != '\0' && boundary != ' ' && boundary != '\t') {
            return false;
        }

        const char *args = line + command_len;

        while (*args == ' ' || *args == '\t') {
            ++args;
        }

        *arguments = args;
        return true;
    }
    return false;
}

static void session_handle_set_lf(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (arguments == nullptr || arguments[0] == '\0') {
        session_send_system_line(ctx, "Usage: /set-lf <auto|lf|crlf>");
        return;
    }

    char mode[16];
    const char *unused = session_consume_token(arguments, mode, sizeof(mode));
    (void)unused;

    if (strcasecmp(mode, "auto") == 0) {
        ctx->newline_mode = SESSION_NEWLINE_MODE_AUTO;
        session_send_system_line(ctx, "Line ending mode set to auto.");
        return;
    }
    if (strcasecmp(mode, "lf") == 0) {
        ctx->newline_mode = SESSION_NEWLINE_MODE_LF;
        session_send_system_line(ctx, "Line ending mode set to LF.");
        return;
    }
    if (strcasecmp(mode, "crlf") == 0) {
        ctx->newline_mode = SESSION_NEWLINE_MODE_CRLF;
        session_send_system_line(ctx, "Line ending mode set to CRLF.");
        return;
    }

    session_send_system_line(ctx, "Usage: /set-lf <auto|lf|crlf>");
}

static void session_dispatch_command(session_ctx_t *ctx, const char *line)
{
    if (ctx == nullptr || line == nullptr) {
        return;
    }

    const char *args = nullptr;
    const char *effective_line = line;

    // Handle double slash commands by effectively removing the first slash
    if (line != nullptr && line[0] == '/' && line[1] == '/') {
        effective_line = line + 1;
    }

    if (session_parse_command_any(ctx, "/help", effective_line, &args)) {
        session_print_help(ctx);
        return;
    }

    else if (session_parse_command_any(ctx, "/advanced", effective_line,
                                       &args)) {
        session_handle_advanced(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/sync-trigger", effective_line,
                                       &args)) {
        return;
    }

    else if (session_parse_command_any(ctx, "/filestore", effective_line,
                                       &args)) {
        session_handle_filestore(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/filestore-upload",
                                       effective_line, &args)) {
        session_handle_filestore_upload(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/filestore-download",
                                       effective_line, &args)) {
        session_handle_filestore_download(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/grant", effective_line, &args)) {
        session_handle_grant(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/shell", effective_line, &args)) {
        session_handle_shell(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/kick", effective_line, &args)) {
        session_handle_kick(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/set-sync-url", effective_line,
                                       &args)) {
        if (args == nullptr || *args == '\0') {
            session_send_system_line(ctx, "Usage: /set-sync-url <host> <port>");
            return;
        }

        char host_str[256];
        char port_str[16];
        const char *remaining_args =
            session_consume_token(args, host_str, sizeof(host_str));
        remaining_args =
            session_consume_token(remaining_args, port_str, sizeof(port_str));

        if (host_str[0] == '\0' || port_str[0] == '\0') {
            session_send_system_line(ctx, "Usage: /set-sync-url <host> <port>");
            return;
        }

        char *endptr;
        long port_long = strtol(port_str, &endptr, 10);
        if (*endptr != '\0' || port_long <= 0 || port_long > 65535) {
            session_send_system_line(
                ctx, "Invalid port number. Port must be between 1 and 65535.");
            return;
        }

        // For now, using default username and password. This can be extended later.
        ssh_chatter_sync_set_connection_details(host_str, (int)port_long,
                                                "chatter_sync", "password");
        ssh_chatter_sync_manual_trigger();
        session_send_system_line(
            ctx, "SSH sync URL updated. Attempting to reconnect...");
        return;
    }

    else if (session_parse_command_any(ctx, "/set-sync-url", effective_line,
                                       &args)) {
        if (args == nullptr || *args == '\0') {
            session_send_system_line(ctx, "Usage: /set-sync-url <host> <port>");
            return;
        }

        char host_str[256];
        char port_str[16];
        const char *remaining_args =
            session_consume_token(args, host_str, sizeof(host_str));
        remaining_args =
            session_consume_token(remaining_args, port_str, sizeof(port_str));

        if (host_str[0] == '\0' || port_str[0] == '\0') {
            session_send_system_line(ctx, "Usage: /set-sync-url <host> <port>");
            return;
        }

        char *endptr;
        long port_long = strtol(port_str, &endptr, 10);
        if (*endptr != '\0' || port_long <= 0 || port_long > 65535) {
            session_send_system_line(
                ctx, "Invalid port number. Port must be between 1 and 65535.");
            return;
        }

        // For now, using default username and password. This can be extended later.
        ssh_chatter_sync_set_connection_details(host_str, (int)port_long,
                                                "chatter_sync", "password");
        ssh_chatter_sync_manual_trigger();
        session_send_system_line(
            ctx, "SSH sync URL updated. Attempting to reconnect...");
        return;
    }

    else if (session_parse_command_any(ctx, "/history", effective_line,
                                       &args)) {
        session_handle_history(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/exit", effective_line, &args)) {
        if (ctx->ops != nullptr && ctx->ops->handle_exit != nullptr) {
            ctx->ops->handle_exit(ctx);
        }
        return;
    }

    else if (session_parse_command_any(ctx, "/nick", effective_line, &args)) {
        if (ctx->ops != nullptr && ctx->ops->handle_nick != nullptr) {
            ctx->ops->handle_nick(ctx, args);
        } else {
            session_send_system_line(ctx, "Usage: /nick <name>");
        }
        return;
    }

    else if (session_parse_command_any(ctx, "/pm", effective_line, &args)) {
        session_handle_pm(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/asciiart", effective_line,
                                       &args)) {
        if (*args != '\0') {
            session_send_system_line(ctx, "Usage: /asciiart");
        } else {
            session_asciiart_begin(ctx, SESSION_ASCIIART_TARGET_CHAT);
        }
        return;
    }

    else if (session_parse_command_any(ctx, "/motd", effective_line, &args)) {
        if (*args != '\0') {
            session_send_system_line(ctx, "Usage: /motd");
        } else {
            session_handle_motd(ctx);
        }
        return;
    }

    else if (session_parse_command_any(ctx, "/status", effective_line, &args)) {
        session_handle_status(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/showstatus", effective_line,
                                       &args)) {
        session_handle_showstatus(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/users", effective_line, &args)) {
        if (*args != '\0') {
            session_send_system_line(ctx, "Usage: /users");
        } else {
            session_handle_usercount(ctx);
        }
        return;
    }

    else if (session_parse_command_any(ctx, "/search", effective_line, &args)) {
        session_handle_search(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/chat", effective_line, &args)) {
        session_handle_chat_lookup(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/reply", effective_line, &args)) {
        session_handle_reply(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/image", effective_line, &args)) {
        session_handle_image(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/video", effective_line, &args)) {
        session_handle_video(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/audio", effective_line, &args)) {
        session_handle_audio(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/files", effective_line, &args)) {
        session_handle_files(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/mail", effective_line, &args)) {
        session_handle_mail(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/game", effective_line, &args)) {
        session_handle_game(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/othello", effective_line,
                                       &args)) {
        session_handle_othello_command(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/banlist", effective_line,
                                       &args)) {
        session_handle_ban_list(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/banname", effective_line,
                                       &args)) {
        session_handle_ban_name(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/ban", effective_line, &args)) {
        session_handle_ban(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/delete-msg", effective_line,
                                         &args)) {
        session_handle_delete_message(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/block", effective_line,
                                         &args)) {
        session_handle_block(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/unblock", effective_line,
                                         &args)) {
        session_handle_unblock(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/pardon", effective_line, &args)) {
        session_handle_pardon(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/poke", effective_line, &args)) {
        session_handle_poke(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/color", effective_line, &args)) {
        session_handle_color(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/systemcolor", effective_line,
                                       &args)) {
        session_handle_system_color(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/set-trans-lang", effective_line,
                                         &args)) {
        session_handle_set_trans_lang(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/set-target-lang",
                                         effective_line, &args)) {
        session_handle_set_target_lang(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/set-ui-lang", effective_line,
                                         &args)) {
        session_handle_set_ui_lang(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/set-lf", effective_line,
                                         &args)) {
        session_handle_set_lf(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/weather", effective_line,
                                         &args)) {
        session_handle_weather(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/translate", effective_line,
                                         &args)) {
        session_handle_translate(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/translate-scope",
                                         effective_line, &args)) {
        session_handle_translate_scope(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/gemini-unfreeze",
                                         effective_line, &args)) {
        session_handle_gemini_unfreeze(ctx);
        return;
    } else if (session_parse_command_any(ctx, "/gemini", effective_line,
                                         &args)) {
        session_handle_gemini(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/captcha", effective_line,
                                         &args)) {
        session_handle_captcha(ctx, args);
        return;
    } else if (session_parse_command(effective_line, "/geo", &args)) {
        session_handle_geo_language(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/eliza", effective_line,
                                         &args)) {
        session_handle_eliza(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/ai-chat", effective_line,
                                         &args)) {
        session_handle_ai_chat(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/ollama-model", effective_line,
                                         &args)) {
        session_handle_ollama_model(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/breaking", effective_line,
                                         &args)) {
        session_handle_breaking_alerts(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/morse", effective_line,
                                         &args)) {
        session_handle_morse(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/morse-chat", effective_line,
                                         &args)) {
        session_handle_morse_chat(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/morse-reply", effective_line,
                                         &args)) {
        session_handle_morse_chat(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/chat-spacing", effective_line,
                                         &args)) {
        session_handle_chat_spacing(ctx, args);
        return;
    }

    else if (session_parse_command_any(ctx, "/retro", effective_line, &args)) {
        session_handle_retro(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/hybrid", effective_line,
                                         &args)) {
        session_handle_hybrid(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/saerom", effective_line,
                                         &args)) {
        if (*args != '\0') {
            session_send_system_line(ctx, "Usage: /saerom");
        } else {
            session_handle_saerom(ctx);
        }
        return;
    }

    else if (session_parse_command_any(ctx, "/mode", effective_line, &args)) {
        if (ctx->ops != nullptr && ctx->ops->handle_mode != nullptr) {
            ctx->ops->handle_mode(ctx, args);
        } else {
            session_send_system_line(ctx, "Usage: /mode <chat|command|toggle>");
        }
        return;
    } else if (session_parse_command_any(ctx, "/palette", effective_line,
                                         &args)) {
        session_handle_palette(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/suspend!", effective_line,
                                         &args)) {
        if (ctx->game.active) {
            session_game_suspend(ctx, "Game suspended.");
        } else {
            session_game_suspend(ctx, nullptr);
        }
        return;
    } else if (session_parse_command_any(ctx, "/today", effective_line,
                                         &args)) {
        if (*args != '\0') {
            session_send_system_line(ctx, "Usage: /today");
        } else {
            session_handle_today(ctx);
        }
        return;
    } else if (session_parse_command_any(ctx, "/date", effective_line, &args)) {
        session_handle_date(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/os", effective_line, &args)) {
        session_handle_os(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/getos", effective_line,
                                         &args)) {
        session_handle_getos(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/getaddr", effective_line,
                                         &args)) {
        session_handle_getaddr(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/birthday", effective_line,
                                         &args)) {
        session_handle_birthday(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/setpw", effective_line,
                                         &args)) {
        session_handle_setpw(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/delpw", effective_line,
                                         &args)) {
        session_handle_delpw(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/grant", effective_line,
                                         &args)) {
        session_handle_grant(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/shell", effective_line,
                                         &args)) {
        session_handle_shell(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/revoke", effective_line,
                                         &args)) {
        session_handle_revoke(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/pair", effective_line, &args)) {
        if (*args != '\0') {
            session_send_system_line(ctx, "Usage: /pair");
        } else {
            session_handle_pair(ctx);
        }
        return;
    } else if (session_parse_command_any(ctx, "/connected", effective_line,
                                         &args)) {
        if (*args != '\0') {
            session_send_system_line(ctx, "Usage: /connected");
        } else {
            session_handle_connected(ctx);
        }
        return;
    } else if (session_parse_command_any(ctx, "/alpha-centauri-landers",
                                         effective_line, &args)) {
        if (*args != '\0') {
            session_send_system_line(ctx, "Usage: /alpha-centauri-landers");
        } else {
            session_handle_alpha_centauri_landers(ctx);
        }
        return;
    } else if (session_parse_command_any(ctx, "/poll", effective_line, &args)) {
        session_handle_poll(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/vote-single", effective_line,
                                         &args)) {
        if (*args == '\0') {
            session_handle_vote_command(ctx, nullptr, false);
        } else {
            session_handle_vote_command(ctx, args, false);
        }
        return;
    } else if (session_parse_command_any(ctx, "/vote", effective_line, &args)) {
        if (*args == '\0') {
            session_handle_vote_command(ctx, nullptr, true);
        } else {
            session_handle_vote_command(ctx, args, true);
        }
        return;
    } else if (session_parse_command_any(ctx, "/elect", effective_line,
                                         &args)) {
        if (*args == '\0') {
            session_handle_elect_command(ctx, nullptr);
        } else {
            session_handle_elect_command(ctx, args);
        }
        return;
    } else if (session_parse_command(effective_line, "/rss", &args)) {
        session_handle_rss(ctx, args);
        return;
    } else if (session_parse_command_any(ctx, "/bbs", effective_line, &args)) {
        session_handle_bbs(ctx, (args != nullptr && args[0] != '\0') ? args
                                                                     : nullptr);
        return;
    }

    else if (session_parse_command_any(ctx, "/resetpw", effective_line,
                                       &args)) {
        session_handle_resetpw(ctx, args);
        return;
    }

    else if (effective_line[0] == '/') {
        if (isdigit((unsigned char)effective_line[1])) {
            char *endptr = nullptr;
            unsigned long vote_index = strtoul(effective_line + 1, &endptr, 10);
            const unsigned long max_vote = sizeof(ctx->owner->poll.options) /
                                           sizeof(ctx->owner->poll.options[0]);
            if (vote_index >= 1UL && vote_index <= max_vote) {
                while (endptr != nullptr &&
                       (*endptr == ' ' || *endptr == '\t')) {
                    ++endptr;
                }
                if (endptr == nullptr || *endptr == '\0') {
                    session_handle_vote(ctx, (size_t)(vote_index - 1UL));
                    return;
                } else {
                    while (*endptr == ' ' || *endptr == '\t') {
                        ++endptr;
                    }
                    if (*endptr != '\0') {
                        char label[SSH_CHATTER_POLL_LABEL_LEN];
                        size_t label_len = 0U;
                        while (*endptr != '\0' &&
                               !isspace((unsigned char)*endptr)) {
                            if (label_len + 1U >= sizeof(label)) {
                                label_len = 0U;
                                break;
                            }
                            label[label_len++] = *endptr++;
                        }
                        label[label_len] = '\0';
                        if (label_len > 0U) {
                            session_handle_named_vote(
                                ctx, (size_t)(vote_index - 1UL), label);
                            return;
                        }
                    }
                }
            }
        }
        for (size_t idx = 0U; idx < SSH_CHATTER_REACTION_KIND_COUNT; ++idx) {
            const reaction_descriptor_t *descriptor =
                &REACTION_DEFINITIONS[idx];
            char canonical[32];
            int written = snprintf(canonical, sizeof(canonical), "/%s",
                                   descriptor->command);
            if (written < 0 || (size_t)written >= sizeof(canonical)) {
                continue;
            }

            const char *arguments = nullptr;
            if (!session_parse_command_any(ctx, canonical, effective_line,
                                           &arguments)) {
                continue;
            }

            session_handle_reaction(ctx, idx, arguments);
            return;
        }
    }

    const session_ui_locale_t *locale = session_ui_get_locale(ctx);
    const char *format = (locale->unknown_command != nullptr &&
                          locale->unknown_command[0] != '\0')
                             ? locale->unknown_command
                             : "Unknown command. Type %shelp for help.";
    const char *prefix = session_command_prefix(ctx);
    const char *prefix_args[] = {prefix};
    char message[SSH_CHATTER_MESSAGE_LIMIT];
    session_format_template(format, prefix_args,
                            sizeof(prefix_args) / sizeof(prefix_args[0]),
                            message, sizeof(message));
    session_send_system_line(ctx, message);
}

void trim_whitespace_inplace(char *text)
{
    if (text == nullptr) {
        return;
    }

    char *start = text;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        ++start;
    }

    char *end = text + strlen(text);
    while (end > start && isspace((unsigned char)*(end - 1))) {
        --end;
    }

    const size_t length = (size_t)(end - start);
    if (start != text && length > 0U) {
        memmove(text, start, length);
    }
    text[length] = '\0';
}

static const char *session_consume_token(const char *input, char *token,
                                         size_t length)
{
    if (token == nullptr || length == 0U) {
        return input;
    }

    token[0] = '\0';
    if (input == nullptr) {
        return nullptr;
    }

    while (*input == ' ' || *input == '\t') {
        ++input;
    }

    size_t out_idx = 0U;
    while (*input != '\0' && !isspace((unsigned char)*input)) {
        if (out_idx + 1U < length) {
            token[out_idx++] = *input;
        }
        ++input;
    }
    token[out_idx] = '\0';

    while (*input == ' ' || *input == '\t') {
        ++input;
    }

    return input;
}

static bool session_user_data_available(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return false;
    }

    if (!ctx->owner->user_data_ready) {
        return false;
    }

    if (ctx->user.name[0] == '\0') {
        return false;
    }

    return true;
}

bool host_user_data_load_existing(host_t *host, const char *username,
                                  const char *ip, user_data_record_t *record,
                                  bool create_if_missing)
{
    if (host == nullptr || username == nullptr || username[0] == '\0') {
        return false;
    }

    if (!host->user_data_ready) {
        return false;
    }

    bool success = false;
    if (host->user_data_lock_initialized) {
        ttak_mutex_lock(&host->user_data_lock);
    }

    if (create_if_missing) {
        success =
            user_data_ensure_exists(host->user_data_root, username, ip, record);
    } else {
        success = user_data_load(host->user_data_root, username, ip, record);
    }

    if (host->user_data_lock_initialized) {
        ttak_mutex_unlock(&host->user_data_lock);
    }

    return success;
}

static bool host_lookup_last_ip(host_t *host, const char *username, char *ip,
                                size_t length)
{
    if (ip != nullptr && length > 0U) {
        ip[0] = '\0';
    }

    if (host == nullptr || username == nullptr || username[0] == '\0' ||
        ip == nullptr || length == 0U) {
        return false;
    }

    if (host_lookup_member_ip(host, username, ip, length)) {
        return true;
    }

    user_data_record_t record;
    if (!host_user_data_load_existing(host, username, nullptr, &record,
                                      false)) {
        return false;
    }

    if (record.last_ip[0] == '\0') {
        return false;
    }

    snprintf(ip, length, "%s", record.last_ip);
    return true;
}

static bool host_user_data_send_mail(host_t *host, const char *recipient,
                                     const char *recipient_ip,
                                     const char *sender, const char *message,
                                     char *error, size_t error_length)
{
    if (error != nullptr && error_length > 0U) {
        error[0] = '\0';
    }

    if (host == nullptr || recipient == nullptr || recipient[0] == '\0' ||
        message == nullptr || message[0] == '\0') {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length, "%s", "Invalid mailbox parameters.");
        }
        return false;
    }

    if (!host->user_data_ready) {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length, "%s", "Mailbox storage unavailable.");
        }
        return false;
    }

    char resolved_ip[SSH_CHATTER_IP_LEN];
    resolved_ip[0] = '\0';
    if (recipient_ip != nullptr && recipient_ip[0] != '\0') {
        snprintf(resolved_ip, sizeof(resolved_ip), "%s", recipient_ip);
    }

    const bool target_is_lan_ops =
        host_is_lan_operator_username(host, recipient);
    if (target_is_lan_ops) {
        session_ctx_t *target_session =
            chat_room_find_user(&host->room, recipient);
        if (target_session == nullptr ||
            !target_session->user.is_lan_operator) {
            if (error != nullptr && error_length > 0U) {
                snprintf(error, error_length, "%s",
                         "LAN operator mailbox is unavailable.");
            }
            return false;
        }
        snprintf(resolved_ip, sizeof(resolved_ip), "%s",
                 target_session->client_ip);
    }

    if (resolved_ip[0] == '\0') {
        session_ctx_t *target_session =
            chat_room_find_user(&host->room, recipient);
        if (target_session != nullptr) {
            snprintf(resolved_ip, sizeof(resolved_ip), "%s",
                     target_session->client_ip);
        }
    }

    if (resolved_ip[0] == '\0') {
        if (error != nullptr && error_length > 0U) {
            snprintf(
                error, error_length, "%s",
                "Provide the recipient's IP (name@ip) when they are offline.");
        }
        return false;
    }

    user_data_record_t record;
    if (!host_user_data_load_existing(host, recipient, resolved_ip, &record,
                                      true)) {
        if (error != nullptr && error_length > 0U) {
            const int mailbox_name_precision =
                (int)(SSH_CHATTER_USERNAME_LEN / 4U);
            snprintf(error, error_length, "Unable to open mailbox for %.*s.",
                     mailbox_name_precision, recipient);
        }
        return false;
    }

    if (record.mailbox_count >= USER_DATA_MAILBOX_LIMIT) {
        for (size_t idx = 1U; idx < USER_DATA_MAILBOX_LIMIT; ++idx) {
            record.mailbox[idx - 1U] = record.mailbox[idx];
        }
        record.mailbox_count = USER_DATA_MAILBOX_LIMIT - 1U;
    }

    user_data_mail_entry_t *entry = &record.mailbox[record.mailbox_count++];
    time_t now = time(nullptr);
    if (now == (time_t)-1) {
        now = 0;
    }
    entry->timestamp = (uint64_t)now;
    if (sender != nullptr && sender[0] != '\0') {
        snprintf(entry->sender, sizeof(entry->sender), "%s", sender);
    } else {
        snprintf(entry->sender, sizeof(entry->sender), "%s", "system");
    }
    snprintf(entry->message, sizeof(entry->message), "%s", message);
    record.last_updated = (uint64_t)now;
    snprintf(record.last_ip, sizeof(record.last_ip), "%s", resolved_ip);

    bool success;
    if (host->user_data_lock_initialized) {
        ttak_mutex_lock(&host->user_data_lock);
    }
    success = user_data_save(host->user_data_root, &record, resolved_ip);
    if (host->user_data_lock_initialized) {
        ttak_mutex_unlock(&host->user_data_lock);
    }

    if (!success) {
        if (error != nullptr && error_length > 0U) {
            snprintf(error, error_length, "%s",
                     "Failed to write mailbox file.");
        }
        humanized_log_error("mailbox", "failed to persist mailbox entry",
                            errno != 0 ? errno : EIO);
        return false;
    }

    return true;
}

static void rss_trim_whitespace(char *text)
{
    trim_whitespace_inplace(text);
}

static void rss_strip_html(char *text)
{
    if (text == nullptr) {
        return;
    }

    size_t read = 0U;
    size_t write = 0U;
    bool in_tag = false;
    while (text[read] != '\0') {
        char ch = text[read++];
        if (ch == '<') {
            in_tag = true;
            continue;
        }
        if (in_tag) {
            if (ch == '>') {
                in_tag = false;
            }
            continue;
        }
        text[write++] = ch;
    }
    text[write] = '\0';
}

static void rss_decode_entities(char *text)
{
    if (text == nullptr) {
        return;
    }

    char *src = text;
    char *dst = text;
    while (*src != '\0') {
        if (*src == '&') {
            if (strncmp(src, "&amp;", 5) == 0) {
                *dst++ = '&';
                src += 5;
                continue;
            }
            if (strncmp(src, "&lt;", 4) == 0) {
                *dst++ = '<';
                src += 4;
                continue;
            }
            if (strncmp(src, "&gt;", 4) == 0) {
                *dst++ = '>';
                src += 4;
                continue;
            }
            if (strncmp(src, "&quot;", 6) == 0) {
                *dst++ = '\"';
                src += 6;
                continue;
            }
            if (strncmp(src, "&#39;", 5) == 0) {
                *dst++ = '\'';
                src += 5;
                continue;
            }
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

// Reset a poll structure to a neutral inactive state.
static void poll_state_reset(poll_state_t *poll)
{
    if (poll == nullptr) {
        return;
    }

    poll->active = false;
    poll->option_count = 0U;
    poll->question[0] = '\0';
    poll->allow_multiple = false;
    for (size_t idx = 0U;
         idx < sizeof(poll->options) / sizeof(poll->options[0]); ++idx) {
        poll->options[idx].text[0] = '\0';
        poll->options[idx].votes = 0U;
    }
}

// Reset a named poll entry including its label and voter tracking list.
static void named_poll_reset(named_poll_state_t *poll)
{
    if (poll == nullptr) {
        return;
    }

    poll_state_reset(&poll->poll);
    poll->label[0] = '\0';
    poll->owner[0] = '\0';
    poll->voter_count = 0U;
    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_NAMED_VOTERS; ++idx) {
        poll->voters[idx].username[0] = '\0';
        poll->voters[idx].choice = -1;
        poll->voters[idx].choices_mask = 0U;
    }
}

// Look up a named poll by its label while the host lock is already held.
static named_poll_state_t *host_find_named_poll_locked(host_t *host,
                                                       const char *label)
{
    if (host == nullptr || label == nullptr || label[0] == '\0') {
        return nullptr;
    }

    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_NAMED_POLLS; ++idx) {
        named_poll_state_t *entry = &host->named_polls[idx];
        if (entry->label[0] == '\0') {
            continue;
        }
        if (strcasecmp(entry->label, label) == 0) {
            return entry;
        }
    }

    return nullptr;
}

// Either fetch an existing named poll or initialise a new slot for the provided label.
static __attribute__((unused)) named_poll_state_t *
host_ensure_named_poll_locked(host_t *host, const char *label)
{
    if (host == nullptr || label == nullptr || label[0] == '\0') {
        return nullptr;
    }

    named_poll_state_t *existing = host_find_named_poll_locked(host, label);
    if (existing != nullptr) {
        return existing;
    }

    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_NAMED_POLLS; ++idx) {
        named_poll_state_t *entry = &host->named_polls[idx];
        if (entry->label[0] != '\0') {
            continue;
        }
        named_poll_reset(entry);
        snprintf(entry->label, sizeof(entry->label), "%s", label);
        return entry;
    }

    return nullptr;
}

// Recompute how many named polls are active so list summaries remain accurate.
static void host_recount_named_polls_locked(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    size_t count = 0U;
    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_NAMED_POLLS; ++idx) {
        if (host->named_polls[idx].label[0] != '\0' &&
            host->named_polls[idx].poll.active) {
            ++count;
        }
    }
    host->named_poll_count = count;
}

// Ensure poll labels remain short and shell-friendly.
static __attribute__((unused)) bool poll_label_is_valid(const char *label)
{
    if (label == nullptr || label[0] == '\0') {
        return false;
    }

    for (size_t idx = 0U; label[idx] != '\0'; ++idx) {
        char ch = label[idx];
        if (!(isalnum((unsigned char)ch) || ch == '_' || ch == '-')) {
            return false;
        }
    }
    return true;
}

static void session_normalize_newlines(char *text)
{
    if (text == nullptr) {
        return;
    }

    size_t read_idx = 0U;
    size_t write_idx = 0U;
    while (text[read_idx] != '\0') {
        char ch = text[read_idx++];
        if (ch == '\r') {
            if (text[read_idx] == '\n') {
                ++read_idx;
            }
            text[write_idx++] = '\n';
        } else {
            text[write_idx++] = ch;
        }
    }

    text[write_idx] = '\0';
}

static bool timezone_sanitize_identifier(const char *input, char *output,
                                         size_t length)
{
    if (input == nullptr || output == nullptr || length == 0U) {
        return false;
    }

    size_t out_idx = 0U;
    bool last_was_slash = true;

    for (size_t idx = 0U; input[idx] != '\0'; ++idx) {
        unsigned char ch = (unsigned char)input[idx];
        if (isspace(ch)) {
            return false;
        }

        if (ch == '/') {
            if (last_was_slash) {
                return false;
            }
            if (out_idx + 1U >= length) {
                return false;
            }
            output[out_idx++] = '/';
            last_was_slash = true;
            continue;
        }

        if (!(isalnum(ch) || ch == '_' || ch == '-' || ch == '+' ||
              ch == '.')) {
            return false;
        }

        if (out_idx + 1U >= length) {
            return false;
        }
        output[out_idx++] = (char)ch;
        last_was_slash = false;
    }

    if (out_idx == 0U || last_was_slash) {
        return false;
    }

    output[out_idx] = '\0';

    if (output[0] == '/' || strstr(output, "..") != nullptr) {
        return false;
    }

    return true;
}

static bool timezone_resolve_identifier(const char *input, char *resolved,
                                        size_t length)
{
    if (input == nullptr || input[0] == '\0' || resolved == nullptr ||
        length == 0U) {
        return false;
    }

    static const char kTimezoneDir[] = "/usr/share/zoneinfo";

    char full_path[PATH_MAX];
    int full_written =
        snprintf(full_path, sizeof(full_path), "%s/%s", kTimezoneDir, input);
    if (full_written >= 0 && (size_t)full_written < sizeof(full_path) &&
        access(full_path, R_OK) == 0) {
        int copy_written = snprintf(resolved, length, "%s", input);
        return copy_written >= 0 && (size_t)copy_written < length;
    }

    char working[PATH_MAX];
    int working_written = snprintf(working, sizeof(working), "%s", input);
    if (working_written < 0 || (size_t)working_written >= sizeof(working)) {
        return false;
    }

    char accumulated[PATH_MAX];
    accumulated[0] = '\0';
    size_t accumulated_len = 0U;
    char current_dir[PATH_MAX];
    int dir_written =
        snprintf(current_dir, sizeof(current_dir), "%s", kTimezoneDir);
    if (dir_written < 0 || (size_t)dir_written >= sizeof(current_dir)) {
        return false;
    }

    char *saveptr = nullptr;
    char *segment = strtok_r(working, "/", &saveptr);
    if (segment == nullptr) {
        return false;
    }

    while (segment != nullptr) {
        DIR *dir = opendir(current_dir);
        if (dir == nullptr) {
            return false;
        }

        bool found = false;
        char matched[NAME_MAX + 1];
        matched[0] = '\0';
        struct dirent *entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') {
                if (entry->d_name[1] == '\0') {
                    continue;
                }
                if (entry->d_name[1] == '.' && entry->d_name[2] == '\0') {
                    continue;
                }
            }

            if (strcasecmp(entry->d_name, segment) == 0) {
                found = true;
                snprintf(matched, sizeof(matched), "%s", entry->d_name);
                break;
            }
        }
        closedir(dir);

        if (!found) {
            return false;
        }

        if (accumulated_len > 0U) {
            if (accumulated_len + 1U >= sizeof(accumulated)) {
                return false;
            }
            accumulated[accumulated_len++] = '/';
        }

        size_t match_len = strlen(matched);
        if (accumulated_len + match_len >= sizeof(accumulated)) {
            return false;
        }
        memcpy(accumulated + accumulated_len, matched, match_len);
        accumulated_len += match_len;
        accumulated[accumulated_len] = '\0';

        dir_written = snprintf(current_dir, sizeof(current_dir), "%s/%s",
                               kTimezoneDir, accumulated);
        if (dir_written < 0 || (size_t)dir_written >= sizeof(current_dir)) {
            return false;
        }

        segment = strtok_r(nullptr, "/", &saveptr);
    }

    if (accumulated_len == 0U) {
        return false;
    }

    full_written = snprintf(full_path, sizeof(full_path), "%s/%s", kTimezoneDir,
                            accumulated);
    if (full_written < 0 || (size_t)full_written >= sizeof(full_path)) {
        return false;
    }

    if (access(full_path, R_OK) != 0) {
        return false;
    }

    int copy_written = snprintf(resolved, length, "%s", accumulated);
    return copy_written >= 0 && (size_t)copy_written < length;
}

static const os_descriptor_t *session_lookup_os_descriptor(const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        return nullptr;
    }

    for (size_t idx = 0U; idx < sizeof(OS_CATALOG) / sizeof(OS_CATALOG[0]);
         ++idx) {
        if (strcasecmp(OS_CATALOG[idx].name, name) == 0) {
            return &OS_CATALOG[idx];
        }
    }

    return nullptr;
}

static const char *lookup_color_code(const color_entry_t *entries,
                                     size_t entry_count, const char *name)
{
    if (entries == nullptr || name == nullptr) {
        return nullptr;
    }

    for (size_t idx = 0; idx < entry_count; ++idx) {
        if (strcasecmp(entries[idx].name, name) == 0) {
            return entries[idx].code;
        }
    }

#if defined(__STDC_NO_THREADS__)
    static char fg_cache[8][16];
    static size_t fg_cache_index = 0U;
    static char bg_cache[8][16];
    static size_t bg_cache_index = 0U;
#else
    static _Thread_local char fg_cache[8][16];
    static _Thread_local size_t fg_cache_index = 0U;
    static _Thread_local char bg_cache[8][16];
    static _Thread_local size_t bg_cache_index = 0U;
#endif

    if (strncasecmp(name, "xterm:", 6) == 0) {
        const char *digits = name + 6;
        char *endptr = nullptr;
        unsigned long code = strtoul(digits, &endptr, 10);
        if (endptr != nullptr && *endptr == '\0' && code <= 255U) {
            char(*slot)[16] = &fg_cache[fg_cache_index];
            fg_cache_index = (fg_cache_index + 1U) %
                             (sizeof(fg_cache) / sizeof(fg_cache[0]));
            ansi_256(*slot, sizeof(fg_cache[0]), (unsigned int)code);
            return *slot;
        }
    }

    if (strncasecmp(name, "xterm-bg:", 9) == 0) {
        const char *digits = name + 9;
        char *endptr = nullptr;
        unsigned long code = strtoul(digits, &endptr, 10);
        if (endptr != nullptr && *endptr == '\0' && code <= 255U) {
            char(*slot)[16] = &bg_cache[bg_cache_index];
            bg_cache_index = (bg_cache_index + 1U) %
                             (sizeof(bg_cache) / sizeof(bg_cache[0]));
            ansi_bg_256(*slot, sizeof(bg_cache[0]), (unsigned int)code);
            return *slot;
        }
    }

    return nullptr;
}

static const palette_descriptor_t *palette_find_descriptor(const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        return nullptr;
    }

    for (size_t idx = 0U;
         idx < sizeof(PALETTE_DEFINITIONS) / sizeof(PALETTE_DEFINITIONS[0]);
         ++idx) {
        if (strcasecmp(PALETTE_DEFINITIONS[idx].id, name) == 0) {
            return &PALETTE_DEFINITIONS[idx];
        }
    }

    return nullptr;
}

static bool palette_apply_to_session(session_ctx_t *ctx,
                                     const palette_descriptor_t *descriptor)
{
    if (ctx == nullptr || descriptor == nullptr) {
        return false;
    }

    const char *user_color_code = lookup_color_code(
        USER_COLOR_MAP, sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
        descriptor->user_color_name);
    const char *user_highlight_code = lookup_color_code(
        HIGHLIGHT_COLOR_MAP,
        sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
        descriptor->user_highlight_name);
    const char *system_fg_code = lookup_color_code(
        USER_COLOR_MAP, sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
        descriptor->system_fg_name);
    const char *system_bg_code = lookup_color_code(
        HIGHLIGHT_COLOR_MAP,
        sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
        descriptor->system_bg_name);
    const char *system_highlight_code = lookup_color_code(
        HIGHLIGHT_COLOR_MAP,
        sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
        descriptor->system_highlight_name);

    if (user_color_code == nullptr || user_highlight_code == nullptr ||
        system_fg_code == nullptr || system_bg_code == nullptr ||
        system_highlight_code == nullptr) {
        return false;
    }

    snprintf(ctx->user_color_code, sizeof(ctx->user_color_code), "%s",
             user_color_code);
    snprintf(ctx->user_highlight_code, sizeof(ctx->user_highlight_code), "%s",
             user_highlight_code);
    ctx->user_is_bold = descriptor->user_is_bold;
    snprintf(ctx->user_color_name, sizeof(ctx->user_color_name), "%s",
             descriptor->user_color_name);
    snprintf(ctx->user_highlight_name, sizeof(ctx->user_highlight_name), "%s",
             descriptor->user_highlight_name);

    ctx->system_fg_code = system_fg_code;
    ctx->system_bg_code = system_bg_code;
    ctx->system_highlight_code = system_highlight_code;
    ctx->system_is_bold = descriptor->system_is_bold;
    snprintf(ctx->system_fg_name, sizeof(ctx->system_fg_name), "%s",
             descriptor->system_fg_name);
    snprintf(ctx->system_bg_name, sizeof(ctx->system_bg_name), "%s",
             descriptor->system_bg_name);
    snprintf(ctx->system_highlight_name, sizeof(ctx->system_highlight_name),
             "%s", descriptor->system_highlight_name);

    session_force_dark_mode_foreground(ctx);

    return true;
}

static void
host_apply_palette_descriptor(host_t *host,
                              const palette_descriptor_t *descriptor)
{
    if (host == nullptr || descriptor == nullptr) {
        return;
    }

    const char *user_color_code = lookup_color_code(
        USER_COLOR_MAP, sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
        descriptor->user_color_name);
    const char *user_highlight_code = lookup_color_code(
        HIGHLIGHT_COLOR_MAP,
        sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
        descriptor->user_highlight_name);
    const char *system_fg_code = lookup_color_code(
        USER_COLOR_MAP, sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
        descriptor->system_fg_name);
    const char *system_bg_code = lookup_color_code(
        HIGHLIGHT_COLOR_MAP,
        sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
        descriptor->system_bg_name);
    const char *system_highlight_code = lookup_color_code(
        HIGHLIGHT_COLOR_MAP,
        sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
        descriptor->system_highlight_name);

    if (user_color_code == nullptr) {
        user_color_code = ANSI_GREEN;
    }
    if (user_highlight_code == nullptr) {
        user_highlight_code = ANSI_BG_DEFAULT;
    }
    if (system_fg_code == nullptr) {
        system_fg_code = ANSI_WHITE;
    }
    if (system_bg_code == nullptr) {
        system_bg_code = ANSI_BG_BLUE;
    }
    if (system_highlight_code == nullptr) {
        system_highlight_code = ANSI_BG_YELLOW;
    }

    host->user_theme.userColor = user_color_code;
    host->user_theme.highlight = user_highlight_code;
    host->user_theme.isBold = descriptor->user_is_bold;
    host->system_theme.foregroundColor = system_fg_code;
    host->system_theme.backgroundColor = system_bg_code;
    host->system_theme.highlightColor = system_highlight_code;
    host->system_theme.isBold = descriptor->system_is_bold;

    snprintf(host->default_user_color_name,
             sizeof(host->default_user_color_name), "%s",
             descriptor->user_color_name);
    snprintf(host->default_user_highlight_name,
             sizeof(host->default_user_highlight_name), "%s",
             descriptor->user_highlight_name);
    snprintf(host->default_system_fg_name, sizeof(host->default_system_fg_name),
             "%s", descriptor->system_fg_name);
    snprintf(host->default_system_bg_name, sizeof(host->default_system_bg_name),
             "%s", descriptor->system_bg_name);
    snprintf(host->default_system_highlight_name,
             sizeof(host->default_system_highlight_name), "%s",
             descriptor->system_highlight_name);
}

static bool parse_bool_token(const char *token, bool *value)
{
    if (token == nullptr || value == nullptr) {
        return false;
    }

    if (strcasecmp(token, "true") == 0 || strcasecmp(token, "yes") == 0 ||
        strcasecmp(token, "on") == 0 || strcasecmp(token, "bold") == 0 ||
        strcmp(token, "켜기") == 0 || strcmp(token, "オン") == 0 ||
        strcmp(token, "开") == 0 || strcmp(token, "вкл") == 0) {
        *value = true;
        return true;
    }

    if (strcasecmp(token, "false") == 0 || strcasecmp(token, "no") == 0 ||
        strcasecmp(token, "off") == 0 || strcasecmp(token, "normal") == 0 ||
        strcmp(token, "끄기") == 0 || strcmp(token, "オフ") == 0 ||
        strcmp(token, "关") == 0 || strcmp(token, "выкл") == 0) {
        *value = false;
        return true;
    }

    return false;
}

static bool session_transport_active(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        return ctx->telnet_fd >= 0 && !ctx->telnet_eof;
    }

    return ctx->channel != nullptr;
}

static bool session_transport_is_open(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        return ctx->telnet_fd >= 0 && !ctx->telnet_eof;
    }

    return ctx->channel != nullptr && ssh_channel_is_open(ctx->channel);
}

static bool session_transport_is_eof(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return true;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        return ctx->telnet_eof || ctx->telnet_fd < 0;
    }

    return ctx->channel == nullptr || ssh_channel_is_eof(ctx->channel);
}

static void session_transport_request_close(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        if (ctx->telnet_fd >= 0) {
            shutdown(ctx->telnet_fd, SHUT_RDWR);
        }
        ctx->telnet_eof = true;
        return;
    }

    if (ctx->channel != nullptr) {
        ssh_channel_send_eof(ctx->channel);
        ssh_channel_close(ctx->channel);
    }
}

static void session_close_channel(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        if (ctx->telnet_fd >= 0) {
            shutdown(ctx->telnet_fd, SHUT_RDWR);
            close(ctx->telnet_fd);
            ctx->telnet_fd = -1;
        }
        ctx->telnet_eof = true;
        return;
    }

    if (ctx->channel == nullptr) {
        return;
    }

    /* Nullify the channel pointer first so that concurrent readers
     * (e.g. in-flight broadcast threads) see NULL via
     * session_transport_active() and bail out before we free the
     * underlying libssh object. */
    ssh_channel old_channel = ctx->channel;
    ctx->channel = nullptr;

    ssh_channel_send_eof(old_channel);
    ssh_channel_close(old_channel);
    ssh_channel_free(old_channel);
}

static void session_reset_for_retry(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    session_close_channel(ctx);
    ctx->should_exit = false;
    ctx->exit_notice_sent = false;
    ctx->username_conflict = false;
    ctx->has_joined_room = false;
    ctx->prelogin_banner_rendered = false;
    ctx->input_length = 0U;
    ctx->input_buffer[0] = '\0';
    ctx->input_escape_active = false;
    ctx->input_escape_length = 0U;
    ctx->input_escape_buffer[0] = '\0';
    ctx->multibyte_input_length = 0U;
    memset(ctx->multibyte_input_buffer, 0, sizeof(ctx->multibyte_input_buffer));
    ctx->bbs_post_pending = false;
    ctx->pending_bbs_body_length = 0U;
    ctx->pending_bbs_tag_count = 0U;
    if (ctx->pending_bbs_title != nullptr) {
        ctx->pending_bbs_title[0] = '\0';
    }
    if (ctx->pending_bbs_body != nullptr) {
        ctx->pending_bbs_body[0] = '\0';
    }
    if (ctx->pending_bbs_tags != nullptr) {
        memset(ctx->pending_bbs_tags, 0,
               sizeof(*ctx->pending_bbs_tags) * SSH_CHATTER_BBS_MAX_TAGS);
    }
    ctx->bbs_view_active = false;
    ctx->bbs_view_post_id = 0U;
    ctx->bbs_view_scroll_offset = 0U;
    ctx->bbs_view_total_lines = 0U;
    ctx->bbs_view_notice_pending = false;
    if (ctx->bbs_view_notice != nullptr) {
        ctx->bbs_view_notice[0] = '\0';
    }
    ctx->bbs_rendering_editor = false;
    ctx->telnet_terminal_type_requested = false;
    ctx->terminal_type[0] = '\0';
    ctx->prefer_cp437_output = false;
    ctx->cp437_output_scope = SESSION_CP437_SCOPE_ALL;
    ctx->output_kind = SESSION_OUTPUT_KIND_SYSTEM;
    ctx->cp437_override = SESSION_CP437_OVERRIDE_NONE;
    ctx->cp437_input_enabled = false;
    session_asciiart_reset(ctx);
    ctx->asciiart_has_cooldown = false;
    ctx->last_asciiart_post.tv_sec = 0;
    ctx->last_asciiart_post.tv_nsec = 0;
    session_game_tetris_reset(ctx->game.tetris);
    ctx->game.liar.awaiting_guess = false;
    ctx->game.liar.round_number = 0U;
    ctx->game.liar.score = 0U;
    ctx->game.othello = (othello_game_state_t){0};
    ctx->game.saved_othello_state = (othello_game_state_t){0};
    ctx->game.active = false;
    ctx->game.type = SESSION_GAME_NONE;
    ctx->game.rng_seeded = false;
    ctx->game.rng_state = 0U;
    ctx->game.alpha = (alpha_centauri_game_state_t){0};
    ctx->input_history_count = 0U;
    memset(ctx->input_history_is_command, 0,
           sizeof(ctx->input_history_is_command));
    ctx->input_history_position = -1;
    session_scrollback_reset_position(ctx);
    ctx->has_last_message_time = false;
    ctx->last_message_time.tv_sec = 0;
    ctx->last_message_time.tv_nsec = 0;
    ctx->user_data_loaded = false;
    memset(&ctx->user_data, 0, sizeof(ctx->user_data));
    session_refresh_output_encoding(ctx);
}

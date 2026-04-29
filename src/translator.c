/**
 * @file translator.c
 * @desc File-level documentation for translator.c, describing its role
 *       in the SSH-Chatter server and providing a consistent header
 *       comment format across C sources.
 * @return None.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "ssh_chatter/translator.h"

#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>

#include "ssh_chatter/abstract_byte_buffer.h"
#include "ssh_chatter/memory_manager.h"

#define TRANSLATOR_MAX_RESPONSE 1 << 20
#define TRANSLATOR_DEFAULT_BASE_URL                                            \
    "https://generativelanguage.googleapis.com/v1beta"
#define TRANSLATOR_DEFAULT_MODEL "gemini-2.5"
#define TRANSLATOR_CONNECT_TIMEOUT_MS 5000L
#define TRANSLATOR_TOTAL_TIMEOUT_MS 15000L

typedef struct translator_buffer {
    sshc_abstract_byte_buffer_t bytes;
} translator_buffer_t;

static void translator_buffer_init(translator_buffer_t *buffer)
{
    if (buffer == nullptr) {
        return;
    }
    sshc_abstract_byte_buffer_init(&buffer->bytes);
}

static void translator_buffer_reset(translator_buffer_t *buffer)
{
    if (buffer == nullptr) {
        return;
    }
    buffer->bytes.length = 0U;
}

static void translator_buffer_free(translator_buffer_t *buffer)
{
    if (buffer == nullptr) {
        return;
    }
    sshc_abstract_byte_buffer_free(&buffer->bytes);
}

static bool translator_buffer_has_data(const translator_buffer_t *buffer)
{
    return buffer != nullptr && buffer->bytes.storage != nullptr;
}

static bool translator_buffer_map_read(translator_buffer_t *buffer,
                                       sshc_abstract_byte_buffer_view_t *view)
{
    if (buffer == nullptr) {
        return false;
    }
    return sshc_abstract_byte_buffer_map_cstr(&buffer->bytes, view);
}

typedef enum translator_provider {
    TRANSLATOR_PROVIDER_GEMINI,
    TRANSLATOR_PROVIDER_OLLAMA,
} translator_provider_t;

typedef struct translator_candidate {
    translator_provider_t provider;
    const char *model;
    const char *api_key;
    const char *api_key_name;
} translator_candidate_t;

static ttak_mutex_t g_init_mutex;
static ttak_mutex_t g_error_mutex;
static ttak_mutex_t g_rate_mutex;
static ttak_mutex_t g_moderation_mutex;
static ttak_mutex_t g_provider_mutex;
static ttak_mutex_t g_memory_mutex;
static bool g_init_mutex_initialized = false;
static bool g_curl_initialised = false;
static char g_last_error[256] = "";
static bool g_last_error_was_quota = false;
static struct timespec g_next_allowed_request = {0, 0};
static struct timespec g_next_allowed_moderation = {0, 0};
static bool g_gemini_manually_disabled = false;
static struct timespec g_gemini_disabled_until = {0, 0};
static bool g_manual_chat_bbs_only = true;
static bool g_manual_skip_scrollback_translation = true;
static char g_gemini_cooldown_file_path[PATH_MAX] = "";
static ttak_mutex_t g_gemini_cooldown_file_mutex;
static bool g_gemini_cooldown_file_initialised = false;
static sshc_memory_context_t *g_translator_memory_context = nullptr;

static void translator_mutex_init_all(void)
{
    if (!g_init_mutex_initialized) {
        ttak_mutex_init(&g_init_mutex);
        ttak_mutex_init(&g_error_mutex);
        ttak_mutex_init(&g_rate_mutex);
        ttak_mutex_init(&g_moderation_mutex);
        ttak_mutex_init(&g_provider_mutex);
        ttak_mutex_init(&g_memory_mutex);
        ttak_mutex_init(&g_gemini_cooldown_file_mutex);
        g_init_mutex_initialized = true;
    }
}

typedef struct translator_memory_scope {
    bool active;
    sshc_memory_context_t *previous;
} translator_memory_scope_t;

static translator_memory_scope_t translator_memory_scope_enter(void)
{
    translator_memory_scope_t scope = {.active = false, .previous = nullptr};
    translator_mutex_init_all();
    ttak_mutex_lock(&g_memory_mutex);
    if (g_translator_memory_context == nullptr) {
        g_translator_memory_context =
            sshc_memory_context_create("translator");
    }
    sshc_memory_context_t *context = g_translator_memory_context;
    ttak_mutex_unlock(&g_memory_mutex);
    if (context == nullptr) {
        return scope;
    }
    scope.previous = sshc_memory_context_push(context);
    scope.active = true;
    return scope;
}

static void translator_memory_scope_exit(translator_memory_scope_t *scope)
{
    if (scope == nullptr || !scope->active) {
        return;
    }
    sshc_memory_context_pop(scope->previous);
    translator_mutex_init_all();
    ttak_mutex_lock(&g_memory_mutex);
    if (g_translator_memory_context != nullptr) {
        sshc_memory_context_epoch_gc_rotate(g_translator_memory_context);
    }
    ttak_mutex_unlock(&g_memory_mutex);
    sshc_epoch_reclaim();
    scope->active = false;
}

#define TRANSLATOR_RATE_LIMIT_INTERVAL_NS 800000000L
#define TRANSLATOR_MODERATION_INTERVAL_NS 300000000L
#define TRANSLATOR_RATE_LIMIT_PENALTY_NS 3000000000L
#define TRANSLATOR_GEMINI_RATE_LIMIT_DURATION_NS (60L * 60L * 1000000000L)
#define TRANSLATOR_GEMINI_FALLBACK_DURATION_NS (24L * 60L * 60L * 1000000000L)

#define TRANSLATOR_GEMINI_COOLDOWN_MAGIC 0x47434f4fU /* 'GCOO' */
#define TRANSLATOR_GEMINI_COOLDOWN_VERSION 1U

typedef struct translator_gemini_cooldown_state_v1 {
    uint32_t magic;
    uint32_t version;
    int64_t remaining_ns;
} translator_gemini_cooldown_state_v1_t;

static void translator_clear_gemini_backoff_locked(void);

static struct timespec translator_timespec_now(void)
{
    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now;
}

static int translator_timespec_compare(const struct timespec *a,
                                       const struct timespec *b)
{
    if (a == nullptr || b == nullptr) {
        return 0;
    }

    if (a->tv_sec != b->tv_sec) {
        return (a->tv_sec < b->tv_sec) ? -1 : 1;
    }

    if (a->tv_nsec != b->tv_nsec) {
        return (a->tv_nsec < b->tv_nsec) ? -1 : 1;
    }

    return 0;
}

static struct timespec translator_timespec_add_ns(const struct timespec *base,
                                                  long nanoseconds)
{
    struct timespec result = {0, 0};
    if (base != nullptr) {
        result = *base;
    }

    long seconds = nanoseconds / 1000000000L;
    long remainder = nanoseconds % 1000000000L;
    if (remainder < 0) {
        --seconds;
        remainder += 1000000000L;
    }

    result.tv_sec += seconds;
    result.tv_nsec += remainder;
    if (result.tv_nsec >= 1000000000L) {
        ++result.tv_sec;
        result.tv_nsec -= 1000000000L;
    }

    if (result.tv_nsec < 0) {
        --result.tv_sec;
        result.tv_nsec += 1000000000L;
    }

    if (result.tv_sec < 0) {
        result.tv_sec = 0;
        result.tv_nsec = 0;
    }

    return result;
}

static struct timespec translator_timespec_diff(const struct timespec *end,
                                                const struct timespec *start)
{
    struct timespec result = {0, 0};
    if (end == nullptr || start == nullptr) {
        return result;
    }

    if (translator_timespec_compare(end, start) <= 0) {
        return result;
    }

    result.tv_sec = end->tv_sec - start->tv_sec;
    result.tv_nsec = end->tv_nsec - start->tv_nsec;
    if (result.tv_nsec < 0) {
        --result.tv_sec;
        result.tv_nsec += 1000000000L;
    }

    if (result.tv_sec < 0) {
        result.tv_sec = 0;
        result.tv_nsec = 0;
    }

    return result;
}

static void translator_gemini_cooldown_write_snapshot(long long remaining_ns)
{
    if (g_gemini_cooldown_file_path[0] == '\0') {
        return;
    }

    ttak_mutex_lock(&g_gemini_cooldown_file_mutex);

    char temp_path[PATH_MAX];
    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp",
                           g_gemini_cooldown_file_path);
    if (written < 0 || (size_t)written >= sizeof(temp_path)) {
        ttak_mutex_unlock(&g_gemini_cooldown_file_mutex);
        return;
    }

    FILE *fp = fopen(temp_path, "wb");
    if (fp == nullptr) {
        ttak_mutex_unlock(&g_gemini_cooldown_file_mutex);
        return;
    }

    translator_gemini_cooldown_state_v1_t state = {0};
    state.magic = TRANSLATOR_GEMINI_COOLDOWN_MAGIC;
    state.version = TRANSLATOR_GEMINI_COOLDOWN_VERSION;
    state.remaining_ns = remaining_ns >= 0 ? remaining_ns : 0;

    bool success = fwrite(&state, sizeof(state), 1U, fp) == 1U;

    if (success && fflush(fp) != 0) {
        success = false;
    }

    if (success) {
        int fd = fileno(fp);
        if (fd >= 0 && fsync(fd) != 0) {
            success = false;
        }
    }

    if (fclose(fp) != 0) {
        success = false;
    }

    if (!success) {
        unlink(temp_path);
    } else if (rename(temp_path, g_gemini_cooldown_file_path) != 0) {
        unlink(temp_path);
    }

    ttak_mutex_unlock(&g_gemini_cooldown_file_mutex);
}

static void translator_gemini_cooldown_persist_locked(void)
{
    struct timespec now = translator_timespec_now();
    long long remaining_ns = 0;

    if (g_gemini_disabled_until.tv_sec != 0 ||
        g_gemini_disabled_until.tv_nsec != 0) {
        struct timespec remaining =
            translator_timespec_diff(&g_gemini_disabled_until, &now);
        remaining_ns = (long long)remaining.tv_sec * 1000000000LL +
                       (long long)remaining.tv_nsec;
        if (remaining_ns < 0) {
            remaining_ns = 0;
        }
    }

    translator_gemini_cooldown_write_snapshot(remaining_ns);
}

static void translator_gemini_cooldown_resolve_path(void)
{
    const char *path = getenv("CHATTER_GEMINI_COOLDOWN_FILE");
    if (path == nullptr || path[0] == '\0') {
        path = "gemini_cooldown.dat";
    }

    int written = snprintf(g_gemini_cooldown_file_path,
                           sizeof(g_gemini_cooldown_file_path), "%s", path);
    if (written < 0 || (size_t)written >= sizeof(g_gemini_cooldown_file_path)) {
        g_gemini_cooldown_file_path[0] = '\0';
    }
}

static void translator_gemini_cooldown_load(void)
{
    if (g_gemini_cooldown_file_path[0] == '\0') {
        return;
    }

    ttak_mutex_lock(&g_gemini_cooldown_file_mutex);
    FILE *fp = fopen(g_gemini_cooldown_file_path, "rb");
    if (fp == nullptr) {
        ttak_mutex_unlock(&g_gemini_cooldown_file_mutex);
        return;
    }

    translator_gemini_cooldown_state_v1_t state = {0};
    bool success = fread(&state, sizeof(state), 1U, fp) == 1U;
    (void)fclose(fp);
    ttak_mutex_unlock(&g_gemini_cooldown_file_mutex);

    if (!success) {
        return;
    }

    if (state.magic != TRANSLATOR_GEMINI_COOLDOWN_MAGIC) {
        return;
    }

    if (state.version == 0U ||
        state.version > TRANSLATOR_GEMINI_COOLDOWN_VERSION) {
        return;
    }

    long long remaining_ns = state.remaining_ns;
    if (remaining_ns <= 0) {
        ttak_mutex_lock(&g_provider_mutex);
        translator_clear_gemini_backoff_locked();
        ttak_mutex_unlock(&g_provider_mutex);
        return;
    }

    struct timespec now = translator_timespec_now();
    struct timespec until = now;

    long long seconds = remaining_ns / 1000000000LL;
    long long nanos = remaining_ns % 1000000000LL;
    if (seconds < 0) {
        seconds = 0;
    }
    if (nanos < 0) {
        nanos += 1000000000LL;
    }

    until.tv_sec += (time_t)seconds;
    long nsec_add = (long)nanos;
    until.tv_nsec += nsec_add;
    if (until.tv_nsec >= 1000000000L) {
        ++until.tv_sec;
        until.tv_nsec -= 1000000000L;
    }

    ttak_mutex_lock(&g_provider_mutex);
    g_gemini_disabled_until = until;
    translator_gemini_cooldown_persist_locked();
    ttak_mutex_unlock(&g_provider_mutex);
}

static void translator_rate_limit_wait(void)
{
    for (;;) {
        ttak_mutex_lock(&g_rate_mutex);
        struct timespec now = translator_timespec_now();
        if (g_next_allowed_request.tv_sec == 0 &&
            g_next_allowed_request.tv_nsec == 0) {
            g_next_allowed_request = translator_timespec_add_ns(
                &now, TRANSLATOR_RATE_LIMIT_INTERVAL_NS);
            ttak_mutex_unlock(&g_rate_mutex);
            return;
        }

        if (translator_timespec_compare(&now, &g_next_allowed_request) >= 0) {
            g_next_allowed_request = translator_timespec_add_ns(
                &now, TRANSLATOR_RATE_LIMIT_INTERVAL_NS);
            ttak_mutex_unlock(&g_rate_mutex);
            return;
        }

        struct timespec wait_time =
            translator_timespec_diff(&g_next_allowed_request, &now);
        ttak_mutex_unlock(&g_rate_mutex);
        struct timespec request = wait_time;
        struct timespec remaining = {0};
        while (nanosleep(&request, &remaining) != 0) {
            if (errno != EINTR) {
                break;
            }
            request = remaining;
        }
    }
}

static void translator_moderation_throttle_wait(void)
{
    for (;;) {
        ttak_mutex_lock(&g_moderation_mutex);
        struct timespec now = translator_timespec_now();
        if ((g_next_allowed_moderation.tv_sec == 0 &&
             g_next_allowed_moderation.tv_nsec == 0) ||
            translator_timespec_compare(&now, &g_next_allowed_moderation) >=
                0) {
            g_next_allowed_moderation = translator_timespec_add_ns(
                &now, TRANSLATOR_MODERATION_INTERVAL_NS);
            ttak_mutex_unlock(&g_moderation_mutex);
            return;
        }

        struct timespec wait_time =
            translator_timespec_diff(&g_next_allowed_moderation, &now);
        ttak_mutex_unlock(&g_moderation_mutex);
        struct timespec request = wait_time;
        struct timespec remaining = {0};
        while (nanosleep(&request, &remaining) != 0) {
            if (errno != EINTR) {
                break;
            }
            request = remaining;
        }
    }
}

static void translator_rate_limit_penalise_until(const struct timespec *until)
{
    if (until == nullptr) {
        return;
    }

    ttak_mutex_lock(&g_rate_mutex);
    if (translator_timespec_compare(until, &g_next_allowed_request) > 0) {
        g_next_allowed_request = *until;
    }
    ttak_mutex_unlock(&g_rate_mutex);
}

static void translator_rate_limit_penalize(long status)
{
    if (status != 429L) {
        return;
    }

    struct timespec now = translator_timespec_now();
    struct timespec penalty_until =
        translator_timespec_add_ns(&now, TRANSLATOR_RATE_LIMIT_PENALTY_NS);
    translator_rate_limit_penalise_until(&penalty_until);
}

static bool translator_cancel_requested(const volatile bool *flag)
{
    return flag != nullptr && *flag;
}

static int translator_progress_abort(void *clientp, curl_off_t dltotal,
                                     curl_off_t dlnow, curl_off_t ultotal,
                                     curl_off_t ulnow)
{
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    return translator_cancel_requested((const volatile bool *)clientp) ? 1 : 0;
}

static void
translator_configure_cancel_callback(CURL *curl,
                                     const volatile bool *cancel_flag)
{
    if (curl == nullptr) {
        return;
    }

    if (cancel_flag != nullptr) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                         translator_progress_abort);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void *)cancel_flag);
    } else {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                         (curl_xferinfo_callback) nullptr);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, nullptr);
    }
}

static void translator_clear_gemini_backoff_locked(void)
{
    g_gemini_disabled_until.tv_sec = 0;
    g_gemini_disabled_until.tv_nsec = 0;
    translator_gemini_cooldown_persist_locked();
}

static void translator_clear_gemini_backoff_internal(void)
{
    ttak_mutex_lock(&g_provider_mutex);
    translator_clear_gemini_backoff_locked();
    ttak_mutex_unlock(&g_provider_mutex);
}

void translator_clear_gemini_backoff(void)
{
    translator_clear_gemini_backoff_internal();
}

static void translator_schedule_gemini_backoff_ns(long duration_ns)
{
    struct timespec now = translator_timespec_now();
    struct timespec until = translator_timespec_add_ns(&now, duration_ns);

    ttak_mutex_lock(&g_provider_mutex);
    if (duration_ns <= 0L) {
        translator_clear_gemini_backoff_locked();
    } else {
        g_gemini_disabled_until = until;
        translator_gemini_cooldown_persist_locked();
    }
    ttak_mutex_unlock(&g_provider_mutex);
}

static long translator_gemini_backoff_duration_until_midnight(void)
{
    time_t now_wall = time(nullptr);
    if (now_wall == (time_t)-1) {
        return TRANSLATOR_GEMINI_FALLBACK_DURATION_NS;
    }

    struct tm local_tm = {0};
#if defined(_POSIX_THREAD_SAFE_FUNCTIONS) && !defined(__APPLE__)
    if (localtime_r(&now_wall, &local_tm) == nullptr) {
        return TRANSLATOR_GEMINI_FALLBACK_DURATION_NS;
    }
#else
    struct tm *local_tmp = localtime(&now_wall);
    if (local_tmp == nullptr) {
        return TRANSLATOR_GEMINI_FALLBACK_DURATION_NS;
    }
    local_tm = *local_tmp;
#endif

    local_tm.tm_hour = 0;
    local_tm.tm_min = 0;
    local_tm.tm_sec = 0;
    local_tm.tm_isdst = -1;
    local_tm.tm_mday += 1;

    time_t midnight = mktime(&local_tm);
    if (midnight == (time_t)-1) {
        return TRANSLATOR_GEMINI_FALLBACK_DURATION_NS;
    }

    double seconds = difftime(midnight, now_wall);
    if (seconds <= 0.0) {
        return 1L * 1000000000L;
    }

    double nanoseconds = seconds * 1000000000.0;
    if (nanoseconds > (double)LONG_MAX) {
        return TRANSLATOR_GEMINI_FALLBACK_DURATION_NS;
    }

    long rounded = (long)(nanoseconds + 0.5);
    if (rounded <= 0L) {
        return 1L * 1000000000L;
    }

    return rounded;
}

static void translator_schedule_gemini_backoff_until_midnight(void)
{
    long duration = translator_gemini_backoff_duration_until_midnight();
    translator_schedule_gemini_backoff_ns(duration);
}

static bool
translator_gemini_backoff_active_unlocked(const struct timespec *now,
                                          struct timespec *remaining)
{
    if (g_gemini_disabled_until.tv_sec == 0 &&
        g_gemini_disabled_until.tv_nsec == 0) {
        if (remaining != nullptr) {
            remaining->tv_sec = 0;
            remaining->tv_nsec = 0;
        }
        return false;
    }

    if (now != nullptr &&
        translator_timespec_compare(now, &g_gemini_disabled_until) >= 0) {
        translator_clear_gemini_backoff_locked();
        if (remaining != nullptr) {
            remaining->tv_sec = 0;
            remaining->tv_nsec = 0;
        }
        return false;
    }

    if (now != nullptr && remaining != nullptr) {
        *remaining = translator_timespec_diff(&g_gemini_disabled_until, now);
    }

    return true;
}

static bool translator_gemini_enabled_internal(void)
{
    struct timespec now = translator_timespec_now();
    bool enabled = true;

    ttak_mutex_lock(&g_provider_mutex);
    if (g_gemini_manually_disabled) {
        enabled = false;
    } else if (translator_gemini_backoff_active_unlocked(&now, nullptr)) {
        enabled = false;
    }
    ttak_mutex_unlock(&g_provider_mutex);

    return enabled;
}

void translator_set_gemini_enabled(bool enabled)
{
    ttak_mutex_lock(&g_provider_mutex);
    g_gemini_manually_disabled = !enabled;
    if (enabled) {
        translator_clear_gemini_backoff_locked();
    }
    ttak_mutex_unlock(&g_provider_mutex);
}

bool translator_is_gemini_enabled(void)
{
    return translator_gemini_enabled_internal();
}

bool translator_is_gemini_manually_disabled(void)
{
    ttak_mutex_lock(&g_provider_mutex);
    bool disabled = g_gemini_manually_disabled;
    ttak_mutex_unlock(&g_provider_mutex);
    return disabled;
}

bool translator_gemini_backoff_remaining(struct timespec *remaining)
{
    struct timespec now = translator_timespec_now();
    bool active = false;

    ttak_mutex_lock(&g_provider_mutex);
    active = translator_gemini_backoff_active_unlocked(&now, remaining);
    ttak_mutex_unlock(&g_provider_mutex);

    return active;
}

bool translator_is_ollama_only(void)
{
    if (!translator_is_gemini_enabled()) {
        return true;
    }

    const char *api_key = getenv("GEMINI_API_KEY");
    if (api_key == nullptr || api_key[0] == '\0') {
        return true;
    }

    return false;
}

void translator_set_manual_chat_bbs_only(bool enabled)
{
    ttak_mutex_lock(&g_provider_mutex);
    g_manual_chat_bbs_only = enabled;
    if (!enabled) {
        g_manual_skip_scrollback_translation = false;
    }
    ttak_mutex_unlock(&g_provider_mutex);
}

bool translator_is_manual_chat_bbs_only(void)
{
    ttak_mutex_lock(&g_provider_mutex);
    bool limited = g_manual_chat_bbs_only;
    ttak_mutex_unlock(&g_provider_mutex);
    return limited;
}

void translator_set_manual_skip_scrollback(bool enabled)
{
    ttak_mutex_lock(&g_provider_mutex);
    g_manual_skip_scrollback_translation = enabled;
    ttak_mutex_unlock(&g_provider_mutex);
}

bool translator_is_manual_skip_scrollback(void)
{
    ttak_mutex_lock(&g_provider_mutex);
    bool skip = g_manual_skip_scrollback_translation;
    ttak_mutex_unlock(&g_provider_mutex);
    return skip;
}

bool translator_should_limit_to_chat_bbs(void)
{
    if (translator_is_ollama_only()) {
        return true;
    }

    return translator_is_manual_chat_bbs_only();
}

bool translator_should_skip_scrollback_translation(void)
{
    if (!translator_is_manual_chat_bbs_only()) {
        return false;
    }

    return translator_is_manual_skip_scrollback();
}

static size_t translator_utf8_encode(uint32_t codepoint, char *output,
                                     size_t max_len)
{
    if (output == nullptr || max_len == 0U) {
        return 0U;
    }

    if (codepoint <= 0x7FU) {
        if (max_len < 1U) {
            return 0U;
        }
        output[0] = (char)codepoint;
        return 1U;
    }

    if (codepoint <= 0x7FFU) {
        if (max_len < 2U) {
            return 0U;
        }
        output[0] = (char)(0xC0 | (codepoint >> 6));
        output[1] = (char)(0x80 | (codepoint & 0x3FU));
        return 2U;
    }

    if (codepoint <= 0xFFFFU) {
        if (max_len < 3U) {
            return 0U;
        }
        output[0] = (char)(0xE0 | (codepoint >> 12));
        output[1] = (char)(0x80 | ((codepoint >> 6) & 0x3FU));
        output[2] = (char)(0x80 | (codepoint & 0x3FU));
        return 3U;
    }

    if (codepoint <= 0x10FFFFU) {
        if (max_len < 4U) {
            return 0U;
        }
        output[0] = (char)(0xF0 | (codepoint >> 18));
        output[1] = (char)(0x80 | ((codepoint >> 12) & 0x3FU));
        output[2] = (char)(0x80 | ((codepoint >> 6) & 0x3FU));
        output[3] = (char)(0x80 | (codepoint & 0x3FU));
        return 4U;
    }

    return 0U;
}

static bool translator_parse_hex4(const char *input, uint32_t *value)
{
    if (input == nullptr || value == nullptr) {
        return false;
    }

    uint32_t result = 0U;
    for (size_t idx = 0U; idx < 4U; ++idx) {
        char ch = input[idx];
        if (ch == '\0') {
            return false;
        }
        result <<= 4U;
        if (ch >= '0' && ch <= '9') {
            result |= (uint32_t)(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            result |= (uint32_t)(10 + (ch - 'a'));
        } else if (ch >= 'A' && ch <= 'F') {
            result |= (uint32_t)(10 + (ch - 'A'));
        } else {
            return false;
        }
    }

    *value = result;
    return true;
}

static bool translator_decode_json_string(const char *input, char *output,
                                          size_t output_len,
                                          const char **end_out)
{
    if (input == nullptr || output == nullptr || output_len == 0U) {
        return false;
    }

    size_t out_idx = 0U;
    const char *cursor = input;

    while (*cursor != '\0') {
        char ch = *cursor++;
        if (ch == '"') {
            if (out_idx >= output_len) {
                output[output_len - 1U] = '\0';
            } else {
                output[out_idx] = '\0';
            }
            if (end_out != nullptr) {
                *end_out = cursor;
            }
            return true;
        }

        if (ch == '\\') {
            char next = *cursor++;
            if (next == '\0') {
                break;
            }

            switch (next) {
            case '"':
            case '\\':
            case '/':
            case 'b':
            case 'f':
            case 'n':
            case 'r':
            case 't': {
                char decoded = next;
                switch (next) {
                case 'b':
                    decoded = '\b';
                    break;
                case 'f':
                    decoded = '\f';
                    break;
                case 'n':
                    decoded = '\n';
                    break;
                case 'r':
                    decoded = '\r';
                    break;
                case 't':
                    decoded = '\t';
                    break;
                default:
                    break;
                }

                if (out_idx + 1U >= output_len) {
                    return false;
                }
                output[out_idx++] = decoded;
                break;
            }
            case 'u': {
                uint32_t codepoint = 0U;
                if (!translator_parse_hex4(cursor, &codepoint)) {
                    return false;
                }
                cursor += 4U;

                if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                    if (cursor[0] == '\\' && cursor[1] == 'u') {
                        uint32_t low = 0U;
                        if (!translator_parse_hex4(cursor + 2U, &low)) {
                            return false;
                        }
                        cursor += 6U;
                        if (low >= 0xDC00U && low <= 0xDFFFU) {
                            codepoint =
                                0x10000U + (((codepoint - 0xD800U) << 10U) |
                                            (low - 0xDC00U));
                        } else {
                            codepoint = 0xFFFD;
                        }
                    } else {
                        codepoint = 0xFFFD;
                    }
                } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
                    codepoint = 0xFFFD;
                }

                char encoded[4];
                size_t encoded_len =
                    translator_utf8_encode(codepoint, encoded, sizeof(encoded));
                if (encoded_len == 0U || out_idx + encoded_len >= output_len) {
                    return false;
                }
                memcpy(output + out_idx, encoded, encoded_len);
                out_idx += encoded_len;
                break;
            }
            default:
                if (out_idx + 1U >= output_len) {
                    return false;
                }
                output[out_idx++] = next;
                break;
            }
            continue;
        }

        if (out_idx + 1U >= output_len) {
            return false;
        }
        output[out_idx++] = ch;
    }

    return false;
}

static void translator_mark_quota_exhausted(void)
{
    ttak_mutex_lock(&g_error_mutex);
    g_last_error_was_quota = true;
    ttak_mutex_unlock(&g_error_mutex);
}

static void translator_set_error(const char *fmt, ...)
{
    ttak_mutex_lock(&g_error_mutex);
    if (fmt == nullptr) {
        g_last_error[0] = '\0';
        g_last_error_was_quota = false;
    } else {
        va_list args;
        va_start(args, fmt);
        vsnprintf(g_last_error, sizeof(g_last_error), fmt, args);
        va_end(args);
        g_last_error_was_quota = false;
    }
    ttak_mutex_unlock(&g_error_mutex);
}

const char *translator_last_error(void)
{
    static _Thread_local char snapshot[256];
    ttak_mutex_lock(&g_error_mutex);
    snprintf(snapshot, sizeof(snapshot), "%s", g_last_error);
    ttak_mutex_unlock(&g_error_mutex);
    return snapshot;
}

bool translator_last_error_was_quota(void)
{
    ttak_mutex_lock(&g_error_mutex);
    const bool was_quota = g_last_error_was_quota;
    ttak_mutex_unlock(&g_error_mutex);
    return was_quota;
}

void translator_global_init(void)
{
    translator_mutex_init_all();
    ttak_mutex_lock(&g_init_mutex);
    if (!g_curl_initialised) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {
            g_curl_initialised = true;
        }
    }
    if (!g_gemini_cooldown_file_initialised) {
        translator_gemini_cooldown_resolve_path();
        translator_gemini_cooldown_load();
        g_gemini_cooldown_file_initialised = true;
    }
    ttak_mutex_unlock(&g_init_mutex);
}

void translator_global_cleanup(void)
{
    translator_mutex_init_all();
    ttak_mutex_lock(&g_init_mutex);
    if (g_curl_initialised) {
        curl_global_cleanup();
        g_curl_initialised = false;
    }
    ttak_mutex_unlock(&g_init_mutex);

    ttak_mutex_lock(&g_memory_mutex);
    if (g_translator_memory_context != nullptr) {
        sshc_memory_context_destroy(g_translator_memory_context);
        g_translator_memory_context = nullptr;
    }
    ttak_mutex_unlock(&g_memory_mutex);
}

static size_t translator_write_callback(void *contents, size_t size,
                                        size_t nmemb, void *userp)
{
    const size_t total = size * nmemb;
    translator_buffer_t *buffer = (translator_buffer_t *)userp;
    if (buffer == nullptr) {
        return 0U;
    }

    if (total == 0U) {
        return 0U;
    }

    if (buffer->bytes.length + total + 1U > TRANSLATOR_MAX_RESPONSE) {
        return 0U;
    }

    return sshc_abstract_byte_buffer_append(&buffer->bytes, contents, total)
               ? total
               : 0U;
}

static char *translator_escape_string(const char *input)
{
    if (input == nullptr) {
        return nullptr;
    }

    size_t required = 1U; // null terminator
    for (const unsigned char *cursor = (const unsigned char *)input;
         *cursor != '\0'; ++cursor) {
        switch (*cursor) {
        case '\\':
        case '"':
            required += 2U;
            break;
        case '\n':
        case '\r':
        case '\t':
            required += 2U;
            break;
        default:
            required += 1U;
            break;
        }
    }

    char *escaped = sshc_gc_malloc(required);
    if (escaped == nullptr) {
        return nullptr;
    }

    size_t offset = 0U;
    for (const unsigned char *cursor = (const unsigned char *)input;
         *cursor != '\0'; ++cursor) {
        switch (*cursor) {
        case '\\':
        case '"':
            escaped[offset++] = '\\';
            escaped[offset++] = (char)*cursor;
            break;
        case '\n':
            escaped[offset++] = '\\';
            escaped[offset++] = 'n';
            break;
        case '\r':
            escaped[offset++] = '\\';
            escaped[offset++] = 'r';
            break;
        case '\t':
            escaped[offset++] = '\\';
            escaped[offset++] = 't';
            break;
        default:
            escaped[offset++] = (char)*cursor;
            break;
        }
    }

    escaped[offset] = '\0';
    return escaped;
}

static const char *translator_skip_whitespace(const char *cursor)
{
    if (cursor == nullptr) {
        return nullptr;
    }

    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        ++cursor;
    }

    return cursor;
}

static char *translator_extract_first_text_generic(const char *response)
{
    if (response == nullptr) {
        return nullptr;
    }

    const char *text_marker = "\"text\"";
    const size_t text_marker_len = strlen(text_marker);
    const char *cursor = response;

    while ((cursor = strstr(cursor, text_marker)) != nullptr) {
        const char *value_start = cursor + text_marker_len;
        value_start = translator_skip_whitespace(value_start);
        if (value_start == nullptr || *value_start != ':') {
            cursor += text_marker_len;
            continue;
        }

        ++value_start;
        value_start = translator_skip_whitespace(value_start);
        if (value_start == nullptr || *value_start != '"') {
            cursor += text_marker_len;
            continue;
        }

        ++value_start;

        size_t capacity = strlen(value_start) + 1U;
        char *candidate = sshc_gc_malloc(capacity);
        if (candidate == nullptr) {
            return nullptr;
        }

        const char *after_string = nullptr;
        if (!translator_decode_json_string(value_start, candidate, capacity,
                                           &after_string)) {
            return nullptr;
        }

        if (candidate[0] != '\0') {
            return candidate;
        }

        if (after_string == nullptr) {
            break;
        }
        cursor = after_string;
    }

    return nullptr;
}

static char *translator_extract_payload_text(const char *response)
{
    if (response == nullptr) {
        return nullptr;
    }

    const char *text_marker = "\"text\"";
    const size_t text_marker_len = strlen(text_marker);
    const char *cursor = response;
    char *latest_payload = nullptr;

    while ((cursor = strstr(cursor, text_marker)) != nullptr) {
        const char *value_start = cursor + text_marker_len;
        value_start = translator_skip_whitespace(value_start);
        if (value_start == nullptr || *value_start != ':') {
            cursor += text_marker_len;
            continue;
        }

        ++value_start;
        value_start = translator_skip_whitespace(value_start);
        if (value_start == nullptr || *value_start != '"') {
            cursor += text_marker_len;
            continue;
        }

        ++value_start;

        size_t capacity = strlen(value_start) + 1U;
        char *candidate = sshc_gc_malloc(capacity);
        if (candidate == nullptr) {
            return nullptr;
        }

        const char *after_string = nullptr;
        if (!translator_decode_json_string(value_start, candidate, capacity,
                                           &after_string)) {
            return nullptr;
        }

        if (candidate[0] == '{' &&
            strstr(candidate, "\"translation\"") != nullptr) {
            latest_payload = candidate;
        } else {
        }

        if (after_string == nullptr) {
            break;
        }

        cursor = after_string;
    }

    if (latest_payload == nullptr) {
        const char *error_marker = "\"message\":\"";
        const char *error_pos = strstr(response, error_marker);
        if (error_pos != nullptr) {
            fprintf(stderr, "API Error Message Found in Response.\n");
        }
    }

    return latest_payload;
}

static bool translator_extract_plaintext_response(const char *response,
                                                  char *dest, size_t dest_len)
{
    if (dest == nullptr || dest_len == 0U) {
        return false;
    }

    dest[0] = '\0';

    char *payload = translator_extract_first_text_generic(response);
    if (payload == nullptr) {
        return false;
    }

    snprintf(dest, dest_len, "%s", payload);
    return dest[0] != '\0';
}

static bool translator_extract_json_value(const char *json, const char *key,
                                          char *dest, size_t dest_len)
{
    if (json == nullptr || key == nullptr || dest == nullptr ||
        dest_len == 0U) {
        return false;
    }

    const char *key_pos = strstr(json, key);
    if (key_pos == nullptr) {
        dest[0] = '\0';
        return false;
    }

    key_pos += strlen(key);
    while (*key_pos != '\0' && *key_pos != '"') {
        ++key_pos;
    }
    if (*key_pos != '"') {
        dest[0] = '\0';
        return false;
    }

    ++key_pos;
    const char *after_value = nullptr;
    if (!translator_decode_json_string(key_pos, dest, dest_len, &after_value)) {
        dest[0] = '\0';
        return false;
    }

    return true;
}

static char *translator_extract_last_text_block(const char *response)
{
    if (response == nullptr) {
        return nullptr;
    }

    const char *text_marker = "\"text\"";
    const size_t text_marker_len = strlen(text_marker);
    const char *cursor = response;
    char *latest_payload = nullptr;

    while ((cursor = strstr(cursor, text_marker)) != nullptr) {
        const char *value_start = cursor + text_marker_len;
        value_start = translator_skip_whitespace(value_start);
        if (value_start == nullptr || *value_start != ':') {
            cursor += text_marker_len;
            continue;
        }

        ++value_start;
        value_start = translator_skip_whitespace(value_start);
        if (value_start == nullptr || *value_start != '"') {
            cursor += text_marker_len;
            continue;
        }

        ++value_start;

        size_t capacity = strlen(value_start) + 1U;
        char *candidate = sshc_gc_malloc(capacity);
        if (candidate == nullptr) {
            return nullptr;
        }

        const char *after_string = nullptr;
        if (!translator_decode_json_string(value_start, candidate, capacity,
                                           &after_string)) {
            return nullptr;
        }

        latest_payload = candidate;

        if (after_string == nullptr) {
            break;
        }

        cursor = after_string;
    }

    return latest_payload;
}

static bool translator_string_contains_case_insensitive(const char *haystack,
                                                        const char *needle);

static bool translator_parse_moderation_decision(const char *json,
                                                 bool *blocked, char *reason,
                                                 size_t reason_len)
{
    if (reason != nullptr && reason_len > 0U) {
        reason[0] = '\0';
    }

    bool local_block = false;
    bool has_block_value = false;

    if (json != nullptr) {
        const char *block_pos = strstr(json, "\"block\"");
        if (block_pos != nullptr) {
            const char *value = strchr(block_pos, ':');
            if (value != nullptr) {
                ++value;
                value = translator_skip_whitespace(value);
                if (value != nullptr) {
                    char token[6];
                    size_t idx = 0U;
                    memset(token, 0, sizeof(token));
                    while (value[idx] != '\0' && idx < sizeof(token) - 1U &&
                           isalpha((unsigned char)value[idx])) {
                        token[idx] = (char)tolower((unsigned char)value[idx]);
                        ++idx;
                    }
                    token[idx] = '\0';
                    if (strcmp(token, "true") == 0) {
                        local_block = true;
                        has_block_value = true;
                    } else if (strcmp(token, "false") == 0) {
                        local_block = false;
                        has_block_value = true;
                    }
                }
            }
        }

        if (reason != nullptr && reason_len > 0U) {
            if (!translator_extract_json_value(json, "\"reason\"", reason,
                                               reason_len)) {
                reason[0] = '\0';
            }
            if (local_block && reason[0] == '\0') {
                snprintf(reason, reason_len, "%s",
                         "Potential intrusion content detected");
            }
        }

        if (local_block && reason != nullptr && reason[0] != '\0') {
            if (translator_string_contains_case_insensitive(reason, "no issue") ||
                translator_string_contains_case_insensitive(reason, "no issues") ||
                translator_string_contains_case_insensitive(reason, "safe") ||
                translator_string_contains_case_insensitive(reason, "allowed") ||
                translator_string_contains_case_insensitive(reason, "clean") ||
                translator_string_contains_case_insensitive(reason, "문제없") ||
                translator_string_contains_case_insensitive(reason, "정상")) {
                local_block = false;
                reason[0] = '\0';
            }
        }
    }

    if (blocked != nullptr) {
        *blocked = has_block_value ? local_block : false;
    }

    return json != nullptr;
}

static bool translator_string_contains_case_insensitive(const char *haystack,
                                                        const char *needle)
{
    if (haystack == nullptr || needle == nullptr || *needle == '\0') {
        return false;
    }

    size_t haystack_len = strlen(haystack);
    size_t needle_len = strlen(needle);
    if (needle_len > haystack_len) {
        return false;
    }

    for (size_t idx = 0U; idx + needle_len <= haystack_len; ++idx) {
        size_t matched = 0U;
        while (matched < needle_len &&
               tolower((unsigned char)haystack[idx + matched]) ==
                   tolower((unsigned char)needle[matched])) {
            ++matched;
        }
        if (matched == needle_len) {
            return true;
        }
    }

    return false;
}

static const char *const TRANSLATOR_OLLAMA_NON_STREAM_BODY_FORMAT =
    "{"
    "\"model\":\"%s\","
    "\"prompt\":\"%s\","
    "\"system\":\"%s\","
    "\"stream\":false"
    "}";
static const char *const TRANSLATOR_MODERATION_TRIGGER_TOKEN =
    "cucumber-ballet-fly-tetromino";

static bool translator_moderation_should_run(const char *content)
{
    return translator_string_contains_case_insensitive(
        content, TRANSLATOR_MODERATION_TRIGGER_TOKEN);
}

static char *translator_build_gemini_url(const char *base, const char *model,
                                         const char *api_key, bool stream_mode)
{
    if (api_key == nullptr || api_key[0] == '\0') {
        return nullptr;
    }

    if (base == nullptr || base[0] == '\0') {
        base = TRANSLATOR_DEFAULT_BASE_URL;
    }

    if (model == nullptr || model[0] == '\0') {
        model = TRANSLATOR_DEFAULT_MODEL;
    }

    size_t base_len = strlen(base);
    bool base_has_slash = (base_len > 0U && base[base_len - 1U] == '/');
    const char *models_prefix = "models/";
    size_t models_prefix_len = strlen(models_prefix);
    size_t model_len = strlen(model);
    const char *suffix =
        stream_mode ? ":streamGenerateContent" : ":generateContent";
    const char *query_prefix = stream_mode ? "?alt=sse&key=" : "?key=";

    size_t total = base_len + (base_has_slash ? 0U : 1U) + models_prefix_len +
                   model_len + strlen(suffix) + strlen(query_prefix) +
                   strlen(api_key) + 1U;

    char *url = sshc_gc_malloc(total);
    if (url == nullptr) {
        return nullptr;
    }

    snprintf(url, total, "%s%s%s%s%s%s%s", base, base_has_slash ? "" : "/",
             models_prefix, model, suffix, query_prefix, api_key);
    return url;
}

static char *translator_build_ollama_url(const char *base)
{
    const char *address = base;
    if (address == nullptr || address[0] == '\0') {
        address = "http://127.0.0.1:11434";
    }

    size_t base_len = strlen(address);
    bool has_trailing_slash = base_len > 0U && address[base_len - 1U] == '/';
    const char *suffix = "api/generate";
    size_t total =
        base_len + (has_trailing_slash ? 0U : 1U) + strlen(suffix) + 1U;

    char *url = sshc_gc_malloc(total);
    if (url == nullptr) {
        return nullptr;
    }

    snprintf(url, total, "%s%s%s", address, has_trailing_slash ? "" : "/",
             suffix);
    return url;
}

static CURLcode translator_issue_gemini_request(
    CURL *curl, const char *url, const char *api_key, const char *body,
    bool stream_mode, const volatile bool *cancel_flag,
    translator_buffer_t *buffer, long *status)
{
    if (curl == nullptr || url == nullptr || body == nullptr ||
        buffer == nullptr) {
        return CURLE_FAILED_INIT;
    }

    translator_buffer_reset(buffer);

    long local_status = 0L;
    long *status_out = status != nullptr ? status : &local_status;

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (stream_mode) {
        headers = curl_slist_append(headers, "Accept: text/event-stream");
    }

    if (api_key != nullptr && api_key[0] != '\0') {
        size_t header_len = strlen("x-goog-api-key: ") + strlen(api_key) + 1U;
        char *header_value = sshc_gc_malloc(header_len);
        if (header_value != nullptr) {
            snprintf(header_value, header_len, "x-goog-api-key: %s", api_key);
            headers = curl_slist_append(headers, header_value);
        }
    }

    translator_rate_limit_wait();
    if (translator_cancel_requested(cancel_flag)) {
        curl_slist_free_all(headers);
        return CURLE_ABORTED_BY_CALLBACK;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, translator_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buffer);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                     TRANSLATOR_CONNECT_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, TRANSLATOR_TOTAL_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    translator_configure_cancel_callback(curl, cancel_flag);

    CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status_out);
    translator_rate_limit_penalize(*status_out);

    curl_slist_free_all(headers);
    return result;
}

static bool translator_handle_payload(const char *response, char *translation,
                                      size_t translation_len,
                                      char *detected_language,
                                      size_t detected_len)
{
    if (response == nullptr || translation == nullptr ||
        translation_len == 0U) {
        return false;
    }

    char *payload = translator_extract_payload_text(response);
    if (payload == nullptr) {
        translator_set_error("Unable to parse Gemini translation payload.");
        return false;
    }

    if (detected_language != nullptr && detected_len > 0U) {
        detected_language[0] = '\0';
    }

    char detected[64];
    char translated[TRANSLATOR_MAX_RESPONSE];
    detected[0] = '\0';
    translated[0] = '\0';

    (void)translator_extract_json_value(payload, "\"detected_language\"",
                                        detected, sizeof(detected));
    if (!translator_extract_json_value(payload, "\"translation\"", translated,
                                       sizeof(translated))) {
        translator_set_error(
            "Gemini response did not contain a translation field.");
        return false;
    }

    if (detected_language != nullptr && detected_len > 0U) {
        snprintf(detected_language, detected_len, "%s", detected);
    }

    snprintf(translation, translation_len, "%s", translated);
    translator_set_error(nullptr);
    return true;
}

static CURLcode translator_issue_json_post(
    CURL *curl, const char *url, const char *body, const char *auth_header_name,
    const char *auth_header_value, const char *const *extra_headers,
    const volatile bool *cancel_flag, translator_buffer_t *buffer, long *status)
{
    if (curl == nullptr || url == nullptr || body == nullptr ||
        buffer == nullptr) {
        return CURLE_FAILED_INIT;
    }

    translator_buffer_reset(buffer);

    long local_status = 0L;
    long *status_out = status != nullptr ? status : &local_status;

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (auth_header_name != nullptr && auth_header_value != nullptr) {
        size_t header_len =
            strlen(auth_header_name) + 2U + strlen(auth_header_value) + 1U;
        char *header_value = sshc_gc_malloc(header_len);
        if (header_value != nullptr) {
            snprintf(header_value, header_len, "%s: %s", auth_header_name,
                     auth_header_value);
            headers = curl_slist_append(headers, header_value);
        }
    }

    if (extra_headers != nullptr) {
        for (size_t idx = 0U; extra_headers[idx] != nullptr; ++idx) {
            headers = curl_slist_append(headers, extra_headers[idx]);
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, translator_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buffer);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                     TRANSLATOR_CONNECT_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, TRANSLATOR_TOTAL_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    translator_configure_cancel_callback(curl, cancel_flag);

    if (translator_cancel_requested(cancel_flag)) {
        curl_slist_free_all(headers);
        return CURLE_ABORTED_BY_CALLBACK;
    }

    CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status_out);

    curl_slist_free_all(headers);
    return result;
}

static bool translator_candidate_configure(translator_candidate_t *candidate,
                                           translator_provider_t provider,
                                           const char *model)
{
    if (candidate == nullptr) {
        return false;
    }

    const char *api_key = nullptr;
    const char *api_key_name = nullptr;

    switch (provider) {
    case TRANSLATOR_PROVIDER_GEMINI:
        api_key = getenv("GEMINI_API_KEY");
        api_key_name = "GEMINI_API_KEY";
        break;
    case TRANSLATOR_PROVIDER_OLLAMA:
        api_key = nullptr;
        api_key_name = nullptr;
        if (model == nullptr || model[0] == '\0') {
            model = "zongwei/gemma3-translator:1b";
        }
        break;
    }

    if ((api_key == nullptr || api_key[0] == '\0') &&
        provider != TRANSLATOR_PROVIDER_OLLAMA) {
        return false;
    }

    candidate->provider = provider;
    candidate->model = model;
    candidate->api_key = api_key;
    candidate->api_key_name = api_key_name;
    return true;
}

static bool
translator_candidate_is_duplicate(const translator_candidate_t *candidates,
                                  size_t count, translator_provider_t provider,
                                  const char *model)
{
    if (candidates == nullptr) {
        return false;
    }

    for (size_t idx = 0U; idx < count; ++idx) {
        const char *existing_model =
            candidates[idx].model != nullptr ? candidates[idx].model : "";
        const char *candidate_model = model != nullptr ? model : "";
        if (candidates[idx].provider == provider &&
            strcmp(existing_model, candidate_model) == 0) {
            return true;
        }
    }

    return false;
}

static bool translator_add_candidate(translator_candidate_t *candidates,
                                     size_t *count, size_t capacity,
                                     translator_provider_t provider,
                                     const char *model)
{
    if (candidates == nullptr || count == nullptr || *count >= capacity) {
        return false;
    }

    if (translator_candidate_is_duplicate(candidates, *count, provider,
                                          model)) {
        return false;
    }

    if (!translator_candidate_configure(&candidates[*count], provider, model)) {
        return false;
    }

    ++(*count);
    return true;
}

static bool translator_try_gemini(const translator_candidate_t *candidate,
                                  const char *text, const char *target_language,
                                  char *translation, size_t translation_len,
                                  char *detected_language, size_t detected_len,
                                  const volatile bool *cancel_flag,
                                  bool *retryable)
{
    if (retryable != nullptr) {
        *retryable = false;
    }

    if (candidate == nullptr) {
        translator_set_error("Gemini provider is not configured.");
        if (retryable != nullptr) {
            *retryable = true;
        }
        return false;
    }

    const char *api_key = candidate->api_key;
    if ((api_key == nullptr || api_key[0] == '\0')) {
        translator_set_error("GEMINI_API_KEY is not configured.");
        if (retryable != nullptr) {
            *retryable = true;
        }
        return false;
    }

    if (translator_cancel_requested(cancel_flag)) {
        translator_set_error("Translation canceled.");
        return false;
    }

    const char *base = getenv("GEMINI_API_BASE");
    if (base == nullptr || base[0] == '\0') {
        base = getenv("GEMINI_BASE_URL");
    }

    const char *model_name =
        candidate->model != nullptr && candidate->model[0] != '\0'
            ? candidate->model
            : TRANSLATOR_DEFAULT_MODEL;

    char *escaped_text = translator_escape_string(text);
    char *escaped_target = translator_escape_string(target_language);
    if (escaped_text == nullptr || escaped_target == nullptr) {
        translator_set_error("Failed to prepare translation request payload.");
        if (retryable != nullptr) {
            *retryable = false;
        }
        return false;
    }

    char *api_url =
        translator_build_gemini_url(base, model_name, api_key, false);
    char *stream_url =
        translator_build_gemini_url(base, model_name, api_key, true);
    if (api_url == nullptr && stream_url == nullptr) {
        translator_set_error("Failed to build Gemini API URL.");
        if (retryable != nullptr) {
            *retryable = false;
        }
        return false;
    }

    static const char body_format[] =
        "{"
        "\"system_instruction\":{"
        "\"parts\":[{\"text\":\"You are a translation engine that detects the "
        "source language of text and translates it to a requested target "
        "language. Preserve tokens like [[ANSI0]] or [[SEG00]] unchanged. "
        "Respond only with a JSON object containing keys detected_language and "
        "translation.\"}]"
        "},"
        "\"contents\":["
        "{"
        "\"role\":\"user\","
        "\"parts\":["
        "{\"text\":\"Target language: %s\\nText: %s\"}"
        "]"
        "}"
        "],"
        "\"generationConfig\":{\"responseMimeType\":\"application/json\"}"
        "}";

    int computed =
        snprintf(nullptr, 0, body_format, escaped_target, escaped_text);
    if (computed < 0) {
        translator_set_error("Failed to prepare translation request payload.");
        if (retryable != nullptr) {
            *retryable = false;
        }
        return false;
    }

    size_t body_len = (size_t)computed + 1U;
    char *body = sshc_gc_malloc(body_len);
    if (body == nullptr) {
        translator_set_error("Failed to prepare translation request payload.");
        if (retryable != nullptr) {
            *retryable = false;
        }
        return false;
    }

    int written =
        snprintf(body, body_len, body_format, escaped_target, escaped_text);
    if (written < 0 || (size_t)written >= body_len) {
        translator_set_error("Failed to prepare translation request payload.");
        if (retryable != nullptr) {
            *retryable = false;
        }
        return false;
    }

    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        translator_set_error("Failed to initialise CURL.");
        if (retryable != nullptr) {
            *retryable = false;
        }
        return false;
    }

    translator_buffer_t buffer;
    translator_buffer_t stream_buffer;
    translator_buffer_init(&buffer);
    translator_buffer_init(&stream_buffer);
    bool success = false;
    bool attempted_request = false;
    bool request_failed = false;
    bool cancelled = false;
    bool rate_limited = false;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    if (stream_url != nullptr) {
        long stream_status = 0L;
        CURLcode stream_result = translator_issue_gemini_request(
            curl, stream_url, api_key, body, true, cancel_flag, &stream_buffer,
            &stream_status);
        if (stream_result == CURLE_ABORTED_BY_CALLBACK ||
            translator_cancel_requested(cancel_flag)) {
            cancelled = true;
            translator_set_error("Translation canceled.");
        } else if (stream_result == CURLE_OK && stream_status >= 200L &&
                   stream_status < 300L &&
                   translator_buffer_has_data(&stream_buffer)) {
            sshc_abstract_byte_buffer_view_t stream_view = {0};
            if (translator_buffer_map_read(&stream_buffer, &stream_view) &&
                translator_handle_payload(stream_view.data, translation,
                                          translation_len, detected_language,
                                          detected_len)) {
                success = true;
            }
            sshc_abstract_byte_buffer_unmap(&stream_view);
        }
    }

    if (!success && !cancelled && api_url != nullptr) {
        attempted_request = true;
        translator_set_error(nullptr);
        long status = 0L;
        CURLcode result = translator_issue_gemini_request(
            curl, api_url, api_key, body, false, cancel_flag, &buffer, &status);
        if (result == CURLE_ABORTED_BY_CALLBACK ||
            translator_cancel_requested(cancel_flag)) {
            cancelled = true;
            translator_set_error("Translation canceled.");
        } else if (result != CURLE_OK) {
            translator_set_error("Failed to contact Gemini API: %s",
                                 curl_easy_strerror(result));
            if (retryable != nullptr) {
                *retryable = true;
            }
            request_failed = true;
        } else if (status < 200L || status >= 300L ||
                   !translator_buffer_has_data(&buffer)) {
            char message[256];
            message[0] = '\0';
            sshc_abstract_byte_buffer_view_t buffer_view = {0};
            if (translator_buffer_map_read(&buffer, &buffer_view)) {
                (void)translator_extract_json_value(buffer_view.data,
                                                    "\"message\"", message,
                                                    sizeof(message));
            }
            sshc_abstract_byte_buffer_unmap(&buffer_view);
            const bool quota_like =
                status == 429L ||
                translator_string_contains_case_insensitive(message, "quota") ||
                translator_string_contains_case_insensitive(message, "limit") ||
                translator_string_contains_case_insensitive(message, "exhaust");
            if (status == 429L || status == 404L ||
                translator_string_contains_case_insensitive(message, "quota") ||
                translator_string_contains_case_insensitive(message,
                                                            "not found")) {
                if (retryable != nullptr) {
                    *retryable = true;
                }
            }
            if (status == 429L) {
                rate_limited = true;
            }
            if (message[0] != '\0') {
                translator_set_error("Gemini (%s) HTTP %ld: %s", model_name,
                                     status, message);
            } else if (status != 0L) {
                translator_set_error("Gemini (%s) returned HTTP %ld.",
                                     model_name, status);
            } else {
                translator_set_error("Gemini (%s) returned an empty response.",
                                     model_name);
            }
            if (quota_like) {
                translator_mark_quota_exhausted();
            }
            request_failed = true;
        } else {
            sshc_abstract_byte_buffer_view_t buffer_view = {0};
            if (translator_buffer_map_read(&buffer, &buffer_view) &&
                translator_handle_payload(buffer_view.data, translation,
                                          translation_len,
                                          detected_language, detected_len)) {
                success = true;
            } else {
                request_failed = true;
            }
            sshc_abstract_byte_buffer_unmap(&buffer_view);
        }
    }

    if (success) {
        translator_clear_gemini_backoff();
    } else if (!cancelled && attempted_request && request_failed) {
        if (rate_limited) {
            translator_schedule_gemini_backoff_ns(
                TRANSLATOR_GEMINI_RATE_LIMIT_DURATION_NS);
        } else {
            translator_schedule_gemini_backoff_until_midnight();
        }
    }

    translator_buffer_free(&stream_buffer);
    translator_buffer_free(&buffer);
    curl_easy_cleanup(curl);

    return success;
}

static bool translator_try_gemini_eliza(const translator_candidate_t *candidate,
                                        const char *prompt, char *reply,
                                        size_t reply_len, bool *retryable)
{
    if (retryable != nullptr) {
        *retryable = false;
    }

    if (candidate == nullptr) {
        translator_set_error("Gemini provider is not configured.");
        if (retryable != nullptr) {
            *retryable = true;
        }
        return false;
    }

    if (reply == nullptr || reply_len == 0U || prompt == nullptr) {
        translator_set_error("Invalid eliza prompt.");
        return false;
    }

    const char *api_key = candidate->api_key;
    if (api_key == nullptr || api_key[0] == '\0') {
        translator_set_error("GEMINI_API_KEY is not configured.");
        if (retryable != nullptr) {
            *retryable = true;
        }
        return false;
    }

    const char *base = getenv("GEMINI_API_BASE");
    if (base == nullptr || base[0] == '\0') {
        base = getenv("GEMINI_BASE_URL");
    }

    const char *model_name =
        candidate->model != nullptr && candidate->model[0] != '\0'
            ? candidate->model
            : TRANSLATOR_DEFAULT_MODEL;

    static const char *system_prompt =
        "You are eliza, a calm and empathetic moderator for a retro terminal "
        "chat room. Offer concise, supportive "
        "guidance while reinforcing community rules. Keep replies under three "
        "sentences, avoid roleplay as law "
        "enforcement, and respond in the same language as the user whenever "
        "possible."
        "don't be too strict. A user can use some words like \"war\", "
        "\"terror\", \"kill\""
        "But if someone say the exact place, and time, specified plans to do it"
        "Or the link that is sent by user is well known as illegal and "
        "dangerous one"
        "you can kick one."
        "for example, I will kill you, motherfucker. <- don't kick"
        "I will commit a terror at 9 am, Gwanghwamun. Nov, 11. I will use the "
        "fireworks to terror people, later I will use chlorine to finish it"
        "I've already bought all of them, here is the receipt,"
        "<- kick him";

    char *escaped_prompt = translator_escape_string(prompt);
    char *escaped_system = translator_escape_string(system_prompt);
    if (escaped_prompt == nullptr || escaped_system == nullptr) {
        translator_set_error("Failed to prepare eliza request payload.");
        return false;
    }

    char *api_url =
        translator_build_gemini_url(base, model_name, api_key, false);
    if (api_url == nullptr) {
        translator_set_error("Failed to build Gemini API URL.");
        return false;
    }

    static const char body_format[] =
        "{"
        "\"system_instruction\":{"
        "\"parts\":[{\"text\":\"%s\"}]"
        "},"
        "\"contents\":["
        "{"
        "\"role\":\"user\","
        "\"parts\":[{\"text\":\"%s\"}]"
        "}"
        "],"
        "\"generationConfig\":{\"responseMimeType\":\"text/"
        "plain\",\"temperature\":0.4,\"maxOutputTokens\":256}"
        "}";

    int computed =
        snprintf(nullptr, 0, body_format, escaped_system, escaped_prompt);
    if (computed < 0) {
        translator_set_error("Failed to prepare eliza request payload.");
        return false;
    }

    size_t body_len = (size_t)computed + 1U;
    char *body = sshc_gc_malloc(body_len);
    if (body == nullptr) {
        translator_set_error("Failed to prepare eliza request payload.");
        return false;
    }

    int written =
        snprintf(body, body_len, body_format, escaped_system, escaped_prompt);
    if (written < 0 || (size_t)written >= body_len) {
        translator_set_error("Failed to prepare eliza request payload.");
        return false;
    }

    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        translator_set_error("Failed to initialise HTTP client.");
        return false;
    }

    translator_buffer_t buffer;
    translator_buffer_init(&buffer);
    long status = 0L;
    bool success = false;
    bool request_failed = false;
    bool rate_limited = false;

    CURLcode result = translator_issue_gemini_request(
        curl, api_url, api_key, body, false, nullptr, &buffer, &status);
    if (result != CURLE_OK) {
        translator_set_error("Failed to contact Gemini API: %s",
                             curl_easy_strerror(result));
        if (retryable != nullptr) {
            *retryable = true;
        }
        request_failed = true;
    } else if (status < 200L || status >= 300L ||
               !translator_buffer_has_data(&buffer)) {
        char message[256];
        message[0] = '\0';
        sshc_abstract_byte_buffer_view_t buffer_view = {0};
        if (translator_buffer_map_read(&buffer, &buffer_view)) {
            (void)translator_extract_json_value(buffer_view.data, "\"message\"",
                                                message, sizeof(message));
        }
        sshc_abstract_byte_buffer_unmap(&buffer_view);

        const bool quota_like =
            status == 429L ||
            translator_string_contains_case_insensitive(message, "quota") ||
            translator_string_contains_case_insensitive(message, "limit") ||
            translator_string_contains_case_insensitive(message, "exhaust") ||
            translator_string_contains_case_insensitive(message, "not found");

        if (status == 429L || status == 404L || quota_like) {
            if (retryable != nullptr) {
                *retryable = true;
            }
        }
        if (status == 429L) {
            rate_limited = true;
        }

        if (message[0] != '\0') {
            translator_set_error("Gemini (%s) HTTP %ld: %s", model_name, status,
                                 message);
        } else if (status != 0L) {
            translator_set_error("Gemini (%s) returned HTTP %ld.", model_name,
                                 status);
        } else {
            translator_set_error("Gemini (%s) returned an empty response.",
                                 model_name);
        }

        if (quota_like) {
            translator_mark_quota_exhausted();
        }
        request_failed = true;
    } else {
        sshc_abstract_byte_buffer_view_t buffer_view = {0};
        if (!translator_buffer_map_read(&buffer, &buffer_view) ||
            !translator_extract_plaintext_response(buffer_view.data, reply,
                                                   reply_len)) {
            translator_set_error("Gemini response did not include text output.");
            request_failed = true;
        } else {
            translator_set_error(nullptr);
            success = true;
        }
        sshc_abstract_byte_buffer_unmap(&buffer_view);
    }

    if (success) {
        translator_clear_gemini_backoff();
    } else if (request_failed) {
        if (rate_limited) {
            translator_schedule_gemini_backoff_ns(
                TRANSLATOR_GEMINI_RATE_LIMIT_DURATION_NS);
        } else {
            translator_schedule_gemini_backoff_until_midnight();
        }
    }

    translator_buffer_free(&buffer);
    curl_easy_cleanup(curl);

    return success;
}

static bool
translator_try_gemini_moderation(const translator_candidate_t *candidate,
                                 const char *category, const char *content,
                                 bool *blocked, char *reason, size_t reason_len,
                                 bool *retryable)
{
    if (retryable != nullptr) {
        *retryable = false;
    }

    if (candidate == nullptr) {
        translator_set_error("Gemini provider is not configured.");
        if (retryable != nullptr) {
            *retryable = true;
        }
        return false;
    }

    const char *api_key = candidate->api_key;
    if (api_key == nullptr || api_key[0] == '\0') {
        translator_set_error("GEMINI_API_KEY is not configured.");
        if (retryable != nullptr) {
            *retryable = true;
        }
        return false;
    }

    const char *base = getenv("GEMINI_API_BASE");
    if (base == nullptr || base[0] == '\0') {
        base = getenv("GEMINI_BASE_URL");
    }

    const char *model_name =
        candidate->model != nullptr && candidate->model[0] != '\0'
            ? candidate->model
            : TRANSLATOR_DEFAULT_MODEL;
    const char *label =
        (category != nullptr && category[0] != '\0') ? category : "submission";

    char *escaped_category = translator_escape_string(label);
    char *escaped_content =
        translator_escape_string(content != nullptr ? content : "");
    if (escaped_category == nullptr || escaped_content == nullptr) {
        translator_set_error("Failed to prepare moderation request payload.");
        return false;
    }

    char *api_url =
        translator_build_gemini_url(base, model_name, api_key, false);
    if (api_url == nullptr) {
        translator_set_error("Failed to build Gemini API URL.");
        return false;
    }

    static const char body_format[] =
        "{"
        "\"system_instruction\":{"
        "\"parts\":[{\"text\":\"You are a security reviewer for a retro "
        "terminal BBS. Allow normal conversation, jokes, or "
        "emotional support. Only block content that clearly attempts to hack, "
        "exploit, spread malware, or steal credentials. "
        "If uncertain, allow the message. Only moderate content when it "
        "includes the token cucumber-ballet-fly-tetromino. "
        "If token is missing, reply with block=false and empty reason. "
        "Respond ONLY with JSON containing boolean block and string reason.\"}]"
        "},"
        "\"contents\":["
        "{"
        "\"role\":\"user\","
        "\"parts\":["
        "{\"text\":\"Category: %s\\nText:\\n%s\\nReturn only JSON with keys "
        "block and reason.\"}"
        "]"
        "}"
        "],"
        "\"generationConfig\":{\"responseMimeType\":\"application/json\"}"
        "}";

    int body_length =
        snprintf(nullptr, 0, body_format, escaped_category, escaped_content);
    if (body_length < 0) {
        translator_set_error("Failed to prepare moderation request payload.");
        return false;
    }

    size_t body_size = (size_t)body_length + 1U;
    char *body = sshc_gc_malloc(body_size);
    if (body == nullptr) {
        translator_set_error("Failed to prepare moderation request payload.");
        return false;
    }

    int written = snprintf(body, body_size, body_format, escaped_category,
                           escaped_content);
    if (written < 0 || (size_t)written >= body_size) {
        translator_set_error("Failed to prepare moderation request payload.");
        return false;
    }

    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        translator_set_error("Failed to initialise CURL.");
        return false;
    }

    translator_buffer_t buffer;
    translator_buffer_init(&buffer);
    long status = 0L;
    CURLcode result = translator_issue_gemini_request(
        curl, api_url, api_key, body, false, nullptr, &buffer, &status);

    bool success = false;
    if (result != CURLE_OK) {
        translator_set_error("Failed to contact Gemini API: %s",
                             curl_easy_strerror(result));
        if (retryable != nullptr) {
            *retryable = true;
        }
    } else if (status < 200L || status >= 300L ||
               !translator_buffer_has_data(&buffer)) {
        char message[256];
        message[0] = '\0';
        sshc_abstract_byte_buffer_view_t buffer_view = {0};
        if (translator_buffer_map_read(&buffer, &buffer_view)) {
            (void)translator_extract_json_value(buffer_view.data, "\"message\"",
                                                message, sizeof(message));
        }
        sshc_abstract_byte_buffer_unmap(&buffer_view);
        if (status == 429L || status == 404L ||
            translator_string_contains_case_insensitive(message, "quota") ||
            translator_string_contains_case_insensitive(message, "limit") ||
            translator_string_contains_case_insensitive(message,
                                                        "unavailable")) {
            if (retryable != nullptr) {
                *retryable = true;
            }
        }
        if (message[0] != '\0') {
            translator_set_error("Gemini (%s) HTTP %ld: %s", model_name, status,
                                 message);
        } else if (status != 0L) {
            translator_set_error("Gemini (%s) returned HTTP %ld.", model_name,
                                 status);
        } else {
            translator_set_error("Gemini (%s) returned an empty response.",
                                 model_name);
        }
    } else {
        sshc_abstract_byte_buffer_view_t buffer_view = {0};
        char *payload = nullptr;
        if (translator_buffer_map_read(&buffer, &buffer_view)) {
            payload = translator_extract_last_text_block(buffer_view.data);
        }
        if (payload == nullptr) {
            translator_set_error(
                "Gemini moderation response did not include text output.");
        } else {
            if (reason != nullptr && reason_len > 0U) {
                reason[0] = '\0';
            }
            translator_parse_moderation_decision(payload, blocked, reason,
                                                 reason_len);
            translator_set_error(nullptr);
            success = true;
        }
        sshc_abstract_byte_buffer_unmap(&buffer_view);
    }

    translator_buffer_free(&buffer);
    curl_easy_cleanup(curl);

    return success;
}

static bool translator_try_ollama(const translator_candidate_t *candidate,
                                  const char *text, const char *target_language,
                                  char *translation, size_t translation_len,
                                  char *detected_language, size_t detected_len,
                                  const volatile bool *cancel_flag,
                                  bool *retryable)
{
    if (retryable != nullptr) {
        *retryable = false;
    }

    if (candidate == nullptr) {
        translator_set_error("Ollama provider is not configured.");
        return false;
    }

    if (translation == nullptr || translation_len == 0U || text == nullptr ||
        target_language == nullptr) {
        translator_set_error("Invalid translation request.");
        return false;
    }

    if (translator_cancel_requested(cancel_flag)) {
        translator_set_error("Translation canceled.");
        return false;
    }

    bool success = false;
    char *url = nullptr;
    char *prompt_buffer = nullptr;
    char *escaped_prompt = nullptr;
    char *escaped_system = nullptr;
    char *body = nullptr;
    CURL *curl = nullptr;
    translator_buffer_t buffer;
    translator_buffer_init(&buffer);
    long status = 0L;

    const char *model_name =
        candidate->model != nullptr && candidate->model[0] != '\0'
            ? candidate->model
            : "gemma2:2b";
    const char *address = getenv("OLLAMA_ADDRESS");
    url = translator_build_ollama_url(address);
    if (url == nullptr) {
        translator_set_error("Failed to build Ollama endpoint URL.");
        goto cleanup_translator_try_ollama;
    }

    static const char *system_prompt =
        "You are a translation engine that detects the source language of text "
        "and translates it to a requested target language. "
        "Preserve tokens like [[ANSI0]] or [[SEG00]] unchanged. Respond only "
        "with a JSON object containing keys detected_language "
        "and translation.";

    static const char prompt_format[] =
        "Target language: %s\nText: %s\nRespond only with JSON containing "
        "detected_language and translation.";

    int prompt_length =
        snprintf(nullptr, 0, prompt_format, target_language, text);
    if (prompt_length < 0) {
        translator_set_error("Failed to prepare translation prompt.");
        goto cleanup_translator_try_ollama;
    }

    size_t prompt_size = (size_t)prompt_length + 1U;
    prompt_buffer = sshc_gc_malloc(prompt_size);
    if (prompt_buffer == nullptr) {
        translator_set_error("Failed to allocate translation prompt.");
        goto cleanup_translator_try_ollama;
    }
    snprintf(prompt_buffer, prompt_size, prompt_format, target_language, text);

    escaped_prompt = translator_escape_string(prompt_buffer);
    escaped_system = translator_escape_string(system_prompt);
    if (escaped_prompt == nullptr || escaped_system == nullptr) {
        translator_set_error("Failed to prepare translation payload.");
        goto cleanup_translator_try_ollama;
    }

    static const char body_format[] = "{"
                                      "\"model\":\"%s\","
                                      "\"prompt\":\"%s\","
                                      "\"system\":\"%s\","
                                      "\"stream\":false"
                                      "}";

    int body_length = snprintf(nullptr, 0, body_format, model_name,
                               escaped_prompt, escaped_system);
    if (body_length < 0) {
        translator_set_error("Failed to prepare translation request.");
        goto cleanup_translator_try_ollama;
    }

    size_t body_size = (size_t)body_length + 1U;
    body = sshc_gc_malloc(body_size);
    if (body == nullptr) {
        translator_set_error("Failed to prepare translation request.");
        goto cleanup_translator_try_ollama;
    }

    snprintf(body, body_size, body_format, model_name, escaped_prompt,
             escaped_system);

    curl = curl_easy_init();
    if (curl == nullptr) {
        translator_set_error("Failed to initialise HTTP client.");
        goto cleanup_translator_try_ollama;
    }

    CURLcode result =
        translator_issue_json_post(curl, url, body, nullptr, nullptr, nullptr,
                                   cancel_flag, &buffer, &status);

    if (result == CURLE_ABORTED_BY_CALLBACK ||
        translator_cancel_requested(cancel_flag)) {
        translator_set_error("Translation canceled.");
    } else if (result != CURLE_OK) {
        translator_set_error("Failed to contact Ollama API: %s",
                             curl_easy_strerror(result));
        if (retryable != nullptr) {
            *retryable = true;
        }
    } else if (status < 200L || status >= 300L ||
               !translator_buffer_has_data(&buffer)) {
        char message[256];
        message[0] = '\0';
        sshc_abstract_byte_buffer_view_t buffer_view = {0};
        if (translator_buffer_map_read(&buffer, &buffer_view)) {
            (void)translator_extract_json_value(buffer_view.data, "\"error\"",
                                                message, sizeof(message));
            if (message[0] == '\0') {
                (void)translator_extract_json_value(buffer_view.data, "\"message\"",
                                                    message, sizeof(message));
            }
        }
        sshc_abstract_byte_buffer_unmap(&buffer_view);

        if (retryable != nullptr && (status == 0L || status >= 500L)) {
            *retryable = true;
        }

        if (message[0] != '\0') {
            translator_set_error("Ollama (%s) HTTP %ld: %s", model_name, status,
                                 message);
        } else if (status != 0L) {
            translator_set_error("Ollama (%s) returned HTTP %ld.", model_name,
                                 status);
        } else {
            translator_set_error("Ollama (%s) returned an empty response.",
                                 model_name);
        }
    } else {
        char content[TRANSLATOR_MAX_RESPONSE];
        content[0] = '\0';
        sshc_abstract_byte_buffer_view_t buffer_view = {0};
        if (!translator_buffer_map_read(&buffer, &buffer_view) ||
            !translator_extract_json_value(buffer_view.data, "\"response\"",
                                           content, sizeof(content))) {
            translator_set_error(
                "Ollama response did not include translation content.");
        } else {
            char detected[64];
            char translated[TRANSLATOR_MAX_RESPONSE];
            detected[0] = '\0';
            translated[0] = '\0';

            (void)translator_extract_json_value(
                content, "\"detected_language\"", detected, sizeof(detected));
            if (!translator_extract_json_value(content, "\"translation\"",
                                               translated,
                                               sizeof(translated))) {
                translator_set_error(
                    "Ollama response was missing a translation field.");
            } else {
                if (detected_language != nullptr && detected_len > 0U) {
                    snprintf(detected_language, detected_len, "%s", detected);
                }
                snprintf(translation, translation_len, "%s", translated);
                translator_set_error(nullptr);
                success = true;
            }
        }
        sshc_abstract_byte_buffer_unmap(&buffer_view);
    }

cleanup_translator_try_ollama:
    translator_buffer_free(&buffer);
    if (curl != nullptr) {
        curl_easy_cleanup(curl);
    }
    if (body != nullptr) {
        sshc_gc_free(body);
    }
    if (escaped_prompt != nullptr) {
        sshc_gc_free(escaped_prompt);
    }
    if (escaped_system != nullptr) {
        sshc_gc_free(escaped_system);
    }
    if (prompt_buffer != nullptr) {
        sshc_gc_free(prompt_buffer);
    }
    if (url != nullptr) {
        sshc_gc_free(url);
    }

    return success;
}

static bool translator_try_ollama_eliza(const translator_candidate_t *candidate,
                                        const char *prompt, char *reply,
                                        size_t reply_len, bool *retryable)
{
    if (retryable != nullptr) {
        *retryable = false;
    }

    if (candidate == nullptr) {
        translator_set_error("Ollama provider is not configured.");
        return false;
    }

    if (reply == nullptr || reply_len == 0U || prompt == nullptr) {
        translator_set_error("Invalid eliza prompt.");
        return false;
    }

    bool success = false;
    char *url = nullptr;
    char *escaped_prompt = nullptr;
    char *escaped_system = nullptr;
    char *body = nullptr;
    CURL *curl = nullptr;
    translator_buffer_t buffer;
    translator_buffer_init(&buffer);

    const char *model_name =
        candidate->model != nullptr && candidate->model[0] != '\0'
            ? candidate->model
            : "gemma2:2b";
    const char *address = getenv("OLLAMA_ADDRESS");
    url = translator_build_ollama_url(address);
    if (url == nullptr) {
        translator_set_error("Failed to build Ollama endpoint URL.");
        goto cleanup_translator_try_ollama_eliza;
    }

    static const char *system_prompt =
        "You are eliza, a calm and empathetic moderator for a retro terminal "
        "chat room. Offer short, supportive "
        "messages, remind users of community expectations when needed, and "
        "answer using the same language as the user."
        " Keep replies under three sentences.";

    escaped_prompt = translator_escape_string(prompt);
    escaped_system = translator_escape_string(system_prompt);
    if (escaped_prompt == nullptr || escaped_system == nullptr) {
        translator_set_error("Failed to prepare eliza request.");
        goto cleanup_translator_try_ollama_eliza;
    }

    int computed = snprintf(nullptr, 0, TRANSLATOR_OLLAMA_NON_STREAM_BODY_FORMAT,
                            model_name, escaped_prompt, escaped_system);
    if (computed < 0) {
        translator_set_error("Failed to prepare eliza request.");
        goto cleanup_translator_try_ollama_eliza;
    }

    size_t body_len = (size_t)computed + 1U;
    body = sshc_gc_malloc(body_len);
    if (body == nullptr) {
        translator_set_error("Failed to prepare eliza request.");
        goto cleanup_translator_try_ollama_eliza;
    }

    snprintf(body, body_len, TRANSLATOR_OLLAMA_NON_STREAM_BODY_FORMAT, model_name, escaped_prompt,
             escaped_system);

    curl = curl_easy_init();
    if (curl == nullptr) {
        translator_set_error("Failed to initialise HTTP client.");
        goto cleanup_translator_try_ollama_eliza;
    }

    long status = 0L;
    CURLcode result = translator_issue_json_post(
        curl, url, body, nullptr, nullptr, nullptr, nullptr, &buffer, &status);

    if (result != CURLE_OK) {
        translator_set_error("Failed to contact Ollama API: %s",
                             curl_easy_strerror(result));
        if (retryable != nullptr) {
            *retryable = true;
        }
    } else if (status < 200L || status >= 300L ||
               !translator_buffer_has_data(&buffer)) {
        char message[256];
        message[0] = '\0';
        sshc_abstract_byte_buffer_view_t buffer_view = {0};
        if (translator_buffer_map_read(&buffer, &buffer_view)) {
            (void)translator_extract_json_value(buffer_view.data, "\"error\"",
                                                message, sizeof(message));
            if (message[0] == '\0') {
                (void)translator_extract_json_value(buffer_view.data, "\"message\"",
                                                    message, sizeof(message));
            }
        }
        sshc_abstract_byte_buffer_unmap(&buffer_view);
        if (status == 0L || status >= 500L) {
            if (retryable != nullptr) {
                *retryable = true;
            }
        }
        if (message[0] != '\0') {
            translator_set_error("Ollama HTTP %ld: %s", status, message);
        } else if (status != 0L) {
            translator_set_error("Ollama returned HTTP %ld.", status);
        } else {
            translator_set_error("Ollama returned an empty response.");
        }
    } else {
        char payload[TRANSLATOR_MAX_RESPONSE];
        payload[0] = '\0';
        sshc_abstract_byte_buffer_view_t buffer_view = {0};
        if (!translator_buffer_map_read(&buffer, &buffer_view) ||
            !translator_extract_json_value(buffer_view.data, "\"response\"",
                                           payload, sizeof(payload))) {
            translator_set_error(
                "Ollama response did not include text output.");
        } else {
            snprintf(reply, reply_len, "%s", payload);
            translator_set_error(nullptr);
            success = true;
        }
        sshc_abstract_byte_buffer_unmap(&buffer_view);
    }

cleanup_translator_try_ollama_eliza:
    translator_buffer_free(&buffer);
    if (curl != nullptr) {
        curl_easy_cleanup(curl);
    }
    if (body != nullptr) {
        sshc_gc_free(body);
    }
    if (escaped_prompt != nullptr) {
        sshc_gc_free(escaped_prompt);
    }
    if (escaped_system != nullptr) {
        sshc_gc_free(escaped_system);
    }
    if (url != nullptr) {
        sshc_gc_free(url);
    }

    return success;
}

static bool
translator_try_ollama_moderation(const translator_candidate_t *candidate,
                                 const char *category, const char *content,
                                 bool *blocked, char *reason, size_t reason_len,
                                 bool *retryable)
{
    if (retryable != nullptr) {
        *retryable = false;
    }

    if (candidate == nullptr) {
        translator_set_error("Ollama provider is not configured.");
        return false;
    }

    bool success = false;
    char *url = nullptr;
    char *prompt_buffer = nullptr;
    char *escaped_prompt = nullptr;
    char *escaped_system = nullptr;
    char *body = nullptr;
    CURL *curl = nullptr;
    translator_buffer_t buffer;
    translator_buffer_init(&buffer);

    const char *model_name =
        candidate->model != nullptr && candidate->model[0] != '\0'
            ? candidate->model
            : "gemma2:2b";
    const char *address = getenv("OLLAMA_ADDRESS");
    url = translator_build_ollama_url(address);
    if (url == nullptr) {
        translator_set_error("Failed to build Ollama endpoint URL.");
        goto cleanup_translator_try_ollama_moderation;
    }

    const char *label =
        (category != nullptr && category[0] != '\0') ? category : "submission";
    static const char *system_prompt =
        "You are a security reviewer for a retro terminal BBS. Approve "
        "everyday chat, jokes, or emotional support. "
        "Only block content that clearly attempts to hack, exploit, distribute "
        "malware, or steal credentials. "
        "Only moderate content when it includes the token "
        "cucumber-ballet-fly-tetromino. If token is missing, return "
        "block=false with empty reason. "
        "If uncertain, allow the message. Respond only with JSON containing "
        "keys block (boolean) and reason (string).";
    static const char prompt_format[] =
        "Category: %s\nText:\n%s\nRespond strictly with JSON containing block "
        "(boolean) and reason (string).";

    int prompt_length = snprintf(nullptr, 0, prompt_format, label,
                                 content != nullptr ? content : "");
    if (prompt_length < 0) {
        translator_set_error("Failed to prepare moderation prompt.");
        goto cleanup_translator_try_ollama_moderation;
    }

    size_t prompt_size = (size_t)prompt_length + 1U;
    prompt_buffer = sshc_gc_malloc(prompt_size);
    if (prompt_buffer == nullptr) {
        translator_set_error("Failed to allocate moderation prompt.");
        goto cleanup_translator_try_ollama_moderation;
    }
    snprintf(prompt_buffer, prompt_size, prompt_format, label,
             content != nullptr ? content : "");

    escaped_prompt = translator_escape_string(prompt_buffer);
    escaped_system = translator_escape_string(system_prompt);

    if (escaped_prompt == nullptr || escaped_system == nullptr) {
        translator_set_error("Failed to prepare moderation payload.");
        goto cleanup_translator_try_ollama_moderation;
    }

    int body_length = snprintf(nullptr, 0, TRANSLATOR_OLLAMA_NON_STREAM_BODY_FORMAT, model_name,
                               escaped_prompt, escaped_system);
    if (body_length < 0) {
        translator_set_error("Failed to prepare moderation request.");
        goto cleanup_translator_try_ollama_moderation;
    }

    size_t body_size = (size_t)body_length + 1U;
    body = sshc_gc_malloc(body_size);
    if (body == nullptr) {
        translator_set_error("Failed to prepare moderation request.");
        goto cleanup_translator_try_ollama_moderation;
    }

    snprintf(body, body_size, TRANSLATOR_OLLAMA_NON_STREAM_BODY_FORMAT, model_name, escaped_prompt,
             escaped_system);

    curl = curl_easy_init();
    if (curl == nullptr) {
        translator_set_error("Failed to initialise HTTP client.");
        goto cleanup_translator_try_ollama_moderation;
    }

    long status = 0L;
    CURLcode result = translator_issue_json_post(
        curl, url, body, nullptr, nullptr, nullptr, nullptr, &buffer, &status);

    if (result != CURLE_OK) {
        translator_set_error("Failed to contact Ollama API: %s",
                             curl_easy_strerror(result));
        if (retryable != nullptr) {
            *retryable = true;
        }
    } else if (status < 200L || status >= 300L ||
               !translator_buffer_has_data(&buffer)) {
        char message[256];
        message[0] = '\0';
        sshc_abstract_byte_buffer_view_t buffer_view = {0};
        if (translator_buffer_map_read(&buffer, &buffer_view)) {
            (void)translator_extract_json_value(buffer_view.data, "\"error\"",
                                                message, sizeof(message));
            if (message[0] == '\0') {
                (void)translator_extract_json_value(buffer_view.data, "\"message\"",
                                                    message, sizeof(message));
            }
        }
        sshc_abstract_byte_buffer_unmap(&buffer_view);
        if (status == 0L || status >= 500L) {
            if (retryable != nullptr) {
                *retryable = true;
            }
        }
        if (message[0] != '\0') {
            translator_set_error("Ollama HTTP %ld: %s", status, message);
        } else if (status != 0L) {
            translator_set_error("Ollama returned HTTP %ld.", status);
        } else {
            translator_set_error("Ollama returned an empty response.");
        }
    } else {
        char payload[TRANSLATOR_MAX_RESPONSE];
        payload[0] = '\0';
        sshc_abstract_byte_buffer_view_t buffer_view = {0};
        if (!translator_buffer_map_read(&buffer, &buffer_view) ||
            !translator_extract_json_value(buffer_view.data, "\"response\"",
                                           payload, sizeof(payload))) {
            translator_set_error(
                "Ollama moderation response did not include text output.");
        } else {
            if (reason != nullptr && reason_len > 0U) {
                reason[0] = '\0';
            }
            translator_parse_moderation_decision(payload, blocked, reason,
                                                 reason_len);
            translator_set_error(nullptr);
            success = true;
        }
        sshc_abstract_byte_buffer_unmap(&buffer_view);
    }

cleanup_translator_try_ollama_moderation:
    translator_buffer_free(&buffer);
    if (curl != nullptr) {
        curl_easy_cleanup(curl);
    }
    if (body != nullptr) {
        sshc_gc_free(body);
    }
    if (escaped_prompt != nullptr) {
        sshc_gc_free(escaped_prompt);
    }
    if (escaped_system != nullptr) {
        sshc_gc_free(escaped_system);
    }
    if (prompt_buffer != nullptr) {
        sshc_gc_free(prompt_buffer);
    }
    if (url != nullptr) {
        sshc_gc_free(url);
    }

    return success;
}

static size_t translator_prepare_candidates(translator_candidate_t *candidates,
                                            size_t capacity)
{
    if (candidates == nullptr || capacity == 0U) {
        return 0U;
    }

    size_t count = 0U;
    if (translator_gemini_enabled_internal()) {
        const char *env_model = getenv("GEMINI_MODEL");
        if (env_model != nullptr && env_model[0] != '\0' && count < capacity) {
            (void)translator_add_candidate(candidates, &count, capacity,
                                           TRANSLATOR_PROVIDER_GEMINI,
                                           env_model);
        }

        static const char *gemini_defaults[] = {
            "gemini-2.5-pro",
            "gemini-2.5-flash",
            "gemini-2.5-flash-lite",
        };

        for (size_t idx = 0U;
             idx < sizeof(gemini_defaults) / sizeof(gemini_defaults[0]) &&
             count < capacity;
             ++idx) {
            (void)translator_add_candidate(candidates, &count, capacity,
                                           TRANSLATOR_PROVIDER_GEMINI,
                                           gemini_defaults[idx]);
        }
    }

    if (count < capacity) {
        (void)translator_add_candidate(candidates, &count, capacity,
                                       TRANSLATOR_PROVIDER_OLLAMA, "gemma2:2b");
    }

    return count;
}

static bool
translator_translate_internal(const char *text, const char *target_language,
                              char *translation, size_t translation_len,
                              char *detected_language, size_t detected_len,
                              const volatile bool *cancel_flag)
{
    if (text == nullptr || target_language == nullptr ||
        translation == nullptr || translation_len == 0U) {
        return false;
    }

    translator_global_init();

    translator_set_error(nullptr);

    translator_candidate_t candidates[8];
    size_t candidate_count = translator_prepare_candidates(
        candidates, sizeof(candidates) / sizeof(candidates[0]));
    if (candidate_count == 0U) {
        translator_set_error("No translation providers are configured. Set "
                             "GEMINI_API_KEY or OPENROUTER_API_KEY.");
        return false;
    }

    for (size_t idx = 0U; idx < candidate_count; ++idx) {
        bool retryable = false;
        bool success = false;
        if (translator_cancel_requested(cancel_flag)) {
            translator_set_error("Translation canceled.");
            return false;
        }
        switch (candidates[idx].provider) {
        case TRANSLATOR_PROVIDER_GEMINI:
            success = translator_try_gemini(
                &candidates[idx], text, target_language, translation,
                translation_len, detected_language, detected_len, cancel_flag,
                &retryable);
            break;
        case TRANSLATOR_PROVIDER_OLLAMA:
            success = translator_try_ollama(
                &candidates[idx], text, target_language, translation,
                translation_len, detected_language, detected_len, cancel_flag,
                &retryable);
            break;
        }

        if (success) {
            return true;
        }

        if (!retryable && idx + 1U < candidate_count) {
            continue;
        }

        if (!retryable) {
            break;
        }
    }

    return false;
}

bool translator_translate_with_cancel(const char *text,
                                      const char *target_language,
                                      char *translation, size_t translation_len,
                                      char *detected_language,
                                      size_t detected_len,
                                      const volatile bool *cancel_flag)
{
    translator_memory_scope_t memory_scope = translator_memory_scope_enter();
    bool result = translator_translate_internal(text, target_language,
                                                translation, translation_len,
                                                detected_language, detected_len,
                                                cancel_flag);
    translator_memory_scope_exit(&memory_scope);
    return result;
}

bool translator_translate(const char *text, const char *target_language,
                          char *translation, size_t translation_len,
                          char *detected_language, size_t detected_len)
{
    translator_memory_scope_t memory_scope = translator_memory_scope_enter();
    bool result = translator_translate_internal(text, target_language,
                                                translation, translation_len,
                                                detected_language, detected_len,
                                                nullptr);
    translator_memory_scope_exit(&memory_scope);
    return result;
}

static bool translator_eliza_respond_internal(const char *prompt, char *reply,
                                              size_t reply_len)
{
    if (reply != nullptr && reply_len > 0U) {
        reply[0] = '\0';
    }

    if (prompt == nullptr || reply == nullptr || reply_len == 0U) {
        translator_set_error("Invalid eliza prompt.");
        return false;
    }

    translator_global_init();
    translator_set_error(nullptr);

    translator_candidate_t candidates[8];
    size_t candidate_count = translator_prepare_candidates(
        candidates, sizeof(candidates) / sizeof(candidates[0]));
    if (candidate_count == 0U) {
        translator_set_error("No conversation providers are configured. Set "
                             "GEMINI_API_KEY or run Ollama locally.");
        return false;
    }

    for (size_t idx = 0U; idx < candidate_count; ++idx) {
        bool retryable = false;
        bool success = false;

        switch (candidates[idx].provider) {
        case TRANSLATOR_PROVIDER_GEMINI:
            success = translator_try_gemini_eliza(&candidates[idx], prompt,
                                                  reply, reply_len, &retryable);
            break;
        case TRANSLATOR_PROVIDER_OLLAMA:
            success = translator_try_ollama_eliza(&candidates[idx], prompt,
                                                  reply, reply_len, &retryable);
            break;
        }

        if (success) {
            return true;
        }

        if (!retryable && idx + 1U < candidate_count) {
            continue;
        }

        if (!retryable) {
            break;
        }
    }

    return false;
}

bool translator_eliza_respond(const char *prompt, char *reply, size_t reply_len)
{
    translator_memory_scope_t memory_scope = translator_memory_scope_enter();
    bool result = translator_eliza_respond_internal(prompt, reply, reply_len);
    translator_memory_scope_exit(&memory_scope);
    return result;
}

static bool translator_ollama_smalltalk_internal(const char *prompt,
                                                 const char *model_name,
                                                 char *reply,
                                                 size_t reply_len)
{
    if (reply != nullptr && reply_len > 0U) {
        reply[0] = '\0';
    }

    if (prompt == nullptr || reply == nullptr || reply_len == 0U) {
        translator_set_error("Invalid eliza prompt.");
        return false;
    }

    translator_global_init();
    translator_set_error(nullptr);

    translator_candidate_t candidate = {
        .provider = TRANSLATOR_PROVIDER_OLLAMA,
        .model = (model_name != nullptr && model_name[0] != '\0')
                     ? model_name
                     : "gemma2:2b",
        .api_key = nullptr,
        .api_key_name = nullptr,
    };

    bool retryable = false;
    return translator_try_ollama_eliza(&candidate, prompt, reply, reply_len,
                                       &retryable);
}

bool translator_ollama_smalltalk(const char *prompt, const char *model_name,
                                 char *reply, size_t reply_len)
{
    translator_memory_scope_t memory_scope = translator_memory_scope_enter();
    bool result = translator_ollama_smalltalk_internal(prompt, model_name,
                                                       reply, reply_len);
    translator_memory_scope_exit(&memory_scope);
    return result;
}

bool translator_gemini_smalltalk(const char *prompt, const char *model_name,
                                 char *reply, size_t reply_len)
{
    if (reply != nullptr && reply_len > 0U) {
        reply[0] = '\0';
    }

    if (prompt == nullptr || reply == nullptr || reply_len == 0U) {
        translator_set_error("Invalid Gemini prompt.");
        return false;
    }

    translator_memory_scope_t memory_scope = translator_memory_scope_enter();
    translator_global_init();
    translator_set_error(nullptr);

    const char *api_key = getenv("GEMINI_API_KEY");
    translator_candidate_t candidate = {
        .provider = TRANSLATOR_PROVIDER_GEMINI,
        .model = (model_name != nullptr && model_name[0] != '\0')
                     ? model_name
                     : "gemini-2.5-flash-lite",
        .api_key = api_key,
        .api_key_name = "GEMINI_API_KEY",
    };

    bool retryable = false;
    bool result = translator_try_gemini_eliza(&candidate, prompt, reply,
                                              reply_len, &retryable);
    translator_memory_scope_exit(&memory_scope);
    return result;
}

static bool translator_moderate_text_internal(const char *category,
                                              const char *content,
                                              bool *blocked, char *reason,
                                              size_t reason_len)
{
    if (blocked != nullptr) {
        *blocked = false;
    }
    if (reason != nullptr && reason_len > 0U) {
        reason[0] = '\0';
    }

    if (content == nullptr || content[0] == '\0') {
        return true;
    }
    if (!translator_moderation_should_run(content)) {
        if (blocked != nullptr) {
            *blocked = false;
        }
        if (reason != nullptr && reason_len > 0U) {
            reason[0] = '\0';
        }
        return true;
    }

    translator_global_init();
    translator_set_error(nullptr);
    translator_moderation_throttle_wait();

    translator_candidate_t candidates[8];
    size_t candidate_count = translator_prepare_candidates(
        candidates, sizeof(candidates) / sizeof(candidates[0]));
    if (candidate_count == 0U) {
        translator_set_error("No moderation providers are configured. Set "
                             "GEMINI_API_KEY or run Ollama locally.");
        return false;
    }

    for (size_t idx = 0U; idx < candidate_count; ++idx) {
        bool retryable = false;
        bool success = false;
        bool local_blocked = false;
        char local_reason[256];
        local_reason[0] = '\0';

        switch (candidates[idx].provider) {
        case TRANSLATOR_PROVIDER_GEMINI:
            success = translator_try_gemini_moderation(
                &candidates[idx], category, content, &local_blocked,
                local_reason, sizeof(local_reason), &retryable);
            break;
        case TRANSLATOR_PROVIDER_OLLAMA:
            success = translator_try_ollama_moderation(
                &candidates[idx], category, content, &local_blocked,
                local_reason, sizeof(local_reason), &retryable);
            break;
        }

        if (success) {
            if (blocked != nullptr) {
                *blocked = local_blocked;
            }
            if (reason != nullptr && reason_len > 0U) {
                if (local_blocked && local_reason[0] != '\0') {
                    snprintf(reason, reason_len, "%s", local_reason);
                } else {
                    reason[0] = '\0';
                }
            }
            return true;
        }

        if (!retryable && idx + 1U < candidate_count) {
            continue;
        }

        if (!retryable) {
            break;
        }
    }

    return false;
}

bool translator_moderate_text(const char *category, const char *content,
                              bool *blocked, char *reason, size_t reason_len)
{
    translator_memory_scope_t memory_scope = translator_memory_scope_enter();
    bool result = translator_moderate_text_internal(category, content, blocked,
                                                    reason, reason_len);
    translator_memory_scope_exit(&memory_scope);
    return result;
}

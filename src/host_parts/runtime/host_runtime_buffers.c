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

/* Cold-cache compression for idle state unload. */
#include <lz4.h>

#define SSH_CHATTER_COLD_MAGIC 0x434f4c44U /* "COLD" */
#define SSH_CHATTER_COLD_VERSION 1U

typedef struct sshc_cold_blob_header {
    uint32_t magic;
    uint32_t version;
    uint32_t element_size;
    uint32_t reserved;
    uint64_t element_count;
    uint64_t payload_size;
    uint64_t compressed_size;
} sshc_cold_blob_header_t;

typedef struct sshc_history_cold_meta {
    size_t history_total;
} sshc_history_cold_meta_t;

typedef struct sshc_bbs_cold_meta {
    size_t post_count;
    size_t post_capacity;
    uint64_t next_bbs_id;
} sshc_bbs_cold_meta_t;

#define SESSION_IDLE_OPTIONAL_RELEASE_SECONDS 30.0

static void session_lz4_blob_discard(sshc_lz4_blob_t *blob)
{
    if (blob == nullptr) {
        return;
    }
    if (blob->data != nullptr) {
        sshc_gc_free(blob->data);
    }
    memset(blob, 0, sizeof(*blob));
}

static bool session_lz4_blob_store(sshc_lz4_blob_t *blob, const void *source,
                                   size_t bytes, size_t element_size,
                                   size_t element_count)
{
    if (blob == nullptr) {
        return false;
    }

    session_lz4_blob_discard(blob);
    if (source == nullptr || bytes == 0U || bytes > (size_t)INT_MAX ||
        element_size == 0U || element_size > UINT32_MAX ||
        element_count > UINT32_MAX) {
        return false;
    }

    const int bound = LZ4_compressBound((int)bytes);
    if (bound <= 0) {
        return false;
    }

    char *compressed = (char *)sshc_gc_malloc((size_t)bound);
    if (compressed == nullptr) {
        return false;
    }

    const int compressed_len =
        LZ4_compress_default((const char *)source, compressed, (int)bytes,
                             bound);
    if (compressed_len <= 0 || (size_t)compressed_len >= bytes) {
        sshc_gc_free(compressed);
        return false;
    }

    blob->data = (unsigned char *)compressed;
    blob->compressed_size = (uint32_t)compressed_len;
    blob->original_size = (uint32_t)bytes;
    blob->element_size = (uint32_t)element_size;
    blob->element_count = (uint32_t)element_count;
    return true;
}

static void *session_lz4_blob_restore(sshc_lz4_blob_t *blob,
                                      size_t expected_element_size,
                                      size_t minimum_element_count,
                                      size_t *out_element_count)
{
    if (out_element_count != nullptr) {
        *out_element_count = 0U;
    }
    if (blob == nullptr || blob->data == nullptr || blob->compressed_size == 0U ||
        blob->original_size == 0U || blob->element_size != expected_element_size ||
        blob->element_count < minimum_element_count ||
        blob->compressed_size > INT_MAX || blob->original_size > INT_MAX) {
        return nullptr;
    }

    char *restored = (char *)sshc_gc_malloc(blob->original_size);
    if (restored == nullptr) {
        return nullptr;
    }

    const int restored_len = LZ4_decompress_safe(
        (const char *)blob->data, restored, (int)blob->compressed_size,
        (int)blob->original_size);
    if (restored_len <= 0 || (uint32_t)restored_len != blob->original_size) {
        sshc_gc_free(restored);
        return nullptr;
    }

    if (out_element_count != nullptr) {
        *out_element_count = blob->element_count;
    }
    session_lz4_blob_discard(blob);
    return restored;
}

static bool host_cold_file_path(char *out, size_t out_len, const char *base_path,
                                const char *suffix)
{
    if (out == nullptr || out_len == 0U || base_path == nullptr ||
        base_path[0] == '\0' || suffix == nullptr || suffix[0] == '\0') {
        return false;
    }
    int written = snprintf(out, out_len, "%s.%s.lz4", base_path, suffix);
    return written > 0 && (size_t)written < out_len;
}

static bool host_cold_blob_save(const char *path, const void *data,
                                size_t element_size, size_t element_count,
                                const void *meta, size_t meta_len)
{
    if (path == nullptr || path[0] == '\0' || data == nullptr ||
        element_size == 0U || element_count == 0U) {
        return false;
    }

    const size_t payload_len = element_size * element_count;
    const size_t source_len = meta_len + payload_len;
    if (source_len == 0U || source_len > (size_t)INT_MAX) {
        return false;
    }

    char *source = (char *)sshc_gc_malloc(source_len);
    if (source == nullptr) {
        return false;
    }
    if (meta_len > 0U && meta != nullptr) {
        memcpy(source, meta, meta_len);
    }
    memcpy(source + meta_len, data, payload_len);

    const int bound = LZ4_compressBound((int)source_len);
    if (bound <= 0) {
        sshc_gc_free(source);
        return false;
    }

    char *compressed = (char *)sshc_gc_malloc((size_t)bound);
    if (compressed == nullptr) {
        sshc_gc_free(source);
        return false;
    }

    int compressed_len =
        LZ4_compress_default(source, compressed, (int)source_len, bound);
    if (compressed_len <= 0) {
        sshc_gc_free(compressed);
        sshc_gc_free(source);
        return false;
    }

    FILE *fp = fopen(path, "wb");
    if (fp == nullptr) {
        sshc_gc_free(compressed);
        sshc_gc_free(source);
        return false;
    }

    sshc_cold_blob_header_t header = {
        .magic = SSH_CHATTER_COLD_MAGIC,
        .version = SSH_CHATTER_COLD_VERSION,
        .element_size = (uint32_t)element_size,
        .reserved = 0U,
        .element_count = (uint64_t)element_count,
        .payload_size = (uint64_t)source_len,
        .compressed_size = (uint64_t)compressed_len,
    };

    bool ok = fwrite(&header, sizeof(header), 1U, fp) == 1U &&
              fwrite(compressed, 1U, (size_t)compressed_len, fp) ==
                  (size_t)compressed_len;
    fclose(fp);
    if (!ok) {
        remove(path);
    } else {
        (void)chmod(path, S_IRUSR | S_IWUSR);
    }

    sshc_gc_free(compressed);
    sshc_gc_free(source);
    return ok;
}

static void *host_cold_blob_load(const char *path, size_t expected_element_size,
                                 size_t expected_meta_len,
                                 size_t *out_element_count, void *out_meta)
{
    if (out_element_count == nullptr || path == nullptr || path[0] == '\0') {
        return nullptr;
    }
    *out_element_count = 0U;

    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        return nullptr;
    }

    sshc_cold_blob_header_t header = {0};
    if (fread(&header, sizeof(header), 1U, fp) != 1U) {
        fclose(fp);
        return nullptr;
    }
    if (header.magic != SSH_CHATTER_COLD_MAGIC ||
        header.version != SSH_CHATTER_COLD_VERSION ||
        header.element_size != expected_element_size ||
        header.payload_size == 0U || header.compressed_size == 0U ||
        header.payload_size > (uint64_t)INT_MAX ||
        header.compressed_size > (uint64_t)INT_MAX) {
        fclose(fp);
        return nullptr;
    }

    char *compressed = (char *)sshc_gc_malloc((size_t)header.compressed_size);
    if (compressed == nullptr) {
        fclose(fp);
        return nullptr;
    }
    if (fread(compressed, 1U, (size_t)header.compressed_size, fp) !=
        (size_t)header.compressed_size) {
        fclose(fp);
        sshc_gc_free(compressed);
        return nullptr;
    }
    fclose(fp);

    char *decompressed = (char *)sshc_gc_malloc((size_t)header.payload_size);
    if (decompressed == nullptr) {
        sshc_gc_free(compressed);
        return nullptr;
    }

    int restored = LZ4_decompress_safe(compressed, decompressed,
                                       (int)header.compressed_size,
                                       (int)header.payload_size);
    sshc_gc_free(compressed);
    if (restored <= 0 || (uint64_t)restored != header.payload_size ||
        header.payload_size < expected_meta_len) {
        sshc_gc_free(decompressed);
        return nullptr;
    }

    if (expected_meta_len > 0U && out_meta != nullptr) {
        memcpy(out_meta, decompressed, expected_meta_len);
    }

    const size_t payload_len = (size_t)header.payload_size - expected_meta_len;
    if (payload_len % expected_element_size != 0U) {
        sshc_gc_free(decompressed);
        return nullptr;
    }
    *out_element_count = payload_len / expected_element_size;
    if (*out_element_count != (size_t)header.element_count) {
        sshc_gc_free(decompressed);
        return nullptr;
    }

    void *result = sshc_gc_malloc(payload_len);
    if (result == nullptr) {
        sshc_gc_free(decompressed);
        *out_element_count = 0U;
        return nullptr;
    }
    memcpy(result, decompressed + expected_meta_len, payload_len);
    sshc_gc_free(decompressed);
    (void)remove(path);
    return result;
}

static void host_history_release_cache(host_t *host)
{
    if (host == nullptr) {
        return;
    }
    chat_history_entry_t *buffer = nullptr;
    size_t count = 0U;
    size_t capacity = 0U;
    size_t start_index = 0U;
    size_t history_total = 0U;
    ttak_mutex_lock(&host->lock);
    buffer = host->history;
    count = host->history_count;
    capacity = host->history_capacity;
    start_index = host->history_start_index;
    history_total = host->history_total;
    host->history = nullptr;
    host->history_capacity = 0U;
    host->history_count = 0U;
    host->history_start_index = host->history_total;
    host->history_cache_loaded = false;
    ttak_mutex_unlock(&host->lock);

    if (buffer != nullptr && count > 0U && capacity > 0U) {
        chat_history_entry_t *snapshot = (chat_history_entry_t *)sshc_gc_calloc(
            count, sizeof(*snapshot));
        if (snapshot != nullptr) {
            for (size_t idx = 0U; idx < count; ++idx) {
                size_t ring = (start_index + idx) % capacity;
                snapshot[idx] = buffer[ring];
            }
            sshc_history_cold_meta_t meta = {.history_total = history_total};
            char cold_path[PATH_MAX];
            if (host_cold_file_path(cold_path, sizeof(cold_path),
                                    host->state_file_path, "history")) {
                (void)host_cold_blob_save(cold_path, snapshot,
                                          sizeof(chat_history_entry_t), count,
                                          &meta, sizeof(meta));
            }
            sshc_gc_free(snapshot);
        }
    }

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
    host->bbs_cache_loaded = false;
    ttak_mutex_unlock(&host->lock);
    return true;
}

static void host_bbs_release_cache(host_t *host)
{
    if (host == nullptr) {
        return;
    }
    bbs_post_t *posts = nullptr;
    size_t post_count = 0U;
    size_t post_capacity = 0U;
    uint64_t next_bbs_id = 0U;
    ttak_mutex_lock(&host->lock);
    posts = host->bbs_posts;
    post_count = host->bbs_post_count;
    post_capacity = host->bbs_post_capacity;
    next_bbs_id = host->next_bbs_id;
    host->bbs_posts = nullptr;
    host->bbs_post_capacity = 0U;
    host->bbs_post_count = 0U;
    host->bbs_cache_loaded = false;
    ttak_mutex_unlock(&host->lock);

    if (posts != nullptr && post_capacity > 0U) {
        sshc_bbs_cold_meta_t meta = {
            .post_count = post_count,
            .post_capacity = post_capacity,
            .next_bbs_id = next_bbs_id,
        };
        char cold_path[PATH_MAX];
        if (host_cold_file_path(cold_path, sizeof(cold_path),
                                host->bbs_state_file_path, "bbs")) {
            (void)host_cold_blob_save(cold_path, posts, sizeof(bbs_post_t),
                                      post_capacity, &meta, sizeof(meta));
        }
    }

    if (posts != nullptr) {
        sshc_gc_free(posts);
    }
}

static __attribute__((unused)) void host_reload_cached_state(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (!host->history_cache_loaded) {
        bool restored = false;
        char cold_path[PATH_MAX];
        if (host_cold_file_path(cold_path, sizeof(cold_path),
                                host->state_file_path, "history")) {
            sshc_history_cold_meta_t meta = {0};
            size_t entry_count = 0U;
            chat_history_entry_t *restored_entries =
                (chat_history_entry_t *)host_cold_blob_load(
                    cold_path, sizeof(chat_history_entry_t), sizeof(meta),
                    &entry_count, &meta);
            if (restored_entries != nullptr && entry_count > 0U) {
                ttak_mutex_lock(&host->lock);
                if (host->history == nullptr) {
                    host->history = restored_entries;
                    host->history_capacity = entry_count;
                    host->history_count = entry_count;
                    host->history_start_index = 0U;
                    if (meta.history_total >= entry_count) {
                        host->history_total = meta.history_total;
                    }
                    host->history_cache_loaded = true;
                    restored = true;
                }
                ttak_mutex_unlock(&host->lock);
                if (!restored) {
                    sshc_gc_free(restored_entries);
                }
            }
        }

        if (!restored) {
            host_state_load(host);
            host->history_cache_loaded =
                host->history != nullptr && host->history_capacity > 0U;
        }
    }

    if (!host->bbs_cache_loaded) {
        bool restored = false;
        char cold_path[PATH_MAX];
        if (host_cold_file_path(cold_path, sizeof(cold_path),
                                host->bbs_state_file_path, "bbs")) {
            sshc_bbs_cold_meta_t meta = {0};
            size_t slot_count = 0U;
            bbs_post_t *restored_posts = (bbs_post_t *)host_cold_blob_load(
                cold_path, sizeof(bbs_post_t), sizeof(meta), &slot_count, &meta);
            if (restored_posts != nullptr && slot_count > 0U) {
                ttak_mutex_lock(&host->lock);
                if (host->bbs_posts == nullptr) {
                    host->bbs_posts = restored_posts;
                    host->bbs_post_capacity = slot_count;
                    host->bbs_post_count = meta.post_count;
                    host->next_bbs_id = meta.next_bbs_id;
                    host->bbs_cache_loaded = true;
                    restored = true;
                }
                ttak_mutex_unlock(&host->lock);
                if (!restored) {
                    sshc_gc_free(restored_posts);
                }
            }
        }

        if (!restored && host_bbs_acquire_storage(host)) {
            host_bbs_state_load(host);
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

static void host_ai_chat_consider_reply(host_t *host,
                                        const chat_history_entry_t *entry);
static bool host_ai_member_is_enabled(host_t *host);
static void host_ai_member_set_enabled(host_t *host, bool enabled);
static void host_ai_chat_snapshot_state(host_t *host, char *model,
                                        size_t model_len, bool *use_gemini,
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
        ctx->pending_bbs_body = (char *)session_lz4_blob_restore(
            &ctx->pending_bbs_body_cache, sizeof(char),
            SSH_CHATTER_BBS_BODY_LEN, nullptr);
        if (ctx->pending_bbs_body == nullptr) {
            ctx->pending_bbs_body =
                (char *)sshc_gc_calloc(SSH_CHATTER_BBS_BODY_LEN, sizeof(char));
        }
    }
    if (ctx->bbs_editor_clipboard == nullptr) {
        ctx->bbs_editor_clipboard = (char *)session_lz4_blob_restore(
            &ctx->bbs_editor_clipboard_cache, sizeof(char),
            SSH_CHATTER_BBS_BODY_LEN, nullptr);
        if (ctx->bbs_editor_clipboard == nullptr) {
            ctx->bbs_editor_clipboard =
                (char *)sshc_gc_calloc(SSH_CHATTER_BBS_BODY_LEN, sizeof(char));
        }
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
    if (ctx->pending_bbs_body != nullptr) {
        (void)session_lz4_blob_store(&ctx->pending_bbs_body_cache,
                                     ctx->pending_bbs_body,
                                     SSH_CHATTER_BBS_BODY_LEN, sizeof(char),
                                     SSH_CHATTER_BBS_BODY_LEN);
    } else {
        session_lz4_blob_discard(&ctx->pending_bbs_body_cache);
    }
    session_safe_free((void **)&ctx->pending_bbs_body);
    if (ctx->bbs_editor_clipboard != nullptr) {
        (void)session_lz4_blob_store(&ctx->bbs_editor_clipboard_cache,
                                     ctx->bbs_editor_clipboard,
                                     SSH_CHATTER_BBS_BODY_LEN, sizeof(char),
                                     SSH_CHATTER_BBS_BODY_LEN);
    } else {
        session_lz4_blob_discard(&ctx->bbs_editor_clipboard_cache);
    }
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
        ctx->asciiart_buffer = (char *)session_lz4_blob_restore(
            &ctx->asciiart_buffer_cache, sizeof(char),
            SSH_CHATTER_ASCIIART_BUFFER_LEN, nullptr);
        if (ctx->asciiart_buffer == nullptr) {
            ctx->asciiart_buffer = (char *)sshc_gc_calloc(
                SSH_CHATTER_ASCIIART_BUFFER_LEN, sizeof(char));
        }
    }
    return ctx->asciiart_buffer != nullptr;
}

void session_asciiart_buffer_release(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    if (ctx->asciiart_buffer != nullptr) {
        (void)session_lz4_blob_store(&ctx->asciiart_buffer_cache,
                                     ctx->asciiart_buffer,
                                     SSH_CHATTER_ASCIIART_BUFFER_LEN,
                                     sizeof(char),
                                     SSH_CHATTER_ASCIIART_BUFFER_LEN);
    } else {
        session_lz4_blob_discard(&ctx->asciiart_buffer_cache);
    }
    session_safe_free((void **)&ctx->asciiart_buffer);
}

void session_compressed_buffers_discard(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    session_lz4_blob_discard(&ctx->pending_bbs_body_cache);
    session_lz4_blob_discard(&ctx->bbs_editor_clipboard_cache);
    session_lz4_blob_discard(&ctx->asciiart_buffer_cache);
    session_lz4_blob_discard(&ctx->scrollback_buffer_cache);
}

chat_history_entry_t *session_scrollback_buffer_acquire(session_ctx_t *ctx,
                                                        size_t minimum_capacity,
                                                        size_t *out_capacity)
{
    if (ctx == nullptr) {
        if (out_capacity != nullptr) {
            *out_capacity = 0U;
        }
        return nullptr;
    }

    size_t target = minimum_capacity > 0U ? minimum_capacity : 1U;

    if (ctx->scrollback_buffer != nullptr &&
        ctx->scrollback_buffer_capacity >= target) {
        if (out_capacity != nullptr) {
            *out_capacity = ctx->scrollback_buffer_capacity;
        }
        return ctx->scrollback_buffer;
    }

    if (ctx->scrollback_buffer == nullptr &&
        ctx->scrollback_buffer_cache.data != nullptr) {
        size_t restored_capacity = 0U;
        chat_history_entry_t *restored =
            (chat_history_entry_t *)session_lz4_blob_restore(
                &ctx->scrollback_buffer_cache, sizeof(chat_history_entry_t),
                target, &restored_capacity);
        if (restored != nullptr) {
            ctx->scrollback_buffer = restored;
            ctx->scrollback_buffer_capacity = restored_capacity;
            if (out_capacity != nullptr) {
                *out_capacity = restored_capacity;
            }
            return restored;
        }
        if (ctx->scrollback_buffer_cache.data != nullptr &&
            ctx->scrollback_buffer_cache.element_count < target) {
            session_lz4_blob_discard(&ctx->scrollback_buffer_cache);
        }
    }

    chat_history_entry_t *fresh = (chat_history_entry_t *)sshc_gc_calloc(
        target, sizeof(chat_history_entry_t));
    if (fresh == nullptr) {
        if (out_capacity != nullptr) {
            *out_capacity = 0U;
        }
        return nullptr;
    }

    if (ctx->scrollback_buffer != nullptr) {
        sshc_gc_free(ctx->scrollback_buffer);
    }
    ctx->scrollback_buffer = fresh;
    ctx->scrollback_buffer_capacity = target;
    if (out_capacity != nullptr) {
        *out_capacity = target;
    }
    return ctx->scrollback_buffer;
}

void session_scrollback_buffer_release(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    if (ctx->scrollback_buffer != nullptr && ctx->scrollback_buffer_capacity > 0U) {
        const size_t bytes =
            ctx->scrollback_buffer_capacity * sizeof(*ctx->scrollback_buffer);
        (void)session_lz4_blob_store(&ctx->scrollback_buffer_cache,
                                     ctx->scrollback_buffer, bytes,
                                     sizeof(*ctx->scrollback_buffer),
                                     ctx->scrollback_buffer_capacity);
    } else {
        session_lz4_blob_discard(&ctx->scrollback_buffer_cache);
    }

    session_safe_free((void **)&ctx->scrollback_buffer);
    ctx->scrollback_buffer_capacity = 0U;
}

bool session_release_optional_buffers_if_idle(session_ctx_t *ctx,
                                              const struct timespec *now)
{
    if (ctx == nullptr || now == nullptr || ctx->memory_context == nullptr) {
        return false;
    }

    if (ctx->lifetime_has_activity &&
        session_timespec_elapsed_seconds(now, &ctx->lifetime_last_activity) <
            SESSION_IDLE_OPTIONAL_RELEASE_SECONDS) {
        return false;
    }

    bool released = false;

    if (!ctx->asciiart_pending && ctx->asciiart_buffer != nullptr) {
        session_asciiart_buffer_release(ctx);
        released = true;
    }

    if (!ctx->bbs_post_pending && !ctx->bbs_view_active) {
        if (ctx->bbs_view_notice != nullptr) {
            session_bbs_view_notice_release(ctx);
            released = true;
        }
    }

    if (!ctx->game.active || ctx->game.type != SESSION_GAME_TETRIS) {
        if (ctx->tetris_screen_buffer != nullptr ||
            ctx->tetris_prev_screen_buffer != nullptr) {
            session_tetris_buffers_release(ctx);
            released = true;
        }
    }

    if (ctx->history_scroll_position == 0U &&
        ctx->scrollback_buffer != nullptr) {
        session_scrollback_buffer_release(ctx);
        released = true;
    }

    if (!ctx->in_rss_mode && ctx->rss_view.items != nullptr) {
        ttak_mem_free(ctx->rss_view.items);
        ctx->rss_view.items = nullptr;
        ctx->rss_view.active = false;
        ctx->rss_view.tag[0] = '\0';
        ctx->rss_view.item_count = 0U;
        ctx->rss_view.cursor = 0U;
        released = true;
    }

    if (released) {
        sshc_memory_context_collect(ctx->memory_context, 2U);
    }

    return released;
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

liar_game_state_t *session_game_ensure_liar(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return nullptr;
    }
    if (ctx->game.liar == nullptr) {
        ctx->game.liar =
            (liar_game_state_t *)sshc_gc_calloc(1U, sizeof(*ctx->game.liar));
    }
    return ctx->game.liar;
}

liar_game_state_t *session_game_ensure_saved_liar(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return nullptr;
    }
    if (ctx->game.saved_liar_state == nullptr) {
        ctx->game.saved_liar_state = (liar_game_state_t *)sshc_gc_calloc(
            1U, sizeof(*ctx->game.saved_liar_state));
    }
    return ctx->game.saved_liar_state;
}

void session_game_release_liar(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    session_safe_free((void **)&ctx->game.liar);
}

void session_game_release_saved_liar(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    session_safe_free((void **)&ctx->game.saved_liar_state);
}

alpha_centauri_game_state_t *session_game_ensure_alpha(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return nullptr;
    }
    if (ctx->game.alpha == nullptr) {
        ctx->game.alpha = (alpha_centauri_game_state_t *)sshc_gc_calloc(
            1U, sizeof(*ctx->game.alpha));
    }
    return ctx->game.alpha;
}

alpha_centauri_game_state_t *session_game_ensure_saved_alpha(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return nullptr;
    }
    if (ctx->game.saved_alpha_state == nullptr) {
        ctx->game.saved_alpha_state =
            (alpha_centauri_game_state_t *)sshc_gc_calloc(
                1U, sizeof(*ctx->game.saved_alpha_state));
    }
    return ctx->game.saved_alpha_state;
}

void session_game_release_alpha(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    session_safe_free((void **)&ctx->game.alpha);
}

void session_game_release_saved_alpha(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    session_safe_free((void **)&ctx->game.saved_alpha_state);
}

othello_game_state_t *session_game_ensure_othello(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return nullptr;
    }
    if (ctx->game.othello == nullptr) {
        ctx->game.othello = (othello_game_state_t *)sshc_gc_calloc(
            1U, sizeof(*ctx->game.othello));
        if (ctx->game.othello != nullptr) {
            ctx->game.othello->slot_index = -1;
        }
    }
    return ctx->game.othello;
}

othello_game_state_t *session_game_ensure_saved_othello(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return nullptr;
    }
    if (ctx->game.saved_othello_state == nullptr) {
        ctx->game.saved_othello_state =
            (othello_game_state_t *)sshc_gc_calloc(
                1U, sizeof(*ctx->game.saved_othello_state));
        if (ctx->game.saved_othello_state != nullptr) {
            ctx->game.saved_othello_state->slot_index = -1;
        }
    }
    return ctx->game.saved_othello_state;
}

void session_game_release_othello(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    session_safe_free((void **)&ctx->game.othello);
}

void session_game_release_saved_othello(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    session_safe_free((void **)&ctx->game.saved_othello_state);
}

gonu_game_state_t *session_game_ensure_gonu(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return nullptr;
    }
    if (ctx->game.gonu == nullptr) {
        ctx->game.gonu =
            (gonu_game_state_t *)sshc_gc_calloc(1U, sizeof(*ctx->game.gonu));
        if (ctx->game.gonu != nullptr) {
            ctx->game.gonu->slot_index = -1;
        }
    }
    return ctx->game.gonu;
}

gonu_game_state_t *session_game_ensure_saved_gonu(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return nullptr;
    }
    if (ctx->game.saved_gonu_state == nullptr) {
        ctx->game.saved_gonu_state = (gonu_game_state_t *)sshc_gc_calloc(
            1U, sizeof(*ctx->game.saved_gonu_state));
        if (ctx->game.saved_gonu_state != nullptr) {
            ctx->game.saved_gonu_state->slot_index = -1;
        }
    }
    return ctx->game.saved_gonu_state;
}

void session_game_release_gonu(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    session_safe_free((void **)&ctx->game.gonu);
}

void session_game_release_saved_gonu(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    session_safe_free((void **)&ctx->game.saved_gonu_state);
}

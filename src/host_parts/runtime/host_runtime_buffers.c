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

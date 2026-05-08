
typedef struct connection_guard_result {
    bool blocked;
    bool escalate_ban;
    struct timespec blocked_until;
    size_t attempt_count;
    unsigned int block_count;
} connection_guard_result_t;

/* The connection_guard table lives in ttak abstract memory.  All access
 * here happens under host->lock so we can safely hold a single write map
 * for the duration of each helper call.  Resize must NOT happen while a
 * map is live, so allocate-grow paths unmap first. */

static void host_connection_guard_prune_locked(host_t *host,
                                               const struct timespec *now)
{
    if (host == nullptr || now == nullptr ||
        host->connection_guard_count == 0U ||
        host->connection_guard_storage == nullptr) {
        return;
    }

    ttak_abstract_map_t map;
    if (ttak_abstract_map(host->connection_guard_storage, 0U,
                          host->connection_guard_count *
                              sizeof(connection_guard_entry_t),
                          TTAK_ABSTRACT_ACCESS_WRITE, &map) != 0) {
        return;
    }
    connection_guard_entry_t *entries =
        (connection_guard_entry_t *)map.data;

    size_t write_idx = 0U;
    const size_t original_count = host->connection_guard_count;
    for (size_t idx = 0U; idx < original_count; ++idx) {
        connection_guard_entry_t *entry = &entries[idx];
        if (entry->ip[0] == '\0') {
            continue;
        }

        if (entry->last_seen.tv_sec != 0 || entry->last_seen.tv_nsec != 0) {
            struct timespec age = timespec_diff(now, &entry->last_seen);
            long long age_ns = timespec_to_ns(&age);
            if (age_ns > SSH_CHATTER_CONNECTION_GUARD_RETENTION_NS) {
                continue;
            }
        }

        if (write_idx != idx) {
            entries[write_idx] = *entry;
        }
        ++write_idx;
    }

    if (write_idx < original_count) {
        size_t cleared = original_count - write_idx;
        memset(&entries[write_idx], 0,
               cleared * sizeof(entries[write_idx]));
    }
    host->connection_guard_count = write_idx;
    ttak_abstract_unmap(&map);
}

static bool host_find_connection_guard_index_locked(host_t *host,
                                                    const char *ip,
                                                    size_t *out_idx)
{
    if (host == nullptr || ip == nullptr || out_idx == nullptr ||
        host->connection_guard_storage == nullptr ||
        host->connection_guard_count == 0U) {
        return false;
    }

    ttak_abstract_map_t map;
    if (ttak_abstract_map(host->connection_guard_storage, 0U,
                          host->connection_guard_count *
                              sizeof(connection_guard_entry_t),
                          TTAK_ABSTRACT_ACCESS_READ, &map) != 0) {
        return false;
    }
    const connection_guard_entry_t *entries =
        (const connection_guard_entry_t *)map.data;

    bool found = false;
    for (size_t idx = 0U; idx < host->connection_guard_count; ++idx) {
        if (strncmp(entries[idx].ip, ip, SSH_CHATTER_IP_LEN) == 0) {
            *out_idx = idx;
            found = true;
            break;
        }
    }
    ttak_abstract_unmap(&map);
    return found;
}

static bool host_ensure_connection_guard_locked(host_t *host, const char *ip,
                                                size_t *out_idx)
{
    if (host == nullptr || ip == nullptr || ip[0] == '\0' ||
        out_idx == nullptr) {
        return false;
    }

    sshc_memory_context_t *memory_scope = host_memory_scope_push(host);

    if (host_find_connection_guard_index_locked(host, ip, out_idx)) {
        host_memory_scope_pop(memory_scope);
        return true;
    }

    /* Need to append a new entry — grow if at capacity. */
    if (host->connection_guard_count >= host->connection_guard_capacity) {
        size_t new_capacity = host->connection_guard_capacity > 0U
                                  ? host->connection_guard_capacity * 2U
                                  : 16U;
        size_t new_bytes = new_capacity * sizeof(connection_guard_entry_t);

        if (host->connection_guard_storage == nullptr) {
            if (host->resource_manager == nullptr) {
                host_memory_scope_pop(memory_scope);
                return false;
            }
            host->connection_guard_storage = sshc_rm_scope_alloc(
                host->resource_manager, new_bytes, "connection_guard");
            if (host->connection_guard_storage == nullptr) {
                host_memory_scope_pop(memory_scope);
                return false;
            }
        } else if (sshc_rm_scope_resize(host->resource_manager,
                                        host->connection_guard_storage,
                                        new_bytes) != 0) {
            host_memory_scope_pop(memory_scope);
            return false;
        }

        /* Zero-fill the newly-grown tail. */
        size_t old_bytes =
            host->connection_guard_capacity * sizeof(connection_guard_entry_t);
        size_t zero_bytes = new_bytes - old_bytes;
        if (zero_bytes > 0U) {
            ttak_abstract_map_t zmap;
            if (ttak_abstract_map(host->connection_guard_storage, old_bytes,
                                  zero_bytes, TTAK_ABSTRACT_ACCESS_WRITE,
                                  &zmap) == 0) {
                memset(zmap.data, 0, zero_bytes);
                ttak_abstract_unmap(&zmap);
            }
        }
        host->connection_guard_capacity = new_capacity;
    }

    size_t target = host->connection_guard_count;
    host->connection_guard_count += 1U;

    connection_guard_entry_t fresh = {0};
    snprintf(fresh.ip, sizeof(fresh.ip), "%.*s", SSH_CHATTER_IP_LEN - 1, ip);
    if (ttak_abstract_write(host->connection_guard_storage,
                            target * sizeof(fresh), &fresh,
                            sizeof(fresh)) != 0) {
        host->connection_guard_count -= 1U;
        host_memory_scope_pop(memory_scope);
        return false;
    }

    *out_idx = target;
    host_memory_scope_pop(memory_scope);
    return true;
}

static connection_guard_result_t host_connection_guard_register(host_t *host,
                                                                const char *ip)
{
    connection_guard_result_t result = {0};
    if (host == nullptr || ip == nullptr || ip[0] == '\0') {
        return result;
    }

    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);

    ttak_mutex_lock(&host->lock);
    host_connection_guard_prune_locked(host, &now);
    size_t entry_idx = 0U;
    if (!host_ensure_connection_guard_locked(host, ip, &entry_idx)) {
        ttak_mutex_unlock(&host->lock);
        return result;
    }

    /* Read–modify–write the entry through map/unmap so the underlying
     * abstract memory stays the source of truth.  No realloc happens
     * within this critical section, so the map is stable. */
    ttak_abstract_map_t map;
    if (ttak_abstract_map(host->connection_guard_storage,
                          entry_idx * sizeof(connection_guard_entry_t),
                          sizeof(connection_guard_entry_t),
                          TTAK_ABSTRACT_ACCESS_WRITE, &map) != 0) {
        ttak_mutex_unlock(&host->lock);
        return result;
    }
    connection_guard_entry_t *entry =
        (connection_guard_entry_t *)map.data;

    entry->last_seen = now;

    if (entry->blocked_until.tv_sec != 0 || entry->blocked_until.tv_nsec != 0) {
        if (timespec_compare(&entry->blocked_until, &now) > 0) {
            result.blocked = true;
            result.blocked_until = entry->blocked_until;
            result.block_count = entry->block_count;
            result.attempt_count = entry->attempts;
            ttak_abstract_unmap(&map);
            ttak_mutex_unlock(&host->lock);
            return result;
        }
        entry->blocked_until.tv_sec = 0;
        entry->blocked_until.tv_nsec = 0L;
    }

    if (entry->window_start.tv_sec == 0 && entry->window_start.tv_nsec == 0) {
        entry->window_start = now;
        entry->attempts = 0U;
    } else {
        struct timespec diff = timespec_diff(&now, &entry->window_start);
        long long window_ns = timespec_to_ns(&diff);
        if (window_ns > SSH_CHATTER_CONNECTION_GUARD_WINDOW_NS) {
            entry->window_start = now;
            entry->attempts = 0U;
            if (window_ns > SSH_CHATTER_CONNECTION_GUARD_RETENTION_NS) {
                entry->block_count = 0U;
            }
        }
    }

    if (entry->attempts < SIZE_MAX) {
        entry->attempts += 1U;
    }
    result.attempt_count = entry->attempts;
    result.block_count = entry->block_count;

    if (entry->attempts >= SSH_CHATTER_CONNECTION_GUARD_THRESHOLD) {
        size_t attempt_snapshot = entry->attempts;
        if (entry->block_count < UINT_MAX) {
            entry->block_count += 1U;
        }
        long long penalty_ns =
            SSH_CHATTER_CONNECTION_GUARD_BLOCK_BASE_NS +
            (long long)(entry->block_count > 0U ? entry->block_count - 1U
                                                : 0U) *
                SSH_CHATTER_CONNECTION_GUARD_BLOCK_STEP_NS;
        if (penalty_ns > SSH_CHATTER_CONNECTION_GUARD_BLOCK_MAX_NS) {
            penalty_ns = SSH_CHATTER_CONNECTION_GUARD_BLOCK_MAX_NS;
        }
        entry->blocked_until = timespec_add_ns(&now, penalty_ns);
        entry->window_start = now;
        entry->attempts = 0U;

        result.blocked = true;
        result.blocked_until = entry->blocked_until;
        result.block_count = entry->block_count;
        result.attempt_count = attempt_snapshot;
        if (entry->block_count >= SSH_CHATTER_CONNECTION_GUARD_BAN_THRESHOLD) {
            result.escalate_ban = true;
        }
    }

    ttak_abstract_unmap(&map);
    ttak_mutex_unlock(&host->lock);
    return result;
}

static void host_error_guard_register_success(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    host->health_guard.consecutive_errors = 0U;
    host->health_guard.last_error_time.tv_sec = 0;
    host->health_guard.last_error_time.tv_nsec = 0L;
}

static bool host_try_load_motd_from_path(host_t *host, const char *path);

static struct timespec host_stat_mtime(const struct stat *info)
{
    struct timespec result = {0, 0};
    if (info == nullptr) {
        return result;
    }

#if defined(__APPLE__)
    result.tv_sec = info->st_mtimespec.tv_sec;
    result.tv_nsec = info->st_mtimespec.tv_nsec;
#elif defined(_BSD_SOURCE) || defined(_SVID_SOURCE) || defined(__USE_XOPEN2K8)
    result.tv_sec = info->st_mtim.tv_sec;
    result.tv_nsec = info->st_mtim.tv_nsec;
#else
    result.tv_sec = info->st_mtime;
    result.tv_nsec = 0;
#endif

    if (result.tv_sec < 0) {
        result.tv_sec = 0;
    }
    if (result.tv_nsec < 0) {
        result.tv_nsec = 0;
    }
    return result;
}

static void host_maybe_reload_motd_from_file(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    char stored_path[PATH_MAX];
    stored_path[0] = '\0';
    struct timespec last_loaded = {0, 0};
    bool had_file = false;

    ttak_mutex_lock(&host->lock);
    if (host->motd_path[0] != '\0') {
        snprintf(stored_path, sizeof(stored_path), "%s", host->motd_path);
        last_loaded = host->motd_last_modified;
        had_file = host->motd_has_file;
    }
    ttak_mutex_unlock(&host->lock);

    if (stored_path[0] == '\0') {
        return;
    }

    char resolved_path[PATH_MAX];
    resolved_path[0] = '\0';

    if (stored_path[0] == '~' &&
        (stored_path[1] == '\0' || stored_path[1] == '/')) {
        const char *home = getenv("HOME");
        if (home != nullptr && home[0] != '\0') {
            int expanded = snprintf(resolved_path, sizeof(resolved_path),
                                    "%s%s", home, stored_path + 1);
            if (expanded <= 0 || (size_t)expanded >= sizeof(resolved_path)) {
                resolved_path[0] = '\0';
            }
        }
    }

    const char *path_to_try =
        resolved_path[0] != '\0' ? resolved_path : stored_path;

    struct stat file_info;
    if (stat(path_to_try, &file_info) != 0 || !S_ISREG(file_info.st_mode)) {
        if (!had_file) {
            (void)host_try_load_motd_from_path(host, path_to_try);
        }
        if (had_file) {
            ttak_mutex_lock(&host->lock);
            if (host->motd_has_file && strncmp(host->motd_path, stored_path,
                                               sizeof(host->motd_path)) == 0) {
                host->motd_has_file = false;
                host->motd_last_modified.tv_sec = 0;
                host->motd_last_modified.tv_nsec = 0L;
            }
            ttak_mutex_unlock(&host->lock);
        }
        return;
    }

    struct timespec modified = host_stat_mtime(&file_info);

    if (had_file && modified.tv_sec == last_loaded.tv_sec &&
        modified.tv_nsec == last_loaded.tv_nsec) {
        return;
    }

    (void)host_try_load_motd_from_path(host, path_to_try);
}

static unsigned session_simple_hash(const char *text)
{
    unsigned hash = 5381U;
    if (text == nullptr) {
        return hash;
    }

    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor != '\0'; ++cursor) {
        hash = (hash * 33U) ^ *cursor;
    }
    return hash;
}

static void session_build_captcha_prompt(session_ctx_t *ctx,
                                         captcha_prompt_t *prompt)
{
    if (prompt == nullptr) {
        return;
    }

    memset(prompt, 0, sizeof(*prompt));

    unsigned basis =
        session_simple_hash(ctx != nullptr ? ctx->user.name : "user");
    basis ^= session_simple_hash(ctx != nullptr ? ctx->client_ip : "ip");

    unsigned entropy = 0U;
    struct timespec now = {0, 0};
    if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
        uint64_t now_sec = (uint64_t)now.tv_sec;
        entropy ^= (unsigned)now_sec;
        entropy ^= (unsigned)(now_sec >> 32);
        entropy ^= (unsigned)now.tv_nsec;
    } else {
        uint64_t fallback = (uint64_t)time(nullptr);
        entropy ^= (unsigned)fallback;
        entropy ^= (unsigned)(fallback >> 32);
    }

    host_t *host = (ctx != nullptr) ? ctx->owner : nullptr;
    if (host != nullptr) {
        ttak_mutex_lock(&host->lock);
        uint64_t nonce = ++host->captcha_nonce;
        ttak_mutex_unlock(&host->lock);
        entropy ^= (unsigned)nonce;
        entropy ^= (unsigned)(nonce >> 32);
    }

    basis ^= entropy;

    const unsigned variant_seed = basis ^ (basis >> 16U) ^ (entropy << 1U);
    unsigned prng_state = variant_seed | 1U;

    session_fill_digit_sum_prompt(prompt, &prng_state);
}

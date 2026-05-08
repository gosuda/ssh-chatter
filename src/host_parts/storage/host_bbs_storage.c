
static bool host_path_is_absolute(const char *path)
{
    return path != nullptr && path[0] == '/';
}

static void host_resolve_state_path(char *output, size_t output_size,
                                    const char *override_env,
                                    const char *default_name,
                                    const char *log_component)
{
    if (output == nullptr || output_size == 0U || default_name == nullptr ||
        log_component == nullptr) {
        return;
    }

    const char *configured = nullptr;
    if (override_env != nullptr && override_env[0] != '\0') {
        configured = getenv(override_env);
    }

    if (configured == nullptr || configured[0] == '\0') {
        configured = default_name;
    }

    const char *state_dir = getenv("CHATTER_STATE_DIR");
    bool use_state_dir = state_dir != nullptr && state_dir[0] != '\0' &&
                         !host_path_is_absolute(configured);

    int written = 0;
    if (use_state_dir) {
        written =
            snprintf(output, output_size, "%s/%s", state_dir, configured);
    } else {
        written = snprintf(output, output_size, "%s", configured);
    }

    if (written < 0 || (size_t)written >= output_size) {
        humanized_log_error(log_component, "bbs state file path is too long",
                            ENAMETOOLONG);
        output[0] = '\0';
    }
}

static void host_bbs_resolve_path(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    host_resolve_state_path(host->bbs_state_file_path,
                            sizeof(host->bbs_state_file_path),
                            "CHATTER_BBS_FILE", "bbs_state.dat", "bbs");
}

static size_t host_column_reset_sequence_length(const char *text)
{
    if (text == nullptr) {
        return 0U;
    }

    if (text[0] == '\033' && text[1] == '[' && text[2] == '1' &&
        text[3] == 'G') {
        return 4U;
    }

    if (text[0] == '[' && text[1] == '1' && text[2] == 'G') {
        return 3U;
    }

    return 0U;
}

static void host_strip_column_reset(char *text)
{
    if (text == nullptr || text[0] == '\0') {
        return;
    }

    char *dst = text;
    const char *src = text;
    while (*src != '\0') {
        size_t skip = host_column_reset_sequence_length(src);
        if (skip > 0U) {
            src += skip;
            continue;
        }

        *dst++ = *src++;
    }

    *dst = '\0';
}

static bool host_bbs_serialized_has_required_fields(
    const bbs_state_post_entry_t *serialized)
{
    if (serialized == nullptr) {
        return false;
    }

    if (serialized->id == 0U) {
        return false;
    }

    if (serialized->author[0] == '\0' || serialized->title[0] == '\0') {
        return false;
    }

    return true;
}

static bool host_bbs_text_has_disallowed_controls(const char *text)
{
    if (text == nullptr) {
        return true;
    }

    for (size_t idx = 0U; text[idx] != '\0'; ++idx) {
        unsigned char value = (unsigned char)text[idx];
        if (value == '\n' || value == '\r' || value == '\t') {
            continue;
        }
        if (value < 0x20U || value == 0x7FU) {
            return true;
        }
    }

    return false;
}

static bool host_bbs_serialized_is_sane(const bbs_state_post_entry_t *serialized,
                                        time_t now)
{
    if (serialized == nullptr) {
        return false;
    }

    if (host_bbs_text_has_disallowed_controls(serialized->author) ||
        host_bbs_text_has_disallowed_controls(serialized->title)) {
        return false;
    }

    const int64_t min_valid = 946684800; /* 2000-01-01 00:00:00 UTC */
    const int64_t max_valid = (int64_t)now + (7 * 24 * 60 * 60);
    if (serialized->created_at < min_valid ||
        serialized->bumped_at < serialized->created_at ||
        serialized->bumped_at > max_valid) {
        return false;
    }

    if (strstr(serialized->title, "[BREAKING NEWS]") != nullptr) {
        return false;
    }

    return true;
}

/* Read the on-disk record for @p post_id into @p out (zeroed on miss).
 * Returns true on hit, false if the file is missing or the id is unknown. */
static bool host_bbs_read_disk_record(const char *path, uint64_t post_id,
                                      bbs_state_post_entry_t *out)
{
    if (out == nullptr) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (path == nullptr || path[0] == '\0' || post_id == 0U) {
        return false;
    }

    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        return false;
    }
    bbs_state_header_t header = {0};
    if (fread(&header, sizeof(header), 1U, fp) != 1U ||
        header.magic != BBS_STATE_MAGIC) {
        fclose(fp);
        return false;
    }
    /* Only the current entry version supports the simple seek path; for
     * legacy versions fall through to a sequential scan. */
    bool found = false;
    if (header.version == BBS_STATE_VERSION) {
        bbs_state_post_entry_t serialized = {0};
        for (uint32_t idx = 0U; idx < header.post_count; ++idx) {
            if (fread(&serialized, sizeof(serialized), 1U, fp) != 1U) {
                break;
            }
            if (serialized.id == post_id) {
                *out = serialized;
                found = true;
                break;
            }
        }
    }
    fclose(fp);
    return found;
}

static void host_bbs_state_save_locked_with_change(
    host_t *host, uint64_t override_id,
    const bbs_post_content_t *override_content)
{
    if (!host_bbs_storage_ready(host)) {
        return;
    }

    if (host->bbs_state_file_path[0] == '\0') {
        return;
    }

    if (!host_ensure_private_data_path(host, host->bbs_state_file_path, true)) {
        return;
    }

    char temp_path[PATH_MAX];
    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp",
                           host->bbs_state_file_path);
    if (written < 0 || (size_t)written >= sizeof(temp_path)) {
        humanized_log_error("host", "bbs state file path is too long",
                            ENAMETOOLONG);
        return;
    }

    int temp_fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW,
                       S_IRUSR | S_IWUSR);
    if (temp_fd < 0) {
        humanized_log_error("host", "failed to open bbs state file",
                            errno != 0 ? errno : EIO);
        return;
    }

    FILE *fp = fdopen(temp_fd, "wb");
    if (fp == nullptr) {
        int saved_errno = errno;
        close(temp_fd);
        unlink(temp_path);
        humanized_log_error("host", "failed to wrap bbs state descriptor",
                            saved_errno != 0 ? saved_errno : EIO);
        return;
    }

    size_t capacity = host_bbs_loop_limit(host);
    uint32_t post_count = 0U;
    for (size_t idx = 0U; idx < capacity; ++idx) {
        if (host->bbs_posts[idx].in_use) {
            ++post_count;
        }
    }

    bbs_state_header_t header = {0};
    header.magic = BBS_STATE_MAGIC;
    header.version = BBS_STATE_VERSION;
    header.post_count = post_count;
    header.next_id = host->next_bbs_id;

    bool success = fwrite(&header, sizeof(header), 1U, fp) == 1U;

    for (size_t idx = 0U; success && idx < capacity; ++idx) {
        const bbs_post_t *post = &host->bbs_posts[idx];
        if (!post->in_use) {
            continue;
        }

        /* Build the disk record by combining the in-memory header with the
         * body+comments from either the explicit override (when this save
         * was triggered by a content-changing op like /bbs new) or the
         * existing on-disk record (otherwise — preserves prior content). */
        bbs_state_post_entry_t serialized = {0};
        if (override_content != nullptr && post->id == override_id) {
            snprintf(serialized.body, sizeof(serialized.body), "%s",
                     override_content->body);
            size_t copy_count = override_content->comment_count;
            if (copy_count > SSH_CHATTER_BBS_MAX_COMMENTS) {
                copy_count = SSH_CHATTER_BBS_MAX_COMMENTS;
            }
            for (size_t comment = 0U; comment < copy_count; ++comment) {
                serialized.comments[comment] =
                    override_content->comments[comment];
            }
            serialized.comment_count = (uint32_t)copy_count;
        } else {
            bbs_state_post_entry_t existing = {0};
            if (host_bbs_read_disk_record(host->bbs_state_file_path, post->id,
                                          &existing)) {
                memcpy(serialized.body, existing.body,
                       sizeof(serialized.body));
                memcpy(serialized.comments, existing.comments,
                       sizeof(serialized.comments));
                serialized.comment_count = existing.comment_count;
            }
        }

        serialized.id = post->id;
        serialized.created_at = (int64_t)post->created_at;
        serialized.bumped_at = (int64_t)post->bumped_at;
        serialized.tag_count = (uint32_t)post->tag_count;
        if (serialized.tag_count > SSH_CHATTER_BBS_MAX_TAGS) {
            serialized.tag_count = SSH_CHATTER_BBS_MAX_TAGS;
        }
        if ((size_t)serialized.comment_count > post->comment_count) {
            serialized.comment_count = (uint32_t)post->comment_count;
        }
        if (post->comment_count > SSH_CHATTER_BBS_MAX_COMMENTS) {
            serialized.comment_count = SSH_CHATTER_BBS_MAX_COMMENTS;
        } else if ((size_t)serialized.comment_count < post->comment_count &&
                   override_content == nullptr) {
            /* Header says more comments than disk had — clamp to memory. */
            serialized.comment_count = (uint32_t)post->comment_count;
        }

        snprintf(serialized.author, sizeof(serialized.author), "%s",
                 post->author);
        snprintf(serialized.title, sizeof(serialized.title), "%s", post->title);

        for (size_t tag = 0U; tag < serialized.tag_count; ++tag) {
            snprintf(serialized.tags[tag], sizeof(serialized.tags[tag]), "%s",
                     post->tags[tag]);
        }

        if (fwrite(&serialized, sizeof(serialized), 1U, fp) != 1U) {
            success = false;
            break;
        }
    }

    if (success && fflush(fp) != 0) {
        success = false;
    }

    if (success) {
        int file_descriptor = fileno(fp);
        if (file_descriptor >= 0 && fsync(file_descriptor) != 0) {
            success = false;
        }
    }

    if (fclose(fp) != 0) {
        success = false;
    }

    if (!success) {
        humanized_log_error("host", "failed to write bbs state file", errno);
        unlink(temp_path);
        return;
    }

    if (chmod(temp_path, S_IRUSR | S_IWUSR) != 0) {
        humanized_log_error("host",
                            "failed to tighten temporary bbs state permissions",
                            errno != 0 ? errno : EACCES);
        unlink(temp_path);
        return;
    }

    if (rename(temp_path, host->bbs_state_file_path) != 0) {
        humanized_log_error("host", "failed to update bbs state file", errno);
        unlink(temp_path);
    } else if (chmod(host->bbs_state_file_path, S_IRUSR | S_IWUSR) != 0) {
        humanized_log_error("host", "failed to tighten bbs state permissions",
                            errno != 0 ? errno : EACCES);
    }
}

/* Save with no content override — used by header-only changes (delete /
 * bump / watchdog).  Body+comments for each post are preserved verbatim
 * from the existing on-disk record. */
static void host_bbs_state_save_locked(host_t *host)
{
    host_bbs_state_save_locked_with_change(host, 0U, nullptr);
}

/* --- Lazy body/comment loaders --- */

/* Sidecar map registry — one map per outstanding content acquire.  The
 * RM tracks the abstract handle, this table tracks the live map. */
typedef struct host_bbs_content_map_entry {
    ttak_abstract_mem_t *handle;
    ttak_abstract_map_t map;
} host_bbs_content_map_entry_t;

static host_bbs_content_map_entry_t g_bbs_content_maps[16];
static pthread_mutex_t g_bbs_content_maps_lock = PTHREAD_MUTEX_INITIALIZER;

static bool host_bbs_content_map_register(ttak_abstract_mem_t *handle,
                                          ttak_abstract_map_t map)
{
    pthread_mutex_lock(&g_bbs_content_maps_lock);
    bool ok = false;
    for (size_t idx = 0U;
         idx < (sizeof(g_bbs_content_maps) / sizeof(g_bbs_content_maps[0]));
         ++idx) {
        if (g_bbs_content_maps[idx].handle == nullptr) {
            g_bbs_content_maps[idx].handle = handle;
            g_bbs_content_maps[idx].map = map;
            ok = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_bbs_content_maps_lock);
    return ok;
}

static bool host_bbs_content_map_take(ttak_abstract_mem_t *handle,
                                      ttak_abstract_map_t *out_map)
{
    pthread_mutex_lock(&g_bbs_content_maps_lock);
    bool ok = false;
    for (size_t idx = 0U;
         idx < (sizeof(g_bbs_content_maps) / sizeof(g_bbs_content_maps[0]));
         ++idx) {
        if (g_bbs_content_maps[idx].handle == handle) {
            if (out_map != nullptr) {
                *out_map = g_bbs_content_maps[idx].map;
            }
            g_bbs_content_maps[idx].handle = nullptr;
            memset(&g_bbs_content_maps[idx].map, 0,
                   sizeof(g_bbs_content_maps[idx].map));
            ok = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_bbs_content_maps_lock);
    return ok;
}

bool host_bbs_content_acquire_empty(host_t *host,
                                    ttak_abstract_mem_t **out_handle,
                                    bbs_post_content_t **out_content)
{
    return host_bbs_content_acquire(host, 0U, out_handle, out_content);
}

bool host_bbs_content_acquire(host_t *host, uint64_t post_id,
                              ttak_abstract_mem_t **out_handle,
                              bbs_post_content_t **out_content)
{
    if (host == nullptr || out_handle == nullptr || out_content == nullptr ||
        host->resource_manager == nullptr) {
        return false;
    }

    *out_handle = nullptr;
    *out_content = nullptr;

    ttak_abstract_mem_t *handle = sshc_rm_scope_alloc(
        host->resource_manager, sizeof(bbs_post_content_t), "bbs_content");
    if (handle == nullptr) {
        return false;
    }

    ttak_abstract_map_t map;
    memset(&map, 0, sizeof(map));
    if (ttak_abstract_map(handle, 0U, sizeof(bbs_post_content_t),
                          TTAK_ABSTRACT_ACCESS_WRITE, &map) != 0) {
        sshc_rm_scope_free(host->resource_manager, handle);
        return false;
    }

    bbs_post_content_t *content = (bbs_post_content_t *)map.data;
    if (content == nullptr) {
        ttak_abstract_unmap(&map);
        sshc_rm_scope_free(host->resource_manager, handle);
        return false;
    }
    memset(content, 0, sizeof(*content));

    /* Read body + comments from disk for the requested post id (if present). */
    if (post_id != 0U && host->bbs_state_file_path[0] != '\0') {
        bbs_state_post_entry_t serialized = {0};
        if (host_bbs_read_disk_record(host->bbs_state_file_path, post_id,
                                      &serialized)) {
            memcpy(content->body, serialized.body, sizeof(content->body));
            content->body[sizeof(content->body) - 1U] = '\0';
            host_strip_column_reset(content->body);

            size_t copy_count = serialized.comment_count;
            if (copy_count > SSH_CHATTER_BBS_MAX_COMMENTS) {
                copy_count = SSH_CHATTER_BBS_MAX_COMMENTS;
            }
            for (size_t comment = 0U; comment < copy_count; ++comment) {
                content->comments[comment] = serialized.comments[comment];
                host_strip_column_reset(content->comments[comment].author);
                host_strip_column_reset(content->comments[comment].text);
            }
            content->comment_count = copy_count;
        }
    }

    if (!host_bbs_content_map_register(handle, map)) {
        ttak_abstract_unmap(&map);
        sshc_rm_scope_free(host->resource_manager, handle);
        return false;
    }

    *out_handle = handle;
    *out_content = content;
    return true;
}

void host_bbs_content_release(host_t *host, ttak_abstract_mem_t *handle)
{
    if (host == nullptr || handle == nullptr ||
        host->resource_manager == nullptr) {
        return;
    }
    ttak_abstract_map_t map;
    if (host_bbs_content_map_take(handle, &map)) {
        ttak_abstract_unmap(&map);
    }
    sshc_rm_scope_free(host->resource_manager, handle);
}

static void host_bbs_state_load(host_t *host)
{
    if (host == nullptr) {
        return;
    }
    host->bbs_cache_loaded = false;
    if (!host_bbs_storage_ready(host)) {
        return;
    }

    if (host->bbs_state_file_path[0] == '\0') {
        host->bbs_cache_loaded = true;
        return;
    }

    if (!host_ensure_private_data_path(host, host->bbs_state_file_path,
                                       false)) {
        host->bbs_cache_loaded = true;
        return;
    }

    FILE *fp = fopen(host->bbs_state_file_path, "rb");
    if (fp == nullptr) {
        host->bbs_cache_loaded = true;
        return;
    }

    int fd = fileno(fp);
    if (fd < 0) {
        fclose(fp);
        host->bbs_cache_loaded = true;
        return;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        fclose(fp);
        host->bbs_cache_loaded = true;
        return;
    }

    size_t mapped_len = (size_t)st.st_size;
    if (mapped_len < sizeof(bbs_state_header_t)) {
        fclose(fp);
        host->bbs_cache_loaded = true;
        return;
    }

    unsigned char *mapped = mmap(nullptr, mapped_len, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE, fd, 0);
    fclose(fp);
    fp = nullptr;
    if (mapped == MAP_FAILED) {
        host->bbs_cache_loaded = true;
        return;
    }

    const unsigned char *cursor = mapped;
    size_t remaining = mapped_len;

    bbs_state_header_t header = {0};
    memcpy(&header, cursor, sizeof(header));
    cursor += sizeof(header);
    remaining -= sizeof(header);

    if (header.magic != BBS_STATE_MAGIC) {
        memset(mapped, 0, mapped_len);
        munmap(mapped, mapped_len);
        host->bbs_cache_loaded = true;
        return;
    }

    if (header.version == 0U || header.version > BBS_STATE_VERSION) {
        memset(mapped, 0, mapped_len);
        munmap(mapped, mapped_len);
        host->bbs_cache_loaded = true;
        return;
    }

    ttak_mutex_lock(&host->lock);

    size_t capacity = host_bbs_loop_limit(host);
    for (size_t idx = 0U; idx < capacity; ++idx) {
        memset(&host->bbs_posts[idx], 0, sizeof(host->bbs_posts[idx]));
    }
    host->bbs_post_count = 0U;

    uint64_t max_id = 0U;
    bool success = true;

    time_t now = time(nullptr);
    if (now <= 0) {
        now = 1;
    }

    for (uint32_t idx = 0U; idx < header.post_count; ++idx) {
        bbs_state_post_entry_t serialized = {0};
        if (header.version == 1U) {
            bbs_state_post_entry_v1_t legacy = {0};
            if (remaining < sizeof(legacy)) {
                success = false;
                break;
            }
            memcpy(&legacy, cursor, sizeof(legacy));
            cursor += sizeof(legacy);
            remaining -= sizeof(legacy);

            serialized.id = legacy.id;
            serialized.created_at = legacy.created_at;
            serialized.bumped_at = legacy.bumped_at;
            serialized.tag_count = legacy.tag_count;
            serialized.comment_count = legacy.comment_count;
            snprintf(serialized.author, sizeof(serialized.author), "%s",
                     legacy.author);
            snprintf(serialized.title, sizeof(serialized.title), "%s",
                     legacy.title);
            snprintf(serialized.body, sizeof(serialized.body), "%s",
                     legacy.body);
            for (size_t tag = 0U; tag < SSH_CHATTER_BBS_MAX_TAGS; ++tag) {
                snprintf(serialized.tags[tag], sizeof(serialized.tags[tag]),
                         "%s", legacy.tags[tag]);
            }
            for (size_t comment = 0U; comment < SSH_CHATTER_BBS_MAX_COMMENTS;
                 ++comment) {
                snprintf(serialized.comments[comment].author,
                         sizeof(serialized.comments[comment].author), "%s",
                         legacy.comments[comment].author);
                snprintf(serialized.comments[comment].text,
                         sizeof(serialized.comments[comment].text), "%s",
                         legacy.comments[comment].text);
                serialized.comments[comment].created_at =
                    legacy.comments[comment].created_at;
            }
        } else if (header.version == 2U) {
            bbs_state_post_entry_v2_t legacy = {0};
            if (remaining < sizeof(legacy)) {
                success = false;
                break;
            }
            memcpy(&legacy, cursor, sizeof(legacy));
            cursor += sizeof(legacy);
            remaining -= sizeof(legacy);

            serialized.id = legacy.id;
            serialized.created_at = legacy.created_at;
            serialized.bumped_at = legacy.bumped_at;
            serialized.tag_count = legacy.tag_count;
            serialized.comment_count = legacy.comment_count;
            snprintf(serialized.author, sizeof(serialized.author), "%s",
                     legacy.author);
            snprintf(serialized.title, sizeof(serialized.title), "%s",
                     legacy.title);
            snprintf(serialized.body, sizeof(serialized.body), "%s",
                     legacy.body);
            for (size_t tag = 0U; tag < SSH_CHATTER_BBS_MAX_TAGS; ++tag) {
                snprintf(serialized.tags[tag], sizeof(serialized.tags[tag]),
                         "%s", legacy.tags[tag]);
            }
            for (size_t comment = 0U; comment < SSH_CHATTER_BBS_MAX_COMMENTS;
                 ++comment) {
                snprintf(serialized.comments[comment].author,
                         sizeof(serialized.comments[comment].author), "%s",
                         legacy.comments[comment].author);
                snprintf(serialized.comments[comment].text,
                         sizeof(serialized.comments[comment].text), "%s",
                         legacy.comments[comment].text);
                serialized.comments[comment].created_at =
                    legacy.comments[comment].created_at;
            }
        } else if (header.version == 3U) {
            bbs_state_post_entry_v3_t legacy = {0};
            if (remaining < sizeof(legacy)) {
                success = false;
                break;
            }
            memcpy(&legacy, cursor, sizeof(legacy));
            cursor += sizeof(legacy);
            remaining -= sizeof(legacy);

            serialized.id = legacy.id;
            serialized.created_at = legacy.created_at;
            serialized.bumped_at = legacy.bumped_at;
            serialized.tag_count = legacy.tag_count;
            serialized.comment_count = legacy.comment_count;
            snprintf(serialized.author, sizeof(serialized.author), "%s",
                     legacy.author);
            snprintf(serialized.title, sizeof(serialized.title), "%s",
                     legacy.title);
            snprintf(serialized.body, sizeof(serialized.body), "%s",
                     legacy.body);
            for (size_t tag = 0U; tag < SSH_CHATTER_BBS_MAX_TAGS; ++tag) {
                snprintf(serialized.tags[tag], sizeof(serialized.tags[tag]),
                         "%s", legacy.tags[tag]);
            }
            for (size_t comment = 0U; comment < SSH_CHATTER_BBS_MAX_COMMENTS;
                 ++comment) {
                snprintf(serialized.comments[comment].author,
                         sizeof(serialized.comments[comment].author), "%s",
                         legacy.comments[comment].author);
                snprintf(serialized.comments[comment].text,
                         sizeof(serialized.comments[comment].text), "%s",
                         legacy.comments[comment].text);
                serialized.comments[comment].created_at =
                    legacy.comments[comment].created_at;
            }
        } else {
            if (remaining < sizeof(serialized)) {
                success = false;
                break;
            }
            memcpy(&serialized, cursor, sizeof(serialized));
            cursor += sizeof(serialized);
            remaining -= sizeof(serialized);
        }

        serialized.author[sizeof(serialized.author) - 1U] = '\0';
        serialized.title[sizeof(serialized.title) - 1U] = '\0';
        serialized.body[sizeof(serialized.body) - 1U] = '\0';
        for (size_t tag = 0U; tag < SSH_CHATTER_BBS_MAX_TAGS; ++tag) {
            serialized.tags[tag][sizeof(serialized.tags[tag]) - 1U] = '\0';
        }
        for (size_t comment = 0U; comment < SSH_CHATTER_BBS_MAX_COMMENTS;
             ++comment) {
            serialized.comments[comment]
                .author[sizeof(serialized.comments[comment].author) - 1U] =
                '\0';
            serialized.comments[comment]
                .text[sizeof(serialized.comments[comment].text) - 1U] = '\0';
            if (serialized.comments[comment].created_at <= 0) {
                serialized.comments[comment].created_at = (int64_t)now;
            }
        }
        if (serialized.created_at <= 0) {
            serialized.created_at = (int64_t)now;
        }
        if (serialized.bumped_at <= 0 ||
            serialized.bumped_at < serialized.created_at) {
            serialized.bumped_at = serialized.created_at;
        }

        if (!host_bbs_serialized_has_required_fields(&serialized) ||
            !host_bbs_serialized_is_sane(&serialized, now)) {
            continue;
        }

        if (serialized.id > max_id) {
            max_id = serialized.id;
        }

        if (idx >= host->bbs_post_capacity) {
            continue;
        }

        bbs_post_t *post = &host->bbs_posts[host->bbs_post_count];
        memset(post, 0, sizeof(*post));
        post->in_use = true;
        post->id = serialized.id;
        post->created_at = (time_t)serialized.created_at;
        post->bumped_at = (time_t)serialized.bumped_at;
        snprintf(post->author, sizeof(post->author), "%s", serialized.author);
        snprintf(post->title, sizeof(post->title), "%s", serialized.title);
        host_strip_column_reset(post->author);
        host_strip_column_reset(post->title);
        /* Body is intentionally NOT loaded into memory — fetched lazily by
         * host_bbs_content_acquire when /bbs read needs it. */

        size_t tag_limit = serialized.tag_count;
        if (tag_limit > SSH_CHATTER_BBS_MAX_TAGS) {
            tag_limit = SSH_CHATTER_BBS_MAX_TAGS;
        }
        post->tag_count = tag_limit;
        for (size_t tag = 0U; tag < tag_limit; ++tag) {
            snprintf(post->tags[tag], sizeof(post->tags[tag]), "%s",
                     serialized.tags[tag]);
            host_strip_column_reset(post->tags[tag]);
        }

        size_t comment_limit = serialized.comment_count;
        if (comment_limit > SSH_CHATTER_BBS_MAX_COMMENTS) {
            comment_limit = SSH_CHATTER_BBS_MAX_COMMENTS;
        }
        post->comment_count = comment_limit;
        /* Comment payloads also live on disk; only the count stays
         * resident. */

        ++host->bbs_post_count;
    }

    if (success) {
        host->next_bbs_id = header.next_id;
        if (host->next_bbs_id == 0U || host->next_bbs_id <= max_id) {
            host->next_bbs_id = max_id + 1U;
        }
    } else {
        for (size_t idx = 0U; idx < capacity; ++idx) {
            memset(&host->bbs_posts[idx], 0, sizeof(host->bbs_posts[idx]));
        }
        host->bbs_post_count = 0U;
        host->next_bbs_id = 1U;
    }

    ttak_mutex_unlock(&host->lock);
    memset(mapped, 0, mapped_len);
    munmap(mapped, mapped_len);
    host->bbs_cache_loaded = true;
}

static void host_bbs_watchdog_scan(host_t *host)
{
    if (!host_bbs_storage_ready(host)) {
        return;
    }

    if (!atomic_load(&host->eliza_enabled)) {
        return;
    }

    if (!atomic_load(&host->security_ai_enabled)) {
        return;
    }

    bbs_post_t *snapshot = (bbs_post_t *)ttak_mem_alloc(
        SSH_CHATTER_BBS_MAX_POSTS * sizeof(*snapshot),
        __TTAK_UNSAFE_MEM_FOREVER__, ttak_get_tick_count());
    if (snapshot == nullptr) {
        humanized_log_error("bbs", "failed to allocate watchdog snapshot",
                            ENOMEM);
        return;
    }
    memset(snapshot, 0, SSH_CHATTER_BBS_MAX_POSTS * sizeof(*snapshot));

    size_t snapshot_count = 0U;

    ttak_mutex_lock(&host->lock);
    for (size_t idx = 0U; idx < SSH_CHATTER_BBS_MAX_POSTS; ++idx) {
        if (!host->bbs_posts[idx].in_use) {
            continue;
        }

        if (snapshot_count < SSH_CHATTER_BBS_MAX_POSTS) {
            snapshot[snapshot_count++] = host->bbs_posts[idx];
        }
    }
    ttak_mutex_unlock(&host->lock);

    if (snapshot_count == 0U) {
        ttak_mem_free(snapshot);
        return;
    }

    const size_t content_capacity =
        SSH_CHATTER_BBS_BODY_LEN +
        (SSH_CHATTER_BBS_COMMENT_LEN * SSH_CHATTER_BBS_MAX_COMMENTS) + 1024U;
    char *content = (char *)ttak_mem_alloc(content_capacity,
                                           __TTAK_UNSAFE_MEM_FOREVER__,
                                           ttak_get_tick_count());
    if (content == nullptr) {
        humanized_log_error("bbs", "failed to allocate watchdog buffer",
                            ENOMEM);
        ttak_mem_free(snapshot);
        return;
    }

    for (size_t idx = 0U; idx < snapshot_count; ++idx) {
        const bbs_post_t *post = &snapshot[idx];

        int written =
            snprintf(content, content_capacity, "Title: %s\nTags: ",
                     post->title[0] != '\0' ? post->title : "(untitled)");
        if (written < 0) {
            continue;
        }

        size_t offset = (size_t)written;
        if (offset >= content_capacity) {
            offset = content_capacity - 1U;
        }

        for (size_t tag = 0U; tag < post->tag_count; ++tag) {
            const char *prefix = (tag == 0U) ? "" : ",";
            int tag_written =
                snprintf(content + offset, content_capacity - offset, "%s%s",
                         prefix, post->tags[tag]);
            if (tag_written < 0) {
                break;
            }
            offset += (size_t)tag_written;
            if (offset >= content_capacity) {
                offset = content_capacity - 1U;
                break;
            }
        }

        if (offset + 2U < content_capacity) {
            content[offset++] = '\n';
            content[offset++] = '\n';
            content[offset] = '\0';
        } else {
            content[content_capacity - 1U] = '\0';
            offset = content_capacity - 1U;
        }

        /* Lazily fetch body+comments for this post — they live on disk. */
        ttak_abstract_mem_t *body_handle = nullptr;
        bbs_post_content_t *body_content = nullptr;
        bool have_body = host_bbs_content_acquire(host, post->id,
                                                  &body_handle, &body_content);

        int body_written = snprintf(
            content + offset, content_capacity - offset, "Body:\n%s",
            (have_body && body_content->body[0] != '\0') ? body_content->body
                                                         : "(empty)");
        if (body_written < 0) {
            if (body_handle != nullptr) {
                host_bbs_content_release(host, body_handle);
            }
            continue;
        }
        offset += (size_t)body_written;
        if (offset >= content_capacity) {
            offset = content_capacity - 1U;
        }

        size_t comment_count = have_body ? body_content->comment_count : 0U;
        for (size_t comment = 0U; comment < comment_count; ++comment) {
            if (offset + 2U >= content_capacity) {
                break;
            }
            content[offset++] = '\n';
            content[offset++] = '\n';
            content[offset] = '\0';

            const bbs_comment_t *entry = &body_content->comments[comment];
            int comment_written = snprintf(
                content + offset, content_capacity - offset,
                "Comment by %s:\n%s",
                entry->author[0] != '\0' ? entry->author : "(anonymous)",
                entry->text[0] != '\0' ? entry->text : "(empty)");
            if (comment_written < 0) {
                break;
            }
            offset += (size_t)comment_written;
            if (offset >= content_capacity) {
                offset = content_capacity - 1U;
                break;
            }
        }
        if (body_handle != nullptr) {
            host_bbs_content_release(host, body_handle);
        }

        bool blocked = false;
        char reason[256];
        reason[0] = '\0';
        if (!translator_moderate_text("bbs_post", content, &blocked, reason,
                                      sizeof(reason))) {
            const char *error = translator_last_error();
            if (error != nullptr && error[0] != '\0') {
                printf("[bbs] moderation unavailable for post #%" PRIu64
                       ": %s\n",
                       post->id, error);
            } else {
                printf("[bbs] moderation unavailable for post #%" PRIu64 "\n",
                       post->id);
            }
            break;
        }

        if (!blocked) {
            continue;
        }

        trim_whitespace_inplace(reason);
        const char *diagnostic =
            (reason[0] != '\0') ? reason : "policy violation";

        ttak_mutex_lock(&host->lock);
        bbs_post_t *live = host_find_bbs_post_locked(host, post->id);
        if (live != nullptr) {
            host_clear_bbs_post_locked(host, live);
            host_bbs_state_save_locked(host);
        }
        ttak_mutex_unlock(&host->lock);

        if (live == nullptr) {
            continue;
        }

        printf("[bbs] removed post #%" PRIu64 " by %s (%s)\n", post->id,
               post->author[0] != '\0' ? post->author : "unknown", diagnostic);

        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(notice, sizeof(notice),
                 "* [eliza] removed BBS post #%" PRIu64 " by %s (%s).",
                 post->id, post->author[0] != '\0' ? post->author : "unknown",
                 diagnostic);
        host_history_record_system(host, notice, nullptr);
        chat_room_broadcast(&host->room, notice, nullptr);
    }

    ttak_mem_free(content);
    ttak_mem_free(snapshot);
}

static void *host_bbs_watchdog_thread(void *arg)
{
    host_t *host = (host_t *)arg;
    if (host == nullptr) {
        return nullptr;
    }

    sshc_epoch_thread_enter();
    sshc_memory_context_t *memory_scope =
        sshc_memory_context_push(host->memory_context);

    atomic_store(&host->bbs_watchdog_thread_running, true);
    printf("[bbs] watchdog thread started\n");

    while (!atomic_load(&host->bbs_watchdog_thread_stop)) {
        host_bbs_watchdog_scan(host);

        clock_gettime(CLOCK_MONOTONIC, &host->bbs_watchdog_last_run);

        unsigned int remaining = SSH_CHATTER_BBS_REVIEW_INTERVAL_SECONDS;
        while (remaining > 0U &&
               !atomic_load(&host->bbs_watchdog_thread_stop)) {
            unsigned int chunk =
                remaining > SSH_CHATTER_BBS_WATCHDOG_SLEEP_SECONDS
                    ? SSH_CHATTER_BBS_WATCHDOG_SLEEP_SECONDS
                    : remaining;
            struct timespec pause = {
                .tv_sec = (time_t)chunk,
                .tv_nsec = 0L,
            };
            host_sleep_uninterruptible(&pause);
            if (remaining <= chunk) {
                remaining = 0U;
            } else {
                remaining -= chunk;
            }
        }
    }

    atomic_store(&host->bbs_watchdog_thread_running, false);
    printf("[bbs] watchdog thread stopped\n");
    sshc_memory_context_pop(memory_scope);
    sshc_epoch_thread_exit();
    return nullptr;
}

static void host_bbs_start_watchdog(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (host->bbs_watchdog_thread_initialized) {
        return;
    }

    atomic_store(&host->bbs_watchdog_thread_stop, false);
    atomic_store(&host->bbs_watchdog_thread_running, false);

    int error = pthread_create(&host->bbs_watchdog_thread, nullptr,
                               host_bbs_watchdog_thread, host);
    if (error != 0) {
        printf("[bbs] failed to start watchdog thread: %s\n", strerror(error));
        return;
    }

    host->bbs_watchdog_thread_initialized = true;
}

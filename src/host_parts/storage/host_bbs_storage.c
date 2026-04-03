
static void host_bbs_resolve_path(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    const char *bbs_path = getenv("CHATTER_BBS_FILE");
    if (bbs_path == nullptr || bbs_path[0] == '\0') {
        bbs_path = "bbs_state.dat";
    }

    int written = snprintf(host->bbs_state_file_path,
                           sizeof(host->bbs_state_file_path), "%s", bbs_path);
    if (written < 0 || (size_t)written >= sizeof(host->bbs_state_file_path)) {
        humanized_log_error("host", "bbs state file path is too long",
                            ENAMETOOLONG);
        host->bbs_state_file_path[0] = '\0';
    }
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

static void host_bbs_state_save_locked(host_t *host)
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

        bbs_state_post_entry_t serialized = {0};
        serialized.id = post->id;
        serialized.created_at = (int64_t)post->created_at;
        serialized.bumped_at = (int64_t)post->bumped_at;
        serialized.tag_count = (uint32_t)post->tag_count;
        if (serialized.tag_count > SSH_CHATTER_BBS_MAX_TAGS) {
            serialized.tag_count = SSH_CHATTER_BBS_MAX_TAGS;
        }
        serialized.comment_count = (uint32_t)post->comment_count;
        if (serialized.comment_count > SSH_CHATTER_BBS_MAX_COMMENTS) {
            serialized.comment_count = SSH_CHATTER_BBS_MAX_COMMENTS;
        }

        snprintf(serialized.author, sizeof(serialized.author), "%s",
                 post->author);
        snprintf(serialized.title, sizeof(serialized.title), "%s", post->title);
        snprintf(serialized.body, sizeof(serialized.body), "%s", post->body);

        for (size_t tag = 0U; tag < serialized.tag_count; ++tag) {
            snprintf(serialized.tags[tag], sizeof(serialized.tags[tag]), "%s",
                     post->tags[tag]);
        }

        for (size_t comment = 0U; comment < serialized.comment_count;
             ++comment) {
            snprintf(serialized.comments[comment].author,
                     sizeof(serialized.comments[comment].author), "%s",
                     post->comments[comment].author);
            snprintf(serialized.comments[comment].text,
                     sizeof(serialized.comments[comment].text), "%s",
                     post->comments[comment].text);
            serialized.comments[comment].created_at =
                (int64_t)post->comments[comment].created_at;
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

static void host_bbs_state_load(host_t *host)
{
    if (!host_bbs_storage_ready(host)) {
        return;
    }

    if (host->bbs_state_file_path[0] == '\0') {
        return;
    }

    if (!host_ensure_private_data_path(host, host->bbs_state_file_path,
                                       false)) {
        return;
    }

    FILE *fp = fopen(host->bbs_state_file_path, "rb");
    if (fp == nullptr) {
        return;
    }

    int fd = fileno(fp);
    if (fd < 0) {
        fclose(fp);
        return;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        fclose(fp);
        return;
    }

    size_t mapped_len = (size_t)st.st_size;
    if (mapped_len < sizeof(bbs_state_header_t)) {
        fclose(fp);
        return;
    }

    unsigned char *mapped = mmap(nullptr, mapped_len, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE, fd, 0);
    fclose(fp);
    fp = nullptr;
    if (mapped == MAP_FAILED) {
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
        return;
    }

    if (header.version == 0U || header.version > BBS_STATE_VERSION) {
        memset(mapped, 0, mapped_len);
        munmap(mapped, mapped_len);
        return;
    }

    ttak_mutex_lock(&host->lock);

    size_t capacity = host_bbs_loop_limit(host);
    for (size_t idx = 0U; idx < capacity; ++idx) {
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
        snprintf(post->body, sizeof(post->body), "%s", serialized.body);
        host_strip_column_reset(post->author);
        host_strip_column_reset(post->title);
        host_strip_column_reset(post->body);

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
        for (size_t comment = 0U; comment < comment_limit; ++comment) {
            snprintf(post->comments[comment].author,
                     sizeof(post->comments[comment].author), "%s",
                     serialized.comments[comment].author);
            snprintf(post->comments[comment].text,
                     sizeof(post->comments[comment].text), "%s",
                     serialized.comments[comment].text);
            post->comments[comment].created_at =
                (time_t)serialized.comments[comment].created_at;
            host_strip_column_reset(post->comments[comment].author);
            host_strip_column_reset(post->comments[comment].text);
        }

        ++host->bbs_post_count;
    }

    if (success) {
        host->next_bbs_id = header.next_id;
        if (host->next_bbs_id == 0U || host->next_bbs_id <= max_id) {
            host->next_bbs_id = max_id + 1U;
        }
    } else {
        for (size_t idx = 0U; idx < capacity; ++idx) {
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
        host->bbs_post_count = 0U;
        host->next_bbs_id = 1U;
    }

    ttak_mutex_unlock(&host->lock);
    memset(mapped, 0, mapped_len);
    munmap(mapped, mapped_len);
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

        int body_written =
            snprintf(content + offset, content_capacity - offset, "Body:\n%s",
                     post->body[0] != '\0' ? post->body : "(empty)");
        if (body_written < 0) {
            continue;
        }
        offset += (size_t)body_written;
        if (offset >= content_capacity) {
            offset = content_capacity - 1U;
        }

        for (size_t comment = 0U; comment < post->comment_count; ++comment) {
            if (offset + 2U >= content_capacity) {
                break;
            }
            content[offset++] = '\n';
            content[offset++] = '\n';
            content[offset] = '\0';

            const bbs_comment_t *entry = &post->comments[comment];
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

static void bbs_format_time(time_t value, char *buffer, size_t length)
{
    if (buffer == nullptr || length == 0U) {
        return;
    }
    struct tm tm_value;
    if (localtime_r(&value, &tm_value) == nullptr) {
        snprintf(buffer, length, "-");
        return;
    }
    strftime(buffer, length, "%Y-%m-%d %H:%M", &tm_value);
}

static bool bbs_post_has_required_fields(const bbs_post_t *post)
{
    if (post == nullptr || !post->in_use) {
        return false;
    }

    if (post->id == 0U) {
        return false;
    }

    if (post->author[0] == '\0' || post->title[0] == '\0') {
        return false;
    }

    return true;
}

typedef struct bbs_listing {
    uint64_t id;
    char title[SSH_CHATTER_BBS_TITLE_LEN];
    char author[SSH_CHATTER_USERNAME_LEN];
    char tags[SSH_CHATTER_BBS_MAX_TAGS][SSH_CHATTER_BBS_TAG_LEN];
    size_t tag_count;
    time_t created_at;
    time_t bumped_at;
} bbs_listing_t;

static bool session_bbs_read_serialized_entry(
    const unsigned char **cursor_ptr, size_t *remaining_ptr, uint32_t version,
    bbs_state_post_entry_t *serialized)
{
    if (cursor_ptr == nullptr || remaining_ptr == nullptr ||
        serialized == nullptr) {
        return false;
    }

    const unsigned char *cursor = *cursor_ptr;
    size_t remaining = *remaining_ptr;
    memset(serialized, 0, sizeof(*serialized));

    if (version == 1U) {
        bbs_state_post_entry_v1_t legacy = {0};
        if (remaining < sizeof(legacy)) {
            return false;
        }
        memcpy(&legacy, cursor, sizeof(legacy));
        cursor += sizeof(legacy);
        remaining -= sizeof(legacy);

        serialized->id = legacy.id;
        serialized->created_at = legacy.created_at;
        serialized->bumped_at = legacy.bumped_at;
        serialized->tag_count = legacy.tag_count;
        serialized->comment_count = legacy.comment_count;
        snprintf(serialized->author, sizeof(serialized->author), "%s",
                 legacy.author);
        snprintf(serialized->title, sizeof(serialized->title), "%s",
                 legacy.title);
        snprintf(serialized->body, sizeof(serialized->body), "%s",
                 legacy.body);
        for (size_t tag = 0U; tag < SSH_CHATTER_BBS_MAX_TAGS; ++tag) {
            snprintf(serialized->tags[tag], sizeof(serialized->tags[tag]), "%s",
                     legacy.tags[tag]);
        }
        for (size_t comment = 0U; comment < SSH_CHATTER_BBS_MAX_COMMENTS;
             ++comment) {
            snprintf(serialized->comments[comment].author,
                     sizeof(serialized->comments[comment].author), "%s",
                     legacy.comments[comment].author);
            snprintf(serialized->comments[comment].text,
                     sizeof(serialized->comments[comment].text), "%s",
                     legacy.comments[comment].text);
            serialized->comments[comment].created_at =
                legacy.comments[comment].created_at;
        }
    } else if (version == 2U) {
        bbs_state_post_entry_v2_t legacy = {0};
        if (remaining < sizeof(legacy)) {
            return false;
        }
        memcpy(&legacy, cursor, sizeof(legacy));
        cursor += sizeof(legacy);
        remaining -= sizeof(legacy);

        serialized->id = legacy.id;
        serialized->created_at = legacy.created_at;
        serialized->bumped_at = legacy.bumped_at;
        serialized->tag_count = legacy.tag_count;
        serialized->comment_count = legacy.comment_count;
        snprintf(serialized->author, sizeof(serialized->author), "%s",
                 legacy.author);
        snprintf(serialized->title, sizeof(serialized->title), "%s",
                 legacy.title);
        snprintf(serialized->body, sizeof(serialized->body), "%s",
                 legacy.body);
        for (size_t tag = 0U; tag < SSH_CHATTER_BBS_MAX_TAGS; ++tag) {
            snprintf(serialized->tags[tag], sizeof(serialized->tags[tag]), "%s",
                     legacy.tags[tag]);
        }
        for (size_t comment = 0U; comment < SSH_CHATTER_BBS_MAX_COMMENTS;
             ++comment) {
            snprintf(serialized->comments[comment].author,
                     sizeof(serialized->comments[comment].author), "%s",
                     legacy.comments[comment].author);
            snprintf(serialized->comments[comment].text,
                     sizeof(serialized->comments[comment].text), "%s",
                     legacy.comments[comment].text);
            serialized->comments[comment].created_at =
                legacy.comments[comment].created_at;
        }
    } else if (version == 3U) {
        bbs_state_post_entry_v3_t legacy = {0};
        if (remaining < sizeof(legacy)) {
            return false;
        }
        memcpy(&legacy, cursor, sizeof(legacy));
        cursor += sizeof(legacy);
        remaining -= sizeof(legacy);

        serialized->id = legacy.id;
        serialized->created_at = legacy.created_at;
        serialized->bumped_at = legacy.bumped_at;
        serialized->tag_count = legacy.tag_count;
        serialized->comment_count = legacy.comment_count;
        snprintf(serialized->author, sizeof(serialized->author), "%s",
                 legacy.author);
        snprintf(serialized->title, sizeof(serialized->title), "%s",
                 legacy.title);
        snprintf(serialized->body, sizeof(serialized->body), "%s",
                 legacy.body);
        for (size_t tag = 0U; tag < SSH_CHATTER_BBS_MAX_TAGS; ++tag) {
            snprintf(serialized->tags[tag], sizeof(serialized->tags[tag]), "%s",
                     legacy.tags[tag]);
        }
        for (size_t comment = 0U; comment < SSH_CHATTER_BBS_MAX_COMMENTS;
             ++comment) {
            snprintf(serialized->comments[comment].author,
                     sizeof(serialized->comments[comment].author), "%s",
                     legacy.comments[comment].author);
            snprintf(serialized->comments[comment].text,
                     sizeof(serialized->comments[comment].text), "%s",
                     legacy.comments[comment].text);
            serialized->comments[comment].created_at =
                legacy.comments[comment].created_at;
        }
    } else {
        if (remaining < sizeof(*serialized)) {
            return false;
        }
        memcpy(serialized, cursor, sizeof(*serialized));
        cursor += sizeof(*serialized);
        remaining -= sizeof(*serialized);
    }

    *cursor_ptr = cursor;
    *remaining_ptr = remaining;
    return true;
}

static void session_bbs_normalize_serialized_entry(
    bbs_state_post_entry_t *serialized)
{
    if (serialized == nullptr) {
        return;
    }

    time_t now = time(nullptr);
    if (now <= 0) {
        now = 1;
    }

    serialized->author[sizeof(serialized->author) - 1U] = '\0';
    serialized->title[sizeof(serialized->title) - 1U] = '\0';
    serialized->body[sizeof(serialized->body) - 1U] = '\0';
    for (size_t tag = 0U; tag < SSH_CHATTER_BBS_MAX_TAGS; ++tag) {
        serialized->tags[tag][sizeof(serialized->tags[tag]) - 1U] = '\0';
    }
    for (size_t comment = 0U; comment < SSH_CHATTER_BBS_MAX_COMMENTS;
         ++comment) {
        serialized->comments[comment]
            .author[sizeof(serialized->comments[comment].author) - 1U] = '\0';
        serialized->comments[comment]
            .text[sizeof(serialized->comments[comment].text) - 1U] = '\0';
        if (serialized->comments[comment].created_at <= 0) {
            serialized->comments[comment].created_at = (int64_t)now;
        }
    }

    if (serialized->created_at <= 0) {
        serialized->created_at = (int64_t)now;
    }
    if (serialized->bumped_at <= 0 ||
        serialized->bumped_at < serialized->created_at) {
        serialized->bumped_at = serialized->created_at;
    }
    if (serialized->tag_count > SSH_CHATTER_BBS_MAX_TAGS) {
        serialized->tag_count = SSH_CHATTER_BBS_MAX_TAGS;
    }
    if (serialized->comment_count > SSH_CHATTER_BBS_MAX_COMMENTS) {
        serialized->comment_count = SSH_CHATTER_BBS_MAX_COMMENTS;
    }
}

static bool session_bbs_map_state_file(host_t *host, unsigned char **mapped,
                                       size_t *mapped_len,
                                       bbs_state_header_t *header)
{
    if (host == nullptr || mapped == nullptr || mapped_len == nullptr ||
        header == nullptr || host->bbs_state_file_path[0] == '\0') {
        return false;
    }

    *mapped = nullptr;
    *mapped_len = 0U;
    memset(header, 0, sizeof(*header));

    if (!host_ensure_private_data_path(host, host->bbs_state_file_path,
                                       false)) {
        return false;
    }

    FILE *fp = fopen(host->bbs_state_file_path, "rb");
    if (fp == nullptr) {
        return false;
    }

    int fd = fileno(fp);
    if (fd < 0) {
        fclose(fp);
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        fclose(fp);
        return false;
    }

    *mapped_len = (size_t)st.st_size;
    if (*mapped_len < sizeof(*header)) {
        fclose(fp);
        *mapped_len = 0U;
        return false;
    }

    *mapped = mmap(nullptr, *mapped_len, PROT_READ, MAP_PRIVATE, fd, 0);
    fclose(fp);
    if (*mapped == MAP_FAILED) {
        *mapped = nullptr;
        *mapped_len = 0U;
        return false;
    }

    memcpy(header, *mapped, sizeof(*header));
    if (header->magic != BBS_STATE_MAGIC || header->version == 0U ||
        header->version > BBS_STATE_VERSION) {
        munmap(*mapped, *mapped_len);
        *mapped = nullptr;
        *mapped_len = 0U;
        memset(header, 0, sizeof(*header));
        return false;
    }

    return true;
}

static bool session_bbs_collect_listings_from_state(host_t *host,
                                                    bbs_listing_t *listings,
                                                    size_t *count)
{
    if (host == nullptr || listings == nullptr || count == nullptr) {
        return false;
    }

    *count = 0U;
    if (host->bbs_state_file_path[0] == '\0') {
        return false;
    }
    if (access(host->bbs_state_file_path, F_OK) != 0) {
        return true;
    }

    unsigned char *mapped = nullptr;
    size_t mapped_len = 0U;
    bbs_state_header_t header = {0};
    if (!session_bbs_map_state_file(host, &mapped, &mapped_len, &header)) {
        return false;
    }

    const unsigned char *cursor = mapped + sizeof(header);
    size_t remaining = mapped_len - sizeof(header);
    time_t now = time(nullptr);
    if (now <= 0) {
        now = 1;
    }

    for (uint32_t idx = 0U;
         idx < header.post_count && *count < SSH_CHATTER_BBS_MAX_POSTS; ++idx) {
        bbs_state_post_entry_t serialized = {0};
        if (!session_bbs_read_serialized_entry(&cursor, &remaining,
                                               header.version, &serialized)) {
            break;
        }
        session_bbs_normalize_serialized_entry(&serialized);
        if (!host_bbs_serialized_has_required_fields(&serialized) ||
            !host_bbs_serialized_is_sane(&serialized, now)) {
            continue;
        }

        bbs_listing_t *entry = &listings[*count];
        memset(entry, 0, sizeof(*entry));
        entry->id = serialized.id;
        entry->tag_count = serialized.tag_count;
        entry->created_at = (time_t)serialized.created_at;
        entry->bumped_at = (time_t)serialized.bumped_at;
        snprintf(entry->title, sizeof(entry->title), "%s", serialized.title);
        snprintf(entry->author, sizeof(entry->author), "%s",
                 serialized.author);
        for (size_t tag = 0U; tag < serialized.tag_count; ++tag) {
            snprintf(entry->tags[tag], sizeof(entry->tags[tag]), "%s",
                     serialized.tags[tag]);
        }
        *count += 1U;
    }

    munmap(mapped, mapped_len);
    return true;
}

static bool session_bbs_load_post_from_state(host_t *host, uint64_t id,
                                             bbs_post_t *post)
{
    if (host == nullptr || post == nullptr || id == 0U) {
        return false;
    }

    memset(post, 0, sizeof(*post));

    unsigned char *mapped = nullptr;
    size_t mapped_len = 0U;
    bbs_state_header_t header = {0};
    if (!session_bbs_map_state_file(host, &mapped, &mapped_len, &header)) {
        return false;
    }

    const unsigned char *cursor = mapped + sizeof(header);
    size_t remaining = mapped_len - sizeof(header);
    time_t now = time(nullptr);
    if (now <= 0) {
        now = 1;
    }

    bool found = false;
    for (uint32_t idx = 0U; idx < header.post_count; ++idx) {
        bbs_state_post_entry_t serialized = {0};
        if (!session_bbs_read_serialized_entry(&cursor, &remaining,
                                               header.version, &serialized)) {
            break;
        }
        session_bbs_normalize_serialized_entry(&serialized);
        if (!host_bbs_serialized_has_required_fields(&serialized) ||
            !host_bbs_serialized_is_sane(&serialized, now) ||
            serialized.id != id) {
            continue;
        }

        post->in_use = true;
        post->id = serialized.id;
        post->tag_count = serialized.tag_count;
        post->comment_count = serialized.comment_count;
        post->created_at = (time_t)serialized.created_at;
        post->bumped_at = (time_t)serialized.bumped_at;
        snprintf(post->author, sizeof(post->author), "%s", serialized.author);
        snprintf(post->title, sizeof(post->title), "%s", serialized.title);
        snprintf(post->body, sizeof(post->body), "%s", serialized.body);
        for (size_t tag = 0U; tag < serialized.tag_count; ++tag) {
            snprintf(post->tags[tag], sizeof(post->tags[tag]), "%s",
                     serialized.tags[tag]);
        }
        for (size_t comment = 0U; comment < serialized.comment_count;
             ++comment) {
            snprintf(post->comments[comment].author,
                     sizeof(post->comments[comment].author), "%s",
                     serialized.comments[comment].author);
            snprintf(post->comments[comment].text,
                     sizeof(post->comments[comment].text), "%s",
                     serialized.comments[comment].text);
            post->comments[comment].created_at =
                (time_t)serialized.comments[comment].created_at;
        }
        found = true;
        break;
    }

    munmap(mapped, mapped_len);
    return found;
}

// Return a post by identifier while the host lock is held.
static bbs_post_t *host_find_bbs_post_locked(host_t *host, uint64_t id)
{
    if (!host_bbs_storage_ready(host) || id == 0U) {
        return nullptr;
    }
    for (size_t idx = 0U; idx < host->bbs_post_capacity; ++idx) {
        if (!bbs_post_has_required_fields(&host->bbs_posts[idx])) {
            continue;
        }
        if (host->bbs_posts[idx].id == id) {
            return &host->bbs_posts[idx];
        }
    }
    return nullptr;
}

// Allocate a new post slot, returning nullptr if capacity has been reached.
static bbs_post_t *host_allocate_bbs_post_locked(host_t *host)
{
    if (!host_bbs_storage_ready(host)) {
        return nullptr;
    }
    for (size_t idx = 0U; idx < host->bbs_post_capacity; ++idx) {
        if (host->bbs_posts[idx].in_use) {
            continue;
        }
        bbs_post_t *post = &host->bbs_posts[idx];
        post->in_use = true;
        post->id = host->next_bbs_id++;
        post->tag_count = 0U;
        post->comment_count = 0U;
        post->created_at = time(nullptr);
        post->bumped_at = post->created_at;
        post->title[0] = '\0';
        post->body[0] = '\0';
        post->author[0] = '\0';
        for (size_t tag = 0U; tag < SSH_CHATTER_BBS_MAX_TAGS; ++tag) {
            post->tags[tag][0] = '\0';
        }
        for (size_t comment = 0U; comment < SSH_CHATTER_BBS_MAX_COMMENTS;
             ++comment) {
            post->comments[comment].author[0] = '\0';
            post->comments[comment].text[0] = '\0';
            post->comments[comment].created_at = 0;
        }
        if (host->bbs_post_count < SSH_CHATTER_BBS_MAX_POSTS) {
            host->bbs_post_count += 1U;
        }
        return post;
    }
    return nullptr;
}

static void host_reset_bbs_post(bbs_post_t *post)
{
    if (post == nullptr) {
        return;
    }

    post->in_use = false;
    post->id = 0U;
    post->author[0] = '\0';
    post->title[0] = '\0';
    post->body[0] = '\0';
    post->tag_count = 0U;
    post->created_at = 0;
    post->bumped_at = 0;
    post->comment_count = 0U;
    for (size_t tag = 0U; tag < SSH_CHATTER_BBS_MAX_TAGS; ++tag) {
        post->tags[tag][0] = '\0';
    }
    for (size_t comment = 0U; comment < SSH_CHATTER_BBS_MAX_COMMENTS;
         ++comment) {
        post->comments[comment].author[0] = '\0';
        post->comments[comment].text[0] = '\0';
        post->comments[comment].created_at = 0;
    }
}

static void host_clear_bbs_post_locked(host_t *host, bbs_post_t *post)
{
    if (!host_bbs_storage_ready(host) || post == nullptr) {
        return;
    }

    host_reset_bbs_post(post);

    size_t write_index = 0U;
    for (size_t idx = 0U; idx < host->bbs_post_capacity; ++idx) {
        if (!host->bbs_posts[idx].in_use) {
            continue;
        }

        if (write_index != idx) {
            host->bbs_posts[write_index] = host->bbs_posts[idx];
        }

        ++write_index;
    }

    for (size_t idx = write_index; idx < host->bbs_post_capacity; ++idx) {
        host_reset_bbs_post(&host->bbs_posts[idx]);
    }

    host->bbs_post_count = write_index;
}

// Render an ASCII framed view of a post, including metadata and comments.

static bool session_bbs_refresh_view(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr || !ctx->bbs_view_active ||
        ctx->bbs_view_post_id == 0U) {
        return false;
    }

    host_t *host = ctx->owner;
    if (!host_bbs_storage_ready(host)) {
        session_send_system_line(ctx, "BBS storage is unavailable.");
        return false;
    }
    ttak_mutex_lock(&host->lock);
    bbs_post_t *post = host_find_bbs_post_locked(host, ctx->bbs_view_post_id);
    bbs_post_t snapshot = {0};
    if (post != nullptr) {
        snapshot = *post;
    }
    ttak_mutex_unlock(&host->lock);

    if (post == nullptr || !snapshot.in_use) {
        ctx->bbs_view_active = false;
        ctx->bbs_view_post_id = 0U;
        ctx->bbs_view_total_lines = 0U;
        ctx->bbs_view_scroll_offset = 0U;
        session_send_system_line(ctx, "That post is no longer available.");
        return false;
    }

    session_bbs_render_post(ctx, &snapshot, nullptr, false);
    return true;
}

static bool session_bbs_scroll(session_ctx_t *ctx, int direction, size_t step)
{
    if (ctx == nullptr || ctx->owner == nullptr || !ctx->bbs_view_active ||
        direction == 0) {
        return false;
    }

    size_t window = SSH_CHATTER_BBS_VIEW_WINDOW;
    if (window == 0U) {
        window = 1U;
    }

    size_t total = ctx->bbs_view_total_lines;
    if (total <= window) {
        if (direction > 0) {
            session_send_system_line(ctx,
                                     "Already viewing the top of this post.");
        } else if (direction < 0) {
            session_send_system_line(ctx,
                                     "Already viewing the end of this post.");
        }
        return true;
    }

    size_t max_offset = total - window;
    size_t offset = ctx->bbs_view_scroll_offset;
    size_t effective_step = step;
    if (effective_step == 0U) {
        effective_step = window;
    }
    if (effective_step == 0U) {
        effective_step = 1U;
    }

    size_t new_offset = offset;
    if (direction > 0) {
        if (offset == 0U) {
            session_send_system_line(ctx,
                                     "Already viewing the top of this post.");
            return true;
        }
        if (effective_step > offset) {
            effective_step = offset;
        }
        if (effective_step == 0U) {
            effective_step = 1U;
        }
        new_offset = offset - effective_step;
    } else if (direction < 0) {
        if (offset >= max_offset) {
            session_send_system_line(ctx,
                                     "Already viewing the end of this post.");
            return true;
        }
        size_t advance = effective_step;
        if (advance > max_offset - offset) {
            advance = max_offset - offset;
        }
        if (advance == 0U) {
            advance = 1U;
        }
        new_offset = offset + advance;
    }

    if (new_offset == offset) {
        if (direction > 0) {
            session_send_system_line(ctx,
                                     "Already viewing the top of this post.");
        } else if (direction < 0) {
            session_send_system_line(ctx,
                                     "Already viewing the end of this post.");
        }
        return true;
    }

    ctx->bbs_view_scroll_offset = new_offset;
    return session_bbs_refresh_view(ctx);
}

// Show the BBS dashboard and mark the session as being in BBS mode.
static void session_bbs_show_dashboard(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    ctx->in_bbs_mode = true;
    ctx->bbs_view_active = false;
    ctx->bbs_view_post_id = 0U;
    session_bbs_prepare_canvas(ctx);
    session_render_separator(ctx, "BBS Dashboard");
    session_send_system_line(
        ctx, "Commands: list, read <id>, topic read <tag>, post <title> "
             "[tags...], comment <id>|<text>, regen <id>, delete <id>, exit");
    session_bbs_list(ctx);
}

// List posts sorted by most recent activity.
static void session_bbs_list(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    enum { SESSION_BBS_TOPIC_NAME_PREC = SSH_CHATTER_BBS_TAG_LEN - 1 };

    bool previous_override = session_translation_push_scope_override(ctx);
    bbs_listing_t listings[SSH_CHATTER_BBS_MAX_POSTS];
    size_t count = 0U;

    host_t *host = ctx->owner;
    if (!session_bbs_collect_listings_from_state(host, listings, &count)) {
        session_send_system_line(ctx, "BBS storage is unavailable.");
        session_translation_pop_scope_override(ctx, previous_override);
        return;
    }

    if (count == 0U) {
        char empty_hint[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(
            empty_hint, sizeof(empty_hint),
            "The bulletin board is empty. Use /bbs post <title> [tags...] to "
            "write something. Finish drafts with %s.",
            session_bbs_terminator(ctx));
        session_send_system_line(ctx, empty_hint);
        session_translation_pop_scope_override(ctx, previous_override);
        return;
    }

    for (size_t outer = 1U; outer < count; ++outer) {
        bbs_listing_t key = listings[outer];
        size_t position = outer;
        while (position > 0U &&
               listings[position - 1U].bumped_at < key.bumped_at) {
            listings[position] = listings[position - 1U];
            --position;
        }
        listings[position] = key;
    }

    ctx->bbs_view_active = false;
    ctx->bbs_view_post_id = 0U;

    typedef struct bbs_topic_group {
        char name[SSH_CHATTER_BBS_TAG_LEN];
        size_t indexes[SSH_CHATTER_BBS_MAX_POSTS];
        size_t count;
    } bbs_topic_group_t;

    bbs_topic_group_t topics[SSH_CHATTER_BBS_MAX_POSTS];
    size_t topic_count = 0U;
    memset(topics, 0, sizeof(topics));

    for (size_t idx = 0U; idx < count; ++idx) {
        const char *topic_name = (listings[idx].tag_count > 0U)
                                     ? listings[idx].tags[0]
                                     : SSH_CHATTER_BBS_DEFAULT_TAG;
        size_t match = topic_count;
        for (size_t topic_idx = 0U; topic_idx < topic_count; ++topic_idx) {
            if (strcasecmp(topics[topic_idx].name, topic_name) == 0) {
                match = topic_idx;
                break;
            }
        }
        if (match == topic_count) {
            if (topic_count >= SSH_CHATTER_BBS_MAX_POSTS) {
                continue;
            }
            snprintf(topics[match].name, sizeof(topics[match].name), "%s",
                     topic_name);
            topics[match].count = 0U;
            ++topic_count;
        }
        if (topics[match].count < SSH_CHATTER_BBS_MAX_POSTS) {
            topics[match].indexes[topics[match].count++] = idx;
        }
    }

    for (size_t outer = 1U; outer < topic_count; ++outer) {
        bbs_topic_group_t key = topics[outer];
        size_t position = outer;
        while (position > 0U &&
               strcasecmp(topics[position - 1U].name, key.name) > 0) {
            topics[position] = topics[position - 1U];
            --position;
        }
        topics[position] = key;
    }

    session_render_separator(ctx, "BBS Posts by Topic");
    for (size_t topic_idx = 0U; topic_idx < topic_count; ++topic_idx) {
        char section_label[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(section_label, sizeof(section_label), "Topic: %.*s",
                 SESSION_BBS_TOPIC_NAME_PREC, topics[topic_idx].name);
        session_render_separator(ctx, section_label);

        for (size_t entry_idx = 0U; entry_idx < topics[topic_idx].count;
             ++entry_idx) {
            size_t listing_index = topics[topic_idx].indexes[entry_idx];
            const bbs_listing_t *entry = &listings[listing_index];
            char created_buffer[32];
            bbs_format_time(entry->bumped_at, created_buffer,
                            sizeof(created_buffer));
            char line[SSH_CHATTER_MESSAGE_LIMIT];
            int title_preview =
                (int)strnlen(entry->title, sizeof(entry->title));
            if (title_preview > 80) {
                title_preview = 80;
            }
            if (entry->tag_count == 0U) {
                snprintf(line, sizeof(line), "#%" PRIu64 " [%s] %.*s|(no tags)",
                         entry->id, created_buffer, title_preview,
                         entry->title);
            } else {
                char tag_buffer[SSH_CHATTER_MESSAGE_LIMIT];
                size_t buffer_offset = 0U;
                tag_buffer[0] = '\0';
                for (size_t tag = 0U; tag < entry->tag_count; ++tag) {
                    size_t len = strlen(entry->tags[tag]);
                    if (buffer_offset + len + 2U >= sizeof(tag_buffer)) {
                        break;
                    }
                    if (tag > 0U) {
                        tag_buffer[buffer_offset++] = ',';
                    }
                    memcpy(tag_buffer + buffer_offset, entry->tags[tag], len);
                    buffer_offset += len;
                    tag_buffer[buffer_offset] = '\0';
                }
                int tags_preview = (int)strnlen(tag_buffer, sizeof(tag_buffer));
                if (tags_preview > 80) {
                    tags_preview = 80;
                }
                snprintf(line, sizeof(line), "#%" PRIu64 " [%s] %.*s|%.*s",
                         entry->id, created_buffer, title_preview, entry->title,
                         tags_preview, tag_buffer);
            }
            session_send_system_line(ctx, line);
        }
    }

    session_render_separator(ctx, "End");
    session_translation_pop_scope_override(ctx, previous_override);
}

static void session_bbs_list_topic(session_ctx_t *ctx, const char *topic)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    char working_topic[SSH_CHATTER_BBS_TAG_LEN];
    if (topic != nullptr) {
        snprintf(working_topic, sizeof(working_topic), "%s", topic);
    } else {
        working_topic[0] = '\0';
    }
    trim_whitespace_inplace(working_topic);

    if (working_topic[0] == '\0') {
        session_send_system_line(ctx, "Specify a topic to read.");
        return;
    }

    bool previous_override = session_translation_push_scope_override(ctx);

    bbs_listing_t listings[SSH_CHATTER_BBS_MAX_POSTS];
    size_t count = 0U;

    host_t *host = ctx->owner;
    if (!session_bbs_collect_listings_from_state(host, listings, &count)) {
        session_send_system_line(ctx, "BBS storage is unavailable.");
        session_translation_pop_scope_override(ctx, previous_override);
        return;
    }

    if (count == 0U) {
        session_send_system_line(ctx, "The bulletin board is empty.");
        session_translation_pop_scope_override(ctx, previous_override);
        return;
    }

    for (size_t outer = 1U; outer < count; ++outer) {
        bbs_listing_t key = listings[outer];
        size_t position = outer;
        while (position > 0U &&
               listings[position - 1U].bumped_at < key.bumped_at) {
            listings[position] = listings[position - 1U];
            --position;
        }
        listings[position] = key;
    }

    ctx->bbs_view_active = false;
    ctx->bbs_view_post_id = 0U;

    char section_label[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(section_label, sizeof(section_label), "BBS Topic: %s",
             working_topic);
    session_render_separator(ctx, section_label);

    bool found = false;
    for (size_t idx = 0U; idx < count; ++idx) {
        const bbs_listing_t *entry = &listings[idx];
        const char *entry_topic = (entry->tag_count > 0U)
                                      ? entry->tags[0]
                                      : SSH_CHATTER_BBS_DEFAULT_TAG;
        if (strcasecmp(entry_topic, working_topic) != 0) {
            continue;
        }

        char created_buffer[32];
        bbs_format_time(entry->bumped_at, created_buffer,
                        sizeof(created_buffer));

        char line[SSH_CHATTER_MESSAGE_LIMIT];
        int title_preview = (int)strnlen(entry->title, sizeof(entry->title));
        if (title_preview > 80) {
            title_preview = 80;
        }

        if (entry->tag_count <= 1U) {
            snprintf(line, sizeof(line), "#%" PRIu64 " [%s] %.*s", entry->id,
                     created_buffer, title_preview, entry->title);
        } else {
            char tag_buffer[SSH_CHATTER_MESSAGE_LIMIT];
            size_t buffer_offset = 0U;
            tag_buffer[0] = '\0';
            for (size_t tag_idx = 0U; tag_idx < entry->tag_count; ++tag_idx) {
                const char *tag_value = entry->tags[tag_idx];
                if (tag_value[0] == '\0') {
                    continue;
                }
                size_t len = strlen(tag_value);
                if (buffer_offset + len + 2U >= sizeof(tag_buffer)) {
                    break;
                }
                if (buffer_offset > 0U) {
                    tag_buffer[buffer_offset++] = ',';
                }
                memcpy(tag_buffer + buffer_offset, tag_value, len);
                buffer_offset += len;
                tag_buffer[buffer_offset] = '\0';
            }
            int tags_preview = (int)strnlen(tag_buffer, sizeof(tag_buffer));
            if (tags_preview > 80) {
                tags_preview = 80;
            }
            snprintf(line, sizeof(line), "#%" PRIu64 " [%s] %.*s|%.*s",
                     entry->id, created_buffer, title_preview, entry->title,
                     tags_preview, tag_buffer);
        }

        session_send_system_line(ctx, line);
        found = true;
    }

    if (!found) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "No posts found for topic '%s'.",
                 working_topic);
        session_send_system_line(ctx, message);
    }

    session_render_separator(ctx, "End");
    session_translation_pop_scope_override(ctx, previous_override);
}

// Display a single post to the user.
static void session_bbs_read(session_ctx_t *ctx, uint64_t id)
{
    if (ctx == nullptr || ctx->owner == nullptr || id == 0U) {
        return;
    }

    host_t *host = ctx->owner;
    bbs_post_t *snapshot = (bbs_post_t *)sshc_gc_calloc(1U, sizeof(*snapshot));
    if (snapshot == nullptr) {
        session_send_system_line(ctx, "BBS storage is unavailable.");
        return;
    }

    if (!session_bbs_load_post_from_state(host, id, snapshot)) {
        sshc_gc_free(snapshot);
        snapshot = nullptr;
        session_send_system_line(ctx, "No post exists with that identifier.");
        return;
    }

    if (!snapshot->in_use) {
        sshc_gc_free(snapshot);
        session_send_system_line(ctx, "BBS storage is unavailable.");
        return;
    }

    session_bbs_render_post(ctx, snapshot, nullptr, true);
    sshc_gc_free(snapshot);
}

// Create a new post using the provided argument format.
static bool session_bbs_is_admin_only_tag(const char *tag)
{
    if (tag == nullptr || tag[0] == '\0') {
        return false;
    }

    if (strcasecmp(tag, "manual") == 0 || strcasecmp(tag, "notice") == 0) {
        return true;
    }

    if (strcmp(tag, "설명서") == 0 || strcmp(tag, "공지") == 0) {
        return true;
    }

    return false;
}

static void session_bbs_compact_preview(const char *input, char *output,
                                        size_t length)
{
    if (output == nullptr || length == 0U) {
        return;
    }
    output[0] = '\0';
    if (input == nullptr) {
        return;
    }

    size_t out_idx = 0U;
    bool last_space = true;
    bool truncated = false;
    const unsigned char *cursor = (const unsigned char *)input;

    while (*cursor != '\0') {
        unsigned char ch = *cursor++;
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            ch = ' ';
        }
        if (ch < 32U) {
            continue;
        }
        if (ch == ' ') {
            if (last_space) {
                continue;
            }
            last_space = true;
        } else {
            last_space = false;
        }

        if (out_idx + 1U >= length) {
            truncated = true;
            break;
        }

        output[out_idx++] = (char)ch;
    }

    if (last_space && out_idx > 0U) {
        --out_idx;
    }

    if (truncated && out_idx + 3U < length) {
        output[out_idx++] = '.';
        output[out_idx++] = '.';
        output[out_idx++] = '.';
    }

    output[out_idx] = '\0';
}

static void session_bbs_announce_post(host_t *host, const bbs_post_t *post)
{
    if (host == nullptr || post == nullptr) {
        return;
    }

    char author[SSH_CHATTER_USERNAME_LEN];
    snprintf(author, sizeof(author), "%s", post->author);
    trim_whitespace_inplace(author);

    char title[SSH_CHATTER_BBS_TITLE_LEN];
    snprintf(title, sizeof(title), "%s", post->title);
    trim_whitespace_inplace(title);

    char preview[128];
    session_bbs_compact_preview(post->body, preview, sizeof(preview));

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    if (preview[0] != '\0') {
        snprintf(notice, sizeof(notice), "* [bbs] #%llu %s posted \"%s\" --%s",
                 (unsigned long long)post->id,
                 author[0] != '\0' ? author : "unknown",
                 title[0] != '\0' ? title : "(untitled)", preview);
    } else {
        snprintf(notice, sizeof(notice), "* [bbs] #%llu %s posted \"%s\"",
                 (unsigned long long)post->id,
                 author[0] != '\0' ? author : "unknown",
                 title[0] != '\0' ? title : "(untitled)");
    }

    host_history_record_system(host, notice, nullptr);
}

static void session_bbs_announce_comment(host_t *host, const bbs_post_t *post,
                                         const bbs_comment_t *comment)
{
    if (host == nullptr || post == nullptr || comment == nullptr) {
        return;
    }

    char author[SSH_CHATTER_USERNAME_LEN];
    snprintf(author, sizeof(author), "%s", comment->author);
    trim_whitespace_inplace(author);

    char title[SSH_CHATTER_BBS_TITLE_LEN];
    snprintf(title, sizeof(title), "%s", post->title);
    trim_whitespace_inplace(title);

    char preview[128];
    session_bbs_compact_preview(comment->text, preview, sizeof(preview));

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    if (preview[0] != '\0') {
        snprintf(notice, sizeof(notice),
                 "* [bbs] #%llu %s commented on \"%s\": %s",
                 (unsigned long long)post->id,
                 author[0] != '\0' ? author : "unknown",
                 title[0] != '\0' ? title : "(untitled)", preview);
    } else {
        snprintf(notice, sizeof(notice), "* [bbs] #%llu %s commented on \"%s\"",
                 (unsigned long long)post->id,
                 author[0] != '\0' ? author : "unknown",
                 title[0] != '\0' ? title : "(untitled)");
    }

    host_history_record_system(host, notice, nullptr);
}

static void session_bbs_reset_pending_post(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    ctx->bbs_post_pending = false;
    ctx->editor_mode = SESSION_EDITOR_MODE_NONE;
    ctx->pending_bbs_edit_id = 0U;
    ctx->pending_bbs_body_length = 0U;
    ctx->pending_bbs_tag_count = 0U;
    ctx->pending_bbs_line_count = 0U;
    ctx->pending_bbs_cursor_line = 0U;
    ctx->pending_bbs_editing_line = false;
    ctx->bbs_editor_scroll_offset = 0U;
    ctx->bbs_editor_selection_start = 0U;
    ctx->bbs_editor_selection_start_set = false;
    ctx->bbs_editor_selection_end = 0U;
    ctx->bbs_editor_selection_end_set = false;
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
    if (ctx->bbs_editor_clipboard != nullptr) {
        ctx->bbs_editor_clipboard[0] = '\0';
    }
    ctx->bbs_editor_clipboard_length = 0U;
    ctx->bbs_editor_clipboard_lines = 0U;
    ctx->bbs_line_edit_mode = false;
    ctx->bbs_line_edit_target = 0U;
    ctx->bbs_search_active = false;
    ctx->bbs_search_restore_line = 0U;
    ctx->bbs_search_restore_editing = false;
    ctx->bbs_search_restore_scroll = 0U;
    ctx->bbs_rendering_editor = false;
}

static void session_bbs_commit_pending_post(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    if (!ctx->bbs_post_pending) {
        return;
    }

    if (ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART) {
        session_asciiart_import_from_editor(ctx);
        if (ctx->asciiart_length == 0U) {
            session_asciiart_cancel(ctx, "ASCII art draft discarded.");
            session_bbs_reset_pending_post(ctx);
            return;
        }

        session_asciiart_commit(ctx);
        session_bbs_reset_pending_post(ctx);
        return;
    }

    if (ctx->pending_bbs_body_length == 0U) {
        session_send_system_line(ctx, "Post body was empty. Draft discarded.");
        session_bbs_reset_pending_post(ctx);
        return;
    }

    if (session_security_check_text(ctx, "BBS post", ctx->pending_bbs_body,
                                    ctx->pending_bbs_body_length,
                                    false) != HOST_SECURITY_SCAN_CLEAN) {
        session_bbs_reset_pending_post(ctx);
        return;
    }

    host_t *host = ctx->owner;
    if (host == nullptr) {
        session_bbs_reset_pending_post(ctx);
        return;
    }

    ttak_mutex_lock(&host->lock);
    bbs_post_t snapshot = {0};
    if (ctx->editor_mode == SESSION_EDITOR_MODE_BBS_EDIT) {
        uint64_t edit_id = ctx->pending_bbs_edit_id;
        bbs_post_t *post = host_find_bbs_post_locked(host, edit_id);
        if (post == nullptr || !post->in_use) {
            ttak_mutex_unlock(&host->lock);
            session_send_system_line(
                ctx, "No post exists with that identifier anymore.");
            session_bbs_reset_pending_post(ctx);
            return;
        }

        bool can_edit = (strncmp(post->author, ctx->user.name,
                                 SSH_CHATTER_USERNAME_LEN) == 0) ||
                        ctx->user.is_operator || ctx->user.is_lan_operator;
        if (!can_edit) {
            ttak_mutex_unlock(&host->lock);
            session_send_system_line(
                ctx, "Only the author or an operator may edit this post.");
            session_bbs_reset_pending_post(ctx);
            return;
        }

        snprintf(post->title, sizeof(post->title), "%s",
                 ctx->pending_bbs_title);
        memcpy(post->body, ctx->pending_bbs_body, ctx->pending_bbs_body_length);
        post->body[ctx->pending_bbs_body_length] = '\0';
        host_strip_column_reset(post->title);
        host_strip_column_reset(post->body);
        post->tag_count = ctx->pending_bbs_tag_count;
        for (size_t idx = 0U; idx < post->tag_count; ++idx) {
            snprintf(post->tags[idx], sizeof(post->tags[idx]), "%s",
                     ctx->pending_bbs_tags[idx]);
            host_strip_column_reset(post->tags[idx]);
        }

        post->bumped_at = time(nullptr);
        snapshot = *post;
        host_bbs_state_save_locked(host);
        ttak_mutex_unlock(&host->lock);

        session_bbs_reset_pending_post(ctx);
        session_bbs_render_post(ctx, &snapshot, "Post updated.", false);
        return;
    }

    bbs_post_t *post = host_allocate_bbs_post_locked(host);
    if (post == nullptr) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "The bulletin board is full right now.");
        return;
    }

    snprintf(post->author, sizeof(post->author), "%s", ctx->user.name);
    snprintf(post->title, sizeof(post->title), "%s", ctx->pending_bbs_title);
    memcpy(post->body, ctx->pending_bbs_body, ctx->pending_bbs_body_length);
    post->body[ctx->pending_bbs_body_length] = '\0';
    host_strip_column_reset(post->author);
    host_strip_column_reset(post->title);
    host_strip_column_reset(post->body);
    post->tag_count = ctx->pending_bbs_tag_count;
    for (size_t idx = 0U; idx < post->tag_count; ++idx) {
        snprintf(post->tags[idx], sizeof(post->tags[idx]), "%s",
                 ctx->pending_bbs_tags[idx]);
        host_strip_column_reset(post->tags[idx]);
    }

    snapshot = *post;
    host_bbs_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);

    session_bbs_reset_pending_post(ctx);

    session_bbs_announce_post(ctx->owner, &snapshot);
    session_bbs_render_post(ctx, &snapshot, "Post created.", true);
}

static void session_bbs_begin_post(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (ctx->bbs_post_pending) {
        const char *terminator = session_editor_terminator(ctx);
        char warning[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(warning, sizeof(warning),
                 "You are already composing a post. Finish it with %s.",
                 terminator);
        session_send_system_line(ctx, warning);
        return;
    }

    ctx->bbs_view_active = false;
    ctx->bbs_view_post_id = 0U;

    if (ctx->owner == nullptr) {
        session_send_system_line(
            ctx, "The bulletin board is unavailable right now.");
        return;
    }

    session_bbs_reset_pending_post(ctx);
    if (!session_bbs_workspace_acquire(ctx)) {
        session_send_system_line(ctx, "Unable to allocate editor workspace.");
        return;
    }

    if (arguments == nullptr) {
        session_bbs_send_usage(ctx, "post", "<title>[|tags...]");
        session_send_system_line(
            ctx, "Use | to separate tags when the title has spaces.");
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);
    if (working[0] == '\0') {
        session_bbs_send_usage(ctx, "post", "<title>[|tags...]");
        session_send_system_line(
            ctx, "Use | to separate tags when the title has spaces.");
        return;
    }

    char title[SSH_CHATTER_BBS_TITLE_LEN];
    title[0] = '\0';
    char *tag_cursor = nullptr;
    char *separator = strchr(working, '|');
    if (separator != nullptr) {
        *separator = '\0';
        char *title_part = working;
        char *tags_part = separator + 1;
        trim_whitespace_inplace(title_part);
        trim_whitespace_inplace(tags_part);
        size_t title_len = strnlen(title_part, sizeof(title));
        if (title_len > 1U &&
            (title_part[0] == '\"' || title_part[0] == '\'') &&
            title_part[title_len - 1U] == title_part[0]) {
            title_part[title_len - 1U] = '\0';
            ++title_part;
            trim_whitespace_inplace(title_part);
        }
        size_t copy_len = strnlen(title_part, sizeof(title) - 1U);
        memcpy(title, title_part, copy_len);
        title[copy_len] = '\0';
        tag_cursor = tags_part;
    } else {
        char *cursor = working;
        if (*cursor == '\"' || *cursor == '\'') {
            char quote = *cursor++;
            char *closing = strchr(cursor, quote);
            if (closing == nullptr) {
                session_send_system_line(
                    ctx, "Missing closing quote for the title.");
                return;
            }
            size_t copy_len = (size_t)(closing - cursor);
            if (copy_len >= sizeof(title)) {
                copy_len = sizeof(title) - 1U;
            }
            memcpy(title, cursor, copy_len);
            title[copy_len] = '\0';
            cursor = closing + 1;
        } else {
            char *space = cursor;
            while (*space != '\0' && !isspace((unsigned char)*space)) {
                ++space;
            }
            size_t copy_len = (size_t)(space - cursor);
            if (copy_len >= sizeof(title)) {
                copy_len = sizeof(title) - 1U;
            }
            memcpy(title, cursor, copy_len);
            title[copy_len] = '\0';
            cursor = space;
        }

        trim_whitespace_inplace(cursor);
        tag_cursor = cursor;
    }

    if (title[0] == '\0') {
        session_send_system_line(ctx, "A title is required to create a post.");
        return;
    }

    size_t tag_count = 0U;
    bool discarded_tags = false;
    bool default_tag_applied = false;
    while (tag_cursor != nullptr && *tag_cursor != '\0') {
        while (isspace((unsigned char)*tag_cursor)) {
            ++tag_cursor;
        }
        if (*tag_cursor == '\0') {
            break;
        }
        char *end = tag_cursor;
        while (*end != '\0' && !isspace((unsigned char)*end)) {
            ++end;
        }
        size_t length = (size_t)(end - tag_cursor);
        if (length > 0U) {
            if (tag_count < SSH_CHATTER_BBS_MAX_TAGS) {
                if (length >= SSH_CHATTER_BBS_TAG_LEN) {
                    length = SSH_CHATTER_BBS_TAG_LEN - 1U;
                }
                char tag_value[SSH_CHATTER_BBS_TAG_LEN];
                memcpy(tag_value, tag_cursor, length);
                tag_value[length] = '\0';
                if (!ctx->user.is_operator &&
                    session_bbs_is_admin_only_tag(tag_value)) {
                    char warning[SSH_CHATTER_MESSAGE_LIMIT];
                    snprintf(warning, sizeof(warning),
                             "The '%s' tag is reserved for administrators.",
                             tag_value);
                    session_send_system_line(ctx, warning);
                    return;
                }
                snprintf(ctx->pending_bbs_tags[tag_count],
                         SSH_CHATTER_BBS_TAG_LEN, "%s",
                         tag_value);
                ++tag_count;
            } else {
                discarded_tags = true;
            }
        }
        tag_cursor = end;
    }

    if (tag_count == 0U) {
        snprintf(ctx->pending_bbs_tags[0], SSH_CHATTER_BBS_TAG_LEN,
                 "%s", SSH_CHATTER_BBS_DEFAULT_TAG);
        tag_count = 1U;
        default_tag_applied = true;
    }

    snprintf(ctx->pending_bbs_title, SSH_CHATTER_BBS_TITLE_LEN, "%s", title);
    ctx->pending_bbs_tag_count = tag_count;
    ctx->pending_bbs_body[0] = '\0';
    ctx->pending_bbs_body_length = 0U;
    ctx->bbs_post_pending = true;
    ctx->editor_mode = SESSION_EDITOR_MODE_BBS_CREATE;
    ctx->pending_bbs_edit_id = 0U;

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    notice[0] = '\0';
    if (default_tag_applied) {
        snprintf(notice, sizeof(notice),
                 "No tags provided; default tag '%s' applied.",
                 SSH_CHATTER_BBS_DEFAULT_TAG);
    }
    if (discarded_tags) {
        if (notice[0] != '\0') {
            strncat(notice, "\n", sizeof(notice) - strlen(notice) - 1U);
        }
        strncat(notice,
                "Only the first four tags were kept. Extra tags were ignored.",
                sizeof(notice) - strlen(notice) - 1U);
    }

    session_bbs_render_editor(ctx, notice[0] != '\0' ? notice : nullptr);
}

static void session_bbs_capture_body_text(session_ctx_t *ctx, const char *text)
{
    if (ctx == nullptr || !ctx->bbs_post_pending || text == nullptr) {
        return;
    }

    session_capture_multiline_text(ctx, text, session_bbs_capture_body_line,
                                   session_bbs_capture_continue);
}

static void session_bbs_capture_body_line(session_ctx_t *ctx, const char *line)
{
    if (ctx == nullptr || !ctx->bbs_post_pending) {
        return;
    }

    char trimmed[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(trimmed, sizeof(trimmed), "%s", line != nullptr ? line : "");
    trim_whitespace_inplace(trimmed);
    if (session_editor_matches_terminator(ctx, trimmed)) {
        session_bbs_commit_pending_post(ctx);
        return;
    }

    if (line == nullptr) {
        line = "";
    }

    char status[SSH_CHATTER_MESSAGE_LIMIT];
    status[0] = '\0';

    session_bbs_recalculate_line_count(ctx);
    bool editing_line =
        ctx->pending_bbs_editing_line &&
        ctx->pending_bbs_cursor_line < ctx->pending_bbs_line_count;
    bool inserting_line =
        !editing_line &&
        ctx->pending_bbs_cursor_line < ctx->pending_bbs_line_count;

    bool updated = false;
    if (editing_line) {
        updated = session_bbs_replace_line(ctx, ctx->pending_bbs_cursor_line,
                                           line, status, sizeof(status));
        ctx->bbs_line_edit_mode = false;
        if (updated) {
            /* session_bbs_replace_line advanced the cursor to line_index+1.
             * If that position is still within the post, continue editing
             * there so the user can keep typing without extra keystrokes. */
            session_bbs_recalculate_line_count(ctx);
            const size_t next = ctx->pending_bbs_cursor_line;
            if (next < ctx->pending_bbs_line_count) {
                session_bbs_set_cursor(ctx, next, true);
            }
        }
    } else if (inserting_line) {
        updated = session_bbs_insert_line(ctx, ctx->pending_bbs_cursor_line,
                                          line, status, sizeof(status));
        if (updated) {
            session_bbs_set_cursor(ctx, ctx->pending_bbs_cursor_line + 1U,
                                   false);
        }
    } else {
        updated = session_bbs_append_line(ctx, line, status, sizeof(status));
    }

    if (!updated && status[0] == '\0') {
        snprintf(status, sizeof(status),
                 "Unable to update the draft right now.");
    }
    if (updated) {
        ctx->bbs_line_edit_mode = false;
    }

    session_bbs_render_editor(ctx, status[0] != '\0' ? status : nullptr);
}

static void session_bbs_begin_edit(session_ctx_t *ctx, uint64_t id)
{
    if (ctx == nullptr || id == 0U) {
        session_send_system_line(ctx, "Invalid post identifier.");
        return;
    }

    if (ctx->bbs_post_pending) {
        const char *terminator = session_editor_terminator(ctx);
        char warning[SSH_CHATTER_MESSAGE_LIMIT];
        if (ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART) {
            snprintf(warning, sizeof(warning),
                     "You are already composing ASCII art. Finish it with %s.",
                     terminator);
        } else {
            snprintf(warning, sizeof(warning),
                     "You are already composing a post. Finish it with %s.",
                     terminator);
        }
        session_send_system_line(ctx, warning);
        return;
    }

    if (ctx->owner == nullptr) {
        session_send_system_line(
            ctx, "The bulletin board is unavailable right now.");
        return;
    }

    host_t *host = ctx->owner;
    ttak_mutex_lock(&host->lock);
    bbs_post_t *post = host_find_bbs_post_locked(host, id);
    bbs_post_t snapshot = {0};
    if (post != nullptr && post->in_use) {
        snapshot = *post;
    }
    ttak_mutex_unlock(&host->lock);

    if (post == nullptr || !snapshot.in_use) {
        session_send_system_line(ctx, "No post exists with that identifier.");
        return;
    }

    bool can_edit = (strncmp(snapshot.author, ctx->user.name,
                             SSH_CHATTER_USERNAME_LEN) == 0) ||
                    ctx->user.is_operator || ctx->user.is_lan_operator;
    if (!can_edit) {
        session_send_system_line(
            ctx, "Only the author or an operator may edit this post.");
        return;
    }

    session_bbs_reset_pending_post(ctx);
    if (!session_bbs_workspace_acquire(ctx)) {
        session_send_system_line(ctx, "Unable to allocate editor workspace.");
        return;
    }
    ctx->bbs_post_pending = true;
    ctx->editor_mode = SESSION_EDITOR_MODE_BBS_EDIT;
    ctx->pending_bbs_edit_id = id;

    snprintf(ctx->pending_bbs_title, SSH_CHATTER_BBS_TITLE_LEN, "%s",
             snapshot.title);

    size_t body_len =
        strnlen(snapshot.body, SSH_CHATTER_BBS_BODY_LEN - 1U);
    memcpy(ctx->pending_bbs_body, snapshot.body, body_len);
    ctx->pending_bbs_body[body_len] = '\0';
    ctx->pending_bbs_body_length = body_len;

    ctx->pending_bbs_tag_count = snapshot.tag_count;
    if (ctx->pending_bbs_tag_count > SSH_CHATTER_BBS_MAX_TAGS) {
        ctx->pending_bbs_tag_count = SSH_CHATTER_BBS_MAX_TAGS;
    }
    for (size_t idx = 0U; idx < ctx->pending_bbs_tag_count; ++idx) {
        snprintf(ctx->pending_bbs_tags[idx], SSH_CHATTER_BBS_TAG_LEN,
                 "%s", snapshot.tags[idx]);
    }

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice),
             "Editing post #%" PRIu64 ". Finish with %s to save changes.", id,
             session_bbs_terminator(ctx));
    session_bbs_render_editor(ctx, notice);
}

// Append a comment to a post.
static void session_bbs_add_comment(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr || arguments == nullptr) {
        session_bbs_send_usage(ctx, "comment", "<id>|<text>");
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);
    if (working[0] == '\0') {
        session_bbs_send_usage(ctx, "comment", "<id>|<text>");
        return;
    }

    char *separator = strchr(working, '|');
    if (separator == nullptr) {
        session_bbs_send_usage(ctx, "comment", "<id>|<text>");
        return;
    }
    *separator = '\0';
    char *id_text = working;
    char *comment_text = separator + 1;
    trim_whitespace_inplace(id_text);
    trim_whitespace_inplace(comment_text);

    if (id_text[0] == '\0' || comment_text[0] == '\0') {
        session_bbs_send_usage(ctx, "comment", "<id>|<text>");
        return;
    }

    uint64_t id = (uint64_t)strtoull(id_text, nullptr, 10);
    if (id == 0U) {
        session_send_system_line(ctx, "Invalid post identifier.");
        return;
    }

    size_t comment_scan_length =
        strnlen(comment_text, SSH_CHATTER_BBS_COMMENT_LEN);
    if (session_security_check_text(ctx, "BBS comment", comment_text,
                                    comment_scan_length,
                                    false) != HOST_SECURITY_SCAN_CLEAN) {
        return;
    }

    host_t *host = ctx->owner;
    ttak_mutex_lock(&host->lock);
    bbs_post_t *post = host_find_bbs_post_locked(host, id);
    if (post == nullptr || !post->in_use) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "No post exists with that identifier.");
        return;
    }
    if (post->comment_count >= SSH_CHATTER_BBS_MAX_COMMENTS) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx,
                                 "This post has reached the comment limit.");
        return;
    }

    size_t comment_index = post->comment_count;
    bbs_comment_t *comment = &post->comments[comment_index];
    post->comment_count++;
    snprintf(comment->author, sizeof(comment->author), "%s", ctx->user.name);
    size_t comment_len =
        strnlen(comment_text, SSH_CHATTER_BBS_COMMENT_LEN - 1U);
    memcpy(comment->text, comment_text, comment_len);
    comment->text[comment_len] = '\0';
    host_strip_column_reset(comment->author);
    host_strip_column_reset(comment->text);
    comment->created_at = time(nullptr);
    post->bumped_at = comment->created_at;
    bbs_post_t snapshot = *post;
    host_bbs_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);

    if (comment_index < snapshot.comment_count) {
        session_bbs_announce_comment(ctx->owner, &snapshot,
                                     &snapshot.comments[comment_index]);
    }
    session_bbs_render_post(ctx, &snapshot, "Comment added.", false);
}

static void session_bbs_delete(session_ctx_t *ctx, uint64_t id)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (id == 0U) {
        session_send_system_line(ctx, "Invalid post identifier.");
        return;
    }

    host_t *host = ctx->owner;
    ttak_mutex_lock(&host->lock);
    bbs_post_t *post = host_find_bbs_post_locked(host, id);
    if (post == nullptr || !post->in_use) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "No post exists with that identifier.");
        return;
    }

    bool can_delete = (strncmp(post->author, ctx->user.name,
                               SSH_CHATTER_USERNAME_LEN) == 0) ||
                      ctx->user.is_operator || ctx->user.is_lan_operator;
    if (!can_delete) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(
            ctx, "Only the author or an operator may delete this post.");
        return;
    }

    host_clear_bbs_post_locked(host, post);
    host_bbs_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);

    session_send_system_line(ctx, "Post deleted.");
}

// Bump a post to the top of the list by refreshing its activity time.
static void session_bbs_regen_post(session_ctx_t *ctx, uint64_t id)
{
    if (ctx == nullptr || ctx->owner == nullptr || id == 0U) {
        return;
    }

    host_t *host = ctx->owner;
    ttak_mutex_lock(&host->lock);
    bbs_post_t *post = host_find_bbs_post_locked(host, id);
    if (post == nullptr || !post->in_use) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "No post exists with that identifier.");
        return;
    }

    post->bumped_at = time(nullptr);
    bbs_post_t snapshot = *post;
    host_bbs_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);

    session_bbs_render_post(ctx, &snapshot, "Post bumped to the top.", false);
}

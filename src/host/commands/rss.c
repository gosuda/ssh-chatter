static void session_rss_clear(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    if (ctx->rss_view.items != nullptr) {
        ttak_mem_free(ctx->rss_view.items);
        ctx->rss_view.items = nullptr;
    }
    ctx->rss_view.active = false;
    ctx->rss_view.tag[0] = '\0';
    ctx->rss_view.item_count = 0U;
    ctx->rss_view.cursor = 0U;
    ctx->in_rss_mode = false;
}

static void session_rss_exit(session_ctx_t *ctx, const char *reason)
{
    if (ctx == nullptr) {
        return;
    }

    const bool was_active = ctx->in_rss_mode;
    session_rss_clear(ctx);

    if (reason != nullptr && reason[0] != '\0') {
        session_send_system_line(ctx, reason);
    } else if (was_active) {
        session_send_system_line(ctx, "RSS reader closed.");
    }

    if (was_active) {
        session_render_prompt(ctx, false);
    }
}

static void session_rss_show_current(session_ctx_t *ctx)
{
    if (ctx == nullptr || !ctx->rss_view.active ||
        ctx->rss_view.item_count == 0U) {
        return;
    }

    if (ctx->rss_view.cursor >= ctx->rss_view.item_count) {
        ctx->rss_view.cursor = ctx->rss_view.item_count - 1U;
    }

    if (ctx->rss_view.items == nullptr) {
        return;
    }

    const rss_session_item_t *item = &ctx->rss_view.items[ctx->rss_view.cursor];

    char header[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(header, sizeof(header), "Feed %s (%zu/%zu)", ctx->rss_view.tag,
             ctx->rss_view.cursor + 1U, ctx->rss_view.item_count);
    session_render_separator(ctx, header);

    char line[SSH_CHATTER_MESSAGE_LIMIT];
    if (item->title[0] != '\0') {
        snprintf(line, sizeof(line), "Title : %s", item->title);
    } else {
        snprintf(line, sizeof(line), "Title : (untitled)");
    }
    session_send_system_line(ctx, line);

    if (item->link[0] != '\0') {
        snprintf(line, sizeof(line), "Link  : %s", item->link);
    } else {
        snprintf(line, sizeof(line), "Link  : (none)");
    }
    session_send_system_line(ctx, line);

    if (item->summary[0] != '\0') {
        session_send_system_line(ctx, "Summary:");
        char working[SSH_CHATTER_RSS_SUMMARY_LEN];
        snprintf(working, sizeof(working), "%s", item->summary);
        char *saveptr = nullptr;
        char *fragment = strtok_r(working, "\r\n", &saveptr);
        while (fragment != nullptr) {
            rss_trim_whitespace(fragment);
            if (fragment[0] != '\0') {
                snprintf(line, sizeof(line), "  %s", fragment);
                session_send_system_line(ctx, line);
            }
            fragment = strtok_r(nullptr, "\r\n", &saveptr);
        }
    } else {
        session_send_system_line(ctx, "Summary: (none)");
    }
}

static void session_rss_begin(session_ctx_t *ctx, const char *tag,
                              const rss_session_item_t *items, size_t count)
{
    if (ctx == nullptr || tag == nullptr || tag[0] == '\0' ||
        items == nullptr || count == 0U) {
        return;
    }

    session_rss_clear(ctx);

    if (count > SSH_CHATTER_RSS_MAX_ITEMS) {
        count = SSH_CHATTER_RSS_MAX_ITEMS;
    }

    ctx->rss_view.items = (rss_session_item_t *)ttak_mem_alloc(
        count * sizeof(rss_session_item_t), __TTAK_UNSAFE_MEM_FOREVER__,
        ttak_get_tick_count());
    if (ctx->rss_view.items == nullptr) {
        session_send_system_line(ctx, "Unable to open RSS reader right now.");
        return;
    }
    memset(ctx->rss_view.items, 0, count * sizeof(rss_session_item_t));

    ctx->rss_view.active = true;
    ctx->rss_view.item_count = count;
    ctx->rss_view.cursor = 0U;
    snprintf(ctx->rss_view.tag, sizeof(ctx->rss_view.tag), "%s", tag);
    for (size_t idx = 0U; idx < count; ++idx) {
        ctx->rss_view.items[idx] = items[idx];
    }
    ctx->in_rss_mode = true;

    char intro[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(
        intro, sizeof(intro),
        "Browsing feed '%s'. Use Up/Down arrows to navigate. Type /exit or "
        "press Ctrl+Z to return.",
        ctx->rss_view.tag);
    session_render_separator(ctx, "RSS Reader");
    session_send_system_line(ctx, intro);
    session_rss_show_current(ctx);
}

static bool session_rss_move(session_ctx_t *ctx, int delta)
{
    if (ctx == nullptr || !ctx->rss_view.active ||
        ctx->rss_view.item_count == 0U || delta == 0) {
        return false;
    }

    size_t current = ctx->rss_view.cursor;
    size_t next = current;

    if (delta > 0) {
        if (next + 1U < ctx->rss_view.item_count) {
            next += 1U;
        }
    } else {
        if (next > 0U) {
            next -= 1U;
        }
    }

    if (next == current) {
        return false;
    }

    ctx->rss_view.cursor = next;
    session_rss_show_current(ctx);
    return true;
}

static void session_rss_list(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    bool refresh_requested = host_rss_schedule_manual_refresh(ctx->owner);
    if (refresh_requested) {
        session_send_system_line(
            ctx,
            "Refreshing RSS feeds in the background; cached results follow:");
    }

    rss_feed_t *snapshot = (rss_feed_t *)ttak_mem_alloc(
        sizeof(rss_feed_t) * SSH_CHATTER_RSS_MAX_FEEDS,
        __TTAK_UNSAFE_MEM_FOREVER__, ttak_get_tick_count());
    size_t count = 0U;

    if (snapshot != nullptr) {
        ttak_mutex_lock(&ctx->owner->lock);
        for (size_t idx = 0U; idx < SSH_CHATTER_RSS_MAX_FEEDS; ++idx) {
            if (!ctx->owner->rss_feeds[idx].in_use) {
                continue;
            }
            snapshot[count++] = ctx->owner->rss_feeds[idx];
            if (count >= SSH_CHATTER_RSS_MAX_FEEDS) {
                break;
            }
        }
        ttak_mutex_unlock(&ctx->owner->lock);
    }

    session_render_separator(ctx, "RSS Feeds");
    if (count == 0U) {
        session_send_system_line(ctx,
                                 "No RSS feeds registered. Operators can add "
                                 "one with /rss add <url> <tag>.");
        if (snapshot != nullptr) {
            ttak_mem_free(snapshot);
        }
        return;
    }

    for (size_t idx = 0U; idx < count; ++idx) {
        const rss_feed_t *entry = &snapshot[idx];
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        if (entry->last_title[0] != '\0') {
            char preview[72];
            snprintf(preview, sizeof(preview), "%.64s", entry->last_title);
            snprintf(line, sizeof(line), "[%s] %s (last: %s)", entry->tag,
                     entry->url, preview);
        } else {
            snprintf(line, sizeof(line), "[%s] %s", entry->tag, entry->url);
        }
        session_send_system_line(ctx, line);
    }

    if (snapshot != nullptr) {
        ttak_mem_free(snapshot);
    }
}

static void session_rss_read(session_ctx_t *ctx, const char *tag)
{
    if (ctx == nullptr || ctx->owner == nullptr || tag == nullptr ||
        tag[0] == '\0') {
        session_send_system_line(ctx, "Usage: /rss read <tag>");
        return;
    }

    char working[SSH_CHATTER_RSS_TAG_LEN];
    snprintf(working, sizeof(working), "%s", tag);
    rss_trim_whitespace(working);
    if (!rss_tag_is_valid(working)) {
        session_send_system_line(
            ctx, "Tags may only contain letters, numbers, '-', '_' or '.'.");
        return;
    }

    rss_feed_t *feed_snapshot = (rss_feed_t *)ttak_mem_alloc(
        sizeof(rss_feed_t), __TTAK_UNSAFE_MEM_FOREVER__,
        ttak_get_tick_count());
    rss_session_item_t *items = (rss_session_item_t *)ttak_mem_alloc(
        sizeof(rss_session_item_t) * SSH_CHATTER_RSS_MAX_ITEMS,
        __TTAK_UNSAFE_MEM_FOREVER__, ttak_get_tick_count());
    size_t item_count = 0U;

    if (feed_snapshot != nullptr && items != nullptr) {
        memset(feed_snapshot, 0, sizeof(rss_feed_t));
        memset(items, 0, sizeof(rss_session_item_t) * SSH_CHATTER_RSS_MAX_ITEMS);

        ttak_mutex_lock(&ctx->owner->lock);
        rss_feed_t *entry = host_find_rss_feed_locked(ctx->owner, working);
        if (entry != nullptr && entry->in_use) {
            *feed_snapshot = *entry;
            item_count = entry->stored_item_count;
            if (item_count > SSH_CHATTER_RSS_MAX_ITEMS) {
                item_count = SSH_CHATTER_RSS_MAX_ITEMS;
            }
            if (item_count > 0U) {
                memcpy(items, entry->stored_items,
                       item_count * sizeof(rss_session_item_t));
            }
        }
        ttak_mutex_unlock(&ctx->owner->lock);
    }

    if (feed_snapshot == nullptr || feed_snapshot->tag[0] == '\0') {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "No RSS feed found for tag '%s'.",
                 working);
        session_send_system_line(ctx, message);
        if (feed_snapshot != nullptr) {
            ttak_mem_free(feed_snapshot);
        }
        if (items != nullptr) {
            ttak_mem_free(items);
        }
        return;
    }

    if (item_count == 0U) {
        session_send_system_line(
            ctx,
            "The feed does not contain any entries for the current window yet.");
        ttak_mem_free(feed_snapshot);
        ttak_mem_free(items);
        return;
    }

    session_rss_begin(ctx, feed_snapshot->tag, items, item_count);
    ttak_mem_free(feed_snapshot);
    ttak_mem_free(items);
}

static void session_handle_rss(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    static const char *kUsage =
        "Usage: /rss <add <url> <tag>|del <tag>|rename <old> <new>|read <tag>|list>";

    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_format_usage(ctx, "/rss", kUsage, usage, sizeof(usage));

    char working[SSH_CHATTER_MAX_INPUT_LEN];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
    }
    rss_trim_whitespace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, usage);
        return;
    }

    char *saveptr = nullptr;
    char *command = strtok_r(working, " \t", &saveptr);
    if (command == nullptr) {
        session_send_system_line(ctx, usage);
        return;
    }

    if (strcasecmp(command, "list") == 0) {
        session_rss_list(ctx);
        return;
    }

    if (strcasecmp(command, "add") == 0 || strcasecmp(command, "추가") == 0) {
        if (!ctx->user.is_operator) {
            session_send_system_line(ctx, "Only operators may add RSS feeds.");
            return;
        }

        char *arg1 = strtok_r(nullptr, " \t", &saveptr);
        char *arg2 = strtok_r(nullptr, " \t", &saveptr);
        if (arg1 == nullptr || arg2 == nullptr) {
            session_send_system_line(ctx, "Usage: /rss add <url> <tag>");
            return;
        }

        rss_trim_whitespace(arg1);
        rss_trim_whitespace(arg2);
        if (arg1[0] == '\0' || arg2[0] == '\0') {
            session_send_system_line(ctx, "Usage: /rss add <url> <tag>");
            return;
        }

        char *url = arg1;
        char *tag = arg2;

        // Auto-correction: if arg2 looks like a URL and arg1 does not, swap them.
        bool arg1_is_url = (strncasecmp(arg1, "http://", 7) == 0 ||
                            strncasecmp(arg1, "https://", 8) == 0);
        bool arg2_is_url = (strncasecmp(arg2, "http://", 7) == 0 ||
                            strncasecmp(arg2, "https://", 8) == 0);

        if (arg2_is_url && !arg1_is_url) {
            url = arg2;
            tag = arg1;
        }

        char error[128];
        if (host_rss_add_feed(ctx->owner, url, tag, error, sizeof(error))) {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message),
                     "RSS feed '%s' registered as '%s'.", url, tag);
            session_send_system_line(ctx, message);
            host_rss_start_backend(ctx->owner);
        } else {
            if (error[0] == '\0') {
                snprintf(error, sizeof(error), "Failed to add RSS feed.");
            }
            session_send_system_line(ctx, error);
        }
        return;
    }

    if (strcasecmp(command, "del") == 0 || strcasecmp(command, "삭제") == 0) {
        if (!ctx->user.is_operator) {
            session_send_system_line(ctx,
                                     "Only operators may delete RSS feeds.");
            return;
        }

        char *tag = strtok_r(nullptr, " \t", &saveptr);
        if (tag == nullptr) {
            session_send_system_line(ctx, "Usage: /rss del <tag>");
            return;
        }

        rss_trim_whitespace(tag);
        if (tag[0] == '\0') {
            session_send_system_line(ctx, "Usage: /rss del <tag>");
            return;
        }

        char error[128];
        if (host_rss_remove_feed(ctx->owner, tag, error, sizeof(error))) {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message), "RSS feed '%s' deleted.", tag);
            session_send_system_line(ctx, message);
        } else {
            if (error[0] == '\0') {
                snprintf(error, sizeof(error), "Failed to delete RSS feed.");
            }
            session_send_system_line(ctx, error);
        }
        return;
    }

    if (strcasecmp(command, "rename") == 0 || strcasecmp(command, "이름변경") == 0) {
        if (!ctx->user.is_operator) {
            session_send_system_line(ctx,
                                     "Only operators may rename RSS feeds.");
            return;
        }

        char *old_tag = strtok_r(nullptr, " \t", &saveptr);
        char *new_tag = strtok_r(nullptr, " \t", &saveptr);
        if (old_tag == nullptr || new_tag == nullptr) {
            session_send_system_line(ctx,
                                     "Usage: /rss rename <old_name> <new_name>");
            return;
        }

        rss_trim_whitespace(old_tag);
        rss_trim_whitespace(new_tag);
        if (old_tag[0] == '\0' || new_tag[0] == '\0') {
            session_send_system_line(ctx,
                                     "Usage: /rss rename <old_name> <new_name>");
            return;
        }

        char error[128];
        if (host_rss_rename_feed(ctx->owner, old_tag, new_tag, error,
                                 sizeof(error))) {
            char message[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(message, sizeof(message),
                     "RSS feed '%s' renamed to '%s'.", old_tag, new_tag);
            session_send_system_line(ctx, message);
        } else {
            if (error[0] == '\0') {
                snprintf(error, sizeof(error), "Failed to rename RSS feed.");
            }
            session_send_system_line(ctx, error);
        }
        return;
    }

    if (strcasecmp(command, "read") == 0) {
        char *tag = strtok_r(nullptr, " \t", &saveptr);
        if (tag == nullptr) {
            session_send_system_line(ctx, "Usage: /rss read <tag>");
            return;
        }
        session_rss_read(ctx, tag);
        return;
    }

    session_send_system_line(ctx, usage);
}

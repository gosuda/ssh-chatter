// Eliza memory management, BBS persistence, and rendering helpers.
#include "host_internal.h"

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

static void __attribute__((unused))
host_eliza_memory_store(host_t *host, const char *prompt, const char *reply)
{
    if (host == nullptr || prompt == nullptr || reply == nullptr) {
        return;
    }

    char clean_prompt[SSH_CHATTER_MESSAGE_LIMIT];
    char clean_reply[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(clean_prompt, sizeof(clean_prompt), "%s", prompt);
    snprintf(clean_reply, sizeof(clean_reply), "%s", reply);
    trim_whitespace_inplace(clean_prompt);
    trim_whitespace_inplace(clean_reply);

    pthread_mutex_lock(&host->lock);
    if (host->eliza_memory_count >= SSH_CHATTER_ELIZA_MEMORY_MAX) {
        memmove(host->eliza_memory, host->eliza_memory + 1,
                (SSH_CHATTER_ELIZA_MEMORY_MAX - 1U) *
                    sizeof(host->eliza_memory[0]));
        host->eliza_memory_count = SSH_CHATTER_ELIZA_MEMORY_MAX - 1U;
    }

    eliza_memory_entry_t *entry =
        &host->eliza_memory[host->eliza_memory_count++];
    if (host->eliza_memory_next_id == 0U) {
        host->eliza_memory_next_id = 1U;
    }
    entry->id = host->eliza_memory_next_id;
    if (host->eliza_memory_next_id < UINT64_MAX) {
        host->eliza_memory_next_id += 1U;
    }
    entry->stored_at = time(nullptr);
    snprintf(entry->prompt, sizeof(entry->prompt), "%s", clean_prompt);
    snprintf(entry->reply, sizeof(entry->reply), "%s", clean_reply);

    host_eliza_memory_save_locked(host);
    pthread_mutex_unlock(&host->lock);
}

static size_t __attribute__((unused))
host_eliza_memory_collect_tokens(const char *prompt, char tokens[][32],
                                 size_t max_tokens)
{
    if (tokens == nullptr || max_tokens == 0U || prompt == nullptr) {
        return 0U;
    }

    size_t count = 0U;
    size_t length = strlen(prompt);
    size_t idx = 0U;
    while (idx < length && count < max_tokens) {
        while (idx < length && isspace((unsigned char)prompt[idx])) {
            ++idx;
        }
        if (idx >= length) {
            break;
        }

        size_t token_idx = 0U;
        char buffer[32];
        while (idx < length && !isspace((unsigned char)prompt[idx])) {
            unsigned char ch = (unsigned char)prompt[idx];
            if (token_idx + 1U < sizeof(buffer)) {
                buffer[token_idx++] =
                    (ch < 0x80U) ? (char)tolower(ch) : (char)ch;
            }
            ++idx;
        }
        buffer[token_idx] = '\0';

        if (token_idx == 0U) {
            continue;
        }
        if (token_idx < 3U && (unsigned char)buffer[0] < 0x80U) {
            continue;
        }

        bool duplicate = false;
        for (size_t existing = 0U; existing < count; ++existing) {
            if (strcmp(tokens[existing], buffer) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        snprintf(tokens[count], 32U, "%s", buffer);
        ++count;
    }

    return count;
}

static size_t __attribute__((unused))
host_eliza_memory_collect_context(host_t *host, const char *prompt,
                                  char *context, size_t context_length)
{
    if (context == nullptr || context_length == 0U) {
        return 0U;
    }

    context[0] = '\0';
    if (host == nullptr || prompt == nullptr) {
        return 0U;
    }

    eliza_memory_entry_t snapshot[SSH_CHATTER_ELIZA_MEMORY_MAX];
    size_t snapshot_count = 0U;

    pthread_mutex_lock(&host->lock);
    snapshot_count = host->eliza_memory_count;
    if (snapshot_count > SSH_CHATTER_ELIZA_MEMORY_MAX) {
        snapshot_count = SSH_CHATTER_ELIZA_MEMORY_MAX;
    }
    if (snapshot_count > 0U) {
        memcpy(snapshot, host->eliza_memory,
               snapshot_count * sizeof(snapshot[0]));
    }
    pthread_mutex_unlock(&host->lock);

    if (snapshot_count == 0U) {
        return 0U;
    }

    char tokens[SSH_CHATTER_ELIZA_TOKEN_LIMIT][32];
    size_t token_count = host_eliza_memory_collect_tokens(
        prompt, tokens, SSH_CHATTER_ELIZA_TOKEN_LIMIT);

    size_t best_indices[SSH_CHATTER_ELIZA_CONTEXT_LIMIT] = {0U};
    size_t best_scores[SSH_CHATTER_ELIZA_CONTEXT_LIMIT] = {0U};
    size_t best_count = 0U;

    for (size_t idx = 0U; idx < snapshot_count; ++idx) {
        const eliza_memory_entry_t *entry = &snapshot[idx];
        size_t score = 0U;

        if (token_count > 0U) {
            for (size_t token_idx = 0U; token_idx < token_count; ++token_idx) {
                if (tokens[token_idx][0] == '\0') {
                    continue;
                }
                if (string_contains_case_insensitive(entry->prompt,
                                                     tokens[token_idx]) ||
                    string_contains_case_insensitive(entry->reply,
                                                     tokens[token_idx])) {
                    ++score;
                }
            }

            if (score == 0U) {
                continue;
            }
        }

        size_t recency_bonus = snapshot_count - idx;
        if (recency_bonus > 4U) {
            recency_bonus = 4U;
        }
        score += recency_bonus;

        size_t insert_pos = best_count;
        if (best_count < SSH_CHATTER_ELIZA_CONTEXT_LIMIT) {
            ++best_count;
        } else if (score <= best_scores[SSH_CHATTER_ELIZA_CONTEXT_LIMIT - 1U]) {
            continue;
        } else {
            insert_pos = SSH_CHATTER_ELIZA_CONTEXT_LIMIT - 1U;
        }

        while (insert_pos > 0U && score > best_scores[insert_pos - 1U]) {
            if (insert_pos < SSH_CHATTER_ELIZA_CONTEXT_LIMIT) {
                best_scores[insert_pos] = best_scores[insert_pos - 1U];
                best_indices[insert_pos] = best_indices[insert_pos - 1U];
            }
            --insert_pos;
        }

        best_scores[insert_pos] = score;
        best_indices[insert_pos] = idx;
    }

    if (best_count == 0U && token_count == 0U) {
        size_t fallback = snapshot_count < SSH_CHATTER_ELIZA_CONTEXT_LIMIT
                              ? snapshot_count
                              : SSH_CHATTER_ELIZA_CONTEXT_LIMIT;
        for (size_t idx = 0U; idx < fallback; ++idx) {
            best_indices[idx] = snapshot_count - idx - 1U;
        }
        best_count = fallback;
    }

    if (best_count == 0U) {
        return 0U;
    }

    size_t offset = 0U;
    for (size_t idx = 0U; idx < best_count; ++idx) {
        const eliza_memory_entry_t *entry = &snapshot[best_indices[idx]];
        char time_buffer[32];
        time_buffer[0] = '\0';
        if (entry->stored_at != 0) {
            struct tm tm_value;
            if (localtime_r(&entry->stored_at, &tm_value) != nullptr) {
                strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M",
                         &tm_value);
            }
        }
        if (time_buffer[0] == '\0') {
            snprintf(time_buffer, sizeof(time_buffer), "-");
        }

        char block[SSH_CHATTER_MESSAGE_LIMIT * 2];
        int written =
            snprintf(block, sizeof(block), "%s- [%s] user: %s\n  eliza: %s",
                     idx == 0U ? "" : "\n", time_buffer,
                     entry->prompt[0] != '\0' ? entry->prompt : "(empty)",
                     entry->reply[0] != '\0' ? entry->reply : "(empty)");
        if (written < 0) {
            continue;
        }

        size_t block_len = (size_t)written;
        if (block_len >= sizeof(block)) {
            block_len = sizeof(block) - 1U;
            block[block_len] = '\0';
        }

        if (offset + block_len >= context_length) {
            size_t available =
                (offset < context_length) ? context_length - offset - 1U : 0U;
            if (available > 0U) {
                memcpy(context + offset, block, available);
                offset += available;
                context[offset] = '\0';
            }
            break;
        }

        memcpy(context + offset, block, block_len);
        offset += block_len;
        context[offset] = '\0';
    }

    return best_count;
}

static void __attribute__((unused))
host_eliza_history_normalize_line(char *text)
{
    if (text == nullptr) {
        return;
    }

    size_t read_index = 0U;
    size_t write_index = 0U;
    bool last_was_space = true;

    while (text[read_index] != '\0') {
        unsigned char ch = (unsigned char)text[read_index++];
        if (ch < 0x20U || ch == 0x7FU) {
            ch = ' ';
        }

        if (ch == ' ') {
            if (last_was_space) {
                continue;
            }
            text[write_index++] = ' ';
            last_was_space = true;
            continue;
        }

        text[write_index++] = (char)ch;
        last_was_space = false;
    }

    if (write_index > 0U && text[write_index - 1U] == ' ') {
        --write_index;
    }

    text[write_index] = '\0';
}

static size_t __attribute__((unused))
host_eliza_history_collect_context(host_t *host, char *context,
                                   size_t context_length)
{
    if (context == nullptr || context_length == 0U) {
        return 0U;
    }

    context[0] = '\0';
    if (host == nullptr) {
        return 0U;
    }

    size_t total = host_history_total(host);
    if (total == 0U) {
        return 0U;
    }

    size_t start_index = 0U;
    if (total > SSH_CHATTER_ELIZA_HISTORY_WINDOW) {
        start_index = total - SSH_CHATTER_ELIZA_HISTORY_WINDOW;
    }

    chat_history_entry_t snapshot[SSH_CHATTER_ELIZA_HISTORY_WINDOW];
    size_t retrieved = host_history_copy_range(
        host, start_index, snapshot, SSH_CHATTER_ELIZA_HISTORY_WINDOW);
    if (retrieved == 0U) {
        return 0U;
    }

    char messages[SSH_CHATTER_ELIZA_HISTORY_LIMIT][SSH_CHATTER_MESSAGE_LIMIT];
    char names[SSH_CHATTER_ELIZA_HISTORY_LIMIT][SSH_CHATTER_USERNAME_LEN];
    size_t collected = 0U;

    for (size_t idx = 0U;
         idx < retrieved && collected < SSH_CHATTER_ELIZA_HISTORY_LIMIT;
         ++idx) {
        size_t current = retrieved - idx - 1U;
        const chat_history_entry_t *entry = &snapshot[current];
        if (!entry->is_user_message) {
            continue;
        }

        char working[SSH_CHATTER_MESSAGE_LIMIT * 2U];
        working[0] = '\0';
        if (entry->message[0] != '\0') {
            snprintf(working, sizeof(working), "%s", entry->message);
        } else if (entry->attachment_type != CHAT_ATTACHMENT_NONE) {
            const char *label =
                chat_attachment_type_label(entry->attachment_type);
            snprintf(working, sizeof(working), "shared a %s" ANSI_RESET,
                     label != nullptr ? label : "attachment");
        }

        if (entry->attachment_caption[0] != '\0') {
            size_t existing = strnlen(working, sizeof(working));
            if (existing < sizeof(working) - 1U) {
                int appended =
                    snprintf(working + existing, sizeof(working) - existing,
                             "%s(caption: %s)", existing > 0U ? " " : "",
                             entry->attachment_caption);
                if (appended < 0) {
                    working[existing] = '\0';
                }
            }
        } else if (entry->attachment_type != CHAT_ATTACHMENT_NONE &&
                   entry->attachment_target[0] != '\0') {
            size_t existing = strnlen(working, sizeof(working));
            if (existing < sizeof(working) - 1U) {
                int appended =
                    snprintf(working + existing, sizeof(working) - existing,
                             "%s(link shared)", existing > 0U ? " " : "");
                if (appended < 0) {
                    working[existing] = '\0';
                }
            }
        }

        host_eliza_history_normalize_line(working);
        trim_whitespace_inplace(working);

        if (working[0] == '\0') {
            continue;
        }

        snprintf(messages[collected], sizeof(messages[collected]), "%s",
                 working);
        if (entry->username[0] != '\0') {
            snprintf(names[collected], sizeof(names[collected]), "%s",
                     entry->username);
        } else {
            snprintf(names[collected], sizeof(names[collected]), "%s",
                     "unknown");
        }
        ++collected;
    }

    if (collected == 0U) {
        return 0U;
    }

    size_t offset = 0U;
    for (size_t idx = 0U; idx < collected; ++idx) {
        size_t source = collected - idx - 1U;
        const char *name = names[source][0] != '\0' ? names[source] : "unknown";
        const char *message = messages[source];

        char line[SSH_CHATTER_MESSAGE_LIMIT * 2U];
        int written = snprintf(line, sizeof(line), "%s- [%s] %s",
                               offset == 0U ? "" : "\n", name, message);
        if (written < 0) {
            continue;
        }

        size_t line_length = (size_t)written;
        if (line_length >= sizeof(line)) {
            line_length = sizeof(line) - 1U;
            line[line_length] = '\0';
        }

        size_t remaining =
            (offset < context_length) ? context_length - offset : 0U;
        if (remaining <= 1U) {
            context[context_length - 1U] = '\0';
            break;
        }

        size_t max_append = remaining - 1U;
        if (line_length > max_append) {
            memcpy(context + offset, line, max_append);
            offset += max_append;
            context[offset] = '\0';
            break;
        }

        memcpy(context + offset, line, line_length);
        offset += line_length;
        context[offset] = '\0';
    }

    return collected;
}

static void __attribute__((unused))
host_eliza_prepare_preview(const char *source, char *dest, size_t dest_length)
{
    if (dest == nullptr || dest_length == 0U) {
        return;
    }

    dest[0] = '\0';
    if (source == nullptr || source[0] == '\0') {
        return;
    }

    size_t copy_length = strnlen(source, dest_length);
    bool truncated = false;
    if (copy_length >= dest_length) {
        copy_length = dest_length - 1U;
        truncated = true;
    }

    memcpy(dest, source, copy_length);
    dest[copy_length] = '\0';

    host_eliza_history_normalize_line(dest);
    trim_whitespace_inplace(dest);

    if (truncated && dest_length > 4U) {
        size_t length = strnlen(dest, dest_length);
        if (length + 3U < dest_length) {
            dest[length++] = '.';
            dest[length++] = '.';
            dest[length++] = '.';
            dest[length] = '\0';
        }
    }
}

static size_t __attribute__((unused))
host_eliza_bbs_collect_context(host_t *host, char *context,
                               size_t context_length)
{
    if (context == nullptr || context_length == 0U) {
        return 0U;
    }

    context[0] = '\0';
    if (host == nullptr) {
        return 0U;
    }

    bbs_post_t snapshot[SSH_CHATTER_BBS_MAX_POSTS];
    size_t snapshot_count = 0U;

    pthread_mutex_lock(&host->lock);
    for (size_t idx = 0U; idx < SSH_CHATTER_BBS_MAX_POSTS; ++idx) {
        if (!host->bbs_posts[idx].in_use) {
            continue;
        }

        if (snapshot_count < SSH_CHATTER_BBS_MAX_POSTS) {
            snapshot[snapshot_count++] = host->bbs_posts[idx];
        }
    }
    pthread_mutex_unlock(&host->lock);

    if (snapshot_count == 0U) {
        return 0U;
    }

    for (size_t idx = 0U; idx + 1U < snapshot_count; ++idx) {
        size_t best = idx;
        time_t best_time = snapshot[idx].bumped_at != 0
                               ? snapshot[idx].bumped_at
                               : snapshot[idx].created_at;
        for (size_t scan = idx + 1U; scan < snapshot_count; ++scan) {
            time_t candidate = snapshot[scan].bumped_at != 0
                                   ? snapshot[scan].bumped_at
                                   : snapshot[scan].created_at;
            if (candidate > best_time) {
                best = scan;
                best_time = candidate;
            }
        }
        if (best != idx) {
            bbs_post_t temp = snapshot[idx];
            snapshot[idx] = snapshot[best];
            snapshot[best] = temp;
        }
    }

    size_t limit = snapshot_count;
    if (limit > SSH_CHATTER_ELIZA_BBS_CONTEXT_LIMIT) {
        limit = SSH_CHATTER_ELIZA_BBS_CONTEXT_LIMIT;
    }

    size_t offset = 0U;
    size_t appended_count = 0U;
    for (size_t idx = 0U; idx < limit; ++idx) {
        const bbs_post_t *post = &snapshot[idx];

        char title[SSH_CHATTER_BBS_TITLE_LEN];
        snprintf(title, sizeof(title), "%s",
                 post->title[0] != '\0' ? post->title : "(untitled)");
        host_eliza_history_normalize_line(title);
        trim_whitespace_inplace(title);

        char tags_buffer[SSH_CHATTER_BBS_MAX_TAGS *
                         (SSH_CHATTER_BBS_TAG_LEN + 2U)];
        size_t tags_offset = 0U;
        tags_buffer[0] = '\0';
        for (size_t tag = 0U;
             tag < post->tag_count && tag < SSH_CHATTER_BBS_MAX_TAGS; ++tag) {
            if (post->tags[tag][0] == '\0') {
                continue;
            }
            if (tags_offset + 1U < sizeof(tags_buffer)) {
                if (tags_offset > 0U) {
                    tags_buffer[tags_offset++] = ',';
                }
                size_t remaining = sizeof(tags_buffer) - tags_offset;
                size_t tag_length = strnlen(post->tags[tag], remaining);
                if (tag_length >= remaining) {
                    tag_length = remaining - 1U;
                }
                memcpy(tags_buffer + tags_offset, post->tags[tag], tag_length);
                tags_offset += tag_length;
                tags_buffer[tags_offset] = '\0';
            }
        }

        char body_preview[SSH_CHATTER_ELIZA_BBS_PREVIEW_LEN];
        host_eliza_prepare_preview(post->body, body_preview,
                                   sizeof(body_preview));

        char comment_preview[SSH_CHATTER_ELIZA_BBS_PREVIEW_LEN];
        comment_preview[0] = '\0';
        char comment_author[SSH_CHATTER_USERNAME_LEN];
        comment_author[0] = '\0';
        if (post->comment_count > 0U) {
            const bbs_comment_t *comment =
                &post->comments[post->comment_count - 1U];
            host_eliza_prepare_preview(comment->text, comment_preview,
                                       sizeof(comment_preview));
            snprintf(comment_author, sizeof(comment_author), "%s",
                     comment->author[0] != '\0' ? comment->author
                                                : "(anonymous)");
            host_eliza_history_normalize_line(comment_author);
            trim_whitespace_inplace(comment_author);
        }

        char line[SSH_CHATTER_MESSAGE_LIMIT];
        size_t line_offset = 0U;
        int written =
            snprintf(line, sizeof(line), "%s- [#%" PRIu64 " %s] %s",
                     idx == 0U ? "" : "\n", post->id,
                     post->author[0] != '\0' ? post->author : "(unknown)",
                     title[0] != '\0' ? title : "(untitled)");
        if (written < 0) {
            continue;
        }

        line_offset = (size_t)written;
        if (line_offset >= sizeof(line)) {
            line_offset = sizeof(line) - 1U;
            line[line_offset] = '\0';
        }

        if (tags_buffer[0] != '\0' && line_offset + 1U < sizeof(line)) {
            int appended =
                snprintf(line + line_offset, sizeof(line) - line_offset,
                         " | tags: %s", tags_buffer);
            if (appended > 0) {
                size_t used = (size_t)appended;
                if (used >= sizeof(line) - line_offset) {
                    line_offset = sizeof(line) - 1U;
                    line[line_offset] = '\0';
                } else {
                    line_offset += used;
                }
            }
        }

        if (body_preview[0] != '\0' && line_offset + 1U < sizeof(line)) {
            int appended =
                snprintf(line + line_offset, sizeof(line) - line_offset,
                         " | body: %s", body_preview);
            if (appended > 0) {
                size_t used = (size_t)appended;
                if (used >= sizeof(line) - line_offset) {
                    line_offset = sizeof(line) - 1U;
                    line[line_offset] = '\0';
                } else {
                    line_offset += used;
                }
            }
        }

        if (comment_preview[0] != '\0' && line_offset + 1U < sizeof(line)) {
            const char *author_label =
                comment_author[0] != '\0' ? comment_author : "(anonymous)";
            int appended = snprintf(
                line + line_offset, sizeof(line) - line_offset,
                " | last comment by %s: %s", author_label, comment_preview);
            if (appended > 0) {
                size_t used = (size_t)appended;
                if (used >= sizeof(line) - line_offset) {
                    line_offset = sizeof(line) - 1U;
                    line[line_offset] = '\0';
                } else {
                    line_offset += used;
                }
            }
        }

        size_t remaining =
            (offset < context_length) ? context_length - offset : 0U;
        if (remaining <= 1U) {
            context[context_length - 1U] = '\0';
            break;
        }

        size_t max_copy = remaining - 1U;
        size_t copy_len = strnlen(line, sizeof(line));
        if (copy_len > max_copy) {
            memcpy(context + offset, line, max_copy);
            offset += max_copy;
            context[offset] = '\0';
            ++appended_count;
            break;
        }

        memcpy(context + offset, line, copy_len);
        offset += copy_len;
        context[offset] = '\0';
        ++appended_count;
    }

    if (context[0] == '\0') {
        return 0U;
    }

    if (appended_count == 0U) {
        return 0U;
    }

    return appended_count;
}

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
    if (host == nullptr) {
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

    uint32_t post_count = 0U;
    for (size_t idx = 0U; idx < SSH_CHATTER_BBS_MAX_POSTS; ++idx) {
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

    for (size_t idx = 0U; success && idx < SSH_CHATTER_BBS_MAX_POSTS; ++idx) {
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
    if (host == nullptr) {
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

    bbs_state_header_t header = {0};
    if (fread(&header, sizeof(header), 1U, fp) != 1U) {
        fclose(fp);
        return;
    }

    if (header.magic != BBS_STATE_MAGIC) {
        fclose(fp);
        return;
    }

    if (header.version == 0U || header.version > BBS_STATE_VERSION) {
        fclose(fp);
        return;
    }

    pthread_mutex_lock(&host->lock);

    for (size_t idx = 0U; idx < SSH_CHATTER_BBS_MAX_POSTS; ++idx) {
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

    for (uint32_t idx = 0U; idx < header.post_count; ++idx) {
        bbs_state_post_entry_t serialized = {0};
        if (header.version == 1U) {
            bbs_state_post_entry_v1_t legacy = {0};
            if (fread(&legacy, sizeof(legacy), 1U, fp) != 1U) {
                success = false;
                break;
            }

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
            if (fread(&legacy, sizeof(legacy), 1U, fp) != 1U) {
                success = false;
                break;
            }

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
            if (fread(&legacy, sizeof(legacy), 1U, fp) != 1U) {
                success = false;
                break;
            }

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
            if (fread(&serialized, sizeof(serialized), 1U, fp) != 1U) {
                success = false;
                break;
            }
        }

        if (serialized.id > max_id) {
            max_id = serialized.id;
        }

        if (idx >= SSH_CHATTER_BBS_MAX_POSTS) {
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
        for (size_t idx = 0U; idx < SSH_CHATTER_BBS_MAX_POSTS; ++idx) {
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

    pthread_mutex_unlock(&host->lock);
    fclose(fp);
}

static void host_bbs_watchdog_scan(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (!atomic_load(&host->eliza_enabled)) {
        return;
    }

    if (!atomic_load(&host->security_ai_enabled)) {
        return;
    }

    bbs_post_t *snapshot =
        GC_CALLOC(SSH_CHATTER_BBS_MAX_POSTS, sizeof(*snapshot));
    if (snapshot == nullptr) {
        humanized_log_error("bbs", "failed to allocate watchdog snapshot",
                            ENOMEM);
        return;
    }

    size_t snapshot_count = 0U;

    pthread_mutex_lock(&host->lock);
    for (size_t idx = 0U; idx < SSH_CHATTER_BBS_MAX_POSTS; ++idx) {
        if (!host->bbs_posts[idx].in_use) {
            continue;
        }

        if (snapshot_count < SSH_CHATTER_BBS_MAX_POSTS) {
            snapshot[snapshot_count++] = host->bbs_posts[idx];
        }
    }
    pthread_mutex_unlock(&host->lock);

    if (snapshot_count == 0U) {
        return;
    }

    const size_t content_capacity =
        SSH_CHATTER_BBS_BODY_LEN +
        (SSH_CHATTER_BBS_COMMENT_LEN * SSH_CHATTER_BBS_MAX_COMMENTS) + 1024U;
    char *content = (char *)GC_MALLOC(content_capacity);
    if (content == nullptr) {
        humanized_log_error("bbs", "failed to allocate watchdog buffer",
                            ENOMEM);
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

        pthread_mutex_lock(&host->lock);
        bbs_post_t *live = host_find_bbs_post_locked(host, post->id);
        if (live != nullptr) {
            host_clear_bbs_post_locked(host, live);
            host_bbs_state_save_locked(host);
        }
        pthread_mutex_unlock(&host->lock);

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
}

static void *host_bbs_watchdog_thread(void *arg)
{
    host_t *host = (host_t *)arg;
    if (host == nullptr) {
        return nullptr;
    }

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

static void session_refresh_output_encoding(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    bool use_utf16 = false;
    if (ctx->os_name[0] != '\0') {
        const os_descriptor_t *descriptor =
            session_lookup_os_descriptor(ctx->os_name);
        if (descriptor != nullptr &&
            strcasecmp(descriptor->name, "windows") == 0) {
            use_utf16 = true;
        }
    }

    const bool previous_cp437 = ctx->prefer_cp437_output;
    const bool previous_cp437_input = ctx->cp437_input_enabled;
    const bool has_client_identity =
        (ctx->terminal_type[0] != '\0') || (ctx->client_banner[0] != '\0');

    bool use_cp437 = session_detect_retro_client(ctx);

    if (!has_client_identity &&
        ctx->cp437_override == SESSION_CP437_OVERRIDE_NONE && previous_cp437) {
        use_cp437 = true;
        ctx->cp437_input_enabled = previous_cp437_input;
    }

    if (ctx->cp437_override == SESSION_CP437_OVERRIDE_FORCE_ON) {
        use_cp437 = true;
    } else if (ctx->cp437_override == SESSION_CP437_OVERRIDE_FORCE_OFF) {
        use_cp437 = false;
    }

    ctx->prefer_cp437_output = use_cp437;

    if (use_cp437 != previous_cp437) {
        const char *subject =
            ctx->user.name[0] != '\0' ? ctx->user.name : ctx->client_ip;
        if (subject == nullptr || subject[0] == '\0') {
            subject = "unknown";
        }

        if (use_cp437) {
            if (ctx->cp437_override == SESSION_CP437_OVERRIDE_FORCE_ON) {
                printf("[retro] manually enabling CP437 for %s via /retro "
                       "command\n",
                       subject);
            } else {
                const char *marker = ctx->retro_client_marker[0] != '\0'
                                         ? ctx->retro_client_marker
                                         : "retro client";
                if (ctx->telnet_identity[0] != '\0') {
                    printf("[retro] enabling CP437 output for %s via %s (%s)\n",
                           subject, marker, ctx->telnet_identity);
                } else {
                    printf("[retro] enabling CP437 output for %s via %s\n",
                           subject, marker);
                }
            }
        } else {
            if (ctx->cp437_override == SESSION_CP437_OVERRIDE_FORCE_OFF) {
                printf("[retro] manually disabling CP437 for %s via /retro "
                       "command\n",
                       subject);
            } else {
                printf("[retro] CP437 output disabled for %s\n", subject);
            }
        }

        if (use_cp437 && ctx->prelogin_banner_rendered) {
            session_render_banner_ascii(ctx);
        }
    }

    if (ctx->cp437_override == SESSION_CP437_OVERRIDE_FORCE_ON) {
        ctx->cp437_input_enabled = true;
    } else if (ctx->cp437_override == SESSION_CP437_OVERRIDE_FORCE_OFF) {
        ctx->cp437_input_enabled = false;
    }

    if (use_cp437) {
        ctx->prefer_utf16_output = false;
        return;
    }

    ctx->prefer_utf16_output = use_utf16;
}

static bool session_detect_retro_client(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }

    ctx->retro_client_marker[0] = '\0';

    typedef struct retro_marker {
        const char *marker;
        const char *label;
    } retro_marker_t;

    static const retro_marker_t kRetroMarkers[] = {
        {"ftelnet", "fTelnet"},      {"htmlterm", "HTMLTerm"},
        {"syncterm", "SyncTERM"},    {"netrunner", "NetRunner"},
        {"netfury", "NetFury"},      {"qodem", "Qodem"},
        {"mtelnet", "MTelnet"},      {"etherterm", "EtherTerm"},
        {"mysticbbs", "Mystic BBS"}, {"ansi-bbs", "ANSI-BBS"},
        {"pc-ansi", "PC-ANSI"},      {"cp-437", "CP437 terminal"},
        {"cp437", "CP437 terminal"}, {"avatar", "AVATAR terminal"},
        {"ripterm", "RIPTerm"},      {"ansiart", "ANSI art terminal"},
        {"ansi", "ANSI terminal"},   {"icyterm", "IcyTerm"},
    };

    const char *sources[] = {
        ctx->terminal_type,
        ctx->client_banner,
    };

    const char *label = nullptr;
    const char *identity_label = nullptr;
    bool detected = false;

    for (size_t source_idx = 0U;
         source_idx < sizeof(sources) / sizeof(sources[0]) && !detected;
         ++source_idx) {
        const char *candidate = sources[source_idx];
        if (candidate == nullptr || candidate[0] == '\0') {
            continue;
        }
        for (size_t marker_idx = 0U;
             marker_idx < sizeof(kRetroMarkers) / sizeof(kRetroMarkers[0]);
             ++marker_idx) {
            if (string_contains_case_insensitive(
                    candidate, kRetroMarkers[marker_idx].marker)) {
                label = kRetroMarkers[marker_idx].label;
                identity_label = label;
                detected = true;
                break;
            }
        }
    }

    if (!detected && ctx->terminal_type[0] != '\0') {
        const char *type = ctx->terminal_type;
        if (string_contains_case_insensitive(type, "syncterm")) {
            label = "SyncTERM";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(type, "ANSI-BBS")) {
            label = "ANSI-BBS terminal";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(type, "PC-ANSI")) {
            label = "PC-ANSI terminal";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(type, "CP-437") ||
                   string_contains_token_case_insensitive(type, "CP437")) {
            label = "CP437 terminal";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(type,
                                                          "IBMGRAPHICS") ||
                   string_contains_token_case_insensitive(type, "IBM-ASCII") ||
                   string_contains_token_case_insensitive(type, "IBMPC")) {
            label = "IBM PC terminal";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(type, "AVATAR")) {
            label = "AVATAR terminal";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(type, "RIPTERM")) {
            label = "RIPTerm terminal";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(type, "PETSCII") ||
                   string_contains_token_case_insensitive(type, "ATASCII")) {
            label = "8-bit art terminal";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(type, "DOS")) {
            label = "DOS ANSI terminal";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(type, "BBS")) {
            label = "BBS terminal";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(type, "ANSI")) {
            label = "ANSI terminal";
            identity_label = label;
            detected = true;
        }
    }

    if (!detected && ctx->client_banner[0] != '\0') {
        const char *banner = ctx->client_banner;
        if (string_contains_case_insensitive(banner, "syncterm")) {
            label = "SyncTERM";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(banner, "ANSI-BBS") ||
                   string_contains_token_case_insensitive(banner, "PC-ANSI")) {
            label = "ANSI-BBS banner";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(banner, "BBS")) {
            label = "BBS banner";
            identity_label = label;
            detected = true;
        } else if (string_contains_token_case_insensitive(banner, "ANSI")) {
            label = "ANSI banner";
            identity_label = label;
            detected = true;
        }
    }

    if (!detected && ctx->os_name[0] != '\0') {
        static const char *const kDosFamilies[] = {"msdos", "drdos", "pcdos",
                                                   "kdos"};
        for (size_t idx = 0U;
             idx < sizeof(kDosFamilies) / sizeof(kDosFamilies[0]); ++idx) {
            if (strcasecmp(ctx->os_name, kDosFamilies[idx]) == 0) {
                label = "DOS OS";
                identity_label = nullptr;
                detected = true;
                break;
            }
        }
    }

    if (detected) {
        const char *display =
            (label != nullptr && label[0] != '\0') ? label : "Retro terminal";
        snprintf(ctx->retro_client_marker, sizeof(ctx->retro_client_marker),
                 "%s", display);
    }

    session_format_telnet_identity(ctx, detected ? identity_label : nullptr);

    ctx->cp437_input_enabled = detected;

    return detected;
}

static void session_apply_saved_preferences(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    host_t *host = ctx->owner;
    user_preference_t snapshot = (user_preference_t){0};
    bool has_snapshot = false;

    pthread_mutex_lock(&host->lock);
    user_preference_t *pref = host_find_preference_locked(host, ctx->user.name);
    if (pref != nullptr) {
        snapshot = *pref;
        has_snapshot = true;
    }
    pthread_mutex_unlock(&host->lock);

    session_ui_language_t previous_language = ctx->ui_language;
    session_cp437_override_t previous_cp437_override = ctx->cp437_override;
    bool previous_cp437_output = ctx->prefer_cp437_output;
    bool previous_cp437_input = ctx->cp437_input_enabled;

    ctx->prefer_utf16_output = false;

    ctx->translation_caption_spacing = 0U;
    ctx->translation_enabled = false;
    ctx->output_translation_enabled = false;
    ctx->output_translation_language[0] = '\0';
    ctx->input_translation_enabled = false;
    ctx->input_translation_language[0] = '\0';
    ctx->last_detected_input_language[0] = '\0';
    ctx->breaking_alerts_enabled = false;

    if (has_snapshot) {
        if (snapshot.ui_language[0] != '\0') {
            session_ui_language_t saved_language =
                session_ui_language_from_code(snapshot.ui_language);
            if (saved_language != SESSION_UI_LANGUAGE_COUNT) {
                ctx->ui_language = saved_language;
            }
        }

        if (ctx->ui_language == SESSION_UI_LANGUAGE_COUNT) {
            ctx->ui_language = previous_language;
        }

        if (snapshot.has_user_theme) {
            const char *color_code = lookup_color_code(
                USER_COLOR_MAP,
                sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
                snapshot.user_color_name);
            const char *highlight_code = lookup_color_code(
                HIGHLIGHT_COLOR_MAP,
                sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
                snapshot.user_highlight_name);
            if (color_code != nullptr && highlight_code != nullptr) {
                ctx->user_color_code = color_code;
                ctx->user_highlight_code = highlight_code;
                ctx->user_is_bold = snapshot.user_is_bold;
                snprintf(ctx->user_color_name, sizeof(ctx->user_color_name),
                         "%s", snapshot.user_color_name);
                snprintf(ctx->user_highlight_name,
                         sizeof(ctx->user_highlight_name), "%s",
                         snapshot.user_highlight_name);
            }
        }

        if (snapshot.has_system_theme) {
            const char *fg_code = lookup_color_code(
                USER_COLOR_MAP,
                sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
                snapshot.system_fg_name);
            const char *bg_code = lookup_color_code(
                HIGHLIGHT_COLOR_MAP,
                sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
                snapshot.system_bg_name);
            if (fg_code != nullptr && bg_code != nullptr) {
                const char *highlight_code = ctx->system_highlight_code;
                if (snapshot.system_highlight_name[0] != '\0') {
                    const char *candidate =
                        lookup_color_code(HIGHLIGHT_COLOR_MAP,
                                          sizeof(HIGHLIGHT_COLOR_MAP) /
                                              sizeof(HIGHLIGHT_COLOR_MAP[0]),
                                          snapshot.system_highlight_name);
                    if (candidate != nullptr) {
                        highlight_code = candidate;
                    }
                }

                ctx->system_fg_code = fg_code;
                ctx->system_bg_code = bg_code;
                ctx->system_highlight_code = highlight_code;
                ctx->system_is_bold = snapshot.system_is_bold;
                snprintf(ctx->system_fg_name, sizeof(ctx->system_fg_name), "%s",
                         snapshot.system_fg_name);
                snprintf(ctx->system_bg_name, sizeof(ctx->system_bg_name), "%s",
                         snapshot.system_bg_name);
                if (snapshot.system_highlight_name[0] != '\0') {
                    snprintf(ctx->system_highlight_name,
                             sizeof(ctx->system_highlight_name), "%s",
                             snapshot.system_highlight_name);
                }
            }
        }

        if (snapshot.os_name[0] != '\0') {
            snprintf(ctx->os_name, sizeof(ctx->os_name), "%s",
                     snapshot.os_name);
        }
        ctx->daily_year = snapshot.daily_year;
        ctx->daily_yday = snapshot.daily_yday;
        if (snapshot.daily_function[0] != '\0') {
            snprintf(ctx->daily_function, sizeof(ctx->daily_function), "%s",
                     snapshot.daily_function);
        }
        ctx->has_birthday = snapshot.has_birthday;
        if (ctx->has_birthday) {
            snprintf(ctx->birthday, sizeof(ctx->birthday), "%s",
                     snapshot.birthday);
        } else {
            ctx->birthday[0] = '\0';
        }

        ctx->translation_caption_spacing = snapshot.translation_caption_spacing;
        if (ctx->translation_caption_spacing > 8U) {
            ctx->translation_caption_spacing = 8U;
        }

        if (snapshot.translation_master_explicit) {
            ctx->translation_enabled = snapshot.translation_master_enabled;
        }

        ctx->output_translation_enabled = snapshot.output_translation_enabled;
        snprintf(ctx->output_translation_language,
                 sizeof(ctx->output_translation_language), "%s",
                 snapshot.output_translation_language);
        ctx->input_translation_enabled = snapshot.input_translation_enabled;
        snprintf(ctx->input_translation_language,
                 sizeof(ctx->input_translation_language), "%s",
                 snapshot.input_translation_language);
        ctx->breaking_alerts_enabled = pref->breaking_alerts_enabled;
        snprintf(ctx->game.chosen_camouflage_language,
                 sizeof(ctx->game.chosen_camouflage_language), "%s",
                 pref->camouflage_language);
    }

    if (!has_snapshot || snapshot.ui_language[0] == '\0') {
        ctx->ui_language = previous_language;
    }

    ctx->cp437_override = previous_cp437_override;
    ctx->prefer_cp437_output = previous_cp437_output;
    ctx->cp437_input_enabled = previous_cp437_input;

    session_refresh_output_encoding(ctx);

    (void)session_user_data_load(ctx);
    session_force_dark_mode_foreground(ctx);
}

bool session_user_data_load(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return false;
    }

    if (ctx->user_data_loaded) {
        return true;
    }

    // Use a placeholder IP for loading if ctx->client_ip is not available or empty
    const char *ip_to_use =
        ctx->client_ip[0] != '\0' ? ctx->client_ip : nullptr;

    if (!user_data_load(ctx->owner->user_data_root, ctx->user.name, ip_to_use,
                        &ctx->user_data)) {
        // If loading fails, try to ensure it exists (create new)
        if (!user_data_ensure_exists(ctx->owner->user_data_root, ctx->user.name,
                                     ip_to_use, &ctx->user_data)) {
            return false;
        }
    }

    ctx->user_data_loaded = true;
    return true;
}

bool session_user_data_commit(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr || !ctx->user_data_loaded) {
        return false;
    }

    // Use a placeholder IP for saving if ctx->client_ip is not available or empty
    const char *ip_to_use =
        ctx->client_ip[0] != '\0' ? ctx->client_ip : nullptr;

    return user_data_save(ctx->owner->user_data_root, &ctx->user_data,
                          ip_to_use);
}

static bool session_argument_is_disable(const char *token)
{
    if (token == nullptr) {
        return false;
    }

    if (strcasecmp(token, "off") == 0 || strcasecmp(token, "none") == 0 ||
        strcasecmp(token, "disable") == 0 || strcasecmp(token, "stop") == 0) {
        return true;
    }

    return strcmp(token, "끄기") == 0 || strcmp(token, "オフ") == 0 ||
           strcmp(token, "关") == 0 || strcmp(token, "выкл") == 0;
}

static bool session_argument_is_enable(const char *token)
{
    if (token == nullptr) {
        return false;
    }

    if (strcasecmp(token, "on") == 0 || strcasecmp(token, "enable") == 0 ||
        strcasecmp(token, "start") == 0 || strcasecmp(token, "show") == 0) {
        return true;
    }

    return strcmp(token, "켜기") == 0 || strcmp(token, "オン") == 0 ||
           strcmp(token, "开") == 0 || strcmp(token, "开启") == 0 ||
           strcmp(token, "表示") == 0 || strcmp(token, "вкл") == 0;
}

static void session_language_normalize(const char *input, char *normalized,
                                       size_t length)
{
    if (normalized == nullptr || length == 0U) {
        return;
    }

    normalized[0] = '\0';
    if (input == nullptr) {
        return;
    }

    size_t out_idx = 0U;
    for (size_t idx = 0U; input[idx] != '\0'; ++idx) {
        unsigned char ch = (unsigned char)input[idx];
        if (isspace(ch)) {
            continue;
        }

        char lowered = (char)tolower(ch);
        if (lowered == '_') {
            lowered = '-';
        }

        if (out_idx + 1U >= length) {
            break;
        }

        normalized[out_idx++] = lowered;
    }

    if (out_idx < length) {
        normalized[out_idx] = '\0';
    } else {
        normalized[length - 1U] = '\0';
    }
}

static bool session_language_equals(const char *lhs, const char *rhs)
{
    if (lhs == nullptr || rhs == nullptr) {
        return false;
    }

    char normalized_lhs[SSH_CHATTER_LANG_NAME_LEN];
    char normalized_rhs[SSH_CHATTER_LANG_NAME_LEN];
    session_language_normalize(lhs, normalized_lhs, sizeof(normalized_lhs));
    session_language_normalize(rhs, normalized_rhs, sizeof(normalized_rhs));

    return strcmp(normalized_lhs, normalized_rhs) == 0;
}

typedef enum translation_job_type {
    TRANSLATION_JOB_CAPTION = 0,
    TRANSLATION_JOB_INPUT,
    TRANSLATION_JOB_PRIVATE_MESSAGE,
} translation_job_type_t;

typedef struct translation_job {
    translation_job_type_t type;
    char target_language[SSH_CHATTER_LANG_NAME_LEN];
    size_t placeholder_lines;
    struct translation_job *next;
    union {
        struct {
            char sanitized[SSH_CHATTER_TRANSLATION_WORKING_LEN];
            translation_placeholder_t
                placeholders[SSH_CHATTER_MAX_TRANSLATION_PLACEHOLDERS];
            size_t placeholder_count;
        } caption;
        struct {
            char original[SSH_CHATTER_TRANSLATION_WORKING_LEN];
        } input;
        struct {
            char original[SSH_CHATTER_TRANSLATION_WORKING_LEN];
            char target_name[SSH_CHATTER_USERNAME_LEN];
            char to_target_label[SSH_CHATTER_MESSAGE_LIMIT];
            char to_sender_label[SSH_CHATTER_MESSAGE_LIMIT];
        } pm;
    } data;
} translation_job_t;

typedef struct translation_result {
    translation_job_type_t type;
    bool success;
    size_t placeholder_lines;
    char translated[SSH_CHATTER_TRANSLATION_WORKING_LEN];
    char detected_language[SSH_CHATTER_LANG_NAME_LEN];
    char original[SSH_CHATTER_TRANSLATION_WORKING_LEN];
    char error_message[128];
    char pm_target_name[SSH_CHATTER_USERNAME_LEN];
    char pm_to_target_label[SSH_CHATTER_MESSAGE_LIMIT];
    char pm_to_sender_label[SSH_CHATTER_MESSAGE_LIMIT];
    struct translation_result *next;
} translation_result_t;

static translation_job_t *session_translation_job_alloc(void)
{
    translation_job_t *job = (translation_job_t *)GC_MALLOC(sizeof(*job));
    if (job != nullptr) {
        memset(job, 0, sizeof(*job));
    }
    return job;
}

static translation_result_t *session_translation_result_alloc(void)
{
    translation_result_t *result =
        (translation_result_t *)GC_MALLOC(sizeof(*result));
    if (result != nullptr) {
        memset(result, 0, sizeof(*result));
    }
    return result;
}

static bool session_translation_worker_ensure(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }

    if (!ctx->translation_mutex_initialized) {
        if (pthread_mutex_init(&ctx->translation_mutex, nullptr) != 0) {
            return false;
        }
        ctx->translation_mutex_initialized = true;
    }

    if (!ctx->translation_cond_initialized) {
        if (pthread_cond_init(&ctx->translation_cond, nullptr) != 0) {
            pthread_mutex_destroy(&ctx->translation_mutex);
            ctx->translation_mutex_initialized = false;
            return false;
        }
        ctx->translation_cond_initialized = true;
    }

    if (!ctx->translation_thread_started) {
        ctx->translation_thread_stop = false;
        if (pthread_create(&ctx->translation_thread, nullptr,
                           session_translation_worker, ctx) != 0) {
            pthread_cond_destroy(&ctx->translation_cond);
            ctx->translation_cond_initialized = false;
            pthread_mutex_destroy(&ctx->translation_mutex);
            ctx->translation_mutex_initialized = false;
            return false;
        }
        ctx->translation_thread_started = true;
    }

    return true;
}

static void session_translation_clear_queue(session_ctx_t *ctx)
{
    if (ctx == nullptr || !ctx->translation_mutex_initialized) {
        return;
    }

    translation_job_t *pending = nullptr;
    translation_result_t *ready = nullptr;

    pthread_mutex_lock(&ctx->translation_mutex);
    pending = ctx->translation_pending_head;
    ctx->translation_pending_head = nullptr;
    ctx->translation_pending_tail = nullptr;
    ready = ctx->translation_ready_head;
    ctx->translation_ready_head = nullptr;
    ctx->translation_ready_tail = nullptr;
    pthread_mutex_unlock(&ctx->translation_mutex);

    while (pending != nullptr) {
        translation_job_t *next = pending->next;
        pending = next;
    }

    while (ready != nullptr) {
        translation_result_t *next = ready->next;
        ready = next;
    }

    ctx->translation_placeholder_active_lines = 0U;
}

static bool session_translation_queue_caption(session_ctx_t *ctx,
                                              const char *message,
                                              size_t placeholder_lines)
{
    if (ctx == nullptr || message == nullptr) {
        return false;
    }

    char stripped[SSH_CHATTER_TRANSLATION_WORKING_LEN];
    if (translation_strip_no_translate_prefix(message, stripped,
                                              sizeof(stripped))) {
        return false;
    }

    if (!ctx->translation_enabled || !ctx->output_translation_enabled ||
        ctx->output_translation_language[0] == '\0' || message[0] == '\0') {
        return false;
    }

    if (!session_translation_worker_ensure(ctx)) {
        return false;
    }

    translation_job_t *job = session_translation_job_alloc();
    if (job == nullptr) {
        return false;
    }

    size_t placeholder_count = 0U;
    if (!translation_prepare_text(message, job->data.caption.sanitized,
                                  sizeof(job->data.caption.sanitized),
                                  job->data.caption.placeholders,
                                  &placeholder_count)) {
        return false;
    }

    if (job->data.caption.sanitized[0] == '\0') {
        return false;
    }

    job->type = TRANSLATION_JOB_CAPTION;
    job->data.caption.placeholder_count = placeholder_count;
    job->placeholder_lines = placeholder_lines;
    snprintf(job->target_language, sizeof(job->target_language), "%s",
             ctx->output_translation_language);

    pthread_mutex_lock(&ctx->translation_mutex);
    job->next = nullptr;
    if (ctx->translation_pending_tail != nullptr) {
        ctx->translation_pending_tail->next = job;
    } else {
        ctx->translation_pending_head = job;
    }
    ctx->translation_pending_tail = job;
    pthread_cond_signal(&ctx->translation_cond);
    pthread_mutex_unlock(&ctx->translation_mutex);

    return true;
}

static void session_translation_reserve_placeholders(session_ctx_t *ctx,
                                                     size_t placeholder_lines)
{
    if (ctx == nullptr || !session_transport_active(ctx) ||
        placeholder_lines == 0U) {
        return;
    }

    for (size_t idx = 0U; idx < placeholder_lines; ++idx) {
        session_write_rendered_line(ctx, "");
    }

    if (SIZE_MAX - ctx->translation_placeholder_active_lines <
        placeholder_lines) {
        ctx->translation_placeholder_active_lines = SIZE_MAX;
    } else {
        ctx->translation_placeholder_active_lines += placeholder_lines;
    }

    if (ctx->history_scroll_position == 0U) {
        session_refresh_input_line(ctx);
    }
}

static bool session_translation_push_scope_override(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }

    bool previous = ctx->translation_manual_scope_override;
    ctx->translation_manual_scope_override = true;
    return previous;
}

static void session_translation_pop_scope_override(session_ctx_t *ctx,
                                                   bool previous)
{
    if (ctx == nullptr) {
        return;
    }

    ctx->translation_manual_scope_override = previous;
}

static bool session_translation_queue_private_message(session_ctx_t *ctx,
                                                      session_ctx_t *target,
                                                      const char *message)
{
    if (ctx == nullptr || target == nullptr || message == nullptr) {
        return false;
    }

    if (!ctx->translation_enabled || !ctx->input_translation_enabled ||
        ctx->input_translation_language[0] == '\0' || message[0] == '\0') {
        return false;
    }

    if (!session_translation_worker_ensure(ctx)) {
        return false;
    }

    translation_job_t *job = session_translation_job_alloc();
    if (job == nullptr) {
        return false;
    }

    job->type = TRANSLATION_JOB_PRIVATE_MESSAGE;
    job->placeholder_lines = 0U;
    snprintf(job->target_language, sizeof(job->target_language), "%s",
             ctx->input_translation_language);
    snprintf(job->data.pm.original, sizeof(job->data.pm.original), "%s",
             message);
    snprintf(job->data.pm.target_name, sizeof(job->data.pm.target_name), "%s",
             target->user.name);
    snprintf(job->data.pm.to_target_label, sizeof(job->data.pm.to_target_label),
             "%s -> you", ctx->user.name);
    snprintf(job->data.pm.to_sender_label, sizeof(job->data.pm.to_sender_label),
             "you -> %s", target->user.name);

    pthread_mutex_lock(&ctx->translation_mutex);
    job->next = nullptr;
    if (ctx->translation_pending_tail != nullptr) {
        ctx->translation_pending_tail->next = job;
    } else {
        ctx->translation_pending_head = job;
    }
    ctx->translation_pending_tail = job;
    pthread_cond_signal(&ctx->translation_cond);
    pthread_mutex_unlock(&ctx->translation_mutex);

    return true;
}

static bool session_translation_queue_input(session_ctx_t *ctx,
                                            const char *text)
{
    if (ctx == nullptr || text == nullptr || text[0] == '\0') {
        return false;
    }

    if (!ctx->translation_enabled || !ctx->input_translation_enabled ||
        ctx->input_translation_language[0] == '\0') {
        return false;
    }

    if (!session_translation_worker_ensure(ctx)) {
        return false;
    }

    translation_job_t *job = session_translation_job_alloc();
    if (job == nullptr) {
        return false;
    }

    job->type = TRANSLATION_JOB_INPUT;
    job->placeholder_lines = 0U;
    snprintf(job->target_language, sizeof(job->target_language), "%s",
             ctx->input_translation_language);
    snprintf(job->data.input.original, sizeof(job->data.input.original), "%s",
             text);

    pthread_mutex_lock(&ctx->translation_mutex);
    job->next = nullptr;
    if (ctx->translation_pending_tail != nullptr) {
        ctx->translation_pending_tail->next = job;
    } else {
        ctx->translation_pending_head = job;
    }
    ctx->translation_pending_tail = job;
    pthread_cond_signal(&ctx->translation_cond);
    pthread_mutex_unlock(&ctx->translation_mutex);

    return true;
}

static void session_translation_normalize_output(char *text)
{
    if (text == nullptr) {
        return;
    }

    size_t length = strlen(text);
    size_t idx = 0U;
    while (idx < length) {
        char ch = text[idx];
        if ((ch == 'u' || ch == 'U') && idx + 4U < length &&
            text[idx + 1U] == '0' && text[idx + 2U] == '0' &&
            text[idx + 3U] == '3' &&
            (text[idx + 4U] == 'c' || text[idx + 4U] == 'C' ||
             text[idx + 4U] == 'e' || text[idx + 4U] == 'E')) {
            char replacement =
                (text[idx + 4U] == 'c' || text[idx + 4U] == 'C') ? '<' : '>';
            size_t remove_start = idx;
            if (remove_start > 0U && text[remove_start - 1U] == '\\') {
                --remove_start;
            }

            size_t remove_end = idx + 5U;
            size_t removed = remove_end - remove_start;
            text[remove_start] = replacement;
            memmove(text + remove_start + 1U, text + remove_end,
                    length - remove_end + 1U);
            length -= (removed - 1U);
            idx = remove_start + 1U;
            continue;
        }

        ++idx;
    }
}

static bool host_motd_contains_translation_notice(const char *motd_text)
{
    if (motd_text == nullptr) {
        return false;
    }

    const size_t notice_length = strlen(kTranslationQuotaNotice);
    const char *cursor = motd_text;
    while (*cursor != '\0') {
        size_t skip = host_column_reset_sequence_length(cursor);
        if (skip > 0U) {
            cursor += skip;
            continue;
        }
        if (*cursor == '\r' || *cursor == '\n') {
            ++cursor;
            continue;
        }
        if (strncmp(cursor, kTranslationQuotaNotice, notice_length) == 0) {
            return true;
        }
        while (*cursor != '\0' && *cursor != '\n') {
            ++cursor;
        }
    }

    return false;
}

static void host_prepend_translation_notice_in_memory(host_t *host,
                                                      const char *existing_motd)
{
    if (host == nullptr) {
        return;
    }

    char updated[sizeof(host->motd)];
    if (existing_motd != nullptr && existing_motd[0] != '\0') {
        snprintf(updated, sizeof(updated), "%s\n\n%s", kTranslationQuotaNotice,
                 existing_motd);
    } else {
        snprintf(updated, sizeof(updated), "%s\n", kTranslationQuotaNotice);
    }

    pthread_mutex_lock(&host->lock);
    snprintf(host->motd_base, sizeof(host->motd_base), "%s", updated);
    host_refresh_motd_locked(host);
    pthread_mutex_unlock(&host->lock);
}

static void host_handle_translation_quota_exhausted(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    bool already_marked = false;
    char motd_path[PATH_MAX];
    motd_path[0] = '\0';
    char motd_snapshot[sizeof(host->motd_base)];
    motd_snapshot[0] = '\0';

    pthread_mutex_lock(&host->lock);
    if (host->translation_quota_exhausted) {
        already_marked = true;
    } else {
        host->translation_quota_exhausted = true;
        if (host->motd_has_file && host->motd_path[0] != '\0') {
            snprintf(motd_path, sizeof(motd_path), "%s", host->motd_path);
        }
        snprintf(motd_snapshot, sizeof(motd_snapshot), "%s", host->motd_base);
    }
    pthread_mutex_unlock(&host->lock);

    if (already_marked) {
        return;
    }

    if (motd_path[0] == '\0') {
        if (host_motd_contains_translation_notice(motd_snapshot)) {
            host_refresh_motd(host);
            return;
        }
        host_prepend_translation_notice_in_memory(host, motd_snapshot);
        return;
    }

    char existing[8192];
    existing[0] = '\0';
    size_t existing_len = 0U;

    FILE *motd_file = fopen(motd_path, "rb");
    if (motd_file != nullptr) {
        existing_len = fread(existing, 1U, sizeof(existing) - 1U, motd_file);
        if (ferror(motd_file)) {
            const int read_error = errno;
            humanized_log_error("host", "failed to read motd file", read_error);
            existing_len = 0U;
            existing[0] = '\0';
        }
        existing[existing_len] = '\0';
        if (fclose(motd_file) != 0) {
            const int close_error = errno;
            humanized_log_error("host", "failed to close motd file",
                                close_error);
        }
    } else {
        host_prepend_translation_notice_in_memory(host, motd_snapshot);
        return;
    }

    const char *existing_start = existing;
    while (*existing_start == '\n' || *existing_start == '\r') {
        ++existing_start;
    }

    if (strncmp(existing_start, kTranslationQuotaNotice,
                strlen(kTranslationQuotaNotice)) == 0) {
        (void)host_try_load_motd_from_path(host, motd_path);
        return;
    }

    FILE *out = fopen(motd_path, "wb");
    if (out == nullptr) {
        const int write_error = errno != 0 ? errno : EIO;
        humanized_log_error("host", "failed to update motd file", write_error);
        host_prepend_translation_notice_in_memory(host, motd_snapshot);
        return;
    }

    (void)fprintf(out, "%s\n", kTranslationQuotaNotice);
    if (existing[0] != '\0') {
        fputc('\n', out);
        (void)fwrite(existing, 1U, existing_len, out);
    }

    if (fclose(out) != 0) {
        const int close_error = errno;
        humanized_log_error("host", "failed to close motd file", close_error);
    }

    (void)host_try_load_motd_from_path(host, motd_path);
}

static void session_handle_translation_quota_exhausted(session_ctx_t *ctx,
                                                       const char *error_detail)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    host_handle_translation_quota_exhausted(ctx->owner);

    const bool was_enabled = ctx->translation_enabled ||
                             ctx->output_translation_enabled ||
                             ctx->input_translation_enabled;
    ctx->translation_enabled = false;
    ctx->output_translation_enabled = false;
    ctx->input_translation_enabled = false;

    if (was_enabled) {
        host_store_translation_preferences(ctx->owner, ctx);
    }

    if (!ctx->translation_quota_notified) {
        char message[256];
        if (error_detail != nullptr && error_detail[0] != '\0') {
            (void)snprintf(message, sizeof(message), "%s (%s)",
                           kTranslationQuotaSystemMessage, error_detail);
        } else {
            (void)snprintf(message, sizeof(message), "%s",
                           kTranslationQuotaSystemMessage);
        }
        session_send_system_line(ctx, message);
        ctx->translation_quota_notified = true;
    }
}

static void session_translation_flush_ready(session_ctx_t *ctx)
{
    if (ctx == nullptr || !ctx->translation_mutex_initialized) {
        return;
    }

    translation_result_t *ready = nullptr;

    pthread_mutex_lock(&ctx->translation_mutex);
    ready = ctx->translation_ready_head;
    ctx->translation_ready_head = nullptr;
    ctx->translation_ready_tail = nullptr;
    pthread_mutex_unlock(&ctx->translation_mutex);

    if (ready == nullptr) {
        return;
    }

    const bool translation_active = ctx->translation_enabled &&
                                    ctx->output_translation_enabled &&
                                    ctx->output_translation_language[0] != '\0';

    bool refreshed = false;
    while (ready != nullptr) {
        translation_result_t *next = ready->next;
        if (ready->type == TRANSLATION_JOB_INPUT) {
            if (ready->success) {
                if (ready->detected_language[0] != '\0') {
                    snprintf(ctx->last_detected_input_language,
                             sizeof(ctx->last_detected_input_language), "%s",
                             ready->detected_language);
                }
                session_deliver_outgoing_message(ctx, ready->translated, false);
            } else {
                const char *error_message =
                    ready->error_message[0] != '\0'
                        ? ready->error_message
                        : "Translation failed; sending your original message.";
                session_send_system_line(ctx, error_message);
                session_deliver_outgoing_message(ctx, ready->original, false);
            }
            refreshed = true;
            ready = next;
            continue;
        }

        if (ready->type == TRANSLATION_JOB_PRIVATE_MESSAGE) {
            session_ctx_t *target = nullptr;
            if (ctx->owner != nullptr && ready->pm_target_name[0] != '\0') {
                target = chat_room_find_user(&ctx->owner->room,
                                             ready->pm_target_name);
            }

            if (ready->success) {
                if (target != nullptr) {
                    session_send_private_message_line(target, ctx,
                                                      ready->pm_to_target_label,
                                                      ready->translated);
                } else if (ready->pm_target_name[0] != '\0') {
                    char notice[SSH_CHATTER_MESSAGE_LIMIT];
                    snprintf(notice, sizeof(notice),
                             "User '%s' disconnected before your private "
                             "message was "
                             "delivered.",
                             ready->pm_target_name);
                    session_send_system_line(ctx, notice);
                }
                session_send_private_message_line(
                    ctx, ctx, ready->pm_to_sender_label, ready->translated);
            } else {
                const char *error_message =
                    ready->error_message[0] != '\0'
                        ? ready->error_message
                        : "Translation failed; sending your original message.";
                session_send_system_line(ctx, error_message);
                if (target != nullptr) {
                    session_send_private_message_line(target, ctx,
                                                      ready->pm_to_target_label,
                                                      ready->original);
                } else if (ready->pm_target_name[0] != '\0') {
                    char notice[SSH_CHATTER_MESSAGE_LIMIT];
                    snprintf(notice, sizeof(notice),
                             "User '%s' disconnected before your private "
                             "message was "
                             "delivered.",
                             ready->pm_target_name);
                    session_send_system_line(ctx, notice);
                }
                session_send_private_message_line(
                    ctx, ctx, ready->pm_to_sender_label, ready->original);
            }

            refreshed = true;
            ready = next;
            continue;
        }

        size_t placeholder_lines = ready->placeholder_lines;
        size_t move_up = 0U;
        if (placeholder_lines > 0U &&
            ctx->translation_placeholder_active_lines >= placeholder_lines) {
            size_t remaining_after =
                ctx->translation_placeholder_active_lines - placeholder_lines;
            move_up = remaining_after + 1U;
        }

        if (translation_active) {
            const char *body = ready->translated;
            if (body[0] == '\0') {
                body = "translation unavailable.";
            }

            const char *line_cursor = body;
            size_t line_index = 0U;
            while (line_cursor != nullptr) {
                const char *line_end = strchr(line_cursor, '\n');
                size_t line_length = (line_end != nullptr)
                                         ? (size_t)(line_end - line_cursor)
                                         : strlen(line_cursor);
                if (line_length >= SSH_CHATTER_TRANSLATION_WORKING_LEN) {
                    line_length = SSH_CHATTER_TRANSLATION_WORKING_LEN - 1U;
                }

                char line_fragment[SSH_CHATTER_TRANSLATION_WORKING_LEN];
                memcpy(line_fragment, line_cursor, line_length);
                line_fragment[line_length] = '\0';

                char annotated[SSH_CHATTER_TRANSLATION_WORKING_LEN + 64U];
                snprintf(annotated, sizeof(annotated), "    \342\206\263 %s",
                         line_fragment);
                session_render_caption_with_offset(
                    ctx, annotated, line_index == 0U ? move_up : 0U);
                refreshed = true;

                if (line_end == nullptr) {
                    break;
                }

                line_cursor = line_end + 1;
                ++line_index;
            }
        }

        if (placeholder_lines > 0U) {
            if (ctx->translation_placeholder_active_lines >=
                placeholder_lines) {
                ctx->translation_placeholder_active_lines -= placeholder_lines;
            } else {
                ctx->translation_placeholder_active_lines = 0U;
            }
        }

        ready = next;
    }

    if (refreshed && ctx->history_scroll_position == 0U) {
        session_refresh_input_line(ctx);
    }
}

static void session_translation_worker_shutdown(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    if (ctx->translation_mutex_initialized) {
        pthread_mutex_lock(&ctx->translation_mutex);
        if (ctx->translation_thread_started) {
            ctx->translation_thread_stop = true;
            pthread_cond_broadcast(&ctx->translation_cond);
            pthread_mutex_unlock(&ctx->translation_mutex);
            pthread_join(ctx->translation_thread, nullptr);
            ctx->translation_thread_started = false;
        } else {
            pthread_mutex_unlock(&ctx->translation_mutex);
        }
    }

    session_translation_clear_queue(ctx);

    if (ctx->translation_cond_initialized) {
        pthread_cond_destroy(&ctx->translation_cond);
        ctx->translation_cond_initialized = false;
    }
    if (ctx->translation_mutex_initialized) {
        pthread_mutex_destroy(&ctx->translation_mutex);
        ctx->translation_mutex_initialized = false;
    }

    ctx->translation_thread_stop = false;
}

static void session_translation_publish_result(
    session_ctx_t *ctx, const translation_job_t *job, const char *payload,
    const char *detected_language, const char *error_message, bool success)
{
    if (ctx == nullptr || job == nullptr) {
        return;
    }

    translation_result_t *result = session_translation_result_alloc();
    if (result == nullptr) {
        return;
    }

    result->type = job->type;
    result->success = success;
    result->placeholder_lines = job->placeholder_lines;

    if (job->type == TRANSLATION_JOB_INPUT) {
        snprintf(result->original, sizeof(result->original), "%s",
                 job->data.input.original);
        if (payload != nullptr) {
            snprintf(result->translated, sizeof(result->translated), "%s",
                     payload);
        } else {
            result->translated[0] = '\0';
        }
        if (detected_language != nullptr) {
            snprintf(result->detected_language,
                     sizeof(result->detected_language), "%s",
                     detected_language);
        } else {
            result->detected_language[0] = '\0';
        }
        if (error_message != nullptr) {
            snprintf(result->error_message, sizeof(result->error_message), "%s",
                     error_message);
        } else {
            result->error_message[0] = '\0';
        }
        session_translation_normalize_output(result->translated);
    } else if (job->type == TRANSLATION_JOB_PRIVATE_MESSAGE) {
        snprintf(result->original, sizeof(result->original), "%s",
                 job->data.pm.original);
        if (payload != nullptr) {
            snprintf(result->translated, sizeof(result->translated), "%s",
                     payload);
        } else {
            result->translated[0] = '\0';
        }
        if (error_message != nullptr) {
            snprintf(result->error_message, sizeof(result->error_message), "%s",
                     error_message);
        } else {
            result->error_message[0] = '\0';
        }
        result->detected_language[0] = '\0';
        snprintf(result->pm_target_name, sizeof(result->pm_target_name), "%s",
                 job->data.pm.target_name);
        snprintf(result->pm_to_target_label, sizeof(result->pm_to_target_label),
                 "%s", job->data.pm.to_target_label);
        snprintf(result->pm_to_sender_label, sizeof(result->pm_to_sender_label),
                 "%s", job->data.pm.to_sender_label);
        session_translation_normalize_output(result->translated);
    } else {
        const char *message = payload;
        if (message == nullptr || message[0] == '\0') {
            if (success) {
                message = "";
            } else {
                message = "⚠️ translation unavailable.";
            }
        }

        snprintf(result->translated, sizeof(result->translated), "%s", message);
        session_translation_normalize_output(result->translated);
        result->detected_language[0] = '\0';
        result->error_message[0] = '\0';
        result->original[0] = '\0';
    }

    pthread_mutex_lock(&ctx->translation_mutex);
    result->next = nullptr;
    if (ctx->translation_ready_tail != nullptr) {
        ctx->translation_ready_tail->next = result;
    } else {
        ctx->translation_ready_head = result;
    }
    ctx->translation_ready_tail = result;
    pthread_mutex_unlock(&ctx->translation_mutex);
}

static void session_translation_process_single_job(session_ctx_t *ctx,
                                                   translation_job_t *job)
{
    if (ctx == nullptr || job == nullptr) {
        return;
    }

    if (ctx->translation_thread_stop) {
        return;
    }

    if (job->type == TRANSLATION_JOB_INPUT ||
        job->type == TRANSLATION_JOB_PRIVATE_MESSAGE) {
        char translated_body[SSH_CHATTER_TRANSLATION_WORKING_LEN];
        char detected_language[SSH_CHATTER_LANG_NAME_LEN];
        translated_body[0] = '\0';
        detected_language[0] = '\0';

        const bool is_private_message =
            (job->type == TRANSLATION_JOB_PRIVATE_MESSAGE);
        const char *source_text = is_private_message ? job->data.pm.original
                                                     : job->data.input.original;
        char *detected_target =
            is_private_message ? nullptr : detected_language;
        size_t detected_length =
            is_private_message ? 0U : sizeof(detected_language);

        if (translator_translate_with_cancel(
                source_text, job->target_language, translated_body,
                sizeof(translated_body), detected_target, detected_length,
                &ctx->translation_thread_stop)) {
            if (ctx->translation_thread_stop) {
                return;
            }
            session_translation_publish_result(
                ctx, job, translated_body,
                is_private_message ? nullptr : detected_language, nullptr,
                true);
        } else {
            const char *error = translator_last_error();
            char message[128];
            const bool quota_failure = translator_last_error_was_quota();
            if (ctx->translation_thread_stop) {
                return;
            }
            if (quota_failure) {
                if (error != nullptr && error[0] != '\0') {
                    snprintf(message, sizeof(message),
                             "⚠️ translation unavailable (quota exhausted: %s); "
                             "sending "
                             "your original message.",
                             error);
                } else {
                    snprintf(message, sizeof(message),
                             "⚠️ translation unavailable (quota exhausted); "
                             "sending your "
                             "original message.");
                }
                session_handle_translation_quota_exhausted(ctx, error);
            } else if (error != nullptr && error[0] != '\0') {
                snprintf(
                    message, sizeof(message),
                    "Translation failed (%s); sending your original message.",
                    error);
            } else {
                snprintf(message, sizeof(message),
                         "Translation failed; sending your original message.");
            }
            if (ctx->translation_thread_stop) {
                return;
            }
            session_translation_publish_result(ctx, job, nullptr, nullptr,
                                               message, false);
        }
        return;
    }

    char translated_body[SSH_CHATTER_TRANSLATION_WORKING_LEN];
    char restored[SSH_CHATTER_TRANSLATION_WORKING_LEN];
    translated_body[0] = '\0';
    restored[0] = '\0';

    bool success = false;
    char failure_message[128];
    failure_message[0] = '\0';
    const int max_attempts = 3;
    for (int attempt = 0; attempt < max_attempts && !success; ++attempt) {
        translated_body[0] = '\0';

        if (ctx->translation_thread_stop) {
            return;
        }

        if (!translator_translate_with_cancel(
                job->data.caption.sanitized, job->target_language,
                translated_body, sizeof(translated_body), nullptr, 0U,
                &ctx->translation_thread_stop)) {
            const char *error = translator_last_error();
            const bool quota_failure = translator_last_error_was_quota();
            if (ctx->translation_thread_stop) {
                return;
            }
            if (quota_failure) {
                if (error != nullptr && error[0] != '\0') {
                    snprintf(failure_message, sizeof(failure_message),
                             "⚠️ translation unavailable (quota exhausted: %s)",
                             error);
                } else {
                    snprintf(failure_message, sizeof(failure_message),
                             "⚠️ translation unavailable (quota exhausted).");
                }
                session_handle_translation_quota_exhausted(ctx, error);
                break;
            }

            if (error != nullptr && error[0] != '\0') {
                snprintf(failure_message, sizeof(failure_message),
                         "⚠️ translation failed: %s", error);
            } else {
                snprintf(failure_message, sizeof(failure_message),
                         "⚠️ translation failed.");
            }

            if (attempt + 1 < max_attempts) {
                struct timespec retry_delay = {.tv_sec = 1, .tv_nsec = 0L};
                host_sleep_uninterruptible(&retry_delay);
            }
            continue;
        }

        if (!translation_restore_text(translated_body, restored,
                                      sizeof(restored),
                                      job->data.caption.placeholders,
                                      job->data.caption.placeholder_count)) {
            snprintf(failure_message, sizeof(failure_message),
                     "⚠️ translation post-processing failed.");
            break;
        }

        success = true;
        failure_message[0] = '\0';
    }

    if (!success && failure_message[0] == '\0') {
        snprintf(failure_message, sizeof(failure_message),
                 "⚠️ translation unavailable.");
    }

    if (ctx->translation_thread_stop) {
        return;
    }

    if (success) {
        session_translation_publish_result(ctx, job, restored, nullptr, nullptr,
                                           true);
    } else {
        session_translation_publish_result(ctx, job, failure_message, nullptr,
                                           nullptr, false);
    }
}

static bool session_translation_process_batch(session_ctx_t *ctx,
                                              translation_job_t **jobs,
                                              size_t job_count)
{
    if (ctx == nullptr || jobs == nullptr || job_count == 0U) {
        return false;
    }

    if (jobs[0] == nullptr || jobs[0]->type != TRANSLATION_JOB_CAPTION) {
        return false;
    }

    if (ctx->translation_thread_stop) {
        for (size_t idx = 0U; idx < job_count; ++idx) {
            if (jobs[idx] != nullptr) {
                jobs[idx] = nullptr;
            }
        }
        return true;
    }

    char *combined =
        GC_CALLOC(SSH_CHATTER_TRANSLATION_BATCH_BUFFER, sizeof(char));
    char *translated =
        GC_CALLOC(SSH_CHATTER_TRANSLATION_BATCH_BUFFER, sizeof(char));
    if (combined == nullptr || translated == nullptr) {
        return false;
    }

    size_t offset = 0U;
    for (size_t idx = 0U; idx < job_count; ++idx) {
        if (ctx->translation_thread_stop) {
            for (size_t release = idx; release < job_count; ++release) {
                if (jobs[release] != nullptr) {
                    jobs[release] = nullptr;
                }
            }
            return true;
        }
        if (jobs[idx] == nullptr ||
            jobs[idx]->type != TRANSLATION_JOB_CAPTION) {
            return false;
        }

        char marker[32];
        int marker_len =
            snprintf(marker, sizeof(marker), "[[SEG%02zu]]\n", idx);
        if (marker_len < 0) {
            return false;
        }

        size_t marker_size = (size_t)marker_len;
        size_t text_len = strlen(jobs[idx]->data.caption.sanitized);
        if (offset + marker_size + text_len + 1U >
            SSH_CHATTER_TRANSLATION_BATCH_BUFFER) {
            return false;
        }

        memcpy(combined + offset, marker, marker_size);
        offset += marker_size;
        memcpy(combined + offset, jobs[idx]->data.caption.sanitized, text_len);
        offset += text_len;
        combined[offset++] = '\n';
    }
    combined[offset] = '\0';

    if (!translator_translate_with_cancel(
            combined, jobs[0]->target_language, translated,
            SSH_CHATTER_TRANSLATION_BATCH_BUFFER, nullptr, 0U,
            &ctx->translation_thread_stop)) {
        if (ctx->translation_thread_stop) {
            for (size_t idx = 0U; idx < job_count; ++idx) {
                if (jobs[idx] != nullptr) {
                    jobs[idx] = nullptr;
                }
            }
            return true;
        }
        return false;
    }

    if (ctx->translation_thread_stop) {
        for (size_t idx = 0U; idx < job_count; ++idx) {
            if (jobs[idx] != nullptr) {
                jobs[idx] = nullptr;
            }
        }
        return true;
    }

    char *segment_starts[SSH_CHATTER_TRANSLATION_BATCH_MAX] = {0};
    char *segment_ends[SSH_CHATTER_TRANSLATION_BATCH_MAX] = {0};

    char *search_cursor = translated;
    for (size_t idx = 0U; idx < job_count; ++idx) {
        char marker[32];
        int marker_len = snprintf(marker, sizeof(marker), "[[SEG%02zu]]", idx);
        if (marker_len < 0) {
            return false;
        }

        char *marker_pos = strstr(search_cursor, marker);
        if (marker_pos == nullptr) {
            return false;
        }

        char *start = marker_pos + (size_t)marker_len;
        while (*start == '\r' || *start == '\n') {
            ++start;
        }

        segment_starts[idx] = start;
        search_cursor = start;
    }

    for (size_t idx = 0U; idx + 1U < job_count; ++idx) {
        char marker[32];
        int marker_len =
            snprintf(marker, sizeof(marker), "[[SEG%02zu]]", idx + 1U);
        if (marker_len < 0) {
            return false;
        }

        char *next_pos = strstr(segment_starts[idx], marker);
        if (next_pos == nullptr) {
            return false;
        }

        char *end = next_pos;
        while (end > segment_starts[idx] &&
               (end[-1] == '\r' || end[-1] == '\n')) {
            --end;
        }
        segment_ends[idx] = end;
    }

    char *last_end = translated + strlen(translated);
    while (last_end > segment_starts[job_count - 1U] &&
           (last_end[-1] == '\r' || last_end[-1] == '\n')) {
        --last_end;
    }
    segment_ends[job_count - 1U] = last_end;

    char restored_segments[SSH_CHATTER_TRANSLATION_BATCH_MAX]
                          [SSH_CHATTER_TRANSLATION_WORKING_LEN];
    for (size_t idx = 0U; idx < job_count; ++idx) {
        if (segment_starts[idx] == nullptr || segment_ends[idx] == nullptr ||
            segment_ends[idx] < segment_starts[idx]) {
            return false;
        }

        size_t segment_len = (size_t)(segment_ends[idx] - segment_starts[idx]);
        if (segment_len + 1U > SSH_CHATTER_TRANSLATION_WORKING_LEN) {
            return false;
        }

        char segment_buffer[SSH_CHATTER_TRANSLATION_WORKING_LEN];
        memcpy(segment_buffer, segment_starts[idx], segment_len);
        segment_buffer[segment_len] = '\0';

        if (!translation_restore_text(
                segment_buffer, restored_segments[idx],
                sizeof(restored_segments[idx]),
                jobs[idx]->data.caption.placeholders,
                jobs[idx]->data.caption.placeholder_count)) {
            return false;
        }
    }

    if (ctx->translation_thread_stop) {
        for (size_t idx = 0U; idx < job_count; ++idx) {
            if (jobs[idx] != nullptr) {
                jobs[idx] = nullptr;
            }
        }
        return true;
    }

    for (size_t idx = 0U; idx < job_count; ++idx) {
        session_translation_publish_result(
            ctx, jobs[idx], restored_segments[idx], nullptr, nullptr, true);
    }

    return true;
}

static void *session_translation_worker(void *arg)
{
    session_ctx_t *ctx = (session_ctx_t *)arg;
    if (ctx == nullptr) {
        return nullptr;
    }

    sshc_memory_context_t *memory_scope = nullptr;
    if (ctx->owner != nullptr) {
        memory_scope = sshc_memory_context_push(ctx->owner->memory_context);
    }

    for (;;) {
        translation_job_t *batch[SSH_CHATTER_TRANSLATION_BATCH_MAX] = {0};
        size_t batch_count = 0U;

        pthread_mutex_lock(&ctx->translation_mutex);
        while (!ctx->translation_thread_stop &&
               ctx->translation_pending_head == nullptr) {
            pthread_cond_wait(&ctx->translation_cond, &ctx->translation_mutex);
        }

        if (ctx->translation_thread_stop) {
            pthread_mutex_unlock(&ctx->translation_mutex);
            break;
        }

        translation_job_t *job = ctx->translation_pending_head;
        if (job != nullptr) {
            ctx->translation_pending_head = job->next;
            if (ctx->translation_pending_head == nullptr) {
                ctx->translation_pending_tail = nullptr;
            }
            job->next = nullptr;
            batch[batch_count++] = job;
        }
        pthread_mutex_unlock(&ctx->translation_mutex);

        if (batch_count == 0U) {
            continue;
        }

        if (batch[0]->type != TRANSLATION_JOB_CAPTION) {
            session_translation_process_single_job(ctx, batch[0]);
            continue;
        }

        size_t estimate = strlen(batch[0]->data.caption.sanitized) +
                          SSH_CHATTER_TRANSLATION_SEGMENT_GUARD;

        if (batch_count == 1U) {
            bool delay_needed = false;
            pthread_mutex_lock(&ctx->translation_mutex);
            if (!ctx->translation_thread_stop &&
                ctx->translation_pending_head == nullptr) {
                delay_needed = true;
            }
            pthread_mutex_unlock(&ctx->translation_mutex);

            if (delay_needed) {
                struct timespec aggregation_delay = {
                    .tv_sec = 0,
                    .tv_nsec = SSH_CHATTER_TRANSLATION_BATCH_DELAY_NS};
                host_sleep_uninterruptible(&aggregation_delay);
            }
        }

        pthread_mutex_lock(&ctx->translation_mutex);
        while (batch_count < SSH_CHATTER_TRANSLATION_BATCH_MAX &&
               ctx->translation_pending_head != nullptr) {
            translation_job_t *candidate = ctx->translation_pending_head;
            if (candidate == nullptr) {
                break;
            }

            if (candidate->type != TRANSLATION_JOB_CAPTION) {
                break;
            }

            if (strcmp(candidate->target_language, batch[0]->target_language) !=
                0) {
                break;
            }

            size_t candidate_len = strlen(candidate->data.caption.sanitized) +
                                   SSH_CHATTER_TRANSLATION_SEGMENT_GUARD;
            if (estimate + candidate_len >=
                SSH_CHATTER_TRANSLATION_BATCH_BUFFER) {
                break;
            }

            ctx->translation_pending_head = candidate->next;
            if (ctx->translation_pending_head == nullptr) {
                ctx->translation_pending_tail = nullptr;
            }
            candidate->next = nullptr;
            batch[batch_count++] = candidate;
            estimate += candidate_len;
        }
        pthread_mutex_unlock(&ctx->translation_mutex);

        bool processed = false;
        if (batch_count > 1U) {
            processed =
                session_translation_process_batch(ctx, batch, batch_count);
        }

        if (!processed) {
            for (size_t idx = 0U; idx < batch_count; ++idx) {
                session_translation_process_single_job(ctx, batch[idx]);
            }
        }
    }

    if (memory_scope != nullptr) {
        sshc_memory_context_pop(memory_scope);
    }
    return nullptr;
}

static void session_channel_log_write_failure(session_ctx_t *ctx,
                                              const char *reason)
{
    if (ctx == nullptr) {
        return;
    }

    if (reason == nullptr || reason[0] == '\0') {
        reason = "transport write failure";
    }

    const char *username =
        ctx->user.name[0] != '\0' ? ctx->user.name : "unknown";
    printf("[session] transport write failure for %s: %s\n", username, reason);
}

static bool session_telnet_write_block(session_ctx_t *ctx,
                                       const unsigned char *data, size_t length)
{
    if (ctx == nullptr || data == nullptr || length == 0U ||
        ctx->telnet_fd < 0) {
        return true;
    }

    while (length > 0U) {
        size_t chunk = length;
        if (chunk > SSH_CHATTER_CHANNEL_WRITE_CHUNK) {
            chunk = SSH_CHATTER_CHANNEL_WRITE_CHUNK;
        }

        /* Expand data for telnet IAC escaping (IAC byte must be doubled) */
        unsigned char buffer[SSH_CHATTER_CHANNEL_WRITE_CHUNK * 2U];
        size_t expanded = 0U;
        for (size_t idx = 0U; idx < chunk; ++idx) {
            unsigned char byte = data[idx];
            buffer[expanded++] = byte;
            if (byte == TELNET_IAC) {
                buffer[expanded++] = TELNET_IAC;
            }
        }

        /* Write entire expanded buffer with proper error handling */
        size_t offset = 0U;
        unsigned int retry_count = 0U;
        const unsigned int max_retries = 3U;
        
        while (offset < expanded) {
            ssize_t written = send(ctx->telnet_fd, buffer + offset,
                                   expanded - offset, MSG_NOSIGNAL);
            if (written < 0) {
                if (errno == EINTR) {
                    /* Interrupted system call - retry immediately */
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* Socket buffer full - wait and retry */
                    if (retry_count++ >= max_retries) {
                        return false; /* Too many retries */
                    }
                    struct timespec backoff = {
                        .tv_sec = 0,
                        .tv_nsec = 10000000, /* 10ms backoff */
                    };
                    nanosleep(&backoff, NULL);
                    continue;
                }
                /* Other errors are fatal */
                return false;
            }
            
            if (written == 0) {
                /* Connection closed */
                return false;
            }
            
            offset += (size_t)written;
            retry_count = 0U; /* Reset retry counter on successful write */
        }

        data += chunk;
        length -= chunk;
    }

    /* Ensure data is pushed to network layer (strong sync point) */
    /* Note: TCP_NODELAY should be set on socket for immediate send */
    int flags = 0;
    socklen_t flags_len = sizeof(flags);
    if (getsockopt(ctx->telnet_fd, IPPROTO_TCP, TCP_NODELAY, &flags, &flags_len) == 0) {
        if (flags == 0) {
            /* TCP_NODELAY not set - force flush with empty MSG_OOB as sync marker */
            /* This ensures message boundaries are preserved */
            (void)send(ctx->telnet_fd, "", 0, MSG_NOSIGNAL);
        }
    }

    return true;
}

#define SSH_CHATTER_MAX_WRITE_TIMEOUT_MS 100

static bool session_channel_wait_writable(session_ctx_t *ctx, int timeout_ms)
{
    if (ctx == nullptr) {
        return false;
    }

    if (timeout_ms > SSH_CHATTER_MAX_WRITE_TIMEOUT_MS) {
        timeout_ms = SSH_CHATTER_MAX_WRITE_TIMEOUT_MS;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        if (ctx->telnet_fd < 0) {
            return false;
        }

        struct pollfd pfd = {
            .fd = ctx->telnet_fd,
            .events = POLLOUT,
            .revents = 0,
        };

        for (;;) {
            int result = poll(&pfd, 1, timeout_ms);
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (result == 0) {
                return false;
            }
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                return false;
            }
            if (pfd.revents & POLLOUT) {
                return true;
            }
            return false;
        }
    }

    if (ctx->session == nullptr) {
        return false;
    }

    int fd = ssh_get_fd(ctx->session);
    if (fd < 0) {
        struct timespec backoff = {
            .tv_sec = 0,
            .tv_nsec = SSH_CHATTER_CHANNEL_WRITE_BACKOFF_NS,
        };
        host_sleep_uninterruptible(&backoff);
        return true;
    }

    struct pollfd pfd = {
        .fd = fd,
        .events = POLLOUT,
        .revents = 0,
    };

    if (timeout_ms > SSH_CHATTER_MAX_WRITE_TIMEOUT_MS) {
        timeout_ms = SSH_CHATTER_MAX_WRITE_TIMEOUT_MS;
    }

    for (;;) {
        int result = poll(&pfd, 1, timeout_ms);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return false;
        }
        if (pfd.revents & POLLOUT) {
            return true;
        }
        return false;
    }
}

static bool session_channel_write_all(session_ctx_t *ctx, const void *data,
                                      size_t length)
{
    if (ctx == nullptr || data == nullptr || length == 0U ||
        !session_transport_active(ctx)) {
        return true;
    }

    const unsigned char *cursor = (const unsigned char *)data;
    size_t remaining = length;
    unsigned int stalled = 0U;

    while (remaining > 0U) {
        if (!session_channel_wait_writable(
                ctx, SSH_CHATTER_CHANNEL_WRITE_TIMEOUT_MS)) {
            if (++stalled >= SSH_CHATTER_CHANNEL_WRITE_MAX_STALLS) {
                session_channel_log_write_failure(ctx, "write timed out");
                return false;
            }
            continue;
        }

        stalled = 0U;

        size_t chunk = remaining;
        if (chunk > SSH_CHATTER_CHANNEL_WRITE_CHUNK) {
            chunk = SSH_CHATTER_CHANNEL_WRITE_CHUNK;
        }

        if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
            if (!session_telnet_write_block(ctx, cursor, chunk)) {
                session_channel_log_write_failure(ctx, "telnet write error");
                return false;
            }
            cursor += chunk;
            remaining -= chunk;
            continue;
        }

        ssize_t written =
            ssh_channel_write(ctx->channel, cursor, (uint32_t)chunk);
        if (written == SSH_ERROR) {
            const char *error = ssh_get_error(ctx->session);
            session_channel_log_write_failure(
                ctx, (error != nullptr && error[0] != '\0')
                         ? error
                         : "channel write error");
            return false;
        }

        if (written == 0) {
            if (ssh_channel_is_eof(ctx->channel) ||
                !ssh_channel_is_open(ctx->channel)) {
                session_channel_log_write_failure(
                    ctx, "channel closed during write");
                return false;
            }

            if (++stalled >= SSH_CHATTER_CHANNEL_WRITE_MAX_STALLS) {
                session_channel_log_write_failure(ctx, "channel write stalled");
                return false;
            }

            continue;
        }

        cursor += written;
        remaining -= (size_t)written;
    }

    return true;
}

typedef struct cp437_replacement {
    uint32_t codepoint;
    const char *replacement;
} cp437_replacement_t;

static const cp437_replacement_t kCp437AsciiReplacements[] = {
    {0x00A0U, " "},   {0x2013U, "-"},  {0x2014U, "-"},  {0x2015U, "-"},
    {0x2018U, "'"},   {0x2019U, "'"},  {0x201CU, "\""}, {0x201DU, "\""},
    {0x2026U, "..."}, {0x2190U, "<-"}, {0x2191U, "^"},  {0x2192U, "->"},
    {0x2193U, "v"},   {0x21B3U, "->"},
};

static const char *session_cp437_ascii_replacement(uint32_t codepoint)
{
    for (size_t idx = 0U; idx < sizeof(kCp437AsciiReplacements) /
                                    sizeof(kCp437AsciiReplacements[0]);
         ++idx) {
        if (kCp437AsciiReplacements[idx].codepoint == codepoint) {
            return kCp437AsciiReplacements[idx].replacement;
        }
    }
    return nullptr;
}

static char *session_cp437_normalize_utf8(const char *data, size_t length,
                                          size_t *normalized_length)
{
    if (data == nullptr || length == 0U) {
        if (normalized_length != nullptr) {
            *normalized_length = length;
        }
        return nullptr;
    }

    size_t capacity = length + 16U;
    char *buffer = (char *)GC_MALLOC(capacity);
    if (buffer == nullptr) {
        if (normalized_length != nullptr) {
            *normalized_length = length;
        }
        return nullptr;
    }

    size_t output = 0U;
    bool modified = false;
    const unsigned char *cursor = (const unsigned char *)data;
    size_t remaining = length;

    while (remaining > 0U) {
        uint32_t codepoint = 0U;
        size_t consumed =
            session_utf8_decode_codepoint(cursor, remaining, &codepoint);
        if (consumed == 0U) {
            codepoint = (uint32_t)(*cursor);
            consumed = 1U;
        }

        const char *replacement = session_cp437_ascii_replacement(codepoint);
        if (replacement != nullptr) {
            modified = true;
            size_t rep_len = strlen(replacement);
            size_t needed = output + rep_len + 1U;
            if (needed > capacity) {
                size_t new_capacity = capacity * 2U;
                if (new_capacity < needed) {
                    new_capacity = needed + 16U;
                }
                char *resized = (char *)GC_REALLOC(buffer, new_capacity);
                if (resized == nullptr) {
                    GC_FREE(buffer);
                    if (normalized_length != nullptr) {
                        *normalized_length = length;
                    }
                    return nullptr;
                }
                buffer = resized;
                capacity = new_capacity;
            }
            memcpy(buffer + output, replacement, rep_len);
            output += rep_len;
        } else {
            size_t needed = output + consumed + 1U;
            if (needed > capacity) {
                size_t new_capacity = capacity * 2U;
                if (new_capacity < needed) {
                    new_capacity = needed + 16U;
                }
                char *resized = (char *)GC_REALLOC(buffer, new_capacity);
                if (resized == nullptr) {
                    GC_FREE(buffer);
                    if (normalized_length != nullptr) {
                        *normalized_length = length;
                    }
                    return nullptr;
                }
                buffer = resized;
                capacity = new_capacity;
            }
            memcpy(buffer + output, cursor, consumed);
            output += consumed;
        }

        cursor += consumed;
        remaining -= consumed;
    }

    if (!modified) {
        GC_FREE(buffer);
        if (normalized_length != nullptr) {
            *normalized_length = length;
        }
        return nullptr;
    }

    buffer[output] = '\0';
    if (normalized_length != nullptr) {
        *normalized_length = output;
    }
    return buffer;
}

static bool __attribute__((unused))
session_channel_write_cp437(session_ctx_t *ctx, const char *data,
                                        size_t length)
{
    if (ctx == nullptr || data == nullptr || length == 0U) {
        return true;
    }

    iconv_t descriptor = iconv_open("CP437//TRANSLIT", "UTF-8");
    if (descriptor == (iconv_t)(-1)) {
        return session_channel_write_all(ctx, data, length);
    }

    size_t capacity = (length > 0U ? length : 1U) * 4U + 16U;
    char *buffer = (char *)GC_MALLOC(capacity);
    if (buffer == nullptr) {
        iconv_close(descriptor);
        return session_channel_write_all(ctx, data, length);
    }

    size_t normalized_length = 0U;
    char *normalized =
        session_cp437_normalize_utf8(data, length, &normalized_length);

    const char *input_cursor = normalized != nullptr ? normalized : data;
    size_t input_remaining = normalized != nullptr ? normalized_length : length;
    char *output_cursor = buffer;
    size_t output_remaining = capacity;

    bool fallback_to_plaintext = false;

    while (input_remaining > 0U) {
        size_t result =
            iconv(descriptor, (char **)&input_cursor, &input_remaining,
                  &output_cursor, &output_remaining);
        if (result == (size_t)-1) {
            if (errno == E2BIG) {
                size_t produced = capacity - output_remaining;
                size_t new_capacity = capacity * 2U;
                if (new_capacity <= capacity) {
                    new_capacity = capacity + length + 32U;
                }
                char *resized = (char *)GC_REALLOC(buffer, new_capacity);
                if (resized == nullptr) {
                    fallback_to_plaintext = true;
                    goto cleanup;
                }
                buffer = resized;
                output_cursor = buffer + produced;
                output_remaining = new_capacity - produced;
                capacity = new_capacity;
                continue;
            }
            if (errno == EILSEQ || errno == EINVAL) {
                ++input_cursor;
                --input_remaining;
                if (output_remaining == 0U) {
                    size_t produced = capacity - output_remaining;
                    size_t new_capacity = capacity * 2U;
                    if (new_capacity <= capacity) {
                        new_capacity = capacity + length + 32U;
                    }
                    char *resized = (char *)GC_REALLOC(buffer, new_capacity);
                    if (resized == nullptr) {
                        fallback_to_plaintext = true;
                        goto cleanup;
                    }
                    buffer = resized;
                    output_cursor = buffer + produced;
                    output_remaining = new_capacity - produced;
                    capacity = new_capacity;
                }
                *output_cursor++ = '?';
                output_remaining -= 1U;
                continue;
            }
            fallback_to_plaintext = true;
            goto cleanup;
        }
    }

cleanup:
    iconv_close(descriptor);
    bool success = false;
    if (fallback_to_plaintext) {
        const char *fallback_data = normalized != nullptr ? normalized : data;
        size_t fallback_length =
            normalized != nullptr ? normalized_length : length;
        success =
            session_channel_write_all(ctx, fallback_data, fallback_length);
    } else {
        size_t produced = capacity - output_remaining;
        success = session_channel_write_all(ctx, buffer, produced);
    }
    GC_FREE(buffer);
    if (normalized != nullptr) {
        GC_FREE(normalized);
    }
    return success;
}

static bool session_channel_write_codepage(session_ctx_t *ctx, const char *data,
                                           size_t length,
                                           session_codepage_t codepage)
{
    if (ctx == nullptr || data == nullptr || length == 0U) {
        return true;
    }

    /* For UTF-8 or unknown codepages, just pass through */
    if (codepage == SESSION_CODEPAGE_UTF8) {
        return session_channel_write_all(ctx, data, length);
    }

    const char *iconv_name = session_codepage_iconv_name(codepage);
    if (iconv_name == nullptr) {
        return session_channel_write_all(ctx, data, length);
    }

    iconv_t descriptor = iconv_open(iconv_name, "UTF-8");
    if (descriptor == (iconv_t)(-1)) {
        return session_channel_write_all(ctx, data, length);
    }

    size_t capacity = (length > 0U ? length : 1U) * 4U + 16U;
    char *buffer = (char *)GC_MALLOC(capacity);
    if (buffer == nullptr) {
        iconv_close(descriptor);
        return session_channel_write_all(ctx, data, length);
    }

    /* For CP437, apply normalization. For others, use data as-is */
    size_t normalized_length = 0U;
    char *normalized = nullptr;
    if (codepage == SESSION_CODEPAGE_CP437) {
        normalized = session_cp437_normalize_utf8(data, length, &normalized_length);
    }

    const char *input_cursor = normalized != nullptr ? normalized : data;
    size_t input_remaining = normalized != nullptr ? normalized_length : length;
    char *output_cursor = buffer;
    size_t output_remaining = capacity;

    bool fallback_to_plaintext = false;

    while (input_remaining > 0U) {
        size_t result =
            iconv(descriptor, (char **)&input_cursor, &input_remaining,
                  &output_cursor, &output_remaining);
        if (result == (size_t)-1) {
            if (errno == E2BIG) {
                size_t produced = capacity - output_remaining;
                size_t new_capacity = capacity * 2U;
                if (new_capacity <= capacity) {
                    new_capacity = capacity + length + 32U;
                }
                char *resized = (char *)GC_REALLOC(buffer, new_capacity);
                if (resized == nullptr) {
                    fallback_to_plaintext = true;
                    goto cleanup;
                }
                buffer = resized;
                output_cursor = buffer + produced;
                output_remaining = new_capacity - produced;
                capacity = new_capacity;
                continue;
            }
            if (errno == EILSEQ || errno == EINVAL) {
                ++input_cursor;
                --input_remaining;
                if (output_remaining == 0U) {
                    size_t produced = capacity - output_remaining;
                    size_t new_capacity = capacity * 2U;
                    if (new_capacity <= capacity) {
                        new_capacity = capacity + length + 32U;
                    }
                    char *resized = (char *)GC_REALLOC(buffer, new_capacity);
                    if (resized == nullptr) {
                        fallback_to_plaintext = true;
                        goto cleanup;
                    }
                    buffer = resized;
                    output_cursor = buffer + produced;
                    output_remaining = new_capacity - produced;
                    capacity = new_capacity;
                }
                *output_cursor++ = '?';
                output_remaining -= 1U;
                continue;
            }
            fallback_to_plaintext = true;
            goto cleanup;
        }
    }

cleanup:
    iconv_close(descriptor);
    bool success = false;
    if (fallback_to_plaintext) {
        const char *fallback_data = normalized != nullptr ? normalized : data;
        size_t fallback_length =
            normalized != nullptr ? normalized_length : length;
        success =
            session_channel_write_all(ctx, fallback_data, fallback_length);
    } else {
        size_t produced = capacity - output_remaining;
        success = session_channel_write_all(ctx, buffer, produced);
    }
    GC_FREE(buffer);
    if (normalized != nullptr) {
        GC_FREE(normalized);
    }
    return success;
}

static bool session_output_lock(session_ctx_t *ctx)
{
    if (ctx == nullptr || !ctx->output_lock_initialized) {
        return false;
    }

    int error = pthread_mutex_lock(&ctx->output_lock);
    if (error != 0) {
        printf("[session] failed to lock output for %s: %s\n",
               (ctx->user.name[0] != '\0') ? ctx->user.name : "unknown",
               strerror(error));
        return false;
    }

    return true;
}

static void session_output_unlock(session_ctx_t *ctx)
{
    if (ctx == nullptr || !ctx->output_lock_initialized) {
        return;
    }

    int error = pthread_mutex_unlock(&ctx->output_lock);
    if (error != 0) {
        printf("[session] failed to unlock output for %s: %s\n",
               (ctx->user.name[0] != '\0') ? ctx->user.name : "unknown",
               strerror(error));
    }
}

// Output buffering functions to prevent flickering
static void session_output_buffer_start(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    ctx->output_buffering_enabled = true;
    ctx->output_buffer_length = 0U;
}

static void session_output_buffer_clear(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    ctx->output_buffer_length = 0U;
}

static void session_output_buffer_flush(session_ctx_t *ctx)
{
    if (ctx == nullptr || !ctx->output_buffering_enabled) {
        return;
    }

    if (ctx->output_buffer_length > 0U) {
        // Temporarily disable buffering to avoid infinite recursion
        ctx->output_buffering_enabled = false;
        session_channel_write(ctx, ctx->output_buffer,
                              ctx->output_buffer_length);
        ctx->output_buffer_length = 0U;
        ctx->output_buffering_enabled = true;
    }
}

static void session_output_buffer_stop(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    session_output_buffer_flush(ctx);
    session_output_buffer_clear(ctx);
    ctx->output_buffering_enabled = false;
}

static bool session_output_buffer_append(session_ctx_t *ctx, const void *data,
                                         size_t length)
{
    if (ctx == nullptr || data == nullptr || length == 0U) {
        return false;
    }

    // Check if buffer has enough space
    if (ctx->output_buffer_length + length > SSH_CHATTER_OUTPUT_BUFFER_SIZE) {
        // Buffer is full, flush it first
        session_output_buffer_flush(ctx);
        session_output_buffer_clear(ctx);

        // If still not enough space after flush, this write is too large
        if (length > SSH_CHATTER_OUTPUT_BUFFER_SIZE) {
            // Write directly without buffering
            ctx->output_buffering_enabled = false;
            session_channel_write(ctx, data, length);
            ctx->output_buffering_enabled = true;
            return true;
        }
    }

    // Append data to buffer
    memcpy(ctx->output_buffer + ctx->output_buffer_length, data, length);
    ctx->output_buffer_length += length;
    return true;
}

static void session_channel_write(session_ctx_t *ctx, const void *data,
                                  size_t length)
{
    if (ctx == nullptr || data == nullptr || length == 0U || ctx->should_exit ||
        !session_transport_active(ctx)) {
        return;
    }

    // If output buffering is enabled, append to buffer instead of writing directly
    if (ctx->output_buffering_enabled) {
        session_output_buffer_append(ctx, data, length);
        return;
    }

    bool locked = session_output_lock(ctx);

    bool success = true;
    if (ctx->channel_mutex_initialized) {
        int lock_result = pthread_mutex_lock(&ctx->channel_mutex);
        if (lock_result == 0) {
            locked = true;
        } else {
            humanized_log_error("session", "failed to lock channel mutex",
                                lock_result);
        }
    }

    if (ctx->prefer_cp437_output) {
        /* Use the generic codepage conversion with the active codepage */
        success = session_channel_write_codepage(ctx, (const char *)data, length,
                                                 ctx->active_codepage);
    } else if (ctx->prefer_utf16_output) {
        success = session_channel_write_utf16(ctx, (const char *)data, length);
    } else {
        success = session_channel_write_all(ctx, data, length);
    }

    if (locked) {
        int unlock_result = pthread_mutex_unlock(&ctx->channel_mutex);
        if (unlock_result != 0) {
            humanized_log_error("session", "failed to unlock channel mutex",
                                unlock_result);
        }
    }

    if (!success) {
        ctx->should_exit = true;
    }

    if (locked) {
        session_output_unlock(ctx);
    }
}

static void session_channel_flush(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    // For Telnet connections, ensure output buffer is flushed immediately
    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        // Flush any pending data in the output buffer to ensure immediate delivery
        // TCP_NODELAY is already set on the socket when the connection is accepted,
        // so flushing the output buffer will cause immediate transmission
        session_output_buffer_flush(ctx);
        return;
    }

    // For SSH, flush any pending data in libssh's buffers
    if (ctx->session != nullptr) {
        // Use a short timeout (50ms) to avoid blocking
        ssh_blocking_flush(ctx->session, 50);
    }
}

static bool session_channel_write_utf16(session_ctx_t *ctx, const char *data,
                                        size_t length)
{
    if (ctx == nullptr || data == nullptr) {
        return true;
    }

    size_t idx = 0U;
    while (idx < length) {
        unsigned char byte = (unsigned char)data[idx];
        if (byte == '\033') {
            size_t start = idx++;
            if (idx < length) {
                unsigned char next = (unsigned char)data[idx];
                if (next == '[') {
                    ++idx;
                    while (idx < length) {
                        unsigned char ch = (unsigned char)data[idx++];
                        if (ch >= '@' && ch <= '~') {
                            break;
                        }
                    }
                } else if (next == ']') {
                    ++idx;
                    while (idx < length) {
                        unsigned char ch = (unsigned char)data[idx++];
                        if (ch == '\a') {
                            break;
                        }
                        if (ch == '\033' && idx < length) {
                            unsigned char terminator = (unsigned char)data[idx];
                            if (terminator == '\\') {
                                ++idx;
                                break;
                            }
                        }
                    }
                }
            }

            if (!session_channel_write_all(ctx, data + start, idx - start)) {
                return false;
            }
            continue;
        }

        if (byte < 0x20U || byte == 0x7FU) {
            if (!session_channel_write_all(ctx, data + idx, 1U)) {
                return false;
            }
            ++idx;
            continue;
        }

        size_t start = idx;
        while (idx < length) {
            unsigned char ch = (unsigned char)data[idx];
            if (ch == '\033' || ch < 0x20U || ch == 0x7FU) {
                break;
            }
            ++idx;
        }

        if (!session_channel_write_utf16_segment(ctx, data + start,
                                                 idx - start)) {
            return false;
        }
    }

    return true;
}

static bool session_channel_write_utf16_segment(session_ctx_t *ctx,
                                                const char *data, size_t length)
{
    if (ctx == nullptr || data == nullptr || length == 0U) {
        return true;
    }

    size_t max_output = length * 4U;
    if (max_output == 0U) {
        return true;
    }

    unsigned char stack_buffer[512];
    unsigned char *buffer = nullptr;
    bool use_stack = max_output <= sizeof(stack_buffer);
    if (use_stack) {
        buffer = stack_buffer;
    } else {
        buffer = (unsigned char *)GC_MALLOC(max_output);
        if (buffer == nullptr) {
            return session_channel_write_all(ctx, data, length);
        }
    }

    size_t produced = 0U;
    bool encoded =
        session_utf8_to_utf16le(data, length, buffer, max_output, &produced);
    bool result = false;
    if (encoded) {
        result = session_channel_write_all(ctx, buffer, produced);
    } else {
        result = session_channel_write_all(ctx, data, length);
    }

    if (!use_stack) {
    }

    return result;
}

static size_t session_utf8_decode_codepoint(const unsigned char *data,
                                            size_t length, uint32_t *codepoint)
{
    if (data == nullptr || length == 0U || codepoint == nullptr) {
        return 0U;
    }

    unsigned char b0 = data[0];
    if (b0 < 0x80U) {
        *codepoint = b0;
        return 1U;
    }

    if ((b0 & 0xE0U) == 0xC0U) {
        if (length < 2U) {
            return 0U;
        }
        unsigned char b1 = data[1];
        if ((b1 & 0xC0U) != 0x80U) {
            return 0U;
        }
        uint32_t value =
            ((uint32_t)(b0 & 0x1FU) << 6U) | (uint32_t)(b1 & 0x3FU);
        if (value < 0x80U) {
            return 0U;
        }
        *codepoint = value;
        return 2U;
    }

    if ((b0 & 0xF0U) == 0xE0U) {
        if (length < 3U) {
            return 0U;
        }
        unsigned char b1 = data[1];
        unsigned char b2 = data[2];
        if ((b1 & 0xC0U) != 0x80U || (b2 & 0xC0U) != 0x80U) {
            return 0U;
        }
        uint32_t value = ((uint32_t)(b0 & 0x0FU) << 12U) |
                         ((uint32_t)(b1 & 0x3FU) << 6U) |
                         (uint32_t)(b2 & 0x3FU);
        if (value < 0x800U || (value >= 0xD800U && value <= 0xDFFFU)) {
            return 0U;
        }
        *codepoint = value;
        return 3U;
    }

    if ((b0 & 0xF8U) == 0xF0U) {
        if (length < 4U) {
            return 0U;
        }
        unsigned char b1 = data[1];
        unsigned char b2 = data[2];
        unsigned char b3 = data[3];
        if ((b1 & 0xC0U) != 0x80U || (b2 & 0xC0U) != 0x80U ||
            (b3 & 0xC0U) != 0x80U) {
            return 0U;
        }
        uint32_t value =
            ((uint32_t)(b0 & 0x07U) << 18U) | ((uint32_t)(b1 & 0x3FU) << 12U) |
            ((uint32_t)(b2 & 0x3FU) << 6U) | (uint32_t)(b3 & 0x3FU);
        if (value < 0x10000U || value > 0x10FFFFU) {
            return 0U;
        }
        *codepoint = value;
        return 4U;
    }

    return 0U;
}

static bool session_utf8_to_utf16le(const char *input, size_t length,
                                    unsigned char *output, size_t capacity,
                                    size_t *produced)
{
    if (input == nullptr || output == nullptr) {
        return false;
    }

    size_t out_idx = 0U;
    size_t idx = 0U;
    while (idx < length) {
        uint32_t codepoint = 0U;
        size_t consumed = session_utf8_decode_codepoint(
            (const unsigned char *)input + idx, length - idx, &codepoint);
        if (consumed == 0U) {
            codepoint = 0xFFFD;
            consumed = 1U;
        }
        idx += consumed;

        if (codepoint <= 0xFFFFU) {
            if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
                codepoint = 0xFFFD;
            }
            if (out_idx + 2U > capacity) {
                return false;
            }
            output[out_idx++] = (unsigned char)(codepoint & 0xFFU);
            output[out_idx++] = (unsigned char)((codepoint >> 8U) & 0xFFU);
            continue;
        }

        uint32_t adjusted = codepoint - 0x10000U;
        uint16_t high = (uint16_t)(0xD800U | ((adjusted >> 10U) & 0x3FFU));
        uint16_t low = (uint16_t)(0xDC00U | (adjusted & 0x3FFU));
        if (out_idx + 4U > capacity) {
            return false;
        }
        output[out_idx++] = (unsigned char)(high & 0xFFU);
        output[out_idx++] = (unsigned char)((high >> 8U) & 0xFFU);
        output[out_idx++] = (unsigned char)(low & 0xFFU);
        output[out_idx++] = (unsigned char)((low >> 8U) & 0xFFU);
    }

    if (produced != nullptr) {
        *produced = out_idx;
    }
    return true;
}

static const char SESSION_COLUMN_RESET[] = "\033[1G";

static void session_fill_line_with_theme(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    const char *bg = ctx->system_bg_code != nullptr ? ctx->system_bg_code : "";
    const size_t bg_len = strlen(bg);

    static const char ERASE_ENTIRE_LINE[] = "\033[2K";

    session_channel_write(ctx, SESSION_COLUMN_RESET,
                          sizeof(SESSION_COLUMN_RESET) - 1U);

    if (bg_len > 0U) {
        session_channel_write(ctx, bg, bg_len);
    }

    session_channel_write(ctx, ERASE_ENTIRE_LINE,
                          sizeof(ERASE_ENTIRE_LINE) - 1U);

    session_channel_write(ctx, SESSION_COLUMN_RESET,
                          sizeof(SESSION_COLUMN_RESET) - 1U);

    if (bg_len > 0U) {
        session_channel_write(ctx, bg, bg_len);
    }
}

static void session_apply_background_fill(session_ctx_t *ctx)
{
    if (ctx == nullptr || !session_transport_active(ctx)) {
        return;
    }

    bool locked = session_output_lock(ctx);
    session_fill_line_with_theme(ctx);

    if (locked) {
        session_output_unlock(ctx);
    }
}

static size_t session_append_fragment(char *dest, size_t dest_size,
                                      size_t offset, const char *fragment);

static bool session_sequence_resets_theme(const char *sequence_start,
                                          size_t sequence_length,
                                          bool *reset_fg, bool *reset_bg,
                                          bool *reset_bold)
{
    if (sequence_start == nullptr || sequence_length == 0U) {
        return false;
    }

    if (sequence_start[0] != '\033' || sequence_length < 2U) {
        return false;
    }

    const char final_char = sequence_start[sequence_length - 1U];
    if (final_char != 'm') {
        return false;
    }

    if (reset_fg != nullptr) {
        *reset_fg = false;
    }
    if (reset_bg != nullptr) {
        *reset_bg = false;
    }
    if (reset_bold != nullptr) {
        *reset_bold = false;
    }

    const char *params_start = sequence_start + 2U;
    const char *params_end = sequence_start + sequence_length - 1U;
    bool reset_all = false;

    if (params_start >= params_end) {
        reset_all = true;
    }

    while (!reset_all && params_start < params_end) {
        if (*params_start == ';') {
            ++params_start;
            continue;
        }

        char *parse_end = nullptr;
        long value = strtol(params_start, &parse_end, 10);
        if (parse_end == params_start) {
            reset_all = true;
            break;
        }

        if (value == 0L) {
            reset_all = true;
            break;
        }
        if (value == 39L && reset_fg != nullptr) {
            *reset_fg = true;
        }
        if (value == 49L && reset_bg != nullptr) {
            *reset_bg = true;
        }
        if ((value == 21L || value == 22L) && reset_bold != nullptr) {
            *reset_bold = true;
        }

        params_start = parse_end;
    }

    if (reset_all) {
        if (reset_fg != nullptr) {
            *reset_fg = true;
        }
        if (reset_bg != nullptr) {
            *reset_bg = true;
        }
        if (reset_bold != nullptr) {
            *reset_bold = true;
        }
    }

    return reset_all || (reset_fg != nullptr && *reset_fg) ||
           (reset_bg != nullptr && *reset_bg) ||
           (reset_bold != nullptr && *reset_bold);
}

static size_t session_prepare_themed_output(session_ctx_t *ctx,
                                            const char *render_source,
                                            char *dest, size_t dest_size)
{
    if (dest == nullptr || dest_size == 0U) {
        return 0U;
    }

    dest[0] = '\0';

    const char *bg = ctx->system_bg_code != nullptr ? ctx->system_bg_code : "";
    const char *fg = ctx->system_fg_code != nullptr ? ctx->system_fg_code : "";
    const char *bold = ctx->system_is_bold ? ANSI_BOLD : "";

    size_t offset = 0U;
    offset = session_append_fragment(dest, dest_size, offset, bg);
    offset = session_append_fragment(dest, dest_size, offset, fg);
    offset = session_append_fragment(dest, dest_size, offset, bold);

    const char *cursor = render_source;
    while (cursor != nullptr && *cursor != '\0') {
        if ((size_t)offset >= dest_size - 1U) {
            break;
        }

        if (*cursor == '\033' && cursor[1] == '[') {
            const char *sequence_start = cursor;
            cursor += 2;
            while (*cursor != '\0' && (*cursor < '@' || *cursor > '~')) {
                ++cursor;
            }
            if (*cursor == '\0') {
                break;
            }
            ++cursor;
            size_t sequence_length = (size_t)(cursor - sequence_start);
            if (sequence_length >= dest_size - offset) {
                sequence_length = dest_size - offset - 1U;
            }
            memcpy(dest + offset, sequence_start, sequence_length);
            offset += sequence_length;
            dest[offset] = '\0';

            bool reset_fg = false;
            bool reset_bg = false;
            bool reset_bold = false;
            if (session_sequence_resets_theme(sequence_start, sequence_length,
                                              &reset_fg, &reset_bg,
                                              &reset_bold)) {
                if (reset_bg) {
                    offset =
                        session_append_fragment(dest, dest_size, offset, bg);
                }
                if (reset_fg) {
                    offset =
                        session_append_fragment(dest, dest_size, offset, fg);
                }
                if (ctx->system_is_bold && reset_bold) {
                    offset = session_append_fragment(dest, dest_size, offset,
                                                     ANSI_BOLD);
                }
            }

            continue;
        }

        dest[offset++] = *cursor++;
        dest[offset] = '\0';
    }

    return offset;
}

static void session_write_rendered_line(session_ctx_t *ctx,
                                        const char *render_source)
{
    if (ctx == nullptr || render_source == nullptr ||
        !session_transport_active(ctx)) {
        return;
    }

    bool locked = session_output_lock(ctx);
    session_fill_line_with_theme(ctx);

    char buffer[SSH_CHATTER_MESSAGE_LIMIT * 4U];
    size_t offset = session_prepare_themed_output(ctx, render_source, buffer,
                                                  sizeof(buffer));

    // Prepend \033[1G to ensure cursor is at the beginning of the line
    static const char column_reset[] = "\033[1G";
    session_channel_write(ctx, column_reset, sizeof(column_reset) - 1U);

    if (offset > 0U) {
        session_channel_write(ctx, buffer, offset);
    }

    session_channel_write(ctx, ANSI_RESET, sizeof(ANSI_RESET) - 1U);
    session_channel_write(ctx, "\r\n", 2U);

    if (locked) {
        session_output_unlock(ctx);
    }
}

static void session_send_caption_line(session_ctx_t *ctx, const char *message)
{
    if (ctx == nullptr || message == nullptr ||
        !session_transport_active(ctx)) {
        return;
    }

    bool locked = session_output_lock(ctx);
    session_channel_write(ctx, SESSION_COLUMN_RESET,
                          sizeof(SESSION_COLUMN_RESET) - 1U);
    session_channel_write(ctx, ANSI_INSERT_LINE, sizeof(ANSI_INSERT_LINE) - 1U);

    session_write_rendered_line(ctx, message);

    if (locked) {
        session_output_unlock(ctx);
    }
}

static void session_render_caption_with_offset(session_ctx_t *ctx,
                                               const char *message,
                                               size_t move_up)
{
    if (ctx == nullptr || message == nullptr ||
        !session_transport_active(ctx)) {
        return;
    }

    if (move_up == 0U) {
        session_send_caption_line(ctx, message);
        return;
    }

    bool locked = session_output_lock(ctx);
    session_channel_write(ctx, "\033[s", 3U);

    char command[32];
    int written = snprintf(command, sizeof(command), "\033[%zuA", move_up);
    if (written > 0 && (size_t)written < sizeof(command)) {
        session_channel_write(ctx, command, (size_t)written);
    }

    session_channel_write(ctx, SESSION_COLUMN_RESET,
                          sizeof(SESSION_COLUMN_RESET) - 1U);
    session_write_rendered_line(ctx, message);
    session_channel_write(ctx, "\033[u", 3U);

    if (locked) {
        session_output_unlock(ctx);
    }
}

static void session_telnet_send_option(session_ctx_t *ctx,
                                       unsigned char command,
                                       unsigned char option)
{
    if (ctx == nullptr || ctx->telnet_fd < 0) {
        return;
    }

    unsigned char payload[3] = {TELNET_IAC, command, option};
    send(ctx->telnet_fd, payload, sizeof(payload), MSG_NOSIGNAL);
}

static void session_telnet_request_terminal_type(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->telnet_fd < 0 ||
        ctx->telnet_terminal_type_requested) {
        return;
    }

    unsigned char payload[] = {
        TELNET_IAC, TELNET_CMD_SB, TELNET_OPT_TERMINAL_TYPE,
        1U,         TELNET_IAC,    TELNET_CMD_SE};
    send(ctx->telnet_fd, payload, sizeof(payload), MSG_NOSIGNAL);
    ctx->telnet_terminal_type_requested = true;
}

static void session_telnet_handle_option(session_ctx_t *ctx,
                                         unsigned char command,
                                         unsigned char option)
{
    if (ctx == nullptr) {
        return;
    }

    switch (command) {
    case TELNET_CMD_DO:
        if (option == TELNET_OPT_BINARY) {
            session_telnet_send_option(ctx, TELNET_CMD_WILL, option);
        } else if (option == TELNET_OPT_SUPPRESS_GO_AHEAD ||
                   option == TELNET_OPT_ECHO) {
            session_telnet_send_option(ctx, TELNET_CMD_WILL, option);
        } else if (option == TELNET_OPT_TERMINAL_TYPE) {
            session_telnet_send_option(ctx, TELNET_CMD_WONT, option);
        } else {
            session_telnet_send_option(ctx, TELNET_CMD_WONT, option);
        }
        break;
    case TELNET_CMD_DONT:
        session_telnet_send_option(ctx, TELNET_CMD_WONT, option);
        break;
    case TELNET_CMD_WILL:
        if (option == TELNET_OPT_BINARY ||
            option == TELNET_OPT_SUPPRESS_GO_AHEAD) {
            session_telnet_send_option(ctx, TELNET_CMD_DO, option);
        } else if (option == TELNET_OPT_TERMINAL_TYPE) {
            session_telnet_send_option(ctx, TELNET_CMD_DO, option);
            session_telnet_request_terminal_type(ctx);
        } else {
            session_telnet_send_option(ctx, TELNET_CMD_DONT, option);
        }
        break;
    case TELNET_CMD_WONT:
        session_telnet_send_option(ctx, TELNET_CMD_DONT, option);
        break;
    default:
        break;
    }
}

static void session_telnet_initialize(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->telnet_fd < 0 || ctx->telnet_negotiated) {
        return;
    }

    session_telnet_send_option(ctx, TELNET_CMD_WILL, TELNET_OPT_BINARY);
    session_telnet_send_option(ctx, TELNET_CMD_DO, TELNET_OPT_BINARY);
    session_telnet_send_option(ctx, TELNET_CMD_WILL, TELNET_OPT_ECHO);
    session_telnet_send_option(ctx, TELNET_CMD_WILL,
                               TELNET_OPT_SUPPRESS_GO_AHEAD);
    session_telnet_send_option(ctx, TELNET_CMD_DO,
                               TELNET_OPT_SUPPRESS_GO_AHEAD);
    session_telnet_send_option(ctx, TELNET_CMD_DONT, TELNET_OPT_LINEMODE);
    session_telnet_send_option(ctx, TELNET_CMD_WONT, TELNET_OPT_STATUS);
    session_telnet_send_option(ctx, TELNET_CMD_DO, TELNET_OPT_TERMINAL_TYPE);
    session_telnet_send_option(ctx, TELNET_CMD_WONT, TELNET_OPT_TERMINAL_SPEED);
    session_telnet_send_option(ctx, TELNET_CMD_WONT, TELNET_OPT_NAWS);

    ctx->telnet_negotiated = true;
}

static int session_telnet_read_byte(session_ctx_t *ctx, unsigned char *out,
                                    int timeout_ms)
{
    if (ctx == nullptr || out == nullptr || ctx->telnet_fd < 0) {
        return SSH_ERROR;
    }

    if (ctx->telnet_pending_valid) {
        ctx->telnet_pending_valid = false;
        *out = (unsigned char)ctx->telnet_pending_char;
        return 1;
    }

    for (;;) {
        struct pollfd pfd = {
            .fd = ctx->telnet_fd,
            .events = POLLIN,
            .revents = 0,
        };

        int poll_result = poll(&pfd, 1, timeout_ms);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SSH_ERROR;
        }
        if (poll_result == 0) {
            return SSH_AGAIN;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            ctx->telnet_eof = true;
            return 0;
        }

        unsigned char byte = 0U;
        ssize_t read_result = recv(ctx->telnet_fd, &byte, 1, 0);
        if (read_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return SSH_ERROR;
        }
        if (read_result == 0) {
            ctx->telnet_eof = true;
            return 0;
        }

        if (byte == TELNET_IAC) {
            unsigned char command = 0U;
            ssize_t command_result = recv(ctx->telnet_fd, &command, 1, 0);
            if (command_result <= 0) {
                if (command_result < 0 && errno == EINTR) {
                    continue;
                }
                ctx->telnet_eof = (command_result == 0);
                return ctx->telnet_eof ? 0 : SSH_ERROR;
            }

            if (command == TELNET_IAC) {
                *out = TELNET_IAC;
                return 1;
            }

            if (command == TELNET_CMD_DO || command == TELNET_CMD_DONT ||
                command == TELNET_CMD_WILL || command == TELNET_CMD_WONT) {
                unsigned char option = 0U;
                ssize_t option_result = recv(ctx->telnet_fd, &option, 1, 0);
                if (option_result <= 0) {
                    if (option_result < 0 && errno == EINTR) {
                        continue;
                    }
                    ctx->telnet_eof = (option_result == 0);
                    return ctx->telnet_eof ? 0 : SSH_ERROR;
                }
                session_telnet_handle_option(ctx, command, option);
                continue;
            }

            if (command == TELNET_CMD_SB) {
                unsigned char option = 0U;
                ssize_t option_result = recv(ctx->telnet_fd, &option, 1, 0);
                if (option_result <= 0) {
                    if (option_result < 0 && errno == EINTR) {
                        continue;
                    }
                    ctx->telnet_eof = (option_result == 0);
                    return ctx->telnet_eof ? 0 : SSH_ERROR;
                }

                if (option == TELNET_OPT_TERMINAL_TYPE) {
                    unsigned char qualifier = 0U;
                    ssize_t qual_result =
                        recv(ctx->telnet_fd, &qualifier, 1, 0);
                    if (qual_result <= 0) {
                        if (qual_result < 0 && errno == EINTR) {
                            continue;
                        }
                        ctx->telnet_eof = (qual_result == 0);
                        return ctx->telnet_eof ? 0 : SSH_ERROR;
                    }

                    char type_buffer[SSH_CHATTER_TERMINAL_TYPE_LEN];
                    size_t type_len = 0U;
                    bool finished = false;

                    while (!finished) {
                        unsigned char chunk = 0U;
                        ssize_t chunk_result =
                            recv(ctx->telnet_fd, &chunk, 1, 0);
                        if (chunk_result < 0) {
                            if (errno == EINTR) {
                                continue;
                            }
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                continue;
                            }
                            return SSH_ERROR;
                        }
                        if (chunk_result == 0) {
                            ctx->telnet_eof = true;
                            return 0;
                        }

                        if (chunk == TELNET_IAC) {
                            unsigned char next = 0U;
                            ssize_t next_result =
                                recv(ctx->telnet_fd, &next, 1, 0);
                            if (next_result < 0) {
                                if (errno == EINTR) {
                                    continue;
                                }
                                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                    continue;
                                }
                                return SSH_ERROR;
                            }
                            if (next_result == 0) {
                                ctx->telnet_eof = true;
                                return 0;
                            }

                            if (next == TELNET_CMD_SE) {
                                finished = true;
                                break;
                            }
                            if (next == TELNET_IAC) {
                                if (type_len + 1U < sizeof(type_buffer)) {
                                    type_buffer[type_len++] = (char)TELNET_IAC;
                                }
                            }
                            continue;
                        }

                        if (type_len + 1U < sizeof(type_buffer)) {
                            type_buffer[type_len++] = (char)chunk;
                        }
                    }

                    if (type_len < sizeof(type_buffer)) {
                        type_buffer[type_len] = '\0';
                    } else {
                        type_buffer[sizeof(type_buffer) - 1U] = '\0';
                    }

                    if (qualifier == 0U) {
                        trim_whitespace_inplace(type_buffer);
                        if (type_buffer[0] != '\0') {
                            for (size_t idx = 0U; type_buffer[idx] != '\0';
                                 ++idx) {
                                type_buffer[idx] = (char)toupper(
                                    (unsigned char)type_buffer[idx]);
                            }
                            snprintf(ctx->terminal_type,
                                     sizeof(ctx->terminal_type), "%s",
                                     type_buffer);
                            session_refresh_output_encoding(ctx);
                        }
                    }
                } else {
                    unsigned char prev = 0U;
                    for (;;) {
                        unsigned char chunk = 0U;
                        ssize_t chunk_result =
                            recv(ctx->telnet_fd, &chunk, 1, 0);
                        if (chunk_result < 0) {
                            if (errno == EINTR) {
                                continue;
                            }
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                continue;
                            }
                            return SSH_ERROR;
                        }
                        if (chunk_result == 0) {
                            ctx->telnet_eof = true;
                            return 0;
                        }
                        if (prev == TELNET_IAC && chunk == TELNET_CMD_SE) {
                            break;
                        }
                        prev = (chunk == TELNET_IAC) ? TELNET_IAC : 0U;
                    }
                }
                continue;
            }

            if (command == TELNET_CMD_NOP || command == TELNET_CMD_DM ||
                command == TELNET_CMD_BREAK) {
                continue;
            }

            continue;
        }

        if (byte == '\r') {
            for (;;) {
                unsigned char next = 0U;
                ssize_t next_result =
                    recv(ctx->telnet_fd, &next, 1, MSG_PEEK | MSG_DONTWAIT);
                if (next_result < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    return SSH_ERROR;
                }
                if (next_result == 0) {
                    ctx->telnet_eof = true;
                    break;
                }

                if (next == '\n' || next == '\0') {
                    recv(ctx->telnet_fd, &next, 1, 0);
                } else {
                    recv(ctx->telnet_fd, &next, 1, 0);
                    ctx->telnet_pending_char = (int)next;
                    ctx->telnet_pending_valid = true;
                }
                break;
            }

            ctx->telnet_consume_next_lf = true;
            *out = '\r';
            return 1;
        }

        *out = byte;
        return 1;
    }
}

static bool session_telnet_collect_line(session_ctx_t *ctx, char *buffer,
                                        size_t length)
{
    if (ctx == nullptr || buffer == nullptr || length == 0U) {
        return false;
    }

    size_t written = 0U;
    bool ignore_next_newline = ctx->telnet_consume_next_lf;
    unsigned char glyph_sizes[SSH_CHATTER_MESSAGE_LIMIT];
    size_t glyph_count = 0U;

    while (!ctx->should_exit) {
        unsigned char byte = 0U;
        int read_result = session_telnet_read_byte(ctx, &byte, -1);
        if (read_result == SSH_AGAIN) {
            continue;
        }
        if (read_result <= 0) {
            ctx->should_exit = true;
            return false;
        }

        if (ignore_next_newline) {
            if (byte == '\n' || byte == '\0') {
                ignore_next_newline = false;
                continue;
            }
            ignore_next_newline = false;
        }

        if (byte == '\0') {
            // Some telnet clients emit NUL padding bytes (e.g. CR NUL) when
            // sending user input. Treat them as no-ops so clients like IcyTerm
            // stay connected.
            continue;
        }

        if (byte == '\r' || byte == '\n') {
            session_channel_write(ctx, "\r\n", 2U);
            if (byte == '\r') {
                ignore_next_newline = true;
            }
            break;
        }

        if (byte == 0x7FU || byte == '\b') {
            if (glyph_count > 0U) {
                size_t remove = glyph_sizes[--glyph_count];
                if (remove > written) {
                    written = 0U;
                } else {
                    written -= remove;
                }
                buffer[written] = '\0';
                session_channel_write(ctx, "\b \b", 3U);
            }
            /* Also reset multi-byte buffer on backspace */
            ctx->multibyte_input_length = 0U;
            continue;
        }

        if (byte == 0x03U || byte == 0x04U) {
            ctx->should_exit = true;
            return false;
        }

        if (byte < 0x20U) {
            continue;
        }

        char encoded[8];
        size_t encoded_len = 0U;
        
        if (ctx->cp437_input_enabled) {
            encoded_len =
                session_codepage_byte_to_utf8(ctx->active_codepage, &ctx->codepage_ctx, byte, encoded, sizeof(encoded));
            if (encoded_len == 0U) {
                encoded[0] = '?';
                encoded_len = 1U;
            }
        } else {
            /* UTF-8 mode - pass through */
            encoded[0] = (char)byte;
            encoded_len = 1U;
        }

        if (written + encoded_len >= length) {
            const char bell = '\a';
            session_channel_write(ctx, &bell, 1U);
            continue;
        }

        if (glyph_count < sizeof(glyph_sizes)) {
            glyph_sizes[glyph_count++] = (unsigned char)encoded_len;
        }

        memcpy(&buffer[written], encoded, encoded_len);
        written += encoded_len;
        buffer[written] = '\0';
        session_channel_write(ctx, encoded, encoded_len);
    }

    buffer[written] = '\0';
    ctx->telnet_consume_next_lf = ignore_next_newline;
    return !ctx->should_exit;
}

static int session_pw_auth_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool session_pw_auth_decode_hex(const char *hex, uint8_t *out,
                                       size_t out_len)
{
    if (hex == nullptr || out == nullptr || out_len == 0U) {
        return false;
    }

    size_t hex_len = strlen(hex);
    if (hex_len != out_len * 2U) {
        return false;
    }

    for (size_t idx = 0U; idx < out_len; ++idx) {
        int hi = session_pw_auth_hex_value(hex[idx * 2U]);
        int lo = session_pw_auth_hex_value(hex[idx * 2U + 1U]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[idx] = (uint8_t)((hi << 4U) | lo);
    }

    return true;
}

static bool session_telnet_pw_auth_lookup(host_t *host, const char *username,
                                          uint8_t *salt_out, size_t salt_len,
                                          uint8_t *hash_out, size_t hash_len)
{
    if (host == nullptr || username == nullptr || username[0] == '\0' ||
        salt_out == nullptr || hash_out == nullptr || salt_len == 0U ||
        hash_len == 0U) {
        return false;
    }

    if (host->pw_auth_file_path[0] == '\0') {
        return false;
    }

    FILE *fp = fopen(host->pw_auth_file_path, "rb");
    if (fp == nullptr) {
        return false;
    }

    char line[SSH_CHATTER_MESSAGE_LIMIT];
    bool found = false;

    while (fgets(line, sizeof(line), fp) != nullptr) {
        size_t length = strcspn(line, "\r\n");
        line[length] = '\0';

        char *first = strchr(line, ':');
        if (first == nullptr) {
            continue;
        }
        char *second = strchr(first + 1, ':');
        if (second == nullptr) {
            continue;
        }

        *first = '\0';
        *second = '\0';

        const char *name = line;
        const char *salt_hex = first + 1;
        const char *hash_hex = second + 1;

        if (name[0] == '\0') {
            continue;
        }

        if (strcasecmp(name, username) != 0) {
            continue;
        }

        uint8_t decoded_salt[SECURITY_LAYER_SALT_LEN];
        uint8_t decoded_hash[SECURITY_LAYER_HASH_LEN];

        if (!session_pw_auth_decode_hex(salt_hex, decoded_salt,
                                        sizeof(decoded_salt)) ||
            !session_pw_auth_decode_hex(hash_hex, decoded_hash,
                                        sizeof(decoded_hash))) {
            continue;
        }

        size_t salt_copy =
            salt_len < sizeof(decoded_salt) ? salt_len : sizeof(decoded_salt);
        size_t hash_copy =
            hash_len < sizeof(decoded_hash) ? hash_len : sizeof(decoded_hash);
        if (salt_copy > 0U) {
            memset(salt_out, 0, salt_len);
            memcpy(salt_out, decoded_salt, salt_copy);
        }
        if (hash_copy > 0U) {
            memset(hash_out, 0, hash_len);
            memcpy(hash_out, decoded_hash, hash_copy);
        }
        found = true;
    }

    int read_error = ferror(fp);
    fclose(fp);

    if (read_error != 0) {
        return false;
    }

    return found;
}

static bool session_telnet_pw_auth_verify(host_t *host, const char *username,
                                          const char *password)
{
    if (host == nullptr || username == nullptr || username[0] == '\0' ||
        password == nullptr) {
        return false;
    }

    uint8_t salt[SECURITY_LAYER_SALT_LEN];
    uint8_t expected[SECURITY_LAYER_HASH_LEN];
    if (!session_telnet_pw_auth_lookup(host, username, salt, sizeof(salt),
                                       expected, sizeof(expected))) {
        return false;
    }

    uint8_t computed[SECURITY_LAYER_HASH_LEN];
    security_layer_hash_password(password, salt, computed);
    return memcmp(computed, expected, sizeof(expected)) == 0;
}

static bool session_telnet_pw_auth_exists(host_t *host, const char *username)
{
    uint8_t salt[SECURITY_LAYER_SALT_LEN];
    uint8_t hash[SECURITY_LAYER_HASH_LEN];
    return session_telnet_pw_auth_lookup(host, username, salt, sizeof(salt),
                                         hash, sizeof(hash));
}

static bool session_telnet_prompt_unicode_check(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return false;
    }

    char resp[2];
    memset(resp, 0, 2);

    while (!ctx->should_exit) {
        static jmp_buf ask_unicode_sanity;
        int ret = 0;
        if (!ret)
            ret = setjmp(ask_unicode_sanity);
        session_send_system_line(ctx, "Does it display fine? >> 한국 << <Y/N>");
        session_channel_write(ctx, "> ", 2U);

        if (!session_telnet_collect_line(ctx, resp, sizeof(resp))) {
            return false;
        }

        trim_whitespace_inplace(resp);

        if (resp[0] == '\0') {
            resp[0] = 'N';
        }

        if (is_pure_ascii(resp)) {
            to_lowercase(resp);
        } else {
            session_send_system_line(ctx, "Type Y/N. No other response.");
            longjmp(ask_unicode_sanity, ret++);
        }

        if (resp[0] == 'n') {
            session_handle_retro(ctx, "on");
            session_handle_set_ui_lang(ctx, "en");
            break;
        } else {
            session_handle_retro(ctx, "off");
            break;
        }
    }

    return session_telnet_login_prompt(ctx);
}

bool session_telnet_login_prompt(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return false;
    }

    char id_buffer[SSH_CHATTER_USERNAME_LEN];
    memset(id_buffer, 0, sizeof(id_buffer));

    while (!ctx->should_exit) {
        session_send_system_line(ctx,
                                 "Enter ID (leave empty to stay as Guest):");
        session_channel_write(ctx, "> ", 2U);

        char input_line[SSH_CHATTER_MESSAGE_LIMIT];
        if (!session_telnet_collect_line(ctx, input_line, sizeof(input_line))) {
            return false;
        }

        trim_whitespace_inplace(input_line);

        if (input_line[0] == '\0') {
            session_send_system_line(
                ctx,
                "Continuing without an ID. Use /register to claim one later.");
            return true;
        }

        const char separators[] = " ,;.";
        char *password_inline = nullptr;
        for (char *cursor = input_line; *cursor != '\0'; ++cursor) {
            if (strchr(separators, *cursor) != nullptr) {
                *cursor = '\0';
                password_inline = cursor + 1;
                break;
            }
        }

        snprintf(id_buffer, sizeof(id_buffer), "%s", input_line);
        trim_whitespace_inplace(id_buffer);
        if (id_buffer[0] == '\0') {
            session_send_system_line(ctx,
                                     "ID must include at least one character.");
            continue;
        }

        char provided_password[128];
        provided_password[0] = '\0';
        if (password_inline != nullptr) {
            snprintf(provided_password, sizeof(provided_password), "%s",
                     password_inline);
        }

        user_data_record_t user_data;
        memset(&user_data, 0, sizeof(user_data));
        const char *ip_for_lookup =
            (ctx->client_ip[0] != '\0') ? ctx->client_ip : nullptr;
        bool user_data_loaded = false;
        bool user_data_has_password = false;

        if (host_user_data_load_existing(ctx->owner, id_buffer, ip_for_lookup,
                                         &user_data, false)) {
            user_data_loaded = true;
            user_data_has_password = !security_layer_is_zero_hash(
                user_data.password_hash, sizeof(user_data.password_hash));
        }

        if (!user_data_has_password) {
            user_data_record_t fallback_data;
            if (host_user_data_load_existing(ctx->owner, id_buffer, nullptr,
                                             &fallback_data, false)) {
                bool fallback_has_password = !security_layer_is_zero_hash(
                    fallback_data.password_hash,
                    sizeof(fallback_data.password_hash));
                if (!user_data_loaded || fallback_has_password) {
                    user_data = fallback_data;
                    user_data_loaded = true;
                }
                if (fallback_has_password) {
                    user_data_has_password = true;
                }
            }
        }

        lan_operator_credential_t *lan_credential = nullptr;
        if (ctx->owner != nullptr) {
            lan_credential =
                host_find_lan_operator_credential(ctx->owner, id_buffer);
            if (lan_credential != nullptr &&
                !session_is_lan_client(ctx->client_ip)) {
                session_send_system_line(
                    ctx, "That nickname is reserved for LAN operators.");
                continue;
            }
        }

        bool pw_auth_available =
            session_telnet_pw_auth_exists(ctx->owner, id_buffer);
        bool requires_password = user_data_has_password || pw_auth_available;

        if (lan_credential != nullptr) {
            requires_password = true;
        }

        const char *password_to_check = nullptr;
        if (provided_password[0] != '\0') {
            password_to_check = provided_password;
        }

        if (!requires_password) {
            snprintf(ctx->user.name, sizeof(ctx->user.name), "%s", id_buffer);
            ctx->user.is_authenticated = true;
            session_send_system_line(ctx, "Login successful.");
            return true;
        }

        if (password_to_check == nullptr) {
            session_send_system_line(ctx, "Password required. Enter password:");
            session_channel_write(ctx, "> ", 2U);

            char password_buffer[128];
            if (!session_telnet_collect_line(ctx, password_buffer,
                                             sizeof(password_buffer))) {
                return false;
            }
            if (password_buffer[0] == '\0') {
                session_send_system_line(ctx, "Password required.");
                continue;
            }
            // Copy into provided_password for later comparisons and make that
            // the canonical pointer for verification. The local
            // password_buffer will go out of scope after this block, so
            // retaining its address would leave password_to_check referencing
            // invalid stack memory.
            snprintf(provided_password, sizeof(provided_password), "%s",
                     password_buffer);
            password_to_check = provided_password;
        }

        if (lan_credential != nullptr) {
            if (lan_credential->password[0] != '\0' &&
                strcmp(lan_credential->password, password_to_check) == 0) {
                snprintf(ctx->user.name, sizeof(ctx->user.name), "%s",
                         lan_credential->nickname);
                ctx->lan_operator_credentials_valid = true;
                ctx->user.is_lan_operator = true;
                ctx->user.is_authenticated = true;
                (void)host_user_data_load_existing(ctx->owner, ctx->user.name,
                                                   ctx->client_ip, &user_data,
                                                   true);
                session_send_system_line(ctx, "Login successful.");
                return true;
            }

            session_send_system_line(ctx, "Invalid password.");
            continue;
        }

        bool authenticated = false;
        if (user_data_loaded && user_data_has_password) {
            uint8_t hashed_password[SECURITY_LAYER_HASH_LEN];
            security_layer_hash_password(
                password_to_check, user_data.password_salt, hashed_password);
            authenticated = memcmp(hashed_password, user_data.password_hash,
                                   sizeof(hashed_password)) == 0;
        }

        if (!authenticated && pw_auth_available) {
            authenticated = session_telnet_pw_auth_verify(ctx->owner, id_buffer,
                                                          password_to_check);

            if (authenticated) {
                (void)host_user_data_load_existing(
                    ctx->owner, id_buffer, ctx->client_ip, &user_data, true);
            }
        }

        if (authenticated) {
            snprintf(ctx->user.name, sizeof(ctx->user.name), "%s", id_buffer);
            ctx->user.is_authenticated = true;
            session_send_system_line(ctx, "Login successful.");
            return true;
        }

        session_send_system_line(ctx, "Invalid password.");
    }

    return false;
}

static int session_transport_read(session_ctx_t *ctx, void *buffer,
                                  size_t length, int timeout_ms)
{
    if (ctx == nullptr || buffer == nullptr || length == 0U) {
        return SSH_ERROR;
    }

    if (ctx->transport_kind == SESSION_TRANSPORT_TELNET) {
        unsigned char *output = (unsigned char *)buffer;
        size_t produced = 0U;

        while (produced < length) {
            unsigned char byte = 0U;
            int read_result = session_telnet_read_byte(ctx, &byte, timeout_ms);
            if (read_result == SSH_AGAIN) {
                if (produced > 0U) {
                    return (int)produced;
                }
                return SSH_AGAIN;
            }
            if (read_result <= 0) {
                if (produced > 0U) {
                    return (int)produced;
                }
                return read_result;
            }

            output[produced++] = byte;
            if (timeout_ms >= 0) {
                break;
            }
        }

        return (int)produced;
    }

    const uint32_t chunk =
        (length > UINT32_MAX) ? UINT32_MAX : (uint32_t)length;

    if (timeout_ms >= 0) {
        return ssh_channel_read_timeout(ctx->channel, buffer, chunk, 0,
                                        timeout_ms);
    }

    return ssh_channel_read(ctx->channel, buffer, chunk, 0);
}

static void session_deliver_outgoing_message(session_ctx_t *ctx,
                                             const char *message,
                                             bool clear_prompt_text)
{
    if (ctx == nullptr || ctx->owner == nullptr || message == nullptr) {
        return;
    }

    // Trim whitespace and check if message is empty
    char trimmed[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(trimmed, sizeof(trimmed), "%s", message);
    trim_whitespace_inplace(trimmed);

    if (trimmed[0] == '\0') {
        // Don't send empty messages
        return;
    }

    chat_history_entry_t entry = {0};
    if (!host_history_record_user(ctx->owner, ctx, trimmed, false, &entry)) {
        return;
    }

    if (ctx->chat_message_count < SIZE_MAX) {
        pthread_mutex_lock(&ctx->chat_message_count_mutex);
        ctx->chat_message_count += 1U;
        pthread_mutex_unlock(&ctx->chat_message_count_mutex);
    }

    // Show the latest MESSAGE_CHUNK of history to the sender
    session_scrollback_reset_position(ctx);

    // Clear screen and show latest chunk of messages
    session_clear_screen(ctx);

    size_t total = host_history_total(ctx->owner);
    size_t chunk_size = SSH_CHATTER_SCROLLBACK_CHUNK;
    if (chunk_size > total) {
        chunk_size = total;
    }

    // Show header
    char header[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(header, sizeof(header), "Latest messages (1-%zu of %zu)",
             chunk_size, total);
    session_send_system_line(ctx, header);

    // Display the latest chunk
    chat_history_entry_t buffer[SSH_CHATTER_SCROLLBACK_CHUNK];
    size_t start_index = (total > chunk_size) ? (total - chunk_size) : 0U;
    size_t copied =
        host_history_copy_range(ctx->owner, start_index, buffer, chunk_size);
    for (size_t i = 0; i < copied; ++i) {
        session_send_history_entry(ctx, &buffer[i]);
    }

    if (ctx->history_scroll_position == 0U && !ctx->bracket_paste_active) {
        if (clear_prompt_text) {
            ctx->input_length = 0U;
            ctx->input_buffer[0] = '\0';
        }
        session_refresh_input_line(ctx);
    }
    chat_room_broadcast_entry(&ctx->owner->room, &entry, ctx);
    host_notify_external_clients(ctx->owner, &entry);

    (void)host_eliza_intervene(ctx, trimmed, nullptr, false);

    size_t message_length = strnlen(trimmed, SSH_CHATTER_MESSAGE_LIMIT);

    if (!host_moderation_queue_chat(ctx, trimmed, message_length)) {
        (void)session_security_check_text(ctx, "chat message", trimmed,
                                          message_length, true);
    }
}

// session_send_line writes a single line while preserving the session's
// background color even when individual strings reset their ANSI attributes by
// clearing the row with the palette tint before printing.
static void session_send_line(session_ctx_t *ctx, const char *message)
{
    if (ctx == nullptr || !session_transport_active(ctx) ||
        message == nullptr) {
        return;
    }

    char buffer[SSH_CHATTER_MESSAGE_LIMIT];
    memset(buffer, 0, sizeof(buffer));
    strncpy(buffer, message, SSH_CHATTER_MESSAGE_LIMIT);
    buffer[SSH_CHATTER_MESSAGE_LIMIT - 1] = '\0';

    char stripped[SSH_CHATTER_MESSAGE_LIMIT];
    bool suppress_translation = translation_strip_no_translate_prefix(
        buffer, stripped, sizeof(stripped));
    const char *render_text = suppress_translation ? stripped : buffer;

    session_write_rendered_line(ctx, render_text);

    bool at_latest = (ctx->history_scroll_position == 0U);
    size_t placeholder_lines = 0U;
    const bool scope_allows_translation =
        (!translator_should_limit_to_chat_bbs() ||
         ctx->translation_manual_scope_override);
    const bool translation_ready =
        scope_allows_translation && !suppress_translation &&
        !ctx->translation_suppress_output && ctx->translation_enabled &&
        ctx->output_translation_enabled &&
        ctx->output_translation_language[0] != '\0' && render_text[0] != '\0';
    if (translation_ready && at_latest && !ctx->in_bbs_mode &&
        !ctx->in_rss_mode) {
        size_t spacing = ctx->translation_caption_spacing;
        if (spacing > 8U) {
            spacing = 8U;
        }
        placeholder_lines = spacing + 1U;
    }

    if (translation_ready && session_translation_queue_caption(
                                 ctx, render_text, placeholder_lines)) {
        if (placeholder_lines > 0U) {
            session_translation_reserve_placeholders(ctx, placeholder_lines);
        }
    }

    session_translation_flush_ready(ctx);
}

static size_t session_append_fragment(char *dest, size_t dest_size,
                                      size_t offset, const char *fragment)
{
    if (dest == nullptr || dest_size == 0U) {
        return offset;
    }

    if (offset >= dest_size) {
        return dest_size > 0U ? dest_size - 1U : offset;
    }

    if (fragment == nullptr) {
        dest[offset] = '\0';
        return offset;
    }

    const size_t fragment_len = strlen(fragment);
    if (fragment_len == 0U) {
        return offset;
    }

    if (offset >= dest_size - 1U) {
        dest[dest_size - 1U] = '\0';
        return dest_size - 1U;
    }

    size_t available = dest_size - offset - 1U;
    if (fragment_len < available) {
        memcpy(dest + offset, fragment, fragment_len);
        offset += fragment_len;
    } else {
        memcpy(dest + offset, fragment, available);
        offset += available;
    }

    dest[offset] = '\0';
    return offset;
}

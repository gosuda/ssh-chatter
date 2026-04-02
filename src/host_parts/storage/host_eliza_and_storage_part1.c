/**
 * @file host_eliza_and_storage.c
 * @desc File-level documentation for host_eliza_and_storage.c, describing its role
 *       in the SSH-Chatter server and providing a consistent header
 *       comment format across C sources.
 * @return None.
 */

// Eliza memory management, BBS persistence, and rendering helpers.
#include "../host_internal.h"

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

#define SESSION_TELNET_INPUT_POLL_MS 1000
#define SESSION_TELNET_IDLE_WARNING_MS (60 * 1000)
#define SESSION_TELNET_IDLE_TIMEOUT_MS (180 * 1000)

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

    ttak_mutex_lock(&host->lock);
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
    ttak_mutex_unlock(&host->lock);
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

    ttak_mutex_lock(&host->lock);
    snapshot_count = host->eliza_memory_count;
    if (snapshot_count > SSH_CHATTER_ELIZA_MEMORY_MAX) {
        snapshot_count = SSH_CHATTER_ELIZA_MEMORY_MAX;
    }
    if (snapshot_count > 0U) {
        memcpy(snapshot, host->eliza_memory,
               snapshot_count * sizeof(snapshot[0]));
    }
    ttak_mutex_unlock(&host->lock);

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

    ttak_mutex_lock(&host->lock);
    size_t capacity = host_bbs_loop_limit(host);
    for (size_t idx = 0U; idx < capacity; ++idx) {
        if (!host->bbs_posts[idx].in_use) {
            continue;
        }

        if (snapshot_count < SSH_CHATTER_BBS_MAX_POSTS) {
            snapshot[snapshot_count++] = host->bbs_posts[idx];
        }
    }
    ttak_mutex_unlock(&host->lock);

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
    fclose(fp);
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

    bbs_post_t *snapshot =
        sshc_gc_calloc(SSH_CHATTER_BBS_MAX_POSTS, sizeof(*snapshot));
    if (snapshot == nullptr) {
        humanized_log_error("bbs", "failed to allocate watchdog snapshot",
                            ENOMEM);
        return;
    }

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
        sshc_gc_free(snapshot);
        return;
    }

    const size_t content_capacity =
        SSH_CHATTER_BBS_BODY_LEN +
        (SSH_CHATTER_BBS_COMMENT_LEN * SSH_CHATTER_BBS_MAX_COMMENTS) + 1024U;
    char *content = (char *)sshc_gc_malloc(content_capacity);
    if (content == nullptr) {
        humanized_log_error("bbs", "failed to allocate watchdog buffer",
                            ENOMEM);
        sshc_gc_free(snapshot);
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

    sshc_gc_free(content);
    sshc_gc_free(snapshot);
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

static void session_refresh_output_encoding(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    /*
     * UTF-16 output is reserved for explicit local console backends only.
     * Network transports (SSH/TELNET) must always remain byte-oriented.
     */
    bool use_utf16 = false;

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

    ctx->prefer_utf16_output = (!use_cp437) && use_utf16;
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
    bool saerom_client = false;

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
                if (string_contains_case_insensitive(candidate, "saerom") ||
                    string_contains_case_insensitive(candidate, "dataman")) {
                    saerom_client = true;
                }
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
        } else if (string_contains_case_insensitive(type, "saerom") ||
                   string_contains_case_insensitive(type, "dataman")) {
            label = "Saerom DataMan";
            identity_label = label;
            detected = true;
            saerom_client = true;
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
        } else if (string_contains_case_insensitive(banner, "saerom") ||
                   string_contains_case_insensitive(banner, "dataman")) {
            label = "Saerom DataMan";
            identity_label = label;
            detected = true;
            saerom_client = true;
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

        if (saerom_client) {
            ctx->cp437_output_scope = SESSION_CP437_SCOPE_SYSTEM_ONLY;
            ctx->hybrid_output_mode = true;
        }
    }

    session_format_telnet_identity(ctx, detected ? identity_label : nullptr);

    ctx->cp437_input_enabled = saerom_client ? false : detected;

    return detected;
}

static void session_apply_user_data_theme(session_ctx_t *ctx,
                                          const user_data_record_t *record)
{
    if (ctx == nullptr || record == nullptr || !record->has_user_theme) {
        return;
    }

    const char *color_code = nullptr;
    const char *highlight_code = nullptr;

    if (record->user_color_code[0] != '\0') {
        color_code = record->user_color_code;
    } else if (record->user_color_name[0] != '\0') {
        color_code = lookup_color_code(
            USER_COLOR_MAP, sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
            record->user_color_name);
    }

    if (record->user_highlight_code[0] != '\0') {
        highlight_code = record->user_highlight_code;
    } else if (record->user_highlight_name[0] != '\0') {
        highlight_code = lookup_color_code(HIGHLIGHT_COLOR_MAP,
                                           sizeof(HIGHLIGHT_COLOR_MAP) /
                                               sizeof(HIGHLIGHT_COLOR_MAP[0]),
                                           record->user_highlight_name);
    }

    if (color_code != nullptr && highlight_code != nullptr) {
        snprintf(ctx->user_color_code, sizeof(ctx->user_color_code), "%s",
                 color_code);
        snprintf(ctx->user_highlight_code, sizeof(ctx->user_highlight_code),
                 "%s", highlight_code);
        ctx->user_is_bold = record->user_is_bold != 0U;
        snprintf(ctx->user_color_name, sizeof(ctx->user_color_name), "%s",
                 record->user_color_name);
        snprintf(ctx->user_highlight_name, sizeof(ctx->user_highlight_name),
                 "%s", record->user_highlight_name);
    }
}

static void session_apply_saved_preferences(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    host_t *host = ctx->owner;
    const bool user_data_loaded = session_user_data_load(ctx);
    const user_data_record_t *user_record =
        user_data_loaded ? &ctx->user_data : nullptr;
    user_preference_t base_snapshot = (user_preference_t){0};
    user_preference_t ip_snapshot = (user_preference_t){0};
    bool has_base_snapshot = false;
    bool has_ip_snapshot = false;
    bool user_theme_applied = false;

    ttak_mutex_lock(&host->lock);
    user_preference_t *pref =
        host_find_preference_locked(host, ctx->user.name, "");
    if (pref != nullptr) {
        base_snapshot = *pref;
        has_base_snapshot = true;
    }
    if (ctx->client_ip[0] != '\0') {
        user_preference_t *ip_pref =
            host_find_preference_locked(host, ctx->user.name, ctx->client_ip);
        if (ip_pref != nullptr && (pref == nullptr || ip_pref != pref)) {
            ip_snapshot = *ip_pref;
            has_ip_snapshot = true;
        }
    }
    ttak_mutex_unlock(&host->lock);

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

    if (has_base_snapshot) {
        if (base_snapshot.ui_language[0] != '\0') {
            session_ui_language_t saved_language =
                session_ui_language_from_code(base_snapshot.ui_language);
            if (saved_language != SESSION_UI_LANGUAGE_COUNT) {
                ctx->ui_language = saved_language;
            }
        }

        if (ctx->ui_language == SESSION_UI_LANGUAGE_COUNT) {
            ctx->ui_language = previous_language;
        }

        if (base_snapshot.has_user_theme) {
            const bool has_custom_color =
                base_snapshot.user_color_code[0] != '\0';
            const bool has_custom_highlight =
                base_snapshot.user_highlight_code[0] != '\0';

            const char *color_code = nullptr;
            if (has_custom_color) {
                color_code = base_snapshot.user_color_code;
            } else {
                color_code = lookup_color_code(USER_COLOR_MAP,
                                               sizeof(USER_COLOR_MAP) /
                                                   sizeof(USER_COLOR_MAP[0]),
                                               base_snapshot.user_color_name);
            }

            const char *highlight_code = nullptr;
            if (has_custom_highlight) {
                highlight_code = base_snapshot.user_highlight_code;
            } else {
                highlight_code =
                    lookup_color_code(HIGHLIGHT_COLOR_MAP,
                                      sizeof(HIGHLIGHT_COLOR_MAP) /
                                          sizeof(HIGHLIGHT_COLOR_MAP[0]),
                                      base_snapshot.user_highlight_name);
            }

            if (color_code != nullptr && highlight_code != nullptr) {
                snprintf(ctx->user_color_code, sizeof(ctx->user_color_code),
                         "%s", color_code);
                snprintf(ctx->user_highlight_code,
                         sizeof(ctx->user_highlight_code), "%s",
                         highlight_code);
                ctx->user_is_bold = base_snapshot.user_is_bold;
                snprintf(ctx->user_color_name, sizeof(ctx->user_color_name),
                         "%s", base_snapshot.user_color_name);
                snprintf(ctx->user_highlight_name,
                         sizeof(ctx->user_highlight_name), "%s",
                         base_snapshot.user_highlight_name);
                user_theme_applied = true;
            }
        }

        if (base_snapshot.has_system_theme) {
            const char *fg_code = lookup_color_code(
                USER_COLOR_MAP,
                sizeof(USER_COLOR_MAP) / sizeof(USER_COLOR_MAP[0]),
                base_snapshot.system_fg_name);
            const char *bg_code = lookup_color_code(
                HIGHLIGHT_COLOR_MAP,
                sizeof(HIGHLIGHT_COLOR_MAP) / sizeof(HIGHLIGHT_COLOR_MAP[0]),
                base_snapshot.system_bg_name);
            if (fg_code != nullptr && bg_code != nullptr) {
                const char *highlight_code = ctx->system_highlight_code;
                if (base_snapshot.system_highlight_name[0] != '\0') {
                    const char *candidate =
                        lookup_color_code(HIGHLIGHT_COLOR_MAP,
                                          sizeof(HIGHLIGHT_COLOR_MAP) /
                                              sizeof(HIGHLIGHT_COLOR_MAP[0]),
                                          base_snapshot.system_highlight_name);
                    if (candidate != nullptr) {
                        highlight_code = candidate;
                    }
                }

                ctx->system_fg_code = fg_code;
                ctx->system_bg_code = bg_code;
                ctx->system_highlight_code = highlight_code;
                ctx->system_is_bold = base_snapshot.system_is_bold;
                snprintf(ctx->system_fg_name, sizeof(ctx->system_fg_name), "%s",
                         base_snapshot.system_fg_name);
                snprintf(ctx->system_bg_name, sizeof(ctx->system_bg_name), "%s",
                         base_snapshot.system_bg_name);
                if (base_snapshot.system_highlight_name[0] != '\0') {
                    snprintf(ctx->system_highlight_name,
                             sizeof(ctx->system_highlight_name), "%s",
                             base_snapshot.system_highlight_name);
                }
            }
        }

        if (base_snapshot.os_name[0] != '\0') {
            snprintf(ctx->os_name, sizeof(ctx->os_name), "%s",
                     base_snapshot.os_name);
        }
        ctx->daily_year = base_snapshot.daily_year;
        ctx->daily_yday = base_snapshot.daily_yday;
        if (base_snapshot.daily_function[0] != '\0') {
            snprintf(ctx->daily_function, sizeof(ctx->daily_function), "%s",
                     base_snapshot.daily_function);
        }
        ctx->has_birthday = base_snapshot.has_birthday;
        if (ctx->has_birthday) {
            snprintf(ctx->birthday, sizeof(ctx->birthday), "%s",
                     base_snapshot.birthday);
        } else {
            ctx->birthday[0] = '\0';
        }

        ctx->translation_caption_spacing =
            base_snapshot.translation_caption_spacing;
        if (ctx->translation_caption_spacing > 8U) {
            ctx->translation_caption_spacing = 8U;
        }

        if (base_snapshot.translation_master_explicit) {
            ctx->translation_enabled = base_snapshot.translation_master_enabled;
        }

        ctx->output_translation_enabled =
            base_snapshot.output_translation_enabled;
        snprintf(ctx->output_translation_language,
                 sizeof(ctx->output_translation_language), "%s",
                 base_snapshot.output_translation_language);
        ctx->input_translation_enabled =
            base_snapshot.input_translation_enabled;
        snprintf(ctx->input_translation_language,
                 sizeof(ctx->input_translation_language), "%s",
                 base_snapshot.input_translation_language);
        ctx->breaking_alerts_enabled = base_snapshot.breaking_alerts_enabled;
        snprintf(ctx->game.chosen_camouflage_language,
                 sizeof(ctx->game.chosen_camouflage_language), "%s",
                 base_snapshot.camouflage_language);
    }

    if (!user_theme_applied && user_record != nullptr) {
        session_apply_user_data_theme(ctx, user_record);
    }

    if (has_ip_snapshot && ip_snapshot.ui_language[0] != '\0') {
        session_ui_language_t saved_language =
            session_ui_language_from_code(ip_snapshot.ui_language);
        if (saved_language != SESSION_UI_LANGUAGE_COUNT) {
            ctx->ui_language = saved_language;
        }
    }

    if (!has_base_snapshot && !has_ip_snapshot) {
        ctx->ui_language = previous_language;
    } else if (ctx->ui_language == SESSION_UI_LANGUAGE_COUNT) {
        ctx->ui_language = previous_language;
    }

    ctx->cp437_override = previous_cp437_override;
    ctx->prefer_cp437_output = previous_cp437_output;
    ctx->cp437_input_enabled = previous_cp437_input;

    session_refresh_output_encoding(ctx);

    if (!user_data_loaded) {
        (void)session_user_data_load(ctx);
    }
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
    translation_job_t *job = (translation_job_t *)sshc_gc_malloc(sizeof(*job));
    if (job != nullptr) {
        memset(job, 0, sizeof(*job));
    }
    return job;
}

static translation_result_t *session_translation_result_alloc(void)
{
    translation_result_t *result =
        (translation_result_t *)sshc_gc_malloc(sizeof(*result));
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
        if (ttak_mutex_init(&ctx->translation_mutex) != 0) {
            return false;
        }
        ctx->translation_mutex_initialized = true;
    }

    if (!ctx->translation_cond_initialized) {
        if (ttak_cond_init(&ctx->translation_cond) != 0) {
            ttak_mutex_destroy(&ctx->translation_mutex);
            ctx->translation_mutex_initialized = false;
            return false;
        }
        ctx->translation_cond_initialized = true;
    }

    if (!ctx->translation_thread_started) {
        ctx->translation_thread_stop = false;
        if (pthread_create(&ctx->translation_thread, nullptr,
                           session_translation_worker, ctx) != 0) {
            ttak_cond_destroy(&ctx->translation_cond);
            ctx->translation_cond_initialized = false;
            ttak_mutex_destroy(&ctx->translation_mutex);
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

    ttak_mutex_lock(&ctx->translation_mutex);
    pending = ctx->translation_pending_head;
    ctx->translation_pending_head = nullptr;
    ctx->translation_pending_tail = nullptr;
    ready = ctx->translation_ready_head;
    ctx->translation_ready_head = nullptr;
    ctx->translation_ready_tail = nullptr;
    ttak_mutex_unlock(&ctx->translation_mutex);

    while (pending != nullptr) {
        translation_job_t *next = pending->next;
        sshc_gc_free(pending);
        pending = next;
    }

    while (ready != nullptr) {
        translation_result_t *next = ready->next;
        sshc_gc_free(ready);
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

    ttak_mutex_lock(&ctx->translation_mutex);
    job->next = nullptr;
    if (ctx->translation_pending_tail != nullptr) {
        ctx->translation_pending_tail->next = job;
    } else {
        ctx->translation_pending_head = job;
    }
    ctx->translation_pending_tail = job;
    ttak_cond_signal(&ctx->translation_cond);
    ttak_mutex_unlock(&ctx->translation_mutex);

    return true;
}

static void session_translation_reserve_placeholders(session_ctx_t *ctx,
                                                     size_t placeholder_lines)
{
    if (ctx == nullptr || !session_transport_active(ctx) ||
        placeholder_lines == 0U) {
        return;
    }

    session_output_kind_t previous_kind =
        session_output_set_kind(ctx, SESSION_OUTPUT_KIND_CHAT);

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

    session_output_restore_kind(ctx, previous_kind);
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

    ttak_mutex_lock(&ctx->translation_mutex);
    job->next = nullptr;
    if (ctx->translation_pending_tail != nullptr) {
        ctx->translation_pending_tail->next = job;
    } else {
        ctx->translation_pending_head = job;
    }
    ctx->translation_pending_tail = job;
    ttak_cond_signal(&ctx->translation_cond);
    ttak_mutex_unlock(&ctx->translation_mutex);

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

    ttak_mutex_lock(&ctx->translation_mutex);
    job->next = nullptr;
    if (ctx->translation_pending_tail != nullptr) {
        ctx->translation_pending_tail->next = job;
    } else {
        ctx->translation_pending_head = job;
    }
    ctx->translation_pending_tail = job;
    ttak_cond_signal(&ctx->translation_cond);
    ttak_mutex_unlock(&ctx->translation_mutex);

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

    ttak_mutex_lock(&host->lock);
    snprintf(host->motd_base, sizeof(host->motd_base), "%s", updated);
    host_refresh_motd_locked(host);
    ttak_mutex_unlock(&host->lock);
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

    ttak_mutex_lock(&host->lock);
    if (host->translation_quota_exhausted) {
        already_marked = true;
    } else {
        host->translation_quota_exhausted = true;
        if (host->motd_has_file && host->motd_path[0] != '\0') {
            snprintf(motd_path, sizeof(motd_path), "%s", host->motd_path);
        }
        snprintf(motd_snapshot, sizeof(motd_snapshot), "%s", host->motd_base);
    }
    ttak_mutex_unlock(&host->lock);

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

    ttak_mutex_lock(&ctx->translation_mutex);
    ready = ctx->translation_ready_head;
    ctx->translation_ready_head = nullptr;
    ctx->translation_ready_tail = nullptr;
    ttak_mutex_unlock(&ctx->translation_mutex);

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
        ttak_mutex_lock(&ctx->translation_mutex);
        if (ctx->translation_thread_started) {
            ctx->translation_thread_stop = true;
            ttak_cond_broadcast(&ctx->translation_cond);
            ttak_mutex_unlock(&ctx->translation_mutex);
            pthread_join(ctx->translation_thread, nullptr);
            ctx->translation_thread_started = false;
        } else {
            ttak_mutex_unlock(&ctx->translation_mutex);
        }
    }

    session_translation_clear_queue(ctx);

    if (ctx->translation_cond_initialized) {
        ttak_cond_destroy(&ctx->translation_cond);
        ctx->translation_cond_initialized = false;
    }
    if (ctx->translation_mutex_initialized) {
        ttak_mutex_destroy(&ctx->translation_mutex);
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
                message = "[!] translation unavailable.";
            }
        }

        snprintf(result->translated, sizeof(result->translated), "%s", message);
        session_translation_normalize_output(result->translated);
        result->detected_language[0] = '\0';
        result->error_message[0] = '\0';
        result->original[0] = '\0';
    }

    ttak_mutex_lock(&ctx->translation_mutex);
    result->next = nullptr;
    if (ctx->translation_ready_tail != nullptr) {
        ctx->translation_ready_tail->next = result;
    } else {
        ctx->translation_ready_head = result;
    }
    ctx->translation_ready_tail = result;
    ttak_mutex_unlock(&ctx->translation_mutex);
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
                             "[!] translation unavailable (quota exhausted: %s); "
                             "sending "
                             "your original message.",
                             error);
                } else {
                    snprintf(message, sizeof(message),
                             "[!] translation unavailable (quota exhausted); "
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
                             "[!] translation unavailable (quota exhausted: %s)",
                             error);
                } else {
                    snprintf(failure_message, sizeof(failure_message),
                             "[!] translation unavailable (quota exhausted).");
                }
                session_handle_translation_quota_exhausted(ctx, error);
                break;
            }

            if (error != nullptr && error[0] != '\0') {
                snprintf(failure_message, sizeof(failure_message),
                         "[!] translation failed: %s", error);
            } else {

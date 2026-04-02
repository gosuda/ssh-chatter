/**
 * @file host_eliza_and_storage.c
 * @desc File-level documentation for host_eliza_and_storage.c, describing its role
 *       in the SSH-Chatter server and providing a consistent header
 *       comment format across C sources.
 * @return None.
 */

// Eliza memory management, BBS persistence, and rendering helpers.
#include "../host_internal.h"
#include <sys/mman.h>

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

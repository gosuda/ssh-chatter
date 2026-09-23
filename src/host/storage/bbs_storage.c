
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
    const bbs_state_post_entry_disk_t *serialized)
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

static bool host_bbs_serialized_is_sane(const bbs_state_post_entry_disk_t *serialized,
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

static void host_bbs_boards_save_locked(host_t *host);
static void host_bbs_votes_save_locked(host_t *host);
static void host_bbs_drafts_save_locked(host_t *host);
static void host_bbs_notifications_save_locked(host_t *host);
static void host_bbs_readmarks_save_locked(host_t *host);
static void host_bbs_reports_save_locked(host_t *host);
static void host_bbs_mutes_save_locked(host_t *host);
static void host_bbs_modlog_save_locked(host_t *host);
static void host_bbs_ipaudit_save_locked(host_t *host);
static void host_ipaudit_sweep_locked(host_t *host, time_t now);
static void host_bbs_boards_load(host_t *host);
static void host_bbs_votes_load(host_t *host);
static void host_bbs_drafts_load(host_t *host);
static void host_bbs_notifications_load(host_t *host);
static void host_bbs_readmarks_load(host_t *host);
static void host_bbs_reports_load(host_t *host);
static void host_bbs_mutes_load(host_t *host);
static void host_bbs_modlog_load(host_t *host);
static void host_bbs_ipaudit_load(host_t *host);

/* The boards/votes/drafts arrays are host-owned, but the loaders run on
 * session threads where the current memory context may belong to the
 * session.  Pin the loads to the host memory context so the arrays are not
 * reclaimed when the calling session's context is collected (this was the
 * SIGSEGV in session_bbs_list: host->bbs_boards dangled). */
static void host_bbs_aux_loads(host_t *host)
{
    sshc_memory_context_t *prev_ctx =
        sshc_memory_context_push(host->memory_context);
    host_bbs_boards_load(host);
    host_bbs_votes_load(host);
    host_bbs_drafts_load(host);
    host_bbs_notifications_load(host);
    host_bbs_readmarks_load(host);
    host_bbs_reports_load(host);
    host_bbs_mutes_load(host);
    host_bbs_modlog_load(host);
    host_bbs_ipaudit_load(host);
    sshc_memory_context_pop(prev_ctx);
}

/* Best-effort time-based backup rotation for the main BBS state file.
 * Never propagates failure: the primary save has already succeeded by the
 * time this runs, so any error here simply skips the backup. */
static time_t host_bbs_last_backup_time;

static int host_bbs_backup_name_compare(const void *left, const void *right)
{
    const char *const *a = (const char *const *)left;
    const char *const *b = (const char *const *)right;
    return strcmp(*a, *b);
}

static void host_bbs_backup_rotate_locked(host_t *host)
{
    if (host == nullptr || host->bbs_state_file_path[0] == '\0') {
        return;
    }

    time_t now = time(nullptr);
    if (now <= 0) {
        return;
    }
    if (host_bbs_last_backup_time != 0 &&
        now - host_bbs_last_backup_time < 3600) {
        return;
    }

    char dir[PATH_MAX];
    const char *slash = strrchr(host->bbs_state_file_path, '/');
    if (slash != nullptr) {
        size_t dir_len = (size_t)(slash - host->bbs_state_file_path);
        if (dir_len == 0U) {
            dir_len = 1U; /* root directory "/" */
        }
        if (dir_len >= sizeof(dir)) {
            return;
        }
        memcpy(dir, host->bbs_state_file_path, dir_len);
        dir[dir_len] = '\0';
    } else {
        snprintf(dir, sizeof(dir), ".");
    }

    char stamp[32];
    struct tm backup_tm;
    if (localtime_r(&now, &backup_tm) == nullptr) {
        return;
    }
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &backup_tm);

    char backup_path[PATH_MAX];
    int written = snprintf(backup_path, sizeof(backup_path),
                           "%s/bbs_backup_%s.dat", dir, stamp);
    if (written < 0 || (size_t)written >= sizeof(backup_path)) {
        return;
    }

    FILE *source = fopen(host->bbs_state_file_path, "rb");
    if (source == nullptr) {
        return;
    }
    FILE *target = fopen(backup_path, "wb");
    if (target == nullptr) {
        fclose(source);
        return;
    }
    chmod(backup_path, S_IRUSR | S_IWUSR);

    char copy_buffer[8192];
    size_t chunk;
    bool copy_ok = true;
    while ((chunk = fread(copy_buffer, 1U, sizeof(copy_buffer), source)) >
           0U) {
        if (fwrite(copy_buffer, 1U, chunk, target) != chunk) {
            copy_ok = false;
            break;
        }
    }
    if (ferror(source)) {
        copy_ok = false;
    }
    if (fclose(target) != 0) {
        copy_ok = false;
    }
    fclose(source);

    if (!copy_ok) {
        unlink(backup_path);
        return;
    }
    host_bbs_last_backup_time = now;

    // Prune older backups, keeping the newest 7.  Names are timestamped so
    // lexicographic order matches chronological order.
    DIR *directory = opendir(dir);
    if (directory == nullptr) {
        return;
    }
    char *names[64];
    size_t name_count = 0U;
    struct dirent *entry;
    while ((entry = readdir(directory)) != nullptr && name_count < 64U) {
        if (strncmp(entry->d_name, "bbs_backup_", 11) != 0) {
            continue;
        }
        size_t name_len = strlen(entry->d_name);
        if (name_len < 4U ||
            strcmp(entry->d_name + name_len - 4U, ".dat") != 0) {
            continue;
        }
        names[name_count] = strdup(entry->d_name);
        if (names[name_count] != nullptr) {
            ++name_count;
        }
    }
    closedir(directory);

    qsort(names, name_count, sizeof(names[0]),
          host_bbs_backup_name_compare);

    const size_t keep = 7U;
    size_t delete_count =
        name_count > keep ? name_count - keep : 0U;
    for (size_t idx = 0U; idx < delete_count; ++idx) {
        char old_path[PATH_MAX];
        int old_written =
            snprintf(old_path, sizeof(old_path), "%s/%s", dir, names[idx]);
        if (old_written > 0 && (size_t)old_written < sizeof(old_path)) {
            unlink(old_path);
        }
    }
    for (size_t idx = 0U; idx < name_count; ++idx) {
        free(names[idx]);
    }
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

        bbs_state_post_entry_disk_t serialized = {0};
        serialized.id = post->id;
        serialized.board_id = post->board_id;
        serialized.mod_flags = post->mod_flags;
        serialized.created_at = (int64_t)post->created_at;
        serialized.bumped_at = (int64_t)post->bumped_at;
        serialized.upvotes = post->upvotes;
        serialized.downvotes = post->downvotes;
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
            serialized.comments[comment].edited_at =
                (int64_t)post->comments[comment].edited_at;
            serialized.comments[comment].upvotes = post->comments[comment].upvotes;
            serialized.comments[comment].downvotes = post->comments[comment].downvotes;
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
        return;
    } else if (chmod(host->bbs_state_file_path, S_IRUSR | S_IWUSR) != 0) {
        humanized_log_error("host", "failed to tighten bbs state permissions",
                            errno != 0 ? errno : EACCES);
    }

    host_bbs_backup_rotate_locked(host);

    host_bbs_boards_save_locked(host);
    host_bbs_votes_save_locked(host);
    host_bbs_drafts_save_locked(host);
    host_bbs_notifications_save_locked(host);
    host_bbs_readmarks_save_locked(host);
    host_bbs_reports_save_locked(host);
    host_bbs_mutes_save_locked(host);
    host_bbs_modlog_save_locked(host);
    host_ipaudit_sweep_locked(host, time(nullptr));
    host_bbs_ipaudit_save_locked(host);
}

/* ------------------------------------------------------------------ */
/* BBS v2 state helpers                                               */
/* ------------------------------------------------------------------ */

static void host_bbs_v2_path(const char *base_path, const char *suffix,
                             char *out, size_t out_size)
{
    if (base_path == nullptr || out == nullptr || out_size == 0U) {
        return;
    }
    size_t base_len = strlen(base_path);
    if (base_len > 4 && strcmp(base_path + base_len - 4, ".dat") == 0) {
        snprintf(out, out_size, "%.*s_%s",
                 (int)(base_len - 4), base_path, suffix);
    } else {
        snprintf(out, out_size, "%s_%s", base_path, suffix);
    }
}

static bool host_bbs_file_exists(const char *path)
{
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}



static void host_bbs_boards_save_locked(host_t *host)
{
    if (host == nullptr || host->bbs_state_file_path[0] == '\0') {
        return;
    }
    char path[PATH_MAX];
    host_bbs_v2_path(host->bbs_state_file_path, "boards.dat", path, sizeof(path));
    char temp_path[PATH_MAX + 16];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    FILE *fp = fdopen(fd, "wb");
    if (fp == nullptr) { close(fd); return; }

    uint32_t magic = BBS_BOARDS_MAGIC;
    uint32_t version = BBS_BOARDS_VERSION;
    uint32_t count = (uint32_t)host->bbs_board_count;
    uint32_t reserved = 0U;
    fwrite(&magic, sizeof(magic), 1, fp);
    fwrite(&version, sizeof(version), 1, fp);
    fwrite(&count, sizeof(count), 1, fp);
    fwrite(&reserved, sizeof(reserved), 1, fp);

    for (size_t i = 0; i < host->bbs_board_count; ++i) {
        bbs_state_board_entry_t entry = {0};
        entry.board_id = host->bbs_boards[i].board_id;
        entry.is_notice = host->bbs_boards[i].is_notice ? 1 : 0;
        snprintf(entry.name, sizeof(entry.name), "%s", host->bbs_boards[i].name);
        snprintf(entry.description, sizeof(entry.description), "%s", host->bbs_boards[i].description);
        fwrite(&entry, sizeof(entry), 1, fp);
    }
    fflush(fp);
    fsync(fd);
    fclose(fp);
    chmod(temp_path, S_IRUSR | S_IWUSR);
    rename(temp_path, path);
    chmod(path, S_IRUSR | S_IWUSR);
}

static void host_bbs_boards_load(host_t *host)
{
    if (host == nullptr || host->bbs_state_file_path[0] == '\0') {
        return;
    }
    char path[PATH_MAX];
    host_bbs_v2_path(host->bbs_state_file_path, "boards.dat", path, sizeof(path));
    if (!host_bbs_file_exists(path)) {
        /* initialize default boards */
        host->bbs_boards = sshc_gc_calloc(SSH_CHATTER_BBS_MAX_BOARDS, sizeof(bbs_board_t));
        if (host->bbs_boards != nullptr) {
            host->bbs_board_capacity = SSH_CHATTER_BBS_MAX_BOARDS;
            host->bbs_board_count = 4;
            host->bbs_boards[0] = (bbs_board_t){0, "general", "General discussion", false};
            host->bbs_boards[1] = (bbs_board_t){1, "notice", "Notices and announcements", true};
            host->bbs_boards[2] = (bbs_board_t){2, "qna", "Questions and answers", false};
            host->bbs_boards[3] = (bbs_board_t){3, "games", "Game-related posts", false};
            host_bbs_boards_save_locked(host);
        }
        return;
    }
    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) return;
    uint32_t magic, version, count, reserved;
    if (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != BBS_BOARDS_MAGIC ||
        fread(&version, sizeof(version), 1, fp) != 1 || version != BBS_BOARDS_VERSION ||
        fread(&count, sizeof(count), 1, fp) != 1 ||
        fread(&reserved, sizeof(reserved), 1, fp) != 1) {
        fclose(fp);
        return;
    }
    size_t cap = count > SSH_CHATTER_BBS_MAX_BOARDS ? count : SSH_CHATTER_BBS_MAX_BOARDS;
    host->bbs_boards = sshc_gc_calloc(cap, sizeof(bbs_board_t));
    if (host->bbs_boards == nullptr) {
        fclose(fp);
        return;
    }
    host->bbs_board_capacity = cap;
    host->bbs_board_count = 0;
    for (size_t i = 0; i < count; ++i) {
        bbs_state_board_entry_t entry;
        if (fread(&entry, sizeof(entry), 1, fp) != 1) break;
        entry.name[sizeof(entry.name) - 1] = '\0';
        entry.description[sizeof(entry.description) - 1] = '\0';
        if (host->bbs_board_count >= cap) break;
        bbs_board_t *b = &host->bbs_boards[host->bbs_board_count++];
        b->board_id = entry.board_id;
        b->is_notice = entry.is_notice != 0;
        snprintf(b->name, sizeof(b->name), "%s", entry.name);
        snprintf(b->description, sizeof(b->description), "%s", entry.description);
    }
    fclose(fp);
}

static void host_bbs_votes_save_locked(host_t *host)
{
    if (host == nullptr || host->bbs_state_file_path[0] == '\0') {
        return;
    }
    char path[PATH_MAX];
    host_bbs_v2_path(host->bbs_state_file_path, "votes.dat", path, sizeof(path));
    char temp_path[PATH_MAX + 16];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    FILE *fp = fdopen(fd, "wb");
    if (fp == nullptr) { close(fd); return; }

    uint32_t magic = BBS_VOTES_MAGIC;
    uint32_t version = BBS_VOTES_VERSION;
    uint32_t count = (uint32_t)host->bbs_vote_count;
    uint32_t reserved = 0U;
    fwrite(&magic, sizeof(magic), 1, fp);
    fwrite(&version, sizeof(version), 1, fp);
    fwrite(&count, sizeof(count), 1, fp);
    fwrite(&reserved, sizeof(reserved), 1, fp);

    for (size_t i = 0; i < host->bbs_vote_count; ++i) {
        bbs_state_vote_entry_t entry = {0};
        entry.target_post_id = host->bbs_votes[i].target_post_id;
        entry.target_comment_idx = host->bbs_votes[i].target_comment_idx;
        entry.vote_type = host->bbs_votes[i].vote_type;
        entry.created_at = (int64_t)host->bbs_votes[i].created_at;
        snprintf(entry.voter_username, sizeof(entry.voter_username), "%s", host->bbs_votes[i].voter_username);
        fwrite(&entry, sizeof(entry), 1, fp);
    }
    fflush(fp);
    fsync(fd);
    fclose(fp);
    chmod(temp_path, S_IRUSR | S_IWUSR);
    rename(temp_path, path);
    chmod(path, S_IRUSR | S_IWUSR);
}

static void host_bbs_votes_load(host_t *host)
{
    if (host == nullptr || host->bbs_state_file_path[0] == '\0') {
        return;
    }
    char path[PATH_MAX];
    host_bbs_v2_path(host->bbs_state_file_path, "votes.dat", path, sizeof(path));
    if (!host_bbs_file_exists(path)) {
        host->bbs_votes = sshc_gc_calloc(SSH_CHATTER_BBS_MAX_VOTES, sizeof(bbs_vote_t));
        if (host->bbs_votes != nullptr) {
            host->bbs_vote_capacity = SSH_CHATTER_BBS_MAX_VOTES;
            host->bbs_vote_count = 0;
        }
        return;
    }
    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) return;
    uint32_t magic, version, count, reserved;
    if (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != BBS_VOTES_MAGIC ||
        fread(&version, sizeof(version), 1, fp) != 1 || version != BBS_VOTES_VERSION ||
        fread(&count, sizeof(count), 1, fp) != 1 ||
        fread(&reserved, sizeof(reserved), 1, fp) != 1) {
        fclose(fp);
        return;
    }
    size_t cap = count > SSH_CHATTER_BBS_MAX_VOTES ? count : SSH_CHATTER_BBS_MAX_VOTES;
    host->bbs_votes = sshc_gc_calloc(cap, sizeof(bbs_vote_t));
    if (host->bbs_votes == nullptr) {
        fclose(fp);
        return;
    }
    host->bbs_vote_capacity = cap;
    host->bbs_vote_count = 0;
    for (size_t i = 0; i < count; ++i) {
        bbs_state_vote_entry_t entry;
        if (fread(&entry, sizeof(entry), 1, fp) != 1) break;
        entry.voter_username[sizeof(entry.voter_username) - 1] = '\0';
        if (host->bbs_vote_count >= cap) break;
        bbs_vote_t *v = &host->bbs_votes[host->bbs_vote_count++];
        v->target_post_id = entry.target_post_id;
        v->target_comment_idx = entry.target_comment_idx;
        v->vote_type = entry.vote_type;
        v->created_at = (time_t)entry.created_at;
        snprintf(v->voter_username, sizeof(v->voter_username), "%s", entry.voter_username);
    }
    fclose(fp);
}

static void host_bbs_drafts_save_locked(host_t *host)
{
    if (host == nullptr || host->bbs_state_file_path[0] == '\0') {
        return;
    }
    char path[PATH_MAX];
    host_bbs_v2_path(host->bbs_state_file_path, "drafts.dat", path, sizeof(path));
    char temp_path[PATH_MAX + 16];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    FILE *fp = fdopen(fd, "wb");
    if (fp == nullptr) { close(fd); return; }

    uint32_t magic = BBS_DRAFTS_MAGIC;
    uint32_t version = BBS_DRAFTS_VERSION;
    uint32_t count = (uint32_t)host->bbs_draft_count;
    uint32_t reserved = 0U;
    fwrite(&magic, sizeof(magic), 1, fp);
    fwrite(&version, sizeof(version), 1, fp);
    fwrite(&count, sizeof(count), 1, fp);
    fwrite(&reserved, sizeof(reserved), 1, fp);

    for (size_t i = 0; i < host->bbs_draft_count; ++i) {
        bbs_state_draft_entry_t entry = {0};
        entry.id = host->bbs_drafts[i].id;
        entry.board_id = host->bbs_drafts[i].board_id;
        entry.created_at = (int64_t)host->bbs_drafts[i].created_at;
        entry.tag_count = (uint32_t)host->bbs_drafts[i].tag_count;
        snprintf(entry.author, sizeof(entry.author), "%s", host->bbs_drafts[i].author);
        snprintf(entry.title, sizeof(entry.title), "%s", host->bbs_drafts[i].title);
        snprintf(entry.body, sizeof(entry.body), "%s", host->bbs_drafts[i].body);
        for (size_t t = 0; t < SSH_CHATTER_BBS_MAX_TAGS; ++t) {
            snprintf(entry.tags[t], sizeof(entry.tags[t]), "%s", host->bbs_drafts[i].tags[t]);
        }
        fwrite(&entry, sizeof(entry), 1, fp);
    }
    fflush(fp);
    fsync(fd);
    fclose(fp);
    chmod(temp_path, S_IRUSR | S_IWUSR);
    rename(temp_path, path);
    chmod(path, S_IRUSR | S_IWUSR);
}

static void host_bbs_drafts_load(host_t *host)
{
    if (host == nullptr || host->bbs_state_file_path[0] == '\0') {
        return;
    }
    char path[PATH_MAX];
    host_bbs_v2_path(host->bbs_state_file_path, "drafts.dat", path, sizeof(path));
    if (!host_bbs_file_exists(path)) {
        host->bbs_drafts = sshc_gc_calloc(SSH_CHATTER_BBS_MAX_DRAFTS_PER_USER, sizeof(bbs_draft_t));
        if (host->bbs_drafts != nullptr) {
            host->bbs_draft_capacity = SSH_CHATTER_BBS_MAX_DRAFTS_PER_USER;
            host->bbs_draft_count = 0;
        }
        return;
    }
    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) return;
    uint32_t magic, version, count, reserved;
    if (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != BBS_DRAFTS_MAGIC ||
        fread(&version, sizeof(version), 1, fp) != 1 || version != BBS_DRAFTS_VERSION ||
        fread(&count, sizeof(count), 1, fp) != 1 ||
        fread(&reserved, sizeof(reserved), 1, fp) != 1) {
        fclose(fp);
        return;
    }
    size_t cap = count > SSH_CHATTER_BBS_MAX_DRAFTS_PER_USER ? count : SSH_CHATTER_BBS_MAX_DRAFTS_PER_USER;
    host->bbs_drafts = sshc_gc_calloc(cap, sizeof(bbs_draft_t));
    if (host->bbs_drafts == nullptr) {
        fclose(fp);
        return;
    }
    host->bbs_draft_capacity = cap;
    host->bbs_draft_count = 0;
    for (size_t i = 0; i < count; ++i) {
        bbs_state_draft_entry_t entry;
        if (fread(&entry, sizeof(entry), 1, fp) != 1) break;
        entry.author[sizeof(entry.author) - 1] = '\0';
        entry.title[sizeof(entry.title) - 1] = '\0';
        entry.body[sizeof(entry.body) - 1] = '\0';
        for (size_t t = 0; t < SSH_CHATTER_BBS_MAX_TAGS; ++t) {
            entry.tags[t][sizeof(entry.tags[t]) - 1] = '\0';
        }
        if (host->bbs_draft_count >= cap) break;
        bbs_draft_t *d = &host->bbs_drafts[host->bbs_draft_count++];
        d->in_use = true;
        d->id = entry.id;
        d->board_id = entry.board_id;
        d->created_at = (time_t)entry.created_at;
        d->tag_count = entry.tag_count;
        snprintf(d->author, sizeof(d->author), "%s", entry.author);
        snprintf(d->title, sizeof(d->title), "%s", entry.title);
        snprintf(d->body, sizeof(d->body), "%s", entry.body);
        for (size_t t = 0; t < SSH_CHATTER_BBS_MAX_TAGS; ++t) {
            snprintf(d->tags[t], sizeof(d->tags[t]), "%s", entry.tags[t]);
        }
    }
    fclose(fp);
}

/* ------------------------------------------------------------------ */
/* Notification queue (text sidecar, one entry per line)              */
/* ------------------------------------------------------------------ */

static void host_bbs_notifications_save_locked(host_t *host)
{
    if (host == nullptr || host->bbs_state_file_path[0] == '\0') {
        return;
    }
    char path[PATH_MAX];
    host_bbs_v2_path(host->bbs_state_file_path, "notifications.dat", path,
                     sizeof(path));
    char temp_path[PATH_MAX + 16];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return;
    }
    FILE *fp = fdopen(fd, "wb");
    if (fp == nullptr) {
        close(fd);
        return;
    }

    for (size_t i = 0U; i < host->bbs_notification_count; ++i) {
        const bbs_notification_t *n = &host->bbs_notifications[i];
        fprintf(fp, "%d|%s|%s|%" PRIu64 "|%d|%lld\n", (int)n->kind, n->to,
                n->from, n->post_id, (int)n->comment_idx,
                (long long)n->created_at);
    }

    fflush(fp);
    fsync(fd);
    fclose(fp);
    chmod(temp_path, S_IRUSR | S_IWUSR);
    rename(temp_path, path);
    chmod(path, S_IRUSR | S_IWUSR);
}

static void host_bbs_notifications_load(host_t *host)
{
    if (host == nullptr || host->bbs_state_file_path[0] == '\0') {
        return;
    }
    if (host->bbs_notifications == nullptr) {
        host->bbs_notifications =
            sshc_gc_calloc(SSH_CHATTER_BBS_MAX_NOTIFICATIONS,
                           sizeof(bbs_notification_t));
        if (host->bbs_notifications == nullptr) {
            return;
        }
        host->bbs_notification_capacity = SSH_CHATTER_BBS_MAX_NOTIFICATIONS;
        host->bbs_notification_count = 0U;
    }

    char path[PATH_MAX];
    host_bbs_v2_path(host->bbs_state_file_path, "notifications.dat", path,
                     sizeof(path));
    if (!host_bbs_file_exists(path)) {
        return;
    }
    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        return;
    }

    // Read all lines, then keep the newest cap: on overflow the oldest
    // entries are dropped.
    bbs_notification_t incoming[SSH_CHATTER_BBS_MAX_NOTIFICATIONS];
    size_t incoming_count = 0U;
    char line[256];
    while (fgets(line, sizeof(line), fp) != nullptr) {
        char *fields[6] = {nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr};
        char *cursor = line;
        size_t field_count = 0U;
        while (field_count < 6U) {
            fields[field_count++] = cursor;
            char *sep = strchr(cursor, '|');
            if (sep == nullptr) {
                break;
            }
            *sep = '\0';
            cursor = sep + 1;
        }
        if (field_count < 6U || fields[0] == nullptr || fields[1] == nullptr ||
            fields[2] == nullptr || fields[3] == nullptr ||
            fields[4] == nullptr || fields[5] == nullptr) {
            continue;
        }
        bbs_notification_t entry = {0};
        entry.kind = (int32_t)strtol(fields[0], nullptr, 10);
        snprintf(entry.to, sizeof(entry.to), "%s", fields[1]);
        snprintf(entry.from, sizeof(entry.from), "%s", fields[2]);
        entry.post_id = (uint64_t)strtoull(fields[3], nullptr, 10);
        entry.comment_idx = (int32_t)strtol(fields[4], nullptr, 10);
        entry.created_at = (time_t)strtoll(fields[5], nullptr, 10);
        if (incoming_count >= SSH_CHATTER_BBS_MAX_NOTIFICATIONS) {
            memmove(incoming, incoming + 1,
                    (incoming_count - 1U) * sizeof(incoming[0]));
            --incoming_count;
        }
        incoming[incoming_count++] = entry;
    }
    fclose(fp);

    host->bbs_notification_count = 0U;
    for (size_t i = 0U; i < incoming_count; ++i) {
        host->bbs_notifications[host->bbs_notification_count++] = incoming[i];
    }
}

/* ------------------------------------------------------------------ */
/* Read marks (text sidecar, one entry per line)                      */
/* ------------------------------------------------------------------ */

static void host_bbs_readmarks_save_locked(host_t *host)
{
    if (host == nullptr || host->bbs_state_file_path[0] == '\0') {
        return;
    }
    char path[PATH_MAX];
    host_bbs_v2_path(host->bbs_state_file_path, "readmark.dat", path,
                     sizeof(path));
    char temp_path[PATH_MAX + 16];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return;
    }
    FILE *fp = fdopen(fd, "wb");
    if (fp == nullptr) {
        close(fd);
        return;
    }

    time_t now = time(nullptr);
    for (size_t i = 0U; i < host->bbs_read_mark_count; ++i) {
        const bbs_read_mark_t *mark = &host->bbs_read_marks[i];
        // Prune stamps older than 30 days on save.
        if (mark->last_read_at > 0 && now - mark->last_read_at >
                                          (time_t)(30 * 24 * 60 * 60)) {
            continue;
        }
        fprintf(fp, "%s|%lld\n", mark->username,
                (long long)mark->last_read_at);
    }

    fflush(fp);
    fsync(fd);
    fclose(fp);
    chmod(temp_path, S_IRUSR | S_IWUSR);
    rename(temp_path, path);
    chmod(path, S_IRUSR | S_IWUSR);
}

static void host_bbs_readmarks_load(host_t *host)
{
    if (host == nullptr || host->bbs_state_file_path[0] == '\0') {
        return;
    }
    if (host->bbs_read_marks == nullptr) {
        host->bbs_read_marks = sshc_gc_calloc(SSH_CHATTER_BBS_MAX_READMARKS,
                                              sizeof(bbs_read_mark_t));
        if (host->bbs_read_marks == nullptr) {
            return;
        }
        host->bbs_read_mark_capacity = SSH_CHATTER_BBS_MAX_READMARKS;
        host->bbs_read_mark_count = 0U;
    }

    char path[PATH_MAX];
    host_bbs_v2_path(host->bbs_state_file_path, "readmark.dat", path,
                     sizeof(path));
    if (!host_bbs_file_exists(path)) {
        return;
    }
    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        return;
    }

    bbs_read_mark_t incoming[SSH_CHATTER_BBS_MAX_READMARKS];
    size_t incoming_count = 0U;
    char line[128];
    while (fgets(line, sizeof(line), fp) != nullptr) {
        char *sep = strchr(line, '|');
        if (sep == nullptr) {
            continue;
        }
        *sep = '\0';
        bbs_read_mark_t entry = {0};
        snprintf(entry.username, sizeof(entry.username), "%s", line);
        entry.last_read_at = (time_t)strtoll(sep + 1, nullptr, 10);
        if (incoming_count >= SSH_CHATTER_BBS_MAX_READMARKS) {
            memmove(incoming, incoming + 1,
                    (incoming_count - 1U) * sizeof(incoming[0]));
            --incoming_count;
        }
        incoming[incoming_count++] = entry;
    }
    fclose(fp);

    host->bbs_read_mark_count = 0U;
    for (size_t i = 0U; i < incoming_count; ++i) {
        host->bbs_read_marks[host->bbs_read_mark_count++] = incoming[i];
    }
}

/* ------------------------------------------------------------------ */
/* Moderation reports (text sidecar)                                  */
/* ------------------------------------------------------------------ */

static void host_bbs_reports_save_locked(host_t *host)
{
    if (host == nullptr || host->bbs_state_file_path[0] == '\0') {
        return;
    }
    char path[PATH_MAX];
    host_bbs_v2_path(host->bbs_state_file_path, "reports.dat", path,
                     sizeof(path));
    char temp_path[PATH_MAX + 16];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return;
    }
    FILE *fp = fdopen(fd, "wb");
    if (fp == nullptr) {
        close(fd);
        return;
    }

    for (size_t i = 0U; i < host->bbs_report_count; ++i) {
        const bbs_report_t *report = &host->bbs_reports[i];
        fprintf(fp, "%" PRIu64 "|%s|%lld|%d|%s\n", report->post_id,
                report->reporter, (long long)report->created_at,
                (int)report->status, report->reason);
    }

    fflush(fp);
    fsync(fd);
    fclose(fp);
    chmod(temp_path, S_IRUSR | S_IWUSR);
    rename(temp_path, path);
    chmod(path, S_IRUSR | S_IWUSR);
}

static void host_bbs_reports_load(host_t *host)
{
    if (host == nullptr || host->bbs_state_file_path[0] == '\0') {
        return;
    }
    if (host->bbs_reports == nullptr) {
        host->bbs_reports =
            sshc_gc_calloc(SSH_CHATTER_BBS_MAX_REPORTS, sizeof(bbs_report_t));
        if (host->bbs_reports == nullptr) {
            return;
        }
        host->bbs_report_capacity = SSH_CHATTER_BBS_MAX_REPORTS;
        host->bbs_report_count = 0U;
    }

    char path[PATH_MAX];
    host_bbs_v2_path(host->bbs_state_file_path, "reports.dat", path,
                     sizeof(path));
    if (!host_bbs_file_exists(path)) {
        return;
    }
    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        return;
    }

    bbs_report_t incoming[SSH_CHATTER_BBS_MAX_REPORTS];
    size_t incoming_count = 0U;
    char line[512];
    while (fgets(line, sizeof(line), fp) != nullptr) {
        line[strcspn(line, "\r\n")] = '\0';
        char *fields[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
        char *cursor = line;
        size_t field_count = 0U;
        while (field_count < 5U) {
            fields[field_count++] = cursor;
            char *sep = strchr(cursor, '|');
            if (sep == nullptr) {
                break;
            }
            *sep = '\0';
            cursor = sep + 1;
        }
        if (field_count < 4U || fields[0] == nullptr ||
            fields[1] == nullptr || fields[2] == nullptr ||
            fields[3] == nullptr) {
            continue;
        }
        bbs_report_t entry = {0};
        entry.post_id = (uint64_t)strtoull(fields[0], nullptr, 10);
        snprintf(entry.reporter, sizeof(entry.reporter), "%s", fields[1]);
        entry.created_at = (time_t)strtoll(fields[2], nullptr, 10);
        entry.status = (int32_t)strtol(fields[3], nullptr, 10);
        if (fields[4] != nullptr) {
            snprintf(entry.reason, sizeof(entry.reason), "%s", fields[4]);
        }
        if (incoming_count >= SSH_CHATTER_BBS_MAX_REPORTS) {
            memmove(incoming, incoming + 1,
                    (incoming_count - 1U) * sizeof(incoming[0]));
            --incoming_count;
        }
        incoming[incoming_count++] = entry;
    }
    fclose(fp);

    host->bbs_report_count = 0U;
    for (size_t i = 0U; i < incoming_count; ++i) {
        host->bbs_reports[host->bbs_report_count++] = incoming[i];
    }
}

/* ------------------------------------------------------------------ */
/* Mutes (text sidecar)                                               */
/* ------------------------------------------------------------------ */

static void host_bbs_mutes_save_locked(host_t *host)
{
    if (host == nullptr || host->bbs_state_file_path[0] == '\0') {
        return;
    }
    char path[PATH_MAX];
    host_bbs_v2_path(host->bbs_state_file_path, "mutes.dat", path,
                     sizeof(path));
    char temp_path[PATH_MAX + 16];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return;
    }
    FILE *fp = fdopen(fd, "wb");
    if (fp == nullptr) {
        close(fd);
        return;
    }

    for (size_t i = 0U; i < host->bbs_mute_count; ++i) {
        const bbs_mute_t *mute = &host->bbs_mutes[i];
        fprintf(fp, "%s|%lld|%s|%lld\n", mute->username,
                (long long)mute->until, mute->muted_by,
                (long long)mute->created_at);
    }

    fflush(fp);
    fsync(fd);
    fclose(fp);
    chmod(temp_path, S_IRUSR | S_IWUSR);
    rename(temp_path, path);
    chmod(path, S_IRUSR | S_IWUSR);
}

static void host_bbs_mutes_load(host_t *host)
{
    if (host == nullptr || host->bbs_state_file_path[0] == '\0') {
        return;
    }
    if (host->bbs_mutes == nullptr) {
        host->bbs_mutes =
            sshc_gc_calloc(SSH_CHATTER_BBS_MAX_MUTES, sizeof(bbs_mute_t));
        if (host->bbs_mutes == nullptr) {
            return;
        }
        host->bbs_mute_capacity = SSH_CHATTER_BBS_MAX_MUTES;
        host->bbs_mute_count = 0U;
    }

    char path[PATH_MAX];
    host_bbs_v2_path(host->bbs_state_file_path, "mutes.dat", path,
                     sizeof(path));
    if (!host_bbs_file_exists(path)) {
        return;
    }
    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        return;
    }

    bbs_mute_t incoming[SSH_CHATTER_BBS_MAX_MUTES];
    size_t incoming_count = 0U;
    char line[256];
    while (fgets(line, sizeof(line), fp) != nullptr) {
        char *fields[4] = {nullptr, nullptr, nullptr, nullptr};
        char *cursor = line;
        size_t field_count = 0U;
        while (field_count < 4U) {
            fields[field_count++] = cursor;
            char *sep = strchr(cursor, '|');
            if (sep == nullptr) {
                break;
            }
            *sep = '\0';
            cursor = sep + 1;
        }
        if (field_count < 4U || fields[0] == nullptr ||
            fields[1] == nullptr || fields[2] == nullptr ||
            fields[3] == nullptr) {
            continue;
        }
        bbs_mute_t entry = {0};
        snprintf(entry.username, sizeof(entry.username), "%s", fields[0]);
        entry.until = (time_t)strtoll(fields[1], nullptr, 10);
        snprintf(entry.muted_by, sizeof(entry.muted_by), "%s", fields[2]);
        entry.created_at = (time_t)strtoll(fields[3], nullptr, 10);
        if (incoming_count >= SSH_CHATTER_BBS_MAX_MUTES) {
            memmove(incoming, incoming + 1,
                    (incoming_count - 1U) * sizeof(incoming[0]));
            --incoming_count;
        }
        incoming[incoming_count++] = entry;
    }
    fclose(fp);

    host->bbs_mute_count = 0U;
    for (size_t i = 0U; i < incoming_count; ++i) {
        host->bbs_mutes[host->bbs_mute_count++] = incoming[i];
    }
}

/* ------------------------------------------------------------------ */
/* Moderation action log (text sidecar)                               */
/* ------------------------------------------------------------------ */

static void host_bbs_modlog_save_locked(host_t *host)
{
    if (host == nullptr || host->bbs_state_file_path[0] == '\0') {
        return;
    }
    char path[PATH_MAX];
    host_bbs_v2_path(host->bbs_state_file_path, "modlog.dat", path,
                     sizeof(path));
    char temp_path[PATH_MAX + 16];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return;
    }
    FILE *fp = fdopen(fd, "wb");
    if (fp == nullptr) {
        close(fd);
        return;
    }

    for (size_t i = 0U; i < host->bbs_modlog_count; ++i) {
        const bbs_modlog_entry_t *entry = &host->bbs_modlog[i];
        fprintf(fp, "%lld|%s|%s|%s\n", (long long)entry->created_at,
                entry->actor, entry->action, entry->target);
    }

    fflush(fp);
    fsync(fd);
    fclose(fp);
    chmod(temp_path, S_IRUSR | S_IWUSR);
    rename(temp_path, path);
    chmod(path, S_IRUSR | S_IWUSR);
}

static void host_bbs_modlog_load(host_t *host)
{
    if (host == nullptr || host->bbs_state_file_path[0] == '\0') {
        return;
    }
    if (host->bbs_modlog == nullptr) {
        host->bbs_modlog = sshc_gc_calloc(SSH_CHATTER_BBS_MAX_MODLOG,
                                          sizeof(bbs_modlog_entry_t));
        if (host->bbs_modlog == nullptr) {
            return;
        }
        host->bbs_modlog_capacity = SSH_CHATTER_BBS_MAX_MODLOG;
        host->bbs_modlog_count = 0U;
    }

    char path[PATH_MAX];
    host_bbs_v2_path(host->bbs_state_file_path, "modlog.dat", path,
                     sizeof(path));
    if (!host_bbs_file_exists(path)) {
        return;
    }
    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        return;
    }

    bbs_modlog_entry_t incoming[SSH_CHATTER_BBS_MAX_MODLOG];
    size_t incoming_count = 0U;
    char line[256];
    while (fgets(line, sizeof(line), fp) != nullptr) {
        line[strcspn(line, "\r\n")] = '\0';
        char *fields[4] = {nullptr, nullptr, nullptr, nullptr};
        char *cursor = line;
        size_t field_count = 0U;
        while (field_count < 4U) {
            fields[field_count++] = cursor;
            char *sep = strchr(cursor, '|');
            if (sep == nullptr) {
                break;
            }
            *sep = '\0';
            cursor = sep + 1;
        }
        if (field_count < 4U || fields[0] == nullptr ||
            fields[1] == nullptr || fields[2] == nullptr ||
            fields[3] == nullptr) {
            continue;
        }
        bbs_modlog_entry_t entry = {0};
        entry.created_at = (time_t)strtoll(fields[0], nullptr, 10);
        snprintf(entry.actor, sizeof(entry.actor), "%s", fields[1]);
        snprintf(entry.action, sizeof(entry.action), "%s", fields[2]);
        snprintf(entry.target, sizeof(entry.target), "%s", fields[3]);
        if (incoming_count >= SSH_CHATTER_BBS_MAX_MODLOG) {
            memmove(incoming, incoming + 1,
                    (incoming_count - 1U) * sizeof(incoming[0]));
            --incoming_count;
        }
        incoming[incoming_count++] = entry;
    }
    fclose(fp);

    host->bbs_modlog_count = 0U;
    for (size_t i = 0U; i < incoming_count; ++i) {
        host->bbs_modlog[host->bbs_modlog_count++] = incoming[i];
    }
}

// Append one moderation action log entry.  Callers must hold host->lock.
static void host_bbs_modlog_append_locked(host_t *host, const char *actor,
                                          const char *action,
                                          const char *target)
{
    if (host == nullptr || actor == nullptr || action == nullptr ||
        target == nullptr) {
        return;
    }
    if (host->bbs_modlog == nullptr || host->bbs_modlog_capacity == 0U) {
        // Host-lifetime block: allocate in the host memory context, never
        // in the calling session's context (freed at session end).
        sshc_memory_context_t *scope = host_memory_scope_push(host);
        host->bbs_modlog = sshc_gc_calloc(SSH_CHATTER_BBS_MAX_MODLOG,
                                          sizeof(bbs_modlog_entry_t));
        host_memory_scope_pop(scope);
        if (host->bbs_modlog == nullptr) {
            return;
        }
        host->bbs_modlog_capacity = SSH_CHATTER_BBS_MAX_MODLOG;
        host->bbs_modlog_count = 0U;
    }
    if (host->bbs_modlog_count >= host->bbs_modlog_capacity) {
        memmove(host->bbs_modlog, host->bbs_modlog + 1,
                (host->bbs_modlog_count - 1U) * sizeof(*host->bbs_modlog));
        --host->bbs_modlog_count;
    }

    bbs_modlog_entry_t *entry =
        &host->bbs_modlog[host->bbs_modlog_count++];
    memset(entry, 0, sizeof(*entry));
    entry->created_at = time(nullptr);
    snprintf(entry->actor, sizeof(entry->actor), "%s", actor);
    snprintf(entry->action, sizeof(entry->action), "%s", action);
    snprintf(entry->target, sizeof(entry->target), "%s", target);
}

/* ------------------------------------------------------------------ */
/* IP audit log (text sidecar, 5-day retention, never backed up)      */
/* ------------------------------------------------------------------ */

// Destroy entries past the retention window: disconnected entries aged
// 5 days, and still-connected entries whose connect time is 5 days old
// (stale/crashed sessions).  Callers must hold host->lock.
static void host_ipaudit_sweep_locked(host_t *host, time_t now)
{
    if (host == nullptr || host->bbs_ipaudit == nullptr || now <= 0) {
        return;
    }
    size_t write_idx = 0U;
    for (size_t idx = 0U; idx < host->bbs_ipaudit_count; ++idx) {
        bbs_ipaudit_entry_t *entry = &host->bbs_ipaudit[idx];
        time_t anchor = entry->disconnect_epoch > 0
                            ? entry->disconnect_epoch
                            : entry->connect_epoch;
        if (anchor > 0 && now - anchor >
                              (time_t)SSH_CHATTER_IPAUDIT_RETENTION_SECONDS) {
            continue;
        }
        host->bbs_ipaudit[write_idx++] = *entry;
    }
    host->bbs_ipaudit_count = write_idx;
}

static void host_bbs_ipaudit_save_locked(host_t *host)
{
    if (host == nullptr || host->bbs_state_file_path[0] == '\0') {
        return;
    }
    char path[PATH_MAX];
    host_bbs_v2_path(host->bbs_state_file_path, "ipaudit.dat", path,
                     sizeof(path));
    char temp_path[PATH_MAX + 16];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return;
    }
    FILE *fp = fdopen(fd, "wb");
    if (fp == nullptr) {
        close(fd);
        return;
    }

    for (size_t i = 0U; i < host->bbs_ipaudit_count; ++i) {
        const bbs_ipaudit_entry_t *entry = &host->bbs_ipaudit[i];
        fprintf(fp, "%s|%s|%lld|%lld\n", entry->username, entry->ip,
                (long long)entry->connect_epoch,
                (long long)entry->disconnect_epoch);
    }

    fflush(fp);
    fsync(fd);
    fclose(fp);
    chmod(temp_path, S_IRUSR | S_IWUSR);
    rename(temp_path, path);
    chmod(path, S_IRUSR | S_IWUSR);
}

static void host_bbs_ipaudit_load(host_t *host)
{
    if (host == nullptr || host->bbs_state_file_path[0] == '\0') {
        return;
    }
    if (host->bbs_ipaudit == nullptr) {
        host->bbs_ipaudit = sshc_gc_calloc(SSH_CHATTER_BBS_MAX_IPAUDIT,
                                           sizeof(bbs_ipaudit_entry_t));
        if (host->bbs_ipaudit == nullptr) {
            return;
        }
        host->bbs_ipaudit_capacity = SSH_CHATTER_BBS_MAX_IPAUDIT;
        host->bbs_ipaudit_count = 0U;
    }

    char path[PATH_MAX];
    host_bbs_v2_path(host->bbs_state_file_path, "ipaudit.dat", path,
                     sizeof(path));
    if (!host_bbs_file_exists(path)) {
        return;
    }
    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        return;
    }

    bbs_ipaudit_entry_t incoming[SSH_CHATTER_BBS_MAX_IPAUDIT];
    size_t incoming_count = 0U;
    char line[256];
    while (fgets(line, sizeof(line), fp) != nullptr) {
        char *fields[4] = {nullptr, nullptr, nullptr, nullptr};
        char *cursor = line;
        size_t field_count = 0U;
        while (field_count < 4U) {
            fields[field_count++] = cursor;
            char *sep = strchr(cursor, '|');
            if (sep == nullptr) {
                break;
            }
            *sep = '\0';
            cursor = sep + 1;
        }
        if (field_count < 4U || fields[0] == nullptr ||
            fields[1] == nullptr || fields[2] == nullptr ||
            fields[3] == nullptr) {
            continue;
        }
        bbs_ipaudit_entry_t entry = {0};
        snprintf(entry.username, sizeof(entry.username), "%s", fields[0]);
        snprintf(entry.ip, sizeof(entry.ip), "%s", fields[1]);
        entry.connect_epoch = (time_t)strtoll(fields[2], nullptr, 10);
        entry.disconnect_epoch = (time_t)strtoll(fields[3], nullptr, 10);
        if (incoming_count >= SSH_CHATTER_BBS_MAX_IPAUDIT) {
            memmove(incoming, incoming + 1,
                    (incoming_count - 1U) * sizeof(incoming[0]));
            --incoming_count;
        }
        incoming[incoming_count++] = entry;
    }
    fclose(fp);

    host->bbs_ipaudit_count = 0U;
    for (size_t i = 0U; i < incoming_count; ++i) {
        host->bbs_ipaudit[host->bbs_ipaudit_count++] = incoming[i];
    }
    // Enforce retention on load as well.
    host_ipaudit_sweep_locked(host, time(nullptr));
}

// Record a connect for an authenticated user.  Callers must hold host->lock.
static void host_ipaudit_record_connect_locked(host_t *host,
                                               const char *username,
                                               const char *ip)
{
    if (host == nullptr || username == nullptr || username[0] == '\0' ||
        ip == nullptr || ip[0] == '\0') {
        return;
    }
    if (!host_bbs_storage_ready(host)) {
        return;
    }
    if (host->bbs_ipaudit == nullptr || host->bbs_ipaudit_capacity == 0U) {
        // Host-lifetime block: allocate in the host memory context, never
        // in the calling session's context (freed at session end).
        sshc_memory_context_t *scope = host_memory_scope_push(host);
        host->bbs_ipaudit = sshc_gc_calloc(SSH_CHATTER_BBS_MAX_IPAUDIT,
                                           sizeof(bbs_ipaudit_entry_t));
        host_memory_scope_pop(scope);
        if (host->bbs_ipaudit == nullptr) {
            return;
        }
        host->bbs_ipaudit_capacity = SSH_CHATTER_BBS_MAX_IPAUDIT;
        host->bbs_ipaudit_count = 0U;
    }
    if (host->bbs_ipaudit_count >= host->bbs_ipaudit_capacity) {
        memmove(host->bbs_ipaudit, host->bbs_ipaudit + 1,
                (host->bbs_ipaudit_count - 1U) *
                    sizeof(*host->bbs_ipaudit));
        --host->bbs_ipaudit_count;
    }

    bbs_ipaudit_entry_t *entry =
        &host->bbs_ipaudit[host->bbs_ipaudit_count++];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->username, sizeof(entry->username), "%s", username);
    snprintf(entry->ip, sizeof(entry->ip), "%s", ip);
    entry->connect_epoch = time(nullptr);
    entry->disconnect_epoch = 0;
}

// Stamp the disconnect time on the newest matching open entry.
// Callers must hold host->lock.
static void host_ipaudit_record_disconnect_locked(host_t *host,
                                                  const char *username,
                                                  const char *ip)
{
    if (host == nullptr || host->bbs_ipaudit == nullptr ||
        username == nullptr || ip == nullptr) {
        return;
    }
    for (size_t idx = host->bbs_ipaudit_count; idx > 0U; --idx) {
        bbs_ipaudit_entry_t *entry = &host->bbs_ipaudit[idx - 1U];
        if (entry->disconnect_epoch == 0 &&
            strncmp(entry->username, username, SSH_CHATTER_USERNAME_LEN) ==
                0 &&
            strncmp(entry->ip, ip, SSH_CHATTER_IP_LEN) == 0) {
            entry->disconnect_epoch = time(nullptr);
            break;
        }
    }
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
        host_bbs_aux_loads(host);
        return;
    }

    int fd = fileno(fp);
    if (fd < 0) {
        fclose(fp);
        host->bbs_cache_loaded = true;
        host_bbs_aux_loads(host);
        return;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        fclose(fp);
        host->bbs_cache_loaded = true;
        host_bbs_aux_loads(host);
        return;
    }

    size_t mapped_len = (size_t)st.st_size;
    if (mapped_len < sizeof(bbs_state_header_t)) {
        fclose(fp);
        host->bbs_cache_loaded = true;
        host_bbs_aux_loads(host);
        return;
    }

    unsigned char *mapped = mmap(nullptr, mapped_len, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE, fd, 0);
    fclose(fp);
    fp = nullptr;
    if (mapped == MAP_FAILED) {
        host->bbs_cache_loaded = true;
        host_bbs_aux_loads(host);
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
        host_bbs_aux_loads(host);
        return;
    }

    if (header.version != BBS_STATE_VERSION &&
        header.version != BBS_STATE_VERSION_V2 &&
        header.version != BBS_STATE_VERSION_V1) {
        memset(mapped, 0, mapped_len);
        munmap(mapped, mapped_len);
        host->bbs_cache_loaded = true;
        host_bbs_aux_loads(host);
        return;
    }

    ttak_mutex_lock(&host->lock);

    size_t capacity = host_bbs_loop_limit(host);
    for (size_t idx = 0U; idx < capacity; ++idx) {
        host->bbs_posts[idx].in_use = false;
        host->bbs_posts[idx].id = 0U;
        host->bbs_posts[idx].board_id = 0U;
        host->bbs_posts[idx].author[0] = '\0';
        host->bbs_posts[idx].title[0] = '\0';
        host->bbs_posts[idx].body[0] = '\0';
        host->bbs_posts[idx].tag_count = 0U;
        host->bbs_posts[idx].created_at = 0;
        host->bbs_posts[idx].bumped_at = 0;
        host->bbs_posts[idx].upvotes = 0;
        host->bbs_posts[idx].downvotes = 0;
        host->bbs_posts[idx].comment_count = 0U;
        for (size_t comment = 0U; comment < SSH_CHATTER_BBS_MAX_COMMENTS;
             ++comment) {
            host->bbs_posts[idx].comments[comment].author[0] = '\0';
            host->bbs_posts[idx].comments[comment].text[0] = '\0';
            host->bbs_posts[idx].comments[comment].created_at = 0;
            host->bbs_posts[idx].comments[comment].edited_at = 0;
            host->bbs_posts[idx].comments[comment].upvotes = 0;
            host->bbs_posts[idx].comments[comment].downvotes = 0;
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
        bbs_state_post_entry_disk_t serialized = {0};
        if (header.version == BBS_STATE_VERSION_V1) {
            bbs_state_post_entry_disk_v1_t legacy = {0};
            if (remaining < sizeof(legacy)) {
                success = false;
                break;
            }
            memcpy(&legacy, cursor, sizeof(legacy));
            cursor += sizeof(legacy);
            remaining -= sizeof(legacy);
            bbs_state_post_entry_from_v1(&serialized, &legacy);
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
        bbs_comment_t *saved_comments = post->comments;
        memset(post, 0, sizeof(*post));
        post->comments = saved_comments;
        post->in_use = true;
        post->id = serialized.id;
        post->board_id = serialized.board_id;
        post->mod_flags = serialized.mod_flags;
        post->created_at = (time_t)serialized.created_at;
        post->bumped_at = (time_t)serialized.bumped_at;
        post->upvotes = serialized.upvotes;
        post->downvotes = serialized.downvotes;
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
            post->comments[comment].edited_at =
                (time_t)serialized.comments[comment].edited_at;
            post->comments[comment].upvotes = serialized.comments[comment].upvotes;
            post->comments[comment].downvotes = serialized.comments[comment].downvotes;
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
            host->bbs_posts[idx].board_id = 0U;
            host->bbs_posts[idx].author[0] = '\0';
            host->bbs_posts[idx].title[0] = '\0';
            host->bbs_posts[idx].body[0] = '\0';
            host->bbs_posts[idx].tag_count = 0U;
            host->bbs_posts[idx].created_at = 0;
            host->bbs_posts[idx].bumped_at = 0;
            host->bbs_posts[idx].upvotes = 0;
            host->bbs_posts[idx].downvotes = 0;
            host->bbs_posts[idx].comment_count = 0U;
            for (size_t comment = 0U; comment < SSH_CHATTER_BBS_MAX_COMMENTS;
                 ++comment) {
                host->bbs_posts[idx].comments[comment].author[0] = '\0';
                host->bbs_posts[idx].comments[comment].text[0] = '\0';
                host->bbs_posts[idx].comments[comment].created_at = 0;
                host->bbs_posts[idx].comments[comment].edited_at = 0;
                host->bbs_posts[idx].comments[comment].upvotes = 0;
                host->bbs_posts[idx].comments[comment].downvotes = 0;
            }
        }
        host->bbs_post_count = 0U;
        host->next_bbs_id = 1U;
    }

    ttak_mutex_unlock(&host->lock);
    memset(mapped, 0, mapped_len);
    munmap(mapped, mapped_len);
    host->bbs_cache_loaded = true;

    /* load auxiliary data directly */
    host_bbs_aux_loads(host);
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
    pthread_detach(pthread_self());
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

/* ------------------------------------------------------------------ */
/* Door game lock persistence                                          */
/* ------------------------------------------------------------------ */

void host_door_games_save_locked(host_t *host)
{
    if (host == nullptr || host->bbs_state_file_path[0] == '\0') {
        return;
    }
    char path[PATH_MAX];
    host_bbs_v2_path(host->bbs_state_file_path, "door_games.dat",
                     path, sizeof(path));
    char temp_path[PATH_MAX + 16];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return;
    }
    FILE *fp = fdopen(fd, "w");
    if (fp == nullptr) {
        close(fd);
        return;
    }

    for (size_t i = 0U; i < host->door_game_count; ++i) {
        if (!host->door_games[i].in_use) {
            continue;
        }
        fprintf(fp, "%s:%d\n", host->door_games[i].name,
                host->door_games[i].locked ? 1 : 0);
    }

    fclose(fp);
    if (rename(temp_path, path) != 0) {
        humanized_log_error("door", "failed to update door game state file",
                            errno);
        unlink(temp_path);
    } else if (chmod(path, S_IRUSR | S_IWUSR) != 0) {
        humanized_log_error("door",
                            "failed to tighten door game state permissions",
                            errno != 0 ? errno : EACCES);
    }
}

void host_door_games_load_locked(host_t *host)
{
    if (host == nullptr || host->bbs_state_file_path[0] == '\0' ||
        host->door_game_count == 0U) {
        return;
    }
    char path[PATH_MAX];
    host_bbs_v2_path(host->bbs_state_file_path, "door_games.dat",
                     path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (fp == nullptr) {
        return;
    }

    char line[SSH_CHATTER_DOOR_GAME_NAME_LEN + 8];
    while (fgets(line, sizeof(line), fp) != nullptr) {
        size_t len = strlen(line);
        if (len > 0U && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }
        char *colon = strchr(line, ':');
        if (colon == nullptr) {
            continue;
        }
        *colon = '\0';
        int locked_val = atoi(colon + 1);
        for (size_t i = 0U; i < host->door_game_count; ++i) {
            if (host->door_games[i].in_use &&
                strcasecmp(host->door_games[i].name, line) == 0) {
                host->door_games[i].locked = (locked_val != 0);
                break;
            }
        }
    }
    fclose(fp);
}


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

static void host_bbs_boards_save_locked(host_t *host);
static void host_bbs_votes_save_locked(host_t *host);
static void host_bbs_drafts_save_locked(host_t *host);
static void host_bbs_boards_load(host_t *host);
static void host_bbs_votes_load(host_t *host);
static void host_bbs_drafts_load(host_t *host);

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
        serialized.board_id = post->board_id;
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

    host_bbs_boards_save_locked(host);
    host_bbs_votes_save_locked(host);
    host_bbs_drafts_save_locked(host);
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
        host_bbs_boards_load(host);
        host_bbs_votes_load(host);
        host_bbs_drafts_load(host);
        return;
    }

    int fd = fileno(fp);
    if (fd < 0) {
        fclose(fp);
        host->bbs_cache_loaded = true;
        host_bbs_boards_load(host);
        host_bbs_votes_load(host);
        host_bbs_drafts_load(host);
        return;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        fclose(fp);
        host->bbs_cache_loaded = true;
        host_bbs_boards_load(host);
        host_bbs_votes_load(host);
        host_bbs_drafts_load(host);
        return;
    }

    size_t mapped_len = (size_t)st.st_size;
    if (mapped_len < sizeof(bbs_state_header_t)) {
        fclose(fp);
        host->bbs_cache_loaded = true;
        host_bbs_boards_load(host);
        host_bbs_votes_load(host);
        host_bbs_drafts_load(host);
        return;
    }

    unsigned char *mapped = mmap(nullptr, mapped_len, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE, fd, 0);
    fclose(fp);
    fp = nullptr;
    if (mapped == MAP_FAILED) {
        host->bbs_cache_loaded = true;
        host_bbs_boards_load(host);
        host_bbs_votes_load(host);
        host_bbs_drafts_load(host);
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
        host_bbs_boards_load(host);
        host_bbs_votes_load(host);
        host_bbs_drafts_load(host);
        return;
    }

    if (header.version != BBS_STATE_VERSION) {
        memset(mapped, 0, mapped_len);
        munmap(mapped, mapped_len);
        host->bbs_cache_loaded = true;
        host_bbs_boards_load(host);
        host_bbs_votes_load(host);
        host_bbs_drafts_load(host);
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
        bbs_state_post_entry_t serialized = {0};
        if (remaining < sizeof(serialized)) {
            success = false;
            break;
        }
        memcpy(&serialized, cursor, sizeof(serialized));
        cursor += sizeof(serialized);
        remaining -= sizeof(serialized);

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
    host_bbs_boards_load(host);
    host_bbs_votes_load(host);
    host_bbs_drafts_load(host);
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

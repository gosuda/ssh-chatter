{
    if (host == nullptr) {
        return;
    }

    if (host->vote_state_file_path[0] == '\0') {
        return;
    }

    FILE *fp = fopen(host->vote_state_file_path, "rb");
    if (fp == nullptr) {
        return;
    }

    vote_state_header_t header = {0};
    if (fread(&header, sizeof(header), 1U, fp) != 1U) {
        fclose(fp);
        return;
    }

    if (header.magic != VOTE_STATE_MAGIC) {
        fclose(fp);
        return;
    }

    if (header.version == 0U || header.version > VOTE_STATE_VERSION) {
        fclose(fp);
        return;
    }

    ttak_mutex_lock(&host->lock);

    poll_state_reset(&host->poll);
    for (size_t idx = 0U; idx < SSH_CHATTER_MAX_NAMED_POLLS; ++idx) {
        named_poll_reset(&host->named_polls[idx]);
    }
    host->named_poll_count = 0U;

    bool success = true;

    vote_state_poll_entry_t main_entry = {0};
    if (fread(&main_entry, sizeof(main_entry), 1U, fp) != 1U) {
        success = false;
    } else {
        vote_state_import_poll_entry(&main_entry, &host->poll);
    }

    for (uint32_t idx = 0U; success && idx < header.named_count; ++idx) {
        vote_state_named_entry_t entry = {0};
        if (fread(&entry, sizeof(entry), 1U, fp) != 1U) {
            success = false;
            break;
        }

        if (idx >= SSH_CHATTER_MAX_NAMED_POLLS) {
            continue;
        }

        named_poll_state_t *poll = &host->named_polls[idx];
        vote_state_import_poll_entry(&entry.poll, &poll->poll);
        snprintf(poll->label, sizeof(poll->label), "%s", entry.label);
        snprintf(poll->owner, sizeof(poll->owner), "%s", entry.owner);
        poll->voter_count = entry.voter_count;
        if (poll->voter_count > SSH_CHATTER_MAX_NAMED_VOTERS) {
            poll->voter_count = SSH_CHATTER_MAX_NAMED_VOTERS;
        }
        for (size_t voter = 0U; voter < SSH_CHATTER_MAX_NAMED_VOTERS; ++voter) {
            snprintf(poll->voters[voter].username,
                     sizeof(poll->voters[voter].username), "%s",
                     entry.voters[voter].username);
            poll->voters[voter].choice = entry.voters[voter].choice;
            poll->voters[voter].choices_mask = entry.voters[voter].choices_mask;
        }
    }

    if (success) {
        host_recount_named_polls_locked(host);
    } else {
        poll_state_reset(&host->poll);
        for (size_t idx = 0U; idx < SSH_CHATTER_MAX_NAMED_POLLS; ++idx) {
            named_poll_reset(&host->named_polls[idx]);
        }
        host->named_poll_count = 0U;
    }

    ttak_mutex_unlock(&host->lock);
    fclose(fp);
}

static void host_ban_state_load(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (host->ban_state_file_path[0] == '\0') {
        return;
    }

    FILE *fp = fopen(host->ban_state_file_path, "rb");
    if (fp == nullptr) {
        return;
    }

    ban_state_header_t header = {0};
    if (fread(&header, sizeof(header), 1U, fp) != 1U) {
        fclose(fp);
        return;
    }

    if (header.magic != BAN_STATE_MAGIC || header.version == 0U ||
        header.version > BAN_STATE_VERSION) {
        fclose(fp);
        return;
    }

    uint32_t entry_count = header.entry_count;
    ban_state_entry_t *entries = nullptr;
    if (entry_count > 0U) {
        entries = sshc_gc_calloc(entry_count, sizeof(*entries));
        if (entries == nullptr) {
            fclose(fp);
            humanized_log_error("host", "failed to allocate ban state buffer",
                                ENOMEM);
            return;
        }
    }

    bool success = true;
    int read_error = 0;
    for (uint32_t idx = 0U; idx < entry_count; ++idx) {
        if (fread(&entries[idx], sizeof(entries[idx]), 1U, fp) != 1U) {
            success = false;
            if (errno != 0) {
                read_error = errno;
            }
            break;
        }
    }

    fclose(fp);

    if (!success) {
        humanized_log_error("host", "failed to read ban state file",
                            read_error != 0 ? read_error : EIO);
        sshc_gc_free(entries);
        return;
    }

    ttak_mutex_lock(&host->lock);
    memset(host->bans, 0, sizeof(host->bans));
    host->ban_count = 0U;
    for (uint32_t idx = 0U; idx < entry_count; ++idx) {
        if (host->ban_count >= SSH_CHATTER_MAX_BANS) {
            break;
        }
        if (entries[idx].ip[0] != '\0' &&
            host_is_protected_ip_unlocked(host, entries[idx].ip)) {
            continue;
        }
        if (entries[idx].ip[0] != '\0' &&
            strchr(entries[idx].ip, '/') != nullptr) {
            bool intersects_protected = false;
            for (size_t protected_idx = 0;
                 protected_idx < host->protected_ip_count &&
                 protected_idx < SSH_CHATTER_MAX_PROTECTED_IPS;
                 ++protected_idx) {
                if (host_cidr_contains_ip(entries[idx].ip,
                                          host->protected_ips[protected_idx])) {
                    intersects_protected = true;
                    break;
                }
            }
            if (intersects_protected) {
                continue;
            }
        }
        snprintf(host->bans[host->ban_count].username,
                 sizeof(host->bans[host->ban_count].username), "%s",
                 entries[idx].username);
        snprintf(host->bans[host->ban_count].ip,
                 sizeof(host->bans[host->ban_count].ip), "%s", entries[idx].ip);
        ++host->ban_count;
    }
    ttak_mutex_unlock(&host->lock);
    sshc_gc_free(entries);
}

static void host_reply_state_load(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (host->reply_state_file_path[0] == '\0') {
        return;
    }

    FILE *fp = fopen(host->reply_state_file_path, "rb");
    if (fp == nullptr) {
        return;
    }

    reply_state_header_t header = {0};
    if (fread(&header, sizeof(header), 1U, fp) != 1U) {
        fclose(fp);
        return;
    }

    if (header.magic != REPLY_STATE_MAGIC || header.version == 0U ||
        header.version > REPLY_STATE_VERSION) {
        fclose(fp);
        return;
    }

    uint32_t entry_count = header.entry_count;
    reply_state_entry_t *entries = nullptr;
    if (entry_count > 0U) {
        entries = sshc_gc_calloc(entry_count, sizeof(*entries));
        if (entries == nullptr) {
            fclose(fp);
            humanized_log_error("host", "failed to allocate reply state buffer",
                                ENOMEM);
            return;
        }
    }

    bool success = true;
    int read_error = 0;
    for (uint32_t idx = 0U; idx < entry_count; ++idx) {
        if (fread(&entries[idx], sizeof(entries[idx]), 1U, fp) != 1U) {
            success = false;
            if (errno != 0) {
                read_error = errno;
            }
            break;
        }
    }

    fclose(fp);

    if (!success) {
        humanized_log_error("host", "failed to read reply state file",
                            read_error != 0 ? read_error : EIO);
        return;
    }

    ttak_mutex_lock(&host->lock);
    memset(host->replies, 0, sizeof(host->replies));
    host->reply_count = 0U;
    host->next_reply_id =
        header.next_reply_id != 0U ? header.next_reply_id : 1U;
    uint64_t max_reply_id = 0U;

    for (uint32_t idx = 0U; idx < entry_count; ++idx) {
        if (host->reply_count >= SSH_CHATTER_MAX_REPLIES) {
            if (entries[idx].reply_id > max_reply_id) {
                max_reply_id = entries[idx].reply_id;
            }
            continue;
        }

        chat_reply_entry_t *slot = &host->replies[host->reply_count];
        memset(slot, 0, sizeof(*slot));
        slot->in_use = true;
        slot->reply_id = entries[idx].reply_id != 0U
                             ? entries[idx].reply_id
                             : (uint64_t)(host->reply_count + 1U);
        if (slot->reply_id > max_reply_id) {
            max_reply_id = slot->reply_id;
        }
        slot->parent_message_id = entries[idx].parent_message_id;
        slot->parent_reply_id = entries[idx].parent_reply_id;
        slot->created_at = (time_t)entries[idx].created_at;
        snprintf(slot->username, sizeof(slot->username), "%s",
                 entries[idx].username);
        snprintf(slot->message, sizeof(slot->message), "%s",
                 entries[idx].message);
        ++host->reply_count;
    }

    if (host->next_reply_id <= max_reply_id) {
        if (max_reply_id == UINT64_MAX) {
            host->next_reply_id = UINT64_MAX;
        } else {
            host->next_reply_id = max_reply_id + 1U;
        }
    }

    if (host->next_reply_id == 0U) {
        host->next_reply_id = (uint64_t)host->reply_count + 1U;
    }

    ttak_mutex_unlock(&host->lock);
}

static void host_eliza_memory_resolve_path(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    const char *memory_path = getenv("CHATTER_ELIZA_MEMORY_FILE");
    if (memory_path == nullptr || memory_path[0] == '\0') {
        memory_path = "eliza_memory.dat";
    }

    int written =
        snprintf(host->eliza_memory_file_path,
                 sizeof(host->eliza_memory_file_path), "%s", memory_path);
    if (written < 0 ||
        (size_t)written >= sizeof(host->eliza_memory_file_path)) {
        humanized_log_error("host", "eliza memory file path is too long",
                            ENAMETOOLONG);
        host->eliza_memory_file_path[0] = '\0';
    }
}

static void host_eliza_memory_save_locked(host_t *host)
{
    if (host == nullptr || host->eliza_memory_file_path[0] == '\0') {
        return;
    }

    if (!host_ensure_private_data_path(host, host->eliza_memory_file_path,
                                       true)) {
        return;
    }

    char temp_path[PATH_MAX];
    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp",
                           host->eliza_memory_file_path);
    if (written < 0 || (size_t)written >= sizeof(temp_path)) {
        humanized_log_error("host", "eliza memory path is too long",
                            ENAMETOOLONG);
        return;
    }

    FILE *fp = fopen(temp_path, "wb");
    if (fp == nullptr) {
        humanized_log_error("host", "failed to open eliza memory file",
                            errno != 0 ? errno : EIO);
        return;
    }

    size_t stored = host->eliza_memory_count;
    if (stored > SSH_CHATTER_ELIZA_MEMORY_MAX) {
        stored = SSH_CHATTER_ELIZA_MEMORY_MAX;
    }

    eliza_memory_header_t header = {0};
    header.magic = ELIZA_MEMORY_MAGIC;
    header.version = ELIZA_MEMORY_VERSION;
    header.entry_count = (uint32_t)stored;
    header.next_id = host->eliza_memory_next_id;

    bool success = fwrite(&header, sizeof(header), 1U, fp) == 1U;
    int write_error = 0;
    if (!success && errno != 0) {
        write_error = errno;
    }

    for (size_t idx = 0U; success && idx < stored; ++idx) {
        const eliza_memory_entry_t *entry = &host->eliza_memory[idx];
        eliza_memory_entry_serialized_t serialized = {0};
        serialized.id = entry->id;
        serialized.stored_at = (int64_t)entry->stored_at;
        snprintf(serialized.prompt, sizeof(serialized.prompt), "%s",
                 entry->prompt);
        snprintf(serialized.reply, sizeof(serialized.reply), "%s",
                 entry->reply);
        if (fwrite(&serialized, sizeof(serialized), 1U, fp) != 1U) {
            success = false;
            if (errno != 0) {
                write_error = errno;
            }
            break;
        }
    }

    if (success && fflush(fp) != 0) {
        success = false;
        if (errno != 0) {
            write_error = errno;
        }
    }

    if (success) {
        int fd = fileno(fp);
        if (fd >= 0 && fsync(fd) != 0) {
            success = false;
            if (errno != 0) {
                write_error = errno;
            }
        }
    }

    if (fclose(fp) != 0) {
        if (success && errno != 0) {
            write_error = errno;
        }
        success = false;
    }

    if (!success) {
        unlink(temp_path);
        humanized_log_error("host", "failed to write eliza memory file",
                            write_error != 0 ? write_error : EIO);
        return;
    }

    if (rename(temp_path, host->eliza_memory_file_path) != 0) {
        int rename_error = errno != 0 ? errno : EIO;
        unlink(temp_path);
        humanized_log_error("host", "failed to install eliza memory file",
                            rename_error);
        return;
    }

    if (chmod(host->eliza_memory_file_path, S_IRUSR | S_IWUSR) != 0) {
        humanized_log_error("host", "failed to set eliza memory permissions",
                            errno != 0 ? errno : EACCES);
    }
}

static void host_eliza_memory_load(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    if (host->eliza_memory_file_path[0] == '\0') {
        return;
    }

    if (!host_ensure_private_data_path(host, host->eliza_memory_file_path,
                                       false)) {
        return;
    }

    FILE *fp = fopen(host->eliza_memory_file_path, "rb");
    if (fp == nullptr) {
        return;
    }

    eliza_memory_header_t header = {0};
    if (fread(&header, sizeof(header), 1U, fp) != 1U) {
        fclose(fp);
        return;
    }

    if (header.magic != ELIZA_MEMORY_MAGIC || header.version == 0U ||
        header.version > ELIZA_MEMORY_VERSION) {
        fclose(fp);
        return;
    }

    uint32_t entry_count = header.entry_count;
    eliza_memory_entry_serialized_t *entries = nullptr;
    if (entry_count > 0U) {
        entries = sshc_gc_calloc(entry_count, sizeof(*entries));
        if (entries == nullptr) {
            fclose(fp);
            humanized_log_error(
                "host", "failed to allocate eliza memory buffer", ENOMEM);
            return;
        }
    }

    bool success = true;
    int read_error = 0;
    for (uint32_t idx = 0U; idx < entry_count; ++idx) {
        if (fread(&entries[idx], sizeof(entries[idx]), 1U, fp) != 1U) {
            success = false;
            if (errno != 0) {
                read_error = errno;
            }
            break;
        }
    }

    fclose(fp);

    if (!success) {
        humanized_log_error("host", "failed to read eliza memory file",
                            read_error != 0 ? read_error : EIO);
        return;
    }

    ttak_mutex_lock(&host->lock);
    memset(host->eliza_memory, 0, sizeof(host->eliza_memory));
    host->eliza_memory_count = 0U;
    host->eliza_memory_next_id = header.next_id != 0U ? header.next_id : 1U;

    uint64_t max_id = 0U;
    for (uint32_t idx = 0U; idx < entry_count; ++idx) {
        uint64_t entry_id =
            entries[idx].id != 0U ? entries[idx].id : (uint64_t)(idx + 1U);
        if (idx < SSH_CHATTER_ELIZA_MEMORY_MAX) {
            eliza_memory_entry_t *slot =
                &host->eliza_memory[host->eliza_memory_count++];
            slot->id = entry_id;
            slot->stored_at = (time_t)entries[idx].stored_at;
            snprintf(slot->prompt, sizeof(slot->prompt), "%s",
                     entries[idx].prompt);
            snprintf(slot->reply, sizeof(slot->reply), "%s",
                     entries[idx].reply);
        }
        if (entry_id > max_id) {
            max_id = entry_id;
        }
    }

    if (max_id >= host->eliza_memory_next_id) {
        host->eliza_memory_next_id =
            (max_id == UINT64_MAX) ? UINT64_MAX : max_id + 1U;
    }
    if (host->eliza_memory_next_id == 0U) {
        host->eliza_memory_next_id = (uint64_t)host->eliza_memory_count + 1U;
    }

    ttak_mutex_unlock(&host->lock);
}

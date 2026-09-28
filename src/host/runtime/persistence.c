/* State that used to live only in memory: the small-talk personas'
 * conversation memory and the operator's runtime switches (small-talk
 * backend, Gemini on/off, translation scope, a DDial link set with
 * /ddial connect). Both are written atomically (temp file, fsync, rename)
 * and read back at startup; the memory also after an idle release. */

static const uint32_t AI_MEMORY_STATE_MAGIC = 0x41494d4dU;   /* 'AIMM' */
static const uint32_t AI_MEMORY_STATE_VERSION = 1U;
static const uint32_t RUNTIME_SETTINGS_MAGIC = 0x52545354U;  /* 'RTST' */
static const uint32_t RUNTIME_SETTINGS_VERSION = 1U;

typedef struct ai_memory_state_header {
    uint32_t magic;
    uint32_t version;
    uint32_t entry_count;
    uint32_t reserved;
} ai_memory_state_header_t;

typedef struct ai_memory_entry_serialized {
    int64_t stored_at;
    char username[SSH_CHATTER_USERNAME_LEN];
    char prompt[SSH_CHATTER_MESSAGE_LIMIT];
    char reply[SSH_CHATTER_MESSAGE_LIMIT];
} ai_memory_entry_serialized_t;

typedef struct runtime_settings_record {
    uint32_t magic;
    uint32_t version;
    uint8_t ai_chat_use_gemini;
    uint8_t gemini_disabled;
    uint8_t translate_chat_bbs_only;
    uint8_t translate_skip_scrollback;
    uint8_t ddial_mode;
    uint8_t reserved[3];
    int32_t ddial_port;
    char ddial_host[256];
    char ddial_handle[SSH_CHATTER_USERNAME_LEN];
    char ddial_key[256];
    uint8_t reserved_tail[64];
} runtime_settings_record_t;

static void host_persist_resolve_path(char *out, size_t out_len,
                                      const char *env_name,
                                      const char *fallback)
{
    const char *path = getenv(env_name);
    if (path == nullptr || path[0] == '\0') {
        path = fallback;
    }
    int written = snprintf(out, out_len, "%s", path);
    if (written < 0 || (size_t)written >= out_len) {
        humanized_log_error("host", "state file path is too long",
                            ENAMETOOLONG);
        out[0] = '\0';
    }
}

/* Write header+payload to path via a fsynced temp file and rename. */
static bool host_persist_write_atomic(host_t *host, const char *path,
                                      const void *header, size_t header_len,
                                      const void *payload, size_t payload_len)
{
    if (path == nullptr || path[0] == '\0' ||
        !host_ensure_private_data_path(host, path, true)) {
        return false;
    }

    char temp_path[PATH_MAX];
    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    if (written < 0 || (size_t)written >= sizeof(temp_path)) {
        return false;
    }

    FILE *fp = fopen(temp_path, "wb");
    if (fp == nullptr) {
        humanized_log_error("host", "failed to open state file",
                            errno != 0 ? errno : EIO);
        return false;
    }

    bool ok = fwrite(header, header_len, 1U, fp) == 1U;
    if (ok && payload_len > 0U) {
        ok = fwrite(payload, payload_len, 1U, fp) == 1U;
    }
    ok = ok && fflush(fp) == 0 && fsync(fileno(fp)) == 0;
    if (fclose(fp) != 0) {
        ok = false;
    }
    if (ok) {
        (void)chmod(temp_path, S_IRUSR | S_IWUSR);
        ok = rename(temp_path, path) == 0;
    }
    if (!ok) {
        humanized_log_error("host", "failed to write state file",
                            errno != 0 ? errno : EIO);
        (void)remove(temp_path);
    }
    return ok;
}

/* ---- small-talk memory -------------------------------------------------- */

static void host_ai_chat_memory_save_locked(host_t *host)
{
    if (host == nullptr || host->ai_chat_memory == nullptr ||
        host->ai_chat_memory_file_path[0] == '\0') {
        return;
    }

    size_t count = host->ai_chat_memory_count;
    if (count > SSH_CHATTER_AI_MEMORY_MAX) {
        count = SSH_CHATTER_AI_MEMORY_MAX;
    }

    ai_memory_entry_serialized_t *entries = nullptr;
    if (count > 0U) {
        entries = (ai_memory_entry_serialized_t *)sshc_gc_calloc(
            count, sizeof(*entries));
        if (entries == nullptr) {
            return;
        }
        for (size_t idx = 0U; idx < count; ++idx) {
            const ai_chat_memory_entry_t *entry = &host->ai_chat_memory[idx];
            entries[idx].stored_at = (int64_t)entry->stored_at;
            snprintf(entries[idx].username, sizeof(entries[idx].username),
                     "%s", entry->username);
            snprintf(entries[idx].prompt, sizeof(entries[idx].prompt), "%s",
                     entry->prompt);
            snprintf(entries[idx].reply, sizeof(entries[idx].reply), "%s",
                     entry->reply);
        }
    }

    ai_memory_state_header_t header = {
        .magic = AI_MEMORY_STATE_MAGIC,
        .version = AI_MEMORY_STATE_VERSION,
        .entry_count = (uint32_t)count,
    };
    (void)host_persist_write_atomic(host, host->ai_chat_memory_file_path,
                                    &header, sizeof(header), entries,
                                    count * sizeof(*entries));
    if (entries != nullptr) {
        sshc_gc_free(entries);
    }
}

static void host_ai_chat_memory_load(host_t *host)
{
    if (host == nullptr || host->ai_chat_memory_file_path[0] == '\0' ||
        !host_ensure_private_data_path(host, host->ai_chat_memory_file_path,
                                       false)) {
        return;
    }

    FILE *fp = fopen(host->ai_chat_memory_file_path, "rb");
    if (fp == nullptr) {
        return;
    }

    ai_memory_state_header_t header = {0};
    ai_memory_entry_serialized_t *entries = nullptr;
    size_t count = 0U;
    if (fread(&header, sizeof(header), 1U, fp) == 1U &&
        header.magic == AI_MEMORY_STATE_MAGIC && header.version != 0U &&
        header.version <= AI_MEMORY_STATE_VERSION) {
        count = header.entry_count;
        if (count > SSH_CHATTER_AI_MEMORY_MAX) {
            count = SSH_CHATTER_AI_MEMORY_MAX;
        }
        if (count > 0U) {
            entries = (ai_memory_entry_serialized_t *)sshc_gc_calloc(
                count, sizeof(*entries));
            if (entries == nullptr ||
                fread(entries, sizeof(*entries), count, fp) != count) {
                count = 0U;
            }
        }
    }
    fclose(fp);

    ttak_mutex_lock(&host->lock);
    host_ai_chat_memory_ensure(host);
    if (host->ai_chat_memory != nullptr) {
        host->ai_chat_memory_count = 0U;
        for (size_t idx = 0U; idx < count; ++idx) {
            ai_chat_memory_entry_t *slot =
                &host->ai_chat_memory[host->ai_chat_memory_count++];
            slot->stored_at = (time_t)entries[idx].stored_at;
            snprintf(slot->username, sizeof(slot->username), "%.*s",
                     (int)sizeof(slot->username) - 1, entries[idx].username);
            snprintf(slot->prompt, sizeof(slot->prompt), "%.*s",
                     (int)sizeof(slot->prompt) - 1, entries[idx].prompt);
            snprintf(slot->reply, sizeof(slot->reply), "%.*s",
                     (int)sizeof(slot->reply) - 1, entries[idx].reply);
        }
    }
    ttak_mutex_unlock(&host->lock);

    if (entries != nullptr) {
        sshc_gc_free(entries);
    }
}

/* After an idle release the memory is reloaded from its file rather than
 * starting over empty. */
static void host_ai_chat_memory_restore(host_t *host)
{
    if (host == nullptr) {
        return;
    }
    if (host->ai_chat_memory == nullptr) {
        host_ai_chat_memory_load(host);
    }
    host_ai_chat_memory_ensure(host);
}

static void host_eliza_memory_restore(host_t *host)
{
    if (host == nullptr) {
        return;
    }
    if (host->eliza_memory_released) {
        host->eliza_memory_released = false;
        host_eliza_memory_load(host);
    }
    host_eliza_memory_ensure(host);
}

/* ---- operator runtime switches ----------------------------------------- */

static void host_runtime_settings_save(host_t *host)
{
    if (host == nullptr || host->runtime_settings_file_path[0] == '\0') {
        return;
    }

    runtime_settings_record_t record;
    memset(&record, 0, sizeof(record));
    record.magic = RUNTIME_SETTINGS_MAGIC;
    record.version = RUNTIME_SETTINGS_VERSION;
    record.gemini_disabled = translator_is_gemini_manually_disabled() ? 1U : 0U;
    record.translate_chat_bbs_only =
        translator_is_manual_chat_bbs_only() ? 1U : 0U;
    record.translate_skip_scrollback =
        translator_is_manual_skip_scrollback() ? 1U : 0U;

    ttak_mutex_lock(&host->lock);
    record.ai_chat_use_gemini = host->ai_chat_use_gemini ? 1U : 0U;
    record.ddial_mode = host->runtime_ddial_mode;
    ttak_mutex_unlock(&host->lock);

    if (record.ddial_mode == RUNTIME_DDIAL_LINK) {
        const ddial_client_t *client =
            (const ddial_client_t *)&host->ddial_relay;
        record.ddial_port = client->port;
        snprintf(record.ddial_host, sizeof(record.ddial_host), "%s",
                 client->host);
        snprintf(record.ddial_handle, sizeof(record.ddial_handle), "%s",
                 client->handle);
        snprintf(record.ddial_key, sizeof(record.ddial_key), "%s",
                 client->key);
    }

    (void)host_persist_write_atomic(host, host->runtime_settings_file_path,
                                    &record, sizeof(record), nullptr, 0U);
}

/* Remember how the operator left the DDial link: a /ddial connect target,
 * an explicit /ddial disconnect, or nothing (follow the environment). */
static void host_runtime_settings_note_ddial(host_t *host, uint8_t mode)
{
    if (host == nullptr) {
        return;
    }
    ttak_mutex_lock(&host->lock);
    host->runtime_ddial_mode = mode;
    ttak_mutex_unlock(&host->lock);
    host_runtime_settings_save(host);
}

/* Startup: apply what the operator last set. Runs before the DDial client
 * starts, so a saved link replaces the environment's. */
static void host_runtime_settings_load(host_t *host)
{
    if (host == nullptr || host->runtime_settings_file_path[0] == '\0' ||
        !host_ensure_private_data_path(host, host->runtime_settings_file_path,
                                       false)) {
        return;
    }

    FILE *fp = fopen(host->runtime_settings_file_path, "rb");
    if (fp == nullptr) {
        return;
    }
    runtime_settings_record_t record;
    memset(&record, 0, sizeof(record));
    bool ok = fread(&record, sizeof(record), 1U, fp) == 1U;
    fclose(fp);
    if (!ok || record.magic != RUNTIME_SETTINGS_MAGIC ||
        record.version == 0U || record.version > RUNTIME_SETTINGS_VERSION) {
        return;
    }
    record.ddial_host[sizeof(record.ddial_host) - 1U] = '\0';
    record.ddial_handle[sizeof(record.ddial_handle) - 1U] = '\0';
    record.ddial_key[sizeof(record.ddial_key) - 1U] = '\0';

    ttak_mutex_lock(&host->lock);
    host->ai_chat_use_gemini = record.ai_chat_use_gemini != 0U;
    host->runtime_ddial_mode = record.ddial_mode;
    ttak_mutex_unlock(&host->lock);

    if (record.gemini_disabled != 0U) {
        translator_set_gemini_enabled(false);
    }
    translator_set_manual_chat_bbs_only(record.translate_chat_bbs_only != 0U);
    translator_set_manual_skip_scrollback(record.translate_skip_scrollback !=
                                          0U);

    if (record.ddial_mode == RUNTIME_DDIAL_LINK &&
        record.ddial_host[0] != '\0' && record.ddial_port > 0) {
        printf("[ddial] restoring operator link %s:%d\n", record.ddial_host,
               record.ddial_port);
        (void)host_ddial_client_configure(
            host, record.ddial_host, record.ddial_port, record.ddial_handle,
            record.ddial_key[0] != '\0' ? record.ddial_key : nullptr);
    } else if (record.ddial_mode == RUNTIME_DDIAL_DISCONNECTED) {
        printf("[ddial] upstream link left disconnected by an operator\n");
        host_ddial_client_disconnect(host);
    }
}

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "ssh_chatter/host.h"
#include "ssh_chatter/user_data.h"
#include "ssh_chatter/security_layer.h"

#include <ctype.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define USER_DATA_MAGIC 0x4D424F58U /* 'MBOX' */
#define USER_DATA_VERSION 6U

#define USER_DATA_PROFILE_DIRECTORY "profiles"
#define USER_DATA_VARIANT_LIMIT 32U

static bool user_data_profile_directory_path(const char *root, char *path,
                                             size_t length);
static bool user_data_profile_picture_path(const char *root,
                                           const char *username, char *path,
                                           size_t length);
static void user_data_profile_picture_overlay(const char *root,
                                              const char *username,
                                              user_data_record_t *record);
static bool user_data_profile_picture_store(const char *root,
                                            const user_data_record_t *record);

static bool user_data_should_skip_osc_terminator(const char *text, size_t idx)
{
    return text[idx] == '\033' && text[idx + 1U] != '\0' &&
           text[idx + 1U] == '\\';
}

bool user_data_strip_ansi_sequences(const char *restrict input,
                                    char *restrict output, size_t length)
{
    if (output == nullptr || length == 0U) {
        return false;
    }

    output[0] = '\0';
    if (input == nullptr) {
        return false;
    }

    size_t out_idx = 0U;
    for (size_t idx = 0U; input[idx] != '\0';) {
        unsigned char ch = (unsigned char)input[idx];
        if (ch == '\033') {
            ++idx; // Skip ESC
            if (input[idx] == '\0') {
                break;
            }
            if (input[idx] == '[') {
                ++idx;
                while (input[idx] != '\0' &&
                       !(input[idx] >= '@' && input[idx] <= '~')) {
                    ++idx;
                }
                if (input[idx] != '\0') {
                    ++idx; // Consume final byte
                }
                continue;
            }

            if (input[idx] == ']') {
                ++idx;
                while (input[idx] != '\0' && input[idx] != '\007' &&
                       !user_data_should_skip_osc_terminator(input, idx)) {
                    ++idx;
                }
                if (input[idx] == '\007') {
                    ++idx;
                } else if (user_data_should_skip_osc_terminator(input, idx)) {
                    idx += 2U;
                }
                continue;
            }

            continue;
        }

        if (iscntrl(ch) && ch != '\t') {
            ++idx;
            continue;
        }

        if (out_idx + 1U < length) {
            output[out_idx++] = (char)ch;
        }
        ++idx;
    }

    output[out_idx] = '\0';
    return true;
}

static size_t user_data_column_reset_sequence_length(const char *text)
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

static void user_data_strip_column_reset(char *text)
{
    if (text == nullptr || text[0] == '\0') {
        return;
    }

    char *dst = text;
    const char *src = text;
    while (*src != '\0') {
        size_t skip = user_data_column_reset_sequence_length(src);
        if (skip > 0U) {
            src += skip;
            continue;
        }

        *dst++ = *src++;
    }

    *dst = '\0';
}

static bool user_data_is_directory(const char *path)
{
    if (path == nullptr || path[0] == '\0') {
        return false;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }

    return S_ISDIR(st.st_mode);
}

static bool user_data_create_directory(const char *path)
{
    if (path == nullptr || path[0] == '\0') {
        return false;
    }

    if (user_data_is_directory(path)) {
        return true;
    }

    if (mkdir(path, 0750) == 0) {
        return true;
    }

    if (errno == EEXIST) {
        return user_data_is_directory(path);
    }

    return false;
}

static bool user_data_ensure_parent(const char *path)
{
    if (path == nullptr || path[0] == '\0') {
        return false;
    }

    char temp[PATH_MAX];
    snprintf(temp, sizeof(temp), "%s", path);
    char *parent = dirname(temp);
    if (parent == nullptr || parent[0] == '\0') {
        return false;
    }

    return user_data_create_directory(parent);
}

static bool user_data_file_exists(const char *path)
{
    if (path == nullptr || path[0] == '\0') {
        return false;
    }

    return access(path, F_OK) == 0;
}

static bool user_data_build_variant_name(const char *base, size_t index,
                                         char *buffer, size_t length)
{
    if (buffer == nullptr || length == 0U || base == nullptr ||
        base[0] == '\0') {
        return false;
    }

    int written;
    if (index == 0U) {
        written = snprintf(buffer, length, "%s", base);
    } else {
        written = snprintf(buffer, length, "%s-%zu", base, index);
    }

    return written >= 0 && (size_t)written < length;
}

static bool user_data_load_raw(const char *path, user_data_record_t *record,
                               bool *needs_upgrade)
{
    if (record == nullptr || path == nullptr || path[0] == '\0') {
        return false;
    }

    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        return false;
    }

    struct stat st;
    if (fstat(fileno(fp), &st) != 0) {
        fclose(fp);
        return false;
    }

    if (fseek(fp, 0L, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }

    if (st.st_size < 0) {
        fclose(fp);
        return false;
    }

    const size_t file_size = (size_t)st.st_size;
    const size_t expected_size = sizeof(user_data_record_t);

    user_data_record_t temp;
    memset(&temp, 0, sizeof(temp));

    const size_t to_read =
        file_size < expected_size ? file_size : expected_size;
    bool loaded = fread(&temp, 1U, to_read, fp) == to_read &&
                  temp.magic == USER_DATA_MAGIC && temp.version > 0U &&
                  temp.version <= USER_DATA_VERSION;

    fclose(fp);

    if (!loaded) {
        return false;
    }

    if (needs_upgrade != nullptr) {
        *needs_upgrade =
            temp.version != USER_DATA_VERSION || file_size != expected_size;
    }

    *record = temp;
    return true;
}

bool user_data_ensure_root(const char *restrict root)
{
    if (root == nullptr || root[0] == '\0') {
        return false;
    }

    if (!user_data_create_directory(root)) {
        return false;
    }

    char profile_root[PATH_MAX];
    if (!user_data_profile_directory_path(root, profile_root,
                                          sizeof(profile_root))) {
        return false;
    }

    return user_data_create_directory(profile_root);
}

bool user_data_sanitize_username(const char *restrict username,
                                 char *restrict sanitized, size_t length)
{
    if (sanitized == nullptr || length == 0U) {
        return false;
    }

    sanitized[0] = '\0';
    if (username == nullptr || username[0] == '\0') {
        return false;
    }

    char temp_username[SSH_CHATTER_USERNAME_LEN + 1]; // +1 for null terminator
    strncpy(temp_username, username, SSH_CHATTER_USERNAME_LEN);
    temp_username[SSH_CHATTER_USERNAME_LEN] = '\0'; // Ensure null termination

    // Convert all ASCII uppercase characters to lowercase
    for (size_t idx = 0U; temp_username[idx] != '\0'; ++idx) {
        if (temp_username[idx] >= 'A' && temp_username[idx] <= 'Z') {
            temp_username[idx] =
                (char)tolower((unsigned char)temp_username[idx]);
        }
    }

    size_t out_idx = 0U;
    for (size_t idx = 0U; temp_username[idx] != '\0'; ++idx) {
        unsigned char ch = (unsigned char)temp_username[idx];
        if (isalnum(ch)) {
            if (out_idx + 1U < length) {
                sanitized[out_idx++] =
                    (char)ch; // Already lowercased if it was uppercase
            }
        } else if (ch == '-' || ch == '_' || ch == '.') {
            if (out_idx + 1U < length) {
                sanitized[out_idx++] = (char)ch;
            }
        } else if (!isspace(ch)) {
            if (out_idx + 1U < length) {
                sanitized[out_idx++] = '_';
            }
        }
    }

    if (out_idx == 0U) {
        if (length < 5U) {
            return false;
        }
        snprintf(sanitized, length, "user");
        return true;
    }

    sanitized[out_idx] = '\0';
    return true;
}

bool user_data_path_for(const char *restrict root,
                        const char *restrict username, const char *restrict ip,
                        bool create_if_missing, char *restrict path,
                        size_t length)
{
    if (path == nullptr || length == 0U || root == nullptr || root[0] == '\0') {
        return false;
    }

    char sanitized[SSH_CHATTER_USERNAME_LEN * 2U];
    if (!user_data_sanitize_username(username, sanitized, sizeof(sanitized))) {
        return false;
    }

    // Attempt to find a user data file matching just the username first.
    // This prioritizes users with passwords, allowing them to retain their
    // preferred nickname even if they roam across different IP addresses.
    char username_only_path[PATH_MAX];
    int written = snprintf(username_only_path, sizeof(username_only_path),
                           "%s/%s.dat", root, sanitized);
    if (written >= 0 && (size_t)written < length) {
        if (user_data_file_exists(username_only_path)) {
            user_data_record_t existing;
            if (user_data_load_raw(username_only_path, &existing, nullptr)) {
                // If there's a password hash, prioritize this record.
                if (!security_layer_is_zero_hash(existing.password_hash,
                                                 sizeof(existing.password_hash))) {
                    memcpy(path, username_only_path, (size_t)written + 1U);
                    return true;
                }
            }
        }
    }

    // If no password-protected username-only record was found, proceed with
    // IP-based matching or creation for non-password users.
    // This is essentially the original logic.
    if (ip == nullptr || ip[0] == '\0') {
        int final_written = snprintf(path, length, "%s/%s.dat", root, sanitized);
        return final_written >= 0 && (size_t)final_written < length;
    }

    size_t available_index = USER_DATA_VARIANT_LIMIT;
    char candidate_name[SSH_CHATTER_USERNAME_LEN * 2U];
    char candidate_path[PATH_MAX];
    for (size_t idx = 0U; idx < USER_DATA_VARIANT_LIMIT; ++idx) {
        if (!user_data_build_variant_name(sanitized, idx, candidate_name,
                                          sizeof(candidate_name))) {
            continue;
        }

        written = snprintf(candidate_path, sizeof(candidate_path),
                               "%s/%s.dat", root, candidate_name);
        if (written < 0 || (size_t)written >= sizeof(candidate_path)) {
            continue;
        }

        if (user_data_file_exists(candidate_path)) {
            user_data_record_t existing;
            if (user_data_load_raw(candidate_path, &existing, nullptr)) {
                bool username_match = strncmp(existing.username, username,
                                              sizeof(existing.username)) == 0;
                bool ip_match =
                    strncmp(existing.last_ip, ip, SSH_CHATTER_IP_LEN) == 0;
                if (username_match && ip_match) {
                    if ((size_t)written < length) {
                        memcpy(path, candidate_path, (size_t)written + 1U);
                        return true;
                    }
                    return false;
                }
            }
            continue;
        }

        if (available_index == USER_DATA_VARIANT_LIMIT) {
            available_index = idx;
        }
    }

    if (!create_if_missing || available_index == USER_DATA_VARIANT_LIMIT) {
        return false;
    }

    if (!user_data_build_variant_name(sanitized, available_index,
                                      candidate_name, sizeof(candidate_name))) {
        return false;
    }

    written = snprintf(path, length, "%s/%s.dat", root, candidate_name);
    return written >= 0 && (size_t)written < length;
}

static bool user_data_profile_directory_path(const char *root, char *path,
                                             size_t length)
{
    if (path == nullptr || length == 0U || root == nullptr || root[0] == '\0') {
        return false;
    }

    int written =
        snprintf(path, length, "%s/%s", root, USER_DATA_PROFILE_DIRECTORY);
    if (written < 0 || (size_t)written >= length) {
        return false;
    }

    return true;
}

static bool user_data_profile_picture_path(const char *root,
                                           const char *username, char *path,
                                           size_t length)
{
    if (path == nullptr || length == 0U || root == nullptr || root[0] == '\0' ||
        username == nullptr || username[0] == '\0') {
        return false;
    }

    char sanitized[SSH_CHATTER_USERNAME_LEN * 2U];
    if (!user_data_sanitize_username(username, sanitized, sizeof(sanitized))) {
        return false;
    }

    if (sanitized[0] == '\0') {
        return false;
    }

    char directory[PATH_MAX];
    if (!user_data_profile_directory_path(root, directory, sizeof(directory))) {
        return false;
    }

    int written = snprintf(path, length, "%s/%s.dat", directory, sanitized);
    if (written < 0 || (size_t)written >= length) {
        return false;
    }

    return true;
}

static void user_data_profile_picture_overlay(const char *root,
                                              const char *username,
                                              user_data_record_t *record)
{
    if (record == nullptr || root == nullptr || root[0] == '\0' ||
        username == nullptr || username[0] == '\0') {
        return;
    }

    char path[PATH_MAX];
    if (!user_data_profile_picture_path(root, username, path, sizeof(path))) {
        return;
    }

    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        return;
    }

    char buffer[USER_DATA_PROFILE_PICTURE_LEN];
    size_t read = fread(buffer, 1U, sizeof(buffer) - 1U, fp);
    if (ferror(fp) != 0) {
        fclose(fp);
        return;
    }

    fclose(fp);

    buffer[read] = '\0';
    user_data_strip_column_reset(buffer);
    buffer[USER_DATA_PROFILE_PICTURE_LEN - 1U] = '\0';
    snprintf(record->profile_picture, sizeof(record->profile_picture), "%s",
             buffer);
}

static bool user_data_profile_picture_store(const char *root,
                                            const user_data_record_t *record)
{
    if (root == nullptr || root[0] == '\0' || record == nullptr ||
        record->username[0] == '\0') {
        return false;
    }

    char path[PATH_MAX];
    if (!user_data_profile_picture_path(root, record->username, path,
                                        sizeof(path))) {
        return false;
    }

    if (record->profile_picture[0] == '\0') {
        if (unlink(path) == 0) {
            return true;
        }
        if (errno == ENOENT) {
            return true;
        }
        return false;
    }

    char directory[PATH_MAX];
    if (!user_data_profile_directory_path(root, directory, sizeof(directory))) {
        return false;
    }

    if (!user_data_create_directory(directory)) {
        return false;
    }

    char temp_path[PATH_MAX];
    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    if (written < 0 || (size_t)written >= sizeof(temp_path)) {
        return false;
    }

    FILE *fp = fopen(temp_path, "wb");
    if (fp == nullptr) {
        return false;
    }

    const char *picture = record->profile_picture;
    size_t remaining = strlen(picture);
    bool success = true;
    while (remaining > 0U) {
        size_t chunk = fwrite(picture, 1U, remaining, fp);
        if (chunk == 0U) {
            success = false;
            break;
        }
        picture += chunk;
        remaining -= chunk;
    }

    int error = success ? 0 : errno;
    if (success && fflush(fp) != 0) {
        success = false;
        error = errno;
    }

    if (success) {
        int fd = fileno(fp);
        if (fd >= 0 && fsync(fd) != 0) {
            success = false;
            error = errno;
        }
    }

    if (fclose(fp) != 0) {
        if (success) {
            error = errno;
        }
        success = false;
    }

    if (!success) {
        unlink(temp_path);
        errno = error != 0 ? error : EIO;
        return false;
    }

    if (rename(temp_path, path) != 0) {
        int rename_error = errno;
        unlink(temp_path);
        errno = rename_error;
        return false;
    }

    return true;
}

static void user_data_normalize_record(user_data_record_t *record,
                                       const char *username)
{
    if (record == nullptr) {
        return;
    }

    if (record->mailbox_count > USER_DATA_MAILBOX_LIMIT) {
        record->mailbox_count = USER_DATA_MAILBOX_LIMIT;
    }
    if (record->flag_history_count > USER_DATA_FLAG_HISTORY_LIMIT) {
        record->flag_history_count = USER_DATA_FLAG_HISTORY_LIMIT;
    }
    record->has_user_theme = record->has_user_theme ? 1U : 0U;
    record->user_is_bold = record->user_is_bold ? 1U : 0U;
    record->user_color_code[SSH_CHATTER_COLOR_CODE_LEN - 1U] = '\0';
    record->user_highlight_code[SSH_CHATTER_COLOR_CODE_LEN - 1U] = '\0';
    record->user_color_name[SSH_CHATTER_COLOR_NAME_LEN - 1U] = '\0';
    record->user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN - 1U] = '\0';
    record->profile_picture[USER_DATA_PROFILE_PICTURE_LEN - 1U] = '\0';
    record->last_ip[SSH_CHATTER_IP_LEN - 1U] = '\0';
    user_data_strip_column_reset(record->profile_picture);
    if (username != nullptr && username[0] != '\0') {
        snprintf(record->username, sizeof(record->username), "%s", username);
    }
}

bool user_data_init(user_data_record_t *restrict record,
                    const char *restrict username, const char *restrict ip)
{
    if (record == nullptr) {
        return false;
    }

    memset(record, 0, sizeof(*record));
    record->magic = USER_DATA_MAGIC;
    record->version = USER_DATA_VERSION;
    if (username != nullptr) {
        snprintf(record->username, sizeof(record->username), "%s", username);
    }
    record->has_user_theme = 0U;
    record->user_is_bold = 0U;
    if (ip != nullptr) {
        snprintf(record->last_ip, sizeof(record->last_ip), "%s", ip);
    }
    record->alpha.active = 0U;
    record->alpha.stage = 0U;
    record->alpha.eva_ready = 0U;
    record->alpha.awaiting_flag = 0U;
    record->alpha.velocity_fraction_c = 0.0;
    record->alpha.distance_travelled_ly = 0.0;
    record->alpha.distance_remaining_ly = 4.24;
    record->alpha.fuel_percent = 100.0;
    record->alpha.oxygen_days = 730.0;
    record->alpha.mission_time_years = 0.0;
    record->alpha.radiation_msv = 0.0;
    record->mailbox_count = 0U;
    record->flag_count = 0U;
    record->flag_history_count = 0U;
    record->last_updated = (uint64_t)time(nullptr);
    memset(record->reserved, 0, sizeof(record->reserved));

    // Initialize password salt and hash
    security_layer_generate_salt(record->password_salt);
    memset(record->password_hash, 0, sizeof(record->password_hash));

    return true;
}

bool user_data_load(const char *restrict root, const char *restrict username,
                    const char *restrict ip,
                    user_data_record_t *restrict record)
{
    if (record == nullptr) {
        return false;
    }

    char path[PATH_MAX];
    if (!user_data_path_for(root, username, ip, false, path, sizeof(path))) {
        return false;
    }

    user_data_record_t temp;
    bool needs_upgrade = false;
    if (!user_data_load_raw(path, &temp, &needs_upgrade)) {
        return false;
    }

    user_data_normalize_record(&temp, username);
    user_data_profile_picture_overlay(root, username, &temp);
    if (needs_upgrade) {
        temp.version = USER_DATA_VERSION;
    }
    if (ip != nullptr && ip[0] != '\0') {
        snprintf(temp.last_ip, sizeof(temp.last_ip), "%s", ip);
    }
    *record = temp;
    return true;
}

bool user_data_save(const char *restrict root,
                    const user_data_record_t *restrict record,
                    const char *restrict ip)
{
    if (record == nullptr) {
        return false;
    }

    char path[PATH_MAX];
    const char *effective_ip =
        (ip != nullptr && ip[0] != '\0') ? ip : record->last_ip;
    if (!user_data_path_for(root, record->username, effective_ip, true, path,
                            sizeof(path))) {
        return false;
    }

    if (!user_data_ensure_root(root)) {
        return false;
    }

    if (!user_data_ensure_parent(path)) {
        return false;
    }

    char temp_path[PATH_MAX];
    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    if (written < 0 || (size_t)written >= sizeof(temp_path)) {
        return false;
    }

    FILE *fp = fopen(temp_path, "wb");
    if (fp == nullptr) {
        return false;
    }

    user_data_record_t normalized = *record;
    user_data_normalize_record(&normalized, record->username);
    normalized.magic = USER_DATA_MAGIC;
    normalized.version = USER_DATA_VERSION;
    if (effective_ip != nullptr && effective_ip[0] != '\0') {
        snprintf(normalized.last_ip, sizeof(normalized.last_ip), "%s",
                 effective_ip);
    }

    user_data_record_t disk_record = normalized;
    /* Profile pictures are persisted in dedicated per-user .dat files. */
    memset(disk_record.profile_picture, 0, sizeof(disk_record.profile_picture));

    bool success = fwrite(&disk_record, sizeof(disk_record), 1U, fp) == 1U;
    int error = success ? 0 : errno;
    if (success && fflush(fp) != 0) {
        success = false;
        error = errno;
    }

    if (success) {
        int fd = fileno(fp);
        if (fd >= 0 && fsync(fd) != 0) {
            success = false;
            error = errno;
        }
    }

    if (fclose(fp) != 0) {
        if (success) {
            error = errno;
        }
        success = false;
    }

    if (!success) {
        unlink(temp_path);
        errno = error;
        return false;
    }

    if (rename(temp_path, path) != 0) {
        int rename_error = errno;
        unlink(temp_path);
        errno = rename_error;
        return false;
    }

    if (!user_data_profile_picture_store(root, &normalized)) {
        return false;
    }

    return true;
}

bool user_data_ensure_exists(const char *restrict root,
                             const char *restrict username,
                             const char *restrict ip,
                             user_data_record_t *restrict record)
{
    if (record != nullptr && user_data_load(root, username, ip, record)) {
        return true;
    }

    user_data_record_t temp;
    if (!user_data_init(&temp, username, ip)) {
        return false;
    }

    if (!user_data_save(root, &temp, ip)) {
        return false;
    }

    if (record != nullptr) {
        *record = temp;
    }
    return true;
}

void user_data_set_ssh_chat_server_config(user_data_record_t *restrict record,
                                          const char *restrict url,
                                          uint16_t port)
{
    if (record == nullptr) {
        return;
    }
    if (url != nullptr) {
        strncpy(record->ssh_chat_server_url, url,
                sizeof(record->ssh_chat_server_url) - 1);
        record->ssh_chat_server_url[sizeof(record->ssh_chat_server_url) - 1] =
            '\0';
    } else {
        record->ssh_chat_server_url[0] = '\0';
    }
    record->ssh_chat_server_port = port;
}

void user_data_get_ssh_chat_server_config(
    const user_data_record_t *restrict record, char *restrict url,
    size_t url_len, uint16_t *restrict port)
{
    if (record == nullptr) {
        if (url != nullptr && url_len > 0) {
            url[0] = '\0';
        }
        if (port != nullptr) {
            *port = 0;
        }
        return;
    }

    if (url != nullptr && url_len > 0) {
        strncpy(url, record->ssh_chat_server_url, url_len - 1);
        url[url_len - 1] = '\0';
    }
    if (port != nullptr) {
        *port = record->ssh_chat_server_port;
    }
}

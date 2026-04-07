#include "host_parts/host_internal.h"

#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <fcntl.h>
#include <poll.h>
#ifndef WITH_SERVER
#define WITH_SERVER 1
#endif
#include <libssh/sftp.h>
#include <libgen.h>

#define FILE_STORAGE_LIST_LIMIT 64
#define FILE_TRANSFER_BUFFER 65536
#define ZMODEM_IO_CHUNK 4096
#define ZMODEM_POLL_TIMEOUT_MS 200

typedef enum {
    SCP_MODE_UPLOAD,
    SCP_MODE_DOWNLOAD,
} scp_mode_t;

typedef enum {
    SFTP_HANDLE_FILE,
    SFTP_HANDLE_DIR
} sftp_handle_type_t;

typedef struct {
    sftp_handle_type_t type;
    union {
        int fd;
        DIR *dir;
    } u;
    char path[PATH_MAX];
} sftp_handle_data_t;

static bool file_storage_ensure_directory(const char *path);
static bool file_storage_ensure_parent(const char *path);
static bool file_storage_prepare_staging(host_t *host, char *directory,
                                         size_t length);
static bool file_storage_locate_single_file(const char *root,
                                            char *absolute_path,
                                            size_t length);
static void file_storage_remove_tree(const char *root);
static bool file_storage_is_ready(host_t *host, session_ctx_t *ctx);

static bool scp_parse_command(const char *command, scp_mode_t *mode,
                              char *virtual_path, size_t path_length);
static bool scp_send_status(session_ctx_t *ctx, unsigned char code,
                            const char *message);
static int scp_expect_byte(session_ctx_t *ctx);
static bool scp_expect_ok(session_ctx_t *ctx);
static bool scp_send_ok(session_ctx_t *ctx);
static ssize_t scp_channel_read(session_ctx_t *ctx, void *buffer,
                                size_t length);
static bool scp_channel_write_all(session_ctx_t *ctx, const void *buffer,
                                  size_t length);
static int scp_handle_upload(session_ctx_t *ctx, const char *virtual_path);
static int scp_handle_download(session_ctx_t *ctx, const char *virtual_path);

static void telnet_force_binary(session_ctx_t *ctx);
static bool telnet_binary_write(session_ctx_t *ctx, const unsigned char *data,
                                size_t length);
static ssize_t telnet_binary_read(session_ctx_t *ctx, unsigned char *buffer,
                                  size_t length);
static bool telnet_spawn_zmodem(session_ctx_t *ctx, char *const argv[],
                                const char *working_dir,
                                const char *label);

static sftp_attributes sftp_attr_from_stat(const char *name, struct stat *st)
{
    // libssh 0.11 doesn't seem to have sftp_attributes_new in public headers,
    // so we allocate the structure ourselves.
    sftp_attributes attr = calloc(1, sizeof(struct sftp_attributes_struct));
    if (attr == nullptr) {
        return nullptr;
    }

    if (name != nullptr) {
        attr->name = strdup(name);
    }
    attr->flags = SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_UIDGID |
                  SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_ACMODTIME;
    attr->size = (uint64_t)st->st_size;
    attr->uid = (uint32_t)st->st_uid;
    attr->gid = (uint32_t)st->st_gid;
    attr->permissions = (uint32_t)st->st_mode;
    attr->atime = (uint32_t)st->st_atime;
    attr->mtime = (uint32_t)st->st_mtime;

    if (S_ISREG(st->st_mode)) {
        attr->type = SSH_FILEXFER_TYPE_REGULAR;
    } else if (S_ISDIR(st->st_mode)) {
        attr->type = SSH_FILEXFER_TYPE_DIRECTORY;
    } else if (S_ISLNK(st->st_mode)) {
        attr->type = SSH_FILEXFER_TYPE_SYMLINK;
    } else {
        attr->type = SSH_FILEXFER_TYPE_SPECIAL;
    }

    return attr;
}

bool host_file_storage_init(host_t *host)
{
    if (host == nullptr) {
        return false;
    }

    const char *override_filestore = getenv("CHATTER_FILESTORE_PATH");
    const char *override_legacy = getenv("CHATTER_FILE_STORAGE_ROOT");
    const char *root =
        (override_filestore != nullptr && override_filestore[0] != '\0')
            ? override_filestore
            : ((override_legacy != nullptr && override_legacy[0] != '\0')
                   ? override_legacy
                   : SSH_CHATTER_FILE_STORAGE_ROOT);

    if (root[0] != '/') {
        humanized_log_error("files", "file storage path must be absolute",
                            EINVAL);
        host->file_storage_ready = false;
        host->file_storage_root[0] = '\0';
        return false;
    }

    if (snprintf(host->file_storage_root, sizeof(host->file_storage_root), "%s",
                 root) >= (int)sizeof(host->file_storage_root)) {
        humanized_log_error("files", "file storage path is too long", ENAMETOOLONG);
        host->file_storage_ready = false;
        host->file_storage_root[0] = '\0';
        return false;
    }

    if (!file_storage_ensure_directory(host->file_storage_root)) {
        humanized_log_error("files", "unable to prepare storage directory",
                            errno != 0 ? errno : EIO);
        host->file_storage_ready = false;
        return false;
    }

    host->file_storage_ready = true;
    return true;
}

static bool path_buffer_copy(char *dest, size_t dest_len, const char *src)
{
    if (dest == nullptr || dest_len == 0U || src == nullptr) {
        return false;
    }

    size_t len = strnlen(src, dest_len);
    if (len >= dest_len) {
        return false;
    }

    memcpy(dest, src, len);
    dest[len] = '\0';
    return true;
}

static bool path_buffer_join(char *dest, size_t dest_len, const char *base,
                             const char *suffix)
{
    if (dest == nullptr || dest_len == 0U || base == nullptr ||
        suffix == nullptr) {
        return false;
    }

    size_t base_len = strnlen(base, dest_len);
    size_t suffix_len = strnlen(suffix, dest_len);
    if (base_len >= dest_len || suffix_len >= dest_len) {
        return false;
    }

    size_t needed = base_len + 1U + suffix_len + 1U;
    if (needed > dest_len) {
        return false;
    }

    memcpy(dest, base, base_len);
    dest[base_len] = '/';
    memcpy(dest + base_len + 1U, suffix, suffix_len);
    dest[base_len + 1U + suffix_len] = '\0';
    return true;
}

static bool file_transfer_is_root_reference(const char *virtual_path)
{
    if (virtual_path == nullptr) {
        return false;
    }
    const char *cursor = virtual_path;
    bool saw_component = false;

    while (*cursor != '\0') {
        while (*cursor == '/') {
            ++cursor;
        }
        if (*cursor == '\0') {
            return saw_component || strchr(virtual_path, '/') != nullptr;
        }

        const char *component_start = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            ++cursor;
        }
        size_t component_len = (size_t)(cursor - component_start);
        saw_component = true;
        if (component_len != 1U || component_start[0] != '.') {
            return false;
        }
    }

    return saw_component;
}

static void host_file_storage_list_recursive(const char *root_path,
                                             const char *relative_path,
                                             int depth, char *buffer,
                                             size_t length, size_t *written,
                                             size_t *count)
{
    char full_path[PATH_MAX];
    if (relative_path == nullptr || relative_path[0] == '\0') {
        if (!path_buffer_copy(full_path, sizeof(full_path), root_path)) {
            return;
        }
    } else {
        if (!path_buffer_join(full_path, sizeof(full_path), root_path,
                              relative_path)) {
            return;
        }
    }

    DIR *dir = opendir(full_path);
    if (dir == nullptr) {
        return;
    }

    struct dirent *entry = nullptr;
    while ((entry = readdir(dir)) != nullptr &&
           *count < FILE_STORAGE_LIST_LIMIT) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        char sub_rel_path[PATH_MAX];
        if (relative_path == nullptr || relative_path[0] == '\0') {
            if (!path_buffer_copy(sub_rel_path, sizeof(sub_rel_path),
                                  entry->d_name)) {
                continue;
            }
        } else {
            if (!path_buffer_join(sub_rel_path, sizeof(sub_rel_path),
                                  relative_path, entry->d_name)) {
                continue;
            }
        }

        char sub_full_path[PATH_MAX];
        if (!path_buffer_join(sub_full_path, sizeof(sub_full_path), root_path,
                              sub_rel_path)) {
            continue;
        }

        struct stat st;
        if (stat(sub_full_path, &st) != 0) {
            continue;
        }

                char line[512];
                size_t line_len = 0;
                int res;
        
                // Force \033[1G at the start of every line
                res = snprintf(line + line_len, sizeof(line) - line_len, "\033[1G");
                if (res > 0) {
                    line_len += (size_t)res;
                }
        
                // Hierarchy representation
                for (int i = 0; i < depth; ++i) {
                    if (line_len < sizeof(line)) {
                        res = snprintf(line + line_len, sizeof(line) - line_len, "  ");
                        if (res > 0) {
                            line_len += (size_t)res;
                        }
                    }
                }
                if (line_len < sizeof(line)) {
                    res = snprintf(line + line_len, sizeof(line) - line_len, "-> ");
                    if (res > 0) {
                        line_len += (size_t)res;
                    }
                }
        
                if (line_len < sizeof(line)) {
                    if (S_ISDIR(st.st_mode)) {
                        res = snprintf(line + line_len, sizeof(line) - line_len,
                                             "%s/\n", entry->d_name);
                    } else {
                        res = snprintf(line + line_len, sizeof(line) - line_len,
                                     "%s (%lld bytes)\n", entry->d_name, (long long)st.st_size);
                    }
                    if (res > 0) {
                        line_len += (size_t)res;
                    }
                }
        
                if (line_len > 0 && *written + line_len < length) {
                    memcpy(buffer + *written, line, line_len);
                    *written += line_len;
                    buffer[*written] = '\0';
                    (*count)++;
                }

        if (S_ISDIR(st.st_mode) && *count < FILE_STORAGE_LIST_LIMIT) {
            host_file_storage_list_recursive(root_path, sub_rel_path, depth + 1,
                                             buffer, length, written, count);
        }
    }

    closedir(dir);
}

bool host_file_storage_list(host_t *host, char *buffer, size_t length)
{
    if (buffer == nullptr || length == 0U) {
        return false;
    }
    buffer[0] = '\0';

    if (host == nullptr || !host->file_storage_ready ||
        host->file_storage_root[0] == '\0') {
        snprintf(buffer, length,
                 "\033[1GFile storage is unavailable. Please contact the operator.");
        return false;
    }

    size_t written = 0U;
    size_t count = 0U;
    host_file_storage_list_recursive(host->file_storage_root, "", 0, buffer,
                                     length, &written, &count);

    if (written == 0U) {
        snprintf(buffer, length, "\033[1GStorage is empty. Upload something first!");
    }

    return true;
}

static bool file_storage_is_ready(host_t *host, session_ctx_t *ctx)
{
    if (host == nullptr || !host->file_storage_ready ||
        host->file_storage_root[0] == '\0') {
        if (ctx != nullptr) {
            session_send_system_line(
                ctx, "File storage is unavailable. Please contact an operator.");
        }
        return false;
    }
    return true;
}

static bool file_storage_ensure_directory(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    if (errno != ENOENT) {
        return false;
    }

    if (!file_storage_ensure_parent(path)) {
        return false;
    }

    if (mkdir(path, 0750) == 0) {
        return true;
    }

    if (errno == EEXIST) {
        return true;
    }

    return false;
}

static bool file_storage_ensure_parent(const char *path)
{
    if (path == nullptr || path[0] == '\0') {
        return false;
    }

    char copy[PATH_MAX];
    snprintf(copy, sizeof(copy), "%s", path);
    char *parent = dirname(copy);
    if (parent == nullptr || parent[0] == '\0' || strcmp(parent, "/") == 0) {
        return true;
    }

    struct stat st;
    if (stat(parent, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    if (errno != ENOENT) {
        return false;
    }

    if (!file_storage_ensure_parent(parent)) {
        return false;
    }

    if (mkdir(parent, 0750) == 0) {
        return true;
    }

    if (errno == EEXIST) {
        return true;
    }

    return false;
}

bool file_transfer_resolve_path(host_t *host, const char *virtual_path,
                                char *resolved, size_t resolved_len,
                                char *display, size_t display_len)
{
    if (host == nullptr || resolved == nullptr || resolved_len == 0U ||
        virtual_path == nullptr) {
        return false;
    }

    if (!file_storage_is_ready(host, nullptr)) {
        return false;
    }

    while (*virtual_path == ' ' || *virtual_path == '\t') {
        ++virtual_path;
    }

    char working[PATH_MAX];
    snprintf(working, sizeof(working), "%s", virtual_path);
    trim_whitespace_inplace(working);
    if (working[0] == '\0') {
        return false;
    }

    char sanitized[PATH_MAX];
    sanitized[0] = '\0';
    size_t sanitized_len = 0U;

    const char *cursor = working;
    while (*cursor != '\0') {
        char component[NAME_MAX + 1];
        size_t comp_len = 0U;

        while (*cursor == '/') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }

        while (*cursor != '/' && *cursor != '\0') {
            if (comp_len + 1U >= sizeof(component)) {
                return false;
            }
            component[comp_len++] = *cursor++;
        }
        component[comp_len] = '\0';

        if (comp_len == 0U || strcmp(component, ".") == 0) {
            continue;
        }
        if (strcmp(component, "..") == 0) {
            return false;
        }

        if (sanitized_len + comp_len + 2U >= sizeof(sanitized)) {
            return false;
        }
        if (sanitized_len > 0U) {
            sanitized[sanitized_len++] = '/';
        }
        memcpy(sanitized + sanitized_len, component, comp_len);
        sanitized_len += comp_len;
        sanitized[sanitized_len] = '\0';
    }

    if (sanitized_len == 0U) {
        if (!file_transfer_is_root_reference(working)) {
            return false;
        }
        if (!path_buffer_copy(resolved, resolved_len, host->file_storage_root)) {
            return false;
        }
        if (display != nullptr && display_len > 0U) {
            if (snprintf(display, display_len, "/") >= (int)display_len) {
                return false;
            }
        }
        return true;
    }

    if (!path_buffer_join(resolved, resolved_len, host->file_storage_root,
                          sanitized)) {
        return false;
    }

    if (display != nullptr && display_len > 0U) {
        if (snprintf(display, display_len, "/%s", sanitized) >=
            (int)display_len) {
            return false;
        }
    }

    return true;
}

bool file_transfer_telnet_receive(session_ctx_t *ctx,
                                  const char *resolved_target)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return false;
    }

    if (ctx->transport_kind != SESSION_TRANSPORT_TELNET ||
        ctx->telnet_fd < 0) {
        session_send_system_line(
            ctx, "ZMODEM upload is only available for TELNET sessions.");
        return false;
    }

    if (!file_storage_is_ready(ctx->owner, ctx)) {
        return false;
    }

    ctx->telnet_pending_valid = false;
    ctx->telnet_consume_next_lf = false;
    telnet_force_binary(ctx);

    bool has_target = resolved_target != nullptr && resolved_target[0] != '\0';
    char staging_dir[PATH_MAX];
    staging_dir[0] = '\0';
    const char *working_dir = ctx->owner->file_storage_root;

    if (has_target) {
        if (!file_storage_ensure_parent(resolved_target)) {
            session_send_system_line(ctx,
                                     "Unable to prepare target directory.");
            return false;
        }
        if (!file_storage_prepare_staging(ctx->owner, staging_dir,
                                          sizeof(staging_dir))) {
            session_send_system_line(
                ctx, "Unable to prepare staging directory for upload.");
            return false;
        }
        working_dir = staging_dir;
    }

    char *argv[] = {"rz", "-y", "-q", "--binary", "--escape", nullptr};
    if (!telnet_spawn_zmodem(ctx, argv, working_dir, "rz")) {
        session_send_system_line(
            ctx, "Failed to start rz. Install lrzsz on the server.");
        if (has_target) {
            file_storage_remove_tree(staging_dir);
        }
        return false;
    }

    if (has_target) {
        char staged_file[PATH_MAX];
        if (!file_storage_locate_single_file(staging_dir, staged_file,
                                             sizeof(staged_file))) {
            session_send_system_line(
                ctx,
                "Unable to identify uploaded file in staging directory.");
            file_storage_remove_tree(staging_dir);
            return false;
        }

        if (!file_storage_ensure_parent(resolved_target)) {
            session_send_system_line(ctx,
                                     "Unable to create target directories.");
            file_storage_remove_tree(staging_dir);
            return false;
        }

        if (rename(staged_file, resolved_target) != 0) {
            session_send_system_line(
                ctx, "Failed to move uploaded file to the destination.");
            file_storage_remove_tree(staging_dir);
            return false;
        }

        file_storage_remove_tree(staging_dir);
    }

    return true;
}

bool file_transfer_telnet_send(session_ctx_t *ctx, const char *virtual_path)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return false;
    }

    if (ctx->transport_kind != SESSION_TRANSPORT_TELNET ||
        ctx->telnet_fd < 0) {
        session_send_system_line(
            ctx, "ZMODEM download is only available for TELNET sessions.");
        return false;
    }

    if (!file_storage_is_ready(ctx->owner, ctx)) {
        return false;
    }

    char resolved[PATH_MAX];
    char display[PATH_MAX];
    if (!file_transfer_resolve_path(ctx->owner, virtual_path, resolved,
                                    sizeof(resolved), display,
                                    sizeof(display))) {
        session_send_system_line(ctx,
                                 "Invalid path. Use /filestore to inspect names.");
        return false;
    }

    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISREG(st.st_mode)) {
        session_send_system_line(ctx, "File not found. Use /filestore first.");
        return false;
    }

    session_send_system_line(
        ctx, "Starting ZMODEM download. Trigger your client's RECEIVE now.");

    ctx->telnet_pending_valid = false;
    ctx->telnet_consume_next_lf = false;
    telnet_force_binary(ctx);

    char relative[PATH_MAX];
    snprintf(relative, sizeof(relative), "%s",
             resolved + strlen(ctx->owner->file_storage_root));
    char *relative_ptr = relative;
    while (*relative_ptr == '/') {
        ++relative_ptr;
    }
    if (relative_ptr[0] == '\0') {
        snprintf(relative, sizeof(relative), "%s", basename(resolved));
        relative_ptr = relative;
    }

    char *argv[] = {"sz", "-q", "--binary", "--escape", relative_ptr,
                    nullptr};
    if (!telnet_spawn_zmodem(ctx, argv, ctx->owner->file_storage_root,
                             "sz")) {
        session_send_system_line(
            ctx, "Failed to start sz. Install lrzsz on the server.");
        return false;
    }

    session_send_system_line(ctx, "Download session finished.");
    return true;
}

static bool file_storage_prepare_staging(host_t *host, char *directory,
                                         size_t length)
{
    if (host == nullptr || directory == nullptr || length == 0U) {
        return false;
    }

    char incoming_root[PATH_MAX];
    if (snprintf(incoming_root, sizeof(incoming_root), "%s/.incoming",
                 host->file_storage_root) >= (int)sizeof(incoming_root)) {
        return false;
    }

    if (!file_storage_ensure_directory(incoming_root)) {
        return false;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        now.tv_sec = time(nullptr);
        now.tv_nsec = 0;
    }

    for (int attempt = 0; attempt < 32; ++attempt) {
        if (snprintf(directory, length, "%s/session_%ld_%ld_%p_%d",
                     incoming_root, (long)now.tv_sec, (long)now.tv_nsec,
                     (void *)host, attempt) >= (int)length) {
            return false;
        }
        if (mkdir(directory, 0750) == 0) {
            return true;
        }
        if (errno != EEXIST) {
            return false;
        }
    }

    return false;
}

static bool file_storage_locate_single_file_recursive(const char *root,
                                                      char *absolute_path,
                                                      size_t length,
                                                      size_t *count)
{
    DIR *dir = opendir(root);
    if (dir == nullptr) {
        return false;
    }

    bool ok = true;
    struct dirent *entry = nullptr;
    while (ok && (entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char path[PATH_MAX];
        if (!path_buffer_join(path, sizeof(path), root, entry->d_name)) {
            ok = false;
            break;
        }

        struct stat st;
        if (stat(path, &st) != 0) {
            ok = false;
            break;
        }

        if (S_ISDIR(st.st_mode)) {
            ok = file_storage_locate_single_file_recursive(path, absolute_path,
                                                           length, count);
        } else if (S_ISREG(st.st_mode)) {
            if (*count == 0U) {
                if (snprintf(absolute_path, length, "%s", path) >=
                    (int)length) {
                    ok = false;
                    break;
                }
            }
            *count += 1U;
        }

        if (*count > 1U) {
            ok = false;
            break;
        }
    }

    closedir(dir);
    return ok;
}

static bool file_storage_locate_single_file(const char *root,
                                            char *absolute_path,
                                            size_t length)
{
    if (root == nullptr || absolute_path == nullptr || length == 0U) {
        return false;
    }

    size_t count = 0U;
    if (!file_storage_locate_single_file_recursive(root, absolute_path, length,
                                                   &count)) {
        return false;
    }

    return count == 1U;
}

static void file_storage_remove_tree(const char *root)
{
    if (root == nullptr || root[0] == '\0') {
        return;
    }

    DIR *dir = opendir(root);
    if (dir == nullptr) {
        unlink(root);
        return;
    }

    struct dirent *entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char path[PATH_MAX];
        if (!path_buffer_join(path, sizeof(path), root, entry->d_name)) {
            continue;
        }

        struct stat st;
        if (lstat(path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            file_storage_remove_tree(path);
        } else {
            unlink(path);
        }
    }

    closedir(dir);
    rmdir(root);
}

static bool scp_parse_command(const char *command, scp_mode_t *mode,
                              char *virtual_path, size_t path_length)
{
    if (command == nullptr || mode == nullptr || virtual_path == nullptr ||
        path_length == 0U) {
        return false;
    }

    const char *cursor = command;
    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }

    if (strncmp(cursor, "scp", 3) != 0) {
        return false;
    }

    cursor += 3;
    bool end_of_options = false;
    bool mode_set = false;
    bool path_set = false;

    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }

        if (*cursor == '\0') {
            break;
        }

        if (!end_of_options && *cursor == '-') {
            ++cursor;
            if (*cursor == '-') {
                ++cursor;
                end_of_options = true;
                continue;
            }

            while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
                char opt = *cursor++;
                if (opt == 't') {
                    *mode = SCP_MODE_UPLOAD;
                    mode_set = true;
                } else if (opt == 'f') {
                    *mode = SCP_MODE_DOWNLOAD;
                    mode_set = true;
                } else if (opt == 'd' || opt == 'p' || opt == 'r' ||
                           opt == 'v') {
                    // Ignore unsupported but harmless flags.
                    continue;
                } else {
                    return false;
                }
            }
            continue;
        }

        if (path_set) {
            return false;
        }

        char token[PATH_MAX];
        size_t length = 0U;
        char quote = '\0';
        if (*cursor == '\'' || *cursor == '"') {
            quote = *cursor++;
        }

        while (*cursor != '\0') {
            if (quote != '\0') {
                if (*cursor == quote) {
                    ++cursor;
                    break;
                }
            } else if (*cursor == ' ' || *cursor == '\t') {
                break;
            }

            if (length + 1U >= sizeof(token)) {
                return false;
            }
            token[length++] = *cursor++;
        }
        token[length] = '\0';

        if (length == 0U) {
            continue;
        }

        if (snprintf(virtual_path, path_length, "%s", token) >=
            (int)path_length) {
            return false;
        }
        path_set = true;
        end_of_options = true;
    }

    return mode_set && path_set;
}

int file_transfer_handle_scp_exec(session_ctx_t *ctx, const char *command)
{
    if (ctx == nullptr || ctx->owner == nullptr || ctx->channel == nullptr) {
        return -1;
    }

    char path[PATH_MAX];
    scp_mode_t mode = SCP_MODE_DOWNLOAD;
    if (!scp_parse_command(command, &mode, path, sizeof(path))) {
        scp_send_status(ctx, 2, "Unsupported SCP command.");
        return -1;
    }

    char resolved[PATH_MAX];
    char display[PATH_MAX];
    if (!file_transfer_resolve_path(ctx->owner, path, resolved,
                                    sizeof(resolved), display,
                                    sizeof(display))) {
        scp_send_status(ctx, 2,
                        "Invalid target path or directory traversal attempt.");
        return -1;
    }

    int result = -1;
    switch (mode) {
    case SCP_MODE_UPLOAD:
        result = scp_handle_upload(ctx, resolved);
        break;
    case SCP_MODE_DOWNLOAD:
        result = scp_handle_download(ctx, resolved);
        break;
    }

    if (result == 0) {
        scp_send_status(ctx, 0, nullptr);
    }
    return result;
}

static bool scp_send_status(session_ctx_t *ctx, unsigned char code,
                            const char *message)
{
    if (ctx == nullptr || ctx->channel == nullptr) {
        return false;
    }

    unsigned char header = code;
    if (ssh_channel_write(ctx->channel, &header, 1) != 1) {
        return false;
    }

    if (message != nullptr && message[0] != '\0') {
        size_t length = strnlen(message, SSH_CHATTER_MESSAGE_LIMIT - 1U);
        if (!scp_channel_write_all(ctx, message, length)) {
            return false;
        }
        const char newline = '\n';
        if (!scp_channel_write_all(ctx, &newline, 1)) {
            return false;
        }
    }
    return true;
}

static int scp_expect_byte(session_ctx_t *ctx)
{
    unsigned char byte = 0U;
    ssize_t read_len = scp_channel_read(ctx, &byte, 1);
    if (read_len <= 0) {
        return -1;
    }
    return (int)byte;
}

static bool scp_expect_ok(session_ctx_t *ctx)
{
    int status = scp_expect_byte(ctx);
    return status == 0;
}

static bool scp_send_ok(session_ctx_t *ctx)
{
    const unsigned char ok = 0U;
    return scp_channel_write_all(ctx, &ok, 1);
}

static ssize_t scp_channel_read(session_ctx_t *ctx, void *buffer,
                                size_t length)
{
    if (ctx == nullptr || ctx->channel == nullptr || buffer == nullptr ||
        length == 0U) {
        return -1;
    }

    uint32_t chunk =
        (length > UINT32_MAX) ? UINT32_MAX : (uint32_t)length;
    return ssh_channel_read(ctx->channel, buffer, chunk, 0);
}

static bool scp_channel_write_all(session_ctx_t *ctx, const void *buffer,
                                  size_t length)
{
    if (ctx == nullptr || ctx->channel == nullptr || buffer == nullptr) {
        return false;
    }

    const unsigned char *cursor = (const unsigned char *)buffer;
    size_t remaining = length;
    while (remaining > 0U) {
        int written =
            ssh_channel_write(ctx->channel, cursor, (uint32_t)remaining);
        if (written <= 0) {
            return false;
        }
        cursor += (size_t)written;
        remaining -= (size_t)written;
    }
    return true;
}

static int scp_handle_upload(session_ctx_t *ctx, const char *real_path)
{
    if (!file_storage_is_ready(ctx->owner, nullptr)) {
        scp_send_status(ctx, 2, "File storage unavailable.");
        return -1;
    }

    if (!file_storage_ensure_parent(real_path)) {
        scp_send_status(ctx, 2, "Unable to create destination directory.");
        return -1;
    }

    if (!scp_send_ok(ctx)) {
        return -1;
    }

    char header[PATH_MAX];
    size_t header_len = 0U;
    while (header_len + 1U < sizeof(header)) {
        int byte = scp_expect_byte(ctx);
        if (byte < 0) {
            return -1;
        }
        if (byte == '\n') {
            break;
        }
        header[header_len++] = (char)byte;
    }
    header[header_len] = '\0';

    if (header[0] != 'C') {
        scp_send_status(ctx, 2, "Only regular file uploads are supported.");
        return -1;
    }

    char mode[8];
    unsigned long long size = 0ULL;
    char filename[PATH_MAX];
    if (sscanf(header, "C%7s %llu %1023s", mode, &size, filename) != 3) {
        scp_send_status(ctx, 2, "Malformed SCP header.");
        return -1;
    }

    int fd = open(real_path, O_WRONLY | O_CREAT | O_TRUNC, 0640);
    if (fd < 0) {
        scp_send_status(ctx, 2, "Unable to open target for writing.");
        return -1;
    }

    if (!scp_send_ok(ctx)) {
        close(fd);
        return -1;
    }

    unsigned char buffer[FILE_TRANSFER_BUFFER];
    unsigned long long remaining = size;
    while (remaining > 0ULL) {
        size_t chunk = remaining > sizeof(buffer) ? sizeof(buffer)
                                                  : (size_t)remaining;
        ssize_t read_len = scp_channel_read(ctx, buffer, chunk);
        if (read_len <= 0) {
            close(fd);
            return -1;
        }
        if (write(fd, buffer, (size_t)read_len) != read_len) {
            close(fd);
            scp_send_status(ctx, 2, "Failed writing destination file.");
            return -1;
        }
        remaining -= (unsigned long long)read_len;
    }
    close(fd);

    if (!scp_expect_ok(ctx)) {
        scp_send_status(ctx, 2, "Sender aborted transfer.");
        return -1;
    }

    return 0;
}

static int scp_handle_download(session_ctx_t *ctx, const char *real_path)
{
    struct stat st;
    if (stat(real_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        scp_send_status(ctx, 2, "File not found.");
        return -1;
    }

    if (!scp_expect_ok(ctx)) {
        return -1;
    }

    const char *basename_ptr = strrchr(real_path, '/');
    basename_ptr = (basename_ptr != nullptr) ? basename_ptr + 1 : real_path;

    char header[PATH_MAX + 32];
    int header_len = snprintf(header, sizeof(header), "C%04o %lld %s\n",
                              (int)(st.st_mode & 0777),
                              (long long)st.st_size, basename_ptr);
    if (header_len <= 0 ||
        !scp_channel_write_all(ctx, header, (size_t)header_len)) {
        return -1;
    }

    if (!scp_expect_ok(ctx)) {
        return -1;
    }

    int fd = open(real_path, O_RDONLY);
    if (fd < 0) {
        scp_send_status(ctx, 2, "Unable to read file.");
        return -1;
    }

    unsigned char buffer[FILE_TRANSFER_BUFFER];
    ssize_t read_len = 0;
    while ((read_len = read(fd, buffer, sizeof(buffer))) > 0) {
        if (!scp_channel_write_all(ctx, buffer, (size_t)read_len)) {
            close(fd);
            return -1;
        }
    }
    close(fd);

    if (read_len < 0) {
        scp_send_status(ctx, 2, "Failed reading file.");
        return -1;
    }

    if (!scp_send_ok(ctx)) {
        return -1;
    }

    return scp_expect_ok(ctx) ? 0 : -1;
}

static void telnet_force_binary(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->telnet_fd < 0) {
        return;
    }
    unsigned char payload[] = {
        TELNET_IAC, TELNET_CMD_WILL, TELNET_OPT_BINARY,
        TELNET_IAC, TELNET_CMD_DO,   TELNET_OPT_BINARY,
        TELNET_IAC, TELNET_CMD_WILL, TELNET_OPT_SUPPRESS_GO_AHEAD,
        TELNET_IAC, TELNET_CMD_DO,   TELNET_OPT_SUPPRESS_GO_AHEAD,
    };
    send(ctx->telnet_fd, payload, sizeof(payload), MSG_NOSIGNAL);
}

static bool telnet_binary_write(session_ctx_t *ctx, const unsigned char *data,
                                size_t length)
{
    if (ctx == nullptr || ctx->telnet_fd < 0 || data == nullptr) {
        return false;
    }

    for (size_t idx = 0; idx < length; ++idx) {
        unsigned char byte = data[idx];
        if (send(ctx->telnet_fd, &byte, 1, MSG_NOSIGNAL) != 1) {
            return false;
        }
        if (byte == TELNET_IAC) {
            if (send(ctx->telnet_fd, &byte, 1, MSG_NOSIGNAL) != 1) {
                return false;
            }
        }
    }
    return true;
}

static ssize_t telnet_binary_read(session_ctx_t *ctx, unsigned char *buffer,
                                  size_t length)
{
    if (ctx == nullptr || ctx->telnet_fd < 0 || buffer == nullptr ||
        length == 0U) {
        return -1;
    }

    size_t produced = 0U;
    while (produced < length) {
        struct pollfd pfd = {
            .fd = ctx->telnet_fd,
            .events = POLLIN,
            .revents = 0,
        };
        int poll_result = poll(&pfd, 1, ZMODEM_POLL_TIMEOUT_MS);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (poll_result == 0) {
            if (produced > 0U) {
                return (ssize_t)produced;
            }
            continue;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return -1;
        }

        unsigned char byte = 0U;
        ssize_t read_len = recv(ctx->telnet_fd, &byte, 1, 0);
        if (read_len <= 0) {
            return -1;
        }

        if (byte == TELNET_IAC) {
            unsigned char command = 0U;
            if (recv(ctx->telnet_fd, &command, 1, 0) <= 0) {
                return -1;
            }
            if (command == TELNET_IAC) {
                buffer[produced++] = TELNET_IAC;
                break;
            }
            unsigned char option = 0U;
            switch (command) {
            case TELNET_CMD_DO:
                if (recv(ctx->telnet_fd, &option, 1, 0) <= 0) {
                    return -1;
                }
                if (option == TELNET_OPT_BINARY ||
                    option == TELNET_OPT_SUPPRESS_GO_AHEAD) {
                    unsigned char response[] = {TELNET_IAC, TELNET_CMD_WILL,
                                                option};
                    send(ctx->telnet_fd, response, sizeof(response),
                         MSG_NOSIGNAL);
                } else {
                    unsigned char response[] = {TELNET_IAC, TELNET_CMD_WONT,
                                                option};
                    send(ctx->telnet_fd, response, sizeof(response),
                         MSG_NOSIGNAL);
                }
                break;
            case TELNET_CMD_DONT:
                if (recv(ctx->telnet_fd, &option, 1, 0) <= 0) {
                    return -1;
                }
                {
                    unsigned char response[] = {TELNET_IAC, TELNET_CMD_WONT,
                                                option};
                    send(ctx->telnet_fd, response, sizeof(response),
                         MSG_NOSIGNAL);
                }
                break;
            case TELNET_CMD_WILL:
                if (recv(ctx->telnet_fd, &option, 1, 0) <= 0) {
                    return -1;
                }
                if (option == TELNET_OPT_BINARY ||
                    option == TELNET_OPT_SUPPRESS_GO_AHEAD) {
                    unsigned char response[] = {TELNET_IAC, TELNET_CMD_DO,
                                                option};
                    send(ctx->telnet_fd, response, sizeof(response),
                         MSG_NOSIGNAL);
                } else {
                    unsigned char response[] = {TELNET_IAC, TELNET_CMD_DONT,
                                                option};
                    send(ctx->telnet_fd, response, sizeof(response),
                         MSG_NOSIGNAL);
                }
                break;
            case TELNET_CMD_SB: {
                // Consume until IAC SE
                unsigned char prev = 0U;
                unsigned char chunk = 0U;
                while (recv(ctx->telnet_fd, &chunk, 1, 0) > 0) {
                    if (prev == TELNET_IAC && chunk == TELNET_CMD_SE) {
                        break;
                    }
                    prev = (chunk == TELNET_IAC) ? TELNET_IAC : 0U;
                }
                break;
            }
            default:
                break;
            }
            continue;
        }

        buffer[produced++] = byte;
        break;
    }

    return (ssize_t)produced;
}

static bool telnet_spawn_zmodem(session_ctx_t *ctx, char *const argv[],
                                const char *working_dir, const char *label)
{
    if (ctx == nullptr || ctx->telnet_fd < 0) {
        return false;
    }

    int stdin_pipe[2];
    int stdout_pipe[2];
    if (pipe(stdin_pipe) != 0) {
        return false;
    }
    if (pipe(stdout_pipe) != 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        return false;
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return false;
    }

    if (pid == 0) {
        if (working_dir != nullptr) {
            if (chdir(working_dir) != 0) {
                fprintf(stderr, "[filestore] chdir(%s) failed: %s\n",
                        working_dir, strerror(errno));
                _exit(127);
            }
        }
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stdout_pipe[1], STDERR_FILENO);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    bool stdin_open = true;
    bool stdout_open = true;

    while (stdin_open || stdout_open) {
        struct pollfd fds[2];
        nfds_t nfds = 0U;
        if (stdin_open) {
            fds[nfds++] = (struct pollfd){.fd = ctx->telnet_fd, .events = POLLIN};
        }
        if (stdout_open) {
            fds[nfds++] = (struct pollfd){.fd = stdout_pipe[0], .events = POLLIN};
        }

        int poll_result = poll(fds, nfds, ZMODEM_POLL_TIMEOUT_MS);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (poll_result == 0) {
            continue;
        }

        nfds_t index = 0U;
        if (stdin_open) {
            struct pollfd telnet_pfd = fds[index++];
            if (telnet_pfd.revents & POLLIN) {
                unsigned char buffer[ZMODEM_IO_CHUNK];
                ssize_t read_len = telnet_binary_read(ctx, buffer,
                                                      sizeof(buffer));
                if (read_len <= 0) {
                    stdin_open = false;
                    shutdown(stdin_pipe[1], SHUT_WR);
                    close(stdin_pipe[1]);
                } else {
                    ssize_t written =
                        write(stdin_pipe[1], buffer, (size_t)read_len);
                    if (written != read_len) {
                        stdin_open = false;
                        shutdown(stdin_pipe[1], SHUT_WR);
                        close(stdin_pipe[1]);
                    }
                }
            } else if (telnet_pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                stdin_open = false;
                shutdown(stdin_pipe[1], SHUT_WR);
                close(stdin_pipe[1]);
            }
        }

        if (stdout_open) {
            struct pollfd child_pfd = fds[index];
            if (child_pfd.revents & POLLIN) {
                unsigned char buffer[ZMODEM_IO_CHUNK];
                ssize_t read_len = read(stdout_pipe[0], buffer,
                                        sizeof(buffer));
                if (read_len <= 0) {
                    stdout_open = false;
                    close(stdout_pipe[0]);
                } else {
                    if (!telnet_binary_write(ctx, buffer, (size_t)read_len)) {
                        stdout_open = false;
                        close(stdout_pipe[0]);
                    }
                }
            } else if (child_pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                stdout_open = false;
                close(stdout_pipe[0]);
            }
        }
    }

    if (stdin_open) {
        close(stdin_pipe[1]);
    }
    if (stdout_open) {
        close(stdout_pipe[0]);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        char message[128];
        snprintf(message, sizeof(message), "%s exited with code %d.", label,
                 WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        session_send_system_line(ctx, message);
        return false;
    }

    return true;
}

int file_transfer_handle_sftp(session_ctx_t *ctx)
{
    sftp_session sftp;
    sftp_client_message msg;

    if (ctx == nullptr || ctx->owner == nullptr || ctx->session == nullptr ||
        ctx->channel == nullptr) {
        return -1;
    }

    sftp = sftp_server_new(ctx->session, ctx->channel);
    if (sftp == nullptr) {
        return -1;
    }

    if (sftp_server_init(sftp) != SSH_OK) {
        sftp_server_free(sftp);
        return -1;
    }

    while (true) {
        msg = sftp_get_client_message(sftp);
        if (msg == nullptr) {
            break;
        }

        uint8_t type = sftp_client_message_get_type(msg);
        switch (type) {
        case SSH_FXP_REALPATH: {
            const char *path = sftp_client_message_get_filename(msg);
            char resolved[PATH_MAX];
            char display[PATH_MAX];
            if (file_transfer_resolve_path(ctx->owner, path ? path : "/",
                                            resolved, sizeof(resolved),
                                            display, sizeof(display))) {
                sftp_reply_name(msg, display, nullptr);
            } else {
                sftp_reply_name(msg, "/", nullptr);
            }
            break;
        }
        case SSH_FXP_STAT:
        case SSH_FXP_LSTAT: {
            const char *path = sftp_client_message_get_filename(msg);
            char resolved[PATH_MAX];
            if (!file_transfer_resolve_path(ctx->owner, path, resolved,
                                            sizeof(resolved), NULL, 0U)) {
                sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED,
                                  "Permission denied.");
                break;
            }
            struct stat st;
            if ((type == SSH_FXP_STAT ? stat(resolved, &st)
                                      : lstat(resolved, &st)) != 0) {
                sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, strerror(errno));
                break;
            }
            sftp_attributes attr = sftp_attr_from_stat(basename(resolved), &st);
            if (attr == nullptr) {
                sftp_reply_status(msg, SSH_FX_FAILURE, "Memory error.");
                break;
            }
            sftp_reply_attr(msg, attr);
            sftp_attributes_free(attr);
            break;
        }
        case SSH_FXP_OPENDIR: {
            const char *path = sftp_client_message_get_filename(msg);
            char resolved[PATH_MAX];
            if (!file_transfer_resolve_path(ctx->owner, path, resolved,
                                            sizeof(resolved), NULL, 0U)) {
                sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED,
                                  "Permission denied.");
                break;
            }
            DIR *dir = opendir(resolved);
            if (dir == nullptr) {
                sftp_reply_status(msg, SSH_FX_FAILURE, strerror(errno));
                break;
            }
            sftp_handle_data_t *hdata = calloc(1, sizeof(sftp_handle_data_t));
            if (hdata == nullptr) {
                closedir(dir);
                sftp_reply_status(msg, SSH_FX_FAILURE, "Memory error.");
                break;
            }
            hdata->type = SFTP_HANDLE_DIR;
            hdata->u.dir = dir;
            snprintf(hdata->path, sizeof(hdata->path), "%s", resolved);
            ssh_string h_str = sftp_handle_alloc(sftp, hdata);
            sftp_reply_handle(msg, h_str);
            ssh_string_free(h_str);
            break;
        }
        case SSH_FXP_READDIR: {
            ssh_string h_str = msg->handle;
            sftp_handle_data_t *hdata = sftp_handle(sftp, h_str);
            if (hdata == nullptr || hdata->type != SFTP_HANDLE_DIR) {
                sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle.");
                break;
            }
            struct dirent *entry;
            int count = 0;
            while ((entry = readdir(hdata->u.dir)) != nullptr) {
                if (strcmp(entry->d_name, ".") == 0 ||
                    strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                char full_path[PATH_MAX];
                if (!path_buffer_join(full_path, sizeof(full_path),
                                      hdata->path, entry->d_name)) {
                    continue;
                }
                struct stat st;
                if (stat(full_path, &st) == 0) {
                    sftp_attributes attr =
                        sftp_attr_from_stat(entry->d_name, &st);
                    char longname[1024];
                    snprintf(longname, sizeof(longname),
                             "%s %4u %4u %8llu %s",
                             S_ISDIR(st.st_mode) ? "drwxr-xr-x" : "-rw-r--r--",
                             (unsigned)st.st_uid, (unsigned)st.st_gid,
                             (unsigned long long)st.st_size, entry->d_name);
                    sftp_reply_names_add(msg, entry->d_name, longname, attr);
                    sftp_attributes_free(attr);
                    count++;
                }
                if (count >= 100) {
                    break;
                }
            }
            if (count > 0) {
                sftp_reply_names(msg);
            } else {
                sftp_reply_status(msg, SSH_FX_EOF, "End of directory.");
            }
            break;
        }
        case SSH_FXP_OPEN: {
            const char *path = sftp_client_message_get_filename(msg);
            uint32_t flags = sftp_client_message_get_flags(msg);
            char resolved[PATH_MAX];
            if (!file_transfer_resolve_path(ctx->owner, path, resolved,
                                            sizeof(resolved), NULL, 0U)) {
                sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED,
                                  "Permission denied.");
                break;
            }

            if ((flags & SSH_FXF_CREAT) &&
                !file_storage_ensure_parent(resolved)) {
                sftp_reply_status(msg, SSH_FX_FAILURE,
                                  "Unable to create parent directory.");
                break;
            }

            int sys_flags = 0;
            if ((flags & SSH_FXF_READ) && (flags & SSH_FXF_WRITE)) {
                sys_flags = O_RDWR;
            } else if (flags & SSH_FXF_WRITE) {
                sys_flags = O_WRONLY;
            } else {
                sys_flags = O_RDONLY;
            }

            if (flags & SSH_FXF_CREAT) {
                sys_flags |= O_CREAT;
            }
            if (flags & SSH_FXF_TRUNC) {
                sys_flags |= O_TRUNC;
            }
            if (flags & SSH_FXF_EXCL) {
                sys_flags |= O_EXCL;
            }
            if (flags & SSH_FXF_APPEND) {
                sys_flags |= O_APPEND;
            }

            int fd = open(resolved, sys_flags, 0640);
            if (fd < 0) {
                sftp_reply_status(msg, SSH_FX_FAILURE, strerror(errno));
                break;
            }
            sftp_handle_data_t *hdata = calloc(1, sizeof(sftp_handle_data_t));
            if (hdata == nullptr) {
                close(fd);
                sftp_reply_status(msg, SSH_FX_FAILURE, "Memory error.");
                break;
            }
            hdata->type = SFTP_HANDLE_FILE;
            hdata->u.fd = fd;
            snprintf(hdata->path, sizeof(hdata->path), "%s", resolved);
            ssh_string h_str = sftp_handle_alloc(sftp, hdata);
            sftp_reply_handle(msg, h_str);
            ssh_string_free(h_str);
            break;
        }
        case SSH_FXP_READ: {
            ssh_string h_str = msg->handle;
            uint64_t offset = msg->offset;
            uint32_t len = msg->len;
            sftp_handle_data_t *hdata = sftp_handle(sftp, h_str);
            if (hdata == nullptr || hdata->type != SFTP_HANDLE_FILE) {
                sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle.");
                break;
            }
            void *data = malloc(len);
            if (data == nullptr) {
                sftp_reply_status(msg, SSH_FX_FAILURE, "Memory error.");
                break;
            }
            ssize_t read_len = pread(hdata->u.fd, data, len, (off_t)offset);
            if (read_len < 0) {
                free(data);
                sftp_reply_status(msg, SSH_FX_FAILURE, strerror(errno));
            } else if (read_len == 0) {
                free(data);
                sftp_reply_status(msg, SSH_FX_EOF, "EOF");
            } else {
                sftp_reply_data(msg, data, (int)read_len);
                free(data);
            }
            break;
        }
        case SSH_FXP_WRITE: {
            ssh_string h_str = msg->handle;
            uint64_t offset = msg->offset;
            sftp_handle_data_t *hdata = sftp_handle(sftp, h_str);
            if (hdata == nullptr || hdata->type != SFTP_HANDLE_FILE) {
                sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle.");
                break;
            }
            if (msg->data == nullptr) {
                sftp_reply_status(msg, SSH_FX_FAILURE, "No data in write message.");
                break;
            }
            size_t len = ssh_string_len(msg->data);
            const void *data = ssh_string_data(msg->data);
            ssize_t written = pwrite(hdata->u.fd, data, len, (off_t)offset);
            if (written < 0) {
                sftp_reply_status(msg, SSH_FX_FAILURE, strerror(errno));
            } else if (written != (ssize_t)len) {
                sftp_reply_status(msg, SSH_FX_FAILURE, "Partial write.");
            } else {
                sftp_reply_status(msg, SSH_FX_OK, "Success");
            }
            break;
        }
        case SSH_FXP_FSTAT: {
            ssh_string h_str = msg->handle;
            sftp_handle_data_t *hdata = sftp_handle(sftp, h_str);
            if (hdata == nullptr || hdata->type != SFTP_HANDLE_FILE) {
                sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle.");
                break;
            }
            struct stat st;
            if (fstat(hdata->u.fd, &st) < 0) {
                sftp_reply_status(msg, SSH_FX_FAILURE, strerror(errno));
                break;
            }
            sftp_attributes attr =
                sftp_attr_from_stat(basename(hdata->path), &st);
            if (attr == nullptr) {
                sftp_reply_status(msg, SSH_FX_FAILURE, "Memory error.");
                break;
            }
            sftp_reply_attr(msg, attr);
            sftp_attributes_free(attr);
            break;
        }
        case SSH_FXP_CLOSE: {
            ssh_string h_str = msg->handle;
            sftp_handle_data_t *hdata = sftp_handle(sftp, h_str);
            if (hdata != nullptr) {
                if (hdata->type == SFTP_HANDLE_FILE) {
                    close(hdata->u.fd);
                } else {
                    closedir(hdata->u.dir);
                }
                sftp_handle_remove(sftp, hdata);
                free(hdata);
            }
            sftp_reply_status(msg, SSH_FX_OK, "Success");
            break;
        }
        case SSH_FXP_REMOVE: {
            const char *path = sftp_client_message_get_filename(msg);
            char resolved[PATH_MAX];
            if (!file_transfer_resolve_path(ctx->owner, path, resolved,
                                            sizeof(resolved), NULL, 0U)) {
                sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED,
                                  "Permission denied.");
                break;
            }
            if (unlink(resolved) == 0) {
                sftp_reply_status(msg, SSH_FX_OK, "Success");
            } else {
                sftp_reply_status(msg, SSH_FX_FAILURE, strerror(errno));
            }
            break;
        }
        case SSH_FXP_RENAME: {
            const char *oldpath = sftp_client_message_get_filename(msg);
            const char *newpath = sftp_client_message_get_data(msg);
            char resolved_old[PATH_MAX];
            char resolved_new[PATH_MAX];
            if (!file_transfer_resolve_path(ctx->owner, oldpath, resolved_old,
                                            sizeof(resolved_old), NULL, 0U) ||
                !file_transfer_resolve_path(ctx->owner, newpath, resolved_new,
                                            sizeof(resolved_new), NULL, 0U)) {
                sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED,
                                  "Permission denied.");
                break;
            }
            if (rename(resolved_old, resolved_new) == 0) {
                sftp_reply_status(msg, SSH_FX_OK, "Success");
            } else {
                sftp_reply_status(msg, SSH_FX_FAILURE, strerror(errno));
            }
            break;
        }
        case SSH_FXP_MKDIR: {
            const char *path = sftp_client_message_get_filename(msg);
            char resolved[PATH_MAX];
            if (!file_transfer_resolve_path(ctx->owner, path, resolved,
                                            sizeof(resolved), NULL, 0U)) {
                sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED,
                                  "Permission denied.");
                break;
            }
            if (file_storage_ensure_directory(resolved)) {
                sftp_reply_status(msg, SSH_FX_OK, "Success");
            } else {
                sftp_reply_status(msg, SSH_FX_FAILURE, strerror(errno));
            }
            break;
        }
        case SSH_FXP_RMDIR: {
            const char *path = sftp_client_message_get_filename(msg);
            char resolved[PATH_MAX];
            if (!file_transfer_resolve_path(ctx->owner, path, resolved,
                                            sizeof(resolved), NULL, 0U)) {
                sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED,
                                  "Permission denied.");
                break;
            }
            if (rmdir(resolved) == 0) {
                sftp_reply_status(msg, SSH_FX_OK, "Success");
            } else {
                sftp_reply_status(msg, SSH_FX_FAILURE, strerror(errno));
            }
            break;
        }
        case SSH_FXP_SETSTAT: {
            const char *path = sftp_client_message_get_filename(msg);
            char resolved[PATH_MAX];
            if (!file_transfer_resolve_path(ctx->owner, path, resolved,
                                            sizeof(resolved), NULL, 0U)) {
                sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED,
                                  "Permission denied.");
                break;
            }
            sftp_attributes attr = msg->attr;
            if (attr != nullptr) {
                if (attr->flags & SSH_FILEXFER_ATTR_SIZE) {
                    if (truncate(resolved, (off_t)attr->size) < 0) {
                        sftp_reply_status(msg, SSH_FX_FAILURE, strerror(errno));
                        break;
                    }
                }
                if (attr->flags & SSH_FILEXFER_ATTR_PERMISSIONS) {
                    if (chmod(resolved, attr->permissions & 0777) < 0) {
                        /* ignore non-fatal permission errors */
                    }
                }
                if (attr->flags & SSH_FILEXFER_ATTR_ACMODTIME) {
                    struct timeval tv[2];
                    tv[0].tv_sec = (long)attr->atime;
                    tv[0].tv_usec = 0;
                    tv[1].tv_sec = (long)attr->mtime;
                    tv[1].tv_usec = 0;
                    utimes(resolved, tv);
                }
            }
            sftp_reply_status(msg, SSH_FX_OK, "Success");
            break;
        }
        case SSH_FXP_FSETSTAT: {
            ssh_string h_str = msg->handle;
            sftp_handle_data_t *hdata = sftp_handle(sftp, h_str);
            if (hdata == nullptr || hdata->type != SFTP_HANDLE_FILE) {
                sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle.");
                break;
            }
            sftp_attributes attr = msg->attr;
            if (attr != nullptr) {
                if (attr->flags & SSH_FILEXFER_ATTR_SIZE) {
                    if (ftruncate(hdata->u.fd, (off_t)attr->size) < 0) {
                        sftp_reply_status(msg, SSH_FX_FAILURE, strerror(errno));
                        break;
                    }
                }
                if (attr->flags & SSH_FILEXFER_ATTR_PERMISSIONS) {
                    if (fchmod(hdata->u.fd, attr->permissions & 0777) < 0) {
                        /* ignore non-fatal permission errors */
                    }
                }
                if (attr->flags & SSH_FILEXFER_ATTR_ACMODTIME) {
                    struct timeval tv[2];
                    tv[0].tv_sec = (long)attr->atime;
                    tv[0].tv_usec = 0;
                    tv[1].tv_sec = (long)attr->mtime;
                    tv[1].tv_usec = 0;
                    futimes(hdata->u.fd, tv);
                }
            }
            sftp_reply_status(msg, SSH_FX_OK, "Success");
            break;
        }
        default:
            sftp_reply_status(msg, SSH_FX_OP_UNSUPPORTED,
                              "Operation not supported.");
            break;
        }
        sftp_client_message_free(msg);
    }

    sftp_server_free(sftp);
    return 0;
}

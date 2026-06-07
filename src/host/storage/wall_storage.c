typedef struct wall_state_header {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
} wall_state_header_t;

#define WALL_STATE_MAGIC 0x57414c4cU
#define WALL_STATE_VERSION 1U

static void host_wall_resolve_path(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    const char *wall_path = getenv("CHATTER_WALL_FILE");
    if (wall_path == nullptr || wall_path[0] == '\0') {
        wall_path = "wall_state.dat";
    }

    int written = snprintf(host->wall_state_file_path,
                           sizeof(host->wall_state_file_path), "%s",
                           wall_path);
    if (written < 0 || (size_t)written >= sizeof(host->wall_state_file_path)) {
        humanized_log_error("wall", "wall state file path is too long",
                            ENAMETOOLONG);
        host->wall_state_file_path[0] = '\0';
    }
}

static void host_wall_reset_locked(host_t *host)
{
    if (host == nullptr) {
        return;
    }

    for (size_t y = 0U; y < SSH_CHATTER_WALL_HEIGHT; ++y) {
        for (size_t x = 0U; x < SSH_CHATTER_WALL_WIDTH; ++x) {
            host->wall[y][x].ch = ' ';
            snprintf(host->wall[y][x].color_name,
                     sizeof(host->wall[y][x].color_name), "%s", "default");
            host->wall[y][x].updated_at_ns = 0;
        }
    }
}

static void host_wall_state_save_locked(host_t *host)
{
    if (host == nullptr || host->wall_state_file_path[0] == '\0') {
        return;
    }

    if (!host_ensure_private_data_path(host, host->wall_state_file_path, true)) {
        return;
    }

    char temp_path[PATH_MAX];
    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp",
                           host->wall_state_file_path);
    if (written < 0 || (size_t)written >= sizeof(temp_path)) {
        humanized_log_error("wall", "wall state file path is too long",
                            ENAMETOOLONG);
        return;
    }

    int temp_fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW,
                       S_IRUSR | S_IWUSR);
    if (temp_fd < 0) {
        humanized_log_error("wall", "failed to open wall state file",
                            errno != 0 ? errno : EIO);
        return;
    }

    FILE *fp = fdopen(temp_fd, "wb");
    if (fp == nullptr) {
        int saved_errno = errno;
        close(temp_fd);
        unlink(temp_path);
        humanized_log_error("wall", "failed to wrap wall state descriptor",
                            saved_errno != 0 ? saved_errno : EIO);
        return;
    }

    wall_state_header_t header = {
        .magic = WALL_STATE_MAGIC,
        .version = WALL_STATE_VERSION,
        .width = SSH_CHATTER_WALL_WIDTH,
        .height = SSH_CHATTER_WALL_HEIGHT,
    };

    bool success = fwrite(&header, sizeof(header), 1U, fp) == 1U;
    if (success) {
        success = fwrite(host->wall, sizeof(host->wall), 1U, fp) == 1U;
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
        humanized_log_error("wall", "failed to write wall state file",
                            errno != 0 ? errno : EIO);
        unlink(temp_path);
        return;
    }

    if (chmod(temp_path, S_IRUSR | S_IWUSR) != 0) {
        humanized_log_error("wall",
                            "failed to tighten temporary wall permissions",
                            errno != 0 ? errno : EACCES);
        unlink(temp_path);
        return;
    }

    if (rename(temp_path, host->wall_state_file_path) != 0) {
        humanized_log_error("wall", "failed to update wall state file", errno);
        unlink(temp_path);
    } else if (chmod(host->wall_state_file_path, S_IRUSR | S_IWUSR) != 0) {
        humanized_log_error("wall", "failed to tighten wall state permissions",
                            errno != 0 ? errno : EACCES);
    }
}

static void host_wall_state_load(host_t *host)
{
    if (host == nullptr || host->wall_state_file_path[0] == '\0') {
        return;
    }

    host_wall_reset_locked(host);

    if (!host_ensure_private_data_path(host, host->wall_state_file_path,
                                       false)) {
        return;
    }

    FILE *fp = fopen(host->wall_state_file_path, "rb");
    if (fp == nullptr) {
        return;
    }

    wall_state_header_t header = {0};
    if (fread(&header, sizeof(header), 1U, fp) != 1U) {
        fclose(fp);
        return;
    }

    if (header.magic != WALL_STATE_MAGIC || header.version == 0U ||
        header.version > WALL_STATE_VERSION ||
        header.width != SSH_CHATTER_WALL_WIDTH ||
        header.height != SSH_CHATTER_WALL_HEIGHT) {
        fclose(fp);
        return;
    }

    ascii_pixel_t loaded[SSH_CHATTER_WALL_HEIGHT][SSH_CHATTER_WALL_WIDTH];
    if (fread(loaded, sizeof(loaded), 1U, fp) != 1U) {
        fclose(fp);
        return;
    }
    fclose(fp);

    for (size_t y = 0U; y < SSH_CHATTER_WALL_HEIGHT; ++y) {
        for (size_t x = 0U; x < SSH_CHATTER_WALL_WIDTH; ++x) {
            ascii_pixel_t *pixel = &loaded[y][x];
            unsigned char ch = (unsigned char)pixel->ch;
            if (ch < 0x20U || ch > 0x7eU) {
                pixel->ch = ' ';
            }
            pixel->color_name[sizeof(pixel->color_name) - 1U] = '\0';
            if (pixel->color_name[0] == '\0') {
                snprintf(pixel->color_name, sizeof(pixel->color_name), "%s",
                         "default");
            }
            if (pixel->updated_at_ns < 0) {
                pixel->updated_at_ns = 0;
            }
        }
    }

    ttak_mutex_lock(&host->lock);
    memcpy(host->wall, loaded, sizeof(host->wall));
    ttak_mutex_unlock(&host->lock);
}

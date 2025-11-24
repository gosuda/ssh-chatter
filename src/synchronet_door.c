#include "ssh_chatter/synchronet_door.h"
#include "ssh_chatter/host.h"
#include "ssh_chatter/client.h"
#include "ssh_chatter/memory_manager.h"
#include "ssh_chatter/user_data.h"
#include "ssh_chatter/security_layer.h"
#include "ssh_chatter/translation_helpers.h"
#include "ssh_chatter/translator.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static bool looks_like_ip_address(const char *text)
{
    if (text == nullptr) {
        return false;
    }

    bool has_separator = false;
    bool has_hex = false;
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        unsigned char ch = (unsigned char)*cursor;
        if (ch == '.' || ch == ':' || ch == '%') {
            has_separator = true;
            continue;
        }
        if (ch == '[' || ch == ']' || ch == '-') {
            continue;
        }
        if (!isxdigit(ch)) {
            return false;
        }
        has_hex = true;
    }

    return has_separator && has_hex;
}

static bool door_file_load(const char *path,
                           char lines[][SSH_CHATTER_MESSAGE_LIMIT],
                           size_t max_lines, size_t *line_count)
{
    if (path == nullptr || path[0] == '\0' || lines == nullptr ||
        line_count == nullptr) {
        return false;
    }

    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        return false;
    }

    size_t count = 0U;
    while (count < max_lines &&
           fgets(lines[count], (int)SSH_CHATTER_MESSAGE_LIMIT, fp) != nullptr) {
        lines[count][strcspn(lines[count], "\r\n")] = '\0';
        trim_whitespace_inplace(lines[count]);
        ++count;
    }

    int read_error = ferror(fp);
    fclose(fp);

    if (read_error != 0 || count == 0U) {
        return false;
    }

    *line_count = count;
    return true;
}

static void synchronet_apply_security_level(session_ctx_t *ctx,
                                            unsigned long security_level)
{
    if (ctx == nullptr) {
        return;
    }

    if (security_level >= 90UL) {
        ctx->user.is_operator = true;
        ctx->user.is_lan_operator = true;
    }
}

static bool parse_door32_lines(session_ctx_t *ctx,
                               char lines[][SSH_CHATTER_MESSAGE_LIMIT],
                               size_t line_count)
{
    if (ctx == nullptr || lines == nullptr || line_count < 6U) {
        return false;
    }

    char *endptr = nullptr;
    (void)strtoul(lines[0], &endptr, 10);
    if (lines[0][0] == '\0' || (endptr != nullptr && *endptr != '\0')) {
        return false;
    }

    const char *alias = nullptr;
    if (line_count > 5U && lines[5][0] != '\0') {
        alias = lines[5];
    }
    if ((alias == nullptr || alias[0] == '\0') && line_count > 4U &&
        lines[4][0] != '\0') {
        alias = lines[4];
    }
    if (alias == nullptr || alias[0] == '\0') {
        return false;
    }

    char cleaned_alias[SSH_CHATTER_USERNAME_LEN];
    if (!user_data_strip_ansi_sequences(alias, cleaned_alias,
                                        sizeof(cleaned_alias)) ||
        cleaned_alias[0] == '\0') {
        snprintf(cleaned_alias, sizeof(cleaned_alias), "%s", alias);
    }

    snprintf(ctx->user.name, sizeof(ctx->user.name), "%s", cleaned_alias);
    ctx->user.is_authenticated = true;

    if (line_count > 6U) {
        unsigned long security_level = strtoul(lines[6], &endptr, 10);
        if (lines[6][0] != '\0' && endptr != nullptr && *endptr == '\0') {
            synchronet_apply_security_level(ctx, security_level);
        }
    }

    for (size_t idx = 0; idx < line_count; ++idx) {
        if (looks_like_ip_address(lines[idx])) {
            snprintf(ctx->client_ip, sizeof(ctx->client_ip), "%s", lines[idx]);
            break;
        }
    }

    snprintf(ctx->client_banner, sizeof(ctx->client_banner), "%s",
             "Synchronet DOOR32");
    return true;
}

static bool parse_classic_door_lines(session_ctx_t *ctx,
                                     char lines[][SSH_CHATTER_MESSAGE_LIMIT],
                                     size_t line_count)
{
    if (ctx == nullptr || lines == nullptr || line_count < 7U) {
        return false;
    }

    if (strncasecmp(lines[0], "COM", 3) != 0 &&
        !isdigit((unsigned char)lines[0][0])) {
        return false;
    }

    char alias[SSH_CHATTER_USERNAME_LEN];
    alias[0] = '\0';

    if (line_count > 7U && lines[7][0] != '\0') {
        snprintf(alias, sizeof(alias), "%s", lines[7]);
    }

    if (alias[0] == '\0') {
        const char *first = (line_count > 5U) ? lines[5] : "";
        const char *last = (line_count > 6U) ? lines[6] : "";
        if (first[0] == '\0' && last[0] == '\0') {
            return false;
        }
        if (last[0] == '\0') {
            snprintf(alias, sizeof(alias), "%s", first);
        } else if (first[0] == '\0') {
            snprintf(alias, sizeof(alias), "%s", last);
        } else {
            snprintf(alias, sizeof(alias), "%s %s", first, last);
        }
    }

    if (alias[0] == '\0') {
        return false;
    }

    char cleaned_alias[SSH_CHATTER_USERNAME_LEN];
    if (!user_data_strip_ansi_sequences(alias, cleaned_alias,
                                        sizeof(cleaned_alias)) ||
        cleaned_alias[0] == '\0') {
        snprintf(cleaned_alias, sizeof(cleaned_alias), "%s", alias);
    }

    snprintf(ctx->user.name, sizeof(ctx->user.name), "%s", cleaned_alias);
    ctx->user.is_authenticated = true;

    for (size_t idx = 7U; idx < line_count && idx < 12U; ++idx) {
        char *endptr = nullptr;
        unsigned long maybe_level = strtoul(lines[idx], &endptr, 10);
        if (lines[idx][0] != '\0' && endptr != nullptr && *endptr == '\0') {
            synchronet_apply_security_level(ctx, maybe_level);
            break;
        }
    }

    for (size_t idx = 0; idx < line_count; ++idx) {
        if (looks_like_ip_address(lines[idx])) {
            snprintf(ctx->client_ip, sizeof(ctx->client_ip), "%s", lines[idx]);
            break;
        }
    }

    snprintf(ctx->client_banner, sizeof(ctx->client_banner), "%s",
             "Synchronet DOOR.SYS");
    return true;
}

static bool try_parse_drop_file(session_ctx_t *ctx, const char *path)
{
    char lines[64][SSH_CHATTER_MESSAGE_LIMIT];
    size_t line_count = 0U;
    if (!door_file_load(path, lines, sizeof(lines) / sizeof(lines[0]),
                        &line_count)) {
        return false;
    }

    if (parse_door32_lines(ctx, lines, line_count)) {
        return true;
    }

    return parse_classic_door_lines(ctx, lines, line_count);
}

static bool append_path(char *dest, size_t length, const char *base,
                        const char *suffix)
{
    if (dest == nullptr || length == 0U || base == nullptr || base[0] == '\0') {
        return false;
    }

    if (suffix == nullptr) {
        return snprintf(dest, length, "%s", base) > 0;
    }

    const char *separator = "";
    if (base[strlen(base) - 1U] != '/' && base[strlen(base) - 1U] != '\\') {
        separator = "/";
    }

    return snprintf(dest, length, "%s%s%s", base, separator, suffix) > 0;
}

static bool parse_door_sys(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return false;
    }

    const char *override_path = getenv("DOOR_SYS_PATH");
    const char *node_path = getenv("SBBSNODE");
    const char *node_alt_path = getenv("SBBSNODEDIR");

    char candidate[PATH_MAX];

    const char *file_names[] = {"door32.sys", "DOOR32.SYS", "door.sys",
                                "DOOR.SYS"};

    const char *bases[] = {override_path, node_path, node_alt_path, "."};

    for (size_t base_idx = 0; base_idx < sizeof(bases) / sizeof(bases[0]);
         ++base_idx) {
        const char *base = bases[base_idx];
        if (base == nullptr || base[0] == '\0') {
            continue;
        }

        bool base_is_file = false;
        const char *dot = strrchr(base, '.');
        if (dot != nullptr) {
            char ext[8];
            snprintf(ext, sizeof(ext), "%s", dot);
            for (size_t idx = 0;
                 idx < sizeof(file_names) / sizeof(file_names[0]); ++idx) {
                if (strcasecmp(ext, strrchr(file_names[idx], '.')) == 0) {
                    base_is_file = true;
                    break;
                }
            }
        }

        if (base_is_file) {
            if (append_path(candidate, sizeof(candidate), base, nullptr) &&
                try_parse_drop_file(ctx, candidate)) {
                return true;
            }
            continue;
        }

        for (size_t idx = 0; idx < sizeof(file_names) / sizeof(file_names[0]);
             ++idx) {
            if (!append_path(candidate, sizeof(candidate), base,
                             file_names[idx])) {
                continue;
            }
            if (try_parse_drop_file(ctx, candidate)) {
                return true;
            }
        }
    }

    snprintf(ctx->user.name, sizeof(ctx->user.name), "%s", "SynchronetUser");
    ctx->user.is_authenticated = true;
    snprintf(ctx->client_banner, sizeof(ctx->client_banner), "%s",
             "Synchronet Door");
    return false;
}

// Main function for running in Synchronet door mode
int synchronet_door_run(void)
{
    printf("Synchronet door mode activated.\n");

    // Initialize a dummy host for the session context
    host_t *host = calloc(1, sizeof(*host));
    if (host == nullptr) {
        fprintf(stderr, "Failed to allocate host for Synchronet door.\n");
        return EXIT_FAILURE;
    }
    host->memory_context = sshc_memory_context_create("synchronet_host");
    if (host->memory_context == nullptr) {
        fprintf(stderr,
                "Failed to create memory context for Synchronet host.\n");
        GC_FREE(host);
        return EXIT_FAILURE;
    }
    host_init(host, nullptr); // Initialize host with default profile

    // Create a session context for the Synchronet user
    session_ctx_t *ctx = session_create();
    if (ctx == nullptr) {
        fprintf(stderr,
                "Failed to create session context for Synchronet door.\n");
        host_shutdown(host);
        sshc_memory_context_destroy(host->memory_context);
        GC_FREE(host);
        return EXIT_FAILURE;
    }
    ctx->owner = host;
    ctx->transport_kind = SESSION_TRANSPORT_TELNET; // Treat as Telnet for now

    if (!parse_door_sys(ctx)) {
        fprintf(
            stderr,
            "Warning: unable to locate DOOR.SYS/DOOR32.SYS, using defaults.\n");
    }

    // Main loop for Synchronet door
    char input_buffer[SSH_CHATTER_MESSAGE_LIMIT];
    while (fgets(input_buffer, sizeof(input_buffer), stdin) != nullptr) {
        // Remove newline characters
        input_buffer[strcspn(input_buffer, "\r\n")] = 0;

        // Process input (e.g., chat messages, game commands)
        // For now, just echo back and handle a simple exit command
        if (strcmp(input_buffer, "/exit") == 0) {
            session_send_system_line(ctx,
                                     "Exiting Synchronet door mode. Goodbye!");
            break;
        } else if (strcmp(input_buffer, "/tetris") == 0) {
            session_game_start_tetris(ctx);
        } else {
            char output_buffer[SSH_CHATTER_MESSAGE_LIMIT + 32];
            snprintf(output_buffer, sizeof(output_buffer), "You said: %s\n",
                     input_buffer);
            session_send_raw_text(ctx, output_buffer);
        }
    }

    // Cleanup
    session_destroy(ctx);
    host_shutdown(host);
    sshc_memory_context_destroy(host->memory_context);
    GC_FREE(host);

    return EXIT_SUCCESS;
}

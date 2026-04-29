#ifndef SSH_CHATTER_HOST_INTERNAL_H
#define SSH_CHATTER_HOST_INTERNAL_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "ssh_chatter/host.h"
#include "ssh_chatter/client.h"
#include "ssh_chatter/webssh_client.h"
#include "ssh_chatter/morse_client.h"
#include "ssh_chatter/translator.h"
#include "ssh_chatter/translation_helpers.h"
#include "ssh_chatter/file_transfer.h"

#include "ssh_chatter/memory_manager.h"

#ifndef HOST_MEMORY_SCOPE_HELPERS
#define HOST_MEMORY_SCOPE_HELPERS

static inline sshc_memory_context_t *
session_memory_scope_push(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->memory_context == nullptr) {
        return nullptr;
    }
    return sshc_memory_context_push(ctx->memory_context);
}

static inline void session_memory_scope_pop(sshc_memory_context_t *scope)
{
    if (scope != nullptr) {
        sshc_memory_context_pop(scope);
    }
}

static inline sshc_memory_context_t *
host_memory_scope_push(host_t *host)
{
    if (host == nullptr || host->memory_context == nullptr) {
        return nullptr;
    }
    return sshc_memory_context_push(host->memory_context);
}

static inline void host_memory_scope_pop(sshc_memory_context_t *scope)
{
    if (scope != nullptr) {
        sshc_memory_context_pop(scope);
    }
}

#endif

#define TELNET_IAC 255
#define TELNET_CMD_SE 240
#define TELNET_CMD_NOP 241
#define TELNET_CMD_DM 242
#define TELNET_CMD_BREAK 243
#define TELNET_CMD_WILL 251
#define TELNET_CMD_WONT 252
#define TELNET_CMD_DO 253
#define TELNET_CMD_DONT 254
#define TELNET_CMD_SB 250
#define TELNET_OPT_BINARY 0
#define TELNET_OPT_ECHO 1
#define TELNET_OPT_SUPPRESS_GO_AHEAD 3
#define TELNET_OPT_STATUS 5
#define TELNET_OPT_TERMINAL_TYPE 24
#define TELNET_OPT_NAWS 31
#define TELNET_OPT_TERMINAL_SPEED 32
#define TELNET_OPT_LINEMODE 34

#include <arpa/inet.h>
#include <ctype.h>
#include <curl/curl.h>

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>

#include <iconv.h>
#include <inttypes.h>

#include <libgen.h>
#include <libssh/libssh.h>
#include <libssh/server.h>

#include <limits.h>
#include <math.h>

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

lan_operator_credential_t *
host_find_lan_operator_credential(host_t *host, const char *username);
bool session_is_lan_client(const char *ip);
bool session_detect_provider_ip(const char *ip, char *label, size_t length);

#include <poll.h>
#include <pthread.h>
#include <signal.h>

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <wchar.h>
#include <wctype.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <setjmp.h>

#include "ssh_chatter/memory_manager.h"
#include "ssh_chatter/ssh_chatter_sync.h"
#include "ssh_chatter/humanized/humanized.h"

bool session_game_othello_handle_forced_exit(session_ctx_t *ctx);
bool host_json_api_listener_start(host_t *host, const char *bind_addr,
                                  const char *port);
void host_json_api_listener_stop(host_t *host);

static inline session_output_kind_t
session_output_set_kind(session_ctx_t *ctx, session_output_kind_t new_kind)
{
    if (ctx == nullptr) {
        return SESSION_OUTPUT_KIND_SYSTEM;
    }

    session_output_kind_t previous = ctx->output_kind;
    ctx->output_kind = new_kind;
    return previous;
}

static inline void
session_output_restore_kind(session_ctx_t *ctx,
                            session_output_kind_t previous_kind)
{
    if (ctx == nullptr) {
        return;
    }

    ctx->output_kind = previous_kind;
}

static inline bool host_bbs_storage_ready(const host_t *host)
{
    return host != nullptr && host->bbs_posts != nullptr &&
           host->bbs_post_capacity > 0U;
}

static inline size_t host_bbs_loop_limit(const host_t *host)
{
    return host_bbs_storage_ready(host) ? host->bbs_post_capacity : 0U;
}

static inline void session_build_network_topology_key(const session_ctx_t *ctx,
                                                      char *buffer,
                                                      size_t length)
{
    if (buffer == nullptr || length == 0U) {
        return;
    }

    buffer[0] = '\0';
    if (ctx == nullptr) {
        return;
    }

    const char *ip = ctx->client_ip[0] != '\0' ? ctx->client_ip : "-";
    const char *terminal =
        ctx->terminal_type[0] != '\0' ? ctx->terminal_type : "-";
    const char *identity =
        ctx->telnet_identity[0] != '\0' ? ctx->telnet_identity : "-";
    const char *banner =
        ctx->client_banner[0] != '\0' ? ctx->client_banner : "-";
    const char *transport =
        ctx->transport_kind == SESSION_TRANSPORT_TELNET ? "telnet" : "ssh";
    const int lan = session_is_lan_client(ip) ? 1 : 0;

    snprintf(buffer, length, "ip=%s|lan=%d|transport=%s|term=%s|id=%s|banner=%s",
             ip, lan, transport, terminal, identity, banner);
}

static inline bool session_output_should_use_retro_encoding(
    const session_ctx_t *ctx, session_output_kind_t kind)
{
    if (ctx == nullptr || !ctx->prefer_cp437_output) {
        return false;
    }

    switch (ctx->cp437_output_scope) {
    case SESSION_CP437_SCOPE_SYSTEM_ONLY:
        return kind == SESSION_OUTPUT_KIND_SYSTEM;
    case SESSION_CP437_SCOPE_CHAT_ONLY:
        return kind == SESSION_OUTPUT_KIND_CHAT;
    case SESSION_CP437_SCOPE_ALL:
    default:
        return true;
    }
}

size_t session_cp437_byte_to_utf8(unsigned char byte, char *output,
                                  size_t capacity);

void session_render_banner_ascii(session_ctx_t *ctx);
void session_note_output_lines(session_ctx_t *ctx, size_t line_count);
void session_manual_gc_tick(session_ctx_t *ctx);
void host_manual_gc_tick(host_t *host);

bool host_compact_id_encode(uint64_t id, char *buffer, size_t length);
bool host_compact_id_decode(const char *text, uint64_t *id_out);

void session_scrollback_reset_position(session_ctx_t *ctx);
void session_scrollback_navigate(session_ctx_t *ctx, int direction,
                                 size_t step);
void session_process_pending_sink(session_ctx_t *ctx);
void session_flag_should_sink(session_ctx_t *ctx);
void session_mark_should_sink(session_ctx_t *ctx);
void session_clear_pending_sink(session_ctx_t *ctx);
void session_send_system_line(session_ctx_t *ctx, const char *message);
void session_handle_wall(session_ctx_t *ctx, const char *arguments);
void session_wall_render(session_ctx_t *ctx, const char *status);
void session_wall_exit(session_ctx_t *ctx, const char *status);
bool session_wall_process_line(session_ctx_t *ctx, const char *line,
                               size_t length);
bool session_wall_process_escape(session_ctx_t *ctx, const char *sequence,
                                 size_t length);

session_ui_language_t session_ui_language_from_code(const char *code);
void host_store_ui_language(host_t *host, const session_ctx_t *ctx);

bool session_bbs_workspace_acquire(session_ctx_t *ctx);
void session_bbs_workspace_release(session_ctx_t *ctx);
bool session_bbs_view_notice_acquire(session_ctx_t *ctx);
void session_bbs_view_notice_release(session_ctx_t *ctx);
bool session_asciiart_buffer_acquire(session_ctx_t *ctx);
void session_asciiart_buffer_release(session_ctx_t *ctx);
void session_compressed_buffers_discard(session_ctx_t *ctx);
chat_history_entry_t *session_scrollback_buffer_acquire(session_ctx_t *ctx,
                                                        size_t minimum_capacity,
                                                        size_t *out_capacity);
void session_scrollback_buffer_release(session_ctx_t *ctx);
bool session_tetris_buffers_acquire(session_ctx_t *ctx);
void session_tetris_buffers_release(session_ctx_t *ctx);
tetris_game_state_t *session_game_ensure_tetris(session_ctx_t *ctx);
tetris_game_state_t *session_game_ensure_saved_tetris(session_ctx_t *ctx);
void session_game_release_tetris(session_ctx_t *ctx);
void session_game_release_saved_tetris(session_ctx_t *ctx);
liar_game_state_t *session_game_ensure_liar(session_ctx_t *ctx);
liar_game_state_t *session_game_ensure_saved_liar(session_ctx_t *ctx);
void session_game_release_liar(session_ctx_t *ctx);
void session_game_release_saved_liar(session_ctx_t *ctx);
alpha_centauri_game_state_t *session_game_ensure_alpha(session_ctx_t *ctx);
alpha_centauri_game_state_t *session_game_ensure_saved_alpha(session_ctx_t *ctx);
void session_game_release_alpha(session_ctx_t *ctx);
void session_game_release_saved_alpha(session_ctx_t *ctx);
othello_game_state_t *session_game_ensure_othello(session_ctx_t *ctx);
othello_game_state_t *
session_game_ensure_saved_othello(session_ctx_t *ctx);
void session_game_release_othello(session_ctx_t *ctx);
void session_game_release_saved_othello(session_ctx_t *ctx);
gonu_game_state_t *session_game_ensure_gonu(session_ctx_t *ctx);
gonu_game_state_t *session_game_ensure_saved_gonu(session_ctx_t *ctx);
void session_game_release_gonu(session_ctx_t *ctx);
void session_game_release_saved_gonu(session_ctx_t *ctx);
void session_game_handle_screen_cleared(session_ctx_t *ctx);
bool session_release_optional_buffers_if_idle(session_ctx_t *ctx,
                                              const struct timespec *now);
void session_mark_activity(session_ctx_t *ctx);
bool session_enforce_lifetime(session_ctx_t *ctx, const struct timespec *now);

static inline const char *
chat_history_entry_display_name(const chat_history_entry_t *entry)
{
    if (entry == nullptr) {
        return "";
    }

    if (entry->raw_username[0] != '\0') {
        return entry->raw_username;
    }

    return entry->username;
}

static inline void chat_history_entry_format_user_line(
    const chat_history_entry_t *entry, char *buffer, size_t length,
    bool append_message)
{
    if (buffer == nullptr || length == 0U) {
        return;
    }

    buffer[0] = '\0';
    if (entry == nullptr || !entry->is_user_message) {
        return;
    }

    const char *highlight = (entry->user_highlight_code[0] != '\0')
                                ? entry->user_highlight_code
                                : "";
    const char *color =
        (entry->user_color_code[0] != '\0') ? entry->user_color_code : "";
    const char *bold = entry->user_is_bold ? ANSI_BOLD : "";
    const bool has_custom_codes = (color[0] != '\0') || (highlight[0] != '\0');

    char id_label[32] = "-";
    if (entry->message_id > 0U) {
        host_compact_id_encode(entry->message_id, id_label, sizeof(id_label));
    }

    const char *display_name = chat_history_entry_display_name(entry);
    if (has_custom_codes) {
        snprintf(buffer, length,
                 ANSI_CYAN "[%s]" ANSI_RESET " <%s%s%s%s%s>%s%s", id_label,
                 highlight, color, bold, display_name, ANSI_RESET,
                 append_message ? " " : "",
                 (append_message && entry->message[0] != '\0') ? entry->message
                                                                : "");
    } else {
        snprintf(buffer, length,
                 "%s%s%s " ANSI_CYAN "[%s]" ANSI_RESET " <%s>%s%s%s",
                 highlight, bold, color, id_label, display_name, ANSI_RESET,
                 append_message ? " " : "",
                 (append_message && entry->message[0] != '\0') ? entry->message
                                                                : "");
    }
}

#endif // SSH_CHATTER_HOST_INTERNAL_H

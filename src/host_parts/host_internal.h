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

session_ui_language_t session_ui_language_from_code(const char *code);
void host_store_ui_language(host_t *host, const session_ctx_t *ctx);

bool session_bbs_workspace_acquire(session_ctx_t *ctx);
void session_bbs_workspace_release(session_ctx_t *ctx);
bool session_bbs_view_notice_acquire(session_ctx_t *ctx);
void session_bbs_view_notice_release(session_ctx_t *ctx);
bool session_asciiart_buffer_acquire(session_ctx_t *ctx);
void session_asciiart_buffer_release(session_ctx_t *ctx);
bool session_tetris_buffers_acquire(session_ctx_t *ctx);
void session_tetris_buffers_release(session_ctx_t *ctx);
tetris_game_state_t *session_game_ensure_tetris(session_ctx_t *ctx);
tetris_game_state_t *session_game_ensure_saved_tetris(session_ctx_t *ctx);
void session_game_release_tetris(session_ctx_t *ctx);
void session_game_release_saved_tetris(session_ctx_t *ctx);
void session_game_handle_screen_cleared(session_ctx_t *ctx);
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

#endif // SSH_CHATTER_HOST_INTERNAL_H

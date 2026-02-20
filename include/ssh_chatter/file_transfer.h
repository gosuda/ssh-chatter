#ifndef SSH_CHATTER_FILE_TRANSFER_H
#define SSH_CHATTER_FILE_TRANSFER_H

#include "ssh_chatter/host.h"

bool host_file_storage_init(host_t *host);
bool host_file_storage_list(host_t *host, char *buffer, size_t length);
bool file_transfer_resolve_path(host_t *host, const char *virtual_path,
                                char *resolved, size_t resolved_len,
                                char *display, size_t display_len);
bool file_transfer_telnet_receive(session_ctx_t *ctx,
                                  const char *resolved_target);
bool file_transfer_telnet_send(session_ctx_t *ctx, const char *virtual_path);
int file_transfer_handle_scp_exec(session_ctx_t *ctx, const char *command);
int file_transfer_handle_sftp(session_ctx_t *ctx);

#endif /* SSH_CHATTER_FILE_TRANSFER_H */

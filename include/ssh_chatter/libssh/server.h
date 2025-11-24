#ifndef LIBSSH_SERVER_H
#define LIBSSH_SERVER_H

#include <libssh/libssh.h>

typedef struct ssh_bind_struct {
    int placeholder;
} *ssh_bind;

typedef struct ssh_message_struct {
    int type;
    int subtype;
    char user[32];
    char service[32];
} *ssh_message;

typedef struct ssh_key_struct {
    int placeholder;
} *ssh_key;

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SSH_BIND_OPTIONS_BINDADDR,
    SSH_BIND_OPTIONS_BINDPORT,
    SSH_BIND_OPTIONS_BINDPORT_STR,
    SSH_BIND_OPTIONS_HOSTKEY,
    SSH_BIND_OPTIONS_DSAKEY,
    SSH_BIND_OPTIONS_RSAKEY,
    SSH_BIND_OPTIONS_BANNER,
    SSH_BIND_OPTIONS_LOG_VERBOSITY,
    SSH_BIND_OPTIONS_LOG_VERBOSITY_STR,
    SSH_BIND_OPTIONS_ECDSAKEY,
    SSH_BIND_OPTIONS_IMPORT_KEY,
    SSH_BIND_OPTIONS_KEY_EXCHANGE,
    SSH_BIND_OPTIONS_CIPHERS_C_S,
    SSH_BIND_OPTIONS_CIPHERS_S_C,
    SSH_BIND_OPTIONS_HMAC_C_S,
    SSH_BIND_OPTIONS_HMAC_S_C,
    SSH_BIND_OPTIONS_COMPRESSION_C_S,
    SSH_BIND_OPTIONS_COMPRESSION_S_C,
    SSH_BIND_OPTIONS_CONFIG_DIR,
    SSH_BIND_OPTIONS_PUBKEY_ACCEPTED_KEY_TYPES,
    SSH_BIND_OPTIONS_HOSTKEY_ALGORITHMS,
    SSH_BIND_OPTIONS_PROCESS_CONFIG,
    SSH_BIND_OPTIONS_MODULI,
    SSH_BIND_OPTIONS_RSA_MIN_SIZE
} ssh_bind_options_e;

ssh_bind ssh_bind_new(void);
void ssh_bind_free(ssh_bind bind);
int ssh_bind_options_set(ssh_bind bind, ssh_bind_options_e type,
                         const void *value);
int ssh_bind_listen(ssh_bind bind);
int ssh_bind_accept(ssh_bind bind, ssh_session session);
const char *ssh_message_service_service(ssh_message message);
int ssh_message_service_reply_success(ssh_message message);
void ssh_message_reply_default(ssh_message message);
const char *ssh_message_auth_user(ssh_message message);
int ssh_message_auth_reply_success(ssh_message message, int partial);
int ssh_channel_request_send_exit_status(ssh_channel channel, int exit_status);
int ssh_handle_key_exchange(ssh_session session);
const char *ssh_message_auth_password(ssh_message message);
int ssh_message_auth_set_methods(ssh_message message, int methods);
unsigned int ssh_message_channel_request_pty_width(ssh_message message);
unsigned int ssh_message_channel_request_pty_height(ssh_message message);
#if defined(LIBSSH_VERSION_INT) && defined(SSH_VERSION_INT)
#if LIBSSH_VERSION_INT >= SSH_VERSION_INT(0, 9, 0)
unsigned int
ssh_message_channel_request_window_change_width(ssh_message message);
unsigned int
ssh_message_channel_request_window_change_height(ssh_message message);
#endif
#endif

#ifdef __cplusplus
}
#endif

#endif

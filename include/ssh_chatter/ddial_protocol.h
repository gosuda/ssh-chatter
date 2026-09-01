#ifndef SSH_CHATTER_DDIAL_PROTOCOL_H
#define SSH_CHATTER_DDIAL_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DDIAL_MIN_CHANNEL 1
#define DDIAL_MAX_CHANNEL 4
#define DDIAL_DEFAULT_CHANNEL 1
#define DDIAL_MAX_HANDLE_LEN 32
#define DDIAL_MAX_HANDLE_WIRE_LEN 25 /* spec: never exceed 25 chars */
#define DDIAL_MAX_MESSAGE_LEN 512
#define DDIAL_MAX_BODY_LEN 255 /* spec: ddial message body limit */
#define DDIAL_MAX_LINE_LEN 1024
#define DDIAL_LINK_ESCAPE 0x7E /* '~' */
/* How often a linked station sends its }}} user broadcast list. */
#define DDIAL_STATION_BROADCAST_INTERVAL_SEC (15 * 60)

typedef enum ddial_command {
    DDIAL_CMD_UNKNOWN = 0,
    DDIAL_CMD_CHAT,       /* /C */
    DDIAL_CMD_PRIVATE,    /* /P */
    DDIAL_CMD_JOIN,       /* /J */
    DDIAL_CMD_WHO,        /* /W */
    DDIAL_CMD_MAIL,       /* /E */
    DDIAL_CMD_QUIT,       /* /Q */
    DDIAL_CMD_HELP,       /* /H, /? */
    DDIAL_CMD_TIME,       /* /T */
    DDIAL_CMD_USERS,      /* /U */
    DDIAL_CMD_STATS,      /* /S */
    DDIAL_CMD_NEWS,       /* /N */
    DDIAL_CMD_INFO,       /* /I */
    DDIAL_CMD_VERSION,    /* /V */
    DDIAL_CMD_BELL,       /* /B */
    DDIAL_CMD_DUPLEX,     /* /D */
    DDIAL_CMD_KICK,       /* /K<slot> - cosysop only over link */
    DDIAL_CMD_LINK,       /* /LINK (legacy extension stub) */
} ddial_command_t;

typedef enum ddial_user_tier {
    DDIAL_TIER_GUEST = 0,
    DDIAL_TIER_PASSWORD,
    DDIAL_TIER_MASTER,
} ddial_user_tier_t;

/* Inbound link broadcast message type (prefix parsing result). */
typedef enum ddial_link_msg {
    DDIAL_LINK_MSG_UNKNOWN = 0,
    DDIAL_LINK_MSG_MEMBER_EVENT,      /* } prefix  - member login/logout    */
    DDIAL_LINK_MSG_GUEST_EVENT,       /* }} prefix - guest login/logout      */
    DDIAL_LINK_MSG_STATION_BROADCAST, /* }}} prefix - /SP who-is-online list */
} ddial_link_msg_t;

typedef struct ddial_parsed_message {
    ddial_command_t command;
    char handle[DDIAL_MAX_HANDLE_LEN];
    char body[DDIAL_MAX_MESSAGE_LEN];
    uint8_t channel;
} ddial_parsed_message_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque session handle used by the DDial server and integration registry. */
struct ddial_session;

void host_ddial_register_session(struct ddial_session *sess);
void host_ddial_unregister_session(struct ddial_session *sess);
void host_ddial_broadcast_to_sessions(host_t *host, const char *message);
bool host_ddial_send_private(host_t *host, uint16_t target_slot,
                             uint16_t from_slot, const char *from_handle,
                             const char *message);

ddial_command_t ddial_parse_command(const char *line, size_t line_len,
                                    ddial_parsed_message_t *out_msg);

/* Standard (non-link) formatters */
bool ddial_format_chat(char *dst, size_t dst_cap, uint16_t slot,
                       uint8_t channel, ddial_user_tier_t tier,
                       const char *handle, const char *message);

bool ddial_format_private(char *dst, size_t dst_cap, uint16_t from_slot,
                          const char *from_handle, const char *message);

bool ddial_format_who_entry(char *dst, size_t dst_cap, uint16_t slot,
                            uint8_t channel, ddial_user_tier_t tier,
                            const char *handle, uint16_t account);

bool ddial_format_prompt(char *dst, size_t dst_cap);

bool ddial_format_system(char *dst, size_t dst_cap, const char *message);

/* Link-mode wire formatters */
bool ddial_format_link_chat(char *dst, size_t dst_cap, uint16_t slot,
                            uint8_t channel, ddial_user_tier_t tier,
                            const char *handle, const char *message);

bool ddial_format_link_private(char *dst, size_t dst_cap,
                               uint16_t target_slot,
                               uint16_t our_slot, uint8_t channel,
                               ddial_user_tier_t our_tier,
                               const char *our_handle, const char *message);

bool ddial_format_link_email(char *dst, size_t dst_cap,
                             uint16_t to_station, uint16_t from_account,
                             const char *from_handle, const char *message);

bool ddial_format_link_login(char *dst, size_t dst_cap,
                             uint16_t slot, uint8_t channel,
                             ddial_user_tier_t tier,
                             const char *handle, uint16_t account,
                             bool is_link, bool station_locked);

bool ddial_format_link_logout(char *dst, size_t dst_cap,
                              uint16_t slot, uint8_t channel,
                              ddial_user_tier_t tier,
                              const char *handle, uint16_t account,
                              bool is_link, bool station_locked);

/* Inbound link helpers */
ddial_link_msg_t ddial_parse_link_prefix(const char *line,
                                          const char **out_rest);

/* Handle/slot hygiene per the wire spec:
 * handles may never contain '^' ')' '}' CR or LF and are capped at
 * DDIAL_MAX_HANDLE_WIRE_LEN; slot numbers containing the digit 8 or 9 are
 * not recognized by some ddials and must be skipped. */
size_t ddial_sanitize_handle(const char *src, size_t src_len, char *dst,
                             size_t dst_cap);
bool ddial_slot_is_valid(uint16_t slot);

/* Dual-channel link chat: ~#slot<bracket>T<ch>:<handle>) message\r\n */
bool ddial_format_link_dual_chat(char *dst, size_t dst_cap, uint16_t slot,
                                 uint8_t channel, ddial_user_tier_t tier,
                                 const char *handle, const char *message);

/* Station broadcast list (}}} prefix).  Build the header first, then append
 * one entry per online user; terminate the finished list with "\r\n". */
bool ddial_format_broadcast_header(char *dst, size_t dst_cap,
                                   const char *station_name,
                                   bool station_locked);
bool ddial_format_broadcast_entry(char *dst, size_t dst_cap, uint16_t slot,
                                  uint8_t channel, ddial_user_tier_t tier,
                                  const char *handle, uint16_t account,
                                  bool is_link);

/* Inbound link line parsers.  All take a single line without CR/LF. */
bool ddial_parse_incoming_chat(const char *line, uint16_t *out_link_slot,
                               uint16_t *out_slot, uint8_t *out_channel,
                               bool *out_is_link, bool *out_dual_channel,
                               char *out_handle, size_t handle_cap,
                               const char **out_message);

bool ddial_parse_incoming_private(const char *line, uint16_t *out_target_slot,
                                  uint16_t *out_from_slot,
                                  uint8_t *out_channel, char *out_handle,
                                  size_t handle_cap, const char **out_message);

bool ddial_parse_incoming_email(const char *line, unsigned *out_from_station,
                                unsigned *out_from_id, char *out_handle,
                                size_t handle_cap, const char **out_message);

size_t ddial_expand_carets(const char *src, size_t src_len,
                           char *dst, size_t dst_cap);

size_t ddial_strip_ansi(const char *src, size_t src_len, char *dst,
                        size_t dst_cap);

size_t ddial_filter_telnet_iac(const char *src, size_t src_len, char *dst,
                               size_t dst_cap);

#ifdef __cplusplus
}
#endif

#endif /* SSH_CHATTER_DDIAL_PROTOCOL_H */

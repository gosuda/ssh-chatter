#ifndef SSH_CHATTER_DDIAL_PROTOCOL_H
#define SSH_CHATTER_DDIAL_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DDIAL_MIN_CHANNEL 1
#define DDIAL_MAX_CHANNEL 4
#define DDIAL_DEFAULT_CHANNEL 1
#define DDIAL_MAX_HANDLE_LEN 32
#define DDIAL_MAX_MESSAGE_LEN 512
#define DDIAL_MAX_LINE_LEN 1024
#define DDIAL_LINK_ESCAPE 0x7E /* '~' */

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
    DDIAL_CMD_LINK,       /* /LINK (legacy extension stub) */
} ddial_command_t;

typedef enum ddial_user_tier {
    DDIAL_TIER_GUEST = 0,
    DDIAL_TIER_PASSWORD,
    DDIAL_TIER_MASTER,
} ddial_user_tier_t;

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

ddial_command_t ddial_parse_command(const char *line, size_t line_len,
                                    ddial_parsed_message_t *out_msg);

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

size_t ddial_strip_ansi(const char *src, size_t src_len, char *dst,
                        size_t dst_cap);

size_t ddial_filter_telnet_iac(const char *src, size_t src_len, char *dst,
                               size_t dst_cap);

#ifdef __cplusplus
}
#endif

#endif /* SSH_CHATTER_DDIAL_PROTOCOL_H */

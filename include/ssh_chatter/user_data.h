#ifndef SSH_CHATTER_USER_DATA_H
#define SSH_CHATTER_USER_DATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef USER_DATA_MAILBOX_LIMIT
#define USER_DATA_MAILBOX_LIMIT 32U
#endif

#ifndef USER_DATA_MAILBOX_MESSAGE_LEN
#define USER_DATA_MAILBOX_MESSAGE_LEN 256U
#endif

#ifndef USER_DATA_PROFILE_PICTURE_LEN
#define USER_DATA_PROFILE_PICTURE_LEN 20480U
#endif

#ifndef USER_DATA_FLAG_HISTORY_LIMIT
#define USER_DATA_FLAG_HISTORY_LIMIT 16U
#endif

#ifndef SSH_CHATTER_USERNAME_LEN
#error "SSH_CHATTER_USERNAME_LEN must be defined before including user_data.h"
#endif

#ifndef SSH_CHATTER_COLOR_CODE_LEN
#error "SSH_CHATTER_COLOR_CODE_LEN must be defined before including user_data.h"
#endif

#ifndef SSH_CHATTER_COLOR_NAME_LEN
#error "SSH_CHATTER_COLOR_NAME_LEN must be defined before including user_data.h"
#endif

#ifndef SSH_CHAT_SERVER_URL_LEN
#define SSH_CHAT_SERVER_URL_LEN 256U
#endif

typedef struct user_data_mail_entry {
    uint64_t timestamp;
    char sender[SSH_CHATTER_USERNAME_LEN];
    char message[USER_DATA_MAILBOX_MESSAGE_LEN];
} user_data_mail_entry_t;

typedef struct alpha_centauri_save {
    uint8_t active;
    uint8_t stage;
    uint8_t eva_ready;
    uint8_t awaiting_flag;
    double velocity_fraction_c;
    double distance_travelled_ly;
    double distance_remaining_ly;
    double fuel_percent;
    double oxygen_days;
    double mission_time_years;
    double radiation_msv;
} alpha_centauri_save_t;

typedef struct user_data_record {
    uint32_t magic;
    uint32_t version;
    char username[SSH_CHATTER_USERNAME_LEN];
    char preferred_nickname[SSH_CHATTER_USERNAME_LEN];
    uint8_t has_user_theme;
    uint8_t user_is_bold;
    char user_color_code[SSH_CHATTER_COLOR_CODE_LEN];
    char user_highlight_code[SSH_CHATTER_COLOR_CODE_LEN];
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char last_ip[SSH_CHATTER_IP_LEN];
    uint32_t mailbox_count;
    user_data_mail_entry_t mailbox[USER_DATA_MAILBOX_LIMIT];
    char profile_picture[USER_DATA_PROFILE_PICTURE_LEN];
    alpha_centauri_save_t alpha;
    uint32_t flag_count;
    uint32_t flag_history_count;
    uint64_t flag_history[USER_DATA_FLAG_HISTORY_LIMIT];
    uint64_t last_updated;
    char ssh_chat_server_url[SSH_CHAT_SERVER_URL_LEN];
    uint16_t ssh_chat_server_port;
    uint8_t password_salt[16];
    uint8_t password_hash[32];
    uint8_t reserved[16];
} user_data_record_t;

bool user_data_sanitize_username(const char *restrict username,
                                 char *restrict sanitized, size_t length);
bool user_data_strip_ansi_sequences(const char *restrict input,
                                    char *restrict output, size_t length);
bool user_data_path_for(const char *restrict root,
                        const char *restrict username, const char *restrict ip,
                        bool create_if_missing, char *restrict path,
                        size_t length);
bool user_data_ensure_root(const char *restrict root);
bool user_data_init(user_data_record_t *restrict record,
                    const char *restrict username, const char *restrict ip);
bool user_data_load(const char *restrict root, const char *restrict username,
                    const char *restrict ip,
                    user_data_record_t *restrict record);
bool user_data_save(const char *restrict root,
                    const user_data_record_t *restrict record,
                    const char *restrict ip);
bool user_data_ensure_exists(const char *restrict root,
                             const char *restrict username,
                             const char *restrict ip,
                             user_data_record_t *restrict record);
void user_data_set_ssh_chat_server_config(user_data_record_t *restrict record,
                                          const char *restrict url,
                                          uint16_t port);
void user_data_get_ssh_chat_server_config(
    const user_data_record_t *restrict record, char *restrict url,
    size_t url_len, uint16_t *restrict port);
bool user_data_has_password(const user_data_record_t *restrict record);
bool user_data_reserved_nickname_is_ip_wide(
    const user_data_record_t *restrict record);
void user_data_set_reserved_nickname_ip_wide(
    user_data_record_t *restrict record, bool enabled);

#endif /* SSH_CHATTER_USER_DATA_H */

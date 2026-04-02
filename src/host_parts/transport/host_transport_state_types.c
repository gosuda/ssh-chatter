static const uint32_t HOST_STATE_MAGIC = 0x53484354U; /* 'SHCT' */
static const uint32_t HOST_STATE_VERSION = 16U;
static const uint32_t ELIZA_STATE_MAGIC = 0x454c5354U; /* 'ELST' */
static const uint32_t ELIZA_STATE_VERSION = 1U;

typedef struct eliza_memory_header {
    uint32_t magic;
    uint32_t version;
    uint32_t entry_count;
    uint32_t reserved;
    uint64_t next_id;
} eliza_memory_header_t;

typedef struct eliza_memory_entry_serialized {
    uint64_t id;
    int64_t stored_at;
    char prompt[SSH_CHATTER_MESSAGE_LIMIT];
    char reply[SSH_CHATTER_MESSAGE_LIMIT];
} eliza_memory_entry_serialized_t;

typedef struct eliza_state_record {
    uint32_t magic;
    uint32_t version;
    uint8_t enabled;
    uint8_t reserved[7];
} eliza_state_record_t;

typedef struct host_state_header_v1 {
    uint32_t magic;
    uint32_t version;
    uint32_t history_count;
    uint32_t preference_count;
} host_state_header_v1_t;

typedef struct host_state_header {
    host_state_header_v1_t base;
    uint32_t legacy_sound_count;
    uint32_t grant_count;
    uint64_t next_message_id;
    uint8_t captcha_enabled;
    uint8_t geo_language_enabled;
    uint8_t reserved[6];
} host_state_header_t;

typedef struct host_state_history_entry {
    uint8_t is_user_message;
    uint8_t user_is_bold;
    char username[SSH_CHATTER_USERNAME_LEN];
    char message[SSH_CHATTER_MESSAGE_LIMIT];
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char raw_username[SSH_CHATTER_USERNAME_LEN];
    uint64_t message_id;
    int64_t created_at;
    uint8_t attachment_type;
    uint8_t reserved[7];
    char attachment_target[SSH_CHATTER_ATTACHMENT_TARGET_LEN];
    char attachment_caption[SSH_CHATTER_ATTACHMENT_CAPTION_LEN];
    char user_color_code[SSH_CHATTER_COLOR_CODE_LEN];
    char user_highlight_code[SSH_CHATTER_COLOR_CODE_LEN];
    uint32_t reaction_counts[SSH_CHATTER_REACTION_KIND_COUNT];
} host_state_history_entry_t;

static void host_state_assign_color_codes(chat_history_entry_t *entry,
                                          const char *color_code,
                                          const char *highlight_code)
{
    if (entry == nullptr) {
        return;
    }

    entry->user_color_code[0] = '\0';
    entry->user_highlight_code[0] = '\0';

    if (color_code != nullptr && color_code[0] != '\0') {
        snprintf(entry->user_color_code, sizeof(entry->user_color_code), "%s",
                 color_code);
    }

    if (highlight_code != nullptr && highlight_code[0] != '\0') {
        snprintf(entry->user_highlight_code,
                 sizeof(entry->user_highlight_code), "%s", highlight_code);
    }
}

typedef struct host_state_preference_entry_v3 {
    uint8_t has_user_theme;
    uint8_t has_system_theme;
    uint8_t user_is_bold;
    uint8_t system_is_bold;
    char username[SSH_CHATTER_USERNAME_LEN];
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_fg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_bg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
} host_state_preference_entry_v3_t;

typedef struct host_state_preference_entry_v4 {
    uint8_t has_user_theme;
    uint8_t has_system_theme;
    uint8_t user_is_bold;
    uint8_t system_is_bold;
    char username[SSH_CHATTER_USERNAME_LEN];
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_fg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_bg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char os_name[SSH_CHATTER_OS_NAME_LEN];
    int32_t daily_year;
    int32_t daily_yday;
    char daily_function[64];
    uint64_t last_poll_id;
    int32_t last_poll_choice;
} host_state_preference_entry_v4_t;

typedef struct host_state_preference_entry_v5 {
    uint8_t has_user_theme;
    uint8_t has_system_theme;
    uint8_t user_is_bold;
    uint8_t system_is_bold;
    char username[SSH_CHATTER_USERNAME_LEN];
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_fg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_bg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char os_name[SSH_CHATTER_OS_NAME_LEN];
    int32_t daily_year;
    int32_t daily_yday;
    char daily_function[64];
    uint64_t last_poll_id;
    int32_t last_poll_choice;
    uint8_t has_birthday;
    uint8_t reserved[3];
    char birthday[16];
} host_state_preference_entry_v5_t;

typedef struct host_state_preference_entry_v6 {
    uint8_t has_user_theme;
    uint8_t has_system_theme;
    uint8_t user_is_bold;
    uint8_t system_is_bold;
    char username[SSH_CHATTER_USERNAME_LEN];
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_fg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_bg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char os_name[SSH_CHATTER_OS_NAME_LEN];
    int32_t daily_year;
    int32_t daily_yday;
    char daily_function[64];
    uint64_t last_poll_id;
    int32_t last_poll_choice;
    uint8_t has_birthday;
    uint8_t translation_caption_spacing;
    uint8_t translation_enabled;
    uint8_t output_translation_enabled;
    uint8_t input_translation_enabled;
    uint8_t reserved[3];
    char birthday[16];
    char output_translation_language[SSH_CHATTER_LANG_NAME_LEN];
    char input_translation_language[SSH_CHATTER_LANG_NAME_LEN];
} host_state_preference_entry_v6_t;

typedef struct host_state_preference_entry_v7 {
    uint8_t has_user_theme;
    uint8_t has_system_theme;
    uint8_t user_is_bold;
    uint8_t system_is_bold;
    char username[SSH_CHATTER_USERNAME_LEN];
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_fg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_bg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char os_name[SSH_CHATTER_OS_NAME_LEN];
    int32_t daily_year;
    int32_t daily_yday;
    char daily_function[64];
    uint64_t last_poll_id;
    int32_t last_poll_choice;
    uint8_t has_birthday;
    uint8_t translation_caption_spacing;
    uint8_t translation_enabled;
    uint8_t output_translation_enabled;
    uint8_t input_translation_enabled;
    uint8_t translation_master_explicit;
    uint8_t reserved[2];
    char birthday[16];
    char output_translation_language[SSH_CHATTER_LANG_NAME_LEN];
    char input_translation_language[SSH_CHATTER_LANG_NAME_LEN];
} host_state_preference_entry_v7_t;

typedef struct host_state_preference_entry_v8 {
    uint8_t has_user_theme;
    uint8_t has_system_theme;
    uint8_t user_is_bold;
    uint8_t system_is_bold;
    char username[SSH_CHATTER_USERNAME_LEN];
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_fg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_bg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char os_name[SSH_CHATTER_OS_NAME_LEN];
    int32_t daily_year;
    int32_t daily_yday;
    char daily_function[64];
    uint64_t last_poll_id;
    int32_t last_poll_choice;
    uint8_t has_birthday;
    uint8_t translation_caption_spacing;
    uint8_t translation_enabled;
    uint8_t output_translation_enabled;
    uint8_t input_translation_enabled;
    uint8_t translation_master_explicit;
    uint8_t reserved[2];
    char birthday[16];
    char output_translation_language[SSH_CHATTER_LANG_NAME_LEN];
    char input_translation_language[SSH_CHATTER_LANG_NAME_LEN];
    char ui_language[SSH_CHATTER_LANG_NAME_LEN];
} host_state_preference_entry_v8_t;

typedef struct host_state_preference_entry_v9 {
    uint8_t has_user_theme;
    uint8_t has_system_theme;
    uint8_t user_is_bold;
    uint8_t system_is_bold;
    char username[SSH_CHATTER_USERNAME_LEN];
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_fg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_bg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char os_name[SSH_CHATTER_OS_NAME_LEN];
    int32_t daily_year;
    int32_t daily_yday;
    char daily_function[64];
    uint64_t last_poll_id;
    int32_t last_poll_choice;
    uint8_t has_birthday;
    uint8_t translation_caption_spacing;
    uint8_t translation_enabled;
    uint8_t output_translation_enabled;
    uint8_t input_translation_enabled;
    uint8_t translation_master_explicit;
    uint8_t reserved[2];
    char birthday[16];
    char output_translation_language[SSH_CHATTER_LANG_NAME_LEN];
    char input_translation_language[SSH_CHATTER_LANG_NAME_LEN];
    char ui_language[SSH_CHATTER_LANG_NAME_LEN];
    uint8_t breaking_alerts_enabled;
    uint8_t reserved2[7];
} host_state_preference_entry_v9_t;

typedef struct host_state_preference_entry {
    uint8_t has_user_theme;
    uint8_t has_system_theme;
    uint8_t user_is_bold;
    uint8_t system_is_bold;
    char username[SSH_CHATTER_USERNAME_LEN];
    char ip[SSH_CHATTER_IP_LEN];
    char user_color_code[SSH_CHATTER_COLOR_CODE_LEN];
    char user_highlight_code[SSH_CHATTER_COLOR_CODE_LEN];
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_fg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_bg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char os_name[SSH_CHATTER_OS_NAME_LEN];
    int32_t daily_year;
    int32_t daily_yday;
    char daily_function[64];
    uint64_t last_poll_id;
    int32_t last_poll_choice;
    uint8_t has_birthday;
    uint8_t translation_caption_spacing;
    uint8_t translation_enabled;
    uint8_t output_translation_enabled;
    uint8_t input_translation_enabled;
    uint8_t translation_master_explicit;
    uint8_t reserved[2];
    char birthday[16];
    char output_translation_language[SSH_CHATTER_LANG_NAME_LEN];
    char input_translation_language[SSH_CHATTER_LANG_NAME_LEN];
    char ui_language[SSH_CHATTER_LANG_NAME_LEN];
    uint8_t breaking_alerts_enabled;
    char provider_label[SSH_CHATTER_PROVIDER_LABEL_LEN];
    uint8_t reserved2[7];
} host_state_preference_entry_t;

static const uint32_t BAN_STATE_MAGIC = 0x5348424eU; /* 'SHBN' */
static const uint32_t BAN_STATE_VERSION = 1U;

typedef struct ban_state_header {
    uint32_t magic;
    uint32_t version;
    uint32_t entry_count;
} ban_state_header_t;

typedef struct ban_state_entry {
    char username[SSH_CHATTER_USERNAME_LEN];
    char ip[SSH_CHATTER_IP_LEN];
} ban_state_entry_t;

static const uint32_t REPLY_STATE_MAGIC = 0x53485250U; /* 'SHRP' */
static const uint32_t REPLY_STATE_VERSION = 1U;

typedef struct reply_state_header {
    uint32_t magic;
    uint32_t version;
    uint32_t entry_count;
    uint64_t next_reply_id;
} reply_state_header_t;

typedef struct reply_state_entry {
    uint64_t reply_id;
    uint64_t parent_message_id;
    uint64_t parent_reply_id;
    int64_t created_at;
    char username[SSH_CHATTER_USERNAME_LEN];
    char message[SSH_CHATTER_MESSAGE_LIMIT];
} reply_state_entry_t;

typedef struct host_state_grant_entry {
    char ip[SSH_CHATTER_IP_LEN];
} host_state_grant_entry_t;

static const uint32_t BBS_STATE_MAGIC = 0x42425331U; /* 'BBS1' */
static const uint32_t BBS_STATE_VERSION = 4U;

#define SSH_CHATTER_BBS_TITLE_LEN_V1 96U
#define SSH_CHATTER_BBS_BODY_LEN_V1 2048U
#define SSH_CHATTER_BBS_BODY_LEN_V2 10240U
#define SSH_CHATTER_BBS_BODY_LEN_V3 20480U

typedef struct bbs_state_header {
    uint32_t magic;
    uint32_t version;
    uint32_t post_count;
    uint32_t reserved;
    uint64_t next_id;
} bbs_state_header_t;

typedef struct bbs_state_comment_entry {
    char author[SSH_CHATTER_USERNAME_LEN];
    char text[SSH_CHATTER_BBS_COMMENT_LEN];
    int64_t created_at;
} bbs_state_comment_entry_t;

typedef struct bbs_state_post_entry {
    uint64_t id;
    int64_t created_at;
    int64_t bumped_at;
    uint32_t tag_count;
    uint32_t comment_count;
    char author[SSH_CHATTER_USERNAME_LEN];
    char title[SSH_CHATTER_BBS_TITLE_LEN];
    char body[SSH_CHATTER_BBS_BODY_LEN];
    char tags[SSH_CHATTER_BBS_MAX_TAGS][SSH_CHATTER_BBS_TAG_LEN];
    bbs_state_comment_entry_t comments[SSH_CHATTER_BBS_MAX_COMMENTS];
} bbs_state_post_entry_t;

typedef struct bbs_state_post_entry_v1 {
    uint64_t id;
    int64_t created_at;
    int64_t bumped_at;
    uint32_t tag_count;
    uint32_t comment_count;
    char author[SSH_CHATTER_USERNAME_LEN];
    char title[SSH_CHATTER_BBS_TITLE_LEN_V1];
    char body[SSH_CHATTER_BBS_BODY_LEN_V1];
    char tags[SSH_CHATTER_BBS_MAX_TAGS][SSH_CHATTER_BBS_TAG_LEN];
    bbs_state_comment_entry_t comments[SSH_CHATTER_BBS_MAX_COMMENTS];
} bbs_state_post_entry_v1_t;

typedef struct bbs_state_post_entry_v2 {
    uint64_t id;
    int64_t created_at;
    int64_t bumped_at;
    uint32_t tag_count;
    uint32_t comment_count;
    char author[SSH_CHATTER_USERNAME_LEN];
    char title[SSH_CHATTER_BBS_TITLE_LEN];
    char body[SSH_CHATTER_BBS_BODY_LEN_V2];
    char tags[SSH_CHATTER_BBS_MAX_TAGS][SSH_CHATTER_BBS_TAG_LEN];
    bbs_state_comment_entry_t comments[SSH_CHATTER_BBS_MAX_COMMENTS];
} bbs_state_post_entry_v2_t;

typedef struct bbs_state_post_entry_v3 {
    uint64_t id;
    int64_t created_at;
    int64_t bumped_at;
    uint32_t tag_count;
    uint32_t comment_count;
    char author[SSH_CHATTER_USERNAME_LEN];
    char title[SSH_CHATTER_BBS_TITLE_LEN];
    char body[SSH_CHATTER_BBS_BODY_LEN_V3];
    char tags[SSH_CHATTER_BBS_MAX_TAGS][SSH_CHATTER_BBS_TAG_LEN];
    bbs_state_comment_entry_t comments[SSH_CHATTER_BBS_MAX_COMMENTS];
} bbs_state_post_entry_v3_t;

static const uint32_t ALPHA_LANDERS_STATE_MAGIC = 0x464C4147U; /* 'FLAG' */
static const uint32_t ALPHA_LANDERS_STATE_VERSION = 1U;

typedef struct alpha_landers_file_header {
    uint32_t magic;
    uint32_t version;
    uint32_t entry_count;
    uint32_t reserved;
} alpha_landers_file_header_t;

typedef struct alpha_landers_file_entry {
    char username[SSH_CHATTER_USERNAME_LEN];
    uint32_t flag_count;
    uint64_t last_flag_timestamp;
    uint32_t reserved;
} alpha_landers_file_entry_t;

static const uint32_t VOTE_STATE_MAGIC = 0x564F5445U; /* 'VOTE' */
static const uint32_t VOTE_STATE_VERSION = 1U;

typedef struct vote_state_header {
    uint32_t magic;
    uint32_t version;
    uint32_t named_count;
    uint32_t reserved;
} vote_state_header_t;

typedef struct vote_state_poll_option_entry {
    char text[SSH_CHATTER_MESSAGE_LIMIT];
    uint32_t votes;
} vote_state_poll_option_entry_t;

typedef struct vote_state_poll_entry {
    uint8_t active;
    uint8_t allow_multiple;
    uint8_t reserved[6];
    uint64_t id;
    uint32_t option_count;
    uint32_t reserved2;
    char question[SSH_CHATTER_MESSAGE_LIMIT];
    vote_state_poll_option_entry_t options[5];
} vote_state_poll_entry_t;

typedef struct vote_state_named_voter_entry {
    char username[SSH_CHATTER_USERNAME_LEN];
    int32_t choice;
    uint32_t choices_mask;
} vote_state_named_voter_entry_t;

typedef struct vote_state_named_entry {
    vote_state_poll_entry_t poll;
    char label[SSH_CHATTER_POLL_LABEL_LEN];
    char owner[SSH_CHATTER_USERNAME_LEN];
    uint32_t voter_count;
    uint32_t reserved;
    vote_state_named_voter_entry_t voters[SSH_CHATTER_MAX_NAMED_VOTERS];
} vote_state_named_entry_t;

typedef struct reaction_descriptor {
    const char *command;
    const char *label;
    const char *icon;
} reaction_descriptor_t;

static const reaction_descriptor_t
    REACTION_DEFINITIONS[SSH_CHATTER_REACTION_KIND_COUNT] = {
        {"good", "good", "b"},         {"sad", "sad", ":("},
        {"cool", "cool", "(ツ)!"},     {"angry", "angry", ":/"},
        {"checked", "checked", "[v]"}, {"love", "love", "<3"},
        {"wtf", "wtf", "凸_(ツ)"},
};

typedef struct os_descriptor {
    const char *name;
    const char *display;
} os_descriptor_t;

static const os_descriptor_t OS_CATALOG[] = {{"windows", "Windows"},
                                             {"macos", "macOS"},
                                             {"linux", "Linux"},
                                             {"freebsd", "FreeBSD"},
                                             {"ios", "iOS"},
                                             {"android", "Android"},
                                             {"watchos", "watchOS"},
                                             {"solaris", "Solaris"},
                                             {"openbsd", "OpenBSD"},
                                             {"netbsd", "NetBSD"},
                                             {"dragonflybsd", "DragonFlyBSD"},
                                             {"reactos", "ReactOS"},
                                             {"tizen", "Tizen"},
                                             {"bsd", "BSD"},
                                             {"msdos", "MS-DOS"},
                                             {"drdos", "DR-DOS"},
                                             {"kdos", "K-DOS"},
                                             {"templeos", "TempleOS"},
                                             {"zealos", "ZealOS"},
                                             {"haiku", "Haiku"},
                                             {"pcdos", "PC-DOS"}};

static const os_descriptor_t *session_lookup_os_descriptor(const char *name);

// random pool for daily functions
static const char *DAILY_FUNCTIONS[] __attribute__((unused)) = {
    "sin",     "cos",    "tan",    "sqrt",           "log",
    "exp",     "printf", "malloc", "free",           "memcpy",
    "strncpy", "qsort",  "fopen",  "close",          "select",
    "poll",    "fork",   "exec",   "pthread_create", "strtok"};

static bool chat_room_ensure_capacity(chat_room_t *room, size_t required)
{ // chat members should be within capacity
    if (room == nullptr) {
        return false;
    }

    if (required <= room->member_capacity) {
        return true;
    }

    size_t new_capacity =
        room->member_capacity == 0U ? 8U : room->member_capacity;
    while (new_capacity < required) {
        if (new_capacity > SIZE_MAX / 2U) {
            new_capacity = required;
            break;
        }
        new_capacity *= 2U;
    }

    session_ctx_t **resized =
        sshc_gc_realloc(room->members, new_capacity * sizeof(*resized));
    if (resized == nullptr) {
        return false;
    }

    for (size_t idx = room->member_capacity; idx < new_capacity; ++idx) {
        resized[idx] = nullptr;
    }

    room->members = resized;
    room->member_capacity = new_capacity;
    return true;
}

static void chat_room_init(chat_room_t *room)
{
    if (room == nullptr) {
        return;
    }
    ttak_mutex_init(&room->lock);
    room->members = nullptr;
    room->member_count = 0U;
    room->member_capacity = 0U;
}

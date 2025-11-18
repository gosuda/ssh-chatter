#ifndef SSH_CHATTER_HOST_H
#define SSH_CHATTER_HOST_H

#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/types.h>

#include "memory_manager.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#include <libssh/libssh.h>
#include <libssh/server.h>

#include "theme.h"
#include "security_layer.h"

#define HOST_GRANTS_CLEAR_SIZE 4512

#define SSH_CHATTER_SOUND_URL_LEN 1024
#define SSH_CHATTER_MESSAGE_LIMIT 4096
#define SSH_CHATTER_MOTD_MAX_NOTIFICATION_LEN 16384
#define SSH_CHATTER_BANNER_MAX_LEN SSH_CHATTER_MOTD_MAX_NOTIFICATION_LEN * 4
#define SSH_CHATTER_MAX_INPUT_LEN 1024
#define SSH_CHATTER_USERNAME_LEN 24
#define SSH_CHATTER_IP_LEN 64
#define SSH_CHATTER_COLOR_NAME_LEN 32
#define ALPHA_GRAVITY_NAME_LEN 32
#define ALPHA_MAX_GRAVITY_SOURCES 16
#define ALPHA_MAX_WAYPOINTS 4U
#define SSH_CHATTER_MAX_BANS 16384
#define SSH_CHATTER_HISTORY_LIMIT 64
#define SSH_CHATTER_HISTORY_CACHE_LIMIT 512
#define SSH_CHATTER_INPUT_HISTORY_LIMIT 64
#define SSH_CHATTER_SCROLLBACK_CHUNK 100
#define SSH_CHATTER_MAX_PREFERENCES 1024
#define SSH_CHATTER_ATTACHMENT_TARGET_LEN 256
#define SSH_CHATTER_ATTACHMENT_CAPTION_LEN 256
#define SSH_CHATTER_REACTION_KIND_COUNT 7
#define SSH_CHATTER_MAX_LAN_OPERATORS 5
#define SSH_CHATTER_LAN_PASSWORD_LEN 128
#define SSH_CHATTER_OS_NAME_LEN 16
#define SSH_CHATTER_POLL_LABEL_LEN 32
#define SSH_CHATTER_MAX_NAMED_POLLS 16
#define SSH_CHATTER_MAX_NAMED_VOTERS 256
#define SSH_CHATTER_BBS_MAX_POSTS 128
#define SSH_CHATTER_BBS_TITLE_LEN 512
#define SSH_CHATTER_BBS_BODY_LEN 40960
#define SSH_CHATTER_BBS_TAG_LEN 24
#define SSH_CHATTER_BBS_MAX_TAGS 4
#define SSH_CHATTER_BBS_MAX_COMMENTS 64
#define SSH_CHATTER_BBS_COMMENT_LEN 512
#define SSH_CHATTER_BBS_VIEW_WINDOW 60
#define SSH_CHATTER_BBS_BREAKING_MAX 4
#define SSH_CHATTER_RSS_MAX_FEEDS 32
#define SSH_CHATTER_RSS_TAG_LEN 32
#define SSH_CHATTER_RSS_URL_LEN 512
#define SSH_CHATTER_RSS_ITEM_KEY_LEN 512
#define SSH_CHATTER_RSS_TITLE_LEN 256
#define SSH_CHATTER_RSS_LINK_LEN 512
#define SSH_CHATTER_RSS_SUMMARY_LEN 32768
#define SSH_CHATTER_RSS_MAX_ITEMS 32
#define SSH_CHATTER_MAX_GRANTS 128
#define SSH_CHATTER_MAX_BLOCKED 64
#define SSH_CHATTER_JOIN_BAR_MAX 17
#define SSH_CHATTER_LANG_NAME_LEN 64
#define SSH_CHATTER_STATUS_LEN 128
#define SSH_CHATTER_CLIENT_BANNER_LEN 128
#define SSH_CHATTER_TERMINAL_TYPE_LEN 64
#define SSH_CHATTER_BBS_MAX_LINES 1000
#define SSH_CHATTER_ASCIIART_MAX_LINES 700
#define SSH_CHATTER_ASCIIART_BUFFER_LEN SSH_CHATTER_BBS_BODY_LEN
#define SSH_CHATTER_ASCIIART_COOLDOWN_SECONDS 600
#define SSH_CHATTER_ELIZA_MEMORY_MAX 128
#define SSH_CHATTER_TETRIS_WIDTH 15
#define SSH_CHATTER_TETRIS_HEIGHT 20
#define SSH_CHATTER_TETRIS_GRAVITY_THRESHOLD 5U
#define SSH_CHATTER_TETRIS_GRAVITY_RATE 1U
#define SSH_CHATTER_TETRIS_GRAVITY_INTERVAL_NS 100000000ULL
#define SSH_CHATTER_TETRIS_LINES_PER_ROUND 10U
#define SSH_CHATTER_TETRIS_MAX_ROUNDS 3U
#define SSH_CHATTER_MAX_REPLIES 1024
#define SSH_CHATTER_MAX_VERSION_IP_BANS 8192
#define SSH_CHATTER_VERSION_PATTERN_LEN 16384
#define SSH_CHATTER_VERSION_NOTE_LEN 96
#define SSH_CHATTER_CIDR_TEXT_LEN 64
#define SSH_CHATTER_MAX_PROTECTED_IPS 16
#define SSH_CHATTER_MAX_RESERVED_NAMES 16384
#define SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE 4096
#define SSH_CHATTER_OTHELLO_BOARD_SIZE 8
#define SSH_CHATTER_OTHELLO_MAX_MOVES \
    (SSH_CHATTER_OTHELLO_BOARD_SIZE * SSH_CHATTER_OTHELLO_BOARD_SIZE)
#define SSH_CHATTER_OTHELLO_MAX_SLOTS 1024
#define SSH_CHATTER_GONU_MAX_SLOTS 1024
#define SSH_CHATTER_OUTPUT_BUFFER_SIZE 65536

#include "user_data.h"

struct host;
struct session_ctx;
struct client_manager;
struct webssh_client;
struct matrix_client;
struct irc_client;
struct fidonet_client;
struct translation_job;
struct translation_result;

struct session_ctx;

typedef struct session_ops {
    void (*dispatch_command)(struct session_ctx *ctx, const char *line);
    void (*handle_mode)(struct session_ctx *ctx, const char *arguments);
    void (*handle_nick)(struct session_ctx *ctx, const char *arguments);
    void (*handle_exit)(struct session_ctx *ctx);
} session_ops_t;

typedef struct join_activity_entry {
    char ip[SSH_CHATTER_IP_LEN];
    char last_username[SSH_CHATTER_USERNAME_LEN];
    struct timespec last_attempt;
    size_t rapid_attempts;
    size_t same_name_attempts;
    struct timespec join_window_start;
    size_t join_window_attempts;
    struct timespec last_suspicious;
    size_t suspicious_events;
    bool asciiart_has_cooldown;
    struct timespec last_asciiart_post;
} join_activity_entry_t;

typedef struct connection_guard_entry {
    char ip[SSH_CHATTER_IP_LEN];
    struct timespec window_start;
    size_t attempts;
    struct timespec blocked_until;
    unsigned int block_count;
    struct timespec last_seen;
} connection_guard_entry_t;

typedef struct client_manager client_manager_t;
typedef struct webssh_client webssh_client_t;

typedef struct chat_user {
    char name[SSH_CHATTER_USERNAME_LEN];
    bool is_operator;
    bool is_lan_operator;
    bool is_authenticated;
} chat_user_t;

typedef struct lan_operator_credential {
    bool active;
    char nickname[SSH_CHATTER_USERNAME_LEN];
    char password[SSH_CHATTER_LAN_PASSWORD_LEN];
} lan_operator_credential_t;

typedef struct chat_room {
    pthread_mutex_t lock;
    struct session_ctx **members;
    size_t member_count;
    size_t member_capacity;
} chat_room_t;

typedef struct host_moderation_task host_moderation_task_t;

typedef struct host_moderation_state {
    bool active;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool mutex_initialized;
    bool cond_initialized;
    bool thread_started;
    bool stop;
    pthread_t thread;
    host_moderation_task_t *head;
    host_moderation_task_t *tail;
    uint64_t next_task_id;
    int request_fd;
    int response_fd;
    pid_t worker_pid;
    unsigned int restart_attempts;
    struct timespec worker_start_time;
} host_moderation_state_t;

typedef struct host_eliza_intervene_task host_eliza_intervene_task_t;

typedef struct host_eliza_worker_state {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool mutex_initialized;
    bool cond_initialized;
    bool thread_started;
    _Atomic bool stop;
    _Atomic bool active;
    pthread_t thread;
    host_eliza_intervene_task_t *head;
    host_eliza_intervene_task_t *tail;
} host_eliza_worker_state_t;

typedef enum chat_attachment_type {
    CHAT_ATTACHMENT_NONE = 0,
    CHAT_ATTACHMENT_IMAGE,
    CHAT_ATTACHMENT_VIDEO,
    CHAT_ATTACHMENT_AUDIO,
    CHAT_ATTACHMENT_FILE,
} chat_attachment_type_t;

typedef struct chat_history_entry {
    bool is_user_message;
    bool preserve_whitespace;
    char message[SSH_CHATTER_MESSAGE_LIMIT];
    char username[SSH_CHATTER_USERNAME_LEN];
    const char *user_color_code;
    const char *user_highlight_code;
    bool user_is_bold;
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    uint64_t message_id;
    chat_attachment_type_t attachment_type;
    char attachment_target[SSH_CHATTER_ATTACHMENT_TARGET_LEN];
    char attachment_caption[SSH_CHATTER_ATTACHMENT_CAPTION_LEN];
    time_t created_at;
    uint32_t reaction_counts[SSH_CHATTER_REACTION_KIND_COUNT];
} chat_history_entry_t;

typedef struct chat_reply_entry {
    bool in_use;
    uint64_t reply_id;
    uint64_t parent_message_id;
    uint64_t parent_reply_id;
    time_t created_at;
    char username[SSH_CHATTER_USERNAME_LEN];
    char message[SSH_CHATTER_MESSAGE_LIMIT];
} chat_reply_entry_t;

typedef struct eliza_memory_entry {
    uint64_t id;
    time_t stored_at;
    char prompt[SSH_CHATTER_MESSAGE_LIMIT];
    char reply[SSH_CHATTER_MESSAGE_LIMIT];
} eliza_memory_entry_t;

typedef enum version_pattern_match {
    VERSION_PATTERN_MATCH_ANY = 0,
    VERSION_PATTERN_MATCH_EXACT,
    VERSION_PATTERN_MATCH_PREFIX,
    VERSION_PATTERN_MATCH_SUFFIX,
    VERSION_PATTERN_MATCH_SUBSTRING,
} version_pattern_match_t;

typedef struct version_ip_ban_rule {
    bool in_use;
    version_pattern_match_t match_mode;
    char original_pattern[SSH_CHATTER_VERSION_PATTERN_LEN];
    char normalized_pattern[SSH_CHATTER_VERSION_PATTERN_LEN];
    char cidr_text[SSH_CHATTER_CIDR_TEXT_LEN];
    char note[SSH_CHATTER_VERSION_NOTE_LEN];
    bool is_ipv6;
    uint32_t ipv4_network;
    uint32_t ipv4_mask;
    struct in6_addr ipv6_network;
    struct in6_addr ipv6_mask;
} version_ip_ban_rule_t;

typedef struct session_block_entry {
    bool in_use;
    char ip[SSH_CHATTER_IP_LEN];
    char username[SSH_CHATTER_USERNAME_LEN];
    bool ip_wide;
} session_block_entry_t;

typedef struct session_block_prompt {
    bool active;
    char username[SSH_CHATTER_USERNAME_LEN];
    char ip[SSH_CHATTER_IP_LEN];
    char provider_label[32];
} session_block_prompt_t;

typedef struct auth_profile {
    bool is_banned;
    bool is_operator;
    bool is_observer;
} auth_profile_t;

typedef struct ssh_listener {
    ssh_bind handle;
    unsigned int inplace_recoveries;
    unsigned int restart_attempts;
    struct timespec last_error_time;
} ssh_listener_t;

typedef enum session_game_type {
    SESSION_GAME_NONE = 0,
    SESSION_GAME_TETRIS,
    SESSION_GAME_LIARGAME,
    SESSION_GAME_ALPHA,
    SESSION_GAME_OTHELLO,
    SESSION_GAME_GONU,
} session_game_type_t;

typedef struct tetris_game_state {
    int board[SSH_CHATTER_TETRIS_HEIGHT][SSH_CHATTER_TETRIS_WIDTH];
    int current_piece;
    int rotation;
    int row;
    int column;
    int next_piece;
    unsigned score;
    unsigned lines_cleared;
    bool game_over;
    int bag[7];
    size_t bag_index;
    unsigned gravity_counter;
    unsigned gravity_threshold;
    unsigned gravity_rate;
    bool gravity_timer_initialized;
    struct timespec gravity_timer_last;
    uint64_t gravity_timer_accumulator_ns;
    unsigned round;
    unsigned next_round_line_goal;
    bool input_escape_active;
    char input_escape_buffer[8];
    size_t input_escape_length;
} tetris_game_state_t;

typedef struct liar_game_state {
    unsigned round_number;
    unsigned score;
    unsigned current_prompt_index;
    unsigned liar_index;
    bool awaiting_guess;
} liar_game_state_t;

typedef enum othello_cell_type {
    OTHELLO_CELL_EMPTY = 0,
    OTHELLO_CELL_RED,
    OTHELLO_CELL_GREEN,
} othello_cell_type_t;

typedef struct othello_game_state {
    uint8_t board[SSH_CHATTER_OTHELLO_BOARD_SIZE]
                 [SSH_CHATTER_OTHELLO_BOARD_SIZE];
    bool player_turn;
    bool game_over;
    unsigned consecutive_passes;
    unsigned red_score;
    unsigned green_score;
    int last_player_row;
    int last_player_col;
    int last_ai_row;
    int last_ai_col;
    bool awaiting_mode_selection;
    bool awaiting_difficulty_selection;
    unsigned difficulty_level;  // 1-5, where 5 is hardest
    bool multiplayer;
    bool awaiting_opponent;
    int slot_index;
    unsigned player_number;
} othello_game_state_t;

typedef struct alpha_gravity_source {
    int x;
    int y;
    int influence_radius;
    double mu;
    char symbol;
    char name[ALPHA_GRAVITY_NAME_LEN];
} alpha_gravity_source_t;

typedef struct alpha_waypoint {
    int x;
    int y;
    char symbol;
    bool visited;
    char name[ALPHA_GRAVITY_NAME_LEN];
} alpha_waypoint_t;

typedef struct alpha_centauri_game_state {
    bool active;
    unsigned stage;
    bool eva_ready;
    bool awaiting_flag;
    double velocity_fraction_c;
    double distance_travelled_ly;
    double distance_remaining_ly;
    double fuel_percent;
    double oxygen_days;
    double mission_time_years;
    double radiation_msv;
    int nav_x;
    int nav_y;
    double nav_fx;
    double nav_fy;
    double nav_vx;
    double nav_vy;
    int nav_target_x;
    int nav_target_y;
    unsigned nav_stable_ticks;
    unsigned nav_required_ticks;
    unsigned gravity_source_count;
    alpha_gravity_source_t gravity_sources[ALPHA_MAX_GRAVITY_SOURCES];
    unsigned waypoint_count;
    unsigned waypoint_index;
    alpha_waypoint_t waypoints[ALPHA_MAX_WAYPOINTS];
    alpha_waypoint_t final_waypoint;
} alpha_centauri_game_state_t;

typedef enum gonu_variant {
    GONU_VARIANT_HOBAK = 0,  // 호박고누 - Pumpkin Gonu (3x3 grid)
    GONU_VARIANT_BAKWI,      // 바퀴고누 - Wheel Gonu (circular pattern)
    GONU_VARIANT_UMUL,       // 우물고누 - Well Gonu (井字 pattern)
} gonu_variant_t;

typedef enum gonu_cell {
    GONU_CELL_EMPTY = 0,
    GONU_CELL_PLAYER,
    GONU_CELL_AI,
} gonu_cell_t;

#define GONU_BOARD_SIZE 5

typedef struct gonu_game_state {
    gonu_variant_t variant;
    uint8_t board[GONU_BOARD_SIZE][GONU_BOARD_SIZE];
    bool player_turn;
    bool game_over;
    bool placement_phase;    // True during initial piece placement
    unsigned player_pieces;
    unsigned ai_pieces;
    int selected_row;
    int selected_col;
    bool piece_selected;
    bool awaiting_variant_selection;
    bool awaiting_mode_selection;
    bool awaiting_difficulty_selection;
    unsigned difficulty_level;  // 1-5, where 5 is hardest
    bool multiplayer;
    bool awaiting_opponent;
    int slot_index;
    unsigned player_number;
} gonu_game_state_t;

typedef struct session_game_state {
    bool active;
    session_game_type_t type;
    tetris_game_state_t tetris;
    bool is_camouflaged;
    tetris_game_state_t saved_tetris_state;
    liar_game_state_t saved_liar_state;
    alpha_centauri_game_state_t saved_alpha_state;
    othello_game_state_t saved_othello_state;
    gonu_game_state_t saved_gonu_state;
    char chosen_camouflage_language[16];
    liar_game_state_t liar;
    alpha_centauri_game_state_t alpha;
    othello_game_state_t othello;
    gonu_game_state_t gonu;
    uint64_t rng_state;
    bool rng_seeded;
} session_game_state_t;

typedef struct othello_multiplayer_slot {
    bool in_use;
    bool active;
    bool awaiting_second_player;
    uint16_t slot_id;
    char owner[SSH_CHATTER_USERNAME_LEN];
    othello_game_state_t state;
    struct session_ctx *players[2];
} othello_multiplayer_slot_t;

typedef struct gonu_multiplayer_slot {
    bool in_use;
    bool active;
    bool awaiting_second_player;
    uint16_t slot_id;
    char owner[SSH_CHATTER_USERNAME_LEN];
    gonu_game_state_t state;
    struct session_ctx *players[2];
} gonu_multiplayer_slot_t;

typedef enum session_transport_kind {
    SESSION_TRANSPORT_SSH = 0,
    SESSION_TRANSPORT_TELNET,
} session_transport_kind_t;

typedef enum session_input_mode {
    SESSION_INPUT_MODE_CHAT = 0,
    SESSION_INPUT_MODE_COMMAND,
} session_input_mode_t;

typedef enum session_ui_language {
    SESSION_UI_LANGUAGE_EN = 0,
    SESSION_UI_LANGUAGE_KO,
    SESSION_UI_LANGUAGE_JP,
    SESSION_UI_LANGUAGE_ZH,
    SESSION_UI_LANGUAGE_RU,
    SESSION_UI_LANGUAGE_DE,
    SESSION_UI_LANGUAGE_FR,
    SESSION_UI_LANGUAGE_PL,
    SESSION_UI_LANGUAGE_COUNT
} session_ui_language_t;

typedef enum session_asciiart_target {
    SESSION_ASCIIART_TARGET_NONE = 0,
    SESSION_ASCIIART_TARGET_CHAT,
    SESSION_ASCIIART_TARGET_PROFILE_PICTURE,
} session_asciiart_target_t;

typedef enum session_editor_mode {
    SESSION_EDITOR_MODE_NONE = 0,
    SESSION_EDITOR_MODE_BBS_CREATE,
    SESSION_EDITOR_MODE_BBS_EDIT,
    SESSION_EDITOR_MODE_ASCIIART,
} session_editor_mode_t;

typedef struct rss_feed {
    bool in_use;
    char tag[SSH_CHATTER_RSS_TAG_LEN];
    char url[SSH_CHATTER_RSS_URL_LEN];
    char last_item_key[SSH_CHATTER_RSS_ITEM_KEY_LEN];
    char last_title[SSH_CHATTER_RSS_TITLE_LEN];
    char last_link[SSH_CHATTER_RSS_LINK_LEN];
    time_t last_checked;
} rss_feed_t;

typedef struct rss_session_item {
    char id[SSH_CHATTER_RSS_ITEM_KEY_LEN];
    char title[SSH_CHATTER_RSS_TITLE_LEN];
    char link[SSH_CHATTER_RSS_LINK_LEN];
    char summary[SSH_CHATTER_RSS_SUMMARY_LEN];
} rss_session_item_t;

typedef struct session_rss_view {
    bool active;
    char tag[SSH_CHATTER_RSS_TAG_LEN];
    size_t item_count;
    size_t cursor;
    rss_session_item_t items[SSH_CHATTER_RSS_MAX_ITEMS];
} session_rss_view_t;

typedef enum session_cp437_override {
    SESSION_CP437_OVERRIDE_NONE = 0,
    SESSION_CP437_OVERRIDE_FORCE_OFF,
    SESSION_CP437_OVERRIDE_FORCE_ON,
} session_cp437_override_t;

typedef struct session_ctx {
    ssh_session session;
    ssh_channel channel;
    session_transport_kind_t transport_kind;
    int telnet_fd;
    bool telnet_negotiated;
    bool telnet_eof;
    bool telnet_pending_valid;
    int telnet_pending_char;
    bool telnet_consume_next_lf;
    bool telnet_terminal_type_requested;
    chat_user_t user;
    bool lan_operator_credentials_valid;
    auth_profile_t auth;
    struct host *owner;
    char input_buffer[SSH_CHATTER_MAX_INPUT_LEN];
    size_t input_length;
    char input_history[SSH_CHATTER_INPUT_HISTORY_LIMIT]
                      [SSH_CHATTER_MAX_INPUT_LEN];
    bool input_history_is_command[SSH_CHATTER_INPUT_HISTORY_LIMIT];
    size_t input_history_count;
    int input_history_position;
    session_input_mode_t input_mode;
    bool input_escape_active;
    char input_escape_buffer[8];
    size_t input_escape_length;
    bool bracket_paste_active;
    char client_ip[SSH_CHATTER_IP_LEN];
    char client_banner[SSH_CHATTER_CLIENT_BANNER_LEN];
    char terminal_type[SSH_CHATTER_TERMINAL_TYPE_LEN];
    char retro_client_marker[SSH_CHATTER_TERMINAL_TYPE_LEN];
    char telnet_identity[SSH_CHATTER_CLIENT_BANNER_LEN];
    const char *user_color_code;
    const char *user_highlight_code;
    bool user_is_bold;
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    const char *system_fg_code;
    const char *system_bg_code;
    const char *system_highlight_code;
    bool system_is_bold;
    char system_fg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_bg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    unsigned int terminal_width;
    unsigned int terminal_height;
    int exit_status;
    bool should_exit;
    bool username_conflict;
    bool has_joined_room;
    bool prelogin_banner_rendered;
    unsigned int channel_error_retries;
    size_t history_scroll_position;
    struct timespec last_message_time;
    bool has_last_message_time;
    size_t chat_message_count;
    char os_name[SSH_CHATTER_OS_NAME_LEN];
    int daily_year;
    int daily_yday;
    char daily_function[64];
    bool in_bbs_mode;
    bool has_birthday;
    char birthday[16];
    bool bbs_post_pending;
    session_editor_mode_t editor_mode;
    uint64_t pending_bbs_edit_id;
    char pending_bbs_title[SSH_CHATTER_BBS_TITLE_LEN];
    char pending_bbs_tags[SSH_CHATTER_BBS_MAX_TAGS][SSH_CHATTER_BBS_TAG_LEN];
    size_t pending_bbs_tag_count;
    char pending_bbs_body[SSH_CHATTER_BBS_BODY_LEN];
    size_t pending_bbs_body_length;
    size_t pending_bbs_line_count;
    size_t pending_bbs_cursor_line;
    bool pending_bbs_editing_line;
    bool bbs_view_active;
    uint64_t bbs_view_post_id;
    size_t bbs_view_scroll_offset;
    size_t bbs_view_total_lines;
    bool bbs_view_notice_pending;
    char bbs_view_notice[SSH_CHATTER_MESSAGE_LIMIT];
    bool bbs_rendering_editor;
    char bbs_breaking_messages[SSH_CHATTER_BBS_BREAKING_MAX]
                              [SSH_CHATTER_MESSAGE_LIMIT];
    size_t bbs_breaking_count;
    bool breaking_alerts_enabled;
    bool prefer_utf16_output;
    bool prefer_cp437_output;
    session_cp437_override_t cp437_override;
    bool cp437_input_enabled;
    bool translation_enabled;
    bool output_translation_enabled;
    char output_translation_language[SSH_CHATTER_LANG_NAME_LEN];
    bool input_translation_enabled;
    char input_translation_language[SSH_CHATTER_LANG_NAME_LEN];
    char last_detected_input_language[SSH_CHATTER_LANG_NAME_LEN];
    size_t translation_caption_spacing;
    size_t translation_placeholder_active_lines;
    bool translation_suppress_output;
    bool translation_manual_scope_override;
    bool translation_quota_notified;
    session_ui_language_t ui_language;
    pthread_mutex_t translation_mutex;
    pthread_mutex_t chat_message_count_mutex;
    pthread_mutex_t channel_mutex;
    pthread_cond_t translation_cond;
    pthread_mutex_t output_lock;
    bool translation_mutex_initialized;
    bool translation_cond_initialized;
    bool output_lock_initialized;
    bool channel_mutex_initialized;
    bool translation_thread_started;
    bool translation_thread_stop;
    pthread_t translation_thread;
    char reserved_nicknames[SSH_CHATTER_MAX_RESERVED_NAMES]
                           [SSH_CHATTER_USERNAME_LEN];
    size_t reserved_nicknames_len;
    pthread_mutex_t nickname_reserve_lock;
    struct translation_job *translation_pending_head;
    struct translation_job *translation_pending_tail;
    struct translation_result *translation_ready_head;
    struct translation_result *translation_ready_tail;
    char status_message[SSH_CHATTER_STATUS_LEN];
    bool asciiart_pending;
    session_asciiart_target_t asciiart_target;
    char asciiart_buffer[SSH_CHATTER_ASCIIART_BUFFER_LEN];
    size_t asciiart_length;
    size_t asciiart_line_count;
    bool asciiart_has_cooldown;
    struct timespec last_asciiart_post;
    session_game_state_t game;
    session_block_entry_t block_entries[SSH_CHATTER_MAX_BLOCKED];
    size_t block_entry_count;
    session_block_prompt_t block_pending;
    bool in_rss_mode;
    session_rss_view_t rss_view;
    bool user_data_loaded;
    user_data_record_t user_data;
    bool password_not_set; // Flag to indicate if user needs to set a password
    char tetris_screen_buffer[SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE];
    char tetris_prev_screen_buffer[SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE];
    const session_ops_t *ops;
    bool history_oldest_notified;
    bool history_latest_notified;
    bool no_update; // Flag to prevent automatic message updates when scrolling history
    // Output buffering to prevent flickering
    bool output_buffering_enabled;
    char output_buffer[SSH_CHATTER_OUTPUT_BUFFER_SIZE];
    size_t output_buffer_length;
} session_ctx_t;

typedef struct user_preference {
    bool in_use;
    bool has_user_theme;
    bool has_system_theme;
    char username[SSH_CHATTER_USERNAME_LEN];
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    bool user_is_bold;
    char system_fg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_bg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char system_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    bool system_is_bold;
    char os_name[SSH_CHATTER_OS_NAME_LEN];
    int daily_year;
    int daily_yday;
    char daily_function[64];
    uint64_t last_poll_id;
    int last_poll_choice;
    bool has_birthday;
    char birthday[16];
    uint8_t translation_caption_spacing;
    bool translation_master_enabled;
    bool translation_master_explicit;
    bool output_translation_enabled;
    bool input_translation_enabled;
    char output_translation_language[SSH_CHATTER_LANG_NAME_LEN];
    char input_translation_language[SSH_CHATTER_LANG_NAME_LEN];
    char ui_language[SSH_CHATTER_LANG_NAME_LEN];
    bool breaking_alerts_enabled;
    char camouflage_language[16];
    bool show_continuous_messages;
    struct {
        char label[SSH_CHATTER_POLL_LABEL_LEN];
        uint64_t poll_id;
        int choice;
    } named_votes[SSH_CHATTER_MAX_NAMED_POLLS];
} user_preference_t;

typedef struct poll_option {
    char text[SSH_CHATTER_MESSAGE_LIMIT];
    uint32_t votes;
} poll_option_t;

typedef struct poll_state {
    bool active;
    uint64_t id;
    char question[SSH_CHATTER_MESSAGE_LIMIT];
    size_t option_count;
    poll_option_t options[5];
    bool allow_multiple;
} poll_state_t;

typedef struct named_poll_state {
    poll_state_t poll;
    char label[SSH_CHATTER_POLL_LABEL_LEN];
    char owner[SSH_CHATTER_USERNAME_LEN];
    struct {
        char username[SSH_CHATTER_USERNAME_LEN];
        int choice;
        uint32_t choices_mask;
    } voters[SSH_CHATTER_MAX_NAMED_VOTERS];
    size_t voter_count;
} named_poll_state_t;

typedef struct bbs_comment {
    char author[SSH_CHATTER_USERNAME_LEN];
    char text[SSH_CHATTER_BBS_COMMENT_LEN];
    time_t created_at;
} bbs_comment_t;

typedef struct bbs_post {
    bool in_use;
    uint64_t id;
    char author[SSH_CHATTER_USERNAME_LEN];
    char title[SSH_CHATTER_BBS_TITLE_LEN];
    char body[SSH_CHATTER_BBS_BODY_LEN];
    char tags[SSH_CHATTER_BBS_MAX_TAGS][SSH_CHATTER_BBS_TAG_LEN];
    size_t tag_count;
    time_t created_at;
    time_t bumped_at;
    bbs_comment_t comments[SSH_CHATTER_BBS_MAX_COMMENTS];
    size_t comment_count;
} bbs_post_t;

typedef struct host {
    sshc_memory_context_t *memory_context;
    chat_room_t room;
    ssh_listener_t listener;
    struct {
        bool enabled;
        int fd;
        pthread_t thread;
        bool thread_initialized;
        _Atomic bool running;
        _Atomic bool stop;
        unsigned int restart_attempts;
        struct timespec last_error_time;
        char bind_address[64];
        char port[16];
    } telnet;
    auth_profile_t *auth;
    UserTheme user_theme;
    SystemTheme system_theme;
    char default_user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char default_user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char default_system_fg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char default_system_bg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char default_system_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    struct {
        char username[SSH_CHATTER_USERNAME_LEN];
        char ip[SSH_CHATTER_IP_LEN];
    } bans[SSH_CHATTER_MAX_BANS];
    size_t ban_count;
    char version[64];
    char motd[4096];
    char motd_base[4096];
    char motd_path[PATH_MAX];
    bool motd_has_file;
    struct timespec motd_last_modified;
    char welcome_banner[SSH_CHATTER_BANNER_MAX_LEN];
    bool welcome_banner_loaded;
    bool translation_quota_exhausted;
    size_t connection_count;
    chat_history_entry_t *history;
    size_t history_count;
    size_t history_capacity;
    size_t history_start_index;
    size_t history_total;
    chat_history_entry_t *history_override;
    size_t history_override_count;
    uint64_t next_message_id;
    chat_reply_entry_t replies[SSH_CHATTER_MAX_REPLIES];
    size_t reply_count;
    uint64_t next_reply_id;
    user_preference_t preferences[SSH_CHATTER_MAX_PREFERENCES];
    size_t preference_count;
    pthread_mutex_t lock;
    char state_file_path[PATH_MAX];
    char bbs_state_file_path[PATH_MAX];
    char vote_state_file_path[PATH_MAX];
    char ban_state_file_path[PATH_MAX];
    char reply_state_file_path[PATH_MAX];
    char pw_auth_file_path[PATH_MAX];
    char alpha_landers_file_path[PATH_MAX];
    char user_data_root[PATH_MAX];
    bool user_data_ready;
    pthread_mutex_t user_data_lock;
    bool user_data_lock_initialized;
    pthread_mutex_t alpha_landers_lock;
    bool alpha_landers_lock_initialized;
    _Atomic bool security_filter_enabled;
    _Atomic bool security_filter_failure_logged;
    _Atomic bool security_ai_enabled;
    _Atomic bool security_clamav_enabled;
    _Atomic bool security_clamav_failure_logged;
    char security_clamav_command[PATH_MAX];
    pthread_t security_clamav_thread;
    bool security_clamav_thread_initialized;
    _Atomic bool security_clamav_thread_running;
    _Atomic bool security_clamav_thread_stop;
    struct timespec security_clamav_last_run;
    host_moderation_state_t moderation;
    host_eliza_worker_state_t eliza_worker;
    pthread_t bbs_watchdog_thread;
    bool bbs_watchdog_thread_initialized;
    _Atomic bool bbs_watchdog_thread_running;
    _Atomic bool bbs_watchdog_thread_stop;
    struct timespec bbs_watchdog_last_run;
    poll_state_t poll;
    named_poll_state_t named_polls[SSH_CHATTER_MAX_NAMED_POLLS];
    size_t named_poll_count;
    bbs_post_t bbs_posts[SSH_CHATTER_BBS_MAX_POSTS];
    size_t bbs_post_count;
    uint64_t next_bbs_id;
    rss_feed_t rss_feeds[SSH_CHATTER_RSS_MAX_FEEDS];
    size_t rss_feed_count;
    othello_multiplayer_slot_t othello_games[SSH_CHATTER_OTHELLO_MAX_SLOTS];
    gonu_multiplayer_slot_t gonu_games[SSH_CHATTER_GONU_MAX_SLOTS];
    bool random_seeded;
    client_manager_t *clients;
    webssh_client_t *web_client;
    struct matrix_client *matrix_client;
    struct irc_client *irc_client;
    struct fidonet_client *fidonet_client;
    security_layer_t security_layer;
    bool security_layer_initialized;
    _Atomic bool eliza_enabled;
    _Atomic bool eliza_announced;
    struct timespec eliza_last_action;
    char eliza_state_file_path[PATH_MAX];
    char eliza_memory_file_path[PATH_MAX];
    char rss_state_file_path[PATH_MAX];
    eliza_memory_entry_t eliza_memory[SSH_CHATTER_ELIZA_MEMORY_MAX];
    size_t eliza_memory_count;
    uint64_t eliza_memory_next_id;
    struct {
        char ip[SSH_CHATTER_IP_LEN];
    } operator_grants[SSH_CHATTER_MAX_GRANTS];
    size_t operator_grant_count;
    char protected_ips[SSH_CHATTER_MAX_PROTECTED_IPS][SSH_CHATTER_IP_LEN];
    size_t protected_ip_count;
    version_ip_ban_rule_t version_ip_ban_rules[SSH_CHATTER_MAX_VERSION_IP_BANS];
    size_t version_ip_ban_rule_count;
    struct {
        lan_operator_credential_t entries[SSH_CHATTER_MAX_LAN_OPERATORS];
        size_t count;
    } lan_ops;
    struct timespec next_join_ready_time;
    bool join_throttle_initialised;
    size_t join_progress_length;
    join_activity_entry_t *join_activity;
    size_t join_activity_count;
    size_t join_activity_capacity;
    connection_guard_entry_t *connection_guard;
    size_t connection_guard_count;
    size_t connection_guard_capacity;
    struct {
        unsigned int consecutive_errors;
        struct timespec last_error_time;
    } health_guard;
    _Atomic bool captcha_enabled;
    uint64_t captcha_nonce;
    bool has_last_captcha;
    char last_captcha_question[1024];
    char last_captcha_answer[64];
    struct timespec last_captcha_generated;
    pthread_t rss_thread;
    bool rss_thread_initialized;
    _Atomic bool rss_thread_running;
    _Atomic bool rss_thread_stop;
    struct timespec rss_last_run;

    // Add members for managing reserved nicknames
    char reserved_nicknames[SSH_CHATTER_MAX_RESERVED_NAMES]
                           [SSH_CHATTER_USERNAME_LEN];
    size_t reserved_nicknames_len;
    pthread_mutex_t nickname_reserve_lock;
    volatile sig_atomic_t *shutdown_flag;
} host_t;

typedef struct {
    unsigned int code_point;
    int count;
} utf8_code_count_t;

bool session_telnet_login_prompt(session_ctx_t *ctx);
bool host_user_data_load_existing(host_t *host, const char *username,
                                  const char *ip, user_data_record_t *record,
                                  bool create_if_missing);
bool host_username_has_password(host_t *host, const char *nick);
void trim_whitespace_inplace(char *text);

void session_send_raw_text(session_ctx_t *ctx, const char *text);
void host_init(host_t *host, auth_profile_t *auth);
void host_set_motd(host_t *host, const char *motd);
void host_set_welcome_banner(host_t *host, const char *banner);
int host_serve(host_t *host, const char *bind_addr, const char *port,
               const char *key_directory, const char *telnet_bind_addr,
               const char *telnet_port);
bool host_post_client_message(host_t *host, const char *username,
                              const char *message, const char *color_name,
                              const char *highlight_name, bool is_bold);
void host_shutdown(host_t *host);
void host_shutdown_for_testing(host_t *host);
bool host_snapshot_last_captcha(host_t *host, char *question,
                                size_t question_length, char *answer,
                                size_t answer_length,
                                struct timespec *timestamp);
bool is_pure_ascii(const char *str);
bool is_nullarray(uint8_t *arr, size_t len);

session_ctx_t *host_session_create_for_testing(host_t *host,
                                               const char *username,
                                               const char *ip,
                                               bool is_operator);
void host_session_destroy_for_testing(session_ctx_t *ctx);
void host_session_process_line_for_testing(session_ctx_t *ctx,
                                           const char *line);

void session_handle_retro(session_ctx_t *ctx, const char *arguments);
#endif

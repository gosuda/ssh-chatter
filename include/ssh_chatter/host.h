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
#include <libssh/callbacks.h>
#include <ttak/container/pool.h>

#include "theme.h"
#include "security_layer.h"
#include "codepage.h"
#include "chardet.h"
#include "display_model.h"

#define HOST_GRANTS_CLEAR_SIZE 4512

#define SSH_CHATTER_SOUND_URL_LEN 1024
#define SSH_CHATTER_MESSAGE_LIMIT 4096
#define SSH_CHATTER_MOTD_MAX_NOTIFICATION_LEN 16384
#define SSH_CHATTER_BANNER_MAX_LEN SSH_CHATTER_MOTD_MAX_NOTIFICATION_LEN * 4
#define SSH_CHATTER_MAX_INPUT_LEN 1024
#define SSH_CHATTER_USERNAME_LEN 256
#define SSH_CHATTER_IP_LEN 64
#define SSH_CHATTER_COLOR_NAME_LEN 32
#define SSH_CHATTER_COLOR_CODE_LEN 256
#define ALPHA_GRAVITY_NAME_LEN 32
#define ALPHA_MAX_GRAVITY_SOURCES 16
#define ALPHA_MAX_WAYPOINTS 4U
#define SSH_CHATTER_MAX_BANS 16384
#define SSH_CHATTER_HISTORY_LIMIT 64
#define SSH_CHATTER_HISTORY_CACHE_LIMIT 256
#define SSH_CHATTER_DOOR_GAME_LIMIT 16
#define SSH_CHATTER_DOOR_GAME_NAME_LEN 32
#define SSH_CHATTER_DOOR_GAME_DESC_LEN 128
#define SSH_CHATTER_INPUT_HISTORY_LIMIT 32
#define SSH_CHATTER_SCROLLBACK_CHUNK 30
#define SSH_CHATTER_SCROLLBACK_MAX_CHUNK 64
#define SSH_CHATTER_TOPOLOGY_LEN 256
#define MSG_SHOULDSINK 0x1U
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
#define SSH_CHATTER_BBS_MAX_BOARDS 16
#define SSH_CHATTER_BBS_MAX_VOTES 8192
#define SSH_CHATTER_BBS_MAX_DRAFTS_PER_USER 8
#define SSH_CHATTER_RSS_MAX_FEEDS 32
#define SSH_CHATTER_RSS_TAG_LEN 32
#define SSH_CHATTER_RSS_URL_LEN 1024
#define SSH_CHATTER_RSS_ITEM_KEY_LEN 1024
#define SSH_CHATTER_RSS_TITLE_LEN 512
#define SSH_CHATTER_RSS_LINK_LEN 1024
#define SSH_CHATTER_RSS_SUMMARY_LEN 2048
#define SSH_CHATTER_RSS_DOWNLOAD_MAX_BYTES (4U * 1024U * 1024U)
#define SSH_CHATTER_RSS_MAX_ITEMS 32
#define SSH_CHATTER_MAX_GRANTS 128
#define SSH_CHATTER_MAX_BLOCKED 64
#define SSH_CHATTER_JOIN_BAR_MAX 17
#define SSH_CHATTER_LANG_NAME_LEN 64
#define SSH_CHATTER_STATUS_LEN 128
#define SSH_CHATTER_FILE_STORAGE_ROOT "/etc/ssh-chatter/user-files"
#define SSH_CHATTER_PROVIDER_LABEL_LEN 64
#define SSH_CHATTER_CLIENT_BANNER_LEN 128
#define SSH_CHATTER_TERMINAL_TYPE_LEN 64
#define SSH_CHATTER_INPUT_ESCAPE_BUFFER_LEN 64
#define SSH_CHATTER_TETRIS_ESCAPE_BUFFER_LEN 64
#define SSH_CHATTER_MULTIBYTE_INPUT_BUFFER_LEN 16
#define SSH_CHATTER_CAMOUFLAGE_LANGUAGE_LEN 64
#define SSH_CHATTER_MORSE_FILTER_LEN 512
#define SSH_CHATTER_REALTIME_RECENT_LIMIT 8
#define SSH_CHATTER_BBS_MAX_LINES 1000
#define SSH_CHATTER_ASCIIART_MAX_LINES 700
#define SSH_CHATTER_ASCIIART_BUFFER_LEN SSH_CHATTER_BBS_BODY_LEN
#define SSH_CHATTER_ASCIIART_COOLDOWN_SECONDS 600
#define SSH_CHATTER_WALL_WIDTH 80
#define SSH_CHATTER_WALL_HEIGHT 24
#define SSH_CHATTER_ELIZA_MEMORY_MAX 128
#define SSH_CHATTER_AI_MEMORY_MAX 64
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
#define SSH_CHATTER_MAX_RESERVED_NAMES 2048 /* reduced from 16384 to curb session-struct RSS */
#define SSH_CHATTER_TETRIS_SCREEN_BUFFER_SIZE 4096
#define SSH_CHATTER_OTHELLO_BOARD_SIZE 8
#define SSH_CHATTER_OTHELLO_MAX_MOVES \
    (SSH_CHATTER_OTHELLO_BOARD_SIZE * SSH_CHATTER_OTHELLO_BOARD_SIZE)
#define SSH_CHATTER_OTHELLO_MAX_SLOTS 1024
#define SSH_CHATTER_OTHELLO_MAX_WAIT_QUEUE SSH_CHATTER_OTHELLO_MAX_SLOTS
#define SSH_CHATTER_GONU_MAX_SLOTS 1024
#define SSH_CHATTER_OUTPUT_BUFFER_SIZE 32768
#define SSH_CHATTER_MAX_NICKNAME_CLAIMS 1024

#include "user_data.h"

struct host;
struct session_ctx;
struct client_manager;
struct webssh_client;
struct morse_client;
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
    ttak_mutex_t lock;
    struct session_ctx **members;
    size_t member_count;
    size_t member_capacity;
    struct host *owner;
} chat_room_t;

typedef struct host_moderation_task host_moderation_task_t;

typedef struct host_moderation_state {
    bool active;
    ttak_mutex_t mutex;
    ttak_cond_t cond;
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
    ttak_mutex_t mutex;
    ttak_cond_t cond;
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

typedef struct chat_history_entry {
    bool is_user_message;
    bool preserve_whitespace;
    char message[SSH_CHATTER_MESSAGE_LIMIT];
    char username[SSH_CHATTER_USERNAME_LEN];
    char raw_username[SSH_CHATTER_USERNAME_LEN];
    char user_ip[SSH_CHATTER_IP_LEN];
    char user_topology[SSH_CHATTER_TOPOLOGY_LEN];
    char user_color_code[SSH_CHATTER_COLOR_CODE_LEN];
    char user_highlight_code[SSH_CHATTER_COLOR_CODE_LEN];
    bool user_is_bold;
    char user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    uint64_t message_id;
    chat_attachment_type_t attachment_type;
    char attachment_target[SSH_CHATTER_ATTACHMENT_TARGET_LEN];
    char attachment_caption[SSH_CHATTER_ATTACHMENT_CAPTION_LEN];
    time_t created_at;
    session_ui_language_t sender_ui_language;
    uint32_t reaction_counts[SSH_CHATTER_REACTION_KIND_COUNT];
} chat_history_entry_t;

static inline bool chat_history_entry_is_empty(const chat_history_entry_t *entry)
{
    if (entry == nullptr) {
        return true;
    }
    return entry->message[0] == '\0' && entry->attachment_type == CHAT_ATTACHMENT_NONE;
}

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

typedef struct ai_chat_memory_entry {
    time_t stored_at;
    char username[SSH_CHATTER_USERNAME_LEN];
    char prompt[SSH_CHATTER_MESSAGE_LIMIT];
    char reply[SSH_CHATTER_MESSAGE_LIMIT];
} ai_chat_memory_entry_t;

typedef struct door_game_entry {
    bool in_use;
    char name[SSH_CHATTER_DOOR_GAME_NAME_LEN];
    char dosbox_conf[PATH_MAX];
    char description[SSH_CHATTER_DOOR_GAME_DESC_LEN];
    bool locked;
} door_game_entry_t;

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
    char *original_pattern;
    char *normalized_pattern;
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
    char provider_label[SSH_CHATTER_PROVIDER_LABEL_LEN];
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
    unsigned int accept_error_streak;
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
    char input_escape_buffer[SSH_CHATTER_TETRIS_ESCAPE_BUFFER_LEN];
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
    unsigned difficulty_level; // 1-5, where 5 is hardest
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
    GONU_VARIANT_HOBAK = 0,   // Hobak-gonu - Pumpkin Gonu (3x3 grid)
    GONU_VARIANT_BAKWI,       // Bakwi-gonu - Wheel Gonu (circular pattern)
    GONU_VARIANT_UMUL,        // Umul-gonu - Well Gonu (well pattern)
    GONU_VARIANT_JANGGI_STAR, // Janggi-gonu variant - palace star lattice
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
    bool placement_phase;  // True during initial piece placement
    bool movement_started; // True after the first slide in movement phase
    unsigned player_pieces;
    unsigned ai_pieces;
    int selected_row;
    int selected_col;
    bool piece_selected;
    bool awaiting_variant_selection;
    bool awaiting_mode_selection;
    bool awaiting_difficulty_selection;
    unsigned difficulty_level; // 1-5, where 5 is hardest
    bool multiplayer;
    bool awaiting_opponent;
    int slot_index;
    unsigned player_number;
} gonu_game_state_t;

typedef struct session_game_state {
    bool active;
    session_game_type_t type;
    tetris_game_state_t *tetris;
    bool is_camouflaged;
    tetris_game_state_t *saved_tetris_state;
    liar_game_state_t *saved_liar_state;
    alpha_centauri_game_state_t *saved_alpha_state;
    othello_game_state_t *saved_othello_state;
    gonu_game_state_t *saved_gonu_state;
    char chosen_camouflage_language[SSH_CHATTER_CAMOUFLAGE_LANGUAGE_LEN];
    liar_game_state_t *liar;
    alpha_centauri_game_state_t *alpha;
    othello_game_state_t *othello;
    gonu_game_state_t *gonu;
    uint64_t rng_state;
    bool rng_seeded;
    unsigned tetris_render_count;
    uint8_t tetris_prev_cells[SSH_CHATTER_TETRIS_HEIGHT]
                             [SSH_CHATTER_TETRIS_WIDTH];
    bool tetris_prev_cells_valid;
    unsigned tetris_prev_score;
    unsigned tetris_prev_lines_cleared;
    unsigned tetris_prev_round;
    int tetris_prev_next_piece;
    bool tetris_prev_game_over;
    bool tetris_prev_hud_valid;
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
    SESSION_TRANSPORT_DDIAL,
} session_transport_kind_t;

typedef enum session_ui_mode {
    SESSION_UI_MODE_ANYTHING = 0,
    SESSION_UI_MODE_BBS,
    SESSION_UI_MODE_RSS,
    SESSION_UI_MODE_GAME,
} session_ui_mode_t;

typedef enum session_input_mode {
    SESSION_INPUT_MODE_CHAT = 0,
    SESSION_INPUT_MODE_COMMAND,
} session_input_mode_t;

typedef enum session_asciiart_target {
    SESSION_ASCIIART_TARGET_NONE = 0,
    SESSION_ASCIIART_TARGET_CHAT,
} session_asciiart_target_t;

typedef enum session_editor_mode {
    SESSION_EDITOR_MODE_NONE = 0,
    SESSION_EDITOR_MODE_BBS_CREATE,
    SESSION_EDITOR_MODE_BBS_EDIT,
    SESSION_EDITOR_MODE_ASCIIART,
} session_editor_mode_t;

typedef struct rss_session_item {
    char id[SSH_CHATTER_RSS_ITEM_KEY_LEN];
    char title[SSH_CHATTER_RSS_TITLE_LEN];
    char link[SSH_CHATTER_RSS_LINK_LEN];
    char summary[SSH_CHATTER_RSS_SUMMARY_LEN];
} rss_session_item_t;

typedef struct ascii_pixel {
    char ch;
    char color_name[SSH_CHATTER_COLOR_NAME_LEN];
    int64_t updated_at_ns;
} ascii_pixel_t;

typedef struct rss_feed {
    bool in_use;
    char tag[SSH_CHATTER_RSS_TAG_LEN];
    char url[SSH_CHATTER_RSS_URL_LEN];
    char last_item_key[SSH_CHATTER_RSS_ITEM_KEY_LEN];
    char last_title[SSH_CHATTER_RSS_TITLE_LEN];
    char last_link[SSH_CHATTER_RSS_LINK_LEN];
    time_t last_checked;
    uint8_t window_id;
    size_t stored_item_count;
    rss_session_item_t stored_items[SSH_CHATTER_RSS_MAX_ITEMS];
} rss_feed_t;

typedef struct session_rss_view {
    bool active;
    char tag[SSH_CHATTER_RSS_TAG_LEN];
    size_t item_count;
    size_t cursor;
    rss_session_item_t *items;
} session_rss_view_t;

typedef enum session_cp437_override {
    SESSION_CP437_OVERRIDE_NONE = 0,
    SESSION_CP437_OVERRIDE_FORCE_OFF,
    SESSION_CP437_OVERRIDE_FORCE_ON,
} session_cp437_override_t;

typedef enum session_cp437_scope {
    SESSION_CP437_SCOPE_ALL = 0,
    SESSION_CP437_SCOPE_SYSTEM_ONLY,
    SESSION_CP437_SCOPE_CHAT_ONLY,
} session_cp437_scope_t;

typedef enum session_output_kind {
    SESSION_OUTPUT_KIND_SYSTEM = 0,
    SESSION_OUTPUT_KIND_CHAT,
    SESSION_OUTPUT_KIND_GENERIC,
} session_output_kind_t;

typedef struct session_runtime_data {
    uint64_t session_id;
    _Atomic bool active;
    struct session_ctx *ctx;
} session_runtime_data_t;

typedef struct nickname_claim {
    char nickname[SSH_CHATTER_USERNAME_LEN];
    char owner_ip[SSH_CHATTER_IP_LEN];
    uint8_t password_salt[SECURITY_LAYER_SALT_LEN];
    uint8_t password_hash[SECURITY_LAYER_HASH_LEN];
    uint64_t owner_session_id;
    bool ip_wide;
    bool fixnick;
} nickname_claim_t;

typedef enum session_newline_mode {
    SESSION_NEWLINE_MODE_AUTO = 0,
    SESSION_NEWLINE_MODE_LF,
    SESSION_NEWLINE_MODE_CRLF,
} session_newline_mode_t;

typedef struct session_ctx {
    uint64_t session_id;
    void *session_data;
    ssh_session session;
    ssh_channel channel;
    struct ssh_channel_callbacks_struct channel_cb;
    bool channel_cb_installed;
    session_transport_kind_t transport_kind;
    int telnet_fd;
    bool telnet_negotiated;
    bool telnet_eof;
    bool telnet_pending_valid;
    int telnet_pending_char;
    bool telnet_consume_next_lf;
    bool telnet_terminal_type_requested;
    int ddial_fd;
    uint8_t ddial_channel;
    bool ddial_logged_in;
    bool ddial_should_exit;
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
    char input_escape_buffer[SSH_CHATTER_INPUT_ESCAPE_BUFFER_LEN];
    size_t input_escape_length;
    /* Multi-byte character buffer for CP949, CP932, CP936, etc. */
    unsigned char multibyte_input_buffer[SSH_CHATTER_MULTIBYTE_INPUT_BUFFER_LEN];
    size_t multibyte_input_length;
    bool bracket_paste_active;
    char client_ip[SSH_CHATTER_IP_LEN];
    char client_banner[SSH_CHATTER_CLIENT_BANNER_LEN];
    char terminal_type[SSH_CHATTER_TERMINAL_TYPE_LEN];
    char retro_client_marker[SSH_CHATTER_TERMINAL_TYPE_LEN];
    char telnet_identity[SSH_CHATTER_CLIENT_BANNER_LEN];
    char user_color_code[SSH_CHATTER_COLOR_CODE_LEN];
    char user_highlight_code[SSH_CHATTER_COLOR_CODE_LEN];
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
    unsigned int output_lines_since_prompt;
    bool prompt_needs_padding;
    int exit_status;
    bool should_exit;
    bool exit_notice_sent;
    bool username_conflict;
    bool has_joined_room;
    atomic_bool room_snapshot_retired;
    atomic_uint room_snapshot_refs;
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
    char *pending_bbs_title;
    char (*pending_bbs_tags)[SSH_CHATTER_BBS_TAG_LEN];
    size_t pending_bbs_tag_count;
    char *pending_bbs_body;
    size_t pending_bbs_body_length;
    size_t pending_bbs_line_count;
    size_t pending_bbs_cursor_line;
    bool pending_bbs_editing_line;
    size_t bbs_editor_scroll_offset;
    size_t bbs_editor_selection_start;
    bool bbs_editor_selection_start_set;
    size_t bbs_editor_selection_end;
    bool bbs_editor_selection_end_set;
    char *bbs_editor_clipboard;
    size_t bbs_editor_clipboard_length;
    size_t bbs_editor_clipboard_lines;
    bool bbs_line_edit_mode;
    size_t bbs_line_edit_target;
    bool bbs_search_active;
    size_t bbs_search_restore_line;
    bool bbs_search_restore_editing;
    size_t bbs_search_restore_scroll;
    bool bbs_view_active;
    uint64_t bbs_view_post_id;
    size_t bbs_view_scroll_offset;
    size_t bbs_view_total_lines;
    bool bbs_view_notice_pending;
    char *bbs_view_notice;
    bool bbs_rendering_editor;
    uint64_t bbs_current_board_id;
    bool breaking_alerts_enabled;
    bool morse_feed_enabled;
    char morse_filter[SSH_CHATTER_MORSE_FILTER_LEN];
    bool prefer_utf16_output;
    session_newline_mode_t newline_mode;
    bool prefer_cp437_output;
    session_cp437_scope_t cp437_output_scope;
    session_output_kind_t output_kind;
    session_ui_mode_t ui_mode;
    bool unicode_all_mode;
    bool hybrid_output_mode;
    session_cp437_override_t cp437_override;
    bool cp437_input_enabled;
    session_codepage_t active_codepage;
    session_codepage_context_t codepage_ctx;
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
    ttak_mutex_t translation_mutex;
    ttak_mutex_t chat_message_count_mutex;
    ttak_mutex_t channel_mutex;
    ttak_cond_t translation_cond;
    ttak_mutex_t output_lock;
    bool translation_mutex_initialized;
    bool translation_cond_initialized;
    bool output_lock_initialized;
    bool channel_mutex_initialized;
    bool translation_thread_started;
    bool translation_thread_stop;
    pthread_t translation_thread;
    struct translation_job *translation_pending_head;
    struct translation_job *translation_pending_tail;
    struct translation_result *translation_ready_head;
    struct translation_result *translation_ready_tail;
    char status_message[SSH_CHATTER_STATUS_LEN];
    bool asciiart_pending;
    session_asciiart_target_t asciiart_target;
    char *asciiart_buffer;
    size_t asciiart_length;
    size_t asciiart_line_count;
    bool asciiart_has_cooldown;
    struct timespec last_asciiart_post;
    session_game_state_t game;
    bool othello_slot_queued;
    session_block_entry_t block_entries[SSH_CHATTER_MAX_BLOCKED];
    size_t block_entry_count;
    session_block_prompt_t block_pending;
    bool in_rss_mode;
    session_rss_view_t rss_view;
    bool user_data_loaded;
    user_data_record_t user_data;
    bool password_not_set; // Flag to indicate if user needs to set a password
    bool wall_active;
    bool wall_command_mode;
    uint8_t wall_cursor_x;
    uint8_t wall_cursor_y;
    char wall_brush_char;
    char wall_brush_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char *tetris_screen_buffer;
    char *tetris_prev_screen_buffer;
    const session_ops_t *ops;
    bool history_oldest_notified;
    bool history_latest_notified;
    size_t scrollback_rendered_lines;
    chat_history_entry_t *scrollback_buffer;
    size_t scrollback_buffer_capacity;
    bool
        no_update; // Flag to prevent automatic message updates when scrolling history
    bool pending_should_sink;
    size_t last_sink_history_total;
    // Output buffering to prevent flickering
    bool output_buffering_enabled;
    char output_buffer[SSH_CHATTER_OUTPUT_BUFFER_SIZE];
    size_t output_buffer_length;
    // Live chat rendering controls
    size_t realtime_line_count;
    size_t realtime_recent_count;
    size_t realtime_recent_start;
    char realtime_recent_lines[SSH_CHATTER_REALTIME_RECENT_LIMIT]
                              [SSH_CHATTER_MESSAGE_LIMIT];
    bool capture_realtime_output;
    char last_output_line[SSH_CHATTER_MESSAGE_LIMIT];
    bool has_last_output_line;
    bool disable_output_dedup;
    // Dedicated memory context for this session
    sshc_memory_context_t *memory_context;
    // Ownership and isolation context
    tt_owner_t *session_owner;
    // Full-frame display model for rendering (replaces incremental path)
    display_model_t display_model;
    bool display_model_initialized;
    uint32_t lifetime_units;
    struct timespec lifetime_last_activity;
    struct timespec lifetime_decay_reference;
    bool lifetime_has_activity;
    bool lifetime_decay_active;
} session_ctx_t;

typedef struct user_preference {
    bool in_use;
    bool has_user_theme;
    bool has_system_theme;
    char username[SSH_CHATTER_USERNAME_LEN];
    char ip[SSH_CHATTER_IP_LEN];
    char user_color_code[SSH_CHATTER_COLOR_CODE_LEN];
    char user_highlight_code[SSH_CHATTER_COLOR_CODE_LEN];
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
    char provider_label[SSH_CHATTER_PROVIDER_LABEL_LEN];
    char camouflage_language[SSH_CHATTER_CAMOUFLAGE_LANGUAGE_LEN];
    bool show_continuous_messages;
    session_ui_mode_t preferred_ui_mode;
    bool unicode_all_mode;
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
    int32_t upvotes;
    int32_t downvotes;
} bbs_comment_t;

typedef struct bbs_post {
    bool in_use;
    uint64_t id;
    uint16_t board_id;
    char author[SSH_CHATTER_USERNAME_LEN];
    char title[SSH_CHATTER_BBS_TITLE_LEN];
    char body[SSH_CHATTER_BBS_BODY_LEN];
    char tags[SSH_CHATTER_BBS_MAX_TAGS][SSH_CHATTER_BBS_TAG_LEN];
    size_t tag_count;
    time_t created_at;
    time_t bumped_at;
    int32_t upvotes;
    int32_t downvotes;
    bbs_comment_t *comments;
    size_t comment_count;
} bbs_post_t;

typedef struct bbs_board {
    uint16_t board_id;
    char name[32];
    char description[128];
    bool is_notice;
} bbs_board_t;

typedef struct bbs_vote {
    uint64_t target_post_id;
    int32_t target_comment_idx; /* -1 for post votes */
    char voter_username[SSH_CHATTER_USERNAME_LEN];
    int8_t vote_type; /* +1 or -1 */
    time_t created_at;
} bbs_vote_t;

typedef struct bbs_draft {
    bool in_use;
    uint64_t id;
    char author[SSH_CHATTER_USERNAME_LEN];
    uint16_t board_id;
    char title[SSH_CHATTER_BBS_TITLE_LEN];
    char body[SSH_CHATTER_BBS_BODY_LEN];
    char tags[SSH_CHATTER_BBS_MAX_TAGS][SSH_CHATTER_BBS_TAG_LEN];
    size_t tag_count;
    time_t created_at;
} bbs_draft_t;

typedef struct host_ban_entry {
    char username[SSH_CHATTER_USERNAME_LEN];
    char ip[SSH_CHATTER_IP_LEN];
} host_ban_entry_t;

typedef struct host_operator_grant {
    char ip[SSH_CHATTER_IP_LEN];
} host_operator_grant_t;

typedef enum ddial_auth_state {
    DDIAL_AUTH_NONE = 0,
    DDIAL_AUTH_PENDING,
    DDIAL_AUTH_APPROVED,
    DDIAL_AUTH_REJECTED,
    DDIAL_AUTH_TIMEOUT,
} ddial_auth_state_t;

#define DDIAL_RELAY_SENT_HISTORY 8

typedef struct ddial_relay {
    bool enabled;
    int upstream_fd;
    pthread_t thread;
    bool thread_initialized;
    _Atomic bool running;
    _Atomic bool stop;
    ttak_mutex_t lock;
    bool lock_initialized;
    bool connected;
    bool auth_sent;
    ddial_auth_state_t auth_state;
    struct timespec auth_deadline;
    bool locally_registered;
    char host[256];
    int port;
    char login_key[64];
    char handle[SSH_CHATTER_USERNAME_LEN];
    char recv_buffer[SSH_CHATTER_MESSAGE_LIMIT * 4];
    size_t recv_buf_len;
    struct timespec last_send_time;
    uint16_t slot;
    bool slot_known;
    struct {
        char raw_line[SSH_CHATTER_MESSAGE_LIMIT];
        struct timespec sent_at;
    } recent_sent[DDIAL_RELAY_SENT_HISTORY];
    size_t recent_sent_index;
    unsigned int reconnect_attempts;
    struct timespec last_disconnect_time;
} ddial_relay_t;

typedef struct host {
    sshc_memory_context_t *memory_context;
    chat_room_t room;
    struct timespec last_room_empty_time;
    bool idle_state_pending;
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
    } json_api;
    char jwt_secret[128];
    auth_profile_t *auth;
    UserTheme user_theme;
    SystemTheme system_theme;
    char default_user_color_name[SSH_CHATTER_COLOR_NAME_LEN];
    char default_user_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    char default_system_fg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char default_system_bg_name[SSH_CHATTER_COLOR_NAME_LEN];
    char default_system_highlight_name[SSH_CHATTER_COLOR_NAME_LEN];
    host_ban_entry_t *bans;
    size_t ban_count;
    size_t ban_capacity;
    char version[64];
    char motd[4096];
    char motd_base[4096];
    char motd_path[PATH_MAX];
    bool motd_has_file;
    struct timespec motd_last_modified;
    char welcome_banner[SSH_CHATTER_BANNER_MAX_LEN];
    bool welcome_banner_loaded;
    bool welcome_banner_is_ans;
    bool translation_quota_exhausted;
    size_t connection_count;
    chat_history_entry_t *history;
    size_t history_count;
    size_t history_capacity;
    size_t history_start_index;
    size_t history_total;
    bool history_cache_loaded;
    chat_history_entry_t *history_override;
    size_t history_override_count;
    uint64_t next_message_id;
    chat_reply_entry_t *replies;
    size_t reply_count;
    size_t reply_capacity;
    uint64_t next_reply_id;
    user_preference_t *preferences;
    size_t preference_count;
    size_t preference_capacity;
    ttak_mutex_t lock;
    char state_file_path[PATH_MAX];
    char sync_state_file_path[PATH_MAX];
    char wall_state_file_path[PATH_MAX];
    char bbs_state_file_path[PATH_MAX];
    char vote_state_file_path[PATH_MAX];
    char ban_state_file_path[PATH_MAX];
    char reply_state_file_path[PATH_MAX];
    char ui_lang_state_file_path[PATH_MAX];
    char pw_auth_file_path[PATH_MAX];
    char alpha_landers_file_path[PATH_MAX];
    char nickname_claim_file_path[PATH_MAX];
    char user_data_root[PATH_MAX];
    bool user_data_ready;
    ttak_mutex_t user_data_lock;
    bool user_data_lock_initialized;
    ttak_mutex_t alpha_landers_lock;
    bool alpha_landers_lock_initialized;
    _Atomic bool security_filter_enabled;
    _Atomic bool security_filter_failure_logged;
    _Atomic bool security_ai_enabled;
    _Atomic bool geo_language_enabled;
    host_moderation_state_t moderation;
    host_eliza_worker_state_t eliza_worker;
    atomic_uint_fast64_t next_session_id;
    pthread_t bbs_watchdog_thread;
    bool bbs_watchdog_thread_initialized;
    _Atomic bool bbs_watchdog_thread_running;
    _Atomic bool bbs_watchdog_thread_stop;
    struct timespec bbs_watchdog_last_run;
    poll_state_t poll;
    named_poll_state_t *named_polls;
    size_t named_poll_count;
    size_t named_poll_capacity;
    bbs_post_t *bbs_posts;
    size_t bbs_post_count;
    size_t bbs_post_capacity;
    uint64_t next_bbs_id;
    bool bbs_cache_loaded;
    bbs_board_t *bbs_boards;
    size_t bbs_board_count;
    size_t bbs_board_capacity;
    bbs_vote_t *bbs_votes;
    size_t bbs_vote_count;
    size_t bbs_vote_capacity;
    bbs_draft_t *bbs_drafts;
    size_t bbs_draft_count;
    size_t bbs_draft_capacity;
    ascii_pixel_t *wall;
    rss_feed_t *rss_feeds;
    size_t rss_feed_count;
    size_t rss_feed_capacity;
    uint8_t rss_current_window_id;
    othello_multiplayer_slot_t *othello_games;
    size_t cpu_slot_side_n;
    size_t cpu_slot_limit;
    size_t cpu_slot_in_use;
    size_t cpu_slot_waiting;
    uint64_t cpu_slot_mask;
    size_t othello_slot_side_n;
    size_t othello_slot_limit;
    uint64_t othello_slot_mask;
    struct session_ctx
        *othello_wait_queue[SSH_CHATTER_OTHELLO_MAX_WAIT_QUEUE];
    size_t othello_wait_queue_head;
    size_t othello_wait_queue_tail;
    size_t othello_wait_queue_count;
    gonu_multiplayer_slot_t *gonu_games;
    bool random_seeded;
    client_manager_t *clients;
    webssh_client_t *web_client;
    struct morse_client *morse_client;
    security_layer_t security_layer;
    bool security_layer_initialized;
    bool file_storage_ready;
    char file_storage_root[PATH_MAX];
    _Atomic bool eliza_enabled;
    _Atomic bool eliza_announced;
    struct timespec eliza_last_action;
    char eliza_state_file_path[PATH_MAX];
    char eliza_memory_file_path[PATH_MAX];
    _Atomic bool ai_chat_enabled;
    bool ai_chat_use_gemini;
    char ai_chat_model[64];
    struct timespec ai_chat_last_reply;
    ai_chat_memory_entry_t *ai_chat_memory;
    size_t ai_chat_memory_count;
    size_t ai_chat_memory_capacity;
    char ai_persona_a_name[64];
    char ai_persona_a_alias[64];
    char ai_persona_b_name[64];
    char ai_persona_b_alias[64];
    door_game_entry_t *door_games;
    size_t door_game_count;
    size_t door_game_capacity;
    size_t active_door_sessions;
    size_t max_door_sessions;
    char rss_state_file_path[PATH_MAX];
    eliza_memory_entry_t *eliza_memory;
    size_t eliza_memory_count;
    size_t eliza_memory_capacity;
    uint64_t eliza_memory_next_id;
    host_operator_grant_t *operator_grants;
    size_t operator_grant_count;
    size_t operator_grant_capacity;
    char (*protected_ips)[SSH_CHATTER_IP_LEN];
    size_t protected_ip_count;
    size_t protected_ip_capacity;
    version_ip_ban_rule_t *version_ip_ban_rules;
    size_t version_ip_ban_rule_count;
    size_t version_ip_ban_rule_capacity;
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
    bool force_restart_requested;
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
    _Atomic bool rss_manual_refresh_running;
    ttak_mutex_t rss_refresh_lock;
    bool rss_refresh_lock_initialized;
    pthread_t archive_thread;
    bool archive_thread_initialized;
    _Atomic bool archive_thread_running;
    _Atomic bool archive_thread_stop;
    struct timespec archive_last_run;

    // Legacy reserved nickname list
    char (*reserved_nicknames)[SSH_CHATTER_USERNAME_LEN];
    size_t reserved_nicknames_len;
    size_t reserved_nicknames_capacity;
    ttak_mutex_t nickname_reserve_lock;
    // Runtime nickname claims guarded by libttak object pool
    ttak_object_pool_t *nickname_claim_pool;
    nickname_claim_t *nickname_claims[SSH_CHATTER_MAX_NICKNAME_CLAIMS];
    size_t nickname_claim_count;
    ddial_relay_t ddial_relay;
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
        char requested_port[16];
        bool port_auto_adjusted;
    } ddial_listener;
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
bool host_nickname_claim_can_use(host_t *host, const session_ctx_t *ctx,
                                 const char *nick);
bool host_nickname_claim_upsert(host_t *host, const session_ctx_t *ctx,
                                const char *nick, const uint8_t *salt,
                                const uint8_t *hash, bool ip_wide,
                                bool fixnick);
void host_nickname_claim_release(host_t *host, const session_ctx_t *ctx,
                                 const char *nick);
void host_nickname_claim_remove(host_t *host, const char *nick);
void host_nickname_claims_save(host_t *host);
void host_nickname_claims_load(host_t *host);
void trim_whitespace_inplace(char *text);

void session_send_raw_text(session_ctx_t *ctx, const char *text);
void session_channel_write(session_ctx_t *ctx, const void *data, size_t length);
void host_init(host_t *host, auth_profile_t *auth);
void host_set_motd(host_t *host, const char *motd);
void host_set_welcome_banner(host_t *host, const char *banner, bool is_ans);
int host_serve(host_t *host, const char *bind_addr, const char *port,
               const char *key_directory, const char *telnet_bind_addr,
               const char *telnet_port, const char *json_bind_addr,
               const char *json_port, const char *ddial_bind_addr,
               const char *ddial_port);

bool host_ddial_listener_start(host_t *host, const char *bind_addr,
                               const char *port);
void host_ddial_listener_stop(host_t *host);
void host_ddial_inject_message(host_t *host, const char *username,
                               const char *message);
void host_ddial_broadcast_system(host_t *host, const char *message);
void host_ddial_init(host_t *host);
void host_ddial_shutdown(host_t *host);
bool host_ddial_client_configure(host_t *host, const char *host_str, int port,
                                 const char *key);
void host_ddial_client_disconnect(host_t *host);
void host_ddial_client_reconnect(host_t *host);
void host_ddial_client_start(host_t *host);
void host_ddial_client_send(host_t *host, const char *handle,
                            const char *message);
void host_ddial_notify_admin_if_port_adjusted(host_t *host, session_ctx_t *ctx);
uint64_t host_allocate_session_id(host_t *host);
void host_archive_start_backend(host_t *host);
bool host_post_client_message(host_t *host, const char *username,
                              const char *message, const char *color_name,
                              const char *highlight_name, bool is_bold);
void host_append_sync_log(host_t *host, const char *source,
                          const char *message);
bool host_post_ephemeral_message(host_t *host, const char *username,
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
void session_handle_hybrid(session_ctx_t *ctx, const char *arguments);
void session_handle_iyagi(session_ctx_t *ctx, const char *arguments);

static inline void host_bans_ensure(host_t *host)
{
    if (host != nullptr && host->bans == nullptr) {
        host->bans = (host_ban_entry_t *)sshc_gc_calloc(
            SSH_CHATTER_MAX_BANS, sizeof(host_ban_entry_t));
        host->ban_capacity = SSH_CHATTER_MAX_BANS;
    }
}

static inline void host_replies_ensure(host_t *host)
{
    if (host != nullptr && host->replies == nullptr) {
        host->replies = (chat_reply_entry_t *)sshc_gc_calloc(
            SSH_CHATTER_MAX_REPLIES, sizeof(chat_reply_entry_t));
        host->reply_capacity = SSH_CHATTER_MAX_REPLIES;
    }
}

static inline void host_preferences_ensure(host_t *host)
{
    if (host != nullptr && host->preferences == nullptr) {
        host->preferences = (user_preference_t *)sshc_gc_calloc(
            SSH_CHATTER_MAX_PREFERENCES, sizeof(user_preference_t));
        host->preference_capacity = SSH_CHATTER_MAX_PREFERENCES;
    }
}

static inline void host_rss_feeds_ensure(host_t *host)
{
    if (host != nullptr && host->rss_feeds == nullptr) {
        host->rss_feeds = (rss_feed_t *)sshc_gc_calloc(
            SSH_CHATTER_RSS_MAX_FEEDS, sizeof(rss_feed_t));
        host->rss_feed_capacity = SSH_CHATTER_RSS_MAX_FEEDS;
    }
}

static inline void host_eliza_memory_ensure(host_t *host)
{
    if (host != nullptr && host->eliza_memory == nullptr) {
        host->eliza_memory = (eliza_memory_entry_t *)sshc_gc_calloc(
            SSH_CHATTER_ELIZA_MEMORY_MAX, sizeof(eliza_memory_entry_t));
        host->eliza_memory_capacity = SSH_CHATTER_ELIZA_MEMORY_MAX;
    }
}

static inline void host_ai_chat_memory_ensure(host_t *host)
{
    if (host != nullptr && host->ai_chat_memory == nullptr) {
        host->ai_chat_memory = (ai_chat_memory_entry_t *)sshc_gc_calloc(
            SSH_CHATTER_AI_MEMORY_MAX, sizeof(ai_chat_memory_entry_t));
        host->ai_chat_memory_capacity = SSH_CHATTER_AI_MEMORY_MAX;
    }
}

static inline void host_gonu_games_ensure(host_t *host)
{
    if (host != nullptr && host->gonu_games == nullptr) {
        host->gonu_games = (gonu_multiplayer_slot_t *)sshc_gc_calloc(
            SSH_CHATTER_GONU_MAX_SLOTS, sizeof(gonu_multiplayer_slot_t));
    }
}

static inline void host_named_polls_ensure(host_t *host)
{
    if (host != nullptr && host->named_polls == nullptr) {
        host->named_polls = (named_poll_state_t *)sshc_gc_calloc(
            SSH_CHATTER_MAX_NAMED_POLLS, sizeof(named_poll_state_t));
        host->named_poll_capacity = SSH_CHATTER_MAX_NAMED_POLLS;
    }
}

static inline void host_othello_games_ensure(host_t *host)
{
    if (host != nullptr && host->othello_games == nullptr) {
        host->othello_games = (othello_multiplayer_slot_t *)sshc_gc_calloc(
            SSH_CHATTER_OTHELLO_MAX_SLOTS, sizeof(othello_multiplayer_slot_t));
    }
}

static inline void host_operator_grants_ensure(host_t *host)
{
    if (host != nullptr && host->operator_grants == nullptr) {
        host->operator_grants = (host_operator_grant_t *)sshc_gc_calloc(
            SSH_CHATTER_MAX_GRANTS, sizeof(host_operator_grant_t));
        host->operator_grant_capacity = SSH_CHATTER_MAX_GRANTS;
    }
}

static inline void host_protected_ips_ensure(host_t *host)
{
    if (host != nullptr && host->protected_ips == nullptr) {
        host->protected_ips = (char (*)[SSH_CHATTER_IP_LEN])sshc_gc_calloc(
            SSH_CHATTER_MAX_PROTECTED_IPS, SSH_CHATTER_IP_LEN);
        host->protected_ip_capacity = SSH_CHATTER_MAX_PROTECTED_IPS;
    }
}

typedef enum {
    AVATAR_NONE = 0,
    AVATAR_MONITOR,
    AVATAR_MOUSE,
    AVATAR_HUMAN,
    AVATAR_MUSHROOM,
    AVATAR_COUNT
} session_avatar_type_t;

extern const char * const kSessionAvatarArt[AVATAR_COUNT];
extern const char * const kSessionAvatarNames[AVATAR_COUNT];

#endif

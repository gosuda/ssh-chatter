/**
 * @file magviz.h
 * @desc Per-session state for the MagViz-compatible command layer that sits
 *       on top of the Diversi Dial listener (see magviz.c).  Private to the
 *       host transport translation unit; included by server.c before the
 *       ddial_session_t definition.
 */

#ifndef SSH_CHATTER_DDIAL_MAGVIZ_H
#define SSH_CHATTER_DDIAL_MAGVIZ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Local channels.  Channels 1-4 map onto the Station Link's tunable
 * channels; 5-999 are station-local rooms.  Only channel 1 is shared with
 * the Chatter main room. */
#define DDIAL_MV_MAX_CHANNEL 999U
#define DDIAL_MV_GUEST_MAX_CHANNEL 4U
#define DDIAL_MV_ROOM_CHANNEL 1U

#define DDIAL_MV_SLOT_LIST 16U
#define DDIAL_MV_MONITOR_LIST 8U
#define DDIAL_MV_MENTION_LOG 20U
#define DDIAL_MV_SAVED_HANDLES 9U
#define DDIAL_MV_CODE_LIMIT 2048U
#define DDIAL_MV_TEXT_LIMIT 512U

/* /b? notification events. */
enum {
    DDIAL_MV_BEEP_PM = 1U << 0,
    DDIAL_MV_BEEP_MENTION = 1U << 1,
    DDIAL_MV_BEEP_LOGIN = 1U << 2,
    DDIAL_MV_BEEP_CHAT = 1U << 3,
    DDIAL_MV_BEEP_BUZZ = 1U << 4,
};

typedef struct ddial_mv_slot_list {
    uint32_t items[DDIAL_MV_SLOT_LIST];
    size_t count;
} ddial_mv_slot_list_t;

typedef struct ddial_mv_session {
    uint32_t member_no; /* 0 = guest */
    bool cosysop;
    bool full_muted;    /* /xx: guest may not chat or run commands */

    /* Display */
    char display_handle[256]; /* ANSI-rendered /h#COLOR handle */
    char msg_color[16];       /* "RRGGBB" or "RRGGBB,RRGGBB" gradient */
    char prefix[48];
    char postfix[48];
    char status[96];
    bool badges_on;
    bool lorem_on;
    bool reverse_on;
    bool scramble_on;
    bool replies_on;
    bool auto_slash;

    /* Client preferences that only matter to web/GUI front ends. */
    char font[48];
    bool voice_on;
    bool voice_echo;
    bool notify_on;
    bool remember_on;

    /* Notifications */
    bool beep_on;
    uint8_t beep_volume;
    uint8_t beep_events;

    /* Output pacing (/baud): 0 = unthrottled. */
    uint16_t baud;

    /* Line (slot) lists */
    ddial_mv_slot_list_t auto_pm;       /* /p= */
    ddial_mv_slot_list_t ignore_slots;  /* /ig# */
    ddial_mv_slot_list_t ignore_members;/* /ig+# */
    ddial_mv_slot_list_t squelch_slots; /* /x# */
    ddial_mv_slot_list_t null_slots;    /* /null# */
    ddial_mv_slot_list_t monitored;     /* /t+# */
    uint16_t last_monitor;

    /* Multi-line capture: /code and /r= */
    bool code_capture;
    char code_buf[DDIAL_MV_CODE_LIMIT];
    size_t code_len;
    bool profile_capture;

    /* /@? mention log */
    char mentions[DDIAL_MV_MENTION_LOG][160];
    size_t mention_head;
    size_t mention_count;

    /* /q+ countdown */
    time_t quit_deadline;
    time_t quit_last_tick;
    char quit_msg[128];

    time_t login_time;
    time_t channel_joined_at;
    uint32_t messages_sent;

    /* Sentry flood window */
    time_t sentry_window_start;
    uint32_t sentry_window_count;
} ddial_mv_session_t;

#endif /* SSH_CHATTER_DDIAL_MAGVIZ_H */

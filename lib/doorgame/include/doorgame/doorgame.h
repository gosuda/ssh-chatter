/**
 * @file doorgame.h
 * @desc Public API for the ssh-chatter door-game library.
 *       The library knows nothing about ssh-chatter internal types;
 *       all host/session interactions go through the callback tables.
 */

#ifndef DOORGAME_H
#define DOORGAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handles ---------------------------------------------------------- */
typedef struct doorgame_session doorgame_session_t;
typedef struct doorgame_host    doorgame_host_t;

/* Entry ------------------------------------------------------------------- */
#define DOORGAME_NAME_LEN        32
#define DOORGAME_DESC_LEN        128
#define DOORGAME_DOSBOX_CONF_LEN PATH_MAX

typedef struct {
    bool in_use;
    char name[DOORGAME_NAME_LEN];
    char dosbox_conf[DOORGAME_DOSBOX_CONF_LEN];
    char description[DOORGAME_DESC_LEN];
    bool locked;
} doorgame_entry_t;

/* Localization message IDs ------------------------------------------------ */
enum {
    DOORGAME_MSG_AVAILABLE = 0,
    DOORGAME_MSG_LAUNCH_HINT,
    DOORGAME_MSG_NO_DOORS,
    DOORGAME_MSG_LAUNCHING,
    DOORGAME_MSG_WAITING,
    DOORGAME_MSG_CONNECTED,
    DOORGAME_MSG_TIMEOUT,
    DOORGAME_MSG_FAILED_LAUNCH,
    DOORGAME_MSG_SESSION_ENDED,
    DOORGAME_MSG_ENDED_IMMEDIATELY,
    DOORGAME_MSG_ESCAPE,
    DOORGAME_MSG_MAX_RUNTIME,
    DOORGAME_MSG_TOO_MANY,
    DOORGAME_MSG_ONLY_OPS_LOCK,
    DOORGAME_MSG_GAME_LOCKED,
    DOORGAME_MSG_SETGAMELOCK_USAGE,
    DOORGAME_MSG_NOW_LOCKED,
    DOORGAME_MSG_NOW_UNLOCKED,
    DOORGAME_MSG_LIST_LOCKED,
    DOORGAME_MSG_COUNT
};

/* Session callbacks ------------------------------------------------------- */
typedef struct {
    int  (*read_poll)(doorgame_session_t *s, char *buf, size_t len, int timeout_ms);
    void (*write)(doorgame_session_t *s, const void *data, size_t len);
    bool (*write_all)(doorgame_session_t *s, const void *data, size_t len);
    void (*send_system_line)(doorgame_session_t *s, const char *msg);
    void (*set_buffering)(doorgame_session_t *s, bool enabled);
    bool (*get_buffering)(doorgame_session_t *s);
    const char *(*localized)(doorgame_session_t *s, int msg_id);
    bool (*is_operator)(doorgame_session_t *s);
    bool (*is_lan_operator)(doorgame_session_t *s);
    void *(*gc_malloc)(size_t);
    void  (*gc_free)(void *);
    void *(*gc_realloc)(void *, size_t);
} doorgame_session_ops_t;

/* Host callbacks ---------------------------------------------------------- */
typedef struct {
    size_t max_sessions;
    size_t active_sessions;
    doorgame_entry_t *entries;
    size_t entry_count;
    void (*inc_active)(doorgame_host_t *h);
    void (*dec_active)(doorgame_host_t *h);
    bool (*is_shutting_down)(doorgame_host_t *h);
    void (*save_locks)(doorgame_host_t *h);
    const char *(*get_ddial_port)(doorgame_host_t *h);
} doorgame_host_ops_t;

/* API --------------------------------------------------------------------- */

/** List available door games. */
void doorgame_list(doorgame_session_t *s, doorgame_host_t *h,
                   const doorgame_session_ops_t *sops,
                   const doorgame_host_ops_t *hops);

/** Run a door game. Returns true if the session ran. */
bool doorgame_run(doorgame_session_t *s, doorgame_host_t *h,
                  const doorgame_entry_t *entry,
                  const doorgame_session_ops_t *sops,
                  const doorgame_host_ops_t *hops);

/** Toggle lock on a door game. Returns true if state changed. */
bool doorgame_toggle_lock(doorgame_session_t *s, doorgame_host_t *h,
                          const char *name, bool is_operator,
                          const doorgame_session_ops_t *sops,
                          const doorgame_host_ops_t *hops);

#ifdef __cplusplus
}
#endif

#endif /* DOORGAME_H */

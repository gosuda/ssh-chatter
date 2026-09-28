/**
 * @file roster.c
 * @desc Roster of the users on the far side of the DDial link.
 *
 * A hash map keyed by handle (case-insensitive) holds everyone the link has
 * shown us.  It is fed with normalized relay lines -- the same text that
 * goes into Chatter history -- so it can be rebuilt from history at start-up
 * (the last DDIAL_ROSTER_HISTORY_SCAN entries) and then kept current live:
 *
 *   station user list  "[SYSOP] 73-.SynerChat #0[T1:AriZona Mae #2<T1:Joe
 *                       Boo:232 #3[T1:bassboy:057"  (link slot, '.'/',' lock
 *                       flag, station name, then one entry per user; the
 *                       wire's '^' separators arrive as spaces).  Adds or
 *                       updates everyone listed and removes that station's
 *                       users who are no longer in its list.
 *   login / logout     "[LINK] -->. + #5[T1:Freddy:#123*" / "... - #5..."
 *   public chat        "[link]#slot<b>T<ch>:handle) message"
 *
 * Users are removed only when they leave: a logout event, or dropping out
 * of their station's list.  Guests ("?") are keyed by line number.
 *
 * Updates only take g_ddial_roster_lock (they run under the relay's
 * client->lock).  Local users (dial-ins and Chatter members the link echoes
 * back) are filtered out when the roster is read, outside that lock.
 */

#include "ssh_chatter/ddial_protocol.h"
#include "ssh_chatter/host.h"

#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DDIAL_ROSTER_BUCKETS 1024U /* power of two */
#define DDIAL_ROSTER_MAX 512U      /* live users; keeps the load factor low */
#define DDIAL_ROSTER_HISTORY_SCAN 2048U
#define DDIAL_ROSTER_KEY_LEN (DDIAL_MAX_HANDLE_LEN + 16U)

typedef struct ddial_roster_entry {
    char key[DDIAL_ROSTER_KEY_LEN]; /* lowercased handle, or "?#link#slot" */
    char handle[DDIAL_MAX_HANDLE_LEN];
    char station[DDIAL_MAX_HANDLE_LEN]; /* from a station list, if any */
    uint16_t link_slot;
    uint16_t slot;
} ddial_roster_entry_t;

enum {
    DDIAL_ROSTER_EMPTY = 0,
    DDIAL_ROSTER_USED,
    DDIAL_ROSTER_TOMBSTONE,
};

static pthread_mutex_t g_ddial_roster_lock = PTHREAD_MUTEX_INITIALIZER;
static ddial_roster_entry_t g_ddial_roster[DDIAL_ROSTER_BUCKETS];
static uint8_t g_ddial_roster_state[DDIAL_ROSTER_BUCKETS];
static size_t g_ddial_roster_count = 0U;

/* ---- map ---------------------------------------------------------------- */

static void ddial_roster_make_key(const char *handle, uint16_t link_slot,
                                  uint16_t slot, char *key, size_t cap)
{
    if (strcmp(handle, "?") == 0) {
        snprintf(key, cap, "?#%u#%u", (unsigned)link_slot, (unsigned)slot);
        return;
    }
    size_t i = 0U;
    for (; handle[i] != '\0' && i + 1U < cap; ++i) {
        key[i] = (char)tolower((unsigned char)handle[i]);
    }
    key[i] = '\0';
}

static size_t ddial_roster_hash(const char *key)
{
    uint32_t h = 2166136261U; /* FNV-1a */
    for (; *key != '\0'; ++key) {
        h ^= (unsigned char)*key;
        h *= 16777619U;
    }
    return (size_t)h & (DDIAL_ROSTER_BUCKETS - 1U);
}

/* Bucket holding key, or SIZE_MAX. */
static size_t ddial_roster_lookup_locked(const char *key)
{
    size_t idx = ddial_roster_hash(key);
    for (size_t probe = 0U; probe < DDIAL_ROSTER_BUCKETS; ++probe) {
        if (g_ddial_roster_state[idx] == DDIAL_ROSTER_EMPTY) {
            return SIZE_MAX;
        }
        if (g_ddial_roster_state[idx] == DDIAL_ROSTER_USED &&
            strcmp(g_ddial_roster[idx].key, key) == 0) {
            return idx;
        }
        idx = (idx + 1U) & (DDIAL_ROSTER_BUCKETS - 1U);
    }
    return SIZE_MAX;
}

static void ddial_roster_put_locked(const char *handle, uint16_t link_slot,
                                    uint16_t slot, const char *station)
{
    char key[DDIAL_ROSTER_KEY_LEN];
    ddial_roster_make_key(handle, link_slot, slot, key, sizeof(key));
    if (key[0] == '\0') {
        return;
    }
    size_t idx = ddial_roster_lookup_locked(key);
    if (idx == SIZE_MAX) {
        if (g_ddial_roster_count >= DDIAL_ROSTER_MAX) {
            return;
        }
        idx = ddial_roster_hash(key);
        while (g_ddial_roster_state[idx] == DDIAL_ROSTER_USED) {
            idx = (idx + 1U) & (DDIAL_ROSTER_BUCKETS - 1U);
        }
        memset(&g_ddial_roster[idx], 0, sizeof(g_ddial_roster[idx]));
        snprintf(g_ddial_roster[idx].key, sizeof(g_ddial_roster[idx].key), "%s",
                 key);
        g_ddial_roster_state[idx] = DDIAL_ROSTER_USED;
        ++g_ddial_roster_count;
    }
    ddial_roster_entry_t *e = &g_ddial_roster[idx];
    snprintf(e->handle, sizeof(e->handle), "%s", handle);
    e->link_slot = link_slot;
    e->slot = slot;
    /* Chat and login lines do not name the station; keep a known one. */
    if (station != nullptr) {
        snprintf(e->station, sizeof(e->station), "%s", station);
    }
}

static void ddial_roster_remove_at_locked(size_t idx)
{
    g_ddial_roster_state[idx] = DDIAL_ROSTER_TOMBSTONE;
    --g_ddial_roster_count;
}

static void ddial_roster_remove_locked(const char *handle, uint16_t link_slot,
                                       uint16_t slot)
{
    char key[DDIAL_ROSTER_KEY_LEN];
    ddial_roster_make_key(handle, link_slot, slot, key, sizeof(key));
    size_t idx = ddial_roster_lookup_locked(key);
    if (idx != SIZE_MAX) {
        ddial_roster_remove_at_locked(idx);
    }
}

/* ---- parsing ------------------------------------------------------------ */

/* True when p starts a user entry: "#<digits><bracket>T<digit>". */
static bool ddial_roster_is_entry(const char *p)
{
    if (*p != '#' || !isdigit((unsigned char)p[1])) {
        return false;
    }
    ++p;
    while (isdigit((unsigned char)*p)) {
        ++p;
    }
    return (*p == '(' || *p == '[' || *p == '<') && p[1] == 'T' &&
           isdigit((unsigned char)p[2]);
}

/* Next entry start at or after p, preceded by a space, '^' or p itself. */
static const char *ddial_roster_next_entry(const char *p)
{
    for (const char *q = p; *q != '\0'; ++q) {
        if (*q == '#' && (q == p || q[-1] == ' ' || q[-1] == '^') &&
            ddial_roster_is_entry(q)) {
            return q;
        }
    }
    return nullptr;
}

/* Parse "[link]#<slot><bo>T<ch>(:|=)<handle>[:acct][*]" starting at p.  The
 * handle may contain spaces; it ends at ':', '*', a closing bracket, '^' or
 * limit (the next entry), whichever comes first. */
static bool ddial_roster_parse_identity(const char *p, const char *limit,
                                        uint16_t *out_link, uint16_t *out_slot,
                                        bool *out_is_link, char *out_handle,
                                        size_t cap)
{
    uint16_t link_slot = 0U;
    if (isdigit((unsigned char)*p)) {
        char *end = nullptr;
        unsigned long v = strtoul(p, &end, 10);
        if (*end != '#' || v > UINT16_MAX) {
            return false;
        }
        link_slot = (uint16_t)v;
        p = end;
    }
    if (!ddial_roster_is_entry(p)) {
        return false;
    }
    char *end = nullptr;
    unsigned long slot = strtoul(p + 1, &end, 10);
    if (slot > UINT16_MAX) {
        return false;
    }
    p = end + 2; /* bracket, 'T' */
    while (isdigit((unsigned char)*p)) {
        ++p;
    }
    if (*p != ':' && *p != '=') {
        return false;
    }
    *out_is_link = (*p == '=');
    ++p;

    const char *start = p;
    while (*p != '\0' && (limit == nullptr || p < limit) &&
           strchr("^:*)]>", *p) == nullptr) {
        ++p;
    }
    while (p > start && p[-1] == ' ') {
        --p;
    }
    size_t len = (size_t)(p - start);
    if (len == 0U) {
        return false;
    }
    if (len >= cap) {
        len = cap - 1U;
    }
    memcpy(out_handle, start, len);
    out_handle[len] = '\0';
    *out_link = link_slot;
    *out_slot = (uint16_t)slot;
    return true;
}

/* "[<link>]-.<station> #0[T1:A #2<T1:B:232 ..." ("-," when locked). */
static void ddial_roster_note_station_list_locked(const char *p)
{
    uint16_t link_slot = 0U;
    if (isdigit((unsigned char)*p)) {
        char *end = nullptr;
        unsigned long v = strtoul(p, &end, 10);
        if (v > UINT16_MAX) {
            return;
        }
        link_slot = (uint16_t)v;
        p = end;
    }
    if (p[0] != '-' || (p[1] != '.' && p[1] != ',')) {
        return;
    }
    p += 2;
    const char *first = ddial_roster_next_entry(p);
    const char *name_end = first != nullptr ? first : p + strlen(p);
    while (name_end > p && (name_end[-1] == ' ' || name_end[-1] == '^')) {
        --name_end;
    }
    char station[DDIAL_MAX_HANDLE_LEN];
    size_t n = (size_t)(name_end - p);
    if (n == 0U) {
        return;
    }
    if (n >= sizeof(station)) {
        n = sizeof(station) - 1U;
    }
    memcpy(station, p, n);
    station[n] = '\0';
    /* Our own list coming back over the link describes local users. */
    const char *own = getenv("CHATTER_DDIAL_STATION");
    if (own != nullptr && strcasecmp(own, station) == 0) {
        return;
    }

    /* Collect this list's keys first. */
    char listed[DDIAL_ROSTER_MAX][DDIAL_ROSTER_KEY_LEN];
    size_t listed_count = 0U;
    for (const char *q = first; q != nullptr;) {
        const char *next = ddial_roster_next_entry(q + 1);
        uint16_t l = 0U;
        uint16_t s = 0U;
        bool is_link = false;
        char handle[DDIAL_MAX_HANDLE_LEN];
        if (listed_count < DDIAL_ROSTER_MAX &&
            ddial_roster_parse_identity(q, next, &l, &s, &is_link, handle,
                                        sizeof(handle)) &&
            !is_link) {
            ddial_roster_make_key(handle, link_slot, s, listed[listed_count],
                                  sizeof(listed[listed_count]));
            ddial_roster_put_locked(handle, link_slot, s, station);
            ++listed_count;
        }
        q = next;
    }

    /* Whoever this station listed before and no longer lists has left. */
    for (size_t i = 0U; i < DDIAL_ROSTER_BUCKETS; ++i) {
        if (g_ddial_roster_state[i] != DDIAL_ROSTER_USED ||
            strcasecmp(g_ddial_roster[i].station, station) != 0) {
            continue;
        }
        bool still_listed = false;
        for (size_t k = 0U; k < listed_count && !still_listed; ++k) {
            still_listed = strcmp(listed[k], g_ddial_roster[i].key) == 0;
        }
        if (!still_listed) {
            ddial_roster_remove_at_locked(i);
        }
    }
}

/* "-->. + #5[T1:Freddy:#123*" / "-->. - #6(T1:?" (caret may remain). */
static void ddial_roster_note_event_locked(const char *p)
{
    if (strncmp(p, "-->", 3U) != 0) {
        return;
    }
    p += 3;
    if (*p == '.' || *p == ',') {
        ++p;
    }
    while (*p == ' ') {
        ++p;
    }
    char sign = *p;
    if (sign != '+' && sign != '-') {
        return;
    }
    ++p;
    while (*p == ' ' || *p == '^') {
        ++p;
    }
    uint16_t link_slot = 0U;
    uint16_t slot = 0U;
    bool is_link = false;
    char handle[DDIAL_MAX_HANDLE_LEN];
    if (!ddial_roster_parse_identity(p, nullptr, &link_slot, &slot, &is_link,
                                     handle, sizeof(handle)) ||
        is_link) {
        return;
    }
    if (sign == '+') {
        ddial_roster_put_locked(handle, link_slot, slot, nullptr);
        return;
    }
    ddial_roster_remove_locked(handle, link_slot, slot);
}

static void ddial_roster_note_locked(const char *line)
{
    if (strncmp(line, "[SYSOP] ", 8U) == 0) {
        ddial_roster_note_station_list_locked(line + 8);
        return;
    }
    if (strncmp(line, "[LINK] ", 7U) == 0) {
        ddial_roster_note_event_locked(line + 7);
        return;
    }
    uint16_t link_slot = 0U;
    uint16_t slot = 0U;
    bool is_link = false;
    char handle[DDIAL_MAX_HANDLE_LEN];
    if (ddial_parse_incoming_chat(line, &link_slot, &slot, nullptr, &is_link,
                                  nullptr, handle, sizeof(handle), nullptr) &&
        !is_link && handle[0] != '\0') {
        ddial_roster_put_locked(handle, link_slot, slot, nullptr);
    }
}

/* ---- public ------------------------------------------------------------- */

/* Feed one normalized relay line (as it is stored in Chatter history). */
static void ddial_roster_note_line(const char *line)
{
    if (line == nullptr || line[0] == '\0') {
        return;
    }
    pthread_mutex_lock(&g_ddial_roster_lock);
    ddial_roster_note_locked(line);
    pthread_mutex_unlock(&g_ddial_roster_lock);
}

/* True for relay lines that only operate the link (station user lists,
 * login/logout events): the roster reads them, people do not need to. */
static bool ddial_roster_line_is_operational(const char *line)
{
    return line != nullptr && (strncmp(line, "[SYSOP] ", 8U) == 0 ||
                               strncmp(line, "[LINK] ", 7U) == 0);
}

/* Replay the last DDIAL_ROSTER_HISTORY_SCAN history entries, oldest first,
 * so users who were already online before this start are known. */
static void ddial_roster_seed_from_history(host_t *host)
{
    if (host == nullptr) {
        return;
    }
    ttak_mutex_lock(&host->lock);
    host_history_restore_cache_locked(host);
    size_t count = host->history_count;
    size_t start = count > DDIAL_ROSTER_HISTORY_SCAN
                       ? count - DDIAL_ROSTER_HISTORY_SCAN
                       : 0U;
    pthread_mutex_lock(&g_ddial_roster_lock);
    for (size_t i = start; i < count && host->history != nullptr; ++i) {
        const chat_history_entry_t *entry = &host->history[i];
        if (!entry->is_user_message) {
            ddial_roster_note_locked(entry->message);
        }
    }
    pthread_mutex_unlock(&g_ddial_roster_lock);
    ttak_mutex_unlock(&host->lock);
}

/* Copy the roster, minus users that are really ours (dial-ins and Chatter
 * members echoed back by the link) and minus exclude_handle (the relay's
 * own account in user mode).  Returns the number copied. */
static size_t ddial_roster_snapshot(host_t *host, const char *exclude_handle,
                                    ddial_roster_entry_t *out, size_t cap)
{
    ddial_roster_entry_t *copy =
        (ddial_roster_entry_t *)calloc(DDIAL_ROSTER_MAX, sizeof(*copy));
    if (copy == nullptr) {
        return 0U;
    }
    size_t count = 0U;
    pthread_mutex_lock(&g_ddial_roster_lock);
    for (size_t i = 0U; i < DDIAL_ROSTER_BUCKETS && count < DDIAL_ROSTER_MAX;
         ++i) {
        if (g_ddial_roster_state[i] == DDIAL_ROSTER_USED) {
            copy[count++] = g_ddial_roster[i];
        }
    }
    pthread_mutex_unlock(&g_ddial_roster_lock);

    size_t written = 0U;
    for (size_t i = 0U; i < count && written < cap; ++i) {
        const char *handle = copy[i].handle;
        if (strcmp(handle, "?") != 0 &&
            ((exclude_handle != nullptr && exclude_handle[0] != '\0' &&
              strcasecmp(handle, exclude_handle) == 0) ||
             host_ddial_handle_is_local(host, handle, true))) {
            continue;
        }
        out[written++] = copy[i];
    }
    free(copy);
    return written;
}

/* Line number of the remote user called handle.  Only users directly on the
 * linked station qualify: a /P cannot be addressed past a further link. */
static bool ddial_roster_find(host_t *host, const char *handle,
                              uint16_t *out_slot)
{
    if (handle == nullptr || handle[0] == '\0' || strcmp(handle, "?") == 0) {
        return false;
    }
    char key[DDIAL_ROSTER_KEY_LEN];
    ddial_roster_make_key(handle, 0U, 0U, key, sizeof(key));
    bool found = false;
    pthread_mutex_lock(&g_ddial_roster_lock);
    size_t idx = ddial_roster_lookup_locked(key);
    if (idx != SIZE_MAX && g_ddial_roster[idx].link_slot == 0U) {
        *out_slot = g_ddial_roster[idx].slot;
        found = true;
    }
    pthread_mutex_unlock(&g_ddial_roster_lock);
    if (found && host_ddial_handle_is_local(host, handle, true)) {
        return false;
    }
    return found;
}

/**
 * @file roster.c
 * @desc Estimated roster of the users on the far side of the DDial link.
 *
 * Nothing on the wire hands us a complete user list, so the roster is built
 * from what the link shows: public chat lines ("[link]#slot[T1:handle) msg")
 * and member/guest login/logout events ("}-->. +^#slot[T1:handle:#acct*",
 * "}}-->. -^#slot(T1:?").  Entries are keyed by (link slot, slot).  A
 * logout removes an entry; anything not seen for DDIAL_ROSTER_TTL_SEC is
 * dropped so a missed logout cannot count someone forever.
 *
 * Recording only takes g_ddial_roster_lock (it runs under the relay's
 * client->lock).  Local users (dial-ins and Chatter members echoed back by
 * the link) are filtered out when the roster is read, outside that lock.
 */

#include "ssh_chatter/ddial_protocol.h"
#include "ssh_chatter/host.h"

#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define DDIAL_ROSTER_MAX 256U
#define DDIAL_ROSTER_TTL_SEC (2 * 60 * 60)

typedef struct ddial_roster_entry {
    uint16_t link_slot;
    uint16_t slot;
    char handle[DDIAL_MAX_HANDLE_LEN];
    time_t seen;
} ddial_roster_entry_t;

static pthread_mutex_t g_ddial_roster_lock = PTHREAD_MUTEX_INITIALIZER;
static ddial_roster_entry_t g_ddial_roster[DDIAL_ROSTER_MAX];
static size_t g_ddial_roster_count = 0U;

static void ddial_roster_expire_locked(time_t now)
{
    size_t keep = 0U;
    for (size_t i = 0U; i < g_ddial_roster_count; ++i) {
        if (now - g_ddial_roster[i].seen > (time_t)DDIAL_ROSTER_TTL_SEC) {
            continue;
        }
        if (keep != i) {
            g_ddial_roster[keep] = g_ddial_roster[i];
        }
        ++keep;
    }
    g_ddial_roster_count = keep;
}

static void ddial_roster_seen(uint16_t link_slot, uint16_t slot,
                              const char *handle)
{
    if (slot == 0U || handle == nullptr || handle[0] == '\0') {
        return;
    }
    time_t now = time(nullptr);
    pthread_mutex_lock(&g_ddial_roster_lock);
    ddial_roster_expire_locked(now);

    ddial_roster_entry_t *entry = nullptr;
    for (size_t i = 0U; i < g_ddial_roster_count; ++i) {
        if (g_ddial_roster[i].link_slot == link_slot &&
            g_ddial_roster[i].slot == slot) {
            entry = &g_ddial_roster[i];
            break;
        }
    }
    if (entry == nullptr) {
        if (g_ddial_roster_count < DDIAL_ROSTER_MAX) {
            entry = &g_ddial_roster[g_ddial_roster_count++];
        } else {
            /* Full: reuse the stalest entry. */
            entry = &g_ddial_roster[0];
            for (size_t i = 1U; i < g_ddial_roster_count; ++i) {
                if (g_ddial_roster[i].seen < entry->seen) {
                    entry = &g_ddial_roster[i];
                }
            }
        }
        memset(entry, 0, sizeof(*entry));
        entry->link_slot = link_slot;
        entry->slot = slot;
    }
    /* A guest login only says "?"; keep a real handle once we have one. */
    if (strcmp(handle, "?") != 0 || entry->handle[0] == '\0') {
        snprintf(entry->handle, sizeof(entry->handle), "%s", handle);
    }
    entry->seen = now;
    pthread_mutex_unlock(&g_ddial_roster_lock);
}

static void ddial_roster_gone(uint16_t link_slot, uint16_t slot)
{
    pthread_mutex_lock(&g_ddial_roster_lock);
    for (size_t i = 0U; i < g_ddial_roster_count; ++i) {
        if (g_ddial_roster[i].link_slot == link_slot &&
            g_ddial_roster[i].slot == slot) {
            g_ddial_roster[i] = g_ddial_roster[--g_ddial_roster_count];
            break;
        }
    }
    pthread_mutex_unlock(&g_ddial_roster_lock);
}

/* Forget everyone, e.g. when the link drops. */
static void ddial_roster_clear(void)
{
    pthread_mutex_lock(&g_ddial_roster_lock);
    g_ddial_roster_count = 0U;
    pthread_mutex_unlock(&g_ddial_roster_lock);
}

/* Parse "[link]#<slot><bo>T<ch>(:|=)<handle>..." as used by login/logout
 * events.  Unlike chat lines there is no closing bracket: the handle ends
 * at '^', ':', '*', a bracket or the end of the line. */
static bool ddial_roster_parse_identity(const char *p, uint16_t *out_link,
                                        uint16_t *out_slot, bool *out_is_link,
                                        char *out_handle, size_t cap)
{
    uint16_t link_slot = 0U;
    if (isdigit((unsigned char)*p)) {
        char *end = nullptr;
        unsigned long v = strtoul(p, &end, 10);
        if (*end != '#' || v == 0U || v > UINT16_MAX) {
            return false;
        }
        link_slot = (uint16_t)v;
        p = end;
    }
    if (*p != '#' || !isdigit((unsigned char)p[1])) {
        return false;
    }
    char *end = nullptr;
    unsigned long slot = strtoul(p + 1, &end, 10);
    if (slot == 0U || slot > UINT16_MAX) {
        return false;
    }
    p = end;
    if (*p != '(' && *p != '[' && *p != '<') {
        return false;
    }
    ++p;
    if (*p != 'T' || !isdigit((unsigned char)p[1])) {
        return false;
    }
    p += 2;
    while (isdigit((unsigned char)*p)) {
        ++p;
    }
    if (*p != ':' && *p != '=') {
        return false;
    }
    *out_is_link = (*p == '=');
    ++p;

    const char *start = p;
    while (*p != '\0' && strchr("^:*)]>", *p) == nullptr) {
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

/* Feed one inbound link line.  raw is the terminal-sanitized wire line
 * (login/logout events keep their '}' prefix and '^' separators there);
 * normalized is the same line as it goes to Chatter history. */
static void ddial_roster_note_line(const char *raw, const char *normalized)
{
    if (raw != nullptr && raw[0] == '}') {
        const char *p = raw;
        while (*p == '}') {
            ++p;
        }
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
        if (*p == '^') {
            ++p;
        }
        uint16_t link_slot = 0U;
        uint16_t slot = 0U;
        bool is_link = false;
        char handle[DDIAL_MAX_HANDLE_LEN];
        if (!ddial_roster_parse_identity(p, &link_slot, &slot, &is_link, handle,
                                         sizeof(handle)) ||
            is_link) {
            return;
        }
        if (sign == '+') {
            ddial_roster_seen(link_slot, slot, handle);
        } else {
            ddial_roster_gone(link_slot, slot);
        }
        return;
    }

    uint16_t link_slot = 0U;
    uint16_t slot = 0U;
    bool is_link = false;
    char handle[DDIAL_MAX_HANDLE_LEN];
    if (normalized != nullptr &&
        ddial_parse_incoming_chat(normalized, &link_slot, &slot, nullptr,
                                  &is_link, nullptr, handle, sizeof(handle),
                                  nullptr) &&
        !is_link) {
        ddial_roster_seen(link_slot, slot, handle);
    }
}

/* Copy the live roster, minus users that are really ours (dial-ins and
 * Chatter members the link echoes back) and minus exclude_handle (the
 * relay's own account in user mode).  Returns the number copied. */
static size_t ddial_roster_snapshot(host_t *host, const char *exclude_handle,
                                    ddial_roster_entry_t *out, size_t cap)
{
    ddial_roster_entry_t copy[DDIAL_ROSTER_MAX];
    size_t count = 0U;
    pthread_mutex_lock(&g_ddial_roster_lock);
    ddial_roster_expire_locked(time(nullptr));
    count = g_ddial_roster_count;
    memcpy(copy, g_ddial_roster, count * sizeof(copy[0]));
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
    ddial_roster_entry_t entries[DDIAL_ROSTER_MAX];
    size_t count =
        ddial_roster_snapshot(host, nullptr, entries, DDIAL_ROSTER_MAX);
    for (size_t i = 0U; i < count; ++i) {
        if (entries[i].link_slot == 0U &&
            strcasecmp(entries[i].handle, handle) == 0) {
            *out_slot = entries[i].slot;
            return true;
        }
    }
    return false;
}

/**
 * @file magviz.c
 * @desc MagViz-compatible command layer for the Diversi Dial listener.
 *
 * Dial-in sessions keep the classic DDial wire look ("#3[T1:alice*] hi")
 * but accept the MagViz command set: /s, /p#, /t#, /h, /a, colours,
 * membership, mailboxes, message boxes, fun commands and the chat extras.
 * Commands that only make sense in MagViz's web client (fonts, browser
 * notifications, text-to-speech, remember-me) are stored as preferences so
 * a GUI front end can honour them, and say so on the terminal.
 *
 * Included into the host transport translation unit after server.c, so it
 * sees ddial_session_t and the server's static write helpers directly.
 */

#include "ssh_chatter/ddial_protocol.h"
#include "ssh_chatter/host.h"

#include <ctype.h>
#include <curl/curl.h>
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* Registry helpers implemented in integration.c (same translation unit). */
typedef void (*ddial_session_visit_cb)(ddial_session_t *sess, void *user);
static void host_ddial_foreach_session(host_t *host, ddial_session_visit_cb cb,
                                       void *user);
typedef void (*ddial_chat_link_visit_cb)(uint16_t slot, const char *handle,
                                         const char *username, void *user);
static void host_ddial_foreach_chat_link(host_t *host,
                                         ddial_chat_link_visit_cb cb,
                                         void *user);
static bool host_ddial_chat_link_username(host_t *host, uint16_t slot,
                                          char *out, size_t cap);
static uint16_t host_ddial_chat_link_slot_of(host_t *host,
                                             const char *username);

#define DDIAL_MV_LINK_CHANNEL(ch)                                             \
    ((uint8_t)(((ch) >= DDIAL_MIN_CHANNEL && (ch) <= DDIAL_MAX_CHANNEL)       \
                   ? (ch)                                                     \
                   : DDIAL_DEFAULT_CHANNEL))

#define DDIAL_MV_VISITORS 30U
#define DDIAL_MV_META_MAX 64U
#define DDIAL_MV_BOOT_MAX 64U
#define DDIAL_MV_VANISH_MAX 32U
#define DDIAL_MV_VANISH_SECONDS 30
#define DDIAL_MV_BOOT_SECONDS 600
#define DDIAL_MV_SENTRY_WINDOW 10
#define DDIAL_MV_SENTRY_LIMIT 6U

/* ---- station-wide state ------------------------------------------------ */

typedef struct ddial_mv_channel_meta {
    uint16_t channel;
    char topic[160];
    char question[256];
    char asker[DDIAL_MAX_HANDLE_LEN];
} ddial_mv_channel_meta_t;

typedef struct ddial_mv_boot {
    uint16_t channel;
    char handle[DDIAL_MAX_HANDLE_LEN];
    time_t until;
} ddial_mv_boot_t;

typedef struct ddial_mv_visit {
    char handle[DDIAL_MAX_HANDLE_LEN];
    uint32_t member;
    time_t when;
    bool logon;
} ddial_mv_visit_t;

typedef struct ddial_mv_vanish {
    host_t *host;
    uint16_t channel;
    time_t due;
    char handle[DDIAL_MAX_HANDLE_LEN];
    char room_line[SSH_CHATTER_MESSAGE_LIMIT];
} ddial_mv_vanish_t;

static pthread_mutex_t g_mv_lock = PTHREAD_MUTEX_INITIALIZER;
static ddial_mv_channel_meta_t g_mv_meta[DDIAL_MV_META_MAX];
static ddial_mv_boot_t g_mv_boots[DDIAL_MV_BOOT_MAX];
static ddial_mv_visit_t g_mv_visitors[DDIAL_MV_VISITORS];
static size_t g_mv_visitor_head = 0U;
static size_t g_mv_visitor_count = 0U;
static ddial_mv_vanish_t g_mv_vanish[DDIAL_MV_VANISH_MAX];
static bool g_mv_sentry = false;

/* ---- small helpers ----------------------------------------------------- */

static void mv_line(ddial_session_t *sess, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void mv_line(ddial_session_t *sess, const char *fmt, ...)
{
    char buf[SSH_CHATTER_MESSAGE_LIMIT];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ddial_session_write_line(sess, buf);
}

static void mv_lines(ddial_session_t *sess, const char *const *lines)
{
    for (size_t i = 0U; lines[i] != nullptr; ++i) {
        ddial_session_write_line(sess, lines[i]);
    }
}

static const char *mv_skip_ws(const char *p)
{
    while (p != nullptr && (*p == ' ' || *p == '\t')) {
        ++p;
    }
    return p != nullptr ? p : "";
}

static bool mv_is_member(const ddial_session_t *sess)
{
    return sess->mv.member_no != 0U;
}

static bool mv_require_member(ddial_session_t *sess)
{
    if (mv_is_member(sess)) {
        return true;
    }
    mv_line(sess, "* That command is for members. Type /signup <password> "
                  "to join (it's free).");
    return false;
}

static void mv_bell(ddial_session_t *sess, uint8_t event)
{
    if (sess->mv.beep_on && sess->mv.beep_volume > 0U &&
        (sess->mv.beep_events & event) != 0U) {
        ddial_session_write_raw(sess, "\a", 1U);
    }
}

static const char *mv_handle(const ddial_session_t *sess)
{
    return sess->mv.display_handle[0] != '\0' ? sess->mv.display_handle
                                              : sess->handle;
}

static ddial_user_tier_t mv_tier(const ddial_session_t *sess)
{
    return mv_is_member(sess) ? DDIAL_TIER_PASSWORD : DDIAL_TIER_GUEST;
}

/* Parse a decimal number at *p, advancing it.  Returns false if none. */
static bool mv_parse_uint(const char **p, uint32_t *out)
{
    const char *s = *p;
    if (!isdigit((unsigned char)*s)) {
        return false;
    }
    unsigned long value = 0UL;
    while (isdigit((unsigned char)*s)) {
        value = value * 10UL + (unsigned long)(*s - '0');
        if (value > 0xFFFFFFFFUL) {
            return false;
        }
        ++s;
    }
    *p = s;
    *out = (uint32_t)value;
    return true;
}

static bool mv_list_contains(const ddial_mv_slot_list_t *list, uint32_t v)
{
    for (size_t i = 0U; i < list->count; ++i) {
        if (list->items[i] == v) {
            return true;
        }
    }
    return false;
}

static bool mv_list_add(ddial_mv_slot_list_t *list, uint32_t v)
{
    if (mv_list_contains(list, v)) {
        return true;
    }
    if (list->count >= DDIAL_MV_SLOT_LIST) {
        return false;
    }
    list->items[list->count++] = v;
    return true;
}

static bool mv_list_remove(ddial_mv_slot_list_t *list, uint32_t v)
{
    for (size_t i = 0U; i < list->count; ++i) {
        if (list->items[i] == v) {
            list->items[i] = list->items[--list->count];
            return true;
        }
    }
    return false;
}

/* Returns true when the value is now in the list. */
static bool mv_list_toggle(ddial_mv_slot_list_t *list, uint32_t v)
{
    if (mv_list_remove(list, v)) {
        return false;
    }
    return mv_list_add(list, v);
}

/* Parse "#,#,#" into list.  Returns number parsed and advances *p. */
static size_t mv_parse_slot_csv(const char **p, ddial_mv_slot_list_t *list)
{
    list->count = 0U;
    const char *s = *p;
    for (;;) {
        uint32_t v = 0U;
        if (!mv_parse_uint(&s, &v)) {
            break;
        }
        if (v != 0U) {
            mv_list_add(list, v);
        }
        if (*s != ',') {
            break;
        }
        ++s;
    }
    *p = s;
    return list->count;
}

static void mv_format_time(time_t when, char *buf, size_t cap)
{
    struct tm tm_value;
    localtime_r(&when, &tm_value);
    strftime(buf, cap, "%Y-%m-%d %H:%M", &tm_value);
}

static bool mv_valid_date(const char *s, int *y, int *m, int *d)
{
    if (s == nullptr || strlen(s) != 10U || s[4] != '-' || s[7] != '-') {
        return false;
    }
    if (sscanf(s, "%4d-%2d-%2d", y, m, d) != 3) {
        return false;
    }
    return *y >= 1900 && *y <= 2100 && *m >= 1 && *m <= 12 && *d >= 1 &&
           *d <= 31;
}

/* ---- colour rendering -------------------------------------------------- */

typedef struct mv_rgb {
    uint8_t r, g, b;
} mv_rgb_t;

static int mv_hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static bool mv_hex6(const char *s, mv_rgb_t *out)
{
    int v[6];
    for (int i = 0; i < 6; ++i) {
        v[i] = mv_hexval(s[i]);
        if (v[i] < 0) {
            return false;
        }
    }
    out->r = (uint8_t)(v[0] * 16 + v[1]);
    out->g = (uint8_t)(v[2] * 16 + v[3]);
    out->b = (uint8_t)(v[4] * 16 + v[5]);
    return true;
}

/* Map a 24-bit colour onto the xterm-256 palette so output stays within the
 * ANSI palette retro terminals understand. */
static unsigned mv_ansi256(mv_rgb_t c)
{
    int max = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
    int min = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);
    if (max - min < 12) {
        if (max < 8) {
            return 16U;
        }
        if (max > 238) {
            return 231U;
        }
        return 232U + (unsigned)((max - 8) / 10);
    }
    unsigned r = (unsigned)((c.r * 5 + 127) / 255);
    unsigned g = (unsigned)((c.g * 5 + 127) / 255);
    unsigned b = (unsigned)((c.b * 5 + 127) / 255);
    return 16U + 36U * r + 6U * g + b;
}

static mv_rgb_t mv_hue(double t)
{
    double h = fmod(t, 1.0) * 6.0;
    double x = 1.0 - fabs(fmod(h, 2.0) - 1.0);
    double r = 0.0, g = 0.0, b = 0.0;
    switch ((int)h) {
    case 0: r = 1.0; g = x; break;
    case 1: r = x; g = 1.0; break;
    case 2: g = 1.0; b = x; break;
    case 3: g = x; b = 1.0; break;
    case 4: r = x; b = 1.0; break;
    default: r = 1.0; b = x; break;
    }
    mv_rgb_t out = {(uint8_t)(r * 255.0), (uint8_t)(g * 255.0),
                    (uint8_t)(b * 255.0)};
    return out;
}

typedef enum mv_color_mode {
    MV_COLOR_NONE = 0,
    MV_COLOR_SOLID,
    MV_COLOR_GRADIENT,
    MV_COLOR_RAINBOW,
    MV_COLOR_SPARKLE,
    MV_COLOR_WAVE,
} mv_color_mode_t;

typedef struct mv_out {
    char *buf;
    size_t cap;
    size_t len;
} mv_out_t;

static void mv_out_put(mv_out_t *o, const char *s)
{
    size_t n = strlen(s);
    if (o->len + n + 1U > o->cap) {
        n = o->cap > o->len + 1U ? o->cap - o->len - 1U : 0U;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = '\0';
}

static void mv_out_putn(mv_out_t *o, const char *s, size_t n)
{
    if (o->len + n + 1U > o->cap) {
        n = o->cap > o->len + 1U ? o->cap - o->len - 1U : 0U;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = '\0';
}

static bool mv_is_marker(const char *p, size_t *skip, char *kind,
                         mv_rgb_t *rgb)
{
    if (p[0] == '\\' && (p[1] == 'i' || p[1] == 'b' || p[1] == 'u')) {
        *skip = 2U;
        *kind = p[1];
        return true;
    }
    if (p[0] == '#' && strlen(p) >= 7U && mv_hex6(p + 1, rgb)) {
        *skip = 7U;
        *kind = '#';
        return true;
    }
    return false;
}

/* Render text with a colour mode, inline markers (\i \b \u and #RRGGBB)
 * and produce both an ANSI version and a plain version (markers removed). */
static void mv_render(const char *text, mv_color_mode_t mode, mv_rgb_t a,
                      mv_rgb_t b, char *styled, size_t styled_cap,
                      char *plain, size_t plain_cap)
{
    size_t visible = 0U;
    for (const char *p = text; *p != '\0';) {
        size_t skip = 0U;
        char kind = 0;
        mv_rgb_t rgb;
        if (mv_is_marker(p, &skip, &kind, &rgb)) {
            p += skip;
            continue;
        }
        if (((unsigned char)*p & 0xC0U) != 0x80U && *p != ' ') {
            ++visible;
        }
        ++p;
    }

    mv_out_t so = {styled, styled_cap, 0U};
    mv_out_t po = {plain, plain_cap, 0U};
    styled[0] = '\0';
    plain[0] = '\0';

    bool italic = false, bold = false, underline = false;
    bool manual = false;
    size_t index = 0U;
    unsigned last_color = 9999U;
    char sgr[32];

    if (mode == MV_COLOR_SOLID) {
        snprintf(sgr, sizeof(sgr), "\033[38;5;%um", mv_ansi256(a));
        mv_out_put(&so, sgr);
    }

    for (const char *p = text; *p != '\0';) {
        size_t skip = 0U;
        char kind = 0;
        mv_rgb_t rgb;
        if (mv_is_marker(p, &skip, &kind, &rgb)) {
            switch (kind) {
            case 'i':
                italic = !italic;
                mv_out_put(&so, italic ? "\033[3m" : "\033[23m");
                break;
            case 'b':
                bold = !bold;
                mv_out_put(&so, bold ? "\033[1m" : "\033[22m");
                break;
            case 'u':
                underline = !underline;
                mv_out_put(&so, underline ? "\033[4m" : "\033[24m");
                break;
            default:
                manual = true;
                snprintf(sgr, sizeof(sgr), "\033[38;5;%um", mv_ansi256(rgb));
                mv_out_put(&so, sgr);
                break;
            }
            p += skip;
            continue;
        }

        bool lead = ((unsigned char)*p & 0xC0U) != 0x80U;
        if (lead && *p != ' ' && !manual && mode != MV_COLOR_NONE &&
            mode != MV_COLOR_SOLID) {
            mv_rgb_t c = a;
            double t = visible > 1U ? (double)index / (double)(visible - 1U)
                                    : 0.0;
            if (mode == MV_COLOR_GRADIENT) {
                c.r = (uint8_t)(a.r + (b.r - a.r) * t);
                c.g = (uint8_t)(a.g + (b.g - a.g) * t);
                c.b = (uint8_t)(a.b + (b.b - a.b) * t);
            } else if (mode == MV_COLOR_RAINBOW) {
                c = mv_hue(t * 0.85);
            } else if (mode == MV_COLOR_SPARKLE) {
                c = mv_hue((double)(rand() % 360) / 360.0);
            } else if (mode == MV_COLOR_WAVE) {
                double w = 0.5 + 0.5 * sin((double)index * 0.9);
                c.r = 0;
                c.g = (uint8_t)(120 + 135 * w);
                c.b = 255;
            }
            unsigned idx = mv_ansi256(c);
            if (idx != last_color) {
                snprintf(sgr, sizeof(sgr), "\033[38;5;%um", idx);
                mv_out_put(&so, sgr);
                last_color = idx;
            }
            ++index;
        } else if (lead && *p != ' ') {
            ++index;
        }
        mv_out_putn(&so, p, 1U);
        mv_out_putn(&po, p, 1U);
        ++p;
    }
    if (so.len > 0U) {
        mv_out_put(&so, "\033[0m");
    }
}

/* Parse "RRGGBB" or "RRGGBB,RRGGBB" into a colour mode. */
static mv_color_mode_t mv_parse_color_spec(const char *spec, mv_rgb_t *a,
                                           mv_rgb_t *b)
{
    if (spec == nullptr || spec[0] == '\0') {
        return MV_COLOR_NONE;
    }
    if (spec[0] == '#') {
        ++spec;
    }
    if (!mv_hex6(spec, a)) {
        return MV_COLOR_NONE;
    }
    const char *comma = spec + 6;
    if (*comma == ',') {
        ++comma;
        if (*comma == '#') {
            ++comma;
        }
        if (mv_hex6(comma, b)) {
            return MV_COLOR_GRADIENT;
        }
    }
    *b = *a;
    return MV_COLOR_SOLID;
}

/* Render a /h#RRGGBBName spec: fills display (ANSI) and plain handle. */
static void mv_render_handle(const char *spec, char *display, size_t dcap,
                             char *plain, size_t pcap)
{
    mv_rgb_t none = {0, 0, 0};
    mv_render(spec, MV_COLOR_NONE, none, none, display, dcap, plain, pcap);
}

static void mv_strip_ansi(const char *src, char *dst, size_t cap)
{
    ddial_strip_ansi(src, strlen(src), dst, cap);
}

/* ---- station state ------------------------------------------------------ */

static ddial_mv_channel_meta_t *mv_meta_locked(uint16_t channel, bool create)
{
    ddial_mv_channel_meta_t *empty = nullptr;
    for (size_t i = 0U; i < DDIAL_MV_META_MAX; ++i) {
        if (g_mv_meta[i].channel == channel) {
            return &g_mv_meta[i];
        }
        if (empty == nullptr && g_mv_meta[i].channel == 0U) {
            empty = &g_mv_meta[i];
        }
    }
    if (create && empty != nullptr) {
        memset(empty, 0, sizeof(*empty));
        empty->channel = channel;
        return empty;
    }
    return nullptr;
}

static void mv_record_visit(const ddial_session_t *sess, bool logon)
{
    pthread_mutex_lock(&g_mv_lock);
    ddial_mv_visit_t *v = &g_mv_visitors[g_mv_visitor_head];
    snprintf(v->handle, sizeof(v->handle), "%s", sess->handle);
    v->member = sess->mv.member_no;
    v->when = time(nullptr);
    v->logon = logon;
    g_mv_visitor_head = (g_mv_visitor_head + 1U) % DDIAL_MV_VISITORS;
    if (g_mv_visitor_count < DDIAL_MV_VISITORS) {
        ++g_mv_visitor_count;
    }
    pthread_mutex_unlock(&g_mv_lock);
}

static bool mv_is_booted(uint16_t channel, const char *handle)
{
    bool booted = false;
    time_t now = time(nullptr);
    pthread_mutex_lock(&g_mv_lock);
    for (size_t i = 0U; i < DDIAL_MV_BOOT_MAX; ++i) {
        if (g_mv_boots[i].channel == channel && g_mv_boots[i].until > now &&
            strcasecmp(g_mv_boots[i].handle, handle) == 0) {
            booted = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_mv_lock);
    return booted;
}

static void mv_add_boot(uint16_t channel, const char *handle)
{
    time_t now = time(nullptr);
    pthread_mutex_lock(&g_mv_lock);
    size_t pick = 0U;
    for (size_t i = 0U; i < DDIAL_MV_BOOT_MAX; ++i) {
        if (g_mv_boots[i].until <= now) {
            pick = i;
            break;
        }
    }
    g_mv_boots[pick].channel = channel;
    snprintf(g_mv_boots[pick].handle, sizeof(g_mv_boots[pick].handle), "%s",
             handle);
    g_mv_boots[pick].until = now + DDIAL_MV_BOOT_SECONDS;
    pthread_mutex_unlock(&g_mv_lock);
}

static bool mv_env_cosysop(uint32_t member_no)
{
    const char *env = getenv("CHATTER_DDIAL_COSYSOPS");
    if (env == nullptr || member_no == 0U) {
        return false;
    }
    const char *p = env;
    while (*p != '\0') {
        while (*p != '\0' && !isdigit((unsigned char)*p)) {
            ++p;
        }
        uint32_t v = 0U;
        if (!mv_parse_uint(&p, &v)) {
            break;
        }
        if (v == member_no) {
            return true;
        }
    }
    return false;
}

/* ---- session lookup ----------------------------------------------------- */

typedef struct mv_find {
    uint16_t slot;
    uint32_t member;
    const char *handle;
    ddial_session_t *found;
} mv_find_t;

static void mv_find_cb(ddial_session_t *sess, void *user)
{
    mv_find_t *f = (mv_find_t *)user;
    if (f->found != nullptr || !sess->logged_in) {
        return;
    }
    if ((f->slot != 0U && sess->slot == f->slot) ||
        (f->member != 0U && sess->mv.member_no == f->member) ||
        (f->handle != nullptr && strcasecmp(sess->handle, f->handle) == 0)) {
        f->found = sess;
    }
}

/* The returned pointer is only a hint for reading simple fields; sessions
 * are GC-managed and validated by the registry on every visit. */
static ddial_session_t *mv_find_slot(host_t *host, uint16_t slot)
{
    mv_find_t f = {slot, 0U, nullptr, nullptr};
    host_ddial_foreach_session(host, mv_find_cb, &f);
    return f.found;
}

static ddial_session_t *mv_find_member(host_t *host, uint32_t member)
{
    mv_find_t f = {0U, member, nullptr, nullptr};
    host_ddial_foreach_session(host, mv_find_cb, &f);
    return f.found;
}

static ddial_session_t *mv_find_handle(host_t *host, const char *handle)
{
    mv_find_t f = {0U, 0U, handle, nullptr};
    host_ddial_foreach_session(host, mv_find_cb, &f);
    return f.found;
}

/* Resolve a line number to a handle across dial-ins and Chatter members. */
static bool mv_slot_handle(host_t *host, uint16_t slot, char *out, size_t cap)
{
    ddial_session_t *target = mv_find_slot(host, slot);
    if (target != nullptr) {
        snprintf(out, cap, "%s", target->handle);
        return true;
    }
    char username[SSH_CHATTER_USERNAME_LEN];
    if (host_ddial_chat_link_username(host, slot, username, sizeof(username))) {
        snprintf(out, cap, "%s", username);
        return true;
    }
    return false;
}

/* ---- public delivery ---------------------------------------------------- */

typedef struct ddial_mv_public {
    uint16_t channel;   /* 0 = every channel */
    uint16_t from_slot; /* 0 = system / remote */
    uint32_t from_member;
    const char *line;       /* rendered line, no CRLF */
    const char *plain_body; /* for @mention detection, may be nullptr */
    bool members_only;
    bool system;
    uint16_t pace_baud;
} ddial_mv_public_t;

static bool mv_mentions(const char *body, const char *handle)
{
    if (body == nullptr || handle == nullptr || handle[0] == '\0') {
        return false;
    }
    size_t hlen = strlen(handle);
    for (const char *p = strchr(body, '@'); p != nullptr;
         p = strchr(p + 1, '@')) {
        if (strncasecmp(p + 1, handle, hlen) == 0 &&
            !isalnum((unsigned char)p[1 + hlen])) {
            return true;
        }
    }
    return false;
}

static void mv_add_mention(ddial_session_t *sess, const char *text)
{
    char stamp[8];
    time_t now = time(nullptr);
    struct tm tm_value;
    localtime_r(&now, &tm_value);
    strftime(stamp, sizeof(stamp), "%H:%M", &tm_value);
    char plain[SSH_CHATTER_MESSAGE_LIMIT];
    mv_strip_ansi(text, plain, sizeof(plain));
    snprintf(sess->mv.mentions[sess->mv.mention_head],
             sizeof(sess->mv.mentions[0]), "%s %s", stamp, plain);
    sess->mv.mention_head = (sess->mv.mention_head + 1U) % DDIAL_MV_MENTION_LOG;
    if (sess->mv.mention_count < DDIAL_MV_MENTION_LOG) {
        ++sess->mv.mention_count;
    }
}

static void mv_deliver_cb(ddial_session_t *rec, void *user)
{
    const ddial_mv_public_t *pub = (const ddial_mv_public_t *)user;
    if (!rec->logged_in) {
        return;
    }
    bool on_channel = pub->channel == 0U || rec->channel == pub->channel;
    bool monitored =
        !on_channel && mv_list_contains(&rec->mv.monitored, pub->channel);
    if (!on_channel && !monitored) {
        return;
    }
    if (pub->members_only && !mv_is_member(rec)) {
        return;
    }
    bool own = pub->from_slot != 0U && pub->from_slot == rec->slot;
    bool mentioned = false;
    if (!pub->system && !own) {
        if (pub->from_slot != 0U &&
            mv_list_contains(&rec->mv.ignore_slots, pub->from_slot)) {
            return;
        }
        if (pub->from_member != 0U &&
            mv_list_contains(&rec->mv.ignore_members, pub->from_member)) {
            return;
        }
        mentioned = mv_mentions(pub->plain_body, rec->handle);
        if (!rec->mv.replies_on && pub->plain_body != nullptr &&
            pub->plain_body[0] == '@' && !mentioned) {
            return;
        }
    }

    char text[SSH_CHATTER_MESSAGE_LIMIT + 32];
    const char *line = pub->line;
    char stripped[SSH_CHATTER_MESSAGE_LIMIT];
    if (pub->from_slot != 0U &&
        mv_list_contains(&rec->mv.null_slots, pub->from_slot)) {
        mv_strip_ansi(pub->line, stripped, sizeof(stripped));
        line = stripped;
    }
    int n;
    if (monitored) {
        n = snprintf(text, sizeof(text), "[T%u] %s\r\n",
                     (unsigned)pub->channel, line);
    } else {
        n = snprintf(text, sizeof(text), "%s\r\n", line);
    }
    if (n <= 0) {
        return;
    }
    size_t len = (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1U;
    if (pub->pace_baud != 0U) {
        ddial_session_write_paced(rec, text, len, pub->pace_baud);
    } else {
        ddial_session_write_raw(rec, text, len);
    }

    if (mentioned) {
        mv_add_mention(rec, pub->line);
        mv_bell(rec, DDIAL_MV_BEEP_MENTION);
    } else if (!pub->system && !own) {
        mv_bell(rec, DDIAL_MV_BEEP_CHAT);
    }
}

static void host_ddial_deliver_public(host_t *host,
                                      const ddial_mv_public_t *pub)
{
    if (host == nullptr || pub == nullptr || pub->line == nullptr) {
        return;
    }
    host_ddial_foreach_session(host, mv_deliver_cb, (void *)pub);
}

static void mv_notice_channel(ddial_session_t *sess, uint16_t channel,
                              const char *text)
{
    ddial_mv_public_t pub = {0};
    pub.channel = channel;
    pub.line = text;
    pub.system = true;
    host_ddial_deliver_public(sess->owner, &pub);
}

/* Record a line into Chatter room history (system entry, so the room's
 * broadcast path does not loop it back into DDial). */
static void mv_share_room(host_t *host, const char *line)
{
    chat_history_entry_t stored = {0};
    if (host_history_record_system(host, line, &stored)) {
        chat_room_broadcast_entry(&host->room, &stored, nullptr);
    }
}

static void mv_room_forget(host_t *host, const char *line)
{
    ttak_mutex_lock(&host->lock);
    for (size_t i = host->history_count; i > 0U; --i) {
        size_t idx = i - 1U;
        chat_history_entry_t *entry = &host->history[idx];
        if (!entry->is_user_message && strcmp(entry->message, line) == 0) {
            if (idx + 1U < host->history_count) {
                memmove(host->history + idx, host->history + idx + 1U,
                        (host->history_count - idx - 1U) *
                            sizeof(chat_history_entry_t));
            }
            host->history_count--;
            break;
        }
    }
    ttak_mutex_unlock(&host->lock);
}

/* ---- public chat pipeline ---------------------------------------------- */

typedef struct mv_chat_opts {
    bool local_only;   /* /ps: no link, no Chatter room */
    bool members_only; /* /pv */
    bool action;       /* /a */
    bool uno;          /* /u */
    bool block;        /* /code */
    bool no_transform; /* taco, 8ball, giphy results */
    int anim;          /* /anim# 1-5 */
    uint16_t pace_baud;/* /300 /1200 /2400 */
} mv_chat_opts_t;

static const char *const k_mv_lorem[] = {
    "lorem", "ipsum", "dolor", "sit", "amet", "consectetur", "adipiscing",
    "elit", "sed", "do", "eiusmod", "tempor", "incididunt", "ut", "labore",
    "et", "dolore", "magna", "aliqua", "enim", "minim", "veniam", "quis",
    "nostrud", "exercitation", "ullamco", "laboris", "nisi", "aliquip",
};

static void mv_lorem(const char *src, char *dst, size_t cap)
{
    mv_out_t o = {dst, cap, 0U};
    dst[0] = '\0';
    bool in_word = false;
    for (const char *p = src; *p != '\0'; ++p) {
        if (isspace((unsigned char)*p)) {
            mv_out_putn(&o, p, 1U);
            in_word = false;
        } else if (!in_word) {
            mv_out_put(&o, k_mv_lorem[(size_t)rand() %
                                      (sizeof(k_mv_lorem) /
                                       sizeof(k_mv_lorem[0]))]);
            in_word = true;
        }
    }
}

static void mv_reverse(char *s)
{
    size_t n = strlen(s);
    /* Reverse by UTF-8 code point so multibyte text survives. */
    char tmp[DDIAL_MV_TEXT_LIMIT * 2U];
    if (n >= sizeof(tmp)) {
        return;
    }
    size_t out = 0U;
    size_t end = n;
    while (end > 0U) {
        size_t start = end - 1U;
        while (start > 0U && ((unsigned char)s[start] & 0xC0U) == 0x80U) {
            --start;
        }
        memcpy(tmp + out, s + start, end - start);
        out += end - start;
        end = start;
    }
    memcpy(s, tmp, out);
    s[out] = '\0';
}

static void mv_scramble(char *s)
{
    /* Shuffle the inner ASCII letters of each word; keep first/last. */
    size_t n = strlen(s);
    size_t i = 0U;
    while (i < n) {
        while (i < n && !isalpha((unsigned char)s[i])) {
            ++i;
        }
        size_t start = i;
        while (i < n && isalpha((unsigned char)s[i])) {
            ++i;
        }
        size_t len = i - start;
        if (len > 3U) {
            for (size_t k = len - 2U; k > 1U; --k) {
                size_t j = 1U + (size_t)rand() % k;
                char t = s[start + k];
                s[start + k] = s[start + j];
                s[start + j] = t;
            }
        }
    }
}

static void mv_alternate_case(char *s)
{
    bool upper = true;
    for (char *p = s; *p != '\0'; ++p) {
        if (isalpha((unsigned char)*p)) {
            *p = upper ? (char)toupper((unsigned char)*p)
                       : (char)tolower((unsigned char)*p);
            upper = !upper;
        }
    }
}

/* Replace "@<line#>" with "@<handle>" so mentions read naturally. */
static void mv_resolve_mentions(host_t *host, const char *src, char *dst,
                                size_t cap)
{
    mv_out_t o = {dst, cap, 0U};
    dst[0] = '\0';
    for (const char *p = src; *p != '\0';) {
        if (*p == '@' && isdigit((unsigned char)p[1])) {
            const char *q = p + 1;
            uint32_t slot = 0U;
            char handle[SSH_CHATTER_USERNAME_LEN];
            if (mv_parse_uint(&q, &slot) && slot <= UINT16_MAX &&
                mv_slot_handle(host, (uint16_t)slot, handle, sizeof(handle))) {
                mv_out_put(&o, "@");
                mv_out_put(&o, handle);
                p = q;
                continue;
            }
        }
        mv_out_putn(&o, p, 1U);
        ++p;
    }
}

static bool mv_sentry_check(ddial_session_t *sess)
{
    if (!g_mv_sentry || mv_is_member(sess)) {
        return true;
    }
    time_t now = time(nullptr);
    if (now - sess->mv.sentry_window_start > DDIAL_MV_SENTRY_WINDOW) {
        sess->mv.sentry_window_start = now;
        sess->mv.sentry_window_count = 0U;
    }
    if (++sess->mv.sentry_window_count > DDIAL_MV_SENTRY_LIMIT) {
        sess->mv.full_muted = true;
        mv_line(sess, "* Sentry: you have been muted for flooding.");
        char note[128];
        snprintf(note, sizeof(note), "* Sentry muted line #%u (%s).",
                 (unsigned)sess->slot, sess->handle);
        mv_notice_channel(sess, 0U, note);
        return false;
    }
    return true;
}

static void mv_schedule_vanish(host_t *host, uint16_t channel,
                               const char *handle, const char *room_line)
{
    pthread_mutex_lock(&g_mv_lock);
    for (size_t i = 0U; i < DDIAL_MV_VANISH_MAX; ++i) {
        if (g_mv_vanish[i].host == nullptr) {
            g_mv_vanish[i].host = host;
            g_mv_vanish[i].channel = channel;
            g_mv_vanish[i].due = time(nullptr) + DDIAL_MV_VANISH_SECONDS;
            snprintf(g_mv_vanish[i].handle, sizeof(g_mv_vanish[i].handle),
                     "%s", handle);
            snprintf(g_mv_vanish[i].room_line,
                     sizeof(g_mv_vanish[i].room_line), "%s",
                     room_line != nullptr ? room_line : "");
            break;
        }
    }
    pthread_mutex_unlock(&g_mv_lock);
}

static void mv_public_chat(ddial_session_t *sess, uint16_t channel,
                           const char *raw, const mv_chat_opts_t *opts)
{
    static const mv_chat_opts_t k_defaults = {0};
    if (opts == nullptr) {
        opts = &k_defaults;
    }
    raw = mv_skip_ws(raw);
    if (raw[0] == '\0' || sess->owner == nullptr) {
        return;
    }
    if (sess->mv.full_muted) {
        mv_line(sess, "* You are muted.");
        return;
    }
    if (!mv_sentry_check(sess)) {
        return;
    }
    host_t *host = sess->owner;

    char body[DDIAL_MV_TEXT_LIMIT * 2U];
    mv_color_mode_t mode = MV_COLOR_NONE;
    mv_rgb_t ca = {0, 0, 0}, cb = {0, 0, 0};
    bool vanish = false;

    if (opts->block || opts->no_transform) {
        snprintf(body, sizeof(body), "%s", raw);
    } else {
        /* Leading-character extras. */
        if (raw[0] == '`' && raw[1] != '\0') {
            snprintf(body, sizeof(body), "%s", raw + 1);
            mv_alternate_case(body);
        } else if (raw[0] == '~' && raw[1] != '\0') {
            snprintf(body, sizeof(body), "%s", raw + 1);
            mode = MV_COLOR_RAINBOW;
        } else if (raw[0] == ',' && raw[1] != '\0') {
            snprintf(body, sizeof(body), "%s", raw + 1);
            vanish = true;
        } else {
            snprintf(body, sizeof(body), "%s", raw);
        }

        if (sess->mv.lorem_on) {
            char tmp[sizeof(body)];
            mv_lorem(body, tmp, sizeof(tmp));
            snprintf(body, sizeof(body), "%s", tmp);
        }
        if (sess->mv.reverse_on) {
            mv_reverse(body);
        }
        if (sess->mv.scramble_on) {
            mv_scramble(body);
        }
        if (!opts->action && (sess->mv.prefix[0] != '\0' ||
                              sess->mv.postfix[0] != '\0')) {
            char tmp[sizeof(body)];
            snprintf(tmp, sizeof(tmp), "%s%s%s%s%s", sess->mv.prefix,
                     sess->mv.prefix[0] != '\0' ? " " : "", body,
                     sess->mv.postfix[0] != '\0' ? " " : "", sess->mv.postfix);
            snprintf(body, sizeof(body), "%s", tmp);
        }
        char resolved[sizeof(body)];
        mv_resolve_mentions(host, body, resolved, sizeof(resolved));
        snprintf(body, sizeof(body), "%s", resolved);

        if (mode == MV_COLOR_NONE) {
            mode = mv_parse_color_spec(sess->mv.msg_color, &ca, &cb);
        }
        switch (opts->anim) {
        case 2:
            mode = MV_COLOR_RAINBOW;
            break;
        case 3:
            mode = MV_COLOR_WAVE;
            mv_alternate_case(body);
            break;
        case 5:
            mode = MV_COLOR_SPARKLE;
            break;
        default:
            break;
        }
    }

    char styled[SSH_CHATTER_MESSAGE_LIMIT];
    char plain[DDIAL_MV_TEXT_LIMIT * 2U];
    if (opts->uno) {
        mv_out_t o = {styled, sizeof(styled), 0U};
        static const char *const bg[] = {"41", "43", "42", "44"};
        size_t k = 0U;
        for (const char *p = body; *p != '\0'; ++p) {
            if (*p == ' ') {
                mv_out_put(&o, "  ");
                continue;
            }
            char tile[32];
            snprintf(tile, sizeof(tile), "\033[1;97;%sm %c \033[0m",
                     bg[k++ % 4U], toupper((unsigned char)*p));
            mv_out_put(&o, tile);
        }
        snprintf(plain, sizeof(plain), "%s", body);
    } else if (opts->block) {
        snprintf(styled, sizeof(styled), "%s", body);
        snprintf(plain, sizeof(plain), "%s", body);
    } else {
        mv_render(body, mode, ca, cb, styled, sizeof(styled), plain,
                  sizeof(plain));
        if (opts->anim == 4) {
            char tmp[sizeof(styled)];
            snprintf(tmp, sizeof(tmp), "\033[5m%s\033[0m", styled);
            snprintf(styled, sizeof(styled), "%s", tmp);
        }
    }

    uint16_t pace = opts->pace_baud;
    if (opts->anim == 1 && pace == 0U) {
        pace = 300U;
    }

    const char *bo = mv_is_member(sess) ? "[" : "(";
    const char *bc = mv_is_member(sess) ? "]" : ")";
    const char *sym = mv_is_member(sess) ? "*" : "";
    char line[SSH_CHATTER_MESSAGE_LIMIT];
    if (opts->action) {
        snprintf(line, sizeof(line), "#%u%sT%u:* %s %s%s", (unsigned)sess->slot,
                 bo, (unsigned)channel, mv_handle(sess), styled, bc);
    } else if (opts->block) {
        snprintf(line, sizeof(line), "#%u%sT%u:%s%s%s code:\r\n%s",
                 (unsigned)sess->slot, bo, (unsigned)channel, mv_handle(sess),
                 sym, bc, styled);
    } else {
        snprintf(line, sizeof(line), "#%u%sT%u:%s%s%s %s%s",
                 (unsigned)sess->slot, bo, (unsigned)channel, mv_handle(sess),
                 sym, bc, styled,
                 vanish ? " \033[2m(vanishes in 30s)\033[0m" : "");
    }

    ddial_mv_public_t pub = {0};
    pub.channel = channel;
    pub.from_slot = sess->slot;
    pub.from_member = sess->mv.member_no;
    pub.line = line;
    pub.plain_body = plain;
    pub.members_only = opts->members_only;
    pub.pace_baud = pace;
    host_ddial_deliver_public(host, &pub);

    bool shared = !opts->local_only && !opts->members_only;
    if (shared && channel == DDIAL_MV_ROOM_CHANNEL) {
        mv_share_room(host, line);
    }
    if (shared && channel <= DDIAL_MAX_CHANNEL && !opts->block) {
        char wire[DDIAL_MV_TEXT_LIMIT * 2U + 8U];
        if (opts->action) {
            snprintf(wire, sizeof(wire), "* %s", plain);
        } else {
            snprintf(wire, sizeof(wire), "%s", plain);
        }
        host_ddial_client_send_channel(host, sess->handle,
                                       DDIAL_MV_LINK_CHANNEL(channel), wire);
    }
    if (vanish) {
        mv_schedule_vanish(host, channel, sess->handle,
                           shared && channel == DDIAL_MV_ROOM_CHANNEL ? line
                                                                      : "");
    }
    ++sess->mv.messages_sent;
}

/* ---- private messages --------------------------------------------------- */

static void mv_private_one(ddial_session_t *sess, uint16_t slot,
                           const char *body, bool action)
{
    host_t *host = sess->owner;
    char text[SSH_CHATTER_MESSAGE_LIMIT];
    if (action) {
        snprintf(text, sizeof(text), "P#%u(* %s %s)", (unsigned)sess->slot,
                 sess->handle, body);
    } else {
        snprintf(text, sizeof(text), "P#%u(%s) %s", (unsigned)sess->slot,
                 sess->handle, body);
    }

    ddial_session_t *target = mv_find_slot(host, slot);
    if (target != nullptr) {
        if (mv_list_contains(&target->mv.squelch_slots, sess->slot)) {
            mv_line(sess, "* Line #%u is not accepting your messages.",
                    (unsigned)slot);
            return;
        }
        ddial_session_write_line(target, text);
        mv_bell(target, DDIAL_MV_BEEP_PM);
        mv_line(sess, "* Sent to #%u (%s).", (unsigned)slot, target->handle);
        return;
    }

    char username[SSH_CHATTER_USERNAME_LEN];
    if (host_ddial_chat_link_username(host, slot, username, sizeof(username))) {
        session_ctx_t *ctx = chat_room_find_user_ref(&host->room, username);
        if (ctx != nullptr) {
            char line[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(line, sizeof(line), "[DDial PM from #%u %s] %s%s",
                     (unsigned)sess->slot, sess->handle, action ? "* " : "",
                     body);
            session_send_line(ctx, line);
            if (ctx->history_scroll_position == 0U) {
                session_refresh_input_line(ctx);
            }
            chat_room_release_user_ref(ctx);
            mv_line(sess, "* Sent to #%u (%s, Chatter).", (unsigned)slot,
                    username);
            return;
        }
    }

    /* Not local: hand it to the Station Link, if one is up. */
    host_ddial_client_send_private(host, slot, sess->handle, body);
    mv_line(sess, "* Sent to #%u over the Station Link (if connected).",
            (unsigned)slot);
}

static void mv_private(ddial_session_t *sess, const ddial_mv_slot_list_t *to,
                       const char *body, bool action)
{
    body = mv_skip_ws(body);
    if (body[0] == '\0') {
        mv_line(sess, "* Usage: /p# message");
        return;
    }
    if (sess->mv.full_muted) {
        mv_line(sess, "* You are muted.");
        return;
    }
    for (size_t i = 0U; i < to->count; ++i) {
        if (to->items[i] <= UINT16_MAX) {
            mv_private_one(sess, (uint16_t)to->items[i], body, action);
        }
    }
}

/* ---- member persistence bridge ----------------------------------------- */

static void mv_apply_member(ddial_session_t *sess, const ddial_member_t *m)
{
    sess->mv.member_no = m->number;
    sess->mv.cosysop = m->cosysop || mv_env_cosysop(m->number);
    snprintf(sess->handle, sizeof(sess->handle), "%s", m->handle);
    if (m->handle_color[0] != '\0') {
        char plain[DDIAL_MAX_HANDLE_LEN];
        mv_render_handle(m->handle_color, sess->mv.display_handle,
                         sizeof(sess->mv.display_handle), plain,
                         sizeof(plain));
    } else {
        sess->mv.display_handle[0] = '\0';
    }
    snprintf(sess->mv.msg_color, sizeof(sess->mv.msg_color), "%s",
             m->msg_color);
    snprintf(sess->mv.prefix, sizeof(sess->mv.prefix), "%s", m->prefix);
    snprintf(sess->mv.postfix, sizeof(sess->mv.postfix), "%s", m->postfix);
    snprintf(sess->mv.status, sizeof(sess->mv.status), "%s", m->status);
    snprintf(sess->mv.font, sizeof(sess->mv.font), "%s", m->font);
    sess->mv.beep_on = m->beep_on;
    sess->mv.beep_volume = m->beep_volume;
    sess->mv.beep_events = m->beep_events;
    sess->mv.baud = m->baud;
    sess->mv.badges_on = m->badges_on;
    sess->mv.replies_on = m->replies_on;
    sess->mv.auto_slash = m->auto_slash;
    sess->mv.voice_on = m->voice_on;
    sess->mv.notify_on = m->notify_on;
}

/* Copy session preferences back into the member record and persist. */
static void mv_save_member(ddial_session_t *sess)
{
    if (!mv_is_member(sess)) {
        return;
    }
    ddial_member_t m;
    if (!ddial_member_get(sess->owner, sess->mv.member_no, &m)) {
        return;
    }
    snprintf(m.msg_color, sizeof(m.msg_color), "%s", sess->mv.msg_color);
    snprintf(m.prefix, sizeof(m.prefix), "%s", sess->mv.prefix);
    snprintf(m.postfix, sizeof(m.postfix), "%s", sess->mv.postfix);
    snprintf(m.status, sizeof(m.status), "%s", sess->mv.status);
    snprintf(m.font, sizeof(m.font), "%s", sess->mv.font);
    m.beep_on = sess->mv.beep_on;
    m.beep_volume = sess->mv.beep_volume;
    m.beep_events = sess->mv.beep_events;
    m.baud = sess->mv.baud;
    m.badges_on = sess->mv.badges_on;
    m.replies_on = sess->mv.replies_on;
    m.auto_slash = sess->mv.auto_slash;
    m.voice_on = sess->mv.voice_on;
    m.notify_on = sess->mv.notify_on;
    ddial_member_put(sess->owner, &m);
}

/* ---- login / logout ----------------------------------------------------- */

static void mv_defaults(ddial_session_t *sess)
{
    memset(&sess->mv, 0, sizeof(sess->mv));
    sess->mv.beep_on = true;
    sess->mv.beep_volume = 100U;
    sess->mv.beep_events =
        DDIAL_MV_BEEP_PM | DDIAL_MV_BEEP_MENTION | DDIAL_MV_BEEP_BUZZ;
    sess->mv.badges_on = true;
    sess->mv.replies_on = true;
    sess->mv.auto_slash = true;
    sess->mv.login_time = time(nullptr);
    sess->mv.channel_joined_at = sess->mv.login_time;
}

static void mv_print_offline_cb(const ddial_mail_t *mail, void *user)
{
    ddial_session_t *sess = (ddial_session_t *)user;
    char when[32];
    mv_format_time((time_t)mail->sent, when, sizeof(when));
    mv_line(sess, "* Message from %s (#%u) at %s: %s", mail->from_handle,
            (unsigned)mail->from, when, mail->text);
}

static bool mv_today_is_bday(const char *bday)
{
    int y, m, d;
    if (!mv_valid_date(bday, &y, &m, &d)) {
        return false;
    }
    time_t now = time(nullptr);
    struct tm tm_value;
    localtime_r(&now, &tm_value);
    return tm_value.tm_mon + 1 == m && tm_value.tm_mday == d;
}

static void mv_finish_login(ddial_session_t *sess, const ddial_member_t *member)
{
    host_t *host = sess->owner;
    sess->logged_in = true;
    sess->channel = DDIAL_DEFAULT_CHANNEL;
    sess->mv.channel_joined_at = time(nullptr);

    if (member != nullptr) {
        ddial_member_t m = *member;
        m.logins++;
        m.last_seen = (int64_t)time(nullptr);
        ddial_member_put(host, &m);
        mv_apply_member(sess, &m);
        mv_line(sess, "Welcome back, %s (member #%u). You are on line #%u, "
                      "channel %u.",
                sess->handle, (unsigned)m.number, (unsigned)sess->slot,
                (unsigned)sess->channel);
        ddial_mail_visit(host, m.number, true, true, mv_print_offline_cb,
                         sess);
        size_t mail = ddial_mail_visit(host, m.number, false, false, nullptr,
                                       nullptr);
        if (mail > 0U) {
            mv_line(sess, "* You have %zu email%s. Type /e to read.", mail,
                    mail == 1U ? "" : "s");
        }
        if (sess->mv.baud != 0U) {
            mv_line(sess, "* Baud rate is %u.", (unsigned)sess->mv.baud);
        }
    } else {
        mv_line(sess, "Welcome, %s. You are on line #%u, channel %u (guest).",
                sess->handle, (unsigned)sess->slot, (unsigned)sess->channel);
        mv_line(sess, "Type /i for help, /s for who's online, /signup "
                      "<password> to become a member.");
    }

    mv_record_visit(sess, true);

    char note[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(note, sizeof(note), "* #%u %s has dialed in.",
             (unsigned)sess->slot, sess->handle);
    ddial_mv_public_t pub = {0};
    pub.channel = 0U;
    pub.from_slot = sess->slot;
    pub.line = note;
    pub.system = true;
    host_ddial_foreach_session(host, mv_deliver_cb, &pub);
    mv_share_room(host, note);

    if (member != nullptr && mv_today_is_bday(member->bday)) {
        snprintf(note, sizeof(note),
                 "\033[1;95m*** Happy birthday, %s! ***\033[0m", sess->handle);
        pub.line = note;
        pub.members_only = true;
        host_ddial_deliver_public(host, &pub);
    }

    host_ddial_client_send_login(host, sess->slot,
                                 DDIAL_MV_LINK_CHANNEL(sess->channel),
                                 mv_tier(sess), sess->handle,
                                 (uint16_t)sess->mv.member_no);
}

/* Handle the first line(s) typed at the "Enter your handle" prompt.
 * Accepts a guest handle, "###password" or "###:password[:command]". */
static void ddial_mv_login_line(ddial_session_t *sess, const char *line)
{
    host_t *host = sess->owner;
    line = mv_skip_ws(line);
    if (line[0] == '\0') {
        ddial_session_write_line(sess, "Invalid handle. Try again.");
        return;
    }

    if (isdigit((unsigned char)line[0])) {
        const char *p = line;
        uint32_t number = 0U;
        if (mv_parse_uint(&p, &number) && *p != '\0' &&
            ddial_member_get(host, number, nullptr)) {
            char password[128];
            const char *after = nullptr;
            if (*p == ':') {
                ++p;
                const char *colon = strchr(p, ':');
                size_t len = colon != nullptr ? (size_t)(colon - p) : strlen(p);
                if (len >= sizeof(password)) {
                    len = sizeof(password) - 1U;
                }
                memcpy(password, p, len);
                password[len] = '\0';
                after = colon != nullptr ? colon + 1 : nullptr;
            } else {
                snprintf(password, sizeof(password), "%s", p);
            }
            ddial_member_t member;
            if (!ddial_member_authenticate(host, number, password, &member)) {
                ddial_session_write_line(sess, "Invalid member login.");
                return;
            }
            if (mv_find_member(host, number) != nullptr) {
                ddial_session_write_line(sess,
                                         "That member is already online.");
                return;
            }
            mv_defaults(sess);
            mv_finish_login(sess, &member);
            if (after != nullptr && after[0] != '\0') {
                ddial_session_process_line(sess, after);
            }
            return;
        }
    }

    char clean[DDIAL_MAX_HANDLE_LEN];
    if (ddial_sanitize_handle(line, strlen(line), clean, sizeof(clean)) == 0U ||
        clean[0] == '\0') {
        ddial_session_write_line(sess, "Invalid handle. Try again.");
        return;
    }
    uint32_t owner = ddial_member_owner_of(host, clean);
    if (owner != 0U) {
        mv_line(sess, "'%s' is a registered handle. Log in with %u<password> "
                      "or pick another handle.",
                clean, (unsigned)owner);
        return;
    }
    if (mv_find_handle(host, clean) != nullptr) {
        ddial_session_write_line(sess, "That handle is in use. Try another.");
        return;
    }
    snprintf(sess->handle, sizeof(sess->handle), "%s", clean);
    mv_defaults(sess);
    mv_finish_login(sess, nullptr);
}

static void ddial_mv_on_logout(ddial_session_t *sess)
{
    if (!sess->logged_in || sess->owner == nullptr) {
        return;
    }
    host_t *host = sess->owner;
    mv_record_visit(sess, false);
    if (mv_is_member(sess)) {
        ddial_member_t m;
        if (ddial_member_get(host, sess->mv.member_no, &m)) {
            m.last_seen = (int64_t)time(nullptr);
            m.messages += sess->mv.messages_sent;
            ddial_member_put(host, &m);
        }
        sess->mv.messages_sent = 0U;
    }
    char note[SSH_CHATTER_MESSAGE_LIMIT];
    if (sess->mv.quit_msg[0] != '\0') {
        snprintf(note, sizeof(note), "* #%u %s has hung up: %s",
                 (unsigned)sess->slot, sess->handle, sess->mv.quit_msg);
    } else {
        snprintf(note, sizeof(note), "* #%u %s has hung up.",
                 (unsigned)sess->slot, sess->handle);
    }
    ddial_mv_public_t pub = {0};
    pub.channel = 0U;
    pub.from_slot = sess->slot;
    pub.line = note;
    pub.system = true;
    host_ddial_deliver_public(host, &pub);
    mv_share_room(host, note);
}

/* Called from the session thread roughly every poll tick. */
static void ddial_mv_tick(ddial_session_t *sess)
{
    time_t now = time(nullptr);

    if (sess->mv.quit_deadline != 0 && now != sess->mv.quit_last_tick) {
        sess->mv.quit_last_tick = now;
        long left = (long)(sess->mv.quit_deadline - now);
        if (left <= 0) {
            ddial_session_write_line(sess, "Goodbye!");
            sess->should_exit = true;
        } else {
            char note[96];
            snprintf(note, sizeof(note), "* %s leaves in %ld...", sess->handle,
                     left);
            mv_notice_channel(sess, sess->channel, note);
        }
    }

    /* Vanishing messages: whichever session ticks first reaps them. */
    ddial_mv_vanish_t due[DDIAL_MV_VANISH_MAX];
    size_t due_count = 0U;
    pthread_mutex_lock(&g_mv_lock);
    for (size_t i = 0U; i < DDIAL_MV_VANISH_MAX; ++i) {
        if (g_mv_vanish[i].host == sess->owner && g_mv_vanish[i].due <= now) {
            due[due_count++] = g_mv_vanish[i];
            g_mv_vanish[i].host = nullptr;
        }
    }
    pthread_mutex_unlock(&g_mv_lock);
    for (size_t i = 0U; i < due_count; ++i) {
        if (due[i].room_line[0] != '\0') {
            mv_room_forget(due[i].host, due[i].room_line);
        }
        char note[128];
        snprintf(note, sizeof(note), "\033[2m* A message from %s has "
                                     "vanished.\033[0m",
                 due[i].handle);
        ddial_mv_public_t pub = {0};
        pub.channel = due[i].channel;
        pub.line = note;
        pub.system = true;
        host_ddial_deliver_public(due[i].host, &pub);
    }
}

/* ---- listings ----------------------------------------------------------- */

typedef struct mv_who_ctx {
    ddial_session_t *viewer;
    uint16_t channel; /* 0 = all */
    size_t count;
} mv_who_ctx_t;

static void mv_badges(const ddial_session_t *s, char *buf, size_t cap)
{
    buf[0] = '\0';
    if (s->mv.cosysop) {
        strncat(buf, "[CoSysop]", cap - strlen(buf) - 1U);
    }
    if (s->mv.member_no != 0U) {
        char tag[24];
        snprintf(tag, sizeof(tag), "[M#%u]", (unsigned)s->mv.member_no);
        strncat(buf, tag, cap - strlen(buf) - 1U);
    } else {
        strncat(buf, "[Guest]", cap - strlen(buf) - 1U);
    }
    if (s->mv.full_muted) {
        strncat(buf, "[Muted]", cap - strlen(buf) - 1U);
    }
}

static void mv_bday_countdown(uint32_t member_no, host_t *host, char *buf,
                              size_t cap)
{
    buf[0] = '\0';
    ddial_member_t m;
    int y, mo, d;
    if (member_no == 0U || !ddial_member_get(host, member_no, &m) ||
        !m.bday_countdown || !mv_valid_date(m.bday, &y, &mo, &d)) {
        return;
    }
    time_t now = time(nullptr);
    struct tm today;
    localtime_r(&now, &today);
    struct tm next = {0};
    next.tm_year = today.tm_year;
    next.tm_mon = mo - 1;
    next.tm_mday = d;
    next.tm_hour = 12;
    time_t when = mktime(&next);
    if (when < now - 86400) {
        next.tm_year++;
        when = mktime(&next);
    }
    long days = (long)((when - now) / 86400);
    if (days <= 0) {
        snprintf(buf, cap, " [bday today!]");
    } else {
        snprintf(buf, cap, " [bday in %ld day%s]", days, days == 1 ? "" : "s");
    }
}

static void mv_who_cb(ddial_session_t *s, void *user)
{
    mv_who_ctx_t *w = (mv_who_ctx_t *)user;
    if (!s->logged_in) {
        return;
    }
    if (w->channel != 0U && s->channel != w->channel) {
        return;
    }
    char badges[64] = "";
    char cd[40] = "";
    if (w->viewer->mv.badges_on) {
        mv_badges(s, badges, sizeof(badges));
        mv_bday_countdown(s->mv.member_no, s->owner, cd, sizeof(cd));
    }
    mv_line(w->viewer, " #%-4u T%-3u %s%s %s%s%s%s", (unsigned)s->slot,
            (unsigned)s->channel, mv_handle(s), "\033[0m", badges, cd,
            s->mv.status[0] != '\0' ? " - " : "", s->mv.status);
    ++w->count;
}

static void mv_who_link_cb(uint16_t slot, const char *handle,
                           const char *username, void *user)
{
    (void)username;
    mv_who_ctx_t *w = (mv_who_ctx_t *)user;
    if (w->channel != 0U && w->channel != DDIAL_MV_ROOM_CHANNEL) {
        return;
    }
    mv_line(w->viewer, " #%-4u T%-3u %s [Chatter]", (unsigned)slot,
            (unsigned)DDIAL_MV_ROOM_CHANNEL, handle);
    ++w->count;
}

static void mv_cmd_who(ddial_session_t *sess, const char *arg)
{
    uint32_t channel = 0U;
    const char *p = arg;
    mv_parse_uint(&p, &channel);
    if (channel > DDIAL_MV_MAX_CHANNEL) {
        channel = 0U;
    }
    if (channel != 0U) {
        mv_line(sess, "Users on channel %u:", (unsigned)channel);
    } else {
        mv_line(sess, "Users online:");
    }
    mv_line(sess, " Line  Ch   Handle");
    mv_who_ctx_t w = {sess, (uint16_t)channel, 0U};
    host_ddial_foreach_session(sess->owner, mv_who_cb, &w);
    host_ddial_foreach_chat_link(sess->owner, mv_who_link_cb, &w);
    mv_line(sess, "%zu user%s.", w.count, w.count == 1U ? "" : "s");
}

typedef struct mv_member_list_ctx {
    ddial_session_t *viewer;
    const char *keyword;
    size_t count;
} mv_member_list_ctx_t;

static void mv_member_list_cb(const ddial_member_t *m, void *user)
{
    mv_member_list_ctx_t *c = (mv_member_list_ctx_t *)user;
    if (c->keyword != nullptr && c->keyword[0] != '\0' &&
        strcasestr(m->handle, c->keyword) == nullptr &&
        strcasestr(m->status, c->keyword) == nullptr) {
        return;
    }
    char seen[32];
    mv_format_time((time_t)m->last_seen, seen, sizeof(seen));
    char badge[48] = "";
    if (m->cosysop || mv_env_cosysop(m->number)) {
        snprintf(badge, sizeof(badge), " [CoSysop]");
    }
    char line[256];
    snprintf(line, sizeof(line), " #%-5u %-25s last seen %s%s",
             (unsigned)m->number, m->handle, seen, badge);
    ddial_session_write_line(c->viewer, line);
    ++c->count;
}

static void mv_cmd_member_list(ddial_session_t *sess, const char *keyword)
{
    keyword = mv_skip_ws(keyword);
    mv_line(sess, keyword[0] != '\0' ? "Members matching '%s':" : "Members:%s",
            keyword);
    mv_member_list_ctx_t c = {sess, keyword, 0U};
    ddial_member_foreach(sess->owner, mv_member_list_cb, &c);
    mv_line(sess, "%zu member%s.", c.count, c.count == 1U ? "" : "s");
}

static void mv_cmd_last_visitors(ddial_session_t *sess)
{
    mv_line(sess, "Last %u visitors:", (unsigned)DDIAL_MV_VISITORS);
    pthread_mutex_lock(&g_mv_lock);
    ddial_mv_visit_t copy[DDIAL_MV_VISITORS];
    size_t count = g_mv_visitor_count;
    size_t head = g_mv_visitor_head;
    memcpy(copy, g_mv_visitors, sizeof(copy));
    pthread_mutex_unlock(&g_mv_lock);
    for (size_t i = 0U; i < count; ++i) {
        size_t idx = (head + DDIAL_MV_VISITORS - 1U - i) % DDIAL_MV_VISITORS;
        char when[32];
        mv_format_time(copy[idx].when, when, sizeof(when));
        if (copy[idx].member != 0U) {
            mv_line(sess, " %s  %-25s #%u  %s", when, copy[idx].handle,
                    (unsigned)copy[idx].member,
                    copy[idx].logon ? "on" : "off");
        } else {
            mv_line(sess, " %s  %-25s guest %s", when, copy[idx].handle,
                    copy[idx].logon ? "on" : "off");
        }
    }
    if (count == 0U) {
        mv_line(sess, " (nobody yet)");
    }
}

static void mv_cosysop_cb(const ddial_member_t *m, void *user)
{
    mv_member_list_ctx_t *c = (mv_member_list_ctx_t *)user;
    if (m->cosysop || mv_env_cosysop(m->number)) {
        char line[128];
        snprintf(line, sizeof(line), " #%-5u %s%s", (unsigned)m->number,
                 m->handle,
                 mv_find_member(c->viewer->owner, m->number) != nullptr
                     ? " (online)"
                     : "");
        ddial_session_write_line(c->viewer, line);
        ++c->count;
    }
}

static void mv_cmd_cosysops(ddial_session_t *sess, const char *arg)
{
    arg = mv_skip_ws(arg);
    if ((arg[0] == '+' || arg[0] == '-') && sess->mv.cosysop) {
        const char *p = arg + 1;
        uint32_t no = 0U;
        ddial_member_t m;
        if (mv_parse_uint(&p, &no) && ddial_member_get(sess->owner, no, &m)) {
            m.cosysop = arg[0] == '+';
            ddial_member_put(sess->owner, &m);
            ddial_session_t *t = mv_find_member(sess->owner, no);
            if (t != nullptr) {
                t->mv.cosysop = m.cosysop || mv_env_cosysop(no);
            }
            mv_line(sess, "* Member #%u is %s a CoSysop.", (unsigned)no,
                    m.cosysop ? "now" : "no longer");
        } else {
            mv_line(sess, "* No such member.");
        }
        return;
    }
    mv_line(sess, "CoSysops:");
    mv_member_list_ctx_t c = {sess, nullptr, 0U};
    ddial_member_foreach(sess->owner, mv_cosysop_cb, &c);
    if (c.count == 0U) {
        mv_line(sess, " (none)");
    }
}

/* ---- channels ------------------------------------------------------------ */

typedef struct mv_first_ctx {
    uint16_t channel;
    ddial_session_t *first;
    time_t first_at;
} mv_first_ctx_t;

static void mv_first_cb(ddial_session_t *s, void *user)
{
    mv_first_ctx_t *c = (mv_first_ctx_t *)user;
    if (!s->logged_in || s->channel != c->channel) {
        return;
    }
    if (c->first == nullptr || s->mv.channel_joined_at < c->first_at) {
        c->first = s;
        c->first_at = s->mv.channel_joined_at;
    }
}

static bool mv_is_moderator(ddial_session_t *sess, uint16_t channel)
{
    if (sess->mv.cosysop) {
        return true;
    }
    mv_first_ctx_t c = {channel, nullptr, 0};
    host_ddial_foreach_session(sess->owner, mv_first_cb, &c);
    return c.first == sess;
}

static void mv_show_topic(ddial_session_t *sess, uint16_t channel)
{
    pthread_mutex_lock(&g_mv_lock);
    ddial_mv_channel_meta_t *meta = mv_meta_locked(channel, false);
    char topic[160] = "";
    char question[256] = "";
    char asker[DDIAL_MAX_HANDLE_LEN] = "";
    if (meta != nullptr) {
        snprintf(topic, sizeof(topic), "%s", meta->topic);
        snprintf(question, sizeof(question), "%s", meta->question);
        snprintf(asker, sizeof(asker), "%s", meta->asker);
    }
    pthread_mutex_unlock(&g_mv_lock);
    if (topic[0] != '\0') {
        mv_line(sess, "* Topic: %s", topic);
    }
    if (question[0] != '\0') {
        mv_line(sess, "* Open question from %s: %s (/answer to reply)", asker,
                question);
    }
}

static void mv_tune(ddial_session_t *sess, uint32_t channel)
{
    if (channel < 1U || channel > DDIAL_MV_MAX_CHANNEL) {
        mv_line(sess, "* Channels are 1-%u.", DDIAL_MV_MAX_CHANNEL);
        return;
    }
    if (!mv_is_member(sess) && channel > DDIAL_MV_GUEST_MAX_CHANNEL) {
        mv_line(sess, "* Guests may use channels 1-%u. /signup for more.",
                DDIAL_MV_GUEST_MAX_CHANNEL);
        return;
    }
    if (mv_is_booted((uint16_t)channel, sess->handle)) {
        mv_line(sess, "* You were booted from channel %u; try later.",
                (unsigned)channel);
        return;
    }
    uint16_t old = sess->channel;
    if (old == channel) {
        mv_line(sess, "* You are already on channel %u.", (unsigned)channel);
        return;
    }
    char note[128];
    snprintf(note, sizeof(note), "* #%u %s tuned out.", (unsigned)sess->slot,
             sess->handle);
    mv_notice_channel(sess, old, note);
    sess->channel = (uint16_t)channel;
    sess->mv.channel_joined_at = time(nullptr);
    snprintf(note, sizeof(note), "* #%u %s tuned in.", (unsigned)sess->slot,
             sess->handle);
    mv_notice_channel(sess, sess->channel, note);
    mv_line(sess, "* Tuned to channel %u%s.", (unsigned)channel,
            channel == DDIAL_MV_ROOM_CHANNEL
                ? " (shared with the Chatter room)"
            : channel <= DDIAL_MAX_CHANNEL ? " (linked)"
                                           : " (local)");
    mv_show_topic(sess, sess->channel);
}

typedef struct mv_space {
    uint16_t channel;
    size_t users;
} mv_space_t;

typedef struct mv_spaces_ctx {
    mv_space_t spaces[DDIAL_MV_META_MAX];
    size_t count;
} mv_spaces_ctx_t;

static void mv_spaces_add(mv_spaces_ctx_t *c, uint16_t channel)
{
    for (size_t i = 0U; i < c->count; ++i) {
        if (c->spaces[i].channel == channel) {
            c->spaces[i].users++;
            return;
        }
    }
    if (c->count < DDIAL_MV_META_MAX) {
        c->spaces[c->count].channel = channel;
        c->spaces[c->count].users = 1U;
        c->count++;
    }
}

static void mv_spaces_cb(ddial_session_t *s, void *user)
{
    if (s->logged_in) {
        mv_spaces_add((mv_spaces_ctx_t *)user, s->channel);
    }
}

static void mv_spaces_link_cb(uint16_t slot, const char *handle,
                              const char *username, void *user)
{
    (void)slot;
    (void)handle;
    (void)username;
    mv_spaces_add((mv_spaces_ctx_t *)user, DDIAL_MV_ROOM_CHANNEL);
}

static void mv_cmd_spaces(ddial_session_t *sess, const char *arg)
{
    mv_spaces_ctx_t c;
    memset(&c, 0, sizeof(c));
    host_ddial_foreach_session(sess->owner, mv_spaces_cb, &c);
    host_ddial_foreach_chat_link(sess->owner, mv_spaces_link_cb, &c);
    arg = mv_skip_ws(arg);
    if (arg[0] != '\0') {
        if (!mv_require_member(sess)) {
            return;
        }
        for (size_t i = 0U; i < c.count; ++i) {
            mv_public_chat(sess, c.spaces[i].channel, arg, nullptr);
        }
        return;
    }
    mv_line(sess, "Active spaces:");
    for (size_t i = 0U; i < c.count; ++i) {
        pthread_mutex_lock(&g_mv_lock);
        ddial_mv_channel_meta_t *meta =
            mv_meta_locked(c.spaces[i].channel, false);
        char topic[160] = "";
        if (meta != nullptr) {
            snprintf(topic, sizeof(topic), "%s", meta->topic);
        }
        pthread_mutex_unlock(&g_mv_lock);
        mv_line(sess, " T%-4u %zu user%s%s%s", (unsigned)c.spaces[i].channel,
                c.spaces[i].users, c.spaces[i].users == 1U ? "" : "s",
                topic[0] != '\0' ? " - " : "", topic);
    }
}

/* ---- help ---------------------------------------------------------------- */

static void mv_cmd_help(ddial_session_t *sess)
{
    static const char *const lines[] = {
        "Commands (type /?m for member commands):",
        "/i            this help            /s[#]        users online (channel#)",
        "/h <handle>   change handle        /sm [word]   member list / search",
        "/p# <msg>     private to line#     /ps <msg>    local only, no links",
        "@#            mention line#        /m[#|a-z]    message boxes (/m lists)",
        "/r#           profile of member#   /ls          last 30 visitors",
        "/b            beep on/off (/b?)    /bv#         beep volume 0-100",
        "/baud #       0,300,600,1200,2400,9600 output speed",
        "/font <name>  preferred font       /sc          CoSysop list",
        "/re+ /re-     replies on/off       /ai          AI users",
        "/q [msg]      quit (/q+ countdown) /jk[#]       a joke",
        "/giphy <kw>   find a GIF           /forgot      password help",
        "/signup <pw>  free membership      /t#          tune to channel#",
        "Login: ###password  or  ###:password:command-or-message",
        "Extras: `alt case  ~rainbow  ,vanish  \\i \\b \\u  #RRGGBB colour",
        nullptr,
    };
    mv_lines(sess, lines);
}

static void mv_cmd_member_help(ddial_session_t *sess)
{
    static const char *const lines[] = {
        "Member commands:",
        "/t# tune 1-999  /t+# monitor  /t-# unmonitor  /ts# <msg>  ;<msg>",
        "/tb# boot line# from your channel (moderator)  /t? channel help",
        "/p#,# <msg> multi-PM   /p=#,# auto-PM (/p <msg>, /p= reset)",
        "/pv <msg> members only  /a <action>  /a# <action> private action",
        "/h#RRGGBBName colour handle  /h#=<handle> save  /h# use  /h? list",
        "/us <status>  /c RRGGBB[,RRGGBB]  /pre <txt>  /post <txt>",
        "/r= <profile>  /m= <box>  /m=rotate  /ig# /ig+# /ig-#  /x#  /xx#",
        "/null# strip colour of line#  /cls  /n  /topic  /ask  /answer",
        "/8ball  /anim# <msg>  /taco <msg>  /buzz ### <msg>  /image <desc>",
        "/pw=<new>  /remember+/-  //+ //-  /@? mentions  /bday YYYY-MM-DD",
        "/bday+ /bday- /bdayRemove  /bg badges  /o# <msg>  /e? email",
        "/d link settings  /u <msg>  /300 /1200 /2400 <msg>  /reverse",
        "/scramble  /lorem  /code  /define <word>  /whois#  /stats  /bior",
        "/voice /voice?  /spaces [msg]  /sentry +/-",
        nullptr,
    };
    mv_lines(sess, lines);
}

static void mv_cmd_channel_help(ddial_session_t *sess)
{
    static const char *const lines[] = {
        "Channel commands:",
        "/t#         tune to channel# (1-4 linked, 5-999 local; 1 = Chatter room)",
        "/t+#        monitor channel#     /t-#   stop monitoring",
        "/ts# <msg>  send to a monitored channel (shortcut: ;<msg>)",
        "/tb#        boot line# from your channel (first person in moderates)",
        "/topic <x>  suggest a topic      /ask <q> /answer <a>",
        nullptr,
    };
    mv_lines(sess, lines);
}

/* ---- fun ----------------------------------------------------------------- */

static const char *const k_mv_jokes[] = {
    "I told my modem a joke. It didn't get it; the carrier was lost.",
    "Why did the BBS sysop go broke? Too many free downloads.",
    "There are 10 kinds of people: those who read binary and those who don't.",
    "My 300 baud modem is so slow, it downloads in sepia.",
    "Why do programmers prefer dark mode? Light attracts bugs.",
    "I would tell a UDP joke, but you might not get it.",
    "A SQL query walks into a bar, walks up to two tables and asks: may I join you?",
    "Why was the computer cold? It left its Windows open.",
    "Knock knock. Race condition. Who's there?",
    "I changed my password to 'incorrect' so it tells me when I forget it.",
    "The FidoNet mailer called at 3am. It was a wrong zone.",
    "Why did the ANSI artist break up? Too many escape sequences.",
    "Hardware: the part of a computer you can kick.",
    "I'd tell a TCP joke, but I'd have to keep repeating it until you got it.",
    "Sysop's law: the user who crashes the board is always on long distance.",
    "How many sysops does it take to change a bulb? None, it's a hardware problem.",
    "My BBS has 99 problems, but a busy signal ain't one. Oh wait.",
    "Why did the handle cross the channel? To get to the other /t.",
    "Real modems don't need error correction; they just yell louder.",
    "Ctrl+Z: the original undo button for your life choices.",
};

static const char *const k_mv_8ball[] = {
    "It is certain.", "It is decidedly so.", "Without a doubt.",
    "Yes, definitely.", "You may rely on it.", "As I see it, yes.",
    "Most likely.", "Outlook good.", "Yes.", "Signs point to yes.",
    "Reply hazy, try again.", "Ask again later.", "Better not tell you now.",
    "Cannot predict now.", "Concentrate and ask again.", "Don't count on it.",
    "My reply is no.", "My sources say no.", "Outlook not so good.",
    "Very doubtful.",
};

static void mv_cmd_joke(ddial_session_t *sess, const char *arg)
{
    size_t count = sizeof(k_mv_jokes) / sizeof(k_mv_jokes[0]);
    const char *p = arg;
    uint32_t n = 0U;
    size_t idx;
    if (mv_parse_uint(&p, &n) && n >= 1U && n <= count) {
        idx = n - 1U;
    } else {
        idx = (size_t)rand() % count;
    }
    char text[256];
    snprintf(text, sizeof(text), "Joke #%zu: %s", idx + 1U, k_mv_jokes[idx]);
    mv_chat_opts_t o = {0};
    o.no_transform = true;
    mv_public_chat(sess, sess->channel, text, &o);
}

static void mv_cmd_8ball(ddial_session_t *sess, const char *question)
{
    question = mv_skip_ws(question);
    if (question[0] == '\0') {
        mv_line(sess, "* Usage: /8ball <question>");
        return;
    }
    char text[DDIAL_MV_TEXT_LIMIT];
    snprintf(text, sizeof(text), "asks the Magic 8-Ball \"%s\" ... %s",
             question,
             k_mv_8ball[(size_t)rand() %
                        (sizeof(k_mv_8ball) / sizeof(k_mv_8ball[0]))]);
    mv_chat_opts_t o = {0};
    o.action = true;
    o.no_transform = true;
    mv_public_chat(sess, sess->channel, text, &o);
}

static void mv_cmd_taco(ddial_session_t *sess, const char *msg)
{
    msg = mv_skip_ws(msg);
    char text[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(text, sizeof(text),
             "\033[33m   ,~~~~~~~~~,\033[0m\r\n"
             "\033[33m  /\033[32m~*~*~*~*~*\033[33m\\\033[0m  %s\r\n"
             "\033[33m (\033[31m@\033[32m~\033[31m@\033[32m~\033[31m@\033[32m~"
             "\033[31m@\033[32m~\033[31m@\033[33m)\033[0m\r\n"
             "\033[33m  \\_________/\033[0m",
             msg[0] != '\0' ? msg : "Taco time!");
    mv_chat_opts_t o = {0};
    o.block = true;
    mv_public_chat(sess, sess->channel, text, &o);
}

static void mv_cmd_bior(ddial_session_t *sess)
{
    ddial_member_t m;
    int y, mo, d;
    if (!ddial_member_get(sess->owner, sess->mv.member_no, &m) ||
        !mv_valid_date(m.bday, &y, &mo, &d)) {
        mv_line(sess, "* Set your birthday first: /bday YYYY-MM-DD");
        return;
    }
    struct tm born = {0};
    born.tm_year = y - 1900;
    born.tm_mon = mo - 1;
    born.tm_mday = d;
    born.tm_hour = 12;
    double days = difftime(time(nullptr), mktime(&born)) / 86400.0;
    static const struct {
        const char *name;
        double period;
    } cycles[] = {{"Physical", 23.0}, {"Emotional", 28.0},
                  {"Intellectual", 33.0}};
    mv_line(sess, "Biorhythms for today (day %.0f):", days);
    for (size_t i = 0U; i < 3U; ++i) {
        double v = sin(2.0 * M_PI * days / cycles[i].period);
        int pct = (int)lround(v * 100.0);
        int bars = (int)lround((v + 1.0) * 10.0);
        char bar[24];
        for (int k = 0; k < 20; ++k) {
            bar[k] = k < bars ? '#' : '.';
        }
        bar[20] = '\0';
        mv_line(sess, " %-12s [%s] %+4d%%", cycles[i].name, bar, pct);
    }
}

/* ---- HTTP helpers (/define, /giphy) --------------------------------------- */

typedef struct mv_http_buf {
    char data[65536];
    size_t len;
} mv_http_buf_t;

static size_t mv_http_write(void *ptr, size_t size, size_t nmemb, void *user)
{
    mv_http_buf_t *b = (mv_http_buf_t *)user;
    size_t n = size * nmemb;
    size_t room = sizeof(b->data) - 1U - b->len;
    size_t take = n < room ? n : room;
    memcpy(b->data + b->len, ptr, take);
    b->len += take;
    b->data[b->len] = '\0';
    return n;
}

static bool mv_http_get(const char *url, mv_http_buf_t *out)
{
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        return false;
    }
    out->len = 0U;
    out->data[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ssh-chatter-ddial/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mv_http_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0L;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK && status == 200L;
}

/* Extract the JSON string value following "key": starting at from. */
static const char *mv_json_string(const char *from, const char *key,
                                  char *out, size_t cap)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *p = strstr(from, needle);
    if (p == nullptr) {
        return nullptr;
    }
    p += strlen(needle);
    size_t n = 0U;
    while (*p != '\0' && *p != '"' && n + 1U < cap) {
        if (*p == '\\' && p[1] != '\0') {
            ++p;
            out[n++] = *p == 'n' ? ' ' : *p;
        } else {
            out[n++] = *p;
        }
        ++p;
    }
    out[n] = '\0';
    return p;
}

static void mv_url_escape(const char *src, char *dst, size_t cap)
{
    CURL *curl = curl_easy_init();
    char *escaped = curl != nullptr ? curl_easy_escape(curl, src, 0) : nullptr;
    snprintf(dst, cap, "%s", escaped != nullptr ? escaped : "");
    if (escaped != nullptr) {
        curl_free(escaped);
    }
    if (curl != nullptr) {
        curl_easy_cleanup(curl);
    }
}

static void mv_cmd_define(ddial_session_t *sess, const char *word)
{
    word = mv_skip_ws(word);
    if (word[0] == '\0') {
        mv_line(sess, "* Usage: /define <word>");
        return;
    }
    char escaped[256];
    mv_url_escape(word, escaped, sizeof(escaped));
    char url[512];
    snprintf(url, sizeof(url),
             "https://api.dictionaryapi.dev/api/v2/entries/en/%s", escaped);
    static mv_http_buf_t buf; /* 64 KiB: keep it off the thread stack */
    static pthread_mutex_t buf_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&buf_lock);
    if (!mv_http_get(url, &buf)) {
        pthread_mutex_unlock(&buf_lock);
        mv_line(sess, "* No definition found for '%s'.", word);
        return;
    }
    mv_line(sess, "Definition of '%s':", word);
    const char *p = buf.data;
    for (int i = 0; i < 3 && p != nullptr; ++i) {
        char def[400];
        p = mv_json_string(p, "definition", def, sizeof(def));
        if (p != nullptr) {
            mv_line(sess, " %d. %s", i + 1, def);
        }
    }
    pthread_mutex_unlock(&buf_lock);
}

static void mv_cmd_giphy(ddial_session_t *sess, const char *keyword)
{
    keyword = mv_skip_ws(keyword);
    if (keyword[0] == '\0') {
        mv_line(sess, "* Usage: /giphy <keyword>");
        return;
    }
    const char *key = getenv("CHATTER_GIPHY_API_KEY");
    if (key == nullptr || key[0] == '\0') {
        mv_line(sess, "* GIF search is not configured on this station "
                      "(CHATTER_GIPHY_API_KEY).");
        return;
    }
    char escaped[256];
    mv_url_escape(keyword, escaped, sizeof(escaped));
    char url[768];
    snprintf(url, sizeof(url),
             "https://api.giphy.com/v1/gifs/search?api_key=%s&q=%s&limit=1"
             "&rating=pg-13",
             key, escaped);
    static mv_http_buf_t buf;
    static pthread_mutex_t buf_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&buf_lock);
    char gif[512] = "";
    if (mv_http_get(url, &buf)) {
        const char *data = strstr(buf.data, "\"data\"");
        if (data != nullptr) {
            mv_json_string(data, "url", gif, sizeof(gif));
        }
    }
    pthread_mutex_unlock(&buf_lock);
    if (gif[0] == '\0') {
        mv_line(sess, "* No GIF found for '%s'.", keyword);
        return;
    }
    char text[768];
    snprintf(text, sizeof(text), "[GIF: %s] %s", keyword, gif);
    mv_chat_opts_t o = {0};
    o.no_transform = true;
    mv_public_chat(sess, sess->channel, text, &o);
}

/* ---- member commands ------------------------------------------------------ */

static bool mv_load_self(ddial_session_t *sess, ddial_member_t *m)
{
    if (!ddial_member_get(sess->owner, sess->mv.member_no, m)) {
        mv_line(sess, "* Member record not found.");
        return false;
    }
    return true;
}

static void mv_cmd_signup(ddial_session_t *sess, const char *password)
{
    password = mv_skip_ws(password);
    if (mv_is_member(sess)) {
        mv_line(sess, "* You are already member #%u.",
                (unsigned)sess->mv.member_no);
        return;
    }
    if (password[0] == '\0') {
        mv_line(sess, "* Usage: /signup <password>  (registers your current "
                      "handle)");
        return;
    }
    char err[128] = "";
    uint32_t no =
        ddial_member_signup(sess->owner, sess->handle, password, err,
                            sizeof(err));
    if (no == 0U) {
        mv_line(sess, "* Signup failed: %s", err);
        return;
    }
    ddial_member_t m;
    if (ddial_member_get(sess->owner, no, &m)) {
        m.logins = 1U;
        ddial_member_put(sess->owner, &m);
        mv_apply_member(sess, &m);
    }
    mv_line(sess, "* Welcome aboard! You are member #%u.", (unsigned)no);
    mv_line(sess, "* Next time log in with: %u%s   (or %u:<password>)",
            (unsigned)no, "<password>", (unsigned)no);
    char note[128];
    snprintf(note, sizeof(note), "* %s is now member #%u.", sess->handle,
             (unsigned)no);
    mv_notice_channel(sess, sess->channel, note);
}

static void mv_cmd_handle(ddial_session_t *sess, const char *arg)
{
    arg = mv_skip_ws(arg);
    host_t *host = sess->owner;

    if (arg[0] == '?') {
        ddial_member_t m;
        if (!mv_require_member(sess) || !mv_load_self(sess, &m)) {
            return;
        }
        mv_line(sess, "Saved handles:");
        for (size_t i = 0U; i < DDIAL_MV_SAVED_HANDLES; ++i) {
            if (m.saved_handles[i][0] != '\0') {
                mv_line(sess, " /h%zu  %s", i + 1U, m.saved_handles[i]);
            }
        }
        return;
    }

    /* /h#=handle (save) and /h# (activate) */
    if (arg[0] >= '1' && arg[0] <= '9' &&
        (arg[1] == '=' || arg[1] == '\0')) {
        ddial_member_t m;
        if (!mv_require_member(sess) || !mv_load_self(sess, &m)) {
            return;
        }
        size_t slot = (size_t)(arg[0] - '1');
        if (arg[1] == '=') {
            char clean[DDIAL_MAX_HANDLE_LEN];
            ddial_sanitize_handle(arg + 2, strlen(arg + 2), clean,
                                  sizeof(clean));
            snprintf(m.saved_handles[slot], sizeof(m.saved_handles[slot]),
                     "%s", clean);
            ddial_member_put(host, &m);
            mv_line(sess, "* Saved handle %zu: %s", slot + 1U,
                    clean[0] != '\0' ? clean : "(cleared)");
            return;
        }
        if (m.saved_handles[slot][0] == '\0') {
            mv_line(sess, "* Handle %zu is empty. Save one with /h%zu=Name",
                    slot + 1U, slot + 1U);
            return;
        }
        arg = m.saved_handles[slot];
        char copy[DDIAL_MAX_HANDLE_LEN];
        snprintf(copy, sizeof(copy), "%s", arg);
        mv_cmd_handle(sess, copy);
        return;
    }

    if (arg[0] == '\0') {
        mv_cmd_help(sess);
        return;
    }

    char display[256] = "";
    char plain[DDIAL_MAX_HANDLE_LEN * 2U];
    bool colored = arg[0] == '#';
    if (colored) {
        if (!mv_require_member(sess)) {
            return;
        }
        mv_render_handle(arg, display, sizeof(display), plain, sizeof(plain));
    } else {
        snprintf(plain, sizeof(plain), "%s", arg);
    }
    char clean[DDIAL_MAX_HANDLE_LEN];
    if (ddial_sanitize_handle(plain, strlen(plain), clean, sizeof(clean)) ==
            0U ||
        clean[0] == '\0') {
        mv_line(sess, "* Invalid handle.");
        return;
    }
    uint32_t owner = ddial_member_owner_of(host, clean);
    if (owner != 0U && owner != sess->mv.member_no) {
        mv_line(sess, "* '%s' belongs to member #%u.", clean, (unsigned)owner);
        return;
    }
    ddial_session_t *other = mv_find_handle(host, clean);
    if (other != nullptr && other != sess) {
        mv_line(sess, "* '%s' is in use.", clean);
        return;
    }

    char old[DDIAL_MAX_HANDLE_LEN];
    snprintf(old, sizeof(old), "%s", sess->handle);
    host_ddial_client_send_logout(host, sess->slot,
                                  DDIAL_MV_LINK_CHANNEL(sess->channel),
                                  mv_tier(sess), sess->handle,
                                  (uint16_t)sess->mv.member_no);
    snprintf(sess->handle, sizeof(sess->handle), "%s", clean);
    snprintf(sess->mv.display_handle, sizeof(sess->mv.display_handle), "%s",
             colored ? display : "");
    host_ddial_client_send_login(host, sess->slot,
                                 DDIAL_MV_LINK_CHANNEL(sess->channel),
                                 mv_tier(sess), sess->handle,
                                 (uint16_t)sess->mv.member_no);

    if (mv_is_member(sess)) {
        ddial_member_t m;
        if (ddial_member_get(host, sess->mv.member_no, &m)) {
            snprintf(m.handle, sizeof(m.handle), "%s", clean);
            snprintf(m.handle_color, sizeof(m.handle_color), "%s",
                     colored ? arg : "");
            ddial_member_put(host, &m);
        }
    }
    if (strcmp(old, clean) != 0) {
        char note[160];
        snprintf(note, sizeof(note), "* #%u %s is now %s.",
                 (unsigned)sess->slot, old, clean);
        mv_notice_channel(sess, sess->channel, note);
    } else {
        mv_line(sess, "* Handle updated.");
    }
}

static void mv_cmd_profile_show(ddial_session_t *sess, uint32_t no)
{
    ddial_member_t m;
    if (!ddial_member_get(sess->owner, no, &m)) {
        mv_line(sess, "* No member #%u.", (unsigned)no);
        return;
    }
    char created[32], seen[32];
    mv_format_time((time_t)m.created, created, sizeof(created));
    mv_format_time((time_t)m.last_seen, seen, sizeof(seen));
    mv_line(sess, "Profile of member #%u: %s%s", (unsigned)m.number, m.handle,
            mv_find_member(sess->owner, no) != nullptr ? " (online)" : "");
    mv_line(sess, " Member since %s, last seen %s", created, seen);
    if (m.status[0] != '\0') {
        mv_line(sess, " Status: %s", m.status);
    }
    int y, mo, d;
    if (mv_valid_date(m.bday, &y, &mo, &d)) {
        mv_line(sess, " Birthday: %02d-%02d", mo, d);
    }
    mv_line(sess, " %s", m.profile[0] != '\0' ? m.profile : "(no profile)");
}

static void mv_cmd_profile(ddial_session_t *sess, const char *arg)
{
    if (arg[0] == '=') {
        if (!mv_require_member(sess)) {
            return;
        }
        const char *text = mv_skip_ws(arg + 1);
        if (text[0] == '\0') {
            sess->mv.profile_capture = true;
            mv_line(sess, "* Type your profile on the next line:");
            return;
        }
        ddial_member_t m;
        if (mv_load_self(sess, &m)) {
            snprintf(m.profile, sizeof(m.profile), "%s", text);
            ddial_member_put(sess->owner, &m);
            mv_line(sess, "* Profile saved.");
        }
        return;
    }
    const char *p = arg;
    uint32_t no = 0U;
    if (!mv_parse_uint(&p, &no)) {
        if (mv_is_member(sess)) {
            mv_cmd_profile_show(sess, sess->mv.member_no);
        } else {
            mv_line(sess, "* Usage: /r<member#>   (members: /r= to set)");
        }
        return;
    }
    mv_cmd_profile_show(sess, no);
}

static void mv_boxes_cb(const ddial_member_t *m, void *user)
{
    mv_member_list_ctx_t *c = (mv_member_list_ctx_t *)user;
    if (m->box[0] != '\0') {
        char line[96];
        snprintf(line, sizeof(line), " /m%u  %s", (unsigned)m->number,
                 m->handle);
        ddial_session_write_line(c->viewer, line);
        ++c->count;
    }
}

static void mv_box_dir(host_t *host, char *out, size_t cap)
{
    snprintf(out, cap, "%s/ddial_boxes",
             host->user_data_root[0] != '\0' ? host->user_data_root : ".");
}

static void mv_print_box_text(ddial_session_t *sess, const char *text,
                              bool rotate)
{
    /* Boxes hold lines separated by '|'.  With rotation on, show one line
     * that changes every minute. */
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", text);
    char *parts[32];
    size_t n = 0U;
    char *save = nullptr;
    for (char *tok = strtok_r(copy, "|", &save); tok != nullptr && n < 32U;
         tok = strtok_r(nullptr, "|", &save)) {
        parts[n++] = tok;
    }
    if (n == 0U) {
        return;
    }
    if (rotate) {
        mv_line(sess, " %s", mv_skip_ws(parts[(size_t)(time(nullptr) / 60) % n]));
        return;
    }
    for (size_t i = 0U; i < n; ++i) {
        mv_line(sess, " %s", mv_skip_ws(parts[i]));
    }
}

static void mv_cmd_box(ddial_session_t *sess, const char *arg)
{
    host_t *host = sess->owner;
    if (arg[0] == '=') {
        if (!mv_require_member(sess)) {
            return;
        }
        ddial_member_t m;
        if (!mv_load_self(sess, &m)) {
            return;
        }
        const char *text = mv_skip_ws(arg + 1);
        if (strcasecmp(text, "rotate") == 0) {
            m.box_rotate = !m.box_rotate;
            mv_line(sess, "* Box rotation %s.", m.box_rotate ? "on" : "off");
        } else {
            snprintf(m.box, sizeof(m.box), "%s", text);
            mv_line(sess, text[0] != '\0'
                              ? "* Message box saved (use | between lines)."
                              : "* Message box cleared.");
        }
        ddial_member_put(host, &m);
        return;
    }

    char dir[PATH_MAX];
    mv_box_dir(host, dir, sizeof(dir));

    if (isalpha((unsigned char)arg[0]) && !isalpha((unsigned char)arg[1])) {
        char path[PATH_MAX + 8];
        snprintf(path, sizeof(path), "%s/%c.txt", dir,
                 tolower((unsigned char)arg[0]));
        FILE *fp = fopen(path, "r");
        if (fp == nullptr) {
            mv_line(sess, "* Box %c is empty.", tolower((unsigned char)arg[0]));
            return;
        }
        char line[512];
        for (int i = 0; i < 40 && fgets(line, sizeof(line), fp) != nullptr;
             ++i) {
            line[strcspn(line, "\r\n")] = '\0';
            ddial_session_write_line(sess, line);
        }
        fclose(fp);
        return;
    }

    const char *p = arg;
    uint32_t no = 0U;
    if (mv_parse_uint(&p, &no)) {
        ddial_member_t m;
        if (!ddial_member_get(host, no, &m) || m.box[0] == '\0') {
            mv_line(sess, "* Member #%u has no message box.", (unsigned)no);
            return;
        }
        mv_line(sess, "Message box of %s:", m.handle);
        mv_print_box_text(sess, m.box, m.box_rotate);
        return;
    }

    mv_line(sess, "Viewable boxes:");
    DIR *d = opendir(dir);
    if (d != nullptr) {
        struct dirent *ent;
        char letters[32] = "";
        while ((ent = readdir(d)) != nullptr) {
            if (isalpha((unsigned char)ent->d_name[0]) &&
                strcmp(ent->d_name + 1, ".txt") == 0) {
                size_t l = strlen(letters);
                if (l + 2U < sizeof(letters)) {
                    letters[l] = (char)tolower((unsigned char)ent->d_name[0]);
                    letters[l + 1U] = '\0';
                }
            }
        }
        closedir(d);
        if (letters[0] != '\0') {
            mv_line(sess, " Station boxes: /m + one of [%s]", letters);
        }
    }
    mv_member_list_ctx_t c = {sess, nullptr, 0U};
    ddial_member_foreach(host, mv_boxes_cb, &c);
    if (c.count == 0U) {
        mv_line(sess, " (no member boxes yet; members set one with /m=)");
    }
}

static void mv_mail_list_cb(const ddial_mail_t *mail, void *user)
{
    ddial_session_t *sess = (ddial_session_t *)user;
    char when[32];
    mv_format_time((time_t)mail->sent, when, sizeof(when));
    mv_line(sess, " [%u] %s from %s (#%u): %s", (unsigned)mail->id, when,
            mail->from_handle, (unsigned)mail->from, mail->text);
}

static void mv_cmd_email(ddial_session_t *sess, const char *arg)
{
    host_t *host = sess->owner;
    if (arg[0] == '?') {
        static const char *const lines[] = {
            "Email commands:",
            "/e          list emails received",
            "/e=# <msg>  send email to member#",
            "/e-#        delete email#",
            "/e-all      delete all emails",
            nullptr,
        };
        mv_lines(sess, lines);
        return;
    }
    if (!mv_require_member(sess)) {
        return;
    }
    if (arg[0] == '=') {
        const char *p = arg + 1;
        uint32_t to = 0U;
        if (!mv_parse_uint(&p, &to)) {
            mv_line(sess, "* Usage: /e=<member#> <message>");
            return;
        }
        p = mv_skip_ws(p);
        if (p[0] == '\0') {
            mv_line(sess, "* Usage: /e=<member#> <message>");
            return;
        }
        if (ddial_mail_send(host, to, sess->mv.member_no, sess->handle, p,
                            false) == 0U) {
            mv_line(sess, "* No member #%u (or mailbox full).", (unsigned)to);
            return;
        }
        mv_line(sess, "* Email sent to member #%u.", (unsigned)to);
        ddial_session_t *t = mv_find_member(host, to);
        if (t != nullptr) {
            ddial_session_write_line(t, "* You have new email. Type /e.");
            mv_bell(t, DDIAL_MV_BEEP_PM);
        }
        return;
    }
    if (arg[0] == '-') {
        uint32_t id = 0U;
        if (strcasecmp(arg + 1, "all") == 0) {
            size_t n = ddial_mail_delete(host, sess->mv.member_no, 0U);
            mv_line(sess, "* Deleted %zu email%s.", n, n == 1U ? "" : "s");
            return;
        }
        const char *p = arg + 1;
        if (!mv_parse_uint(&p, &id) || id == 0U) {
            mv_line(sess, "* Usage: /e-<email#> or /e-all");
            return;
        }
        mv_line(sess, ddial_mail_delete(host, sess->mv.member_no, id) > 0U
                          ? "* Email deleted."
                          : "* No such email.");
        return;
    }
    mv_line(sess, "Your email:");
    if (ddial_mail_visit(host, sess->mv.member_no, false, false,
                         mv_mail_list_cb, sess) == 0U) {
        mv_line(sess, " (empty)");
    }
}

static void mv_cmd_offline(ddial_session_t *sess, const char *arg, bool buzz)
{
    host_t *host = sess->owner;
    const char *p = mv_skip_ws(arg);
    uint32_t to = 0U;
    if (!mv_parse_uint(&p, &to)) {
        mv_line(sess, buzz ? "* Usage: /buzz <member#> [message]"
                           : "* Usage: /o<member#> <message>");
        return;
    }
    p = mv_skip_ws(p);
    if (!buzz && p[0] == '\0') {
        mv_line(sess, "* Usage: /o<member#> <message>");
        return;
    }
    ddial_member_t m;
    if (!ddial_member_get(host, to, &m)) {
        mv_line(sess, "* No member #%u.", (unsigned)to);
        return;
    }
    if (buzz) {
        ddial_session_t *t = mv_find_member(host, to);
        if (t != nullptr) {
            mv_line(t, "* BUZZ! %s (#%u) is calling you%s%s", sess->handle,
                    (unsigned)sess->slot, p[0] != '\0' ? ": " : ".", p);
            if (t->mv.beep_on && t->mv.beep_volume > 0U &&
                (t->mv.beep_events & DDIAL_MV_BEEP_BUZZ) != 0U) {
                ddial_session_write_raw(t, "\a\a\a", 3U);
            }
            mv_line(sess, "* Buzzed %s.", m.handle);
            return;
        }
        char text[DDIAL_MV_TEXT_LIMIT];
        snprintf(text, sizeof(text), "BUZZ - come online!%s%s",
                 p[0] != '\0' ? " " : "", p);
        ddial_mail_send(host, to, sess->mv.member_no, sess->handle, text,
                        true);
        mv_line(sess, "* %s is offline; the buzz waits for their next login.",
                m.handle);
        return;
    }
    ddial_mail_send(host, to, sess->mv.member_no, sess->handle, p, true);
    mv_line(sess, "* %s will get your message at next login.", m.handle);
}

static void mv_cmd_bday(ddial_session_t *sess, const char *arg)
{
    ddial_member_t m;
    if (!mv_load_self(sess, &m)) {
        return;
    }
    arg = mv_skip_ws(arg);
    int y, mo, d;
    if (strcasecmp(arg, "remove") == 0) {
        m.bday[0] = '\0';
        m.bday_countdown = false;
        mv_line(sess, "* Birthday removed.");
    } else if (strcmp(arg, "+") == 0 || strcmp(arg, "-") == 0) {
        if (m.bday[0] == '\0') {
            mv_line(sess, "* Set your birthday first: /bday YYYY-MM-DD");
            return;
        }
        m.bday_countdown = arg[0] == '+';
        mv_line(sess, "* Birthday countdown %s.",
                m.bday_countdown ? "shown" : "hidden");
    } else if (arg[0] == '\0') {
        mv_line(sess, m.bday[0] != '\0' ? "* Your birthday: %s"
                                        : "* No birthday set.%s",
                m.bday);
        return;
    } else if (mv_valid_date(arg, &y, &mo, &d)) {
        snprintf(m.bday, sizeof(m.bday), "%s", arg);
        mv_line(sess, "* Birthday saved. Only members see a notice on the "
                      "day; your age stays private.");
    } else {
        mv_line(sess, "* Usage: /bday YYYY-MM-DD, /bday+, /bday-, "
                      "/bdayRemove");
        return;
    }
    ddial_member_put(sess->owner, &m);
}

static void mv_cmd_stats(ddial_session_t *sess)
{
    ddial_member_t m;
    if (!mv_load_self(sess, &m)) {
        return;
    }
    char created[32];
    mv_format_time((time_t)m.created, created, sizeof(created));
    long online = (long)(time(nullptr) - sess->mv.login_time);
    mv_line(sess, "Stats for %s (member #%u):", m.handle, (unsigned)m.number);
    mv_line(sess, " Member since:   %s", created);
    mv_line(sess, " Logins:         %u", (unsigned)m.logins);
    mv_line(sess, " Messages:       %u (+%u this call)", (unsigned)m.messages,
            (unsigned)sess->mv.messages_sent);
    mv_line(sess, " Online now:     %ldh %02ldm on line #%u, channel %u",
            online / 3600, (online / 60) % 60, (unsigned)sess->slot,
            (unsigned)sess->channel);
}

static void mv_cmd_whois(ddial_session_t *sess, const char *arg)
{
    const char *p = arg;
    uint32_t slot = 0U;
    if (!mv_parse_uint(&p, &slot) || slot > UINT16_MAX) {
        mv_line(sess, "* Usage: /whois<line#>");
        return;
    }
    ddial_session_t *t = mv_find_slot(sess->owner, (uint16_t)slot);
    if (t == nullptr) {
        char username[SSH_CHATTER_USERNAME_LEN];
        if (host_ddial_chat_link_username(sess->owner, (uint16_t)slot,
                                          username, sizeof(username))) {
            mv_line(sess, "* Line #%u is Chatter user %s.", (unsigned)slot,
                    username);
        } else {
            mv_line(sess, "* Nobody on line #%u.", (unsigned)slot);
        }
        return;
    }
    if (!mv_is_member(t)) {
        mv_line(sess, "* #%u %s is a guest.", (unsigned)slot, t->handle);
        return;
    }
    ddial_member_t m;
    if (!ddial_member_get(sess->owner, t->mv.member_no, &m)) {
        return;
    }
    mv_line(sess, "#%u %s (member #%u) also uses:", (unsigned)slot, t->handle,
            (unsigned)m.number);
    size_t shown = 0U;
    for (size_t i = 0U; i < DDIAL_MV_SAVED_HANDLES; ++i) {
        if (m.saved_handles[i][0] != '\0') {
            mv_line(sess, " %s", m.saved_handles[i]);
            ++shown;
        }
    }
    if (shown == 0U) {
        mv_line(sess, " (no other handles)");
    }
}

static void mv_cmd_link_settings(ddial_session_t *sess)
{
    ddial_relay_t *relay = &sess->owner->ddial_relay;
    mv_line(sess, "Link settings:");
    mv_line(sess, " Station Link: %s%s%s",
            relay->enabled && relay->connected ? "connected to "
            : relay->enabled                   ? "connecting to "
                                               : "offline",
            relay->enabled ? relay->host : "", relay->enabled ? "" : "");
    mv_line(sess, " You are line #%u on channel %u: %s", (unsigned)sess->slot,
            (unsigned)sess->channel,
            sess->channel == DDIAL_MV_ROOM_CHANNEL
                ? "shared with the Chatter room and the link"
            : sess->channel <= DDIAL_MAX_CHANNEL ? "shared over the link"
                                                 : "local to this station");
    mv_line(sess, " /ps <msg> keeps a message on this station only.");
}

/* ---- toggles & preferences ---------------------------------------------- */

static void mv_toggle_msg(ddial_session_t *sess, bool *flag, const char *arg,
                          const char *label)
{
    arg = mv_skip_ws(arg);
    if (arg[0] == '+') {
        *flag = true;
    } else if (arg[0] == '-') {
        *flag = false;
    } else if (strcasecmp(arg, "show") != 0) {
        *flag = !*flag;
    }
    mv_line(sess, "* %s %s.", label, *flag ? "on" : "off");
    mv_save_member(sess);
}

static const struct {
    const char *name;
    uint8_t bit;
} k_mv_beep_events[] = {
    {"pm", DDIAL_MV_BEEP_PM},       {"mention", DDIAL_MV_BEEP_MENTION},
    {"login", DDIAL_MV_BEEP_LOGIN}, {"chat", DDIAL_MV_BEEP_CHAT},
    {"buzz", DDIAL_MV_BEEP_BUZZ},
};

static void mv_cmd_beep(ddial_session_t *sess, const char *arg)
{
    if (arg[0] == '?') {
        mv_line(sess, "Beep is %s, volume %u%%. Events:",
                sess->mv.beep_on ? "on" : "off",
                (unsigned)sess->mv.beep_volume);
        for (size_t i = 0U;
             i < sizeof(k_mv_beep_events) / sizeof(k_mv_beep_events[0]); ++i) {
            mv_line(sess, " %-8s %s", k_mv_beep_events[i].name,
                    (sess->mv.beep_events & k_mv_beep_events[i].bit) != 0U
                        ? "on"
                        : "off");
        }
        mv_line(sess, "Use /b+<event> or /b-<event>, e.g. /b+login");
        return;
    }
    if (arg[0] == '+' || arg[0] == '-') {
        for (size_t i = 0U;
             i < sizeof(k_mv_beep_events) / sizeof(k_mv_beep_events[0]); ++i) {
            if (strcasecmp(arg + 1, k_mv_beep_events[i].name) == 0) {
                if (arg[0] == '+') {
                    sess->mv.beep_events |= k_mv_beep_events[i].bit;
                } else {
                    sess->mv.beep_events &= (uint8_t)~k_mv_beep_events[i].bit;
                }
                mv_line(sess, "* Beep on %s %s.", k_mv_beep_events[i].name,
                        arg[0] == '+' ? "on" : "off");
                mv_save_member(sess);
                return;
            }
        }
        mv_line(sess, "* Unknown event. Type /b? for the list.");
        return;
    }
    sess->mv.beep_on = !sess->mv.beep_on;
    mv_line(sess, "* Beep %s.", sess->mv.beep_on ? "on" : "off");
    mv_save_member(sess);
}

static void mv_cmd_color(ddial_session_t *sess, const char *arg)
{
    arg = mv_skip_ws(arg);
    if (arg[0] == '\0') {
        sess->mv.msg_color[0] = '\0';
        mv_line(sess, "* Message colour cleared.");
        mv_save_member(sess);
        return;
    }
    mv_rgb_t a, b;
    mv_color_mode_t mode = mv_parse_color_spec(arg, &a, &b);
    if (mode == MV_COLOR_NONE) {
        mv_line(sess, "* Usage: /cRRGGBB or /cRRGGBB,RRGGBB (gradient)");
        return;
    }
    char spec[16];
    const char *s = arg[0] == '#' ? arg + 1 : arg;
    if (mode == MV_COLOR_GRADIENT) {
        const char *c2 = s + 7;
        if (*c2 == '#') {
            ++c2;
        }
        snprintf(spec, sizeof(spec), "%.6s,%.6s", s, c2);
    } else {
        snprintf(spec, sizeof(spec), "%.6s", s);
    }
    snprintf(sess->mv.msg_color, sizeof(sess->mv.msg_color), "%s", spec);
    char styled[256], plain[128];
    mv_render("This is your new colour.", mode, a, b, styled, sizeof(styled),
              plain, sizeof(plain));
    mv_line(sess, "* %s", styled);
    mv_save_member(sess);
}

static void mv_cmd_affix(ddial_session_t *sess, char *field, size_t cap,
                         const char *arg, const char *label)
{
    arg = mv_skip_ws(arg);
    snprintf(field, cap, "%s", arg);
    mv_line(sess, arg[0] != '\0' ? "* %s set." : "* %s cleared.", label);
    mv_save_member(sess);
}

static void mv_cmd_quit(ddial_session_t *sess, const char *arg)
{
    if (arg[0] == '+') {
        snprintf(sess->mv.quit_msg, sizeof(sess->mv.quit_msg), "%s",
                 mv_skip_ws(arg + 1));
        sess->mv.quit_deadline = time(nullptr) + 10;
        mv_line(sess, "* Leaving in 10 seconds.");
        return;
    }
    snprintf(sess->mv.quit_msg, sizeof(sess->mv.quit_msg), "%s",
             mv_skip_ws(arg));
    ddial_session_write_line(sess, "Goodbye!");
    sess->should_exit = true;
}

static void mv_cmd_baud(ddial_session_t *sess, const char *arg)
{
    const char *p = mv_skip_ws(arg);
    uint32_t rate = 0U;
    if (!mv_parse_uint(&p, &rate)) {
        mv_line(sess, "* Baud rate is %u. Set with /baud 0|300|600|1200|2400|"
                      "9600",
                (unsigned)sess->mv.baud);
        return;
    }
    if (rate != 0U && rate != 300U && rate != 600U && rate != 1200U &&
        rate != 2400U && rate != 9600U) {
        mv_line(sess, "* Choose 0 (fast), 300, 600, 1200, 2400 or 9600.");
        return;
    }
    sess->mv.baud = (uint16_t)rate;
    mv_line(sess, rate == 0U ? "* Baud: full speed." : "* Baud set to %u.",
            (unsigned)rate);
    mv_save_member(sess);
}

/* ---- moderation ------------------------------------------------------------ */

static void mv_cmd_ignore(ddial_session_t *sess, const char *arg)
{
    const char *p = arg;
    uint32_t v = 0U;
    if (arg[0] == '+' || arg[0] == '-') {
        p = arg + 1;
        if (!mv_parse_uint(&p, &v)) {
            mv_line(sess, "* Usage: /ig+<member#> or /ig-<member#>");
            return;
        }
        if (arg[0] == '+') {
            mv_list_add(&sess->mv.ignore_members, v);
            mv_line(sess, "* Ignoring member #%u.", (unsigned)v);
        } else {
            mv_list_remove(&sess->mv.ignore_members, v);
            mv_list_remove(&sess->mv.ignore_slots, v);
            mv_line(sess, "* No longer ignoring #%u.", (unsigned)v);
        }
        return;
    }
    if (!mv_parse_uint(&p, &v)) {
        mv_line(sess, "* Usage: /ig<line#>, /ig+<member#>, /ig-<member#>");
        return;
    }
    mv_line(sess, mv_list_toggle(&sess->mv.ignore_slots, v)
                      ? "* Ignoring public messages from line #%u."
                      : "* Showing public messages from line #%u again.",
            (unsigned)v);
}

static void mv_cmd_slot_toggle(ddial_session_t *sess, const char *arg,
                               ddial_mv_slot_list_t *list, const char *on,
                               const char *off, const char *usage)
{
    const char *p = arg;
    uint32_t v = 0U;
    if (!mv_parse_uint(&p, &v)) {
        mv_line(sess, "%s", usage);
        return;
    }
    mv_line(sess, "* %s line #%u.", mv_list_toggle(list, v) ? on : off,
            (unsigned)v);
}

static void mv_cmd_full_mute(ddial_session_t *sess, const char *arg)
{
    if (!sess->mv.cosysop) {
        mv_line(sess, "* Only CoSysops can full-mute.");
        return;
    }
    const char *p = arg;
    uint32_t slot = 0U;
    if (!mv_parse_uint(&p, &slot) || slot > UINT16_MAX) {
        mv_line(sess, "* Usage: /xx<line#>");
        return;
    }
    ddial_session_t *t = mv_find_slot(sess->owner, (uint16_t)slot);
    if (t == nullptr) {
        mv_line(sess, "* Nobody on line #%u.", (unsigned)slot);
        return;
    }
    if (mv_is_member(t)) {
        mv_line(sess, "* Full mute applies to guests only.");
        return;
    }
    t->mv.full_muted = !t->mv.full_muted;
    mv_line(t, t->mv.full_muted ? "* You have been muted by a CoSysop."
                                : "* You have been unmuted.");
    mv_line(sess, "* Line #%u %s.", (unsigned)slot,
            t->mv.full_muted ? "muted" : "unmuted");
}

static void mv_cmd_boot(ddial_session_t *sess, const char *arg)
{
    const char *p = arg;
    uint32_t slot = 0U;
    if (!mv_parse_uint(&p, &slot) || slot > UINT16_MAX) {
        mv_line(sess, "* Usage: /tb<line#>");
        return;
    }
    if (sess->channel == DDIAL_MV_ROOM_CHANNEL && !sess->mv.cosysop) {
        mv_line(sess, "* Nobody can be booted from the main channel.");
        return;
    }
    if (!mv_is_moderator(sess, sess->channel)) {
        mv_line(sess, "* Only the channel moderator (first person in) can "
                      "boot.");
        return;
    }
    ddial_session_t *t = mv_find_slot(sess->owner, (uint16_t)slot);
    if (t == nullptr || t->channel != sess->channel || t == sess) {
        mv_line(sess, "* Line #%u is not on your channel.", (unsigned)slot);
        return;
    }
    if (t->mv.cosysop) {
        mv_line(sess, "* CoSysops cannot be booted.");
        return;
    }
    uint16_t channel = sess->channel;
    mv_add_boot(channel, t->handle);
    t->channel = DDIAL_MV_ROOM_CHANNEL;
    t->mv.channel_joined_at = time(nullptr);
    mv_line(t, "* You were booted from channel %u by %s.", (unsigned)channel,
            sess->handle);
    char note[128];
    snprintf(note, sizeof(note), "* #%u %s was booted from the channel.",
             (unsigned)slot, t->handle);
    mv_notice_channel(sess, channel, note);
}

static void mv_cmd_kick(ddial_session_t *sess, const char *arg)
{
    if (!sess->mv.cosysop) {
        mv_line(sess, "* Only CoSysops can disconnect users.");
        return;
    }
    const char *p = mv_skip_ws(arg);
    uint32_t slot = 0U;
    if (!mv_parse_uint(&p, &slot) || slot > UINT16_MAX) {
        mv_line(sess, "* Usage: /k<line#>");
        return;
    }
    ddial_session_t *t = mv_find_slot(sess->owner, (uint16_t)slot);
    if (t == nullptr || t == sess) {
        mv_line(sess, "* Nobody to disconnect on line #%u.", (unsigned)slot);
        return;
    }
    ddial_session_write_line(t, "* You have been disconnected by a CoSysop.");
    snprintf(t->mv.quit_msg, sizeof(t->mv.quit_msg), "disconnected by %s",
             sess->handle);
    t->should_exit = true;
}

static void mv_cmd_sentry(ddial_session_t *sess, const char *arg)
{
    arg = mv_skip_ws(arg);
    if ((arg[0] == '+' || arg[0] == '-')) {
        if (!sess->mv.cosysop) {
            mv_line(sess, "* Only CoSysops can change Sentry.");
            return;
        }
        pthread_mutex_lock(&g_mv_lock);
        g_mv_sentry = arg[0] == '+';
        pthread_mutex_unlock(&g_mv_lock);
        char note[96];
        snprintf(note, sizeof(note), "* Sentry %s by %s.",
                 arg[0] == '+' ? "activated" : "deactivated", sess->handle);
        mv_notice_channel(sess, 0U, note);
        return;
    }
    mv_line(sess, "* Sentry is %s (auto-mutes flooding guests).",
            g_mv_sentry ? "on" : "off");
}

/* ---- channel meta ---------------------------------------------------------- */

static void mv_cmd_topic(ddial_session_t *sess, const char *arg)
{
    arg = mv_skip_ws(arg);
    if (arg[0] == '\0') {
        mv_show_topic(sess, sess->channel);
        return;
    }
    pthread_mutex_lock(&g_mv_lock);
    ddial_mv_channel_meta_t *meta = mv_meta_locked(sess->channel, true);
    if (meta != nullptr) {
        snprintf(meta->topic, sizeof(meta->topic), "%s", arg);
    }
    pthread_mutex_unlock(&g_mv_lock);
    char note[256];
    snprintf(note, sizeof(note), "* %s suggests a topic: %s", sess->handle,
             arg);
    mv_notice_channel(sess, sess->channel, note);
}

static void mv_cmd_ask(ddial_session_t *sess, const char *arg)
{
    arg = mv_skip_ws(arg);
    if (arg[0] == '\0') {
        mv_show_topic(sess, sess->channel);
        return;
    }
    pthread_mutex_lock(&g_mv_lock);
    ddial_mv_channel_meta_t *meta = mv_meta_locked(sess->channel, true);
    if (meta != nullptr) {
        snprintf(meta->question, sizeof(meta->question), "%s", arg);
        snprintf(meta->asker, sizeof(meta->asker), "%s", sess->handle);
    }
    pthread_mutex_unlock(&g_mv_lock);
    char note[384];
    snprintf(note, sizeof(note), "? %s asks the channel: %s  (/answer <msg>)",
             sess->handle, arg);
    mv_notice_channel(sess, sess->channel, note);
}

static void mv_cmd_answer(ddial_session_t *sess, const char *arg)
{
    arg = mv_skip_ws(arg);
    char question[256] = "";
    pthread_mutex_lock(&g_mv_lock);
    ddial_mv_channel_meta_t *meta = mv_meta_locked(sess->channel, false);
    if (meta != nullptr) {
        snprintf(question, sizeof(question), "%s", meta->question);
    }
    pthread_mutex_unlock(&g_mv_lock);
    if (question[0] == '\0') {
        mv_line(sess, "* There is no open question. Ask one with /ask.");
        return;
    }
    if (arg[0] == '\0') {
        mv_line(sess, "* Usage: /answer <message>");
        return;
    }
    char note[640];
    snprintf(note, sizeof(note), "! %s answers \"%s\": %s", sess->handle,
             question, arg);
    mv_notice_channel(sess, sess->channel, note);
}

static void mv_cmd_mentions(ddial_session_t *sess)
{
    mv_line(sess, "Mention log:");
    if (sess->mv.mention_count == 0U) {
        mv_line(sess, " (nobody has mentioned you)");
        return;
    }
    for (size_t i = 0U; i < sess->mv.mention_count; ++i) {
        size_t idx = (sess->mv.mention_head + DDIAL_MV_MENTION_LOG -
                      sess->mv.mention_count + i) %
                     DDIAL_MV_MENTION_LOG;
        mv_line(sess, " %s", sess->mv.mentions[idx]);
    }
}

/* ---- dispatcher ------------------------------------------------------------ */

/* Match a command word.  Alphabetic names must not run into another letter
 * ("/sm" must not match "/smile").  *rest points after the name. */
static bool mv_is(const char *cmd, const char *name, const char **rest)
{
    size_t n = strlen(name);
    if (strncasecmp(cmd, name, n) != 0) {
        return false;
    }
    if (isalpha((unsigned char)name[n - 1U]) &&
        isalpha((unsigned char)cmd[n])) {
        return false;
    }
    *rest = cmd + n;
    return true;
}

static void mv_code_finish(ddial_session_t *sess)
{
    sess->mv.code_capture = false;
    if (sess->mv.code_len == 0U) {
        mv_line(sess, "* Code block discarded (empty).");
        return;
    }
    mv_chat_opts_t o = {0};
    o.block = true;
    mv_public_chat(sess, sess->channel, sess->mv.code_buf, &o);
    sess->mv.code_len = 0U;
    sess->mv.code_buf[0] = '\0';
}

static void mv_code_append(ddial_session_t *sess, const char *line)
{
    size_t n = strlen(line);
    size_t room = sizeof(sess->mv.code_buf) - sess->mv.code_len;
    if (n + 3U > room) {
        mv_line(sess, "* Code block is full; finishing it.");
        mv_code_finish(sess);
        return;
    }
    if (sess->mv.code_len > 0U) {
        memcpy(sess->mv.code_buf + sess->mv.code_len, "\r\n", 2U);
        sess->mv.code_len += 2U;
    }
    memcpy(sess->mv.code_buf + sess->mv.code_len, line, n);
    sess->mv.code_len += n;
    sess->mv.code_buf[sess->mv.code_len] = '\0';
}

/* Handle one logged-in line.  Returns true when consumed; false lets the
 * legacy DDial parser in server.c have it. */
static bool ddial_mv_dispatch(ddial_session_t *sess, const char *line)
{
    if (sess->mv.code_capture) {
        const char *t = mv_skip_ws(line);
        if (strcasecmp(t, "/code") == 0 || strcasecmp(t, "/end") == 0 ||
            strcmp(t, ".") == 0) {
            mv_code_finish(sess);
        } else {
            mv_code_append(sess, line);
        }
        return true;
    }
    if (sess->mv.profile_capture) {
        sess->mv.profile_capture = false;
        char cmd[DDIAL_MV_TEXT_LIMIT + 2U];
        snprintf(cmd, sizeof(cmd), "=%s", line);
        mv_cmd_profile(sess, cmd);
        return true;
    }

    const char *text = mv_skip_ws(line);
    if (sess->mv.full_muted && text[0] != '\0') {
        const char *q = text[0] == '/' ? text + 1 : "";
        if (!(tolower((unsigned char)q[0]) == 'q' &&
              !isalpha((unsigned char)q[1]))) {
            mv_line(sess, "* You are muted.");
            return true;
        }
    }

    if (text[0] == ';') {
        if (sess->mv.last_monitor == 0U) {
            mv_line(sess, "* Monitor a channel first with /t+#.");
            return true;
        }
        mv_public_chat(sess, sess->mv.last_monitor, text + 1, nullptr);
        return true;
    }
    if (text[0] != '/') {
        mv_public_chat(sess, sess->channel, text, nullptr);
        return true;
    }

    const char *cmd = text + 1;
    if (cmd[0] == '/') {
        ++cmd; /* "//s" is the same as "/s" */
    }
    const char *r = nullptr;
    ddial_mv_slot_list_t slots;

    /* --- symbols & numbers --- */
    if (mv_is(cmd, "?m", &r)) {
        mv_cmd_member_help(sess);
    } else if (mv_is(cmd, "@?", &r)) {
        mv_cmd_mentions(sess);
    } else if (strcmp(cmd, "+") == 0 || strcmp(cmd, "-") == 0) {
        sess->mv.auto_slash = cmd[0] == '+';
        mv_line(sess, "* Auto // commands %s (both / and // always work "
                      "here).",
                sess->mv.auto_slash ? "on" : "off");
        mv_save_member(sess);
    } else if (mv_is(cmd, "8ball", &r)) {
        if (mv_require_member(sess)) {
            mv_cmd_8ball(sess, r);
        }
    } else if (mv_is(cmd, "300", &r) || mv_is(cmd, "1200", &r) ||
               mv_is(cmd, "2400", &r)) {
        if (mv_require_member(sess)) {
            mv_chat_opts_t o = {0};
            o.pace_baud = (uint16_t)atoi(cmd);
            mv_public_chat(sess, sess->channel, r, &o);
        }
    }
    /* --- long words --- */
    else if (mv_is(cmd, "whois", &r)) {
        if (mv_require_member(sess)) {
            mv_cmd_whois(sess, r);
        }
    } else if (mv_is(cmd, "voice", &r)) {
        r = mv_skip_ws(r);
        if (r[0] == '?') {
            mv_line(sess, "Voice profiles are provided by web/GUI clients. "
                          "/voice toggles speech, /voice echo toggles "
                          "hearing your own messages.");
        } else if (strncasecmp(r, "echo", 4) == 0) {
            mv_toggle_msg(sess, &sess->mv.voice_echo, "", "Voice echo");
        } else {
            mv_toggle_msg(sess, &sess->mv.voice_on, r,
                          "Text-to-speech (GUI clients)");
        }
    } else if (mv_is(cmd, "topic", &r)) {
        if (mv_require_member(sess)) {
            mv_cmd_topic(sess, r);
        }
    } else if (mv_is(cmd, "taco", &r)) {
        if (mv_require_member(sess)) {
            mv_cmd_taco(sess, r);
        }
    } else if (mv_is(cmd, "stats", &r)) {
        if (mv_require_member(sess)) {
            mv_cmd_stats(sess);
        }
    } else if (mv_is(cmd, "spaces", &r)) {
        mv_cmd_spaces(sess, r);
    } else if (mv_is(cmd, "signup", &r)) {
        mv_cmd_signup(sess, r);
    } else if (mv_is(cmd, "sentry", &r)) {
        mv_cmd_sentry(sess, r);
    } else if (mv_is(cmd, "scramble", &r)) {
        if (mv_require_member(sess)) {
            if (mv_skip_ws(r)[0] != '\0') {
                bool saved = sess->mv.scramble_on;
                sess->mv.scramble_on = true;
                mv_public_chat(sess, sess->channel, r, nullptr);
                sess->mv.scramble_on = saved;
            } else {
                mv_toggle_msg(sess, &sess->mv.scramble_on, "", "Scramble");
            }
        }
    } else if (mv_is(cmd, "reverse", &r)) {
        if (mv_require_member(sess)) {
            if (mv_skip_ws(r)[0] != '\0') {
                bool saved = sess->mv.reverse_on;
                sess->mv.reverse_on = true;
                mv_public_chat(sess, sess->channel, r, nullptr);
                sess->mv.reverse_on = saved;
            } else {
                mv_toggle_msg(sess, &sess->mv.reverse_on, "", "Reverse");
            }
        }
    } else if (mv_is(cmd, "remember", &r)) {
        mv_toggle_msg(sess, &sess->mv.remember_on, r,
                      "Remember sign-in (GUI clients; terminals can log in "
                      "with ###password)");
    } else if (mv_is(cmd, "post", &r)) {
        if (mv_require_member(sess)) {
            mv_cmd_affix(sess, sess->mv.postfix, sizeof(sess->mv.postfix), r,
                         "Postfix");
        }
    } else if (mv_is(cmd, "pre", &r)) {
        if (mv_require_member(sess)) {
            mv_cmd_affix(sess, sess->mv.prefix, sizeof(sess->mv.prefix), r,
                         "Prefix");
        }
    } else if (mv_is(cmd, "null", &r)) {
        if (mv_require_member(sess)) {
            mv_cmd_slot_toggle(sess, r, &sess->mv.null_slots,
                               "Colour nullified for",
                               "Colour restored for",
                               "* Usage: /null<line#>");
        }
    } else if (mv_is(cmd, "lorem", &r)) {
        if (mv_require_member(sess)) {
            mv_toggle_msg(sess, &sess->mv.lorem_on, r, "Lorem ipsum");
        }
    } else if (mv_is(cmd, "image", &r)) {
        if (mv_require_member(sess)) {
            mv_line(sess, "* AI image generation is not available on this "
                          "station. Share one with /image in the Chatter "
                          "room instead.");
        }
    } else if (mv_is(cmd, "help", &r)) {
        mv_cmd_help(sess);
    } else if (mv_is(cmd, "giphy", &r)) {
        mv_cmd_giphy(sess, r);
    } else if (mv_is(cmd, "forgot", &r)) {
        mv_line(sess, "* Password resets are done by the sysop or a CoSysop "
                      "(/sc). Ask them in chat; mention your member number.");
    } else if (mv_is(cmd, "font", &r)) {
        r = mv_skip_ws(r);
        if (r[0] == '\0') {
            mv_line(sess, "* Font: %s. Terminals draw with your terminal's "
                          "font; GUI clients use /font <Google Font Name>.",
                    sess->mv.font[0] != '\0' ? sess->mv.font
                                             : "default (Poppins)");
        } else {
            snprintf(sess->mv.font, sizeof(sess->mv.font), "%s", r);
            mv_line(sess, "* Font preference saved: %s", sess->mv.font);
            mv_save_member(sess);
        }
    } else if (mv_is(cmd, "define", &r)) {
        if (mv_require_member(sess)) {
            mv_cmd_define(sess, r);
        }
    } else if (mv_is(cmd, "code", &r)) {
        if (mv_require_member(sess)) {
            r = mv_skip_ws(r);
            if (r[0] != '\0') {
                mv_chat_opts_t o = {0};
                o.block = true;
                mv_public_chat(sess, sess->channel, r, &o);
            } else {
                sess->mv.code_capture = true;
                sess->mv.code_len = 0U;
                sess->mv.code_buf[0] = '\0';
                mv_line(sess, "* Paste your code. End with /code (or a line "
                              "with a single '.').");
            }
        }
    } else if (mv_is(cmd, "cls", &r)) {
        ddial_session_write_raw(sess, "\033[2J\033[H", 7U);
    } else if (mv_is(cmd, "buzz", &r)) {
        if (mv_require_member(sess)) {
            mv_cmd_offline(sess, r, true);
        }
    } else if (mv_is(cmd, "bior", &r)) {
        if (mv_require_member(sess)) {
            mv_cmd_bior(sess);
        }
    } else if (mv_is(cmd, "bdayremove", &r)) {
        if (mv_require_member(sess)) {
            mv_cmd_bday(sess, "remove");
        }
    } else if (mv_is(cmd, "bday", &r)) {
        if (mv_require_member(sess)) {
            mv_cmd_bday(sess, r);
        }
    } else if (mv_is(cmd, "baud", &r)) {
        mv_cmd_baud(sess, r);
    } else if (mv_is(cmd, "anim", &r)) {
        if (mv_require_member(sess)) {
            const char *p = r;
            uint32_t n = 0U;
            if (!mv_parse_uint(&p, &n) || n < 1U || n > 5U) {
                mv_line(sess, "* Usage: /anim<1-5> <message>");
            } else if (mv_skip_ws(p)[0] == '\0') {
                static const char *const names[] = {
                    "typewriter", "rainbow", "wave", "blink", "sparkle"};
                mv_line(sess, "* /anim%u is %s. Try: /anim%u hello",
                        (unsigned)n, names[n - 1U], (unsigned)n);
            } else {
                mv_chat_opts_t o = {0};
                o.anim = (int)n;
                mv_public_chat(sess, sess->channel, p, &o);
            }
        }
    } else if (mv_is(cmd, "answer", &r)) {
        if (mv_require_member(sess)) {
            mv_cmd_answer(sess, r);
        }
    } else if (mv_is(cmd, "ask", &r)) {
        if (mv_require_member(sess)) {
            mv_cmd_ask(sess, r);
        }
    }
    /* --- two-letter words --- */
    else if (mv_is(cmd, "jk", &r)) {
        mv_cmd_joke(sess, r);
    } else if (mv_is(cmd, "ls", &r)) {
        mv_cmd_last_visitors(sess);
    } else if (mv_is(cmd, "sm", &r)) {
        mv_cmd_member_list(sess, r);
    } else if (mv_is(cmd, "sc", &r)) {
        mv_cmd_cosysops(sess, r);
    } else if (mv_is(cmd, "re", &r)) {
        mv_toggle_msg(sess, &sess->mv.replies_on, r, "Replies");
    } else if (mv_is(cmd, "ai", &r)) {
        mv_line(sess, "AI users: the Chatter room may host the 'eliza' "
                      "moderator persona. Talk to it on channel 1 by "
                      "mentioning @eliza.");
    } else if (mv_is(cmd, "us", &r)) {
        if (mv_require_member(sess)) {
            mv_cmd_affix(sess, sess->mv.status, sizeof(sess->mv.status), r,
                         "Status");
        }
    } else if (mv_is(cmd, "ts", &r)) {
        if (mv_require_member(sess)) {
            const char *p = r;
            uint32_t ch = 0U;
            if (!mv_parse_uint(&p, &ch)) {
                mv_line(sess, "* Usage: /ts<channel#> <message>");
            } else if (!mv_list_contains(&sess->mv.monitored, ch) &&
                       ch != sess->channel) {
                mv_line(sess, "* Monitor channel %u first (/t+%u).",
                        (unsigned)ch, (unsigned)ch);
            } else {
                sess->mv.last_monitor = (uint16_t)ch;
                mv_public_chat(sess, (uint16_t)ch, p, nullptr);
            }
        }
    } else if (mv_is(cmd, "tb", &r)) {
        if (mv_require_member(sess)) {
            mv_cmd_boot(sess, r);
        }
    } else if (mv_is(cmd, "ig", &r)) {
        if (mv_require_member(sess)) {
            mv_cmd_ignore(sess, r);
        }
    } else if (mv_is(cmd, "xx", &r)) {
        if (mv_require_member(sess)) {
            mv_cmd_full_mute(sess, r);
        }
    } else if (mv_is(cmd, "bv", &r)) {
        const char *p = r;
        uint32_t v = 0U;
        if (!mv_parse_uint(&p, &v) || v > 100U) {
            mv_line(sess, "* Usage: /bv<0-100>");
        } else {
            sess->mv.beep_volume = (uint8_t)v;
            mv_line(sess, "* Beep volume %u%%%s", (unsigned)v,
                    v == 0U ? " (silent)" : "");
            mv_save_member(sess);
        }
    } else if (mv_is(cmd, "bg", &r)) {
        mv_toggle_msg(sess, &sess->mv.badges_on, r, "Badges");
    } else if (mv_is(cmd, "pv", &r)) {
        if (mv_require_member(sess)) {
            mv_chat_opts_t o = {0};
            o.members_only = true;
            mv_public_chat(sess, sess->channel, r, &o);
        }
    } else if (mv_is(cmd, "ps", &r)) {
        mv_chat_opts_t o = {0};
        o.local_only = true;
        mv_public_chat(sess, sess->channel, r, &o);
    } else if (mv_is(cmd, "pw=", &r)) {
        if (mv_require_member(sess)) {
            mv_line(sess, ddial_member_set_password(sess->owner,
                                                    sess->mv.member_no,
                                                    mv_skip_ws(r))
                              ? "* Password changed."
                              : "* Password must be 4+ characters without "
                                "spaces or ':'.");
        }
    }
    /* --- single letters --- */
    else if (tolower((unsigned char)cmd[0]) == 'c' &&
             (cmd[1] == '\0' || cmd[1] == '#' || cmd[1] == ' ' ||
              (strlen(cmd + 1) >= 6U && mv_hexval(cmd[1]) >= 0 &&
               mv_hexval(cmd[2]) >= 0 && mv_hexval(cmd[3]) >= 0 &&
               mv_hexval(cmd[4]) >= 0 && mv_hexval(cmd[5]) >= 0 &&
               mv_hexval(cmd[6]) >= 0 &&
               (cmd[7] == '\0' || cmd[7] == ',')))) {
        if (mv_require_member(sess)) {
            mv_cmd_color(sess, cmd + 1);
        }
    } else if (tolower((unsigned char)cmd[0]) == 'm' &&
               (cmd[1] == '\0' || cmd[1] == '=' ||
                isdigit((unsigned char)cmd[1]) ||
                (isalpha((unsigned char)cmd[1]) &&
                 !isalpha((unsigned char)cmd[2])))) {
        if (cmd[1] == '=' && !mv_require_member(sess)) {
            return true;
        }
        mv_cmd_box(sess, cmd + 1);
    } else if (mv_is(cmd, "i", &r)) {
        mv_cmd_help(sess);
    } else if (mv_is(cmd, "h", &r)) {
        mv_cmd_handle(sess, r);
    } else if (mv_is(cmd, "s", &r)) {
        mv_cmd_who(sess, r);
    } else if (mv_is(cmd, "w", &r)) {
        mv_cmd_who(sess, r);
    } else if (mv_is(cmd, "p", &r)) {
        if (r[0] == '=') {
            if (!mv_require_member(sess)) {
                return true;
            }
            const char *p = r + 1;
            mv_parse_slot_csv(&p, &sess->mv.auto_pm);
            if (sess->mv.auto_pm.count == 0U) {
                mv_line(sess, "* Auto /p reset.");
            } else {
                mv_line(sess, "* /p <message> now goes to %zu line%s.",
                        sess->mv.auto_pm.count,
                        sess->mv.auto_pm.count == 1U ? "" : "s");
            }
        } else if (isdigit((unsigned char)r[0])) {
            const char *p = r;
            mv_parse_slot_csv(&p, &slots);
            if (slots.count > 1U && !mv_require_member(sess)) {
                return true;
            }
            mv_private(sess, &slots, p, false);
        } else if (sess->mv.auto_pm.count > 0U) {
            mv_private(sess, &sess->mv.auto_pm, r, false);
        } else {
            mv_line(sess, "* Usage: /p<line#> <message>");
        }
    } else if (mv_is(cmd, "r", &r)) {
        mv_cmd_profile(sess, r);
    } else if (mv_is(cmd, "b", &r)) {
        mv_cmd_beep(sess, r);
    } else if (mv_is(cmd, "q", &r)) {
        mv_cmd_quit(sess, r);
    } else if (mv_is(cmd, "t", &r) || mv_is(cmd, "j", &r)) {
        if (r[0] == '?') {
            mv_cmd_channel_help(sess);
        } else if (r[0] == '+' || r[0] == '-') {
            if (!mv_require_member(sess)) {
                return true;
            }
            const char *p = r + 1;
            uint32_t ch = 0U;
            if (!mv_parse_uint(&p, &ch) || ch < 1U ||
                ch > DDIAL_MV_MAX_CHANNEL) {
                mv_line(sess, "* Usage: /t+<channel#> or /t-<channel#>");
            } else if (r[0] == '+') {
                if (mv_is_booted((uint16_t)ch, sess->handle)) {
                    mv_line(sess, "* You were booted from channel %u.",
                            (unsigned)ch);
                } else if (mv_list_add(&sess->mv.monitored, ch)) {
                    sess->mv.last_monitor = (uint16_t)ch;
                    mv_line(sess, "* Monitoring channel %u. Send with "
                                  "/ts%u <msg> or ;<msg>.",
                            (unsigned)ch, (unsigned)ch);
                } else {
                    mv_line(sess, "* You can monitor up to %u channels.",
                            DDIAL_MV_SLOT_LIST);
                }
            } else {
                mv_list_remove(&sess->mv.monitored, ch);
                if (sess->mv.last_monitor == ch) {
                    sess->mv.last_monitor =
                        sess->mv.monitored.count > 0U
                            ? (uint16_t)sess->mv.monitored.items[0]
                            : 0U;
                }
                mv_line(sess, "* Stopped monitoring channel %u.",
                        (unsigned)ch);
            }
        } else {
            const char *p = r;
            uint32_t ch = 0U;
            if (mv_parse_uint(&p, &ch)) {
                mv_tune(sess, ch);
            } else {
                char when[32];
                mv_format_time(time(nullptr), when, sizeof(when));
                mv_line(sess, "* %s - you are on channel %u. /t# to tune, /t? "
                              "for help.",
                        when, (unsigned)sess->channel);
            }
        }
    } else if (mv_is(cmd, "a", &r)) {
        if (!mv_require_member(sess)) {
            return true;
        }
        if (isdigit((unsigned char)r[0])) {
            const char *p = r;
            mv_parse_slot_csv(&p, &slots);
            mv_private(sess, &slots, p, true);
        } else if (mv_skip_ws(r)[0] != '\0') {
            mv_chat_opts_t o = {0};
            o.action = true;
            mv_public_chat(sess, sess->channel, r, &o);
        } else {
            mv_line(sess, "* Usage: /a <action>  or  /a<line#> <action>");
        }
    } else if (mv_is(cmd, "o", &r)) {
        if (mv_require_member(sess)) {
            mv_cmd_offline(sess, r, false);
        }
    } else if (mv_is(cmd, "e", &r)) {
        mv_cmd_email(sess, r);
    } else if (mv_is(cmd, "n", &r)) {
        mv_toggle_msg(sess, &sess->mv.notify_on, r,
                      "Browser notifications (GUI clients; terminals use "
                      "/b beeps)");
    } else if (mv_is(cmd, "d", &r)) {
        mv_cmd_link_settings(sess);
    } else if (mv_is(cmd, "u", &r)) {
        if (mv_require_member(sess)) {
            if (mv_skip_ws(r)[0] == '\0') {
                mv_line(sess, "* Usage: /u <message>");
            } else {
                mv_chat_opts_t o = {0};
                o.uno = true;
                mv_public_chat(sess, sess->channel, mv_skip_ws(r), &o);
            }
        }
    } else if (mv_is(cmd, "x", &r)) {
        if (mv_require_member(sess)) {
            mv_cmd_slot_toggle(sess, r, &sess->mv.squelch_slots,
                               "Squelched private messages from",
                               "Accepting private messages from",
                               "* Usage: /x<line#>");
        }
    } else if (mv_is(cmd, "k", &r)) {
        mv_cmd_kick(sess, r);
    } else if (mv_is(cmd, "v", &r)) {
        mv_line(sess, "SSH-Chatter DDial station, MagViz command set.");
    } else {
        return false; /* legacy DDial parser gets a look (/C<text> ...) */
    }
    return true;
}

/**
 * @file members.c
 * @desc Member accounts, mailboxes and offline messages for the DDial
 *       listener's MagViz-compatible command layer.
 *
 * Members sign up with /signup <password>, receive a member number, and log
 * in at the handle prompt with "###password" or "###:password[:command]".
 * Records live in two key=value-per-field text files under the user data
 * root (ddial_members.tsv, ddial_mail.tsv) and are rewritten on change.
 */

#include "ssh_chatter/ddial_protocol.h"
#include "ssh_chatter/host.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define DDIAL_MEMBER_MAX 1024U
#define DDIAL_MAIL_MAX 1024U
#define DDIAL_MEMBER_PBKDF2_ROUNDS 60000
#define DDIAL_MEMBER_MIN_PASSWORD 4U

typedef struct ddial_member {
    uint32_t number;
    char handle[DDIAL_MAX_HANDLE_LEN];
    uint8_t salt[16];
    uint8_t hash[32];
    bool cosysop;
    char handle_color[160]; /* /h#RRGGBB spec, empty = plain handle */
    char msg_color[16];
    char prefix[48];
    char postfix[48];
    char status[96];
    char profile[512];
    char bday[11]; /* YYYY-MM-DD */
    bool bday_countdown;
    char box[512];
    bool box_rotate;
    char saved_handles[DDIAL_MV_SAVED_HANDLES][DDIAL_MAX_HANDLE_LEN];
    char font[48];
    bool beep_on;
    uint8_t beep_volume;
    uint8_t beep_events;
    uint16_t baud;
    bool badges_on;
    bool replies_on;
    bool auto_slash;
    bool voice_on;
    bool notify_on;
    uint32_t logins;
    uint32_t messages;
    int64_t created;
    int64_t last_seen;
} ddial_member_t;

typedef struct ddial_mail {
    uint32_t id;
    uint32_t to;
    uint32_t from;
    char from_handle[DDIAL_MAX_HANDLE_LEN];
    int64_t sent;
    bool offline; /* /o and /buzz: shown once at next login, then removed */
    char text[DDIAL_MV_TEXT_LIMIT];
} ddial_mail_t;

static pthread_mutex_t g_ddial_member_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_ddial_members_loaded = false;
static char g_ddial_members_path[PATH_MAX];
static char g_ddial_mail_path[PATH_MAX];
static ddial_member_t g_ddial_members[DDIAL_MEMBER_MAX];
static size_t g_ddial_member_count = 0U;
static ddial_mail_t g_ddial_mail[DDIAL_MAIL_MAX];
static size_t g_ddial_mail_count = 0U;
static uint32_t g_ddial_mail_next_id = 1U;

/* ---- field escaping ---------------------------------------------------- */

static void ddial_member_escape(FILE *fp, const char *key, const char *value)
{
    fputs(key, fp);
    fputc('=', fp);
    for (const char *p = value; p != nullptr && *p != '\0'; ++p) {
        switch (*p) {
        case '\\':
            fputs("\\\\", fp);
            break;
        case '\t':
            fputs("\\t", fp);
            break;
        case '\n':
            fputs("\\n", fp);
            break;
        case '\r':
            fputs("\\r", fp);
            break;
        default:
            fputc(*p, fp);
            break;
        }
    }
    fputc('\t', fp);
}

static void ddial_member_escape_u64(FILE *fp, const char *key, uint64_t value)
{
    fprintf(fp, "%s=%llu\t", key, (unsigned long long)value);
}

static void ddial_member_unescape(const char *src, char *dst, size_t cap)
{
    size_t out = 0U;
    for (const char *p = src; *p != '\0' && out + 1U < cap; ++p) {
        if (*p == '\\' && p[1] != '\0') {
            ++p;
            switch (*p) {
            case 't':
                dst[out++] = '\t';
                break;
            case 'n':
                dst[out++] = '\n';
                break;
            case 'r':
                dst[out++] = '\r';
                break;
            default:
                dst[out++] = *p;
                break;
            }
            continue;
        }
        dst[out++] = *p;
    }
    dst[out] = '\0';
}

static void ddial_member_hex(const uint8_t *src, size_t len, char *dst)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0U; i < len; ++i) {
        dst[i * 2U] = digits[src[i] >> 4];
        dst[i * 2U + 1U] = digits[src[i] & 0x0FU];
    }
    dst[len * 2U] = '\0';
}

static void ddial_member_unhex(const char *src, uint8_t *dst, size_t len)
{
    memset(dst, 0, len);
    for (size_t i = 0U; i < len && src[i * 2U] != '\0' && src[i * 2U + 1U] != '\0';
         ++i) {
        unsigned int value = 0U;
        if (sscanf(src + i * 2U, "%2x", &value) == 1) {
            dst[i] = (uint8_t)value;
        }
    }
}

/* Iterate "key=value\t" pairs of one record line in place. */
typedef void (*ddial_member_field_cb)(const char *key, const char *value,
                                      void *user);

static void ddial_member_parse_line(char *line, ddial_member_field_cb cb,
                                    void *user)
{
    char *save = nullptr;
    for (char *tok = strtok_r(line, "\t", &save); tok != nullptr;
         tok = strtok_r(nullptr, "\t", &save)) {
        char *eq = strchr(tok, '=');
        if (eq == nullptr) {
            continue;
        }
        *eq = '\0';
        cb(tok, eq + 1, user);
    }
}

/* ---- persistence -------------------------------------------------------- */

static void ddial_member_field(const char *key, const char *value, void *user)
{
    ddial_member_t *m = (ddial_member_t *)user;
    char buf[DDIAL_MV_TEXT_LIMIT * 2U];
    ddial_member_unescape(value, buf, sizeof(buf));

    if (strcmp(key, "no") == 0) {
        m->number = (uint32_t)strtoul(buf, nullptr, 10);
    } else if (strcmp(key, "handle") == 0) {
        snprintf(m->handle, sizeof(m->handle), "%s", buf);
    } else if (strcmp(key, "salt") == 0) {
        ddial_member_unhex(buf, m->salt, sizeof(m->salt));
    } else if (strcmp(key, "hash") == 0) {
        ddial_member_unhex(buf, m->hash, sizeof(m->hash));
    } else if (strcmp(key, "cosysop") == 0) {
        m->cosysop = atoi(buf) != 0;
    } else if (strcmp(key, "hcolor") == 0) {
        snprintf(m->handle_color, sizeof(m->handle_color), "%s", buf);
    } else if (strcmp(key, "mcolor") == 0) {
        snprintf(m->msg_color, sizeof(m->msg_color), "%s", buf);
    } else if (strcmp(key, "pre") == 0) {
        snprintf(m->prefix, sizeof(m->prefix), "%s", buf);
    } else if (strcmp(key, "post") == 0) {
        snprintf(m->postfix, sizeof(m->postfix), "%s", buf);
    } else if (strcmp(key, "status") == 0) {
        snprintf(m->status, sizeof(m->status), "%s", buf);
    } else if (strcmp(key, "profile") == 0) {
        snprintf(m->profile, sizeof(m->profile), "%s", buf);
    } else if (strcmp(key, "bday") == 0) {
        snprintf(m->bday, sizeof(m->bday), "%s", buf);
    } else if (strcmp(key, "bdaycd") == 0) {
        m->bday_countdown = atoi(buf) != 0;
    } else if (strcmp(key, "box") == 0) {
        snprintf(m->box, sizeof(m->box), "%s", buf);
    } else if (strcmp(key, "boxrot") == 0) {
        m->box_rotate = atoi(buf) != 0;
    } else if (strncmp(key, "h", 1U) == 0 && key[1] >= '1' && key[1] <= '9' &&
               key[2] == '\0') {
        snprintf(m->saved_handles[key[1] - '1'],
                 sizeof(m->saved_handles[0]), "%s", buf);
    } else if (strcmp(key, "font") == 0) {
        snprintf(m->font, sizeof(m->font), "%s", buf);
    } else if (strcmp(key, "beep") == 0) {
        m->beep_on = atoi(buf) != 0;
    } else if (strcmp(key, "beepvol") == 0) {
        m->beep_volume = (uint8_t)atoi(buf);
    } else if (strcmp(key, "beepev") == 0) {
        m->beep_events = (uint8_t)atoi(buf);
    } else if (strcmp(key, "baud") == 0) {
        m->baud = (uint16_t)atoi(buf);
    } else if (strcmp(key, "badges") == 0) {
        m->badges_on = atoi(buf) != 0;
    } else if (strcmp(key, "replies") == 0) {
        m->replies_on = atoi(buf) != 0;
    } else if (strcmp(key, "autoslash") == 0) {
        m->auto_slash = atoi(buf) != 0;
    } else if (strcmp(key, "voice") == 0) {
        m->voice_on = atoi(buf) != 0;
    } else if (strcmp(key, "notify") == 0) {
        m->notify_on = atoi(buf) != 0;
    } else if (strcmp(key, "logins") == 0) {
        m->logins = (uint32_t)strtoul(buf, nullptr, 10);
    } else if (strcmp(key, "messages") == 0) {
        m->messages = (uint32_t)strtoul(buf, nullptr, 10);
    } else if (strcmp(key, "created") == 0) {
        m->created = strtoll(buf, nullptr, 10);
    } else if (strcmp(key, "seen") == 0) {
        m->last_seen = strtoll(buf, nullptr, 10);
    }
}

static void ddial_mail_field(const char *key, const char *value, void *user)
{
    ddial_mail_t *m = (ddial_mail_t *)user;
    char buf[DDIAL_MV_TEXT_LIMIT * 2U];
    ddial_member_unescape(value, buf, sizeof(buf));

    if (strcmp(key, "id") == 0) {
        m->id = (uint32_t)strtoul(buf, nullptr, 10);
    } else if (strcmp(key, "to") == 0) {
        m->to = (uint32_t)strtoul(buf, nullptr, 10);
    } else if (strcmp(key, "from") == 0) {
        m->from = (uint32_t)strtoul(buf, nullptr, 10);
    } else if (strcmp(key, "fromh") == 0) {
        snprintf(m->from_handle, sizeof(m->from_handle), "%s", buf);
    } else if (strcmp(key, "sent") == 0) {
        m->sent = strtoll(buf, nullptr, 10);
    } else if (strcmp(key, "offline") == 0) {
        m->offline = atoi(buf) != 0;
    } else if (strcmp(key, "text") == 0) {
        snprintf(m->text, sizeof(m->text), "%s", buf);
    }
}

static void ddial_members_save_locked(void)
{
    if (g_ddial_members_path[0] == '\0') {
        return;
    }
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_ddial_members_path);
    FILE *fp = fopen(tmp, "w");
    if (fp == nullptr) {
        return;
    }
    for (size_t i = 0U; i < g_ddial_member_count; ++i) {
        const ddial_member_t *m = &g_ddial_members[i];
        char salt_hex[33];
        char hash_hex[65];
        ddial_member_hex(m->salt, sizeof(m->salt), salt_hex);
        ddial_member_hex(m->hash, sizeof(m->hash), hash_hex);
        ddial_member_escape_u64(fp, "no", m->number);
        ddial_member_escape(fp, "handle", m->handle);
        ddial_member_escape(fp, "salt", salt_hex);
        ddial_member_escape(fp, "hash", hash_hex);
        ddial_member_escape_u64(fp, "cosysop", m->cosysop ? 1U : 0U);
        ddial_member_escape(fp, "hcolor", m->handle_color);
        ddial_member_escape(fp, "mcolor", m->msg_color);
        ddial_member_escape(fp, "pre", m->prefix);
        ddial_member_escape(fp, "post", m->postfix);
        ddial_member_escape(fp, "status", m->status);
        ddial_member_escape(fp, "profile", m->profile);
        ddial_member_escape(fp, "bday", m->bday);
        ddial_member_escape_u64(fp, "bdaycd", m->bday_countdown ? 1U : 0U);
        ddial_member_escape(fp, "box", m->box);
        ddial_member_escape_u64(fp, "boxrot", m->box_rotate ? 1U : 0U);
        for (size_t h = 0U; h < DDIAL_MV_SAVED_HANDLES; ++h) {
            if (m->saved_handles[h][0] != '\0') {
                char key[4] = {'h', (char)('1' + h), '\0', '\0'};
                ddial_member_escape(fp, key, m->saved_handles[h]);
            }
        }
        ddial_member_escape(fp, "font", m->font);
        ddial_member_escape_u64(fp, "beep", m->beep_on ? 1U : 0U);
        ddial_member_escape_u64(fp, "beepvol", m->beep_volume);
        ddial_member_escape_u64(fp, "beepev", m->beep_events);
        ddial_member_escape_u64(fp, "baud", m->baud);
        ddial_member_escape_u64(fp, "badges", m->badges_on ? 1U : 0U);
        ddial_member_escape_u64(fp, "replies", m->replies_on ? 1U : 0U);
        ddial_member_escape_u64(fp, "autoslash", m->auto_slash ? 1U : 0U);
        ddial_member_escape_u64(fp, "voice", m->voice_on ? 1U : 0U);
        ddial_member_escape_u64(fp, "notify", m->notify_on ? 1U : 0U);
        ddial_member_escape_u64(fp, "logins", m->logins);
        ddial_member_escape_u64(fp, "messages", m->messages);
        ddial_member_escape_u64(fp, "created", (uint64_t)m->created);
        ddial_member_escape_u64(fp, "seen", (uint64_t)m->last_seen);
        fputc('\n', fp);
    }
    if (fclose(fp) == 0) {
        rename(tmp, g_ddial_members_path);
    }
}

static void ddial_mail_save_locked(void)
{
    if (g_ddial_mail_path[0] == '\0') {
        return;
    }
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_ddial_mail_path);
    FILE *fp = fopen(tmp, "w");
    if (fp == nullptr) {
        return;
    }
    for (size_t i = 0U; i < g_ddial_mail_count; ++i) {
        const ddial_mail_t *m = &g_ddial_mail[i];
        ddial_member_escape_u64(fp, "id", m->id);
        ddial_member_escape_u64(fp, "to", m->to);
        ddial_member_escape_u64(fp, "from", m->from);
        ddial_member_escape(fp, "fromh", m->from_handle);
        ddial_member_escape_u64(fp, "sent", (uint64_t)m->sent);
        ddial_member_escape_u64(fp, "offline", m->offline ? 1U : 0U);
        ddial_member_escape(fp, "text", m->text);
        fputc('\n', fp);
    }
    if (fclose(fp) == 0) {
        rename(tmp, g_ddial_mail_path);
    }
}

static void ddial_members_ensure_loaded_locked(host_t *host)
{
    if (g_ddial_members_loaded || host == nullptr) {
        return;
    }
    g_ddial_members_loaded = true;

    const char *root = host->user_data_root[0] != '\0' ? host->user_data_root
                                                       : ".";
    const char *override = getenv("CHATTER_DDIAL_MEMBERS_DIR");
    if (override != nullptr && override[0] != '\0') {
        root = override;
    }
    snprintf(g_ddial_members_path, sizeof(g_ddial_members_path),
             "%s/ddial_members.tsv", root);
    snprintf(g_ddial_mail_path, sizeof(g_ddial_mail_path), "%s/ddial_mail.tsv",
             root);

    char line[8192];
    FILE *fp = fopen(g_ddial_members_path, "r");
    if (fp != nullptr) {
        while (fgets(line, sizeof(line), fp) != nullptr &&
               g_ddial_member_count < DDIAL_MEMBER_MAX) {
            line[strcspn(line, "\r\n")] = '\0';
            ddial_member_t m;
            memset(&m, 0, sizeof(m));
            ddial_member_parse_line(line, ddial_member_field, &m);
            if (m.number != 0U && m.handle[0] != '\0') {
                g_ddial_members[g_ddial_member_count++] = m;
            }
        }
        fclose(fp);
    }

    fp = fopen(g_ddial_mail_path, "r");
    if (fp != nullptr) {
        while (fgets(line, sizeof(line), fp) != nullptr &&
               g_ddial_mail_count < DDIAL_MAIL_MAX) {
            line[strcspn(line, "\r\n")] = '\0';
            ddial_mail_t m;
            memset(&m, 0, sizeof(m));
            ddial_member_parse_line(line, ddial_mail_field, &m);
            if (m.id != 0U && m.to != 0U) {
                g_ddial_mail[g_ddial_mail_count++] = m;
                if (m.id >= g_ddial_mail_next_id) {
                    g_ddial_mail_next_id = m.id + 1U;
                }
            }
        }
        fclose(fp);
    }
}

static ddial_member_t *ddial_member_find_no_locked(uint32_t number)
{
    for (size_t i = 0U; i < g_ddial_member_count; ++i) {
        if (g_ddial_members[i].number == number) {
            return &g_ddial_members[i];
        }
    }
    return nullptr;
}

static ddial_member_t *ddial_member_find_handle_locked(const char *handle)
{
    for (size_t i = 0U; i < g_ddial_member_count; ++i) {
        if (strcasecmp(g_ddial_members[i].handle, handle) == 0) {
            return &g_ddial_members[i];
        }
    }
    return nullptr;
}

static bool ddial_member_derive(const char *password, const uint8_t *salt,
                                uint8_t *out)
{
    return PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt, 16,
                             DDIAL_MEMBER_PBKDF2_ROUNDS, EVP_sha256(), 32,
                             out) == 1;
}

/* ---- public API (translation-unit static) ------------------------------ */

static bool ddial_member_get(host_t *host, uint32_t number, ddial_member_t *out)
{
    pthread_mutex_lock(&g_ddial_member_lock);
    ddial_members_ensure_loaded_locked(host);
    ddial_member_t *m = ddial_member_find_no_locked(number);
    if (m != nullptr && out != nullptr) {
        *out = *m;
    }
    pthread_mutex_unlock(&g_ddial_member_lock);
    return m != nullptr;
}

/* Returns the member number owning handle, or 0. */
static uint32_t ddial_member_owner_of(host_t *host, const char *handle)
{
    pthread_mutex_lock(&g_ddial_member_lock);
    ddial_members_ensure_loaded_locked(host);
    ddial_member_t *m = ddial_member_find_handle_locked(handle);
    uint32_t number = m != nullptr ? m->number : 0U;
    pthread_mutex_unlock(&g_ddial_member_lock);
    return number;
}

static bool ddial_member_put(host_t *host, const ddial_member_t *member)
{
    pthread_mutex_lock(&g_ddial_member_lock);
    ddial_members_ensure_loaded_locked(host);
    ddial_member_t *m = ddial_member_find_no_locked(member->number);
    if (m != nullptr) {
        *m = *member;
        ddial_members_save_locked();
    }
    pthread_mutex_unlock(&g_ddial_member_lock);
    return m != nullptr;
}

/* Register handle as a new member.  Returns the new number, or 0 with a
 * reason in err. */
static uint32_t ddial_member_signup(host_t *host, const char *handle,
                                    const char *password, char *err,
                                    size_t err_cap)
{
    if (password == nullptr || strlen(password) < DDIAL_MEMBER_MIN_PASSWORD) {
        snprintf(err, err_cap, "Password must be at least %u characters.",
                 DDIAL_MEMBER_MIN_PASSWORD);
        return 0U;
    }
    if (strchr(password, ':') != nullptr || strchr(password, ' ') != nullptr) {
        snprintf(err, err_cap, "Password may not contain spaces or ':'.");
        return 0U;
    }

    uint32_t number = 0U;
    pthread_mutex_lock(&g_ddial_member_lock);
    ddial_members_ensure_loaded_locked(host);
    if (ddial_member_find_handle_locked(handle) != nullptr) {
        snprintf(err, err_cap, "Handle '%s' is already a member.", handle);
    } else if (g_ddial_member_count >= DDIAL_MEMBER_MAX) {
        snprintf(err, err_cap, "Member roll is full.");
    } else {
        uint32_t next = 1U;
        for (size_t i = 0U; i < g_ddial_member_count; ++i) {
            if (g_ddial_members[i].number >= next) {
                next = g_ddial_members[i].number + 1U;
            }
        }
        ddial_member_t *m = &g_ddial_members[g_ddial_member_count];
        memset(m, 0, sizeof(*m));
        if (RAND_bytes(m->salt, sizeof(m->salt)) == 1 &&
            ddial_member_derive(password, m->salt, m->hash)) {
            m->number = next;
            snprintf(m->handle, sizeof(m->handle), "%s", handle);
            m->beep_on = true;
            m->beep_volume = 100U;
            m->beep_events = DDIAL_MV_BEEP_PM | DDIAL_MV_BEEP_MENTION |
                             DDIAL_MV_BEEP_BUZZ;
            m->badges_on = true;
            m->replies_on = true;
            m->auto_slash = true;
            m->created = (int64_t)time(nullptr);
            m->last_seen = m->created;
            ++g_ddial_member_count;
            ddial_members_save_locked();
            number = next;
        } else {
            snprintf(err, err_cap, "Could not hash password.");
        }
    }
    pthread_mutex_unlock(&g_ddial_member_lock);
    return number;
}

static bool ddial_member_authenticate(host_t *host, uint32_t number,
                                      const char *password,
                                      ddial_member_t *out)
{
    bool ok = false;
    pthread_mutex_lock(&g_ddial_member_lock);
    ddial_members_ensure_loaded_locked(host);
    ddial_member_t *m = ddial_member_find_no_locked(number);
    if (m != nullptr && password != nullptr) {
        uint8_t derived[32];
        if (ddial_member_derive(password, m->salt, derived) &&
            CRYPTO_memcmp(derived, m->hash, sizeof(derived)) == 0) {
            ok = true;
            if (out != nullptr) {
                *out = *m;
            }
        }
    }
    pthread_mutex_unlock(&g_ddial_member_lock);
    return ok;
}

static bool ddial_member_set_password(host_t *host, uint32_t number,
                                      const char *password)
{
    if (password == nullptr || strlen(password) < DDIAL_MEMBER_MIN_PASSWORD ||
        strchr(password, ':') != nullptr || strchr(password, ' ') != nullptr) {
        return false;
    }
    bool ok = false;
    pthread_mutex_lock(&g_ddial_member_lock);
    ddial_members_ensure_loaded_locked(host);
    ddial_member_t *m = ddial_member_find_no_locked(number);
    if (m != nullptr && RAND_bytes(m->salt, sizeof(m->salt)) == 1 &&
        ddial_member_derive(password, m->salt, m->hash)) {
        ddial_members_save_locked();
        ok = true;
    }
    pthread_mutex_unlock(&g_ddial_member_lock);
    return ok;
}

typedef void (*ddial_member_visit_cb)(const ddial_member_t *member,
                                      void *user);

static void ddial_member_foreach(host_t *host, ddial_member_visit_cb cb,
                                 void *user)
{
    pthread_mutex_lock(&g_ddial_member_lock);
    ddial_members_ensure_loaded_locked(host);
    for (size_t i = 0U; i < g_ddial_member_count; ++i) {
        cb(&g_ddial_members[i], user);
    }
    pthread_mutex_unlock(&g_ddial_member_lock);
}

/* ---- mail --------------------------------------------------------------- */

static uint32_t ddial_mail_send(host_t *host, uint32_t to, uint32_t from,
                                const char *from_handle, const char *text,
                                bool offline)
{
    uint32_t id = 0U;
    pthread_mutex_lock(&g_ddial_member_lock);
    ddial_members_ensure_loaded_locked(host);
    if (ddial_member_find_no_locked(to) != nullptr &&
        g_ddial_mail_count < DDIAL_MAIL_MAX) {
        ddial_mail_t *m = &g_ddial_mail[g_ddial_mail_count++];
        memset(m, 0, sizeof(*m));
        m->id = g_ddial_mail_next_id++;
        m->to = to;
        m->from = from;
        snprintf(m->from_handle, sizeof(m->from_handle), "%s",
                 from_handle != nullptr ? from_handle : "?");
        m->sent = (int64_t)time(nullptr);
        m->offline = offline;
        snprintf(m->text, sizeof(m->text), "%s", text);
        ddial_mail_save_locked();
        id = m->id;
    }
    pthread_mutex_unlock(&g_ddial_member_lock);
    return id;
}

typedef void (*ddial_mail_visit_cb)(const ddial_mail_t *mail, void *user);

/* Visit mail for a member.  offline selects /o-style notes vs mailbox. When
 * consume is true the visited entries are removed afterwards. */
static size_t ddial_mail_visit(host_t *host, uint32_t to, bool offline,
                               bool consume, ddial_mail_visit_cb cb,
                               void *user)
{
    size_t visited = 0U;
    pthread_mutex_lock(&g_ddial_member_lock);
    ddial_members_ensure_loaded_locked(host);
    size_t keep = 0U;
    for (size_t i = 0U; i < g_ddial_mail_count; ++i) {
        ddial_mail_t *m = &g_ddial_mail[i];
        bool match = m->to == to && m->offline == offline;
        if (match) {
            if (cb != nullptr) {
                cb(m, user);
            }
            ++visited;
            if (consume) {
                continue;
            }
        }
        if (keep != i) {
            g_ddial_mail[keep] = *m;
        }
        ++keep;
    }
    if (consume && keep != g_ddial_mail_count) {
        g_ddial_mail_count = keep;
        ddial_mail_save_locked();
    }
    pthread_mutex_unlock(&g_ddial_member_lock);
    return visited;
}

/* Delete one mailbox entry (id != 0) or all of them (id == 0). */
static size_t ddial_mail_delete(host_t *host, uint32_t to, uint32_t id)
{
    size_t removed = 0U;
    pthread_mutex_lock(&g_ddial_member_lock);
    ddial_members_ensure_loaded_locked(host);
    size_t keep = 0U;
    for (size_t i = 0U; i < g_ddial_mail_count; ++i) {
        ddial_mail_t *m = &g_ddial_mail[i];
        if (m->to == to && !m->offline && (id == 0U || m->id == id)) {
            ++removed;
            continue;
        }
        if (keep != i) {
            g_ddial_mail[keep] = *m;
        }
        ++keep;
    }
    if (removed > 0U) {
        g_ddial_mail_count = keep;
        ddial_mail_save_locked();
    }
    pthread_mutex_unlock(&g_ddial_member_lock);
    return removed;
}

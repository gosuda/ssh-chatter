/**
 * @file mailbridge.c
 * @desc Mail between Chatter mailboxes (/mail) and DDial member mailboxes
 *       (/e), plus e-mail arriving over the Station Link.
 *
 *   Chatter -> DDial   /mail send <name>|<text> reaches the DDial member
 *                      called <name> when no Chatter user has that name
 *                      (or always, with <name>@ddial).
 *   DDial -> Chatter   /e=<name>|<text> from a DDial member reaches the
 *                      Chatter user called <name>.  Only existing Chatter
 *                      users qualify: the DDial side never creates Chatter
 *                      user records.
 *   Link e-mail        "/E~SSSNNN(aaa:handle) text" is delivered to local
 *                      member NNN's mailbox, never to the room.
 *
 * Text is cleaned before it is stored: terminal escapes and control
 * characters are dropped, line breaks folded to spaces, and it is clipped
 * to the destination mailbox without splitting a UTF-8 sequence.  A
 * bridged mail carries no member number to reply to (from = 0); the sender
 * is named in from_handle as "<name> (Chatter)" or "<name> (station SSS)".
 */

#include "ssh_chatter/ddial_protocol.h"
#include "ssh_chatter/host.h"

#include <string.h>

static bool ddial_mail_clean_text(const char *in, char *out, size_t cap)
{
    if (in == nullptr || out == nullptr || cap == 0U) {
        return false;
    }
    size_t n = 0U;
    for (const unsigned char *p = (const unsigned char *)in;
         *p != '\0' && n + 1U < cap; ++p) {
        if (*p == 0x1B) {
            /* ESC [ ... final byte, or a lone ESC + one byte. */
            if (p[1] == '[') {
                p += 2;
                while (*p != '\0' && (*p < 0x40U || *p > 0x7EU)) {
                    ++p;
                }
            } else if (p[1] != '\0') {
                ++p;
            }
            if (*p == '\0') {
                break;
            }
            continue;
        }
        if (*p == '\r' || *p == '\n' || *p == '\t') {
            if (n > 0U && out[n - 1U] != ' ') {
                out[n++] = ' ';
            }
            continue;
        }
        if (*p < 0x20U || *p == 0x7FU) {
            continue;
        }
        out[n++] = (char)*p;
    }
    /* Do not end on a partial UTF-8 sequence. */
    size_t cut = n;
    while (cut > 0U && ((unsigned char)out[cut - 1U] & 0xC0U) == 0x80U) {
        --cut;
    }
    if (cut > 0U && ((unsigned char)out[cut - 1U] & 0x80U) != 0U) {
        unsigned char lead = (unsigned char)out[cut - 1U];
        size_t need = (lead & 0xE0U) == 0xC0U   ? 2U
                      : (lead & 0xF0U) == 0xE0U ? 3U
                      : (lead & 0xF8U) == 0xF0U ? 4U
                                                : 1U;
        if (n - (cut - 1U) < need) {
            n = cut - 1U;
        }
    }
    while (n > 0U && out[n - 1U] == ' ') {
        --n;
    }
    out[n] = '\0';
    size_t lead_ws = 0U;
    while (out[lead_ws] == ' ') {
        ++lead_ws;
    }
    if (lead_ws > 0U) {
        memmove(out, out + lead_ws, n - lead_ws + 1U);
    }
    return out[0] != '\0';
}

typedef struct ddial_mail_notify_ctx {
    uint32_t member;
} ddial_mail_notify_ctx_t;

static void ddial_mail_notify_cb(ddial_session_t *target, void *user)
{
    ddial_mail_notify_ctx_t *c = (ddial_mail_notify_ctx_t *)user;
    if (target->logged_in && target->mv.member_no == c->member) {
        ddial_session_write_line(target, "* You have new email. Type /e.");
        mv_bell(target, DDIAL_MV_BEEP_PM);
    }
}

static void ddial_mail_notify_member(host_t *host, uint32_t member)
{
    ddial_mail_notify_ctx_t c = {member};
    host_ddial_foreach_session(host, ddial_mail_notify_cb, &c);
}

bool host_ddial_member_exists(host_t *host, const char *handle)
{
    return host != nullptr && handle != nullptr && handle[0] != '\0' &&
           ddial_member_owner_of(host, handle) != 0U;
}

bool host_ddial_mail_chatter_to_member(host_t *host, const char *from_name,
                                       const char *handle, const char *text,
                                       char *error, size_t error_cap)
{
    if (error == nullptr || error_cap == 0U) {
        return false;
    }
    error[0] = '\0';
    if (host == nullptr || from_name == nullptr || handle == nullptr ||
        text == nullptr) {
        return false;
    }
    uint32_t to = ddial_member_owner_of(host, handle);
    if (to == 0U) {
        snprintf(error, error_cap, "%s", "No DDial member has that name.");
        return false;
    }
    char clean[DDIAL_MV_TEXT_LIMIT];
    if (!ddial_mail_clean_text(text, clean, sizeof(clean))) {
        snprintf(error, error_cap, "%s", "Mailbox message cannot be empty.");
        return false;
    }
    char sender[DDIAL_MAX_HANDLE_LEN];
    snprintf(sender, sizeof(sender), "%.21s (Chatter)", from_name);
    if (ddial_mail_send(host, to, 0U, sender, clean, false) == 0U) {
        snprintf(error, error_cap, "%s", "That DDial mailbox is full.");
        return false;
    }
    ddial_mail_notify_member(host, to);
    return true;
}

bool host_ddial_mail_member_to_chatter(host_t *host, const char *from_handle,
                                       const char *name, const char *text,
                                       char *error, size_t error_cap)
{
    if (error == nullptr || error_cap == 0U) {
        return false;
    }
    error[0] = '\0';
    if (host == nullptr || from_handle == nullptr || name == nullptr ||
        text == nullptr) {
        return false;
    }

    char ip[SSH_CHATTER_IP_LEN] = "";
    session_ctx_t *online = chat_room_find_user_ref(&host->room, name);
    if (online != nullptr) {
        snprintf(ip, sizeof(ip), "%s", online->client_ip);
    } else if (!host_lookup_last_ip(host, name, ip, sizeof(ip))) {
        snprintf(error, error_cap, "%s",
                 "No member or Chatter user by that name.");
        return false;
    }

    char clean[USER_DATA_MAILBOX_MESSAGE_LEN];
    if (!ddial_mail_clean_text(text, clean, sizeof(clean))) {
        if (online != nullptr) {
            chat_room_release_user_ref(online);
        }
        snprintf(error, error_cap, "%s", "Message cannot be empty.");
        return false;
    }
    char sender[SSH_CHATTER_USERNAME_LEN];
    snprintf(sender, sizeof(sender), "%s (DDial)", from_handle);

    char send_error[128];
    bool ok = host_user_data_send_mail(host, name, ip, sender, clean,
                                       send_error, sizeof(send_error));
    if (!ok) {
        snprintf(error, error_cap, "%s",
                 send_error[0] != '\0' ? send_error
                                       : "Mailbox is unavailable.");
    } else if (online != nullptr) {
        char notice[SSH_CHATTER_MESSAGE_LIMIT];
        session_command_snprintf(online, notice, sizeof(notice),
                                 "You have new mail from %s. Use /mail to "
                                 "read it.",
                                 sender);
        session_send_system_line(online, notice);
        if (online->history_scroll_position == 0U) {
            session_refresh_input_line(online);
        }
    }
    if (online != nullptr) {
        chat_room_release_user_ref(online);
    }
    return ok;
}

bool host_mail_send(host_t *host, const char *from_name, const char *recipient,
                    const char *recipient_ip, const char *text,
                    bool *out_ddial, char *error, size_t error_cap)
{
    if (out_ddial != nullptr) {
        *out_ddial = false;
    }
    if (error == nullptr || error_cap == 0U) {
        return false;
    }
    error[0] = '\0';
    if (host == nullptr || from_name == nullptr || recipient == nullptr ||
        recipient[0] == '\0' || text == nullptr) {
        snprintf(error, error_cap, "%s", "Invalid mailbox recipient.");
        return false;
    }

    const bool force_ddial =
        recipient_ip != nullptr && strcasecmp(recipient_ip, "ddial") == 0;
    char ip[SSH_CHATTER_IP_LEN] = "";
    if (!force_ddial && recipient_ip != nullptr) {
        snprintf(ip, sizeof(ip), "%s", recipient_ip);
    }
    bool chatter_known =
        !force_ddial &&
        (ip[0] != '\0' || chat_room_find_user(&host->room, recipient) != nullptr);
    if (!force_ddial && !chatter_known) {
        /* Offline Chatter user: their last known address (existing records
         * only, nothing is created). */
        chatter_known = host_lookup_last_ip(host, recipient, ip, sizeof(ip));
    }
    if (force_ddial ||
        (!chatter_known && host_ddial_member_exists(host, recipient))) {
        if (out_ddial != nullptr) {
            *out_ddial = true;
        }
        return host_ddial_mail_chatter_to_member(host, from_name, recipient,
                                                 text, error, error_cap);
    }

    char clean[USER_DATA_MAILBOX_MESSAGE_LEN];
    if (!ddial_mail_clean_text(text, clean, sizeof(clean))) {
        snprintf(error, error_cap, "%s", "Mailbox message cannot be empty.");
        return false;
    }
    return host_user_data_send_mail(host, recipient,
                                    ip[0] != '\0' ? ip : nullptr, from_name,
                                    clean, error, error_cap);
}

/* E-mail over the Station Link for local member to_id. */
static void host_ddial_mail_link_inbound(host_t *host, unsigned from_station,
                                         unsigned to_id,
                                         const char *from_handle,
                                         const char *text)
{
    char clean[DDIAL_MV_TEXT_LIMIT];
    if (host == nullptr || to_id == 0U ||
        !ddial_mail_clean_text(text, clean, sizeof(clean))) {
        return;
    }
    char sender[DDIAL_MAX_HANDLE_LEN];
    snprintf(sender, sizeof(sender), "%.16s (station %03u)",
             from_handle != nullptr ? from_handle : "?", from_station);
    if (ddial_mail_send(host, to_id, 0U, sender, clean, false) == 0U) {
        printf("[ddial] link e-mail for member #%u dropped (no such member or "
               "mailbox full)\n",
               to_id);
        return;
    }
    ddial_mail_notify_member(host, to_id);
}

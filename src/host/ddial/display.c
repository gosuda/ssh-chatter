/**
 * @file display.c
 * @desc The one display-time filter for DDial relay lines in history.
 *
 * History stores what the link sent (the normalized wire text), untouched,
 * so the link roster can be replayed from it.  Whatever shows history to a
 * person -- SSH rendering and scrollback, the live broadcast, the JSON/web
 * event feed -- asks host_ddial_display_view() what to show instead of
 * reading entry->message directly, so old and new entries look the same:
 *
 *   "[SYSOP] 73-.SynerChat #0[T1:..." / "[LINK] -->. + #5[T1:..."
 *       link housekeeping: hidden.
 *   "[E-MAIL #012@070 Bob] text"
 *       link e-mail older builds wrote to the room: hidden (it is private).
 *   "71#4[T1:MaxMouse) text"
 *       shown as a Chatter chat message from MaxMouse.
 *   anything else
 *       shown as stored.
 */

#include "ssh_chatter/ddial_protocol.h"
#include "ssh_chatter/host.h"

#include <string.h>

bool host_ddial_display_view(const chat_history_entry_t *entry,
                             chat_history_entry_t *scratch,
                             const chat_history_entry_t **view)
{
    if (view != nullptr) {
        *view = entry;
    }
    if (entry == nullptr || entry->is_user_message) {
        return true;
    }
    if (ddial_roster_line_is_operational(entry->message)) {
        return false;
    }
    /* Link e-mail used to be written to the room; it is private. */
    if (strncmp(entry->message, "[E-MAIL ", 8U) == 0) {
        return false;
    }

    char handle[DDIAL_MAX_HANDLE_LEN];
    const char *body = nullptr;
    bool is_link = false;
    if (scratch == nullptr ||
        !ddial_parse_incoming_chat(entry->message, nullptr, nullptr, nullptr,
                                   &is_link, nullptr, handle, sizeof(handle),
                                   &body) ||
        is_link || handle[0] == '\0' || body == nullptr) {
        return true;
    }

    /* Same id, time and reactions; speaker and text taken from the line. */
    *scratch = *entry;
    scratch->is_user_message = true;
    snprintf(scratch->username, sizeof(scratch->username), "%s", handle);
    snprintf(scratch->raw_username, sizeof(scratch->raw_username), "%s",
             handle);
    snprintf(scratch->message, sizeof(scratch->message), "%s", body);
    if (view != nullptr) {
        *view = scratch;
    }
    return true;
}

/**
 * @file ddial_protocol.c
 * @desc Diversi Dial protocol parser and formatter.
 *
 * Reference: Diversi-DIAL Master 6502 assembly (refs/ddialmaster/dialmstr.txt)
 * and Retro-Dial/magviz.ca wire conventions.
 */

#include "ssh_chatter/ddial_protocol.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *ddial_tier_symbol(ddial_user_tier_t tier)
{
    switch (tier) {
    case DDIAL_TIER_PASSWORD:
        return "*";
    case DDIAL_TIER_MASTER:
        return "$";
    case DDIAL_TIER_GUEST:
    default:
        return "";
    }
}

static const char *ddial_tier_bracket_open(ddial_user_tier_t tier)
{
    switch (tier) {
    case DDIAL_TIER_PASSWORD:
        return "[";
    case DDIAL_TIER_MASTER:
        return "<";
    case DDIAL_TIER_GUEST:
    default:
        return "(";
    }
}

static const char *ddial_tier_bracket_close(ddial_user_tier_t tier)
{
    switch (tier) {
    case DDIAL_TIER_PASSWORD:
        return "]";
    case DDIAL_TIER_MASTER:
        return ">";
    case DDIAL_TIER_GUEST:
    default:
        return ")";
    }
}

ddial_command_t ddial_parse_command(const char *line, size_t line_len,
                                    ddial_parsed_message_t *out_msg)
{
    if (line == nullptr || line_len == 0U || out_msg == nullptr) {
        return DDIAL_CMD_UNKNOWN;
    }

    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->channel = DDIAL_DEFAULT_CHANNEL;

    size_t pos = 0U;
    while (pos < line_len && isspace((unsigned char)line[pos])) {
        ++pos;
    }
    if (pos >= line_len) {
        return DDIAL_CMD_UNKNOWN;
    }

    if (line[pos] != '/') {
        /* Bare text is treated as chat on current channel. */
        out_msg->command = DDIAL_CMD_CHAT;
        size_t copy_len = line_len - pos;
        if (copy_len >= sizeof(out_msg->body)) {
            copy_len = sizeof(out_msg->body) - 1U;
        }
        memcpy(out_msg->body, line + pos, copy_len);
        out_msg->body[copy_len] = '\0';
        return DDIAL_CMD_CHAT;
    }

    if (pos + 1U >= line_len) {
        return DDIAL_CMD_UNKNOWN;
    }

    char cmd_char = (char)toupper((unsigned char)line[pos + 1U]);
    size_t arg_start = pos + 2U;
    while (arg_start < line_len && isspace((unsigned char)line[arg_start])) {
        ++arg_start;
    }
    const char *args = (arg_start < line_len) ? line + arg_start : "";
    size_t args_len = (arg_start < line_len) ? line_len - arg_start : 0U;

    switch (cmd_char) {
    case 'C':
        out_msg->command = DDIAL_CMD_CHAT;
        break;
    case 'P':
        out_msg->command = DDIAL_CMD_PRIVATE;
        break;
    case 'J':
        out_msg->command = DDIAL_CMD_JOIN;
        break;
    case 'W':
        out_msg->command = DDIAL_CMD_WHO;
        return DDIAL_CMD_WHO;
    case 'E':
        out_msg->command = DDIAL_CMD_MAIL;
        return DDIAL_CMD_MAIL;
    case 'Q':
        out_msg->command = DDIAL_CMD_QUIT;
        return DDIAL_CMD_QUIT;
    case 'H':
    case '?':
        out_msg->command = DDIAL_CMD_HELP;
        return DDIAL_CMD_HELP;
    case 'T':
        out_msg->command = DDIAL_CMD_TIME;
        return DDIAL_CMD_TIME;
    case 'U':
        out_msg->command = DDIAL_CMD_USERS;
        return DDIAL_CMD_USERS;
    case 'S':
        out_msg->command = DDIAL_CMD_STATS;
        return DDIAL_CMD_STATS;
    case 'N':
        out_msg->command = DDIAL_CMD_NEWS;
        return DDIAL_CMD_NEWS;
    case 'I':
        out_msg->command = DDIAL_CMD_INFO;
        return DDIAL_CMD_INFO;
    case 'V':
        out_msg->command = DDIAL_CMD_VERSION;
        return DDIAL_CMD_VERSION;
    case 'B':
        out_msg->command = DDIAL_CMD_BELL;
        return DDIAL_CMD_BELL;
    case 'D':
        out_msg->command = DDIAL_CMD_DUPLEX;
        return DDIAL_CMD_DUPLEX;
    default:
        return DDIAL_CMD_UNKNOWN;
    }

    if (out_msg->command == DDIAL_CMD_PRIVATE && args_len > 0U) {
        /* Format: /P<handle><message> (handle is first token) */
        size_t name_start = 0U;
        while (name_start < args_len &&
               isspace((unsigned char)args[name_start])) {
            ++name_start;
        }
        size_t name_end = name_start;
        while (name_end < args_len &&
               !isspace((unsigned char)args[name_end])) {
            ++name_end;
        }
        size_t name_len = name_end - name_start;
        if (name_len > 0U && name_len < sizeof(out_msg->handle)) {
            memcpy(out_msg->handle, args + name_start, name_len);
            out_msg->handle[name_len] = '\0';
        }
        size_t body_start = name_end;
        while (body_start < args_len &&
               isspace((unsigned char)args[body_start])) {
            ++body_start;
        }
        size_t body_len = args_len - body_start;
        if (body_len >= sizeof(out_msg->body)) {
            body_len = sizeof(out_msg->body) - 1U;
        }
        if (body_len > 0U) {
            memcpy(out_msg->body, args + body_start, body_len);
            out_msg->body[body_len] = '\0';
        }
    } else if (out_msg->command == DDIAL_CMD_JOIN && args_len > 0U) {
        int ch = args[0] - '0';
        if (ch >= DDIAL_MIN_CHANNEL && ch <= DDIAL_MAX_CHANNEL) {
            out_msg->channel = (uint8_t)ch;
        }
    } else {
        size_t body_len = args_len;
        if (body_len >= sizeof(out_msg->body)) {
            body_len = sizeof(out_msg->body) - 1U;
        }
        if (body_len > 0U) {
            memcpy(out_msg->body, args, body_len);
            out_msg->body[body_len] = '\0';
        }
    }

    return out_msg->command;
}

bool ddial_format_chat(char *dst, size_t dst_cap, uint16_t slot,
                       uint8_t channel, ddial_user_tier_t tier,
                       const char *handle, const char *message)
{
    if (dst == nullptr || dst_cap == 0U || handle == nullptr ||
        message == nullptr) {
        return false;
    }

    char clean_handle[DDIAL_MAX_HANDLE_LEN];
    ddial_strip_ansi(handle, strlen(handle), clean_handle,
                     sizeof(clean_handle));

    const char *bo = ddial_tier_bracket_open(tier);
    const char *bc = ddial_tier_bracket_close(tier);
    const char *sym = ddial_tier_symbol(tier);

    if (channel < DDIAL_MIN_CHANNEL) {
        channel = DDIAL_DEFAULT_CHANNEL;
    }
    if (channel > DDIAL_MAX_CHANNEL) {
        channel = DDIAL_MAX_CHANNEL;
    }

    int written = snprintf(dst, dst_cap, "#%u%sCH%u:%s%s%s %s\r\n",
                           (unsigned int)slot, bo, (unsigned int)channel,
                           clean_handle, sym, bc, message);
    return written > 0 && (size_t)written < dst_cap;
}

bool ddial_format_private(char *dst, size_t dst_cap, uint16_t from_slot,
                          const char *from_handle, const char *message)
{
    if (dst == nullptr || dst_cap == 0U || from_handle == nullptr ||
        message == nullptr) {
        return false;
    }
    char clean_handle[DDIAL_MAX_HANDLE_LEN];
    ddial_strip_ansi(from_handle, strlen(from_handle), clean_handle,
                     sizeof(clean_handle));
    int written =
        snprintf(dst, dst_cap, "IM from #%u[%s]: %s\r\n",
                 (unsigned int)from_slot, clean_handle, message);
    return written > 0 && (size_t)written < dst_cap;
}

bool ddial_format_who_entry(char *dst, size_t dst_cap, uint16_t slot,
                            uint8_t channel, ddial_user_tier_t tier,
                            const char *handle, uint16_t account)
{
    if (dst == nullptr || dst_cap == 0U || handle == nullptr) {
        return false;
    }
    char clean_handle[DDIAL_MAX_HANDLE_LEN];
    ddial_strip_ansi(handle, strlen(handle), clean_handle,
                     sizeof(clean_handle));
    const char *bo = ddial_tier_bracket_open(tier);
    const char *bc = ddial_tier_bracket_close(tier);
    const char *sym = ddial_tier_symbol(tier);

    int written =
        snprintf(dst, dst_cap, "#%u%sCH%u:%s%s) #%03u\r\n",
                 (unsigned int)slot, bo, (unsigned int)channel, clean_handle,
                 sym, (unsigned int)account);
    (void)bc;
    return written > 0 && (size_t)written < dst_cap;
}

bool ddial_format_prompt(char *dst, size_t dst_cap)
{
    if (dst == nullptr || dst_cap == 0U) {
        return false;
    }
    int written = snprintf(dst, dst_cap, "--> ");
    return written > 0 && (size_t)written < dst_cap;
}

bool ddial_format_system(char *dst, size_t dst_cap, const char *message)
{
    if (dst == nullptr || dst_cap == 0U || message == nullptr) {
        return false;
    }
    int written = snprintf(dst, dst_cap, "** %s **\r\n", message);
    return written > 0 && (size_t)written < dst_cap;
}

size_t ddial_strip_ansi(const char *src, size_t src_len, char *dst,
                        size_t dst_cap)
{
    if (src == nullptr || dst == nullptr || dst_cap == 0U) {
        return 0U;
    }

    size_t j = 0U;
    for (size_t i = 0U; i < src_len && j + 1U < dst_cap; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == 0x1B) {
            if (i + 1U < src_len && src[i + 1U] == '[') {
                i += 2U;
                while (i < src_len) {
                    unsigned char p = (unsigned char)src[i];
                    if (p >= 0x40 && p <= 0x7E) {
                        break;
                    }
                    ++i;
                }
                continue;
            }
            if (i + 1U < src_len) {
                ++i;
                continue;
            }
            continue;
        }
        if (c == 0x07 || c == 0x08) {
            continue;
        }
        dst[j++] = (char)c;
    }
    dst[j] = '\0';
    return j;
}

size_t ddial_filter_telnet_iac(const char *src, size_t src_len, char *dst,
                               size_t dst_cap)
{
    if (src == nullptr || dst == nullptr || dst_cap == 0U) {
        return 0U;
    }

    size_t j = 0U;
    for (size_t i = 0U; i < src_len && j < dst_cap; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c != 0xFF) {
            if (j + 1U < dst_cap) {
                dst[j++] = (char)c;
            }
            continue;
        }
        if (i + 1U >= src_len) {
            break;
        }
        unsigned char cmd = (unsigned char)src[i + 1U];
        if (cmd == 0xFF) {
            dst[j++] = '\xFF';
            ++i;
        } else if (cmd >= 0xF0 && cmd <= 0xF9) {
            ++i;
        } else if ((cmd >= 0xFB && cmd <= 0xFE) && i + 2U < src_len) {
            i += 2U;
        } else {
            ++i;
        }
    }
    return j;
}

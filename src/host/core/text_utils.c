static int session_utf8_display_width(const char *text)
{
    if (text == nullptr) {
        return 0;
    }

    mbstate_t state;
    memset(&state, 0, sizeof(state));

    int width = 0;
    const char *cursor = text;
    size_t remaining = strlen(text);
    while (remaining > 0U) {
        wchar_t wc;
        size_t consumed = mbrtowc(&wc, cursor, remaining, &state);
        if (consumed == (size_t)-1 || consumed == (size_t)-2) {
            ++cursor;
            --remaining;
            memset(&state, 0, sizeof(state));
            width += 1;
            continue;
        }
        if (consumed == 0U) {
            break;
        }

        int char_width = wcwidth(wc);
        if (char_width < 0) {
            char_width = 1;
        }
        width += char_width;
        cursor += consumed;
        remaining -= consumed;
    }

    return width;
}


static void session_format_template(const char *format, const char *const *args,
                                    size_t arg_count, char *buffer,
                                    size_t length)
{
    if (buffer == nullptr || length == 0U) {
        return;
    }

    buffer[0] = '\0';
    if (format == nullptr) {
        return;
    }

    size_t out_index = 0U;
    size_t arg_index = 0U;
    for (size_t idx = 0U; format[idx] != '\0' && out_index + 1U < length;
         ++idx) {
        if (format[idx] == '%' && format[idx + 1U] == 's') {
            const char *replacement =
                (args != nullptr && arg_index < arg_count &&
                 args[arg_index] != nullptr)
                    ? args[arg_index]
                    : "";
            size_t available = length - out_index - 1U;
            size_t rep_len = strnlen(replacement, available);
            memcpy(buffer + out_index, replacement, rep_len);
            out_index += rep_len;
            ++arg_index;
            ++idx;
            continue;
        }
        if (format[idx] == '%' && format[idx + 1U] == '%') {
            buffer[out_index++] = '%';
            ++idx;
            continue;
        }

        buffer[out_index++] = format[idx];
    }

    buffer[out_index] = '\0';
}

static size_t session_help_collect_arguments(
    session_ctx_t *ctx, const session_help_template_arg_kind_t *kinds,
    size_t kind_count, const char **output, size_t capacity)
{
    if (output == nullptr || capacity == 0U) {
        return 0U;
    }

    size_t produced = 0U;
    const char *prefix = session_command_prefix(ctx);

    if (kind_count == 0U) {
        size_t repeat = capacity < 8U ? capacity : 8U;
        for (size_t idx = 0; idx < repeat; ++idx) {
            output[produced++] = prefix;
        }
        return produced;
    }

    for (size_t idx = 0; idx < kind_count && produced < capacity; ++idx) {
        const char *value = "";
        switch (kinds[idx]) {
        case SESSION_HELP_TEMPLATE_ARG_PREFIX:
            value = prefix;
            break;
        case SESSION_HELP_TEMPLATE_ARG_ASCIIART_TERMINATOR:
            value = session_asciiart_terminator(ctx);
            break;
        case SESSION_HELP_TEMPLATE_ARG_BBS_TERMINATOR:
            value = session_bbs_terminator(ctx);
            break;
        case SESSION_HELP_TEMPLATE_ARG_COMMAND_REPLY:
            value = session_command_alias_preferred_by_canonical(ctx, "/reply");
            break;
        case SESSION_HELP_TEMPLATE_ARG_COMMAND_GOOD:
            value = session_command_alias_preferred_by_canonical(ctx, "/good");
            break;
        case SESSION_HELP_TEMPLATE_ARG_COMMAND_SAD:
            value = session_command_alias_preferred_by_canonical(ctx, "/sad");
            break;
        case SESSION_HELP_TEMPLATE_ARG_COMMAND_WTF:
            value = session_command_alias_preferred_by_canonical(ctx, "/wtf");
            break;
        case SESSION_HELP_TEMPLATE_ARG_COMMAND_COOL:
            value = session_command_alias_preferred_by_canonical(ctx, "/cool");
            break;
        case SESSION_HELP_TEMPLATE_ARG_COMMAND_ANGRY:
            value = session_command_alias_preferred_by_canonical(ctx, "/angry");
            break;
        case SESSION_HELP_TEMPLATE_ARG_COMMAND_CHECKED:
            value =
                session_command_alias_preferred_by_canonical(ctx, "/checked");
            break;
        case SESSION_HELP_TEMPLATE_ARG_COMMAND_LOVE:
            value = session_command_alias_preferred_by_canonical(ctx, "/love");
            break;
        case SESSION_HELP_TEMPLATE_ARG_COMMAND_BBS:
            value = session_command_alias_preferred_by_canonical(ctx, "/bbs");
            break;
        default:
            value = prefix;
            break;
        }
        if (value == nullptr) {
            value = "";
        }
        output[produced++] = value;
    }

    return produced;
}

static void session_format_help_line(session_ctx_t *ctx,
                                     const session_help_entry_t *entry,
                                     const char *description, char *buffer,
                                     size_t length)
{
    if (ctx == nullptr || entry == nullptr || buffer == nullptr ||
        length == 0U) {
        return;
    }

    const size_t language_index = (size_t)session_ui_language_current(ctx);
    const char *label_template = entry->label;
    if (language_index < SESSION_UI_LANGUAGE_COUNT &&
        entry->label_translations[language_index] != nullptr &&
        entry->label_translations[language_index][0] != '\0') {
        label_template = entry->label_translations[language_index];
    }
    if (label_template == nullptr) {
        label_template = "";
    }

    const char *prefix = session_command_prefix(ctx);
    char label[128];
    label[0] = '\0';

    if (entry->kind == SESSION_HELP_ENTRY_COMMAND) {
        snprintf(label, sizeof(label), "%s%s", prefix, label_template);
    } else if (entry->kind == SESSION_HELP_ENTRY_FORMATTED) {
        const char *args[SESSION_HELP_TEMPLATE_ARG_LIMIT];
        size_t arg_count = session_help_collect_arguments(
            ctx, entry->label_args, entry->label_arg_count, args,
            sizeof(args) / sizeof(args[0]));
        session_format_template(label_template, args, arg_count, label,
                                sizeof(label));
    }

    if (entry->kind == SESSION_HELP_ENTRY_TEXT) {
        snprintf(buffer, length, "%s",
                 description != nullptr ? description : "");
        return;
    }

    int display_width = session_utf8_display_width(label);
    if (display_width < 0) {
        display_width = 0;
    }

    int padding = kSessionHelpLabelWidth - display_width;
    if (padding < 1) {
        padding = 1;
    }
    if (padding > 32) {
        padding = 32;
    }

    char padding_buffer[33];
    memset(padding_buffer, ' ', (size_t)padding);
    padding_buffer[padding] = '\0';

    snprintf(buffer, length, "%s%s- %s", label, padding_buffer,
             description != nullptr ? description : "");
}

static void session_format_help_entries_to_buffer(
    session_ctx_t *ctx, const session_help_entry_t *entries, size_t count,
    char *buffer, size_t buffer_length)
{
    if (ctx == nullptr || entries == nullptr || buffer == nullptr ||
        buffer_length == 0U) {
        if (buffer != nullptr && buffer_length > 0U) {
            buffer[0] = '\0';
        }
        return;
    }

    buffer[0] = '\0';
    size_t current_offset = 0U;
    const size_t language_index = (size_t)session_ui_language_current(ctx);

    for (size_t idx = 0; idx < count; ++idx) {
        const session_help_entry_t *entry = &entries[idx];
        const char *format = entry->description[language_index];
        if (format == nullptr || format[0] == '\0') {
            continue;
        }

        char description[SSH_CHATTER_MESSAGE_LIMIT];
        const char *args[SESSION_HELP_TEMPLATE_ARG_LIMIT];
        size_t arg_count = session_help_collect_arguments(
            ctx, entry->description_args, entry->description_arg_count, args,
            sizeof(args) / sizeof(args[0]));
        session_format_template(format, args, arg_count, description,
                                sizeof(description));

        char line_buffer[SSH_CHATTER_MESSAGE_LIMIT];
        if (entry->kind == SESSION_HELP_ENTRY_TEXT) {
            snprintf(line_buffer, sizeof(line_buffer), "%s", description);
        } else {
            session_format_help_line(ctx, entry, description, line_buffer,
                                     sizeof(line_buffer));
        }

        size_t line_len = strnlen(line_buffer, sizeof(line_buffer));
        if (current_offset + line_len + 1U < buffer_length) { // +1 for newline
            memcpy(buffer + current_offset, line_buffer, line_len);
            current_offset += line_len;
            buffer[current_offset++] = '\n';
        } else {
            // Buffer full, stop adding lines
            break;
        }
    }
    buffer[current_offset] = '\0';
}

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

typedef struct {
    char question_en[256];
    char question_ko[256];
    char question_ru[256];
    char question_zh[256];
    char answer[64];
} captcha_prompt_t;

typedef enum {
    CAPTCHA_LANGUAGE_KO = 0,
    CAPTCHA_LANGUAGE_EN,
    CAPTCHA_LANGUAGE_ZH,
    CAPTCHA_LANGUAGE_RU,
    CAPTCHA_LANGUAGE_COUNT,
} captcha_language_t;

typedef enum {
    HOST_SECURITY_SCAN_CLEAN = 0,
    HOST_SECURITY_SCAN_BLOCKED,
    HOST_SECURITY_SCAN_ERROR,
} host_security_scan_result_t;

static unsigned session_prng_next(unsigned *state)
{
    if (state == nullptr) {
        return 0U;
    }

    *state = (*state * 1664525U) + 1013904223U;
    return *state;
}

static void session_fill_digit_sum_prompt(captcha_prompt_t *prompt,
                                          unsigned *state)
{
    if (prompt == nullptr) {
        return;
    }

    unsigned digits_count = 3U;
    if (state != nullptr) {
        digits_count = 2U + (session_prng_next(state) % 2U);
    }
    if (digits_count < 2U) {
        digits_count = 2U;
    }
    if (digits_count > 3U) {
        digits_count = 3U;
    }

    unsigned digits[4] = {0U, 0U, 0U, 0U};
    unsigned sum = 0U;

    if (digits_count == 3U) {
        bool valid = false;
        for (unsigned attempt = 0U; attempt < 16U && !valid; ++attempt) {
            sum = 0U;
            for (unsigned idx = 0U; idx < digits_count; ++idx) {
                unsigned raw = (unsigned)((idx + 1U) % 10U);
                if (state != nullptr) {
                    raw = session_prng_next(state) % 10U;
                }
                digits[idx] = raw;
                sum += raw;
            }
            if (sum < 10U) {
                valid = true;
            }
        }

        if (!valid && sum >= 10U) {
            unsigned overflow = sum - 9U;
            for (int idx = (int)digits_count - 1; idx >= 0 && overflow > 0U;
                 --idx) {
                unsigned current = digits[(size_t)idx];
                unsigned reduction = current > overflow ? overflow : current;
                digits[(size_t)idx] = current - reduction;
                sum -= reduction;
                overflow -= reduction;
            }
            if (sum >= 10U) {
                digits[0] = 3U;
                digits[1] = 3U;
                digits[2] = 3U;
                sum = 9U;
            }
        }
    } else {
        sum = 0U;
        for (unsigned idx = 0U; idx < digits_count; ++idx) {
            unsigned raw = (unsigned)(idx + 1U);
            if (state != nullptr) {
                raw = (session_prng_next(state) % 9U) + 1U;
            } else {
                raw = (raw % 9U) + 1U;
            }
            digits[idx] = raw;
            sum += raw;
        }
    }

    char expression[64];
    expression[0] = '\0';
    size_t written = 0U;
    for (unsigned idx = 0U; idx < digits_count; ++idx) {
        int appended = 0;
        if (idx == 0U) {
            appended =
                snprintf(expression + written, sizeof(expression) - written,
                         "%u", digits[idx]);
        } else {
            appended =
                snprintf(expression + written, sizeof(expression) - written,
                         " + %u", digits[idx]);
        }
        if (appended < 0) {
            expression[sizeof(expression) - 1U] = '\0';
            break;
        }
        size_t appended_size = (size_t)appended;
        if (appended_size >= sizeof(expression) - written) {
            expression[sizeof(expression) - 1U] = '\0';
            break;
        }
        written += appended_size;
    }

    snprintf(prompt->question_en, sizeof(prompt->question_en),
             "Add the digits: %s = ?", expression);
    snprintf(prompt->question_ko, sizeof(prompt->question_ko),
             "다음 숫자들의 합은 얼마인가요? %s = ?", expression);
    snprintf(prompt->question_ru, sizeof(prompt->question_ru),
             "Чему равна сумма цифр: %s = ?", expression);
    snprintf(prompt->question_zh, sizeof(prompt->question_zh),
             "請計算以下數字的總和：%s = ?", expression);
    snprintf(prompt->answer, sizeof(prompt->answer), "%u", sum);
}

static bool string_contains_case_insensitive(const char *haystack,
                                             const char *needle)
{
    if (haystack == nullptr || needle == nullptr || *needle == '\0') {
        return false;
    }

    const size_t haystack_length = strlen(haystack);
    const size_t needle_length = strlen(needle);
    if (needle_length == 0U || haystack_length < needle_length) {
        return false;
    }

    for (size_t idx = 0; idx <= haystack_length - needle_length; ++idx) {
        size_t matched = 0U;
        while (matched < needle_length) {
            const unsigned char hay = (unsigned char)haystack[idx + matched];
            const unsigned char nee = (unsigned char)needle[matched];
            if (tolower(hay) != tolower(nee)) {
                break;
            }
            ++matched;
        }
        if (matched == needle_length) {
            return true;
        }
    }

    return false;
}

static bool session_editor_matches_terminator(const session_ctx_t *ctx,
                                              const char *line)
{
    if (ctx == nullptr) {
        return false;
    }

    if (ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART) {
        return session_asciiart_matches_terminator(line);
    }

    return session_bbs_matches_terminator(line);
}

static bool string_contains_token_case_insensitive(const char *haystack,
                                                   const char *needle)
{
    if (haystack == nullptr || needle == nullptr || *needle == '\0') {
        return false;
    }

    const size_t haystack_length = strlen(haystack);
    const size_t needle_length = strlen(needle);
    if (needle_length == 0U || haystack_length < needle_length) {
        return false;
    }

    for (size_t idx = 0; idx <= haystack_length - needle_length; ++idx) {
        size_t matched = 0U;
        while (matched < needle_length) {
            const unsigned char hay = (unsigned char)haystack[idx + matched];
            const unsigned char nee = (unsigned char)needle[matched];
            if (tolower(hay) != tolower(nee)) {
                break;
            }
            ++matched;
        }

        if (matched != needle_length) {
            continue;
        }

        const bool has_prev = idx > 0U;
        const bool has_next = (idx + needle_length) < haystack_length;
        const unsigned char prev =
            has_prev ? (unsigned char)haystack[idx - 1U] : 0U;
        const unsigned char next =
            has_next ? (unsigned char)haystack[idx + needle_length] : 0U;
        const bool prev_boundary = !has_prev || (!isalnum(prev) && prev != '_');
        const bool next_boundary = !has_next || (!isalnum(next) && next != '_');

        if (prev_boundary && next_boundary) {
            return true;
        }
    }

    return false;
}

static void session_extract_banner_token(const char *banner, char *buffer,
                                         size_t length)
{
    if (buffer == nullptr || length == 0U) {
        return;
    }

    buffer[0] = '\0';
    if (banner == nullptr || *banner == '\0') {
        return;
    }

    size_t idx = 0U;
    while (banner[idx] != '\0' && isspace((unsigned char)banner[idx])) {
        ++idx;
    }

    size_t produced = 0U;
    while (banner[idx] != '\0' && !isspace((unsigned char)banner[idx])) {
        if (produced + 1U >= length) {
            break;
        }
        buffer[produced++] = banner[idx++];
    }

    if (produced == 0U) {
        snprintf(buffer, length, "%.*s", (int)length - 1, banner);
    } else {
        buffer[produced] = '\0';
    }
}

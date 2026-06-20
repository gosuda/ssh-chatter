#include "ssh_chatter/host.h"
#include "../internal.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *archive_text_header(session_ui_language_t lang)
{
    switch (lang) {
    case SESSION_UI_LANGUAGE_KO: return "\033[1;36m=== 아카이브 ===\033[0m";
    case SESSION_UI_LANGUAGE_JP: return "\033[1;36m=== アーカイブ ===\033[0m";
    case SESSION_UI_LANGUAGE_ZH: return "\033[1;36m=== 档案 ===\033[0m";
    case SESSION_UI_LANGUAGE_RU: return "\033[1;36m=== Архив ===\033[0m";
    case SESSION_UI_LANGUAGE_DE: return "\033[1;36m=== Archiv ===\033[0m";
    case SESSION_UI_LANGUAGE_FR: return "\033[1;36m=== Archive ===\033[0m";
    case SESSION_UI_LANGUAGE_PL: return "\033[1;36m=== Archiwum ===\033[0m";
    default: return "\033[1;36m=== Archive ===\033[0m";
    }
}

static const char *archive_text_showing_format(session_ui_language_t lang)
{
    switch (lang) {
    case SESSION_UI_LANGUAGE_KO: return "보관된 메시지 %zu개를 표시합니다.";
    case SESSION_UI_LANGUAGE_JP: return "アーカイブされたメッセージ %zu 件を表示します。";
    case SESSION_UI_LANGUAGE_ZH: return "显示 %zu 条已归档消息。";
    case SESSION_UI_LANGUAGE_RU: return "Показано архивных сообщений: %zu.";
    case SESSION_UI_LANGUAGE_DE: return "Zeige %zu archivierte Nachricht(en).";
    case SESSION_UI_LANGUAGE_FR: return "Affichage de %zu message(s) archivé(s).";
    case SESSION_UI_LANGUAGE_PL: return "Wyświetlanie %zu zarchiwizowanych wiadomości.";
    default: return "Showing %zu archived message(s).";
    }
}

static const char *archive_text_exit_hint(session_ui_language_t lang)
{
    switch (lang) {
    case SESSION_UI_LANGUAGE_KO: return "채팅으로 돌아가려면 /archive exit를 입력하세요.";
    case SESSION_UI_LANGUAGE_JP: return "チャットに戻るには /archive exit を入力してください。";
    case SESSION_UI_LANGUAGE_ZH: return "输入 /archive exit 返回聊天。";
    case SESSION_UI_LANGUAGE_RU: return "Введите /archive exit, чтобы вернуться в чат.";
    case SESSION_UI_LANGUAGE_DE: return "Geben Sie /archive exit ein, um zum Chat zurückzukehren.";
    case SESSION_UI_LANGUAGE_FR: return "Tapez /archive exit pour revenir au chat.";
    case SESSION_UI_LANGUAGE_PL: return "Wpisz /archive exit, aby wrócić do czatu.";
    default: return "Type /archive exit to return to chat.";
    }
}

static const char *archive_text_no_files(session_ui_language_t lang)
{
    switch (lang) {
    case SESSION_UI_LANGUAGE_KO: return "보관된 대화 파일이 없습니다.";
    case SESSION_UI_LANGUAGE_JP: return "アーカイブされたチャットファイルが見つかりません。";
    case SESSION_UI_LANGUAGE_ZH: return "未找到已归档的聊天文件。";
    case SESSION_UI_LANGUAGE_RU: return "Архивные файлы чатов не найдены.";
    case SESSION_UI_LANGUAGE_DE: return "Keine archivierten Chat-Dateien gefunden.";
    case SESSION_UI_LANGUAGE_FR: return "Aucun fichier de discussion archivé trouvé.";
    case SESSION_UI_LANGUAGE_PL: return "Nie znaleziono zarchiwizowanych plików czatu.";
    default: return "No archived chat files found.";
    }
}

static const char *archive_text_file_list_header(session_ui_language_t lang)
{
    switch (lang) {
    case SESSION_UI_LANGUAGE_KO: return "보관된 대화 파일:";
    case SESSION_UI_LANGUAGE_JP: return "アーカイブされたチャットファイル:";
    case SESSION_UI_LANGUAGE_ZH: return "已归档的聊天文件:";
    case SESSION_UI_LANGUAGE_RU: return "Архивные файлы чатов:";
    case SESSION_UI_LANGUAGE_DE: return "Archivierte Chat-Dateien:";
    case SESSION_UI_LANGUAGE_FR: return "Fichiers de discussion archivés :";
    case SESSION_UI_LANGUAGE_PL: return "Zarchiwizowane pliki czatu:";
    default: return "Archived chat files:";
    }
}

static const char *archive_text_no_date_format(session_ui_language_t lang)
{
    switch (lang) {
    case SESSION_UI_LANGUAGE_KO: return "해당 날짜의 아카이브가 없거나 잘못된 날짜입니다: %s.";
    case SESSION_UI_LANGUAGE_JP: return "該当する日付のアーカイブがないか、無効な日付です: %s。";
    case SESSION_UI_LANGUAGE_ZH: return "未找到该日期的档案或日期无效：%s。";
    case SESSION_UI_LANGUAGE_RU: return "Архив не найден или указана неверная дата: %s.";
    case SESSION_UI_LANGUAGE_DE: return "Kein Archiv für das Datum gefunden oder ungültiges Datum: %s.";
    case SESSION_UI_LANGUAGE_FR: return "Aucun archive pour cette date ou date invalide : %s.";
    case SESSION_UI_LANGUAGE_PL: return "Nie znaleziono archiwum dla podanej daty lub data jest nieprawidłowa: %s.";
    default: return "No archive found or invalid date: %s.";
    }
}

static const char *archive_text_not_in_mode(session_ui_language_t lang)
{
    switch (lang) {
    case SESSION_UI_LANGUAGE_KO: return "현재 아카이브 모드가 아닙니다.";
    case SESSION_UI_LANGUAGE_JP: return "現在アーカイブモードではありません。";
    case SESSION_UI_LANGUAGE_ZH: return "当前不在档案模式。";
    case SESSION_UI_LANGUAGE_RU: return "Вы не в режиме архива.";
    case SESSION_UI_LANGUAGE_DE: return "Sie befinden sich nicht im Archivmodus.";
    case SESSION_UI_LANGUAGE_FR: return "Vous n'êtes pas en mode archive.";
    case SESSION_UI_LANGUAGE_PL: return "Nie jesteś w trybie archiwum.";
    default: return "You are not in archive mode.";
    }
}

static const char *archive_text_left_mode(session_ui_language_t lang)
{
    switch (lang) {
    case SESSION_UI_LANGUAGE_KO: return "아카이브 모드를 종료했습니다.";
    case SESSION_UI_LANGUAGE_JP: return "アーカイブモードを終了しました。";
    case SESSION_UI_LANGUAGE_ZH: return "已退出档案模式。";
    case SESSION_UI_LANGUAGE_RU: return "Режим архива завершён.";
    case SESSION_UI_LANGUAGE_DE: return "Archivmodus verlassen.";
    case SESSION_UI_LANGUAGE_FR: return "Mode archive quitté.";
    case SESSION_UI_LANGUAGE_PL: return "Opuszczono tryb archiwum.";
    default: return "Left archive mode.";
    }
}

static const char *archive_text_usage(session_ui_language_t lang)
{
    switch (lang) {
    case SESSION_UI_LANGUAGE_KO: return "사용법: /archive list | enter <YYYY-MM-DD> | exit";
    case SESSION_UI_LANGUAGE_JP: return "使い方: /archive list | enter <YYYY-MM-DD> | exit";
    case SESSION_UI_LANGUAGE_ZH: return "用法：/archive list | enter <YYYY-MM-DD> | exit";
    case SESSION_UI_LANGUAGE_RU: return "Использование: /archive list | enter <YYYY-MM-DD> | exit";
    case SESSION_UI_LANGUAGE_DE: return "Nutzung: /archive list | enter <YYYY-MM-DD> | exit";
    case SESSION_UI_LANGUAGE_FR: return "Utilisation : /archive list | enter <YYYY-MM-DD> | exit";
    case SESSION_UI_LANGUAGE_PL: return "Użycie: /archive list | enter <YYYY-MM-DD> | exit";
    default: return "Usage: /archive list | enter <YYYY-MM-DD> | exit";
    }
}

static const char *archive_text_enter_usage(session_ui_language_t lang)
{
    switch (lang) {
    case SESSION_UI_LANGUAGE_KO: return "사용법: /archive enter <YYYY-MM-DD> (UTC 날짜)";
    case SESSION_UI_LANGUAGE_JP: return "使い方: /archive enter <YYYY-MM-DD> (UTC 日付)";
    case SESSION_UI_LANGUAGE_ZH: return "用法：/archive enter <YYYY-MM-DD>（UTC 日期）";
    case SESSION_UI_LANGUAGE_RU: return "Использование: /archive enter <YYYY-MM-DD> (дата UTC)";
    case SESSION_UI_LANGUAGE_DE: return "Nutzung: /archive enter <YYYY-MM-DD> (UTC-Datum)";
    case SESSION_UI_LANGUAGE_FR: return "Utilisation : /archive enter <YYYY-MM-DD> (date UTC)";
    case SESSION_UI_LANGUAGE_PL: return "Użycie: /archive enter <YYYY-MM-DD> (data UTC)";
    default: return "Usage: /archive enter <YYYY-MM-DD> (UTC date)";
    }
}

static session_ui_language_t archive_current_language(session_ctx_t *ctx)
{
    session_ui_language_t lang = session_ui_language_current(ctx);
    if (lang < 0 || lang >= SESSION_UI_LANGUAGE_COUNT) {
        lang = SESSION_UI_LANGUAGE_EN;
    }
    return lang;
}

static bool host_archive_list_files(host_t *host, char ***out_files,
                                    size_t *out_count)
{
    if (host == nullptr || out_files == nullptr || out_count == nullptr) {
        return false;
    }

    *out_files = nullptr;
    *out_count = 0U;

    char base_dir[PATH_MAX];
    base_dir[0] = '\0';

    const char *archive_dir = getenv("CHATTER_ARCHIVE_DIR");
    if (archive_dir == nullptr || archive_dir[0] == '\0') {
        archive_dir = getenv("CHATTER_STATE_DIR");
    }
    if (archive_dir != nullptr && archive_dir[0] != '\0') {
        snprintf(base_dir, sizeof(base_dir), "%s", archive_dir);
    } else if (host->state_file_path[0] != '\0') {
        const char *last_slash = strrchr(host->state_file_path, '/');
        if (last_slash != nullptr) {
            size_t len = (size_t)(last_slash - host->state_file_path);
            if (len >= sizeof(base_dir)) {
                len = sizeof(base_dir) - 1U;
            }
            memcpy(base_dir, host->state_file_path, len);
            base_dir[len] = '\0';
        }
    }

    const char *scan_dir = base_dir[0] != '\0' ? base_dir : ".";
    DIR *dir = opendir(scan_dir);
    if (dir == nullptr) {
        return false;
    }

    char **files = nullptr;
    size_t count = 0U;
    size_t capacity = 0U;

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "chat_archive_", 13) != 0 ||
            strcmp(entry->d_name + strlen(entry->d_name) - 4, ".dat") != 0) {
            continue;
        }
        if (count >= capacity) {
            size_t new_capacity = capacity == 0U ? 8U : capacity * 2U;
            char **resized = (char **)realloc(files, new_capacity * sizeof(char *));
            if (resized == nullptr) {
                break;
            }
            files = resized;
            capacity = new_capacity;
        }
        files[count] = strdup(entry->d_name);
        if (files[count] == nullptr) {
            break;
        }
        ++count;
    }
    closedir(dir);

    qsort(files, count, sizeof(char *),
          (int (*)(const void *, const void *))strcmp);

    *out_files = files;
    *out_count = count;
    return true;
}

static void host_archive_free_file_list(char **files, size_t count)
{
    if (files == nullptr) {
        return;
    }
    for (size_t idx = 0U; idx < count; ++idx) {
        free(files[idx]);
    }
    free(files);
}

static void session_archive_render_entries(session_ctx_t *ctx,
                                           const chat_history_entry_t *entries,
                                           size_t count)
{
    if (ctx == nullptr || entries == nullptr || count == 0U) {
        return;
    }

    const session_ui_language_t lang = archive_current_language(ctx);
    const bool buffering_started = !ctx->output_buffering_enabled;
    if (buffering_started) {
        session_output_buffer_start(ctx);
    }

    session_send_system_line(ctx, archive_text_header(lang));
    char header[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(header, sizeof(header), archive_text_showing_format(lang), count);
    session_send_system_line(ctx, header);

    for (size_t idx = 0U; idx < count; ++idx) {
        session_send_history_entry(ctx, &entries[idx]);
    }

    session_send_system_line(ctx, archive_text_exit_hint(lang));

    if (buffering_started) {
        session_output_buffer_stop(ctx);
    }
}

static void session_handle_archive_list(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    const session_ui_language_t lang = archive_current_language(ctx);
    char **files = nullptr;
    size_t count = 0U;
    if (!host_archive_list_files(ctx->owner, &files, &count) || count == 0U) {
        session_send_system_line(ctx, archive_text_no_files(lang));
        return;
    }

    session_send_system_line(ctx, archive_text_file_list_header(lang));
    for (size_t idx = 0U; idx < count; ++idx) {
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(line, sizeof(line), "  %s", files[idx]);
        session_send_system_line(ctx, line);
    }
    host_archive_free_file_list(files, count);
}

static void session_handle_archive_enter(session_ctx_t *ctx, const char *date_str)
{
    if (ctx == nullptr || ctx->owner == nullptr || date_str == nullptr) {
        return;
    }

    const session_ui_language_t lang = archive_current_language(ctx);
    chat_history_entry_t *entries = nullptr;
    size_t count = host_archive_read_date(ctx->owner, date_str, &entries);
    if (count == 0U || entries == nullptr) {
        char msg[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(msg, sizeof(msg), archive_text_no_date_format(lang), date_str);
        session_send_system_line(ctx, msg);
        return;
    }

    ctx->in_archive_mode = true;
    ctx->archive_view_date = 0;

    session_archive_render_entries(ctx, entries, count);
}

static void session_handle_archive_exit(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    const session_ui_language_t lang = archive_current_language(ctx);
    if (!ctx->in_archive_mode) {
        session_send_system_line(ctx, archive_text_not_in_mode(lang));
        return;
    }

    ctx->in_archive_mode = false;
    ctx->archive_view_date = 0;
    session_send_system_line(ctx, archive_text_left_mode(lang));
    session_scrollback_reset_position(ctx);
}

void session_handle_archive(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    const session_ui_language_t lang = archive_current_language(ctx);

    char working[SSH_CHATTER_MAX_INPUT_LEN];
    if (arguments == nullptr) {
        working[0] = '\0';
    } else {
        snprintf(working, sizeof(working), "%s", arguments);
    }
    trim_whitespace_inplace(working);

    if (working[0] == '\0') {
        session_send_system_line(ctx, archive_text_usage(lang));
        return;
    }

    char *saveptr = nullptr;
    char *command = strtok_r(working, " \t", &saveptr);
    if (command == nullptr) {
        session_send_system_line(ctx, archive_text_usage(lang));
        return;
    }

    if (strcasecmp(command, "list") == 0) {
        session_handle_archive_list(ctx);
        return;
    }

    if (strcasecmp(command, "enter") == 0) {
        char *date = strtok_r(nullptr, " \t", &saveptr);
        if (date == nullptr || date[0] == '\0') {
            session_send_system_line(ctx, archive_text_enter_usage(lang));
            return;
        }
        session_handle_archive_enter(ctx, date);
        return;
    }

    if (strcasecmp(command, "exit") == 0) {
        session_handle_archive_exit(ctx);
        return;
    }

    session_send_system_line(ctx, archive_text_usage(lang));
}

/**
 * @file door_adapter.c
 * @desc Adapter shim between ssh-chatter internals and libdoorgame.
 */

#include "ssh_chatter/host.h"
#include "doorgame/doorgame.h"

#ifndef nullptr
#define nullptr (void *)(NULL)
#endif

/* ------------------------------------------------------------------------- */
/* Localization helper (mirrors the old in-tree bbs_door.c implementation)   */
/* ------------------------------------------------------------------------- */
static const char *door_localized(session_ctx_t *ctx, int msg_id)
{
    session_ui_language_t lang = session_ui_language_current(ctx);
    if (lang < 0 || lang >= SESSION_UI_LANGUAGE_COUNT) {
        lang = SESSION_UI_LANGUAGE_EN;
    }

    switch (msg_id) {
        case DOORGAME_MSG_AVAILABLE:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "사용 가능한 DOOR 게임:";
                case SESSION_UI_LANGUAGE_JP: return "利用可能なDOORゲーム:";
                case SESSION_UI_LANGUAGE_ZH: return "可用的DOOR游戏：";
                case SESSION_UI_LANGUAGE_RU: return "Доступные DOOR игры:";
                case SESSION_UI_LANGUAGE_DE: return "Verfügbare DOOR-Spiele:";
                case SESSION_UI_LANGUAGE_FR: return "Jeux DOOR disponibles :";
                default: return "Available DOOR games:";
            }
        case DOORGAME_MSG_LAUNCH_HINT:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "`/bbs door <name>`으로 실행. 실행 중인 게임은 Ctrl-]로 종료.";
                case SESSION_UI_LANGUAGE_JP: return "`/bbs door <name>`で起動。実行中のゲームはCtrl-]で終了。";
                case SESSION_UI_LANGUAGE_ZH: return "使用 `/bbs door <name>` 启动。按 Ctrl-] 退出正在运行的游戏。";
                case SESSION_UI_LANGUAGE_RU: return "Запуск: `/bbs door <name>`. Выход: Ctrl-].";
                case SESSION_UI_LANGUAGE_DE: return "Starten mit `/bbs door <name>`. Ctrl-] beendet das Spiel.";
                case SESSION_UI_LANGUAGE_FR: return "Lancez avec `/bbs door <name>`. Ctrl-] pour quitter.";
                default: return "Launch with `/bbs door <name>`. Press Ctrl-] to exit a running game.";
            }
        case DOORGAME_MSG_NO_DOORS:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "설정된 DOOR 게임이 없습니다. 관리자: CHATTER_DOOR_1=name:dosbox_conf[:desc] (및 _2, _3 ...) 형식으로 등록하세요.";
                case SESSION_UI_LANGUAGE_JP: return "DOORゲームが設定されていません。管理者: CHATTER_DOOR_1=name:dosbox_conf[:desc] で登録してください。";
                case SESSION_UI_LANGUAGE_ZH: return "未配置 DOOR 游戏。管理员请使用 CHATTER_DOOR_1=name:dosbox_conf[:desc] 注册。";
                case SESSION_UI_LANGUAGE_RU: return "DOOR игры не настроены. Оператор: установите CHATTER_DOOR_1=name:dosbox_conf[:desc].";
                case SESSION_UI_LANGUAGE_DE: return "Keine DOOR-Spiele konfiguriert. Operator: CHATTER_DOOR_1=name:dosbox_conf[:desc] setzen.";
                case SESSION_UI_LANGUAGE_FR: return "Aucun jeu DOOR configuré. Opérateur : définissez CHATTER_DOOR_1=name:dosbox_conf[:desc].";
                default: return "No DOOR games configured. Operators: set CHATTER_DOOR_1=name:dosbox_conf[:desc] (and _2, _3 ...) to register doors.";
            }
        case DOORGAME_MSG_LAUNCHING:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] '%s' 실행 중 (Ctrl-]로 종료, 최대 %ds).";
                case SESSION_UI_LANGUAGE_JP: return "[door] '%s' を起動中 (Ctrl-] で終了、最大 %ds)。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] 正在启动 '%s'（按 Ctrl-] 退出，最多 %d 秒）。";
                case SESSION_UI_LANGUAGE_RU: return "[door] запуск '%s' (Ctrl-] для выхода, макс. %dс).";
                case SESSION_UI_LANGUAGE_DE: return "[door] starte '%s' (Ctrl-] zum Beenden, max. %ds).";
                case SESSION_UI_LANGUAGE_FR: return "[door] lancement de '%s' (Ctrl-] pour quitter, max. %ds).";
                default: return "[door] launching '%s' (Ctrl-] to exit, %ds max).";
            }
        case DOORGAME_MSG_WAITING:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] DOSBox 연결 대기 중...";
                case SESSION_UI_LANGUAGE_JP: return "[door] DOSBox の接続を待っています...";
                case SESSION_UI_LANGUAGE_ZH: return "[door] 等待 DOSBox 连接...";
                case SESSION_UI_LANGUAGE_RU: return "[door] ожидание подключения DOSBox...";
                case SESSION_UI_LANGUAGE_DE: return "[door] warte auf DOSBox-Verbindung...";
                case SESSION_UI_LANGUAGE_FR: return "[door] attente de la connexion DOSBox...";
                default: return "[door] waiting for DOSBox to connect...";
            }
        case DOORGAME_MSG_CONNECTED:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] 연결됨. 종료하려면 Ctrl-]를 누르세요.";
                case SESSION_UI_LANGUAGE_JP: return "[door] 接속完了。Ctrl-] で終了します。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] 已连接。按 Ctrl-] 退出。";
                case SESSION_UI_LANGUAGE_RU: return "[door] подключено. Ctrl-] для выхода.";
                case SESSION_UI_LANGUAGE_DE: return "[door] verbunden. Ctrl-] zum Beenden.";
                case SESSION_UI_LANGUAGE_FR: return "[door] connecté. Ctrl-] pour quitter.";
                default: return "[door] connected. Press Ctrl-] to exit.";
            }
        case DOORGAME_MSG_TIMEOUT:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] DOSBox가 시간 내에 연결되지 않았습니다. dosbox가 설치되어 있고 설정이 유효한지 확인하세요.";
                case SESSION_UI_LANGUAGE_JP: return "[door] DOSBox が時間内に接続しませんでした。dosbox がインストールされ設定が有効か確認してください。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] DOSBox 未在时限内连接。请确认 dosbox 已安装且配置有效。";
                case SESSION_UI_LANGUAGE_RU: return "[door] DOSBox не подключился вовремя. Проверьте установку и конфигурацию.";
                case SESSION_UI_LANGUAGE_DE: return "[door] DOSBox hat sich nicht rechtzeitig verbunden. Überprüfen Sie Installation und Konfiguration.";
                case SESSION_UI_LANGUAGE_FR: return "[door] DOSBox ne s'est pas connecté à temps. Vérifiez l'installation et la configuration.";
                default: return "[door] DOSBox did not connect in time. Verify dosbox is installed and the configuration is valid.";
            }
        case DOORGAME_MSG_FAILED_LAUNCH:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] dosbox 실행 실패. 설정 경로가 존재하고 dosbox가 PATH에 있는지 확인하세요.";
                case SESSION_UI_LANGUAGE_JP: return "[door] dosbox の起動に失敗しました。設定パスと PATH を確認してください。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] 无法启动 dosbox。请确认配置路径存在且 dosbox 在 PATH 中。";
                case SESSION_UI_LANGUAGE_RU: return "[door] не удалось запустить dosbox. Проверьте путь и наличие в PATH.";
                case SESSION_UI_LANGUAGE_DE: return "[door] dosbox konnte nicht gestartet werden. Prüfen Sie den Pfad und PATH.";
                case SESSION_UI_LANGUAGE_FR: return "[door] échec du lancement de dosbox. Vérifiez le chemin et la variable PATH.";
                default: return "[door] failed to launch dosbox. Verify the conf path exists and dosbox is on PATH.";
            }
        case DOORGAME_MSG_SESSION_ENDED:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] 세션 종료.";
                case SESSION_UI_LANGUAGE_JP: return "[door] セッション終了。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] 会话已结束。";
                case SESSION_UI_LANGUAGE_RU: return "[door] сессия завершена.";
                case SESSION_UI_LANGUAGE_DE: return "[door] Sitzung beendet.";
                case SESSION_UI_LANGUAGE_FR: return "[door] session terminée.";
                default: return "[door] session ended.";
            }
        case DOORGAME_MSG_ENDED_IMMEDIATELY:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] 세션이 즉시 종료되었습니다. 'dosbox'가 설치되어 있고 게임 파일이 설정된 경로에 있는지 확인하세요.";
                case SESSION_UI_LANGUAGE_JP: return "[door] セッションがすぐに終了しました。'dosbox' がインストールされゲームファイルが存在するか確認してください。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] 会话立即结束。请确认 dosbox 已安装且游戏文件位于配置路径。";
                case SESSION_UI_LANGUAGE_RU: return "[door] сессия завершилась сразу. Проверьте установку dosbox и наличие файлов.";
                case SESSION_UI_LANGUAGE_DE: return "[door] Sitzung sofort beendet. Prüfen Sie, ob dosbox installiert und die Spieldateien vorhanden sind.";
                case SESSION_UI_LANGUAGE_FR: return "[door] session terminée immédiatement. Vérifiez l'installation de dosbox et les fichiers de jeu.";
                default: return "[door] session ended immediately. Verify that 'dosbox' is installed and the conf/game files exist at the configured path.";
            }
        case DOORGAME_MSG_ESCAPE:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] 이탈 시퀀스 감지, 게임 종료.";
                case SESSION_UI_LANGUAGE_JP: return "[door] エスケープシーケンスを検出、ゲームを終了します。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] 检测到退出序列，正在结束游戏。";
                case SESSION_UI_LANGUAGE_RU: return "[door] обнаружена escape-последовательность, завершение игры.";
                case SESSION_UI_LANGUAGE_DE: return "[door] Escape-Sequenz erkannt, Spiel wird beendet.";
                case SESSION_UI_LANGUAGE_FR: return "[door] séquence d'échappement détectée, fin du jeu.";
                default: return "[door] escape sequence detected, ending door game.";
            }
        case DOORGAME_MSG_MAX_RUNTIME:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] 최대 실행 시간 도달, 게임 종료.";
                case SESSION_UI_LANGUAGE_JP: return "[door] 最大実行時間に達しました、ゲームを終了します。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] 已达到最大运行时间，正在结束游戏。";
                case SESSION_UI_LANGUAGE_RU: return "[door] достигнут максимальный runtime, завершение игры.";
                case SESSION_UI_LANGUAGE_DE: return "[door] maximale Laufzeit erreicht, Spiel wird beendet.";
                case SESSION_UI_LANGUAGE_FR: return "[door] durée maximale atteinte, fin du jeu.";
                default: return "[door] maximum runtime reached, terminating door game.";
            }
        case DOORGAME_MSG_TOO_MANY:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] 활성 door 세션이 너무 많습니다. 나중에 다시 시도하세요.";
                case SESSION_UI_LANGUAGE_JP: return "[door] アクティブな door セッションが多すぎます。後でもう一度お試しください。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] 活跃的 door 会话过多，请稍后再试。";
                case SESSION_UI_LANGUAGE_RU: return "[door] слишком много активных сессий. Попробуйте позже.";
                case SESSION_UI_LANGUAGE_DE: return "[door] zu viele aktive Sitzungen. Versuchen Sie es später erneut.";
                case SESSION_UI_LANGUAGE_FR: return "[door] trop de sessions actives. Réessayez plus tard.";
                default: return "[door] too many active door sessions. Try again later.";
            }
        case DOORGAME_MSG_ONLY_OPS_LOCK:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "관리자만 door 게임을 잠그거나 해제할 수 있습니다.";
                case SESSION_UI_LANGUAGE_JP: return "オペレータのみが door ゲームのロック/解除ができます。";
                case SESSION_UI_LANGUAGE_ZH: return "只有管理员可以锁定或解锁 door 游戏。";
                case SESSION_UI_LANGUAGE_RU: return "Только операторы могут блокировать/разблокировать DOOR игры.";
                case SESSION_UI_LANGUAGE_DE: return "Nur Operatoren können DOOR-Spiele sperren/entsperren.";
                case SESSION_UI_LANGUAGE_FR: return "Seuls les opérateurs peuvent verrouiller/déverrouiller les jeux DOOR.";
                default: return "Only operators may lock or unlock door games.";
            }
        case DOORGAME_MSG_GAME_LOCKED:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] 이 게임은 현재 관리자에 의해 잠겨 있습니다.";
                case SESSION_UI_LANGUAGE_JP: return "[door] このゲームは現在オペレータによってロックされています。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] 此游戏当前已被管理员锁定。";
                case SESSION_UI_LANGUAGE_RU: return "[door] эта игра заблокирована оператором.";
                case SESSION_UI_LANGUAGE_DE: return "[door] dieses Spiel ist derzeit vom Operator gesperrt.";
                case SESSION_UI_LANGUAGE_FR: return "[door] ce jeu est actuellement verrouillé par l'opérateur.";
                default: return "[door] This game is currently locked by the operator.";
            }
        case DOORGAME_MSG_SETGAMELOCK_USAGE:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "사용법: /bbs setgamelock <game_name>";
                case SESSION_UI_LANGUAGE_JP: return "使い方: /bbs setgamelock <game_name>";
                case SESSION_UI_LANGUAGE_ZH: return "用法: /bbs setgamelock <game_name>";
                case SESSION_UI_LANGUAGE_RU: return "Использование: /bbs setgamelock <game_name>";
                case SESSION_UI_LANGUAGE_DE: return "Nutzung: /bbs setgamelock <game_name>";
                case SESSION_UI_LANGUAGE_FR: return "Utilisation : /bbs setgamelock <game_name>";
                default: return "Usage: /bbs setgamelock <game_name>";
            }
        case DOORGAME_MSG_NOW_LOCKED:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] '%s'이(가) 잠금 상태로 변경되었습니다.";
                case SESSION_UI_LANGUAGE_JP: return "[door] '%s' をロックしました。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] '%s' 已锁定。";
                case SESSION_UI_LANGUAGE_RU: return "[door] '%s' заблокирована.";
                case SESSION_UI_LANGUAGE_DE: return "[door] '%s' ist jetzt GESPERRT.";
                case SESSION_UI_LANGUAGE_FR: return "[door] '%s' est maintenant VERROUILLÉ.";
                default: return "[door] '%s' is now LOCKED.";
            }
        case DOORGAME_MSG_NOW_UNLOCKED:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] '%s'의 잠금이 해제되었습니다.";
                case SESSION_UI_LANGUAGE_JP: return "[door] '%s' のロックを解除しました。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] '%s' 已解锁。";
                case SESSION_UI_LANGUAGE_RU: return "[door] '%s' разблокирована.";
                case SESSION_UI_LANGUAGE_DE: return "[door] '%s' ist jetzt ENTSPERRT.";
                case SESSION_UI_LANGUAGE_FR: return "[door] '%s' est maintenant DÉVERROUILLÉ.";
                default: return "[door] '%s' is now UNLOCKED.";
            }
        case DOORGAME_MSG_LIST_LOCKED:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return " [잠김]";
                case SESSION_UI_LANGUAGE_JP: return " [ロック]";
                case SESSION_UI_LANGUAGE_ZH: return " [已锁定]";
                case SESSION_UI_LANGUAGE_RU: return " [ЗАБЛОКИРОВАНО]";
                case SESSION_UI_LANGUAGE_DE: return " [GESPERRT]";
                case SESSION_UI_LANGUAGE_FR: return " [VERROUILLÉ]";
                default: return " [LOCKED]";
            }
        default:
            return "";
    }
}

/* ------------------------------------------------------------------------- */
/* Session callbacks                                                         */
/* ------------------------------------------------------------------------- */
static int adapter_read_poll(doorgame_session_t *s, char *buf, size_t len,
                             int timeout_ms)
{
    session_ctx_t *ctx = (session_ctx_t *)s;
    return session_channel_read_poll(ctx, buf, len, timeout_ms);
}

static void adapter_write(doorgame_session_t *s, const void *data, size_t len)
{
    session_ctx_t *ctx = (session_ctx_t *)s;
    session_channel_write(ctx, data, len);
}

static bool adapter_write_all(doorgame_session_t *s, const void *data,
                              size_t len)
{
    session_ctx_t *ctx = (session_ctx_t *)s;
    return session_channel_write_all(ctx, data, len);
}

static void adapter_send_system_line(doorgame_session_t *s, const char *msg)
{
    session_ctx_t *ctx = (session_ctx_t *)s;
    session_send_system_line(ctx, msg);
}

static void adapter_set_buffering(doorgame_session_t *s, bool enabled)
{
    session_ctx_t *ctx = (session_ctx_t *)s;
    ctx->output_buffering_enabled = enabled;
}

static bool adapter_get_buffering(doorgame_session_t *s)
{
    session_ctx_t *ctx = (session_ctx_t *)s;
    return ctx->output_buffering_enabled;
}

static const char *adapter_localized(doorgame_session_t *s, int msg_id)
{
    session_ctx_t *ctx = (session_ctx_t *)s;
    return door_localized(ctx, msg_id);
}

static bool adapter_is_operator(doorgame_session_t *s)
{
    session_ctx_t *ctx = (session_ctx_t *)s;
    return ctx->user.is_operator;
}

static bool adapter_is_lan_operator(doorgame_session_t *s)
{
    session_ctx_t *ctx = (session_ctx_t *)s;
    return ctx->user.is_lan_operator;
}

static void *adapter_gc_malloc(size_t sz)
{
    return sshc_gc_malloc(sz);
}

static void adapter_gc_free(void *p)
{
    sshc_gc_free(p);
}

static void *adapter_gc_realloc(void *p, size_t sz)
{
    return sshc_gc_realloc(p, sz);
}

/* ------------------------------------------------------------------------- */
/* Host callbacks                                                            */
/* ------------------------------------------------------------------------- */
static void adapter_inc_active(doorgame_host_t *h)
{
    host_t *host = (host_t *)h;
    ++host->active_door_sessions;
}

static void adapter_dec_active(doorgame_host_t *h)
{
    host_t *host = (host_t *)h;
    if (host->active_door_sessions > 0U) {
        --host->active_door_sessions;
    }
}

static bool adapter_is_shutting_down(doorgame_host_t *h)
{
    host_t *host = (host_t *)h;
    return host != nullptr && host->shutdown_flag != nullptr &&
           *host->shutdown_flag != 0;
}

static void adapter_save_locks(doorgame_host_t *h)
{
    host_t *host = (host_t *)h;
    host_door_games_save_locked(host);
}

/* ------------------------------------------------------------------------- */
/* Session ops table (read-only, safe to share across threads)               */
/* ------------------------------------------------------------------------- */
static doorgame_session_ops_t g_door_session_ops = {
    .read_poll = adapter_read_poll,
    .write = adapter_write,
    .write_all = adapter_write_all,
    .send_system_line = adapter_send_system_line,
    .set_buffering = adapter_set_buffering,
    .get_buffering = adapter_get_buffering,
    .localized = adapter_localized,
    .is_operator = adapter_is_operator,
    .is_lan_operator = adapter_is_lan_operator,
    .gc_malloc = adapter_gc_malloc,
    .gc_free = adapter_gc_free,
    .gc_realloc = adapter_gc_realloc,
};

/* ------------------------------------------------------------------------- */
/* Adapter entry points (called from bbs_handler.c)                          */
/* ------------------------------------------------------------------------- */
void session_bbs_door_run(session_ctx_t *ctx, const char *name)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }
    host_t *host = ctx->owner;

    doorgame_host_ops_t hops = {
        .entries = (doorgame_entry_t *)host->door_games,
        .entry_count = host->door_game_count,
        .max_sessions = host->max_door_sessions,
        .active_sessions = host->active_door_sessions,
        .inc_active = adapter_inc_active,
        .dec_active = adapter_dec_active,
        .is_shutting_down = adapter_is_shutting_down,
        .save_locks = adapter_save_locks,
    };

    if (name == nullptr || name[0] == '\0') {
        doorgame_list((doorgame_session_t *)ctx, (doorgame_host_t *)host,
                      &g_door_session_ops, &hops);
        return;
    }

    const door_game_entry_t *entry = nullptr;
    for (size_t i = 0U; i < host->door_game_count; ++i) {
        if (host->door_games[i].in_use &&
            strcasecmp(host->door_games[i].name, name) == 0) {
            entry = &host->door_games[i];
            break;
        }
    }

    if (entry == nullptr) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message),
                 "Unknown DOOR game '%s'. Try `/bbs door` for the list.",
                 name);
        session_send_system_line(ctx, message);
        return;
    }

    doorgame_run((doorgame_session_t *)ctx, (doorgame_host_t *)host,
                 (const doorgame_entry_t *)entry,
                 &g_door_session_ops, &hops);
}

void session_bbs_setgamelock(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }
    host_t *host = ctx->owner;

    bool is_op = ctx->user.is_operator || ctx->user.is_lan_operator;

    doorgame_host_ops_t hops = {
        .entries = (doorgame_entry_t *)host->door_games,
        .entry_count = host->door_game_count,
        .max_sessions = host->max_door_sessions,
        .active_sessions = host->active_door_sessions,
        .inc_active = adapter_inc_active,
        .dec_active = adapter_dec_active,
        .is_shutting_down = adapter_is_shutting_down,
        .save_locks = adapter_save_locks,
    };

    doorgame_toggle_lock((doorgame_session_t *)ctx, (doorgame_host_t *)host,
                         arguments, is_op,
                         &g_door_session_ops, &hops);
}

/**
 * @file host_bbs_door.c
 * @desc BBS DOOR GAME runner. Spawns a configured dosbox session and proxies
 *       stdin/stdout between the chat session and the child PTY. Doors are
 *       declared at startup via env vars `CHATTER_DOOR_<N>=name:conf[:desc]`.
 *
 *       Open to all authenticated users. The launch path uses execvp directly with a
 *       fixed argv — there is no shell layer and no user-controlled string
 *       reaches a shell, so users cannot inject extra arguments by naming a
 *       door creatively.
 */

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <pty.h>
#include <iconv.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <libgen.h>

#ifdef SSH_CHATTER_HAVE_UCHARDET
#include <uchardet.h>
#endif

/* Forward declarations of helpers from sibling translation units in the
 * same aggregated TU. */
static int session_channel_read_poll(session_ctx_t *ctx, char *buffer,
                                     size_t length, int timeout_ms);
void session_channel_write(session_ctx_t *ctx, const void *data,
                           size_t length);
extern bool session_channel_write_all(session_ctx_t *ctx, const void *data,
                                      size_t length);
void session_send_system_line(session_ctx_t *ctx, const char *message);

/* ---- Door game localized messages ---- */
enum {
    DOOR_MSG_AVAILABLE = 0,
    DOOR_MSG_LAUNCH_HINT,
    DOOR_MSG_NO_DOORS,
    DOOR_MSG_LAUNCHING,
    DOOR_MSG_WAITING,
    DOOR_MSG_CONNECTED,
    DOOR_MSG_TIMEOUT,
    DOOR_MSG_FAILED_LAUNCH,
    DOOR_MSG_SESSION_ENDED,
    DOOR_MSG_ENDED_IMMEDIATELY,
    DOOR_MSG_ESCAPE,
    DOOR_MSG_MAX_RUNTIME,
    DOOR_MSG_TOO_MANY,
    DOOR_MSG_ONLY_OPS_LOCK,
    DOOR_MSG_GAME_LOCKED,
    DOOR_MSG_SETGAMELOCK_USAGE,
    DOOR_MSG_NOW_LOCKED,
    DOOR_MSG_NOW_UNLOCKED,
    DOOR_MSG_LIST_LOCKED,
    DOOR_MSG_COUNT
};

static const char *door_localized(session_ctx_t *ctx, int msg_id)
{
    session_ui_language_t lang = session_ui_language_current(ctx);
    if (lang < 0 || lang >= SESSION_UI_LANGUAGE_COUNT) {
        lang = SESSION_UI_LANGUAGE_EN;
    }

    switch (msg_id) {
        case DOOR_MSG_AVAILABLE:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "사용 가능한 DOOR 게임:";
                case SESSION_UI_LANGUAGE_JP: return "利用可能なDOORゲーム:";
                case SESSION_UI_LANGUAGE_ZH: return "可用的DOOR游戏：";
                case SESSION_UI_LANGUAGE_RU: return "Доступные DOOR игры:";
                case SESSION_UI_LANGUAGE_DE: return "Verfügbare DOOR-Spiele:";
                case SESSION_UI_LANGUAGE_FR: return "Jeux DOOR disponibles :";
                default: return "Available DOOR games:";
            }
        case DOOR_MSG_LAUNCH_HINT:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "`/bbs door <name>`으로 실행. 실행 중인 게임은 Ctrl-]로 종료.";
                case SESSION_UI_LANGUAGE_JP: return "`/bbs door <name>`で起動。実行中のゲームはCtrl-]で終了。";
                case SESSION_UI_LANGUAGE_ZH: return "使用 `/bbs door <name>` 启动。按 Ctrl-] 退出正在运行的游戏。";
                case SESSION_UI_LANGUAGE_RU: return "Запуск: `/bbs door <name>`. Выход: Ctrl-].";
                case SESSION_UI_LANGUAGE_DE: return "Starten mit `/bbs door <name>`. Ctrl-] beendet das Spiel.";
                case SESSION_UI_LANGUAGE_FR: return "Lancez avec `/bbs door <name>`. Ctrl-] pour quitter.";
                default: return "Launch with `/bbs door <name>`. Press Ctrl-] to exit a running game.";
            }
        case DOOR_MSG_NO_DOORS:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "설정된 DOOR 게임이 없습니다. 관리자: CHATTER_DOOR_1=name:dosbox_conf[:desc] (및 _2, _3 ...) 형식으로 등록하세요.";
                case SESSION_UI_LANGUAGE_JP: return "DOORゲームが設定されていません。管理者: CHATTER_DOOR_1=name:dosbox_conf[:desc] で登録してください。";
                case SESSION_UI_LANGUAGE_ZH: return "未配置 DOOR 游戏。管理员请使用 CHATTER_DOOR_1=name:dosbox_conf[:desc] 注册。";
                case SESSION_UI_LANGUAGE_RU: return "DOOR игры не настроены. Оператор: установите CHATTER_DOOR_1=name:dosbox_conf[:desc].";
                case SESSION_UI_LANGUAGE_DE: return "Keine DOOR-Spiele konfiguriert. Operator: CHATTER_DOOR_1=name:dosbox_conf[:desc] setzen.";
                case SESSION_UI_LANGUAGE_FR: return "Aucun jeu DOOR configuré. Opérateur : définissez CHATTER_DOOR_1=name:dosbox_conf[:desc].";
                default: return "No DOOR games configured. Operators: set CHATTER_DOOR_1=name:dosbox_conf[:desc] (and _2, _3 ...) to register doors.";
            }
        case DOOR_MSG_LAUNCHING:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] '%s' 실행 중 (Ctrl-]로 종료, 최대 %ds).";
                case SESSION_UI_LANGUAGE_JP: return "[door] '%s' を起動中 (Ctrl-] で終了、最大 %ds)。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] 正在启动 '%s'（按 Ctrl-] 退出，最多 %d 秒）。";
                case SESSION_UI_LANGUAGE_RU: return "[door] запуск '%s' (Ctrl-] для выхода, макс. %dс).";
                case SESSION_UI_LANGUAGE_DE: return "[door] starte '%s' (Ctrl-] zum Beenden, max. %ds).";
                case SESSION_UI_LANGUAGE_FR: return "[door] lancement de '%s' (Ctrl-] pour quitter, max. %ds).";
                default: return "[door] launching '%s' (Ctrl-] to exit, %ds max).";
            }
        case DOOR_MSG_WAITING:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] DOSBox 연결 대기 중...";
                case SESSION_UI_LANGUAGE_JP: return "[door] DOSBox の接続を待っています...";
                case SESSION_UI_LANGUAGE_ZH: return "[door] 等待 DOSBox 连接...";
                case SESSION_UI_LANGUAGE_RU: return "[door] ожидание подключения DOSBox...";
                case SESSION_UI_LANGUAGE_DE: return "[door] warte auf DOSBox-Verbindung...";
                case SESSION_UI_LANGUAGE_FR: return "[door] attente de la connexion DOSBox...";
                default: return "[door] waiting for DOSBox to connect...";
            }
        case DOOR_MSG_CONNECTED:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] 연결됨. 종료하려면 Ctrl-]를 누르세요.";
                case SESSION_UI_LANGUAGE_JP: return "[door] 接続完了。Ctrl-] で終了します。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] 已连接。按 Ctrl-] 退出。";
                case SESSION_UI_LANGUAGE_RU: return "[door] подключено. Ctrl-] для выхода.";
                case SESSION_UI_LANGUAGE_DE: return "[door] verbunden. Ctrl-] zum Beenden.";
                case SESSION_UI_LANGUAGE_FR: return "[door] connecté. Ctrl-] pour quitter.";
                default: return "[door] connected. Press Ctrl-] to exit.";
            }
        case DOOR_MSG_TIMEOUT:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] DOSBox가 시간 내에 연결되지 않았습니다. dosbox가 설치되어 있고 설정이 유효한지 확인하세요.";
                case SESSION_UI_LANGUAGE_JP: return "[door] DOSBox が時間内に接続しませんでした。dosbox がインストールされ設定が有効か確認してください。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] DOSBox 未在时限内连接。请确认 dosbox 已安装且配置有效。";
                case SESSION_UI_LANGUAGE_RU: return "[door] DOSBox не подключился вовремя. Проверьте установку и конфигурацию.";
                case SESSION_UI_LANGUAGE_DE: return "[door] DOSBox hat sich nicht rechtzeitig verbunden. Überprüfen Sie Installation und Konfiguration.";
                case SESSION_UI_LANGUAGE_FR: return "[door] DOSBox ne s'est pas connecté à temps. Vérifiez l'installation et la configuration.";
                default: return "[door] DOSBox did not connect in time. Verify dosbox is installed and the configuration is valid.";
            }
        case DOOR_MSG_FAILED_LAUNCH:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] dosbox 실행 실패. 설정 경로가 존재하고 dosbox가 PATH에 있는지 확인하세요.";
                case SESSION_UI_LANGUAGE_JP: return "[door] dosbox の起動に失敗しました。設定パスと PATH を確認してください。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] 无法启动 dosbox。请确认配置路径存在且 dosbox 在 PATH 中。";
                case SESSION_UI_LANGUAGE_RU: return "[door] не удалось запустить dosbox. Проверьте путь и наличие в PATH.";
                case SESSION_UI_LANGUAGE_DE: return "[door] dosbox konnte nicht gestartet werden. Prüfen Sie den Pfad und PATH.";
                case SESSION_UI_LANGUAGE_FR: return "[door] échec du lancement de dosbox. Vérifiez le chemin et la variable PATH.";
                default: return "[door] failed to launch dosbox. Verify the conf path exists and dosbox is on PATH.";
            }
        case DOOR_MSG_SESSION_ENDED:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] 세션 종료.";
                case SESSION_UI_LANGUAGE_JP: return "[door] セッション終了。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] 会话已结束。";
                case SESSION_UI_LANGUAGE_RU: return "[door] сессия завершена.";
                case SESSION_UI_LANGUAGE_DE: return "[door] Sitzung beendet.";
                case SESSION_UI_LANGUAGE_FR: return "[door] session terminée.";
                default: return "[door] session ended.";
            }
        case DOOR_MSG_ENDED_IMMEDIATELY:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] 세션이 즉시 종료되었습니다. 'dosbox'가 설치되어 있고 게임 파일이 설정된 경로에 있는지 확인하세요.";
                case SESSION_UI_LANGUAGE_JP: return "[door] セッションがすぐに終了しました。'dosbox' がインストールされゲームファイルが存在するか確認してください。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] 会话立即结束。请确认 dosbox 已安装且游戏文件位于配置路径。";
                case SESSION_UI_LANGUAGE_RU: return "[door] сессия завершилась сразу. Проверьте установку dosbox и наличие файлов.";
                case SESSION_UI_LANGUAGE_DE: return "[door] Sitzung sofort beendet. Prüfen Sie, ob dosbox installiert und die Spieldateien vorhanden sind.";
                case SESSION_UI_LANGUAGE_FR: return "[door] session terminée immédiatement. Vérifiez l'installation de dosbox et les fichiers de jeu.";
                default: return "[door] session ended immediately. Verify that 'dosbox' is installed and the conf/game files exist at the configured path.";
            }
        case DOOR_MSG_ESCAPE:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] 이탈 시퀀스 감지, 게임 종료.";
                case SESSION_UI_LANGUAGE_JP: return "[door] エスケープシーケンスを検出、ゲームを終了します。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] 检测到退出序列，正在结束游戏。";
                case SESSION_UI_LANGUAGE_RU: return "[door] обнаружена escape-последовательность, завершение игры.";
                case SESSION_UI_LANGUAGE_DE: return "[door] Escape-Sequenz erkannt, Spiel wird beendet.";
                case SESSION_UI_LANGUAGE_FR: return "[door] séquence d'échappement détectée, fin du jeu.";
                default: return "[door] escape sequence detected, ending door game.";
            }
        case DOOR_MSG_MAX_RUNTIME:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] 최대 실행 시간 도달, 게임 종료.";
                case SESSION_UI_LANGUAGE_JP: return "[door] 最大実行時間に達しました、ゲームを終了します。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] 已达到最大运行时间，正在结束游戏。";
                case SESSION_UI_LANGUAGE_RU: return "[door] достигнут максимальный runtime, завершение игры.";
                case SESSION_UI_LANGUAGE_DE: return "[door] maximale Laufzeit erreicht, Spiel wird beendet.";
                case SESSION_UI_LANGUAGE_FR: return "[door] durée maximale atteinte, fin du jeu.";
                default: return "[door] maximum runtime reached, terminating door game.";
            }
        case DOOR_MSG_TOO_MANY:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] 활성 door 세션이 너무 많습니다. 나중에 다시 시도하세요.";
                case SESSION_UI_LANGUAGE_JP: return "[door] アクティブな door セッションが多すぎます。後でもう一度お試しください。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] 活跃的 door 会话过多，请稍后再试。";
                case SESSION_UI_LANGUAGE_RU: return "[door] слишком много активных сессий. Попробуйте позже.";
                case SESSION_UI_LANGUAGE_DE: return "[door] zu viele aktive Sitzungen. Versuchen Sie es später erneut.";
                case SESSION_UI_LANGUAGE_FR: return "[door] trop de sessions actives. Réessayez plus tard.";
                default: return "[door] too many active door sessions. Try again later.";
            }
        case DOOR_MSG_ONLY_OPS_LOCK:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "관리자만 door 게임을 잠그거나 해제할 수 있습니다.";
                case SESSION_UI_LANGUAGE_JP: return "オペレータのみが door ゲームのロック/解除ができます。";
                case SESSION_UI_LANGUAGE_ZH: return "只有管理员可以锁定或解锁 door 游戏。";
                case SESSION_UI_LANGUAGE_RU: return "Только операторы могут блокировать/разблокировать DOOR игры.";
                case SESSION_UI_LANGUAGE_DE: return "Nur Operatoren können DOOR-Spiele sperren/entsperren.";
                case SESSION_UI_LANGUAGE_FR: return "Seuls les opérateurs peuvent verrouiller/déverrouiller les jeux DOOR.";
                default: return "Only operators may lock or unlock door games.";
            }
        case DOOR_MSG_GAME_LOCKED:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] 이 게임은 현재 관리자에 의해 잠겨 있습니다.";
                case SESSION_UI_LANGUAGE_JP: return "[door] このゲームは現在オペレータによってロックされています。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] 此游戏当前已被管理员锁定。";
                case SESSION_UI_LANGUAGE_RU: return "[door] эта игра заблокирована оператором.";
                case SESSION_UI_LANGUAGE_DE: return "[door] dieses Spiel ist derzeit vom Operator gesperrt.";
                case SESSION_UI_LANGUAGE_FR: return "[door] ce jeu est actuellement verrouillé par l'opérateur.";
                default: return "[door] This game is currently locked by the operator.";
            }
        case DOOR_MSG_SETGAMELOCK_USAGE:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "사용법: /bbs setgamelock <game_name>";
                case SESSION_UI_LANGUAGE_JP: return "使い方: /bbs setgamelock <game_name>";
                case SESSION_UI_LANGUAGE_ZH: return "用法: /bbs setgamelock <game_name>";
                case SESSION_UI_LANGUAGE_RU: return "Использование: /bbs setgamelock <game_name>";
                case SESSION_UI_LANGUAGE_DE: return "Nutzung: /bbs setgamelock <game_name>";
                case SESSION_UI_LANGUAGE_FR: return "Utilisation : /bbs setgamelock <game_name>";
                default: return "Usage: /bbs setgamelock <game_name>";
            }
        case DOOR_MSG_NOW_LOCKED:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] '%s'이(가) 잠금 상태로 변경되었습니다.";
                case SESSION_UI_LANGUAGE_JP: return "[door] '%s' をロックしました。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] '%s' 已锁定。";
                case SESSION_UI_LANGUAGE_RU: return "[door] '%s' заблокирована.";
                case SESSION_UI_LANGUAGE_DE: return "[door] '%s' ist jetzt GESPERRT.";
                case SESSION_UI_LANGUAGE_FR: return "[door] '%s' est maintenant VERROUILLÉ.";
                default: return "[door] '%s' is now LOCKED.";
            }
        case DOOR_MSG_NOW_UNLOCKED:
            switch (lang) {
                case SESSION_UI_LANGUAGE_KO: return "[door] '%s'의 잠금이 해제되었습니다.";
                case SESSION_UI_LANGUAGE_JP: return "[door] '%s' のロックを解除しました。";
                case SESSION_UI_LANGUAGE_ZH: return "[door] '%s' 已解锁。";
                case SESSION_UI_LANGUAGE_RU: return "[door] '%s' разблокирована.";
                case SESSION_UI_LANGUAGE_DE: return "[door] '%s' ist jetzt ENTSPERRT.";
                case SESSION_UI_LANGUAGE_FR: return "[door] '%s' est maintenant DÉVERROUILLÉ.";
                default: return "[door] '%s' is now UNLOCKED.";
            }
        case DOOR_MSG_LIST_LOCKED:
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

#define SSH_CHATTER_DOOR_IDLE_POLL_MS 100
#define SSH_CHATTER_DOOR_MAX_RUNTIME_SECONDS 3600
#define SSH_CHATTER_DOOR_BUFFER_SIZE 4096
#define SSH_CHATTER_DOOR_QUIT_BYTE 0x1D /* Ctrl-] — escape from door. */
#define SSH_CHATTER_DOOR_DETECT_BUFFER_BYTES 4096U

typedef struct host_door_runner {
    pid_t child_pid;
    int master_fd;
    time_t started_at;
    bool active;
    /* Encoding detection state for the dosbox stdout stream. We accumulate
     * up to SSH_CHATTER_DOOR_DETECT_BUFFER_BYTES bytes before deciding which
     * Korean encoding (CP949 vs JOHAB vs UTF-8 vs none) the child is using;
     * once a decision is reached, every subsequent chunk is converted to
     * UTF-8 before being written to the SSH session. */
    unsigned char detect_buffer[SSH_CHATTER_DOOR_DETECT_BUFFER_BYTES];
    size_t detect_length;
    bool encoding_decided;
    int detected_encoding; /* see DOOR_ENC_* below */
} host_door_runner_t;

enum {
    DOOR_ENC_UNKNOWN = 0,
    DOOR_ENC_PASSTHROUGH = 1,
    DOOR_ENC_UTF8 = 2,
    DOOR_ENC_CP949 = 3,
    DOOR_ENC_JOHAB = 4,
    DOOR_ENC_CP437 = 5,
};

/* ---- Encoding detection ----
 *
 * dosbox output is typically a mix of ANSI control bytes and either CP949,
 * JOHAB (older Korean DOS games), or already-UTF-8 (modern dosbox builds
 * with translation layers). We accumulate the first
 * SSH_CHATTER_DOOR_DETECT_BUFFER_BYTES of stdout and run three independent
 * tests:
 *
 * 1) UTF-8 well-formedness (every multi-byte sequence's continuation bytes
 *    are 10xxxxxx, no overlong forms, no surrogate pairs in payload).
 * 2) CP949 plausibility (lead byte in 0x81–0xFE, trail in
 *    0x41–0x5A, 0x61–0x7A, 0x81–0xFE — the standard EUC-KR + extended
 *    range used in CP949).
 * 3) JOHAB plausibility (lead byte in 0x84–0xD3, trail in
 *    0x41–0x7E or 0x81–0xFE; high-bit pattern distinct from CP949).
 *
 * The decision rule:
 *   - If the buffer is fully 7-bit ASCII, mark PASSTHROUGH.
 *   - Else if the UTF-8 test passes and at least one multi-byte sequence is
 *     decoded validly, mark UTF8.
 *   - Else if the CP949 test passes more high-byte transitions than the
 *     JOHAB test, mark CP949; else if JOHAB has more, mark JOHAB.
 *   - Otherwise mark PASSTHROUGH (we don't guess wildly).
 */

static bool door_enc_byte_is_utf8_continuation(unsigned char b)
{
    return (b & 0xC0U) == 0x80U;
}

static int door_enc_score_utf8(const unsigned char *data, size_t len,
                               bool *all_ascii_out)
{
    int score = 0;
    bool all_ascii = true;
    size_t idx = 0U;
    while (idx < len) {
        unsigned char ch = data[idx];
        if (ch < 0x80U) {
            ++idx;
            continue;
        }
        all_ascii = false;
        size_t need = 0U;
        if ((ch & 0xE0U) == 0xC0U) {
            need = 1U;
        } else if ((ch & 0xF0U) == 0xE0U) {
            need = 2U;
        } else if ((ch & 0xF8U) == 0xF0U) {
            need = 3U;
        } else {
            return -1;
        }
        if (idx + need >= len) {
            /* Truncated tail — let it slide; will check next round. */
            break;
        }
        for (size_t k = 1U; k <= need; ++k) {
            if (!door_enc_byte_is_utf8_continuation(data[idx + k])) {
                return -1;
            }
        }
        score += (int)(need + 1U);
        idx += need + 1U;
    }
    if (all_ascii_out != nullptr) {
        *all_ascii_out = all_ascii;
    }
    return score;
}

static int door_enc_score_cp949(const unsigned char *data, size_t len)
{
    int score = 0;
    size_t idx = 0U;
    while (idx + 1U < len) {
        unsigned char lead = data[idx];
        if (lead < 0x80U) {
            ++idx;
            continue;
        }
        if (lead >= 0x81U && lead <= 0xFEU) {
            unsigned char trail = data[idx + 1U];
            bool trail_ok =
                (trail >= 0x41U && trail <= 0x5AU) ||
                (trail >= 0x61U && trail <= 0x7AU) ||
                (trail >= 0x81U && trail <= 0xFEU);
            if (trail_ok) {
                score += 2;
                idx += 2U;
                continue;
            }
        }
        --score; /* high byte that doesn't form a valid CP949 pair */
        ++idx;
    }
    return score;
}

static int door_enc_score_johab(const unsigned char *data, size_t len)
{
    int score = 0;
    size_t idx = 0U;
    while (idx + 1U < len) {
        unsigned char lead = data[idx];
        if (lead < 0x80U) {
            ++idx;
            continue;
        }
        if (lead >= 0x84U && lead <= 0xD3U) {
            unsigned char trail = data[idx + 1U];
            bool trail_ok =
                (trail >= 0x41U && trail <= 0x7EU) ||
                (trail >= 0x81U && trail <= 0xFEU);
            if (trail_ok) {
                score += 2;
                idx += 2U;
                continue;
            }
        }
        --score;
        ++idx;
    }
    return score;
}

static int door_enc_decide(const unsigned char *data, size_t len)
{
    if (len == 0U) {
        return DOOR_ENC_UNKNOWN;
    }

    bool all_ascii = false;
    int utf8_score = door_enc_score_utf8(data, len, &all_ascii);
    if (utf8_score >= 0 && all_ascii) {
        return DOOR_ENC_PASSTHROUGH;
    }
    if (utf8_score > 0) {
        return DOOR_ENC_UTF8;
    }

#ifdef SSH_CHATTER_HAVE_UCHARDET
    uchardet_t ud = uchardet_new();
    if (ud != nullptr) {
        if (uchardet_handle_data(ud, (const char *)data, len) == 0) {
            uchardet_data_end(ud);
            const char *charset = uchardet_get_charset(ud);
            if (charset != nullptr && charset[0] != '\0') {
                int result = DOOR_ENC_UNKNOWN;
                if (strcasecmp(charset, "UTF-8") == 0) {
                    result = DOOR_ENC_UTF8;
                } else if (strncasecmp(charset, "EUC-KR", 6) == 0 ||
                           strncasecmp(charset, "CP949", 5) == 0 ||
                           strncasecmp(charset, "ISO-2022-KR", 11) == 0) {
                    result = DOOR_ENC_CP949;
                } else if (strncasecmp(charset, "JOHAB", 5) == 0) {
                    result = DOOR_ENC_JOHAB;
                } else if (strncasecmp(charset, "CP437", 5) == 0 ||
                           strncasecmp(charset, "IBM437", 6) == 0) {
                    result = DOOR_ENC_CP437;
                } else if (strncasecmp(charset, "ASCII", 5) == 0) {
                    result = DOOR_ENC_PASSTHROUGH;
                }
                if (result != DOOR_ENC_UNKNOWN) {
                    uchardet_delete(ud);
                    return result;
                }
            }
        }
        uchardet_delete(ud);
    }
#endif

    size_t high_byte_count = 0;
    for (size_t idx = 0; idx < len; ++idx) {
        if (data[idx] >= 0x80U) {
            high_byte_count++;
        }
    }

    int cp949_score = door_enc_score_cp949(data, len);
    int johab_score = door_enc_score_johab(data, len);

    if (cp949_score > 0 && (size_t)cp949_score > high_byte_count / 2 && cp949_score >= johab_score) {
        return DOOR_ENC_CP949;
    }
    if (johab_score > 0 && (size_t)johab_score > high_byte_count / 2 && johab_score > cp949_score) {
        return DOOR_ENC_JOHAB;
    }

    return DOOR_ENC_CP437;
}

static const char *door_enc_iconv_label(int encoding)
{
    switch (encoding) {
        case DOOR_ENC_CP949:
            return "CP949";
        case DOOR_ENC_JOHAB:
            return "JOHAB";
        case DOOR_ENC_UTF8:
            return "UTF-8";
        case DOOR_ENC_CP437:
            return "CP437";
        default:
            return nullptr;
    }
}

/* Convert `len` bytes of `src` from runner->detected_encoding into UTF-8 and
 * write to the SSH session via session_channel_write. Falls back to
 * passthrough on conversion failure. The function is best-effort: it never
 * returns an error to the caller, since the alternative is to drop the
 * door's output. */
static void door_emit_converted(session_ctx_t *ctx, host_door_runner_t *runner,
                                const void *src, size_t len)
{
    if (ctx == nullptr || runner == nullptr || src == nullptr || len == 0U) {
        return;
    }

    if (runner->detected_encoding == DOOR_ENC_PASSTHROUGH ||
        runner->detected_encoding == DOOR_ENC_UTF8 ||
        runner->detected_encoding == DOOR_ENC_UNKNOWN) {
        (void)session_channel_write_all(ctx, src, len);
        return;
    }

    const char *label = door_enc_iconv_label(runner->detected_encoding);
    if (label == nullptr) {
        (void)session_channel_write_all(ctx, src, len);
        return;
    }

    iconv_t cd = iconv_open("UTF-8", label);
    if (cd == (iconv_t)-1) {
        (void)session_channel_write_all(ctx, src, len);
        return;
    }

    /* Output buffer roughly 4× input — Korean glyphs occupy 3 bytes in
     * UTF-8 so 2× CP949 → 3× UTF-8 worst case; ANSI pass-through is 1×.
     * Round up generously. */
    size_t out_capacity = len * 4U + 64U;
    char *out_buffer = (char *)sshc_gc_malloc(out_capacity);
    if (out_buffer == nullptr) {
        iconv_close(cd);
        session_channel_write(ctx, src, len);
        return;
    }

    char *in_ptr = (char *)src; /* iconv signature requires non-const */
    size_t in_left = len;
    char *out_ptr = out_buffer;
    size_t out_left = out_capacity;

    while (in_left > 0U) {
        size_t rc = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
        if (rc == (size_t)-1) {
            if (errno == E2BIG) {
                /* grow output buffer */
                size_t produced = out_capacity - out_left;
                size_t new_capacity = out_capacity * 2U;
                char *grown = (char *)sshc_gc_realloc(out_buffer, new_capacity);
                if (grown == nullptr) {
                    break;
                }
                out_buffer = grown;
                out_ptr = out_buffer + produced;
                out_left = new_capacity - produced;
                out_capacity = new_capacity;
                continue;
            }
            if (errno == EILSEQ || errno == EINVAL) {
                /* Skip the offending byte, emit a replacement, continue. */
                if (out_left >= 3U) {
                    out_ptr[0] = '\xEF';
                    out_ptr[1] = '\xBF';
                    out_ptr[2] = '\xBD';
                    out_ptr += 3;
                    out_left -= 3U;
                }
                if (in_left > 0U) {
                    ++in_ptr;
                    --in_left;
                }
                continue;
            }
            break;
        }
    }

    size_t produced = out_capacity - out_left;
    if (produced > 0U) {
        (void)session_channel_write_all(ctx, out_buffer, produced);
    }
    sshc_gc_free(out_buffer);
    iconv_close(cd);
}

static void door_flush_detect_buffer(session_ctx_t *ctx,
                                     host_door_runner_t *runner)
{
    if (ctx == nullptr || runner == nullptr || runner->detect_length == 0U) {
        return;
    }

    runner->detected_encoding =
        door_enc_decide(runner->detect_buffer, runner->detect_length);
    runner->encoding_decided = true;
    door_emit_converted(ctx, runner, runner->detect_buffer,
                        runner->detect_length);
    runner->detect_length = 0U;
}

static const door_game_entry_t *host_door_lookup(const host_t *host,
                                                 const char *name)
{
    if (host == nullptr || name == nullptr || name[0] == '\0') {
        return nullptr;
    }
    for (size_t idx = 0U; idx < host->door_game_count; ++idx) {
        if (!host->door_games[idx].in_use) {
            continue;
        }
        if (strcasecmp(host->door_games[idx].name, name) == 0) {
            return &host->door_games[idx];
        }
    }
    return nullptr;
}

static void session_bbs_door_list(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }
    host_t *host = ctx->owner;
    if (host->door_game_count == 0U) {
        session_send_system_line(ctx, door_localized(ctx, DOOR_MSG_NO_DOORS));
        return;
    }

    session_send_system_line(ctx, door_localized(ctx, DOOR_MSG_AVAILABLE));
    char buffer[SSH_CHATTER_MESSAGE_LIMIT];
    for (size_t idx = 0U; idx < host->door_game_count; ++idx) {
        if (!host->door_games[idx].in_use) {
            continue;
        }
        const char *desc = host->door_games[idx].description;
        const char *locked_tag =
            host->door_games[idx].locked ? door_localized(ctx, DOOR_MSG_LIST_LOCKED) : "";
        if (desc[0] == '\0') {
            snprintf(buffer, sizeof(buffer), "  %s%s",
                     host->door_games[idx].name, locked_tag);
        } else {
            snprintf(buffer, sizeof(buffer), "  %s%s — %s",
                     host->door_games[idx].name, locked_tag, desc);
        }
        session_send_system_line(ctx, buffer);
    }
    session_send_system_line(ctx, door_localized(ctx, DOOR_MSG_LAUNCH_HINT));
}

static bool session_bbs_door_caller_authorised(const session_ctx_t *ctx)
{
    (void)ctx;
    /* Door games are open to all authenticated users. */
    return true;
}

static int session_bbs_door_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void session_bbs_door_terminate_child(host_door_runner_t *runner)
{
    if (runner == nullptr || !runner->active) {
        return;
    }
    if (runner->child_pid > 0) {
        kill(runner->child_pid, SIGTERM);
        for (int wait_idx = 0; wait_idx < 20; ++wait_idx) {
            int status = 0;
            pid_t reaped = waitpid(runner->child_pid, &status, WNOHANG);
            if (reaped == runner->child_pid || reaped < 0) {
                runner->child_pid = -1;
                break;
            }
            struct timespec naptime = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000L};
            nanosleep(&naptime, nullptr);
        }
        if (runner->child_pid > 0) {
            kill(runner->child_pid, SIGKILL);
            (void)waitpid(runner->child_pid, nullptr, 0);
            runner->child_pid = -1;
        }
    }
    if (runner->master_fd >= 0) {
        close(runner->master_fd);
        runner->master_fd = -1;
    }
    runner->active = false;
}

static bool session_bbs_door_spawn(const char *dosbox_conf,
                                   host_door_runner_t *runner)
{
    if (dosbox_conf == nullptr || dosbox_conf[0] == '\0' || runner == nullptr) {
        return false;
    }

    struct stat conf_stat;
    if (stat(dosbox_conf, &conf_stat) != 0 || !S_ISREG(conf_stat.st_mode)) {
        return false;
    }

    int master_fd = -1;
    int slave_fd = -1;
    if (openpty(&master_fd, &slave_fd, nullptr, nullptr, nullptr) != 0) {
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(master_fd);
        close(slave_fd);
        return false;
    }

    char conf_dir_copy[PATH_MAX];
    snprintf(conf_dir_copy, sizeof(conf_dir_copy), "%s", dosbox_conf);
    const char *game_dir = dirname(conf_dir_copy);

    int debug_fd = open("/tmp/door_debug.log",
                        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);

    if (pid == 0) {
        /* Child: hook up PTY slave as stdio, drop libssh fds, exec dosbox. */
        close(master_fd);
        if (setsid() < 0) {
            _exit(127);
        }
        if (ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
            /* Non-fatal — continue without controlling terminal. */
        }
        if (dup2(slave_fd, STDIN_FILENO) < 0 ||
            dup2(slave_fd, STDOUT_FILENO) < 0 ||
            dup2(slave_fd, STDERR_FILENO) < 0) {
            _exit(127);
        }
        if (slave_fd > STDERR_FILENO) {
            close(slave_fd);
        }

        /* Redirect stderr to a persistent debug log so exec/dosbox errors
         * survive after the PTY is torn down. */
        if (debug_fd >= 0) {
            dup2(debug_fd, STDERR_FILENO);
            close(debug_fd);
        }

        /* Close any other inherited fds so dosbox cannot accidentally talk
         * to a chat socket. */
        for (int fd = STDERR_FILENO + 1; fd < 1024; ++fd) {
            close(fd);
        }

        /* Ensure dosbox runs in the directory where the conf and game files
         * live, so relative mounts / autoexec paths resolve correctly. */
        if (game_dir != nullptr && game_dir[0] != '\0') {
            if (chdir(game_dir) != 0) {
                fprintf(stderr, "[door] chdir(%s) failed: %s\n",
                        game_dir, strerror(errno));
            }
        }

        setenv("SDL_VIDEODRIVER", "dummy", 1);
        setenv("SDL_AUDIODRIVER", "dummy", 1);
        unsetenv("DISPLAY");

        const char *chosen_runner = getenv("CHATTER_DOOR_RUNNER");
        if (chosen_runner == nullptr || chosen_runner[0] == '\0') {
            chosen_runner = getenv("DOSBOX_RUNNER");
        }

        char resolved_runner[PATH_MAX];
        if (chosen_runner == nullptr || chosen_runner[0] == '\0') {
            chosen_runner = nullptr;
            const char *candidates[] = {"dosbox", "dosbox-x", "dosbox-staging"};
            const char *path_env = getenv("PATH");
            if (path_env != nullptr && path_env[0] != '\0') {
                char *path_copy = strdup(path_env);
                if (path_copy != nullptr) {
                    for (size_t i = 0U; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
                        char *saveptr = nullptr;
                        char *token = strtok_r(path_copy, ":", &saveptr);
                        bool found = false;
                        while (token != nullptr) {
                            char full_path[PATH_MAX];
                            snprintf(full_path, sizeof(full_path), "%s/%s", token, candidates[i]);
                            struct stat st;
                            if (stat(full_path, &st) == 0 && (st.st_mode & S_IXUSR) && !S_ISDIR(st.st_mode)) {
                                snprintf(resolved_runner, sizeof(resolved_runner), "%s", candidates[i]);
                                chosen_runner = resolved_runner;
                                found = true;
                                break;
                            }
                            token = strtok_r(nullptr, ":", &saveptr);
                        }
                        if (found) {
                            break;
                        }
                        strcpy(path_copy, path_env);
                    }
                    free(path_copy);
                }
            }
            if (chosen_runner == nullptr) {
                chosen_runner = "dosbox"; // default fallback
            }
        }

        char *const argv[] = {
            (char *)chosen_runner,
            (char *)"-conf",
            (char *)dosbox_conf,
            (char *)"-exit",
            nullptr,
        };
        execvp(chosen_runner, argv);
        _exit(127);
    }

    close(slave_fd);
    if (debug_fd >= 0) {
        close(debug_fd);
    }
    if (session_bbs_door_set_nonblocking(master_fd) < 0) {
        kill(pid, SIGKILL);
        (void)waitpid(pid, nullptr, 0);
        close(master_fd);
        return false;
    }

    runner->child_pid = pid;
    runner->master_fd = master_fd;
    runner->started_at = time(nullptr);
    runner->active = true;
    return true;
}

static bool session_bbs_door_io_loop(session_ctx_t *ctx,
                                     host_door_runner_t *runner)
{
    if (ctx == nullptr || runner == nullptr || !runner->active) {
        return false;
    }

    char buffer[SSH_CHATTER_DOOR_BUFFER_SIZE];

    while (runner->active) {
        struct pollfd pfd = {
            .fd = runner->master_fd,
            .events = POLLIN,
            .revents = 0,
        };
        int poll_result = poll(&pfd, 1, SSH_CHATTER_DOOR_IDLE_POLL_MS);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (poll_result > 0 && (pfd.revents & POLLIN)) {
            ssize_t got = read(runner->master_fd, buffer, sizeof(buffer));
            if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            }
            if (got == 0) {
                /* PTY master read 0: slave closed.  Do not assume the child
                 * is gone — DOSBox may have closed stdout briefly during
                 * startup.  Verify with waitpid before tearing down. */
                int verify_status = 0;
                pid_t reaped = waitpid(runner->child_pid, &verify_status,
                                       WNOHANG);
                if (reaped == runner->child_pid) {
                    runner->child_pid = -1;
                    door_flush_detect_buffer(ctx, runner);
                    break;
                }
                /* Child still alive — give it a moment and keep polling. */
                struct timespec naptime = {
                    .tv_sec = 0,
                    .tv_nsec = 50 * 1000 * 1000L,
                };
                nanosleep(&naptime, nullptr);
                continue;
            }
            if (got > 0) {
                /* Encoding detection: pre-decision, accumulate into the
                 * detect buffer; once decided, every chunk goes through the
                 * converter. */
                if (!runner->encoding_decided) {
                    size_t previous_detect_length = runner->detect_length;
                    size_t room =
                        sizeof(runner->detect_buffer) - runner->detect_length;
                    size_t copy = ((size_t)got < room) ? (size_t)got : room;
                    if (copy > 0U) {
                        memcpy(runner->detect_buffer + runner->detect_length,
                               buffer, copy);
                        runner->detect_length += copy;
                    }
                    bool buffer_full = runner->detect_length ==
                                       sizeof(runner->detect_buffer);
                    bool seen_high_bit = false;
                    for (ssize_t k = 0; k < got; ++k) {
                        if ((unsigned char)buffer[k] >= 0x80U) {
                            seen_high_bit = true;
                            break;
                        }
                    }
                    if (!seen_high_bit && previous_detect_length == 0U) {
                        door_emit_converted(ctx, runner, buffer, (size_t)got);
                        runner->detect_length = 0U;
                        continue;
                    }
                    bool decide_now =
                        buffer_full ||
                        (runner->detect_length >= 256U && seen_high_bit);
                    if (decide_now) {
                        runner->detected_encoding =
                            door_enc_decide(runner->detect_buffer,
                                            runner->detect_length);
                        runner->encoding_decided = true;
                        /* Flush the accumulated buffer through the converter
                         * in one shot so we don't lose pre-decision output. */
                        door_emit_converted(ctx, runner, runner->detect_buffer,
                                            runner->detect_length);
                        runner->detect_length = 0U;
                    }
                } else {
                    door_emit_converted(ctx, runner, buffer, (size_t)got);
                }
            }
        } else if (poll_result > 0 &&
                   (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            door_flush_detect_buffer(ctx, runner);
            break;
        }

        /* Reap if the child has exited even when the PTY still has buffered
         * output to drain. */
        int status = 0;
        pid_t reaped = waitpid(runner->child_pid, &status, WNOHANG);
        if (reaped == runner->child_pid) {
            runner->child_pid = -1;
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                /* Drain any final output before returning. */
                for (;;) {
                    ssize_t residual =
                        read(runner->master_fd, buffer, sizeof(buffer));
                    if (residual <= 0) {
                        break;
                    }
                    if (!runner->encoding_decided) {
                        runner->detected_encoding =
                            door_enc_decide(runner->detect_buffer,
                                            runner->detect_length);
                        runner->encoding_decided = true;
                        if (runner->detect_length > 0U) {
                            door_emit_converted(ctx, runner,
                                                runner->detect_buffer,
                                                runner->detect_length);
                            runner->detect_length = 0U;
                        }
                    }
                    door_emit_converted(ctx, runner, buffer,
                                        (size_t)residual);
                }
                door_flush_detect_buffer(ctx, runner);
                break;
            }
        }

        /* Optionally read from the user, with a small timeout so we keep
         * polling the master fd. */
        int read_result =
            session_channel_read_poll(ctx, buffer, sizeof(buffer), 50);
        if (read_result == SESSION_CHANNEL_TIMEOUT) {
            /* nothing to forward */
        } else if (read_result <= 0) {
            /* Session lost; tear down the door. */
            break;
        } else {
            /* Look for the escape byte (Ctrl-]). */
            int escape_idx = -1;
            for (int idx = 0; idx < read_result; ++idx) {
                if ((unsigned char)buffer[idx] == SSH_CHATTER_DOOR_QUIT_BYTE) {
                    escape_idx = idx;
                    break;
                }
            }
            int forward_len = (escape_idx >= 0) ? escape_idx : read_result;
            ssize_t cursor = 0;
            while (cursor < forward_len) {
                ssize_t wrote = write(runner->master_fd, buffer + cursor,
                                      (size_t)(forward_len - cursor));
                if (wrote < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        struct timespec naptime = {
                            .tv_sec = 0,
                            .tv_nsec = 5 * 1000 * 1000L,
                        };
                        nanosleep(&naptime, nullptr);
                        continue;
                    }
                    forward_len = (int)cursor; /* stop forwarding */
                    break;
                }
                cursor += wrote;
            }
            if (escape_idx >= 0) {
                session_send_system_line(
                    ctx, "[door] escape sequence detected, ending door game.");
                break;
            }
        }

        /* Hard runtime cap. */
        if (runner->started_at != (time_t)-1 &&
            time(nullptr) - runner->started_at >
                SSH_CHATTER_DOOR_MAX_RUNTIME_SECONDS) {
            session_send_system_line(
                ctx, door_localized(ctx, DOOR_MSG_MAX_RUNTIME));
            break;
        }

        /* Honour a host shutdown signal so we don't pin the daemon. */
        if (ctx->owner != nullptr && ctx->owner->shutdown_flag != nullptr &&
            *ctx->owner->shutdown_flag != 0) {
            break;
        }
    }

    session_bbs_door_terminate_child(runner);
    return true;
}

static void session_bbs_door_run(session_ctx_t *ctx, const char *name)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (name == nullptr || name[0] == '\0') {
        session_bbs_door_list(ctx);
        return;
    }

    if (!session_bbs_door_caller_authorised(ctx)) {
        session_send_system_line(
            ctx, "Only operators may launch DOOR games on this server.");
        return;
    }

    const door_game_entry_t *entry = host_door_lookup(ctx->owner, name);
    if (entry == nullptr) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message),
                 "Unknown DOOR game '%s'. Try `/bbs door` for the list.",
                 name);
        session_send_system_line(ctx, message);
        return;
    }

    if (entry->locked && !ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(
            ctx,
            "[door] This game is currently locked by the operator.");
        return;
    }

    host_t *host = ctx->owner;
    if (host != nullptr && host->max_door_sessions > 0U &&
        host->active_door_sessions >= host->max_door_sessions) {
        session_send_system_line(
            ctx,
            "[door] too many active door sessions. Try again later.");
        return;
    }

    /* Default to the TCP-nullmodem relay path (door_relay.c).  The legacy
     * PTY path can be forced back with CHATTER_DOOR_USE_PTY=1. */
    if (getenv("CHATTER_DOOR_USE_PTY") == nullptr) {
        extern bool door_relay_run_session(session_ctx_t *,
                                           const door_game_entry_t *);
        (void)door_relay_run_session(ctx, entry);
        return;
    }

    char status[SSH_CHATTER_MESSAGE_LIMIT];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    snprintf(status, sizeof(status),
             door_localized(ctx, DOOR_MSG_LAUNCHING),
             entry->name, SSH_CHATTER_DOOR_MAX_RUNTIME_SECONDS);
#pragma GCC diagnostic pop
    session_send_system_line(ctx, status);

    host_door_runner_t runner = {
        .child_pid = -1,
        .master_fd = -1,
        .started_at = 0,
        .active = false,
        .detect_length = 0U,
        .encoding_decided = false,
        .detected_encoding = DOOR_ENC_UNKNOWN,
    };

    bool was_buffering = ctx->output_buffering_enabled;
    ctx->output_buffering_enabled = false;

    if (host != nullptr) {
        ++host->active_door_sessions;
    }

    if (!session_bbs_door_spawn(entry->dosbox_conf, &runner)) {
        ctx->output_buffering_enabled = was_buffering;
        if (host != nullptr && host->active_door_sessions > 0U) {
            --host->active_door_sessions;
        }
        session_send_system_line(ctx, door_localized(ctx, DOOR_MSG_FAILED_LAUNCH));
        return;
    }

    (void)session_bbs_door_io_loop(ctx, &runner);

    ctx->output_buffering_enabled = was_buffering;
    if (host != nullptr && host->active_door_sessions > 0U) {
        --host->active_door_sessions;
    }

    time_t elapsed = time(nullptr) - runner.started_at;
    if (elapsed <= 2) {
        session_send_system_line(ctx, door_localized(ctx, DOOR_MSG_ENDED_IMMEDIATELY));
    } else {
        session_send_system_line(ctx, door_localized(ctx, DOOR_MSG_SESSION_ENDED));
    }
}

static void session_bbs_setgamelock(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        session_send_system_line(
            ctx, door_localized(ctx, DOOR_MSG_ONLY_OPS_LOCK));
        return;
    }

    if (arguments == nullptr || arguments[0] == '\0') {
        session_send_system_line(ctx, door_localized(ctx, DOOR_MSG_SETGAMELOCK_USAGE));
        return;
    }

    char name[SSH_CHATTER_DOOR_GAME_NAME_LEN];
    snprintf(name, sizeof(name), "%s", arguments);
    trim_whitespace_inplace(name);

    host_t *host = ctx->owner;
    door_game_entry_t *target = nullptr;
    for (size_t i = 0U; i < host->door_game_count; ++i) {
        if (host->door_games[i].in_use &&
            strcasecmp(host->door_games[i].name, name) == 0) {
            target = &host->door_games[i];
            break;
        }
    }

    if (target == nullptr) {
        char msg[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(msg, sizeof(msg),
                 "Unknown door game '%s'. Try `/bbs door` for the list.",
                 name);
        session_send_system_line(ctx, msg);
        return;
    }

    target->locked = !target->locked;

    void host_door_games_save_locked(host_t *host);
    host_door_games_save_locked(host);

    char msg[SSH_CHATTER_MESSAGE_LIMIT];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    snprintf(msg, sizeof(msg),
             door_localized(ctx, target->locked ? DOOR_MSG_NOW_LOCKED
                                                : DOOR_MSG_NOW_UNLOCKED),
             target->name);
#pragma GCC diagnostic pop
    session_send_system_line(ctx, msg);
}

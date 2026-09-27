/**
 * @file host_bbs_and_games.c
 * @desc File-level documentation for host_bbs_and_games.c, describing its role
 *       in the SSH-Chatter server and providing a consistent header
 *       comment format across C sources.
 * @return None.
 */

// BBS workflows plus interactive mini-games.
#include "../internal.h"

static int session_game_random_range(session_ctx_t *ctx, int max);
static session_ctx_t *chat_room_find_user(chat_room_t *room,
                                          const char *username);
static void session_bbs_search_posts(session_ctx_t *ctx, const char *arguments);
extern void session_bbs_door_run(session_ctx_t *ctx, const char *name);
extern void session_bbs_setgamelock(session_ctx_t *ctx, const char *arguments);

typedef struct session_bbs_help_row {
    const char *syntax;
    const char *description[SESSION_UI_LANGUAGE_COUNT];
} session_bbs_help_row_t;

static const session_bbs_help_row_t kSessionBbsHelpRows[] = {
    {
        "list [all|hot|top|new] [page]",
        {
            [SESSION_UI_LANGUAGE_EN] = "List posts",
            [SESSION_UI_LANGUAGE_KO] = "게시물 목록",
            [SESSION_UI_LANGUAGE_JP] = "投稿一覧",
            [SESSION_UI_LANGUAGE_ZH] = "列出帖子",
            [SESSION_UI_LANGUAGE_RU] = "Список постов",
            [SESSION_UI_LANGUAGE_DE] = "Beiträge auflisten",
            [SESSION_UI_LANGUAGE_FR] = "Lister les messages",
            [SESSION_UI_LANGUAGE_PL] = "Lista wpisów",
        },
    },
    {
        "read <id>",
        {
            [SESSION_UI_LANGUAGE_EN] = "Read a post",
            [SESSION_UI_LANGUAGE_KO] = "게시물 읽기",
            [SESSION_UI_LANGUAGE_JP] = "投稿を読む",
            [SESSION_UI_LANGUAGE_ZH] = "阅读帖子",
            [SESSION_UI_LANGUAGE_RU] = "Прочитать пост",
            [SESSION_UI_LANGUAGE_DE] = "Beitrag lesen",
            [SESSION_UI_LANGUAGE_FR] = "Lire un message",
            [SESSION_UI_LANGUAGE_PL] = "Czytaj wpis",
        },
    },
    {
        "topic read <tag>",
        {
            [SESSION_UI_LANGUAGE_EN] = "List posts by tag",
            [SESSION_UI_LANGUAGE_KO] = "태그별 게시물 목록",
            [SESSION_UI_LANGUAGE_JP] = "タグ別の投稿一覧",
            [SESSION_UI_LANGUAGE_ZH] = "按标签列出帖子",
            [SESSION_UI_LANGUAGE_RU] = "Посты по тегу",
            [SESSION_UI_LANGUAGE_DE] = "Beiträge nach Tag auflisten",
            [SESSION_UI_LANGUAGE_FR] = "Lister les messages par tag",
            [SESSION_UI_LANGUAGE_PL] = "Wpisy według tagu",
        },
    },
    {
        "post <title> [tags...]",
        {
            [SESSION_UI_LANGUAGE_EN] = "Create a post",
            [SESSION_UI_LANGUAGE_KO] = "게시물 작성",
            [SESSION_UI_LANGUAGE_JP] = "投稿を作成",
            [SESSION_UI_LANGUAGE_ZH] = "发帖",
            [SESSION_UI_LANGUAGE_RU] = "Создать пост",
            [SESSION_UI_LANGUAGE_DE] = "Beitrag erstellen",
            [SESSION_UI_LANGUAGE_FR] = "Créer un message",
            [SESSION_UI_LANGUAGE_PL] = "Utwórz wpis",
        },
    },
    {
        "edit <id>",
        {
            [SESSION_UI_LANGUAGE_EN] = "Edit a post",
            [SESSION_UI_LANGUAGE_KO] = "게시물 수정",
            [SESSION_UI_LANGUAGE_JP] = "投稿を編集",
            [SESSION_UI_LANGUAGE_ZH] = "编辑帖子",
            [SESSION_UI_LANGUAGE_RU] = "Редактировать пост",
            [SESSION_UI_LANGUAGE_DE] = "Beitrag bearbeiten",
            [SESSION_UI_LANGUAGE_FR] = "Modifier un message",
            [SESSION_UI_LANGUAGE_PL] = "Edytuj wpis",
        },
    },
    {
        "comment <id>|<text>",
        {
            [SESSION_UI_LANGUAGE_EN] =
                "Add a comment (:N quotes, @nick mentions)",
            [SESSION_UI_LANGUAGE_KO] = "댓글 달기 (:N 인용, @닉 멘션)",
            [SESSION_UI_LANGUAGE_JP] =
                "コメントを追加（:N で引用、@ニックでメンション）",
            [SESSION_UI_LANGUAGE_ZH] = "添加评论（:N 引用，@昵称 提及）",
            [SESSION_UI_LANGUAGE_RU] =
                "Добавить комментарий (:N — цитата, @ник — упоминание)",
            [SESSION_UI_LANGUAGE_DE] =
                "Kommentar hinzufügen (:N zitiert, @Nick erwähnt)",
            [SESSION_UI_LANGUAGE_FR] =
                "Ajouter un commentaire (:N cite, @pseudo mentionne)",
            [SESSION_UI_LANGUAGE_PL] =
                "Dodaj komentarz (:N cytuje, @nick wspomina)",
        },
    },
    {
        "cmtedit <id> <idx> <text>",
        {
            [SESSION_UI_LANGUAGE_EN] = "Edit a comment",
            [SESSION_UI_LANGUAGE_KO] = "댓글 수정",
            [SESSION_UI_LANGUAGE_JP] = "コメントを編集",
            [SESSION_UI_LANGUAGE_ZH] = "编辑评论",
            [SESSION_UI_LANGUAGE_RU] = "Редактировать комментарий",
            [SESSION_UI_LANGUAGE_DE] = "Kommentar bearbeiten",
            [SESSION_UI_LANGUAGE_FR] = "Modifier un commentaire",
            [SESSION_UI_LANGUAGE_PL] = "Edytuj komentarz",
        },
    },
    {
        "cmtdel <id> <idx>",
        {
            [SESSION_UI_LANGUAGE_EN] = "Delete a comment",
            [SESSION_UI_LANGUAGE_KO] = "댓글 삭제",
            [SESSION_UI_LANGUAGE_JP] = "コメントを削除",
            [SESSION_UI_LANGUAGE_ZH] = "删除评论",
            [SESSION_UI_LANGUAGE_RU] = "Удалить комментарий",
            [SESSION_UI_LANGUAGE_DE] = "Kommentar löschen",
            [SESSION_UI_LANGUAGE_FR] = "Supprimer un commentaire",
            [SESSION_UI_LANGUAGE_PL] = "Usuń komentarz",
        },
    },
    {
        "upvote <id>",
        {
            [SESSION_UI_LANGUAGE_EN] = "Upvote a post",
            [SESSION_UI_LANGUAGE_KO] = "게시물 추천",
            [SESSION_UI_LANGUAGE_JP] = "投稿に高評価",
            [SESSION_UI_LANGUAGE_ZH] = "给帖子点赞",
            [SESSION_UI_LANGUAGE_RU] = "Проголосовать за пост",
            [SESSION_UI_LANGUAGE_DE] = "Beitrag positiv bewerten",
            [SESSION_UI_LANGUAGE_FR] = "Voter pour un message",
            [SESSION_UI_LANGUAGE_PL] = "Zagłosuj za wpisem",
        },
    },
    {
        "downvote <id>",
        {
            [SESSION_UI_LANGUAGE_EN] = "Downvote a post",
            [SESSION_UI_LANGUAGE_KO] = "게시물 비추천",
            [SESSION_UI_LANGUAGE_JP] = "投稿に低評価",
            [SESSION_UI_LANGUAGE_ZH] = "给帖子点踩",
            [SESSION_UI_LANGUAGE_RU] = "Проголосовать против поста",
            [SESSION_UI_LANGUAGE_DE] = "Beitrag negativ bewerten",
            [SESSION_UI_LANGUAGE_FR] = "Voter contre un message",
            [SESSION_UI_LANGUAGE_PL] = "Zagłosuj przeciw wpisowi",
        },
    },
    {
        "cmtvote <id> <idx> up|down",
        {
            [SESSION_UI_LANGUAGE_EN] = "Vote on a comment",
            [SESSION_UI_LANGUAGE_KO] = "댓글 투표",
            [SESSION_UI_LANGUAGE_JP] = "コメントに投票",
            [SESSION_UI_LANGUAGE_ZH] = "为评论投票",
            [SESSION_UI_LANGUAGE_RU] = "Оценить комментарий",
            [SESSION_UI_LANGUAGE_DE] = "Über einen Kommentar abstimmen",
            [SESSION_UI_LANGUAGE_FR] = "Voter sur un commentaire",
            [SESSION_UI_LANGUAGE_PL] = "Oceń komentarz",
        },
    },
    {
        "report <id> [reason]",
        {
            [SESSION_UI_LANGUAGE_EN] = "Report a post",
            [SESSION_UI_LANGUAGE_KO] = "게시물 신고",
            [SESSION_UI_LANGUAGE_JP] = "投稿を通報",
            [SESSION_UI_LANGUAGE_ZH] = "举报帖子",
            [SESSION_UI_LANGUAGE_RU] = "Пожаловаться на пост",
            [SESSION_UI_LANGUAGE_DE] = "Beitrag melden",
            [SESSION_UI_LANGUAGE_FR] = "Signaler un message",
            [SESSION_UI_LANGUAGE_PL] = "Zgłoś wpis",
        },
    },
    {
        "hide|unhide <id>",
        {
            [SESSION_UI_LANGUAGE_EN] = "Hide/unhide a post (op)",
            [SESSION_UI_LANGUAGE_KO] = "게시물 숨김/해제 (운영자)",
            [SESSION_UI_LANGUAGE_JP] = "投稿を非表示/再表示（オペ）",
            [SESSION_UI_LANGUAGE_ZH] = "隐藏/取消隐藏帖子（管理员）",
            [SESSION_UI_LANGUAGE_RU] = "Скрыть/показать пост (оп.)",
            [SESSION_UI_LANGUAGE_DE] = "Beitrag aus-/einblenden (Op)",
            [SESSION_UI_LANGUAGE_FR] = "Masquer/afficher un message (op)",
            [SESSION_UI_LANGUAGE_PL] = "Ukryj/pokaż wpis (op)",
        },
    },
    {
        "pin|unpin <id>",
        {
            [SESSION_UI_LANGUAGE_EN] = "Pin/unpin a post (op)",
            [SESSION_UI_LANGUAGE_KO] = "게시물 고정/해제 (운영자)",
            [SESSION_UI_LANGUAGE_JP] = "投稿を固定/解除（オペ）",
            [SESSION_UI_LANGUAGE_ZH] = "置顶/取消置顶帖子（管理员）",
            [SESSION_UI_LANGUAGE_RU] = "Закрепить/открепить пост (оп.)",
            [SESSION_UI_LANGUAGE_DE] = "Beitrag anheften/lösen (Op)",
            [SESSION_UI_LANGUAGE_FR] = "Épingler/désépingler un message (op)",
            [SESSION_UI_LANGUAGE_PL] = "Przypnij/odepnij wpis (op)",
        },
    },
    {
        "mute <user>[|min]",
        {
            [SESSION_UI_LANGUAGE_EN] = "Mute a user (op)",
            [SESSION_UI_LANGUAGE_KO] = "사용자 음소거 (운영자)",
            [SESSION_UI_LANGUAGE_JP] = "ユーザーを発言禁止（オペ）",
            [SESSION_UI_LANGUAGE_ZH] = "禁言用户（管理员）",
            [SESSION_UI_LANGUAGE_RU] = "Заглушить пользователя (оп.)",
            [SESSION_UI_LANGUAGE_DE] = "Benutzer stummschalten (Op)",
            [SESSION_UI_LANGUAGE_FR] = "Rendre un utilisateur muet (op)",
            [SESSION_UI_LANGUAGE_PL] = "Wycisz użytkownika (op)",
        },
    },
    {
        "unmute <user>",
        {
            [SESSION_UI_LANGUAGE_EN] = "Unmute a user (op)",
            [SESSION_UI_LANGUAGE_KO] = "사용자 음소거 해제 (운영자)",
            [SESSION_UI_LANGUAGE_JP] = "発言禁止を解除（オペ）",
            [SESSION_UI_LANGUAGE_ZH] = "解除禁言（管理员）",
            [SESSION_UI_LANGUAGE_RU] = "Снять мут (оп.)",
            [SESSION_UI_LANGUAGE_DE] = "Stummschaltung aufheben (Op)",
            [SESSION_UI_LANGUAGE_FR] = "Rétablir la parole (op)",
            [SESSION_UI_LANGUAGE_PL] = "Odcisz użytkownika (op)",
        },
    },
    {
        "mutes",
        {
            [SESSION_UI_LANGUAGE_EN] = "List active mutes (op)",
            [SESSION_UI_LANGUAGE_KO] = "음소거 목록 (운영자)",
            [SESSION_UI_LANGUAGE_JP] = "発言禁止の一覧（オペ）",
            [SESSION_UI_LANGUAGE_ZH] = "列出禁言（管理员）",
            [SESSION_UI_LANGUAGE_RU] = "Список мутов (оп.)",
            [SESSION_UI_LANGUAGE_DE] = "Aktive Stummschaltungen (Op)",
            [SESSION_UI_LANGUAGE_FR] = "Lister les mises en sourdine (op)",
            [SESSION_UI_LANGUAGE_PL] = "Lista wyciszeń (op)",
        },
    },
    {
        "reports [all]",
        {
            [SESSION_UI_LANGUAGE_EN] = "List reports (op)",
            [SESSION_UI_LANGUAGE_KO] = "신고 목록 (운영자)",
            [SESSION_UI_LANGUAGE_JP] = "通報一覧（オペ）",
            [SESSION_UI_LANGUAGE_ZH] = "列出举报（管理员）",
            [SESSION_UI_LANGUAGE_RU] = "Список жалоб (оп.)",
            [SESSION_UI_LANGUAGE_DE] = "Meldungen auflisten (Op)",
            [SESSION_UI_LANGUAGE_FR] = "Lister les signalements (op)",
            [SESSION_UI_LANGUAGE_PL] = "Lista zgłoszeń (op)",
        },
    },
    {
        "modlog",
        {
            [SESSION_UI_LANGUAGE_EN] = "Moderation action log (op)",
            [SESSION_UI_LANGUAGE_KO] = "운영 기록 (운영자)",
            [SESSION_UI_LANGUAGE_JP] = "モデレーション記録（オペ）",
            [SESSION_UI_LANGUAGE_ZH] = "管理操作日志（管理员）",
            [SESSION_UI_LANGUAGE_RU] = "Журнал модерации (оп.)",
            [SESSION_UI_LANGUAGE_DE] = "Moderationsprotokoll (Op)",
            [SESSION_UI_LANGUAGE_FR] = "Journal de modération (op)",
            [SESSION_UI_LANGUAGE_PL] = "Dziennik moderacji (op)",
        },
    },
    {
        "ipaudit <user>|ip <a>",
        {
            [SESSION_UI_LANGUAGE_EN] = "IP audit, 5-day retention (op)",
            [SESSION_UI_LANGUAGE_KO] = "IP 감사, 5일 보관 (운영자)",
            [SESSION_UI_LANGUAGE_JP] = "IP監査、5日間保持（オペ）",
            [SESSION_UI_LANGUAGE_ZH] = "IP 审计，保留 5 天（管理员）",
            [SESSION_UI_LANGUAGE_RU] = "Аудит IP, хранение 5 дней (оп.)",
            [SESSION_UI_LANGUAGE_DE] = "IP-Prüfung, 5 Tage Aufbewahrung (Op)",
            [SESSION_UI_LANGUAGE_FR] = "Audit IP, conservation 5 jours (op)",
            [SESSION_UI_LANGUAGE_PL] = "Audyt IP, przechowywanie 5 dni (op)",
        },
    },
    {
        "regen <id>",
        {
            [SESSION_UI_LANGUAGE_EN] = "Regenerate a post",
            [SESSION_UI_LANGUAGE_KO] = "게시물 재생성",
            [SESSION_UI_LANGUAGE_JP] = "投稿を再生成",
            [SESSION_UI_LANGUAGE_ZH] = "重新生成帖子",
            [SESSION_UI_LANGUAGE_RU] = "Пересоздать пост",
            [SESSION_UI_LANGUAGE_DE] = "Beitrag neu erzeugen",
            [SESSION_UI_LANGUAGE_FR] = "Régénérer un message",
            [SESSION_UI_LANGUAGE_PL] = "Wygeneruj wpis ponownie",
        },
    },
    {
        "delete <id>",
        {
            [SESSION_UI_LANGUAGE_EN] = "Delete a post",
            [SESSION_UI_LANGUAGE_KO] = "게시물 삭제",
            [SESSION_UI_LANGUAGE_JP] = "投稿を削除",
            [SESSION_UI_LANGUAGE_ZH] = "删除帖子",
            [SESSION_UI_LANGUAGE_RU] = "Удалить пост",
            [SESSION_UI_LANGUAGE_DE] = "Beitrag löschen",
            [SESSION_UI_LANGUAGE_FR] = "Supprimer un message",
            [SESSION_UI_LANGUAGE_PL] = "Usuń wpis",
        },
    },
    {
        "search <keyword>",
        {
            [SESSION_UI_LANGUAGE_EN] = "Search posts",
            [SESSION_UI_LANGUAGE_KO] = "게시물 검색",
            [SESSION_UI_LANGUAGE_JP] = "投稿を検索",
            [SESSION_UI_LANGUAGE_ZH] = "搜索帖子",
            [SESSION_UI_LANGUAGE_RU] = "Поиск постов",
            [SESSION_UI_LANGUAGE_DE] = "Beiträge durchsuchen",
            [SESSION_UI_LANGUAGE_FR] = "Rechercher des messages",
            [SESSION_UI_LANGUAGE_PL] = "Szukaj wpisów",
        },
    },
    {
        "board <id>",
        {
            [SESSION_UI_LANGUAGE_EN] = "Select a board",
            [SESSION_UI_LANGUAGE_KO] = "게시판 선택",
            [SESSION_UI_LANGUAGE_JP] = "掲示板を選択",
            [SESSION_UI_LANGUAGE_ZH] = "选择版块",
            [SESSION_UI_LANGUAGE_RU] = "Выбрать доску",
            [SESSION_UI_LANGUAGE_DE] = "Board auswählen",
            [SESSION_UI_LANGUAGE_FR] = "Choisir un tableau",
            [SESSION_UI_LANGUAGE_PL] = "Wybierz tablicę",
        },
    },
    {
        "boards",
        {
            [SESSION_UI_LANGUAGE_EN] = "List boards",
            [SESSION_UI_LANGUAGE_KO] = "게시판 목록",
            [SESSION_UI_LANGUAGE_JP] = "掲示板一覧",
            [SESSION_UI_LANGUAGE_ZH] = "列出版块",
            [SESSION_UI_LANGUAGE_RU] = "Список досок",
            [SESSION_UI_LANGUAGE_DE] = "Boards auflisten",
            [SESSION_UI_LANGUAGE_FR] = "Lister les tableaux",
            [SESSION_UI_LANGUAGE_PL] = "Lista tablic",
        },
    },
    {
        "profile [username]",
        {
            [SESSION_UI_LANGUAGE_EN] = "View user profile",
            [SESSION_UI_LANGUAGE_KO] = "사용자 프로필 보기",
            [SESSION_UI_LANGUAGE_JP] = "ユーザープロフィールを表示",
            [SESSION_UI_LANGUAGE_ZH] = "查看用户资料",
            [SESSION_UI_LANGUAGE_RU] = "Профиль пользователя",
            [SESSION_UI_LANGUAGE_DE] = "Benutzerprofil anzeigen",
            [SESSION_UI_LANGUAGE_FR] = "Voir le profil d'un utilisateur",
            [SESSION_UI_LANGUAGE_PL] = "Zobacz profil użytkownika",
        },
    },
    {
        "set-profile",
        {
            [SESSION_UI_LANGUAGE_EN] =
                "Set profile picture from pending ASCII art",
            [SESSION_UI_LANGUAGE_KO] =
                "대기 중인 ASCII 아트로 프로필 사진 설정",
            [SESSION_UI_LANGUAGE_JP] =
                "保留中のASCIIアートをプロフィール画像に設定",
            [SESSION_UI_LANGUAGE_ZH] = "用待发布的 ASCII 艺术设置头像",
            [SESSION_UI_LANGUAGE_RU] = "Сделать ожидающий ASCII-арт аватаром",
            [SESSION_UI_LANGUAGE_DE] =
                "Profilbild aus ausstehender ASCII-Art setzen",
            [SESSION_UI_LANGUAGE_FR] =
                "Définir la photo de profil depuis l'art ASCII en attente",
            [SESSION_UI_LANGUAGE_PL] =
                "Ustaw zdjęcie profilu z oczekującej grafiki ASCII",
        },
    },
    {
        "setavatar <name>",
        {
            [SESSION_UI_LANGUAGE_EN] =
                "Set profile logo (monitor|mouse|human|mushroom|none)",
            [SESSION_UI_LANGUAGE_KO] =
                "프로필 로고 설정 (monitor|mouse|human|mushroom|none)",
            [SESSION_UI_LANGUAGE_JP] =
                "プロフィールロゴを設定（monitor|mouse|human|mushroom|none）",
            [SESSION_UI_LANGUAGE_ZH] =
                "设置资料标志（monitor|mouse|human|mushroom|none）",
            [SESSION_UI_LANGUAGE_RU] =
                "Выбрать логотип профиля (monitor|mouse|human|mushroom|none)",
            [SESSION_UI_LANGUAGE_DE] =
                "Profillogo festlegen (monitor|mouse|human|mushroom|none)",
            [SESSION_UI_LANGUAGE_FR] =
                "Définir le logo du profil (monitor|mouse|human|mushroom|none)",
            [SESSION_UI_LANGUAGE_PL] =
                "Ustaw logo profilu (monitor|mouse|human|mushroom|none)",
        },
    },
    {
        "draft <save|list|load|delete>",
        {
            [SESSION_UI_LANGUAGE_EN] = "Manage drafts",
            [SESSION_UI_LANGUAGE_KO] = "임시저장 관리",
            [SESSION_UI_LANGUAGE_JP] = "下書きを管理",
            [SESSION_UI_LANGUAGE_ZH] = "管理草稿",
            [SESSION_UI_LANGUAGE_RU] = "Управление черновиками",
            [SESSION_UI_LANGUAGE_DE] = "Entwürfe verwalten",
            [SESSION_UI_LANGUAGE_FR] = "Gérer les brouillons",
            [SESSION_UI_LANGUAGE_PL] = "Zarządzaj szkicami",
        },
    },
    {
        "door [name]",
        {
            [SESSION_UI_LANGUAGE_EN] = "List or launch door games",
            [SESSION_UI_LANGUAGE_KO] = "도어 게임 목록/실행",
            [SESSION_UI_LANGUAGE_JP] = "ドアゲームの一覧/起動",
            [SESSION_UI_LANGUAGE_ZH] = "列出或启动门游戏",
            [SESSION_UI_LANGUAGE_RU] = "Список или запуск door-игр",
            [SESSION_UI_LANGUAGE_DE] = "Door-Spiele auflisten oder starten",
            [SESSION_UI_LANGUAGE_FR] = "Lister ou lancer les jeux door",
            [SESSION_UI_LANGUAGE_PL] = "Lista lub uruchomienie gier door",
        },
    },
    {
        "setgamelock <name>",
        {
            [SESSION_UI_LANGUAGE_EN] = "Lock a door game (admin)",
            [SESSION_UI_LANGUAGE_KO] = "도어 게임 잠금 (관리자)",
            [SESSION_UI_LANGUAGE_JP] = "ドアゲームをロック（管理者）",
            [SESSION_UI_LANGUAGE_ZH] = "锁定门游戏（管理员）",
            [SESSION_UI_LANGUAGE_RU] = "Заблокировать door-игру (админ)",
            [SESSION_UI_LANGUAGE_DE] = "Door-Spiel sperren (Admin)",
            [SESSION_UI_LANGUAGE_FR] = "Verrouiller un jeu door (admin)",
            [SESSION_UI_LANGUAGE_PL] = "Zablokuj grę door (admin)",
        },
    },
    {
        "exit",
        {
            [SESSION_UI_LANGUAGE_EN] = "Exit BBS mode",
            [SESSION_UI_LANGUAGE_KO] = "BBS 모드 종료",
            [SESSION_UI_LANGUAGE_JP] = "BBSモードを終了",
            [SESSION_UI_LANGUAGE_ZH] = "退出 BBS 模式",
            [SESSION_UI_LANGUAGE_RU] = "Выйти из BBS",
            [SESSION_UI_LANGUAGE_DE] = "BBS-Modus verlassen",
            [SESSION_UI_LANGUAGE_FR] = "Quitter le mode BBS",
            [SESSION_UI_LANGUAGE_PL] = "Wyjdź z trybu BBS",
        },
    },
};

static const char *session_bbs_help_title(session_ctx_t *ctx)
{
    switch (session_ui_language_current(ctx)) {
    case SESSION_UI_LANGUAGE_KO:
        return "BBS 하위 명령:";
    case SESSION_UI_LANGUAGE_JP:
        return "BBS サブコマンド:";
    case SESSION_UI_LANGUAGE_ZH:
        return "BBS 子命令:";
    case SESSION_UI_LANGUAGE_RU:
        return "Подкоманды BBS:";
    case SESSION_UI_LANGUAGE_DE:
        return "BBS-Unterbefehle:";
    case SESSION_UI_LANGUAGE_FR:
        return "Sous-commandes BBS :";
    case SESSION_UI_LANGUAGE_PL:
        return "Podpolecenia BBS:";
    default:
        return "BBS Subcommands:";
    }
}

// Handle the /bbs command entry point.
static void session_bbs_print_help(session_ctx_t *ctx)
{
    const size_t language = (size_t)session_ui_language_current(ctx);
    session_send_system_line(ctx, "--------------------------------------------------");
    session_send_system_line(ctx, session_bbs_help_title(ctx));
    for (size_t idx = 0U;
         idx < sizeof(kSessionBbsHelpRows) / sizeof(kSessionBbsHelpRows[0]);
         ++idx) {
        const session_bbs_help_row_t *row = &kSessionBbsHelpRows[idx];
        const char *description = row->description[SESSION_UI_LANGUAGE_EN];
        if (language < SESSION_UI_LANGUAGE_COUNT &&
            row->description[language] != nullptr) {
            description = row->description[language];
        }
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(line, sizeof(line), "  %-22s - %s", row->syntax, description);
        session_send_system_line(ctx, line);
    }
    session_send_system_line(ctx, "--------------------------------------------------");
}

static void session_handle_bbs(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (arguments == nullptr || *arguments == '\0') {
        session_bbs_show_dashboard(ctx);
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);
    if (working[0] == '\0') {
        session_bbs_show_dashboard(ctx);
        return;
    }

    char *command = working;
    char *rest = nullptr;
    for (char *cursor = working; *cursor != '\0'; ++cursor) {
        if (isspace((unsigned char)*cursor)) {
            *cursor = '\0';
            rest = cursor + 1;
            break;
        }
    }
    if (rest != nullptr) {
        trim_whitespace_inplace(rest);
    }

    if (strcmp(command, "exit") == 0) {
        ctx->in_bbs_mode = false;
        ctx->bbs_post_pending = false;
        ctx->bbs_view_active = false;
        ctx->bbs_view_post_id = 0U;
        ctx->bbs_view_scroll_offset = 0U;
        ctx->bbs_view_total_lines = 0U;
        ctx->bbs_rendering_editor = false;
        session_bbs_workspace_release(ctx);
        session_mode_pop_chat_context(ctx);
        session_send_system_line(ctx, "Exited BBS mode.");
        return;
    }

    if (strcmp(command, "help") == 0 || strcmp(command, "도움말") == 0) {
        session_bbs_print_help(ctx);
        return;
    }

    ctx->in_bbs_mode = true;
    session_mode_push_chat_context(ctx);

    const char *canonical_command =
        session_bbs_subcommand_canonicalize(ctx, command);
    if (canonical_command == nullptr) {
        session_send_system_line(
            ctx, "Unknown /bbs subcommand. Try /bbs for usage.");
        return;
    }

    if (strcmp(canonical_command, "list") == 0) {
        session_bbs_prepare_canvas(ctx);
        session_bbs_list(ctx, rest);
    } else if (strcmp(canonical_command, "help") == 0) {
        session_bbs_print_help(ctx);
    } else if (strcmp(canonical_command, "read") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_bbs_send_usage(ctx, "read", "<id>");
            return;
        }
        uint64_t id = (uint64_t)strtoull(rest, nullptr, 10);
        session_bbs_read(ctx, id);
    } else if (strcmp(canonical_command, "topic") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_bbs_send_usage(ctx, "topic", "read <tag>");
            return;
        }

        char topic_full[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(topic_full, sizeof(topic_full), "%s", rest);
        trim_whitespace_inplace(topic_full);
        if (topic_full[0] == '\0') {
            session_bbs_send_usage(ctx, "topic", "read <tag>");
            return;
        }

        char topic_args[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(topic_args, sizeof(topic_args), "%s", topic_full);

        char action_token[32];
        size_t action_len = 0U;
        char *cursor = topic_args;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
            if (action_len + 1U < sizeof(action_token)) {
                action_token[action_len++] = *cursor;
            }
            ++cursor;
        }
        action_token[action_len] = '\0';

        char *remaining = nullptr;
        if (*cursor != '\0') {
            *cursor = '\0';
            remaining = cursor + 1;
            trim_whitespace_inplace(remaining);
        }

        if (action_token[0] != '\0') {
            const char *canonical_action =
                session_bbs_subcommand_canonicalize(ctx, action_token);
            if (canonical_action != nullptr &&
                strcmp(canonical_action, "read") == 0) {
                if (remaining == nullptr || remaining[0] == '\0') {
                    session_bbs_send_usage(ctx, "topic", "read <tag>");
                    return;
                }
                session_bbs_prepare_canvas(ctx);
                session_bbs_list_topic(ctx, remaining);
                return;
            }
        }

        session_bbs_prepare_canvas(ctx);
        session_bbs_list_topic(ctx, topic_full);
    } else if (strcmp(canonical_command, "post") == 0) {
        session_bbs_begin_post(ctx, rest);
    } else if (strcmp(canonical_command, "edit") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_bbs_send_usage(ctx, "edit", "<id>");
            return;
        }
        uint64_t id = (uint64_t)strtoull(rest, nullptr, 10);
        session_bbs_begin_edit(ctx, id);
    } else if (strcmp(canonical_command, "comment") == 0) {
        session_bbs_add_comment(ctx, rest);
    } else if (strcmp(canonical_command, "cmtedit") == 0) {
        session_bbs_cmtedit(ctx, rest);
    } else if (strcmp(canonical_command, "cmtdel") == 0) {
        session_bbs_cmtdel(ctx, rest);
    } else if (strcmp(canonical_command, "regen") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_bbs_send_usage(ctx, "regen", "<id>");
            return;
        }
        uint64_t id = (uint64_t)strtoull(rest, nullptr, 10);
        session_bbs_regen_post(ctx, id);
    } else if (strcmp(canonical_command, "delete") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_bbs_send_usage(ctx, "delete", "<id>");
            return;
        }
        uint64_t id = (uint64_t)strtoull(rest, nullptr, 10);
        session_bbs_delete(ctx, id);
    } else if (strcmp(canonical_command, "door") == 0) {
        /* `/bbs door` lists doors; `/bbs door <name>` launches one. */
        const char *door_name = (rest != nullptr && rest[0] != '\0') ? rest
                                                                     : nullptr;
        session_bbs_door_run(ctx, door_name);
    } else if (strcmp(canonical_command, "boards") == 0) {
        session_bbs_boards(ctx);
    } else if (strcmp(canonical_command, "upvote") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_bbs_send_usage(ctx, "upvote", "<post_id>");
            return;
        }
        uint64_t id = (uint64_t)strtoull(rest, nullptr, 10);
        session_bbs_upvote(ctx, id);
    } else if (strcmp(canonical_command, "downvote") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_bbs_send_usage(ctx, "downvote", "<post_id>");
            return;
        }
        uint64_t id = (uint64_t)strtoull(rest, nullptr, 10);
        session_bbs_downvote(ctx, id);
    } else if (strcmp(canonical_command, "cmtvote") == 0) {
        session_bbs_cmtvote(ctx, rest);
    } else if (strcmp(canonical_command, "report") == 0) {
        session_bbs_report(ctx, rest);
    } else if (strcmp(canonical_command, "reports") == 0) {
        session_bbs_reports(ctx, rest);
    } else if (strcmp(canonical_command, "hide") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_bbs_send_usage(ctx, "hide", "<id>");
            return;
        }
        session_bbs_set_mod_flag(ctx, (uint64_t)strtoull(rest, nullptr, 10),
                                 SSH_CHATTER_BBS_MOD_FLAG_HIDDEN, true, "hide",
                                 "Post hidden.");
    } else if (strcmp(canonical_command, "unhide") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_bbs_send_usage(ctx, "unhide", "<id>");
            return;
        }
        session_bbs_set_mod_flag(ctx, (uint64_t)strtoull(rest, nullptr, 10),
                                 SSH_CHATTER_BBS_MOD_FLAG_HIDDEN, false,
                                 "unhide", "Post unhidden.");
    } else if (strcmp(canonical_command, "pin") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_bbs_send_usage(ctx, "pin", "<id>");
            return;
        }
        session_bbs_set_mod_flag(ctx, (uint64_t)strtoull(rest, nullptr, 10),
                                 SSH_CHATTER_BBS_MOD_FLAG_PINNED, true, "pin",
                                 "Post pinned.");
    } else if (strcmp(canonical_command, "unpin") == 0) {
        if (rest == nullptr || rest[0] == '\0') {
            session_bbs_send_usage(ctx, "unpin", "<id>");
            return;
        }
        session_bbs_set_mod_flag(ctx, (uint64_t)strtoull(rest, nullptr, 10),
                                 SSH_CHATTER_BBS_MOD_FLAG_PINNED, false,
                                 "unpin", "Post unpinned.");
    } else if (strcmp(canonical_command, "mute") == 0) {
        session_bbs_mute(ctx, rest);
    } else if (strcmp(canonical_command, "unmute") == 0) {
        session_bbs_unmute(ctx, rest);
    } else if (strcmp(canonical_command, "mutes") == 0) {
        session_bbs_mutes(ctx);
    } else if (strcmp(canonical_command, "modlog") == 0) {
        session_bbs_modlog(ctx);
    } else if (strcmp(canonical_command, "ipaudit") == 0) {
        session_bbs_ipaudit(ctx, rest);
    } else if (strcmp(canonical_command, "profile") == 0) {
        session_bbs_profile(ctx, rest);
    } else if (strcmp(canonical_command, "set-profile") == 0) {
        session_bbs_set_profile(ctx);
    } else if (strcmp(canonical_command, "draft") == 0) {
        session_bbs_draft(ctx, rest);
    } else if (strcmp(canonical_command, "board") == 0) {
        session_bbs_select_board(ctx, rest);
    } else if (strcmp(canonical_command, "search") == 0) {
        session_bbs_search_posts(ctx, rest);
    } else if (strcmp(canonical_command, "setavatar") == 0) {
        session_bbs_setavatar(ctx, rest);
    } else if (strcmp(canonical_command, "setgamelock") == 0) {
        session_bbs_setgamelock(ctx, rest);
    } else {
        session_send_system_line(
            ctx, "Unknown /bbs subcommand. Try /bbs for usage.");
    }
}

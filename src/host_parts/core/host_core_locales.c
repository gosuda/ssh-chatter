static const char kTranslationQuotaNotice[] =
    "[!] Translation quota exhausted. Translation features are temporarily "
    "disabled.";
static const char kTranslationQuotaSystemMessage[] =
    "Translation quota exhausted. Translation has been disabled. Try again "
    "later.";

static const char *const kSessionCommandNames[] = {
    "asciiart",
    "audio",
    "ban",
    "banlist",
    "bbs",
    "birthday",
    "block",
    "breaking",
    "captcha",
    "chat",
    "chat-spacing",
    "color",
    "connected",
    "date",
    "delete-msg",
    "elect",
    "eliza",
    "eliza-chat",
    "exit",
    "files",
    "game",
    "gemini",
    "gemini-unfreeze",
    "getos",
    "grant",
    "help",
    "history",
    "advanced",
    "image",
    "kick",
    "mode",
    "motd",
    "nick",
    "os",
    "pair",
    "palette",
    "pardon",
    "pm",
    "poke",
    "poll",
    "reply",
    "revoke",
    "rss",
    "search",
    "setpw",
    "shell",
    "set-target-lang",
    "set-trans-lang",
    "set-ui-lang",
    "showstatus",
    "status",
    "suspend!",
    "sync-trigger",
    "systemcolor",
    "today",
    "translate",
    "translate-scope",
    "unblock",
    "users",
    "video",
    "vote",
    "vote-single",
    "weather",
    "gameopt",
};
#define SSH_CHATTER_COMMAND_COUNT                                              \
    (sizeof(kSessionCommandNames) / sizeof(kSessionCommandNames[0]))

typedef enum session_help_entry_kind {
    SESSION_HELP_ENTRY_COMMAND = 0,
    SESSION_HELP_ENTRY_FORMATTED,
    SESSION_HELP_ENTRY_TEXT,
} session_help_entry_kind_t;

#define SESSION_HELP_TEMPLATE_ARG_LIMIT 8U

typedef enum session_help_template_arg_kind {
    SESSION_HELP_TEMPLATE_ARG_PREFIX = 0,
    SESSION_HELP_TEMPLATE_ARG_ASCIIART_TERMINATOR,
    SESSION_HELP_TEMPLATE_ARG_BBS_TERMINATOR,
    SESSION_HELP_TEMPLATE_ARG_COMMAND_REPLY,
    SESSION_HELP_TEMPLATE_ARG_COMMAND_GOOD,
    SESSION_HELP_TEMPLATE_ARG_COMMAND_SAD,
    SESSION_HELP_TEMPLATE_ARG_COMMAND_WTF,
    SESSION_HELP_TEMPLATE_ARG_COMMAND_COOL,
    SESSION_HELP_TEMPLATE_ARG_COMMAND_ANGRY,
    SESSION_HELP_TEMPLATE_ARG_COMMAND_CHECKED,
    SESSION_HELP_TEMPLATE_ARG_COMMAND_LOVE,
    SESSION_HELP_TEMPLATE_ARG_COMMAND_BBS,
} session_help_template_arg_kind_t;

typedef struct session_help_entry {
    session_help_entry_kind_t kind;
    const char *label;
    const char *description[SESSION_UI_LANGUAGE_COUNT];
    const char *label_translations[SESSION_UI_LANGUAGE_COUNT];
    size_t label_arg_count;
    session_help_template_arg_kind_t
        label_args[SESSION_HELP_TEMPLATE_ARG_LIMIT];
    size_t description_arg_count;
    session_help_template_arg_kind_t
        description_args[SESSION_HELP_TEMPLATE_ARG_LIMIT];
} session_help_entry_t;

typedef struct session_command_alias {
    const char *canonical;
    const char *localized[SESSION_UI_LANGUAGE_COUNT];
} session_command_alias_t;

typedef struct session_ui_locale {
    session_ui_language_t language;
    const char *code;
    const char *help_title;
    const char *help_hint_extra;
    const char *help_scroll_hint;
    const char *help_regular_hint;
    const char *help_extra_title;
    const char *help_extra_hint;
    const char *help_operator_title;
    const char *welcome_help_hint;
    const char *welcome_motd_hint;
    const char *welcome_history_hint;
    const char *chat_spacing_usage;
    const char *chat_spacing_immediate;
    const char *chat_spacing_single;
    const char *chat_spacing_multiple;
    const char *set_ui_lang_usage;
    const char *set_ui_lang_success;
    const char *set_ui_lang_invalid;
    const char *mode_status_format;
    const char *mode_label_chat;
    const char *mode_label_command;
    const char *mode_explain_chat;
    const char *mode_explain_command;
    const char *mode_already_chat;
    const char *mode_already_command;
    const char *mode_enabled_chat;
    const char *mode_enabled_command;
    const char *mode_usage;
    const char *unknown_command;
    const char *help_morse;
} session_ui_locale_t;

static const char *const kSessionUiLanguageCodes[SESSION_UI_LANGUAGE_COUNT] = {
    "en", "ko", "jp", "zh", "ru", "de", "fr", "pl",
};

static const char *const kSessionUiLanguageNames
    [SESSION_UI_LANGUAGE_COUNT][SESSION_UI_LANGUAGE_COUNT] = {
        [SESSION_UI_LANGUAGE_EN] = {"English", "Korean", "Japanese", "Chinese",
                                    "Russian", "German", "French", "Polish"},
        [SESSION_UI_LANGUAGE_KO] = {"영어", "한국어", "일본어", "중국어",
                                    "러시아어", "독일어", "프랑스어",
                                    "폴란드어"},
        [SESSION_UI_LANGUAGE_JP] = {"英語", "韓国語", "日本語", "中国語",
                                    "ロシア語", "ドイツ語", "フランス語",
                                    "ポーランド語"},
        [SESSION_UI_LANGUAGE_ZH] = {"英语", "韩语", "日语", "中文", "俄语",
                                    "德语", "法语", "波兰语"},
        [SESSION_UI_LANGUAGE_RU] = {"английский", "корейский", "японский",
                                    "китайский", "русский", "немецкий",
                                    "французский", "польский"},
        [SESSION_UI_LANGUAGE_DE] = {"Englisch", "Koreanisch", "Japanisch",
                                    "Chinesisch", "Russisch", "Deutsch",
                                    "Französisch", "Polnisch"},
        [SESSION_UI_LANGUAGE_FR] = {"Anglais", "Coréen", "Japonais", "Chinois",
                                    "Russe", "Allemand", "Français",
                                    "Polonais"},
        [SESSION_UI_LANGUAGE_PL] = {"Angielski", "Koreański", "Japoński",
                                    "Chiński", "Rosyjski", "Niemiecki",
                                    "Francuski", "Polski"},
};

static const session_ui_locale_t kSessionUiLocales[SESSION_UI_LANGUAGE_COUNT] =
    {
        {
            .language = SESSION_UI_LANGUAGE_EN,
            .code = "en",
            .help_title = "Essential commands:",
            .help_hint_extra = "See %sadvanced for optional commands.",
            .help_scroll_hint =
                "Use Up/Down arrows to scroll chat or command history.",
            .help_regular_hint = "Regular messages are shared with everyone.",
            .help_extra_title = "Extended commands:",
            .help_extra_hint = "Return to %shelp for essentials.",
            .help_operator_title = "Operator commands:",
            .welcome_help_hint = "Use %shelp to view the manual.",
            .welcome_motd_hint = "Use %smotd to read the information.",
            .welcome_history_hint = "Previous messages are hidden. Use Up/Down "
                                    "arrows to browse older chat.",
            .chat_spacing_usage = "Usage: %schat-spacing <0-5>",
            .chat_spacing_immediate = "Translation captions will appear "
                                      "immediately without reserving "
                                      "extra blank lines.",
            .chat_spacing_single = "Translation captions will reserve 1 blank "
                                   "line before appearing in chat threads.",
            .chat_spacing_multiple =
                "Translation captions will reserve %s blank lines before "
                "appearing in chat threads.",
            .set_ui_lang_usage =
                "Usage: %sset-ui-lang <ko|en|jp|zh|ru|de|fr|pl>",
            .set_ui_lang_success =
                "UI language set to %s. Use %shelp to review commands.",
            .set_ui_lang_invalid = "Unsupported language. Use one of: ko, en, "
                                   "jp, zh, ru, de, fr, pl.",
            .mode_status_format = "Current input mode: %s.",
            .mode_label_chat = "chat",
            .mode_label_command = "command",
            .mode_explain_chat =
                "Chat mode: send messages normally. Prefix commands with %s.",
            .mode_explain_command =
                "Command mode: type commands without a prefix, use "
                "UpArrow/DownArrow for history, Tab for completion.",
            .mode_already_chat =
                "Already in chat mode. Commands require the %s prefix.",
            .mode_already_command =
                "Command mode already active. Enter commands without a prefix, "
                "use UpArrow/DownArrow for history, Tab to autocomplete.",
            .mode_enabled_chat =
                "Chat mode enabled. Commands once again require the %s prefix.",
            .mode_enabled_command =
                "Command mode enabled. Enter commands without a prefix; use "
                "UpArrow/DownArrow for history and Tab for completion.",
            .mode_usage = "Usage: %smode <chat|command|toggle>",
            .unknown_command = "Unknown command. Type %shelp for help.",
        },
        {
            .language = SESSION_UI_LANGUAGE_KO,
            .code = "ko",
            .help_title = "필수 명령:",
            .help_hint_extra =
                "%sadvanced에서 선택 및 운영자 명령을 확인합니다.",
            .help_scroll_hint =
                "위/아래 화살표로 채팅이나 명령 기록을 살펴볼 수 있습니다.",
            .help_regular_hint = "일반 메시지는 모두에게 공유됩니다.",
            .help_extra_title = "확장 명령:",
            .help_extra_hint = "핵심 목록은 %shelp에서 다시 볼 수 있습니다.",
            .help_operator_title = "운영자 명령:",
            .welcome_help_hint = "%shelp 명령으로 도움말을 확인하세요.",
            .welcome_motd_hint = "%smotd 명령으로 안내를 읽을 수 있습니다.",
            .welcome_history_hint = "이전 메시지는 숨겨져 있습니다. 위/아래 "
                                    "화살표로 지난 채팅을 살펴보세요.",
            .chat_spacing_usage = "사용법: %schat-spacing <0-5>",
            .chat_spacing_immediate =
                "번역 자막이 빈 줄을 예약하지 않고 즉시 표시됩니다.",
            .chat_spacing_single =
                "번역 자막이 표시되기 전에 빈 줄 1줄을 예약합니다.",
            .chat_spacing_multiple =
                "번역 자막이 표시되기 전에 빈 줄 %s줄을 예약합니다.",
            .set_ui_lang_usage =
                "사용법: %sset-ui-lang <ko|en|jp|zh|ru|de|fr|pl>",
            .set_ui_lang_success = "UI 언어를 %s로 설정했습니다. %shelp "
                                   "명령으로 목록을 다시 확인하세요.",
            .set_ui_lang_invalid =
                "지원하지 않는 언어입니다. ko, en, jp, zh, ru, de, fr, pl"
                "ru 중에서 선택하세요.",
            .mode_status_format = "현재 입력 모드: %s.",
            .mode_label_chat = "채팅",
            .mode_label_command = "명령",
            .mode_explain_chat = "채팅 모드: 일반 메시지를 보내고, 명령은 %s "
                                 "접두사를 붙여 입력하세요.",
            .mode_explain_command = "명령 모드: 접두사 없이 명령을 입력하고, "
                                    "위/아래 화살표로 기록을 "
                                    "탐색하며 Tab으로 자동완성하세요.",
            .mode_already_chat =
                "이미 채팅 모드입니다. 명령은 %s 접두사가 필요합니다.",
            .mode_already_command =
                "이미 명령 모드입니다. 접두사 없이 입력하고, 위/아래 화살표와 "
                "Tab을 활용하세요.",
            .mode_enabled_chat = "채팅 모드가 활성화되었습니다. 명령은 다시 %s "
                                 "접두사가 필요합니다.",
            .mode_enabled_command =
                "명령 모드가 활성화되었습니다. 접두사 없이 입력하고, 위/아래 "
                "화살표와 Tab을 사용하세요.",
            .mode_usage = "사용법: %smode <chat|command|toggle>",
            .unknown_command =
                "알 수 없는 명령입니다. 도움말은 %shelp에서 확인하세요.",
        },
        {
            .language = SESSION_UI_LANGUAGE_JP,
            .code = "jp",
            .help_title = "基本コマンド:",
            .help_hint_extra = "追加コマンドは %sadvanced で確認できます。",
            .help_scroll_hint =
                "上下の矢印でチャットやコマンド履歴をたどれます。",
            .help_regular_hint = "通常のメッセージは全員に共有されます。",
            .help_extra_title = "拡張コマンド:",
            .help_extra_hint = "基本一覧に戻るには %shelp を実行してください。",
            .help_operator_title = "オペレーター用コマンド:",
            .welcome_help_hint = "%shelp でヘルプを表示できます。",
            .welcome_motd_hint = "%smotd でお知らせを確認できます。",
            .welcome_history_hint = "以前のメッセージは非表示です。上下の矢印で"
                                    "過去のチャットを確認できます。",
            .chat_spacing_usage = "使い方: %schat-spacing <0-5>",
            .chat_spacing_immediate =
                "翻訳字幕は空行を確保せずすぐに表示されます。",
            .chat_spacing_single = "翻訳字幕は表示前に空行を 1 行確保します。",
            .chat_spacing_multiple =
                "翻訳字幕は表示前に空行を %s 行確保します。",
            .set_ui_lang_usage =
                "使い方: %sset-ui-lang <ko|en|jp|zh|ru|de|fr|pl>",
            .set_ui_lang_success = "UI 言語を %s に設定しました。%shelp "
                                   "でコマンドを再確認してください。",
            .set_ui_lang_invalid =
                "対応していない言語です。ko, en, jp, zh, ru, de, fr, pl"
                "から選んでください。",
            .mode_status_format = "現在の入力モード: %s。",
            .mode_label_chat = "チャット",
            .mode_label_command = "コマンド",
            .mode_explain_chat =
                "チャットモード: 通常通りメッセージを送り、コマンドは %s "
                "を付けて入力します。",
            .mode_explain_command =
                "コマンドモード: 接頭辞なしで入力し、上下矢印で履歴を、Tab "
                "で補完を利用できます。",
            .mode_already_chat =
                "すでにチャットモードです。コマンドには %s を付けてください。",
            .mode_already_command = "すでにコマンドモードです。接頭辞なしで入力"
                                    "し、上下矢印と Tab を使ってください。",
            .mode_enabled_chat = "チャットモードを有効にしました。コマンドには"
                                 "再び %s が必要です。",
            .mode_enabled_command = "コマンドモードを有効にしました。接頭辞なし"
                                    "で入力し、上下矢印と "
                                    "Tab を使ってください。",
            .mode_usage = "使い方: %smode <chat|command|toggle>",
            .unknown_command =
                "不明なコマンドです。%shelp で確認してください。",
        },
        {
            .language = SESSION_UI_LANGUAGE_ZH,
            .code = "zh",
            .help_title = "核心命令：",
            .help_hint_extra = "更多命令请查看 %sadvanced。",
            .help_scroll_hint = "使用上下方向键查看聊天或命令历史。",
            .help_regular_hint = "普通消息会分享给所有人。",
            .help_extra_title = "扩展命令：",
            .help_extra_hint = "返回核心列表请使用 %shelp。",
            .help_operator_title = "管理员命令：",
            .welcome_help_hint = "使用 %shelp 查看帮助。",
            .welcome_motd_hint = "使用 %smotd 阅读公告。",
            .welcome_history_hint =
                "之前的消息已隐藏。使用上下方向键查看较早的聊天。",
            .chat_spacing_usage = "用法：%schat-spacing <0-5>",
            .chat_spacing_immediate = "翻译字幕会立即显示，不再预留空行。",
            .chat_spacing_single = "翻译字幕在显示前会预留 1 行空白。",
            .chat_spacing_multiple = "翻译字幕在显示前会预留 %s 行空白。",
            .set_ui_lang_usage =
                "用法：%sset-ui-lang <ko|en|jp|zh|ru|de|fr|pl>",
            .set_ui_lang_success =
                "界面语言已切换为 %s。可用 %shelp 重新查看命令。",
            .set_ui_lang_invalid =
                "不支持的语言，请选择 ko、en、jp、zh、ru、de、fr、pl。",
            .mode_status_format = "当前输入模式：%s。",
            .mode_label_chat = "聊天",
            .mode_label_command = "命令",
            .mode_explain_chat = "聊天模式：正常发送消息，命令需加上 %s 前缀。",
            .mode_explain_command = "命令模式：直接输入命令，不需要前缀；用上下"
                                    "方向键查看历史，Tab 自动补全。",
            .mode_already_chat = "已经是聊天模式。命令需要 %s 前缀。",
            .mode_already_command =
                "已经是命令模式。无需前缀，使用上下方向键和 Tab。",
            .mode_enabled_chat = "聊天模式已启用。命令重新需要 %s 前缀。",
            .mode_enabled_command =
                "命令模式已启用。无需前缀，可用上下方向键和 Tab。",
            .mode_usage = "用法：%smode <chat|command|toggle>",
            .unknown_command = "未知命令。请使用 %shelp 查看帮助。",
        },
        {
            .language = SESSION_UI_LANGUAGE_RU,
            .code = "ru",
            .help_title = "Основные команды:",
            .help_hint_extra = "Дополнительные команды смотрите в %sadvanced.",
            .help_scroll_hint =
                "Стрелки вверх/вниз листают чат или историю команд.",
            .help_regular_hint = "Обычные сообщения видны всем.",
            .help_extra_title = "Дополнительные команды:",
            .help_extra_hint = "К основному списку вернёт %shelp.",
            .help_operator_title = "Команды оператора:",
            .welcome_help_hint = "Команду %shelp используйте для справки.",
            .welcome_motd_hint = "%smotd покажет объявление.",
            .welcome_history_hint =
                "Предыдущие сообщения скрыты. Используйте стрелки вверх/вниз, "
                "чтобы просмотреть старый чат.",
            .chat_spacing_usage = "Использование: %schat-spacing <0-5>",
            .chat_spacing_immediate = "Подписи перевода будут появляться "
                                      "сразу, без запасных пустых строк.",
            .chat_spacing_single =
                "Подписи перевода перед выводом резервируют 1 пустую строку.",
            .chat_spacing_multiple =
                "Подписи перевода перед выводом резервируют %s пустых строк.",
            .set_ui_lang_usage =
                "Использование: %sset-ui-lang <ko|en|jp|zh|ru|de|fr|pl>",
            .set_ui_lang_success = "Язык интерфейса переключён на %s. Команды "
                                   "можно пересмотреть через %shelp.",
            .set_ui_lang_invalid = "Этот язык не поддерживается. Выберите один "
                                   "из: ko, en, jp, zh, ru, de, fr, pl.",
            .mode_status_format = "Текущий режим ввода: %s.",
            .mode_label_chat = "чат",
            .mode_label_command = "команды",
            .mode_explain_chat = "Режим чата: отправляйте сообщения, а команды "
                                 "вводите с префиксом %s.",
            .mode_explain_command =
                "Режим команд: вводите без префикса, используйте стрелки "
                "вверх/вниз для истории и Tab для автодополнения.",
            .mode_already_chat =
                "Вы уже в режиме чата. Командам нужен префикс %s.",
            .mode_already_command = "Режим команд уже активен. Вводите без "
                                    "префикса, пользуйтесь стрелками и Tab.",
            .mode_enabled_chat =
                "Включён режим чата. Командам снова требуется префикс %s.",
            .mode_enabled_command = "Включён режим команд. Вводите без "
                                    "префикса, применяйте стрелки и Tab.",
            .mode_usage = "Использование: %smode <chat|command|toggle>",
            .unknown_command = "Неизвестная команда. Подсказка — %shelp.",
        },
        {
            .language = SESSION_UI_LANGUAGE_DE,
            .code = "de",
            .help_title = "Wichtige Befehle:",
            .help_hint_extra =
                "%sadvanced für zusätzliche und Operator-Befehle.",
            .help_scroll_hint =
                "Auf-/Ab-Pfeile zum Scrollen von Chat oder Befehlsverlauf.",
            .help_regular_hint = "Normal Nachrichten werden mit allen geteilt.",
            .help_extra_title = "Erweiterte Befehle:",
            .help_extra_hint = "Zurück zu %shelp für die Grundlagen.",
            .help_operator_title = "Operator-Befehle:",
            .welcome_help_hint = "Benutzen Sie %shelp für das Handbuch.",
            .welcome_motd_hint = "Benutzen Sie %smotd für die Informationen.",
            .welcome_history_hint =
                "Vorherige Nachrichten sind verborgen. Auf-/Ab-Pfeile für "
                "älteren Chat.",
            .chat_spacing_usage = "Nutzung: %schat-spacing <0-5>",
            .chat_spacing_immediate =
                "Übersetzungsunterschriften erscheinen "
                "sofort ohne Reservierung von Leerzeilen.",
            .chat_spacing_single =
                "Übersetzungsunterschriften reservieren 1 Leerzeile vor "
                "Erscheinen in Chat-Threads.",
            .chat_spacing_multiple =
                "Übersetzungsunterschriften reservieren %s Leerzeilen vor "
                "Erscheinen in Chat-Threads.",
            .set_ui_lang_usage =
                "Nutzung: %sset-ui-lang <ko|en|jp|zh|ru|de|fr|pl>",
            .set_ui_lang_success =
                "UI-Sprache auf %s gesetzt. %shelp zeigt Befehle.",
            .set_ui_lang_invalid =
                "Nicht unterstützte Sprache. Wählen Sie: ko, "
                "en, jp, zh, ru, de, fr, pl.",
            .mode_status_format = "Aktueller Eingabemodus: %s.",
            .mode_label_chat = "Chat",
            .mode_label_command = "Befehl",
            .mode_explain_chat =
                "Chat-Modus: Nachrichten normal senden. Befehle mit %s "
                "prefixen.",
            .mode_explain_command = "Befehlsmodus: Befehle ohne Präfix "
                                    "eingeben, Auf-/Ab-Pfeile für "
                                    "Verlauf, Tab für Vervollständigung.",
            .mode_already_chat =
                "Bereits im Chat-Modus. Befehle benötigen das %s Präfix.",
            .mode_already_command =
                "Befehlsmodus bereits aktiv. Befehle ohne Präfix, "
                "Auf-/Ab-Pfeile für Verlauf, Tab für Vervollständigung.",
            .mode_enabled_chat =
                "Chat-Modus aktiviert. Befehle benötigen wieder das %s Präfix.",
            .mode_enabled_command =
                "Befehlsmodus aktiviert. Befehle ohne Präfix, Auf-/Ab-Pfeile "
                "und Tab verfügbar.",
            .mode_usage = "Nutzung: %smode <chat|command|toggle>",
            .unknown_command =
                "Unbekannter Befehl. Tippen Sie %shelp für Hilfe.",
        },
        {
            .language = SESSION_UI_LANGUAGE_FR,
            .code = "fr",
            .help_title = "Commandes essentielles:",
            .help_hint_extra =
                "%sadvanced pour les commandes supplémentaires et d'opérateur.",
            .help_scroll_hint =
                "Flèches Haut/Bas pour faire défiler le chat ou l'historique.",
            .help_regular_hint =
                "Les messages normaux sont partagés avec tous.",
            .help_extra_title = "Commandes étendues:",
            .help_extra_hint = "Retour à %shelp pour l'essentiel.",
            .help_operator_title = "Commandes d'opérateur:",
            .welcome_help_hint = "Utilisez %shelp pour afficher le manuel.",
            .welcome_motd_hint = "Utilisez %smotd pour lire les informations.",
            .welcome_history_hint =
                "Messages précédents sont cachés. Flèches Haut/Bas pour "
                "parcourir l'ancien chat.",
            .chat_spacing_usage = "Utilisation: %schat-spacing <0-5>",
            .chat_spacing_immediate = "Les sous-titres de traduction "
                                      "apparaissent immédiatement sans "
                                      "réserver de lignes vides.",
            .chat_spacing_single =
                "Les sous-titres de traduction réserveront 1 ligne vide avant "
                "d'apparaître dans les fils de discussion.",
            .chat_spacing_multiple =
                "Les sous-titres de traduction réserveront %s lignes vides "
                "avant d'apparaître dans les fils de discussion.",
            .set_ui_lang_usage =
                "Utilisation: %sset-ui-lang <ko|en|jp|zh|ru|de|fr|pl>",
            .set_ui_lang_success = "Langue de l'interface définie sur %s. "
                                   "Utilisez %shelp pour revoir les commandes.",
            .set_ui_lang_invalid = "Langue non prise en charge. Utilisez l'une "
                                   "des: ko, en, jp, zh, ru, de, fr, pl.",
            .mode_status_format = "Mode d'entrée actuel: %s.",
            .mode_label_chat = "chat",
            .mode_label_command = "commande",
            .mode_explain_chat =
                "Mode chat: envoyez des messages normalement. Préfixez les "
                "commandes avec %s.",
            .mode_explain_command =
                "Mode commande: tapez des commandes sans préfixe, flèches "
                "Haut/Bas pour l'historique, Tab pour la complétion.",
            .mode_already_chat =
                "Déjà en mode chat. Les commandes nécessitent le préfixe %s.",
            .mode_already_command =
                "Mode commande déjà actif. Entrez les commandes sans préfixe, "
                "flèches Haut/Bas pour l'historique, Tab pour compléter.",
            .mode_enabled_chat =
                "Mode chat activé. Les commandes nécessitent à "
                "nouveau le préfixe %s.",
            .mode_enabled_command =
                "Mode commande activé. Entrez les commandes sans préfixe, "
                "flèches Haut/Bas et Tab disponibles.",
            .mode_usage = "Utilisation: %smode <chat|command|toggle>",
            .unknown_command = "Commande inconnue. Tapez %shelp pour l'aide.",
        },
        {
            .language = SESSION_UI_LANGUAGE_PL,
            .code = "pl",
            .help_title = "Podstawowe polecenia:",
            .help_hint_extra =
                "%sadvanced dla dodatkowych poleceń i poleceń operatora.",
            .help_scroll_hint =
                "Strzałki Góra/Dół do przewijania czatu lub historii poleceń.",
            .help_regular_hint = "Zwykłe wiadomości są udostępniane wszystkim.",
            .help_extra_title = "Rozszerzone polecenia:",
            .help_extra_hint = "Wróć do %shelp dla podstaw.",
            .help_operator_title = "Polecenia operatora:",
            .welcome_help_hint = "Użyj %shelp aby zobaczyć instrukcję.",
            .welcome_motd_hint = "Użyj %smotd aby przeczytać informacje.",
            .welcome_history_hint =
                "Poprzednie wiadomości są ukryte. Strzałki Góra/Dół do "
                "przewijania starszego czatu.",
            .chat_spacing_usage = "Użycie: %schat-spacing <0-5>",
            .chat_spacing_immediate =
                "Napisy tłumaczeń pojawiają się "
                "natychmiast bez rezerwowania pustych linii.",
            .chat_spacing_single =
                "Napisy tłumaczeń rezerwują 1 pustą linię przed "
                "pojawieniem się w wątkach czatu.",
            .chat_spacing_multiple =
                "Napisy tłumaczeń rezerwują %s pustych linii przed "
                "pojawieniem się w wątkach czatu.",
            .set_ui_lang_usage =
                "Użycie: %sset-ui-lang <ko|en|jp|zh|ru|de|fr|pl>",
            .set_ui_lang_success = "Język interfejsu ustawiony na %s. "
                                   "Użyj %shelp aby zobaczyć polecenia.",
            .set_ui_lang_invalid = "Nieobsługiwany język. Użyj jednego z: ko, "
                                   "en, jp, zh, ru, de, fr, pl.",
            .mode_status_format = "Aktualny tryb wprowadzania: %s.",
            .mode_label_chat = "czat",
            .mode_label_command = "polecenie",
            .mode_explain_chat =
                "Tryb czatu: wysyłaj wiadomości normalnie. Dodaj prefiks %s do "
                "poleceń.",
            .mode_explain_command =
                "Tryb poleceń: wpisuj polecenia bez prefiksu, strzałki "
                "Góra/Dół dla historii, Tab dla uzupełniania.",
            .mode_already_chat =
                "Już w trybie czatu. Polecenia wymagają prefiksu %s.",
            .mode_already_command =
                "Tryb poleceń już aktywny. Wpisuj polecenia bez prefiksu, "
                "strzałki Góra/Dół dla historii, Tab dla uzupełniania.",
            .mode_enabled_chat = "Tryb czatu włączony. Polecenia ponownie "
                                 "wymagają prefiksu %s.",
            .mode_enabled_command =
                "Tryb poleceń włączony. Wpisuj polecenia bez prefiksu, "
                "strzałki Góra/Dół i Tab dostępne.",
            .mode_usage = "Użycie: %smode <chat|command|toggle>",
            .unknown_command =
                "Nieznane polecenie. Wpisz %shelp aby uzyskać pomoc.",
        },
};

static const char
    *const kSessionAsciiartTerminators[SESSION_UI_LANGUAGE_COUNT] = {
        [SESSION_UI_LANGUAGE_EN] = SSH_CHATTER_ASCIIART_TERMINATOR_EN,
        [SESSION_UI_LANGUAGE_KO] = ">/__그림_끝>",
        [SESSION_UI_LANGUAGE_JP] = ">/__アート_終了>",
        [SESSION_UI_LANGUAGE_ZH] = ">/__图像_结束>",
        [SESSION_UI_LANGUAGE_RU] = ">/__АРТ_КОНЕЦ>",
        [SESSION_UI_LANGUAGE_DE] = ">/__KUNST_ENDE>",
        [SESSION_UI_LANGUAGE_FR] = ">/__ART_FIN>",
        [SESSION_UI_LANGUAGE_PL] = ">/__SZTUKA_KONIEC>",
};

static const char *const kSessionBbsTerminators[SESSION_UI_LANGUAGE_COUNT] = {
    [SESSION_UI_LANGUAGE_EN] = SSH_CHATTER_BBS_TERMINATOR_EN,
    [SESSION_UI_LANGUAGE_KO] = ">/__게시판_끝>",
    [SESSION_UI_LANGUAGE_JP] = ">/__掲示板_終了>",
    [SESSION_UI_LANGUAGE_ZH] = ">/__公告板_结束>",
    [SESSION_UI_LANGUAGE_RU] = ">/__ДОСКА_КОНЕЦ>",
    [SESSION_UI_LANGUAGE_DE] = ">/__BBS_ENDE>",
    [SESSION_UI_LANGUAGE_FR] = ">/__BBS_FIN>",
    [SESSION_UI_LANGUAGE_PL] = ">/__BBS_KONIEC>",
};

static const session_command_alias_t kSessionCommandAliases[] = {
    {
        .canonical = "/reply",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/답장",
                [SESSION_UI_LANGUAGE_JP] = "/返信",
                [SESSION_UI_LANGUAGE_ZH] = "/回复",
                [SESSION_UI_LANGUAGE_RU] = "/ответ",
            },
    },
    {
        .canonical = "/nick",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/닉",
                [SESSION_UI_LANGUAGE_JP] = "/ニックネ",
                [SESSION_UI_LANGUAGE_ZH] = "/昵称",
                [SESSION_UI_LANGUAGE_RU] = "/ник",
            },
    },
    {
        .canonical = "/good",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/좋아요",
                [SESSION_UI_LANGUAGE_JP] = "/いいね",
                [SESSION_UI_LANGUAGE_ZH] = "/点赞",
                [SESSION_UI_LANGUAGE_RU] = "/класс",
            },
    },
    {
        .canonical = "/sad",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/슬퍼요",
                [SESSION_UI_LANGUAGE_JP] = "/かなしい",
                [SESSION_UI_LANGUAGE_ZH] = "/难过",
                [SESSION_UI_LANGUAGE_RU] = "/грусть",
            },
    },
    {
        .canonical = "/wtf",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/어쩌라고",
                [SESSION_UI_LANGUAGE_JP] = "/なんだと",
                [SESSION_UI_LANGUAGE_ZH] = "/搞什么",
                [SESSION_UI_LANGUAGE_RU] = "/чтоэто",
            },
    },
    {
        .canonical = "/cool",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/멋져요",
                [SESSION_UI_LANGUAGE_JP] = "/クール",
                [SESSION_UI_LANGUAGE_ZH] = "/酷",
                [SESSION_UI_LANGUAGE_RU] = "/круто",
            },
    },
    {
        .canonical = "/angry",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/화나요",
                [SESSION_UI_LANGUAGE_JP] = "/怒り",
                [SESSION_UI_LANGUAGE_ZH] = "/生气",
                [SESSION_UI_LANGUAGE_RU] = "/злой",
            },
    },
    {
        .canonical = "/checked",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/확인",
                [SESSION_UI_LANGUAGE_JP] = "/確認済み",
                [SESSION_UI_LANGUAGE_ZH] = "/已检查",
                [SESSION_UI_LANGUAGE_RU] = "/проверено",
            },
    },
    {
        .canonical = "/love",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/사랑해요",
                [SESSION_UI_LANGUAGE_JP] = "/愛",
                [SESSION_UI_LANGUAGE_ZH] = "/爱",
                [SESSION_UI_LANGUAGE_RU] = "/любовь",
            },
    },
    {
        .canonical = "/bbs",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/게시판",
                [SESSION_UI_LANGUAGE_JP] = "/掲示板",
                [SESSION_UI_LANGUAGE_ZH] = "/公告板",
                [SESSION_UI_LANGUAGE_RU] = "/доска",
            },
    },
    {
        .canonical = "/vote",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/투표",
            },
    },
    {
        .canonical = "/poll",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/전역투표",
            },
    },
    {
        .canonical = "/vote-single",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/단일투표",
            },
    },
    {
        .canonical = "/elect",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/선택",
            },
    },
    {
        .canonical = "/ban",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/밴",
                [SESSION_UI_LANGUAGE_JP] = "/バン",
                [SESSION_UI_LANGUAGE_ZH] = "/封禁",
                [SESSION_UI_LANGUAGE_RU] = "/бан",
            },
    },
    {
        .canonical = "/banlist",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/밴목록",
                [SESSION_UI_LANGUAGE_JP] = "/バンリスト",
                [SESSION_UI_LANGUAGE_ZH] = "/封禁列表",
                [SESSION_UI_LANGUAGE_RU] = "/список-банов",
            },
    },
    {
        .canonical = "/banname",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/닉차단",
                [SESSION_UI_LANGUAGE_JP] = "/ニックネ禁止",
                [SESSION_UI_LANGUAGE_ZH] = "/屏蔽昵称",
                [SESSION_UI_LANGUAGE_RU] = "/забанить-ник",
            },
    },
    {
        .canonical = "/delete-msg",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/메시지삭제",
            },
    },
    {
        .canonical = "/search",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/검색",
                [SESSION_UI_LANGUAGE_JP] = "/検索",
                [SESSION_UI_LANGUAGE_ZH] = "/搜索",
                [SESSION_UI_LANGUAGE_RU] = "/поиск",
            },
    },
    {
        .canonical = "/image",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/이미지",
                [SESSION_UI_LANGUAGE_JP] = "/画像",
                [SESSION_UI_LANGUAGE_ZH] = "/图片",
                [SESSION_UI_LANGUAGE_RU] = "/изображение",
            },
    },
    {
        .canonical = "/video",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/영상",
                [SESSION_UI_LANGUAGE_JP] = "/動画",
                [SESSION_UI_LANGUAGE_ZH] = "/视频",
                [SESSION_UI_LANGUAGE_RU] = "/видео",
            },
    },
    {
        .canonical = "/audio",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/오디오",
                [SESSION_UI_LANGUAGE_JP] = "/音声",
                [SESSION_UI_LANGUAGE_ZH] = "/音频",
                [SESSION_UI_LANGUAGE_RU] = "/аудио",
            },
    },
    {
        .canonical = "/files",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/파일",
                [SESSION_UI_LANGUAGE_JP] = "/ファイル",
                [SESSION_UI_LANGUAGE_ZH] = "/文件",
                [SESSION_UI_LANGUAGE_RU] = "/файлы",
            },
    },
    {
        .canonical = "/filestore",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/filestore",
                [SESSION_UI_LANGUAGE_JP] = "/filestore",
                [SESSION_UI_LANGUAGE_ZH] = "/filestore",
                [SESSION_UI_LANGUAGE_RU] = "/filestore",
            },
    },
    {
        .canonical = "/filestore-upload",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/filestore-upload",
                [SESSION_UI_LANGUAGE_JP] = "/filestore-upload",
                [SESSION_UI_LANGUAGE_ZH] = "/filestore-upload",
                [SESSION_UI_LANGUAGE_RU] = "/filestore-upload",
            },
    },
    {
        .canonical = "/filestore-download",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/filestore-download",
                [SESSION_UI_LANGUAGE_JP] = "/filestore-download",
                [SESSION_UI_LANGUAGE_ZH] = "/filestore-download",
                [SESSION_UI_LANGUAGE_RU] = "/filestore-download",
            },
    },
    {
        .canonical = "/mail",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/메일",
                [SESSION_UI_LANGUAGE_JP] = "/メール",
                [SESSION_UI_LANGUAGE_ZH] = "/邮件",
                [SESSION_UI_LANGUAGE_RU] = "/почта",
            },
    },
    {
        .canonical = "/asciiart",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/아스키아트",
                [SESSION_UI_LANGUAGE_JP] = "/アスキーアート",
                [SESSION_UI_LANGUAGE_ZH] = "/艺术",
                [SESSION_UI_LANGUAGE_RU] = "/ASCIIарт",
            },
    },
    {
        .canonical = "/game",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/게임",
                [SESSION_UI_LANGUAGE_JP] = "/ゲーム",
                [SESSION_UI_LANGUAGE_ZH] = "/游戏",
                [SESSION_UI_LANGUAGE_RU] = "/игра",
            },
    },
    {
        .canonical = "/gemini",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/제미니",
                [SESSION_UI_LANGUAGE_JP] = "/ジェミニ",
                [SESSION_UI_LANGUAGE_ZH] = "/Gemini",
                [SESSION_UI_LANGUAGE_RU] = "/джемини",
            },
    },
    {
        .canonical = "/gemini-unfreeze",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/제미니-해제",
                [SESSION_UI_LANGUAGE_JP] = "/ジェミニ-解除",
                [SESSION_UI_LANGUAGE_ZH] = "/Gemini-解冻",
                [SESSION_UI_LANGUAGE_RU] = "/джемини-разморозить",
            },
    },
    {
        .canonical = "/color",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/색상",
                [SESSION_UI_LANGUAGE_JP] = "/色",
                [SESSION_UI_LANGUAGE_ZH] = "/颜色",
                [SESSION_UI_LANGUAGE_RU] = "/цвет",
            },
    },
    {
        .canonical = "/captcha",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/캡차",
                [SESSION_UI_LANGUAGE_JP] = "/キャプチャ",
                [SESSION_UI_LANGUAGE_ZH] = "/验证码",
                [SESSION_UI_LANGUAGE_RU] = "/капча",
            },
    },
    {
        .canonical = "/systemcolor",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/시스템색상",
                [SESSION_UI_LANGUAGE_JP] = "/システムカラー",
                [SESSION_UI_LANGUAGE_ZH] = "/系统颜色",
                [SESSION_UI_LANGUAGE_RU] = "/системныйцвет",
            },
    },
    {
        .canonical = "/set-trans-lang",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/번역언어설정",
                [SESSION_UI_LANGUAGE_JP] = "/翻訳言語設定",
                [SESSION_UI_LANGUAGE_ZH] = "/设置翻译语言",
                [SESSION_UI_LANGUAGE_RU] = "/установить-язык-перевода",
            },
    },
    {
        .canonical = "/set-target-lang",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/대상언어설정",
                [SESSION_UI_LANGUAGE_JP] = "/ターゲット言語設定",
                [SESSION_UI_LANGUAGE_ZH] = "/设置目标语言",
                [SESSION_UI_LANGUAGE_RU] = "/установить-целевой-язык",
            },
    },
    {
        .canonical = "/set-ui-lang",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/UI언어설정",
                [SESSION_UI_LANGUAGE_JP] = "/UI言語設定",
                [SESSION_UI_LANGUAGE_ZH] = "/界面语言设置",
                [SESSION_UI_LANGUAGE_RU] = "/язык-интерфейса",
            },
    },
    {
        .canonical = "/grant",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/권한부여",
            },
    },
    {
        .canonical = "/shell",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/쉘",
            },
    },
    {
        .canonical = "/revoke",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/권한해제",
            },
    },
    {
        .canonical = "/kick",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/강퇴",
                [SESSION_UI_LANGUAGE_JP] = "/キック",
                [SESSION_UI_LANGUAGE_ZH] = "/踢出",
                [SESSION_UI_LANGUAGE_RU] = "/кик",
            },
    },
    {
        .canonical = "/poke",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/호출",
                [SESSION_UI_LANGUAGE_JP] = "/つつく",
                [SESSION_UI_LANGUAGE_ZH] = "/戳",
                [SESSION_UI_LANGUAGE_RU] = "/пинг",
            },
    },
    {
        .canonical = "/weather",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/날씨",
                [SESSION_UI_LANGUAGE_JP] = "/天気",
                [SESSION_UI_LANGUAGE_ZH] = "/天气",
                [SESSION_UI_LANGUAGE_RU] = "/погода",
            },
    },
    {
        .canonical = "/translate",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/번역",
                [SESSION_UI_LANGUAGE_JP] = "/翻訳",
                [SESSION_UI_LANGUAGE_ZH] = "/翻译",
                [SESSION_UI_LANGUAGE_RU] = "/перевод",
            },
    },
    {
        .canonical = "/translate-scope",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/번역범위",
                [SESSION_UI_LANGUAGE_JP] = "/翻訳範囲",
                [SESSION_UI_LANGUAGE_ZH] = "/翻译范围",
                [SESSION_UI_LANGUAGE_RU] = "/область-перевода",
            },
    },
    {
        .canonical = "/chat-spacing",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/채팅간격",
                [SESSION_UI_LANGUAGE_JP] = "/チャット間隔",
                [SESSION_UI_LANGUAGE_ZH] = "/聊天间距",
                [SESSION_UI_LANGUAGE_RU] = "/интервал-чата",
            },
    },
    {
        .canonical = "/palette",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/팔레트",
                [SESSION_UI_LANGUAGE_JP] = "/パレット",
                [SESSION_UI_LANGUAGE_ZH] = "/调色板",
                [SESSION_UI_LANGUAGE_RU] = "/палитра",
            },
    },
    {
        .canonical = "/today",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/오늘",
                [SESSION_UI_LANGUAGE_JP] = "/今日",
                [SESSION_UI_LANGUAGE_ZH] = "/今日",
                [SESSION_UI_LANGUAGE_RU] = "/сегодня",
            },
    },
    {
        .canonical = "/date",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/날짜",
                [SESSION_UI_LANGUAGE_JP] = "/日付",
                [SESSION_UI_LANGUAGE_ZH] = "/日期",
                [SESSION_UI_LANGUAGE_RU] = "/дата",
            },
    },
    {
        .canonical = "/os",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/운영체제",
                [SESSION_UI_LANGUAGE_JP] = "/OS",
                [SESSION_UI_LANGUAGE_ZH] = "/操作系统",
                [SESSION_UI_LANGUAGE_RU] = "/ОС",
            },
    },
    {
        .canonical = "/getos",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/운영체제확인",
                [SESSION_UI_LANGUAGE_JP] = "/OS取得",
                [SESSION_UI_LANGUAGE_ZH] = "/获取操作系统",
                [SESSION_UI_LANGUAGE_RU] = "/получить-ОС",
            },
    },
    {
        .canonical = "/birthday",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/생일",
                [SESSION_UI_LANGUAGE_JP] = "/誕生日",
                [SESSION_UI_LANGUAGE_ZH] = "/生日",
                [SESSION_UI_LANGUAGE_RU] = "/деньрождения",
            },
    },
    {
        .canonical = "/pair",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/짝",
                [SESSION_UI_LANGUAGE_JP] = "/ペア",
                [SESSION_UI_LANGUAGE_ZH] = "/配对",
                [SESSION_UI_LANGUAGE_RU] = "/пара",
            },
    },
    {
        .canonical = "/connected",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/접속자",
                [SESSION_UI_LANGUAGE_JP] = "/接続中",
                [SESSION_UI_LANGUAGE_ZH] = "/在线",
                [SESSION_UI_LANGUAGE_RU] = "/подключенные",
            },
    },
    {
        .canonical = "/users",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/접속자수",
                [SESSION_UI_LANGUAGE_JP] = "/ユーザー数",
                [SESSION_UI_LANGUAGE_ZH] = "/用户数量",
                [SESSION_UI_LANGUAGE_RU] = "/пользователи",
            },
    },
    {
        .canonical = "/pardon",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/사면",
                [SESSION_UI_LANGUAGE_JP] = "/許し",
                [SESSION_UI_LANGUAGE_ZH] = "/赦免",
                [SESSION_UI_LANGUAGE_RU] = "/помиловать",
            },
    },
    {
        .canonical = "/eliza",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/엘리자",
                [SESSION_UI_LANGUAGE_JP] = "/エリザ",
                [SESSION_UI_LANGUAGE_ZH] = "/伊丽莎",
                [SESSION_UI_LANGUAGE_RU] = "/элиза",
            },
    },
    {
        .canonical = "/ollama-model",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/ollama-모델",
                [SESSION_UI_LANGUAGE_JP] = "/ollamaモデル",
                [SESSION_UI_LANGUAGE_ZH] = "/ollama模型",
                [SESSION_UI_LANGUAGE_RU] = "/ollama-модель",
            },
    },
    {
        .canonical = "/getaddr",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/주소확인",
                [SESSION_UI_LANGUAGE_JP] = "/アドレス取得",
                [SESSION_UI_LANGUAGE_ZH] = "/获取地址",
                [SESSION_UI_LANGUAGE_RU] = "/получить-адрес",
            },
    },
    {
        .canonical = "/alpha-centauri-landers",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/알파센타우리착륙자",
            },
    },
    {
        .canonical = "/block",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/차단",
            },
    },
    {
        .canonical = "/advanced",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/advanced",
            },
    },
    {
        .canonical = "/sync-trigger",
    },
    {
        .canonical = "/set-sync-url",
    },
    {
        .canonical = "/setpw",
        .localized =
            {
                [SESSION_UI_LANGUAGE_KO] = "/비밀번호설정",
                [SESSION_UI_LANGUAGE_JP] = "/パスワード設定",
                [SESSION_UI_LANGUAGE_ZH] = "/设置密码",
                [SESSION_UI_LANGUAGE_RU] = "/установить-пароль",
            },
    },
};

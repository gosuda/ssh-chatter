static const size_t kSessionCommandAliasCount =
    sizeof(kSessionCommandAliases) / sizeof(kSessionCommandAliases[0]);

typedef struct session_bbs_subcommand_alias {
    const char *canonical;
    const char *localized[SESSION_UI_LANGUAGE_COUNT];
} session_bbs_subcommand_alias_t;

static const session_bbs_subcommand_alias_t kSessionBbsSubcommands[] = {
    {
        .canonical = "list",
        .localized =
            {
                [SESSION_UI_LANGUAGE_EN] = "list",
                [SESSION_UI_LANGUAGE_KO] = "목록",
                [SESSION_UI_LANGUAGE_JP] = "一覧",
                [SESSION_UI_LANGUAGE_ZH] = "列表",
                [SESSION_UI_LANGUAGE_RU] = "список",
            },
    },
    {
        .canonical = "read",
        .localized =
            {
                [SESSION_UI_LANGUAGE_EN] = "read",
                [SESSION_UI_LANGUAGE_KO] = "읽기",
                [SESSION_UI_LANGUAGE_JP] = "閲覧",
                [SESSION_UI_LANGUAGE_ZH] = "阅读",
                [SESSION_UI_LANGUAGE_RU] = "читать",
            },
    },
    {
        .canonical = "topic",
        .localized =
            {
                [SESSION_UI_LANGUAGE_EN] = "topic",
                [SESSION_UI_LANGUAGE_KO] = "주제",
                [SESSION_UI_LANGUAGE_JP] = "トピック",
                [SESSION_UI_LANGUAGE_ZH] = "主题",
                [SESSION_UI_LANGUAGE_RU] = "тема",
            },
    },
    {
        .canonical = "post",
        .localized =
            {
                [SESSION_UI_LANGUAGE_EN] = "post",
                [SESSION_UI_LANGUAGE_KO] = "게시",
                [SESSION_UI_LANGUAGE_JP] = "投稿",
                [SESSION_UI_LANGUAGE_ZH] = "发布",
                [SESSION_UI_LANGUAGE_RU] = "пост",
            },
    },
    {
        .canonical = "edit",
        .localized =
            {
                [SESSION_UI_LANGUAGE_EN] = "edit",
                [SESSION_UI_LANGUAGE_KO] = "수정",
                [SESSION_UI_LANGUAGE_JP] = "編集",
                [SESSION_UI_LANGUAGE_ZH] = "编辑",
                [SESSION_UI_LANGUAGE_RU] = "редакт",
            },
    },
    {
        .canonical = "comment",
        .localized =
            {
                [SESSION_UI_LANGUAGE_EN] = "comment",
                [SESSION_UI_LANGUAGE_KO] = "댓글",
                [SESSION_UI_LANGUAGE_JP] = "コメント",
                [SESSION_UI_LANGUAGE_ZH] = "评论",
                [SESSION_UI_LANGUAGE_RU] = "коммент",
            },
    },
    {
        .canonical = "regen",
        .localized =
            {
                [SESSION_UI_LANGUAGE_EN] = "regen",
                [SESSION_UI_LANGUAGE_KO] = "갱신",
                [SESSION_UI_LANGUAGE_JP] = "再掲",
                [SESSION_UI_LANGUAGE_ZH] = "置顶",
                [SESSION_UI_LANGUAGE_RU] = "поднять",
            },
    },
    {
        .canonical = "delete",
        .localized =
            {
                [SESSION_UI_LANGUAGE_EN] = "delete",
                [SESSION_UI_LANGUAGE_KO] = "삭제",
                [SESSION_UI_LANGUAGE_JP] = "削除",
                [SESSION_UI_LANGUAGE_ZH] = "删除",
                [SESSION_UI_LANGUAGE_RU] = "удалить",
            },
    },
};

static const size_t kSessionBbsSubcommandCount =
    sizeof(kSessionBbsSubcommands) / sizeof(kSessionBbsSubcommands[0]);

static const session_help_entry_t kSessionHelpEssential[] = {
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "help",
        .description =
            {
                "Show essential chat and BBS commands.",
                "채팅과 게시판에 필요한 핵심 명령을 보여줍니다.",
                "チャットと掲示板で必要な基本コマンドを表示します。",
                "显示聊天与公告板所需的核心命令。",
                "Показать основные команды для чата и доски объявлений.",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "advanced",
        .description =
            {
                "List optional and operator commands.",
                "선택 및 운영자 명령을 확인합니다.",
                "補助および運営向けコマンドを一覧表示します。",
                "列出可选命令和管理员命令。",
                "Показать дополнительные и операторские команды.",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "reply <message-id|r<reply-id>> <text>",
        .description =
            {
                "Reply to a message or reply.",
                "메시지 또는 답글에 답장합니다.",
                "メッセージまたは返信に返信します。",
                "回复消息或回复链。",
                "Ответить на сообщение или ответ.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "답장 <메시지ID|r<답글ID>> <내용>",
                [SESSION_UI_LANGUAGE_JP] =
                    "返信 <メッセージID|r<返信ID>> <本文>",
                [SESSION_UI_LANGUAGE_ZH] = "回复 <消息ID|r<回复ID>> <内容>",
                [SESSION_UI_LANGUAGE_RU] =
                    "ответ <id сообщения|r<id ответа>> <текст>",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "color (text;highlight[;bold])",
        .description =
            {
                "Style your handle.",
                "사용자 이름 색상을 꾸밉니다.",
                "ハンドル名の配色を設定します。",
                "设置昵称的配色。",
                "Настроить оформление вашего ника.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "색상 (텍스트;하이라이트[;굵게])",
                [SESSION_UI_LANGUAGE_JP] = "色 (テキスト;ハイライト[;太字])",
                [SESSION_UI_LANGUAGE_ZH] = "颜色 (文本;高亮[;粗体])",
                [SESSION_UI_LANGUAGE_RU] = "цвет (текст;выделение[;жирный])",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "systemcolor (fg;background[;highlight][;bold])",
        .description =
            {
                "Customize interface colors (reset with %ssystemcolor reset).",
                "인터페이스 색상을 조정합니다 (%ssystemcolor reset으로 "
                "초기화).",
                "インターフェースの色を調整します（%ssystemcolor reset "
                "で初期化）。",
                "自定义界面颜色（用 %ssystemcolor reset 重置）。",
                "Настроить цвета интерфейса (сброс — %ssystemcolor reset).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] =
                    "시스템색상 (전경;배경[;하이라이트][;굵게])",
                [SESSION_UI_LANGUAGE_JP] =
                    "システムカラー (前景色;背景色[;ハイライト][;太字])",
                [SESSION_UI_LANGUAGE_ZH] = "系统颜色 (前景;背景[;高亮][;粗体])",
                [SESSION_UI_LANGUAGE_RU] =
                    "системныйцвет (передний;фон[;выделение][;жирный])",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "bbs [list|read|post|comment|regen|delete]",
        .description =
            {
                "Open the bulletin board system (finish %s to post).",
                "게시판을 엽니다 (%s 로 입력을 마칩니다).",
                "掲示板を開きます（投稿は %s で終了）。",
                "打开公告板系统（以 %s 结束提交）。",
                "Открыть доску объявлений (завершайте ввод строкой %s).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] =
                    "게시판 [목록|읽기|게시|댓글|갱신|삭제]",
                [SESSION_UI_LANGUAGE_JP] =
                    "掲示板 [一覧|閲覧|投稿|コメント|再掲|削除]",
                [SESSION_UI_LANGUAGE_ZH] =
                    "公告板 [列表|阅读|发布|评论|置顶|删除]",
                [SESSION_UI_LANGUAGE_RU] =
                    "доска [список|читать|пост|коммент|поднять|удалить]",
            },
        .description_arg_count = 1,
        .description_args = {SESSION_HELP_TEMPLATE_ARG_BBS_TERMINATOR},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "bbs topic read <tag>",
        .description =
            {
                "Show posts under a specific topic.",
                "특정 주제의 게시물을 확인합니다.",
                "特定のトピックに属する投稿を表示します。",
                "查看特定主题下的帖子。",
                "Показать записи выбранной темы.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "게시판 주제 읽기 <태그>",
                [SESSION_UI_LANGUAGE_JP] = "掲示板 トピック 閲覧 <タグ>",
                [SESSION_UI_LANGUAGE_ZH] = "公告板 主题 阅读 <标签>",
                [SESSION_UI_LANGUAGE_RU] = "доска тема читать <тег>",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "delete-msg <id|start-end>",
        .description =
            {
                "Delete a message range.",
                "메시지 범위를 삭제합니다.",
                "メッセージの範囲を削除します。",
                "删除一段消息。",
                "Удалить диапазон сообщений.",
            },
        .description_arg_count = 1,
        .description_args = {SESSION_HELP_TEMPLATE_ARG_COMMAND_GOOD},
    },
};

static const session_help_entry_t kSessionHelpExtended[] = {
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "search <text>",
        .description =
            {
                "Search for users whose name matches the text.",
                "이름에 텍스트가 포함된 사용자를 찾습니다.",
                "名前にテキストが含まれるユーザーを検索します。",
                "按名称中包含的文字搜索用户。",
                "Найти пользователей, чьи имена содержат указанный текст.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "검색 <텍스트>",
                [SESSION_UI_LANGUAGE_JP] = "検索 <テキスト>",
                [SESSION_UI_LANGUAGE_ZH] = "搜索 <文本>",
                [SESSION_UI_LANGUAGE_RU] = "поиск <текст>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "status <message|off>",
        .description =
            {
                "Set or clear your status message.",
                "상태 메시지를 설정하거나 지웁니다.",
                "ステータスメッセージを設定または削除します。",
                "设置或清除状态消息。",
                "Установить или очистить статусное сообщение.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "상태 <메시지|끄기>",
                [SESSION_UI_LANGUAGE_JP] = "ステータス <メッセージ|解除>",
                [SESSION_UI_LANGUAGE_ZH] = "状态 <消息|关闭>",
                [SESSION_UI_LANGUAGE_RU] = "статус <сообщение|выкл>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "showstatus <nickname>",
        .description =
            {
                "Show someone's status message.",
                "다른 사용자의 상태 메시지를 확인합니다.",
                "他のユーザーのステータスメッセージを表示します。",
                "查看其他用户的状态消息。",
                "Показать статус другого пользователя.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "상태확인 <닉네임>",
                [SESSION_UI_LANGUAGE_JP] = "ステータス確認 <ニックネーム>",
                [SESSION_UI_LANGUAGE_ZH] = "状态查询 <昵称>",
                [SESSION_UI_LANGUAGE_RU] = "показать-статус <ник>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "image <url> [caption]",
        .description =
            {
                "Share an image link.",
                "이미지 링크를 공유합니다.",
                "画像リンクを共有します。",
                "分享图片链接。",
                "Поделиться ссылкой на изображение.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "이미지 <url> [캡션]",
                [SESSION_UI_LANGUAGE_JP] = "画像 <url> [キャプション]",
                [SESSION_UI_LANGUAGE_ZH] = "图片 <url> [标题]",
                [SESSION_UI_LANGUAGE_RU] = "изображение <url> [подпись]",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "video <url> [caption]",
        .description =
            {
                "Share a video link.",
                "영상 링크를 공유합니다.",
                "動画リンクを共有します。",
                "分享视频链接。",
                "Поделиться ссылкой на видео.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "영상 <url> [캡션]",
                [SESSION_UI_LANGUAGE_JP] = "動画 <url> [キャプション]",
                [SESSION_UI_LANGUAGE_ZH] = "视频 <url> [标题]",
                [SESSION_UI_LANGUAGE_RU] = "видео <url> [подпись]",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "audio <url> [caption]",
        .description =
            {
                "Share an audio clip link.",
                "오디오 링크를 공유합니다.",
                "音声クリップのリンクを共有します。",
                "分享音频链接。",
                "Поделиться ссылкой на аудио.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "오디오 <url> [캡션]",
                [SESSION_UI_LANGUAGE_JP] = "音声 <url> [キャプション]",
                [SESSION_UI_LANGUAGE_ZH] = "音频 <url> [标题]",
                [SESSION_UI_LANGUAGE_RU] = "аудио <url> [подпись]",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "files <url> [caption]",
        .description =
            {
                "Share a downloadable file.",
                "다운로드 가능한 파일을 공유합니다.",
                "ダウンロード可能なファイルを共有します。",
                "分享可下载的文件。",
                "Поделиться загружаемым файлом.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "파일 <url> [캡션]",
                [SESSION_UI_LANGUAGE_JP] = "ファイル <url> [キャプション]",
                [SESSION_UI_LANGUAGE_ZH] = "文件 <url> [标题]",
                [SESSION_UI_LANGUAGE_RU] = "файлы <url> [подпись]",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "filestore",
        .description =
            {
                "List managed storage files.",
                "저장소 파일을 나열합니다.",
                "ストレージのファイル一覧を表示します。",
                "列出存储区中的文件。",
                "Показать список файлов в хранилище.",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "filestore-upload",
        .description =
            {
                "Start a TELNET ZMODEM upload into /etc/ssh-chatter/user-files.",
                "TELNET ZMODEM 업로드를 시작합니다.",
                "TELNET ZMODEM アップロードを開始します。",
                "启动 TELNET ZMODEM 上传。",
                "Запустить загрузку через ZMODEM для TELNET.",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "filestore-download <name>",
        .description =
            {
                "Start a TELNET ZMODEM download of the named file.",
                "지정한 파일을 TELNET ZMODEM으로 내려받습니다.",
                "指定したファイルを TELNET ZMODEM でダウンロードします。",
                "以 TELNET ZMODEM 下载指定文件。",
                "Загрузить указанный файл через ZMODEM (TELNET).",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },

    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "mail [inbox|send <user> <message>|clear]",
        .description =
            {
                "Manage your mailbox.",
                "사서함을 관리합니다.",
                "メールボックスを管理します。",
                "管理你的邮箱。",
                "Управлять почтовым ящиком.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] =
                    "메일 [받은편지함|보내기 <사용자> <메시지>|지우기]",
                [SESSION_UI_LANGUAGE_JP] =
                    "メール [受信箱|送信 <ユーザー> <メッセージ>|クリア]",
                [SESSION_UI_LANGUAGE_ZH] =
                    "邮件 [收件箱|发送 <用户> <消息>|清除]",
                [SESSION_UI_LANGUAGE_RU] =
                    "почта [входящие|отправить <пользователь> "
                    "<сообщение>|очистить]",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },

    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "asciiart",
        .description =
            {
                "Open the ASCII art composer (max 128 lines, 1/10 min per IP).",
                "ASCII 아트 작성기를 엽니다 (최대 128줄, IP당 10분에 1회).",
                "ASCII "
                "アート作成ツールを開きます（最大128行、IPごと10分に1回）。",
                "打开 ASCII 艺术编辑器（最多128行，每个 IP 10 分钟一次）。",
                "Открыть редактор ASCII-арта (до 128 строк, раз в 10 минут на "
                "IP).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "아스키아트",
                [SESSION_UI_LANGUAGE_JP] = "アスキーアート",
                [SESSION_UI_LANGUAGE_ZH] = "艺术",
                [SESSION_UI_LANGUAGE_RU] = "ASCIIарт",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "color (text;highlight[;bold])",
        .description =
            {
                "Style your handle.",
                "사용자 이름 색상을 꾸밉니다.",
                "ハンドル名の配色を設定します。",
                "设置昵称的配色。",
                "Настроить оформление вашего ника.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "색상 (텍스트;하이라이트[;굵게])",
                [SESSION_UI_LANGUAGE_JP] = "色 (テキスト;ハイライト[;太字])",
                [SESSION_UI_LANGUAGE_ZH] = "颜色 (文本;高亮[;粗体])",
                [SESSION_UI_LANGUAGE_RU] = "цвет (текст;выделение[;жирный])",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "systemcolor (fg;background[;highlight][;bold])",
        .description =
            {
                "Customize interface colors (reset with %ssystemcolor reset).",
                "인터페이스 색상을 조정합니다 (%ssystemcolor reset으로 "
                "초기화).",
                "インターフェースの色を調整します（%ssystemcolor reset "
                "で初期化）。",
                "自定义界面颜色（用 %ssystemcolor reset 重置）。",
                "Настроить цвета интерфейса (сброс — %ssystemcolor reset).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] =
                    "시스템색상 (전경;배경[;하이라이트][;굵게])",
                [SESSION_UI_LANGUAGE_JP] =
                    "システムカラー (前景色;背景色[;ハイライト][;太字])",
                [SESSION_UI_LANGUAGE_ZH] = "系统颜色 (前景;背景[;高亮][;粗体])",
                [SESSION_UI_LANGUAGE_RU] =
                    "системныйцвет (передний;фон[;выделение][;жирный])",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "set-trans-lang <language|off>",
        .description =
            {
                "Translate terminal output to a language.",
                "터미널 출력 번역 대상 언어를 지정합니다.",
                "端末出力の翻訳先を指定します。",
                "设置终端输出的翻译语言。",
                "Задать язык для перевода терминальных сообщений.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "번역언어설정 <언어|끄기>",
                [SESSION_UI_LANGUAGE_JP] = "翻訳言語設定 <言語|オフ>",
                [SESSION_UI_LANGUAGE_ZH] = "设置翻译语言 <语言|关闭>",
                [SESSION_UI_LANGUAGE_RU] =
                    "установить-язык-перевода <язык|выкл>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "set-target-lang <language|off>",
        .description =
            {
                "Translate your outgoing messages.",
                "내보내는 메시지를 번역합니다.",
                "自分の送信メッセージを翻訳します。",
                "翻译你发送的消息。",
                "Переводить исходящие сообщения.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "대상언어설정 <언어|끄기>",
                [SESSION_UI_LANGUAGE_JP] = "ターゲット言語設定 <言語|オフ>",
                [SESSION_UI_LANGUAGE_ZH] = "设置目标语言 <语言|关闭>",
                [SESSION_UI_LANGUAGE_RU] =
                    "установить-целевой-язык <язык|выкл>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "weather <region> <city>",
        .description =
            {
                "Show weather for a region and city.",
                "지역과 도시의 날씨를 보여줍니다.",
                "地域と都市の天気を表示します。",
                "显示指定地区和城市的天气。",
                "Показать погоду для региона и города.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "날씨 <지역> <도시>",
                [SESSION_UI_LANGUAGE_JP] = "天気 <地域> <都市>",
                [SESSION_UI_LANGUAGE_ZH] = "天气 <地区> <城市>",
                [SESSION_UI_LANGUAGE_RU] = "погода <регион> <город>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "translate <on|off>",
        .description =
            {
                "Enable or disable translation after configuring languages.",
                "언어를 설정한 후 번역 기능을 켜거나 끕니다.",
                "言語設定後に翻訳機能を有効/無効にします。",
                "在设定语言后开启或关闭翻译。",
                "Включить или отключить перевод после настройки языков.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "번역 <켜기|끄기>",
                [SESSION_UI_LANGUAGE_JP] = "翻訳 <オン|オフ>",
                [SESSION_UI_LANGUAGE_ZH] = "翻译 <开|关>",
                [SESSION_UI_LANGUAGE_RU] = "перевод <вкл|выкл>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "morse <on <filter>|off|status>",
        .description =
            {
                "Toggle the Morse relay feed and optionally filter messages. Commands stay in English.",
                "모스 릴레이 피드를 켜거나 끄고, 문자열로 선택적으로 필터링합니다. 명령어는 영어로만 제공됩니다.",
                "モールス中継フィードをオン/オフし、文字列で任意にフィルターします。コマンドは英語のみです。",
                "莫尔斯中继提要的开关，并可选按字符串过滤。命令仅提供英文。",
                "Включить или отключить канал Морзе и при желании фильтровать строки. Команды доступны только на английском.",
                "Morse-Relais ein-/ausschalten und optional nach Zeichenfolgen filtern. Befehle sind nur auf Englisch verfügbar.",
                "Activer ou désactiver le flux Morse et filtrer éventuellement par chaîne. Les commandes restent en anglais.",
                "Włącz/wyłącz kanał Morse i opcjonalnie filtruj wiadomości po tekście. Polecenia są dostępne wyłącznie po angielsku.",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "morse-chat <text>",
        .description =
            {
                "Send a line to the Morse relay (ASCII/English only).",
                "모스 릴레이로 한 줄을 보냅니다 (ASCII/영문 전용).",
                "モールス中継に1行送信します（ASCII/英語のみ）。",
                "向莫尔斯中继发送一行（仅限 ASCII/英语）。",
                "Отправить строку в ретранслятор Морзе (только ASCII/английский).",
                "Sende eine Zeile an das Morse-Relais (nur ASCII/Englisch).",
                "Envoyer une ligne vers le relais Morse (ASCII/anglais uniquement).",
                "Wyślij wiersz do przekaźnika Morse'a (tylko ASCII/angielski).",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "morse-reply <text>",
        .description =
            {
                "Reply back over the Morse relay (ASCII/English only).",
                "모스 릴레이로 답신을 보냅니다 (ASCII/영문 전용).",
                "モールス中継に返信します（ASCII/英語のみ）。",
                "通过莫尔斯中继回复（仅限 ASCII/英语）。",
                "Ответить через ретранслятор Морзе (только ASCII/английский).",
                "Über das Morse-Relais antworten (nur ASCII/Englisch).",
                "Répondre via le relais Morse (ASCII/anglais uniquement).",
                "Odpowiedz przez przekaźnik Morse'a (tylko ASCII/angielski).",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "breaking <on|off>",
        .description =
            {
                "Choose whether blue-and-magenta breaking alerts are shown.",
                "파랑 배경 마젠타 속보 알림 표시 여부를 선택합니다.",
                "青地にマゼンタの速報通知を表示するか選択します。",
                "选择是否显示蓝底洋红色的快讯提示。",
                "Выбрать показ синих оповещений о срочных новостях.",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },

    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "chat-spacing <0-5>",
        .description =
            {
                "Reserve blank lines before translated captions in chat.",
                "번역 자막 앞에 공백 줄을 예약합니다.",
                "翻訳キャプション前に空行を確保します。",
                "在聊天翻译字幕前预留空行。",
                "Резервировать пустые строки перед переводами в чате.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "채팅간격 <0-5>",
                [SESSION_UI_LANGUAGE_JP] = "チャット間隔 <0-5>",
                [SESSION_UI_LANGUAGE_ZH] = "聊天间距 <0-5>",
                [SESSION_UI_LANGUAGE_RU] = "интервал-чата <0-5>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "palette <name>",
        .description =
            {
                "Apply a predefined interface palette (%spalette list).",
                "미리 정의된 팔레트를 적용합니다 (%spalette list 참고).",
                "定義済みの配色を適用します（%spalette list を参照）。",
                "应用预设的界面配色（参见 %spalette list）。",
                "Применить готовую палитру интерфейса (см. %spalette list).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "팔레트 <이름>",
                [SESSION_UI_LANGUAGE_JP] = "パレット <名前>",
                [SESSION_UI_LANGUAGE_ZH] = "调色板 <名称>",
                [SESSION_UI_LANGUAGE_RU] = "палитра <имя>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },

    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "date <timezone>",
        .description =
            {
                "View the server time in another timezone.",
                "다른 시간대의 서버 시간을 확인합니다.",
                "別のタイムゾーンでサーバー時刻を表示します。",
                "查看其他时区的服务器时间。",
                "Показать серверное время в другом часовом поясе.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "날짜 <시간대>",
                [SESSION_UI_LANGUAGE_JP] = "日付 <タイムゾーン>",
                [SESSION_UI_LANGUAGE_ZH] = "日期 <时区>",
                [SESSION_UI_LANGUAGE_RU] = "дата <часовойпояс>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "os <name>",
        .description =
            {
                "Record the operating system you use.",
                "사용 중인 운영체제를 기록합니다.",
                "使用中のOSを記録します。",
                "记录你使用的操作系统。",
                "Сохранить информацию о вашей ОС.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "운영체제 <이름>",
                [SESSION_UI_LANGUAGE_JP] = "OS <名前>",
                [SESSION_UI_LANGUAGE_ZH] = "操作系统 <名称>",
                [SESSION_UI_LANGUAGE_RU] = "ОС <имя>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "getos <username>",
        .description =
            {
                "Look up someone else's recorded operating system.",
                "다른 사용자가 기록한 운영체제를 확인합니다.",
                "他のユーザーが登録したOSを確認します。",
                "查看他人记录的操作系统。",
                "Посмотреть, какую ОС указал другой пользователь.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "운영체제확인 <사용자이름>",
                [SESSION_UI_LANGUAGE_JP] = "OS取得 <ユーザー名>",
                [SESSION_UI_LANGUAGE_ZH] = "获取操作系统 <用户名>",
                [SESSION_UI_LANGUAGE_RU] = "получить-ОС <имяпользователя>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "birthday YYYY-MM-DD",
        .description =
            {
                "Register your birthday.",
                "생일을 등록합니다.",
                "誕生日を登録します。",
                "登记你的生日。",
                "Зарегистрировать дату рождения.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "생일 YYYY-MM-DD",
                [SESSION_UI_LANGUAGE_JP] = "誕生日 YYYY-MM-DD",
                [SESSION_UI_LANGUAGE_ZH] = "生日 YYYY-MM-DD",
                [SESSION_UI_LANGUAGE_RU] = "деньрождения ГГГГ-ММ-ДД",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },

    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "connected",
        .description =
            {
                "Privately list everyone connected.",
                "현재 접속 중인 사용자 목록을 비공개로 확인합니다.",
                "接続中のユーザーを自分だけに一覧表示します。",
                "私下查看所有在线用户。",
                "Получить приватный список всех подключённых.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "접속자",
                [SESSION_UI_LANGUAGE_JP] = "接続中",
                [SESSION_UI_LANGUAGE_ZH] = "在线",
                [SESSION_UI_LANGUAGE_RU] = "подключенные",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "alpha-centauri-landers",
        .description =
            {
                "View the Immigrants' Flag hall of fame.",
                "Immigrants' Flag 명예의 전당을 확인합니다.",
                "Immigrants' Flag 殿堂を表示します。",
                "查看 Immigrants' Flag 名人堂。",
                "Открыть зал славы Immigrants' Flag.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "알파센타우리착륙자",
                [SESSION_UI_LANGUAGE_JP] = "アルファケンタウリ着陸者",
                [SESSION_UI_LANGUAGE_ZH] = "半人马座阿尔法星登陆者",
                [SESSION_UI_LANGUAGE_RU] = "альфа-центавра-посадочные",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },

    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "poll <question>|<option1>|<option2>|...",
        .description =
            {
                "Start or close the global poll (operators can start).",
                "전역 투표를 시작하거나 종료합니다 (시작은 운영자만 가능).",
                "グローバル投票を開始または終了します（開始は運営のみ）。",
                "开始或结束全局投票（仅管理员可开始）。",
                "Запустить или завершить общий опрос (запуск только для оператора).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] =
                    "전역투표 <질문>|<옵션1>|<옵션2>|...",
                [SESSION_UI_LANGUAGE_JP] =
                    "グローバル投票 <質問>|<選択肢1>|<選択肢2>|...",
                [SESSION_UI_LANGUAGE_ZH] = "全局投票 <问题>|<选项1>|<选项2>|...",
                [SESSION_UI_LANGUAGE_RU] =
                    "опрос <вопрос>|<вариант1>|<вариант2>|...",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "vote <label> <question>|<option1>|<option2>|...",
        .description =
            {
                "Start or manage a named poll.",
                "라벨 투표를 시작하거나 관리합니다.",
                "ラベル付き投票を開始または管理します。",
                "开始或管理命名投票。",
                "Запустить или управлять именованным опросом.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] =
                    "투표 <라벨> <질문>|<옵션1>|<옵션2>|...",
                [SESSION_UI_LANGUAGE_JP] =
                    "投票 <ラベル> <質問>|<選択肢1>|<選択肢2>|...",
                [SESSION_UI_LANGUAGE_ZH] = "投票 <标签> <问题>|<选项1>|<选项2>|...",
                [SESSION_UI_LANGUAGE_RU] =
                    "голосование <метка> <вопрос>|<вариант1>|<вариант2>|...",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "elect <label> <choice>",
        .description =
            {
                "Vote in a named poll by label.",
                "라벨로 지정된 투표에 참여합니다.",
                "ラベルを指定して名前付き投票に投票します。",
                "按标签在命名投票中投票。",
                "Проголосовать в именованном опросе по метке.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "선택 <라벨> <선택>",
                [SESSION_UI_LANGUAGE_JP] = "選択 <ラベル> <選択肢>",
                [SESSION_UI_LANGUAGE_ZH] = "选择 <标签> <选项>",
                [SESSION_UI_LANGUAGE_RU] = "выбрать <метка> <выбор>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "poke <username>",
        .description =
            {
                "Send a bell to call a user.",
                "사용자를 호출하는 종소리를 보냅니다.",
                "ユーザーを呼び出すベルを送ります。",
                "向用户发送提醒铃声。",
                "Отправить звуковой сигнал пользователю.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "호출 <사용자이름>",
                [SESSION_UI_LANGUAGE_JP] = "つつく <ユーザー名>",
                [SESSION_UI_LANGUAGE_ZH] = "戳 <用户名>",
                [SESSION_UI_LANGUAGE_RU] = "пинг <имяпользователя>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "block <user|ip>",
        .description =
            {
                "Hide messages from a user or IP locally (%sblock list to "
                "review).",
                "사용자 또는 IP의 메시지를 차단합니다 (%sblock list로 확인).",
                "ユーザーやIPのメッセージをローカルで非表示にします（確認は "
                "%sblock list）。",
                "本地屏蔽某用户或 IP 的消息（用 %sblock list 查看）。",
                "Скрыть сообщения пользователя или IP локально (проверка через "
                "%sblock list).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "차단 <사용자|IP>",
                [SESSION_UI_LANGUAGE_JP] = "ブロック <ユーザー|IP>",
                [SESSION_UI_LANGUAGE_ZH] = "屏蔽 <用户|IP>",
                [SESSION_UI_LANGUAGE_RU] = "заблокировать <пользователь|IP>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "unblock <target|all>",
        .description =
            {
                "Remove a local block entry.",
                "로컬 차단을 해제합니다.",
                "ローカルのブロックを解除します。",
                "解除本地屏蔽。",
                "Удалить локальную блокировку.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "차단해제 <대상|모두>",
                [SESSION_UI_LANGUAGE_JP] = "ブロック解除 <ターゲット|すべて>",
                [SESSION_UI_LANGUAGE_ZH] = "解除屏蔽 <目标|全部>",
                [SESSION_UI_LANGUAGE_RU] = "разблокировать <цель|все>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_FORMATTED,
        .label = "%sgood|%ssad|%scool|%sangry|%schecked|%slove|%swtf <id>",
        .description =
            {
                "React to a message by number.",
                "번호로 메시지에 반응합니다.",
                "番号を指定してメッセージにリアクションします。",
                "按编号为消息添加表情反应。",
                "Реагировать на сообщение по номеру.",
            },
        .label_arg_count = 7U,
        .label_args =
            {
                SESSION_HELP_TEMPLATE_ARG_PREFIX,
                SESSION_HELP_TEMPLATE_ARG_PREFIX,
                SESSION_HELP_TEMPLATE_ARG_PREFIX,
                SESSION_HELP_TEMPLATE_ARG_PREFIX,
                SESSION_HELP_TEMPLATE_ARG_PREFIX,
                SESSION_HELP_TEMPLATE_ARG_PREFIX,
                SESSION_HELP_TEMPLATE_ARG_PREFIX,
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "좋아요|/슬퍼요|/멋져요|/화나요|/"
                                           "확인|/사랑해요|/어쩌라고 <id>",
                [SESSION_UI_LANGUAGE_JP] = "いいね|/かなしい|/クール|/怒り|/"
                                           "確認済み|/愛|/なんだと <id>",
                [SESSION_UI_LANGUAGE_ZH] =
                    "点赞|/难过|/酷|/生气|/已检查|/爱|/搞什么 <id>",
                [SESSION_UI_LANGUAGE_RU] = "класс|/грусть|/круто|/злой|/"
                                           "проверено|/любовь|/чтоэто <id>",
            },
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "rss list",
        .description =
            {
                "List saved RSS feeds.",
                "저장된 RSS 피드를 나열합니다.",
                "保存された RSS フィードを一覧表示します。",
                "列出已保存的 RSS 源。",
                "Показать сохранённые RSS-ленты.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "rss 목록",
                [SESSION_UI_LANGUAGE_JP] = "rss 一覧",
                [SESSION_UI_LANGUAGE_ZH] = "rss 列表",
                [SESSION_UI_LANGUAGE_RU] = "rss список",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "rss read <tag>",
        .description =
            {
                "Open a saved feed in the inline reader.",
                "저장된 피드를 인라인 리더로 엽니다.",
                "保存されたフィードをインラインリーダーで開きます。",
                "在内嵌阅读器中打开已保存的源。",
                "Открыть сохранённую ленту во встроенном ридере.",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "rss 읽기 <태그>",
                [SESSION_UI_LANGUAGE_JP] = "rss 読む <タグ>",
                [SESSION_UI_LANGUAGE_ZH] = "rss 阅读 <标签>",
                [SESSION_UI_LANGUAGE_RU] = "rss читать <тег>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "retro <on [ko|en|jp|zh|ru|de|fr|pl]|off|auto|status>",
        .description =
            {
                "Toggle retro CP437 terminal encoding with optional language.",
                "선택적 언어와 함께 레트로 CP437 터미널 인코딩을 전환합니다.",
                "オプションの言語でレトロ CP437 "
                "ターミナルエンコーディングを切り替えます。",
                "使用可选语言切换复古 CP437 终端编码。",
                "Переключить ретро CP437 кодировку терминала с необязательным "
                "языком.",
                "Retro CP437-Terminalcodierung mit optionaler Sprache "
                "umschalten.",
                "Basculer l'encodage de terminal rétro CP437 avec une langue "
                "optionnelle.",
                "Przełącz kodowanie terminala retro CP437 z opcjonalnym "
                "językiem.",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "suspend!",
        .description =
            {
                "Suspend the active game (Ctrl+Z while playing).",
                "진행 중인 게임을 일시 중단합니다 (플레이 중 Ctrl+Z).",
                "進行中のゲームを一時停止します（プレイ中に Ctrl+Z）。",
                "暂停正在进行的游戏（游戏中按 Ctrl+Z）。",
                "Приостановить активную игру (Ctrl+Z во время игры).",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
};

static const session_help_entry_t kSessionHelpOperator[] = {
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "translate-scope <chat|chat-nohistory|all>",
        .description =
            {
                "Limit translation scope (operators only).",
                "번역 범위를 제한합니다 (운영자 전용).",
                "翻訳対象範囲を制限します（オペレーター専用）。",
                "限制翻译范围（仅限管理员）。",
                "Ограничить область перевода (только для операторов).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "번역범위 <채팅|채팅기록없음|모두>",
                [SESSION_UI_LANGUAGE_JP] =
                    "翻訳範囲 <チャット|チャット履歴なし|すべて>",
                [SESSION_UI_LANGUAGE_ZH] = "翻译范围 <聊天|无聊天记录|全部>",
                [SESSION_UI_LANGUAGE_RU] =
                    "область-перевода <чат|чат-без-истории|все>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "gemini <on|off>",
        .description =
            {
                "Toggle Gemini provider (operator only).",
                "Gemini 제공자를 전환합니다 (운영자 전용).",
                "Gemini プロバイダーを切り替えます（オペレーター専用）。",
                "切换 Gemini 提供方（仅限管理员）。",
                "Включить/выключить провайдер Gemini (оператор).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "제미니 <켜기|끄기>",
                [SESSION_UI_LANGUAGE_JP] = "ジェミニ <オン|オフ>",
                [SESSION_UI_LANGUAGE_ZH] = "Gemini <开|关>",
                [SESSION_UI_LANGUAGE_RU] = "джемини <вкл|выкл>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "gemini-unfreeze",
        .description =
            {
                "Clear automatic Gemini cooldown (operator only).",
                "Gemini 자동 쿨다운을 해제합니다 (운영자 전용).",
                "Gemini の自動クールダウンを解除します（オペレーター専用）。",
                "清除 Gemini 自动冷却（仅限管理员）。",
                "Снять автоматическую задержку Gemini (оператор).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "제미니-해제",
                [SESSION_UI_LANGUAGE_JP] = "ジェミニ-解除",
                [SESSION_UI_LANGUAGE_ZH] = "Gemini-解冻",
                [SESSION_UI_LANGUAGE_RU] = "джемини-разморозить",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "captcha <on|off>",
        .description =
            {
                "Toggle captcha requirement (operator only).",
                "캡차 요구를 전환합니다 (운영자 전용).",
                "CAPTCHA の必須設定を切り替えます（オペレーター専用）。",
                "切换验证码要求（仅限管理员）。",
                "Включить/выключить требование капчи (оператор).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "캡차 <켜기|끄기>",
                [SESSION_UI_LANGUAGE_JP] = "キャプチャ <オン|オフ>",
                [SESSION_UI_LANGUAGE_ZH] = "验证码 <开|关>",
                [SESSION_UI_LANGUAGE_RU] = "капча <вкл|выкл>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },

    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "grant <ip>",
        .description =
            {
                "Grant operator access to an IP (LAN only).",
                "IP에 운영자 권한을 부여합니다 (LAN 한정).",
                "IP にオペレーター権限を付与します（LAN 限定）。",
                "为 IP 授予管理员权限（仅限局域网）。",
                "Выдать операторские права IP-адресу (только LAN).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "권한부여 <IP>",
                [SESSION_UI_LANGUAGE_JP] = "許可 <IP>",
                [SESSION_UI_LANGUAGE_ZH] = "授予 <IP>",
                [SESSION_UI_LANGUAGE_RU] = "выдать <IP>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "revoke <ip>",
        .description =
            {
                "Revoke an IP's operator access (LAN top admin).",
                "IP의 운영자 권한을 회수합니다 (LAN 최고 관리자).",
                "IP のオペレーター権限を剥奪します（LAN トップ管理者）。",
                "撤销 IP 的管理员权限（局域网最高管理员）。",
                "Отозвать операторские права IP (только старший LAN-админ).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "권한해제 <IP>",
                [SESSION_UI_LANGUAGE_JP] = "取り消し <IP>",
                [SESSION_UI_LANGUAGE_ZH] = "撤销 <IP>",
                [SESSION_UI_LANGUAGE_RU] = "отозвать <IP>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "kick <username>",
        .description =
            {
                "Disconnect a user (operator only).",
                "사용자를 강제로 종료합니다 (운영자 전용).",
                "ユーザーを切断します（オペレーター専用）。",
                "断开某位用户（仅限管理员）。",
                "Отключить пользователя (operator).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "강퇴 <사용자이름>",
                [SESSION_UI_LANGUAGE_JP] = "キック <ユーザー名>",
                [SESSION_UI_LANGUAGE_ZH] = "踢出 <用户名>",
                [SESSION_UI_LANGUAGE_RU] = "кик <имяпользователя>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "ban <username>",
        .description =
            {
                "Ban a user (operator only).",
                "사용자를 차단합니다 (운영자 전용).",
                "ユーザーを追放します（オペレーター専用）。",
                "封禁用户（仅限管理员）。",
                "Забанить пользователя (оператор).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "밴 <사용자이름>",
                [SESSION_UI_LANGUAGE_JP] = "バン <ユーザー名>",
                [SESSION_UI_LANGUAGE_ZH] = "封禁 <用户名>",
                [SESSION_UI_LANGUAGE_RU] = "бан <имяпользователя>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "banname <nickname>",
        .description =
            {
                "Block a nickname (operator only).",
                "닉네임을 차단합니다 (운영자 전용).",
                "ニックネームを禁止します（オペレーター専用）。",
                "屏蔽昵称（仅限管理员）。",
                "Заблокировать ник (оператор).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "닉차단 <닉네임>",
                [SESSION_UI_LANGUAGE_JP] = "ニックネ禁止 <ニックネーム>",
                [SESSION_UI_LANGUAGE_ZH] = "屏蔽昵称 <昵称>",
                [SESSION_UI_LANGUAGE_RU] = "забанить-ник <ник>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "banlist",
        .description =
            {
                "List active bans (operator only).",
                "활성화된 차단 목록을 봅니다 (운영자 전용).",
                "現在のBANを一覧表示します（オペレーター専用）。",
                "列出当前生效的封禁（仅限管理员）。",
                "Показать активные баны (оператор).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "밴목록",
                [SESSION_UI_LANGUAGE_JP] = "バンリスト",
                [SESSION_UI_LANGUAGE_ZH] = "封禁列表",
                [SESSION_UI_LANGUAGE_RU] = "список-банов",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "delete-msg <id|start-end>",
        .description =
            {
                "Remove chat history messages (operator only).",
                "채팅 기록 메시지를 삭제합니다 (운영자 전용).",
                "チャット履歴のメッセージを削除します（オペレーター専用）。",
                "删除聊天记录中的消息（仅限管理员）。",
                "Удалить сообщения из истории чата (operator).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "메시지삭제 <ID|시작-끝>",
                [SESSION_UI_LANGUAGE_JP] = "メッセージ削除 <ID|開始-終了>",
                [SESSION_UI_LANGUAGE_ZH] = "删除消息 <ID|开始-结束>",
                [SESSION_UI_LANGUAGE_RU] =
                    "удалить-сообщение <ID|начало-конец>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "pardon <user|ip>",
        .description =
            {
                "Remove a ban (operator only).",
                "차단을 해제합니다 (운영자 전용).",
                "BAN を解除します（オペレーター専用）。",
                "解除封禁（仅限管理员）。",
                "Снять бан (operator).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "사면 <사용자|IP>",
                [SESSION_UI_LANGUAGE_JP] = "許し <ユーザー|IP>",
                [SESSION_UI_LANGUAGE_ZH] = "赦免 <用户|IP>",
                [SESSION_UI_LANGUAGE_RU] = "помиловать <пользователь|IP>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "rss add <url> <tag>",
        .description =
            {
                "Register a feed (operator only).",
                "피드를 등록합니다 (운영자 전용).",
                "フィードを登録します（オペレーター専用）。",
                "注册新的源（仅限管理员）。",
                "Добавить ленту (оператор).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "rss 추가 <url> <태그>",
                [SESSION_UI_LANGUAGE_JP] = "rss 追加 <url> <タグ>",
                [SESSION_UI_LANGUAGE_ZH] = "rss 添加 <url> <标签>",
                [SESSION_UI_LANGUAGE_RU] = "rss добавить <url> <тег>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "rss del <tag>",
        .description =
            {
                "Delete a feed (operator only).",
                "피드를 삭제합니다 (운영자 전용).",
                "フィードを削除します（オペレーター専用）。",
                "删除源（仅限管理员）。",
                "Удалить ленту (operator).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "rss 삭제 <태그>",
                [SESSION_UI_LANGUAGE_JP] = "rss 削除 <タグ>",
                [SESSION_UI_LANGUAGE_ZH] = "rss 删除 <标签>",
                [SESSION_UI_LANGUAGE_RU] = "rss удалить <тег>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
    {
        .kind = SESSION_HELP_ENTRY_COMMAND,
        .label = "getaddr <username>",
        .description =
            {
                "Look up a user's last known address (operator only).",
                "사용자의 마지막 접속 주소를 확인합니다 (운영자 전용).",
                "ユーザーの最新の接続アドレスを確認します（オペレーター専用）"
                "。",
                "查看用户最近的连接地址（仅限管理员）。",
                "Посмотреть последний известный адрес пользователя (operator).",
            },
        .label_translations =
            {
                [SESSION_UI_LANGUAGE_KO] = "주소확인 <사용자이름>",
                [SESSION_UI_LANGUAGE_JP] = "アドレス取得 <ユーザー名>",
                [SESSION_UI_LANGUAGE_ZH] = "获取地址 <用户名>",
                [SESSION_UI_LANGUAGE_RU] = "получить-адрес <имяпользователя>",
            },
        .label_arg_count = 0U,
        .label_args = {},
    },
};

static const int kSessionHelpLabelWidth = 26;

session_ui_language_t session_ui_language_from_code(const char *code)
{
    if (code == nullptr || code[0] == '\0') {
        return SESSION_UI_LANGUAGE_COUNT;
    }

    for (size_t idx = 0; idx < SESSION_UI_LANGUAGE_COUNT; ++idx) {
        if (strcasecmp(code, kSessionUiLanguageCodes[idx]) == 0) {
            return (session_ui_language_t)idx;
        }
    }

    return SESSION_UI_LANGUAGE_COUNT;
}

static const char *session_ui_language_code(session_ui_language_t language)
{
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }
    return kSessionUiLanguageCodes[language];
}

static const char *session_ui_language_name(session_ui_language_t language,
                                            session_ui_language_t locale)
{
    if (locale < 0 || locale >= SESSION_UI_LANGUAGE_COUNT) {
        locale = SESSION_UI_LANGUAGE_KO;
    }
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }
    return kSessionUiLanguageNames[locale][language];
}

static const session_ui_locale_t *
session_ui_get_locale(const session_ctx_t *ctx)
{
    session_ui_language_t language = SESSION_UI_LANGUAGE_KO;
    if (ctx != nullptr) {
        language = ctx->ui_language;
    }
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }

    for (size_t idx = 0; idx < SESSION_UI_LANGUAGE_COUNT; ++idx) {
        if (kSessionUiLocales[idx].language == language) {
            return &kSessionUiLocales[idx];
        }
    }

    return &kSessionUiLocales[SESSION_UI_LANGUAGE_KO];
}

static void session_dispatch_command(session_ctx_t *ctx, const char *line);
static void session_handle_mode(session_ctx_t *ctx, const char *arguments);

static const char *session_command_prefix(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return "/";
    }
    return ctx->input_mode == SESSION_INPUT_MODE_COMMAND ? "" : "/";
}

static bool session_try_localized_command_forward(session_ctx_t *ctx,
                                                  const char *line)
{
    if (ctx == nullptr || line == nullptr || ctx->ops == nullptr ||
        ctx->ops->dispatch_command == nullptr || ctx->ops->handle_mode == nullptr) {
        return false;
    }

    const session_ui_locale_t *locale = session_ui_get_locale(ctx);
    const char *command_label =
        (locale != nullptr && locale->mode_label_command != nullptr &&
         locale->mode_label_command[0] != '\0')
            ? locale->mode_label_command
            : "command";

    char localized_prefix[128];
    int prefix_len = snprintf(localized_prefix, sizeof(localized_prefix), "/%s",
                              command_label);
    if (prefix_len <= 0 || (size_t)prefix_len >= sizeof(localized_prefix)) {
        return false;
    }

    if (strncmp(line, localized_prefix, (size_t)prefix_len) != 0) {
        return false;
    }

    const char *remainder = line + prefix_len;
    while (*remainder == ' ' || *remainder == '\t') {
        ++remainder;
    }

    if (*remainder == '\0') {
        ctx->ops->handle_mode(ctx, command_label);
        return true;
    }

    if (*remainder == '/') {
        ctx->ops->dispatch_command(ctx, remainder);
        return true;
    }

    char forwarded[SSH_CHATTER_MAX_INPUT_LEN];
    forwarded[0] = '/';
    size_t copy_len = strnlen(remainder, sizeof(forwarded) - 2U);
    memcpy(&forwarded[1], remainder, copy_len);
    forwarded[copy_len + 1U] = '\0';
    ctx->ops->dispatch_command(ctx, forwarded);
    return true;
}

static session_ui_language_t
session_ui_language_current(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return SESSION_UI_LANGUAGE_KO;
    }
    session_ui_language_t language = ctx->ui_language;
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }
    return language;
}

static const char *
session_asciiart_terminator_for_language(session_ui_language_t language)
{
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }
    const char *terminator = kSessionAsciiartTerminators[language];
    if (terminator != nullptr && terminator[0] != '\0') {
        return terminator;
    }
    const char *fallback = kSessionAsciiartTerminators[SESSION_UI_LANGUAGE_KO];
    return (fallback != nullptr && fallback[0] != '\0')
               ? fallback
               : SSH_CHATTER_ASCIIART_TERMINATOR_EN;
}

static const char *
session_bbs_terminator_for_language(session_ui_language_t language)
{
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }
    const char *terminator = kSessionBbsTerminators[language];
    if (terminator != nullptr && terminator[0] != '\0') {
        return terminator;
    }
    const char *fallback = kSessionBbsTerminators[SESSION_UI_LANGUAGE_KO];
    return (fallback != nullptr && fallback[0] != '\0')
               ? fallback
               : SSH_CHATTER_BBS_TERMINATOR_EN;
}

static const char *session_asciiart_terminator(const session_ctx_t *ctx)
{
    return session_asciiart_terminator_for_language(
        session_ui_language_current(ctx));
}

static const char *session_bbs_terminator(const session_ctx_t *ctx)
{
    return session_bbs_terminator_for_language(
        session_ui_language_current(ctx));
}

static const char *session_editor_terminator(const session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return SSH_CHATTER_BBS_TERMINATOR_EN;
    }

    if (ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART) {
        return session_asciiart_terminator(ctx);
    }

    return session_bbs_terminator(ctx);
}

static bool session_asciiart_matches_terminator(const char *line)
{
    if (line == nullptr) {
        return false;
    }
    for (size_t idx = 0; idx < SESSION_UI_LANGUAGE_COUNT; ++idx) {
        const char *terminator = session_asciiart_terminator_for_language(
            (session_ui_language_t)idx);
        if (terminator != nullptr && strcmp(line, terminator) == 0) {
            return true;
        }
    }
    return false;
}

static bool session_bbs_matches_terminator(const char *line)
{
    if (line == nullptr) {
        return false;
    }
    for (size_t idx = 0; idx < SESSION_UI_LANGUAGE_COUNT; ++idx) {
        const char *terminator =
            session_bbs_terminator_for_language((session_ui_language_t)idx);
        if (terminator != nullptr && strcmp(line, terminator) == 0) {
            return true;
        }
    }
    return false;
}

static const char *
session_command_alias_preferred_by_canonical(const session_ctx_t *ctx,
                                             const char *canonical);

static const session_bbs_subcommand_alias_t *
session_bbs_subcommand_lookup(const char *canonical)
{
    if (canonical == nullptr || canonical[0] == '\0') {
        return nullptr;
    }
    for (size_t idx = 0; idx < kSessionBbsSubcommandCount; ++idx) {
        if (strcmp(kSessionBbsSubcommands[idx].canonical, canonical) == 0) {
            return &kSessionBbsSubcommands[idx];
        }
    }
    return nullptr;
}

static const char *
session_bbs_subcommand_localized(const session_bbs_subcommand_alias_t *alias,
                                 session_ui_language_t language)
{
    if (alias == nullptr) {
        return nullptr;
    }
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }
    const char *localized = alias->localized[language];
    if (localized != nullptr && localized[0] != '\0') {
        return localized;
    }
    return nullptr;
}

static const char *session_bbs_subcommand_preferred(const session_ctx_t *ctx,
                                                    const char *canonical)
{
    const session_bbs_subcommand_alias_t *alias =
        session_bbs_subcommand_lookup(canonical);
    if (alias == nullptr) {
        return canonical;
    }
    const char *localized = session_bbs_subcommand_localized(
        alias, session_ui_language_current(ctx));
    if (localized != nullptr) {
        return localized;
    }
    return alias->canonical;
}

static const char *session_bbs_subcommand_canonicalize(const session_ctx_t *ctx,
                                                       const char *command)
{
    if (command == nullptr || command[0] == '\0') {
        return nullptr;
    }

    for (size_t idx = 0; idx < kSessionBbsSubcommandCount; ++idx) {
        if (strcmp(command, kSessionBbsSubcommands[idx].canonical) == 0) {
            return kSessionBbsSubcommands[idx].canonical;
        }
    }

    session_ui_language_t language = session_ui_language_current(ctx);
    for (size_t idx = 0; idx < kSessionBbsSubcommandCount; ++idx) {
        const char *localized = session_bbs_subcommand_localized(
            &kSessionBbsSubcommands[idx], language);
        if (localized != nullptr && strcmp(command, localized) == 0) {
            return kSessionBbsSubcommands[idx].canonical;
        }
    }

    for (size_t idx = 0; idx < kSessionBbsSubcommandCount; ++idx) {
        for (size_t lang = 0; lang < SESSION_UI_LANGUAGE_COUNT; ++lang) {
            const char *localized = session_bbs_subcommand_localized(
                &kSessionBbsSubcommands[idx], (session_ui_language_t)lang);
            if (localized != nullptr && strcmp(command, localized) == 0) {
                return kSessionBbsSubcommands[idx].canonical;
            }
        }
    }

    return nullptr;
}

static void session_bbs_format_usage(session_ctx_t *ctx, const char *canonical,
                                     const char *arguments, char *buffer,
                                     size_t length)
{
    if (buffer == nullptr || length == 0U) {
        return;
    }

    buffer[0] = '\0';
    const char *bbs_command =
        session_command_alias_preferred_by_canonical(ctx, "/bbs");
    if (bbs_command == nullptr || bbs_command[0] == '\0') {
        bbs_command = "/bbs";
    }

    const char *subcommand =
        canonical != nullptr ? session_bbs_subcommand_preferred(ctx, canonical)
                             : nullptr;
    if (subcommand == nullptr || subcommand[0] == '\0') {
        subcommand = canonical != nullptr ? canonical : "";
    }

    const char *args = arguments != nullptr ? arguments : "";
    const char *separator = args[0] != '\0' ? " " : "";

    snprintf(buffer, length, "Usage: %s %s%s%s", bbs_command, subcommand,
             separator, args);
}

static void session_bbs_send_usage(session_ctx_t *ctx, const char *canonical,
                                   const char *arguments)
{
    char usage[SSH_CHATTER_MESSAGE_LIMIT];
    session_bbs_format_usage(ctx, canonical, arguments, usage, sizeof(usage));
    session_send_system_line(ctx, usage);
}

static const session_command_alias_t *
session_command_alias_lookup(const char *canonical)
{
    if (canonical == nullptr || canonical[0] == '\0') {
        return nullptr;
    }
    for (size_t idx = 0; idx < kSessionCommandAliasCount; ++idx) {
        if (strcmp(kSessionCommandAliases[idx].canonical, canonical) == 0) {
            return &kSessionCommandAliases[idx];
        }
    }
    return nullptr;
}

static const char *
session_command_alias_for_language(const session_command_alias_t *alias,
                                   session_ui_language_t language)
{
    if (alias == nullptr) {
        return nullptr;
    }
    if (language < 0 || language >= SESSION_UI_LANGUAGE_COUNT) {
        language = SESSION_UI_LANGUAGE_KO;
    }
    const char *localized = alias->localized[language];
    if (localized != nullptr && localized[0] != '\0') {
        return localized;
    }
    return alias->canonical;
}

static const char *
session_command_alias_preferred(const session_ctx_t *ctx,
                                const session_command_alias_t *alias)
{
    return session_command_alias_for_language(alias,
                                              session_ui_language_current(ctx));
}

static const char *
session_command_alias_preferred_by_canonical(const session_ctx_t *ctx,
                                             const char *canonical)
{
    const session_command_alias_t *alias =
        session_command_alias_lookup(canonical);
    if (alias == nullptr) {
        return canonical;
    }
    return session_command_alias_preferred(ctx, alias);
}

static bool session_parse_command(const char *line, const char *command,
                                  const char **arguments);
static bool
session_parse_localized_command(session_ctx_t *ctx,
                                const session_command_alias_t *alias,
                                const char *line, const char **arguments);

static bool session_parse_command_any(session_ctx_t *ctx, const char *canonical,
                                      const char *line, const char **arguments)
{
    if (canonical == nullptr) {
        return false;
    }
    const session_command_alias_t *alias =
        session_command_alias_lookup(canonical);
    if (alias != nullptr) {
        return session_parse_localized_command(ctx, alias, line, arguments);
    }
    return session_parse_command(line, canonical, arguments);
}

static void session_command_collect_localized_matches(session_ctx_t *ctx,
                                                      const char *prefix,
                                                      const char **matches,
                                                      size_t *match_count,
                                                      size_t max_count)
{
    if (ctx == nullptr || matches == nullptr || match_count == nullptr) {
        return;
    }

    size_t prefix_len = prefix != nullptr ? strlen(prefix) : 0U;

    for (size_t idx = 0; idx < kSessionCommandAliasCount; ++idx) {
        const session_command_alias_t *alias = &kSessionCommandAliases[idx];
        const char *localized = session_command_alias_preferred(ctx, alias);
        if (localized == nullptr || localized[0] == '\0') {
            continue;
        }
        if (strcmp(localized, alias->canonical) == 0) {
            continue;
        }

        const char *name = localized[0] == '/' ? localized + 1 : localized;
        if (name[0] == '\0') {
            continue;
        }

        if (prefix_len > 0U && strncasecmp(name, prefix, prefix_len) != 0) {
            continue;
        }

        bool duplicate = false;
        for (size_t existing = 0; existing < *match_count; ++existing) {
            if (strcmp(matches[existing], name) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        if (*match_count >= max_count) {
            break;
        }
        matches[(*match_count)++] = name;
    }
}

static void session_command_format_usage(session_ctx_t *ctx,
                                         const char *canonical,
                                         const char *fallback, char *buffer,
                                         size_t length)
{
    if (buffer == nullptr || length == 0U) {
        return;
    }

    buffer[0] = '\0';
    if (fallback == nullptr) {
        return;
    }

    if (canonical == nullptr || canonical[0] == '\0') {
        snprintf(buffer, length, "%s", fallback);
        return;
    }

    const char *alias =
        session_command_alias_preferred_by_canonical(ctx, canonical);
    if (alias == nullptr || alias[0] == '\0') {
        alias = canonical;
    }

    const char *prefix = session_command_prefix(ctx);
    if (prefix == nullptr) {
        prefix = "";
    }

    const char *alias_body = alias;
    if (alias_body != nullptr && alias_body[0] == '/') {
        ++alias_body;
    }

    const char *canonical_body = canonical;
    if (canonical_body[0] == '/') {
        ++canonical_body;
    }

    if (alias_body == nullptr || alias_body[0] == '\0') {
        alias_body = canonical_body;
    }

    char replacement[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(replacement, sizeof(replacement), "%s%s", prefix,
             alias_body != nullptr ? alias_body : "");

    const char *source = fallback;
    size_t canonical_len = strlen(canonical);
    size_t out_index = 0U;
    bool replaced = false;

    for (size_t idx = 0U; source[idx] != '\0' && out_index + 1U < length;) {
        if (canonical_len > 0U &&
            strncmp(source + idx, canonical, canonical_len) == 0) {
            size_t repl_len = strnlen(replacement, length - out_index - 1U);
            memcpy(buffer + out_index, replacement, repl_len);
            out_index += repl_len;
            idx += canonical_len;
            replaced = true;
            continue;
        }
        buffer[out_index++] = source[idx++];
    }
    buffer[out_index] = '\0';

    if (replaced) {
        return;
    }

    size_t body_len = canonical_body != nullptr ? strlen(canonical_body) : 0U;
    if (body_len == 0U) {
        snprintf(buffer, length, "%s", fallback);
        return;
    }

    out_index = 0U;
    bool body_replaced = false;
    for (size_t idx = 0U; source[idx] != '\0' && out_index + 1U < length;) {
        if (strncmp(source + idx, canonical_body, body_len) == 0) {
            size_t repl_len = strnlen(replacement, length - out_index - 1U);
            memcpy(buffer + out_index, replacement, repl_len);
            out_index += repl_len;
            idx += body_len;
            body_replaced = true;
            continue;
        }
        buffer[out_index++] = source[idx++];
    }
    buffer[out_index] = '\0';

    if (!body_replaced) {
        snprintf(buffer, length, "%s", fallback);
    }
}

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

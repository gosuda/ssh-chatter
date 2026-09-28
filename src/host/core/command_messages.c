/**
 * @file command_messages.c
 * @desc Localized system messages for chat commands (/pm, /mail, /block).
 */

typedef struct session_command_message {
    const char *english;
    const char *localized[SESSION_UI_LANGUAGE_COUNT];
} session_command_message_t;

static const session_command_message_t kSessionCommandMessages[] = {
    {
        "Private messages are unavailable right now.",
        {
            [SESSION_UI_LANGUAGE_KO] = "지금은 귓속말을 사용할 수 없습니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "現在プライベートメッセージは利用できません。",
            [SESSION_UI_LANGUAGE_ZH] = "私信功能暂时不可用。",
            [SESSION_UI_LANGUAGE_RU] = "Личные сообщения сейчас недоступны.",
            [SESSION_UI_LANGUAGE_DE] =
                "Private Nachrichten sind derzeit nicht verfügbar.",
            [SESSION_UI_LANGUAGE_FR] =
                "Les messages privés sont indisponibles pour le moment.",
            [SESSION_UI_LANGUAGE_PL] =
                "Prywatne wiadomości są teraz niedostępne.",
        },
    },
    {
        "User '%s' is not connected.",
        {
            [SESSION_UI_LANGUAGE_KO] = "사용자 '%s'님은 접속해 있지 않습니다.",
            [SESSION_UI_LANGUAGE_JP] = "ユーザー「%s」は接続していません。",
            [SESSION_UI_LANGUAGE_ZH] = "用户“%s”不在线。",
            [SESSION_UI_LANGUAGE_RU] = "Пользователь «%s» не в сети.",
            [SESSION_UI_LANGUAGE_DE] = "Benutzer „%s“ ist nicht verbunden.",
            [SESSION_UI_LANGUAGE_FR] =
                "L'utilisateur « %s » n'est pas connecté.",
            [SESSION_UI_LANGUAGE_PL] = "Użytkownik „%s” nie jest połączony.",
        },
    },
    {
        "User '%.256s' is not connected.",
        {
            [SESSION_UI_LANGUAGE_KO] =
                "사용자 '%.256s'님은 접속해 있지 않습니다.",
            [SESSION_UI_LANGUAGE_JP] = "ユーザー「%.256s」は接続していません。",
            [SESSION_UI_LANGUAGE_ZH] = "用户“%.256s”不在线。",
            [SESSION_UI_LANGUAGE_RU] = "Пользователь «%.256s» не в сети.",
            [SESSION_UI_LANGUAGE_DE] = "Benutzer „%.256s“ ist nicht verbunden.",
            [SESSION_UI_LANGUAGE_FR] =
                "L'utilisateur « %.256s » n'est pas connecté.",
            [SESSION_UI_LANGUAGE_PL] =
                "Użytkownik „%.256s” nie jest połączony.",
        },
    },
    {
        "Translation unavailable; sending your original message.",
        {
            [SESSION_UI_LANGUAGE_KO] =
                "번역을 사용할 수 없어 원문으로 보냅니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "翻訳できないため、原文のまま送信します。",
            [SESSION_UI_LANGUAGE_ZH] = "翻译不可用，将发送原文。",
            [SESSION_UI_LANGUAGE_RU] =
                "Перевод недоступен; отправляется исходное сообщение.",
            [SESSION_UI_LANGUAGE_DE] = "Übersetzung nicht verfügbar; Ihre "
                                       "Originalnachricht wird gesendet.",
            [SESSION_UI_LANGUAGE_FR] =
                "Traduction indisponible ; envoi de votre message original.",
            [SESSION_UI_LANGUAGE_PL] =
                "Tłumaczenie niedostępne; wysyłam oryginalną wiadomość.",
        },
    },
    {
        "No provider block is awaiting confirmation.",
        {
            [SESSION_UI_LANGUAGE_KO] =
                "확인을 기다리는 공용 IP 차단이 없습니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "確認待ちのプロバイダーブロックはありません。",
            [SESSION_UI_LANGUAGE_ZH] = "没有等待确认的运营商屏蔽。",
            [SESSION_UI_LANGUAGE_RU] =
                "Нет блокировки провайдера, ожидающей подтверждения.",
            [SESSION_UI_LANGUAGE_DE] =
                "Keine Provider-Sperre wartet auf Bestätigung.",
            [SESSION_UI_LANGUAGE_FR] =
                "Aucun blocage de fournisseur n'attend de confirmation.",
            [SESSION_UI_LANGUAGE_PL] =
                "Żadna blokada dostawcy nie czeka na potwierdzenie.",
        },
    },
    {
        "Pending block is for [%s], not [%s].",
        {
            [SESSION_UI_LANGUAGE_KO] =
                "대기 중인 차단은 [%s] 대상이며 [%s]이(가) 아닙니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "保留中のブロックは [%s] 宛てで、[%s] ではありません。",
            [SESSION_UI_LANGUAGE_ZH] = "待确认的屏蔽对象是 [%s]，不是 [%s]。",
            [SESSION_UI_LANGUAGE_RU] =
                "Ожидающая блокировка относится к [%s], а не к [%s].",
            [SESSION_UI_LANGUAGE_DE] =
                "Die ausstehende Sperre gilt für [%s], nicht für [%s].",
            [SESSION_UI_LANGUAGE_FR] =
                "Le blocage en attente concerne [%s], pas [%s].",
            [SESSION_UI_LANGUAGE_PL] =
                "Oczekująca blokada dotyczy [%s], nie [%s].",
        },
    },
    {
        "That target is already blocked.",
        {
            [SESSION_UI_LANGUAGE_KO] = "이미 차단된 대상입니다.",
            [SESSION_UI_LANGUAGE_JP] = "その対象はすでにブロックされています。",
            [SESSION_UI_LANGUAGE_ZH] = "该对象已被屏蔽。",
            [SESSION_UI_LANGUAGE_RU] = "Эта цель уже заблокирована.",
            [SESSION_UI_LANGUAGE_DE] = "Dieses Ziel ist bereits blockiert.",
            [SESSION_UI_LANGUAGE_FR] = "Cette cible est déjà bloquée.",
            [SESSION_UI_LANGUAGE_PL] = "Ten cel jest już zablokowany.",
        },
    },
    {
        "Unable to add block entry (limit reached?).",
        {
            [SESSION_UI_LANGUAGE_KO] =
                "차단 항목을 추가할 수 없습니다 (한도 초과?).",
            [SESSION_UI_LANGUAGE_JP] = "ブロック項目を追加できません（上限に達"
                                       "した可能性があります）。",
            [SESSION_UI_LANGUAGE_ZH] = "无法添加屏蔽条目（可能已达上限）。",
            [SESSION_UI_LANGUAGE_RU] =
                "Не удалось добавить блокировку (достигнут лимит?).",
            [SESSION_UI_LANGUAGE_DE] = "Sperreintrag konnte nicht hinzugefügt "
                                       "werden (Limit erreicht?).",
            [SESSION_UI_LANGUAGE_FR] =
                "Impossible d'ajouter le blocage (limite atteinte ?).",
            [SESSION_UI_LANGUAGE_PL] =
                "Nie można dodać blokady (osiągnięto limit?).",
        },
    },
    {
        "Blocking all users from %.63s.",
        {
            [SESSION_UI_LANGUAGE_KO] = "%.63s의 모든 사용자를 차단합니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "%.63s からの全ユーザーをブロックします。",
            [SESSION_UI_LANGUAGE_ZH] = "正在屏蔽来自 %.63s 的所有用户。",
            [SESSION_UI_LANGUAGE_RU] = "Блокируются все пользователи с %.63s.",
            [SESSION_UI_LANGUAGE_DE] =
                "Alle Benutzer von %.63s werden blockiert.",
            [SESSION_UI_LANGUAGE_FR] =
                "Blocage de tous les utilisateurs depuis %.63s.",
            [SESSION_UI_LANGUAGE_PL] =
                "Blokowanie wszystkich użytkowników z %.63s.",
        },
    },
    {
        "Blocking all users from %.256s.",
        {
            [SESSION_UI_LANGUAGE_KO] = "%.256s의 모든 사용자를 차단합니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "%.256s からの全ユーザーをブロックします。",
            [SESSION_UI_LANGUAGE_ZH] = "正在屏蔽来自 %.256s 的所有用户。",
            [SESSION_UI_LANGUAGE_RU] = "Блокируются все пользователи с %.256s.",
            [SESSION_UI_LANGUAGE_DE] =
                "Alle Benutzer von %.256s werden blockiert.",
            [SESSION_UI_LANGUAGE_FR] =
                "Blocage de tous les utilisateurs depuis %.256s.",
            [SESSION_UI_LANGUAGE_PL] =
                "Blokowanie wszystkich użytkowników z %.256s.",
        },
    },
    {
        "Blocking [%.23s] only (IP %.63s).",
        {
            [SESSION_UI_LANGUAGE_KO] = "[%.23s]만 차단합니다 (IP %.63s).",
            [SESSION_UI_LANGUAGE_JP] =
                "[%.23s] のみをブロックします（IP %.63s）。",
            [SESSION_UI_LANGUAGE_ZH] = "仅屏蔽 [%.23s]（IP %.63s）。",
            [SESSION_UI_LANGUAGE_RU] = "Блокируется только [%.23s] (IP %.63s).",
            [SESSION_UI_LANGUAGE_DE] = "Nur [%.23s] wird blockiert (IP %.63s).",
            [SESSION_UI_LANGUAGE_FR] =
                "Blocage de [%.23s] uniquement (IP %.63s).",
            [SESSION_UI_LANGUAGE_PL] = "Blokowanie tylko [%.23s] (IP %.63s).",
        },
    },
    {
        "Error: You cannot ban a country. %.256s is flagged as %.63s; other "
        "people may also be hidden.",
        {
            [SESSION_UI_LANGUAGE_KO] =
                "오류: 국가 단위로는 차단할 수 없습니다. %.256s은(는) "
                "%.63s(으)로 분류되어 다른 사람도 숨겨질 수 있습니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "エラー: 国単位ではブロックできません。%.256s は %.63s "
                "と識別されており、他の人も非表示になる可能性があります。",
            [SESSION_UI_LANGUAGE_ZH] = "错误：不能按国家屏蔽。%.256s 被标记为 "
                                       "%.63s，其他人也可能被隐藏。",
            [SESSION_UI_LANGUAGE_RU] =
                "Ошибка: нельзя заблокировать страну. %.256s помечен как "
                "%.63s; могут скрыться и другие люди.",
            [SESSION_UI_LANGUAGE_DE] =
                "Fehler: Sie können kein Land sperren. %.256s ist als %.63s "
                "markiert; auch andere Personen könnten ausgeblendet werden.",
            [SESSION_UI_LANGUAGE_FR] =
                "Erreur : vous ne pouvez pas bannir un pays. %.256s est "
                "identifié comme %.63s ; d'autres personnes pourraient aussi "
                "être masquées.",
            [SESSION_UI_LANGUAGE_PL] =
                "Błąd: nie można zablokować kraju. %.256s oznaczono jako "
                "%.63s; inne osoby też mogą zostać ukryte.",
        },
    },
    {
        "That IP is already blocked.",
        {
            [SESSION_UI_LANGUAGE_KO] = "이미 차단된 IP입니다.",
            [SESSION_UI_LANGUAGE_JP] = "そのIPはすでにブロックされています。",
            [SESSION_UI_LANGUAGE_ZH] = "该 IP 已被屏蔽。",
            [SESSION_UI_LANGUAGE_RU] = "Этот IP уже заблокирован.",
            [SESSION_UI_LANGUAGE_DE] = "Diese IP ist bereits blockiert.",
            [SESSION_UI_LANGUAGE_FR] = "Cette IP est déjà bloquée.",
            [SESSION_UI_LANGUAGE_PL] = "To IP jest już zablokowane.",
        },
    },
    {
        "Block list unavailable right now.",
        {
            [SESSION_UI_LANGUAGE_KO] = "지금은 차단 목록을 사용할 수 없습니다.",
            [SESSION_UI_LANGUAGE_JP] = "現在ブロックリストは利用できません。",
            [SESSION_UI_LANGUAGE_ZH] = "屏蔽列表暂时不可用。",
            [SESSION_UI_LANGUAGE_RU] = "Список блокировок сейчас недоступен.",
            [SESSION_UI_LANGUAGE_DE] =
                "Die Sperrliste ist derzeit nicht verfügbar.",
            [SESSION_UI_LANGUAGE_FR] =
                "La liste de blocage est indisponible pour le moment.",
            [SESSION_UI_LANGUAGE_PL] = "Lista blokad jest teraz niedostępna.",
        },
    },
    {
        "You do not need to block yourself.",
        {
            [SESSION_UI_LANGUAGE_KO] = "자기 자신은 차단할 필요가 없습니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "自分自身をブロックする必要はありません。",
            [SESSION_UI_LANGUAGE_ZH] = "你无需屏蔽自己。",
            [SESSION_UI_LANGUAGE_RU] = "Блокировать себя не нужно.",
            [SESSION_UI_LANGUAGE_DE] =
                "Sie müssen sich nicht selbst blockieren.",
            [SESSION_UI_LANGUAGE_FR] = "Inutile de vous bloquer vous-même.",
            [SESSION_UI_LANGUAGE_PL] = "Nie musisz blokować samego siebie.",
        },
    },
    {
        "Unable to identify that user's IP address right now.",
        {
            [SESSION_UI_LANGUAGE_KO] =
                "지금은 해당 사용자의 IP 주소를 확인할 수 없습니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "現在そのユーザーのIPアドレスを特定できません。",
            [SESSION_UI_LANGUAGE_ZH] = "暂时无法识别该用户的 IP 地址。",
            [SESSION_UI_LANGUAGE_RU] =
                "Сейчас не удаётся определить IP-адрес этого пользователя.",
            [SESSION_UI_LANGUAGE_DE] = "Die IP-Adresse dieses Benutzers kann "
                                       "derzeit nicht ermittelt werden.",
            [SESSION_UI_LANGUAGE_FR] = "Impossible d'identifier l'adresse IP "
                                       "de cet utilisateur pour le moment.",
            [SESSION_UI_LANGUAGE_PL] =
                "Nie można teraz ustalić adresu IP tego użytkownika.",
        },
    },
    {
        "%.63s appears to belong to %.63s.",
        {
            [SESSION_UI_LANGUAGE_KO] = "%.63s은(는) %.63s 소속으로 보입니다.",
            [SESSION_UI_LANGUAGE_JP] = "%.63s は %.63s に属しているようです。",
            [SESSION_UI_LANGUAGE_ZH] = "%.63s 似乎属于 %.63s。",
            [SESSION_UI_LANGUAGE_RU] = "%.63s, по-видимому, принадлежит %.63s.",
            [SESSION_UI_LANGUAGE_DE] = "%.63s scheint zu %.63s zu gehören.",
            [SESSION_UI_LANGUAGE_FR] = "%.63s semble appartenir à %.63s.",
            [SESSION_UI_LANGUAGE_PL] = "%.63s wydaje się należeć do %.63s.",
        },
    },
    {
        "That address is already blocked.",
        {
            [SESSION_UI_LANGUAGE_KO] = "이미 차단된 주소입니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "そのアドレスはすでにブロックされています。",
            [SESSION_UI_LANGUAGE_ZH] = "该地址已被屏蔽。",
            [SESSION_UI_LANGUAGE_RU] = "Этот адрес уже заблокирован.",
            [SESSION_UI_LANGUAGE_DE] = "Diese Adresse ist bereits blockiert.",
            [SESSION_UI_LANGUAGE_FR] = "Cette adresse est déjà bloquée.",
            [SESSION_UI_LANGUAGE_PL] = "Ten adres jest już zablokowany.",
        },
    },
    {
        "Blocking all users from %.63s (triggered by [%.23s]).",
        {
            [SESSION_UI_LANGUAGE_KO] =
                "%.63s의 모든 사용자를 차단합니다 ([%.23s] 기준).",
            [SESSION_UI_LANGUAGE_JP] =
                "%.63s からの全ユーザーをブロックします（[%.23s] による）。",
            [SESSION_UI_LANGUAGE_ZH] =
                "正在屏蔽来自 %.63s 的所有用户（由 [%.23s] 触发）。",
            [SESSION_UI_LANGUAGE_RU] =
                "Блокируются все пользователи с %.63s (по [%.23s]).",
            [SESSION_UI_LANGUAGE_DE] = "Alle Benutzer von %.63s werden "
                                       "blockiert (ausgelöst durch [%.23s]).",
            [SESSION_UI_LANGUAGE_FR] = "Blocage de tous les utilisateurs "
                                       "depuis %.63s (déclenché par [%.23s]).",
            [SESSION_UI_LANGUAGE_PL] = "Blokowanie wszystkich użytkowników z "
                                       "%.63s (z powodu [%.23s]).",
        },
    },
    {
        "Mailbox storage is unavailable.",
        {
            [SESSION_UI_LANGUAGE_KO] = "사서함 저장소를 사용할 수 없습니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "メールボックスの保存領域を利用できません。",
            [SESSION_UI_LANGUAGE_ZH] = "邮箱存储不可用。",
            [SESSION_UI_LANGUAGE_RU] = "Хранилище почты недоступно.",
            [SESSION_UI_LANGUAGE_DE] =
                "Der Postfachspeicher ist nicht verfügbar.",
            [SESSION_UI_LANGUAGE_FR] =
                "Le stockage de la boîte mail est indisponible.",
            [SESSION_UI_LANGUAGE_PL] =
                "Magazyn skrzynki pocztowej jest niedostępny.",
        },
    },
    {
        "Mailbox storage unavailable.",
        {
            [SESSION_UI_LANGUAGE_KO] = "사서함 저장소를 사용할 수 없습니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "メールボックスの保存領域を利用できません。",
            [SESSION_UI_LANGUAGE_ZH] = "邮箱存储不可用。",
            [SESSION_UI_LANGUAGE_RU] = "Хранилище почты недоступно.",
            [SESSION_UI_LANGUAGE_DE] =
                "Der Postfachspeicher ist nicht verfügbar.",
            [SESSION_UI_LANGUAGE_FR] =
                "Le stockage de la boîte mail est indisponible.",
            [SESSION_UI_LANGUAGE_PL] =
                "Magazyn skrzynki pocztowej jest niedostępny.",
        },
    },
    {
        "Invalid mailbox recipient.",
        {
            [SESSION_UI_LANGUAGE_KO] = "받는 사람이 올바르지 않습니다.",
            [SESSION_UI_LANGUAGE_JP] = "宛先が正しくありません。",
            [SESSION_UI_LANGUAGE_ZH] = "收件人无效。",
            [SESSION_UI_LANGUAGE_RU] = "Недопустимый получатель.",
            [SESSION_UI_LANGUAGE_DE] = "Ungültiger Empfänger.",
            [SESSION_UI_LANGUAGE_FR] = "Destinataire invalide.",
            [SESSION_UI_LANGUAGE_PL] = "Nieprawidłowy odbiorca.",
        },
    },
    {
        "Recipient IP is too long.",
        {
            [SESSION_UI_LANGUAGE_KO] = "받는 사람 IP가 너무 깁니다.",
            [SESSION_UI_LANGUAGE_JP] = "宛先IPが長すぎます。",
            [SESSION_UI_LANGUAGE_ZH] = "收件人 IP 过长。",
            [SESSION_UI_LANGUAGE_RU] = "IP получателя слишком длинный.",
            [SESSION_UI_LANGUAGE_DE] = "Die Empfänger-IP ist zu lang.",
            [SESSION_UI_LANGUAGE_FR] = "L'IP du destinataire est trop longue.",
            [SESSION_UI_LANGUAGE_PL] = "IP odbiorcy jest za długie.",
        },
    },
    {
        "Mailbox message cannot be empty.",
        {
            [SESSION_UI_LANGUAGE_KO] = "메일 내용은 비워 둘 수 없습니다.",
            [SESSION_UI_LANGUAGE_JP] = "メッセージを空にすることはできません。",
            [SESSION_UI_LANGUAGE_ZH] = "邮件内容不能为空。",
            [SESSION_UI_LANGUAGE_RU] = "Сообщение не может быть пустым.",
            [SESSION_UI_LANGUAGE_DE] = "Die Nachricht darf nicht leer sein.",
            [SESSION_UI_LANGUAGE_FR] = "Le message ne peut pas être vide.",
            [SESSION_UI_LANGUAGE_PL] = "Wiadomość nie może być pusta.",
        },
    },
    {
        "Unable to deliver mailbox message.",
        {
            [SESSION_UI_LANGUAGE_KO] = "메일을 전달할 수 없습니다.",
            [SESSION_UI_LANGUAGE_JP] = "メッセージを配達できません。",
            [SESSION_UI_LANGUAGE_ZH] = "无法投递邮件。",
            [SESSION_UI_LANGUAGE_RU] = "Не удалось доставить сообщение.",
            [SESSION_UI_LANGUAGE_DE] =
                "Nachricht konnte nicht zugestellt werden.",
            [SESSION_UI_LANGUAGE_FR] = "Impossible de remettre le message.",
            [SESSION_UI_LANGUAGE_PL] = "Nie można dostarczyć wiadomości.",
        },
    },
    {
        "Delivered mailbox message to %s.",
        {
            [SESSION_UI_LANGUAGE_KO] = "%s님에게 메일을 보냈습니다.",
            [SESSION_UI_LANGUAGE_JP] = "%s にメッセージを配達しました。",
            [SESSION_UI_LANGUAGE_ZH] = "已将邮件投递给 %s。",
            [SESSION_UI_LANGUAGE_RU] = "Сообщение доставлено пользователю %s.",
            [SESSION_UI_LANGUAGE_DE] = "Nachricht an %s zugestellt.",
            [SESSION_UI_LANGUAGE_FR] = "Message remis à %s.",
            [SESSION_UI_LANGUAGE_PL] = "Dostarczono wiadomość do %s.",
        },
    },
    {
        "Mailbox cleared.",
        {
            [SESSION_UI_LANGUAGE_KO] = "사서함을 비웠습니다.",
            [SESSION_UI_LANGUAGE_JP] = "メールボックスを空にしました。",
            [SESSION_UI_LANGUAGE_ZH] = "邮箱已清空。",
            [SESSION_UI_LANGUAGE_RU] = "Почтовый ящик очищен.",
            [SESSION_UI_LANGUAGE_DE] = "Postfach geleert.",
            [SESSION_UI_LANGUAGE_FR] = "Boîte mail vidée.",
            [SESSION_UI_LANGUAGE_PL] = "Skrzynka wyczyszczona.",
        },
    },
    {
        "Failed to update mailbox.",
        {
            [SESSION_UI_LANGUAGE_KO] = "사서함을 업데이트하지 못했습니다.",
            [SESSION_UI_LANGUAGE_JP] = "メールボックスを更新できませんでした。",
            [SESSION_UI_LANGUAGE_ZH] = "更新邮箱失败。",
            [SESSION_UI_LANGUAGE_RU] = "Не удалось обновить почтовый ящик.",
            [SESSION_UI_LANGUAGE_DE] =
                "Postfach konnte nicht aktualisiert werden.",
            [SESSION_UI_LANGUAGE_FR] =
                "Échec de la mise à jour de la boîte mail.",
            [SESSION_UI_LANGUAGE_PL] = "Nie udało się zaktualizować skrzynki.",
        },
    },
    {
        "Invalid mailbox parameters.",
        {
            [SESSION_UI_LANGUAGE_KO] = "메일 매개변수가 올바르지 않습니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "メールのパラメーターが正しくありません。",
            [SESSION_UI_LANGUAGE_ZH] = "邮件参数无效。",
            [SESSION_UI_LANGUAGE_RU] = "Недопустимые параметры почты.",
            [SESSION_UI_LANGUAGE_DE] = "Ungültige Postfachparameter.",
            [SESSION_UI_LANGUAGE_FR] = "Paramètres de messagerie invalides.",
            [SESSION_UI_LANGUAGE_PL] = "Nieprawidłowe parametry poczty.",
        },
    },
    {
        "LAN operator mailbox is unavailable.",
        {
            [SESSION_UI_LANGUAGE_KO] =
                "LAN 운영자 사서함을 사용할 수 없습니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "LANオペレーターのメールボックスは利用できません。",
            [SESSION_UI_LANGUAGE_ZH] = "LAN 管理员邮箱不可用。",
            [SESSION_UI_LANGUAGE_RU] =
                "Почтовый ящик оператора LAN недоступен.",
            [SESSION_UI_LANGUAGE_DE] =
                "Das Postfach des LAN-Operators ist nicht verfügbar.",
            [SESSION_UI_LANGUAGE_FR] =
                "La boîte mail de l'opérateur LAN est indisponible.",
            [SESSION_UI_LANGUAGE_PL] =
                "Skrzynka operatora LAN jest niedostępna.",
        },
    },
    {
        "Provide the recipient's IP (name@ip) when they are offline.",
        {
            [SESSION_UI_LANGUAGE_KO] =
                "받는 사람이 오프라인이면 IP를 함께 적어 주세요 (이름@IP).",
            [SESSION_UI_LANGUAGE_JP] =
                "相手がオフラインの場合は IP を指定してください（名前@IP）。",
            [SESSION_UI_LANGUAGE_ZH] = "收件人离线时请提供其 IP（名字@IP）。",
            [SESSION_UI_LANGUAGE_RU] =
                "Если получатель не в сети, укажите его IP (имя@ip).",
            [SESSION_UI_LANGUAGE_DE] =
                "Ist der Empfänger offline, geben Sie seine IP an (Name@IP).",
            [SESSION_UI_LANGUAGE_FR] =
                "Si le destinataire est hors ligne, indiquez son IP (nom@ip).",
            [SESSION_UI_LANGUAGE_PL] =
                "Gdy odbiorca jest offline, podaj jego IP (nazwa@ip).",
        },
    },
    {
        "Failed to write mailbox file.",
        {
            [SESSION_UI_LANGUAGE_KO] = "사서함 파일을 쓰지 못했습니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "メールボックスファイルを書き込めませんでした。",
            [SESSION_UI_LANGUAGE_ZH] = "写入邮箱文件失败。",
            [SESSION_UI_LANGUAGE_RU] =
                "Не удалось записать файл почтового ящика.",
            [SESSION_UI_LANGUAGE_DE] =
                "Postfachdatei konnte nicht geschrieben werden.",
            [SESSION_UI_LANGUAGE_FR] =
                "Échec de l'écriture du fichier de boîte mail.",
            [SESSION_UI_LANGUAGE_PL] = "Nie udało się zapisać pliku skrzynki.",
        },
    },
    {
        "%s -> you",
        {
            [SESSION_UI_LANGUAGE_KO] = "%s -> 나",
            [SESSION_UI_LANGUAGE_JP] = "%s -> あなた",
            [SESSION_UI_LANGUAGE_ZH] = "%s -> 你",
            [SESSION_UI_LANGUAGE_RU] = "%s -> вам",
            [SESSION_UI_LANGUAGE_DE] = "%s -> dir",
            [SESSION_UI_LANGUAGE_FR] = "%s -> vous",
            [SESSION_UI_LANGUAGE_PL] = "%s -> do ciebie",
        },
    },
    {
        "you -> %s",
        {
            [SESSION_UI_LANGUAGE_KO] = "나 -> %s",
            [SESSION_UI_LANGUAGE_JP] = "あなた -> %s",
            [SESSION_UI_LANGUAGE_ZH] = "你 -> %s",
            [SESSION_UI_LANGUAGE_RU] = "вы -> %s",
            [SESSION_UI_LANGUAGE_DE] = "du -> %s",
            [SESSION_UI_LANGUAGE_FR] = "vous -> %s",
            [SESSION_UI_LANGUAGE_PL] = "ty -> %s",
        },
    },
    {
        "Unable to open mailbox for %s.",
        {
            [SESSION_UI_LANGUAGE_KO] = "%s님의 사서함을 열 수 없습니다.",
            [SESSION_UI_LANGUAGE_JP] = "%s のメールボックスを開けません。",
            [SESSION_UI_LANGUAGE_ZH] = "无法打开 %s 的邮箱。",
            [SESSION_UI_LANGUAGE_RU] = "Не удалось открыть почтовый ящик %s.",
            [SESSION_UI_LANGUAGE_DE] =
                "Postfach von %s kann nicht geöffnet werden.",
            [SESSION_UI_LANGUAGE_FR] =
                "Impossible d'ouvrir la boîte mail de %s.",
            [SESSION_UI_LANGUAGE_PL] =
                "Nie można otworzyć skrzynki użytkownika %s.",
        },
    },
    {
        "You are not allowed to kick users.",
        {
            [SESSION_UI_LANGUAGE_KO] = "사용자를 내보낼 권한이 없습니다.",
            [SESSION_UI_LANGUAGE_JP] = "ユーザーをキックする権限がありません。",
            [SESSION_UI_LANGUAGE_ZH] = "你无权踢出用户。",
            [SESSION_UI_LANGUAGE_RU] = "У вас нет прав выгонять пользователей.",
            [SESSION_UI_LANGUAGE_DE] =
                "Sie dürfen keine Benutzer hinauswerfen.",
            [SESSION_UI_LANGUAGE_FR] =
                "Vous n'êtes pas autorisé à expulser des utilisateurs.",
            [SESSION_UI_LANGUAGE_PL] =
                "Nie masz uprawnień do wyrzucania użytkowników.",
        },
    },
    {
        "Usage: /kick <username>",
        {
            [SESSION_UI_LANGUAGE_KO] = "사용법: /kick <사용자>",
            [SESSION_UI_LANGUAGE_JP] = "使い方: /kick <ユーザー>",
            [SESSION_UI_LANGUAGE_ZH] = "用法: /kick <用户>",
            [SESSION_UI_LANGUAGE_RU] = "Использование: /kick <пользователь>",
            [SESSION_UI_LANGUAGE_DE] = "Nutzung: /kick <Benutzer>",
            [SESSION_UI_LANGUAGE_FR] = "Utilisation: /kick <utilisateur>",
            [SESSION_UI_LANGUAGE_PL] = "Użycie: /kick <użytkownik>",
        },
    },
    {
        "You cannot kick yourself.",
        {
            [SESSION_UI_LANGUAGE_KO] = "자기 자신은 내보낼 수 없습니다.",
            [SESSION_UI_LANGUAGE_JP] = "自分自身をキックすることはできません。",
            [SESSION_UI_LANGUAGE_ZH] = "你不能踢出自己。",
            [SESSION_UI_LANGUAGE_RU] = "Нельзя выгнать самого себя.",
            [SESSION_UI_LANGUAGE_DE] =
                "Sie können sich nicht selbst hinauswerfen.",
            [SESSION_UI_LANGUAGE_FR] =
                "Vous ne pouvez pas vous expulser vous-même.",
            [SESSION_UI_LANGUAGE_PL] = "Nie możesz wyrzucić samego siebie.",
        },
    },
    {
        "You have been kicked by an operator.",
        {
            [SESSION_UI_LANGUAGE_KO] = "운영자에 의해 퇴장되었습니다.",
            [SESSION_UI_LANGUAGE_JP] = "オペレーターによってキックされました。",
            [SESSION_UI_LANGUAGE_ZH] = "你已被管理员踢出。",
            [SESSION_UI_LANGUAGE_RU] = "Вас выгнал оператор.",
            [SESSION_UI_LANGUAGE_DE] =
                "Sie wurden von einem Operator hinausgeworfen.",
            [SESSION_UI_LANGUAGE_FR] =
                "Vous avez été expulsé par un opérateur.",
            [SESSION_UI_LANGUAGE_PL] = "Zostałeś wyrzucony przez operatora.",
        },
    },
    {
        "User removed from the chat.",
        {
            [SESSION_UI_LANGUAGE_KO] = "사용자를 채팅에서 내보냈습니다.",
            [SESSION_UI_LANGUAGE_JP] = "ユーザーをチャットから退出させました。",
            [SESSION_UI_LANGUAGE_ZH] = "已将用户移出聊天。",
            [SESSION_UI_LANGUAGE_RU] = "Пользователь удалён из чата.",
            [SESSION_UI_LANGUAGE_DE] = "Benutzer aus dem Chat entfernt.",
            [SESSION_UI_LANGUAGE_FR] = "Utilisateur retiré du chat.",
            [SESSION_UI_LANGUAGE_PL] = "Użytkownik usunięty z czatu.",
        },
    },
    {
        "You are not allowed to ban nicknames.",
        {
            [SESSION_UI_LANGUAGE_KO] = "닉네임을 차단할 권한이 없습니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "ニックネームを禁止する権限がありません。",
            [SESSION_UI_LANGUAGE_ZH] = "你无权封禁昵称。",
            [SESSION_UI_LANGUAGE_RU] = "У вас нет прав банить ники.",
            [SESSION_UI_LANGUAGE_DE] = "Sie dürfen keine Nicknames sperren.",
            [SESSION_UI_LANGUAGE_FR] =
                "Vous n'êtes pas autorisé à bannir des pseudos.",
            [SESSION_UI_LANGUAGE_PL] =
                "Nie masz uprawnień do banowania nicków.",
        },
    },
    {
        "Host unavailable.",
        {
            [SESSION_UI_LANGUAGE_KO] = "호스트를 사용할 수 없습니다.",
            [SESSION_UI_LANGUAGE_JP] = "ホストを利用できません。",
            [SESSION_UI_LANGUAGE_ZH] = "主机不可用。",
            [SESSION_UI_LANGUAGE_RU] = "Хост недоступен.",
            [SESSION_UI_LANGUAGE_DE] = "Host nicht verfügbar.",
            [SESSION_UI_LANGUAGE_FR] = "Hôte indisponible.",
            [SESSION_UI_LANGUAGE_PL] = "Host niedostępny.",
        },
    },
    {
        "Usage: /banname <nickname>",
        {
            [SESSION_UI_LANGUAGE_KO] = "사용법: /banname <닉네임>",
            [SESSION_UI_LANGUAGE_JP] = "使い方: /banname <ニックネーム>",
            [SESSION_UI_LANGUAGE_ZH] = "用法: /banname <昵称>",
            [SESSION_UI_LANGUAGE_RU] = "Использование: /banname <ник>",
            [SESSION_UI_LANGUAGE_DE] = "Nutzung: /banname <Nickname>",
            [SESSION_UI_LANGUAGE_FR] = "Utilisation: /banname <pseudo>",
            [SESSION_UI_LANGUAGE_PL] = "Użycie: /banname <nick>",
        },
    },
    {
        "Nicknames may not include control characters or whitespace.",
        {
            [SESSION_UI_LANGUAGE_KO] =
                "닉네임에는 제어 문자나 공백을 넣을 수 없습니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "ニックネームに制御文字や空白は使えません。",
            [SESSION_UI_LANGUAGE_ZH] = "昵称不能包含控制字符或空白。",
            [SESSION_UI_LANGUAGE_RU] =
                "Ник не может содержать управляющие символы или пробелы.",
            [SESSION_UI_LANGUAGE_DE] = "Nicknames dürfen keine Steuerzeichen "
                                       "oder Leerzeichen enthalten.",
            [SESSION_UI_LANGUAGE_FR] = "Les pseudos ne peuvent pas contenir de "
                                       "caractères de contrôle ni d'espaces.",
            [SESSION_UI_LANGUAGE_PL] =
                "Nick nie może zawierać znaków sterujących ani spacji.",
        },
    },
    {
        "That nickname is already blocked for bot detection.",
        {
            [SESSION_UI_LANGUAGE_KO] = "이미 봇 탐지로 차단된 닉네임입니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "そのニックネームはすでにボット検出のため禁止されています。",
            [SESSION_UI_LANGUAGE_ZH] = "该昵称已因机器人检测被封禁。",
            [SESSION_UI_LANGUAGE_RU] =
                "Этот ник уже заблокирован для обнаружения ботов.",
            [SESSION_UI_LANGUAGE_DE] =
                "Dieser Nickname ist bereits zur Bot-Erkennung gesperrt.",
            [SESSION_UI_LANGUAGE_FR] =
                "Ce pseudo est déjà bloqué pour la détection de bots.",
            [SESSION_UI_LANGUAGE_PL] =
                "Ten nick jest już zablokowany w ramach wykrywania botów.",
        },
    },
    {
        "Unable to add ban entry (list full?).",
        {
            [SESSION_UI_LANGUAGE_KO] =
                "차단 항목을 추가할 수 없습니다 (목록이 가득 찼나요?).",
            [SESSION_UI_LANGUAGE_JP] =
                "禁止項目を追加できません（リストが満杯の可能性があります）。",
            [SESSION_UI_LANGUAGE_ZH] = "无法添加封禁条目（列表可能已满）。",
            [SESSION_UI_LANGUAGE_RU] =
                "Не удалось добавить бан (список заполнен?).",
            [SESSION_UI_LANGUAGE_DE] =
                "Sperreintrag konnte nicht hinzugefügt werden (Liste voll?).",
            [SESSION_UI_LANGUAGE_FR] =
                "Impossible d'ajouter le bannissement (liste pleine ?).",
            [SESSION_UI_LANGUAGE_PL] = "Nie można dodać bana (lista pełna?).",
        },
    },
    {
        "Nickname ban applied.",
        {
            [SESSION_UI_LANGUAGE_KO] = "닉네임 차단을 적용했습니다.",
            [SESSION_UI_LANGUAGE_JP] = "ニックネームの禁止を適用しました。",
            [SESSION_UI_LANGUAGE_ZH] = "已应用昵称封禁。",
            [SESSION_UI_LANGUAGE_RU] = "Бан ника применён.",
            [SESSION_UI_LANGUAGE_DE] = "Nickname-Sperre angewendet.",
            [SESSION_UI_LANGUAGE_FR] = "Bannissement du pseudo appliqué.",
            [SESSION_UI_LANGUAGE_PL] = "Ban nicku zastosowany.",
        },
    },
    {
        "Your nickname is now blocked for bot detection. Use /nick <name> to "
        "change immediately.",
        {
            [SESSION_UI_LANGUAGE_KO] =
                "지금 닉네임이 봇 탐지로 차단되었습니다. /nick <이름>으로 바로 "
                "바꿔 주세요.",
            [SESSION_UI_LANGUAGE_JP] =
                "あなたのニックネームはボット検出のため禁止されました。/nick "
                "<名前> ですぐに変更してください。",
            [SESSION_UI_LANGUAGE_ZH] =
                "你的昵称已因机器人检测被封禁。请立即使用 /nick <名字> 更改。",
            [SESSION_UI_LANGUAGE_RU] =
                "Ваш ник заблокирован для обнаружения ботов. Сразу смените его "
                "через /nick <имя>.",
            [SESSION_UI_LANGUAGE_DE] =
                "Ihr Nickname ist jetzt zur Bot-Erkennung gesperrt. Ändern Sie "
                "ihn sofort mit /nick <Name>.",
            [SESSION_UI_LANGUAGE_FR] =
                "Votre pseudo est désormais bloqué pour la détection de bots. "
                "Changez-le tout de suite avec /nick <nom>.",
            [SESSION_UI_LANGUAGE_PL] =
                "Twój nick został zablokowany w ramach wykrywania botów. Zmień "
                "go od razu przez /nick <nazwa>.",
        },
    },
    {
        "You are not allowed to ban users.",
        {
            [SESSION_UI_LANGUAGE_KO] = "사용자를 차단할 권한이 없습니다.",
            [SESSION_UI_LANGUAGE_JP] = "ユーザーを禁止する権限がありません。",
            [SESSION_UI_LANGUAGE_ZH] = "你无权封禁用户。",
            [SESSION_UI_LANGUAGE_RU] = "У вас нет прав банить пользователей.",
            [SESSION_UI_LANGUAGE_DE] = "Sie dürfen keine Benutzer sperren.",
            [SESSION_UI_LANGUAGE_FR] =
                "Vous n'êtes pas autorisé à bannir des utilisateurs.",
            [SESSION_UI_LANGUAGE_PL] =
                "Nie masz uprawnień do banowania użytkowników.",
        },
    },
    {
        "Usage: /ban <username>",
        {
            [SESSION_UI_LANGUAGE_KO] = "사용법: /ban <사용자>",
            [SESSION_UI_LANGUAGE_JP] = "使い方: /ban <ユーザー>",
            [SESSION_UI_LANGUAGE_ZH] = "用法: /ban <用户>",
            [SESSION_UI_LANGUAGE_RU] = "Использование: /ban <пользователь>",
            [SESSION_UI_LANGUAGE_DE] = "Nutzung: /ban <Benutzer>",
            [SESSION_UI_LANGUAGE_FR] = "Utilisation: /ban <utilisateur>",
            [SESSION_UI_LANGUAGE_PL] = "Użycie: /ban <użytkownik>",
        },
    },
    {
        "%s '%s' has been banned.",
        {
            [SESSION_UI_LANGUAGE_KO] = "%s '%s'을(를) 차단했습니다.",
            [SESSION_UI_LANGUAGE_JP] = "%s「%s」を禁止しました。",
            [SESSION_UI_LANGUAGE_ZH] = "%s“%s”已被封禁。",
            [SESSION_UI_LANGUAGE_RU] = "%s «%s» заблокирован.",
            [SESSION_UI_LANGUAGE_DE] = "%s „%s“ wurde gesperrt.",
            [SESSION_UI_LANGUAGE_FR] = "%s « %s » a été banni.",
            [SESSION_UI_LANGUAGE_PL] = "%s „%s” został zbanowany.",
        },
    },
    {
        "LAN operators cannot be banned.",
        {
            [SESSION_UI_LANGUAGE_KO] = "LAN 운영자는 차단할 수 없습니다.",
            [SESSION_UI_LANGUAGE_JP] = "LANオペレーターは禁止できません。",
            [SESSION_UI_LANGUAGE_ZH] = "不能封禁 LAN 管理员。",
            [SESSION_UI_LANGUAGE_RU] = "Операторов LAN нельзя забанить.",
            [SESSION_UI_LANGUAGE_DE] =
                "LAN-Operatoren können nicht gesperrt werden.",
            [SESSION_UI_LANGUAGE_FR] =
                "Les opérateurs LAN ne peuvent pas être bannis.",
            [SESSION_UI_LANGUAGE_PL] = "Operatorów LAN nie można zbanować.",
        },
    },
    {
        "Ban applied.",
        {
            [SESSION_UI_LANGUAGE_KO] = "차단을 적용했습니다.",
            [SESSION_UI_LANGUAGE_JP] = "禁止を適用しました。",
            [SESSION_UI_LANGUAGE_ZH] = "已应用封禁。",
            [SESSION_UI_LANGUAGE_RU] = "Бан применён.",
            [SESSION_UI_LANGUAGE_DE] = "Sperre angewendet.",
            [SESSION_UI_LANGUAGE_FR] = "Bannissement appliqué.",
            [SESSION_UI_LANGUAGE_PL] = "Ban zastosowany.",
        },
    },
    {
        "You have been banned by [%s].",
        {
            [SESSION_UI_LANGUAGE_KO] = "[%s]님에 의해 차단되었습니다.",
            [SESSION_UI_LANGUAGE_JP] = "[%s] によって禁止されました。",
            [SESSION_UI_LANGUAGE_ZH] = "你已被 [%s] 封禁。",
            [SESSION_UI_LANGUAGE_RU] = "Вас забанил [%s].",
            [SESSION_UI_LANGUAGE_DE] = "Sie wurden von [%s] gesperrt.",
            [SESSION_UI_LANGUAGE_FR] = "Vous avez été banni par [%s].",
            [SESSION_UI_LANGUAGE_PL] = "Zostałeś zbanowany przez [%s].",
        },
    },
    {
        "You are not allowed to view the ban list.",
        {
            [SESSION_UI_LANGUAGE_KO] = "차단 목록을 볼 권한이 없습니다.",
            [SESSION_UI_LANGUAGE_JP] = "禁止リストを見る権限がありません。",
            [SESSION_UI_LANGUAGE_ZH] = "你无权查看封禁列表。",
            [SESSION_UI_LANGUAGE_RU] =
                "У вас нет прав просматривать список банов.",
            [SESSION_UI_LANGUAGE_DE] =
                "Sie dürfen die Sperrliste nicht einsehen.",
            [SESSION_UI_LANGUAGE_FR] =
                "Vous n'êtes pas autorisé à voir la liste des bannissements.",
            [SESSION_UI_LANGUAGE_PL] =
                "Nie masz uprawnień do przeglądania listy banów.",
        },
    },
    {
        "Usage: /banlist",
        {
            [SESSION_UI_LANGUAGE_KO] = "사용법: /banlist",
            [SESSION_UI_LANGUAGE_JP] = "使い方: /banlist",
            [SESSION_UI_LANGUAGE_ZH] = "用法: /banlist",
            [SESSION_UI_LANGUAGE_RU] = "Использование: /banlist",
            [SESSION_UI_LANGUAGE_DE] = "Nutzung: /banlist",
            [SESSION_UI_LANGUAGE_FR] = "Utilisation: /banlist",
            [SESSION_UI_LANGUAGE_PL] = "Użycie: /banlist",
        },
    },
    {
        "No active bans.",
        {
            [SESSION_UI_LANGUAGE_KO] = "활성 차단이 없습니다.",
            [SESSION_UI_LANGUAGE_JP] = "有効な禁止はありません。",
            [SESSION_UI_LANGUAGE_ZH] = "没有生效的封禁。",
            [SESSION_UI_LANGUAGE_RU] = "Активных банов нет.",
            [SESSION_UI_LANGUAGE_DE] = "Keine aktiven Sperren.",
            [SESSION_UI_LANGUAGE_FR] = "Aucun bannissement actif.",
            [SESSION_UI_LANGUAGE_PL] = "Brak aktywnych banów.",
        },
    },
    {
        "Active bans:",
        {
            [SESSION_UI_LANGUAGE_KO] = "활성 차단:",
            [SESSION_UI_LANGUAGE_JP] = "有効な禁止:",
            [SESSION_UI_LANGUAGE_ZH] = "生效的封禁:",
            [SESSION_UI_LANGUAGE_RU] = "Активные баны:",
            [SESSION_UI_LANGUAGE_DE] = "Aktive Sperren:",
            [SESSION_UI_LANGUAGE_FR] = "Bannissements actifs :",
            [SESSION_UI_LANGUAGE_PL] = "Aktywne bany:",
        },
    },
    {
        "%zu. user: %.*s, ip: %.*s",
        {
            [SESSION_UI_LANGUAGE_KO] = "%zu. 사용자: %.*s, IP: %.*s",
            [SESSION_UI_LANGUAGE_JP] = "%zu. ユーザー: %.*s, IP: %.*s",
            [SESSION_UI_LANGUAGE_ZH] = "%zu. 用户: %.*s, IP: %.*s",
            [SESSION_UI_LANGUAGE_RU] = "%zu. пользователь: %.*s, IP: %.*s",
            [SESSION_UI_LANGUAGE_DE] = "%zu. Benutzer: %.*s, IP: %.*s",
            [SESSION_UI_LANGUAGE_FR] = "%zu. utilisateur : %.*s, IP : %.*s",
            [SESSION_UI_LANGUAGE_PL] = "%zu. użytkownik: %.*s, IP: %.*s",
        },
    },
    {
        "%zu. user: %.*s",
        {
            [SESSION_UI_LANGUAGE_KO] = "%zu. 사용자: %.*s",
            [SESSION_UI_LANGUAGE_JP] = "%zu. ユーザー: %.*s",
            [SESSION_UI_LANGUAGE_ZH] = "%zu. 用户: %.*s",
            [SESSION_UI_LANGUAGE_RU] = "%zu. пользователь: %.*s",
            [SESSION_UI_LANGUAGE_DE] = "%zu. Benutzer: %.*s",
            [SESSION_UI_LANGUAGE_FR] = "%zu. utilisateur : %.*s",
            [SESSION_UI_LANGUAGE_PL] = "%zu. użytkownik: %.*s",
        },
    },
    {
        "%zu. ip: %.*s",
        {
            [SESSION_UI_LANGUAGE_KO] = "%zu. IP: %.*s",
            [SESSION_UI_LANGUAGE_JP] = "%zu. IP: %.*s",
            [SESSION_UI_LANGUAGE_ZH] = "%zu. IP: %.*s",
            [SESSION_UI_LANGUAGE_RU] = "%zu. IP: %.*s",
            [SESSION_UI_LANGUAGE_DE] = "%zu. IP: %.*s",
            [SESSION_UI_LANGUAGE_FR] = "%zu. IP : %.*s",
            [SESSION_UI_LANGUAGE_PL] = "%zu. IP: %.*s",
        },
    },
    {
        "%zu. <empty>",
        {
            [SESSION_UI_LANGUAGE_KO] = "%zu. <비어 있음>",
            [SESSION_UI_LANGUAGE_JP] = "%zu. <空>",
            [SESSION_UI_LANGUAGE_ZH] = "%zu. <空>",
            [SESSION_UI_LANGUAGE_RU] = "%zu. <пусто>",
            [SESSION_UI_LANGUAGE_DE] = "%zu. <leer>",
            [SESSION_UI_LANGUAGE_FR] = "%zu. <vide>",
            [SESSION_UI_LANGUAGE_PL] = "%zu. <pusty>",
        },
    },
    {
        "You are not allowed to run that command.",
        {
            [SESSION_UI_LANGUAGE_KO] = "이 명령을 실행할 권한이 없습니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "このコマンドを実行する権限がありません。",
            [SESSION_UI_LANGUAGE_ZH] = "你无权执行该命令。",
            [SESSION_UI_LANGUAGE_RU] = "У вас нет прав на эту команду.",
            [SESSION_UI_LANGUAGE_DE] =
                "Sie dürfen diesen Befehl nicht ausführen.",
            [SESSION_UI_LANGUAGE_FR] =
                "Vous n'êtes pas autorisé à exécuter cette commande.",
            [SESSION_UI_LANGUAGE_PL] = "Nie masz uprawnień do tego polecenia.",
        },
    },
    {
        "Usage: /getaddr <username>",
        {
            [SESSION_UI_LANGUAGE_KO] = "사용법: /getaddr <사용자>",
            [SESSION_UI_LANGUAGE_JP] = "使い方: /getaddr <ユーザー>",
            [SESSION_UI_LANGUAGE_ZH] = "用法: /getaddr <用户>",
            [SESSION_UI_LANGUAGE_RU] = "Использование: /getaddr <пользователь>",
            [SESSION_UI_LANGUAGE_DE] = "Nutzung: /getaddr <Benutzer>",
            [SESSION_UI_LANGUAGE_FR] = "Utilisation: /getaddr <utilisateur>",
            [SESSION_UI_LANGUAGE_PL] = "Użycie: /getaddr <użytkownik>",
        },
    },
    {
        "No recorded address for '%s'.",
        {
            [SESSION_UI_LANGUAGE_KO] = "'%s'의 기록된 주소가 없습니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "「%s」の記録されたアドレスはありません。",
            [SESSION_UI_LANGUAGE_ZH] = "没有“%s”的地址记录。",
            [SESSION_UI_LANGUAGE_RU] = "Для «%s» адрес не записан.",
            [SESSION_UI_LANGUAGE_DE] = "Keine gespeicherte Adresse für „%s“.",
            [SESSION_UI_LANGUAGE_FR] =
                "Aucune adresse enregistrée pour « %s ».",
            [SESSION_UI_LANGUAGE_PL] = "Brak zapisanego adresu dla „%s”.",
        },
    },
    {
        "Last known address for '%s': %s",
        {
            [SESSION_UI_LANGUAGE_KO] = "'%s'의 마지막 주소: %s",
            [SESSION_UI_LANGUAGE_JP] = "「%s」の最終アドレス: %s",
            [SESSION_UI_LANGUAGE_ZH] = "“%s”的最后已知地址: %s",
            [SESSION_UI_LANGUAGE_RU] = "Последний известный адрес «%s»: %s",
            [SESSION_UI_LANGUAGE_DE] = "Letzte bekannte Adresse von „%s“: %s",
            [SESSION_UI_LANGUAGE_FR] = "Dernière adresse connue de « %s » : %s",
            [SESSION_UI_LANGUAGE_PL] = "Ostatni znany adres „%s”: %s",
        },
    },
    {
        "Usage: /poke <username>",
        {
            [SESSION_UI_LANGUAGE_KO] = "사용법: /poke <사용자>",
            [SESSION_UI_LANGUAGE_JP] = "使い方: /poke <ユーザー>",
            [SESSION_UI_LANGUAGE_ZH] = "用法: /poke <用户>",
            [SESSION_UI_LANGUAGE_RU] = "Использование: /poke <пользователь>",
            [SESSION_UI_LANGUAGE_DE] = "Nutzung: /poke <Benutzer>",
            [SESSION_UI_LANGUAGE_FR] = "Utilisation: /poke <utilisateur>",
            [SESSION_UI_LANGUAGE_PL] = "Użycie: /poke <użytkownik>",
        },
    },
    {
        "Poke sent.",
        {
            [SESSION_UI_LANGUAGE_KO] = "호출을 보냈습니다.",
            [SESSION_UI_LANGUAGE_JP] = "呼び出しを送りました。",
            [SESSION_UI_LANGUAGE_ZH] = "已发送提醒。",
            [SESSION_UI_LANGUAGE_RU] = "Вызов отправлен.",
            [SESSION_UI_LANGUAGE_DE] = "Anstupser gesendet.",
            [SESSION_UI_LANGUAGE_FR] = "Appel envoyé.",
            [SESSION_UI_LANGUAGE_PL] = "Szturchnięcie wysłane.",
        },
    },
    {
        "Usage: /unblock <username|ip|all>",
        {
            [SESSION_UI_LANGUAGE_KO] = "사용법: /unblock <사용자|IP|all>",
            [SESSION_UI_LANGUAGE_JP] = "使い方: /unblock <ユーザー|IP|all>",
            [SESSION_UI_LANGUAGE_ZH] = "用法: /unblock <用户|IP|all>",
            [SESSION_UI_LANGUAGE_RU] =
                "Использование: /unblock <пользователь|IP|all>",
            [SESSION_UI_LANGUAGE_DE] = "Nutzung: /unblock <Benutzer|IP|all>",
            [SESSION_UI_LANGUAGE_FR] =
                "Utilisation: /unblock <utilisateur|IP|all>",
            [SESSION_UI_LANGUAGE_PL] = "Użycie: /unblock <użytkownik|IP|all>",
        },
    },
    {
        "No blocked entries to remove.",
        {
            [SESSION_UI_LANGUAGE_KO] = "해제할 차단 항목이 없습니다.",
            [SESSION_UI_LANGUAGE_JP] = "解除するブロック項目はありません。",
            [SESSION_UI_LANGUAGE_ZH] = "没有可移除的屏蔽条目。",
            [SESSION_UI_LANGUAGE_RU] = "Нет блокировок для удаления.",
            [SESSION_UI_LANGUAGE_DE] = "Keine Sperreinträge zum Entfernen.",
            [SESSION_UI_LANGUAGE_FR] = "Aucun blocage à supprimer.",
            [SESSION_UI_LANGUAGE_PL] = "Brak blokad do usunięcia.",
        },
    },
    {
        "Removed 1 blocked entry.",
        {
            [SESSION_UI_LANGUAGE_KO] = "차단 항목 1개를 해제했습니다.",
            [SESSION_UI_LANGUAGE_JP] = "ブロック項目を1件解除しました。",
            [SESSION_UI_LANGUAGE_ZH] = "已移除 1 个屏蔽条目。",
            [SESSION_UI_LANGUAGE_RU] = "Удалена 1 блокировка.",
            [SESSION_UI_LANGUAGE_DE] = "1 Sperreintrag entfernt.",
            [SESSION_UI_LANGUAGE_FR] = "1 blocage supprimé.",
            [SESSION_UI_LANGUAGE_PL] = "Usunięto 1 blokadę.",
        },
    },
    {
        "Removed %zu blocked entries.",
        {
            [SESSION_UI_LANGUAGE_KO] = "차단 항목 %zu개를 해제했습니다.",
            [SESSION_UI_LANGUAGE_JP] = "ブロック項目を%zu件解除しました。",
            [SESSION_UI_LANGUAGE_ZH] = "已移除 %zu 个屏蔽条目。",
            [SESSION_UI_LANGUAGE_RU] = "Удалено блокировок: %zu.",
            [SESSION_UI_LANGUAGE_DE] = "%zu Sperreinträge entfernt.",
            [SESSION_UI_LANGUAGE_FR] = "%zu blocages supprimés.",
            [SESSION_UI_LANGUAGE_PL] = "Usunięto blokady: %zu.",
        },
    },
    {
        "Removed block for %.256s.",
        {
            [SESSION_UI_LANGUAGE_KO] = "%.256s의 차단을 해제했습니다.",
            [SESSION_UI_LANGUAGE_JP] = "%.256s のブロックを解除しました。",
            [SESSION_UI_LANGUAGE_ZH] = "已解除对 %.256s 的屏蔽。",
            [SESSION_UI_LANGUAGE_RU] = "Блокировка %.256s снята.",
            [SESSION_UI_LANGUAGE_DE] = "Sperre für %.256s entfernt.",
            [SESSION_UI_LANGUAGE_FR] = "Blocage de %.256s supprimé.",
            [SESSION_UI_LANGUAGE_PL] = "Usunięto blokadę dla %.256s.",
        },
    },
    {
        "No block entry matched '%.256s'.",
        {
            [SESSION_UI_LANGUAGE_KO] =
                "'%.256s'와(과) 일치하는 차단 항목이 없습니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "「%.256s」に一致するブロック項目はありません。",
            [SESSION_UI_LANGUAGE_ZH] = "没有与“%.256s”匹配的屏蔽条目。",
            [SESSION_UI_LANGUAGE_RU] =
                "Нет блокировки, совпадающей с «%.256s».",
            [SESSION_UI_LANGUAGE_DE] = "Kein Sperreintrag passt zu „%.256s“.",
            [SESSION_UI_LANGUAGE_FR] =
                "Aucun blocage ne correspond à « %.256s ».",
            [SESSION_UI_LANGUAGE_PL] = "Brak blokady pasującej do „%.256s”.",
        },
    },
    {
        "You are not allowed to pardon users.",
        {
            [SESSION_UI_LANGUAGE_KO] = "차단을 해제할 권한이 없습니다.",
            [SESSION_UI_LANGUAGE_JP] = "禁止を解除する権限がありません。",
            [SESSION_UI_LANGUAGE_ZH] = "你无权解除封禁。",
            [SESSION_UI_LANGUAGE_RU] = "У вас нет прав снимать баны.",
            [SESSION_UI_LANGUAGE_DE] = "Sie dürfen keine Sperren aufheben.",
            [SESSION_UI_LANGUAGE_FR] =
                "Vous n'êtes pas autorisé à lever des bannissements.",
            [SESSION_UI_LANGUAGE_PL] =
                "Nie masz uprawnień do zdejmowania banów.",
        },
    },
    {
        "Usage: /pardon <user|ip>",
        {
            [SESSION_UI_LANGUAGE_KO] = "사용법: /pardon <사용자|IP>",
            [SESSION_UI_LANGUAGE_JP] = "使い方: /pardon <ユーザー|IP>",
            [SESSION_UI_LANGUAGE_ZH] = "用法: /pardon <用户|IP>",
            [SESSION_UI_LANGUAGE_RU] =
                "Использование: /pardon <пользователь|IP>",
            [SESSION_UI_LANGUAGE_DE] = "Nutzung: /pardon <Benutzer|IP>",
            [SESSION_UI_LANGUAGE_FR] = "Utilisation: /pardon <utilisateur|IP>",
            [SESSION_UI_LANGUAGE_PL] = "Użycie: /pardon <użytkownik|IP>",
        },
    },
    {
        "Ban lifted for '%s'.",
        {
            [SESSION_UI_LANGUAGE_KO] = "'%s'의 차단을 해제했습니다.",
            [SESSION_UI_LANGUAGE_JP] = "「%s」の禁止を解除しました。",
            [SESSION_UI_LANGUAGE_ZH] = "已解除对“%s”的封禁。",
            [SESSION_UI_LANGUAGE_RU] = "Бан для «%s» снят.",
            [SESSION_UI_LANGUAGE_DE] = "Sperre für „%s“ aufgehoben.",
            [SESSION_UI_LANGUAGE_FR] = "Bannissement levé pour « %s ».",
            [SESSION_UI_LANGUAGE_PL] = "Zdjęto bana dla „%s”.",
        },
    },
    {
        "No matching ban found.",
        {
            [SESSION_UI_LANGUAGE_KO] = "일치하는 차단이 없습니다.",
            [SESSION_UI_LANGUAGE_JP] = "一致する禁止はありません。",
            [SESSION_UI_LANGUAGE_ZH] = "未找到匹配的封禁。",
            [SESSION_UI_LANGUAGE_RU] = "Подходящий бан не найден.",
            [SESSION_UI_LANGUAGE_DE] = "Keine passende Sperre gefunden.",
            [SESSION_UI_LANGUAGE_FR] = "Aucun bannissement correspondant.",
            [SESSION_UI_LANGUAGE_PL] = "Nie znaleziono pasującego bana.",
        },
    },
    {
        "Usage: /getos <username>",
        {
            [SESSION_UI_LANGUAGE_KO] = "사용법: /getos <사용자>",
            [SESSION_UI_LANGUAGE_JP] = "使い方: /getos <ユーザー>",
            [SESSION_UI_LANGUAGE_ZH] = "用法: /getos <用户>",
            [SESSION_UI_LANGUAGE_RU] = "Использование: /getos <пользователь>",
            [SESSION_UI_LANGUAGE_DE] = "Nutzung: /getos <Benutzer>",
            [SESSION_UI_LANGUAGE_FR] = "Utilisation: /getos <utilisateur>",
            [SESSION_UI_LANGUAGE_PL] = "Użycie: /getos <użytkownik>",
        },
    },
    {
        "No operating system is recorded for %s.",
        {
            [SESSION_UI_LANGUAGE_KO] = "%s님의 운영체제 기록이 없습니다.",
            [SESSION_UI_LANGUAGE_JP] = "%s のOSは記録されていません。",
            [SESSION_UI_LANGUAGE_ZH] = "没有 %s 的操作系统记录。",
            [SESSION_UI_LANGUAGE_RU] =
                "Для %s операционная система не записана.",
            [SESSION_UI_LANGUAGE_DE] =
                "Für %s ist kein Betriebssystem gespeichert.",
            [SESSION_UI_LANGUAGE_FR] =
                "Aucun système d'exploitation enregistré pour %s.",
            [SESSION_UI_LANGUAGE_PL] =
                "Brak zapisanego systemu operacyjnego dla %s.",
        },
    },
    {
        "%s reports using %s.",
        {
            [SESSION_UI_LANGUAGE_KO] = "%s님은 %s을(를) 사용합니다.",
            [SESSION_UI_LANGUAGE_JP] = "%s は %s を使っています。",
            [SESSION_UI_LANGUAGE_ZH] = "%s 使用的是 %s。",
            [SESSION_UI_LANGUAGE_RU] = "%s использует %s.",
            [SESSION_UI_LANGUAGE_DE] = "%s nutzt %s.",
            [SESSION_UI_LANGUAGE_FR] = "%s utilise %s.",
            [SESSION_UI_LANGUAGE_PL] = "%s używa %s.",
        },
    },
    {
        "Only operators can reset passwords.",
        {
            [SESSION_UI_LANGUAGE_KO] =
                "운영자만 비밀번호를 초기화할 수 있습니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "パスワードをリセットできるのはオペレーターだけです。",
            [SESSION_UI_LANGUAGE_ZH] = "只有管理员可以重置密码。",
            [SESSION_UI_LANGUAGE_RU] =
                "Сбрасывать пароли могут только операторы.",
            [SESSION_UI_LANGUAGE_DE] =
                "Nur Operatoren können Passwörter zurücksetzen.",
            [SESSION_UI_LANGUAGE_FR] =
                "Seuls les opérateurs peuvent réinitialiser les mots de passe.",
            [SESSION_UI_LANGUAGE_PL] = "Tylko operatorzy mogą resetować hasła.",
        },
    },
    {
        "Usage: /resetpw <nickname>",
        {
            [SESSION_UI_LANGUAGE_KO] = "사용법: /resetpw <닉네임>",
            [SESSION_UI_LANGUAGE_JP] = "使い方: /resetpw <ニックネーム>",
            [SESSION_UI_LANGUAGE_ZH] = "用法: /resetpw <昵称>",
            [SESSION_UI_LANGUAGE_RU] = "Использование: /resetpw <ник>",
            [SESSION_UI_LANGUAGE_DE] = "Nutzung: /resetpw <Nickname>",
            [SESSION_UI_LANGUAGE_FR] = "Utilisation: /resetpw <pseudo>",
            [SESSION_UI_LANGUAGE_PL] = "Użycie: /resetpw <nick>",
        },
    },
    {
        "Could not find IP for user '%s'. Cannot reset password.",
        {
            [SESSION_UI_LANGUAGE_KO] = "사용자 '%s'의 IP를 찾을 수 없어 "
                                       "비밀번호를 초기화할 수 없습니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "ユーザー「%"
                "s」のIPが見つからないため、パスワードをリセットできません。",
            [SESSION_UI_LANGUAGE_ZH] = "找不到用户“%s”的 IP，无法重置密码。",
            [SESSION_UI_LANGUAGE_RU] =
                "Не найден IP пользователя «%s». Сбросить пароль нельзя.",
            [SESSION_UI_LANGUAGE_DE] =
                "IP für Benutzer „%s“ nicht gefunden. Passwort kann nicht "
                "zurückgesetzt werden.",
            [SESSION_UI_LANGUAGE_FR] = "IP introuvable pour « %s ». Impossible "
                                       "de réinitialiser le mot de passe.",
            [SESSION_UI_LANGUAGE_PL] = "Nie znaleziono IP użytkownika „%s”. "
                                       "Nie można zresetować hasła.",
        },
    },
    {
        "Failed to load data for user '%s'.",
        {
            [SESSION_UI_LANGUAGE_KO] =
                "사용자 '%s'의 데이터를 불러오지 못했습니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "ユーザー「%s」のデータを読み込めませんでした。",
            [SESSION_UI_LANGUAGE_ZH] = "无法加载用户“%s”的数据。",
            [SESSION_UI_LANGUAGE_RU] =
                "Не удалось загрузить данные пользователя «%s».",
            [SESSION_UI_LANGUAGE_DE] =
                "Daten für Benutzer „%s“ konnten nicht geladen werden.",
            [SESSION_UI_LANGUAGE_FR] =
                "Impossible de charger les données de « %s ».",
            [SESSION_UI_LANGUAGE_PL] =
                "Nie udało się wczytać danych użytkownika „%s”.",
        },
    },
    {
        "Password for '%s' has been reset.",
        {
            [SESSION_UI_LANGUAGE_KO] = "'%s'의 비밀번호를 초기화했습니다.",
            [SESSION_UI_LANGUAGE_JP] = "「%s」のパスワードをリセットしました。",
            [SESSION_UI_LANGUAGE_ZH] = "已重置“%s”的密码。",
            [SESSION_UI_LANGUAGE_RU] = "Пароль «%s» сброшен.",
            [SESSION_UI_LANGUAGE_DE] = "Passwort für „%s“ wurde zurückgesetzt.",
            [SESSION_UI_LANGUAGE_FR] =
                "Le mot de passe de « %s » a été réinitialisé.",
            [SESSION_UI_LANGUAGE_PL] = "Hasło „%s” zostało zresetowane.",
        },
    },
    {
        "Warning: unable to update pw_auth.dat.",
        {
            [SESSION_UI_LANGUAGE_KO] =
                "경고: pw_auth.dat을 갱신할 수 없습니다.",
            [SESSION_UI_LANGUAGE_JP] = "警告: pw_auth.dat を更新できません。",
            [SESSION_UI_LANGUAGE_ZH] = "警告：无法更新 pw_auth.dat。",
            [SESSION_UI_LANGUAGE_RU] =
                "Предупреждение: не удалось обновить pw_auth.dat.",
            [SESSION_UI_LANGUAGE_DE] =
                "Warnung: pw_auth.dat konnte nicht aktualisiert werden.",
            [SESSION_UI_LANGUAGE_FR] =
                "Avertissement : impossible de mettre à jour pw_auth.dat.",
            [SESSION_UI_LANGUAGE_PL] =
                "Ostrzeżenie: nie można zaktualizować pw_auth.dat.",
        },
    },
    {
        "Failed to reset password for '%s'.",
        {
            [SESSION_UI_LANGUAGE_KO] =
                "'%s'의 비밀번호를 초기화하지 못했습니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "「%s」のパスワードをリセットできませんでした。",
            [SESSION_UI_LANGUAGE_ZH] = "重置“%s”的密码失败。",
            [SESSION_UI_LANGUAGE_RU] = "Не удалось сбросить пароль «%s».",
            [SESSION_UI_LANGUAGE_DE] =
                "Passwort für „%s“ konnte nicht zurückgesetzt werden.",
            [SESSION_UI_LANGUAGE_FR] =
                "Échec de la réinitialisation du mot de passe de « %s ».",
            [SESSION_UI_LANGUAGE_PL] = "Nie udało się zresetować hasła „%s”.",
        },
    },
    {
        "Usage: /showstatus <nickname>",
        {
            [SESSION_UI_LANGUAGE_KO] = "사용법: /showstatus <닉네임>",
            [SESSION_UI_LANGUAGE_JP] = "使い方: /showstatus <ニックネーム>",
            [SESSION_UI_LANGUAGE_ZH] = "用法: /showstatus <昵称>",
            [SESSION_UI_LANGUAGE_RU] = "Использование: /showstatus <ник>",
            [SESSION_UI_LANGUAGE_DE] = "Nutzung: /showstatus <Nickname>",
            [SESSION_UI_LANGUAGE_FR] = "Utilisation: /showstatus <pseudo>",
            [SESSION_UI_LANGUAGE_PL] = "Użycie: /showstatus <nick>",
        },
    },
    {
        "User status lookup is unavailable.",
        {
            [SESSION_UI_LANGUAGE_KO] = "사용자 상태 조회를 사용할 수 없습니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "ユーザーステータスの照会は利用できません。",
            [SESSION_UI_LANGUAGE_ZH] = "用户状态查询不可用。",
            [SESSION_UI_LANGUAGE_RU] =
                "Просмотр статуса пользователей недоступен.",
            [SESSION_UI_LANGUAGE_DE] = "Statusabfrage ist nicht verfügbar.",
            [SESSION_UI_LANGUAGE_FR] =
                "La consultation du statut est indisponible.",
            [SESSION_UI_LANGUAGE_PL] = "Sprawdzanie statusu jest niedostępne.",
        },
    },
    {
        "No status message set for '%s'.",
        {
            [SESSION_UI_LANGUAGE_KO] =
                "'%s'님은 상태 메시지를 설정하지 않았습니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "「%s」はステータスメッセージを設定していません。",
            [SESSION_UI_LANGUAGE_ZH] = "“%s”未设置状态消息。",
            [SESSION_UI_LANGUAGE_RU] = "У «%s» нет статуса.",
            [SESSION_UI_LANGUAGE_DE] =
                "„%s“ hat keine Statusnachricht gesetzt.",
            [SESSION_UI_LANGUAGE_FR] = "« %s » n'a pas défini de statut.",
            [SESSION_UI_LANGUAGE_PL] = "„%s” nie ustawił statusu.",
        },
    },
    {
        "Status for %s: %s",
        {
            [SESSION_UI_LANGUAGE_KO] = "%s님의 상태: %s",
            [SESSION_UI_LANGUAGE_JP] = "%s のステータス: %s",
            [SESSION_UI_LANGUAGE_ZH] = "%s 的状态: %s",
            [SESSION_UI_LANGUAGE_RU] = "Статус %s: %s",
            [SESSION_UI_LANGUAGE_DE] = "Status von %s: %s",
            [SESSION_UI_LANGUAGE_FR] = "Statut de %s : %s",
            [SESSION_UI_LANGUAGE_PL] = "Status %s: %s",
        },
    },
    {
        "No blocked users or IPs.",
        {
            [SESSION_UI_LANGUAGE_KO] = "차단한 사용자나 IP가 없습니다.",
            [SESSION_UI_LANGUAGE_JP] = "ブロック中のユーザーやIPはありません。",
            [SESSION_UI_LANGUAGE_ZH] = "没有屏蔽的用户或 IP。",
            [SESSION_UI_LANGUAGE_RU] =
                "Нет заблокированных пользователей или IP.",
            [SESSION_UI_LANGUAGE_DE] = "Keine blockierten Benutzer oder IPs.",
            [SESSION_UI_LANGUAGE_FR] = "Aucun utilisateur ni IP bloqué.",
            [SESSION_UI_LANGUAGE_PL] =
                "Brak zablokowanych użytkowników lub IP.",
        },
    },
    {
        "Blocked targets:",
        {
            [SESSION_UI_LANGUAGE_KO] = "차단 대상:",
            [SESSION_UI_LANGUAGE_JP] = "ブロック対象:",
            [SESSION_UI_LANGUAGE_ZH] = "屏蔽对象:",
            [SESSION_UI_LANGUAGE_RU] = "Заблокированы:",
            [SESSION_UI_LANGUAGE_DE] = "Blockierte Ziele:",
            [SESSION_UI_LANGUAGE_FR] = "Cibles bloquées :",
            [SESSION_UI_LANGUAGE_PL] = "Zablokowane cele:",
        },
    },
    {
        "- %s (all users from this IP, originally [%s])",
        {
            [SESSION_UI_LANGUAGE_KO] =
                "- %s (이 IP의 모든 사용자, 처음 대상 [%s])",
            [SESSION_UI_LANGUAGE_JP] =
                "- %s（このIPの全ユーザー、元の対象 [%s]）",
            [SESSION_UI_LANGUAGE_ZH] = "- %s（该 IP 的所有用户，最初为 [%s]）",
            [SESSION_UI_LANGUAGE_RU] =
                "- %s (все пользователи с этого IP, изначально [%s])",
            [SESSION_UI_LANGUAGE_DE] =
                "- %s (alle Benutzer dieser IP, ursprünglich [%s])",
            [SESSION_UI_LANGUAGE_FR] =
                "- %s (tous les utilisateurs de cette IP, à l'origine [%s])",
            [SESSION_UI_LANGUAGE_PL] =
                "- %s (wszyscy użytkownicy z tego IP, pierwotnie [%s])",
        },
    },
    {
        "- %s (all users from this IP)",
        {
            [SESSION_UI_LANGUAGE_KO] = "- %s (이 IP의 모든 사용자)",
            [SESSION_UI_LANGUAGE_JP] = "- %s（このIPの全ユーザー）",
            [SESSION_UI_LANGUAGE_ZH] = "- %s（该 IP 的所有用户）",
            [SESSION_UI_LANGUAGE_RU] = "- %s (все пользователи с этого IP)",
            [SESSION_UI_LANGUAGE_DE] = "- %s (alle Benutzer dieser IP)",
            [SESSION_UI_LANGUAGE_FR] =
                "- %s (tous les utilisateurs de cette IP)",
            [SESSION_UI_LANGUAGE_PL] = "- %s (wszyscy użytkownicy z tego IP)",
        },
    },
    {
        "- [%s] (only this user, IP %s)",
        {
            [SESSION_UI_LANGUAGE_KO] = "- [%s] (이 사용자만, IP %s)",
            [SESSION_UI_LANGUAGE_JP] = "- [%s]（このユーザーのみ、IP %s）",
            [SESSION_UI_LANGUAGE_ZH] = "- [%s]（仅此用户，IP %s）",
            [SESSION_UI_LANGUAGE_RU] =
                "- [%s] (только этот пользователь, IP %s)",
            [SESSION_UI_LANGUAGE_DE] = "- [%s] (nur dieser Benutzer, IP %s)",
            [SESSION_UI_LANGUAGE_FR] =
                "- [%s] (cet utilisateur seulement, IP %s)",
            [SESSION_UI_LANGUAGE_PL] = "- [%s] (tylko ten użytkownik, IP %s)",
        },
    },
    {
        "- entry #%zu",
        {
            [SESSION_UI_LANGUAGE_KO] = "- 항목 #%zu",
            [SESSION_UI_LANGUAGE_JP] = "- 項目 #%zu",
            [SESSION_UI_LANGUAGE_ZH] = "- 条目 #%zu",
            [SESSION_UI_LANGUAGE_RU] = "- запись #%zu",
            [SESSION_UI_LANGUAGE_DE] = "- Eintrag #%zu",
            [SESSION_UI_LANGUAGE_FR] = "- entrée #%zu",
            [SESSION_UI_LANGUAGE_PL] = "- wpis #%zu",
        },
    },
    {
        "Delivered mailbox message to %s (DDial).",
        {
            [SESSION_UI_LANGUAGE_KO] = "%s님의 DDial 메일함으로 보냈습니다.",
            [SESSION_UI_LANGUAGE_JP] =
                "%s の DDial メールボックスに配達しました。",
            [SESSION_UI_LANGUAGE_ZH] = "已投递到 %s 的 DDial 邮箱。",
            [SESSION_UI_LANGUAGE_RU] =
                "Сообщение доставлено в почтовый ящик DDial пользователя %s.",
            [SESSION_UI_LANGUAGE_DE] =
                "Nachricht an das DDial-Postfach von %s zugestellt.",
            [SESSION_UI_LANGUAGE_FR] =
                "Message remis dans la boîte DDial de %s.",
            [SESSION_UI_LANGUAGE_PL] =
                "Dostarczono wiadomość do skrzynki DDial użytkownika %s.",
        },
    },
    {
        "No DDial member has that name.",
        {
            [SESSION_UI_LANGUAGE_KO] = "그 이름의 DDial 회원이 없습니다.",
            [SESSION_UI_LANGUAGE_JP] = "その名前の DDial 会員はいません。",
            [SESSION_UI_LANGUAGE_ZH] = "没有该名字的 DDial 会员。",
            [SESSION_UI_LANGUAGE_RU] = "Нет участника DDial с таким именем.",
            [SESSION_UI_LANGUAGE_DE] = "Kein DDial-Mitglied hat diesen Namen.",
            [SESSION_UI_LANGUAGE_FR] = "Aucun membre DDial ne porte ce nom.",
            [SESSION_UI_LANGUAGE_PL] = "Brak członka DDial o tej nazwie.",
        },
    },
    {
        "That DDial mailbox is full.",
        {
            [SESSION_UI_LANGUAGE_KO] = "해당 DDial 메일함이 가득 찼습니다.",
            [SESSION_UI_LANGUAGE_JP] = "その DDial メールボックスは満杯です。",
            [SESSION_UI_LANGUAGE_ZH] = "该 DDial 邮箱已满。",
            [SESSION_UI_LANGUAGE_RU] = "Этот почтовый ящик DDial переполнен.",
            [SESSION_UI_LANGUAGE_DE] = "Dieses DDial-Postfach ist voll.",
            [SESSION_UI_LANGUAGE_FR] = "Cette boîte DDial est pleine.",
            [SESSION_UI_LANGUAGE_PL] = "Ta skrzynka DDial jest pełna.",
        },
    },
    {
        "You have new mail from %s. Use /mail to read it.",
        {
            [SESSION_UI_LANGUAGE_KO] =
                "%s님에게서 새 메일이 왔습니다. /mail로 확인하세요.",
            [SESSION_UI_LANGUAGE_JP] =
                "%s から新しいメールが届きました。/mail で読めます。",
            [SESSION_UI_LANGUAGE_ZH] =
                "你收到来自 %s 的新邮件。使用 /mail 查看。",
            [SESSION_UI_LANGUAGE_RU] = "Новое письмо от %s. Прочитать: /mail.",
            [SESSION_UI_LANGUAGE_DE] =
                "Neue Nachricht von %s. Lesen mit /mail.",
            [SESSION_UI_LANGUAGE_FR] =
                "Nouveau message de %s. Lisez-le avec /mail.",
            [SESSION_UI_LANGUAGE_PL] =
                "Nowa wiadomość od %s. Przeczytaj przez /mail.",
        },
    },
    {
        "Only operators may control AI members.",
        {
            [SESSION_UI_LANGUAGE_KO] = "AI 멤버는 운영자만 제어할 수 있습니다.",
            [SESSION_UI_LANGUAGE_JP] = "AI メンバーを操作できるのはオペレーターだけです。",
            [SESSION_UI_LANGUAGE_ZH] = "只有管理员可以控制 AI 成员。",
            [SESSION_UI_LANGUAGE_RU] = "Управлять AI-участниками могут только операторы.",
            [SESSION_UI_LANGUAGE_DE] = "Nur Operatoren dürfen KI-Mitglieder steuern.",
            [SESSION_UI_LANGUAGE_FR] = "Seuls les opérateurs peuvent gérer les membres IA.",
            [SESSION_UI_LANGUAGE_PL] = "Tylko operatorzy mogą sterować członkami AI.",
        },
    },
    {
        "%s (on)",
        {
            [SESSION_UI_LANGUAGE_KO] = "%s (켜짐)",
            [SESSION_UI_LANGUAGE_JP] = "%s (オン)",
            [SESSION_UI_LANGUAGE_ZH] = "%s（开）",
            [SESSION_UI_LANGUAGE_RU] = "%s (вкл)",
            [SESSION_UI_LANGUAGE_DE] = "%s (an)",
            [SESSION_UI_LANGUAGE_FR] = "%s (activé)",
            [SESSION_UI_LANGUAGE_PL] = "%s (wł.)",
        },
    },
    {
        "%s (off)",
        {
            [SESSION_UI_LANGUAGE_KO] = "%s (꺼짐)",
            [SESSION_UI_LANGUAGE_JP] = "%s (オフ)",
            [SESSION_UI_LANGUAGE_ZH] = "%s（关）",
            [SESSION_UI_LANGUAGE_RU] = "%s (выкл)",
            [SESSION_UI_LANGUAGE_DE] = "%s (aus)",
            [SESSION_UI_LANGUAGE_FR] = "%s (désactivé)",
            [SESSION_UI_LANGUAGE_PL] = "%s (wył.)",
        },
    },
    {
        "AI members: %s",
        {
            [SESSION_UI_LANGUAGE_KO] = "AI 멤버: %s",
            [SESSION_UI_LANGUAGE_JP] = "AI メンバー: %s",
            [SESSION_UI_LANGUAGE_ZH] = "AI 成员：%s",
            [SESSION_UI_LANGUAGE_RU] = "AI-участники: %s",
            [SESSION_UI_LANGUAGE_DE] = "KI-Mitglieder: %s",
            [SESSION_UI_LANGUAGE_FR] = "Membres IA : %s",
            [SESSION_UI_LANGUAGE_PL] = "Członkowie AI: %s",
        },
    },
    {
        "Small talk backend: %s.",
        {
            [SESSION_UI_LANGUAGE_KO] = "잡담 백엔드: %s.",
            [SESSION_UI_LANGUAGE_JP] = "雑談バックエンド: %s。",
            [SESSION_UI_LANGUAGE_ZH] = "闲聊后端：%s。",
            [SESSION_UI_LANGUAGE_RU] = "Бэкенд болтовни: %s.",
            [SESSION_UI_LANGUAGE_DE] = "Smalltalk-Backend: %s.",
            [SESSION_UI_LANGUAGE_FR] = "Moteur de discussion : %s.",
            [SESSION_UI_LANGUAGE_PL] = "Backend pogawędek: %s.",
        },
    },
    {
        "Usage: /ai-member <on|off> [name ...|all] [use-gemini]",
        {
            [SESSION_UI_LANGUAGE_KO] = "사용법: /ai-member <켜기|끄기> [이름 ...|모두] [use-gemini]",
            [SESSION_UI_LANGUAGE_JP] = "使い方: /ai-member <オン|オフ> [名前 ...|すべて] [use-gemini]",
            [SESSION_UI_LANGUAGE_ZH] = "用法：/ai-member <开|关> [名字 ...|全部] [use-gemini]",
            [SESSION_UI_LANGUAGE_RU] = "Использование: /ai-member <вкл|выкл> [имя ...|все] [use-gemini]",
            [SESSION_UI_LANGUAGE_DE] = "Verwendung: /ai-member <on|off> [Name ...|all] [use-gemini]",
            [SESSION_UI_LANGUAGE_FR] = "Usage : /ai-member <on|off> [nom ...|all] [use-gemini]",
            [SESSION_UI_LANGUAGE_PL] = "Użycie: /ai-member <on|off> [nazwa ...|all] [use-gemini]",
        },
    },
    {
        "Unknown AI member '%s'. Choose from: %s.",
        {
            [SESSION_UI_LANGUAGE_KO] = "'%s'라는 AI 멤버는 없습니다. 선택 가능: %s.",
            [SESSION_UI_LANGUAGE_JP] = "AI メンバー「%s」は存在しません。選択肢: %s。",
            [SESSION_UI_LANGUAGE_ZH] = "没有名为“%s”的 AI 成员。可选：%s。",
            [SESSION_UI_LANGUAGE_RU] = "AI-участника «%s» нет. Доступны: %s.",
            [SESSION_UI_LANGUAGE_DE] = "Unbekanntes KI-Mitglied „%s“. Zur Wahl: %s.",
            [SESSION_UI_LANGUAGE_FR] = "Membre IA « %s » inconnu. Choix possibles : %s.",
            [SESSION_UI_LANGUAGE_PL] = "Nieznany członek AI „%s”. Do wyboru: %s.",
        },
    },
    {
        "%s is busy right now; try again soon.",
        {
            [SESSION_UI_LANGUAGE_KO] = "%s은(는) 지금 바쁩니다. 잠시 후 다시 시도하세요.",
            [SESSION_UI_LANGUAGE_JP] = "%s は今忙しいようです。少ししてからもう一度どうぞ。",
            [SESSION_UI_LANGUAGE_ZH] = "%s 现在正忙，请稍后再试。",
            [SESSION_UI_LANGUAGE_RU] = "%s сейчас занят(а); попробуйте чуть позже.",
            [SESSION_UI_LANGUAGE_DE] = "%s ist gerade beschäftigt; versuche es gleich noch einmal.",
            [SESSION_UI_LANGUAGE_FR] = "%s est occupé(e) pour le moment ; réessayez bientôt.",
            [SESSION_UI_LANGUAGE_PL] = "%s jest teraz zajęty; spróbuj za chwilę.",
        },
    },
};

// Returns the message in the session's UI language, or the English text when
// no translation exists. Format specifiers match the English original.
static const char *session_command_localize(const session_ctx_t *ctx,
                                            const char *english)
{
    if (english == nullptr) {
        return nullptr;
    }

    const size_t language = (size_t)session_ui_language_current(ctx);
    if (language == (size_t)SESSION_UI_LANGUAGE_EN ||
        language >= SESSION_UI_LANGUAGE_COUNT) {
        return english;
    }

    for (size_t idx = 0U; idx < sizeof(kSessionCommandMessages) /
                                    sizeof(kSessionCommandMessages[0]);
         ++idx) {
        const session_command_message_t *entry = &kSessionCommandMessages[idx];
        if (strcmp(entry->english, english) == 0) {
            const char *translated = entry->localized[language];
            return translated != nullptr ? translated : english;
        }
    }
    return english;
}

// snprintf with a localized format; the translations keep the English
// conversion specifiers in the same order.
#define session_command_snprintf(ctx, buffer, length, english, ...)            \
    do {                                                                       \
        _Pragma("GCC diagnostic push")                                         \
        _Pragma("GCC diagnostic ignored \"-Wformat-nonliteral\"")              \
        snprintf((buffer), (length),                                           \
                 session_command_localize((ctx), (english)), __VA_ARGS__);     \
        _Pragma("GCC diagnostic pop")                                          \
    } while (0)

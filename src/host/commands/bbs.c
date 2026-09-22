static const char *session_bbs_localize(session_ui_language_t lang, const char *msg)
{
    if (msg == nullptr) return nullptr;
    switch (lang) {
    case SESSION_UI_LANGUAGE_KO:
        if (strcmp(msg, "That post is no longer available.") == 0) return "해당 게시물을 더 이상 사용할 수 없습니다.";
        if (strcmp(msg, "BBS storage is unavailable.") == 0) return "BBS 저장소를 사용할 수 없습니다.";
        if (strcmp(msg, "No post exists with that identifier.") == 0) return "해당 ID의 게시물이 존재하지 않습니다.";
        if (strcmp(msg, "The bulletin board is empty.") == 0) return "게시판이 비어 있습니다.";
        if (strcmp(msg, "Post body was empty. Draft discarded.") == 0) return "게시물 본문이 비어 있어 임시저장이 취소되었습니다.";
        if (strcmp(msg, "The bulletin board is full right now.") == 0) return "현재 게시판이 가득 찼습니다.";
        if (strcmp(msg, "A title is required to create a post.") == 0) return "게시물을 작성하려면 제목이 필요합니다.";
        if (strcmp(msg, "Invalid post identifier.") == 0) return "유효하지 않은 게시물 ID입니다.";
        if (strcmp(msg, "This post has reached the comment limit.") == 0) return "이 게시물은 댓글 한도에 도달했습니다.";
        if (strcmp(msg, "Post deleted.") == 0) return "게시물이 삭제되었습니다.";
        if (strcmp(msg, "The bulletin board is unavailable right now.") == 0) return "현재 게시판을 사용할 수 없습니다.";
        if (strcmp(msg, "Unable to allocate editor workspace.") == 0) return "편집기 작업 공간을 할당할 수 없습니다.";
        if (strcmp(msg, "Draft saved.") == 0) return "임시저장이 완료되었습니다.";
        if (strcmp(msg, "Draft loaded.") == 0) return "임시저장된 글을 불러왔습니다.";
        if (strcmp(msg, "Comment added.") == 0) return "댓글이 추가되었습니다.";
        if (strcmp(msg, "You cannot recommend your own post.") == 0) return "자신의 게시물은 추천할 수 없습니다.";
        if (strcmp(msg, "You have already voted on this post.") == 0) return "이미 이 게시물에 투표하셨습니다.";
        if (strcmp(msg, "Recommendation added.") == 0) return "추천이 추가되었습니다.";
        if (strcmp(msg, "Oppose vote added.") == 0) return "반대 투표가 추가되었습니다.";
        if (strcmp(msg, "Joined board: %s — %s") == 0) return "게시판에 입장했습니다: %s — %s";
        if (strcmp(msg, "Board not found.") == 0) return "게시판을 찾을 수 없습니다.";
        if (strcmp(msg, "Comment editing is disabled.") == 0) return "댓글 수정이 비활성화되었습니다.";
        if (strcmp(msg, "Comment deletion is disabled.") == 0) return "댓글 삭제가 비활성화되었습니다.";
        if (strcmp(msg, "Invalid post or comment index.") == 0) return "유효하지 않은 게시물 또는 댓글 인덱스입니다.";
        if (strcmp(msg, "Only the author or an operator may edit this comment.") == 0) return "댓글 작성자 또는 오퍼레이터만 이 댓글을 수정할 수 있습니다.";
        if (strcmp(msg, "Only the author or an operator may delete this comment.") == 0) return "댓글 작성자 또는 오퍼레이터만 이 댓글을 삭제할 수 있습니다.";
        if (strcmp(msg, "Comment updated.") == 0) return "댓글이 수정되었습니다.";
        if (strcmp(msg, "Comment deleted.") == 0) return "댓글이 삭제되었습니다.";
        if (strcmp(msg, "  Choose your Profile Logo (Avatar)") == 0) return "  프로필 로고(아바타)를 선택하세요";
        if (strcmp(msg, "  Enter choice (1-5):") == 0) return "  선택 입력 (1-5):";
        if (strcmp(msg, "  [1] Monitor") == 0) return "  [1] Monitor";
        if (strcmp(msg, "  [2] Mouse") == 0) return "  [2] Mouse";
        if (strcmp(msg, "  [3] Human") == 0) return "  [3] Human";
        if (strcmp(msg, "  [4] Mushroom") == 0) return "  [4] Mushroom";
        if (strcmp(msg, "  [5] None / Keep Current") == 0) return "  [5] 없음 / 현재 유지";
        if (strcmp(msg, " - Read a post:   Type '\033[1;32mread <id>\033[0m'") == 0) return " - 게시물 읽기:   '\033[1;32mread <id>\033[0m' 입력";
        if (strcmp(msg, " - Write a post:  Type '\033[1;32mpost <title>\033[0m'") == 0) return " - 게시물 작성:  '\033[1;32mpost <title>\033[0m' 입력";
        if (strcmp(msg, "(No profile picture set)") == 0) return "(설정된 프로필 사진 없음)";
        if (strcmp(msg, "----------------------------------------") == 0) return "----------------------------------------";
        if (strcmp(msg, "--------------------------------------------------") == 0) return "--------------------------------------------------";
        if (strcmp(msg, "========================================") == 0) return "========================================";
        if (strcmp(msg, "Already viewing the end of this post.") == 0) return "이미 이 게시물의 끝을 보고 있습니다.";
        if (strcmp(msg, "Already viewing the top of this post.") == 0) return "이미 이 게시물의 처음을 보고 있습니다.";
        if (strcmp(msg, "Available avatars:") == 0) return "사용 가능한 아바타:";
        if (strcmp(msg, "Downvoted.") == 0) return "반대했습니다.";
        if (strcmp(msg, "Draft deleted.") == 0) return "임시저장이 삭제되었습니다.";
        if (strcmp(msg, "Draft loaded. Continue editing with the post editor.") == 0) return "임시저장을 불러왔습니다. 게시물 편집기로 계속 수정하세요.";
        if (strcmp(msg, "Draft not found.") == 0) return "임시저장을 찾을 수 없습니다.";
        if (strcmp(msg, "Draft storage full. Delete old drafts first.") == 0) return "임시저장 공간이 가득 찼습니다. 먼저 오래된 임시저장을 삭제하세요.";
        if (strcmp(msg, "Failed to initialize user data.") == 0) return "사용자 데이터를 초기화하지 못했습니다.";
        if (strcmp(msg, "Failed to save avatar.") == 0) return "아바타를 저장하지 못했습니다.";
        if (strcmp(msg, "Failed to save profile logo.") == 0) return "프로필 로고를 저장하지 못했습니다.";
        if (strcmp(msg, "Failed to save profile picture.") == 0) return "프로필 사진을 저장하지 못했습니다.";
        if (strcmp(msg, "Missing closing quote for the title.") == 0) return "제목의 닫는 따옴표가 없습니다.";
        if (strcmp(msg, "No ASCII art pending. Use /asciiart to create one first.") == 0) return "대기 중인 ASCII 아트가 없습니다. 먼저 /asciiart로 만드세요.";
        if (strcmp(msg, "No drafts.") == 0) return "임시저장이 없습니다.";
        if (strcmp(msg, "No post exists with that identifier anymore.") == 0) return "해당 ID의 게시물이 더 이상 존재하지 않습니다.";
        if (strcmp(msg, "No posts found matching the keyword.") == 0) return "키워드와 일치하는 게시물이 없습니다.";
        if (strcmp(msg, "Nothing to save. Start a post first.") == 0) return "저장할 내용이 없습니다. 먼저 게시물을 작성하세요.";
        if (strcmp(msg, "Only the author or an operator may delete this post.") == 0) return "작성자 또는 운영자만 이 게시물을 삭제할 수 있습니다.";
        if (strcmp(msg, "Only the author or an operator may edit this post.") == 0) return "작성자 또는 운영자만 이 게시물을 수정할 수 있습니다.";
        if (strcmp(msg, "Post bumped to the top.") == 0) return "게시물이 맨 위로 올라갔습니다.";
        if (strcmp(msg, "Post created.") == 0) return "게시물이 작성되었습니다.";
        if (strcmp(msg, "Post updated.") == 0) return "게시물이 수정되었습니다.";
        if (strcmp(msg, "Profile picture updated.") == 0) return "프로필 사진이 업데이트되었습니다.";
        if (strcmp(msg, "Recent posts:") == 0) return "최근 게시물:";
        if (strcmp(msg, "Specify a topic to read.") == 0) return "읽을 주제를 지정하세요.";
        if (strcmp(msg, "Unknown avatar name.") == 0) return "알 수 없는 아바타 이름입니다.";
        if (strcmp(msg, "Upvoted.") == 0) return "추천했습니다.";
        if (strcmp(msg, "Usage: /bbs cmtvote <post_id> <comment_idx> [up|down]") == 0) return "사용법: /bbs cmtvote <post_id> <comment_idx> [up|down]";
        if (strcmp(msg, "Usage: /bbs draft <save|list|load|delete>") == 0) return "사용법: /bbs draft <save|list|load|delete>";
        if (strcmp(msg, "Usage: /bbs draft delete <index>") == 0) return "사용법: /bbs draft delete <index>";
        if (strcmp(msg, "Usage: /bbs draft load <index>") == 0) return "사용법: /bbs draft load <index>";
        if (strcmp(msg, "Usage: /bbs setavatar <monitor|mouse|human|mushroom|none>") == 0) return "사용법: /bbs setavatar <monitor|mouse|human|mushroom|none>";
        if (strcmp(msg, "Usage: search <keyword>") == 0) return "사용법: search <keyword>";
        if (strcmp(msg, "Use | to separate tags when the title has spaces.") == 0) return "제목에 공백이 있으면 |로 태그를 구분하세요.";
        if (strcmp(msg, "You already downvoted this post.") == 0) return "이미 이 게시물에 반대하셨습니다.";
        if (strcmp(msg, "You already upvoted this post.") == 0) return "이미 이 게시물을 추천하셨습니다.";
        if (strcmp(msg, "You already voted this comment.") == 0) return "이미 이 댓글에 투표하셨습니다.";
        if (strcmp(msg, "[Tip] Share your thoughts on the board!") == 0) return "[팁] 게시판에 의견을 남겨보세요!";
        if (strcmp(msg, "\033[1;32m>>> Baud Rate: 14400 bps | Terminal: ANSI-BBS | STATUS: ONLINE <<<\033[0m\n") == 0) return "\033[1;32m>>> 통신 속도: 14400 bps | 단말기: ANSI-BBS | 상태: 온라인 <<<\033[0m\n";
        if (strcmp(msg, "\n\033[1;36m>>> Connecting to Retro BBS Network... <<<\033[0m") == 0) return "\n\033[1;36m>>> 레트로 BBS 네트워크에 접속 중... <<<\033[0m";
        if (strcmp(msg, "No posts on that page.") == 0) return "해당 페이지에 게시물이 없습니다.";
        if (strcmp(msg, "Page %zu/%zu (next: list %s %zu)") == 0) return "페이지 %zu/%zu (다음: list %s %zu)";
        if (strcmp(msg, "Page %zu/%zu") == 0) return "페이지 %zu/%zu";
        if (strcmp(msg, "You are commenting too quickly. Try again in a moment.") == 0) return "댓글을 너무 빨리 달고 있습니다. 잠시 후 다시 시도하세요.";
        if (strcmp(msg, "You are posting too quickly. Try again in a moment.") == 0) return "게시물을 너무 빨리 작성하고 있습니다. 잠시 후 다시 시도하세요.";
        if (strcmp(msg, "--- Notifications ---") == 0) return "--- 알림 ---";
        if (strcmp(msg, "%s replied to your post #%llu.") == 0) return "%s님이 회원님의 게시물 #%llu에 댓글을 달았습니다.";
        if (strcmp(msg, "%s mentioned you in post #%llu.") == 0) return "%s님이 게시물 #%llu에서 회원님을 언급했습니다.";
        if (strcmp(msg, "%llu new posts since your last visit.") == 0) return "지난 방문 이후 새 게시물 %llu개.";
        break;
    case SESSION_UI_LANGUAGE_JP:
        if (strcmp(msg, "That post is no longer available.") == 0) return "その投稿は利用できなくなりました。";
        if (strcmp(msg, "BBS storage is unavailable.") == 0) return "BBSストレージを利用できません。";
        if (strcmp(msg, "No post exists with that identifier.") == 0) return "そのIDの投稿は存在しません。";
        if (strcmp(msg, "The bulletin board is empty.") == 0) return "掲示板は空です。";
        if (strcmp(msg, "Post body was empty. Draft discarded.") == 0) return "投稿本文が空のため、下書きは破棄されました。";
        if (strcmp(msg, "The bulletin board is full right now.") == 0) return "現在掲示板がいっぱいです。";
        if (strcmp(msg, "A title is required to create a post.") == 0) return "投稿を作成するにはタイトルが必要です。";
        if (strcmp(msg, "Invalid post identifier.") == 0) return "無効な投稿IDです。";
        if (strcmp(msg, "This post has reached the comment limit.") == 0) return "この投稿はコメント数の上限に達しました。";
        if (strcmp(msg, "Post deleted.") == 0) return "投稿が削除されました。";
        if (strcmp(msg, "The bulletin board is unavailable right now.") == 0) return "現在掲示板を利用できません。";
        if (strcmp(msg, "Unable to allocate editor workspace.") == 0) return "エディタ of ワークスペースを割り当てられません。";
        if (strcmp(msg, "Draft saved.") == 0) return "下書きが保存されました。";
        if (strcmp(msg, "Draft loaded.") == 0) return "下書きが読み込まれました。";
        if (strcmp(msg, "Comment added.") == 0) return "コメントが追加されました。";
        if (strcmp(msg, "You cannot recommend your own post.") == 0) return "自分の投稿を推薦することはできません。";
        if (strcmp(msg, "You have already voted on this post.") == 0) return "この投稿には既に投票済みです。";
        if (strcmp(msg, "Recommendation added.") == 0) return "推薦が追加されました。";
        if (strcmp(msg, "Oppose vote added.") == 0) return "反対票が追加されました。";
        if (strcmp(msg, "Joined board: %s — %s") == 0) return "掲示板に移動しました: %s — %s";
        if (strcmp(msg, "Board not found.") == 0) return "掲示板が見つかりません。";
        if (strcmp(msg, "Comment editing is disabled.") == 0) return "コメントの編集は無効になっています。";
        if (strcmp(msg, "Comment deletion is disabled.") == 0) return "コメントの削除は無効になっています。";
        if (strcmp(msg, "Invalid post or comment index.") == 0) return "無効な投稿またはコメントのインデックスです。";
        if (strcmp(msg, "Only the author or an operator may edit this comment.") == 0) return "コメントの作者またはオペレーターのみがこのコメントを編集できます。";
        if (strcmp(msg, "Only the author or an operator may delete this comment.") == 0) return "コメントの作者またはオペレーターのみがこのコメントを削除できます。";
        if (strcmp(msg, "Comment updated.") == 0) return "コメントが更新されました。";
        if (strcmp(msg, "Comment deleted.") == 0) return "コメントが削除されました。";
        if (strcmp(msg, "  Choose your Profile Logo (Avatar)") == 0) return "  プロフィールロゴ（アバター）を選択してください";
        if (strcmp(msg, "  Enter choice (1-5):") == 0) return "  選択してください (1-5):";
        if (strcmp(msg, "  [1] Monitor") == 0) return "  [1] Monitor";
        if (strcmp(msg, "  [2] Mouse") == 0) return "  [2] Mouse";
        if (strcmp(msg, "  [3] Human") == 0) return "  [3] Human";
        if (strcmp(msg, "  [4] Mushroom") == 0) return "  [4] Mushroom";
        if (strcmp(msg, "  [5] None / Keep Current") == 0) return "  [5] なし / 現在を保持";
        if (strcmp(msg, " - Read a post:   Type '\033[1;32mread <id>\033[0m'") == 0) return " - 投稿を読む:   '\033[1;32mread <id>\033[0m' と入力";
        if (strcmp(msg, " - Write a post:  Type '\033[1;32mpost <title>\033[0m'") == 0) return " - 投稿する:  '\033[1;32mpost <title>\033[0m' と入力";
        if (strcmp(msg, "(No profile picture set)") == 0) return "(プロフィール画像が設定されていません)";
        if (strcmp(msg, "----------------------------------------") == 0) return "----------------------------------------";
        if (strcmp(msg, "--------------------------------------------------") == 0) return "--------------------------------------------------";
        if (strcmp(msg, "========================================") == 0) return "========================================";
        if (strcmp(msg, "Already viewing the end of this post.") == 0) return "すでにこの投稿の末尾を表示しています。";
        if (strcmp(msg, "Already viewing the top of this post.") == 0) return "すでにこの投稿の先頭を表示しています。";
        if (strcmp(msg, "Available avatars:") == 0) return "利用可能なアバター:";
        if (strcmp(msg, "Downvoted.") == 0) return "反対投票しました。";
        if (strcmp(msg, "Draft deleted.") == 0) return "下書きが削除されました。";
        if (strcmp(msg, "Draft loaded. Continue editing with the post editor.") == 0) return "下書きを読み込みました。投稿エディタで編集を続けてください。";
        if (strcmp(msg, "Draft not found.") == 0) return "下書きが見つかりません。";
        if (strcmp(msg, "Draft storage full. Delete old drafts first.") == 0) return "下書きの容量がいっぱいです。先に古い下書きを削除してください。";
        if (strcmp(msg, "Failed to initialize user data.") == 0) return "ユーザーデータの初期化に失敗しました。";
        if (strcmp(msg, "Failed to save avatar.") == 0) return "アバターの保存に失敗しました。";
        if (strcmp(msg, "Failed to save profile logo.") == 0) return "プロフィールロゴの保存に失敗しました。";
        if (strcmp(msg, "Failed to save profile picture.") == 0) return "プロフィール画像の保存に失敗しました。";
        if (strcmp(msg, "Missing closing quote for the title.") == 0) return "タイトルの閉じる引用符がありません。";
        if (strcmp(msg, "No ASCII art pending. Use /asciiart to create one first.") == 0) return "保留中のASCIIアートがありません。先に /asciiart で作成してください。";
        if (strcmp(msg, "No drafts.") == 0) return "下書きはありません。";
        if (strcmp(msg, "No post exists with that identifier anymore.") == 0) return "そのIDの投稿はもう存在しません。";
        if (strcmp(msg, "No posts found matching the keyword.") == 0) return "キーワードに一致する投稿が見つかりません。";
        if (strcmp(msg, "Nothing to save. Start a post first.") == 0) return "保存するものがありません。先に投稿を開始してください。";
        if (strcmp(msg, "Only the author or an operator may delete this post.") == 0) return "投稿者またはオペレーターのみがこの投稿を削除できます。";
        if (strcmp(msg, "Only the author or an operator may edit this post.") == 0) return "投稿者またはオペレーターのみがこの投稿を編集できます。";
        if (strcmp(msg, "Post bumped to the top.") == 0) return "投稿が一番上に移動しました。";
        if (strcmp(msg, "Post created.") == 0) return "投稿が作成されました。";
        if (strcmp(msg, "Post updated.") == 0) return "投稿が更新されました。";
        if (strcmp(msg, "Profile picture updated.") == 0) return "プロフィール画像が更新されました。";
        if (strcmp(msg, "Recent posts:") == 0) return "最近の投稿:";
        if (strcmp(msg, "Specify a topic to read.") == 0) return "読むトピックを指定してください。";
        if (strcmp(msg, "Unknown avatar name.") == 0) return "不明なアバター名です。";
        if (strcmp(msg, "Upvoted.") == 0) return "賛成投票しました。";
        if (strcmp(msg, "Usage: /bbs cmtvote <post_id> <comment_idx> [up|down]") == 0) return "使い方: /bbs cmtvote <post_id> <comment_idx> [up|down]";
        if (strcmp(msg, "Usage: /bbs draft <save|list|load|delete>") == 0) return "使い方: /bbs draft <save|list|load|delete>";
        if (strcmp(msg, "Usage: /bbs draft delete <index>") == 0) return "使い方: /bbs draft delete <index>";
        if (strcmp(msg, "Usage: /bbs draft load <index>") == 0) return "使い方: /bbs draft load <index>";
        if (strcmp(msg, "Usage: /bbs setavatar <monitor|mouse|human|mushroom|none>") == 0) return "使い方: /bbs setavatar <monitor|mouse|human|mushroom|none>";
        if (strcmp(msg, "Usage: search <keyword>") == 0) return "使い方: search <keyword>";
        if (strcmp(msg, "Use | to separate tags when the title has spaces.") == 0) return "タイトルに空白が含まれる場合は | でタグを区切ってください。";
        if (strcmp(msg, "You already downvoted this post.") == 0) return "すでにこの投稿に反対票を投じています。";
        if (strcmp(msg, "You already upvoted this post.") == 0) return "すでにこの投稿に賛成票を投じています。";
        if (strcmp(msg, "You already voted this comment.") == 0) return "すでにこのコメントに投票しています。";
        if (strcmp(msg, "[Tip] Share your thoughts on the board!") == 0) return "[ヒント] 掲示板で感想を共有しましょう!";
        if (strcmp(msg, "\033[1;32m>>> Baud Rate: 14400 bps | Terminal: ANSI-BBS | STATUS: ONLINE <<<\033[0m\n") == 0) return "\033[1;32m>>> ボーレート: 14400 bps | 端末: ANSI-BBS | 状態: オンライン <<<\033[0m\n";
        if (strcmp(msg, "\n\033[1;36m>>> Connecting to Retro BBS Network... <<<\033[0m") == 0) return "\n\033[1;36m>>> レトロBBSネットワークに接続中... <<<\033[0m";
        if (strcmp(msg, "No posts on that page.") == 0) return "そのページには投稿がありません。";
        if (strcmp(msg, "Page %zu/%zu (next: list %s %zu)") == 0) return "ページ %zu/%zu (次へ: list %s %zu)";
        if (strcmp(msg, "Page %zu/%zu") == 0) return "ページ %zu/%zu";
        if (strcmp(msg, "You are commenting too quickly. Try again in a moment.") == 0) return "コメントの送信が速すぎます。少し待ってから再試行してください。";
        if (strcmp(msg, "You are posting too quickly. Try again in a moment.") == 0) return "投稿の頻度が速すぎます。少し待ってから再試行してください。";
        if (strcmp(msg, "--- Notifications ---") == 0) return "--- 通知 ---";
        if (strcmp(msg, "%s replied to your post #%llu.") == 0) return "%s があなたの投稿 #%llu にコメントしました。";
        if (strcmp(msg, "%s mentioned you in post #%llu.") == 0) return "%s が投稿 #%llu であなたを言及しました。";
        if (strcmp(msg, "%llu new posts since your last visit.") == 0) return "前回の訪問以降に新着投稿 %llu 件。";
        break;
    case SESSION_UI_LANGUAGE_ZH:
        if (strcmp(msg, "That post is no longer available.") == 0) return "该帖子已不可用。";
        if (strcmp(msg, "BBS storage is unavailable.") == 0) return "BBS 存储不可用。";
        if (strcmp(msg, "No post exists with that identifier.") == 0) return "不存在该 ID 的帖子。";
        if (strcmp(msg, "The bulletin board is empty.") == 0) return "留言板为空。";
        if (strcmp(msg, "Post body was empty. Draft discarded.") == 0) return "帖子正文为空。草稿已丢弃。";
        if (strcmp(msg, "The bulletin board is full right now.") == 0) return "留言板目前已满。";
        if (strcmp(msg, "A title is required to create a post.") == 0) return "发帖需要标题。";
        if (strcmp(msg, "Invalid post identifier.") == 0) return "帖子 ID 无效。";
        if (strcmp(msg, "This post has reached the comment limit.") == 0) return "该帖子已达到评论上限。";
        if (strcmp(msg, "Post deleted.") == 0) return "帖子已删除。";
        if (strcmp(msg, "The bulletin board is unavailable right now.") == 0) return "留言板目前不可用。";
        if (strcmp(msg, "Unable to allocate editor workspace.") == 0) return "无法分配编辑器工作区。";
        if (strcmp(msg, "Draft saved.") == 0) return "草稿已保存。";
        if (strcmp(msg, "Draft loaded.") == 0) return "草稿已加载。";
        if (strcmp(msg, "Comment added.") == 0) return "评论已添加。";
        if (strcmp(msg, "You cannot recommend your own post.") == 0) return "您不能推荐自己的帖子。";
        if (strcmp(msg, "You have already voted on this post.") == 0) return "您已经对该帖子投票。";
        if (strcmp(msg, "Recommendation added.") == 0) return "推荐已添加。";
        if (strcmp(msg, "Oppose vote added.") == 0) return "反对票已添加。";
        if (strcmp(msg, "Joined board: %s — %s") == 0) return "已进入版面: %s — %s";
        if (strcmp(msg, "Board not found.") == 0) return "未找到版面。";
        if (strcmp(msg, "Comment editing is disabled.") == 0) return "评论编辑已禁用。";
        if (strcmp(msg, "Comment deletion is disabled.") == 0) return "评论删除已禁用。";
        if (strcmp(msg, "Invalid post or comment index.") == 0) return "无效的帖子或评论索引。";
        if (strcmp(msg, "Only the author or an operator may edit this comment.") == 0) return "只有评论作者或操作员可以编辑此评论。";
        if (strcmp(msg, "Only the author or an operator may delete this comment.") == 0) return "只有评论作者或操作员可以删除此评论。";
        if (strcmp(msg, "Comment updated.") == 0) return "评论已更新。";
        if (strcmp(msg, "Comment deleted.") == 0) return "评论已删除。";
        if (strcmp(msg, "  Choose your Profile Logo (Avatar)") == 0) return "  请选择个人资料徽标（头像）";
        if (strcmp(msg, "  Enter choice (1-5):") == 0) return "  请输入选项 (1-5):";
        if (strcmp(msg, "  [1] Monitor") == 0) return "  [1] Monitor";
        if (strcmp(msg, "  [2] Mouse") == 0) return "  [2] Mouse";
        if (strcmp(msg, "  [3] Human") == 0) return "  [3] Human";
        if (strcmp(msg, "  [4] Mushroom") == 0) return "  [4] Mushroom";
        if (strcmp(msg, "  [5] None / Keep Current") == 0) return "  [5] 无 / 保持当前";
        if (strcmp(msg, " - Read a post:   Type '\033[1;32mread <id>\033[0m'") == 0) return " - 阅读帖子:   输入 '\033[1;32mread <id>\033[0m'";
        if (strcmp(msg, " - Write a post:  Type '\033[1;32mpost <title>\033[0m'") == 0) return " - 发布帖子:  输入 '\033[1;32mpost <title>\033[0m'";
        if (strcmp(msg, "(No profile picture set)") == 0) return "(未设置个人资料图片)";
        if (strcmp(msg, "----------------------------------------") == 0) return "----------------------------------------";
        if (strcmp(msg, "--------------------------------------------------") == 0) return "--------------------------------------------------";
        if (strcmp(msg, "========================================") == 0) return "========================================";
        if (strcmp(msg, "Already viewing the end of this post.") == 0) return "已经显示此帖子的末尾。";
        if (strcmp(msg, "Already viewing the top of this post.") == 0) return "已经显示此帖子的开头。";
        if (strcmp(msg, "Available avatars:") == 0) return "可用头像:";
        if (strcmp(msg, "Downvoted.") == 0) return "已投反对票。";
        if (strcmp(msg, "Draft deleted.") == 0) return "草稿已删除。";
        if (strcmp(msg, "Draft loaded. Continue editing with the post editor.") == 0) return "草稿已加载。请使用帖子编辑器继续编辑。";
        if (strcmp(msg, "Draft not found.") == 0) return "未找到草稿。";
        if (strcmp(msg, "Draft storage full. Delete old drafts first.") == 0) return "草稿存储已满。请先删除旧草稿。";
        if (strcmp(msg, "Failed to initialize user data.") == 0) return "初始化用户数据失败。";
        if (strcmp(msg, "Failed to save avatar.") == 0) return "保存头像失败。";
        if (strcmp(msg, "Failed to save profile logo.") == 0) return "保存个人资料徽标失败。";
        if (strcmp(msg, "Failed to save profile picture.") == 0) return "保存个人资料图片失败。";
        if (strcmp(msg, "Missing closing quote for the title.") == 0) return "标题缺少结束引号。";
        if (strcmp(msg, "No ASCII art pending. Use /asciiart to create one first.") == 0) return "没有待处理的 ASCII 艺术。请先使用 /asciiart 创建。";
        if (strcmp(msg, "No drafts.") == 0) return "没有草稿。";
        if (strcmp(msg, "No post exists with that identifier anymore.") == 0) return "该 ID 的帖子已不存在。";
        if (strcmp(msg, "No posts found matching the keyword.") == 0) return "未找到与该关键词匹配的帖子。";
        if (strcmp(msg, "Nothing to save. Start a post first.") == 0) return "没有可保存的内容。请先开始撰写帖子。";
        if (strcmp(msg, "Only the author or an operator may delete this post.") == 0) return "只有作者或管理员才能删除此帖子。";
        if (strcmp(msg, "Only the author or an operator may edit this post.") == 0) return "只有作者或管理员才能编辑此帖子。";
        if (strcmp(msg, "Post bumped to the top.") == 0) return "帖子已置顶。";
        if (strcmp(msg, "Post created.") == 0) return "帖子已创建。";
        if (strcmp(msg, "Post updated.") == 0) return "帖子已更新。";
        if (strcmp(msg, "Profile picture updated.") == 0) return "个人资料图片已更新。";
        if (strcmp(msg, "Recent posts:") == 0) return "最近的帖子:";
        if (strcmp(msg, "Specify a topic to read.") == 0) return "请指定要阅读的主题。";
        if (strcmp(msg, "Unknown avatar name.") == 0) return "未知的头像名称。";
        if (strcmp(msg, "Upvoted.") == 0) return "已投赞成票。";
        if (strcmp(msg, "Usage: /bbs cmtvote <post_id> <comment_idx> [up|down]") == 0) return "用法: /bbs cmtvote <post_id> <comment_idx> [up|down]";
        if (strcmp(msg, "Usage: /bbs draft <save|list|load|delete>") == 0) return "用法: /bbs draft <save|list|load|delete>";
        if (strcmp(msg, "Usage: /bbs draft delete <index>") == 0) return "用法: /bbs draft delete <index>";
        if (strcmp(msg, "Usage: /bbs draft load <index>") == 0) return "用法: /bbs draft load <index>";
        if (strcmp(msg, "Usage: /bbs setavatar <monitor|mouse|human|mushroom|none>") == 0) return "用法: /bbs setavatar <monitor|mouse|human|mushroom|none>";
        if (strcmp(msg, "Usage: search <keyword>") == 0) return "用法: search <keyword>";
        if (strcmp(msg, "Use | to separate tags when the title has spaces.") == 0) return "标题包含空格时，请使用 | 分隔标签。";
        if (strcmp(msg, "You already downvoted this post.") == 0) return "您已经对此帖子投过反对票。";
        if (strcmp(msg, "You already upvoted this post.") == 0) return "您已经对此帖子投过赞成票。";
        if (strcmp(msg, "You already voted this comment.") == 0) return "您已经对该评论投过票。";
        if (strcmp(msg, "[Tip] Share your thoughts on the board!") == 0) return "[提示] 在公告板上分享您的想法!";
        if (strcmp(msg, "\033[1;32m>>> Baud Rate: 14400 bps | Terminal: ANSI-BBS | STATUS: ONLINE <<<\033[0m\n") == 0) return "\033[1;32m>>> 波特率: 14400 bps | 终端: ANSI-BBS | 状态: 在线 <<<\033[0m\n";
        if (strcmp(msg, "\n\033[1;36m>>> Connecting to Retro BBS Network... <<<\033[0m") == 0) return "\n\033[1;36m>>> 正在连接复古 BBS 网络... <<<\033[0m";
        if (strcmp(msg, "No posts on that page.") == 0) return "该页面没有帖子。";
        if (strcmp(msg, "Page %zu/%zu (next: list %s %zu)") == 0) return "第 %zu/%zu 页 (下一页: list %s %zu)";
        if (strcmp(msg, "Page %zu/%zu") == 0) return "第 %zu/%zu 页";
        if (strcmp(msg, "You are commenting too quickly. Try again in a moment.") == 0) return "您发送评论过于频繁，请稍后再试。";
        if (strcmp(msg, "You are posting too quickly. Try again in a moment.") == 0) return "您发布帖子过于频繁，请稍后再试。";
        if (strcmp(msg, "--- Notifications ---") == 0) return "--- 通知 ---";
        if (strcmp(msg, "%s replied to your post #%llu.") == 0) return "%s 回复了您的帖子 #%llu。";
        if (strcmp(msg, "%s mentioned you in post #%llu.") == 0) return "%s 在帖子 #%llu 中提到了您。";
        if (strcmp(msg, "%llu new posts since your last visit.") == 0) return "自您上次访问以来有 %llu 条新帖子。";
        break;
    case SESSION_UI_LANGUAGE_RU:
        if (strcmp(msg, "That post is no longer available.") == 0) return "Этот пост больше недоступен.";
        if (strcmp(msg, "BBS storage is unavailable.") == 0) return "Хранилище BBS недоступно.";
        if (strcmp(msg, "No post exists with that identifier.") == 0) return "Поста с таким идентификатором не существует.";
        if (strcmp(msg, "The bulletin board is empty.") == 0) return "Доска объявлений пуста.";
        if (strcmp(msg, "Post body was empty. Draft discarded.") == 0) return "Тело поста было пустым. Черновик отброшен.";
        if (strcmp(msg, "The bulletin board is full right now.") == 0) return "Доска объявлений сейчас заполнена.";
        if (strcmp(msg, "A title is required to create a post.") == 0) return "Для создания поста требуется заголовок.";
        if (strcmp(msg, "Invalid post identifier.") == 0) return "Недопустимый идентификатор поста.";
        if (strcmp(msg, "This post has reached the comment limit.") == 0) return "Этот пост достиг лимита комментариев.";
        if (strcmp(msg, "Post deleted.") == 0) return "Пост удален.";
        if (strcmp(msg, "The bulletin board is unavailable right now.") == 0) return "Доска объявлений сейчас недоступна.";
        if (strcmp(msg, "Unable to allocate editor workspace.") == 0) return "Не удалось выделить рабочую область редактора.";
        if (strcmp(msg, "Draft saved.") == 0) return "Черновик сохранен.";
        if (strcmp(msg, "Draft loaded.") == 0) return "Черновик загружен.";
        if (strcmp(msg, "Comment added.") == 0) return "Комментарий добавлен.";
        if (strcmp(msg, "You cannot recommend your own post.") == 0) return "Вы не можете рекомендовать свой собственный пост.";
        if (strcmp(msg, "You have already voted on this post.") == 0) return "Вы уже проголосовали за этот пост.";
        if (strcmp(msg, "Recommendation added.") == 0) return "Рекомендация добавлена.";
        if (strcmp(msg, "Oppose vote added.") == 0) return "Голос против добавлен.";
        if (strcmp(msg, "Joined board: %s — %s") == 0) return "Вы вошли на доску: %s — %s";
        if (strcmp(msg, "Board not found.") == 0) return "Доска не найдена.";
        if (strcmp(msg, "Comment editing is disabled.") == 0) return "Редактирование комментариев отключено.";
        if (strcmp(msg, "Comment deletion is disabled.") == 0) return "Удаление комментариев отключено.";
        if (strcmp(msg, "Invalid post or comment index.") == 0) return "Недопустимый индекс поста или комментария.";
        if (strcmp(msg, "Only the author or an operator may edit this comment.") == 0) return "Только автор или оператор могут редактировать этот комментарий.";
        if (strcmp(msg, "Only the author or an operator may delete this comment.") == 0) return "Только автор или оператор могут удалить этот комментарий.";
        if (strcmp(msg, "Comment updated.") == 0) return "Комментарий обновлен.";
        if (strcmp(msg, "Comment deleted.") == 0) return "Комментарий удален.";
        if (strcmp(msg, "  Choose your Profile Logo (Avatar)") == 0) return "  Выберите логотип профиля (аватар)";
        if (strcmp(msg, "  Enter choice (1-5):") == 0) return "  Введите выбор (1-5):";
        if (strcmp(msg, "  [1] Monitor") == 0) return "  [1] Monitor";
        if (strcmp(msg, "  [2] Mouse") == 0) return "  [2] Mouse";
        if (strcmp(msg, "  [3] Human") == 0) return "  [3] Human";
        if (strcmp(msg, "  [4] Mushroom") == 0) return "  [4] Mushroom";
        if (strcmp(msg, "  [5] None / Keep Current") == 0) return "  [5] Нет / Оставить текущий";
        if (strcmp(msg, " - Read a post:   Type '\033[1;32mread <id>\033[0m'") == 0) return " - Читать пост:   введите '\033[1;32mread <id>\033[0m'";
        if (strcmp(msg, " - Write a post:  Type '\033[1;32mpost <title>\033[0m'") == 0) return " - Написать пост:  введите '\033[1;32mpost <title>\033[0m'";
        if (strcmp(msg, "(No profile picture set)") == 0) return "(Изображение профиля не установлено)";
        if (strcmp(msg, "----------------------------------------") == 0) return "----------------------------------------";
        if (strcmp(msg, "--------------------------------------------------") == 0) return "--------------------------------------------------";
        if (strcmp(msg, "========================================") == 0) return "========================================";
        if (strcmp(msg, "Already viewing the end of this post.") == 0) return "Вы уже просматриваете конец этого поста.";
        if (strcmp(msg, "Already viewing the top of this post.") == 0) return "Вы уже просматриваете начало этого поста.";
        if (strcmp(msg, "Available avatars:") == 0) return "Доступные аватары:";
        if (strcmp(msg, "Downvoted.") == 0) return "Голос против учтён.";
        if (strcmp(msg, "Draft deleted.") == 0) return "Черновик удалён.";
        if (strcmp(msg, "Draft loaded. Continue editing with the post editor.") == 0) return "Черновик загружен. Продолжайте редактирование в редакторе постов.";
        if (strcmp(msg, "Draft not found.") == 0) return "Черновик не найден.";
        if (strcmp(msg, "Draft storage full. Delete old drafts first.") == 0) return "Хранилище черновиков заполнено. Сначала удалите старые черновики.";
        if (strcmp(msg, "Failed to initialize user data.") == 0) return "Не удалось инициализировать данные пользователя.";
        if (strcmp(msg, "Failed to save avatar.") == 0) return "Не удалось сохранить аватар.";
        if (strcmp(msg, "Failed to save profile logo.") == 0) return "Не удалось сохранить логотип профиля.";
        if (strcmp(msg, "Failed to save profile picture.") == 0) return "Не удалось сохранить изображение профиля.";
        if (strcmp(msg, "Missing closing quote for the title.") == 0) return "Отсутствует закрывающая кавычка заголовка.";
        if (strcmp(msg, "No ASCII art pending. Use /asciiart to create one first.") == 0) return "Нет ожидающего ASCII-арта. Сначала создайте его через /asciiart.";
        if (strcmp(msg, "No drafts.") == 0) return "Нет черновиков.";
        if (strcmp(msg, "No post exists with that identifier anymore.") == 0) return "Поста с таким идентификатором больше не существует.";
        if (strcmp(msg, "No posts found matching the keyword.") == 0) return "Постов, соответствующих ключевому слову, не найдено.";
        if (strcmp(msg, "Nothing to save. Start a post first.") == 0) return "Нечего сохранять. Сначала начните пост.";
        if (strcmp(msg, "Only the author or an operator may delete this post.") == 0) return "Только автор или оператор могут удалить этот пост.";
        if (strcmp(msg, "Only the author or an operator may edit this post.") == 0) return "Только автор или оператор могут редактировать этот пост.";
        if (strcmp(msg, "Post bumped to the top.") == 0) return "Пост поднят наверх.";
        if (strcmp(msg, "Post created.") == 0) return "Пост создан.";
        if (strcmp(msg, "Post updated.") == 0) return "Пост обновлён.";
        if (strcmp(msg, "Profile picture updated.") == 0) return "Изображение профиля обновлено.";
        if (strcmp(msg, "Recent posts:") == 0) return "Последние посты:";
        if (strcmp(msg, "Specify a topic to read.") == 0) return "Укажите тему для чтения.";
        if (strcmp(msg, "Unknown avatar name.") == 0) return "Неизвестное имя аватара.";
        if (strcmp(msg, "Upvoted.") == 0) return "Голос за учтён.";
        if (strcmp(msg, "Usage: /bbs cmtvote <post_id> <comment_idx> [up|down]") == 0) return "Использование: /bbs cmtvote <post_id> <comment_idx> [up|down]";
        if (strcmp(msg, "Usage: /bbs draft <save|list|load|delete>") == 0) return "Использование: /bbs draft <save|list|load|delete>";
        if (strcmp(msg, "Usage: /bbs draft delete <index>") == 0) return "Использование: /bbs draft delete <index>";
        if (strcmp(msg, "Usage: /bbs draft load <index>") == 0) return "Использование: /bbs draft load <index>";
        if (strcmp(msg, "Usage: /bbs setavatar <monitor|mouse|human|mushroom|none>") == 0) return "Использование: /bbs setavatar <monitor|mouse|human|mushroom|none>";
        if (strcmp(msg, "Usage: search <keyword>") == 0) return "Использование: search <keyword>";
        if (strcmp(msg, "Use | to separate tags when the title has spaces.") == 0) return "Используйте | для разделения тегов, если в заголовке есть пробелы.";
        if (strcmp(msg, "You already downvoted this post.") == 0) return "Вы уже голосовали против этого поста.";
        if (strcmp(msg, "You already upvoted this post.") == 0) return "Вы уже голосовали за этот пост.";
        if (strcmp(msg, "You already voted this comment.") == 0) return "Вы уже голосовали за этот комментарий.";
        if (strcmp(msg, "[Tip] Share your thoughts on the board!") == 0) return "[Совет] Поделитесь мнением на доске!";
        if (strcmp(msg, "\033[1;32m>>> Baud Rate: 14400 bps | Terminal: ANSI-BBS | STATUS: ONLINE <<<\033[0m\n") == 0) return "\033[1;32m>>> Скорость: 14400 бод | Терминал: ANSI-BBS | СТАТУС: ОНЛАЙН <<<\033[0m\n";
        if (strcmp(msg, "\n\033[1;36m>>> Connecting to Retro BBS Network... <<<\033[0m") == 0) return "\n\033[1;36m>>> Подключение к сети Retro BBS... <<<\033[0m";
        if (strcmp(msg, "No posts on that page.") == 0) return "На этой странице нет постов.";
        if (strcmp(msg, "Page %zu/%zu (next: list %s %zu)") == 0) return "Страница %zu/%zu (далее: list %s %zu)";
        if (strcmp(msg, "Page %zu/%zu") == 0) return "Страница %zu/%zu";
        if (strcmp(msg, "You are commenting too quickly. Try again in a moment.") == 0) return "Вы комментируете слишком часто. Попробуйте снова через несколько секунд.";
        if (strcmp(msg, "You are posting too quickly. Try again in a moment.") == 0) return "Вы публикуете слишком часто. Попробуйте снова через несколько секунд.";
        if (strcmp(msg, "--- Notifications ---") == 0) return "--- Уведомления ---";
        if (strcmp(msg, "%s replied to your post #%llu.") == 0) return "%s ответил на ваш пост #%llu.";
        if (strcmp(msg, "%s mentioned you in post #%llu.") == 0) return "%s упомянул вас в посте #%llu.";
        if (strcmp(msg, "%llu new posts since your last visit.") == 0) return "С момента вашего последнего визита новых постов: %llu.";
        break;
    case SESSION_UI_LANGUAGE_DE:
        if (strcmp(msg, "That post is no longer available.") == 0) return "Dieser Beitrag ist nicht mehr verfügbar.";
        if (strcmp(msg, "BBS storage is unavailable.") == 0) return "BBS-Speicher ist nicht verfügbar.";
        if (strcmp(msg, "No post exists with that identifier.") == 0) return "Es existiert kein Beitrag mit dieser Kennung.";
        if (strcmp(msg, "The bulletin board is empty.") == 0) return "Das Schwarze Brett ist leer.";
        if (strcmp(msg, "Post body was empty. Draft discarded.") == 0) return "Beitragstext war leer. Entwurf verworfen.";
        if (strcmp(msg, "The bulletin board is full right now.") == 0) return "Das Schwarze Brett ist derzeit voll.";
        if (strcmp(msg, "A title is required to create a post.") == 0) return "Ein Titel ist erforderlich, um einen Beitrag zu erstellen.";
        if (strcmp(msg, "Invalid post identifier.") == 0) return "Ungültige Beitragskennung.";
        if (strcmp(msg, "This post has reached the comment limit.") == 0) return "Dieser Beitrag hat das Kommentarlimit erreicht.";
        if (strcmp(msg, "Post deleted.") == 0) return "Beitrag gelöscht.";
        if (strcmp(msg, "The bulletin board is unavailable right now.") == 0) return "Das Schwarze Brett ist derzeit nicht verfügbar.";
        if (strcmp(msg, "Unable to allocate editor workspace.") == 0) return "Editor-Arbeitsbereich konnte nicht zugewiesen werden.";
        if (strcmp(msg, "Draft saved.") == 0) return "Entwurf gespeichert.";
        if (strcmp(msg, "Draft loaded.") == 0) return "Entwurf geladen.";
        if (strcmp(msg, "Comment added.") == 0) return "Kommentar hinzugefügt.";
        if (strcmp(msg, "You cannot recommend your own post.") == 0) return "Sie können Ihren eigenen Beitrag nicht empfehlen.";
        if (strcmp(msg, "You have already voted on this post.") == 0) return "Sie haben bereits über diesen Beitrag abgestimmt.";
        if (strcmp(msg, "Recommendation added.") == 0) return "Empfehlung hinzugefügt.";
        if (strcmp(msg, "Oppose vote added.") == 0) return "Gegenstimme hinzugefügt.";
        if (strcmp(msg, "Joined board: %s — %s") == 0) return "Board beigetreten: %s — %s";
        if (strcmp(msg, "Board not found.") == 0) return "Board nicht gefunden.";
        if (strcmp(msg, "Comment editing is disabled.") == 0) return "Das Bearbeiten von Kommentaren ist deaktiviert.";
        if (strcmp(msg, "Comment deletion is disabled.") == 0) return "Das Löschen von Kommentaren ist deaktiviert.";
        if (strcmp(msg, "Invalid post or comment index.") == 0) return "Ungültiger Beitrags- oder Kommentarindex.";
        if (strcmp(msg, "Only the author or an operator may edit this comment.") == 0) return "Nur der Autor oder ein Operator kann diesen Kommentar bearbeiten.";
        if (strcmp(msg, "Only the author or an operator may delete this comment.") == 0) return "Nur der Autor oder ein Operator kann diesen Kommentar löschen.";
        if (strcmp(msg, "Comment updated.") == 0) return "Kommentar aktualisiert.";
        if (strcmp(msg, "Comment deleted.") == 0) return "Kommentar gelöscht.";
        if (strcmp(msg, "  Choose your Profile Logo (Avatar)") == 0) return "  Wählen Sie Ihr Profil-Logo (Avatar)";
        if (strcmp(msg, "  Enter choice (1-5):") == 0) return "  Auswahl eingeben (1-5):";
        if (strcmp(msg, "  [1] Monitor") == 0) return "  [1] Monitor";
        if (strcmp(msg, "  [2] Mouse") == 0) return "  [2] Mouse";
        if (strcmp(msg, "  [3] Human") == 0) return "  [3] Human";
        if (strcmp(msg, "  [4] Mushroom") == 0) return "  [4] Mushroom";
        if (strcmp(msg, "  [5] None / Keep Current") == 0) return "  [5] Keins / Aktuelles behalten";
        if (strcmp(msg, " - Read a post:   Type '\033[1;32mread <id>\033[0m'") == 0) return " - Beitrag lesen:   '\033[1;32mread <id>\033[0m' eingeben";
        if (strcmp(msg, " - Write a post:  Type '\033[1;32mpost <title>\033[0m'") == 0) return " - Beitrag schreiben:  '\033[1;32mpost <title>\033[0m' eingeben";
        if (strcmp(msg, "(No profile picture set)") == 0) return "(Kein Profilbild gesetzt)";
        if (strcmp(msg, "----------------------------------------") == 0) return "----------------------------------------";
        if (strcmp(msg, "--------------------------------------------------") == 0) return "--------------------------------------------------";
        if (strcmp(msg, "========================================") == 0) return "========================================";
        if (strcmp(msg, "Already viewing the end of this post.") == 0) return "Sie sehen bereits das Ende dieses Beitrags.";
        if (strcmp(msg, "Already viewing the top of this post.") == 0) return "Sie sehen bereits den Anfang dieses Beitrags.";
        if (strcmp(msg, "Available avatars:") == 0) return "Verfügbare Avatare:";
        if (strcmp(msg, "Downvoted.") == 0) return "Dagegen gestimmt.";
        if (strcmp(msg, "Draft deleted.") == 0) return "Entwurf gelöscht.";
        if (strcmp(msg, "Draft loaded. Continue editing with the post editor.") == 0) return "Entwurf geladen. Bearbeiten Sie ihn weiter im Beitragseditor.";
        if (strcmp(msg, "Draft not found.") == 0) return "Entwurf nicht gefunden.";
        if (strcmp(msg, "Draft storage full. Delete old drafts first.") == 0) return "Entwurfsspeicher voll. Löschen Sie zuerst alte Entwürfe.";
        if (strcmp(msg, "Failed to initialize user data.") == 0) return "Benutzerdaten konnten nicht initialisiert werden.";
        if (strcmp(msg, "Failed to save avatar.") == 0) return "Avatar konnte nicht gespeichert werden.";
        if (strcmp(msg, "Failed to save profile logo.") == 0) return "Profil-Logo konnte nicht gespeichert werden.";
        if (strcmp(msg, "Failed to save profile picture.") == 0) return "Profilbild konnte nicht gespeichert werden.";
        if (strcmp(msg, "Missing closing quote for the title.") == 0) return "Schließendes Anführungszeichen für den Titel fehlt.";
        if (strcmp(msg, "No ASCII art pending. Use /asciiart to create one first.") == 0) return "Kein ASCII-Art ausstehend. Erstellen Sie zuerst eines mit /asciiart.";
        if (strcmp(msg, "No drafts.") == 0) return "Keine Entwürfe.";
        if (strcmp(msg, "No post exists with that identifier anymore.") == 0) return "Ein Beitrag mit dieser Kennung existiert nicht mehr.";
        if (strcmp(msg, "No posts found matching the keyword.") == 0) return "Keine Beiträge gefunden, die dem Schlüsselwort entsprechen.";
        if (strcmp(msg, "Nothing to save. Start a post first.") == 0) return "Nichts zu speichern. Beginnen Sie zuerst einen Beitrag.";
        if (strcmp(msg, "Only the author or an operator may delete this post.") == 0) return "Nur der Autor oder ein Operator kann diesen Beitrag löschen.";
        if (strcmp(msg, "Only the author or an operator may edit this post.") == 0) return "Nur der Autor oder ein Operator kann diesen Beitrag bearbeiten.";
        if (strcmp(msg, "Post bumped to the top.") == 0) return "Beitrag nach oben verschoben.";
        if (strcmp(msg, "Post created.") == 0) return "Beitrag erstellt.";
        if (strcmp(msg, "Post updated.") == 0) return "Beitrag aktualisiert.";
        if (strcmp(msg, "Profile picture updated.") == 0) return "Profilbild aktualisiert.";
        if (strcmp(msg, "Recent posts:") == 0) return "Letzte Beiträge:";
        if (strcmp(msg, "Specify a topic to read.") == 0) return "Geben Sie ein Thema zum Lesen an.";
        if (strcmp(msg, "Unknown avatar name.") == 0) return "Unbekannter Avatar-Name.";
        if (strcmp(msg, "Upvoted.") == 0) return "Dafür gestimmt.";
        if (strcmp(msg, "Usage: /bbs cmtvote <post_id> <comment_idx> [up|down]") == 0) return "Verwendung: /bbs cmtvote <post_id> <comment_idx> [up|down]";
        if (strcmp(msg, "Usage: /bbs draft <save|list|load|delete>") == 0) return "Verwendung: /bbs draft <save|list|load|delete>";
        if (strcmp(msg, "Usage: /bbs draft delete <index>") == 0) return "Verwendung: /bbs draft delete <index>";
        if (strcmp(msg, "Usage: /bbs draft load <index>") == 0) return "Verwendung: /bbs draft load <index>";
        if (strcmp(msg, "Usage: /bbs setavatar <monitor|mouse|human|mushroom|none>") == 0) return "Verwendung: /bbs setavatar <monitor|mouse|human|mushroom|none>";
        if (strcmp(msg, "Usage: search <keyword>") == 0) return "Verwendung: search <keyword>";
        if (strcmp(msg, "Use | to separate tags when the title has spaces.") == 0) return "Verwenden Sie |, um Tags zu trennen, wenn der Titel Leerzeichen enthält.";
        if (strcmp(msg, "You already downvoted this post.") == 0) return "Sie haben bereits gegen diesen Beitrag gestimmt.";
        if (strcmp(msg, "You already upvoted this post.") == 0) return "Sie haben bereits für diesen Beitrag gestimmt.";
        if (strcmp(msg, "You already voted this comment.") == 0) return "Sie haben bereits für diesen Kommentar gestimmt.";
        if (strcmp(msg, "[Tip] Share your thoughts on the board!") == 0) return "[Tipp] Teilen Sie Ihre Meinung auf dem Board mit!";
        if (strcmp(msg, "\033[1;32m>>> Baud Rate: 14400 bps | Terminal: ANSI-BBS | STATUS: ONLINE <<<\033[0m\n") == 0) return "\033[1;32m>>> Baudrate: 14400 bps | Terminal: ANSI-BBS | STATUS: ONLINE <<<\033[0m\n";
        if (strcmp(msg, "\n\033[1;36m>>> Connecting to Retro BBS Network... <<<\033[0m") == 0) return "\n\033[1;36m>>> Verbindung mit Retro-BBS-Netzwerk... <<<\033[0m";
        if (strcmp(msg, "No posts on that page.") == 0) return "Auf dieser Seite gibt es keine Beiträge.";
        if (strcmp(msg, "Page %zu/%zu (next: list %s %zu)") == 0) return "Seite %zu/%zu (weiter: list %s %zu)";
        if (strcmp(msg, "Page %zu/%zu") == 0) return "Seite %zu/%zu";
        if (strcmp(msg, "You are commenting too quickly. Try again in a moment.") == 0) return "Sie kommentieren zu schnell. Versuchen Sie es gleich erneut.";
        if (strcmp(msg, "You are posting too quickly. Try again in a moment.") == 0) return "Sie posten zu schnell. Versuchen Sie es gleich erneut.";
        if (strcmp(msg, "--- Notifications ---") == 0) return "--- Benachrichtigungen ---";
        if (strcmp(msg, "%s replied to your post #%llu.") == 0) return "%s hat auf Ihren Beitrag #%llu geantwortet.";
        if (strcmp(msg, "%s mentioned you in post #%llu.") == 0) return "%s hat Sie in Beitrag #%llu erwähnt.";
        if (strcmp(msg, "%llu new posts since your last visit.") == 0) return "%llu neue Beiträge seit Ihrem letzten Besuch.";
        break;
    case SESSION_UI_LANGUAGE_FR:
        if (strcmp(msg, "That post is no longer available.") == 0) return "Ce message n'est plus disponible.";
        if (strcmp(msg, "BBS storage is unavailable.") == 0) return "Le stockage BBS est indisponible.";
        if (strcmp(msg, "No post exists with that identifier.") == 0) return "Aucun message n'existe avec cet identifiant.";
        if (strcmp(msg, "The bulletin board is empty.") == 0) return "Le panneau d'affichage est vide.";
        if (strcmp(msg, "Post body was empty. Draft discarded.") == 0) return "Le corps du message était vide. Brouillon jeté.";
        if (strcmp(msg, "The bulletin board is full right now.") == 0) return "Le panneau d'affichage est complet pour le moment.";
        if (strcmp(msg, "A title is required to create a post.") == 0) return "Un titre est requis pour créer un message.";
        if (strcmp(msg, "Invalid post identifier.") == 0) return "Identifiant de message invalide.";
        if (strcmp(msg, "This post has reached the comment limit.") == 0) return "Ce message a atteint la limite de commentaires.";
        if (strcmp(msg, "Post deleted.") == 0) return "Message supprimé.";
        if (strcmp(msg, "The bulletin board is unavailable right now.") == 0) return "Le panneau d'affichage est indisponible pour le moment.";
        if (strcmp(msg, "Unable to allocate editor workspace.") == 0) return "Impossible d'allouer l'espace de travail de l'éditeur.";
        if (strcmp(msg, "Draft saved.") == 0) return "Brouillon enregistré.";
        if (strcmp(msg, "Draft loaded.") == 0) return "Brouillon chargé.";
        if (strcmp(msg, "Comment added.") == 0) return "Commentaire ajouté.";
        if (strcmp(msg, "You cannot recommend your own post.") == 0) return "Vous ne pouvez pas recommander votre propre message.";
        if (strcmp(msg, "You have already voted on this post.") == 0) return "Vous avez déjà voté sur ce message.";
        if (strcmp(msg, "Recommendation added.") == 0) return "Recommandation ajoutée.";
        if (strcmp(msg, "Oppose vote added.") == 0) return "Vote contre ajouté.";
        if (strcmp(msg, "Joined board: %s — %s") == 0) return "Sujet rejoint: %s — %s";
        if (strcmp(msg, "Board not found.") == 0) return "Sujet non trouvé.";
        if (strcmp(msg, "Comment editing is disabled.") == 0) return "La modification des commentaires est désactivée.";
        if (strcmp(msg, "Comment deletion is disabled.") == 0) return "La suppression des commentaires est désactivée.";
        if (strcmp(msg, "Invalid post or comment index.") == 0) return "Index de message ou de commentaire invalide.";
        if (strcmp(msg, "Only the author or an operator may edit this comment.") == 0) return "Seul l'auteur ou un opérateur peut modifier ce commentaire.";
        if (strcmp(msg, "Only the author or an operator may delete this comment.") == 0) return "Seul l'auteur ou un opérateur peut supprimer ce commentaire.";
        if (strcmp(msg, "Comment updated.") == 0) return "Commentaire mis à jour.";
        if (strcmp(msg, "Comment deleted.") == 0) return "Commentaire supprimé.";
        if (strcmp(msg, "  Choose your Profile Logo (Avatar)") == 0) return "  Choisissez votre logo de profil (avatar)";
        if (strcmp(msg, "  Enter choice (1-5):") == 0) return "  Entrez votre choix (1-5) :";
        if (strcmp(msg, "  [1] Monitor") == 0) return "  [1] Monitor";
        if (strcmp(msg, "  [2] Mouse") == 0) return "  [2] Mouse";
        if (strcmp(msg, "  [3] Human") == 0) return "  [3] Human";
        if (strcmp(msg, "  [4] Mushroom") == 0) return "  [4] Mushroom";
        if (strcmp(msg, "  [5] None / Keep Current") == 0) return "  [5] Aucun / Garder l'actuel";
        if (strcmp(msg, " - Read a post:   Type '\033[1;32mread <id>\033[0m'") == 0) return " - Lire un message :   tapez '\033[1;32mread <id>\033[0m'";
        if (strcmp(msg, " - Write a post:  Type '\033[1;32mpost <title>\033[0m'") == 0) return " - Écrire un message :  tapez '\033[1;32mpost <title>\033[0m'";
        if (strcmp(msg, "(No profile picture set)") == 0) return "(Aucune image de profil définie)";
        if (strcmp(msg, "----------------------------------------") == 0) return "----------------------------------------";
        if (strcmp(msg, "--------------------------------------------------") == 0) return "--------------------------------------------------";
        if (strcmp(msg, "========================================") == 0) return "========================================";
        if (strcmp(msg, "Already viewing the end of this post.") == 0) return "Vous voyez déjà la fin de ce message.";
        if (strcmp(msg, "Already viewing the top of this post.") == 0) return "Vous voyez déjà le début de ce message.";
        if (strcmp(msg, "Available avatars:") == 0) return "Avatars disponibles :";
        if (strcmp(msg, "Downvoted.") == 0) return "Vote contre enregistré.";
        if (strcmp(msg, "Draft deleted.") == 0) return "Brouillon supprimé.";
        if (strcmp(msg, "Draft loaded. Continue editing with the post editor.") == 0) return "Brouillon chargé. Continuez la modification avec l'éditeur de message.";
        if (strcmp(msg, "Draft not found.") == 0) return "Brouillon non trouvé.";
        if (strcmp(msg, "Draft storage full. Delete old drafts first.") == 0) return "Stockage de brouillons plein. Supprimez d'abord les anciens brouillons.";
        if (strcmp(msg, "Failed to initialize user data.") == 0) return "Échec de l'initialisation des données utilisateur.";
        if (strcmp(msg, "Failed to save avatar.") == 0) return "Échec de l'enregistrement de l'avatar.";
        if (strcmp(msg, "Failed to save profile logo.") == 0) return "Échec de l'enregistrement du logo de profil.";
        if (strcmp(msg, "Failed to save profile picture.") == 0) return "Échec de l'enregistrement de l'image de profil.";
        if (strcmp(msg, "Missing closing quote for the title.") == 0) return "Guillemet fermant du titre manquant.";
        if (strcmp(msg, "No ASCII art pending. Use /asciiart to create one first.") == 0) return "Aucune ASCII art en attente. Créez-en une d'abord avec /asciiart.";
        if (strcmp(msg, "No drafts.") == 0) return "Aucun brouillon.";
        if (strcmp(msg, "No post exists with that identifier anymore.") == 0) return "Aucun message n'existe plus avec cet identifiant.";
        if (strcmp(msg, "No posts found matching the keyword.") == 0) return "Aucun message ne correspond au mot-clé.";
        if (strcmp(msg, "Nothing to save. Start a post first.") == 0) return "Rien à enregistrer. Commencez d'abord un message.";
        if (strcmp(msg, "Only the author or an operator may delete this post.") == 0) return "Seul l'auteur ou un opérateur peut supprimer ce message.";
        if (strcmp(msg, "Only the author or an operator may edit this post.") == 0) return "Seul l'auteur ou un opérateur peut modifier ce message.";
        if (strcmp(msg, "Post bumped to the top.") == 0) return "Message remonté en haut.";
        if (strcmp(msg, "Post created.") == 0) return "Message créé.";
        if (strcmp(msg, "Post updated.") == 0) return "Message mis à jour.";
        if (strcmp(msg, "Profile picture updated.") == 0) return "Image de profil mise à jour.";
        if (strcmp(msg, "Recent posts:") == 0) return "Messages récents :";
        if (strcmp(msg, "Specify a topic to read.") == 0) return "Précisez un sujet à lire.";
        if (strcmp(msg, "Unknown avatar name.") == 0) return "Nom d'avatar inconnu.";
        if (strcmp(msg, "Upvoted.") == 0) return "Vote pour enregistré.";
        if (strcmp(msg, "Usage: /bbs cmtvote <post_id> <comment_idx> [up|down]") == 0) return "Utilisation : /bbs cmtvote <post_id> <comment_idx> [up|down]";
        if (strcmp(msg, "Usage: /bbs draft <save|list|load|delete>") == 0) return "Utilisation : /bbs draft <save|list|load|delete>";
        if (strcmp(msg, "Usage: /bbs draft delete <index>") == 0) return "Utilisation : /bbs draft delete <index>";
        if (strcmp(msg, "Usage: /bbs draft load <index>") == 0) return "Utilisation : /bbs draft load <index>";
        if (strcmp(msg, "Usage: /bbs setavatar <monitor|mouse|human|mushroom|none>") == 0) return "Utilisation : /bbs setavatar <monitor|mouse|human|mushroom|none>";
        if (strcmp(msg, "Usage: search <keyword>") == 0) return "Utilisation : search <keyword>";
        if (strcmp(msg, "Use | to separate tags when the title has spaces.") == 0) return "Utilisez | pour séparer les tags si le titre contient des espaces.";
        if (strcmp(msg, "You already downvoted this post.") == 0) return "Vous avez déjà voté contre ce message.";
        if (strcmp(msg, "You already upvoted this post.") == 0) return "Vous avez déjà voté pour ce message.";
        if (strcmp(msg, "You already voted this comment.") == 0) return "Vous avez déjà voté sur ce commentaire.";
        if (strcmp(msg, "[Tip] Share your thoughts on the board!") == 0) return "[Astuce] Partagez votre avis sur le panneau !";
        if (strcmp(msg, "\033[1;32m>>> Baud Rate: 14400 bps | Terminal: ANSI-BBS | STATUS: ONLINE <<<\033[0m\n") == 0) return "\033[1;32m>>> Débit : 14400 bps | Terminal : ANSI-BBS | STATUT : EN LIGNE <<<\033[0m\n";
        if (strcmp(msg, "\n\033[1;36m>>> Connecting to Retro BBS Network... <<<\033[0m") == 0) return "\n\033[1;36m>>> Connexion au réseau BBS rétro... <<<\033[0m";
        if (strcmp(msg, "No posts on that page.") == 0) return "Aucun message sur cette page.";
        if (strcmp(msg, "Page %zu/%zu (next: list %s %zu)") == 0) return "Page %zu/%zu (suivant : list %s %zu)";
        if (strcmp(msg, "Page %zu/%zu") == 0) return "Page %zu/%zu";
        if (strcmp(msg, "You are commenting too quickly. Try again in a moment.") == 0) return "Vous commentez trop rapidement. Réessayez dans un instant.";
        if (strcmp(msg, "You are posting too quickly. Try again in a moment.") == 0) return "Vous publiez trop rapidement. Réessayez dans un instant.";
        if (strcmp(msg, "--- Notifications ---") == 0) return "--- Notifications ---";
        if (strcmp(msg, "%s replied to your post #%llu.") == 0) return "%s a répondu à votre message #%llu.";
        if (strcmp(msg, "%s mentioned you in post #%llu.") == 0) return "%s vous a mentionné dans le message #%llu.";
        if (strcmp(msg, "%llu new posts since your last visit.") == 0) return "%llu nouveaux messages depuis votre dernière visite.";
        break;
    case SESSION_UI_LANGUAGE_PL:
        if (strcmp(msg, "That post is no longer available.") == 0) return "Ten post nie jest już dostępny.";
        if (strcmp(msg, "BBS storage is unavailable.") == 0) return "Pamięć BBS jest niedostępna.";
        if (strcmp(msg, "No post exists with that identifier.") == 0) return "Nie istnieje post o tym identyfikatorze.";
        if (strcmp(msg, "The bulletin board is empty.") == 0) return "Tablica ogłoszeń jest pusta.";
        if (strcmp(msg, "Post body was empty. Draft discarded.") == 0) return "Treść posta była pusta. Szkic odrzucony.";
        if (strcmp(msg, "The bulletin board is full right now.") == 0) return "Tablica ogłoszeń jest obecnie pełna.";
        if (strcmp(msg, "A title is required to create a post.") == 0) return "Tytuł jest wymagany do utworzenia posta.";
        if (strcmp(msg, "Invalid post identifier.") == 0) return "Nieprawidłowy identyfikator posta.";
        if (strcmp(msg, "This post has reached the comment limit.") == 0) return "Ten post osiągnął limit komentarzy.";
        if (strcmp(msg, "Post deleted.") == 0) return "Post usunięty.";
        if (strcmp(msg, "The bulletin board is unavailable right now.") == 0) return "Tablica ogłoszeń jest obecnie niedostępna.";
        if (strcmp(msg, "Unable to allocate editor workspace.") == 0) return "Nie można przydzielić obszaru roboczego edytora.";
        if (strcmp(msg, "Draft saved.") == 0) return "Szkic zapisany.";
        if (strcmp(msg, "Draft loaded.") == 0) return "Szkic wczytany.";
        if (strcmp(msg, "Comment added.") == 0) return "Komentarz dodany.";
        if (strcmp(msg, "You cannot recommend your own post.") == 0) return "Nie możesz polecić własnego posta.";
        if (strcmp(msg, "You have already voted on this post.") == 0) return "Już głosowałeś na ten post.";
        if (strcmp(msg, "Recommendation added.") == 0) return "Polecenie dodane.";
        if (strcmp(msg, "Oppose vote added.") == 0) return "Sprzeciw dodany.";
        if (strcmp(msg, "Joined board: %s — %s") == 0) return "Dołączono do tablicy: %s — %s";
        if (strcmp(msg, "Board not found.") == 0) return "Nie znaleziono tablicy.";
        if (strcmp(msg, "Comment editing is disabled.") == 0) return "Edytowanie komentarzy jest wyłączone.";
        if (strcmp(msg, "Comment deletion is disabled.") == 0) return "Usuwanie komentarzy jest wyłączone.";
        if (strcmp(msg, "Invalid post or comment index.") == 0) return "Nieprawidłowy indeks posta lub komentarza.";
        if (strcmp(msg, "Only the author or an operator may edit this comment.") == 0) return "Tylko autor lub operator może edytować ten komentarz.";
        if (strcmp(msg, "Only the author or an operator may delete this comment.") == 0) return "Tylko autor lub operator może usunąć ten komentarz.";
        if (strcmp(msg, "Comment updated.") == 0) return "Komentarz zaktualizowany.";
        if (strcmp(msg, "Comment deleted.") == 0) return "Komentarz usunięty.";
        if (strcmp(msg, "  Choose your Profile Logo (Avatar)") == 0) return "  Wybierz logo profilu (awatar)";
        if (strcmp(msg, "  Enter choice (1-5):") == 0) return "  Wprowadź wybór (1-5):";
        if (strcmp(msg, "  [1] Monitor") == 0) return "  [1] Monitor";
        if (strcmp(msg, "  [2] Mouse") == 0) return "  [2] Mouse";
        if (strcmp(msg, "  [3] Human") == 0) return "  [3] Human";
        if (strcmp(msg, "  [4] Mushroom") == 0) return "  [4] Mushroom";
        if (strcmp(msg, "  [5] None / Keep Current") == 0) return "  [5] Brak / Zachowaj obecny";
        if (strcmp(msg, " - Read a post:   Type '\033[1;32mread <id>\033[0m'") == 0) return " - Czytaj post:   wpisz '\033[1;32mread <id>\033[0m'";
        if (strcmp(msg, " - Write a post:  Type '\033[1;32mpost <title>\033[0m'") == 0) return " - Napisz post:  wpisz '\033[1;32mpost <title>\033[0m'";
        if (strcmp(msg, "(No profile picture set)") == 0) return "(Brak zdjęcia profilowego)";
        if (strcmp(msg, "----------------------------------------") == 0) return "----------------------------------------";
        if (strcmp(msg, "--------------------------------------------------") == 0) return "--------------------------------------------------";
        if (strcmp(msg, "========================================") == 0) return "========================================";
        if (strcmp(msg, "Already viewing the end of this post.") == 0) return "Już wyświetlasz koniec tego posta.";
        if (strcmp(msg, "Already viewing the top of this post.") == 0) return "Już wyświetlasz początek tego posta.";
        if (strcmp(msg, "Available avatars:") == 0) return "Dostępne awatary:";
        if (strcmp(msg, "Downvoted.") == 0) return "Oddano głos przeciw.";
        if (strcmp(msg, "Draft deleted.") == 0) return "Szkic usunięty.";
        if (strcmp(msg, "Draft loaded. Continue editing with the post editor.") == 0) return "Szkic wczytany. Kontynuuj edycję w edytorze postów.";
        if (strcmp(msg, "Draft not found.") == 0) return "Nie znaleziono szkicu.";
        if (strcmp(msg, "Draft storage full. Delete old drafts first.") == 0) return "Magazyn szkiców jest pełny. Najpierw usuń stare szkice.";
        if (strcmp(msg, "Failed to initialize user data.") == 0) return "Nie udało się zainicjować danych użytkownika.";
        if (strcmp(msg, "Failed to save avatar.") == 0) return "Nie udało się zapisać awatara.";
        if (strcmp(msg, "Failed to save profile logo.") == 0) return "Nie udało się zapisać logo profilu.";
        if (strcmp(msg, "Failed to save profile picture.") == 0) return "Nie udało się zapisać zdjęcia profilowego.";
        if (strcmp(msg, "Missing closing quote for the title.") == 0) return "Brak zamykającego cudzysłowu tytułu.";
        if (strcmp(msg, "No ASCII art pending. Use /asciiart to create one first.") == 0) return "Brak oczekującego ASCII art. Najpierw utwórz je poleceniem /asciiart.";
        if (strcmp(msg, "No drafts.") == 0) return "Brak szkiców.";
        if (strcmp(msg, "No post exists with that identifier anymore.") == 0) return "Post o tym identyfikatorze już nie istnieje.";
        if (strcmp(msg, "No posts found matching the keyword.") == 0) return "Nie znaleziono postów pasujących do słowa kluczowego.";
        if (strcmp(msg, "Nothing to save. Start a post first.") == 0) return "Nic do zapisania. Najpierw rozpocznij post.";
        if (strcmp(msg, "Only the author or an operator may delete this post.") == 0) return "Tylko autor lub operator może usunąć ten post.";
        if (strcmp(msg, "Only the author or an operator may edit this post.") == 0) return "Tylko autor lub operator może edytować ten post.";
        if (strcmp(msg, "Post bumped to the top.") == 0) return "Post przeniesiony na górę.";
        if (strcmp(msg, "Post created.") == 0) return "Post utworzony.";
        if (strcmp(msg, "Post updated.") == 0) return "Post zaktualizowany.";
        if (strcmp(msg, "Profile picture updated.") == 0) return "Zdjęcie profilowe zaktualizowane.";
        if (strcmp(msg, "Recent posts:") == 0) return "Ostatnie posty:";
        if (strcmp(msg, "Specify a topic to read.") == 0) return "Podaj temat do przeczytania.";
        if (strcmp(msg, "Unknown avatar name.") == 0) return "Nieznana nazwa awatara.";
        if (strcmp(msg, "Upvoted.") == 0) return "Oddano głos za.";
        if (strcmp(msg, "Usage: /bbs cmtvote <post_id> <comment_idx> [up|down]") == 0) return "Użycie: /bbs cmtvote <post_id> <comment_idx> [up|down]";
        if (strcmp(msg, "Usage: /bbs draft <save|list|load|delete>") == 0) return "Użycie: /bbs draft <save|list|load|delete>";
        if (strcmp(msg, "Usage: /bbs draft delete <index>") == 0) return "Użycie: /bbs draft delete <index>";
        if (strcmp(msg, "Usage: /bbs draft load <index>") == 0) return "Użycie: /bbs draft load <index>";
        if (strcmp(msg, "Usage: /bbs setavatar <monitor|mouse|human|mushroom|none>") == 0) return "Użycie: /bbs setavatar <monitor|mouse|human|mushroom|none>";
        if (strcmp(msg, "Usage: search <keyword>") == 0) return "Użycie: search <keyword>";
        if (strcmp(msg, "Use | to separate tags when the title has spaces.") == 0) return "Użyj |, aby oddzielić tagi, gdy tytuł zawiera spacje.";
        if (strcmp(msg, "You already downvoted this post.") == 0) return "Już głosowałeś przeciw temu postowi.";
        if (strcmp(msg, "You already upvoted this post.") == 0) return "Już głosowałeś za tym postem.";
        if (strcmp(msg, "You already voted this comment.") == 0) return "Już głosowałeś na ten komentarz.";
        if (strcmp(msg, "[Tip] Share your thoughts on the board!") == 0) return "[Wskazówka] Podziel się opinią na tablicy!";
        if (strcmp(msg, "\033[1;32m>>> Baud Rate: 14400 bps | Terminal: ANSI-BBS | STATUS: ONLINE <<<\033[0m\n") == 0) return "\033[1;32m>>> Szybkość: 14400 bps | Terminal: ANSI-BBS | STATUS: ONLINE <<<\033[0m\n";
        if (strcmp(msg, "\n\033[1;36m>>> Connecting to Retro BBS Network... <<<\033[0m") == 0) return "\n\033[1;36m>>> Łączenie z siecią Retro BBS... <<<\033[0m";
        if (strcmp(msg, "No posts on that page.") == 0) return "Brak postów na tej stronie.";
        if (strcmp(msg, "Page %zu/%zu (next: list %s %zu)") == 0) return "Strona %zu/%zu (dalej: list %s %zu)";
        if (strcmp(msg, "Page %zu/%zu") == 0) return "Strona %zu/%zu";
        if (strcmp(msg, "You are commenting too quickly. Try again in a moment.") == 0) return "Komentujesz zbyt szybko. Spróbuj ponownie za chwilę.";
        if (strcmp(msg, "You are posting too quickly. Try again in a moment.") == 0) return "Publikujesz zbyt szybko. Spróbuj ponownie za chwilę.";
        if (strcmp(msg, "--- Notifications ---") == 0) return "--- Powiadomienia ---";
        if (strcmp(msg, "%s replied to your post #%llu.") == 0) return "%s odpowiedział na twój post #%llu.";
        if (strcmp(msg, "%s mentioned you in post #%llu.") == 0) return "%s wspomniał o tobie w poście #%llu.";
        if (strcmp(msg, "%llu new posts since your last visit.") == 0) return "%llu nowych postów od ostatniej wizyty.";
        break;
    default:
        break;
    }
    return msg;
}

#define session_send_system_line(ctx, msg) session_send_system_line((ctx), session_bbs_localize((ctx)->ui_language, (msg)))
#define session_bbs_render_editor(ctx, status) session_bbs_render_editor((ctx), session_bbs_localize((ctx)->ui_language, (status)))
#define session_bbs_render_post(ctx, post, status, is_new) session_bbs_render_post((ctx), (post), session_bbs_localize((ctx)->ui_language, (status)), (is_new))

static void bbs_format_time(time_t value, char *buffer, size_t length)
{
    if (buffer == nullptr || length == 0U) {
        return;
    }
    struct tm tm_value;
    if (localtime_r(&value, &tm_value) == nullptr) {
        snprintf(buffer, length, "-");
        return;
    }
    strftime(buffer, length, "%Y-%m-%d %H:%M", &tm_value);
}

bool host_bbs_acquire_storage(host_t *host);
static void host_bbs_state_load(host_t *host);

static bool bbs_post_has_required_fields(const bbs_post_t *post)
{
    if (post == nullptr || !post->in_use) {
        return false;
    }

    if (post->id == 0U) {
        return false;
    }

    if (post->author[0] == '\0' || post->title[0] == '\0') {
        return false;
    }

    return true;
}

typedef struct bbs_listing {
    uint64_t id;
    uint16_t board_id;
    char title[SSH_CHATTER_BBS_TITLE_LEN];
    char author[SSH_CHATTER_USERNAME_LEN];
    char tags[SSH_CHATTER_BBS_MAX_TAGS][SSH_CHATTER_BBS_TAG_LEN];
    size_t tag_count;
    time_t created_at;
    time_t bumped_at;
    size_t comment_count;
    int32_t upvotes;
    int32_t downvotes;
} bbs_listing_t;

typedef struct bbs_topic_group {
    char name[SSH_CHATTER_BBS_TAG_LEN];
    size_t indexes[SSH_CHATTER_BBS_MAX_POSTS];
    size_t count;
} bbs_topic_group_t;

static bool session_bbs_ensure_live_storage(host_t *host)
{
    if (host == nullptr) {
        return false;
    }
    if (!host_bbs_acquire_storage(host)) {
        return false;
    }
    if (!host->bbs_cache_loaded) {
        host_bbs_state_load(host);
    }
    return host_bbs_storage_ready(host);
}

static bool session_bbs_read_serialized_entry(
    const unsigned char **cursor_ptr, size_t *remaining_ptr,
    uint32_t state_version, bbs_state_post_entry_disk_t *serialized)
{
    if (cursor_ptr == nullptr || remaining_ptr == nullptr ||
        serialized == nullptr) {
        return false;
    }

    const unsigned char *cursor = *cursor_ptr;
    size_t remaining = *remaining_ptr;
    memset(serialized, 0, sizeof(*serialized));

    if (state_version == BBS_STATE_VERSION_V1) {
        bbs_state_post_entry_disk_v1_t legacy = {0};
        if (remaining < sizeof(legacy)) {
            return false;
        }
        memcpy(&legacy, cursor, sizeof(legacy));
        cursor += sizeof(legacy);
        remaining -= sizeof(legacy);
        bbs_state_post_entry_from_v1(serialized, &legacy);
    } else {
        if (remaining < sizeof(*serialized)) {
            return false;
        }
        memcpy(serialized, cursor, sizeof(*serialized));
        cursor += sizeof(*serialized);
        remaining -= sizeof(*serialized);
    }

    *cursor_ptr = cursor;
    *remaining_ptr = remaining;
    return true;
}

static void session_bbs_normalize_serialized_entry(
    bbs_state_post_entry_disk_t *serialized)
{
    if (serialized == nullptr) {
        return;
    }

    time_t now = time(nullptr);
    if (now <= 0) {
        now = 1;
    }

    serialized->author[sizeof(serialized->author) - 1U] = '\0';
    serialized->title[sizeof(serialized->title) - 1U] = '\0';
    serialized->body[sizeof(serialized->body) - 1U] = '\0';
    for (size_t tag = 0U; tag < SSH_CHATTER_BBS_MAX_TAGS; ++tag) {
        serialized->tags[tag][sizeof(serialized->tags[tag]) - 1U] = '\0';
    }
    for (size_t comment = 0U; comment < SSH_CHATTER_BBS_MAX_COMMENTS;
         ++comment) {
        serialized->comments[comment]
            .author[sizeof(serialized->comments[comment].author) - 1U] = '\0';
        serialized->comments[comment]
            .text[sizeof(serialized->comments[comment].text) - 1U] = '\0';
        if (serialized->comments[comment].created_at <= 0) {
            serialized->comments[comment].created_at = (int64_t)now;
        }
    }

    if (serialized->created_at <= 0) {
        serialized->created_at = (int64_t)now;
    }
    if (serialized->bumped_at <= 0 ||
        serialized->bumped_at < serialized->created_at) {
        serialized->bumped_at = serialized->created_at;
    }
    if (serialized->tag_count > SSH_CHATTER_BBS_MAX_TAGS) {
        serialized->tag_count = SSH_CHATTER_BBS_MAX_TAGS;
    }
    if (serialized->comment_count > SSH_CHATTER_BBS_MAX_COMMENTS) {
        serialized->comment_count = SSH_CHATTER_BBS_MAX_COMMENTS;
    }
}

static bool session_bbs_map_state_file(host_t *host, unsigned char **mapped,
                                       size_t *mapped_len,
                                       bbs_state_header_t *header)
{
    if (host == nullptr || mapped == nullptr || mapped_len == nullptr ||
        header == nullptr || host->bbs_state_file_path[0] == '\0') {
        return false;
    }

    *mapped = nullptr;
    *mapped_len = 0U;
    memset(header, 0, sizeof(*header));

    if (!host_ensure_private_data_path(host, host->bbs_state_file_path,
                                       false)) {
        return false;
    }

    FILE *fp = fopen(host->bbs_state_file_path, "rb");
    if (fp == nullptr) {
        return false;
    }

    int fd = fileno(fp);
    if (fd < 0) {
        fclose(fp);
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        fclose(fp);
        return false;
    }

    *mapped_len = (size_t)st.st_size;
    if (*mapped_len < sizeof(*header)) {
        fclose(fp);
        *mapped_len = 0U;
        return false;
    }

    *mapped = mmap(nullptr, *mapped_len, PROT_READ, MAP_PRIVATE, fd, 0);
    fclose(fp);
    if (*mapped == MAP_FAILED) {
        *mapped = nullptr;
        *mapped_len = 0U;
        return false;
    }

    memcpy(header, *mapped, sizeof(*header));
    if (header->magic != BBS_STATE_MAGIC ||
        (header->version != BBS_STATE_VERSION &&
         header->version != BBS_STATE_VERSION_V1)) {
        munmap(*mapped, *mapped_len);
        *mapped = nullptr;
        *mapped_len = 0U;
        memset(header, 0, sizeof(*header));
        return false;
    }

    return true;
}

static bool session_bbs_collect_listings_from_state(host_t *host,
                                                    bbs_listing_t *listings,
                                                    size_t *count)
{
    if (host == nullptr || listings == nullptr || count == nullptr) {
        return false;
    }

    *count = 0U;
    if (host->bbs_state_file_path[0] == '\0') {
        return false;
    }
    if (access(host->bbs_state_file_path, F_OK) != 0) {
        return true;
    }

    unsigned char *mapped = nullptr;
    size_t mapped_len = 0U;
    bbs_state_header_t header = {0};
    if (!session_bbs_map_state_file(host, &mapped, &mapped_len, &header)) {
        return false;
    }

    const unsigned char *cursor = mapped + sizeof(header);
    size_t remaining = mapped_len - sizeof(header);
    time_t now = time(nullptr);
    if (now <= 0) {
        now = 1;
    }

    for (uint32_t idx = 0U;
         idx < header.post_count && *count < SSH_CHATTER_BBS_MAX_POSTS; ++idx) {
        bbs_state_post_entry_disk_t serialized = {0};
        if (!session_bbs_read_serialized_entry(&cursor, &remaining,
                                               header.version, &serialized)) {
            break;
        }
        session_bbs_normalize_serialized_entry(&serialized);
        if (!host_bbs_serialized_has_required_fields(&serialized) ||
            !host_bbs_serialized_is_sane(&serialized, now)) {
            continue;
        }

        bbs_listing_t *entry = &listings[*count];
        memset(entry, 0, sizeof(*entry));
        entry->id = serialized.id;
        entry->board_id = serialized.board_id;
        entry->tag_count = serialized.tag_count;
        entry->created_at = (time_t)serialized.created_at;
        entry->bumped_at = (time_t)serialized.bumped_at;
        entry->comment_count = serialized.comment_count;
        entry->upvotes = serialized.upvotes;
        entry->downvotes = serialized.downvotes;
        snprintf(entry->title, sizeof(entry->title), "%s", serialized.title);
        snprintf(entry->author, sizeof(entry->author), "%s",
                 serialized.author);
        for (size_t tag = 0U; tag < serialized.tag_count; ++tag) {
            snprintf(entry->tags[tag], sizeof(entry->tags[tag]), "%s",
                     serialized.tags[tag]);
        }
        *count += 1U;
    }

    munmap(mapped, mapped_len);
    return true;
}

static bool session_bbs_collect_listings(host_t *host, bbs_listing_t *listings,
                                         size_t *count)
{
    if (host == nullptr || listings == nullptr || count == nullptr) {
        return false;
    }

    *count = 0U;
    if (host_bbs_storage_ready(host) && host->bbs_cache_loaded) {
        ttak_mutex_lock(&host->lock);
        size_t capacity = host_bbs_loop_limit(host);
        for (size_t idx = 0U;
             idx < capacity && *count < SSH_CHATTER_BBS_MAX_POSTS; ++idx) {
            const bbs_post_t *post = &host->bbs_posts[idx];
            if (!bbs_post_has_required_fields(post)) {
                continue;
            }

            bbs_listing_t *entry = &listings[*count];
            memset(entry, 0, sizeof(*entry));
            entry->id = post->id;
            entry->board_id = post->board_id;
            entry->tag_count = post->tag_count;
            entry->created_at = post->created_at;
            entry->bumped_at = post->bumped_at;
            entry->comment_count = post->comment_count;
            entry->upvotes = post->upvotes;
            entry->downvotes = post->downvotes;
            snprintf(entry->title, sizeof(entry->title), "%s", post->title);
            snprintf(entry->author, sizeof(entry->author), "%s",
                     post->author);
            for (size_t tag = 0U; tag < post->tag_count; ++tag) {
                snprintf(entry->tags[tag], sizeof(entry->tags[tag]), "%s",
                         post->tags[tag]);
            }
            *count += 1U;
        }
        ttak_mutex_unlock(&host->lock);
        return true;
    }

    return session_bbs_collect_listings_from_state(host, listings, count);
}

static bool session_bbs_load_post_from_state(host_t *host, uint64_t id,
                                             bbs_post_t *post)
{
    if (host == nullptr || post == nullptr || id == 0U) {
        return false;
    }

    bbs_comment_t *saved_comments = post->comments;
    memset(post, 0, sizeof(*post));
    post->comments = saved_comments;
    if (post->comments == nullptr) {
        post->comments = sshc_gc_calloc(SSH_CHATTER_BBS_MAX_COMMENTS, sizeof(bbs_comment_t));
    }

    unsigned char *mapped = nullptr;
    size_t mapped_len = 0U;
    bbs_state_header_t header = {0};
    if (!session_bbs_map_state_file(host, &mapped, &mapped_len, &header)) {
        return false;
    }

    const unsigned char *cursor = mapped + sizeof(header);
    size_t remaining = mapped_len - sizeof(header);
    time_t now = time(nullptr);
    if (now <= 0) {
        now = 1;
    }

    bool found = false;
    for (uint32_t idx = 0U; idx < header.post_count; ++idx) {
        bbs_state_post_entry_disk_t serialized = {0};
        if (!session_bbs_read_serialized_entry(&cursor, &remaining,
                                               header.version, &serialized)) {
            break;
        }
        session_bbs_normalize_serialized_entry(&serialized);
        if (!host_bbs_serialized_has_required_fields(&serialized) ||
            !host_bbs_serialized_is_sane(&serialized, now) ||
            serialized.id != id) {
            continue;
        }

        post->in_use = true;
        post->id = serialized.id;
        post->tag_count = serialized.tag_count;
        post->comment_count = serialized.comment_count;
        post->created_at = (time_t)serialized.created_at;
        post->bumped_at = (time_t)serialized.bumped_at;
        snprintf(post->author, sizeof(post->author), "%s", serialized.author);
        snprintf(post->title, sizeof(post->title), "%s", serialized.title);
        snprintf(post->body, sizeof(post->body), "%s", serialized.body);
        for (size_t tag = 0U; tag < serialized.tag_count; ++tag) {
            snprintf(post->tags[tag], sizeof(post->tags[tag]), "%s",
                     serialized.tags[tag]);
        }
        for (size_t comment = 0U; comment < serialized.comment_count;
             ++comment) {
            snprintf(post->comments[comment].author,
                     sizeof(post->comments[comment].author), "%s",
                     serialized.comments[comment].author);
            snprintf(post->comments[comment].text,
                     sizeof(post->comments[comment].text), "%s",
                     serialized.comments[comment].text);
            post->comments[comment].created_at =
                (time_t)serialized.comments[comment].created_at;
        }
        found = true;
        break;
    }

    munmap(mapped, mapped_len);
    return found;
}

static bool session_bbs_load_post(host_t *host, uint64_t id, bbs_post_t *post)
{
    if (host == nullptr || post == nullptr || id == 0U) {
        return false;
    }

    bbs_comment_t *saved_comments = post->comments;
    memset(post, 0, sizeof(*post));
    post->comments = saved_comments;

    if (host_bbs_storage_ready(host) && host->bbs_cache_loaded) {
        ttak_mutex_lock(&host->lock);
        bbs_post_t *live = host_find_bbs_post_locked(host, id);
        if (live != nullptr && live->in_use) {
            bbs_comment_t *dest_comments = post->comments;
            if (dest_comments == nullptr) {
                dest_comments = sshc_gc_calloc(SSH_CHATTER_BBS_MAX_COMMENTS, sizeof(bbs_comment_t));
            }
            *post = *live;
            post->comments = dest_comments;
            if (live->comments != nullptr && dest_comments != nullptr) {
                memcpy(dest_comments, live->comments, sizeof(bbs_comment_t) * SSH_CHATTER_BBS_MAX_COMMENTS);
            }
        }
        ttak_mutex_unlock(&host->lock);
        if (post->in_use) {
            return true;
        }
    }

    return session_bbs_load_post_from_state(host, id, post);
}

// Return a post by identifier while the host lock is held.
static bbs_post_t *host_find_bbs_post_locked(host_t *host, uint64_t id)
{
    if (!host_bbs_storage_ready(host) || id == 0U) {
        return nullptr;
    }
    for (size_t idx = 0U; idx < host->bbs_post_capacity; ++idx) {
        if (!bbs_post_has_required_fields(&host->bbs_posts[idx])) {
            continue;
        }
        if (host->bbs_posts[idx].id == id) {
            return &host->bbs_posts[idx];
        }
    }
    return nullptr;
}

// Allocate a new post slot, returning nullptr if capacity has been reached.
static bbs_post_t *host_allocate_bbs_post_locked(host_t *host)
{
    if (!host_bbs_storage_ready(host)) {
        return nullptr;
    }
    for (size_t idx = 0U; idx < host->bbs_post_capacity; ++idx) {
        if (host->bbs_posts[idx].in_use) {
            continue;
        }
        bbs_post_t *post = &host->bbs_posts[idx];
        post->in_use = true;
        post->id = host->next_bbs_id++;
        post->tag_count = 0U;
        post->comment_count = 0U;
        post->created_at = time(nullptr);
        post->bumped_at = post->created_at;
        post->title[0] = '\0';
        post->body[0] = '\0';
        post->author[0] = '\0';
        for (size_t tag = 0U; tag < SSH_CHATTER_BBS_MAX_TAGS; ++tag) {
            post->tags[tag][0] = '\0';
        }
        for (size_t comment = 0U; comment < SSH_CHATTER_BBS_MAX_COMMENTS;
             ++comment) {
            post->comments[comment].author[0] = '\0';
            post->comments[comment].text[0] = '\0';
            post->comments[comment].created_at = 0;
        }
        if (host->bbs_post_count < SSH_CHATTER_BBS_MAX_POSTS) {
            host->bbs_post_count += 1U;
        }
        return post;
    }
    return nullptr;
}

static void host_reset_bbs_post(bbs_post_t *post)
{
    if (post == nullptr) {
        return;
    }

    post->in_use = false;
    post->id = 0U;
    post->author[0] = '\0';
    post->title[0] = '\0';
    post->body[0] = '\0';
    post->tag_count = 0U;
    post->created_at = 0;
    post->bumped_at = 0;
    post->comment_count = 0U;
    for (size_t tag = 0U; tag < SSH_CHATTER_BBS_MAX_TAGS; ++tag) {
        post->tags[tag][0] = '\0';
    }
    for (size_t comment = 0U; comment < SSH_CHATTER_BBS_MAX_COMMENTS;
         ++comment) {
        post->comments[comment].author[0] = '\0';
        post->comments[comment].text[0] = '\0';
        post->comments[comment].created_at = 0;
        post->comments[comment].edited_at = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Anti-flood rate limiting (per user, in-RAM only)                   */
/* ------------------------------------------------------------------ */

static bbs_rate_limit_entry_t *
host_bbs_rate_limit_find_locked(host_t *host, const char *username)
{
    if (host == nullptr || username == nullptr) {
        return nullptr;
    }
    for (size_t idx = 0U; idx < host->bbs_rate_limits.count; ++idx) {
        if (strncmp(host->bbs_rate_limits.entries[idx].username, username,
                    SSH_CHATTER_USERNAME_LEN) == 0) {
            return &host->bbs_rate_limits.entries[idx];
        }
    }
    return nullptr;
}

// Find-or-add a rate limit entry.  When the table is full, evict the entry
// with the oldest activity timestamp.
static bbs_rate_limit_entry_t *
host_bbs_rate_limit_ensure_locked(host_t *host, const char *username)
{
    if (host == nullptr || username == nullptr || username[0] == '\0') {
        return nullptr;
    }
    bbs_rate_limit_entry_t *existing =
        host_bbs_rate_limit_find_locked(host, username);
    if (existing != nullptr) {
        return existing;
    }

    size_t slot = 0U;
    if (host->bbs_rate_limits.count < SSH_CHATTER_BBS_MAX_RATE_LIMITS) {
        slot = host->bbs_rate_limits.count;
        ++host->bbs_rate_limits.count;
    } else {
        time_t oldest = (time_t)-1;
        for (size_t idx = 0U; idx < SSH_CHATTER_BBS_MAX_RATE_LIMITS; ++idx) {
            time_t stamp = host->bbs_rate_limits.entries[idx].last_comment_at;
            if (host->bbs_rate_limits.entries[idx].last_post_at > stamp) {
                stamp = host->bbs_rate_limits.entries[idx].last_post_at;
            }
            if (stamp < oldest) {
                oldest = stamp;
                slot = idx;
            }
        }
    }

    bbs_rate_limit_entry_t *entry = &host->bbs_rate_limits.entries[slot];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->username, sizeof(entry->username), "%s", username);
    return entry;
}

/* ------------------------------------------------------------------ */
/* Notification queue producers                                       */
/* ------------------------------------------------------------------ */

static void host_bbs_notification_enqueue_locked(host_t *host, int32_t kind,
                                                 const char *to,
                                                 const char *from,
                                                 uint64_t post_id,
                                                 int32_t comment_idx)
{
    if (host == nullptr || to == nullptr || to[0] == '\0' || from == nullptr ||
        from[0] == '\0') {
        return;
    }
    if (host->bbs_notifications == nullptr ||
        host->bbs_notification_capacity == 0U) {
        host->bbs_notifications =
            sshc_gc_calloc(SSH_CHATTER_BBS_MAX_NOTIFICATIONS,
                           sizeof(bbs_notification_t));
        if (host->bbs_notifications == nullptr) {
            return;
        }
        host->bbs_notification_capacity = SSH_CHATTER_BBS_MAX_NOTIFICATIONS;
        host->bbs_notification_count = 0U;
    }
    if (host->bbs_notification_count >= host->bbs_notification_capacity) {
        // Overflow: drop the oldest entry.
        memmove(host->bbs_notifications, host->bbs_notifications + 1,
                (host->bbs_notification_count - 1U) *
                    sizeof(*host->bbs_notifications));
        --host->bbs_notification_count;
    }

    bbs_notification_t *entry =
        &host->bbs_notifications[host->bbs_notification_count++];
    memset(entry, 0, sizeof(*entry));
    entry->kind = kind;
    snprintf(entry->to, sizeof(entry->to), "%s", to);
    snprintf(entry->from, sizeof(entry->from), "%s", from);
    entry->post_id = post_id;
    entry->comment_idx = comment_idx;
    entry->created_at = time(nullptr);
}

static void
host_bbs_notification_drop_for_post_locked(host_t *host, uint64_t post_id)
{
    if (host == nullptr || host->bbs_notifications == nullptr) {
        return;
    }
    size_t write_idx = 0U;
    for (size_t idx = 0U; idx < host->bbs_notification_count; ++idx) {
        if (host->bbs_notifications[idx].post_id == post_id) {
            continue;
        }
        host->bbs_notifications[write_idx++] = host->bbs_notifications[idx];
    }
    host->bbs_notification_count = write_idx;
}

// Scan text for @nickname tokens: '@' followed by 1..USERNAME_LEN-1 chars of
// [A-Za-z0-9_], stopping at the first other character.  Returns the number of
// distinct names collected (bounded by max_names).
static size_t
session_bbs_scan_mentions(const char *text,
                          char names[][SSH_CHATTER_USERNAME_LEN],
                          size_t max_names)
{
    if (text == nullptr || names == nullptr || max_names == 0U) {
        return 0U;
    }

    size_t found = 0U;
    const char *cursor = text;
    while (*cursor != '\0' && found < max_names) {
        const char *at = strchr(cursor, '@');
        if (at == nullptr) {
            break;
        }
        const char *name_start = at + 1;
        size_t name_len = 0U;
        while (name_len + 1U < SSH_CHATTER_USERNAME_LEN &&
               name_start[name_len] != '\0') {
            char c = name_start[name_len];
            bool name_char = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '_';
            if (!name_char) {
                break;
            }
            ++name_len;
        }
        if (name_len == 0U) {
            cursor = name_start;
            continue;
        }

        char candidate[SSH_CHATTER_USERNAME_LEN] = {0};
        memcpy(candidate, name_start, name_len);
        candidate[name_len] = '\0';

        bool distinct = true;
        for (size_t idx = 0U; idx < found; ++idx) {
            if (strncmp(names[idx], candidate, SSH_CHATTER_USERNAME_LEN) ==
                0) {
                distinct = false;
                break;
            }
        }
        if (distinct) {
            snprintf(names[found], SSH_CHATTER_USERNAME_LEN, "%s", candidate);
            ++found;
        }
        cursor = name_start + name_len;
    }
    return found;
}

// Enqueue mention notifications for an existing user named in text, excluding
// the actor and unknown usernames.
static void
session_bbs_notify_mentions_locked(host_t *host, const char *text,
                                   const char *actor, uint64_t post_id,
                                   int32_t comment_idx)
{
    if (host == nullptr || text == nullptr || actor == nullptr) {
        return;
    }
    char names[8][SSH_CHATTER_USERNAME_LEN];
    size_t count = session_bbs_scan_mentions(text, names, 8U);
    user_data_record_t record = {0};
    for (size_t idx = 0U; idx < count; ++idx) {
        if (strncmp(names[idx], actor, SSH_CHATTER_USERNAME_LEN) == 0) {
            continue;
        }
        if (!user_data_load(host->user_data_root, names[idx], "", &record)) {
            continue;
        }
        host_bbs_notification_enqueue_locked(host, 1, names[idx], actor,
                                             post_id, comment_idx);
    }
}

static void host_clear_bbs_post_locked(host_t *host, bbs_post_t *post)
{
    if (!host_bbs_storage_ready(host) || post == nullptr) {
        return;
    }

    host_reset_bbs_post(post);

    size_t write_index = 0U;
    for (size_t idx = 0U; idx < host->bbs_post_capacity; ++idx) {
        if (!host->bbs_posts[idx].in_use) {
            continue;
        }

        if (write_index != idx) {
            host->bbs_posts[write_index] = host->bbs_posts[idx];
        }

        ++write_index;
    }

    for (size_t idx = write_index; idx < host->bbs_post_capacity; ++idx) {
        host_reset_bbs_post(&host->bbs_posts[idx]);
    }

    host->bbs_post_count = write_index;
}

// Render an ASCII framed view of a post, including metadata and comments.

static bool session_bbs_refresh_view(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr || !ctx->bbs_view_active ||
        ctx->bbs_view_post_id == 0U) {
        return false;
    }

    host_t *host = ctx->owner;
    bbs_post_t snapshot = {0};
    if (!session_bbs_load_post(host, ctx->bbs_view_post_id, &snapshot) ||
        !snapshot.in_use) {
        ctx->bbs_view_active = false;
        ctx->bbs_view_post_id = 0U;
        ctx->bbs_view_total_lines = 0U;
        ctx->bbs_view_scroll_offset = 0U;
        session_send_system_line(ctx, "That post is no longer available.");
        return false;
    }

    session_bbs_render_post(ctx, &snapshot, nullptr, false);
    return true;
}

static bool session_bbs_scroll(session_ctx_t *ctx, int direction, size_t step)
{
    if (ctx == nullptr || ctx->owner == nullptr || !ctx->bbs_view_active ||
        direction == 0) {
        return false;
    }

    size_t window = SSH_CHATTER_BBS_VIEW_WINDOW;
    if (window == 0U) {
        window = 1U;
    }

    size_t total = ctx->bbs_view_total_lines;
    if (total <= window) {
        if (direction > 0) {
            session_send_system_line(ctx,
                                     "Already viewing the top of this post.");
        } else if (direction < 0) {
            session_send_system_line(ctx,
                                     "Already viewing the end of this post.");
        }
        return true;
    }

    size_t max_offset = total - window;
    size_t offset = ctx->bbs_view_scroll_offset;
    size_t effective_step = step;
    if (effective_step == 0U) {
        effective_step = window;
    }
    if (effective_step == 0U) {
        effective_step = 1U;
    }

    size_t new_offset = offset;
    if (direction > 0) {
        if (offset == 0U) {
            session_send_system_line(ctx,
                                     "Already viewing the top of this post.");
            return true;
        }
        if (effective_step > offset) {
            effective_step = offset;
        }
        if (effective_step == 0U) {
            effective_step = 1U;
        }
        new_offset = offset - effective_step;
    } else if (direction < 0) {
        if (offset >= max_offset) {
            session_send_system_line(ctx,
                                     "Already viewing the end of this post.");
            return true;
        }
        size_t advance = effective_step;
        if (advance > max_offset - offset) {
            advance = max_offset - offset;
        }
        if (advance == 0U) {
            advance = 1U;
        }
        new_offset = offset + advance;
    }

    if (new_offset == offset) {
        if (direction > 0) {
            session_send_system_line(ctx,
                                     "Already viewing the top of this post.");
        } else if (direction < 0) {
            session_send_system_line(ctx,
                                     "Already viewing the end of this post.");
        }
        return true;
    }

    ctx->bbs_view_scroll_offset = new_offset;
    return session_bbs_refresh_view(ctx);
}

// Show the BBS dashboard and mark the session as being in BBS mode.
static void session_bbs_show_dashboard(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    ctx->in_bbs_mode = true;
    ctx->bbs_view_active = false;
    ctx->bbs_view_post_id = 0U;
    session_mode_push_chat_context(ctx);

    host_t *host = ctx->owner;
    if (host != nullptr && session_bbs_ensure_live_storage(host)) {
        ttak_mutex_lock(&host->lock);

        // Drain notifications addressed to this user.
        size_t pending_count = 0U;
        for (size_t idx = 0U; idx < host->bbs_notification_count; ++idx) {
            if (strncmp(host->bbs_notifications[idx].to, ctx->user.name,
                        SSH_CHATTER_USERNAME_LEN) == 0) {
                ++pending_count;
            }
        }
        if (pending_count > 0U) {
            session_send_system_line(ctx, "--- Notifications ---");
            size_t write_idx = 0U;
            for (size_t idx = 0U; idx < host->bbs_notification_count; ++idx) {
                bbs_notification_t notice = host->bbs_notifications[idx];
                if (strncmp(notice.to, ctx->user.name,
                            SSH_CHATTER_USERNAME_LEN) != 0) {
                    host->bbs_notifications[write_idx++] = notice;
                    continue;
                }
                char line[SSH_CHATTER_MESSAGE_LIMIT];
                const char *fmt =
                    notice.kind == 0
                        ? session_bbs_localize(ctx->ui_language,
                                               "%s replied to your post #%llu.")
                        : session_bbs_localize(ctx->ui_language,
                                               "%s mentioned you in post #%llu.");
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
                snprintf(line, sizeof(line), fmt, notice.from,
                         (unsigned long long)notice.post_id);
#pragma GCC diagnostic pop
                session_send_system_line(ctx, line);
            }
            host->bbs_notification_count = write_idx;
        }

        // Read tracking: report posts bumped since the last visit, then
        // stamp "now".
        bbs_read_mark_t *mark = nullptr;
        for (size_t idx = 0U; idx < host->bbs_read_mark_count; ++idx) {
            if (strncmp(host->bbs_read_marks[idx].username, ctx->user.name,
                        SSH_CHATTER_USERNAME_LEN) == 0) {
                mark = &host->bbs_read_marks[idx];
                break;
            }
        }
        if (mark != nullptr && mark->last_read_at > 0) {
            size_t fresh = 0U;
            for (size_t idx = 0U; idx < host->bbs_post_capacity; ++idx) {
                if (host->bbs_posts[idx].in_use &&
                    host->bbs_posts[idx].bumped_at > mark->last_read_at) {
                    ++fresh;
                }
            }
            if (fresh > 0U) {
                const char *fmt = session_bbs_localize(
                    ctx->ui_language, "%llu new posts since your last visit.");
                char line[SSH_CHATTER_MESSAGE_LIMIT];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
                snprintf(line, sizeof(line), fmt,
                         (unsigned long long)fresh);
#pragma GCC diagnostic pop
                session_send_system_line(ctx, line);
            }
        }
        if (mark == nullptr && host->bbs_read_marks != nullptr &&
            host->bbs_read_mark_count < SSH_CHATTER_BBS_MAX_READMARKS) {
            mark = &host->bbs_read_marks[host->bbs_read_mark_count++];
            memset(mark, 0, sizeof(*mark));
            snprintf(mark->username, sizeof(mark->username), "%s",
                     ctx->user.name);
        }
        if (mark != nullptr) {
            mark->last_read_at = time(nullptr);
        }
        host_bbs_readmarks_save_locked(host);

        ttak_mutex_unlock(&host->lock);
    }

    session_send_system_line(ctx, "\n\033[1;36m>>> Connecting to Retro BBS Network... <<<\033[0m");
    session_send_system_line(ctx, "\033[1;32m>>> Baud Rate: 14400 bps | Terminal: ANSI-BBS | STATUS: ONLINE <<<\033[0m\n");

    session_bbs_prepare_canvas(ctx);
    session_render_separator(ctx, "BBS Dashboard");
    session_send_system_line(
        ctx, "Commands: list [all|hot|top|new] [page], read <id>, topic read <tag>, "
             "post <title> [tags...], edit <id>, "
             "comment <id>|<text> (:N quotes, @nick mentions), "
             "cmtedit <id> <idx> <text>, cmtdel <id> <idx>, "
             "upvote <id>, downvote <id>, cmtvote <id> <idx> up|down, "
             "regen <id>, delete <id>, search <keyword>, board <id>, "
             "boards, profile, set-profile, draft <save|list|load|delete>, "
             "setavatar <name>, door [name], setgamelock <name>, exit");
    session_bbs_list(ctx, nullptr);
}

// List posts sorted by most recent activity.
static void session_bbs_list(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    enum { SESSION_BBS_TOPIC_NAME_PREC = SSH_CHATTER_BBS_TAG_LEN - 1 };

    bool previous_override = session_translation_push_scope_override(ctx);
    bbs_listing_t *listings =
        (bbs_listing_t *)sshc_gc_calloc(SSH_CHATTER_BBS_MAX_POSTS,
                                          sizeof(bbs_listing_t));
    if (listings == nullptr) {
        session_send_system_line(ctx, "BBS storage is unavailable.");
        session_translation_pop_scope_override(ctx, previous_override);
        return;
    }
    size_t count = 0U;

    host_t *host = ctx->owner;
    if (!session_bbs_collect_listings(host, listings, &count)) {
        session_send_system_line(ctx, "BBS storage is unavailable.");
        sshc_gc_free(listings);
        session_translation_pop_scope_override(ctx, previous_override);
        return;
    }

    // Filter by current board selection unless "/bbs list all" (or variant) is requested
    bool list_all = false;
    size_t requested_page = 1U;
    enum { SORT_BUMPED, SORT_HOT, SORT_TOP, SORT_NEW } sort_mode = SORT_BUMPED;

    if (arguments != nullptr) {
        char args_copy[128];
        snprintf(args_copy, sizeof(args_copy), "%s", arguments);
        trim_whitespace_inplace(args_copy);

        char *token = args_copy;
        while (token != nullptr && *token != '\0') {
            char *next = strchr(token, ' ');
            if (next != nullptr) {
                *next = '\0';
                ++next;
                while (*next == ' ') ++next;
            }

            if (strcasecmp(token, "all") == 0) {
                list_all = true;
            } else if (strcasecmp(token, "hot") == 0) {
                sort_mode = SORT_HOT;
            } else if (strcasecmp(token, "top") == 0) {
                sort_mode = SORT_TOP;
            } else if (strcasecmp(token, "new") == 0) {
                sort_mode = SORT_NEW;
            } else {
                char *page_end = nullptr;
                errno = 0;
                unsigned long parsed_page = strtoul(token, &page_end, 10);
                if (errno == 0 && page_end != nullptr && page_end != token &&
                    *page_end == '\0' && parsed_page > 0UL) {
                    requested_page = (size_t)parsed_page;
                }
            }
            token = next;
        }
    }

    if (!list_all) {
        size_t write_idx = 0U;
        for (size_t read_idx = 0U; read_idx < count; ++read_idx) {
            if (listings[read_idx].board_id == ctx->bbs_current_board_id) {
                listings[write_idx++] = listings[read_idx];
            }
        }
        count = write_idx;
    }

    if (count == 0U) {
        char empty_hint[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(
            empty_hint, sizeof(empty_hint),
            "The bulletin board is empty. Use /bbs post <title> [tags...] to "
            "write something. Finish drafts with %s.",
            session_bbs_terminator(ctx));
        session_send_system_line(ctx, empty_hint);
        sshc_gc_free(listings);
        session_translation_pop_scope_override(ctx, previous_override);
        return;
    }

    for (size_t outer = 1U; outer < count; ++outer) {
        bbs_listing_t key = listings[outer];
        size_t position = outer;
        while (position > 0U) {
            bool swap = false;
            bbs_listing_t prev = listings[position - 1U];
            if (sort_mode == SORT_HOT) {
                int64_t score_prev = (int64_t)(prev.upvotes - prev.downvotes) * 2 + (int64_t)prev.comment_count;
                int64_t score_key = (int64_t)(key.upvotes - key.downvotes) * 2 + (int64_t)key.comment_count;
                if (score_prev < score_key) {
                    swap = true;
                } else if (score_prev == score_key && prev.bumped_at < key.bumped_at) {
                    swap = true;
                }
            } else if (sort_mode == SORT_TOP) {
                int32_t score_prev = prev.upvotes - prev.downvotes;
                int32_t score_key = key.upvotes - key.downvotes;
                if (score_prev < score_key) {
                    swap = true;
                } else if (score_prev == score_key && prev.bumped_at < key.bumped_at) {
                    swap = true;
                }
            } else if (sort_mode == SORT_NEW) {
                if (prev.created_at < key.created_at) {
                    swap = true;
                }
            } else {
                if (prev.bumped_at < key.bumped_at) {
                    swap = true;
                }
            }

            if (swap) {
                listings[position] = listings[position - 1U];
                --position;
            } else {
                break;
            }
        }
        listings[position] = key;
    }

    // Paginate the sorted listings: 20 posts per page, 1-based page numbers.
    const size_t page_size = 20U;
    size_t total_pages = (count + page_size - 1U) / page_size;
    size_t current_page = requested_page;
    if (current_page > total_pages) {
        session_send_system_line(ctx, "No posts on that page.");
        sshc_gc_free(listings);
        session_translation_pop_scope_override(ctx, previous_override);
        return;
    }
    size_t page_start = (current_page - 1U) * page_size;
    size_t page_end = page_start + page_size;
    if (page_end > count) {
        page_end = count;
    }
    if (page_start > 0U) {
        memmove(listings, listings + page_start,
                (page_end - page_start) * sizeof(*listings));
    }
    count = page_end - page_start;

    ctx->bbs_view_active = false;
    ctx->bbs_view_post_id = 0U;

    bbs_topic_group_t *topics =
        (bbs_topic_group_t *)sshc_gc_calloc(SSH_CHATTER_BBS_MAX_POSTS,
                                            sizeof(bbs_topic_group_t));
    if (topics == nullptr) {
        sshc_gc_free(listings);
        session_translation_pop_scope_override(ctx, previous_override);
        return;
    }
    size_t topic_count = 0U;

    for (size_t idx = 0U; idx < count; ++idx) {
        const char *topic_name = (listings[idx].tag_count > 0U)
                                     ? listings[idx].tags[0]
                                     : SSH_CHATTER_BBS_DEFAULT_TAG;
        size_t match = topic_count;
        for (size_t topic_idx = 0U; topic_idx < topic_count; ++topic_idx) {
            if (strcasecmp(topics[topic_idx].name, topic_name) == 0) {
                match = topic_idx;
                break;
            }
        }
        if (match == topic_count) {
            if (topic_count >= SSH_CHATTER_BBS_MAX_POSTS) {
                continue;
            }
            snprintf(topics[match].name, sizeof(topics[match].name), "%s",
                     topic_name);
            topics[match].count = 0U;
            ++topic_count;
        }
        if (topics[match].count < SSH_CHATTER_BBS_MAX_POSTS) {
            topics[match].indexes[topics[match].count++] = idx;
        }
    }

    for (size_t outer = 1U; outer < topic_count; ++outer) {
        bbs_topic_group_t key = topics[outer];
        size_t position = outer;
        while (position > 0U &&
               strcasecmp(topics[position - 1U].name, key.name) > 0) {
            topics[position] = topics[position - 1U];
            --position;
        }
        topics[position] = key;
    }

    char separator_title[128];
    const char *sort_name = "bumped";
    if (sort_mode == SORT_HOT) sort_name = "hot";
    else if (sort_mode == SORT_TOP) sort_name = "top";
    else if (sort_mode == SORT_NEW) sort_name = "new";

    if (list_all) {
        snprintf(separator_title, sizeof(separator_title), "BBS Posts (All Boards / Sort: %s)", sort_name);
    } else {
        const char *board_name = "general";
        ttak_mutex_lock(&host->lock);
        for (size_t i = 0; i < host->bbs_board_count; ++i) {
            if (host->bbs_boards[i].board_id == ctx->bbs_current_board_id) {
                board_name = host->bbs_boards[i].name;
                break;
            }
        }
        ttak_mutex_unlock(&host->lock);
        snprintf(separator_title, sizeof(separator_title), "BBS Posts (Board: %s / Sort: %s)", board_name, sort_name);
    }
    session_render_separator(ctx, separator_title);
    for (size_t topic_idx = 0U; topic_idx < topic_count; ++topic_idx) {
        char section_label[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(section_label, sizeof(section_label), "Topic: %.*s",
                 SESSION_BBS_TOPIC_NAME_PREC, topics[topic_idx].name);
        session_render_separator(ctx, section_label);

        for (size_t entry_idx = 0U; entry_idx < topics[topic_idx].count;
             ++entry_idx) {
            size_t listing_index = topics[topic_idx].indexes[entry_idx];
            const bbs_listing_t *entry = &listings[listing_index];
            char created_buffer[32];
            bbs_format_time(entry->bumped_at, created_buffer,
                            sizeof(created_buffer));
            char line[SSH_CHATTER_MESSAGE_LIMIT];
            int title_preview =
                (int)strnlen(entry->title, sizeof(entry->title));
            if (title_preview > 80) {
                title_preview = 80;
            }

            int32_t score = entry->upvotes - entry->downvotes;
            char stats_buf[64];
            snprintf(stats_buf, sizeof(stats_buf), "\033[1;32mScore: %d\033[0m \033[1;36mComments: %zu\033[0m", score, entry->comment_count);

            if (entry->tag_count == 0U) {
                snprintf(line, sizeof(line), "%s #%" PRIu64 " [%s] %.*s | (no tags)",
                         stats_buf, entry->id, created_buffer, title_preview,
                         entry->title);
            } else {
                char tag_buffer[SSH_CHATTER_MESSAGE_LIMIT];
                size_t buffer_offset = 0U;
                tag_buffer[0] = '\0';
                for (size_t tag = 0U; tag < entry->tag_count; ++tag) {
                    size_t len = strlen(entry->tags[tag]);
                    if (buffer_offset + len + 2U >= sizeof(tag_buffer)) {
                        break;
                    }
                    if (tag > 0U) {
                        tag_buffer[buffer_offset++] = ',';
                    }
                    memcpy(tag_buffer + buffer_offset, entry->tags[tag], len);
                    buffer_offset += len;
                    tag_buffer[buffer_offset] = '\0';
                }
                int tags_preview = (int)strnlen(tag_buffer, sizeof(tag_buffer));
                if (tags_preview > 80) {
                    tags_preview = 80;
                }
                snprintf(line, sizeof(line), "%s #%" PRIu64 " [%s] %.*s | %.*s",
                         stats_buf, entry->id, created_buffer, title_preview, entry->title,
                         tags_preview, tag_buffer);
            }
            session_send_system_line(ctx, line);
        }
    }

    if (total_pages > 1U) {
        char footer[SSH_CHATTER_MESSAGE_LIMIT];
        if (current_page < total_pages) {
            const char *footer_fmt = session_bbs_localize(
                ctx->ui_language, "Page %zu/%zu (next: list %s %zu)");
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
            snprintf(footer, sizeof(footer), footer_fmt, current_page,
                     total_pages, list_all ? "all" : sort_name,
                     current_page + 1U);
#pragma GCC diagnostic pop
        } else {
            const char *footer_fmt = session_bbs_localize(
                ctx->ui_language, "Page %zu/%zu");
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
            snprintf(footer, sizeof(footer), footer_fmt, current_page,
                     total_pages);
#pragma GCC diagnostic pop
        }
        session_send_system_line(ctx, footer);
    }

    session_render_separator(ctx, "End");
    session_send_system_line(ctx, "--------------------------------------------------");
    session_send_system_line(ctx, "[Tip] Share your thoughts on the board!");
    session_send_system_line(ctx, " - Write a post:  Type '\033[1;32mpost <title>\033[0m'");
    session_send_system_line(ctx, " - Read a post:   Type '\033[1;32mread <id>\033[0m'");
    session_send_system_line(ctx, "--------------------------------------------------");
    sshc_gc_free(topics);
    sshc_gc_free(listings);
    session_translation_pop_scope_override(ctx, previous_override);
}

static void session_bbs_list_topic(session_ctx_t *ctx, const char *topic)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    char working_topic[SSH_CHATTER_BBS_TAG_LEN];
    if (topic != nullptr) {
        snprintf(working_topic, sizeof(working_topic), "%s", topic);
    } else {
        working_topic[0] = '\0';
    }
    trim_whitespace_inplace(working_topic);

    if (working_topic[0] == '\0') {
        session_send_system_line(ctx, "Specify a topic to read.");
        return;
    }

    bool previous_override = session_translation_push_scope_override(ctx);

    bbs_listing_t *listings =
        (bbs_listing_t *)sshc_gc_calloc(SSH_CHATTER_BBS_MAX_POSTS,
                                          sizeof(bbs_listing_t));
    if (listings == nullptr) {
        session_send_system_line(ctx, "BBS storage is unavailable.");
        session_translation_pop_scope_override(ctx, previous_override);
        return;
    }
    size_t count = 0U;

    host_t *host = ctx->owner;
    if (!session_bbs_collect_listings(host, listings, &count)) {
        session_send_system_line(ctx, "BBS storage is unavailable.");
        sshc_gc_free(listings);
        session_translation_pop_scope_override(ctx, previous_override);
        return;
    }

    if (count == 0U) {
        session_send_system_line(ctx, "The bulletin board is empty.");
        sshc_gc_free(listings);
        session_translation_pop_scope_override(ctx, previous_override);
        return;
    }

    for (size_t outer = 1U; outer < count; ++outer) {
        bbs_listing_t key = listings[outer];
        size_t position = outer;
        while (position > 0U &&
               listings[position - 1U].bumped_at < key.bumped_at) {
            listings[position] = listings[position - 1U];
            --position;
        }
        listings[position] = key;
    }

    ctx->bbs_view_active = false;
    ctx->bbs_view_post_id = 0U;

    char section_label[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(section_label, sizeof(section_label), "BBS Topic: %s",
             working_topic);
    session_render_separator(ctx, section_label);

    bool found = false;
    for (size_t idx = 0U; idx < count; ++idx) {
        const bbs_listing_t *entry = &listings[idx];
        const char *entry_topic = (entry->tag_count > 0U)
                                      ? entry->tags[0]
                                      : SSH_CHATTER_BBS_DEFAULT_TAG;
        if (strcasecmp(entry_topic, working_topic) != 0) {
            continue;
        }

        char created_buffer[32];
        bbs_format_time(entry->bumped_at, created_buffer,
                        sizeof(created_buffer));

        char line[SSH_CHATTER_MESSAGE_LIMIT];
        int title_preview = (int)strnlen(entry->title, sizeof(entry->title));
        if (title_preview > 80) {
            title_preview = 80;
        }

        if (entry->tag_count <= 1U) {
            snprintf(line, sizeof(line), "#%" PRIu64 " [%s] %.*s", entry->id,
                     created_buffer, title_preview, entry->title);
        } else {
            char tag_buffer[SSH_CHATTER_MESSAGE_LIMIT];
            size_t buffer_offset = 0U;
            tag_buffer[0] = '\0';
            for (size_t tag_idx = 0U; tag_idx < entry->tag_count; ++tag_idx) {
                const char *tag_value = entry->tags[tag_idx];
                if (tag_value[0] == '\0') {
                    continue;
                }
                size_t len = strlen(tag_value);
                if (buffer_offset + len + 2U >= sizeof(tag_buffer)) {
                    break;
                }
                if (buffer_offset > 0U) {
                    tag_buffer[buffer_offset++] = ',';
                }
                memcpy(tag_buffer + buffer_offset, tag_value, len);
                buffer_offset += len;
                tag_buffer[buffer_offset] = '\0';
            }
            int tags_preview = (int)strnlen(tag_buffer, sizeof(tag_buffer));
            if (tags_preview > 80) {
                tags_preview = 80;
            }
            snprintf(line, sizeof(line), "#%" PRIu64 " [%s] %.*s|%.*s",
                     entry->id, created_buffer, title_preview, entry->title,
                     tags_preview, tag_buffer);
        }

        session_send_system_line(ctx, line);
        found = true;
    }

    if (!found) {
        char message[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(message, sizeof(message), "No posts found for topic '%s'.",
                 working_topic);
        session_send_system_line(ctx, message);
    }

    session_render_separator(ctx, "End");
    sshc_gc_free(listings);
    session_translation_pop_scope_override(ctx, previous_override);
}

static void session_bbs_search_posts(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (arguments == nullptr || arguments[0] == '\0') {
        session_send_system_line(ctx, "Usage: search <keyword>");
        return;
    }

    char keyword[128];
    snprintf(keyword, sizeof(keyword), "%s", arguments);
    trim_whitespace_inplace(keyword);
    if (keyword[0] == '\0') {
        session_send_system_line(ctx, "Usage: search <keyword>");
        return;
    }

    bool previous_override = session_translation_push_scope_override(ctx);
    bbs_listing_t *listings =
        (bbs_listing_t *)sshc_gc_calloc(SSH_CHATTER_BBS_MAX_POSTS,
                                          sizeof(bbs_listing_t));
    if (listings == nullptr) {
        session_send_system_line(ctx, "BBS storage is unavailable.");
        session_translation_pop_scope_override(ctx, previous_override);
        return;
    }
    size_t count = 0U;

    host_t *host = ctx->owner;
    if (!session_bbs_collect_listings(host, listings, &count)) {
        session_send_system_line(ctx, "BBS storage is unavailable.");
        sshc_gc_free(listings);
        session_translation_pop_scope_override(ctx, previous_override);
        return;
    }

    bbs_listing_t *results =
        (bbs_listing_t *)sshc_gc_calloc(SSH_CHATTER_BBS_MAX_POSTS,
                                          sizeof(bbs_listing_t));
    if (results == nullptr) {
        sshc_gc_free(listings);
        session_translation_pop_scope_override(ctx, previous_override);
        return;
    }
    size_t result_count = 0U;

    for (size_t idx = 0U; idx < count; ++idx) {
        bbs_post_t *post = (bbs_post_t *)sshc_gc_calloc(1U, sizeof(*post));
        if (post == nullptr) {
            continue;
        }

        if (session_bbs_load_post(host, listings[idx].id, post) && post->in_use) {
            bool match = false;
            if (strcasestr(post->title, keyword) != nullptr ||
                strcasestr(post->body, keyword) != nullptr ||
                strcasestr(post->author, keyword) != nullptr) {
                match = true;
            } else {
                for (size_t t = 0; t < post->tag_count; ++t) {
                    if (strcasecmp(post->tags[t], keyword) == 0) {
                        match = true;
                        break;
                    }
                }
            }

            if (match) {
                results[result_count++] = listings[idx];
            }
        }
        sshc_gc_free(post);
    }

    char separator_title[256];
    snprintf(separator_title, sizeof(separator_title), "Search Results for: '%s' (%zu found)", keyword, result_count);
    session_render_separator(ctx, separator_title);

    if (result_count == 0U) {
        session_send_system_line(ctx, "No posts found matching the keyword.");
    } else {
        for (size_t idx = 0U; idx < result_count; ++idx) {
            const bbs_listing_t *entry = &results[idx];
            char created_buffer[32];
            bbs_format_time(entry->bumped_at, created_buffer, sizeof(created_buffer));
            
            char line[SSH_CHATTER_MESSAGE_LIMIT];
            int title_preview = (int)strnlen(entry->title, sizeof(entry->title));
            if (title_preview > 80) {
                title_preview = 80;
            }

            int32_t score = entry->upvotes - entry->downvotes;
            char stats_buf[64];
            snprintf(stats_buf, sizeof(stats_buf), "\033[1;32mScore: %d\033[0m \033[1;36mComments: %zu\033[0m", score, entry->comment_count);

            if (entry->tag_count == 0U) {
                snprintf(line, sizeof(line), "%s #%" PRIu64 " [%s] %.*s | (no tags)",
                         stats_buf, entry->id, created_buffer, title_preview, entry->title);
            } else {
                char tag_buffer[SSH_CHATTER_MESSAGE_LIMIT];
                size_t buffer_offset = 0U;
                tag_buffer[0] = '\0';
                for (size_t tag = 0U; tag < entry->tag_count; ++tag) {
                    size_t len = strlen(entry->tags[tag]);
                    if (buffer_offset + len + 2U >= sizeof(tag_buffer)) {
                        break;
                    }
                    if (tag > 0U) {
                        tag_buffer[buffer_offset++] = ',';
                    }
                    memcpy(tag_buffer + buffer_offset, entry->tags[tag], len);
                    buffer_offset += len;
                    tag_buffer[buffer_offset] = '\0';
                }
                int tags_preview = (int)strnlen(tag_buffer, sizeof(tag_buffer));
                if (tags_preview > 80) {
                    tags_preview = 80;
                }
                snprintf(line, sizeof(line), "%s #%" PRIu64 " [%s] %.*s | %.*s",
                         stats_buf, entry->id, created_buffer, title_preview, entry->title,
                         tags_preview, tag_buffer);
            }
            session_send_system_line(ctx, line);
        }
    }

    session_render_separator(ctx, "End");
    sshc_gc_free(results);
    sshc_gc_free(listings);
    session_translation_pop_scope_override(ctx, previous_override);
}

// Display a single post to the user.
static void session_bbs_read(session_ctx_t *ctx, uint64_t id)
{
    if (ctx == nullptr || ctx->owner == nullptr || id == 0U) {
        return;
    }

    host_t *host = ctx->owner;
    bbs_post_t *snapshot = (bbs_post_t *)sshc_gc_calloc(1U, sizeof(*snapshot));
    if (snapshot == nullptr) {
        session_send_system_line(ctx, "BBS storage is unavailable.");
        return;
    }

    if (!session_bbs_load_post(host, id, snapshot)) {
        sshc_gc_free(snapshot);
        snapshot = nullptr;
        session_send_system_line(ctx, "No post exists with that identifier.");
        return;
    }

    if (!snapshot->in_use) {
        sshc_gc_free(snapshot);
        session_send_system_line(ctx, "BBS storage is unavailable.");
        return;
    }

    char loading_msg[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(loading_msg, sizeof(loading_msg), "\033[1;33m>>> Loading Post #%" PRIu64 "... Please wait. <<<\033[0m", id);
    session_send_system_line(ctx, loading_msg);

    session_bbs_render_post(ctx, snapshot, nullptr, true);
    sshc_gc_free(snapshot);
}

// Create a new post using the provided argument format.
static bool session_bbs_is_admin_only_tag(const char *tag)
{
    if (tag == nullptr || tag[0] == '\0') {
        return false;
    }

    if (strcasecmp(tag, "manual") == 0 || strcasecmp(tag, "notice") == 0) {
        return true;
    }

    if (strcmp(tag, "설명서") == 0 || strcmp(tag, "공지") == 0) {
        return true;
    }

    return false;
}

static void session_bbs_compact_preview(const char *input, char *output,
                                        size_t length)
{
    if (output == nullptr || length == 0U) {
        return;
    }
    output[0] = '\0';
    if (input == nullptr) {
        return;
    }

    size_t out_idx = 0U;
    bool last_space = true;
    bool truncated = false;
    const unsigned char *cursor = (const unsigned char *)input;

    while (*cursor != '\0') {
        unsigned char ch = *cursor++;
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            ch = ' ';
        }
        if (ch < 32U) {
            continue;
        }
        if (ch == ' ') {
            if (last_space) {
                continue;
            }
            last_space = true;
        } else {
            last_space = false;
        }

        if (out_idx + 1U >= length) {
            truncated = true;
            break;
        }

        output[out_idx++] = (char)ch;
    }

    if (last_space && out_idx > 0U) {
        --out_idx;
    }

    if (truncated && out_idx + 3U < length) {
        output[out_idx++] = '.';
        output[out_idx++] = '.';
        output[out_idx++] = '.';
    }

    output[out_idx] = '\0';
}

static void session_bbs_announce_post(host_t *host, const bbs_post_t *post)
{
    if (host == nullptr || post == nullptr) {
        return;
    }

    char author[SSH_CHATTER_USERNAME_LEN];
    snprintf(author, sizeof(author), "%s", post->author);
    trim_whitespace_inplace(author);

    char title[SSH_CHATTER_BBS_TITLE_LEN];
    snprintf(title, sizeof(title), "%s", post->title);
    trim_whitespace_inplace(title);

    char preview[128];
    session_bbs_compact_preview(post->body, preview, sizeof(preview));

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    if (preview[0] != '\0') {
        snprintf(notice, sizeof(notice), "* [bbs] #%llu %s posted \"%s\" --%s",
                 (unsigned long long)post->id,
                 author[0] != '\0' ? author : "unknown",
                 title[0] != '\0' ? title : "(untitled)", preview);
    } else {
        snprintf(notice, sizeof(notice), "* [bbs] #%llu %s posted \"%s\"",
                 (unsigned long long)post->id,
                 author[0] != '\0' ? author : "unknown",
                 title[0] != '\0' ? title : "(untitled)");
    }

    host_history_record_system(host, notice, nullptr);
}

static void session_bbs_announce_comment(host_t *host, const bbs_post_t *post,
                                         const bbs_comment_t *comment)
{
    if (host == nullptr || post == nullptr || comment == nullptr) {
        return;
    }

    char author[SSH_CHATTER_USERNAME_LEN];
    snprintf(author, sizeof(author), "%s", comment->author);
    trim_whitespace_inplace(author);

    char title[SSH_CHATTER_BBS_TITLE_LEN];
    snprintf(title, sizeof(title), "%s", post->title);
    trim_whitespace_inplace(title);

    char preview[128];
    session_bbs_compact_preview(comment->text, preview, sizeof(preview));

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    if (preview[0] != '\0') {
        snprintf(notice, sizeof(notice),
                 "* [bbs] #%llu %s commented on \"%s\": %s",
                 (unsigned long long)post->id,
                 author[0] != '\0' ? author : "unknown",
                 title[0] != '\0' ? title : "(untitled)", preview);
    } else {
        snprintf(notice, sizeof(notice), "* [bbs] #%llu %s commented on \"%s\"",
                 (unsigned long long)post->id,
                 author[0] != '\0' ? author : "unknown",
                 title[0] != '\0' ? title : "(untitled)");
    }

    host_history_record_system(host, notice, nullptr);
}

static void session_bbs_reset_pending_post(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    ctx->bbs_post_pending = false;
    ctx->editor_mode = SESSION_EDITOR_MODE_NONE;
    ctx->pending_bbs_edit_id = 0U;
    ctx->pending_bbs_body_length = 0U;
    ctx->pending_bbs_tag_count = 0U;
    ctx->pending_bbs_line_count = 0U;
    ctx->pending_bbs_cursor_line = 0U;
    ctx->pending_bbs_editing_line = false;
    ctx->bbs_editor_scroll_offset = 0U;
    ctx->bbs_editor_selection_start = 0U;
    ctx->bbs_editor_selection_start_set = false;
    ctx->bbs_editor_selection_end = 0U;
    ctx->bbs_editor_selection_end_set = false;
    if (ctx->pending_bbs_title != nullptr) {
        ctx->pending_bbs_title[0] = '\0';
    }
    if (ctx->pending_bbs_body != nullptr) {
        ctx->pending_bbs_body[0] = '\0';
    }
    if (ctx->pending_bbs_tags != nullptr) {
        memset(ctx->pending_bbs_tags, 0,
               sizeof(*ctx->pending_bbs_tags) * SSH_CHATTER_BBS_MAX_TAGS);
    }
    if (ctx->bbs_editor_clipboard != nullptr) {
        ctx->bbs_editor_clipboard[0] = '\0';
    }
    ctx->bbs_editor_clipboard_length = 0U;
    ctx->bbs_editor_clipboard_lines = 0U;
    ctx->bbs_line_edit_mode = false;
    ctx->bbs_line_edit_target = 0U;
    ctx->bbs_search_active = false;
    ctx->bbs_search_restore_line = 0U;
    ctx->bbs_search_restore_editing = false;
    ctx->bbs_search_restore_scroll = 0U;
    ctx->bbs_rendering_editor = false;
}

static void session_bbs_commit_pending_post(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    if (!ctx->bbs_post_pending) {
        return;
    }

    if (ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART) {
        session_asciiart_import_from_editor(ctx);
        if (ctx->asciiart_length == 0U) {
            session_asciiart_cancel(ctx, "ASCII art draft discarded.");
            session_bbs_reset_pending_post(ctx);
            return;
        }

        session_asciiart_commit(ctx);
        session_bbs_reset_pending_post(ctx);
        return;
    }

    if (ctx->pending_bbs_body_length == 0U) {
        session_send_system_line(ctx, "Post body was empty. Draft discarded.");
        session_bbs_reset_pending_post(ctx);
        return;
    }

    if (session_security_check_text(ctx, "BBS post", ctx->pending_bbs_body,
                                    ctx->pending_bbs_body_length,
                                    false) != HOST_SECURITY_SCAN_CLEAN) {
        session_bbs_reset_pending_post(ctx);
        return;
    }

    host_t *host = ctx->owner;
    if (host == nullptr) {
        session_bbs_reset_pending_post(ctx);
        return;
    }
    if (!session_bbs_ensure_live_storage(host)) {
        session_send_system_line(
            ctx, "The bulletin board is unavailable right now.");
        session_bbs_reset_pending_post(ctx);
        return;
    }

    ttak_mutex_lock(&host->lock);
    bbs_post_t snapshot = {0};
    if (ctx->editor_mode == SESSION_EDITOR_MODE_BBS_EDIT) {
        uint64_t edit_id = ctx->pending_bbs_edit_id;
        bbs_post_t *post = host_find_bbs_post_locked(host, edit_id);
        if (post == nullptr || !post->in_use) {
            ttak_mutex_unlock(&host->lock);
            session_send_system_line(
                ctx, "No post exists with that identifier anymore.");
            session_bbs_reset_pending_post(ctx);
            return;
        }

        bool can_edit = (strncmp(post->author, ctx->user.name,
                                 SSH_CHATTER_USERNAME_LEN) == 0) ||
                        ctx->user.is_operator || ctx->user.is_lan_operator;
        if (!can_edit) {
            ttak_mutex_unlock(&host->lock);
            session_send_system_line(
                ctx, "Only the author or an operator may edit this post.");
            session_bbs_reset_pending_post(ctx);
            return;
        }

        snprintf(post->title, sizeof(post->title), "%s",
                 ctx->pending_bbs_title);
        memcpy(post->body, ctx->pending_bbs_body, ctx->pending_bbs_body_length);
        post->body[ctx->pending_bbs_body_length] = '\0';
        host_strip_column_reset(post->title);
        host_strip_column_reset(post->body);
        post->tag_count = ctx->pending_bbs_tag_count;
        for (size_t idx = 0U; idx < post->tag_count; ++idx) {
            snprintf(post->tags[idx], sizeof(post->tags[idx]), "%s",
                     ctx->pending_bbs_tags[idx]);
            host_strip_column_reset(post->tags[idx]);
        }

        post->bumped_at = time(nullptr);
        snapshot = *post;
        host_bbs_state_save_locked(host);
        ttak_mutex_unlock(&host->lock);

        session_bbs_reset_pending_post(ctx);
        session_bbs_render_post(ctx, &snapshot, "Post updated.", false);
        return;
    }

    bbs_post_t *post = host_allocate_bbs_post_locked(host);
    if (post == nullptr) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "The bulletin board is full right now.");
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        time_t now = time(nullptr);
        const bbs_rate_limit_entry_t *limit =
            host_bbs_rate_limit_find_locked(host, ctx->user.name);
        if (limit != nullptr && limit->last_post_at != 0 &&
            now - limit->last_post_at < 30) {
            ttak_mutex_unlock(&host->lock);
            session_send_system_line(
                ctx, "You are posting too quickly. Try again in a moment.");
            session_bbs_reset_pending_post(ctx);
            return;
        }
    }

    post->board_id = (uint16_t)ctx->bbs_current_board_id;
    snprintf(post->author, sizeof(post->author), "%s", ctx->user.name);
    snprintf(post->title, sizeof(post->title), "%s", ctx->pending_bbs_title);
    memcpy(post->body, ctx->pending_bbs_body, ctx->pending_bbs_body_length);
    post->body[ctx->pending_bbs_body_length] = '\0';
    host_strip_column_reset(post->author);
    host_strip_column_reset(post->title);
    host_strip_column_reset(post->body);
    post->tag_count = ctx->pending_bbs_tag_count;
    for (size_t idx = 0U; idx < post->tag_count; ++idx) {
        snprintf(post->tags[idx], sizeof(post->tags[idx]), "%s",
                 ctx->pending_bbs_tags[idx]);
        host_strip_column_reset(post->tags[idx]);
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        bbs_rate_limit_entry_t *limit =
            host_bbs_rate_limit_ensure_locked(host, ctx->user.name);
        if (limit != nullptr) {
            limit->last_post_at = time(nullptr);
        }
    }

    // Mention notifications for the new post (title + body, post level).
    session_bbs_notify_mentions_locked(host, post->title, post->author,
                                       post->id, -1);
    session_bbs_notify_mentions_locked(host, post->body, post->author,
                                       post->id, -1);

    snapshot = *post;
    host_bbs_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);

    session_bbs_reset_pending_post(ctx);

    session_bbs_announce_post(ctx->owner, &snapshot);
    session_bbs_render_post(ctx, &snapshot, "Post created.", true);
}

static void session_bbs_select_avatar_interactive(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    session_send_system_line(ctx, "");
    session_send_system_line(ctx, "========================================");
    session_send_system_line(ctx, "  Choose your Profile Logo (Avatar)");
    session_send_system_line(ctx, "----------------------------------------");
    session_send_system_line(ctx, "  [1] Monitor");
    session_send_system_line(ctx, "  [2] Mouse");
    session_send_system_line(ctx, "  [3] Human");
    session_send_system_line(ctx, "  [4] Mushroom");
    session_send_system_line(ctx, "  [5] None / Keep Current");
    session_send_system_line(ctx, "----------------------------------------");
    session_send_system_line(ctx, "  Enter choice (1-5):");

    char ch = '\0';
    const int read_result = session_transport_read(ctx, &ch, 1, -1);
    if (read_result <= 0) {
        return;
    }
    session_local_echo_char(ctx, ch);
    session_local_echo_char(ctx, '\n');

    session_avatar_type_t chosen = AVATAR_NONE;
    bool change = false;
    if (ch == '1') {
        chosen = AVATAR_MONITOR;
        change = true;
    } else if (ch == '2') {
        chosen = AVATAR_MOUSE;
        change = true;
    } else if (ch == '3') {
        chosen = AVATAR_HUMAN;
        change = true;
    } else if (ch == '4') {
        chosen = AVATAR_MUSHROOM;
        change = true;
    } else if (ch == '5') {
        chosen = AVATAR_NONE;
        change = true;
    }

    if (change) {
        const char *root = (ctx->owner != nullptr) ? ctx->owner->user_data_root : "";
        user_data_record_t record = {0};
        if (!user_data_load(root, ctx->user.name, ctx->client_ip, &record)) {
            if (!user_data_init(&record, ctx->user.name, ctx->client_ip)) {
                session_send_system_line(ctx, "Failed to initialize user data.");
                return;
            }
        }

        if (chosen == AVATAR_NONE) {
            record.profile_picture[0] = '\0';
        } else {
            const char *art = kSessionAvatarArt[chosen];
            size_t len = strlen(art);
            if (len >= sizeof(record.profile_picture)) {
                len = sizeof(record.profile_picture) - 1;
            }
            memcpy(record.profile_picture, art, len);
            record.profile_picture[len] = '\0';
        }

        if (user_data_save(root, &record, ctx->client_ip)) {
            /* Keep the session record in sync so the logout-time save does
             * not overwrite (and delete) the picture we just stored. */
            snprintf(ctx->user_data.profile_picture,
                     sizeof(ctx->user_data.profile_picture), "%s",
                     record.profile_picture);
            session_send_system_line(
                ctx, chosen == AVATAR_NONE ? "Profile logo cleared." : "Profile logo updated.");
        } else {
            session_send_system_line(ctx, "Failed to save profile logo.");
        }
    }
}

static void session_bbs_begin_post(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr) {
        return;
    }

    if (ctx->bbs_post_pending) {
        const char *terminator = session_editor_terminator(ctx);
        char warning[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(warning, sizeof(warning),
                 "You are already composing a post. Finish it with %s.",
                 terminator);
        session_send_system_line(ctx, warning);
        return;
    }

    ctx->bbs_view_active = false;
    ctx->bbs_view_post_id = 0U;

    if (ctx->owner == nullptr) {
        session_send_system_line(
            ctx, "The bulletin board is unavailable right now.");
        return;
    }
    if (!session_bbs_ensure_live_storage(ctx->owner)) {
        session_send_system_line(
            ctx, "The bulletin board is unavailable right now.");
        return;
    }

    session_bbs_reset_pending_post(ctx);
    if (!session_bbs_workspace_acquire(ctx)) {
        session_send_system_line(ctx, "Unable to allocate editor workspace.");
        return;
    }

    if (arguments == nullptr) {
        session_bbs_send_usage(ctx, "post", "<title>[|tags...]");
        session_send_system_line(
            ctx, "Use | to separate tags when the title has spaces.");
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);
    if (working[0] == '\0') {
        session_bbs_send_usage(ctx, "post", "<title>[|tags...]");
        session_send_system_line(
            ctx, "Use | to separate tags when the title has spaces.");
        return;
    }

    char title[SSH_CHATTER_BBS_TITLE_LEN];
    title[0] = '\0';
    char *tag_cursor = nullptr;
    char *separator = strchr(working, '|');
    if (separator != nullptr) {
        *separator = '\0';
        char *title_part = working;
        char *tags_part = separator + 1;
        trim_whitespace_inplace(title_part);
        trim_whitespace_inplace(tags_part);
        size_t title_len = strnlen(title_part, sizeof(title));
        if (title_len > 1U &&
            (title_part[0] == '\"' || title_part[0] == '\'') &&
            title_part[title_len - 1U] == title_part[0]) {
            title_part[title_len - 1U] = '\0';
            ++title_part;
            trim_whitespace_inplace(title_part);
        }
        size_t copy_len = strnlen(title_part, sizeof(title) - 1U);
        memcpy(title, title_part, copy_len);
        title[copy_len] = '\0';
        tag_cursor = tags_part;
    } else {
        char *cursor = working;
        if (*cursor == '\"' || *cursor == '\'') {
            char quote = *cursor++;
            char *closing = strchr(cursor, quote);
            if (closing == nullptr) {
                session_send_system_line(
                    ctx, "Missing closing quote for the title.");
                return;
            }
            size_t copy_len = (size_t)(closing - cursor);
            if (copy_len >= sizeof(title)) {
                copy_len = sizeof(title) - 1U;
            }
            memcpy(title, cursor, copy_len);
            title[copy_len] = '\0';
            cursor = closing + 1;
        } else {
            char *space = cursor;
            while (*space != '\0' && !isspace((unsigned char)*space)) {
                ++space;
            }
            size_t copy_len = (size_t)(space - cursor);
            if (copy_len >= sizeof(title)) {
                copy_len = sizeof(title) - 1U;
            }
            memcpy(title, cursor, copy_len);
            title[copy_len] = '\0';
            cursor = space;
        }

        trim_whitespace_inplace(cursor);
        tag_cursor = cursor;
    }

    if (title[0] == '\0') {
        session_send_system_line(ctx, "A title is required to create a post.");
        return;
    }

    size_t tag_count = 0U;
    bool discarded_tags = false;
    bool default_tag_applied = false;
    while (tag_cursor != nullptr && *tag_cursor != '\0') {
        while (isspace((unsigned char)*tag_cursor)) {
            ++tag_cursor;
        }
        if (*tag_cursor == '\0') {
            break;
        }
        char *end = tag_cursor;
        while (*end != '\0' && !isspace((unsigned char)*end)) {
            ++end;
        }
        size_t length = (size_t)(end - tag_cursor);
        if (length > 0U) {
            if (tag_count < SSH_CHATTER_BBS_MAX_TAGS) {
                if (length >= SSH_CHATTER_BBS_TAG_LEN) {
                    length = SSH_CHATTER_BBS_TAG_LEN - 1U;
                }
                char tag_value[SSH_CHATTER_BBS_TAG_LEN];
                memcpy(tag_value, tag_cursor, length);
                tag_value[length] = '\0';
                if (!ctx->user.is_operator &&
                    session_bbs_is_admin_only_tag(tag_value)) {
                    char warning[SSH_CHATTER_MESSAGE_LIMIT];
                    snprintf(warning, sizeof(warning),
                             "The '%s' tag is reserved for administrators.",
                             tag_value);
                    session_send_system_line(ctx, warning);
                    return;
                }
                snprintf(ctx->pending_bbs_tags[tag_count],
                         SSH_CHATTER_BBS_TAG_LEN, "%s",
                         tag_value);
                ++tag_count;
            } else {
                discarded_tags = true;
            }
        }
        tag_cursor = end;
    }

    if (tag_count == 0U) {
        snprintf(ctx->pending_bbs_tags[0], SSH_CHATTER_BBS_TAG_LEN,
                 "%s", SSH_CHATTER_BBS_DEFAULT_TAG);
        tag_count = 1U;
        default_tag_applied = true;
    }

    snprintf(ctx->pending_bbs_title, SSH_CHATTER_BBS_TITLE_LEN, "%s", title);
    ctx->pending_bbs_tag_count = tag_count;
    ctx->pending_bbs_body[0] = '\0';
    ctx->pending_bbs_body_length = 0U;
    ctx->bbs_post_pending = true;
    ctx->editor_mode = SESSION_EDITOR_MODE_BBS_CREATE;
    ctx->pending_bbs_edit_id = 0U;

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    notice[0] = '\0';
    if (default_tag_applied) {
        snprintf(notice, sizeof(notice),
                 "No tags provided; default tag '%s' applied.",
                 SSH_CHATTER_BBS_DEFAULT_TAG);
    }
    if (discarded_tags) {
        if (notice[0] != '\0') {
            strncat(notice, "\n", sizeof(notice) - strlen(notice) - 1U);
        }
        strncat(notice,
                "Only the first four tags were kept. Extra tags were ignored.",
                sizeof(notice) - strlen(notice) - 1U);
    }

    session_bbs_select_avatar_interactive(ctx);
    session_bbs_render_editor(ctx, notice[0] != '\0' ? notice : nullptr);
}

static void session_bbs_capture_body_text(session_ctx_t *ctx, const char *text)
{
    if (ctx == nullptr || !ctx->bbs_post_pending || text == nullptr) {
        return;
    }

    session_capture_multiline_text(ctx, text, session_bbs_capture_body_line,
                                   session_bbs_capture_continue);
}

static void session_bbs_capture_body_line(session_ctx_t *ctx, const char *line)
{
    if (ctx == nullptr || !ctx->bbs_post_pending) {
        return;
    }

    char trimmed[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(trimmed, sizeof(trimmed), "%s", line != nullptr ? line : "");
    trim_whitespace_inplace(trimmed);
    if (session_editor_matches_terminator(ctx, trimmed)) {
        session_bbs_commit_pending_post(ctx);
        return;
    }

    if (line == nullptr) {
        line = "";
    }

    char status[SSH_CHATTER_MESSAGE_LIMIT];
    status[0] = '\0';

    session_bbs_recalculate_line_count(ctx);
    bool editing_line =
        ctx->pending_bbs_editing_line &&
        ctx->pending_bbs_cursor_line < ctx->pending_bbs_line_count;
    bool inserting_line =
        !editing_line &&
        ctx->pending_bbs_cursor_line < ctx->pending_bbs_line_count;

    bool updated = false;
    if (editing_line) {
        updated = session_bbs_replace_line(ctx, ctx->pending_bbs_cursor_line,
                                           line, status, sizeof(status));
        ctx->bbs_line_edit_mode = false;
        if (updated) {
            /* session_bbs_replace_line advanced the cursor to line_index+1.
             * If that position is still within the post, continue editing
             * there so the user can keep typing without extra keystrokes. */
            session_bbs_recalculate_line_count(ctx);
            const size_t next = ctx->pending_bbs_cursor_line;
            if (next < ctx->pending_bbs_line_count) {
                session_bbs_set_cursor(ctx, next, true);
            }
        }
    } else if (inserting_line) {
        updated = session_bbs_insert_line(ctx, ctx->pending_bbs_cursor_line,
                                          line, status, sizeof(status));
        if (updated) {
            session_bbs_set_cursor(ctx, ctx->pending_bbs_cursor_line + 1U,
                                   false);
        }
    } else {
        updated = session_bbs_append_line(ctx, line, status, sizeof(status));
    }

    if (!updated && status[0] == '\0') {
        snprintf(status, sizeof(status),
                 "Unable to update the draft right now.");
    }
    if (updated) {
        ctx->bbs_line_edit_mode = false;
    }

    session_bbs_render_editor(ctx, status[0] != '\0' ? status : nullptr);
}

static void session_bbs_begin_edit(session_ctx_t *ctx, uint64_t id)
{
    if (ctx == nullptr || id == 0U) {
        session_send_system_line(ctx, "Invalid post identifier.");
        return;
    }

    if (ctx->bbs_post_pending) {
        const char *terminator = session_editor_terminator(ctx);
        char warning[SSH_CHATTER_MESSAGE_LIMIT];
        if (ctx->editor_mode == SESSION_EDITOR_MODE_ASCIIART) {
            snprintf(warning, sizeof(warning),
                     "You are already composing ASCII art. Finish it with %s.",
                     terminator);
        } else {
            snprintf(warning, sizeof(warning),
                     "You are already composing a post. Finish it with %s.",
                     terminator);
        }
        session_send_system_line(ctx, warning);
        return;
    }

    if (ctx->owner == nullptr) {
        session_send_system_line(
            ctx, "The bulletin board is unavailable right now.");
        return;
    }

    host_t *host = ctx->owner;
    if (!session_bbs_ensure_live_storage(host)) {
        session_send_system_line(
            ctx, "The bulletin board is unavailable right now.");
        return;
    }
    ttak_mutex_lock(&host->lock);
    bbs_post_t *post = host_find_bbs_post_locked(host, id);
    bbs_post_t snapshot = {0};
    if (post != nullptr && post->in_use) {
        snapshot = *post;
    }
    ttak_mutex_unlock(&host->lock);

    if (post == nullptr || !snapshot.in_use) {
        session_send_system_line(ctx, "No post exists with that identifier.");
        return;
    }

    bool can_edit = (strncmp(snapshot.author, ctx->user.name,
                             SSH_CHATTER_USERNAME_LEN) == 0) ||
                    ctx->user.is_operator || ctx->user.is_lan_operator;
    if (!can_edit) {
        session_send_system_line(
            ctx, "Only the author or an operator may edit this post.");
        return;
    }

    session_bbs_reset_pending_post(ctx);
    if (!session_bbs_workspace_acquire(ctx)) {
        session_send_system_line(ctx, "Unable to allocate editor workspace.");
        return;
    }
    ctx->bbs_post_pending = true;
    ctx->editor_mode = SESSION_EDITOR_MODE_BBS_EDIT;
    ctx->pending_bbs_edit_id = id;

    snprintf(ctx->pending_bbs_title, SSH_CHATTER_BBS_TITLE_LEN, "%s",
             snapshot.title);

    size_t body_len =
        strnlen(snapshot.body, SSH_CHATTER_BBS_BODY_LEN - 1U);
    memcpy(ctx->pending_bbs_body, snapshot.body, body_len);
    ctx->pending_bbs_body[body_len] = '\0';
    ctx->pending_bbs_body_length = body_len;

    ctx->pending_bbs_tag_count = snapshot.tag_count;
    if (ctx->pending_bbs_tag_count > SSH_CHATTER_BBS_MAX_TAGS) {
        ctx->pending_bbs_tag_count = SSH_CHATTER_BBS_MAX_TAGS;
    }
    for (size_t idx = 0U; idx < ctx->pending_bbs_tag_count; ++idx) {
        snprintf(ctx->pending_bbs_tags[idx], SSH_CHATTER_BBS_TAG_LEN,
                 "%s", snapshot.tags[idx]);
    }

    char notice[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(notice, sizeof(notice),
             "Editing post #%" PRIu64 ". Finish with %s to save changes.", id,
             session_bbs_terminator(ctx));
    session_bbs_render_editor(ctx, notice);
}

// Append a comment to a post.
static void session_bbs_add_comment(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr || arguments == nullptr) {
        session_bbs_send_usage(ctx, "comment", "<id>|<text>");
        return;
    }

    char working[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(working, sizeof(working), "%s", arguments);
    trim_whitespace_inplace(working);
    if (working[0] == '\0') {
        session_bbs_send_usage(ctx, "comment", "<id>|<text>");
        return;
    }

    char *separator = strchr(working, '|');
    char *id_text = nullptr;
    char *comment_text = nullptr;
    uint64_t id = 0U;

    if (separator == nullptr) {
        if (ctx->bbs_view_active && ctx->bbs_view_post_id != 0U) {
            id = ctx->bbs_view_post_id;
            comment_text = working;
        } else {
            session_bbs_send_usage(ctx, "comment", "<id>|<text>");
            return;
        }
    } else {
        *separator = '\0';
        id_text = working;
        comment_text = separator + 1;
        trim_whitespace_inplace(id_text);
        trim_whitespace_inplace(comment_text);

        if (id_text[0] == '\0' || comment_text[0] == '\0') {
            session_bbs_send_usage(ctx, "comment", "<id>|<text>");
            return;
        }

        id = (uint64_t)strtoull(id_text, nullptr, 10);
    }

    if (id == 0U) {
        session_send_system_line(ctx, "Invalid post identifier.");
        return;
    }

    size_t comment_scan_length =
        strnlen(comment_text, SSH_CHATTER_BBS_COMMENT_LEN);
    if (session_security_check_text(ctx, "BBS comment", comment_text,
                                    comment_scan_length,
                                    false) != HOST_SECURITY_SCAN_CLEAN) {
        return;
    }

    host_t *host = ctx->owner;
    if (!session_bbs_ensure_live_storage(host)) {
        session_send_system_line(
            ctx, "The bulletin board is unavailable right now.");
        return;
    }
    ttak_mutex_lock(&host->lock);
    bbs_post_t *post = host_find_bbs_post_locked(host, id);
    if (post == nullptr || !post->in_use) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "No post exists with that identifier.");
        return;
    }
    if (post->comment_count >= SSH_CHATTER_BBS_MAX_COMMENTS) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx,
                                 "This post has reached the comment limit.");
        return;
    }

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        time_t now = time(nullptr);
        const bbs_rate_limit_entry_t *limit =
            host_bbs_rate_limit_find_locked(host, ctx->user.name);
        if (limit != nullptr && limit->last_comment_at != 0 &&
            now - limit->last_comment_at < 3) {
            ttak_mutex_unlock(&host->lock);
            session_send_system_line(
                ctx, "You are commenting too quickly. Try again in a moment.");
            return;
        }
    }

    size_t comment_index = post->comment_count;
    bbs_comment_t *comment = &post->comments[comment_index];
    post->comment_count++;
    snprintf(comment->author, sizeof(comment->author), "%s", ctx->user.name);
    size_t comment_len =
        strnlen(comment_text, SSH_CHATTER_BBS_COMMENT_LEN - 1U);
    memcpy(comment->text, comment_text, comment_len);
    comment->text[comment_len] = '\0';
    host_strip_column_reset(comment->author);
    host_strip_column_reset(comment->text);
    comment->created_at = time(nullptr);
    comment->edited_at = 0;
    post->bumped_at = comment->created_at;

    if (!ctx->user.is_operator && !ctx->user.is_lan_operator) {
        bbs_rate_limit_entry_t *limit =
            host_bbs_rate_limit_ensure_locked(host, ctx->user.name);
        if (limit != nullptr) {
            limit->last_comment_at = comment->created_at;
        }
    }

    // Notifications: reply to the post author, plus @mentions.
    if (post->author[0] != '\0' &&
        strncmp(post->author, comment->author, SSH_CHATTER_USERNAME_LEN) !=
            0) {
        host_bbs_notification_enqueue_locked(host, 0, post->author,
                                             comment->author, post->id,
                                             (int32_t)comment_index);
    }
    session_bbs_notify_mentions_locked(host, comment->text, comment->author,
                                       post->id, (int32_t)comment_index);

    bbs_post_t snapshot = *post;
    host_bbs_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);

    if (comment_index < snapshot.comment_count) {
        session_bbs_announce_comment(ctx->owner, &snapshot,
                                     &snapshot.comments[comment_index]);
    }
    session_bbs_render_post(ctx, &snapshot, "Comment added.", false);
}

static void session_bbs_delete(session_ctx_t *ctx, uint64_t id)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }

    if (id == 0U) {
        session_send_system_line(ctx, "Invalid post identifier.");
        return;
    }

    host_t *host = ctx->owner;
    if (!session_bbs_ensure_live_storage(host)) {
        session_send_system_line(
            ctx, "The bulletin board is unavailable right now.");
        return;
    }
    ttak_mutex_lock(&host->lock);
    bbs_post_t *post = nullptr;
    for (size_t idx = 0U; idx < host->bbs_post_capacity; ++idx) {
        if (host->bbs_posts[idx].id == id) {
            post = &host->bbs_posts[idx];
            break;
        }
    }
    if (post == nullptr) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "No post exists with that identifier.");
        return;
    }

    bool can_delete = (post->author[0] == '\0' ||
                       strncmp(post->author, ctx->user.name,
                               SSH_CHATTER_USERNAME_LEN) == 0) ||
                      ctx->user.is_operator || ctx->user.is_lan_operator;
    if (!can_delete) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(
            ctx, "Only the author or an operator may delete this post.");
        return;
    }

    post->in_use = false;
    post->id = 0U;
    host_clear_bbs_post_locked(host, post);
    host_bbs_notification_drop_for_post_locked(host, id);
    host_bbs_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);

    session_send_system_line(ctx, "Post deleted.");
}

// Bump a post to the top of the list by refreshing its activity time.
static void session_bbs_regen_post(session_ctx_t *ctx, uint64_t id)
{
    if (ctx == nullptr || ctx->owner == nullptr || id == 0U) {
        return;
    }

    host_t *host = ctx->owner;
    if (!session_bbs_ensure_live_storage(host)) {
        session_send_system_line(
            ctx, "The bulletin board is unavailable right now.");
        return;
    }
    ttak_mutex_lock(&host->lock);
    bbs_post_t *post = host_find_bbs_post_locked(host, id);
    if (post == nullptr || !post->in_use) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "No post exists with that identifier.");
        return;
    }

    post->bumped_at = time(nullptr);
    bbs_post_t snapshot = *post;
    host_bbs_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);

    session_bbs_render_post(ctx, &snapshot, "Post bumped to the top.", false);
}

/* ------------------------------------------------------------------ */
/* BBS v2 enhancements: boards, votes, profile, drafts                */
/* ------------------------------------------------------------------ */

void session_bbs_boards(session_ctx_t *ctx)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }
    host_t *host = ctx->owner;
    if (!session_bbs_ensure_live_storage(host)) {
        session_send_system_line(ctx, "BBS storage is unavailable.");
        return;
    }
    session_bbs_prepare_canvas(ctx);
    session_render_separator(ctx, "Boards");
    ttak_mutex_lock(&host->lock);
    for (size_t i = 0; i < host->bbs_board_count; ++i) {
        const bbs_board_t *b = &host->bbs_boards[i];
        size_t post_count = 0;
        for (size_t p = 0; p < host->bbs_post_capacity; ++p) {
            if (host->bbs_posts[p].in_use && host->bbs_posts[p].board_id == b->board_id) {
                ++post_count;
            }
        }
        char line[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(line, sizeof(line), "  [%s] %s — %s (%zu posts)%s",
                 b->is_notice ? "NOTICE" : "board",
                 b->name, b->description, post_count,
                 b->is_notice ? " [PINNED]" : "");
        session_send_system_line(ctx, line);
    }
    ttak_mutex_unlock(&host->lock);
}

void session_bbs_upvote(session_ctx_t *ctx, uint64_t post_id)
{
    if (ctx == nullptr || ctx->owner == nullptr || post_id == 0U) {
        return;
    }
    host_t *host = ctx->owner;
    if (!session_bbs_ensure_live_storage(host)) {
        session_send_system_line(ctx, "BBS storage is unavailable.");
        return;
    }
    ttak_mutex_lock(&host->lock);
    bbs_post_t *post = host_find_bbs_post_locked(host, post_id);
    if (post == nullptr || !post->in_use) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "No post exists with that identifier.");
        return;
    }
    /* check if already voted */
    bool already = false;
    for (size_t i = 0; i < host->bbs_vote_count; ++i) {
        bbs_vote_t *v = &host->bbs_votes[i];
        if (v->target_post_id == post_id && v->target_comment_idx == -1 &&
            strcmp(v->voter_username, ctx->user.name) == 0) {
            if (v->vote_type == 1) {
                ttak_mutex_unlock(&host->lock);
                session_send_system_line(ctx, "You already upvoted this post.");
                return;
            }
            post->downvotes -= 1;
            v->vote_type = 1;
            already = true;
            break;
        }
    }
    if (!already) {
        if (host->bbs_vote_count < host->bbs_vote_capacity) {
            bbs_vote_t *v = &host->bbs_votes[host->bbs_vote_count++];
            v->target_post_id = post_id;
            v->target_comment_idx = -1;
            v->vote_type = 1;
            v->created_at = time(nullptr);
            snprintf(v->voter_username, sizeof(v->voter_username), "%s", ctx->user.name);
        }
    }
    post->upvotes += 1;
    host_bbs_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);
    session_send_system_line(ctx, "Upvoted.");
}

void session_bbs_downvote(session_ctx_t *ctx, uint64_t post_id)
{
    if (ctx == nullptr || ctx->owner == nullptr || post_id == 0U) {
        return;
    }
    host_t *host = ctx->owner;
    if (!session_bbs_ensure_live_storage(host)) {
        session_send_system_line(ctx, "BBS storage is unavailable.");
        return;
    }
    ttak_mutex_lock(&host->lock);
    bbs_post_t *post = host_find_bbs_post_locked(host, post_id);
    if (post == nullptr || !post->in_use) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "No post exists with that identifier.");
        return;
    }
    bool already = false;
    for (size_t i = 0; i < host->bbs_vote_count; ++i) {
        bbs_vote_t *v = &host->bbs_votes[i];
        if (v->target_post_id == post_id && v->target_comment_idx == -1 &&
            strcmp(v->voter_username, ctx->user.name) == 0) {
            if (v->vote_type == -1) {
                ttak_mutex_unlock(&host->lock);
                session_send_system_line(ctx, "You already downvoted this post.");
                return;
            }
            post->upvotes -= 1;
            v->vote_type = -1;
            already = true;
            break;
        }
    }
    if (!already) {
        if (host->bbs_vote_count < host->bbs_vote_capacity) {
            bbs_vote_t *v = &host->bbs_votes[host->bbs_vote_count++];
            v->target_post_id = post_id;
            v->target_comment_idx = -1;
            v->vote_type = -1;
            v->created_at = time(nullptr);
            snprintf(v->voter_username, sizeof(v->voter_username), "%s", ctx->user.name);
        }
    }
    post->downvotes += 1;
    host_bbs_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);
    session_send_system_line(ctx, "Downvoted.");
}

void session_bbs_cmtvote(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr || arguments == nullptr || arguments[0] == '\0') {
        if (ctx != nullptr) {
            session_send_system_line(ctx, "Usage: /bbs cmtvote <post_id> <comment_idx> [up|down]");
        }
        return;
    }
    uint64_t post_id = 0;
    int comment_idx = 0;
    char vote_dir[8] = {0};

    char *endptr = nullptr;
    errno = 0;
    post_id = strtoull(arguments, &endptr, 10);
    if (errno != 0 || endptr == nullptr || endptr == arguments || *endptr != ' ') {
        session_send_system_line(ctx, "Usage: /bbs cmtvote <post_id> <comment_idx> [up|down]");
        return;
    }

    errno = 0;
    long comment_idx_long = strtol(endptr, &endptr, 10);
    if (errno != 0 || endptr == nullptr || *endptr != ' ' ||
        comment_idx_long < 0 || comment_idx_long > INT_MAX) {
        session_send_system_line(ctx, "Usage: /bbs cmtvote <post_id> <comment_idx> [up|down]");
        return;
    }
    comment_idx = (int)comment_idx_long;

    while (*endptr == ' ') ++endptr;
    if (endptr[0] == '\0' ||
        (strncasecmp(endptr, "up", 2) != 0 && strncasecmp(endptr, "down", 4) != 0)) {
        session_send_system_line(ctx, "Usage: /bbs cmtvote <post_id> <comment_idx> [up|down]");
        return;
    }
    snprintf(vote_dir, sizeof(vote_dir), "%s", endptr);
    int vote_type = (strcasecmp(vote_dir, "up") == 0) ? 1 : -1;
    host_t *host = ctx->owner;
    if (!session_bbs_ensure_live_storage(host)) {
        session_send_system_line(ctx, "BBS storage is unavailable.");
        return;
    }
    ttak_mutex_lock(&host->lock);
    bbs_post_t *post = host_find_bbs_post_locked(host, post_id);
    if (post == nullptr || !post->in_use || comment_idx < 0 ||
        (size_t)comment_idx >= post->comment_count) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "Invalid post or comment index.");
        return;
    }
    bool already = false;
    for (size_t i = 0; i < host->bbs_vote_count; ++i) {
        bbs_vote_t *v = &host->bbs_votes[i];
        if (v->target_post_id == post_id && v->target_comment_idx == comment_idx &&
            strcmp(v->voter_username, ctx->user.name) == 0) {
            if (v->vote_type == vote_type) {
                ttak_mutex_unlock(&host->lock);
                session_send_system_line(ctx, "You already voted this comment.");
                return;
            }
            if (v->vote_type == 1) post->comments[comment_idx].upvotes -= 1;
            else post->comments[comment_idx].downvotes -= 1;
            v->vote_type = (int8_t)vote_type;
            already = true;
            break;
        }
    }
    if (!already) {
        if (host->bbs_vote_count < host->bbs_vote_capacity) {
            bbs_vote_t *v = &host->bbs_votes[host->bbs_vote_count++];
            v->target_post_id = post_id;
            v->target_comment_idx = comment_idx;
            v->vote_type = (int8_t)vote_type;
            v->created_at = time(nullptr);
            snprintf(v->voter_username, sizeof(v->voter_username), "%s", ctx->user.name);
        }
    }
    if (vote_type == 1) post->comments[comment_idx].upvotes += 1;
    else post->comments[comment_idx].downvotes += 1;
    host_bbs_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);
    session_send_system_line(ctx, vote_type == 1 ? "Comment upvoted." : "Comment downvoted.");
}

// Edit the text of an existing comment on a post.
static void session_bbs_cmtedit(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr || arguments == nullptr ||
        arguments[0] == '\0') {
        session_bbs_send_usage(ctx, "cmtedit", "<id> <idx> <text>");
        return;
    }

    if (!atomic_load(&ctx->owner->bbs_comment_edit_enabled)) {
        session_send_system_line(ctx, "Comment editing is disabled.");
        return;
    }

    uint64_t post_id = 0U;
    int comment_idx = 0;

    char *endptr = nullptr;
    errno = 0;
    post_id = strtoull(arguments, &endptr, 10);
    if (errno != 0 || endptr == nullptr || endptr == arguments ||
        *endptr != ' ') {
        session_bbs_send_usage(ctx, "cmtedit", "<id> <idx> <text>");
        return;
    }

    errno = 0;
    long comment_idx_long = strtol(endptr, &endptr, 10);
    if (errno != 0 || endptr == nullptr || *endptr != ' ' ||
        comment_idx_long < 0 || comment_idx_long > INT_MAX) {
        session_bbs_send_usage(ctx, "cmtedit", "<id> <idx> <text>");
        return;
    }
    comment_idx = (int)comment_idx_long;

    while (*endptr == ' ') ++endptr;
    const char *new_text = endptr;
    if (new_text[0] == '\0') {
        session_bbs_send_usage(ctx, "cmtedit", "<id> <idx> <text>");
        return;
    }

    char trimmed[SSH_CHATTER_BBS_COMMENT_LEN];
    snprintf(trimmed, sizeof(trimmed), "%s", new_text);
    trim_whitespace_inplace(trimmed);
    if (trimmed[0] == '\0') {
        session_bbs_send_usage(ctx, "cmtedit", "<id> <idx> <text>");
        return;
    }

    size_t text_scan_length = strnlen(trimmed, SSH_CHATTER_BBS_COMMENT_LEN);
    if (session_security_check_text(ctx, "BBS comment", trimmed,
                                    text_scan_length,
                                    false) != HOST_SECURITY_SCAN_CLEAN) {
        return;
    }

    host_t *host = ctx->owner;
    if (!session_bbs_ensure_live_storage(host)) {
        session_send_system_line(ctx, "BBS storage is unavailable.");
        return;
    }
    ttak_mutex_lock(&host->lock);
    bbs_post_t *post = host_find_bbs_post_locked(host, post_id);
    if (post == nullptr || !post->in_use || comment_idx < 0 ||
        (size_t)comment_idx >= post->comment_count) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "Invalid post or comment index.");
        return;
    }

    bbs_comment_t *comment = &post->comments[comment_idx];
    bool can_edit = (comment->author[0] == '\0' ||
                     strncmp(comment->author, ctx->user.name,
                             SSH_CHATTER_USERNAME_LEN) == 0) ||
                    ctx->user.is_operator || ctx->user.is_lan_operator;
    if (!can_edit) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(
            ctx, "Only the author or an operator may edit this comment.");
        return;
    }

    snprintf(comment->text, sizeof(comment->text), "%s", trimmed);
    host_strip_column_reset(comment->text);
    comment->edited_at = time(nullptr);
    bbs_post_t snapshot = *post;
    host_bbs_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);

    session_bbs_render_post(ctx, &snapshot, "Comment updated.", false);
}

// Delete a comment from a post, fixing up comment vote indexes afterwards.
static void session_bbs_cmtdel(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr || arguments == nullptr ||
        arguments[0] == '\0') {
        session_bbs_send_usage(ctx, "cmtdel", "<id> <idx>");
        return;
    }

    if (!atomic_load(&ctx->owner->bbs_comment_delete_enabled)) {
        session_send_system_line(ctx, "Comment deletion is disabled.");
        return;
    }

    uint64_t post_id = 0U;
    int comment_idx = 0;

    char *endptr = nullptr;
    errno = 0;
    post_id = strtoull(arguments, &endptr, 10);
    if (errno != 0 || endptr == nullptr || endptr == arguments ||
        *endptr != ' ') {
        session_bbs_send_usage(ctx, "cmtdel", "<id> <idx>");
        return;
    }

    errno = 0;
    long comment_idx_long = strtol(endptr, &endptr, 10);
    if (errno != 0 || endptr == nullptr ||
        (endptr[0] != '\0' && *endptr != ' ') ||
        comment_idx_long < 0 || comment_idx_long > INT_MAX) {
        session_bbs_send_usage(ctx, "cmtdel", "<id> <idx>");
        return;
    }
    comment_idx = (int)comment_idx_long;

    host_t *host = ctx->owner;
    if (!session_bbs_ensure_live_storage(host)) {
        session_send_system_line(ctx, "BBS storage is unavailable.");
        return;
    }
    ttak_mutex_lock(&host->lock);
    bbs_post_t *post = host_find_bbs_post_locked(host, post_id);
    if (post == nullptr || !post->in_use || comment_idx < 0 ||
        (size_t)comment_idx >= post->comment_count) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "Invalid post or comment index.");
        return;
    }

    bbs_comment_t *comment = &post->comments[comment_idx];
    bool can_delete = (comment->author[0] == '\0' ||
                       strncmp(comment->author, ctx->user.name,
                               SSH_CHATTER_USERNAME_LEN) == 0) ||
                      ctx->user.is_operator || ctx->user.is_lan_operator;
    if (!can_delete) {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(
            ctx, "Only the author or an operator may delete this comment.");
        return;
    }

    // Shift the dense comment array left over the removed entry.
    size_t removed_idx = (size_t)comment_idx;
    if (removed_idx + 1U < post->comment_count) {
        memmove(&post->comments[removed_idx], &post->comments[removed_idx + 1U],
                (post->comment_count - removed_idx - 1U) *
                    sizeof(*post->comments));
    }
    post->comment_count--;

    // Fix up comment votes: drop votes on the removed comment and shift
    // indexes of later comments on the same post down by one.
    size_t vote_idx = 0U;
    while (vote_idx < host->bbs_vote_count) {
        bbs_vote_t *vote = &host->bbs_votes[vote_idx];
        if (vote->target_post_id == post_id) {
            if (vote->target_comment_idx == (int32_t)removed_idx) {
                if (vote_idx + 1U < host->bbs_vote_count) {
                    memmove(&host->bbs_votes[vote_idx],
                            &host->bbs_votes[vote_idx + 1U],
                            (host->bbs_vote_count - vote_idx - 1U) *
                                sizeof(*host->bbs_votes));
                }
                host->bbs_vote_count--;
                continue;
            }
            if (vote->target_comment_idx > (int32_t)removed_idx) {
                vote->target_comment_idx -= 1;
            }
        }
        vote_idx++;
    }

    bbs_post_t snapshot = *post;
    host_bbs_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);

    session_bbs_render_post(ctx, &snapshot, "Comment deleted.", false);
}

void session_bbs_profile(session_ctx_t *ctx, const char *username)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }
    const char *target = (username != nullptr && username[0] != '\0') ? username : ctx->user.name;
    session_bbs_prepare_canvas(ctx);
    session_render_separator(ctx, "User Profile");

    /* Load user data to get profile picture */
    user_data_record_t record = {0};
    bool has_data = false;
    const char *root = ctx->owner->user_data_root;
    if (user_data_load(root, target, "", &record)) {
        has_data = true;
    }

    if (has_data && record.profile_picture[0] != '\0') {
        /* Multi-line avatar art: split into per-line sends for proper CRLF. */
        session_send_raw_text(ctx, record.profile_picture);
    } else {
        session_send_system_line(ctx, "(No profile picture set)");
    }

    host_t *host = ctx->owner;
    size_t post_count = 0;
    size_t comment_count = 0;
    int32_t total_upvotes = 0;
    if (host != nullptr && host->bbs_posts != nullptr) {
        for (size_t i = 0; i < host->bbs_post_capacity; ++i) {
            if (!host->bbs_posts[i].in_use) continue;
            if (strcmp(host->bbs_posts[i].author, target) == 0) {
                ++post_count;
                total_upvotes += host->bbs_posts[i].upvotes;
            }
            for (size_t c = 0; c < host->bbs_posts[i].comment_count; ++c) {
                if (strcmp(host->bbs_posts[i].comments[c].author, target) == 0) {
                    ++comment_count;
                }
            }
        }
    }
    char stats[SSH_CHATTER_MESSAGE_LIMIT];
    snprintf(stats, sizeof(stats), "Posts: %zu | Comments: %zu | Total Upvotes: %d",
             post_count, comment_count, total_upvotes);
    session_send_system_line(ctx, stats);

    if (post_count > 0) {
        session_send_system_line(ctx, "Recent posts:");
        size_t shown = 0;
        for (size_t i = 0; i < host->bbs_post_capacity && shown < 5; ++i) {
            if (!host->bbs_posts[i].in_use) continue;
            if (strcmp(host->bbs_posts[i].author, target) == 0) {
                char line[SSH_CHATTER_MESSAGE_LIMIT];
                snprintf(line, sizeof(line), "  [#%zu] %s", (size_t)host->bbs_posts[i].id, host->bbs_posts[i].title);
                session_send_system_line(ctx, line);
                ++shown;
            }
        }
    }
}

void session_bbs_set_profile(session_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    if (!ctx->asciiart_pending || ctx->asciiart_buffer == nullptr ||
        ctx->asciiart_length == 0U) {
        session_send_system_line(ctx, "No ASCII art pending. Use /asciiart to create one first.");
        return;
    }
    const char *root = (ctx->owner != nullptr) ? ctx->owner->user_data_root : "";
    user_data_record_t record = {0};
    if (!user_data_load(root, ctx->user.name, ctx->client_ip, &record)) {
        if (!user_data_init(&record, ctx->user.name, ctx->client_ip)) {
            session_send_system_line(ctx, "Failed to initialize user data.");
            return;
        }
    }
    size_t copy_len = ctx->asciiart_length;
    if (copy_len >= sizeof(record.profile_picture)) {
        copy_len = sizeof(record.profile_picture) - 1;
    }
    memcpy(record.profile_picture, ctx->asciiart_buffer, copy_len);
    record.profile_picture[copy_len] = '\0';
    if (user_data_save(root, &record, ctx->client_ip)) {
        snprintf(ctx->user_data.profile_picture,
                 sizeof(ctx->user_data.profile_picture), "%s",
                 record.profile_picture);
        session_send_system_line(ctx, "Profile picture updated.");
    } else {
        session_send_system_line(ctx, "Failed to save profile picture.");
    }
}

void session_bbs_setavatar(session_ctx_t *ctx, const char *name)
{
    if (ctx == nullptr) {
        return;
    }
    if (name == nullptr || name[0] == '\0') {
        session_send_system_line(
            ctx, "Usage: /bbs setavatar <monitor|mouse|human|mushroom|none>");
        session_send_system_line(ctx, "Available avatars:");
        for (size_t i = 1; i < AVATAR_COUNT; ++i) {
            session_send_raw_text(ctx, kSessionAvatarArt[i]);
            char line[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(line, sizeof(line), "  -> %s", kSessionAvatarNames[i]);
            session_send_system_line(ctx, line);
        }
        return;
    }

    session_avatar_type_t chosen = AVATAR_NONE;
    for (size_t i = 0; i < AVATAR_COUNT; ++i) {
        if (strcmp(name, kSessionAvatarNames[i]) == 0) {
            chosen = (session_avatar_type_t)i;
            break;
        }
    }

    if (chosen == AVATAR_NONE && strcmp(name, "none") != 0 &&
        strcmp(name, "off") != 0) {
        session_send_system_line(ctx, "Unknown avatar name.");
        return;
    }

    const char *root = (ctx->owner != nullptr) ? ctx->owner->user_data_root : "";
    user_data_record_t record = {0};
    if (!user_data_load(root, ctx->user.name, ctx->client_ip, &record)) {
        if (!user_data_init(&record, ctx->user.name, ctx->client_ip)) {
            session_send_system_line(ctx, "Failed to initialize user data.");
            return;
        }
    }

    if (chosen == AVATAR_NONE) {
        record.profile_picture[0] = '\0';
    } else {
        const char *art = kSessionAvatarArt[chosen];
        size_t len = strlen(art);
        if (len >= sizeof(record.profile_picture)) {
            len = sizeof(record.profile_picture) - 1;
        }
        memcpy(record.profile_picture, art, len);
        record.profile_picture[len] = '\0';
    }

    if (user_data_save(root, &record, ctx->client_ip)) {
        snprintf(ctx->user_data.profile_picture,
                 sizeof(ctx->user_data.profile_picture), "%s",
                 record.profile_picture);
        session_send_system_line(
            ctx, chosen == AVATAR_NONE ? "Avatar removed." : "Avatar updated.");
    } else {
        session_send_system_line(ctx, "Failed to save avatar.");
    }
}

void session_bbs_draft(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }
    if (arguments == nullptr || arguments[0] == '\0') {
        session_send_system_line(ctx, "Usage: /bbs draft <save|list|load|delete>");
        return;
    }
    char action[16] = {0};
    const char *rest = nullptr;
    sscanf(arguments, "%15s", action);
    rest = arguments + strlen(action);
    while (*rest == ' ' || *rest == '\t') ++rest;

    host_t *host = ctx->owner;
    if (strcmp(action, "save") == 0) {
        if (!ctx->bbs_post_pending || ctx->pending_bbs_body == nullptr) {
            session_send_system_line(ctx, "Nothing to save. Start a post first.");
            return;
        }
        ttak_mutex_lock(&host->lock);
        bbs_draft_t *slot = nullptr;
        for (size_t i = 0; i < host->bbs_draft_capacity; ++i) {
            if (!host->bbs_drafts[i].in_use) {
                slot = &host->bbs_drafts[i];
                break;
            }
        }
        if (slot == nullptr) {
            ttak_mutex_unlock(&host->lock);
            session_send_system_line(ctx, "Draft storage full. Delete old drafts first.");
            return;
        }
        slot->in_use = true;
        slot->id = host->next_bbs_id++; /* reuse next_bbs_id for draft ids */
        slot->board_id = (uint16_t)ctx->bbs_current_board_id;
        snprintf(slot->author, sizeof(slot->author), "%s", ctx->user.name);
        snprintf(slot->title, sizeof(slot->title), "%s",
                 ctx->pending_bbs_title != nullptr ? ctx->pending_bbs_title : "");
        snprintf(slot->body, sizeof(slot->body), "%s", ctx->pending_bbs_body);
        slot->tag_count = ctx->pending_bbs_tag_count;
        for (size_t t = 0; t < ctx->pending_bbs_tag_count && t < SSH_CHATTER_BBS_MAX_TAGS; ++t) {
            snprintf(slot->tags[t], sizeof(slot->tags[t]), "%s", ctx->pending_bbs_tags[t]);
        }
        slot->created_at = time(nullptr);
        host->bbs_draft_count++;
        host_bbs_state_save_locked(host);
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "Draft saved.");
    } else if (strcmp(action, "list") == 0) {
        session_bbs_prepare_canvas(ctx);
        session_render_separator(ctx, "My Drafts");
        ttak_mutex_lock(&host->lock);
        size_t shown = 0;
        for (size_t i = 0; i < host->bbs_draft_capacity; ++i) {
            bbs_draft_t *d = &host->bbs_drafts[i];
            if (!d->in_use || strcmp(d->author, ctx->user.name) != 0) continue;
            char line[SSH_CHATTER_MESSAGE_LIMIT];
            snprintf(line, sizeof(line), "  [%zu] %s", i, d->title);
            session_send_system_line(ctx, line);
            ++shown;
        }
        ttak_mutex_unlock(&host->lock);
        if (shown == 0) {
            session_send_system_line(ctx, "No drafts.");
        }
    } else if (strcmp(action, "load") == 0) {
        size_t idx = 0;
        if (rest == nullptr || rest[0] == '\0' || sscanf(rest, "%zu", &idx) != 1) {
            session_send_system_line(ctx, "Usage: /bbs draft load <index>");
            return;
        }
        ttak_mutex_lock(&host->lock);
        if (idx >= host->bbs_draft_capacity || !host->bbs_drafts[idx].in_use ||
            strcmp(host->bbs_drafts[idx].author, ctx->user.name) != 0) {
            ttak_mutex_unlock(&host->lock);
            session_send_system_line(ctx, "Draft not found.");
            return;
        }
        bbs_draft_t *d = &host->bbs_drafts[idx];
        if (ctx->pending_bbs_title != nullptr) {
            sshc_gc_free(ctx->pending_bbs_title);
        }
        if (ctx->pending_bbs_body != nullptr) {
            sshc_gc_free(ctx->pending_bbs_body);
        }
        ctx->pending_bbs_title = sshc_strdup(d->title);
        ctx->pending_bbs_body = sshc_strdup(d->body);
        ctx->pending_bbs_body_length = strlen(d->body);
        ctx->pending_bbs_body_length = ctx->pending_bbs_body_length;
        ctx->pending_bbs_tag_count = d->tag_count;
        for (size_t t = 0; t < d->tag_count && t < SSH_CHATTER_BBS_MAX_TAGS; ++t) {
            snprintf(ctx->pending_bbs_tags[t], sizeof(ctx->pending_bbs_tags[t]), "%s", d->tags[t]);
        }
        ctx->bbs_post_pending = true;
        ctx->editor_mode = SESSION_EDITOR_MODE_BBS_CREATE;
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "Draft loaded. Continue editing with the post editor.");
    } else if (strcmp(action, "delete") == 0) {
        size_t idx = 0;
        if (rest == nullptr || rest[0] == '\0' || sscanf(rest, "%zu", &idx) != 1) {
            session_send_system_line(ctx, "Usage: /bbs draft delete <index>");
            return;
        }
        ttak_mutex_lock(&host->lock);
        if (idx >= host->bbs_draft_capacity || !host->bbs_drafts[idx].in_use ||
            strcmp(host->bbs_drafts[idx].author, ctx->user.name) != 0) {
            ttak_mutex_unlock(&host->lock);
            session_send_system_line(ctx, "Draft not found.");
            return;
        }
        host->bbs_drafts[idx].in_use = false;
        if (host->bbs_draft_count > 0) --host->bbs_draft_count;
        host_bbs_state_save_locked(host);
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "Draft deleted.");
    } else {
        session_send_system_line(ctx, "Usage: /bbs draft <save|list|load|delete>");
    }
}

void session_bbs_select_board(session_ctx_t *ctx, const char *arguments)
{
    if (ctx == nullptr || ctx->owner == nullptr) {
        return;
    }
    host_t *host = ctx->owner;
    if (!session_bbs_ensure_live_storage(host)) {
        session_send_system_line(ctx, "BBS storage is unavailable.");
        return;
    }

    if (arguments == nullptr || arguments[0] == '\0') {
        session_bbs_boards(ctx);
        return;
    }

    char target[64];
    snprintf(target, sizeof(target), "%s", arguments);
    trim_whitespace_inplace(target);

    // Try parsing as integer ID first
    char *endptr = nullptr;
    long long parsed_id = strtoll(target, &endptr, 10);
    bool is_id = (endptr != nullptr && *endptr == '\0');

    ttak_mutex_lock(&host->lock);
    const bbs_board_t *found = nullptr;
    for (size_t i = 0; i < host->bbs_board_count; ++i) {
        const bbs_board_t *b = &host->bbs_boards[i];
        if (is_id && b->board_id == (uint16_t)parsed_id) {
            found = b;
            break;
        }
        if (strcasecmp(b->name, target) == 0) {
            found = b;
            break;
        }
    }

    if (found != nullptr) {
        ctx->bbs_current_board_id = found->board_id;
        ttak_mutex_unlock(&host->lock);
        
        char msg[256];
        const char *fmt = session_bbs_localize(ctx->ui_language, "Joined board: %s — %s");
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
        snprintf(msg, sizeof(msg), fmt, found->name, found->description);
#pragma GCC diagnostic pop
        session_send_system_line(ctx, msg);
    } else {
        ttak_mutex_unlock(&host->lock);
        session_send_system_line(ctx, "Board not found.");
    }
}

#undef session_send_system_line
#undef session_bbs_render_editor
#undef session_bbs_render_post

/* AI members (ELIZA and the two small-talk personas) as room participants:
 * presence (join/part + DDial chat link), deciding when they answer, and a
 * detached reply thread so LLM calls never block the thread that received
 * the message.
 *
 * Reply policy: a member addressed by name (or by /pm) always answers; an
 * unaddressed message only occasionally draws a short remark from one of the
 * personas, which may also decline. ELIZA only speaks when addressed. */

#define HOST_AI_REPLY_QUEUE_MAX 16U
/* Unaddressed small talk is dropped once this many replies are waiting. */
#define HOST_AI_REPLY_AMBIENT_QUEUE_MAX 2U
#define HOST_AI_REPLY_AMBIENT_COOLDOWN_SECONDS 3.0

typedef enum host_ai_reply_kind {
    HOST_AI_REPLY_PUBLIC = 0,
    HOST_AI_REPLY_PRIVATE,
} host_ai_reply_kind_t;

struct host_ai_reply_job {
    struct host_ai_reply_job *next;
    host_ai_reply_kind_t kind;
    size_t member;
    bool addressed;
    /* Private replies from a DDial line go back to that line. */
    uint16_t ddial_from_slot;
    char username[SSH_CHATTER_USERNAME_LEN];
    char message[SSH_CHATTER_MESSAGE_LIMIT];
};

static const char *host_ai_member_name(const host_t *host, size_t member)
{
    switch (member) {
    case HOST_AI_MEMBER_ELIZA:
        return "eliza";
    case HOST_AI_MEMBER_PERSONA_A:
        return host_ai_persona_name(host, false);
    case HOST_AI_MEMBER_PERSONA_B:
    default:
        return host_ai_persona_name(host, true);
    }
}

/* Korean call name, used alongside the Latin one. */
static const char *host_ai_member_alias(const host_t *host, size_t member)
{
    switch (member) {
    case HOST_AI_MEMBER_ELIZA:
        return "엘리자";
    case HOST_AI_MEMBER_PERSONA_A:
        if (host->ai_persona_a_alias[0] != '\0') {
            return host->ai_persona_a_alias;
        }
        return host->ai_persona_a_name[0] == '\0' ? "카카" : nullptr;
    case HOST_AI_MEMBER_PERSONA_B:
    default:
        if (host->ai_persona_b_alias[0] != '\0') {
            return host->ai_persona_b_alias;
        }
        return host->ai_persona_b_name[0] == '\0' ? "다다" : nullptr;
    }
}

static bool host_ai_member_is_present(host_t *host, size_t member)
{
    if (member == HOST_AI_MEMBER_ELIZA) {
        return atomic_load(&host->eliza_enabled);
    }
    if (member >= HOST_AI_MEMBER_COUNT) {
        return false;
    }
    return atomic_load(
        &host->ai_persona_enabled[member - HOST_AI_MEMBER_PERSONA_A]);
}

/* Index of the AI member called name, or HOST_AI_MEMBER_COUNT. */
static size_t host_ai_member_lookup(const host_t *host, const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        return HOST_AI_MEMBER_COUNT;
    }
    for (size_t member = 0U; member < HOST_AI_MEMBER_COUNT; ++member) {
        const char *alias = host_ai_member_alias(host, member);
        if (strcasecmp(name, host_ai_member_name(host, member)) == 0 ||
            (alias != nullptr && strcmp(name, alias) == 0)) {
            return member;
        }
    }
    return HOST_AI_MEMBER_COUNT;
}

static void host_ai_member_presence(host_t *host, size_t member, bool joined)
{
    if (host == nullptr || member >= HOST_AI_MEMBER_COUNT) {
        return;
    }

    const char *name = host_ai_member_name(host, member);
    host_announce_presence(host, name, joined);

    /* Linked DDial stations see AI members on their own lines, exactly like
     * the room's human members. */
    if (joined) {
        if (host->ai_ddial_slots[member] == 0U) {
            host->ai_ddial_slots[member] =
                host_ddial_chat_link_add(host, name, name);
        }
    } else {
        host_ddial_chat_link_remove(host, host->ai_ddial_slots[member]);
        host->ai_ddial_slots[member] = 0U;
    }
}

/* Switch one AI member on or off: announces the join/part, holds or frees
 * its DDial line and saves the choice. Returns whether anything changed. */
static bool host_ai_member_set_present(host_t *host, size_t member,
                                       bool enabled)
{
    if (host == nullptr || member >= HOST_AI_MEMBER_COUNT) {
        return false;
    }
    if (member == HOST_AI_MEMBER_ELIZA) {
        return enabled ? host_eliza_enable(host) : host_eliza_disable(host);
    }

    bool was_enabled = atomic_exchange(
        &host->ai_persona_enabled[member - HOST_AI_MEMBER_PERSONA_A], enabled);
    if (was_enabled == enabled) {
        return false;
    }
    if (enabled) {
        struct timespec now = session_now_monotonic();
        host_ai_chat_update_last_reply(host, &now);
    }
    host_ai_member_presence(host, member, enabled);

    ttak_mutex_lock(&host->lock);
    host_eliza_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);
    return true;
}

/* Startup: bring in the personas the saved state left on (ELIZA restores
 * herself in host_eliza_state_load). */
static void host_ai_members_restore(host_t *host)
{
    for (size_t member = HOST_AI_MEMBER_PERSONA_A;
         member < HOST_AI_MEMBER_COUNT; ++member) {
        unsigned bit = 1U << (member - HOST_AI_MEMBER_PERSONA_A);
        if ((host->ai_persona_saved_off & bit) != 0U) {
            continue;
        }
        atomic_store(
            &host->ai_persona_enabled[member - HOST_AI_MEMBER_PERSONA_A], true);
        host_ai_member_presence(host, member, true);
    }

    /* ELIZA's restore may already have saved while the personas were still
     * off; write the real state back so the next start sees it. */
    ttak_mutex_lock(&host->lock);
    host_eliza_state_save_locked(host);
    ttak_mutex_unlock(&host->lock);
}

/* AI members currently present in the room, comma-joined into out (like
 * host_ddial_participants); returns how many there are. */
static size_t host_ai_participants(host_t *host, char *out, size_t cap)
{
    if (host == nullptr || out == nullptr || cap == 0U) {
        return 0U;
    }

    out[0] = '\0';
    size_t offset = 0U;
    size_t count = 0U;
    for (size_t member = 0U; member < HOST_AI_MEMBER_COUNT; ++member) {
        if (!host_ai_member_is_present(host, member)) {
            continue;
        }
        int written = snprintf(out + offset, cap - offset, "%s%s",
                               count == 0U ? "" : ", ",
                               host_ai_member_name(host, member));
        if (written > 0) {
            size_t advance = (size_t)written;
            offset += (advance < cap - offset) ? advance : cap - offset - 1U;
        }
        ++count;
    }
    return count;
}

/* Particles that may follow a name when someone calls it in Korean
 * ("카카야", "dada한테"); anything else glued on means a different word
 * ("카카오", "다다미"). */
static const char *const kHostAiCallSuffixes[] = {
    "야", "아", "님", "씨", "는", "은", "가", "이", "도", "한테",
    "에게", "랑", "의", "요",
};

static bool host_ai_byte_is_word(unsigned char ch)
{
    return isalnum(ch) || ch == '_' || ch >= 0x80U;
}

/* True when text addresses name as a word of its own ("kaka?", "@dada",
 * "카카야") rather than containing it inside another word. */
static bool host_ai_text_calls(const char *text, const char *name)
{
    if (text == nullptr || name == nullptr || name[0] == '\0') {
        return false;
    }

    const size_t name_len = strlen(name);
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (strncasecmp(cursor, name, name_len) != 0) {
            continue;
        }
        if (cursor > text &&
            host_ai_byte_is_word((unsigned char)cursor[-1])) {
            continue;
        }
        const char *after = cursor + name_len;
        if (!host_ai_byte_is_word((unsigned char)*after)) {
            return true;
        }
        for (size_t idx = 0U; idx < sizeof(kHostAiCallSuffixes) /
                                        sizeof(kHostAiCallSuffixes[0]);
             ++idx) {
            const char *suffix = kHostAiCallSuffixes[idx];
            size_t suffix_len = strlen(suffix);
            if (strncmp(after, suffix, suffix_len) == 0 &&
                !host_ai_byte_is_word((unsigned char)after[suffix_len])) {
                return true;
            }
        }
    }
    return false;
}

static bool host_ai_member_is_called(const host_t *host, size_t member,
                                     const char *text)
{
    const char *alias = host_ai_member_alias(host, member);
    return host_ai_text_calls(text, host_ai_member_name(host, member)) ||
           (alias != nullptr && host_ai_text_calls(text, alias));
}

/* Deterministic ~1-in-4 pick of a persona for unaddressed small talk, or
 * HOST_AI_MEMBER_COUNT to stay quiet. */
static size_t host_ai_pick_ambient_persona(const char *username,
                                           const char *message)
{
    uint32_t hash = 2166136261U;
    for (const unsigned char *cur = (const unsigned char *)username;
         *cur != '\0'; ++cur) {
        hash ^= (uint32_t)(*cur);
        hash *= 16777619U;
    }
    for (const unsigned char *cur = (const unsigned char *)message;
         *cur != '\0'; ++cur) {
        hash ^= (uint32_t)(*cur);
        hash *= 16777619U;
    }

    if ((hash % 4U) != 0U) {
        return HOST_AI_MEMBER_COUNT;
    }
    return ((hash % 2U) == 0U) ? HOST_AI_MEMBER_PERSONA_A
                               : HOST_AI_MEMBER_PERSONA_B;
}

static bool host_ai_reply_enqueue(host_t *host, host_ai_reply_kind_t kind,
                                  size_t member, bool addressed,
                                  const char *username, const char *message,
                                  uint16_t ddial_from_slot)
{
    host_ai_reply_state_t *state = &host->ai_reply;
    if (!state->initialized || !state->thread_started) {
        return false;
    }

    host_ai_reply_job_t *job =
        (host_ai_reply_job_t *)sshc_gc_calloc(1U, sizeof(*job));
    if (job == nullptr) {
        return false;
    }
    job->kind = kind;
    job->member = member;
    job->addressed = addressed;
    job->ddial_from_slot = ddial_from_slot;
    snprintf(job->username, sizeof(job->username), "%s", username);
    snprintf(job->message, sizeof(job->message), "%s", message);

    ttak_mutex_lock(&state->mutex);
    size_t limit =
        addressed ? HOST_AI_REPLY_QUEUE_MAX : HOST_AI_REPLY_AMBIENT_QUEUE_MAX;
    if (state->stop || state->pending >= limit) {
        ttak_mutex_unlock(&state->mutex);
        sshc_gc_free(job);
        return false;
    }
    if (state->tail == nullptr) {
        state->head = job;
    } else {
        state->tail->next = job;
    }
    state->tail = job;
    ++state->pending;
    ttak_cond_signal(&state->cond);
    ttak_mutex_unlock(&state->mutex);
    return true;
}

/* A public room message: queue a reply from every AI member it calls, or
 * occasionally an unprompted short one. Cheap; runs on the sender's thread. */
static void host_ai_route_public(host_t *host, const char *username,
                                 const char *message)
{
    if (host == nullptr || username == nullptr || message == nullptr ||
        message[0] == '\0' || message[0] == '/') {
        return;
    }
    /* Never answer another AI member (or ourselves). */
    if (host_ai_member_lookup(host, username) < HOST_AI_MEMBER_COUNT ||
        strcasecmp(username, "ai-eliza") == 0) {
        return;
    }

    bool any_called = false;
    for (size_t member = 0U; member < HOST_AI_MEMBER_COUNT; ++member) {
        if (!host_ai_member_is_present(host, member) ||
            !host_ai_member_is_called(host, member, message)) {
            continue;
        }
        any_called = true;
        (void)host_ai_reply_enqueue(host, HOST_AI_REPLY_PUBLIC, member, true,
                                    username, message, 0U);
    }
    if (any_called) {
        return;
    }

    size_t member = host_ai_pick_ambient_persona(username, message);
    if (member < HOST_AI_MEMBER_COUNT &&
        !host_ai_member_is_present(host, member)) {
        /* The picked persona is switched off: let the other one speak. */
        member = (member == HOST_AI_MEMBER_PERSONA_A)
                     ? HOST_AI_MEMBER_PERSONA_B
                     : HOST_AI_MEMBER_PERSONA_A;
    }
    if (member < HOST_AI_MEMBER_COUNT &&
        host_ai_member_is_present(host, member)) {
        (void)host_ai_reply_enqueue(host, HOST_AI_REPLY_PUBLIC, member, false,
                                    username, message, 0U);
    }
}

/* A /pm (or DDial /p#) to target: returns whether target is a present AI
 * member; *queued says whether the reply was accepted. */
static bool host_ai_route_private(host_t *host, const char *from,
                                  const char *target, const char *message,
                                  uint16_t ddial_from_slot, bool *queued)
{
    if (queued != nullptr) {
        *queued = false;
    }
    if (host == nullptr || from == nullptr || message == nullptr) {
        return false;
    }
    size_t member = host_ai_member_lookup(host, target);
    if (member >= HOST_AI_MEMBER_COUNT ||
        !host_ai_member_is_present(host, member)) {
        return false;
    }
    bool accepted =
        host_ai_reply_enqueue(host, HOST_AI_REPLY_PRIVATE, member, true, from,
                              message, ddial_from_slot);
    if (queued != nullptr) {
        *queued = accepted;
    }
    return true;
}

/* Decode one UTF-8 sequence; returns its length, 0 when malformed. */
static size_t host_ai_utf8_decode(const unsigned char *p, uint32_t *cp)
{
    if (p[0] < 0x80U) {
        *cp = p[0];
        return 1U;
    }
    size_t len;
    uint32_t value;
    if ((p[0] & 0xE0U) == 0xC0U) {
        len = 2U;
        value = p[0] & 0x1FU;
    } else if ((p[0] & 0xF0U) == 0xE0U) {
        len = 3U;
        value = p[0] & 0x0FU;
    } else if ((p[0] & 0xF8U) == 0xF0U) {
        len = 4U;
        value = p[0] & 0x07U;
    } else {
        return 0U;
    }
    for (size_t i = 1U; i < len; ++i) {
        if ((p[i] & 0xC0U) != 0x80U) {
            return 0U;
        }
        value = (value << 6U) | (p[i] & 0x3FU);
    }
    *cp = value;
    return len;
}

/* ASCII stand-in for a code point that often slips into English model
 * output: "" drops it (emoji, pictographs, joiners), nullptr means the
 * character is real non-English text and has no stand-in. */
static const char *host_ai_ascii_stand_in(uint32_t cp)
{
    switch (cp) {
    case 0x2018U: case 0x2019U: case 0x201AU: case 0x201BU: case 0x2032U:
        return "'";
    case 0x201CU: case 0x201DU: case 0x201EU: case 0x201FU: case 0x2033U:
        return "\"";
    case 0x2010U: case 0x2011U: case 0x2012U: case 0x2013U: case 0x2014U:
    case 0x2015U: case 0x2212U:
        return "-";
    case 0x2026U:
        return "...";
    case 0x00A0U: case 0x202FU: case 0x205FU:
        return " ";
    case 0x2022U: case 0x00B7U:
        return "*";
    case 0x00D7U:
        return "x";
    case 0x00A9U:
        return "(c)";
    case 0x00AEU:
        return "(R)";
    case 0x2122U:
        return "TM";
    case 0x2190U:
        return "<-";
    case 0x2192U:
        return "->";
    case 0x200BU: case 0x200CU: case 0x200DU: case 0x2060U: case 0xFE0EU:
    case 0xFE0FU: case 0xFEFFU:
        return "";
    default:
        break;
    }
    if (cp >= 0x2000U && cp <= 0x200AU) {
        return " ";
    }
    if ((cp >= 0x2190U && cp <= 0x21FFU) ||   /* arrows */
        (cp >= 0x2300U && cp <= 0x23FFU) ||   /* misc technical */
        (cp >= 0x25A0U && cp <= 0x27BFU) ||   /* shapes, symbols, dingbats */
        (cp >= 0x2B00U && cp <= 0x2BFFU) ||   /* misc symbols and arrows */
        (cp >= 0x1F000U && cp <= 0x1FAFFU) || /* emoji and pictographs */
        (cp >= 0xE0020U && cp <= 0xE007FU)) { /* emoji tag sequences */
        return "";
    }
    return nullptr;
}

/* Replies go to DDial too, which only takes plain ASCII lines.  Fold the
 * typographic punctuation and emoji an English reply tends to carry so it
 * still gets through.  A reply with real non-English text is left alone:
 * those were never going to reach DDial. */
static void host_ai_reply_fold_ascii(char *reply, size_t reply_len)
{
    char folded[SSH_CHATTER_MESSAGE_LIMIT];
    size_t out = 0U;
    const unsigned char *p = (const unsigned char *)reply;
    while (*p != '\0') {
        uint32_t cp = 0U;
        size_t len = host_ai_utf8_decode(p, &cp);
        if (len == 0U) {
            return;
        }
        const char *piece;
        char single[2] = {(char)cp, '\0'};
        if (cp < 0x80U) {
            piece = single;
        } else {
            piece = host_ai_ascii_stand_in(cp);
            if (piece == nullptr) {
                return;
            }
        }
        for (const char *q = piece; *q != '\0'; ++q) {
            /* Dropped emoji leave their surrounding spaces doubled. */
            if (*q == ' ' && out > 0U && folded[out - 1U] == ' ') {
                continue;
            }
            if (out + 1U >= sizeof(folded)) {
                return;
            }
            folded[out++] = *q;
        }
        p += len;
    }
    folded[out] = '\0';
    trim_whitespace_inplace(folded);
    if (folded[0] != '\0') {
        snprintf(reply, reply_len, "%s", folded);
    }
}

static bool host_ai_persona_reply(host_t *host, const host_ai_reply_job_t *job,
                                  char *reply, size_t reply_len)
{
    struct timespec now = session_now_monotonic();
    struct timespec last_reply = {0, 0};
    char model[64];
    bool use_gemini = false;
    host_ai_chat_snapshot_state(host, model, sizeof(model), &use_gemini,
                                &last_reply);

    if (!job->addressed && session_timespec_elapsed_seconds(
                               &now, &last_reply) <
                               HOST_AI_REPLY_AMBIENT_COOLDOWN_SECONDS) {
        return false;
    }

    char username_snippet[SSH_CHATTER_USERNAME_LEN];
    char message_snippet[SSH_CHATTER_AI_PROMPT_MESSAGE_MAX];
    host_ai_chat_copy_limited(username_snippet, sizeof(username_snippet),
                              job->username,
                              SSH_CHATTER_AI_PROMPT_USERNAME_MAX);
    host_ai_chat_copy_limited(message_snippet, sizeof(message_snippet),
                              job->message,
                              SSH_CHATTER_AI_PROMPT_MESSAGE_MAX - 1U);

    host_ai_chat_memory_restore(host);
    char context[SSH_CHATTER_AI_MEMORY_CONTEXT_BUFFER];
    size_t context_matches = host_ai_chat_memory_collect_context(
        host, job->message, context, sizeof(context));

    const char *name = host_ai_member_name(host, job->member);
    char tone[256];
    if (job->member == HOST_AI_MEMBER_PERSONA_A) {
        snprintf(tone, sizeof(tone),
                 "Respond as %s, a slightly cheerful and playful chat "
                 "participant.",
                 name);
    } else {
        snprintf(tone, sizeof(tone),
                 "Respond as %s, a calm-but-absurd jokester who sounds a "
                 "little childish.",
                 name);
    }
    const char *language =
        host_ai_chat_message_looks_korean(job->message)
            ? "The user is speaking Korean. Reply in Korean."
            : "The user is speaking English. Reply in English, in plain "
              "ASCII only: no emoji, curly quotes or special dashes.";

    char length_rule[256];
    if (job->kind == HOST_AI_REPLY_PRIVATE) {
        snprintf(length_rule, sizeof(length_rule),
                 "This is a private message to you; answer it directly in at "
                 "most three sentences.");
    } else if (job->addressed) {
        snprintf(length_rule, sizeof(length_rule),
                 "They are talking to you; answer them in at most three "
                 "sentences.");
    } else {
        snprintf(length_rule, sizeof(length_rule),
                 "They are not talking to you. Only chime in with one brief "
                 "sentence if you have something light to add; otherwise "
                 "output exactly: %s",
                 host_ai_chat_skip_token());
    }

    char context_block[SSH_CHATTER_AI_PROMPT_CONTEXT_MAX + 32U];
    context_block[0] = '\0';
    if (context_matches > 0U && context[0] != '\0') {
        char context_snippet[SSH_CHATTER_AI_PROMPT_CONTEXT_MAX];
        host_ai_chat_copy_limited(context_snippet, sizeof(context_snippet),
                                  context,
                                  SSH_CHATTER_AI_PROMPT_CONTEXT_MAX - 1U);
        snprintf(context_block, sizeof(context_block),
                 "Memory context:\n%s\n\n", context_snippet);
    }

    char prompt[SSH_CHATTER_MESSAGE_LIMIT * 2U];
    snprintf(prompt, sizeof(prompt),
             "%sUser %s says: %s\n%s %s %s Keep it natural and avoid "
             "moderation or BBS topics.",
             context_block, username_snippet, message_snippet, tone, language,
             length_rule);

    bool success =
        use_gemini
            ? translator_gemini_smalltalk(prompt, model, reply, reply_len)
            : translator_ollama_smalltalk(prompt, model, reply, reply_len);
    if (!success) {
        const char *error = translator_last_error();
        printf("[ai-chat] small-talk request failed%s%s\n",
               (error != nullptr && error[0] != '\0') ? ": " : ".",
               (error != nullptr) ? error : "");
        return false;
    }

    trim_whitespace_inplace(reply);
    if (reply[0] == '\0') {
        return false;
    }
    if (host_ai_chat_reply_is_skip_token(reply)) {
        /* Honour a decline only when nobody asked this member anything. */
        if (!job->addressed) {
            return false;
        }
        snprintf(reply, reply_len, "%s",
                 host_ai_chat_message_looks_korean(job->message) ? "응?"
                                                                  : "Hm?");
    }

    host_ai_chat_memory_store(host, job->username, job->message, reply);
    host_ai_chat_update_last_reply(host, &now);
    return true;
}

static void host_ai_deliver_private(host_t *host, const host_ai_reply_job_t *job,
                                    const char *reply)
{
    const char *name = host_ai_member_name(host, job->member);

    if (job->ddial_from_slot != 0U) {
        uint16_t own_slot = host->ai_ddial_slots[job->member];
        if (own_slot == 0U) {
            return;
        }
        if (!host_ddial_send_private(host, job->ddial_from_slot, own_slot,
                                     name, reply)) {
            host_ddial_client_send_private(host, job->ddial_from_slot,
                                           own_slot, name, reply);
        }
        return;
    }

    session_ctx_t *target = chat_room_find_user_ref(&host->room, job->username);
    if (target == nullptr) {
        return;
    }
    char label[SSH_CHATTER_MESSAGE_LIMIT];
    session_command_snprintf(target, label, sizeof(label), "%s -> you", name);
    session_send_private_message_line(target, target, label, reply);
    chat_room_release_user_ref(target);
}

static void host_ai_reply_run(host_t *host, const host_ai_reply_job_t *job)
{
    if (!host_ai_member_is_present(host, job->member)) {
        return;
    }

    char reply[SSH_CHATTER_MESSAGE_LIMIT];
    reply[0] = '\0';
    if (job->member == HOST_AI_MEMBER_ELIZA) {
        host_eliza_prepare_private_reply(job->message, reply, sizeof(reply));
    } else if (!host_ai_persona_reply(host, job, reply, sizeof(reply))) {
        return;
    }
    host_ai_reply_fold_ascii(reply, sizeof(reply));
    if (reply[0] == '\0') {
        return;
    }

    if (job->kind == HOST_AI_REPLY_PRIVATE) {
        host_ai_deliver_private(host, job, reply);
    } else {
        (void)host_post_client_message(host,
                                       host_ai_member_name(host, job->member),
                                       reply, nullptr, nullptr, false);
    }
}

static void *host_ai_reply_thread(void *arg)
{
    host_t *host = (host_t *)arg;
    if (host == nullptr) {
        return nullptr;
    }

    sshc_epoch_thread_enter();
    pthread_detach(pthread_self());
    sshc_memory_context_t *memory_scope =
        sshc_memory_context_push(host->memory_context);

    host_ai_reply_state_t *state = &host->ai_reply;
    while (true) {
        ttak_mutex_lock(&state->mutex);
        while (!state->stop && state->head == nullptr) {
            ttak_cond_wait(&state->cond, &state->mutex);
        }
        if (state->stop) {
            host_ai_reply_job_t *pending = state->head;
            state->head = nullptr;
            state->tail = nullptr;
            state->pending = 0U;
            ttak_mutex_unlock(&state->mutex);
            while (pending != nullptr) {
                host_ai_reply_job_t *next = pending->next;
                sshc_gc_free(pending);
                pending = next;
            }
            break;
        }
        host_ai_reply_job_t *job = state->head;
        state->head = job->next;
        if (state->head == nullptr) {
            state->tail = nullptr;
        }
        --state->pending;
        ttak_mutex_unlock(&state->mutex);

        host_ai_reply_run(host, job);
        sshc_gc_free(job);
    }

    sshc_memory_context_pop(memory_scope);
    sshc_epoch_thread_exit();
    return nullptr;
}

static bool host_ai_reply_init(host_t *host)
{
    if (host == nullptr) {
        return false;
    }

    host_ai_reply_state_t *state = &host->ai_reply;
    memset(state, 0, sizeof(*state));
    memset(host->ai_ddial_slots, 0, sizeof(host->ai_ddial_slots));

    if (ttak_mutex_init(&state->mutex) != 0) {
        return false;
    }
    if (ttak_cond_init(&state->cond) != 0) {
        ttak_mutex_destroy(&state->mutex);
        return false;
    }
    state->initialized = true;

    if (pthread_create(&state->thread, nullptr, host_ai_reply_thread, host) !=
        0) {
        ttak_cond_destroy(&state->cond);
        ttak_mutex_destroy(&state->mutex);
        state->initialized = false;
        return false;
    }
    state->thread_started = true;
    return true;
}

static void host_ai_reply_shutdown(host_t *host)
{
    if (host == nullptr || !host->ai_reply.initialized) {
        return;
    }

    /* Same detached + epoch model as the ELIZA/moderation workers: signal
     * only; the thread drops the queue, and the embedded mutex/cond are
     * reclaimed with host_t by epoch GC. */
    host_ai_reply_state_t *state = &host->ai_reply;
    ttak_mutex_lock(&state->mutex);
    state->stop = true;
    ttak_cond_broadcast(&state->cond);
    ttak_mutex_unlock(&state->mutex);
}

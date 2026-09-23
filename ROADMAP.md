# SSH-Chatter Roadmap

## Shipped — v1.2 (2026-09-22)

- Comment edit/delete (`cmtedit`/`cmtdel`) with author/operator checks and feature flags
- Edited-comment marker (storage format v1→v2 with legacy read path)
- `list` pagination (20/page), post capacity 128 → 256
- Anti-flood rate limiting (3s comments / 30s posts, per-user, operator bypass)
- Reply notifications + `@mention` (persistent sidecar, drained at dashboard)
- Read tracking ("N new posts since your last visit")
- Comment quoting (`:N` renders an excerpt of comment N)
- Hourly rotating backups of `bbs_state.dat` (newest 7 kept)
- Full 8-language localization (EN/KO/JP/ZH/RU/DE/FR/PL): messages, help tables, subcommand aliases
- Legacy codepage conversion fixes: lossless CP949/CP932/CP936 state machine, strict UTF-8 detection, GB18030/SJIS/ISO-2022 iconv fallbacks

---

## v1.3 — Moderation & Operator Tools

Goal: the BBS can be operated by a small crew without reading every post.

- `report <post_id>` — users file reports into an operator queue (persistent sidecar, capped, drop-oldest)
- Operator actions on queued/targeted posts: **hide** (soft-hide from listings, visible to operators) and **pin** (keep on top of the board; doubles as the announcement mechanism)
- User moderation: **mute** (blocks post/comment for that user, lifts after a duration or manually)
- All actions logged with actor, target, and timestamp

## v1.3 — IP Audit Log with Automatic Expiry

Goal: give operators just enough signal to fight abuse, with privacy destruction built in.

- On session connect, record `{username, ip, connected_at, disconnected_at}` in a capped, append-only audit log (in-RAM ring + short-lived on-disk sidecar)
- Operator-only command: look up the IPs a user connected from (and vice versa) for abuse investigation
- **Expiry:** every entry is destroyed 5 days after its session ends — enforced at write time and by a periodic sweep, not just "hidden from queries". No backups of the log are kept.
- Access is itself audit-logged (who queried what, when); LAN operators follow the same rule
- Documented in the MOTD/admin docs: what is stored, why, and when it disappears

## v1.4 — Nested Reply Threading

Goal: conversations, not flat comment lists.

- `parent_idx` on comments (points at another comment in the same post), set via `comment <id>|<text>` extension (`:reply N` syntax or a `cmtreply` subcommand)
- Render: indented thread tree under each comment (ANSI-safe, depth-capped)
- Notifications reuse the v1.2 sidecar: replies notify the parent comment's author; mention scanning covers reply text
- Vote fixup in `cmtdel` extended to re-parent or block deletion of comments that have children

## v1.4 — ANSI Art Board

Goal: lean into the retro-terminal identity; give Syncterm/NetRunner users a native home.

- New board type that stores posts as ANSI art via the existing full-screen ASCII-art editor pipeline
- Render with 256-color/ANSI passthrough preserved end-to-end (no column stripping, no translation)
- `@` / sauce-style metadata optional; plain `.ans` upload path for operators
- Rate-limit exempt for operators; regular users follow the standard post throttle

## v1.4 — Stability & Operations Hardening

Goal: the service survives real-world traffic without babysitting.

- Root-cause the crash paths seen in production core dumps; fix and add regression coverage
- Routine `valgrind` + random-nick memory stress in CI/script form (`scripts/run_valgrind_check.sh` made part of the release checklist)
- Watchdog self-recovery review: BBS state corruption must never take the listener down
- Admin **export/import**: dump/restore BBS state (posts, comments, votes, boards) to a portable file — insurance for migrations and the backup-rotation story

---

Guiding principles: storage-format changes stay backward-readable (v1→v2 precedent); every user-visible string ships with all 8 localizations from day one; operator powers are always gated and always logged.

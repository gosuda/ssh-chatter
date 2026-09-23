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

## Shipped — v1.3 (2026-09-23)

- Moderation pipeline: `report` queue (`reports.dat`, dedup, operator `reports`), post `hide`/`pin` flags (storage v3, layout-compatible with v2), `mute`/`unmute`/`mutes` enforced before rate limits, `modlog` action audit (operator actions and IP lookups)
- IP audit log: connect/disconnect records with operator-only `ipaudit` lookup (queries themselves logged), entries destroyed when older than 5 days, excluded from backups
- 11 new subcommands with 8-language aliases; list/search views show post author; tip blocks cover voting and comment editing

---

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

## v1.4 — FidoNet Connectivity (long-term)

Goal: join the surviving store-and-forward network for real retro credibility — but only after the moderation pipeline is proven, because echomail imports external content.

- **Phase 1 — minimal tosser**: BinkP listener (port 24554) + inbound spool; import one or two low-volume echoes into dedicated boards. Type 2+ packet parsing, MSGID dupe cache (sidecar pattern), SEEN-BY/PATH merge, FTS-5003 charset handling via the existing codepage layer
- **Phase 2 — outbound**: export our posts as echomail (origin/tear lines, proper MSGID generation), netmail support, bundle handling
- **Phase 3**: full node operation — node address, areafix, bounce/crash handling
- FidoNet message bases stay in per-area append-only bases, separate from `bbs_state.dat` (never mixed with the full-rewrite state model); the tosser runs on the existing watchdog/scheduler thread pattern
- Hard requirement: v1.3 moderation (hide/mute) must be able to act on imported mail; a half-behaving node floods peers with dupes and gets delisted by coordinators, so each phase ships complete or not at all

## v1.4 — Stability & Operations Hardening

Goal: the service survives real-world traffic without babysitting.

- Root-cause the crash paths seen in production core dumps; fix and add regression coverage
- Routine `valgrind` + random-nick memory stress in CI/script form (`scripts/run_valgrind_check.sh` made part of the release checklist)
- Watchdog self-recovery review: BBS state corruption must never take the listener down
- Admin **export/import**: dump/restore BBS state (posts, comments, votes, boards) to a portable file — insurance for migrations and the backup-rotation story

---

Guiding principles: storage-format changes stay backward-readable (v1→v2 precedent); every user-visible string ships with all 8 localizations from day one; operator powers are always gated and always logged.

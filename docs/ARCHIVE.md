# ssh-chatter Chat Archive

## Overview

By default, ssh-chatter keeps the most recent **32,768** chat messages in memory and the state file. When this limit is reached, the oldest messages are discarded.

If you enable archiving, messages that rotate out of the live history are written to date-stamped archive files instead of being deleted.

## Enabling Archiving

Set the environment variable before starting the server:

```bash
export SSH_CHATTER_USE_ARCHIVE=yes
./ssh-chatter
```

Accepted values (case-insensitive): `yes`, `true`, `1`.

## Archive File Layout

Archived messages are stored in binary files next to the main state file (or under `CHATTER_STATE_DIR` / `CHATTER_ARCHIVE_DIR` if set):

```
chat_archive_YYYY-MM-DD.dat
```

The date is **UTC**. For example, a message created on `2026-03-21` in UTC is stored in `chat_archive_2026-03-21.dat`.

## Commands

### List available archives

```text
/archive list
```

Shows all `chat_archive_*.dat` files in the archive directory, sorted by date.

### Enter archive browsing mode

```text
/archive enter 2026-03-21
```

Opens the archive for the given UTC date and displays the stored messages. While in archive mode:

- Live chat messages are **not** rendered on your screen.
- DDial bridge traffic is also suppressed.
- You cannot send chat messages.

### Leave archive browsing mode

```text
/archive exit
```

Returns to normal chat mode and synchronizes any pending messages.

## Disabling Archiving

If `SSH_CHATTER_USE_ARCHIVE` is not set, messages that exceed the 32,768 limit are simply removed.

## Notes

- The archive file format is the same binary format used for the main state file.
- Reaction counts, attachments, colors, and message IDs are preserved.
- There is no automatic cleanup of old archive files; manage disk space yourself.

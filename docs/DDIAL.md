# DDial in SSH-Chatter

SSH-Chatter speaks the Diversi Dial (DDial) line protocol in two directions:

- **Listener** (`-D bind:port`): retro terminals dial in over raw TCP/telnet
  and get a DDial-style chat with the MagViz command set.
- **Upstream client** (`CHATTER_DDIAL_HOST`/`PORT`): Chatter connects out to
  another DDial station and bridges it with the Chatter room.

Channel 1 is one conversation across all three: the Chatter room (SSH and
telnet users), local dial-ins tuned to T1, and the upstream station.

## Upstream client modes

| Variable | Meaning |
| --- | --- |
| `CHATTER_DDIAL_HOST`, `CHATTER_DDIAL_PORT` | Remote station. |
| `CHATTER_DDIAL_KEY` | Password sent at the `Enter Password` prompt (`###password` for member accounts). Empty = guest. Keep it in a root-only `EnvironmentFile`. |
| `CHATTER_DDIAL_HANDLE` | Handle set with `/H` after login. |
| `CHATTER_DDIAL_MODE` | `station` (default) or `user` (alias `hijack`). |
| `CHATTER_DDIAL_RELOGIN_COOLDOWN` | User mode: seconds to wait between hanging up and dialing again (default 120). |
| `CHATTER_DDIAL_LOCK_DIR` | User mode: directory for the account lock file (default: the user data root). |
| `CHATTER_DDIAL_STATION` | Station name used in `}}}` broadcasts (station mode). |

### Station mode

For a real Station Link account. Outbound chat uses the full wire form
`#slot[T1:handle) message`, and Chatter sends `}` / `}}` login and logout
events and a `}}}` user list every 15 minutes, so the remote sees each
Chatter user as their own line.

### User ("hijack") mode

For an ordinary member or guest account on a DDial that won't issue a link
account. Chatter logs in as one user and relays traffic through that user.
Two problems with this approach are handled explicitly:

1. **Echo loops.** The remote repeats everything we type as
   `#N[T1:ourhandle) ...`. If those lines came back into Chatter, every
   message would show up again as the relay user. In user mode:
   - Outbound chat goes out as `speaker: message`. The remote adds our own
     prefix itself.
   - The last eight outbound lines are remembered for 90 s. An incoming line
     that matches one of them, or any line under our own handle, is dropped
     before it reaches the room or the dial-ins.
   - Link-only traffic (login/logout events, `}}}` lists, link-form private
     messages) is never sent.
2. **Double logins disable the account.** Classic ddials disable an account
   that is logged in twice. Over telnet that happens easily after a quick
   reconnect or a service restart. User mode therefore:
   - Holds an exclusive `flock` on `ddial-account-<host>-<port>.lock`, so a
     second ssh-chatter process (a restart overlap or a test run) cannot
     dial the same account. It waits instead.
   - Writes `active <epoch>` / `closed <epoch>` to that file and, across
     restarts, waits `CHATTER_DDIAL_RELOGIN_COOLDOWN` seconds after the
     last hang-up before dialing again. If the previous process died while
     connected, the full cooldown starts from the moment the new process
     notices.
   - Sends `/Q` before hanging up (including on `systemctl stop`), so the
     remote logs the account out instead of waiting for a carrier timeout.
   - Stops reconnecting when the remote reports a duplicate or locked login
     ("already logged in", "account disabled", ...). An operator clears this
     with `/ddial reconnect`.
   - Only treats a line as a kick or hang-up if it is not user-authored chat
     (someone typing "bye" no longer drops the link). A `-->` prompt glued to
     the front of a chat line is stripped first.

Only operators can run `/ddial connect|disconnect|reconnect`.
`/ddial status` shows the mode and any lockout.

## Listener: MagViz command set

Dial-ins log in with a guest handle, or as members with `###password` /
`###:password[:command]`. `/signup <password>` registers the current handle.
Members, mail and offline messages are stored in `ddial_members.tsv` and
`ddial_mail.tsv` under the user data root (`CHATTER_DDIAL_MEMBERS_DIR`
overrides it), with PBKDF2-SHA256 password hashes.

Type `/i`, `/?m` and `/t?` on a dial-in for the full lists. Summary:

- **Everyone:** `/i /h /s[#] /sm /p# @# /m /r# /ls /b /b? /bv# /font /baud /sc /re
  /ai /q /q+ /jk[#] /giphy /forgot /signup /ps`
- **Members:** `/t# (1-999) /t+# /t-# /ts# ;msg /tb# /p#,# /p=#,# /pv /h#RRGGBBName
  /h#= /h? /a /a# /us /c /pre /post /r= /t? /ig# /ig+# /ig-# /x# /xx# /cls /m= /n
  /null# /topic /ask /answer /8ball /anim# /taco /buzz /image /pw= /remember //+
  /@? /bday /bday+ /bday- /bdayRemove /bg /o# /e? /e /e=# /e-# /e-all /d /u /300
  /1200 /2400 /reverse /scramble /lorem /code /define /whois# /stats /bior /voice
  /spaces /sentry`
- **Extras:** `` ` `` alternating case, `~` rainbow, `,` vanishes after 30 s,
  `\i \b \u` italic/bold/underline, `#RRGGBB` inline colour.

Notes:

- Channels 1-4 map to the linked station's channels. 5-999 stay on this
  station. Guests are limited to 1-4.
- Colours are mapped onto the xterm-256 palette so retro terminals render
  them.
- `/baud` and `/300`-style messages are paced on the server side.
- Commands that only make sense in a web client (`/font`, `/n`, `/voice`,
  `/remember`) are stored as preferences for GUI front ends, and the terminal
  says so.
- `/giphy` needs `CHATTER_GIPHY_API_KEY`. `/define` uses dictionaryapi.dev.
  `/image` (AI image generation) is not available.
- CoSysops are set with `CHATTER_DDIAL_COSYSOPS=1,7,...` (member numbers) or
  by another CoSysop with `/sc+#`.

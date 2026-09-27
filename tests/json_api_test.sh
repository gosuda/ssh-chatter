#!/usr/bin/env bash
# json_api_test.sh — JSON API smoke test
# usage: ./tests/json_api_test.sh [host] [port]
#
# Connects to the JSON API, exercises login, auth, history, users,
# mail (list / send / clear) and verifies that message events arrive.
# Exits 0 on success, 1 on any failure.

set -euo pipefail

HOST=${1:-127.0.0.1}
PORT=${2:-34567}
TIMEOUT=5     # seconds to wait for each response
PASS=0
FAIL=0

# ── helpers ────────────────────────────────────────────────────────────────

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }

check_nc() {
    if ! command -v nc &>/dev/null; then
        echo "nc (netcat) not found — install it and retry" >&2
        exit 1
    fi
}

# Send one JSON line, read lines until we find one matching $pattern.
# Returns the matched line in global REPLY_LINE.
send_recv() {
    local req="$1" pattern="$2"
    # Write request + newline, then read for up to $TIMEOUT seconds.
    REPLY_LINE=$(printf '%s\n' "$req" | nc -q "$TIMEOUT" "$HOST" "$PORT" 2>/dev/null \
        | grep -m1 "$pattern" || true)
}

# Send request over a persistent connection (FD 3/4 opened by caller).
send_fd() { printf '%s\n' "$1" >&3; }

recv_fd() {
    local pattern="$1"
    local deadline=$(( $(date +%s) + TIMEOUT ))
    REPLY_LINE=""
    while [[ $(date +%s) -lt $deadline ]]; do
        if IFS= read -r -t 1 line <&4; then
            if echo "$line" | grep -q "$pattern"; then
                REPLY_LINE="$line"
                return 0
            fi
        fi
    done
    return 1
}

ok() {
    green "  PASS: $1"
    (( PASS++ )) || true
}

fail() {
    red "  FAIL: $1"
    red "        got: ${REPLY_LINE:-<nothing>}"
    (( FAIL++ )) || true
}

assert_ok() {
    local label="$1"
    if echo "$REPLY_LINE" | python3 -c "
import sys, json
d=json.load(sys.stdin)
sys.exit(0 if d.get('ok') else 1)
" 2>/dev/null; then
        ok "$label"
    else
        fail "$label"
    fi
}

assert_field() {
    local label="$1" field="$2"
    if echo "$REPLY_LINE" | python3 -c "
import sys, json
d=json.load(sys.stdin)
sys.exit(0 if '$field' in d or '$field' in d.get('payload',{}) else 1)
" 2>/dev/null; then
        ok "$label"
    else
        fail "$label"
    fi
}

extract() {
    # extract_json_field <field> <json>
    python3 -c "import sys,json; d=json.loads(sys.argv[2]); print(d.get('payload',{}).get(sys.argv[1], d.get(sys.argv[1],'')), end='')" "$1" "$2" 2>/dev/null || true
}

# ── connectivity ───────────────────────────────────────────────────────────

check_nc
echo "=== JSON API test  $HOST:$PORT ==="

# Quick port-open check before opening persistent FD
if ! nc -z -w2 "$HOST" "$PORT" 2>/dev/null; then
    echo "Cannot reach $HOST:$PORT — is ssh-chatter running?" >&2
    exit 1
fi

# Open a persistent TCP connection via FDs 3 (write) and 4 (read).
exec 3<>/dev/tcp/"$HOST"/"$PORT"
exec 4<&3

echo ""
echo "--- auth ---"

# 1. login (get JWT)
send_fd '{"type":"login","username":"testbot","request_id":"1"}'
if recv_fd '"ok":true'; then
    assert_ok "login returns ok=true"
    TOKEN=$(extract token "$REPLY_LINE")
    if [[ -z "$TOKEN" ]]; then
        fail "login response contains token field"
    else
        ok "login response contains token field"
    fi
else
    fail "login: no response"
    TOKEN=""
fi

# 2. auth with token
if [[ -n "$TOKEN" ]]; then
    send_fd "{\"type\":\"auth\",\"token\":\"$TOKEN\",\"request_id\":\"2\"}"
    if recv_fd '"ok":true'; then
        assert_ok "auth returns ok=true"
    else
        fail "auth: no response"
    fi
fi

# 3. auth with bad token → expect ok=false
send_fd '{"type":"auth","token":"bad.token.here","request_id":"3"}'
if recv_fd '"ok"'; then
    if echo "$REPLY_LINE" | grep -q '"ok":false'; then
        ok "bad token rejected"
    else
        fail "bad token should be rejected"
    fi
else
    fail "bad token: no response"
fi

# Re-auth with good token so subsequent requests work
if [[ -n "$TOKEN" ]]; then
    send_fd "{\"type\":\"auth\",\"token\":\"$TOKEN\",\"request_id\":\"4\"}"
    recv_fd '"ok":true' || true
fi

echo ""
echo "--- history ---"

# 4. history default
send_fd '{"type":"history","request_id":"5"}'
if recv_fd '"messages"'; then
    ok "history returns messages array"
    assert_field "history payload has has_more" "has_more"
else
    fail "history: no response"
fi

# 5. history with limit=1
send_fd '{"type":"history","limit":1,"request_id":"6"}'
if recv_fd '"messages"'; then
    COUNT=$(echo "$REPLY_LINE" | python3 -c "
import sys,json
d=json.loads(sys.stdin.read())
p=d.get('payload',d)
msgs=p.get('messages',[]) if isinstance(p,dict) else []
print(len(msgs))
" 2>/dev/null || echo "?")
    if [[ "$COUNT" -le 1 ]] 2>/dev/null; then
        ok "history limit=1 returns ≤1 message"
    else
        fail "history limit=1 returned $COUNT messages"
    fi
else
    fail "history limit=1: no response"
fi

echo ""
echo "--- users ---"

# 6. users
send_fd '{"type":"users","request_id":"7"}'
if recv_fd '"chatter"'; then
    ok "users returns chatter array"
    assert_field "users payload has ddial array" "ddial"
else
    fail "users: no response"
fi

echo ""
echo "--- mail ---"

# 7. mail list
send_fd '{"type":"mail","action":"list","request_id":"8"}'
if recv_fd '"ok"'; then
    assert_ok "mail list returns ok"
else
    fail "mail list: no response"
fi

# 8. mail send to self (testbot → testbot; tests the dispatch path even if
#    offline delivery is used)
send_fd '{"type":"mail","action":"send","to":"testbot","message":"hello from json api test","request_id":"9"}'
if recv_fd '"ok"'; then
    # ok=true means delivered, ok=false with an error message is also fine
    # (e.g. "cannot send to yourself") — we just want a response
    ok "mail send gets a response"
else
    fail "mail send: no response"
fi

# 9. mail clear
send_fd '{"type":"mail","action":"clear","request_id":"10"}'
if recv_fd '"ok":true'; then
    assert_ok "mail clear returns ok=true"
else
    fail "mail clear: no response"
fi

echo ""
echo "--- message event ---"

# 10. Post a chat message and verify we receive it back as an event.
send_fd '{"type":"chat","message":"json-api-test-probe","request_id":"11"}'
# Expect both the ACK and the broadcast event.
if recv_fd '"ok":true'; then
    ok "chat post acknowledged"
else
    fail "chat post: no ack"
fi

# The server broadcasts to all connected clients including us.
if recv_fd '"event":"message"'; then
    ok "message event received after chat post"
    # Check source field
    if echo "$REPLY_LINE" | grep -q '"source"'; then
        ok "message event has source field"
    else
        fail "message event missing source field"
    fi
else
    fail "no message event received"
fi

exec 3>&-
exec 4>&-

echo ""
echo "=== results: $PASS passed, $FAIL failed ==="
[[ $FAIL -eq 0 ]]

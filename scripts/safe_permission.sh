#!/usr/bin/env bash
set -euo pipefail

require_binary() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required executable: $1" >&2
    exit 1
  fi
}

require_binary chmod
require_binary install

python_resolve() {
  python3 - <<'PY' "$1"
import os
import sys
path = sys.argv[1]
print(os.path.abspath(path))
PY
}

resolve_path() {
  local input="$1"
  if [[ -z "$input" ]]; then
    echo "";
    return
  fi

  if command -v python3 >/dev/null 2>&1; then
    python_resolve "$input"
    return
  fi

  if command -v realpath >/dev/null 2>&1; then
    local resolved
    if resolved=$(realpath "$input" 2>/dev/null); then
      echo "$resolved"
      return
    fi
  fi

  local dir
  dir=$(dirname -- "$input")
  local base
  base=$(basename -- "$input")
  if dir=$(cd "$dir" 2>/dev/null && pwd); then
    echo "$dir/$base"
    return
  fi

  echo "$input"
}

STATE_ROOT="${STATE_ROOT:-${CHATTER_STATE_DIR:-/var/lib/ssh-chatter}}"
if [[ -n "$STATE_ROOT" ]]; then
  STATE_ROOT=$(resolve_path "$STATE_ROOT")
fi

DEFAULT_TARGETS=(
  "${CHATTER_BBS_FILE:-$STATE_ROOT/bbs_state.dat}"
  "${CHATTER_VOTE_FILE:-$STATE_ROOT/vote_state.dat}"
  "${CHATTER_GEMINI_COOLDOWN_FILE:-$STATE_ROOT/gemini_cooldown.dat}"
  "${CHATTER_STATE_FILE:-$STATE_ROOT/chatter_state.dat}"
  "${CHATTER_ELIZA_MEMORY_FILE:-$STATE_ROOT/eliza_memory.dat}"
  "${CHATTER_PW_AUTH_FILE:-$STATE_ROOT/pw_auth.dat}"
  "${CHATTER_NICKNAME_CLAIM_FILE:-$STATE_ROOT/nickname_claims.dat}"
)

DEFAULT_DIRECTORIES=(
  "${CHATTER_USER_DATA_ROOT:-$STATE_ROOT/user-data}"
)

declare -a TARGETS=()
if [[ $# -gt 0 ]]; then
  TARGETS=("$@")
else
  TARGETS=("${DEFAULT_TARGETS[@]}")
fi

secure_file() {
  local target_input="$1"
  if [[ -z "$target_input" ]]; then
    return
  fi

  local target
  target=$(resolve_path "$target_input")
  if [[ -z "$target" ]]; then
    echo "Unable to resolve path: $target_input" >&2
    return
  fi

  if [[ -n "$STATE_ROOT" ]]; then
    case "$target" in
      "$STATE_ROOT"|"$STATE_ROOT"/*) ;;
      *)
        echo "Skipping $target (outside $STATE_ROOT)" >&2
        return
        ;;
    esac
  fi

  local parent
  parent=$(dirname -- "$target")
  mkdir -p "$parent"
  chmod 750 "$parent" 2>/dev/null || true

  if [[ -e "$target" ]]; then
    if [[ -d "$target" ]]; then
      echo "Skipping directory $target" >&2
      return
    fi
  else
    install -m 600 /dev/null "$target"
  fi

  chmod 600 "$target"
  echo "Secured $target"
}

for path in "${TARGETS[@]}"; do
  secure_file "$path"
done

for directory in "${DEFAULT_DIRECTORIES[@]}"; do
  if [[ -z "$directory" ]]; then
    continue
  fi
  resolved=$(resolve_path "$directory")
  if [[ -n "$STATE_ROOT" ]]; then
    case "$resolved" in
      "$STATE_ROOT"|"$STATE_ROOT"/*) ;;
      *)
        echo "Skipping $resolved (outside $STATE_ROOT)" >&2
        continue
        ;;
    esac
  fi
  install -d -m 750 "$resolved"
  install -d -m 750 "$resolved/profiles"
  echo "Secured $resolved"
done

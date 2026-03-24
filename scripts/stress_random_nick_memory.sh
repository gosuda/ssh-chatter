#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BINARY="${REPO_ROOT}/ssh-chatter"

HOST="${HOST:-127.0.0.1}"
SSH_PORT="${SSH_PORT:-2222}"
TELNET_PORT="${TELNET_PORT:-2323}"
DURATION_SECONDS="${DURATION_SECONDS:-600}"
BURST_SIZE="${BURST_SIZE:-20}"
BURST_INTERVAL_SECONDS="${BURST_INTERVAL_SECONDS:-2}"
RUN_DIR="${REPO_ROOT}/stress_random_nick_run"
mkdir -p "${RUN_DIR}"
HOST_KEY_PATH="${RUN_DIR}/ssh_host_rsa_key"
MOTD_PATH="${RUN_DIR}/motd.txt"
SERVER_LOG="${RUN_DIR}/server.log"
MEMORY_LOG="${RUN_DIR}/memory.log"

if [[ ! -x "${BINARY}" ]]; then
    echo "ERROR: ${BINARY} not found. Run: make"
    exit 1
fi

if ! command -v nc >/dev/null 2>&1; then
    echo "ERROR: nc(netcat) is required."
    exit 1
fi

if [[ ! -f "${HOST_KEY_PATH}" ]]; then
    ssh-keygen -t rsa -b 2048 -f "${HOST_KEY_PATH}" -N "" -q
fi

printf "Random nick stress test\n" > "${MOTD_PATH}"

cleanup() {
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

"${BINARY}" \
    -a "${HOST}" \
    -p "${SSH_PORT}" \
    -T "${TELNET_PORT}" \
    -m "${MOTD_PATH}" \
    -k "${RUN_DIR}" > "${SERVER_LOG}" 2>&1 &
SERVER_PID=$!

sleep 2
if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
    echo "ERROR: server failed to start."
    exit 1
fi

echo "# time elapsed_sec rss_kb vmrss_line" > "${MEMORY_LOG}"

start_ts="$(date +%s)"
end_ts=$((start_ts + DURATION_SECONDS))

while [[ "$(date +%s)" -lt "${end_ts}" ]]; do
    for _ in $(seq 1 "${BURST_SIZE}"); do
        nick="u$(tr -dc 'a-z0-9' </dev/urandom | head -c 10)"
        {
            printf "/nick %s\r\n" "${nick}"
            printf "/exit\r\n"
        } | nc -w 2 "${HOST}" "${TELNET_PORT}" >/dev/null 2>&1 || true
    done

    if [[ -r "/proc/${SERVER_PID}/status" ]]; then
        vmrss_line="$(grep '^VmRSS:' "/proc/${SERVER_PID}/status" || true)"
        rss_kb="$(awk '/^VmRSS:/ {print $2}' "/proc/${SERVER_PID}/status" 2>/dev/null || echo 0)"
        now_ts="$(date +%s)"
        elapsed=$((now_ts - start_ts))
        printf "%s %s %s %s\n" "$(date -Iseconds)" "${elapsed}" "${rss_kb}" "${vmrss_line}" >> "${MEMORY_LOG}"
    fi

    sleep "${BURST_INTERVAL_SECONDS}"
done

echo "done. logs:"
echo "  server: ${SERVER_LOG}"
echo "  memory: ${MEMORY_LOG}"

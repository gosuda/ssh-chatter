#!/usr/bin/env bash
# Sophisticated multi-user memory leak test script for ssh-chatter

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
VALGRIND_SUPP="${REPO_ROOT}/valgrind-libgc.supp"
BINARY="${REPO_ROOT}/ssh-chatter"

# --- Configuration ---
DURATION=20          # How long to run the test in seconds
NUM_CLIENTS=3        # Number of concurrent clients
MESSAGES_PER_CLIENT=10 # Number of messages each client will send
PORT=2223            # Use a different port to avoid conflict
HOST="127.0.0.1"
PASSWORD="testpassword" # A dummy password for the test users

# Check if dependencies are installed
if ! command -v valgrind &> /dev/null; then
    echo "ERROR: valgrind is not installed." && exit 1
fi
if ! command -v sshpass &> /dev/null; then
    echo "ERROR: sshpass is not installed." && exit 1
fi
if [[ ! -f "${BINARY}" ]]; then
    echo "ERROR: ${BINARY} not found. Build it first with 'make'"
    exit 1
fi

# --- Setup ---
echo "=========================================="
echo "SSH-Chatter Multi-User Leak Test"
echo "=========================================="

RUN_DIR="${REPO_ROOT}/leak_test_run"
rm -rf "${RUN_DIR}"
mkdir -p "${RUN_DIR}"
echo "Using run directory: ${RUN_DIR}"

# Generate SSH host key
SSH_KEY_PATH="${RUN_DIR}/ssh_host_rsa_key"
ssh-keygen -t rsa -b 2048 -f "${SSH_KEY_PATH}" -N "" -q
echo "Generated host key at ${SSH_KEY_PATH}"

# Create MOTD
MOTD_PATH="${RUN_DIR}/motd"
echo "Multi-user leak test server" > "${MOTD_PATH}"

# --- Server Start ---
VALGRIND_LOG="${RUN_DIR}/valgrind.log"
SUPP_ARG=""
if [[ -f "${VALGRIND_SUPP}" ]]; then
    SUPP_ARG="--suppressions=${VALGRIND_SUPP}"
fi

echo "Starting server with valgrind..."
valgrind \
    --leak-check=full \
    --show-leak-kinds=definite,possible \
    --track-origins=yes \
    --log-file="${VALGRIND_LOG}" \
    ${SUPP_ARG} \
    "${BINARY}" \
    -a "${HOST}" \
    -p "${PORT}" \
    -m "${MOTD_PATH}" \
    -k "${RUN_DIR}" \
    -T off & 
SERVER_PID=$!
echo "Server started with PID ${SERVER_PID}"
# Give the server a moment to start up
sleep 2

# --- Client Simulation ---
CLIENT_PIDS=()
echo "Starting ${NUM_CLIENTS} clients..."

# Function to run a single client
run_client() {
    local client_id=$1
    local user="user${client_id}"
    local output_log="${RUN_DIR}/client_${client_id}_output.log"
    
    # Prepare the commands for the client to send
    local commands=""
    for i in $(seq 1 ${MESSAGES_PER_CLIENT}); do
        commands+="say Hello from ${user}, message ${i}\n"
        commands+="sleep 0.5\n" # Stagger messages
    done
    commands+="quit\n"

    # Use sshpass to run the client non-interactively
    # The 'stty -echo' is to prevent the commands from being echoed back in the log
    echo -e "${commands}" | sshpass -p "${PASSWORD}" \
        ssh -o "StrictHostKeyChecking=no" -o "UserKnownHostsFile=/dev/null" \
        -p "${PORT}" "${user}@${HOST}" > "${output_log}" 2>&1
}

# Start all clients in the background
for i in $(seq 1 ${NUM_CLIENTS}); do
    run_client "$i" &
    CLIENT_PIDS+=($!)
done

echo "${NUM_CLIENTS} clients running in background with PIDs: ${CLIENT_PIDS[*]}"
echo "Test will run for ${DURATION} seconds..."

# --- Wait and Teardown ---
# Wait for clients to finish or timeout
wait_time=$((DURATION - 5)) # Give clients time to finish before killing server
echo "Waiting for clients to finish (timeout: ${wait_time}s)..."
for pid in "${CLIENT_PIDS[@]}"; do
    # Use a simple wait with timeout
    if ! wait "${pid}" 2>/dev/null; then
        echo "Client PID ${pid} did not exit gracefully, killing."
        kill "${pid}" 2>/dev/null || true
    fi
done
echo "All clients have finished."

# Stop the server
echo "Stopping server (PID ${SERVER_PID})..."
kill "${SERVER_PID}"
wait "${SERVER_PID}" || true
echo "Server stopped."

# --- Analysis ---
echo ""
echo "=========================================="
echo "Valgrind Analysis"
echo "=========================================="
echo ""

if [[ ! -f "${VALGRIND_LOG}" ]]; then
    echo "ERROR: Valgrind log file not found!"
    exit 1
fi

# Display the valgrind log
cat "${VALGRIND_LOG}"
echo "----------------------------------------"

# Check for leaks
if grep -q "definitely lost: 0 bytes in 0 blocks" "${VALGRIND_LOG}" && \
   grep -q "possibly lost: 0 bytes in 0 blocks" "${VALGRIND_LOG}" && \
   ! grep -q "ERROR SUMMARY: [1-9]" "${VALGRIND_LOG}"; then
    echo "✅ PASSED: No memory leaks detected."
    exit 0
else
    echo "❌ FAILED: Memory leaks or errors detected."
    exit 1
fi

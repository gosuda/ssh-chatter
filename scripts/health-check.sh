#!/bin/bash
# health-check.sh
#
# This script performs a health check on the ssh-chatter service.
# It attempts to connect to the SSH server using a key-based authentication.
# If the connection fails, it restarts the ssh-chatter service.

set -euo pipefail

# Constants
readonly SSH_HOST="127.0.0.1"
readonly SSH_PORT="2222"
readonly USERNAME="health-check"
readonly KEY_PATH="/etc/ssh-chatter/keys/ssh_host_rsa_key"
readonly SERVICE_NAME="chatter"

# Logging function
log() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') - $1"
}

# Perform the health check
log "Performing health check for ${SERVICE_NAME} on ${SSH_HOST}:${SSH_PORT}..."

if ssh -p "${SSH_PORT}" -i "${KEY_PATH}" -o "PasswordAuthentication=no" -o "ConnectTimeout=10" "${USERNAME}@${SSH_HOST}" 'exit' &>/dev/null; then
    log "Health check PASSED. Service is running."
    exit 0
else
    log "Health check FAILED. Service seems to be down."
    log "Attempting to restart ${SERVICE_NAME} service..."
    if sudo systemctl restart "${SERVICE_NAME}"; then
        log "Service ${SERVICE_NAME} restarted successfully."
    else
        log "ERROR: Failed to restart ${SERVICE_NAME} service."
        exit 1
    fi
fi

#!/bin/bash
# health-check.sh
#
# This script checks if the specific port of the ssh-chatter service is open.
# If the port is unreachable, it restarts the service.

set -euo pipefail

# Constants
readonly SSH_HOST="127.0.0.1"
readonly SSH_PORT="2222"
readonly SERVICE_NAME="chatter"

# Logging function
log() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') - $1"
}

# Perform the health check by testing TCP connection
log "Checking if port ${SSH_PORT} is open on ${SSH_HOST}..."

# nc -z: scan mode (check connection without sending data)
# -w 5: timeout in seconds
if nc -z -w 5 "${SSH_HOST}" "${SSH_PORT}" &>/dev/null; then
    log "Health check PASSED. Port ${SSH_PORT} is reachable."
    exit 0
else
    log "Health check FAILED. Port ${SSH_PORT} is unreachable."
    log "Attempting to restart ${SERVICE_NAME} service..."
    
    if sudo systemctl restart "${SERVICE_NAME}"; then
        log "Service ${SERVICE_NAME} restarted successfully."
    else
        log "ERROR: Failed to restart ${SERVICE_NAME} service."
        exit 1
    fi
fi

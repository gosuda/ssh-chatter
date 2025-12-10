#!/bin/bash
# install_health_check.sh
#
# This script installs and enables the systemd timer for the ssh-chatter health check.

set -euo pipefail

# Constants
readonly SCRIPT_NAME="health-check.sh"
readonly SERVICE_FILE="ssh-chatter-health-check.service"
readonly TIMER_FILE="ssh-chatter-health-check.timer"

# The script is in the scripts directory, so paths are relative to the project root
readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

readonly SOURCE_SCRIPT_PATH="${PROJECT_ROOT}/scripts/${SCRIPT_NAME}"
readonly SOURCE_SERVICE_PATH="${PROJECT_ROOT}/${SERVICE_FILE}"
readonly SOURCE_TIMER_PATH="${PROJECT_ROOT}/${TIMER_FILE}"

readonly DEST_SCRIPT_PATH="/usr/local/bin/${SCRIPT_NAME}"
readonly DEST_SYSTEMD_PATH="/etc/systemd/system"

# Logging function
log() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') - $1"
}

# --- Main installation logic ---

# 1. Check for root privileges
if [[ $EUID -ne 0 ]]; then
   log "This script must be run as root. Please use sudo."
   exit 1
fi

log "Starting installation of ssh-chatter health check..."

# 2. Make health-check.sh executable
log "Making ${SCRIPT_NAME} executable."
chmod +x "${SOURCE_SCRIPT_PATH}"

# 3. Copy files to their destinations
log "Copying ${SCRIPT_NAME} to ${DEST_SCRIPT_PATH}..."
cp "${SOURCE_SCRIPT_PATH}" "${DEST_SCRIPT_PATH}"

log "Copying ${SERVICE_FILE} to ${DEST_SYSTEMD_PATH}..."
cp "${SOURCE_SERVICE_PATH}" "${DEST_SYSTEMD_PATH}/"

log "Copying ${TIMER_FILE} to ${DEST_SYSTEMD_PATH}..."
cp "${SOURCE_TIMER_PATH}" "${DEST_SYSTEMD_PATH}/"

# 4. Reload systemd daemon
log "Reloading systemd daemon..."
systemctl daemon-reload

# 5. Enable and start the timer
log "Enabling and starting ${TIMER_FILE}..."
systemctl enable "${TIMER_FILE}"
systemctl start "${TIMER_FILE}"

# 6. Final status
log "Installation complete."
log "The health check is now active and will run hourly."
systemctl status "${TIMER_FILE}"

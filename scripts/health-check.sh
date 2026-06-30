#!/bin/bash
# Simple health check for SSH-Chatter server
# Checks if the ssh-chatter binary is running and logs allocation counter if available.

PID_FILE="/var/run/ssh-chatter.pid"
LOG_FILE="/var/log/ssh-chatter/health-check.log"

if [ -f "$PID_FILE" ]; then
  PID=$(cat "$PID_FILE")
  if kill -0 $PID 2>/dev/null; then
    echo "$(date '+%Y-%m-%d %H:%M:%S') - ssh-chatter is running (PID $PID)" >> "$LOG_FILE"
    # Placeholder for allocation counter reporting.
  else
    echo "$(date '+%Y-%m-%d %H:%M:%S') - ssh-chatter not running, attempting restart" >> "$LOG_FILE"
    systemctl restart ssh-chatter.service
  fi
else
  echo "$(date '+%Y-%m-%d %H:%M:%S') - PID file missing, cannot perform health check" >> "$LOG_FILE"
fi

#!/usr/bin/env bash
set -euo pipefail

SERVICE_NAME="chatter.service"
INSTALL_PREFIX="${INSTALL_PREFIX:-/usr/local/bin}"
INSTALL_PATH="${INSTALL_PREFIX%/}/ssh-chatter"
STATE_DIR="${STATE_DIR:-/var/lib/ssh-chatter}"
CONFIG_DIR="${CONFIG_DIR:-/etc/ssh-chatter}"
SERVICE_USER="${SERVICE_USER:-ssh-chatter}"
SERVICE_GROUP="${SERVICE_GROUP:-$SERVICE_USER}"
SKIP_START="${SKIP_START:-0}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}"

require_binary() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required executable: $1" >&2
    exit 1
  fi
}

if [[ $EUID -ne 0 ]]; then
  echo "This installer must be run as root." >&2
  exit 1
fi

require_binary make
require_binary systemctl
require_binary install
require_binary ssh-keygen

mkdir -p "$CONFIG_DIR" "$STATE_DIR"

if ! getent group "$SERVICE_GROUP" >/dev/null 2>&1; then
  groupadd --system "$SERVICE_GROUP"
fi

if ! id -u "$SERVICE_USER" >/dev/null 2>&1; then
  useradd --system --home "$STATE_DIR" --shell /usr/sbin/nologin --gid "$SERVICE_GROUP" "$SERVICE_USER"
fi

usermod -a -G "$SERVICE_GROUP" "$SERVICE_USER" 2>/dev/null || true
chown "$SERVICE_USER:$SERVICE_GROUP" "$STATE_DIR"
chmod 750 "$STATE_DIR"

if [[ ! -f "$CONFIG_DIR/motd" ]]; then
  cat <<MOTD >"$CONFIG_DIR/motd"
Welcome to ssh-chatter!
This is the default MOTD. Update this file at $CONFIG_DIR/motd to customise.
MOTD
fi

if [[ ! -f "$CONFIG_DIR/chatter.env" ]]; then
  cat <<ENV >"$CONFIG_DIR/chatter.env"
# Override ssh-chatter runtime defaults by editing and uncommenting the values below.
# CHATTER_BIND_ADDRESS=0.0.0.0
# CHATTER_PORT=2222
# CHATTER_MOTD_FILE=$CONFIG_DIR/motd
# CHATTER_HOST_KEY_DIR=$STATE_DIR
# CHATTER_EXTRA_ARGS=
ENV
fi

if [[ ! -f "$STATE_DIR/ssh_host_rsa_key" ]]; then
  ssh-keygen -q -t rsa -b 4096 -N "" -f "$STATE_DIR/ssh_host_rsa_key"
  chown "$SERVICE_USER:$SERVICE_GROUP" "$STATE_DIR"/ssh_host_rsa_key*
  chmod 640 "$STATE_DIR/ssh_host_rsa_key"
  chmod 644 "$STATE_DIR/ssh_host_rsa_key.pub"
fi

make -C "$PROJECT_ROOT"
install -Dm755 "$PROJECT_ROOT/ssh-chatter" "$INSTALL_PATH"
chown root:root "$INSTALL_PATH"

cat <<EOF_SERVICE >"$SERVICE_FILE"
[Unit]
Description=ssh-chatter SSH chat service
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$SERVICE_USER
Group=$SERVICE_GROUP
WorkingDirectory=$STATE_DIR
ExecStart=$INSTALL_PATH -a \$CHATTER_BIND_ADDRESS -p \$CHATTER_PORT -m \$CHATTER_MOTD_FILE -k \$CHATTER_HOST_KEY_DIR \$CHATTER_EXTRA_ARGS
Environment=CHATTER_BIND_ADDRESS=0.0.0.0
Environment=CHATTER_PORT=2222
Environment=CHATTER_MOTD_FILE=$CONFIG_DIR/motd
Environment=CHATTER_HOST_KEY_DIR=$STATE_DIR
Environment=CHATTER_STATE_DIR=$STATE_DIR
Environment=CHATTER_EXTRA_ARGS=
EnvironmentFile=-$CONFIG_DIR/chatter.env
Restart=on-failure
RestartSec=2s
AmbientCapabilities=CAP_NET_BIND_SERVICE
CapabilityBoundingSet=CAP_NET_BIND_SERVICE
NoNewPrivileges=true
ProtectSystem=full
ProtectHome=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
EOF_SERVICE

systemctl daemon-reload

if [[ "$SKIP_START" -eq 0 ]]; then
  systemctl enable --now "$SERVICE_NAME"
  systemctl status "$SERVICE_NAME" --no-pager
else
  echo "Skipping service start due to SKIP_START=$SKIP_START"
fi

cat <<SUMMARY
Installation complete.
Configuration directory: $CONFIG_DIR
State directory: $STATE_DIR
Binary installed to: $INSTALL_PATH
Service unit: $SERVICE_FILE
Manage the service with: systemctl status chatter.service
SUMMARY

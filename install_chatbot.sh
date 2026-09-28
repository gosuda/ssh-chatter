#!/bin/bash

# Define installation directory
INSTALL_DIR="/var/lib/ssh-chatter-ai"
CURRENT_DIR="/home/yjlee/ssh-chatter"

echo "Starting SSH Chatter AI chatbot installation to $INSTALL_DIR..."

# Check and install uv if not found
if ! command -v uv &> /dev/null
then
    echo "uv not found, installing uv..."
    curl -LsSf https://astral.sh/uv/install.sh | sh
    # Add uv to PATH for the current session if it's not already there
    export PATH="$HOME/.local/bin:$PATH"
    if ! command -v uv &> /dev/null
    then
        echo "Error: Failed to install uv or add it to PATH. Please install uv manually and ensure it's in your PATH."
        exit 1
    fi
    echo "uv installed successfully."
fi

# Create installation directory if it doesn't exist
if [ ! -d "$INSTALL_DIR" ]; then
    echo "Creating installation directory: $INSTALL_DIR"
    sudo mkdir -p "$INSTALL_DIR"
    sudo chown $USER:$USER "$INSTALL_DIR" # Temporarily give ownership to current user for setup
else
    echo "Installation directory $INSTALL_DIR already exists."
fi

# Copy ssh-chat-server
echo "Copying ssh-chat-server components..."
sudo cp -R "$CURRENT_DIR/ssh-chat-with-ai/ssh-chat" "$INSTALL_DIR/"
sudo chown -R $USER:$USER "$INSTALL_DIR/ssh-chat" # Temporarily give ownership

# Copy stream-ai-message-sender
echo "Copying stream-ai-message-sender components..."
sudo cp -R "$CURRENT_DIR/ssh-chat-with-ai/stream-ai-message-sender" "$INSTALL_DIR/"
sudo chown -R $USER:$USER "$INSTALL_DIR/stream-ai-message-sender" # Temporarily give ownership

# Build ssh-chat-server
echo "Building ssh-chat-server..."
cd "$INSTALL_DIR/ssh-chat" || { echo "Failed to change directory to $INSTALL_DIR/ssh-chat"; exit 1; }
go build -o ssh-chat-server .
if [ $? -ne 0 ]; then
    echo "Error: Failed to build ssh-chat-server."
    exit 1
fi
echo "ssh-chat-server built successfully."

# Install Python dependencies for stream-ai-message-sender using uv
echo "Installing Python dependencies for stream-ai-message-sender using uv..."
cd "$INSTALL_DIR/stream-ai-message-sender" || { echo "Failed to change directory to $INSTALL_DIR/stream-ai-message-sender"; exit 1; }
uv sync
if [ $? -ne 0 ]; then
    echo "Error: Failed to install Python dependencies."
    exit 1
fi
echo "Python dependencies installed successfully."

# Generate self-signed SSL/TLS certificates for gRPC
echo "Generating self-signed SSL/TLS certificates for gRPC..."
CERT_DIR="$INSTALL_DIR/stream-ai-message-sender/certs"
if [ ! -d "$CERT_DIR" ]; then
    mkdir -p "$CERT_DIR"
fi

openssl genrsa -out "$CERT_DIR/grpc_server.key" 2048
openssl req -new -x509 -sha256 -key "$CERT_DIR/grpc_server.key" -out "$CERT_DIR/grpc_server.cert" -days 3650 -nodes -subj "/C=US/ST=State/L=City/O=Organization/OU=Unit/CN=localhost"

if [ $? -ne 0 ]; then
    echo "Error: Failed to generate SSL/TLS certificates."
    exit 1
fi
echo "SSL/TLS certificates generated successfully in $CERT_DIR."

# Generate the chat server's SSH host key locally (never ship one in the repo)
HOST_KEY="$INSTALL_DIR/ssh_host_rsa_key"
if [ ! -f "$HOST_KEY" ]; then
    echo "Generating SSH host key at $HOST_KEY..."
    ssh-keygen -q -t rsa -b 4096 -N "" -f "$HOST_KEY" || { echo "Error: Failed to generate SSH host key."; exit 1; }
    chmod 600 "$HOST_KEY"
fi

# Prompt for API Key
API_KEY=""
read -p "Enter your GOOGLE_API_KEY or GEMINI_API_KEY (leave blank if you want to set it manually later): " API_KEY

# Create and configure service files
echo "Creating and configuring systemd service files..."

# ssh-chat-server.service
SSH_CHAT_SERVICE_CONTENT="[Unit]\nDescription=SSH Chat Server\nAfter=network.target\n\n[Service]\nUser=sshchatterai ; IMPORTANT: Change this to a dedicated, less privileged user\nGroup=sshchatterai ; IMPORTANT: Change this to a dedicated, less privileged group\nWorkingDirectory=$INSTALL_DIR/ssh-chat/\nExecStartPre=/usr/bin/bash -c \"go build -o ssh-chat-server .\"\nExecStart=$INSTALL_DIR/ssh-chat/ssh-chat-server -key $HOST_KEY -port 2222\nRestart=always\nRestartSec=5\n\n[Install]\nWantedBy=multi-user.target\n"
echo -e "$SSH_CHAT_SERVICE_CONTENT" | sudo tee /etc/systemd/system/ssh-chat-server.service > /dev/null

# ai-message-sender.service
AI_MESSAGE_SERVICE_CONTENT="[Unit]\nDescription=AI Message Sender for SSH Chatter\nAfter=network.target ssh-chat-server.service\n\n[Service]\nUser=sshchatterai ; IMPORTANT: Change this to a dedicated, less privileged user\nGroup=sshchatterai ; IMPORTANT: Change this to a dedicated, less privileged group\nWorkingDirectory=$INSTALL_DIR/stream-ai-message-sender/\nExecStartPre=/usr/bin/bash -c \"uv sync\"\nExecStart=/usr/bin/python3 main.py\nRestart=always\nRestartSec=5\n"

if [ -n "$API_KEY" ]; then
    AI_MESSAGE_SERVICE_CONTENT+="Environment=\"GOOGLE_API_KEY=$API_KEY\"\n"
    echo "API Key will be set in ai-message-sender.service."
else
    AI_MESSAGE_SERVICE_CONTENT+";Environment=\"GOOGLE_API_KEY=your_api_key_here\" ; Uncomment and set your API key\n"
    echo "API Key not provided. Please set it manually in ai-message-sender.service."
fi

AI_MESSAGE_SERVICE_CONTENT+="\n[Install]\nWantedBy=multi-user.target\n"
echo -e "$AI_MESSAGE_SERVICE_CONTENT" | sudo tee /etc/systemd/system/ai-message-sender.service > /dev/null

echo "Systemd service files created and configured in /etc/systemd/system/."
sudo systemctl daemon-reload

echo "Installation script finished. Please follow the next steps to finalize the setup:"
echo ""
echo "--- IMPORTANT NEXT STEPS ---"
echo "1. Create a dedicated, less privileged user and group for these services (e.g., 'sshchatterai')."
echo "   Example: sudo adduser --system --no-create-home --group sshchatterai"
echo "   (If you already ran this, you can skip)"

echo "2. Update the ownership of the installed directories to this new user/group:"
echo "   sudo chown -R sshchatterai:sshchatterai $INSTALL_DIR"

echo "3. Reload systemd, enable, and start the services:"
echo "   sudo systemctl daemon-reload"
echo "   sudo systemctl enable ssh-chat-server.service"
echo "   sudo systemctl enable ai-message-sender.service"
echo "   sudo systemctl start ssh-chat-server.service"
echo "   sudo systemctl start ai-message-sender.service"

echo "4. Check service status:"
echo "   sudo systemctl status ssh-chat-server.service"
echo "   sudo systemctl status ai-message-sender.service"
echo "----------------------------"

#define _POSIX_C_SOURCE 200809L

#include "headers/irc_client.h"
#include "headers/host.h"
#include "headers/humanized/humanized.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define IRC_BUFFER_SIZE 4096
#define IRC_RECONNECT_DELAY_SECONDS 30
#define IRC_PING_INTERVAL_SECONDS 60

struct irc_client {
    host_t *host;
    pthread_mutex_t lock;
    bool lock_initialized;
    pthread_t thread;
    bool thread_initialized;
    _Atomic bool stop;
    _Atomic bool running;
    _Atomic bool disabled;
    _Atomic bool connected;
    char server_host[256];
    int server_port;
    char channel[128];
    char nickname[64];
    char username[64];
    char realname[128];
    char status_message[256];
    int socket_fd;
    time_t last_ping;
};

static const char *irc_getenv(const char *name)
{
    const char *value = getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return nullptr;
    }
    return value;
}

static void irc_set_status(irc_client_t *client, const char *status)
{
    if (client == nullptr || status == nullptr) {
        return;
    }
    pthread_mutex_lock(&client->lock);
    snprintf(client->status_message, sizeof(client->status_message), "%s",
             status);
    pthread_mutex_unlock(&client->lock);
}

__attribute__((unused)) static void
irc_client_disable(irc_client_t *client, const char *message, int error_code)
{
    if (client == nullptr) {
        return;
    }

    bool expected = false;
    if (!atomic_compare_exchange_strong(&client->disabled, &expected, true)) {
        return;
    }

    int log_code = (error_code != 0) ? error_code : EIO;
    if (message != nullptr && message[0] != '\0') {
        humanized_log_error("irc", message, log_code);
    } else {
        humanized_log_error("irc", "IRC relay disabled after failure",
                            log_code);
    }

    irc_set_status(client, "Disabled");
    atomic_store(&client->stop, true);
}

static bool irc_connect_socket(irc_client_t *client)
{
    if (client == nullptr) {
        return false;
    }

    if (client->socket_fd >= 0) {
        close(client->socket_fd);
        client->socket_fd = -1;
    }

    struct addrinfo hints, *result, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", client->server_port);

    int ret = getaddrinfo(client->server_host, port_str, &hints, &result);
    if (ret != 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Failed to resolve IRC server %s: %s",
                 client->server_host, gai_strerror(ret));
        irc_set_status(client, msg);
        return false;
    }

    for (rp = result; rp != nullptr; rp = rp->ai_next) {
        client->socket_fd =
            socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (client->socket_fd == -1) {
            continue;
        }

        if (connect(client->socket_fd, rp->ai_addr, rp->ai_addrlen) != -1) {
            break; // Success
        }

        close(client->socket_fd);
        client->socket_fd = -1;
    }

    freeaddrinfo(result);

    if (client->socket_fd == -1) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Failed to connect to IRC server %s:%d",
                 client->server_host, client->server_port);
        irc_set_status(client, msg);
        return false;
    }

    // Send IRC-style handshake
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "NICK %s\r\n", client->nickname);
    if (send(client->socket_fd, buffer, strlen(buffer), 0) < 0) {
        close(client->socket_fd);
        client->socket_fd = -1;
        return false;
    }

    snprintf(buffer, sizeof(buffer), "USER %s 0 * :%s\r\n", client->username,
             client->realname);
    if (send(client->socket_fd, buffer, strlen(buffer), 0) < 0) {
        close(client->socket_fd);
        client->socket_fd = -1;
        return false;
    }

    // Join channel
    snprintf(buffer, sizeof(buffer), "JOIN %s\r\n", client->channel);
    if (send(client->socket_fd, buffer, strlen(buffer), 0) < 0) {
        close(client->socket_fd);
        client->socket_fd = -1;
        return false;
    }

    atomic_store(&client->connected, true);
    client->last_ping = time(nullptr);
    irc_set_status(client, "Connected");

    return true;
}

static void irc_disconnect_socket(irc_client_t *client)
{
    if (client == nullptr) {
        return;
    }

    atomic_store(&client->connected, false);

    if (client->socket_fd >= 0) {
        // Send QUIT message
        const char *quit_msg = "QUIT :Disconnecting\r\n";
        send(client->socket_fd, quit_msg, strlen(quit_msg), 0);
        close(client->socket_fd);
        client->socket_fd = -1;
    }

    irc_set_status(client, "Disconnected");
}

// RFC 1459 / RFC 2812 compliant IRC message parser
static void irc_handle_message(irc_client_t *client, const char *line)
{
    if (client == nullptr || line == nullptr || client->host == nullptr) {
        return;
    }

    // IRC message format: [':' prefix SPACE] command [params] CRLF
    const char *cursor = line;
    char prefix[256] = {0};
    char command[64] = {0};
    
    // Parse optional prefix (starts with ':')
    if (*cursor == ':') {
        cursor++; // Skip ':'
        const char *space = strchr(cursor, ' ');
        if (space == nullptr) {
            return; // Invalid message
        }
        size_t prefix_len = (size_t)(space - cursor);
        if (prefix_len >= sizeof(prefix)) {
            prefix_len = sizeof(prefix) - 1;
        }
        memcpy(prefix, cursor, prefix_len);
        prefix[prefix_len] = '\0';
        cursor = space + 1; // Move past space
    }
    
    // Parse command
    const char *space = strchr(cursor, ' ');
    size_t cmd_len;
    if (space != nullptr) {
        cmd_len = (size_t)(space - cursor);
    } else {
        cmd_len = strlen(cursor);
    }
    if (cmd_len >= sizeof(command)) {
        cmd_len = sizeof(command) - 1;
    }
    memcpy(command, cursor, cmd_len);
    command[cmd_len] = '\0';
    
    // Move cursor past command and space
    if (space != nullptr) {
        cursor = space + 1;
    } else {
        cursor += cmd_len;
    }
    
    // Handle PING command (RFC 1459 section 4.6.2)
    if (strcmp(command, "PING") == 0) {
        // PING format: PING <server1> [<server2>]
        // Response: PONG <server2> <server1>
        char pong[512];
        snprintf(pong, sizeof(pong), "PONG %s\r\n", cursor);
        send(client->socket_fd, pong, strlen(pong), 0);
        return;
    }
    
    // Handle PRIVMSG command (RFC 1459 section 4.4.1)
    if (strcmp(command, "PRIVMSG") == 0) {
        // PRIVMSG format: PRIVMSG <target> :<message>
        // Find the target (channel or nick)
        const char *target_end = strchr(cursor, ' ');
        if (target_end == nullptr) {
            return; // No message text
        }
        
        // Move to message text (skip target and space)
        const char *msg_text = target_end + 1;
        
        // Check if message starts with ':' (trailing parameter)
        if (*msg_text == ':') {
            msg_text++; // Skip ':'
        }
        
        // Check for CTCP message (starts and ends with \x01)
        if (*msg_text == '\x01') {
            const char *ctcp_end = strchr(msg_text + 1, '\x01');
            if (ctcp_end != nullptr) {
                size_t ctcp_len = (size_t)(ctcp_end - (msg_text + 1));
                
                // Handle CTCP VERSION query
                if (ctcp_len == 7 && strncmp(msg_text + 1, "VERSION", 7) == 0) {
                    // Extract sender nickname from prefix (nick!user@host)
                    if (prefix[0] != '\0') {
                        const char *nick_end = strchr(prefix, '!');
                        char sender_nick[64] = {0};
                        size_t nick_len;
                        
                        if (nick_end != nullptr) {
                            nick_len = (size_t)(nick_end - prefix);
                        } else {
                            nick_len = strlen(prefix);
                        }
                        
                        if (nick_len < sizeof(sender_nick)) {
                            memcpy(sender_nick, prefix, nick_len);
                            sender_nick[nick_len] = '\0';
                            
                            // Send CTCP VERSION reply via NOTICE
                            char reply[512];
                            snprintf(reply, sizeof(reply),
                                   "NOTICE %s :\x01VERSION SSH-Chatter IRC Bridge v1.0\x01\r\n",
                                   sender_nick);
                            send(client->socket_fd, reply, strlen(reply), 0);
                        }
                    }
                }
            }
            // Don't post CTCP messages to chat room
            return;
        }
        
        // Extract nickname from prefix (format: nick!user@host or nick@host or nick)
        char nick[64] = {0};
        if (prefix[0] != '\0') {
            const char *nick_end = strchr(prefix, '!');
            if (nick_end == nullptr) {
                nick_end = strchr(prefix, '@');
            }
            
            size_t nick_len;
            if (nick_end != nullptr) {
                nick_len = (size_t)(nick_end - prefix);
            } else {
                nick_len = strlen(prefix);
            }
            
            if (nick_len < sizeof(nick)) {
                memcpy(nick, prefix, nick_len);
                nick[nick_len] = '\0';
            }
        }
        
        // Post message to chat room
        char formatted[SSH_CHATTER_MESSAGE_LIMIT];
        snprintf(formatted, sizeof(formatted), "[IRC] %s", msg_text);
        const char *username = nick[0] != '\0' ? nick : "irc-relay";
        
        if (!host_post_client_message(client->host, username, formatted,
                                      nullptr, nullptr, false)) {
            // Silently fail - don't flood logs
        }
    }
}

static void *irc_client_thread(void *arg)
{
    irc_client_t *client = (irc_client_t *)arg;
    if (client == nullptr) {
        return nullptr;
    }

    atomic_store(&client->running, true);
    irc_set_status(client, "Starting");

    char buffer[IRC_BUFFER_SIZE];
    size_t buffer_pos = 0;

    while (!atomic_load(&client->stop)) {
        if (!atomic_load(&client->connected)) {
            if (!irc_connect_socket(client)) {
                sleep(IRC_RECONNECT_DELAY_SECONDS);
                continue;
            }
        }

        // Send periodic PING
        time_t now = time(nullptr);
        if (now - client->last_ping > IRC_PING_INTERVAL_SECONDS) {
            const char *ping_msg = "PING :keepalive\r\n";
            if (send(client->socket_fd, ping_msg, strlen(ping_msg), 0) < 0) {
                irc_disconnect_socket(client);
                continue;
            }
            client->last_ping = now;
        }

        // Receive data
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client->socket_fd, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int ret = select(client->socket_fd + 1, &read_fds, nullptr, nullptr,
                         &timeout);
        if (ret < 0) {
            irc_disconnect_socket(client);
            continue;
        }

        if (ret == 0) {
            continue; // Timeout
        }

        ssize_t bytes = recv(client->socket_fd, buffer + buffer_pos,
                             sizeof(buffer) - buffer_pos - 1, 0);
        if (bytes <= 0) {
            irc_disconnect_socket(client);
            continue;
        }

        buffer_pos += (size_t)bytes;
        buffer[buffer_pos] = '\0';

        // Process complete lines
        char *line_start = buffer;
        char *line_end;
        while ((line_end = strstr(line_start, "\r\n")) != nullptr) {
            *line_end = '\0';
            irc_handle_message(client, line_start);
            line_start = line_end + 2;
        }

        // Move incomplete line to buffer start
        if (line_start != buffer) {
            size_t remaining = buffer_pos - (size_t)(line_start - buffer);
            if (remaining > 0) {
                memmove(buffer, line_start, remaining);
            }
            buffer_pos = remaining;
        }

        // Prevent buffer overflow
        if (buffer_pos >= sizeof(buffer) - 1) {
            buffer_pos = 0;
        }
    }

    irc_disconnect_socket(client);
    atomic_store(&client->running, false);
    irc_set_status(client, "Stopped");

    return nullptr;
}

irc_client_t *irc_client_create(host_t *host)
{
    if (host == nullptr) {
        return nullptr;
    }

    const char *server = irc_getenv("CHATTER_IRC_SERVER");
    const char *port_str = irc_getenv("CHATTER_IRC_PORT");
    const char *channel = irc_getenv("CHATTER_IRC_CHANNEL");
    const char *nickname = irc_getenv("CHATTER_IRC_NICKNAME");
    const char *username = irc_getenv("CHATTER_IRC_USERNAME");
    const char *realname = irc_getenv("CHATTER_IRC_REALNAME");

    // IRC is optional, return nullptr if not configured
    if (server == nullptr || channel == nullptr) {
        return nullptr;
    }

    irc_client_t *client = (irc_client_t *)calloc(1, sizeof(irc_client_t));
    if (client == nullptr) {
        return nullptr;
    }

    client->host = host;
    client->socket_fd = -1;
    atomic_init(&client->stop, false);
    atomic_init(&client->running, false);
    atomic_init(&client->disabled, false);
    atomic_init(&client->connected, false);

    snprintf(client->server_host, sizeof(client->server_host), "%s", server);
    client->server_port = (port_str != nullptr) ? atoi(port_str) : 6667;
    snprintf(client->channel, sizeof(client->channel), "%s", channel);
    snprintf(client->nickname, sizeof(client->nickname), "%s",
             nickname != nullptr ? nickname : "ssh-chatter");
    snprintf(client->username, sizeof(client->username), "%s",
             username != nullptr
                 ? username
                 : (nickname != nullptr ? nickname : "ssh-chatter"));
    snprintf(client->realname, sizeof(client->realname), "%s",
             realname != nullptr ? realname : "SSH-Chatter Bot");

    if (pthread_mutex_init(&client->lock, nullptr) != 0) {
        GC_FREE(client);
        return nullptr;
    }
    client->lock_initialized = true;

    irc_set_status(client, "Initializing");

    if (pthread_create(&client->thread, nullptr, irc_client_thread, client) !=
        0) {
        pthread_mutex_destroy(&client->lock);
        GC_FREE(client);
        return nullptr;
    }
    client->thread_initialized = true;

    return client;
}

void irc_client_destroy(irc_client_t *client)
{
    if (client == nullptr) {
        return;
    }

    atomic_store(&client->stop, true);

    if (client->thread_initialized) {
        pthread_join(client->thread, nullptr);
    }

    irc_disconnect_socket(client);

    if (client->lock_initialized) {
        pthread_mutex_destroy(&client->lock);
    }

    GC_FREE(client);
}

bool irc_client_is_connected(irc_client_t *client)
{
    if (client == nullptr) {
        return false;
    }
    return atomic_load(&client->connected);
}

const char *irc_client_get_status(irc_client_t *client)
{
    if (client == nullptr) {
        return "Not initialized";
    }
    return client->status_message;
}

bool irc_client_reconnect(irc_client_t *client)
{
    if (client == nullptr) {
        return false;
    }

    irc_disconnect_socket(client);
    atomic_store(&client->disabled, false);

    return irc_connect_socket(client);
}

void irc_client_disconnect(irc_client_t *client)
{
    if (client == nullptr) {
        return;
    }

    atomic_store(&client->disabled, true);
    irc_disconnect_socket(client);
}

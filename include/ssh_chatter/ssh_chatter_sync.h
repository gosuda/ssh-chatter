#ifndef SSH_CHATTER_SYNC_H
#define SSH_CHATTER_SYNC_H

#include <libssh/libssh.h>
#include <stdbool.h>
#include <time.h>

// Structure to hold synchronization settings
typedef struct {
    bool sync_in_enabled;
    bool sync_out_enabled;
    struct timespec last_sync_attempt;
    char go_chat_host[256];
    int go_chat_port;
    char go_chat_username[256];
    char go_chat_password
        [256]; // For simplicity, storing password directly. In a real app, use secure storage.
} sync_settings_t;

// Structure to represent a chat message
typedef struct chat_message_t {
    char *username;
    char *message_body;
    time_t timestamp;
    struct chat_message_t *next;
} chat_message_t;

// Function to add a message to the history
void ssh_chatter_sync_add_message_to_history(const chat_message_t *message);

// Function to get the last N messages from history
chat_message_t *ssh_chatter_sync_get_last_messages(int count);

// Function to free the chat message history
void ssh_chatter_sync_free_history();

// Initialize the synchronization module
void ssh_chatter_sync_init();

// Start the synchronization process
void ssh_chatter_sync_start();

// Stop the synchronization process
void ssh_chatter_sync_stop();

// Cleanup synchronization resources (history and mutex)
void ssh_chatter_sync_cleanup();

// Manually trigger synchronization
void ssh_chatter_sync_manual_trigger();

// Enable/disable incoming synchronization
void ssh_chatter_sync_set_in_enabled(bool enabled);

// Enable/disable outgoing synchronization
void ssh_chatter_sync_set_out_enabled(bool enabled);

// Get current synchronization settings
sync_settings_t ssh_chatter_sync_get_settings();

// Save synchronization settings to file
void ssh_chatter_sync_save_settings();

// Load synchronization settings from file
void ssh_chatter_sync_load_settings();

// Function to send a message to the Go chat
void ssh_chatter_sync_send_message(const char *message);

// Callback for receiving messages from Go chat (to be implemented in main.c or similar)
typedef void (*message_received_callback_t)(const chat_message_t *message);
void ssh_chatter_sync_set_message_received_callback(
    message_received_callback_t callback);

// Function to set SSH connection details at runtime
void ssh_chatter_sync_set_connection_details(const char *host, int port,
                                             const char *username,
                                             const char *password);

#endif // SSH_CHATTER_SYNC_H

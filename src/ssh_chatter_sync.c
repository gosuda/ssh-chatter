
/**
 * @file ssh_chatter_sync.c
 * @desc File-level documentation for ssh_chatter_sync.c, describing its role
 *       in the SSH-Chatter server and providing a consistent header
 *       comment format across C sources.
 * @return None.
 */

#define LIBSSH_STATIC
#include "ssh_chatter/ssh_chatter_sync.h"
#include "ssh_chatter/memory_manager.h"
#include <libssh/libssh.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

static ssh_session session = nullptr;
static ssh_channel channel = nullptr;
static pthread_t read_thread;
static pthread_t retry_thread;
static bool sync_running = false;
static bool retry_pending = false;
static message_received_callback_t msg_callback = nullptr;

#define MAX_CHAT_HISTORY_SIZE 50
#define RETRY_INTERVAL_SECONDS 5

static chat_message_t *chat_history_head = nullptr;
static int chat_history_size = 0;
static ttak_mutex_t history_mutex;
static bool history_mutex_initialized = false;

static sync_settings_t current_settings = {.sync_in_enabled = true,
                                           .sync_out_enabled = true,
                                           .last_sync_attempt = {0, 0}};

static void *read_channel_thread(void *arg);
static int authenticate_ssh_session(ssh_session sess);

static double timespec_elapsed_seconds(const struct timespec *start,
                                       const struct timespec *end)
{
    if (!start || !end)
        return 0.0;

    time_t sec = end->tv_sec - start->tv_sec;
    long nsec = end->tv_nsec - start->tv_nsec;
    if (nsec < 0) {
        sec--;
        nsec += 1000000000L;
    }
    if (sec < 0) {
        sec = 0;
        nsec = 0;
    }
    return (double)sec + (double)nsec / 1000000000.0;
}

static void *retry_connection_thread(void *arg)
{
    (void)arg;
    fprintf(stderr, "[SSH_SYNC] Retry thread started.\n\n");

    while (sync_running) {
        if (retry_pending) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = timespec_elapsed_seconds(
                &current_settings.last_sync_attempt, &now);

            if (elapsed >= RETRY_INTERVAL_SECONDS) {
                fprintf(stderr,
                        "[SSH_SYNC] Attempting to retry connection...\n\n");
                ssh_chatter_sync_start();
            }
        }
        sleep(10);
    }
    fprintf(stderr, "[SSH_SYNC] Retry thread stopped.\n\n");
    return nullptr;
}

void ssh_chatter_sync_add_message_to_history(const chat_message_t *new_msg)
{
    if (!history_mutex_initialized) {
        return;
    }

    ttak_mutex_lock(&history_mutex);

    if (chat_history_head &&
        strcmp(chat_history_head->message_body, new_msg->message_body) == 0 &&
        strcmp(chat_history_head->username, new_msg->username) == 0) {
        ttak_mutex_unlock(&history_mutex);
        return;
    }

    chat_message_t *node = sshc_gc_malloc(sizeof(chat_message_t));
    if (!node) {
        ttak_mutex_unlock(&history_mutex);
        return;
    }

    node->username = sshc_strdup(new_msg->username);
    node->message_body = sshc_strdup(new_msg->message_body);
    node->timestamp = new_msg->timestamp;
    node->next = chat_history_head;
    chat_history_head = node;
    chat_history_size++;

    if (chat_history_size > MAX_CHAT_HISTORY_SIZE) {
        chat_message_t *cur = chat_history_head;
        chat_message_t *prev = nullptr;
        for (int i = 0; i < MAX_CHAT_HISTORY_SIZE - 1 && cur; i++) {
            prev = cur;
            cur = cur->next;
        }
        if (prev && cur) {
#if !(defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC)
            sshc_gc_free(cur->username);
            sshc_gc_free(cur->message_body);
            sshc_gc_free(cur);
#else
            cur->username = nullptr;
            cur->message_body = nullptr;
            cur->next = nullptr;
#endif
            prev->next = nullptr;
            chat_history_size--;
        }
    }

    ttak_mutex_unlock(&history_mutex);
}

void ssh_chatter_sync_free_history()
{
    if (!history_mutex_initialized) {
        return;
    }

    // handling history can be dangerous: should lock process
    ttak_mutex_lock(&history_mutex);
    chat_message_t *cur = chat_history_head;
    while (cur) {
        chat_message_t *next = cur->next;
#if !(defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC)
        sshc_gc_free(cur->username);
        sshc_gc_free(cur->message_body);
        sshc_gc_free(cur);
#else
        cur->username = nullptr;     // no sshc_gc_free when manual memory management: set to nullptr 
        cur->message_body = nullptr;
        cur->next = nullptr;
#endif
        cur = next;
    }
    chat_history_head = nullptr;
    chat_history_size = 0;
    // tasks are done, unlock
    ttak_mutex_unlock(&history_mutex);
}

chat_message_t *ssh_chatter_sync_get_last_messages(int count)
{
    (void)count;
    return nullptr;
}

void ssh_chatter_sync_init()
{
    fprintf(stderr, "[SSH_SYNC] Initialized SSH Chatter Sync module.\n");
    if (!history_mutex_initialized) {
        ttak_mutex_init(&history_mutex);
        history_mutex_initialized = true;
    }
    ssh_chatter_sync_load_settings();

    if (pthread_create(&retry_thread, nullptr, retry_connection_thread,
                       nullptr) != 0) {
        fprintf(stderr, "[SSH_SYNC] Failed to create retry thread.\n\n");
    }
    pthread_detach(retry_thread);
}

void ssh_chatter_sync_start()
{
    if (sync_running)
        return;

    session = ssh_new();
    if (!session)
        return;

    ssh_options_set(session, SSH_OPTIONS_HOST, current_settings.go_chat_host);
    ssh_options_set(session, SSH_OPTIONS_PORT, &current_settings.go_chat_port);

    fprintf(stderr, "[SSH_SYNC] Connecting.\n");
    int rc = ssh_connect(session);
    if (rc != SSH_OK) {
        ssh_free(session);
        session = nullptr;
        clock_gettime(CLOCK_MONOTONIC, &current_settings.last_sync_attempt);
        retry_pending = true;
        ssh_chatter_sync_save_settings();
        return;
    }

    if (authenticate_ssh_session(session) != SSH_OK) {
        ssh_disconnect(session);
        ssh_free(session);
        session = nullptr;
        clock_gettime(CLOCK_MONOTONIC, &current_settings.last_sync_attempt);
        retry_pending = true;
        ssh_chatter_sync_save_settings();
        return;
    }

    channel = ssh_channel_new(session);
    if (!channel)
        goto fail_exit;

    if (ssh_channel_open_session(channel) != SSH_OK)
        goto fail_exit;

    if (ssh_channel_request_pty(channel) != SSH_OK)
        goto fail_exit;

    if (ssh_channel_request_shell(channel) != SSH_OK)
        goto fail_exit;

    sync_running = true;
    retry_pending = false;
    clock_gettime(CLOCK_MONOTONIC, &current_settings.last_sync_attempt);
    ssh_chatter_sync_save_settings();

    if (pthread_create(&read_thread, nullptr, read_channel_thread, nullptr) !=
        0)
        goto fail_exit;

    fprintf(stderr, "[SSH_SYNC] Synchronization started.\n\n");
    return;

fail_exit:
    if (channel) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        channel = nullptr;
    }
    if (session) {
        ssh_disconnect(session);
        ssh_free(session);
        session = nullptr;
    }
}

void ssh_chatter_sync_stop()
{
    if (!sync_running)
        return;

    sync_running = false;
    retry_pending = false;

    if (read_thread)
        pthread_join(read_thread, nullptr);

    if (channel) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        channel = nullptr;
    }
    if (session) {
        ssh_disconnect(session);
        ssh_free(session);
        session = nullptr;
    }

    ssh_chatter_sync_free_history();
}

void ssh_chatter_sync_cleanup()
{
    if (!history_mutex_initialized) {
        return;
    }

    ssh_chatter_sync_free_history();
    ttak_mutex_destroy(&history_mutex);
    history_mutex_initialized = false;
}

void ssh_chatter_sync_manual_trigger()
{
    if (sync_running)
        ssh_chatter_sync_stop();
    ssh_chatter_sync_start();
}

void ssh_chatter_sync_set_in_enabled(bool enabled)
{
    current_settings.sync_in_enabled = enabled;
    ssh_chatter_sync_save_settings();
}

void ssh_chatter_sync_set_out_enabled(bool enabled)
{
    current_settings.sync_out_enabled = enabled;
    ssh_chatter_sync_save_settings();
}

sync_settings_t ssh_chatter_sync_get_settings()
{
    return current_settings;
}

void ssh_chatter_sync_save_settings()
{
    FILE *fp = fopen("ssh_chatter_sync.dat", "wb");
    if (fp) {
        sync_settings_t s = current_settings;
        s.last_sync_attempt.tv_nsec = 0;
        fwrite(&s, sizeof(sync_settings_t), 1, fp);
        fclose(fp);
    }
}

void ssh_chatter_sync_load_settings()
{
    FILE *fp = fopen("ssh_chatter_sync.dat", "rb");
    if (fp) {
        size_t read_size =
            fread(&current_settings, sizeof(sync_settings_t), 1, fp);
        if (read_size < 1) {
            fprintf(stderr, "[SSH_SYNC] Error reading settings from file, "
                            "using defaults.\n\n");
            current_settings.sync_in_enabled = true;
            current_settings.sync_out_enabled = true;
            current_settings.last_sync_attempt.tv_sec = 0;
            current_settings.last_sync_attempt.tv_nsec = 0;
            memset(current_settings.go_chat_host, 0,
                   sizeof(current_settings.go_chat_host));
            memset(current_settings.go_chat_username, 0,
                   sizeof(current_settings.go_chat_username));
            memset(current_settings.go_chat_password, 0,
                   sizeof(current_settings.go_chat_password));
            current_settings.go_chat_port = 2022;
        }
        fclose(fp);
        return;
    }

    strncpy(current_settings.go_chat_host, "127.0.0.1",
            sizeof(current_settings.go_chat_host) - 1);
    current_settings.go_chat_port = 2022;
    strncpy(current_settings.go_chat_username, "chatter_sync",
            sizeof(current_settings.go_chat_username) - 1);
    strncpy(current_settings.go_chat_password, "password",
            sizeof(current_settings.go_chat_password) - 1);
}

void ssh_chatter_sync_send_message(const char *message)
{
    if (!sync_running || !channel || !current_settings.sync_out_enabled)
        return;

    size_t len = strlen(message) + 2;
    char *msg = sshc_gc_malloc(len);
    if (!msg)
        return;

    snprintf(msg, len, "%s\n", message);

    ssh_channel_write(channel, msg, (uint32_t)strlen(msg));
    sshc_gc_free(msg);
}

void ssh_chatter_sync_set_message_received_callback(
    message_received_callback_t callback)
{
    msg_callback = callback;
}

static void *read_channel_thread(void *arg)
{
    (void)arg;
    char buffer[256];
    int nbytes;
    bool should_reconnect = false;

    fprintf(stderr, "[SSH_SYNC] Read thread started.\n\n");

    while (sync_running && channel) {
        nbytes = ssh_channel_read_nonblocking(
            channel, buffer, sizeof(buffer) - 1U, 0);
        if (nbytes < 0) {
            should_reconnect = true;
            break;
        }
        if (nbytes == 0 && ssh_channel_is_eof(channel)) {
            should_reconnect = true;
            break;
        }

        if (nbytes > 0) {
            buffer[nbytes] = '\0';
            char *line = buffer;
            char *next;
            while ((next = strchr(line, '\n')) != nullptr) {
                *next = '\0';

                char *trim = line;
                while (*trim == ' ' || *trim == '\t' || *trim == '\r')
                    trim++;
                size_t trimmed_len = strlen(trim);
                while (trimmed_len > 0) {
                    char last = trim[trimmed_len - 1];
                    if (last != ' ' && last != '\t' && last != '\r') {
                        break;
                    }
                    trim[trimmed_len - 1] = '\0';
                    --trimmed_len;
                }

                if (*trim) {
                    char *colon = strstr(trim, ": ");
                    if (colon && msg_callback &&
                        current_settings.sync_in_enabled) {
                        *colon = '\0';
                        chat_message_t msg;
                        msg.username = trim;
                        msg.message_body = colon + 2;
                        msg.timestamp = time(nullptr);
                        ssh_chatter_sync_add_message_to_history(&msg);
                        msg_callback(&msg);
                    }
                }
                line = next + 1;
            }
        }

        usleep(100000);
    }

    sync_running = false;
    fprintf(stderr, "[SSH_SYNC] Read thread stopped.\n\n");

    if (should_reconnect) {
        if (channel) {
            ssh_channel_close(channel);
            ssh_channel_free(channel);
            channel = nullptr;
        }
        if (session) {
            ssh_disconnect(session);
            ssh_free(session);
            session = nullptr;
        }
        retry_pending = true;
        clock_gettime(CLOCK_MONOTONIC, &current_settings.last_sync_attempt);
    }

    return nullptr;
}

static int authenticate_ssh_session(ssh_session sess)
{
    (void)sess;
    return SSH_OK;
}

void ssh_chatter_sync_set_connection_details(const char *host, int port,
                                             const char *username,
                                             const char *password)
{
    if (host) {
        strncpy(current_settings.go_chat_host, host,
                sizeof(current_settings.go_chat_host) - 1);
    }
    current_settings.go_chat_port = port;
    if (username) {
        strncpy(current_settings.go_chat_username, username,
                sizeof(current_settings.go_chat_username) - 1);
    }
    if (password) {
        strncpy(current_settings.go_chat_password, password,
                sizeof(current_settings.go_chat_password) - 1);
    }
    ssh_chatter_sync_save_settings();
}

#include "ssh_chatter/host.h"
#include "ssh_chatter/memory_manager.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct stress_client {
    session_ctx_t *ctx;
    size_t id;
    size_t rounds;
} stress_client_t;

static size_t parse_size_arg(const char *text, size_t fallback)
{
    if (text == nullptr || text[0] == '\0') {
        return fallback;
    }

    char *endptr = nullptr;
    unsigned long parsed = strtoul(text, &endptr, 10);
    if (endptr == text || *endptr != '\0') {
        return fallback;
    }
    if (parsed == 0UL || parsed > SIZE_MAX) {
        return fallback;
    }
    return (size_t)parsed;
}

static void *stress_client_thread(void *arg)
{
    stress_client_t *client = (stress_client_t *)arg;
    char buffer[SSH_CHATTER_MAX_INPUT_LEN];
    const struct timespec pause = {.tv_sec = 1, .tv_nsec = 0};

    for (size_t round = 0; round < client->rounds; ++round) {
        if ((round % 2U) == 0U) {
            snprintf(buffer, sizeof(buffer), "load message %zu-%zu", client->id,
                     round);
        } else if ((round % 4U) == 1U) {
            snprintf(buffer, sizeof(buffer), "/usercount");
        } else {
            snprintf(buffer, sizeof(buffer), "/today");
        }

        host_session_process_line_for_testing(client->ctx, buffer);
        nanosleep(&pause, nullptr);
    }

    return nullptr;
}

int main(int argc, char **argv)
{
    size_t client_count = 1000U;
    size_t rounds = 3U;

    if (argc > 1) {
        client_count = parse_size_arg(argv[1], client_count);
    }
    if (argc > 2) {
        rounds = parse_size_arg(argv[2], rounds);
    }

    sshc_memory_runtime_init();

    host_t *host = (host_t *)calloc(1U, sizeof(*host));
    if (host == nullptr) {
        fprintf(stderr, "failed to allocate host\n");
        sshc_memory_runtime_shutdown();
        return EXIT_FAILURE;
    }
    volatile sig_atomic_t shutdown_flag = 0;
    host->shutdown_flag = &shutdown_flag;
    host->memory_context = sshc_memory_context_create("stress-host");
    if (host->memory_context == nullptr) {
        fprintf(stderr, "failed to create host memory context\n");
        free(host);
        sshc_memory_runtime_shutdown();
        return EXIT_FAILURE;
    }

    auth_profile_t auth = {0};
    sshc_memory_context_t *init_scope =
        sshc_memory_context_push(host->memory_context);
    host_init(host, &auth);
    if (init_scope != nullptr) {
        sshc_memory_context_pop(init_scope);
    }

    session_ctx_t **sessions =
        (session_ctx_t **)calloc(client_count, sizeof(*sessions));
    pthread_t *threads = (pthread_t *)calloc(client_count, sizeof(*threads));
    stress_client_t *clients =
        (stress_client_t *)calloc(client_count, sizeof(*clients));

    int exit_code = EXIT_SUCCESS;
    size_t created_sessions = 0U;
    size_t launched_threads = 0U;

    if (sessions == nullptr || threads == nullptr || clients == nullptr) {
        fprintf(stderr, "failed to allocate stress test structures\n");
        exit_code = EXIT_FAILURE;
        goto cleanup;
    }

    for (; created_sessions < client_count; ++created_sessions) {
        char username[SSH_CHATTER_USERNAME_LEN];
        snprintf(username, sizeof(username), "stress%zu", created_sessions);
        char ip[SSH_CHATTER_IP_LEN];
        snprintf(ip, sizeof(ip), "192.0.2.%zu", (created_sessions % 254U) + 1U);

        sessions[created_sessions] =
            host_session_create_for_testing(host, username, ip, false);
        if (sessions[created_sessions] == nullptr) {
            fprintf(stderr, "failed to prepare session %zu\n",
                    created_sessions);
            exit_code = EXIT_FAILURE;
            break;
        }

        clients[created_sessions].ctx = sessions[created_sessions];
        clients[created_sessions].id = created_sessions;
        clients[created_sessions].rounds = rounds;

        if (pthread_create(&threads[created_sessions], nullptr,
                           stress_client_thread,
                           &clients[created_sessions]) != 0) {
            fprintf(stderr, "failed to launch thread %zu\n", created_sessions);
            exit_code = EXIT_FAILURE;
            break;
        }
        ++launched_threads;
    }

cleanup:
    for (size_t idx = 0U; idx < launched_threads; ++idx) {
        pthread_join(threads[idx], nullptr);
    }

    for (size_t idx = 0U; idx < created_sessions; ++idx) {
        if (sessions[idx] != nullptr) {
            host_session_destroy_for_testing(sessions[idx]);
        }
    }

    host_shutdown_for_testing(host);
    sshc_memory_context_destroy(host->memory_context);
    sshc_memory_runtime_shutdown();

    free(host);
    free(clients);
    free(threads);
    free(sessions);

    return exit_code;
}

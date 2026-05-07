/**
 * @file main.c
 * @desc File-level documentation for main.c, describing its role
 *       in the SSH-Chatter server and providing a consistent header
 *       comment format across C sources.
 * @return None.
 */

#define _POSIX_C_SOURCE 200809L
#include "ssh_chatter/host.h"
#include "ssh_chatter/humanized/humanized.h"
#include "ssh_chatter/ssh_chatter_sync.h"
#include "ssh_chatter/user_data.h"
#include "ssh_chatter/memory_manager.h"
#include "ssh_chatter/translator.h"
#include "ssh_chatter/ssh_chatter_backend.h" // backend functions that is external should be located here

#include <libssh/libssh.h>

#include <errno.h>
#include <getopt.h>

#include <sys/types.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <locale.h>
#include <time.h>
#include <signal.h>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#define HOST_STABLE_RESET_SECONDS 10.0
#define SSH_CHATTER_MAX_HOST_RESTARTS 2U

static volatile sig_atomic_t g_shutdown_flag = 0;
static char *g_welcome_banner_content = nullptr;
static bool g_sync_initialized = false;

static void signal_handler(int signum)
{
    (void)signum;
    fprintf(stderr, "Received signal %d\n", signum);
    static const char message[] =
        "[signal] Received shutdown signal, setting shutdown flag\n";
    long ret = write(STDERR_FILENO, message, sizeof(message) - 1U);
    if(ret < 0) {
        fprintf(stderr,"%s",  message);
    }
    g_shutdown_flag = 1;
}

static void print_usage(const char *prog_name)
{
    fprintf(stderr,

            "Usage: %s [-a address] [-p port] [-m motd_file] [-k host_key_dir] "
            "[-T telnet_port|off] [-J json_port|off]\n",

            prog_name);
}

static double timespec_elapsed_seconds(const struct timespec *start,
                                       const struct timespec *end)
{
    if (start == nullptr || end == nullptr) {
        return 0.0;
    }

    time_t sec = end->tv_sec - start->tv_sec;

    long nsec = end->tv_nsec - start->tv_nsec;

    if (nsec < 0L) {
        --sec;

        nsec += 1000000000L;
    }

    if (sec < 0) {
        sec = 0;

        nsec = 0L;
    }

    return (double)sec + (double)nsec / 1000000000.0;
}

static void sleep_before_restart(unsigned int attempts)
{
    struct timespec restart_delay = {

        .tv_sec = attempts < 5U ? 1L : (attempts < 10U ? 5L : 30L),

        .tv_nsec = 0L,

    };

    struct timespec request = restart_delay;

    struct timespec remaining = {0};

    while (nanosleep(&request, &remaining) != 0) {
        if (errno != EINTR) {
            break;
        }

        request = remaining;
    }
}

static void daemon_extreme_gc_collect(void)
{
    for (int pass = 0; pass < 4; ++pass) {
        sshc_epoch_reclaim();
        sshc_epoch_reclaim();
#if defined(__GLIBC__)
        malloc_trim(0);
#endif

        struct timespec pause = {
            .tv_sec = 0,
            .tv_nsec = 20 * 1000 * 1000L,
        };
        struct timespec remaining = {0};
        while (nanosleep(&pause, &remaining) != 0) {
            if (errno != EINTR) {
                break;
            }
            pause = remaining;
        }
    }
}

int main(int argc, char **argv)
{
    int exit_code = EXIT_SUCCESS;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sa.sa_flags =
        0; // Do NOT use SA_RESTART - we want signals to interrupt accept()
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    setlocale(LC_ALL, "");

    const char *bind_address = nullptr;

    const char *bind_port = nullptr;

    const char *motd = nullptr;

    const char *host_key_dir = nullptr;

    const char *telnet_port = "2323";
    const char *json_port = "34567";

    bool telnet_enabled = true;
    bool json_enabled = true;

    char telnet_bind_storage[64];

    telnet_bind_storage[0] = '\0';

    bool telnet_bind_overridden = false;

    char telnet_port_storage[16];

    telnet_port_storage[0] = '\0';

    bool json_bind_overridden = false;
    char json_bind_storage[64];
    json_bind_storage[0] = '\0';
    char json_port_storage[16];
    json_port_storage[0] = '\0';

    int opt = 0;

    bool show_usage = false;
    bool show_version = false;

    while ((opt = getopt(argc, argv, "a:p:m:k:T:J:hV")) != -1) {
        switch (opt) {
        case 'a':

            bind_address = optarg;

            break;

        case 'p':

            bind_port = optarg;

            break;

        case 'm':

            motd = optarg;

            break;

        case 'k':

            host_key_dir = optarg;

            break;

        case 'T':

            if (optarg != nullptr &&

                (strcmp(optarg, "off") == 0 || strcmp(optarg, "disable") == 0 ||
                 strcmp(optarg, "none") == 0)) {
                telnet_enabled = false;

                telnet_port = nullptr;

                telnet_bind_overridden = false;

            } else if (optarg != nullptr) {
                const char *value = optarg;

                const char *colon = strchr(value, ':');

                if (colon != nullptr) {
                    size_t host_len = (size_t)(colon - value);

                    if (host_len >= sizeof(telnet_bind_storage)) {
                        fprintf(stderr,

                                "telnet bind address is too long; ignoring "
                                "override and "
                                "using default listener address\n");

                        telnet_bind_storage[0] = '\0';

                        telnet_bind_overridden = false;

                    } else if (host_len > 0U) {
                        memcpy(telnet_bind_storage, value, host_len);

                        telnet_bind_storage[host_len] = '\0';

                        telnet_bind_overridden = true;

                    } else {
                        telnet_bind_overridden = false;
                    }

                    const char *port_part = colon + 1;

                    if (port_part[0] == '\0') {
                        telnet_port = "2323";

                    } else {
                        size_t port_len = strlen(port_part);

                        if (port_len >= sizeof(telnet_port_storage)) {
                            fprintf(stderr, "telnet port is too long; using "
                                            "default port 2323\n");

                            telnet_port = "2323";

                        } else {
                            memcpy(telnet_port_storage, port_part,
                                   port_len + 1);

                            telnet_port = telnet_port_storage;
                        }
                    }

                } else {
                    telnet_bind_overridden = false;

                    if (value[0] == '\0') {
                        telnet_port = "2323";

                    } else {
                        size_t port_len = strlen(value);

                        if (port_len >= sizeof(telnet_port_storage)) {
                            fprintf(stderr, "telnet port is too long; using "
                                            "default port 2323\n");

                            telnet_port = "2323";

                        } else {
                            memcpy(telnet_port_storage, value, port_len + 1);

                            telnet_port = telnet_port_storage;
                        }
                    }
                }

                telnet_enabled = true;
            }

            break;
        case 'J':

            if (optarg != nullptr &&

                (strcmp(optarg, "off") == 0 || strcmp(optarg, "disable") == 0 ||
                 strcmp(optarg, "none") == 0)) {
                json_enabled = false;

                json_port = nullptr;

                json_bind_overridden = false;

            } else if (optarg != nullptr) {
                const char *value = optarg;

                const char *colon = strchr(value, ':');

                if (colon != nullptr) {
                    size_t host_len = (size_t)(colon - value);

                    if (host_len >= sizeof(json_bind_storage)) {
                        fprintf(stderr,

                                "json bind address is too long; ignoring "
                                "override and using default listener "
                                "address\n");

                        json_bind_storage[0] = '\0';

                        json_bind_overridden = false;

                    } else if (host_len > 0U) {
                        memcpy(json_bind_storage, value, host_len);

                        json_bind_storage[host_len] = '\0';

                        json_bind_overridden = true;

                    } else {
                        json_bind_overridden = false;
                    }

                    const char *port_part = colon + 1;

                    if (port_part[0] == '\0') {
                        json_port = "34567";

                    } else {
                        size_t port_len = strlen(port_part);

                        if (port_len >= sizeof(json_port_storage)) {
                            fprintf(stderr,
                                    "json port is too long; using default "
                                    "port 34567\n");

                            json_port = "34567";

                        } else {
                            memcpy(json_port_storage, port_part,
                                   port_len + 1);

                            json_port = json_port_storage;
                        }
                    }

                } else {
                    json_bind_overridden = false;

                    if (value[0] == '\0') {
                        json_port = "34567";

                    } else {
                        size_t port_len = strlen(value);

                        if (port_len >= sizeof(json_port_storage)) {
                            fprintf(stderr,
                                    "json port is too long; using default "
                                    "port 34567\n");

                            json_port = "34567";

                        } else {
                            memcpy(json_port_storage, value, port_len + 1);

                            json_port = json_port_storage;
                        }
                    }
                }
            }

            break;

        case 'h':
            show_usage = true;
            break;

        case 'V':
            show_version = true;
            break;

        default:
            show_usage = true;
            exit_code = EXIT_FAILURE;
            break;
        }
    }

    sshc_gc_init();

    if (show_usage) {
        print_usage(argv[0]);
        goto cleanup;
    }

    if (show_version) {
        printf("ssh-chatter (C)\n");
        goto cleanup;
    }

    ssh_chatter_sync_init(); // Initialize SSH Chatter Sync module
    g_sync_initialized = true;

    if (!telnet_enabled) {
        telnet_port = nullptr;

        telnet_bind_overridden = false;

    } else if (telnet_port != nullptr && telnet_port[0] == '\0') {
        telnet_port = "2323";
    }

    const char *telnet_bind_address =
        telnet_bind_overridden ? telnet_bind_storage : nullptr;

    if (!json_enabled) {
        json_port = nullptr;

        json_bind_overridden = false;

    } else if (json_port != nullptr && json_port[0] == '\0') {
        json_port = "34567";
    }

    const char *json_bind_address =
        json_bind_overridden ? json_bind_storage : nullptr;

    auth_profile_t default_profile = {0};

    unsigned int restart_attempts = 0U;

    while (!g_shutdown_flag) {
        host_t *host = sshc_gc_calloc(1U, sizeof(*host));

        if (host == nullptr) {
            ++restart_attempts;

            humanized_log_error("daemon", "failed to allocate host state",
                                errno != 0 ? errno : ENOMEM);

            if (restart_attempts > SSH_CHATTER_MAX_HOST_RESTARTS) {
                printf("[daemon] host startup failed %u times; exiting\n",
                       SSH_CHATTER_MAX_HOST_RESTARTS);
                exit_code = EXIT_FAILURE;
                goto cleanup;
            }

            printf("[daemon] retrying host startup (attempt %u)\n",
                   restart_attempts);

            sleep_before_restart(restart_attempts);

            continue;
        }

        host->memory_context = sshc_memory_context_create("host");

        if (host->memory_context == nullptr) {
            ++restart_attempts;

            humanized_log_error("daemon",
                                "failed to create host memory context",
                                errno != 0 ? errno : ENOMEM);

            if (restart_attempts > SSH_CHATTER_MAX_HOST_RESTARTS) {
                printf("[daemon] host startup failed %u times; exiting\n",
                       SSH_CHATTER_MAX_HOST_RESTARTS);
                sshc_gc_free(host);
                exit_code = EXIT_FAILURE;
                goto cleanup;
            }

            printf("[daemon] retrying host startup (attempt %u)\n",
                   restart_attempts);

            sshc_gc_free(host);

            sleep_before_restart(restart_attempts);

            continue;
        }

        sshc_memory_context_t *init_scope =
            sshc_memory_context_push(host->memory_context);

        host->shutdown_flag = &g_shutdown_flag;
        host_init(host, &default_profile);

        sshc_memory_context_pop(init_scope);

        if (motd != nullptr) {
            sshc_memory_context_t *motd_scope =
                sshc_memory_context_push(host->memory_context);

            host_set_motd(host, motd);

            sshc_memory_context_pop(motd_scope);
        }

        const char *address =
            bind_address != nullptr ? bind_address : "0.0.0.0";

        const char *port = bind_port != nullptr ? bind_port : "2222";

        printf("Starting ssh-chatter on %s:%s\n", address, port);

        struct timespec serve_start;

        clock_gettime(CLOCK_MONOTONIC, &serve_start);

        errno = 0;

        sshc_memory_context_t *serve_scope =
            sshc_memory_context_push(host->memory_context);

        const char *welcome_banner_path = getenv("CHATTER_WELCOME_BANNER");
        if (g_welcome_banner_content != nullptr) {
            sshc_gc_free(g_welcome_banner_content);
            g_welcome_banner_content = nullptr;
        }
        g_welcome_banner_content =
            session_show_welcome_banner(welcome_banner_path);
        if (g_welcome_banner_content == nullptr) {
            // Optionally log an error if banner path is set but file is not found/readable
            if (welcome_banner_path != nullptr) {
                fprintf(
                    stderr,
                    "[main] Warning: Could not load welcome banner from %s\n",
                    welcome_banner_path);
            }
        } else {
            host_set_welcome_banner(host, g_welcome_banner_content);
        }

        const int serve_result =
            host_serve(host, bind_address, bind_port, host_key_dir,
                       telnet_bind_address, telnet_port, json_bind_address,
                       json_port);
        const bool force_restart_requested = host->force_restart_requested;

        const int serve_errno = errno;

        sshc_memory_context_pop(serve_scope);

        struct timespec serve_end;

        clock_gettime(CLOCK_MONOTONIC, &serve_end);

        sshc_memory_context_t *shutdown_scope =
            sshc_memory_context_push(host->memory_context);

        host_shutdown(host);

        sshc_memory_context_pop(shutdown_scope);

        sshc_memory_context_destroy(host->memory_context);

        host->memory_context = nullptr;

        sshc_gc_free(host);

        host = nullptr;

        if (force_restart_requested) {
            printf("[daemon] memory pressure cleanup complete; restarting "
                   "listeners without exiting the process\n");
            daemon_extreme_gc_collect();
            g_shutdown_flag = 0;
            restart_attempts = 0U;
            continue;
        }

        if (g_shutdown_flag) {
            printf("[daemon] shutdown signal received, exiting gracefully\n");
            goto cleanup;
        }

        if (serve_result == 0) {
            printf("[daemon] host_serve returned 0 without shutdown signal, "
                   "treating as unexpected exit\n");
        }

        double runtime_seconds =
            timespec_elapsed_seconds(&serve_start, &serve_end);

        bool skip_restart_delay = false;

        if (runtime_seconds >= HOST_STABLE_RESET_SECONDS) {
            if (restart_attempts > 0U) {
                printf("[daemon] host ran for %.3f seconds; clearing restart "
                       "backoff\n",
                       runtime_seconds);
            }

            restart_attempts = 0U;

            skip_restart_delay = true;
        }

        ++restart_attempts;

        char detail[128];

        if (serve_result != 0) {
            snprintf(detail, sizeof(detail), "host_serve failed (code %d)",
                     serve_result);

        } else {
            snprintf(detail, sizeof(detail),
                     "host_serve returned unexpectedly");
        }

        humanized_log_error("daemon", detail,
                            serve_errno != 0 ? serve_errno : EIO);

        if (restart_attempts > SSH_CHATTER_MAX_HOST_RESTARTS) {
            printf("[daemon] host restart limit (%u) reached; exiting\n",
                   SSH_CHATTER_MAX_HOST_RESTARTS);
            exit_code = EXIT_FAILURE;
            goto cleanup;
        }

        printf("[daemon] restarting ssh-chatter (attempt %u)\n",
               restart_attempts);

        if (!skip_restart_delay) {
            sleep_before_restart(restart_attempts);
        }
    }

cleanup:
    if (g_welcome_banner_content != nullptr) {
        sshc_gc_free(g_welcome_banner_content);
        g_welcome_banner_content = nullptr;
    }
    if (g_sync_initialized) {
        ssh_chatter_sync_stop();
        ssh_chatter_sync_cleanup();
    }
    translator_global_cleanup();
    sshc_memory_runtime_shutdown();

    return exit_code;
}

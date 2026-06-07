int host_serve(host_t *host, const char *bind_addr, const char *port,
               const char *key_directory, const char *telnet_bind_addr,
               const char *telnet_port, const char *json_bind_addr,
               const char *json_port)
{
    if (host == nullptr) {
        return -1;
    }

    const char *address =
        (bind_addr != nullptr && bind_addr[0] != '\0') ? bind_addr : "0.0.0.0";
    const char *bind_port =
        (port != nullptr && port[0] != '\0') ? port : "2222";
    const char *telnet_bind = nullptr;
    if (telnet_bind_addr != nullptr) {
        telnet_bind = telnet_bind_addr;
    } else if (bind_addr != nullptr && bind_addr[0] != '\0') {
        telnet_bind = bind_addr;
    } else {
        telnet_bind = address;
    }
    if (telnet_port != nullptr && telnet_port[0] != '\0') {
        if (!host_telnet_listener_start(host, telnet_bind, telnet_port)) {
            const char *display_addr =
                (telnet_bind != nullptr && telnet_bind[0] != '\0') ? telnet_bind
                                                                   : "*";
            printf("[telnet] telnet listener unavailable on %s:%s\n",
                   display_addr, telnet_port);
        }
    } else {
        host_telnet_listener_stop(host);
    }
    const char *json_bind = nullptr;
    if (json_bind_addr != nullptr) {
        json_bind = json_bind_addr;
    } else if (bind_addr != nullptr && bind_addr[0] != '\0') {
        json_bind = bind_addr;
    } else {
        json_bind = address;
    }
    if (json_port != nullptr && json_port[0] != '\0') {
        if (!host_json_api_listener_start(host, json_bind, json_port)) {
            const char *display_addr =
                (json_bind != nullptr && json_bind[0] != '\0') ? json_bind : "*";
            printf("[json-api] listener unavailable on %s:%s\n", display_addr,
                   json_port);
        }
    } else {
        host_json_api_listener_stop(host);
    }
    host_register_protected_bind_address(host, address);
    host_register_protected_bind_address(host, telnet_bind);
    host_register_protected_bind_address(host, json_bind);
    const bool key_dir_specified =
        key_directory != nullptr && key_directory[0] != '\0';
    const host_key_definition_t host_key_definitions[] = {
        {"ssh-ed25519", "ssh_host_ed25519_key", SSH_BIND_OPTIONS_IMPORT_KEY,
         true},
        {"ecdsa-sha2-nistp256", "ssh_host_ecdsa_key", SSH_BIND_OPTIONS_ECDSAKEY,
         false},
        {"ssh-rsa", "ssh_host_rsa_key", SSH_BIND_OPTIONS_RSAKEY, false},
    };
    const size_t host_key_count =
        sizeof(host_key_definitions) / sizeof(host_key_definitions[0]);

    while (host->shutdown_flag == nullptr || *host->shutdown_flag == 0) {
        ssh_bind bind_handle = ssh_bind_new();
        bool restart_listener = false;
        if (bind_handle == nullptr) {
            humanized_log_error("host", "failed to allocate ssh_bind", ENOMEM);
            host_sleep_after_error(host);
            continue;
        }

        ssh_key imported_keys[host_key_count];
        memset(imported_keys, 0, sizeof(imported_keys));

        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_BINDADDR, address);
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_BINDPORT_STR,
                             bind_port);

        bool fatal_key_error = false;
        bool key_loaded = false;
        char preferred_algorithm[64];
        preferred_algorithm[0] = '\0';
        char algorithm_buffer[256];
        algorithm_buffer[0] = '\0';
        size_t algorithm_length = 0U;

        for (size_t idx = 0; idx < host_key_count; ++idx) {
            const host_key_definition_t *definition =
                &host_key_definitions[idx];
            char key_path[PATH_MAX];
            bool path_valid = false;
            char custom_candidate[PATH_MAX];
            bool attempted_custom = false;

            if (key_dir_specified) {
                attempted_custom = true;
                if (!host_join_key_path(key_directory, definition->filename,
                                        custom_candidate,
                                        sizeof(custom_candidate))) {
                    humanized_log_error("host",
                                        "host key directory path is too long",
                                        ENAMETOOLONG);
                    fatal_key_error = true;
                    break;
                }
                if (access(custom_candidate, R_OK) == 0) {
                    snprintf(key_path, sizeof(key_path), "%s",
                             custom_candidate);
                    path_valid = true;
                }
            }

            if (!path_valid) {
                if (access(definition->filename, R_OK) == 0) {
                    snprintf(key_path, sizeof(key_path), "%s",
                             definition->filename);
                    path_valid = true;
                } else {
                    char fallback_path[PATH_MAX];
                    const int written =
                        snprintf(fallback_path, sizeof(fallback_path),
                                 "/etc/ssh/%s", definition->filename);
                    if (written >= 0 &&
                        (size_t)written < sizeof(fallback_path) &&
                        access(fallback_path, R_OK) == 0) {
                        snprintf(key_path, sizeof(key_path), "%s",
                                 fallback_path);
                        path_valid = true;
                    }
                }
            }

            if (!path_valid) {
                if (attempted_custom) {
                    printf(
                        "[listener] %s host key not found at %s (skipping)\n",
                        definition->algorithm, custom_candidate);
                }
                continue;
            }

            if (!host_bind_load_key(bind_handle, definition, key_path,
                                    &imported_keys[idx])) {
                continue;
            }

            if (!key_loaded) {
                snprintf(preferred_algorithm, sizeof(preferred_algorithm), "%s",
                         definition->algorithm);
                key_loaded = true;
            }

            host_bind_append_algorithm(
                algorithm_buffer, sizeof(algorithm_buffer), &algorithm_length,
                definition->algorithm);
        }

        if (fatal_key_error) {
            goto loop_cleanup;
        }

        if (algorithm_length == 0U) {
            humanized_log_error("host", "no host keys configured", 0);
            goto loop_cleanup;
        }

        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_HOSTKEY_ALGORITHMS,
                             algorithm_buffer);
        if (preferred_algorithm[0] != '\0') {
            ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_HOSTKEY,
                                 preferred_algorithm);
        }

        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_KEY_EXCHANGE,
                             SSH_CHATTER_SUPPORTED_KEX_ALGORITHMS);
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_CIPHERS_C_S,
                             SSH_CHATTER_STRONG_CIPHERS);
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_CIPHERS_S_C,
                             SSH_CHATTER_STRONG_CIPHERS);
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_HMAC_C_S,
                             SSH_CHATTER_STRONG_MACS);
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_HMAC_S_C,
                             SSH_CHATTER_STRONG_MACS);
#ifdef SSH_BIND_OPTIONS_COMPRESSION_C_S
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_COMPRESSION_C_S,
                             SSH_CHATTER_SECURE_COMPRESSION);
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_COMPRESSION_S_C,
                             SSH_CHATTER_SECURE_COMPRESSION);
#endif

        if (ssh_bind_listen(bind_handle) < 0) {
            humanized_log_error("host", ssh_get_error(bind_handle), EIO);
            goto loop_cleanup;
        }

        host->listener.handle = bind_handle;
        host->listener.accept_error_streak = 0U;
        host->listener.last_error_time.tv_sec = 0;
        host->listener.last_error_time.tv_nsec = 0L;
        printf("[listener] listening on %s:%s\n", address, bind_port);
        host_error_guard_register_success(host);

        // Retrieve the bind socket fd for poll()-based accept gating
        socket_t bind_fd = ssh_bind_get_fd(bind_handle);
        unsigned int idle_poll_cycles = 0U;
        struct timespec last_gc_run = {0};
        struct timespec last_pressure_check = {0};
        clock_gettime(CLOCK_MONOTONIC, &last_gc_run);
        last_pressure_check = last_gc_run;
        struct timespec last_idle_check = last_gc_run;

        while (!restart_listener &&
               (host->shutdown_flag == nullptr || *host->shutdown_flag == 0)) {

            // Gate accept() with poll() so we never block indefinitely.
            // This lets us check shutdown flags and run health probes.
            if (bind_fd >= 0) {
                struct pollfd pfd;
                pfd.fd = bind_fd;
                pfd.events = POLLIN;
                pfd.revents = 0;

                int poll_rc =
                    poll(&pfd, 1, SSH_CHATTER_ACCEPT_POLL_TIMEOUT_MS);
                if (poll_rc < 0) {
                    if (errno == EINTR) {
                        if (host_gc_cycle(host, &last_gc_run,
                                          &last_pressure_check)) {
                            break;
                        }
                        continue;
                    }
                    // poll() failed on the bind socket -- treat as fatal
                    printf("[listener] poll() on bind socket failed: %s\n",
                           strerror(errno));
                    restart_listener = true;
                    break;
                }
                if (poll_rc == 0) {
                    // Timeout: no incoming connection yet
                    if (host_gc_cycle(host, &last_gc_run,
                                      &last_pressure_check)) {
                        restart_listener = true;
                        break;
                    }
                    ++idle_poll_cycles;
                    if (idle_poll_cycles >=
                        SSH_CHATTER_ACCEPT_HEALTH_CHECK_POLLS) {
                        // Verify bind socket is still healthy
                        idle_poll_cycles = 0U;
                        int sock_err = 0;
                        socklen_t err_len = sizeof(sock_err);
                        if (getsockopt(bind_fd, SOL_SOCKET, SO_ERROR,
                                       &sock_err, &err_len) < 0 ||
                            sock_err != 0) {
                            printf("[listener] bind socket health check "
                                   "failed (error=%d), restarting\n",
                                   sock_err);
                            restart_listener = true;
                            break;
                        }
                    }
                    continue;
                }
                if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                    printf("[listener] bind socket reported error "
                           "(revents=0x%x), restarting\n",
                           (unsigned)pfd.revents);
                    restart_listener = true;
                    break;
                }
                idle_poll_cycles = 0U;
            }

            if (host_gc_cycle(host, &last_gc_run, &last_pressure_check)) {
                restart_listener = true;
                break;
            }
            host_idle_state_maintenance(host, &last_idle_check);

            ssh_session session = ssh_new();
            if (session == nullptr) {
                humanized_log_error("host", "failed to allocate session",
                                    ENOMEM);
                continue;
            }

            if (ssh_bind_accept(bind_handle, session) == SSH_ERROR) {
                int accept_error = errno;
                const char *bind_error = ssh_get_error(bind_handle);
                const bool bind_error_present =
                    bind_error != nullptr && bind_error[0] != '\0';

                if (accept_error == 0) {
                    if (host->listener.accept_error_streak < UINT_MAX) {
                        host->listener.accept_error_streak += 1U;
                    }
                } else {
                    host->listener.accept_error_streak = 0U;
                }

                printf("[listener] accept failed, error=%d, shutdown_flag=%p "
                       "value=%d\n",
                       accept_error, (void *)host->shutdown_flag,
                       (host->shutdown_flag != nullptr ? *host->shutdown_flag
                                                       : -1));
                fflush(stdout);

                if (accept_error != 0) {
                    char log_message[512];
                    const char *system_message = strerror(accept_error);

                    if (system_message != nullptr &&
                        system_message[0] != '\0') {
                        if (bind_error_present &&
                            !string_contains_case_insensitive(bind_error,
                                                              system_message)) {
                            snprintf(log_message, sizeof(log_message),
                                     "Socket error: %s (%s)", system_message,
                                     bind_error);
                        } else {
                            snprintf(log_message, sizeof(log_message),
                                     "Socket error: %s", system_message);
                        }
                    } else if (bind_error_present) {
                        snprintf(log_message, sizeof(log_message),
                                 "Socket error (code %d): %s", accept_error,
                                 bind_error);
                    } else {
                        snprintf(log_message, sizeof(log_message),
                                 "Socket error (code %d)", accept_error);
                    }

                    humanized_log_error("host", log_message, accept_error);
                } else if (bind_error != nullptr && bind_error[0] != '\0') {
                    humanized_log_error("host", bind_error, EIO);
                } else {
                    humanized_log_error("host", "Socket accept failed", EIO);
                }

                bool fatal_socket_error = false;
                bool should_backoff_after_socket_error = false;
                bool force_restart_after_empty_errno = false;

                if (accept_error != 0) {
                    should_backoff_after_socket_error = true;
                    switch (accept_error) {
                    case EAGAIN:
#ifdef EWOULDBLOCK
#if EWOULDBLOCK != EAGAIN
                    case EWOULDBLOCK:
#endif
#endif
                    case EINTR:
                    case ECONNRESET:
                    case ECONNABORTED:
                    case ETIMEDOUT:
                    case ENOTCONN:
                    case EPIPE:
#ifdef EPROTO
                    case EPROTO:
#endif
                        should_backoff_after_socket_error = false;
                        break;
                    case EBADF:
                    case ENOTSOCK:
                    case EINVAL:
                        fatal_socket_error = true;
                        break;
                    case EMFILE:
                    case ENFILE:
                    case ENOBUFS:
#ifdef ENOMEM
                    case ENOMEM:
#endif
#ifdef ENOSR
                    case ENOSR:
#endif
                        // Resource exhaustion: treat as transient with backoff
                        // instead of fatal restart.  Restarting the listener
                        // while FDs are exhausted will just fail again.
                        should_backoff_after_socket_error = true;
                        break;
                    default:
                        break;
                    }
                } else {
                    should_backoff_after_socket_error = true;
                    if (host->listener.accept_error_streak >= 3U) {
                        fatal_socket_error = true;
                        force_restart_after_empty_errno = true;
                    }
                }

                ssh_free(session);
                if (fatal_socket_error && !force_restart_after_empty_errno) {
                    if (bind_error_present &&
                        string_contains_case_insensitive(bind_error, "kex")) {
                        fatal_socket_error = false;
                    } else if (bind_error_present) {
                        fatal_socket_error = false;
                    }
                }

                if (fatal_socket_error) {
                    if (force_restart_after_empty_errno) {
                        printf("[listener] forcing restart after %u consecutive "
                               "accept failures without errno\n",
                               host->listener.accept_error_streak);
                    }
                    host->listener.accept_error_streak = 0U;
                    clock_gettime(CLOCK_MONOTONIC,
                                  &host->listener.last_error_time);
                    if (host_listener_attempt_recover(host, bind_handle,
                                                      address, bind_port)) {
                        continue;
                    }
                    host->listener.restart_attempts += 1U;
                    printf("[listener] scheduling full listener restart after "
                           "socket "
                           "error (attempt %u)\n",
                           host->listener.restart_attempts);
                    restart_listener = true;
                    break;
                }

                if (should_backoff_after_socket_error) {
                    // Use longer backoff for resource exhaustion errors
                    long backoff_ns = 200000000L; // 200ms default
                    if (accept_error == EMFILE || accept_error == ENFILE ||
                        accept_error == ENOBUFS) {
                        backoff_ns = 1000000000L; // 1s for FD/buffer exhaustion
                    }
                    struct timespec retry_delay = {
                        .tv_sec = backoff_ns / 1000000000L,
                        .tv_nsec = backoff_ns % 1000000000L,
                    };
                    host_sleep_uninterruptible(&retry_delay);
                }

                // Check shutdown flag after non-fatal errors (e.g., EINTR from signal)
                if (host->shutdown_flag != nullptr &&
                    *host->shutdown_flag != 0) {
                    printf(
                        "[listener] detected shutdown flag after socket error, "
                        "breaking from accept loop\n");
                    restart_listener = true;
                    break;
                }

                continue;
            }

            char peer_address[NI_MAXHOST];
            session_describe_peer(session, peer_address, sizeof(peer_address));
            if (peer_address[0] == '\0') {
                strncpy(peer_address, "unknown", sizeof(peer_address) - 1U);
                peer_address[sizeof(peer_address) - 1U] = '\0';
            }

            printf("[connect] accepted client from %s\n", peer_address);
            host->listener.accept_error_streak = 0U;

            session_ctx_t *ctx = session_create();
            if (ctx == nullptr) {
                humanized_log_error(
                    "host", "failed to allocate session context", ENOMEM);
                ssh_disconnect(session);
                ssh_free(session);
                continue;
            }
            ctx->ops = &ssh_session_ops;

            ctx->session = session;
            ctx->channel = nullptr;
            ctx->transport_kind = SESSION_TRANSPORT_SSH;
            ctx->telnet_fd = -1;
            ctx->telnet_eof = false;
            ctx->telnet_pending_valid = false;
            pthread_mutexattr_t lock_attr;
            pthread_mutexattr_init(&lock_attr);
            pthread_mutexattr_settype(&lock_attr, PTHREAD_MUTEX_RECURSIVE);
            int mutex_error = pthread_mutex_init(&ctx->output_lock, &lock_attr);
            pthread_mutexattr_destroy(&lock_attr);
            if (mutex_error != 0) {
                humanized_log_error("host",
                                    "failed to initialise session output lock",
                                    mutex_error);
                session_destroy(ctx);
                continue;
            }
            ctx->output_lock_initialized = true;
            ctx->owner = host;
            if (ttak_mutex_init(&ctx->channel_mutex) == 0) {
                ctx->channel_mutex_initialized = true;
            } else {
                humanized_log_error("session",
                                    "failed to initialize channel mutex",
                                    errno != 0 ? errno : ENOMEM);
            }
            ctx->auth = (auth_profile_t){0};
            snprintf(ctx->client_ip, sizeof(ctx->client_ip), "%.*s",
                     (int)sizeof(ctx->client_ip) - 1, peer_address);
            session_log_encoding_state("transport_setup", ctx);
            ctx->input_mode = SESSION_INPUT_MODE_CHAT;

            bool geo_language_enabled =
                atomic_load(&ctx->owner->geo_language_enabled);
            session_ui_language_t provider_language = SESSION_UI_LANGUAGE_COUNT;
            char provider_label[SSH_CHATTER_PROVIDER_LABEL_LEN];
            bool provider_detected =
                geo_language_enabled &&
                session_detect_provider_ip(ctx->client_ip, provider_label,
                                           sizeof(provider_label));
            if (provider_detected &&
                host_provider_language_preference(host, provider_label,
                                                  &provider_language)) {
                ctx->ui_language = provider_language;
                ctx->active_codepage =
                    session_codepage_for_language(provider_language);
            } else if (geo_language_enabled) {
                session_ui_language_t geo_language =
                    session_client_geo_language(ctx);
                if (geo_language != SESSION_UI_LANGUAGE_COUNT) {
                    ctx->ui_language = geo_language;
                    ctx->active_codepage =
                        session_codepage_for_language(geo_language);
                } else {
                    ctx->ui_language = SESSION_UI_LANGUAGE_EN;
                    ctx->active_codepage =
                        session_codepage_for_language(SESSION_UI_LANGUAGE_EN);
                }
            } else {
                ctx->ui_language = SESSION_UI_LANGUAGE_EN;
                ctx->active_codepage =
                    session_codepage_for_language(SESSION_UI_LANGUAGE_EN);
            }
            session_refresh_output_encoding(ctx);

            ttak_mutex_lock(&host->lock);
            ++host->connection_count;
            ctx->user.is_operator = false;
            ctx->user.is_lan_operator = false;
            ttak_mutex_unlock(&host->lock);

            pthread_t thread_id;
            if (pthread_create(&thread_id, nullptr, session_thread, ctx) != 0) {
                humanized_log_error("host", "failed to spawn session thread",
                                    errno);
                session_destroy(ctx);
                continue;
            }

            pthread_detach(thread_id);
            host_error_guard_register_success(host);

        }

loop_cleanup:
        if (bind_handle != nullptr) {
            ssh_bind_free(bind_handle);
            host->listener.handle = nullptr;
        }

        for (size_t idx = 0; idx < host_key_count; ++idx) {
            if (imported_keys[idx] != nullptr) {
                ssh_key_free(imported_keys[idx]);
                imported_keys[idx] = nullptr;
            }
        }

        // Check for shutdown signal before deciding to restart
        if (host->shutdown_flag != nullptr && *host->shutdown_flag != 0) {
            printf(
                "[listener] shutdown signal received, exiting listener loop\n");
            break;
        }

        if (!restart_listener) {
            host_sleep_after_error(host);
            continue;
        }

        struct timespec backoff = {
            .tv_sec = 1,
            .tv_nsec = 0,
        };
        host_sleep_uninterruptible(&backoff);

        if (host->listener.restart_attempts > 0U) {
            printf("[listener] attempting full listener restart after socket "
                   "error "
                   "(attempt %u)\n",
                   host->listener.restart_attempts);
        } else {
            printf("[listener] attempting full listener restart after socket "
                   "error\n");
        }

        if (host->listener.last_error_time.tv_sec != 0 ||
            host->listener.last_error_time.tv_nsec != 0L) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            struct timespec elapsed =
                timespec_diff(&now, &host->listener.last_error_time);
            double elapsed_seconds =
                (double)elapsed.tv_sec + (double)elapsed.tv_nsec / 1000000000.0;
            printf("[listener] last fatal error occurred %.3f seconds ago\n",
                   elapsed_seconds);
        }
    }

    return 0;
}

# Thread-Migration Plan (complete)

## Context
The DDial listener was successfully migrated to the detached + epoch-GC model:
- Listener thread self-detaches (`pthread_detach(pthread_self())`).
- Stop path signals `stop` + `shutdown(fd)` and returns immediately.
- No `pthread_join` — epoch rotation reclaims the `host_t` after the thread exits naturally.

## Completed Migrations
- `session_thread` — self-detaches; already epoch-registered.
- `host_telnet_listener_thread` — self-detaches + epoch; join removed from stop.
- `host_json_api_listener_thread` — self-detaches + epoch; join removed from stop.
- `host_rss_backend` — added epoch enter/exit + self-detaches; join removed from shutdown.
- `host_rss_manual_refresh_worker` — self-detaches for consistency.
- `host_archive_backend` — self-detaches + epoch; join removed from shutdown.
- `host_bbs_watchdog_thread` — self-detaches + epoch; join removed from shutdown.
- `host_eliza_worker_thread` — self-detaches; shutdown signals `stop` + broadcast only. The thread drops the pending queue on stop instead of draining it.
- `host_moderation_thread` — self-detaches; shutdown signals `stop`/`active=false` + broadcast only. The thread now owns the child process: it closes the pipes, reaps the child and flushes pending tasks on exit, and `host_moderation_recover_worker` refuses to respawn once `stop` is set.

## Mutex/cond lifetime (option b)
Neither worker's shutdown destroys its mutex/cond once the thread has started; the primitives stay embedded in `host_t` and are reclaimed by epoch GC after the thread calls `sshc_epoch_thread_exit()`. `thread_started` is intentionally left set after shutdown so a repeated shutdown can't hit the "never started" path that does destroy them. Valgrind/TSan may report them as leaked until epoch rotation — expected.


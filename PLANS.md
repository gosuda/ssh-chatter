# Remaining Thread-Migration Plan

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

## Blocked Migrations (requires redesign)

### ELIZA worker (`host_eliza_worker_thread`)
**Current pattern:**
- Thread blocks on `ttak_cond_wait(&worker->cond, &worker->mutex)`.
- Shutdown sets `stop`, broadcasts cond, **joins**, then destroys mutex/cond.

**Why direct migration is unsafe:**
- If we remove `pthread_join`, shutdown will destroy `worker->mutex` and `worker->cond` while the thread may still be inside `ttak_cond_wait` or waking up.
- POSIX condition-variable destruction while a thread is waiting on it is undefined behavior.

**Required redesign:**
1. Make the worker thread self-detach and epoch-register.
2. Move mutex/cond destruction out of `host_eliza_worker_shutdown`.
3. Either:
   - a) Use a separate "worker exited" atomic flag + spin-wait in shutdown until the flag is set (the thread sets it after releasing the mutex for the last time), or
   - b) Let mutex/cond be destroyed when the host is reclaimed by epoch GC (do not explicitly destroy them in shutdown).

Option (b) is simpler but wastes a small amount of memory until epoch rotation. Option (a) is cleaner but adds a new synchronization primitive.

### Moderation worker (`host_moderation_thread`)
**Same issue as ELIZA.** The thread blocks on `ttak_cond_wait(&host->moderation.cond, &host->moderation.mutex)`, and shutdown destroys both after broadcasting.

**Required redesign:** identical to ELIZA.

## Recommendation
Adopt option (b) for both workers: stop setting `stop` + broadcast, skip join, and skip mutex/cond destruction in shutdown. The epoch GC will reclaim the entire `host_t` (including embedded mutex/cond) after the worker thread calls `sshc_epoch_thread_exit()`.

Caveat: Valgrind/TSan may flag the leaked mutex/cond as a false-positive until epoch rotation occurs. This is acceptable given the project's explicit epoch-GC memory model.

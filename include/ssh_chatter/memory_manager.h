#ifndef SSH_CHATTER_MEMORY_MANAGER_H
#define SSH_CHATTER_MEMORY_MANAGER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include <ttak/mem/mem.h>
#include <ttak/mem/owner.h>
#include <ttak/mem/epoch_gc.h>
#include <ttak/mem/epoch.h>
#include <ttak/mem/detachable.h>
#include <ttak/timing/timing.h>
#include <ttak/sync/sync.h>
#include <ttak/atomic/atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ====================================================================
 * Variable Lifecycle & Ownership Model
 * ====================================================================
 *
 * Every allocation in ssh-chatter flows through four cooperating layers
 * provided by libttak.  Understanding them is essential for safe memory
 * management in a multi-threaded chat server.
 *
 * 1. Owner  (ttak_owner_t / tt_owner_t)
 *    - Represents who *owns* a resource.
 *    - Each sshc_memory_context carries an owner that tracks all live
 *      allocations inside it (via ttak_owner_register_resource).
 *    - When the context is destroyed, the owner releases every tracked
 *      resource.
 *
 * 2. EpochGC  (ttak_epoch_gc_t)
 *    - Generational garbage collector layered on top of ttak_mem_tree.
 *    - Allocations are registered with the *current* epoch.
 *    - Calling sshc_memory_context_epoch_gc_rotate() advances the epoch
 *      and frees blocks whose epochs have expired, *without* a global
 *      stop-the-world pause.
 *    - Ideal for periodic bulk cleanup (e.g. once per accept() iteration).
 *
 * 3. EBR  (Epoch-Based Reclamation, ttak_epoch_*)
 *    - Protects *shared* pointers that may be read concurrently.
 *    - A thread calls sshc_epoch_thread_enter() to announce it is active
 *      and sshc_epoch_thread_exit() when done.
 *    - sshc_epoch_retire(ptr) defers freeing until *all* threads have
 *      moved past the current epoch, ensuring no dangling reads.
 *    - sshc_epoch_reclaim() actually frees pointers that are no longer
 *      observable by any thread.
 *
 * 4. Detachable Memory  (ttak_detachable_*)
 *    - Short-lived, arena-like allocations with automatic cache recycling.
 *    - Each sshc_memory_context owns a ttak_detachable_context that
 *      provides fast alloc/free with epoch protection and a small LRU
 *      cache for tiny allocations (<= 16 bytes).
 *    - Use sshc_detachable_alloc() / sshc_detachable_free() for
 *      per-session scratch buffers, temporary strings, or any data whose
 *      lifetime is strictly bounded by the enclosing session.
 *
 * Typical session thread lifecycle:
 *
 *   sshc_epoch_thread_enter();              // register with EBR
 *   sshc_memory_context_push(session_ctx);  // bind TLS to session owner
 *
 *     ... sshc_gc_malloc / sshc_detachable_alloc ...
 *     ... sshc_epoch_retire(shared_ptr) ...
 *
 *   sshc_memory_context_pop(prev);          // unbind
 *   sshc_epoch_thread_exit();               // deregister from EBR
 *
 * ==================================================================== */

typedef struct sshc_memory_context sshc_memory_context_t;

void sshc_memory_runtime_init(void);
void sshc_memory_runtime_shutdown(void);

sshc_memory_context_t *sshc_memory_context_create(const char *label);
void sshc_memory_context_destroy(sshc_memory_context_t *ctx);
sshc_memory_context_t *sshc_memory_context_push(sshc_memory_context_t *ctx);
void sshc_memory_context_pop(sshc_memory_context_t *previous);
void sshc_memory_context_reset(sshc_memory_context_t *ctx);
sshc_memory_context_t *sshc_memory_context_current(void);

/**
 * @brief Retrieve the owner associated with a memory context.
 *
 * The owner tracks all resources registered within the context and
 * enforces isolation policies (see ttak_owner_policy_t).
 *
 * @return The owner pointer, or NULL if the context is NULL.
 */
tt_owner_t *sshc_memory_context_get_owner(sshc_memory_context_t *ctx);

/**
 * @brief Retrieve the detachable context for short-lived allocations.
 *
 * Returns the ttak_detachable_context_t embedded in the memory context.
 * Use it with sshc_detachable_alloc() / sshc_detachable_free() for
 * arena-style scratch memory with automatic epoch protection.
 *
 * @return The detachable context pointer, or NULL if ctx is NULL.
 */
ttak_detachable_context_t *sshc_memory_context_get_detachable(
    sshc_memory_context_t *ctx);

/**
 * @brief Rotate the EpochGC associated with a memory context.
 *
 * Advances the epoch and triggers a non-blocking cleanup pass that frees
 * blocks from expired epochs.  Call periodically (e.g. once per second)
 * from a background thread or at natural safepoints.
 */
void sshc_memory_context_epoch_gc_rotate(sshc_memory_context_t *ctx);

/* ------------------------------------------------------------------ */
/* EBR (Epoch-Based Reclamation) wrappers                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Register the calling thread with the EBR subsystem.
 *
 * Must be called once from each thread that will perform deferred frees
 * via sshc_epoch_retire.  Typically invoked at the start of a session
 * thread or worker thread.
 *
 * Internally calls ttak_epoch_register_thread() and ttak_epoch_enter()
 * so the thread immediately participates in the global epoch.
 */
void sshc_epoch_thread_enter(void);

/**
 * @brief Deregister the calling thread from the EBR subsystem.
 *
 * Should be called before a thread exits to indicate it is no longer
 * holding references to any retired memory.  Performs a final
 * ttak_epoch_reclaim() to free any reclaimable pointers.
 */
void sshc_epoch_thread_exit(void);

/**
 * @brief Defer freeing a pointer until it is safe to do so.
 *
 * The pointer is retired via EBR and will be freed (via ttak_mem_free)
 * once all threads have observed the current epoch.  This guarantees
 * no thread can dereference the pointer after it has been freed.
 */
void sshc_epoch_retire(void *ptr);

/**
 * @brief Retire a pointer with a caller-specified cleanup routine.
 *
 * Useful for complex allocations (such as session_ctx_t) that need to
 * run custom teardown logic or interact with sshc_gc_free before the
 * underlying memory is released.
 */
void sshc_epoch_retire_with(void *ptr, void (*cleanup)(void *));

/**
 * @brief Attempt to reclaim memory from safe epochs.
 *
 * Advances the global epoch and frees pointers that were retired in
 * epochs that are no longer observable by any thread.  Call periodically
 * from the main accept loop or a background maintenance thread.
 */
void sshc_epoch_reclaim(void);

/* ------------------------------------------------------------------ */
/* Detachable memory wrappers                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Allocate detachable (short-lived) memory from the current context.
 *
 * The allocation is tracked by the context's detachable arena and
 * protected by epoch-based reclamation.  Tiny allocations (<= 16 bytes)
 * are served from a per-context LRU cache for near-zero overhead.
 *
 * @param size  Number of bytes to allocate (0 is treated as 1).
 * @return Detachable allocation descriptor; check .data for NULL on failure.
 */
ttak_detachable_allocation_t sshc_detachable_alloc(size_t size);

/**
 * @brief Free a detachable allocation.
 *
 * Small allocations may be returned to the cache instead of being freed
 * immediately.  Larger allocations are retired through EBR for safe
 * deferred reclamation.
 *
 * @param alloc  Pointer to the allocation descriptor (zeroed on return).
 */
void sshc_detachable_free(ttak_detachable_allocation_t *alloc);

// Internal implementation functions to avoid naming conflicts with libgc
void *sshc_gc_malloc(size_t size);
void *sshc_gc_realloc(void *ptr, size_t size);
void *sshc_gc_calloc(size_t count, size_t size);
void sshc_gc_free(void *ptr);
void sshc_gc_init(void);

#if defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC
#include <gc/gc.h>
#endif

// Macros to map GC_* names to our wrapped implementations
// We undefine them first in case gc.h or other headers defined them
#undef GC_MALLOC
#define GC_MALLOC(s) sshc_gc_malloc(s)

#undef GC_REALLOC
#define GC_REALLOC(p, s) sshc_gc_realloc(p, s)

#undef GC_CALLOC
#define GC_CALLOC(c, s) sshc_gc_calloc(c, s)

#undef GC_FREE
#define GC_FREE(p) sshc_gc_free(p)

#undef GC_INIT
#define GC_INIT() sshc_gc_init()

// Compatibility for lowercase GC_free if used
#undef GC_free
#define GC_free(p) sshc_gc_free(p)

static inline char *sshc_strdup(const char *text)
{
    if (text == nullptr) {
        return nullptr;
    }

    size_t length = strlen(text) + 1U;
    char *copy = (char *)sshc_gc_malloc(length);
    if (copy == nullptr) {
        return nullptr;
    }

    memcpy(copy, text, length);
    return copy;
}

#ifdef __cplusplus
}
#endif

#endif

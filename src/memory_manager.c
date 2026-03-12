/**
 * @file memory_manager.c
 * @desc Unified memory manager for SSH-Chatter using libttak for manual lifetimes
 *       and Boehm GC for automatic collection when enabled.
 *
 *       Integrates four libttak subsystems to govern variable lifecycles:
 *
 *       * Owner (tt_owner_t)
 *           Each memory context carries an owner that tracks live allocations.
 *           Resources are registered via ttak_owner_register_resource() so that
 *           the owner can audit and release them on context destruction.
 *
 *       * EpochGC (ttak_epoch_gc_t)
 *           Generational garbage collector backed by ttak_mem_tree.  Allocations
 *           are registered with the current epoch; calling epoch_gc_rotate()
 *           advances the epoch and frees expired blocks without a global pause.
 *
 *       * EBR (ttak_epoch_* / Epoch-Based Reclamation)
 *           Lock-free deferred reclamation for shared pointers.  A pointer
 *           passed to sshc_epoch_retire() is freed only after every thread has
 *           moved past the epoch in which the retirement occurred.
 *
 *       * Detachable Memory (ttak_detachable_*)
 *           Arena-like allocator with a small LRU cache for tiny chunks.
 *           Each context owns a ttak_detachable_context_t that provides fast
 *           alloc/free with epoch protection, suitable for per-session scratch
 *           buffers whose lifetime is strictly bounded by the session.
 */

#include "ssh_chatter/memory_manager.h"
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct sshc_memory_allocation {
    void *ptr;
    size_t size;
    struct sshc_memory_allocation *next_in_context;
    struct sshc_memory_allocation *next_global;
    struct sshc_memory_context *context;
    bool gc_registered;
    pthread_t creator_thread;
} sshc_memory_allocation_t;

struct sshc_memory_context {
    pthread_mutex_t mutex;
    sshc_memory_allocation_t *allocations;
    const char *label;
    struct sshc_memory_context *next;
    /** Owner: tracks resource provenance and enforces isolation policies. */
    tt_owner_t *owner;
    /** EpochGC: generational epoch-based garbage collector. */
    ttak_epoch_gc_t epoch_gc;
    /** Detachable: arena-like allocator for short-lived, session-scoped data. */
    ttak_detachable_context_t detachable;
};

#define SSH_CHATTER_DEFAULT_LIFETIME TT_HOUR(24)

static pthread_mutex_t sshc_registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool sshc_runtime_initialised = false;
static sshc_memory_context_t sshc_global_context;
static sshc_memory_context_t *sshc_contexts = nullptr;
static sshc_memory_allocation_t *sshc_allocations = nullptr;
static __thread sshc_memory_context_t *sshc_tls_context = nullptr;
static __thread bool sshc_tls_defer_gc_registration = false;

static void sshc_memory_context_init(sshc_memory_context_t *ctx,
                                     const char *label)
{
    pthread_mutex_init(&ctx->mutex, nullptr);
    ctx->allocations = nullptr;
    ctx->label = label;
    ctx->next = nullptr;

    /* Owner: Maximum restriction. Deny threading and dangerous memory for strict verticality. */
    ctx->owner = ttak_owner_create(TTAK_OWNER_DENY_DANGEROUS_MEM | 
                                   TTAK_OWNER_STRICT_ISOLATION |
                                   TTAK_OWNER_DENY_THREADING);

    /* EpochGC: generational collector. */
    ttak_epoch_gc_init(&ctx->epoch_gc);

    /* Reclamation: 50ms. */
    ttak_mem_tree_set_manual_cleanup(&ctx->epoch_gc.tree, false);
    ttak_mem_tree_set_cleaning_intervals(&ctx->epoch_gc.tree, 
                                         TT_MILLI_SECOND(50), 
                                         TT_MILLI_SECOND(50));
    
    /* Detachable arena: fast alloc/free with EBR. */
    ttak_detachable_context_init(
        &ctx->detachable,
        TTAK_ARENA_HAS_EPOCH_RECLAMATION | TTAK_ARENA_HAS_DEFAULT_EPOCH_GC
        | TTAK_ARENA_HAS_DEFAULT_EPOCH_GC);
}

static sshc_memory_context_t *sshc_memory_context_global(void)
{
    return &sshc_global_context;
}

void sshc_memory_runtime_init(void)
{
    pthread_mutex_lock(&sshc_registry_mutex);
    if (!sshc_runtime_initialised) {
#if defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC
        GC_set_free_space_divisor(10); 
        GC_init(); 
#endif
        // Global TTAK tuning: 50ms
        ttak_mem_set_trace(1);
        ttak_mem_configure_gc(TT_MILLI_SECOND(50), TT_MILLI_SECOND(50), 1);

        sshc_memory_context_init(&sshc_global_context, "global");
        
        sshc_global_context.next = nullptr;
        sshc_contexts = sshc_memory_context_global();
        sshc_runtime_initialised = true;
    }
    pthread_mutex_unlock(&sshc_registry_mutex);
}

static void sshc_memory_registry_remove_locked(sshc_memory_allocation_t *allocation)
{
    // Internal helper that assumes sshc_registry_mutex is already held
    sshc_memory_allocation_t **prev = &sshc_allocations;
    while (*prev != nullptr) {
        if (*prev == allocation) {
            *prev = allocation->next_global;
            break;
        }
        prev = &(*prev)->next_global;
    }
}

void sshc_memory_runtime_shutdown(void)
{
    pthread_mutex_lock(&sshc_registry_mutex);
    if (!sshc_runtime_initialised) {
        pthread_mutex_unlock(&sshc_registry_mutex);
        return;
    }

    // Final EBR reclaim pass
    ttak_epoch_reclaim();

    sshc_memory_context_t *ctx = sshc_contexts;
    while (ctx != nullptr) {
        sshc_memory_context_t *next = ctx->next;
        if (ctx != sshc_memory_context_global()) {
            // Manual cleanup of session contexts
            pthread_mutex_lock(&ctx->mutex);
            sshc_memory_allocation_t *allocation = ctx->allocations;
            ctx->allocations = nullptr;
            pthread_mutex_unlock(&ctx->mutex);

            while (allocation != nullptr) {
                sshc_memory_allocation_t *next_alloc = allocation->next_in_context;
                sshc_memory_registry_remove_locked(allocation);
#if defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC
                if (allocation->gc_registered) {
                    GC_remove_roots(allocation->ptr,
                                    (char *)allocation->ptr +
                                        allocation->size);
                }
#endif
                ttak_mem_node_t *node = ttak_mem_tree_find_node(
                    &ctx->epoch_gc.tree, allocation->ptr);
                if (!allocation->gc_registered && node == nullptr) {
                    ttak_mem_free(allocation->ptr);
                }
                ttak_mem_free(allocation);
                allocation = next_alloc;
            }

            ttak_detachable_context_destroy(&ctx->detachable);
            ttak_epoch_gc_destroy(&ctx->epoch_gc);
            if (ctx->owner) ttak_owner_destroy(ctx->owner);
            pthread_mutex_destroy(&ctx->mutex);
            ttak_mem_free(ctx);
        }
        ctx = next;
    }
    
    // Clean up the global context
    pthread_mutex_lock(&sshc_global_context.mutex);
    sshc_memory_allocation_t *allocation = sshc_global_context.allocations;
    sshc_global_context.allocations = nullptr;
    pthread_mutex_unlock(&sshc_global_context.mutex);

    while (allocation != nullptr) {
        sshc_memory_allocation_t *next_alloc = allocation->next_in_context;
        sshc_memory_registry_remove_locked(allocation);
        ttak_mem_node_t *node = ttak_mem_tree_find_node(
            &sshc_global_context.epoch_gc.tree, allocation->ptr);
        if (!allocation->gc_registered && node == nullptr) {
            ttak_mem_free(allocation->ptr);
        }
        ttak_mem_free(allocation);
        allocation = next_alloc;
    }

    ttak_detachable_context_destroy(&sshc_global_context.detachable);
    ttak_epoch_gc_destroy(&sshc_global_context.epoch_gc);
    if (sshc_global_context.owner) ttak_owner_destroy(sshc_global_context.owner);
    pthread_mutex_destroy(&sshc_global_context.mutex);
    
    sshc_contexts = nullptr;
    sshc_runtime_initialised = false;
    pthread_mutex_unlock(&sshc_registry_mutex);
}

sshc_memory_context_t *sshc_memory_context_create(const char *label)
{
    sshc_memory_runtime_init();
    sshc_memory_context_t *ctx =
        (sshc_memory_context_t *)ttak_mem_alloc(sizeof(*ctx),
                                                __TTAK_UNSAFE_MEM_FOREVER__,
                                                ttak_get_tick_count());
    if (ctx == nullptr) {
        errno = ENOMEM;
        return nullptr;
    }
    sshc_memory_context_init(ctx, label);
    
    /* Session Context Tuning: 50ms.  */
    ttak_mem_tree_set_manual_cleanup(&ctx->epoch_gc.tree, false);
    ttak_mem_tree_set_cleaning_intervals(&ctx->epoch_gc.tree, 
                                         TT_MILLI_SECOND(50), 
                                         TT_MILLI_SECOND(50));
    ttak_mem_tree_set_pressure_threshold(&ctx->epoch_gc.tree, 1);

    /* Vertical Hierarchy: Register this session owner as a child of the global owner.
     * This ensures that if the global context is destroyed, all session contexts are audited. */
    ttak_owner_register_resource(sshc_global_context.owner, label, ctx->owner);

    pthread_mutex_lock(&sshc_registry_mutex);
    ctx->next = sshc_contexts;
    sshc_contexts = ctx;
    pthread_mutex_unlock(&sshc_registry_mutex);
    return ctx;
}

static void sshc_memory_registry_remove(sshc_memory_allocation_t *allocation)
{
    pthread_mutex_lock(&sshc_registry_mutex);
    sshc_memory_registry_remove_locked(allocation);
    pthread_mutex_unlock(&sshc_registry_mutex);
}

static void sshc_memory_registry_add(sshc_memory_allocation_t *allocation)
{
    pthread_mutex_lock(&sshc_registry_mutex);
    allocation->next_global = sshc_allocations;
    sshc_allocations = allocation;
    pthread_mutex_unlock(&sshc_registry_mutex);
}

void sshc_memory_context_destroy(sshc_memory_context_t *ctx)
{
    if (ctx == nullptr || ctx == sshc_memory_context_global()) {
        return;
    }

    sshc_memory_context_reset(ctx);
    ttak_detachable_context_destroy(&ctx->detachable);
    ttak_epoch_gc_destroy(&ctx->epoch_gc);
    if (ctx->owner) ttak_owner_destroy(ctx->owner);
    pthread_mutex_destroy(&ctx->mutex);

    pthread_mutex_lock(&sshc_registry_mutex);
    sshc_memory_context_t **prev = &sshc_contexts;
    while (*prev != nullptr) {
        if (*prev == ctx) {
            *prev = ctx->next;
            break;
        }
        prev = &(*prev)->next;
    }
    pthread_mutex_unlock(&sshc_registry_mutex);
    ttak_mem_free(ctx);
}

sshc_memory_context_t *sshc_memory_context_push(sshc_memory_context_t *ctx)
{
    sshc_memory_runtime_init();
    sshc_memory_context_t *previous = sshc_tls_context;
    if (ctx == nullptr) {
        sshc_tls_context = sshc_memory_context_global();
    } else {
        sshc_tls_context = ctx;
    }
    return previous;
}

void sshc_memory_context_pop(sshc_memory_context_t *previous)
{
    sshc_tls_context = previous;
}

sshc_memory_context_t *sshc_memory_context_current(void)
{
    sshc_memory_runtime_init();
    return (sshc_tls_context != nullptr) ? sshc_tls_context
                                         : sshc_memory_context_global();
}

static sshc_memory_allocation_t *
sshc_memory_context_remove_allocation(sshc_memory_context_t *ctx, void *ptr)
{
    if (ctx == nullptr || ptr == nullptr) {
        return nullptr;
    }

    pthread_mutex_lock(&ctx->mutex);
    sshc_memory_allocation_t **prev = &ctx->allocations;
    while (*prev != nullptr) {
        if ((*prev)->ptr == ptr) {
            sshc_memory_allocation_t *found = *prev;
            *prev = (*prev)->next_in_context;
            pthread_mutex_unlock(&ctx->mutex);
            return found;
        }
        prev = &(*prev)->next_in_context;
    }
    pthread_mutex_unlock(&ctx->mutex);
    return nullptr;
}

static void
sshc_memory_context_register_allocation(sshc_memory_context_t *ctx,
                                        sshc_memory_allocation_t *allocation)
{
    pthread_mutex_lock(&ctx->mutex);
    allocation->next_in_context = ctx->allocations;
    ctx->allocations = allocation;
    pthread_mutex_unlock(&ctx->mutex);
    
    if (sshc_tls_defer_gc_registration) {
        return;
    }

    // Register with EpochGC. This adds it to the tree with ref_count=1.
    ttak_epoch_gc_register(&ctx->epoch_gc, allocation->ptr, allocation->size);
    allocation->gc_registered = true;

    // Also register with ttak owner if available
    if (ctx->owner) {
        char name[32];
        snprintf(name, sizeof(name), "alloc_%p", allocation->ptr);
        ttak_owner_register_resource(ctx->owner, name, allocation->ptr);
    }

#if defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC
    // Ensure Boehm GC scans this manually managed block for pointers
    GC_add_roots(allocation->ptr, (char *)allocation->ptr + allocation->size);
#endif
}

static void
sshc_memory_register_deferred_allocation(sshc_memory_allocation_t *allocation)
{
    if (allocation == nullptr || allocation->context == nullptr ||
        allocation->gc_registered) {
        return;
    }

    ttak_epoch_gc_register(&allocation->context->epoch_gc, allocation->ptr,
                           allocation->size);
    allocation->gc_registered = true;

    if (allocation->context->owner) {
        char name[32];
        snprintf(name, sizeof(name), "alloc_%p", allocation->ptr);
        ttak_owner_register_resource(allocation->context->owner, name,
                                     allocation->ptr);
    }

#if defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC
    GC_add_roots(allocation->ptr, (char *)allocation->ptr + allocation->size);
#endif
}

void sshc_memory_defer_gc_registration_begin(void)
{
    sshc_tls_defer_gc_registration = true;
}

void sshc_memory_defer_gc_registration_flush(void)
{
    pthread_t self = pthread_self();
    pthread_mutex_lock(&sshc_registry_mutex);
    sshc_memory_allocation_t *allocation = sshc_allocations;
    while (allocation != nullptr) {
        if (!allocation->gc_registered &&
            pthread_equal(allocation->creator_thread, self)) {
            sshc_memory_register_deferred_allocation(allocation);
        }
        allocation = allocation->next_global;
    }
    pthread_mutex_unlock(&sshc_registry_mutex);
}

void sshc_memory_defer_gc_registration_end(void)
{
    sshc_memory_defer_gc_registration_flush();
    sshc_tls_defer_gc_registration = false;
}

void *sshc_gc_malloc(size_t size)
{
    sshc_memory_context_t *ctx = sshc_memory_context_current();
    if (size == 0U) size = 1U;

    void *ptr = ttak_mem_alloc(size, SSH_CHATTER_DEFAULT_LIFETIME, ttak_get_tick_count());
    if (ptr == nullptr) return nullptr;

    sshc_memory_allocation_t *allocation =
        (sshc_memory_allocation_t *)ttak_mem_alloc(
            sizeof(*allocation), SSH_CHATTER_DEFAULT_LIFETIME,
            ttak_get_tick_count());
    if (allocation == nullptr) {
        ttak_mem_free(ptr);
        return nullptr;
    }

    allocation->ptr = ptr;
    allocation->size = size;
    allocation->context = ctx;
    allocation->next_in_context = nullptr;
    allocation->next_global = nullptr;
    allocation->gc_registered = false;
    allocation->creator_thread = pthread_self();

    sshc_memory_context_register_allocation(ctx, allocation);
    sshc_memory_registry_add(allocation);
    return ptr;
}

void *sshc_gc_realloc(void *ptr, size_t size)
{
    if (ptr == nullptr) return sshc_gc_malloc(size);
    if (size == 0U) {
        sshc_gc_free(ptr);
        return nullptr;
    }

    sshc_memory_context_t *ctx = sshc_memory_context_current();

    // Find old allocation
    pthread_mutex_lock(&sshc_registry_mutex);
    sshc_memory_allocation_t **prev = &sshc_allocations;
    sshc_memory_allocation_t *old_allocation = nullptr;
    while (*prev != nullptr) {
        if ((*prev)->ptr == ptr) {
            old_allocation = *prev;
            *prev = (*prev)->next_global;
            break;
        }
        prev = &(*prev)->next_global;
    }
    pthread_mutex_unlock(&sshc_registry_mutex);

    void *new_ptr = ttak_mem_realloc(ptr, size, SSH_CHATTER_DEFAULT_LIFETIME, ttak_get_tick_count());
    if (new_ptr == nullptr) {
        if (old_allocation) sshc_memory_registry_add(old_allocation);
        return nullptr;
    }

    sshc_memory_allocation_t *allocation = old_allocation;
    sshc_memory_context_t *allocation_ctx =
        old_allocation != nullptr ? old_allocation->context : ctx;

    if (old_allocation) {
#if defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC
        if (old_allocation->gc_registered) {
            GC_remove_roots(old_allocation->ptr,
                            (char *)old_allocation->ptr +
                                old_allocation->size);
        }
#endif
        sshc_memory_context_remove_allocation(old_allocation->context, ptr);

        // Safe release
        ttak_mem_node_t *node = ttak_mem_tree_find_node(&old_allocation->context->epoch_gc.tree, ptr);
        if (node) ttak_mem_node_release(node);
    } else {
        allocation = (sshc_memory_allocation_t *)ttak_mem_alloc(
            sizeof(*allocation), SSH_CHATTER_DEFAULT_LIFETIME,
            ttak_get_tick_count());
        if (allocation == nullptr) {
            return new_ptr;
        }
    }

    allocation->ptr = new_ptr;
    allocation->size = size;
    allocation->context = allocation_ctx;
    allocation->next_in_context = nullptr;
    allocation->next_global = nullptr;
    allocation->gc_registered = false;
    allocation->creator_thread = pthread_self();

    sshc_memory_context_register_allocation(allocation_ctx, allocation);
    sshc_memory_registry_add(allocation);
    return new_ptr;
}

void *sshc_gc_calloc(size_t count, size_t size)
{
    if (count == 0 || size == 0) return sshc_gc_malloc(0);
    size_t total = count * size;
    void *ptr = sshc_gc_malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void sshc_gc_free(void *ptr)
{
    if (ptr == nullptr) return;

    pthread_mutex_lock(&sshc_registry_mutex);
    sshc_memory_allocation_t **prev = &sshc_allocations;
    sshc_memory_allocation_t *allocation = nullptr;
    while (*prev != nullptr) {
        if ((*prev)->ptr == ptr) {
            allocation = *prev;
            *prev = (*prev)->next_global;
            break;
        }
        prev = &(*prev)->next_global;
    }
    pthread_mutex_unlock(&sshc_registry_mutex);

    if (allocation) {
        sshc_memory_context_remove_allocation(allocation->context, ptr);
#if defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC
        if (allocation->gc_registered) {
            GC_remove_roots(allocation->ptr,
                            (char *)allocation->ptr + allocation->size);
        }
#endif
        // Release reference in EpochGC tree. 
        // Background thread will then free ptr safely.
        ttak_mem_node_t *node = ttak_mem_tree_find_node(&allocation->context->epoch_gc.tree, ptr);
        if (node) {
            ttak_mem_node_release(node);
        } else {
            // Fallback if not in tree for some reason
            ttak_mem_free(ptr);
        }
        ttak_mem_free(allocation);
    } else {
        // Not tracked by our registry? Use direct free
        ttak_mem_free(ptr);
    }
}

void sshc_memory_context_reset(sshc_memory_context_t *ctx)
{
    if (ctx == nullptr) return;

    pthread_mutex_lock(&ctx->mutex);
    sshc_memory_allocation_t *allocation = ctx->allocations;
    ctx->allocations = nullptr;
    pthread_mutex_unlock(&ctx->mutex);

    while (allocation != nullptr) {
        sshc_memory_allocation_t *next = allocation->next_in_context;
        sshc_memory_registry_remove(allocation);
#if defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC
        if (allocation->gc_registered) {
            GC_remove_roots(allocation->ptr,
                            (char *)allocation->ptr + allocation->size);
        }
#endif
        // Safe release. Let background thread handle the actual free.
        ttak_mem_node_t *node = ttak_mem_tree_find_node(&ctx->epoch_gc.tree, allocation->ptr);
        if (node) {
            ttak_mem_node_release(node);
        } else {
            ttak_mem_free(allocation->ptr);
        }
        
        ttak_mem_free(allocation);
        allocation = next;
    }

    // Force rotation and cleanup pass
    ttak_epoch_gc_rotate(&ctx->epoch_gc);
}

void sshc_memory_context_epoch_gc_rotate(sshc_memory_context_t *ctx)
{
    if (ctx == nullptr) return;
    ttak_epoch_gc_rotate(&ctx->epoch_gc);
}

void sshc_gc_init(void) 
{ 
    sshc_memory_runtime_init(); 
}

/* ------------------------------------------------------------------ */
/* EBR wrappers -- thin helpers around libttak's epoch-based reclaimer */
/* ------------------------------------------------------------------ */

static void sshc_epoch_free_callback(void *ptr)
{
    ttak_mem_free(ptr);
}

void sshc_epoch_thread_enter(void)
{
    ttak_epoch_register_thread();
    ttak_epoch_enter();
}

void sshc_epoch_thread_exit(void)
{
    ttak_epoch_exit();
    ttak_epoch_reclaim();
    ttak_epoch_deregister_thread();
}

void sshc_epoch_retire(void *ptr)
{
    if (ptr == nullptr) return;
    ttak_epoch_retire(ptr, sshc_epoch_free_callback);
}

void sshc_epoch_retire_with(void *ptr, void (*cleanup)(void *))
{
    if (ptr == nullptr || cleanup == nullptr) {
        return;
    }
    ttak_epoch_retire(ptr, cleanup);
}

void sshc_epoch_reclaim(void)
{
    ttak_epoch_reclaim();
}

/* ------------------------------------------------------------------ */
/* Owner / detachable context accessors                               */
/* ------------------------------------------------------------------ */

tt_owner_t *sshc_memory_context_get_owner(sshc_memory_context_t *ctx)
{
    if (ctx == nullptr) return nullptr;
    return ctx->owner;
}

ttak_detachable_context_t *sshc_memory_context_get_detachable(
    sshc_memory_context_t *ctx)
{
    if (ctx == nullptr) return nullptr;
    return &ctx->detachable;
}

/* ------------------------------------------------------------------ */
/* Detachable memory wrappers                                         */
/* ------------------------------------------------------------------ */

ttak_detachable_allocation_t sshc_detachable_alloc(size_t size)
{
    sshc_memory_context_t *ctx = sshc_memory_context_current();
    uint64_t epoch_hint = ctx->epoch_gc.current_epoch;
    return ttak_detachable_mem_alloc(&ctx->detachable, size, epoch_hint);
}

void sshc_detachable_free(ttak_detachable_allocation_t *alloc)
{
    if (alloc == nullptr || alloc->data == nullptr) return;
    sshc_memory_context_t *ctx = sshc_memory_context_current();
    ttak_detachable_mem_free(&ctx->detachable, alloc);
}

/**
 * @file memory_manager.c
 * @desc Unified memory manager for SSH-Chatter using libttak for manual lifetimes
 *       and Boehm GC for automatic collection when enabled.
 *
 *       Integrates three libttak subsystems to govern variable lifecycles:
 *
 *       * Owner (tt_owner_t)
 *           Each memory context carries an owner that tracks live allocations.
 *           Resources are registered via ttak_owner_register_resource() so that
 *           the owner can audit and release them on context destruction.
 *
 *       * EpochGC (ttak_epoch_gc_t)
 *           Generational garbage collector backed by ttak_mem_tree.  All user
 *           allocations are registered via ttak_fastalloc / ttak_fastcalloc,
 *           which combine allocation + epoch registration in a single call.
 *           Calling epoch_gc_rotate() advances the epoch and frees expired
 *           blocks without a global pause.  ttak_epoch_gc_destroy() at context
 *           teardown reclaims every remaining tracked block.
 *
 *       * EBR (ttak_epoch_* / Epoch-Based Reclamation)
 *           Lock-free deferred reclamation for shared pointers.  A pointer
 *           passed to sshc_epoch_retire() is freed only after every thread has
 *           moved past the epoch in which the retirement occurred.
 *
 *  Memory lifecycle:
 *    - sshc_gc_malloc / sshc_gc_calloc: ttak_fastalloc / ttak_fastcalloc
 *      allocate and immediately register the block with the per-context
 *      epoch GC tree.  No separate gc_registered flag is needed.
 *    - sshc_gc_free: deterministically detaches the block from the GC tree
 *      and frees it, except session-scoped explicit deferred frees.
 *    - sshc_gc_realloc: allocates the new block via ttak_fastalloc,
 *      copies the payload, then releases the old GC node.
 *    - Context destroy: ttak_epoch_gc_destroy drains the tree and calls
 *      ttak_mem_free on every remaining registered block.
 */

#include "ssh_chatter/memory_manager.h"
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ttak/ht/map.h>
#include <signal.h>


typedef struct sshc_memory_allocation {
    void *ptr;
    size_t size;
    struct sshc_memory_allocation *next_in_context;
    /* Doubly-linked global list enables O(1) removal without a back-scan. */
    struct sshc_memory_allocation *next_global;
    struct sshc_memory_allocation *prev_global;
    struct sshc_memory_context *context;
    pthread_t creator_thread;
    /* gc_registered is removed: ttak_fastalloc always registers in epoch GC. */
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
    /** Per-context lifetime hint for allocations (ticks). */
    uint64_t max_lifetime_ticks;
};

static pthread_mutex_t sshc_registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool sshc_runtime_initialised = false;
static pthread_once_t sshc_alloc_limit_once = PTHREAD_ONCE_INIT;
static sshc_memory_context_t sshc_global_context;
static sshc_memory_context_t *sshc_contexts = nullptr;
static sshc_memory_allocation_t *sshc_allocations = nullptr;
/* Hash map: ptr → sshc_memory_allocation_t* for O(1) lookup in free/realloc. */
static ttak_map_t *sshc_alloc_map = nullptr;
static __thread sshc_memory_context_t *sshc_tls_context = nullptr;
static __thread bool sshc_tls_defer_gc_registration = false;
/* SIZE_MAX = no hard cap by default; override with CHATTER_MAX_ALLOC_BYTES. */
static size_t sshc_max_single_allocation_bytes = SIZE_MAX;

static void sshc_memory_load_alloc_limit_from_env(void)
{
    const char *raw = getenv("CHATTER_MAX_ALLOC_BYTES");
    if (raw == nullptr || raw[0] == '\0') {
        return;
    }

    errno = 0;
    char *end_ptr = nullptr;
    unsigned long long parsed = strtoull(raw, &end_ptr, 10);
    if (errno != 0 || end_ptr == raw || (end_ptr != nullptr && *end_ptr != '\0')) {
        return;
    }
    if (parsed == 0U || parsed > (unsigned long long)SIZE_MAX) {
        return;
    }

    sshc_max_single_allocation_bytes = (size_t)parsed;
}

static bool sshc_memory_validate_single_allocation(size_t size)
{
    pthread_once(&sshc_alloc_limit_once, sshc_memory_load_alloc_limit_from_env);
    /* When the limit is SIZE_MAX (default, uncapped) skip the check entirely. */
    if (sshc_max_single_allocation_bytes != SIZE_MAX &&
        size > sshc_max_single_allocation_bytes) {
        errno = ENOMEM;
        return false;
    }
    return true;
}

static bool sshc_env_truthy(const char *value)
{
    if (value == nullptr || value[0] == '\0') {
        return false;
    }

    return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
           strcmp(value, "TRUE") == 0 || strcmp(value, "yes") == 0 ||
           strcmp(value, "YES") == 0 || strcmp(value, "on") == 0 ||
           strcmp(value, "ON") == 0;
}

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

    /* EpochGC: per-context generational collector (local gc init<->destroy cycle). */
    ttak_epoch_gc_init(&ctx->epoch_gc);

    /* Reclamation: relaxed cadence to reduce CPU overhead under load.
     * The background thread still runs, but less aggressively. */
    ttak_mem_tree_set_manual_cleanup(&ctx->epoch_gc.tree, false);
    ttak_mem_tree_set_cleaning_intervals(&ctx->epoch_gc.tree,
                                         TT_MILLI_SECOND(100),
                                         TT_MILLI_SECOND(400));
    ctx->max_lifetime_ticks = 0;

}

static bool sshc_memory_context_deferred_free_enabled(
    const sshc_memory_context_t *ctx)
{
    return ctx != nullptr && ctx->max_lifetime_ticks > 0;
}

static uint64_t sshc_memory_context_lifetime(
    const sshc_memory_context_t *ctx)
{
    return sshc_memory_context_deferred_free_enabled(ctx)
               ? ctx->max_lifetime_ticks
               : __TTAK_UNSAFE_MEM_FOREVER__;
}

static void sshc_memory_context_refresh_node_expiry(
    sshc_memory_context_t *ctx, void *ptr)
{
    if (ctx == nullptr || ptr == nullptr) {
        return;
    }

    ttak_mem_node_t *node = ttak_mem_tree_find_node(&ctx->epoch_gc.tree, ptr);
    if (node == nullptr) {
        return;
    }

    pthread_mutex_lock(&node->lock);
    node->expires_tick = sshc_memory_context_deferred_free_enabled(ctx)
                             ? ttak_get_tick_count() + ctx->max_lifetime_ticks
                             : __TTAK_UNSAFE_MEM_FOREVER__;
    pthread_mutex_unlock(&node->lock);
}

static void sshc_memory_context_detach_and_free_ptr(
    sshc_memory_context_t *ctx, void *ptr)
{
    if (ptr == nullptr) {
        return;
    }

    if (ctx != nullptr) {
        ttak_mem_node_t *node =
            ttak_mem_tree_find_node(&ctx->epoch_gc.tree, ptr);
        if (node != nullptr) {
            ttak_mem_tree_remove(&ctx->epoch_gc.tree, node);
        }
    }
    ttak_mem_free(ptr);
}

static void sshc_memory_context_defer_ptr(
    sshc_memory_context_t *ctx, void *ptr)
{
    if (ctx == nullptr || ptr == nullptr) {
        ttak_mem_free(ptr);
        return;
    }

    ttak_mem_node_t *node = ttak_mem_tree_find_node(&ctx->epoch_gc.tree, ptr);
    if (node != nullptr) {
        pthread_mutex_lock(&node->lock);
        node->expires_tick = ttak_get_tick_count() + ctx->max_lifetime_ticks;
        pthread_mutex_unlock(&node->lock);
        ttak_mem_node_release(node);
    } else {
        ttak_mem_free(ptr);
    }
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
        /* Global TTAK tuning: slightly denser cleanup cadence and lower
         * pressure threshold to reduce deferred-epoch buildup. */
        ttak_mem_set_trace(
            sshc_env_truthy(getenv("SSH_CHATTER_MEM_TRACE")) ? 1 : 0);
        ttak_mem_configure_gc(TT_MILLI_SECOND(100), TT_MILLI_SECOND(500), 4096);

        /* Hash map for O(1) ptr → allocation* lookup (initial capacity 1024). */
        sshc_alloc_map = ttak_create_map(1024, ttak_get_tick_count());

        sshc_memory_context_init(&sshc_global_context, "global");
        
        sshc_global_context.next = nullptr;
        sshc_contexts = sshc_memory_context_global();
        sshc_crash_handler_init();
        sshc_runtime_initialised = true;
    }
    pthread_mutex_unlock(&sshc_registry_mutex);
}


static void sshc_memory_registry_remove_locked(sshc_memory_allocation_t *allocation)
{
    /* O(1) removal via doubly-linked list splice. */
    if (allocation->prev_global != nullptr) {
        allocation->prev_global->next_global = allocation->next_global;
    } else {
        sshc_allocations = allocation->next_global;
    }
    if (allocation->next_global != nullptr) {
        allocation->next_global->prev_global = allocation->prev_global;
    }
    allocation->next_global = nullptr;
    allocation->prev_global = nullptr;

    /* Remove from O(1) hash map. */
    if (sshc_alloc_map != nullptr) {
        ttak_delete_from_map(sshc_alloc_map, (uintptr_t)allocation->ptr,
                             ttak_get_tick_count());
    }
}

void sshc_memory_runtime_shutdown(void)
{
    pthread_mutex_lock(&sshc_registry_mutex);
    if (!sshc_runtime_initialised) {
        pthread_mutex_unlock(&sshc_registry_mutex);
        return;
    }

    /* Final EBR reclaim pass. */
    ttak_epoch_reclaim();

    sshc_memory_context_t *ctx = sshc_contexts;
    while (ctx != nullptr) {
        sshc_memory_context_t *next = ctx->next;
        if (ctx != sshc_memory_context_global()) {
            /* Drain the per-context tracking list.
             * Do NOT call ttak_mem_free on allocation->ptr here –
             * ttak_epoch_gc_destroy → ttak_mem_tree_destroy handles that. */
            pthread_mutex_lock(&ctx->mutex);
            sshc_memory_allocation_t *allocation = ctx->allocations;
            ctx->allocations = nullptr;
            pthread_mutex_unlock(&ctx->mutex);

            while (allocation != nullptr) {
                sshc_memory_allocation_t *next_alloc = allocation->next_in_context;
                sshc_memory_registry_remove_locked(allocation);
#if defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC
                GC_remove_roots(allocation->ptr,
                                (char *)allocation->ptr + allocation->size);
#endif
                /* Free the lightweight tracking node only; user ptr is
                 * reclaimed by ttak_epoch_gc_destroy below. */
                ttak_mem_free(allocation);
                allocation = next_alloc;
            }

            ttak_epoch_gc_destroy(&ctx->epoch_gc);
            if (ctx->owner) ttak_owner_destroy(ctx->owner);
            pthread_mutex_destroy(&ctx->mutex);
            ttak_mem_free(ctx);
        }
        ctx = next;
    }

    /* Clean up the global context. */
    pthread_mutex_lock(&sshc_global_context.mutex);
    sshc_memory_allocation_t *allocation = sshc_global_context.allocations;
    sshc_global_context.allocations = nullptr;
    pthread_mutex_unlock(&sshc_global_context.mutex);

    while (allocation != nullptr) {
        sshc_memory_allocation_t *next_alloc = allocation->next_in_context;
        sshc_memory_registry_remove_locked(allocation);
#if defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC
        GC_remove_roots(allocation->ptr,
                        (char *)allocation->ptr + allocation->size);
#endif
        /* Immediate free for deterministic shutdown. */
        ttak_mem_node_t *node = ttak_mem_tree_find_node(
            &sshc_global_context.epoch_gc.tree, allocation->ptr);
        if (node) {
            ttak_mem_tree_remove(&sshc_global_context.epoch_gc.tree, node);
        }
        ttak_mem_free(allocation->ptr);
        ttak_mem_free(allocation);
        allocation = next_alloc;
    }

    ttak_epoch_gc_destroy(&sshc_global_context.epoch_gc);
    if (sshc_global_context.owner) ttak_owner_destroy(sshc_global_context.owner);
    pthread_mutex_destroy(&sshc_global_context.mutex);

    /* Release the allocation hash map (free SoA arrays then the struct). */
    if (sshc_alloc_map != nullptr) {
        ttak_mem_free(sshc_alloc_map);
        sshc_alloc_map = nullptr;
    }

    sshc_contexts = nullptr;
    sshc_crash_handler_cleanup();
    sshc_runtime_initialised = false;
    pthread_mutex_unlock(&sshc_registry_mutex);
}


sshc_memory_context_t *sshc_memory_context_create(const char *label,
                                                      uint64_t max_lifetime_ticks)
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
    ctx->max_lifetime_ticks = max_lifetime_ticks;

    /* Session context: relaxed intervals to batch cleanups under churn.
     * Pressure threshold raised to 4 KiB so tiny allocations don't force
     * immediate background passes. */
    ttak_mem_tree_set_manual_cleanup(&ctx->epoch_gc.tree, false);
    ttak_mem_tree_set_cleaning_intervals(&ctx->epoch_gc.tree,
                                         TT_MILLI_SECOND(100),
                                         TT_MILLI_SECOND(400));
    ttak_mem_tree_set_pressure_threshold(&ctx->epoch_gc.tree, 4096);

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
    /* Prepend to doubly-linked global list. */
    allocation->next_global = sshc_allocations;
    allocation->prev_global = nullptr;
    if (sshc_allocations != nullptr) {
        sshc_allocations->prev_global = allocation;
    }
    sshc_allocations = allocation;
    /* Insert into O(1) hash map. */
    if (sshc_alloc_map != nullptr) {
        ttak_insert_to_map(sshc_alloc_map, (uintptr_t)allocation->ptr,
                           (size_t)(uintptr_t)allocation,
                           ttak_get_tick_count());
    }
    pthread_mutex_unlock(&sshc_registry_mutex);
}

void sshc_memory_context_destroy(sshc_memory_context_t *ctx)
{
    if (ctx == nullptr || ctx == sshc_memory_context_global()) {
        return;
    }

    sshc_memory_context_reset(ctx);
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

    /* ttak_fastalloc already registered the block with the epoch GC tree.
     * Do not register every allocation in tt_owner_t; that metadata list is
     * unbounded and grows under allocation churn even after free(). */

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
    /* All allocations are registered in the epoch GC immediately via
     * ttak_fastalloc; deferred registration is no longer needed.
     * This function is retained for ABI / caller compatibility. */
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
    if (!sshc_memory_validate_single_allocation(size)) {
        return nullptr;
    }

    /* ttak_fastalloc: allocates + registers in the per-context epoch GC tree
     * in a single call, replacing the former two-step ttak_mem_alloc +
     * ttak_epoch_gc_register pattern. */
    uint64_t lifetime = sshc_memory_context_lifetime(ctx);
    void *ptr = ttak_fastalloc(&ctx->epoch_gc, size, lifetime,
                               ttak_get_tick_count());
    if (ptr == nullptr) return nullptr;
    sshc_memory_context_refresh_node_expiry(ctx, ptr);

    sshc_memory_allocation_t *allocation =
        (sshc_memory_allocation_t *)ttak_mem_alloc(
            sizeof(*allocation), __TTAK_UNSAFE_MEM_FOREVER__,
            ttak_get_tick_count());
    if (allocation == nullptr) {
        sshc_memory_context_detach_and_free_ptr(ctx, ptr);
        return nullptr;
    }

    allocation->ptr = ptr;
    allocation->size = size;
    allocation->context = ctx;
    allocation->next_in_context = nullptr;
    allocation->next_global = nullptr;
    allocation->prev_global = nullptr;
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
    if (!sshc_memory_validate_single_allocation(size)) {
        return nullptr;
    }

    sshc_memory_context_t *ctx = sshc_memory_context_current();

    /* O(1) lookup via hash map to find the old allocation, then remove. */
    pthread_mutex_lock(&sshc_registry_mutex);
    sshc_memory_allocation_t *old_allocation = nullptr;
    if (sshc_alloc_map != nullptr) {
        size_t val = 0;
        if (ttak_map_get_key(sshc_alloc_map, (uintptr_t)ptr, &val,
                             ttak_get_tick_count())) {
            old_allocation = (sshc_memory_allocation_t *)(uintptr_t)val;
            sshc_memory_registry_remove_locked(old_allocation);
        }
    } else {
        /* Fallback: linear scan when map is not yet initialised. */
        sshc_memory_allocation_t **prev_p = &sshc_allocations;
        while (*prev_p != nullptr) {
            if ((*prev_p)->ptr == ptr) {
                old_allocation = *prev_p;
                sshc_memory_registry_remove_locked(old_allocation);
                break;
            }
            prev_p = &(*prev_p)->next_global;
        }
    }
    pthread_mutex_unlock(&sshc_registry_mutex);

    /* Allocate new block via ttak_fastalloc; copy payload; release old node. */
    sshc_memory_context_t *allocation_ctx =
        old_allocation != nullptr ? old_allocation->context : ctx;
    size_t old_size = old_allocation != nullptr ? old_allocation->size : 0;

    uint64_t lifetime = sshc_memory_context_lifetime(allocation_ctx);
    void *new_ptr = ttak_fastalloc(&allocation_ctx->epoch_gc, size,
                                   lifetime, ttak_get_tick_count());
    if (new_ptr == nullptr) {
        if (old_allocation) sshc_memory_registry_add(old_allocation);
        return nullptr;
    }
    sshc_memory_context_refresh_node_expiry(allocation_ctx, new_ptr);

    if (old_size > 0) {
        memcpy(new_ptr, ptr, old_size < size ? old_size : size);
    }

    if (old_allocation) {
#if defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC
        GC_remove_roots(old_allocation->ptr,
                        (char *)old_allocation->ptr + old_allocation->size);
#endif
        sshc_memory_context_remove_allocation(old_allocation->context, ptr);

        if (sshc_memory_context_deferred_free_enabled(old_allocation->context)) {
            sshc_memory_context_defer_ptr(old_allocation->context, ptr);
        } else {
            sshc_memory_context_detach_and_free_ptr(old_allocation->context,
                                                    ptr);
        }
    }

    sshc_memory_allocation_t *allocation = old_allocation;
    if (allocation == nullptr) {
        allocation = (sshc_memory_allocation_t *)ttak_mem_alloc(
            sizeof(*allocation), __TTAK_UNSAFE_MEM_FOREVER__,
            ttak_get_tick_count());
        if (allocation == nullptr) {
            sshc_memory_context_detach_and_free_ptr(allocation_ctx, new_ptr);
            return nullptr;
        }
    }

    allocation->ptr = new_ptr;
    allocation->size = size;
    allocation->context = allocation_ctx;
    allocation->next_in_context = nullptr;
    allocation->next_global = nullptr;
    allocation->prev_global = nullptr;
    allocation->creator_thread = pthread_self();

    sshc_memory_context_register_allocation(allocation_ctx, allocation);
    sshc_memory_registry_add(allocation);
    return new_ptr;
}

void *sshc_gc_calloc(size_t count, size_t size)
{
    if (count == 0 || size == 0) return sshc_gc_malloc(0);
    /* Guard against multiplicative integer overflow before allocating. */
    if (size > SIZE_MAX / count) {
        errno = ENOMEM;
        return nullptr;
    }
    /* ttak_mem_alloc (used by ttak_fastalloc) returns zeroed memory; no
     * explicit memset needed.  Use sshc_gc_malloc which calls ttak_fastalloc
     * internally. */
    size_t total = count * size;
    if (!sshc_memory_validate_single_allocation(total)) {
        return nullptr;
    }
    return sshc_gc_malloc(total);
}

static void sshc_secure_zero(void *ptr, size_t len)
{
    if (ptr == nullptr || len == 0U) {
        return;
    }

    volatile unsigned char *p = (volatile unsigned char *)ptr;
    for (size_t idx = 0U; idx < len; ++idx) {
        p[idx] = 0U;
    }
}

void sshc_gc_free(void *ptr)
{
    if (ptr == nullptr) return;

    /* O(1) lookup via hash map; fall back to linear scan when map absent. */
    pthread_mutex_lock(&sshc_registry_mutex);
    sshc_memory_allocation_t *allocation = nullptr;
    if (sshc_alloc_map != nullptr) {
        size_t val = 0;
        if (ttak_map_get_key(sshc_alloc_map, (uintptr_t)ptr, &val,
                             ttak_get_tick_count())) {
            allocation = (sshc_memory_allocation_t *)(uintptr_t)val;
            sshc_memory_registry_remove_locked(allocation);
        }
    } else {
        sshc_memory_allocation_t **prev_p = &sshc_allocations;
        while (*prev_p != nullptr) {
            if ((*prev_p)->ptr == ptr) {
                allocation = *prev_p;
                sshc_memory_registry_remove_locked(allocation);
                break;
            }
            prev_p = &(*prev_p)->next_global;
        }
    }
    pthread_mutex_unlock(&sshc_registry_mutex);

    if (allocation) {
        sshc_memory_context_remove_allocation(allocation->context, ptr);
#if defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC
        GC_remove_roots(allocation->ptr,
                        (char *)allocation->ptr + allocation->size);
#endif
        sshc_secure_zero(allocation->ptr, allocation->size);
        if (sshc_memory_context_deferred_free_enabled(allocation->context)) {
            sshc_memory_context_defer_ptr(allocation->context, ptr);
        } else {
            sshc_memory_context_detach_and_free_ptr(allocation->context, ptr);
        }
        ttak_mem_free(allocation);
    } else {
        /* Unknown to this registry: treat as already released or foreign.
         * Calling ttak_mem_free here can turn shutdown double-cleanup into
         * lock-after-free on libttak's allocation header. */
        return;
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
        GC_remove_roots(allocation->ptr,
                        (char *)allocation->ptr + allocation->size);
#endif
        sshc_memory_context_detach_and_free_ptr(ctx, allocation->ptr);
        ttak_mem_free(allocation);
        allocation = next;
    }

    /* Per-user GC rotate: single-pass cleanup of every unreferenced block
     * belonging to this session. */
    ttak_epoch_gc_rotate(&ctx->epoch_gc);
}

void sshc_memory_context_epoch_gc_rotate(sshc_memory_context_t *ctx)
{
    if (ctx == nullptr) return;
    ttak_epoch_gc_rotate(&ctx->epoch_gc);
}

void sshc_memory_context_set_gc_aggressive(sshc_memory_context_t *ctx)
{
    if (ctx == nullptr) return;
    ttak_mem_tree_set_cleaning_intervals(&ctx->epoch_gc.tree,
                                         TT_MILLI_SECOND(100),
                                         TT_MILLI_SECOND(400));
    ttak_mem_tree_set_pressure_threshold(&ctx->epoch_gc.tree, 4096);
}

void sshc_memory_context_set_gc_relaxed(sshc_memory_context_t *ctx)
{
    if (ctx == nullptr) return;
    ttak_mem_tree_set_cleaning_intervals(&ctx->epoch_gc.tree,
                                         TT_MILLI_SECOND(2000),
                                         TT_MILLI_SECOND(10000));
    ttak_mem_tree_set_pressure_threshold(&ctx->epoch_gc.tree, 65536);
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
/* Owner context accessor                                             */
/* ------------------------------------------------------------------ */

tt_owner_t *sshc_memory_context_get_owner(sshc_memory_context_t *ctx)
{
    if (ctx == nullptr) return nullptr;
    return ctx->owner;
}

__thread sigjmp_buf g_sshc_safe_jmpbuf;
__thread bool g_sshc_safe_active = false;

static struct sigaction g_old_segv_action;
static struct sigaction g_old_bus_action;

static void sshc_crash_signal_handler(int sig, siginfo_t *info, void *context)
{
    if (g_sshc_safe_active) {
        siglongjmp(g_sshc_safe_jmpbuf, 1);
    }
    struct sigaction *old_act = (sig == SIGSEGV) ? &g_old_segv_action : &g_old_bus_action;
    if (old_act->sa_flags & SA_SIGINFO) {
        if (old_act->sa_sigaction != nullptr) {
            old_act->sa_sigaction(sig, info, context);
            return;
        }
    } else {
        if (old_act->sa_handler == SIG_DFL) {
            signal(sig, SIG_DFL);
            raise(sig);
            return;
        } else if (old_act->sa_handler != SIG_IGN && old_act->sa_handler != nullptr) {
            old_act->sa_handler(sig);
            return;
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void sshc_crash_handler_init(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sshc_crash_signal_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_old_segv_action);
    sigaction(SIGBUS, &sa, &g_old_bus_action);
}

void sshc_crash_handler_cleanup(void)
{
    sigaction(SIGSEGV, &g_old_segv_action, nullptr);
    sigaction(SIGBUS, &g_old_bus_action, nullptr);
}

bool sshc_memory_is_valid_gc_pointer(const void *ptr)
{
    if (ptr == nullptr) {
        return false;
    }
    pthread_mutex_lock(&sshc_registry_mutex);
    bool found = false;
    if (sshc_alloc_map != nullptr) {
        size_t val = 0;
        if (ttak_map_get_key(sshc_alloc_map, (uintptr_t)ptr, &val,
                             ttak_get_tick_count())) {
            found = true;
        }
    } else {
        sshc_memory_allocation_t *curr = sshc_allocations;
        while (curr != nullptr) {
            if (curr->ptr == ptr) {
                found = true;
                break;
            }
            curr = curr->next_global;
        }
    }
    pthread_mutex_unlock(&sshc_registry_mutex);
    return found;
}

bool sshc_safe_read(const void *src, void *dst, size_t size)
{
    if (src == nullptr || dst == nullptr || size == 0) {
        return false;
    }
    bool ok = false;
    SSHC_SAFE_BLOCK_BEGIN() {
        memcpy(dst, src, size);
        ok = true;
    } SSHC_SAFE_BLOCK_END({
        ok = false;
    });
    return ok;
}

bool sshc_pointer_check(const void *ptr, size_t size)
{
    if (ptr == nullptr) {
        return false;
    }
    char temp;
    if (!sshc_safe_read(ptr, &temp, 1)) {
        return false;
    }
    if (size > 1) {
        const char *end_ptr = (const char *)ptr + size - 1;
        if (!sshc_safe_read(end_ptr, &temp, 1)) {
            return false;
        }
    }
    return true;
}


/**
 * @file memory_manager.c
 * @desc Unified memory manager for SSH-Chatter using libttak for manual lifetimes
 *       and Boehm GC for automatic collection when enabled.
 *       Integrates EpochGC for generational cleanup and EBR for safe deferred
 *       reclamation of shared data structures.
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
} sshc_memory_allocation_t;

struct sshc_memory_context {
    pthread_mutex_t mutex;
    sshc_memory_allocation_t *allocations;
    const char *label;
    struct sshc_memory_context *next;
    tt_owner_t *owner;
    ttak_epoch_gc_t epoch_gc;
};

static pthread_mutex_t sshc_registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool sshc_runtime_initialised = false;
static sshc_memory_context_t sshc_global_context;
static sshc_memory_context_t *sshc_contexts = nullptr;
static sshc_memory_allocation_t *sshc_allocations = nullptr;
static __thread sshc_memory_context_t *sshc_tls_context = nullptr;

static void sshc_memory_context_init(sshc_memory_context_t *ctx,
                                     const char *label)
{
    pthread_mutex_init(&ctx->mutex, nullptr);
    ctx->allocations = nullptr;
    ctx->label = label;
    ctx->next = nullptr;
    ctx->owner = ttak_owner_create(TTAK_OWNER_SAFE_DEFAULT);
    ttak_epoch_gc_init(&ctx->epoch_gc);
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
        // Boehm GC Tuning: Reduce stop-the-world frequency by allowing more free space
        // and setting a moderate free space divisor.
        GC_set_free_space_divisor(10); 
        GC_init(); // Call libgc's init
#endif
        // Initialize ttak memory system
        ttak_mem_set_trace(0); // Disable tracing by default for performance
        
        // Configure ttak's background cleaning: 100ms min, 1s max, 100MB pressure
        ttak_mem_configure_gc(TT_MILLI_SECOND(100), TT_SECOND(1), 100 * 1024 * 1024);

        sshc_memory_context_init(&sshc_global_context, "global");
        sshc_global_context.next = nullptr;
        sshc_contexts = sshc_memory_context_global();
        sshc_runtime_initialised = true;
    }
    pthread_mutex_unlock(&sshc_registry_mutex);
}

void sshc_memory_runtime_shutdown(void)
{
    pthread_mutex_lock(&sshc_registry_mutex);
    if (!sshc_runtime_initialised) {
        pthread_mutex_unlock(&sshc_registry_mutex);
        return;
    }

    // Final EBR reclaim pass before tearing down contexts
    ttak_epoch_reclaim();

    sshc_memory_context_t *ctx = sshc_contexts;
    while (ctx != nullptr) {
        sshc_memory_context_t *next = ctx->next;
        if (ctx != sshc_memory_context_global()) {
            // We can't call sshc_memory_context_destroy here because it locks registry
            // So we do manual cleanup
            sshc_memory_context_reset(ctx);
            ttak_epoch_gc_destroy(&ctx->epoch_gc);
            if (ctx->owner) ttak_owner_destroy(ctx->owner);
            pthread_mutex_destroy(&ctx->mutex);
            ttak_mem_free(ctx);
        }
        ctx = next;
    }
    // Clean up the global context's allocations
    sshc_memory_context_reset(sshc_memory_context_global());
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

    pthread_mutex_lock(&sshc_registry_mutex);
    ctx->next = sshc_contexts;
    sshc_contexts = ctx;
    pthread_mutex_unlock(&sshc_registry_mutex);
    return ctx;
}

static void sshc_memory_registry_remove(sshc_memory_allocation_t *allocation)
{
    pthread_mutex_lock(&sshc_registry_mutex);
    sshc_memory_allocation_t **prev = &sshc_allocations;
    while (*prev != nullptr) {
        if (*prev == allocation) {
            *prev = allocation->next_global;
            break;
        }
        prev = &(*prev)->next_global;
    }
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
    
    // Register with EpochGC for generational tracking
    ttak_epoch_gc_register(&ctx->epoch_gc, allocation->ptr, allocation->size);

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

void *sshc_gc_malloc(size_t size)
{
    sshc_memory_context_t *ctx = sshc_memory_context_current();
    if (size == 0U) size = 1U;

    void *ptr = nullptr;
    ptr = ttak_mem_alloc(size, __TTAK_UNSAFE_MEM_FOREVER__, ttak_get_tick_count());

    if (ptr == nullptr) return nullptr;

    sshc_memory_allocation_t *allocation =
        (sshc_memory_allocation_t *)ttak_mem_alloc(
            sizeof(*allocation), __TTAK_UNSAFE_MEM_FOREVER__,
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

    void *new_ptr = ttak_mem_realloc(ptr, size, __TTAK_UNSAFE_MEM_FOREVER__, ttak_get_tick_count());
    if (new_ptr == nullptr) {
        if (old_allocation) sshc_memory_registry_add(old_allocation);
        return nullptr;
    }

    sshc_memory_allocation_t *allocation =
        (sshc_memory_allocation_t *)ttak_mem_alloc(
            sizeof(*allocation), __TTAK_UNSAFE_MEM_FOREVER__,
            ttak_get_tick_count());
    if (allocation == nullptr) {
        // We reallocated but can't track. This is bad.
        if (old_allocation) {
#if defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC
            GC_remove_roots(old_allocation->ptr, (char *)old_allocation->ptr + old_allocation->size);
#endif
            sshc_memory_context_remove_allocation(old_allocation->context, ptr);
            ttak_mem_free(old_allocation);
        }
        return new_ptr;
    }

    allocation->ptr = new_ptr;
    allocation->size = size;
    allocation->context = ctx;
    
    if (old_allocation) {
#if defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC
        GC_remove_roots(old_allocation->ptr, (char *)old_allocation->ptr + old_allocation->size);
#endif
        sshc_memory_context_remove_allocation(old_allocation->context, ptr);
        ttak_mem_free(old_allocation);
    }

    sshc_memory_context_register_allocation(ctx, allocation);
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
        GC_remove_roots(allocation->ptr, (char *)allocation->ptr + allocation->size);
#endif
        ttak_mem_free(allocation);
    }

    ttak_mem_free(ptr);
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
        GC_remove_roots(allocation->ptr, (char *)allocation->ptr + allocation->size);
#endif
        ttak_mem_free(allocation->ptr);
        ttak_mem_free(allocation);
        allocation = next;
    }

    // Rotate the epoch GC to free expired blocks tracked by the tree
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
/* EBR wrappers – thin helpers around libttak's epoch-based reclaimer */
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

void sshc_epoch_reclaim(void)
{
    ttak_epoch_reclaim();
}

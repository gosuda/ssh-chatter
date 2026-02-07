/**
 * @file memory_manager.c
 * @desc Unified memory manager for SSH-Chatter using libttak for manual lifetimes
 *       and Boehm GC for automatic collection when enabled.
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
        GC_INIT();
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

    sshc_memory_context_t *ctx = sshc_contexts;
    while (ctx != nullptr) {
        sshc_memory_context_t *next = ctx->next;
        if (ctx != sshc_memory_context_global()) {
            // We can't call sshc_memory_context_destroy here because it locks registry
            // So we do manual cleanup
            sshc_memory_context_reset(ctx);
            if (ctx->owner) ttak_owner_destroy(ctx->owner);
            pthread_mutex_destroy(&ctx->mutex);
            free(ctx);
        }
        ctx = next;
    }
    // Clean up the global context's allocations
    sshc_memory_context_reset(sshc_memory_context_global());
    if (sshc_global_context.owner) ttak_owner_destroy(sshc_global_context.owner);
    pthread_mutex_destroy(&sshc_global_context.mutex);
    
    sshc_contexts = nullptr;
    sshc_runtime_initialised = false;
    pthread_mutex_unlock(&sshc_registry_mutex);
}

sshc_memory_context_t *sshc_memory_context_create(const char *label)
{
    sshc_memory_runtime_init();
    sshc_memory_context_t *ctx = (sshc_memory_context_t *)malloc(sizeof(*ctx));
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
    free(ctx);
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

void *GC_MALLOC(size_t size)
{
    sshc_memory_context_t *ctx = sshc_memory_context_current();
    if (size == 0U) size = 1U;

    void *ptr = nullptr;
#if defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC
    // Extreme combination: Use ttak's safe allocation but with Boehm GC backing if possible.
    // Since ttak_mem_alloc_safe uses malloc, we'll use it for manual lifetime management,
    // and Boehm GC will still see pointers in the stack.
    ptr = ttak_mem_alloc(size, __TTAK_UNSAFE_MEM_FOREVER__, ttak_get_tick_count());
#else
    ptr = ttak_mem_alloc(size, __TTAK_UNSAFE_MEM_FOREVER__, ttak_get_tick_count());
#endif

    if (ptr == nullptr) return nullptr;

    sshc_memory_allocation_t *allocation = (sshc_memory_allocation_t *)malloc(sizeof(*allocation));
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

void *GC_REALLOC(void *ptr, size_t size)
{
    if (ptr == nullptr) return GC_MALLOC(size);
    if (size == 0U) {
        GC_free(ptr);
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

    sshc_memory_allocation_t *allocation = (sshc_memory_allocation_t *)malloc(sizeof(*allocation));
    if (allocation == nullptr) {
        // We reallocated but can't track. This is bad.
        if (old_allocation) {
#if defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC
            GC_remove_roots(old_allocation->ptr, (char *)old_allocation->ptr + old_allocation->size);
#endif
            sshc_memory_context_remove_allocation(old_allocation->context, ptr);
            free(old_allocation);
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
        free(old_allocation);
    }

    sshc_memory_context_register_allocation(ctx, allocation);
    sshc_memory_registry_add(allocation);
    return new_ptr;
}

void *GC_CALLOC(size_t count, size_t size)
{
    if (count == 0 || size == 0) return GC_MALLOC(0);
    size_t total = count * size;
    void *ptr = GC_MALLOC(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void GC_free(void *ptr)
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
        free(allocation);
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
        free(allocation);
        allocation = next;
    }
}

#if !(defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC)
void GC_INIT(void) { sshc_memory_runtime_init(); }
#endif

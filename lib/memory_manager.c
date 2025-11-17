#include "headers/memory_manager.h"

#if !(defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC)

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
}

static sshc_memory_context_t *sshc_memory_context_global(void)
{
    return &sshc_global_context;
}

void sshc_memory_runtime_init(void)
{
    pthread_mutex_lock(&sshc_registry_mutex);
    if (!sshc_runtime_initialised) {
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
    sshc_memory_context_t *ctx = sshc_contexts;
    while (ctx != nullptr) {
        sshc_memory_context_t *next = ctx->next;
        if (ctx != sshc_memory_context_global()) {
            sshc_memory_context_destroy(ctx);
        }
        ctx = next;
    }
    // Clean up the global context's allocations
    sshc_memory_context_reset(sshc_memory_context_global());
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
    if (ctx == nullptr) {
        return;
    }

    sshc_memory_context_reset(ctx);
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
}

static void *sshc_memory_context_alloc(sshc_memory_context_t *ctx, size_t size,
                                       bool zero)
{
    if (ctx == nullptr) {
        ctx = sshc_memory_context_current();
    }

    if (size == 0U) {
        size = 1U;
    }

    void *ptr = zero ? calloc(1U, size) : malloc(size);
    if (ptr == nullptr) {
        errno = ENOMEM;
        return nullptr;
    }

    sshc_memory_allocation_t *allocation =
        (sshc_memory_allocation_t *)malloc(sizeof(*allocation));
    if (allocation == nullptr) {
        free(ptr);
        errno = ENOMEM;
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

static void *sshc_memory_context_realloc(sshc_memory_context_t *ctx, void *ptr,
                                         size_t size, bool zero) {
    if (ctx == nullptr) {
        ctx = sshc_memory_context_current();
    }

    if(size == 0U) {
        size = 1U;
    }

    // If ptr is null, just allocate new memory
    if(ptr == nullptr) {
        return sshc_memory_context_alloc(ctx, size, zero);
    }

    // Find and remove the old allocation entry
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

    // Perform the realloc
    void *new_ptr = realloc(ptr, size);
    if (new_ptr == nullptr) {
        // Realloc failed, restore the old allocation entry
        if (old_allocation != nullptr) {
            sshc_memory_registry_add(old_allocation);
        }
        errno = ENOMEM;
        return nullptr;
    }

    // Create a new allocation entry for the reallocated memory
    sshc_memory_allocation_t *allocation =
        (sshc_memory_allocation_t *)malloc(sizeof(*allocation));
    if(allocation == nullptr) {
        // Can't track the allocation, but the memory was reallocated successfully
        // Clean up the old allocation entry if it exists
        if (old_allocation != nullptr) {
            if (old_allocation->context != nullptr) {
                sshc_memory_context_remove_allocation(old_allocation->context, ptr);
            }
            free(old_allocation);
        }
        errno = ENOMEM;
        return new_ptr;
    }

    // Set up the new allocation entry
    allocation->ptr = new_ptr;
    allocation->size = size;
    allocation->context = ctx;
    allocation->next_in_context = nullptr;
    allocation->next_global = nullptr;
    
    // Clean up the old allocation entry
    if (old_allocation != nullptr) {
        if (old_allocation->context != nullptr) {
            sshc_memory_context_remove_allocation(old_allocation->context, ptr);
        }
        free(old_allocation);
    }

    // Register the new allocation
    sshc_memory_context_register_allocation(ctx, allocation);
    sshc_memory_registry_add(allocation);

    return new_ptr;
}

void sshc_memory_context_reset(sshc_memory_context_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }

    pthread_mutex_lock(&ctx->mutex);
    sshc_memory_allocation_t *allocation = ctx->allocations;
    ctx->allocations = nullptr;
    pthread_mutex_unlock(&ctx->mutex);

    while (allocation != nullptr) {
        sshc_memory_allocation_t *next = allocation->next_in_context;
        sshc_memory_registry_remove(allocation);
        free(allocation->ptr);
        free(allocation);
        allocation = next;
    }
}

void GC_INIT(void)
{
    sshc_memory_runtime_init();
}

void *GC_MALLOC(size_t size)
{
    return sshc_memory_context_alloc(sshc_memory_context_current(), size,
                                     false);
}

void *GC_REALLOC(void *ptr, size_t size) {
    return sshc_memory_context_realloc(sshc_memory_context_current(), ptr, size,
                                       false);
}

void *GC_CALLOC(size_t count, size_t size)
{
    if (count == 0U || size == 0U) {
        return sshc_memory_context_alloc(sshc_memory_context_current(), 1U,
                                         true);
    }
    if (count > SIZE_MAX / size) {
        errno = ENOMEM;
        return nullptr;
    }
    return sshc_memory_context_alloc(sshc_memory_context_current(),
                                     count * size, true);
}

void GC_FREE(void *ptr)
{
    if (ptr == nullptr) {
        return;
    }

    sshc_memory_runtime_init();
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

    if (allocation == nullptr) {
        free(ptr);
        return;
    }

    sshc_memory_context_t *ctx = allocation->context;
    if (ctx != nullptr) {
        sshc_memory_context_remove_allocation(ctx, ptr);
    }

    free(allocation->ptr);
    free(allocation);
}

#endif

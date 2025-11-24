#ifndef SSH_CHATTER_MEMORY_MANAGER_H
#define SSH_CHATTER_MEMORY_MANAGER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sshc_memory_context sshc_memory_context_t;

#if defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC
#include <gc/gc.h>

struct sshc_memory_context {
};

static inline void sshc_memory_runtime_init(void)
{
    GC_INIT();
}

static inline void sshc_memory_runtime_shutdown(void)
{
}

static inline sshc_memory_context_t *
sshc_memory_context_create(const char *label)
{
    (void)label;
    static sshc_memory_context_t dummy_context;
    return &dummy_context;
}

static inline void sshc_memory_context_destroy(sshc_memory_context_t *ctx)
{
    (void)ctx;
}

static inline sshc_memory_context_t *
sshc_memory_context_push(sshc_memory_context_t *ctx)
{
    (void)ctx;
    return nullptr;
}

static inline void sshc_memory_context_pop(sshc_memory_context_t *previous)
{
    (void)previous;
}

static inline void sshc_memory_context_reset(sshc_memory_context_t *ctx)
{
    (void)ctx;
}

static inline sshc_memory_context_t *sshc_memory_context_current(void)
{
    return nullptr;
}

static inline void *GC_CALLOC(size_t count, size_t size)
{
    if (count == 0U || size == 0U) {
        return GC_MALLOC(0U);
    }

    if (count > SIZE_MAX / size) {
        return nullptr;
    }

    size_t total = count * size;
    void *ptr = GC_MALLOC(total);
    if (ptr == nullptr) {
        return nullptr;
    }
    memset(ptr, 0, total);
    return ptr;
}

static inline char *sshc_strdup(const char *text)
{
    if (text == nullptr) {
        return nullptr;
    }
    return GC_strdup(text);
}

#else

#include <stdbool.h>

void sshc_memory_runtime_init(void);
void sshc_memory_runtime_shutdown(void);

sshc_memory_context_t *sshc_memory_context_create(const char *label);
void sshc_memory_context_destroy(sshc_memory_context_t *ctx);
sshc_memory_context_t *sshc_memory_context_push(sshc_memory_context_t *ctx);
void sshc_memory_context_pop(sshc_memory_context_t *previous);
void sshc_memory_context_reset(sshc_memory_context_t *ctx);
sshc_memory_context_t *sshc_memory_context_current(void);

void GC_INIT(void);
void *GC_MALLOC(size_t size);
void *GC_REALLOC(void *ptr, size_t size);
void GC_free(void *ptr);
#define GC_FREE(x) GC_free(x)
void *GC_CALLOC(size_t count, size_t size);

static inline char *sshc_strdup(const char *text)
{
    if (text == nullptr) {
        return nullptr;
    }

    size_t length = strlen(text) + 1U;
    char *copy = (char *)GC_MALLOC(length);
    if (copy == nullptr) {
        return nullptr;
    }

    memcpy(copy, text, length);
    return copy;
}

#endif

#ifdef __cplusplus
}
#endif

#endif

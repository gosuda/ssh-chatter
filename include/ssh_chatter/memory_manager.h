#ifndef SSH_CHATTER_MEMORY_MANAGER_H
#define SSH_CHATTER_MEMORY_MANAGER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include <ttak/mem/mem.h>
#include <ttak/mem/owner.h>
#include <ttak/timing/timing.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sshc_memory_context sshc_memory_context_t;

void sshc_memory_runtime_init(void);
void sshc_memory_runtime_shutdown(void);

sshc_memory_context_t *sshc_memory_context_create(const char *label);
void sshc_memory_context_destroy(sshc_memory_context_t *ctx);
sshc_memory_context_t *sshc_memory_context_push(sshc_memory_context_t *ctx);
void sshc_memory_context_pop(sshc_memory_context_t *previous);
void sshc_memory_context_reset(sshc_memory_context_t *ctx);
sshc_memory_context_t *sshc_memory_context_current(void);

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
    char *copy = (char *)GC_MALLOC(length);
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

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

#if defined(SSH_CHATTER_USE_GC) && SSH_CHATTER_USE_GC
#include <gc/gc.h>
#ifndef GC_FREE
#define GC_FREE(x) GC_free(x)
#endif
#else
void GC_INIT(void);
void GC_free(void *ptr);
#ifndef GC_FREE
#define GC_FREE(x) GC_free(x)
#endif
#endif

void *GC_MALLOC(size_t size);
void *GC_REALLOC(void *ptr, size_t size);
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

#ifdef __cplusplus
}
#endif

#endif

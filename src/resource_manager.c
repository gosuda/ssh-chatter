/**
 * @file resource_manager.c
 * @desc On-demand allocate / immediate-free registry on top of libttak's
 *       abstract allocator.  See include/ssh_chatter/resource_manager.h
 *       for the contract.
 *
 *       Both modes (scoped, keyed) share a single linked-list registry
 *       protected by a mutex.  Volume is low (one entry per active
 *       launch / per shared cache), so a list scan dominates only at
 *       shutdown audit time and is acceptable.
 */

#include "ssh_chatter/resource_manager.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ssh_chatter/humanized/humanized.h"

typedef enum rm_slot_mode {
    RM_SLOT_MODE_SCOPED = 0,
    RM_SLOT_MODE_KEYED = 1,
} rm_slot_mode_t;

typedef struct rm_slot {
    struct rm_slot *next;
    rm_slot_mode_t mode;
    ttak_abstract_mem_t *handle;
    size_t byte_size;
    char *key; /* keyed mode only; owned */
    char *tag; /* scoped mode label, owned, may be NULL */
    sshc_rm_destructor_fn destructor;
    void *user_data;
    uint32_t refcount; /* keyed mode only; scoped slots use 1 */
} rm_slot_t;

struct sshc_resource_manager {
    char *label;
    pthread_mutex_t lock;
    rm_slot_t *head;
    size_t live_handles;
    size_t total_bytes;
};

/* --- helpers ----------------------------------------------------------- */

static char *rm_strdup_safe(const char *s)
{
    if (s == nullptr) {
        return nullptr;
    }
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1U);
    if (out == nullptr) {
        return nullptr;
    }
    memcpy(out, s, n + 1U);
    return out;
}

static rm_slot_t *rm_find_by_handle(sshc_resource_manager_t *rm,
                                    const ttak_abstract_mem_t *handle)
{
    for (rm_slot_t *cur = rm->head; cur != nullptr; cur = cur->next) {
        if (cur->handle == handle) {
            return cur;
        }
    }
    return nullptr;
}

static rm_slot_t *rm_find_by_key(sshc_resource_manager_t *rm, const char *key)
{
    if (key == nullptr) {
        return nullptr;
    }
    for (rm_slot_t *cur = rm->head; cur != nullptr; cur = cur->next) {
        if (cur->mode == RM_SLOT_MODE_KEYED && cur->key != nullptr &&
            strcmp(cur->key, key) == 0) {
            return cur;
        }
    }
    return nullptr;
}

static void rm_unlink_and_free_slot(sshc_resource_manager_t *rm, rm_slot_t *slot)
{
    rm_slot_t **link = &rm->head;
    while (*link != nullptr && *link != slot) {
        link = &(*link)->next;
    }
    if (*link == slot) {
        *link = slot->next;
    }
    if (rm->live_handles > 0U) {
        rm->live_handles -= 1U;
    }
    if (rm->total_bytes >= slot->byte_size) {
        rm->total_bytes -= slot->byte_size;
    } else {
        rm->total_bytes = 0U;
    }
    free(slot->key);
    free(slot->tag);
    free(slot);
}

/* --- lifecycle --------------------------------------------------------- */

sshc_resource_manager_t *sshc_resource_manager_create(const char *label)
{
    sshc_resource_manager_t *rm =
        (sshc_resource_manager_t *)calloc(1U, sizeof(*rm));
    if (rm == nullptr) {
        humanized_log_error("rm", "resource manager alloc failed", ENOMEM);
        return nullptr;
    }
    rm->label = rm_strdup_safe(label != nullptr ? label : "unnamed");
    if (pthread_mutex_init(&rm->lock, nullptr) != 0) {
        humanized_log_error("rm", "mutex init failed", errno);
        free(rm->label);
        free(rm);
        return nullptr;
    }
    return rm;
}

void sshc_resource_manager_destroy(sshc_resource_manager_t *rm)
{
    if (rm == nullptr) {
        return;
    }

    pthread_mutex_lock(&rm->lock);
    rm_slot_t *cur = rm->head;
    rm->head = nullptr;
    size_t leaked = rm->live_handles;
    rm->live_handles = 0U;
    rm->total_bytes = 0U;
    pthread_mutex_unlock(&rm->lock);

    if (leaked > 0U) {
        humanized_log_error("rm", "destroying with live handles", 0);
    }

    while (cur != nullptr) {
        rm_slot_t *next = cur->next;
        if (cur->destructor != nullptr && cur->handle != nullptr) {
            cur->destructor(cur->handle, cur->user_data);
        }
        ttak_abstract_free(cur->handle);
        free(cur->key);
        free(cur->tag);
        free(cur);
        cur = next;
    }

    pthread_mutex_destroy(&rm->lock);
    free(rm->label);
    free(rm);
}

void sshc_resource_manager_stats(const sshc_resource_manager_t *rm,
                                 size_t *out_live_handles,
                                 size_t *out_total_bytes)
{
    if (rm == nullptr) {
        if (out_live_handles != nullptr) {
            *out_live_handles = 0U;
        }
        if (out_total_bytes != nullptr) {
            *out_total_bytes = 0U;
        }
        return;
    }
    /* const cast is intentional: we only need the lock for a read. */
    pthread_mutex_lock(&((sshc_resource_manager_t *)rm)->lock);
    if (out_live_handles != nullptr) {
        *out_live_handles = rm->live_handles;
    }
    if (out_total_bytes != nullptr) {
        *out_total_bytes = rm->total_bytes;
    }
    pthread_mutex_unlock(&((sshc_resource_manager_t *)rm)->lock);
}

/* --- scoped mode ------------------------------------------------------- */

ttak_abstract_mem_t *sshc_rm_scope_alloc(sshc_resource_manager_t *rm,
                                         size_t byte_size, const char *tag)
{
    if (rm == nullptr || byte_size == 0U) {
        return nullptr;
    }

    ttak_abstract_mem_t *handle = ttak_abstract_alloc(byte_size);
    if (handle == nullptr) {
        humanized_log_error("rm", "ttak_abstract_alloc failed", ENOMEM);
        return nullptr;
    }

    rm_slot_t *slot = (rm_slot_t *)calloc(1U, sizeof(*slot));
    if (slot == nullptr) {
        ttak_abstract_free(handle);
        humanized_log_error("rm", "slot alloc failed", ENOMEM);
        return nullptr;
    }

    slot->mode = RM_SLOT_MODE_SCOPED;
    slot->handle = handle;
    slot->byte_size = byte_size;
    slot->refcount = 1U;
    slot->tag = rm_strdup_safe(tag);

    pthread_mutex_lock(&rm->lock);
    slot->next = rm->head;
    rm->head = slot;
    rm->live_handles += 1U;
    rm->total_bytes += byte_size;
    pthread_mutex_unlock(&rm->lock);

    return handle;
}

int sshc_rm_scope_resize(sshc_resource_manager_t *rm,
                         ttak_abstract_mem_t *handle, size_t new_size)
{
    if (rm == nullptr || handle == nullptr) {
        return -1;
    }

    pthread_mutex_lock(&rm->lock);
    rm_slot_t *slot = rm_find_by_handle(rm, handle);
    if (slot == nullptr) {
        pthread_mutex_unlock(&rm->lock);
        humanized_log_error("rm", "resize unknown handle", EINVAL);
        return -1;
    }
    pthread_mutex_unlock(&rm->lock);

    if (ttak_abstract_resize(handle, new_size) != 0) {
        humanized_log_error("rm", "ttak_abstract_resize failed", ENOMEM);
        return -1;
    }

    pthread_mutex_lock(&rm->lock);
    if (rm->total_bytes >= slot->byte_size) {
        rm->total_bytes -= slot->byte_size;
    } else {
        rm->total_bytes = 0U;
    }
    slot->byte_size = new_size;
    rm->total_bytes += new_size;
    pthread_mutex_unlock(&rm->lock);

    return 0;
}

void sshc_rm_scope_free(sshc_resource_manager_t *rm,
                        ttak_abstract_mem_t *handle)
{
    if (rm == nullptr || handle == nullptr) {
        return;
    }

    pthread_mutex_lock(&rm->lock);
    rm_slot_t *slot = rm_find_by_handle(rm, handle);
    if (slot == nullptr) {
        pthread_mutex_unlock(&rm->lock);
        humanized_log_error("rm", "scope_free unknown handle", EINVAL);
        return;
    }
    if (slot->mode != RM_SLOT_MODE_SCOPED) {
        pthread_mutex_unlock(&rm->lock);
        humanized_log_error("rm", "scope_free on keyed handle", EINVAL);
        return;
    }
    sshc_rm_destructor_fn destructor = slot->destructor;
    void *user_data = slot->user_data;
    rm_unlink_and_free_slot(rm, slot);
    pthread_mutex_unlock(&rm->lock);

    if (destructor != nullptr) {
        destructor(handle, user_data);
    }
    ttak_abstract_free(handle);
}

/* --- keyed / refcount mode -------------------------------------------- */

ttak_abstract_mem_t *sshc_rm_acquire(sshc_resource_manager_t *rm,
                                     const char *key, size_t byte_size,
                                     sshc_rm_factory_fn factory,
                                     sshc_rm_destructor_fn destructor,
                                     void *user_data)
{
    if (rm == nullptr || key == nullptr || key[0] == '\0' || byte_size == 0U) {
        return nullptr;
    }

    pthread_mutex_lock(&rm->lock);
    rm_slot_t *existing = rm_find_by_key(rm, key);
    if (existing != nullptr) {
        existing->refcount += 1U;
        ttak_abstract_mem_t *h = existing->handle;
        pthread_mutex_unlock(&rm->lock);
        return h;
    }
    pthread_mutex_unlock(&rm->lock);

    /* First acquire — alloc + factory outside the lock so the factory
     * can take its own time / locks without serializing the registry. */
    ttak_abstract_mem_t *handle = ttak_abstract_alloc(byte_size);
    if (handle == nullptr) {
        humanized_log_error("rm", "ttak_abstract_alloc failed", ENOMEM);
        return nullptr;
    }
    if (factory != nullptr && factory(handle, user_data) != 0) {
        ttak_abstract_free(handle);
        return nullptr;
    }

    rm_slot_t *slot = (rm_slot_t *)calloc(1U, sizeof(*slot));
    if (slot == nullptr) {
        if (destructor != nullptr) {
            destructor(handle, user_data);
        }
        ttak_abstract_free(handle);
        humanized_log_error("rm", "slot alloc failed", ENOMEM);
        return nullptr;
    }
    slot->mode = RM_SLOT_MODE_KEYED;
    slot->handle = handle;
    slot->byte_size = byte_size;
    slot->refcount = 1U;
    slot->key = rm_strdup_safe(key);
    slot->destructor = destructor;
    slot->user_data = user_data;

    pthread_mutex_lock(&rm->lock);
    /* A racing thread may have created the same key while we were
     * doing the factory work.  If so, drop ours and reuse theirs. */
    rm_slot_t *racer = rm_find_by_key(rm, key);
    if (racer != nullptr) {
        racer->refcount += 1U;
        ttak_abstract_mem_t *h = racer->handle;
        pthread_mutex_unlock(&rm->lock);

        if (destructor != nullptr) {
            destructor(handle, user_data);
        }
        ttak_abstract_free(handle);
        free(slot->key);
        free(slot);
        return h;
    }
    slot->next = rm->head;
    rm->head = slot;
    rm->live_handles += 1U;
    rm->total_bytes += byte_size;
    pthread_mutex_unlock(&rm->lock);

    return handle;
}

void sshc_rm_release(sshc_resource_manager_t *rm,
                     ttak_abstract_mem_t *handle)
{
    if (rm == nullptr || handle == nullptr) {
        return;
    }

    pthread_mutex_lock(&rm->lock);
    rm_slot_t *slot = rm_find_by_handle(rm, handle);
    if (slot == nullptr) {
        pthread_mutex_unlock(&rm->lock);
        humanized_log_error("rm", "release unknown handle", EINVAL);
        return;
    }
    if (slot->mode != RM_SLOT_MODE_KEYED) {
        pthread_mutex_unlock(&rm->lock);
        humanized_log_error("rm", "release on scoped handle", EINVAL);
        return;
    }
    if (slot->refcount > 1U) {
        slot->refcount -= 1U;
        pthread_mutex_unlock(&rm->lock);
        return;
    }

    /* refcount about to hit 0 — extract before unlock so destructor
     * runs without the lock held. */
    sshc_rm_destructor_fn destructor = slot->destructor;
    void *user_data = slot->user_data;
    rm_unlink_and_free_slot(rm, slot);
    pthread_mutex_unlock(&rm->lock);

    if (destructor != nullptr) {
        destructor(handle, user_data);
    }
    ttak_abstract_free(handle);
}

/* --- typed POD vector helper ------------------------------------------ */

static size_t rm_next_capacity(size_t current, size_t needed)
{
    size_t cap = current == 0U ? 4U : current;
    while (cap < needed) {
        size_t doubled = cap * 2U;
        if (doubled <= cap) {
            return needed;
        }
        cap = doubled;
    }
    return cap;
}

int sshc_rm_vector_init(sshc_rm_vector_t *vec,
                        sshc_resource_manager_t *rm,
                        size_t element_size, size_t initial_capacity,
                        const char *tag)
{
    if (vec == nullptr || rm == nullptr || element_size == 0U) {
        return -1;
    }
    memset(vec, 0, sizeof(*vec));
    vec->rm = rm;
    vec->element_size = element_size;
    if (initial_capacity == 0U) {
        return 0;
    }
    size_t bytes = element_size * initial_capacity;
    if (bytes / element_size != initial_capacity) {
        return -1;
    }
    vec->backing = sshc_rm_scope_alloc(rm, bytes, tag);
    if (vec->backing == nullptr) {
        return -1;
    }
    vec->capacity = initial_capacity;
    return 0;
}

void sshc_rm_vector_destroy(sshc_rm_vector_t *vec)
{
    if (vec == nullptr) {
        return;
    }
    if (vec->backing != nullptr && vec->rm != nullptr) {
        sshc_rm_scope_free(vec->rm, vec->backing);
    }
    memset(vec, 0, sizeof(*vec));
}

int sshc_rm_vector_reserve(sshc_rm_vector_t *vec, size_t needed_capacity)
{
    if (vec == nullptr || vec->rm == nullptr || vec->element_size == 0U) {
        return -1;
    }
    if (needed_capacity <= vec->capacity) {
        return 0;
    }
    size_t new_capacity = rm_next_capacity(vec->capacity, needed_capacity);
    size_t new_bytes = vec->element_size * new_capacity;
    if (new_bytes / vec->element_size != new_capacity) {
        return -1;
    }

    if (vec->backing == nullptr) {
        vec->backing = sshc_rm_scope_alloc(vec->rm, new_bytes, "rm_vector");
        if (vec->backing == nullptr) {
            return -1;
        }
    } else if (sshc_rm_scope_resize(vec->rm, vec->backing, new_bytes) != 0) {
        return -1;
    }
    vec->capacity = new_capacity;
    return 0;
}

int sshc_rm_vector_resize(sshc_rm_vector_t *vec, size_t new_count)
{
    if (vec == nullptr) {
        return -1;
    }
    if (new_count > vec->capacity) {
        if (sshc_rm_vector_reserve(vec, new_count) != 0) {
            return -1;
        }
    }
    vec->count = new_count;
    return 0;
}

int sshc_rm_vector_append(sshc_rm_vector_t *vec, const void *element)
{
    if (vec == nullptr || element == nullptr) {
        return -1;
    }
    if (vec->count + 1U <= vec->count) {
        return -1; /* overflow */
    }
    if (sshc_rm_vector_reserve(vec, vec->count + 1U) != 0) {
        return -1;
    }
    int rc = ttak_abstract_write(vec->backing,
                                 vec->count * vec->element_size,
                                 element, vec->element_size);
    if (rc != 0) {
        return -1;
    }
    vec->count += 1U;
    return 0;
}

int sshc_rm_vector_get(const sshc_rm_vector_t *vec, size_t idx, void *out)
{
    if (vec == nullptr || out == nullptr || idx >= vec->count ||
        vec->backing == nullptr) {
        return -1;
    }
    return ttak_abstract_read(vec->backing, idx * vec->element_size, out,
                              vec->element_size) == 0
               ? 0
               : -1;
}

int sshc_rm_vector_set(sshc_rm_vector_t *vec, size_t idx, const void *element)
{
    if (vec == nullptr || element == nullptr || idx >= vec->count ||
        vec->backing == nullptr) {
        return -1;
    }
    return ttak_abstract_write(vec->backing, idx * vec->element_size,
                               element, vec->element_size) == 0
               ? 0
               : -1;
}

int sshc_rm_vector_map_range(sshc_rm_vector_t *vec, size_t start_idx,
                             size_t element_count,
                             ttak_abstract_access_t access,
                             ttak_abstract_map_t *out_view)
{
    if (vec == nullptr || out_view == nullptr || vec->backing == nullptr) {
        return -1;
    }
    if (start_idx + element_count < start_idx ||
        start_idx + element_count > vec->count) {
        return -1;
    }
    size_t bytes = element_count * vec->element_size;
    return ttak_abstract_map(vec->backing, start_idx * vec->element_size,
                             bytes, access, out_view);
}

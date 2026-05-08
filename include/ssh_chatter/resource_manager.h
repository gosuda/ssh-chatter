#ifndef SSH_CHATTER_RESOURCE_MANAGER_H
#define SSH_CHATTER_RESOURCE_MANAGER_H

/**
 * @file resource_manager.h
 * @brief On-demand allocate / immediate free registry for SSH-Chatter.
 *
 * Backs every block via libttak's pointer-stable abstract allocator
 * (see lib/libttak/include/ttak/mem/abstract.h). All large dynamic
 * blocks should flow through this layer so that:
 *
 *   - Memory is allocated only when a feature is in use.
 *   - Memory is freed immediately when the last user releases it.
 *   - The OS-overridden virtual contiguous backing of ttak is reused
 *     instead of plain malloc/realloc/free.
 *
 * Two modes are exposed:
 *
 *   1. SCOPED   — single owner, no key.  Acquire returns a handle,
 *                  release frees it.  Used for per-launch / per-request
 *                  buffers (DOOR PTY scratch, /bbs read body load).
 *
 *   2. KEYED    — refcounted shared resource.  acquire(key, factory, size)
 *                  alloc+factory on first acquire, refcount++ on subsequent.
 *                  release() decrements; the destructor + free run when
 *                  refcount reaches zero.  Used for shared caches that
 *                  multiple sessions may pin at the same time.
 *
 * A typed vector helper (sshc_rm_vector_t) wraps the abstract handle for
 * grow-on-demand POD vectors and bounds the map/unmap dance to a single
 * helper per element-access pattern.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ttak/mem/abstract.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef nullptr
#define nullptr (void *)(NULL)
#endif

typedef struct sshc_resource_manager sshc_resource_manager_t;

/**
 * @brief Factory invoked the first time a keyed resource is acquired.
 *
 * Receives the freshly allocated abstract handle.  Use ttak_abstract_map
 * /unmap or ttak_abstract_write to populate the backing.
 *
 * @return 0 on success, non-zero to abort the acquire (the manager will
 *         free the handle and return NULL to the caller).
 */
typedef int (*sshc_rm_factory_fn)(ttak_abstract_mem_t *handle, void *user_data);

/**
 * @brief Optional destructor invoked just before the abstract handle is
 *        freed (refcount reached zero, or scope exit).  May be NULL.
 */
typedef void (*sshc_rm_destructor_fn)(ttak_abstract_mem_t *handle, void *user_data);

/* --- Lifecycle ---------------------------------------------------------- */

sshc_resource_manager_t *sshc_resource_manager_create(const char *label);
void sshc_resource_manager_destroy(sshc_resource_manager_t *rm);

/**
 * @brief Snapshot of currently-tracked usage; safe to call from any thread.
 *
 * @param[out] out_live_handles   Number of handles currently outstanding.
 * @param[out] out_total_bytes    Sum of logical sizes of those handles.
 */
void sshc_resource_manager_stats(const sshc_resource_manager_t *rm,
                                 size_t *out_live_handles,
                                 size_t *out_total_bytes);

/* --- Scoped mode -------------------------------------------------------- */

/**
 * @brief Allocate a tracked abstract handle of @p byte_size bytes.
 *
 * The returned handle is registered with the manager so it can be audited
 * (and forcibly freed at shutdown if the caller leaks it).  The caller
 * still owns the lifetime and must call sshc_rm_scope_free when done.
 *
 * @return Handle on success, NULL on allocation failure.
 */
ttak_abstract_mem_t *sshc_rm_scope_alloc(sshc_resource_manager_t *rm,
                                         size_t byte_size,
                                         const char *tag);

/**
 * @brief Resize a scoped handle in place.
 *
 * Wraps ttak_abstract_resize and updates the manager's accounting.
 * @return 0 on success, -1 on failure (existing handle is unchanged).
 */
int sshc_rm_scope_resize(sshc_resource_manager_t *rm,
                         ttak_abstract_mem_t *handle, size_t new_size);

/**
 * @brief Release a scoped handle.  Frees backing immediately.
 *
 * Safe to call with NULL.  If @p handle is unknown to the manager the
 * call is a no-op (with a humanized error) so double-frees fail loudly
 * but do not crash.
 */
void sshc_rm_scope_free(sshc_resource_manager_t *rm,
                        ttak_abstract_mem_t *handle);

/* --- Keyed / refcount mode --------------------------------------------- */

/**
 * @brief Acquire a shared resource keyed by @p key.
 *
 * On first acquire allocates a fresh @p byte_size handle, runs @p factory
 * to populate it, and returns it with refcount = 1.  Subsequent acquires
 * with the same key bump the refcount and return the existing handle.
 *
 * @return Handle on success, NULL on allocation / factory failure.
 */
ttak_abstract_mem_t *sshc_rm_acquire(sshc_resource_manager_t *rm,
                                     const char *key, size_t byte_size,
                                     sshc_rm_factory_fn factory,
                                     sshc_rm_destructor_fn destructor,
                                     void *user_data);

/**
 * @brief Release a keyed acquire.  When refcount hits zero, the optional
 *        destructor runs and the abstract handle is freed.
 */
void sshc_rm_release(sshc_resource_manager_t *rm,
                     ttak_abstract_mem_t *handle);

/* --- Typed POD vector helper ------------------------------------------- */

/**
 * @brief Grow-on-demand vector backed by an abstract handle.
 *
 * Element access goes through the helpers below (which copy in/out) or
 * sshc_rm_vector_map_range (for direct scoped windows).  The backing
 * handle may relocate during reserve/append, so callers must NOT cache
 * data pointers across vector mutations.
 */
typedef struct sshc_rm_vector {
    sshc_resource_manager_t *rm;
    ttak_abstract_mem_t *backing; /* owned */
    size_t element_size;
    size_t count;
    size_t capacity;
} sshc_rm_vector_t;

int sshc_rm_vector_init(sshc_rm_vector_t *vec,
                        sshc_resource_manager_t *rm,
                        size_t element_size, size_t initial_capacity,
                        const char *tag);

void sshc_rm_vector_destroy(sshc_rm_vector_t *vec);

int sshc_rm_vector_reserve(sshc_rm_vector_t *vec, size_t needed_capacity);

int sshc_rm_vector_resize(sshc_rm_vector_t *vec, size_t new_count);

int sshc_rm_vector_append(sshc_rm_vector_t *vec, const void *element);

int sshc_rm_vector_get(const sshc_rm_vector_t *vec, size_t idx, void *out);

int sshc_rm_vector_set(sshc_rm_vector_t *vec, size_t idx, const void *element);

/**
 * @brief Map a contiguous range of @p element_count starting at @p start_idx.
 *
 * Caller must call ttak_abstract_unmap on @p out_view when done.
 */
int sshc_rm_vector_map_range(sshc_rm_vector_t *vec, size_t start_idx,
                             size_t element_count,
                             ttak_abstract_access_t access,
                             ttak_abstract_map_t *out_view);

#ifdef __cplusplus
}
#endif

#endif /* SSH_CHATTER_RESOURCE_MANAGER_H */

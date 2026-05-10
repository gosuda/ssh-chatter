#ifndef SSH_CHATTER_ABSTRACT_BYTE_BUFFER_H
#define SSH_CHATTER_ABSTRACT_BYTE_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <ttak/mem/abstract.h>

typedef struct sshc_abstract_byte_buffer {
    ttak_abstract_mem_t *storage;
    size_t length;
    size_t capacity;
} sshc_abstract_byte_buffer_t;

typedef struct sshc_abstract_byte_buffer_view {
    ttak_abstract_map_t map;
    char *data;
} sshc_abstract_byte_buffer_view_t;

static inline void
sshc_abstract_byte_buffer_init(sshc_abstract_byte_buffer_t *buffer)
{
    if (buffer == NULL) {
        return;
    }
    buffer->storage = NULL;
    buffer->length = 0U;
    buffer->capacity = 0U;
}

static inline void
sshc_abstract_byte_buffer_free(sshc_abstract_byte_buffer_t *buffer)
{
    if (buffer == NULL) {
        return;
    }
    ttak_abstract_free(buffer->storage);
    buffer->storage = NULL;
    buffer->length = 0U;
    buffer->capacity = 0U;
}

static inline bool
sshc_abstract_byte_buffer_reserve_total(sshc_abstract_byte_buffer_t *buffer,
                                        size_t total_bytes)
{
    if (buffer == NULL) {
        return false;
    }
    if (total_bytes <= buffer->capacity) {
        return true;
    }

    size_t new_cap = buffer->capacity > 0U ? buffer->capacity : 256U;
    while (new_cap < total_bytes) {
        if (new_cap > SIZE_MAX / 2U) {
            return false;
        }
        new_cap *= 2U;
    }

    if (buffer->storage == NULL) {
        buffer->storage = ttak_abstract_alloc(new_cap);
        if (buffer->storage == NULL) {
            return false;
        }
    } else if (ttak_abstract_resize(buffer->storage, new_cap) != 0) {
        return false;
    }

    buffer->capacity = new_cap;
    return true;
}

static inline bool
sshc_abstract_byte_buffer_append(sshc_abstract_byte_buffer_t *buffer,
                                 const void *data, size_t length)
{
    if (buffer == NULL || (data == NULL && length > 0U)) {
        return false;
    }

    size_t required = buffer->length + length + 1U;
    if (!sshc_abstract_byte_buffer_reserve_total(buffer, required)) {
        return false;
    }

    ttak_abstract_map_t map;
    if (ttak_abstract_map(buffer->storage, buffer->length, length + 1U,
                          TTAK_ABSTRACT_ACCESS_WRITE, &map) != 0) {
        return false;
    }

    if (length > 0U) {
        memcpy(map.data, data, length);
    }
    ((char *)map.data)[length] = '\0';
    ttak_abstract_unmap(&map);
    buffer->length += length;
    return true;
}

static inline bool sshc_abstract_byte_buffer_map_cstr(
    sshc_abstract_byte_buffer_t *buffer, sshc_abstract_byte_buffer_view_t *view)
{
    if (view == NULL) {
        return false;
    }
    memset(view, 0, sizeof(*view));

    if (buffer == NULL || buffer->storage == NULL) {
        return false;
    }

    size_t length = buffer->length + 1U;
    if (ttak_abstract_map(buffer->storage, 0U, length, TTAK_ABSTRACT_ACCESS_READ,
                          &view->map) != 0) {
        return false;
    }
    view->data = (char *)view->map.data;
    return true;
}

static inline void sshc_abstract_byte_buffer_unmap(
    sshc_abstract_byte_buffer_view_t *view)
{
    if (view == NULL) {
        return;
    }
    ttak_abstract_unmap(&view->map);
    view->data = NULL;
}

static inline bool sshc_abstract_byte_buffer_copy_out(
    const sshc_abstract_byte_buffer_t *buffer, void *out, size_t length)
{
    if (buffer == NULL || out == NULL) {
        return false;
    }
    if (length > buffer->length + 1U) {
        return false;
    }
    return ttak_abstract_read(buffer->storage, 0U, out, length) == 0;
}

#endif

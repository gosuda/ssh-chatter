# Allocator–OS Memory Residency Control Plan

This document defines a low-level memory residency strategy for `ssh-chatter` without changing object layouts, message/feed limits, or allocator determinism.

## 1) Span Layer at Allocator–OS Boundary

Add a **span manager** below existing slab/arena logic.

- **span**: page-aligned contiguous memory chunk (multiple pages, fixed span class sizes).
- **slabs** are carved from spans for fixed-size allocations.
- allocator metadata tracks span state, not object layout.

### Span metadata (out-of-band)

```c
typedef enum {
    SPAN_HOT = 0,
    SPAN_COLD = 1,
    SPAN_FREE = 2,
    SPAN_MAPPED_FILE = 3
} span_state_t;

typedef struct span {
    void   *base;             // page-aligned start
    size_t  length;           // bytes, multiple of page size
    uint32_t page_count;
    uint32_t inuse_objects;   // live allocs in span
    uint32_t free_objects;
    uint64_t last_touch_ns;   // monotonic timestamp
    uint32_t reclaim_epoch;
    bool     no_hugepage_set;
    bool     file_backed;
    int      backing_fd;      // -1 for anonymous
    off_t    backing_off;
    span_state_t state;
    struct span *next;
} span_t;
```

Rules:

1. `base` must be aligned to system page size.
2. `length` must be a multiple of page size.
3. metadata is allocated separately so existing app structs remain unchanged.

## 2) Page-Span Tracking + Reclamation

Track per-span occupancy and temperature.

- On each alloc/free from slab: update `inuse_objects`, `free_objects`, `last_touch_ns`.
- Reclamation only happens at span granularity (never partial object compaction).

### Reclamation primitives

- **Anonymous cold/free spans:** `madvise(base, length, MADV_DONTNEED)`.
- **Large dedicated allocations:** `munmap(base, length)` when fully free and not pooled.
- **Optional lazy reclaim mode:** `MADV_FREE` for non-critical latency profiles.

### Core reclaim logic (pseudocode)

```c
void span_on_object_free(span_t *s) {
    s->inuse_objects--;
    s->free_objects++;
    s->last_touch_ns = now_monotonic_ns();

    if (s->inuse_objects == 0) {
        s->state = SPAN_FREE;
        maybe_release_span(s, REASON_FULLY_FREE);
    }
}

void maybe_release_span(span_t *s, int reason) {
    if (!is_page_aligned(s->base) || !is_page_multiple(s->length)) return;

    if (s->file_backed) {
        // persistent content: keep mapping, let page cache reclaim naturally
        if (reason == REASON_COLD || reason == REASON_FULLY_FREE) {
            madvise(s->base, s->length, MADV_DONTNEED);  // drop private clean pages
        }
        return;
    }

    if (is_large_dedicated_span(s) && s->inuse_objects == 0) {
        munmap(s->base, s->length);
        s->state = SPAN_FREE;
        remove_from_active_span_sets(s);
        enqueue_virtual_hole_for_remap(s->length);
        return;
    }

    if (s->inuse_objects == 0 || is_cold_enough(s)) {
        madvise(s->base, s->length, MADV_DONTNEED);
        s->state = (s->inuse_objects == 0) ? SPAN_FREE : SPAN_COLD;
    }
}
```

## 3) Hot/Cold Arena Segmentation

For large text/message arenas, split into fixed segments (each segment == one or more spans).

- **hot segment set**: currently active window (recent chat, active sessions).
- **cold segment set**: old history / infrequently read blocks.

Segment policy:

1. New writes go to current hot segment.
2. Segment becomes cold after idle timeout and write cursor rotation.
3. Cold segments are reclaimed with `MADV_DONTNEED` (anonymous) or file-backed mapping (preferred for persistence).
4. Accessing cold segment is legal; page faults repopulate from zero-fill (anon) or file cache (mapped).

### Temperature scan loop (pseudocode)

```c
void residency_maintenance_tick(void) {
    uint64_t now = now_monotonic_ns();

    for_each_span(s) {
        uint64_t idle_ns = now - s->last_touch_ns;

        if (s->state == SPAN_HOT && idle_ns > COLD_IDLE_NS) {
            s->state = SPAN_COLD;
            maybe_release_span(s, REASON_COLD);
        }

        if (s->state == SPAN_COLD && idle_ns > FREE_IDLE_NS && s->inuse_objects == 0) {
            maybe_release_span(s, REASON_FULLY_FREE);
        }
    }
}
```

Determinism is preserved because state transitions are threshold-based and run from a fixed periodic maintenance tick.

## 4) File-Backed Mapping for Persistent Cold Data

Move persistent, rarely written bulk storage (BBS posts, archived messages, feed history) to mmap-backed segment files.

- `fd = open(..., O_RDWR|O_CREAT)`
- `ftruncate(fd, segment_size)`
- `ptr = mmap(NULL, segment_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, off)`

Keep RAM-resident:

- indexes
- small recent-write window
- segment directory metadata

When cold:

- `madvise(ptr, segment_size, MADV_DONTNEED)` to drop clean/unused resident pages.
- data remains durable in file, and reload is page-cache driven.

## 5) Prevent False Retention

To avoid spans staying resident due to allocator internals:

1. **Bound per-size-class free lists** by span count, not object count.
2. Prefer freeing objects back into the same span until it becomes fully free.
3. Disable cross-span stealing when a span is near-empty (to create reclaimable empty spans).
4. Add optional **arena rotation**: new allocations avoid nearly-empty old spans once a fresh hot span exists.

### Free-list trimming rule

```c
if (size_class_cache.spans_idle > IDLE_SPAN_LIMIT) {
    span_t *victim = pick_oldest_idle_span(size_class);
    if (victim->inuse_objects == 0) {
        maybe_release_span(victim, REASON_FULLY_FREE);
        unlink_from_size_class_cache(victim);
    }
}
```

## 6) Kernel Interaction Settings

Apply mapping hints immediately after span creation:

```c
void span_post_map_init(span_t *s) {
    madvise(s->base, s->length, MADV_NOHUGEPAGE); // avoid THP 2MB pinning
}
```

Reclaim mode selection:

- Default: `MADV_DONTNEED` for predictable immediate RSS drop.
- Optional mode (`CHATTER_RESIDENCY_LAZY=1`): `MADV_FREE` for lower immediate CPU cost.

Large-span unmap threshold:

- if span is dedicated and `length >= 1 MiB` and fully free → `munmap`.

## 7) Trigger Matrix (Exact Rules)

1. **On free path**
   - If `inuse_objects == 0`:
     - dedicated large span: `munmap`
     - pooled span: `madvise(DONTNEED)` + keep metadata for deterministic remap
2. **On maintenance tick (e.g., every 1s)**
   - if `state == HOT` and idle > `COLD_IDLE_NS` → mark COLD + `madvise(DONTNEED)`
   - if `state == COLD`, idle > `FREE_IDLE_NS`, and empty → release/unmap
3. **On memory pressure signal (optional self-trigger)**
   - immediate pass over oldest cold spans until RSS budget reached.
4. **On access fault/hit to cold span**
   - mark HOT, update `last_touch_ns`; no layout changes.

Suggested initial thresholds:

- `COLD_IDLE_NS = 30s`
- `FREE_IDLE_NS = 300s`
- maintenance interval = `1000ms`
- dedicated unmap threshold = `1 MiB`

## 8) Observability / Verification

Use `/proc/<pid>/smaps_rollup` and allocator counters.

Track:

- `Rss`
- `Anonymous`
- `File`
- `Private_Dirty`

Expected behavior after rollout:

- virtual size near current baseline
- RSS converges toward active set (~120–220MB under moderate load)
- flatter `Private_Dirty` growth due to aggressive cold-span eviction

## 9) Integration Notes for Existing `ssh-chatter` Memory Stack

Existing context/epoch ownership can remain intact. This design only adds a lower span residency layer:

- `ttak_fastalloc`/existing slab path unchanged at API level.
- New hooks on alloc/free update span counters.
- Maintenance thread/tick (already present in runtime loops) invokes `residency_maintenance_tick()`.
- No struct ABI changes for application-visible objects.

This keeps deterministic allocation semantics while enabling active RSS control against long-lived idle memory.

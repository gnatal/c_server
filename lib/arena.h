#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>
#include "vendor/yyjson/yyjson.h"

/*
 * An Arena Allocator (Bump Allocator) for per-connection request data.
 * Drastically reduces malloc/free overhead and heap fragmentation.
 */

typedef struct ArenaNode {
    struct ArenaNode *next;
    char data[];
} ArenaNode;

typedef struct {
    char *buf;          /* Pre-allocated fixed buffer (e.g., 64KB) */
    size_t cap;         /* Capacity of the fixed buffer */
    size_t offset;      /* Current allocation offset within buf */
    ArenaNode *large;   /* Linked list of allocations exceeding capacity */
} Arena;

/* Initializes an arena with a pre-allocated static/heap buffer. */
void arena_init(Arena *a, char *buf, size_t cap);

/* Allocates `size` bytes from the arena. Returns NULL on hard OOM or when `size` is so large
 * (near SIZE_MAX) that aligning it or adding the fallback node header would overflow. */
void *arena_alloc(Arena *a, size_t size);

/*
 * Resizes a block this arena returned, keeping its first min(old_size, new_size) bytes; `old_size` is the
 * size it was allocated (or last grown) with. Never leaves a dead copy behind when it can avoid it:
 *   - the last block in `buf` is extended (or shrunk) in place when the new size fits the remaining capacity;
 *   - the newest fallback block (head of `large`) is realloc'd, so large growth costs 1x, not a copy per step;
 *   - anything else is allocated anew and copied, the old block stays dead until arena_reset.
 * `ptr` NULL behaves as arena_alloc(a, new_size). Returns NULL on the same failures as arena_alloc; `ptr` is
 * then still valid and unchanged.
 */
void *arena_grow(Arena *a, void *ptr, size_t old_size, size_t new_size);

/* Resets the arena offset to 0 and frees any fallback 'large' allocations.
 * Call this at the end of every request. */
void arena_reset(Arena *a);

/* Destroys the arena (frees fallback allocations). Does not free `buf`. */
void arena_destroy(Arena *a);

/* Provides a yyjson_alc backed by the given Arena. */
yyjson_alc arena_yyjson_alc(Arena *a);

#endif /* ARENA_H */

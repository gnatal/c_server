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

/* Resets the arena offset to 0 and frees any fallback 'large' allocations.
 * Call this at the end of every request. */
void arena_reset(Arena *a);

/* Destroys the arena (frees fallback allocations). Does not free `buf`. */
void arena_destroy(Arena *a);

/* Provides a yyjson_alc backed by the given Arena. */
yyjson_alc arena_yyjson_alc(Arena *a);

#endif /* ARENA_H */

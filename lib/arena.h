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
    size_t large_bytes; /* bytes requested by the blocks in `large` (their live sizes); 0 after reset */
} Arena;

/*
 * A free list of equally sized arena buffers (block_size >= sizeof(char *)), linked through each free
 * block's first bytes. arena_pool_take reuses a spare block or mallocs one; arena_pool_give keeps at
 * most max_spare blocks and frees the rest. Blocks handed out belong to the taker until given back.
 */
typedef struct {
    char *free_head;
    size_t block_size;
    size_t spare;       /* blocks on the free list */
    size_t max_spare;
} ArenaPool;

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

/*
 * Moves everything allocated in `from` (its buffer, offset and fallback list) into `to`, then starts
 * `from` over on fresh_buf/cap. Nothing is copied: every pointer into the old allocations stays valid,
 * now owned by `to`. Pointers to the `from` struct itself (an Arena * or a yyjson_alc made from it) keep
 * allocating from `from`, i.e. from fresh_buf.
 */
void arena_hand_over(Arena *from, Arena *to, char *fresh_buf, size_t cap);

void arena_pool_init(ArenaPool *pool, size_t block_size, size_t max_spare);
/* A block of pool->block_size bytes, or NULL on OOM. */
char *arena_pool_take(ArenaPool *pool);
/* Returns a block from arena_pool_take. NULL is a no-op. */
void arena_pool_give(ArenaPool *pool, char *block);
/* Frees every spare block. Blocks still handed out are not tracked: give them back first. */
void arena_pool_destroy(ArenaPool *pool);
/* Frees a's fallback blocks and gives its buffer (taken from pool) back to the pool. `a` is then empty. */
void arena_release_to_pool(Arena *a, ArenaPool *pool);

/* Provides a yyjson_alc backed by the given Arena. */
yyjson_alc arena_yyjson_alc(Arena *a);

#endif /* ARENA_H */

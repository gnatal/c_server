#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "arena.h"

#define ALIGNMENT 8

static inline size_t align_up(size_t n) {
    return (n + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}

void arena_init(Arena *a, char *buf, size_t cap) {
    a->buf = buf;
    a->cap = cap;
    a->offset = 0;
    a->large = NULL;
    a->large_bytes = 0;
}

void *arena_alloc(Arena *a, size_t size) {
    /* Reject sizes where align_up or sizeof(ArenaNode) + size would wrap around */
    if (size > SIZE_MAX - ALIGNMENT - sizeof(ArenaNode)) {
        return NULL;
    }
    const size_t aligned_size = align_up(size);

    /* Try to allocate from the pre-allocated fixed buffer (offset <= cap always holds, so no overflow) */
    if (aligned_size <= a->cap - a->offset) {
        void *ptr = a->buf + a->offset;
        a->offset += aligned_size;
        return ptr;
    }

    /* Fallback to standard malloc for allocations that exceed remaining capacity */
    ArenaNode *node = malloc(sizeof(ArenaNode) + size);
    if (node == NULL) {
        return NULL;
    }
    
    node->next = a->large;
    a->large = node;
    a->large_bytes += size;

    return node->data;
}

void *arena_grow(Arena *a, void *ptr, size_t old_size, size_t new_size) {
    if (ptr == NULL) {
        return arena_alloc(a, new_size);
    }
    if (new_size > SIZE_MAX - ALIGNMENT - sizeof(ArenaNode) || old_size > SIZE_MAX - ALIGNMENT) {
        return NULL;
    }
    char *const p = ptr;

    /* Last block in the fixed buffer: p lies inside [buf, buf + offset) and ends exactly at offset. The
     * range check comes first so a pointer from elsewhere (a fallback node, a connection-owned malloc) is
     * never compared as if it were inside buf. */
    if (a->buf != NULL && p >= a->buf && p < a->buf + a->offset) {
        const size_t start = (size_t)(p - a->buf);
        if (start + align_up(old_size) == a->offset && align_up(new_size) <= a->cap - start) {
            a->offset = start + align_up(new_size);
            return ptr;
        }
    }

    /* Newest fallback block: realloc keeps the list intact (the node's `next` moves with it). */
    if (a->large != NULL && p == a->large->data) {
        ArenaNode *node = realloc(a->large, sizeof(ArenaNode) + new_size);
        if (node == NULL) {
            return NULL;
        }
        a->large = node;
        a->large_bytes = a->large_bytes - old_size + new_size;
        return node->data;
    }

    void *moved = arena_alloc(a, new_size);
    if (moved != NULL) {
        memcpy(moved, ptr, old_size < new_size ? old_size : new_size);
    }
    return moved;
}

void arena_reset(Arena *a) {
    a->offset = 0;
    
    /* Free all fallback allocations */
    ArenaNode *curr = a->large;
    while (curr != NULL) {
        ArenaNode *next = curr->next;
        free(curr);
        curr = next;
    }
    a->large = NULL;
    a->large_bytes = 0;
}

void arena_destroy(Arena *a) {
    arena_reset(a);
}

void arena_hand_over(Arena *from, Arena *to, char *fresh_buf, size_t cap) {
    *to = *from;
    arena_init(from, fresh_buf, cap);
}

void arena_pool_init(ArenaPool *pool, size_t block_size, size_t max_spare) {
    pool->free_head = NULL;
    pool->block_size = block_size;
    pool->spare = 0;
    pool->max_spare = max_spare;
}

char *arena_pool_take(ArenaPool *pool) {
    char *const block = pool->free_head;
    if (block == NULL) {
        return malloc(pool->block_size); /* given back by arena_pool_give, freed there or by arena_pool_destroy */
    }
    memcpy(&pool->free_head, block, sizeof(char *));
    pool->spare--;
    return block;
}

void arena_pool_give(ArenaPool *pool, char *block) {
    if (block == NULL) {
        return;
    }
    if (pool->spare >= pool->max_spare) {
        free(block);
        return;
    }
    memcpy(block, &pool->free_head, sizeof(char *));
    pool->free_head = block;
    pool->spare++;
}

void arena_pool_destroy(ArenaPool *pool) {
    while (pool->free_head != NULL) {
        char *const block = pool->free_head;
        memcpy(&pool->free_head, block, sizeof(char *));
        free(block);
    }
    pool->spare = 0;
}

void arena_release_to_pool(Arena *a, ArenaPool *pool) {
    arena_destroy(a);
    arena_pool_give(pool, a->buf);
    arena_init(a, NULL, 0);
}

/* yyjson integration */
static void *arena_yyjson_malloc(void *ctx, size_t size) {
    return arena_alloc((Arena *)ctx, size);
}

static void *arena_yyjson_realloc(void *ctx, void *ptr, size_t old_size, size_t size) {
    return arena_grow((Arena *)ctx, ptr, old_size, size);
}

static void arena_yyjson_free(void *ctx, void *ptr) {
    (void)ctx;
    (void)ptr;
    /* No-op: memory is freed in bulk via arena_reset */
}

yyjson_alc arena_yyjson_alc(Arena *a) {
    yyjson_alc alc;
    alc.malloc = arena_yyjson_malloc;
    alc.realloc = arena_yyjson_realloc;
    alc.free = arena_yyjson_free;
    alc.ctx = a;
    return alc;
}

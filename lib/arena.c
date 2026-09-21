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
}

void *arena_alloc(Arena *a, size_t size) {
    size_t aligned_size = align_up(size);

    /* Try to allocate from the pre-allocated fixed buffer */
    if (a->offset + aligned_size <= a->cap) {
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
    
    return node->data;
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
}

void arena_destroy(Arena *a) {
    arena_reset(a);
}

/* yyjson integration */
static void *arena_yyjson_malloc(void *ctx, size_t size) {
    return arena_alloc((Arena *)ctx, size);
}

static void *arena_yyjson_realloc(void *ctx, void *ptr, size_t old_size, size_t size) {
    void *new_ptr = arena_alloc((Arena *)ctx, size);
    if (new_ptr && ptr) {
        size_t copy_size = old_size < size ? old_size : size;
        memcpy(new_ptr, ptr, copy_size);
    }
    return new_ptr;
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

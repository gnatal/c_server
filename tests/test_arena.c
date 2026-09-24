#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arena.h"

/*
 * Arena allocator: bump allocation from the fixed buffer, malloc fallback past capacity, reset,
 * and rejection of sizes near SIZE_MAX that would overflow the alignment or fallback-node math.
 */

static void test_bump_allocations_are_aligned_and_in_buffer(void) {
    static char buf[256];
    Arena a;
    arena_init(&a, buf, sizeof(buf));

    char *const p1 = arena_alloc(&a, 3);
    char *const p2 = arena_alloc(&a, 1);
    assert(p1 == buf);
    assert(p2 == buf + 8);
    assert(a.offset == 16);
    assert(a.large == NULL);

    arena_destroy(&a);
}

static void test_exact_fit_stays_in_buffer(void) {
    static char buf[64];
    Arena a;
    arena_init(&a, buf, sizeof(buf));

    assert(arena_alloc(&a, 56) == buf);
    assert(arena_alloc(&a, 8) == buf + 56);
    assert(a.offset == a.cap);
    assert(a.large == NULL);

    arena_destroy(&a);
}

static void test_fallback_past_capacity_and_reset(void) {
    static char buf[64];
    Arena a;
    arena_init(&a, buf, sizeof(buf));

    assert(arena_alloc(&a, 60) == buf);
    char *const big = arena_alloc(&a, 1000);
    assert(big != NULL);
    assert(big < buf || big >= buf + sizeof(buf));
    assert(a.large != NULL);
    memset(big, 'x', 1000);

    arena_reset(&a);
    assert(a.offset == 0);
    assert(a.large == NULL);
    assert(arena_alloc(&a, 8) == buf);

    arena_destroy(&a);
}

static void test_huge_sizes_return_null(void) {
    static char buf[64];
    Arena a;
    arena_init(&a, buf, sizeof(buf));
    assert(arena_alloc(&a, 8) == buf);

    /* align_up would wrap to 0 and the old check would hand out the buffer */
    assert(arena_alloc(&a, SIZE_MAX) == NULL);
    assert(arena_alloc(&a, SIZE_MAX - 3) == NULL);
    /* sizeof(ArenaNode) + size would wrap in the fallback malloc */
    assert(arena_alloc(&a, SIZE_MAX - sizeof(ArenaNode) + 1) == NULL);
    assert(arena_alloc(&a, SIZE_MAX - 8 - sizeof(ArenaNode) + 1) == NULL);

    /* No state was touched by the rejected calls */
    assert(a.offset == 8);
    assert(a.large == NULL);
    assert(arena_alloc(&a, 8) == buf + 8);

    arena_destroy(&a);
}

static void test_yyjson_realloc_huge_returns_null(void) {
    static char buf[64];
    Arena a;
    arena_init(&a, buf, sizeof(buf));
    const yyjson_alc alc = arena_yyjson_alc(&a);

    char *const p = alc.malloc(alc.ctx, 8);
    assert(p != NULL);
    memcpy(p, "abcdefg", 8);
    assert(alc.realloc(alc.ctx, p, 8, SIZE_MAX) == NULL);

    char *const q = alc.realloc(alc.ctx, p, 8, 16);
    assert(q != NULL);
    assert(strcmp(q, "abcdefg") == 0);

    arena_destroy(&a);
}

/* The last block in buf grows and shrinks in place: same pointer, offset moves, nothing left behind. */
static void test_grow_last_block_in_place(void) {
    static char buf[128];
    Arena a;
    arena_init(&a, buf, sizeof(buf));
    assert(arena_alloc(&a, 8) == buf);
    char *const p = arena_alloc(&a, 10);
    memcpy(p, "0123456789", 10);

    assert(arena_grow(&a, p, 10, 40) == p);
    assert(a.offset == 8 + 40);
    assert(memcmp(p, "0123456789", 10) == 0);
    assert(arena_grow(&a, p, 40, 16) == p); /* shrink: offset gives the space back */
    assert(a.offset == 8 + 16);
    assert(arena_grow(&a, p, 16, 120) == p); /* exactly the remaining capacity */
    assert(a.offset == a.cap && a.large == NULL);

    arena_destroy(&a);
}

/* A block with another allocation after it is copied; the later block is untouched. */
static void test_grow_non_last_block_copies(void) {
    static char buf[128];
    Arena a;
    arena_init(&a, buf, sizeof(buf));
    char *const p = arena_alloc(&a, 8);
    char *const q = arena_alloc(&a, 8);
    memcpy(p, "pppppppp", 8);
    memcpy(q, "qqqqqqqq", 8);

    char *const g = arena_grow(&a, p, 8, 32);
    assert(g == buf + 16 && a.offset == 16 + 32);
    assert(memcmp(g, "pppppppp", 8) == 0);
    assert(memcmp(q, "qqqqqqqq", 8) == 0);

    arena_destroy(&a);
}

/* Growing past capacity moves the block to a fallback node once; later growth reallocs that node
 * instead of adding one node per step. */
static void test_grow_past_capacity_reuses_fallback_node(void) {
    static char buf[64];
    Arena a;
    arena_init(&a, buf, sizeof(buf));
    char *const p = arena_alloc(&a, 32);
    memset(p, 'a', 32);

    char *big = arena_grow(&a, p, 32, 200);
    assert(big != NULL && (big < buf || big >= buf + sizeof(buf)));
    assert(a.large != NULL && a.large->data == big && a.large->next == NULL);
    size_t size = 200;
    memset(big + 32, 'b', size - 32);
    while (size < ((size_t)1 << 20)) {
        big = arena_grow(&a, big, size, size * 2);
        assert(big != NULL && a.large->data == big && a.large->next == NULL); /* still one node */
        memset(big + size, 'b', size); /* fill the new half */
        size *= 2;
    }
    assert(big[0] == 'a' && big[31] == 'a' && big[32] == 'b' && big[size - 1] == 'b');

    arena_reset(&a); /* frees the grown node (ASan/leaks check) */
    assert(a.large == NULL);
    arena_destroy(&a);
}

/* A pointer the arena did not hand out is copied, never extended or realloc'd - even one that ends
 * exactly where buf starts while offset is 0 (it would pass a bare "ptr + old == buf + offset" test). */
static void test_grow_foreign_pointer_is_copied(void) {
    static char region[128];
    char *const buf = region + 64;
    Arena a;
    arena_init(&a, buf, 64);
    char *const foreign = region + 56; /* foreign + 8 == buf + a.offset */
    memcpy(foreign, "foreign", 8);

    char *const g = arena_grow(&a, foreign, 8, 16);
    assert(g == buf && a.offset == 16);
    assert(strcmp(g, "foreign") == 0);

    char *const heap = malloc(8);
    assert(heap != NULL);
    memcpy(heap, "heapblk", 8);
    char *const h = arena_grow(&a, heap, 8, 8);
    assert(h == buf + 16 && strcmp(h, "heapblk") == 0);
    free(heap); /* still the caller's: arena_grow never took it over */

    arena_destroy(&a);
}

/* A failed grow leaves the block and the arena exactly as they were. */
static void test_grow_failure_leaves_block_intact(void) {
    static char buf[64];
    Arena a;
    arena_init(&a, buf, sizeof(buf));
    char *const p = arena_alloc(&a, 8);
    memcpy(p, "intact!", 8);
    assert(arena_grow(&a, p, 8, SIZE_MAX) == NULL);
    assert(a.offset == 8 && a.large == NULL && strcmp(p, "intact!") == 0);
    assert(arena_grow(&a, NULL, 0, 8) == buf + 8); /* NULL: plain arena_alloc */
    arena_destroy(&a);
}

int main(void) {
    test_bump_allocations_are_aligned_and_in_buffer();
    test_exact_fit_stays_in_buffer();
    test_fallback_past_capacity_and_reset();
    test_huge_sizes_return_null();
    test_yyjson_realloc_huge_returns_null();
    test_grow_last_block_in_place();
    test_grow_non_last_block_copies();
    test_grow_past_capacity_reuses_fallback_node();
    test_grow_foreign_pointer_is_copied();
    test_grow_failure_leaves_block_intact();
    printf("test_arena: all tests passed\n");
    return 0;
}

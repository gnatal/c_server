#include <assert.h>
#include <stdint.h>
#include <stdio.h>
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

int main(void) {
    test_bump_allocations_are_aligned_and_in_buffer();
    test_exact_fit_stays_in_buffer();
    test_fallback_past_capacity_and_reset();
    test_huge_sizes_return_null();
    test_yyjson_realloc_huge_returns_null();
    printf("test_arena: all tests passed\n");
    return 0;
}

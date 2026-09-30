#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_types.h"
#include "response.h"

#include "arena.h"

/*
 * Wire framing of one response: whatever sequence of calls a handler makes, out_buf must hold exactly one
 * well-framed response. Each test decodes the bytes the way a client would and checks where the body ends.
 */

static Connection *make_conn(void) {
    Connection *conn = calloc(1, sizeof(Connection));
    assert(conn != NULL);
    Arena *arena = malloc(sizeof(Arena));
    assert(arena != NULL);
    arena_init(arena, malloc(64 * 1024), 64 * 1024);
    conn->arena = arena;
    conn->keep_alive = 1;
    conn->file_fd = -1;
    return conn;
}

static void free_conn(Connection *conn) {
    void *buf = conn->arena->buf;
    arena_reset(conn->arena);
    free(buf);
    free(conn->arena);
    free(conn);
}

/* Decodes the chunked body that follows the head in `wire` into `out` (NUL-terminated) and returns the number
 * of wire bytes the response used, head through the last-chunk's final CRLF (no trailers expected). A client
 * stops at the first zero-size chunk, so a return below `wire_len` means bytes left over for a "next response". */
static size_t decode_chunked(const char *wire, size_t wire_len, char *out, size_t out_cap) {
    const char *const head_end = strstr(wire, "\r\n\r\n");
    assert(head_end != NULL);
    assert(strstr(wire, "Transfer-Encoding: chunked\r\n") != NULL);
    size_t pos = (size_t)(head_end - wire) + 4;
    size_t out_len = 0;
    for (;;) {
        char *size_end = NULL;
        const unsigned long size = strtoul(wire + pos, &size_end, 16);
        assert(size_end != wire + pos);
        assert(size_end[0] == '\r' && size_end[1] == '\n');
        pos = (size_t)(size_end - wire) + 2;
        if (size == 0) {
            assert(pos + 2 <= wire_len);
            assert(wire[pos] == '\r' && wire[pos + 1] == '\n');
            out[out_len] = '\0';
            return pos + 2;
        }
        assert(pos + size + 2 <= wire_len);
        assert(out_len + size < out_cap);
        memcpy(out + out_len, wire + pos, size);
        out_len += size;
        pos += size;
        assert(wire[pos] == '\r' && wire[pos + 1] == '\n');
        pos += 2;
    }
}

/* An empty chunk mid-body is a no-op: the body is not ended early, and res_end's terminator is the only one. */
static void test_empty_write_mid_body_is_a_no_op(void) {
    Connection *conn = make_conn();
    Response res;
    res_init(&res, conn);

    assert(res_write(&res, "ab", 2) == 0);
    const size_t len_before = conn->out_len;
    assert(res_write(&res, "", 0) == 0);
    assert(conn->out_len == len_before);
    assert(res_write(&res, "cd", 2) == 0);
    res_end(&res);

    char body[16];
    assert(decode_chunked(conn->out_buf, conn->out_len, body, sizeof(body)) == conn->out_len);
    assert(strcmp(body, "abcd") == 0);

    free_conn(conn);
}

/* An empty first write still commits the head (headers set later are not sent) but frames no chunk. */
static void test_empty_first_write_commits_head_only(void) {
    Connection *conn = make_conn();
    Response res;
    res_init(&res, conn);

    assert(res_write(&res, "", 0) == 0);
    assert(res.headers_sent);
    const char *const head_end = strstr(conn->out_buf, "\r\n\r\n");
    assert(head_end != NULL);
    assert((size_t)(head_end - conn->out_buf) + 4 == conn->out_len);

    assert(res_write(&res, "abcd", 4) == 0);
    assert(res_write(&res, NULL, 0) == 0);
    res_end(&res);

    char body[16];
    assert(decode_chunked(conn->out_buf, conn->out_len, body, sizeof(body)) == conn->out_len);
    assert(strcmp(body, "abcd") == 0);

    free_conn(conn);
}

/* Only empty writes: the body is empty and ends once, at res_end. */
static void test_only_empty_writes_end_once(void) {
    Connection *conn = make_conn();
    Response res;
    res_init(&res, conn);

    assert(res_write(&res, "", 0) == 0);
    assert(res_write(&res, "", 0) == 0);
    res_end(&res);

    char body[4];
    assert(decode_chunked(conn->out_buf, conn->out_len, body, sizeof(body)) == conn->out_len);
    assert(body[0] == '\0');

    free_conn(conn);
}

/* A NULL pointer with a nonzero length is refused with nothing written: framing it would promise bytes that
 * never come. The head is not committed either, so the response can still be built normally. */
static void test_null_data_with_length_is_refused(void) {
    Connection *conn = make_conn();
    Response res;
    res_init(&res, conn);

    assert(res_write(&res, NULL, 5) == -1);
    assert(!res.headers_sent);
    assert(conn->out_len == 0);

    assert(res_write(&res, "ab", 2) == 0);
    const size_t len_before = conn->out_len;
    assert(res_write(&res, NULL, 5) == -1);
    assert(conn->out_len == len_before);
    res_end(&res);

    char body[16];
    assert(decode_chunked(conn->out_buf, conn->out_len, body, sizeof(body)) == conn->out_len);
    assert(strcmp(body, "ab") == 0);

    free_conn(conn);
}

int main(void) {
    test_empty_write_mid_body_is_a_no_op();
    test_empty_first_write_commits_head_only();
    test_only_empty_writes_end_once();
    test_null_data_with_length_is_refused();
    printf("all response framing tests passed\n");
    return 0;
}

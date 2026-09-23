/*
 * `make fuzz [FUZZ_ITERS=n]`: mutation fuzzer for the request parser, framing, router and response
 * path, built with ASan + UBSan. Seeds are realistic requests; each iteration flips/inserts/deletes
 * bytes, truncates, and hands the parser an exact-size heap buffer (no NUL, no slack) so any
 * out-of-bounds read is caught. Exit 0 = no finding. Run it after touching lib/http_parser.c or
 * lib/router.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cexpress.h"
#include "arena.h"

Arena test_arena;
char test_arena_buf[64 * 1024];
char fuzz_conn_buf[64 * 1024];

static void h(const Request *r, Response *s) { (void)r; res_send(s, "x"); }
static unsigned long long rng = 88172645463325252ULL;
static unsigned r32(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)(rng >> 16); }
static const char *seeds[] = {
  "GET /api/todos/42?a=1&b=%41%20c&flag HTTP/1.1\r\nHost: x\r\nCookie: a=1; b=2;c\r\nConnection: keep-alive\r\n\r\n",
  "POST /api/todos HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 15\r\n\r\n{\"title\":\"abc\"}",
  "POST /u HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n5;ext=1\r\npedia\r\n0\r\nTrailer: v\r\n\r\n",
  "GET / HTTP/1.1\r\n\r\n", "GET\r\n\r\n", "HEAD /a/b/c/d/e/f/g/h/i HTTP/1.0\r\nX: y\r\n\r\n",
  "PUT /files/x/y/z HTTP/1.1\r\nContent-Length: 0\r\nContent-Length: 0\r\nX-Content-Length: 9\r\n\r\n",
};
int main(int argc, char **argv) {
    arena_init(&test_arena, test_arena_buf, sizeof(test_arena_buf));
  const long iters = argc > 1 ? atol(argv[1]) : 1000000;
  static App app; app_init(&app);
  app_get(&app, "/", h); app_get(&app, "/api/todos/:id", h); app_post(&app, "/api/todos", h);
  app_put(&app, "/files/*", h); app_get(&app, "/a/*/c", h);
  long parsed_ok = 0, complete = 0;
  for (long it = 0; it < iters; it++) {
    const char *seed = seeds[r32() % (sizeof seeds / sizeof *seeds)];
    size_t len = strlen(seed);
    char *buf = malloc(len + 64 + 1);          /* exact-size heap buffer so ASan sees every OOB read */
    memcpy(buf, seed, len);
    int muts = 1 + (int)(r32() % 6);
    for (int m = 0; m < muts; m++) {
      switch (r32() % 6) {
        case 0: if (len) buf[r32() % len] = (char)r32(); break;                    /* random byte */
        case 1: if (len > 1) len = r32() % len; break;                             /* truncate */
        case 2: if (len < 64 + strlen(seed)) { size_t p = len ? r32() % len : 0; memmove(buf + p + 1, buf + p, len - p); buf[p] = "\r\n: ;=%&?/*"[r32() % 12]; len++; } break;
        case 3: if (len > 2) { size_t p = r32() % (len - 1); memmove(buf + p, buf + p + 1, len - p - 1); len--; } break;
        case 4: if (len) { size_t p = r32() % len; buf[p] = "\r\n\0:"[r32() % 4]; } break;
        default: break;
      }
    }
    char *exact = malloc(len ? len : 1); memcpy(exact, buf, len); free(buf); buf = exact;  /* no NUL, no slack */
    size_t hl; int ch; const char *fpath; size_t fpath_len;
    (void)request_framing(buf, len, &hl, &ch, &fpath, &fpath_len);
    if (request_is_complete(buf, len)) complete++;
    if (ch && hl > 0) {
      /* P8: resuming across a random split point must agree with one from-scratch scan. */
      const size_t body_len = len - hl, cut = body_len ? r32() % (body_len + 1) : 0;
      ChunkScanState st = {0}; size_t d_scratch = 0, d_resume = 0;
      const int first = chunked_body_scan_resume(buf + hl, cut, MAX_BODY_SIZE, &st, &d_resume);
      const int scratch = chunked_body_scan(buf + hl, body_len, MAX_BODY_SIZE, &d_scratch);
      if (first == 0) {
        const int resumed = chunked_body_scan_resume(buf + hl, body_len, MAX_BODY_SIZE, &st, &d_resume);
        if (resumed != scratch || d_resume != d_scratch) abort();
      }
    }
    Request req;
    if (parse_http_request(buf, len, &req, &test_arena) == 0) {
      parsed_ok++;
      (void)req_get_header(&req, "host"); (void)req_get_query(&req, "a"); (void)req_get_cookie(&req, "a");
      (void)request_wants_close(&req);
      const Route *rt = match_route(&app, &req);
      Connection conn; memset(&conn, 0, sizeof conn);
      Arena conn_arena; arena_init(&conn_arena, fuzz_conn_buf, sizeof(fuzz_conn_buf)); conn.arena = &conn_arena;
      conn.file_fd = -1; conn.keep_alive = 1;
      Response res; res_init(&res, &conn); dispatch(&app, rt, &req, &res);
      arena_reset(conn.arena);
    }
    arena_reset(&test_arena);
    free(buf);
  }
  printf("fuzz ok: %ld iterations, %ld parsed, %ld complete\n", iters, parsed_ok, complete);
  free(app.connections);
  return 0;
}

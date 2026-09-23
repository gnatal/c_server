/*
 * `make fuzz [FUZZ_ITERS=n]`: mutation fuzzer for the request parser, framing, router and response
 * path, built with ASan + UBSan. Seeds are realistic requests; each iteration flips/inserts/deletes
 * bytes, truncates, and hands the parser an exact-size heap buffer (no NUL, no slack) so any
 * out-of-bounds read is caught. Exit 0 = no finding. Run it after touching lib/http_parser.c or
 * lib/router.c.
 * beyond memory safety it asserts check_answered's oracle on every input - the parser never makes the
 * engine wait on bytes that already hold a whole head (only a genuinely short body may be waited for), the
 * head ends exactly at the first blank line, and the verdict does not depend on how recv() split the bytes.
 * tests/fuzz_connection.c checks the same promise end to end through handle_readable.
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
/* offset just past the first blank line ('\n' followed by "\n" or "\r\n": every line ending
 * picohttpparser accepts), or 0 when there is none. Deliberately parser-independent. */
static size_t first_blank_line_end(const char *buf, size_t len) {
  for (size_t i = 0; i + 1 < len; i++) {
    if (buf[i] != '\n') continue;
    if (buf[i + 1] == '\n') return i + 2;
    if (buf[i + 1] == '\r' && i + 2 < len && buf[i + 2] == '\n') return i + 3;
  }
  return 0;
}

/* the status connection.c answers a failed parse with (same mapping as serve_buffered_requests),
 * 0 for a parse that dispatches. Aborts on a status the engine has no answer for. */
static int answer_for(int parse_status, const Request *req) {
  if (parse_status == 0) return 0;
  if (parse_status != -1 && parse_status != -2 && parse_status != -4) abort();
  return parse_status == -2 ? 414 : req->content_length == -2 ? 413 : req->content_length == -3 ? 501 : 400;
}

static int answer_for_bytes(const char *buf, size_t len) {
  Request req;
  const int st = parse_http_request(buf, len, &req, &test_arena);
  const int answer = answer_for(st, &req);
  arena_reset(&test_arena);
  return answer;
}

/*
 * answered-or-closed oracle ("every input is answered or closed"), for buf[0..len) as the engine's whole buffer:
 *  1. request_is_complete == 0 only while a WELL-FRAMED head waits for a body that is genuinely short
 *     (Content-Length not all there, or a chunked scan that needs more), or while no blank line has
 *     arrived at all (the 431 cap and the request header deadline bound that case in connection.c). Terminated
 *     garbage is always "complete", so it is answered at once instead of held until the timeout.
 *  2. A parsed head ends exactly at the first blank line, on "\r\n\r\n" (a head that reached past it
 *     would disagree with a strict proxy about where the request ends).
 *  3. Recv-split invariance: walking the bytes through random cuts the way connection.c does (head
 *     re-parsed per read, chunk_scan carried) decides "complete" at the same first prefix as a
 *     from-scratch scan; once complete, every longer prefix is complete too; and the answer chosen at
 *     that first prefix is the one the whole buffer gets.
 */
/* an oracle failure names its line and prints the input escaped, so it can become a regression test. */
static void oracle_fail(int line, const char *buf, size_t len) {
  fprintf(stderr, "answered-or-closed oracle failed at tests/fuzz_parser.c:%d on %zu bytes: \"", line, len);
  for (size_t i = 0; i < len; i++) {
    const unsigned char c = (unsigned char)buf[i];
    if (c == '\r') fputs("\\r", stderr); else if (c == '\n') fputs("\\n", stderr);
    else if (c == '"' || c == '\\') fprintf(stderr, "\\%c", c);
    else if (c < 0x20 || c > 0x7e) fprintf(stderr, "\\x%02x\"\"", c);
    else fputc(c, stderr);
  }
  fputs("\"\n", stderr);
  abort();
}
#define ORACLE(cond) do { if (!(cond)) oracle_fail(__LINE__, buf, len); } while (0)

static void check_answered(const char *buf, size_t len) {
  ParsedHead head;
  parse_request_head(buf, len, &head);
  const int complete = request_head_is_complete(&head, buf, len, NULL);
  ORACLE(complete == request_is_complete(buf, len));
  const size_t blank = first_blank_line_end(buf, len);
  if (head.header_len > 0) {
    ORACLE(blank == head.header_len && memcmp(buf + head.header_len - 4, "\r\n\r\n", 4) == 0);
  }
  if (!complete) {
    if (head.header_len == 0) {
      ORACLE(blank == 0); /* a terminated head that is neither parsed nor rejected */
    } else if (head.chunked) {
      size_t decoded;
      ORACLE(chunked_body_scan(buf + head.header_len, len - head.header_len, MAX_BODY_SIZE, &decoded) == 0);
    } else {
      /* waiting on a body that is absent or already all there */
      ORACLE(head.content_length > 0 && len - head.header_len < (size_t)head.content_length);
    }
  }

  ChunkScanState st = {0};
  size_t first = 0;
  for (size_t cut = 0; cut < len && first == 0;) {
    cut += 1 + r32() % (len - cut);
    ParsedHead h;
    parse_request_head(buf, cut, &h);
    const int inc = request_head_is_complete(&h, buf, cut, &st);
    ORACLE(inc == request_is_complete(buf, cut));
    if (inc) first = cut;
  }
  ORACLE((first != 0) == complete);
  /* Any complete prefix (one a recv boundary could land on) stays complete and gets the same answer. */
  for (int s = 0; s < 4 && len > 0; s++) {
    const size_t k = 1 + r32() % len;
    if (!request_is_complete(buf, k)) continue;
    for (size_t j = k + 1; j <= len && j < k + 8; j++) ORACLE(request_is_complete(buf, j));
    ORACLE(complete && answer_for_bytes(buf, k) == answer_for_bytes(buf, len));
  }
}

static const char *seeds[] = {
  "GET /api/todos/42?a=1&b=%41%20c&flag HTTP/1.1\r\nHost: x\r\nCookie: a=1; b=2;c\r\nConnection: keep-alive\r\n\r\n",
  "POST /api/todos HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 15\r\n\r\n{\"title\":\"abc\"}",
  "POST /u HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n5;ext=1\r\npedia\r\n0\r\nTrailer: v\r\n\r\n",
  "GET / HTTP/1.1\r\n\r\n", "GET\r\n\r\n", "HEAD /a/b/c/d/e/f/g/h/i HTTP/1.0\r\nX: y\r\n\r\n",
  "PUT /files/x/y/z HTTP/1.1\r\nContent-Length: 0\r\nContent-Length: 0\r\nX-Content-Length: 9\r\n\r\n",
  "POST /api/todos HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 4\r\n\r\nabcd",
  "GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\nConnection: close\r\n\r\n",
  "\r\nGET / HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n",
};
int main(int argc, char **argv) {
    arena_init(&test_arena, test_arena_buf, sizeof(test_arena_buf));
  const long iters = argc > 1 ? atol(argv[1]) : 1000000;
  if (argc > 2) rng ^= strtoull(argv[2], NULL, 10) * 0x2545F4914F6CDD1DULL; /* FUZZ_SEED; 0 = the default stream */
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
    check_answered(buf, len);
    size_t hl; int ch; const char *fpath; size_t fpath_len;
    (void)request_framing(buf, len, &hl, &ch, &fpath, &fpath_len);
    if (request_is_complete(buf, len)) complete++;
    if (ch && hl > 0) {
      /* resuming across a random split point must agree with one from-scratch scan. */
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
    const int copy_status = parse_http_request(buf, len, &req, &test_arena);
    {
      /* the in-place parser must agree with the copying one - same status, same body bytes - and
       * after its saved byte is restored, nothing past the body may differ (all of it, for a
       * Content-Length body). The one extra byte is the writable raw[raw_len] slot it may NUL. */
      char *mut = malloc(len + 1); memcpy(mut, buf, len); mut[len] = '\0';
      ParsedHead head; parse_request_head(mut, len, &head);
      /* never true for a head the engine would not wait on (incomplete, malformed, 1.0, no body). */
      if (request_head_expects_continue(&head) &&
          (head.header_len == 0 || head.minor_version < 1 || head.content_length < 0 ||
           (!head.chunked && head.content_length == 0))) abort();
      Request ip; char saved = 0;
      /* Only a request the engine would parse: parse_http_request also accepts an incomplete one
       * (a short body is clamped), the connection path never hands it one. */
      const int ready = request_head_is_complete(&head, mut, len, NULL);
      const int ip_status = ready ? parse_http_request_in_place(mut, len, &head, &ip, &test_arena, &saved) : copy_status;
      if (ip_status != copy_status) abort();
      if (!ready) {
        if (memcmp(mut, buf, len) != 0) abort();
      } else if (ip_status == 0) {
        if (ip.content_length != req.content_length || memcmp(ip.body, req.body, (size_t)req.content_length) != 0) abort();
        ip.body[ip.content_length] = saved;
        if (!head.chunked && memcmp(mut, buf, len) != 0) abort();
      } else if (memcmp(mut, buf, len) != 0) {
        abort(); /* a failed parse writes nothing */
      }
      free(mut);
    }
    if (copy_status == 0) {
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

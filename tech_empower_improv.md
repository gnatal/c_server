# TechEmpower improvements: where CExpress loses, and why

Findings from running the TechEmpower Framework Benchmarks (TFB) against CExpress, `lib/` as of commit `bf91ef7`
(identical to the copy vendored in `Rest_in_c/vendor/cexpress/lib`). Full numbers, setup and raw result folders:
`Rest_in_c/benchmark_techempower.md`; the TFB entry itself lives in `Rest_in_c/techempower/cexpress/`.

**Evidence labels** (same as `improvements.md`). MEASURED: from the TFB runs. CODE: cause found by reading the
code, not profiled. ESTIMATED: the gain is an estimate.

**Effort.** S = up to half a day, including the regression test. M = 1–2 days. L = 3 days or more.

**Setup, in short.** TFB's own harness on one Apple Silicon Mac: a Colima VM with 10 CPUs / 12 GB running the
server, TFB's Postgres and wrk together; 2 full runs plus a worker-count sweep, 2026-09-25/26. Linux, epoll backend.
Zero non-2xx responses anywhere; all entries pass TFB verification. Compared against Actix, Axum, h2o and Fiber.

---

## Where CExpress stands

| Test | CExpress best req/s (2 runs) | Place of 5 | vs. leader |
|---|---:|---|---|
| JSON | 764k / 771k | **1st** | ahead of Actix (720k / 753k) |
| Plaintext (pipelined ×16) | 1.59M / 1.60M | 5th | 35–40% of Actix (4.27M / 4.62M) |
| Single query | 155k / 178k | 4th | ~55% (Actix-http 289k / 338k) |
| Multiple queries (20) | 152k / 170k | 4th | ~55% (Axum-pg / Actix-http ~300k) |
| Fortunes | 170k / 181k | 4th | ~60% (h2o / Actix-http ~300k) |
| Updates (20) | 75k / 78k | 4th | ~50% (Actix-http 142k / 169k) |

The per-request path (parse, route, build a response, write it) is competitive. JSON, one small request per
round trip, is won outright. The losses come from two places: writing pipelined responses, and waiting on the database.

## Summary

| ID | Problem | Tests affected | Impact | Effort | Evidence |
|---|---|---|---|---|---|
| **T1** | ~~Pipelined responses are written with one `write` syscall each~~ **Done** (2026-09-26) | plaintext | **High** (2.5–3× gap) | M | MEASURED symptom, CODE cause |
| **T2** | Handlers can't wait on I/O: a DB query blocks the whole worker | db, query, fortune, update | **High** (~2× gap) | L | MEASURED symptom, CODE cause |
| **T3** | No per-request memory that outlives the shared arena reset (prerequisite for T2) | db, query, fortune, update | (part of T2) | M | CODE |
| **T4** | TFB app: one blocking libpq connection per worker, no multiplexing | db, query, fortune, update | High, after T2 | M | CODE |
| **T5** | Worker count is a blunt tool; best setting differs per test | db tests | Low–Medium | S | MEASURED |
| **T6** | `MAX_PIPELINED_PER_EVENT` (16) equals wrk's pipeline depth | plaintext | Low | S | CODE |

Order to do them: **T1** (self-contained, big payoff), then **T3 → T2 → T4** as one project, then retune **T5**.

---

## T1. Pipelined responses: one syscall per response

**Status: done (2026-09-26).** Fix option 1 (a per-worker coalescing buffer). `App.batch_buf` (`BATCH_BUF_SIZE`,
16 KiB); `can_coalesce` / `take_batch` / `flush_batch` / `finish_request` in `lib/connection.c`; rules in
`lib/CLAUDE.md`, "Pipelining, Coalesced writes". Differences from the plan below: the batch is not written by a
separate call before a non-coalescible response but put in front of it (`take_batch` at the start of every
`flush_connection`), so a file, shared body, stream head, `Connection: close` answer or 400/413 rejection carries the
batch in its own `write`/`writev`/`sendfile`; and `request_len == 0` now means "consume nothing" in the keep-alive reset
(it used to drop `in_buf`), which is what a batch flushed alone needs. Tests: 7 new cases in `tests/test_pipelining.c`
counting writes on a `SOCK_DGRAM` socketpair (16 pipelined GETs = 1 write, byte-identical to answering them one by
one). MEASURED locally (macOS loopback, one worker, `wrk -t2 -c64 -d6s`, 16-deep pipeline, 13-byte body, 3 rounds):
440–465k → 1.77–1.81M req/s (~3.9×); non-pipelined unchanged (247–256k → 259k). Not yet re-measured in TFB.

**Symptom (MEASURED).** Plaintext tops out at ~1.6M req/s at every pipelined concurrency level (1.37M, 1.57M,
1.59M, 1.41M at 256/1024/4096/16384 connections). Actix, Axum and Fiber reach 3.4–4.6M on the same run. JSON (not
pipelined, similar response size) is 1st, so the per-request cost itself is fine.

**Cause (CODE).** `serve_buffered_requests` (`lib/connection.c`) runs parse → dispatch → `flush_connection` →
`arena_reset` for each request in `in_buf`. For TFB's 16-deep pipeline, one `recv` brings 16 requests and the
engine answers with **16 `write` calls**, each a ~130-byte response. The fast entries gather every response for the
batch and write once. At these rates the syscall, not HTTP work, is the cost.

**Why it isn't a one-liner.** `out_buf` is allocated from the worker's shared arena (`res_send` → `arena_alloc` in
`response.c`), and `arena_reset(&app->arena)` runs right after each request's flush. So a response can't just stay
in `out_buf` while the next request is dispatched: its bytes are released by the arena reset.

**Fix.**
1. Add a per-worker coalescing buffer (e.g. `App.batch_buf`, 16–64 KB, malloc'd once per worker). After `dispatch`,
   if the response is fully in memory (`out_buf` only; no `file_fd`, no `shared_body`, no producer stream) and the
   batch buffer has room, `memcpy` it there instead of flushing, then `arena_reset` as today.
2. Flush the batch buffer with one `write` when: the loop leaves `serve_buffered_requests` (need more / cap hit), a
   response that can't be coalesced arrives (flush the batch first to keep order, then that response), the
   buffer would overflow, or the connection is closing (`Connection: close` - flush, then close).
3. On a partial write / `EAGAIN`, move the unsent tail into the connection-owned buffer exactly as
   `keep_unsent_and_wait` does now, so `FLUSH_PENDING` semantics don't change.
4. An alternative with no copy: keep the arena alive for the whole batch (reset once per `serve_buffered_requests`
   call, not per request) and `writev` the collected `out_buf`s. Simpler memory-wise, but the arena then holds up to
   16 requests' allocations; check `ServerConfig.max_buffered_bytes` accounting.

**Test.** A `socketpair` test that sends 16 pipelined `GET`s in one write and asserts (a) responses come back
byte-identical and in order, and (b) the count of `conn_write`/`conn_writev` calls is 1 (a counter in a test build,
or wrap `conn_write`). Plus: a pipeline with a static file in the middle, one with `Connection: close` in the
middle (nothing after it is answered), and a batch larger than the buffer.

**Expected gain (ESTIMATED).** Plaintext 2–2.5×, putting it in the 3–4M range with Fiber/Axum on this machine.
No effect on non-pipelined traffic (a batch of one is flushed exactly as today).

---

## T2. Handlers can't wait on I/O

**Symptom (MEASURED).** On all four DB tests CExpress runs at 50–60% of Actix-http, Axum-pg and h2o. It still beats Fiber.

**Cause (CODE).** `lib/CLAUDE.md`, "Model": *handlers run synchronously on the loop: a blocking call (DB, sleep)
stalls that whole worker, so scale with workers, not threads.* The TFB handlers call libpq and block in
`PQgetResult` until Postgres answers. While a worker waits, it serves nobody. The Postgres variant runs 4 workers
per core to cover the wait, which costs context switches and a Postgres backend per worker (40 with 10 CPUs),
and T5 shows that doesn't close the gap. The leaders keep a few connections per core busy with many in-flight
queries, and serve other requests while they wait.

**Fix: a way for a handler to suspend and be resumed by the event loop.** The engine has the pieces (per-fd
readiness in every backend, a `Connection` state machine that already pauses for `FLUSH_PENDING`); what's missing
is a public API. Sketch:

```c
/* Handler side */
void res_defer(Response *res);                        /* "no response yet": the engine must not flush */
void res_resume(Connection *conn);                    /* later: response built, flush it and continue the pipeline */

/* Loop side: watch any fd owned by the app */
int app_watch_fd(App *app, int fd, unsigned events,
                 void (*on_ready)(App *app, int fd, unsigned events, void *udata), void *udata);
int app_unwatch_fd(App *app, int fd);
```

What the engine has to do when a handler defers:
- Stop reading or serving that connection's pipelined requests (responses must stay in order), exactly as
  `FLUSH_PENDING` does today: unwatch read, leave `in_off` at the next request.
- Keep everything the deferred response still needs alive past the handler's return: see **T3**.
- Keep the connection's idle / write-stall timers from firing on a response that is merely waiting for the DB, but
  add a separate deadline (e.g. 30 s) that answers 504 and closes.
- If the client disconnects while deferred, the app must be told (a cancel callback) so it doesn't touch a freed
  `Connection`. Simplest safe rule: `res_resume` takes a generation-checked handle, not a raw pointer.
- kqueue, epoll and io_uring backends all need the new external-fd watch: `event_loop_watch_read` today takes a
  `Connection *` as `udata`, and the dispatcher assumes that type. A tagged udata (`{kind, ptr}`) fixes it.

**Test.** A fake-DB test: an app watches one end of a `socketpair`, the handler defers, the test writes to the pair,
and the response arrives. Plus: two pipelined requests where the first defers (the second must not be answered
first), client close while deferred (no use-after-free under ASan), and the defer deadline.

**Expected gain (ESTIMATED).** This is how the leaders get ~300k on db/fortune here; with T4 CExpress should land
within 10–20% of them. The engine itself, as JSON shows, isn't the bottleneck.

---

## T3. Deferred responses need memory that outlives `arena_reset`

**Cause (CODE).** `lib/CLAUDE.md`, Files: the arena is *one shared per worker process (`App.arena`), not one per
connection*, and `serve_buffered_requests` resets it after every dispatch. `Request` fields are views into
`in_buf`, which may be the worker's borrowed `App.read_buf`. Both assumptions hold only because a handler finishes
before the next request starts. A deferred handler breaks both:
- anything it allocated from the arena (parsed query params, the JSON doc it will fill in) is gone at the next reset;
- `req` views into a borrowed `App.read_buf` are overwritten by the next `recv` on another connection.

**Fix.** On `res_defer`:
- Copy the request's live bytes out of a borrowed `read_buf` into a connection-owned buffer; `stop_borrowing_read_buf`
  already does this for pipelined tails.
- Give the deferred request its own small arena (from a per-worker free list of arena blocks, so no malloc in the
  steady state), and have `res_*` build into it. `arena_reset(&app->arena)` then no longer touches it; the block goes
  back to the free list after the response is flushed.
- Count deferred arenas against `ServerConfig.max_buffered_bytes` so a slow DB can't grow memory without bound: over
  budget, new requests get 503 instead of deferring.

**Test.** ASan build of the T2 tests with many concurrent deferred requests across connections, plus a test that
fills the budget and checks for 503.

---

## T4. TFB app: one blocking connection per worker

**Cause (CODE).** `Rest_in_c/techempower/cexpress/src/db.c` opens one libpq connection per worker and already uses
pipeline mode (`PQenterPipelineMode`, `PQsendQueryPrepared` ×N, `PQpipelineSync`) so the 20 queries of `/queries`
and `/updates` go out in one round trip. That was a good call. But it still ends in a blocking `PQgetResult`, and
the pipeline only ever carries one HTTP request's queries.

**Fix, after T2/T3.**
- `PQsetnonblocking(conn, 1)`, register `PQsocket(conn)` with `app_watch_fd`, and have each handler append its
  queries to the shared pipeline and `res_defer`. Keep a FIFO of `{handle, how many results, callback}`.
- In `on_ready`: `PQconsumeInput`, then drain `PQgetResult` while `!PQisBusy`, popping FIFO entries as their
  `PGRES_PIPELINE_SYNC` arrives; build the response and `res_resume`.
- Queries from many HTTP requests then share one connection's pipeline: that is what the leaders do, and it is
  why they reach ~300k with a handful of connections.
- Then drop the Postgres image's `CEXPRESS_WORKERS=$(nproc)*4` back to one worker per core (see T5).
- Watch write readiness too: under load `PQflush` can return 1 (output not fully sent).

**Test.** In the TFB folder: `./techempower/run.sh` (TFB's own verify must stay all-PASS), then a full benchmark run.

---

## T5. Worker count is a blunt tool

**MEASURED** (cexpress-postgres, one run per setting; 4× is the average of the two full runs):

| Workers per core | db | query | fortune | update |
|---|---:|---:|---:|---:|
| 2× | **216,946** | **206,326** | 196,882 | 65,227 |
| 4× (shipped) | 166,381 | 160,881 | 175,734 | 76,447 |
| 8× | 162,088 | 140,014 | 172,581 | **86,795** |
| 16× | 180,131 | 143,256 | **197,681** | 83,613 |

Reads prefer fewer workers (less contention with wrk and Postgres on shared cores); updates prefer more (each
worker spends longer blocked on row locks). No setting passes ~70% of the leaders, so this is a symptom of T2, not a
fix. **After T2/T4, retune from 1× per core.** Until then, 2× is a better default for the read tests if the entry
is submitted before T2 lands. Single runs, so treat differences under ~10% as noise.

---

## T6. The pipelining cap equals wrk's pipeline depth

**Cause (CODE).** `MAX_PIPELINED_PER_EVENT` is 16 (`lib/app_types.h`), and TFB's plaintext sends batches of 16. When
more than one batch is already buffered (possible under load, since the `recv` reads up to `BUF_SIZE` = 8 KB and a
batch is ~2.6 KB), the cap is hit mid-buffer and the rest waits for an `event_loop_watch_write` round trip: one
extra `epoll_ctl` plus one extra wakeup.

**Fix.** Keep the fairness cap, but count bytes or batches, not requests, or raise it to 64. Measure the change
with T1 in place, since T1 changes the per-request cost the cap is balancing. Not worth doing alone.

---

## Not a problem

- **JSON path**: 1st in both runs. yyjson with the arena allocator, the per-second `Date` cache (`http_date_for`) and
  one `write` per response are already on par with or ahead of the Rust entries.
- **Correctness under load**: zero non-2xx across the ~2.6 billion requests of the 5 runs (all entries). The wrk socket timeouts seen at
  the highest concurrency levels appear for every framework in similar numbers; they come from wrk, the server and
  Postgres sharing 10 cores, not from CExpress.

## Caveat on the numbers

TechEmpower runs the server, database and load generator on three separate machines with many cores and 10 GbE.
Here all three shared one laptop's VM, so absolute req/s are low and noisy (up to ~15% between runs for some
entries). The ranking within a run is what matters; re-measure after each fix with `./techempower/run.sh --mode
benchmark` in `Rest_in_c`, with the same competitors in the same run.

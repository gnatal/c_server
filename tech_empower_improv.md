# TechEmpower improvements: where CExpress loses, and why

Findings from running the TechEmpower Framework Benchmarks (TFB) against CExpress, `lib/` as of commit `bf91ef7`
(identical to the copy vendored in `Rest_in_c/vendor/cexpress/lib`). Full numbers, setup and raw result folders:
`techEmpV1.md`; the TFB entry itself lives in `Rest_in_c/techempower/cexpress/`.

**Evidence labels** (same as `improvements.md`). MEASURED: from the TFB runs. CODE: cause found by reading the
code, not profiled. ESTIMATED: the gain is an estimate.

**Effort.** S = up to half a day, including the regression test. M = 1–2 days. L = 3 days or more.

**Setup, in short.** TFB's own harness on one Apple Silicon Mac: a Colima VM with 10 CPUs / 12 GB running the
server, TFB's Postgres and wrk together; 2 full runs plus a worker-count sweep, 2026-09-25/26, then a plaintext-only
run after T1, full runs 3 and 4 after T2–T4, and two A/B runs of the Postgres flush strategy (runs 5 and 6, DB
tests only, all variants in the same run). Linux, epoll backend.
Zero non-2xx responses anywhere; all entries pass TFB verification. Compared against Actix, Axum, h2o and Fiber.

---

## Where CExpress stands

MEASURED. Best req/s across levels; full tables in `techEmpV1.md`. Runs 1 / 2: before any fix. After: plaintext from
the plaintext-only run after T1 and full run 4 (`20260926145131`); DB tests from full runs 3 (`20260926132736`) and
4. Now: DB tests from run 10 (`20260927113740`, eight entries including Round 23's leaders), the first run with one
Sync per SQL statement (TFB general requirement #7); JSON and plaintext unchanged since run 4.

| Test | Runs 1 / 2 | Place | After (runs 3 / 4) | Place | Now (run 10) | Place | vs. best other (now) |
|---|---:|---|---:|---|---:|---|---|
| JSON | 764k / 764k | **1st** | 788k (run 4) | **1st** | (run 4) | **1st** | 107% of Actix (739k) |
| Plaintext (pipelined ×16) | 1.59M / 1.60M | 4th | 4.91M / 4.81M | **1st / 1st** | (run 4) | **1st** | 109% of Actix |
| Single query (db) | 155k / 171k | 4th | 298k / 300k | 4th / 4th | 328k | 2nd | 98.8% of xitca-web-barebone (332k) |
| Multiple queries, 1 query | 152k / 170k | 4th | 301k / 303k | 1st / 3rd | 329k | **1st** | 100.3% of may-minihttp (328k) |
| Multiple queries, 20 queries | – | – | (not compliant) | – | 29.7k | **1st** | 100.4% of Actix-http (29.6k) |
| Fortunes | 163k / 181k | 4th | 283k / 275k | 3rd / 4th | 311k | 4th | 94.2% of xitca-web-barebone (330k) |
| Updates, 1 query | 70k / 73k | 4th | 159k / 157k | 1st / 1st (tie) | 165k | **1st** | 106.6% of Actix-http (155k) |
| Updates, 20 queries | – | – | (not compliant) | – | 20.6k | **1st** | 102.7% of xitca-web (20.1k) |

Runs 1-9 sent all of a request's SELECTs with one Sync, which TFB's rules forbid, so their 5-20 query results (1.4-2.3×
ahead of every other entry) do not count; db, fortune and the 1-query cases send one statement per request and are
unaffected. With one Sync per statement, query and update stay 1st at every count but by 0.4-6.6%. The two losses
found in runs 1 / 2, pipelined writes (T1) and waiting on the database (T2-T4), are fixed. **Fortune is the one
consistent gap: 91-94% of xitca-web-barebone in runs 8, 9 and 10** (T7).

## Summary

| ID | Problem | Tests affected | Impact | Effort | Evidence |
|---|---|---|---|---|---|
| **T1** | ~~Pipelined responses are written with one `write` syscall each~~ **Done** (2026-09-26) | plaintext | **High** (2.5–3× gap) | M | MEASURED symptom, CODE cause |
| **T2** | ~~Handlers can't wait on I/O: a DB query blocks the whole worker~~ **Done** (2026-09-26), used by T4 | db, query, fortune, update | **High** (~2× gap) | L | MEASURED symptom, CODE cause |
| **T3** | ~~No per-request memory that outlives the shared arena reset~~ **Done** (2026-09-26), with T2 | db, query, fortune, update | (part of T2) | M | CODE |
| **T4** | ~~TFB app: one blocking libpq connection per worker, no multiplexing~~ **Done** (2026-09-26); full run 3: db/fortune at parity, query/update 1st | db, query, fortune, update | High, after T2 | M | CODE, MEASURED (1 run) |
| **T5** | ~~Worker count is a blunt tool; best setting differs per test~~ **Done** (2026-09-26): 1 per core stays, beats 2 per core by 5–6% on db/query (one run) | db tests | Low–Medium | S | MEASURED |
| **T6** | `MAX_PIPELINED_PER_EVENT` (16) equals wrk's pipeline depth | plaintext | Low | S | CODE |
| **T7** | Fortune 3-9% behind the Round 23 leaders; workers not pinned to CPUs. Pinning **implemented as an A/B toggle** (2026-09-27), awaiting a TFB run | fortune | Medium (+4% locally) | S | MEASURED (perf, local A/B) |

Order to do them: **T1** (self-contained, big payoff), then **T3 → T2 → T4** as one project, then retune **T5**.
T1–T5 are done, and full run 4 confirms the ranking. One `send` per event-loop turn to Postgres is the shipped
behavior (T4's status): +10–13% on db and single-query in run 5. Run 5's update loss (−8–16% at 1–5 queries) did not
reproduce in run 6, and flushing an `/updates` UPDATE at once instead of at turn end made no measurable difference.

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
440–465k → 1.77–1.81M req/s (~3.9×); non-pipelined unchanged (247–256k → 259k).
**MEASURED in TFB (2026-09-26, plaintext only, one run, same setup, same competitors):** 1st at every pipelined level,
with the lowest average latency at each. req/s at 256 / 1,024 / 4,096 / 16,384 connections: cexpress 4,894,116 /
4,910,252 / 4,095,808 / 3,361,283; actix 4,689,204 / 4,588,299 / 3,865,569 / 3,119,534; axum 3.69M best; fiber 3.65M
best; h2o 1.62M best. Best-level result 1.60M → 4.91M (3.07×). One run: the 4–8% lead over Actix is inside the
run-to-run noise seen before (Actix 4.27M / 4.62M in the two full runs), so repeat before quoting a ranking.

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

**Status: done (2026-09-26); the TFB app uses it since T4.** API: `res_defer(res)` → `DeferHandle`,
`res_resume(app, h)` → `Response *` (NULL once the request is gone), `req_deferred(app, h)`, `app_watch_fd(app, fd,
WATCH_READ | WATCH_WRITE, cb, udata)` / `app_unwatch_fd`, and `app_run_once` (one loop turn; `app_listen_worker` loops
over it). Differences from the sketch below: `res_resume` takes a generation-checked handle, not a `Connection *`, and
there is no cancel callback (a stale handle resolves to NULL, after a client close, the 504 deadline or shutdown).
Resumed responses are written from a queue drained after every event and before every poll, with no extra
syscall, instead of through write readiness. External fds live in `App.watched[fd]` (every backend is fd-indexed except kqueue's
udata), not in a tagged udata. A coalesced batch in front of a deferred request is flushed when it parks; a tail the
socket does not take is held and written in front of the deferred answer. Details: `lib/CLAUDE.md`, "Behavior
reference, Deferred responses" and "Application fds". Tests: `tests/test_defer.c` (14 cases, including every scenario
listed below plus a batch tail held across the deferral, slot reuse with a stale handle, over-budget 503, 100 connections
× 3 rounds), passing on kqueue, epoll (epoll-shim and Linux) and io_uring (Linux, Docker), plain and under ASan + UBSan;
mutation-checked with 19 injected bugs; cookbook recipe 15 (job queue over a socket, long poll). MEASURED no regression
(macOS loopback, one worker, `scripts/tfb_plaintext.sh HEAD`, 10 s per level): pipelined ×16 0.99x / 0.99x at 256 / 1,024
connections, then 1.03x at 256; non-pipelined 1.00x at 64, and 0.94x / 1.03x / 1.00x in three runs at 256 (noise).

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

**Status: done (2026-09-26), used by `res_defer` (T2).** Differences from the plan below: nothing is
copied out of the shared arena. `arena_hand_over` gives the deferred request the whole `App.arena` (buffer, fallback
list), and `App.arena` restarts on a block from `App.arena_pool`, a free list of `ARENA_SIZE` blocks (at most 64 spares
kept). So headers set and JSON built before deferring stay valid. `Arena.large_bytes` makes the kept arena chargeable
to `max_buffered_bytes`. `request_clone_used` and `response_clone_used` copy only the used slots of the stack
`Request`/`Response`; the Request clone rebases header and body views onto a copy of the request's wire bytes. The
deferred request is charged `ARENA_SIZE + large_bytes` (+ a held batch tail) and released to the pool after its flush. Tests: `test_arena.c`,
`test_http_parser.c: test_request_clone_used`, `test_response.c: test_response_clone_used_sends_identical_bytes`, each
mutation-checked.

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

**Status: done (2026-09-26), in `Rest_in_c/techempower/cexpress/src/db.c` + `main.c`.** As planned below:
non-blocking libpq in pipeline mode, `PQsocket` watched with `app_watch_fd` (registered from the worker-start hook,
applied when the loop opens), one connection per worker shared by every request, a FIFO of jobs in send order (each
request's queries end with a sync, so each `PGRES_PIPELINE_SYNC` completes the head job), write interest only while
`PQflush` returns 1. `/updates` is a two-phase job: its UPDATE goes to the back of the pipeline when its SELECTs'
sync arrives. Jobs come from an app-owned free list, never the request arena (the client may leave mid-query; its
result is then dropped via `res_resume` returning NULL). A broken connection answers every queued and later request 500.
Workers: one per core (`CEXPRESS_WORKERS` overrides). TFB verify: all PASS. MEASURED (one run, DB tests only, against
actix-http alone, same setup as above; `techEmpV1.md`, "Run 3"): db 295k vs
307k (was 155-178k), query 294k vs 308k at 1 query and 65k vs 29k at 20, fortune 280k vs 299k (was 170-181k), update
167k vs 156k at 1 query and 34k vs 19k at 20 (was 75-78k). Zero non-2xx. **MEASURED in full run 3** (`20260926132736`, all five
frameworks, DB tests only, one run): db 297,671 (4th; Actix-http 307,632, Axum-pg 302,879, h2o 301,387), query 301,411
(1st), fortune 282,844 (3rd; h2o 295,283), update 158,507 (1st; Actix-http 151,049). At 20 queries 65.5k vs 28.5k (best
other), at 20 updates 34.3k vs 18.9k. Zero non-2xx; wrk timeouts in the same range as the other entries. Per-level
numbers: `techEmpV1.md`, "Run 3". **Full run 4** (`20260926145131`, same code, all six tests):
db 299,887 (4th, top four within 3.8%), query 302,805 at 1 query (3rd, within 3.8%) and 66,051 at 20 (2.27×),
fortune 275,448 (4th, 93% of h2o), update 157,191 at 1 query (tied 1st with Actix-http at 157,190) and 33,306 at 20
(1.69×). Zero non-2xx. Open: at 16 connections Actix-http is ~2× every other entry
(db 146k vs cexpress 79k, axum 71k, h2o 63k; fortune 169k vs 78k), so this is Actix-specific rather than a CExpress
gap; from 32 connections the top four track each other.
**One flush per event-loop turn (2026-09-26).** Engine: `app_on_turn_end` (a hook called once at the end of every
`app_run_once`, after all its events and resumed responses; `lib/CLAUDE.md`, "Event-loop turn"). App: libpq 18.6 from
the PostgreSQL apt repository (Ubuntu 24.04 ships 16, whose `PQpipelineSync` flushes per request); every sync is
`PQsendPipelineSync` (buffered), and the hook does one `PQflush` per turn. Syncs are unchanged by the batching (at the time one per request; one per statement since run 10, see below). The Postgres socket is flushed
from the readiness callback only on write readiness (the rest of a partial send). **MEASURED** (run 5, `20260926163928`, one run, batched vs
flush-each in the same run, 1 worker per core): db 321,196 vs 291,353 (1.10×), query 325,489 vs 286,777 at 1 query
(1.13×) and 1.04× at 20, fortune 292,644 vs 286,124 (1.02×), update 139,560 vs 151,470 at 1 query (**0.92×**; 0.84× at
5, even from 10). actix-http in the same run: db 322,366, query 316,781, fortune 280,625, update 163,504. The
flush-each baseline was an env toggle of the same binary, since removed.
**MEASURED** (run 6, `20260926224646`, one run, db and update only): the unchanged batched build, in the same position
of the same test order, did 160,909 at 1 update and 85,093 at 5 (run 5: 139,560 and 69,951), ahead of run 5's
flush-each numbers and of actix-http (154,827 and 61,086); run 5's update loss did not reproduce. The suspected cause
was tested in the same run: a variant that flushes right after reading Postgres results whenever those results queued
new queries (the `/updates` UPDATE), instead of waiting for the end of the turn. Updates +0.5% to +2.8% at every
count, inside noise; not kept. On `/db` that variant runs the batched code exactly, so its db column is an A/A
measure of noise: 1–3% apart from 32 connections, 12% at 16.

**Rules compliance (2026-09-27).** Until run 9 each request's SELECTs shared one sync, which TFB general requirement
#7 forbids ("If using PostgreSQL's extended query protocol, each query must be separated by a Sync message"): it runs
them in one implicit transaction, where an error skips the rest. TFB verify cannot see this (it counts statements
with `pg_stat_statements`). Since run 10 every statement gets its own sync (`send_selects`, `DbJob.syncs_left`), and
the lazy PREPARE of the `/updates` UPDATE too. MEASURED (run 10, `20260927113740`, one run): query at 20 went from
66k (runs 7-9) to 29.7k, 1st by 0.4%; update at 20 from 33k to 20.6k, 1st by 2.7%. db, fortune and single-query
numbers match runs 7-9. The 1.4-2.3× leads at 5-20 queries quoted below and in runs 3-9 came from the shared sync.

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
fix. **After T2/T4, retune from 1× per core.** The table above is from the blocking app and doesn't carry over.

**Status: done (2026-09-26).** MEASURED, run 5 (`20260926163928`, one run, batched per-turn flush, same run): 1× vs 2×
per core: db 321,196 vs 305,382 (1×, +5%), query 325,489 vs 306,823 at 1 query (+6%), fortune 292,644 vs 290,215 (tie),
update 139,560 vs 152,828 at 1 query (2× +9.5%, the same as flush-each at 1×; run 6 did not reproduce the batched
update loss, see T4's status). From 10 queries on both are within 4%. The default stays at one worker per core; with one
non-blocking connection per worker, more workers only add Postgres backends and context switches. Single runs, so treat differences under ~10% as noise.

---

## T6. The pipelining cap equals wrk's pipeline depth

**Cause (CODE).** `MAX_PIPELINED_PER_EVENT` is 16 (`lib/app_types.h`), and TFB's plaintext sends batches of 16. When
more than one batch is already buffered (possible under load, since the `recv` reads up to `BUF_SIZE` = 8 KB and a
batch is ~2.6 KB), the cap is hit mid-buffer and the rest waits for an `event_loop_watch_write` round trip: one
extra `epoll_ctl` plus one extra wakeup.

**Fix.** Keep the fairness cap, but count bytes or batches, not requests, or raise it to 64. Measure the change
with T1 in place, since T1 changes the per-request cost the cap is balancing. Not worth doing alone.

---

## T7. Fortune: behind the Round 23 leaders

**Symptom (MEASURED).** Fortune is the one DB test where CExpress trails: 91.2-94.2% of xitca-web-barebone and
92.9-97.0% of may-minihttp in runs 8, 9 and 10, while db, which runs the same request path except the 12-row result
and the HTML render, is at parity.

**Profile (MEASURED, 2026-09-27).** Each server run alone in the Colima VM with TFB's Postgres image and wrk image
(`wrk -c 512 -t 10`, TFB's headers), `perf record -a -g -F 999` for 10 s in the middle of a 25 s load, the run-10
image. 999 samples ≈ one CPU-second.

| Per request | cexpress fortune | xitca-web-barebone fortune | cexpress db | xitca-web-barebone db |
|---|---:|---:|---:|---:|
| req/s | 299,057 | 314,498 | 319,868 | 317,845 |
| server CPU (user + kernel) | 9.40 µs | 9.31 µs | 7.61 µs | 8.18 µs |
| Postgres CPU | 9.88 µs | 9.85 µs | 10.49 µs | 10.08 µs |
| wrk CPU | 6.49 µs | 6.62 µs | 6.05 µs | 6.58 µs |
| idle (`swapper`) samples | 7,820 | 843 | 8,930 | 1,261 |

- Per-request CPU is the same for both servers on fortune; CExpress leaves more of the VM idle (on db too, where
  its lower server CPU per request makes up for it). Fortune adds 1.79 µs of server CPU per request to CExpress
  (fortune minus db) and 1.13 µs to xitca, which cancels the db advantage.
- Postgres work per request is equal (9.88 vs 9.85 µs), so libpq's per-execute Describe (xitca-postgres and
  may_postgres skip it) costs nothing measurable here.
- Inside `tfb-cexpress`: `done_fortunes` is the top user symbol (2,139 samples, 0.72 µs per request); with
  `snprintf`, `cmp_fortune` and the sort's `memcmp` the render is ~0.88 µs per request, 3.1% of all VM CPU. libpq
  2,007 samples, the app's kernel time 17,612 (TCP).
- Syscalls (`perf stat`, 5 s): 0.96 `write` per response, 1.10 `recvfrom`, 0.16 `sendto` to Postgres (one flush per
  ~6 requests), 0.20 `epoll_pwait`; 2.4 syscalls and 0.16 context switches per request. Nothing to remove.
- Render microbenchmark (macOS M3, gcc-16 -O2, TFB's 12 rows + 1, the code of `done_fortunes`): 2.05 µs total:
  escape loop 1.40 µs (1.5 ns per byte: the byte store through `char *` may alias `HtmlOut`, so `o->len` and
  `o->buf` are reloaded per byte), 13× `snprintf("%d")` 0.44 µs, `qsort` 0.16 µs, body copy in `res_send_bytes`
  0.04 µs, 12× `atoi` 0.08 µs.
- The leaders' code (TFB checkout): the same query shape (one prepared statement, one Sync), zero-copy message
  views, a sort, and escaped rendering into a preallocated buffer (yarte / sailfish). xitca-web-barebone pins each
  thread to one core (`core_affinity`); CExpress's workers were not pinned.

**Candidates, ranked.**

| # | Fix | Evidence | Gain | Effort |
|---|---|---|---|---|
| 1 | Pin each worker to one CPU | MEASURED, `taskset` on the running workers, 3 alternating pairs: fortune 298.3k → 310.1k (+4.0%); db 2 pairs, no effect (302.3k / 303.3k unpinned, 295.6k / 306.1k pinned) | +4% | S |
| 2 | Faster render: integer formatter, escape that copies runs between special bytes, insertion sort | perf 0.88 µs per request; microbenchmark 2.05 µs → ~0.3 µs (ESTIMATED) | ≤ 2.5% (its CPU share; the VM is not fully busy) | S |
| 3 | Render into the response buffer (no body copy) | microbenchmark 0.04 µs | ~0.2% | M (engine API) |
| 4 | Binary result format (no `atoi`) | microbenchmark 0.08 µs | ~0.3% | S |
| 5 | No Describe per execute | Postgres CPU equal to xitca's | ~0 | L (own wire protocol) |
| 6 | Fewer syscalls | already one `write` per response | ~0 | – |

**Status: #1 implemented (2026-09-27), as an A/B toggle.** `src/pin.c`: `pin_worker(cluster_worker_id())` from an
`app_on_worker_start` hook registered before `db_setup`'s (so each worker connects to Postgres from its own CPU);
worker i gets the i-th CPU of its startup affinity (`pin_cpu_for_worker`, pure, `tests/test_pin.c`, mutation-checked).
Only the `cexpress-postgres` entry; `CEXPRESS_PIN_WORKERS=0` turns it off, and the `cexpress-postgres-nopin` TFB
entry is the same binary with that set. MEASURED (same local setup, the built image, the toggle itself, 3
alternating pairs): fortune 294.6k off → 307.6k on (+4.4%); response bytes identical. TFB verify passes both entries.
Next: a TFB run with `cexpress-postgres` and `cexpress-postgres-nopin` in the same run (all four DB tests), then keep
or drop it. Candidate #2 is the next one if the gap remains.

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

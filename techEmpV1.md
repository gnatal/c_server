# TechEmpower (TFB) Benchmark: CExpress

All six TFB test types, run by TechEmpower's own harness (`Rest_in_c/techempower/run.sh`): their Postgres image,
their wrk load generator, their request mix and concurrency levels. Every entry passes TFB verification. Result
folders are under `Rest_in_c/techempower/.tfb/results/<timestamp>/`.

## Current state

Run 10 (`20260927113740`, 2026-09-27) is the current, rules-compliant build: one Sync per SQL statement (see
[TFB rules compliance](#tfb-rules-compliance)), one `PQflush` per event-loop turn, one worker per core, libpq 18.6.
Eight entries on the four database tests: the five used since run 1 plus Round 23's database leaders
`may-minihttp`, `xitca-web` and `xitca-web-barebone`. Every test succeeded; zero non-2xx; zero wrk timeouts for
cexpress-postgres on every test.

- **Wins.** JSON: 1st in runs 1, 2 and 4 (787,754 vs actix 738,709 in run 4). Plaintext: 1st in both runs since
  pipelined responses are written together (4.81M vs actix 4.41M in run 4). Neither test has been re-run since. Updates: 1st at
  every query count in run 10, 2.7-6.6% ahead of the next entry, and 1st at one update in runs 8, 9 and 10.
- **Ties.** db and single query: the top three (cexpress, may-minihttp, xitca-web-barebone) are within 2.4% (db)
  and 0.8% (single query) in run 10. cexpress was 1st, 5th, 2nd and 2nd on db in runs 7-10. Multiple queries at 10 and 20: cexpress is 1st by 0.4-1.6%, which is
  inside single-run noise; at 5 queries it leads by 6.4%.
- **Trails.** Fortune: 4th in run 10 at 94.2% of xitca-web-barebone and 97.0% of may-minihttp. Across runs 8, 9 and
  10 it is 91.2-94.2% of xitca-web-barebone and 92.9-97.0% of may-minihttp, while level with actix-http and h2o.
- **Setup caveat.** One laptop: server, Postgres and wrk share a 10-CPU Colima VM, where TechEmpower uses three
  machines. Absolute req/s are low, and single-run differences under a few percent are noise (an A/A pair in run 6
  was 2% apart at the best level and 12% at 16 connections). Rankings within one run are what this measures.

Run 10, best req/s (for query and update, at the query count in the row name):

| Test | cexpress | may-minihttp | xitca-web-barebone | actix-http | h2o | xitca-web | axum-pg | fiber | cexpress place | vs. best other |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| db | 328,331 | 324,252 | **332,258** | 282,603 | 313,905 | 312,819 | 309,672 | 131,424 | 2nd | 98.8% of xitca-web-barebone |
| query, 1 query | **329,366** | 328,412 | 326,744 | 311,912 | 316,957 | 311,579 | 302,375 | 125,554 | 1st | 100.3% of may-minihttp |
| query, 5 queries | **110,799** | 92,742 | 93,956 | 100,647 | 87,092 | 104,086 | 103,264 | 34,203 | 1st | 106.4% of xitca-web |
| query, 10 queries | **57,868** | 50,752 | 50,449 | 51,991 | 45,099 | 56,975 | 56,952 | 18,433 | 1st | 101.6% of xitca-web |
| query, 20 queries | **29,713** | 25,105 | 24,722 | 29,582 | 23,420 | 28,206 | 29,173 | 9,580 | 1st | 100.4% of actix-http |
| fortune | 310,625 | 320,386 | **329,595** | 290,773 | 313,540 | 295,349 | 289,137 | 118,865 | 4th | 94.2% of xitca-web-barebone |
| update, 1 query | **165,029** | 146,817 | 141,732 | 154,826 | 148,476 | 146,809 | 143,973 | 68,815 | 1st | 106.6% of actix-http |
| update, 5 queries | **64,341** | 55,942 | 57,217 | 62,488 | 53,736 | 59,001 | 59,193 | 24,648 | 1st | 103.0% of actix-http |
| update, 10 queries | **37,123** | 33,273 | 33,901 | 35,945 | 31,082 | 35,844 | 35,958 | 13,853 | 1st | 103.2% of axum-pg |
| update, 20 queries | **20,641** | 17,808 | 18,331 | 19,196 | 16,792 | 20,107 | 19,554 | 7,412 | 1st | 102.7% of xitca-web |

TFB verify in run 10: every entry passes db, query and fortune; axum-pg, xitca-web and xitca-web-barebone get a
WARN on update (they still pass rows updated, and TFB benchmarks WARN entries). Local patches in TFB's checkout,
needed to build and not touching server code: xitca-web's git dependencies moved from `http://` to `https://`.
`ntex-db` (Round 23 #2 on db/query) is not included: current ntex crates no longer compile with its archived code.

## What the numbers say

- **The request path is competitive.** CExpress won JSON in runs 1, 2 and 4, and plaintext since pipelined responses
  are gathered and written once (1.60M → 4.81-4.91M req/s, 1st in both runs that measured it).
- **The database tests are at parity with Round 23's leaders.** With libpq's socket in the event loop, a worker
  serves other requests while its queries are in flight, and one pipelined connection per worker carries every
  request's statements. db and single query are a three-way tie with may-minihttp and xitca-web-barebone.
- **Multiple queries and updates: a narrow lead, not the 1.4-2.3× of runs 1-9.** That margin came from sending a
  request's SELECTs with one Sync, which TFB's rules forbid. With one Sync per statement (run 10) CExpress is still
  1st at every count, by 0.4-6.6%, mostly inside noise except update at one query (+6.6%) and query at five (+6.4%).
- **Batching Postgres output per event-loop turn** lifts db and single query by 10-13% (run 5). The update loss
  run 5 showed with batching is resolved: run 6 did not reproduce it, and flushing the `/updates` UPDATE at once
  instead of at turn end made no measurable difference.
- **Open gap: fortune against the Round 23 leaders.** 91-94% of xitca-web-barebone and 93-97% of may-minihttp in
  runs 8, 9 and 10 (3-9% behind), larger than their run-to-run spread; level with actix-http and h2o. db, which
  shares the whole request path except the 12-row result and the HTML render, is at parity, so the difference is
  most likely in that part (an inference, profiled in `tech_empower_improv.md`, T7).
- Everything ran on one laptop. Rankings within a run matter more than the absolute numbers.

## Setup

| | |
|---|---|
| Host | Apple Silicon Mac, 12 cores, 18 GB |
| Docker | Colima VM, **10 CPUs, 12 GB**. Server, Postgres and wrk share it (TechEmpower uses 3 separate machines) |
| Entries | `cexpress` / `cexpress-postgres`, `actix` / `actix-http`, `axum` / `axum-pg`, `h2o`, `fiber`; from run 8 also `may-minihttp`, `xitca-web`, `xitca-web-barebone` |
| Load | TFB defaults: 15 s per level; concurrency 16-512 (json, db, fortune), 512 (query, update, 1-20 queries), 256-16,384 with pipelining 16 (plaintext) |

**How the numbers are computed.** req/s = `totalRequests / 15` for each level of `results.json`, and a table cell is
the best level. (The first version of this report divided runs 1 and 2 by `endTime - startTime`; those timestamps
are whole seconds, so a 15 s level sometimes counted as 14 s and came out 7% high. Every number here uses `/ 15`.)
No test failed in any run and there were **zero non-2xx responses** (TFB records a `5xx` count only when wrk reports
non-2xx responses; no entry of any run has one). Every entry has some wrk socket timeouts at the higher
concurrency levels, in similar amounts; they come from sharing 10 cores between wrk, Postgres and the server.

What CExpress ran:

| Runs | Engine | TFB app (`cexpress-postgres`) |
|---|---|---|
| 1, 2 | `c_server` `bf91ef7`: one `write` per pipelined response, handlers can't wait on I/O | one **blocking** libpq connection per worker, 4 workers per core |
| 3, 4 | `c_server` `288068e`: coalesced pipelined writes, `res_defer` / `res_resume`, `app_watch_fd` | one **non-blocking, pipelined** libpq connection per worker, shared by its requests; 1 worker per core; libpq 16 |
| 5-9 | + `app_on_turn_end` | + one `PQflush` per event-loop turn; libpq 18.6 |
| 10 | same as 5-9 | + **one Sync per statement** (TFB rules compliance, below) |

**Runs 1-9 are not rules-compliant for multiple queries and updates.** Their app sent all of a request's SELECTs
with one Sync, so the numbers at 5-20 queries in those runs (and the 1.4-2.3× leads they showed) do not count.
Their db, fortune and single-query numbers are unaffected: those send one statement and one Sync per request in
every version.

## TFB rules compliance

The rules are TechEmpower's "Framework Tests Overview" wiki page
(`https://github.com/TechEmpower/FrameworkBenchmarks/wiki/Project-Information-Framework-Tests-Overview`; the TFB
checkout only links to it, from `toolset/benchmark/framework_test.py`).

**General requirement #7** (applies to every test):

> Except where noted, all database queries should be delivered to the database servers as-is and not coalesced or
> deduplicated at the database driver. In all cases where a database query is required, it is expected that the
> query will reach and execute on the database server. However, it is permissible to pipeline traffic between the
> application and database as a network-level optimization. That is, two or more separate queries may be delivered
> from the application to the database in a single transmission over the network, and likewise for query results
> from the database back to the application, as long as the queries themselves are executed separately on the
> database server and not combined into a single complex query or transaction. If any pipelining or network
> optimization is performed, then the execution behavior and semantics must match what they would be if the
> multiple SQL statements were executed without the optimization, i.e. in separate roundtrips. For example, it is
> forbidden to perform any optimization that could change the transactionality and/or error handling of the SQL
> statements. [...] If using PostgreSQL's extended query protocol, each query must be separated by a Sync message.

**Multiple queries, requirement 6:**

> This test is designed to exercise multiple queries with each resulting row selected individually. It is not
> acceptable to execute multiple SELECTs within a single complex query. It is not acceptable to retrieve all
> required rows using a `SELECT ... WHERE id IN (...)` clause. However, note General Requirement #7 which permits
> pipelining of network traffic between the application and database.

**Updates, requirements 5, 9 and 12:**

> Each row must be selected randomly using one query in the same fashion as the single database query test [...].
> As with the read-only multiple-query test type (#3 above), use of `IN` clauses or similar means to consolidate
> multiple queries into one operation is not permitted. Similarly, use of a batch or multiple SELECTs within a
> single statement are not permitted.

> Using bulk updates—batches of update statements or individual update statements that affect multiple rows—is
> acceptable but not required. To be clear: bulk reads are not permissible for selecting/reading the rows, but bulk
> updates are acceptable for writing the updates.

> For raw tests (that is, tests without an ORM), each updated row must receive a unique new `randomNumber` value.
> It is not acceptable to change the `randomNumber` value of all rows to the same random number using an
> `UPDATE ... WHERE id IN (...)` clause.

**How the app meets them** (`Rest_in_c/techempower/cexpress/src/db.c`, from run 10 on):

- **Each query is its own statement.** Two statements are prepared once per worker: `world` = `SELECT id,
  randomnumber FROM world WHERE id = $1` and `fortune` = `SELECT id, message FROM fortune`. `/db` executes `world`
  once, `/queries?queries=n` and the read phase of `/updates` execute it n times with n separate ids
  (`send_selects`), and `/fortunes` executes `fortune` once. There is no `IN` clause, no multi-row SELECT, no
  `UNION`, and no batch SELECT anywhere; every row a request reads comes from its own execution of `world`.
- **Each statement is followed by its own Sync.** `send_selects` calls `PQsendQueryPrepared` then `pipeline_sync`
  (`PQsendPipelineSync`, libpq 17+) for every id, so each SELECT runs in its own implicit transaction and an error
  in one does not skip the others: the execution semantics of separate round trips. `DbJob.syncs_left` counts the
  Syncs a request still waits for; the request is answered when the last arrives.
- **Updates.** Once all of a request's SELECTs are back, each row gets a new random number that differs from its
  old one, and one `UPDATE world SET randomnumber = v.r FROM (VALUES ($1,$2), ...) AS v(id, r) WHERE world.id = v.id`
  writes all n rows, each with its own value (the "individual update statements that affect multiple rows" that
  requirement 9 allows). It is prepared once per row count and worker; that PREPARE also gets its own Sync.
- **What is shared across requests.** One libpq connection per worker process, in pipeline mode, carries the
  statements of every request on that worker. Statements are queued in libpq's output buffer as handlers run, and
  one `PQflush` per event-loop turn (the `app_on_turn_end` hook) sends what that turn queued in one write. This is
  the network-level pipelining that requirement #7 permits: no two requests' statements share a Sync, a
  transaction or a statement, and nothing is coalesced, deduplicated or cached. Results are matched back to
  requests in order (a FIFO of jobs).
- TFB's verifier counts executed statements and rows with `pg_stat_statements` and cannot see Sync placement; runs
  1-9 passed verification while sending one Sync per request. Run 10's verify (`20260927113514`): db, query, update
  and fortune all pass.

## History

### Runs 1 and 2: the first measurement (2026-09-25/26)

`20260926010854` and `20260926023941`, all six tests, eight entries. Engine `bf91ef7`, blocking libpq with 4 workers
per core.

JSON, CExpress 1st in both (and in run 4, below):

| Framework | Run 1 | Run 2 | Run 4 |
|---|---:|---:|---:|
| **cexpress** | **764,242** | **764,248** | **787,754** |
| actix | 719,879 | 744,592 | 738,709 |
| axum | 692,841 | 708,224 | 727,775 |
| h2o | 638,607 | 706,159 | 699,260 |
| fiber | 579,567 | 581,602 | 590,756 |

Plaintext (pipelined ×16), CExpress 4th in both, at 34-37% of Actix:

| Framework | Run 1 | Run 2 |
|---|---:|---:|
| actix | 4,273,428 | 4,623,852 |
| axum | 3,421,789 | 3,676,240 |
| fiber | 3,356,678 | 3,561,613 |
| **cexpress** | **1,587,520** | **1,597,938** |
| h2o | 1,496,581 | 1,575,258 |

The database tests, CExpress 4th on each, at 46-60% of the leader:

| Test | cexpress run 1 / 2 | leader run 1 | leader run 2 |
|---|---:|---|---|
| db | 155,174 / 171,268 | h2o 298,922 | actix-http 315,319 |
| query, 1 query | 152,222 / 169,539 | actix-http 284,788 | actix-http 310,681 |
| fortune | 163,313 / 180,969 | h2o 275,478 | h2o 301,100 |
| update, 1 query | 70,005 / 72,696 | actix-http 142,456 | actix-http 157,432 |

Two causes, both found in the code (`tech_empower_improv.md`, T1 and T2): each pipelined response went out in its
own `write`, and a worker blocked on every query.

### Worker-count sweep (blocking libpq, 2026-09-26)

`20260926040724` (2×), `20260926041845` (8×), `20260926043010` (16×), cexpress-postgres alone, one run each. The 16×
setting asked for 160 workers and got 128 (`MAX_CLUSTER_WORKERS`, `lib/cluster.h`).

| Workers per core | db | query, 1 query | fortune | update, 1 query |
|---|---:|---:|---:|---:|
| 2× | 202,786 | **206,326** | 196,881 | 65,227 |
| 4× (runs 1/2, avg) | 163,221 | 160,881 | 172,141 | 71,351 |
| 8× | 162,087 | 140,014 | 169,579 | **86,794** |
| 16× (128 workers) | 180,131 | 143,255 | **197,681** | 83,613 |

No setting got past ~70% of the leaders: with blocking queries, workers stood in for concurrency.

### Plaintext-only run after coalesced writes (2026-09-26)

`20260926105312`, engine `3d804c9` (a pipelined batch's responses gathered and written once). CExpress 1st at every
pipelined level: 4,894,116 / 4,910,252 / 4,095,808 / 3,361,283 req/s at 256 / 1,024 / 4,096 / 16,384 connections,
against actix 4,689,204 / 4,588,299 / 3,865,569 / 3,119,534. Best level: cexpress 4,910,252, actix 4,689,204, axum
3,685,342, fiber 3,647,948, h2o 1,615,637. Run 4 (below) repeated it: cexpress 4,813,888 against actix 4,412,559.

### Run 3: non-blocking libpq (2026-09-26)

`20260926132736`, DB tests, five entries. Engine `288068e` (`res_defer` / `res_resume`, `app_watch_fd`); the app
puts libpq's socket in the event loop, one pipelined connection per worker shared by its requests, 1 worker per
core. A head-to-head with actix-http alone just before (`20260926125325`, same code) gave db 294,803 vs 306,652,
fortune 280,285 vs 299,172.

| Test | cexpress | actix-http | axum-pg | h2o | fiber | cexpress place |
|---|---:|---:|---:|---:|---:|---|
| db | 297,671 | 307,632 | 302,879 | 301,387 | 131,994 | 4th, top four within 3.2% |
| query, 1 query | 301,411 | 299,320 | 293,978 | 297,611 | 124,136 | 1st, top four within 2.5% |
| fortune | 282,844 | 288,853 | 282,753 | 295,283 | 119,492 | 3rd, top four within 4.2% |
| update, 1 query | 158,507 | 151,049 | 143,699 | 126,403 | 68,507 | 1st |

CExpress went from 46-60% of the leader to 96% of it or 1st. At 16 connections actix-http was about 2× every other entry
(db 146k vs cexpress 79k, axum 71k, h2o 63k; fortune 169k vs 78k, 78k, 64k); from 32 connections on, the top four
tracked each other. The 5-20 query columns of this run are not compliant (see Setup).

### Run 4: repeat of run 3, all six tests (2026-09-26)

`20260926145131`, same code as run 3, eight entries.

| Test | cexpress | leader | cexpress place |
|---|---:|---|---|
| json | **787,754** | – | 1st (actix 738,709) |
| plaintext | **4,813,888** | – | 1st (actix 4,412,559) |
| db | 299,887 | h2o 311,737 (actix-http 311,464) | 4th, top four within 3.8% |
| query, 1 query | 302,805 | actix-http 309,852 | 3rd, within 3.8% |
| fortune | 275,448 | h2o 295,790 | 4th, 93% of h2o |
| update, 1 query | 157,191 | actix-http 157,190 | tied 1st |

CExpress's own numbers moved at most 2.8% from run 3; its places moved because competitors did. At 16 connections
actix-http (156k on db) and h2o (144k) were ahead of cexpress (76k) and axum-pg (77k).

### Run 5: batched Postgres flush and worker count, A/B (2026-09-26)

`20260926163928`, DB tests. Engine `288068e` + `app_on_turn_end`, libpq 18.6. Four entries in the same run:
`cexpress-postgres` (one `PQflush` per event-loop turn, 1 worker per core), `cexpress-postgres-flusheach` (same
binary, `CEXPRESS_PG_FLUSH_EACH=1`: flush after every request, the behavior of runs 3 and 4),
`cexpress-postgres-w2x` (batched, 2 workers per core), `actix-http`. Zero wrk timeouts.

| Test | batched, 1× | flush each, 1× | batched, 2× | actix-http | batched / flush each |
|---|---:|---:|---:|---:|---:|
| db | 321,196 | 291,353 | 305,382 | 322,366 | **1.10** |
| query, 1 query | 325,489 | 286,777 | 306,823 | 316,781 | **1.13** |
| fortune | 292,644 | 286,124 | 290,215 | 280,625 | 1.02 |
| update, 1 query | 139,560 | 151,470 | 152,828 | 163,504 | 0.92 |

Batching lifted db and single query by 10-13%. The update loss (and 0.84× at 5 updates) was checked in run 6. One
worker per core beat two on db (+5%) and query (+6%), and the default stayed at one.

### Run 6: update loss re-checked, eager flush A/B (2026-09-26)

`20260926224646`, db and update only. The unchanged batched build, in the same position of the same test order as
run 5, and `cexpress-postgres-eager` (`CEXPRESS_PG_EAGER_CALLBACK=1`: queries queued while reading results, the
`/updates` UPDATE, flushed right after that read instead of at turn end).

| Test | batched | eager | actix-http | batched, run 5 |
|---|---:|---:|---:|---:|
| db (best level) | 320,914 | 326,795 | 303,456 | 321,196 |
| db, 16 connections | 84,045 | 73,992 | 144,583 | 81,726 |
| update, 1 query | 160,909 | 161,636 | 154,827 | 139,560 |

**Run 5's update loss did not reproduce**: the batched build did 160,909 at 1 update (run 5: 139,560), ahead of run
5's flush-each number (151,470) and of actix-http. Run 5's batched update rows were also its noisiest (latency stdev
18.7 ms at 5 queries, against 11.4 ms here), so the loss looks like a bad sample, not a cost of batching. The eager
flush changed updates by +0.5% to +2.8% (noise) and was not kept. On `/db` the eager variant runs exactly the
batched code, so its column is an A/A measure of noise: 2% apart at the best level, 12% at 16 connections.

### Run 7: the shipped build, all five original entries (2026-09-27)

`20260927005504`, DB tests. One `PQflush` per event-loop turn, one Sync per request (not compliant for query and
update at 5-20, see Setup), 1 worker per core, libpq 18.6.

| Test | cexpress | actix-http | axum-pg | h2o | fiber | cexpress place |
|---|---:|---:|---:|---:|---:|---|
| db | **330,950** | 328,594 | 309,173 | 300,771 | 129,869 | 1st, 100.7% of actix-http |
| query, 1 query | **327,943** | 320,815 | 300,067 | 283,701 | 121,838 | 1st, 102.2% of actix-http |
| fortune | 304,025 | **308,830** | 290,449 | 299,714 | 117,295 | 2nd, 98.4% of actix-http |
| update, 1 query | 164,863 | **167,707** | 143,616 | 133,174 | 67,149 | 2nd, 98.3% of actix-http |

Against run 4, CExpress moved from 4th to 1st on db (299,887 → 330,950) and from 4th to 2nd on fortune (275,448 →
304,025, ahead of h2o).

### Runs 8 and 9: against Round 23's leaders (2026-09-27)

`20260927062404` and `20260927075426`, back to back, same build as run 7, the eight entries of run 10. Round 23
(February 2025) is TFB's last official round; the project was archived on 2026-03-24. `may-minihttp` was Round 23's
#1 on db, query and fortune; `xitca-web` #2 on update; `xitca-web-barebone` is the archived repository's name for
the stripped variant (Round 23's `xitca-web-unrealistic`, #1 on update and #2-3 elsewhere). Average of the two runs:

| Test | cexpress | may-minihttp | xitca-web-barebone | actix-http | h2o | xitca-web | axum-pg | fiber | cexpress place |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| db | 324,083 | **330,599** | 328,840 | 322,961 | 310,653 | 307,675 | 306,846 | 129,556 | 3rd, 98.0% |
| query, 1 query | 323,541 | **331,166** | 330,956 | 316,828 | 305,889 | 305,218 | 296,401 | 123,442 | 3rd, 97.7% |
| fortune | 303,164 | 323,723 | **329,013** | 302,238 | 309,262 | 298,019 | 286,301 | 118,674 | 4th, 92.1% |
| update, 1 query | **161,220** | 150,376 | 143,409 | 159,005 | 145,274 | 148,537 | 144,648 | 68,248 | 1st, 1.01× |

cexpress was 5th then 2nd on db and 3rd both times on single query. Fortune: 93.1% / 91.2% of xitca-web-barebone
and 92.9% / 94.4% of may-minihttp. The 5-20 query and update rows of these runs (1.4-2.3× ahead) are not compliant
and are left out.

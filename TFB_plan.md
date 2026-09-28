# TFB plan: where CExpress stands and how to get it listed

Status as of 2026-09-27. Full numbers and method: [`techEmpV1.md`](techEmpV1.md); fixes and profiling:
[`tech_empower_improv.md`](tech_empower_improv.md).

> **TechEmpower Framework Benchmarks no longer accepts submissions.** TechEmpower announced the project's sunset
> ([issue #10932](https://github.com/TechEmpower/FrameworkBenchmarks/issues/10932)) and archived
> `TechEmpower/FrameworkBenchmarks` on 2026-03-24 (GitHub API: `archived: true`, last push 2026-03-24). Pull requests can
> no longer be opened against it, and the announcement mentions no further rounds. The harness
> still works locally, which is how every CExpress number below was produced.
>
> The active successor is [HttpArena](https://www.http-arena.com/) (`MDA2AV/HttpArena`, MIT, last push 2026-09-25,
> results updated 2026-09-25), described by the community as TFB's heir
> ([post](https://dev.to/kaliumhexacyanoferrat/techempower-framework-benchmarks-are-now-archived-whats-next-3l0a)).
> Section 2 covers both: what a TFB submission would have needed, and what an HttpArena submission needs.

## 1. What we already achieved

**A working, verified TFB entry.** `Rest_in_c/techempower/cexpress/` implements all six TFB tests in two entries:
`cexpress` (json, plaintext) and `cexpress-postgres` (db, query, fortune, update). TFB's own verifier passes every
test (latest: `20260927113514`; the pinned and unpinned builds `20260927133212` and `20260927133341`).

**Rules-compliant database access.** Every SQL statement is followed by its own Sync, as TFB general requirement #7
demands. Each row is read by its own `SELECT ... WHERE id = $1`, with no `IN` clauses or batched SELECTs. Runs 1-9 had
shared one Sync per request; that was fixed on 2026-09-27 and re-measured in run 10.

**Engine work driven by TFB** (all in `lib/`, all tested):

| Change | Effect (measured in TFB runs) |
|---|---|
| Pipelined responses gathered and written once | plaintext 1.60M → 4.81-4.91M req/s, 5th/4th → 1st |
| `res_defer` / `res_resume` + `app_watch_fd`: handlers can wait on I/O without blocking the worker | db 155-171k → ~300k req/s |
| Non-blocking, pipelined libpq connection per worker, shared by its requests | DB tests from 46-60% of the leader to parity |
| `app_on_turn_end`: one Postgres flush per event-loop turn | db and single query +10-13% (run 5) |

**Where it lands** (run 10, eight entries including Round 23's database leaders): 1st on updates and multiple queries
at every count, tied on db and single query, 4th on fortune; 1st on JSON and plaintext in the last run that measured
them (run 4).

**Fortune gap profiled, first fix ready.** perf in the Linux VM showed CExpress and xitca-web-barebone spend the same
CPU per fortune request; CExpress leaves the VM more idle. Pinning each worker to one CPU gave +4.4% locally (three
alternating pairs). It is implemented as an A/B toggle (`CEXPRESS_PIN_WORKERS`, entry `cexpress-postgres-nopin`) and
has not yet been measured in a TFB run.

## 2. What needs to be done to submit

### 2a. TFB: not possible any more (for the record)

The repository is read-only. Had it been open, a pull request to `frameworks/C/cexpress/` would have needed:

1. The entry files TFB's wiki lists ("Codebase: Framework Files"): `benchmark_config.json` (with a `maintainers`
   list), `config.toml`, one `<name>-<test>.dockerfile` per entry, and a `README.md`. All exist today.
2. A self-contained build. Today `run.sh` copies the engine from `Rest_in_c/vendor/cexpress` into the entry at run
   time. A PR would instead fetch a tagged, public CExpress release in the dockerfile, and pin the libpq version
   exactly (the contributing guide asks for "specific versions" rather than whatever the package manager returns).
3. Dropping the local A/B variant (`cexpress-postgres-nopin`) and anything else not needed for the benchmark
   ("Related Source Code Only").
4. Passing verify in TFB's GitHub Actions on a fork, then a PR against `master`.

### 2b. HttpArena: the active option

HttpArena is organised differently from TFB: every entry is `frameworks/<name>/` with a `Dockerfile` and a
`meta.json`, and **profiles are opt-in** ("Only include profiles your framework supports"). Its database profiles
(`async-db`, `fortunes`) are shown but **reference-only**: they do not count toward the composite ranking. Most of the
scored profiles are plain HTTP/1.1, compression, TLS and latency/CPU-efficiency.

What CExpress can enter today, and what would need engine work:

| HttpArena profile | Endpoint | CExpress today |
|---|---|---|
| `baseline`, `limited-conn` | `/baseline11` (GET/POST, query parsing) | Ready: router + `req_get_query` |
| `pipelined` (reference-only) | `/pipeline` | Ready: coalesced pipelined writes |
| `latency-1m`, `latency-10k` | fixed-rate load, CPU per request scored | Likely ready; endpoint to confirm in HttpArena's profile docs |
| `async` | `/delay/{ms}`, 32K held connections | Needs a timer: `res_defer` plus a `timerfd` via `app_watch_fd`; not written yet |
| `async-db`, `fortunes` (reference-only) | Postgres via `DATABASE_URL` | Portable from the TFB app (`db.c`, pipelined libpq) |
| `api-4`, `api-16` | `/baseline11`, `/json/{count}`, `/async-db` | Ready once the above exist |
| `json-comp` | `Accept-Encoding: gzip, br` | **Not possible**: no response compression in the engine |
| `json-tls`, `8gbit`, `static-tls` | HTTP/1.1 over TLS | **Not possible**: TLS was removed from the engine on 2026-09-22 by design |
| `baseline-h2`, `-h2c`, `-h3`, gRPC, WebSocket | HTTP/2, HTTP/3, gRPC, WebSocket | **Not possible**: HTTP/1.1 only |

Steps:

1. Fork `MDA2AV/HttpArena`; add `frameworks/cexpress/` with a `Dockerfile` (binds port 8080; containers run with
   `--network host`), the handlers, and `meta.json`:
   `language: "C"`, `engine: "cexpress"`, a framework `type` (HttpArena tiers frameworks as `flagship`, `emerging`
   or `experimental` by how production-proven they are; CExpress is new, so likely `experimental`), `mode:
   "standard"`, and `tests` listing only the supported profiles.
2. Follow the Standard-mode rules: use the framework's documented APIs (router, query parsing, yyjson), and only
   production-documented settings. Worker count per core is explicitly allowed. **CPU pinning is not, unless it is
   documented as a production setting**, so either document it in CExpress (ideally as a `ServerConfig` option) or
   submit it as a separate `tuned` entry.
3. Implement the `/delay/{ms}` endpoint for `async` (timer plus `res_defer`).
4. Run `./scripts/validate.sh cexpress` and `./scripts/benchmark.sh cexpress` locally, then open the PR. Maintainers
   benchmark PRs on the reference machine with `/benchmark -f cexpress`.
5. Later, if wanted: response compression and a TLS story (HttpArena's TLS profiles terminate TLS in the server
   container, so a proxy in front would not be the same entry).

### 2c. Keep using the TFB harness locally

It remains the best yardstick for the DB-heavy work. Next runs:

1. `cexpress-postgres` vs `cexpress-postgres-nopin` in the same run (all four DB tests) to confirm pinning in TFB.
2. Re-run json and plaintext; they were last measured in run 4.
3. If fortune still trails: the faster render (tracker T7, candidate 2, estimated up to +2.5%).

## 3. Current results and how they differ from an official TFB run

Run 10 (`20260927113740`, 2026-09-27) for the database tests; run 4 (`20260926145131`) for JSON and plaintext. Best
req/s over the concurrency levels (for query and update: at the query count named).

| Test | CExpress | Best other | CExpress place | Run |
|---|---:|---|---|---|
| JSON | 787,754 | actix 738,709 | **1st** of 5 | 4 |
| Plaintext (pipelined ×16) | 4,813,888 | actix 4,412,559 | **1st** of 5 | 4 |
| Single query (db) | 328,331 | xitca-web-barebone 332,258 | 2nd of 8 (98.8%) | 10 |
| Multiple queries, 1 | 329,366 | may-minihttp 328,412 | **1st** of 8 | 10 |
| Multiple queries, 20 | 29,713 | actix-http 29,582 | **1st** of 8 (+0.4%) | 10 |
| Fortunes | 310,625 | xitca-web-barebone 329,595 | 4th of 8 (94.2%) | 10 |
| Updates, 1 | 165,029 | actix-http 154,826 | **1st** of 8 (+6.6%) | 10 |
| Updates, 20 | 20,641 | xitca-web 20,107 | **1st** of 8 (+2.7%) | 10 |

How these were produced, against how TFB runs an official round (TFB wiki, "Project Information: Environment"):

| | Our runs | Official TFB (Citrine) | Effect on the numbers |
|---|---|---|---|
| Harness | TFB's own `./tfb` toolset, unmodified | Same | None |
| Load generator and test mix | TFB's wrk image, 15 s per level, TFB's concurrency levels and headers | Same | None |
| Database | TFB's Postgres image and data | Same | None |
| Verification | TFB verify, all PASS | Same, required to be benchmarked | None |
| Machines | **One**: server, Postgres and wrk share a Colima VM (10 vCPUs, 12 GB) on an Apple Silicon laptop | **Three** separate machines: server, database, load generator | Our absolute req/s are far lower. wrk (~6.5 µs of CPU per request) and Postgres (~10 µs) compete with the server for the same cores, so a server that uses less CPU leaves more for the others and the ranking partly measures that |
| Hardware | Apple Silicon (arm64), 10 vCPUs | 3 × Intel Xeon Gold 5120, 28 HT cores, 32 GB each | Different architecture and core count; per-core behaviour does not carry over directly |
| Network | Docker bridge inside one VM | Switched 10-gigabit Ethernet | No real network latency or NIC limits here; our runs are CPU-bound (the VM was ~91% busy in the fortune profile) |
| Kernel tuning | Colima defaults | `somaxconn` and `tcp_max_syn_backlog` 65535, `tcp_tw_reuse` 1 | Can affect the highest concurrency levels; not measured |
| Competitors | Built from TFB's archived code in the same run; xitca-web patched to fetch git dependencies over https (build fix only) | Each framework's merged code at round time | Same code, same run, so the ranking is fair within a run |
| Repetitions | One run per configuration; ~2% A/A noise at the best level, up to 12% at 16 connections | Not stated in the environment docs | Differences of a few percent are inside noise here |
| CPU pinning | Not in runs 1-10; implemented as a toggle after run 10 | Up to each entry | xitca-web-barebone pins its threads; CExpress's first TFB-measured pinning result is still pending |

In short: the method (harness, load, database, verification) is TFB's own, so the **ranking within a run** is a fair
comparison. The **absolute numbers** are not comparable with TechEmpower's published rounds, because everything
shares one laptop's VM instead of three dedicated 28-core machines on 10 GbE.

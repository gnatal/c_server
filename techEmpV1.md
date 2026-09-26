# TechEmpower (TFB) Benchmark: CExpress vs Actix, Axum, h2o, Fiber

All six TFB test types, run by TechEmpower's own harness (`./techempower/run.sh`): their Postgres image,
their wrk load generator, their request mix and concurrency levels. Every entry passes TFB verification.

## Setup

| | |
|---|---|
| Host | Apple Silicon Mac, 12 cores, 18 GB |
| Docker | Colima VM, **10 CPUs, 12 GB**. Server, Postgres and wrk share it (TechEmpower uses 3 separate machines) |
| Entries | `cexpress` / `cexpress-postgres` (4 workers per core), `actix` / `actix-http`, `axum` / `axum-pg`, `h2o`, `fiber` |
| Load | TFB defaults: 15 s per level; concurrency 16-512 (json, db, fortune), 512 (query, update, 20 queries), 256-16384 with pipelining 16 (plaintext) |
| Runs | 2 full runs (2026-09-25/26): results `20260926010854`, `20260926023941` under `techempower/.tfb/results/` |

Numbers are the best req/s across concurrency levels. No test failed, and there were **zero non-2xx responses** in any
run. Every entry has some wrk socket timeouts at the highest concurrency levels in similar amounts, which comes from
sharing 10 cores between wrk, Postgres and the server.

## Results (req/s, run 1 / run 2)

### JSON serialization: CExpress 1st
| Framework | Run 1 | Run 2 |
|---|---:|---:|
| **cexpress** | **764,242** | **771,136** |
| actix | 719,879 | 752,766 |
| axum | 692,841 | 708,224 |
| h2o | 684,221 | 706,159 |
| fiber | 620,965 | 581,602 |

### Plaintext (pipelined): CExpress 5th, ~35-40% of Actix
| Framework | Run 1 | Run 2 |
|---|---:|---:|
| actix | 4,273,428 | 4,623,852 |
| axum | 3,421,789 | 3,938,829 |
| fiber | 3,356,678 | 3,561,613 |
| h2o | 1,587,542 | 1,656,091 |
| **cexpress** | **1,587,520** | **1,597,938** |

### Single query (db): CExpress 4th, ~55% of the leader
| Framework | Run 1 | Run 2 |
|---|---:|---:|
| actix-http | 289,136 | 337,842 |
| h2o | 298,922 | 308,487 |
| axum-pg | 285,426 | 293,864 |
| **cexpress-postgres** | **155,174** | **177,588** |
| fiber | 132,298 | 126,166 |

### Multiple queries (query): CExpress 4th, ~55%
| Framework | Run 1 | Run 2 |
|---|---:|---:|
| actix-http | 284,788 | 310,681 |
| axum-pg | 304,269 | 294,780 |
| h2o | 268,648 | 296,421 |
| **cexpress-postgres** | **152,222** | **169,539** |
| fiber | 109,526 | 119,204 |

### Fortunes: CExpress 4th, ~60%
| Framework | Run 1 | Run 2 |
|---|---:|---:|
| h2o | 275,478 | 301,100 |
| actix-http | 294,812 | 295,137 |
| axum-pg | 294,532 | 279,486 |
| **cexpress-postgres** | **170,498** | **180,969** |
| fiber | 113,815 | 117,632 |

### Updates: CExpress 4th, ~50%
| Framework | Run 1 | Run 2 |
|---|---:|---:|
| actix-http | 142,456 | 168,677 |
| axum-pg | 135,391 | 142,293 |
| h2o | 118,694 | 139,556 |
| **cexpress-postgres** | **75,005** | **77,888** |
| fiber | 63,222 | 68,221 |

## Worker-count sweep (cexpress-postgres, one run each)

| Workers per core | db | query | fortune | update |
|---|---:|---:|---:|---:|
| 2x | **216,946** | **206,326** | 196,882 | 65,227 |
| 4x (current, avg of 2 runs) | 166,381 | 160,881 | 175,734 | 76,447 |
| 8x | 162,088 | 140,014 | 172,581 | **86,795** |
| 16x | 180,131 | 143,256 | **197,681** | 83,613 |

Fewer workers help the read tests, and more workers help updates. No setting gets past ~70% of the leaders,
so the Dockerfile stays at 4x.

## What the numbers say

- **The engine is competitive on the request path.** CExpress wins JSON in both runs.
- **Plaintext is an engine issue: pipelined responses are written one syscall each.** `serve_buffered_requests`
(`vendor/cexpress/lib/connection.c`) calls `flush_connection` once per request, so a batch of 16 pipelined
requests costs 16 `write`s. Actix, Axum and Fiber gather the batch's responses and write them once. Fix: let responses
accumulate in `out_buf` across the pipelined batch and flush once at the end (or when the buffer fills).
- **DB tests are limited by blocking libpq.** A worker waits on each query, and changing the worker count doesn't fix it.
CExpress beats Fiber but runs at 50-70% of Actix/Axum/h2o. The fix is the one in `techempower.md` §2: put libpq's socket
into the event loop (non-blocking `PQsendQueryParams` + pipeline mode), so a worker serves other requests during a query.
- Everything ran on one laptop. Rankings within a run matter more than the absolute numbers.
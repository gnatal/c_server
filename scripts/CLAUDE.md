# scripts/ — performance & benchmarking tools

## Architecture
Utility and load-testing scripts used to benchmark latency, throughput, and memory stability of the server under high concurrency:
- `wrk_echo_json.lua`: Lua request generator script for `wrk` benchmarking against the `POST /echo/json` endpoint.

## Execution & Concurrency
- Designed to test keep-alive connection reuse and JSON payload processing under high concurrent connection loads (e.g. 5,000 connections over 8 threads).
- Benchmarking should run against a release-optimized build with keep-alive active to verify zero TCP round-trip latency anomalies (avoiding Nagle/delayed ACK interaction).

- Commands:
wrk -t8 -c5000 -d15s http://127.0.0.1:8080/
wrk -t8 -c1000 -d15s http://127.0.0.1:8080/
wrk -t8 -c100 -d15s http://127.0.0.1:8080/
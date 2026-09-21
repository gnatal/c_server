# Project: CExpress HTTP Server

## What is this project?
This project is an attempt to create an HTTP server with a developer experience similar to Express.js, but written entirely in C. We are choosing C for maximum performance, minimal footprint, and fine-grained control over memory and networking.

## Workflow & Context Maintenance
- **Continuous Documentation:** After completing a task or significant update within any folder, you must automatically summarize the technical findings and update/create the nested `CLAUDE.md` file for that specific directory.
- **Strictly Technical Findings:** When updating folder-level `CLAUDE.md` files, document *only* architectural decisions, state management, data flow, and memory lifecycle rules. Never write coding styles, formatting guidelines, or developer preferences in these local files.

## Coding Standards & Patterns
- **Type Definitions:** All `typedef` and `struct` definitions must live in dedicated header files (e.g., `appTypes.h`). Do not declare them in `.c` files.
- **File Size Limits:** No file may exceed 1,000 lines of code. If a file approaches this limit, proactively refactor and split the logic into two files.
- **Const Correctness:** Apply `const` aggressively to variables and pointer arguments to simulate immutability wherever possible.
- **Isolate Side Effects:** Separate I/O operations (reading/writing to sockets) from data processing. HTTP parsing functions should be pure, testable, and take `const char*` buffers as input.
- **Write Tests for all new code:** Every new function or behavior gets a test in `tests/` (plain `assert`, no framework). Test pure logic against buffers and a fake `Connection` (see `tests/test_cookbook.c`); use `socketpair(2)` only for socket-layer tests. Run `make test` before finishing.

## Security & Memory Considerations
- **Ban Unsafe Functions:** Never use `strcpy`, `strcat`, `sprintf`, or `gets`. Always use their bounded equivalents (`strncpy`, `strncat`, `snprintf`).
- **Deny by Default:** Network routers and file servers must use a deny-by-default policy. Return 403/404 unless a route or resource is explicitly matched.
- **Strict I/O Limits:** Always validate and bound user input. Enforce hard limits on HTTP header sizes (e.g., max 8KB) and payload bodies to prevent buffer overflows and DOS attacks.
- **Memory Management:** Every `malloc` or `calloc` must have a clearly documented and matching `free`.

## Where to start (for coding agents)
- Writing an app on the engine: `lib/API.md` (every public function, one line each) then `lib/examples/cookbook.c` (tested recipes: JSON in/out, path/query params, middleware, sub-routers, cookies, forms, uploads, streaming, per-worker resources). `examples/todo_sqlite/` is a full worked application.
- Changing the engine: `lib/CLAUDE.md` (request lifecycle, limits, ownership table, return conventions, hot-path rules, known gaps), then the header of the module you touch.
- Verify with `make test`, `make SANITIZE=1 BUILD_DIR=build-asan test`, `make fuzz`, `make bench`, `make check-docs`.
- Why the engine is built the way it is (arena allocator, yyjson, picohttpparser, Patricia router, io_uring): `tradeoffs.md`. The demo builds with `make demo` and must be started from `examples/todo_sqlite/` (it loads `public/` and `todos.db` relative to the working directory).

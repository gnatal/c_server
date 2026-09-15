# Project: C-Express HTTP Server

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
- **Write Tests for all new code:** Separate I/O operations (reading/writing to sockets) from data processing. HTTP parsing functions should be pure, testable, and take `const char*` buffers as input.

## Security & Memory Considerations
- **Ban Unsafe Functions:** Never use `strcpy`, `strcat`, `sprintf`, or `gets`. Always use their bounded equivalents (`strncpy`, `strncat`, `snprintf`).
- **Deny by Default:** Network routers and file servers must use a deny-by-default policy. Return 403/404 unless a route or resource is explicitly matched.
- **Strict I/O Limits:** Always validate and bound user input. Enforce hard limits on HTTP header sizes (e.g., max 8KB) and payload bodies to prevent buffer overflows and DOS attacks.
- **Memory Management:** Every `malloc` or `calloc` must have a clearly documented and matching `free`.
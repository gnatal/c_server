# CExpress Architecture Correction: Framework Decoupling & SQLite Isolation

> **Status:** PENDING  
> **Priority:** High  
> **Target:** Decouple CExpress core framework from the `/app` demo and SQLite dependencies to match the Express.js developer experience.

---

## 1. Executive Summary & Problem Statement

When attempting to import and vendor **CExpress** into external projects, a major architectural collision was identified:

1. **Top-Level Directory Collision (`/app`)**:  
   The repository currently contains a root-level `/app` directory hosting a complete Todo CRUD application (`app/main.c`, `app/handlers.c`, `app/db.c`, `app/middlewares.c`, etc.). When consumer projects (which frequently use `/app` or `app/` as their primary source folder) vendor or submodule CExpress, the name and file paths collide, creating build system ambiguities, include collisions, and tooling friction.

2. **Unwanted SQLite Demonstration Coupling**:  
   The core HTTP engine (`lib/`) is 100% independent of SQLite, but the root `Makefile` unconditionally inspects and links SQLite (`-lsqlite3`, `SQLITE_CFLAGS`, `SQLITE_LDFLAGS`). Furthermore, the default build target (`make` / `make all`) compiles the SQLite Todo application binary rather than a standalone framework library.

3. **Deviation from the Express.js Philosophy**:  
   Just like Express in the Node.js ecosystem (`npm install express`), developers import CExpress expecting an **unopinionated foundational HTTP framework** to build their own systems on top of. Express does not bundle a pre-baked database or sample application into consumer builds; it provides router primitives, middleware pipelines, request/response abstractions, and leaves database/business architecture entirely to the user.

---

## 2. Root Cause Breakdown

| Issue Area | Current State in CExpress | Expected Behavior (Express-like) |
| :--- | :--- | :--- |
| **Directory Structure** | Top-level `/app` contains a full Todo application with SQLite persistence. | The core repository root should only contain framework source (`lib/`), tests (`tests/`), tooling (`scripts/`), documentation (`docs/`), and optional isolated examples (`examples/`). |
| **Build Target** | `make all` builds `build/bin/cexpress` using `app/*.c`. | `make all` (or `make lib`) should build only `build/lib/libcexpress.a`. Demos should live under dedicated optional targets (`make demo`). |
| **Dependencies** | SQLite3 is checked globally and added to `CFLAGS` and `LDFLAGS` in the root `Makefile`. | The framework requires **zero** database dependencies. SQLite should only be checked/linked if building the optional Todo example. |
| **Test Suite Coupling** | `tests/test_ping.c` links directly against `$(OBJ_DIR)/app/ping.o`. | Engine tests must only depend on `lib/` components and self-contained test helpers. |
| **Consumer Vendoring** | Consumers importing the repo get the whole demo app, database files (`todos.db`), and risk directory collisions. | Consumers submodule or vendor CExpress cleanly, include `<cexpress.h>`, link `libcexpress.a`, and build their own application inside their own `/app` directory. |

---

## 3. Required Architectural Corrections

### A. Relocate and Isolate the Demo Application
- **Action**: Move the entire contents of `/app/` into `examples/todo_sqlite/` (or `examples/todo_app/`):
  ```text
  c_server/
  ├── lib/                      # Core framework (unchanged, zero DB dependencies)
  │   ├── cexpress.h            # Master public header
  │   ├── router.c / .h
  │   ├── response.c / .h
  │   └── ...
  ├── examples/
  │   └── todo_sqlite/          # Relocated demo application
  │       ├── main.c
  │       ├── db.c / db.h
  │       ├── handlers.c / handlers.h
  │       ├── middlewares.c / middlewares.h
  │       ├── public/
  │       │   └── index.html
  │       └── Makefile          # Dedicated Makefile for the demo
  ├── tests/                    # Core engine test suite
  ├── scripts/
  └── Makefile                  # Framework-first root Makefile
  ```
- **Result**: The root level of CExpress will no longer have an `/app` directory, completely eliminating path and namespace collisions with consuming projects.

### B. Clean the Root `Makefile`
- **Default Target**: Change default target `all` to build the static library:
  ```makefile
  all: lib
  ```
- **Remove Global SQLite Requirements**:
  - Strip `SQLITE_CFLAGS` and `SQLITE_LDFLAGS` from the primary `CFLAGS` and `LDFLAGS`.
  - Compile `libcexpress.a` strictly with platform flags and optional OpenSSL flags.
- **Optional Demo Targets**:
  - Add `demo` or `examples` target that explicitly navigates to `examples/todo_sqlite/` and builds the executable with SQLite flags enabled.

### C. Decouple Framework Tests from Demo Code
- **Fix `test_ping`**:
  - `tests/test_ping.c` currently depends on `app/ping.c`.
  - Move the lightweight ping handler directly into `tests/test_ping.c` or create a minimal fixture under `tests/fixtures/ping_handler.c`.
  - Ensure running `make test` requires **no** external database headers, libraries, or example source objects.

### D. First-Class Consumer Integration ("Create on Top of CExpress")
A developer importing CExpress into their own application must be able to do:

```c
/* my_project/app/main.c (Consumer's own app directory) */
#include <cexpress.h>
#include <stdio.h>

static void handle_hello(const Request *req, Response *res) {
    (void)req;
    res_send(res, "Hello from my custom application!\n");
}

int main(void) {
    App app;
    app_init(&app);
    app_get(&app, "/", handle_hello);
    
    printf("Starting custom app on :8080...\n");
    app_listen(&app, 8080);
    app_destroy(&app);
    return 0;
}
```

With a consumer `Makefile` that simply points to the vendored framework:
```makefile
CFLAGS += -Ivendor/cexpress/lib
LDLIBS += vendor/cexpress/build/lib/libcexpress.a -lssl -lcrypto
```
No SQLite flags, no `/app` collisions, and zero unwanted demo code.

---

## 4. Implementation Checklist

- [ ] **1. Directory Restructuring**
  - [ ] Create `examples/todo_sqlite/`.
  - [ ] Move `app/*` into `examples/todo_sqlite/`.
  - [ ] Remove root `/app` folder.
  - [ ] Move `todos.db` to `examples/todo_sqlite/` or add to `.gitignore`.

- [ ] **2. Test Decoupling**
  - [ ] Refactor `tests/test_ping.c` to define its own handler without referencing `app/ping.c`.
  - [ ] Remove `$(OBJ_DIR)/app/ping.o` from `PING_TEST_BIN` rule in root `Makefile`.

- [ ] **3. Root `Makefile` Refactoring**
  - [ ] Remove `SQLITE_PREFIX`, `SQLITE_CFLAGS`, and `SQLITE_LDFLAGS` from default build pipeline.
  - [ ] Set `all: $(LIB)` as default.
  - [ ] Create `demo` target:
    ```makefile
    demo: $(LIB)
    	$(MAKE) -C examples/todo_sqlite
    ```
  - [ ] Ensure `clean` target removes engine artifacts and cascades clean to `examples/todo_sqlite`.

- [ ] **4. Standalone Example Build**
  - [ ] Add `examples/todo_sqlite/Makefile` that links against `../../build/lib/libcexpress.a` and manages SQLite flags locally.
  - [ ] Update static asset paths in `examples/todo_sqlite/main.c` (e.g., pointing to `examples/todo_sqlite/public`).

- [ ] **5. Documentation & Script Updates**
  - [ ] Update `README.md` to present CExpress as a pure web framework with a dedicated section on examples.
  - [ ] Update `importing.md` to reflect that the repository can be cleanly submoduled without any directory collisions.
  - [ ] Update `CLAUDE.md` and remove references to root `/app/`.
  - [ ] Update `Dockerfile` to build and run from `examples/todo_sqlite/` (or create a dedicated framework development container).
  - [ ] Update `scripts/stress_test.sh` to target the executable built in `examples/todo_sqlite/`.

---

## 5. Acceptance Criteria

1. **Clean Namespace**: When CExpress is cloned or submoduled into another project, there is no `app/` folder in the CExpress root to conflict with the host project.
2. **Zero SQLite Requirement for Framework**: A user can run `make` and `make test` on a machine with **no SQLite development headers or libraries installed**, and the library + test suite build and pass completely.
3. **Pure Express-like Consumption**: A consumer project can create its own `/app/main.c`, include `"cexpress.h"`, link `libcexpress.a`, and spin up a server with zero SQLite baggage.

---

## 6. Performance Upgrades (Future)

To push CExpress from its current simplicity-first design to advanced, state-of-the-art C performance, the following architectural upgrades should be considered:

1. **Replace the JSON Parser (simdjson / yyjson)**
   - **Current**: Custom parser using `malloc` and `memcpy` for every string (`json_value.c`).
   - **Upgrade**: Use [simdjson](https://simdjson.org/) or [yyjson](https://github.com/ibireme/yyjson) to leverage SIMD instructions. This allows gigabytes of JSON to be parsed per second, often parsing payloads in-place without per-key allocations.

2. **Implement an Arena Allocator (Bump Allocator)**
   - **Current**: Heavy reliance on `malloc`/`free` (`req->body`, `conn->out_buf`, strings).
   - **Upgrade**: Pre-allocate a fixed slab (e.g., 64KB) per connection. When parsing or building responses, simply bump a pointer forward. Reset pointer to 0 when the response is sent, dropping allocation and fragmentation overhead to effectively zero.

3. **Replace the HTTP Parser (`picohttpparser`)**
   - **Current**: Custom byte-by-byte parser (`lib/http_parser.c`) using `memchr`.
   - **Upgrade**: Integrate [picohttpparser](https://github.com/h2o/picohttpparser). It uses SIMD (AVX2/SSE4) to parse HTTP headers extremely quickly, serving as the backbone for some of the world's fastest web servers.

4. **Upgrade the Router to a Radix Trie**
   - **Current**: Route matching is likely linear O(N) iterating over registered routes in `lib/router.c`.
   - **Upgrade**: Implement a Radix Trie (Prefix Tree) similar to Go's `httprouter` or Fastify. This makes route matching O(k) (where k is path length), maintaining high performance even with thousands of routes.

5. **Adopt `io_uring` (Linux Only)**
   - **Current**: Uses `kqueue` (macOS) and `epoll` (Linux) via multi-process workers.
   - **Upgrade**: Replace `epoll` with Linux's `io_uring` to perform asynchronous I/O that bypasses standard syscall overhead, allowing massive concurrency with zero user-kernel context switches.

6. **Non-Blocking Database Drivers / Thread Pool**
   - **Current**: SQLite operations (e.g., in the demo) block the worker process entirely.
   - **Upgrade**: Use async/non-blocking drivers for databases (like `libpq` for PostgreSQL) within the event loop, or implement a `libuv`-style thread pool for strictly blocking I/O (like SQLite or file reads) so the main event loop never stalls.

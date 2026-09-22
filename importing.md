# Importing CExpress into Another Project

This guide explains how to import and use **CExpress** in a brand-new, standalone C project.

The recommended approach is **Vendoring via Git Submodule (or direct copy)** inside a `vendor/` directory. This makes your new project 100% self-contained, portable across machines, and **ideal for AI/LLM coding assistants** (like Claude, Cursor, and Antigravity) because all type definitions, API cheat sheets, and few-shot recipe files remain accessible directly in the project workspace.

The Makefile and `main.c` below were built and run as a consumer project against the current engine on macOS (the Linux branch of the Makefile was not run for this update).

---

## 1. Project Directory Layout

Here is the recommended structure for your new application:

```text
my_app/
├── vendor/
│   └── cexpress/          # Git submodule or copy of the c_server repository
├── public/                # (Optional) Static files (HTML, CSS, images)
│   └── index.html
├── AGENTS.md              # (Optional but recommended) AI context rules
├── Makefile               # Builds your app and auto-compiles CExpress
└── main.c                 # Your application code
```

---

## 2. Adding CExpress to Your Project

### Option A: As a Git Submodule (Recommended)
If your new project is a Git repository, add CExpress as a submodule:

```bash
mkdir -p vendor
git submodule add <URL_TO_CEXPRESS_REPO> vendor/cexpress
```

When cloning `my_app` on another machine later:
```bash
git clone --recursive <URL_TO_MY_APP>
```

### Option B: Direct Clone or Copy
Alternatively, you can clone or copy the repository directly into `vendor/cexpress`:

```bash
mkdir -p vendor
git clone <URL_TO_CEXPRESS_REPO> vendor/cexpress
```

### Option C: Export only the engine
From a checkout of the CExpress repository, `make export DEST=/path/to/my_app/vendor/cexpress` copies `lib/` (headers, sources, `API.md`, the cookbook and the vendored yyjson / picohttpparser) plus a small standalone `Makefile` that builds `build/lib/libcexpress.a`. The demo app, tests and scripts are left behind, so the layout in section 1 and the Makefile below work unchanged.

---

## 3. What the Build Needs

| Dependency | Needed for | Notes |
|---|---|---|
| A C11 compiler and GNU `make` | everything | The engine's own Makefile uses `gcc-16` on macOS (Homebrew) and `gcc` on Linux. |
| **liburing** (`liburing-dev`, `liburing` on Alpine) | Linux only | The Linux event loop is io_uring; link `-luring`. Needs a kernel with multishot poll (5.13+). A container runtime that blocks io_uring makes the server exit at startup (see `concurrency.md`). |
| yyjson, picohttpparser | always | Vendored inside `lib/vendor/`. Nothing to install. |
| SQLite | never | Only the demo app in `examples/todo_sqlite/` uses it. |

There is no TLS dependency: the engine is plaintext HTTP/1.1 only. If your app needs HTTPS, terminate TLS at a gateway or reverse proxy in front of it (nginx, an ALB, a sidecar).

---

## 4. The Consumer `Makefile`

Create a `Makefile` in the root of your new project. It compiles `libcexpress.a` on demand if it hasn't been built yet (through the engine's own Makefile), and links your application.

```makefile
# --- Compiler & Platform Detection ---
UNAME_S := $(shell uname -s)

# `make` always defines CC (default: cc), so `CC ?=` never applies; check where the value came from.
ifeq ($(UNAME_S),Linux)
    ifeq ($(origin CC),default)
        CC = gcc
    endif
    PLATFORM_CFLAGS = -D_GNU_SOURCE
    PLATFORM_LIBS   = -luring
else
    # macOS: Homebrew GCC (the engine's own Makefile uses gcc-16)
    ifeq ($(origin CC),default)
        CC = gcc-16
    endif
    PLATFORM_CFLAGS =
    PLATFORM_LIBS   =
endif

# --- CExpress Engine Path ---
CEXPRESS_DIR = vendor/cexpress
LIB_CEXPRESS = $(CEXPRESS_DIR)/build/lib/libcexpress.a

# --- Build Flags ---
CFLAGS = -Wall -Wextra -std=c11 -O2 -I$(CEXPRESS_DIR)/lib $(PLATFORM_CFLAGS)
LDLIBS = $(LIB_CEXPRESS) $(PLATFORM_LIBS)

TARGET = my_app

.PHONY: all run clean

all: $(TARGET)

# Compile the CExpress static library if it is not present (delegates to the engine's own Makefile)
$(LIB_CEXPRESS):
	$(MAKE) -C $(CEXPRESS_DIR) build/lib/libcexpress.a

# Compile and link the consumer application
$(TARGET): main.c $(LIB_CEXPRESS)
	$(CC) $(CFLAGS) -o $@ main.c $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
```

> [!NOTE]
> `make` always defines `CC` (default `cc`), so a plain `CC ?= gcc-16` never takes effect. The Makefile above checks `$(origin CC)` instead, so `make CC=clang` still overrides it.

> [!NOTE]
> **No SQLite dependency required!** The CExpress engine (`libcexpress.a`) has zero dependency on SQLite. You only need SQLite if your own application specifically uses it.

---

## 5. Starter `main.c` Application

Create `main.c` in your project root. Including `"cexpress.h"` exposes all routing, response, middleware and JSON (yyjson) capabilities:

```c
#include "cexpress.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

/* GET /: Plain text response */
static void handle_home(const Request *req, Response *res) {
    (void)req;
    res_send(res, "Welcome to my CExpress application!\n");
}

/* GET /square/:n: Path parameter parsing and JSON response */
static void handle_square(const Request *req, Response *res) {
    const char *n_str = req_get_param(req, "n");
    char *end = NULL;
    errno = 0;
    long n = strtol(n_str, &end, 10);

    if (n_str[0] == '\0' || *end != '\0' || errno != 0 || n < -1000000 || n > 1000000) {
        res_status(res, 400);
        res_send(res, "Parameter :n must be an integer between -1000000 and 1000000.\n");
        return;
    }

    /* Build the document over the per-connection arena: nothing to free but the serialized text. */
    yyjson_alc alc = arena_yyjson_alc(&res->conn->arena);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&alc);
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, obj);
    yyjson_mut_obj_add_int(doc, obj, "input", n);
    yyjson_mut_obj_add_int(doc, obj, "square", n * n);

    char *json = yyjson_mut_write(doc, 0, NULL); /* libc-malloc'd: always free() it */
    if (json != NULL) {
        res_json(res, json); /* res_json copies the bytes */
        free(json);
    } else {
        res_status(res, 500);
        res_send(res, "JSON encoding failed\n");
    }
    yyjson_mut_doc_free(doc); /* a no-op for an arena document; harmless and future-proof */
}

int main(void) {
    App app;
    app_init(&app);

    /* 1. Register routes */
    app_get(&app, "/", handle_home);
    app_get(&app, "/square/:n", handle_square);

    /* 2. Optional: Serve static assets from ./public at /static/ */
    app_serve_static(&app, "/static", "./public");

    /* 3. Start event loop */
    int port = 8080;
    printf("Server running on http://localhost:%d\n", port);
    app_listen(&app, port);

    /* 4. Cleanup when the server stops (SIGINT / SIGTERM) */
    app_destroy(&app);
    return 0;
}
```

Compile and run (from the project root, because `./public` is resolved against the working directory):
```bash
make run
```
Test with `curl`:
```bash
curl http://localhost:8080/
# Output: Welcome to my CExpress application!

curl http://localhost:8080/square/7
# Output: {"input":7,"square":49}

curl -i http://localhost:8080/square/abc
# HTTP/1.1 400 Bad Request
```

### JSON in two rules
1. Build documents over the connection arena: `yyjson_alc alc = arena_yyjson_alc(&res->conn->arena);`. The arena is reset after the response is written, so nothing about the document needs freeing.
2. The text returned by `yyjson_mut_write` is allocated by libc, **not** the arena. `free()` it, or every request leaks one string.

Parsing a request body works the same way: `yyjson_read_opts(req->body, (size_t)req->content_length, 0, &alc, NULL)`. See `lib/examples/cookbook.c` (recipes 2, 4 and 5) and `lib/API.md` for the yyjson subset in use.

---

## 6. Optimal Setup for LLMs & AI Assistants

Because CExpress is a custom native C framework, public LLMs have no pre-trained memory of its function names or signatures.

Vendoring CExpress directly into `vendor/cexpress/` gives the LLM complete visibility over the code. To ensure coding agents (Cursor, Claude Code, Antigravity, Copilot) write idiomatic CExpress code without hallucinations, create an `AGENTS.md` (or `.cursorrules` / `CLAUDE.md`) in your project root:

```markdown
# CExpress Framework Guidelines

This project builds on top of the CExpress HTTP engine located in `vendor/cexpress/`.

## Key Resources to Read First:
1. `vendor/cexpress/lib/API.md`: 1-line index of every public function.
2. `vendor/cexpress/lib/examples/cookbook.c`: Tested recipes for path parameters, query strings, headers, JSON in and out, cookies, forms, uploads, streaming. Follow these patterns, and its DON'T list.
3. `vendor/cexpress/lib/app_types.h`: Core struct definitions (`Request`, `Response`, `App`) and compile-time limits.
4. `vendor/cexpress/lib/CLAUDE.md`: request lifecycle, memory model and ownership table, known gaps.

## Coding Rules:
- Include `"cexpress.h"` for all CExpress types and functions.
- Memory ownership:
  - DO NOT free `req->body` or strings returned by `req_get_*` (they live in the per-connection arena and die when the handler returns; copy what you need to keep).
  - DO NOT allocate `Response`; handlers receive a pointer and call `res_*` functions.
  - For JSON, use yyjson over the arena: `arena_yyjson_alc(&res->conn->arena)`. ALWAYS `free()` the string returned by `yyjson_mut_write` (it is libc-malloc'd).
  - Use the same `:name` for a path parameter at the same position in every route.
- Open databases and sockets in an `app_on_worker_start` hook, never before `app_listen` (workers are forked).
- Always pair `app_init(&app)` with `app_destroy(&app)`.
```

With this file in place, LLMs will consult `cookbook.c` and `API.md` directly, producing clean, tested, memory-safe C handlers on their first try.

---

## 7. Quick Cheat Sheet of Core Functions

| Task | Function Signature / Pattern | Header |
| :--- | :--- | :--- |
| **Initialize / Destroy App** | `app_init(App *app)`, `app_destroy(App *app)` | `router.h` / `connection.h` |
| **Start Server** | `app_listen(App *app, int port)` | `connection.h` |
| **Register Route** | `app_get(App *app, const char *path, Handler h)` (`app_post`, `app_put`, etc.) | `router.h` |
| **Sub-Routers** | `router_init(Router *r)`, `app_mount(App *app, "/prefix", Router *r)` | `router.h` |
| **Serve Static Directory** | `app_serve_static(App *app, "/prefix", "./dir")` | `router.h` |
| **Path Parameter** | `req_get_param(req, "id")` | `router.h` |
| **Query Parameter** | `req_get_query(req, "key")` | `http_parser.h` |
| **Header Values** | `req_get_header(req, "Content-Type")` | `http_parser.h` |
| **Send Text / HTML** | `res_send(res, "Hello")` | `response.h` |
| **Send JSON** | `res_json(res, json_string)` | `response.h` |
| **Build JSON** | `arena_yyjson_alc(&res->conn->arena)`, `yyjson_mut_doc_new(&alc)`, `yyjson_mut_obj_add_str(...)`, `yyjson_mut_write(doc, 0, NULL)` then `free` | `arena.h`, `vendor/yyjson/yyjson.h` |
| **Parse JSON body** | `yyjson_read_opts(req->body, len, 0, &alc, NULL)`, `yyjson_obj_get(root, "k")` | `vendor/yyjson/yyjson.h` |
| **Set Status Code** | `res_status(res, 404)` | `response.h` |
| **Set Cookie** | `res_set_cookie(res, "name", "val", &options)` | `response.h` |

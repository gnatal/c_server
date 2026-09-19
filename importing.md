# Importing CExpress into Another Project

This guide explains how to import and use **CExpress** in a brand-new, standalone C project. 

The recommended approach is **Vendoring via Git Submodule (or direct copy)** inside a `vendor/` directory. This makes your new project 100% self-contained, portable across machines, and **ideal for AI/LLM coding assistants** (like Claude, Cursor, and Antigravity) because all type definitions, API cheat sheets, and few-shot recipe files remain accessible directly in the project workspace.

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

---

## 3. The Consumer `Makefile`

Create a `Makefile` in the root of your new project. This Makefile automatically compiles `libcexpress.a` on demand if it hasn't been built yet, detects OpenSSL on both macOS and Linux, and links your application.

```makefile
# --- Compiler & Platform Detection ---
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Linux)
    CC ?= gcc
    PLATFORM_CFLAGS = -D_GNU_SOURCE
    OPENSSL_CFLAGS ?= $(shell pkg-config --cflags openssl 2>/dev/null)
    OPENSSL_LIBS   ?= $(shell pkg-config --libs openssl 2>/dev/null || echo "-lssl -lcrypto")
else
    # macOS: Homebrew GCC or Clang; Homebrew OpenSSL detection
    CC ?= gcc-16
    PLATFORM_CFLAGS =
    OPENSSL_PREFIX ?= $(shell brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null || echo /opt/homebrew/opt/openssl@3)
    ifeq ($(shell test -d $(OPENSSL_PREFIX)/include && echo yes),yes)
        OPENSSL_CFLAGS = -I$(OPENSSL_PREFIX)/include
        OPENSSL_LIBS   = -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto
    else
        OPENSSL_CFLAGS = $(shell pkg-config --cflags openssl 2>/dev/null)
        OPENSSL_LIBS   = $(shell pkg-config --libs openssl 2>/dev/null || echo "-lssl -lcrypto")
    endif
endif

# --- CExpress Engine Path ---
CEXPRESS_DIR = vendor/cexpress
LIB_CEXPRESS = $(CEXPRESS_DIR)/build/lib/libcexpress.a

# --- Build Flags ---
CFLAGS = -Wall -Wextra -std=c11 -O2 -I$(CEXPRESS_DIR)/lib $(PLATFORM_CFLAGS) $(OPENSSL_CFLAGS)
LDLIBS = $(LIB_CEXPRESS) $(OPENSSL_LIBS)

TARGET = my_app

.PHONY: all run clean

all: $(TARGET)

# Automatically compile the CExpress static library if not present
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
> **No SQLite dependency required!** The CExpress engine (`libcexpress.a`) has zero dependency on SQLite. You only need SQLite if your own application specifically uses it.

---

## 4. Starter `main.c` Application

Create `main.c` in your project root. Including `"cexpress.h"` exposes all routing, response, JSON, and middleware capabilities:

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

    if (n_str[0] == '\0' || *end != '\0' || errno != 0) {
        res_status(res, 400);
        res_send(res, "Parameter :n must be a valid integer.\n");
        return;
    }

    /* Fast, allocation-safe JSON serialization using JsonWriter */
    JsonWriter jw;
    jw_init(&jw);
    jw_object_begin(&jw);
    jw_key(&jw, "input");
    jw_int(&jw, n);
    jw_key(&jw, "square");
    jw_int(&jw, n * n);
    jw_object_end(&jw);

    res_json(res, jw_data(&jw));
    jw_free(&jw);
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

Compile and run:
```bash
make run
```
Test with `curl`:
```bash
curl http://localhost:8080/
# Output: Welcome to my CExpress application!

curl http://localhost:8080/square/7
# Output: {"input":7,"square":49}
```

---

## 5. Optimal Setup for LLMs & AI Assistants

Because CExpress is a custom native C framework, public LLMs have no pre-trained memory of its function names or signatures.

Vendoring CExpress directly into `vendor/cexpress/` gives the LLM complete visibility over the code. To ensure coding agents (Cursor, Claude Code, Antigravity, Copilot) write idiomatic CExpress code without hallucinations, create an `AGENTS.md` (or `.cursorrules` / `CLAUDE.md`) in your project root:

```markdown
# CExpress Framework Guidelines

This project builds on top of the CExpress HTTP engine located in `vendor/cexpress/`.

## Key Resources to Read First:
1. `vendor/cexpress/lib/API.md`: 1-line index of every public function.
2. `vendor/cexpress/lib/examples/cookbook.c`: Tested recipes for path parameters, query strings, headers, JSON serialization, and cookies. Follow these patterns.
3. `vendor/cexpress/lib/app_types.h`: Core struct definitions (`Request`, `Response`, `App`) and compile-time limits.

## Coding Rules:
- Include `"cexpress.h"` for all CExpress types and functions.
- Memory ownership:
  - DO NOT free `req->body` or strings returned by `req_get_*` (managed by the engine).
  - DO NOT allocate `Response`; handlers receive a pointer and call `res_*` functions.
  - For JSON output, prefer `JsonWriter` (`jw_init`, `jw_*`, `jw_free`) over node trees. Always call `jw_free(&jw)`.
- Always pair `app_init(&app)` with `app_destroy(&app)`.
```

With this file in place, LLMs will consult `cookbook.c` and `API.md` directly, producing clean, tested, memory-safe C handlers on their first try.

---

## 6. Quick Cheat Sheet of Core Functions

| Task | Function Signature / Pattern | Header |
| :--- | :--- | :--- |
| **Initialize / Destroy App** | `app_init(App *app)`, `app_destroy(App *app)` | `router.h` |
| **Start Server** | `app_listen(App *app, int port)` | `connection.h` |
| **Register Route** | `app_get(App *app, const char *path, Handler h)` (`app_post`, `app_put`, etc.) | `router.h` |
| **Sub-Routers** | `router_init(Router *r)`, `app_mount(App *app, "/prefix", Router *r)` | `router.h` |
| **Serve Static Directory** | `app_serve_static(App *app, "/prefix", "./dir")` | `static.h` |
| **Path Parameter** | `req_get_param(req, "id")` | `router.h` |
| **Query Parameter** | `req_get_query(req, "key")` | `http_parser.h` |
| **Header Values** | `req_get_header(req, "Content-Type")` | `http_parser.h` |
| **Send Text / HTML** | `res_send(res, "Hello")` | `response.h` |
| **Send JSON** | `res_json(res, json_string)` | `response.h` |
| **Fast JSON Emission** | `jw_init(&w)`, `jw_key(&w, "k")`, `jw_string(&w, "v")`, `jw_free(&w)` | `json/json.h` |
| **Set Status Code** | `res_status(res, 404)` | `response.h` |
| **Set Cookie** | `res_set_cookie(res, "name", "val", &options)` | `response.h` |
| **Enable TLS / HTTPS** | `app_enable_tls(App *app, "cert.pem", "key.pem")` | `tls.h` |

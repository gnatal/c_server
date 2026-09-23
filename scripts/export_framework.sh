#!/bin/bash
set -euo pipefail

# -----------------------------------------------------------------------------
# CExpress Framework Exporter
# Exports ONLY the CExpress engine (lib/) without the demo app (examples/todo_sqlite/) or SQLite code.
# -----------------------------------------------------------------------------

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <destination_directory>"
    exit 1
fi

DEST_DIR="$1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "==> Exporting CExpress framework to: ${DEST_DIR}"

mkdir -p "${DEST_DIR}/lib"

# Copy engine sources and headers
cp -R "${ROOT_DIR}/lib/"* "${DEST_DIR}/lib/"

# Write a clean, dedicated standalone Makefile inside the vendor directory
cat << 'MAKEFILE_EOF' > "${DEST_DIR}/Makefile"
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Linux)
    # Linux: io_uring (liburing) with a runtime fallback to epoll: link -luring when you link libcexpress.a.
    # NO_URING=1 builds epoll only and needs no liburing.
    ifeq ($(origin CC),default)
        CC = gcc
    endif
    ifeq ($(NO_URING),1)
        PLATFORM_CFLAGS = -D_GNU_SOURCE -DCEXPRESS_USE_EPOLL
        EVENT_LOOP_SRC = lib/event_loop_linux.c lib/event_loop_epoll.c
    else
        PLATFORM_CFLAGS = -D_GNU_SOURCE
        EVENT_LOOP_SRC = lib/event_loop_linux.c lib/event_loop_io_uring.c lib/event_loop_epoll.c
    endif
else
    ifneq ($(shell which gcc-16 2>/dev/null),)
        CC = gcc-16
    else
        CC = gcc
    endif
    PLATFORM_CFLAGS =
    EVENT_LOOP_SRC = lib/event_loop_kqueue.c
endif

CFLAGS = -Wall -Wextra -std=c11 -O2 -Ilib $(PLATFORM_CFLAGS)
AR = ar

BUILD_DIR = build
OBJ_DIR   = $(BUILD_DIR)/obj
LIB_DIR   = $(BUILD_DIR)/lib

LIB_SRCS = lib/connection.c $(EVENT_LOOP_SRC) lib/cluster.c lib/http_parser.c \
           lib/router.c lib/response.c lib/middleware.c lib/multipart.c lib/urlencoded.c \
           lib/static.c lib/arena.c \
           lib/vendor/yyjson/yyjson.c lib/vendor/picohttpparser/picohttpparser.c
LIB_OBJS = $(patsubst %.c, $(OBJ_DIR)/%.o, $(LIB_SRCS))
LIB      = $(LIB_DIR)/libcexpress.a

.PHONY: all clean

all: $(LIB)

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(shell find $(OBJ_DIR) -name '*.d' 2>/dev/null)

$(LIB): $(LIB_OBJS)
	@mkdir -p $(LIB_DIR)
	$(AR) rcs $@ $(LIB_OBJS)
	@echo "==> CExpress static library built: $@"

clean:
	rm -rf $(BUILD_DIR)
MAKEFILE_EOF

# Add a concise README to the exported directory
cat << 'README_EOF' > "${DEST_DIR}/README.md"
# CExpress Web Framework (Engine Only)

This directory contains the standalone CExpress engine without any application code or SQLite dependencies.

## Key Files
- `lib/cexpress.h`: Primary header to include.
- `lib/API.md`: Public API function reference.
- `lib/examples/cookbook.c`: Reference recipes for common HTTP patterns.
- `lib/vendor/`: vendored yyjson (JSON) and picohttpparser (HTTP parsing); nothing else to install.
- `build/lib/libcexpress.a`: Compiled engine archive built via `make`. Link it with `-luring` on Linux (not with `NO_URING=1`).
README_EOF

echo "==> CExpress framework successfully exported to ${DEST_DIR}"

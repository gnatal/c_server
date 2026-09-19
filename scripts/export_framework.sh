#!/bin/bash
set -euo pipefail

# -----------------------------------------------------------------------------
# CExpress Framework Exporter
# Exports ONLY the CExpress engine (lib/) without any demo app/ or SQLite code.
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
    CC ?= gcc
    PLATFORM_CFLAGS = -D_GNU_SOURCE
    EVENT_LOOP_SRC = lib/event_loop_epoll.c
    OPENSSL_CFLAGS ?= $(shell pkg-config --cflags openssl 2>/dev/null)
else
    ifneq ($(shell which gcc-16 2>/dev/null),)
        CC = gcc-16
    else
        CC = gcc
    endif
    PLATFORM_CFLAGS =
    EVENT_LOOP_SRC = lib/event_loop_kqueue.c
    OPENSSL_PREFIX ?= $(shell brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null || echo /opt/homebrew/opt/openssl@3)
    ifeq ($(shell test -d $(OPENSSL_PREFIX)/include/openssl && echo yes),yes)
        OPENSSL_CFLAGS = -I$(OPENSSL_PREFIX)/include -DCEXPRESS_HAS_TLS=1
    else ifeq ($(shell pkg-config --exists openssl 2>/dev/null && echo yes),yes)
        OPENSSL_CFLAGS = $(shell pkg-config --cflags openssl) -DCEXPRESS_HAS_TLS=1
    else
        OPENSSL_CFLAGS = -DCEXPRESS_HAS_TLS=0
    endif
endif

CFLAGS = -Wall -Wextra -std=c11 -O2 -Ilib $(PLATFORM_CFLAGS) $(OPENSSL_CFLAGS)
AR = ar

BUILD_DIR = build
OBJ_DIR   = $(BUILD_DIR)/obj
LIB_DIR   = $(BUILD_DIR)/lib

LIB_SRCS = lib/connection.c $(EVENT_LOOP_SRC) lib/cluster.c lib/tls.c lib/http_parser.c \
           lib/router.c lib/response.c lib/middleware.c lib/multipart.c lib/urlencoded.c \
           lib/static.c lib/json/json_parser.c lib/json/json_value.c lib/json/json_writer.c
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
- `build/lib/libcexpress.a`: Compiled engine archive built via `make`.
README_EOF

echo "==> CExpress framework successfully exported to ${DEST_DIR}"

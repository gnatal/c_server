UNAME_S := $(shell uname -s)

# Linux: io_uring and epoll are both built; event_loop_linux.c picks one at runtime (io_uring, falling
# back to epoll when the ring cannot be created). NO_URING=1 builds epoll only and drops the liburing dependency.
ifeq ($(UNAME_S),Linux)
    CC = gcc
    CFLAGS = -Wall -Wextra -std=c11 -O2 -Ilib -D_GNU_SOURCE
    ifeq ($(NO_URING),1)
        CFLAGS += -DCEXPRESS_USE_EPOLL
        EVENT_LOOP_SRC = lib/event_loop_linux.c lib/event_loop_epoll.c
    else
        LDFLAGS += -luring
        EVENT_LOOP_SRC = lib/event_loop_linux.c lib/event_loop_io_uring.c lib/event_loop_epoll.c
    endif
else
    CC = gcc-16
    CFLAGS = -Wall -Wextra -std=c11 -O2 -Ilib
    EVENT_LOOP_SRC = lib/event_loop_kqueue.c
endif

# --- Sanitizers: `make SANITIZE=1 test` (use a separate build dir: `make clean` first) ---
# ASan + UBSan over the whole suite. Memory bugs in the hand-written parsers show up here, not in normal runs.
ifeq ($(SANITIZE),1)
    CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1
    LDFLAGS += -fsanitize=address,undefined
endif

AR = ar

# --- Build output directories (strictly out-of-source) ---
BUILD_DIR = build
OBJ_DIR   = $(BUILD_DIR)/obj
BIN_DIR   = $(BUILD_DIR)/bin
LIB_DIR   = $(BUILD_DIR)/lib

# --- lib: the reusable engine ---
LIB_SRCS = lib/connection.c $(EVENT_LOOP_SRC) lib/cluster.c lib/http_parser.c lib/router.c lib/response.c lib/middleware.c \
           lib/multipart.c lib/urlencoded.c lib/static.c lib/arena.c \
           lib/vendor/yyjson/yyjson.c lib/vendor/picohttpparser/picohttpparser.c
LIB_OBJS = $(patsubst %.c, $(OBJ_DIR)/%.o, $(LIB_SRCS))
EVENT_LOOP_OBJS = $(patsubst %.c, $(OBJ_DIR)/%.o, $(EVENT_LOOP_SRC))
LIB      = $(LIB_DIR)/libcexpress.a


# --- test binaries ---
JSON_TEST_BIN        = $(BIN_DIR)/test_json
MIDDLEWARE_TEST_BIN  = $(BIN_DIR)/test_middleware
ROUTER_TEST_BIN      = $(BIN_DIR)/test_router
HTTP_PARSER_TEST_BIN = $(BIN_DIR)/test_http_parser
HTTP_HARDENING_TEST_BIN = $(BIN_DIR)/test_http_hardening
CONNECTION_TEST_BIN  = $(BIN_DIR)/test_connection
RESPONSE_TEST_BIN    = $(BIN_DIR)/test_response
MULTIPART_TEST_BIN   = $(BIN_DIR)/test_multipart
URLENCODED_TEST_BIN  = $(BIN_DIR)/test_urlencoded
STATIC_TEST_BIN      = $(BIN_DIR)/test_static
EVENT_LOOP_TEST_BIN  = $(BIN_DIR)/test_event_loop
CLUSTER_TEST_BIN     = $(BIN_DIR)/test_cluster
COOKBOOK_TEST_BIN    = $(BIN_DIR)/test_cookbook
BENCH_BIN            = $(BIN_DIR)/bench_hotpath
PING_TEST_BIN        = $(BIN_DIR)/test_ping
PIPELINING_TEST_BIN  = $(BIN_DIR)/test_pipelining
READ_BUF_TEST_BIN    = $(BIN_DIR)/test_read_buf
STREAM_TEST_BIN      = $(BIN_DIR)/test_stream
ANSWERED_TEST_BIN    = $(BIN_DIR)/test_answered

TEST_BINS = $(MIDDLEWARE_TEST_BIN) $(ROUTER_TEST_BIN) $(HTTP_PARSER_TEST_BIN) \
            $(CONNECTION_TEST_BIN) $(RESPONSE_TEST_BIN) $(MULTIPART_TEST_BIN) $(URLENCODED_TEST_BIN) \
            $(STATIC_TEST_BIN) $(EVENT_LOOP_TEST_BIN) $(CLUSTER_TEST_BIN) \
            $(COOKBOOK_TEST_BIN) $(HTTP_HARDENING_TEST_BIN) $(PING_TEST_BIN) $(PIPELINING_TEST_BIN) \
            $(READ_BUF_TEST_BIN) $(STREAM_TEST_BIN) $(ANSWERED_TEST_BIN)

.PHONY: all demo test clean test_epoll bench fuzz check-docs

all: $(LIB)

demo: $(LIB)
	$(MAKE) -C examples/todo_sqlite

# Generic compilation rule into $(OBJ_DIR). -MMD -MP emits a .d file per object listing the
# headers it included, so editing a header (e.g. lib/app_types.h) rebuilds every dependent object.
$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(shell find $(OBJ_DIR) -name '*.d' 2>/dev/null)

$(LIB): $(LIB_OBJS)
	@mkdir -p $(LIB_DIR)
	$(AR) rcs $@ $(LIB_OBJS)



$(MIDDLEWARE_TEST_BIN): $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/vendor/picohttpparser/picohttpparser.o $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/lib/arena.o $(OBJ_DIR)/tests/test_middleware.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(ROUTER_TEST_BIN): $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/vendor/picohttpparser/picohttpparser.o $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/lib/arena.o $(OBJ_DIR)/tests/test_router.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(HTTP_PARSER_TEST_BIN): $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/vendor/picohttpparser/picohttpparser.o $(OBJ_DIR)/lib/arena.o $(OBJ_DIR)/tests/test_http_parser.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(HTTP_HARDENING_TEST_BIN): $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/vendor/picohttpparser/picohttpparser.o $(OBJ_DIR)/lib/arena.o $(OBJ_DIR)/tests/test_http_hardening.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(CONNECTION_TEST_BIN): $(OBJ_DIR)/lib/cluster.o $(OBJ_DIR)/lib/connection.o $(EVENT_LOOP_OBJS) $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/vendor/picohttpparser/picohttpparser.o $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/lib/arena.o $(OBJ_DIR)/tests/test_connection.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(PIPELINING_TEST_BIN): $(OBJ_DIR)/lib/cluster.o $(OBJ_DIR)/lib/connection.o $(EVENT_LOOP_OBJS) $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/vendor/picohttpparser/picohttpparser.o $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/lib/arena.o $(OBJ_DIR)/tests/test_pipelining.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(READ_BUF_TEST_BIN): $(OBJ_DIR)/lib/cluster.o $(OBJ_DIR)/lib/connection.o $(EVENT_LOOP_OBJS) $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/vendor/picohttpparser/picohttpparser.o $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/lib/arena.o $(OBJ_DIR)/tests/test_read_buf.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(STREAM_TEST_BIN): $(OBJ_DIR)/lib/cluster.o $(OBJ_DIR)/lib/connection.o $(EVENT_LOOP_OBJS) $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/vendor/picohttpparser/picohttpparser.o $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/lib/arena.o $(OBJ_DIR)/tests/test_stream.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(ANSWERED_TEST_BIN): $(OBJ_DIR)/lib/cluster.o $(OBJ_DIR)/lib/connection.o $(EVENT_LOOP_OBJS) $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/vendor/picohttpparser/picohttpparser.o $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/lib/arena.o $(OBJ_DIR)/tests/test_answered.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(RESPONSE_TEST_BIN): $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/vendor/picohttpparser/picohttpparser.o $(OBJ_DIR)/lib/arena.o $(OBJ_DIR)/tests/test_response.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(MULTIPART_TEST_BIN): $(OBJ_DIR)/lib/multipart.o $(OBJ_DIR)/tests/test_multipart.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(URLENCODED_TEST_BIN): $(OBJ_DIR)/lib/urlencoded.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/vendor/picohttpparser/picohttpparser.o $(OBJ_DIR)/lib/arena.o $(OBJ_DIR)/tests/test_urlencoded.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(STATIC_TEST_BIN): $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/vendor/picohttpparser/picohttpparser.o $(OBJ_DIR)/lib/arena.o $(OBJ_DIR)/tests/test_static.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(EVENT_LOOP_TEST_BIN): $(OBJ_DIR)/lib/cluster.o $(OBJ_DIR)/lib/connection.o $(EVENT_LOOP_OBJS) $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/vendor/picohttpparser/picohttpparser.o $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/lib/arena.o $(OBJ_DIR)/tests/test_event_loop.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(CLUSTER_TEST_BIN): $(OBJ_DIR)/lib/cluster.o $(OBJ_DIR)/lib/connection.o $(EVENT_LOOP_OBJS) $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/vendor/picohttpparser/picohttpparser.o $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/lib/arena.o $(OBJ_DIR)/tests/test_cluster.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Cookbook: lib/examples/cookbook.c is the tested few-shot reference; the test drives every recipe.
$(COOKBOOK_TEST_BIN): $(OBJ_DIR)/tests/test_cookbook.o $(OBJ_DIR)/lib/examples/cookbook.o $(LIB)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/tests/test_cookbook.o $(OBJ_DIR)/lib/examples/cookbook.o $(LIB) $(LDFLAGS)

# The /ping handler test.
$(PING_TEST_BIN): $(OBJ_DIR)/tests/test_ping.o $(LIB)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/tests/test_ping.o $(LIB) $(LDFLAGS)

# Per-request CPU cost of the pure path (parse -> route -> dispatch -> response), no sockets.
$(BENCH_BIN): $(OBJ_DIR)/tests/bench_hotpath.o $(LIB)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/tests/bench_hotpath.o $(LIB) $(LDFLAGS)

bench: $(BENCH_BIN)
	./$(BENCH_BIN)

# Fails if lib/API.md and the lib headers disagree about which public functions exist.
check-docs:
	./scripts/check_docs.sh

# Mutation fuzzing of the request parser under ASan + UBSan (see tests/fuzz_parser.c), then the end-to-end
# "every input is answered or closed" oracle over real sockets (tests/test_answered.c) with
# FUZZ_ANSWERED_ITERS random inputs. FUZZ_SEED varies both runs' inputs (0 = the default stream).
FUZZ_ITERS ?= 1000000
FUZZ_ANSWERED_ITERS ?= 100000
FUZZ_SEED ?= 0
fuzz:
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 -o $(BIN_DIR)/fuzz_parser \
	    tests/fuzz_parser.c lib/http_parser.c lib/router.c lib/middleware.c lib/response.c lib/static.c lib/arena.c lib/vendor/picohttpparser/picohttpparser.c
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./$(BIN_DIR)/fuzz_parser $(FUZZ_ITERS) $(FUZZ_SEED)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 -o $(BIN_DIR)/fuzz_answered \
	    tests/test_answered.c lib/connection.c $(EVENT_LOOP_SRC) lib/cluster.c lib/http_parser.c lib/router.c lib/middleware.c \
	    lib/response.c lib/static.c lib/arena.c lib/vendor/picohttpparser/picohttpparser.c $(LDFLAGS)
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./$(BIN_DIR)/fuzz_answered $(FUZZ_ANSWERED_ITERS) $(FUZZ_SEED)

# Epoll verification on macOS via epoll-shim (if installed)
EPOLL_SHIM_PREFIX ?= /opt/homebrew/opt/epoll-shim
ifeq ($(shell test -d $(EPOLL_SHIM_PREFIX) && echo yes),yes)
    EPOLL_SHIM_CFLAGS = -I$(EPOLL_SHIM_PREFIX)/include/libepoll-shim -DCEXPRESS_USE_EPOLL
    EPOLL_SHIM_LDFLAGS = -L$(EPOLL_SHIM_PREFIX)/lib -lepoll-shim
    EPOLL_TEST_BIN = $(BIN_DIR)/test_epoll
    CONNECTION_EPOLL_TEST_BIN = $(BIN_DIR)/test_connection_epoll
    PIPELINING_EPOLL_TEST_BIN = $(BIN_DIR)/test_pipelining_epoll
    STREAM_EPOLL_TEST_BIN = $(BIN_DIR)/test_stream_epoll
    ANSWERED_EPOLL_TEST_BIN = $(BIN_DIR)/test_answered_epoll

$(OBJ_DIR)/lib/event_loop_epoll_shim.o: lib/event_loop_epoll.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(EPOLL_SHIM_CFLAGS) -c -o $@ $<

$(OBJ_DIR)/lib/event_loop_linux_shim.o: lib/event_loop_linux.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(EPOLL_SHIM_CFLAGS) -c -o $@ $<

$(OBJ_DIR)/tests/test_event_loop_shim.o: tests/test_event_loop.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(EPOLL_SHIM_CFLAGS) -c -o $@ $<

$(OBJ_DIR)/lib/connection_shim.o: lib/connection.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(EPOLL_SHIM_CFLAGS) -c -o $@ $<

$(OBJ_DIR)/tests/test_connection_shim.o: tests/test_connection.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(EPOLL_SHIM_CFLAGS) -c -o $@ $<

$(EPOLL_TEST_BIN): $(OBJ_DIR)/lib/cluster.o $(OBJ_DIR)/lib/connection_shim.o $(OBJ_DIR)/lib/event_loop_epoll_shim.o $(OBJ_DIR)/lib/event_loop_linux_shim.o $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/vendor/picohttpparser/picohttpparser.o $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/lib/arena.o $(OBJ_DIR)/tests/test_event_loop_shim.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(EPOLL_SHIM_LDFLAGS) $(LDFLAGS)

$(CONNECTION_EPOLL_TEST_BIN): $(OBJ_DIR)/lib/cluster.o $(OBJ_DIR)/lib/connection_shim.o $(OBJ_DIR)/lib/event_loop_epoll_shim.o $(OBJ_DIR)/lib/event_loop_linux_shim.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/vendor/picohttpparser/picohttpparser.o $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/lib/arena.o $(OBJ_DIR)/tests/test_connection_shim.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(EPOLL_SHIM_LDFLAGS) $(LDFLAGS)

$(OBJ_DIR)/tests/test_pipelining_shim.o: tests/test_pipelining.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(EPOLL_SHIM_CFLAGS) -c -o $@ $<

$(PIPELINING_EPOLL_TEST_BIN): $(OBJ_DIR)/lib/cluster.o $(OBJ_DIR)/lib/connection_shim.o $(OBJ_DIR)/lib/event_loop_epoll_shim.o $(OBJ_DIR)/lib/event_loop_linux_shim.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/vendor/picohttpparser/picohttpparser.o $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/lib/arena.o $(OBJ_DIR)/tests/test_pipelining_shim.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(EPOLL_SHIM_LDFLAGS) $(LDFLAGS)

$(OBJ_DIR)/tests/test_stream_shim.o: tests/test_stream.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(EPOLL_SHIM_CFLAGS) -c -o $@ $<

$(STREAM_EPOLL_TEST_BIN): $(OBJ_DIR)/lib/cluster.o $(OBJ_DIR)/lib/connection_shim.o $(OBJ_DIR)/lib/event_loop_epoll_shim.o $(OBJ_DIR)/lib/event_loop_linux_shim.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/vendor/picohttpparser/picohttpparser.o $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/lib/arena.o $(OBJ_DIR)/tests/test_stream_shim.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(EPOLL_SHIM_LDFLAGS) $(LDFLAGS)

$(OBJ_DIR)/tests/test_answered_shim.o: tests/test_answered.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(EPOLL_SHIM_CFLAGS) -c -o $@ $<

$(ANSWERED_EPOLL_TEST_BIN): $(OBJ_DIR)/lib/cluster.o $(OBJ_DIR)/lib/connection_shim.o $(OBJ_DIR)/lib/event_loop_epoll_shim.o $(OBJ_DIR)/lib/event_loop_linux_shim.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/vendor/picohttpparser/picohttpparser.o $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/lib/arena.o $(OBJ_DIR)/tests/test_answered_shim.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(EPOLL_SHIM_LDFLAGS) $(LDFLAGS)

test_epoll: $(EPOLL_TEST_BIN) $(CONNECTION_EPOLL_TEST_BIN) $(PIPELINING_EPOLL_TEST_BIN) $(STREAM_EPOLL_TEST_BIN) $(ANSWERED_EPOLL_TEST_BIN)
	./$(EPOLL_TEST_BIN)
	./$(CONNECTION_EPOLL_TEST_BIN)
	./$(PIPELINING_EPOLL_TEST_BIN)
	./$(STREAM_EPOLL_TEST_BIN)
	./$(ANSWERED_EPOLL_TEST_BIN)
endif

test: $(TEST_BINS)
	./$(MIDDLEWARE_TEST_BIN)
	./$(ROUTER_TEST_BIN)
	./$(HTTP_PARSER_TEST_BIN)
	./$(CONNECTION_TEST_BIN)
	./$(RESPONSE_TEST_BIN)
	./$(MULTIPART_TEST_BIN)
	./$(URLENCODED_TEST_BIN)
	./$(STATIC_TEST_BIN)
	./$(EVENT_LOOP_TEST_BIN)
	./$(CLUSTER_TEST_BIN)
	./$(COOKBOOK_TEST_BIN)
	./$(HTTP_HARDENING_TEST_BIN)
	./$(PING_TEST_BIN)
	./$(PIPELINING_TEST_BIN)
	./$(READ_BUF_TEST_BIN)
	./$(STREAM_TEST_BIN)
	./$(ANSWERED_TEST_BIN)

clean:
	rm -rf $(BUILD_DIR) cexpress httpServer
	$(MAKE) -C examples/todo_sqlite clean || true

export:
	@if [ -z "$(DEST)" ]; then echo "Usage: make export DEST=/path/to/target/vendor/cexpress"; exit 1; fi
	@./scripts/export_framework.sh "$(DEST)"

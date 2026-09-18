UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Linux)
    CC = gcc
    CFLAGS = -Wall -Wextra -std=c11 -O2 -Ilib -D_GNU_SOURCE
    EVENT_LOOP_SRC = lib/event_loop_epoll.c
else
    CC = gcc-16
    CFLAGS = -Wall -Wextra -std=c11 -O2 -Ilib
    EVENT_LOOP_SRC = lib/event_loop_kqueue.c
endif

# --- OpenSSL configuration (Auto-detected with optional NO_TLS=1 override) ---
ifeq ($(NO_TLS),1)
    CEXPRESS_HAS_TLS = 0
    OPENSSL_CFLAGS = -DCEXPRESS_HAS_TLS=0
    OPENSSL_LDFLAGS =
else
    OPENSSL_PREFIX ?= $(shell brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null || echo /opt/homebrew/opt/openssl@3)
    ifeq ($(shell test -d $(OPENSSL_PREFIX)/include/openssl && echo yes),yes)
        CEXPRESS_HAS_TLS = 1
        OPENSSL_CFLAGS = -I$(OPENSSL_PREFIX)/include -DCEXPRESS_HAS_TLS=1
        OPENSSL_LDFLAGS = -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto
    else ifeq ($(shell pkg-config --exists openssl 2>/dev/null && echo yes),yes)
        CEXPRESS_HAS_TLS = 1
        OPENSSL_CFLAGS = $(shell pkg-config --cflags openssl) -DCEXPRESS_HAS_TLS=1
        OPENSSL_LDFLAGS = $(shell pkg-config --libs openssl)
    else
        CEXPRESS_HAS_TLS = 0
        OPENSSL_CFLAGS = -DCEXPRESS_HAS_TLS=0
        OPENSSL_LDFLAGS =
    endif
endif

CFLAGS += $(OPENSSL_CFLAGS)
LDFLAGS += $(OPENSSL_LDFLAGS)

AR = ar

# --- Build output directories (strictly out-of-source) ---
BUILD_DIR = build
OBJ_DIR   = $(BUILD_DIR)/obj
BIN_DIR   = $(BUILD_DIR)/bin
LIB_DIR   = $(BUILD_DIR)/lib

# --- lib: the reusable engine ---
LIB_SRCS = lib/connection.c $(EVENT_LOOP_SRC) lib/cluster.c lib/tls.c lib/http_parser.c lib/router.c lib/response.c lib/middleware.c \
           lib/multipart.c lib/urlencoded.c lib/static.c \
           lib/json/json_parser.c lib/json/json_value.c lib/json/json_writer.c
LIB_OBJS = $(patsubst %.c, $(OBJ_DIR)/%.o, $(LIB_SRCS))
LIB      = $(LIB_DIR)/libcexpress.a

# --- app: the application built on top of the lib ---
APP_SRCS = app/main.c app/handlers.c app/middlewares.c
APP_OBJS = $(patsubst %.c, $(OBJ_DIR)/%.o, $(APP_SRCS))
TARGET   = $(BIN_DIR)/cexpress

# --- test binaries ---
JSON_TEST_BIN        = $(BIN_DIR)/test_json
MIDDLEWARE_TEST_BIN  = $(BIN_DIR)/test_middleware
ROUTER_TEST_BIN      = $(BIN_DIR)/test_router
HTTP_PARSER_TEST_BIN = $(BIN_DIR)/test_http_parser
CONNECTION_TEST_BIN  = $(BIN_DIR)/test_connection
RESPONSE_TEST_BIN    = $(BIN_DIR)/test_response
MULTIPART_TEST_BIN   = $(BIN_DIR)/test_multipart
URLENCODED_TEST_BIN  = $(BIN_DIR)/test_urlencoded
STATIC_TEST_BIN      = $(BIN_DIR)/test_static
EVENT_LOOP_TEST_BIN  = $(BIN_DIR)/test_event_loop
CLUSTER_TEST_BIN     = $(BIN_DIR)/test_cluster
TLS_TEST_BIN         = $(BIN_DIR)/test_tls

TEST_BINS = $(JSON_TEST_BIN) $(MIDDLEWARE_TEST_BIN) $(ROUTER_TEST_BIN) $(HTTP_PARSER_TEST_BIN) \
            $(CONNECTION_TEST_BIN) $(RESPONSE_TEST_BIN) $(MULTIPART_TEST_BIN) $(URLENCODED_TEST_BIN) \
            $(STATIC_TEST_BIN) $(EVENT_LOOP_TEST_BIN) $(CLUSTER_TEST_BIN) $(TLS_TEST_BIN)

.PHONY: all run test clean test_epoll

all: cexpress

# Generic compilation rule into $(OBJ_DIR)
$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(LIB): $(LIB_OBJS)
	@mkdir -p $(LIB_DIR)
	$(AR) rcs $@ $(LIB_OBJS)

$(TARGET): $(APP_OBJS) $(LIB)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(APP_OBJS) $(LIB) $(LDFLAGS)

cexpress: $(TARGET)
	@ln -sf $(TARGET) cexpress

run: $(TARGET)
	./$(TARGET)

$(JSON_TEST_BIN): $(OBJ_DIR)/lib/json/json_parser.o $(OBJ_DIR)/lib/json/json_value.o $(OBJ_DIR)/lib/json/json_writer.o $(OBJ_DIR)/tests/test_json.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(MIDDLEWARE_TEST_BIN): $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/tests/test_middleware.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(ROUTER_TEST_BIN): $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/tests/test_router.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(HTTP_PARSER_TEST_BIN): $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/tests/test_http_parser.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(CONNECTION_TEST_BIN): $(OBJ_DIR)/lib/cluster.o $(OBJ_DIR)/lib/connection.o $(OBJ_DIR)/lib/tls.o $(OBJ_DIR)/$(EVENT_LOOP_SRC:.c=.o) $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/tests/test_connection.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(RESPONSE_TEST_BIN): $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/tests/test_response.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(MULTIPART_TEST_BIN): $(OBJ_DIR)/lib/multipart.o $(OBJ_DIR)/tests/test_multipart.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(URLENCODED_TEST_BIN): $(OBJ_DIR)/lib/urlencoded.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/tests/test_urlencoded.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(STATIC_TEST_BIN): $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/tests/test_static.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(EVENT_LOOP_TEST_BIN): $(OBJ_DIR)/lib/cluster.o $(OBJ_DIR)/lib/connection.o $(OBJ_DIR)/lib/tls.o $(OBJ_DIR)/$(EVENT_LOOP_SRC:.c=.o) $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/tests/test_event_loop.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(CLUSTER_TEST_BIN): $(OBJ_DIR)/lib/cluster.o $(OBJ_DIR)/lib/connection.o $(OBJ_DIR)/lib/tls.o $(OBJ_DIR)/$(EVENT_LOOP_SRC:.c=.o) $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/tests/test_cluster.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TLS_TEST_BIN): $(OBJ_DIR)/lib/cluster.o $(OBJ_DIR)/lib/connection.o $(OBJ_DIR)/lib/tls.o $(OBJ_DIR)/$(EVENT_LOOP_SRC:.c=.o) $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/tests/test_tls.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Epoll verification on macOS via epoll-shim (if installed)
EPOLL_SHIM_PREFIX ?= /opt/homebrew/opt/epoll-shim
ifeq ($(shell test -d $(EPOLL_SHIM_PREFIX) && echo yes),yes)
    EPOLL_SHIM_CFLAGS = -I$(EPOLL_SHIM_PREFIX)/include/libepoll-shim -DCEXPRESS_USE_EPOLL
    EPOLL_SHIM_LDFLAGS = -L$(EPOLL_SHIM_PREFIX)/lib -lepoll-shim
    EPOLL_TEST_BIN = $(BIN_DIR)/test_epoll
    CONNECTION_EPOLL_TEST_BIN = $(BIN_DIR)/test_connection_epoll

$(OBJ_DIR)/lib/event_loop_epoll_shim.o: lib/event_loop_epoll.c
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

$(EPOLL_TEST_BIN): $(OBJ_DIR)/lib/cluster.o $(OBJ_DIR)/lib/connection_shim.o $(OBJ_DIR)/lib/tls.o $(OBJ_DIR)/lib/event_loop_epoll_shim.o $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/tests/test_event_loop_shim.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(EPOLL_SHIM_LDFLAGS) $(LDFLAGS)

$(CONNECTION_EPOLL_TEST_BIN): $(OBJ_DIR)/lib/cluster.o $(OBJ_DIR)/lib/connection_shim.o $(OBJ_DIR)/lib/tls.o $(OBJ_DIR)/lib/event_loop_epoll_shim.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/static.o $(OBJ_DIR)/tests/test_connection_shim.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(EPOLL_SHIM_LDFLAGS) $(LDFLAGS)

test_epoll: $(EPOLL_TEST_BIN) $(CONNECTION_EPOLL_TEST_BIN)
	./$(EPOLL_TEST_BIN)
	./$(CONNECTION_EPOLL_TEST_BIN)
endif

test: $(TEST_BINS)
	./$(JSON_TEST_BIN)
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
	./$(TLS_TEST_BIN)

clean:
	rm -rf $(BUILD_DIR) cexpress httpServer

CC = gcc-16
AR = ar
CFLAGS = -Wall -Wextra -std=c11 -Ilib

# --- lib: the reusable engine (connection handling, router, parser, response, middleware, json) ---
LIB_SRCS = lib/connection.c lib/httpParser.c lib/router.c lib/response.c lib/middleware.c \
           lib/json/jsonParser.c lib/json/jsonValue.c lib/json/jsonWriter.c
LIB_OBJS = $(LIB_SRCS:.c=.o)
LIB = lib/libcexpress.a

# --- app: the application built on top of the lib ---
APP_SRCS = app/main.c app/handlers.c app/middlewares.c
APP_OBJS = $(APP_SRCS:.c=.o)

JSON_TEST_SRCS = lib/json/jsonParser.c lib/json/jsonValue.c lib/json/jsonWriter.c lib/json/json_test.c
JSON_TEST_BIN = lib/json/json_test

MIDDLEWARE_TEST_SRCS = lib/middleware.c lib/router.c lib/response.c lib/httpParser.c lib/middleware_test.c
MIDDLEWARE_TEST_BIN = lib/middleware_test

ROUTER_TEST_SRCS = lib/router.c lib/router_test.c
ROUTER_TEST_BIN = lib/router_test

HTTP_PARSER_TEST_SRCS = lib/httpParser.c lib/http_parser_test.c
HTTP_PARSER_TEST_BIN = lib/http_parser_test

CONNECTION_TEST_SRCS = lib/connection.c lib/httpParser.c lib/router.c lib/response.c lib/middleware.c lib/connection_test.c
CONNECTION_TEST_BIN = lib/connection_test

TEST_BINS = $(JSON_TEST_BIN) $(MIDDLEWARE_TEST_BIN) $(ROUTER_TEST_BIN) $(HTTP_PARSER_TEST_BIN) $(CONNECTION_TEST_BIN)

.PHONY: all run test clean

all: cexpress

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $(LIB_OBJS)

cexpress: $(APP_OBJS) $(LIB)
	$(CC) $(CFLAGS) -o $@ $(APP_OBJS) $(LIB)

run: cexpress
	./cexpress

$(JSON_TEST_BIN): $(JSON_TEST_SRCS)
	$(CC) $(CFLAGS) -o $@ $(JSON_TEST_SRCS)

$(MIDDLEWARE_TEST_BIN): $(MIDDLEWARE_TEST_SRCS)
	$(CC) $(CFLAGS) -o $@ $(MIDDLEWARE_TEST_SRCS)

$(ROUTER_TEST_BIN): $(ROUTER_TEST_SRCS)
	$(CC) $(CFLAGS) -o $@ $(ROUTER_TEST_SRCS)

$(HTTP_PARSER_TEST_BIN): $(HTTP_PARSER_TEST_SRCS)
	$(CC) $(CFLAGS) -o $@ $(HTTP_PARSER_TEST_SRCS)

$(CONNECTION_TEST_BIN): $(CONNECTION_TEST_SRCS)
	$(CC) $(CFLAGS) -o $@ $(CONNECTION_TEST_SRCS)

test: $(TEST_BINS)
	./$(JSON_TEST_BIN)
	./$(MIDDLEWARE_TEST_BIN)
	./$(ROUTER_TEST_BIN)
	./$(HTTP_PARSER_TEST_BIN)
	./$(CONNECTION_TEST_BIN)

clean:
	rm -f cexpress httpServer $(TEST_BINS) $(LIB) $(LIB_OBJS) $(APP_OBJS)

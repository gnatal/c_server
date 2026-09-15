CC = gcc-16
AR = ar
CFLAGS = -Wall -Wextra -std=c11 -Ilib

# --- lib: the reusable engine (connection handling, router, parser, response, middleware, json) ---
LIB_SRCS = lib/connection.c lib/httpParser.c lib/router.c lib/response.c lib/middleware.c \
           lib/json/jsonParser.c lib/json/jsonValue.c lib/json/jsonWriter.c
LIB_OBJS = $(LIB_SRCS:.c=.o)
LIB = lib/libcserver.a

# --- app: the application built on top of the lib ---
APP_SRCS = app/main.c app/handlers.c app/middlewares.c
APP_OBJS = $(APP_SRCS:.c=.o)

JSON_TEST_SRCS = lib/json/jsonParser.c lib/json/jsonValue.c lib/json/jsonWriter.c lib/json/json_test.c
JSON_TEST_BIN = lib/json/json_test

MIDDLEWARE_TEST_SRCS = lib/middleware.c lib/router.c lib/response.c lib/httpParser.c lib/middleware_test.c
MIDDLEWARE_TEST_BIN = lib/middleware_test

.PHONY: all run test clean

all: httpServer

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $(LIB_OBJS)

httpServer: $(APP_OBJS) $(LIB)
	$(CC) $(CFLAGS) -o $@ $(APP_OBJS) $(LIB)

run: httpServer
	./httpServer

$(JSON_TEST_BIN): $(JSON_TEST_SRCS)
	$(CC) $(CFLAGS) -o $@ $(JSON_TEST_SRCS)

$(MIDDLEWARE_TEST_BIN): $(MIDDLEWARE_TEST_SRCS)
	$(CC) $(CFLAGS) -o $@ $(MIDDLEWARE_TEST_SRCS)

test: $(JSON_TEST_BIN) $(MIDDLEWARE_TEST_BIN)
	./$(JSON_TEST_BIN)
	./$(MIDDLEWARE_TEST_BIN)

clean:
	rm -f httpServer $(JSON_TEST_BIN) $(MIDDLEWARE_TEST_BIN) $(LIB) $(LIB_OBJS) $(APP_OBJS)

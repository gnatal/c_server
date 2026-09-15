CC = gcc-16
AR = ar
CFLAGS = -Wall -Wextra -std=c11 -Ilib

# --- lib: the reusable engine (connection handling, router, parser, response, json) ---
LIB_SRCS = lib/connection.c lib/httpParser.c lib/router.c lib/response.c \
           lib/json/jsonParser.c lib/json/jsonValue.c lib/json/jsonWriter.c
LIB_OBJS = $(LIB_SRCS:.c=.o)
LIB = lib/libcserver.a

# --- app: the application built on top of the lib ---
APP_SRCS = app/main.c app/handlers.c
APP_OBJS = $(APP_SRCS:.c=.o)

TEST_SRCS = lib/json/jsonParser.c lib/json/jsonValue.c lib/json/jsonWriter.c lib/json/json_test.c
TEST_BIN = lib/json/json_test

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

$(TEST_BIN): $(TEST_SRCS)
	$(CC) $(CFLAGS) -o $@ $(TEST_SRCS)

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -f httpServer $(TEST_BIN) $(LIB) $(LIB_OBJS) $(APP_OBJS)

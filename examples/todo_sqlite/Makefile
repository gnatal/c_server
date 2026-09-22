UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Linux)
    CC = gcc
    CFLAGS = -Wall -Wextra -std=c11 -O2 -I../../lib -D_GNU_SOURCE
else
    CC = gcc-16
    CFLAGS = -Wall -Wextra -std=c11 -O2 -I../../lib
endif
LDFLAGS = -L../../build/lib -lcexpress
ifeq ($(UNAME_S),Linux)
    LDFLAGS += -luring
endif

# Copy SQLite setup from root Makefile
SQLITE_PREFIX ?= $(shell brew --prefix sqlite3 2>/dev/null || brew --prefix sqlite 2>/dev/null || echo /opt/homebrew/opt/sqlite)
ifeq ($(shell test -d $(SQLITE_PREFIX)/include && echo yes),yes)
    SQLITE_CFLAGS = -I$(SQLITE_PREFIX)/include
    SQLITE_LDFLAGS = -L$(SQLITE_PREFIX)/lib -lsqlite3
else ifeq ($(shell pkg-config --exists sqlite3 2>/dev/null && echo yes),yes)
    SQLITE_CFLAGS = $(shell pkg-config --cflags sqlite3)
    SQLITE_LDFLAGS = $(shell pkg-config --libs sqlite3)
else
    SQLITE_CFLAGS =
    SQLITE_LDFLAGS = -lsqlite3
endif

CFLAGS += $(SQLITE_CFLAGS)
LDFLAGS += $(SQLITE_LDFLAGS)

SRCS = main.c handlers.c middlewares.c db.c
OBJS = $(SRCS:.c=.o)
TARGET = cexpress_demo

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

run: $(TARGET)
	./$(TARGET)

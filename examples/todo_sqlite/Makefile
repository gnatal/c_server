UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Linux)
    CC = gcc
    CFLAGS = -Wall -Wextra -std=c11 -O2 -I../../lib -D_GNU_SOURCE
else
    CC = gcc-16
    CFLAGS = -Wall -Wextra -std=c11 -O2 -I../../lib
endif
LDFLAGS = -L../../build/lib -lcexpress

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

OPENSSL_PREFIX ?= $(shell brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null || echo /opt/homebrew/opt/openssl@3)
ifeq ($(shell test -d $(OPENSSL_PREFIX)/include/openssl && echo yes),yes)
    OPENSSL_LDFLAGS = -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto
else ifeq ($(shell pkg-config --exists openssl 2>/dev/null && echo yes),yes)
    OPENSSL_LDFLAGS = $(shell pkg-config --libs openssl)
else
    OPENSSL_LDFLAGS =
endif

CFLAGS += $(SQLITE_CFLAGS)
LDFLAGS += $(SQLITE_LDFLAGS) $(OPENSSL_LDFLAGS)

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

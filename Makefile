CC = gcc-16
AR = ar
CFLAGS = -Wall -Wextra -std=c11 -Ilib

# --- Build output directories (strictly out-of-source) ---
BUILD_DIR = build
OBJ_DIR   = $(BUILD_DIR)/obj
BIN_DIR   = $(BUILD_DIR)/bin
LIB_DIR   = $(BUILD_DIR)/lib

# --- lib: the reusable engine ---
LIB_SRCS = lib/connection.c lib/http_parser.c lib/router.c lib/response.c lib/middleware.c \
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

TEST_BINS = $(JSON_TEST_BIN) $(MIDDLEWARE_TEST_BIN) $(ROUTER_TEST_BIN) $(HTTP_PARSER_TEST_BIN) $(CONNECTION_TEST_BIN) $(RESPONSE_TEST_BIN)

.PHONY: all run test clean

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
	$(CC) $(CFLAGS) -o $@ $(APP_OBJS) $(LIB)

cexpress: $(TARGET)
	@ln -sf $(TARGET) cexpress

run: $(TARGET)
	./$(TARGET)

$(JSON_TEST_BIN): $(OBJ_DIR)/lib/json/json_parser.o $(OBJ_DIR)/lib/json/json_value.o $(OBJ_DIR)/lib/json/json_writer.o $(OBJ_DIR)/tests/test_json.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(MIDDLEWARE_TEST_BIN): $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/tests/test_middleware.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(ROUTER_TEST_BIN): $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/tests/test_router.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(HTTP_PARSER_TEST_BIN): $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/tests/test_http_parser.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(CONNECTION_TEST_BIN): $(OBJ_DIR)/lib/connection.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/lib/router.o $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/middleware.o $(OBJ_DIR)/tests/test_connection.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(RESPONSE_TEST_BIN): $(OBJ_DIR)/lib/response.o $(OBJ_DIR)/lib/http_parser.o $(OBJ_DIR)/tests/test_response.o
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

test: $(TEST_BINS)
	./$(JSON_TEST_BIN)
	./$(MIDDLEWARE_TEST_BIN)
	./$(ROUTER_TEST_BIN)
	./$(HTTP_PARSER_TEST_BIN)
	./$(CONNECTION_TEST_BIN)
	./$(RESPONSE_TEST_BIN)

clean:
	rm -rf $(BUILD_DIR) cexpress httpServer

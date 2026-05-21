CC ?= gcc
AR ?= ar
RM ?= rm -f

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin
LIB_DIR := $(BUILD_DIR)/lib

CPPFLAGS := -Ilibs/http
CFLAGS ?= -Wall -Wextra -Wpedantic -std=gnu11 -O2
ARFLAGS := rcs

HTTP_SRCS := libs/http/http.c libs/http/dynbuf.c
HTTP_OBJS := $(OBJ_DIR)/http.o $(OBJ_DIR)/dynbuf.o

.PHONY: all lib server smoke smoke-features check clean

all: lib server smoke

lib: $(LIB_DIR)/libhttp.a

server: $(BIN_DIR)/test_server

smoke: $(BIN_DIR)/smoke_http

smoke-features: $(BIN_DIR)/smoke_http_features

check: smoke smoke-features
	$(BIN_DIR)/smoke_http
	$(BIN_DIR)/smoke_http_features

$(OBJ_DIR) $(BIN_DIR) $(LIB_DIR):
	mkdir -p $@

$(OBJ_DIR)/http.o: libs/http/http.c | $(OBJ_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/dynbuf.o: libs/http/dynbuf.c | $(OBJ_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(LIB_DIR)/libhttp.a: $(HTTP_OBJS) | $(LIB_DIR)
	$(AR) $(ARFLAGS) $@ $(HTTP_OBJS)

$(BIN_DIR)/test_server: test_server.c $(LIB_DIR)/libhttp.a | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_DIR)/libhttp.a -o $@

$(BIN_DIR)/smoke_http: tests/smoke_http.c libs/http/http.c libs/http/dynbuf.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BIN_DIR)/smoke_http_features: tests/smoke_http.c libs/http/http.c libs/http/dynbuf.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		-DHTTP_ENABLE_MONITORING \
		-DHTTP_ENABLE_SELF_TESTS \
		-DHTTP_ENABLE_MULTITHREADING \
		-DHTTP_ENABLE_TLS \
		$^ -o $@

clean:
	$(RM) -r $(BUILD_DIR)

CC ?= gcc
AR ?= ar
RM ?= rm -f

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin
LIB_DIR := $(BUILD_DIR)/lib

CPPFLAGS := -Iinclude -Ilibs/http
CFLAGS ?= -Wall -Wextra -Wpedantic -std=gnu11 -O2
ARFLAGS := rcs

HTTP_SRCS := \
	libs/http/http.c \
	libs/http/http_parser.c \
	libs/http/http_response.c \
	libs/http/http_support.c \
	libs/http/dynbuf.c
HTTP_OBJS := $(patsubst libs/http/%.c,$(OBJ_DIR)/%.o,$(HTTP_SRCS))

.PHONY: all help lib server examples smoke smoke-features test test-smoke test-unit test-integration check clean

all: lib examples test-smoke

help:
	@printf '%s\n' 'Project Make Targets'
	@printf '%s\n' ''
	@printf '%s\n' 'Usage:'
	@printf '%s\n' '  make help              Show this project help'
	@printf '%s\n' '  make                   Build the library, examples, and smoke tests'
	@printf '%s\n' ''
	@printf '%s\n' 'Build targets:'
	@printf '%s\n' '  make lib               Build build/lib/libhttp.a'
	@printf '%s\n' '  make examples          Build demo/example binaries'
	@printf '%s\n' '  make server            Build the demo HTTP server only'
	@printf '%s\n' ''
	@printf '%s\n' 'Test targets:'
	@printf '%s\n' '  make test-smoke        Run smoke tests: fast API/build sanity checks'
	@printf '%s\n' '  make test-unit         Run unit tests: utility and response API checks'
	@printf '%s\n' '  make test-integration  Run integration tests: start server and probe HTTP routes'
	@printf '%s\n' '  make test              Run all test suites'
	@printf '%s\n' '  make check             Alias for make test'
	@printf '%s\n' ''
	@printf '%s\n' 'Maintenance:'
	@printf '%s\n' '  make clean             Remove build artifacts from build/'
	@printf '%s\n' ''


lib: $(LIB_DIR)/libhttp.a

examples: $(BIN_DIR)/test_server

server: $(BIN_DIR)/test_server

smoke: $(BIN_DIR)/smoke_http

smoke-features: $(BIN_DIR)/smoke_http_features

test-smoke: smoke smoke-features
	$(BIN_DIR)/smoke_http
	$(BIN_DIR)/smoke_http_features

test-unit: $(BIN_DIR)/http_unit
	$(BIN_DIR)/http_unit

test-integration: examples
	tests/integration/http_server_integration.sh

test: test-smoke test-unit test-integration

check: test

$(OBJ_DIR) $(BIN_DIR) $(LIB_DIR):
	mkdir -p $@

$(OBJ_DIR)/%.o: libs/http/%.c | $(OBJ_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(LIB_DIR)/libhttp.a: $(HTTP_OBJS) | $(LIB_DIR)
	$(AR) $(ARFLAGS) $@ $(HTTP_OBJS)

$(BIN_DIR)/test_server: examples/test_server.c $(LIB_DIR)/libhttp.a | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_DIR)/libhttp.a -o $@

$(BIN_DIR)/smoke_http: tests/smoke/smoke_http.c $(HTTP_SRCS) | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BIN_DIR)/smoke_http_features: tests/smoke/smoke_http.c $(HTTP_SRCS) | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		-DHTTP_ENABLE_MONITORING \
		-DHTTP_ENABLE_SELF_TESTS \
		-DHTTP_ENABLE_MULTITHREADING \
		-DHTTP_ENABLE_TLS \
		$^ -o $@

$(BIN_DIR)/http_unit: tests/unit/http_unit.c $(HTTP_SRCS) | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

clean:
	$(RM) -r $(BUILD_DIR)

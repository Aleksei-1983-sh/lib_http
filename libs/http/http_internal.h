#ifndef HTTP_INTERNAL_H
#define HTTP_INTERNAL_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "http/http.h"
#include "dynbuf.h"

#define INITIAL_CONN_CAPACITY 64
#define INITIAL_ROUTE_CAPACITY 16
#define MAX_HEADER_NAME_LEN 256
#define MAX_HEADER_VALUE_LEN 1024
#define URL_UTIL_BUF_SIZE (3 * 1024)

#if defined(DEBUG) && DEBUG == 1
#define HTTP_DBG(ctxptr, fmt, ...) \
    HTTP_DBG_IMPL((void *)(ctxptr), fmt, ##__VA_ARGS__)

#define HTTP_DBG_IMPL(_vctx, fmt, ...) \
    do { \
        if ((_vctx) && ((http_ctx_t *)(_vctx))->config.log_fn) { \
            const char *_fmt = "[%s:%d] " fmt; \
            ((http_ctx_t *)(_vctx))->config.log_fn( \
                HTTP_LOG_DEBUG, \
                ((http_ctx_t *)(_vctx))->config.log_user_data, \
                _fmt, \
                __func__, \
                __LINE__, \
                ##__VA_ARGS__); \
        } else { \
            const char *_fmt = "\033[0;32m[DEBUG] \033[0m [%s:%d] " fmt "\n"; \
            fprintf(stderr, _fmt, __func__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)
#else
#define HTTP_DBG(ctxptr, fmt, ...) ((void)0)
#endif

#define HTTP_ERR(ctxptr, fmt, ...) \
    HTTP_ERR_IMPL((void *)(ctxptr), fmt, ##__VA_ARGS__)

#define HTTP_ERR_IMPL(_vctx, fmt, ...) \
    do { \
        if ((_vctx) && ((http_ctx_t *)(_vctx))->config.log_fn) { \
            const char *_fmt = "[%s:%d] " fmt; \
            ((http_ctx_t *)(_vctx))->config.log_fn( \
                HTTP_LOG_ERROR, \
                ((http_ctx_t *)(_vctx))->config.log_user_data, \
                _fmt, \
                __func__, \
                __LINE__, \
                ##__VA_ARGS__); \
        } else { \
            const char *_fmt = "\033[0;31m[ERROR] \033[0m [%s:%d] " fmt "\n"; \
            fprintf(stderr, _fmt, __func__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)

typedef struct http_conn http_conn_t;

typedef struct {
    http_conn_t *conn;
    dynbuf_t bodybuf;
    int headers_sent;
} http_response_internal_t;

typedef struct http_timer {
    int id;
    struct timeval due;
    int interval_ms;
    http_timer_fn cb;
    void *user_data;
    struct http_timer *next;
} http_timer_t;

typedef struct {
    char *method;
    char *pattern;
    http_handler_fn handler;
    void *user_data;
} route_entry_t;

typedef enum {
    CONN_STATE_READING,
    CONN_STATE_WRITING,
    CONN_STATE_CLOSED
} conn_state_t;

struct http_conn {
    int fd;
    int is_client;
    conn_state_t state;
    struct timeval start_time;
    dynbuf_t rb;
    size_t header_parsed;
    size_t content_length;
    http_request_t req;
    http_response_t res;
    dynbuf_t wb;
    size_t wb_sent;
    int keep_alive;
    struct http_conn *next;
};

struct http_ctx {
    http_config_t config;
    int stopped;
    int listen_fds[MAX_LISTENERS];
    int listen_count;
    http_conn_t *conns;
    route_entry_t *routes;
    size_t route_count;
    size_t route_cap;
    http_timer_t *timers;
    int next_timer_id;
#ifdef HTTP_ENABLE_MONITORING
    http_metrics_t metrics;
    long double total_response_time_ms;
#endif
};

void timeval_now(struct timeval *tv);
double timeval_diff_ms(const struct timeval *start, const struct timeval *end);
void *http_malloc(http_ctx_t *ctx, size_t sz);
void http_free_mem(http_ctx_t *ctx, void *p);

int parse_request(http_ctx_t *ctx, http_conn_t *c);
int prepare_response(http_ctx_t *ctx, http_conn_t *c);
int send_response(http_ctx_t *ctx, http_conn_t *c);

void timers_add(http_ctx_t *ctx, http_timer_t *timer);
void timers_remove(http_ctx_t *ctx, int timer_id);
void timers_check(http_ctx_t *ctx);
int compute_poll_timeout(http_ctx_t *ctx, int default_ms);

#endif

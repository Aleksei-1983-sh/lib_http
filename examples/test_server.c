#include "http/http.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static http_ctx_t *g_ctx = NULL;

static void server_log_cb(http_log_level_t level, void *user_data, const char *fmt, ...)
{
    (void)user_data;

    static const char *names[] = {"ERROR", "WARN", "INFO", "DEBUG"};
    va_list args;

    va_start(args, fmt);
    fprintf(stderr, "[%s] ", names[level]);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

static void on_sigint(int sig)
{
    (void)sig;
    if (g_ctx) {
        fprintf(stderr, "stopping server\n");
        http_stop_default();
    }
}

static void respond_text(http_response_t *res, int status, const char *reason, const char *body)
{
    http_response_init(res);
    http_response_set_status(res, status, reason);
    http_response_add_header(res, "Content-Type", "text/plain");
    http_response_write_body(res, body, strlen(body));
    http_response_end(res);
}

static void health_handler(http_request_t *req, http_response_t *res, void *user_data)
{
    (void)req;
    (void)user_data;
    respond_text(res, 200, "OK", "OK\n");
}

static void echo_handler(http_request_t *req, http_response_t *res, void *user_data)
{
    (void)user_data;

    http_response_init(res);
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/plain");

    if (strcmp(req->method, "GET") == 0) {
        const char *query = req->uri_query ? req->uri_query : "";
        http_response_write_body(res, query, strlen(query));
        http_response_write_body(res, "\n", 1);
    } else if (req->body && req->body_len > 0) {
        http_response_write_body(res, req->body, req->body_len);
        http_response_write_body(res, "\n", 1);
    } else {
        http_response_write_body(res, "(empty)\n", 8);
    }

    http_response_end(res);
}

static void headers_handler(http_request_t *req, http_response_t *res, void *user_data)
{
    (void)user_data;

    http_response_init(res);
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/plain");

    for (size_t i = 0; i < req->num_headers; ++i) {
        char line[512];
        int n = snprintf(line, sizeof(line), "%s: %s\n",
                         req->headers[i].name,
                         req->headers[i].value);
        if (n > 0) {
            http_response_write_body(res, line, (size_t)n);
        }
    }

    http_response_end(res);
}


static void default_ctx_handler(http_request_t *req, http_response_t *res, void *user_data)
{
    (void)req;
    (void)user_data;

    http_ctx_t *ctx = http_get_default_ctx();
    if (!ctx) {
        respond_text(res, 500, "Internal Server Error", "default ctx is not set\n");
        return;
    }

    if (ctx != g_ctx) {
        respond_text(res, 500, "Internal Server Error", "default ctx mismatch\n");
        return;
    }

    respond_text(res, 200, "OK", "default ctx is set\n");
}

static void timer_log_cb(void *user_data)
{
    const char *message = user_data ? (const char *)user_data : "timer fired";
    fprintf(stderr, "[timer] %s\n", message);
}

static void timer_handler(http_request_t *req, http_response_t *res, void *user_data)
{
    (void)user_data;

    int delay_ms = 0;
    if (!req->uri_query || sscanf(req->uri_query, "delay=%d", &delay_ms) != 1 || delay_ms <= 0) {
        respond_text(res, 400, "Bad Request", "usage: /set_timer?delay=1000\n");
        return;
    }

    int timer_id = http_set_timer_default(delay_ms, 0, timer_log_cb, "scheduled from HTTP route");
    if (timer_id < 0) {
        respond_text(res, 500, "Internal Server Error", "failed to schedule timer\n");
        return;
    }

    char body[128];
    int n = snprintf(body, sizeof(body), "timer scheduled: id=%d delay_ms=%d\n", timer_id, delay_ms);
    if (n < 0) {
        respond_text(res, 500, "Internal Server Error", "failed to render response\n");
        return;
    }

    http_response_init(res);
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/plain");
    http_response_write_body(res, body, (size_t)n);
    http_response_end(res);
}

#ifdef HTTP_ENABLE_MONITORING
static void metrics_handler(http_request_t *req, http_response_t *res, void *user_data)
{
    (void)req;
    (void)user_data;

    http_metrics_t metrics;
    http_ctx_t *ctx = http_get_default_ctx();
    if (!ctx || http_get_metrics(ctx, &metrics) != 0) {
        respond_text(res, 500, "Internal Server Error", "failed to read metrics\n");
        return;
    }

    char body[256];
    int n = snprintf(body, sizeof(body),
                     "requests=%llu\nresponses=%llu\nconnections=%llu\nerrors=%llu\navg_ms=%.2f\n",
                     (unsigned long long)metrics.total_requests,
                     (unsigned long long)metrics.total_responses,
                     (unsigned long long)metrics.active_connections,
                     (unsigned long long)metrics.total_errors,
                     metrics.average_response_time_ms);
    if (n < 0) {
        respond_text(res, 500, "Internal Server Error", "failed to render metrics\n");
        return;
    }

    http_response_init(res);
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/plain");
    http_response_write_body(res, body, (size_t)n);
    http_response_end(res);
}
#endif

struct route_spec {
    const char *method;
    const char *path;
    http_handler_fn handler;
};

static void print_usage(const char *progname)
{
    fprintf(stderr, "usage: %s <host> <port>\n", progname);
    fprintf(stderr, "example: %s 127.0.0.1 9091\n", progname);
}

static int register_routes(void)
{
    static const struct route_spec routes[] = {
        {"GET",  "/health",    health_handler},
        {"GET",  "/echo",      echo_handler},
        {"POST", "/echo",      echo_handler},
        {"GET",  "/headers",   headers_handler},
        {"GET",  "/set_timer", timer_handler},
        {"GET",  "/ctx",       default_ctx_handler},
#ifdef HTTP_ENABLE_MONITORING
        {"GET",  "/metrics",   metrics_handler},
#endif
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        if (http_register_route_default(routes[i].method, routes[i].path, routes[i].handler, NULL) != 0) {
            fprintf(stderr, "failed to register route %s %s\n", routes[i].method, routes[i].path);
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    const char *host;
    const char *port;
    char listen_addr[256];

    if (argc != 3) {
        print_usage(argv[0]);
        return 1;
    }

    host = argv[1];
    port = argv[2];

    if (snprintf(listen_addr, sizeof(listen_addr), "%s:%s", host, port) >= (int)sizeof(listen_addr)) {
        fprintf(stderr, "listen address is too long\n");
        return 1;
    }

    signal(SIGINT, on_sigint);

    http_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.recv_buffer_size = 4096;
    cfg.send_buffer_size = 4096;
    cfg.max_connections = 32;
    cfg.keep_alive_timeout_ms = 10000;
    cfg.request_timeout_ms = 5000;
    cfg.enable_chunked = 1;
    cfg.log_fn = server_log_cb;

    g_ctx = http_init(&cfg);
    if (!g_ctx) {
        fprintf(stderr, "failed to initialize HTTP context\n");
        return 1;
    }

    http_set_default_ctx(g_ctx);

    if (register_routes() != 0) {
        http_free(g_ctx);
        return 1;
    }

    if (http_listen_default(listen_addr, NULL, NULL) != 0) {
        fprintf(stderr, "failed to listen on %s\n", listen_addr);
        http_free(g_ctx);
        return 1;
    }

    fprintf(stderr, "listening on http://%s:%s\n", host, port);
    fprintf(stderr, "routes: GET /health, GET|POST /echo, GET /headers, GET /set_timer, GET /ctx\n");
#ifdef HTTP_ENABLE_MONITORING
    fprintf(stderr, "routes: GET /metrics\n");
#endif

    http_run_default();
    http_free(g_ctx);
    return 0;
}

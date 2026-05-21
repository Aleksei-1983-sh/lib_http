
// -------------------- test_server_improved.c --------------------
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>

#include "http.h"

#define HTTP_HANDLER_FN http_handler_fn

static volatile sig_atomic_t stop_flag = 0;
static http_ctx_t *g_ctx = NULL;

// Таймерный callback
void timer_cb(void *user_data) {
    const char *msg = user_data ? (const char *)user_data : "(no data)";
    time_t now = time(NULL);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(stderr, "[TIMER] %s at %s\n", msg, ts);
}

void handle_sigint(int sig) {
    (void)sig;
    stop_flag = 1;
    if (g_ctx) {
        fprintf(stderr, "SIGINT received, shutting down...\n");
        http_stop(g_ctx);
#ifdef HTTP_ENABLE_MULTITHREADING
        http_stop_multithreaded(g_ctx);
#endif
    }
}

void server_log_cb(http_log_level_t level, void *user_data, const char *fmt, ...) {
    (void)user_data;
    va_list args;
    va_start(args, fmt);
    const char *levels[] = {"ERROR","WARN","INFO","DEBUG"};
    fprintf(stderr, "[%s] ", levels[level]);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

#ifdef HTTP_ENABLE_MONITORING
void metrics_cb(const http_metrics_t *metrics, void *user_data) {
    (void)user_data;
    fprintf(stderr,
        "[METRICS] req=%llu resp=%llu conn=%llu err=%llu avg=%.2f ms\n",
        (unsigned long long)metrics->total_requests,
        (unsigned long long)metrics->total_responses,
        (unsigned long long)metrics->active_connections,
        (unsigned long long)metrics->total_errors,
        metrics->average_response_time_ms);
}
#endif

static void url_test(const char *s) {
    char *e = http_url_encode(s);
    char *d = http_url_decode(e);
    fprintf(stderr, "URL test: '%s' -> '%s' -> '%s'\n", s, e, d);
    free(e); free(d);
}

static void json_test(const char *s) {
    char *e = http_escape_json_string(s);
    fprintf(stderr, "JSON test: '%s' -> '%s'\n", s, e);
    free(e);
}

void health_handler(http_request_t *req, http_response_t *res, void *ud) {
    (void)req; (void)ud;
    http_response_init(res);
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/plain");
    http_response_write_body(res, "OK", 2);
    http_response_end(res);
}

void echo_handler(http_request_t *req, http_response_t *res, void *ud) {
    (void)ud;
    http_response_init(res);
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/plain");
    if (strcmp(req->method, "GET") == 0) {
        const char *q = req->uri_query ? req->uri_query : "";
        char *d = http_url_decode(q);
        char buf[1024];
        snprintf(buf, sizeof(buf), "raw='%s', decoded='%s'", q, d);
        http_response_write_body(res, buf, strlen(buf));
        free(d);
    } else {
        http_response_write_body(res, "Received:\n", 10);
        if (req->body && req->body_len > 0)
            http_response_write_body(res, req->body, req->body_len);
    }
    http_response_end(res);
}

void headers_handler(http_request_t *req, http_response_t *res, void *ud) {
    (void)ud;
    http_response_init(res);
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/plain");
    for (size_t i = 0; i < req->num_headers; ++i) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s: %s\n",
                 req->headers[i].name, req->headers[i].value);
        http_response_write_body(res, buf, strlen(buf));
    }
    http_response_end(res);
}

void chunked_handler(http_request_t *req, http_response_t *res, void *ud) {
    (void)req; (void)ud;
    http_response_init(res);
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/plain");
    const char *parts[] = {"One\n","Two\n","Three\n"};
    for (int i = 0; i < 3; ++i) {
        http_response_write_body(res, parts[i], strlen(parts[i]));
        sleep(1);
    }
    http_response_end(res);
}

void utils_handler(http_request_t *req, http_response_t *res, void *ud) {
    (void)req; (void)ud;
    http_response_init(res);
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/plain");
    url_test("Hello World?&=/");
    json_test("He said: \"Hello\"\nNew line");
    const char *msg = "URL and JSON utils tested, check logs.";
    http_response_write_body(res, msg, strlen(msg));
    http_response_end(res);
}

void mime_handler(http_request_t *req, http_response_t *res, void *ud) {
    (void)req; (void)ud;
    http_response_init(res);
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/plain");
    const char *exts[] = {"html","jpg","txt","unknown_ext"};
    char buf[256];
    for (int i = 0; i < 4; ++i) {
        const char *m = http_mime_type_from_ext(exts[i]);
        int n = snprintf(buf, sizeof(buf), "Ext: %s => %s\n", exts[i], m);
        http_response_write_body(res, buf, n);
    }
    http_response_end(res);
}

#ifdef HTTP_ENABLE_MONITORING
void metrics_handler(http_request_t *req, http_response_t *res, void *ud) {
    (void)req; (void)ud;
    http_metrics_t m;
    http_response_init(res);
    if (http_get_metrics(g_ctx, &m) == 0) {
        http_response_set_status(res, 200, "OK");
        http_response_add_header(res, "Content-Type", "text/plain");
        char buf[512];
        int n = snprintf(buf, sizeof(buf),
            "total_requests=%llu, total_responses=%llu, active_connections=%llu, total_errors=%llu, avg_rt=%.2f ms\n",
            (unsigned long long)m.total_requests,
            (unsigned long long)m.total_responses,
            (unsigned long long)m.active_connections,
            (unsigned long long)m.total_errors,
            m.average_response_time_ms);
        http_response_write_body(res, buf, n);
    } else {
        http_response_set_status(res, 500, "Error");
        http_response_add_header(res, "Content-Type", "text/plain");
        const char *e = "Cannot fetch metrics";
        http_response_write_body(res, e, strlen(e));
    }
    http_response_end(res);
}
#endif

void timer_handler(http_request_t *req, http_response_t *res, void *ud) {
    (void)req; (void)ud;
    http_response_init(res);
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/plain");
    int delay_ms = 0;
    if (req->uri_query &&
        sscanf(req->uri_query, "delay=%d", &delay_ms) == 1 &&
        delay_ms > 0)
    {
        int tid = http_set_timer(g_ctx, delay_ms, 0, timer_cb, "Hello from timer");
        char buf[128];
        int n = snprintf(buf, sizeof(buf),
            "Timer scheduled: id=%d, delay=%d ms\n", tid, delay_ms);
        http_response_write_body(res, buf, n);
    } else {
        const char *err = "Usage: /set_timer?delay=ms";
        http_response_write_body(res, err, strlen(err));
    }
    http_response_end(res);
}

int main(void) {
    signal(SIGINT, handle_sigint);

    http_config_t cfg = {0};
    cfg.recv_buffer_size      = 4096;
    cfg.send_buffer_size      = 4096;
    cfg.max_connections       = 10;
    cfg.keep_alive_timeout_ms = 10000;
    cfg.request_timeout_ms    = 5000;
    cfg.log_fn                = NULL;
    cfg.enable_chunked        = 1;
#ifdef HTTP_ENABLE_MULTITHREADING
    cfg.thread_count          = 4;
#endif
#ifdef HTTP_ENABLE_MONITORING
    cfg.metrics_interval_ms   = 5000;
    cfg.metrics_cb            = metrics_cb;
#endif

    g_ctx = http_init(&cfg);
    if (!g_ctx) {
        fprintf(stderr, "Failed to initialize server\n");
        return 1;
    }

    struct {
        const char *method, *path;
        HTTP_HANDLER_FN handler;
    } routes[] = {
        {"GET",    "/health",     health_handler},
        {"GET",    "/echo",       echo_handler},
        {"POST",   "/echo",       echo_handler},
        {"GET",    "/headers",    headers_handler},
        {"GET",    "/chunked",    chunked_handler},
        {"GET",    "/url_utils",  utils_handler},
        {"GET",    "/json_utils", utils_handler},
        {"GET",    "/mime",       mime_handler},
#ifdef HTTP_ENABLE_MONITORING
        {"GET",    "/metrics",    metrics_handler},
#endif
        {"GET",    "/set_timer",  timer_handler},
    };

    for (size_t i = 0; i < sizeof(routes)/sizeof(routes[0]); ++i) {
        http_register_route(g_ctx,
            routes[i].method,
            routes[i].path,
            routes[i].handler,
            NULL);
    }

    const char *addr = "0.0.0.0:8080";
#ifdef HTTP_ENABLE_MULTITHREADING
    http_run_multithreaded(g_ctx, addr, NULL, NULL);
#else
    http_listen(g_ctx, addr, NULL, NULL);
    http_run(g_ctx);
#endif

    http_free(g_ctx);
    return 0;
}

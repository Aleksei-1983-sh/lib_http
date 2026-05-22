#include "http/http.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static http_ctx_t *g_ctx = NULL;
static const char *g_role = "unknown";

static void server_log_cb(http_log_level_t level, void *user_data, const char *fmt, ...)
{
    (void)user_data;
    static const char *names[] = {"ERROR", "WARN", "INFO", "DEBUG"};
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[%s][%s] ", g_role, names[level]);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

static void on_sigint(int sig)
{
    (void)sig;
    if (g_ctx) {
        fprintf(stderr, "[%s] stopping server\n", g_role);
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

static void role_handler(http_request_t *req, http_response_t *res, void *user_data)
{
    (void)req;
    (void)user_data;
    char body[128];
    snprintf(body, sizeof(body), "role=%s\n", g_role);
    respond_text(res, 200, "OK", body);
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

static void timer_log_cb(void *user_data)
{
    const char *message = user_data ? (const char *)user_data : "timer fired";
    fprintf(stderr, "[%s][timer] %s\n", g_role, message);
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
    snprintf(body, sizeof(body), "timer scheduled: id=%d delay_ms=%d\n", timer_id, delay_ms);
    respond_text(res, 200, "OK", body);
}

static int run_server_role(const char *role, const char *host, const char *port)
{
    char listen_addr[256];
    if (snprintf(listen_addr, sizeof(listen_addr), "%s:%s", host, port) >= (int)sizeof(listen_addr)) {
        fprintf(stderr, "[%s] listen address is too long\n", role);
        return 1;
    }

    g_role = role;
    signal(SIGINT, on_sigint);

    http_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (strcmp(role, "external") == 0) {
        cfg.recv_buffer_size = 8192;
        cfg.send_buffer_size = 8192;
        cfg.max_connections = 128;
        cfg.keep_alive_timeout_ms = 15000;
        cfg.request_timeout_ms = 7000;
    } else {
        cfg.recv_buffer_size = 2048;
        cfg.send_buffer_size = 2048;
        cfg.max_connections = 16;
        cfg.keep_alive_timeout_ms = 3000;
        cfg.request_timeout_ms = 2000;
    }
    cfg.enable_chunked = 1;
    cfg.log_fn = server_log_cb;

    g_ctx = http_init(&cfg);
    if (!g_ctx) {
        fprintf(stderr, "[%s] failed to initialize HTTP context\n", role);
        return 1;
    }

    if (http_register_route_default("GET", "/health", health_handler, NULL) != 0 ||
        http_register_route_default("GET", "/role", role_handler, NULL) != 0 ||
        http_register_route_default("GET", "/echo", echo_handler, NULL) != 0 ||
        http_register_route_default("POST", "/echo", echo_handler, NULL) != 0 ||
        http_register_route_default("GET", "/set_timer", timer_handler, NULL) != 0) {
        fprintf(stderr, "[%s] failed to register routes\n", role);
        http_free(g_ctx);
        return 1;
    }

    if (http_listen_default(listen_addr, NULL, NULL) != 0) {
        fprintf(stderr, "[%s] failed to listen on %s\n", role, listen_addr);
        http_free(g_ctx);
        return 1;
    }

    fprintf(stderr, "[%s] listening on http://%s:%s\n", role, host, port);
    fprintf(stderr, "[%s] routes: GET /health, GET /role, GET|POST /echo, GET /set_timer\n", role);
    http_run_default();
    http_free(g_ctx);
    return 0;
}

static void print_usage(const char *prog)
{
    fprintf(stderr, "usage: %s <host> <external_port> <internal_port>\n", prog);
    fprintf(stderr, "example: %s 127.0.0.1 19091 19092\n", prog);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        print_usage(argv[0]);
        return 1;
    }

    const char *host = argv[1];
    const char *external_port = argv[2];
    const char *internal_port = argv[3];

    pid_t internal_pid = fork();
    if (internal_pid < 0) {
        perror("fork internal");
        return 1;
    }
    if (internal_pid == 0) {
        return run_server_role("internal", host, internal_port);
    }

    int external_rc = run_server_role("external", host, external_port);

    kill(internal_pid, SIGINT);
    int status = 0;
    waitpid(internal_pid, &status, 0);

    return external_rc;
}

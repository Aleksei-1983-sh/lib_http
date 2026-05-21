/*
 * http.c - Core server lifecycle and event loop implementation.
 */

#include "http_internal.h"

static void default_log(http_log_level_t level, void *user_data, const char *fmt, ...)
{
    FILE *out = stderr;
    if (user_data) {
        out = (FILE *)user_data;
    }

    const char *lvl = "ERROR";
    if (level == HTTP_LOG_WARN) {
        lvl = "WARN";
    } else if (level == HTTP_LOG_INFO) {
        lvl = "INFO";
    } else if (level == HTTP_LOG_DEBUG) {
        lvl = "DEBUG";
    }

    if (level == HTTP_LOG_DEBUG) {
        fprintf(out, "[\033[0;32m%s\033[0m] ", lvl);
    } else if (level == HTTP_LOG_ERROR) {
        fprintf(out, "[\033[0;31m%s\033[0m] ", lvl);
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);
    fprintf(out, "\n");
}

void timeval_now(struct timeval *tv)
{
    gettimeofday(tv, NULL);
}

double timeval_diff_ms(const struct timeval *start, const struct timeval *end)
{
    double s = start->tv_sec + start->tv_usec / 1e6;
    double e = end->tv_sec + end->tv_usec / 1e6;
    return (e - s) * 1000.0;
}

void *http_malloc(http_ctx_t *ctx, size_t sz)
{
    if (ctx->config.malloc_fn) {
        return ctx->config.malloc_fn(sz);
    }
    return malloc(sz);
}

void http_free_mem(http_ctx_t *ctx, void *p)
{
    if (ctx->config.free_fn) {
        ctx->config.free_fn(p);
    } else {
        free(p);
    }
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    return 0;
}

static int create_and_bind(const char *address)
{
    char host[256] = {0};
    char serv[16] = {0};

    const char *p = strchr(address, ':');
    if (!p) {
        return -1;
    }

    size_t hlen = (size_t)(p - address);
    if (hlen >= sizeof(host)) {
        return -1;
    }
    if (hlen > 0) {
        memcpy(host, address, hlen);
        host[hlen] = '\0';
    }

    strncpy(serv, p + 1, sizeof(serv) - 1);
    serv[sizeof(serv) - 1] = '\0';

    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *rp = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int err = getaddrinfo(hlen ? host : NULL, serv, &hints, &res);
    if (err) {
        fprintf(stderr, "[ERROR] create_and_bind: getaddrinfo(%s, %s) failed: %s\n",
                host[0] ? host : "<any>", serv, gai_strerror(err));
        return -1;
    }

    int listen_fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        listen_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (listen_fd < 0) {
            continue;
        }

        int opt = 1;
        if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            fprintf(stderr, "[WARN] create_and_bind: setsockopt SO_REUSEADDR failed: %s\n",
                    strerror(errno));
        }

        if (bind(listen_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }

        fprintf(stderr, "[WARN] create_and_bind: bind(%s:%s) failed: %s\n",
                host[0] ? host : "0.0.0.0", serv, strerror(errno));
        close(listen_fd);
        listen_fd = -1;
    }

    freeaddrinfo(res);

    if (listen_fd < 0) {
        fprintf(stderr, "[ERROR] create_and_bind: unable to bind to %s\n", address);
        return -1;
    }

    return listen_fd;
}

static void accept_new(http_ctx_t *ctx, int listen_fd)
{
    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    int fd = accept(listen_fd, (struct sockaddr *)&ss, &slen);
    if (fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            HTTP_ERR(ctx, "accept() failed: %s", strerror(errno));
        }
        return;
    }

    if (set_nonblocking(fd) < 0) {
        HTTP_ERR(ctx, "set_nonblocking(fd=%d) failed: %s", fd, strerror(errno));
    }

    http_conn_t *c = http_malloc(ctx, sizeof(*c));
    if (!c) {
        close(fd);
        return;
    }
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->state = CONN_STATE_READING;
    dynbuf_init(&c->rb);
    dynbuf_init(&c->wb);
    timeval_now(&c->start_time);
    http_response_init(&c->res);

    char host[NI_MAXHOST];
    char serv[NI_MAXSERV];
    if (getnameinfo((struct sockaddr *)&ss, slen, host, sizeof(host),
                    serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        size_t len = strlen(host) + 1 + strlen(serv) + 1;
        char *pa = http_malloc(ctx, len);
        if (pa) {
            snprintf(pa, len, "%s:%s", host, serv);
            c->req.peer_addr = pa;
        }
    }

    c->next = ctx->conns;
    ctx->conns = c;
#ifdef HTTP_ENABLE_MONITORING
    ctx->metrics.active_connections++;
#endif
}

static void close_conn(http_ctx_t *ctx, http_conn_t *c_prev, http_conn_t *c)
{
    if (c_prev) {
        c_prev->next = c->next;
    } else {
        ctx->conns = c->next;
    }

    close(c->fd);
    dynbuf_free(&c->rb);
    dynbuf_free(&c->wb);
    if (c->req.headers) {
        http_free_mem(ctx, c->req.headers);
    }
    if (c->req.peer_addr) {
        http_free_mem(ctx, (void *)c->req.peer_addr);
    }
    http_response_free(&c->res);
#ifdef HTTP_ENABLE_MONITORING
    ctx->metrics.active_connections--;
#endif
    http_free_mem(ctx, c);
}

static void print_request(const http_request_t *req)
{
    if (!req) {
        return;
    }

    printf("=== HTTP Request ===\n");
    printf("Method: %s\n", req->method ? req->method : "<none>");
    printf("URI Path: %s\n", req->uri_path ? req->uri_path : "<none>");
    printf("Query: %s\n", req->uri_query ? req->uri_query : "<none>");
    printf("HTTP Version: %d.%d\n", req->http_major, req->http_minor);
    printf("Peer Address: %s\n", req->peer_addr ? req->peer_addr : "<unknown>");
    printf("\n-- Headers (%zu) --\n", req->num_headers);
    for (size_t i = 0; i < req->num_headers; ++i) {
        printf("%s: %s\n", req->headers[i].name, req->headers[i].value);
    }
    printf("\n-- Body (length: %zu) --\n", req->body_len);
    if (req->body && req->body_len > 0) {
        fwrite(req->body, 1, req->body_len, stdout);
        printf("\n");
    } else {
        printf("<empty>\n");
    }
}

static int handle_conn(http_ctx_t *ctx, http_conn_t *c)
{
    if (!ctx || !c) {
        HTTP_ERR(ctx, "ctx or connection is NULL");
        return -1;
    }

    if (c->state == CONN_STATE_READING) {
        char buf[4096];
        ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            HTTP_ERR(ctx, "recv error on fd=%d: %s", c->fd, strerror(errno));
            return -1;
        }
        if (n == 0) {
            return -1;
        }

        if (dynbuf_append(&c->rb, buf, (size_t)n) < 0) {
            return -1;
        }

        int pr = parse_request(ctx, c);
        if (pr < 0) {
            return -1;
        }
        if (pr == 1) {
            print_request(&c->req);
#ifdef HTTP_ENABLE_MONITORING
            ctx->metrics.total_requests++;
#endif
            int found = 0;
            for (size_t i = 0; i < ctx->route_count; i++) {
                if ((ctx->routes[i].method == NULL ||
                     strcasecmp(ctx->routes[i].method, c->req.method) == 0) &&
                    strcmp(ctx->routes[i].pattern, c->req.uri_path) == 0) {
                    http_response_init(&c->res);
                    if (c->res.internal) {
                        http_response_internal_t *ri = (http_response_internal_t *)c->res.internal;
                        ri->conn = c;
                    }
                    ctx->routes[i].handler(&c->req, &c->res, ctx->routes[i].user_data);
                    found = 1;
                    break;
                }
            }

            if (!found) {
                http_response_init(&c->res);
                if (c->res.internal) {
                    http_response_internal_t *ri = (http_response_internal_t *)c->res.internal;
                    ri->conn = c;
                }
                http_response_set_status(&c->res, 404, "Not Found");
                http_response_add_header(&c->res, "Content-Type", "text/plain");
                http_response_write_body(&c->res, "Not Found", strlen("Not Found"));
            }

            if (prepare_response(ctx, c) < 0) {
                return -1;
            }
        }
    }

    if (c->state == CONN_STATE_WRITING) {
        return send_response(ctx, c);
    }

    return 0;
}

http_ctx_t *http_init(const http_config_t *config)
{
    fprintf(stderr, "[DEBUG] http_init: Inception\n");

    http_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        fprintf(stderr, "[ERROR] http_init: failed to allocate http_ctx_t\n");
        return NULL;
    }
    memset(ctx, 0, sizeof(*ctx));

    if (config) {
        memcpy(&ctx->config, config, sizeof(ctx->config));
    }
    if (!ctx->config.recv_buffer_size) {
        ctx->config.recv_buffer_size = 4096;
    }
    if (!ctx->config.send_buffer_size) {
        ctx->config.send_buffer_size = 4096;
    }
    if (!ctx->config.log_fn) {
        ctx->config.log_fn = default_log;
    }

    ctx->routes = malloc(sizeof(route_entry_t) * INITIAL_ROUTE_CAPACITY);
    if (!ctx->routes) {
        free(ctx);
        return NULL;
    }
    ctx->route_cap = INITIAL_ROUTE_CAPACITY;
    ctx->next_timer_id = 1;

#ifdef HTTP_ENABLE_MONITORING
    memset(&ctx->metrics, 0, sizeof(ctx->metrics));
#endif

#ifdef HTTP_ENABLE_MULTITHREADING
    if (ctx->config.thread_count <= 0) {
        ctx->config.thread_count = 1;
    }
#endif

    return ctx;
}

void http_free(http_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    for (int i = 0; i < ctx->listen_count; i++) {
        close(ctx->listen_fds[i]);
    }

    while (ctx->conns) {
        close_conn(ctx, NULL, ctx->conns);
    }

    for (size_t i = 0; i < ctx->route_count; i++) {
        free(ctx->routes[i].method);
        free(ctx->routes[i].pattern);
    }
    free(ctx->routes);

    http_timer_t *t = ctx->timers;
    while (t) {
        http_timer_t *n = t->next;
        free(t);
        t = n;
    }

    free(ctx);
}

int http_listen(http_ctx_t *ctx, const char *address, http_handler_fn handler, void *user_data)
{
    if (!ctx) {
        fprintf(stderr, "[ERROR] http_listen: ctx is NULL\n");
        return -1;
    }
    if (ctx->listen_count >= MAX_LISTENERS) {
        return -1;
    }

    const char *addr = address && *address ? address : "0.0.0.0:80";
    int fd = create_and_bind(addr);
    if (fd < 0) {
        return -1;
    }
    if (listen(fd, SOMAXCONN) < 0) {
        close(fd);
        return -1;
    }
    if (set_nonblocking(fd) < 0) {
        HTTP_ERR(ctx, "set_nonblocking(fd=%d) failed: %s", fd, strerror(errno));
    }

    ctx->listen_fds[ctx->listen_count++] = fd;
    if (http_register_route(ctx, NULL, "*", handler, user_data) < 0) {
        close(fd);
        ctx->listen_count--;
        return -1;
    }

    return 0;
}

#ifdef HTTP_ENABLE_TLS
int http_listen_https(http_ctx_t *ctx,
                      const char *address __attribute__((unused)),
                      http_handler_fn handler __attribute__((unused)),
                      void *user_data __attribute__((unused)))
{
    ctx->config.log_fn(HTTP_LOG_WARN, ctx->config.log_user_data, "HTTPS not implemented");
    return -1;
}
#endif

int http_run(http_ctx_t *ctx)
{
    ctx->stopped = 0;
    while (!ctx->stopped) {
        int timeout = compute_poll_timeout(ctx, 1000);
        int rc = http_poll(ctx, timeout);
        if (rc < 0) {
            continue;
        }
        timers_check(ctx);
    }
    return 0;
}

void http_stop(http_ctx_t *ctx)
{
    ctx->stopped = 1;
}

int http_poll(http_ctx_t *ctx, int timeout_ms)
{
    int ret = -1;
    size_t total_fds = ctx->listen_count;
    for (http_conn_t *c = ctx->conns; c; c = c->next) {
        total_fds++;
    }

    struct pollfd *pfds = malloc(sizeof(*pfds) * total_fds);
    if (!pfds) {
        return -1;
    }

    size_t idx = 0;
    for (int i = 0; i < ctx->listen_count; i++) {
        pfds[idx].fd = ctx->listen_fds[i];
        pfds[idx].events = POLLIN;
        pfds[idx].revents = 0;
        idx++;
    }

    for (http_conn_t *c = ctx->conns; c; c = c->next) {
        pfds[idx].fd = c->fd;
        pfds[idx].events = 0;
        if (c->state == CONN_STATE_READING) {
            pfds[idx].events |= POLLIN;
        }
        if (c->state == CONN_STATE_WRITING) {
            pfds[idx].events |= POLLOUT;
        }
        pfds[idx].revents = 0;
        idx++;
    }

    ret = poll(pfds, total_fds, timeout_ms);
    if (ret < 0) {
        free(pfds);
        return ret;
    }

    idx = 0;
    for (int i = 0; i < ctx->listen_count; i++, idx++) {
        if (pfds[idx].revents & POLLIN) {
            accept_new(ctx, pfds[idx].fd);
        }
    }

    http_conn_t *prev = NULL;
    http_conn_t *cur = ctx->conns;
    for (; cur;) {
        short re = pfds[idx].revents;
        http_conn_t *next = cur->next;

        if (re & (POLLIN | POLLOUT | POLLHUP | POLLERR)) {
            int rc = handle_conn(ctx, cur);
            if (rc < 0 || rc == 1) {
                close_conn(ctx, prev, cur);
                cur = next;
                idx++;
                continue;
            }
        }

        prev = cur;
        cur = next;
        idx++;
    }

    free(pfds);
    return ret;
}

#ifdef HTTP_ENABLE_MULTITHREADING
int http_run_multithreaded(http_ctx_t *ctx,
                           const char *address __attribute__((unused)),
                           http_handler_fn handler __attribute__((unused)),
                           void *user_data __attribute__((unused)))
{
    ctx->config.log_fn(HTTP_LOG_WARN, ctx->config.log_user_data,
                       "Multithreading not implemented fully");
    return -1;
}

void http_stop_multithreaded(http_ctx_t *ctx)
{
    http_stop(ctx);
}
#endif

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
        HTTP_ERR(NULL, "set_nonblocking: fcntl(F_GETFL) failed for fd=%d: %s",
                 fd, strerror(errno));
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        HTTP_ERR(NULL, "set_nonblocking: fcntl(F_SETFL) failed for fd=%d: %s",
                 fd, strerror(errno));
        return -1;
    }
    return 0;
}

static int create_and_bind(http_ctx_t *ctx, const char *address)
{
    char host[256] = {0};
    char serv[16] = {0};

    const char *p = strchr(address, ':');
    if (!p) {
        HTTP_ERR(ctx, "address '%s' must be in host:port format", address);
        return -1;
    }

    size_t hlen = (size_t)(p - address);
    if (hlen >= sizeof(host)) {
        HTTP_ERR(ctx, "host part is too long in address '%s'", address);
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
        HTTP_ERR(ctx, "getaddrinfo(%s, %s) failed: %s",
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
            HTTP_DBG(ctx, "setsockopt SO_REUSEADDR failed: %s",
                     strerror(errno));
        }

        if (bind(listen_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            HTTP_DBG(ctx, "bound %s successfully", address);
            break;
        }

        HTTP_DBG(ctx, "bind(%s:%s) failed: %s",
                 host[0] ? host : "0.0.0.0", serv, strerror(errno));
        close(listen_fd);
        listen_fd = -1;
    }

    freeaddrinfo(res);

    if (listen_fd < 0) {
        HTTP_ERR(ctx, "unable to bind to %s", address);
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
    HTTP_DBG(ctx, "accept_new: accepted fd=%d on listener=%d", fd, listen_fd);

    if (set_nonblocking(fd) < 0) {
        HTTP_ERR(ctx, "set_nonblocking(fd=%d) failed: %s", fd, strerror(errno));
    }

    http_conn_t *c = http_malloc(ctx, sizeof(*c));
    if (!c) {
        HTTP_ERR(ctx, "accept_new: failed to allocate connection for fd=%d", fd);
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
            HTTP_DBG(ctx, "accept_new: peer=%s", pa);
        }
    }

    c->next = ctx->conns;
    ctx->conns = c;
#ifdef HTTP_ENABLE_MONITORING
    ctx->metrics.active_connections++;
#endif
    HTTP_DBG(ctx, "accept_new: connection fd=%d added", fd);
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
    HTTP_DBG(ctx, "close_conn: closing fd=%d", c->fd);
    http_free_mem(ctx, c);
}

static void print_request(http_ctx_t *ctx, const http_request_t *req)
{
    (void)ctx;
    if (!req) {
        return;
    }

    HTTP_DBG(ctx, "request: method=%s path=%s query=%s http=%d.%d peer=%s",
             req->method ? req->method : "<none>",
             req->uri_path ? req->uri_path : "<none>",
             req->uri_query ? req->uri_query : "<none>",
             req->http_major,
             req->http_minor,
             req->peer_addr ? req->peer_addr : "<unknown>");
    HTTP_DBG(ctx, "request: headers=%zu body_len=%zu", req->num_headers, req->body_len);
    for (size_t i = 0; i < req->num_headers; ++i) {
        HTTP_DBG(ctx, "request header[%zu]: %s: %s",
                 i, req->headers[i].name, req->headers[i].value);
    }
    if (req->body && req->body_len > 0) {
        size_t preview_len = req->body_len > 256 ? 256 : req->body_len;
        char preview[257];
        memcpy(preview, req->body, preview_len);
        preview[preview_len] = '\0';
        HTTP_DBG(ctx, "request body preview: \"%s\"%s",
                 preview, req->body_len > preview_len ? "..." : "");
    } else {
        HTTP_DBG(ctx, "request body: <empty>");
    }
}

static void init_response_for_conn(http_conn_t *c)
{
    http_response_init(&c->res);
    if (c->res.internal) {
        http_response_internal_t *ri = (http_response_internal_t *)c->res.internal;
        ri->conn = c;
    }
}

static int queue_error_response(http_ctx_t *ctx, http_conn_t *c, int status, const char *reason, const char *body)
{
    init_response_for_conn(c);
    c->keep_alive = 0;
    http_response_set_status(&c->res, status, reason);
    http_response_add_header(&c->res, "Content-Type", "text/plain");
    if (body) {
        if (http_response_write_body(&c->res, body, strlen(body)) < 0) {
            HTTP_ERR(ctx, "queue_error_response: failed to write error body status=%d fd=%d",
                     status, c ? c->fd : -1);
            return -1;
        }
    }
    if (prepare_response(ctx, c) < 0) {
        HTTP_ERR(ctx, "queue_error_response: prepare_response failed status=%d fd=%d",
                 status, c ? c->fd : -1);
        return -1;
    }
    return 0;
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
            HTTP_DBG(ctx, "handle_conn: peer closed connection fd=%d", c->fd);
            return -1;
        }

        if (dynbuf_append(&c->rb, buf, (size_t)n) < 0) {
            HTTP_ERR(ctx, "handle_conn: failed to append %zd bytes to read buffer fd=%d",
                     n, c->fd);
            return -1;
        }

        int pr = parse_request(ctx, c);
        if (pr == HTTP_PARSE_BAD_REQUEST) {
            HTTP_DBG(ctx, "handle_conn: malformed request on fd=%d", c->fd);
            return queue_error_response(ctx, c, 400, "Bad Request", "Bad Request");
        }
        if (pr < 0) {
            HTTP_ERR(ctx, "handle_conn: parse_request failed on fd=%d", c->fd);
            return -1;
        }
        if (pr == HTTP_PARSE_OK) {
            print_request(ctx, &c->req);
#ifdef HTTP_ENABLE_MONITORING
            ctx->metrics.total_requests++;
#endif
            int found = 0;
            int path_found = 0;
            for (size_t i = 0; i < ctx->route_count; i++) {
                if (strcmp(ctx->routes[i].pattern, c->req.uri_path) != 0) {
                    continue;
                }

                path_found = 1;
                if (ctx->routes[i].method != NULL &&
                    strcasecmp(ctx->routes[i].method, c->req.method) != 0) {
                    continue;
                }

                init_response_for_conn(c);
                HTTP_DBG(ctx, "handle_conn: route matched method=%s path=%s",
                         c->req.method, c->req.uri_path);
                ctx->routes[i].handler(&c->req, &c->res, ctx->routes[i].user_data);
                found = 1;
                break;
            }

            if (!found) {
                if (path_found) {
                    HTTP_DBG(ctx, "handle_conn: method not allowed method=%s path=%s",
                             c->req.method, c->req.uri_path);
                    if (queue_error_response(ctx, c, 405, "Method Not Allowed", "Method Not Allowed") < 0) {
                        return -1;
                    }
                } else {
                    HTTP_DBG(ctx, "handle_conn: route not found path=%s",
                             c->req.uri_path ? c->req.uri_path : "<none>");
                    if (queue_error_response(ctx, c, 404, "Not Found", "Not Found") < 0) {
                        return -1;
                    }
                }
            } else if (prepare_response(ctx, c) < 0) {
                HTTP_ERR(ctx, "handle_conn: prepare_response failed on fd=%d", c->fd);
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
    HTTP_DBG(NULL, "http_init: Inception");

    http_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        HTTP_ERR(NULL, "http_init: failed to allocate http_ctx_t");
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
    HTTP_DBG(ctx, "http_init: buffers recv=%zu send=%zu",
             ctx->config.recv_buffer_size, ctx->config.send_buffer_size);

    ctx->routes = malloc(sizeof(route_entry_t) * INITIAL_ROUTE_CAPACITY);
    if (!ctx->routes) {
        HTTP_ERR(ctx, "http_init: failed to allocate routes array");
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

    HTTP_DBG(ctx, "http_init: initialization complete");

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
        HTTP_ERR(NULL, "http_listen: ctx is NULL");
        return -1;
    }
    if (ctx->listen_count >= MAX_LISTENERS) {
        HTTP_ERR(ctx, "http_listen: too many listeners");
        return -1;
    }

    const char *addr = address && *address ? address : "0.0.0.0:80";
    HTTP_DBG(ctx, "http_listen: address=%s", addr);
    int fd = create_and_bind(ctx, addr);
    if (fd < 0) {
        HTTP_ERR(ctx, "create_and_bind failed for address=%s", addr);
        return -1;
    }
    if (listen(fd, SOMAXCONN) < 0) {
        HTTP_ERR(ctx, "listen failed on fd=%d: %s", fd, strerror(errno));
        close(fd);
        return -1;
    }
    if (set_nonblocking(fd) < 0) {
        HTTP_ERR(ctx, "set_nonblocking(fd=%d) failed: %s", fd, strerror(errno));
    }

    ctx->listen_fds[ctx->listen_count++] = fd;
    HTTP_DBG(ctx, "listener fd=%d registered", fd);
    if (http_register_route(ctx, NULL, "*", handler, user_data) < 0) {
        HTTP_ERR(ctx, "failed to register catch-all route");
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
    HTTP_ERR(ctx, "HTTPS/TLS support is not implemented");
    return -1;
}
#endif

int http_run(http_ctx_t *ctx)
{
    ctx->stopped = 0;
    HTTP_DBG(ctx, "http_run: enter");
    while (!ctx->stopped) {
        int timeout = compute_poll_timeout(ctx, 1000);
        int rc = http_poll(ctx, timeout);
        if (rc < 0) {
            HTTP_ERR(ctx, "http_poll returned %d", rc);
            continue;
        }
        timers_check(ctx);
    }
    HTTP_DBG(ctx, "http_run: exit");
    return 0;
}

void http_stop(http_ctx_t *ctx)
{
    HTTP_DBG(ctx, "stopping event loop");
    ctx->stopped = 1;
}

int http_poll(http_ctx_t *ctx, int timeout_ms)
{
    int ret = -1;
    HTTP_DBG(ctx, "timeout=%d", timeout_ms);
    size_t total_fds = ctx->listen_count;
    for (http_conn_t *c = ctx->conns; c; c = c->next) {
        total_fds++;
    }

    struct pollfd *pfds = malloc(sizeof(*pfds) * total_fds);
    if (!pfds) {
        HTTP_ERR(ctx, "failed to allocate pollfd array");
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
        HTTP_ERR(ctx, "poll failed: %s", strerror(errno));
        free(pfds);
        return ret;
    }
    HTTP_DBG(ctx, "ready_fds=%d", ret);

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
            HTTP_DBG(ctx, "fd=%d revents=0x%x", cur->fd, re);
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
    HTTP_DBG(ctx, "exit ret=%d", ret);
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
    HTTP_ERR(ctx, "multithreading support is not implemented");
    return -1;
}

void http_stop_multithreaded(http_ctx_t *ctx)
{
    http_stop(ctx);
}
#endif

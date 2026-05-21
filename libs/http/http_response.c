#include "http_internal.h"

int send_response(http_ctx_t *ctx, http_conn_t *c)
{
    int ret = 0;

    if (!ctx || !c) {
        HTTP_ERR(ctx, "ctx or connection is NULL");
        return -1;
    }

    if (c->state != CONN_STATE_WRITING) {
        return 0;
    }

    while (c->wb_sent < c->wb.len) {
        ssize_t n = send(c->fd, c->wb.data + c->wb_sent, c->wb.len - c->wb_sent, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            HTTP_ERR(ctx, "send() failed: %s", strerror(errno));
            return -1;
        }
        c->wb_sent += (size_t)n;
    }

    if (c->res.internal) {
        http_response_internal_t *ri = (http_response_internal_t *)c->res.internal;
        if (ri->bodybuf.data) {
            dynbuf_free(&ri->bodybuf);
        }
        free(ri);
        c->res.internal = NULL;
    }

#ifdef HTTP_ENABLE_MONITORING
    {
        struct timeval now;
        timeval_now(&now);
        double dur = timeval_diff_ms(&c->start_time, &now);
        ctx->total_response_time_ms += dur;
        ctx->metrics.total_responses++;
        ctx->metrics.average_response_time_ms = ctx->metrics.total_responses
            ? ctx->total_response_time_ms / ctx->metrics.total_responses
            : 0;
    }
#endif

    if (c->keep_alive) {
        dynbuf_free(&c->rb);
        dynbuf_init(&c->rb);
        c->header_parsed = 0;
        c->content_length = 0;

        if (c->req.method) {
            free((void *)c->req.method);
        }
        if (c->req.uri_path) {
            free((void *)c->req.uri_path);
        }
        if (c->req.uri_query) {
            free((void *)c->req.uri_query);
        }
        for (size_t i = 0; i < c->req.num_headers; i++) {
            free((void *)c->req.headers[i].name);
            free((void *)c->req.headers[i].value);
        }
        free(c->req.headers);
        if (c->req.body) {
            free((void *)c->req.body);
        }

        c->req.method = NULL;
        c->req.uri_path = NULL;
        c->req.uri_query = NULL;
        c->req.headers = NULL;
        c->req.num_headers = 0;
        c->req.body = NULL;
        c->req.body_len = 0;

        dynbuf_free(&c->wb);
        dynbuf_init(&c->wb);
        c->wb_sent = 0;
        c->state = CONN_STATE_READING;
        timeval_now(&c->start_time);
        return 0;
    }

    ret = 1;
    return ret;
}

int prepare_response(http_ctx_t *ctx, http_conn_t *c)
{
    if (!ctx || !c) {
        HTTP_ERR(ctx, "ctx or connection is NULL");
        return -1;
    }

    char line[64];
    const char *reason = c->res.status_reason;
    if (!reason) {
        switch (c->res.status_code) {
            case 200: reason = "OK"; break;
            case 400: reason = "Bad Request"; break;
            case 404: reason = "Not Found"; break;
            case 500: reason = "Internal Server Error"; break;
            default: reason = ""; break;
        }
    }

    int n = snprintf(line, sizeof(line), "HTTP/1.1 %d %s\r\n", c->res.status_code, reason);
    if (n < 0 || (size_t)n >= sizeof(line)) {
        HTTP_ERR(ctx, "failed to render status line for code=%d",
                 c->res.status_code);
        return -1;
    }
    if (dynbuf_append(&c->wb, line, (size_t)n) < 0) {
        HTTP_ERR(ctx, "failed to append status line");
        return -1;
    }

    int has_length = 0;
    for (size_t i = 0; i < c->res.num_resp_headers; i++) {
        const char *name = c->res.resp_headers[i].name;
        const char *value = c->res.resp_headers[i].value;
        if (!name || !value) {
            continue;
        }

        char buf[1024];
        int rn = snprintf(buf, sizeof(buf), "%s: %s\r\n", name, value);
        if (rn < 0 || (size_t)rn >= sizeof(buf)) {
            HTTP_ERR(ctx, "failed to render header '%s'", name);
            return -1;
        }
        if (dynbuf_append(&c->wb, buf, (size_t)rn) < 0) {
            HTTP_ERR(ctx, "failed to append header '%s'", name);
            return -1;
        }
        if (strcasecmp(name, "Content-Length") == 0 ||
            strcasecmp(name, "Transfer-Encoding") == 0) {
            has_length = 1;
        }
    }

    size_t body_len = 0;
    http_response_internal_t *ri = NULL;
    if (c->res.internal) {
        ri = (http_response_internal_t *)c->res.internal;
        body_len = ri->bodybuf.len;
    }

    if (!has_length) {
        char buf[64];
        int rn = snprintf(buf, sizeof(buf), "Content-Length: %zu\r\n", body_len);
        if (rn < 0 || (size_t)rn >= sizeof(buf)) {
            HTTP_ERR(ctx, "failed to render Content-Length");
            return -1;
        }
        if (dynbuf_append(&c->wb, buf, (size_t)rn) < 0) {
            HTTP_ERR(ctx, "failed to append Content-Length");
            return -1;
        }
    }

    const char *conn_hdr = c->keep_alive
        ? "Connection: keep-alive\r\n"
        : "Connection: close\r\n";
    if (dynbuf_append(&c->wb, conn_hdr, strlen(conn_hdr)) < 0) {
        HTTP_ERR(ctx, "failed to append Connection header");
        return -1;
    }
    if (dynbuf_append(&c->wb, "\r\n", 2) < 0) {
        HTTP_ERR(ctx, "failed to append header/body separator");
        return -1;
    }

    if (body_len > 0 && ri && ri->bodybuf.data) {
        if (dynbuf_append(&c->wb, ri->bodybuf.data, body_len) < 0) {
            HTTP_ERR(ctx, "failed to append body len=%zu", body_len);
            return -1;
        }
    }

    if (ri) {
        dynbuf_free(&ri->bodybuf);
        ri->bodybuf.data = NULL;
        ri->bodybuf.len = 0;
        ri->bodybuf.cap = 0;
    }

    c->state = CONN_STATE_WRITING;
    c->wb_sent = 0;
    return 0;
}

void http_response_init(http_response_t *res)
{
    if (!res) {
        HTTP_ERR(NULL, "response pointer is NULL");
        return;
    }

    res->status_code = 0;
    res->status_reason = NULL;

    if (res->resp_headers) {
        for (size_t i = 0; i < res->num_resp_headers; i++) {
            free(res->resp_headers[i].name);
            free(res->resp_headers[i].value);
        }
        free(res->resp_headers);
    }
    res->resp_headers = NULL;
    res->num_resp_headers = 0;

    if (res->internal) {
        http_response_internal_t *old = (http_response_internal_t *)res->internal;
        dynbuf_free(&old->bodybuf);
        free(old);
        res->internal = NULL;
    }

    http_response_internal_t *ri = malloc(sizeof(*ri));
    if (!ri) {
        HTTP_ERR(NULL, "failed to allocate internal response state");
        return;
    }
    ri->conn = NULL;
    dynbuf_init(&ri->bodybuf);
    ri->headers_sent = 0;
    res->internal = ri;
}

void http_response_set_status(http_response_t *res, int status_code, const char *reason)
{
    res->status_code = status_code;
    res->status_reason = reason;
}

void http_response_add_header(http_response_t *res, const char *name, const char *value)
{
    size_t n = res->num_resp_headers;
    void *tmp = realloc(res->resp_headers, sizeof(*res->resp_headers) * (n + 1));
    if (!tmp) {
        HTTP_ERR(NULL, "realloc failed");
        return;
    }
    res->resp_headers = tmp;
    res->resp_headers[n].name = strdup(name);
    res->resp_headers[n].value = strdup(value);
    if (!res->resp_headers[n].name || !res->resp_headers[n].value) {
        HTTP_ERR(NULL, "failed to copy header '%s'",
                 name ? name : "<null>");
    }
    res->num_resp_headers++;
}

int http_response_write_body(http_response_t *res, const void *data, size_t len)
{
    if (!res) {
        HTTP_ERR(NULL, "response pointer is NULL");
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (!data) {
        HTTP_ERR(NULL, "data is NULL while len=%zu", len);
        return -1;
    }

    http_response_internal_t *ri = (http_response_internal_t *)res->internal;
    if (!ri) {
        HTTP_ERR(NULL, "internal response state is NULL");
        return -1;
    }

    if (dynbuf_append(&ri->bodybuf, data, len) < 0) {
        HTTP_ERR(NULL, "failed to append body len=%zu", len);
        return -1;
    }
    return 0;
}

int http_response_end(http_response_t *res)
{
    if (!res) {
        HTTP_ERR(NULL, "response pointer is NULL");
        return -1;
    }
    if (!res->internal) {
        HTTP_ERR(NULL, "internal response state is NULL");
        return 0;
    }
    return 0;
}

void http_response_free(http_response_t *res)
{
    if (!res) {
        return;
    }

    if (res->resp_headers) {
        for (size_t i = 0; i < res->num_resp_headers; i++) {
            free(res->resp_headers[i].name);
            free(res->resp_headers[i].value);
        }
        free(res->resp_headers);
        res->resp_headers = NULL;
        res->num_resp_headers = 0;
    }

    if (res->internal) {
        http_response_internal_t *ri = (http_response_internal_t *)res->internal;
        dynbuf_free(&ri->bodybuf);
        free(ri);
        res->internal = NULL;
    }
}

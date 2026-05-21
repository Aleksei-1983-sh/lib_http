#include "http_internal.h"

void timers_add(http_ctx_t *ctx, http_timer_t *timer)
{
    if (!ctx->timers || timercmp(&timer->due, &ctx->timers->due, <)) {
        timer->next = ctx->timers;
        ctx->timers = timer;
    } else {
        http_timer_t *p = ctx->timers;
        while (p->next && timercmp(&p->next->due, &timer->due, <)) {
            p = p->next;
        }
        timer->next = p->next;
        p->next = timer;
    }
}

void timers_remove(http_ctx_t *ctx, int timer_id)
{
    http_timer_t *p = ctx->timers;
    http_timer_t *prev = NULL;
    while (p) {
        if (p->id == timer_id) {
            if (prev) {
                prev->next = p->next;
            } else {
                ctx->timers = p->next;
            }
            free(p);
            return;
        }
        prev = p;
        p = p->next;
    }
}

void timers_check(http_ctx_t *ctx)
{
    struct timeval now;
    timeval_now(&now);

    while (ctx->timers && timercmp(&ctx->timers->due, &now, <=)) {
        http_timer_t *timer = ctx->timers;
        ctx->timers = timer->next;
        timer->cb(timer->user_data);

        if (timer->interval_ms > 0) {
            struct timeval next_due;
            timeval_now(&next_due);

            long sec = timer->interval_ms / 1000;
            long usec = (timer->interval_ms % 1000) * 1000;
            next_due.tv_sec += sec;
            next_due.tv_usec += usec;
            if (next_due.tv_usec >= 1000000) {
                next_due.tv_sec += 1;
                next_due.tv_usec -= 1000000;
            }
            timer->due = next_due;
            timers_add(ctx, timer);
            timeval_now(&now);
        } else {
            free(timer);
            timeval_now(&now);
        }
    }
}

int compute_poll_timeout(http_ctx_t *ctx, int default_ms)
{
    if (!ctx->timers) {
        return default_ms;
    }

    struct timeval now;
    timeval_now(&now);
    struct timeval due = ctx->timers->due;
    long ms = (due.tv_sec - now.tv_sec) * 1000 + (due.tv_usec - now.tv_usec) / 1000;
    if (ms < 0) {
        return 0;
    }
    return (int)ms;
}

int http_register_route(http_ctx_t *ctx, const char *method, const char *route_pattern, http_handler_fn handler, void *user_data)
{
    if (ctx->route_count >= ctx->route_cap) {
        size_t ncap = ctx->route_cap * 2;
        route_entry_t *nr = realloc(ctx->routes, sizeof(route_entry_t) * ncap);
        if (!nr) {
            return -1;
        }
        ctx->routes = nr;
        ctx->route_cap = ncap;
    }

    char *m = method ? strdup(method) : strdup("*");
    char *p = strdup(route_pattern);
    if (!m || !p) {
        free(m);
        free(p);
        return -1;
    }

    ctx->routes[ctx->route_count].method = m;
    ctx->routes[ctx->route_count].pattern = p;
    ctx->routes[ctx->route_count].handler = handler;
    ctx->routes[ctx->route_count].user_data = user_data;
    ctx->route_count++;
    return 0;
}

char *http_url_encode(const char *src)
{
    if (!src) {
        return NULL;
    }
    size_t len = strlen(src);
    char *out = malloc(len * 3 + 1);
    if (!out) {
        return NULL;
    }

    char *p = out;
    for (; *src; src++) {
        unsigned char c = (unsigned char)*src;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            *p++ = (char)c;
        } else {
            sprintf(p, "%%%02X", c);
            p += 3;
        }
    }
    *p = '\0';
    return out;
}

char *http_url_decode(const char *src)
{
    if (!src) {
        return NULL;
    }
    size_t len = strlen(src);
    char *out = malloc(len + 1);
    if (!out) {
        return NULL;
    }

    char *p = out;
    for (; *src; src++) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = {src[1], src[2], '\0'};
            *p++ = (char)strtol(hex, NULL, 16);
            src += 2;
        } else if (*src == '+') {
            *p++ = ' ';
        } else {
            *p++ = *src;
        }
    }
    *p = '\0';
    return out;
}

char *http_escape_json_string(const char *src)
{
    if (!src) {
        return NULL;
    }
    size_t len = strlen(src);
    char *out = malloc(len * 6 + 1);
    if (!out) {
        return NULL;
    }

    char *p = out;
    for (; *src; src++) {
        unsigned char c = (unsigned char)*src;
        switch (c) {
            case '"': *p++ = '\\'; *p++ = '"'; break;
            case '\\': *p++ = '\\'; *p++ = '\\'; break;
            case '\b': *p++ = '\\'; *p++ = 'b'; break;
            case '\f': *p++ = '\\'; *p++ = 'f'; break;
            case '\n': *p++ = '\\'; *p++ = 'n'; break;
            case '\r': *p++ = '\\'; *p++ = 'r'; break;
            case '\t': *p++ = '\\'; *p++ = 't'; break;
            default:
                if (c < 0x20) {
                    sprintf(p, "\\u%04x", c);
                    p += 6;
                } else {
                    *p++ = (char)c;
                }
        }
    }
    *p = '\0';
    return out;
}

int http_set_timer(http_ctx_t *ctx, int delay_ms, int interval_ms, http_timer_fn cb, void *user_data)
{
    http_timer_t *timer = malloc(sizeof(*timer));
    if (!timer) {
        HTTP_ERR(ctx, "failed to allocate timer");
        return -1;
    }
    memset(timer, 0, sizeof(*timer));

    if (interval_ms < 0) {
        interval_ms = 0;
    }

    timer->id = ctx->next_timer_id++;

    struct timeval now;
    timeval_now(&now);

    long sec = delay_ms / 1000;
    long usec = (delay_ms % 1000) * 1000;
    now.tv_sec += sec;
    now.tv_usec += usec;
    if (now.tv_usec >= 1000000) {
        now.tv_sec += 1;
        now.tv_usec -= 1000000;
    }

    timer->due = now;
    timer->interval_ms = interval_ms;
    timer->cb = cb;
    timer->user_data = user_data;
    timer->next = NULL;
    timers_add(ctx, timer);
    return timer->id;
}

void http_cancel_timer(http_ctx_t *ctx, int timer_id)
{
    timers_remove(ctx, timer_id);
}

#ifdef HTTP_ENABLE_MONITORING
int http_get_metrics(http_ctx_t *ctx, http_metrics_t *out_metrics)
{
    if (!ctx || !out_metrics) {
        return -1;
    }
    *out_metrics = ctx->metrics;
    return 0;
}

int http_reset_metrics(http_ctx_t *ctx)
{
    if (!ctx) {
        return -1;
    }
    ctx->metrics.total_requests = 0;
    ctx->metrics.total_responses = 0;
    ctx->metrics.total_errors = 0;
    ctx->metrics.average_response_time_ms = 0;
    ctx->total_response_time_ms = 0;
    return 0;
}

static void metrics_timer_cb(void *arg)
{
    http_ctx_t *ctx = (http_ctx_t *)arg;
    if (ctx->config.metrics_cb) {
        ctx->config.metrics_cb(&ctx->metrics, ctx->config.metrics_user_data);
    }
}

int http_set_metrics_callback(http_ctx_t *ctx, int interval_ms, http_metrics_callback cb, void *user_data)
{
    if (!ctx) {
        return -1;
    }
    if (interval_ms <= 0) {
        ctx->config.metrics_interval_ms = 0;
        ctx->config.metrics_cb = NULL;
        ctx->config.metrics_user_data = NULL;
        return 0;
    }

    ctx->config.metrics_interval_ms = interval_ms;
    ctx->config.metrics_cb = cb;
    ctx->config.metrics_user_data = user_data;

    int timer_id = http_set_timer(ctx, interval_ms, interval_ms, metrics_timer_cb, ctx);
    if (timer_id < 0) {
        return -1;
    }
    return 0;
}
#endif

#ifdef HTTP_ENABLE_SELF_TESTS
int http_run_self_tests(void)
{
    char *enc = http_url_encode("abc 123/");
    char *dec = http_url_decode(enc);
    int ok = dec && strcmp(dec, "abc 123/") == 0;
    free(enc);
    free(dec);
    return ok ? 0 : 1;
}

int http_register_test(const char *test_name __attribute__((unused)),
                       http_test_fn fn __attribute__((unused)))
{
    return 0;
}
#endif

void http_request_free(http_request_t *req)
{
    if (req->method) {
        free((void *)req->method);
    }
    if (req->uri_path) {
        free((void *)req->uri_path);
    }
    if (req->uri_query) {
        free((void *)req->uri_query);
    }
    if (req->headers) {
        for (size_t i = 0; i < req->num_headers; i++) {
            free((void *)req->headers[i].name);
            free((void *)req->headers[i].value);
        }
        free(req->headers);
    }
    if (req->body) {
        free((void *)req->body);
    }
    if (req->peer_addr) {
        free((void *)req->peer_addr);
    }
}

typedef struct {
    const char *ext;
    const char *type;
} mime_map_t;

static const mime_map_t mime_map[] = {
    {"html", "text/html"},
    {"htm", "text/html"},
    {"css", "text/css"},
    {"js", "application/javascript"},
    {"json", "application/json"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"txt", "text/plain"},
    {"pdf", "application/pdf"},
    {NULL, "application/octet-stream"}
};

const char *http_mime_type_from_ext(const char *ext)
{
    if (!ext) {
        return "application/octet-stream";
    }
    for (int i = 0; mime_map[i].ext; i++) {
        if (strcasecmp(ext, mime_map[i].ext) == 0) {
            return mime_map[i].type;
        }
    }
    return "application/octet-stream";
}

#include "http_internal.h"

static char *find_header_end(char *data, size_t len)
{
    for (size_t i = 0; i + 3 < len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n') {
            return data + i + 4;
        }
    }
    return NULL;
}

static int parse_request_line(http_conn_t *c, char *hdr_block)
{
    char *line_end = strstr(hdr_block, "\r\n");
    if (!line_end) {
        HTTP_ERR(NULL, "Request line not terminated");
        return -1;
    }
    *line_end = '\0';

    char method[16];
    char uri[1024];
    char version[16];

    if (sscanf(hdr_block, "%15s %1023s %15s", method, uri, version) != 3) {
        HTTP_ERR(NULL, "Malformed request line");
        return HTTP_PARSE_BAD_REQUEST;
    }

    c->req.method = strdup(method);
    if (!c->req.method) {
        HTTP_ERR(NULL, "Out of memory copying method");
        return -1;
    }

    char *q = strchr(uri, '?');
    if (q) {
        *q = '\0';
        c->req.uri_path = strdup(uri);
        c->req.uri_query = strdup(q + 1);
    } else {
        c->req.uri_path = strdup(uri);
        c->req.uri_query = NULL;
    }
    if (!c->req.uri_path || (q && !c->req.uri_query)) {
        HTTP_ERR(NULL, "Out of memory copying URI");
        return -1;
    }

    if (strncmp(version, "HTTP/", 5) == 0) {
        if (sscanf(version + 5, "%d.%d", &c->req.http_major, &c->req.http_minor) != 2) {
            return HTTP_PARSE_BAD_REQUEST;
        }
    } else {
        return HTTP_PARSE_BAD_REQUEST;
    }

    return 0;
}

static int parse_headers(http_ctx_t *ctx, http_conn_t *c, char *hdr_start, size_t hdr_len)
{
    size_t hdr_count = 0;
    char *tmp = hdr_start;

    while (tmp < hdr_start + hdr_len && *tmp) {
        char *nl = strstr(tmp, "\r\n");
        if (!nl || nl == tmp) {
            break;
        }
        hdr_count++;
        tmp = nl + 2;
    }

    c->req.headers = http_malloc(ctx, sizeof(*c->req.headers) * hdr_count);
    if (!c->req.headers) {
        HTTP_ERR(ctx, "Out of memory allocating headers array");
        return -1;
    }

    c->req.num_headers = 0;
    tmp = hdr_start;
    while (tmp < hdr_start + hdr_len && *tmp) {
        char *nl = strstr(tmp, "\r\n");
        if (!nl || nl == tmp) {
            break;
        }
        *nl = '\0';
        char *colon = strchr(tmp, ':');
        if (colon) {
            *colon = '\0';
            char *name = tmp;
            char *value = colon + 1;
            while (*value && isspace((unsigned char)*value)) {
                value++;
            }
            c->req.headers[c->req.num_headers].name = strdup(name);
            c->req.headers[c->req.num_headers].value = strdup(value);
            if (!c->req.headers[c->req.num_headers].name ||
                !c->req.headers[c->req.num_headers].value) {
                HTTP_ERR(ctx, "Out of memory copying header");
                return -1;
            }
            c->req.num_headers++;
        }
        tmp = nl + 2;
    }

    return 0;
}

static int parse_body(http_ctx_t *ctx, http_conn_t *c, char *body_start, size_t total_len, size_t header_len)
{
    size_t received = total_len - header_len;
    if (received < c->content_length) {
        return 0;
    }

    if (c->content_length > 0) {
        c->req.body = malloc(c->content_length + 1);
        if (!c->req.body) {
            HTTP_ERR(ctx, "Out of memory copying body");
            return -1;
        }
        memcpy((char *)c->req.body, body_start, c->content_length);
        ((char *)c->req.body)[c->content_length] = '\0';
        c->req.body_len = c->content_length;
    }

    return 1;
}

int parse_request(http_ctx_t *ctx, http_conn_t *c)
{
    if (c->header_parsed) {
        return 1;
    }

    char *data = c->rb.data;
    size_t len = c->rb.len;
    char *body_start = find_header_end(data, len);
    if (!body_start) {
        return 0;
    }

    size_t header_len = (size_t)(body_start - data);
    char *hdr_block = malloc(header_len + 1);
    if (!hdr_block) {
        HTTP_ERR(ctx, "Out of memory allocating hdr_block");
        return HTTP_PARSE_ERROR;
    }
    memcpy(hdr_block, data, header_len);
    hdr_block[header_len] = '\0';

    int req_line_res = parse_request_line(c, hdr_block);
    if (req_line_res < 0) {
        if (req_line_res == HTTP_PARSE_BAD_REQUEST) {
            goto err_bad_request;
        }
        goto err_free_hdr;
    }
    if (parse_headers(ctx, c, hdr_block + strlen(hdr_block) + 2,
                      header_len - (strlen(hdr_block) + 2)) < 0) {
        goto err_free_hdr;
    }

    c->content_length = 0;
    for (size_t i = 0; i < c->req.num_headers; i++) {
        if (strcasecmp(c->req.headers[i].name, "Content-Length") == 0) {
            c->content_length = strtoul(c->req.headers[i].value, NULL, 10);
        }
        if (strcasecmp(c->req.headers[i].name, "Connection") == 0 &&
            strcasecmp(c->req.headers[i].value, "keep-alive") == 0) {
            c->keep_alive = 1;
        }
    }

    int body_res = parse_body(ctx, c, body_start, len, header_len);
    if (body_res < 0) {
        goto err_free_hdr;
    }
    if (body_res == 0) {
        free(hdr_block);
        return HTTP_PARSE_INCOMPLETE;
    }

    c->header_parsed = 1;
    free(hdr_block);
    return HTTP_PARSE_OK;

err_bad_request:
    free(hdr_block);
    return HTTP_PARSE_BAD_REQUEST;
err_free_hdr:
    free(hdr_block);
    return HTTP_PARSE_ERROR;
}

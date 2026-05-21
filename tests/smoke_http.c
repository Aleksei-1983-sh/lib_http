#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s\n", msg); \
            failures++; \
        } \
    } while (0)

static void noop_handler(http_request_t *req, http_response_t *res, void *user_data)
{
    (void)req;
    (void)res;
    (void)user_data;
}

static void noop_timer(void *user_data)
{
    (void)user_data;
}

static void test_url_utils(void)
{
    char *enc = http_url_encode("a b/c?d");
    char *dec = http_url_decode(enc);
    char *json = http_escape_json_string("x\"\n");

    CHECK(enc != NULL, "http_url_encode returned NULL");
    CHECK(dec != NULL, "http_url_decode returned NULL");
    CHECK(json != NULL, "http_escape_json_string returned NULL");

    if (enc) {
        CHECK(strcmp(enc, "a%20b%2Fc%3Fd") == 0, "unexpected URL encoding");
    }
    if (dec) {
        CHECK(strcmp(dec, "a b/c?d") == 0, "unexpected URL decoding");
    }
    if (json) {
        CHECK(strcmp(json, "x\\\"\\n") == 0, "unexpected JSON escaping");
    }

    free(enc);
    free(dec);
    free(json);
}

static void test_mime(void)
{
    CHECK(strcmp(http_mime_type_from_ext("json"), "application/json") == 0,
          "json MIME mismatch");
    CHECK(strcmp(http_mime_type_from_ext("unknown"), "application/octet-stream") == 0,
          "fallback MIME mismatch");
}

static void test_response_api(void)
{
    http_response_t res;
    memset(&res, 0, sizeof(res));

    http_response_init(&res);
    http_response_set_status(&res, 201, "Created");
    http_response_add_header(&res, "Content-Type", "text/plain");
    CHECK(http_response_write_body(&res, "abc", 3) == 0, "write_body failed");
    CHECK(http_response_end(&res) == 0, "response_end failed");

    CHECK(res.status_code == 201, "status code not stored");
    CHECK(res.num_resp_headers == 1, "header count mismatch");
    if (res.num_resp_headers == 1) {
        CHECK(strcmp(res.resp_headers[0].name, "Content-Type") == 0,
              "header name mismatch");
        CHECK(strcmp(res.resp_headers[0].value, "text/plain") == 0,
              "header value mismatch");
    }

    http_response_free(&res);
}

static void test_context_and_routes(void)
{
    http_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.recv_buffer_size = 1024;
    cfg.send_buffer_size = 1024;
    cfg.enable_chunked = 1;

    http_ctx_t *ctx = http_init(&cfg);
    CHECK(ctx != NULL, "http_init failed");
    if (!ctx) {
        return;
    }

    CHECK(http_register_route(ctx, "GET", "/health", noop_handler, NULL) == 0,
          "register_route failed");
    CHECK(http_set_timer(ctx, 10, 0, noop_timer, NULL) > 0, "set_timer failed");
    http_cancel_timer(ctx, 1);

    CHECK(http_listen(ctx, "127.0.0.1:0", noop_handler, NULL) == 0,
          "http_listen failed on ephemeral port");
    CHECK(http_poll(ctx, 0) >= 0, "http_poll failed");

    http_free(ctx);
}

#ifdef HTTP_ENABLE_MONITORING
static void metrics_cb(const http_metrics_t *metrics, void *user_data)
{
    (void)metrics;
    (void)user_data;
}

static void test_metrics_api(void)
{
    http_ctx_t *ctx = http_init(NULL);
    http_metrics_t metrics;

    CHECK(ctx != NULL, "http_init failed for metrics test");
    if (!ctx) {
        return;
    }

    CHECK(http_get_metrics(ctx, &metrics) == 0, "http_get_metrics failed");
    CHECK(http_reset_metrics(ctx) == 0, "http_reset_metrics failed");
    CHECK(http_set_metrics_callback(ctx, 100, metrics_cb, NULL) == 0,
          "http_set_metrics_callback enable failed");
    CHECK(http_set_metrics_callback(ctx, 0, NULL, NULL) == 0,
          "http_set_metrics_callback disable failed");

    http_free(ctx);
}
#endif

#ifdef HTTP_ENABLE_SELF_TESTS
static int dummy_test(void)
{
    return 0;
}

static void test_self_tests_api(void)
{
    CHECK(http_run_self_tests() == 0, "http_run_self_tests failed");
    CHECK(http_register_test("dummy", dummy_test) == 0,
          "http_register_test failed");
}
#endif

#ifdef HTTP_ENABLE_MULTITHREADING
static void test_multithreading_stub(void)
{
    http_ctx_t *ctx = http_init(NULL);

    CHECK(ctx != NULL, "http_init failed for MT test");
    if (!ctx) {
        return;
    }

    CHECK(http_run_multithreaded(ctx, "127.0.0.1:0", noop_handler, NULL) == -1,
          "multithreaded stub returned unexpected value");
    http_stop_multithreaded(ctx);
    http_free(ctx);
}
#endif

#ifdef HTTP_ENABLE_TLS
static void test_tls_stub(void)
{
    http_ctx_t *ctx = http_init(NULL);

    CHECK(ctx != NULL, "http_init failed for TLS test");
    if (!ctx) {
        return;
    }

    CHECK(http_listen_https(ctx, "127.0.0.1:0", noop_handler, NULL) == -1,
          "https stub returned unexpected value");
    http_free(ctx);
}
#endif

int main(void)
{
    test_url_utils();
    test_mime();
    test_response_api();
    test_context_and_routes();

#ifdef HTTP_ENABLE_MONITORING
    test_metrics_api();
#endif
#ifdef HTTP_ENABLE_SELF_TESTS
    test_self_tests_api();
#endif
#ifdef HTTP_ENABLE_MULTITHREADING
    test_multithreading_stub();
#endif
#ifdef HTTP_ENABLE_TLS
    test_tls_stub();
#endif

    if (failures != 0) {
        fprintf(stderr, "smoke tests failed: %d\n", failures);
        return 1;
    }

    printf("smoke tests passed\n");
    return 0;
}

#include "http/http.h"

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

static const char *find_resp_header(const http_response_t *res, const char *name)
{
    for (size_t i = 0; i < res->num_resp_headers; i++) {
        if (strcmp(res->resp_headers[i].name, name) == 0) {
            return res->resp_headers[i].value;
        }
    }
    return NULL;
}

static void assert_header_value(const http_response_t *res, const char *name, const char *expected)
{
    const char *value = find_resp_header(res, name);
    CHECK(value != NULL, "expected response header was not found");
    if (value) {
        CHECK(strcmp(value, expected) == 0, "response header value mismatch");
    }
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

static void test_url_utils_errors(void)
{
    CHECK(http_url_encode(NULL) == NULL, "http_url_encode(NULL) should return NULL");
    CHECK(http_url_decode(NULL) == NULL, "http_url_decode(NULL) should return NULL");
    CHECK(http_escape_json_string(NULL) == NULL,
          "http_escape_json_string(NULL) should return NULL");
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
    assert_header_value(&res, "Content-Type", "text/plain");

    http_response_free(&res);
}

static void test_response_headers_api(void)
{
    http_response_t res;
    memset(&res, 0, sizeof(res));

    http_response_init(&res);
    http_response_add_header(&res, "Content-Type", "text/plain");
    http_response_add_header(&res, "Connection", "keep-alive");

    CHECK(res.num_resp_headers == 2, "response should store multiple headers");
    assert_header_value(&res, "Content-Type", "text/plain");
    assert_header_value(&res, "Connection", "keep-alive");

    http_response_free(&res);
}

static void test_response_api_errors(void)
{
    http_response_t res;
    memset(&res, 0, sizeof(res));

    http_response_init(&res);

    CHECK(http_response_write_body(NULL, "abc", 3) == -1,
          "write_body should fail on NULL response");
    CHECK(http_response_write_body(&res, NULL, 3) == -1,
          "write_body should fail on NULL data with non-zero len");
    CHECK(http_response_write_body(&res, "abc", 0) == 0,
          "write_body should allow zero-length writes");
    CHECK(http_response_end(NULL) == -1, "response_end should fail on NULL response");

    http_response_free(&res);
}

int main(void)
{
    test_url_utils();
    test_url_utils_errors();
    test_mime();
    test_response_api();
    test_response_headers_api();
    test_response_api_errors();

    if (failures != 0) {
        fprintf(stderr, "unit tests failed: %d\n", failures);
        return 1;
    }

    printf("unit tests passed\n");
    return 0;
}

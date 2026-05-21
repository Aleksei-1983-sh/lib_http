
// -------------------- test_client.c --------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    fwrite(ptr, size, nmemb, stdout);
    return size * nmemb;
}

void test(const char *method, const char *url, const char *data) {
    CURL *c = curl_easy_init();
    if (!c) return;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    if (data) curl_easy_setopt(c, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    printf("\n===== %s %s =====\n", method, url);
    curl_easy_perform(c);
    curl_easy_cleanup(c);
}

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    test("GET", "http://localhost:8080/health", NULL);
    test("GET", "http://localhost:8080/echo?msg=Hello%20C", NULL);
    test("POST", "http://localhost:8080/echo", "This is body");
    test("GET", "http://localhost:8080/headers", NULL);
    test("GET", "http://localhost:8080/chunked", NULL);
    test("GET", "http://localhost:8080/url_utils", NULL);
    test("GET", "http://localhost:8080/json_utils", NULL);
    test("GET", "http://localhost:8080/mime", NULL);
    test("GET", "http://localhost:8080/metrics", NULL);
    test("GET", "http://localhost:8080/set_timer?delay=2000", NULL);
    curl_global_cleanup();
    return 0;
}

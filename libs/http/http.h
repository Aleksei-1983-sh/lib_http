
/*
 * http.h - Lightweight HTTP/HTTPS client-server library
 * Version: 1.0.0
 *
 * Features:
 *  - Event-driven HTTP server and client (sync and async APIs)
 *  - Optional multithreaded server support
 *  - TLS/HTTPS support via pluggable TLS backends
 *  - Basic routing API for server
 *  - URL utilities, header manipulation
 *  - JSON integration via external parsers (e.g., jsmn)
 *  - Monitoring (metrics collection and callbacks)
 *  - Timer API in event loop
 *  - Logging abstraction
 *  - Self-test (auto-tests) hooks (enabled via HTTP_ENABLE_SELF_TESTS)
 *
 * Configuration via http_config_t with compile-time flags to include/exclude features.
 */
#ifndef HTTP_H
#define HTTP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/time.h>

// If multithreading enabled, include pthreads
#ifdef HTTP_ENABLE_MULTITHREADING
#include <pthread.h>
#endif

/** Opaque HTTP context */
/** Forward declarations */
typedef struct http_ctx http_ctx_t;

#include "log.h"
#include "http_response.h"
#include "http_request.h"
#include "metrics.h"
#include "config.h"

// Максимальное число слушающих сокетов
#ifndef MAX_LISTENERS
#define MAX_LISTENERS 16
#endif

/** Library version */
#define HTTP_LIB_VERSION "1.0.0"

/** Server request handler callback */
typedef void (*http_handler_fn)(http_request_t *req,
                                http_response_t *res,
                                void *user_data);

/** Timer callback type */
typedef void (*http_timer_fn)(void *user_data);

/** Metrics structure (opaque fields internally) */
typedef struct http_metrics http_metrics_t;

/** Self-test function prototype */
typedef int (*http_test_fn)(void);

/*─────────────────────────────────────────────────────────*/
/*  Core API                                               */
/*─────────────────────────────────────────────────────────*/

/** Initialize library (returns context or NULL on error) */
http_ctx_t *http_init(const http_config_t *config);

/** Free context and all associated resources */
void http_free(http_ctx_t *ctx);

/** Start server (single-threaded)
 *  address: "ip:port" or NULL for default (e.g., "0.0.0.0:80").
 *  handler: callback for each request
 *  user_data: passed to handler
 *  Returns 0 on success, negative on error.
 */
int http_listen(http_ctx_t *ctx, const char *address, http_handler_fn handler, void *user_data);

/** Start HTTPS server (single-threaded)
 *  Requires HTTP_ENABLE_TLS.
 */
#ifdef HTTP_ENABLE_TLS
int http_listen_https(http_ctx_t *ctx, const char *address, http_handler_fn handler, void *user_data);
#endif

/** Start server and run event loop (blocking until stopped)
 *  For single-threaded server: calls http_listen internally, then loops.
 */
int http_run(http_ctx_t *ctx);

/** Stop the running server/event loop */
void http_stop(http_ctx_t *ctx);

/** Poll event loop once (timeout in ms) for custom loops or integration
 *  Returns number of events handled, or negative on error.
 */
int http_poll(http_ctx_t *ctx, int timeout_ms);

#ifdef HTTP_ENABLE_MULTITHREADING
/** Start multithreaded server: spawn worker threads running event loops
 *  Each thread handles a subset of connections or accepts connections via a shared listening socket.
 *  address: as in http_listen
 *  handler/user_data: as in http_listen
 *  Returns 0 on success, negative on error.
 */
int http_run_multithreaded(http_ctx_t *ctx, const char *address, http_handler_fn handler, void *user_data);

/** Stop multithreaded server */
void http_stop_multithreaded(http_ctx_t *ctx);
#endif

/** Routing API (optional): register route patterns with methods
 *  pattern: exact match or simple parameterized (e.g., "/api/item/{id}")
 *  Handler will be called if route and method match.
 *  Returns 0 on success.
 */
int http_register_route(http_ctx_t *ctx, const char *method, const char *route_pattern, http_handler_fn handler, void *user_data);

/** HTTP client APIs **/
/** Timer API: schedule a callback after delay_ms; if interval_ms>0, repeats every interval_ms
 *  Returns timer_id >=0 on success, or negative on error.
 */
int http_set_timer(http_ctx_t *ctx, int delay_ms, int interval_ms, http_timer_fn cb, void *user_data);
void http_cancel_timer(http_ctx_t *ctx, int timer_id);

/** Self-test / auto-tests
 * If HTTP_ENABLE_SELF_TESTS is defined, library includes built-in tests for parser, URL utils, etc.
 * User can call http_run_self_tests() to execute tests and get summary.
 */
#ifdef HTTP_ENABLE_SELF_TESTS
/** Run all built-in self-tests
 * Returns 0 if all tests pass, or non-zero if any fail.
 */
int http_run_self_tests(void);

/** Register additional test (if user wants to add custom tests)
 * Returns 0 on success.
 */
int http_register_test(const char *test_name, http_test_fn fn);
#endif

/** Other utilities: MIME type lookup for static files
 * Caller provides file extension, library returns common MIME type or "application/octet-stream".
 */
const char *http_mime_type_from_ext(const char *ext);


#ifdef __cplusplus
}
#endif

#endif /* HTTP_H */

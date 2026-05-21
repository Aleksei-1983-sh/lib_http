
/*
 * http.h - Lightweight HTTP/1.x server-oriented library
 *
 * Current public surface matches the parts implemented in libs/http/http.c:
 *  - single-threaded HTTP server event loop
 *  - basic routing
 *  - timer callbacks
 *  - request/response helper types
 *  - URL / JSON / MIME utility helpers
 *
 * Optional APIs are exposed only behind feature flags. Some of those entry
 * points are still partial or stubbed.
 */
#ifndef HTTP_H
#define HTTP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/time.h>

/* Forward declarations */
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

/** Start HTTPS server.
 *  Declared only with `HTTP_ENABLE_TLS`.
 *  The current implementation is still a stub and returns an error.
 */
#ifdef HTTP_ENABLE_TLS
int http_listen_https(http_ctx_t *ctx, const char *address, http_handler_fn handler, void *user_data);
#endif

/** Run the event loop until `http_stop()` is called. */
int http_run(http_ctx_t *ctx);

/** Stop the running server/event loop */
void http_stop(http_ctx_t *ctx);

/** Poll event loop once (timeout in ms) for custom loops or integration
 *  Returns number of events handled, or negative on error.
 */
int http_poll(http_ctx_t *ctx, int timeout_ms);

#ifdef HTTP_ENABLE_MULTITHREADING
/** Multithreaded entry points.
 *  Declared only with `HTTP_ENABLE_MULTITHREADING`.
 *  The current implementation is not complete.
 */
int http_run_multithreaded(http_ctx_t *ctx, const char *address, http_handler_fn handler, void *user_data);

/** Stop multithreaded server */
void http_stop_multithreaded(http_ctx_t *ctx);
#endif

/** Register a route handler for an HTTP method and path pattern.
 *  Matching is currently basic and implementation-defined.
 */
int http_register_route(http_ctx_t *ctx, const char *method, const char *route_pattern, http_handler_fn handler, void *user_data);

/** Timer API: schedule a callback after delay_ms; if interval_ms>0, repeats every interval_ms
 *  Returns timer_id >=0 on success, or negative on error.
 */
int http_set_timer(http_ctx_t *ctx, int delay_ms, int interval_ms, http_timer_fn cb, void *user_data);
void http_cancel_timer(http_ctx_t *ctx, int timer_id);

/** Built-in self-tests. Declared only with `HTTP_ENABLE_SELF_TESTS`. */
#ifdef HTTP_ENABLE_SELF_TESTS
/** Run the built-in smoke-style self-tests. */
int http_run_self_tests(void);

/** Register an additional test hook.
 *  The current implementation is only a placeholder.
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

#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#ifdef __cplusplus
extern "C" {
#endif

#include "http_response.h"

/** HTTP request structure (opaque fields for internal state)
 *  Fields for user to inspect request data.
 */
typedef struct http_request {
	const char *method;    /**< "GET", "POST", ... */
	const char *uri_path;  /**< Path component, e.g., "/api/item" */
	const char *uri_query; /**< Query string (after '?') or NULL */
	int http_major;        /**< HTTP version major */
	int http_minor;        /**< HTTP version minor */

	struct {
		const char *name;
		const char *value;
	} *headers;
	size_t num_headers;

	const char *body;      /**< Pointer to body data (if fully buffered) */
	size_t body_len;

	// For streaming body reads, library-specific callbacks can be used (not exposed here)

	// Peer information, TLS info, etc. (opaque or via other APIs)
	const char *peer_addr; /**< String with client IP:port */
	void *internal;        /**< For internal use */

	// Настройки клиента:
	int timeout_ms;
	const char *user_agent;
	int keep_alive;
	void *custom_data;
} http_request_t;

/*─────────────────────────────────────────────────────────*/
/*  Client API                                             */
/*─────────────────────────────────────────────────────────*/

/** Perform blocking request; returns HTTP status or negative on error */
int http_client_request_blocking(const http_request_t *req, http_response_t *out_resp);


/** Asynchronous client request
 *  ctx: HTTP context (with event loop);
 *  url/method/headers/body as above;
 *  cb: called when response is complete or error occurred;
 *  user_data: passed to cb.
 *  Returns >0 as request id, or negative on error.
 */
/*
int http_client_request_async(http_ctx_t *ctx,
                              const char *url,
                              const char *method,
                              const char * const headers[],
                              const void *body, size_t body_len,
                              void (*cb)(http_response_t *res, void *user_data);
                              void *user_data);
*/
/** Free any internal allocations inside request */
void http_request_free(http_request_t *req);


/*─────────────────────────────────────────────────────────*/
/*  URL & utility functions                                */
/*─────────────────────────────────────────────────────────*/

/** URL utilities **/
char *http_url_encode(const char *src);
char *http_url_decode(const char *src);

/** JSON utilities: escape string for JSON output */
char *http_escape_json_string(const char *src);

/*
void http_request_init(http_request_t *req);
void http_request_free(http_request_t *req);

void http_request_set_method(http_request_t *req, const char *method);
void http_request_set_path(http_request_t *req, const char *path);
void http_request_set_version(http_request_t *req, const char *version);

void http_request_add_header(http_request_t *req, const char *name, const char *value);
void http_request_set_body(http_request_t *req, const void *data, size_t len);

int  http_request_serialize(const http_request_t *req, dynbuf_t *out);
*/

#ifdef __cplusplus
}
#endif

#endif /* HTTP_REQUEST_H */

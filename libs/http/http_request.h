#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#ifdef __cplusplus
extern "C" {
#endif

#include "http_response.h"

/** HTTP request structure passed to server handlers.
 *  Fields are filled by the parser and should be treated as read-only.
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

	const char *peer_addr; /**< String with client IP:port */
	void *internal;        /**< For internal use */

	/* Reserved fields kept for ABI evolution and internal use. */
	int timeout_ms;
	const char *user_agent;
	int keep_alive;
	void *custom_data;
} http_request_t;
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

#ifdef __cplusplus
}
#endif

#endif /* HTTP_REQUEST_H */


#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/** HTTP response structure filled through the helper API below.
 *  `internal` is managed by the library.
 */
typedef struct http_response {
    int status_code;            /**< e.g., 200 */
    const char *status_reason;  /**< e.g., "OK"; if NULL, library provides default */

    struct {
        char *name;
        char *value;
    } *resp_headers;
    size_t num_resp_headers;

    void *internal;             /**< For internal use */
} http_response_t;



/*─────────────────────────────────────────────────────────*/
/*  Response-building API                                  */
/*─────────────────────────────────────────────────────────*/

/** Initialize to defaults (status=200 OK, no headers, empty body) */
void http_response_init(http_response_t *res);

/** Set status code and (optional) custom reason */
void http_response_set_status(http_response_t *res,
                              int status_code,
                              const char *reason);

/** Add a header (name/value copied internally) */
void http_response_add_header(http_response_t *res,
                              const char *name,
                              const char *value);

/** Append body data to the buffered response body.
 */
int http_response_write_body(http_response_t *res, const void *data, size_t len);

/** Finalize the response object for the current request.
 *  In the current implementation this mostly validates/logs buffered state.
 */
int http_response_end(http_response_t *res);

/** Free any internal allocations inside response */
void http_response_free(http_response_t *res);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_RESPONSE_H */

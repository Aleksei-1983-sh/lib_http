
#ifndef HTTP_CONFIG_H
#define HTTP_CONFIG_H

#include <stddef.h>
#include "log.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct http_metrics http_metrics_t;

/** Callback для периодической отдачи метрик */
typedef void (*http_metrics_callback)(const http_metrics_t *metrics, void *user_data);

/** Функция логирования (пример) */
typedef void (*http_log_fn)(http_log_level_t level, void *user_data, const char *fmt, ...);


/** Configuration for HTTP context */
typedef struct {
    // Buffer sizes
    size_t recv_buffer_size;    /**< Size of receive buffer per connection */
    size_t send_buffer_size;    /**< Size of send buffer per connection */
    int max_connections;        /**< Maximum simultaneous connections (server) */
    int keep_alive_timeout_ms;  /**< Timeout for keep-alive connections */
    int request_timeout_ms;     /**< Timeout for initial request read */

    // TLS configuration
#ifdef HTTP_ENABLE_TLS
    const char *tls_cert_path;  /**< Server certificate (PEM) */
    const char *tls_key_path;   /**< Server private key (PEM) */
    const char *tls_ca_path;    /**< CA bundle for client verification */
    int tls_verify_peer;        /**< 0: no verification, 1: verify peer */
#endif

    // Multithreading
#ifdef HTTP_ENABLE_MULTITHREADING
    int thread_count;           /**< Number of worker threads for server */
#endif

    // Monitoring
#ifdef HTTP_ENABLE_MONITORING
    int metrics_interval_ms;          /**< Interval for metrics callback invocation, ms */
    http_metrics_callback metrics_cb; /**< Callback for metrics */
    void *metrics_user_data;          /**< User data for metrics callback */
#endif

    // Logging
    http_log_fn log_fn;         /**< Logging Callback */
    void *log_user_data;        /**< Data passed to log callback */

    // Allocators
    void *(*malloc_fn)(size_t);
    void (*free_fn)(void *);

    // Feature flags
    int enable_compression;     /**< If non-zero, enable response compression (requires zlib) */
    int enable_chunked;         /**< If non-zero, support chunked transfer encoding */
    int enable_http2;           /**< Placeholder for HTTP/2 support (not implemented by default) */

    // DNS resolver: blocking or async? Documented separately.

} http_config_t;

#ifdef __cplusplus
}
#endif

#endif /* HTTP_CONFIG_H */

#ifndef METRICS_H
#define METRICS_H

/** Monitoring: metrics structure */
typedef struct http_metrics {
    uint64_t total_requests;      /**< Total requests handled since start/reset */
    uint64_t total_responses;     /**< Total responses sent */
    uint64_t active_connections;  /**< Current active connections */
    uint64_t total_errors;        /**< Total errors encountered */
    double average_response_time_ms; /**< Average response time in ms */
    // Extendable: histograms, percentiles, etc.
} http_metrics_t;

typedef void (*http_metrics_callback)(const http_metrics_t *metrics, void *user_data);

/** Get current metrics snapshot */
int http_get_metrics(http_ctx_t *ctx, http_metrics_t *out_metrics);

/** Reset metrics counters (active connections remains current) */
int http_reset_metrics(http_ctx_t *ctx);

/** Set or update metrics callback and interval (ms). If interval_ms <=0, disable callback. */
int http_set_metrics_callback(http_ctx_t *ctx, int interval_ms, http_metrics_callback cb, void *user_data);

#endif /* METRICS_H */

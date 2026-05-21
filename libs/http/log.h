#ifndef LOG_H
#define LOG_H

/** Log levels for the library */
typedef enum {
    HTTP_LOG_ERROR = 0,
    HTTP_LOG_WARN,
    HTTP_LOG_INFO,
    HTTP_LOG_DEBUG
} http_log_level_t;

#endif /* LOG_H */

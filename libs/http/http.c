/*
 * http.c - Implementation for Lightweight HTTP/HTTPS client-server library
 * Version: 1.0.0
 *
 * Note: This implementation focuses on core single-threaded HTTP/1.0/1.1 functionality,
 * basic routing, timers, metrics, URL utilities, blocking client requests.
 * Optional features (TLS, multithreading, async client, compression) are stubbed or marked TODO.
 * Platform: POSIX (uses poll, sockets).
 */

#define _POSIX_C_SOURCE 200112L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // для strcasecmp
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/time.h>  // для struct timeval, timercmp для функции gettimeofday()
#include <ctype.h>
#include <stdarg.h>  // для va_list
#include <limits.h>  // для SIZE_MAX

#include "http.h"
#include "dynbuf.h"

#ifndef HTTP_ENABLE_TLS
// TLS stub: no-op or error if HTTPS used
#endif

#ifndef HTTP_ENABLE_MULTITHREADING
// Multithreading stubs
#endif

#ifndef HTTP_ENABLE_MONITORING
// Monitoring disabled
#endif

#ifndef HTTP_ENABLE_SELF_TESTS
// Self-tests disabled
#endif

// Internal structures
#define INITIAL_CONN_CAPACITY 64
#define INITIAL_ROUTE_CAPACITY 16
#define MAX_HEADER_NAME_LEN 256
#define MAX_HEADER_VALUE_LEN 1024
#define URL_UTIL_BUF_SIZE 3 * 1024

/*
 * Макросы для отладки:
 * OL_DBG — печатает отладочные сообщения при DEBUG=1;
 * ERR — печатает ошибки всегда.
 */

#if defined(DEBUG) && DEBUG == 1
#define HTTP_DBG(ctxptr, fmt, ...) \
	HTTP_DBG_IMPL((void *)(ctxptr), fmt, ##__VA_ARGS__)

#define HTTP_DBG_IMPL(_vctx, fmt, ...) \
	do { \
		if ((_vctx) && ((http_ctx_t *)(_vctx))->config.log_fn) { \
			const char *_fmt = "[%s:%d] " fmt ; \
			((http_ctx_t *)(_vctx))->config.log_fn(HTTP_LOG_DEBUG, ((http_ctx_t *)(_vctx))->config.log_user_data, _fmt, __func__, __LINE__, ##__VA_ARGS__); \
		} else { \
			const char *_fmt = "\033[0;32m[DEBUG] \033[0m [%s:%d] " fmt "\n" ; \
			fprintf(stderr, _fmt , __func__, __LINE__, ##__VA_ARGS__); \
		} \
	} while (0)

#else
#define HTTP_DBG(ctxptr, fmt, ...) ((void)0)
#endif

#define HTTP_ERR(ctxptr, fmt, ...) \
	HTTP_ERR_IMPL((void *)(ctxptr), fmt, ##__VA_ARGS__)

#define HTTP_ERR_IMPL(_vctx, fmt, ...) \
	do { \
		if ((_vctx) && ((http_ctx_t *)(_vctx))->config.log_fn) { \
			const char *_fmt = "[%s:%d] " fmt ; \
			((http_ctx_t *)(_vctx))->config.log_fn(HTTP_LOG_ERROR, ((http_ctx_t *)(_vctx))->config.log_user_data, _fmt, __func__, __LINE__, ##__VA_ARGS__); \
		} else { \
			const char *_fmt = "\033[0;31m[ERROR] \033[0m [%s:%d] " fmt "\n"; \
			fprintf(stderr, _fmt , __func__, __LINE__, ##__VA_ARGS__); \
		} \
	} while (0)



typedef struct http_conn http_conn_t;

// =================
// Внутренняя структура для буферизации тела ответа
// Хранится в http_response_t.internal
typedef struct {
	// Если в вашей библиотеке есть структура соединения, мы храним указатель на неё,
	// чтобы знать, куда отправлять сформированный ответ. Предполагаем, что в http_conn_t есть поле fd и флаги keep-alive.
	http_conn_t *conn;

	// Буфер для накопления тела ответа. Аналог dynbuf_t, но отдельный экземпляр.
	// Можно переиспользовать dynbuf_t, если он подходит. 
	// Предположим, что dynbuf_t есть и имеет поля data, len, cap, и функции dynbuf_init/free/append.
	dynbuf_t bodybuf;

	// Флаг: заголовки ещё не отправлены (не нужно для буферизованного режима), но может пригодиться, если захотите стриминг.
	int headers_sent;
} http_response_internal_t;

// Timer structure
typedef struct http_timer {
    int id;
    struct timeval due;
    int interval_ms; // 0 for one-shot
    http_timer_fn cb;
    void *user_data;
    struct http_timer *next;
} http_timer_t;

// Routing entry
typedef struct {
    char *method;
    char *pattern;
    http_handler_fn handler;
    void *user_data;
} route_entry_t;

// Connection state
typedef enum {
    CONN_STATE_READING,
    CONN_STATE_WRITING,
    CONN_STATE_CLOSED
} conn_state_t;

struct http_conn {
    int fd;
    int is_client; // 0=server conn, 1=client async (stub)
    conn_state_t state;
    struct timeval start_time;
    // Read buffer for request
    dynbuf_t rb;
    size_t header_parsed; // 0 until headers parsed
    size_t content_length;
    // Parsed request
    http_request_t req;
    // Response
    http_response_t res;
    dynbuf_t wb;
    size_t wb_sent;
    int keep_alive;
    struct http_conn *next;
};

struct http_ctx {
    http_config_t config;
    int stopped;
    int listen_fds[MAX_LISTENERS];
    int listen_count;
    http_conn_t *conns;
    // routing
    route_entry_t *routes;
    size_t route_count;
    size_t route_cap;
    // timers
    http_timer_t *timers;
    int next_timer_id;
    // metrics
#ifdef HTTP_ENABLE_MONITORING
    http_metrics_t metrics;
    long double total_response_time_ms;
#endif
};

// Default logging
static void default_log(http_log_level_t level, void *user_data, const char *fmt, ...) {
	// выбор куда писать: stderr по умолчанию, или FILE* из user_data
	FILE *out = stderr;
	if (user_data) {
		out = (FILE *)user_data;
	}
	// Префикс уровня

	const char *lvl = "ERROR";
	if (level == HTTP_LOG_WARN)  lvl = "WARN";
	else if (level == HTTP_LOG_INFO)  lvl = "INFO";
	else if (level == HTTP_LOG_DEBUG) lvl = "DEBUG";
	// Печатаем [LEVEL] 
	if (level == HTTP_LOG_DEBUG)
		fprintf(out, "[\033[0;32m%s\033[0m] ", lvl);
	else if (level == HTTP_LOG_ERROR)
		fprintf(out, "[\033[0;31m%s\033[0m] ", lvl);

	// Печатаем само сообщение через va_list
	va_list args;
	va_start(args, fmt);
	vfprintf(out, fmt, args);
	va_end(args);

	fprintf(out, "\n");
	// можно fflush(out) если нужно немедленно сбросить буфер
}

// Utility: get current time
static void timeval_now(struct timeval *tv)
{
    gettimeofday(tv, NULL);
}
// timeval difference in ms
static inline double timeval_diff_ms(const struct timeval *start, const struct timeval *end)
{
    double s = start->tv_sec + start->tv_usec / 1e6;
    double e = end->tv_sec + end->tv_usec / 1e6;
    return (e - s) * 1000.0;
}

// Allocate via config or default
static void *http_malloc(http_ctx_t *ctx, size_t sz) {
    if (ctx->config.malloc_fn) return ctx->config.malloc_fn(sz);
    return malloc(sz);
}
static void http_free_mem(http_ctx_t *ctx, void *p) {
    if (ctx->config.free_fn) ctx->config.free_fn(p);
    else free(p);
}

// Non-blocking set
static int set_nonblocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0) {
		return -1;
	}
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		return -1;
	}
	return 0;
}

// Parse "ip:port". If ip is NULL or empty, use INADDR_ANY. Supports IPv4 only for now.
// Парсит "ip:port". Если ip пустая часть до ":", используется INADDR_ANY.
// Поддерживает IPv4 и IPv6 (через getaddrinfo). Возвращает bound socket fd или -1.
static int create_and_bind(const char *address) {
	// address формата "host:port"
	char host[256] = {0};
	char serv[16] = {0};

	const char *p = strchr(address, ':');
	if (!p) {
		// Некорректный формат
		return -1;
	}
	size_t hlen = p - address;
	if (hlen >= sizeof(host)) {
		// Слишком длинный хост
		return -1;
	}
	if (hlen > 0) {
		memcpy(host, address, hlen);
		host[hlen] = '\0';
	} else {
		host[0] = '\0'; // INADDR_ANY / in6addr_any
	}
	// Копируем порт
	strncpy(serv, p + 1, sizeof(serv) - 1);
	serv[sizeof(serv) - 1] = '\0';

	struct addrinfo hints;
	struct addrinfo *res = NULL, *rp = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;       // IPv4 или IPv6
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;       // для bind INADDR_ANY, если host пустой

	int err = getaddrinfo(hlen ? host : NULL, serv, &hints, &res);
	if (err) {
		// getaddrinfo возвратил ошибку. Логировать невозможно через ctx, т.к. ctx неизвестен здесь.
		// Можно печатать stderr или оставить тихо:
		fprintf(stderr, "[ERROR] create_and_bind: getaddrinfo(%s, %s) failed: %s\n",
				host[0] ? host : "<any>", serv, gai_strerror(err));
		return -1;
	}

	int listen_fd = -1;
	for (rp = res; rp; rp = rp->ai_next) {
		listen_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (listen_fd < 0) {
			continue; // пробуем следующий адрес
		}
		// Устанавливаем SO_REUSEADDR, чтобы можно было быстро перезапускать сервер в тестах
		int opt = 1;
		if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
			// Ошибка setsockopt, но продолжаем — не фатально
			fprintf(stderr, "[WARN] create_and_bind: setsockopt SO_REUSEADDR failed: %s\n", strerror(errno));
		}
#ifdef SO_REUSEPORT
		// При желании можно поставить SO_REUSEPORT для балансировки в многопоточном окружении:
		// if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
		//     fprintf(stderr, "[WARN] create_and_bind: setsockopt SO_REUSEPORT failed: %s\n", strerror(errno));
		// }
#endif
		// Попытка bind
		if (bind(listen_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
			// Успешно связали
			break;
		}
		// Не удалось bind: закроем и попробуем следующий addrinfo
		fprintf(stderr, "[WARN] create_and_bind: bind(%s:%s) failed: %s\n",
				host[0] ? host : "0.0.0.0", serv, strerror(errno));
		close(listen_fd);
		listen_fd = -1;
	}
	freeaddrinfo(res);

	// Если ни один адрес не связался, listen_fd остаётся -1
	if (listen_fd < 0) {
		fprintf(stderr, "[ERROR] create_and_bind: unable to bind to %s\n", address);
		return -1;
	}
	return listen_fd;
}

// Accept new connection
static void accept_new(http_ctx_t *ctx, int listen_fd) {
	HTTP_DBG(ctx, "accept_new: Inception on listen_fd=%d", listen_fd);
	struct sockaddr_storage ss;
	socklen_t slen = sizeof(ss);
	int fd = accept(listen_fd, (struct sockaddr*)&ss, &slen);
	if (fd < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			HTTP_ERR(ctx, "accept_new: accept() failed: %s", strerror(errno));
		}
		return;
	}
	HTTP_DBG(ctx, "accept_new: accepted fd=%d", fd);

	if (set_nonblocking(fd) < 0) {
		HTTP_ERR(ctx, "accept_new: set_nonblocking(fd=%d) failed: %s", fd, strerror(errno));
	} else {
		HTTP_DBG(ctx, "accept_new: fd=%d set non-blocking", fd);
	}

	http_conn_t *c = http_malloc(ctx, sizeof(*c));
	if (!c) {
		HTTP_ERR(ctx, "accept_new: http_malloc failed for connection");
		close(fd);
		return;
	}
	memset(c, 0, sizeof(*c));
	c->fd = fd;
	c->state = CONN_STATE_READING;
	c->is_client = 0;
	dynbuf_init(&c->rb);
	dynbuf_init(&c->wb);
	timeval_now(&c->start_time);
	c->keep_alive = 0;
	// init req/res
	c->req.method = NULL;
	c->req.uri_path = NULL;
	c->req.uri_query = NULL;
	c->req.headers = NULL;
	c->req.num_headers = 0;
	c->req.body = NULL;
	c->req.body_len = 0;
	c->req.peer_addr = NULL;
	c->req.internal = NULL;
	http_response_init(&c->res);

	// get peer addr string
	char host[NI_MAXHOST], serv[NI_MAXSERV];
	if (getnameinfo((struct sockaddr*)&ss, slen, host, sizeof(host),
					serv, sizeof(serv),
					NI_NUMERICHOST|NI_NUMERICSERV) == 0) {
		size_t len = strlen(host) + 1 + strlen(serv) + 1;
		char *pa = http_malloc(ctx, len);
		if (pa) {
			snprintf(pa, len, "%s:%s", host, serv);
			c->req.peer_addr = pa;
			HTTP_DBG(ctx, "accept_new: peer_addr=%s", pa);
		}
	} else {
		HTTP_DBG(ctx, "accept_new: getnameinfo failed for fd=%d", fd);
	}

	// add to list
	c->next = ctx->conns;
	ctx->conns = c;
#ifdef HTTP_ENABLE_MONITORING
	ctx->metrics.active_connections++;
	HTTP_DBG(ctx, "accept_new: active_connections=%lu",
			(unsigned long)ctx->metrics.active_connections);
#endif
	HTTP_DBG(ctx, "accept_new: Exit, connection fd=%d added", fd);
}

// Remove connection
static void close_conn(http_ctx_t *ctx, http_conn_t *c_prev, http_conn_t *c) {
    if (c_prev) c_prev->next = c->next;
    else ctx->conns = c->next;
    close(c->fd);
    dynbuf_free(&c->rb);
    dynbuf_free(&c->wb);
    if (c->req.headers) {
        for (size_t i = 0; i < c->req.num_headers; i++) {
            // headers point into rb.data; no free needed
        }
        http_free_mem(ctx, c->req.headers);
    }
    if (c->req.peer_addr) http_free_mem(ctx, (void*)c->req.peer_addr);
    // free response headers
    http_response_free(&c->res);
#ifdef HTTP_ENABLE_MONITORING
    ctx->metrics.active_connections--;
#endif
    http_free_mem(ctx, c);
}
/*
// Simple header parsing state machine: parse until \r\n\r\n in rb
static int parse_request(http_ctx_t *ctx, http_conn_t *c) {
    // Check if headers already parsed
    if (c->header_parsed) return 1;
    char *data = c->rb.data;
    size_t len = c->rb.len;
    // find header end
    char *p = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (data[i]=='\r' && data[i+1]=='\n' && data[i+2]=='\r' && data[i+3]=='\n') {
            p = data + i + 4;
            size_t header_len = i + 4;
            // parse request line and headers
            // duplicate header block for parsing convenience
            char *hdr = malloc(header_len + 1);
            if (!hdr) return -1;
            memcpy(hdr, data, header_len);
            hdr[header_len] = '\0';
            char *line = hdr;
            char *next_line;
            // Request line
            next_line = strstr(line, "\r\n");
            if (!next_line) { free(hdr); return -1; }
            *next_line = '\0';
            // parse method, URI, version
            char method[16], uri[1024], version[16];
            if (sscanf(line, "%15s %1023s %15s", method, uri, version) != 3) {
                free(hdr); return -1;
            }
            c->req.method = strdup(method);
            // split URI into path and query
            char *q = strchr(uri, '?');
            if (q) {
                *q = '\0';
                c->req.uri_path = strdup(uri);
                c->req.uri_query = strdup(q+1);
            } else {
                c->req.uri_path = strdup(uri);
                c->req.uri_query = NULL;
            }
            if (strncmp(version, "HTTP/", 5)==0) {
                int maj=1,min=0;
                sscanf(version+5, "%d.%d", &maj, &min);
                c->req.http_major=maj;
                c->req.http_minor=min;
            } else {
                c->req.http_major=1;
                c->req.http_minor=0;
            }
            // Headers
            size_t hdr_count = 0;
            char *hdr_line = next_line + 2;
            // Count headers
            char *tmp = hdr_line;
            while (tmp && *tmp) {
                char *nl = strstr(tmp, "\r\n");
                if (!nl || nl == tmp) break;
                hdr_count++;
                tmp = nl + 2;
            }
            c->req.headers = http_malloc(ctx, sizeof(*c->req.headers) * hdr_count);
            if (!c->req.headers) { free(hdr); return -1; }
            c->req.num_headers = 0;
            tmp = hdr_line;
            while (tmp && *tmp) {
                char *nl = strstr(tmp, "\r\n");
                if (!nl || nl == tmp) break;
                *nl = '\0';
                char *colon = strchr(tmp, ':');
                if (colon) {
                    *colon = '\0';
                    char *name = tmp;
                    char *value = colon + 1;
                    // skip spaces
                    while (*value && isspace((unsigned char)*value)) value++;
                    c->req.headers[c->req.num_headers].name = strdup(name);
                    c->req.headers[c->req.num_headers].value = strdup(value);
                    c->req.num_headers++;
                }
                tmp = nl + 2;
            }
            // Determine Content-Length
            c->content_length = 0;
            for (size_t i = 0; i < c->req.num_headers; i++) {
                if (strcasecmp(c->req.headers[i].name, "Content-Length")==0) {
                    c->content_length = strtoul(c->req.headers[i].value, NULL, 10);
                }
                if (strcasecmp(c->req.headers[i].name, "Connection")==0) {
                    if (strcasecmp(c->req.headers[i].value, "keep-alive")==0) c->keep_alive = 1;
                }
            }
            // Body if any: check if full body received
            size_t body_received = len - header_len;
            if (body_received >= c->content_length) {
                if (c->content_length > 0) {
                    c->req.body = malloc(c->content_length+1);
                    if (!c->req.body) { free(hdr); return -1; }
                    memcpy((char*)c->req.body, p, c->content_length);
                    ((char*)c->req.body)[c->content_length] = '\0';
                    c->req.body_len = c->content_length;
                }
                c->header_parsed = 1;
            } else {
                // wait for more data
                free(hdr);
                return 0;
            }
            free(hdr);
            return 1;
        }
    }
    return 0;
}
*/


// src/common/request_parser.c

/** 
 * Вспомогательная функция: находит конец заголовков (\r\n\r\n).
 * @return указатель на byte после "\r\n\r\n" или NULL, если ещё нет полного блока.
 */
static char *find_header_end(char *data, size_t len) {
    for (size_t i = 0; i + 3 < len; i++) {
        if (data[i]=='\r' && data[i+1]=='\n'
         && data[i+2]=='\r' && data[i+3]=='\n') {
            return data + i + 4;
        }
    }
    return NULL;
}

/**
 * Парсит строку запроса (метод, URI, версия).
 * @param hdr_block — нуль-терминированный блок заголовков.
 * @return 0 на успех, <0 при ошибке.
 */
static int parse_request_line(http_conn_t *c, char *hdr_block) {

	HTTP_DBG(NULL, "Entering hdr_block: %s", hdr_block);
	char *line_end = strstr(hdr_block, "\r\n");
	if (!line_end) {
		HTTP_ERR(NULL, "Request line not terminated");
		return -1;
	}
	*line_end = '\0';

	char method[16], uri[1024], version[16];
	if (sscanf(hdr_block, "%15s %1023s %15s", method, uri, version) != 3) {
		HTTP_ERR(NULL, "Malformed request line");
		return -1;
	}
	// Копируем в структуру
	c->req.method = strdup(method);
	if (!c->req.method) {
		HTTP_ERR(NULL, "Out of memory copying method");
		return -1;
	}
	// Разбиваем URI на path и query
	char *q = strchr(uri, '?');
	if (q) {
		*q = '\0';
		c->req.uri_path  = strdup(uri);
		c->req.uri_query = strdup(q + 1);
	} else {
		c->req.uri_path  = strdup(uri);
		c->req.uri_query = NULL;
	}
	if (!c->req.uri_path 
	 || (q && !c->req.uri_query)) {
		HTTP_ERR(NULL, "Out of memory copying URI");
		return -1;
	}
	// Парсим версию
	if (strncmp(version, "HTTP/", 5) == 0) {
		if (sscanf(version + 5, "%d.%d",
				   &c->req.http_major,
				   &c->req.http_minor) != 2) {
			c->req.http_major = 1;
			c->req.http_minor = 0;
		}
	} else {
		c->req.http_major = 1;
		c->req.http_minor = 0;
	}
	HTTP_DBG(NULL, "Exiting: method: %s, path: %s, HTTP: %d.%d",
			c->req.method, c->req.uri_path, c->req.http_major, c->req.http_minor);
	return 0;
}

/**
 * Парсит все заголовки из блока (после Request-Line до пустой строки).
 * @param hdr_start — первый символ после CRLF в Request-Line.
 * @param hdr_len   — длина блока заголовков (до CRLFCRLF).
 * @return 0 на успех, <0 при ошибке.
 */
static int parse_headers(http_ctx_t *ctx, http_conn_t *c, 
                         char *hdr_start, size_t hdr_len) {
    HTTP_DBG(ctx, "Entering hdr_len: %zu, hdr_start:\n%s ", hdr_len, hdr_start);
    size_t hdr_count = 0;
    char *tmp = hdr_start;

    // Считаем количество строк-заголовков
    while (tmp < hdr_start + hdr_len && *tmp) {
        char *nl = strstr(tmp, "\r\n");
        if (!nl || nl == tmp) break;
        hdr_count++;
        tmp = nl + 2;
    }

    // Выделяем массив
    c->req.headers = http_malloc(ctx,
        sizeof(*c->req.headers) * hdr_count);
    if (!c->req.headers) {
        HTTP_ERR(ctx, "Out of memory allocating headers array");
        return -1;
    }

    // Заполняем
    c->req.num_headers = 0;
    tmp = hdr_start;
    while (tmp < hdr_start + hdr_len && *tmp) {
        char *nl = strstr(tmp, "\r\n");
        if (!nl || nl == tmp) break;
        *nl = '\0';
        char *colon = strchr(tmp, ':');
        if (colon) {
            *colon = '\0';
            char *name  = tmp;
            char *value = colon + 1;
            while (*value && isspace((unsigned char)*value))
                value++;
            c->req.headers[c->req.num_headers].name  = strdup(name);
            c->req.headers[c->req.num_headers].value = strdup(value);
            if (!c->req.headers[c->req.num_headers].name
             || !c->req.headers[c->req.num_headers].value) {
                HTTP_ERR(ctx, "Out of memory copying header");
                return -1;
            }
            c->req.num_headers++;
        }
        tmp = nl + 2;
    }
    HTTP_DBG(ctx, "Exiting: headers=%zu", c->req.num_headers);
    return 0;
}

/**
 * Извлекает тело запроса, если оно полностью получено.
 * @param body_start — указатель на первый байт после CRLFCRLF.
 * @param total_len  — длина буфера rb.len.
 * @param header_len — число байт до тела.
 * @return 1, если тело собрано и готово; 0 — ещё ждём; <0 — ошибка.
 */
static int parse_body(http_ctx_t *ctx, http_conn_t *c,
					  char *body_start, size_t total_len,
					  size_t header_len) {
	HTTP_DBG(ctx, "Entering parse_body");
	size_t received = total_len - header_len;
	if (received < c->content_length) {
		HTTP_DBG(ctx, "Body incomplete: %zu/%zu", received, c->content_length);
		return 0;
	}
	if (c->content_length > 0) {
		c->req.body = malloc(c->content_length + 1);
		if (!c->req.body) {
			HTTP_ERR(ctx, "Out of memory copying body");
			return -1;
		}
		memcpy((char *)c->req.body, body_start, c->content_length);
		((char*)c->req.body)[c->content_length] = '\0';
		c->req.body_len = c->content_length;
	}
	HTTP_DBG(ctx, "Exiting parse_body: body_len=%zu", c->req.body_len);
	return 1;
}

/**
 * Основная функция — собирает всё вместе.
 */
int parse_request(http_ctx_t *ctx, http_conn_t *c) {
	HTTP_DBG(ctx, "ENTER");
	if (c->header_parsed) {
		HTTP_DBG(c, "Already parsed");
		return 1;
	}

	char *data = c->rb.data;
	size_t len = c->rb.len;
	char *body_start = find_header_end(data, len);
	if (!body_start) {
		HTTP_DBG(ctx, "Waiting for headers");
		return 0;
	}

	size_t header_len = body_start - data;
	// Дублируем блок для простоты разбиения
	char *hdr_block = malloc(header_len + 1);
	if (!hdr_block) {
		HTTP_ERR(ctx, "Out of memory allocating hdr_block");
		return -1;
	}
	memcpy(hdr_block, data, header_len);
	hdr_block[header_len] = '\0';

	// Парсинг Request-Line
	if (parse_request_line(c, hdr_block) < 0) {
		goto err_free_hdr;
	}
	// Парсинг заголовков
	if (parse_headers(ctx, c, hdr_block + strlen(hdr_block) + 2,
					  header_len - (strlen(hdr_block) + 2)) < 0) {
		goto err_free_hdr;
	}
	// Вычисляем Content-Length и keep-alive
	c->content_length = 0;
	for (size_t i = 0; i < c->req.num_headers; i++) {
		if (strcasecmp(c->req.headers[i].name, "Content-Length") == 0)
			c->content_length = strtoul(c->req.headers[i].value, NULL, 10);
		if (strcasecmp(c->req.headers[i].name, "Connection") == 0
		 && strcasecmp(c->req.headers[i].value, "keep-alive") == 0)
			c->keep_alive = 1;
	}
	// Парсинг тела
	int body_res = parse_body(ctx, c, body_start, len, header_len);
	if (body_res < 0) {
		goto err_free_hdr;
	} else if (body_res == 0) {
		free(hdr_block);
		return 0;  // ждём дальше
	}

	c->header_parsed = 1;
	free(hdr_block);
	HTTP_DBG(ctx, "EXIT: success");
	return 1;

err_free_hdr:
	free(hdr_block);
	// TODO: здесь нужно освободить всё, что было частично аллоцировано:
	//   c->req.method, c->req.uri_path, c->req.uri_query,
	//   c->req.headers[i].name/value и сам массив c->req.headers
	HTTP_ERR(ctx, "EXIT: error");
	return -1;
}


// Send response: пишет заголовки и тело ответа в сокет,
// управляет состоянием соединения (keep-alive или закрытие),
// обновляет метрики (если включено HTTP_ENABLE_MONITORING).
static int send_response(http_ctx_t *ctx, http_conn_t *c) {
    int ret = 0;

    // ----------------------
    // Вход в функцию
    // ----------------------
    // Логируем момент входа: fd (если c не NULL), текущее состояние соединения,
    // сколько байт уже отправлено (wb_sent) и сколько всего байт в буфере записи (wb.len).
    // Это помогает понять, в какой точке мы попали в send_response.
    HTTP_DBG(ctx, "Inception, fd=%d, state=%d, wb_sent=%zu, wb_len=%zu",
            c ? c->fd : -1,
            c ? (int)c->state : -1,
            c ? c->wb_sent : 0,
            c ? c->wb.len : 0);

    // Проверяем валидность входных параметров
    if (!ctx || !c) {
        // Если контекст или соединение не переданы — это ошибка конфигурации/логики.
        HTTP_ERR(ctx, "ctx or connection is NULL");
        ret = -1;
        goto exit;
    }

    // ----------------------
    // Проверка состояния соединения
    // ----------------------
    // Мы ожидаем, что соединение находится в состоянии WRITING, т.е. нам нужно отправлять данные.
    if (c->state != CONN_STATE_WRITING) {
        // Если состояние не WRITING, значит либо отправлять нечего, либо вызов некорректен.
        HTTP_DBG(ctx, "State not WRITING (state=%d), nothing to send", (int)c->state);
        ret = 0;
        goto exit;
    }

    // Лог о начале цикла отправки
    HTTP_DBG(ctx, "Start send loop: wb_sent=%zu, wb_len=%zu", c->wb_sent, c->wb.len);

    // ----------------------
    // Основной цикл отправки данных из буфера c->wb
    // ----------------------
    // Буфер c->wb хранит уже сформированный полный HTTP-ответ: статусную строку, заголовки, пустую строку и тело.
    // c->wb_sent показывает, сколько байт уже было отправлено ранее (если вызывали send несколько раз).
    // Пока wb_sent < wb.len, нужно отправить остаток.
    while (c->wb_sent < c->wb.len) {
        // Вызываем send на не блокирующем сокете
        ssize_t n = send(c->fd,
                         c->wb.data + c->wb_sent,
                         c->wb.len - c->wb_sent,
                         0);
        if (n < 0) {
            // Ошибка при отправке
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Сокет временно не готов к отправке (нет места в буфере ядра).
                // В неблокирующем режиме нужно отложить отправку и вернуться позже.
                HTTP_DBG(ctx, "send() would block (errno=%d), try later", errno);
                ret = 0;
                goto exit;
            }
            // Фатальная ошибка отправки
            HTTP_ERR(ctx, "send() failed: %s", strerror(errno));
            ret = -1;
            goto exit;
        }
        // Успешно отправили n байт
        HTTP_DBG(ctx, "Sent chunk: %zd bytes", n);
        c->wb_sent += (size_t)n;
        HTTP_DBG(ctx, "Total sent so far: %zu/%zu", c->wb_sent, c->wb.len);
        // Повторяем цикл, пока не отправим весь буфер
    }

	// Освобождаем internal структуры ответа, чтобы не держать мусор до следующего запроса
	if (c->res.internal) {
		http_response_internal_t *ri = (http_response_internal_t *)c->res.internal;
		// bodybuf уже freed в prepare_response, но на всякий случай:
		if (ri->bodybuf.data) {
			dynbuf_free(&ri->bodybuf);
		}
		// Не закрываем c->fd здесь; просто освобождаем ri
		free(ri);
		c->res.internal = NULL;
	}

    // Если вышли из цикла, значит весь буфер c->wb был отправлен.
    HTTP_DBG(ctx, "Finished sending response body");

    // ----------------------
    // Обновление метрик (если включено HTTP_ENABLE_MONITORING)
    // ----------------------
#ifdef HTTP_ENABLE_MONITORING
    {
        struct timeval now;
        timeval_now(&now);
        // Вычисляем длительность обработки запроса (от старта чтения до конца отправки)
        double dur = timeval_diff_ms(&c->start_time, &now);
        ctx->total_response_time_ms += dur;
        ctx->metrics.total_responses++;
        ctx->metrics.average_response_time_ms = ctx->metrics.total_responses
            ? ctx->total_response_time_ms / ctx->metrics.total_responses
            : 0;
        // Логируем новые метрики для отладки и мониторинга
        HTTP_DBG(ctx, "Monitoring updated: total_responses=%zu, avg_time_ms=%.2f",
                ctx->metrics.total_responses, ctx->metrics.average_response_time_ms);
    }
#endif

    // ----------------------
    // Обработка keep-alive
    // ----------------------
    // Что такое c->keep_alive?
    //   В HTTP/1.1 по умолчанию соединение является постоянным (keep-alive), если клиент не указал "Connection: close".
    //   Флаг c->keep_alive обычно устанавливается при разборе заголовков запроса:
    //     - Если запрос содержит "Connection: close", или если сервер не хочет держать соединение открытым, флаг = 0.
    //     - Иначе, если разрешаем повторное использование соединения для следующего запроса, флаг = 1.
    //   Когда keep-alive включён, после отправки ответа мы не закрываем сокет, а сбрасываем состояние соединения
    //   и готовимся принять следующий HTTP-запрос на том же соединении.
    if (c->keep_alive) {
        HTTP_DBG(ctx, "Keep-alive enabled, resetting connection for next request");

        // 1) Сброс буфера чтения (c->rb) для приёма следующего запроса.
        //    Освобождаем старый буфер и инициализируем заново пустой.
        dynbuf_free(&c->rb);
        dynbuf_init(&c->rb);
        c->header_parsed = 0;    // помечаем, что заголовки следующего запроса ещё не распарсены
        c->content_length = 0;   // сбрасываем информацию о длине тела предыдущего запроса

        // 2) Освобождение полей предыдущего запроса (c->req.*)
        //    Удаляем и обнуляем метод, путь, query, заголовки и тело предыдущего запроса.
        if (c->req.method) {
            free((void *)c->req.method);
            HTTP_DBG(ctx, "Freed req.method");
        }
        if (c->req.uri_path) {
            free((void *)c->req.uri_path);
            HTTP_DBG(ctx, "Freed req.uri_path");
        }
        if (c->req.uri_query) {
            free((void *)c->req.uri_query);
            HTTP_DBG(ctx, "Freed req.uri_query");
        }
        for (size_t i = 0; i < c->req.num_headers; i++) {
            free((void *)c->req.headers[i].name);
            free((void *)c->req.headers[i].value);
            HTTP_DBG(ctx, "Freed req header[%zu]", i);
        }
        free(c->req.headers);
        HTTP_DBG(ctx, "Freed req.headers array");
        if (c->req.body) {
            free((void *)c->req.body);
            HTTP_DBG(ctx, "Freed req.body");
        }
        // Устанавливаем поля структуры запроса в NULL/0
        c->req.method      = NULL;
        c->req.uri_path    = NULL;
        c->req.uri_query   = NULL;
        c->req.headers     = NULL;
        c->req.num_headers = 0;
        c->req.body        = NULL;
        c->req.body_len    = 0;

        // 3) Сброс буфера записи (c->wb): предыдущий ответ уже отправлен, освобождаем старый буфер и инициализируем новый.
        dynbuf_free(&c->wb);
        dynbuf_init(&c->wb);
        c->wb_sent = 0;

        // 4) Устанавливаем состояние соединения в READING, чтобы следующий цикл внешней логики
        //    начал чтение нового запроса на этом же сокете.
        c->state = CONN_STATE_READING;

        // 5) Обновляем метку времени старта обработки следующего запроса.
        timeval_now(&c->start_time);
        HTTP_DBG(ctx, "Connection reset: state set to READING, start_time updated");

        ret = 0;
        goto exit;
    }

    // ----------------------
    // Если keep-alive выключен, закрываем соединение
    // ----------------------
    HTTP_DBG(ctx, "Keep-alive disabled, closing connection after send");
    ret = 1;

exit:
    // Логируем выход из функции: возвращаемое значение, финальное состояние (state),
    // сколько всего байт отправлено и длину буфера (wb.len). Если c == NULL, выводим -1 для state.
    HTTP_DBG(ctx, "Exit, ret=%d, final state=%d, wb_sent=%zu, wb_len=%zu",
            ret,
            c ? (int)c->state : -1,
            c ? c->wb_sent : 0,
            c ? c->wb.len : 0);
    return ret;
}

// Build status line и append в c->wb
// Append headers из c->res.resp_headers
// Проверяет Content-Length в заголовках: если нет, вычисляет body_len как c->wb.len - hdrs_pos
// Append заголовок Content-Length
// Append Connection: ...\r\n
// Append "\r\n"
// Тело: "уже добавлен в http_response_write_body" ??? и ожидает, что оно лежит уже в c->wb, но сейчас тело в res.internal->bodybuf

static int prepare_response(http_ctx_t *ctx, http_conn_t *c) {
	HTTP_DBG(ctx, "Inception, fd=%d", c ? c->fd : -1);
	int ret = 0;

	if (!ctx || !c) {
		HTTP_ERR(ctx, "ctx or connection is NULL");
		ret = -1;
		goto exit;
	}

	// 1. Начинаем формировать полный HTTP-ответ в c->wb
	//    c->wb это dynbuf_t, буфер, в который мы складываем ответ для последующей отправки через send_response.
	//    Предполагаем, что c->wb инициализирован заранее (dynbuf_init).

	// 1.1 Status line
	char line[64];
	const char *reason = c->res.status_reason;
	if (!reason) {
		switch (c->res.status_code) {
			case 200: reason = "OK"; break;
			case 400: reason = "Bad Request"; break;
			case 404: reason = "Not Found"; break;
			case 500: reason = "Internal Server Error"; break;
			default: reason = ""; break;
		}
	}
	int n = snprintf(line, sizeof(line), "HTTP/1.1 %d %s\r\n", c->res.status_code, reason);
	if (n < 0 || (size_t)n >= sizeof(line)) {
		HTTP_ERR(ctx, "snprintf status line failed or truncated");
		ret = -1;
		goto exit;
	}
	if (dynbuf_append(&c->wb, line, (size_t)n) < 0) {
		HTTP_ERR(ctx, "dynbuf_append status line failed for fd=%d", c->fd);
		ret = -1;
		goto exit;
	}

	// 2. Заголовки, которые пользователь добавил через http_response_add_header	int has_length = 0;
	int has_length = 0;
	for (size_t i = 0; i < c->res.num_resp_headers; i++) {
		const char *name  = c->res.resp_headers[i].name;
		const char *value = c->res.resp_headers[i].value;
		if (!name || !value) continue;
		char buf[1024];
		int rn = snprintf(buf, sizeof(buf), "%s: %s\r\n", name, value);
		if (rn < 0 || (size_t)rn >= sizeof(buf)) {
			HTTP_ERR(ctx, "header snprintf too long: %s", name);
			ret = -1;
			goto exit;
		}
		if (dynbuf_append(&c->wb, buf, (size_t)rn) < 0) {
			HTTP_ERR(ctx, "dynbuf_append header failed for fd=%d", c->fd);
			ret = -1;
			goto exit;
		}
		if (strcasecmp(name, "Content-Length") == 0) {
			has_length = 1;
		}
		if (strcasecmp(name, "Transfer-Encoding") == 0) {
			// Если пользователь явно указал Transfer-Encoding: chunked, можно отметить, но сейчас не реализуем стриминг.
			has_length = 1;
		}
	}

	// 3. Тело из res->internal->bodybuf
	size_t body_len = 0;
	http_response_internal_t *ri = NULL;
	if (c->res.internal) {
		ri = (http_response_internal_t *)c->res.internal;
		body_len = ri->bodybuf.len;
	}

    // Если Content-Length не задан пользователем, добавляем заголовок
	if (!has_length) {
		char buf[64];
		int rn = snprintf(buf, sizeof(buf), "Content-Length: %zu\r\n", body_len);
		if (rn < 0 || (size_t)rn >= sizeof(buf)) {
			HTTP_ERR(ctx, "snprintf Content-Length failed");
			ret = -1;
			goto exit;
		}
		if (dynbuf_append(&c->wb, buf, (size_t)rn) < 0) {
			HTTP_ERR(ctx, "dynbuf_append Content-Length failed for fd=%d", c->fd);
			ret = -1;
			goto exit;
		}
	}

	// 4. Connection header (keep-alive или close)
	const char *conn_hdr = c->keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
	if (dynbuf_append(&c->wb, conn_hdr, strlen(conn_hdr)) < 0) {
		HTTP_ERR(ctx, "dynbuf_append Connection header failed for fd=%d", c->fd);
		ret = -1;
		goto exit;
	}

	// 5. Разделитель заголовков и тела End of headers
	if (dynbuf_append(&c->wb, "\r\n", 2) < 0) {
		HTTP_ERR(ctx, "dynbuf_append header terminator failed for fd=%d", c->fd);
		ret = -1;
		goto exit;
	}

	// 6. Тело: копируем из ri->bodybuf, если есть
	if (body_len > 0 && ri && ri->bodybuf.data) {
		if (dynbuf_append(&c->wb, ri->bodybuf.data, body_len) < 0) {
			HTTP_ERR(ctx, "dynbuf_append body failed for fd=%d", c->fd);
			return -1;
		}
	}

	// 7. После копирования тела: освобождаем internal->bodybuf, т.к. данные теперь в c->wb
	if (ri) {
		dynbuf_free(&ri->bodybuf);
		// Оставляем ri->conn, но сброс buf, чтобы не было повторного освобождения
		ri->bodybuf.data = NULL;
		ri->bodybuf.len = 0;
		ri->bodybuf.cap = 0;
	}

	// Body: уже добавлен в http_response_write_body
	c->state = CONN_STATE_WRITING;
	c->wb_sent = 0;

exit:
	HTTP_DBG(ctx, "Exit, ret=%d, queued bytes=%zu", ret, c ? c->wb.len : 0);
	return ret;
}

/**
 * @brief Prints the contents of an HTTP request in a readable format.
 *
 * @param req Pointer to the http_request_t to print.
 */
void print_request(const http_request_t *req) {
    if (!req) {
        fprintf(stderr, "print_request: req is NULL\n");
        return;
    }

    // Start line
    printf("=== HTTP Request ===\n");
    printf("Method: %s\n", req->method ? req->method : "<none>");
    printf("URI Path: %s\n", req->uri_path ? req->uri_path : "<none>");
    printf("Query: %s\n", req->uri_query ? req->uri_query : "<none>");
    printf("HTTP Version: %d.%d\n", req->http_major, req->http_minor);
    printf("Peer Address: %s\n", req->peer_addr ? req->peer_addr : "<unknown>");

    // Headers
    printf("\n-- Headers (%zu) --\n", req->num_headers);
    for (size_t i = 0; i < req->num_headers; ++i) {
        const char *name = req->headers[i].name ? req->headers[i].name : "<no-name>";
        const char *value = req->headers[i].value ? req->headers[i].value : "<no-value>";
        printf("%s: %s\n", name, value);
    }

    // Body
    printf("\n-- Body (length: %zu) --\n", req->body_len);
    if (req->body && req->body_len > 0) {
        // Print up to a reasonable amount
        size_t to_print = req->body_len;
        if (to_print > 1024) {
            to_print = 1024;
        }
        fwrite(req->body, 1, to_print, stdout);
        if (to_print < req->body_len) {
            printf("... (truncated)\n");
        } else {
            printf("\n");
        }
    } else {
        printf("<empty>\n");
    }

    // Client settings
    printf("\n-- Client Settings --\n");
    printf("Timeout (ms): %d\n", req->timeout_ms);
    printf("User-Agent: %s\n", req->user_agent ? req->user_agent : "<none>");
    printf("Keep-Alive: %s\n", req->keep_alive ? "yes" : "no");
    printf("Custom Data Pointer: %p\n", req->custom_data);
    printf("======================\n");
}

// Handle one connection: read or write
static int handle_conn(http_ctx_t *ctx, http_conn_t *c) {
	int ret = 0;

	if (!ctx || !c) {
		HTTP_ERR(ctx, "ctx or connection is NULL");
		ret = -1;
		goto exit;
	}

	HTTP_DBG(ctx, "Inception fd=%d state=%d", c->fd, (int)c->state);

	if (c->state == CONN_STATE_READING) {
		char buf[4096];
		ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				// Нечего читать сейчас
				ret = 0;
				goto exit;
			}
			HTTP_ERR(ctx, "recv error on fd=%d: %s", c->fd, strerror(errno));
			ret = -1;
			goto exit;
		} else if (n == 0) {
			// Клиент закрыл соединение
			HTTP_DBG(ctx, "client closed connection fd=%d", c->fd);
			ret = -1;
			goto exit;
		}

		// Добавляем прочитанные данные в буфер
		if (dynbuf_append(&c->rb, buf, (size_t)n) < 0) {
			HTTP_ERR(ctx, "dynbuf_append to read buffer failed on fd=%d", c->fd);
			ret = -1;
			goto exit;
		}
		HTTP_DBG(ctx, "read %zd bytes into buffer fd=%d, total buffered=%zu", n, c->fd, c->rb.len);

		// Парсим запрос: parse_request должен возвращать:
		//  <0 при ошибке (фатальной) → закрываем соединение
		//   0 если ещё не полный запрос
		//   1 если полный запрос готов
		//!!!!!!!!!!!непонятно что делпет эта функция
		int pr = parse_request(ctx, c);
		if (pr < 0) {
			HTTP_ERR(ctx, " parse_request failed on fd=%d", c->fd);
			ret = -1;
			goto exit;
		}
		if (pr == 1) {
			print_request(&c->req);
			// Полный запрос распарсен
#ifdef HTTP_ENABLE_MONITORING
			ctx->metrics.total_requests++;
			HTTP_DBG(ctx, "total_requests incremented to %lu", (unsigned long)ctx->metrics.total_requests);
#endif
			// Routing: ищем подходящий обработчик
			int found = 0;
			for (size_t i = 0; i < ctx->route_count; i++) {
				// Сравниваем методы и пути. При необходимости здесь можно логировать сравнение:
				if ((ctx->routes[i].method == NULL || strcasecmp(ctx->routes[i].method, c->req.method) == 0)
					&& strcmp(ctx->routes[i].pattern, c->req.uri_path) == 0) {
					HTTP_DBG(ctx, "routing matched for fd=%d, pattern=%s", c->fd, ctx->routes[i].pattern);
					/* предлогается завести структуру 
					 * typedef struct {
						http_conn_t *conn;
						dynbuf_t bodybuf;
						int headers_sent;
					} http_response_internal_t;
					и дальще ее инициализировать а в поле conn присвоеть тикущее подключение 
					чтобы в функции 
					int http_response_end(http_response_t *res)
					был доступ к дескриптору*/
					http_response_init(&c->res);
					// Сразу после инициализации internal, привязываем текущее соединение:
					if (c->res.internal) {
						http_response_internal_t *ri = (http_response_internal_t *)c->res.internal;
						ri->conn = c;
					}

					ctx->routes[i].handler(&c->req, &c->res, ctx->routes[i].user_data);
					found = 1;
					break;
				}
			}
			if (!found) {
				// Нет маршрута — 404
				// 	поскольку у нас нет зарегестрированного маршрута 
				// мы формируем ответ в структуре http_response_t
				HTTP_DBG(ctx, "no route matched for fd=%d, sending 404", c->fd);
				http_response_init(&c->res);
				// Сразу после инициализации internal, привязываем текущее соединение:
				if (c->res.internal) {
					http_response_internal_t *ri = (http_response_internal_t *)c->res.internal;
					ri->conn = c;
				}

				http_response_set_status(&c->res, 404, "Not Found");
				const char *msg = "Not Found";
				http_response_add_header(&c->res, "Content-Type", "text/plain");
				http_response_write_body(&c->res, msg, strlen(msg));
			}
			// Подготавливаем ответ в буфер c->wb
			if (prepare_response(ctx, c) < 0) {
				HTTP_ERR(ctx, "prepare_response failed on fd=%d", c->fd);
				ret = -1;
				goto exit;
			}
			// После prepare_response состояние c->state должно стать CONN_STATE_WRITING
			HTTP_DBG(ctx, "response prepared, switching to WRITING for fd=%d", c->fd);
		}
	}
/*
	if (c->state == CONN_STATE_WRITING) {
		int sr = send_response(ctx, c);
		if (sr < 0) {
			HTTP_ERR(ctx, "handle_conn: send_response failed on fd=%d", c->fd);
			ret = -1;
			goto exit;
		}
		HTTP_DBG(ctx, "handle_conn: send_response succeeded on fd=%d", c->fd);
		// В send_response, когда отправка завершена, состояние соединения может перейти в закрытие или ожидание нового запроса.
	}
*/


// Пример фрагмента внутри handle_conn или аналогичной функции:
	if (c->state == CONN_STATE_WRITING) {
	int sr = send_response(ctx, c);
	if (sr < 0) {
		// Фатальная ошибка отправки
		HTTP_ERR(ctx, "send_response failed on fd=%d", c->fd);
		// Закрыть соединение из-за ошибки
		// Возможно, проставить ret = -1, выйти из обработчика
		ret = -1;
		goto exit;
	} else if (sr == 1) {
		// Нормальное завершение: ответ отправлен, keep-alive выключен => закрываем соединение без ошибки
		ret = sr;
		HTTP_DBG(ctx, "response sent, closing connection fd=%d", c->fd);
		// Здесь ret можно оставить 0 или специфично пометить, что соединение закрыто, 
		// но это не считается ошибкой отправки.
		// Если в вашем внешнем цикле надо убрать этот сокет, делаем это здесь.
		// После cleanup_connection, обычно просто продолжаем цикл без ошибки.
	} else {
		// sr == 0: отправка не завершена (EAGAIN) или keep-alive: ожидаем дальнейших событий.
		HTTP_DBG(ctx, "send_response returned on fd=%d, state now=%d", 
				c->fd, (int)c->state);
		// Ничего особенного: либо осталось что отправить, либо уже сбросили состояние на READING.
		// Внешняя логика: если state==READING, переключаем на чтение; если state still WRITING, ждём готовности записи.
	}
}
exit:
	HTTP_DBG(ctx, "Exit fd=%d ret=%d", c ? c->fd : -1, ret);
	return ret;
}

// Timer management
static void timers_add(http_ctx_t *ctx, http_timer_t *timer)
{
    // insert sorted by due
    if (!ctx->timers || timercmp(&timer->due, &ctx->timers->due, <)) {
        timer->next = ctx->timers;
        ctx->timers = timer;
    } else {
        http_timer_t *p = ctx->timers;
        while (p->next && timercmp(&p->next->due, &timer->due, <)) p = p->next;
        timer->next = p->next;
        p->next = timer;
    }
}

static void timers_remove(http_ctx_t *ctx, int timer_id) {
    http_timer_t *p = ctx->timers, *prev = NULL;
    while (p) {
        if (p->id == timer_id) {
            if (prev) prev->next = p->next;
            else ctx->timers = p->next;
            free(p);
            return;
        }
        prev = p;
        p = p->next;
    }
}

static void timers_check(http_ctx_t *ctx) {
    HTTP_DBG(ctx, "timers_check: Inception");

    struct timeval now;
    timeval_now(&now);

    // Обрабатываем все таймеры, время которых наступило (due <= now)
    while (ctx->timers && timercmp(&ctx->timers->due, &now, <=)) {
        http_timer_t *timer = ctx->timers;
        ctx->timers = timer->next;  // извлекаем первый таймер из списка

        HTTP_DBG(ctx, "timers_check: timer id=%d due reached (interval_ms=%d)", timer->id, timer->interval_ms);

        // Вызываем callback. Он может внутри вызвать http_stop(ctx) или изменить ctx.
        // Если callback вызывает долгие операции, это происходит в контексте event loop.
        // Пользовательский cb: void (*cb)(void *user_data)
        // Здесь не защищаем от исключений, предполагаем корректность cb.
        timer->cb(timer->user_data);

        if (timer->interval_ms > 0) {
            // Повторяющийся таймер: пересчитываем next_due относительно текущего времени
            struct timeval next_due;
            timeval_now(&next_due);

            long sec = timer->interval_ms / 1000;
            long usec = (timer->interval_ms % 1000) * 1000;
            next_due.tv_sec += sec;
            next_due.tv_usec += usec;
            if (next_due.tv_usec >= 1000000) {
                next_due.tv_sec += 1;
                next_due.tv_usec -= 1000000;
            }
            timer->due = next_due;

            // Вставляем обратно в отсортированный список
            timers_add(ctx, timer);

            HTTP_DBG(ctx, "timers_check: timer id=%d rescheduled to %ld.%06ld",
                    timer->id, (long)next_due.tv_sec, (long)next_due.tv_usec);

            // Обновляем now для следующей итерации,
            // чтобы корректно сравнивать последующие таймеры.
            timeval_now(&now);
        } else {
            // Одноразовый таймер: освобождаем память
            HTTP_DBG(ctx, "timers_check: timer id=%d freed", timer->id);
            free(timer);
            // now можно не обновлять, т.к. список сдвинулся, но лучше обновить, чтобы точнее:
            timeval_now(&now);
        }
    }

    HTTP_DBG(ctx, "timers_check: Exit");
}
// Compute timeout for poll based on next timer due
static int compute_poll_timeout(http_ctx_t *ctx, int default_ms) {
    if (!ctx->timers) return default_ms;
    struct timeval now;
    timeval_now(&now);
    struct timeval due = ctx->timers->due;
    long ms = (due.tv_sec - now.tv_sec) * 1000 + (due.tv_usec - now.tv_usec) / 1000;
    if (ms < 0) return 0;
    return (int)ms;
}

// Initialize context
http_ctx_t *http_init(const http_config_t *config) 
{
	// Логируем вход в функцию
	// Но так как ctx ещё не создан, логировать через ctx нельзя.
	// Можно иметь глобальный логер или временно писать в stderr:

	fprintf(stderr, "[DEBUG] http_init: Inception\n");

	// Выделяем память для контекста
	http_ctx_t *ctx = malloc(sizeof(*ctx));
	if (!ctx) {
		// Здесь ctx==NULL, нет контекста для логирования через ctx->config.log_fn.
		// Можно логировать напрямую в stderr:
		fprintf(stderr, "[ERROR] http_init: failed to allocate http_ctx_t\n");
		return NULL;
	}
	// Нулим всё поле сразу
	memset(ctx, 0, sizeof(*ctx));

	// Если передали конфиг, копируем. Если config содержит указатели, копируется только указатель.
	if (config) {
		// Можно перед копированием логировать, но ctx->config.log_fn ещё не установлен.
		memcpy(&ctx->config, config, sizeof(ctx->config));
	}

	// Устанавливаем дефолтные значения, если из config не заданы
	if (!ctx->config.recv_buffer_size) {
		ctx->config.recv_buffer_size = 4096;
	}
	if (!ctx->config.send_buffer_size) {
		ctx->config.send_buffer_size = 4096;
	}
	// Если лог-функция не передана, ставим default_log
	if (!ctx->config.log_fn) {
		ctx->config.log_fn = default_log;
	}
	// log_user_data при этом остаётся тем, что было в config (NULL или задано пользователем)

	// Теперь можно логировать через HTTP_DBG
	HTTP_DBG(ctx, "http_init: context allocated, recv_buf=%zu, send_buf=%zu",
			ctx->config.recv_buffer_size, ctx->config.send_buffer_size);

	// Инициализируем остальные поля
	ctx->stopped = 0;
	ctx->listen_count = 0;
	ctx->conns = NULL;

	// Маршруты: выделяем начальный массив
	ctx->routes = malloc(sizeof(route_entry_t) * INITIAL_ROUTE_CAPACITY);
	if (!ctx->routes) {
		HTTP_ERR(ctx, "http_init: failed to allocate routes array");
		free(ctx);
		return NULL;
	}
	ctx->route_cap = INITIAL_ROUTE_CAPACITY;
	ctx->route_count = 0;

	// Таймеры
	ctx->timers = NULL;
	ctx->next_timer_id = 1;

#ifdef HTTP_ENABLE_MONITORING
	memset(&ctx->metrics, 0, sizeof(ctx->metrics));
	ctx->total_response_time_ms = 0;
	HTTP_DBG(ctx, "http_init: monitoring enabled, metrics initialized");
#endif

#ifdef HTTP_ENABLE_MULTITHREADING
	// Если есть параметр thread_count, можно логировать
	if (ctx->config.thread_count <= 0) {
		// По умолчанию, если не задано, можно выбирать 1 или число ядер.
		ctx->config.thread_count = 1;
	}
	HTTP_DBG(ctx, "http_init: multithreading enabled, thread_count=%d", ctx->config.thread_count);
	// Здесь можно дополнительно инициализировать мьютексы, пул потоков и т.п.
	// Например: pthread_mutex_init(&ctx->timer_lock, NULL);
#endif

	// Другие опции (TLS, allocators и т.д.) можно тоже проверить/логировать
#ifdef HTTP_ENABLE_TLS
	if (ctx->config.tls_cert_path && ctx->config.tls_key_path) {
		HTTP_DBG(ctx, "http_init: TLS enabled, cert=%s, key=%s",
				ctx->config.tls_cert_path, ctx->config.tls_key_path);
		// Здесь можно заранее подготовить контекст TLS, если нужно
	} else {
		HTTP_DBG(ctx, "http_init: TLS not configured or disabled");
	}
#endif

	// И другие флаги:
	HTTP_DBG(ctx, "http_init: enable_compression=%d, enable_chunked=%d, enable_http2=%d",
			ctx->config.enable_compression,
			ctx->config.enable_chunked,
			ctx->config.enable_http2);

	HTTP_DBG(ctx, "http_init: Exit");
	return ctx;
}

// Free context
void http_free(http_ctx_t *ctx) {
    if (!ctx) return;
    // close listeners
    for (int i = 0; i < ctx->listen_count; i++) close(ctx->listen_fds[i]);
    // close conns
    http_conn_t *c = ctx->conns;
    while (c) {
        http_conn_t *n = c->next;
        close(c->fd);
        dynbuf_free(&c->rb);
        dynbuf_free(&c->wb);
        if (c->req.headers) free(c->req.headers);
        if (c->req.peer_addr) free((void*)c->req.peer_addr);
        http_response_free(&c->res);
        free(c);
        c = n;
    }
    // routes
    for (size_t i = 0; i < ctx->route_count; i++) {
        free(ctx->routes[i].method);
        free(ctx->routes[i].pattern);
    }
    free(ctx->routes);
    // timers
    http_timer_t *t = ctx->timers;
    while (t) { http_timer_t *n = t->next; free(t); t = n; }
    free(ctx);
}

int http_listen(http_ctx_t *ctx, const char *address, http_handler_fn handler, void *user_data) {
    HTTP_DBG(ctx, "http_listen: Inception (address=%s)", address ? address : "NULL (default 0.0.0.0:80)");

    if (!ctx) {
        // Нет контекста — ошибка
        // Нечего логировать через ctx->config.log_fn, т.к. ctx==NULL; логируем в stderr
        fprintf(stderr, "[ERROR] http_listen: ctx is NULL\n");
        return -1;
    }
    if (ctx->listen_count >= MAX_LISTENERS) {
        HTTP_ERR(ctx, "http_listen: too many listeners (max=%d)", MAX_LISTENERS);
        return -1;
    }

    const char *addr = address && *address ? address : "0.0.0.0:80";

    int fd = create_and_bind(addr);
    if (fd < 0) {
        HTTP_ERR(ctx, "http_listen: create_and_bind(%s) failed", addr);
        return -1;
    }
    HTTP_DBG(ctx, "http_listen: socket created and bound, fd=%d", fd);

    if (listen(fd, SOMAXCONN) < 0) {
        HTTP_ERR(ctx, "http_listen: listen(fd=%d) failed: %s", fd, strerror(errno));
        close(fd);
        return -1;
    }
    HTTP_DBG(ctx, "http_listen: listening on %s, fd=%d", addr, fd);

    if (set_nonblocking(fd) < 0) {
        HTTP_ERR(ctx, "http_listen: set_nonblocking(fd=%d) failed: %s", fd, strerror(errno));
        // Не критично: можно продолжить, но потенциально блокирующий accept
        // Закрывать или нет — решаем здесь оставить слушающий сокет, но логируем.
    } else {
        HTTP_DBG(ctx, "http_listen: fd=%d set to non-blocking mode", fd);
    }

    // Сохраняем слушающий дескриптор
    ctx->listen_fds[ctx->listen_count++] = fd;

    // Регистрируем маршрут "catch-all" для этого слушающего сокета
    // Предполагается, что http_register_route сам кладёт handler и user_data в ctx
    int rc = http_register_route(ctx, NULL, "*", handler, user_data);
    if (rc < 0) {
        HTTP_ERR(ctx, "http_listen: http_register_route failed");
        // Несмотря на неудачу маршрутизации, сокет открыт — решаем закрыть
        close(fd);
        ctx->listen_count--;
        return -1;
    }
    HTTP_DBG(ctx, "http_listen: registered catch-all route");

    HTTP_DBG(ctx, "http_listen: Exit");
    return 0;
}

#ifdef HTTP_ENABLE_TLS
int http_listen_https(http_ctx_t *ctx,
                      const char *address __attribute__((unused)),
                      http_handler_fn handler __attribute__((unused)),
                      void *user_data __attribute__((unused))) {
    // TODO: TLS accept setup
    ctx->config.log_fn(HTTP_LOG_WARN, ctx->config.log_user_data, "HTTPS not implemented" );
    return -1;
}
#endif


int http_run(http_ctx_t *ctx)
{
	HTTP_DBG(ctx, "http_run: Inception");

	ctx->stopped = 0;

	while (!ctx->stopped) {
		int timeout = compute_poll_timeout(ctx, 1000);

		HTTP_DBG(ctx, "http_run: poll timeout = %d ms", timeout);

		int rc = http_poll(ctx, timeout);
		if (rc < 0) {
			HTTP_ERR(ctx, "http_run: http_poll() returned error %d", rc);
			// Можно тут break или continue — зависит от желаемого поведения:
			// break; // выйти из цикла
			continue; // просто пропустить итерацию
		}

		timers_check(ctx);
	}

	HTTP_DBG(ctx, "http_run: Exit");
	return 0;
}


void http_stop(http_ctx_t *ctx) {
    ctx->stopped = 1;
}

int http_poll(http_ctx_t *ctx, int timeout_ms)
{
    // Логируем вход в функцию http_poll: показываем таймаут ожидания в миллисекундах.
    HTTP_DBG(ctx, "http_poll: Inception (timeout=%d ms)", timeout_ms);

    int ret = -1;

    // Подсчитываем, сколько descriptor-ов мы будем передавать в poll:
    // Сюда входят listen-сокеты (ctx->listen_count) и все активные клиентские соединения ctx->conns.
    size_t total_fds = ctx->listen_count;

    // Обход списка соединений: для каждого увеличиваем total_fds на 1.
    for (http_conn_t *c = ctx->conns; c; c = c->next) {
        total_fds++;
    }
    // Теперь total_fds = число слушающих сокетов + число клиентских соединений.

    // Выделяем массив struct pollfd длины total_fds.
    struct pollfd *pfds = malloc(sizeof(*pfds) * total_fds);
    if (!pfds) {
        // Если не удалось выделить память — логируем ошибку и выходим.
        HTTP_ERR(ctx, "http_poll: failed to allocate pollfd array (total_fds=%zu)", total_fds);
        goto exit;
    }

    size_t idx = 0;  // Индекс в массиве pfds.

    // ===== Добавляем listen-сокеты (слушатели) в pfds =====
    // Для каждого слушающего сокета мы регистрируем событие POLLIN (входящее соединение).
    for (int i = 0; i < ctx->listen_count; i++) {
        pfds[idx].fd = ctx->listen_fds[i];
        pfds[idx].events = POLLIN;  // Нас интересует только новое соединение (чтение доступно).
        pfds[idx].revents = 0;      // Обнуляем revents перед вызовом poll.
        HTTP_DBG(ctx, "http_poll: added listener fd=%d to poll", pfds[idx].fd);
        idx++;
    }

    // ===== Добавляем клиентские соединения =====
    // Для каждого активного соединения смотрим его состояние c->state:
    // - Если соединение ожидает чтения (CONN_STATE_READING), регистрируем POLLIN.
    // - Если соединение ожидает дозаписи (CONN_STATE_WRITING), регистрируем POLLOUT.
    // - Возможно, одновременно читаем и пишем, но в нашей модели либо читаем, либо пишем.
    for (http_conn_t *c = ctx->conns; c; c = c->next) {
        pfds[idx].fd = c->fd;
        pfds[idx].events = 0;

        // Если соединение в состоянии чтения, ждем данных от клиента.
        if (c->state == CONN_STATE_READING) {
            pfds[idx].events |= POLLIN;
            HTTP_DBG(ctx, "http_poll: connection fd=%d in READING, registering POLLIN", c->fd);
        }
        // Если соединение в состоянии записи, ждем готовности сокета на запись.
        if (c->state == CONN_STATE_WRITING) {
            pfds[idx].events |= POLLOUT;
            HTTP_DBG(ctx, "http_poll: connection fd=%d in WRITING, registering POLLOUT", c->fd);
        }
        // Инициализируем revents
        pfds[idx].revents = 0;
        idx++;
    }

    // ===== Вызов poll =====
    // Ждем события на любом из файловых дескрипторов до timeout_ms миллисекунд.
    // ret = количество descriptor-ов, у которых появились события, или 0 при таймауте, или -1 при ошибке.
    ret = poll(pfds, total_fds, timeout_ms);
    if (ret < 0) {
        // Ошибка в poll (например, EINTR и т.п.) — логируем и пойдём к освобождению ресурсов.
        HTTP_ERR(ctx, "http_poll: poll() returned error %d", ret);
        goto cleanup;
    }
    // Если ret == 0, таймаут: просто не было событий. Можно выйти, освободив pfds.
    // Если ret > 0, есть события на одном или нескольких дескрипторах.
    HTTP_DBG(ctx, "http_poll: poll returned %d (ready descriptors)", ret);

    // Сбросим idx и начнем обработку результатов poll:
    idx = 0;

    // ===== Обрабатываем события на listen-сокетах =====
    // Проходим по первым ctx->listen_count элементам в pfds.
    for (int i = 0; i < ctx->listen_count; i++, idx++) {
        // Если на слушающем сокете есть POLLIN, значит пришло новое соединение.
        if (pfds[idx].revents & POLLIN) {
            HTTP_DBG(ctx, "http_poll: accepting new connection on listener fd=%d", pfds[idx].fd);
            // Вызываем функцию accept_new: она принимает новое соединение и добавляет его в ctx->conns.
            accept_new(ctx, pfds[idx].fd);
        }
        // Можно также обрабатывать ошибки POLLERR/POLLHUP, но для слушающего сокета обычно не нужно.
    }

    // ===== Обрабатываем события на клиентских соединениях =====
    // Перебираем списком ctx->conns и соответствующие pfds[idx].
    http_conn_t *prev = NULL;
    http_conn_t *c_cur = ctx->conns;
    for (; c_cur; ) {
        int handle = 0;
        short re = pfds[idx].revents;

        // Если произошли интересующие события: читаем, пишем, или ошибки/закрытие.
        if (re & (POLLIN | POLLOUT | POLLHUP | POLLERR)) {
            handle = 1;
            HTTP_DBG(ctx, "http_poll: event on fd=%d: revents=0x%x", c_cur->fd, re);
        }

        // Сохраняем указатель на следующий, т.к. current может быть удалён в handle_conn/close_conn.
        http_conn_t *next = c_cur->next;

        if (handle) {
            // Вызываем обработчик соединения: handle_conn читает или пишет данные, парсит запрос, формирует ответ и т.д.
            int rc = handle_conn(ctx, c_cur);

            // rc < 0: фатальная ошибка при обработке (например, ошибка чтения или логики) — закрываем соединение.
            // rc == 1: нормальное завершение соединения, например клиент закрыл соединение или keep-alive выключен после ответа.
            if (rc < 0) {
                HTTP_ERR(ctx, "http_poll: handle_conn error on fd=%d, closing", c_cur->fd);
                close_conn(ctx, prev, c_cur);
                // После close_conn c_cur удалён из списка, prev остаётся тем же, c_cur переходим на next.
                c_cur = next;
                idx++;
                continue;
            } else if (rc == 1) {
                // Нормальное закрытие соединения по протоколу: после отправки ответа, keep-alive выключен или клиент закрыл.
                HTTP_DBG(ctx, "http_poll: handle_conn indicated normal close on fd=%d", c_cur->fd);
                close_conn(ctx, prev, c_cur);
                c_cur = next;
                idx++;
                continue;
            }
            // rc == 0: соединение продолжает существовать, состояние (c_cur->state) могло измениться внутри handle_conn:
            // - Если запрос прочитан и ответ запланирован, могло перейти в WRITING.
            // - Если ответ отправлен и keep-alive, могло перейти в READING.
            // - Или осталось в том же состоянии: тогда ждём следующего события.
            HTTP_DBG(ctx, "http_poll: handle_conn returned 0 on fd=%d, state now=%d", c_cur->fd, (int)c_cur->state);
        }

        // Если handle == 0 или после handle_conn с rc==0: соединение остаётся в списке.
        prev = c_cur;
        c_cur = next;
        idx++;
    }

cleanup:
    // Освобождаем ранее выделенный массив pfds
    free(pfds);

exit:
    // Логируем выход из http_poll с кодом ret:
    // ret > 0: число дескрипторов с событиями,
    // ret == 0: таймаут без событий,
    // ret < 0: ошибка poll или alloc.
    HTTP_DBG(ctx, "http_poll: Exit, ret=%d", ret);
    return ret;
}

#ifdef HTTP_ENABLE_MULTITHREADING
int http_run_multithreaded(http_ctx_t *ctx, const char *address __attribute__((unused)), http_handler_fn handler __attribute__((unused)), void *user_data __attribute__((unused))) {
    ctx->config.log_fn(HTTP_LOG_WARN, ctx->config.log_user_data, "Multithreading not implemented fully");
    return -1;
}
void http_stop_multithreaded(http_ctx_t *ctx) {
    http_stop(ctx);
}
#endif

int http_register_route(http_ctx_t *ctx, const char *method, const char *route_pattern, http_handler_fn handler, void *user_data) {
    if (ctx->route_count >= ctx->route_cap) {
        size_t ncap = ctx->route_cap * 2;
        route_entry_t *nr = realloc(ctx->routes, sizeof(route_entry_t) * ncap);
        if (!nr) return -1;
        ctx->routes = nr;
        ctx->route_cap = ncap;
    }
    char *m = method ? strdup(method) : strdup("*");
    char *p = strdup(route_pattern);
    if (!m || !p) return -1;
    ctx->routes[ctx->route_count].method = m;
    ctx->routes[ctx->route_count].pattern = p;
    ctx->routes[ctx->route_count].handler = handler;
    ctx->routes[ctx->route_count].user_data = user_data;
    ctx->route_count++;
    return 0;
}

// URL encode/decode
char *http_url_encode(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    char *out = malloc(len * 3 + 1);
    char *p = out;
    for (; *src; src++) {
        unsigned char c = *src;
        if (isalnum(c) || c=='-'||c=='_'||c=='.'||c=='~') {
            *p++ = c;
        } else {
            sprintf(p, "%%%02X", c);
            p += 3;
        }
    }
    *p = '\0';
    return out;
}
char *http_url_decode(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    char *out = malloc(len + 1);
    char *p = out;
    for (; *src; src++) {
        if (*src=='%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], '\0' };
            *p++ = (char)strtol(hex, NULL, 16);
            src += 2;
        } else if (*src=='+') {
            *p++ = ' ';
        } else {
            *p++ = *src;
        }
    }
    *p = '\0';
    return out;
}

char *http_escape_json_string(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    // worst-case every char escaped as \u00XX => 6x
    char *out = malloc(len * 6 + 1);
    char *p = out;
    for (; *src; src++) {
        unsigned char c = *src;
        switch (c) {
            case '"': *p++='\\'; *p++='"'; break;
            case '\\': *p++='\\'; *p++='\\'; break;
            case '\b': *p++='\\'; *p++='b'; break;
            case '\f': *p++='\\'; *p++='f'; break;
            case '\n': *p++='\\'; *p++='n'; break;
            case '\r': *p++='\\'; *p++='r'; break;
            case '\t': *p++='\\'; *p++='t'; break;
            default:
                if (c < 0x20) {
                    sprintf(p, "\\u%04x", c);
                    p += 6;
                } else {
                    *p++ = c;
                }
        }
    }
    *p='\0';
    return out;
}

// Timer API
int http_set_timer(http_ctx_t *ctx, int delay_ms, int interval_ms, http_timer_fn cb, void *user_data)
{
	HTTP_DBG(NULL, "Inception");

	http_timer_t *timer = malloc(sizeof(*timer));
	memset(timer, 0, sizeof(*timer));

	if (!timer)
	{
		HTTP_ERR(NULL, "failed to allocate memory");
		goto exit;
	}

	if (interval_ms < 0)
		interval_ms = 0;

	timer->id = ctx->next_timer_id++;

	struct timeval now;
	timeval_now(&now);

	long sec = delay_ms / 1000;
	long usec = (delay_ms % 1000) * 1000;

	now.tv_sec += sec;
	now.tv_usec += usec;
	if (now.tv_usec >= 1000000)
	{
		now.tv_sec += 1;
	 	now.tv_usec -= 1000000;
	}

	timer->due = now;
	timer->interval_ms = interval_ms;
	timer->cb = cb;
	timer->user_data = user_data;
	timer->next = NULL;

	timers_add(ctx, timer);

exit:
	HTTP_DBG(NULL, "Exit");
	return timer->id;

}

void http_cancel_timer(http_ctx_t *ctx, int timer_id) {
    timers_remove(ctx, timer_id);
}

void http_response_init(http_response_t *res) {
    if (!res) return;
    // Сброс полей
    res->status_code = 0;
    res->status_reason = NULL;
    // Освободить старые заголовки, если вдруг вызывали несколько раз
    if (res->resp_headers) {
        for (size_t i = 0; i < res->num_resp_headers; i++) {
            free(res->resp_headers[i].name);
            free(res->resp_headers[i].value);
        }
        free(res->resp_headers);
    }
    res->resp_headers = NULL;
    res->num_resp_headers = 0;

    // Если ранее уже был internal, освободим его
    if (res->internal) {
        http_response_internal_t *old = (http_response_internal_t *)res->internal;
        dynbuf_free(&old->bodybuf);
        free(old);
        res->internal = NULL;
    }

    // Создаём новый internal для буферизации тела
    http_response_internal_t *ri = malloc(sizeof(*ri));
    if (!ri) {
        // Плохая ситуация: не удалось выделить память.
        res->internal = NULL;
        return;
    }
    // Инициализируем: conn пока не знаем здесь напрямую. 
    // Запомним позже в handle_conn: присвоим ri->conn = c (текущее соединение).
    ri->conn = NULL;
    // Инициализируем dynbuf для тела. 
    // Предположим, dynbuf_init выделяет начальный буфер (или ставится cap=0 и ждет append).
    dynbuf_init(&ri->bodybuf);
    ri->headers_sent = 0;
    res->internal = ri;
}

void http_response_set_status(http_response_t *res, int status_code, const char *reason) {
    res->status_code = status_code;
    res->status_reason = reason;
}

void http_response_add_header(http_response_t *res, const char *name, const char *value) {
    size_t n = res->num_resp_headers;
    res->resp_headers = realloc(res->resp_headers, sizeof(*res->resp_headers) * (n+1));
    res->resp_headers[n].name = strdup(name);
    res->resp_headers[n].value = strdup(value);
    res->num_resp_headers++;
}

int http_response_write_body(http_response_t *res, const void *data, size_t len) {
	HTTP_DBG(NULL, "http_response_write_body: Inception"); // Если макрос не допускает NULL, можно убрать
	int ret = 0;

	if (!res) {
		fprintf(stderr, "[ERROR] http_response_write_body: res is NULL\n");
		ret = -1;
		goto exit;
	}
	if (len == 0) {
		// Ничего не добавляем
		goto exit;
	}
	if (!data) {
		fprintf(stderr, "[ERROR] http_response_write_body: data is NULL but len=%zu\n", len);
		ret = -1;
		goto exit;
	}

	http_response_internal_t *ri = (http_response_internal_t *)res->internal;
	// Добавляем данные в буфер
	if (dynbuf_append(&ri->bodybuf, data, len) < 0) {
		fprintf(stderr, "[ERROR] http_response_write_body: dynbuf_append failed for len=%zu\n", len);
		ret = -1;
		goto exit;
	}


	HTTP_DBG(NULL,"body: %s body_len: %ld cap: %ld", ri->bodybuf.data, ri->bodybuf.len, ri->bodybuf.cap);
exit:
	HTTP_DBG(NULL, "http_response_write_body: Exit, ret=%d", ret);
	return ret;
}

int http_response_end(http_response_t *res __attribute__((unused))) {
    HTTP_DBG(NULL, "http_response_end: called");

    if (!res) {
        HTTP_ERR(NULL, "http_response_end: response pointer is NULL");
        return -1;
    }

    // Проверим, есть ли внутреннее поле internal
    if (!res->internal) {
        HTTP_DBG(NULL, "http_response_end: no internal state, nothing to do");
        return 0;
    }

    http_response_internal_t *ri = (http_response_internal_t *)res->internal;

    // Логируем основные параметры ответа
    HTTP_DBG(NULL, "http_response_end: status_code=%d", res->status_code);
    if (res->status_reason)
        HTTP_DBG(NULL, "http_response_end: status_reason='%s'", res->status_reason);

    // Заголовки
    HTTP_DBG(NULL, "http_response_end: num_resp_headers=%zu", res->num_resp_headers);
    for (size_t i = 0; i < res->num_resp_headers; i++) {
        if (res->resp_headers[i].name && res->resp_headers[i].value) {
            HTTP_DBG(NULL, "http_response_end: header[%zu]: '%s: %s'",
                     i,
                     res->resp_headers[i].name,
                     res->resp_headers[i].value);
        }
    }

    // Тело
    if (ri->bodybuf.data && ri->bodybuf.len > 0) {
        size_t preview_len = ri->bodybuf.len > 256 ? 256 : ri->bodybuf.len;
        char preview[257] = {0};
        memcpy(preview, ri->bodybuf.data, preview_len);
        preview[preview_len] = '\0';

        // Логируем первые 256 байт тела
        HTTP_DBG(NULL, "http_response_end: body length = %zu", ri->bodybuf.len);
        HTTP_DBG(NULL, "http_response_end: body (preview) = \"%s\"%s",
                 preview,
                 ri->bodybuf.len > 256 ? "..." : "");
    } else {
        HTTP_DBG(NULL, "http_response_end: body is empty");
    }

    // Обратите внимание: реальная отправка уже должна была быть выполнена
    // через prepare_response + send_response. Здесь только лог.

    HTTP_DBG(NULL, "http_response_end: completed");
    return 0;
}



void http_response_free(http_response_t *res) {
    if (!res) return;
    // Очистить заголовки
    if (res->resp_headers) {
        for (size_t i = 0; i < res->num_resp_headers; i++) {
            free(res->resp_headers[i].name);
            free(res->resp_headers[i].value);
        }
        free(res->resp_headers);
        res->resp_headers = NULL;
        res->num_resp_headers = 0;
    }
    // Очистить internal-буфер, если есть
    if (res->internal) {
        http_response_internal_t *ri = (http_response_internal_t *)res->internal;
        dynbuf_free(&ri->bodybuf);
        // Не закрываем conn здесь! Закрытие соединения делается в close_conn.
        free(ri);
        res->internal = NULL;
    }
    // Остальные поля (status_code, status_reason) можно не обнулять, т.к. структура удаляется вскоре.
}

#ifdef HTTP_ENABLE_MONITORING

int http_get_metrics(http_ctx_t *ctx, http_metrics_t *out_metrics) {
    if (!ctx || !out_metrics) return -1;
    *out_metrics = ctx->metrics;
    return 0;
}

int http_reset_metrics(http_ctx_t *ctx) {
    if (!ctx) return -1;
    HTTP_DBG(ctx, "http_reset_metrics: resetting metrics");
    ctx->metrics.total_requests = 0;
    ctx->metrics.total_responses = 0;
    ctx->metrics.total_errors = 0;
    ctx->metrics.average_response_time_ms = 0;
    ctx->total_response_time_ms = 0;
    return 0;
}

// Вспомогательная callback-функция, вызываемая таймером для метрик
static void metrics_timer_cb(void *arg) {
    http_ctx_t *ctx = (http_ctx_t *)arg;
    HTTP_DBG(ctx, "metrics_timer_cb: invoking metrics callback");
    if (ctx->config.metrics_cb) {
        // Вызываем пользовательский callback с текущими метриками
        ctx->config.metrics_cb(&ctx->metrics, ctx->config.metrics_user_data);
    }
}

// Устанавливает или обновляет периодический вызов метрик
int http_set_metrics_callback(http_ctx_t *ctx, int interval_ms, http_metrics_callback cb, void *user_data) {
    if (!ctx) {
        return -1;
    }
    // Если interval_ms <= 0 — отключаем callback
    if (interval_ms <= 0) {
        HTTP_DBG(ctx, "http_set_metrics_callback: disabling metrics callback");
        // TODO: здесь можно отменить ранее установленный таймер, если храним его id
        ctx->config.metrics_interval_ms = 0;
        ctx->config.metrics_cb = NULL;
        ctx->config.metrics_user_data = NULL;
        return 0;
    }
    // Устанавливаем в конфигурацию
    ctx->config.metrics_interval_ms = interval_ms;
    ctx->config.metrics_cb = cb;
    ctx->config.metrics_user_data = user_data;
    HTTP_DBG(ctx, "http_set_metrics_callback: scheduling periodic metrics every %d ms", interval_ms);

    // Запускаем таймер через metrics_timer_cb
    // Если нужно отменить старый таймер, для этого придётся хранить его ID в ctx, например:
    //    if (ctx->metrics_timer_id > 0) http_cancel_timer(ctx, ctx->metrics_timer_id);
    //    ctx->metrics_timer_id = http_set_timer(ctx, interval_ms, interval_ms, metrics_timer_cb, ctx);
    // Для простоты, если ранее таймер не хранится, просто ставим новый:
    int timer_id = http_set_timer(ctx, interval_ms, interval_ms, metrics_timer_cb, ctx);
    if (timer_id < 0) {
        HTTP_ERR(ctx, "http_set_metrics_callback: failed to set timer for metrics");
        return -1;
    }
    // Если вы хотите потом уметь отменять этот таймер, подумайте о добавлении в http_ctx_t поля:
    //    int metrics_timer_id;
    // и сохранении туда timer_id.
    return 0;
}

#endif // HTTP_ENABLE_MONITORING


#ifdef HTTP_ENABLE_SELF_TESTS
int http_run_self_tests(void) {
    // Simple tests for URL utils
    // TODO: implement more tests
    char *enc = http_url_encode("abc 123/");
    char *dec = http_url_decode(enc);
    int ok = strcmp(dec, "abc 123/") == 0;
    free(enc);
    free(dec);
    return ok ? 0 : 1;
}
int http_register_test(const char *test_name __attribute__((unused)),
                       http_test_fn fn __attribute__((unused))) {
    // stub: user can store tests externally
    return 0;
}
#endif

void http_request_free(http_request_t *req) {
    // free allocated fields
    if (req->method) free((void*)req->method);
    if (req->uri_path) free((void*)req->uri_path);
    if (req->uri_query) free((void*)req->uri_query);
    if (req->headers) {
        for (size_t i = 0; i < req->num_headers; i++) {
            free((void*)req->headers[i].name);
            free((void*)req->headers[i].value);
        }
        free(req->headers);
    }
    if (req->body) free((void*)req->body);
    if (req->peer_addr) free((void*)req->peer_addr);
}

// MIME types
typedef struct { const char *ext; const char *type; } mime_map_t;
static const mime_map_t mime_map[] = {
    {"html", "text/html"},
    {"htm", "text/html"},
    {"css", "text/css"},
    {"js", "application/javascript"},
    {"json", "application/json"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"txt", "text/plain"},
    {"pdf", "application/pdf"},
    {NULL, "application/octet-stream"}
};
const char *http_mime_type_from_ext(const char *ext) {
    if (!ext) return "application/octet-stream";
    for (int i = 0; mime_map[i].ext; i++) {
        if (strcasecmp(ext, mime_map[i].ext)==0) return mime_map[i].type;
    }
    return "application/octet-stream";
}
/*
int http_client_request_async(http_ctx_t *ctx ,
                              const char *url __attribute__((unused)),
                              const char *method __attribute__((unused)),
                              const char * const headers[] __attribute__((unused)),
                              const void *body __attribute__((unused)),
							  size_t body_len __attribute__((unused)),
                              void (*cb)(http_response_t *res, void *user_data) __attribute__((unused)),
                              void *user_data __attribute__((unused))) {
    // TODO: integrate into event loop
    ctx->config.log_fn(HTTP_LOG_WARN, ctx->config.log_user_data, "Async client not implemented");
    return -1;
}
*/


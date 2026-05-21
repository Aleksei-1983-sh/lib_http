# HTTP Library (`libs/http`)

Этот `README` описывает публичный API, который декларирует [libs/http/http.h](/home/di/projects_С/git_progect/libs_v0.10/libs/http/http.h:1), и связанные с ним заголовки:

- [libs/http/config.h](/home/di/projects_С/git_progect/libs_v0.10/libs/http/config.h:1)
- [libs/http/http_request.h](/home/di/projects_С/git_progect/libs_v0.10/libs/http/http_request.h:1)
- [libs/http/http_response.h](/home/di/projects_С/git_progect/libs_v0.10/libs/http/http_response.h:1)
- [libs/http/metrics.h](/home/di/projects_С/git_progect/libs_v0.10/libs/http/metrics.h:1)

Ниже описано, какие возможности библиотека заявляет публично, как они группируются и как этим API пользоваться.

## Что это за библиотека

`libs/http` позиционируется как легковесная HTTP/HTTPS библиотека на C с event-driven моделью. В публичном API заявлены:

- HTTP сервер
- HTTP/HTTPS запуск через event loop
- базовая маршрутизация
- таймеры внутри event loop
- подготовка HTTP-ответов
- работа с HTTP-запросом в обработчике
- utility-функции для URL, JSON и MIME
- логирование
- метрики
- self-tests
- optional multithreading

## Версия и основные типы

В [libs/http/http.h](/home/di/projects_С/git_progect/libs_v0.10/libs/http/http.h:1) объявлены:

- `HTTP_LIB_VERSION`
- `http_ctx_t` — opaque context библиотеки
- `http_handler_fn` — callback обработчика HTTP-запроса
- `http_timer_fn` — callback таймера
- `http_test_fn` — callback self-test

Главные структуры, с которыми работает пользователь:

- `http_config_t` — конфигурация библиотеки
- `http_request_t` — разобранный HTTP-запрос
- `http_response_t` — формируемый HTTP-ответ
- `http_metrics_t` — снимок метрик, если включён monitoring

## 1. Инициализация и жизненный цикл

Публичный API:

```c
http_ctx_t *http_init(const http_config_t *config);
void http_free(http_ctx_t *ctx);
```

Назначение:

- `http_init` создаёт контекст библиотеки
- `http_free` освобождает контекст и связанные ресурсы

Минимальная заготовка:

```c
#include "http.h"
#include <string.h>

int main(void) {
    http_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.recv_buffer_size = 4096;
    cfg.send_buffer_size = 4096;
    cfg.max_connections = 128;
    cfg.keep_alive_timeout_ms = 10000;
    cfg.request_timeout_ms = 5000;
    cfg.enable_chunked = 1;

    http_ctx_t *ctx = http_init(&cfg);
    if (!ctx) {
        return 1;
    }

    http_free(ctx);
    return 0;
}
```

## 2. Конфигурация `http_config_t`

В [libs/http/config.h](/home/di/projects_С/git_progect/libs_v0.10/libs/http/config.h:1) декларируются такие группы настроек:

### Базовые параметры сервера

- `recv_buffer_size`
- `send_buffer_size`
- `max_connections`
- `keep_alive_timeout_ms`
- `request_timeout_ms`

### TLS-параметры

Доступны только при `HTTP_ENABLE_TLS`:

- `tls_cert_path`
- `tls_key_path`
- `tls_ca_path`
- `tls_verify_peer`

### Параметры многопоточности

Доступны только при `HTTP_ENABLE_MULTITHREADING`:

- `thread_count`

### Параметры monitoring

Доступны только при `HTTP_ENABLE_MONITORING`:

- `metrics_interval_ms`
- `metrics_cb`
- `metrics_user_data`

### Логирование

- `log_fn`
- `log_user_data`

### Кастомные аллокаторы

- `malloc_fn`
- `free_fn`

### Feature flags

- `enable_compression`
- `enable_chunked`
- `enable_http2`

Пример конфигурации с логированием:

```c
#include "http.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static void my_log(http_log_level_t level, void *user_data, const char *fmt, ...) {
    (void)user_data;

    const char *name = "ERROR";
    if (level == HTTP_LOG_WARN)  name = "WARN";
    if (level == HTTP_LOG_INFO)  name = "INFO";
    if (level == HTTP_LOG_DEBUG) name = "DEBUG";

    fprintf(stderr, "[%s] ", name);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fputc('\n', stderr);
}

int main(void) {
    http_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.log_fn = my_log;
    cfg.enable_chunked = 1;

    http_ctx_t *ctx = http_init(&cfg);
    if (!ctx) {
        return 1;
    }

    http_free(ctx);
    return 0;
}
```

## 3. Запуск HTTP-сервера

Публичный API:

```c
int http_listen(http_ctx_t *ctx, const char *address, http_handler_fn handler, void *user_data);
int http_run(http_ctx_t *ctx);
void http_stop(http_ctx_t *ctx);
int http_poll(http_ctx_t *ctx, int timeout_ms);
```

Назначение:

- `http_listen` открывает listening socket на адресе вида `"ip:port"`
- `http_run` запускает blocking event loop
- `http_stop` останавливает event loop
- `http_poll` делает одну итерацию цикла вручную

### Самый простой сервер

```c
#include "http.h"
#include <string.h>

static void hello_handler(http_request_t *req, http_response_t *res, void *user_data) {
    (void)req;
    (void)user_data;

    http_response_init(res);
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/plain");
    http_response_write_body(res, "Hello, world!\n", 14);
    http_response_end(res);
}

int main(void) {
    http_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    http_ctx_t *ctx = http_init(&cfg);
    if (!ctx) {
        return 1;
    }

    if (http_listen(ctx, "0.0.0.0:8080", hello_handler, NULL) != 0) {
        http_free(ctx);
        return 1;
    }

    http_run(ctx);
    http_free(ctx);
    return 0;
}
```

### Ручная интеграция через `http_poll`

Если нужен свой цикл:

```c
while (!should_stop) {
    int rc = http_poll(ctx, 100);
    if (rc < 0) {
        break;
    }

    /* своя логика приложения */
}
```

## 4. Маршрутизация

Публичный API:

```c
int http_register_route(http_ctx_t *ctx,
                        const char *method,
                        const char *route_pattern,
                        http_handler_fn handler,
                        void *user_data);
```

Назначение:

- регистрирует маршрут по HTTP-методу и шаблону пути
- `method` обычно `"GET"`, `"POST"` и т.д.
- `route_pattern` ожидается как путь вроде `"/health"` или `"/api/item/{id}"`

Пример:

```c
static void health_handler(http_request_t *req, http_response_t *res, void *ud) {
    (void)req;
    (void)ud;

    http_response_init(res);
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/plain");
    http_response_write_body(res, "OK", 2);
    http_response_end(res);
}

static void echo_handler(http_request_t *req, http_response_t *res, void *ud) {
    (void)ud;

    http_response_init(res);
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/plain");

    if (req->body && req->body_len > 0) {
        http_response_write_body(res, req->body, req->body_len);
    }

    http_response_end(res);
}

/* после http_init(ctx) */
http_register_route(ctx, "GET",  "/health", health_handler, NULL);
http_register_route(ctx, "POST", "/echo",   echo_handler,   NULL);
```

## 5. Что доступно в `http_request_t`

Структура запроса из [libs/http/http_request.h](/home/di/projects_С/git_progect/libs_v0.10/libs/http/http_request.h:1):

- `method` — метод HTTP
- `uri_path` — путь запроса
- `uri_query` — query string без `?`
- `http_major`, `http_minor` — версия HTTP
- `headers`, `num_headers` — массив заголовков
- `body`, `body_len` — тело запроса
- `peer_addr` — строковый адрес клиента
- `timeout_ms`, `user_agent`, `keep_alive`, `custom_data` — дополнительные поля структуры

Пример чтения запроса:

```c
static void inspect_handler(http_request_t *req, http_response_t *res, void *ud) {
    (void)ud;

    http_response_init(res);
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/plain");

    if (req->method) {
        http_response_write_body(res, req->method, strlen(req->method));
        http_response_write_body(res, "\n", 1);
    }

    if (req->uri_path) {
        http_response_write_body(res, req->uri_path, strlen(req->uri_path));
        http_response_write_body(res, "\n", 1);
    }

    for (size_t i = 0; i < req->num_headers; ++i) {
        http_response_write_body(res, req->headers[i].name, strlen(req->headers[i].name));
        http_response_write_body(res, ": ", 2);
        http_response_write_body(res, req->headers[i].value, strlen(req->headers[i].value));
        http_response_write_body(res, "\n", 1);
    }

    http_response_end(res);
}
```

Очистка структуры запроса:

```c
http_request_free(req);
```

Обычно в обработчике входящий `req` освобождать вручную не нужно, если этим управляет сама библиотека. Эта функция полезна, если вы отдельно создаёте или копируете `http_request_t`.

## 6. Формирование ответа через `http_response_t`

Публичный API:

```c
void http_response_init(http_response_t *res);
void http_response_set_status(http_response_t *res, int status_code, const char *reason);
void http_response_add_header(http_response_t *res, const char *name, const char *value);
int http_response_write_body(http_response_t *res, const void *data, size_t len);
int http_response_end(http_response_t *res);
void http_response_free(http_response_t *res);
```

Типичный шаблон обработчика:

```c
static void json_handler(http_request_t *req, http_response_t *res, void *ud) {
    (void)req;
    (void)ud;

    const char *body = "{\"status\":\"ok\"}";

    http_response_init(res);
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "application/json");
    http_response_write_body(res, body, strlen(body));
    http_response_end(res);
}
```

Пример ответа с несколькими частями тела:

```c
static void stream_like_handler(http_request_t *req, http_response_t *res, void *ud) {
    (void)req;
    (void)ud;

    http_response_init(res);
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/plain");

    http_response_write_body(res, "part-1\n", 7);
    http_response_write_body(res, "part-2\n", 7);
    http_response_write_body(res, "part-3\n", 7);

    http_response_end(res);
}
```

## 7. Таймеры внутри event loop

Публичный API:

```c
int http_set_timer(http_ctx_t *ctx, int delay_ms, int interval_ms, http_timer_fn cb, void *user_data);
void http_cancel_timer(http_ctx_t *ctx, int timer_id);
```

Назначение:

- one-shot timer: `interval_ms == 0`
- periodic timer: `interval_ms > 0`

Пример:

```c
#include <stdio.h>

static void timer_cb(void *user_data) {
    const char *name = user_data ? (const char *)user_data : "timer";
    printf("timer fired: %s\n", name);
}

int timer_id = http_set_timer(ctx, 1000, 0, timer_cb, "once");
if (timer_id >= 0) {
    /* при необходимости */
    http_cancel_timer(ctx, timer_id);
}
```

Пример periodic timer:

```c
http_set_timer(ctx, 5000, 5000, timer_cb, "heartbeat");
```

## 8. TLS / HTTPS

Публичный API:

```c
#ifdef HTTP_ENABLE_TLS
int http_listen_https(http_ctx_t *ctx, const char *address, http_handler_fn handler, void *user_data);
#endif
```

Назначение:

- запуск HTTPS listener
- использование TLS-полей из `http_config_t`

Заготовка:

```c
#define HTTP_ENABLE_TLS
#include "http.h"

http_config_t cfg = {0};
cfg.tls_cert_path = "server.crt";
cfg.tls_key_path = "server.key";
cfg.tls_verify_peer = 0;

http_ctx_t *ctx = http_init(&cfg);
if (!ctx) {
    return 1;
}

http_listen_https(ctx, "0.0.0.0:8443", hello_handler, NULL);
http_run(ctx);
http_free(ctx);
```

## 9. Многопоточный режим

Публичный API:

```c
#ifdef HTTP_ENABLE_MULTITHREADING
int http_run_multithreaded(http_ctx_t *ctx, const char *address, http_handler_fn handler, void *user_data);
void http_stop_multithreaded(http_ctx_t *ctx);
#endif
```

Назначение:

- запуск event loop в многопоточном режиме
- остановка многопоточного режима

Заготовка:

```c
#define HTTP_ENABLE_MULTITHREADING
#include "http.h"
#include <string.h>

http_config_t cfg;
memset(&cfg, 0, sizeof(cfg));
cfg.thread_count = 4;

http_ctx_t *ctx = http_init(&cfg);
if (!ctx) {
    return 1;
}

http_run_multithreaded(ctx, "0.0.0.0:8080", hello_handler, NULL);

/* позже */
http_stop_multithreaded(ctx);
http_free(ctx);
```

## 10. Метрики и monitoring

Публичный API доступен только при `HTTP_ENABLE_MONITORING`:

```c
int http_get_metrics(http_ctx_t *ctx, http_metrics_t *out_metrics);
int http_reset_metrics(http_ctx_t *ctx);
int http_set_metrics_callback(http_ctx_t *ctx, int interval_ms, http_metrics_callback cb, void *user_data);
```

Поля `http_metrics_t`:

- `total_requests`
- `total_responses`
- `active_connections`
- `total_errors`
- `average_response_time_ms`

Пример callback:

```c
#ifdef HTTP_ENABLE_MONITORING
static void metrics_cb(const http_metrics_t *metrics, void *user_data) {
    (void)user_data;

    printf("req=%llu resp=%llu conn=%llu err=%llu avg=%.2f ms\n",
           (unsigned long long)metrics->total_requests,
           (unsigned long long)metrics->total_responses,
           (unsigned long long)metrics->active_connections,
           (unsigned long long)metrics->total_errors,
           metrics->average_response_time_ms);
}
#endif
```

Пример настройки:

```c
#ifdef HTTP_ENABLE_MONITORING
cfg.metrics_interval_ms = 5000;
cfg.metrics_cb = metrics_cb;
#endif
```

Пример ручного чтения:

```c
#ifdef HTTP_ENABLE_MONITORING
http_metrics_t metrics;
if (http_get_metrics(ctx, &metrics) == 0) {
    /* использовать metrics */
}
#endif
```

## 11. Self-tests

Публичный API доступен только при `HTTP_ENABLE_SELF_TESTS`:

```c
int http_run_self_tests(void);
int http_register_test(const char *test_name, http_test_fn fn);
```

Назначение:

- запуск встроенных самотестов
- регистрация пользовательских тестов

Заготовка:

```c
#ifdef HTTP_ENABLE_SELF_TESTS
static int my_test(void) {
    return 0;
}

int main(void) {
    http_register_test("my_test", my_test);
    return http_run_self_tests();
}
#endif
```

## 12. Utility-функции

### URL encode / decode

Публичный API:

```c
char *http_url_encode(const char *src);
char *http_url_decode(const char *src);
```

Пример:

```c
char *enc = http_url_encode("Hello World?&=/");
char *dec = http_url_decode(enc);

/* использовать enc и dec */

free(enc);
free(dec);
```

### Экранирование JSON-строки

Публичный API:

```c
char *http_escape_json_string(const char *src);
```

Пример:

```c
char *escaped = http_escape_json_string("He said: \"Hello\"\nNew line");
/* использовать escaped */
free(escaped);
```

### MIME type lookup

Публичный API:

```c
const char *http_mime_type_from_ext(const char *ext);
```

Пример:

```c
const char *mime1 = http_mime_type_from_ext("html");
const char *mime2 = http_mime_type_from_ext("jpg");
const char *mime3 = http_mime_type_from_ext("unknown");
```

## 13. Логические сценарии использования

### REST-like server

Подходит связка:

- `http_init`
- `http_register_route`
- `http_listen`
- `http_run`
- `http_response_*`

### Event loop с фоновыми задачами

Подходит связка:

- `http_poll`
- `http_set_timer`
- `http_cancel_timer`

### Инструмент с periodic monitoring

Подходит связка:

- `HTTP_ENABLE_MONITORING`
- `http_set_metrics_callback`
- `http_get_metrics`

### HTTPS endpoint

Подходит связка:

- `HTTP_ENABLE_TLS`
- TLS-поля `http_config_t`
- `http_listen_https`

## 14. Практическая заготовка целиком

Ниже компактный пример сервера, который ближе всего к тому, как этот API предполагается использовать:

```c
#include "http.h"
#include <string.h>

static void app_handler(http_request_t *req, http_response_t *res, void *ud) {
    (void)ud;

    http_response_init(res);

    if (strcmp(req->uri_path, "/health") == 0) {
        http_response_set_status(res, 200, "OK");
        http_response_add_header(res, "Content-Type", "text/plain");
        http_response_write_body(res, "OK", 2);
    } else {
        http_response_set_status(res, 404, "Not Found");
        http_response_add_header(res, "Content-Type", "text/plain");
        http_response_write_body(res, "Not Found\n", 10);
    }

    http_response_end(res);
}

int main(void) {
    http_config_t cfg = {0};
    cfg.recv_buffer_size = 4096;
    cfg.send_buffer_size = 4096;
    cfg.max_connections = 64;
    cfg.keep_alive_timeout_ms = 10000;
    cfg.request_timeout_ms = 5000;
    cfg.enable_chunked = 1;

    http_ctx_t *ctx = http_init(&cfg);
    if (!ctx) {
        return 1;
    }

    if (http_listen(ctx, "0.0.0.0:8080", app_handler, NULL) != 0) {
        http_free(ctx);
        return 1;
    }

    http_run(ctx);
    http_free(ctx);
    return 0;
}
```

## 15. Что важно понимать при чтении API

Этот `README` описывает именно то, что публичный header декларирует пользователю. При этом часть возможностей является условной:

- TLS доступен только при `HTTP_ENABLE_TLS`
- multithreading доступен только при `HTTP_ENABLE_MULTITHREADING`
- metrics доступны только при `HTTP_ENABLE_MONITORING`
- self-tests доступны только при `HTTP_ENABLE_SELF_TESTS`

Кроме того, некоторые поля и флаги в API выглядят как задел на будущее:

- `enable_http2`
- `enable_compression`
- закомментированный async client в `http_request.h`

Если после изменений в `libs/http` публичный API, сценарии использования или список возможностей меняются, этот `README.md` нужно актуализировать вместе с кодом.

# HTTP Library (`libs/http`)

Этот `README` описывает публичный API, который декларирует [include/http/http.h](/home/di/projects_С/git_progect/libs_v0.10/include/http/http.h:1), и связанные с ним заголовки:

- [include/http/config.h](/home/di/projects_С/git_progect/libs_v0.10/include/http/config.h:1)
- [include/http/http_request.h](/home/di/projects_С/git_progect/libs_v0.10/include/http/http_request.h:1)
- [include/http/http_response.h](/home/di/projects_С/git_progect/libs_v0.10/include/http/http_response.h:1)
- [include/http/metrics.h](/home/di/projects_С/git_progect/libs_v0.10/include/http/metrics.h:1)

Ниже описано, какие возможности библиотека заявляет публично, как они группируются и как этим API пользоваться.

## Сборка и тесты

В репозитории есть базовый `Makefile` без внешних зависимостей по умолчанию.

Основные команды:

- `make` — собрать статическую библиотеку, примеры и smoke-тесты
- `make lib` — собрать `build/lib/libhttp.a`
- `make examples` — собрать demo-сервер
- `make test-smoke` — собрать и запустить smoke-тесты в дефолтной и feature-flag сборке
- `make test-unit` — собрать и запустить unit-тесты
- `make test-integration` — поднять demo-сервер и проверить маршруты через `curl`
- `make test` — прогнать все тестовые наборы
- `make check` — алиас для `make test`
- `make clean` — удалить артефакты из `build/`

Тесты разделены по типам:

- `tests/smoke/` — быстрый sanity-check сборки и основных API
- `tests/unit/` — локальная корректность utility-функций и небольших API-компонентов
- `tests/integration/` — реальные HTTP-запросы к поднятому серверу

Smoke-тесты проверяют:

- URL/JSON/MIME utility-функции
- базовый `http_response_*` API
- `http_init`, `http_register_route`, `http_set_timer`, `http_listen`, `http_poll`
- optional API под feature-флагами: metrics, self-tests, TLS stub, multithreading stub

Integration-тесты сейчас проверяют:

- `GET /health`
- `GET /echo?...`
- `POST /echo`
- `GET /headers`
- `GET /set_timer?delay=...`

## Что это за библиотека

`libs/http` позиционируется как легковесная HTTP-библиотека на C с event-driven моделью. В текущем рабочем контракте заявлены:

- HTTP сервер
- запуск через event loop
- базовая маршрутизация
- таймеры внутри event loop
- подготовка HTTP-ответов
- работа с HTTP-запросом в обработчике
- utility-функции для URL, JSON и MIME
- логирование
- optional метрики
- optional self-tests
- optional multithreading / TLS entry points

## Версия и основные типы

В [include/http/http.h](/home/di/projects_С/git_progect/libs_v0.10/include/http/http.h:1) объявлены:

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
#include "http/http.h"
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

В [include/http/config.h](/home/di/projects_С/git_progect/libs_v0.10/include/http/config.h:1) декларируются такие группы настроек:

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
#include "http/http.h"
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

### Текущие правила HTTP-ответа

Для текущей реализации `libs/http` полезно считать зафиксированными такие правила ответа:

- если обработчик сформировал ответ через `http_response_*`, библиотека буферизует его и сама собирает итоговый HTTP/1.1 response;
- если пользователь явно не добавил `Content-Length`, библиотека добавляет его автоматически по размеру накопленного body;
- библиотека всегда добавляет `Connection`:
  - `Connection: keep-alive`, если запрос был распознан как keep-alive;
  - `Connection: close` во всех остальных случаях;
- body ответа отправляется как buffered payload после пустой строки `\r\n\r\n`;
- при неизвестном пути сервер возвращает `404 Not Found`;
- если путь существует, но метод для него не зарегистрирован, сервер возвращает `405 Method Not Allowed`;
- malformed request сейчас приводит к `400 Bad Request`;
- для error-ответов текущая demo/server реализация использует plain text body.

### Канонический `server-first` пример

Этот пример синхронизирован с [examples/test_server.c](/home/di/projects_С/git_progect/libs_v0.10/examples/test_server.c:1).

```c
#include "http/http.h"
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static http_ctx_t *g_ctx = NULL;

static void server_log_cb(http_log_level_t level, void *user_data, const char *fmt, ...)
{
    (void)user_data;

    static const char *names[] = {"ERROR", "WARN", "INFO", "DEBUG"};
    va_list args;

    va_start(args, fmt);
    fprintf(stderr, "[%s] ", names[level]);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

static void on_sigint(int sig)
{
    (void)sig;
    if (g_ctx) {
        fprintf(stderr, "stopping server\n");
        http_stop(g_ctx);
    }
}

static void respond_text(http_response_t *res, int status, const char *reason, const char *body)
{
    http_response_init(res);
    http_response_set_status(res, status, reason);
    http_response_add_header(res, "Content-Type", "text/plain");
    http_response_write_body(res, body, strlen(body));
    http_response_end(res);
}

static void health_handler(http_request_t *req, http_response_t *res, void *user_data)
{
    (void)req;
    (void)user_data;
    respond_text(res, 200, "OK", "OK\n");
}

static void echo_handler(http_request_t *req, http_response_t *res, void *user_data)
{
    (void)user_data;

    http_response_init(res);
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/plain");

    if (strcmp(req->method, "GET") == 0) {
        const char *query = req->uri_query ? req->uri_query : "";
        http_response_write_body(res, query, strlen(query));
        http_response_write_body(res, "\n", 1);
    } else if (req->body && req->body_len > 0) {
        http_response_write_body(res, req->body, req->body_len);
        http_response_write_body(res, "\n", 1);
    } else {
        http_response_write_body(res, "(empty)\n", 8);
    }

    http_response_end(res);
}

static void headers_handler(http_request_t *req, http_response_t *res, void *user_data)
{
    (void)user_data;

    http_response_init(res);
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/plain");

    for (size_t i = 0; i < req->num_headers; ++i) {
        char line[512];
        int n = snprintf(line, sizeof(line), "%s: %s\n",
                         req->headers[i].name,
                         req->headers[i].value);
        if (n > 0) {
            http_response_write_body(res, line, (size_t)n);
        }
    }

    http_response_end(res);
}

static void timer_log_cb(void *user_data)
{
    const char *message = user_data ? (const char *)user_data : "timer fired";
    fprintf(stderr, "[timer] %s\n", message);
}

static void timer_handler(http_request_t *req, http_response_t *res, void *user_data)
{
    (void)user_data;

    int delay_ms = 0;
    if (!req->uri_query || sscanf(req->uri_query, "delay=%d", &delay_ms) != 1 || delay_ms <= 0) {
        respond_text(res, 400, "Bad Request", "usage: /set_timer?delay=1000\n");
        return;
    }

    int timer_id = http_set_timer(g_ctx, delay_ms, 0, timer_log_cb, "scheduled from HTTP route");
    if (timer_id < 0) {
        respond_text(res, 500, "Internal Server Error", "failed to schedule timer\n");
        return;
    }

    char body[128];
    int n = snprintf(body, sizeof(body), "timer scheduled: id=%d delay_ms=%d\n", timer_id, delay_ms);
    if (n < 0) {
        respond_text(res, 500, "Internal Server Error", "failed to render response\n");
        return;
    }

    http_response_init(res);
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/plain");
    http_response_write_body(res, body, (size_t)n);
    http_response_end(res);
}

#ifdef HTTP_ENABLE_MONITORING
static void metrics_handler(http_request_t *req, http_response_t *res, void *user_data)
{
    (void)req;
    (void)user_data;

    http_metrics_t metrics;
    if (http_get_metrics(g_ctx, &metrics) != 0) {
        respond_text(res, 500, "Internal Server Error", "failed to read metrics\n");
        return;
    }

    char body[256];
    int n = snprintf(body, sizeof(body),
                     "requests=%llu\nresponses=%llu\nconnections=%llu\nerrors=%llu\navg_ms=%.2f\n",
                     (unsigned long long)metrics.total_requests,
                     (unsigned long long)metrics.total_responses,
                     (unsigned long long)metrics.active_connections,
                     (unsigned long long)metrics.total_errors,
                     metrics.average_response_time_ms);
    if (n < 0) {
        respond_text(res, 500, "Internal Server Error", "failed to render metrics\n");
        return;
    }

    http_response_init(res);
    http_response_set_status(res, 200, "OK");
    http_response_add_header(res, "Content-Type", "text/plain");
    http_response_write_body(res, body, (size_t)n);
    http_response_end(res);
}
#endif

struct route_spec {
    const char *method;
    const char *path;
    http_handler_fn handler;
};

static int register_routes(http_ctx_t *ctx)
{
    static const struct route_spec routes[] = {
        {"GET",  "/health",    health_handler},
        {"GET",  "/echo",      echo_handler},
        {"POST", "/echo",      echo_handler},
        {"GET",  "/headers",   headers_handler},
        {"GET",  "/set_timer", timer_handler},
#ifdef HTTP_ENABLE_MONITORING
        {"GET",  "/metrics",   metrics_handler},
#endif
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        if (http_register_route(ctx, routes[i].method, routes[i].path, routes[i].handler, NULL) != 0) {
            fprintf(stderr, "failed to register route %s %s\n", routes[i].method, routes[i].path);
            return -1;
        }
    }

    return 0;
}

int main(void)
{
    signal(SIGINT, on_sigint);

    http_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.recv_buffer_size = 4096;
    cfg.send_buffer_size = 4096;
    cfg.max_connections = 32;
    cfg.keep_alive_timeout_ms = 10000;
    cfg.request_timeout_ms = 5000;
    cfg.enable_chunked = 1;
    cfg.log_fn = server_log_cb;

    g_ctx = http_init(&cfg);
    if (!g_ctx) {
        fprintf(stderr, "failed to initialize HTTP context\n");
        return 1;
    }

    if (register_routes(g_ctx) != 0) {
        http_free(g_ctx);
        return 1;
    }

    if (http_listen(g_ctx, "0.0.0.0:8080", NULL, NULL) != 0) {
        fprintf(stderr, "failed to listen on 0.0.0.0:8080\n");
        http_free(g_ctx);
        return 1;
    }

    fprintf(stderr, "listening on http://127.0.0.1:8080\n");
    fprintf(stderr, "routes: GET /health, GET|POST /echo, GET /headers, GET /set_timer\n");
#ifdef HTTP_ENABLE_MONITORING
    fprintf(stderr, "routes: GET /metrics\n");
#endif

    http_run(g_ctx);
    http_free(g_ctx);
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
- канонический пример регистрации маршрутов показан в секции выше и совпадает с `examples/test_server.c`

## 5. Что доступно в `http_request_t`

Структура запроса из [include/http/http_request.h](/home/di/projects_С/git_progect/libs_v0.10/include/http/http_request.h:1):

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

Практический смысл этого API в текущей реализации:

- `http_response_init` сбрасывает структуру ответа и подготавливает внутренний буфер body;
- `http_response_add_header` накапливает пользовательские заголовки;
- `http_response_write_body` дописывает body в буферизованном режиме;
- `http_response_end` завершает формирование объекта ответа, но не заменяет внутреннюю сборку итогового HTTP-сообщения библиотекой;
- итоговый wire-format response библиотека собирает сама при отправке:
  - status line;
  - user headers;
  - auto-generated `Content-Length`, если он не был передан вручную;
  - `Connection`;
  - body.

Что важно учитывать:

- если вручную задать `Content-Length`, библиотека не пересчитывает его поверх пользовательского значения;
- текущее поведение ориентировано на buffered responses, а не на настоящий streaming;
- `Transfer-Encoding: chunked` как полноценный streaming-контракт сейчас не следует считать доведённой возможностью.

## 7. Поведение demo-маршрутов

Текущее ожидаемое поведение канонического demo-сервера из `examples/test_server.c`:

- `GET /health`
  - статус: `200 OK`
  - body: `OK\n`

- `GET /echo?...`
  - статус: `200 OK`
  - body: исходная query string плюс перевод строки
  - пример: `/echo?msg=hello&x=1` возвращает `msg=hello&x=1\n`

- `POST /echo`
  - статус: `200 OK`
  - body: присланное тело запроса плюс перевод строки
  - если body пустое, возвращается `(empty)\n`

- `GET /headers`
  - статус: `200 OK`
  - body: список полученных request headers по одному на строку в формате `Name: Value\n`

- `GET /set_timer?delay=<ms>`
  - статус: `200 OK`, если `delay` распознан и больше нуля
  - body: строка вида `timer scheduled: id=... delay_ms=...\n`
  - статус: `400 Bad Request`, если `delay` отсутствует или невалиден
  - body ошибки: `usage: /set_timer?delay=1000\n`

- `GET /metrics`
  - существует только при `HTTP_ENABLE_MONITORING`
  - статус: `200 OK` при успешном получении снимка метрик

- неизвестный путь
  - статус: `404 Not Found`
  - body: `Not Found`

- известный путь с неверным методом
  - статус: `405 Method Not Allowed`
  - body: `Method Not Allowed`

- malformed request
  - статус: `400 Bad Request`
  - body: `Bad Request`

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
#include "http/http.h"

http_config_t cfg = {0};
cfg.tls_cert_path = "server.crt";
cfg.tls_key_path = "server.key";
cfg.tls_verify_peer = 0;

http_ctx_t *ctx = http_init(&cfg);
if (!ctx) {
    return 1;
}

/* используйте handler из канонического примера выше */
http_listen_https(ctx, "0.0.0.0:8443", health_handler, NULL);
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
#include "http/http.h"
#include <string.h>

http_config_t cfg;
memset(&cfg, 0, sizeof(cfg));
cfg.thread_count = 4;

http_ctx_t *ctx = http_init(&cfg);
if (!ctx) {
    return 1;
}

/* используйте handler из канонического примера выше */
http_run_multithreaded(ctx, "0.0.0.0:8080", health_handler, NULL);

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
#include "http/http.h"
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

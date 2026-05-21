# AGENTS.md

Этот файл фиксирует текущее состояние проекта. Если в проекте, особенно в `libs/http`, были внесены изменения в API, структуру файлов, сценарии сборки, примеры, ограничения или известные проблемы, нужно сразу актуализировать этот файл.

## Назначение проекта

Репозиторий сейчас выглядит как заготовка низкоуровневой HTTP-библиотеки на C с фокусом на `libs/http`. Внутри заявлены:

- HTTP-сервер на `poll`
- базовый HTTP-клиент
- маршрутизация
- таймеры
- логирование
- URL/JSON utility-функции
- метрики, TLS, multithreading и self-tests как опциональные возможности

Реальное состояние реализации уже, чем описание в публичном API: часть возможностей работает, часть только задекларирована или включается условной компиляцией, часть остаётся незавершённой.

## Текущая структура

- `libs/http/http.c` — основная реализация библиотеки
- `libs/http/http.h` — публичный API верхнего уровня
- `libs/http/http_request.h` — структура запроса, client API и URL/JSON utility-функции
- `libs/http/http_response.h` — API формирования ответа
- `libs/http/dynbuf.c`, `libs/http/dynbuf.h` — динамический буфер
- `libs/http/config.h` — конфигурация контекста
- `libs/http/metrics.h` — API метрик
- `libs/http/log.h` — enum уровней логирования
- `libs/http/error.h` — сейчас фактически пустой
- `libs/json/cJSON.c`, `libs/json/cJSON.h` — сторонняя JSON-библиотека
- `test_server.c` — демонстрационный HTTP-сервер с маршрутами
- `test_client.c` — демонстрационный клиент на `libcurl`
- `libs/http/lib_http` — отдельный текстовый файл с альтернативной/сводной версией HTTP-кода, это не собранная библиотека

## Что реально реализовано сейчас

- `dynbuf` собирается и реализует `init/free/reserve/append`
- `http.c` собирается в объект в дефолтной конфигурации
- работают базовые utility-функции:
  - `http_url_encode`
  - `http_url_decode`
  - `http_escape_json_string`
  - `http_mime_type_from_ext`
- реализованы базовые части серверной модели:
  - `http_init`
  - `http_free`
  - `http_listen`
  - `http_run`
  - `http_stop`
  - `http_poll`
  - `http_register_route`
  - `http_set_timer`
  - `http_cancel_timer`
- реализован буферизованный API ответа:
  - `http_response_init`
  - `http_response_set_status`
  - `http_response_add_header`
  - `http_response_write_body`
  - `http_response_end`
  - `http_response_free`
- `http_request_free` реализован

## Что выглядит неполным или условным

- TLS/HTTPS заявлен, но в `http.c` прямо отмечен как stub/TODO
- multithreading заявлен, но зависит от `HTTP_ENABLE_MULTITHREADING` и по текущему состоянию не выглядит основной рабочей веткой
- monitoring/metrics зависят от `HTTP_ENABLE_MONITORING`
- self-tests зависят от `HTTP_ENABLE_SELF_TESTS`
- async client задекларирован в кодовой базе, но не является завершённой рабочей частью публичного API
- `enable_http2` в конфиге пока только placeholder
- `enable_compression` в конфиге не выглядит доведённым до полноценной реализации

## Текущие технические проблемы

- Публичные заголовки местами рассинхронизированы с реализацией: API обещает больше, чем доступно без feature-флагов
- В заголовках есть неудачные зависимости:
  - `config.h` включает `http.h`
  - `http.h` включает `config.h`
  - это создаёт циклическую связность и усложняет сопровождение API
- `metrics.h` использует `uint64_t` и `http_ctx_t`, но сам не содержит необходимых базовых include/forward declaration
- `error.h` пустой и пока не несёт полезной контрактной нагрузки
- `libs/http/lib_http` по имени похож на бинарь или артефакт сборки, но фактически является текстовым C-файлом; это может путать

## Состояние сборки на момент анализа

Проверено локально:

- `gcc -Ilibs/http -c libs/http/http.c -o /tmp/http.o` — успешно
- `gcc -Ilibs/http -c libs/http/dynbuf.c -o /tmp/dynbuf.o` — успешно
- сборка примера сервера в дефолтной конфигурации не проходит:
  - `gcc -Ilibs/http -Ilibs/json test_server.c libs/http/http.c libs/http/dynbuf.c libs/json/cJSON.c -o /tmp/test_server`
  - причина: `undefined reference to 'http_get_metrics'`

Практический вывод:

- библиотечное ядро в текущем виде частично компилируемо
- демонстрационный сервер зависит от возможностей, которые не доступны в дефолтной сборке без нужных compile-time флагов
- у проекта сейчас нет оформленной системы сборки (`Makefile`, `CMakeLists.txt` и т.д.), поэтому правила сборки и feature-флаги не зафиксированы в одном месте

## Поведение примеров

`test_server.c` поднимает маршруты:

- `GET /health`
- `GET /echo`
- `POST /echo`
- `GET /headers`
- `GET /chunked`
- `GET /url_utils`
- `GET /json_utils`
- `GET /mime`
- `GET /metrics`
- `GET /set_timer`

`test_client.c` не тестирует библиотечный клиент, а использует внешний `libcurl` для обращения к локальному серверу. То есть это smoke/demo-клиент для сервера, а не пример использования собственного HTTP client API из `libs/http`.

## Что важно понимать агенту перед изменениями

- Основная рабочая зона проекта сейчас — `libs/http`
- Перед изменениями нужно проверять не только сигнатуры в заголовках, но и факт наличия реализации в `http.c`
- Если меняются feature-флаги, контракты API, список маршрутов примера, сценарии сборки или известные ограничения, этот `AGENTS.md` нужно обновить в том же изменении
- Если файл `libs/http/lib_http` не нужен как отдельная версия исходника, стоит отдельно решить его судьбу: переименовать, удалить или документировать более явно

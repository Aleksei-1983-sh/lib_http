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

Реальное состояние реализации уже, чем описание в публичном API: часть возможностей работает, часть только задекларирована или включается условной компиляцией, часть остаётся незавершённой. Публичные заголовки частично приведены в соответствие с реализацией, но библиотека всё ещё находится в стадии активной стабилизации контракта.

## Текущая структура

- `libs/http/http.c` — core-модуль: lifecycle, listen/run/poll и event loop
- `libs/http/http_parser.c` — разбор HTTP-запроса
- `libs/http/http_response.c` — буферизация и отправка HTTP-ответа
- `libs/http/http_support.c` — маршруты, таймеры, utility-функции, MIME, metrics/self-tests
- `libs/http/http_internal.h` — внутренние структуры и shared helper-прототипы для модулей `libs/http`
- `include/http/http.h` — публичный API верхнего уровня
- `include/http/http_request.h` — структура запроса и URL/JSON utility-функции
- `include/http/http_response.h` — API формирования ответа
- `libs/http/dynbuf.c`, `libs/http/dynbuf.h` — динамический буфер
- `include/http/config.h` — конфигурация контекста
- `include/http/metrics.h` — API метрик
- `include/http/log.h` — enum уровней логирования
- `include/http/error.h` — сейчас фактически пустой
- `libs/json/cJSON.c`, `libs/json/cJSON.h` — сторонняя JSON-библиотека
- `API_IMPLEMENTATION_STATUS.md` — короткая таблица соответствия “задекларировано в API / реально реализовано в `libs/http/http*.c`”
- `Makefile` — базовая сборка библиотеки, примеров и тестов разных типов
- `tests/smoke/smoke_http.c` — smoke-tests для дефолтной и feature-flag сборки
- `tests/unit/http_unit.c` — unit-тесты utility и response API
- `tests/integration/http_server_integration.sh` — integration-тесты через реальный запуск demo-сервера и `curl`
- `examples/test_server.c` — демонстрационный HTTP-сервер с маршрутами
- `examples/test_client.c` — демонстрационный клиент на `libcurl`
- `libs/http/lib_http` — отдельный текстовый файл с альтернативной/сводной версией HTTP-кода, это не собранная библиотека

## Что реально реализовано сейчас

- `dynbuf` собирается и реализует `init/free/reserve/append`
- HTTP-ядро теперь собирается как набор модулей: `http.o`, `http_parser.o`, `http_response.o`, `http_support.o`
- есть минимальная система сборки через `Makefile`
- работают базовые utility-функции:
  - `http_url_encode`
  - `http_url_decode`
  - `http_escape_json_string`
  - `http_mime_type_from_ext`
- есть smoke-tests без внешних зависимостей:
  - дефолтная сборка
  - сборка с `HTTP_ENABLE_MONITORING`
  - сборка с `HTTP_ENABLE_SELF_TESTS`
  - сборка с `HTTP_ENABLE_MULTITHREADING`
  - сборка с `HTTP_ENABLE_TLS`
- есть unit-тесты для utility-функций и `http_response_*`
- есть integration-тесты, которые поднимают `examples/test_server.c` и проверяют маршруты `/health`, `/echo`, `/headers`, `/set_timer`
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
- публичный контракт по метрикам теперь корректно зависит от `HTTP_ENABLE_MONITORING`
- из публичного header убран незадействованный prototype `http_client_request_blocking`, которого нет в реализации

## Что выглядит неполным или условным

- TLS/HTTPS заявлен, но текущая реализация остаётся stub/TODO в core-модуле
- multithreading заявлен, но зависит от `HTTP_ENABLE_MULTITHREADING` и по текущему состоянию не выглядит основной рабочей веткой
- monitoring/metrics зависят от `HTTP_ENABLE_MONITORING`
- self-tests зависят от `HTTP_ENABLE_SELF_TESTS`
- async client не является частью рабочего публичного API
- `enable_http2` в конфиге пока только placeholder
- `enable_compression` в конфиге не выглядит доведённым до полноценной реализации

## Текущие технические проблемы

- Публичные заголовки в `include/http` местами рассинхронизированы с реализацией: API обещает больше, чем доступно без feature-флагов
- В заголовках ещё остаются поля и флаги-плейсхолдеры, но комментарии уже жёстче ограничены текущим контрактом
- Критичная циклическая зависимость между `config.h` и `http.h` убрана, `metrics.h` теперь самодостаточен по базовым typedef/include
- `error.h` пустой и пока не несёт полезной контрактной нагрузки
- `libs/http/lib_http` по имени похож на бинарь или артефакт сборки, но фактически является текстовым C-файлом; это может путать
- `examples/test_client.c` по-прежнему зависит от внешнего `libcurl` и не входит в дефолтную сборку `make`
- integration-тесты зависят от локально доступного `curl`

## Состояние сборки на момент анализа

Проверено локально:

- `make test-smoke` — успешно
- `make test-unit` — успешно
- `make test-integration` — успешно
- `make test` / `make check` — успешно, запускают smoke, unit и integration наборы
- `make help` — выводит project-specific подсказку по основным целям сборки и тестов
- `make examples` — успешно, собирает demo-сервер
- после выравнивания `examples/test_server.c` примерный сервер не должен требовать `http_get_metrics`, если `HTTP_ENABLE_MONITORING` не включён

Практический вывод:

- библиотечное ядро в текущем виде частично компилируемо
- базовая сборка теперь зафиксирована через `Makefile`
- тесты теперь разведены по типам: `tests/smoke`, `tests/unit`, `tests/integration`
- основной агрегирующий запуск — `make test` или `make check`
- для project-specific справки по сценариям сборки и тестов используется `make help`
- демонстрационный сервер теперь частично согласован с дефолтной сборкой: маршрут и обработчик `/metrics` активны только при `HTTP_ENABLE_MONITORING`
- у проекта теперь есть минимально оформленная система сборки, но пока без `CMakeLists.txt` и без отдельного install/package слоя

## Поведение примеров

`examples/test_server.c` теперь выступает как канонический `server-first` пример и поднимает маршруты:

- `GET /health`
- `GET /echo`
- `POST /echo`
- `GET /headers`
- `GET /set_timer`

Маршрут `GET /metrics` существует только при сборке с `HTTP_ENABLE_MONITORING`.

`examples/test_server.c` больше не пытается демонстрировать все utility-функции и условные возможности сразу; файл сфокусирован на базовом сценарии: `http_init` -> `http_register_route` -> `http_listen` -> `http_run`.

`examples/test_server.c` теперь требует параметры командной строки `<host> <port>`. Если запустить сервер без них, он завершится с кратким helper/usage-сообщением и примером запуска `127.0.0.1 9091`.

Канонический пример в `README.md` синхронизирован с `examples/test_server.c` и должен обновляться вместе с ним.

`examples/test_client.c` не тестирует библиотечный клиент, а использует внешний `libcurl` для обращения к локальному серверу. То есть это вспомогательный demo-клиент для сервера, а не пример использования собственного HTTP client API из `libs/http`.

## Что важно понимать агенту перед изменениями

- Основная рабочая зона проекта сейчас — `libs/http`
- Перед изменениями нужно проверять не только сигнатуры в заголовках, но и факт наличия реализации в соответствующем модуле `libs/http/http*.c`
- Если меняются feature-флаги, контракты API, список маршрутов примера, сценарии сборки или известные ограничения, этот `AGENTS.md` нужно обновить в том же изменении
- Если файл `libs/http/lib_http` не нужен как отдельная версия исходника, стоит отдельно решить его судьбу: переименовать, удалить или документировать более явно

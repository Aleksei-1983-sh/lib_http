# API Implementation Status

Короткая сверка публичного контракта `libs/http/*.h` с фактической реализацией в `libs/http/http.c`.

| Группа | API | Статус | Примечание |
| --- | --- | --- | --- |
| Server core | `http_init` `http_free` `http_listen` `http_run` `http_stop` `http_poll` | `OK` | Базовая реализация есть |
| HTTPS / TLS | `http_listen_https` | `STUB` | Только при `HTTP_ENABLE_TLS`; возвращает ошибку |
| Multithreading | `http_run_multithreaded` `http_stop_multithreaded` | `STUB` | Только при `HTTP_ENABLE_MULTITHREADING`; stop делегирует в `http_stop` |
| Routing | `http_register_route` | `OK` | Базовая реализация есть |
| Timers | `http_set_timer` `http_cancel_timer` | `OK` | Базовая реализация есть |
| Response API | `http_response_init` `http_response_set_status` `http_response_add_header` `http_response_write_body` `http_response_end` `http_response_free` | `PARTIAL` | Буферизация работает; `end` в основном завершает/логирует состояние |
| Request API | `http_request_free` | `OK` | Реализация есть |
| URL / JSON utils | `http_url_encode` `http_url_decode` `http_escape_json_string` | `OK` | Реализация есть |
| MIME | `http_mime_type_from_ext` | `OK` | Реализация есть |
| Metrics | `http_get_metrics` `http_reset_metrics` `http_set_metrics_callback` | `FLAGGED` | Только при `HTTP_ENABLE_MONITORING`; таймер callback без полного lifecycle |
| Self-tests | `http_run_self_tests` `http_register_test` | `PARTIAL` | Только при `HTTP_ENABLE_SELF_TESTS`; тесты минимальные, register stub |
| Async client | не экспортируется | `N/A` | В `http.c` только закомментированная заготовка |
| Request builder | закомментирован в `http_request.h` | `N/A` | Не активный публичный контракт, реализации нет |

Статусы читать так:

- `OK` — рабочая базовая реализация есть.
- `PARTIAL` — часть контракта работает, но поведение упрощено или неполно.
- `STUB` — символ есть, но это заглушка.
- `FLAGGED` — доступно только при соответствующем `#define`.
- `N/A` — не является активным публичным API.

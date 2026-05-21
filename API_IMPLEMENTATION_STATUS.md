# API Implementation Status

Короткая сверка публичного контракта `libs/http/*.h` с фактической реализацией в `libs/http/http.c`.

```text
+-----------------+-----------+----------------------------------------------+
| Group           | Status    | Notes                                        |
+-----------------+-----------+----------------------------------------------+
| Server core     | OK        | init/free/listen/run/stop/poll               |
| HTTPS / TLS     | STUB      | listen_https; only with HTTP_ENABLE_TLS      |
| Multithreading  | STUB      | MT API behind flag; run is stub              |
| Routing         | OK        | register_route                               |
| Timers          | OK        | set_timer/cancel_timer                       |
| Response API    | PARTIAL   | buffered API works; end mostly final/log     |
| Request API     | OK        | request_free                                 |
| URL / JSON      | OK        | encode/decode/escape                         |
| MIME            | OK        | mime_type_from_ext                           |
| Metrics         | FLAGGED   | only with HTTP_ENABLE_MONITORING             |
| Self-tests      | PARTIAL   | only with HTTP_ENABLE_SELF_TESTS             |
| Async client    | N/A       | not exported; only commented draft in c      |
| Request builder | N/A       | commented in header; no implementation       |
+-----------------+-----------+----------------------------------------------+
```

Расшифровка API по строкам:

- `Server core`:
  `http_init` `http_free` `http_listen` `http_run` `http_stop` `http_poll`
- `HTTPS / TLS`:
  `http_listen_https`
- `Multithreading`:
  `http_run_multithreaded` `http_stop_multithreaded`
- `Routing`:
  `http_register_route`
- `Timers`:
  `http_set_timer` `http_cancel_timer`
- `Response API`:
  `http_response_init` `http_response_set_status` `http_response_add_header`
  `http_response_write_body` `http_response_end` `http_response_free`
- `Request API`:
  `http_request_free`
- `URL / JSON`:
  `http_url_encode` `http_url_decode` `http_escape_json_string`
- `MIME`:
  `http_mime_type_from_ext`
- `Metrics`:
  `http_get_metrics` `http_reset_metrics` `http_set_metrics_callback`
- `Self-tests`:
  `http_run_self_tests` `http_register_test`

Статусы читать так:

- `OK` — рабочая базовая реализация есть.
- `PARTIAL` — часть контракта работает, но поведение упрощено или неполно.
- `STUB` — символ есть, но это заглушка.
- `FLAGGED` — доступно только при соответствующем `#define`.
- `N/A` — не является активным публичным API.

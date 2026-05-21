# Integration Tests

Этот каталог содержит integration-тесты, которые проверяют библиотеку через реальный запуск demo-сервера и HTTP-запросы к нему.

## Файл `http_server_integration.sh`

Основной integration-раннер:

- [http_server_integration.sh](/home/di/projects_С/git_progect/libs_v0.10/tests/integration/http_server_integration.sh:1)

## Что делает тест по шагам

Сценарий `http_server_integration.sh` работает так:

1. Вычисляет корень проекта и путь к бинарнику сервера:
   - `build/bin/test_server`

2. Выбирает локальный адрес для теста:
   - host: `127.0.0.1`
   - port: `19091`

3. Запускает demo-сервер в фоне:
   - `./build/bin/test_server 127.0.0.1 19091`
   - stdout/stderr сервера складываются в:
     - `build/test_server.integration.log`

4. Ждёт готовности сервера:
   - несколько раз пробует `GET /health` через `curl`
   - если сервер не начал отвечать вовремя, тест падает

5. Выполняет позитивные HTTP-проверки:
   - `GET /health`
   - `GET /echo?...`
   - `POST /echo`
   - пустой `POST /echo`
   - `GET /headers`
   - `GET /set_timer?delay=10`

6. Выполняет дополнительные поведенческие проверки:
   - keep-alive roundtrip
   - повторное использование соединения через `curl --http1.1`

7. Выполняет негативные проверки:
   - неизвестный путь -> `404 Not Found`
   - неверный метод на известном пути -> `405 Method Not Allowed`
   - `GET /set_timer` без `delay` -> `400 Bad Request`
   - `GET /set_timer?delay=0` -> `400 Bad Request`
   - malformed request через `nc` -> `400 Bad Request`

8. По завершении останавливает сервер:
   - процесс завершается через `trap cleanup EXIT`

## Что именно проверяется

Текущий integration-тест закрепляет такой внешний контракт demo-сервера:

- сервер реально поднимается и слушает TCP-порт;
- маршруты отвечают ожидаемыми статусами и телами;
- keep-alive не ломает базовый сценарий повторных запросов;
- error-path поведение для `400`, `404`, `405` стабильно;
- malformed request не приводит к молчаливому зависанию теста.

## Зависимости

Для запуска нужны локально доступные утилиты:

- `curl`
- `nc`
- `timeout`
- `bash`

## Как запускать

Из корня проекта:

```bash
make test-integration
```

Либо напрямую:

```bash
tests/integration/http_server_integration.sh
```

Обычно предпочтительнее запуск через `make test-integration`, потому что эта цель сначала гарантирует сборку `build/bin/test_server`.

## Где смотреть логи

Если integration-тест упал, первым делом полезно смотреть:

- `build/test_server.integration.log`

Там будут сообщения самого demo-сервера, включая startup и внутренние ошибки.

#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SERVER_BIN="$ROOT_DIR/build/bin/test_server"
HOST="127.0.0.1"
PORT="19091"
BASE_URL="http://$HOST:$PORT"
SERVER_LOG="$ROOT_DIR/build/test_server.integration.log"

cleanup() {
    if [[ -n "${SERVER_PID:-}" ]]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        wait "$SERVER_PID" >/dev/null 2>&1 || true
    fi
}

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

expect_status_and_body() {
    local expected_status="$1"
    local url="$2"
    local expected_substr="$3"
    local response
    local body
    local status

    response="$(curl -sS -o - -w $'\n%{http_code}' "$url")"
    status="${response##*$'\n'}"
    body="${response%$'\n'*}"

    [[ "$status" == "$expected_status" ]] || fail "$url returned status $status, expected $expected_status"
    grep -q "$expected_substr" <<<"$body" || fail "$url body did not contain '$expected_substr'"
}

expect_keep_alive_roundtrip() {
    local output

    output="$(curl -sS -i --http1.1 \
        -H "Connection: keep-alive" \
        "$BASE_URL/health" \
        "$BASE_URL/echo?keep=alive")"

    grep -q "Connection: keep-alive" <<<"$output" || fail "keep-alive response header was not returned"
    grep -q "OK" <<<"$output" || fail "keep-alive probe did not include /health response"
    grep -q "keep=alive" <<<"$output" || fail "keep-alive probe did not include second response body"
}

expect_custom_method_405() {
    local response
    local body
    local status

    response="$(curl -sS -X FROB -o - -w $'\n%{http_code}' "$BASE_URL/health")"
    status="${response##*$'\n'}"
    body="${response%$'\n'*}"

    [[ "$status" == "405" ]] || fail "custom method on /health returned status $status, expected 405"
    grep -q "Method Not Allowed" <<<"$body" || \
        fail "custom method on /health did not return Method Not Allowed body"
}

expect_malformed_request_400() {
    local output

    output="$(
        {
            printf 'BROKEN REQUEST\r\n\r\n'
            sleep 0.1
        } | timeout 2 nc "$HOST" "$PORT" || true
    )"

    grep -q "HTTP/1.1 400 Bad Request" <<<"$output" || \
        fail "malformed request did not return 400 Bad Request"
    grep -q "Bad Request" <<<"$output" || \
        fail "malformed request did not return Bad Request body"
}

trap cleanup EXIT

[[ -x "$SERVER_BIN" ]] || fail "server binary not found: $SERVER_BIN"

"$SERVER_BIN" "$HOST" "$PORT" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!

ready=0
for _ in $(seq 1 50); do
    if curl -fsS "$BASE_URL/health" >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 0.1
done

[[ "$ready" -eq 1 ]] || fail "server did not become ready"

health="$(curl -fsS "$BASE_URL/health")"
[[ "$health" == $'OK' || "$health" == $'OK\n' ]] || fail "/health returned unexpected body: $health"

echo_get="$(curl -fsS "$BASE_URL/echo?msg=hello&x=1")"
[[ "$echo_get" == $'msg=hello&x=1' || "$echo_get" == $'msg=hello&x=1\n' ]] || \
    fail "/echo GET returned unexpected body: $echo_get"

echo_post="$(curl -fsS -X POST "$BASE_URL/echo" -d "post body")"
[[ "$echo_post" == $'post body' || "$echo_post" == $'post body\n' ]] || \
    fail "/echo POST returned unexpected body: $echo_post"

empty_post="$(curl -fsS -X POST "$BASE_URL/echo")"
[[ "$empty_post" == $'(empty)' || "$empty_post" == $'(empty)\n' ]] || \
    fail "/echo empty POST returned unexpected body: $empty_post"

headers_body="$(curl -fsS -H "X-Test: 123" "$BASE_URL/headers")"
grep -q "X-Test: 123" <<<"$headers_body" || fail "/headers did not include custom header"

timer_body="$(curl -fsS "$BASE_URL/set_timer?delay=10")"
grep -q "timer scheduled:" <<<"$timer_body" || fail "/set_timer did not confirm scheduling"

expect_keep_alive_roundtrip
expect_custom_method_405
expect_malformed_request_400

expect_status_and_body "404" "$BASE_URL/does-not-exist" "Not Found"
expect_status_and_body "400" "$BASE_URL/set_timer" "usage: /set_timer?delay=1000"
expect_status_and_body "400" "$BASE_URL/set_timer?delay=0" "usage: /set_timer?delay=1000"

echo "integration tests passed"

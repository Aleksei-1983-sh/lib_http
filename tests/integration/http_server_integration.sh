#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SERVER_BIN="$ROOT_DIR/build/bin/test_server"
HOST="127.0.0.1"
EXTERNAL_PORT="19091"
INTERNAL_PORT="19092"
EXTERNAL_URL="http://$HOST:$EXTERNAL_PORT"
INTERNAL_URL="http://$HOST:$INTERNAL_PORT"
SERVER_LOG="$ROOT_DIR/build/test_server.integration.log"

cleanup() {
    if [[ -n "${SERVER_PID:-}" ]]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        wait "$SERVER_PID" >/dev/null 2>&1 || true
    fi
}
fail() { echo "FAIL: $1" >&2; exit 1; }
trap cleanup EXIT

[[ -x "$SERVER_BIN" ]] || fail "server binary not found: $SERVER_BIN"
"$SERVER_BIN" "$HOST" "$EXTERNAL_PORT" "$INTERNAL_PORT" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!

for _ in $(seq 1 80); do
    if curl -fsS "$EXTERNAL_URL/health" >/dev/null 2>&1 && curl -fsS "$INTERNAL_URL/health" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

curl -fsS "$EXTERNAL_URL/health" >/dev/null || fail "external not ready"
curl -fsS "$INTERNAL_URL/health" >/dev/null || fail "internal not ready"

external_role="$(curl -fsS "$EXTERNAL_URL/role")"
internal_role="$(curl -fsS "$INTERNAL_URL/role")"
[[ "$external_role" == $'role=external' || "$external_role" == $'role=external
' ]] || fail "unexpected external role: $external_role"
[[ "$internal_role" == $'role=internal' || "$internal_role" == $'role=internal
' ]] || fail "unexpected internal role: $internal_role"

echo_get="$(curl -fsS "$EXTERNAL_URL/echo?msg=hello&x=1")"
[[ "$echo_get" == $'msg=hello&x=1' || "$echo_get" == $'msg=hello&x=1
' ]] || fail "external echo GET mismatch"

echo_post="$(curl -fsS -X POST "$INTERNAL_URL/echo" -d "inside")"
[[ "$echo_post" == $'inside' || "$echo_post" == $'inside
' ]] || fail "internal echo POST mismatch"

curl -fsS "$EXTERNAL_URL/set_timer?delay=10" | grep -q "timer scheduled:" || fail "external timer not scheduled"
curl -fsS "$INTERNAL_URL/set_timer?delay=10" | grep -q "timer scheduled:" || fail "internal timer not scheduled"

status_404="$(curl -sS -o /dev/null -w '%{http_code}' "$EXTERNAL_URL/nope")"
[[ "$status_404" == "404" ]] || fail "external 404 mismatch: $status_404"

status_400="$(curl -sS -o /dev/null -w '%{http_code}' "$INTERNAL_URL/set_timer")"
[[ "$status_400" == "400" ]] || fail "internal 400 mismatch: $status_400"

echo "integration tests passed"

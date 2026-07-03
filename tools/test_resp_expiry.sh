#!/bin/sh
# RESP expiry semantics: expired keys are absent for direct key command paths.
# DBSIZE stays O(1) and may include logically expired keys until a sweep/touch.

set -eu

if ! command -v redis-cli >/dev/null 2>&1; then
  echo "  (redis-cli absent — RESP expiry check skipped)"
  echo "==> RESP EXPIRY SKIPPED"
  exit 0
fi

PORT=${PORT:-17622}
LOG=$(mktemp -t alcove-resp-expiry.XXXXXX.log)

./alcove --noload --no-init -r "$PORT" --threads 4 >"$LOG" 2>&1 &
SRV=$!

cleanup() {
  kill -INT "$SRV" 2>/dev/null || true
  wait "$SRV" 2>/dev/null || true
  rm -f "$LOG"
}
trap cleanup EXIT INT TERM

i=0
while [ "$i" -lt 100 ]; do
  if redis-cli -p "$PORT" PING >/dev/null 2>&1; then
    break
  fi
  i=$((i + 1))
  sleep 0.05
done

run() {
  redis-cli -p "$PORT" --raw "$@" | tr -d '\r'
}

fail() {
  echo "RESP expiry FAILED: $1"
  echo "server log:"
  sed 's/^/  /' "$LOG" | tail -80
  exit 1
}

check() {
  name=$1
  want=$2
  shift 2
  got=$(run "$@")
  [ "$got" = "$want" ] || fail "$name: got '$got', want '$want'"
}

check "ping" "PONG" PING
check "flushdb" "OK" FLUSHDB

check "set ttl" "OK" SET k v PX 100
check "get before expiry" "v" GET k
sleep 0.2
check "get expired" "" GET k
check "ttl expired" "-2" TTL k
check "pttl expired" "-2" PTTL k
check "exists expired" "0" EXISTS k
check "del expired" "0" DEL k

check "set nx seed" "OK" SET nx v PX 100
sleep 0.2
check "set nx after expiry" "OK" SET nx fresh NX
check "get nx replacement" "fresh" GET nx

check "set xx seed" "OK" SET xx v PX 100
sleep 0.2
check "set xx after expiry is nil" "" SET xx fresh XX

check "persist seed" "OK" SET p v PX 100
sleep 0.2
check "persist expired" "0" PERSIST p

check "expire seed" "OK" SET e v PX 100
sleep 0.2
check "expire expired" "0" EXPIRE e 10

check "persist live seed" "OK" SET live v PX 1000
check "persist live" "1" PERSIST live
check "ttl after persist" "-1" TTL live

echo "RESP EXPIRY: OK"

#!/bin/bash
# Tests for runtime/net/http.c (http:// and https://) and runtime/net/nextendo.c (sign-in) against local mocks (tools/net_mock.py) and a throwaway CA made
# in build/net_test/. NET_TEST_LIVE=1 adds the cases that reach nextendo.network and 51.178.29.194.
# Usage: tools/test_net.sh
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT=build/net_test
mkdir -p "$OUT/obj"
EXE=""; [ -n "$WINDIR" ] && EXE=".exe"

# throwaway CA, and a leaf for localhost / 127.0.0.1
if [ ! -f "$OUT/leaf.pem" ]; then
    openssl req -x509 -newkey rsa:2048 -nodes -days 30 -subj "/CN=net-test-ca" -keyout "$OUT/ca.key" -out "$OUT/ca.pem" 2>/dev/null
    openssl req -newkey rsa:2048 -nodes -subj "/CN=localhost" -keyout "$OUT/leaf.key" -out "$OUT/leaf.csr" 2>/dev/null
    printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=CA:FALSE\n' > "$OUT/leaf.ext"
    openssl x509 -req -in "$OUT/leaf.csr" -CA "$OUT/ca.pem" -CAkey "$OUT/ca.key" -CAcreateserial -days 30 \
        -extfile "$OUT/leaf.ext" -out "$OUT/leaf.pem" 2>/dev/null
fi

# a second leaf, signed by the same CA, that names other.test only (the host-name check must refuse it for localhost)
if [ ! -f "$OUT/bad.pem" ]; then
    openssl req -newkey rsa:2048 -nodes -subj "/CN=other.test" -keyout "$OUT/bad.key" -out "$OUT/bad.csr" 2>/dev/null
    printf 'subjectAltName=DNS:other.test\nbasicConstraints=CA:FALSE\n' > "$OUT/bad.ext"
    openssl x509 -req -in "$OUT/bad.csr" -CA "$OUT/ca.pem" -CAkey "$OUT/ca.key" -CAcreateserial -days 30 \
        -extfile "$OUT/bad.ext" -out "$OUT/bad.pem" 2>/dev/null
fi

# the test binary: http.c + json.c + roots.c + netcfg.c + nextendo.c + secure_store.c and the Mbed TLS sources, built as the Makefile does
MBEDTLS=third_party/mbedtls
FLAGS="-std=c11 -O1 -g -D_GNU_SOURCE -Wall -I$MBEDTLS/include -Iruntime/net -Iruntime -DMBEDTLS_USER_CONFIG_FILE=\"mbedtls_user_config.h\""
pids=()
for f in $MBEDTLS/library/*.c runtime/net/http.c runtime/net/json.c runtime/net/roots.c runtime/net/netcfg.c runtime/net/nextendo.c runtime/net/secure_store.c tools/test_net.c; do
    o="$OUT/obj/$(basename "$f" .c).o"
    # (the Mbed TLS objects are kept between runs; ours are cheap)
    if [ ! -f "$o" ] || [ "${f#$MBEDTLS}" = "$f" ] || [ runtime/net/mbedtls_user_config.h -nt "$o" ]; then
        clang $FLAGS -c "$f" -o "$o" &
        pids+=($!)
    fi
done
for p in "${pids[@]}"; do wait "$p"; done
LIBS=-lpthread
if [ "$(uname -s)" = Darwin ]; then  # (the Keychain backend is linked too; the tests select the file with RECOMP_SECURE_STORE=file)
    clang $FLAGS -DGAME_BUNDLE_ID='"test.nextendo"' -fobjc-arc -c runtime/net/secure_store_apple.m -o "$OUT/obj/secure_store_apple.o"
    LIBS="$LIBS -framework Foundation -framework Security"
fi
clang "$OUT"/obj/*.o $LIBS -o "$OUT/test_net$EXE"

free_port() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
export NET_TEST_TLS_PORT=$(free_port) NET_TEST_HTTP_PORT=$(free_port) NET_TEST_BADHOST_PORT=$(free_port) NET_TEST_OAUTH_PORT=$(free_port) NET_TEST_OFFLINE_PORT=$(free_port) NET_TEST_SLOW_PORT=$(free_port) NET_TEST_CA="$OUT/ca.pem"
python3 tools/net_mock.py --port "$NET_TEST_TLS_PORT" --tls "$OUT/leaf.pem" "$OUT/leaf.key" > "$OUT/mock_tls.log" 2>&1 &
M1=$!
python3 tools/net_mock.py --port "$NET_TEST_HTTP_PORT" > "$OUT/mock_http.log" 2>&1 &
M2=$!
python3 tools/net_mock.py --port "$NET_TEST_BADHOST_PORT" --tls "$OUT/bad.pem" "$OUT/bad.key" > "$OUT/mock_bad.log" 2>&1 &
M3=$!
python3 tools/net_mock.py --port "$NET_TEST_OAUTH_PORT" > "$OUT/mock_oauth.log" 2>&1 &
M4=$!
python3 tools/net_mock.py --port "$NET_TEST_OFFLINE_PORT" --offline > "$OUT/mock_offline.log" 2>&1 &
M5=$!
python3 tools/net_mock.py --port "$NET_TEST_SLOW_PORT" --slow-ms 1500 > "$OUT/mock_slow.log" 2>&1 &
M6=$!
trap 'kill $M1 $M2 $M3 $M4 $M5 $M6 2>/dev/null' EXIT
for i in $(seq 50); do
    grep -q ready "$OUT/mock_tls.log" 2>/dev/null && grep -q ready "$OUT/mock_http.log" 2>/dev/null && grep -q ready "$OUT/mock_bad.log" 2>/dev/null && grep -q ready "$OUT/mock_oauth.log" 2>/dev/null && grep -q ready "$OUT/mock_offline.log" 2>/dev/null && grep -q ready "$OUT/mock_slow.log" 2>/dev/null && break
    sleep 0.1
done

CASES="embedded_roots_valid https_local_ok https_untrusted https_wrong_host http_plain_still_works"
CASES="$CASES pkce_rfc7636 signin_loopback_full refresh_rotates_and_persists callback_state_mismatch callback_error_denied begin_twice_cancels_first prepare_offline_times_out no_secret_in_logs signout_during_refresh api_override_rejects_lookalikes"
[ -n "$NET_TEST_LIVE" ] && CASES="$CASES https_live_nextendo https_live_nextendo_embedded_only https_live_selfsigned_refused"
if "$OUT/test_net$EXE" $CASES; then echo "ALL PASS"; else echo "FAILURES"; exit 1; fi

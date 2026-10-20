#!/bin/bash
# Tests for runtime/net/http.c (http:// and https://) against local mocks (tools/net_mock.py) and a throwaway CA made
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

# the test binary: http.c + json.c + roots.c + netcfg.c and the Mbed TLS sources, built as the Makefile does
MBEDTLS=third_party/mbedtls
FLAGS="-std=c11 -O1 -g -D_GNU_SOURCE -Wall -I$MBEDTLS/include -Iruntime/net -Iruntime -DMBEDTLS_USER_CONFIG_FILE=\"mbedtls_user_config.h\""
pids=()
for f in $MBEDTLS/library/*.c runtime/net/http.c runtime/net/json.c runtime/net/roots.c runtime/net/netcfg.c tools/test_net.c; do
    o="$OUT/obj/$(basename "$f" .c).o"
    # (the Mbed TLS objects are kept between runs; ours are cheap)
    if [ ! -f "$o" ] || [ "${f#$MBEDTLS}" = "$f" ] || [ runtime/net/mbedtls_user_config.h -nt "$o" ]; then
        clang $FLAGS -c "$f" -o "$o" &
        pids+=($!)
    fi
done
for p in "${pids[@]}"; do wait "$p"; done
clang "$OUT"/obj/*.o -lpthread -o "$OUT/test_net$EXE"

free_port() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
export NET_TEST_TLS_PORT=$(free_port) NET_TEST_HTTP_PORT=$(free_port) NET_TEST_CA="$OUT/ca.pem"
python3 tools/net_mock.py --port "$NET_TEST_TLS_PORT" --tls "$OUT/leaf.pem" "$OUT/leaf.key" > "$OUT/mock_tls.log" 2>&1 &
M1=$!
python3 tools/net_mock.py --port "$NET_TEST_HTTP_PORT" > "$OUT/mock_http.log" 2>&1 &
M2=$!
trap 'kill $M1 $M2 2>/dev/null' EXIT
for i in $(seq 50); do
    grep -q ready "$OUT/mock_tls.log" 2>/dev/null && grep -q ready "$OUT/mock_http.log" 2>/dev/null && break
    sleep 0.1
done

CASES="https_local_ok https_untrusted https_wrong_host http_plain_still_works"
[ -n "$NET_TEST_LIVE" ] && CASES="$CASES https_live_nextendo https_live_selfsigned_refused"
if "$OUT/test_net$EXE" $CASES; then echo "ALL PASS"; else echo "FAILURES"; exit 1; fi

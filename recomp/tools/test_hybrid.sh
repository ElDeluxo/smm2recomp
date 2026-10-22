#!/bin/bash
# Tests for games/smm2/mods/hybrid.c (tools/test_hybrid.c): which parts of Course World the SMM2 mod answers for
# each online backend. Built with the address and undefined-behaviour sanitizers.
# Usage: tools/test_hybrid.sh [case...]
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p build/hybrid_test
clang -std=c11 -Wall -Werror -O1 -g -pthread -fsanitize=address,undefined games/smm2/mods/hybrid.c tools/test_hybrid.c -o build/hybrid_test/test_hybrid
build/hybrid_test/test_hybrid "$@"

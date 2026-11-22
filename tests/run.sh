#!/usr/bin/env bash
# tests/run.sh — host tests for everything that does not need ESP-IDF.
#
#   CC=cc tests/run.sh            # build and run all three C suites
#   tests/run.sh --asan           # same, with AddressSanitizer
#
# The C suites cover frame_pure (semver/JSON/base64url/SHA-256/zip), the store
# logic (canonical JSON, manifest, slot table) and the crypto (SHA-512,
# Ed25519 verify). The Python suite (tests/test_tools.py) covers the packaging
# tools and the end-to-end package round trip through the device's own C
# verifier (tools/verify_pkg.c).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PURE="$ROOT/components/frame_pure"
OUT="${TMPDIR:-/tmp}/frame_host_tests"
CC="${CC:-cc}"
FLAGS="-std=c11 -Wall -Wextra -Werror -O1 -I$PURE/include"
case "${1:-}" in
  --asan) FLAGS="$FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer" ;;
esac
mkdir -p "$OUT"

# Compile frame_pure once; link the same objects into every suite.
OBJS=()
for src in semver json base64url sha256 canonical manifest slot_table sha512 ed25519 zip; do
  obj="$OUT/$src.o"
  "$CC" $FLAGS -c "$PURE/$src.c" -o "$obj"
  OBJS+=("$obj")
done

fail=0
for suite in test_pure test_store_logic test_crypto; do
  "$CC" $FLAGS "$ROOT/tests/$suite.c" "${OBJS[@]}" -lm -o "$OUT/$suite"
  if "$OUT/$suite"; then
    echo "== $suite PASSED"
  else
    echo "== $suite FAILED"
    fail=1
  fi
done

echo
echo "== python tools suite"
if python3 "$ROOT/tests/test_tools.py"; then
  echo "== test_tools PASSED"
else
  echo "== test_tools FAILED"
  fail=1
fi

echo
if [ "$fail" -eq 0 ]; then
  echo "ALL HOST TESTS PASSED"
else
  echo "SOME HOST TESTS FAILED"
fi
exit "$fail"

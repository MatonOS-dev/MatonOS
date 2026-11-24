#!/usr/bin/env bash
set -Eeuo pipefail
src=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
compiler=${CXX:-$(command -v clang++ || command -v g++)}
"$compiler" -std=c++17 -Wall -Wextra -Werror -O2 "$src/switch_test.cpp" -o "$tmp/switch_test"
for scenario in already swap three states rollback; do
  unshare -Urn bash -c 'ip link set lo up; exec "$1" "$2"' _ "$tmp/switch_test" "$scenario"
  echo "PASS $scenario"
done

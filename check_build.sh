#!/bin/sh
# Local compile check of the mod WITHOUT GitHub Actions (clang, Linux, dedicated build: no video driver, but all the game and GUI code is compiled and linked).
# Usage: sh check_build.sh [work dir]      (needs git, cmake, ninja, clang++, python3 and network access to github.com; about 10 min the first time, seconds later)
# It clones OpenTTD 15.3, applies stock-market.patch, copies src/, runs apply_overlay.py, configures and builds with "ninja -k 0" so that ALL errors show in one run.
# It finds missing includes and the rules of section 3 of ROADMAP.md (safeguards.h bans to_string, malloc ...). It is NOT MSVC: a clean run here can still fail on CI
# for a compiler specific reason, but it has found real errors that CI had not reached yet.
set -e
MOD="$(cd "$(dirname "$0")" && pwd)"
WORK="${1:-/tmp/ottd-check}"
if [ ! -d "$WORK/openttd" ]; then
  mkdir -p "$WORK"
  git clone --depth 1 --branch 15.3 https://github.com/OpenTTD/OpenTTD.git "$WORK/openttd"
  (cd "$WORK/openttd" && git apply --whitespace=nowarn "$MOD/stock-market.patch")
fi
cp -r "$MOD/src/." "$WORK/openttd/src/"
python3 "$MOD/apply_overlay.py" "$WORK/openttd"
mkdir -p "$WORK/openttd/build"
cd "$WORK/openttd/build"
[ -f build.ninja ] || CC=clang CXX=clang++ cmake .. -GNinja -DCMAKE_BUILD_TYPE=Release -DOPTION_DEDICATED=ON
ninja -k 0 2>&1 | grep -E "error|FAILED|undefined reference|^ninja:" || echo "no errors"

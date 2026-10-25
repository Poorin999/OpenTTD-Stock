#!/bin/sh
# Stand-alone test of the Fed style monetary policy (no OpenTTD needed).
# It compiles src/central_bank.h with stub headers and the real text of MonetaryPolicy::Update / CheckUnscheduled taken from src/central_bank.cpp.
set -e
cd "$(dirname "$0")"
S=../../src
awk '/^bool MonetaryPolicy::Update/{f=1} f{print} f&&/^}/{exit}' $S/central_bank.cpp > update_body.inc
awk '/^bool MonetaryPolicy::CheckUnscheduled/{f=1} f{print} f&&/^}/{exit}' $S/central_bank.cpp > check_body.inc
g++ -std=c++17 -Wall -Wextra -Istub -I$S -o /tmp/cb_policy_test test.cpp
/tmp/cb_policy_test

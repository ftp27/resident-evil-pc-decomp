#!/usr/bin/env bash
# run_scenarios.sh - run every input script in tests/scenarios against a built
# game and report pass / fail (docs/TESTING.md).
#
#   bash tests/run_scenarios.sh [path/to/residentevil] [scenario.re1 ...]
#
# The binary defaults to build/macos/residentevil, then build/linux/residentevil.
# It needs game data: config.ini beside the binary must point [Assets] Path at
# a USA tree. Each scenario runs in its own folder under test-output/<name>/,
# where its captures, dumps and log land. Exit status: 0 = all passed.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${1:-}
if [ -n "$BIN" ] && [ "${BIN##*.}" != "re1" ]; then
    shift
else
    BIN=""
    for cand in "$ROOT/build/macos/residentevil" "$ROOT/build/linux/residentevil"; do
        if [ -x "$cand" ]; then BIN=$cand; break; fi
    done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    echo "no residentevil binary found; pass its path as the first argument" >&2
    exit 2
fi
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")

if [ $# -gt 0 ]; then
    SCENARIOS=("$@")
else
    SCENARIOS=("$ROOT"/tests/scenarios/*.re1)
fi

TIMEOUT=${RE1_TEST_TIMEOUT:-300}
OUT="$ROOT/test-output"
mkdir -p "$OUT"

pass=0
fail=0
for script in "${SCENARIOS[@]}"; do
    script=$(cd "$(dirname "$script")" && pwd)/$(basename "$script")
    name=$(basename "$script" .re1)
    dir="$OUT/$name"
    mkdir -p "$dir"
    start=$(date +%s)
    # perl's alarm works on stock macOS and Linux alike (no coreutils timeout).
    (cd "$dir" && perl -e 'alarm shift; exec @ARGV' "$TIMEOUT" \
        "$BIN" --script "$script" --fast > run.log 2>&1) 2>/dev/null
    rc=$?
    secs=$(( $(date +%s) - start ))
    case $rc in
        0)   verdict="PASS"; pass=$((pass + 1)) ;;
        1)   verdict="FAIL (expectations)"; fail=$((fail + 1)) ;;
        142) verdict="FAIL (timeout after ${TIMEOUT}s)"; fail=$((fail + 1)) ;;
        *)   if [ $rc -gt 128 ]; then verdict="FAIL (crashed: signal $((rc - 128)))"
             else verdict="FAIL (exit $rc)"; fi
             fail=$((fail + 1)) ;;
    esac
    printf '%-28s %-28s %4ss\n' "$name" "$verdict" "$secs"
    if [ $rc -ne 0 ]; then
        grep -E '^\[TEST\] FAIL|ptr32:' "$dir/run.log" | sed 's/^/    /'
    fi
done

echo "$pass passed, $fail failed  (logs and captures in test-output/)"
[ $fail -eq 0 ]

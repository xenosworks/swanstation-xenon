#!/bin/sh
# Checks a console run of a golden suite, as run_golden in tests/run_tests.sh does, or of one seed of its cpu suite.
#
# usage: tests/xenon/check.sh mdec|polyline|gte|cpu1|cpu2|cpu3|cpu4|cpu5|cpu6 swanstation_test.log
#
# swanstation_test.log is the log beside the .xex after a run of the folder tests/xenon/prepare.sh wrote for the suite.
# It must say the frontend returned 0, and its RAMDUMP words must match the suite's .expected file.
set -eu

SUITE=${1:?usage: check.sh mdec|polyline|gte|cpu1 to cpu6 swanstation_test.log}
LOG=${2:?usage: check.sh mdec|polyline|gte|cpu1 to cpu6 swanstation_test.log}
TESTS=$(cd "$(dirname "$0")/.." && pwd)

# The suite's expected file, then run_golden's case count and pairs.
case "$SUITE" in
  cpu[1-6])
    set -- cpu_fuzz
    ;;
  mdec)
    set -- mdec_decode 20 pairs=16
    ;;
  polyline)
    set -- gpu_polyline 4
    ;;
  gte)
    set -- gte_ops 24
    ;;
  *)
    echo "unknown suite $SUITE"
    exit 1
    ;;
esac
EXPECTED=$1
shift

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

tr -d '\r' < "$LOG" > "$WORK/log.txt"
if ! grep -q "^swanstation_test: the frontend returned 0$" "$WORK/log.txt"; then
  echo "$SUITE: the frontend did not return 0, or the run did not end; the end of the log:"
  tail -n 20 "$WORK/log.txt"
  exit 1
fi

# check_expected.py reads any line of two words as a RAMDUMP line; keep only those.
grep -E "^[0-9a-f]{8} [0-9a-f]{8}$" "$WORK/log.txt" > "$WORK/ramdump.txt" || true

# The cpu suite's expected file has every seed's checksums, each line starting "seed N ".
case "$SUITE" in
  cpu[1-6])
    seed=${SUITE#cpu}
    tr -d '\r' < "$TESTS/$EXPECTED.expected" | grep "^seed $seed " | sed "s/^seed $seed //" | diff - "$WORK/ramdump.txt"
    echo "$SUITE: the checksums match"
    exit 0
    ;;
esac
python3 "$TESTS/check_expected.py" "$WORK/ramdump.txt" "$TESTS/$EXPECTED.expected" "$@"

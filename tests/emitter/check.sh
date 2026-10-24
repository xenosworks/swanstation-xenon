#!/bin/sh
# Builds ppc_emitter_test for this host, runs it and compares the disassembly of what it emitted with ppc_emitter.expected.
# usage: tests/emitter/check.sh [work dir]; UPDATE_EXPECTED=1 rewrites the expected file. Needs g++ and
# powerpc-linux-gnu-objdump.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-$(mktemp -d)}
mkdir -p "$WORK"

g++ -std=c++17 -O1 -I"$HERE/.." -I"$HERE/../../src" "$HERE/ppc_emitter_test.cpp" \
	"$HERE/../../src/core/cpu_recompiler_emitter_ppc.cpp" -o "$WORK/ppc_emitter_test"
"$WORK/ppc_emitter_test" "$WORK/emitted.bin"
powerpc-linux-gnu-objdump -D -EB -b binary -m powerpc:common64 "$WORK/emitted.bin" | sed '1,/<.data>:/d' | sed 's/^ *[0-9a-f]*:\t//' > "$WORK/emitted.txt"

if [ "${UPDATE_EXPECTED:-}" = 1 ]; then
	cp "$WORK/emitted.txt" "$HERE/ppc_emitter.expected"
	echo "emitter: wrote tests/emitter/ppc_emitter.expected"
	exit 0
fi

diff "$HERE/ppc_emitter.expected" "$WORK/emitted.txt"
echo "emitter: the disassembly matches"

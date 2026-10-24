#!/bin/sh
# Prepares the console runs of tests/run_tests.sh's golden suites, mdec, polyline and gte, for swanstation_test.xex.
#
# usage: tests/xenon/prepare.sh out_dir
#
# Writes out_dir/mdec, out_dir/polyline and out_dir/gte. Each holds bios.bin, the suite's test ROM as run_golden builds it (the same
# clang flags, mkrom.py with seed 1 and mode 0), and frontend.txt, the frontend's command line and environment as
# swanstation_test reads them: the same frame counts and RAMDUMP range, the interpreter, fastmem off. Copy both beside
# swanstation_test.xex on the console, run it, and check swanstation_test.log with tests/xenon/check.sh.
#
# Needs clang, ld.lld and llvm-objcopy for the MIPS test ROMs, and python3.
set -eu

OUT=$(mkdir -p "${1:?usage: prepare.sh out_dir}" && cd "$1" && pwd)
TESTS=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
CLANG=${CLANG:-clang}
LLD=${LLD:-ld.lld}
OBJCOPY=${OBJCOPY:-llvm-objcopy}
trap 'rm -rf "$WORK"' EXIT

# As build_rom in run_tests.sh.
build_rom()
{
  name=$1
  flags="--target=mipsel-unknown-none -march=mips1 -mno-abicalls -fno-pic -G0 -O2 -ffreestanding -fno-builtin -nostdlib"
  for src in crt0.S "$name.c"; do
    if ! "$CLANG" $flags -c "$TESTS/guest/$src" -o "$WORK/${src%.*}.o" > "$WORK/cc.log" 2>&1; then
      cat "$WORK/cc.log"
      exit 1
    fi
    # clang calls MIPS I experimental; it is the console's CPU.
    grep -v "experimental" "$WORK/cc.log" || true
  done
  "$LLD" -T "$TESTS/guest/rom.ld" "$WORK/crt0.o" "$WORK/$name.o" -o "$WORK/$name.elf"
  "$OBJCOPY" -O binary -j .reset -j .magic -j .params -j .exc -j .text -j .rodata "$WORK/$name.elf" "$WORK/$name.rom"
}

# prepare_suite suite rom frames dump_words: run_golden's arguments, without its case counts, which check.sh has.
prepare_suite()
{
  suite=$1
  rom=$2
  build_rom "$rom"
  dir="$OUT/$suite"
  rm -rf "$dir"
  mkdir -p "$dir"
  python3 "$TESTS/guest/mkrom.py" "$WORK/$rom.rom" "$dir/bios.bin" 1 0
  # The first line is the frontend's command line after its name: a core path it ignores here, then the frames.
  # game: is the folder the .xex is in, where the frontend finds bios.bin as its system directory.
  cat > "$dir/frontend.txt" <<EOF
swanstation $3
FE_DIR=game:
FE_CPU=Interpreter
FE_FASTMEM=Disabled
RAMDUMP=0:$4
EOF
  echo "$suite: $dir"
}

prepare_suite mdec mdec_decode 1500 160
prepare_suite polyline gpu_polyline 600 80
prepare_suite gte gte_ops 200 160

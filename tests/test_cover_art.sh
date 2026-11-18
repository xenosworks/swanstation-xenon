#!/usr/bin/env bash
set -euo pipefail

# Testy portów nie wymagają konsoli; prawdziwy dekoder stb jest kompilowany z repozytorium.
repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
xsdk_dir="${XSDK_DIR:-$repo_dir/work/xsdk}"
imgui_dir="${IMGUI_XENON_DIR:-$repo_dir/work/imgui-xenon}"
scratch_dir="$(mktemp -d)"
trap 'rm -rf -- "$scratch_dir"' EXIT
cd "$scratch_dir"
cp -- "$repo_dir/tests/fixtures/cover-test.jpg" valid.jpg
python3 - <<'PY'
from pathlib import Path
import struct
import zlib


def chunk(kind, payload):
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))


def png(width, height):
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(b"\x00\xff\x00\x00\xff\x00\xff\x00\xff"))
        + chunk(b"IEND", b"")
    )


Path("valid.png").write_bytes(png(2, 1))
Path("wide.png").write_bytes(png(2049, 1))
Path("corrupt.png").write_bytes(png(2, 1)[:40])
Path("large.png").write_bytes(
    b"\x89PNG\r\n\x1a\n"
    + chunk(b"IHDR", struct.pack(">IIBBBBB", 600, 300, 8, 6, 0, 0, 0))
    + chunk(b"IDAT", zlib.compress((b"\x00" + b"\xff\x00\x00\xff" * 600) * 300))
    + chunk(b"IEND", b"")
)
with Path("oversized.bin").open("wb") as output:
    output.truncate(2 * 1024 * 1024 + 1)
PY

sanitizer_flags=()
if [[ "${SANITIZE:-0}" == 1 ]]; then
  # Rdzeń CDImage jest portem testowym, więc nie linkujemy jego RTTI do kontroli vptr.
  sanitizer_flags=(-fsanitize=address,undefined -fno-sanitize=vptr -fno-omit-frame-pointer)
fi
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -g \
  "${sanitizer_flags[@]}" \
  -I"$repo_dir/src" -I"$repo_dir/dep/libretro-common/include" -I"$repo_dir/dep/stb/include" \
  -I"$imgui_dir" -I"$imgui_dir/backends" -I"$xsdk_dir/include" \
  "$repo_dir/tests/cover_art_host.cpp" "$repo_dir/dep/stb/src/stb_image.c" "$repo_dir/dep/stb/src/stb_image_resize.c" \
  -Wl,--gc-sections -o cover_art_host
./cover_art_host

#!/usr/bin/env bash
set -euo pipefail

# Ten test nie łączy się z konsolą ani z Internetem.
project_root=$(cd "$(dirname "$0")/../.." && pwd)
xsdk_dir="${XSDK_DIR:-$project_root/work/xsdk}"
curl_dir="${CURL_XENON_DIR:-$project_root/work/curl-xenon}"
test_directory="$project_root/work/tests/cover-download"
mkdir -p "$test_directory"
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -I"$project_root/src/xenon" \
  "$project_root/src/xenon/cover_download.cpp" "$project_root/tests/xenon/test_cover_download.cpp" \
  -o "$test_directory/test_cover_download"
"$test_directory/test_cover_download" "$project_root/data/cover_fallback.tsv"
# Prawdziwe nagłówki curl, lecz testowy transport bez połączeń sieciowych.
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -pthread -DSWANSTATION_COVER_HTTPS=1 \
  -I"$project_root/src/xenon" -I"$curl_dir/include" -I"$xsdk_dir/include" \
  "$project_root/src/xenon/cover_download.cpp" "$project_root/tests/xenon/test_cover_download_https.cpp" \
  -o "$test_directory/test_cover_download_https"
(cd "$test_directory" && ./test_cover_download_https)

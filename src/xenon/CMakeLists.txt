# The console's host, built by the XSDK branch of the top-level CMakeLists.txt: host.c and the picker with the core
# linked in, packed into swanstation.xex. Beside it on the console go the BIOS, the games in games\ and, if wanted,
# host.txt (host.c's header).
enable_language(ASM)

# Dear ImGui for the picker, from a checkout of xenosworks/imgui-xenon (its docs/README-xsdk.md).
set(IMGUI_XENON_DIR "/mnt/z/imgui-xenon" CACHE PATH "A checkout of imgui-xenon")
add_library(imgui STATIC "${IMGUI_XENON_DIR}/imgui.cpp" "${IMGUI_XENON_DIR}/imgui_demo.cpp"
  "${IMGUI_XENON_DIR}/imgui_draw.cpp" "${IMGUI_XENON_DIR}/imgui_tables.cpp" "${IMGUI_XENON_DIR}/imgui_widgets.cpp"
  "${IMGUI_XENON_DIR}/backends/imgui_impl_xsdk.cpp")
target_include_directories(imgui PUBLIC "${IMGUI_XENON_DIR}" "${IMGUI_XENON_DIR}/backends")
# ImGui's default shell function calls fork() and execvp(), which the console hasn't got.
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)

add_executable(swanstation host.c picker.cpp cover_art.cpp cover_download.cpp fonts.S jit_pool.S)
target_include_directories(swanstation PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.."
  "${CMAKE_CURRENT_SOURCE_DIR}/../../dep/libretro-common/include")
target_link_libraries(swanstation PRIVATE swanstation_libretro imgui stb)

# Pobieranie można wyłączyć; lokalne okładki działają również bez bibliotek HTTPS.
option(COVERART_HTTPS "Pobieranie okładek przez zweryfikowane HTTPS" ON)
# Lokalne kopie zależności pozostają poza Git; ścieżki można nadpisać przez -D.
set(CURL_XENON_DIR "${CMAKE_SOURCE_DIR}/work/curl-xenon" CACHE PATH "Źródła curl-xenon")
set(MBEDTLS_XENON_DIR "${CMAKE_SOURCE_DIR}/work/mbedtls-xenon" CACHE PATH "Źródła mbedtls-xenon")
if(COVERART_HTTPS AND EXISTS "${CURL_XENON_DIR}/CMakeLists.txt" AND EXISTS "${MBEDTLS_XENON_DIR}/CMakeLists.txt")
  # Przykłady i testy zależności nie należą do wynikowego emulatora.
  set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  add_subdirectory("${MBEDTLS_XENON_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/mbedtls")
  # Targety niosą konfigurację Mbed TLS wymaganą przez port curl.
  set(MBEDTLS_INCLUDE_DIR "${MBEDTLS_XENON_DIR}/include" CACHE PATH "")
  set(MBEDTLS_LIBRARY mbedtls CACHE STRING "")
  set(MBEDX509_LIBRARY mbedx509 CACHE STRING "")
  set(MBEDCRYPTO_LIBRARY mbedcrypto CACHE STRING "")
  set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
  add_subdirectory("${CURL_XENON_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/curl")
  target_link_libraries(swanstation PRIVATE CURL::libcurl)
  target_compile_definitions(swanstation PRIVATE SWANSTATION_COVER_HTTPS=1)
  # Plik zaufanych CA musi leżeć obok XEX, aby pobieranie mogło zweryfikować certyfikat.
  configure_file("${MBEDTLS_XENON_DIR}/programs/xsdk/cacert.pem" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/cacert.pem" COPYONLY)
else()
  message(STATUS "Okładki: tylko lokalne pliki; HTTPS wyłączone lub brak curl-xenon/mbedtls-xenon")
endif()
# Mapa to metadane CC-BY-SA-4.0; pozostaje osobnym plikiem, niezależnie od licencji kodu.
configure_file("${CMAKE_SOURCE_DIR}/data/cover_fallback.tsv" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/cover_fallback.tsv" COPYONLY)
configure_file("${CMAKE_SOURCE_DIR}/data/cover_fallback.source.json" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/cover_fallback.source.json" COPYONLY)
set_source_files_properties(host.c PROPERTIES COMPILE_OPTIONS "-std=gnu17;-Wall;-Wextra;-Werror")
set_source_files_properties(picker.cpp PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
set_source_files_properties(cover_art.cpp cover_download.cpp PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
# The assembler looks for .incbin's files beside this file, and fonts.S is built again when a font changes.
set_source_files_properties(fonts.S PROPERTIES COMPILE_OPTIONS "-Wa,-I${CMAKE_CURRENT_SOURCE_DIR}"
  OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fonts/ArchivoNarrow-Regular.ttf;${CMAKE_CURRENT_SOURCE_DIR}/fonts/ArchivoNarrow-Medium.ttf")
# Writes swanstation_shaders.h, with frame_vert and frame_frag.
xsdk_add_shaders(swanstation frame.vert frame.frag)
xsdk_add_xex(swanstation TITLE_ID 0x58535353 NAME "SwanStation" VERSION 0.1.2.0)

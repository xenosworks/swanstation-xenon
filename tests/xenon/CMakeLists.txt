# The console's test host, built by the XSDK branch of the top-level CMakeLists.txt: tests/harness/frontend.c with the
# core linked in, packed into swanstation_test.xex. Beside it on the console go frontend.txt and bios.bin, which
# tests/xenon/prepare.sh writes for each suite.
add_executable(swanstation_test
  ../harness/frontend.c
  ../harness/frontend_xsdk.c
)
target_include_directories(swanstation_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/libretro"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../dep/libretro-common/include"
)
target_link_libraries(swanstation_test PRIVATE swanstation_libretro)
set_source_files_properties(../harness/frontend_xsdk.c PROPERTIES COMPILE_OPTIONS "-std=gnu17;-Wall;-Wextra;-Werror")
xsdk_add_xex(swanstation_test TITLE_ID 0x58535357 NAME "swanstation_test")

# The console's host, built by the XSDK branch of the top-level CMakeLists.txt: host.c with the core linked in, packed
# into swanstation.xex. Beside it on the console go host.txt, the BIOS and the game (host.c's header).
add_executable(swanstation host.c)
target_include_directories(swanstation PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.."
  "${CMAKE_CURRENT_SOURCE_DIR}/../../dep/libretro-common/include")
target_link_libraries(swanstation PRIVATE swanstation_libretro)
set_source_files_properties(host.c PROPERTIES COMPILE_OPTIONS "-std=gnu17;-Wall;-Wextra;-Werror")
# Writes swanstation_shaders.h, with frame_vert and frame_frag.
xsdk_add_shaders(swanstation frame.vert frame.frag)
xsdk_add_xex(swanstation TITLE_ID 0x58535353 NAME "SwanStation")

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

add_executable(swanstation host.c picker.cpp fonts.S jit_pool.S)
target_include_directories(swanstation PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.."
  "${CMAKE_CURRENT_SOURCE_DIR}/../../dep/libretro-common/include")
target_link_libraries(swanstation PRIVATE swanstation_libretro imgui)
set_source_files_properties(host.c PROPERTIES COMPILE_OPTIONS "-std=gnu17;-Wall;-Wextra;-Werror")
set_source_files_properties(picker.cpp PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
# The assembler looks for .incbin's files beside this file, and fonts.S is built again when a font changes.
set_source_files_properties(fonts.S PROPERTIES COMPILE_OPTIONS "-Wa,-I${CMAKE_CURRENT_SOURCE_DIR}"
  OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fonts/ArchivoNarrow-Regular.ttf;${CMAKE_CURRENT_SOURCE_DIR}/fonts/ArchivoNarrow-Medium.ttf")
# Writes swanstation_shaders.h, with frame_vert and frame_frag.
xsdk_add_shaders(swanstation frame.vert frame.frag)
xsdk_add_xex(swanstation TITLE_ID 0x58535353 NAME "SwanStation" VERSION 0.1.0.0)

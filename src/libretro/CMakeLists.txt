# A static library on the console, which loads no shared ones.
if(XSDK)
  set(LIBRETRO_CORE_TYPE STATIC)
else()
  set(LIBRETRO_CORE_TYPE SHARED)
endif()

add_library(swanstation_libretro ${LIBRETRO_CORE_TYPE}
  libretro_audio_stream.cpp
  libretro_audio_stream.h
  libretro_game_settings.cpp
  libretro_game_settings.h
  libretro_host_display.cpp
  libretro_host_display.h
  libretro_host_interface.cpp
  libretro_settings_interface.cpp
  libretro_settings_interface.h
)

if(SOFTWARE_RENDERER_ONLY)
  target_link_libraries(swanstation_libretro PRIVATE core common libretro-common)
else()
  target_link_libraries(swanstation_libretro PRIVATE core common glad vulkan-loader libretro-common)
endif()

# no lib prefix
set_target_properties(swanstation_libretro PROPERTIES PREFIX "")

# drop in the build directory
set_target_properties(swanstation_libretro PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set_target_properties(swanstation_libretro PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# work around .sln issue of hardcoded config directories
if(WIN32)
  set_target_properties(swanstation_libretro PROPERTIES RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}")
  set_target_properties(swanstation_libretro PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
endif()

# for android, suffix _android
if(ANDROID)
  set_target_properties(swanstation_libretro PROPERTIES OUTPUT_NAME "swanstation_libretro_android")
endif()

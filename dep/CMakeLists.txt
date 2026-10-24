# No OpenGL or Vulkan where there's the software renderer only.
if(NOT SOFTWARE_RENDERER_ONLY)
  add_subdirectory(glad)
endif()
add_subdirectory(stb)
add_subdirectory(libchdr EXCLUDE_FROM_ALL)
add_subdirectory(xxhash)
if(NOT SOFTWARE_RENDERER_ONLY)
  add_subdirectory(vulkan-loader)
endif()
if(XSDK)
  # xsdk refuses file-backed mappings (ENOTSUP), so libretro-common reads files with the unmapped fallbacks.
  set(LIBRETRO_COMMON_NO_MMAP ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(libretro-common)
add_subdirectory(openbios)

if(${CPU_ARCH} STREQUAL "aarch32" OR ${CPU_ARCH} STREQUAL "aarch64")
  add_subdirectory(vixl)
endif()

if(XSDK)
  # xsdk has no madvise (xsdk:include/sys/mman.h), so memdecommit() leaves the pages committed.
  target_compile_options(libretro-common PRIVATE "-Dmadvise(address,length,advice)=((void)0)")
endif()

# Configure libchdr to use libretro's file IO.
target_compile_definitions(chdr-static PUBLIC USE_LIBRETRO_VFS)
target_include_directories(chdr-static PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libretro-common/include")

# Make it easier to include miniz's header.
target_include_directories(miniz INTERFACE "libchdr/deps/miniz-3.1.1")

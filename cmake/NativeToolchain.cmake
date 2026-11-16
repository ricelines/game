cmake_minimum_required(VERSION 3.29)

get_filename_component(AMBER_CHECKOUT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(CMAKE_C_COMPILER "${AMBER_CHECKOUT}/.tools/bin/cc" CACHE FILEPATH "Pinned GCC")
set(CMAKE_CXX_COMPILER "${AMBER_CHECKOUT}/.tools/bin/c++" CACHE FILEPATH "Pinned G++")
set(CMAKE_MAKE_PROGRAM "${AMBER_CHECKOUT}/.tools/bin/ninja" CACHE FILEPATH "Ninja")
# A custom linker type keeps GCC's linker search inside the pinned toolchain.
set(CMAKE_LINKER_TYPE amber_mold CACHE STRING "Pinned mold linker")
foreach(language C CXX)
  set(CMAKE_${language}_USING_LINKER_amber_mold
      "-B${AMBER_CHECKOUT}/.tools/bin;-fuse-ld=mold")
endforeach()
set(AMBER_COMPILER_CACHE
    "${AMBER_CHECKOUT}/.tools/bin/ccache;cache_dir=${AMBER_CHECKOUT}/.cache/ccache;compiler_check=content")
set(CMAKE_C_COMPILER_LAUNCHER "${AMBER_COMPILER_CACHE}" CACHE STRING "Compiler cache")
set(CMAKE_CXX_COMPILER_LAUNCHER "${AMBER_COMPILER_CACHE}" CACHE STRING "Compiler cache")

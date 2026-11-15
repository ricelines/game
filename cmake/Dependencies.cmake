# Import the separately built libraries without recompiling them in each application build.
include_guard(GLOBAL)
find_package(Threads REQUIRED)
get_filename_component(AMBERGRIS_CHECKOUT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(AMBERGRIS_DEPENDENCY_EXPORT
  "${AMBERGRIS_CHECKOUT}/.build/third-party/AmbergrisDependenciesTargets.cmake")
if(NOT EXISTS "${AMBERGRIS_DEPENDENCY_EXPORT}")
  message(FATAL_ERROR "Run python3 tools/bootstrap.py --build before configuring native consumers")
endif()
include("${AMBERGRIS_DEPENDENCY_EXPORT}")
# Pinned Jolt is built without C++ RTTI. UBSan's vptr check would reference RTTI
# symbols that deliberately do not exist; all other ASan/UBSan checks remain on.
target_compile_options(Ambergris::Jolt INTERFACE
  "$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-fno-sanitize=vptr>")

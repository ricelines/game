cmake_minimum_required(VERSION 3.24)
project(AmbergrisDependencies LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
get_filename_component(CHECKOUT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SOURCES "${CHECKOUT}/third_party")
find_package(Threads REQUIRED)

set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(GENERATE_DEBUG_SYMBOLS OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
add_subdirectory("${SOURCES}/jolt/Build" jolt EXCLUDE_FROM_ALL)
# Jolt's PCH makes these compilations uncacheable with our ccache settings.
set_target_properties(Jolt PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)

set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${SOURCES}/recast" recast EXCLUDE_FROM_ALL)
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${SOURCES}/luau" luau EXCLUDE_FROM_ALL)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory("${SOURCES}/zstd/build/cmake" zstd EXCLUDE_FROM_ALL)
set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_GLTFPACK OFF CACHE BOOL "" FORCE)
add_subdirectory("${SOURCES}/meshoptimizer" meshoptimizer EXCLUDE_FROM_ALL)
option(AMBERGRIS_TRACY "Enable Tracy instrumentation" OFF)
set(TRACY_ENABLE ${AMBERGRIS_TRACY} CACHE BOOL "" FORCE)
set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
add_subdirectory("${SOURCES}/tracy" tracy EXCLUDE_FROM_ALL)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
add_subdirectory("${SOURCES}/json" json EXCLUDE_FROM_ALL)

file(GLOB USOCKETS_SOURCES
  "${SOURCES}/usockets/src/*.c"
  "${SOURCES}/usockets/src/eventing/*.c"
  "${SOURCES}/usockets/src/crypto/*.c"
  "${SOURCES}/usockets/src/io_uring/*.c")
add_library(uSockets STATIC ${USOCKETS_SOURCES})
target_include_directories(uSockets SYSTEM PUBLIC "${SOURCES}/usockets/src")
target_compile_definitions(uSockets PUBLIC LIBUS_NO_SSL)
target_link_libraries(uSockets PUBLIC Threads::Threads)
add_library(WebSockets INTERFACE)
target_include_directories(WebSockets SYSTEM INTERFACE "${SOURCES}/uwebsockets/src")
target_compile_definitions(WebSockets INTERFACE UWS_NO_ZLIB)
target_link_libraries(WebSockets INTERFACE uSockets)
add_library(bitsery INTERFACE)
target_include_directories(bitsery SYSTEM INTERFACE "${SOURCES}/bitsery/include")
add_library(cgltf INTERFACE)
target_include_directories(cgltf SYSTEM INTERFACE "${SOURCES}/cgltf")
add_library(tomlplusplus INTERFACE)
target_include_directories(tomlplusplus SYSTEM INTERFACE "${SOURCES}/tomlplusplus/include")
add_library(picosha2 INTERFACE)
target_include_directories(picosha2 SYSTEM INTERFACE "${SOURCES}/picosha2")

set(EXPORTED_LIBRARIES
  Jolt Recast Detour DetourCrowd
  Luau.Common Luau.Ast Luau.Bytecode Luau.Compiler Luau.VM Luau.VM.Internals
  uSockets WebSockets bitsery libzstd_static meshoptimizer cgltf TracyClient nlohmann_json
  tomlplusplus picosha2)
export(TARGETS ${EXPORTED_LIBRARIES} NAMESPACE Ambergris::
  FILE "${CMAKE_BINARY_DIR}/AmbergrisDependenciesTargets.cmake")
add_custom_target(ambergris_dependencies ALL DEPENDS
  Jolt Recast Detour DetourCrowd Luau.Compiler Luau.VM
  uSockets libzstd_static meshoptimizer TracyClient)

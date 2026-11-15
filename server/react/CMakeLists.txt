add_library(amber_react STATIC
  values.cpp
  watches.cpp
  vm.cpp
  execution.cpp
  installation.cpp
  activation.cpp
  install_dispatch.cpp
  install_watches.cpp
  snapshot.cpp
  allocation.cpp
  catalog.cpp
  catalog_snapshot.cpp
  artifacts.cpp
  registry.cpp
  authoring_install.cpp
  authoring_worker.cpp
  action_declarations.cpp
  signature.cpp
)
target_include_directories(amber_react PUBLIC ${PROJECT_SOURCE_DIR}/server)
target_compile_features(amber_react PUBLIC cxx_std_20)
target_link_libraries(amber_react PUBLIC amber_store amber_record amber_snap PRIVATE Ambergris::Luau.Compiler Ambergris::Luau.Ast Ambergris::Luau.VM Ambergris::TracyClient Threads::Threads)
add_executable(amb_script_validate script_worker.cpp)
target_link_libraries(amb_script_validate PRIVATE amber_react)

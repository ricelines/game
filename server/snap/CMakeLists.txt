add_library(amber_snap STATIC
  snap.cpp
  objects.cpp
  catalog.cpp
  parts.cpp
)
target_include_directories(amber_snap PUBLIC ${PROJECT_SOURCE_DIR}/server)
target_compile_features(amber_snap PUBLIC cxx_std_20)
target_link_libraries(amber_snap PUBLIC amber_store Ambergris::bitsery PRIVATE Ambergris::libzstd_static Ambergris::TracyClient)

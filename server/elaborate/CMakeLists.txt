add_library(amber_elaborate STATIC
  elaborate.cpp
  worker.cpp
  process.cpp
  snapshot.cpp
  fixture.cpp
)
target_include_directories(amber_elaborate PUBLIC ${PROJECT_SOURCE_DIR}/server)
target_compile_features(amber_elaborate PUBLIC cxx_std_20)
target_link_libraries(amber_elaborate PUBLIC amber_store amber_record amber_snap PRIVATE Ambergris::picosha2 Ambergris::libzstd_static Threads::Threads)

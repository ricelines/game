add_library(amber_perm STATIC
  perm.cpp
  snapshot.cpp
)
target_include_directories(amber_perm PUBLIC ${PROJECT_SOURCE_DIR}/server)
target_compile_features(amber_perm PUBLIC cxx_std_20)
target_link_libraries(amber_perm PUBLIC Ambergris::nlohmann_json amber_store amber_snap)

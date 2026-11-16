add_library(amber_record STATIC
  record.cpp
)
target_include_directories(amber_record PUBLIC ${PROJECT_SOURCE_DIR}/server)
target_compile_features(amber_record PUBLIC cxx_std_20)
target_link_libraries(amber_record PUBLIC Ambergris::nlohmann_json amber_store PRIVATE amber_snap Ambergris::libzstd_static Threads::Threads)

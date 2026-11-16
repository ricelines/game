add_library(amber_store STATIC
  store.cpp
  instances.cpp
)
target_include_directories(amber_store PUBLIC ${PROJECT_SOURCE_DIR}/server)
target_compile_features(amber_store PUBLIC cxx_std_20)

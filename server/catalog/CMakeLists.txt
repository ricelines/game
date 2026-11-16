add_library(amber_catalog STATIC
  assets.cpp
  definition.cpp
  catalog.cpp
  snapshot.cpp
  artifacts.cpp
  registry.cpp
  models_snapshot.cpp
  instances.cpp
  native_actions.cpp
)
target_include_directories(amber_catalog PUBLIC ${PROJECT_SOURCE_DIR}/server)
target_compile_features(amber_catalog PUBLIC cxx_std_20)
target_link_libraries(amber_catalog PUBLIC amber_store amber_react amber_snap PRIVATE Ambergris::picosha2)

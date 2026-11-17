add_library(amber_space STATIC
  space.cpp
  assembly.cpp
  custody.cpp
  properties.cpp
  navigation.cpp
  snapshot.cpp
  perception.cpp
)
target_include_directories(amber_space PUBLIC ${PROJECT_SOURCE_DIR}/server)
target_compile_features(amber_space PUBLIC cxx_std_20)
target_link_libraries(amber_space PUBLIC amber_store amber_snap PRIVATE Ambergris::Jolt Ambergris::Recast Ambergris::Detour Ambergris::DetourCrowd Ambergris::TracyClient)

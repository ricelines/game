add_library(amber_net STATIC
  replication.cpp
  server.cpp
  profiles.cpp
  profiles_snapshot.cpp
  protocol.cpp
  sessions.cpp
  presentation.cpp
  perception_format.cpp
  scene_replication.cpp
  read_actions.cpp
  read_events.cpp
  read_inspect.cpp
  read_land.cpp
  read_look.cpp
)
target_include_directories(amber_net PUBLIC ${PROJECT_SOURCE_DIR}/server)
target_compile_features(amber_net PUBLIC cxx_std_20)
target_link_libraries(amber_net PUBLIC amber_catalog amber_store amber_snap amber_react amber_perm amber_space PRIVATE Ambergris::WebSockets Ambergris::bitsery Ambergris::picosha2 Ambergris::TracyClient Threads::Threads)

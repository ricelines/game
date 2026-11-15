add_library(amber_record record.cpp)
target_link_libraries(amber_record PUBLIC amber_store Ambergris::nlohmann_json
                                  PRIVATE Ambergris::libzstd_static Threads::Threads)

if(BUILD_TESTING)
  add_executable(record_test ../../tests/record_test.cpp)
  target_link_libraries(record_test PRIVATE amber_record Ambergris::libzstd_static)
  add_test(NAME record COMMAND record_test)
endif()

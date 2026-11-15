# Integration code shares a library; engine owners retain their independent targets.
# Execution stays separate because drive uses it without depending on integration.
add_library(amber_bindings STATIC
  allocation/allocation.cpp
  arguments/arguments.cpp
  authoring/failures.cpp
  authoring/install.cpp
  authoring/worker.cpp
  authority/descriptors.cpp
  causality.cpp
  causality_snapshot.cpp
  checkpoint/checkpoint.cpp
  commerce/admin.cpp
  commerce/custody.cpp
  commerce/mailbox.cpp
  commerce/offers.cpp
  commerce/payments.cpp
  commerce/placement.cpp
  commerce/schema.cpp
  commerce/schema_admin.cpp
  commerce/trades.cpp
  construction/construction.cpp
  controls/controls.cpp
  editing/actions.cpp
  editing/claims.cpp
  editing/properties.cpp
  editing/registry.cpp
  editing/schema.cpp
  editing/scripts.cpp
  elaboration/operations.cpp
  elaboration/rules.cpp
  elaboration/schema.cpp
  finance/finance.cpp
  fixture/fixture.cpp
  geometry/geometry.cpp
  geometry/material.cpp
  goods.cpp
  illumination.cpp
  installation/activation.cpp
  installation/installation.cpp
  measurements.cpp
  metadata/metadata.cpp
  navigation.cpp
  physical.cpp
  presentation.cpp
  projection.cpp
  replication.cpp
  scene.cpp
  start/catalog.cpp
  start/objects.cpp
  start/permissions.cpp
  structural/replacement.cpp
  structural/revision.cpp
  structural/structural.cpp
  travel.cpp
  turns/actions.cpp
  turns/handlers.cpp
  viewing/actions.cpp
  viewing/events.cpp
  viewing/inspect.cpp
  viewing/look.cpp
)
target_include_directories(amber_bindings PUBLIC ${PROJECT_SOURCE_DIR}/server)
target_link_libraries(amber_bindings PUBLIC
  amber_catalog amber_command amber_control amber_drive amber_elaborate amber_execution
  amber_install amber_lifecycle amber_net amber_observe amber_perm amber_react amber_snap amber_space
  PRIVATE Ambergris::Luau.VM Ambergris::TracyClient Threads::Threads)

if(BUILD_TESTING)
  add_executable(physical_test ${PROJECT_SOURCE_DIR}/tests/physical_test.cpp)
  set_target_properties(physical_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(physical_test PRIVATE amber_bindings Ambergris::Jolt Ambergris::tomlplusplus)
  target_compile_definitions(physical_test PRIVATE AMBER_CONFIG_PATH="${PROJECT_SOURCE_DIR}/config.toml")
  add_test(NAME physical COMMAND physical_test)
  add_executable(physical_causality_test ${PROJECT_SOURCE_DIR}/tests/physical_causality_test.cpp)
  set_target_properties(physical_causality_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(physical_causality_test PRIVATE amber_bindings amber_execution Ambergris::Jolt Ambergris::tomlplusplus)
  target_compile_definitions(physical_causality_test PRIVATE AMBER_CONFIG_PATH="${PROJECT_SOURCE_DIR}/config.toml")
  add_test(NAME physical_causality COMMAND physical_causality_test)
  add_executable(illumination_test ${PROJECT_SOURCE_DIR}/tests/illumination_test.cpp)
  set_target_properties(illumination_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(illumination_test PRIVATE amber_bindings amber_install Ambergris::Jolt Ambergris::tomlplusplus)
  target_compile_definitions(illumination_test PRIVATE AMBER_CONFIG_PATH="${PROJECT_SOURCE_DIR}/config.toml")
  add_test(NAME illumination COMMAND illumination_test)
  add_executable(presentation_test ${PROJECT_SOURCE_DIR}/tests/presentation_test.cpp)
  set_target_properties(presentation_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(presentation_test PRIVATE amber_bindings Ambergris::Jolt Ambergris::tomlplusplus)
  target_compile_definitions(presentation_test PRIVATE AMBER_CONFIG_PATH="${PROJECT_SOURCE_DIR}/config.toml")
  add_test(NAME presentation COMMAND presentation_test)
  add_executable(measurements_test ${PROJECT_SOURCE_DIR}/tests/measurements_test.cpp)
  set_target_properties(measurements_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(measurements_test PRIVATE amber_bindings)
  add_test(NAME measurements COMMAND measurements_test)
  add_executable(goods_test ${PROJECT_SOURCE_DIR}/tests/goods_test.cpp)
  set_target_properties(goods_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(goods_test PRIVATE amber_bindings Ambergris::Jolt Ambergris::tomlplusplus)
  target_compile_definitions(goods_test PRIVATE AMBER_CONFIG_PATH="${PROJECT_SOURCE_DIR}/config.toml")
  add_test(NAME goods COMMAND goods_test)
  add_executable(projection_lifecycle_test ${PROJECT_SOURCE_DIR}/tests/projection_lifecycle_test.cpp)
  set_target_properties(projection_lifecycle_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(projection_lifecycle_test PRIVATE amber_bindings Ambergris::Jolt Ambergris::tomlplusplus)
  target_compile_definitions(projection_lifecycle_test PRIVATE AMBER_CONFIG_PATH="${PROJECT_SOURCE_DIR}/config.toml")
  add_test(NAME projection_lifecycle COMMAND projection_lifecycle_test)
  add_executable(navigation_diagnostics_test ${PROJECT_SOURCE_DIR}/tests/navigation_diagnostics_test.cpp)
  set_target_properties(navigation_diagnostics_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(navigation_diagnostics_test PRIVATE amber_bindings Ambergris::Jolt Ambergris::tomlplusplus)
  target_compile_definitions(navigation_diagnostics_test PRIVATE AMBER_CONFIG_PATH="${PROJECT_SOURCE_DIR}/config.toml")
  add_test(NAME navigation_diagnostics COMMAND navigation_diagnostics_test)
  add_executable(travel_test ${PROJECT_SOURCE_DIR}/tests/travel_test.cpp)
  set_target_properties(travel_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(travel_test PRIVATE amber_bindings)
  add_test(NAME travel COMMAND travel_test)
  add_executable(allocation_bench ${PROJECT_SOURCE_DIR}/tests/allocation_bench.cpp)
  set_target_properties(allocation_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/allocation")
  target_link_libraries(allocation_bench PRIVATE amber_bindings)
  add_executable(authoring_test ${PROJECT_SOURCE_DIR}/tests/authoring_test.cpp)
  set_target_properties(authoring_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/authoring")
  target_link_libraries(authoring_test PRIVATE amber_bindings)
  add_executable(checkpoint_test ${PROJECT_SOURCE_DIR}/tests/checkpoint_test.cpp)
  set_target_properties(checkpoint_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/checkpoint")
  target_link_libraries(checkpoint_test PRIVATE amber_bindings)
  add_test(NAME checkpoint COMMAND checkpoint_test)
  set_tests_properties(checkpoint PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/checkpoint")
  add_executable(checkpoint_bench ${PROJECT_SOURCE_DIR}/tests/checkpoint_bench.cpp)
  set_target_properties(checkpoint_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/checkpoint")
  target_link_libraries(checkpoint_bench PRIVATE amber_bindings)
  add_executable(commerce_test ${PROJECT_SOURCE_DIR}/tests/commerce_test.cpp)
  set_target_properties(commerce_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/commerce")
  target_link_libraries(commerce_test PRIVATE amber_bindings Ambergris::Jolt Ambergris::tomlplusplus)
  target_compile_definitions(commerce_test PRIVATE AMBER_CONFIG_PATH="${PROJECT_SOURCE_DIR}/config.toml")
  add_test(NAME commerce COMMAND commerce_test)
  set_tests_properties(commerce PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/commerce")
  add_executable(commerce_bench ${PROJECT_SOURCE_DIR}/tests/commerce_bench.cpp)
  set_target_properties(commerce_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/commerce")
  target_link_libraries(commerce_bench PRIVATE amber_bindings)
  add_executable(editing_test ${PROJECT_SOURCE_DIR}/tests/editing_test.cpp)
  set_target_properties(editing_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/editing")
  target_link_libraries(editing_test PRIVATE amber_bindings)
  target_link_libraries(editing_test PRIVATE Ambergris::Jolt Ambergris::tomlplusplus)
  target_compile_definitions(editing_test PRIVATE AMBER_CONFIG_PATH="${PROJECT_SOURCE_DIR}/config.toml")
  add_test(NAME editing COMMAND editing_test)
  set_tests_properties(editing PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/editing")
  add_executable(elaboration_bindings_test ${PROJECT_SOURCE_DIR}/tests/elaboration_bindings_test.cpp)
  set_target_properties(elaboration_bindings_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/elaboration")
  target_link_libraries(elaboration_bindings_test PRIVATE amber_bindings)
  add_test(NAME elaboration_bindings COMMAND elaboration_bindings_test $<TARGET_FILE:fixture_transport_probe>)
  set_tests_properties(elaboration_bindings PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/elaboration")
  add_executable(financial_test ${PROJECT_SOURCE_DIR}/tests/financial_test.cpp)
  set_target_properties(financial_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/finance")
  target_link_libraries(financial_test PRIVATE amber_bindings Ambergris::Jolt Ambergris::tomlplusplus)
  target_compile_definitions(financial_test PRIVATE AMBER_CONFIG_PATH="${PROJECT_SOURCE_DIR}/config.toml")
  add_test(NAME financial COMMAND financial_test)
  set_tests_properties(financial PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/finance")
  add_executable(rule_activation_test ${PROJECT_SOURCE_DIR}/tests/rule_activation_test.cpp)
  set_target_properties(rule_activation_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/installation")
  target_link_libraries(rule_activation_test PRIVATE amber_bindings)
  add_test(NAME rule_activation COMMAND rule_activation_test)
  set_tests_properties(rule_activation PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/installation")
  add_executable(rule_activation_bench ${PROJECT_SOURCE_DIR}/tests/rule_activation_bench.cpp)
  set_target_properties(rule_activation_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/installation")
  target_link_libraries(rule_activation_bench PRIVATE amber_bindings)
  add_executable(allocation_test ${PROJECT_SOURCE_DIR}/tests/allocation_test.cpp)
  set_target_properties(allocation_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/installation")
  target_link_libraries(allocation_test PRIVATE amber_bindings Ambergris::Jolt Ambergris::tomlplusplus)
  target_compile_definitions(allocation_test PRIVATE AMBER_CONFIG_PATH="${PROJECT_SOURCE_DIR}/config.toml")
  add_test(NAME allocation COMMAND allocation_test)
  set_tests_properties(allocation PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/installation")
  add_executable(script_metadata_test ${PROJECT_SOURCE_DIR}/tests/script_metadata_test.cpp)
  set_target_properties(script_metadata_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/metadata")
  target_link_libraries(script_metadata_test PRIVATE amber_bindings)
  add_test(NAME script_metadata COMMAND script_metadata_test)
  set_tests_properties(script_metadata PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/metadata")

  add_executable(model_revision_bindings_test ${PROJECT_SOURCE_DIR}/tests/model_revision_bindings_test.cpp)
  set_target_properties(model_revision_bindings_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/revision")
  target_link_libraries(model_revision_bindings_test PRIVATE amber_bindings Ambergris::Jolt Ambergris::tomlplusplus)
  target_compile_definitions(model_revision_bindings_test PRIVATE AMBER_CONFIG_PATH="${PROJECT_SOURCE_DIR}/config.toml")
  add_test(NAME model_revision_bindings COMMAND model_revision_bindings_test)
  set_tests_properties(model_revision_bindings PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/revision")
  add_executable(start_test ${PROJECT_SOURCE_DIR}/tests/start_test.cpp)
  set_target_properties(start_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/start")
  target_link_libraries(start_test PRIVATE amber_bindings)
  add_executable(structural_test ${PROJECT_SOURCE_DIR}/tests/structural_test.cpp)
  set_target_properties(structural_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/structural")
  target_link_libraries(structural_test PRIVATE amber_bindings Ambergris::Jolt Ambergris::tomlplusplus)
  target_compile_definitions(structural_test PRIVATE AMBER_CONFIG_PATH="${PROJECT_SOURCE_DIR}/config.toml")
  add_test(NAME structural COMMAND structural_test)
  set_tests_properties(structural PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/structural")
  add_executable(turns_test ${PROJECT_SOURCE_DIR}/tests/turns_test.cpp)
  set_target_properties(turns_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/turns")
  target_link_libraries(turns_test PRIVATE amber_bindings Ambergris::Jolt Ambergris::tomlplusplus)
  target_compile_definitions(turns_test PRIVATE AMBER_CONFIG_PATH="${PROJECT_SOURCE_DIR}/config.toml")
  add_test(NAME turns COMMAND turns_test)
  set_tests_properties(turns PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/turns")
  add_executable(viewing_test ${PROJECT_SOURCE_DIR}/tests/viewing_test.cpp)
  set_target_properties(viewing_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/viewing")
  target_link_libraries(viewing_test PRIVATE amber_bindings Ambergris::Jolt Ambergris::tomlplusplus)
  target_compile_definitions(viewing_test PRIVATE AMBER_CONFIG_PATH="${PROJECT_SOURCE_DIR}/config.toml")
  add_test(NAME viewing COMMAND viewing_test)
  set_tests_properties(viewing PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/viewing")
  add_executable(commerce_admin_test ${PROJECT_SOURCE_DIR}/tests/commerce_admin_test.cpp)
  set_target_properties(commerce_admin_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/commerce")
  target_link_libraries(commerce_admin_test PRIVATE amber_bindings)
  add_test(NAME commerce_admin COMMAND commerce_admin_test)
  set_tests_properties(commerce_admin PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/commerce")
endif()

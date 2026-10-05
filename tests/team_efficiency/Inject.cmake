# Injected with CMAKE_PROJECT_TOP_LEVEL_INCLUDES so the normal repository root remains
# CMAKE_SOURCE_DIR. The callback runs after the root CMakeLists has created oe_team.
function(oe_add_team_efficiency_tests)
  if(NOT TARGET OwnEngine OR NOT TARGET oe_team)
    message(FATAL_ERROR "team efficiency tests require the native OwnEngine + oe_team targets")
  endif()
  add_executable(oe_team_efficiency_tests ${CMAKE_SOURCE_DIR}/tests/team_efficiency_tests.cpp)
  target_link_libraries(oe_team_efficiency_tests PRIVATE OwnEngine oe_team)
  enable_testing()
  add_test(NAME oe_team_efficiency_tests COMMAND oe_team_efficiency_tests)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL oe_add_team_efficiency_tests)

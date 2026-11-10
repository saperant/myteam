add_executable(taskml-tests test_main.cpp)
target_link_libraries(taskml-tests PRIVATE taskml::taskml)
target_compile_definitions(taskml-tests PRIVATE TASKML_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME taskml-tests COMMAND taskml-tests)

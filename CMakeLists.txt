cmake_minimum_required(VERSION 3.20)

project(myteam
    VERSION 0.1.0
    DESCRIPTION "A team of AI agents working through a Markdown plan"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(MYTEAM_BUILD_TESTS "Build the unit tests (needs GoogleTest)" ON)

# --- tasks/ (the plan parser: TaskML), built in statically ------------------------
set(BUILD_SHARED_LIBS OFF)
set(TASKML_BUILD_TESTS ${MYTEAM_BUILD_TESTS})
if(MYTEAM_BUILD_TESTS)
    enable_testing()
endif()
add_subdirectory(tasks)

# --- core library -----------------------------------------------------------------
add_library(myteam_core STATIC
    src/blockers.cpp
    src/contracts.cpp
    src/changelog.cpp
    src/coach.cpp
    src/memory.cpp
    src/context.cpp
    src/feedback.cpp
    src/mcp.cpp
    src/skills.cpp
    src/guard.cpp
    src/inspect.cpp
    src/checkpoint.cpp
    src/commits.cpp
    src/services.cpp
    src/sandbox.cpp
    src/frontmatter.cpp
    src/loop.cpp
    src/plan.cpp
    src/process.cpp
    src/project.cpp
    src/report.cpp
    src/runner.cpp
    src/team.cpp
    src/util.cpp
    src/yaml.cpp
    src/runners/claude.cpp
    src/runners/codex.cpp
    src/runners/vibe.cpp)
target_include_directories(myteam_core PUBLIC include PRIVATE src)
target_link_libraries(myteam_core PUBLIC taskml::taskml)
target_compile_options(myteam_core PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(myteam_core PUBLIC
    MYTEAM_VERSION="${PROJECT_VERSION}"
    MYTEAM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# --- myorg: teams of teams, in step --------------------------------------------------
add_library(myorg_core STATIC
    src/org/spec.cpp
    src/org/engine.cpp
    src/org/watch.cpp)
target_include_directories(myorg_core PUBLIC include PRIVATE src)
target_link_libraries(myorg_core PUBLIC myteam_core)
target_compile_options(myorg_core PRIVATE -Wall -Wextra -Wpedantic)

# --- CLI -------------------------------------------------------------------------
add_executable(myteam apps/myteam.cpp)
target_link_libraries(myteam PRIVATE myteam_core)
target_compile_options(myteam PRIVATE -Wall -Wextra -Wpedantic)

add_executable(myorg apps/myorg.cpp)
target_link_libraries(myorg PRIVATE myorg_core)
target_compile_options(myorg PRIVATE -Wall -Wextra -Wpedantic)

# --- tests -----------------------------------------------------------------------
if(MYTEAM_BUILD_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()
    add_executable(myteam_tests
        tests/blockers_test.cpp
        tests/contracts_test.cpp
        tests/coach_test.cpp
        tests/context_test.cpp
        tests/feedback_test.cpp
        tests/mcp_test.cpp
        tests/guard_test.cpp
        tests/services_test.cpp
        tests/loop_test.cpp
        tests/plan_test.cpp
        tests/report_test.cpp
        tests/runners_test.cpp
        tests/team_test.cpp
        tests/yaml_test.cpp
        tests/org_test.cpp)
    target_link_libraries(myteam_tests PRIVATE myorg_core GTest::gtest GTest::gtest_main)
    target_compile_definitions(myteam_tests PRIVATE
        MYTEAM_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    include(GoogleTest)
    gtest_discover_tests(myteam_tests)
endif()

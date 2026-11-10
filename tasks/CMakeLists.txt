cmake_minimum_required(VERSION 3.15)

project(taskml
    VERSION 0.1.0
    DESCRIPTION "C++ implementation of the TaskML task markup language"
    HOMEPAGE_URL "https://taskml.dev"
    LANGUAGES CXX)

if(PROJECT_IS_TOP_LEVEL OR CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(TASKML_TOP_LEVEL ON)
else()
    set(TASKML_TOP_LEVEL OFF)
endif()

option(BUILD_SHARED_LIBS "Build taskml as a shared library" ON)
option(TASKML_BUILD_TESTS "Build the C++ test suite" ${TASKML_TOP_LEVEL})
option(TASKML_BUILD_CLI "Build the taskml command line tool" ${TASKML_TOP_LEVEL})
option(TASKML_BUILD_EXAMPLES "Build the examples" ${TASKML_TOP_LEVEL})
option(TASKML_BUILD_PYTHON "Build the Python bindings (requires pybind11)" OFF)
option(TASKML_INSTALL "Generate install rules" ${TASKML_TOP_LEVEL})

# When built by scikit-build-core (pip install .), only the Python module is
# needed; the core library is linked statically into it.
if(SKBUILD)
    set(TASKML_BUILD_PYTHON ON)
    set(BUILD_SHARED_LIBS OFF)
    set(TASKML_BUILD_TESTS OFF)
    set(TASKML_BUILD_CLI OFF)
    set(TASKML_BUILD_EXAMPLES OFF)
    set(TASKML_INSTALL OFF)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------

add_library(taskml
    src/json.cpp
    src/model.cpp
    src/parser.cpp
    src/serialize.cpp
    src/validate.cpp)
add_library(taskml::taskml ALIAS taskml)

target_compile_features(taskml PUBLIC cxx_std_17)
target_include_directories(taskml
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_definitions(taskml PRIVATE TASKML_BUILDING)
if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(taskml PUBLIC TASKML_STATIC)
endif()

set_target_properties(taskml PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME taskml)

if(MSVC)
    target_compile_options(taskml PRIVATE /W4 /utf-8)
else()
    target_compile_options(taskml PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()

# ---------------------------------------------------------------------------
# Tools, examples, tests, bindings
# ---------------------------------------------------------------------------

if(TASKML_BUILD_CLI)
    add_executable(taskml-cli tools/taskml_cli.cpp)
    target_link_libraries(taskml-cli PRIVATE taskml::taskml)
    set_target_properties(taskml-cli PROPERTIES OUTPUT_NAME taskml)
endif()

if(TASKML_BUILD_EXAMPLES)
    add_executable(taskml-example examples/basic.cpp)
    target_link_libraries(taskml-example PRIVATE taskml::taskml)
endif()

if(TASKML_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(TASKML_BUILD_PYTHON)
    add_subdirectory(python)
endif()

# ---------------------------------------------------------------------------
# Installation: headers, library, CMake package and pkg-config file
# ---------------------------------------------------------------------------

if(TASKML_INSTALL)
    include(CMakePackageConfigHelpers)

    install(TARGETS taskml
        EXPORT taskmlTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(DIRECTORY include/taskml DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    if(TASKML_BUILD_CLI)
        install(TARGETS taskml-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()

    set(TASKML_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/taskml)
    install(EXPORT taskmlTargets
        NAMESPACE taskml::
        DESTINATION ${TASKML_CMAKE_DIR})
    configure_package_config_file(cmake/taskmlConfig.cmake.in
        ${PROJECT_BINARY_DIR}/taskmlConfig.cmake
        INSTALL_DESTINATION ${TASKML_CMAKE_DIR})
    write_basic_package_version_file(${PROJECT_BINARY_DIR}/taskmlConfigVersion.cmake
        COMPATIBILITY SameMinorVersion)
    install(FILES
        ${PROJECT_BINARY_DIR}/taskmlConfig.cmake
        ${PROJECT_BINARY_DIR}/taskmlConfigVersion.cmake
        DESTINATION ${TASKML_CMAKE_DIR})

    if(BUILD_SHARED_LIBS)
        set(TASKML_PC_CFLAGS "")
    else()
        set(TASKML_PC_CFLAGS "-DTASKML_STATIC")
    endif()
    configure_file(cmake/taskml.pc.in ${PROJECT_BINARY_DIR}/taskml.pc @ONLY)
    install(FILES ${PROJECT_BINARY_DIR}/taskml.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/taskml)
endif()

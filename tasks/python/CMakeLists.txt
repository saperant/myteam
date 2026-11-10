find_package(Python 3.8 REQUIRED COMPONENTS Interpreter Development.Module)
find_package(pybind11 2.11 CONFIG REQUIRED)

pybind11_add_module(_taskml MODULE src/bindings.cpp)
target_link_libraries(_taskml PRIVATE taskml::taskml)
target_compile_features(_taskml PRIVATE cxx_std_17)

if(SKBUILD)
    install(TARGETS _taskml LIBRARY DESTINATION taskml)
else()
    # Development build: place the extension next to the Python sources so
    # `PYTHONPATH=python` works straight from the build tree.
    set_target_properties(_taskml PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/taskml)
endif()

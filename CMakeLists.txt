cmake_minimum_required(VERSION 3.15)

project(tev-cpp VERSION 1.1 LANGUAGES CXX)

# Set default build type to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Enable verbose build
set(CMAKE_VERBOSE_MAKEFILE ON)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Strict warnings and errors
add_compile_options(-Wall -Wextra -Werror -pedantic)

# Create shared library with major version as SO name
add_library(tev-cpp SHARED Tev.cpp)

set_target_properties(tev-cpp PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME "tev-cpp"
    PUBLIC_HEADER "Tev.h"
)

# Install rules
install(TARGETS tev-cpp
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    PUBLIC_HEADER DESTINATION include/tev-cpp
)

# The test executable
add_executable(tev-test test.cpp)

target_link_libraries(tev-test tev-cpp)

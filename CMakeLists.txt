cmake_minimum_required(VERSION 3.21)

project(SpaceLens
    VERSION 0.1.0
    DESCRIPTION "Native Windows disk-space analyzer"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(SPACELENS_BUILD_TESTS "Build unit tests" ON)

# ---------------------------------------------------------------------------
# Compiler warnings
# ---------------------------------------------------------------------------
if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Qt 6
# ---------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)

# ---------------------------------------------------------------------------
# Core library (no Qt dependency)
# ---------------------------------------------------------------------------
add_library(spacelens_core STATIC
    src/core/Types.hpp
    src/core/SizeFormatter.hpp
    src/core/SizeFormatter.cpp
)

target_include_directories(spacelens_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ---------------------------------------------------------------------------
# Application executable
# ---------------------------------------------------------------------------
add_executable(SpaceLens
    src/main.cpp
    src/app/Application.hpp
    src/app/Application.cpp
    src/ui/MainWindow.hpp
    src/ui/MainWindow.cpp
)

target_link_libraries(SpaceLens
    PRIVATE
        spacelens_core
        Qt6::Widgets
        Qt6::Concurrent
)

if(WIN32)
    set_target_properties(SpaceLens PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(SPACELENS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Convenience: copy Qt runtime next to the binary on Windows (optional later)
# ---------------------------------------------------------------------------
message(STATUS "SpaceLens ${PROJECT_VERSION}")
message(STATUS "  C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Qt6 dir: ${Qt6_DIR}")
message(STATUS "  Tests: ${SPACELENS_BUILD_TESTS}")

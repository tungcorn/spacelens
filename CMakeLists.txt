cmake_minimum_required(VERSION 3.21)

project(SpaceLens
    VERSION 0.1.0
    DESCRIPTION "Native Windows disk-space analyzer (core + CLI + GUI)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SPACELENS_BUILD_TESTS "Build unit tests" ON)
option(SPACELENS_BUILD_GUI "Build Qt GUI (SpaceLens.exe)" ON)
option(SPACELENS_BUILD_CLI "Build CLI (spacelens.exe)" ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# SQLite amalgamation (vendored) — C compilation unit for index storage
# ---------------------------------------------------------------------------
enable_language(C)
add_library(spacelens_sqlite STATIC
    third_party/sqlite/sqlite3.c
)
set_target_properties(spacelens_sqlite PROPERTIES LINKER_LANGUAGE C)
target_include_directories(spacelens_sqlite
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite
)
target_compile_definitions(spacelens_sqlite
    PUBLIC
        SQLITE_THREADSAFE=1
        SQLITE_DEFAULT_MEMSTATUS=0
        SQLITE_OMIT_LOAD_EXTENSION
        SQLITE_DQS=0
)
if(MSVC)
    # sqlite3.c is third-party; keep warnings quiet for the amalgamation only.
    target_compile_options(spacelens_sqlite PRIVATE /W2)
endif()

# ---------------------------------------------------------------------------
# Core library — no Qt
# ---------------------------------------------------------------------------
add_library(spacelens_core STATIC
    src/core/Types.hpp
    src/core/Json.hpp
    src/core/Json.cpp
    src/core/SizeFormatter.hpp
    src/core/SizeFormatter.cpp
    src/core/SizeParse.hpp
    src/core/SizeParse.cpp
    src/core/FileTime.hpp
    src/core/DirectoryTree.hpp
    src/core/DirectoryTree.cpp
    src/core/TopKCollector.hpp
    src/core/IFileEnumerator.hpp
    src/core/ScanTypes.hpp
    src/core/ScanEngine.hpp
    src/core/ScanEngine.cpp
    src/core/Query.hpp
    src/core/Classification.hpp
    src/core/Classification.cpp
    src/core/SafetyPolicy.hpp
    src/core/SafetyPolicy.cpp
    src/core/CleanupReview.hpp
    src/core/CleanupReview.cpp
    src/core/CleanupReviewStore.hpp
    src/core/CleanupReviewStore.cpp
    src/core/CleanupPlan.hpp
    src/core/CleanupPlan.cpp
    src/core/CleanupRevalidation.hpp
    src/core/CleanupRevalidation.cpp
    src/core/Maintenance.hpp
    src/core/Maintenance.cpp
    src/core/Duplicates.hpp
    src/core/Duplicates.cpp
    src/core/DuplicateDetection.hpp
    src/core/DuplicateDetection.cpp
    src/core/ReclaimAnalysis.hpp
    src/core/ReclaimAnalysis.cpp
    src/core/index/Sqlite.hpp
    src/core/index/Sqlite.cpp
    src/core/index/IndexPaths.hpp
    src/core/index/IndexPaths.cpp
    src/core/index/IndexStore.hpp
    src/core/index/IndexStore.cpp
    src/core/index/IndexBuilder.hpp
    src/core/index/IndexBuilder.cpp
    src/core/index/IndexQuery.hpp
    src/core/index/IndexQuery.cpp
    src/core/index/IndexRefresh.hpp
    src/core/index/IndexRefresh.cpp
    src/core/index/IndexCatalog.hpp
    src/core/index/IndexCatalog.cpp
    src/core/index/IndexOverview.hpp
    src/core/index/IndexOverview.cpp
    src/core/treemap/TreemapLayout.hpp
    src/core/treemap/TreemapLayout.cpp
    src/platform/windows/FindHandle.hpp
    src/platform/windows/VolumeHandle.hpp
    src/platform/windows/VolumeHandle.cpp
    src/platform/windows/FileIdentity.hpp
    src/platform/windows/FileIdentity.cpp
    src/platform/windows/CleanupMetadataReader.hpp
    src/platform/windows/CleanupMetadataReader.cpp
    src/platform/windows/FileContentHasher.hpp
    src/platform/windows/FileContentHasher.cpp
    src/platform/windows/UsnJournal.hpp
    src/platform/windows/UsnJournal.cpp
    src/platform/windows/WindowsFileEnumerator.hpp
    src/platform/windows/WindowsFileEnumerator.cpp
    src/platform/windows/ExplorerIntegration.hpp
    src/platform/windows/ExplorerIntegration.cpp
)

target_include_directories(spacelens_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(spacelens_core
    PUBLIC
        SPACELENS_VERSION_STRING="${PROJECT_VERSION}"
)

target_link_libraries(spacelens_core PUBLIC spacelens_sqlite)

if(WIN32)
    target_link_libraries(spacelens_core PUBLIC shell32 ole32 bcrypt)
endif()

# ---------------------------------------------------------------------------
# CLI — console subsystem, core only
# ---------------------------------------------------------------------------
if(SPACELENS_BUILD_CLI)
    set(SPACELENS_CLI_SOURCES
        src/cli/main.cpp
        src/cli/ExitCodes.hpp
        src/cli/Args.hpp
        src/cli/Args.cpp
        src/cli/Json.hpp
        src/cli/Json.cpp
        src/cli/Commands.hpp
        src/cli/Commands.cpp
    )

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/cli/main.cpp")
        add_executable(spacelens ${SPACELENS_CLI_SOURCES})
        target_link_libraries(spacelens PRIVATE spacelens_core)
        # Console subsystem. On Windows, keep CLI out of the same case-insensitive
        # path as the GUI (spacelens.exe vs SpaceLens.exe collide).
        set_target_properties(spacelens PROPERTIES
            OUTPUT_NAME "spacelens"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/cli"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/cli"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/cli"
        )
        if(WIN32)
            # Force console subsystem even if a global flag tries to set windows.
            target_link_options(spacelens PRIVATE "/SUBSYSTEM:CONSOLE")
            set_target_properties(spacelens PROPERTIES WIN32_EXECUTABLE FALSE)
        endif()
    else()
        message(WARNING "SPACELENS_BUILD_CLI=ON but src/cli/main.cpp missing; CLI target skipped")
    endif()
endif()

# ---------------------------------------------------------------------------
# GUI — optional Qt
# ---------------------------------------------------------------------------
if(SPACELENS_BUILD_GUI)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)

    find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)

    add_executable(SpaceLens
        src/main.cpp
        src/app/Application.hpp
        src/app/Application.cpp
        src/app/ScanSession.hpp
        src/app/ScanSession.cpp
        src/app/IndexSession.hpp
        src/app/IndexSession.cpp
        src/app/CleanupRevalidationSession.hpp
        src/app/CleanupRevalidationSession.cpp
        src/app/DuplicateDetectionSession.hpp
        src/app/DuplicateDetectionSession.cpp
        src/ui/MainWindow.hpp
        src/ui/MainWindow.cpp
        src/ui/IndexBrowserPage.hpp
        src/ui/IndexBrowserPage.cpp
        src/ui/TreemapWidget.hpp
        src/ui/TreemapWidget.cpp
        src/ui/CleanupReviewDialog.hpp
        src/ui/CleanupReviewDialog.cpp
        src/ui/DuplicateFilesDialog.hpp
        src/ui/DuplicateFilesDialog.cpp
    )

    target_link_libraries(SpaceLens
        PRIVATE
            spacelens_core
            Qt6::Widgets
            Qt6::Concurrent
    )

    # Windows file systems are case-insensitive: "SpaceLens.exe" and
    # "spacelens.exe" are the same path. Emit the GUI under a distinct name
    # and directory so the CLI is never overwritten.
    set_target_properties(SpaceLens PROPERTIES
        OUTPUT_NAME "spacelens-gui"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/gui"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/gui"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/gui"
    )
    if(WIN32)
        set_target_properties(SpaceLens PROPERTIES WIN32_EXECUTABLE TRUE)
        target_link_options(SpaceLens PRIVATE "/SUBSYSTEM:WINDOWS")
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests — core only (no Qt)
# ---------------------------------------------------------------------------
if(SPACELENS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "SpaceLens ${PROJECT_VERSION}")
message(STATUS "  C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  CLI:  ${SPACELENS_BUILD_CLI}")
message(STATUS "  GUI:  ${SPACELENS_BUILD_GUI}")
message(STATUS "  Tests:${SPACELENS_BUILD_TESTS}")
if(SPACELENS_BUILD_GUI)
    message(STATUS "  Qt6:  ${Qt6_DIR}")
endif()

set(SPACELENS_TEST_SOURCES
    main.cpp
    test_size_formatter.cpp
    test_size_parse.cpp
    test_directory_tree.cpp
    test_topk.cpp
    test_scan_engine.cpp
    test_windows_integration.cpp
    test_query.cpp
    test_classification.cpp
    test_safety_policy.cpp
    test_ordinary_locations.cpp
    test_cleanup_review.cpp
    test_cleanup_review_store.cpp
    test_cleanup_plan.cpp
    test_cleanup_revalidation.cpp
    test_maintenance.cpp
    test_duplicates.cpp
    test_hash_cache.cpp
    test_reclaim.cpp
    test_physical_storage.cpp
    test_reclaim_providers.cpp
    test_reclaim_intelligence.cpp
    test_index.cpp
    test_index_refresh.cpp
    test_usn_parity.cpp
    test_index_catalog.cpp
    test_index_catalog_list.cpp
    test_index_discovery.cpp
    test_treemap_layout.cpp
    test_index_overview.cpp
    test_storage_intelligence.cpp
    test_storage_analysis.cpp
    test_indexed_intelligence.cpp
    test_index_freshness.cpp
    test_index_breakdown.cpp
    test_mcp_json.cpp
    test_mcp_protocol.cpp
    TestRunner.hpp
    ${CMAKE_SOURCE_DIR}/src/mcp/JsonValue.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/Protocol.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/StorageTools.cpp
    FakeFileEnumerator.hpp
)

# CLI helper sources (no main) for unit tests — keep CLI free of Qt.
if(EXISTS "${CMAKE_SOURCE_DIR}/src/cli/Args.cpp")
    list(APPEND SPACELENS_TEST_SOURCES
        test_cli_args.cpp
        test_cli_json.cpp
        test_cli_readonly.cpp
        ${CMAKE_SOURCE_DIR}/src/cli/Args.cpp
        ${CMAKE_SOURCE_DIR}/src/cli/Json.cpp
    )
endif()

add_executable(spacelens_tests ${SPACELENS_TEST_SOURCES})

target_link_libraries(spacelens_tests
    PRIVATE
        spacelens_core
)

target_include_directories(spacelens_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

add_test(NAME spacelens_tests COMMAND spacelens_tests)

add_executable(spacelens_maintenance_tests
    main.cpp
    test_recycle_adapter.cpp
    TestRunner.hpp
)
target_link_libraries(spacelens_maintenance_tests
    PRIVATE
        spacelens_core
        spacelens_maintenance
)
target_include_directories(spacelens_maintenance_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME spacelens_maintenance_tests COMMAND spacelens_maintenance_tests)

if(SPACELENS_BUILD_GUI)
    set(SPACELENS_GUI_TEST_SOURCES
        main.cpp
        test_gui_ux.cpp
        TestRunner.hpp
    )
    foreach(src IN LISTS SPACELENS_GUI_SOURCES)
        list(APPEND SPACELENS_GUI_TEST_SOURCES "${CMAKE_SOURCE_DIR}/${src}")
    endforeach()

    add_executable(spacelens_gui_tests ${SPACELENS_GUI_TEST_SOURCES})
    target_link_libraries(spacelens_gui_tests
        PRIVATE
            spacelens_core
            spacelens_maintenance
            Qt6::Widgets
            Qt6::Concurrent
    )
    target_include_directories(spacelens_gui_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/src
    )
    set_target_properties(spacelens_gui_tests PROPERTIES
        AUTOMOC ON
        AUTORCC ON
        AUTOUIC ON
        WIN32_EXECUTABLE FALSE
    )
    if(WIN32)
        target_link_options(spacelens_gui_tests PRIVATE "/SUBSYSTEM:CONSOLE")
        target_link_options(spacelens_gui_tests PRIVATE "/INCREMENTAL:NO")
    endif()
    add_test(NAME spacelens_gui_tests COMMAND spacelens_gui_tests)
    # Shared Qt DLLs are not next to the test exe. Prepend the kit bin so
    # `ctest --preset windows-*` works without an ambient Qt PATH.
    if(WIN32 AND DEFINED Qt6_DIR)
        get_filename_component(_spacelens_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
        set(_spacelens_qt_bin "${_spacelens_qt_prefix}/bin")
        if(EXISTS "${_spacelens_qt_bin}/Qt6Core.dll")
            if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.22")
                set_property(TEST spacelens_gui_tests APPEND PROPERTY
                    ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:${_spacelens_qt_bin}")
            else()
                file(TO_NATIVE_PATH "${_spacelens_qt_bin}" _spacelens_qt_bin_native)
                set(_spacelens_gui_test_path
                    "${_spacelens_qt_bin_native};$ENV{PATH}")
                string(REPLACE ";" "\\;" _spacelens_gui_test_path
                       "${_spacelens_gui_test_path}")
                set_tests_properties(spacelens_gui_tests PROPERTIES
                    ENVIRONMENT "PATH=${_spacelens_gui_test_path}")
            endif()
        endif()
    endif()
endif()

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
    test_cleanup_review.cpp
    test_cleanup_review_store.cpp
    test_cleanup_plan.cpp
    test_cleanup_revalidation.cpp
    test_maintenance.cpp
    test_duplicates.cpp
    test_reclaim.cpp
    test_index.cpp
    test_index_refresh.cpp
    test_usn_parity.cpp
    test_index_catalog.cpp
    test_index_discovery.cpp
    test_treemap_layout.cpp
    test_index_overview.cpp
    TestRunner.hpp
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

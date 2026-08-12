add_executable(spacelens_tests
    main.cpp
    test_size_formatter.cpp
    TestRunner.hpp
)

target_link_libraries(spacelens_tests
    PRIVATE
        spacelens_core
)

target_include_directories(spacelens_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

add_test(NAME spacelens_tests COMMAND spacelens_tests)

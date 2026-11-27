cmake_minimum_required(VERSION 3.22)
project(strata
        VERSION 0.1.0
        DESCRIPTION "Strata: a SQL terminal over swappable storage engines"
        LANGUAGES CXX
        )

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_FLAGS "-Wall -Wextra")
set(CMAKE_CXX_FLAGS_DEBUG "-g")
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

find_package(Threads REQUIRED)

# Domain, Application, Infrastructure, and the SQL parser (Adapter), minus
# main(), as one library so the test executable links it without recompiling
# or depending on cli.cpp.
add_library(strata_core STATIC
        src/application.cpp
        src/binder.cpp
        src/bootstrap.cpp
        src/csv_codec.cpp
        src/csv_file_source.cpp
        src/directory_lock.cpp
        src/expression.cpp
        src/flat_file_engine.cpp
        src/flat_file_executor.cpp
        src/import_inference.cpp
        src/infrastructure.cpp
        src/paged/btree.cpp
        src/paged/btree_node.cpp
        src/paged/buffer_manager.cpp
        src/paged/catalog.cpp
        src/paged/crc32c.cpp
        src/paged/database.cpp
        src/paged/heap_access.cpp
        src/paged/key_range.cpp
        src/paged/mutation_planner.cpp
        src/paged/operators.cpp
        src/paged/page_allocator.cpp
        src/paged/page_store.cpp
        src/paged/paged_engine.cpp
        src/paged/replacement_policy.cpp
        src/paged/row_keys.cpp
        src/paged/slotted_page.cpp
        src/paged/table_heap.cpp
        src/paged/tuple_codec.cpp
        src/repl.cpp
        src/scalar_functions.cpp
        src/sql_parser.cpp
        src/statement_buffer.cpp
        src/value.cpp
        )
target_include_directories(strata_core PUBLIC include)
target_compile_features(strata_core PUBLIC cxx_std_20)
target_link_libraries(strata_core PUBLIC Threads::Threads)

add_executable(strata_cli src/cli.cpp)
target_link_libraries(strata_cli PRIVATE strata_core)

enable_testing()
add_executable(strata_tests
        tests/test_main.cpp
        tests/test_support.cpp
        tests/test_binder.cpp
        tests/test_bootstrap.cpp
        tests/test_cli.cpp
        tests/test_csv_codec.cpp
        tests/test_csv_repository.cpp
        tests/test_directory_lock.cpp
        tests/test_engine_command.cpp
        tests/test_equivalence.cpp
        tests/test_equivalence_generator.cpp
        tests/test_equivalence_support.cpp
        tests/test_expression.cpp
        tests/test_flat_file_engine.cpp
        tests/test_import.cpp
        tests/test_layering.cpp
        tests/test_paged_allocator.cpp
        tests/test_paged_btree.cpp
        tests/test_paged_btree_node.cpp
        tests/test_paged_buffer.cpp
        tests/test_paged_catalog.cpp
        tests/test_paged_database.cpp
        tests/test_paged_ddl.cpp
        tests/test_paged_engine.cpp
        tests/test_paged_environment.cpp
        tests/test_paged_execution_environment.cpp
        tests/test_paged_bytes.cpp
        tests/test_paged_operators.cpp
        tests/test_paged_planner.cpp
        tests/test_paged_policy.cpp
        tests/test_paged_slotted_page.cpp
        tests/test_paged_table_heap.cpp
        tests/test_query_service.cpp
        tests/test_query_service_lock.cpp
        tests/test_repl.cpp
        tests/test_scale_data.cpp
        tests/test_scale_data_support.cpp
        tests/test_sql_parser.cpp
        tests/test_statement_buffer.cpp
        )
target_include_directories(strata_tests PRIVATE tests)
target_link_libraries(strata_tests PRIVATE strata_core)
target_compile_definitions(strata_tests PRIVATE
        STRATA_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        STRATA_CLI_PATH="$<TARGET_FILE:strata_cli>")
add_dependencies(strata_tests strata_cli)
add_test(NAME strata_tests COMMAND strata_tests)

# The SQLite differential harness is Python and is not built. It is registered
# only when an interpreter that can run it (3.10 or later) is found.
find_package(Python3 3.10 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME differential
             COMMAND ${Python3_EXECUTABLE} -I tests/differential/differential.py
                     --cli $<TARGET_FILE:strata_cli>
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME differential_unit
             COMMAND ${Python3_EXECUTABLE} -I -m unittest discover
                     -s tests/differential -p "test_*.py"
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(differential differential_unit PROPERTIES
            ENVIRONMENT "STRATA_CLI_PATH=$<TARGET_FILE:strata_cli>")
else()
    message(STATUS "Python3 not found: the differential tests are not registered")
endif()

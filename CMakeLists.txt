cmake_minimum_required(VERSION 3.20)
project(sqlite_rtree_bulk_load C)

include(CTest)
include(GNUInstallDirs)
find_package(SQLite3 3.31 REQUIRED)
find_path(SQLITE3_EXTENSION_INCLUDE_DIR sqlite3ext.h
    HINTS ${SQLite3_INCLUDE_DIRS} ${SQLite3_INCLUDE_DIR})

if(NOT SQLITE3_EXTENSION_INCLUDE_DIR)
    message(FATAL_ERROR "sqlite3ext.h was not found")
endif()

add_library(rtreebulkload MODULE
    src/extension.c
    src/bulk_load.c
    src/str_packer.c
    src/rtree_shadow_writer.c
    src/rstar_vendor.c)
target_include_directories(rtreebulkload PRIVATE ${SQLITE3_EXTENSION_INCLUDE_DIR})
target_compile_features(rtreebulkload PRIVATE c_std_99)
set_target_properties(rtreebulkload PROPERTIES
    PREFIX ""
    OUTPUT_NAME "rtreebulkload"
    C_VISIBILITY_PRESET hidden)
if(WIN32)
    set_target_properties(rtreebulkload PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
else()
    target_link_libraries(rtreebulkload PRIVATE m)
endif()
if(MSVC)
    target_compile_options(rtreebulkload PRIVATE /W4)
else()
    target_compile_options(rtreebulkload PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    add_executable(rtreebulkload_test tests/test_extension.c)
    target_link_libraries(rtreebulkload_test PRIVATE SQLite::SQLite3)
    target_compile_features(rtreebulkload_test PRIVATE c_std_99)
    if(MSVC)
        target_compile_options(rtreebulkload_test PRIVATE /W4)
    else()
        target_compile_options(rtreebulkload_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME rtreebulkload_reentrancy
        COMMAND rtreebulkload_test $<TARGET_FILE:rtreebulkload>)
endif()

install(TARGETS rtreebulkload
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
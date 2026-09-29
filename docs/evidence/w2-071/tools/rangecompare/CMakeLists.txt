# w2-071 D2/D3 evidence: SqlManager before (git 997042d, copied into BEFORE_SRC by
# build-and-compare.bat) and after (work tree Core/) - same tool / same test source, two builds.
cmake_minimum_required(VERSION 3.21)
project(w2071_rangecompare LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Sql Test)
qt_standard_project_setup()

get_filename_component(TAIDAFLOW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(AFTER_SRC "${TAIDAFLOW_ROOT}/Core")
file(TO_CMAKE_PATH "${BEFORE_SRC}" BEFORE_SRC)
get_filename_component(BEFORE_SRC "${BEFORE_SRC}" ABSOLUTE)
if(NOT BEFORE_SRC OR NOT EXISTS "${BEFORE_SRC}/SqlManager.cpp")
    message(FATAL_ERROR "BEFORE_SRC (folder with SqlManager.h/.cpp of git 997042d) not set - use build-and-compare.bat")
endif()

function(w2071_exe name srcdir)
    qt_add_executable(${name} ${ARGN} "${srcdir}/SqlManager.h" "${srcdir}/SqlManager.cpp")
    target_include_directories(${name} PRIVATE "${srcdir}")
    target_link_libraries(${name} PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
    set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE OFF)
    if(MSVC)
        target_compile_options(${name} PRIVATE /utf-8)
    endif()
endfunction()

w2071_exe(range_dump_before "${BEFORE_SRC}" range_dump.cpp)
w2071_exe(range_dump_after "${AFTER_SRC}" range_dump.cpp)
# The Core/tests schema test (public SqlManager API only) against the old SqlManager: expected to
# FAIL there (schema_runs > 1, "Schema file not found" more than once) - shows the test detects D-002.
w2071_exe(tst_sqlmanager_schema_once_before "${BEFORE_SRC}" "${AFTER_SRC}/tests/tst_sqlmanager_schema_once.cpp")

# w2-045 QTest harness + measurement program for the stepped History range request.
# Compiles the REAL taidaflow Core/SqlManager.cpp (same project, not a copy).
#
#   tst_w2045_history  (CTest)  correctness against an independent OFFSET reference,
#                               keyset/anchor rules, step bound, superseded requests.
#                               Data: a fresh copy of build/w2-041-bench/data per run
#                               (made by docs/evidence/w2-041/tools/make_bench_db.py).
#   w2045_bench        (not a test) D3 measurements on build/w2-045-bench/data
#                               (docs/evidence/w2-045/tools/make_bench_db.py).
#
#   ..\run-qtest.bat   (vcvars64 + configure + build + ctest, Release)
cmake_minimum_required(VERSION 3.21)
project(w2045_history_qtest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Core Sql Test)
qt_standard_project_setup()

get_filename_component(TAIDAFLOW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(CORE_DIR "${TAIDAFLOW_ROOT}/Core")

qt_add_executable(tst_w2045_history
    tst_w2045_history.cpp
    "${CORE_DIR}/SqlManager.h"
    "${CORE_DIR}/SqlManager.cpp"
)
target_include_directories(tst_w2045_history PRIVATE "${CORE_DIR}")
target_link_libraries(tst_w2045_history PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
target_compile_definitions(tst_w2045_history PRIVATE
    W2045_SOURCE_DATA_DIR="${TAIDAFLOW_ROOT}/build/w2-041-bench/data"
    W2045_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/work")
set_target_properties(tst_w2045_history PROPERTIES WIN32_EXECUTABLE OFF)

qt_add_executable(w2045_bench
    w2045_bench.cpp
    "${CORE_DIR}/SqlManager.h"
    "${CORE_DIR}/SqlManager.cpp"
)
target_include_directories(w2045_bench PRIVATE "${CORE_DIR}")
target_link_libraries(w2045_bench PRIVATE Qt6::Core Qt6::Sql)
target_compile_definitions(w2045_bench PRIVATE
    W2045_BENCH_DATA_DIR="${TAIDAFLOW_ROOT}/build/w2-045-bench/data"
    W2045_BENCH_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/bench-work")
set_target_properties(w2045_bench PROPERTIES WIN32_EXECUTABLE OFF)

# Before/after comparison: the same program on the pre-w2-045 SqlManager (HEAD 8e87b70), only when
# build/w2-045-old/Core holds that copy (git show HEAD:Core/SqlManager.{h,cpp}).
set(OLD_CORE_DIR "${TAIDAFLOW_ROOT}/build/w2-045-old/Core")
if(EXISTS "${OLD_CORE_DIR}/SqlManager.cpp")
    qt_add_executable(w2045_bench_old
        w2045_bench.cpp
        "${OLD_CORE_DIR}/SqlManager.h"
        "${OLD_CORE_DIR}/SqlManager.cpp"
    )
    target_include_directories(w2045_bench_old PRIVATE "${OLD_CORE_DIR}")
    target_link_libraries(w2045_bench_old PRIVATE Qt6::Core Qt6::Sql)
    target_compile_definitions(w2045_bench_old PRIVATE W2045_OLD
        W2045_BENCH_DATA_DIR="${TAIDAFLOW_ROOT}/build/w2-045-bench/data"
        W2045_BENCH_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/bench-work-old")
    set_target_properties(w2045_bench_old PROPERTIES WIN32_EXECUTABLE OFF)
endif()

enable_testing()
add_test(NAME tst_w2045_history COMMAND tst_w2045_history)
# PATH (Qt bin) and QT_FORCE_STDERR_LOGGING=1 come from run-qtest.bat.
set_tests_properties(tst_w2045_history PROPERTIES TIMEOUT 1800)

# w2-039 QTest harness for the History query changes in taidaflow/Core/SqlManager.
# Compiles the real Core/SqlManager.cpp (same taidaflow project, not a copy) and runs it
# against the 30-day DB made by ../history_query_bench.py
# (taidaflow/build/w2-039-bench/data/sensor_202609.sqlite).
#
#   cmake -S docs/evidence/w2-039/tools/qtest -B build/w2-039-qtest -G Ninja
#         -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/w2-039-qtest
#   ctest --test-dir build/w2-039-qtest -V
cmake_minimum_required(VERSION 3.21)
project(w2039_history_qtest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Core Sql Test)
qt_standard_project_setup()

get_filename_component(TAIDAFLOW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(CORE_DIR "${TAIDAFLOW_ROOT}/Core")

qt_add_executable(tst_w2039_history
    tst_w2039_history.cpp
    "${CORE_DIR}/SqlManager.h"
    "${CORE_DIR}/SqlManager.cpp"
)
target_include_directories(tst_w2039_history PRIVATE "${CORE_DIR}")
target_link_libraries(tst_w2039_history PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
target_compile_definitions(tst_w2039_history PRIVATE
    W2039_BENCH_DATA_DIR="${TAIDAFLOW_ROOT}/build/w2-039-bench/data"
    W2039_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}")
set_target_properties(tst_w2039_history PROPERTIES WIN32_EXECUTABLE OFF)

enable_testing()
add_test(NAME tst_w2039_history COMMAND tst_w2039_history)
# Qt DLLs on PATH and QT_FORCE_STDERR_LOGGING=1 come from run-qtest.bat (without a console
# the Windows plain-text QTest log otherwise goes to OutputDebugString, not to ctest).
set_tests_properties(tst_w2039_history PROPERTIES TIMEOUT 600)

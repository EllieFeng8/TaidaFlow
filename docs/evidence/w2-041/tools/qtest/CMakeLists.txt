# w2-041 QTest harness: History range paging across months + CSV export queue/engine,
# cleanup and download service.  Compiles the REAL taidaflow sources (Core/SqlManager.cpp,
# Core/HistoryExport.cpp, Core/TaidaFlowProxy.h) - same project, not copies - and runs them
# against the 30-day two-month DB made by ../make_bench_db.py
# (taidaflow/build/w2-041-bench/data).
#
#   ..\run-qtest.bat   (vcvars64 + configure + build + ctest, Release)
cmake_minimum_required(VERSION 3.21)
project(w2041_export_qtest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Qml Network Sql HttpServer Test)
qt_standard_project_setup()

get_filename_component(TAIDAFLOW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(CORE_DIR "${TAIDAFLOW_ROOT}/Core")

qt_add_executable(tst_w2041_export
    tst_w2041_export.cpp
    "${CORE_DIR}/SqlManager.h"
    "${CORE_DIR}/SqlManager.cpp"
    "${CORE_DIR}/HistoryExport.h"
    "${CORE_DIR}/HistoryExport.cpp"
    "${CORE_DIR}/TaidaFlowProxy.h"
)
target_include_directories(tst_w2041_export PRIVATE "${CORE_DIR}")
target_link_libraries(tst_w2041_export PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Network Qt6::Sql Qt6::HttpServer Qt6::Test psapi)
target_compile_definitions(tst_w2041_export PRIVATE
    W2041_BENCH_DATA_DIR="${TAIDAFLOW_ROOT}/build/w2-041-bench/data"
    W2041_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/work")
set_target_properties(tst_w2041_export PROPERTIES WIN32_EXECUTABLE OFF)

enable_testing()
add_test(NAME tst_w2041_export COMMAND tst_w2041_export)
# PATH (Qt bin), QT_QPA_PLATFORM=offscreen and QT_FORCE_STDERR_LOGGING=1 come from run-qtest.bat.
set_tests_properties(tst_w2041_export PROPERTIES TIMEOUT 1800)

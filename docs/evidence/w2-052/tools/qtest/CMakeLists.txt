# w2-052 QTest harness: one History view per client (spec §2.1).
# Compiles the REAL taidaflow sources (same project, not copies):
#   tst_w2052_views      Core/HistoryViews.cpp (HistoryViewService, created by Core::init),
#                        Core/SqlManager.cpp, Core/HistoryExport.cpp (+ AppHttpServer it links),
#                        Core/TaidaFlowProxy.h: sessions, revision, idle removal, entry limit,
#                        invalid input, unbounded range, interleaved == single-session entries.
#   tst_w2052_rangepage  Core/SqlManager.cpp: the w2-045 correctness test (every page equal to
#                        the OFFSET / UNION ALL references, keyset, steps, snapshots, cache)
#                        through the per-session API, plus the session rules.
# Data: fresh copies of build/w2-041-bench/data (docs/evidence/w2-041/tools/make_bench_db.py).
#
#   ..\run-qtest.bat   (vcvars64 + configure + build + ctest, Release)
cmake_minimum_required(VERSION 3.21)
project(w2052_history_views_qtest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Qml Network Sql HttpServer Test)
qt_standard_project_setup()

get_filename_component(TAIDAFLOW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(CORE_DIR "${TAIDAFLOW_ROOT}/Core")

qt_add_executable(tst_w2052_views
    tst_w2052_views.cpp
    "${CORE_DIR}/HistoryViews.h"
    "${CORE_DIR}/HistoryViews.cpp"
    "${CORE_DIR}/SqlManager.h"
    "${CORE_DIR}/SqlManager.cpp"
    "${CORE_DIR}/HistoryExport.h"
    "${CORE_DIR}/HistoryExport.cpp"
    "${CORE_DIR}/TaidaFlowProxy.h"
    "${CORE_DIR}/AppHttpServer/AppHttpServer.h"
    "${CORE_DIR}/AppHttpServer/AppHttpServer.cpp"
)
target_include_directories(tst_w2052_views PRIVATE "${CORE_DIR}")
target_link_libraries(tst_w2052_views PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Network Qt6::Sql Qt6::HttpServer Qt6::Test)
target_compile_definitions(tst_w2052_views PRIVATE
    W2052_SOURCE_DATA_DIR="${TAIDAFLOW_ROOT}/build/w2-041-bench/data"
    W2052_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/work-views")
if(MSVC)
    target_compile_options(tst_w2052_views PRIVATE /utf-8)
endif()
set_target_properties(tst_w2052_views PROPERTIES WIN32_EXECUTABLE OFF)

qt_add_executable(tst_w2052_rangepage
    tst_w2052_rangepage.cpp
    "${CORE_DIR}/SqlManager.h"
    "${CORE_DIR}/SqlManager.cpp"
)
target_include_directories(tst_w2052_rangepage PRIVATE "${CORE_DIR}")
target_link_libraries(tst_w2052_rangepage PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
target_compile_definitions(tst_w2052_rangepage PRIVATE
    W2045_SOURCE_DATA_DIR="${TAIDAFLOW_ROOT}/build/w2-041-bench/data"
    W2045_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/work-rangepage")
set_target_properties(tst_w2052_rangepage PROPERTIES WIN32_EXECUTABLE OFF)

enable_testing()
add_test(NAME tst_w2052_views COMMAND tst_w2052_views)
add_test(NAME tst_w2052_rangepage COMMAND tst_w2052_rangepage)
# PATH (Qt bin), QT_QPA_PLATFORM=offscreen and QT_FORCE_STDERR_LOGGING=1 come from run-qtest.bat.
set_tests_properties(tst_w2052_views tst_w2052_rangepage PROPERTIES TIMEOUT 1800)

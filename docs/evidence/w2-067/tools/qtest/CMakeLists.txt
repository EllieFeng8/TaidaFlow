# w2-067 QTest: SqlManager::shutdown() (orderly stop at application exit, called by Core::shutdown).
# Compiles the REAL taidaflow Core source SqlManager.cpp (same project, not a copy). Uses a temporary
# data folder under the build tree; no device, no network port.
#
#   ..\run-qtest.bat   (vcvars64 + configure + build + ctest, Release)
cmake_minimum_required(VERSION 3.21)
project(w2067_shutdown_qtest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Core Sql Test)
qt_standard_project_setup()

get_filename_component(TAIDAFLOW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(CORE_DIR "${TAIDAFLOW_ROOT}/Core")

qt_add_executable(tst_w2067_sqlmanager_shutdown
    tst_w2067_sqlmanager_shutdown.cpp
    "${CORE_DIR}/SqlManager.h"
    "${CORE_DIR}/SqlManager.cpp"
)
target_include_directories(tst_w2067_sqlmanager_shutdown PRIVATE "${CORE_DIR}")
target_link_libraries(tst_w2067_sqlmanager_shutdown PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
target_compile_definitions(tst_w2067_sqlmanager_shutdown PRIVATE
    W2067_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/work")
if(MSVC)
    target_compile_options(tst_w2067_sqlmanager_shutdown PRIVATE /utf-8)
endif()
set_target_properties(tst_w2067_sqlmanager_shutdown PROPERTIES WIN32_EXECUTABLE OFF)

enable_testing()
add_test(NAME tst_w2067_sqlmanager_shutdown COMMAND tst_w2067_sqlmanager_shutdown)
set_tests_properties(tst_w2067_sqlmanager_shutdown PROPERTIES TIMEOUT 120)

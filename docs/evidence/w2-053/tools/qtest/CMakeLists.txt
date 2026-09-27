# w2-053 QTest harness: DI alarm hand-over after a restart.
# Compiles the REAL taidaflow Core sources (same project, not copies): manager.cpp,
# SqlManager.cpp, Modbus_Client.cpp, Ms300FaultReader.cpp and the header-only
# TaidaFlowProxy.h.  Each test uses a temporary data folder under the build tree;
# no device is contacted (no Manager::start(), DI samples are fed through the real
# ModbusClient::registersRead signal -> Manager::mirrorClientData ->
# Manager::checkDigitalInputAlarm path).
#
#   ..\run-qtest.bat   (vcvars64 + configure + build + ctest, Release)
cmake_minimum_required(VERSION 3.21)
project(w2053_di_restart_qtest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Qml Sql SerialBus SerialPort Test)
qt_standard_project_setup()

get_filename_component(TAIDAFLOW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(CORE_DIR "${TAIDAFLOW_ROOT}/Core")

qt_add_executable(tst_w2053_di_restart
    tst_w2053_di_restart.cpp
    "${CORE_DIR}/manager.h"
    "${CORE_DIR}/manager.cpp"
    "${CORE_DIR}/SqlManager.h"
    "${CORE_DIR}/SqlManager.cpp"
    "${CORE_DIR}/Modbus_Client.h"
    "${CORE_DIR}/Modbus_Client.cpp"
    "${CORE_DIR}/Ms300FaultReader.h"
    "${CORE_DIR}/Ms300FaultReader.cpp"
    "${CORE_DIR}/TaidaFlowProxy.h"
    "${CORE_DIR}/ModbusMapping.h"
    "${CORE_DIR}/ModbusServerBridgeMapping.h"
)
target_include_directories(tst_w2053_di_restart PRIVATE "${CORE_DIR}")
target_link_libraries(tst_w2053_di_restart PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Sql Qt6::SerialBus Qt6::SerialPort Qt6::Test)
target_compile_definitions(tst_w2053_di_restart PRIVATE
    W2053_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/work")
if(MSVC)
    target_compile_options(tst_w2053_di_restart PRIVATE /utf-8)
endif()
set_target_properties(tst_w2053_di_restart PROPERTIES WIN32_EXECUTABLE OFF)

enable_testing()
add_test(NAME tst_w2053_di_restart COMMAND tst_w2053_di_restart)
# PATH (Qt bin), QT_QPA_PLATFORM=offscreen and QT_FORCE_STDERR_LOGGING=1 come from run-qtest.bat.
set_tests_properties(tst_w2053_di_restart PROPERTIES TIMEOUT 600)

# Core unit tests (QTest + CTest). Separate small CTest project, like App/tests, so the
# application's Qt Design Studio CMake is not touched. Each test compiles the application's own
# Core source files (same folder tree, nothing copied) and runs without devices, without a
# fixed network port (REST tests listen on a free port picked by the OS) and with its data in a
# temporary folder. See README.md (section on the Core unit tests) and run-core-tests.bat.
#
#   w2-071: tst_rest_range_paging, tst_sqlmanager_schema_once, tst_historyexport_status
#   w2-072: tst_modbusclient_offline, tst_ms300_offline, tst_manager_sv_guard
#   w2-080: tst_alarm_views
#   w2-084: tst_server_heartbeat
#   w2-085: tst_limit_alarms (and LimitAlarms.cpp in tst_manager_sv_guard: Manager owns the monitor)
cmake_minimum_required(VERSION 3.21)
project(TaidaFlowCoreTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Qml Network Sql HttpServer Test)
qt_standard_project_setup()
enable_testing()

get_filename_component(CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
# Qt DLLs for ctest runs (no PATH setup needed).
set(QT_BIN_DIR "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")

function(taidaflow_core_test name)
    cmake_parse_arguments(T "" "" "SOURCES;LIBS" ${ARGN})
    qt_add_executable(${name} ${T_SOURCES})
    set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
    target_include_directories(${name} PRIVATE "${CORE_DIR}")
    target_link_libraries(${name} PRIVATE Qt6::Core Qt6::Test ${T_LIBS})
    if(MSVC)
        target_compile_options(${name} PRIVATE /utf-8)
    endif()
    # Plain-text result per test in the build folder + the same on stdout.
    add_test(NAME ${name} COMMAND ${name} -o "${CMAKE_CURRENT_BINARY_DIR}/${name}.result.txt,txt" -o "-,txt")
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIR};QT_QPA_PLATFORM=set:offscreen;QT_FORCE_STDERR_LOGGING=set:1"
        TIMEOUT 300)
endfunction()

# w2-071 D1/D2 (review D-001): the REST range routes are paged; page/pageSize > INT_MAX and
# from/to outside 0 .. 9999-12-31 are refused (400) at once; month files are listed, not walked.
taidaflow_core_test(tst_rest_range_paging
    SOURCES
        tst_rest_range_paging.cpp
        "${CORE_DIR}/RESTManager.h"
        "${CORE_DIR}/RESTManager.cpp"
        "${CORE_DIR}/SqlManager.h"
        "${CORE_DIR}/SqlManager.cpp"
    LIBS Qt6::Network Qt6::Sql Qt6::HttpServer)

# w2-071 D3 (review D-002): the data schema runs once per month connection; "Schema file not
# found" is logged at most once per run.
taidaflow_core_test(tst_sqlmanager_schema_once
    SOURCES
        tst_sqlmanager_schema_once.cpp
        "${CORE_DIR}/SqlManager.h"
        "${CORE_DIR}/SqlManager.cpp"
    LIBS Qt6::Sql)

# w2-071 D4 (review D-007): HistoryExportManager status map limits.
taidaflow_core_test(tst_historyexport_status
    SOURCES
        tst_historyexport_status.cpp
        "${CORE_DIR}/HistoryExport.h"
        "${CORE_DIR}/HistoryExport.cpp"
        "${CORE_DIR}/SqlManager.h"
        "${CORE_DIR}/SqlManager.cpp"
        "${CORE_DIR}/TaidaFlowProxy.h"
        "${CORE_DIR}/AppHttpServer/AppHttpServer.h"
        "${CORE_DIR}/AppHttpServer/AppHttpServer.cpp"
    LIBS Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Network Qt6::Sql Qt6::HttpServer)

# ---------------------------------------------------------------------------------------------
# w2-072 (review D-003 / I-003): Modbus client / MS300 while offline, NaN/Inf set values.
# ---------------------------------------------------------------------------------------------
find_package(Qt6 6.8 REQUIRED COMPONENTS SerialBus SerialPort)

# D1/D2: read() reports "not connected" once per outage, reconnects only every 3 s, connection
# warnings rate limited (127.0.0.1 only: a refused port and a QTcpServer of the test).
taidaflow_core_test(tst_modbusclient_offline
    SOURCES
        tst_modbusclient_offline.cpp
        "${CORE_DIR}/Modbus_Client.h"
        "${CORE_DIR}/Modbus_Client.cpp"
    LIBS Qt6::Network Qt6::SerialBus)

# D3: MS300 with a serial port that does not exist: one connection attempt per 3 s, warnings
# rate limited.
taidaflow_core_test(tst_ms300_offline
    SOURCES
        tst_ms300_offline.cpp
        "${CORE_DIR}/Ms300FaultReader.h"
        "${CORE_DIR}/Ms300FaultReader.cpp"
        "${CORE_DIR}/Modbus_Client.h"
        "${CORE_DIR}/Modbus_Client.cpp"   # (RepeatedWarningLimiter lives in Modbus_Client.h)
    LIBS Qt6::Network Qt6::SerialBus Qt6::SerialPort)

# D4/D5c: Manager refuses NaN/+Inf/-Inf set values (no Modbus write, no VFD start, SV put back);
# 30.0 is written. Modbus TCP server of the test on 127.0.0.1.
taidaflow_core_test(tst_manager_sv_guard
    SOURCES
        tst_manager_sv_guard.cpp
        "${CORE_DIR}/manager.h"
        "${CORE_DIR}/LimitAlarms.h"
        "${CORE_DIR}/LimitAlarms.cpp"   # w2-085: owned by Manager
        "${CORE_DIR}/manager.cpp"
        "${CORE_DIR}/Modbus_Client.h"
        "${CORE_DIR}/Modbus_Client.cpp"
        "${CORE_DIR}/Ms300FaultReader.h"
        "${CORE_DIR}/Ms300FaultReader.cpp"
        "${CORE_DIR}/ModbusMapping.h"
        "${CORE_DIR}/ModbusServerBridgeMapping.h"
        "${CORE_DIR}/SqlManager.h"
        "${CORE_DIR}/SqlManager.cpp"
        "${CORE_DIR}/TaidaFlowProxy.h"
    LIBS Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Network Qt6::Sql Qt6::SerialBus Qt6::SerialPort)

# ---------------------------------------------------------------------------------------------
# main (f05e0a4, elliefeng): sensor settings contract of TaidaFlowProxy (sensorSettingsSv /
# pressureUnitSv) and the settings page preview (Preview.qml). Merged into core's test project.
# ---------------------------------------------------------------------------------------------
find_package(Qt6 6.8 REQUIRED COMPONENTS Quick)
qt_add_executable(tst_sensor_settings tst_sensor_settings.cpp ../TaidaFlowProxy.h)
set_target_properties(tst_sensor_settings PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
target_link_libraries(tst_sensor_settings PRIVATE Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::Test)
target_compile_definitions(tst_sensor_settings PRIVATE SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../..")
if(MSVC)
    target_compile_options(tst_sensor_settings PRIVATE /utf-8)
endif()
add_test(NAME sensor_settings COMMAND tst_sensor_settings)
set_tests_properties(sensor_settings PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIR};QT_QPA_PLATFORM=set:offscreen;QT_QUICK_BACKEND=set:software;QT_QUICK_CONTROLS_STYLE=set:Universal"
    TIMEOUT 300)

# ---------------------------------------------------------------------------------------------
# w2-080: per-client alarm views (AlarmViewService -> TaidaFlowProxy::alarmViews): ranges across
# month files, order / paging / counts, rows = alarmRecords records, live updates on new and
# resolved alarms, sessions independent, invalid input, idle / 32-entry cleanup (injected clock).
# Real SqlManager on a temporary data folder; no device, no port.
# ---------------------------------------------------------------------------------------------
taidaflow_core_test(tst_alarm_views
    SOURCES
        tst_alarm_views.cpp
        "${CORE_DIR}/AlarmViews.h"
        "${CORE_DIR}/AlarmViews.cpp"
        "${CORE_DIR}/AlarmRecordFormat.h"
        "${CORE_DIR}/AlarmRecordFormat.cpp"
        "${CORE_DIR}/SqlManager.h"
        "${CORE_DIR}/SqlManager.cpp"
        "${CORE_DIR}/SqlManagerAlarms.cpp"
        "${CORE_DIR}/HistoryExport.h"
        "${CORE_DIR}/HistoryExport.cpp"
        "${CORE_DIR}/TaidaFlowProxy.h"
        "${CORE_DIR}/AppHttpServer/AppHttpServer.h"
        "${CORE_DIR}/AppHttpServer/AppHttpServer.cpp"
    LIBS Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Network Qt6::Sql Qt6::HttpServer)

# ---------------------------------------------------------------------------------------------
# w2-084: the Core's server heartbeat (ServerHeartbeat -> TaidaFlowProxy::serverHeartbeatMs every
# 1 s on the main thread; stopped first in Core::shutdown). Real timer (1 s) and an injected clock
# with a short interval; the wiring in core.cpp / Core/CMakeLists.txt is read from the sources.
# No device, no port, no database.
# ---------------------------------------------------------------------------------------------
taidaflow_core_test(tst_server_heartbeat
    SOURCES
        tst_server_heartbeat.cpp
        "${CORE_DIR}/ServerHeartbeat.h"
        "${CORE_DIR}/ServerHeartbeat.cpp"
        "${CORE_DIR}/TaidaFlowProxy.h"
    LIBS Qt6::Gui Qt6::Widgets Qt6::Qml)
target_compile_definitions(tst_server_heartbeat PRIVATE CORE_SOURCE_DIR="${CORE_DIR}")

# ---------------------------------------------------------------------------------------------
# w2-085: settings-page upper / lower limit alarms (LimitAlarmMonitor, Core/LimitAlarms.h): insert
# at once, equal = normal, disabled limits, 2 s back in range (injected clock and the real timer),
# groups judged per sensor, filter = corrected PT-02 - PT-03, offsets, settings changes, restart
# hand-over and lookup failures, alarmRecords / alarmViews, the UI's own JavaScript rule (read from
# TaidaFlowContent), the real Manager against two Modbus TCP servers of the test on 127.0.0.1
# (ports picked by the OS) incl. the unchanged 90 % alarm, and the wiring in the sources.
# Real SqlManager on a temporary data folder; no device, no fixed port.
# ---------------------------------------------------------------------------------------------
taidaflow_core_test(tst_limit_alarms
    SOURCES
        tst_limit_alarms.cpp
        "${CORE_DIR}/LimitAlarms.h"
        "${CORE_DIR}/LimitAlarms.cpp"
        "${CORE_DIR}/manager.h"
        "${CORE_DIR}/manager.cpp"
        "${CORE_DIR}/Modbus_Client.h"
        "${CORE_DIR}/Modbus_Client.cpp"
        "${CORE_DIR}/Ms300FaultReader.h"
        "${CORE_DIR}/Ms300FaultReader.cpp"
        "${CORE_DIR}/ModbusMapping.h"
        "${CORE_DIR}/ModbusServerBridgeMapping.h"
        "${CORE_DIR}/AlarmViews.h"
        "${CORE_DIR}/AlarmViews.cpp"
        "${CORE_DIR}/AlarmRecordFormat.h"
        "${CORE_DIR}/AlarmRecordFormat.cpp"
        "${CORE_DIR}/SqlManager.h"
        "${CORE_DIR}/SqlManager.cpp"
        "${CORE_DIR}/SqlManagerAlarms.cpp"
        "${CORE_DIR}/HistoryExport.h"
        "${CORE_DIR}/HistoryExport.cpp"
        "${CORE_DIR}/TaidaFlowProxy.h"
        "${CORE_DIR}/AppHttpServer/AppHttpServer.h"
        "${CORE_DIR}/AppHttpServer/AppHttpServer.cpp"
    LIBS Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Network Qt6::Sql Qt6::HttpServer Qt6::SerialBus Qt6::SerialPort)
target_compile_definitions(tst_limit_alarms PRIVATE CORE_SOURCE_DIR="${CORE_DIR}")

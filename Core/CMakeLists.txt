# Sources shared by Desktop and WebAssembly: only the QML-facing Proxy.
# (wasm-mirror pack 1.0.1: "Desktop-only Core / WASM 只建 Proxy" - package-integration
#  §1/§5.4, guide §10/§11). TaidaFlowProxy.h is a passive state container (its
#  constructor only fills demo list data, no I/O / thread / singleton), so the WASM
#  replica is built with the same constructor and the same meta-object contract.
set(TAIDAFLOW_CORE_SOURCES
        TaidaFlowProxy.h
)

# Authoritative hardware backend: Desktop only. Never compiled into the WASM build.
if(TAIDAFLOW_IS_WINDOWS_DESKTOP)
    list(APPEND TAIDAFLOW_CORE_SOURCES
        core.h
        core.cpp
        manager.h
        manager.cpp
        Modbus_Client.h
        Modbus_Client.cpp
        Ms300FaultReader.h
        Ms300FaultReader.cpp
        Modbus_Server.h
        Modbus_Server.cpp
        ModbusServerBridgeMapping.h
        ModbusMapping.h
        SqlManager.h
        SqlManager.cpp
        RESTManager.h
        RESTManager.cpp
    )
endif()

qt_add_qml_module(Core STATIC
        URI Core       # ↔ QML 端 import AmpSnap.Core 1.0
        VERSION 1.0
        SOURCES
        ${TAIDAFLOW_CORE_SOURCES}
)

target_include_directories(Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})


target_link_libraries(Core
        PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Gui
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Quick
        Qt${QT_VERSION_MAJOR}::Qml
        Qt${QT_VERSION_MAJOR}::QuickTimeline
        Qt${QT_VERSION_MAJOR}::ShaderTools
        Qt${QT_VERSION_MAJOR}::Network
#        Qt${QT_VERSION_MAJOR}::OpcUa
)

# Desktop-only Qt modules of the backend (find_package in the root CMakeLists.txt).
if(TAIDAFLOW_IS_WINDOWS_DESKTOP)
    target_link_libraries(Core
            PRIVATE
            Qt${QT_VERSION_MAJOR}::Concurrent
            Qt${QT_VERSION_MAJOR}::SerialBus
            Qt${QT_VERSION_MAJOR}::SerialPort
            Qt${QT_VERSION_MAJOR}::Sql
            Qt${QT_VERSION_MAJOR}::HttpServer
    )
endif()

# wasm-mirror (package-integration §5.1/§5.3, guide §2.1/§3): the Proxy target links the
# Mirror core and registers the concrete Proxy class once, then finalizes. Core is the
# target that owns TaidaFlowProxy.h (AUTOMOC) and is linked by both the Desktop and the
# WASM executable, so the typed signal relay is generated here for both builds.
target_link_libraries(Core PUBLIC WasmMirror::Core)
include("${WASM_MIRROR_PACK_DIR}/cmake/WasmMirrorProxyRegister.cmake")
wasm_mirror_register_proxy(
    TARGET Core
    CLASS TaidaFlowProxy
    HEADER "${CMAKE_CURRENT_SOURCE_DIR}/TaidaFlowProxy.h"
)
wasm_mirror_finalize_proxy_registration(TARGET Core)

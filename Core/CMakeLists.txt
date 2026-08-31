qt_add_qml_module(Core STATIC
        URI Core       # ↔ QML 端 import AmpSnap.Core 1.0
        VERSION 1.0
        SOURCES
        TaidaFlowProxy.h
        core.h
        core.cpp
        manager.h
        manager.cpp
        Modbus_Client.h
        Modbus_Client.cpp
        Modbus_Server.h
        Modbus_Server.cpp
        ModbusServerBridgeMapping.h
        ModbusMapping.h
        SqlManager.h
        SqlManager.cpp
        RESTManager.h
        RESTManager.cpp
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
        Qt${QT_VERSION_MAJOR}::Concurrent
        Qt${QT_VERSION_MAJOR}::SerialBus
        Qt${QT_VERSION_MAJOR}::Sql
        Qt${QT_VERSION_MAJOR}::HttpServer
#        Qt${QT_VERSION_MAJOR}::OpcUa
)

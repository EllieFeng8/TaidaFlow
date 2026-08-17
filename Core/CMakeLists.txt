qt_add_qml_module(Core STATIC
        URI TaidaFlowCoreInternal
        VERSION 1.0
        SOURCES
        TaidaFlowProxy.h


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
#        Qt${QT_VERSION_MAJOR}::SerialBus
#        Qt${QT_VERSION_MAJOR}::OpcUa
)

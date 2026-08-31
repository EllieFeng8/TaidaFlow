
cmake_minimum_required(VERSION 3.21.1)

option(LINK_INSIGHT "Link Qt Insight Tracker library" ON)
option(BUILD_QDS_COMPONENTS "Build design studio components" ON)

project(TaidaFlowApp LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(QT_QML_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/qml)
set(QML_IMPORT_PATH ${QT_QML_OUTPUT_DIRECTORY}
    CACHE STRING "Import paths for Qt Creator's code model"
    FORCE
)

find_package(Qt6 6.8 REQUIRED COMPONENTS
    Core Gui Widgets Qml Quick QuickTimeline ShaderTools SerialBus Sql
    Network WebSockets HttpServer
)
qt_standard_project_setup()

set(WASM_MIRROR_PACK_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/integration-pack/wasm-mirror")
add_subdirectory(
    "${WASM_MIRROR_PACK_DIR}"
    "${CMAKE_BINARY_DIR}/wasm-mirror-pack"
)

add_subdirectory(Core)

qt_add_executable(${CMAKE_PROJECT_NAME})
qt_add_resources(${CMAKE_PROJECT_NAME} "configuration"
    PREFIX "/"
    FILES
        qtquickcontrols2.conf)

include(qds)

target_link_libraries(${CMAKE_PROJECT_NAME}
    PRIVATE
        Core
        Coreplugin
        WasmMirror::Core
)


if (BUILD_QDS_COMPONENTS)
    include(qmlcomponents OPTIONAL)
endif()

if (LINK_INSIGHT)
    include(insight OPTIONAL)
endif ()

include(GNUInstallDirs)
install(TARGETS ${CMAKE_PROJECT_NAME}
  BUNDLE DESTINATION .
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

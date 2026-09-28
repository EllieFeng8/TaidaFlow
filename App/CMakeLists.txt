# w2-061 (docs/taidaflow_config_spec.md): config.json reader (desktop only) and the
# /runtime.json format + request (every platform; WebAssembly reads the Mirror port from it).
# Unit tests: App/tests (separate CTest project).
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    "runtimeinfo.cpp"
    "runtimeinfo.h"
)
if(NOT EMSCRIPTEN)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        "appconfig.cpp"
        "appconfig.h"
    )
endif()
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt${QT_VERSION_MAJOR}::Network)

target_sources(${CMAKE_PROJECT_NAME} PUBLIC
    "main.cpp"
)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Quick
    Qt${QT_VERSION_MAJOR}::Qml)
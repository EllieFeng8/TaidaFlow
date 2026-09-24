
target_sources(${CMAKE_PROJECT_NAME} PUBLIC
    "main.cpp"
    "embeddedfonts.cpp"
    "embeddedfonts.h"
)

# Embedded Noto Sans TC subset (OFL-1.1) for WebAssembly, which has no system
# CJK fonts. Regenerate with scripts/make_font_subset.py.
qt_add_resources(${CMAKE_PROJECT_NAME} "embedded_fonts"
    PREFIX "/fonts"
    BASE "fonts"
    FILES
        "fonts/TaidaFlowNotoSansTC-Regular.ttf"
        "fonts/TaidaFlowNotoSansTC-Bold.ttf"
)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Quick
    Qt${QT_VERSION_MAJOR}::Qml

    Core
)

# main.cpp includes the Desktop-only Core (core.h -> Modbus/SQL headers) only in the
# Windows Desktop build; these modules do not exist in the wasm_singlethread kit.
if(TAIDAFLOW_IS_WINDOWS_DESKTOP)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
        Qt${QT_VERSION_MAJOR}::SerialBus
        Qt${QT_VERSION_MAJOR}::Sql
    )
endif()

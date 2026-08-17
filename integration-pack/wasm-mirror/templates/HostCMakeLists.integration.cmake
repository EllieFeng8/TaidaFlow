# 這是導入片段，不應直接以 cmake -P 執行。
# 請把對應區塊合併到既有專案 CMakeLists.txt，並替換 target/路徑名稱。

include(CTest)

set(MYAPP_IS_WASM OFF)
set(MYAPP_IS_WINDOWS_DESKTOP OFF)
if(EMSCRIPTEN)
    set(MYAPP_IS_WASM ON)
elseif(WIN32)
    set(MYAPP_IS_WINDOWS_DESKTOP ON)
else()
    message(FATAL_ERROR "Only Windows Desktop and Qt WASM are configured")
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS
    Core Gui Network Qml Quick QuickControls2 WebSockets
)

if(MYAPP_IS_WINDOWS_DESKTOP)
    set(MYAPP_DESKTOP_QT_COMPONENTS SerialBus)
    if(BUILD_TESTING)
        list(APPEND MYAPP_DESKTOP_QT_COMPONENTS Test)
    endif()
    find_package(Qt6 6.8 REQUIRED COMPONENTS
        ${MYAPP_DESKTOP_QT_COMPONENTS}
    )
endif()

set(WASM_MIRROR_PACK_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wasm-mirror")
add_subdirectory(
    "${WASM_MIRROR_PACK_DIR}"
    "${CMAKE_BINARY_DIR}/wasm-mirror-pack"
)

# MyApplicationCore 必須先由導入專案建立。PUBLIC 讓分離的 executable main.cpp
# 也能取得 Mirror headers；若所有呼叫端都在同一 executable target，才可用 PRIVATE。
target_link_libraries(MyApplicationCore PUBLIC WasmMirror::Core)
set_target_properties(MyApplicationCore PROPERTIES AUTOMOC ON)

include("${WASM_MIRROR_PACK_DIR}/cmake/WasmMirrorProxyRegister.cmake")
wasm_mirror_register_proxy(
    TARGET MyApplicationCore
    CLASS MyUiProxy
    HEADER "${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/myuiproxy.h"
)
wasm_mirror_finalize_proxy_registration(TARGET MyApplicationCore)

# 只給 native Windows GUI executable；不可套到 Emscripten。
if(MYAPP_IS_WINDOWS_DESKTOP)
    set_target_properties(MyApplication PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# Desktop-only tests 不可進 WASM configure graph。
if(MYAPP_IS_WINDOWS_DESKTOP AND BUILD_TESTING)
    add_subdirectory(tests)
endif()

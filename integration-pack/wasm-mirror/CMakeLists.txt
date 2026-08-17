cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(WasmMirrorIntegrationPack VERSION 1.0.0 LANGUAGES CXX)
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS
    Core
    Network
    WebSockets
)

add_library(WasmMirrorProxyCore STATIC
    src/infrastructure/proxy_mirror/proxymirror.cpp
    src/infrastructure/proxy_mirror/proxymirror.h
    src/infrastructure/proxy_mirror/proxyrequestrelay.h
    src/infrastructure/proxy_mirror/wasmmirrorproxy.cpp
    src/infrastructure/proxy_mirror/wasmmirrorproxy.h
)

add_library(WasmMirror::Core ALIAS WasmMirrorProxyCore)

target_compile_features(WasmMirrorProxyCore PUBLIC cxx_std_17)
target_include_directories(WasmMirrorProxyCore
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(WasmMirrorProxyCore
    PUBLIC
        Qt6::Core
        Qt6::Network
        Qt6::WebSockets
)
set_target_properties(WasmMirrorProxyCore PROPERTIES
    AUTOMOC ON
    POSITION_INDEPENDENT_CODE ON
)

# 導入方在自己的 final executable／Core target 建立完成後，呼叫
# wasm_mirror_register_proxy() 與 wasm_mirror_finalize_proxy_registration()。
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/WasmMirrorProxyRegister.cmake")

option(WASM_MIRROR_BUILD_TESTS
    "Build the native ProxyMirror verification tests"
    OFF)

if(WASM_MIRROR_BUILD_TESTS)
    if(EMSCRIPTEN)
        message(FATAL_ERROR
            "WASM_MIRROR_BUILD_TESTS is a native verification suite; configure it with a Desktop Qt kit.")
    endif()
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    enable_testing()
    add_subdirectory(tests)
endif()


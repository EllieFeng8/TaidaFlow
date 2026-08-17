cmake_minimum_required(VERSION 3.21)
project(WasmMirrorConsumerExample LANGUAGES CXX)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Network WebSockets)

get_filename_component(WASM_MIRROR_PACK_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_subdirectory(
    "${WASM_MIRROR_PACK_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/wasm-mirror-pack"
)

qt_add_executable(WasmMirrorConsumer
    main.cpp
    exampleproxy.cpp
    exampleproxy.h
)
set_target_properties(WasmMirrorConsumer PROPERTIES AUTOMOC ON)
target_link_libraries(WasmMirrorConsumer
    PRIVATE
        WasmMirror::Core
        Qt6::Core
)

# 每一種會出現在 WASM Config 的 concrete Proxy class 註冊一次。
include("${WASM_MIRROR_PACK_DIR}/cmake/WasmMirrorProxyRegister.cmake")
wasm_mirror_register_proxy(
    TARGET WasmMirrorConsumer
    CLASS ExampleProxy
    HEADER "${CMAKE_CURRENT_LIST_DIR}/exampleproxy.h"
)
wasm_mirror_finalize_proxy_registration(TARGET WasmMirrorConsumer)

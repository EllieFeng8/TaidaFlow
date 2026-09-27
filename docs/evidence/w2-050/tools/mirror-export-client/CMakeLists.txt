# w2-050 test tool (dev-only): Proxy Mirror client that requests a history export like the web
# page does. Uses this project's own Core/TaidaFlowProxy.h and the vendored wasm-mirror pack
# source (integration-pack/wasm-mirror, same repository folder) - nothing outside taidaflow/.
# Build: see ..\build-mirror-client.bat (output build\w2-050-mirror-client).
cmake_minimum_required(VERSION 3.21)
project(w2050_mirror_export_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Qml WebSockets)

get_filename_component(TF "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(PACK_SRC "${TF}/integration-pack/wasm-mirror/src")

add_executable(mirror_export_client
    main.cpp
    "${TF}/Core/TaidaFlowProxy.h"
    "${PACK_SRC}/infrastructure/proxy_mirror/proxymirror.h"
    "${PACK_SRC}/infrastructure/proxy_mirror/proxymirror.cpp"
)
target_include_directories(mirror_export_client PRIVATE "${TF}/Core" "${PACK_SRC}")
target_link_libraries(mirror_export_client PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::WebSockets)
if(MSVC)
    target_compile_options(mirror_export_client PRIVATE /utf-8)
endif()

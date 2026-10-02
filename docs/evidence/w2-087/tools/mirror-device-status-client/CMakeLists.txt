# w2-087 test tool (dev-only): Proxy Mirror client that prints every change of deviceStatus and checks the
# final map. Copied from the w2-084 heartbeat client (docs/evidence/w2-084/tools/mirror-heartbeat-client;
# that folder is not used, the code was copied and
# adapted). Uses this project's own Core/TaidaFlowProxy.h and the vendored wasm-mirror pack source
# (integration-pack/wasm-mirror) - nothing outside taidaflow/.
# Build: see ..\build-live-tools.bat (output build\w2-087-run\tools).
cmake_minimum_required(VERSION 3.21)
project(w2087_mirror_device_status_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Qml WebSockets)

get_filename_component(TF "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(PACK_SRC "${TF}/integration-pack/wasm-mirror/src")

add_executable(mirror_device_status_client
    main.cpp
    "${TF}/Core/TaidaFlowProxy.h"
    "${PACK_SRC}/infrastructure/proxy_mirror/proxymirror.h"
    "${PACK_SRC}/infrastructure/proxy_mirror/proxymirror.cpp"
)
target_include_directories(mirror_device_status_client PRIVATE "${TF}/Core" "${PACK_SRC}")
target_link_libraries(mirror_device_status_client PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::WebSockets)
if(MSVC)
    target_compile_options(mirror_device_status_client PRIVATE /utf-8)
endif()

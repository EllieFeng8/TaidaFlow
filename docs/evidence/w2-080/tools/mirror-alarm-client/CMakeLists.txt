# w2-080 test tool (dev-only): Proxy Mirror client that sends alarmViewRequested like AlarmPage.qml
# and prints alarmViews[sessionId]. Adapted from the w2-050 export client (same repository folder,
# docs/evidence/w2-050/tools/mirror-export-client). Uses this project's own Core/TaidaFlowProxy.h and
# the vendored wasm-mirror pack source (integration-pack/wasm-mirror) - nothing outside taidaflow/.
# Build: see ..\build-mirror-alarm-client.bat (output build\w2-080-mirror-alarm-client).
cmake_minimum_required(VERSION 3.21)
project(w2080_mirror_alarm_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Qml WebSockets)

get_filename_component(TF "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(PACK_SRC "${TF}/integration-pack/wasm-mirror/src")

add_executable(mirror_alarm_client
    main.cpp
    "${TF}/Core/TaidaFlowProxy.h"
    "${PACK_SRC}/infrastructure/proxy_mirror/proxymirror.h"
    "${PACK_SRC}/infrastructure/proxy_mirror/proxymirror.cpp"
)
target_include_directories(mirror_alarm_client PRIVATE "${TF}/Core" "${PACK_SRC}")
target_link_libraries(mirror_alarm_client PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::WebSockets)
if(MSVC)
    target_compile_options(mirror_alarm_client PRIVATE /utf-8)
endif()

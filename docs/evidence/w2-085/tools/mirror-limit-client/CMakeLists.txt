# w2-085 test tools (dev-only, not part of the application):
#  * mirror_limit_client: Proxy Mirror client that writes sensorSettingsSv like the settings page and
#    watches alarmRecords (copied and changed from docs/evidence/w2-084/tools/mirror-heartbeat-client,
#    itself from the w2-080 alarm client; same hello / snapshot / patch handling).
#  * alarm_dump: prints the alarm_history rows of a data folder (read-only SQLite connection).
# Uses this project's own Core/TaidaFlowProxy.h and the vendored wasm-mirror pack source
# (integration-pack/wasm-mirror) - nothing outside taidaflow/.
# Build: ..\build-live-tools.bat (output build\w2-085-run\tools).
cmake_minimum_required(VERSION 3.21)
project(w2085_live_tools LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Qml WebSockets Sql)

get_filename_component(TF "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(PACK_SRC "${TF}/integration-pack/wasm-mirror/src")

add_executable(mirror_limit_client
    main.cpp
    "${TF}/Core/TaidaFlowProxy.h"
    "${PACK_SRC}/infrastructure/proxy_mirror/proxymirror.h"
    "${PACK_SRC}/infrastructure/proxy_mirror/proxymirror.cpp"
)
target_include_directories(mirror_limit_client PRIVATE "${TF}/Core" "${PACK_SRC}")
target_link_libraries(mirror_limit_client PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::WebSockets)

add_executable(alarm_dump alarm_dump.cpp)
target_link_libraries(alarm_dump PRIVATE Qt6::Core Qt6::Sql)

if(MSVC)
    target_compile_options(mirror_limit_client PRIVATE /utf-8)
    target_compile_options(alarm_dump PRIVATE /utf-8)
endif()

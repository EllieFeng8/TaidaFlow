# w2-086 test tools (dev-only, not part of the application):
#  * mirror_offset_client: Proxy Mirror client that writes sensorSettingsSv offsets like the settings
#    page, and compares the Modbus server input registers (Modbus TCP read) and REST /api/sensor/last
#    with the raw PVs + offsets (mirror handling copied from docs/evidence/w2-085/tools/mirror-limit-client,
#    itself from the w2-084 / w2-080 clients).
#  * sensor_dump: prints the sensor_data rows of a data folder (read-only SQLite connection).
# Uses this project's own Core/TaidaFlowProxy.h and the vendored wasm-mirror pack source
# (integration-pack/wasm-mirror) - nothing outside taidaflow/.
# Build: ..\build-live-tools.bat (output build\w2-086-run\tools).
cmake_minimum_required(VERSION 3.21)
project(w2086_live_tools LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Qml WebSockets Sql Network SerialBus)

get_filename_component(TF "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(PACK_SRC "${TF}/integration-pack/wasm-mirror/src")

add_executable(mirror_offset_client
    main.cpp
    "${TF}/Core/TaidaFlowProxy.h"
    "${PACK_SRC}/infrastructure/proxy_mirror/proxymirror.h"
    "${PACK_SRC}/infrastructure/proxy_mirror/proxymirror.cpp"
)
target_include_directories(mirror_offset_client PRIVATE "${TF}/Core" "${PACK_SRC}")
target_link_libraries(mirror_offset_client PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::WebSockets
                      Qt6::Network Qt6::SerialBus)

add_executable(sensor_dump sensor_dump.cpp)
target_link_libraries(sensor_dump PRIVATE Qt6::Core Qt6::Sql)

if(MSVC)
    target_compile_options(mirror_offset_client PRIVATE /utf-8)
    target_compile_options(sensor_dump PRIVATE /utf-8)
endif()

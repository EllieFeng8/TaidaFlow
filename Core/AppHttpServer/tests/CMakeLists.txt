# Stand-alone QTest of AppHttpServer: compiles ONLY ../AppHttpServer.{h,cpp} and the test,
# links only Qt (Core, Network, HttpServer, Test) - no application source.
#
#   cmake -S Core/AppHttpServer/tests -B build/apphttpserver-qtest -G Ninja
#         -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
#   cmake --build build/apphttpserver-qtest
#   ctest --test-dir build/apphttpserver-qtest -V
# (taidaflow: scripts\run-apphttpserver-tests.bat does exactly this.)
cmake_minimum_required(VERSION 3.22)
project(apphttpserver_qtest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Network HttpServer Test)
qt_standard_project_setup()

get_filename_component(APPHTTPSERVER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

qt_add_executable(tst_apphttpserver
    tst_apphttpserver.cpp
    "${APPHTTPSERVER_DIR}/AppHttpServer.h"
    "${APPHTTPSERVER_DIR}/AppHttpServer.cpp"
)
target_include_directories(tst_apphttpserver PRIVATE "${APPHTTPSERVER_DIR}")
target_link_libraries(tst_apphttpserver PRIVATE Qt6::Core Qt6::Network Qt6::HttpServer Qt6::Test)
if(WIN32)
    target_link_libraries(tst_apphttpserver PRIVATE psapi)   # process memory counters
endif()
target_compile_definitions(tst_apphttpserver PRIVATE
    APPHTTP_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/work")
set_target_properties(tst_apphttpserver PROPERTIES WIN32_EXECUTABLE OFF)

enable_testing()
add_test(NAME tst_apphttpserver COMMAND tst_apphttpserver -maxwarnings 0)
# Qt's bin folder is prepended to PATH when the test runs (Windows DLL lookup).
set_tests_properties(tst_apphttpserver PROPERTIES
    TIMEOUT 900
    ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")

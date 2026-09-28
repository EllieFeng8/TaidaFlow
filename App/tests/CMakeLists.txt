# w2-061: unit tests of App/appconfig.{h,cpp} (config.json reader, desktop) and
# App/runtimeinfo.{h,cpp} (/runtime.json format + request). Separate small CTest project so
# the application's Qt Design Studio CMake is not touched; it compiles the application's own
# source files (same folder tree, nothing copied). See README.md.
cmake_minimum_required(VERSION 3.22)
project(TaidaFlowAppTests LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Network Test)
qt_standard_project_setup()
enable_testing()

set(APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
# Qt DLLs for ctest runs (no PATH setup needed).
set(QT_BIN_DIR "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")

function(taidaflow_add_test name)
    qt_add_executable(${name} ${ARGN})
    set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
    target_include_directories(${name} PRIVATE "${APP_DIR}")
    target_link_libraries(${name} PRIVATE Qt6::Core Qt6::Network Qt6::Test)
    # Plain-text result per test in the build folder (ctest does not always echo QTest's
    # stdout on Windows) + the same on stdout.
    add_test(NAME ${name} COMMAND ${name} -o "${CMAKE_CURRENT_BINARY_DIR}/${name}.result.txt,txt" -o "-,txt")
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIR}"
        TIMEOUT 120)
endfunction()

taidaflow_add_test(tst_appconfig
    tst_appconfig.cpp
    "${APP_DIR}/appconfig.cpp"
    "${APP_DIR}/appconfig.h")

taidaflow_add_test(tst_runtimeinfo
    tst_runtimeinfo.cpp
    "${APP_DIR}/runtimeinfo.cpp"
    "${APP_DIR}/runtimeinfo.h")

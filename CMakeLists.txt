
cmake_minimum_required(VERSION 3.21.1)

option(LINK_INSIGHT "Link Qt Insight Tracker library" ON)
option(BUILD_QDS_COMPONENTS "Build design studio components" ON)

project(TaidaFlowApp LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(QT_QML_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/qml)
set(QML_IMPORT_PATH ${QT_QML_OUTPUT_DIRECTORY}
    CACHE STRING "Import paths for Qt Creator's code model"
    FORCE
)

# Target platform (wasm-mirror pack 1.0.1: package-integration §5.4, guide §11).
# Test EMSCRIPTEN first: CMAKE_HOST_WIN32 is still true while cross-compiling WASM.
set(TAIDAFLOW_IS_WASM OFF)
set(TAIDAFLOW_IS_WINDOWS_DESKTOP OFF)
if(EMSCRIPTEN)
    set(TAIDAFLOW_IS_WASM ON)
elseif(WIN32)
    set(TAIDAFLOW_IS_WINDOWS_DESKTOP ON)
else()
    message(FATAL_ERROR "TaidaFlow supports Windows Desktop and Qt WebAssembly only")
endif()

# Desktop/WASM shared components; the WASM mirror client needs Network + WebSockets.
find_package(Qt6 6.8 REQUIRED COMPONENTS
    Core Gui Widgets Qml Quick QuickTimeline ShaderTools
    Network WebSockets
)

# Desktop-only Core ("Desktop-only Core / WASM 只建 Proxy", package-integration §1, §5.4;
# guide §10, §11): the authoritative hardware backend (Modbus client/server, MS300 RTU,
# REST, SQLite) and its Qt modules exist only in the Windows Desktop build. The Qt 6.8.3
# wasm_singlethread kit ships no SerialBus/SerialPort/Sql/HttpServer/Concurrent config, so
# the component (here), the sources and the links (Core/, App/) stay in this branch together.
if(TAIDAFLOW_IS_WINDOWS_DESKTOP)
    set(TAIDAFLOW_DESKTOP_QT_COMPONENTS SerialBus SerialPort Sql HttpServer Concurrent)
    find_package(Qt6 6.8 REQUIRED COMPONENTS ${TAIDAFLOW_DESKTOP_QT_COMPONENTS})
endif()

qt_standard_project_setup()

set(WASM_MIRROR_PACK_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/integration-pack/wasm-mirror")
add_subdirectory(
    "${WASM_MIRROR_PACK_DIR}"
    "${CMAKE_BINARY_DIR}/wasm-mirror-pack"
)

add_subdirectory(Core)

qt_add_executable(${CMAKE_PROJECT_NAME})
qt_add_resources(${CMAKE_PROJECT_NAME} "configuration"
    PREFIX "/"
    FILES
        qtquickcontrols2.conf)

include(qds)

target_link_libraries(${CMAKE_PROJECT_NAME}
    PRIVATE
        Core
        Coreplugin
        WasmMirror::Core
)


if (BUILD_QDS_COMPONENTS)
    include(qmlcomponents OPTIONAL)
endif()

if (LINK_INSIGHT)
    include(insight OPTIONAL)
endif ()

# ---------------------------------------------------------------------------------------------
# w2-062 (Mango A8): the desktop build also produces a ready nginx folder, <build dir>/nginx, that
# works like the plant's: "cd <build dir>\nginx" + "start nginx" ("nginx -s quit" to stop).
#   <build dir>/nginx/conf/nginx.conf  written by scripts/install-nginx-config.ps1 -Build - the SAME
#                                      generator the plant's install step uses (one implementation)
#                                      - from the template deploy/nginx/taidaflow.conf and the config
#                                      file TAIDAFLOW_NGINX_CONFIG: web root <build dir>/web,
#                                      /exports = <dataDir>/exports, /api/ -> rest.port, /mirror ->
#                                      mirror.internalPort, listen nginx.port;
#   <build dir>/nginx/nginx.exe, conf/mime.types  copied from TAIDAFLOW_NGINX_DIR; logs/, temp/ created.
# Re-generated when the template, the generator or the config file changes; built together with
# TaidaFlowApp (target taidaflow_nginx_conf). No nginx in TAIDAFLOW_NGINX_DIR: only conf/nginx.conf is
# written and a CMake warning is printed; the build does not fail. Build output only (never the
# source tree, never the nginx installation). Desktop build only.
# ---------------------------------------------------------------------------------------------
if(TAIDAFLOW_IS_WINDOWS_DESKTOP)
    set(_tf_nginx_default "")
    set(_tf_nginx_best "")
    file(GLOB _tf_nginx_candidates LIST_DIRECTORIES true "C:/tools/nginx/nginx-*")
    foreach(_tf_c IN LISTS _tf_nginx_candidates)
        get_filename_component(_tf_n "${_tf_c}" NAME)
        string(REGEX REPLACE "^nginx-" "" _tf_v "${_tf_n}")
        if(EXISTS "${_tf_c}/nginx.exe" AND _tf_v MATCHES "^[0-9]+([.][0-9]+)*$"
           AND (_tf_nginx_best STREQUAL "" OR _tf_v VERSION_GREATER _tf_nginx_best))
            set(_tf_nginx_best "${_tf_v}")
            set(_tf_nginx_default "${_tf_c}")
        endif()
    endforeach()
    set(TAIDAFLOW_NGINX_DIR "${_tf_nginx_default}" CACHE PATH
        "nginx for Windows folder (nginx.exe, conf/mime.types) copied into <build>/nginx (default: newest C:/tools/nginx/nginx-<version>)")
    set(TAIDAFLOW_NGINX_CONFIG "${CMAKE_SOURCE_DIR}/deploy/dev/config.dev.json" CACHE FILEPATH
        "config.json from which <build>/nginx/conf/nginx.conf is generated")
    find_program(TAIDAFLOW_POWERSHELL NAMES powershell.exe powershell
                 HINTS "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0" REQUIRED)
    set(_tf_ngx "${CMAKE_BINARY_DIR}/nginx")
    set(_tf_ngx_copy "")
    set(_tf_ngx_deps "")
    if(TAIDAFLOW_NGINX_DIR AND EXISTS "${TAIDAFLOW_NGINX_DIR}/nginx.exe" AND EXISTS "${TAIDAFLOW_NGINX_DIR}/conf/mime.types")
        list(APPEND _tf_ngx_copy
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${TAIDAFLOW_NGINX_DIR}/nginx.exe" "${_tf_ngx}/nginx.exe"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${TAIDAFLOW_NGINX_DIR}/conf/mime.types" "${_tf_ngx}/conf/mime.types")
        list(APPEND _tf_ngx_deps "${TAIDAFLOW_NGINX_DIR}/nginx.exe" "${TAIDAFLOW_NGINX_DIR}/conf/mime.types")
        message(STATUS "[nginx] <build>/nginx: nginx.exe from ${TAIDAFLOW_NGINX_DIR}, conf/nginx.conf from ${TAIDAFLOW_NGINX_CONFIG}")
    else()
        message(WARNING "[nginx] TAIDAFLOW_NGINX_DIR=\"${TAIDAFLOW_NGINX_DIR}\" holds no nginx.exe + conf/mime.types - "
                        "<build>/nginx gets conf/nginx.conf only (no nginx.exe to start; see docs/BUILD.md). The build continues.")
        # A nginx.exe copied by an earlier configuration would no longer match the setting.
        file(REMOVE "${_tf_ngx}/nginx.exe")
    endif()
    add_custom_command(
        OUTPUT "${_tf_ngx}/conf/nginx.conf"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_tf_ngx}/conf" "${_tf_ngx}/logs" "${_tf_ngx}/temp"
        ${_tf_ngx_copy}
        COMMAND "${TAIDAFLOW_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/scripts/install-nginx-config.ps1"
                -Build -Config "${TAIDAFLOW_NGINX_CONFIG}" -InstallDir "${CMAKE_BINARY_DIR}" -NginxDir "${_tf_ngx}"
        DEPENDS
            "${CMAKE_SOURCE_DIR}/deploy/nginx/taidaflow.conf"
            "${CMAKE_SOURCE_DIR}/scripts/install-nginx-config.ps1"
            "${CMAKE_SOURCE_DIR}/scripts/taidaflow-config.ps1"
            "${TAIDAFLOW_NGINX_CONFIG}"
            ${_tf_ngx_deps}
        COMMENT "Generating nginx/conf/nginx.conf from ${TAIDAFLOW_NGINX_CONFIG}"
        VERBATIM)
    add_custom_target(taidaflow_nginx_conf ALL DEPENDS "${_tf_ngx}/conf/nginx.conf")
    # Built whenever TaidaFlowApp is built (the build presets build only that target).
    add_dependencies(${CMAKE_PROJECT_NAME} taidaflow_nginx_conf)
endif()

include(GNUInstallDirs)
install(TARGETS ${CMAKE_PROJECT_NAME}
  BUNDLE DESTINATION .
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# ---------------------------------------------------------------------------------------------
# w2-062 (D6): QML import scan of the WebAssembly build = project folders only.
#
# Qt 6.8 links the static QML plugins of a WebAssembly (static Qt) build from the result of
# qmlimportscanner, which qt_import_qml_plugins() runs at configure time as part of the target
# finalization (Qt 6.8 docs: "qt_import_qml_plugins"). Qt passes the target's SOURCE_DIR as the
# only -rootPath, i.e. this whole folder, and qmlimportscanner reads EVERY .qml/.js below it -
# including build\desktop\qml, build\*\ and dist\<package>\qml\ (a complete copy of Qt's desktop
# QML modules). Their imports (e.g. QtQuick.NativeStyle, QtQuick.Controls.Windows.impl) were
# then linked into TaidaFlowApp.wasm: 34,103,893 bytes in a used work tree vs 33,838,617 bytes
# in a fresh clone (w2-063). Neither qt_add_executable, qt_import_qml_plugins nor a documented
# target property changes that root, and qmlimportscanner's own -exclude option has no effect
# on the -rootPath scan (tested with Qt 6.8.3: identical output).
#
# Fix: after Qt's scan, the same scan is run again with one -rootPath per top-level folder of
# this project, leaving out build*, dist, cmake-build-* and hidden folders (= the git-ignored
# output folders), and its result replaces Qt's. Everything else of Qt's call (tool, import
# paths, qrc files, output file) is kept: the response file Qt wrote is reused with only the
# -rootPath argument replaced. The hook overrides Qt's internal function
# _qt_internal_scan_qml_imports (Qt6QmlMacros.cmake, Qt 6.8); CMake keeps the original as
# __qt_internal_scan_qml_imports. It is defined here, at the very end of the top-level
# CMakeLists.txt, right before Qt's deferred finalizer runs. Only the WebAssembly build is
# affected (a shared desktop Qt does not scan). If Qt's function or response file are not as
# expected (another Qt version) a warning is printed and Qt's own scan result is used.
# Configure log: "[qml-scan] ..." lines.
# ---------------------------------------------------------------------------------------------
if(TAIDAFLOW_IS_WASM)
    if(NOT COMMAND _qt_internal_scan_qml_imports)
        message(WARNING "[qml-scan] Qt's _qt_internal_scan_qml_imports not found (Qt ${Qt6_VERSION}) - "
                        "QML import scan NOT limited to the project folders")
    else()
        set(TAIDAFLOW_QML_SCAN_ROOTS "")
        file(GLOB _taidaflow_top LIST_DIRECTORIES true RELATIVE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/*")
        foreach(_taidaflow_entry IN LISTS _taidaflow_top)
            if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/${_taidaflow_entry}"
               AND NOT _taidaflow_entry MATCHES "^(build.*|dist|cmake-build-.*|[.].*)$")
                list(APPEND TAIDAFLOW_QML_SCAN_ROOTS "${CMAKE_SOURCE_DIR}/${_taidaflow_entry}")
            endif()
        endforeach()
        file(GLOB _taidaflow_root_qml "${CMAKE_SOURCE_DIR}/*.qml" "${CMAKE_SOURCE_DIR}/*.js" "${CMAKE_SOURCE_DIR}/*.mjs")
        if(_taidaflow_root_qml)
            message(WARNING "[qml-scan] QML/JS files directly in ${CMAKE_SOURCE_DIR} are not scanned "
                            "(put them into a folder): ${_taidaflow_root_qml}")
        endif()
        set_property(GLOBAL PROPERTY TAIDAFLOW_QML_SCAN_ROOTS "${TAIDAFLOW_QML_SCAN_ROOTS}")

        function(_qt_internal_scan_qml_imports target imports_file_var when_to_scan)
            __qt_internal_scan_qml_imports(${target} _tf_imports_file ${when_to_scan} ${ARGN})
            set(${imports_file_var} "${_tf_imports_file}" PARENT_SCOPE)
            if(NOT target STREQUAL "TaidaFlowApp" OR NOT when_to_scan STREQUAL "IMMEDIATELY")
                return()
            endif()
            get_target_property(_tf_bin ${target} BINARY_DIR)
            get_target_property(_tf_src ${target} SOURCE_DIR)
            set(_tf_rsp "${_tf_bin}/.qt/qml_imports/${target}_conf.rsp")
            if(NOT EXISTS "${_tf_rsp}")
                message(WARNING "[qml-scan] ${_tf_rsp} not found - Qt's whole-folder scan result is used")
                return()
            endif()
            file(STRINGS "${_tf_rsp}" _tf_args)
            list(FIND _tf_args "-rootPath" _tf_at)
            math(EXPR _tf_value_at "${_tf_at} + 1")
            list(LENGTH _tf_args _tf_count)
            if(_tf_at LESS 0 OR _tf_value_at GREATER_EQUAL _tf_count)
                message(WARNING "[qml-scan] no -rootPath in ${_tf_rsp} - Qt's whole-folder scan result is used")
                return()
            endif()
            list(GET _tf_args ${_tf_value_at} _tf_root)
            if(NOT _tf_root STREQUAL _tf_src)
                message(WARNING "[qml-scan] unexpected -rootPath ${_tf_root} - Qt's whole-folder scan result is used")
                return()
            endif()
            include("${_tf_imports_file}")
            set(_tf_qt_count ${qml_import_scanner_imports_count})
            get_property(_tf_roots GLOBAL PROPERTY TAIDAFLOW_QML_SCAN_ROOTS)
            set(_tf_root_args "")
            foreach(_tf_dir IN LISTS _tf_roots)
                list(APPEND _tf_root_args "-rootPath" "${_tf_dir}")
            endforeach()
            list(REMOVE_AT _tf_args ${_tf_value_at})
            list(REMOVE_AT _tf_args ${_tf_at})
            list(INSERT _tf_args ${_tf_at} ${_tf_root_args})
            set(_tf_rsp2 "${_tf_bin}/.qt/qml_imports/${target}_conf_projectroots.rsp")
            list(JOIN _tf_args "\n" _tf_rsp2_content)
            file(WRITE "${_tf_rsp2}" "${_tf_rsp2_content}")
            _qt_internal_find_qmlimportscanner_path(_tf_tool TRUE)
            set(_tf_exec_args
                COMMAND "${_tf_tool}" "@${_tf_rsp2}"
                WORKING_DIRECTORY "${_tf_src}"
                RESULT_VARIABLE _tf_rc)
            _qt_internal_execute_proccess_in_qt_env(_tf_exec_args)
            if(_tf_rc)
                message(FATAL_ERROR "[qml-scan] qmlimportscanner with the project folders failed: ${_tf_rc}")
            endif()
            include("${_tf_imports_file}")
            string(REPLACE "${_tf_src}/" "" _tf_names "${_tf_roots}")
            message(STATUS "[qml-scan] QML import scan limited to the project folders (${_tf_names}); "
                           "build*/dist/hidden folders left out: ${qml_import_scanner_imports_count} import(s) "
                           "(Qt's whole-folder scan: ${_tf_qt_count})")
        endfunction()
    endif()
endif()

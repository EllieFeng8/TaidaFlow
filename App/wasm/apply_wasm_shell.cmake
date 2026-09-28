# w2-058: replace the Qt-generated WebAssembly page with the TaidaFlow loading page.
#
# Qt 6.8 has no CMake option for a custom HTML shell: at configure time its target finalizer
# (_qt_internal_wasm_add_target_helpers) writes <build>/TaidaFlowApp.html from
# <Qt>/plugins/platforms/wasm_shell.html.  Qt's documentation allows replacing that page with a
# custom HTML file ("You can deploy app.html as-is, or discard it in favor of a custom HTML file").
# App/CMakeLists.txt runs this script at CONFIGURE time, in a deferred call right after Qt's
# finalizer wrote the page (so every configure of a WebAssembly build ends with our page; the build
# itself never writes TaidaFlowApp.html). It can also be run by hand with the command below.
#   1. HTML is Qt's page (no "taidaflow-wasm-shell" marker, the normal case): the placeholder
#      values are taken from it (title = APPNAME, "entryFunction: window.<APPEXPORTNAME>," and the
#      line after "containerElements: [screen]," = PRELOAD) and stored in STATE;
#      HTML is already our page (script run again by hand): the values come from STATE.
#   2. SHELL_TEMPLATE is configured (@ONLY) with those values; the result must not contain any
#      @NAME@ placeholder; HTML is replaced only when the content differs.
#   3. The values and the result are printed (configure log = first part of the build log).
#
# cmake -DSHELL_TEMPLATE=<template> -DHTML=<build>/TaidaFlowApp.html -DSTATE=<values file> -P apply_wasm_shell.cmake
cmake_minimum_required(VERSION 3.21)

foreach(var SHELL_TEMPLATE HTML STATE)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "[wasm-shell] ${var} is not set")
    endif()
endforeach()
if(NOT EXISTS "${SHELL_TEMPLATE}")
    message(FATAL_ERROR "[wasm-shell] template not found: ${SHELL_TEMPLATE}")
endif()
if(NOT EXISTS "${HTML}")
    message(FATAL_ERROR "[wasm-shell] ${HTML} not found (Qt writes it when CMake configures the wasm build)")
endif()

set(marker "taidaflow-wasm-shell")
file(READ "${HTML}" page)
string(FIND "${page}" "${marker}" marker_pos)

if(marker_pos EQUAL -1)
    set(source "Qt-generated page")
    if(NOT page MATCHES "<title>([^<]*)</title>")
        message(FATAL_ERROR "[wasm-shell] no <title> in the Qt-generated ${HTML}")
    endif()
    set(APPNAME "${CMAKE_MATCH_1}")
    if(NOT page MATCHES "entryFunction: window\\.([A-Za-z0-9_$]+),")
        message(FATAL_ERROR "[wasm-shell] no 'entryFunction: window.<name>,' in the Qt-generated ${HTML}")
    endif()
    set(APPEXPORTNAME "${CMAKE_MATCH_1}")
    if(NOT page MATCHES "containerElements: \\[screen\\],\r?\n([^\r\n]*)\r?\n")
        message(FATAL_ERROR "[wasm-shell] no line after 'containerElements: [screen],' in the Qt-generated ${HTML}")
    endif()
    string(STRIP "${CMAKE_MATCH_1}" PRELOAD)
    # Qt 6.8: "" (static build) or "preload: [...]," (shared build).
    if(NOT PRELOAD STREQUAL "" AND NOT PRELOAD MATCHES "^preload: \\[.*\\],$")
        message(FATAL_ERROR "[wasm-shell] unexpected PRELOAD line '${PRELOAD}' in ${HTML} - Qt's wasm_shell.html changed, check the template")
    endif()
    if(NOT page MATCHES "<script src=\"${APPNAME}\\.js\"></script>")
        message(FATAL_ERROR "[wasm-shell] Qt-generated ${HTML} does not load ${APPNAME}.js")
    endif()
    file(WRITE "${STATE}"
        "# written by apply_wasm_shell.cmake from the Qt-generated ${HTML}\n"
        "set(APPNAME \"${APPNAME}\")\n"
        "set(APPEXPORTNAME \"${APPEXPORTNAME}\")\n"
        "set(PRELOAD [==[${PRELOAD}]==])\n")
else()
    set(source "values saved from the Qt-generated page (${STATE})")
    if(NOT EXISTS "${STATE}")
        message(FATAL_ERROR "[wasm-shell] ${HTML} is already the TaidaFlow page but ${STATE} is missing - re-run the CMake configure (Qt then regenerates its page)")
    endif()
    include("${STATE}")
endif()

set(tmp "${STATE}.html")
configure_file("${SHELL_TEMPLATE}" "${tmp}" @ONLY)
file(READ "${tmp}" result)
if(result MATCHES "@[A-Za-z_][A-Za-z0-9_]*@")
    message(FATAL_ERROR "[wasm-shell] placeholder '${CMAKE_MATCH_0}' left in the configured page")
endif()
if(result MATCHES "entryFunction: window\\.${APPEXPORTNAME},")
else()
    message(FATAL_ERROR "[wasm-shell] configured page has no 'entryFunction: window.${APPEXPORTNAME},'")
endif()

file(COPY_FILE "${tmp}" "${HTML}" ONLY_IF_DIFFERENT RESULT copy_result)
if(NOT copy_result STREQUAL "0")
    message(FATAL_ERROR "[wasm-shell] cannot write ${HTML}: ${copy_result}")
endif()
file(REMOVE "${tmp}")
file(SHA256 "${HTML}" sha)
message(STATUS "[wasm-shell] ${HTML} <- ${SHELL_TEMPLATE}")
message(STATUS "[wasm-shell] APPNAME=${APPNAME} APPEXPORTNAME=${APPEXPORTNAME} PRELOAD='${PRELOAD}' (from ${source}) sha256=${sha}")

# w2-061 (docs/taidaflow_config_spec.md): config.json reader (desktop only) and the
# /runtime.json format + request (every platform; WebAssembly reads the Mirror port from it).
# Unit tests: App/tests (separate CTest project).
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    "runtimeinfo.cpp"
    "runtimeinfo.h"
)
if(NOT EMSCRIPTEN)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        "appconfig.cpp"
        "appconfig.h"
        # w2-064 (spec §2 log): the desktop app writes its own daily log files.
        "applog.cpp"
        "applog.h"
    )
endif()
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt${QT_VERSION_MAJOR}::Network)

target_sources(${CMAKE_PROJECT_NAME} PUBLIC
    "main.cpp"
    "embeddedfonts.cpp"
    "embeddedfonts.h"
)

# Embedded Noto Sans TC subset (OFL-1.1): the interface font of BOTH the desktop and the
# WebAssembly build (w2-082; WebAssembly also has no system CJK fonts). Regenerate only with
# scripts/make-font-subset.ps1 (docs/BUILD.md, embedded font subset).
qt_add_resources(${CMAKE_PROJECT_NAME} "embedded_fonts"
    PREFIX "/fonts"
    BASE "fonts"
    FILES
        "fonts/TaidaFlowNotoSansTC-Regular.ttf"
        "fonts/TaidaFlowNotoSansTC-Bold.ttf"
)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Quick
    Qt${QT_VERSION_MAJOR}::Qml

    Core
)

# main.cpp includes the Desktop-only Core (core.h -> Modbus/SQL headers) only in the
# Windows Desktop build; these modules do not exist in the wasm_singlethread kit.
if(TAIDAFLOW_IS_WINDOWS_DESKTOP)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
        Qt${QT_VERSION_MAJOR}::SerialBus
        Qt${QT_VERSION_MAJOR}::Sql
    )
endif()

# w2-058: TaidaFlow loading page for the WebAssembly build.
# Qt 6.8 has no CMake option for a custom HTML shell: its target finalizer (deferred to the end of
# the top-level CMakeLists.txt) writes <build>/TaidaFlowApp.html from Qt's wasm_shell.html at
# CONFIGURE time, and the build never touches that file again. So, also at configure time and
# right after Qt's finalizer, App/wasm/apply_wasm_shell.cmake writes App/wasm/TaidaFlowApp.shell.html
# over it, with the placeholder values (APPNAME, APPEXPORTNAME, PRELOAD) taken from the page Qt
# just generated; the values are printed in the configure log ("[wasm-shell] ..."). The template
# and the script are configure dependencies: editing them re-runs the configure (and this step)
# on the next build. Desktop builds are not affected.
if(TAIDAFLOW_IS_WASM)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/wasm/TaidaFlowApp.shell.html"
        "${CMAKE_CURRENT_SOURCE_DIR}/wasm/apply_wasm_shell.cmake")

    function(taidaflow_apply_wasm_shell target template script state attempt)
        # Qt's finalizer may re-defer itself behind other deferred Qt tasks; wait until it ran.
        get_target_property(finalized ${target} _qt_is_finalized)
        if(NOT finalized)
            if(attempt GREATER 20)
                message(FATAL_ERROR "[wasm-shell] ${target} was not finalized by Qt - TaidaFlowApp.html not replaced")
            endif()
            math(EXPR attempt "${attempt} + 1")
            cmake_language(EVAL CODE "cmake_language(DEFER CALL taidaflow_apply_wasm_shell [[${target}]] [[${template}]] [[${script}]] [[${state}]] ${attempt})")
            return()
        endif()
        # Same location and name as Qt's _qt_internal_wasm_add_target_helpers.
        get_target_property(out_dir ${target} RUNTIME_OUTPUT_DIRECTORY)
        if(NOT out_dir)
            get_target_property(out_dir ${target} BINARY_DIR)
        endif()
        get_target_property(out_name ${target} OUTPUT_NAME)
        if(NOT out_name)
            set(out_name "${target}")
        endif()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" "-DSHELL_TEMPLATE=${template}" "-DHTML=${out_dir}/${out_name}.html"
                    "-DSTATE=${state}" -P "${script}"
            RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "[wasm-shell] apply_wasm_shell.cmake failed (${rc})")
        endif()
    endfunction()

    cmake_language(EVAL CODE "cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]] CALL taidaflow_apply_wasm_shell [[${CMAKE_PROJECT_NAME}]] [[${CMAKE_CURRENT_SOURCE_DIR}/wasm/TaidaFlowApp.shell.html]] [[${CMAKE_CURRENT_SOURCE_DIR}/wasm/apply_wasm_shell.cmake]] [[${CMAKE_CURRENT_BINARY_DIR}/wasm-shell-values.cmake]] 0)")
endif()

# Register QObject-derived Proxy classes whose non-NOTIFY signals need a
# type-safe WebAssembly ProxyMirror relay.
#
# Usage (after the target and its include paths/definitions exist):
#
#   wasm_mirror_register_proxy(
#       TARGET MyCore
#       CLASS UiProxy
#       HEADER "${CMAKE_CURRENT_SOURCE_DIR}/src/uiproxy.h")
#   wasm_mirror_register_proxy(
#       TARGET MyCore
#       CLASS AlarmProxy
#       HEADER "${CMAKE_CURRENT_SOURCE_DIR}/src/alarmproxy.h")
#   wasm_mirror_finalize_proxy_registration(TARGET MyCore)

include_guard(GLOBAL)

function(wasm_mirror_register_proxy)
    cmake_parse_arguments(PARSE_ARGV 0 _mirror
        ""
        "TARGET;CLASS;HEADER"
        "")

    if(_mirror_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "wasm_mirror_register_proxy received unknown arguments: ${_mirror_UNPARSED_ARGUMENTS}")
    endif()
    foreach(_required TARGET CLASS HEADER)
        if(NOT DEFINED _mirror_${_required}
           OR _mirror_${_required} STREQUAL "")
            message(FATAL_ERROR
                "wasm_mirror_register_proxy requires ${_required}")
        endif()
    endforeach()
    if(NOT TARGET "${_mirror_TARGET}")
        message(FATAL_ERROR
            "wasm_mirror_register_proxy target does not exist: ${_mirror_TARGET}")
    endif()
    get_target_property(_aliased_target "${_mirror_TARGET}" ALIASED_TARGET)
    if(_aliased_target)
        message(FATAL_ERROR
            "Register Proxy classes on the real target ${_aliased_target}, not alias ${_mirror_TARGET}")
    endif()
    get_target_property(_imported_target "${_mirror_TARGET}" IMPORTED)
    if(_imported_target)
        message(FATAL_ERROR
            "Cannot generate a Proxy relay in imported target ${_mirror_TARGET}")
    endif()

    get_target_property(_finalized "${_mirror_TARGET}"
        WASM_MIRROR_PROXY_REGISTRATION_FINALIZED)
    if(_finalized)
        message(FATAL_ERROR
            "Proxy registration for target ${_mirror_TARGET} was already finalized")
    endif()

    if(NOT _mirror_CLASS MATCHES
       "^[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)*$")
        message(FATAL_ERROR
            "Invalid registered Proxy C++ class name: ${_mirror_CLASS}")
    endif()

    get_filename_component(_header "${_mirror_HEADER}" ABSOLUTE
        BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    file(TO_CMAKE_PATH "${_header}" _header)
    if(NOT EXISTS "${_header}")
        message(FATAL_ERROR
            "Registered Proxy header does not exist: ${_header}")
    endif()

    get_target_property(_registered_classes "${_mirror_TARGET}"
        WASM_MIRROR_PROXY_CLASSES)
    if(NOT _registered_classes
       OR _registered_classes MATCHES "-NOTFOUND$")
        set(_registered_classes)
    endif()
    list(FIND _registered_classes "${_mirror_CLASS}" _duplicate_index)
    if(NOT _duplicate_index EQUAL -1)
        message(FATAL_ERROR
            "Proxy class ${_mirror_CLASS} is already registered for target ${_mirror_TARGET}")
    endif()

    set_property(TARGET "${_mirror_TARGET}" APPEND PROPERTY
        WASM_MIRROR_PROXY_CLASSES "${_mirror_CLASS}")
    set_property(TARGET "${_mirror_TARGET}" APPEND PROPERTY
        WASM_MIRROR_PROXY_HEADERS "${_header}")

    # Listing the header on the target also guarantees that normal AUTOMOC
    # generates the real QObject meta-object implementation. The standalone
    # moc invocation created by finalize is metadata-only and is not compiled.
    target_sources("${_mirror_TARGET}" PRIVATE "${_header}")
endfunction()

function(wasm_mirror_finalize_proxy_registration)
    cmake_parse_arguments(PARSE_ARGV 0 _mirror
        ""
        "TARGET"
        "")

    if(_mirror_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "wasm_mirror_finalize_proxy_registration received unknown arguments: ${_mirror_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT DEFINED _mirror_TARGET OR _mirror_TARGET STREQUAL "")
        message(FATAL_ERROR
            "wasm_mirror_finalize_proxy_registration requires TARGET")
    endif()
    if(NOT TARGET "${_mirror_TARGET}")
        message(FATAL_ERROR
            "wasm_mirror_finalize_proxy_registration target does not exist: ${_mirror_TARGET}")
    endif()

    get_target_property(_finalized "${_mirror_TARGET}"
        WASM_MIRROR_PROXY_REGISTRATION_FINALIZED)
    if(_finalized)
        message(FATAL_ERROR
            "Proxy registration for target ${_mirror_TARGET} was already finalized")
    endif()

    get_target_property(_registered_classes "${_mirror_TARGET}"
        WASM_MIRROR_PROXY_CLASSES)
    get_target_property(_registered_headers "${_mirror_TARGET}"
        WASM_MIRROR_PROXY_HEADERS)
    if(NOT _registered_classes
       OR _registered_classes MATCHES "-NOTFOUND$")
        message(FATAL_ERROR
            "Target ${_mirror_TARGET} has no registered Proxy classes")
    endif()

    list(LENGTH _registered_classes _class_count)
    list(LENGTH _registered_headers _header_count)
    if(NOT _class_count EQUAL _header_count)
        message(FATAL_ERROR
            "Internal Proxy registration class/header count mismatch for ${_mirror_TARGET}")
    endif()

    get_target_property(_target_binary_dir "${_mirror_TARGET}" BINARY_DIR)
    string(MAKE_C_IDENTIFIER "${_mirror_TARGET}" _target_key)
    set(_generated_dir
        "${_target_binary_dir}/generated/wasm_mirror/${_target_key}")
    set(_manifest "${_generated_dir}/registrations.cmake")
    set(_relay_cpp "${_generated_dir}/proxy_event_relay_generated.cpp")

    set(_moc_include_options
        "$<$<BOOL:$<TARGET_PROPERTY:${_mirror_TARGET},INCLUDE_DIRECTORIES>>:-I$<JOIN:$<TARGET_PROPERTY:${_mirror_TARGET},INCLUDE_DIRECTORIES>,;-I>>")
    set(_moc_definition_options
        "$<$<BOOL:$<TARGET_PROPERTY:${_mirror_TARGET},COMPILE_DEFINITIONS>>:-D$<JOIN:$<TARGET_PROPERTY:${_mirror_TARGET},COMPILE_DEFINITIONS>,;-D>>")
    set(_moc_platform_options)
    if(WIN32)
        list(APPEND _moc_platform_options -DWIN32)
    endif()
    if(EMSCRIPTEN)
        # Host moc does not inherit the target compiler's builtin macro.
        list(APPEND _moc_platform_options -D__EMSCRIPTEN__)
    endif()
    if(MSVC)
        list(APPEND _moc_platform_options --compiler-flavor=msvc)
    endif()

    file(MAKE_DIRECTORY "${_generated_dir}")
    set(_manifest_contents
        "# Generated Proxy relay registration manifest. Do not edit.\n")
    string(APPEND _manifest_contents
        "set(WASM_MIRROR_REGISTRATION_COUNT ${_class_count})\n")
    set(_moc_json_outputs)

    math(EXPR _last_registration "${_class_count} - 1")
    foreach(_index RANGE 0 ${_last_registration})
        list(GET _registered_classes ${_index} _class_name)
        list(GET _registered_headers ${_index} _header)

        string(MAKE_C_IDENTIFIER "${_class_name}" _readable_key)
        string(SHA256 _registration_hash "${_class_name}|${_header}")
        string(SUBSTRING "${_registration_hash}" 0 12 _short_hash)
        set(_class_key "${_readable_key}_${_short_hash}")
        set(_moc_cpp "${_generated_dir}/moc_${_class_key}_for_relay.cpp")
        set(_moc_json "${_moc_cpp}.json")
        set(_moc_depfile "${_moc_cpp}.d")

        add_custom_command(
            OUTPUT
                "${_moc_cpp}"
                "${_moc_json}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "${_generated_dir}"
            COMMAND Qt6::moc
                    --output-json
                    --output-dep-file
                    --dep-file-path "${_moc_depfile}"
                    --dep-file-rule-name "${_moc_cpp}"
                    "${_moc_include_options}"
                    "${_moc_definition_options}"
                    ${_moc_platform_options}
                    -o "${_moc_cpp}"
                    "${_header}"
            DEPENDS
                "${_header}"
                Qt6::moc
            DEPFILE "${_moc_depfile}"
            BYPRODUCTS "${_moc_depfile}"
            COMMENT "Generating ProxyMirror relay metadata for ${_class_name}"
            COMMAND_EXPAND_LISTS
            VERBATIM
        )

        list(APPEND _moc_json_outputs "${_moc_json}")
        string(APPEND _manifest_contents
            "set(WASM_MIRROR_REGISTRATION_${_index}_CLASS [==[${_class_name}]==])\n"
            "set(WASM_MIRROR_REGISTRATION_${_index}_HEADER [==[${_header}]==])\n"
            "set(WASM_MIRROR_REGISTRATION_${_index}_JSON [==[${_moc_json}]==])\n"
            "set(WASM_MIRROR_REGISTRATION_${_index}_KEY [==[${_class_key}]==])\n")
    endforeach()

    file(WRITE "${_manifest}" "${_manifest_contents}")

    add_custom_command(
        OUTPUT "${_relay_cpp}"
        COMMAND "${CMAKE_COMMAND}"
                "-DREGISTRATION_MANIFEST=${_manifest}"
                "-DOUTPUT_CPP=${_relay_cpp}"
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/GenerateProxyRequestRelay.cmake"
        DEPENDS
            ${_moc_json_outputs}
            "${_manifest}"
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/GenerateProxyRequestRelay.cmake"
        COMMENT "Generating aggregate ProxyMirror event relay for ${_mirror_TARGET}"
        VERBATIM
    )

    set_source_files_properties("${_relay_cpp}"
        TARGET_DIRECTORY "${_mirror_TARGET}"
        PROPERTIES
            GENERATED TRUE
            SKIP_AUTOMOC TRUE
            SKIP_AUTOUIC TRUE
    )
    target_sources("${_mirror_TARGET}" PRIVATE "${_relay_cpp}")
    set_property(TARGET "${_mirror_TARGET}" PROPERTY
        WASM_MIRROR_PROXY_REGISTRATION_FINALIZED TRUE)
endfunction()

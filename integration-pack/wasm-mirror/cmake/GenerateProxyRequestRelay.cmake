# Generate one type-safe, type-erased Proxy event relay from the moc JSON of
# every C++ Proxy class registered for a target.
#
# Required inputs:
#   REGISTRATION_MANIFEST - CMake file emitted by
#                           WasmMirrorProxyRegister.cmake.
#   OUTPUT_CPP            - generated aggregate C++ implementation.

cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED REGISTRATION_MANIFEST
   OR NOT EXISTS "${REGISTRATION_MANIFEST}")
    message(FATAL_ERROR
        "Proxy relay registration manifest does not exist: ${REGISTRATION_MANIFEST}")
endif()
if(NOT DEFINED OUTPUT_CPP OR OUTPUT_CPP STREQUAL "")
    message(FATAL_ERROR "Proxy relay OUTPUT_CPP was not provided")
endif()

include("${REGISTRATION_MANIFEST}")

if(NOT DEFINED WASM_MIRROR_REGISTRATION_COUNT
   OR WASM_MIRROR_REGISTRATION_COUNT LESS 1)
    message(FATAL_ERROR "Proxy relay manifest does not contain any registration")
endif()

set(_generated_includes)
set(_generated_helpers)
set(_generated_dispatch)

math(EXPR _last_registration "${WASM_MIRROR_REGISTRATION_COUNT} - 1")
foreach(_registration_index RANGE 0 ${_last_registration})
    set(_class_variable
        "WASM_MIRROR_REGISTRATION_${_registration_index}_CLASS")
    set(_header_variable
        "WASM_MIRROR_REGISTRATION_${_registration_index}_HEADER")
    set(_json_variable
        "WASM_MIRROR_REGISTRATION_${_registration_index}_JSON")
    set(_key_variable
        "WASM_MIRROR_REGISTRATION_${_registration_index}_KEY")

    foreach(_required_variable
            _class_variable _header_variable _json_variable _key_variable)
        if(NOT DEFINED ${${_required_variable}}
           OR "${${${_required_variable}}}" STREQUAL "")
            message(FATAL_ERROR
                "Proxy relay manifest registration ${_registration_index} is incomplete")
        endif()
    endforeach()

    set(_class_name "${${_class_variable}}")
    set(_header_path "${${_header_variable}}")
    set(_json_path "${${_json_variable}}")
    set(_class_key "${${_key_variable}}")

    if(NOT EXISTS "${_json_path}")
        message(FATAL_ERROR
            "moc JSON for registered Proxy ${_class_name} does not exist: ${_json_path}")
    endif()

    file(READ "${_json_path}" _moc_json)
    string(JSON _class_count ERROR_VARIABLE _class_count_error
           LENGTH "${_moc_json}" classes)
    if(NOT _class_count_error STREQUAL "NOTFOUND")
        message(FATAL_ERROR
            "Invalid moc JSON for registered Proxy ${_class_name}: ${_class_count_error}")
    endif()

    set(_class_index -1)
    if(_class_count GREATER 0)
        math(EXPR _last_class "${_class_count} - 1")
        foreach(_candidate_index RANGE 0 ${_last_class})
            string(JSON _candidate_class_name GET
                   "${_moc_json}" classes ${_candidate_index} className)
            string(JSON _candidate_qualified_name
                   ERROR_VARIABLE _qualified_name_error GET
                   "${_moc_json}" classes ${_candidate_index} qualifiedClassName)
            if(NOT _qualified_name_error STREQUAL "NOTFOUND")
                set(_candidate_qualified_name "")
            endif()
            # Require the qualified name when moc provides one. This prevents
            # `Proxy` from accidentally selecting NamespaceA::Proxy when the
            # registered C++ type must be NamespaceB::Proxy.
            if((NOT _candidate_qualified_name STREQUAL ""
                AND _candidate_qualified_name STREQUAL _class_name)
               OR (_candidate_qualified_name STREQUAL ""
                   AND _candidate_class_name STREQUAL _class_name))
                set(_class_index ${_candidate_index})
                break()
            endif()
        endforeach()
    endif()
    if(_class_index LESS 0)
        message(FATAL_ERROR
            "moc JSON ${_json_path} does not contain registered Proxy class ${_class_name}")
    endif()

    string(JSON _is_qobject ERROR_VARIABLE _object_error GET
           "${_moc_json}" classes ${_class_index} object)
    if(NOT _object_error STREQUAL "NOTFOUND" OR NOT _is_qobject)
        message(FATAL_ERROR
            "Registered Proxy ${_class_name} must be a QObject class with Q_OBJECT")
    endif()

    file(TO_CMAKE_PATH "${_header_path}" _header_include)
    string(REPLACE "\\" "\\\\" _header_include "${_header_include}")
    string(REPLACE "\"" "\\\"" _header_include "${_header_include}")
    list(FIND _generated_includes "${_header_include}" _include_index)
    if(_include_index LESS 0)
        list(APPEND _generated_includes "${_header_include}")
    endif()

    # A NOTIFY signal is state, including NOTIFY for CONSTANT and STORED false
    # properties. It must never be reflected as a remotely callable UI event.
    set(_notify_names)
    string(JSON _property_count ERROR_VARIABLE _property_count_error LENGTH
           "${_moc_json}" classes ${_class_index} properties)
    if(NOT _property_count_error STREQUAL "NOTFOUND")
        set(_property_count 0)
    endif()
    if(_property_count GREATER 0)
        math(EXPR _last_property "${_property_count} - 1")
        foreach(_property_index RANGE 0 ${_last_property})
            string(JSON _notify_name ERROR_VARIABLE _notify_error GET
                   "${_moc_json}" classes ${_class_index} properties
                   ${_property_index} notify)
            if(_notify_error STREQUAL "NOTFOUND")
                list(APPEND _notify_names "${_notify_name}")
            endif()
        endforeach()
    endif()

    string(APPEND _generated_helpers
"static void connectProxyMirrorEvents_${_class_key}(
    ${_class_name} &proxy,
    QObject &lifetimeContext,
    const ProxyEventHandler &handler)
{
")

    set(_event_names)
    set(_event_count 0)
    string(JSON _signal_count ERROR_VARIABLE _signal_count_error LENGTH
           "${_moc_json}" classes ${_class_index} signals)
    if(NOT _signal_count_error STREQUAL "NOTFOUND")
        set(_signal_count 0)
    endif()
    if(_signal_count GREATER 0)
        math(EXPR _last_signal "${_signal_count} - 1")
        foreach(_signal_index RANGE 0 ${_last_signal})
            string(JSON _signal_name GET
                   "${_moc_json}" classes ${_class_index} signals
                   ${_signal_index} name)
            if(_signal_name IN_LIST _notify_names)
                continue()
            endif()
            string(JSON _signal_access ERROR_VARIABLE _signal_access_error GET
                   "${_moc_json}" classes ${_class_index} signals
                   ${_signal_index} access)
            if(NOT _signal_access_error STREQUAL "NOTFOUND"
               OR NOT _signal_access STREQUAL "public")
                message(FATAL_ERROR
                    "Registered Proxy ${_class_name} event signal ${_signal_name} must be public")
            endif()
            if(_signal_name IN_LIST _event_names)
                message(FATAL_ERROR
                    "Registered Proxy ${_class_name} has overloaded event signal ${_signal_name}; event signal names must be unique")
            endif()
            list(APPEND _event_names "${_signal_name}")
            math(EXPR _event_count "${_event_count} + 1")

            string(JSON _argument_count ERROR_VARIABLE _argument_count_error
                   LENGTH "${_moc_json}" classes ${_class_index} signals
                   ${_signal_index} arguments)
            if(NOT _argument_count_error STREQUAL "NOTFOUND")
                set(_argument_count 0)
            endif()

            set(_lambda_parameters)
            set(_signature_types)
            set(_variant_arguments)
            if(_argument_count GREATER 0)
                math(EXPR _last_argument "${_argument_count} - 1")
                foreach(_argument_index RANGE 0 ${_last_argument})
                    string(JSON _argument_type ERROR_VARIABLE _argument_type_error
                           GET "${_moc_json}" classes ${_class_index} signals
                           ${_signal_index} arguments ${_argument_index} type)
                    if(NOT _argument_type_error STREQUAL "NOTFOUND"
                       OR _argument_type STREQUAL "")
                        message(FATAL_ERROR
                            "Registered Proxy ${_class_name} event signal ${_signal_name} has an invalid argument type")
                    endif()

                    # moc JSON may omit argument names (for example `void ping(int)`).
                    # Generated lambdas only need a stable local identifier.
                    set(_argument_name "_proxyMirrorArgument${_argument_index}")

                    if(_lambda_parameters)
                        string(APPEND _lambda_parameters ", ")
                        string(APPEND _signature_types ",")
                        string(APPEND _variant_arguments ", ")
                    endif()
                    string(APPEND _lambda_parameters
                           "${_argument_type} ${_argument_name}")
                    string(APPEND _signature_types "${_argument_type}")
                    string(APPEND _variant_arguments
                           "QVariant::fromValue(${_argument_name})")
                endforeach()
            endif()

            set(_signal_signature
                "${_signal_name}(${_signature_types})")
            string(REPLACE "\\" "\\\\" _signal_signature
                           "${_signal_signature}")
            string(REPLACE "\"" "\\\"" _signal_signature
                           "${_signal_signature}")

            string(APPEND _generated_helpers
"    QObject::connect(&proxy, &${_class_name}::${_signal_name},
                     &lifetimeContext,
                     [handler](${_lambda_parameters}) {
        handler(QStringLiteral(\"${_signal_signature}\"),
                QVariantList{${_variant_arguments}});
    });
")
        endforeach()
    endif()

    if(_event_count EQUAL 0)
        string(APPEND _generated_helpers
"    Q_UNUSED(proxy);
    Q_UNUSED(lifetimeContext);
    Q_UNUSED(handler);
")
    endif()
    string(APPEND _generated_helpers "}\n\n")

    string(APPEND _generated_dispatch
"    if (proxy.metaObject() == &${_class_name}::staticMetaObject) {
        connectProxyMirrorEvents_${_class_key}(
            static_cast<${_class_name} &>(proxy), lifetimeContext, handler);
        if (errorMessage) {
            errorMessage->clear();
        }
        return true;
    }
")
endforeach()

string(CONCAT _generated
"// Generated from registered Proxy moc JSON. Do not edit by hand.\n"
"#include \"infrastructure/proxy_mirror/proxyrequestrelay.h\"\n")
foreach(_header_include IN LISTS _generated_includes)
    string(APPEND _generated "#include \"${_header_include}\"\n")
endforeach()
string(APPEND _generated
"\n#include <QObject>\n"
"#include <QVariant>\n\n"
"namespace {\n\n"
"${_generated_helpers}"
"} // namespace\n\n"
"bool connectProxyMirrorEvents(QObject &proxy,\n"
"                              QObject &lifetimeContext,\n"
"                              ProxyEventHandler handler,\n"
"                              QString *errorMessage)\n"
"{\n"
"    if (!handler) {\n"
"        if (errorMessage) {\n"
"            *errorMessage = QStringLiteral(\"Proxy event handler is empty.\");\n"
"        }\n"
"        return false;\n"
"    }\n"
"${_generated_dispatch}"
"    if (errorMessage) {\n"
"        *errorMessage = QStringLiteral(\"No generated Proxy event relay is registered for %1.\")\n"
"                            .arg(QString::fromLatin1(proxy.metaObject()->className()));\n"
"    }\n"
"    return false;\n"
"}\n")

get_filename_component(_output_directory "${OUTPUT_CPP}" DIRECTORY)
file(MAKE_DIRECTORY "${_output_directory}")
file(WRITE "${OUTPUT_CPP}" "${_generated}")

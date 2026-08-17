add_executable(WasmMirrorEngineTests
    tst_proxymirror.cpp
)
target_link_libraries(WasmMirrorEngineTests
    PRIVATE
        WasmMirror::Core
        Qt6::Test
)
set_target_properties(WasmMirrorEngineTests PROPERTIES AUTOMOC ON)
add_test(NAME WasmMirror.Engine COMMAND WasmMirrorEngineTests)
set_tests_properties(WasmMirror.Engine PROPERTIES
    TIMEOUT 15
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Test>"
)

add_executable(WasmMirrorRuntimeTests
    tst_wasmmirrorproxy.cpp
)
target_link_libraries(WasmMirrorRuntimeTests
    PRIVATE
        WasmMirror::Core
        Qt6::Test
        Qt6::WebSockets
)
set_target_properties(WasmMirrorRuntimeTests PROPERTIES AUTOMOC ON)
add_test(NAME WasmMirror.Runtime COMMAND WasmMirrorRuntimeTests)
set_tests_properties(WasmMirror.Runtime PROPERTIES
    TIMEOUT 15
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Test>"
)

cmake_minimum_required(VERSION 3.20)
project(fireball_core VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(fb_apply_strict_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /W4
            /WX
            $<$<COMPILE_LANGUAGE:C>:/std:c11>
        )
        target_compile_definitions(${target_name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# 1. Strict ABI compile checks (both C11 and C++)
# ---------------------------------------------------------------------------
add_library(fireball_abi_check_c11 OBJECT tests/abi_check_c11.c)
target_include_directories(fireball_abi_check_c11 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
fb_apply_strict_warnings(fireball_abi_check_c11)

add_library(fireball_abi_check_cpp OBJECT tests/abi_check_cpp.cpp)
target_include_directories(fireball_abi_check_cpp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
fb_apply_strict_warnings(fireball_abi_check_cpp)

# ---------------------------------------------------------------------------
# 2. Native fireball_core.dll
# ---------------------------------------------------------------------------
add_library(fireball_core SHARED
    src/core/fb_bundle.c
    src/core/fb_plugins.c
    src/core/fb_core.c
)
target_compile_definitions(fireball_core PRIVATE FB_BUILD_DLL)
target_include_directories(fireball_core
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core
)
set_target_properties(fireball_core PROPERTIES
    OUTPUT_NAME "fireball_core"
    PREFIX ""
)
fb_apply_strict_warnings(fireball_core)

# ---------------------------------------------------------------------------
# 3. Native fpv_toggle.fbplugin.dll
# ---------------------------------------------------------------------------
add_library(fpv_toggle_plugin SHARED
    examples/fpv_unlocker/native/fpv_toggle.c
)
target_compile_definitions(fpv_toggle_plugin PRIVATE FB_BUILD_DLL)
target_include_directories(fpv_toggle_plugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(fpv_toggle_plugin PROPERTIES
    OUTPUT_NAME "fpv_toggle.fbplugin"
    PREFIX ""
)
fb_apply_strict_warnings(fpv_toggle_plugin)

# ---------------------------------------------------------------------------
# 3b. Native time_weather.fbplugin.dll and state_scan.fbplugin.dll
# ---------------------------------------------------------------------------
add_library(time_weather_plugin SHARED
    examples/time_weather_control/native/time_weather.c
    examples/time_weather_control/native/time_weather_ui.c
)
target_compile_definitions(time_weather_plugin PRIVATE FB_BUILD_DLL)
target_include_directories(time_weather_plugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(time_weather_plugin PRIVATE comctl32 user32 gdi32)
set_target_properties(time_weather_plugin PROPERTIES
    OUTPUT_NAME "time_weather.fbplugin"
    PREFIX ""
)
fb_apply_strict_warnings(time_weather_plugin)

add_library(state_scan_plugin SHARED
    examples/state_scan/native/state_scan.c
)
target_compile_definitions(state_scan_plugin PRIVATE FB_BUILD_DLL)
target_include_directories(state_scan_plugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(state_scan_plugin PROPERTIES
    OUTPUT_NAME "state_scan.fbplugin"
    PREFIX ""
)
fb_apply_strict_warnings(state_scan_plugin)

add_executable(fireball_plugin_examples_test
    tests/native/plugin_examples_test.c
)
target_include_directories(fireball_plugin_examples_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_dependencies(fireball_plugin_examples_test time_weather_plugin state_scan_plugin)
fb_apply_strict_warnings(fireball_plugin_examples_test)

# ---------------------------------------------------------------------------
# 4. Native VIctory-style Mock Host (dynamically loads fireball_core.dll)
# ---------------------------------------------------------------------------
add_executable(fireball_mock_host
    tests/native/mock_host_test.c
)
target_include_directories(fireball_mock_host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
)
add_dependencies(fireball_mock_host
    fireball_core
    fpv_toggle_plugin
    fireball_abi_check_c11
    fireball_abi_check_cpp
)
fb_apply_strict_warnings(fireball_mock_host)

# ---------------------------------------------------------------------------
# 5. CTest integration
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    add_test(
        NAME fireball_mock_host_native
        COMMAND $<TARGET_FILE:fireball_mock_host>
            --core $<TARGET_FILE:fireball_core>
            --plugin $<TARGET_FILE:fpv_toggle_plugin>
    )
    add_test(
        NAME fireball_plugin_examples_native
        COMMAND $<TARGET_FILE:fireball_plugin_examples_test>
            --time-weather $<TARGET_FILE:time_weather_plugin>
            --state-scan $<TARGET_FILE:state_scan_plugin>
    )
endif()

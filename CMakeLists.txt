cmake_minimum_required(VERSION 3.24)
project(ProdMeshMix VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(PM_SANITIZE "Build with ASan + UBSan" OFF)
option(PM_UBSAN "Build with UBSan only" OFF)
option(PM_TSAN "Build with ThreadSanitizer" OFF)
option(PM_RTSAN "Build with RealtimeSanitizer (LLVM 20+, not Apple clang)" OFF)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter)
endif()
if(PM_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
if(PM_UBSAN)
    add_compile_options(-fsanitize=undefined -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=undefined)
endif()
if(PM_RTSAN)
    add_compile_options(-fsanitize=realtime -Wno-function-effects)
    add_compile_definitions(PM_RTSAN_ENABLED)
    add_link_options(-fsanitize=realtime)
endif()
if(PM_TSAN)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

# --- engine: headless, no Qt ---------------------------------------------
add_library(mix_engine STATIC
    src/core/rt_guard.cpp
    src/core/rt_thread.cpp
    src/engine/node.cpp
    src/engine/engine.cpp
    src/engine/scene.cpp
    src/engine/worker_pool.cpp
    src/driver/sim_driver.cpp
    src/record/wav_writer.cpp
    src/record/recorder.cpp
    src/plugin/plugin.cpp
    src/plugin/clap_host.cpp
    src/plugin/vst3_host.cpp
    src/plugin/sandbox.cpp
    src/plugin/sandbox_shm.cpp
    third_party/vst3sdk/pluginterfaces/base/funknown.cpp
    third_party/vst3sdk/pluginterfaces/base/coreiids.cpp
    third_party/vst3sdk/public.sdk/source/vst/vstinitiids.cpp
)
target_include_directories(mix_engine PUBLIC src)
target_include_directories(mix_engine SYSTEM PRIVATE
    third_party/clap/include third_party/vst3sdk)
# Third-party sources build without our warning set.
set_source_files_properties(
    third_party/vst3sdk/pluginterfaces/base/funknown.cpp
    third_party/vst3sdk/pluginterfaces/base/coreiids.cpp
    third_party/vst3sdk/public.sdk/source/vst/vstinitiids.cpp
    PROPERTIES COMPILE_OPTIONS "-w")
if(APPLE)
    target_sources(mix_engine PRIVATE src/driver/coreaudio_driver.cpp)
    target_link_libraries(mix_engine PUBLIC
        "-framework CoreAudio" "-framework CoreFoundation"
        "-framework AudioToolbox")
endif()

# --- tools ----------------------------------------------------------------
add_executable(mix-bench tools/mix_bench.cpp tools/bench_graph.cpp)
target_link_libraries(mix-bench PRIVATE mix_engine)

# --- tests ----------------------------------------------------------------
enable_testing()
add_executable(mix-tests
    tests/test_main.cpp
    tests/test_core.cpp
    tests/test_engine.cpp
    tests/test_routing.cpp
    tests/test_record.cpp
    tests/test_plugin.cpp
    tools/bench_graph.cpp
)
target_include_directories(mix-tests PRIVATE tests tools)
target_link_libraries(mix-tests PRIVATE mix_engine)
add_test(NAME mix-tests COMMAND mix-tests)

# --- daemon + protocol (Qt Core/Network; the engine itself stays Qt-free) --
find_package(Qt6 COMPONENTS Core Network Widgets)
if(Qt6_FOUND)
    qt_standard_project_setup()

    add_library(mix_proto STATIC src/proto/protocol.cpp)
    target_link_libraries(mix_proto PUBLIC mix_engine Qt6::Core)

    qt_add_executable(mixd src/daemon/main.cpp src/daemon/shows.cpp)
    target_link_libraries(mixd PRIVATE mix_proto Qt6::Network)
    set_target_properties(mixd PROPERTIES MACOSX_BUNDLE FALSE)

    add_executable(mix-proto-tests
        tests/test_main.cpp tests/test_proto.cpp
        src/daemon/shows.cpp tools/bench_graph.cpp)
    target_include_directories(mix-proto-tests PRIVATE tests tools)
    target_link_libraries(mix-proto-tests PRIVATE mix_proto)
    add_test(NAME mix-proto-tests COMMAND mix-proto-tests)
endif()

# --- UI ---------------------------------------------------------------------
if(Qt6_FOUND)
    qt_add_executable(mix-ui src/ui/main.cpp src/ui/widgets.h src/ui/theme.h
        src/ui/plugin_browser.h src/ui/scene_bar.h)
    target_link_libraries(mix-ui PRIVATE mix_proto Qt6::Widgets Qt6::Network)
    add_dependencies(mixd mix-plugin)  # mixd runs it to scan plugins
    set_target_properties(mix-ui PROPERTIES OUTPUT_NAME "ProdMeshMix")
    if(APPLE)
        set_target_properties(mix-ui PROPERTIES MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.prodmesh.mix"
            MACOSX_BUNDLE_BUNDLE_NAME "ProdMesh Mix")
    endif()
endif()

# --- test plugin (CLAP) -----------------------------------------------------
add_library(pm-test-clap MODULE tests/plugins/test_clap.cpp)
target_include_directories(pm-test-clap SYSTEM PRIVATE third_party/clap/include)
set_target_properties(pm-test-clap PROPERTIES OUTPUT_NAME "pm-test" PREFIX "")
if(APPLE)
    set_target_properties(pm-test-clap PROPERTIES BUNDLE TRUE
        BUNDLE_EXTENSION clap)
else()
    set_target_properties(pm-test-clap PROPERTIES SUFFIX ".clap")
endif()
add_dependencies(mix-tests pm-test-clap)
target_compile_definitions(mix-tests PRIVATE
    PM_TEST_CLAP="$<TARGET_BUNDLE_DIR:pm-test-clap>")

add_executable(mix-plugin tools/mix_plugin.cpp)
target_link_libraries(mix-plugin PRIVATE mix_engine)

add_executable(mix-pluginhost tools/mix_pluginhost.cpp)
target_link_libraries(mix-pluginhost PRIVATE mix_engine)
if(Qt6_FOUND)
    # Plugin editors live in the host process.
    target_link_libraries(mix-pluginhost PRIVATE Qt6::Widgets)
    target_compile_definitions(mix-pluginhost PRIVATE PM_HOST_GUI)
endif()
add_dependencies(mix-tests mix-pluginhost)

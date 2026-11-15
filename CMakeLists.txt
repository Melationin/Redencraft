cmake_minimum_required(VERSION 3.24)
project(redstone_cpp VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)
set(REDSTONE_ALLOCATOR "jemalloc" CACHE STRING "Allocation backend: jemalloc or system")
set_property(CACHE REDSTONE_ALLOCATOR PROPERTY STRINGS jemalloc system)
option(REDSTONE_FETCH_ZLIB "Fetch pinned zlib if not installed" ON)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
  if(NOT REDSTONE_FETCH_ZLIB)
    message(FATAL_ERROR "zlib required; provide ZLIB_ROOT or enable REDSTONE_FETCH_ZLIB")
  endif()
  include(FetchContent)
  set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib-github.tar.gz")
    set(zlib_url "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib-github.tar.gz")
  else()
    set(zlib_url "https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz")
  endif()
  FetchContent_Declare(zlib URL "${zlib_url}" URL_HASH SHA256=17e88863f3600672ab49182f217281b6fc4d3c762bde361935e436a95214d05c DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(zlib)
  add_library(ZLIB::ZLIB ALIAS zlibstatic)
  target_include_directories(zlibstatic PUBLIC "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
endif()
add_library(redstone_allocator OBJECT src/allocator.cpp)
target_include_directories(redstone_allocator PUBLIC include)
if(REDSTONE_ALLOCATOR STREQUAL "jemalloc")
  find_path(JEMALLOC_INCLUDE_DIR jemalloc/jemalloc.h)
  find_library(JEMALLOC_LIBRARY NAMES jemalloc jemalloc_s)
  if(NOT JEMALLOC_INCLUDE_DIR OR NOT JEMALLOC_LIBRARY)
    message(FATAL_ERROR "Default jemalloc backend requires jemalloc. Set CMAKE_PREFIX_PATH to its installation, or explicitly use -DREDSTONE_ALLOCATOR=system for comparisons.")
  endif()
  target_compile_definitions(redstone_allocator PUBLIC REDSTONE_USE_JEMALLOC)
  target_include_directories(redstone_allocator PUBLIC "${JEMALLOC_INCLUDE_DIR}")
  target_link_libraries(redstone_allocator PUBLIC "${JEMALLOC_LIBRARY}")
  if(WIN32)
    # jemalloc's generated Windows header defaults to dllimport even for
    # vcpkg's static package; its documented override is JEMALLOC_EXPORT.
    target_compile_definitions(redstone_allocator PUBLIC JEMALLOC_EXPORT=)
    target_link_libraries(redstone_allocator PUBLIC psapi)
  endif()
elseif(NOT REDSTONE_ALLOCATOR STREQUAL "system")
  message(FATAL_ERROR "Unknown allocator: ${REDSTONE_ALLOCATOR}")
endif()
add_library(redstone_core src/state.cpp src/blocks.cpp src/block_tags.cpp src/random_tick.cpp src/fluid.cpp src/fluid_flow.cpp src/light/optical.cpp src/light/snapshot.cpp src/light/common.cpp src/light/block_light_engine.cpp src/light/sky_light_engine.cpp src/light/service.cpp src/dimension.cpp src/poi.cpp src/server_worlds.cpp src/tickets.cpp src/heightmap.cpp src/engine.cpp src/entity.cpp src/rail.cpp src/piston.cpp src/containers.cpp src/redstone/signal.cpp src/redstone/wire.cpp src/redstone/torch.cpp src/redstone/diode.cpp src/redstone/controls.cpp src/redstone/consumers.cpp src/shapes.cpp src/nbt.cpp src/storage.cpp src/region.cpp src/terrain_codec.cpp src/entity_codec.cpp src/chunk_lifecycle.cpp src/world_chunk_manager.cpp src/litematic.cpp src/command.cpp vendor/lz4/lz4.c vendor/lz4/xxhash.c)
set_source_files_properties(src/light/optical.cpp PROPERTIES
  OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/light_metadata.inc;${CMAKE_CURRENT_SOURCE_DIR}/src/light_occlusion.inc")
target_include_directories(redstone_core PUBLIC include PRIVATE src vendor)
target_include_directories(redstone_core PUBLIC vendor/lz4)
target_link_libraries(redstone_core PUBLIC ZLIB::ZLIB Threads::Threads)
if(MSVC)
  target_compile_options(redstone_core PRIVATE /W4 /utf-8)
else()
  target_compile_options(redstone_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_executable(redstone_cli src/main.cpp)
target_link_libraries(redstone_cli PRIVATE redstone_core redstone_allocator)
option(REDSTONE_BUILD_BENCHMARKS "Build offline tick benchmarks" OFF)
option(REDSTONE_BENCHMARK_COUNT_ALLOCATIONS "Count global new allocations in the tick benchmark" OFF)
if(REDSTONE_BUILD_BENCHMARKS)
  add_executable(redstone_tick_benchmark benchmarks/tick_benchmark.cpp)
  target_link_libraries(redstone_tick_benchmark PRIVATE redstone_core)
  add_executable(redstone_fluid_perf_benchmark benchmarks/fluid_perf_benchmark.cpp)
  target_link_libraries(redstone_fluid_perf_benchmark PRIVATE redstone_core redstone_allocator)
  add_executable(redstone_block_light_benchmark benchmarks/block_light_benchmark.cpp)
  target_link_libraries(redstone_block_light_benchmark PRIVATE redstone_core redstone_allocator)
  add_executable(redstone_random_tick_benchmark benchmarks/random_tick_benchmark.cpp)
  target_link_libraries(redstone_random_tick_benchmark PRIVATE redstone_core redstone_allocator)
  if(REDSTONE_BENCHMARK_COUNT_ALLOCATIONS)
    add_library(redstone_counted_allocator OBJECT src/allocator.cpp)
    target_include_directories(redstone_counted_allocator PUBLIC include)
    target_compile_definitions(redstone_counted_allocator PUBLIC REDSTONE_COUNT_ALLOCATIONS)
    if(REDSTONE_ALLOCATOR STREQUAL "jemalloc")
      target_compile_definitions(redstone_counted_allocator PUBLIC REDSTONE_USE_JEMALLOC)
      target_include_directories(redstone_counted_allocator PUBLIC "${JEMALLOC_INCLUDE_DIR}")
      target_link_libraries(redstone_counted_allocator PUBLIC "${JEMALLOC_LIBRARY}")
      if(WIN32)
        target_compile_definitions(redstone_counted_allocator PUBLIC JEMALLOC_EXPORT=)
        target_link_libraries(redstone_counted_allocator PUBLIC psapi)
      endif()
    endif()
    target_link_libraries(redstone_tick_benchmark PRIVATE redstone_counted_allocator)
  else()
    target_link_libraries(redstone_tick_benchmark PRIVATE redstone_allocator)
  endif()
endif()
option(REDSTONE_BUILD_CLIENT "Build the lightweight Windows OpenGL debug client" ON)
if(WIN32 AND REDSTONE_BUILD_CLIENT)
  add_executable(redstone_client WIN32 src/client_win32.cpp)
  target_compile_definitions(redstone_client PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN REDSTONE_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
  target_link_libraries(redstone_client PRIVATE redstone_core redstone_allocator opengl32 gdi32 user32 shell32 ole32 windowscodecs)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/textures/block")
    add_custom_command(TARGET redstone_client POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:redstone_client>/assets")
  endif()
  if(MSVC)
    target_compile_options(redstone_client PRIVATE /W4 /utf-8)
  elseif(MINGW)
    target_link_options(redstone_client PRIVATE -municode)
  endif()
endif()
include(CTest)
option(REDSTONE_BUILD_VANILLA_SERVER "Build the optional protocol 775 native server" OFF)
if(REDSTONE_BUILD_VANILLA_SERVER)
  add_library(redstone_protocol src/net/wire.cpp src/net/session.cpp)
  target_link_libraries(redstone_protocol PUBLIC redstone_core)
  if(MSVC)
    target_compile_options(redstone_protocol PRIVATE /W4 /utf-8)
  else()
    target_compile_options(redstone_protocol PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_executable(redstone_vanilla_server src/net/server_main.cpp)
  target_link_libraries(redstone_vanilla_server PRIVATE redstone_protocol redstone_allocator)
  target_compile_definitions(redstone_vanilla_server PRIVATE REDSTONE_PROTOCOL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/protocol775" REDSTONE_ALLOCATOR_NAME="${REDSTONE_ALLOCATOR}")
  if(WIN32)
    target_link_libraries(redstone_vanilla_server PRIVATE ws2_32)
  endif()
  if(MSVC)
    target_compile_options(redstone_vanilla_server PRIVATE /W4 /utf-8)
  elseif(MINGW)
    target_link_options(redstone_vanilla_server PRIVATE -municode)
  endif()
  if(BUILD_TESTING)
    foreach(name wire protocol outbox light_protocol world_clock_protocol)
      add_executable(${name}_tests tests/${name}_tests.cpp)
      target_link_libraries(${name}_tests PRIVATE redstone_protocol redstone_allocator)
      if(MSVC)
        target_compile_options(${name}_tests PRIVATE /UNDEBUG /utf-8)
      else()
        target_compile_options(${name}_tests PRIVATE -UNDEBUG)
      endif()
      add_test(NAME ${name} COMMAND ${name}_tests "${CMAKE_CURRENT_SOURCE_DIR}/assets/protocol775")
    endforeach()
  endif()
endif()
if(BUILD_TESTING)
  add_executable(block_tags_tests tests/block_tags_tests.cpp)
  target_link_libraries(block_tags_tests PRIVATE redstone_core redstone_allocator)
  if(MSVC)
    target_compile_options(block_tags_tests PRIVATE /UNDEBUG /utf-8)
  else()
    target_compile_options(block_tags_tests PRIVATE -UNDEBUG)
  endif()
  add_test(NAME block_tags_lookup COMMAND block_tags_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/block26_1/block_tags.tsv"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/protocol775/blocks.tsv")
  add_executable(safesave_order_probe tests/safesave_order_probe.cpp)
  target_link_libraries(safesave_order_probe PRIVATE redstone_core redstone_allocator)
  # F-7 differential: the C++ half of the same fixture the vanilla harness runs.
  add_executable(fluid_differential_probe tests/fluid_differential_probe.cpp)
  target_link_libraries(fluid_differential_probe PRIVATE redstone_core redstone_allocator)
  add_executable(fluid_fixture_probe tests/fluid_fixture_probe.cpp)
  target_link_libraries(fluid_fixture_probe PRIVATE redstone_core redstone_allocator)
    add_executable(block_light_probe tests/block_light_probe.cpp)
    target_link_libraries(block_light_probe PRIVATE redstone_core redstone_allocator)
    add_executable(sky_light_probe tests/sky_light_probe.cpp)
    target_link_libraries(sky_light_probe PRIVATE redstone_core redstone_allocator)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME block_inventory COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/validation/vanilla26_1/check_block_inventory.py"
      "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME block_tags COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/validation/vanilla26_1/check_block_tags.py"
      "${CMAKE_CURRENT_SOURCE_DIR}")
    if(REDSTONE_BUILD_VANILLA_SERVER)
      add_test(NAME block_tags_server_smoke COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/validation/vanilla26_1/block_tags_server_smoke.py"
        "$<TARGET_FILE:redstone_vanilla_server>")
      add_test(NAME protocol_socket COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/validation/protocol775/socket_test.py"
        --server "$<TARGET_FILE:redstone_vanilla_server>")
      add_test(NAME protocol_backpressure COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/validation/protocol775/backpressure_test.py"
        --server "$<TARGET_FILE:redstone_vanilla_server>"
        --assets "${CMAKE_CURRENT_SOURCE_DIR}/assets/protocol775")
      set_tests_properties(protocol_backpressure PROPERTIES TIMEOUT 60)
    endif()
    add_test(NAME safesave_order_parity COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/validation/safesave_order/compare.py"
      --native "$<TARGET_FILE:safesave_order_probe>"
      --output "${CMAKE_CURRENT_BINARY_DIR}/safesave-order-comparison.json")
    # F-7 differential against the checked-in vanilla golden states. This is a
    # regression pin: it re-runs the C++ side of the same fixture and compares
    # to the recorded 26.1 output. It does NOT re-run the vanilla server.
    add_test(NAME fluid_differential COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/validation/vanilla26_1/fluid_differential_test.py"
      --probe "$<TARGET_FILE:fluid_differential_probe>"
      --golden "${CMAKE_CURRENT_SOURCE_DIR}/validation/vanilla26_1/fluid-vanilla-states.json")
    # M-01 real-server differential: the golden holds the transition and stable
    # endpoints produced by fluid_mixing_validation.py on official 26.1.
    add_test(NAME fluid_mixing_differential COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/validation/vanilla26_1/fluid_mixing_differential_test.py"
      --probe "$<TARGET_FILE:fluid_differential_probe>"
      --golden "${CMAKE_CURRENT_SOURCE_DIR}/validation/vanilla26_1/fluid-vanilla-mixing.json")
    # W-02, M-03 and the H-01 real-container slice. The checked-in golden is
    # produced by fluid_relative_validation.py against official 26.1; this test
    # only reruns the C++ half and checks every recorded cell at every tick.
    add_test(NAME fluid_fixture_differential COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/validation/vanilla26_1/fluid_fixture_differential_test.py"
      --probe "$<TARGET_FILE:fluid_fixture_probe>"
      --golden "${CMAKE_CURRENT_SOURCE_DIR}/validation/vanilla26_1/fluid-relative-golden.json")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/validation/vanilla26_1/block-light-golden.json")
      add_test(NAME block_light_differential COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/validation/vanilla26_1/block_light_differential_test.py"
        --probe "$<TARGET_FILE:block_light_probe>"
        --golden "${CMAKE_CURRENT_SOURCE_DIR}/validation/vanilla26_1/block-light-golden.json")
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/validation/vanilla26_1/sky-light-golden.json")
      add_test(NAME sky_light_differential COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/validation/vanilla26_1/sky_light_differential_test.py"
        --probe "$<TARGET_FILE:sky_light_probe>"
        --golden "${CMAKE_CURRENT_SOURCE_DIR}/validation/vanilla26_1/sky-light-golden.json")
    endif()
    add_test(NAME light_metadata_asset COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/validation/light_metadata/check.py"
      --tsv "${CMAKE_CURRENT_SOURCE_DIR}/assets/light26_1/block_light.tsv"
      --names "${CMAKE_CURRENT_SOURCE_DIR}/assets/light26_1/block_names.txt"
      --manifest "${CMAKE_CURRENT_SOURCE_DIR}/assets/light26_1/manifest.json"
      --source "${CMAKE_CURRENT_SOURCE_DIR}/src/light_metadata.inc"
      --faces "${CMAKE_CURRENT_SOURCE_DIR}/assets/light26_1/block_light_faces.tsv"
      --faces-source "${CMAKE_CURRENT_SOURCE_DIR}/src/light_occlusion.inc")
  endif()
  foreach(name dimension poi engine entity piston storage litematic allocator shapes client_inventory command fluid_tick fluid_flow fluid_params fluid_block heightmap tickets region terrain_codec entity_codec chunk_lifecycle world_chunk_manager world_clock redstone_components containers)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/${name}_tests.cpp")
      add_executable(${name}_tests tests/${name}_tests.cpp)
      target_link_libraries(${name}_tests PRIVATE redstone_core redstone_allocator)
      # Behavior assertions must remain active in Release, too, and the tests
      # get the same warning level as the core library so dead code in a test
      # is reported instead of silently kept.
      if(MSVC)
        target_compile_options(${name}_tests PRIVATE /UNDEBUG /W4 /utf-8)
      else()
        target_compile_options(${name}_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
      endif()
      if(name STREQUAL "region")
        add_test(NAME ${name} COMMAND ${name}_tests
                 "${CMAKE_CURRENT_BINARY_DIR}/region-test-scratch")
      elseif(name STREQUAL "terrain_codec")
        add_test(NAME ${name} COMMAND ${name}_tests
                 "${CMAKE_CURRENT_BINARY_DIR}/terrain-codec-test-scratch")
      elseif(name STREQUAL "chunk_lifecycle")
        add_test(NAME ${name} COMMAND ${name}_tests
                 "${CMAKE_CURRENT_BINARY_DIR}/chunk-lifecycle-test-scratch")
      elseif(name STREQUAL "world_chunk_manager")
        add_test(NAME ${name} COMMAND ${name}_tests
                 "${CMAKE_CURRENT_BINARY_DIR}/world-chunk-manager-test-scratch")
      elseif(name STREQUAL "poi")
        add_test(NAME ${name} COMMAND ${name}_tests
                 "${CMAKE_CURRENT_BINARY_DIR}/poi-test-scratch")
      else()
        add_test(NAME ${name} COMMAND ${name}_tests)
      endif()
    endif()
  endforeach()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/light_tests.cpp")
    add_executable(light_tests tests/light_tests.cpp)
    target_link_libraries(light_tests PRIVATE redstone_core redstone_allocator)
    if(MSVC)
      target_compile_options(light_tests PRIVATE /UNDEBUG /W4 /utf-8)
    else()
      target_compile_options(light_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME light COMMAND light_tests "${CMAKE_CURRENT_SOURCE_DIR}/assets/light26_1")
    set_tests_properties(light PROPERTIES TIMEOUT 60)
  endif()
  # fluid_tests reads the exported 26.1 block-state table to pin the water/lava
  # protocol ids, so it takes the assets directory as its argument. The timeout
  # turns the FluidState::getAmount() dispatch recursion into a failure instead
  # of a hung run, and /W4 keeps it as strict as the core library.
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fluid_tests.cpp")
    add_executable(fluid_tests tests/fluid_tests.cpp)
    target_link_libraries(fluid_tests PRIVATE redstone_core redstone_allocator)
    if(MSVC)
      target_compile_options(fluid_tests PRIVATE /UNDEBUG /W4 /utf-8)
    else()
      target_compile_options(fluid_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME fluid COMMAND fluid_tests "${CMAKE_CURRENT_SOURCE_DIR}/assets/protocol775")
    set_tests_properties(fluid PROPERTIES TIMEOUT 60)
  endif()
  # The clientbound command tree is encoded by the protocol library, so the
  # command tests exercise it only when the optional server is part of the build.
  if(TARGET redstone_protocol AND TARGET command_tests)
    target_link_libraries(command_tests PRIVATE redstone_protocol)
    target_compile_definitions(command_tests PRIVATE REDSTONE_HAVE_PROTOCOL=1)
  endif()
endif()

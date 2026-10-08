cmake_minimum_required(VERSION 3.28)
project(BreadCamera LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
add_compile_options(/W4 /permissive- /utf-8 /EHsc)
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)

find_package(FFMPEG REQUIRED)
find_package(ixwebsocket CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(doctest CONFIG REQUIRED)
find_package(unofficial-webview2 CONFIG REQUIRED)
find_package(wil CONFIG REQUIRED)

# Optional SDKs (spec §7.2)
find_package(NDI)
option(BC_WITH_NDI "Build the NDI adapters" ${NDI_FOUND})
if(BC_WITH_NDI AND NOT NDI_FOUND)
  message(WARNING "BC_WITH_NDI=ON but the NDI SDK was not found; turning it off")
  set(BC_WITH_NDI OFF)
endif()
message(STATUS "BC_WITH_NDI=${BC_WITH_NDI}")

# ---- core (no GPU) ----
add_library(bc_core STATIC
  src/core/rate.cpp
  # Task 2
  src/core/tick_schedule.cpp
  src/core/audio_delay.cpp
  # Task 3
  src/core/secrets.cpp
  src/core/show.cpp
  src/core/show_store.cpp
  # Task 4
  src/core/signal_state.cpp
  src/core/state_store.cpp
)
target_include_directories(bc_core PUBLIC src)
target_link_libraries(bc_core PUBLIC nlohmann_json::nlohmann_json)
target_link_libraries(bc_core PUBLIC crypt32)

# ---- gpu ----
find_program(FXC fxc REQUIRED)  # Windows SDK; on PATH in the VS developer shell
set(BC_SHADER_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${BC_SHADER_DIR})
set(BC_SHADER_HEADERS)
function(bc_shader name profile)
  set(src ${CMAKE_SOURCE_DIR}/src/gpu/shaders/${name}.hlsl)
  set(out ${BC_SHADER_DIR}/${name}.h)
  add_custom_command(OUTPUT ${out}
    COMMAND ${FXC} /nologo /O3 /T ${profile} /E main /Vn g_${name} /Fh ${out} ${src}
    DEPENDS ${src} VERBATIM)
  set(BC_SHADER_HEADERS ${BC_SHADER_HEADERS} ${out} PARENT_SCOPE)
endfunction()
bc_shader(yuv_to_linear cs_5_0)
bc_shader(bgra_to_linear cs_5_0)
bc_shader(linear_to_bgra8 cs_5_0)
# Task 7 adds fullscreen_vs (vs_5_0) and fullscreen_ps (ps_5_0) here

add_library(bc_gpu STATIC
  src/gpu/device.cpp
  src/gpu/texture_pool.cpp
  src/gpu/converter.cpp
  src/gpu/image_io.cpp
  ${BC_SHADER_HEADERS}
)
target_include_directories(bc_gpu PUBLIC src ${BC_SHADER_DIR})
target_link_libraries(bc_gpu PUBLIC bc_core d3d11 dxgi windowscodecs)

# ---- tests ----
add_executable(bc_tests
  tests/main.cpp
  tests/support/requires.cpp
  tests/rate_test.cpp
  tests/tick_schedule_test.cpp
  tests/frame_sync_test.cpp
  tests/bounded_queue_test.cpp
  tests/audio_delay_test.cpp
  tests/show_test.cpp
  tests/show_store_test.cpp
  tests/state_store_test.cpp
  # Task 5
  tests/gpu_test.cpp
  # Task 2..14 test files
)
target_include_directories(bc_tests PRIVATE tests)
target_link_libraries(bc_tests PRIVATE bc_core bc_gpu doctest::doctest)
target_compile_definitions(bc_tests PRIVATE BC_WITH_NDI=$<BOOL:${BC_WITH_NDI}>)

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

find_package(Maxine)
option(BC_WITH_MAXINE "Build the Maxine matte backend" ${Maxine_FOUND})
if(BC_WITH_MAXINE AND NOT Maxine_FOUND)
  message(WARNING "BC_WITH_MAXINE=ON but the NVIDIA Video Effects SDK was not found; turning it off")
  set(BC_WITH_MAXINE OFF)
endif()
message(STATUS "BC_WITH_MAXINE=${BC_WITH_MAXINE}")

# M3 Task 5 — optional chart detection (OpenCV mcc)
find_package(OpenCV CONFIG QUIET COMPONENTS core imgproc mcc)
if(OpenCV_FOUND)
  set(BC_WITH_CHART_DETECT ON)
else()
  set(BC_WITH_CHART_DETECT OFF)
  message(STATUS "Chart detection OFF: OpenCV with the mcc module not found (manual corners still work)")
endif()
message(STATUS "BC_WITH_CHART_DETECT=${BC_WITH_CHART_DETECT}")

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
  # Task 1
  src/core/stage_params.cpp
  # M2 Task 2
  src/core/stage_guard.cpp
  # Task 4
  src/core/signal_state.cpp
  src/core/state_store.cpp
  # M3 Task 1
  src/core/color_math.cpp
  src/core/lut.cpp
  # M3 Task 2
  src/core/chart.cpp
  src/core/calibration.cpp
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
# Task 11A
bc_shader(fit_linear cs_5_0)
# Task 7
bc_shader(fullscreen_vs vs_5_0)
bc_shader(fullscreen_ps ps_5_0)
# Task 3
foreach(s alpha_threshold alpha_morph alpha_blur bg_downsample bg_blur bg_composite)
  bc_shader(${s} cs_5_0)
endforeach()
# M3 Task 3
bc_shader(color_grade cs_5_0)

add_library(bc_gpu STATIC
  src/gpu/device.cpp
  src/gpu/texture_pool.cpp
  src/gpu/converter.cpp
  src/gpu/image_io.cpp
  src/gpu/compositor.cpp
  # M3 Task 3
  src/gpu/color_grader.cpp
  ${BC_SHADER_HEADERS}
)
target_include_directories(bc_gpu PUBLIC src ${BC_SHADER_DIR})
target_link_libraries(bc_gpu PUBLIC bc_core d3d11 dxgi windowscodecs)

# ---- io ----
add_library(bc_io STATIC
  src/io/srt_params.cpp
  src/io/ffmpeg_input.cpp
  # Task 7
  src/io/queued_output.cpp
  src/io/monitors.cpp
  src/io/fullscreen_output.cpp
  # Task 8
)
target_include_directories(bc_io PUBLIC src ${FFMPEG_INCLUDE_DIRS})
target_link_directories(bc_io PUBLIC ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(bc_io PUBLIC bc_gpu ${FFMPEG_LIBRARIES} srt)
target_link_libraries(bc_io PUBLIC user32)

if(BC_WITH_NDI)
  target_sources(bc_io PRIVATE src/io/ndi_output.cpp)
  target_link_libraries(bc_io PUBLIC NDI::NDI)
  target_compile_definitions(bc_io PUBLIC BC_WITH_NDI=1)
else()
  target_compile_definitions(bc_io PUBLIC BC_WITH_NDI=0)
endif()

# Copies runtime DLLs that vcpkg does not manage (NDI) next to an executable.
function(bc_copy_runtime target)
  if(BC_WITH_NDI)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NDI_DLL}" "$<TARGET_FILE_DIR:${target}>")
  endif()
endfunction()

# ---- engine ----
add_library(bc_engine STATIC
  src/engine/adapter_factory.cpp
  src/engine/camera.cpp
  src/engine/engine.cpp
  # Task 10
  src/engine/preview_encoder.cpp
  src/engine/preview_hub.cpp
  src/engine/preview_service.cpp
  # Task 11
  src/engine/command_router.cpp
  src/engine/control_server.cpp
  # M2 Task 4
  src/engine/matte.cpp
  # M3 Task 5
  src/engine/chart_detector.cpp
)
target_include_directories(bc_engine PUBLIC src)
target_link_libraries(bc_engine PUBLIC bc_io winmm)
target_link_libraries(bc_engine PUBLIC ixwebsocket::ixwebsocket bcrypt)

if(BC_WITH_MAXINE)
  target_sources(bc_engine PRIVATE src/engine/maxine_matte.cpp ${MAXINE_VFX_PROXY} ${MAXINE_IMAGE_PROXY})
  target_include_directories(bc_engine PUBLIC ${MAXINE_INCLUDE_DIR} ${MAXINE_GREENSCREEN_INCLUDE_DIR})
  target_compile_definitions(bc_engine PUBLIC BC_WITH_MAXINE=1 BC_MAXINE_ROOT="${MAXINE_ROOT}")
else()
  target_compile_definitions(bc_engine PUBLIC BC_WITH_MAXINE=0)
endif()

target_compile_definitions(bc_engine PUBLIC BC_WITH_CHART_DETECT=$<BOOL:${BC_WITH_CHART_DETECT}>)
if(BC_WITH_CHART_DETECT)
  target_link_libraries(bc_engine PUBLIC opencv_core opencv_imgproc opencv_mcc)
endif()

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
  # Task 1
  tests/stage_params_test.cpp
  # M2 Task 2
  tests/stage_guard_test.cpp
  # Task 3
  tests/compositor_test.cpp
  # Task 5
  tests/gpu_test.cpp
  # Task 6
  tests/srt_params_test.cpp
  tests/ffmpeg_input_test.cpp
  tests/support/test_media.cpp
  # Task 7
  tests/queued_output_test.cpp
  tests/fullscreen_output_test.cpp
  # Task 8
  tests/ndi_output_test.cpp
  # Task 2..14 test files
  # Task 9
  tests/engine_test.cpp
  # Task 10
  tests/preview_test.cpp
  # Task 11
  tests/command_router_test.cpp
  tests/control_server_test.cpp
  # Task 11A
  tests/input_health_test.cpp
  # Task 12
  tests/supervisor_test.cpp
  # Task 14
  tests/e2e_m1_test.cpp
  # M2 Task 4
  tests/engine_matte_test.cpp
  # M2 Task 5
  tests/maxine_matte_test.cpp
  # M2 Task 7
  tests/e2e_m2_test.cpp
  # M3 Task 1
  tests/color_math_test.cpp
  tests/lut_test.cpp
  # M3 Task 2
  tests/chart_test.cpp
  tests/calibration_test.cpp
  # M3 Task 3
  tests/color_grader_test.cpp
  # M3 Task 4
  tests/engine_color_test.cpp
  # M3 Task 5
  tests/chart_detector_test.cpp
)
target_include_directories(bc_tests PRIVATE tests)
target_link_libraries(bc_tests PRIVATE bc_core bc_gpu bc_io bc_engine bc_shell doctest::doctest)
bc_copy_runtime(bc_tests)
# The M1 acceptance test launches the engine exe from the test binary's folder.
add_dependencies(bc_tests breadcamera-engine)

# ---- engine executable (Task 11) ----
add_executable(breadcamera-engine src/engine/engine_main.cpp)
target_link_libraries(breadcamera-engine PRIVATE bc_engine shell32 ole32)
bc_copy_runtime(breadcamera-engine)

# ---- shell ----
add_library(bc_shell STATIC src/shell/supervisor.cpp src/shell/process_launcher.cpp)
target_include_directories(bc_shell PUBLIC src)
target_link_libraries(bc_shell PUBLIC bc_core)

add_executable(BreadCamera WIN32 src/shell/shell_main.cpp)
target_link_libraries(BreadCamera PRIVATE bc_shell bc_engine unofficial::webview2::webview2 WIL::WIL shell32 ole32 user32 dwmapi)

add_custom_command(TARGET BreadCamera POST_BUILD
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}/ui/dist -DDST=$<TARGET_FILE_DIR:BreadCamera>/ui
          -P ${CMAKE_SOURCE_DIR}/cmake/CopyUi.cmake)

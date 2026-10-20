cmake_minimum_required(VERSION 3.28)
project(BreadCamera LANGUAGES C CXX)

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

# M4b Task 1 — optional DeckLink SDI/HDMI adapters (spec §7.2).
find_package(DeckLink)
option(BC_WITH_DECKLINK "Build the DeckLink SDI/HDMI adapters" ${DeckLink_FOUND})
if(BC_WITH_DECKLINK AND NOT DeckLink_FOUND)
  message(WARNING "BC_WITH_DECKLINK=ON but the DeckLink SDK was not found; turning it off")
  set(BC_WITH_DECKLINK OFF)
endif()
message(STATUS "BC_WITH_DECKLINK=${BC_WITH_DECKLINK}")

# M4b Task 2 — optional AJA NTV2 SDI adapters (experimental, spec §4.2/§4.3). libajantv2 is
# fetched from GitHub; a configure time-out/failure behaves like a missing SDK. Default OFF
# because libajantv2 18.0's own ajabase/common/dpx_hdr.cpp does not compile with the Windows
# SDK on this machine (VS 2022 + recent Win10/11 SDK):

#   dpx_hdr.cpp(38): error C2556: 'float htonf(float)': overloaded function differs only by
#   return type from 'unsigned int htonf(float)'
#   dpx_hdr.cpp(38): error C2371: 'htonf': redefinition; different basic types

# (its `inline float htonf(float)` collides with the SDK's `htonf`). Not caused by our
# /W4 /permissive- flags — it reproduces with a bare /W3 compile. Pass -DBC_WITH_AJA=ON to
# try again; the adapters compile cleanly once the library builds.
option(BC_WITH_AJA "Build the AJA adapters (experimental)" OFF)
if(BC_WITH_AJA)
  include(FetchContent)
  set(AJANTV2_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  foreach(o DEMOS TESTS TOOLS DRIVER PLUGIN_LOAD RDMA)
    set(AJANTV2_DISABLE_${o} ON CACHE BOOL "" FORCE)
  endforeach()
  FetchContent_Declare(libajantv2 GIT_REPOSITORY https://github.com/aja-video/libajantv2.git GIT_TAG ntv2_18_0_0 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(libajantv2)
  if(NOT TARGET ajantv2)
    message(WARNING "libajantv2 did not provide the ajantv2 target; turning AJA off")
    set(BC_WITH_AJA OFF)
  endif()
endif()
message(STATUS "BC_WITH_AJA=${BC_WITH_AJA}")

# M4b Task 3 — Windows 11 virtual camera (spec §4.3). The media-source DLL is built from the
# vendored VCamSample source (vcam/); the engine output registers it with MFCreateVirtualCamera.
# mfvirtualcamera.h ships in the Windows SDK 10.0.22000+; this machine has the 10.0.26100.0 SDK
# (mfvirtualcamera.h and the mfsensorgroup.lib import library are both present).
include(CheckIncludeFileCXX)
check_include_file_cxx(mfvirtualcamera.h HAVE_MFVCAM)
option(BC_WITH_VCAM "Build the virtual camera" ${HAVE_MFVCAM})
if(BC_WITH_VCAM AND NOT HAVE_MFVCAM)
  message(WARNING "BC_WITH_VCAM=ON but mfvirtualcamera.h was not found; turning it off")
  set(BC_WITH_VCAM OFF)
endif()
# C++/WinRT (winrt/base.h) ships with the Windows SDK; locate it from the SDK's mt.exe path
# (.../Windows Kits/10/bin/<ver>/x64/mt.exe) so this works with the Ninja generator too.
if(CMAKE_MT)
  get_filename_component(_bc_mt_dir "${CMAKE_MT}" DIRECTORY)          # .../bin/<ver>/x64
  get_filename_component(_bc_mt_ver_dir "${_bc_mt_dir}" DIRECTORY)    # .../bin/<ver>
  get_filename_component(_bc_sdk_ver "${_bc_mt_ver_dir}" NAME)        # <ver>
  get_filename_component(_bc_sdk_bin "${_bc_mt_ver_dir}" DIRECTORY)   # .../bin
  get_filename_component(_bc_sdk_root "${_bc_sdk_bin}" DIRECTORY)     # .../Windows Kits/10
  set(BC_WINDOWS_SDK_CPPWINRT "${_bc_sdk_root}/Include/${_bc_sdk_ver}/cppwinrt")
endif()
message(STATUS "BC_WITH_VCAM=${BC_WITH_VCAM}")
if(BC_WITH_VCAM)
  add_subdirectory(vcam)
endif()

# Optional OMT-HX output (see ../OMT-HX). Its CMake builds libomt and libvmx from its omt/ submodules,
# so the checkout needs `git submodule update --init` once.
set(OMTHX_ROOT "${CMAKE_SOURCE_DIR}/../OMT-HX" CACHE PATH "OMT-HX checkout")
if(EXISTS "${OMTHX_ROOT}/include/omthx/omthx.h" AND EXISTS "${OMTHX_ROOT}/omt/libomt/libomt.h")
  add_subdirectory("${OMTHX_ROOT}" omthx-build)
  set(BC_WITH_OMT ON)
else()
  set(BC_WITH_OMT OFF)
  message(STATUS "OMT output OFF: no OMT-HX checkout with its submodules at ${OMTHX_ROOT}")
endif()
message(STATUS "BC_WITH_OMT=${BC_WITH_OMT}")

find_package(Maxine)
option(BC_WITH_MAXINE "Build the Maxine matte backend" ${Maxine_FOUND})
if(BC_WITH_MAXINE AND NOT Maxine_FOUND)
  message(WARNING "BC_WITH_MAXINE=ON but the NVIDIA Video Effects SDK was not found; turning it off")
  set(BC_WITH_MAXINE OFF)
endif()
message(STATUS "BC_WITH_MAXINE=${BC_WITH_MAXINE}")

# webcam-fx Task 4 — optional NVIDIA RTX Video SDK (Clean up and AI Upscale, spec D4). The SDK is
# behind an NVIDIA Developer login, so it is never fetched or committed (spec "RTX Video SDK
# licence obligations", §4b/§4e); BC_WITH_RTXVIDEO finds it under third_party/rtx_video_sdk or
# turns itself off, and CI builds without it.
find_package(RtxVideo)
option(BC_WITH_RTXVIDEO "Build the NVIDIA RTX Video SDK Clean up and AI Upscale" ${RtxVideo_FOUND})
if(BC_WITH_RTXVIDEO AND NOT RtxVideo_FOUND)
  message(WARNING "BC_WITH_RTXVIDEO=ON but the NVIDIA RTX Video SDK was not found under RTXVIDEO_ROOT; turning it off")
  set(BC_WITH_RTXVIDEO OFF)
endif()
message(STATUS "BC_WITH_RTXVIDEO=${BC_WITH_RTXVIDEO}")

# M6 Task 1 — optional ONNX Runtime (DirectML) matte backend (spec §7.2). The runtime, the
# DirectML redistributable and the MODNet model are fetched by scripts/fetch-sdks.ps1.
find_package(OnnxRuntime)
option(BC_WITH_ONNX "Build the ONNX Runtime (DirectML) matte backend" ${OnnxRuntime_FOUND})
if(BC_WITH_ONNX AND NOT OnnxRuntime_FOUND)
  message(WARNING "BC_WITH_ONNX=ON but ONNX Runtime / the MODNet model were not found (run scripts/fetch-sdks.ps1); turning it off")
  set(BC_WITH_ONNX OFF)
endif()
message(STATUS "BC_WITH_ONNX=${BC_WITH_ONNX}")

# C1 — the shared LiveKit client (spec D1/§7). libdatachannel comes from vcpkg;
# a missing library turns the client off and its code compiles out.
find_package(LibDataChannel)
option(BC_WITH_LIVECALL "Build the shared LiveKit client" ${LibDataChannel_FOUND})
if(BC_WITH_LIVECALL AND NOT LibDataChannel_FOUND)
  message(WARNING "BC_WITH_LIVECALL=ON but libdatachannel was not found; turning it off")
  set(BC_WITH_LIVECALL OFF)
endif()
message(STATUS "BC_WITH_LIVECALL=${BC_WITH_LIVECALL}")

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
  src/core/rate_estimator.cpp
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
  # webcam-fx Task 6
  src/core/skin_match.cpp
  # M3 Task 2
  src/core/chart.cpp
  src/core/calibration.cpp
  # M4b Task 3 (always built)
  src/core/vcam_shared.cpp
)
target_include_directories(bc_core PUBLIC src)
target_link_libraries(bc_core PUBLIC nlohmann_json::nlohmann_json)
target_link_libraries(bc_core PUBLIC crypt32)
target_link_libraries(bc_core PUBLIC advapi32)

# ---- gpu ----
find_program(FXC fxc REQUIRED)  # Windows SDK; on PATH in the VS developer shell
set(BC_SHADER_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${BC_SHADER_DIR})
set(BC_SHADER_HEADERS)
function(bc_shader name profile)
  set(src ${CMAKE_SOURCE_DIR}/src/gpu/shaders/${name}.hlsl)
  set(out ${BC_SHADER_DIR}/${name}.h)
  # Any extra arguments are header dependencies the shader #includes (e.g. the vendored FSR headers).
  add_custom_command(OUTPUT ${out}
    COMMAND ${FXC} /nologo /O3 /T ${profile} /E main /Vn g_${name} /Fh ${out} ${src}
    DEPENDS ${src} ${ARGN} VERBATIM)
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
# webcam-fx Task 3 — the FSR 1 upscaler's two compute shaders; the vendored headers are dependencies
set(BC_FSR_HEADERS ${CMAKE_SOURCE_DIR}/src/gpu/shaders/ffx/ffx_a.h ${CMAKE_SOURCE_DIR}/src/gpu/shaders/ffx/ffx_fsr1.h)
bc_shader(fsr_easu cs_5_0 ${BC_FSR_HEADERS})
bc_shader(fsr_rcas cs_5_0 ${BC_FSR_HEADERS})

add_library(bc_gpu STATIC
  src/gpu/device.cpp
  src/gpu/texture_pool.cpp
  src/gpu/converter.cpp
  src/gpu/readback.cpp
  src/gpu/image_io.cpp
  src/gpu/compositor.cpp
  # M3 Task 3
  src/gpu/color_grader.cpp
  # webcam-fx Task 1
  src/gpu/gpu_timer.cpp
  # webcam-fx Task 3
  src/gpu/upscaler.cpp
  # webcam-fx Task 4 (compiles to a null path when BC_WITH_RTXVIDEO=0)
  src/gpu/rtx_video.cpp
  ${BC_SHADER_HEADERS}
)
target_include_directories(bc_gpu PUBLIC src ${BC_SHADER_DIR})
target_link_libraries(bc_gpu PUBLIC bc_core d3d11 dxgi windowscodecs)
if(BC_WITH_RTXVIDEO)
  target_include_directories(bc_gpu PUBLIC ${RTXVIDEO_INCLUDE_DIR})
  target_link_libraries(bc_gpu PUBLIC "${RTXVIDEO_NGX_LIB}")
  target_compile_definitions(bc_gpu PUBLIC BC_WITH_RTXVIDEO=1)
else()
  target_compile_definitions(bc_gpu PUBLIC BC_WITH_RTXVIDEO=0)
endif()

# ---- io ----
add_library(bc_io STATIC
  src/io/srt_params.cpp
  src/io/ffmpeg_input.cpp
  # M4b Task 1 (always built)
  src/io/uyvy.cpp
  # Task 7
  src/io/queued_output.cpp
  src/io/monitors.cpp
  src/io/fullscreen_output.cpp
  # Task 8
  # M4a Task 1
  src/io/h264_encoder.cpp
  src/io/srt_output.cpp
  # M4a Task 4
  src/io/mf_capture_input.cpp
  # DirectShow camera input (OBS Virtual Camera)
  src/io/dshow_devices.cpp
  src/io/dshow_format.cpp
  src/io/dshow_capture_input.cpp
  # C1 Task 1
  src/io/lkc/lkc.cpp
  # C1 Task 3
  src/io/breadcall_input.cpp
)
target_include_directories(bc_io PUBLIC src ${FFMPEG_INCLUDE_DIRS})
target_link_directories(bc_io PUBLIC ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(bc_io PUBLIC bc_gpu ${FFMPEG_LIBRARIES} srt)
target_link_libraries(bc_io PUBLIC user32)
target_link_libraries(bc_io PUBLIC mf mfplat mfreadwrite mfuuid ole32)

# C1 Task 1 — the shared LiveKit client. When libdatachannel is absent the
# client still compiles (BC_WITH_LIVECALL=0) and reports `lkc-unavailable`.
if(BC_WITH_LIVECALL)
  target_link_libraries(bc_io PUBLIC LibDataChannel::LibDataChannel)
  target_compile_definitions(bc_io PUBLIC BC_WITH_LIVECALL=1)
else()
  target_compile_definitions(bc_io PUBLIC BC_WITH_LIVECALL=0)
endif()

if(BC_WITH_NDI)
  # M4a Task 2
  target_sources(bc_io PRIVATE src/io/ndi_output.cpp src/io/ndi_discovery.cpp src/io/ndi_input.cpp)
  target_link_libraries(bc_io PUBLIC NDI::NDI)
  target_compile_definitions(bc_io PUBLIC BC_WITH_NDI=1)
else()
  target_compile_definitions(bc_io PUBLIC BC_WITH_NDI=0)
endif()

if(BC_WITH_OMT)
  target_sources(bc_io PRIVATE src/io/omt_output.cpp)
  target_link_libraries(bc_io PUBLIC omthx)
  target_compile_definitions(bc_io PUBLIC BC_WITH_OMT=1)
else()
  target_compile_definitions(bc_io PUBLIC BC_WITH_OMT=0)
endif()

# M4b Task 1 — DeckLink. The SDK ships an IDL (no import library), so MIDL compiles
# DeckLinkAPI.idl into DeckLinkAPI_h.h + DeckLinkAPI_i.c, which the adapters use through
# CoCreateInstance.
if(BC_WITH_DECKLINK)
  set(_dlgen ${CMAKE_BINARY_DIR}/decklink)
  file(MAKE_DIRECTORY ${_dlgen})
  get_filename_component(_dlinc ${DECKLINK_IDL} DIRECTORY)
  add_custom_command(
    OUTPUT ${_dlgen}/DeckLinkAPI_h.h ${_dlgen}/DeckLinkAPI_i.c
    COMMAND midl /nologo /env x64 /I ${_dlinc} /h DeckLinkAPI_h.h /iid DeckLinkAPI_i.c /out ${_dlgen} ${DECKLINK_IDL}
    DEPENDS ${DECKLINK_IDL} VERBATIM)
  set(_dl_sources src/io/decklink.cpp src/io/decklink_input.cpp src/io/decklink_output.cpp)
  # List the generated header as a source AND give each DeckLink TU an explicit OBJECT_DEPENDS
  # edge on it. The header otherwise reaches the compiles only through the include directory,
  # so Ninja has no edge from the MIDL step to the .cpp compiles and can start them in parallel
  # ("cannot open DeckLinkAPI_h.h").
  target_sources(bc_io PRIVATE ${_dlgen}/DeckLinkAPI_i.c ${_dlgen}/DeckLinkAPI_h.h ${_dl_sources})
  set_source_files_properties(${_dl_sources} PROPERTIES OBJECT_DEPENDS ${_dlgen}/DeckLinkAPI_h.h)
  target_include_directories(bc_io PUBLIC ${_dlgen})
  target_compile_definitions(bc_io PUBLIC BC_WITH_DECKLINK=1)
else()
  target_compile_definitions(bc_io PUBLIC BC_WITH_DECKLINK=0)
endif()

# M4b Task 2 — AJA. Fill-only output (spec §4.3 deviation); keying is a follow-up.
if(BC_WITH_AJA)
  target_sources(bc_io PRIVATE src/io/aja.cpp src/io/aja_input.cpp src/io/aja_output.cpp)
  target_link_libraries(bc_io PUBLIC ajantv2)
  target_compile_definitions(bc_io PUBLIC BC_WITH_AJA=1)
else()
  target_compile_definitions(bc_io PUBLIC BC_WITH_AJA=0)
endif()

# M4b Task 3 — the Windows 11 virtual-camera output. The import library provides
# MFCreateVirtualCamera (10.0.26100.0 SDK); missing it would be a link error, not a silent OFF.
if(BC_WITH_VCAM)
  target_sources(bc_io PRIVATE src/io/vcam_output.cpp)
  target_link_libraries(bc_io PUBLIC mfsensorgroup)
  target_compile_definitions(bc_io PUBLIC BC_WITH_VCAM=1)
else()
  target_compile_definitions(bc_io PUBLIC BC_WITH_VCAM=0)
endif()

# Copies runtime DLLs that vcpkg does not manage (NDI) next to an executable.
function(bc_copy_runtime target)
  if(BC_WITH_NDI)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NDI_DLL}" "$<TARGET_FILE_DIR:${target}>")
  endif()
  if(BC_WITH_OMT)
    omthx_copy_runtime(${target})
  endif()
  if(BC_WITH_ONNX)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ORT_DLL}" "${DML_DLL}" "$<TARGET_FILE_DIR:${target}>"
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/models"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MODNET_ONNX}" "$<TARGET_FILE_DIR:${target}>/models/modnet.onnx")
  endif()
  if(BC_WITH_RTXVIDEO)
    # webcam-fx Task 4 — the RTX Video SDK's runtime DLL, next to the engine and the tests.
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RTXVIDEO_NGX_DLL}" "$<TARGET_FILE_DIR:${target}>")
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
  # M3 Task 6
  src/engine/color_calibration_service.cpp
)
target_include_directories(bc_engine PUBLIC src)
target_link_libraries(bc_engine PUBLIC bc_io winmm)
target_link_libraries(bc_engine PUBLIC ixwebsocket::ixwebsocket bcrypt)

# M6 Task 1 — the optional ONNX Runtime (DirectML) matte backend.
if(BC_WITH_ONNX)
  # M6 Task 2 — the backend implementation.
  target_sources(bc_engine PRIVATE src/engine/onnx_matte.cpp)
  target_link_libraries(bc_engine PUBLIC ORT::ORT dxgi)
  target_compile_definitions(bc_engine PUBLIC BC_WITH_ONNX=1)
else()
  target_compile_definitions(bc_engine PUBLIC BC_WITH_ONNX=0)
endif()

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
  tests/rate_estimator_test.cpp
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
  # webcam-fx Task 1
  tests/gpu_timer_test.cpp
  # webcam-fx Task 3
  tests/upscaler_test.cpp
  tests/engine_upscale_test.cpp
  # webcam-fx Task 4
  tests/rtx_video_test.cpp
  # Task 6
  tests/srt_params_test.cpp
  tests/ffmpeg_input_test.cpp
  # M4a Task 1
  tests/srt_output_test.cpp
  tests/support/test_media.cpp
  # Task 7
  tests/queued_output_test.cpp
  tests/fullscreen_output_test.cpp
  # Task 8
  tests/ndi_output_test.cpp
  # M4a Task 2
  tests/ndi_input_test.cpp
  # M4a Task 4
  tests/mf_capture_test.cpp
  # DirectShow camera input
  tests/dshow_devices_test.cpp
  tests/dshow_capture_test.cpp
  tests/dshow_format_test.cpp
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
  # webcam-fx Task 6
  tests/skin_match_test.cpp
  # M3 Task 2
  tests/chart_test.cpp
  tests/calibration_test.cpp
  # M3 Task 3
  tests/color_grader_test.cpp
  # M3 Task 4
  tests/engine_color_test.cpp
  # M3 Task 5
  tests/chart_detector_test.cpp
  # M3 Task 6
  tests/color_calibration_test.cpp
  # M3 Task 8
  tests/e2e_m3_test.cpp
  # M4a Task 6
  tests/e2e_m4a_test.cpp
  # M4c Task 1
  tests/omt_build_test.cpp
  # M4c Task 3
  tests/omt_output_test.cpp
  # M4b Task 1
  tests/uyvy_test.cpp
  tests/decklink_test.cpp
  # C1 Task 1
  tests/lkc_test.cpp
  # C1 Task 3
  tests/breadcall_input_test.cpp
  # C1 Task 5
  tests/e2e_c1_test.cpp
  # M4b Task 2
  tests/aja_test.cpp
  # M4b Task 3
  tests/vcam_shared_test.cpp
  tests/vcam_output_test.cpp
  # M6 Task 2
  tests/onnx_matte_test.cpp
  # M6 Task 3
  tests/e2e_m6_test.cpp
  # real-e2e Task 1
  tests/e2e_real_effects_test.cpp
  # bcam-readback Task 1
  tests/readback_test.cpp
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

# ---- soak / fault-injection tool (M5 Task 3, spec §8.3/§8.4) ----
# Drives the real breadcamera-engine.exe over the control API with in-process SRT test
# streams. Links the engine (control API + adapters) and the shell (ProcessLauncher +
# Supervisor), and compiles the test-media helpers directly into the tool.
add_executable(bc_soak tools/soak/soak_main.cpp tests/support/test_media.cpp)
target_include_directories(bc_soak PRIVATE tests)
target_link_libraries(bc_soak PRIVATE bc_engine bc_shell psapi)
add_dependencies(bc_soak breadcamera-engine)
bc_copy_runtime(bc_soak)

add_executable(BreadCamera WIN32 src/shell/shell_main.cpp)
target_link_libraries(BreadCamera PRIVATE bc_shell bc_engine unofficial::webview2::webview2 WIL::WIL shell32 ole32 user32 dwmapi)

add_custom_command(TARGET BreadCamera POST_BUILD
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}/ui/dist -DDST=$<TARGET_FILE_DIR:BreadCamera>/ui
          -P ${CMAKE_SOURCE_DIR}/cmake/CopyUi.cmake)

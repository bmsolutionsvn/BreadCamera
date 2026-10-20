# NVIDIA RTX Video SDK 1.1 (NGX VSR): Clean up at 1× and AI Upscale (spec D4). Optional (§7.2).
# The SDK sits behind an NVIDIA Developer login and must never be committed (spec "RTX Video SDK
# licence obligations"), so it is not fetched either: the developer unzips it into
# third_party/rtx_video_sdk (untracked) or points RTXVIDEO_ROOT at it. A missing SDK turns
# BC_WITH_RTXVIDEO off and never fails the build.
set(_rtx_root "$ENV{RTXVIDEO_ROOT}")
if(RTXVIDEO_ROOT)
  set(_rtx_root "${RTXVIDEO_ROOT}")
endif()
if(NOT _rtx_root)
  set(_rtx_root "${CMAKE_SOURCE_DIR}/third_party/rtx_video_sdk")
endif()
# Normalise backslashes so the RTXVIDEO_ROOT string is safe to pass around (also a no-op for env).
file(TO_CMAKE_PATH "${_rtx_root}" _rtx_root)
find_path(RTXVIDEO_INCLUDE_DIR nvsdk_ngx_helpers_vsr.h PATHS "${_rtx_root}/include" NO_DEFAULT_PATH)
find_file(RTXVIDEO_NGX_LIB nvsdk_ngx_d.lib PATHS "${_rtx_root}/lib/Windows/x64" NO_DEFAULT_PATH)
find_file(RTXVIDEO_NGX_DLL nvngx_vsr.dll PATHS "${_rtx_root}/bin/Windows/x64/rel" NO_DEFAULT_PATH)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(RtxVideo DEFAULT_MSG RTXVIDEO_INCLUDE_DIR RTXVIDEO_NGX_LIB RTXVIDEO_NGX_DLL)
set(RTXVIDEO_ROOT "${_rtx_root}")
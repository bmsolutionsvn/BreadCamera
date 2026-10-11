# NVIDIA Video Effects SDK (Maxine) core + the green-screen feature. Optional (spec §7.2).
# Set VFXSDK_ROOT (environment or cache) to the SDK folder.
set(_vfx_root "$ENV{VFXSDK_ROOT}")
if(VFXSDK_ROOT)
  set(_vfx_root "${VFXSDK_ROOT}")
endif()
# Windows paths with backslashes would be mangled inside the BC_MAXINE_ROOT
# string define; normalise to forward slashes (also a no-op for env values).
file(TO_CMAKE_PATH "${_vfx_root}" _vfx_root)
find_path(MAXINE_INCLUDE_DIR nvVideoEffects.h PATHS "${_vfx_root}/nvvfx/include" NO_DEFAULT_PATH)
find_file(MAXINE_VFX_PROXY nvVideoEffectsProxy.cpp PATHS "${_vfx_root}/nvvfx/src" NO_DEFAULT_PATH)
find_file(MAXINE_IMAGE_PROXY nvCVImageProxy.cpp PATHS "${_vfx_root}/nvvfx/src" NO_DEFAULT_PATH)
file(GLOB _gs_dirs "${_vfx_root}/features/nvvfxgreenscreen*/include")
find_path(MAXINE_GREENSCREEN_INCLUDE_DIR nvVFXGreenScreen.h PATHS ${_gs_dirs} "${_vfx_root}/nvvfx/include" NO_DEFAULT_PATH)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Maxine DEFAULT_MSG MAXINE_INCLUDE_DIR MAXINE_VFX_PROXY MAXINE_IMAGE_PROXY MAXINE_GREENSCREEN_INCLUDE_DIR)
set(MAXINE_ROOT "${_vfx_root}")

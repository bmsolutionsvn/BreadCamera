# Finds the NDI 6 SDK (spec §7.2: optional). Defines NDI_FOUND, NDI::NDI and NDI_DLL.
set(_ndi_root "$ENV{NDI_SDK_DIR}")
if(NOT _ndi_root)
  set(_ndi_root "C:/Program Files/NDI/NDI 6 SDK")
endif()
find_path(NDI_INCLUDE_DIR Processing.NDI.Lib.h PATHS "${_ndi_root}/Include" NO_DEFAULT_PATH)
find_library(NDI_LIBRARY Processing.NDI.Lib.x64 PATHS "${_ndi_root}/Lib/x64" NO_DEFAULT_PATH)
find_file(NDI_DLL Processing.NDI.Lib.x64.dll PATHS "${_ndi_root}/Bin/x64" NO_DEFAULT_PATH)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(NDI DEFAULT_MSG NDI_INCLUDE_DIR NDI_LIBRARY NDI_DLL)
if(NDI_FOUND AND NOT TARGET NDI::NDI)
  add_library(NDI::NDI UNKNOWN IMPORTED)
  set_target_properties(NDI::NDI PROPERTIES
    IMPORTED_LOCATION "${NDI_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${NDI_INCLUDE_DIR}")
endif()

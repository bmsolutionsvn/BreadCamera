# ONNX Runtime (DirectML build), the DirectML redistributable and the MODNet model, all
# fetched by scripts/fetch-sdks.ps1 (spec §7.2: optional). Defines OnnxRuntime_FOUND,
# ORT::ORT, ORT_DLL, DML_DLL and MODNET_ONNX.
set(_ort "${CMAKE_SOURCE_DIR}/third_party/onnxruntime")
find_path(ORT_INCLUDE_DIR onnxruntime_cxx_api.h PATHS "${_ort}/include" NO_DEFAULT_PATH)
find_path(ORT_DML_INCLUDE_DIR dml_provider_factory.h PATHS "${_ort}/include" NO_DEFAULT_PATH)
find_library(ORT_LIBRARY onnxruntime PATHS "${_ort}/lib" NO_DEFAULT_PATH)
find_file(ORT_DLL onnxruntime.dll PATHS "${_ort}/bin" NO_DEFAULT_PATH)
find_file(DML_DLL DirectML.dll PATHS "${CMAKE_SOURCE_DIR}/third_party/directml/bin" NO_DEFAULT_PATH)
find_file(MODNET_ONNX modnet.onnx PATHS "${CMAKE_SOURCE_DIR}/third_party/models" NO_DEFAULT_PATH)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OnnxRuntime DEFAULT_MSG ORT_INCLUDE_DIR ORT_DML_INCLUDE_DIR ORT_LIBRARY ORT_DLL DML_DLL MODNET_ONNX)
if(OnnxRuntime_FOUND AND NOT TARGET ORT::ORT)
  add_library(ORT::ORT UNKNOWN IMPORTED)
  set_target_properties(ORT::ORT PROPERTIES
    IMPORTED_LOCATION "${ORT_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ORT_INCLUDE_DIR}")
endif()

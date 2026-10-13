# M4b Task 3 — the media source DLL. The engine opens the shared section this creates and
# publishes frames into it; the DLL reads them instead of drawing its own test pattern.
file(GLOB VCAM_SRC ${CMAKE_CURRENT_SOURCE_DIR}/source/*.cpp)
add_library(breadcamera-vcam SHARED ${VCAM_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/source/VCamSampleSource.def
            ${CMAKE_CURRENT_SOURCE_DIR}/source/VCamSampleSource.rc ${CMAKE_SOURCE_DIR}/src/core/vcam_shared.cpp)
# C++/WinRT (winrt/base.h) ships with the Windows SDK at <sdk>/Include/<ver>/cppwinrt.
target_include_directories(breadcamera-vcam PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_SOURCE_DIR}/src
                           ${BC_WINDOWS_SDK_CPPWINRT})
target_compile_definitions(breadcamera-vcam PRIVATE _WINDOWS _USRDLL)
target_compile_options(breadcamera-vcam PRIVATE /await:strict /bigobj)
target_link_libraries(breadcamera-vcam PRIVATE WIL::WIL mfplat mfuuid mf d2d1 dwrite d3d11 dxgi windowscodecs advapi32
                      runtimeobject)
# Build fix (ledgered): the sample's trace helper calls PropVariantToStringAlloc (propsys.lib); its
# vcxproj relied on MSBuild's default libraries, which CMake does not add.
target_link_libraries(breadcamera-vcam PRIVATE propsys)
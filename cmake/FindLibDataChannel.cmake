# libdatachannel (spec D1/§7.2: optional — the shared LiveKit client's WebRTC
# stack). Prefers the CMake package that vcpkg installs and falls back to a
# manual search, matching the repo's other finders. Defines
# LibDataChannel_FOUND and LibDataChannel::LibDataChannel.
find_package(LibDataChannel CONFIG QUIET)
if(NOT LibDataChannel_FOUND)
  find_path(LIBDATACHANNEL_INCLUDE_DIR rtc/rtc.hpp)
  find_library(LIBDATACHANNEL_LIBRARY NAMES datachannel libdatachannel)
  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(LibDataChannel DEFAULT_MSG LIBDATACHANNEL_INCLUDE_DIR LIBDATACHANNEL_LIBRARY)
  if(LibDataChannel_FOUND AND NOT TARGET LibDataChannel::LibDataChannel)
    add_library(LibDataChannel::LibDataChannel UNKNOWN IMPORTED)
    set_target_properties(LibDataChannel::LibDataChannel PROPERTIES
      IMPORTED_LOCATION "${LIBDATACHANNEL_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${LIBDATACHANNEL_INCLUDE_DIR}")
  endif()
endif()
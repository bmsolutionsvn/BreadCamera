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
  # Task 3
  # Task 4
)
target_include_directories(bc_core PUBLIC src)
target_link_libraries(bc_core PUBLIC nlohmann_json::nlohmann_json)

# ---- tests ----
add_executable(bc_tests
  tests/main.cpp
  tests/support/requires.cpp
  tests/rate_test.cpp
  # Task 2..14 test files
)
target_include_directories(bc_tests PRIVATE tests)
target_link_libraries(bc_tests PRIVATE bc_core doctest::doctest)
target_compile_definitions(bc_tests PRIVATE BC_WITH_NDI=$<BOOL:${BC_WITH_NDI}>)

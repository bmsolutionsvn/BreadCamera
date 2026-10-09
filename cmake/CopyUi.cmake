# Copies the built UI next to BreadCamera.exe when it exists (ui/ is built by npm, not CMake).
if(EXISTS "${SRC}/index.html")
  file(COPY "${SRC}/" DESTINATION "${DST}")
endif()

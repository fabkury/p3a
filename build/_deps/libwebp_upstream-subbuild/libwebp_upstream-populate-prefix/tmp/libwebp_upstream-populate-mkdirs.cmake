# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/firmware/build/_deps/libwebp_upstream-src")
  file(MAKE_DIRECTORY "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/firmware/build/_deps/libwebp_upstream-src")
endif()
file(MAKE_DIRECTORY
  "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/firmware/build/_deps/libwebp_upstream-build"
  "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/firmware/build/_deps/libwebp_upstream-subbuild/libwebp_upstream-populate-prefix"
  "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/firmware/build/_deps/libwebp_upstream-subbuild/libwebp_upstream-populate-prefix/tmp"
  "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/firmware/build/_deps/libwebp_upstream-subbuild/libwebp_upstream-populate-prefix/src/libwebp_upstream-populate-stamp"
  "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/firmware/build/_deps/libwebp_upstream-subbuild/libwebp_upstream-populate-prefix/src"
  "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/firmware/build/_deps/libwebp_upstream-subbuild/libwebp_upstream-populate-prefix/src/libwebp_upstream-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/firmware/build/_deps/libwebp_upstream-subbuild/libwebp_upstream-populate-prefix/src/libwebp_upstream-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/firmware/build/_deps/libwebp_upstream-subbuild/libwebp_upstream-populate-prefix/src/libwebp_upstream-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()

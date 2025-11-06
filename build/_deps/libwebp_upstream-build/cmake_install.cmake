# Install script for directory: D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-src

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/Program Files (x86)/p3a")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "TRUE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "C:/Users/Fab/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20241119/riscv32-esp-elf/bin/riscv32-esp-elf-objdump.exe")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-build/sharpyuv/libsharpyuv.pc")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-build/libsharpyuv.a")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/webp/sharpyuv" TYPE FILE FILES
    "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-src/sharpyuv/sharpyuv.h"
    "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-src/sharpyuv/sharpyuv_csp.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-build/src/libwebpdecoder.pc")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-build/src/libwebp.pc")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-build/src/demux/libwebpdemux.pc")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-build/libwebpdecoder.a")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/webp" TYPE FILE FILES
    "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-src/src/webp/decode.h"
    "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-src/src/webp/types.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-build/libwebp.a")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/webp" TYPE FILE FILES
    "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-src/src/webp/decode.h"
    "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-src/src/webp/encode.h"
    "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-src/src/webp/types.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-build/libwebpdemux.a")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/webp" TYPE FILE FILES
    "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-src/src/webp/decode.h"
    "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-src/src/webp/demux.h"
    "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-src/src/webp/mux_types.h"
    "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-src/src/webp/types.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/WebP/cmake/WebPTargets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/WebP/cmake/WebPTargets.cmake"
         "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-build/CMakeFiles/Export/3dd5097d708f2adcdf4871ccc089782a/WebPTargets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/WebP/cmake/WebPTargets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/WebP/cmake/WebPTargets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/WebP/cmake" TYPE FILE FILES "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-build/CMakeFiles/Export/3dd5097d708f2adcdf4871ccc089782a/WebPTargets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/WebP/cmake" TYPE FILE FILES "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-build/CMakeFiles/Export/3dd5097d708f2adcdf4871ccc089782a/WebPTargets-release.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/WebP/cmake" TYPE FILE FILES
    "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-build/WebPConfigVersion.cmake"
    "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-build/WebPConfig.cmake"
    )
endif()


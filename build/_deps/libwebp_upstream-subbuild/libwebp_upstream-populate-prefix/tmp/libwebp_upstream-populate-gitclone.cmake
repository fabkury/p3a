# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

if(EXISTS "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-subbuild/libwebp_upstream-populate-prefix/src/libwebp_upstream-populate-stamp/libwebp_upstream-populate-gitclone-lastrun.txt" AND EXISTS "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-subbuild/libwebp_upstream-populate-prefix/src/libwebp_upstream-populate-stamp/libwebp_upstream-populate-gitinfo.txt" AND
  "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-subbuild/libwebp_upstream-populate-prefix/src/libwebp_upstream-populate-stamp/libwebp_upstream-populate-gitclone-lastrun.txt" IS_NEWER_THAN "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-subbuild/libwebp_upstream-populate-prefix/src/libwebp_upstream-populate-stamp/libwebp_upstream-populate-gitinfo.txt")
  message(VERBOSE
    "Avoiding repeated git clone, stamp file is up to date: "
    "'D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-subbuild/libwebp_upstream-populate-prefix/src/libwebp_upstream-populate-stamp/libwebp_upstream-populate-gitclone-lastrun.txt'"
  )
  return()
endif()

# Even at VERBOSE level, we don't want to see the commands executed, but
# enabling them to be shown for DEBUG may be useful to help diagnose problems.
cmake_language(GET_MESSAGE_LOG_LEVEL active_log_level)
if(active_log_level MATCHES "DEBUG|TRACE")
  set(maybe_show_command COMMAND_ECHO STDOUT)
else()
  set(maybe_show_command "")
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} -E rm -rf "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-src"
  RESULT_VARIABLE error_code
  ${maybe_show_command}
)
if(error_code)
  message(FATAL_ERROR "Failed to remove directory: 'D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-src'")
endif()

# try the clone 3 times in case there is an odd git clone issue
set(error_code 1)
set(number_of_tries 0)
while(error_code AND number_of_tries LESS 3)
  execute_process(
    COMMAND "C:/Program Files/Git/cmd/git.exe"
            clone --no-checkout --depth 1 --no-single-branch --config "advice.detachedHead=false" "https://chromium.googlesource.com/webm/libwebp" "libwebp_upstream-src"
    WORKING_DIRECTORY "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps"
    RESULT_VARIABLE error_code
    ${maybe_show_command}
  )
  math(EXPR number_of_tries "${number_of_tries} + 1")
endwhile()
if(number_of_tries GREATER 1)
  message(NOTICE "Had to git clone more than once: ${number_of_tries} times.")
endif()
if(error_code)
  message(FATAL_ERROR "Failed to clone repository: 'https://chromium.googlesource.com/webm/libwebp'")
endif()

execute_process(
  COMMAND "C:/Program Files/Git/cmd/git.exe"
          checkout "v1.4.0" --
  WORKING_DIRECTORY "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-src"
  RESULT_VARIABLE error_code
  ${maybe_show_command}
)
if(error_code)
  message(FATAL_ERROR "Failed to checkout tag: 'v1.4.0'")
endif()

set(init_submodules TRUE)
if(init_submodules)
  execute_process(
    COMMAND "C:/Program Files/Git/cmd/git.exe" 
            submodule update --recursive --init 
    WORKING_DIRECTORY "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-src"
    RESULT_VARIABLE error_code
    ${maybe_show_command}
  )
endif()
if(error_code)
  message(FATAL_ERROR "Failed to update submodules in: 'D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-src'")
endif()

# Complete success, update the script-last-run stamp file:
#
execute_process(
  COMMAND ${CMAKE_COMMAND} -E copy "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-subbuild/libwebp_upstream-populate-prefix/src/libwebp_upstream-populate-stamp/libwebp_upstream-populate-gitinfo.txt" "D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-subbuild/libwebp_upstream-populate-prefix/src/libwebp_upstream-populate-stamp/libwebp_upstream-populate-gitclone-lastrun.txt"
  RESULT_VARIABLE error_code
  ${maybe_show_command}
)
if(error_code)
  message(FATAL_ERROR "Failed to copy script-last-run stamp file: 'D:/Dropbox/PC/F/Estudo/Tecnologia/ESP32/project/mark-5/firmware/build/_deps/libwebp_upstream-subbuild/libwebp_upstream-populate-prefix/src/libwebp_upstream-populate-stamp/libwebp_upstream-populate-gitclone-lastrun.txt'")
endif()

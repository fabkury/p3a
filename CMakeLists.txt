# p3a 2.0. The 1.x project lives in legacy/ and is never built from here.
cmake_minimum_required(VERSION 3.16)

# The one version number. Embedded in the binary and compared by OTA.
# MAJOR.MINOR.PATCH with an optional -suffix; "-dev" until the release commit.
set(PROJECT_VER "2.0.0-dev")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p3a)

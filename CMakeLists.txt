cmake_minimum_required(VERSION 3.16.0)
# explicit version: ESP-IDF otherwise runs `git describe`, which fails in a
# repository without commits. The release workflow sets FW_VERSION from the
# tag; the settings page compares it with the latest GitHub release.
if(DEFINED ENV{FW_VERSION})
    set(PROJECT_VER "$ENV{FW_VERSION}")
else()
    set(PROJECT_VER "0.3.0")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(usb_host_probe)

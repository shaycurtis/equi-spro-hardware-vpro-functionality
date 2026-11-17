cmake_minimum_required(VERSION 3.16)

# Firmware version, from git (CONFIG_APP_PROJECT_VER_FROM_CONFIG is off). It is what the device
# reports (shadow "fw", charging report) and what a firmware-update job's "version" is compared with.
#   tag sensor-v0.2.0 on this commit   -> 0.2.0
#   3 commits after that tag           -> 0.2.0-3-g1a2b3c4
#   no sensor-v* tag yet               -> 0.1.0-g1a2b3c4
#   uncommitted changes                -> ...-dirty
# A release: commit, `git tag sensor-v0.2.0`, then build (cmake re-runs when HEAD or the index changes).
execute_process(COMMAND git describe --tags --match "sensor-v*" --dirty --always
                WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                OUTPUT_VARIABLE FW_GIT_VER OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE FW_GIT_RC ERROR_QUIET)
if(FW_GIT_RC EQUAL 0 AND FW_GIT_VER MATCHES "^sensor-v(.+)$")
    set(PROJECT_VER "${CMAKE_MATCH_1}")
elseif(FW_GIT_RC EQUAL 0 AND FW_GIT_VER)
    set(PROJECT_VER "0.1.0-g${FW_GIT_VER}")
else()
    set(PROJECT_VER "0.1.0-nogit")
endif()
execute_process(COMMAND git rev-parse --absolute-git-dir
                WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                OUTPUT_VARIABLE FW_GIT_DIR OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(FW_GIT_DIR AND EXISTS "${FW_GIT_DIR}/HEAD")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FW_GIT_DIR}/HEAD" "${FW_GIT_DIR}/index")
endif()
message(STATUS "Firmware version: ${PROJECT_VER}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(equi-sensordevice-vpro)

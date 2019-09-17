cmake_minimum_required (VERSION 3.1)
project (opc-ua-device-service LANGUAGES C)

# Package support

set (CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/cmake)


# Configuration variables

set (DEV_OPCUA_BUILD_DEBUG OFF CACHE BOOL "Build Debug")

# Configure for different target systems

if (${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
  set (CPACK_GENERATOR TGZ)
endif ()
if (${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
  set (CPACK_GENERATOR TGZ)
endif ()
if (${CMAKE_SYSTEM_NAME} MATCHES "Windows")
  set (CPACK_GENERATOR ZIP)
endif ()

# Build modules

add_subdirectory (c)

cmake_minimum_required(VERSION 3.1)
project(device_opcua LANGUAGES C)

# Package support
set (CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/cmake)

# Set additional C compiler flags
if (DEFINED CMAKE_C_COMPILER_ID)
  if (CMAKE_C_COMPILER_ID MATCHES GNU)
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wstrict-prototypes -Wmissing-prototypes")
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c99")
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_GNU_SOURCE")
    set (CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -ggdb -O0")
    set (CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -ggdb -O0")
    set (CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -DNDEBUG")
    set (CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} -O3")
   endif ()
endif ()

file (GLOB C_FILES *.c)

FILE(STRINGS "../../VERSION" VERSION_NUMBER)

find_library(EDGEX_CSDK_LIB NAMES csdk PATHS ENV CSDK_DIR PATH_SUFFIXES lib)
find_path(EDGEX_CSDK_INCLUDE NAMES edgex/devsdk.h PATHS ENV CSDK_DIR PATH_SUFFIXES include)
find_library(OPEN62541_RC2_LIB NAMES open62541)
add_executable(device-opcua-c ${C_FILES})

TARGET_COMPILE_DEFINITIONS(device-opcua-c PUBLIC VERSION="${VERSION_NUMBER}")

target_include_directories(device-opcua-c PRIVATE ${EDGEX_CSDK_INCLUDE} .)
target_link_libraries(device-opcua-c PRIVATE ${EDGEX_CSDK_LIB} ${OPEN62541_RC2_LIB})

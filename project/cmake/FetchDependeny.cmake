include_guard(GLOBAL)

find_package(Git REQUIRED)

set(ATFRAMEWORK_CMAKE_TOOLSET_DIR
    "${PROJECT_SOURCE_DIR}/atframework/cmake-toolset"
    CACHE PATH "PATH to cmake-toolset")

set(ATFRAMEWORK_ATFRAME_UTILS_REPO_DIR
    "${PROJECT_SOURCE_DIR}/atframework/atframe_utils"
    CACHE PATH "PATH to atframe_utils")

set(ATFRAMEWORK_LIBATBUS_REPO_DIR
    "${PROJECT_SOURCE_DIR}/atframework/libatbus"
    CACHE PATH "PATH to libatbus")

if(NOT ATFRAMEWORK_CMAKE_TOOLSET_EXECUTE_PROCESS_OUTPUT_OPTIONS)
  unset(ATFRAMEWORK_CMAKE_TOOLSET_EXECUTE_PROCESS_OUTPUT_OPTIONS)
  if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.15")
    list(APPEND ATFRAMEWORK_CMAKE_TOOLSET_EXECUTE_PROCESS_OUTPUT_OPTIONS COMMAND_ECHO STDOUT)
  endif()
  if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.18")
    list(APPEND ATFRAMEWORK_CMAKE_TOOLSET_EXECUTE_PROCESS_OUTPUT_OPTIONS ECHO_OUTPUT_VARIABLE ECHO_ERROR_VARIABLE)
  endif()
endif()

if(NOT EXISTS "${ATFRAMEWORK_CMAKE_TOOLSET_DIR}/Import.cmake")
  execute_process(
    COMMAND ${GIT_EXECUTABLE} submodule update --depth 100 --recommend-shallow -f --init -- atframework/cmake-toolset
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" ${ATFRAMEWORK_CMAKE_TOOLSET_EXECUTE_PROCESS_OUTPUT_OPTIONS})
  set(ATFRAMEWORK_CMAKE_TOOLSET_DIR
      "${PROJECT_SOURCE_DIR}/atframework/cmake-toolset"
      CACHE PATH "PATH to cmake-toolset" FORCE)
endif()

include("${ATFRAMEWORK_CMAKE_TOOLSET_DIR}/Import.cmake")

macro(ATFRAMEWORK_ATFRAME_UTILS_POPULATE)
  if(NOT EXISTS "${ATFRAMEWORK_ATFRAME_UTILS_REPO_DIR}/CMakeLists.txt")
    execute_process(
      COMMAND ${GIT_EXECUTABLE} submodule update --depth 100 --recommend-shallow -f --init -- atframework/atframe_utils
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" ${ATFRAMEWORK_CMAKE_TOOLSET_EXECUTE_PROCESS_OUTPUT_OPTIONS})
    set(ATFRAMEWORK_ATFRAME_UTILS_REPO_DIR
        "${PROJECT_SOURCE_DIR}/atframework/atframe_utils"
        CACHE PATH "PATH to atframe_utils" FORCE)
  endif()
endmacro()

if(TARGET atframe_utils)
  set(ATFRAMEWORK_ATFRAME_UTILS_LINK_NAME atframe_utils)
elseif(TARGET atframework::atframe_utils)
  set(ATFRAMEWORK_ATFRAME_UTILS_LINK_NAME atframework::atframe_utils)
else()
  set(ATFRAMEWORK_ATFRAME_UTILS_LINK_NAME atframe_utils)
  if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/_deps/${ATFRAMEWORK_ATFRAME_UTILS_LINK_NAME}")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/_deps/${ATFRAMEWORK_ATFRAME_UTILS_LINK_NAME}")
  endif()
  atframework_atframe_utils_populate()
  add_subdirectory("${ATFRAMEWORK_ATFRAME_UTILS_REPO_DIR}"
                   "${CMAKE_CURRENT_BINARY_DIR}/_deps/${ATFRAMEWORK_ATFRAME_UTILS_LINK_NAME}")
endif()

macro(ATFRAMEWORK_LIBATBUS_POPULATE)
  if(NOT EXISTS "${ATFRAMEWORK_LIBATBUS_REPO_DIR}/CMakeLists.txt")
    execute_process(
      COMMAND ${GIT_EXECUTABLE} submodule update --depth 100 --recommend-shallow -f --init -- atframework/libatbus
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" ${ATFRAMEWORK_CMAKE_TOOLSET_EXECUTE_PROCESS_OUTPUT_OPTIONS})
    set(ATFRAMEWORK_LIBATBUS_REPO_DIR
        "${PROJECT_SOURCE_DIR}/atframework/libatbus"
        CACHE PATH "PATH to libatbus" FORCE)
  endif()
endmacro()

if(TARGET atbus)
  set(ATFRAMEWORK_LIBATBUS_LINK_NAME atbus)
  set(ATFRAMEWORK_LIBATBUS_PROTOCOL_LINK_NAME atbus-protocol)
elseif(TARGET atframework::atbus)
  set(ATFRAMEWORK_LIBATBUS_LINK_NAME atframework::atbus)
  set(ATFRAMEWORK_LIBATBUS_PROTOCOL_LINK_NAME atframework::atbus-protocol)
else()
  set(ATFRAMEWORK_LIBATBUS_LINK_NAME atbus)
  set(ATFRAMEWORK_LIBATBUS_PROTOCOL_LINK_NAME atbus-protocol)
  if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/_deps/${ATFRAMEWORK_LIBATBUS_LINK_NAME}")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/_deps/${ATFRAMEWORK_LIBATBUS_LINK_NAME}")
  endif()
  atframework_libatbus_populate()
  add_subdirectory("${ATFRAMEWORK_LIBATBUS_REPO_DIR}"
                   "${CMAKE_CURRENT_BINARY_DIR}/_deps/${ATFRAMEWORK_LIBATBUS_LINK_NAME}")
endif()

# ThirdParty
include("${ATFRAMEWORK_CMAKE_TOOLSET_DIR}/ports/libuv/libuv.cmake")
include("${ATFRAMEWORK_CMAKE_TOOLSET_DIR}/ports/fmtlib/fmtlib.cmake")
include("${ATFRAMEWORK_CMAKE_TOOLSET_DIR}/ports/yaml-cpp/yaml-cpp.cmake")
include("${ATFRAMEWORK_CMAKE_TOOLSET_DIR}/ports/json/rapidjson.cmake")
include("${ATFRAMEWORK_CMAKE_TOOLSET_DIR}/ports/protobuf/protobuf.cmake")
include("${ATFRAMEWORK_CMAKE_TOOLSET_DIR}/ports/ssl/port.cmake")
include("${ATFRAMEWORK_CMAKE_TOOLSET_DIR}/ports/cares/c-ares.cmake")
include("${ATFRAMEWORK_CMAKE_TOOLSET_DIR}/ports/re2/re2.cmake")
include("${ATFRAMEWORK_CMAKE_TOOLSET_DIR}/ports/libcurl/libcurl.cmake")
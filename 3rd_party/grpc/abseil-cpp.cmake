# STL like library
# https://github.com/abseil/abseil-cpp.git
# git@github.com:abseil/abseil-cpp.git

if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.10")
  include_guard(GLOBAL)
endif()

# =========== 3rdparty abseil-cpp ==================
macro(PROJECT_LIBATAPP_ABSEIL_IMPORT)
  if(absl_FOUND)
    message(STATUS "Dependency(${PROJECT_NAME}): abseil-cpp found.")
  endif()
endmacro()

if(NOT absl_FOUND)
  if(VCPKG_TOOLCHAIN)
    find_package(absl QUIET)
    project_libatapp_abseil_import()
  endif()

  if(NOT absl_FOUND)
    set(3RD_PARTY_ABSEIL_DEFAULT_VERSION "20210324.0")

    findconfigurepackage(
      PACKAGE
      absl
      BUILD_WITH_CMAKE
      CMAKE_INHIRT_BUILD_ENV
      CMAKE_FLAGS
      "-DCMAKE_POSITION_INDEPENDENT_CODE=YES"
      "-DBUILD_TESTING=OFF" # "-DBUILD_SHARED_LIBS=OFF"
      MSVC_CONFIGURE
      ${gRPC_MSVC_CONFIGURE}
      WORKING_DIRECTORY
      "${PROJECT_3RD_PARTY_PACKAGE_DIR}"
      BUILD_DIRECTORY
      "${CMAKE_CURRENT_BINARY_DIR}/deps/abseil-cpp-${3RD_PARTY_ABSEIL_DEFAULT_VERSION}/build_jobs_${PROJECT_PREBUILT_PLATFORM_NAME}"
      PREFIX_DIRECTORY
      "${PROJECT_3RD_PARTY_INSTALL_DIR}"
      SRC_DIRECTORY_NAME
      "abseil-cpp-${3RD_PARTY_ABSEIL_DEFAULT_VERSION}"
      GIT_BRANCH
      "${3RD_PARTY_ABSEIL_DEFAULT_VERSION}"
      GIT_URL
      "https://github.com/abseil/abseil-cpp.git")

    if(absl_FOUND)
      project_libatapp_abseil_import()
    endif()
  endif()
else()
  project_libatapp_abseil_import()
endif()

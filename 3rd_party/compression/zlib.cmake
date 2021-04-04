if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.10")
  include_guard(GLOBAL)
endif()

# =========== 3rdparty zlib ==================
# force to use prebuilt when using mingw
macro(PROJECT_LIBATAPP_ZLIB_IMPORT)
  if(TARGET ZLIB::ZLIB)
    # find static library first
    echowithcolor(COLOR GREEN "-- Dependency(${PROJECT_NAME}): zlib found.(${ZLIB_LIBRARIES})")

    if(ZLIB_INCLUDE_DIRS)
      list(APPEND PROJECT_LIBATAPP_PUBLIC_INCLUDE_DIRS ${ZLIB_INCLUDE_DIRS})
    endif()

    if(ZLIB_LIBRARIES)
      list(APPEND PROJECT_LIBATAPP_PUBLIC_LINK_NAMES ${ZLIB_LIBRARIES})
    endif()
  endif()
endmacro()

if(NOT TARGET ZLIB::ZLIB)
  if(VCPKG_TOOLCHAIN)
    find_package(ZLIB QUIET)
    project_libatapp_zlib_import()
  endif()

  if(NOT TARGET ZLIB::ZLIB)
    set(3RD_PARTY_ZLIB_DEFAULT_VERSION "v1.2.11")
    set(ZLIB_ROOT ${PROJECT_3RD_PARTY_INSTALL_DIR})

    findconfigurepackage(
      PACKAGE
      ZLIB
      BUILD_WITH_CMAKE
      CMAKE_INHIRT_BUILD_ENV
      CMAKE_INHIRT_BUILD_ENV_DISABLE_CXX_FLAGS
      CMAKE_FLAGS
      "-DCMAKE_POSITION_INDEPENDENT_CODE=YES"
      "-DBUILD_SHARED_LIBS=OFF"
      MAKE_FLAGS
      "-j8"
      WORKING_DIRECTORY
      "${PROJECT_3RD_PARTY_PACKAGE_DIR}"
      PREFIX_DIRECTORY
      "${PROJECT_3RD_PARTY_INSTALL_DIR}"
      SRC_DIRECTORY_NAME
      "zlib-${3RD_PARTY_ZLIB_DEFAULT_VERSION}"
      # PROJECT_DIRECTORY "${PROJECT_3RD_PARTY_PACKAGE_DIR}/zlib-${3RD_PARTY_ZLIB_DEFAULT_VERSION}"
      BUILD_DIRECTORY
      "${CMAKE_CURRENT_BINARY_DIR}/deps/zlib-${3RD_PARTY_ZLIB_DEFAULT_VERSION}/build_jobs_${PROJECT_PREBUILT_PLATFORM_NAME}"
      GIT_BRANCH
      "${3RD_PARTY_ZLIB_DEFAULT_VERSION}"
      GIT_URL
      "https://github.com/madler/zlib.git")

    if(NOT TARGET ZLIB::ZLIB)
      echowithcolor(COLOR RED "-- Dependency: zlib is required")
      message(FATAL_ERROR "zlib not found")
    endif()
    project_libatapp_zlib_import()
  endif()
else()
  project_libatapp_zlib_import()
endif()

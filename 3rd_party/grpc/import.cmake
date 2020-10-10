# ABSL
# CARES
# Protobuf
# RE2
# SSL
# ZLIB

if(PROJECT_PREBUILT_PLATFORM_NAME STREQUAL PROJECT_PREBUILT_HOST_PLATFORM_NAME)
    set(gRPC_BUILD_CODEGEN ON)
    set(gRPC_CMAKE_CROSSCOMPILING OFF)
else()
    set(gRPC_BUILD_CODEGEN OFF)
    set(gRPC_CMAKE_CROSSCOMPILING ON)
endif()

set(gRPC_MSVC_CONFIGURE ${CMAKE_BUILD_TYPE})

include ("${CMAKE_CURRENT_LIST_DIR}/abseil-cpp.cmake")
include ("${CMAKE_CURRENT_LIST_DIR}/c-ares.cmake")
include ("${CMAKE_CURRENT_LIST_DIR}/re2.cmake")
# include ("${CMAKE_CURRENT_LIST_DIR}/upb.cmake")

# cmake ../.. -DgRPC_INSTALL=ON        \
#     -DCMAKE_BUILD_TYPE=Release       \
#     -DgRPC_ABSL_PROVIDER=package     \
#     -DgRPC_CARES_PROVIDER=package    \
#     -DgRPC_PROTOBUF_PROVIDER=package \
#     -DgRPC_PROTOBUF_PACKAGE_TYPE=CONFIG \
#     -DgRPC_RE2_PROVIDER=package      \
#     -DgRPC_SSL_PROVIDER=package      \
#     -DgRPC_ZLIB_PROVIDER=package     \
##     -DgRPC_UPB_PROVIDER=package       \
#     -DgRPC_GFLAGS_PROVIDER=none
#     -DgRPC_BENCHMARK_PROVIDER=none
#     -DgRPC_BUILD_TESTS=OFF
#     -DgRPC_BUILD_CODEGEN=${gRPC_BUILD_CODEGEN}
#     -DCMAKE_CROSSCOMPILING=${gRPC_CMAKE_CROSSCOMPILING}

include ("${CMAKE_CURRENT_LIST_DIR}/grpc.cmake")
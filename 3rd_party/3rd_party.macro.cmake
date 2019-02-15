﻿# =========== 3rd_party ===========
set (PROJECT_3RD_PARTY_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR})

include("${PROJECT_3RD_PARTY_ROOT_DIR}/atframe_utils/libatframe_utils.cmake")
include("${PROJECT_3RD_PARTY_ROOT_DIR}/libatbus/libatbus.cmake")
include("${PROJECT_3RD_PARTY_ROOT_DIR}/libiniloader/libiniloader.cmake")
include("${3RD_PARTY_LIBATBUS_PKG_DIR}/3rd_party/libuv/libuv.cmake")
include("${3RD_PARTY_LIBATBUS_PKG_DIR}/3rd_party/msgpack/msgpack.cmake")

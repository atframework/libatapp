if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.10")
    include_guard(GLOBAL)
endif()

# =========== 3rdparty libiniloader ==================
if(NOT 3RD_PARTY_LIBINILOADER_BASE_DIR)
    set (3RD_PARTY_LIBINILOADER_BASE_DIR ${CMAKE_CURRENT_LIST_DIR})
endif()

set (3RD_PARTY_LIBINILOADER_REPO_DIR "${3RD_PARTY_LIBINILOADER_BASE_DIR}/repo")

if(NOT EXISTS ${3RD_PARTY_LIBINILOADER_REPO_DIR})
    find_package(Git)
    if(NOT GIT_FOUND)
        message(FATAL_ERROR "git not found")
    endif()

    execute_process(COMMAND ${GIT_EXECUTABLE} clone --depth=1 "https://github.com/owt5008137/libiniloader.git" ${3RD_PARTY_LIBINILOADER_REPO_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    )
endif()

set (3RD_PARTY_LIBINILOADER_INC_DIR ${3RD_PARTY_LIBINILOADER_REPO_DIR})
set (3RD_PARTY_LIBINILOADER_SRC_DIR ${3RD_PARTY_LIBINILOADER_REPO_DIR})
# set (3RD_PARTY_LIBINILOADER_LINK_NAME)

include_directories(${3RD_PARTY_LIBINILOADER_INC_DIR})
list(APPEND PROJECT_3RD_PARTY_SRC_LIST "${3RD_PARTY_LIBINILOADER_REPO_DIR}/ini_loader.cpp")
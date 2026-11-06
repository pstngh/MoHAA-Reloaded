# Unix specific settings

if(NOT UNIX)
    return()
endif()

list(APPEND SYSTEM_PLATFORM_SOURCES ${SOURCE_DIR}/sys/sys_unix.c)

list(APPEND SYSTEM_PLATFORM_SOURCES ${SOURCE_DIR}/sys/con_tty.c)

list(APPEND COMMON_LIBRARIES
    ${CMAKE_DL_LIBS}    # Dynamic loader
    m                   # Math library
)

list(APPEND SYSTEM_PLATFORM_SOURCES ${SOURCE_DIR}/sys/new/sys_unix_new.c)

find_package(Threads)
list(APPEND COMMON_LIBRARIES ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT})

#
# Setup the installation directory
#
set(CMAKE_DEFAULT_INSTALL_RUNTIME_DIR lib${CMAKE_LIB_SUFFIX})
set(BIN_INSTALL_SUBDIR ${CMAKE_PROJECT_NAME})
set(LIB_INSTALL_SUBDIR ${CMAKE_PROJECT_NAME})

cmake_minimum_required(VERSION 3.25)

if(POLICY CMP0177)
    # Policy CMP0177: install() DESTINATION paths are normalized
    # Introduced in CMake 3.31
    cmake_policy(SET CMP0177 NEW)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(identity)

project(${PROJECT_NAME} VERSION ${PROJECT_VERSION} LANGUAGES C CXX)

# The Metal port ships no dedicated server (omohaaded); -DBUILD_SERVER=ON builds it for local tests
option(BUILD_SERVER "Build dedicated server" OFF)
option(BUILD_CLIENT "Build client" ON)
option(BUILD_GAME_LIBRARIES "Build game module libraries" ON)
option(BUILD_STANDALONE "Build binaries for standalone games" OFF)

option(USE_CODEC_VORBIS "Ogg Vorbis support" ON)
option(USE_CODEC_OPUS "Ogg Opus support" ON)
option(USE_CODEC_MAD "MP3 support" ON)
option(USE_VOIP "Voice chat" OFF)
option(USE_MUMBLE "Mumble support" OFF)
option(USE_DISCORD "Discord Rich Presence" ON)
option(USE_FREETYPE "Freetype font rendering" OFF)

option(USE_INTERNAL_LIBS "Use internally packaged libraries" ON)
option(USE_INTERNAL_ZLIB "Use internal copy of zlib" ${USE_INTERNAL_LIBS})
option(USE_INTERNAL_JPEG "Use internal copy of libjpeg" ${USE_INTERNAL_LIBS})
option(USE_INTERNAL_OGG "Use internal copy of ogg" ${USE_INTERNAL_LIBS})
option(USE_INTERNAL_VORBIS "Use internal copy of vorbis" ${USE_INTERNAL_LIBS})
option(USE_INTERNAL_OPUS "Use internal copy of opus" ${USE_INTERNAL_LIBS})
option(USE_INTERNAL_MAD "Use internal copy of libmad" ${USE_INTERNAL_LIBS})

# Release build by default, set externally if you want something else
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Try to enable LTO by default
include(CheckIPOSupported)
check_ipo_supported(RESULT IPO_SUPPORTED)
if(IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG FALSE)
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_SHARED_LIBRARY_PREFIX "")

set(SOURCE_DIR ${CMAKE_SOURCE_DIR}/code)

if(DEFINED ENV{SOURCE_DATE_EPOCH})
    string(TIMESTAMP PRODUCT_DATE "%b %d %Y" UTC)
    add_compile_definitions(PRODUCT_DATE="${PRODUCT_DATE}")
endif()

include(compilers/all)
include(platforms/all)
include(libraries/all)

include(version)

include(server)
include(renderer_metal)
include(client)
include(basegame)

include(post_configure)
#include(installer)

cmake_minimum_required(VERSION 3.14)
project(kinora VERSION 0.2 LANGUAGES C ASM) # ASM: emu68-common's memory functions, in every FFmpeg binary (ffmpeg/)
# The one version of the repository: every binary reports it. Bump it here only.

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/install" CACHE PATH "Install path" FORCE)
endif()

# KINORA_BUILD_STAMP marks a local (non-release) build: build.sh sets it and it rides at
# the end of every $VER cookie. Empty means release.
set(KINORA_BUILD_STAMP "" CACHE STRING "Local-build marker (build.sh sets it); empty = release")
string(TIMESTAMP KINORA_DATE "(%d.%m.%Y)")
if(KINORA_BUILD_STAMP)
    string(APPEND KINORA_DATE " ${KINORA_BUILD_STAMP}")
endif()

add_compile_definitions(
    KINORA_VERSION=${PROJECT_VERSION_MAJOR}
    KINORA_REVISION=${PROJECT_VERSION_MINOR}
    KINORA_VERSION_TEXT="${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
    KINORA_DATE="${KINORA_DATE}"
)

# emu68-common (external/emu68-common): the SFD generator, debug output, the test harness.
# scripts/docker-build.sh builds it first and passes its prefix.
find_package(Emu68Common 2.3 CONFIG REQUIRED)

# The video libraries, repositories of their own that Kinora builds and ships:
# external/videocodec-library (videocodec.library, videocore.codec, vcjpeg.datatype, VCInfo,
# VCGrab) and external/rpivid-codec (rpivid.codec).  scripts/docker-build.sh builds them after
# emu68-common, into a prefix of their own: videocodec.h comes from their package, and what
# they install goes into Kinora's install tree and archive (below).
find_package(VideoCodec 0.7 CONFIG REQUIRED)
get_filename_component(KINORA_VIDEO_PREFIX "${VideoCodec_DIR}/../../.." ABSOLUTE)
set(KINORA_VIDEOCODEC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/videocodec-library" CACHE PATH
    "videocodec-library's sources, for the licences the package takes from them")
set(KINORA_RPIVID_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/rpivid-codec" CACHE PATH
    "rpivid-codec's sources, for the licence and notices the package takes from them")

# The profile-guided FFmpeg (docs/architecture.md, section 3): USE lays FFmpeg out by the
# profiles in profiles/ (each configuration without them is built as OFF); GENERATE builds the
# instrumented libraries and PgoDump for a training round (scripts/pgo-train.py). The profiles
# are made for the default target (68040 with its FPU); another target builds without them.
set(KINORA_PGO "USE" CACHE STRING "Profile-guided FFmpeg: OFF, GENERATE or USE")
set_property(CACHE KINORA_PGO PROPERTY STRINGS OFF GENERATE USE)
if(NOT KINORA_PGO MATCHES "^(OFF|GENERATE|USE)$")
    message(FATAL_ERROR "KINORA_PGO is OFF, GENERATE or USE, not ${KINORA_PGO}")
endif()

# Our own code is gnu23 (enum types with a fixed underlying type). FFmpeg builds by its
# own rules in ffmpeg/.
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_EXTENSIONS ON)

set(KINORA_COMPILE_OPTIONS
    -O2
    -Wall
    -Wextra
    -Werror
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wmissing-prototypes
    -Wstrict-prototypes
)

add_subdirectory(ffmpeg)
add_subdirectory(freetype)
# Every binary of Kinora's own includes include/libraries/*.h, which include
# <libraries/videocodec.h>: the package's, ahead of the toolchain image's copy.
link_libraries(VideoCodec::headers)
add_subdirectory(demuxer)
add_subdirectory(audiocodec)
add_subdirectory(videocodec-ffmpeg)
add_subdirectory(player)
add_subdirectory(tools)

# The video libraries' files, from their prefix into Kinora's install tree as they lie
# there: LIBS/videocodec.library and LIBS/videocodec/ (videocore.codec, rpivid.codec),
# C/VCInfo, Classes/DataTypes/vcjpeg.datatype, Storage/DataTypes/VCJPEG,
# Tools/Commodities/VCGrab with its icon, Developer/ (the rigs and the examples), and the
# public headers into Developer/include.
install(DIRECTORY "${KINORA_VIDEO_PREFIX}/LIBS/" DESTINATION LIBS USE_SOURCE_PERMISSIONS)
install(PROGRAMS "${KINORA_VIDEO_PREFIX}/C/VCInfo" DESTINATION C)
install(DIRECTORY "${KINORA_VIDEO_PREFIX}/Classes" "${KINORA_VIDEO_PREFIX}/Storage" "${KINORA_VIDEO_PREFIX}/Tools"
        "${KINORA_VIDEO_PREFIX}/Developer" DESTINATION . USE_SOURCE_PERMISSIONS)
foreach(kind libraries proto clib inline pragmas)
    install(DIRECTORY "${KINORA_VIDEO_PREFIX}/include/${kind}" DESTINATION Developer/include
            FILES_MATCHING PATTERN "videocodec*")
endforeach()

# ---------------------------------------------------------------------------
# package: Kinora-<version>[-<stamp>].lha with the Installer script.
#
#   Install, Install.info, ReadMe, ReadMe.info
#   Kinora/      the program, its icon, Fonts/
#   LIBS/        demuxer.library, audiocodec.library, audiocodec/ffmpegaudio.codec,
#                videocodec/ffmpegvideo.codec, and the video libraries: videocodec.library,
#                videocodec/videocore.codec, videocodec/rpivid.codec
#   C/           FilmInfo, VCInfo
#   Classes/, Storage/   vcjpeg.datatype and its descriptor VCJPEG
#   Tools/       Commodities/VCGrab and its icon
#   Developer/   the public headers, the video libraries' test rigs and examples
#   Licenses/    the licences, and what FFmpeg was built from and how
#
# The amiga-build-container image ships the archiver (lha): build the target through
# scripts/docker-build.sh --target package, or ./build.sh --package.
# ---------------------------------------------------------------------------
set(PACKAGE_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
set(PACKAGE_VERSION_DISPLAY "${PACKAGE_VERSION}")
set(PACKAGE_SUFFIX "")
set(BUILD_NOTICE "")
if(KINORA_BUILD_STAMP)
    string(APPEND PACKAGE_SUFFIX "-${KINORA_BUILD_STAMP}")
    string(APPEND PACKAGE_VERSION_DISPLAY " ${KINORA_BUILD_STAMP}")
    set(BUILD_NOTICE "*** LOCAL DEVELOPMENT BUILD ${KINORA_BUILD_STAMP}\n*** Not an official release.\n\n")
endif()

set(PACKAGE_NAME    "Kinora-${PACKAGE_VERSION}${PACKAGE_SUFFIX}")
set(PACKAGE_DIR     "${CMAKE_BINARY_DIR}/package")
set(PACKAGE_TREE    "${PACKAGE_DIR}/tree")
set(PACKAGE_STAGE   "${PACKAGE_DIR}/${PACKAGE_NAME}")
set(PACKAGE_ARCHIVE "${PACKAGE_DIR}/${PACKAGE_NAME}.lha")

find_program(LHA_EXECUTABLE lha)
if(NOT LHA_EXECUTABLE)
    set(LHA_EXECUTABLE lha) # the target fails when run without one; the build itself needs none
endif()

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/installer/Install.in" "${CMAKE_BINARY_DIR}/installer/Install" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/installer/ReadMe.in" "${CMAKE_BINARY_DIR}/installer/ReadMe" @ONLY)

add_custom_target(package
    DEPENDS Kinora demuxer_library audiocodec_library ffmpegaudio_codec ffmpegvideo_codec FilmInfo
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${PACKAGE_TREE}" "${PACKAGE_STAGE}" "${PACKAGE_ARCHIVE}"
    # what install() places, in a tree of its own: the build's install prefix is left alone
    COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" --prefix "${PACKAGE_TREE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PACKAGE_STAGE}/Kinora" "${PACKAGE_STAGE}/Licenses/ffmpeg"
    COMMAND ${CMAKE_COMMAND} -E copy "${PACKAGE_TREE}/Kinora" "${PACKAGE_TREE}/Kinora.info" "${PACKAGE_STAGE}/Kinora/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PACKAGE_TREE}/Fonts" "${PACKAGE_STAGE}/Kinora/Fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PACKAGE_TREE}/LIBS" "${PACKAGE_STAGE}/LIBS"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PACKAGE_TREE}/C" "${PACKAGE_STAGE}/C"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PACKAGE_TREE}/Developer" "${PACKAGE_STAGE}/Developer"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PACKAGE_TREE}/Classes" "${PACKAGE_STAGE}/Classes"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PACKAGE_TREE}/Storage" "${PACKAGE_STAGE}/Storage"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PACKAGE_TREE}/Tools" "${PACKAGE_STAGE}/Tools"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/installer/Install" "${CMAKE_BINARY_DIR}/installer/ReadMe"
            "${CMAKE_CURRENT_SOURCE_DIR}/installer/Install.info" "${CMAKE_CURRENT_SOURCE_DIR}/installer/ReadMe.info"
            "${PACKAGE_STAGE}/"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${PACKAGE_STAGE}/Licenses/MPL-2.0"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.LGPL-2.1" "${PACKAGE_STAGE}/Licenses/LGPL-2.1"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/external/freetype/docs/FTL.TXT"
            "${PACKAGE_STAGE}/Licenses/FreeType-FTL"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/dist/fonts/DejaVuSans.license.txt"
            "${PACKAGE_STAGE}/Licenses/DejaVuSans"
    # the video libraries: videocodec-library's (BSD-3-Clause), VCGrab's Matroska writer's
    # (mkv-writer, BSD-3-Clause), rpivid.codec's (GPL-2.0, a binary of its own) with its notices
    COMMAND ${CMAKE_COMMAND} -E copy "${KINORA_VIDEOCODEC_SOURCE_DIR}/LICENSE" "${PACKAGE_STAGE}/Licenses/videocodec-library"
    COMMAND ${CMAKE_COMMAND} -E copy "${KINORA_VIDEOCODEC_SOURCE_DIR}/tools/vcgrab/mkv/LICENSE" "${PACKAGE_STAGE}/Licenses/mkv-writer"
    COMMAND ${CMAKE_COMMAND} -E copy "${KINORA_RPIVID_SOURCE_DIR}/LICENSE" "${PACKAGE_STAGE}/Licenses/rpivid-codec"
    COMMAND ${CMAKE_COMMAND} -E copy "${KINORA_RPIVID_SOURCE_DIR}/NOTICE" "${PACKAGE_STAGE}/Licenses/rpivid-codec.NOTICE"
    # what the three LGPL binaries' FFmpeg was built from: the changes to its source, the script that builds it
    # and the headers it forces in
    COMMAND ${CMAKE_COMMAND} -E copy ${KINORA_FFMPEG_PATCHES} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-ffmpeg.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg/amiga_shim.h" "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg/branchless_clip.h"
            "${PACKAGE_STAGE}/Licenses/ffmpeg/"
    # the archive holds the drawer, so that it unpacks into one
    COMMAND sh -c "cd '${PACKAGE_DIR}' && '${LHA_EXECUTABLE}' a '${PACKAGE_ARCHIVE}' '${PACKAGE_NAME}'"
    COMMAND ${CMAKE_COMMAND} -E echo "package: ${PACKAGE_ARCHIVE}"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Packaging ${PACKAGE_NAME}.lha"
    VERBATIM
)

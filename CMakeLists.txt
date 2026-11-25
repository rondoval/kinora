cmake_minimum_required(VERSION 3.14)
project(kinora VERSION 0.1 LANGUAGES C)
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
add_subdirectory(demuxer)
add_subdirectory(audiocodec)
add_subdirectory(player)
add_subdirectory(tools)

# ---------------------------------------------------------------------------
# package: Kinora-<version>[-<stamp>].lha with the Installer script.
#
#   Install, Install.info, ReadMe, ReadMe.info
#   Kinora/      the program, its icon, Fonts/
#   LIBS/        demuxer.library, audiocodec.library, audiocodec/ffmpeg.codec
#   C/           DMXInfo, ACInfo
#   Developer/   the public headers
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
    DEPENDS Kinora demuxer_library audiocodec_library ffmpeg_codec DMXInfo ACInfo
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${PACKAGE_TREE}" "${PACKAGE_STAGE}" "${PACKAGE_ARCHIVE}"
    # what install() places, in a tree of its own: the build's install prefix is left alone
    COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" --prefix "${PACKAGE_TREE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PACKAGE_STAGE}/Kinora" "${PACKAGE_STAGE}/Licenses/ffmpeg"
    COMMAND ${CMAKE_COMMAND} -E copy "${PACKAGE_TREE}/Kinora" "${PACKAGE_TREE}/Kinora.info" "${PACKAGE_STAGE}/Kinora/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PACKAGE_TREE}/Fonts" "${PACKAGE_STAGE}/Kinora/Fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PACKAGE_TREE}/LIBS" "${PACKAGE_STAGE}/LIBS"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PACKAGE_TREE}/C" "${PACKAGE_STAGE}/C"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PACKAGE_TREE}/Developer" "${PACKAGE_STAGE}/Developer"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/installer/Install" "${CMAKE_BINARY_DIR}/installer/ReadMe"
            "${CMAKE_CURRENT_SOURCE_DIR}/installer/Install.info" "${CMAKE_CURRENT_SOURCE_DIR}/installer/ReadMe.info"
            "${PACKAGE_STAGE}/"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${PACKAGE_STAGE}/Licenses/MPL-2.0"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.LGPL-2.1" "${PACKAGE_STAGE}/Licenses/LGPL-2.1"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/external/freetype/docs/FTL.TXT"
            "${PACKAGE_STAGE}/Licenses/FreeType-FTL"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/dist/fonts/DejaVuSans.license.txt"
            "${PACKAGE_STAGE}/Licenses/DejaVuSans"
    # what the two LGPL libraries' FFmpeg was built from: the change to its source and the script that builds it
    COMMAND ${CMAKE_COMMAND} -E copy ${KINORA_FFMPEG_PATCHES} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-ffmpeg.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg/amiga_shim.h" "${PACKAGE_STAGE}/Licenses/ffmpeg/"
    # the archive holds the drawer, so that it unpacks into one
    COMMAND sh -c "cd '${PACKAGE_DIR}' && '${LHA_EXECUTABLE}' a '${PACKAGE_ARCHIVE}' '${PACKAGE_NAME}'"
    COMMAND ${CMAKE_COMMAND} -E echo "package: ${PACKAGE_ARCHIVE}"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Packaging ${PACKAGE_NAME}.lha"
    VERBATIM
)

# FFmpeg, built twice from the pinned submodule: once for demuxer.library, once for
# audiocodec's ffmpeg.codec. The two never share a binary (docs/architecture.md, section 2).
#
#   Kinora::ffmpeg_demuxer   libavformat + a libavcodec without decoders + libavutil
#   Kinora::ffmpeg_audio     libavcodec's audio decoders + libswresample + libavutil

set(KINORA_FFMPEG_SOURCE "${CMAKE_SOURCE_DIR}/external/ffmpeg")
set(KINORA_FFMPEG_BUILD "${CMAKE_BINARY_DIR}/ffmpeg")
set(KINORA_FFMPEG_TARGET_FLAGS "-mcrt=${M68K_CRT} -m${M68K_CPU} -m${M68K_FPU}-float")
file(GLOB KINORA_FFMPEG_PATCHES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/patches/*.patch")
set(KINORA_FFMPEG_PATCHES "${KINORA_FFMPEG_PATCHES}" PARENT_SCOPE) # the package target ships them beside the script

if(NOT EXISTS "${KINORA_FFMPEG_SOURCE}/configure")
    message(FATAL_ERROR "external/ffmpeg is empty: run git submodule update --init")
endif()

# Helpers both configurations link: the atomics FFmpeg expects from libatomic.
add_library(kinora_ffmpeg_amiga STATIC amiga_atomic.c)
target_compile_options(kinora_ffmpeg_amiga PRIVATE -O2 -Wall -Wextra -Werror)

# What a shared library that carries FFmpeg needs beside libnix's library start-up
# (amiga_library.h).  An object library: its list heads must come first in the link.
add_library(kinora_ffmpeg_library OBJECT amiga_library.c)
target_compile_options(kinora_ffmpeg_library PRIVATE ${KINORA_COMPILE_OPTIONS})
target_include_directories(kinora_ffmpeg_library PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The patched copy of the source both configurations build from. It is made again when
# the submodule moves to another commit or a patch changes.
set(KINORA_FFMPEG_COPY_STAMP "${KINORA_FFMPEG_BUILD}/source/.kinora-stamp")
set(KINORA_FFMPEG_HEAD "${CMAKE_SOURCE_DIR}/.git/modules/external/ffmpeg/HEAD")
if(NOT EXISTS "${KINORA_FFMPEG_HEAD}")
    set(KINORA_FFMPEG_HEAD "")
endif()
add_custom_command(
    OUTPUT "${KINORA_FFMPEG_COPY_STAMP}"
    COMMAND "${CMAKE_SOURCE_DIR}/scripts/build-ffmpeg.sh" source "${KINORA_FFMPEG_SOURCE}"
            "${CMAKE_SOURCE_DIR}/patches" "${KINORA_FFMPEG_BUILD}"
    DEPENDS "${CMAKE_SOURCE_DIR}/scripts/build-ffmpeg.sh" ${KINORA_FFMPEG_PATCHES} ${KINORA_FFMPEG_HEAD}
    COMMENT "Copying and patching FFmpeg's source"
    VERBATIM
)
add_custom_target(kinora_ffmpeg_source DEPENDS "${KINORA_FFMPEG_COPY_STAMP}")

# kinora_ffmpeg(<name> <library>...): one configuration and the target that carries it.
function(kinora_ffmpeg name)
    set(objects "${KINORA_FFMPEG_BUILD}/${name}")
    set(archives "")
    foreach(library IN LISTS ARGN)
        list(APPEND archives "${objects}/lib${library}/lib${library}.a")
    endforeach()

    add_custom_command(
        OUTPUT ${archives}
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/build-ffmpeg.sh" ${name}
                "${CMAKE_CURRENT_SOURCE_DIR}/amiga_shim.h" "${KINORA_FFMPEG_BUILD}"
                "${KINORA_FFMPEG_TARGET_FLAGS}"
        DEPENDS "${KINORA_FFMPEG_COPY_STAMP}" amiga_shim.h
        COMMENT "Building FFmpeg (${name})"
        VERBATIM
    )
    add_custom_target(kinora_ffmpeg_${name}_build DEPENDS ${archives})

    add_library(kinora_ffmpeg_${name} INTERFACE)
    add_library(Kinora::ffmpeg_${name} ALIAS kinora_ffmpeg_${name})
    add_dependencies(kinora_ffmpeg_${name}_build kinora_ffmpeg_source)
    add_dependencies(kinora_ffmpeg_${name} kinora_ffmpeg_${name}_build)
    # FFmpeg's headers are not ours to make warning-clean: SYSTEM keeps them quiet.
    target_include_directories(kinora_ffmpeg_${name} SYSTEM INTERFACE
        "${objects}" "${KINORA_FFMPEG_BUILD}/source")
    target_link_libraries(kinora_ffmpeg_${name} INTERFACE ${archives} kinora_ffmpeg_amiga m)
endfunction()

# Link order matters: a library before the ones it uses.
kinora_ffmpeg(demuxer avformat avcodec avutil)
kinora_ffmpeg(audio avcodec swresample avutil)

# FFmpeg, built three times from the pinned submodule: once for demuxer.library, once for
# audiocodec's ffmpegaudio.codec, once for videocodec's ffmpegvideo.codec. The three never share a
# binary (docs/architecture.md, section 2).
#
#   Kinora::ffmpeg_demuxer   libavformat + a libavcodec without decoders + libavutil
#   Kinora::ffmpeg_audio     libavcodec's audio decoders + libswresample + libavutil
#   Kinora::ffmpeg_video     libavcodec's video decoders + libavutil
#
# Each is built for Emu68 (section 3: -O3, misaligned reads, branchless clamps) and, by
# KINORA_PGO, laid out by its profile in profiles/<configuration>/, or instrumented to make one.

set(KINORA_FFMPEG_SOURCE "${CMAKE_SOURCE_DIR}/external/ffmpeg")
set(KINORA_FFMPEG_BUILD "${CMAKE_BINARY_DIR}/ffmpeg")
set(KINORA_FFMPEG_TARGET_FLAGS "-mcrt=${M68K_CRT} -m${M68K_CPU} -m${M68K_FPU}-float")
set(KINORA_FFMPEG_PROFILES "${CMAKE_SOURCE_DIR}/profiles")
file(GLOB KINORA_FFMPEG_PATCHES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/patches/*.patch")
set(KINORA_FFMPEG_PATCHES "${KINORA_FFMPEG_PATCHES}" PARENT_SCOPE) # the package target ships them beside the script

if(NOT EXISTS "${KINORA_FFMPEG_SOURCE}/configure")
    message(FATAL_ERROR "external/ffmpeg is empty: run git submodule update --init")
endif()

# The profiles are made for the default target; another one gets none.
set(KINORA_FFMPEG_PGO "${KINORA_PGO}")
if(KINORA_PGO STREQUAL "USE" AND NOT (M68K_CPU STREQUAL "68040" AND M68K_FPU STREQUAL "hard"))
    message(STATUS "FFmpeg: the profiles are for the 68040 with its FPU; built without them")
    set(KINORA_FFMPEG_PGO "OFF")
endif()
set(KINORA_FFMPEG_PGO "${KINORA_FFMPEG_PGO}" PARENT_SCOPE)

# Helpers every configuration links: the atomics FFmpeg expects from libatomic.
add_library(kinora_ffmpeg_amiga STATIC amiga_atomic.c)
target_compile_options(kinora_ffmpeg_amiga PRIVATE -O2 -Wall -Wextra -Werror)

# What a shared library that carries FFmpeg needs beside libnix's library start-up
# (amiga_library.h).  An object library: its list heads must come first in the link.
add_library(kinora_ffmpeg_library OBJECT amiga_library.c)
target_compile_options(kinora_ffmpeg_library PRIVATE ${KINORA_COMPILE_OPTIONS})
target_include_directories(kinora_ffmpeg_library PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(KINORA_FFMPEG_PGO STREQUAL "GENERATE")
    target_compile_definitions(kinora_ffmpeg_library PUBLIC KINORA_PGO_GENERATE)
endif()

# emu68-common's memcpy, memmove and memset in every library that carries FFmpeg, in place of
# libnix's, which call Exec's CopyMem() through the library vector for every copy: these are
# shaped for the Emu68 JIT (longword loops for short copies, 64-byte movem.l blocks for long
# ones, branches that keep a translated block whole) and were measured.  Objects, linked
# ahead of every library, so which ones the binaries take does not hang on the link's order.
add_library(kinora_ffmpeg_memory OBJECT
    ${CMAKE_SOURCE_DIR}/external/emu68-common/src/memcpy_movem.S
    ${CMAKE_SOURCE_DIR}/external/emu68-common/src/memmove_movem.S
    ${CMAKE_SOURCE_DIR}/external/emu68-common/src/memset_movem.S
)

# The patched copy of the source the configurations build from. It is made again when
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
    # the mode as a file whose time changes with it, and the profiles: either change builds it again
    set(mode_file "${KINORA_FFMPEG_BUILD}/${name}.pgo-mode")
    file(CONFIGURE OUTPUT "${mode_file}" CONTENT "${KINORA_FFMPEG_PGO}\n")
    file(GLOB_RECURSE profiles CONFIGURE_DEPENDS "${KINORA_FFMPEG_PROFILES}/${name}/*")

    add_custom_command(
        OUTPUT ${archives}
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/build-ffmpeg.sh" ${name}
                "${CMAKE_CURRENT_SOURCE_DIR}" "${KINORA_FFMPEG_BUILD}" "${KINORA_FFMPEG_TARGET_FLAGS}"
                "${KINORA_FFMPEG_PGO}" "${KINORA_FFMPEG_PROFILES}"
        DEPENDS "${KINORA_FFMPEG_COPY_STAMP}" amiga_shim.h branchless_clip.h "${mode_file}" ${profiles}
        COMMENT "Building FFmpeg (${name}, profile ${KINORA_FFMPEG_PGO})"
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
kinora_ffmpeg(video avcodec avutil)

# kinora_ffmpeg_binary(<target>): what every library carrying FFmpeg adds: emu68-common's
# memory functions (kinora_ffmpeg_memory); in a profile-generating build the profile writer and
# its private call (amiga_library.h), libnix's constructor runner, through which the
# profiling code registers, and libgcov's stream writer.
function(kinora_ffmpeg_binary target)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:kinora_ffmpeg_memory>)
    if(KINORA_FFMPEG_PGO STREQUAL "GENERATE")
        target_compile_definitions(${target} PRIVATE KINORA_PGO_GENERATE)
        target_sources(${target} PRIVATE "${CMAKE_SOURCE_DIR}/ffmpeg/pgo_dump.c")
        target_link_options(${target} PRIVATE -fprofile-arcs -Wl,-u,___initcpp)
        target_link_libraries(${target} PRIVATE gcov)
    endif()
endfunction()

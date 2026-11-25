# audiocodec.library (LIBS:): audio decoders behind one API (docs/architecture.md section 5).
# The frontend is MPL-2.0 and freestanding; the decoders are in its backend,
# LIBS:audiocodec/ffmpeg.codec, which is LGPL-2.1-or-later as the FFmpeg in it.

emu68_sfd_headers(audiocodec_sfd_headers ALL
    OUTPUT_DIR ${CMAKE_BINARY_DIR}/generated/include
    SFD ${CMAKE_CURRENT_SOURCE_DIR}/sfd/audiocodec_lib.sfd ${CMAKE_CURRENT_SOURCE_DIR}/sfd/audiocodec_backend_lib.sfd
)

add_executable(audiocodec_library
    src/audiocodec_lib.c
    src/audiocodec_backend.c
    src/audiocodec_session.c
)
target_compile_options(audiocodec_library PRIVATE ${KINORA_COMPILE_OPTIONS} -ffreestanding)
target_include_directories(audiocodec_library PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/generated/include
)
add_dependencies(audiocodec_library audiocodec_sfd_headers)
# emu68-common's freestanding string and memory functions
target_link_libraries(audiocodec_library PRIVATE Emu68Common::common)
target_link_options(audiocodec_library PRIVATE -nostdlib -nostartfiles -s)
emu68_module_layout(audiocodec_library)
set_target_properties(audiocodec_library PROPERTIES
    OUTPUT_NAME audiocodec
    SUFFIX ".library"
)

add_executable(ffmpeg_codec
    $<TARGET_OBJECTS:kinora_ffmpeg_library>
    ffmpeg/codec_lib.c
    ffmpeg/codec_table.c
    ffmpeg/codec_session.c
)
target_compile_options(ffmpeg_codec PRIVATE ${KINORA_COMPILE_OPTIONS})
target_include_directories(ffmpeg_codec PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/ffmpeg
)
target_link_libraries(ffmpeg_codec PRIVATE Kinora::ffmpeg_audio)
# libnix's library start-up: the resident tag and the standard vectors
target_link_options(ffmpeg_codec PRIVATE -ramiga-lib -s)
set_target_properties(ffmpeg_codec PROPERTIES
    OUTPUT_NAME ffmpeg
    SUFFIX ".codec"
)

install(TARGETS audiocodec_library RUNTIME DESTINATION LIBS)
install(TARGETS ffmpeg_codec RUNTIME DESTINATION LIBS/audiocodec)
install(FILES ${CMAKE_SOURCE_DIR}/include/libraries/audiocodec_backend.h DESTINATION Developer/include/libraries)

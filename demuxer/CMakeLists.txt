# demuxer.library (LIBS:): a container file's streams and packets, FFmpeg's libavformat
# behind our own API (docs/architecture.md section 4).  LGPL-2.1-or-later, as the FFmpeg in it.

emu68_sfd_headers(demuxer_sfd_headers ALL
    OUTPUT_DIR ${CMAKE_BINARY_DIR}/generated/include
    SFD ${CMAKE_CURRENT_SOURCE_DIR}/sfd/demuxer_lib.sfd
)

add_executable(demuxer_library
    $<TARGET_OBJECTS:kinora_ffmpeg_library>
    src/demuxer_lib.c
    src/demuxer_source.c
    src/demuxer_codecs.c
    src/demuxer_reader.c
)
target_compile_options(demuxer_library PRIVATE ${KINORA_COMPILE_OPTIONS})
target_include_directories(demuxer_library PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/ffmpeg
)
target_link_libraries(demuxer_library PRIVATE Kinora::ffmpeg_demuxer)
# libnix's library start-up: the resident tag and the standard vectors
target_link_options(demuxer_library PRIVATE -ramiga-lib -s)
kinora_ffmpeg_binary(demuxer_library)
set_target_properties(demuxer_library PROPERTIES
    OUTPUT_NAME demuxer
    SUFFIX ".library"
)

install(TARGETS demuxer_library RUNTIME DESTINATION LIBS)
install(FILES ${CMAKE_SOURCE_DIR}/include/libraries/demuxer.h ${CMAKE_SOURCE_DIR}/include/libraries/audiocodec.h
        DESTINATION Developer/include/libraries)

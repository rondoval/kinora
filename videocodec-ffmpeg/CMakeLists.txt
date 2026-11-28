# ffmpegvideo.codec (LIBS:videocodec/): videocodec.library's backend with FFmpeg's video
# decoders (docs/architecture.md, section 14). LGPL-2.1-or-later, as the FFmpeg in it.

add_executable(ffmpegvideo_codec
    $<TARGET_OBJECTS:kinora_ffmpeg_library>
    codec_lib.c
    codec_table.c
    codec_session.c
    codec_process.c
    codec_frames.c
    codec_memory.c
)
target_compile_options(ffmpegvideo_codec PRIVATE ${KINORA_COMPILE_OPTIONS})
target_include_directories(ffmpegvideo_codec PRIVATE ${CMAKE_SOURCE_DIR}/ffmpeg)
target_link_libraries(ffmpegvideo_codec PRIVATE Kinora::ffmpeg_video Emu68Common::common) # emu68-common: the DMA pool (codec_memory.c)
# libnix's library start-up: the resident tag and the standard vectors
target_link_options(ffmpegvideo_codec PRIVATE -ramiga-lib -s)
kinora_ffmpeg_binary(ffmpegvideo_codec)
set_target_properties(ffmpegvideo_codec PROPERTIES
    OUTPUT_NAME ffmpegvideo
    SUFFIX ".codec"
)

install(TARGETS ffmpegvideo_codec RUNTIME DESTINATION LIBS/videocodec)

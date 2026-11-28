# The command-line tools (C:).

# FilmInfo: what demuxer.library makes of a film, its sound and video decoded, the codec tables
add_executable(FilmInfo filminfo/filminfo.c filminfo/filminfo_file.c filminfo/filminfo_audio.c filminfo/filminfo_video.c
               filminfo/md5.c)
target_compile_options(FilmInfo PRIVATE ${KINORA_COMPILE_OPTIONS})
target_include_directories(FilmInfo PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated/include)
add_dependencies(FilmInfo demuxer_sfd_headers audiocodec_sfd_headers)
target_link_options(FilmInfo PRIVATE -s)
install(TARGETS FilmInfo RUNTIME DESTINATION C)

# PgoDump: a profile-generating library's profile written (KINORA_PGO GENERATE only; not installed)
if(KINORA_FFMPEG_PGO STREQUAL "GENERATE")
    add_executable(PgoDump PgoDump.c)
    target_compile_options(PgoDump PRIVATE ${KINORA_COMPILE_OPTIONS})
    target_link_options(PgoDump PRIVATE -s)
endif()

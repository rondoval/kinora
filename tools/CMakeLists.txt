# The command-line tools (C:).

# DMXInfo: what demuxer.library makes of a file
add_executable(DMXInfo DMXInfo.c)
target_compile_options(DMXInfo PRIVATE ${KINORA_COMPILE_OPTIONS})
target_include_directories(DMXInfo PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated/include)
add_dependencies(DMXInfo demuxer_sfd_headers)
target_link_options(DMXInfo PRIVATE -s)
install(TARGETS DMXInfo RUNTIME DESTINATION C)

# ACInfo: what audiocodec.library decodes, and a file's audio stream decoded through it
add_executable(ACInfo ACInfo.c)
target_compile_options(ACInfo PRIVATE ${KINORA_COMPILE_OPTIONS})
target_include_directories(ACInfo PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated/include)
add_dependencies(ACInfo demuxer_sfd_headers audiocodec_sfd_headers)
target_link_options(ACInfo PRIVATE -s)
install(TARGETS ACInfo RUNTIME DESTINATION C)

# VCDecode: what videocodec.library makes of a file's video: its pictures, their digests, the time
add_executable(VCDecode VCDecode.c md5.c)
target_compile_options(VCDecode PRIVATE ${KINORA_COMPILE_OPTIONS})
target_include_directories(VCDecode PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated/include)
add_dependencies(VCDecode demuxer_sfd_headers)
target_link_options(VCDecode PRIVATE -s)
install(TARGETS VCDecode RUNTIME DESTINATION C)

# PgoDump: a profile-generating library's profile written (KINORA_PGO GENERATE only; not installed)
if(KINORA_FFMPEG_PGO STREQUAL "GENERATE")
    add_executable(PgoDump PgoDump.c)
    target_compile_options(PgoDump PRIVATE ${KINORA_COMPILE_OPTIONS})
    target_link_options(PgoDump PRIVATE -s)
endif()

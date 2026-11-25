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

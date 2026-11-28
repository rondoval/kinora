# Kinora, the player (docs/architecture.md sections 6 to 11).  MPL-2.0.

add_executable(Kinora
    main.c
    engine/clock.c
    engine/queue.c
    engine/engine.c
    engine/engine_file.c
    engine/engine_reader.c
    engine/engine_video.c
    engine/engine_audio.c
    engine/engine_stats.c
    audio/audio_out.c
    audio/audio_low.c
    audio/audio_device.c
    audio/audio_unit.c
    display/display.c
    display/display_window.c
    display/display_screen.c
    subtitles/subtitles_text.c
    subtitles/subtitles_cues.c
    subtitles/subtitles_font.c
    subtitles/subtitles.c
    subtitles/subtitles_layout.c
    subtitles/subtitles_write.c
    subtitles/subtitles_panel.c
    gui/gui.c
    gui/gui_menu.c
    gui/gui_screen.c
    gui/gui_playlist.c
    gui/gui_list.c
    gui/gui_settings.c
    gui/gui_rexx.c
    gui/gui_wb.c
    gui/gui_jump.c
    gui/settings.c
)
target_compile_options(Kinora PRIVATE ${KINORA_COMPILE_OPTIONS})
option(KINORA_TRACE "The engine's perf slots on Emu68's debug port (engine/trace.h); never for a release" OFF)
if(KINORA_TRACE)
    target_sources(Kinora PRIVATE engine/trace.c)
    target_compile_definitions(Kinora PRIVATE KINORA_TRACE DEBUG_SINK PROFILE)
    target_link_libraries(Kinora PRIVATE Emu68Common::common)
endif()
target_include_directories(Kinora PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated/include)
add_dependencies(Kinora demuxer_sfd_headers audiocodec_sfd_headers)
target_link_libraries(Kinora PRIVATE Kinora::freetype)
target_link_options(Kinora PRIVATE -s)
install(TARGETS Kinora RUNTIME DESTINATION .)
# The icon is a committed file, made from dist/icons/ by make_icons.py
install(FILES ${CMAKE_SOURCE_DIR}/dist/Kinora.info DESTINATION .)
# The font subtitles are drawn with, and its licence, in the drawer Fonts beside the program
install(FILES ${CMAKE_SOURCE_DIR}/dist/fonts/DejaVuSans.ttf ${CMAKE_SOURCE_DIR}/dist/fonts/DejaVuSans.license.txt DESTINATION Fonts)

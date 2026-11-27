# Release notes - Kinora 0.2

- **Films in the older video codecs play now:** MPEG-1, MPEG-2, MPEG-4 part 2 (Xvid, DivX),
  DivX 3, H.263 and Theora. The new `ffmpegvideo.codec` decodes them on the 68k; when it
  cannot keep up, Kinora skips pictures instead of falling behind.
- **AVI and MPEG programme streams play:** Xvid, DivX and DivX 3 in `.avi` and `.divx`,
  VCD and DVD files (`.mpg`, `.vob`), with their sound in ADPCM, A-law, µ-law or a DVD's
  LPCM too. A DVD's menus do not: play its `.vob` files one by one.
- **B-pictures come in their order** where a file times them as they are decoded (AVI, Xvid
  in Matroska), and a move in a DVD file starts at a whole picture.
- **The video libraries come with Kinora now:** `videocodec.library`, `videocore.codec`
  (H.264, MJPEG, pictures) and `rpivid.codec` (H.265), with `VCInfo` to check them. From
  the Emu68 driver stack Kinora needs only `vchiq.device`.
- **Two optional extras:** `vcjpeg.datatype` decodes large JPEGs on the VideoCore for every
  program that uses datatypes, and `VCGrab` records the screen to a film file.
- **The FFmpeg codecs are optimized for the 68k under Emu68:** sound and reading take less
  of the processor.
- **The sound decoder is renamed `ffmpegaudio.codec`;** the Installer removes the old
  `ffmpeg.codec`.
- **The statistics (key I)** name the codec drivers in use and fit smaller windows.
- **Project > Close** empties the player and keeps the playlist (ARexx: `CLOSE`).
- **`FilmInfo` replaces `DMXInfo` and `ACInfo`,** and can also test-decode a film's video.
  The Installer removes the old tools.
- **If you use Executive,** the ReadMe's `TASK Kinora* NOSCHEDULE` line covers the new
  decoder too.


# Release notes - Kinora 0.1

The first release: a film player for AmigaOS 3.2 on a PiStorm with a Raspberry Pi 4 or CM4
under Emu68. Matroska, WebM, MP4, QuickTime and MPEG transport streams; H.264, and H.265 on
a Pi 4 or CM4, decoded on the Pi; the common audio codecs through AHI, mixed down to stereo
unless the AHI unit is a 7.1 one; text subtitles drawn into the picture; the window or full
screen; playlists, chapters, tracks and delays; an ARexx port. It needed `videocodec.library`
0.6 from the Emu68 driver stack.

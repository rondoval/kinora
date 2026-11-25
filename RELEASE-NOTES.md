# Release notes - Kinora 0.2

- **The video libraries come with Kinora now.** `videocodec.library` and its codec
  drivers, `videocore.codec` (H.264, MJPEG and pictures on the Pi's VideoCore) and
  `rpivid.codec` (H.265 on the HEVC block of a Pi 4 or CM4), are in this archive, with
  `VCInfo`, which checks them. The Installer installs them with the player, or alone for
  other programs that use them.
- **Two optional parts come with them:** `vcjpeg.datatype`, JPEG pictures of about two
  megapixels and more decoded on the VideoCore for every program that loads pictures
  through datatypes, and `VCGrab`, a commodity that records the screen to a film file.
  Each starts ticked in the Installer once it is installed.
- **What Kinora needs from the Emu68 driver stack is `vchiq.device` alone**, the link to
  the VideoCore (2.4.0's Install script offers it as a part of its own). The video
  libraries the stack brought up to 2.3 are replaced by this archive's, by their version.
- `VCInfo` checks the video side: each codec driver, a session of each opened and closed,
  and `vcjpeg.datatype`'s place among the JPEG descriptors. When the VideoCore does not
  answer, it sends you to the driver stack's `VCHIQInfo`.


# Release notes - Kinora 0.1

The first release: a film player for AmigaOS 3.2 on a PiStorm with a Raspberry Pi 4 or CM4
under Emu68. Matroska, WebM, MP4, QuickTime and MPEG transport streams; H.264, and H.265 on
a Pi 4 or CM4, decoded on the Pi; the common audio codecs through AHI, mixed down to stereo
unless the AHI unit is a 7.1 one; text subtitles drawn into the picture; the window or full
screen; playlists, chapters, tracks and delays; an ARexx port. It needed `videocodec.library`
0.6 from the Emu68 driver stack.

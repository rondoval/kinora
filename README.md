# Kinora

A film player for AmigaOS 3.2 on a PiStorm with a Raspberry Pi 4 or CM4 under Emu68, in the
style of Media Player Classic: one window, the picture above simple playback controls,
everything else in the menus. The Pi's VideoCore decodes H.264 and H.265, the 68k the older
codecs; the sound plays through AHI, and text subtitles are drawn into the picture. Films play
in the window or full screen.

## Installing

Download `Kinora-<version>.lha` and unpack it anywhere, for example with
`lha x Kinora-<version>.lha RAM:`. It unpacks into a drawer `Kinora-<version>`: open it and
double-click `Install`. The Installer asks what to install and where the drawer `Kinora` goes
(`SYS:Utilities` unless you choose another), and says which of the parts Kinora needs (below)
it did not find. Kinora can be installed first and those parts later; until then films do not
open.

| What | Where |
|---|---|
| `Kinora`, its icon, the drawer `Fonts` | the drawer chosen |
| `demuxer.library`, `audiocodec.library` | `LIBS:` |
| `ffmpegaudio.codec` | `LIBS:audiocodec/` |
| the video libraries: `videocodec.library` | `LIBS:` |
| `videocore.codec` (H.264, pictures), `rpivid.codec` (H.265), `ffmpegvideo.codec` (the older codecs) | `LIBS:videocodec/` |
| `VCInfo`, which checks the video libraries | `C:` |
| `vcjpeg.datatype`, JPEG pictures decoded on the VideoCore | `SYS:Classes/DataTypes/` and `SYS:Storage/DataTypes/`, if wanted |
| `VCGrab`, a screen recorder | `SYS:Tools/Commodities/`, if wanted |
| `FilmInfo`, which says what a film holds | `C:`, if wanted |

The video libraries can also be installed alone, for other programs that use them. Files
already there are updated; the Installer asks before it replaces a newer one with an older
one. To remove Kinora, delete its drawer, the files above and the `AddDataTypes` line
`vcjpeg.datatype` put in `S:User-Startup`. Its settings are in `ENVARC:Kinora.prefs`.

## What it needs

- AmigaOS 3.2 on a PiStorm with a Raspberry Pi 4 or CM4 under Emu68. A Pi 3 has no H.265
  decoder and is untested.
- Emu68 1.1 built from its `1_1_wip` branch on 23 October 2026 or later (commit `7384501`), or
  a release after that. Older builds get one kind of multiplication wrong, and several of the
  sound decoders then fail to open: films play without sound.
- `vchiq.device`, the link to the Pi's VideoCore, from the
  [Emu68 driver stack](https://github.com/rondoval/emu68-driver-stack) (a part of its Install
  script).
- Picasso96 with VideoCore.card.
- AHI.

For full screen, give the graphics card 32 MB with this line in `config.txt` on the Pi's boot
partition, then restart the Pi (Ctrl-Amiga-Amiga, or off and on):

```
dtoverlay=videocore,mem=32
```

Full screen keeps four pictures in the card's memory. Emu68's default of 16 MB has room for
one at 1080p, and full screen then plays less smoothly than the window.

## What it plays

| | Supported |
|---|---|
| Containers | Matroska and WebM (`.mkv`, `.webm`); MP4 and QuickTime (`.mp4`, `.m4v`, `.mov`); MPEG transport streams (`.ts`, `.m2ts`); MPEG programme streams, as VCDs and DVDs hold them (`.mpg`, `.mpeg`, `.vob`); AVI (`.avi`, `.divx`); MPEG-1 and MPEG-2 video alone (`.m1v`, `.m2v`) |
| Video | H.264; H.265 (HEVC) on a Pi 4 or CM4; on the 68k: MPEG-1, MPEG-2, MPEG-4 part 2 (Xvid, DivX), DivX 3, H.263, Theora |
| Audio | AAC (LC, HE, HE v2, also as LATM in transport streams); AC-3; E-AC-3; DTS; Dolby TrueHD; MP3; MP2; Opus; Vorbis; FLAC; PCM (8, 16, 24 and 32 bit, 32-bit float); a DVD's LPCM; IMA and Microsoft ADPCM; A-law and µ-law |
| Subtitles | SubRip, ASS/SSA and MP4 timed text inside the film; a SubRip file (`.srt`) beside it or opened by hand |

Not played: other video codecs (VP8, VP9, AV1, VC-1), picture subtitles (Blu-ray PGS, DVD
VobSub, DVB), a DVD's menus (its `.vob` files play one at a time), and anything over the
network. H.264 with B-pictures in AVI, which is rare, comes out of order.

The codecs the 68k decodes take much of the processor: films of DVD size keep up, larger ones
may not. Then Kinora skips pictures and the sound goes on. Interlaced video (from television
and DVDs) is shown combed.

Sound with more than two channels is mixed down to stereo unless the AHI unit's mode is a 7.1
one. Files over 2 GiB play from file systems that can seek that far; PFS3 cannot.

## Using it

Start Kinora from its icon, or from a shell with films to play: `Kinora film.mkv other.mp4`.
Drop films on the window or on the icon, or open them from the Project menu; Project > Open
recent plays one of the last five films. Project > Close empties the player and keeps the
playlist.

The playlist window (Playlist > Show the playlist..., or L) lists the films, the one playing
marked. A double click or Return plays a film, Del takes it out, Up and Down move it, and
icons dropped on the window join the list. Taken out, the film playing plays on.

| Key | Does |
|---|---|
| Space, or a click on the picture in the window | play, pause |
| A double click on the picture | full screen and back |
| Cursor left, right | ten seconds back, on |
| F | full screen and back; Esc leaves it |
| A, S | the next audio track, the next subtitle track |
| PgUp, PgDn | the playlist's film before, the next one |
| L | the playlist window |
| J (with the right Amiga key) | jump to a time |
| I | statistics in the picture's corner, and away again |

In full screen the controls come up along the screen's foot when the pointer moves; the
pointer itself shows only while the film is paused (Space), since the picture flips under it
while it plays. The full screen takes the Workbench's screen mode, or one chosen in the
settings: a 50 Hz mode shows films of 25 pictures a second smoothly, and a smaller mode leaves
fewer pixels to fill. Moving in a film lands on the key frame before the place asked for,
which can be some seconds early.

Subtitles come from the film or from a `.srt` file of the same name beside it. They are drawn
with DejaVu Sans, which has Latin, Greek and Cyrillic letters; for other scripts choose a
TrueType or OpenType font that has them in the settings. Scripts that join their letters or
run from the right do not come out right.

Project > Settings... opens the settings:

| Group | Sets |
|---|---|
| Sound | the AHI unit, and whether to share it with other programs |
| Subtitles | the font file (the Kinora's button goes back to Kinora's own), the size (50 to 200 %), the distance from the picture's foot (0 to 30 % of its height) |
| Full screen | the Workbench's screen mode, or one of the modes Kinora can show in |

What you change shows at once; pause the film to judge the subtitles. Save keeps the settings
for later runs, Cancel puts back the ones the window opened with.

Kinora plays 7.1 where the unit's mode in AHI's prefs is a 7.1 one, and stereo otherwise.
While a film is loaded Kinora holds the unit's sound hardware, so other programs may not be
heard through it. Sharing the unit they can, and Kinora plays 6 dB quieter; it shares the unit
that way too when another program already holds it.

The volume, the last drawer, the recent films and the playlist window's place are remembered
from one run to the next.

## When pictures are dropped, or a film does not play

The key I shows what is playing, which codec drivers decode it, and whether it keeps up. The
ARexx command `STATISTICS` answers the same and more, for a report of a film that does not
play as it should.

If you use Executive, it moves programs' priorities by how much they use the processor, and in
full screen that costs Kinora pictures. To keep Kinora's own priorities, add this line to
`S:Executive.prefs`, with tabs between its fields, and reboot:

```
TASK		Kinora*		NOSCHEDULE
```

ExecutivePrefs makes the same entry: on its Tasks page a new one of type TASK, named
`Kinora*`, its action NOSCHEDULE and its priority left empty.

`VCInfo` checks the video libraries and says what to fix; `VCInfo CODECS` lists what they
decode. When the VideoCore does not answer, it sends you to the driver stack's `VCHIQInfo`.

`FilmInfo` tells what the libraries make of a film:

| Command | Says |
|---|---|
| `FilmInfo` | the sound and video codecs that can be decoded |
| `FilmInfo film.mkv` | the film's container, streams and chapters |
| `FilmInfo film.mkv AUDIO` | decodes the sound and says what came out |
| `FilmInfo film.mkv VIDEO` | decodes the video as fast as it can and says what came out and how long it took |

A decode goes through the whole film, which on the 68k takes as long as the film or longer:
add `PACKETS 500` for the first 500 packets. Ctrl-C stops it and prints what came out.

## Driving it from other programs

Kinora's ARexx port is `KINORA`. Its commands: `PLAY`, `PAUSE`, `STOP`, `SEEK <ms>`,
`OPEN <file>`, `CLOSE`, `ADD <file>`, `NEXT`, `PREVIOUS`, `FULLSCREEN`, `SIZE <width> <height>`,
`FONT <file>`, `SUBTITLES <file>`, `STATUS`, `STATISTICS`, `TRACKS`, `AUDIO`, `SUBTITLE`,
`UNIT`, `VOLUME`, `ASPECT`, `DELAY`, `CHAPTERS`, `QUIT`, and:

| Command | Does |
|---|---|
| `PLAYLIST [SHOW\|HIDE\|CLEAR\|REMOVE n\|UP n\|DOWN n]` | lists the films, the one playing marked `*`; shows or hides the playlist window, or changes the list first (n counts from 1) |
| `RECENT [n]` | lists the recent films, newest first; with a number, plays that one |
| `SUBTITLESIZE [percent]`, `SUBTITLEDISTANCE [percent]` | the subtitles' size and distance from the edge, set with a number |
| `SCREENMODE [WORKBENCH\|id]` | the full screen's mode: the setting and the mode it opens in |
| `SETTINGS [SAVE\|CANCEL]` | opens the settings window; with a word, does what its button does |

Answers come in `RESULT`.

## Building from source

Docker is the only requirement; the cross compiler comes in a container.

```sh
git submodule update --init --recursive
./build.sh               # the build
./build.sh --package     # the release archive, with its Installer script
```

How Kinora is built and tested, and why, is in [docs/architecture.md](docs/architecture.md).

## Licences

Kinora, `audiocodec.library` and `FilmInfo` are under the Mozilla Public
License 2.0 ([LICENSE](LICENSE)). `demuxer.library`, `audiocodec/ffmpegaudio.codec` and
`videocodec/ffmpegvideo.codec` contain FFmpeg and are under the GNU Lesser General Public
License, version 2.1 or later ([LICENSE.LGPL-2.1](LICENSE.LGPL-2.1)). FFmpeg is built without
its GPL and non-free parts, from the unmodified `external/ffmpeg` submodule and the changes in
`patches/`, with the profiles in `profiles/`.

`Kinora` contains FreeType, used under the FreeType License: portions of this software are
copyright (c) 2026 The FreeType Project (www.freetype.org). All rights reserved. The font
DejaVu Sans comes with its own licence
([dist/fonts/DejaVuSans.license.txt](dist/fonts/DejaVuSans.license.txt)).

The video libraries are repositories of their own, built from the submodules
`external/videocodec-library` and `external/rpivid-codec`: `videocodec.library`,
`videocore.codec`, `vcjpeg.datatype`, `VCInfo` and `VCGrab` are under the BSD 3-Clause License
([videocodec-library](https://github.com/rondoval/videocodec-library)), `rpivid.codec` under
the GNU General Public License, version 2, with code from Linux's HEVC driver
([rpivid-codec](https://github.com/rondoval/rpivid-codec)).

The archive holds all of these in its drawer `Licenses`, and the headers for programs that use
the libraries in `Developer/include`.

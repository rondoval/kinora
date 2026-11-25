# Kinora

A film player for AmigaOS 3.2 on a PiStorm with a Raspberry Pi 4 or CM4 under Emu68, in the
style of Media Player Classic: one window, the picture above simple playback controls,
everything else in the menus. The Pi's VideoCore decodes the picture, the sound plays through
AHI, and text subtitles are drawn into the picture. Films play in the window or full screen.

## Installing

Download `Kinora-<version>.lha` and unpack it anywhere, for example with
`lha x Kinora-<version>.lha RAM:`. It unpacks into a drawer `Kinora-<version>`: open it and
double-click `Install`. The Installer asks where the drawer `Kinora` goes (`SYS:Utilities`
unless you choose another) and whether the two shell tools go to `C:`, and says which of the
parts Kinora needs (below) it did not find. Kinora can be installed first and those parts
later; until then films do not open.

| What | Where |
|---|---|
| `Kinora`, its icon, the drawer `Fonts` | the drawer chosen |
| `demuxer.library`, `audiocodec.library` | `LIBS:` |
| `ffmpeg.codec` | `LIBS:audiocodec/` |
| `DMXInfo`, `ACInfo` | `C:`, if wanted |

A library that is already there is replaced when the archive's is newer, or another build of
the same version; before an older one replaces a newer one, the Installer asks. To remove
Kinora, delete its drawer, the three libraries and the two tools. Its settings are in
`ENVARC:Kinora.prefs`.

## What it needs

- AmigaOS 3.2 on a PiStorm with a Raspberry Pi 4 or CM4 under Emu68. A Pi 3 plays H.264 only
  and is untested.
- `videocodec.library` 0.6 with its codecs, `videocore.codec` for H.264 and `rpivid.codec` for
  H.265, from the [Emu68 driver stack](https://github.com/rondoval/emu68-driver-stack).
- Picasso96 with VideoCore.card.
- AHI.

For full screen, give the graphics card 32 MB with this line in `config.txt` on the Pi's boot
partition, then restart the Pi (Ctrl-Amiga-Amiga, or off and on):

```
dtoverlay=videocore,mem=32
```

Full screen keeps up to four pictures in the card's memory and needs all four to play
smoothly. Emu68's default of 16 MB holds one at 1920 by 1080 in 24 bit, beside the Workbench.

## What it plays

| | Supported |
|---|---|
| Containers | Matroska and WebM (`.mkv`, `.webm`); MP4 and QuickTime (`.mp4`, `.m4v`, `.mov`); MPEG transport streams (`.ts`, `.m2ts`) |
| Video | H.264; H.265 (HEVC) on a Pi 4 or CM4 |
| Audio | AAC (LC, HE, HE v2, also as LATM in transport streams); AC-3; E-AC-3; DTS; Dolby TrueHD; MP3; MP2; Opus; Vorbis; FLAC; PCM (8, 16, 24 and 32 bit, 32-bit float) |
| Subtitles | SubRip, ASS/SSA and MP4 timed text inside the film; a SubRip file (`.srt`) beside it or opened by hand |

Not played: other video codecs (MPEG-2, MPEG-4 part 2, VC-1, VP9, AV1), picture subtitles
(Blu-ray PGS, DVD VobSub, DVB), and anything over the network. Sound with more than two
channels is mixed down to stereo unless the AHI unit's mode is a 7.1 one. Files over 2 GiB
play from file systems that can seek that far; PFS3 cannot.

## Using it

Start Kinora from its icon, or from a shell with films to play: `Kinora film.mkv other.mp4`.
Drop films on the window or on the icon, or open them from the Project menu.

| Key | Does |
|---|---|
| Space, or a click on the picture in the window | play, pause |
| A double click on the picture | full screen and back |
| Cursor left, right | ten seconds back, on |
| F | full screen and back; Esc leaves it |
| A, S | the next audio track, the next subtitle track |
| PgUp, PgDn | the playlist's film before, the next one |
| J (with the right Amiga key) | jump to a time |
| I | statistics in the picture's corner, and away again |

In full screen the controls come up along the screen's foot when the pointer moves, and the
picture stands above them meanwhile. Moving in a film lands on the key frame before the place
asked for, which can be some seconds early.

Subtitles come from the film or from a `.srt` file of the same name beside it. They are drawn
with the font DejaVu Sans, which lies in the drawer `Fonts` beside the program and has Latin,
Greek and Cyrillic letters. For other scripts choose a TrueType or OpenType font that has them
(Subtitles menu, Font...). Characters are placed one by one from the left, so scripts that join
their letters or run from the right do not come out right. Italic and bold are honoured, and
so is a cue's wish to stand at the top. Where the picture leaves a black bar under it, a cue
that fits goes there.

The Audio menu chooses the AHI unit the sound goes to. Kinora plays 7.1 where the unit's mode
in AHI's prefs is a 7.1 one, and stereo otherwise, at the film's own level; when it mixes
surround sound down to stereo, its loudest moments are softened rather than clipped. For that,
while a film is loaded Kinora holds the unit's sound hardware, and other programs may not be
heard through it meanwhile. With "Share the unit with other programs" (Audio menu) Kinora
plays through ahi.device the usual way instead: others can be heard too, and Kinora plays
6 dB quieter. It shares the unit that way as well when another program already holds it.

The unit, sharing it, the volume, the font and the last drawer are remembered.

## When pictures are dropped, or a film does not play

The key I shows what is being played and how: the streams, pictures a second, pictures
dropped, what waits in the buffers, the clock.

On AmiKit, Executive moves programs' priorities by how much they use the processor, and in
full screen that costs Kinora pictures: in a heavy 1080p film it dropped several times as
many. To keep Kinora's own priorities, add this line to `S:Executive.prefs`, with tabs between
its fields, and reboot:

```
TASK		Kinora*		NOSCHEDULE
```

ExecutivePrefs makes the same entry: on its Tasks page a new one of type TASK, named
`Kinora*`, its action NOSCHEDULE and its priority left empty.

Full screen with less than 32 MB for the graphics card (What it needs, above) plays less
smoothly than in the window.

The two shell tools tell what the libraries make of a film:

| Command | Says |
|---|---|
| `DMXInfo film.mkv` | what `demuxer.library` makes of the file: container, streams, chapters |
| `ACInfo` | the audio codecs `audiocodec.library` decodes |
| `ACInfo film.mkv` | decodes the film's audio and says what came out |

## Driving it from other programs

Kinora's ARexx port is `KINORA`. Its commands: `PLAY`, `PAUSE`, `STOP`, `SEEK <ms>`,
`OPEN <file>`, `ADD <file>`, `NEXT`, `PREVIOUS`, `FULLSCREEN`, `SIZE <width> <height>`,
`FONT <file>`, `SUBTITLES <file>`, `STATUS`, `STATISTICS`, `TRACKS`, `AUDIO`, `SUBTITLE`,
`UNIT`, `VOLUME`, `ASPECT`, `DELAY`, `CHAPTERS`, `PLAYLIST` and `QUIT`. Answers come in
`RESULT`.

## Building from source

Docker is the only requirement; the cross compiler comes in a container.

```sh
git submodule update --init
./build.sh                               # the build
./build.sh --package                     # the release archive, with its Installer script
tests/run.sh host                        # the code gates
tests/run.sh hw --server amiga-pi4       # the tests on an Amiga (docs/architecture.md, section 13)
```

How Kinora is built, and why, is in [docs/architecture.md](docs/architecture.md).

## Licences

Kinora, `audiocodec.library`, `DMXInfo` and `ACInfo` are under the Mozilla Public License 2.0
([LICENSE](LICENSE)). `demuxer.library` and `audiocodec/ffmpeg.codec` contain FFmpeg and are
under the GNU Lesser General Public License, version 2.1 or later
([LICENSE.LGPL-2.1](LICENSE.LGPL-2.1)). FFmpeg is built without its GPL and non-free parts,
from the unmodified `external/ffmpeg` submodule and the changes in `patches/`.

`Kinora` contains FreeType, used under the FreeType License: portions of this software are
copyright (c) 2026 The FreeType Project (www.freetype.org). All rights reserved. The font
DejaVu Sans comes with its own licence
([dist/fonts/DejaVuSans.license.txt](dist/fonts/DejaVuSans.license.txt)).

The archive holds all of these in its drawer `Licenses`, and the headers for programs that use
the two libraries in `Developer/include`.

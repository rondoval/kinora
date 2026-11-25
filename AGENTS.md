# Kinora: notes for agents

A video player for AmigaOS 3.2 on a PiStorm with a Raspberry Pi 4 or CM4 under Emu68, and the
two libraries it needs beside `videocodec.library`. What must not break is here; how it works
and why is in `docs/architecture.md`.

## Docs

`docs/architecture.md` is for whoever changes the design, `README.md` for end users; this file
holds the rules only: no history, no how-it-works.

## Build and validate

- `./build.sh` (or `scripts/docker-build.sh [--target <t>]`): everything in the build
  container. Zero warnings; our own code is built with `-Werror`.
- `./build.sh --package`: `build/package/Kinora-<version>-<stamp>.lha` with the Installer
  script (`installer/Install.in`). The script names its sources from its own drawer
  (`#archive`), ends with its own message and `(exit (quiet))`, and keeps every page within
  ten lines of fifty columns. A file added to the archive needs its line in the script, in
  `installer/ReadMe.in` and in the `package` target. Icons are committed; `dist/icons/`
  regenerates them.
- `KINORA_CONFIGURE_ARGS="-DKINORA_TRACE=ON" ./build.sh`: the engine's and the flipper's
  timings as emu68-common perf slots (`player/engine/trace.h`) on Emu68's debug port every
  2 s, for emu68-common's `scripts/perf-report.py`. Never for a release; build with
  `-DKINORA_TRACE=OFF` again after.
- `tests/run.sh host` after any change to C: the code gates (a kernel-doc comment on every
  function, cyclomatic complexity 10 at most, 15 with an entry and a reason in
  `tests/complexity-allow.txt`).
- `tests/run.sh hw --server amiga-pi4` after a change to a library, a tool or the player: it
  installs the build in its real places, then runs the libraries' tools and drives Kinora
  through its ARexx port (`-m smoke` for the quick part, `--no-install` to check instead
  of install). A new feature gets an ARexx command if it has none, and a test.
- Hardware runs are the user's to allow. The machines are shared with other sessions: ask
  before any call to an Amiga. Test files go to `RAM:kinora`; run anything that can crash
  detached (`Run >NIL:`), since a crash inside an agent command blocks the agent.

## Rules

1. **Licences by binary.** `Kinora`, `audiocodec.library` and the tools are MPL-2.0.
   `demuxer.library` and `audiocodec/ffmpeg.codec` contain FFmpeg and are
   LGPL-2.1-or-later, our code in them included. FFmpeg is configured without its GPL,
   version 3 and non-free parts; `scripts/build-ffmpeg.sh` stops if FFmpeg's configure says
   anything else. `Kinora` links FreeType (`external/freetype`) under the FreeType License,
   not its GPL choice: the credit line in `README.md` is what that licence asks for.
2. **The demuxer and the audio decoders never share a binary.** Each has its own FFmpeg
   configuration. Neither includes the other's headers beyond the codec numbers.
3. **No FFmpeg type, constant or structure in a public header** (`include/`). A caller
   never sees FFmpeg.
4. **`external/ffmpeg` stays as upstream tagged it.** Changes are patches in `patches/`,
   applied to the build tree's copy.
5. **Public structures start with `Size` and grow only at the end**; new functions go at
   the end of the SFD. Times are `VCTime` (microseconds).
6. **One version for the repository**, in the root `CMakeLists.txt`.
7. **FFmpeg has one caller at a time.** Every call into it sits between `FFMPEG_ENTER()`
   and `ffmpeg_leave()` (`ffmpeg/amiga_library.h`); a variable set after `FFMPEG_ENTER()`
   and read after `ffmpeg_leave()` is `volatile`.
8. **Nothing is printed from a library.** FFmpeg's log callback drops every line.
9. **A caller of the libraries is a Process**: they read through dos.library in the
   caller's context.
10. **Functions carry kernel-doc comments**; register arguments are written `asm("a0")`.
    Code style as in the driver stack (tabs, braces on their own lines, full names,
    declarations at first use, every structure field commented).

## Invariants

### The libraries that carry FFmpeg

- libnix's library start-up (`-ramiga-lib`) gives the resident tag and the standard
  vectors only. `ffmpeg/amiga_library.c` defines `SysBase` and the heads of libnix's
  lists, opens the libraries on libnix's list, runs its init functions by priority at
  init and its exit functions at expunge. Its object goes first in the link.
- The heads of libnix's lists are made in assembly and declared without a size. As C arrays
  of one zero word the compiler took the lists for empty, no init function ran, and the
  first `malloc()` hung. The exit list has its own end word (`.list___EXIT_LIST__end`): the
  linker sorts the function table between the exit and the init list.
- An entry of libnix's library list is the base variable itself (-1 until opened) and the
  name. Writing through it as a pointer crashed the machine once.
- `exit()` and `abort()` land in the `setjmp()` of `FFMPEG_ENTER()` and fail the call.
- The function table is one array in the `.list___FuncTable__` section, in the SFD's
  order, ended by -1.

### audiocodec.library and ffmpeg.codec

- The frontend is freestanding: no C library, no globals, no writable section; the bases it
  calls come from its library base. It parses tags and forwards; decoding, converting and
  mixing down are the backend's.
- A backend never sees a tag and fills no `ACCodecInfo.Backend`.
- `ffmpeg.codec` copies every packet into FFmpeg's own padded memory before decoding.
- Samples leave the backend interleaved, signed, in the WAVE channel order.
- A mix down is never scaled down: libswresample writes floats with `rematrix_maxval` at
  its largest, and the AC-3 decoders get `normalize_downmix` 0 (patch 0002). The backend
  bends mixed samples that pass full scale (`sample_limit()`) and only cuts the others.
  libswresample mixes only what the decoder leaves (user).

### Kinora (player/)

- Three tasks: the front (window), the engine, the audio output. The front only sends the
  engine commands (`engine.h`); nothing of the engine's is touched from outside its task.
- A process's own port (`pr_MsgPort`) is dos.library's. The engine and the audio task take
  only their start message there, the rest on a port of their own: a command arriving while
  the engine read the file, and an end message while AHI read its prefs in the audio task,
  ended in "Unexpected packet received".
- The reader task (`engine_reader.c`) makes every read and every release of the open
  file's packets. Any other `DMX_` call from the engine's task is made with the reader held
  (`reader_hold()` .. `reader_let_go()`), and a packet the engine is done with goes back
  through `reader_take()`: `demuxer.library`'s one-caller semaphore is held while a read
  waits, so the engine's own release would wait as long.
- The engine never waits inside a pass: it sleeps on its commands, the video decoder's
  signal, the audio output's "room again", the reader's "packets read" and its timer.
- The engine draws into the front's window: the front takes the box away
  (`engine_place(engine, NULL)`) before the window closes.
- Going from a screen's imported parts back to the decoder's own frames needs
  `VCA_OutputCount` with the geometry change (videocodec.library 0.6): without it the
  decoder waits for imports for good.
- Another track, another sound unit, or another place for a film that stands still: the
  engine starts again from where the film is (`reseek()`), since the decoders cannot take
  such a change in passing.
- FreeType's memory comes from exec.library (`subtitles_font.c`), not from libnix's
  `malloc()`: the engine's task and the front's cannot share that one. The font file is read
  in the engine's task, and the front names it by its whole path: the engine's process has
  no `PROGDIR:`.
- A change of the picture's place or shape for a film that stands still: flush first
  (`reseek()`), then tell the decoder its new frames. The other order ended in a decoder
  error.
- The window's `VC_SetAttrsTags()` always names its destination rectangle: the session
  keeps a screen's rectangle otherwise, and refuses the smaller frame.
- The audio task runs at priority 10, above what AmiKit's Executive re-prioritises. The
  engine runs at 1, below `ahi.device`'s unit process (2): held up by the engine's work,
  AHI stops playing linked writes for good. Do not raise it. The reader task runs one below
  the engine, the flipper one above it: below it, VideoCore.card's poll for the blank's first line misses and flips
  take refreshes more. Under Executive the engine and the flipper keep their priorities
  only with `TASK Kinora* NOSCHEDULE` in `S:Executive.prefs` (README); without it full
  screen drops twice the pictures.

### demuxer.library

- FFmpeg reads the file only through the reader (`demuxer_reader.c`), which alone decides
  how a position is reached: the 64-bit packets, `Seek()`, steps forward, reading through.
- H.264 and H.265 packets and their configuration leave the library as Annex-B byte
  streams, whatever the container holds.
- A packet's memory is FFmpeg's until `DMX_ReleasePacket()`.
- A container added to `--enable-demuxer` in `scripts/build-ffmpeg.sh` also needs its
  extensions in `FILM_PATTERN` (`player/gui/gui.c`) and in the README's table: FFmpeg's own
  extension lists cannot stand in (MPEG-TS has none, MP4's names still pictures).

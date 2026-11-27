# SPDX-License-Identifier: MPL-2.0
"""The profile-guided build's training round (docs/architecture.md, section 3), run by scripts/pgo-train.py.

The suite's device fixture puts the build in its places: it must be a profile-generating one (KINORA_PGO=GENERATE),
whose three FFmpeg binaries are instrumented. The rigs then read and decode the training material
(tests/material/training.sh), never the tests' own, one binary at a time, all through FilmInfo: each film's video
decoded (ffmpegvideo.codec), every packet of every film read (demuxer.library), each film's sound and each sound
clip decoded, mixed down to stereo as well (audiocodec's ffmpegaudio.codec). The library stays in memory meanwhile, gathering
its counts, and PgoDump has it write its profile at the end of its part, which comes back to build/pgo-streams/
for the merge: a part that fails costs that part only, `-k` runs it again.
"""
from pathlib import Path

import pytest

TESTS = Path(__file__).resolve().parents[1]
ROOT = TESTS.parent
MATERIAL = ROOT / "build" / "training-material"
STREAMS = ROOT / "build" / "pgo-streams"
SOFTWARE_VIDEO = ("xvid.mkv", "xvid_qpel.mkv", "xvid_hd.mkv", "mpeg4.mp4", "divx3.mkv", "msmpeg4v2.mkv",
                  "mpeg2_576i.ts", "mpeg2_1080i.ts", "mpeg1.mkv", "h263.mkv", "flv1.mkv", "theora.mkv",
                  "xvid.avi", "divx3.avi", "divx5_packed.avi", "mpeg1.mpg", "dvd.vob")
FILMS = SOFTWARE_VIDEO + ("h264.mkv", "h265.mp4", "h264.ts")
WITH_SOUND = ("xvid.mkv", "mpeg4.mp4", "mpeg2_576i.ts", "mpeg2_1080i.ts", "theora.mkv", "h264.mkv", "h265.mp4", "h264.ts",
              "xvid.avi", "divx3.avi", "mpeg1.mpg", "dvd.vob")
SOUND = tuple(clip + ".mkv" for clip in ("aac_st", "aac_51", "ac3_51", "eac3_51", "dts_51", "mp3_st", "mp2_st",
                                         "opus_51", "vorbis_st", "flac_51", "truehd_51", "pcm_st")) + \
    ("ima_st.avi", "msadpcm_st.avi", "alaw_st.avi", "mulaw_st.avi", "lpcm_51.vob")
TIMEOUT_S = 3 * 60 * 60  # a part is one test: every file, its rig


def run_ok(device, command: str, args: str, timeout: float = 900) -> str:
    """Run a rig; its output, the run failed when it did."""
    rc, text = device.run_text(command, args, timeout)
    assert rc == 0, f"{command} {args}:\n{text}"
    return text


def each_file(device, names: tuple[str, ...], rig) -> None:
    """Every file of @names uploaded, read by @rig(remote), deleted again."""
    missing = [name for name in names if not (MATERIAL / name).exists()]
    if missing:
        pytest.fail("no training material: run tests/material/training.sh (missing " + ", ".join(missing) + ")")
    for name in names:
        remote = f"{device.ram}/{name}"
        device.upload(MATERIAL / name, remote)
        try:
            rig(remote)
        finally:
            device.delete(remote)


def dump(device, library: str, functions: int, configuration: str) -> None:
    """Have @library write its profile (PgoDump: its private call after @functions public ones), and bring it back."""
    dumper = ROOT / "build" / "tools" / "PgoDump"
    if not dumper.exists():
        pytest.fail("no PgoDump in the build: build with KINORA_CONFIGURE_ARGS=-DKINORA_PGO=GENERATE first")
    device.upload(dumper, f"{device.ram}/PgoDump")
    remote = f"{device.ram}/{configuration}.pgo"
    text = run_ok(device, f"{device.ram}/PgoDump", f"{library} {functions} {remote}", 120)
    assert "dump=" in text and int(text.split("dump=")[1].split()[0]) > 0, text
    STREAMS.mkdir(parents=True, exist_ok=True)
    (STREAMS / f"{configuration}.pgo").write_bytes(device.download(remote))
    device.delete(remote)


@pytest.mark.timeout(TIMEOUT_S)
def test_video(device):
    """ffmpegvideo.codec: every film's video decoded, then its profile."""
    each_file(device, SOFTWARE_VIDEO, lambda remote: run_ok(device, "C:FilmInfo", f"{remote} VIDEO"))
    dump(device, "ffmpegvideo.codec", 16, "video")


@pytest.mark.timeout(TIMEOUT_S)
def test_demuxer(device):
    """demuxer.library: every packet of every film, then its profile."""
    each_file(device, FILMS, lambda remote: run_ok(device, "C:FilmInfo", f"{remote} ALL"))
    dump(device, "demuxer.library", 9, "demuxer")


def sound_rig(device, remote: str) -> None:
    """FilmInfo AUDIO over a file's first sound stream, as it is and mixed down to stereo."""
    run_ok(device, "C:FilmInfo", f"{remote} AUDIO")
    run_ok(device, "C:FilmInfo", f"{remote} AUDIO CHANNELS 2")


@pytest.mark.timeout(TIMEOUT_S)
def test_audio(device):
    """audiocodec's ffmpegaudio.codec: every film's sound and every clip decoded, then its profile."""
    each_file(device, WITH_SOUND + SOUND, lambda remote: sound_rig(device, remote))
    dump(device, "ffmpegaudio.codec", 7, "audio")

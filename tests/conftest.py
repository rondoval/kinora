# SPDX-License-Identifier: MPL-2.0
"""
conftest.py - Kinora's suite: emu68-common's harness (emu68test, from the build's prefix) and the
suite's own helpers (kinoratest/).

The harness was written for the driver stack, whose suites sit in <stack>/components/<name>/tests;
run.sh names Kinora's own build, install and amimcp directories in its place.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

TESTS = Path(__file__).resolve().parent
ROOT = TESTS.parent
sys.path.insert(0, str(TESTS))

pytest.register_assert_rewrite("emu68test", "kinoratest")

from emu68test import plugin  # noqa: E402
from emu68test.install import Installer  # noqa: E402
from emu68test.results import HarnessError  # noqa: E402
from kinoratest.material import Material  # noqa: E402
from kinoratest.player import Player  # noqa: E402

plugin.configure(plugin.Suite(name="Kinora", component="kinora", tests_dir=TESTS, ram="RAM:kn"))
pytest_plugins = ["emu68test.plugin"]

KINORA = "SYS:Utilities/Kinora/Kinora"
# The build's files and their places on the Amiga (build path -> remote path)
INSTALLED = (
    ("demuxer/demuxer.library", "LIBS:demuxer.library"),
    ("audiocodec/audiocodec.library", "LIBS:audiocodec.library"),
    ("audiocodec/ffmpegaudio.codec", "LIBS:audiocodec/ffmpegaudio.codec"),
    ("videocodec-ffmpeg/ffmpegvideo.codec", "LIBS:videocodec/ffmpegvideo.codec"),
    ("tools/FilmInfo", "C:FilmInfo"),
    ("player/Kinora", KINORA),
    # the video libraries Kinora ships, from their prefix in the build dir
    ("video-prefix/LIBS/videocodec.library", "LIBS:videocodec.library"),
    ("video-prefix/LIBS/videocodec/videocore.codec", "LIBS:videocodec/videocore.codec"),
    ("video-prefix/LIBS/videocodec/rpivid.codec", "LIBS:videocodec/rpivid.codec"),
    ("video-prefix/C/VCInfo", "C:VCInfo"),
)
ICON = ("dist/Kinora.info", KINORA + ".info")
FONT = "dist/fonts/DejaVuSans.ttf"          # the font subtitles are drawn with, in the drawer Fonts beside the program
SOURCE_FILES = (ICON, (FONT, "SYS:Utilities/Kinora/Fonts/DejaVuSans.ttf"),
                ("dist/fonts/DejaVuSans.license.txt", "SYS:Utilities/Kinora/Fonts/DejaVuSans.license.txt"))


def build_files(config) -> dict[str, bytes]:
    """What the suite puts on the Amiga: remote path -> content."""
    build = Path(config.getoption("--build-dir"))
    files = {remote: (build / local).read_bytes() for local, remote in INSTALLED}
    for local, remote in SOURCE_FILES:
        files[remote] = (ROOT / local).read_bytes()
    return files


@pytest.fixture(scope="session")
def device(request, rundir, capture):
    """The Amiga: connected, the build in its places (checked with --no-install), no Kinora running, the
    libraries of an earlier build flushed out of memory."""
    config = request.config
    run = plugin.session(config)
    dev = plugin.connect(request, capture)
    dev.break_all("Kinora")
    dev.exec("MakeDir >NIL: LIBS:audiocodec", timeout=15)
    dev.exec("Delete >NIL: LIBS:audiocodec/ffmpeg.codec QUIET", timeout=15)  # the backend's name until 0.2, as the Installer does
    for tool in ("DMXInfo", "ACInfo", "VCDecode"):                               # the tools FilmInfo replaced, as the Installer does
        dev.exec(f"Delete >NIL: C:{tool} QUIET", timeout=15)
    dev.exec("MakeDir >NIL: SYS:Utilities/Kinora", timeout=15)
    dev.exec("MakeDir >NIL: SYS:Utilities/Kinora/Fonts", timeout=15)
    files = build_files(config)
    installer = Installer(dev, TESTS / ".machine" / config.getoption("--server"))
    if config.getoption("--no-install"):
        differ = installer.differing(files)
        if differ:
            pytest.exit("installed files differ from the build (run without --no-install): " + ", ".join(differ),
                        returncode=plugin.EXIT_ENVIRONMENT)
        run.report.header.append("Installed files checked against the build: " + ", ".join(files))
    else:
        try:
            placed = installer.install(files, reboot=False)     # nothing of it is resident: a flush loads the new libraries
        except HarnessError as e:
            pytest.exit(f"the install failed: {e}", returncode=plugin.EXIT_ENVIRONMENT)
        changed = [file.remote for file in placed if file.changed]
        run.report.header.append("Replaced on the Amiga: " + (", ".join(changed) if changed else "nothing, the build was there"))
    dev.flush()
    yield dev
    dev.break_all("Kinora")
    dev.exec(f"Delete {dev.ram} ALL QUIET", timeout=60)


@pytest.fixture(scope="session")
def material(device) -> Material:
    """The test material: made on the host when missing, uploaded to the suite's RAM: drawer when first used."""
    return Material(ROOT, device)


@pytest.fixture
def player(device):
    """Kinora, not yet started: a test starts it with its films. It is ended after the test, whatever happened."""
    kinora = Player(device, KINORA)
    yield kinora
    kinora.quit()

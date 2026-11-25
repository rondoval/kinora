# SPDX-License-Identifier: MPL-2.0
"""
material.py - the suite's films and references: made on the host by tests/material/make.sh into
build/test-material/, uploaded to the suite's RAM: drawer when a test first asks for one.
"""
from __future__ import annotations

import subprocess
from pathlib import Path

CONTAINERS = ("a.mkv", "b.mp4", "c.ts")     # H.264+AAC+SubRip in Matroska, H.265+AC-3 in MP4, H.264+E-AC-3 in a transport stream
AUDIO = ("aac_st", "aac_51", "ac3_51", "eac3_51", "dts_51", "mp3_st", "opus_51", "flac_st", "vorbis_st")
FILM = "film.mkv"           # 720p25, 40 s, two audio tracks, a SubRip track, three chapters
PLAIN = "plain.mkv"         # the same picture, one audio track; plain.srt lies beside it
SHORT = "short.mkv"         # six seconds, for the playlist
WORLD = "world.srt"         # one cue from 1 to 39 s, in three alphabets
STYLES = "styles.srt"       # cues of eight seconds from 1, 9, 17 and 25 s: slanted, plain, heavy, at the top
WORLD_TEXT = "Za\u017c\u00f3\u0142\u0107 g\u0119\u015bl\u0105 \u041f\u0440\u0438\u0432\u0435\u0442 \u03a9\u03bc\u03ad\u03b3\u03b1"
FILM_MS = 40000
SHORT_MS = 6000
FRAME_RATE = 25


class Material:
    """The material of one run."""

    def __init__(self, root: Path, device=None):
        self.dir = root / "build" / "test-material"
        self.maker = root / "tests" / "material" / "make.sh"
        self.device = device

    def path(self, name: str) -> Path:
        """The local file @name, the material made first when it is not there."""
        if not (self.dir / ".complete").exists():
            subprocess.run([str(self.maker)], check=True)
        return self.dir / name

    def ram(self, name: str) -> str:
        """@name in the suite's RAM: drawer, uploaded when it is not there yet."""
        remote = f"{self.device.ram}/{name}"
        self.device.upload_once(self.path(name), remote)
        return remote

    def packet_counts(self, name: str) -> list[int]:
        """ffprobe's packets per stream of a container film, in stream order."""
        counts = [int(line.split(",")[1]) for line in self.path(name + ".ffprobe").read_text().splitlines() if "," in line]
        half = len(counts) // 2
        if len(counts) % 2 == 0 and counts[:half] == counts[half:]:
            counts = counts[:half]      # ffprobe lists a transport stream's streams twice: by programme, then all
        return counts

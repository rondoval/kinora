# SPDX-License-Identifier: MPL-2.0
"""
tools.py - FilmInfo run on the Amiga, its output read: the container, a stream decoded, the codec tables.

It prints lines of name=value pairs; a line may begin with words and a number ("stream 0:",
"packets 1 =", "audio codec 3:"). The readers here turn them into dictionaries and lists.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field

PAIR = re.compile(r"(\w+)=(\S*)")


def pairs(line: str) -> dict[str, str]:
    """The name=value pairs of one line."""
    return dict(PAIR.findall(line))


@dataclass
class FileInfo:
    """What FilmInfo printed about a file's container."""

    rc: int
    text: str
    head: dict[str, str] = field(default_factory=dict)              # format=, streams=, chapters=, duration_ms=, seekable=
    streams: list[dict[str, str]] = field(default_factory=list)     # per stream: kind, codec, ...
    chapters: list[dict[str, str]] = field(default_factory=list)
    packets: list[dict[str, str]] = field(default_factory=list)     # the packets listed (PACKETS n)
    counts: dict[int, int] = field(default_factory=dict)            # packets per stream (ALL)
    result: dict[str, str] = field(default_factory=dict)            # seek=, read=


def film_info(device, args: str, timeout: float = 120) -> FileInfo:
    """FilmInfo @args (a file, and PACKETS, ALL or SEEK), read."""
    rc, text = device.run_text("C:FilmInfo", args, timeout)
    info = FileInfo(rc, text)
    for line in text.splitlines():
        if line.startswith("format="):
            info.head = pairs(line)
        elif line.startswith("stream "):
            info.streams.append({"kind": line.split()[2], **pairs(line)})
        elif line.startswith("chapter "):
            info.chapters.append({**pairs(line), "title": line.split("title=", 1)[-1]})
        elif line.startswith("packet "):
            info.packets.append(pairs(line))
        elif line.startswith("packets "):
            stream, count = re.match(r"packets (\d+) = (\d+)", line).groups()
            info.counts[int(stream)] = int(count)
        elif line.startswith(("seek=", "read=")):
            info.result.update(pairs(line))
    return info


@dataclass
class Decode:
    """What FilmInfo AUDIO printed about a decode."""

    rc: int
    text: str
    facts: dict[str, str] = field(default_factory=dict)             # stream, codec, open, packets, bad, frames, samples, rate, channels, layout, format, decode

    def num(self, name: str) -> int:
        """The fact @name as a number; layout is printed in hexadecimal."""
        return int(self.facts[name], 16 if name == "layout" else 10)


def audio_decode(device, args: str, timeout: float = 300) -> Decode:
    """FilmInfo @args AUDIO, read."""
    rc, text = device.run_text("C:FilmInfo", f"{args} AUDIO", timeout)
    decode = Decode(rc, text)
    for line in text.splitlines():
        decode.facts.update(pairs(line))
    return decode


def codec_rows(device, kind: str) -> list[dict[str, str]]:
    """FilmInfo without a file: the rows of one codec table, @kind "audio" or "video"."""
    _, text = device.run_text("C:FilmInfo", "", 60)
    return [{"codec": re.match(rf"{kind} codec (\d+):", line).group(1), **pairs(line)} for line in text.splitlines()
            if line.startswith(f"{kind} codec ")]


def audio_codecs(device) -> list[dict[str, str]]:
    """audiocodec.library's codec table."""
    return codec_rows(device, "audio")


@dataclass
class VideoDecode:
    """What FilmInfo VIDEO printed about a decode."""

    rc: int
    text: str
    facts: dict[str, str] = field(default_factory=dict)              # stream, codec, tag, open, backend, packets, pictures, us, decode, ...
    infos: list[dict[str, str]] = field(default_factory=list)        # each stream info: width, height, crop, format, colorspace, rate, par
    pictures: list[tuple[int, str]] = field(default_factory=list)    # MD5: (pts_ms, md5) a picture, in order
    moved_at: int = -1                                               # SEEK: how many pictures came before the move

    def num(self, name: str) -> int:
        """The fact @name as a number."""
        return int(self.facts[name])


def video_decode(device, args: str, timeout: float = 300) -> VideoDecode:
    """FilmInfo @args VIDEO, read."""
    rc, text = device.run_text("C:FilmInfo", f"{args} VIDEO", timeout)
    decode = VideoDecode(rc, text)
    for line in text.splitlines():
        if line.startswith("info "):
            decode.infos.append(pairs(line))
        elif line.startswith("picture "):
            facts = pairs(line)
            decode.pictures.append((int(facts["pts_ms"]), facts["md5"]))
        elif line.startswith("moved="):
            decode.facts.update(pairs(line))
            decode.moved_at = len(decode.pictures)
        else:
            decode.facts.update(pairs(line))
    return decode


def video_decoders(device) -> list[dict[str, str]]:
    """Every video codec driver's decoder rows."""
    return codec_rows(device, "video")

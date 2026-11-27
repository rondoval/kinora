# SPDX-License-Identifier: MPL-2.0
"""
tools.py - DMXInfo and ACInfo run on the Amiga, their output read.

Both print lines of name=value pairs; a line may begin with a word and a number ("stream 0:",
"packets 1 ="). The readers here turn them into dictionaries and lists.
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
    """What DMXInfo printed about a file."""

    rc: int
    text: str
    head: dict[str, str] = field(default_factory=dict)              # format=, streams=, chapters=, duration_ms=, seekable=
    streams: list[dict[str, str]] = field(default_factory=list)     # per stream: kind, codec, ...
    chapters: list[dict[str, str]] = field(default_factory=list)
    packets: list[dict[str, str]] = field(default_factory=list)     # the packets listed (PACKETS n)
    counts: dict[int, int] = field(default_factory=dict)            # packets per stream (ALL)
    result: dict[str, str] = field(default_factory=dict)            # seek=, read=


def dmxinfo(device, args: str, timeout: float = 120) -> FileInfo:
    """DMXInfo @args, read."""
    rc, text = device.run_text("C:DMXInfo", args, timeout)
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
    """What ACInfo printed about a decode."""

    rc: int
    text: str
    facts: dict[str, str] = field(default_factory=dict)             # stream, codec, open, packets, bad, frames, samples, rate, channels, layout, format, decode

    def num(self, name: str) -> int:
        """The fact @name as a number; layout is printed in hexadecimal."""
        return int(self.facts[name], 16 if name == "layout" else 10)


def acinfo(device, args: str, timeout: float = 300) -> Decode:
    """ACInfo @args, read."""
    rc, text = device.run_text("C:ACInfo", args, timeout)
    decode = Decode(rc, text)
    for line in text.splitlines():
        decode.facts.update(pairs(line))
    return decode


def codec_table(device) -> list[dict[str, str]]:
    """ACInfo without a file: the rows of the codec table."""
    _, text = device.run_text("C:ACInfo", "", 60)
    return [{"codec": re.match(r"codec (\d+):", line).group(1), **pairs(line)} for line in text.splitlines()
            if line.startswith("codec ")]


@dataclass
class VideoDecode:
    """What VCDecode printed about a decode."""

    rc: int
    text: str
    facts: dict[str, str] = field(default_factory=dict)              # stream, codec, tag, open, backend, packets, pictures, us, decode, ...
    infos: list[dict[str, str]] = field(default_factory=list)        # each stream info: width, height, crop, format, colorspace, rate, par
    pictures: list[tuple[int, str]] = field(default_factory=list)    # MD5: (pts_ms, md5) a picture, in order
    moved_at: int = -1                                               # SEEK: how many pictures came before the move

    def num(self, name: str) -> int:
        """The fact @name as a number."""
        return int(self.facts[name])


def vcdecode(device, args: str, timeout: float = 300) -> VideoDecode:
    """VCDecode @args, read."""
    rc, text = device.run_text("C:VCDecode", args, timeout)
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


def decoder_rows(device) -> list[dict[str, str]]:
    """VCDecode without a file: every backend's decoder rows."""
    _, text = device.run_text("C:VCDecode", "", 60)
    return [{"codec": re.match(r"codec (\d+):", line).group(1), **pairs(line)} for line in text.splitlines()
            if line.startswith("codec ")]

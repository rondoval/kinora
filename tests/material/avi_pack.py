#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""avi_pack.py in.avi out.avi: an AVI's MPEG-4 Part 2 video with its B-pictures packed as DivX 5 packs them.

FFmpeg writes B-pictures in AVI in decode order, a picture a chunk; DivX's packed bitstream puts each
reference picture and the B-picture after it into one chunk and a placeholder (a picture not coded) into
the next, so that every chunk is shown at its own frame's time. demuxer.library unpacks them again
(mpeg4_unpack_bframes) and puts the pair's times in order (demuxer_order.c); this makes such a file to
test that on. The input has one stream, video, at most one B-picture in a row (ffmpeg -bf 1); the
output is a plain AVI 1.0 with its idx1, the input's headers kept but for its OpenDML index.
"""
import struct
import sys

VOP = b"\x00\x00\x01\xb6"
B_VOP = 2
# a P-VOP not coded: type 01, no seconds, a marker, a 5-bit time increment (ffmpeg's at 25 a second), a marker,
# vop_coded 0, then the stuffing to the byte
PLACEHOLDER = VOP + bytes((0b01010000, 0b01001111))
KEY_FRAME = 0x10                        # AVIIF_KEYFRAME


def chunks(data: bytes, start: int, end: int):
    """The chunks between @start and @end: (fourcc, offset of the chunk, payload)."""
    while start + 8 <= end:
        fourcc, size = data[start:start + 4], struct.unpack_from("<I", data, start + 4)[0]
        yield fourcc, start, data[start + 8:start + 8 + size]
        start += 8 + size + (size & 1)


def lists(data: bytes, start: int, end: int):
    """The LISTs between @start and @end: their type and payload's range."""
    for fourcc, offset, payload in chunks(data, start, end):
        if fourcc == b"LIST":
            yield payload[:4], offset + 12, offset + 8 + len(payload)


def vop_type(frame: bytes) -> int:
    """The coding type of the frame's first VOP, -1 for none."""
    at = frame.find(VOP)
    return frame[at + 4] >> 6 if at >= 0 and at + 4 < len(frame) else -1


def read(path: str):
    """The headers (avih, strh, strf) and the video frames in their order."""
    data = open(path, "rb").read()
    assert data[:4] == b"RIFF" and data[8:12] == b"AVI "
    headers, frames = {}, []
    for kind, start, end in lists(data, 12, len(data)):
        if kind == b"hdrl":
            for fourcc, _, payload in chunks(data, start, end):
                if fourcc == b"avih":
                    headers["avih"] = payload
            for skind, sstart, send in lists(data, start, end):
                if skind == b"strl":
                    for fourcc, _, payload in chunks(data, sstart, send):
                        if fourcc in (b"strh", b"strf"):
                            headers[fourcc.decode()] = payload
        elif kind == b"movi":
            frames += [payload for fourcc, _, payload in chunks(data, start, end) if fourcc == b"00dc"]
    return headers, frames


def pack(frames: list[bytes]) -> list[bytes]:
    """Each reference with the B-picture after it in one chunk, a placeholder in the next."""
    packed, i = [], 0
    while i < len(frames):
        if i + 1 < len(frames) and vop_type(frames[i + 1]) == B_VOP and vop_type(frames[i]) != B_VOP:
            packed += [frames[i] + frames[i + 1], PLACEHOLDER]
            i += 2
        else:
            packed.append(frames[i])
            i += 1
    return packed


def chunk(fourcc: bytes, payload: bytes) -> bytes:
    """A RIFF chunk, padded to an even length."""
    return fourcc + struct.pack("<I", len(payload)) + payload + (b"\x00" if len(payload) & 1 else b"")


def write(path: str, headers: dict, frames: list[bytes]) -> None:
    """A plain AVI of one video stream: hdrl, movi, idx1."""
    strl = chunk(b"LIST", b"strl" + chunk(b"strh", headers["strh"]) + chunk(b"strf", headers["strf"]))
    hdrl = chunk(b"LIST", b"hdrl" + chunk(b"avih", headers["avih"]) + strl)
    movi, index, offset = b"movi", b"", 4
    for frame in frames:
        flags = KEY_FRAME if vop_type(frame) == 0 else 0
        index += b"00dc" + struct.pack("<III", flags, offset, len(frame))
        piece = chunk(b"00dc", frame)
        movi += piece
        offset += len(piece)
    body = b"AVI " + hdrl + chunk(b"LIST", movi) + chunk(b"idx1", index)
    open(path, "wb").write(b"RIFF" + struct.pack("<I", len(body)) + body)


def main() -> None:
    headers, frames = read(sys.argv[1])
    packed = pack(frames)
    assert len(packed) == len(frames), "every frame keeps its chunk"
    write(sys.argv[2], headers, packed)


if __name__ == "__main__":
    main()

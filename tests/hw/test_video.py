# SPDX-License-Identifier: MPL-2.0
"""ffmpegvideo.codec through videocodec.library and FilmInfo: its rows, each codec's pictures bit for bit as the host's
decode of the same FFmpeg source gives them, the choice of backend, skipping, a move, the chain to RGB, and the share of
the CPU each codec takes."""
import pytest

from kinoratest import material as films
from kinoratest.tools import video_decode, video_decoders

BACKEND = "ffmpegvideo.codec"
VCC_H264, VCC_MPEG1, VCC_MPEG2, VCC_MPEG4, VCC_MSMPEG4, VCC_H263, VCC_THEORA = 1, 11, 12, 13, 14, 15, 20
SOFTWARE_ROWS = {VCC_MPEG1, VCC_MPEG2, VCC_MPEG4, VCC_MSMPEG4, VCC_H263, VCC_THEORA}
VCF_I420, VCF_BGRA32 = 1, 7
VCERR_NOBACKEND = -3
PRIORITY_SOFTWARE = 0
FRAME_MS = 40               # the clips' pictures: 25 a second
GOP_MPEG2 = 12              # mpeg2.ts: a key frame every 12 pictures, two B-pictures between the others
KEY_GAP_MS = 2000           # the clips' key frames are at most this far apart


def host_digests(material, clip: str) -> list[str]:
    """The host's decode of @clip: an MD5 a picture."""
    return material.path(clip + ".host.md5").read_text().split()


@pytest.mark.smoke
def test_rows_are_listed(device):
    """The backend has a session row of software's priority for each codec it decodes, writing I420, with the
    VideoCore's converter behind it for the screen's formats; H.264 stays the VideoCore's."""
    rows = video_decoders(device)
    ours = {int(row["codec"]): row for row in rows if row["backend"] == BACKEND}
    assert set(ours) == SOFTWARE_ROWS
    for row in ours.values():
        assert int(row["priority"]) == PRIORITY_SOFTWARE
        assert int(row["formats"], 16) == 1 << VCF_I420
        assert int(row["chain"], 16) & (1 << VCF_BGRA32)
    assert all(row["backend"] != BACKEND for row in rows if int(row["codec"]) == VCC_H264)


@pytest.mark.parametrize("clip", films.VIDEO)
def test_decodes_as_the_host_does(device, material, clip):
    """Six seconds of each codec give the pictures the same decoder gives on the host, bit for bit, in order."""
    decode = video_decode(device, f"{material.ram(clip)} MD5")
    assert decode.rc == 0 and decode.num("decode") == 0, decode.text
    assert decode.facts["backend"] == BACKEND and decode.facts["converter"] == "none"
    assert [md5 for _, md5 in decode.pictures] == host_digests(material, clip)
    info = decode.infos[0]
    assert int(info["format"]) == VCF_I420 and int(info["width"]) % 64 == 0 and int(info["height"]) % 16 == 0


def test_h264_stays_on_the_videocore(device, material):
    """An H.264 film goes to the VideoCore, the hardware row of higher priority."""
    decode = video_decode(device, f"{material.ram('a.mkv')} FORMAT BGRA32")
    assert decode.rc == 0 and decode.facts["backend"] == "videocore.codec", decode.text


def test_vp8_is_refused(device, material):
    """No backend decodes VP8: the session is refused, and says so."""
    decode = video_decode(device, material.ram("vp8.webm"))
    assert decode.rc != 0 and decode.num("open") == VCERR_NOBACKEND, decode.text


@pytest.mark.parametrize("skip, most", [(1, 0.45), (2, 1.0 / GOP_MPEG2 + 0.05)])
def test_skips_when_asked(device, material, skip, most):
    """VCA_Skip 1 leaves the B-pictures out (two of every three), 2 all but the key frames."""
    clip = material.ram("mpeg2.ts")
    whole = video_decode(device, clip).num("pictures")
    decode = video_decode(device, f"{clip} SKIP {skip}")
    assert decode.num("decode") == 0, decode.text
    assert 0 < decode.num("pictures") <= whole * most


@pytest.mark.parametrize("clip", ["mpeg2.ts", "mpeg4.mkv"])
def test_a_move_goes_on_from_its_key_frame(device, material, clip):
    """A flush and a move to three seconds, after two seconds decoded: the pictures after it come in their order, from
    a key frame near the time asked for, to the end, each as the same decode without a move gives it; none from
    before the move comes after it."""
    whole = set(video_decode(device, f"{material.ram(clip)} MD5").pictures)
    decode = video_decode(device, f"{material.ram(clip)} MD5 SEEK 3000")
    assert decode.num("decode") == 0 and decode.num("moved") == 0, decode.text
    after = decode.pictures[decode.moved_at:]
    times = [pts for pts, _ in after]
    assert after and times == sorted(times), "the pictures after the move in their order"
    assert 3000 - KEY_GAP_MS <= times[0] <= 3000 + KEY_GAP_MS, "from a key frame near the time asked for"
    assert set(after) <= whole, "each as the decode without a move gives it"
    assert max(times) == max(pts for pts, _ in whole), "on to the end"


def test_the_converter_behind_makes_rgb(device, material):
    """Asked for BGRA32 at a window's size, the session puts the VideoCore's converter behind the decoder: every
    picture comes, at that size."""
    decode = video_decode(device, f"{material.ram('xvid.mkv')} FORMAT BGRA32 WIDTH 736 HEIGHT 400")
    assert decode.rc == 0 and decode.num("decode") == 0, decode.text
    assert decode.facts["backend"] == BACKEND and decode.facts["converter"] == "videocore.codec"
    assert decode.num("pictures") == len(host_digests(material, "xvid.mkv"))
    info = decode.infos[0]
    assert int(info["format"]) == VCF_BGRA32 and (int(info["width"]), int(info["height"])) == (736, 400)


@pytest.mark.parametrize("clip", films.VIDEO)
def test_share_of_the_cpu(device, material, clip, record_property):
    """Each codec's decode time against its pictures' time, at priority 6 (out of Executive's reach): recorded, and
    each clip of standard definition within real time."""
    decode = video_decode(device, f"{material.ram(clip)} PRI 6")
    assert decode.num("decode") == 0, decode.text
    film_us = (decode.num("film_ms") + FRAME_MS) * 1000
    share = decode.num("us") / film_us
    record_property("cpu_share", round(share, 3))
    assert share < 1.0

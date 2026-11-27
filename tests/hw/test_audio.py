# SPDX-License-Identifier: MPL-2.0
"""audiocodec.library and ffmpegaudio.codec through ACInfo: the codec table, nine decodes against the host's, the mix-down
and the 32-bit output."""
import numpy as np
import pytest

from kinoratest import material as films
from kinoratest.tools import acinfo, codec_table

ACERR_END = -6
ACF_S16, ACF_S32 = 1, 2
STEREO = 0x3
CODECS = range(1, 13)       # ACC_AAC to ACC_PCM
SECONDS = 20
RATE = 48000


@pytest.mark.smoke
def test_codec_table_lists_every_codec(device):
    """The library finds ffmpegaudio.codec in LIBS:audiocodec and lists a row for each codec it has a number for."""
    rows = codec_table(device)
    assert sorted(int(row["codec"]) for row in rows) == list(CODECS)
    assert {row["backend"] for row in rows} == {"ffmpegaudio.codec"}


@pytest.mark.parametrize("stem", films.AUDIO)
def test_decodes_as_the_host_does(device, material, stem):
    """Twenty seconds of each codec decode to the samples the same decoder gives on the host, to within one step."""
    out = f"{device.ram}/{stem}.raw"
    try:
        decode = acinfo(device, f"{material.ram(stem + '.mkv')} TO {out}")
        assert decode.rc == 0 and decode.num("decode") == ACERR_END and decode.num("bad") == 0, decode.text
        assert decode.facts["backend"] == "ffmpegaudio.codec"
        got = np.frombuffer(device.download(out), dtype=">i2").astype(np.int32)
    finally:
        device.delete(out)
    want = np.frombuffer(material.path(stem + ".host.raw").read_bytes(), dtype=">i2").astype(np.int32)
    assert len(got) == len(want), "as many samples as the host's decode"
    assert np.abs(got - want).max() <= 1
    assert decode.num("rate") == RATE and decode.num("format") == ACF_S16


@pytest.mark.parametrize("stem", ["ac3_51", "aac_51", "dts_51", "opus_51"])
def test_mixes_down_to_stereo(device, material, stem):
    """A 5.1 stream asked for two channels comes out as stereo, as many sample frames as there were."""
    decode = acinfo(device, f"{material.ram(stem + '.mkv')} CHANNELS 2")
    assert decode.rc == 0, decode.text
    assert decode.num("channels") == 2 and decode.num("layout") == STEREO
    assert abs(decode.num("samples") - SECONDS * RATE) < RATE // 10


def test_gives_32_bit_samples(device, material):
    """BITS 32: the frames say so, and the file holds four bytes a sample."""
    out = f"{device.ram}/wide.raw"
    try:
        decode = acinfo(device, f"{material.ram('flac_st.mkv')} BITS 32 TO {out}")
        assert decode.num("format") == ACF_S32
        assert device.size_of(out) == decode.num("samples") * decode.num("channels") * 4
    finally:
        device.delete(out)

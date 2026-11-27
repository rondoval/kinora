# SPDX-License-Identifier: MPL-2.0
"""demuxer.library through FilmInfo: every packet of five containers, the streams and chapters described, seeking,
and files that are no films."""
import pytest

from kinoratest import material as films
from kinoratest.tools import film_info

DMXERR_FILE, DMXERR_FORMAT, DMXERR_END = -2, -3, -5
VCC_H264, VCC_H265, VCC_MPEG2, VCC_MPEG4 = 1, 4, 12, 13
ACC_AAC, ACC_AC3, ACC_EAC3, ACC_MP3 = 1, 3, 4, 6
KEY_FRAME = 1
GOP_MS = 2000       # the film has a key frame every two seconds
PS_LATE_MS = 1000   # a move in a programme stream lands as late as its next I-picture after the time asked for


@pytest.mark.smoke
@pytest.mark.parametrize("name", films.CONTAINERS)
def test_every_packet_is_read(device, material, name):
    """Each stream of the file gives as many packets as ffprobe counts, and the reading ends at the file's end."""
    info = film_info(device, f"{material.ram(name)} ALL")
    assert info.rc == 0, info.text
    assert [info.counts[stream] for stream in sorted(info.counts)] == material.packet_counts(name)
    assert int(info.result["read"]) == DMXERR_END


@pytest.mark.parametrize("name, container, video, audio, channels, configured", [
    ("a.mkv", "matroska", VCC_H264, ACC_AAC, 1, True),
    ("b.mp4", "mov", VCC_H265, ACC_AC3, 6, True),
    ("c.ts", "mpegts", VCC_H264, ACC_EAC3, 1, False),
    ("d.avi", "avi", VCC_MPEG4, ACC_MP3, 1, False),
    ("e.vob", "mpeg", VCC_MPEG2, ACC_AC3, 1, False),
])
def test_streams_are_described(device, material, name, container, video, audio, channels, configured):
    """The container's name, and each stream's kind, codec and shape. Matroska and MP4 keep the video's parameter sets
    apart, and they come as the stream's configuration; transport and programme streams have them in the stream only,
    and so has AVI as ffmpeg writes MPEG-4 Part 2 (ffprobe takes them out of the first picture, which the library
    leaves to the decoder)."""
    info = film_info(device, material.ram(name))
    assert container in info.head["format"]
    assert int(info.head["seekable"]) == 1
    kinds = {stream["kind"]: stream for stream in info.streams}
    assert int(kinds["video"]["codec"]) == video and "640x360" in info.text
    assert int(kinds["audio"]["codec"]) == audio
    assert int(kinds["audio"]["rate"]) in (44100, 48000) and int(kinds["audio"]["channels"]) == channels
    assert (int(kinds["video"]["extradata"]) > 0) == configured


def test_tracks_and_chapters_are_described(device, material):
    """The film's two audio tracks with their languages and titles, its subtitle track, its three chapters."""
    info = film_info(device, material.ram(films.FILM))
    assert [stream["kind"] for stream in info.streams] == ["video", "audio", "audio", "subtitle"]
    assert [stream["language"] for stream in info.streams[1:]] == ["eng", "pol", "eng"]
    assert "title=Low" in info.text and "title=High" in info.text
    assert [(int(chapter["start_ms"]), chapter["title"]) for chapter in info.chapters] == \
        [(0, "Opening"), (10000, "Middle"), (25000, "Closing")]
    assert abs(int(info.head["duration_ms"]) - films.FILM_MS) < 200


@pytest.mark.parametrize("seconds", [20, 37, 3])
def test_seek_goes_to_the_key_frame_before(device, material, seconds):
    """After a seek the first video packet is a key frame, at or before the time asked for and within one group of
    pictures of it."""
    info = film_info(device, f"{material.ram(films.FILM)} SEEK {seconds * 1000} PACKETS 12")
    assert int(info.result["seek"]) == 0
    first = next(packet for packet in info.packets if int(packet["stream"]) == 0)
    assert int(first["flags"], 16) & KEY_FRAME
    assert seconds * 1000 - GOP_MS <= int(first["pts_ms"]) <= seconds * 1000


@pytest.mark.parametrize("name", ["d.avi", "e.vob"])
def test_a_move_in_avi_and_a_programme_stream_starts_at_an_i_picture(device, material, name):
    """AVI moves by its index, to the key frame at or before the time asked for; a programme stream by its times,
    between key frames, and goes on from the next I-picture, so up to PS_LATE_MS after it. Either way the first video
    packet after the move is an I-picture, marked as a key frame (a programme stream marks none itself), and the
    pictures after it come in the order they are shown."""
    info = film_info(device, f"{material.ram(name)} SEEK 2500 PACKETS 24")
    assert int(info.result["seek"]) == 0, info.text
    video = [packet for packet in info.packets if int(packet["stream"]) == 0]
    assert video and int(video[0]["flags"], 16) & KEY_FRAME, info.text
    latest = 2500 + (PS_LATE_MS if name.endswith(".vob") else 0)
    assert 2500 - GOP_MS <= int(video[0]["pts_ms"]) <= latest, info.text
    assert all(int(packet["pts_ms"]) > int(video[0]["pts_ms"]) for packet in video[1:]), "nothing shown before it"


def test_a_text_file_is_no_film(device, material):
    """A file that is no container is refused with DMXERR_FORMAT, and the tool fails."""
    info = film_info(device, material.ram("plain.srt"))
    assert info.rc != 0 and f"open failed: {DMXERR_FORMAT}" in info.text


def test_a_missing_file_is_refused(device):
    """A file that is not there is refused with DMXERR_FILE."""
    info = film_info(device, f"{device.ram}/no-such-film.mkv")
    assert info.rc != 0 and f"open failed: {DMXERR_FILE}" in info.text

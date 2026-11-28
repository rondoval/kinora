# SPDX-License-Identifier: MPL-2.0
"""Kinora itself, started on the Amiga and driven through its ARexx port: playing, seeking, tracks, subtitles on the
screen, full screen, the picture's shape, the playlist and the settings."""
import contextlib
import time
from pathlib import Path

import numpy as np
import pytest
from PIL import Image, ImageDraw, ImageFont

from emu68test import screen
from kinoratest import material as films

PREFS = "ENVARC:Kinora.prefs"
WHITE = 250                 # a channel of a subtitle's body is at least this
ONE_LINE = (0.75, 0.09)     # where a cue of one line stands in the window: from this of its height down, this high
BRIGHT = 170                # and of a pixel at its edge, half covered, this
FILM_AUDIO, FILM_AUDIO_2, FILM_SUBTITLES = 1, 2, 3     # the film's streams
TRACK_FILE = -2
FONT = Path(__file__).resolve().parents[2] / "dist" / "fonts" / "DejaVuSans.ttf"       # the font Kinora draws with
OTHER_FONT = Path("/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf")                  # one of the host's, to choose
HOST_FONT_SIZE = 120        # the size the host draws a cue at, to compare shapes
ERR_FILE = "-2"             # the engine's answer for a file it cannot use
SLANT = 0x3600 / 0x10000    # how far a slanted glyph leans for each unit up (subtitles_font.c)
HOG = ("ChangeTaskPri 15\n" + "List >NIL: SYS:Libs ALL\nList >NIL: SYS:Devs ALL\n" * 3)    # a job above the audio task's priority
RC_ERROR = 10               # ARexx's return code for a command the port does not know


def statistics(player) -> dict[str, str]:
    """STATISTICS's lines by the word before their colon."""
    return dict(line.split(": ", 1) for line in player.rexx("STATISTICS").splitlines() if ": " in line)


def advance(player, seconds: float) -> tuple[int, int]:
    """How far the film's time and its count of pictures shown move in @seconds of the host's time."""
    before = player.status()
    time.sleep(seconds)
    after = player.status()
    return after.position_ms - before.position_ms, after.shown - before.shown


def subtitle_box(device, name: str = "subtitles", top: float = 0.62, height: float = 0.17) -> np.ndarray:
    """The lower part of Kinora's picture box, where subtitles go, from @top of the window's height down, @height of
    it high; kept in the run's drawer as @name."""
    screen.wake(device)
    x, y, w, h = screen.window(device, "Kinora")
    return screen.screenshot(device, (x + 20, y + int(h * top), w - 40, int(h * height)), name=name)


def white_pixels(device) -> int:
    """White pixels in the lower part of Kinora's picture box."""
    return int(np.all(subtitle_box(device) >= WHITE, axis=2).sum())


def cropped(shape: np.ndarray) -> np.ndarray:
    """A shape cut to the box around its pixels."""
    rows, columns = np.flatnonzero(shape.any(axis=1)), np.flatnonzero(shape.any(axis=0))
    assert rows.size and columns.size, "nothing was drawn"
    return shape[rows[0]:rows[-1] + 1, columns[0]:columns[-1] + 1]


def drawn_text(player, device) -> np.ndarray:
    """The shape of the subtitle text on a film that stands still: the bright pixels that go when the subtitles do."""
    with_text = subtitle_box(device, "subtitles", *ONE_LINE).astype(int)
    player.rexx("SUBTITLE OFF")
    time.sleep(2)
    without = subtitle_box(device, "without", *ONE_LINE).astype(int)
    changed = np.abs(with_text - without).max(axis=2) > 40
    return cropped(changed & (with_text.min(axis=2) >= BRIGHT))


def likeness(shape: np.ndarray, text: str, font: Path = FONT, slant: float = 0.0) -> float:
    """How much of @shape and of @text as the host draws it with @font, leaning by @slant and scaled to the same box,
    lie on each other: 0 to 1."""
    face = ImageFont.truetype(str(font), HOST_FONT_SIZE)
    left, top, right, bottom = face.getbbox(text)
    picture = Image.new("L", (right + 2 * HOST_FONT_SIZE, bottom + HOST_FONT_SIZE), 0)
    ImageDraw.Draw(picture).text((HOST_FONT_SIZE // 2, HOST_FONT_SIZE // 2), text, fill=255, font=face)
    if slant:       # each row moves right by its height over the picture's foot
        picture = picture.transform(picture.size, Image.AFFINE, (1, slant, -slant * picture.size[1], 0, 1, 0), Image.BILINEAR)
    host = Image.fromarray(cropped(np.asarray(picture) >= 128)).resize((shape.shape[1], shape.shape[0]), Image.BILINEAR)
    host = np.asarray(host)
    return float((host & shape).sum()) / float((host | shape).sum())


def windows(device) -> list[str]:
    """The titles of the frontmost screen's windows."""
    return [found.group(5) for line in device.ami.ui_tree().splitlines() if (found := screen.WINDOW_RE.match(line))]


def text_rows(shot: np.ndarray, without: np.ndarray) -> np.ndarray:
    """The rows of @shot that hold subtitle text: bright pixels that are not in @without, the same place with the
    subtitles off."""
    text = (np.abs(shot - without).max(axis=2) > 40) & (shot.min(axis=2) >= BRIGHT)
    rows = np.flatnonzero(text.sum(axis=1) > 3)
    assert rows.size > 8, "no text"
    return rows


@contextlib.contextmanager
def settings_kept(device):
    """The settings file as it was, put back when the block ends."""
    try:
        old = device.download(PREFS)
    except device.mod.AmigaError:
        old = None
    try:
        yield
    finally:
        if old is not None:
            device.upload_bytes(PREFS, old)
            device.upload_bytes("ENV:Kinora.prefs", old)


@pytest.mark.smoke
def test_plays_a_film_to_its_end(player, material):
    """A six-second film: every picture shown, the film ended, nothing failed."""
    player.start(material.ram(films.SHORT))
    end = player.wait_for(lambda s: s.state == "ENDED", 20, "the film's end")
    assert end.error == 0 and (end.width, end.height) == (640, 360)
    assert end.shown >= films.SHORT_MS * films.FRAME_RATE // 1000 - 3 and end.dropped <= 2


@pytest.mark.parametrize("clip", ["xvid.mkv", "mpeg2.ts", "xvid.avi", "mpeg2.vob"])
def test_plays_a_film_the_68k_decodes(player, material, clip):
    """Six seconds of MPEG-4 Part 2 and of interlaced MPEG-2, in Matroska, a transport stream, AVI and a VOB, decoded
    by ffmpegvideo.codec and converted by the VideoCore into the window: every picture shown, the film ended, nothing
    failed; the statistics name both."""
    pictures = len(material.path(clip + ".host.md5").read_text().split())
    player.start(material.ram(clip))
    end = player.wait_for(lambda s: s.state == "ENDED", 30, "the film's end")
    assert end.error == 0 and end.width == 720
    assert end.shown >= pictures - 3 and end.dropped <= 2
    codecs = statistics(player)["Codecs"]
    assert codecs.startswith("video ffmpegvideo.codec, converted by videocore.codec"), codecs


@pytest.mark.parametrize("name", ["d.avi", "e.vob"])
def test_plays_avi_and_a_dvd_with_sound(player, material, name):
    """Four seconds of Xvid with MP3 in AVI and of MPEG-2 with AC-3 in a VOB, sound and picture: the film ended,
    its pictures shown, nothing failed."""
    player.start(material.ram(name))
    end = player.wait_for(lambda s: s.state == "ENDED", 20, "the film's end")
    assert end.error == 0 and end.shown >= 90 and end.dropped <= 2, end


def test_keeps_time(player, material):
    """Ten seconds of the host's clock are ten seconds of film, at the film's picture rate."""
    player.start(material.ram(films.FILM))
    player.playing_at(2000)
    moved, shown = advance(player, 10)
    assert abs(moved - 10000) < 500
    assert abs(shown - 10 * films.FRAME_RATE) <= 8
    assert player.status().dropped <= 2


def test_pause_holds_the_film(player, material):
    """Paused, the time and the picture stand still; played again, they go on from there."""
    player.start(material.ram(films.FILM))
    player.playing_at(2000)
    player.rexx("PAUSE")
    time.sleep(1)
    moved, shown = advance(player, 3)
    held = player.status()
    assert held.state == "PAUSED" and moved == 0 and shown == 0
    player.rexx("PLAY")
    resumed = player.playing_at(held.position_ms + 1000)
    assert resumed.position_ms < held.position_ms + 4000


def test_close_empties_the_player_and_play_opens_the_film_again(player, device, material):
    """CLOSE: no film, the picture box cleared, no statistics, the playlist as it was; a click on the picture then
    plays the playlist's film from its start."""
    film = material.ram(films.FILM)
    player.start(film)
    player.playing_at(2000)
    player.rexx("CLOSE")
    closed = player.wait_for(lambda s: s.state == "EMPTY", 5, "the film closed")
    assert closed.shown == 0 and closed.error == 0 and player.rexx("STATISTICS") == ""
    assert player.rexx("PLAYLIST").splitlines() == [f"* {film}"]
    box = subtitle_box(device, "closed", 0.15, 0.6)
    assert float(box.std()) < 2, "the picture box cleared to one colour"
    x, y, w, h = screen.window(device, "Kinora")
    screen.click(device, x + w // 2, y + h // 3)
    again = player.playing_at(1000)
    assert again.position_ms < 4000 and again.error == 0


@pytest.mark.parametrize("target", [20000, 5000, 36000])
def test_seeks(player, material, target):
    """A seek, forward or back, plays on from the time asked for."""
    player.start(material.ram(films.FILM))
    player.playing_at(1000)
    assert player.rexx(f"SEEK {target}") == "0"
    status = player.wait_for(lambda s: s.state == "PLAYING" and target <= s.position_ms < target + 3000, 8, "after the seek")
    moved, shown = advance(player, 2)
    assert abs(moved - 2000) < 400 and shown >= 40 and status.error == 0


def test_tracks_are_listed_and_chosen(player, material):
    """The film's tracks as it has them; choosing the second audio track makes it the one played, and the film goes on."""
    player.start(material.ram(films.FILM))
    player.playing_at(1000)
    tracks = player.tracks()
    assert [(track.kind, track.stream, track.selected) for track in tracks] == \
        [("AUDIO", FILM_AUDIO, True), ("AUDIO", FILM_AUDIO_2, False), ("SUBTITLE", FILM_SUBTITLES, False)]
    assert [track.language for track in tracks] == ["eng", "pol", "eng"]
    assert player.rexx(f"AUDIO {FILM_AUDIO_2}") == "0"
    assert [track.selected for track in player.tracks()] == [False, True, False]
    time.sleep(3)                       # the sound output is closed and opened again for the new track
    moved, _ = advance(player, 3)
    assert abs(moved - 3000) < 600 and player.status().error == 0


def test_subtitles_are_drawn(player, device, material):
    """With the film's subtitle track on, a picture inside a cue has white text in its lower part; with it off, none."""
    player.start(material.ram(films.FILM))
    player.playing_at(1000)
    player.rexx(f"SUBTITLE {FILM_SUBTITLES}")
    player.rexx("SEEK 5500")            # inside the cue of 5 to 9 s, which the film gives again after the seek
    player.playing_at(6500)
    player.rexx("PAUSE")
    time.sleep(1)
    with_text = white_pixels(device)
    player.rexx("SUBTITLE OFF")         # a film that stands still is drawn again from where it is
    time.sleep(2)
    without = white_pixels(device)
    assert with_text > without + 400, f"{with_text} white pixels with subtitles, {without} without"


def test_a_cue_in_three_alphabets_is_drawn_as_the_font_has_it(player, device, material):
    """A line of Polish, Russian and Greek comes out as the host draws the same line with the same font, and not as
    another line of the same length."""
    player.start(material.ram(films.PLAIN))
    player.playing_at(500)
    assert player.rexx("SUBTITLES " + material.ram(films.WORLD)) == "0"
    player.rexx("SEEK 3000")
    player.playing_at(4000)
    player.rexx("PAUSE")
    time.sleep(1)
    shape = drawn_text(player, device)
    right, wrong = likeness(shape, films.WORLD_TEXT), likeness(shape, "?" * len(films.WORLD_TEXT))
    assert right > 0.6 and right > wrong + 0.25, f"likeness to the cue {right:.2f}, to question marks {wrong:.2f}"


def test_another_font_is_taken_and_kept(player, device, material):
    """A file that is no font is refused; a font file is drawn with and written to the settings; no file is Kinora's
    own font again."""
    if not OTHER_FONT.exists():
        pytest.skip(f"the host has no {OTHER_FONT}")
    font = f"{device.ram}/{OTHER_FONT.name}"
    device.upload_once(OTHER_FONT, font)
    with settings_kept(device):
        player.start(material.ram(films.PLAIN))
        player.playing_at(500)
        world = material.ram(films.WORLD)
        assert player.rexx("SUBTITLES " + world) == "0"
        assert player.rexx("FONT " + world) == ERR_FILE
        assert player.rexx("FONT " + font) == "0"
        player.rexx("SEEK 3000")
        player.playing_at(4000)
        player.rexx("PAUSE")
        time.sleep(1)
        shape = drawn_text(player, device)
        serif, sans = likeness(shape, films.WORLD_TEXT, OTHER_FONT), likeness(shape, films.WORLD_TEXT)
        assert serif > 0.6 and serif > sans + 0.15, f"likeness to the chosen font {serif:.2f}, to Kinora's own {sans:.2f}"
        player.quit()
        assert f"font={font}" in device.download_text(PREFS)
        player.start(material.ram(films.SHORT))
        assert player.rexx("FONT") == "0"
        player.quit()
        assert "font=\n" in device.download_text(PREFS)


def styled_cue(player, device, material, at_ms: int, where=ONE_LINE) -> np.ndarray:
    """The shape of the cue of styles.srt shown at @at_ms, in the part of the window @where names."""
    player.rexx(f"SEEK {at_ms}")
    player.wait_for(lambda s: at_ms + 600 <= s.position_ms < at_ms + 5000, 10, "inside the cue")
    player.rexx("PAUSE")
    time.sleep(1)
    with_text = subtitle_box(device, "subtitles", *where).astype(int)
    player.rexx("SUBTITLE OFF")
    time.sleep(2)
    without = subtitle_box(device, "without", *where).astype(int)
    player.rexx("SUBTITLE FILE")
    player.rexx("PLAY")
    return cropped((np.abs(with_text - without).max(axis=2) > 40) & (with_text.min(axis=2) >= BRIGHT))


def test_styles_are_drawn(player, device, material):
    """Slanted words lean as far as the font module says, heavy words have more ink than the same words plain, and a
    cue marked for the top stands in the picture's upper part."""
    player.start(material.ram(films.PLAIN))
    player.playing_at(500)
    assert player.rexx("SUBTITLES " + material.ram(films.STYLES)) == "0"
    slanted = styled_cue(player, device, material, 1500)
    leaning, upright = likeness(slanted, "Slanted words", slant=SLANT), likeness(slanted, "Slanted words")
    assert leaning > 0.55 and leaning > upright + 0.1, f"likeness to slanted words {leaning:.2f}, to upright ones {upright:.2f}"
    plain = styled_cue(player, device, material, 9500)
    heavy = styled_cue(player, device, material, 17500)
    assert likeness(plain, "Heavy words") > 0.6
    assert heavy.sum() > plain.sum() * 1.2, f"{heavy.sum()} pixels of heavy words, {plain.sum()} of plain ones"
    top = styled_cue(player, device, material, 25500, where=(0.06, 0.12))
    assert likeness(top, "Words at the top") > 0.6


def test_a_cue_goes_into_the_bar_under_a_wide_picture(player, device, material):
    """With the picture shown wider than its box is, a cue of one line stands in the black under it, and is gone from
    there when the subtitles are switched off."""
    material.ram("plain.srt")
    player.start(material.ram(films.PLAIN))
    player.playing_at(500)
    player.rexx("ASPECT 235 100")
    player.rexx("SEEK 1200")            # the cue of 1 to 4 s
    player.wait_for(lambda s: 1800 <= s.position_ms < 3500, 10, "inside the cue")
    player.rexx("PAUSE")
    time.sleep(1)
    low = (0.70, 0.16)                  # the picture's foot and the bar under it
    with_text = subtitle_box(device, "subtitles", *low).astype(int)
    player.rexx("SUBTITLE OFF")
    time.sleep(2)
    without = subtitle_box(device, "without", *low).astype(int)
    black = without.max(axis=2) < 16
    bar = np.flatnonzero(black.mean(axis=1) > 0.98)            # the rows that are black from side to side
    assert bar.size > 20, "no bar under the picture"
    text = np.flatnonzero((with_text.min(axis=2) >= BRIGHT).sum(axis=1) > 3)
    assert text.size > 8, f"no text at {player.status().position_ms} ms"
    assert bar[0] <= text[0] and text[-1] <= bar[-1], f"text in rows {text[[0, -1]]}, the bar in {bar[[0, -1]]}"
    assert likeness(cropped((with_text.min(axis=2) >= BRIGHT)[bar[0]:bar[-1] + 1]), "Hello from Kinora") > 0.5


def test_the_picture_follows_the_window(player, material):
    """The window made smaller, then larger: the picture is shown at the new size each time, and the film runs on."""
    player.start(material.ram(films.FILM))
    player.playing_at(1000)
    shown_at = lambda: tuple(int(n) for n in statistics(player)["Video"].split("shown at ")[1].split("x"))
    first = shown_at()
    player.rexx("SIZE 700 460")
    time.sleep(3)
    small = shown_at()
    player.rexx("SIZE 1300 800")
    time.sleep(3)
    large = shown_at()
    assert small[0] < first[0] < large[0] and small[1] < first[1] < large[1], f"shown at {first}, then {small}, then {large}"
    moved, _ = advance(player, 2)
    assert abs(moved - 2000) < 500 and player.status().error == 0


def test_a_click_on_the_picture_pauses_and_plays(player, device, material):
    """A click on the picture in the window holds the film, the next one lets it run again."""
    player.start(material.ram(films.FILM))
    player.playing_at(1000)
    x, y, w, h = screen.window(device, "Kinora")
    screen.click(device, x + w // 2, y + h // 3)
    player.wait_for(lambda s: s.state == "PAUSED", 5, "paused by the click")
    screen.click(device, x + w // 2, y + h // 3)
    player.wait_for(lambda s: s.state == "PLAYING", 5, "playing again after the second click")


def test_a_double_click_moves_the_picture_to_the_full_screen_and_back(player, device, material):
    """A double click on the picture in the window puts it on the full screen, the film playing as before; a double
    click there brings it back."""
    player.start(material.ram(films.FILM))
    player.playing_at(1000)
    x, y, w, h = screen.window(device, "Kinora")
    device.ami.input_click(x + w // 2, y + h // 3, count=2)
    full = player.wait_for(lambda s: s.full_screen, 8, "on the full screen after the double click")
    player.wait_for(lambda s: s.state == "PLAYING", 5, "playing on the full screen")
    assert full.error == 0
    time.sleep(1)
    device.ami.input_click(x + w // 2, y + h // 3, count=2)
    player.wait_for(lambda s: not s.full_screen and s.state == "PLAYING", 8, "back in the window, playing")


def test_many_moves_do_not_stop_the_reading(player, material):
    """Forty moves in a film that stands still, then play: the film runs.  The packets a move throws away once stayed
    counted, until the reading stopped for good."""
    player.start(material.ram(films.FILM))
    player.playing_at(1000)
    player.rexx("PAUSE")
    for move in range(40):
        player.rexx(f"SEEK {2000 + (move * 7919) % 30000}")
    player.rexx("PLAY")
    time.sleep(2)
    moved, shown = advance(player, 3)
    assert abs(moved - 3000) < 500 and shown >= 3 * films.FRAME_RATE - 10 and player.status().error == 0


def test_the_film_goes_on_after_the_machine_was_held_up(player, device, material):
    """A job above the audio task's priority holds the machine for seconds; afterwards sound and picture run on.
    Both sound blocks then come back from AHI at once, and one sent behind a block that is over was never played."""
    player.start(material.ram(films.FILM))
    player.playing_at(1000)
    device.upload_bytes(f"{device.ram}/hog", HOG.encode())
    for _ in range(3):
        device.exec(f"Execute {device.ram}/hog", timeout=120)
        time.sleep(1)
        moved, shown = advance(player, 3)       # the agent may answer late after the job: the film's own time is the measure
        assert moved > 2500 and shown >= moved * films.FRAME_RATE // 1000 - 15, f"after the hold-up: {moved} ms, {shown} pictures"
    assert player.status().error == 0


def test_statistics_are_told_and_shown(player, device, material):
    """STATISTICS answers what is played and how; with ON a panel of them stands in the picture's corner."""
    player.start(material.ram(films.FILM))
    player.playing_at(3000)
    lines = statistics(player)
    assert lines["Video"].startswith("H.264 1280x720, 25.000 pictures a second")
    assert lines["Audio"].startswith("AAC 48000 Hz, 2 channels")
    assert lines["Codecs"] == "video videocore.codec; audio ffmpegaudio.codec"
    rate = float(lines["Pictures"].split()[0])
    assert 23.5 < rate < 26.5 and "running" in lines["Clock"]
    corner = (0.06, 0.25)
    without = subtitle_box(device, "without", *corner)[:, :300]
    player.rexx("STATISTICS ON")
    time.sleep(2)
    panel = subtitle_box(device, "panel", *corner)[:, :300]
    player.rexx("STATISTICS OFF")
    dark = lambda shot: float((np.abs(shot.astype(int) - 28).max(axis=2) < 10).mean())    # the panel's box is a dark grey
    assert dark(panel) > 0.3 and dark(without) < 0.05, f"{dark(panel):.2f} of the corner dark with the panel, {dark(without):.2f} without"


def test_a_subtitle_file_beside_the_film_is_shown(player, material):
    """plain.srt beside plain.mkv is taken without asking and listed as the track shown."""
    material.ram("plain.srt")
    player.start(material.ram(films.PLAIN))
    player.playing_at(500)
    subtitles = [track for track in player.tracks() if track.kind == "SUBTITLE"]
    assert [(track.stream, track.selected) for track in subtitles] == [(TRACK_FILE, True)]


def test_full_screen_and_back(player, material):
    """The picture moves to a screen of its own and back while the film plays, at the film's rate in both."""
    player.start(material.ram(films.FILM))
    player.playing_at(1000)
    assert player.rexx("FULLSCREEN") == "1"
    time.sleep(2)
    moved, shown = advance(player, 6)
    assert player.status().full_screen and abs(moved - 6000) < 500 and shown >= 6 * films.FRAME_RATE - 10
    assert player.rexx("FULLSCREEN") == "0"
    time.sleep(2)
    moved, shown = advance(player, 4)
    status = player.status()
    assert not status.full_screen and status.error == 0
    assert abs(moved - 4000) < 500 and shown >= 4 * films.FRAME_RATE - 10


def screen_size(device) -> tuple[int, int]:
    """The front screen's width and height, as the agent's tree of the user interface says them."""
    return next(tuple(int(n) for n in line.split()[2].split("x")) for line in device.ami.ui_tree().splitlines()
                if line.startswith("S "))


def test_the_full_screen_controls_are_in_every_part(player, device, material):
    """A move of the pointer brings the controls up along the full screen's foot.  The picture goes on being flipped
    between the screen's parts, so each part must carry the controls' rows, and the film keeps its rate."""
    width, height = screen_size(device)
    player.start(material.ram(films.FILM))
    player.playing_at(1000)
    assert player.rexx("FULLSCREEN") == "1"
    time.sleep(2)
    for step in range(3):
        device.ami.input_move(700 + 30 * step, height // 2)
        time.sleep(0.1)
    time.sleep(1.5)
    feet = []
    for part in range(3):
        device.ami.input_move(760 + part, height // 2)          # the controls go three seconds after the last move
        feet.append(screen.screenshot(device, (0, part * height + height - 40, width, 30), name=f"foot{part}").astype(int))
    assert feet[0].std() > 5, "no controls at the screen's foot"
    for part in (1, 2):
        same = float((np.abs(feet[part] - feet[0]).max(axis=2) < 30).mean())
        assert same > 0.97, f"part {part} carries {same:.2f} of the controls' rows"
    device.ami.input_move(800, height // 2)
    moved, shown = advance(player, 2)
    assert abs(moved - 2000) < 500 and shown >= 2 * films.FRAME_RATE * 3 // 5, f"with the controls up: {moved} ms, {shown} pictures in 2 s"
    assert player.rexx("FULLSCREEN") == "0"


def test_the_full_screen_pointer_shows_only_while_the_film_stands_still(player, device, material):
    """In full screen the pointer stays hidden while the film plays, the controls up or not: the picture flips under
    it, and VideoCore.card would move it a blank after the picture. Paused, it shows, and stays when the controls go."""
    _, height = screen_size(device)
    player.start(material.ram(films.FILM))
    player.playing_at(1000)
    assert player.rexx("FULLSCREEN") == "1"
    time.sleep(1)
    device.ami.input_move(700, height // 2)                     # the controls come up
    time.sleep(1)
    assert not player.status().pointer, "the pointer shows while the film plays"
    player.rexx("PAUSE")
    device.ami.input_move(720, height // 2)                     # the controls stay up
    player.wait_for(lambda s: s.pointer, 2, "the pointer shown while the film stands still")
    time.sleep(4)                                               # the controls go three seconds after the last move
    assert player.status().pointer, "the pointer went with the controls, the film standing still"
    player.rexx("PLAY")
    device.ami.input_move(740, height // 2)
    player.wait_for(lambda s: not s.pointer, 2, "the pointer hidden again while the film plays")
    assert player.rexx("FULLSCREEN") == "0"


def test_a_film_opened_in_full_screen_comes_back_to_the_window(player, material):
    """The playlist's next film opens on the full screen; leaving full screen then shows it in the window."""
    player.start(material.ram(films.SHORT), material.ram(films.FILM))
    player.playing_at(500)
    player.rexx("FULLSCREEN")
    player.rexx("NEXT")
    player.playing_at(2000)
    player.rexx("FULLSCREEN")
    time.sleep(2)
    _, shown = advance(player, 3)
    assert shown >= 3 * films.FRAME_RATE - 10 and player.status().error == 0


@pytest.mark.parametrize("shape", ["4 3", "16 9", "-1 0", "0 0"])
def test_shape_changes_while_paused(player, material, shape):
    """Another shape for a film that stands still: the picture is drawn again, and nothing fails."""
    player.start(material.ram(films.FILM))
    player.playing_at(2000)
    player.rexx("PAUSE")
    time.sleep(1)
    before = player.status()
    player.rexx(f"ASPECT {shape}")
    after = player.wait_for(lambda s: s.shown > before.shown, 8, "the picture drawn again")
    assert after.state == "PAUSED" and after.error == 0


def test_playlist_goes_on_and_back(player, material):
    """At a film's end the next one plays; PREVIOUS and NEXT move between them."""
    short, film = material.ram(films.SHORT), material.ram(films.FILM)
    player.start(short, film)
    player.wait_for(lambda s: s.duration_ms > 30000 and s.state == "PLAYING", 25, "the second film playing")
    assert player.rexx("PLAYLIST").splitlines() == [f"- {short}", f"* {film}"]
    player.rexx("PREVIOUS")
    player.wait_for(lambda s: s.duration_ms < 10000 and s.state == "PLAYING", 10, "the first film again")
    player.rexx("NEXT")
    player.wait_for(lambda s: s.duration_ms > 30000 and s.state == "PLAYING", 10, "the second film again")


def test_chapters_are_listed(player, material):
    """The film's three chapters with their times."""
    player.start(material.ram(films.FILM))
    player.playing_at(500)
    assert player.rexx("CHAPTERS").splitlines() == ["0 Opening", "10000 Middle", "25000 Closing"]


def test_delays_are_taken(player, material):
    """The sound put 200 ms behind the picture: the film goes on, at its rate."""
    player.start(material.ram(films.FILM))
    player.playing_at(1000)
    player.rexx("DELAY 200 500")
    time.sleep(1)
    moved, shown = advance(player, 4)
    assert abs(moved - 4000) < 500 and shown >= 4 * films.FRAME_RATE - 10 and player.status().error == 0


def test_subtitles_take_their_size_and_distance(player, device, material):
    """A cue drawn at 150 % is half as tall again as at 100 %, and at a distance of 20 % from the frame's foot it
    stands well above where it stood at 4 %; the numbers are kept within their ranges."""
    with settings_kept(device):
        player.start(material.ram(films.PLAIN))
        player.playing_at(500)
        assert player.rexx("SUBTITLESIZE 500") == "200" and player.rexx("SUBTITLEDISTANCE 99") == "30"
        assert player.rexx("SUBTITLES " + material.ram(films.WORLD)) == "0"
        player.rexx("SEEK 3000")
        player.playing_at(4000)
        player.rexx("PAUSE")
        time.sleep(1)
        player.rexx("SUBTITLESIZE 100")     # each change draws the paused film again from where it is: every shot
        player.rexx("SUBTITLEDISTANCE 4")   # is of that picture, the one the pause showed perhaps not
        time.sleep(2)
        tall = (0.30, 0.66)                 # from above the raised cue down to the picture's foot
        normal = subtitle_box(device, "normal", *tall).astype(int)
        player.rexx("SUBTITLESIZE 150")     # a film that stands still is drawn again with it
        time.sleep(2)
        large = subtitle_box(device, "large", *tall).astype(int)
        player.rexx("SUBTITLESIZE 100")
        player.rexx("SUBTITLEDISTANCE 20")
        time.sleep(2)
        raised = subtitle_box(device, "raised", *tall).astype(int)
        player.rexx("SUBTITLE OFF")
        time.sleep(2)
        without = subtitle_box(device, "without", *tall).astype(int)
        normal_rows, large_rows, raised_rows = (text_rows(shot, without) for shot in (normal, large, raised))
        ratio = (large_rows[-1] - large_rows[0] + 1) / (normal_rows[-1] - normal_rows[0] + 1)
        assert 1.3 < ratio < 1.7, f"the cue at 150 % is {ratio:.2f} times as tall"
        _, _, _, window_height = screen.window(device, "Kinora")
        lifted = int(normal_rows[-1] - raised_rows[-1])
        assert lifted > 0.08 * window_height, f"at 20 % the cue's foot stands {lifted} rows higher"
        player.quit()


def full_screen_at(player, device, width: int, why: str) -> None:
    """The full screen opened and closed again, checked to be @width wide and a whole number of the Workbench's
    heights tall (it is its parts tall)."""
    _, height = screen_size(device)         # the Workbench's, in front while the picture is in the window
    assert player.rexx("FULLSCREEN") == "1", f"{why}: no full screen"
    time.sleep(2)
    full_width, full_height = screen_size(device)
    assert player.rexx("FULLSCREEN") == "0"
    assert full_width == width and full_height % height == 0, f"{why}: a full screen of {full_width}x{full_height}"


def test_the_full_screen_takes_the_mode_chosen_or_else_the_workbenchs(player, device, material):
    """SCREENMODE takes the Workbench's mode or a display ID and refuses one the full screen cannot be in (a native
    mode); the full screen opens in the mode chosen; a mode in the settings that cannot be used (gone with another
    monitor) gives the Workbench's."""
    width, _ = screen_size(device)
    with settings_kept(device):
        player.start(material.ram(films.FILM))
        player.playing_at(1000)
        setting, workbench = player.rexx("SCREENMODE WORKBENCH").split()
        assert setting == "WORKBENCH"
        assert player.rexx("SCREENMODE 0x00000000").split() == ["WORKBENCH", workbench], "a native mode was taken"
        assert player.rexx(f"SCREENMODE {workbench}").split() == [workbench, workbench]
        full_screen_at(player, device, width, "the mode chosen")
        player.quit()
        prefs = device.download_text(PREFS)
        assert f"screenmode={workbench}" in prefs
        gone = prefs.replace(f"screenmode={workbench}", "screenmode=0x00000000")
        device.upload_bytes("ENV:Kinora.prefs", gone.encode("latin-1"))
        player.start(material.ram(films.FILM))
        player.playing_at(1000)
        assert player.rexx("SCREENMODE").split() == ["0x00000000", workbench]
        full_screen_at(player, device, width, "a mode that cannot be used")
        player.quit()


def test_the_settings_window_puts_back_on_cancel_and_saves_at_once(player, device, material):
    """SETTINGS opens the settings window; what is changed while it is open goes back with CANCEL, SAVE writes the
    settings at once, and the window closes either way."""
    with settings_kept(device):
        player.start(material.ram(films.FILM))
        player.playing_at(1000)
        before = player.rexx("SUBTITLESIZE"), player.rexx("SUBTITLEDISTANCE"), player.rexx("SCREENMODE")
        setting, workbench = before[2].split()
        assert player.rexx("SETTINGS") == "1"
        assert "Kinora: Settings" in windows(device)
        player.rexx("SUBTITLESIZE 150")
        player.rexx("SUBTITLEDISTANCE 12")
        player.rexx("SCREENMODE " + (workbench if setting == "WORKBENCH" else "WORKBENCH"))
        assert player.rexx("SETTINGS CANCEL") == "0"
        assert "Kinora: Settings" not in windows(device)
        assert (player.rexx("SUBTITLESIZE"), player.rexx("SUBTITLEDISTANCE"), player.rexx("SCREENMODE")) == before
        player.rexx("SETTINGS")
        player.rexx("SUBTITLESIZE 120")
        assert player.rexx("SETTINGS SAVE") == "0"
        assert "subtitlesize=120" in device.download_text(PREFS), "Save did not write the settings"
        player.quit()


def test_recent_films_are_kept_newest_first_and_opened(player, device, material):
    """Each film that starts playing goes first among the five recent ones, and one played again moves up; RECENT n
    plays one in place of the playlist, as the Open recent menu does; the list is in the settings after the run."""
    short, film = material.ram(films.SHORT), material.ram(films.FILM)
    with settings_kept(device):
        player.start(short)
        player.playing_at(500)
        player.rexx(f"OPEN {film}")
        player.wait_for(lambda s: s.duration_ms > 30000 and s.state == "PLAYING", 10, "the second film playing")
        assert player.rexx("RECENT").splitlines()[:2] == [film, short]
        player.rexx("RECENT 2")
        player.wait_for(lambda s: s.duration_ms < 10000 and s.state == "PLAYING", 10, "the recent film playing")
        assert player.rexx("RECENT").splitlines()[:2] == [short, film]
        assert player.rexx("PLAYLIST").splitlines() == [f"* {short}"]
        player.quit()
        recent = [line for line in device.download_text(PREFS).splitlines() if line.startswith("recent=")]
        assert recent[:2] == [f"recent={short}", f"recent={film}"] and len(recent) <= 5


def test_the_playlist_window_and_the_words_its_buttons_stand_for(player, device, material):
    """PLAYLIST SHOW opens the playlist window and HIDE closes it, its place kept in the settings. Moving the open
    film down takes its mark along; taking it out leaves it playing, unmarked, the film before its place being the
    previous one; clearing the list leaves the film playing too."""
    short, film = material.ram(films.SHORT), material.ram(films.FILM)
    with settings_kept(device):
        player.start(film, short)
        player.playing_at(500)
        player.rexx("PLAYLIST SHOW")
        assert "Kinora: Playlist" in windows(device)
        assert player.rexx("PLAYLIST DOWN 1").splitlines() == [f"- {short}", f"* {film}"]
        assert player.rexx("PLAYLIST REMOVE 2").splitlines() == [f"- {short}"]
        assert player.status().state == "PLAYING"
        player.rexx("PREVIOUS")
        player.wait_for(lambda s: s.duration_ms < 10000 and s.state == "PLAYING", 10, "the film before the place playing")
        assert player.rexx("PLAYLIST").splitlines() == [f"* {short}"]
        assert player.rexx("PLAYLIST CLEAR") == "" and player.status().error == 0
        player.rexx("PLAYLIST HIDE")
        assert "Kinora: Playlist" not in windows(device)
        player.quit()
        assert "playlistwindow=" in device.download_text(PREFS)


def test_settings_are_kept(player, device, material):
    """The volume set in one run is in the settings file after it; the old file is put back."""
    with settings_kept(device):
        player.start(material.ram(films.SHORT))
        player.rexx("VOLUME 70")
        player.quit()
        assert "volume=70" in device.download_text(PREFS)


def test_the_unit_is_taken_or_shared(player, device, material):
    """The sound takes the unit's hardware through AHI's low-level calls; UNIT n SHARED sends it through ahi.device's
    writes instead (the statistics' audio line says "shared"), and UNIT n takes the hardware again; the film plays on
    through both changes. Needs a unit no other program holds."""
    with settings_kept(device):
        kept = dict(line.split("=", 1) for line in device.download_text(PREFS).splitlines() if "=" in line)
        unit = kept.get("unit", "0")
        player.start(material.ram(films.FILM))
        player.rexx(f"UNIT {unit}")
        player.playing_at(2000)
        shared = lambda: statistics(player)["Audio"].endswith(", shared")
        assert not shared()
        player.rexx(f"UNIT {unit} SHARED")
        time.sleep(1)
        assert shared()
        moved, _ = advance(player, 3)
        assert moved > 2500, f"shared: {moved} ms in 3 s"
        player.rexx(f"UNIT {unit}")
        time.sleep(1)
        assert not shared()
        moved, _ = advance(player, 3)
        assert moved > 2500 and player.status().error == 0, f"taken again: {moved} ms in 3 s"


def test_an_unknown_command_is_refused(player, device, material):
    """A word that is no command comes back with a return code, and the player goes on."""
    player.start(material.ram(films.SHORT))
    _, answer = device.ami.arexx("address KINORA\n'DANCE'\nreturn rc\n")
    assert answer.strip() == str(RC_ERROR)
    assert player.status().state in ("PLAYING", "ENDED")

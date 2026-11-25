# SPDX-License-Identifier: MPL-2.0
"""
player.py - Kinora on the Amiga, started detached and driven through its ARexx port KINORA.
"""
from __future__ import annotations

import time
from dataclasses import dataclass

PORT_WAIT_S = 20
STATES = ("EMPTY", "PAUSED", "PLAYING", "ENDED")


@dataclass
class Status:
    """The answer to STATUS."""

    state: str
    position_ms: int
    duration_ms: int
    width: int
    height: int
    shown: int
    dropped: int
    error: int
    full_screen: bool


@dataclass
class Track:
    """One line of the answer to TRACKS."""

    kind: str           # AUDIO or SUBTITLE
    stream: int
    selected: bool
    channels: int
    language: str
    title: str


class Player:
    """One run of Kinora."""

    def __init__(self, device, program: str):
        self.device = device
        self.program = program
        self.job = None

    def start(self, *films: str, options: str = "") -> "Player":
        """Start Kinora with @films and wait for its ARexx port."""
        self.device.break_all("Kinora")
        self.job = self.device.start(self.program, " ".join(films) + (" " + options if options else ""))
        deadline = time.time() + PORT_WAIT_S
        while time.time() < deadline:
            time.sleep(1)
            try:
                self.status()
                return self
            except (AssertionError, self.device.mod.AmigaError):
                continue
        raise AssertionError(f"Kinora's ARexx port did not appear within {PORT_WAIT_S} s")

    def rexx(self, command: str) -> str:
        """One command to the port; its RESULT. Fails when the port refuses it."""
        source = "options results\naddress KINORA\n'%s'\nreturn rc '|' result\n" % command.replace("'", "''")
        _, answer = self.device.retry(lambda: self.device.ami.arexx(source))
        code, _, result = answer.partition(" | ")
        assert code.strip() == "0", f"{command}: the port answered {answer!r}"
        return "" if result.strip() == "RESULT" else result

    def status(self) -> Status:
        """STATUS, read."""
        words = self.rexx("STATUS").split()
        assert len(words) == 9 and words[0] in STATES, f"STATUS answered {words}"
        numbers = [int(word) for word in words[1:]]
        return Status(words[0], *numbers[:7], bool(numbers[7]))

    def tracks(self) -> list[Track]:
        """TRACKS, read."""
        found = []
        for line in self.rexx("TRACKS").splitlines():
            kind, stream, selected, channels, language, *title = line.split(" ", 5)
            found.append(Track(kind, int(stream), selected == "1", int(channels), language, title[0] if title else ""))
        return found

    def wait_for(self, what, timeout: float, why: str) -> Status:
        """Poll STATUS until @what(status) holds; the last status. Fails after @timeout."""
        deadline = time.time() + timeout
        while True:
            status = self.status()
            if what(status):
                return status
            assert time.time() < deadline, f"{why}: not within {timeout} s, last {status}"
            time.sleep(0.5)

    def playing_at(self, position_ms: int, timeout: float = 15) -> Status:
        """Wait until the film plays at or past @position_ms."""
        return self.wait_for(lambda s: s.state == "PLAYING" and s.position_ms >= position_ms, timeout,
                             f"playing at {position_ms} ms")

    def quit(self) -> None:
        """QUIT, and wait for the program to be gone; Ctrl-C when the port does not answer."""
        try:
            self.rexx("QUIT")
        except (AssertionError, self.device.mod.AmigaError):
            pass
        deadline = time.time() + 15
        while self.device.procs("Kinora") and time.time() < deadline:
            time.sleep(1)
        if self.device.procs("Kinora"):
            self.device.break_all("Kinora")
        assert not self.device.procs("Kinora"), "Kinora did not end"

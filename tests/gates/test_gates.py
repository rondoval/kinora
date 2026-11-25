# SPDX-License-Identifier: MPL-2.0
"""The code gates on Kinora's own C (emu68test/gates.py), on every host run: a kernel-doc comment on
every function and the complexity limit. FFmpeg and FreeType (external/) are not ours
to gate."""
from pathlib import Path

import pytest
from emu68test.gates import Gates

pytestmark = pytest.mark.gates

TESTS = Path(__file__).resolve().parents[1]
ROOT = TESTS.parent
FOLDERS = ("demuxer", "audiocodec", "ffmpeg", "player", "tools", "include")
INHERITED = "0.0:"                 # no code predates the gate, so no ceiling is inherited
COMPLEXITY_LIMIT = 10              # without an entry in complexity-allow.txt
COMPLEXITY_ALLOWED = 15            # the highest ceiling an entry gives
SOURCES = sorted(path for folder in FOLDERS for path in (ROOT / folder).rglob("*.[ch]"))
GATES = Gates(ROOT, SOURCES, TESTS / "complexity-allow.txt", INHERITED, COMPLEXITY_LIMIT, COMPLEXITY_ALLOWED)


def test_every_function_has_its_kernel_doc():
    assert GATES.kerneldoc_findings() == []


def test_every_function_stays_within_its_complexity_limit_and_the_allow_list_only_goes_down():
    assert GATES.complexity_findings() == []

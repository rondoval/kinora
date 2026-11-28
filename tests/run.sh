#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Kinora's test suite.
#
#   run.sh host [pytest options]    the code gates; no Amiga
#   run.sh hw [pytest options]      the libraries, their tools and the player on an Amiga:
#                                   run.sh hw --server amiga-pi4 [-k name] [--no-install]
#   run.sh train [pytest options]   the profile-guided build's training round on an Amiga, with a
#                                   profile-generating build (scripts/pgo-train.py runs it)
#
# The suite builds on emu68-common's test harness, which the build installs to
# build/emu68-prefix/share/emu68-testing ($EMU68_TESTING names another place): build first
# (scripts/docker-build.sh). The first run creates tests/.venv from the harness's and the
# suite's requirements. The hardware tier takes its films from build/test-material, which
# tests/material/make.sh makes on first use.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
harness=${EMU68_TESTING:-$root/build/emu68-prefix/share/emu68-testing}
if [ ! -f "$harness/emu68test/plugin.py" ]; then
    echo "run.sh: no test harness in $harness: build first (scripts/docker-build.sh)" >&2
    exit 10
fi
venv="$here/.venv"
if [ ! -x "$venv/bin/python" ] || ! "$venv/bin/python" -c "import numpy, PIL" 2>/dev/null; then
    [ -x "$venv/bin/python" ] || python3 -m venv "$venv"
    "$venv/bin/pip" install -q -r "$harness/requirements.txt" -r "$here/requirements.txt"
fi
export PYTHONPATH="$harness${PYTHONPATH:+:$PYTHONPATH}"
cd "$here"
# the harness looks for these beside a driver stack: Kinora's own are named here
places="--build-dir=$root/build --install-dir=$root/install --amimcp=${AMIMCP:-$root/../amimcp}"
case "${1:-}" in
    host)
        shift
        exec "$venv/bin/python" -m pytest $places gates "$@"
        ;;
    hw)
        shift
        exec "$venv/bin/python" -m pytest $places hw "$@"
        ;;
    train)
        shift
        exec "$venv/bin/python" -m pytest $places train "$@"
        ;;
    *)
        echo "usage: run.sh host|hw|train [pytest options]" >&2
        exit 2
        ;;
esac

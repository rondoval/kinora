#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""The profile-guided build's training round (docs/architecture.md, section 3).

    scripts/pgo-train.py round --server amiga-cm4   all of it: the profile-generating build, the training on the
                                                    Amiga, the merge, and the profile-guided build again
    scripts/pgo-train.py merge                      only the merge: build/pgo-streams/<configuration>.pgo into
                                                    profiles/<configuration>/ (after `tests/run.sh train`)

The training material comes from tests/material/training.sh, which this runs first. The Amiga is the user's to
allow, and shared: ask before a round. After it the machine has the instrumented libraries in their places;
`tests/run.sh hw` installs the profile-guided build there again.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
STREAMS = BUILD / "pgo-streams"
PROFILES = ROOT / "profiles"
CONFIGURATIONS = ("demuxer", "audio", "video")
IMAGE = os.environ.get("KINORA_BUILD_IMAGE", "ghcr.io/rondoval/amiga-build-container:gcc-v16.2")


def run(*command: str, env: dict | None = None) -> None:
    """A command, its failure the script's."""
    print("+", " ".join(command), flush=True)
    subprocess.run(command, check=True, cwd=ROOT, env=env)


def build(mode: str) -> None:
    """The whole build with FFmpeg in profile mode @mode."""
    env = dict(os.environ)
    env["KINORA_CONFIGURE_ARGS"] = f"{env.get('KINORA_CONFIGURE_ARGS', '')} -DKINORA_PGO={mode}".strip()
    run("./build.sh", env=env)


def container(script: str) -> None:
    """A shell script in the build container, the repository at /work."""
    run("docker", "run", "--rm", "-v", f"{ROOT}:/work", "-w", "/work", "-u", f"{os.getuid()}:{os.getgid()}",
        "-e", "HOME=/tmp", IMAGE, "sh", "-ec", script)


def merge() -> None:
    """Each configuration's stream into the .gcda files beside its objects, then into profiles/ with its stamp."""
    for configuration in CONFIGURATIONS:
        stream = STREAMS / f"{configuration}.pgo"
        objects = BUILD / "ffmpeg" / configuration
        stamp = objects / "kinora-profile-stamp"
        if not stream.exists() or not stamp.exists():
            sys.exit(f"no {stream} or {stamp}: a profile-generating build and its training round come first")
        for old in objects.rglob("*.gcda"):
            old.unlink()
        # gcov-tool writes each record to the .gcda path compiled in: beside the objects, in the container's /work
        container(f"m68k-amigaos-gcov-tool merge-stream /work/build/pgo-streams/{configuration}.pgo")
        made = sorted(objects.rglob("*.gcda"))
        if not made:
            sys.exit(f"the {configuration} stream made no .gcda files")
        target = PROFILES / configuration
        shutil.rmtree(target, ignore_errors=True)
        for gcda in made:
            place = target / gcda.relative_to(objects)
            place.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(gcda, place)
        shutil.copy2(stamp, target / "STAMP")
        print(f"{configuration}: {len(made)} profiles in {target}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("stage", choices=["round", "merge"])
    parser.add_argument("--server", help="the Amiga's MCP server, for a round")
    args = parser.parse_args()
    if args.stage == "merge":
        merge()
        return
    if not args.server:
        sys.exit("a round needs --server")
    run("tests/material/training.sh")
    build("GENERATE")
    run("tests/run.sh", "train", "--server", args.server)
    merge()
    build("USE")


if __name__ == "__main__":
    main()

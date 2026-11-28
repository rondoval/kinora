#!/usr/bin/env bash
# Build Kinora for local use: a container build with a dev stamp in every $VER cookie.
#
#   ./build.sh              build
#   ./build.sh --install    build and stage the install tree into ./install
#   ./build.sh --package    build and make build/package/Kinora-<version>-<stamp>.lha, with the Installer script
#
# Knobs: CPU=68020|68040|68060 (default 68040; 68020 builds soft-float).
# Nothing here uploads to an Amiga.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd "$ROOT"

STAMP="dev-$(date +%Y%m%d-%H%M%S)"
if git rev-parse --short HEAD >/dev/null 2>&1; then
	STAMP+="-g$(git rev-parse --short HEAD)"
	git diff --quiet HEAD 2>/dev/null || STAMP+="-dirty"
fi

CPU=${CPU:-68040}
FPU=hard
[ "$CPU" = 68020 ] && FPU=soft

export KINORA_BUILD_STAMP="${STAMP}"
export KINORA_CONFIGURE_ARGS="-DM68K_CPU=${CPU} -DM68K_FPU=${FPU} ${KINORA_CONFIGURE_ARGS:-}"

case "${1:-}" in
	"")
		scripts/docker-build.sh
		;;
	--install)
		scripts/docker-build.sh
		scripts/docker-build.sh --target install
		;;
	--package)
		scripts/docker-build.sh --target package
		ls -t build/package/Kinora-*.lha | head -1
		;;
	*)
		echo "usage: ./build.sh [--install|--package]" >&2
		exit 2
		;;
esac

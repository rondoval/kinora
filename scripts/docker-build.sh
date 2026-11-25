#!/usr/bin/env bash
# Build Kinora inside the Amiga cross-toolchain container.
#
# No local m68k-amigaos toolchain is required: this runs the public image
# ghcr.io/rondoval/amiga-build-container (GCC 16.2 + NDK 3.2), which ships the
# cross-compiler at /opt/m68k-amigaos. The configure incantation and the image tag
# live HERE and nowhere else, so build.sh and CI stay in lock-step.
#
# Usage:
#   scripts/docker-build.sh                    # configure + build everything
#   scripts/docker-build.sh --target install   # ...stage the install() tree into the prefix
#   KINORA_CONFIGURE_ARGS="-DM68K_CPU=68020 -DM68K_FPU=soft" scripts/docker-build.sh
#
# Any arguments are forwarded to `cmake --build <build dir>`.
#
# Environment overrides:
#   KINORA_BUILD_IMAGE     Toolchain image tag
#   KINORA_CONFIGURE_ARGS  Extra args appended to the configure step
#   KINORA_BUILD_DIR       CMake build directory, relative to the workspace (default: build)
#   KINORA_INSTALL_DIR     Install prefix, relative to the workspace (default: install)
#   KINORA_EMU68_PREFIX    An installed Emu68Common to use instead of building the
#                          external/emu68-common submodule
set -euo pipefail

IMAGE=${KINORA_BUILD_IMAGE:-"ghcr.io/rondoval/amiga-build-container:gcc-v16.2"}
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)

# emu68-common comes as the submodule external/emu68-common and is built here, into the
# build directory. KINORA_EMU68_PREFIX names an already installed Emu68Common instead.
EMU68_MOUNT=()
if [ -n "${KINORA_EMU68_PREFIX:-}" ]; then
	if [ ! -d "${KINORA_EMU68_PREFIX}/lib/cmake/Emu68Common" ]; then
		echo "no Emu68Common package under KINORA_EMU68_PREFIX=${KINORA_EMU68_PREFIX}" >&2
		exit 1
	fi
	EMU68_MOUNT=(-v "$(cd -- "${KINORA_EMU68_PREFIX}" && pwd):/emu68:ro" -e KINORA_EMU68_MOUNTED=1)
elif [ ! -f "${ROOT}/external/emu68-common/CMakeLists.txt" ]; then
	echo "external/emu68-common is empty: run git submodule update --init" >&2
	exit 1
fi

if ! command -v docker >/dev/null 2>&1; then
	echo "docker not found in PATH" >&2
	exit 1
fi

# Must contain no single quote. `sh -ec` runs it, so the first failing step aborts the
# build; "$@" is whatever the caller passed us, forwarded to `cmake --build`.
BUILD_RECIPE='
BD=${KINORA_BUILD_DIR:-build}
ID=/work/${KINORA_INSTALL_DIR:-install}

EP=/emu68
if [ -z "${KINORA_EMU68_MOUNTED:-}" ]; then
	EP=/work/$BD/emu68-prefix
	cmake -S external/emu68-common -B "$BD/emu68-common" \
		-DCMAKE_TOOLCHAIN_FILE=/work/cmake/toolchain.cmake \
		-DCMAKE_INSTALL_PREFIX="$EP" \
		${KINORA_CONFIGURE_ARGS:-}
	cmake --build "$BD/emu68-common" -j"$(nproc)" --target install
fi

cmake -S . -B "$BD" \
	-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain.cmake \
	-DCMAKE_INSTALL_PREFIX="$ID" \
	-DCMAKE_PREFIX_PATH="$EP" \
	${KINORA_CONFIGURE_ARGS:-}

cmake --build "$BD" -j"$(nproc)" "$@"
'

# Outputs are written back to the mounted workspace; -u keeps them host-owned.
# A build/ tree is tied to its path inside the container (/work): never configure it
# with a host cmake.
docker run --rm \
	-v "${ROOT}:/work" \
	"${EMU68_MOUNT[@]}" \
	-w /work \
	-u "$(id -u):$(id -g)" \
	-e HOME=/tmp \
	-e LC_ALL=C \
	-e KINORA_CONFIGURE_ARGS \
	-e KINORA_BUILD_DIR \
	-e KINORA_INSTALL_DIR \
	"${IMAGE}" \
	sh -ec "${BUILD_RECIPE}" sh "$@"

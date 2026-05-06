#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Samuele Voltan
#
# Dreamcast build wrapper — invokes the simulant-dc Docker image with the KOS toolchain.
#
# Usage:
#   ./build-dc.sh                 # configure + build
#   ./build-dc.sh configure       # configure only (re-run after CMakeLists changes)
#   ./build-dc.sh clean           # rm -rf build-dc/
#   ./build-dc.sh shell           # open an interactive shell inside the container
#
# Notes:
#   - The container is started with --rm, so /opt/toolchains/dc/kos/environ.sh
#     must be sourced on every invocation.
#   - On Git Bash for Windows, MSYS_NO_PATHCONV=1 is required so that
#     `-v "$(pwd):/src"` is not mangled into a Windows path.
#
# Environment overrides:
#   DOCKER       — path to the docker binary (default: docker on PATH).
#   IMAGE        — Docker image tag (default: simulant-dc).
#   BUILDDIR     — out-of-source build directory (default: build-dc).

set -e

DOCKER="${DOCKER:-docker}"
IMAGE="${IMAGE:-simulant-dc}"
SRCDIR="$(pwd)"
BUILDDIR="${BUILDDIR:-build-dc}"
ACTION="${1:-build}"

case "$ACTION" in
    clean)
        rm -rf "$BUILDDIR"
        echo "Cleaned $BUILDDIR/"
        exit 0
        ;;
    shell)
        export MSYS_NO_PATHCONV=1
        exec "$DOCKER" run --rm -it -v "$SRCDIR:/src" "$IMAGE" bash
        ;;
    configure|build)
        ;;
    *)
        echo "Unknown action: $ACTION (expected: configure | build | clean | shell)"
        exit 1
        ;;
esac

export MSYS_NO_PATHCONV=1

CONFIGURE_CMD='cd /src && cmake -B '"$BUILDDIR"' \
    -DCMAKE_TOOLCHAIN_FILE=/src/cmake/dreamcast-kos.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -G "Unix Makefiles"'

BUILD_CMD='cd /src/'"$BUILDDIR"' && make -j8'

if [ "$ACTION" = "configure" ]; then
    SCRIPT="$CONFIGURE_CMD"
else
    SCRIPT="$CONFIGURE_CMD && $BUILD_CMD"
fi

exec "$DOCKER" run --rm \
    -v "$SRCDIR:/src" \
    "$IMAGE" \
    bash -c "source /opt/toolchains/dc/kos/environ.sh && $SCRIPT"

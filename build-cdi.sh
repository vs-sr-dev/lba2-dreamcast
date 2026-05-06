#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Samuele Voltan
#
# Builds lba2.cdi (Dreamcast disc image) from build-dc/SOURCES/lba2.elf and a
# directory containing the retail LBA2 assets (HQR / OBL / ILE / CFG / PAL /
# Music/*.ogg / VOX/*.vox / VIDEO/*.hqr).
#
# The retail assets are NOT redistributed with this repository. Provide a
# directory pointing to a legitimate copy of the game (e.g. the GOG release
# of "Twinsen's Little Big Adventure 2 Classic").
#
# Layout: data-only CDI with HQR/OBL/ILE/CFG/PAL at root + Music/ + vox/
# + video/. CDDA multi-track layout is not used — music is streamed from OGG
# files via stb_vorbis on snd_stream, which avoids the GD-ROM single-head
# contention that affects cdrom_cdda_play during gameplay.
#
# Output: lba2.cdi in this directory.
#
# Usage:
#   LBA2_ASSETS_DIR=/path/to/retail/Common ./build-cdi.sh
#   ./build-cdi.sh /path/to/retail/Common
#
# Environment overrides:
#   LBA2_ASSETS_DIR — host path to the retail asset directory (required).
#   DOCKER          — path to the docker binary (default: docker on PATH).
#   IMAGE           — Docker image tag (default: simulant-dc).

set -e

DOCKER="${DOCKER:-docker}"
IMAGE="${IMAGE:-simulant-dc}"
SRCDIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SRCDIR"

ASSETSDIR_HOST="${1:-${LBA2_ASSETS_DIR:-}}"
ELF="build-dc/SOURCES/lba2.elf"
OUT="lba2.cdi"

if [ -z "$ASSETSDIR_HOST" ]; then
    echo "ERROR: retail assets path not set." >&2
    echo "  Set LBA2_ASSETS_DIR=/path/to/retail/Common, or pass it as the first arg." >&2
    exit 1
fi
if [ ! -f "$ELF" ]; then
    echo "ERROR: $ELF not found. Run ./build-dc.sh first." >&2
    exit 1
fi
if [ ! -d "$ASSETSDIR_HOST" ]; then
    echo "ERROR: assets dir $ASSETSDIR_HOST not found." >&2
    exit 1
fi

export MSYS_NO_PATHCONV=1

exec "$DOCKER" run --rm \
    -v "$SRCDIR:/src" \
    -v "$ASSETSDIR_HOST:/assets:ro" \
    "$IMAGE" \
    bash -c "
        set -e
        source /opt/toolchains/dc/kos/environ.sh

        rm -rf /tmp/disc
        mkdir -p /tmp/disc

        # Top-level engine assets (HQR / OBL / ILE / CFG / PAL).
        cd /assets
        cp -v *.[Hh][Qq][Rr] /tmp/disc/ 2>/dev/null || true
        cp -v *.[Oo][Bb][Ll] /tmp/disc/ 2>/dev/null || true
        cp -v *.[Ii][Ll][Ee] /tmp/disc/ 2>/dev/null || true
        cp -v *.[Cc][Ff][Gg] /tmp/disc/ 2>/dev/null || true
        cp -v *.[Pp][Aa][Ll] /tmp/disc/ 2>/dev/null || true

        # Music/ — OGG ambients + scene jingles. Streamed by SOUND_KOS_BACKEND
        # via STREAM_KOS::StartMusicOgg using stb_vorbis (no GD-ROM head
        # contention because each file is read once and decoded from main RAM).
        if [ -d /assets/Music ]; then
            mkdir -p /tmp/disc/Music
            cp -v /assets/Music/*.[Oo][Gg][Gg] /tmp/disc/Music/ 2>/dev/null || true
        fi

        # VOX/ — per-language dialog voice files. PlaySample on a long WAV is
        # rerouted by SOUND_KOS_BACKEND to STREAM_KOS::PlayVoicePcm.
        if [ -d /assets/VOX ]; then
            mkdir -p /tmp/disc/vox
            cp -v /assets/VOX/*.[Vv][Oo][Xx] /tmp/disc/vox/ 2>/dev/null || true
        fi

        # VIDEO/VIDEO.HQR — Smacker FMVs played by PLAYACF.CPP via libsmacker.
        if [ -d /assets/VIDEO ]; then
            mkdir -p /tmp/disc/video
            cp -v /assets/VIDEO/*.[Hh][Qq][Rr] /tmp/disc/video/ 2>/dev/null || true
        fi

        echo
        echo '=== /tmp/disc layout ==='
        ls -la /tmp/disc/
        ls -la /tmp/disc/Music/ 2>/dev/null | head -5 || true
        ls -la /tmp/disc/vox/   2>/dev/null | head -5 || true
        ls -la /tmp/disc/video/ 2>/dev/null | head -5 || true
        echo
        du -sh /tmp/disc/ /tmp/disc/* 2>/dev/null
        echo

        cd /src
        # -N: skip padding to CD-R capacity. Pure data CDI, no -c CDDA tracks.
        mkdcdisc \\
            -e $ELF \\
            -o $OUT \\
            -n 'Little Big Adventure 2' \\
            -a 'Adeline / 2.21 / Dreamcast community port' \\
            -D /tmp/disc \\
            -N

        echo
        echo \"=== built: \$(ls -lh /src/$OUT | awk '{print \$5, \$NF}') ===\"
    "

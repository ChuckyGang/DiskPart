#!/usr/bin/env bash
# Build DiskPart using the Bebbo amiga-gcc toolchain via Docker.
#
# Usage:
#   ./docker.sh                # build (forwards to: make TOOLCHAIN=bebbo)
#   ./docker.sh clean          # clean
#   ./docker.sh <make-args>    # any extra args are forwarded to make
#
# Environment overrides:
#   IMAGE=...   docker image (default: trixitron/m68k-amigaos-gcc)
#   AMIGA=...   toolchain path inside the container (default: /opt/amiga)

set -euo pipefail

IMAGE=${IMAGE:-trixitron/m68k-amigaos-gcc}
AMIGA=${AMIGA:-/opt/amiga}

TTY_ARG=()
[ -t 0 ] && [ -t 1 ] && TTY_ARG=(-it)

# Run as root inside the container — the trixitron image has its NDK headers
# locked to root (-rw------- root:root). Docker Desktop on macOS/Windows
# handles host-user ownership mapping automatically. On Linux, the bind-mount
# preserves container ownership, so chown build artifacts back at the end.
if [ "$(uname -s)" = "Linux" ]; then
    INNER='make TOOLCHAIN=bebbo AMIGA="$AMIGA" "$@"; rc=$?; chown -R "$CHOWN_TO" /work; exit $rc'
    exec docker run --rm "${TTY_ARG[@]}" \
        -e "AMIGA=$AMIGA" \
        -e "CHOWN_TO=$(id -u):$(id -g)" \
        -v "$PWD":/work \
        -w /work \
        "$IMAGE" \
        bash -c "$INNER" bash "$@"
else
    exec docker run --rm "${TTY_ARG[@]}" \
        -v "$PWD":/work \
        -w /work \
        "$IMAGE" \
        make TOOLCHAIN=bebbo AMIGA="$AMIGA" "$@"
fi

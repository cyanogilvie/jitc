#!/bin/sh
# Build and test jitc against musl libc inside an Alpine container, using
# rootless podman so the invoking user doesn't need any privileges.
#
# Usage: ci/test-musl.sh [extra-meson-test-args...]
#
# The container image is built once and cached; the host source tree is
# mounted read/write so build artefacts land in ./build_musl_alpine and
# subproject downloads share the host's ./subprojects/ cache.

set -eu

IMAGE=jitc-musl-test
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
BUILD_DIR=build_musl_alpine

if ! command -v podman >/dev/null 2>&1; then
    echo "podman not found; install podman (rootless) to use this script" >&2
    exit 1
fi

podman build -t "$IMAGE" -f "$SCRIPT_DIR/musl-test.Containerfile" "$SCRIPT_DIR"

TESTARGS="$@"
echo "testargs: ($TESTARGS)"
exec podman run --rm -i \
    --userns=keep-id \
    -v "$PROJECT_ROOT:/src" \
    -w /src \
    "$IMAGE" \
    sh -eu <<EOF
if [ ! -d $BUILD_DIR ]; then
    meson setup $BUILD_DIR -Dtestmode=true
fi
meson compile -C $BUILD_DIR
meson test -C $BUILD_DIR --print-errorlogs $TESTARGS
EOF

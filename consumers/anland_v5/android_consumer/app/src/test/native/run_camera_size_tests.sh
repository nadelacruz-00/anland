#!/bin/sh
# Linux host regression tests for the camera size cap. Pure logic; no camera is opened.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_build_dir=$(mktemp -d "${TMPDIR:-/tmp}/anland-camera-size-test.XXXXXX")
trap 'rm -rf -- "$test_build_dir"' EXIT HUP INT TERM

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
    "$test_dir/camera_size_test.c" \
    -o "$test_build_dir/camera_size_test"
"$test_build_dir/camera_size_test"

#!/bin/sh
# Linux host regression tests for mic capture gating and headset routing. Uses a fake
# AAudio and a fake producer; no audio device is opened.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_build_dir=$(mktemp -d "${TMPDIR:-/tmp}/anland-mic-capture-test.XXXXXX")
trap 'rm -rf -- "$test_build_dir"' EXIT HUP INT TERM

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pthread \
    -I"$test_dir/include" -I"$test_dir/../../../../../../../libdisplay_consumer" \
    -I"$test_dir/../../../../../../../common" \
    "$test_dir/mic_capture_test.c" \
    -o "$test_build_dir/mic_capture_test"
"$test_build_dir/mic_capture_test"

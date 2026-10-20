#!/bin/sh
# Linux host regression test for the mutter audio-out gate. Pure logic; no PipeWire.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_build_dir=$(mktemp -d "${TMPDIR:-/tmp}/anland-audio-gate-test.XXXXXX")
trap 'rm -rf -- "$test_build_dir"' EXIT HUP INT TERM

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
    "$test_dir/audio_gate_test.c" \
    -o "$test_build_dir/audio_gate_test"
"$test_build_dir/audio_gate_test"

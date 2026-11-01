#!/usr/bin/env bash
# Repeat the same native executable and preserve every ordered runtime log.
set -euo pipefail
if [ "$#" -ne 2 ]; then
    echo "usage: $0 NATIVE_EXECUTABLE FRESH_OUTPUT" >&2
    exit 2
fi
binary="$1"
output="$2"
owner="$(cd -- "$(dirname -- "$0")/../.." && pwd -P)"
mkdir "$output"
sha256sum "$binary" > "$output/binary.sha256"
for repeat in 1 2 3; do
    mkdir "$output/run-$repeat"
    (cd "$output/run-$repeat" && timeout 15s "$binary") > "$output/repeat-$repeat.log" 2>&1
    if grep -Eqi 'FAIL:|ASSERTION FAIL|FATAL ERROR|assertion .*failed' "$output/repeat-$repeat.log"; then
        echo "FAIL: native runtime, see $output/repeat-$repeat.log" >&2
        exit 1
    fi
    sed -n -f "$owner/tests/rotation/events.patterns" "$output/repeat-$repeat.log" > "$output/repeat-$repeat.events"
    diff -u "$owner/tests/rotation/keycode_events.snapshot" "$output/repeat-$repeat.events"
    echo "PASS: rotation repeat $repeat"
done

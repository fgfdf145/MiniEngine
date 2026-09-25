#!/bin/sh
# Captures every scene make_scenes.py wrote, in the Khronos reference view, into
# assets/khronos/captures/engine/. Pass scene names to capture only those.
#
#   tools/khronos_reference/capture_engine.sh [CompareMetallic ...]
#
# FRAMES (default 1200) frames are rendered before the capture; APP overrides the executable.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
app=${APP:-$root/out/build/macos-debug/app/miniengine_app}
frames=${FRAMES:-1200}
scenes=$root/assets/scenes/khronos
out=$root/assets/khronos/captures/engine
log=$(mktemp)
trap 'rm -f "$log"' EXIT
mkdir -p "$out"

if [ "$#" -eq 0 ]; then
    set -- $(cd "$scenes" && ls *.yaml | sed 's/\.yaml$//')
fi

cd "$root"
for name in "$@"; do
    # A window manager that resizes the window mid-run (macOS Stage Manager) changes the viewport's
    # aspect ratio and so the framing: the log must show exactly the two start-up resizes. One retry.
    for attempt in 1 2; do
        "$app" --scene "$scenes/$name.yaml" --khronos-reference --frames "$frames" --capture "$out/$name.png" >"$log" 2>&1 || true
        resizes=$(grep -c "Scene render targets resized" "$log" || true)
        if [ "$resizes" -eq 2 ] && [ -f "$out/$name.png" ]; then
            echo "$name: $(grep -o 'resized to [0-9x]*' "$log" | tail -1)"
            break
        fi
        echo "$name: attempt $attempt saw $resizes resizes; $( [ "$attempt" -eq 1 ] && echo retrying || echo giving up)"
    done
    if grep -q "\[error\]" "$log"; then
        grep "\[error\]" "$log" | sed "s/^/  $name: /"
    fi
done

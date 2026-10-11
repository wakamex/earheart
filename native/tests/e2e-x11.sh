#!/bin/bash
# End to end on Linux X11: the app in the tray, its global hotkey pressed, a recording played as
# the microphone, the hotkey pressed again, and the text pasted into a focused window, with the
# clipboard's old contents put back afterwards. Then the same started by `earheart --toggle`.
# Needs an X display (run under xvfb-run), xdotool, and the models:
#   EARHEART_MODELS=<dir> xvfb-run -a tests/e2e-x11.sh WAV [TALK_SECONDS]
set -u
cd "$(dirname "$0")/.."
wav=$1 talk=${2:-8}
bin=$PWD/zig-out/bin
export EARHEART_DATA=$(mktemp -d) EARHEART_FAKE_MIC=$wav
# The small speech model, no cleanup, clipboard restore on, and the default hotkey.
cat > "$EARHEART_DATA/settings.json" <<JSON
{"stt":{"engine":"builtin","builtin":{"model":"${EARHEART_STT_MODEL:-parakeet-tdt-0.6b-v3-int8}"}},
 "cleanup":{"enabled":false,"engine":"builtin"},"output":{"mode":"paste","restoreClipboard":true}}
JSON
fail() { echo "FAILED: $*"; cat "$EARHEART_DATA/earheart.log" 2>/dev/null | tail -20; kill $app 2>/dev/null; exit 1; }

"$bin/earheart" --hidden 2> "$EARHEART_DATA/stderr.log" &
app=$!
for i in $(seq 50); do grep -q "started" "$EARHEART_DATA/earheart.log" 2>/dev/null && break; sleep 0.1; done
grep -q "started" "$EARHEART_DATA/earheart.log" || fail "the app did not start"
grep -q "could not register" "$EARHEART_DATA/earheart.log" && fail "the hotkey was refused"

run() {  # $1: how the dictation is started and stopped
    "$bin/paste-target" "the user's own clipboard" 120 > "$EARHEART_DATA/target.out" &
    target=$!
    for i in $(seq 50); do grep -q ready "$EARHEART_DATA/target.out" && break; sleep 0.1; done
    xdotool search --name paste-target windowfocus 2>/dev/null
    $1
    sleep "$talk"
    $1
    wait $target || fail "$2: nothing was pasted"
    pasted=$(sed -n 's/^pasted: //p' "$EARHEART_DATA/target.out")
    after=$(sed -n 's/^clipboard: //p' "$EARHEART_DATA/target.out")
    echo "$2: pasted \"$pasted\""
    [ ${#pasted} -gt 20 ] || fail "$2: the pasted text is too short to be the dictation"
    [ "$after" = "the user's own clipboard" ] || fail "$2: the clipboard was not put back (it holds \"$after\")"
}
run "xdotool key ctrl+shift+space" "hotkey"
run "$bin/earheart --toggle" "second instance"
kill $app
wait $app 2>/dev/null
grep -c '"text"' "$EARHEART_DATA/history.json" | grep -q 2 || fail "history does not hold both dictations"
rm -rf "$EARHEART_DATA"
echo "e2e-x11: ok"

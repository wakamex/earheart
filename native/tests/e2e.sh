#!/bin/bash
# End to end, as a user's dictation goes: the app in the tray, the record hotkey pressed, a
# recording played as the microphone, the hotkey pressed again, and the text pasted into a focused
# window, with the clipboard's old contents back afterwards. Then the same started and stopped by
# `earheart --toggle`, as a desktop without key grabs does it.
#
#   EARHEART_MODELS=<dir> tests/e2e.sh [WAV [TALK_SECONDS]]
#
# Linux: needs an X display with a window manager (under xvfb-run, start one first: with none, SDL
# gives the keyboard to every window it shows) and xdotool. Windows: run from Git Bash in a
# desktop session. macOS: pasting needs Accessibility granted to the program or to whatever started
# it (GitHub's runners grant it to their shell); without it set EARHEART_E2E_NO_PASTE=1, and the
# test checks the fallback instead: the text on the clipboard and the notice in the log. The hotkey
# itself is not pressed on macOS, where a script needs the same permission to press keys.
set -u
cd "$(dirname "$0")/.."
wav=${1:-$PWD/tests/audio/fleurs-en_us-1904.wav} talk=${2:-7}
bin=${EARHEART_BIN:-$PWD/zig-out/bin}
case "$(uname -s)" in
    Linux) press() { xdotool key ctrl+shift+space; } ;;
    Darwin) press() { "$bin/earheart" --toggle; } ;;
    *) press() { "$bin/paste-target" --hotkey; } ;;
esac
# With EARHEART_E2E_SHOT=<file.png> the screen is saved mid-recording, to look at the overlay.
shot() {
    [ -n "${EARHEART_E2E_SHOT:-}" ] || return 0
    case "$(uname -s)" in
        Linux) import -window root "$EARHEART_E2E_SHOT" ;;
        Darwin) screencapture -x "$EARHEART_E2E_SHOT" ;;
        *) powershell -NoProfile -Command "Add-Type -AssemblyName System.Windows.Forms,System.Drawing; \$b=[System.Windows.Forms.SystemInformation]::VirtualScreen; \$i=New-Object System.Drawing.Bitmap \$b.Width,\$b.Height; [System.Drawing.Graphics]::FromImage(\$i).CopyFromScreen(\$b.Left,\$b.Top,0,0,\$i.Size); \$i.Save('$(cygpath -w "$EARHEART_E2E_SHOT")')" ;;
    esac || echo "no screenshot taken"
    EARHEART_E2E_SHOT=
}
export EARHEART_DATA=$(mktemp -d) EARHEART_FAKE_MIC=$wav
# The small speech model, no cleanup, clipboard restore on, and the default hotkey.
cat > "$EARHEART_DATA/settings.json" <<JSON
{"stt":{"engine":"builtin","builtin":{"model":"${EARHEART_STT_MODEL:-parakeet-tdt-0.6b-v3-int8}"}},
 "cleanup":{"enabled":false,"engine":"builtin"},"output":{"mode":"paste","restoreClipboard":true}}
JSON
log=$EARHEART_DATA/earheart.log
fail() { echo "FAILED: $*"; tail -20 "$log" 2>/dev/null; tail -30 "$EARHEART_DATA/target.out" 2>/dev/null; kill $app 2>/dev/null; exit 1; }

"$bin/earheart" --hidden 2> "$EARHEART_DATA/stderr.log" &
app=$!
for i in $(seq 100); do grep -q "started" "$log" 2>/dev/null && break; sleep 0.1; done
grep -q "started" "$log" || fail "the app did not start"
sleep 1
grep -q "could not register" "$log" && fail "a hotkey was refused"

run() {  # $1: how the dictation is started and stopped; $2: what to call it
    if [ -n "${EARHEART_E2E_NO_PASTE:-}" ]; then
        # No paste can arrive: the text must be on the clipboard, and the log must say why.
        $1; sleep "$talk"; $1
        for i in $(seq 200); do [ "$(grep -c "status: done" "$log")" -ge "$3" ] && break; sleep 0.1; done
        [ "$(grep -c "status: done" "$log")" -ge "$3" ] || fail "$2: the dictation did not finish"
        grep -q "auto-paste failed" "$log" || fail "$2: no notice of the paste that could not be sent"
        pasted=$(pbpaste)
    else
        ${EARHEART_E2E_WRAP:-} "$bin/paste-target" "the user's own clipboard" 120 > "$EARHEART_DATA/target.out" 2>&1 &
        target=$!
        for i in $(seq 100); do grep -q ready "$EARHEART_DATA/target.out" && break; sleep 0.1; done
        command -v xdotool >/dev/null && xdotool search --name paste-target windowfocus 2>/dev/null
        sleep 0.5
        $1; sleep 3
        # Mid-recording the overlay is up, and the keyboard is still the target's.
        if command -v xdotool >/dev/null; then
            xdotool search --onlyvisible --name '^Earheart$' >/dev/null || fail "$2: no overlay on screen while recording"
            [ "$(xdotool getwindowfocus getwindowname)" = paste-target ] || fail "$2: the overlay took the keyboard from the focused window"
        fi
        shot
        sleep $((talk - 3)); $1
        wait $target || fail "$2: nothing was pasted"
        pasted=$(sed -n 's/^pasted: //p' "$EARHEART_DATA/target.out" | tr -d '\r')
        after=$(sed -n 's/^clipboard: //p' "$EARHEART_DATA/target.out" | tr -d '\r')
        [ "$after" = "the user's own clipboard" ] || fail "$2: the clipboard was not put back (it holds \"$after\")"
    fi
    echo "$2: delivered \"$pasted\""
    case "$pasted" in *"communication channels"*) ;; *) fail "$2: that is not the dictation" ;; esac
}
run press "hotkey" 1
run "$bin/earheart --toggle" "second instance" 2
kill $app
wait $app 2>/dev/null
[ "$(grep -c '"text"' "$EARHEART_DATA/history.json")" = 2 ] || fail "history does not hold both dictations"
# The speech engine dying at the stop: the words the live preview had decoded are still delivered,
# marked incomplete, with a notice. Three times the clip, so a chunk has committed by the stop.
long=$EARHEART_DATA/long.wav
"$bin/paste-target" --repeat "$wav" 3 "$long" || fail "could not build the long recording"
crash=$("$bin/earheart" dictate --wav "$long" --data "$EARHEART_DATA/crash" --kill-stt-at-stop \
    --set 'cleanup.enabled=false' --set "stt.builtin.model=\"${EARHEART_STT_MODEL:-parakeet-tdt-0.6b-v3-int8}\"" 2>/dev/null | tail -1)
echo "engine killed at the stop: $crash" | cut -c1-260
case "$crash" in *'"outcome":"done"'*'"incomplete":true'*) ;; *) fail "the recovered words were not delivered as an incomplete dictation" ;; esac
case "$crash" in *"communication channels"*) ;; *) fail "the delivered text is not the words decoded before the engine died" ;; esac
case "$crash" in *'"notices":0'*) fail "no notice of the interrupted transcription" ;; esac
rm -rf "$EARHEART_DATA"
echo "e2e: ok"

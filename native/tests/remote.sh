#!/bin/bash
# The remote engines against a stand-in service (tests/fake_openai.py): a dictation transcribed
# and cleaned over HTTP, what was sent, and the wording of a refusal, which must name the service
# and never repeat its answer. Needs python3; no models and no display.
#   tests/remote.sh
set -u
cd "$(dirname "$0")/.."
bin=${EARHEART_BIN:-$PWD/zig-out/bin}
work=$(mktemp -d)
port=$((20000 + RANDOM % 20000))
python3 tests/fake_openai.py $port "$work" &
server=$!
trap 'kill $server 2>/dev/null; rm -rf "$work"' EXIT
sleep 1
fail() { echo "FAILED: $*"; exit 1; }
dictate() {  # $1: base URL
    "$bin/earheart" dictate --wav tests/audio/fleurs-en_us-1904.wav --talk 3 --data "$work/data-$RANDOM" \
        --set 'stt.engine="remote"' --set "stt.baseUrl=\"$1\"" --set 'stt.apiKey="sk-secret"' --set 'stt.model="remote-stt"' --set 'stt.language="en"' \
        --set 'cleanup.engine="remote"' --set "cleanup.baseUrl=\"$1/\"" --set 'cleanup.apiKey="sk-secret"' --set 'cleanup.model="remote-llm"' 2>"$work/stderr" | tail -1
}
out=$(dictate "http://127.0.0.1:$port/v1")
echo "$out" | cut -c1-200
# The transcript went through the service's cleanup, lost the thought, and then the stumbles the
# default style strips ("um", "the the").
case "$out" in *'"outcome":"done","text":"Cleaned: this is the remote transcript"'*) ;; *) fail "the remote dictation did not come back as expected" ;; esac
grep -q '"wellFormed": true' "$work/stt.json" || fail "the upload was not a well-formed form"
grep -q '"model": true' "$work/stt.json" && grep -q '"language": true' "$work/stt.json" || fail "the model or language was not sent"
grep -q '"authorization": "Bearer sk-secret"' "$work/stt.json" || fail "the API key was not sent"
grep -q '"model": "remote-llm"' "$work/cleanup.json" || fail "the cleanup request names no model"
grep -q '"top_k"\|"min_p"' "$work/cleanup.json" && fail "the cleanup request carries settings strict servers refuse"
grep -q 'Editing style' "$work/cleanup.json" || fail "the cleanup prompt has no style directive"
# A refusal: the dictation fails with the service named, the key hinted at, and nothing echoed.
out=$(dictate "http://127.0.0.1:$port/denied/v1")
echo "$out" | cut -c1-200
case "$out" in *'"outcome":"error"'*'STT service error 401 — check the API key in Settings'*) ;; *) fail "a refused request was not reported in the expected words" ;; esac
grep -rq "sk-secret" "$work"/data-*/ 2>/dev/null | grep -v settings.json && fail "the key reached a file other than the settings"
case "$out" in *"bad key"*) fail "the service's answer was repeated" ;; esac
# A service that is not there.
out=$(dictate "http://127.0.0.1:1/v1")
case "$out" in *"Couldn't reach 127.0.0.1:1"*) ;; *) fail "an unreachable service was not reported as such: $out" ;; esac
echo "remote: ok"

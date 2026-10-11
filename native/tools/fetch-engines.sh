#!/bin/sh
# Fetches the speech and cleanup engines' shared libraries for one target into engines/TARGET/,
# checking each archive against its SHA-256. They are the libraries the Electron build's npm
# packages wrap, at the same versions: sherpa-onnx 1.13.3 and llama.cpp b11146 (its "v0.5.0").
#   tools/fetch-engines.sh [linux-x64 | macos-arm64 | windows-x64]
set -eu
cd "$(dirname "$0")/.."
target=${1:-}
if [ -z "$target" ]; then
    case "$(uname -s)-$(uname -m)" in
        Linux-x86_64) target=linux-x64 ;;
        Darwin-arm64) target=macos-arm64 ;;
        *) echo "name a target: linux-x64, macos-arm64 or windows-x64" >&2; exit 2 ;;
    esac
fi
sherpa=https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.13.3
llama=https://github.com/ggml-org/llama.cpp/releases/download/b11146
case "$target" in
    linux-x64)
        set -- "$sherpa/sherpa-onnx-v1.13.3-linux-x64-shared-lib.tar.bz2" 650d3da32694fa48e6e018f7087e4840aace56b3187a294a18ba3b9f51e80943 \
               "$llama/llama-b11146-bin-ubuntu-vulkan-x64.tar.gz" d3ce40fce7403cc93bcf5718fc46c6efb61ed9709f8e5d9f10c86bf0e30e8fb3 ;;
    *) echo "no engines listed for $target yet" >&2; exit 2 ;;
esac
out=engines/$target
tmp=engines/.fetch-$target
rm -rf "$tmp"; mkdir -p "$tmp/x" "$out"
sum() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
while [ $# -gt 0 ]; do
    url=$1 want=$2; shift 2
    file=$tmp/$(basename "$url")
    curl -fsSL -o "$file" "$url"
    got=$(sum "$file")
    [ "$got" = "$want" ] || { echo "$url: SHA-256 is $got, expected $want" >&2; exit 1; }
    case "$file" in *.zip) unzip -q "$file" -d "$tmp/x" ;; *) tar -xf "$file" -C "$tmp/x" ;; esac
done
# Keep the libraries the app loads and what they load in turn; the archives' programs stay out.
# Each versioned library is kept once, under the name the others ask the loader for (libggml.so.0).
find "$tmp/x" \( -name 'libsherpa-onnx-c-api.*' -o -name 'libonnxruntime*' -o -name 'sherpa-onnx-c-api.dll' -o -name 'onnxruntime*.dll' \
    -o -name 'libllama.*' -o -name 'llama.dll' -o -name 'libggml*' ! -name '*rpc*' -o -name 'ggml*.dll' ! -name '*rpc*' -o -name 'libomp*' \) -exec cp -L {} "$out/" \;
for f in "$out"/*.so.[0-9]*; do
    [ -e "$f" ] || continue
    case "$f" in *.so.[0-9]*.*) rm "$f" ;; *) rm -f "${f%.[0-9]*}" ;; esac
done
rm -rf "$tmp"
ls "$out"

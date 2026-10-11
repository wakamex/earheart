#!/bin/sh
# Fetches the speech and cleanup engines' shared libraries for one target into engines/TARGET/,
# checking each archive against its SHA-256.
#
# Speech: sherpa-onnx 1.13.3 from its npm platform package, the very files the Electron build
# ships. The same version's GitHub release is a different build of the library and transcribes a
# fifth of the evaluation corpus differently (see decisions.md).
# Cleanup: llama.cpp b11146, the build node-llama-cpp 3.22.1 calls v0.5.0, from llama.cpp's release.
#
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
npm=https://registry.npmjs.org
llama=https://github.com/ggml-org/llama.cpp/releases/download/b11146
case "$target" in
    linux-x64)
        set -- "$npm/sherpa-onnx-linux-x64/-/sherpa-onnx-linux-x64-1.13.3.tgz" 6e2ab763715980c59eaac602165ce1aa00e33449e6c811fae6610e0d801a4f3c \
               "$llama/llama-b11146-bin-ubuntu-vulkan-x64.tar.gz" d3ce40fce7403cc93bcf5718fc46c6efb61ed9709f8e5d9f10c86bf0e30e8fb3 ;;
    macos-arm64)
        set -- "$npm/sherpa-onnx-darwin-arm64/-/sherpa-onnx-darwin-arm64-1.13.3.tgz" 676b56c823273f42a9cb8b05a17a270605c7205227a5a62068eb80ed54eeb2ca \
               "$llama/llama-b11146-bin-macos-arm64.tar.gz" 1ad3f9eff80edb9dbef4259ad564d1720612ef7eea48fa4afed0e54f5f3d5711 ;;
    windows-x64)
        set -- "$npm/sherpa-onnx-win-x64/-/sherpa-onnx-win-x64-1.13.3.tgz" d6229f3d756dfcfb631d0dc41a3e21dab0646b7f455351431b36977a5d54bc9e \
               "$llama/llama-b11146-bin-win-vulkan-x64.zip" 55a378aa095b466979d85075234f66d7655c7a7483222af0c006c0e55b4d7bd6 ;;
    *) echo "no engines listed for $target" >&2; exit 2 ;;
esac
out=engines/$target
tmp=engines/.fetch-$target
rm -rf "$tmp" "$out"; mkdir -p "$tmp/x" "$out"
sum() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
while [ $# -gt 0 ]; do
    url=$1 want=$2; shift 2
    file=$tmp/$(basename "$url")
    curl -fsSL -o "$file" "$url"
    got=$(sum "$file")
    [ "$got" = "$want" ] || { echo "$url: SHA-256 is $got, expected $want" >&2; exit 1; }
    case "$file" in *.zip) unzip -q "$file" -d "$tmp/x" ;; *) tar -xf "$file" -C "$tmp/x" ;; esac
done
# Keep the libraries the app loads and what they load in turn: the C interfaces, ONNX Runtime, and
# llama.cpp with its CPU and GPU backends. The archives' programs and the Node addon stay out, and
# so does llama.cpp's network backend where it is a plug-in (on macOS libggml links it).
find "$tmp/x" \( -type f -o -type l \) \( -name '*sherpa-onnx-c-api.*' -o -name '*onnxruntime*' -o -name 'libllama.*' -o -name 'llama.dll' \
    -o -name 'libggml*' -o -name 'ggml*.dll' -o -name 'libomp*' \) ! -name '*rpc*.so' ! -name '*rpc*.dll' -exec cp -L {} "$out/" \;
# A versioned library is kept once, under the name the others ask the loader for (libggml.so.0,
# libggml.0.dylib).
for f in "$out"/*.so.[0-9]* "$out"/*.[0-9]*.dylib; do
    [ -e "$f" ] || continue
    case "$f" in
        *.so.[0-9]*.*) rm "$f" ;;
        *.so.[0-9]*) rm -f "${f%.[0-9]*}" ;;
        *onnxruntime*) ;;
        *.[0-9]*.[0-9]*.[0-9]*.dylib) rm "$f" ;;
        *) base=${f%.dylib}; rm -f "${base%.[0-9]*}.dylib" ;;
    esac
done
rm -rf "$tmp"
ls "$out"

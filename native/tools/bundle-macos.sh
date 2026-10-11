#!/bin/sh
# Makes zig-out/Earheart.app from a macOS build: the executable and its engines, an Info.plist
# that lets it ask for the microphone and keeps it out of the Dock, the icon, and a signature.
# macOS files permission grants (Microphone, Accessibility) under the signing identity, so a build
# signed with a named certificate keeps its grants across updates; an ad hoc signature, the
# default here, loses them with every build.
#   tools/bundle-macos.sh [SIGNING_IDENTITY]
set -eu
cd "$(dirname "$0")/.."
identity=${1:--}
version=$(sed -n 's/.*\.version = "\(.*\)".*/\1/p' build.zig.zon)
app=zig-out/Earheart.app
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp zig-out/bin/earheart "$app/Contents/MacOS/"
cp -R zig-out/bin/engines "$app/Contents/MacOS/engines"
# The icon in the sizes an icon set wants.
set=zig-out/Earheart.iconset
rm -rf "$set"; mkdir -p "$set"
for size in 16 32 128 256 512; do
    sips -z $size $size ../assets/icon.png --out "$set/icon_${size}x${size}.png" >/dev/null
    sips -z $((size * 2)) $((size * 2)) ../assets/icon.png --out "$set/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$set" -o "$app/Contents/Resources/Earheart.icns"
rm -rf "$set"
cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>Earheart</string>
    <key>CFBundleDisplayName</key><string>Earheart</string>
    <key>CFBundleIdentifier</key><string>dev.wakamex.earheart</string>
    <key>CFBundleExecutable</key><string>earheart</string>
    <key>CFBundleIconFile</key><string>Earheart</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$version</string>
    <key>CFBundleVersion</key><string>$version</string>
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <key>LSUIElement</key><true/>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSMicrophoneUsageDescription</key><string>Earheart records your voice while you dictate and transcribes it on this Mac.</string>
</dict>
</plist>
PLIST
# The libraries first, then the app around them.
find "$app/Contents/MacOS/engines" -type f -exec codesign --force --sign "$identity" {} \;
codesign --force --sign "$identity" "$app"
codesign --verify --strict "$app"
echo "$app"

#!/usr/bin/env bash
# Builds the macOS release (universal AU + VST3), signs it with a Developer ID,
# notarizes and staples it, and wraps it in a drag-to-install DMG.
#
#   scripts/release-macos.sh            # build dist/FlayrTune-<version>-macOS.dmg
#   scripts/release-macos.sh --upload   # ...and attach it to the GitHub release v<version>
#
# Needs a "Developer ID Application" identity in the keychain and a notarytool
# keychain profile (xcrun notarytool store-credentials <profile> ...).
set -euo pipefail

IDENTITY="${IDENTITY:-Developer ID Application: Flayr Labs LLC (NRNU83UJ68)}"
NOTARY_PROFILE="${NOTARY_PROFILE:-flayr-notary}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(sed -n 's/^project(FlayrTune VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
BUILD="$ROOT/build-release-macos"
DIST="$ROOT/dist"
DMG="$DIST/FlayrTune-$VERSION-macOS.dmg"

echo "==> Building Flayr Tune $VERSION (universal)"
cmake -S "$ROOT" -B "$BUILD" -G Xcode -DFLAYR_TUNE_COPY_AFTER_BUILD=OFF -DFLAYR_TUNE_BUILD_TESTS=OFF >/dev/null
cmake --build "$BUILD" --config Release --target FlayrTune_AU FlayrTune_VST3 -- -quiet

ART="$BUILD/FlayrTune_artefacts/Release"
STAGE="$(mktemp -d)/Flayr Tune"
mkdir -p "$STAGE" "$DIST"
cp -R "$ART/AU/Flayr Tune.component" "$ART/VST3/Flayr Tune.vst3" "$STAGE/"

echo "==> Signing"
for bundle in "$STAGE/Flayr Tune.component" "$STAGE/Flayr Tune.vst3"; do
    codesign --force --options runtime --timestamp --sign "$IDENTITY" "$bundle"
    codesign --verify --strict --verbose=1 "$bundle"
done

echo "==> Notarizing the plug-ins"
ZIP="$(mktemp -d)/plugins.zip"
ditto -c -k --keepParent "$STAGE" "$ZIP"
xcrun notarytool submit "$ZIP" --keychain-profile "$NOTARY_PROFILE" --wait
xcrun stapler staple "$STAGE/Flayr Tune.component"
xcrun stapler staple "$STAGE/Flayr Tune.vst3"

echo "==> Building the DMG"
ln -s "/Library/Audio/Plug-Ins/Components" "$STAGE/Drag AU here (Logic, GarageBand)"
ln -s "/Library/Audio/Plug-Ins/VST3" "$STAGE/Drag VST3 here (Ableton, FL, Cubase, Reaper...)"
cp "$ROOT/packaging/macos/Install.txt" "$STAGE/Install.txt"
rm -f "$DMG"
hdiutil create -volname "Flayr Tune $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
codesign --force --timestamp --sign "$IDENTITY" "$DMG"

echo "==> Notarizing the DMG"
xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
xcrun stapler staple "$DMG"
spctl --assess --type open --context context:primary-signature --verbose "$DMG"

echo "==> Done: $DMG"

if [[ "${1:-}" == "--upload" ]]; then
    gh release upload "v$VERSION" "$DMG" --clobber --repo FlayrLabs/flayr-tune
    echo "==> Uploaded to release v$VERSION"
fi

#!/usr/bin/env bash
# Builds the macOS release (universal AU + VST3), signs it with a Developer ID,
# notarizes and staples it, and wraps it in a drag-to-install DMG.
#
#   scripts/release-macos.sh            # build dist/FlayrTune-<version>-macOS.dmg
#   scripts/release-macos.sh --upload   # ...and attach it to the GitHub release v<version>
#
# Needs a "Developer ID Application" identity in the keychain and a notarytool
# keychain profile (xcrun notarytool store-credentials <profile> ...).
#
# Only signs a tagged release: the checkout must be clean, HEAD must sit exactly on a
# v<version> tag that matches CMakeLists.txt and the tag on GitHub, and the build runs
# in a fresh clone of that tag with tests, tools and git hooks off. Nothing from the
# working tree (local edits, untracked files, build dirs) reaches the signed bundle.
set -euo pipefail

IDENTITY="${IDENTITY:-Developer ID Application: Flayr Labs LLC (NRNU83UJ68)}"
NOTARY_PROFILE="${NOTARY_PROFILE:-flayr-notary}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(sed -n 's/^project(FlayrTune VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
TAG="v$VERSION"
DIST="$ROOT/dist"
DMG="$DIST/FlayrTune-$VERSION-macOS.dmg"

die() { echo "error: $*" >&2; exit 1; }

[[ -n "$VERSION" ]] || die "could not read the version from CMakeLists.txt"
[[ -z "$(git -C "$ROOT" status --porcelain)" ]] || die "the working tree has uncommitted or untracked changes"
HEAD_SHA="$(git -C "$ROOT" rev-parse HEAD)"
TAG_SHA="$(git -C "$ROOT" rev-parse -q --verify "refs/tags/$TAG^{commit}" || true)"
[[ -n "$TAG_SHA" ]] || die "tag $TAG does not exist; tag the release commit first"
[[ "$HEAD_SHA" == "$TAG_SHA" ]] || die "HEAD is not at $TAG (HEAD $HEAD_SHA, $TAG $TAG_SHA)"
REMOTE_SHA="$(git -C "$ROOT" ls-remote origin "refs/tags/$TAG^{}" | cut -f1)"
[[ -n "$REMOTE_SHA" ]] || REMOTE_SHA="$(git -C "$ROOT" ls-remote origin "refs/tags/$TAG" | cut -f1)"
[[ "$REMOTE_SHA" == "$TAG_SHA" ]] || die "$TAG on origin (${REMOTE_SHA:-missing}) differs from the local tag; push it first"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SRC="$WORK/src"
BUILD="$WORK/build"

echo "==> Cloning $TAG ($TAG_SHA) into a clean directory"
git -c core.hooksPath=/dev/null clone --quiet --depth 1 --branch "$TAG" --no-recurse-submodules "file://$ROOT" "$SRC"
[[ "$(git -C "$SRC" rev-parse HEAD)" == "$TAG_SHA" ]] || die "the clean clone is not at $TAG"

echo "==> Building Flayr Tune $VERSION (universal)"
cmake -S "$SRC" -B "$BUILD" -G Xcode \
    -DFLAYR_TUNE_COPY_AFTER_BUILD=OFF -DFLAYR_TUNE_BUILD_TESTS=OFF -DFLAYR_TUNE_BUILD_TOOLS=OFF >/dev/null
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
cp "$SRC/packaging/macos/Install.txt" "$STAGE/Install.txt"
rm -f "$DMG"
hdiutil create -volname "Flayr Tune $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
codesign --force --timestamp --sign "$IDENTITY" "$DMG"

echo "==> Notarizing the DMG"
xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
xcrun stapler staple "$DMG"
spctl --assess --type open --context context:primary-signature --verbose "$DMG"

echo "==> Done: $DMG"

if [[ "${1:-}" == "--upload" ]]; then
    gh release upload "$TAG" "$DMG" --clobber --repo FlayrLabs/flayr-tune
    echo "==> Uploaded to release $TAG"

    # Checksums for every file on the release, so users can verify their download.
    sums_dir="$(mktemp -d)"
    gh release download "v$VERSION" --repo FlayrLabs/flayr-tune --dir "$sums_dir" --pattern 'FlayrTune-*'
    (cd "$sums_dir" && shasum -a 256 FlayrTune-* > SHA256SUMS.txt && cat SHA256SUMS.txt)
    gh release upload "v$VERSION" "$sums_dir/SHA256SUMS.txt" --clobber --repo FlayrLabs/flayr-tune
    rm -rf "$sums_dir"
    echo "==> Uploaded SHA256SUMS.txt"
fi

#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"
DIST="$ROOT/dist"
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
MIN_IOS="${MIN_IOS:-15.0}"

rm -rf "$BUILD" "$DIST"
mkdir -p "$BUILD" "$DIST"

build_universal_dylib() {
  local name="$1"
  local source="$2"
  local install_name="$3"

  for arch in arm64 arm64e; do
    echo "==> Building $name ($arch)"
    xcrun --sdk iphoneos clang \
      -arch "$arch" \
      -miphoneos-version-min="$MIN_IOS" \
      -isysroot "$SDK" \
      -dynamiclib -O2 -Wall -Wextra \
      -Wl,-not_for_dyld_shared_cache \
      -Wl,-install_name,"$install_name" \
      -o "$BUILD/$name.$arch.dylib" \
      "$ROOT/$source"
  done

  lipo -create \
    "$BUILD/$name.arm64.dylib" \
    "$BUILD/$name.arm64e.dylib" \
    -output "$DIST/$name.dylib"

  codesign --force --sign - --timestamp=none "$DIST/$name.dylib"
  file "$DIST/$name.dylib"
  lipo -archs "$DIST/$name.dylib"
  codesign -v "$DIST/$name.dylib"

  local interpose
  interpose="$(otool -l "$DIST/$name.dylib" | grep -c __interpose || true)"
  if [[ "$interpose" -lt 1 ]]; then
    echo "ERROR: $name has no __interpose section" >&2
    exit 1
  fi
}

build_universal_dylib \
  "Liter8SpawnBridge" \
  "src/spawnbridge.c" \
  "/var/jb/usr/lib/Liter8SpawnBridge.dylib"

build_universal_dylib \
  "lhook-scoped" \
  "src/scoped_lhook.c" \
  "/usr/lib/lhook"

echo
echo "Built:"
ls -lh "$DIST"

#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"
DIST="$ROOT/dist"
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
MIN_IOS="${MIN_IOS:-15.0}"

rm -rf "$BUILD" "$DIST"
mkdir -p "$BUILD" "$DIST"

for arch in arm64 arm64e; do
  echo "==> Building $arch"
  xcrun --sdk iphoneos clang \
    -arch "$arch" \
    -miphoneos-version-min="$MIN_IOS" \
    -isysroot "$SDK" \
    -dynamiclib -O2 -Wall -Wextra \
    -Wl,-not_for_dyld_shared_cache \
    -Wl,-install_name,/var/jb/usr/lib/TweakInject/Liter8SpawnBridge.dylib \
    -o "$BUILD/Liter8SpawnBridge.$arch.dylib" \
    "$ROOT/src/spawnbridge.c"
done

lipo -create \
  "$BUILD/Liter8SpawnBridge.arm64.dylib" \
  "$BUILD/Liter8SpawnBridge.arm64e.dylib" \
  -output "$DIST/Liter8SpawnBridge.dylib"

codesign --force --sign - --timestamp=none "$DIST/Liter8SpawnBridge.dylib"
cp "$ROOT/TweakInject/Liter8SpawnBridge.plist" "$DIST/Liter8SpawnBridge.plist"

file "$DIST/Liter8SpawnBridge.dylib"
lipo -archs "$DIST/Liter8SpawnBridge.dylib"
codesign -v "$DIST/Liter8SpawnBridge.dylib"

echo "Built: $DIST/Liter8SpawnBridge.dylib"

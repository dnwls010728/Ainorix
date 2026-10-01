#!/bin/sh
# Builds the Android runtime (the player as liboe_player.so, a NativeActivity)
# that `oe package --android` puts into APKs:
#   build/bin/android/<abi>/liboe_player.so  (and runtime/android/<abi>/, commit it)
# Needs the Android NDK (Android Studio: SDK Manager > SDK Tools > NDK), found
# through ANDROID_NDK_HOME / ANDROID_NDK_ROOT or the newest ndk/ folder of the
# SDK (ANDROID_HOME, ANDROID_SDK_ROOT, ~/Android/Sdk, ~/Library/Android/sdk).
# Usage: ./build_android.sh [Release|Debug] [ABIs, default "arm64-v8a x86_64"]
#   arm64-v8a = phones and tablets, x86_64 = emulator, armeabi-v7a = old 32-bit phones
set -e
cd "$(dirname "$0")"
CONFIG=${1:-Release}
ABIS=${2:-${OE_ANDROID_ABIS:-"arm64-v8a x86_64"}}
NDK=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-$ANDROID_NDK}}
for sdk in "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$HOME/Android/Sdk" "$HOME/Library/Android/sdk"; do
  if [ -z "$NDK" ] && [ -n "$sdk" ] && [ -d "$sdk/ndk" ]; then
    latest=$(ls "$sdk/ndk" | sort -V | tail -n 1)
    [ -n "$latest" ] && NDK="$sdk/ndk/$latest"
  fi
  # CMake and Ninja from the SDK when they are not installed.
  if [ -n "$sdk" ] && [ -d "$sdk/cmake" ] && ! command -v ninja >/dev/null 2>&1; then
    PATH="$sdk/cmake/$(ls "$sdk/cmake" | sort -V | tail -n 1)/bin:$PATH"
  fi
done
if [ -z "$NDK" ] || [ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
  echo "Android NDK not found: install it (Android Studio > SDK Manager > SDK Tools > NDK) or set ANDROID_NDK_HOME." >&2
  exit 1
fi
echo "NDK: $NDK"
for ABI in $ABIS; do
  cmake -S . -B "build-android/$ABI" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG" -DOE_BUILD_TESTS=OFF \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static
  cmake --build "build-android/$ABI" --target oe_player
  mkdir -p "build/bin/android/$ABI" "runtime/android/$ABI"
  cp "build-android/$ABI/bin/liboe_player.so" "build/bin/android/$ABI/"
  # Refresh the prebuilt runtime that `oe package --android` falls back to (commit it).
  cp "build-android/$ABI/bin/liboe_player.so" "runtime/android/$ABI/"
done
echo "Built: build/bin/android/{$(echo $ABIS | tr ' ' ',')}/liboe_player.so (used by oe package --android)"

#!/usr/bin/env bash
# Configure the ReXGlue SDK for Android arm64 with the NDK toolchain.
# Usage (Git Bash, from the SDK root): source /c/lr/tools/env.sh && scripts/configure_android.sh
set -euo pipefail
cd "$(dirname "$0")/.."
NDK="${ANDROID_NDK_ROOT_CMAKE:-C:/dev/android-sdk/ndk/28.2.13676358}"
cmake -S . -B out/build/android-arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_BUILD_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX=C:/dev/rexglue-android "$@"

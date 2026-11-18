#!/usr/bin/env bash
# Host (Windows x64) build of the rexglue codegen CLI from this repo's sdk/ (clang GNU driver + xwin, lld).
# Output: sdk/out/win-amd64/Release (never installed over the LR port's C:\dev\rexglue).
set -euo pipefail
source "$(dirname "$0")/env.sh"
cd "$DS2_ROOT/sdk"
LOG=$DS2_ROOT/out/logs/codegen_host_build.log
mkdir -p "$DS2_ROOT/out/logs"
{
  if [ ! -f out/build/win-amd64/CMakeCache.txt ]; then
    cmake --preset win-amd64 \
      -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld \
      -DCMAKE_MODULE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_RC_COMPILER=llvm-rc \
      -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_TRACY=OFF -DCMAKE_INSTALL_PREFIX=C:/ds2/out/rexglue-host
  fi
  cmake --build out/build/win-amd64 --config Release --target rexglue --parallel "${JOBS:-6}"
  echo "BUILD OK"
} > "$LOG" 2>&1 || { echo "FAILED, see $LOG"; tail -20 "$LOG"; exit 1; }
find out -name rexglue.exe -not -path "*android*"

#!/usr/bin/env bash
# Build the Android arm64 native libraries (NDK CMake over app/, which builds the in-tree SDK too), stage them into
# the Gradle project's jniLibs, assemble the APK, and optionally install + launch on the device.
#
#   tools/android/build_android.sh [configure] [build] [stage] [apk] [install] [launch] [logs] [all]
#   no step args / "all" = configure build stage apk
#
# Env: DS2_GENERATED_DIR (default app/generated/regs), DS2_BUILD_TYPE (Release), JOBS (6),
#      DS2_ADB_SERIAL, DS2_RUN_SECS (120), DS2_SCREENSHOTS="20 45" (seconds), DS2_DEVICE_OK=1 (the user handed the
#      device over while its screen is on), DS2_CVARS="--name=value ..." (extra cvars for launch).
# Device rules (owner's, from incidents on the LR port): before install/launch, abort if any game app is running or
# the screen is awake (unless DS2_DEVICE_OK=1); every device step ends with force-stop + KEYCODE_SLEEP, also on failure.
set -euo pipefail
source "$(dirname "$0")/env.sh"
unset CC CXX RC CMAKE_PREFIX_PATH   # host compiler settings must not leak into the NDK configure

REPO=$(cygpath -m "$DS2_ROOT")
GEN=${DS2_GENERATED_DIR:-$REPO/app/generated/regs}
BUILD=${DS2_BUILD_DIR:-$REPO/out/build/android-arm64}
BUILD_TYPE=${DS2_BUILD_TYPE:-Release}
JOBS=${JOBS:-6}
JNI_DIR=$REPO/android/app/src/main/jniLibs/arm64-v8a
APK=$REPO/android/app/build/outputs/apk/debug/app-debug.apk
PKG=com.ds2recomp.android
OTHER_GAME_PKGS="com.lrrecomp.android com.ducvd89.bloodborne com.ds3recomp.android"   # other recomps on the same device (LR; DS3, moving from com.ducvd89.bloodborne to com.ds3recomp.android): never kill a live session
ACTIVITY=${DS2_ACTIVITY:-.GpuDriverActivity}
LOGS=$REPO/out/logs
DEVICE_LOGS=$REPO/out/device
HOOK_TARGETS=(hook_impl main_hook file_redirect_hook gsl_alloc_hook)
NDK=$(cygpath -m "$ANDROID_NDK_HOME")
NDK_BIN=$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin
LIBCXX=$NDK/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so

die() { echo "build_android: ERROR: $*" >&2; exit 1; }
step() { echo; echo "=== $* ==="; }
steps=()
for a in "$@"; do
  case "$a" in
    configure|build|stage|apk|install|launch|logs) steps+=("$a") ;;
    all) steps+=(configure build stage apk) ;;
    *) die "unknown step $a" ;;
  esac
done
[ ${#steps[@]} -eq 0 ] && steps=(configure build stage apk)
has() { local s; for s in "${steps[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }
is_aarch64() { [ "$(od -An -t x1 -j 18 -N 2 "$1" | tr -d ' \n')" = "b700" ]; }
mkdir -p "$LOGS"

if has configure; then
  step configure
  # Clang PGO: default = the merged profile out/pgo/ds2.profdata when present (git-ignored: derived from the game);
  # DS2_PGO= (empty) builds without, DS2_PGO=generate builds the instrumented library (app/CMakeLists.txt).
  PGO_DEFAULT=""
  [ -f "$REPO/out/pgo/ds2.profdata" ] && PGO_DEFAULT=$(cygpath -m "$REPO/out/pgo/ds2.profdata")
  PGO=${DS2_PGO-$PGO_DEFAULT}
  echo "PGO: ${PGO:-off}"
  # The GPU plugin's profile (DS2_PGO_SDK: generate | a .profdata; default out/pgo/rexgpu.profdata when present).
  PGO_SDK_DEFAULT=""
  [ -f "$REPO/out/pgo/rexgpu.profdata" ] && PGO_SDK_DEFAULT=$(cygpath -m "$REPO/out/pgo/rexgpu.profdata")
  PGO_SDK=${DS2_PGO_SDK-$PGO_SDK_DEFAULT}
  echo "PGO (GPU plugin): ${PGO_SDK:-off}"
  [ -f "$GEN/sources.cmake" ] || die "$GEN/sources.cmake missing: run tools/android/codegen.sh first"
  cmake -S "$REPO/app" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_shared \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_TRACY=OFF \
    -DREXGLUE_BUILD_TESTS=OFF -DDS2_GENERATED_DIR="$GEN" -DDS2_PGO="$PGO" -DDS2_PGO_SDK="$PGO_SDK" \
    > "$LOGS/android_configure.log" 2>&1 \
    || { grep -n -i -A4 "CMake Error" "$LOGS/android_configure.log" | head -30 || true; die "configure failed ($LOGS/android_configure.log)"; }
  echo "configure OK"
fi

if has build; then
  step build
  [ -f "$BUILD/build.ninja" ] || die "not configured"
  # The GPU plugin, SDL and the adrenotools hook libraries are not link dependencies of libmain.so: name them.
  if ! cmake --build "$BUILD" --target ds2 rexgpu-xenos SDL3-shared "${HOOK_TARGETS[@]}" --parallel "$JOBS" \
      > "$LOGS/android_build.log" 2>&1; then
    grep -n -E "error:|FAILED:|undefined (symbol|reference)" "$LOGS/android_build.log" | head -30 || true
    die "build failed ($LOGS/android_build.log)"
  fi
  echo "build OK"
fi

find_one() {  # name, dir: first match (head may SIGPIPE find under pipefail)
  local f; f=$(find "$2" -name "$1" -type f 2>/dev/null | head -1) || true
  [ -n "$f" ] && echo "$f"
}
if has stage; then
  step stage
  rm -rf "$JNI_DIR" && mkdir -p "$JNI_DIR"
  declare -A SRC
  for lib in libmain.so libSDL3.so librexruntime.so librexgpu-xenos.so; do
    # The SDK puts its libraries (and the GPU plugin) in sdk/out/android-arm64; libmain.so stays in the build tree.
    SRC[$lib]=$(find_one "$lib" "$REPO/sdk/out/android-arm64" || find_one "$lib" "$BUILD") || die "$lib not found"
  done
  SRC[libc++_shared.so]=$LIBCXX
  for t in "${HOOK_TARGETS[@]}"; do
    SRC[lib$t.so]=$(find_one "lib$t.so" "$BUILD/libadrenotools") || die "lib$t.so not built"
  done
  for lib in "${!SRC[@]}"; do
    src=${SRC[$lib]}
    is_aarch64 "$src" || die "$src is not an AArch64 ELF"
    "$NDK_BIN/llvm-strip.exe" --strip-unneeded -o "$JNI_DIR/$lib" "$src" || die "llvm-strip failed on $src"
    printf '  %-24s %11s bytes  <- %s\n' "$lib" "$(stat -c %s "$JNI_DIR/$lib")" "$src"
  done
  # Bundled GPU driver (GpuDriverStore): Stable Turnip v2 (Mesa, MIT), pinned by checksum, as in the LR port.
  TURNIP_URL=https://github.com/whitebelyash/AdrenoToolsDrivers/releases/download/stu_v2/stable-turnip-V2.zip
  TURNIP_SHA256=426846964f9b9c3f182b9298a805e85c1f062a590f622cf4ddc9b0b5934a581e
  TURNIP_ZIP=$REPO/out/drivers/stable-turnip-V2.zip
  if [ ! -f "$TURNIP_ZIP" ]; then
    mkdir -p "$(dirname "$TURNIP_ZIP")"
    if [ -f /c/lr/out/drivers/stable_v2/stable-turnip-V2.zip ]; then cp /c/lr/out/drivers/stable_v2/stable-turnip-V2.zip "$TURNIP_ZIP"
    else curl -fsSL -o "$TURNIP_ZIP" "$TURNIP_URL" || die "could not download $TURNIP_URL"; fi
  fi
  [ "$(sha256sum "$TURNIP_ZIP" | cut -d' ' -f1)" = "$TURNIP_SHA256" ] || die "$TURNIP_ZIP: checksum mismatch"
  mkdir -p "$REPO/android/app/src/main/assets/gpu_driver"
  cp -f "$TURNIP_ZIP" "$REPO/android/app/src/main/assets/gpu_driver/stable-turnip-V2.zip"
  # The Adreno 640 / 650 / 660 / 720 default (GpuDriverStore.BUNDLED_R3_ASSET): StevenMXZ's Turnip v27.0.0-R3 (Mesa, MIT).
  R3_URL=https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases/download/v40/Turnip_v27.0.0-R3.zip
  R3_SHA256=3a3da776b614550fe8822363bfe7bacca8bc22a5e743d221c196333a54a07add
  R3_ZIP=$REPO/out/drivers/r3/Turnip_v27.0.0-R3.zip
  if [ ! -f "$R3_ZIP" ]; then
    mkdir -p "$(dirname "$R3_ZIP")"
    curl -fsSL -o "$R3_ZIP" "$R3_URL" || die "could not download $R3_URL"
  fi
  [ "$(sha256sum "$R3_ZIP" | cut -d' ' -f1)" = "$R3_SHA256" ] || die "$R3_ZIP: checksum mismatch"
  cp -f "$R3_ZIP" "$REPO/android/app/src/main/assets/gpu_driver/turnip-v27.0.0-R3.zip"
  # The second screen's art (tools/android/ds2_companion_assets.py, cut from the owner's own discs): development APKs
  # only. build_release_apk.sh removes it again, and its asset check refuses an APK that carries it.
  COMPANION_ART=$REPO/out/companion_assets/companion
  rm -rf "$REPO/android/app/src/main/assets/companion"
  # DS2_NO_COMPANION_ART=1: leave it out, as a public APK does (the app then builds it from the game files).
  if [ -f "$COMPANION_ART/hud.json" ] && [ "${DS2_NO_COMPANION_ART:-0}" != 1 ]; then
    cp -r "$COMPANION_ART" "$REPO/android/app/src/main/assets/companion"
    echo "  companion art: $(du -sh "$COMPANION_ART" | cut -f1)"
  else
    echo "  companion art: none (out/venv/Scripts/python.exe -I tools/android/ds2_companion_assets.py)"
  fi
  echo "stage OK -> $JNI_DIR"
fi

if has apk; then
  step apk
  rm -f "$APK"
  ( cd "$REPO/android" && JAVA_HOME='C:\dev\jdk17' ./gradlew --no-daemon --console=plain :app:assembleDebug ) \
    > "$LOGS/android_gradle.log" 2>&1 \
    || { grep -n -E "FAILURE|error|What went wrong" -A3 "$LOGS/android_gradle.log" | head -30 || true; die "gradle failed ($LOGS/android_gradle.log)"; }
  [ -f "$APK" ] || die "APK not produced"
  stale=$(find "$JNI_DIR" -type f -newer "$APK" 2>/dev/null | head -1) || true
  [ -z "$stale" ] || die "APK is older than staged $stale"
  echo "APK: $APK ($(stat -c %s "$APK") bytes)"
fi

ADB=(adb ${DS2_ADB_SERIAL:+-s "$DS2_ADB_SERIAL"})
# Only after a guard passed: a refused step must not touch the device at all (a refusal once sent KEYCODE_SLEEP
# under another session's running LR test, 2026-11-18).
DEVICE_TOUCHED=0
device_sleep() {
  [ "$DEVICE_TOUCHED" = 1 ] || return 0
  "${ADB[@]}" shell am force-stop "$PKG" >/dev/null 2>&1 || true; "${ADB[@]}" shell input keyevent KEYCODE_SLEEP >/dev/null 2>&1 || true
}
device_guard() {  # right before each install/launch, never minutes earlier
  local p
  for p in $PKG $OTHER_GAME_PKGS; do
    if "${ADB[@]}" shell pidof "$p" >/dev/null 2>&1; then die "$p is running on the device: not touching it"; fi
  done
  if "${ADB[@]}" shell dumpsys power | grep -q "mWakefulness=Awake" && [ "${DS2_DEVICE_OK:-0}" != 1 ]; then
    die "device screen is on (someone may be using it); set DS2_DEVICE_OK=1 once it is handed over"
  fi
  DEVICE_TOUCHED=1
}
if has install || has launch || has logs; then
  command -v adb >/dev/null || die "adb not on PATH"
  "${ADB[@]}" get-state >/dev/null 2>&1 || die "no device (plug it in, accept USB debugging)"
  on_exit() { local rc=$?; trap - EXIT; device_sleep; exit "$rc"; }
  trap on_exit EXIT
fi
if has install; then
  step install
  [ -f "$APK" ] || die "no APK"
  device_guard
  "${ADB[@]}" install -r "$APK" || die "adb install failed"
fi
if has launch; then
  step launch
  device_guard
  mkdir -p "$DEVICE_LOGS"
  stamp=$(date +%Y%m%d-%H%M%S)
  "${ADB[@]}" logcat -c >/dev/null 2>&1 || true
  "${ADB[@]}" shell input keyevent KEYCODE_WAKEUP
  "${ADB[@]}" shell wm dismiss-keyguard >/dev/null 2>&1 || true
  # DS2_CVARS="--a=1 --b=2": extra runtime cvars for this launch (debug builds only, RuntimeActivity's "cvars" extra).
  am_args=(am start -n "$PKG/$ACTIVITY")
  [ -n "${DS2_CVARS:-}" ] && am_args+=(--es cvars "'$DS2_CVARS'")
  # DS2_AM_EXTRAS="--es DS2_GPU_DRIVER system --es TU_DEBUG sysmem": more launch extras (debug builds; DS2_/REX_/TU_/MESA_
  # extras become environment variables).
  [ -n "${DS2_AM_EXTRAS:-}" ] && am_args+=($DS2_AM_EXTRAS)
  am_out=$("${ADB[@]}" shell "${am_args[@]}" 2>&1) || { echo "$am_out"; die "am start failed"; }
  ! grep -q -E "^Error" <<<"$am_out" || die "am start: $am_out"
  RUN_SECS=${DS2_RUN_SECS:-120}
  # Dual-screen devices: the other physical displays (the second screen's HUD, CompanionScreen) are captured with every
  # screenshot as shot_<stamp>_<t>s_d<n>.png (n = 2, 3, ...).
  SECOND_DISPLAYS=$("${ADB[@]}" shell dumpsys SurfaceFlinger --display-id </dev/null 2>/dev/null | tr -d '\r' \
                    | sed -n 's/^Display \([0-9]*\) (HWC display [1-9][0-9]*).*/\1/p' || true)
  echo "running for ${RUN_SECS}s ..."
  elapsed=0
  rm -f "$DEVICE_LOGS/STOP_RUN"
  for t in ${DS2_SCREENSHOTS:-}; do
    # One second at a time, so STOP_RUN also ends the wait for a late screenshot.
    while [ "$elapsed" -lt "$t" ] && [ ! -e "$DEVICE_LOGS/STOP_RUN" ]; do sleep 1; elapsed=$((elapsed + 1)); done
    [ -e "$DEVICE_LOGS/STOP_RUN" ] && break
    "${ADB[@]}" exec-out screencap -p > "$DEVICE_LOGS/shot_${stamp}_${t}s.png" 2>/dev/null </dev/null || true
    echo "screenshot ${t}s: $DEVICE_LOGS/shot_${stamp}_${t}s.png"
    n=2
    for id in $SECOND_DISPLAYS; do
      "${ADB[@]}" exec-out screencap -p -d "$id" > "$DEVICE_LOGS/shot_${stamp}_${t}s_d$n.png" 2>/dev/null </dev/null || true
      n=$((n + 1))
    done
  done
  # Live sessions end early when out/device/STOP_RUN appears (touch it; it is removed here): the normal end follows
  # (logs, force-stop, sleep; ab_run.sh then keeps the end save and restores the owner's).
  while [ "$elapsed" -lt "$RUN_SECS" ] && [ ! -e "$DEVICE_LOGS/STOP_RUN" ]; do sleep 1; elapsed=$((elapsed + 1)); done
  [ -e "$DEVICE_LOGS/STOP_RUN" ] && { rm -f "$DEVICE_LOGS/STOP_RUN"; echo "stopped early at ~${elapsed}s (STOP_RUN)"; }
  "${ADB[@]}" logcat -d > "$DEVICE_LOGS/logcat_$stamp.txt" 2>/dev/null || true
  # Only the log files this launch wrote (the device keeps ~20 rotated 5 MB logs; pulling them all every run filled
  # out/device with 15 GB of duplicates on 2026-11-18).
  mkdir -p "$DEVICE_LOGS/logs_$stamp"
  for f in $("${ADB[@]}" shell "find '/sdcard/Android/data/$PKG/files/logs' -type f -mmin -$(( elapsed / 60 + 5 ))" \
             </dev/null 2>/dev/null | tr -d '\r'); do
    "${ADB[@]}" pull "$f" "$DEVICE_LOGS/logs_$stamp/" </dev/null >/dev/null 2>&1 || true
  done
  device_sleep
  echo "logs: $DEVICE_LOGS/logcat_$stamp.txt, $DEVICE_LOGS/logs_$stamp/ (device force-stopped and asleep)"
fi
if has logs; then
  step logs
  mkdir -p "$DEVICE_LOGS"
  stamp=$(date +%Y%m%d-%H%M%S)
  "${ADB[@]}" logcat -d > "$DEVICE_LOGS/logcat_$stamp.txt" 2>/dev/null || true
  "${ADB[@]}" pull "/sdcard/Android/data/$PKG/files/logs" "$DEVICE_LOGS/logs_$stamp" >/dev/null 2>&1 || true
  echo "logs: $DEVICE_LOGS/logcat_$stamp.txt"
fi

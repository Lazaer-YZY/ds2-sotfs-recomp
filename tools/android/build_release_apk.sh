#!/usr/bin/env bash
# Builds a release-style APK for a player's device (ported from the Lightning Returns port's tools/build_release_apk.sh).
#
#   bash tools/android/build_release_apk.sh VERSION [--skip-native] [--test] [--pull-seed]
#
# VERSION is x.y.z (versionName; versionCode major*1000000 + minor*10000 + patch). The native libraries come from this
# checkout (tools/android/build_android.sh build stage; --skip-native packages what is already staged). The APK gets
# the public defaults (PUBLIC_DEFAULTS: debug logging off, no save snapshots or other development features) and is
# not debuggable. --test builds com.ds2recomp.android.test instead: it installs beside the normal app with its own
# empty data folder, to try a first start on a device that has the development build.
#
# Output: out/release/ds2-sotfs-android-arm64[-test].apk (+ .sha256). Signed with this PC's Android debug keystore
# (DS2_ANDROID_DEBUG_KEYSTORE overrides): an update installs over a previous one only with the same signature.
#
# Launcher icon: out/branding (tools/android/make_icon.py on the logo; git-ignored, like all game artwork); the script
# fails without it.
#
# It carries a shader cache seed (out/shader_seed: the shaders and pipeline descriptions collected by playing on the
# development device; --pull-seed copies them from the device), so a fresh install compiles every shader at the
# title instead of stuttering on first sight. The driver's own cache is never bundled (one GPU and driver build).
#
# The APK carries code compiled from the game's executable and the shader cache made from the game, never the game
# files themselves: the script fails on any other asset, and on the builder's identifiers or user paths in the
# libraries and the dex (patterns in out/personal_patterns.txt, git-ignored).
# shellcheck disable=SC1091
set -euo pipefail
export MSYS_NO_PATHCONV=1

VERSION=""; NATIVE=1; TEST=0; PULL_SEED=0
for a in "$@"; do
  case "$a" in
    --skip-native) NATIVE=0 ;;
    --pull-seed) PULL_SEED=1 ;;
    --test) TEST=1 ;;
    -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
    -*) echo "unknown option $a" >&2; exit 2 ;;
    *) VERSION=${a#v} ;;
  esac
done
die() { echo "build_release_apk: ERROR: $*" >&2; exit 1; }
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "usage: build_release_apk.sh VERSION (x.y.z) [--skip-native] [--test] [--pull-seed]"

REPO=C:/ds2
OUT=$REPO/out/release
LOG=$REPO/out/logs/android_release.log
PAT=$REPO/out/personal_patterns.txt
mkdir -p "$OUT" "$REPO/out/logs"
cd /c/ds2
set +u; source tools/android/env.sh >/dev/null; set -u
export MSYS_NO_PATHCONV=1
[ -s "$PAT" ] || die "no personal patterns in $PAT"
[ -f out/branding/res/mipmap-anydpi-v26/ic_launcher_ds2.xml ] \
  || die "no launcher icon in out/branding (python -I tools/android/make_icon.py LOGO.png)"
if [ "$TEST" = 0 ] && ! git diff --quiet HEAD -- . ; then
  die "uncommitted changes: a release APK is built from a commit"
fi

if [ "$NATIVE" = 1 ]; then
  echo "== native libraries (tools/android/build_android.sh build stage)"
  bash tools/android/build_android.sh build stage
fi

SEED_FILES="465307E4.xsh 465307E4.fbo.vk.xpso"
SEED_SRC=$REPO/out/shader_seed
SEED_DIR=$REPO/android/app/src/main/assets/shader_seed
if [ "$PULL_SEED" = 1 ]; then
  mkdir -p "$SEED_SRC"
  for f in $SEED_FILES; do
    adb ${DS2_ADB_SERIAL:+-s "$DS2_ADB_SERIAL"} pull \
      "/sdcard/Android/data/com.ds2recomp.android/files/cache/shaders/shareable/$f" "$SEED_SRC/$f" >/dev/null \
      || die "could not pull $f from the device"
  done
fi
for f in $SEED_FILES; do [ -s "$SEED_SRC/$f" ] || die "shader seed $SEED_SRC/$f missing (run with --pull-seed)"; done
rm -rf "$SEED_DIR"; mkdir -p "$SEED_DIR"
for f in $SEED_FILES; do cp -f "$SEED_SRC/$f" "$SEED_DIR/$f"; done
echo "$(sha256sum "$SEED_DIR/465307E4.xsh" | cut -c1-16) $(stat -c %s "$SEED_DIR/465307E4.xsh")" > "$SEED_DIR/stamp.txt"
echo "== shader seed: $(cat "$SEED_DIR/stamp.txt") (sha prefix, bytes of shaders)"
# The seed is a release asset only: development builds start from the device's own cache.
cleanup_seed() { rm -rf "$SEED_DIR"; }
trap cleanup_seed EXIT

SUFFIX=""; NAME=ds2-sotfs-android-arm64
[ "$TEST" = 1 ] && { SUFFIX=.test; NAME=$NAME-test; }
echo "== APK $VERSION${SUFFIX:+ ($SUFFIX)} (log: $LOG)"
( cd android && JAVA_HOME='C:\dev\jdk17' ./gradlew --no-daemon --console=plain :app:assembleRelease \
    -Pds2PublicDefaults=true -Pds2VersionName="$VERSION" \
    ${SUFFIX:+-Pds2AppIdSuffix=$SUFFIX} ) > "$LOG" 2>&1 || { tail -30 "$LOG"; die "gradle failed, see $LOG"; }
SRC_APK=$REPO/android/app/build/outputs/apk/release/app-release.apk
[ -f "$SRC_APK" ] || die "no $SRC_APK"
APK=$OUT/$NAME.apk
cp -f "$SRC_APK" "$APK"

echo "== checks"
BT=$ANDROID_HOME/build-tools/35.0.0
assets=$(unzip -Z1 "$APK" | grep '^assets/' || true)
extra=$(grep -vE '^assets/(gpu_driver|shader_seed|dexopt)/' <<<"$assets" || true)
[ -z "$extra" ] || die "assets that must not ship:
$extra"
for need in gpu_driver/stable-turnip-V2.zip shader_seed/465307E4.xsh shader_seed/465307E4.fbo.vk.xpso shader_seed/stamp.txt; do
  grep -qx "assets/$need" <<<"$assets" || die "assets/$need missing"
done
badging=$("$BT/aapt2.exe" dump badging "$APK")
grep -q "name='com.ds2recomp.android$SUFFIX' versionCode='[0-9]*' versionName='$VERSION'" <<<"$badging" \
  || die "unexpected package/version: $(head -1 <<<"$badging")"
! grep -q "application-debuggable" <<<"$badging" || die "APK is debuggable"
icon_id=$("$BT/aapt2.exe" dump resources "$APK" | sed -n 's|.*resource \(0x[0-9a-f]*\) mipmap/ic_launcher_ds2\b.*|\1|p' | head -1)
[ -n "$icon_id" ] && "$BT/aapt2.exe" dump xmltree --file AndroidManifest.xml "$APK" | grep -q "android:icon([^)]*)=@$icon_id" \
  || die "the launcher icon is not out/branding's (mipmap/ic_launcher_ds2)"
TMP=$OUT/.check   # a C:/ path: llvm-strings is a Windows program and cannot open /tmp
rm -rf "$TMP"; mkdir -p "$TMP"; trap 'rm -rf "$TMP"; cleanup_seed' EXIT
unzip -q -o "$APK" -d "$TMP"
STRINGS=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strings.exe
: > "$TMP/strings.txt"
for f in "$TMP"/lib/*/*.so "$TMP"/classes*.dex "$TMP"/AndroidManifest.xml; do
  "$STRINGS" -n 6 "$f" >> "$TMP/strings.txt" || die "llvm-strings could not read $f"
done
[ "$(wc -c < "$TMP/strings.txt")" -gt 1000000 ] || die "too few strings read from the APK"
hits=$(LC_ALL=C grep -E -f "$PAT" "$TMP/strings.txt" | grep -v "Tom Clay\|Tom Seddon\|Tom's lossless Audio" | cut -c1-160 | sort -u || true)
users=$(LC_ALL=C grep -oiE '[A-Z]:[/\\]Users[/\\][^/\\"]+|/home/[a-z][a-z0-9_-]+/' "$TMP/strings.txt" | sort -u | head || true)
[ -z "$hits$users" ] || die "personal identifiers or user paths in the APK:
$hits
$users"
"$BT/apksigner.bat" verify --print-certs "$APK" | grep -m1 "certificate SHA-256 digest" || die "apksigner verify failed"

(cd "$OUT" && sha256sum "$NAME.apk" > "$NAME.apk.sha256")
echo "== $APK ($(du -m "$APK" | cut -f1) MB)"
cat "$OUT/$NAME.apk.sha256"
unzip -l "$APK" | awk '/lib\/arm64-v8a\/|assets\// { printf "   %9d  %s\n", $1, $4 }'

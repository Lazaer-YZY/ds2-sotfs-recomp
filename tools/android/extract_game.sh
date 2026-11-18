#!/usr/bin/env bash
# Extract the user's own SotFS discs for the port.
#   tools/android/extract_game.sh <disc1 .iso or .7z> [<disc2 .iso or .7z>]
# Disc 1 -> assets/game_root (the layout DS2_switch's manifests expect, git-ignored).
# Disc 2 -> assets/disc2 (installer; holds Compatibility Pack 4 = the DLC areas).
# Each step skips when its output exists; delete the folder to redo it.
set -euo pipefail
source "$(dirname "$0")/env.sh"
SEVENZIP="/c/Program Files/7-Zip/7z.exe"
WORK=$DS2_ROOT/assets
mkdir -p "$WORK/iso"

to_iso() {  # <.iso or .7z> -> prints the .iso path
  local src=$1
  case "$src" in
    *.iso|*.ISO) echo "$src" ;;
    *.7z)
      local listed
      listed=$("$SEVENZIP" l -slt "$(cygpath -w "$src")" | sed -n 's/^Path = \(.*\.iso\)$/\1/Ip' | head -1)
      [ -n "$listed" ] || { echo "no .iso inside $src" >&2; exit 1; }
      if [ ! -f "$WORK/iso/$listed" ]; then
        "$SEVENZIP" x -y -o"$(cygpath -w "$WORK/iso")" "$(cygpath -w "$src")" "$listed" >&2
      fi
      echo "$WORK/iso/$listed" ;;
    *) echo "unsupported input: $src" >&2; exit 1 ;;
  esac
}

extract() {  # <iso> <out dir>
  local iso=$1 out=$2
  if [ -d "$out" ] && [ -n "$(ls -A "$out" 2>/dev/null)" ]; then
    echo "skip: $out exists"
    return
  fi
  mkdir -p "$out"
  python -I "$(cygpath -m "$DS2_ROOT/tools/android/xdvdfs.py")" "$(cygpath -m "$iso")" extract "$(cygpath -m "$out")"
}

DISC1=$(to_iso "$1")
extract "$DISC1" "$WORK/game_root"
XEX=$(ls "$WORK/game_root" | grep -i '^default\.xex$' | head -1)
python -I "$(cygpath -m "$DS2_ROOT/tools/ds2_xexinfo.py")" "$(cygpath -m "$WORK/game_root/$XEX")"
sha256sum "$WORK/game_root/$XEX"

if [ $# -ge 2 ]; then
  DISC2=$(to_iso "$2")
  extract "$DISC2" "$WORK/disc2"
  echo "disc 2 contents:"
  (cd "$WORK/disc2" && find . -type f -printf '%10s %p\n' | sort -k2 | head -40)
fi

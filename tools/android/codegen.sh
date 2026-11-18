#!/usr/bin/env bash
# rexglue codegen for the Android build.
#   tools/android/codegen.sh [manifest]     (default app/ds2_manifest.regs.toml -> app/generated/regs; register-localised)
# First-boot flavour: DS2_switch's validated manifest and override files, plain codegen (registers in the context,
# guest memory volatile) with the real sync/lwsync/eieio fences our SDK emits. Register localisation and the rc97
# steps come later, measured (DECISIONS.md). Never run while a build is compiling the generated tree.
set -euo pipefail
source "$(dirname "$0")/env.sh"
MANIFEST=${1:-app/ds2_manifest.regs.toml}
REXGLUE=$(find "$DS2_ROOT/sdk/out" -name rexglue.exe -not -path "*android*" | head -1)
[ -n "$REXGLUE" ] || { echo "rexglue.exe not built: tools/android/build_codegen_host.sh"; exit 1; }
# The CLI loads the runtime DLLs it was linked against from the build tree.
export PATH="$(dirname "$REXGLUE"):$(find "$DS2_ROOT/sdk/out" -name 'rexruntime*.dll' -printf '%h' -quit):$PATH"
cd "$DS2_ROOT"
python -I -c "import tomllib,sys;tomllib.load(open(sys.argv[1],'rb'))" "$MANIFEST"
LOG=out/logs/codegen_$(date +%Y%m%d_%H%M%S).log
start=$(date +%s)
"$REXGLUE" --log-level info codegen "$MANIFEST" > "$LOG" 2>&1 || { echo "codegen FAILED ($LOG)"; tail -30 "$LOG"; exit 1; }
echo "codegen OK in $(( $(date +%s) - start )) s ($LOG)"
grep -E "^[A-Za-z]+ \([0-9]+\):" "$LOG" | head -20 || true
OUT=$(python -I -c "import tomllib,sys,os;m=tomllib.load(open(sys.argv[1],'rb'));print(os.path.join(os.path.dirname(sys.argv[1]),m['entrypoint']['out_directory_path']))" "$MANIFEST")
python -I tools/build/patch-generated.py "$OUT"
echo "generated: $OUT ($(ls "$OUT" | wc -l) files, $(du -sh "$OUT" | cut -f1))"

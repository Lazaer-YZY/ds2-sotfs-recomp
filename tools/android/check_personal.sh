#!/bin/bash
# Fails when a tracked file (or the given export directory) contains a personal identifier. Patterns: one extended
# regex per line in out/personal_patterns.txt (git-ignored, so the patterns themselves never get published).
# Third-party credits that happen to match (SDK and Mesa authors) are allowed.
# Usage: bash tools/android/check_personal.sh [DIR]   (default: the tracked files of this repo)
set -u
PAT=/c/ds2/out/personal_patterns.txt
ALLOW='Tom Clay|Tom Seddon|Tom.s lossless|robdclark|tomcl7'
[ -s "$PAT" ] || { echo "no patterns in $PAT"; exit 2; }
if [ -n "${1:-}" ]; then
  hits=$(grep -rnIE -f "$PAT" --exclude=stb_image.h --exclude-dir=.git "$1" | grep -vE "$ALLOW")
else
  hits=$(git -C /c/ds2 grep -nIE -f "$PAT" -- . | grep -vE "$ALLOW")
fi
if [ -n "$hits" ]; then echo "$hits" | cut -c1-200 | head -40; echo "personal identifiers found: $(echo "$hits" | wc -l) lines"; exit 1; fi
echo "clean: no personal identifiers"

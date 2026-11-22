# Toolchain environment for the DS2 Android port (Git Bash). Shared toolchain in C:\dev (see CLAUDE.md).
# Same layout as the Lightning Returns port's tools/env.sh; sourced by every script in tools/android/.
source /c/lr/tools/env.sh
# The checkout these scripts belong to, through its space-free junction (C:\ds2; a worktree has its own, e.g.
# C:\ds2hud). Paths with spaces break the NDK, Gradle and CMake: run from a junction, else C:\ds2 is used.
_ds2_here=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
case "$_ds2_here" in *" "*) _ds2_here=/c/ds2 ;; esac
export DS2_ROOT=${DS2_ROOT:-$_ds2_here}
export MSYS_NO_PATHCONV=1

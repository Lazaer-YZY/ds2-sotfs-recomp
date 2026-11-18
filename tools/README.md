# Tools

Scripts used to build, generate and analyse the port. Several names come from DS2_switch and are Spanish
("huecos" = gaps, "llamadas" = calls).

## Android build (the deliverable)

| Path | What it does |
|---|---|
| `android/env.sh` | Toolchain environment (shared `C:\dev`), sourced by every script below. |
| `android/extract_game.sh` | Discs (.7z or .iso) to `assets/game_root` and `assets/disc2`; XEX identity and SHA-256. |
| `android/xdvdfs.py` | Xbox 360 disc filesystem reader used by the extraction. |
| `android/build_codegen_host.sh` | Windows build of the `rexglue` codegen CLI from `sdk/`. |
| `android/codegen.sh` | Codegen with `app/ds2_manifest.diag-dataonly.toml`, then `build/patch-generated.py`. |
| `android/build_android.sh` | NDK build, stage into jniLibs, APK, install/launch/logs with device guards. |
| `android/push_game_data.sh` | Resumable copy of the game to the app's folder on the device. |
| `android/ab_run.sh` | A/B runs from the newest save: per variant (extra cvars) boot, title dialogs, Continue, screenshots, log and fps. |
| `fetch_thirdparty.py` | Fills `sdk/thirdparty` from the ReXGlue release's submodules (alternative to copying the LR checkout). |
| `build/patch-generated.py` | Brings generated code up to the current generator output without a full codegen. |
| `build/build-pc.sh`, `build/env.sh`, `run/` | Linux PC build and run (debug harness, from DS2_switch; unused so far). |

## Code generation analysis (from DS2_switch)

| Path | What it does |
|---|---|
| `run_codegen_diag.sh`, `run_codegen_monitored.sh` | Codegen runs with logs, time and memory (Linux). |
| `rc97/` | The optimised codegen experiment (registers as locals, `share_registers`, direct calls, no `volatile`). Not used: see DECISIONS.md before reviving it; paths in it are hard-coded. |
| `huecos.py`, `huecos_excluir.txt` | Finds code the generator left outside any function; `config/ds2_overrides.gaps.toml` came from it. |
| `ds2_find_dataonly_entries.py`, `ds2_find_split_culprits.py`, `ds2_analyze_unresolved.py`, `ds2_classify_unresolved.py` | Entries referenced only from data or code pointers, unresolved branches (`config/ds2_overrides.*.toml`). |
| `ds2_trace_*.py` | Codegen trace analysis: why GapFill did not split at a target and what happened to the segment. |
| `ds2_imagedump.py` | Decrypts and unpacks the XEX's PE image (what the code generator analyses). |
| `ds2_xexinfo.py`, `ds2_make_elf.py` | XEX header information; the game image wrapped in a PowerPC ELF for disassemblers. |
| `analysis/` | BND4 archives (`bnd4.py`), the 16-bit jump table check, cvar and log helpers, resolve dumps to PNG, SPIR-V from generated headers. |
| `compile_check/` | Compile check of the generated code without the app (Linux). |

## Shaders

| Path | What it does |
|---|---|
| `shaders/` | Offline SPIR-V pack builder for the translator's `ds2_shader_library` (optional; without a pack shaders are translated at run time). |
| `ds2/gen_direct_resolve_shaders.sh` | Compiles the direct-resolve compute shader into the SPIR-V headers used by the renderer. |

Nothing in this folder contains game data. Game files, generated code and built binaries are never committed.

# Third-party notices

This port is built on the work of other projects. Their licenses apply to their code and to the parts of this
repository derived from it.

## Projects this port is derived from

| Project | Used for | License |
|---|---|---|
| [DS2_switch](https://github.com/XDASCATX/DS2_switch) by xDASCATx | The Dark Souls II Switch port this one started from (git history kept for attribution): `app/` (manifests, DLC installer, hooks), `config/`, codegen and runtime changes in `sdk/`, analysis tools | GPL-3.0 |
| [nfsmw-nx](https://github.com/StevensND/nfsmw-nx) by StevensND and [NFSMW Recompiled](https://github.com/madelrandel-blip/NFSMW-Recompiled) by madelrandel-blip | Where DS2_switch started: parts of `sdk/` and the FFmpeg WMV3 decoder configuration | GPL-3.0 |
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) by Tom Clay, with portions from [Xenia](https://xenia.jp) (Ben Vanik and the Xenia contributors) | `sdk/`: code generator, Xbox 360 kernel and XAM, Vulkan renderer | BSD-3-Clause |
| [Lost Odyssey Recomp](https://github.com/freefrank/LostOdysseyRecomp) (via the Lightning Returns port) | `android/` activities, game folder, GPU driver store, touch controls | GPL-3.0 |
| [libadrenotools](https://github.com/bylaws/libadrenotools) by bylaws | `thirdparty/libadrenotools`: loading Turnip on Adreno | BSD-2-Clause |
| [Mesa](https://mesa3d.org) Turnip ("Stable Turnip v2" build by whitebelyash), bundled as an APK asset, not in git | The Vulkan driver on Adreno | MIT |

## Libraries

Fetched from the submodules of ReXGlue SDK v0.10.1 into `sdk/thirdparty` (not in git except the modified files):

| Library | License |
|---|---|
| [SDL3](https://github.com/libsdl-org/SDL) (window, input, audio, Android activity) | zlib |
| [FFmpeg](https://ffmpeg.org) (libavcodec, libavutil: XMA/WMA Pro audio and the WMV3 movie decoder) | LGPL-2.1-or-later |
| [libmspack](https://www.cabextract.org.uk/libmspack/) by Stuart Caie (LZX decompression) | LGPL-2.1 |
| [glslang](https://github.com/KhronosGroup/glslang) | BSD-3-Clause and others |
| [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) | Apache-2.0 |
| [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers), [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | MIT, Apache-2.0 |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | MIT |
| [fmt](https://github.com/fmtlib/fmt), [spdlog](https://github.com/gabime/spdlog) | MIT |
| [xxHash](https://github.com/Cyan4973/xxHash) | BSD-2-Clause |
| [SIMDe](https://github.com/simd-everywhere/simde) | MIT |
| [toml++](https://github.com/marzer/tomlplusplus) | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT |
| [Cinzel](https://github.com/NDISCOVER/Cinzel) font (titles of the port's own windows, `sdk/src/ui/fonts/`) | SIL Open Font License 1.1 |
| [o1heap](https://github.com/pavel-kirienko/o1heap), [utfcpp](https://github.com/nemtrif/utfcpp), [CLI11](https://github.com/CLIUtils/CLI11), AES-128 by LuoPeng | MIT, BSL-1.0, BSD-3-Clause, MIT |

### FFmpeg and libmspack (LGPL)

Modified files kept in `sdk/thirdparty`: `FFmpeg/libavcodec/fft_template.c`, `FFmpeg/libavcodec/mdct_template.c`,
`FFmpeg/config.h`, `FFmpeg/config_nfsmw_wmv3.h` (new: a build limited to the WMV3 decoder), `ffmpeg-overlay/codec_list.c`
(registers the WMV3 decoder) and `CMakeLists.txt`. The rest is the unmodified upstream source at the commits pinned by the
ReXGlue SDK. Everything needed to rebuild and relink with a modified version is in this repository (`tools/android/`).

## Trademarks

Dark Souls is a trademark of FromSoftware, Inc. / Bandai Namco Entertainment. Xbox 360 is a trademark of Microsoft.
This project is not affiliated with or endorsed by any of them.

# DS2 Recomp - Android app

Single Gradle module `:app` (`com.ds2recomp.android`), label "DS2 Recomp". arm64-v8a only, minSdk 26,
compile/target SDK 35, AGP 8.9.3, Gradle 8.11.1, JDK 17. GPL-3.0. Java/Gradle shell only: the native
libraries are built separately (CMake/NDK) and staged into `app/src/main/jniLibs/arm64-v8a/`.

## Build

    set JAVA_HOME=C:\dev\jdk17
    gradlew.bat :app:assembleDebug -Pds2SkipNativeCheck=true --no-daemon     (Java side only)

Full build: stage `libmain.so libSDL3.so libc++_shared.so librexruntime.so librexgpu-xenos.so`
(optional adrenotools hooks: `libhook_impl.so libmain_hook.so libfile_redirect_hook.so libgsl_alloc_hook.so`)
into jniLibs, then `gradlew.bat :app:assembleDebug`; `checkNativeLibraries` fails the build if a required
library is missing. Properties: `-Pds2VersionName=v0.1.0` (versionCode major*1e6+minor*1e4+patch),
`-Pds2AppIdSuffix=.test`, `-Pds2PublicDefaults=true`, env `DS2_ANDROID_DEBUG_KEYSTORE`.
`local.properties`: `sdk.dir=C:/dev/android-sdk`. Output: `app/build/outputs/apk/debug/app-debug.apk`.

## Layout

`app/src/main/java/com/ds2recomp/android`: RuntimeActivity (SDLActivity), GpuDriverActivity/Catalog/Store
(Turnip/PanVK drivers), GameFolderActivity, GameStorage, IsoImport/IsoImportService/XdvdfsReader (disc ISO
import; title ID 465307E4, `Default.xex` matched case-insensitively), PlayerLogs, BootOverlay, ShaderSeed
(optional `assets/shader_seed`, files named `465307E4.*`), TouchControl* (on-screen controller + CTRL dialog).
`sdl-java.gradle` copies SDL's Java from `../../sdk/thirdparty/sdl3/android-project/app/src/main/java`
(relative to `app/`) into `app/build/generated/sdl-java` and patches HIDDeviceManager (fails if the anchor is missing).

## Native contract

Libraries (load order): `c++_shared`, `SDL3`, `rexruntime`, `rexgpu-xenos`, `main`.
JNI (class `com.ds2recomp.android.RuntimeActivity`, all static): `nativeSetTouchInput(int buttons, int lt, int rt, int lx, int ly, int rx, int ry)`,
`boolean nativeHasConnectedController()`, `int nativeBootProgress()`, `nativeSetOption(String, String)`.
Native may call `void reportGraphicsFailure(String)`; `SDL_SendAndroidMessage(0x8000)` opens the game-folder page.

argv: `--game_data_root=<files>/game` (or the chosen folder), `--user_data_root=<files>/user`,
`--cache_root=<files>/cache`, `--metadata_root=<files>/metadata`, `--log_file=<files>/logs/ds2.log`,
`--gpu_plugin=xenos`, `--present_max_height=720`. `<files>/dlc` is created; native finds it via `REX_ANDROID_APP_DIR`.

Environment (nativeSetenv before native main): `DS2_NATIVE_LIB_DIR`, `DS2_LOG_DIR`, `DS2_ANDROID_DEVICE`,
`DS2_CUSTOM_DRIVER_DIR`, `DS2_VK_CUSTOM_DRIVER`, `DS2_VK_CUSTOM_DRIVER_MODE`, `DS2_VK_ADRENO650_SYNC_DRAW`,
`DS2_DIAG` (CTRL "Debug logging"), `REX_ANDROID_APP_DIR` (external files dir + `/`), `MESA_SHADER_CACHE_DIR/_MAX_SIZE`,
`TU_DEBUG=sysmem` / `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER` as needed, `REX_VULKAN_ALLOW_PRESENT_MODE_{MAILBOX,IMMEDIATE,FIFO_RELAXED}=false`.
Debug builds forward `DS2_`/`REX_`/`TU_`/`MESA_`/`FD_`/`IR3_` intent extras as env vars, the `cvars` extra as extra argv,
and honour a `game_root` extra. Logcat tag: `ds2recomp`.

## History

Adapted from the Lightning Returns Recomp Android app (itself from Lost Odyssey Recomp); all game-specific
parts (second screen, archives, FPS/AA/upscale settings, renderer workarounds, art) were removed.

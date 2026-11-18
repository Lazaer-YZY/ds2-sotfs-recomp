package com.ds2recomp.android;

import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.KeyEvent;
import android.view.Window;
import android.view.WindowManager;
import android.widget.RelativeLayout;
import android.widget.Toast;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;
import java.io.File;
import org.libsdl.app.SDLActivity;

/** Game runtime. The user-supplied game folder holds Default.xex and the disc files. */
public final class RuntimeActivity extends SDLActivity {
    /** Matches the SDL_SendAndroidMessage command in settings/menu.cpp. */
    private static final int COMMAND_OPEN_GAME_FOLDER = 0x8000;
    /** A start that stays in the foreground this long counts as reached for the driver choice. */
    private static final long BOOT_SETTLED_MS = 15000;
    /**
     * Presentation surface height cap (SDK cvar present_max_height): the game
     * renders at 720p, so presenting into a larger swapchain only costs GPU time and heat.
     * 0 = present at the display's size (A/B: flip to 0 and rebuild).
     */
    private static final int PRESENT_MAX_HEIGHT = 720;

    private TouchControlsView touchControls;
    /** Why the renderer could not start with the selected driver; set from native code. */
    private volatile String graphicsFailure;
    private final Handler bootHandler = new Handler(Looper.getMainLooper());
    private volatile BootOverlay bootOverlay;
    private final Runnable bootSettled = () -> {
        GpuDriverStore.clearBootPending(this);
        PlayerLogs.publish(this);
    };

    static native void nativeSetTouchInput(int buttons, int leftTrigger, int rightTrigger,
                                           int leftX, int leftY, int rightX, int rightY);
    static native boolean nativeHasConnectedController();
    /** BootOverlay: 0 not started, 1 running but nothing shown, 2 something on screen, 3 fallback. */
    static native int nativeBootProgress();
    /** Live render options (name, value); the native side ignores names it does not know. */
    static native void nativeSetOption(String name, String value);

    /** Display settings of the CTRL dialog (frame rate, anti-aliasing, upscaling). */
    static SharedPreferences renderPrefs(Context context) {
        return context.getSharedPreferences("render_settings", Context.MODE_PRIVATE);
    }

    /** Debug logging on a fresh install: off in the public build, on in the development build. */
    static final boolean DEFAULT_DEBUG_LOG = !BuildConfig.PUBLIC_DEFAULTS;
    // Frame rate (CTRL dialog): "30", "40" (120 Hz panel, 3 refreshes per frame) or "60"; "panel120" runs 60 on
    // the 120 Hz panel mode (2 refreshes per frame). The native side caps the game's swaps in vertical blanks.
    static final String DEFAULT_FPS = "60";

    /** Whether this start runs the panel (and the emulated vertical blank) at 120 Hz. */
    static boolean panel120Wanted(SharedPreferences render) {
        String fps = render.getString("fps", DEFAULT_FPS);
        return "40".equals(fps) || ("60".equals(fps) && render.getBoolean("panel120", false));
    }

    /** Asks for the panel's 120 Hz mode at the current resolution (the Thor: 60 and 120 Hz). */
    private void requestDisplayMode120() {
        Display display = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R ? getDisplay()
                                                                          : getWindowManager().getDefaultDisplay();
        if (display == null) return;
        Display.Mode current = display.getMode();
        for (Display.Mode mode : display.getSupportedModes()) {
            if (mode.getPhysicalWidth() == current.getPhysicalWidth()
                    && mode.getPhysicalHeight() == current.getPhysicalHeight()
                    && Math.abs(mode.getRefreshRate() - 120f) < 1f) {
                WindowManager.LayoutParams params = getWindow().getAttributes();
                params.preferredDisplayModeId = mode.getModeId();
                getWindow().setAttributes(params);
                return;
            }
        }
    }

    /** CTRL dialog: saves the frame rate; the cap changes at once, the panel / vertical blank rate at the next start. */
    void applyFrameRate(String fps, boolean panel120) {
        renderPrefs(this).edit().putString("fps", fps).putBoolean("panel120", panel120).apply();
        nativeSetOption("fps", fps);
    }

    // Sustained performance mode (on by default; debug extra DS2_SUSTAINED=0 turns it off): asks the
    // power HAL for clocks the device can hold indefinitely instead of bursting to peak clocks and
    // then throttling, which on the Thor showed up as temperature spikes and the fan ramping up and
    // down. Only some devices support it; the result goes to logcat (tag ds2recomp).
    private void applySustainedPerformanceMode() {
        boolean want = true;
        if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
            String extra = getIntent().getStringExtra("DS2_SUSTAINED");
            if ("0".equals(extra)) want = false;
        }
        android.os.PowerManager pm = (android.os.PowerManager) getSystemService(POWER_SERVICE);
        boolean supported = pm != null && pm.isSustainedPerformanceModeSupported();
        if (want && supported) getWindow().setSustainedPerformanceMode(true);
        android.util.Log.i("ds2recomp", "sustained performance mode: " + (supported ? (want ? "on" : "off (DS2_SUSTAINED=0)") : "not supported on this device"));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        PlayerLogs.installCrashHandler(this);
        super.onCreate(savedInstanceState);
        hideSystemBars();
        applySustainedPerformanceMode();
        // 40 fps and Smoother 60 run on the panel's 120 Hz mode (debug launches: --es DS2_VBLANK120 1|0 decides).
        boolean want120 = panel120Wanted(renderPrefs(this));
        if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0
                && getIntent().getStringExtra("DS2_VBLANK120") != null) {
            want120 = "1".equals(getIntent().getStringExtra("DS2_VBLANK120"));
        }
        if (want120) requestDisplayMode120();
        // SDLActivity.onCreate posts setWindowStyle(false), which shows the bars
        // again once this method returns; hide after that command has run, and
        // whenever the bars come back later (SDL window-style changes, a swipe).
        bootHandler.post(this::hideSystemBars);
        ViewCompat.setOnApplyWindowInsetsListener(getWindow().getDecorView(), (view, insets) -> {
            if (insets.isVisible(WindowInsetsCompat.Type.statusBars())
                    || insets.isVisible(WindowInsetsCompat.Type.navigationBars())) {
                if (touchControls != null) touchControls.revealCtrl();
                scheduleHideSystemBars();
            }
            return insets;
        });
        GameStorage.prepare(this);
        GpuDriverStore.markBootPending(this);
        if (mLayout != null && !mBrokenLibraries) {
            // "Loading…" over the black screen at boot, under the touch controls (CTRL stays reachable).
            bootOverlay = new BootOverlay(this);
            mLayout.addView(bootOverlay, new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.MATCH_PARENT,
                RelativeLayout.LayoutParams.MATCH_PARENT));
            touchControls = new TouchControlsView(this);
            mLayout.addView(touchControls, new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.MATCH_PARENT,
                RelativeLayout.LayoutParams.MATCH_PARENT));
            nativeSetTouchInput(0, 0, 0, 0, 0, 0, 0);
        }
    }

    /**
     * Called by the native runtime before its main returns when no Vulkan
     * device the renderer can use was created (video.cpp).
     */
    @SuppressWarnings("unused")
    void reportGraphicsFailure(String reason) {
        graphicsFailure = reason;
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK && event.getAction() == KeyEvent.ACTION_UP
                && !event.isCanceled() && touchControls != null) touchControls.revealCtrl();
        // Keep SDL's existing Back mapping/consumption, including gesture-generated keys.
        return super.dispatchKeyEvent(event);
    }

    @Override
    public void onBackPressed() {
        if (touchControls != null) touchControls.revealCtrl();
        // Preserve SDL's fallback behavior on Android versions that call this directly.
        super.onBackPressed();
    }

    /** Opens the GPU driver page over the game (CTRL dialog). */
    void openGpuDriverPage() {
        Intent intent = new Intent(this, GpuDriverActivity.class);
        intent.putExtra(GpuDriverActivity.EXTRA_FROM_GAME, true);
        startActivity(intent);
    }

    @Override
    protected boolean onUnhandledMessage(int command, Object param) {
        if (command == COMMAND_OPEN_GAME_FOLDER) {
            openGameFolderPage();
            return true;
        }
        return super.onUnhandledMessage(command, param);
    }

    /** Opens the game folder page over the game (CTRL dialog or native settings). */
    void openGameFolderPage() {
        Intent intent = new Intent(this, GameFolderActivity.class);
        intent.putExtra(GameFolderActivity.EXTRA_FROM_GAME, true);
        startActivity(intent);
    }

    @Override
    protected void onPause() {
        if (touchControls != null) touchControls.onHostPause();
        // Leaving the game is when a player connects USB to fetch the log.
        PlayerLogs.publish(this);
        super.onPause();
    }

    /**
     * The game always fills the screen (#199): status and navigation bars stay
     * hidden (a swipe shows them briefly) and the picture extends under the
     * display cutout. SDL only does this when the native side switches to a
     * fullscreen window mode, and the default display mode never does.
     */
    private void hideSystemBars() {
        Window window = getWindow();
        if (window == null) return;
        WindowCompat.setDecorFitsSystemWindows(window, false);
        if (Build.VERSION.SDK_INT >= 28 /* Android 9 (P) */) {
            WindowManager.LayoutParams attributes = window.getAttributes();
            int cutoutMode = Build.VERSION.SDK_INT >= 30 /* Android 11 (R) */
                ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            if (attributes.layoutInDisplayCutoutMode != cutoutMode) {
                attributes.layoutInDisplayCutoutMode = cutoutMode;
                window.setAttributes(attributes);
            }
        }
        WindowInsetsControllerCompat controller =
            WindowCompat.getInsetsController(window, window.getDecorView());
        updateSystemBarsBehavior();
        controller.hide(WindowInsetsCompat.Type.systemBars());
    }

    void updateSystemBarsBehavior() {
        // Transient bars can stay "hidden" in app insets even while drawn. Use
        // normal swipe-revealed bars in hide-all mode so their visibility can
        // restore CTRL; the existing timer hides the bars again after 2.5 s.
        WindowCompat.getInsetsController(getWindow(), getWindow().getDecorView())
            .setSystemBarsBehavior(touchControls != null && touchControls.hidesAllControls()
                ? WindowInsetsControllerCompat.BEHAVIOR_DEFAULT
                : WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }

    /** Bars shown by a swipe stay for a moment, like Android's own transient bars. */
    private static final long HIDE_BARS_AFTER_MS = 2500;
    private final Runnable hideSystemBarsRunnable = this::hideSystemBars;

    private void scheduleHideSystemBars() {
        bootHandler.removeCallbacks(hideSystemBarsRunnable);
        bootHandler.postDelayed(hideSystemBarsRunnable, HIDE_BARS_AFTER_MS);
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
        if (touchControls != null) touchControls.onHostResume();
        bootHandler.removeCallbacks(bootSettled);
        bootHandler.postDelayed(bootSettled, BOOT_SETTLED_MS);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        if (!hasFocus && touchControls != null) touchControls.clearTouches();
        super.onWindowFocusChanged(hasFocus);
        // Dialogs and the pages opened over the game can bring the bars back.
        if (hasFocus) hideSystemBars();
    }

    @Override
    protected void onDestroy() {
        if (touchControls != null) touchControls.clearTouches();
        bootHandler.removeCallbacks(bootSettled);
        bootHandler.removeCallbacks(hideSystemBarsRunnable);
        // A normal exit is not a failed start, however short it was.
        GpuDriverStore.clearBootPending(this);
        super.onDestroy();
        // The same driver would fail again on the next start, and the launcher
        // goes straight to the game once a driver was chosen: show the GPU
        // driver page with the reason instead (#185). Devices without custom
        // drivers get the reason in a dialog.
        if (isFinishing() && graphicsFailure != null) {
            Intent intent = new Intent(this, GpuDriverActivity.class);
            intent.putExtra(GpuDriverActivity.EXTRA_GRAPHICS_FAILURE, graphicsFailure);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            startActivity(intent);
            Runtime.getRuntime().exit(0);
        }
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "c++_shared", "SDL3", "rexruntime", "rexgpu-xenos", "main" };
    }

    /**
     * Native argv (ReXGlue cvars use {@code --name=value}) plus the environment
     * the native side reads through getenv(). See android/README.md.
     */
    @Override
    protected String[] getArguments() {
        File game = GameStorage.gameDir(this);
        // Custom Vulkan driver (libadrenotools): the hook libraries sit in the
        // extracted native library directory, the chosen package in its own folder.
        nativeSetenv("DS2_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir + "/");
        // ReXGlue (our android branch) keeps <name>.toml and its default log folder in REX_ANDROID_APP_DIR;
        // without it they land in the read-only native library directory.
        nativeSetenv("REX_ANDROID_APP_DIR", PlayerLogs.directory(this).getParentFile().getAbsolutePath() + "/");
        File logDir = PlayerLogs.directory(this);
        // Logs go where a PC can copy them over USB; the device line names the phone.
        nativeSetenv("DS2_LOG_DIR", logDir.getAbsolutePath());
        nativeSetenv("DS2_ANDROID_DEVICE", PlayerLogs.deviceDescription());
        // Mesa drivers (Turnip) keep compiled pipelines on disk only with a cache
        // directory; Android gives them none. Other drivers ignore these.
        nativeSetenv("MESA_SHADER_CACHE_DIR", new File(getCacheDir(), "mesa_shader_cache").getAbsolutePath());
        nativeSetenv("MESA_SHADER_CACHE_MAX_SIZE", "256M");
        // Debug launches: `--es DS2_GPU_DRIVER system` runs on the device's own Vulkan driver (A/B against Turnip).
        // Turnip debug switches go through the generic extras forward below (`--es TU_DEBUG sysmem`).
        // `--es DS2_GPU_DRIVER <folder>` runs the installed package files/gpu_driver/<folder> for this launch only
        // (driver A/B; the player's selection is untouched).
        boolean debuggable = (getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0;
        String driverExtra = debuggable ? getIntent().getStringExtra("DS2_GPU_DRIVER") : null;
        boolean systemDriver = "system".equals(driverExtra);
        GpuDriverStore.Installed driver = systemDriver ? null : GpuDriverStore.selectedDriver(this);
        if (!systemDriver && driverExtra != null && !driverExtra.isEmpty() && !driverExtra.contains("/")) {
            GpuDriverStore.Installed chosen = GpuDriverStore.read(new File(GpuDriverStore.root(this), driverExtra));
            if (chosen != null && chosen.library().isFile()) {
                driver = chosen;
            } else {
                android.util.Log.w("DS2", "DS2_GPU_DRIVER " + driverExtra + ": not installed; using the selection");
            }
        }
        if (driver != null) {
            nativeSetenv("DS2_CUSTOM_DRIVER_DIR", driver.directory.getAbsolutePath() + "/");
            nativeSetenv("DS2_VK_CUSTOM_DRIVER", driver.metadata.libraryName);
            nativeSetenv("DS2_VK_CUSTOM_DRIVER_MODE", GpuDriverStore.loadMode(this, driver));
            if (GpuDriverStore.needsAdreno650SyncDraw(GpuDriverStore.gpuModel(), driver.metadata)) {
                nativeSetenv("DS2_VK_ADRENO650_SYNC_DRAW", "1");
            }
            if (GpuDriverStore.needsTurnipSysmem(GpuDriverStore.gpuModel(), driver.metadata)) {
                nativeSetenv("TU_DEBUG", "sysmem");
            }
            // PanVK refuses the Mali GPUs Mesa marks experimental without this.
            if (driver.metadata.isPanVk()) nativeSetenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1");
        }
        // FIFO presentation only: with MAILBOX (the driver's default pick) presents never block, and the
        // repaint the ImGui overlays request every frame turned the SDL UI thread into an unthrottled
        // redraw loop (84% of a core at an idle title menu on the Thor, most of the heat).
        nativeSetenv("REX_VULKAN_ALLOW_PRESENT_MODE_MAILBOX", "false");
        nativeSetenv("REX_VULKAN_ALLOW_PRESENT_MODE_IMMEDIATE", "false");
        nativeSetenv("REX_VULKAN_ALLOW_PRESENT_MODE_FIFO_RELAXED", "false");
        // "Debug logging" (CTRL dialog): DS2_DIAG=1 enables the native side's diagnostics.
        // On by default in the development build; scripted launches use `--es DS2_DIAG 1`.
        SharedPreferences render = renderPrefs(this);
        nativeSetenv("DS2_FRAME_OVERLAY", render.getBoolean("overlay", false) ? "1" : "0");
        nativeSetenv("DS2_DEPTH_OF_FIELD", render.getBoolean("dof", false) ? "1" : "0");
        nativeSetenv("DS2_ECO", render.getBoolean("eco", false) ? "1" : "0");
        nativeSetenv("DS2_FPS", render.getString("fps", DEFAULT_FPS));
        nativeSetenv("DS2_VBLANK120", panel120Wanted(render) ? "1" : "0");
        if (!BuildConfig.PUBLIC_DEFAULTS && !render.getBoolean("debug_log_default_on", false)) {
            render.edit().putBoolean("debug_log", true).putBoolean("debug_log_default_on", true).apply();
        }
        boolean diag = render.getBoolean("debug_log", DEFAULT_DEBUG_LOG);
        if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0
                && getIntent().getStringExtra("DS2_DIAG") != null) {
            diag = !"0".equals(getIntent().getStringExtra("DS2_DIAG"));
        }
        nativeSetenv("DS2_DIAG", diag ? "1" : "0");
        // Development builds only: periodic copies of the save for benches in other areas (ds2_save_snapshots.cpp);
        // never in a public build. Bench launches turn it off with --es DS2_SAVE_SNAPSHOTS 0.
        nativeSetenv("DS2_SAVE_SNAPSHOTS", debuggable && !BuildConfig.PUBLIC_DEFAULTS ? "1" : "0");
        // Debug launches can select an isolated fixture and forward DS2_* switches.
        if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
            String override = getIntent().getStringExtra("game_root");
            if (override != null && !override.isEmpty()) game = new File(override);
            // Every DS2_* and REX_* extra is forwarded as an environment variable:
            // `am start --es DS2_DEBUG_FOO 1`, `--es REX_WAIT_SPIN_US 50` (REX_<CVAR> sets a cvar).
            Bundle extras = getIntent().getExtras();
            if (extras != null) {
                for (String name : extras.keySet()) {
                    // DS2_/REX_ for the game and runtime, TU_/MESA_/FD_/IR3_ for the Turnip driver.
                    if (!name.startsWith("DS2_") && !name.startsWith("REX_") && !name.startsWith("TU_")
                            && !name.startsWith("MESA_") && !name.startsWith("FD_") && !name.startsWith("IR3_")) continue;
                    Object value = extras.get(name);
                    if (value != null) nativeSetenv(name, String.valueOf(value));
                }
            }
        }
        // ReXGlue's defaults for these are XDG paths under $HOME (= /data on Android), which the app
        // cannot write; the runtime aborted on create_directories("/data/.local"). Keep everything in
        // the app's external files dir.
        File appRoot = logDir.getParentFile() != null ? logDir.getParentFile() : getFilesDir();
        File userRoot = new File(appRoot, "user");
        File cacheRoot = new File(appRoot, "cache");
        File metadataRoot = new File(appRoot, "metadata");
        new File(appRoot, "dlc").mkdirs(); // found natively through REX_ANDROID_APP_DIR
        userRoot.mkdirs(); cacheRoot.mkdirs(); metadataRoot.mkdirs();
        // Public APKs carry a shader cache, so the first start precompiles at the title (ShaderSeed).
        if (ShaderSeed.install(this, cacheRoot) && bootOverlay != null) bootOverlay.setFirstStart();
        java.util.ArrayList<String> args = new java.util.ArrayList<>(java.util.Arrays.asList(
            "--game_data_root=" + game.getAbsolutePath(),
            "--user_data_root=" + userRoot.getAbsolutePath(),
            "--cache_root=" + cacheRoot.getAbsolutePath(),
            "--metadata_root=" + metadataRoot.getAbsolutePath(),
            "--log_file=" + new File(logDir, "ds2.log").getAbsolutePath(),
            "--gpu_plugin=xenos",
            "--present_max_height=" + PRESENT_MAX_HEIGHT));
        // Debug launches can add runtime cvars: `am start ... --es cvars "--dump_shaders=/sdcard/... --log_level=debug"`.
        if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
            String extra = getIntent().getStringExtra("cvars");
            if (extra != null) {
                for (String token : extra.trim().split("\\s+")) {
                    if (token.startsWith("--")) args.add(token);
                }
            }
        }
        return args.toArray(new String[0]);
    }
}

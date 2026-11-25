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
import android.view.View;
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
     * Presentation surface height cap (SDK cvar present_max_height) without FSR: the game
     * renders at 720p (or 540p), so presenting into a larger swapchain only costs GPU time and heat;
     * the display hardware scales to the screen. With FSR the swapchain is the screen's size.
     */
    private static final int PRESENT_MAX_HEIGHT = 720;

    private TouchControlsView touchControls;
    /** Save backups while the game runs, when the player chose a folder (SaveBackups; null otherwise). */
    private SaveBackups.Worker saveBackups;
    /** Set when the SDL thread starts the game (getArguments): a restored save is put in place only before that. */
    private static volatile boolean gameStarted;
    /** DeviceProfile.seedDefaults' result at this process's first onCreate, for the game log (DS2_GPU_PROFILE). */
    private static volatile String gpuProfileLine = "";

    private PerfOverlayView perfOverlay;
    /** The bottom screen of dual-screen devices (CompanionScreen), null until the game's views exist. */
    private CompanionScreen companion;
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
    /** The game's recent frame times (ms, oldest first) into out; returns how many (PerfOverlayView). */
    static native int nativeFrameTimes(float[] out);
    /** Second screen: what the game's HUD shows now (ds2_companion_android.cpp), key=value pairs. */
    static native String nativeCompanionState();
    /** Pins the calling thread (the second screen's drawing thread) to the little cores; a line for the log. */
    static native String nativeCompanionPinLittleCores();
    /** Second screen: an equipment icon's texture data by its hash from the state line, or null. */
    static native byte[] nativeCompanionIcon(long hash);

    /** Display settings of the CTRL dialog (frame rate, anti-aliasing, upscaling). */
    static SharedPreferences renderPrefs(Context context) {
        return context.getSharedPreferences("render_settings", Context.MODE_PRIVATE);
    }

    /** Debug logging on a fresh install: off in the public build, on in the development build. */
    static final boolean DEFAULT_DEBUG_LOG = !BuildConfig.PUBLIC_DEFAULTS;
    // CTRL dialog "Use the prime core" (ds2_affinity.cpp SetPrimeAssist).
    static final boolean DEFAULT_PRIME = true;
    // Frame rate (CTRL dialog): "30", "40" (120 Hz panel, 3 refreshes per frame) or "60"; "panel120" runs 60 on
    // the 120 Hz panel mode (2 refreshes per frame). The native side caps the game's swaps in vertical blanks.
    static final String DEFAULT_FPS = "60";

    // Display (CTRL dialog, from the next start): "render_height" "540" (the game's own screen made 960x540,
    // ds2_render_size.cpp), "720" (the console's) or "1440" (the renderer draws 720p at 2x, 2560x1440: very sharp,
    // about 24 fps on the AYN Thor); "fsr": the image is upscaled to the screen with AMD FSR 1 (sharper) instead of
    // the display's own scaler. The video mode stays 1280x720: the game ignores it, the presenter takes the shape.
    static final String DEFAULT_RENDER_HEIGHT = "720";
    // The CTRL dialog's choices, in order. "1080": the game's own 1920x1080 screen; its colour and depth targets do not
    // fit the console's 10 MB EDRAM, so the emulated EDRAM is made larger (gpu_edram_tiles, ds2_render_size.cpp).
    // "360" (640x360) and "432" (768x432): the game's own screen made smaller still, for GPUs that cannot hold 30 fps at
    // 540p (Mali-G615 at ~40 ms of GPU work per frame at 540p, issue #21, 2026-11-23); ds2_render_size.cpp accepts
    // heights down to 360.
    static final String[] RENDER_HEIGHTS = { "360", "432", "540", "720", "1080", "1440" };

    // Game language (CTRL dialog, from the next start): one of LANGUAGE_IDS, English by default (the owner,
    // 2026-11-20), or "auto": the device's language when the game has it, English otherwise. The game reads the console's language setting (user_language: Xbox 360
    // language ids) and picks its texts by language and game region (gameRegion; checked on the Thor, DECISIONS.md,
    // 2026-11-20). The disc has no Simplified Chinese (id 10 shows English in every region).
    static final String DEFAULT_LANGUAGE = "1";
    static final String[] LANGUAGE_IDS = { "auto", "1", "4", "3", "6", "5", "9", "11", "12", "2", "7", "8" };
    // Each language in its own words (the dialog's list), in the order of LANGUAGE_IDS; index 0 is "auto".
    static final String[] LANGUAGE_NAMES = { null, "English", "Français", "Deutsch", "Italiano", "Español",
        "Português (Brasil)", "Polski", "Русский", "日本語", "한국어", "中文 (繁體)" };

    /** The Xbox 360 language id the device's own language maps to (1 = English when the game lacks it). */
    static int deviceLanguageId() {
        java.util.Locale locale = java.util.Locale.getDefault();
        switch (locale.getLanguage()) {
            case "ja": return 2;
            case "de": return 3;
            case "fr": return 4;
            case "es": return 5;
            case "it": return 6;
            case "ko": return 7;
            case "zh": return 8;  // the game's only Chinese is Traditional
            case "pt": return 9;
            case "pl": return 11;
            case "ru": return 12;
            default: return 1;
        }
    }

    /** The dialog's name for a language id (LANGUAGE_NAMES), English for an unknown one. */
    static String languageName(int id) {
        for (int i = 1; i < LANGUAGE_IDS.length; ++i) {
            if (LANGUAGE_IDS[i].equals(Integer.toString(id))) return LANGUAGE_NAMES[i];
        }
        return LANGUAGE_NAMES[1];
    }

    /** The language id this start uses (the CTRL choice, or the device's language for "auto"). */
    static int languageId(SharedPreferences render) {
        String choice = render.getString("language", DEFAULT_LANGUAGE);
        if ("auto".equals(choice)) return deviceLanguageId();
        try {
            return Integer.parseInt(choice);
        } catch (NumberFormatException e) {
            return 1;
        }
    }

    /**
     * The game region a language needs (XGetGameRegion), 0 = the runtime's default (North America). Each edition
     * has its own languages: the European one French, German, Italian, Spanish, Polish and Russian; the American one
     * English and Brazilian Portuguese; the Asian ones Japanese (Japan), Korean and Traditional Chinese (rest of Asia).
     */
    static int gameRegion(int language) {
        switch (language) {
            case 1: case 9: return 0;       // English, Portuguese (Brazil): North America, as before
            case 2: return 0x0101;          // Japanese: Japan
            case 7: case 8: return 0x01FC;  // Korean, Traditional Chinese: rest of Asia
            default: return 0x02FE;         // French, German, Italian, Spanish, Polish, Russian: Europe
        }
    }

    /** The language's runtime arguments (unless a debug launch's cvars set them). */
    static void addLanguageArguments(java.util.List<String> args, SharedPreferences render, String debugCvars) {
        if (debugCvars != null && debugCvars.contains("--user_language")) return;
        int language = languageId(render);
        int region = gameRegion(language);
        args.add("--user_language=" + language);
        if (region != 0) args.add("--user_game_region=" + region);
        android.util.Log.i("ds2recomp", "language " + language + " (" + render.getString("language", DEFAULT_LANGUAGE)
            + ", device " + java.util.Locale.getDefault().toLanguageTag() + "), game region " + region);
    }

    /**
     * "Fill wide screens" (CTRL, from the next start; ds2_render_size.cpp): the game's screen as wide as the display
     * at the same height (Hor+: more to the sides, the HUD and menus kept 16:9 in the middle). The colour and depth
     * targets must fit the emulated EDRAM together (4096 tiles: at most 2400 wide at 540, 1760 at 720; 8192 tiles at
     * 1080: at most 2400, since the game's D3D device is not created at 2560 x 1080, the boot crashes; 2400 covers 20:9
     * phones, 2026-11-22). At every resolution since 2026-11-22: 1080p wide had come out lavender (the game's Clear
     * dropped bit 12 of a post-processing target's EDRAM base, fixed in ds2_render_size.cpp); 1440p is 720p wide
     * scaled 2x. Returns the width, or 0 for the console's 16:9.
     */
    /** Whether Fill screen is offered at this CTRL resolution choice (all of them; see wideWidth). */
    static boolean wideSupported(String renderHeight) {
        return "360".equals(renderHeight) || "432".equals(renderHeight) || "540".equals(renderHeight)
            || "720".equals(renderHeight) || "1080".equals(renderHeight) || "1440".equals(renderHeight);
    }

    static int wideWidth(SharedPreferences render, int height, float displayAspect) {
        if (!render.getBoolean("wide", false) || displayAspect <= 16f / 9f + 0.01f) return 0;
        int width = Math.round(height * displayAspect / 16f) * 16;
        int max = height <= 540 ? 2400 : height <= 720 ? 1760 : 2400;
        width = Math.min(width, max);
        return width > (height * 16 / 9 + 15) / 16 * 16 ? width : 0;
    }

    /** The display's long side over its short side (the game runs in landscape). */
    float displayAspect() {
        android.graphics.Point size = new android.graphics.Point();
        getWindowManager().getDefaultDisplay().getRealSize(size);
        int longSide = Math.max(size.x, size.y), shortSide = Math.min(size.x, size.y);
        return shortSide > 0 ? (float) longSide / shortSide : 16f / 9f;
    }

    /**
     * CTRL's anti-aliasing: FXAA on the finished 3D scene before the game draws its 2D layer (vulkan_scene_fxaa), so
     * the menus' and HUD's text stays sharp. (On the whole finished frame it blurred that text: the owner's S25,
     * 2026-11-21, "fxaa actually makes the text more difficult to read"; the option was hidden until this.)
     */
    static final boolean SHOW_AA = true;

    /** The CTRL anti-aliasing choice: "none", "fxaa" or "fxaa_extreme". */
    static String aaMode(SharedPreferences render) {
        if (!SHOW_AA) return "none";
        String aa = render.getString("aa", "off");
        return "fxaa".equals(aa) || "fxaa_extreme".equals(aa) ? aa : "none";
    }

    /** The display options' runtime arguments (render size, presentation). */
    static void addDisplayArguments(java.util.List<String> args, SharedPreferences render, float displayAspect) {
        String height = render.getString("render_height", DEFAULT_RENDER_HEIGHT);
        boolean fsr = render.getBoolean("fsr", false);
        boolean smallScreen = "360".equals(height) || "432".equals(height) || "540".equals(height);
        int gameHeight = smallScreen ? Integer.parseInt(height) : "1080".equals(height) ? 1080 : 720;
        int wide = wideSupported(height) ? wideWidth(render, gameHeight, displayAspect) : 0;
        if (wide != 0) {
            // The game's screen, and the presenter's shape (it letterboxes to the video mode).
            args.add("--ds2_render_height=" + gameHeight);
            args.add("--ds2_render_width=" + wide);
            args.add("--video_mode_width=" + wide);
            args.add("--video_mode_height=" + gameHeight);
            // Colour, depth and the post-processing pool of a wider screen do not fit the console's 10 MB EDRAM
            // side by side: the emulated EDRAM is doubled, at 1080 quadrupled (ds2_render_size.cpp).
            if (gameHeight <= 720) args.add("--gpu_edram_tiles=4096");
            android.util.Log.i("ds2recomp", "display: wide screen " + wide + "x" + gameHeight + " (display aspect "
                + displayAspect + ")");
        }
        // Without FSR the swapchain is the game's height and the display hardware scales it to the screen (cheapest);
        // with FSR, or a picture larger than the screen, it is the screen's own size and the presenter scales.
        int presentHeight = PRESENT_MAX_HEIGHT;
        if (smallScreen) {
            args.add("--ds2_render_height=" + gameHeight);
            presentHeight = gameHeight;
        } else if ("1080".equals(height)) {
            args.add("--ds2_render_height=1080");
            args.add("--gpu_edram_tiles=8192");
            presentHeight = 1080;
        } else if ("1440".equals(height)) {
            args.add("--draw_resolution_scale_x=2");
            args.add("--draw_resolution_scale_y=2");
            presentHeight = 0;
        }
        args.add("--present_max_height=" + (fsr ? 0 : presentHeight));
        // Texture filtering at an angle (CTRL, from the next start): the renderer forces 4x anisotropic filtering on
        // mipmapped textures (anisotropic_override 3); off samples each texture once per pixel, cheaper on GPUs short
        // of texture bandwidth (a test for Mali, issue #21).
        if (!render.getBoolean("aniso", true)) args.add("--anisotropic_override=0");
        // Anti-aliasing (CTRL, also switched live): FXAA on the 3D scene before the 2D layer (the resolve feed of the
        // scene; not with 1440p's resolution scaling, which has no resolve feeding). The swap post effect stays off.
        args.add("--swap_post_effect=none");
        String aa = aaMode(render);
        args.add("--vulkan_scene_fxaa=" + ("fxaa_extreme".equals(aa) ? 2 : "fxaa".equals(aa) ? 1 : 0));
        args.add("--present_effect=" + (fsr ? "fsr" : "bilinear"));
        android.util.Log.i("ds2recomp", "display: render height " + height + ", " + (fsr ? "FSR upscaling" : "display scaling")
            + ", anisotropic filtering " + (render.getBoolean("aniso", true) ? "4x" : "off"));
    }

    /** Whether this start runs the panel (and the emulated vertical blank) at 120 Hz. */
    static boolean panel120Wanted(SharedPreferences render) {
        String fps = render.getString("fps", DEFAULT_FPS);
        return "40".equals(fps) || ("60".equals(fps) && render.getBoolean("panel120", false));
    }

    /**
     * The CTRL settings read only when the game starts (marked * in the dialog): language, resolution, FSR, wide
     * screens, debug logging, the 120 Hz panel (40 fps, Smoother 60). The dialog offers a restart when they change
     * (the owner, 2026-11-21: "Change in resolution etc didn't seem to apply till after restart").
     */
    static String nextStartState(SharedPreferences render) {
        return render.getString("language", DEFAULT_LANGUAGE) + "|" + render.getString("render_height", DEFAULT_RENDER_HEIGHT)
            + "|" + render.getBoolean("fsr", false) + "|" + render.getBoolean("wide", false)
            + "|" + render.getBoolean("aniso", true)
            + "|" + render.getBoolean("debug_log", DEFAULT_DEBUG_LOG) + "|" + panel120Wanted(render)
            + "|" + render.getBoolean("compat_shortcuts", false) + "|" + render.getBoolean("compat_gpl", false)
            + "|" + render.getBoolean("compat_dynrender", false) + "|" + render.getString("compat_tu_mode", "")
            + "|" + render.getBoolean("compat_tu_noubwc", false) + "|" + render.getBoolean("compat_shader", false)
            + "|" + render.getBoolean("compat_decode_bc", false) + "|" + render.getBoolean("compat_tu_reserve", false)
            + "|" + render.getBoolean("compat_hdr_r11g11b10", false)
            + "|" + render.getBoolean("compat_shortcuts_transfers", false)
            + "|" + render.getBoolean("compat_shortcuts_resolves", false)
            + "|" + render.getBoolean("compat_feed_off", false) + "|" + render.getBoolean("compat_lazy_off", false)
            + "|" + render.getBoolean("compat_7e3_resolve_off", false)
            + "|" + render.getBoolean(MALI_RELAXED_INTERPOLATORS, false) + "|" + render.getBoolean(MALI_NO_ROBUST, false);
    }

    /**
     * CTRL -> GPU compatibility (testing; issues #8 Adreno 650 and #11 Adreno 660 draw the game wrongly): renderer
     * paths to try one at a time. Each is off by default; the runtime settings go on the command line (debug launches
     * can still override them), the Turnip ones into TU_DEBUG (turnipDebugFlags).
     */
    static void addCompatArguments(java.util.List<String> args, SharedPreferences render) {
        int first = args.size();
        addCompatArgumentsTo(args, render);
        if (args.size() > first)
            android.util.Log.i("ds2recomp", "GPU compatibility: " + String.join(" ", args.subList(first, args.size())));
    }

    /** The render target transfer shortcuts, switched off (CTRL's transfer group). */
    /** CTRL Mali tests (Mali only, off by default): 16-bit interpolators; robustBufferAccess off. */
    static final String MALI_RELAXED_INTERPOLATORS = "mali_relaxed_interpolators";
    static final String MALI_NO_ROBUST = "mali_no_robust_buffer_access";

    static final String[] TRANSFER_SHORTCUTS_OFF = {
        "--gpu_skip_overwritten_color_transfers=false", "--gpu_constant_transfers_as_clears=false",
        "--gpu_redirect_msaa_depth_clears=false", "--gpu_ownership_from_first_row=false" };
    /** The resolve shortcuts, switched off (CTRL's resolve group). */
    static final String[] RESOLVE_SHORTCUTS_OFF = {
        "--direct_host_resolve=false", "--vulkan_resolve_feed_textures=false",
        "--vulkan_direct_resolve_float24=false", "--vulkan_direct_resolve_7e3=false",
        "--vulkan_direct_resolve_7e3_to_8888=false", "--vulkan_direct_resolve_4x_as_1x=false" };

    private static void addCompatArgumentsTo(java.util.List<String> args, SharedPreferences render) {
        // "Renderer shortcuts off" is the three groups below together. In development builds the transfer and resolve
        // groups, and three switches inside the resolve group, also have their own CTRL switches (the owner's S25,
        // 2026-11-22: the 1080p G-buffer-normals flashes; paused, DECISIONS.md). Release builds ignore those.
        boolean bisect = BuildConfig.DEBUG;
        boolean allShortcuts = render.getBoolean("compat_shortcuts", false);
        if (allShortcuts || (bisect && render.getBoolean("compat_shortcuts_transfers", false))) {
            java.util.Collections.addAll(args, TRANSFER_SHORTCUTS_OFF);
        }
        if (allShortcuts || (bisect && render.getBoolean("compat_shortcuts_resolves", false))) {
            java.util.Collections.addAll(args, RESOLVE_SHORTCUTS_OFF);
        }
        if (bisect && render.getBoolean("compat_feed_off", false)) args.add("--vulkan_resolve_feed_textures=false");
        if (bisect && render.getBoolean("compat_lazy_off", false)) args.add("--vulkan_resolve_feed_lazy_dump=false");
        if (bisect && render.getBoolean("compat_7e3_resolve_off", false)) {
            args.add("--vulkan_direct_resolve_7e3_to_8888=false");
        }
        if (allShortcuts) {
            java.util.Collections.addAll(args,
                "--vulkan_shared_memory_hoist_uploads=false", "--spirv_texture_sign_specialization=false",
                "--spirv_texture_exp_adjust_specialization=false", "--vulkan_memexport_coalesce_barriers=false");
        }
        // v0.1.14-test, for the a6xx textures (#8 / #13: title right with shortcuts off, textures and text noise):
        // the shader translation shortcuts LR's port offers too, and BC textures decoded by the load shaders.
        if (render.getBoolean("compat_shader", false)) {
            java.util.Collections.addAll(args, "--spirv_texture_sign_branch=false",
                "--spirv_structured_forward_jumps=false", "--vulkan_texture_fetch_implicit_lod=false",
                "--vulkan_shared_memory_texel_buffer=false");
        }
        if (render.getBoolean("compat_decode_bc", false)) args.add("--vulkan_decode_bc_textures=true");
        // Issues #8 / #18 (Adreno 650 / 660, 2026-11-22): menus right with decoded textures, gameplay in flat blue and
        // noise with a black band every 64 rows, i.e. what is read back from the game's HDR scene target (7e3, a 64-bit
        // RGBA16F image on the host). This keeps it in a 32-bit B10G11R11 image instead (Thor, 2026-11-22, 540p with
        // decoded textures, Majula: correct picture, 60 fps like without it).
        if (render.getBoolean("compat_hdr_r11g11b10", false)) args.add("--vulkan_7e3_as_r11g11b10=true");
        if (render.getBoolean("compat_gpl", false)) {
            java.util.Collections.addAll(args, "--vulkan_pipeline_libraries=false",
                                         "--vulkan_precompile_vertex_libraries=false");
        }
        if (render.getBoolean("compat_dynrender", false)) args.add("--vulkan_dynamic_rendering=false");
        // Mali tests (the owner, 2026-11-25: "keep it just as a test for mali users, other cpus should have correct
        // defaults even on test build"): only on a Mali GPU, whatever the stored settings say. The image compression
        // line is diagnostics only (Arm's framebuffer compression per render target kind, in the log), with Debug
        // logging on: its query could not be run on any device here (Turnip and Qualcomm's driver lack the extension).
        if (GpuDriverStore.vendor() == GpuDriverStore.Vendor.MALI) {
            if (render.getBoolean("debug_log", DEFAULT_DEBUG_LOG)) args.add("--vulkan_log_image_compression=true");
            if (render.getBoolean(MALI_RELAXED_INTERPOLATORS, false))
                args.add("--vulkan_relaxed_precision_interpolators=true");
            if (render.getBoolean(MALI_NO_ROBUST, false)) args.add("--vulkan_robust_buffer_access=false");
        }
    }

    /** Turnip's TU_DEBUG flags from CTRL's GPU compatibility choices ("" = none). */
    static String turnipDebugFlags(SharedPreferences render) {
        StringBuilder flags = new StringBuilder();
        String mode = render.getString("compat_tu_mode", "");
        if ("sysmem".equals(mode) || "gmem".equals(mode)) flags.append(mode);
        if (render.getBoolean("compat_tu_noubwc", false)) flags.append(flags.length() > 0 ? "," : "").append("noubwc");
        return flags.toString();
    }

    /** After CTRL changed a next-start setting: restart now (progress since the last save is lost) or later. */
    void offerRestart() {
        new android.app.AlertDialog.Builder(this)
            .setTitle(getString(R.string.restart_title))
            .setMessage(getString(R.string.restart_message))
            .setPositiveButton(getString(R.string.restart_now), (dialog, which) -> restartGame())
            .setNegativeButton(getString(R.string.restart_later), null)
            .show();
    }

    /** Restarts the game in a new process (CTRL's next-start settings, a restored save backup). */
    void restartGame() {
        // As the game folder page: the pending activity outlives the process; Android starts a new one for it.
        Intent intent = new Intent(this, RuntimeActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(intent);
        Runtime.getRuntime().exit(0);
    }

    /**
     * CTRL's Exit button (the owner, 2026-11-22: "cant see another way to exit the app"): asks first, then ends the game
     * like a restart does, without starting it again. The game has no exit of its own: its Quit Game (System menu)
     * saves and returns to the title screen.
     */
    void confirmExit() {
        new android.app.AlertDialog.Builder(this)
            .setTitle(getString(R.string.exit_title))
            .setMessage(getString(R.string.exit_message))
            .setPositiveButton(getString(R.string.exit_confirm), (dialog, which) -> exitGame())
            .setNegativeButton(getString(R.string.touch_cancel), null)
            .show();
    }

    private void exitGame() {
        android.util.Log.i("ds2recomp", "exit from CTRL");
        if (touchControls != null) touchControls.clearTouches();
        stopSaveBackups();
        GpuDriverStore.clearBootPending(this);  // a chosen exit is not a failed start, however soon it came
        finishAndRemoveTask();
        Runtime.getRuntime().exit(0);
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
        renderPrefs(this).edit().putString("fps", fps).putBoolean("panel120", panel120).commit();
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
        // This GPU's first-start resolution and frame rate, for settings the player has not saved (DeviceProfile);
        // before anything below reads them.
        if (!gameStarted) gpuProfileLine = DeviceProfile.seedDefaults(this);
        // A save backup restored from CTRL goes in place first, before SDL starts the game and it reads its save
        // (SaveBackups.applyPendingRestore; never under a game already running in this process).
        String restored = gameStarted ? null : SaveBackups.applyPendingRestore(this);
        super.onCreate(savedInstanceState);
        // Development launches on a test phone that locks between unattended runs (`--es DS2_SHOW_WHEN_LOCKED 1`):
        // the game shows over the lock screen like an alarm app; it does not unlock the phone or show anything else.
        if (BuildConfig.DEBUG && android.os.Build.VERSION.SDK_INT >= 27 && getIntent() != null
                && "1".equals(getIntent().getStringExtra("DS2_SHOW_WHEN_LOCKED"))) {
            setShowWhenLocked(true);
            setTurnScreenOn(true);
        }
        if (restored != null) Toast.makeText(this, restored, Toast.LENGTH_LONG).show();
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
        // Off unless the player chose a backup folder in CTRL (nothing runs for anyone else).
        if (SaveBackups.folderSetting(this) != null) startSaveBackups(false);
        if (mLayout != null && !mBrokenLibraries) {
            // "Loading…" over the black screen at boot, under the touch controls (CTRL stays reachable).
            bootOverlay = new BootOverlay(this);
            mLayout.addView(bootOverlay, new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.MATCH_PARENT,
                RelativeLayout.LayoutParams.MATCH_PARENT));
            // Performance overlay (CTRL dialog), drawn by Android over the game, under the touch controls.
            perfOverlay = new PerfOverlayView(this);
            perfOverlay.setVisibility(View.GONE);
            mLayout.addView(perfOverlay, new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.MATCH_PARENT,
                RelativeLayout.LayoutParams.MATCH_PARENT));
            boolean debugOverlay = (getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0
                && "1".equals(getIntent().getStringExtra("DS2_PERF_VIEW"));
            setPerfOverlay(debugOverlay || renderPrefs(this).getBoolean("overlay", false));
            touchControls = new TouchControlsView(this);
            mLayout.addView(touchControls, new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.MATCH_PARENT,
                RelativeLayout.LayoutParams.MATCH_PARENT));
            nativeSetTouchInput(0, 0, 0, 0, 0, 0, 0);
            // The game's HUD on the second screen of dual-screen devices (CTRL "Second screen HUD").
            companion = new CompanionScreen(this);
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
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK && touchControls != null && touchControls.ctrlGone()) {
            // CTRL removed after the start (CTRL option): Back opens Settings, and the key goes nowhere else.
            if (event.getAction() == KeyEvent.ACTION_UP && !event.isCanceled()) touchControls.openSettingsFromBack();
            return true;
        }
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK && event.getAction() == KeyEvent.ACTION_UP
                && !event.isCanceled() && touchControls != null) touchControls.revealCtrl();
        // Keep SDL's existing Back mapping/consumption, including gesture-generated keys.
        return super.dispatchKeyEvent(event);
    }

    @Override
    public void onBackPressed() {
        if (touchControls != null && touchControls.ctrlGone()) {
            touchControls.openSettingsFromBack();  // the back gesture, which comes without a key event
            return;
        }
        if (touchControls != null) touchControls.revealCtrl();
        // Preserve SDL's fallback behavior on Android versions that call this directly.
        super.onBackPressed();
    }

    /** CTRL "Second screen HUD": remembered and applied at once (UI thread). */
    void applyCompanionSetting(boolean on, boolean hideOnMain) {
        renderPrefs(this).edit().putBoolean(CompanionScreen.PREF, on).putBoolean(CompanionScreen.PREF_HIDE, hideOnMain).apply();
        if (companion != null) companion.update();
    }

    /** Shows or hides the performance overlay (UI thread; CTRL dialog). */
    void setPerfOverlay(boolean shown) {
        if (perfOverlay != null) perfOverlay.setShown(shown);
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

    private static final int REQUEST_EXPORT_LOGS = 0x4453;  // 'DS'

    /**
     * CTRL dialog's "Save logs": the system file picker, then a zip of the game's logs there. Android 11+ hides
     * Android/data from the phone's file managers, so this is how a player attaches a log from the phone (the
     * owner's S25, 2026-11-21; ported from LR's public #4 fix).
     */
    void startLogExport() {
        Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT)
            .addCategory(Intent.CATEGORY_OPENABLE)
            .setType("application/zip")
            .putExtra(Intent.EXTRA_TITLE, PlayerLogs.exportName(new java.util.Date()));
        try {
            startActivityForResult(intent, REQUEST_EXPORT_LOGS);
        } catch (android.content.ActivityNotFoundException e) {
            Toast.makeText(this, getString(R.string.logs_no_picker), Toast.LENGTH_LONG).show();
        }
    }

    private static final int REQUEST_SAVE_BACKUP_FOLDER = 0x4454;  // 'DT'

    /** Opens CTRL's save backup page (SaveBackups.Page). */
    void openSaveBackupsPage() {
        new SaveBackups.Page(this, false).show();
    }

    /** The system folder picker for save backups; the folder's permission is kept in onActivityResult. */
    void chooseSaveBackupFolder() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        android.net.Uri previous = SaveBackups.folderSetting(this);
        if (previous != null) {
            try {
                // The picker opens in the current folder (it takes a document, not a tree).
                intent.putExtra(android.provider.DocumentsContract.EXTRA_INITIAL_URI,
                    android.provider.DocumentsContract.buildDocumentUriUsingTree(previous,
                        android.provider.DocumentsContract.getTreeDocumentId(previous)));
            } catch (RuntimeException ignored) {
                // The picker's own starting place.
            }
        }
        try {
            startActivityForResult(intent, REQUEST_SAVE_BACKUP_FOLDER);
        } catch (android.content.ActivityNotFoundException e) {
            Toast.makeText(this, getString(R.string.backup_no_picker), Toast.LENGTH_LONG).show();
        }
    }

    /** Starts the save backup thread if it is not running; checkNow: one check right away (a new folder). */
    void startSaveBackups(boolean checkNow) {
        if (saveBackups == null) saveBackups = new SaveBackups.Worker(this);
        if (checkNow) saveBackups.checkSoon();
    }

    void stopSaveBackups() {
        if (saveBackups != null) saveBackups.shutdown();
        saveBackups = null;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == REQUEST_EXPORT_LOGS) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) exportLogs(data.getData());
            return;
        }
        if (requestCode == REQUEST_SAVE_BACKUP_FOLDER) {
            if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
            try {
                SaveBackups.setFolder(this, data.getData());
            } catch (RuntimeException e) {
                Toast.makeText(this, getString(R.string.backup_failed, String.valueOf(e.getMessage())),
                    Toast.LENGTH_LONG).show();
                return;
            }
            startSaveBackups(true);
            // The page again, offering the folder's newest backup when it has some (a reinstall picks its old folder).
            new SaveBackups.Page(this, true).show();
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    private void exportLogs(android.net.Uri uri) {
        java.util.List<File> files = PlayerLogs.exportable(PlayerLogs.directory(this));
        String version;
        try {
            version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            version = "?";
        }
        GpuDriverStore.Installed driver = GpuDriverStore.selectedDriver(this);
        SharedPreferences prefs = renderPrefs(this);
        String device = PlayerLogs.deviceDescription() + "\napp=" + getPackageName() + " " + version
            + "\ngpu=" + GpuDriverStore.gpuModel()
            + "\ndriver=" + (driver != null ? driver.metadata.name : GpuDriverStore.SYSTEM_DRIVER_NAME)
            + "\nsettings=" + new java.util.TreeMap<>(prefs.getAll());
        new Thread(() -> {
            String message;
            try (java.io.OutputStream output = getContentResolver().openOutputStream(uri, "w")) {
                if (output == null) throw new java.io.IOException("the file could not be opened");
                PlayerLogs.exportZip(files, device, output);
                String name = null;
                try (android.database.Cursor row = getContentResolver().query(uri,
                        new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                    if (row != null && row.moveToFirst()) name = row.getString(0);
                } catch (RuntimeException ignored) {
                    // The name is only for the message.
                }
                message = getString(R.string.logs_saved, name != null ? name : "a zip");
            } catch (java.io.IOException | RuntimeException e) {
                message = getString(R.string.logs_save_failed, String.valueOf(e.getMessage()));
            }
            final String shown = message;
            runOnUiThread(() -> Toast.makeText(this, shown, Toast.LENGTH_LONG).show());
        }, "DS2 log export").start();
    }

    @Override
    protected void onPause() {
        bootHandler.removeCallbacks(deviceNote);
        if (touchControls != null) touchControls.onHostPause();
        // Leaving the game is when a player connects USB to fetch the log.
        PlayerLogs.publish(this);
        // The player may be done for now: back up a changed save (on the backup thread, onPause does not wait).
        if (saveBackups != null) saveBackups.checkSoon();
        if (companion != null) companion.onPause();
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
        if (companion != null) companion.onResume();
        bootHandler.removeCallbacks(bootSettled);
        bootHandler.postDelayed(bootSettled, BOOT_SETTLED_MS);
        bootHandler.removeCallbacks(deviceNote);
        bootHandler.post(deviceNote);
    }

    /**
     * Battery temperature and Android's thermal status (0 none .. 6 shutdown) and headroom (1.0 = at the throttling
     * point, 10 s ahead) for the game log's heartbeat, every few seconds while the game is in front (issue #8: a phone
     * that rebooted with nothing in its log; the native side reads the CPU / GPU sensors and memory itself).
     */
    private static final long DEVICE_NOTE_MS = 4000;
    private final Runnable deviceNote = new Runnable() {
        @Override public void run() {
            StringBuilder note = new StringBuilder();
            Intent battery = registerReceiver(null, new android.content.IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            if (battery != null) {
                int tenths = battery.getIntExtra(android.os.BatteryManager.EXTRA_TEMPERATURE, Integer.MIN_VALUE);
                if (tenths != Integer.MIN_VALUE) note.append("bat_c=").append(tenths / 10f);
            }
            android.os.PowerManager power = (android.os.PowerManager) getSystemService(Context.POWER_SERVICE);
            if (power != null && Build.VERSION.SDK_INT >= 29) {
                note.append(note.length() > 0 ? " " : "").append("thermal=").append(power.getCurrentThermalStatus());
                if (Build.VERSION.SDK_INT >= 30) {
                    float headroom = power.getThermalHeadroom(10);
                    if (!Float.isNaN(headroom)) {
                        note.append(String.format(java.util.Locale.ROOT, " headroom=%.2f", headroom));
                    }
                }
            }
            try {
                if (note.length() > 0) nativeSetOption("device_thermal", note.toString());
            } catch (UnsatisfiedLinkError notLoaded) {
                return;  // the native library did not load: nothing to report to
            }
            bootHandler.postDelayed(this, DEVICE_NOTE_MS);
        }
    };

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
        stopSaveBackups();
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
        gameStarted = true;
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
        nativeSetenv("DS2_GPU_PROFILE", gpuProfileLine);
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
            boolean syncDraw = GpuDriverStore.needsAdreno650SyncDraw(GpuDriverStore.gpuModel(), driver.metadata);
            if (syncDraw) nativeSetenv("DS2_VK_ADRENO650_SYNC_DRAW", "1");
            // TU_DEBUG: the device's needs plus CTRL's GPU compatibility choices in one list (the native side adds
            // syncdraw only while TU_DEBUG is unset, so it goes in here when other flags are set).
            // CTRL -> GPU compatibility: let Turnip keep the descriptor set it reserves on Adreno 6xx for dynamic
            // offsets (the native side turns the reservation off unless this is set; the renderer needs 4 sets).
            if (renderPrefs(this).getBoolean("compat_tu_reserve", false)) {
                nativeSetenv("tu_dont_reserve_descriptor_set", "false");
                android.util.Log.i("ds2recomp", "tu_dont_reserve_descriptor_set=false");
            }
            String turnipFlags = turnipDebugFlags(renderPrefs(this));
            if (GpuDriverStore.isTurnip(driver) && GpuDriverStore.needsTurnipSysmem(GpuDriverStore.gpuModel(), driver.metadata)
                    && !turnipFlags.contains("mem")) {
                turnipFlags = turnipFlags.isEmpty() ? "sysmem" : "sysmem," + turnipFlags;
            }
            if (!turnipFlags.isEmpty()) {
                if (syncDraw) turnipFlags = "syncdraw," + turnipFlags;
                nativeSetenv("TU_DEBUG", turnipFlags);
                android.util.Log.i("ds2recomp", "TU_DEBUG=" + turnipFlags);
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
        // The CTRL dialog's overlay is PerfOverlayView now; the native ImGui one only for debug launches
        // (`--es DS2_FRAME_OVERLAY 1`, forwarded below): while it exists the presenter paints every frame from the
        // UI thread, which cost frames in play (2026-11-20).
        nativeSetenv("DS2_FRAME_OVERLAY", "0");
        nativeSetenv("DS2_DEPTH_OF_FIELD", render.getBoolean("dof", false) ? "1" : "0");
        // CTRL cheats (ds2_cheats.cpp), also switched live through nativeSetOption.
        StringBuilder cheats = new StringBuilder();
        if (render.getBoolean("cheat_hp", false)) cheats.append("hp,");
        if (render.getBoolean("cheat_stamina", false)) cheats.append("stamina,");
        if (render.getBoolean("cheat_wear", false)) cheats.append("wear,");
        if (!debuggable || getIntent().getStringExtra("DS2_CHEATS") == null) nativeSetenv("DS2_CHEATS", cheats.toString());
        // CTRL -> Controller buttons (PadRemap, issue #9), also switched live through nativeSetOption("pad_map").
        nativeSetenv("DS2_PAD_MAP", PadRemap.setting(render));
        if (!debuggable || getIntent().getStringExtra("DS2_PRIME_ASSIST") == null) {
            nativeSetenv("DS2_PRIME_ASSIST", render.getBoolean("prime", DEFAULT_PRIME) ? "1" : "0");
        }
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
        // DS2_DIAG=0 also turns the GPU command thread's timers and statistics off (ds2_android.cpp ApplyDefaults).
        // Development builds only: periodic copies of the save for benches in other areas (ds2_save_snapshots.cpp);
        // never in a public build. Bench launches turn it off with --es DS2_SAVE_SNAPSHOTS 0.
        nativeSetenv("DS2_SAVE_SNAPSHOTS", debuggable && !BuildConfig.PUBLIC_DEFAULTS ? "1" : "0");
        // DLC next to the game (issue #6, GameStorage): the folders its packages are installed from and, with the
        // game on an SD card, where the game installs them (empty = the app's user/ folder, as before).
        StringBuilder dlcDirs = new StringBuilder();
        for (File dir : GameStorage.dlcPackageDirs(this))
            dlcDirs.append(dlcDirs.length() > 0 ? ";" : "").append(dir.getAbsolutePath());
        nativeSetenv("DS2_DLC_DIRS", dlcDirs.toString());
        File dlcRoot = GameStorage.dlcContentRoot(this);
        nativeSetenv("DS2_DLC_ROOT", dlcRoot != null ? dlcRoot.getAbsolutePath() : "");
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
        new File(appRoot, "dlc").mkdirs(); // the game finds packages through DS2_DLC_DIRS (above)
        GameStorage.dlcDir(this).mkdirs();
        userRoot.mkdirs(); cacheRoot.mkdirs(); metadataRoot.mkdirs();
        // Public APKs carry a shader cache, so the first start precompiles at the title (ShaderSeed).
        if (ShaderSeed.install(this, cacheRoot) && bootOverlay != null) bootOverlay.setFirstStart();
        java.util.ArrayList<String> args = new java.util.ArrayList<>(java.util.Arrays.asList(
            "--game_data_root=" + game.getAbsolutePath(),
            "--user_data_root=" + userRoot.getAbsolutePath(),
            "--cache_root=" + cacheRoot.getAbsolutePath(),
            "--metadata_root=" + metadataRoot.getAbsolutePath(),
            "--log_file=" + new File(logDir, "ds2.log").getAbsolutePath(),
            "--gpu_plugin=xenos"));
        addDisplayArguments(args, render, displayAspect());
        addCompatArguments(args, render);
        boolean debuggableBuild = (getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0;
        addLanguageArguments(args, render, debuggableBuild ? getIntent().getStringExtra("cvars") : null);
        // Debug launches can add runtime cvars: `am start ... --es cvars "--dump_shaders=/sdcard/... --log_level=debug"`.
        if (debuggableBuild) {
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

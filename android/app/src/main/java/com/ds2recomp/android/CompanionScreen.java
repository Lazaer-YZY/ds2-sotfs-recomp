package com.ds2recomp.android;

import android.app.Activity;
import android.app.Presentation;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Display;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;

/**
 * The bottom screen of dual-screen devices (AYN Thor: 1240x1080, touch): the game's HUD (HP, stamina, status
 * build-up, souls) drawn with the game's own art, as an Android Presentation on the first presentation display.
 * Shown while the game is in the foreground and the CTRL dialog's "Second screen HUD" is on (on by default; it only
 * exists on devices with a second display). Drawing happens on its own thread, off the game's cores
 * (CompanionSurface). Ported from the Lightning Returns port's companion screen.
 *
 * The window never takes focus (FLAG_NOT_FOCUSABLE): with one global focus, a touch on the second display would
 * otherwise move key focus there and the game would stop seeing the controller.
 */
final class CompanionScreen implements DisplayManager.DisplayListener {
    private static final String TAG = "ds2recomp";
    static final String PREF = "companion";
    static final String PREF_HIDE = "companion_hide";

    private final Activity activity;
    private final DisplayManager displays;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final String demo;
    private final Boolean forced;
    private Presentation presentation;
    private CompanionSurface surface;
    private boolean resumed;

    CompanionScreen(Activity activity) {
        this.activity = activity;
        this.displays = (DisplayManager) activity.getSystemService(Activity.DISPLAY_SERVICE);
        boolean debuggable = (activity.getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0;
        // Debug launches: --es DS2_COMPANION_DEMO hud draws made-up values (layout checks without playing), cycle
        // alternates HUD, cutscene dimming and the cover;
        // --es DS2_COMPANION 0|1 overrides the CTRL setting for an A/B.
        demo = debuggable ? activity.getIntent().getStringExtra("DS2_COMPANION_DEMO") : null;
        String force = debuggable ? activity.getIntent().getStringExtra("DS2_COMPANION") : null;
        forced = force == null ? null : !"0".equals(force);
    }

    /** True when the device has a second screen the companion can use. */
    static boolean available(Activity activity) {
        DisplayManager dm = (DisplayManager) activity.getSystemService(Activity.DISPLAY_SERVICE);
        return dm != null && target(activity, dm) != null;
    }

    /** The setting; on by default (it only does something on dual-screen devices). */
    static boolean enabled(SharedPreferences render) {
        return render.getBoolean(PREF, true);
    }

    /** "Hide them on the main screen" (what the second screen shows); on by default. */
    static boolean hideOnMain(SharedPreferences render) {
        return render.getBoolean(PREF_HIDE, true);
    }

    void onResume() {
        resumed = true;
        if (displays != null) displays.registerDisplayListener(this, handler);
        update();
    }

    void onPause() {
        resumed = false;
        if (displays != null) displays.unregisterDisplayListener(this);
        // Leaving the game gives the bottom screen back (ES-DE's companion, the launcher).
        dismiss();
    }

    /** Hides the moved HUD on the main screen while the second screen shows (and the setting is on). */
    void applyHide() {
        boolean debuggable = (activity.getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0;
        String force = debuggable ? activity.getIntent().getStringExtra("DS2_COMPANION_HIDE") : null;
        boolean hide = force != null ? !"0".equals(force) : hideOnMain(RuntimeActivity.renderPrefs(activity));
        RuntimeActivity.nativeSetOption("companion_hide", hide ? "1" : "0");
    }

    /** Applies the CTRL dialog's setting at once. */
    void update() {
        boolean want = resumed && (forced != null ? forced : enabled(RuntimeActivity.renderPrefs(activity)));
        Display display = want && displays != null ? target(activity, displays) : null;
        if (display == null) {
            dismiss();
            return;
        }
        if (presentation != null && presentation.getDisplay().getDisplayId() == display.getDisplayId()) {
            applyHide();
            return;
        }
        dismiss();
        show(display);
    }

    /**
     * The second screen: a presentation display other than the one the game is on, and only while the game is on the
     * device's own screen (the Thor's top screen). Issue #35: in Samsung DeX the game runs on the external monitor,
     * which is itself a presentation display, so the HUD "moved" to the game's own display and was hidden there.
     */
    private static Display target(Activity activity, DisplayManager dm) {
        int gameDisplay = gameDisplayId(activity);
        if (gameDisplay != Display.DEFAULT_DISPLAY) return null;
        for (Display d : dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)) {
            if (d.getDisplayId() != Display.DEFAULT_DISPLAY && d.getDisplayId() != gameDisplay
                    && d.getState() != Display.STATE_OFF) return d;
        }
        return null;
    }

    @SuppressWarnings("deprecation")
    private static int gameDisplayId(Activity activity) {
        Display display = null;
        try {
            display = android.os.Build.VERSION.SDK_INT >= 30 ? activity.getDisplay()
                : activity.getWindowManager().getDefaultDisplay();
        } catch (RuntimeException ignored) {
            // No display yet: treated as the device's own screen.
        }
        return display != null ? display.getDisplayId() : Display.DEFAULT_DISPLAY;
    }

    private void show(Display display) {
        try {
            surface = new CompanionSurface(activity, demo);
            presentation = new Screen(activity, display, surface);
            presentation.show();
            surface.start();
            RuntimeActivity.nativeSetOption("companion", "1");  // the game's HUD icons are captured while shown
            CompanionAssets.ensure(activity);  // the art from the game files, once (public APKs carry none)
            applyHide();
            Log.i(TAG, "companion: showing on display " + display.getDisplayId() + " (" + display.getName() + ")");
        } catch (WindowManager.InvalidDisplayException | IllegalStateException e) {
            Log.w(TAG, "companion: cannot show on display " + display.getDisplayId(), e);
            if (surface != null) surface.stop();
            presentation = null;
            surface = null;
        }
    }

    private void dismiss() {
        if (surface != null) {
            surface.stop();
            RuntimeActivity.nativeSetOption("companion", "0");
        }
        if (presentation != null) {
            try {
                presentation.dismiss();
            } catch (RuntimeException ignored) {
            }
        }
        presentation = null;
        surface = null;
    }

    @Override public void onDisplayAdded(int displayId) { update(); }
    @Override public void onDisplayRemoved(int displayId) { update(); }
    @Override public void onDisplayChanged(int displayId) { update(); }

    /** The Presentation window: full screen, no system bars, never focused. */
    private static final class Screen extends Presentation {
        private final View content;

        Screen(Activity owner, Display display, View content) {
            super(owner, display);
            this.content = content;
        }

        @Override
        protected void onCreate(Bundle savedInstanceState) {
            super.onCreate(savedInstanceState);
            Window window = getWindow();
            if (window != null) {
                window.addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                    | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
                    | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                    | WindowManager.LayoutParams.FLAG_FULLSCREEN);
                window.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
            }
            setContentView(content);
            // The bottom screen never changes faster than the bars ease: its 60 Hz mode is enough.
            Display d = getDisplay();
            if (window != null && d != null) {
                Display.Mode cur = d.getMode();
                for (Display.Mode m : d.getSupportedModes()) {
                    if (m.getPhysicalWidth() == cur.getPhysicalWidth() && m.getPhysicalHeight() == cur.getPhysicalHeight()
                            && Math.abs(m.getRefreshRate() - 60f) < 1f) {
                        WindowManager.LayoutParams lp = window.getAttributes();
                        lp.preferredDisplayModeId = m.getModeId();
                        window.setAttributes(lp);
                        break;
                    }
                }
            }
        }
    }
}

// Adapted from Lost Odyssey Recomp (GPL-3.0).
package com.ds2recomp.android;

import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.SparseArray;
import android.util.TypedValue;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;

/** Local touch controls layered over SDL's surface; physical pads stay in SDL. */
final class TouchControlsView extends View {
    private static final long CONTROLLER_POLL_MS = 1000;
    private static final int UP = 0x0001, DOWN = 0x0002, LEFT = 0x0004, RIGHT = 0x0008;
    private static final int START = 0x0010, BACK = 0x0020;
    private static final int L3 = 0x0040, R3 = 0x0080;
    private static final int LB = 0x0100, RB = 0x0200;
    private static final int A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;
    private static final String[] NAMES = {
        "LEFT STICK", "RIGHT STICK", "D-PAD", "A", "B", "X", "Y",
        "L3", "R3", "LB", "LT", "BACK", "START", "RT", "RB"
    };

    private static final class Pointer {
        final int element;
        float x, y;

        Pointer(int element, float x, float y) {
            this.element = element;
            this.x = x;
            this.y = y;
        }
    }

    private final RuntimeActivity activity;
    private final SharedPreferences preferences;
    private final InputManager inputManager;
    private final TouchControllerVisibility visibility;
    private final Handler controllerHandler = new Handler(Looper.getMainLooper());
    private final Runnable controllerPoll = new Runnable() {
        @Override public void run() {
            if (!hostResumed || !isAttachedToWindow()) return;
            refreshControllers();
            controllerHandler.postDelayed(this, CONTROLLER_POLL_MS);
        }
    };
    private final InputManager.InputDeviceListener deviceListener = new InputManager.InputDeviceListener() {
        @Override public void onInputDeviceAdded(int deviceId) { refreshControllers(); }
        @Override public void onInputDeviceRemoved(int deviceId) { refreshControllers(); }
        @Override public void onInputDeviceChanged(int deviceId) { refreshControllers(); }
    };
    private final TouchCtrlHandle ctrl = new TouchCtrlHandle();
    private final Runnable ctrlWake = this::invalidate;
    private long ctrlWakeAt = -1;
    private boolean settingsDialogOpen, settingsPointerRestores;
    private final SparseArray<Pointer> pointers = new SparseArray<>();
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private TouchControlLayout layout, draft;
    private CheckBox settingsToggle;
    private TextView controllerHint;
    private boolean settingsChoiceEdited, syncingSettingsToggle;
    private boolean editing, customLayout, hostResumed, deviceListenerRegistered;
    private int settingsPointer = -1, dragPointer = -1, toolbarPointer = -1;
    private int selected = -1, toolbarPressed = -1;
    private float dragOffsetX, dragOffsetY, baseUnit;
    private int shownButtons, shownLt, shownRt, shownLx, shownLy, shownRx, shownRy;

    // Drag to look (CTRL -> "Drag on the screen to move the camera", off by default: the right stick stays; the
    // owner, 2026-11-21). A touch on the right part of the screen, away from the controls, turns the camera: the
    // finger's speed becomes a right-stick deflection, sampled once per display frame (onLookTick), so the camera
    // stops when the finger stops.
    /** Touches that start left of this share of the width never look (the left stick's side). */
    private static final float LOOK_MIN_X = .35f;
    /**
     * Finger speed (units per second) that gives a full deflection at 100 % camera drag speed. DS2 turns the camera
     * about 120 degrees/s at full deflection (a full turn in ~3 s on the Thor) and ~13 degrees/s at 0.43 (pad.sh: a
     * Majula 360 in 27.3 s), roughly linear past its dead zone: a drag across the Thor's screen (14 units) at 8
     * units/s turns the camera about 200 degrees.
     */
    private static final float LOOK_FULL_RATE = 8f;
    /** Slower than this (units per second) counts as a still finger. */
    private static final float LOOK_MIN_RATE = .35f;
    /** Smallest deflection sent while the finger moves: just past the game's own dead zone (~0.35, pad.sh). */
    private static final float LOOK_FLOOR = .36f;
    /** A drag axis slower than this share of the other one is left out (a sideways drag does not tilt the view). */
    private static final float LOOK_MINOR_AXIS = .35f;
    private boolean lookDrag;
    private float lookSpeed = 1f;
    private int lookPointer = -1;
    private float lookLastX, lookLastY, lookAccX, lookAccY, lookVelX, lookVelY;
    private long lookTickNanos;
    private boolean lookTickPosted;
    private final Runnable lookTick = this::onLookTick;

    TouchControlsView(RuntimeActivity activity) {
        super(activity);
        this.activity = activity;
        preferences = activity.getSharedPreferences("touch_controls", Context.MODE_PRIVATE);
        visibility = new TouchControllerVisibility(preferences.getBoolean("enabled", true));
        inputManager = (InputManager) activity.getSystemService(Context.INPUT_SERVICE);
        customLayout = preferences.getInt("layout_version", 0) == 1;
        long now = SystemClock.uptimeMillis();
        ctrl.reset(now);
        ctrl.setEnabled(preferences.getBoolean("ctrl_auto_hide", true), now);
        ctrl.setHideAll(preferences.getBoolean("hide_all", false), now);
        lookDrag = preferences.getBoolean("look_drag", false);
        lookSpeed = bounded(preferences.getFloat("look_speed", 1f), .25f, 3f, 1f);
        setClickable(true);
        setContentDescription("Touch controller settings");
    }

    private void refreshControllers() {
        int controllers = 0;
        if (inputManager != null) {
            for (int deviceId : inputManager.getInputDeviceIds()) {
                InputDevice device = inputManager.getInputDevice(deviceId);
                if (device == null || device.isVirtual()) continue;
                int sources = device.getSources();
                if ((sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                    (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
                    ++controllers;
                }
            }
        }
        if (RuntimeActivity.nativeHasConnectedController()) controllers = Math.max(controllers, 1);
        if (!visibility.setControllerCount(controllers)) return;
        clearTouches();
        if (settingsToggle != null && !settingsChoiceEdited) {
            syncingSettingsToggle = true;
            settingsToggle.setChecked(visibility.visible());
            syncingSettingsToggle = false;
        }
        if (controllerHint != null)
            controllerHint.setVisibility(visibility.controllerPresent() ? VISIBLE : GONE);
        invalidate();
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        startControllerWatch();
    }

    private void startControllerWatch() {
        if (!hostResumed || !isAttachedToWindow()) return;
        if (inputManager != null && !deviceListenerRegistered) {
            inputManager.registerInputDeviceListener(deviceListener, controllerHandler);
            deviceListenerRegistered = true;
        }
        refreshControllers();
        controllerHandler.removeCallbacks(controllerPoll);
        controllerHandler.postDelayed(controllerPoll, CONTROLLER_POLL_MS);
    }

    private void stopControllerWatch() {
        controllerHandler.removeCallbacks(controllerPoll);
        if (inputManager != null && deviceListenerRegistered) {
            inputManager.unregisterInputDeviceListener(deviceListener);
            deviceListenerRegistered = false;
        }
    }

    void onHostResume() {
        hostResumed = true;
        // Back from a CTRL page or another app: show CTRL again, retract later.
        ctrl.reset(SystemClock.uptimeMillis());
        invalidate();
        startControllerWatch();
    }

    void onHostPause() {
        hostResumed = false;
        stopControllerWatch();
        cancelCtrlWake();
        clearTouches();
    }

    /** System bars and Android Back restore only CTRL, never the hidden gamepad. */
    void revealCtrl() {
        ctrl.activity(SystemClock.uptimeMillis());
        invalidate();
    }

    private boolean controlsVisible() {
        return !ctrl.hideAll() && visibility.visible();
    }

    boolean hidesAllControls() { return ctrl.hideAll(); }

    private static float bounded(float value, float minimum, float maximum,
                                 float fallback) {
        return Float.isFinite(value) ? Math.max(minimum, Math.min(maximum, value))
            : fallback;
    }

    @Override
    protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        super.onSizeChanged(width, height, oldWidth, oldHeight);
        if (width <= 0 || height <= 0) return;
        baseUnit = Math.min(width * .0715f, height * .1625f);
        float size = bounded(preferences.getFloat("size", 1f),
                             TouchControlLayout.MIN_SIZE,
                             TouchControlLayout.MAX_SIZE, 1f);
        float opacity = bounded(preferences.getFloat("opacity", 1f),
                                TouchControlLayout.MIN_OPACITY,
                                TouchControlLayout.MAX_OPACITY, 1f);
        TouchControlLayout fresh = TouchControlLayout.defaults(
            width, height, baseUnit * size);
        fresh.size = size;
        fresh.opacity = opacity;
        if (customLayout) {
            for (int id = 0; id < TouchControlLayout.COUNT; ++id) {
                float storedX = preferences.getFloat("x_" + id, fresh.x[id]);
                float storedY = preferences.getFloat("y_" + id, fresh.y[id]);
                if (Float.isFinite(storedX)) fresh.x[id] = storedX;
                if (Float.isFinite(storedY)) fresh.y[id] = storedY;
                fresh.scale[id] = TouchControlLayout.clampScale(preferences.getFloat("scale_" + id, 1f));
                fresh.visible[id] = preferences.getBoolean("visible_" + id, true);
            }
            float ctrlX = preferences.getFloat("ctrl_x", fresh.ctrlX);
            float ctrlY = preferences.getFloat("ctrl_y", fresh.ctrlY);
            if (Float.isFinite(ctrlX)) fresh.ctrlX = ctrlX;
            if (Float.isFinite(ctrlY)) fresh.ctrlY = ctrlY;
        }
        layout = fresh;
        layout.clampAll(width, height, baseUnit * layout.size, 0f);
        if (draft != null) draft.clampAll(width, height, baseUnit * draft.size,
                                          editorReservedBottom());
        clearTouches();
    }

    /**
     * The editor's toolbar sits in the middle band, which the default layout leaves free, so no strip is reserved for
     * it: entering the editor (and SAVE) never moves controls (with a bottom toolbar the clamp pushed L3, R3 and the
     * d-pad up, and SAVE kept them there). Its buttons win touches over a control dragged under them.
     */
    private static final float TOOLBAR_Y = .47f, TOOLBAR_LABEL_Y = .385f;
    private float editorReservedBottom() { return 0f; }
    private TouchControlLayout activeLayout() { return editing ? draft : layout; }
    private float unit() { return baseUnit * activeLayout().size; }
    private float px(int id) { return activeLayout().pixelX(id, getWidth()); }
    private float py(int id) { return activeLayout().pixelY(id, getHeight()); }
    private float settingX() { return activeLayout().ctrlX * getWidth(); }
    private float settingY() { return activeLayout().ctrlY * getHeight(); }
    private float settingRadius() { return unit() * .45f; }
    private boolean onCtrl(float x, float y) {
        return near(x, y, settingX(), settingY(), unit() * TouchControlLayout.CTRL_HIT_RADIUS);
    }

    /** Lays out CTRL for the current slide progress (see TouchCtrlHandle). */
    private float layoutCtrl(long now) {
        float progress = ctrl.progress(now);
        // The retracted tab sits at the physical screen edge: the system bars are
        // hidden in the game, so the layout's status-bar strip is not reserved here.
        ctrl.layout(settingX(), settingY(), settingRadius(), getWidth(), getHeight(),
                    48f * getResources().getDisplayMetrics().density, 0f, progress);
        return progress;
    }

    private void syncCtrlPin() {
        ctrl.setPinned(editing || settingsDialogOpen || settingsPointer != -1,
                       SystemClock.uptimeMillis());
        invalidate();
    }

    private void cancelCtrlWake() {
        controllerHandler.removeCallbacks(ctrlWake);
        ctrlWakeAt = -1;
    }

    private void scheduleCtrlWake(long now) {
        long wait = ctrl.nextWakeMs(now);
        if (wait == 0) {
            postInvalidateOnAnimation();
        } else if (wait > 0 && hostResumed) {
            long at = now + wait;
            if (at == ctrlWakeAt) return;
            controllerHandler.removeCallbacks(ctrlWake);
            controllerHandler.postAtTime(ctrlWake, at);
            ctrlWakeAt = at;
        }
    }

    private static boolean near(float x, float y, float cx, float cy, float radius) {
        float dx = x - cx, dy = y - cy;
        return dx * dx + dy * dy <= radius * radius;
    }

    /** The unit of one control: the overall size times its own (Edit layout's - / +). */
    private float elementUnit(int id) { return unit() * activeLayout().scale[id]; }

    /** How far (x, y) is into control {@code id}'s touch area: below 1 inside, 0 at its centre. */
    private float hitDistance(int id, float x, float y) {
        float unit = elementUnit(id);
        float dx = (x - px(id)) / (TouchControlLayout.hitHalfWidth(id) * unit);
        float dy = (y - py(id)) / (TouchControlLayout.hitHalfHeight(id) * unit);
        if (TouchControlLayout.isRect(id)) {
            float edge = Math.max(Math.abs(dx), Math.abs(dy));
            return edge * edge;
        }
        return dx * dx + dy * dy;
    }

    private boolean hit(int id, float x, float y) {
        return hitDistance(id, x, y) <= 1f;
    }

    private int elementAt(float x, float y, boolean includeHidden) {
        int best = -1;
        float bestDistance = Float.POSITIVE_INFINITY;
        for (int id = 0; id < TouchControlLayout.COUNT; ++id) {
            if (!includeHidden && !activeLayout().visible[id]) continue;
            float distance = hitDistance(id, x, y);
            if (distance <= 1f && distance < bestDistance) {
                best = id;
                bestDistance = distance;
            }
        }
        return best;
    }

    private static int stick(float value) {
        if (Math.abs(value) < .12f) return 0;
        return Math.round(Math.max(-1f, Math.min(1f, value)) * 32767f);
    }

    private static int buttonMask(int element) {
        switch (element) {
            case TouchControlLayout.A: return A;
            case TouchControlLayout.B: return B;
            case TouchControlLayout.X: return X;
            case TouchControlLayout.Y: return Y;
            case TouchControlLayout.L3: return L3;
            case TouchControlLayout.R3: return R3;
            case TouchControlLayout.LB: return LB;
            case TouchControlLayout.RB: return RB;
            case TouchControlLayout.BACK: return BACK;
            case TouchControlLayout.START: return START;
            default: return 0;
        }
    }

    private void publish() {
        int buttons = 0, lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0;
        boolean leftAssigned = false, rightAssigned = false;
        if (controlsVisible() && !editing && layout != null && baseUnit > 0f) {
            for (int i = 0; i < pointers.size(); ++i) {
                Pointer pointer = pointers.valueAt(i);
                int id = pointer.element;
                if (!layout.visible[id]) continue;
                float unit = elementUnit(id);
                if (id == TouchControlLayout.LEFT_STICK && !leftAssigned) {
                    float dx = (pointer.x - px(id)) / unit;
                    float dy = (py(id) - pointer.y) / unit;
                    float length = (float) Math.hypot(dx, dy);
                    if (length > 1f) { dx /= length; dy /= length; }
                    lx = stick(dx); ly = stick(dy); leftAssigned = true;
                } else if (id == TouchControlLayout.RIGHT_STICK && !rightAssigned) {
                    float dx = (pointer.x - px(id)) / unit;
                    float dy = (py(id) - pointer.y) / unit;
                    float length = (float) Math.hypot(dx, dy);
                    if (length > 1f) { dx /= length; dy /= length; }
                    rx = stick(dx); ry = stick(dy); rightAssigned = true;
                } else if (id == TouchControlLayout.DPAD) {
                    // One direction at a time, the one the finger is furthest towards: DS2 gives each d-pad
                    // direction its own item slot, and a press between two squares must not cycle both.
                    float dx = (pointer.x - px(id)) / unit;
                    float dy = (pointer.y - py(id)) / unit;
                    if (Math.max(Math.abs(dx), Math.abs(dy)) >= .25f) {
                        if (Math.abs(dx) > Math.abs(dy)) buttons |= dx < 0f ? LEFT : RIGHT;
                        else buttons |= dy < 0f ? UP : DOWN;
                    }
                } else if (hit(id, pointer.x, pointer.y)) {
                    if (id == TouchControlLayout.LT) lt = 255;
                    else if (id == TouchControlLayout.RT) rt = 255;
                    else buttons |= buttonMask(id);
                }
            }
            if (!rightAssigned && lookPointer != -1) {
                // Like a mouse: a drag to the right turns right, a drag up looks up (stick up). Per axis, because
                // the game's dead zone is per axis (on the Thor 0.30 right + 0.30 up did not turn the camera, 0.45
                // right did); a much slower second axis is dropped so a sideways drag stays level.
                float speedX = Math.abs(lookVelX), speedY = Math.abs(lookVelY);
                if (speedY < speedX * LOOK_MINOR_AXIS) speedY = 0f;
                if (speedX < speedY * LOOK_MINOR_AXIS) speedX = 0f;
                rx = Math.round(Math.signum(lookVelX) * lookDeflection(speedX) * 32767f);
                ry = Math.round(-Math.signum(lookVelY) * lookDeflection(speedY) * 32767f);
            }
        }
        shownButtons = buttons; shownLt = lt; shownRt = rt;
        shownLx = lx; shownLy = ly; shownRx = rx; shownRy = ry;
        RuntimeActivity.nativeSetTouchInput(buttons, lt, rt, lx, ly, rx, ry);
        invalidate();
    }

    /** Stick deflection (0..1) for a finger speed along one axis, in units per second. */
    private float lookDeflection(float speed) {
        if (speed < LOOK_MIN_RATE) return 0f;
        return LOOK_FLOOR + (1f - LOOK_FLOOR) * Math.min(1f, speed * lookSpeed / LOOK_FULL_RATE);
    }

    /**
     * Once per display frame while a look drag is down: the finger's movement since the last frame becomes its speed
     * (units per second, smoothed over about two frames), which publish() turns into a right-stick deflection.
     */
    private void onLookTick() {
        lookTickPosted = false;
        if (lookPointer == -1) {
            lookVelX = lookVelY = 0f;
            return;
        }
        long now = System.nanoTime();
        float seconds = Math.max(1e-3f, (now - lookTickNanos) / 1e9f);
        lookTickNanos = now;
        float unit = Math.max(1f, unit());
        float rateX = lookAccX / unit / seconds, rateY = lookAccY / unit / seconds;
        lookAccX = lookAccY = 0f;
        lookVelX = lookVelX * .5f + rateX * .5f;
        lookVelY = lookVelY * .5f + rateY * .5f;
        publish();
        postLookTick();
    }

    private void postLookTick() {
        if (lookTickPosted || lookPointer == -1) return;
        lookTickPosted = true;
        postOnAnimation(lookTick);
    }

    private void endLook() {
        lookPointer = -1;
        lookAccX = lookAccY = lookVelX = lookVelY = 0f;
    }

    void clearTouches() {
        endLook();
        removeCallbacks(lookTick);
        lookTickPosted = false;
        pointers.clear();
        boolean heldCtrl = settingsPointer != -1;
        settingsPointer = dragPointer = toolbarPointer = -1;
        settingsPointerRestores = false;
        if (heldCtrl) syncCtrlPin();
        toolbarPressed = -1;
        publish();
    }

    private void setControlsEnabled(boolean value) {
        clearTouches();
        boolean previous = visibility.enabledPreference();
        visibility.applyShow(value);
        if (visibility.enabledPreference() != previous)
            preferences.edit().putBoolean("enabled", visibility.enabledPreference()).apply();
        invalidate();
    }

    private void saveLayout() {
        SharedPreferences.Editor editor = preferences.edit().putInt("layout_version", 1);
        for (int id = 0; id < TouchControlLayout.COUNT; ++id) {
            editor.putFloat("x_" + id, layout.x[id]);
            editor.putFloat("y_" + id, layout.y[id]);
            editor.putFloat("scale_" + id, layout.scale[id]);
            editor.putBoolean("visible_" + id, layout.visible[id]);
        }
        editor.putFloat("ctrl_x", layout.ctrlX).putFloat("ctrl_y", layout.ctrlY);
        editor.apply();
        customLayout = true;
    }

    private void applySettings(boolean show, int sizePercent, int opacityPercent,
                               boolean ctrlAutoHide, boolean hideAll) {
        clearTouches();
        ctrl.setEnabled(ctrlAutoHide, SystemClock.uptimeMillis());
        ctrl.setHideAll(hideAll, SystemClock.uptimeMillis());
        activity.updateSystemBarsBehavior();
        float size = sizePercent / 100f;
        if (!customLayout) layout = TouchControlLayout.defaults(
            getWidth(), getHeight(), baseUnit * size);
        layout.size = size;
        layout.opacity = opacityPercent / 100f;
        layout.clampAll(getWidth(), getHeight(), baseUnit * layout.size, 0f);
        preferences.edit().putFloat("size", layout.size)
            .putFloat("opacity", layout.opacity)
            .putBoolean("ctrl_auto_hide", ctrlAutoHide)
            .putBoolean("hide_all", hideAll).apply();
        setControlsEnabled(show);
    }

    /**
     * A labelled radio group of two or three choices added to {@code content}; returns the
     * first button (its group holds the others, see {@link #radioChoice}).
     */
    private RadioButton radio(LinearLayout content, String label, String a, String b, String c,
                              int checked) {
        return radio(content, label, checked, c != null ? new String[] { a, b, c } : new String[] { a, b });
    }

    /** The same with any number of choices. */
    private RadioButton radio(LinearLayout content, String label, int checked, String... texts) {
        if (label != null) header(content, label);
        RadioGroup group = new RadioGroup(content.getContext());
        RadioButton first = null;
        for (int i = 0; i < texts.length; ++i) {
            RadioButton button = new RadioButton(content.getContext());
            button.setId(View.generateViewId());
            button.setText(texts[i]);
            group.addView(button);
            if (i == checked) button.setChecked(true);
            if (first == null) first = button;
        }
        content.addView(group);
        return first;
    }

    /** A bold section header of the settings dialog. */
    private void header(LinearLayout content, String text) {
        TextView view = new TextView(content.getContext());
        view.setText(text);
        view.setTypeface(Typeface.DEFAULT_BOLD);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        float density = getResources().getDisplayMetrics().density;
        view.setPadding(0, Math.round(16f * density), 0, Math.round(2f * density));
        content.addView(view);
    }

    /** A short grey line under an option, indented to the option's text. */
    private TextView hint(LinearLayout content, String text) {
        TextView view = new TextView(content.getContext());
        view.setText(text);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        view.setAlpha(0.7f);
        float density = getResources().getDisplayMetrics().density;
        view.setPadding(Math.round(32f * density), 0, 0, Math.round(4f * density));
        content.addView(view);
        return view;
    }

    /** A checkbox's state; false for one the dialog does not show in this build. */
    private static boolean checked(CheckBox box) {
        return box != null && box.isChecked();
    }

    /** A checkbox with an optional grey explanation under it. */
    private CheckBox option(LinearLayout content, String label, String help, boolean checked) {
        CheckBox box = new CheckBox(content.getContext());
        box.setText(label);
        box.setChecked(checked);
        content.addView(box);
        if (help != null) hint(content, help);
        return box;
    }

    /** Index of the checked button in the group of {@code first}, 0 if none. */
    private static int radioChoice(RadioButton first) {
        RadioGroup group = (RadioGroup) first.getParent();
        for (int i = 0; i < group.getChildCount(); ++i) {
            if (((RadioButton) group.getChildAt(i)).isChecked()) return i;
        }
        return 0;
    }

    private void showSettings() {
        clearTouches();
        Context strings = activity;
        LinearLayout content = new LinearLayout(activity);
        content.setOrientation(LinearLayout.VERTICAL);
        int padding = Math.round(20f * getResources().getDisplayMetrics().density);
        content.setPadding(padding, 0, padding, 0);

        header(content, strings.getString(R.string.touch_section_controls));
        CheckBox toggle = new CheckBox(activity);
        toggle.setText(strings.getString(R.string.touch_show_controls));
        toggle.setChecked(visibility.visible());
        settingsChoiceEdited = false;
        toggle.setOnCheckedChangeListener((button, checked) -> {
            if (!syncingSettingsToggle) settingsChoiceEdited = true;
        });
        content.addView(toggle);
        TextView hint = hint(content, strings.getString(R.string.touch_controller_hint));
        hint.setVisibility(visibility.controllerPresent() ? VISIBLE : GONE);
        settingsToggle = toggle;
        controllerHint = hint;

        TextView sizeLabel = new TextView(activity);
        SeekBar size = new SeekBar(activity);
        size.setMax(80);
        size.setProgress(Math.round(layout.size * 100f) - 60);
        sizeLabel.setText(strings.getString(R.string.touch_control_size, size.getProgress() + 60));
        size.setOnSeekBarChangeListener(seekListener(value ->
            sizeLabel.setText(strings.getString(R.string.touch_control_size, value + 60))));
        content.addView(sizeLabel);
        content.addView(size);

        TextView opacityLabel = new TextView(activity);
        SeekBar opacity = new SeekBar(activity);
        opacity.setMax(75);
        opacity.setProgress(Math.round(layout.opacity * 100f) - 25);
        opacityLabel.setText(strings.getString(R.string.touch_opacity, opacity.getProgress() + 25));
        opacity.setOnSeekBarChangeListener(seekListener(value ->
            opacityLabel.setText(strings.getString(R.string.touch_opacity, value + 25))));
        content.addView(opacityLabel);
        content.addView(opacity);

        CheckBox ctrlAutoHide = new CheckBox(activity);
        ctrlAutoHide.setText(strings.getString(R.string.touch_ctrl_auto_hide));
        ctrlAutoHide.setChecked(ctrl.enabled());
        content.addView(ctrlAutoHide);

        CheckBox hideAll = option(content, strings.getString(R.string.touch_hide_all),
            strings.getString(R.string.touch_hide_all_help), ctrl.hideAll());
        // Drag to look (off by default; the right stick stays): applied at once.
        CheckBox lookDragBox = option(content, strings.getString(R.string.touch_look_drag),
            strings.getString(R.string.touch_look_drag_help), lookDrag);
        TextView lookSpeedLabel = new TextView(activity);
        SeekBar lookSpeedBar = new SeekBar(activity);
        lookSpeedBar.setMax(275);
        lookSpeedBar.setProgress(Math.round(lookSpeed * 100f) - 25);
        lookSpeedLabel.setText(strings.getString(R.string.touch_look_speed, lookSpeedBar.getProgress() + 25));
        lookSpeedBar.setOnSeekBarChangeListener(seekListener(value ->
            lookSpeedLabel.setText(strings.getString(R.string.touch_look_speed, value + 25))));
        content.addView(lookSpeedLabel);
        content.addView(lookSpeedBar);
        // Physical controller buttons (issue #9): PadRemap's page, applied at once.
        header(content, strings.getString(R.string.touch_section_pad));
        Button padButton = new Button(activity);
        padButton.setText(strings.getString(R.string.touch_pad_remap));
        padButton.setAllCaps(false);
        content.addView(padButton);
        Runnable applyLook = () -> {
            lookDrag = lookDragBox.isChecked();
            lookSpeed = (lookSpeedBar.getProgress() + 25) / 100f;
            preferences.edit().putBoolean("look_drag", lookDrag).putFloat("look_speed", lookSpeed).apply();
        };
        SharedPreferences render = RuntimeActivity.renderPrefs(activity);
        // Frame rate: 30 / 40 / 60, and the 120 Hz panel for a smoother 60 (RuntimeActivity.applyFrameRate).
        String fps = render.getString("fps", RuntimeActivity.DEFAULT_FPS);
        RadioButton fpsChoice = radio(content, strings.getString(R.string.touch_frame_rate),
            strings.getString(R.string.touch_fps_30), strings.getString(R.string.touch_fps_40),
            strings.getString(R.string.touch_fps_60), "30".equals(fps) ? 0 : "40".equals(fps) ? 1 : 2);
        CheckBox panel120 = option(content, strings.getString(R.string.touch_panel120),
            strings.getString(R.string.touch_panel120_help), render.getBoolean("panel120", false));
        // Game language (from the next start, RuntimeActivity.addLanguageArguments): the device's language or one
        // of the game's, each written in its own words.
        header(content, strings.getString(R.string.touch_section_language));
        String[] languageNames = RuntimeActivity.LANGUAGE_NAMES.clone();
        languageNames[0] = strings.getString(R.string.touch_language_device,
            RuntimeActivity.languageName(RuntimeActivity.deviceLanguageId()));
        Spinner language = new Spinner(activity);
        ArrayAdapter<String> languageAdapter =
            new ArrayAdapter<>(activity, android.R.layout.simple_spinner_item, languageNames);
        languageAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        language.setAdapter(languageAdapter);
        language.setSelection(Math.max(0, java.util.Arrays.asList(RuntimeActivity.LANGUAGE_IDS)
            .indexOf(render.getString("language", RuntimeActivity.DEFAULT_LANGUAGE))));
        content.addView(language);
        // Display (from the next start, RuntimeActivity.addDisplayArguments): render resolution, FSR upscaling.
        String renderHeight = render.getString("render_height", RuntimeActivity.DEFAULT_RENDER_HEIGHT);
        header(content, strings.getString(R.string.touch_section_image));
        TextView resolutionLabel = new TextView(activity);
        resolutionLabel.setText(strings.getString(R.string.touch_resolution));
        content.addView(resolutionLabel);
        // Fill screen (RuntimeActivity.wideWidth), first in the resolution section and off by default (the owner,
        // 2026-11-21): only offered on displays wider than 16:9.
        CheckBox wide = option(content, strings.getString(R.string.touch_wide),
            strings.getString(R.string.touch_wide_help), render.getBoolean("wide", false));
        if (activity.displayAspect() <= 16f / 9f + 0.01f) {
            wide.setEnabled(false);
            wide.setText(strings.getString(R.string.touch_wide) + " " + strings.getString(R.string.touch_wide_na));
        }
        java.util.List<String> heights = java.util.Arrays.asList(RuntimeActivity.RENDER_HEIGHTS);
        RadioButton resolutionChoice = radio(content, null, Math.max(0, heights.indexOf(renderHeight)),
            strings.getString(R.string.touch_resolution_540), strings.getString(R.string.touch_resolution_720),
            strings.getString(R.string.touch_resolution_1080), strings.getString(R.string.touch_resolution_1440));
        // Fill screen follows the resolution (the owner, 2026-11-21: "if someone select unsupported resolution it should
        // untick/grey automatically"): greyed out and unticked where RuntimeActivity.wideSupported says no (since
        // 2026-11-22 every resolution is supported, so only a 16:9 display greys it out).
        final boolean wideDisplay = activity.displayAspect() > 16f / 9f + 0.01f;
        Runnable syncWide = () -> {
            boolean supported = wideDisplay
                && RuntimeActivity.wideSupported(RuntimeActivity.RENDER_HEIGHTS[radioChoice(resolutionChoice)]);
            if (!supported) wide.setChecked(false);
            wide.setEnabled(supported);
        };
        ((RadioGroup) resolutionChoice.getParent()).setOnCheckedChangeListener((group, id) -> syncWide.run());
        syncWide.run();
        CheckBox fsr = option(content, strings.getString(R.string.touch_fsr),
            strings.getString(R.string.touch_fsr_help), render.getBoolean("fsr", false));
        // Anti-aliasing (RuntimeActivity.aaMode): FXAA on the 3D scene before the game's 2D layer (vulkan_scene_fxaa),
        // applied at once; the HUD's and menus' text stays sharp. Hidden while RuntimeActivity.SHOW_AA is false.
        RadioButton aaChoice = null;
        if (RuntimeActivity.SHOW_AA) {
            TextView aaLabel = new TextView(activity);
            aaLabel.setText(strings.getString(R.string.touch_aa));
            content.addView(aaLabel);
            String aa = render.getString("aa", "off");
            aaChoice = radio(content, null, strings.getString(R.string.touch_aa_off),
                strings.getString(R.string.touch_aa_fxaa), strings.getString(R.string.touch_aa_fxaa_strong),
                "fxaa".equals(aa) ? 1 : "fxaa_extreme".equals(aa) ? 2 : 0);
        }
        final RadioButton aaRadio = aaChoice;
        // The game's depth of field (off by default: it blurs the character on this renderer and costs frames).
        CheckBox depthOfField = option(content, strings.getString(R.string.touch_depth_of_field),
            strings.getString(R.string.touch_depth_of_field_help), render.getBoolean("dof", false));
        // Second screen (dual-screen devices only, CompanionScreen): the game's HUD on the bottom screen, applied at once.
        CheckBox companionBox = null, companionHideBox = null;
        if (CompanionScreen.available(activity)) {
            header(content, strings.getString(R.string.touch_section_second_screen));
            companionBox = option(content, strings.getString(R.string.touch_companion),
                strings.getString(R.string.touch_companion_help), CompanionScreen.enabled(render));
            companionHideBox = option(content, strings.getString(R.string.touch_companion_hide),
                strings.getString(R.string.touch_companion_hide_help), CompanionScreen.hideOnMain(render));
            final CheckBox hideBox = companionHideBox;
            companionBox.setOnCheckedChangeListener((b, checked) -> hideBox.setEnabled(checked));
            companionHideBox.setEnabled(companionBox.isChecked());
        }
        final CheckBox companion = companionBox, companionHide = companionHideBox;
        // Prime core (on by default since 2026-11-20, the owner: "smoother, even with a bit more prime core"): the
        // game's main thread, the GPU command thread and the draw thread may also run on the fastest core. Applied at
        // once.
        header(content, strings.getString(R.string.touch_section_performance));
        CheckBox primeCore = option(content, strings.getString(R.string.touch_prime_core),
            strings.getString(R.string.touch_prime_core_help), render.getBoolean("prime", RuntimeActivity.DEFAULT_PRIME));

        // Cheats (the owner, 2026-11-21; ds2_cheats.cpp): applied at once, remembered for the next start.
        header(content, strings.getString(R.string.touch_section_cheats));
        CheckBox cheatHp = option(content, strings.getString(R.string.touch_cheat_hp), null,
            render.getBoolean("cheat_hp", false));
        CheckBox cheatStamina = option(content, strings.getString(R.string.touch_cheat_stamina), null,
            render.getBoolean("cheat_stamina", false));
        CheckBox cheatWear = option(content, strings.getString(R.string.touch_cheat_wear), null,
            render.getBoolean("cheat_wear", false));
        Button addSouls = new Button(activity);
        addSouls.setText(strings.getString(R.string.touch_cheat_add_souls));
        addSouls.setAllCaps(false);
        addSouls.setOnClickListener(v -> {
            RuntimeActivity.nativeSetOption("cheat_add_souls", "10000");
            android.widget.Toast.makeText(activity, strings.getString(R.string.touch_cheat_souls_added),
                android.widget.Toast.LENGTH_SHORT).show();
        });
        content.addView(addSouls);

        // Troubleshooting. Diagnostics (the owner, 2026-11-11: off for regular play): read at start.
        header(content, strings.getString(R.string.touch_section_troubleshooting));
        CheckBox debugLog = option(content, strings.getString(R.string.touch_debug_log),
            strings.getString(R.string.touch_debug_log_help),
            render.getBoolean("debug_log", RuntimeActivity.DEFAULT_DEBUG_LOG));

        // Qualcomm and Mali devices: open the GPU driver page (download / switch Turnip or PanVK).
        Button driverButton = null;
        if (GpuDriverStore.supported()) {
            driverButton = new Button(activity);
            GpuDriverStore.Installed driver = GpuDriverStore.selectedDriver(activity);
            driverButton.setText(strings.getString(R.string.touch_gpu_driver,
                driver != null ? driver.metadata.name : strings.getString(R.string.touch_gpu_driver_system)));
            driverButton.setAllCaps(false);
            content.addView(driverButton);
        }
        Button folderButton = new Button(activity);
        folderButton.setText(strings.getString(R.string.touch_game_folder));
        folderButton.setAllCaps(false);
        content.addView(folderButton);
        // A zip of the game's logs through the system file picker, so a player can attach it to a bug report from the
        // phone (Android 11+ hides Android/data from file managers; LR's public #4, the owner's S25).
        Button logsButton = new Button(activity);
        logsButton.setText(strings.getString(R.string.touch_save_logs));
        logsButton.setAllCaps(false);
        content.addView(logsButton);
        // Copies of the save in a folder the player picks, outside the app (SaveBackups; off until a folder is chosen).
        Button backupsButton = new Button(activity);
        backupsButton.setText(strings.getString(R.string.touch_save_backups));
        backupsButton.setAllCaps(false);
        content.addView(backupsButton);
        // GPU compatibility (testing): renderer paths for GPUs that draw the game wrongly (issues #8, #11).
        header(content, strings.getString(R.string.touch_section_compat));
        hint(content, strings.getString(R.string.touch_compat_help)).setPadding(0, 0, 0,
            Math.round(4f * getResources().getDisplayMetrics().density));
        CheckBox compatShortcuts = option(content, strings.getString(R.string.touch_compat_shortcuts),
            strings.getString(R.string.touch_compat_shortcuts_help), render.getBoolean("compat_shortcuts", false));
        // The 1080p flash bisect switches (2026-11-22): development builds only (RuntimeActivity ignores them in
        // release builds too).
        boolean bisect = BuildConfig.DEBUG;
        CheckBox compatShortcutsTransfers = !bisect ? null : option(content,
            strings.getString(R.string.touch_compat_shortcuts_transfers),
            strings.getString(R.string.touch_compat_shortcuts_part_help),
            render.getBoolean("compat_shortcuts_transfers", false));
        CheckBox compatShortcutsResolves = !bisect ? null : option(content,
            strings.getString(R.string.touch_compat_shortcuts_resolves),
            strings.getString(R.string.touch_compat_shortcuts_part_help),
            render.getBoolean("compat_shortcuts_resolves", false));
        CheckBox compatFeedOff = !bisect ? null : option(content, strings.getString(R.string.touch_compat_feed_off),
            strings.getString(R.string.touch_compat_resolve_part_help), render.getBoolean("compat_feed_off", false));
        CheckBox compatLazyOff = !bisect ? null : option(content, strings.getString(R.string.touch_compat_lazy_off),
            strings.getString(R.string.touch_compat_resolve_part_help), render.getBoolean("compat_lazy_off", false));
        CheckBox compat7e3Off = !bisect ? null : option(content, strings.getString(R.string.touch_compat_7e3_resolve_off),
            strings.getString(R.string.touch_compat_resolve_part_help),
            render.getBoolean("compat_7e3_resolve_off", false));
        CheckBox compatGpl = option(content, strings.getString(R.string.touch_compat_gpl),
            strings.getString(R.string.touch_compat_gpl_help), render.getBoolean("compat_gpl", false));
        CheckBox compatDynrender = option(content, strings.getString(R.string.touch_compat_dynrender),
            strings.getString(R.string.touch_compat_dynrender_help), render.getBoolean("compat_dynrender", false));
        TextView tuLabel = new TextView(activity);
        tuLabel.setText(strings.getString(R.string.touch_compat_tu_mode));
        content.addView(tuLabel);
        final String[] tuModes = { "", "sysmem", "gmem" };
        int tuMode = java.util.Arrays.asList(tuModes).indexOf(render.getString("compat_tu_mode", ""));
        RadioButton compatTuMode = radio(content, null, Math.max(tuMode, 0),
            strings.getString(R.string.touch_compat_tu_auto), strings.getString(R.string.touch_compat_tu_sysmem),
            strings.getString(R.string.touch_compat_tu_gmem));
        CheckBox compatNoUbwc = option(content, strings.getString(R.string.touch_compat_tu_noubwc), null,
            render.getBoolean("compat_tu_noubwc", false));
        CheckBox compatTuReserve = option(content, strings.getString(R.string.touch_compat_tu_reserve), null,
            render.getBoolean("compat_tu_reserve", false));
        CheckBox compatShader = option(content, strings.getString(R.string.touch_compat_shader),
            strings.getString(R.string.touch_compat_shader_help), render.getBoolean("compat_shader", false));
        CheckBox compatDecodeBc = option(content, strings.getString(R.string.touch_compat_decode_bc),
            strings.getString(R.string.touch_compat_decode_bc_help), render.getBoolean("compat_decode_bc", false));
        CheckBox compatHdr = option(content, strings.getString(R.string.touch_compat_hdr),
            strings.getString(R.string.touch_compat_hdr_help), render.getBoolean("compat_hdr_r11g11b10", false));
        TextView nextStart = hint(content, strings.getString(R.string.touch_next_start_note));
        nextStart.setPadding(0, Math.round(8f * getResources().getDisplayMetrics().density), 0,
                             Math.round(8f * getResources().getDisplayMetrics().density));

        // Performance overlay (fps, 1% low, frame time graph of the game's own frames): applied at once. Last in the
        // dialog with a warning (the owner, 2026-11-21): it is an Android view over the game, which some phones'
        // display hardware hands to the GPU (issue #2: lost GPU device on an Adreno 825; the owner's S25 smoother
        // without it). DECISIONS.md, 2026-11-21 morning.
        CheckBox perfOverlay = option(content, strings.getString(R.string.touch_perf_overlay), null,
            render.getBoolean("overlay", false));
        TextView overlayWarning = hint(content, strings.getString(R.string.touch_perf_overlay_help));
        overlayWarning.setTextColor(Color.rgb(255, 196, 80));
        overlayWarning.setAlpha(1f);
        overlayWarning.setPadding(overlayWarning.getPaddingLeft(), 0, 0,
                                  Math.round(8f * getResources().getDisplayMetrics().density));

        ScrollView scroll = new ScrollView(activity);
        scroll.addView(content);
        // The title row with Exit at its right (the owner, 2026-11-22: "an exit button top right in the ctrl menu").
        LinearLayout titleRow = new LinearLayout(activity);
        titleRow.setOrientation(LinearLayout.HORIZONTAL);
        titleRow.setGravity(android.view.Gravity.CENTER_VERTICAL);
        int titlePad = Math.round(24f * getResources().getDisplayMetrics().density);
        titleRow.setPadding(titlePad, Math.round(18f * getResources().getDisplayMetrics().density),
                            Math.round(12f * getResources().getDisplayMetrics().density), 0);
        TextView titleText = new TextView(activity);
        titleText.setTextAppearance(android.R.style.TextAppearance_DeviceDefault_DialogWindowTitle);
        titleText.setText(strings.getString(R.string.touch_settings_title));
        titleRow.addView(titleText, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        Button exitButton = new Button(activity, null, android.R.attr.borderlessButtonStyle);
        exitButton.setText(strings.getString(R.string.exit_game));
        exitButton.setTextColor(Color.rgb(255, 140, 120));
        titleRow.addView(exitButton, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        AlertDialog settingsDialog = new AlertDialog.Builder(activity)
            .setCustomTitle(titleRow)
            .setView(scroll)
            .setPositiveButton(strings.getString(R.string.touch_apply), (dialog, which) -> {
                applyLook.run();
                applySettings(toggle.isChecked(), size.getProgress() + 60,
                              opacity.getProgress() + 25, ctrlAutoHide.isChecked(), hideAll.isChecked());
                final String nextStartBefore = RuntimeActivity.nextStartState(RuntimeActivity.renderPrefs(activity));
                RuntimeActivity.renderPrefs(activity).edit()
                    .putBoolean("debug_log", debugLog.isChecked())
                    .putBoolean("overlay", perfOverlay.isChecked())
                    .putBoolean("dof", depthOfField.isChecked())
                    .putString("render_height", RuntimeActivity.RENDER_HEIGHTS[radioChoice(resolutionChoice)])
                    .putBoolean("fsr", fsr.isChecked())
                    .putBoolean("wide", wide.isChecked())
                    .putString("aa", aaRadio == null ? "off"
                        : new String[] { "off", "fxaa", "fxaa_extreme" }[radioChoice(aaRadio)])
                    .putString("language", RuntimeActivity.LANGUAGE_IDS[language.getSelectedItemPosition()])
                    .putBoolean("cheat_hp", cheatHp.isChecked())
                    .putBoolean("cheat_stamina", cheatStamina.isChecked())
                    .putBoolean("cheat_wear", cheatWear.isChecked())
                    .putBoolean("compat_shortcuts", compatShortcuts.isChecked())
                    .putBoolean("compat_shortcuts_transfers", checked(compatShortcutsTransfers))
                    .putBoolean("compat_shortcuts_resolves", checked(compatShortcutsResolves))
                    .putBoolean("compat_feed_off", checked(compatFeedOff))
                    .putBoolean("compat_lazy_off", checked(compatLazyOff))
                    .putBoolean("compat_7e3_resolve_off", checked(compat7e3Off))
                    .putBoolean("compat_gpl", compatGpl.isChecked())
                    .putBoolean("compat_dynrender", compatDynrender.isChecked())
                    .putString("compat_tu_mode", tuModes[radioChoice(compatTuMode)])
                    .putBoolean("compat_tu_noubwc", compatNoUbwc.isChecked())
                    .putBoolean("compat_tu_reserve", compatTuReserve.isChecked())
                    .putBoolean("compat_shader", compatShader.isChecked())
                    .putBoolean("compat_decode_bc", compatDecodeBc.isChecked())
                    .putBoolean("compat_hdr_r11g11b10", compatHdr.isChecked())
                    .putBoolean("prime", primeCore.isChecked()).commit();  // on disk before a restart below
                RuntimeActivity.nativeSetOption("cheat_hp", cheatHp.isChecked() ? "1" : "0");
                RuntimeActivity.nativeSetOption("cheat_stamina", cheatStamina.isChecked() ? "1" : "0");
                RuntimeActivity.nativeSetOption("cheat_wear", cheatWear.isChecked() ? "1" : "0");
                RuntimeActivity.nativeSetOption("prime", primeCore.isChecked() ? "1" : "0");
                activity.setPerfOverlay(perfOverlay.isChecked());
                if (companion != null) activity.applyCompanionSetting(companion.isChecked(), companionHide.isChecked());
                RuntimeActivity.nativeSetOption("dof", depthOfField.isChecked() ? "1" : "0");
                RuntimeActivity.nativeSetOption("aa", aaRadio == null ? "off"
                    : new String[] { "off", "fxaa", "fxaa_extreme" }[radioChoice(aaRadio)]);
                activity.applyFrameRate(radioChoice(fpsChoice) == 0 ? "30" : radioChoice(fpsChoice) == 1 ? "40" : "60",
                                        panel120.isChecked());
                if (!nextStartBefore.equals(RuntimeActivity.nextStartState(RuntimeActivity.renderPrefs(activity))))
                    activity.offerRestart();
            })
            .setNeutralButton(strings.getString(R.string.touch_edit_layout), (dialog, which) -> {
                applyLook.run();
                applySettings(toggle.isChecked(), size.getProgress() + 60,
                              opacity.getProgress() + 25, ctrlAutoHide.isChecked(), hideAll.isChecked());
                enterEditor();
            })
            .setNegativeButton(strings.getString(R.string.touch_cancel), null)
            .create();
        settingsDialog.setOnDismissListener(ignored -> {
            settingsToggle = null;
            controllerHint = null;
            settingsChoiceEdited = false;
            settingsDialogOpen = false;
            syncCtrlPin();
        });
        if (driverButton != null) {
            driverButton.setOnClickListener(v -> {
                settingsDialog.dismiss();
                activity.openGpuDriverPage();
            });
        }
        folderButton.setOnClickListener(v -> {
            settingsDialog.dismiss();
            activity.openGameFolderPage();
        });
        logsButton.setOnClickListener(v -> {
            settingsDialog.dismiss();
            activity.startLogExport();
        });
        backupsButton.setOnClickListener(v -> {
            settingsDialog.dismiss();
            activity.openSaveBackupsPage();
        });
        padButton.setOnClickListener(v -> {
            settingsDialog.dismiss();
            PadRemap.show(activity);
        });
        exitButton.setOnClickListener(v -> {
            settingsDialog.dismiss();
            activity.confirmExit();
        });
        settingsDialogOpen = true;
        syncCtrlPin();
        settingsDialog.show();
    }

    private interface ProgressChanged { void update(int value); }

    private static SeekBar.OnSeekBarChangeListener seekListener(ProgressChanged listener) {
        return new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar seekBar, int progress,
                                                     boolean fromUser) {
                listener.update(progress);
            }
            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) {}
        };
    }

    private void enterEditor() {
        clearTouches();
        draft = layout.copy();
        draft.clampAll(getWidth(), getHeight(), baseUnit * draft.size,
                       editorReservedBottom());
        editing = true;
        selected = -1;
        syncCtrlPin();
        publish();
    }

    private void leaveEditor(boolean save) {
        clearTouches();
        if (save) {
            layout = draft.copy();
            layout.clampAll(getWidth(), getHeight(), baseUnit * layout.size, 0f);
            saveLayout();
        }
        editing = false;
        draft = null;
        selected = -1;
        syncCtrlPin();
        publish();
    }

    private void resetDraft() {
        dragPointer = -1;
        float size = draft.size, opacity = draft.opacity;
        draft = TouchControlLayout.defaults(getWidth(), getHeight(), baseUnit * size);
        draft.size = size;
        draft.opacity = opacity;
        draft.clampAll(getWidth(), getHeight(), baseUnit * size, editorReservedBottom());
        selected = -1;
        invalidate();
    }

    /** Editor toolbar: SAVE, CANCEL, RESET, HIDE / SHOW, smaller, bigger (the selected control). */
    private static final int TOOLBAR_BUTTONS = 6;
    private static float toolbarCenter(int i) { return .28f + i * .088f; }

    private int toolbarAt(float x, float y) {
        if (Math.abs(y - getHeight() * TOOLBAR_Y) > getHeight() * .052f) return -1;
        for (int i = 0; i < TOOLBAR_BUTTONS; ++i) {
            if (Math.abs(x - getWidth() * toolbarCenter(i)) <= getWidth() * .041f)
                return i;
        }
        return -1;
    }

    /** Edit layout's - / +: one step of the selected control's own size, kept on screen. */
    private void scaleSelected(int direction) {
        if (selected < 0 || selected == TouchControlLayout.CTRL) return;
        float next = Math.round((draft.scale[selected] + direction * TouchControlLayout.SCALE_STEP) * 10f) / 10f;
        draft.scale[selected] = TouchControlLayout.clampScale(next);
        draft.move(selected, px(selected), py(selected), getWidth(), getHeight(), unit(), editorReservedBottom());
        invalidate();
    }

    private boolean onEditorTouch(MotionEvent event, int action, int index, int id) {
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            float x = event.getX(index), y = event.getY(index);
            int toolbar = toolbarAt(x, y);
            if (toolbar >= 0) {
                if (toolbarPointer == -1) {
                    toolbarPointer = id;
                    toolbarPressed = toolbar;
                }
            } else if (dragPointer == -1) {
                // CTRL wins overlaps, as it does in play (#253).
                int element = onCtrl(x, y) ? TouchControlLayout.CTRL : elementAt(x, y, true);
                if (element == TouchControlLayout.CTRL) {
                    selected = element;
                    dragPointer = id;
                    dragOffsetX = settingX() - x;
                    dragOffsetY = settingY() - y;
                } else if (element >= 0) {
                    selected = element;
                    dragPointer = id;
                    dragOffsetX = px(element) - x;
                    dragOffsetY = py(element) - y;
                } else {
                    selected = -1;
                }
            }
            invalidate();
            return true;
        }
        if (action == MotionEvent.ACTION_MOVE) {
            if (dragPointer != -1) {
                for (int i = 0; i < event.getPointerCount(); ++i) {
                    if (event.getPointerId(i) == dragPointer) {
                        if (selected == TouchControlLayout.CTRL)
                            draft.moveCtrl(event.getX(i) + dragOffsetX,
                                           event.getY(i) + dragOffsetY, getWidth(),
                                           getHeight(), unit(), editorReservedBottom());
                        else
                            draft.move(selected, event.getX(i) + dragOffsetX,
                                       event.getY(i) + dragOffsetY, getWidth(), getHeight(),
                                       unit(), editorReservedBottom());
                        invalidate();
                        break;
                    }
                }
            }
            return true;
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            if (id == dragPointer) {
                dragPointer = -1;
                // CTRL dropped onto controls: move them clear so CTRL stays hittable.
                if (selected == TouchControlLayout.CTRL) {
                    draft.clampAll(getWidth(), getHeight(), unit(), editorReservedBottom());
                    invalidate();
                }
            }
            if (id == toolbarPointer) {
                int pressed = toolbarPressed;
                toolbarPointer = toolbarPressed = -1;
                if (pressed == toolbarAt(event.getX(index), event.getY(index))) {
                    if (pressed == 0) leaveEditor(true);
                    else if (pressed == 1) leaveEditor(false);
                    else if (pressed == 2) resetDraft();
                    else if (pressed == 3 && selected >= 0
                             && selected != TouchControlLayout.CTRL) {
                        dragPointer = -1;
                        draft.visible[selected] = !draft.visible[selected];
                        invalidate();
                    } else if (pressed == 4) scaleSelected(-1);
                    else if (pressed == 5) scaleSelected(1);
                }
            }
            return true;
        }
        if (action == MotionEvent.ACTION_CANCEL) {
            dragPointer = toolbarPointer = toolbarPressed = -1;
            return true;
        }
        return true;
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        // A mouse click belongs to SDL's pointer path, not to the virtual pad.
        if (!event.isFromSource(InputDevice.SOURCE_TOUCHSCREEN)
                && !event.isFromSource(InputDevice.SOURCE_STYLUS)) return false;
        if (event.getToolType(event.getActionIndex()) == MotionEvent.TOOL_TYPE_MOUSE) return false;
        if (layout == null) return false;
        int action = event.getActionMasked();
        int index = event.getActionIndex();
        int id = event.getPointerId(index);
        if (editing) return onEditorTouch(event, action, index, id);
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            float x = event.getX(index), y = event.getY(index);
            long now = SystemClock.uptimeMillis();
            layoutCtrl(now);
            boolean onTab = ctrl.retracted() && ctrl.hitTab(x, y);
            if (ctrl.visible(now) && settingsPointer == -1 && (onTab
                    || (!ctrl.retracted() && onCtrl(x, y)))) {
                // A touch on the retracted tab only brings CTRL back.
                settingsPointer = id;
                settingsPointerRestores = onTab;
                ctrl.activity(now);
                syncCtrlPin();
                return true;
            }
            if (!controlsVisible()) return action == MotionEvent.ACTION_POINTER_DOWN;
            int element = elementAt(x, y, false);
            if (element < 0) {
                if (lookDrag && lookPointer == -1 && x >= getWidth() * LOOK_MIN_X) {
                    lookPointer = id;
                    lookLastX = x;
                    lookLastY = y;
                    lookAccX = lookAccY = lookVelX = lookVelY = 0f;
                    lookTickNanos = System.nanoTime();
                    postLookTick();
                    return true;
                }
                return action == MotionEvent.ACTION_POINTER_DOWN;
            }
            pointers.put(id, new Pointer(element, x, y));
            publish();
            return true;
        }
        if (action == MotionEvent.ACTION_MOVE) {
            for (int i = 0; i < event.getPointerCount(); ++i) {
                int pointerId = event.getPointerId(i);
                if (pointerId == lookPointer) {
                    float x = event.getX(i), y = event.getY(i);
                    lookAccX += x - lookLastX;
                    lookAccY += y - lookLastY;
                    lookLastX = x;
                    lookLastY = y;
                    continue;
                }
                Pointer pointer = pointers.get(pointerId);
                if (pointer != null) { pointer.x = event.getX(i); pointer.y = event.getY(i); }
            }
            publish();
            return true;
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            if (id == lookPointer) endLook();
            pointers.remove(id);
            if (id == settingsPointer) {
                boolean restoring = settingsPointerRestores;
                settingsPointer = -1;
                settingsPointerRestores = false;
                ctrl.activity(SystemClock.uptimeMillis());
                syncCtrlPin();
                if (!restoring && onCtrl(event.getX(index), event.getY(index))) {
                    performClick();
                    showSettings();
                    return true;
                }
            }
            publish();
            return true;
        }
        if (action == MotionEvent.ACTION_CANCEL) {
            clearTouches();
            return true;
        }
        return super.onTouchEvent(event);
    }

    @Override
    public boolean performClick() {
        super.performClick();
        return true;
    }

    @Override
    protected void onDetachedFromWindow() {
        stopControllerWatch();
        cancelCtrlWake();
        clearTouches();
        super.onDetachedFromWindow();
    }

    private void color(int value, float opacity) {
        paint.setColor(Color.argb(Math.round(Color.alpha(value) * opacity),
                                  Color.red(value), Color.green(value), Color.blue(value)));
    }

    private static final int FILL_IDLE = Color.argb(90, 0, 0, 0);
    private static final int FILL_ACTIVE = Color.argb(190, 70, 159, 214);
    private static final int OUTLINE = Color.argb(210, 222, 239, 249);

    /** Xbox 360 face button letter colours, a little softened so they read on the dark fill. */
    private static int labelColor(int id) {
        switch (id) {
            case TouchControlLayout.A: return Color.rgb(120, 205, 90);
            case TouchControlLayout.B: return Color.rgb(240, 100, 85);
            case TouchControlLayout.X: return Color.rgb(90, 160, 240);
            case TouchControlLayout.Y: return Color.rgb(245, 205, 60);
            default: return Color.WHITE;
        }
    }

    private void drawLabel(Canvas canvas, float x, float y, String label, float textSize, boolean bold,
                           int labelColor, float opacity) {
        paint.setStyle(Paint.Style.FILL);
        paint.setTextAlign(Paint.Align.CENTER);
        paint.setTextSize(textSize);
        paint.setFakeBoldText(bold);
        color(labelColor, opacity);
        canvas.drawText(label, x, y - (paint.ascent() + paint.descent()) * .5f, paint);
        paint.setFakeBoldText(false);
    }

    private void drawButton(Canvas canvas, float x, float y, float radius,
                            String label, boolean active, float opacity) {
        drawButton(canvas, x, y, radius, label, active, opacity, Color.WHITE);
    }

    private void drawButton(Canvas canvas, float x, float y, float radius,
                            String label, boolean active, float opacity, int labelColor) {
        paint.setStyle(Paint.Style.FILL);
        color(active ? FILL_ACTIVE : FILL_IDLE, opacity);
        canvas.drawCircle(x, y, radius, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(Math.max(2f, unit() * .015f));
        color(OUTLINE, opacity);
        canvas.drawCircle(x, y, radius, paint);
        drawLabel(canvas, x, y, label, radius * .578f, labelColor != Color.WHITE,
                  active ? Color.WHITE : labelColor, opacity);
    }

    /** A rounded rectangle with a centred label: shoulder buttons, Back and Start, d-pad squares. */
    private void drawRoundRect(Canvas canvas, float x, float y, float halfWidth, float halfHeight, float corner,
                               String label, float textSize, boolean active, float opacity) {
        paint.setStyle(Paint.Style.FILL);
        color(active ? FILL_ACTIVE : FILL_IDLE, opacity);
        canvas.drawRoundRect(x - halfWidth, y - halfHeight, x + halfWidth, y + halfHeight, corner, corner, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(Math.max(2f, unit() * .015f));
        color(OUTLINE, opacity);
        canvas.drawRoundRect(x - halfWidth, y - halfHeight, x + halfWidth, y + halfHeight, corner, corner, paint);
        if (!label.isEmpty()) drawLabel(canvas, x, y, label, textSize, true, Color.WHITE, opacity);
    }

    private final android.graphics.Path arrow = new android.graphics.Path();

    /** One d-pad square with a triangle pointing away from the centre (dirX, dirY: -1, 0 or 1). */
    private void drawDpadSquare(Canvas canvas, float x, float y, float half, int dirX, int dirY,
                                boolean active, float opacity) {
        drawRoundRect(canvas, x, y, half, half, half * .3f, "", 1f, active, opacity);
        float size = half * .48f;
        // Tip towards (dirX, dirY), base across the other axis.
        arrow.reset();
        arrow.moveTo(x + dirX * size, y + dirY * size);
        arrow.lineTo(x - dirX * size * .6f + dirY * size, y - dirY * size * .6f + dirX * size);
        arrow.lineTo(x - dirX * size * .6f - dirY * size, y - dirY * size * .6f - dirX * size);
        arrow.close();
        paint.setStyle(Paint.Style.FILL);
        color(Color.WHITE, opacity);
        canvas.drawPath(arrow, paint);
    }

    private void drawStick(Canvas canvas, float x, float y, float unit, int axisX, int axisY,
                           float opacity) {
        paint.setStyle(Paint.Style.FILL);
        color(Color.argb(72, 0, 0, 0), opacity);
        canvas.drawCircle(x, y, unit, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(Math.max(2f, unit * .015f));
        color(OUTLINE, opacity);
        canvas.drawCircle(x, y, unit, paint);
        color(Color.argb(92, 222, 239, 249), opacity);
        canvas.drawCircle(x, y, unit * .68f, paint);
        paint.setStyle(Paint.Style.FILL);
        color(axisX != 0 || axisY != 0
            ? Color.argb(220, 70, 159, 214) : Color.argb(180, 175, 185, 191), opacity);
        canvas.drawCircle(x + axisX / 32767f * unit * .55f,
                          y - axisY / 32767f * unit * .55f, unit * .40f, paint);
    }

    private void drawElement(Canvas canvas, int id) {
        TouchControlLayout controls = activeLayout();
        if (!editing && !controls.visible[id]) return;
        float opacity = editing ? (controls.visible[id] ? Math.max(.70f,
            controls.opacity) : .40f) : controls.opacity;
        float x = px(id), y = py(id), unit = elementUnit(id);
        switch (id) {
            case TouchControlLayout.LEFT_STICK:
                drawStick(canvas, x, y, unit, shownLx, shownLy, opacity); break;
            case TouchControlLayout.RIGHT_STICK:
                drawStick(canvas, x, y, unit, shownRx, shownRy, opacity); break;
            case TouchControlLayout.DPAD: {
                float spread = TouchControlLayout.DPAD_SPREAD * unit, half = TouchControlLayout.DPAD_HALF * unit;
                drawDpadSquare(canvas, x, y - spread, half, 0, -1, (shownButtons & UP) != 0, opacity);
                drawDpadSquare(canvas, x, y + spread, half, 0, 1, (shownButtons & DOWN) != 0, opacity);
                drawDpadSquare(canvas, x - spread, y, half, -1, 0, (shownButtons & LEFT) != 0, opacity);
                drawDpadSquare(canvas, x + spread, y, half, 1, 0, (shownButtons & RIGHT) != 0, opacity);
                break;
            }
            case TouchControlLayout.BACK:
            case TouchControlLayout.START:
                drawRoundRect(canvas, x, y, unit * .42f, unit * .22f, unit * .22f, NAMES[id], unit * .20f,
                              (shownButtons & buttonMask(id)) != 0, opacity);
                break;
            case TouchControlLayout.LB:
            case TouchControlLayout.LT:
            case TouchControlLayout.RB:
            case TouchControlLayout.RT: {
                boolean active = id == TouchControlLayout.LT ? shownLt != 0
                    : id == TouchControlLayout.RT ? shownRt != 0
                    : (shownButtons & buttonMask(id)) != 0;
                drawRoundRect(canvas, x, y, unit * .76f, unit * .34f, unit * .16f, NAMES[id], unit * .30f,
                              active, opacity);
                break;
            }
            case TouchControlLayout.L3:
            case TouchControlLayout.R3:
                drawButton(canvas, x, y, unit * .38f, NAMES[id], (shownButtons & buttonMask(id)) != 0, opacity);
                break;
            default:
                drawButton(canvas, x, y, unit * .45f, NAMES[id], (shownButtons & buttonMask(id)) != 0, opacity,
                           labelColor(id));
                break;
        }
        if (editing && id == selected) {
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(Math.max(3f, unit * .025f));
            color(Color.argb(230, 255, 195, 67), 1f);
            if (TouchControlLayout.isRect(id)) {
                float halfWidth = TouchControlLayout.hitHalfWidth(id) * unit * 1.08f;
                float halfHeight = TouchControlLayout.hitHalfHeight(id) * unit * 1.15f;
                canvas.drawRoundRect(x - halfWidth, y - halfHeight, x + halfWidth, y + halfHeight,
                                     unit * .2f, unit * .2f, paint);
            } else {
                canvas.drawCircle(x, y, TouchControlLayout.hitRadius(id) * unit * 1.08f, paint);
            }
        }
    }

    private void drawEditorToolbar(Canvas canvas) {
        boolean ctrlSelected = selected == TouchControlLayout.CTRL;
        String description = selected < 0 ? "Drag a control · tap to select"
            : ctrlSelected ? "CTRL · always shown"
            : NAMES[selected] + (draft.visible[selected] ? " · visible" : " · hidden")
                + " · size " + Math.round(draft.scale[selected] * 100f) + "%";
        paint.setStyle(Paint.Style.FILL);
        paint.setTextAlign(Paint.Align.CENTER);
        paint.setTextSize(Math.max(18f, unit() * .16f));
        paint.setFakeBoldText(true);
        color(Color.argb(240, 255, 255, 255), 1f);
        canvas.drawText(description, getWidth() * .5f, getHeight() * TOOLBAR_LABEL_Y, paint);
        for (int i = 0; i < TOOLBAR_BUTTONS; ++i) {
            float x = getWidth() * toolbarCenter(i);
            float y = getHeight() * TOOLBAR_Y;
            float halfWidth = getWidth() * .041f;
            float halfHeight = getHeight() * .045f;
            paint.setStyle(Paint.Style.FILL);
            color(Color.argb(i == toolbarPressed ? 220 : 185, 22, 33, 43), 1f);
            canvas.drawRoundRect(x - halfWidth, y - halfHeight, x + halfWidth,
                                 y + halfHeight, halfHeight * .45f,
                                 halfHeight * .45f, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(Math.max(2f, unit() * .012f));
            color(Color.argb(220, 222, 239, 249), 1f);
            canvas.drawRoundRect(x - halfWidth, y - halfHeight, x + halfWidth,
                                 y + halfHeight, halfHeight * .45f,
                                 halfHeight * .45f, paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setTextSize(Math.max(11f * getResources().getDisplayMetrics().density,
                Math.min(unit() * .18f, getWidth() * .019f)));
            paint.setFakeBoldText(true);
            color(Color.WHITE, i >= 3 && (selected < 0 || ctrlSelected) ? .45f : 1f);
            String label = i == 0 ? "SAVE" : i == 1 ? "CANCEL" : i == 2 ? "RESET"
                : i == 4 ? "SIZE −" : i == 5 ? "SIZE +"
                : selected >= 0 && !ctrlSelected && !draft.visible[selected] ? "SHOW" : "HIDE";
            canvas.drawText(label, x, y - (paint.ascent() + paint.descent()) * .5f, paint);
        }
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (baseUnit <= 0f || activeLayout() == null) return;
        if (controlsVisible() || editing) {
            for (int id = 0; id < TouchControlLayout.COUNT; ++id) drawElement(canvas, id);
        }
        if (editing) {
            drawEditorCtrl(canvas);
            drawEditorToolbar(canvas);
        } else {
            drawCtrl(canvas);
        }
    }

    private void drawEditorCtrl(Canvas canvas) {
        float x = settingX(), y = settingY();
        drawButton(canvas, x, y, settingRadius(), "CTRL", false,
                   Math.max(.70f, draft.opacity));
        if (selected == TouchControlLayout.CTRL) {
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(Math.max(3f, unit() * .025f));
            color(Color.argb(230, 255, 195, 67), 1f);
            canvas.drawCircle(x, y, TouchControlLayout.CTRL_HIT_RADIUS * unit() * 1.08f, paint);
        }
    }

    private void drawCtrl(Canvas canvas) {
        long now = SystemClock.uptimeMillis();
        if (!ctrl.visible(now)) return;
        float progress = layoutCtrl(now);
        float radius = settingRadius();
        // Follows the layout's opacity slider (#253), fainter as a retracted tab.
        float opacity = layout.opacity * ctrl.opacity;
        boolean active = settingsPointer != -1 && !settingsPointerRestores;
        // The circle slides past the edge; the screen (or, at the top, the
        // status-bar strip) clips what is beyond it.
        canvas.save();
        canvas.clipRect(0f, ctrl.clipTop, getWidth(), getHeight());
        paint.setStyle(Paint.Style.FILL);
        color(active ? Color.argb(190, 70, 159, 214) : Color.argb(90, 0, 0, 0), opacity);
        canvas.drawCircle(ctrl.drawX, ctrl.drawY, radius, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(Math.max(2f, unit() * .015f));
        color(Color.argb(210, 222, 239, 249), opacity);
        canvas.drawCircle(ctrl.drawX, ctrl.drawY, radius, paint);
        paint.setStyle(Paint.Style.FILL);
        paint.setTextAlign(Paint.Align.CENTER);
        paint.setTextSize(unit() * .26f * ctrl.labelScale);
        paint.setFakeBoldText(progress > .5f);
        color(Color.WHITE, Math.min(1f, opacity * 1.5f));
        float baseline = ctrl.labelY - (paint.ascent() + paint.descent()) * .5f;
        if (ctrl.labelRotation != 0f) {
            canvas.save();
            canvas.rotate(ctrl.labelRotation, ctrl.labelX, ctrl.labelY);
            canvas.drawText("CTRL", ctrl.labelX, baseline, paint);
            canvas.restore();
        } else {
            canvas.drawText("CTRL", ctrl.labelX, baseline, paint);
        }
        canvas.restore();
        paint.setFakeBoldText(false);
        scheduleCtrlWake(now);
    }
}

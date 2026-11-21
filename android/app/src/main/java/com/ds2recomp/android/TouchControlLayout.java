// Adapted from Lost Odyssey Recomp (GPL-3.0).
package com.ds2recomp.android;

/** Screen-relative positions for the fifteen movable parts of the virtual pad. */
final class TouchControlLayout {
    static final int LEFT_STICK = 0, RIGHT_STICK = 1, DPAD = 2;
    static final int A = 3, B = 4, X = 5, Y = 6, L3 = 7, R3 = 8;
    static final int LB = 9, LT = 10, BACK = 11, START = 12, RT = 13, RB = 14;
    static final int COUNT = 15;
    static final float MIN_SIZE = 0.60f, MAX_SIZE = 1.40f;
    static final float MIN_OPACITY = 0.25f, MAX_OPACITY = 1.0f;
    /** Per-control size (Edit layout's - / +), on top of the overall size. */
    static final float MIN_SCALE = 0.60f, MAX_SCALE = 1.60f, SCALE_STEP = 0.10f;
    /** Editor id of the CTRL button; it is not part of the pad arrays. */
    static final int CTRL = COUNT;
    static final float CTRL_HIT_RADIUS = .50f;
    /** Top strip kept clear because the SDL view can extend behind the status bar. */
    static final float TOP_SAFE = .06f;
    /** The editor's toolbar spans this part of the width (TouchControlsView.toolbarAt). */
    static final float TOOLBAR_LEFT = .237f, TOOLBAR_RIGHT = .763f;
    /** Face buttons' distance from the diamond's centre, in units. */
    static final float FACE_SPREAD = .91f;
    /** D-pad: the four squares' distance from its centre and their half side, in units. */
    static final float DPAD_SPREAD = .62f, DPAD_HALF = .28f;

    final float[] x = new float[COUNT];
    final float[] y = new float[COUNT];
    final float[] scale = new float[COUNT];
    final boolean[] visible = new boolean[COUNT];
    float ctrlX = .5f, ctrlY = .111f;
    float size = 1.0f;
    float opacity = 1.0f;

    /**
     * The default layout, like a phone-native port (the owner, 2026-11-21; reference out/design/touch_layout_reference.webp):
     * left stick bottom-left with L3 under it and the d-pad as four buttons beside it, the face buttons as a diamond
     * bottom-right with R3 (lock-on) and the right stick beside them, wide shoulder buttons in the top corners
     * (triggers above bumpers), Back and Start at the top centre around CTRL. Distances are in units (the stick's
     * radius) from the screen edges, so the layout keeps its shape on 16:9 and wider screens.
     */
    static TouchControlLayout defaults(int width, int height, float unit) {
        TouchControlLayout result = new TouchControlLayout();
        float w = Math.max(1, width), h = Math.max(1, height);
        float topRow = height * TOP_SAFE + .45f * unit;
        float secondRow = topRow + .95f * unit;
        result.set(LT, 1.06f * unit / w, topRow / h);
        result.set(LB, 1.06f * unit / w, secondRow / h);
        result.set(RT, 1 - 1.06f * unit / w, topRow / h);
        result.set(RB, 1 - 1.06f * unit / w, secondRow / h);
        result.set(BACK, .5f - 1.15f * unit / w, topRow / h);
        result.set(START, .5f + 1.15f * unit / w, topRow / h);
        result.set(LEFT_STICK, 1.80f * unit / w, 1 - 1.85f * unit / h);
        result.set(L3, 3.00f * unit / w, 1 - .55f * unit / h);
        result.set(DPAD, 4.40f * unit / w, 1 - 1.25f * unit / h);
        float faceX = 1 - 1.90f * unit / w, faceY = 1 - 1.90f * unit / h;
        float dx = FACE_SPREAD * unit / w, dy = FACE_SPREAD * unit / h;
        result.set(A, faceX, faceY + dy);
        result.set(B, faceX + dx, faceY);
        result.set(X, faceX - dx, faceY);
        result.set(Y, faceX, faceY - dy);
        result.set(R3, 1 - 3.00f * unit / w, 1 - .55f * unit / h);
        result.set(RIGHT_STICK, 1 - 4.60f * unit / w, 1 - 1.30f * unit / h);
        result.clampAll(width, height, unit, 0f);
        return result;
    }

    private void set(int id, float px, float py) {
        x[id] = px;
        y[id] = py;
        scale[id] = 1f;
        visible[id] = true;
    }

    TouchControlLayout copy() {
        TouchControlLayout result = new TouchControlLayout();
        System.arraycopy(x, 0, result.x, 0, COUNT);
        System.arraycopy(y, 0, result.y, 0, COUNT);
        System.arraycopy(scale, 0, result.scale, 0, COUNT);
        System.arraycopy(visible, 0, result.visible, 0, COUNT);
        result.ctrlX = ctrlX;
        result.ctrlY = ctrlY;
        result.size = size;
        result.opacity = opacity;
        return result;
    }

    float pixelX(int id, int width) { return x[id] * width; }
    float pixelY(int id, int height) { return y[id] * height; }

    static float clampScale(float value) {
        return Float.isFinite(value) ? Math.max(MIN_SCALE, Math.min(MAX_SCALE, value)) : 1f;
    }

    /** Shoulder buttons are wide rounded rectangles; everything else is round. */
    static boolean isRect(int id) { return id == LB || id == LT || id == RB || id == RT; }

    /** Keep the entire touch target visible; the editor reserves its bottom toolbar. */
    void move(int id, float px, float py, int width, int height, float unit,
              float reservedBottom) {
        float elementUnit = unit * scale[id];
        float marginX = Math.min(hitHalfWidth(id) * elementUnit, width * .45f);
        float marginY = Math.min(hitHalfHeight(id) * elementUnit, height * .45f);
        float toolbarSpace = px + marginX >= width * TOOLBAR_LEFT
            && px - marginX <= width * TOOLBAR_RIGHT ? reservedBottom : 0f;
        float maxY = Math.max(marginY, height - marginY - toolbarSpace);
        px = Math.max(marginX, Math.min(width - marginX, px));
        // The SDL view can extend behind the Android status bar.
        float minY = Math.min(maxY, marginY + height * TOP_SAFE);
        py = Math.max(minY, Math.min(maxY, py));
        // CTRL remains reachable even if a user drags a button into it (or CTRL onto
        // a button: clampAll then moves the button): take the nearest free spot
        // beside, below or above it.
        float settingsX = ctrlX * width, settingsY = ctrlY * height;
        float clearanceX = marginX + unit * .52f, clearanceY = marginY + unit * .52f;
        float dx = px - settingsX, dy = py - settingsY;
        if ((dx * dx) / (clearanceX * clearanceX) + (dy * dy) / (clearanceY * clearanceY) < 1f) {
            float below = settingsY + clearanceY, above = settingsY - clearanceY;
            float side = settingsX + (dx < 0f ? -clearanceX : clearanceX);
            float best = Float.POSITIVE_INFINITY, bestX = px, bestY = Math.min(maxY, below);
            if (below <= maxY) { best = Math.abs(below - py); bestY = below; }
            if (side >= marginX && side <= width - marginX && Math.abs(side - px) < best) {
                best = Math.abs(side - px); bestX = side; bestY = py;
            }
            if (above >= minY && Math.abs(above - py) < best) { bestX = px; bestY = above; }
            px = Math.max(marginX, Math.min(width - marginX, bestX));
            py = bestY;
        }
        x[id] = px / width;
        y[id] = py / height;
    }

    /** CTRL stays fully on screen, below the top strip and clear of the editor toolbar. */
    void moveCtrl(float px, float py, int width, int height, float unit,
                  float reservedBottom) {
        float margin = Math.min(CTRL_HIT_RADIUS * unit, Math.min(width, height) * .45f);
        float toolbarSpace = px + margin >= width * TOOLBAR_LEFT
            && px - margin <= width * TOOLBAR_RIGHT ? reservedBottom : 0f;
        float maxY = Math.max(margin, height - margin - toolbarSpace);
        float minY = Math.min(maxY, margin + height * TOP_SAFE);
        ctrlX = Math.max(margin, Math.min(width - margin, px)) / width;
        ctrlY = Math.max(minY, Math.min(maxY, py)) / height;
    }

    void clampAll(int width, int height, float unit, float reservedBottom) {
        moveCtrl(ctrlX * width, ctrlY * height, width, height, unit, reservedBottom);
        for (int id = 0; id < COUNT; ++id) {
            move(id, pixelX(id, width), pixelY(id, height), width, height,
                 unit, reservedBottom);
        }
    }

    /** Touch radius of a round control, in units (before its scale). */
    static float hitRadius(int id) {
        if (id == LEFT_STICK || id == RIGHT_STICK) return 1.04f;
        if (id == DPAD) return DPAD_SPREAD + DPAD_HALF + .12f;
        if (id == BACK || id == START) return .49f;
        if (id == L3 || id == R3) return .44f;
        return .50f;
    }

    /** Half the touch box of a control, in units (round controls: their radius). */
    static float hitHalfWidth(int id) { return isRect(id) ? .82f : hitRadius(id); }
    static float hitHalfHeight(int id) { return isRect(id) ? .40f : hitRadius(id); }
}

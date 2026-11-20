package com.ds2recomp.android;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.view.View;

import java.util.Arrays;

/**
 * Performance overlay (CTRL dialog: "Performance overlay"): frame rate, frame time, 1% low, slowest frame and a
 * frame time graph of the game's own presented frames, top right. Drawn by Android over the game four times a
 * second, so it never touches the game's renderer: the native ImGui overlay it replaces made the presenter paint
 * every frame from the UI thread (sdk/src/ui/rex_app.cpp), which cost frames in play (2026-11-20). The native one
 * stays for debug launches (`--es DS2_FRAME_OVERLAY 1`).
 */
final class PerfOverlayView extends View {
    private static final int MAX = 240;
    private static final long PERIOD_MS = 250;

    private final float[] ms = new float[MAX];
    private final float[] sorted = new float[MAX];
    private final Paint panel = new Paint();
    private final Paint graphBack = new Paint();
    private final Paint gridLine = new Paint();
    private final Paint bar = new Paint();
    private final Paint fpsText = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint detailText = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rect = new RectF();
    private boolean running;
    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            if (!running) return;
            invalidate();
            postDelayed(this, PERIOD_MS);
        }
    };

    PerfOverlayView(Context context) {
        super(context);
        setClickable(false);
        setFocusable(false);
        setWillNotDraw(false);
        panel.setColor(Color.argb(166, 15, 15, 15));
        graphBack.setColor(Color.argb(90, 0, 0, 0));
        gridLine.setColor(Color.argb(60, 255, 255, 255));
        fpsText.setTypeface(Typeface.DEFAULT_BOLD);
        detailText.setColor(Color.rgb(170, 170, 170));
    }

    /** Shows or hides the overlay (UI thread). */
    void setShown(boolean shown) {
        if (shown == running) return;
        running = shown;
        setVisibility(shown ? VISIBLE : GONE);
        removeCallbacks(tick);
        if (shown) post(tick);
    }

    @Override
    protected void onDetachedFromWindow() {
        running = false;
        removeCallbacks(tick);
        super.onDetachedFromWindow();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        int n;
        try {
            n = RuntimeActivity.nativeFrameTimes(ms);
        } catch (UnsatisfiedLinkError e) {
            return;
        }
        if (n < 2) return;
        n = Math.min(n, MAX);
        // Frame rate over the last half second; average and slowest frame of the graph; 1% low over the graph (the
        // mean of its slowest 1 % as fps), as the native overlay.
        double sum = 0;
        int count = 0;
        for (int i = n - 1; i >= 0 && sum < 500.0; --i, ++count) sum += ms[i];
        double fps = sum > 0 ? 1000.0 * count / sum : 0;
        float worst = 0;
        for (int i = 0; i < n; ++i) worst = Math.max(worst, ms[i]);
        System.arraycopy(ms, 0, sorted, 0, n);
        Arrays.sort(sorted, 0, n);
        int slow = Math.max(1, n / 100);
        double slowSum = 0;
        for (int i = 0; i < slow; ++i) slowSum += sorted[n - 1 - i];
        double low = slowSum > 0 ? 1000.0 * slow / slowSum : 0;

        float s = Math.max(1.0f, getHeight() / 1080.0f);
        float pad = 12 * s, inner = 8 * s;
        float graphW = 240 * s, graphH = 56 * s;
        fpsText.setTextSize(17 * s);
        detailText.setTextSize(15 * s);
        String fpsLabel = String.format(java.util.Locale.ROOT, "%.0f FPS", fps);
        String detail = String.format(java.util.Locale.ROOT, "  %.1f ms  1%% low %.0f  max %.0f ms",
            count > 0 ? sum / count : 0.0, low, worst);
        float textW = fpsText.measureText(fpsLabel) + detailText.measureText(detail);
        float w = Math.max(graphW, textW) + 2 * inner;
        float lineH = fpsText.getFontSpacing();
        float h = inner + lineH + 4 * s + graphH + inner;
        float left = getWidth() - pad - w, top = pad;
        rect.set(left, top, left + w, top + h);
        canvas.drawRoundRect(rect, 6 * s, 6 * s, panel);

        fpsText.setColor(fps >= 55 ? Color.rgb(120, 230, 120) : fps >= 28 ? Color.rgb(240, 220, 110)
                                                                         : Color.rgb(250, 110, 100));
        float baseline = top + inner - fpsText.ascent();
        canvas.drawText(fpsLabel, left + inner, baseline, fpsText);
        canvas.drawText(detail, left + inner + fpsText.measureText(fpsLabel), baseline, detailText);

        // One bar per frame, oldest left; lines at 16.7 ms (60 fps) and 33.3 ms (30 fps).
        float gx = left + inner, gy = top + inner + lineH + 4 * s;
        canvas.drawRect(gx, gy, gx + graphW, gy + graphH, graphBack);
        float scaleTop = Math.max(50.0f, worst * 1.1f);
        for (float line : new float[] {16.7f, 33.3f}) {
            float y = gy + graphH - Math.min(line, scaleTop) / scaleTop * graphH;
            canvas.drawLine(gx, y, gx + graphW, y, gridLine);
        }
        float bw = graphW / MAX;
        int first = MAX - n;
        for (int i = 0; i < n; ++i) {
            float v = ms[i];
            bar.setColor(v <= 17.5f ? Color.argb(220, 120, 230, 120)
                       : v <= 34.5f ? Color.argb(220, 240, 220, 110) : Color.argb(230, 250, 110, 100));
            float x = gx + (first + i) * bw;
            float y = gy + graphH - Math.min(v, scaleTop) / scaleTop * graphH;
            canvas.drawRect(x, y, x + Math.max(1.0f, bw - 0.5f), gy + graphH, bar);
        }
    }
}

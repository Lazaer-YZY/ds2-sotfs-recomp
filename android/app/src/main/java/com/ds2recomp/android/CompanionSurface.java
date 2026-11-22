package com.ds2recomp.android;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

/**
 * The bottom screen's drawing, kept away from the game (the Lightning Returns port's measured design): its own thread,
 * pinned to the little cores (the game's threads run on the middle and prime cores), Android's hardware canvas (a
 * dozen textured quads; the bitmaps upload once). It polls the game's state every POLL_MS (POLL_IDLE_MS without a
 * player) and redraws only when what is shown changed, plus ~30 fps while a bar eases. Scene changes (no player /
 * in game) dip through dark. A tap on an effect icon names it (CompanionView.tap).
 */
final class CompanionSurface extends SurfaceView implements SurfaceHolder.Callback {
    private static final String TAG = "ds2recomp";
    private static final long POLL_MS = 100;
    private static final long POLL_IDLE_MS = 250;
    private static final long FRAME_MS = 33;
    private static final long FADE_MS = 320;

    private final CompanionView renderer;
    private final HandlerThread thread;
    private final Handler worker;
    private final String demo;
    private final long demoStart = SystemClock.uptimeMillis();
    private final Object surfaceLock = new Object();
    private boolean hasSurface;  // under surfaceLock
    private boolean running, dirty = true, animating;  // worker thread only
    private Boolean lastInGame;
    private long fadeStart;
    private final Paint fadePaint = new Paint();
    // A touch that may be a tap (UI thread).
    private float downX, downY;
    private long downAt;
    // Draw-time statistics for the log (every 10 s while drawing).
    private long statSince, statNanos, statMax;
    private int statFrames;

    CompanionSurface(Context context, String demo) {
        super(context);
        this.demo = demo;
        renderer = new CompanionView(context);
        getHolder().setFormat(PixelFormat.OPAQUE);
        getHolder().addCallback(this);
        // Normal priority, pinned to the little cores: Android's background priority also put the LR port's thread in
        // a power-limited group whose cores stayed at a low clock.
        thread = new HandlerThread("DS2 companion", Process.THREAD_PRIORITY_DEFAULT);
        thread.start();
        worker = new Handler(thread.getLooper());
        worker.post(() -> Log.i(TAG, "companion: drawing thread " + RuntimeActivity.nativeCompanionPinLittleCores()));
    }

    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            if (!running) return;
            String line = demo != null ? CompanionState.demoLine(SystemClock.uptimeMillis() - demoStart, demo)
                                       : RuntimeActivity.nativeCompanionState();
            CompanionState st = CompanionState.parse(line);
            if (renderer.setState(st)) dirty = true;
            boolean inGame = renderer.inGame();  // what is drawn: a short gap without a player keeps the HUD
            if (lastInGame == null || lastInGame != inGame) {
                if (lastInGame != null) fadeStart = SystemClock.uptimeMillis();
                lastInGame = inGame;
                dirty = true;
            }
            if (!animating) frame.run();
            worker.postDelayed(this, st.player ? POLL_MS : POLL_IDLE_MS);
        }
    };

    // Eases the bars and draws; reschedules itself at FRAME_MS while anything moves or a fade runs.
    private final Runnable frame = new Runnable() {
        @Override
        public void run() {
            if (!running) {
                animating = false;
                return;
            }
            long now = SystemClock.uptimeMillis();
            boolean moving = renderer.animate(now);
            boolean fading = fadeStart != 0 && now - fadeStart < FADE_MS;
            if (moving || fading || dirty) present(now);
            animating = moving || fading;
            if (animating) worker.postDelayed(this, FRAME_MS);
        }
    };

    // Redraws once a tapped icon's label has run its time (worker thread).
    private final Runnable labelExpired = () -> {
        dirty = true;
        if (running && !animating) frame.run();
    };

    @Override
    public boolean onTouchEvent(android.view.MotionEvent e) {
        switch (e.getActionMasked()) {
            case android.view.MotionEvent.ACTION_DOWN:
                downX = e.getX();
                downY = e.getY();
                downAt = SystemClock.uptimeMillis();
                return true;
            case android.view.MotionEvent.ACTION_UP: {
                float slop = 24 * getResources().getDisplayMetrics().density;
                if (Math.abs(e.getX() - downX) > slop || Math.abs(e.getY() - downY) > slop
                        || SystemClock.uptimeMillis() - downAt > 800) return true;
                final float x = e.getX(), y = e.getY();
                worker.post(() -> {
                    if (!running || !renderer.tap(x, y, SystemClock.uptimeMillis())) return;
                    dirty = true;
                    if (!animating) frame.run();
                    worker.removeCallbacks(labelExpired);
                    worker.postDelayed(labelExpired, CompanionView.LABEL_MS + 50);
                });
                return true;
            }
            default:
                return true;
        }
    }

    void start() {
        worker.post(() -> {
            running = true;
            worker.removeCallbacks(tick);
            worker.post(tick);
        });
    }

    /** Stops drawing and ends the thread (the Presentation is going away). */
    void stop() {
        worker.post(() -> running = false);
        thread.quitSafely();
    }

    private void present(long now) {
        if (renderer.getWidth() <= 0 || renderer.getHeight() <= 0) return;
        float t = fadeStart == 0 ? 1f : Math.min(1f, (now - fadeStart) / (float) FADE_MS);
        synchronized (surfaceLock) {
            if (!hasSurface) return;
            Canvas c = null;
            try {
                c = getHolder().getSurface().lockHardwareCanvas();
                long t0 = System.nanoTime();
                renderer.draw(c);
                if (t < 1f) {
                    float k = 1f - t * t * (3 - 2 * t);
                    fadePaint.setColor(Color.argb(Math.round(230 * k), 0, 0, 0));
                    c.drawRect(0, 0, renderer.getWidth(), renderer.getHeight(), fadePaint);
                } else {
                    fadeStart = 0;
                }
                dirty = false;
                stat(System.nanoTime() - t0, now);
            } catch (RuntimeException e) {
                Log.w(TAG, "companion: draw failed", e);
            } finally {
                if (c != null) {
                    try {
                        getHolder().getSurface().unlockCanvasAndPost(c);
                    } catch (RuntimeException ignored) {
                    }
                }
            }
        }
    }

    /** Recording time per frame (the GPU work follows asynchronously), logged every 10 s while drawing. */
    private void stat(long nanos, long nowMs) {
        statNanos += nanos;
        statMax = Math.max(statMax, nanos);
        statFrames++;
        if (statSince == 0) statSince = nowMs;
        if (nowMs - statSince >= 10000) {
            Log.i(TAG, String.format(java.util.Locale.US, "companion: %d frames in %.1f s, record avg %.2f ms, max %.2f ms (%s)",
                statFrames, (nowMs - statSince) / 1000f, statNanos / 1e6 / statFrames, statMax / 1e6,
                renderer.inGame() ? "game" : "idle"));
            statSince = nowMs;
            statNanos = statMax = 0;
            statFrames = 0;
        }
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        synchronized (surfaceLock) {
            hasSurface = true;
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        worker.post(() -> {
            renderer.layout(width, height);
            dirty = true;
            if (!animating) frame.run();
        });
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        // Blocks until a draw in progress has posted: the surface must not be used after this returns.
        synchronized (surfaceLock) {
            hasSurface = false;
        }
    }
}

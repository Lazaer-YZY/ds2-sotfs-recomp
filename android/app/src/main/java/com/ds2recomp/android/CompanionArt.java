package com.ds2recomp.android;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffColorFilter;
import android.graphics.Rect;
import android.graphics.RectF;
import android.util.Log;
import android.util.SparseArray;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.InputStream;
import java.util.HashMap;
import java.util.Map;

/**
 * The bottom screen's art, cut from the player's own game files by tools/android/ds2_companion_assets.py (HUD pieces,
 * status and effect icons, the HUD's font, upscaled 4x) and bundled in development APKs under assets/companion/.
 * Pieces are addressed by name with their size in game pixels (hud.json), so layouts are written in the game's units.
 * Without bundled assets (public APKs) the same files come from the device: CompanionAssets builds them from the
 * player's game files into {@code <external files>/companion/}. Until then {@link #ok()} is false and CompanionView
 * draws plain shapes.
 */
final class CompanionArt {
    private static final String TAG = "ds2recomp";
    private static final String DIR = "companion/";

    private final Context context;
    private final java.io.File dir;  // generated art, or null for the APK's assets
    private final Map<String, int[]> sizes = new HashMap<>();
    private final Map<String, Bitmap> bitmaps = new HashMap<>();
    private final SparseArray<int[]> glyphs = new SparseArray<>();
    private final SparseArray<Bitmap> pages = new SparseArray<>();
    private final Map<Long, Integer> effectHashes = new HashMap<>();
    private final Map<Long, Integer> vowHashes = new HashMap<>();  // covenant icons (menu/tex/icon/vow/vi_*.tpf)
    private java.util.Set<Long> iconHashes;  // the icon pack's entries (development APKs), listed on first use
    private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
    private final Rect src = new Rect();
    private final RectF dst = new RectF();
    private boolean ok;

    CompanionArt(Context context) {
        this.context = context;
        this.dir = CompanionAssets.bundled(context) ? null
            : (CompanionAssets.ready(context) ? CompanionAssets.dir(context) : null);
        if (this.dir == null && !CompanionAssets.bundled(context)) {
            Log.i(TAG, "companion: no art yet (built from the game files in the background)");
            return;
        }
        try {
            JSONObject hud = new JSONObject(read("hud.json"));
            JSONObject pieces = hud.getJSONObject("pieces");
            for (java.util.Iterator<String> it = pieces.keys(); it.hasNext(); ) {
                String name = it.next();
                JSONArray wh = pieces.getJSONArray(name);
                sizes.put(name, new int[] { wh.getInt(0), wh.getInt(1) });
            }
            JSONObject eh = hud.optJSONObject("effect_hashes");
            if (eh != null) {
                for (java.util.Iterator<String> it = eh.keys(); it.hasNext(); ) {
                    String h = it.next();
                    effectHashes.put(Long.parseUnsignedLong(h, 16), eh.getInt(h));
                }
            }
            JSONObject vh = hud.optJSONObject("vow_hashes");  // art built before covenant icons has none
            if (vh != null) {
                for (java.util.Iterator<String> it = vh.keys(); it.hasNext(); ) {
                    String h = it.next();
                    vowHashes.put(Long.parseUnsignedLong(h, 16), vh.getInt(h));
                }
            }
            JSONObject font = new JSONObject(read("font.json"));
            JSONObject g = font.getJSONObject("glyphs");
            for (java.util.Iterator<String> it = g.keys(); it.hasNext(); ) {
                String code = it.next();
                JSONArray a = g.getJSONArray(code);
                int[] v = new int[a.length()];
                for (int i = 0; i < v.length; ++i) v[i] = a.getInt(i);
                glyphs.put(Integer.parseInt(code), v);  // page, x0, y0, x1, y1, pre, width, advance
            }
            ok = true;
        } catch (Exception e) {
            Log.i(TAG, "companion: no bundled art (" + e.getMessage() + "), plain drawing");
        }
    }

    boolean ok() { return ok; }

    private InputStream open(String name) throws java.io.IOException {
        return dir != null ? new java.io.FileInputStream(new java.io.File(dir, name)) : context.getAssets().open(DIR + name);
    }

    private String read(String name) throws java.io.IOException {
        try (InputStream in = open(name)) {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
            return out.toString("UTF-8");
        }
    }

    private Bitmap load(String path) {
        Bitmap b = bitmaps.get(path);
        if (b == null && !bitmaps.containsKey(path)) {
            try (InputStream in = open(path)) {
                b = BitmapFactory.decodeStream(in);
                // The art is 4x the game's size and drawn at ~1.5-2.5x: without mipmaps the GPU's bilinear filter
                // skips texels on such a downscale and edges and text look rough (the owner, 2026-11-22).
                if (b != null) b.setHasMipMap(true);
            } catch (java.io.IOException e) {
                Log.w(TAG, "companion: " + path + " missing");
            }
            bitmaps.put(path, b);
        }
        return b;
    }

    /** Size in game pixels, {0, 0} if unknown. */
    int[] size(String piece) {
        int[] s = sizes.get(piece);
        return s != null ? s : new int[] { 0, 0 };
    }

    Bitmap piece(String name) { return ok ? load("hud/" + name + ".png") : null; }

    /** The effect icon id of a 64x64 texture the game drew (by its data hash), or 0 if it is not an effect icon. */
    int effectId(long hash) {
        Integer id = effectHashes.get(hash);
        return id != null ? id : 0;
    }

    Bitmap effect(int id) { return ok ? load(String.format(java.util.Locale.US, "effect/%04d.png", id)) : null; }

    /** The covenant icon for the data hash the HP gauge's emblem drew, null if unknown (or art without them). */
    Bitmap covenant(long hash) {
        Integer id = hash != 0 ? vowHashes.get(hash) : null;
        return ok && id != null ? load(String.format(java.util.Locale.US, "vow/%04d.png", id)) : null;
    }

    /**
     * The icon pack's upscaled version of an item icon the HUD draws (by its data hash; ds2_companion_assets.py
     * --icons, development APKs only), null if there is none. Not cached here: CompanionIcons keeps the recent ones.
     */
    Bitmap icon(long hash) {
        if (!ok) return null;
        if (iconHashes == null) {
            iconHashes = new java.util.HashSet<>();
            try {
                String[] names = dir != null ? new java.io.File(dir, "icon").list() : context.getAssets().list(DIR + "icon");
                if (names != null)
                    for (String n : names)
                        if (n.endsWith(".webp")) iconHashes.add(Long.parseUnsignedLong(n.substring(0, n.length() - 5), 16));
            } catch (Exception e) {
                Log.w(TAG, "companion: icon pack unreadable (" + e.getMessage() + ")");
            }
            Log.i(TAG, "companion: icon pack " + iconHashes.size() + " icons");
        }
        if (!iconHashes.contains(hash)) return null;
        try (InputStream in = open(String.format(java.util.Locale.US, "icon/%016x.webp", hash))) {
            Bitmap b = BitmapFactory.decodeStream(in);
            if (b != null) b.setHasMipMap(true);
            return b;
        } catch (java.io.IOException e) {
            return null;
        }
    }

    /** Draws a piece into x, y, w, h (screen pixels); alpha 0-255. */
    void draw(Canvas c, String name, float x, float y, float w, float h, int alpha) {
        Bitmap b = piece(name);
        if (b == null) return;
        paint.setAlpha(alpha);
        paint.setColorFilter(null);
        dst.set(x, y, x + w, y + h);
        c.drawBitmap(b, null, dst, paint);
    }

    /**
     * Draws a piece stretched horizontally to w: its left and right ends (in game pixels) keep their shape, the middle
     * stretches. h is the drawn height.
     */
    void drawThreeSlice(Canvas c, String name, float x, float y, float w, float h, float left, float right) {
        Bitmap b = piece(name);
        int[] gs = size(name);
        if (b == null || gs[0] == 0) return;
        float k = b.getWidth() / (float) gs[0];  // bitmap pixels per game pixel
        float s = h / gs[1];                      // screen pixels per game pixel
        int l = Math.round(left * k), r = Math.round(right * k);
        paint.setAlpha(255);
        paint.setColorFilter(null);
        src.set(0, 0, l, b.getHeight());
        dst.set(x, y, x + left * s, y + h);
        c.drawBitmap(b, src, dst, paint);
        src.set(l, 0, b.getWidth() - r, b.getHeight());
        dst.set(x + left * s, y, x + w - right * s, y + h);
        c.drawBitmap(b, src, dst, paint);
        src.set(b.getWidth() - r, 0, b.getWidth(), b.getHeight());
        dst.set(x + w - right * s, y, x + w, y + h);
        c.drawBitmap(b, src, dst, paint);
    }

    /** Draws the first frac of a fill piece, squeezed into w * frac (the game's bars stretch their fill). */
    void drawFill(Canvas c, String name, float x, float y, float w, float h, float frac, int alpha) {
        if (frac <= 0) return;
        draw(c, name, x, y, w * Math.min(1f, frac), h, alpha);
    }

    /** Width of a text at a scale (1 = the game's 720p size), in screen pixels. */
    float textWidth(String s, float scale) {
        float w = 0;
        for (int i = 0; i < s.length(); ++i) {
            int[] g = glyphs.get(s.charAt(i));
            w += (g != null ? g[7] : 10) * scale;
        }
        return w;
    }

    /** Whether the game's font (the HUD's subset: Latin letters, digits) has every character of s. */
    boolean hasGlyphs(String s) {
        for (int i = 0; i < s.length(); ++i) if (glyphs.get(s.charAt(i)) == null) return false;
        return true;
    }

    /** Draws a text in the game's font with a drop shadow; align -1 left, 0 centre, 1 right of x. y is the top. */
    void text(Canvas c, String s, float x, float y, float scale, int color, int align) {
        if (align != -1) x -= textWidth(s, scale) * (align == 0 ? 0.5f : 1f);
        drawGlyphs(c, s, x + 2 * scale, y + 2 * scale, scale, Color.argb(200, 0, 0, 0));
        drawGlyphs(c, s, x, y, scale, color);
    }

    private void drawGlyphs(Canvas c, String s, float x, float y, float scale, int color) {
        paint.setAlpha(255);
        paint.setColorFilter(new PorterDuffColorFilter(color, PorterDuff.Mode.SRC_IN));
        for (int i = 0; i < s.length(); ++i) {
            int[] g = glyphs.get(s.charAt(i));
            if (g == null) {
                x += 10 * scale;
                continue;
            }
            Bitmap page = pages.get(g[0]);
            if (page == null) {
                page = load("font/page" + g[0] + ".png");
                if (page == null) return;
                pages.put(g[0], page);
            }
            float k = page.getWidth() / 256f;  // glyph rectangles are in the 256 px page's pixels
            src.set(Math.round(g[1] * k), Math.round(g[2] * k), Math.round(g[3] * k), Math.round(g[4] * k));
            float gx = x + g[5] * scale;
            dst.set(gx, y, gx + (g[3] - g[1]) * scale, y + (g[4] - g[2]) * scale);
            c.drawBitmap(page, src, dst, paint);
            x += g[7] * scale;
        }
        paint.setColorFilter(null);
    }
}

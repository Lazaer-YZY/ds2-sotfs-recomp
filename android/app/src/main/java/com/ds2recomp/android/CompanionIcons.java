package com.ds2recomp.android;

import android.graphics.Bitmap;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Equipment icons by the data hash of the texture the game's HUD draws: the development icon pack's Real-ESRGAN
 * upscale when there is one (CompanionArt.icon), otherwise straight from the running game (RuntimeActivity
 * .nativeCompanionIcon, 16 KB of Xenos-tiled DXT5 for a 64x128 icon) decoded here and enlarged 4x (Lanczos-3 on
 * premultiplied colour, then mipmapped so the card's downscale stays smooth). Public APKs bundle nothing from the game,
 * so they always take the second way; DLC items and every language work either way. Worker thread only.
 */
final class CompanionIcons {
    private static final int W = 64, H = 128, K = 4;
    private final Map<Long, Bitmap> cache = new LinkedHashMap<Long, Bitmap>(16, 0.75f, true) {
        @Override
        protected boolean removeEldestEntry(Map.Entry<Long, Bitmap> e) { return size() > 24; }
    };
    private final Map<Long, Long> failed = new java.util.HashMap<>();

    /**
     * The icon for a hash from the state line: the icon pack's upscale if art has one, else the game's texture enlarged
     * here; null while unavailable (retried at most every second).
     */
    Bitmap get(long hash, CompanionArt art) {
        if (hash == 0) return null;
        Bitmap b = cache.get(hash);
        if (b != null) return b;
        b = art.icon(hash);
        if (b != null) {
            cache.put(hash, b);
            return b;
        }
        long now = android.os.SystemClock.uptimeMillis();
        Long last = failed.get(hash);
        if (last != null && now - last < 1000) return null;
        byte[] data = RuntimeActivity.nativeCompanionIcon(hash);
        int[] px = data == null || data.length < 0x4000 ? null : XenosTexture.decodePixels(XenosTexture.DXT5, W, H, data);
        if (px == null) {
            failed.put(hash, now);
            return null;
        }
        b = enlarge(px, W, H, K);
        cache.put(hash, b);
        failed.remove(hash);
        return b;
    }

    private static float lanczos(float x) {
        if (x == 0) return 1;
        if (x <= -3 || x >= 3) return 0;
        double px = Math.PI * x;
        return (float) (3 * Math.sin(px) * Math.sin(px / 3) / (px * px));
    }

    /** Per output position: the first source index and 6 normalised Lanczos-3 weights. */
    private static float[] weights(int n, int k, int[] first) {
        float[] w = new float[n * k * 6];
        for (int o = 0; o < n * k; ++o) {
            float centre = (o + 0.5f) / k - 0.5f;
            int i0 = (int) Math.floor(centre) - 2;
            first[o] = i0;
            float sum = 0;
            for (int t = 0; t < 6; ++t) sum += (w[o * 6 + t] = lanczos(centre - (i0 + t)));
            for (int t = 0; t < 6; ++t) w[o * 6 + t] /= sum;
        }
        return w;
    }

    /**
     * k x enlargement with Lanczos-3 (sharper than bilinear, no blur halo): separable, on premultiplied colour so the
     * transparent surroundings don't darken the edges. Mipmapped for the drawing downscale.
     */
    static Bitmap enlarge(int[] px, int w, int h, int k) {
        final int ow = w * k, oh = h * k;
        float[] src = new float[w * h * 4];
        for (int i = 0; i < w * h; ++i) {
            int c = px[i];
            float a = (c >>> 24) / 255f;
            src[i * 4] = a;
            src[i * 4 + 1] = ((c >> 16) & 255) / 255f * a;
            src[i * 4 + 2] = ((c >> 8) & 255) / 255f * a;
            src[i * 4 + 3] = (c & 255) / 255f * a;
        }
        int[] fx = new int[ow], fy = new int[oh];
        float[] wx = weights(w, k, fx), wy = weights(h, k, fy);
        float[] mid = new float[ow * h * 4];
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < ow; ++x) {
                float a = 0, r = 0, g = 0, b = 0;
                for (int t = 0; t < 6; ++t) {
                    int sx = Math.min(w - 1, Math.max(0, fx[x] + t));
                    float q = wx[x * 6 + t];
                    int s = (y * w + sx) * 4;
                    a += src[s] * q;
                    r += src[s + 1] * q;
                    g += src[s + 2] * q;
                    b += src[s + 3] * q;
                }
                int d = (y * ow + x) * 4;
                mid[d] = a;
                mid[d + 1] = r;
                mid[d + 2] = g;
                mid[d + 3] = b;
            }
        }
        int[] out = new int[ow * oh];
        for (int y = 0; y < oh; ++y) {
            for (int x = 0; x < ow; ++x) {
                float a = 0, r = 0, g = 0, b = 0;
                for (int t = 0; t < 6; ++t) {
                    int sy = Math.min(h - 1, Math.max(0, fy[y] + t));
                    float q = wy[y * 6 + t];
                    int s = (sy * ow + x) * 4;
                    a += mid[s] * q;
                    r += mid[s + 1] * q;
                    g += mid[s + 2] * q;
                    b += mid[s + 3] * q;
                }
                a = Math.min(1f, Math.max(0f, a));
                int ia = Math.round(a * 255);
                int ir = 0, ig = 0, ib = 0;
                if (a > 1e-4f) {
                    ir = Math.min(255, Math.max(0, Math.round(r / a * 255)));
                    ig = Math.min(255, Math.max(0, Math.round(g / a * 255)));
                    ib = Math.min(255, Math.max(0, Math.round(b / a * 255)));
                }
                out[y * ow + x] = (ia << 24) | (ir << 16) | (ig << 8) | ib;
            }
        }
        Bitmap bm = Bitmap.createBitmap(out, ow, oh, Bitmap.Config.ARGB_8888);
        bm.setHasMipMap(true);
        return bm;
    }
}

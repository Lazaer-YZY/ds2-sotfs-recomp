package com.ds2recomp.android;

import android.graphics.Bitmap;

/**
 * Decodes Xbox 360 textures as the game stores them (TPF data, or its copy in guest memory): DXT1 / DXT3 / DXT5 in 4x4
 * blocks laid out by the Xenos 2D tiling (XGAddress2DTiledOffset), 16-bit words big-endian, and 32-bit A8R8G8B8 (the
 * title screen's logo) tiled per pixel. The same decoder as tools/android/ds2_companion_assets.py (from the Lightning
 * Returns port's lr_gui_tex.py).
 */
final class XenosTexture {
    static final int DXT1 = 1, DXT3 = 3, DXT5 = 5, ARGB8 = 9;

    private XenosTexture() {}

    /** TPF format byte (0 / 1 DXT1, 3 DXT3, 5 DXT5, 9 A8R8G8B8) to a kind, 0 if not decoded. */
    static int kindOfTpf(int fmt) {
        switch (fmt) {
            case 0: case 1: return DXT1;
            case 3: return DXT3;
            case 5: return DXT5;
            case 9: return ARGB8;
            default: return 0;
        }
    }

    /** Block offset (in blocks) of block (x, y) in a tiled texture blocksWide wide, for 4-, 8- or 16-byte blocks. */
    static int tiledOffset(int x, int y, int blocksWide, int bytesPerBlock) {
        final int logBpp = (bytesPerBlock >> 2) + ((bytesPerBlock >> 1) >> (bytesPerBlock >> 2));
        int aw = (blocksWide + 31) & ~31;
        int macro = ((x >> 5) + (y >> 5) * (aw >> 5)) << (logBpp + 7);
        int micro = ((x & 7) + ((y & 6) << 2)) << logBpp;
        int off = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 8) << (3 + logBpp)) + ((y & 1) << 4);
        off = (((off & ~511) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7)
            + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> logBpp;
        return off;
    }

    static Bitmap decode(int kind, int w, int h, byte[] d) {
        int[] px = decodePixels(kind, w, h, d);
        return px == null ? null : Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888);
    }

    /** ARGB pixels, or null for an unknown kind. */
    static int[] decodePixels(int kind, int w, int h, byte[] d) {
        if (kind == ARGB8) {  // one 4-byte "block" per pixel, A R G B in byte order
            int[] px = new int[w * h];
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    int o = tiledOffset(x, y, w, 4) * 4;
                    if (o + 4 > d.length) continue;
                    px[y * w + x] = ((d[o] & 0xFF) << 24) | ((d[o + 1] & 0xFF) << 16) | ((d[o + 2] & 0xFF) << 8) | (d[o + 3] & 0xFF);
                }
            }
            return px;
        }
        if (kind != DXT1 && kind != DXT3 && kind != DXT5) return null;
        final int bpb = kind == DXT1 ? 8 : 16;
        final int bw = (w + 3) / 4, bh = (h + 3) / 4;
        int[] px = new int[w * h];
        byte[] blk = new byte[16];
        int[] alpha = new int[16];
        int[] color = new int[4];
        int[] pal = new int[8];
        for (int by = 0; by < bh; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                int o = tiledOffset(bx, by, bw, bpb) * bpb;
                if (o + bpb > d.length) continue;
                for (int i = 0; i < bpb; i += 2) {  // 16-bit words are big-endian
                    blk[i] = d[o + i + 1];
                    blk[i + 1] = d[o + i];
                }
                int c = kind == DXT1 ? 0 : 8;  // colour block offset
                if (kind == DXT5) {
                    int a0 = blk[0] & 0xFF, a1 = blk[1] & 0xFF;
                    pal[0] = a0;
                    pal[1] = a1;
                    for (int i = 1; i < 7; ++i) {
                        if (a0 > a1) pal[1 + i] = ((7 - i) * a0 + i * a1) / 7;
                        else pal[1 + i] = i < 5 ? ((5 - i) * a0 + i * a1) / 5 : (i == 5 ? 0 : 255);
                    }
                    long bits = 0;
                    for (int i = 0; i < 6; ++i) bits |= (long) (blk[2 + i] & 0xFF) << (8 * i);
                    for (int i = 0; i < 16; ++i) alpha[i] = pal[(int) ((bits >> (3 * i)) & 7)];
                } else if (kind == DXT3) {
                    for (int i = 0; i < 16; ++i) {
                        int nib = (blk[i >> 1] >> ((i & 1) * 4)) & 15;
                        alpha[i] = nib * 17;
                    }
                }
                int c0 = (blk[c] & 0xFF) | ((blk[c + 1] & 0xFF) << 8), c1 = (blk[c + 2] & 0xFF) | ((blk[c + 3] & 0xFF) << 8);
                int r0 = ((c0 >> 11) & 31) * 255 / 31, g0 = ((c0 >> 5) & 63) * 255 / 63, b0 = (c0 & 31) * 255 / 31;
                int r1 = ((c1 >> 11) & 31) * 255 / 31, g1 = ((c1 >> 5) & 63) * 255 / 63, b1 = (c1 & 31) * 255 / 31;
                boolean four = kind != DXT1 || c0 > c1;
                color[0] = 0xFF000000 | (r0 << 16) | (g0 << 8) | b0;
                color[1] = 0xFF000000 | (r1 << 16) | (g1 << 8) | b1;
                if (four) {
                    color[2] = 0xFF000000 | (((2 * r0 + r1) / 3) << 16) | (((2 * g0 + g1) / 3) << 8) | ((2 * b0 + b1) / 3);
                    color[3] = 0xFF000000 | (((r0 + 2 * r1) / 3) << 16) | (((g0 + 2 * g1) / 3) << 8) | ((b0 + 2 * b1) / 3);
                } else {
                    color[2] = 0xFF000000 | (((r0 + r1) / 2) << 16) | (((g0 + g1) / 2) << 8) | ((b0 + b1) / 2);
                    color[3] = 0;  // transparent black
                }
                int cbits = (blk[c + 4] & 0xFF) | ((blk[c + 5] & 0xFF) << 8) | ((blk[c + 6] & 0xFF) << 16) | ((blk[c + 7] & 0xFF) << 24);
                for (int i = 0; i < 16; ++i) {
                    int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x >= w || y >= h) continue;
                    int v = color[(cbits >>> (2 * i)) & 3];
                    if (kind != DXT1) v = (alpha[i] << 24) | (v & 0xFFFFFF);
                    px[y * w + x] = v;
                }
            }
        }
        return px;
    }

    /** FNV-1a 64 of the first len bytes (ds2_hud_sprites.cpp and ds2_companion_assets.py hash textures the same way). */
    static long fnv64(byte[] d, int len) {
        long h = 0xCBF29CE484222325L;
        for (int i = 0; i < Math.min(len, d.length); ++i) h = (h ^ (d[i] & 0xFF)) * 0x100000001B3L;
        return h;
    }
}

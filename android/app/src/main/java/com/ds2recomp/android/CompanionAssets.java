package com.ds2recomp.android;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.os.Process;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeSet;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.Inflater;

/**
 * The second screen's art built on the device from the player's own game files, for APKs that carry none (public
 * builds: nothing derived from the game is shipped). A Java port of tools/android/ds2_companion_assets.py: the same
 * archives (disc 1's GameData*.bhd/.bdt in the game folder, the DLC's DlcData*.bhd/.bdt the runtime unpacked from
 * disc 2's package), the same pieces, composed icons, effect icons with their data hashes and the HUD font, written to
 * {@code <external files>/companion/} in the bundled layout (hud.json, font.json, hud/, effect/, font/). Upscaled 2x
 * with filtering instead of Real-ESRGAN (slightly softer than development builds). Runs once in the background after
 * the second screen first shows; a stamp (VERSION, game folder, Default.xex size) decides when to rebuild, and
 * {@link #generation()} tells CompanionView to reload.
 */
final class CompanionAssets {
    private static final String TAG = "ds2recomp";
    private static final int VERSION = 3;  // 2: the title logo (the cover without a character); 3: covenant icons
    private static final AtomicInteger GENERATION = new AtomicInteger();
    private static volatile boolean running;

    private CompanionAssets() {}

    static int generation() { return GENERATION.get(); }

    static File dir(Context context) {
        File base = context.getExternalFilesDir(null);
        return new File(base != null ? base : context.getFilesDir(), "companion");
    }

    static boolean bundled(Context context) {
        try {
            context.getAssets().open("companion/hud.json").close();
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    private static String stamp(Context context) {
        File game = GameStorage.gameDir(context);
        return "v" + VERSION + " " + game.getAbsolutePath() + " " + new File(game, "Default.xex").length();
    }

    static boolean ready(Context context) {
        File s = new File(dir(context), "stamp.txt");
        try {
            return s.isFile() && new String(java.nio.file.Files.readAllBytes(s.toPath()), StandardCharsets.UTF_8)
                .trim().equals(stamp(context));
        } catch (IOException e) {
            return false;
        }
    }

    /** Builds the art in the background if this APK has none and it was not built for this game folder yet. */
    static void ensure(Context context) {
        final Context app = context.getApplicationContext();
        if (running || bundled(app) || ready(app)) return;
        running = true;
        Thread t = new Thread(() -> {
            Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND);
            long t0 = android.os.SystemClock.uptimeMillis();
            try {
                build(app);
                GENERATION.incrementAndGet();
                Log.i(TAG, "companion: art built from the game files in " + (android.os.SystemClock.uptimeMillis() - t0) + " ms");
            } catch (Exception | OutOfMemoryError e) {
                Log.w(TAG, "companion: building the art failed", e);
            } finally {
                running = false;
            }
        }, "DS2 companion art");
        t.start();
    }

    // ------------------------------------------------------------------------------------------- archives
    private static final class Entry {
        final File bdt;
        final long off;
        final int size;

        Entry(File bdt, long off, int size) {
            this.bdt = bdt;
            this.off = off;
            this.size = size;
        }
    }

    private final Map<String, Entry> index = new HashMap<>();

    private static int u16(byte[] d, int p) { return ((d[p] & 0xFF) << 8) | (d[p + 1] & 0xFF); }
    private static int s32(byte[] d, int p) { return ((d[p] & 0xFF) << 24) | ((d[p + 1] & 0xFF) << 16) | ((d[p + 2] & 0xFF) << 8) | (d[p + 3] & 0xFF); }
    private static long u32(byte[] d, int p) { return s32(d, p) & 0xFFFFFFFFL; }
    private static long s64(byte[] d, int p) { return ((long) s32(d, p) << 32) | u32(d, p + 4); }

    private static byte[] readFile(File f) throws IOException {
        return java.nio.file.Files.readAllBytes(f.toPath());
    }

    /** BND4 / BHF4 file table (big-endian on the 360): name (lower case, '/') -> {offset, size}. */
    private static LinkedHashMap<String, long[]> bnd4(byte[] d) {
        LinkedHashMap<String, long[]> out = new LinkedHashMap<>();
        if (d.length < 0x40 || d[0] != 'B' || !(d[1] == 'N' || d[1] == 'H')) return out;
        int count = s32(d, 0x0C);
        int hsize = (int) s64(d, 0x20);
        boolean unicode = d[0x30] != 0;
        int fmt = d[0x31] & 0xFF;
        boolean bitBe = d[0x0A] == 0;
        if (!(bitBe || ((fmt & 1) != 0 && (fmt & 0x80) == 0))) fmt = Integer.reverse(fmt) >>> 24;
        int pos = 0x40;
        for (int i = 0; i < count; ++i, pos += hsize) {
            int p = pos + 8;
            long size = s64(d, p);
            p += 8 + ((fmt & 0x20) != 0 ? 8 : 0);
            long off;
            if ((fmt & 0x10) != 0) {
                off = s64(d, p);
                p += 8;
            } else {
                off = u32(d, p);
                p += 4;
            }
            if ((fmt & 0x02) != 0) p += 4;
            int noff = (int) u32(d, p);
            StringBuilder name = new StringBuilder();
            if (unicode) {
                for (int q = noff; q + 1 < d.length && (d[q] != 0 || d[q + 1] != 0); q += 2) name.append((char) u16(d, q));
            } else {
                for (int q = noff; q < d.length && d[q] != 0; ++q) name.append((char) (d[q] & 0xFF));
            }
            out.put(name.toString().replace('\\', '/').toLowerCase(java.util.Locale.ROOT), new long[] { off, size });
        }
        return out;
    }

    private void addArchive(File bhd, File bdt) throws IOException {
        if (!bhd.isFile() || !bdt.isFile()) return;
        for (Map.Entry<String, long[]> e : bnd4(readFile(bhd)).entrySet()) {
            index.put(e.getKey(), new Entry(bdt, e.getValue()[0], (int) e.getValue()[1]));
        }
    }

    private static byte[] undcx(byte[] d) throws IOException {
        if (d.length < 8 || d[0] != 'D' || d[1] != 'C' || d[2] != 'X' || d[3] != 0) return d;
        int dca = indexOf(d, new byte[] { 'D', 'C', 'A', 0 });
        int dcs = indexOf(d, new byte[] { 'D', 'C', 'S', 0 });
        int start = dca + s32(d, dca + 4);
        int usize = s32(d, dcs + 4);
        Inflater inf = new Inflater();
        inf.setInput(d, start, d.length - start);
        byte[] out = new byte[usize];
        try {
            int n = 0;
            while (n < usize && !inf.finished()) {
                int k = inf.inflate(out, n, usize - n);
                if (k == 0 && (inf.needsInput() || inf.needsDictionary())) break;
                n += k;
            }
        } catch (java.util.zip.DataFormatException e) {
            throw new IOException(e);
        } finally {
            inf.end();
        }
        return out;
    }

    private static int indexOf(byte[] d, byte[] what) {
        outer:
        for (int i = 0; i + what.length <= d.length; ++i) {
            for (int k = 0; k < what.length; ++k) if (d[i + k] != what[k]) continue outer;
            return i;
        }
        return -1;
    }

    private byte[] get(String name) throws IOException {
        Entry e = index.get(name);
        if (e == null) throw new IOException(name + " not in the game's archives");
        byte[] buf = new byte[e.size];
        try (RandomAccessFile f = new RandomAccessFile(e.bdt, "r")) {
            f.seek(e.off);
            f.readFully(buf);
        }
        return undcx(buf);
    }

    /** The files of a BND4 bundle by their last path part (lower case). */
    private static Map<String, byte[]> bndFiles(byte[] d) throws IOException {
        Map<String, byte[]> out = new HashMap<>();
        for (Map.Entry<String, long[]> e : bnd4(d).entrySet()) {
            String n = e.getKey();
            n = n.substring(n.lastIndexOf('/') + 1);
            byte[] part = java.util.Arrays.copyOfRange(d, (int) e.getValue()[0], (int) (e.getValue()[0] + e.getValue()[1]));
            out.put(n, undcx(part));
        }
        return out;
    }

    /** A TPF's first texture: {kind, width, height} and its raw (tiled) data. */
    private static final class Tex {
        int kind, w, h;
        byte[] raw;
    }

    private static Tex tpf(byte[] d) {
        if (d.length < 0x20 || d[0] != 'T' || d[1] != 'P' || d[2] != 'F') return null;
        int platform = d[0x0C] & 0xFF;
        int p = 0x10;
        Tex t = new Tex();
        int off = (int) u32(d, p), size = s32(d, p + 4);
        t.kind = XenosTexture.kindOfTpf(d[p + 8] & 0xFF);
        t.w = u16(d, p + 12);
        t.h = u16(d, p + 14);
        t.raw = java.util.Arrays.copyOfRange(d, off, off + size);
        return platform == 0 ? null : t;
    }

    private static Bitmap decode(Tex t) {
        return t == null ? null : XenosTexture.decode(t.kind, t.w, t.h, t.raw);
    }

    // ------------------------------------------------------------------------------------------- what we cut
    // Same tables as tools/android/ds2_companion_assets.py (piece -> texture, rectangle x0 y0 x1 y1 in its pixels).
    private static final Object[][] PIECES = {
        { "emblem", "FE_HPgage_01", 0, 0, 92, 88 }, { "bars", "FE_HPgage_01", 84, 20, 253, 62 },
        { "cap_hp", "FE_HPgage_01", 180, 88, 214, 109 }, { "cap_st", "FE_HPgage_01", 141, 92, 175, 106 },
        { "fill_hp", "FE_HPgage_01", 35, 95, 96, 105 }, { "fill_st", "FE_HPgage_01", 35, 112, 96, 118 },
        { "fill_flash", "FE_HPgage_01", 100, 95, 112, 105 }, { "human", "FE_HPgage_01", 190, 59, 218, 88 },
        { "line_red", "FE_HPgage_01", 158, 3, 249, 7 }, { "line_yellow", "FE_HPgage_01", 158, 9, 249, 13 },
        { "bar_dark", "FE_HPgage_01", 156, 15, 251, 23 },
        { "soul_end", "Soul_Gauge", 3, 1, 30, 36 }, { "soul_line", "Soul_Gauge", 35, 4, 62, 7 },
        { "soul_icon", "Soul_Gauge", 0, 38, 26, 64 },
        { "tol_poison", "Icon_tex", 162, 216, 188, 242 }, { "tol_curse", "Icon_tex", 189, 216, 215, 242 },
        { "card_frame", "L_keyMenu", 1, 0, 80, 104 }, { "card_back", "L_keyMenu", 81, 4, 156, 98 },
        { "card_glow", "L_keyMenu", 159, 4, 254, 124 }, { "card_line_red", "L_keyMenu", 11, 111, 70, 113 },
        { "card_line_grey", "L_keyMenu", 11, 117, 70, 119 }, { "plate", "IconBase", 81, 224, 150, 255 },
        { "leather", "ItemSellect_02", 0, 0, 512, 372 }, { "panel", "In-game_01", 16, 4, 892, 544 },
        { "ornament", "In-game_01", 0, 586, 812, 616 },
    };
    private static final String[][] SOURCES = {
        { "FE_HPgage_01", "menu/01.febnd.dcx", "fe_hpgage_01.tpf" }, { "Soul_Gauge", "menu/01.febnd.dcx", "soul_gauge.tpf" },
        { "L_keyMenu", "menu/01.febnd.dcx", "l_keymenu.tpf" }, { "Icon_tex", "menu/99.febnd.dcx", "icon_tex.tpf" },
        { "ItemSellect_02", "menu/99.febnd.dcx", "itemsellect_02.tpf" }, { "IconBase", "menu/99.febnd.dcx", "iconbase.tpf" },
        { "waku_02", "menu/99.febnd.dcx", "waku_02.tpf" }, { "waku_03", "menu/99.febnd.dcx", "waku_03.tpf" },
        { "In-game_01", "menu/40.febnd.dcx", "in-game_01.tpf" },
    };

    private static void build(Context context) throws Exception {
        CompanionAssets a = new CompanionAssets();
        File game = GameStorage.gameDir(context);
        for (String n : new String[] { "GameData1", "GameData2" }) a.addArchive(new File(game, n + ".bhd"), new File(game, n + ".bdt"));
        // The DLC the runtime unpacked from disc 2's package: <root>/<xuid>/465307E4/00000002/<package>/Data/catalog/,
        // root = the app's user/ folder, or GameStorage.dlcContentRoot (dlc_content/ on the game's storage volume when
        // the game is on an SD card, issue #6).
        File base = context.getExternalFilesDir(null);
        List<File> roots = new ArrayList<>();
        roots.add(new File(base != null ? base : context.getFilesDir(), "user"));
        File dlcRoot = GameStorage.dlcContentRoot(context);
        if (dlcRoot != null) roots.add(dlcRoot);
        for (File root : roots) {
            File[] xuids = root.listFiles();
            if (xuids == null) continue;
            for (File x : xuids) {
                File[] pkgs = new File(x, "465307E4/00000002").listFiles();
                if (pkgs == null) continue;
                for (File pkg : pkgs) {
                    File[] cat = new File(pkg, "Data/catalog").listFiles();
                    if (cat == null) continue;
                    for (File f : cat) {
                        String n = f.getName();
                        if (n.toLowerCase(java.util.Locale.ROOT).endsWith(".bhd"))
                            a.addArchive(f, new File(f.getParentFile(), n.substring(0, n.length() - 4) + ".bdt"));
                    }
                }
            }
        }
        File out = dir(context), work = new File(out.getParentFile(), "companion.tmp");
        deleteTree(work);
        new File(work, "hud").mkdirs();
        new File(work, "effect").mkdirs();
        new File(work, "vow").mkdirs();
        new File(work, "font").mkdirs();

        // Textures.
        Map<String, Map<String, byte[]>> bundles = new HashMap<>();
        Map<String, Bitmap> tex = new HashMap<>();
        for (String[] s : SOURCES) {
            Map<String, byte[]> b = bundles.get(s[1]);
            if (b == null) {
                b = bndFiles(a.get(s[1]));
                bundles.put(s[1], b);
            }
            Bitmap bm = decode(tpf(b.get(s[2])));
            if (bm == null) throw new IOException("cannot decode " + s[2]);
            tex.put(s[0], bm);
        }
        JSONObject pieces = new JSONObject();
        for (Object[] p : PIECES) {
            String name = (String) p[0];
            int x0 = (Integer) p[2], y0 = (Integer) p[3], x1 = (Integer) p[4], y1 = (Integer) p[5];
            Bitmap piece = Bitmap.createBitmap(tex.get((String) p[1]), x0, y0, x1 - x0, y1 - y0).copy(Bitmap.Config.ARGB_8888, true);
            if (name.equals("bars")) {  // neighbouring art inside the rectangle (the dark bar above, the human icon below)
                clear(piece, 66, 0, 169, 5);
                clear(piece, 66, 38, 169, 42);
            }
            save(enlarge(piece), new File(work, "hud/" + name + ".png"));
            pieces.put(name, new JSONArray().put(x1 - x0).put(y1 - y0));
        }
        for (Map.Entry<String, Bitmap> e : composeIcons(tex).entrySet()) {
            save(enlarge(e.getValue()), new File(work, "hud/" + e.getKey() + ".png"));
            pieces.put(e.getKey(), new JSONArray().put(26).put(26));
        }
        try {  // the cover without a character; the rest of the art does not depend on it
            Bitmap logo = composeTitle(bndFiles(a.get("menu/17.febnd.dcx")));
            save(logo, new File(work, "hud/title_logo.png"));
            pieces.put("title_logo", new JSONArray().put(logo.getWidth()).put(logo.getHeight()));
        } catch (Exception e) {
            Log.w(TAG, "companion: no title logo (" + e.getMessage() + ")");
        }

        // Effect icons and their data hashes.
        JSONArray effects = new JSONArray();
        JSONObject effectHashes = new JSONObject();
        Pattern ei = Pattern.compile("^menu/tex/icon/effect/ei_(\\d+)\\.tpf$");
        for (String n : new TreeSet<>(a.index.keySet())) {
            Matcher m = ei.matcher(n);
            if (!m.matches()) continue;
            Tex t = tpf(a.get(n));
            Bitmap bm = decode(t);
            if (bm == null) continue;
            int id = Integer.parseInt(m.group(1));
            save(enlarge(bm), new File(work, String.format(java.util.Locale.US, "effect/%04d.png", id)));
            effects.put(id);
            effectHashes.put(Long.toHexString(XenosTexture.fnv64(t.raw, 0x4000)), id);
        }

        // Covenant icons ("vow"; the HP gauge's emblem shows the joined covenant's) and their data hashes.
        JSONObject vowHashes = new JSONObject();
        Pattern vi = Pattern.compile("^menu/tex/icon/vow/vi_(\\d+)\\.tpf$");
        for (String n : new TreeSet<>(a.index.keySet())) {
            Matcher m = vi.matcher(n);
            if (!m.matches()) continue;
            Tex t = tpf(a.get(n));
            Bitmap bm = decode(t);
            if (bm == null) continue;
            int id = Integer.parseInt(m.group(1));
            save(enlarge(bm), new File(work, String.format(java.util.Locale.US, "vow/%04d.png", id)));
            vowHashes.put(Long.toHexString(XenosTexture.fnv64(t.raw, 0x4000)), id);
        }

        // The HUD's font (a loose file in the game folder): CCM glyph table + DXT pages.
        JSONObject font = buildFont(new File(game, "font/English/FeFont_Small.fontbnd.dcx"), new File(work, "font"));

        write(new File(work, "hud.json"), new JSONObject().put("version", 2).put("pieces", pieces).put("effects", effects)
            .put("effect_hashes", effectHashes).put("vow_hashes", vowHashes).put("generated", true).toString(1));
        write(new File(work, "font.json"), font.toString());
        write(new File(work, "stamp.txt"), stamp(context));
        deleteTree(out);
        if (!work.renameTo(out)) throw new IOException("cannot move " + work + " to " + out);
    }

    private static JSONObject buildFont(File fontbnd, File outDir) throws Exception {
        Map<String, byte[]> files = bndFiles(undcx(readFile(fontbnd)));
        byte[] ccm = files.get("fefont_small.ccm");
        int regoff = s32(ccm, 0x14), glyoff = s32(ccm, 0x18);
        JSONObject glyphs = new JSONObject();
        TreeSet<Integer> pages = new TreeSet<>();
        List<Integer> wanted = new ArrayList<>();
        for (int c = 0x20; c < 0x7F; ++c) wanted.add(c);
        wanted.add(0xD7);
        wanted.add(0xB7);
        wanted.add(0x2026);
        for (int p = glyoff; p + 0x18 <= ccm.length; p += 0x18) {
            int code = s32(ccm, p), ro = s32(ccm, p + 4);
            if (ro < regoff || ro >= glyoff) break;
            if (!wanted.contains(code)) continue;
            int page = u16(ccm, p + 8);
            JSONArray g = new JSONArray().put(page).put(u16(ccm, ro)).put(u16(ccm, ro + 2)).put(u16(ccm, ro + 4)).put(u16(ccm, ro + 6))
                .put((short) u16(ccm, p + 10)).put((short) u16(ccm, p + 12)).put((short) u16(ccm, p + 14));
            glyphs.put(Integer.toString(code), g);
            pages.add(page);
        }
        JSONArray pageList = new JSONArray();
        for (int page : pages) {
            String prefix = String.format(java.util.Locale.US, "fefont_small_%04d", page);
            for (Map.Entry<String, byte[]> e : files.entrySet()) {
                if (!e.getKey().startsWith(prefix)) continue;
                Bitmap bm = decode(tpf(e.getValue()));
                if (bm != null) save(enlarge(bm), new File(outDir, "page" + page + ".png"));
            }
            pageList.put(page);
        }
        return new JSONObject().put("line_height", u16(ccm, 8)).put("pages", pageList).put("glyphs", glyphs);
    }

    // The title screen's logo in one line, as ds2_companion_assets.py's TITLE_LOGO: "DARK", "SOULS", "II" from Title
    // (512x512 A8R8G8B8, opaque on black: the game adds it) and "SCHOLAR OF THE FIRST SIN" from Title_Best_DC.
    // Texture (0 Title, 1 Title_Best_DC), source x0 y0 x1 y1, position in the 1249x210 logo.
    private static final int[][] TITLE_LOGO = {
        { 0, 0, 8, 512, 148, 12, 0 }, { 0, 0, 150, 512, 290, 519, 0 }, { 0, 0, 296, 150, 436, 1044, 0 },
        { 1, 0, 10, 640, 52, 308, 150 },
    };

    /** The logo added up like the title screen draws it, black made transparent (it sits on the leather). */
    private static Bitmap composeTitle(Map<String, byte[]> files) throws IOException {
        Bitmap[] tex = { decode(tpf(files.get("title.tpf"))), decode(tpf(files.get("title_best_dc.tpf"))) };
        if (tex[0] == null || tex[1] == null) throw new IOException("title textures not decoded");
        final int W = 1249, H = 210;
        int[] sum = new int[W * H * 3];
        for (int[] p : TITLE_LOGO) {
            int w = Math.min(p[3] - p[1], W - p[5]), h = p[4] - p[2];
            int[] px = new int[w * h];
            tex[p[0]].getPixels(px, 0, w, p[1], p[2], w, h);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    int v = px[y * w + x], o = ((p[6] + y) * W + p[5] + x) * 3;
                    sum[o] += (v >> 16) & 0xFF;
                    sum[o + 1] += (v >> 8) & 0xFF;
                    sum[o + 2] += v & 0xFF;
                }
            }
        }
        int[] out = new int[W * H];
        for (int i = 0; i < W * H; ++i) {
            int r = Math.min(255, sum[3 * i]), g = Math.min(255, sum[3 * i + 1]), b = Math.min(255, sum[3 * i + 2]);
            int al = Math.max(r, Math.max(g, b));
            out[i] = al == 0 ? 0 : (al << 24) | ((r * 255 / al) << 16) | ((g * 255 / al) << 8) | (b * 255 / al);
        }
        return Bitmap.createBitmap(out, W, H, Bitmap.Config.ARGB_8888);
    }

    /** Toxic (waku_03's poison flask in the game's red) and petrification (Icon_tex's crystal) in a build-up rim. */
    private static Map<String, Bitmap> composeIcons(Map<String, Bitmap> tex) {
        Map<String, Bitmap> out = new LinkedHashMap<>();
        Bitmap hexa = Bitmap.createScaledBitmap(Bitmap.createBitmap(tex.get("waku_02"), 788, 91, 31, 31), 20, 20, true);
        for (String name : new String[] { "tol_toxic", "tol_petrify" }) {
            Bitmap base = Bitmap.createBitmap(tex.get("Icon_tex"), 162, 216, 26, 26).copy(Bitmap.Config.ARGB_8888, true);
            Canvas c = new Canvas(base);
            Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);
            c.drawBitmap(hexa, 3, 3, paint);
            Bitmap icon;
            int size;
            if (name.equals("tol_toxic")) {
                Bitmap src = Bitmap.createBitmap(tex.get("waku_03"), 963, 10, 18, 17);
                icon = recolour(src, 205, 105, 125);
                size = 17;
            } else {
                Bitmap cell = Bitmap.createBitmap(tex.get("Icon_tex"), 135 + 5, 162 + 5, 16, 16);
                icon = brightOnly(cell);
                size = 19;
            }
            float k = Math.min(1f, size / (float) Math.max(icon.getWidth(), icon.getHeight()));
            int w = Math.round(icon.getWidth() * k), h = Math.round(icon.getHeight() * k);
            c.drawBitmap(icon, null, new Rect((26 - w) / 2, (26 - h) / 2, (26 - w) / 2 + w, (26 - h) / 2 + h), paint);
            out.put(name, base);
        }
        return out;
    }

    private static Bitmap recolour(Bitmap src, int r, int g, int b) {
        Bitmap out = src.copy(Bitmap.Config.ARGB_8888, true);
        for (int y = 0; y < out.getHeight(); ++y) {
            for (int x = 0; x < out.getWidth(); ++x) {
                int p = out.getPixel(x, y);
                float lum = (Color.red(p) + Color.green(p) + Color.blue(p)) / 3f / 255f * 1.35f;
                out.setPixel(x, y, Color.argb(Color.alpha(p), Math.min(255, Math.round(lum * r)), Math.min(255, Math.round(lum * g)),
                    Math.min(255, Math.round(lum * b))));
            }
        }
        return out;
    }

    private static Bitmap brightOnly(Bitmap src) {
        Bitmap out = src.copy(Bitmap.Config.ARGB_8888, true);
        for (int y = 0; y < out.getHeight(); ++y) {
            for (int x = 0; x < out.getWidth(); ++x) {
                int p = out.getPixel(x, y);
                float mean = (Color.red(p) + Color.green(p) + Color.blue(p)) / 3f;
                int a = Math.round(Math.max(0f, Math.min(255f, (mean - 38) * 2.6f)) * Color.alpha(p) / 255f);
                out.setPixel(x, y, (a << 24) | (p & 0xFFFFFF));
            }
        }
        return out;
    }

    private static void clear(Bitmap b, int x0, int y0, int x1, int y1) {
        for (int y = y0; y < Math.min(y1, b.getHeight()); ++y)
            for (int x = x0; x < Math.min(x1, b.getWidth()); ++x) b.setPixel(x, y, 0);
    }

    /** 2x with filtering (development builds bundle Real-ESRGAN 4x upscales; the drawing code takes any scale). */
    private static Bitmap enlarge(Bitmap b) {
        return Bitmap.createScaledBitmap(b, b.getWidth() * 2, b.getHeight() * 2, true);
    }

    private static void save(Bitmap b, File f) throws IOException {
        try (FileOutputStream o = new FileOutputStream(f)) {
            b.compress(Bitmap.CompressFormat.PNG, 100, o);
        }
    }

    private static void write(File f, String s) throws IOException {
        try (FileOutputStream o = new FileOutputStream(f)) {
            o.write(s.getBytes(StandardCharsets.UTF_8));
        }
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }
}

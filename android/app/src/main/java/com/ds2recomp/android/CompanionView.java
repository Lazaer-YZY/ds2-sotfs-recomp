package com.ds2recomp.android;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Shader;
import android.graphics.Typeface;

/**
 * Draws the bottom screen: the game's HUD values with the game's own art (CompanionArt), laid out on a 1240x1080
 * base (the AYN Thor's second panel) and scaled uniformly to the actual display (other shapes letterbox).
 *
 * Layout (the owner, 2026-11-21: "equipment should take left hand side of the companion, about 60-65% of the space,
 * and status on the right hand side. Health stamina at the top and souls at the bottom right ... keep bottom left for
 * stats like last hit damage"): across the top the emblem with the HP and stamina bars (the game's pieces at their
 * texture positions, the bars stretched to the screen width; HP against its full maximum, the part lost to hollowing
 * dark) and their numbers; in the middle the four equipment slots in the game's diamond (spell, left hand, right hand,
 * item) with the item's count and name and the next item, icons straight from the game (CompanionIcons), the active
 * effect icons beside them (a tap names one); at the bottom the damage figures on the left and the soul counter on the
 * right. The status build-up stays on the main screen (the owner, 2026-11-22: "rather than having space on the right
 * for status build up etc. We could keep those in main screen where they normally sit and just have the 4 boxes sit
 * more in the center"). Worker thread only.
 *
 * Without a character (title screen, main menu, loading between areas) the screen shows the title screen's logo
 * (CompanionAssets / ds2_companion_assets.py, from the player's disc); in cutscenes and movies the HUD dims. Menus,
 * dialogs and bonfires keep it as it is: the game runs on behind its menus (the owner, 2026-11-22, "trust your
 * recommendation").
 */
final class CompanionView {
    static final float BASE_W = 1240f, BASE_H = 1080f;
    private static final float S = 2.5f;  // game pixels -> base pixels for the vitals block
    private static final float EMBLEM_X = 30, EMBLEM_Y = 40;
    private static final float BARS_X = EMBLEM_X + 84 * S, BARS_Y = EMBLEM_Y + 20 * S;
    private static final float BARS_W = BASE_W - 30 - BARS_X;
    // Bar interiors in the bars piece (texture rows 29-41 and 46-54, columns from 104).
    private static final float IN_X = BARS_X + 20 * S, IN_W = BARS_X + BARS_W - 4 * S - IN_X;
    private static final float HP_Y = BARS_Y + 9 * S, HP_H = 12 * S;
    private static final float ST_Y = BARS_Y + 26 * S, ST_H = 8 * S;
    private static final float SPLIT_ROW = 24.5f;     // the bars piece: HP frame rows 7-24, stamina frame rows 25-36
    // The game's bars grow with the maximum (the owner, 2026-11-22). Measured on the Thor's main screen (1280-wide canvas,
    // px of fill for the full maximum): HP 546 -> 155, 826 -> 213, 1024 -> 248 (a hollowed cap fills its share of
    // that); stamina 820 -> 158, 920 (Status "92") -> 175. A base plus a part per point fits all within 2-4 px.
    private static final float HP_BASE = 48, HP_PX = 0.195f, ST_BASE = 19, ST_PX = 0.17f;
    private static final float BAR_K = 2.0f;          // companion px per canvas px of bar (clamped at IN_W)
    private static final int TEXT = Color.rgb(226, 214, 190);
    private static final int TEXT_HP = Color.rgb(222, 170, 160);
    private static final int TEXT_ST = Color.rgb(176, 198, 150);

    // Effect icons (the owner, 2026-11-22: "quite small right now"): up to eight at EFFECT_BIG in a column right of the
    // right-hand card, then one left of the left-hand card; more at EFFECT, two to a row on each side.
    private static final float EFFECTS_X = 990;       // the small icons' first column on the right
    private static final float EFFECT = 112, EFFECT_BIG = 150;
    private static final float EFFECTS_RIGHT_CX = 1099, EFFECTS_LEFT_CX = 141;
    static final long LABEL_MS = 4000;                // a tapped icon's label shows this long
    private static final float SOULS_X = 790;         // the soul counter's left end
    // The owner, 2026-11-21: "a bit less space for the soul counter so you have more real estate for the weapons and status
    // and can make them a bit bigger": a slim band under the ornament line for the damage figures and the souls.
    // The owner, 2026-11-22: no ornament line at the bottom; the cards a bit further from the HP figures.
    private static final float LOWER = 938;           // damage | souls below this
    private static final float TOP = 286;             // the spell card's and the effect icons' top
    private static final float CARD = 2.8f;           // card scale (the game's card is 79x104)
    private static final float BASELINE = 22;         // the game font's baseline in its 28 px line

    private final Context context;
    private final String labelHit, labelChain, labelTarget, labelTaken;
    private CompanionArt art;
    private int artGeneration;
    private final CompanionIcons icons = new CompanionIcons();
    private final Paint plain = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint bitmapPaint = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
    private final android.graphics.RectF dst = new android.graphics.RectF();
    // The equipment as last drawn by the game (kept while its HUD is hidden: menus, cutscenes).
    private final long[] slots = new long[5];  // left, right, spell, item, next
    private String itemCount = "", itemName = "";
    private CompanionState state = new CompanionState();
    private String signature = "";
    private int width, height;
    private float scale = 1, ox, oy;
    // Eased values (the game's bars slide; damage leaves a short yellow trail that drains after a moment).
    private float hpShown = -1, hpTrail = -1, stShown = -1;
    private long trailHold, lastAnim;
    // A gap without a player shorter than COVER_DELAY_MS (the player object rebuilt on a warp or a load) keeps the
    // last picture. Neither the HUD's slots nor another layout (menus, dialogs, messages, the loading screen) drawn
    // for QUIET_MS: a cutscene or a movie, the HUD dims (black at DIM_LEVEL over it, eased in ~1 s, out in ~0.4 s).
    private static final long COVER_DELAY_MS = 1000, QUIET_MS = 1200;
    private static final float DIM_LEVEL = 0.65f;
    private long lastPlayerMs = -1, quietSince = -1;
    private float dim;
    // The player object the game has put its HUD up for (its HP gauge), and the one drawn.
    private long readyPid, shownPid;
    // The effect icons as last drawn (base coordinates) and the tapped one's label (the owner, 2026-11-22: "a little weapon
    // with an arrow up. would be good if i could tap it and there was some kind of hover label to say what it is").
    private final int[] effectIds = new int[12];
    private final Bitmap[] effectBitmaps = new Bitmap[12];
    private final float[] effectRects = new float[4 * 12];
    private final android.graphics.Rect effectSrc = new android.graphics.Rect();
    private int effectCount, labelId;
    private long labelUntil;
    private final android.util.SparseArray<String[]> effectLabels = new android.util.SparseArray<>();

    CompanionView(Context context) {
        this.context = context;
        labelHit = context.getString(R.string.companion_last_hit);
        labelChain = context.getString(R.string.companion_chain);
        labelTarget = context.getString(R.string.companion_target);
        labelTaken = context.getString(R.string.companion_taken);
        for (String e : context.getResources().getStringArray(R.array.companion_effects)) {
            String[] f = e.split("\\|", 3);  // id|name|where it comes from
            if (f.length == 3) effectLabels.put(Integer.parseInt(f[0]), new String[] { f[1], f[2] });
        }
        artGeneration = CompanionAssets.generation();
        art = new CompanionArt(context);
        plain.setTypeface(Typeface.SERIF);
    }

    int getWidth() { return width; }
    int getHeight() { return height; }

    void layout(int w, int h) {
        width = w;
        height = h;
        scale = Math.min(w / BASE_W, h / BASE_H);
        ox = (w - BASE_W * scale) / 2;
        oy = (h - BASE_H * scale) / 2;
    }

    boolean inGame() { return state.player; }

    /** True when the picture changes. */
    boolean setState(CompanionState s) {
        long now = android.os.SystemClock.uptimeMillis();
        if (s.player) {
            lastPlayerMs = now;
        } else if (lastPlayerMs >= 0 && now - lastPlayerMs < COVER_DELAY_MS) {
            return false;  // a short gap: keep the last picture
        }
        // A character shows once the game has put up its HUD for it (its HP gauge): the opening sequence, a load and a
        // warp set the player object up before the HUD appears, its figures still settling (the owner, 2026-11-22: empty
        // boxes during the opening, the HP bar going up and down while teleporting). Until then the last picture stays
        // (a warp from a shown character), else the cover.
        if (!s.player) readyPid = 0;
        else if (s.gauge || s.hud) readyPid = s.pid;
        if (s.player && s.pid != readyPid) {
            if (state.player) return false;
            s = new CompanionState();  // the cover
        }
        if (s.player && s.pid != shownPid) {  // another character, or the same one after a load: nothing carries over
            shownPid = s.pid;
            hpShown = hpTrail = stShown = -1;
            java.util.Arrays.fill(slots, 0);
            itemCount = itemName = "";
            signature = "";
        }
        if (!s.player) shownPid = 0;
        if (s.player && !s.hud && !s.ui) {
            if (quietSince < 0) quietSince = now;
        } else {
            quietSince = -1;
        }
        state = s;
        if (artGeneration != CompanionAssets.generation()) {  // the art was just built from the game files
            artGeneration = CompanionAssets.generation();
            art = new CompanionArt(context);
            signature = "";
        }
        if (s.hud) {
            slots[0] = s.eqLeft;
            slots[1] = s.eqRight;
            slots[2] = s.eqSpell;
            slots[3] = s.eqItem;
            slots[4] = s.eqNext;
            itemCount = s.itemCount;
            itemName = s.itemName;
        }
        if (s.player && hpShown < 0) {
            hpShown = hpTrail = s.hp;
            stShown = s.stamina;
        }
        String sig = s.signature();
        if (sig.equals(signature)) return false;
        signature = sig;
        if (s.player && s.hp < hpShown) trailHold = android.os.SystemClock.uptimeMillis() + 450;
        return true;
    }

    /** Advances the easing; true while anything still moves. */
    boolean animate(long now) {
        float dt = lastAnim == 0 ? 0.016f : Math.min(0.1f, (now - lastAnim) / 1000f);
        lastAnim = now;
        if (!state.player) {
            hpShown = hpTrail = stShown = -1;
            dim = 0;
            return false;
        }
        boolean moving = false;
        float dimTarget = quietSince >= 0 && now - quietSince >= QUIET_MS ? 1f : 0f;
        if (dim != dimTarget) {
            dim += (dimTarget - dim) * (1f - (float) Math.exp(-dt * (dimTarget > dim ? 3f : 8f)));
            if (Math.abs(dimTarget - dim) < 0.01f) dim = dimTarget;
            moving = true;
        }
        float k = 1f - (float) Math.exp(-dt * 14f);
        float hpTarget = state.hp, stTarget = state.stamina;
        if (Math.abs(hpShown - hpTarget) > 0.5f) { hpShown += (hpTarget - hpShown) * k; moving = true; } else hpShown = hpTarget;
        if (Math.abs(stShown - stTarget) > 0.5f) { stShown += (stTarget - stShown) * k; moving = true; } else stShown = stTarget;
        if (hpTrail < hpShown) {
            hpTrail = hpShown;
        } else if (hpTrail > hpShown) {
            if (now >= trailHold) {
                float kt = 1f - (float) Math.exp(-dt * 6f);
                hpTrail += (hpShown - hpTrail) * kt;
                if (hpTrail - hpShown < 0.5f) hpTrail = hpShown;
            }
            moving = true;
        }
        return moving;
    }

    /**
     * A tap at (x, y) in view pixels (worker thread): names the effect icon there for LABEL_MS, or hides the label.
     * True when the picture changes.
     */
    boolean tap(float x, float y, long now) {
        float bx = (x - ox) / scale, by = (y - oy) / scale;
        int hit = 0;
        for (int i = 0; i < effectCount; i++) {
            float l = effectRects[4 * i] - 14, t = effectRects[4 * i + 1] - 14;
            float r = effectRects[4 * i + 2] + 14, b = effectRects[4 * i + 3] + 14;
            if (bx >= l && bx <= r && by >= t && by <= b) {
                hit = effectIds[i];
                break;
            }
        }
        if (hit != 0 && hit != labelId) {
            labelId = hit;
            labelUntil = now + LABEL_MS;
            return true;
        }
        if (labelId == 0) return false;
        labelId = 0;  // a tap elsewhere, or on the same icon again
        return true;
    }

    void draw(Canvas c) {
        long now = android.os.SystemClock.uptimeMillis();
        effectCount = 0;
        c.drawColor(Color.rgb(10, 9, 8));
        c.save();
        c.translate(ox, oy);
        c.scale(scale, scale);
        c.clipRect(0, 0, BASE_W, BASE_H);
        background(c);
        if (state.player) {
            vitals(c);
            equipment(c);
            effects(c);
            damage(c);
            souls(c);
            effectLabel(c, now);
            if (dim > 0) {
                plain.setColor(Color.argb(Math.round(255 * DIM_LEVEL * dim), 0, 0, 0));
                c.drawRect(0, 0, BASE_W, BASE_H, plain);
            }
        } else {
            cover(c);
        }
        c.restore();
    }

    /** The title screen's logo, centred (no character: title screen, main menu, loading between areas). */
    private void cover(Canvas c) {
        int[] s = art.size("title_logo");
        if (s[0] <= 0) return;
        float w = s[0] * 0.95f, h = s[1] * 0.95f;  // about the game's size: the logo is not upscaled
        art.draw(c, "title_logo", (BASE_W - w) / 2, (BASE_H - h) / 2 - 20, w, h, 255);
    }

    private void background(Canvas c) {
        if (art.ok()) {
            art.draw(c, "leather", -30, -40, BASE_W + 60, BASE_H + 80, 255);
        }
        // Vignette: darker towards the edges, like the game's menu panels.
        plain.setShader(new android.graphics.RadialGradient(BASE_W / 2, BASE_H / 2, BASE_W * 0.75f,
            new int[] { Color.argb(0, 0, 0, 0), Color.argb(150, 0, 0, 0) }, new float[] { 0.55f, 1f }, Shader.TileMode.CLAMP));
        c.drawRect(0, 0, BASE_W, BASE_H, plain);
        plain.setShader(null);
    }

    private void vitals(Canvas c) {
        CompanionState s = state;
        float max = Math.max(1, s.hpMax);
        float cap = s.hpCap > 0 ? Math.min(s.hpCap, s.hpMax) : s.hpMax;
        // Each bar as long as the game makes it for its maximum (fill widths; the frames and end caps follow them).
        final float hpW = Math.max(110, Math.min(IN_W, (HP_BASE + HP_PX * max) * BAR_K));
        final float stW = Math.max(110, Math.min(IN_W, (ST_BASE + ST_PX * Math.max(1, s.staminaMax)) * BAR_K));
        if (art.ok()) {
            art.draw(c, "emblem", EMBLEM_X, EMBLEM_Y, 92 * S, 88 * S, 255);
            frame(c, hpW, 0, SPLIT_ROW);
            frame(c, stW, SPLIT_ROW, 42);
        } else {
            plain.setColor(Color.rgb(40, 36, 32));
            c.drawCircle(EMBLEM_X + 46 * S, EMBLEM_Y + 44 * S, 34 * S, plain);
            plain.setColor(Color.rgb(28, 26, 24));
            c.drawRect(IN_X, HP_Y, IN_X + hpW, HP_Y + HP_H, plain);
            c.drawRect(IN_X, ST_Y, IN_X + stW, ST_Y + ST_H, plain);
        }
        // Hollowing lowers the cap: that part of the bar is dark, beyond the end cap (as the game draws it).
        final float capW = hpW * cap / max;
        if (cap < max) {
            plain.setColor(Color.argb(235, 18, 14, 12));
            c.drawRect(IN_X + capW, HP_Y, IN_X + hpW, HP_Y + HP_H, plain);
        }
        float hpFrac = Math.max(0, hpShown) / max, trailFrac = Math.max(0, hpTrail) / max;
        float stFrac = s.staminaMax > 0 ? Math.max(0, stShown) / s.staminaMax : 0;
        if (art.ok()) {
            if (trailFrac > hpFrac) art.drawFill(c, "fill_flash", IN_X, HP_Y, hpW, HP_H, trailFrac, 255);
            art.drawFill(c, "fill_hp", IN_X, HP_Y, hpW, HP_H, hpFrac, 255);
            art.drawFill(c, "fill_st", IN_X, ST_Y, stW, ST_H, stFrac, 255);
            // The end caps sit over the fills' ends, as in the game.
            art.draw(c, "cap_hp", IN_X + capW - 24 * S, BARS_Y + 5 * S, 34 * S, 21 * S, 255);
            art.draw(c, "cap_st", IN_X + stW - 24 * S, BARS_Y + 23.5f * S, 34 * S, 14 * S, 255);
        } else {
            plain.setColor(Color.rgb(200, 170, 60));
            c.drawRect(IN_X, HP_Y, IN_X + hpW * trailFrac, HP_Y + HP_H, plain);
            plain.setColor(Color.rgb(150, 30, 30));
            c.drawRect(IN_X, HP_Y, IN_X + hpW * hpFrac, HP_Y + HP_H, plain);
            plain.setColor(Color.rgb(60, 130, 60));
            c.drawRect(IN_X, ST_Y, IN_X + stW * stFrac, ST_Y + ST_H, plain);
        }
        // Figures under the bars: HP at the left, stamina ending under the stamina bar's end. The game keeps stamina
        // at 10x what its Status screen shows (920 = "Stamina 92"; Thor, 2026-11-22).
        float ty = ST_Y + ST_H + 16;
        String hpText = s.hp + " / " + Math.round(cap);
        String stText = Math.round(s.stamina / 10f) + " / " + Math.round(s.staminaMax / 10f);
        text(c, hpText, IN_X, ty, 1.15f, TEXT_HP, -1);
        float stRight = Math.max(IN_X + stW, IN_X + textWidth(hpText, 1.15f) + 40 + textWidth(stText, 1.0f));
        text(c, stText, stRight, ty + 3, 1.0f, TEXT_ST, 1);
    }

    /**
     * One bar's frame from the bars piece: rows r0-r1 (game pixels) of the connector + frame, stretched so its fill
     * area is fillW wide.
     */
    private void frame(Canvas c, float fillW, float r0, float r1) {
        float w = (IN_X - BARS_X) + fillW + 4 * S;
        c.save();
        c.clipRect(BARS_X - 1, BARS_Y + r0 * S, BARS_X + w + 1, BARS_Y + r1 * S);
        art.drawThreeSlice(c, "bars", BARS_X, BARS_Y, w, 42 * S, 52, 14);
        c.restore();
    }

    // The four equipment slots in the game's diamond: spell on top, left and right hand, the item below with its count
    // and name and the next item small at its side.
    private void equipment(Canvas c) {
        final float cx = BASE_W / 2, fh = 104 * CARD, fw = 79 * CARD;
        final float top = TOP;
        card(c, cx, top, CARD, slots[2]);                          // spell
        card(c, cx - fw * 1.08f, top + fh * 0.52f, CARD, slots[0]);  // left hand
        card(c, cx + fw * 1.08f, top + fh * 0.52f, CARD, slots[1]);  // right hand
        float itemTop = top + fh * 1.04f;
        card(c, cx, itemTop, CARD, slots[3]);                      // item
        if (slots[4] != 0) card(c, cx + fw * 0.78f, itemTop + fh * 0.62f, CARD * 0.42f, slots[4]);  // next item
        float ty = itemTop + fh + 8;
        float cw = itemCount.isEmpty() ? 0 : textWidth(itemCount, 1.6f) + 30;
        float nw = itemName.isEmpty() ? 0 : textWidth(itemName, 1.6f);
        float x = cx - (cw + nw) / 2;
        if (!itemCount.isEmpty()) text(c, itemCount, x, ty, 1.6f, TEXT, -1);
        if (!itemName.isEmpty()) text(c, itemName, x + cw, ty, 1.6f, TEXT, -1);
    }

    /** A slot card: dark back, the item plate, the game's icon, the ornate frame. */
    private void card(Canvas c, float cx, float top, float sc, long hash) {
        float fw = 79 * sc, fh = 104 * sc, x = cx - fw / 2;
        if (art.ok()) {
            art.draw(c, "card_back", x + 2 * sc, top + 5 * sc, 75 * sc, 94 * sc, 235);
            art.draw(c, "plate", cx - 34.5f * sc, top + fh - 37 * sc, 69 * sc, 31 * sc, 255);
        } else {
            plain.setColor(Color.rgb(24, 22, 20));
            c.drawRect(x, top, x + fw, top + fh, plain);
        }
        Bitmap icon = icons.get(hash, art);
        if (icon != null) {
            float iw = 64 * sc * 1.05f, ih = iw * 2;
            float icy = top + fh * 0.47f;           // the art is centred at ~47 % of the 64x128 icon
            dst.set(cx - iw / 2, icy - ih * 0.47f, cx + iw / 2, icy + ih * 0.53f);
            c.save();
            c.clipRect(x + 3 * sc, top + 3 * sc, x + fw - 3 * sc, top + fh - 3 * sc);
            c.drawBitmap(icon, null, dst, bitmapPaint);
            c.restore();
        }
        if (art.ok()) art.draw(c, "card_frame", x, top, fw, fh, 255);
    }

    // Active effect icons the game's HUD draws (64x64 textures recognised by their data, CompanionArt.effectId), beside
    // the equipment from the spell card's top down (the owner, 2026-11-22: "top right or left of boxes", "quite small").
    private void effects(Canvas c) {
        int n = 0;
        for (Long h : state.smallIcons) {
            if (n == effectIds.length) break;
            int id = art.effectId(h);
            Bitmap b = id != 0 ? art.effect(id) : null;
            if (b == null) continue;
            effectIds[n] = id;
            effectBitmaps[n++] = b;
        }
        boolean big = n <= 8;
        float size = big ? EFFECT_BIG : EFFECT, gap = big ? 16 : 10;
        int shown = 0;
        for (int i = 0; i < n; i++) {
            float x, y;
            if (big) {  // a column right of the right-hand card, then one left of the left-hand card
                x = (i < 4 ? EFFECTS_RIGHT_CX : EFFECTS_LEFT_CX) - size / 2;
                y = TOP + (i % 4) * (size + gap);
            } else {    // two to a row, right then left
                int k = i % 10;
                x = (i < 10 ? EFFECTS_X : 20) + (k % 2) * (size + gap);
                y = TOP + (k / 2) * (size + gap);
                if (y + size > LOWER - 4) continue;
            }
            // The icons' art fills only the middle of their textures (all 27 within 59-190 x 66-192 of 256, Thor
            // 2026-11-22: a 150 px slot showed ~60 px of art): the middle 138 x 138 (of 256) fills the slot.
            Bitmap bm = effectBitmaps[i];
            effectSrc.set(bm.getWidth() * 56 / 256, bm.getHeight() * 60 / 256, bm.getWidth() * 194 / 256,
                          bm.getHeight() * 198 / 256);
            dst.set(x, y, x + size, y + size);
            c.drawBitmap(bm, effectSrc, dst, bitmapPaint);
            effectIds[shown] = effectIds[i];
            effectRects[4 * shown] = x;
            effectRects[4 * shown + 1] = y;
            effectRects[4 * shown + 2] = x + size;
            effectRects[4 * shown + 3] = y + size;
            shown++;
        }
        effectCount = shown;
        java.util.Arrays.fill(effectBitmaps, null);
    }

    /** The tapped effect icon's name and where it comes from, in a panel beside it, for LABEL_MS. */
    private void effectLabel(Canvas c, long now) {
        if (labelId == 0) return;
        int k = -1;
        for (int i = 0; i < effectCount; i++)
            if (effectIds[i] == labelId) k = i;
        if (k < 0 || now >= labelUntil) {  // the effect ended, or the label's time is up
            labelId = 0;
            return;
        }
        float l = effectRects[4 * k], t = effectRects[4 * k + 1], r = effectRects[4 * k + 2], b = effectRects[4 * k + 3];
        String[] lab = effectLabels.get(labelId);
        String name = lab != null ? lab[0] : context.getString(R.string.companion_effect_unknown);
        java.util.List<String> lines = wrap(lab != null ? lab[1] : "", 0.95f, 620);
        final float pad = 22, nameSc = 1.25f, lineSc = 0.95f, lineH = 28 * lineSc + 6;
        float w = textWidth(name, nameSc);
        for (String line : lines) w = Math.max(w, textWidth(line, lineSc));
        w += 2 * pad;
        float h = 2 * pad + 28 * nameSc + (lines.isEmpty() ? 0 : 10 + lines.size() * lineH);
        float px = l > BASE_W / 2 ? l - 16 - w : r + 16;
        px = Math.max(12, Math.min(BASE_W - 12 - w, px));
        float py = Math.max(100, Math.min(LOWER - h, (t + b) / 2 - h / 2));
        plain.setStyle(Paint.Style.STROKE);  // the icon picked
        plain.setStrokeWidth(3);
        plain.setColor(Color.rgb(196, 170, 112));
        c.drawRoundRect(l - 6, t - 6, r + 6, b + 6, 10, 10, plain);
        plain.setStyle(Paint.Style.FILL);
        plain.setColor(Color.argb(238, 16, 14, 12));
        c.drawRoundRect(px, py, px + w, py + h, 12, 12, plain);
        plain.setStyle(Paint.Style.STROKE);
        plain.setStrokeWidth(2);
        plain.setColor(Color.rgb(150, 130, 90));
        c.drawRoundRect(px, py, px + w, py + h, 12, 12, plain);
        plain.setStyle(Paint.Style.FILL);
        text(c, name, px + pad, py + pad, nameSc, Color.rgb(240, 222, 170), -1);
        float y = py + pad + 28 * nameSc + 10;
        for (String line : lines) {
            text(c, line, px + pad, y, lineSc, TEXT, -1);
            y += lineH;
        }
    }

    /** Words of s in lines at most maxW wide at text scale sc. */
    private java.util.List<String> wrap(String s, float sc, float maxW) {
        java.util.List<String> lines = new java.util.ArrayList<>();
        String line = "";
        for (String word : s.split(" ")) {
            if (word.isEmpty()) continue;
            String next = line.isEmpty() ? word : line + " " + word;
            if (!line.isEmpty() && textWidth(next, sc) > maxW) {
                lines.add(line);
                line = word;
            } else {
                line = next;
            }
        }
        if (!line.isEmpty()) lines.add(line);
        return lines;
    }

    // Damage figures: the last hit on an enemy, the chain of hits less than 3 s apart, that enemy's HP now, and the last
    // damage the player took (the game shows the damage dealt over the enemy's own bar, not the rest). Fades a while
    // after the last hit.
    private void damage(Canvas c) {
        CompanionState s = state;
        if (s.hit <= 0 && s.taken <= 0) return;
        float fade = s.hitAge < 0 || s.hitAge < 8000 ? 1f : Math.max(0.35f, 1f - (s.hitAge - 8000) / 8000f);
        int a = Math.round(255 * fade);
        // Two rows on shared baselines (the owner, 2026-11-22: "last hit, taken are not inline"): what the player dealt and
        // took side by side, the chain and the target's HP under them; labels and figures in fixed columns.
        final float lx = 50, vx = 175, lx2 = 380, vx2 = 495, b1 = LOWER + 66, b2 = b1 + 54;
        final float ls = 0.95f, vs = 1.35f;
        if (s.hit > 0) {
            text(c, labelHit, lx, b1 - BASELINE * ls, ls, withAlpha(TEXT_ST, a), -1);
            text(c, Integer.toString(s.hit), vx, b1 - BASELINE * vs, vs, withAlpha(Color.rgb(240, 222, 170), a), -1);
        }
        if (s.taken > 0) {
            text(c, labelTaken, lx2, b1 - BASELINE * ls, ls, withAlpha(TEXT_ST, a), -1);
            text(c, Integer.toString(s.taken), vx2, b1 - BASELINE * vs, vs, withAlpha(TEXT_HP, a), -1);
        }
        if (s.combo > s.hit) {
            text(c, labelChain, lx, b2 - BASELINE * ls, ls, withAlpha(TEXT_ST, a), -1);
            text(c, Integer.toString(s.combo), vx, b2 - BASELINE * vs, vs, withAlpha(TEXT, a), -1);
        }
        if (s.targetMax > 0) {  // a dead target: 0, in red (its HP goes below 0)
            text(c, labelTarget, lx2, b2 - BASELINE * ls, ls, withAlpha(TEXT_ST, a), -1);
            text(c, Math.max(0, s.targetHp) + " / " + s.targetMax, vx2, b2 - BASELINE * vs, vs,
                 withAlpha(s.targetHp <= 0 ? Color.rgb(200, 120, 100) : TEXT, a), -1);
        }
    }

    private static int withAlpha(int color, int a) {
        return Color.argb(a, Color.red(color), Color.green(color), Color.blue(color));
    }

    private void souls(Canvas c) {
        if (state.souls < 0) return;
        final float k = 1.75f;  // the game's soul counter, about two thirds of its earlier size
        float y = LOWER + 46;
        final float endW = 27 * k, endH = 35 * k, x0 = SOULS_X, barX = x0 + endW + 26 * k + 6;
        final float barEnd = BASE_W - 26 - endW;
        if (art.ok()) {
            art.draw(c, "soul_end", x0, y - 6, endW, endH, 255);
            art.draw(c, "soul_icon", x0 + endW - 4, y + 4, 26 * k, 26 * k, 255);
            art.drawThreeSlice(c, "bar_dark", barX, y + 6, barEnd - barX, 42, 3, 3);
            c.save();
            c.scale(-1, 1, barEnd + endW / 2, 0);  // the left ornament mirrored at the bar's right end
            art.draw(c, "soul_end", barEnd, y - 6, endW, endH, 255);
            c.restore();
        } else {
            plain.setColor(Color.rgb(20, 18, 16));
            c.drawRect(barX, y + 6, barEnd, y + 48, plain);
        }
        text(c, String.format(java.util.Locale.US, "%,d", state.souls), barEnd - 14, y + 10, 1.15f, TEXT, 1);
    }

    private void text(Canvas c, String s, float x, float y, float sc, int color, int align) {
        if (art.ok() && art.hasGlyphs(s)) {
            art.text(c, s, x, y, sc, color, align);
            return;
        }
        plain.setColor(color);
        plain.setTextSize(26 * sc);
        plain.setTextAlign(align < 0 ? Paint.Align.LEFT : align == 0 ? Paint.Align.CENTER : Paint.Align.RIGHT);
        c.drawText(s, x, y + 24 * sc, plain);
        plain.setTextAlign(Paint.Align.LEFT);
    }

    /** Width of a text as text() draws it (the game's font, or the system serif for characters it lacks). */
    private float textWidth(String s, float sc) {
        if (art.ok() && art.hasGlyphs(s)) return art.textWidth(s, sc);
        plain.setTextSize(26 * sc);
        return plain.measureText(s);
    }
}

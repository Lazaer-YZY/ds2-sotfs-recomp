package com.ds2recomp.android;

import java.util.ArrayList;
import java.util.List;

/**
 * What the game's HUD shows now, parsed from RuntimeActivity.nativeCompanionState's line of key=value pairs
 * (app/src/android/ds2_companion_android.cpp documents the keys). Unknown keys are ignored and missing ones keep their
 * defaults, so the native side can grow without breaking the screen.
 */
final class CompanionState {
    /** One status build-up bar (the game's l01_06_tolerance): poison, toxic, curse, petrify. */
    static final class Buildup {
        final String name;
        final float value, max;

        Buildup(String name, float value, float max) {
            this.name = name;
            this.value = value;
            this.max = max;
        }

        float fraction() { return max > 0 ? Math.max(0f, Math.min(1f, value / max)) : 0f; }
    }

    boolean player;
    long pid;  // the player object's address: a load or a warp builds a new one
    int hp, hpMax, hpCap;
    float stamina, staminaMax;
    long souls = -1;
    final List<Buildup> buildups = new ArrayList<>();
    // Equipment as the game's HUD draws it (hashes of the icon textures, 0 = empty; CompanionIcons decodes them).
    boolean hud, ui;  // ui: another layout drew lately (menus, dialogs, messages, the loading screen)
    boolean gauge;    // the game's HP gauge drawn lately: its HUD is up, the player can act
    // The equipped items: six hands, four armour, four rings (item id, -1 empty; durability).
    final int[] gearIds = new int[14];
    final int[] gearDur = new int[14];
    { java.util.Arrays.fill(gearIds, -1); }
    long eqLeft, eqRight, eqSpell, eqItem, eqNext;
    String itemCount = "", itemName = "";
    final List<Long> smallIcons = new ArrayList<>();
    long covenant;  // the covenant icon in the HP gauge's emblem (vi_*.tpf data hash, 0 = none; meaningful while gauge)
    // Damage: the last hit on an enemy, ms since, the chain of hits, that enemy's HP now, the player's last loss.
    int hit, combo, targetHp = -1, targetMax, taken;
    long hitAge = -1;

    static CompanionState parse(String line) {
        CompanionState s = new CompanionState();
        if (line == null) return s;
        for (String pair : line.split(";")) {
            int eq = pair.indexOf('=');
            if (eq <= 0) continue;
            String k = pair.substring(0, eq), v = pair.substring(eq + 1);
            try {
                switch (k) {
                    case "player": s.player = "1".equals(v); break;
                    case "pid": s.pid = Long.parseLong(v, 16); break;
                    case "gauge": s.gauge = "1".equals(v); break;
                    case "gear": {
                        String[] items = v.split(",");
                        for (int i = 0; i < items.length && i < s.gearIds.length; i++) {
                            String[] f = items[i].split(":");
                            if (f.length != 2) continue;
                            s.gearIds[i] = Integer.parseInt(f[0]);
                            s.gearDur[i] = Integer.parseInt(f[1]);
                        }
                        break;
                    }
                    case "hp": s.hp = Integer.parseInt(v); break;
                    case "hpmax": s.hpMax = Integer.parseInt(v); break;
                    case "hpcap": s.hpCap = Integer.parseInt(v); break;
                    case "st": s.stamina = Float.parseFloat(v); break;
                    case "stmax": s.staminaMax = Float.parseFloat(v); break;
                    case "souls": s.souls = Long.parseLong(v); break;
                    case "hud": s.hud = "1".equals(v); break;
                    case "ui": s.ui = "1".equals(v); break;
                    case "eqL": s.eqLeft = Long.parseUnsignedLong(v, 16); break;
                    case "eqR": s.eqRight = Long.parseUnsignedLong(v, 16); break;
                    case "eqU": s.eqSpell = Long.parseUnsignedLong(v, 16); break;
                    case "eqD": s.eqItem = Long.parseUnsignedLong(v, 16); break;
                    case "eqN": s.eqNext = Long.parseUnsignedLong(v, 16); break;
                    case "cov": s.covenant = Long.parseUnsignedLong(v, 16); break;
                    case "cnt": s.itemCount = v; break;
                    case "name": s.itemName = v; break;
                    case "fx":
                        for (String h : v.split(",")) if (!h.isEmpty()) s.smallIcons.add(Long.parseUnsignedLong(h, 16));
                        break;
                    case "hit": s.hit = Integer.parseInt(v); break;
                    case "hitage": s.hitAge = Long.parseLong(v); break;
                    case "combo": s.combo = Integer.parseInt(v); break;
                    case "taken": s.taken = Integer.parseInt(v); break;
                    case "tgt": {
                        String[] f = v.split(":");
                        if (f.length == 2) {
                            s.targetHp = Integer.parseInt(f[0]);
                            s.targetMax = Integer.parseInt(f[1]);
                        }
                        break;
                    }
                    case "tol":
                        for (String item : v.split(",")) {
                            String[] f = item.split(":");
                            if (f.length == 3) s.buildups.add(new Buildup(f[0], Float.parseFloat(f[1]), Float.parseFloat(f[2])));
                        }
                        break;
                    default: break;
                }
            } catch (NumberFormatException ignored) {
            }
        }
        return s;
    }

    /** What is drawn, without what only moves a bar a fraction of a pixel; equal signatures need no redraw. */
    String signature() {
        if (!player) return "idle";
        StringBuilder b = new StringBuilder();
        b.append(hp).append('/').append(hpMax).append('/').append(hpCap).append(' ')
         .append(Math.round(stamina)).append('/').append(Math.round(staminaMax)).append(' ').append(souls);
        b.append(' ').append(Long.toHexString(eqLeft)).append(Long.toHexString(eqRight)).append(Long.toHexString(eqSpell))
         .append(Long.toHexString(eqItem)).append(Long.toHexString(eqNext)).append(itemCount).append('|').append(itemName);
        for (Long h : smallIcons) b.append(' ').append(Long.toHexString(h));
        b.append(" c").append(Long.toHexString(covenant));
        b.append(" d").append(hit).append('/').append(combo).append('/').append(targetHp).append('/').append(taken)
         .append('/').append(hitAge < 0 ? -1 : Math.min(hitAge / 2000, 6));  // fades in steps after a hit
        return b.toString();
    }

    /**
     * Made-up values for layout checks (debug launches: --es DS2_COMPANION_DEMO hud). "cycle": 8 s of HUD, 8 s of
     * cutscene (no HUD, no other layout: dims), 8 s without a player (the cover).
     */
    static String demoLine(long ms, String mode) {
        if ("cycle".equals(mode)) {
            long phase = (ms / 8000) % 3;
            if (phase == 2) return "v=1;player=0";
            String line = demoLine(ms, "hud");
            return phase == 1 ? line.replace(";hud=1;", ";hud=0;ui=0;") : line;
        }
        double t = ms / 1000.0;
        int hp = (int) (380 + 240 * (0.5 + 0.5 * Math.sin(t / 2.5)));
        int st = (int) (920 * (0.5 + 0.5 * Math.sin(t * 1.3)));
        float poison = (float) Math.max(0, 100 * Math.sin(t / 3.0));
        float bleed = (float) Math.max(0, 90 * Math.sin(t / 4.0 + 1.5));
        StringBuilder b = new StringBuilder("v=1;player=1;hp=").append(hp).append(";hpmax=620;hpcap=560;st=").append(st)
            .append(";stmax=920;souls=").append(12345 + (long) (t * 37));
        StringBuilder tol = new StringBuilder();
        if (poison > 0) tol.append("poison:").append(poison).append(":100");
        if (bleed > 0) tol.append(tol.length() > 0 ? "," : "").append("toxic:").append(bleed).append(":100");
        tol.append(tol.length() > 0 ? "," : "").append("curse:35:120");
        if (tol.length() > 0) b.append(";tol=").append(tol);
        b.append(";hud=1;gauge=1;cov=1e8b702749527ed4;cnt=12;name=Lifegem;hit=")  // cov: the Way of Blue (vi_0004).append(87 + (int) (t * 7) % 50).append(";hitage=").append((long) (t * 1000) % 4000)
         .append(";combo=233;tgt=145:400;taken=37")
         .append(";fx=bafea98102e7c240,80c2959f32387a8a,10049de63c53eec0")  // attack up, the cat, a broken ring
         // An Iron Parma and a Club, armour, Ring of Blades+2 and Stone Ring (both attack up: the label lists both) and
         // a broken Ring of Giants.
         .append(";gear=11020000:80,2400000:50,-1:0,-1:0,-1:0,-1:0,17440100:65,17440101:65,17440102:65,17440103:65,")
         .append("-1:0,40230000:110,40160002:110,40290000:0");
        return b.toString();
    }
}

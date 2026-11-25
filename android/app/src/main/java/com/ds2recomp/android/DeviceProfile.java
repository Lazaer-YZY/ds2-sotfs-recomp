package com.ds2recomp.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;

import java.io.File;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * First-start settings by GPU (the owner, 2026-11-25: "set default for first launch tailored to what cpu/gpu player
 * has ... they could be changed later"). The resolution and frame rate a GPU gets until the player saves their own in
 * CTRL, from the player reports (DECISIONS.md, 2026-11-25):
 * <ul>
 * <li>Adreno 730 and newer (the Thor's 740, 8s Gen 3's 735, the S25's 830): 720p / 60.</li>
 * <li>Adreno 650 / 660: 540p / 30 (issue #24: an Adreno 660 faults at 720p; 540p in sysmem mode ran 30 min), with
 * Turnip in sysmem mode (GpuDriverStore.needsTurnipSysmem).</li>
 * <li>Adreno 640: 720p / 30 (the test S10: 41.6 fps at 720p). Adreno 642L-644, 630, 7xx below 725: 540p / 30 (#5:
 * an Adreno 710 at ~23 fps at 720p). Adreno 610-620: 432p / 30. Adreno 5xx: 360p / 30.</li>
 * <li>Mali-G615 MC6 class (Dimensity 8100-8400, 1100 / 1200): 432p / 30 (issues #16 / #17 / #20 / #21 hold ~30);
 * Dimensity 9000 series: 540p / 30 (untested, about twice the cores); the other MediaTek Malis (G615 MC2, G57, G68,
 * G52): 360p / 30. A Mali the table does not know: 432p / 30.</li>
 * <li>PowerVR: 360p / 30 (issue #25: ~8 fps at 720p). Anything else: 720p / 30.</li>
 * </ul>
 * Mali phones do not let apps read the GPU name (every Mali log has gpu=null) and one Adreno handheld reports
 * "Adreno22" for its Adreno 735 (issue #6), so the chip (Build.SOC_MODEL) decides when the name does not.
 */
final class DeviceProfile {
    static final String KEY_PROFILE = "gpu_profile";

    /** One row of the table: a short id for logs, the GPU's name for CTRL, and what it starts with. */
    static final class Profile {
        final String id;
        final String gpu;
        final String renderHeight;
        final String fps;

        Profile(String id, String gpu, String renderHeight, String fps) {
            this.id = id;
            this.gpu = gpu;
            this.renderHeight = renderHeight;
            this.fps = fps;
        }

        @Override public String toString() {
            return id + " (" + gpu + "): " + renderHeight + "p, " + fps + " fps";
        }
    }

    private DeviceProfile() {}

    /** Adreno by Qualcomm chip, for when the sysfs name says nothing usable. */
    private static final Map<String, Integer> QUALCOMM_ADRENO = new HashMap<>();
    /** MediaTek chips with a Mali: the GPU, and its resolution. Other MediaTek Malis get 360p. */
    private static final Map<String, String[]> MEDIATEK_MALI = new HashMap<>();
    static {
        String[][] qualcomm = {
            { "SM8750", "830" }, { "SM8735", "825" }, { "SM8650", "750" }, { "SM8635", "735" }, { "SG6275P", "735" },
            { "SM8550", "740" }, { "SM8475", "730" }, { "SM8450", "730" }, { "SM7675", "732" }, { "SM7475", "725" },
            { "SM7550", "720" }, { "SM7435", "710" }, { "SM6450", "710" }, { "SM8350", "660" }, { "SM8250", "650" },
            { "SM7450", "644" }, { "SM7325", "642" }, { "SM7350", "642" }, { "SM8150", "640" }, { "SM7250", "620" },
            { "SM7225", "619" }, { "SM6375", "619" }, { "SM7150", "618" }, { "SM6225", "610" }, { "SM6115", "610" },
        };
        for (String[] row : qualcomm) QUALCOMM_ADRENO.put(row[0], Integer.parseInt(row[1]));
        String[][] mediatek = {
            { "MT6991", "Immortalis-G925 MC12", "540" }, { "MT6989", "Immortalis-G720 MC12", "540" },
            { "MT6985", "Immortalis-G715 MC11", "540" }, { "MT6983", "Mali-G710 MC10", "540" },
            { "MT6899", "Mali-G720 MC7", "432" }, { "MT6897", "Mali-G615 MC6", "432" },
            { "MT6896", "Mali-G610 MC6", "432" }, { "MT6895", "Mali-G610 MC6", "432" },
            { "MT6893", "Mali-G77 MC9", "432" }, { "MT6891", "Mali-G77 MC9", "432" },
        };
        for (String[] row : mediatek) MEDIATEK_MALI.put(row[0], new String[] { row[1], row[2] });
    }

    private static final Pattern ADRENO = Pattern.compile("Adreno\\D*(\\d+)", Pattern.CASE_INSENSITIVE);
    private static final Pattern MALI = Pattern.compile("(?:mali|immortalis)[- ]?g[- ]?(\\d+)", Pattern.CASE_INSENSITIVE);
    private static final Pattern CORES = Pattern.compile("(\\d+)\\s*cores|\\b(?:MC|MP)(\\d+)", Pattern.CASE_INSENSITIVE);
    private static final Pattern CHIP = Pattern.compile("(SM\\d{4}|SG\\d{4}[A-Z]?|MT\\d{4})", Pattern.CASE_INSENSITIVE);

    /** "SM8350", "MT6897", ... from a SoC model string; null when it names none. */
    static String chip(String soc) {
        if (soc == null) return null;
        Matcher matcher = CHIP.matcher(soc);
        return matcher.find() ? matcher.group(1).toUpperCase(Locale.ROOT) : null;
    }

    /** The Adreno number from the sysfs name, else from the chip; 0 when neither knows it. */
    static int adrenoNumber(String gpuModel, String soc) {
        if (gpuModel != null) {
            Matcher matcher = ADRENO.matcher(gpuModel);
            if (matcher.find()) {
                try {
                    int number = Integer.parseInt(matcher.group(1));
                    if (number >= 100 && number < 1000) return number;
                } catch (NumberFormatException ignored) {
                    // the chip below
                }
            }
        }
        String chip = chip(soc);
        Integer number = chip != null ? QUALCOMM_ADRENO.get(chip) : null;
        return number != null ? number : 0;
    }

    /** The table. {@code kind}: "adreno", "mali", "powervr" or "other"; the GPU name and chip may be null. */
    static Profile forGpu(String kind, String gpuModel, String soc) {
        if ("adreno".equals(kind)) {
            int n = adrenoNumber(gpuModel, soc);
            String gpu = n > 0 ? "Adreno " + n : "Adreno";
            if (n >= 730) return new Profile("adreno_730_plus", gpu, "720", "60");
            if (n == 725) return new Profile("adreno_725", gpu, "720", "30");
            if (n >= 700) return new Profile("adreno_7xx_low", gpu, "540", "30");
            if (n == 650 || n == 660) return new Profile("adreno_650_660", gpu, "540", "30");
            if (n == 640) return new Profile("adreno_640", gpu, "720", "30");
            if (n >= 630) return new Profile("adreno_630_644", gpu, "540", "30");
            if (n >= 600) return new Profile("adreno_6xx_low", gpu, "432", "30");
            if (n >= 100) return new Profile("adreno_5xx", gpu, "360", "30");
            return new Profile("adreno_unknown", gpu, "720", "30");
        }
        if ("mali".equals(kind)) {
            Matcher name = gpuModel != null ? MALI.matcher(gpuModel) : null;
            if (name != null && name.find()) {
                int model = Integer.parseInt(name.group(1));
                int cores = 0;
                Matcher coreMatch = CORES.matcher(gpuModel);
                if (coreMatch.find()) cores = Integer.parseInt(coreMatch.group(1) != null ? coreMatch.group(1) : coreMatch.group(2));
                String gpu = "Mali-G" + model + (cores > 0 ? " MC" + cores : "");
                boolean valhallCsf = model == 610 || model == 615 || model == 620 || model == 710 || model == 715
                    || model == 720 || model == 725 || model == 925;
                if (valhallCsf && cores >= 10) return new Profile("mali_big", gpu, "540", "30");
                if ((valhallCsf && (cores == 0 || cores >= 5)) || ((model == 77 || model == 78) && cores >= 9))
                    return new Profile("mali_mid", gpu, "432", "30");
                return new Profile("mali_small", gpu, "360", "30");
            }
            String chip = chip(soc);
            String[] row = chip != null ? MEDIATEK_MALI.get(chip) : null;
            if (row != null)
                return new Profile("540".equals(row[1]) ? "mali_big" : "mali_mid", row[0] + " (" + chip + ")", row[1], "30");
            if (chip != null && chip.startsWith("MT")) return new Profile("mali_small", "Mali (" + chip + ")", "360", "30");
            return new Profile("mali_unknown", "Mali", "432", "30");
        }
        if ("powervr".equals(kind)) return new Profile("powervr", "PowerVR", "360", "30");
        return new Profile("other", "this GPU", "720", "30");
    }

    /** "SM8350", "MT6897"-style chip name of this phone (Android 12+), else its hardware name. */
    static String socModel() {
        if (Build.VERSION.SDK_INT >= 31 && Build.SOC_MODEL != null && !Build.SOC_MODEL.isEmpty()
                && !Build.UNKNOWN.equals(Build.SOC_MODEL)) {
            return Build.SOC_MODEL;
        }
        return Build.HARDWARE;
    }

    /** This phone's row. */
    static Profile current() {
        GpuDriverStore.Vendor vendor = GpuDriverStore.vendor();
        String soc = socModel();
        String chip = chip(soc);
        // MediaTek's GPUs are Mali or PowerVR (issue #25's MT6855G: PowerVR BXM-8-256, no /dev/mali0).
        String kind = vendor == GpuDriverStore.Vendor.ADRENO ? "adreno"
            : vendor == GpuDriverStore.Vendor.MALI ? "mali"
            : new File("/dev/pvrsrvkm").exists() || (chip != null && chip.startsWith("MT")) ? "powervr" : "other";
        return forGpu(kind, GpuDriverStore.gpuModel(), soc);
    }

    /**
     * Fills the resolution and frame rate the player has not saved yet (a first start, or a player who never saved
     * CTRL; any CTRL save stores both) with this GPU's row, and notes the row in {@link #KEY_PROFILE} (it shows in
     * Save logs' device.txt). Returns what it did, for the logs.
     */
    static String seedDefaults(Context context) {
        SharedPreferences render = RuntimeActivity.renderPrefs(context);
        Profile profile = current();
        SharedPreferences.Editor editor = render.edit().putString(KEY_PROFILE, profile.id);
        StringBuilder seeded = new StringBuilder();
        if (!render.contains("render_height")) {
            editor.putString("render_height", profile.renderHeight);
            seeded.append("render_height=").append(profile.renderHeight);
        }
        if (!render.contains("fps")) {
            editor.putString("fps", profile.fps);
            seeded.append(seeded.length() > 0 ? ", " : "").append("fps=").append(profile.fps);
        }
        editor.commit();
        String result = "GPU profile " + profile + (seeded.length() > 0 ? "; first-start settings " + seeded
            : "; the player's own resolution and frame rate kept");
        android.util.Log.i("ds2recomp", result);
        return result;
    }
}

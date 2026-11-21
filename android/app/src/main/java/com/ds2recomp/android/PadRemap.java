package com.ds2recomp.android;

import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.util.TypedValue;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;

/**
 * CTRL -> Controller buttons (issue #9): which game button each of a physical controller's buttons presses. Stored
 * as the render setting "pad_map" (the targets of a b x y lb rb lt rt back start l3 r3, comma-separated, "none" for
 * no button; "" = as labelled), handed to the game as DS2_PAD_MAP at start and through nativeSetOption("pad_map") at
 * once (ds2_android.cpp SetPadMap). The touch controls are not remapped.
 */
final class PadRemap {
    static final String[] IDS = { "a", "b", "x", "y", "lb", "rb", "lt", "rt", "back", "start", "l3", "r3" };
    private static final String[] LABELS = { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Back", "Start", "L3", "R3" };
    private static final int NONE = IDS.length;

    private PadRemap() {}

    /** The stored map for DS2_PAD_MAP ("" = as labelled). */
    static String setting(SharedPreferences render) {
        return render.getString("pad_map", "");
    }

    /** Target index of each button from a stored map (as labelled where missing or unknown). */
    private static int[] parse(String map) {
        int[] targets = new int[IDS.length];
        for (int i = 0; i < targets.length; ++i) targets[i] = i;
        String[] parts = map == null || map.isEmpty() ? new String[0] : map.split(",", -1);
        for (int i = 0; i < Math.min(parts.length, targets.length); ++i) {
            int target = java.util.Arrays.asList(IDS).indexOf(parts[i]);
            targets[i] = target >= 0 ? target : "none".equals(parts[i]) ? NONE : i;
        }
        return targets;
    }

    private static String format(int[] targets) {
        boolean identity = true;
        StringBuilder map = new StringBuilder();
        for (int i = 0; i < targets.length; ++i) {
            if (i > 0) map.append(',');
            map.append(targets[i] == NONE ? "none" : IDS[targets[i]]);
            identity &= targets[i] == i;
        }
        return identity ? "" : map.toString();
    }

    static void show(RuntimeActivity activity) {
        SharedPreferences render = RuntimeActivity.renderPrefs(activity);
        int[] targets = parse(setting(render));
        float density = activity.getResources().getDisplayMetrics().density;
        int padding = Math.round(20f * density);
        LinearLayout content = new LinearLayout(activity);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(padding, Math.round(8f * density), padding, 0);
        TextView help = new TextView(activity);
        help.setText(activity.getString(R.string.pad_remap_help));
        help.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        help.setAlpha(.7f);
        content.addView(help);

        Spinner[] spinners = new Spinner[IDS.length];
        // Common layouts in one tap, above the list so they show without scrolling: A/B swapped (Nintendo-style pads),
        // bumpers and triggers swapped.
        LinearLayout presets = new LinearLayout(activity);
        presets.setOrientation(LinearLayout.HORIZONTAL);
        Button swapAb = new Button(activity);
        swapAb.setText(activity.getString(R.string.pad_remap_swap_ab));
        swapAb.setAllCaps(false);
        swapAb.setOnClickListener(v -> swap(spinners, 0, 1));
        Button swapShoulders = new Button(activity);
        swapShoulders.setText(activity.getString(R.string.pad_remap_swap_shoulders));
        swapShoulders.setAllCaps(false);
        swapShoulders.setOnClickListener(v -> { swap(spinners, 4, 6); swap(spinners, 5, 7); });
        presets.addView(swapAb);
        presets.addView(swapShoulders);
        content.addView(presets);
        String[] choices = new String[LABELS.length + 1];
        System.arraycopy(LABELS, 0, choices, 0, LABELS.length);
        choices[NONE] = activity.getString(R.string.pad_remap_none);
        for (int i = 0; i < IDS.length; ++i) {
            LinearLayout row = new LinearLayout(activity);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(android.view.Gravity.CENTER_VERTICAL);
            TextView label = new TextView(activity);
            label.setText(activity.getString(R.string.pad_remap_row, LABELS[i]));
            label.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
            row.addView(label, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
            Spinner spinner = new Spinner(activity);
            ArrayAdapter<String> adapter = new ArrayAdapter<>(activity, android.R.layout.simple_spinner_item, choices);
            adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
            spinner.setAdapter(adapter);
            spinner.setSelection(targets[i]);
            row.addView(spinner, new LinearLayout.LayoutParams(Math.round(140f * density),
                                                               LinearLayout.LayoutParams.WRAP_CONTENT));
            content.addView(row);
            spinners[i] = spinner;
        }

        ScrollView scroll = new ScrollView(activity);
        scroll.addView(content);
        AlertDialog dialog = new AlertDialog.Builder(activity)
            .setTitle(activity.getString(R.string.pad_remap_title))
            .setView(scroll)
            .setPositiveButton(activity.getString(R.string.touch_apply), (d, which) -> {
                int[] chosen = new int[IDS.length];
                for (int i = 0; i < IDS.length; ++i) chosen[i] = spinners[i].getSelectedItemPosition();
                String map = format(chosen);
                render.edit().putString("pad_map", map).apply();
                RuntimeActivity.nativeSetOption("pad_map", map);
            })
            .setNeutralButton(activity.getString(R.string.pad_remap_reset), null)
            .setNegativeButton(activity.getString(R.string.touch_cancel), null)
            .create();
        // Reset only sets the choices back (the dialog stays open until Apply).
        dialog.setOnShowListener(d -> dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v -> {
            for (int i = 0; i < IDS.length; ++i) spinners[i].setSelection(i);
        }));
        dialog.show();
    }

    /** Swaps what buttons {@code a} and {@code b} press. */
    private static void swap(Spinner[] spinners, int a, int b) {
        int first = spinners[a].getSelectedItemPosition();
        spinners[a].setSelection(spinners[b].getSelectedItemPosition());
        spinners[b].setSelection(first);
    }
}

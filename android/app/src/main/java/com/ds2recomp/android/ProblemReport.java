package com.ds2recomp.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.MediaStore;
import android.widget.Toast;
import java.io.File;
import java.io.IOException;
import java.io.OutputStream;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/**
 * CTRL -> Troubleshooting -> "Report a problem on GitHub" (the owner, 2026-11-25): saves the log zip in Download and
 * opens the public repository's issue form (.github/ISSUE_TEMPLATE/report.yml) with the version, phone, GPU, driver
 * and settings filled in through the form fields' ids. GitHub has no way for an app to attach a file to an issue
 * (neither the new-issue URL nor the REST API), so the player attaches the zip on the GitHub page.
 */
final class ProblemReport {
    static final String REPOSITORY = "https://github.com/Lazaer-YZY/ds2-sotfs-recomp";
    static final String TEMPLATE = "report.yml";
    /** The settings field's share of the URL: GitHub answers 414 for very long URLs. */
    private static final int SETTINGS_MAX_CHARS = 1500;

    private ProblemReport() {}

    /** The confirmation dialog; with Debug logging off it offers to turn it on first. */
    static void show(RuntimeActivity activity) {
        SharedPreferences render = RuntimeActivity.renderPrefs(activity);
        boolean debugLog = render.getBoolean("debug_log", RuntimeActivity.DEFAULT_DEBUG_LOG);
        String message = activity.getString(R.string.report_message);
        if (!debugLog) message += "\n\n" + activity.getString(R.string.report_debug_off);
        AlertDialog.Builder builder = new AlertDialog.Builder(activity)
            .setTitle(R.string.report_title)
            .setMessage(message)
            .setPositiveButton(R.string.report_continue, (dialog, which) -> saveAndOpen(activity))
            .setNegativeButton(R.string.touch_cancel, null);
        if (!debugLog) {
            builder.setNeutralButton(R.string.report_debug_on, (dialog, which) -> {
                render.edit().putBoolean("debug_log", true).apply();
                Toast.makeText(activity, R.string.report_debug_enabled, Toast.LENGTH_LONG).show();
            });
        }
        builder.show();
    }

    /**
     * Android 10+: the zip goes straight to Download through MediaStore (no permission needed), then the issue form
     * opens. Older Android: the system file picker first (RuntimeActivity's log export), then the form.
     */
    static void saveAndOpen(RuntimeActivity activity) {
        if (Build.VERSION.SDK_INT < 29) {
            activity.startLogExport(true);
            return;
        }
        List<File> files = PlayerLogs.exportable(PlayerLogs.directory(activity));
        String device = activity.logDeviceText();
        String requested = PlayerLogs.exportName(new java.util.Date());
        new Thread(() -> {
            ContentResolver resolver = activity.getContentResolver();
            Uri uri = null;
            String saved;
            boolean complete;
            try {
                ContentValues values = new ContentValues();
                values.put(MediaStore.MediaColumns.DISPLAY_NAME, requested);
                values.put(MediaStore.MediaColumns.MIME_TYPE, "application/zip");
                values.put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS);
                values.put(MediaStore.MediaColumns.IS_PENDING, 1);
                uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
                if (uri == null) throw new IOException("Download could not be opened");
                try (OutputStream output = resolver.openOutputStream(uri, "w")) {
                    if (output == null) throw new IOException("the file could not be opened");
                    complete = PlayerLogs.exportZip(files, device, output, PlayerLogs.GITHUB_ATTACHMENT_BUDGET);
                }
                values.clear();
                values.put(MediaStore.MediaColumns.IS_PENDING, 0);
                resolver.update(uri, values, null, null);
                saved = displayName(resolver, uri, requested);
            } catch (IOException | RuntimeException e) {
                if (uri != null) {
                    try {
                        resolver.delete(uri, null, null);
                    } catch (RuntimeException ignored) {
                        // A pending entry is cleaned up by the system.
                    }
                }
                String failed = activity.getString(R.string.logs_save_failed, String.valueOf(e.getMessage()));
                activity.runOnUiThread(() -> Toast.makeText(activity, failed, Toast.LENGTH_LONG).show());
                return;
            }
            final String name = saved;
            final String shown = activity.getString(R.string.report_saved, name)
                + (complete ? "" : " " + activity.getString(R.string.report_capture_left_out));
            activity.runOnUiThread(() -> {
                Toast.makeText(activity, shown, Toast.LENGTH_LONG).show();
                openIssue(activity, name);
            });
        }, "DS2 problem report").start();
    }

    /** The name MediaStore gave the file ("DS2-logs-... (1).zip" when the name was taken). */
    private static String displayName(ContentResolver resolver, Uri uri, String fallback) {
        try (Cursor row = resolver.query(uri, new String[] {MediaStore.MediaColumns.DISPLAY_NAME}, null, null, null)) {
            if (row != null && row.moveToFirst() && row.getString(0) != null) return row.getString(0);
        } catch (RuntimeException ignored) {
            // The name is only for the messages.
        }
        return fallback;
    }

    /** Opens the issue form in the browser, its fields filled in; zipName goes into the logs field. */
    static void openIssue(Activity activity, String zipName) {
        Uri uri = issueUri(activity, zipName);
        android.util.Log.i("ds2recomp", "problem report: opening the issue form (" + uri.toString().length()
            + " characters): " + uri);
        try {
            activity.startActivity(new Intent(Intent.ACTION_VIEW, uri).addCategory(Intent.CATEGORY_BROWSABLE));
        } catch (android.content.ActivityNotFoundException e) {
            Toast.makeText(activity, activity.getString(R.string.report_no_browser, REPOSITORY + "/issues", zipName),
                Toast.LENGTH_LONG).show();
        }
    }

    static Uri issueUri(Activity activity, String zipName) {
        String version;
        try {
            version = activity.getPackageManager().getPackageInfo(activity.getPackageName(), 0).versionName;
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            version = "?";
        }
        if (!"com.ds2recomp.android".equals(activity.getPackageName())) {
            version += " (" + activity.getPackageName() + ")";
        }
        DeviceProfile.Profile profile = DeviceProfile.current();
        String gpuModel = GpuDriverStore.gpuModel();
        GpuDriverStore.Installed driver = GpuDriverStore.selectedDriver(activity);
        // "AYN Thor" rather than "AYN AYN Thor"; "samsung SM-S931B" stays as it is.
        String model = Build.MODEL.toLowerCase(java.util.Locale.ROOT)
            .startsWith(Build.MANUFACTURER.toLowerCase(java.util.Locale.ROOT)) ? Build.MODEL
            : Build.MANUFACTURER + " " + Build.MODEL;
        String device = model + ", " + DeviceProfile.socModel() + ", Android "
            + Build.VERSION.RELEASE;
        String gpu = (gpuModel != null ? gpuModel : profile.gpu) + ", "
            + (driver != null ? driver.metadata.name : GpuDriverStore.SYSTEM_DRIVER_NAME);
        StringBuilder settings = new StringBuilder("GPU profile: ").append(profile);
        for (Map.Entry<String, ?> entry : new TreeMap<>(RuntimeActivity.renderPrefs(activity).getAll()).entrySet()) {
            if (DeviceProfile.KEY_PROFILE.equals(entry.getKey())) continue;
            String line = "\n" + entry.getKey() + "=" + entry.getValue();
            if (settings.length() + line.length() > SETTINGS_MAX_CHARS) {
                settings.append("\n...");
                break;
            }
            settings.append(line);
        }
        return Uri.parse(REPOSITORY + "/issues/new").buildUpon()
            .appendQueryParameter("template", TEMPLATE)
            .appendQueryParameter("version", version)
            .appendQueryParameter("device", device)
            .appendQueryParameter("gpu", gpu)
            .appendQueryParameter("settings", settings.toString())
            .appendQueryParameter("logs", activity.getString(R.string.report_attach, zipName) + "\n\n")
            .build();
    }
}

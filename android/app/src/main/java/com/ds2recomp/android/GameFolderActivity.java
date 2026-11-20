// Adapted from Lost Odyssey Recomp (GPL-3.0).
package com.ds2recomp.android;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

/**
 * Game folder page: where the game data is read from. The app folder in
 * {@code Android/data} needs no permission but phone file managers cannot
 * write into it, so a folder anywhere on the internal storage or an SD card
 * can be picked instead. The native runtime opens files by path, so a picked
 * folder needs the "All files access" permission (storage permission before
 * Android 11).
 *
 * It opens instead of the game when no {@code default.xex} is found in the
 * game folder, and from the CTRL dialog while playing; changing the folder
 * then restarts. "Import disc 1/2" reads the player's disc images on the
 * device ({@link IsoImport}): disc 1 is extracted into the game folder, disc 2's
 * DLC package (Compatibility Pack 4, which the game needs) is copied into the
 * app's dlc/ folder. The launcher opens this page until both are there.
 */
public final class GameFolderActivity extends Activity {
    static final String EXTRA_FROM_GAME = "from_game";
    private static final int REQUEST_PICK_FOLDER = 1;
    private static final int REQUEST_STORAGE_PERMISSION = 2;
    private static final int REQUEST_PICK_ISO = 3;

    private boolean fromGame;
    private File originalCustom;
    /** What to continue with once the storage permission is granted. */
    private enum Pending { NONE, PICK }
    private Pending afterPermission = Pending.NONE;
    private LinearLayout page;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable tick = this::updateImport;
    private ProgressBar importBar;
    private TextView importBytes;
    private TextView importFile;
    private Button importCancel;
    private AlertDialog readingDialog;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        PlayerLogs.installCrashHandler(this);
        super.onCreate(savedInstanceState);
        fromGame = getIntent().getBooleanExtra(EXTRA_FROM_GAME, false);
        originalCustom = GameStorage.customRoot(this);
        page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setBackgroundColor(Color.rgb(18, 24, 30));
        int padding = dp(20);
        page.setPadding(padding, padding, padding, padding);
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.rgb(18, 24, 30));
        scroll.setFillViewport(true);
        scroll.addView(page, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT));
        // Android 15 draws apps edge to edge: keep the page clear of the status bar.
        scroll.setOnApplyWindowInsetsListener((view, insets) -> {
            view.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(),
                insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom());
            return insets;
        });
        setContentView(scroll);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Returning from the system permission page.
        if (afterPermission != Pending.NONE && canReadSharedStorage()) {
            Pending pending = afterPermission;
            afterPermission = Pending.NONE;
            if (pending == Pending.PICK) pickFolder();
        }
        render();
    }

    @Override
    protected void onPause() {
        super.onPause();
        handler.removeCallbacks(tick);
    }

    @Override
    protected void onDestroy() {
        if (readingDialog != null) readingDialog.dismiss();
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        IsoImport job = IsoImport.current();
        if (job != null && !job.finished()) {
            Toast.makeText(this, "Cancel the import first", Toast.LENGTH_SHORT).show();
            return;
        }
        super.onBackPressed();
    }

    private int dp(float value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private TextView text(String value, float sizeSp, boolean bold) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(sizeSp);
        view.setTextColor(Color.WHITE);
        if (bold) view.setTypeface(Typeface.DEFAULT_BOLD);
        view.setPadding(0, dp(4), 0, dp(4));
        return view;
    }

    private TextView dim(String value) {
        TextView view = text(value, 13f, false);
        view.setTextColor(Color.argb(255, 190, 200, 210));
        return view;
    }

    private Button button(String label, View.OnClickListener listener) {
        Button button = new Button(this);
        button.setText(label);
        button.setAllCaps(false);
        button.setOnClickListener(listener);
        return button;
    }

    private void render() {
        page.removeAllViews();
        page.addView(text("Game folder", 24f, true));
        if (renderImport()) return;

        File gameDir = GameStorage.gameDir(this);
        boolean found = GameStorage.hasDefaultXex(gameDir);
        TextView status = text(found ? "Disc 1 (game) found: " + gameDir
            : "Disc 1 (game) missing. Import disc 1 below, or put the extracted game (Default.xex and the "
                + "disc files directly inside the folder) in one of the places below.", 15f, true);
        status.setTextColor(found ? Color.rgb(140, 220, 140) : Color.rgb(255, 196, 80));
        page.addView(status);
        boolean dlc = GameStorage.hasDlc(this);
        File dlcDir = GameStorage.dlcDir(this);
        File dlcPackage = GameStorage.dlcPackage(this);
        TextView dlcStatus = text(dlc ? "Disc 2 (DLC) found: " + (dlcPackage != null ? dlcPackage : "installed")
            : "Disc 2 (DLC) missing. The game does not start without disc 2's DLC package (Compatibility "
                + "Pack 4). Import disc 2 below, or copy the one file in disc 2's "
                + "content/0000000000000000/FFED2000/FFFFFFFF/ into " + dlcDir + ".", 15f, true);
        dlcStatus.setTextColor(dlc ? Color.rgb(140, 220, 140) : Color.rgb(255, 196, 80));
        page.addView(dlcStatus);

        if (!fromGame) {
            page.addView(text("Import from the discs", 18f, true));
            page.addView(dim("Pick your Xbox 360 disc images (.iso), one at a time. Disc 1: the app extracts the "
                + "game (about 5.5 GB) into the custom folder if one is set, else into the app folder. Disc 2: it "
                + "copies the DLC package (1.2 GB) into " + dlcDir + "; the game's first start installs it (another "
                + "1.2 GB). The ISOs can be deleted afterwards."));
            LinearLayout imports = new LinearLayout(this);
            imports.setOrientation(LinearLayout.HORIZONTAL);
            imports.setPadding(0, dp(8), 0, dp(8));
            // Not while playing: the running game has the current game files open.
            imports.addView(button("Import disc 1…", v -> pickIso()));
            imports.addView(button("Import disc 2…", v -> pickIso()));
            page.addView(imports);
        }

        page.addView(text("App folder", 18f, true));
        StringBuilder folders = new StringBuilder();
        for (File volume : GameStorage.volumes(this))
            folders.append(new File(volume, "game").getAbsolutePath()).append('\n');
        page.addView(dim(folders.toString().trim()));
        page.addView(dim("No permission needed. Copy the game here from a PC over USB; "
            + "file managers on the device usually cannot write into Android/data. "
            + "Uninstalling the app deletes this folder."));

        page.addView(text("Custom folder", 18f, true));
        File custom = GameStorage.customRoot(this);
        if (custom == null) {
            page.addView(dim("Not set."));
        } else {
            page.addView(dim(custom.getAbsolutePath()));
            if (!canReadSharedStorage()) {
                TextView warning = dim("All files access is off, so the game cannot read this folder.");
                warning.setTextColor(Color.rgb(255, 196, 80));
                page.addView(warning);
            } else if (!GameStorage.hasDefaultXex(custom)) {
                TextView warning = dim("default.xex is missing in this folder.");
                warning.setTextColor(Color.rgb(255, 196, 80));
                page.addView(warning);
            }
        }
        page.addView(dim("Any folder on the internal storage or an SD card, for example one "
            + "filled with a file manager. Pick the folder that holds default.xex. "
            + "Needs the \"All files access\" permission."));

        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        actions.setPadding(0, dp(8), 0, dp(8));
        actions.addView(button("Choose folder…", v -> chooseFolder()));
        if (custom != null) {
            actions.addView(button("Use app folder", v -> {
                GameStorage.setCustomRoot(this, null);
                render();
            }));
        }
        page.addView(actions);

        page.addView(primaryButton(found));
    }

    private Button primaryButton(boolean found) {
        boolean changed = !Objects.equals(originalCustom, GameStorage.customRoot(this));
        if (!fromGame) {
            Button start = button("Start game", v -> {
                if (!GameStorage.hasDlc(this)) {
                    new AlertDialog.Builder(this).setTitle("Disc 2 is missing")
                        .setMessage("The game does not start without disc 2's DLC package (Compatibility Pack 4). "
                            + "Import disc 2 first.")
                        .setPositiveButton("Import disc 2…", (dialog, which) -> pickIso())
                        .setNeutralButton("Start anyway", (dialog, which) -> startGame())
                        .setNegativeButton("Cancel", null)
                        .show();
                    return;
                }
                startGame();
            });
            start.setEnabled(found);
            return start;
        }
        if (!changed) return button("Back to game", v -> finish());
        Button apply = button("Apply and restart", v -> new AlertDialog.Builder(this)
            .setTitle("Restart the game?")
            .setMessage("The game folder is chosen when the game starts, so the game restarts now. "
                + "Progress since the last save is lost.")
            .setPositiveButton("Restart", (dialog, which) -> {
                Intent intent = new Intent(this, RuntimeActivity.class);
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
                startActivity(intent);
                // The pending activity outlives the process; Android starts a new one for it.
                Runtime.getRuntime().exit(0);
            })
            .setNegativeButton("Cancel", null)
            .show());
        // Restarting without game data would only reach the missing-game error.
        apply.setEnabled(found);
        return apply;
    }

    private void startGame() {
        startActivity(new Intent(this, RuntimeActivity.class));
        finish();
    }

    // Permission and picker ---------------------------------------------------

    private boolean canReadSharedStorage() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) return Environment.isExternalStorageManager();
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    private void chooseFolder() {
        if (canReadSharedStorage()) { pickFolder(); return; }
        askForStorage(Pending.PICK);
    }

    private void askForStorage(Pending next) {
        new AlertDialog.Builder(this)
            .setTitle("Allow file access")
            .setMessage("The game reads its files directly, so folders outside "
                + "the app folder need the \"All files access\" permission. Android opens the "
                + "setting now; turn it on for Dark Souls II and come back.")
            .setPositiveButton("Open setting", (dialog, which) -> requestStorage(next))
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void requestStorage(Pending next) {
        afterPermission = next;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
            } catch (RuntimeException e) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        } else {
            requestPermissions(new String[] { Manifest.permission.READ_EXTERNAL_STORAGE,
                Manifest.permission.WRITE_EXTERNAL_STORAGE }, REQUEST_STORAGE_PERMISSION);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        // onResume follows and continues to the picker when the permission was granted.
        if (requestCode == REQUEST_STORAGE_PERMISSION && !canReadSharedStorage()) afterPermission = Pending.NONE;
    }

    private void pickFolder() {
        try {
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQUEST_PICK_FOLDER);
        } catch (RuntimeException e) {
            Toast.makeText(this, "No folder picker available", Toast.LENGTH_SHORT).show();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_PICK_ISO) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) readIso(data.getData());
            return;
        }
        if (requestCode != REQUEST_PICK_FOLDER || resultCode != RESULT_OK || data == null
                || data.getData() == null) return;
        String documentId;
        try {
            documentId = DocumentsContract.getTreeDocumentId(data.getData());
        } catch (RuntimeException e) {
            documentId = null;
        }
        File picked = GameStorage.pathForTreeDocumentId(documentId, Environment.getExternalStorageDirectory());
        File root = GameStorage.normalizeGameRoot(picked);
        if (root == null) {
            new AlertDialog.Builder(this).setTitle("No game in this folder")
                .setMessage(picked == null
                    ? "Pick a folder on the internal storage or an SD card."
                    : "Expected default.xex in " + picked.getAbsolutePath()
                        + ". Pick the folder that holds default.xex and the disc files.")
                .setPositiveButton("OK", null).show();
            return;
        }
        GameStorage.setCustomRoot(this, root);
        render();
    }

    // ISO import --------------------------------------------------------------

    private void pickIso() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        // .iso has no reliable mime type.
        intent.setType("*/*");
        try {
            startActivityForResult(intent, REQUEST_PICK_ISO);
        } catch (RuntimeException e) {
            Toast.makeText(this, "No file picker available", Toast.LENGTH_SHORT).show();
        }
    }

    private void message(String title, String body) {
        new AlertDialog.Builder(this).setTitle(title).setMessage(body).setPositiveButton("OK", null).show();
    }

    /** Opens and lists the picked disc image off the UI thread, then asks to import. */
    private void readIso(Uri uri) {
        readingDialog = new AlertDialog.Builder(this).setTitle("Reading disc image…")
            .setMessage("Checking the files on the disc.").setCancelable(false).show();
        new Thread(() -> {
            IsoImport.Disc disc = null;
            String error = null;
            try {
                disc = IsoImport.open(getApplicationContext(), uri);
            } catch (Exception e) {
                error = e.getMessage() != null ? e.getMessage() : e.toString();
            }
            IsoImport.Disc opened = disc;
            String failure = error;
            runOnUiThread(() -> {
                if (readingDialog != null) readingDialog.dismiss();
                readingDialog = null;
                if (isDestroyed()) {
                    if (opened != null) opened.close();
                    return;
                }
                if (opened == null) message("Cannot import this file", failure);
                else checkDisc(opened);
            });
        }, "ISO check").start();
    }

    /**
     * Only the disc release this app's game code was compiled from can run (its default.xex,
     * byte for byte); any other disc is refused before anything is copied.
     */
    private void checkDisc(IsoImport.Disc disc) {
        if (disc.kind == IsoImport.Kind.DLC) {
            checkDlcTarget(disc);
            return;
        }
        if (disc.titleId == IsoImport.TITLE_ID
                && (IsoImport.TESTED_XEX_SHA256.isEmpty() || IsoImport.TESTED_XEX_SHA256.equals(disc.xexSha256))) {
            checkTarget(disc);
            return;
        }
        disc.close();
        if (disc.titleId != IsoImport.TITLE_ID) {
            String id = disc.titleId < 0 ? "unknown" : String.format("%08X", disc.titleId);
            message("Not Dark Souls II", "This disc's title ID is " + id
                + " and it carries no Dark Souls II DLC package; Dark Souls II: Scholar of the First Sin is "
                + "465307E4 (disc 1) and its disc 2 carries Compatibility Pack 4. Nothing was imported.");
        } else {
            String sha = disc.xexSha256.isEmpty() ? "unknown" : disc.xexSha256.substring(0, 16) + "…";
            message("Different release of the game", "This app's game code was compiled from the USA "
                + "disc's default.xex, and this disc's default.xex is a different version (sha256 " + sha
                + "), so the game would crash. Nothing was imported.\n\nOther releases need their own "
                + "build from the source code.");
        }
    }

    /**
     * The custom folder when one is set and readable; otherwise the first app
     * folder (internal first) with room for the game.
     */
    private void checkTarget(IsoImport.Disc disc) {
        long need = disc.totalBytes + IsoImport.SPACE_MARGIN;
        File custom = GameStorage.customRoot(this);
        List<File> candidates = new ArrayList<>();
        if (custom != null && canReadSharedStorage()) candidates.add(custom);
        else for (File volume : GameStorage.volumes(this)) candidates.add(new File(volume, "game"));
        File target = null;
        StringBuilder space = new StringBuilder();
        for (File candidate : candidates) {
            File existing = candidate;
            while (existing != null && !existing.exists()) existing = existing.getParentFile();
            long free = existing != null ? existing.getUsableSpace() : 0;
            space.append(candidate.getAbsolutePath()).append(": ")
                .append(IsoImport.gigabytes(free)).append(" free\n");
            if (target == null && free >= need) target = candidate;
        }
        if (target == null) {
            disc.close();
            message("Not enough space", "The game needs " + IsoImport.gigabytes(need) + ".\n\n"
                + space.toString().trim() + "\n\nFree some space or pick a custom folder on a larger storage.");
            return;
        }
        String[] existing = target.list();
        boolean hasFiles = false;
        if (existing != null) for (String name : existing) if (!name.equals(GameStorage.README)) hasFiles = true;
        File chosen = target;
        new AlertDialog.Builder(this).setTitle("Import disc 1 (the game)?")
            .setMessage(disc.fileCount + " files, " + IsoImport.gigabytes(disc.totalBytes) + ", into\n"
                + target.getAbsolutePath()
                + (hasFiles ? "\n\nFiles with the same names already in this folder are replaced." : "")
                + "\n\nThis takes a few minutes; keep the device charged. You can leave the app: a notification shows the progress.")
            .setPositiveButton("Import", (dialog, which) -> {
                IsoImport.start(disc, chosen);
                IsoImportService.start(this);  // keeps the copy alive if the user leaves the app
                render();
            })
            .setNegativeButton("Cancel", (dialog, which) -> disc.close())
            .setOnCancelListener(dialog -> disc.close())
            .show();
    }

    /**
     * Disc 2: its DLC package goes to the app's dlc/ folder, where the game installs it at the next
     * start (into user/, as much again), so both copies must fit.
     */
    private void checkDlcTarget(IsoImport.Disc disc) {
        File target = GameStorage.dlcDir(this);
        long need = 2 * disc.totalBytes + IsoImport.SPACE_MARGIN;
        File existing = target;
        while (existing != null && !existing.exists()) existing = existing.getParentFile();
        long free = existing != null ? existing.getUsableSpace() : 0;
        if (free < need) {
            disc.close();
            message("Not enough space", "Disc 2's DLC package needs " + IsoImport.gigabytes(need)
                + " (the package, and the game installs it once more at its first start).\n\n"
                + target.getAbsolutePath() + ": " + IsoImport.gigabytes(free) + " free.\n\nFree some space first.");
            return;
        }
        new AlertDialog.Builder(this).setTitle("Import disc 2 (the DLC)?")
            .setMessage(disc.packageName + ", " + IsoImport.gigabytes(disc.totalBytes) + ", into\n"
                + target.getAbsolutePath()
                + "\n\nThe game installs it at its next start. This takes a minute or two; you can leave the app: "
                + "a notification shows the progress.")
            .setPositiveButton("Import", (dialog, which) -> {
                IsoImport.start(disc, target);
                IsoImportService.start(this);  // keeps the copy alive if the user leaves the app
                render();
            })
            .setNegativeButton("Cancel", (dialog, which) -> disc.close())
            .setOnCancelListener(dialog -> disc.close())
            .show();
    }

    /** After one disc: ask for the other one, or offer to start once both are there. */
    private void importedNext(IsoImport job) {
        boolean game = GameStorage.hasGame(this);
        boolean dlc = GameStorage.hasDlc(this);
        String done = job.kind == IsoImport.Kind.DLC ? "Disc 2 imported" : "Disc 1 imported";
        String warning = job.message == null ? "" : "\n\nNote: " + job.message;
        if (game && dlc) {
            // The next step is starting the game (the page's Start button is below the fold).
            new AlertDialog.Builder(this).setTitle(done)
                .setMessage("The game is ready. Its first start installs the DLC and prepares the shaders for this "
                    + "device, which takes a few minutes; later starts are quick." + warning)
                .setPositiveButton("Start game", (dialog, which) -> startGame())
                .setNegativeButton("Not now", null)
                .show();
        } else {
            String next = game ? "Now import disc 2: the game needs its DLC package (Compatibility Pack 4)."
                : "Now import disc 1 (the game).";
            new AlertDialog.Builder(this).setTitle(done)
                .setMessage(next + warning)
                .setPositiveButton(game ? "Import disc 2…" : "Import disc 1…", (dialog, which) -> pickIso())
                .setNegativeButton("Later", null)
                .show();
        }
    }

    /** The import's progress page while one runs; returns false when there is none to show. */
    private boolean renderImport() {
        IsoImport job = IsoImport.current();
        if (job == null) {
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            return false;
        }
        if (job.finished()) {
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            IsoImport.clearFinished();
            if (job.state == IsoImport.State.DONE && !fromGame) {
                importedNext(job);
            } else if (job.state == IsoImport.State.DONE) {
                message("Game imported", job.message == null ? "The game is ready."
                    : "The game is in place, but: " + job.message);
            } else if (job.state == IsoImport.State.FAILED) {
                message("Import failed", job.message + "\n\nNothing was left behind.");
            }
            return false;
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        page.addView(text(job.kind == IsoImport.Kind.DLC ? "Importing disc 2 (the DLC package)"
            : "Importing disc 1 (the game)", 18f, true));
        page.addView(dim("Into " + job.target.getAbsolutePath()
            + ". Keep the app open; the screen stays on until it is done."));
        importBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        importBar.setMax(1000);
        page.addView(importBar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT));
        importBytes = text("", 15f, false);
        page.addView(importBytes);
        importFile = dim("");
        page.addView(importFile);
        importCancel = button("Cancel import", v -> new AlertDialog.Builder(this)
            .setTitle("Cancel the import?")
            .setMessage("The files extracted so far are deleted.")
            .setPositiveButton("Cancel import", (dialog, which) -> { job.cancel(); updateImport(); })
            .setNegativeButton("Continue", null)
            .show());
        page.addView(importCancel);
        updateImport();
        return true;
    }

    private void updateImport() {
        handler.removeCallbacks(tick);
        IsoImport job = IsoImport.current();
        if (job == null || job.finished() || importBar == null || importBar.getParent() == null) {
            render();
            return;
        }
        long total = Math.max(1, job.totalBytes);
        importBar.setProgress((int) (job.doneBytes * 1000 / total));
        if (job.state == IsoImport.State.CLEANING) {
            importBytes.setText("Deleting the extracted files…");
            importFile.setText("");
            importCancel.setEnabled(false);
        } else {
            importBytes.setText(IsoImport.gigabytes(job.doneBytes) + " of " + IsoImport.gigabytes(job.totalBytes)
                + String.format(" (%d%%)", job.doneBytes * 100 / total));
            importFile.setText(job.currentFile);
        }
        handler.postDelayed(tick, 250);
    }
}

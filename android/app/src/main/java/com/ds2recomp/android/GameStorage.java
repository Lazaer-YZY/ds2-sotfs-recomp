// Adapted from Lost Odyssey Recomp (GPL-3.0).
package com.ds2recomp.android;

import android.content.Context;
import android.media.MediaScannerConnection;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Where the user's game data lives: {@code Android/data/<package>/files/game/}
 * on the internal shared storage or on an SD card.
 *
 * Android creates that folder only when the app first asks for it, and a PC
 * sees new folders over USB (MTP) only after the media scanner has indexed a
 * file inside them. The launcher therefore creates the game folder on every
 * volume and scans a README in each, so it is there to copy into. The game
 * folder holds {@code Default.xex} and the other disc files directly (no disc
 * subfolders).
 */
final class GameStorage {
    static final String README = "README.txt";
    private static final String README_TEXT =
        "Copy the game files here so that game/Default.xex (and the rest of the disc) exist.\n"
        + "The game uses the first storage (internal or SD card) whose game folder has default.xex,\n"
        + "otherwise this folder on the internal storage.\n";

    private GameStorage() {}

    /** The {@code files} folder of every mounted shared-storage volume, internal first. */
    static List<File> volumes(Context context) {
        List<File> result = new ArrayList<>();
        File[] dirs = context.getExternalFilesDirs(null);
        if (dirs != null) for (File dir : Arrays.asList(dirs)) if (dir != null) result.add(dir);
        return result;
    }

    /** {@code /storage/1234-ABCD} for the app folder {@code /storage/1234-ABCD/Android/data/<package>/files}. */
    static File volumeRoot(File appFiles) {
        File root = appFiles;
        for (int level = 0; level < 4 && root != null; ++level) root = root.getParentFile();
        return root;
    }

    /** Creates {@code game/} with a README on each volume; returns the README paths written. */
    static List<String> createFolders(List<File> volumes) {
        List<String> written = new ArrayList<>();
        for (File volume : volumes) {
            File game = new File(volume, "game");
            //noinspection ResultOfMethodCallIgnored
            game.mkdirs();
            File readme = new File(game, README);
            if (readme.exists() || !game.isDirectory()) continue;
            try (OutputStream output = new FileOutputStream(readme)) {
                output.write(README_TEXT.getBytes(StandardCharsets.UTF_8));
                written.add(readme.getAbsolutePath());
            } catch (IOException ignored) {
                // A read-only or removed card keeps its folders without the README.
            }
        }
        return written;
    }

    /** Makes the game folders and shows them to a PC connected over USB. */
    static void prepare(Context context) {
        List<String> written = createFolders(volumes(context));
        if (!written.isEmpty())
            MediaScannerConnection.scanFile(context.getApplicationContext(),
                written.toArray(new String[0]), null, null);
    }

    /**
     * The custom game folder when it holds {@code default.xex}, else the first
     * volume whose {@code game/} does, else the first volume's {@code game/}.
     */
    static File selectGameDir(File custom, List<File> volumes, File fallback) {
        if (custom != null && hasDefaultXex(custom)) return custom;
        for (File volume : volumes) {
            File game = new File(volume, "game");
            if (hasDefaultXex(game)) return game;
        }
        return new File(volumes.isEmpty() ? fallback : volumes.get(0), "game");
    }

    /** The folder passed to the runtime as {@code --game_data_root}. */
    static File gameDir(Context context) {
        return selectGameDir(customRoot(context), volumes(context), context.getFilesDir());
    }

    // The game folder is filled by copying an extracted game (Default.xex +
    // data files) or by IsoImport from the player's disc 1 image; disc 2's DLC
    // package goes to dlc/ (copied, or IsoImport from the disc 2 image).

    /** The app files folder the runtime gets as REX_ANDROID_APP_DIR (RuntimeActivity: the logs' parent). */
    private static File appRoot(Context context) {
        File external = context.getExternalFilesDir(null);
        return external != null ? external : context.getFilesDir();
    }

    /** Where the game looks for DLC packages to install at startup (app/src/ds2_dlc.cpp). */
    static File dlcDir(Context context) {
        return new File(appRoot(context), "dlc");
    }

    /** The first package waiting in dlc/ (regular, not hidden files, as the game takes them), or null. */
    static File dlcPackage(Context context) {
        File[] files = dlcDir(context).listFiles();
        if (files == null) return null;
        for (File file : files) if (file.isFile() && file.length() > 0 && !file.getName().startsWith(".")) return file;
        return null;
    }

    /**
     * Disc 2's DLC (Compatibility Pack 4) is there: a package in dlc/, or already installed by the
     * game (user/0000000000000000/465307E4/00000002/), so a player who deleted the package after the
     * first start is not sent back to this page.
     */
    static boolean hasDlc(Context context) {
        if (dlcPackage(context) != null) return true;
        String[] installed = new File(appRoot(context), "user/0000000000000000/465307E4/00000002").list();
        return installed != null && installed.length > 0;
    }

    static boolean hasDefaultXex(File gameDir) {
        File[] files = gameDir.listFiles();
        if (files == null) return false;
        // The DS2 disc spells it "Default.xex".
        for (File file : files) if (file.isFile() && file.getName().equalsIgnoreCase("default.xex")) return true;
        return false;
    }

    static boolean hasGame(Context context) {
        return hasDefaultXex(gameDir(context));
    }

    // Custom folder -----------------------------------------------------------

    private static final String PREFERENCES = "game_storage";
    private static final String CUSTOM_ROOT = "custom_root";

    /** The chosen game folder (the one holding default.xex), or null for the app folder. */
    static File customRoot(Context context) {
        String path = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
            .getString(CUSTOM_ROOT, null);
        return path == null || path.isEmpty() ? null : new File(path);
    }

    static void setCustomRoot(Context context, File root) {
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).edit()
            .putString(CUSTOM_ROOT, root != null ? root.getAbsolutePath() : null).apply();
    }

    /** The folder the user picked when it holds {@code default.xex}, else null. */
    static File normalizeGameRoot(File chosen) {
        if (chosen == null) return null;
        return hasDefaultXex(chosen) ? chosen : null;
    }

    /**
     * Maps a document-tree id from the system folder picker to a file path:
     * {@code primary:Games/DS2} lies under the internal shared storage and
     * {@code 1234-ABCD:Games/DS2} under {@code /storage/1234-ABCD}. Other
     * providers (downloads, cloud) have no path and return null.
     */
    static File pathForTreeDocumentId(String documentId, File primaryRoot) {
        if (documentId == null) return null;
        int colon = documentId.indexOf(':');
        if (colon <= 0) return null;
        String volume = documentId.substring(0, colon);
        String relative = documentId.substring(colon + 1);
        // The downloads provider names folders by absolute path ("raw:/storage/…").
        if (relative.startsWith("/")) return new File(relative);
        File base = volume.equalsIgnoreCase("primary") ? primaryRoot : new File("/storage", volume);
        return relative.isEmpty() ? base : new File(base, relative);
    }
}

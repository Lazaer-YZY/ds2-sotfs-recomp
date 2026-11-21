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

    // DLC next to the game (issue #6: with the game on an SD card or in a custom folder, the 1.2 GB package and the
    // 1.1 GB the game unpacks from it stayed on the internal storage). Both go to the app folder on the storage that
    // holds the game, which the app can always write; a DLC already installed on the internal storage stays there.

    /**
     * The app folder ({@code Android/data/<package>/files}) on the storage volume that holds the game: the volume's
     * own for {@code <volume>/game}, the one on the same card for a custom folder there, else the internal one.
     */
    static File gameVolumeAppDir(Context context) {
        return volumeAppDirFor(gameDir(context), volumes(context), appRoot(context));
    }

    /** The entry of {@code volumes} on the same storage as {@code game} (by volume root), else {@code fallback}. */
    static File volumeAppDirFor(File game, List<File> volumes, File fallback) {
        String path = game.getAbsolutePath();
        for (File volume : volumes) {
            File root = volumeRoot(volume);
            if (root == null) continue;
            String prefix = root.getAbsolutePath();
            if (path.equals(prefix) || path.startsWith(prefix + File.separator)) return volume;
        }
        return fallback;
    }

    /**
     * Where DLC packages are copied or imported to: dlc/ inside the custom game folder when the game is there and the
     * app can write it (it survives a reinstall, like the game), else dlc/ in the app folder on the game's storage.
     */
    static File dlcDir(Context context) {
        File custom = customRoot(context), game = gameDir(context);
        if (custom != null && game.equals(custom) && game.canWrite()) return new File(game, "dlc");
        return new File(gameVolumeAppDir(context), "dlc");
    }

    /**
     * The folders the game installs DLC packages from (DS2_DLC_DIRS, app/src/ds2_dlc.cpp), in order: dlc/ inside
     * the game folder, {@link #dlcDir}, and the internal app folder's dlc/ (where packages went before).
     */
    static List<File> dlcPackageDirs(Context context) {
        List<File> dirs = new ArrayList<>();
        for (File dir : new File[] { new File(gameDir(context), "dlc"), dlcDir(context), new File(appRoot(context), "dlc") })
            if (!dirs.contains(dir)) dirs.add(dir);
        return dirs;
    }

    /** The first package waiting in a DLC folder (regular, not hidden files, as the game takes them), or null. */
    static File dlcPackage(Context context) {
        for (File dir : dlcPackageDirs(context)) {
            File[] files = dir.listFiles();
            if (files == null) continue;
            for (File file : files) if (file.isFile() && file.length() > 0 && !file.getName().startsWith(".")) return file;
        }
        return null;
    }

    /** The installed marketplace content of this title under a content root. */
    private static File installedDlcDir(File contentRoot) {
        return new File(contentRoot, "0000000000000000/465307E4/00000002");
    }

    private static boolean hasEntries(File dir) {
        String[] names = dir.list();
        return names != null && names.length > 0;
    }

    /**
     * Where the game installs DLC (DS2_DLC_ROOT), or null for its default, the app's user/ folder: {@code
     * dlc_content/} in the app folder on the game's storage when that is not the internal one, unless a DLC is
     * already installed internally (it keeps working where it is).
     */
    static File dlcContentRoot(Context context) {
        return dlcContentRootFor(gameVolumeAppDir(context), appRoot(context));
    }

    static File dlcContentRootFor(File gameVolumeAppDir, File appRoot) {
        if (gameVolumeAppDir.equals(appRoot) || hasEntries(installedDlcDir(new File(appRoot, "user")))) return null;
        return new File(gameVolumeAppDir, "dlc_content");
    }

    /**
     * Disc 2's DLC (Compatibility Pack 4) is there: a package in a DLC folder, or already installed by the game, so
     * a player who deleted the package after the first start is not sent back to this page.
     */
    static boolean hasDlc(Context context) {
        if (dlcPackage(context) != null) return true;
        File root = dlcContentRoot(context);
        return hasEntries(installedDlcDir(root != null ? root : new File(appRoot(context), "user")));
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

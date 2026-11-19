package com.ds2recomp.android;

import android.content.Context;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.List;
import java.util.Locale;

/**
 * Extracts the game from a user's Xbox 360 disc image (picked through the
 * system file picker) into a game folder, as {@code tools/xdvdfs.py extract}
 * does on a PC. The files go to a sibling folder ({@code game.import}) first
 * and are moved into the game folder only when every file is complete, with
 * {@code default.xex} last: a cancelled or failed import never leaves a folder
 * the launcher would take for a game.
 *
 * Disc 2 is told apart by its contents: it carries the DLC package the console
 * installs from the disc (Compatibility Pack 4, an STFS package of this title
 * under {@code content/}). Only that package is copied, into the app's
 * {@code dlc/} folder ({@code dlc.import} first), where the game installs it at
 * its next start (app/src/ds2_dlc.cpp).
 *
 * The running import is process-wide ({@link #current}), so the game folder
 * page re-attaches to it when it is opened again.
 */
final class IsoImport {
    static final long TITLE_ID = 0x465307E4L;
    /**
     * sha256 of the tested disc's default.xex. Empty = not pinned yet (any default.xex with the
     * right title ID is accepted); set it once the compiled XEX is fixed.
     */
    static final String TESTED_XEX_SHA256 = "";
    /** default.xex is read whole to hash it before the import . */
    private static final long MAX_XEX_BYTES = 64L << 20;
    /** Free space kept beyond the game's size. */
    static final long SPACE_MARGIN = 256L << 20;

    /** Disc 1 (the game) or disc 2 (its DLC package). */
    enum Kind { GAME, DLC }

    /**
     * The display name of an STFS package of this title (CON / LIVE / PIRS header, title ID at
     * 0x360, UTF-16 name at 0x411), or null for anything else.
     */
    static String dlcPackageName(byte[] head) {
        if (head == null || head.length < 0x411 + 0x80) return null;
        String magic = new String(head, 0, 4, java.nio.charset.StandardCharsets.US_ASCII);
        if (!magic.equals("CON ") && !magic.equals("LIVE") && !magic.equals("PIRS")) return null;
        long title = ByteBuffer.wrap(head, 0x360, 4).getInt() & 0xFFFFFFFFL;
        if (title != TITLE_ID) return null;
        String name = new String(head, 0x411, 0x80, java.nio.charset.StandardCharsets.UTF_16BE);
        int end = name.indexOf('\0');
        name = (end >= 0 ? name.substring(0, end) : name).trim();
        return name.isEmpty() ? "DLC package" : name;
    }

    /** A picked disc image, opened and listed, before the import starts. */
    static final class Disc implements AutoCloseable {
        final ParcelFileDescriptor descriptor;
        final FileInputStream stream;
        final XdvdfsReader reader;
        /** Disc 1: every file to extract. Disc 2: only the DLC package(s). */
        final List<XdvdfsReader.Entry> entries;
        final Kind kind;
        /** Disc 2: the package's display name ("" for disc 1). */
        final String packageName;
        final long totalBytes;
        final int fileCount;
        /** -1 when default.xex has no readable title ID. */
        final long titleId;
        /** sha256 of the disc's default.xex ("" when it is too large to be this game's). */
        final String xexSha256;

        private Disc(ParcelFileDescriptor descriptor, FileInputStream stream, XdvdfsReader reader,
                List<XdvdfsReader.Entry> entries, Kind kind, String packageName, long titleId, String xexSha256) {
            this.descriptor = descriptor;
            this.stream = stream;
            this.reader = reader;
            this.entries = entries;
            this.kind = kind;
            this.packageName = packageName;
            this.titleId = titleId;
            this.xexSha256 = xexSha256;
            long total = 0;
            int files = 0;
            for (XdvdfsReader.Entry entry : entries) if (!entry.isDir) { total += entry.size; ++files; }
            this.totalBytes = total;
            this.fileCount = files;
        }

        @Override
        public void close() {
            try { stream.close(); } catch (IOException ignored) {}
            try { descriptor.close(); } catch (IOException ignored) {}
        }
    }

    /** Opens and lists a disc image; slow on SD cards, so not on the UI thread. */
    static Disc open(Context context, Uri uri) throws IOException {
        ParcelFileDescriptor descriptor = context.getContentResolver().openFileDescriptor(uri, "r");
        if (descriptor == null) throw new IOException("The file could not be opened.");
        FileInputStream stream = new FileInputStream(descriptor.getFileDescriptor());
        try {
            FileChannel channel = stream.getChannel();
            try {
                channel.size();
                channel.position(0);
            } catch (IOException e) {
                throw new IOException("This file cannot be read directly. Copy the ISO to the "
                    + "device storage or SD card and pick it there.");
            }
            XdvdfsReader reader;
            try {
                reader = new XdvdfsReader(channel);
            } catch (IOException e) {
                throw new IOException("This is not an Xbox 360 disc image (ISO).");
            }
            // The console's own system update ($SystemUpdate, ~150 MB) is never read by the game.
            List<XdvdfsReader.Entry> entries = new java.util.ArrayList<>(reader.entries());
            entries.removeIf(e -> e.path.startsWith("$SystemUpdate"));
            XdvdfsReader.Entry xex = XdvdfsReader.find(entries, "default.xex");
            boolean hasXex = xex != null && !xex.isDir;
            long titleId = hasXex ? XdvdfsReader.xexTitleId(reader.head(xex, 64 << 10)) : -1;
            if (titleId == TITLE_ID) {
                String xexSha256 = xex.size <= MAX_XEX_BYTES ? sha256(reader.head(xex, (int) xex.size)) : "";
                return new Disc(descriptor, stream, reader, entries, Kind.GAME, "", titleId, xexSha256);
            }
            // Disc 2: its default.xex is the console's disc installer (FFED2000); the DLC package is
            // in content/0000000000000000/FFED2000/FFFFFFFF/.
            List<XdvdfsReader.Entry> packages = new java.util.ArrayList<>();
            String packageName = "";
            for (XdvdfsReader.Entry entry : entries) {
                if (entry.isDir || entry.size < (1L << 20)
                        || !entry.path.toLowerCase(Locale.ROOT).startsWith("content/")) continue;
                String name = dlcPackageName(reader.head(entry, 0x500));
                if (name == null) continue;
                packages.add(entry);
                if (packageName.isEmpty()) packageName = name;
            }
            if (!packages.isEmpty())
                return new Disc(descriptor, stream, reader, packages, Kind.DLC, packageName, titleId, "");
            if (!hasXex)
                throw new IOException("This disc has no default.xex and no Dark Souls II DLC package, so it is "
                    + "neither disc 1 nor disc 2 of the game.");
            // Another game: checkDisc names its title ID.
            return new Disc(descriptor, stream, reader, entries, Kind.GAME, "", titleId, "");
        } catch (IOException | RuntimeException e) {
            try { stream.close(); } catch (IOException ignored) {}
            descriptor.close();
            throw e;
        }
    }

    // Running import ------------------------------------------------------------

    enum State { RUNNING, CLEANING, DONE, FAILED, CANCELLED }

    private static volatile IsoImport current;

    final File target;
    final Kind kind;
    final long totalBytes;
    volatile State state = State.RUNNING;
    volatile long doneBytes;
    volatile String currentFile = "";
    /** Failure text, or the default.xex sha256 warning after success (null when it matches). */
    volatile String message;
    private volatile boolean cancelRequested;
    private final Disc disc;

    private IsoImport(Disc disc, File target) {
        this.disc = disc;
        this.target = target;
        this.kind = disc.kind;
        this.totalBytes = disc.totalBytes;
    }

    static IsoImport current() {
        return current;
    }

    /** Forgets a finished import once its result was shown. */
    static void clearFinished() {
        IsoImport job = current;
        if (job != null && job.finished()) current = null;
    }

    boolean finished() {
        return state == State.DONE || state == State.FAILED || state == State.CANCELLED;
    }

    void cancel() {
        cancelRequested = true;
    }

    /** Starts extracting {@code disc} into {@code target}; the import owns and closes the disc. */
    static IsoImport start(Disc disc, File target) {
        IsoImport job = new IsoImport(disc, target);
        current = job;
        Thread thread = new Thread(job::run, "ISO import");
        thread.setPriority(Thread.NORM_PRIORITY - 1);
        thread.start();
        return job;
    }

    static File stagingDir(File target) {
        return new File(target.getParentFile(), target.getName() + ".import");
    }

    private void run() {
        File staging = stagingDir(target);
        try {
            deleteRecursively(staging);
            if (staging.exists()) throw new IOException("Cannot clear " + staging);
            if (!staging.mkdirs()) throw new IOException("Cannot create " + staging);
            ByteBuffer buffer = ByteBuffer.allocateDirect(8 << 20);
            for (XdvdfsReader.Entry entry : disc.entries) {
                if (cancelRequested) break;
                // Disc 2: the package alone, without the disc's content/... folders.
                String name = kind == Kind.DLC ? entry.path.substring(entry.path.lastIndexOf('/') + 1) : entry.path;
                File destination = new File(staging, name);
                if (entry.isDir) {
                    if (!destination.isDirectory() && !destination.mkdirs())
                        throw new IOException("Cannot create " + destination);
                    continue;
                }
                currentFile = entry.path;
                File parent = destination.getParentFile();
                if (parent != null && !parent.isDirectory() && !parent.mkdirs())
                    throw new IOException("Cannot create " + parent);
                boolean complete;
                try (FileOutputStream output = new FileOutputStream(destination)) {
                    complete = disc.reader.copy(entry, output.getChannel(), buffer, count -> {
                        doneBytes += count;
                        return !cancelRequested;
                    });
                }
                if (!complete) break;
            }
            if (cancelRequested) {
                finishCleaning(staging, State.CANCELLED, null);
                return;
            }
            currentFile = "";
            // The disc's default.xex was checked before the import; this catches a bad read.
            String sha = kind == Kind.GAME ? sha256(new File(staging, xexName(staging))) : "";
            if (kind == Kind.GAME && !TESTED_XEX_SHA256.isEmpty() && !TESTED_XEX_SHA256.equals(sha)) {
                finishCleaning(staging, State.FAILED, "default.xex was not copied intact (sha256 "
                    + sha.substring(0, 16) + "…). Try again; if it fails again, copy the ISO to the "
                    + "device's internal storage and import it from there.");
                return;
            }
            moveIntoPlace(staging, target);
            message = null;
            state = State.DONE;
        } catch (IOException | RuntimeException e) {
            String text = e.getMessage() != null ? e.getMessage() : e.toString();
            if (text.contains("ENOSPC") || text.contains("EACCES") || text.contains("EROFS"))
                text += "\nCheck the free space and that the folder is writable.";
            finishCleaning(staging, State.FAILED, text);
        } finally {
            disc.close();
        }
    }

    private void finishCleaning(File staging, State end, String text) {
        state = State.CLEANING;
        deleteRecursively(staging);
        message = text;
        state = end;
    }

    /** The staged default.xex's actual name (the disc may spell it in another case). */
    private static String xexName(File staging) {
        String[] names = staging.list();
        if (names != null) for (String name : names) if (name.equalsIgnoreCase("default.xex")) return name;
        return "default.xex";
    }

    /**
     * Moves the staged files into the game folder, replacing same-named ones
     * (an earlier partial copy); {@code default.xex} goes last so the folder
     * only looks like a game once everything else is in place.
     */
    static void moveIntoPlace(File staging, File target) throws IOException {
        if (!target.exists()) {
            if (staging.renameTo(target)) return;
            if (!target.mkdirs()) throw new IOException("Cannot create " + target);
        }
        File[] children = staging.listFiles();
        if (children == null) throw new IOException("Cannot list " + staging);
        File xex = null;
        for (File child : children) {
            if (child.getName().equalsIgnoreCase("default.xex")) { xex = child; continue; }
            moveReplacing(child, new File(target, child.getName()));
        }
        if (xex != null) moveReplacing(xex, new File(target, xex.getName()));
        deleteRecursively(staging);
    }

    private static void moveReplacing(File from, File to) throws IOException {
        deleteRecursively(to);
        if (!from.renameTo(to)) throw new IOException("Cannot move " + from + " to " + to);
    }

    static void deleteRecursively(File file) {
        if (file == null || !file.exists()) return;
        File[] children = file.isDirectory() ? file.listFiles() : null;
        if (children != null) for (File child : children) deleteRecursively(child);
        //noinspection ResultOfMethodCallIgnored
        file.delete();
    }

    static String sha256(File file) throws IOException {
        MessageDigest digest;
        try {
            digest = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IOException(e);
        }
        byte[] buffer = new byte[1 << 20];
        try (InputStream input = new FileInputStream(file)) {
            for (int n; (n = input.read(buffer)) > 0; ) digest.update(buffer, 0, n);
        }
        return hex(digest);
    }

    static String sha256(byte[] bytes) throws IOException {
        MessageDigest digest;
        try {
            digest = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IOException(e);
        }
        digest.update(bytes);
        return hex(digest);
    }

    private static String hex(MessageDigest digest) {
        StringBuilder hex = new StringBuilder();
        for (byte b : digest.digest()) hex.append(String.format(Locale.ROOT, "%02x", b));
        return hex.toString();
    }

    static String gigabytes(long bytes) {
        return String.format(Locale.ROOT, "%.2f GB", bytes / 1e9);
    }
}

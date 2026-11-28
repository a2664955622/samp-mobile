package com.sampmobile.launcher.utils;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Manages the SA-MP 0.3.DL / open.mp custom model disk cache under {@code files/SAMP/cache/}.
 * Supports scanning per-server & shared {@code local/} cache directories, directly importing
 * PC 0.3.DL cache {@code .zip}, {@code .7z}, and {@code .rar} (RAR4/RAR5) archives (with arbitrary
 * nested folder hierarchies, nested archives, and Windows GBK/UTF-8/UTF-16 paths), recursive SAF
 * directories, or individual {@code 0x*.dff} / {@code 0x*.txd} cache files, and deleting cache folders.
 */
public final class ArtworkCacheManager {

    private static final int IO_BUFFER_SIZE = 64 * 1024;
    private static final long MAX_SINGLE_FILE_BYTES = 64L * 1024L * 1024L;
    private static final long MAX_STAGED_ARCHIVE_BYTES = 4L * 1024L * 1024L * 1024L;
    private static final int MAX_FOLDER_NAME_LEN = 96;
    private static final int MAX_ZIP_NESTING = 3;
    private static final int MAX_SCAN_DEPTH = 4;
    private static final int MAX_TREE_DEPTH = 12;

    private static final int STREAM_RAW = 0;
    private static final int STREAM_ZIP = 1;
    private static final int STREAM_7Z = 2;
    private static final int STREAM_RAR = 3;

    private ArtworkCacheManager() {}

    public static final class FolderEntry {
        public final String name;
        public final boolean isLocal;
        public final int fileCount;
        public final long totalBytes;

        public FolderEntry(String name, boolean isLocal, int fileCount, long totalBytes) {
            this.name = name;
            this.isLocal = isLocal;
            this.fileCount = fileCount;
            this.totalBytes = totalBytes;
        }
    }

    public static final class CacheSummary {
        public final int totalFiles;
        public final long totalBytes;
        public final List<FolderEntry> folders;

        public CacheSummary(int totalFiles, long totalBytes, List<FolderEntry> folders) {
            this.totalFiles = totalFiles;
            this.totalBytes = totalBytes;
            this.folders = folders;
        }
    }

    public static final class ImportResult {
        public final int importedFiles;
        public final int skippedFiles;
        public final long importedBytes;
        public final boolean cancelled;
        public final boolean passwordRequired;
        public final String archiveError;

        public ImportResult(int importedFiles, int skippedFiles, long importedBytes, boolean cancelled) {
            this(importedFiles, skippedFiles, importedBytes, cancelled, false, null);
        }

        public ImportResult(int importedFiles, int skippedFiles, long importedBytes, boolean cancelled,
                            boolean passwordRequired, String archiveError) {
            this.importedFiles = importedFiles;
            this.skippedFiles = skippedFiles;
            this.importedBytes = importedBytes;
            this.cancelled = cancelled;
            this.passwordRequired = passwordRequired;
            this.archiveError = archiveError;
        }
    }

    public interface ProgressListener {
        void onProgress(int importedFiles, int skippedFiles, String currentName);
    }

    public static File getCacheRoot(Context context) {
        if (context == null) return null;
        File base = context.getExternalFilesDir(null);
        if (base == null) return null;
        return new File(base, "SAMP/cache");
    }

    /**
     * Validates and canonicalizes a 0.3.DL cache filename ({@code 0x<hex>.dff} or {@code 0x<hex>.txd},
     * optionally with browser/QQ duplicate suffixes such as {@code 0x99BAE090 (1).dff})
     * to match C++ {@code artwork::formatCacheFileName()} ({@code 0x%X.dff} / {@code 0x%X.txd}).
     * Strictly rejects arbitrary non-0x filenames (such as single-player / Modloader models).
     */
    public static String normalizeCacheFileName(String rawName) {
        if (rawName == null) return null;
        String trimmed = rawName.trim();
        if (trimmed.length() < 7 || trimmed.length() > 32) return null;

        int dot = trimmed.lastIndexOf('.');
        if (dot <= 2 || dot >= trimmed.length() - 1) return null;

        String ext = trimmed.substring(dot + 1).toLowerCase(Locale.US);
        if (!"dff".equals(ext) && !"txd".equals(ext)) {
            return null;
        }

        String stem = stripDuplicateDownloadSuffix(trimmed.substring(0, dot).trim());
        if (stem.length() < 3 || stem.charAt(0) != '0' || (stem.charAt(1) != 'x' && stem.charAt(1) != 'X')) {
            return null;
        }

        String hex = stem.substring(2);
        if (hex.isEmpty() || hex.length() > 8) return null;
        long val = 0;
        for (int i = 0; i < hex.length(); i++) {
            char c = hex.charAt(i);
            int digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = 10 + (c - 'a');
            else if (c >= 'A' && c <= 'F') digit = 10 + (c - 'A');
            else return null;
            val = (val << 4) | digit;
        }
        if (val == 0L) return null;
        return String.format(Locale.US, "0x%X.%s", (int) val, ext);
    }

    private static String stripDuplicateDownloadSuffix(String stem) {
        if (stem.length() > 5 && stem.charAt(stem.length() - 1) == ')') {
            int open = stem.lastIndexOf('(');
            if (open > 2 && open < stem.length() - 2) {
                boolean allDigits = true;
                for (int i = open + 1; i < stem.length() - 1; i++) {
                    char c = stem.charAt(i);
                    if (c < '0' || c > '9') {
                        allDigits = false;
                        break;
                    }
                }
                if (allDigits) {
                    return stem.substring(0, open).trim();
                }
            }
        }
        return stem;
    }

    public static boolean isSafeServerFolderName(String name) {
        if (name == null || name.isEmpty() || name.length() > MAX_FOLDER_NAME_LEN) {
            return false;
        }
        if (".".equals(name) || "..".equals(name) || name.startsWith(".") || name.endsWith(".")) {
            return false;
        }
        boolean hasDot = false;
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            boolean ok = (c >= 'a' && c <= 'z')
                    || (c >= 'A' && c <= 'Z')
                    || (c >= '0' && c <= '9')
                    || c == '.' || c == '_' || c == '-';
            if (!ok) return false;
            if (c == '.') hasDot = true;
        }
        return hasDot;
    }

    /**
     * Resolves any archive/relative entry path to a unified relative destination under
     * {@code SAMP/cache/local/0x<CRC>.dff|txd} regardless of wrapper folder depth or PC server
     * directory name (IP vs domain, port, colon {@code <ip:port>}, hyphen stripping, Chinese wrapper dirs).
     * Returns {@code null} if the entry is not a valid {@code 0x<CRC>.dff|txd} file or fails
     * Zip-Slip traversal checks.
     */
    public static String resolveZipEntryTarget(String rawEntryName) {
        String fileName = extractSafeLeafName(rawEntryName);
        if (fileName == null) return null;
        String canonicalFile = normalizeCacheFileName(fileName);
        if (canonicalFile == null) return null;
        return "local/" + canonicalFile;
    }

    private static String extractSafeLeafName(String rawEntryName) {
        if (rawEntryName == null || rawEntryName.isEmpty()) return null;
        String unified = rawEntryName.replace('\\', '/');
        while (unified.startsWith("/")) {
            unified = unified.substring(1);
        }
        if (unified.isEmpty() || unified.endsWith("/")) return null;

        String[] parts = unified.split("/");
        String last = null;
        for (String p : parts) {
            if (p.isEmpty() || ".".equals(p)) continue;
            if ("..".equals(p)) return null; // Zip-Slip guard
            last = p;
        }
        return last;
    }

    public static CacheSummary scan(File cacheRoot) {
        if (cacheRoot == null || !cacheRoot.isDirectory()) {
            return new CacheSummary(0, 0L, Collections.emptyList());
        }
        cleanupStagedTempFiles(cacheRoot);
        migrateLooseRootCacheFiles(cacheRoot);

        File[] children = cacheRoot.listFiles();
        if (children == null || children.length == 0) {
            return new CacheSummary(0, 0L, Collections.emptyList());
        }

        List<FolderEntry> folders = new ArrayList<>();
        int totalFiles = 0;
        long totalBytes = 0L;

        for (File child : children) {
            if (!child.isDirectory()) continue;
            String name = child.getName();
            if (".".equals(name) || "..".equals(name)) continue;

            long[] stats = new long[2]; // [0]=count, [1]=bytes
            scanDirectoryStats(child, MAX_SCAN_DEPTH, stats);
            int count = (int) stats[0];
            long bytes = stats[1];
            if (count == 0) continue; // skip empty directories like freshly created local/
            boolean isLocal = "local".equalsIgnoreCase(name);
            folders.add(new FolderEntry(name, isLocal, count, bytes));
            totalFiles += count;
            totalBytes += bytes;
        }

        Collections.sort(folders, (a, b) -> {
            if (a.isLocal != b.isLocal) return a.isLocal ? -1 : 1;
            int cmp = Long.compare(b.totalBytes, a.totalBytes);
            return cmp != 0 ? cmp : a.name.compareToIgnoreCase(b.name);
        });

        return new CacheSummary(totalFiles, totalBytes, folders);
    }

    private static void cleanupStagedTempFiles(File cacheRoot) {
        File[] children = cacheRoot.listFiles();
        if (children == null) return;
        for (File f : children) {
            if (f.isFile() && f.getName().startsWith(".stage_import_") && f.getName().endsWith(".tmp")) {
                //noinspection ResultOfMethodCallIgnored
                f.delete();
            }
        }
    }

    private static void migrateLooseRootCacheFiles(File cacheRoot) {
        File[] children = cacheRoot.listFiles();
        if (children == null) return;
        File localDir = null;
        for (File child : children) {
            if (!child.isFile()) continue;
            String canonical = normalizeCacheFileName(child.getName());
            if (canonical == null) continue;
            if (localDir == null) {
                localDir = new File(cacheRoot, "local");
                if (!localDir.exists() && !localDir.mkdirs()) {
                    return;
                }
            }
            File dst = new File(localDir, canonical);
            if (dst.exists()) {
                dst.delete();
            }
            //noinspection ResultOfMethodCallIgnored
            child.renameTo(dst);
        }
    }

    private static void scanDirectoryStats(File dir, int depthLeft, long[] stats) {
        File[] files = dir.listFiles();
        if (files == null) return;
        for (File f : files) {
            String name = f.getName();
            if (".".equals(name) || "..".equals(name)) continue;
            if (f.isFile()) {
                if (normalizeCacheFileName(name) != null) {
                    stats[0]++;
                    stats[1] += Math.max(0L, f.length());
                }
            } else if (depthLeft > 0 && f.isDirectory()) {
                scanDirectoryStats(f, depthLeft - 1, stats);
            }
        }
    }

    public static boolean deleteFolder(File cacheRoot, String folderName) {
        if (cacheRoot == null || folderName == null || folderName.isEmpty()) return false;
        if (folderName.contains("/") || folderName.contains("\\") || "..".equals(folderName) || ".".equals(folderName)) {
            return false;
        }
        File target = new File(cacheRoot, folderName);
        try {
            String rootCanon = cacheRoot.getCanonicalPath();
            String targetCanon = target.getCanonicalPath();
            if (!targetCanon.startsWith(rootCanon + File.separator)) {
                return false;
            }
        } catch (IOException e) {
            return false;
        }
        return deleteRecursively(target);
    }

    public static int clearAll(File cacheRoot) {
        if (cacheRoot == null || !cacheRoot.isDirectory()) return 0;
        File[] children = cacheRoot.listFiles();
        if (children == null) return 0;
        int deleted = 0;
        for (File child : children) {
            if (deleteRecursively(child)) {
                deleted++;
            }
        }
        return deleted;
    }

    private static boolean deleteRecursively(File f) {
        if (f == null || !f.exists()) return false;
        if (f.isDirectory()) {
            File[] kids = f.listFiles();
            if (kids != null) {
                for (File k : kids) {
                    deleteRecursively(k);
                }
            }
        }
        return f.delete();
    }

    /**
     * Imports one or more local {@link File}s ({@code .zip}/{@code .7z}/{@code .rar} archives,
     * loose {@code 0x<CRC>.dff|txd} files, or nested directories) into {@code cacheRoot/local/}.
     */
    public static ImportResult importFromFiles(List<File> sources, File cacheRoot,
                                               ProgressListener listener, AtomicBoolean cancelled) throws IOException {
        if (sources == null || sources.isEmpty() || cacheRoot == null) {
            return new ImportResult(0, 0, 0L, false, false, null);
        }
        if (!cacheRoot.exists()) {
            cacheRoot.mkdirs();
        }
        String cacheRootCanon = cacheRoot.getCanonicalPath();
        ImportSession session = new ImportSession(cacheRoot, cacheRootCanon, listener, cancelled);

        for (File src : sources) {
            if (session.isCancelled()) {
                break;
            }
            if (src == null || !src.exists()) {
                session.skipped++;
                continue;
            }
            if (src.isDirectory()) {
                importLocalDirectory(src, MAX_TREE_DEPTH, session);
            } else if (src.isFile()) {
                importLocalFile(src, session);
            }
        }
        return session.toResult();
    }

    private static void importLocalFile(File src, ImportSession session) throws IOException {
        try (BufferedInputStream bis = new BufferedInputStream(new FileInputStream(src), IO_BUFFER_SIZE)) {
            int kind = sniffStreamKind(bis, src.getName());
            if (kind == STREAM_7Z || kind == STREAM_RAR) {
                // Direct random-access extraction on the local File without copying!
                importNativeArchiveFile(src, session);
                return;
            }
            if (kind == STREAM_ZIP) {
                importZipStream(bis, 1, session);
                return;
            }
            String canonical = normalizeCacheFileName(src.getName());
            if (canonical == null) {
                session.skipped++;
                return;
            }
            writeCanonicalLocalFile(bis, canonical, session);
        }
    }

    private static void importLocalDirectory(File dir, int depthLeft, ImportSession session) throws IOException {
        File[] kids = dir.listFiles();
        if (kids == null) return;
        for (File child : kids) {
            if (session.isCancelled()) return;
            String name = child.getName();
            if (".".equals(name) || "..".equals(name) || "__MACOSX".equals(name)) continue;
            if (child.isDirectory()) {
                if (depthLeft > 0) {
                    importLocalDirectory(child, depthLeft - 1, session);
                }
            } else if (child.isFile()) {
                String canonical = normalizeCacheFileName(name);
                if (canonical != null) {
                    try (InputStream in = new FileInputStream(child)) {
                        writeCanonicalLocalFile(in, canonical, session);
                    }
                } else if (isSupportedArchiveExtension(name)) {
                    importLocalFile(child, session);
                } else {
                    session.skipped++;
                }
            }
        }
    }

    public static ImportResult importFromUris(Context context, List<Uri> uris, File cacheRoot,
                                              ProgressListener listener, AtomicBoolean cancelled) throws IOException {
        if (context == null || uris == null || uris.isEmpty() || cacheRoot == null) {
            return new ImportResult(0, 0, 0L, false, false, null);
        }
        if (!cacheRoot.exists()) {
            cacheRoot.mkdirs();
        }
        String cacheRootCanon = cacheRoot.getCanonicalPath();
        ContentResolver resolver = context.getContentResolver();
        ImportSession session = new ImportSession(cacheRoot, cacheRootCanon, listener, cancelled);

        for (Uri uri : uris) {
            if (session.isCancelled()) {
                break;
            }
            if (uri == null) continue;
            String displayName = queryDisplayName(resolver, uri);
            try (InputStream raw = resolver.openInputStream(uri)) {
                if (raw == null) {
                    session.skipped++;
                    continue;
                }
                importSingleStream(raw, displayName, session);
            }
        }
        return session.toResult();
    }

    /**
     * Recursively imports a user-selected SAF directory ({@code OpenDocumentTree}) at arbitrary
     * subfolder depth, normalizing all {@code 0x<CRC>.dff|txd} files (and any inner {@code .zip},
     * {@code .7z}, or {@code .rar} archives) into {@code SAMP/cache/local/}.
     */
    public static ImportResult importFromTreeUri(Context context, Uri treeUri, File cacheRoot,
                                                 ProgressListener listener, AtomicBoolean cancelled) throws IOException {
        if (context == null || treeUri == null || cacheRoot == null) {
            return new ImportResult(0, 0, 0L, false, false, null);
        }
        if (!cacheRoot.exists()) {
            cacheRoot.mkdirs();
        }
        String cacheRootCanon = cacheRoot.getCanonicalPath();
        ContentResolver resolver = context.getContentResolver();
        ImportSession session = new ImportSession(cacheRoot, cacheRootCanon, listener, cancelled);

        String rootDocId = DocumentsContract.getTreeDocumentId(treeUri);
        if (rootDocId != null && !rootDocId.isEmpty()) {
            traverseSafDirectory(resolver, treeUri, rootDocId, MAX_TREE_DEPTH, session);
        }
        return session.toResult();
    }

    private static void traverseSafDirectory(ContentResolver resolver, Uri treeUri, String dirDocId,
                                             int depthLeft, ImportSession session) throws IOException {
        if (session.isCancelled()) return;
        Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, dirDocId);
        List<String> subDirs = new ArrayList<>();
        List<String[]> candidateFiles = new ArrayList<>();

        try (Cursor c = resolver.query(childrenUri, new String[]{
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE
        }, null, null, null)) {
            if (c != null) {
                while (c.moveToNext()) {
                    if (session.isCancelled()) return;
                    String childId = c.getString(0);
                    String name = c.getString(1);
                    String mime = c.getString(2);
                    if (childId == null || name == null || ".".equals(name) || "..".equals(name) || "__MACOSX".equals(name)) {
                        continue;
                    }
                    if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                        if (depthLeft > 0) {
                            subDirs.add(childId);
                        }
                    } else {
                        candidateFiles.add(new String[]{childId, name});
                    }
                }
            }
        }

        for (String[] item : candidateFiles) {
            if (session.isCancelled()) return;
            String docId = item[0];
            String name = item[1];
            String canonical = normalizeCacheFileName(name);
            if (canonical != null) {
                Uri fileUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, docId);
                try (InputStream in = resolver.openInputStream(fileUri)) {
                    if (in == null) {
                        session.skipped++;
                    } else {
                        writeCanonicalLocalFile(in, canonical, session);
                    }
                }
            } else if (isSupportedArchiveExtension(name)) {
                Uri fileUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, docId);
                try (InputStream in = resolver.openInputStream(fileUri)) {
                    if (in == null) {
                        session.skipped++;
                    } else {
                        importSingleStream(in, name, session);
                    }
                }
            } else {
                session.skipped++;
            }
        }

        for (String subDirId : subDirs) {
            if (session.isCancelled()) return;
            traverseSafDirectory(resolver, treeUri, subDirId, depthLeft - 1, session);
        }
    }

    private static void importSingleStream(InputStream raw, String displayName, ImportSession session) throws IOException {
        BufferedInputStream bis = (raw instanceof BufferedInputStream)
                ? (BufferedInputStream) raw
                : new BufferedInputStream(raw, IO_BUFFER_SIZE);

        int kind = sniffStreamKind(bis, displayName);
        if (kind == STREAM_7Z || kind == STREAM_RAR) {
            stageAndImportNativeArchive(bis, session);
            return;
        }
        if (kind == STREAM_ZIP) {
            importZipStream(bis, 1, session);
            return;
        }

        String canonical = normalizeCacheFileName(displayName);
        if (canonical == null) {
            session.skipped++;
            return;
        }
        writeCanonicalLocalFile(bis, canonical, session);
    }

    private static void stageAndImportNativeArchive(InputStream in, ImportSession session) throws IOException {
        File stageFile = new File(session.cacheRoot, ".stage_import_" + System.nanoTime() + ".tmp");
        try {
            if (session.listener != null) {
                session.listener.onProgress(session.imported, session.skipped, I18n.tr("正在读取所选文件…"));
            }
            long copied = 0L;
            try (FileOutputStream fos = new FileOutputStream(stageFile)) {
                int n;
                while ((n = in.read(session.buf)) != -1) {
                    if (session.isCancelled()) {
                        return;
                    }
                    copied += n;
                    if (copied > MAX_STAGED_ARCHIVE_BYTES) {
                        throw new IOException(I18n.tr("压缩包体积超出安全上限"));
                    }
                    fos.write(session.buf, 0, n);
                }
                fos.flush();
            }
            if (copied <= 0L) {
                session.skipped++;
                return;
            }
            importNativeArchiveFile(stageFile, session);
        } finally {
            if (stageFile.exists()) {
                //noinspection ResultOfMethodCallIgnored
                stageFile.delete();
            }
        }
    }

    private static void importNativeArchiveFile(File archiveFile, ImportSession session) throws IOException {
        File localDir = new File(session.cacheRoot, "local");
        if (!localDir.exists()) {
            localDir.mkdirs();
        }
        final int baseImported = session.imported;
        final int baseSkipped = session.skipped;

        NativeArchiveExtractor.Result res = NativeArchiveExtractor.extractCacheArchive(
                archiveFile,
                localDir,
                (nativeImported, nativeSkipped, relPath) -> {
                    if (session.isCancelled()) {
                        return false;
                    }
                    if (session.listener != null && relPath != null && !relPath.isEmpty()) {
                        session.listener.onProgress(
                                baseImported + nativeImported,
                                baseSkipped + nativeSkipped,
                                relPath);
                    }
                    return !session.isCancelled();
                });

        session.imported += Math.max(0, res.importedFiles);
        session.skipped += Math.max(0, res.skippedFiles);
        session.bytes += Math.max(0L, res.importedBytes);

        // Process any nested .zip files unpacked from inside .7z / .rar
        for (String nestedZipPath : res.nestedZipPaths) {
            File nestedZip = new File(nestedZipPath);
            try {
                if (!session.isCancelled() && nestedZip.isFile()) {
                    try (InputStream zin = new BufferedInputStream(new FileInputStream(nestedZip), IO_BUFFER_SIZE)) {
                        importZipStream(zin, 2, session);
                    }
                }
            } finally {
                if (nestedZip.exists()) {
                    //noinspection ResultOfMethodCallIgnored
                    nestedZip.delete();
                }
            }
        }

        if (res.status == NativeArchiveExtractor.STATUS_PASSWORD_REQUIRED) {
            session.passwordRequired = true;
            if (session.archiveError == null && !res.error.isEmpty()) {
                session.archiveError = res.error;
            }
        } else if (res.status == NativeArchiveExtractor.STATUS_ERROR && session.archiveError == null && !res.error.isEmpty()) {
            session.archiveError = res.error;
        }
    }

    private static int sniffStreamKind(BufferedInputStream bis, String displayName) throws IOException {
        bis.mark(16);
        byte[] hdr = new byte[8];
        int read = 0;
        while (read < hdr.length) {
            int n = bis.read(hdr, read, hdr.length - read);
            if (n <= 0) break;
            read += n;
        }
        bis.reset();

        if (read >= 4 && hdr[0] == 0x50 && hdr[1] == 0x4B) {
            byte b2 = hdr[2];
            byte b3 = hdr[3];
            if ((b2 == 0x03 && b3 == 0x04) || (b2 == 0x05 && b3 == 0x06) || (b2 == 0x07 && b3 == 0x08)) {
                return STREAM_ZIP;
            }
        }
        if (read >= 6
                && (hdr[0] & 0xFF) == 0x37 && (hdr[1] & 0xFF) == 0x7A
                && (hdr[2] & 0xFF) == 0xBC && (hdr[3] & 0xFF) == 0xAF
                && (hdr[4] & 0xFF) == 0x27 && (hdr[5] & 0xFF) == 0x1C) {
            return STREAM_7Z;
        }
        if (read >= 6
                && hdr[0] == 'R' && hdr[1] == 'a' && hdr[2] == 'r' && hdr[3] == '!'
                && (hdr[4] & 0xFF) == 0x1A && (hdr[5] & 0xFF) == 0x07) {
            return STREAM_RAR;
        }

        if (read == 0) {
            if (has7zExtension(displayName)) return STREAM_7Z;
            if (hasRarExtension(displayName)) return STREAM_RAR;
            if (hasZipExtension(displayName)) return STREAM_ZIP;
        }
        return STREAM_RAW;
    }

    private static ZipInputStream openTolerantZipStream(InputStream in) {
        try {
            return new ZipInputStream(in, Charset.forName("GBK"));
        } catch (Throwable ignored) {
            return new ZipInputStream(in, StandardCharsets.UTF_8);
        }
    }

    private static void importZipStream(InputStream stream, int depth, ImportSession session) throws IOException {
        try (ZipInputStream zis = openTolerantZipStream(stream)) {
            ZipEntry entry;
            while ((entry = zis.getNextEntry()) != null) {
                if (session.isCancelled()) {
                    return;
                }
                if (entry.isDirectory()) {
                    zis.closeEntry();
                    continue;
                }
                String entryName = entry.getName();
                String relTarget = resolveZipEntryTarget(entryName);
                if (relTarget != null) {
                    File dst = new File(session.cacheRoot, relTarget);
                    File parent = dst.getParentFile();
                    if (parent != null && !parent.exists()) {
                        parent.mkdirs();
                    }
                    String dstCanon = dst.getCanonicalPath();
                    if (!dstCanon.startsWith(session.cacheRootCanon + File.separator)) {
                        session.skipped++;
                        zis.closeEntry();
                        continue;
                    }
                    long written = writeAtomically(zis, dst, session.buf, session.cancelled);
                    zis.closeEntry();
                    if (written > 0) {
                        session.onImported(written, relTarget);
                    } else {
                        session.skipped++;
                    }
                    continue;
                }

                // Support nested .zip / .7z / .rar inside .zip
                String leaf = extractSafeLeafName(entryName);
                if (leaf != null && depth < MAX_ZIP_NESTING) {
                    if (hasZipExtension(leaf)) {
                        importZipStream(new NonClosingInputStream(zis), depth + 1, session);
                        zis.closeEntry();
                        continue;
                    }
                    if (has7zExtension(leaf) || hasRarExtension(leaf)) {
                        stageAndImportNativeArchive(new NonClosingInputStream(zis), session);
                        zis.closeEntry();
                        continue;
                    }
                }
                session.skipped++;
                zis.closeEntry();
            }
        }
    }

    private static void writeCanonicalLocalFile(InputStream in, String canonical, ImportSession session) throws IOException {
        File dstDir = new File(session.cacheRoot, "local");
        if (!dstDir.exists()) {
            dstDir.mkdirs();
        }
        File dst = new File(dstDir, canonical);
        String dstCanon = dst.getCanonicalPath();
        if (!dstCanon.startsWith(session.cacheRootCanon + File.separator)) {
            session.skipped++;
            return;
        }
        long written = writeAtomically(in, dst, session.buf, session.cancelled);
        if (written > 0) {
            session.onImported(written, "local/" + canonical);
        } else {
            session.skipped++;
        }
    }

    private static boolean isSupportedArchiveExtension(String name) {
        return hasZipExtension(name) || has7zExtension(name) || hasRarExtension(name);
    }

    private static boolean hasZipExtension(String name) {
        if (name == null) return false;
        String lower = stripDuplicateDownloadSuffixForArchive(name.trim().toLowerCase(Locale.US));
        return lower.endsWith(".zip");
    }

    private static boolean has7zExtension(String name) {
        if (name == null) return false;
        String lower = stripDuplicateDownloadSuffixForArchive(name.trim().toLowerCase(Locale.US));
        return lower.endsWith(".7z");
    }

    private static boolean hasRarExtension(String name) {
        if (name == null) return false;
        String lower = stripDuplicateDownloadSuffixForArchive(name.trim().toLowerCase(Locale.US));
        return lower.endsWith(".rar");
    }

    private static String stripDuplicateDownloadSuffixForArchive(String lowerName) {
        int dot = lowerName.lastIndexOf('.');
        if (dot > 0) {
            String base = stripDuplicateDownloadSuffix(lowerName.substring(0, dot).trim());
            return base + lowerName.substring(dot);
        }
        return lowerName;
    }

    private static final class NonClosingInputStream extends FilterInputStream {
        NonClosingInputStream(InputStream in) {
            super(in);
        }

        @Override
        public void close() {
            // Keep parent ZipInputStream open for subsequent entries
        }
    }

    private static final class ImportSession {
        final File cacheRoot;
        final String cacheRootCanon;
        final ProgressListener listener;
        final AtomicBoolean cancelled;
        final byte[] buf = new byte[IO_BUFFER_SIZE];
        int imported = 0;
        int skipped = 0;
        long bytes = 0L;
        long lastNotifyMs = 0L;
        boolean passwordRequired = false;
        String archiveError = null;

        ImportSession(File cacheRoot, String cacheRootCanon, ProgressListener listener, AtomicBoolean cancelled) {
            this.cacheRoot = cacheRoot;
            this.cacheRootCanon = cacheRootCanon;
            this.listener = listener;
            this.cancelled = cancelled;
        }

        boolean isCancelled() {
            return cancelled != null && cancelled.get();
        }

        void onImported(long written, String relPath) {
            imported++;
            bytes += written;
            long now = System.currentTimeMillis();
            if (listener != null && (now - lastNotifyMs >= 80L || imported <= 3)) {
                lastNotifyMs = now;
                listener.onProgress(imported, skipped, relPath);
            }
        }

        ImportResult toResult() {
            return new ImportResult(imported, skipped, bytes, isCancelled(), passwordRequired, archiveError);
        }
    }

    private static long writeAtomically(InputStream in, File dst, byte[] buf, AtomicBoolean cancelled) throws IOException {
        File tmp = new File(dst.getParentFile(), dst.getName() + ".imp.tmp");
        long total = 0L;
        boolean ok = false;
        try {
            try (FileOutputStream fos = new FileOutputStream(tmp)) {
                int n;
                while ((n = in.read(buf)) != -1) {
                    if (cancelled != null && cancelled.get()) {
                        return -1L;
                    }
                    total += n;
                    if (total > MAX_SINGLE_FILE_BYTES) {
                        return -1L;
                    }
                    fos.write(buf, 0, n);
                }
                fos.flush();
            }
            if (total <= 0L) {
                return -1L;
            }
            if (dst.exists()) {
                dst.delete();
            }
            ok = tmp.renameTo(dst);
            return ok ? total : -1L;
        } finally {
            if (!ok && tmp.exists()) {
                tmp.delete();
            }
        }
    }

    private static String queryDisplayName(ContentResolver resolver, Uri uri) {
        if (resolver == null || uri == null) return null;
        try (Cursor c = resolver.query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                String name = c.getString(0);
                if (name != null && !name.isEmpty()) return name;
            }
        } catch (Throwable ignored) {
        }
        String last = uri.getLastPathSegment();
        if (last != null) {
            int slash = last.lastIndexOf('/');
            return (slash >= 0) ? last.substring(slash + 1) : last;
        }
        return null;
    }
}

package com.sampmobile.launcher.utils;

import androidx.annotation.Keep;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * JNI wrapper over {@code libSAMPArchive.so} (7-Zip C SDK + UnRAR RARDLL) for extracting
 * {@code 0x<CRC>.dff} / {@code 0x<CRC>.txd} cache entries from {@code .7z} and {@code .rar}
 * (RAR4/RAR5, solid or non-solid) archives inside the launcher process.
 */
@Keep
public final class NativeArchiveExtractor {

    public static final int STATUS_ERROR = 0;
    public static final int STATUS_OK = 1;
    public static final int STATUS_CANCELLED = 2;
    public static final int STATUS_PASSWORD_REQUIRED = 3;

    @Keep
    public interface BridgeCallback {
        @Keep
        boolean onNativeProgress(int importedFiles, int skippedFiles, String relPath);
    }

    public interface Backend {
        Result extract(File archiveFile, File localCacheDir, BridgeCallback callback);
    }

    public static final class Result {
        public final int status;
        public final int importedFiles;
        public final int skippedFiles;
        public final long importedBytes;
        public final String error;
        public final List<String> nestedZipPaths;

        public Result(int status, int importedFiles, int skippedFiles, long importedBytes,
                      String error, List<String> nestedZipPaths) {
            this.status = status;
            this.importedFiles = importedFiles;
            this.skippedFiles = skippedFiles;
            this.importedBytes = importedBytes;
            this.error = error != null ? error : "";
            this.nestedZipPaths = nestedZipPaths != null ? nestedZipPaths : Collections.emptyList();
        }
    }

    private static volatile int sLoadState = 0; // 0 = untried, 1 = loaded, -1 = unavailable (e.g. host JVM test)
    private static volatile Backend sOverrideBackend = null;

    private NativeArchiveExtractor() {}

    static void setBackendForTesting(Backend backend) {
        sOverrideBackend = backend;
    }

    public static boolean isAvailable() {
        if (sOverrideBackend != null) {
            return true;
        }
        int state = sLoadState;
        if (state != 0) {
            return state == 1;
        }
        synchronized (NativeArchiveExtractor.class) {
            if (sLoadState == 0) {
                try {
                    System.loadLibrary("SAMPArchive");
                    sLoadState = 1;
                } catch (Throwable ignored) {
                    sLoadState = -1;
                }
            }
            return sLoadState == 1;
        }
    }

    public static Result extractCacheArchive(File archiveFile, File localCacheDir, BridgeCallback callback) {
        if (archiveFile == null || localCacheDir == null) {
            return new Result(STATUS_ERROR, 0, 0, 0L, "invalid archive arguments", Collections.emptyList());
        }
        Backend override = sOverrideBackend;
        if (override != null) {
            return override.extract(archiveFile, localCacheDir, callback);
        }
        if (!isAvailable()) {
            return new Result(STATUS_ERROR, 0, 0, 0L, "native archive library unavailable", Collections.emptyList());
        }
        long[] nums = new long[4];
        String[] strings = nativeExtractCacheArchive(
                archiveFile.getAbsolutePath(),
                localCacheDir.getAbsolutePath(),
                callback,
                nums);
        String err = (strings != null && strings.length > 0 && strings[0] != null) ? strings[0] : "";
        List<String> nestedZips = Collections.emptyList();
        if (strings != null && strings.length > 1) {
            nestedZips = new ArrayList<>(strings.length - 1);
            for (int i = 1; i < strings.length; i++) {
                if (strings[i] != null && !strings[i].isEmpty()) {
                    nestedZips.add(strings[i]);
                }
            }
        }
        return new Result((int) nums[0], (int) nums[1], (int) nums[2], nums[3], err, nestedZips);
    }

    @Keep
    private static native String[] nativeExtractCacheArchive(
            String archivePath,
            String localCacheDir,
            Object bridgeCallback,
            long[] outNumbers);
}

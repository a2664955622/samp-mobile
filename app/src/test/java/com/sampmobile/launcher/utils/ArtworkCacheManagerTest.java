package com.sampmobile.launcher.utils;

import org.junit.After;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.Charset;
import java.util.Arrays;
import java.util.Collections;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

import static org.junit.Assert.*;

public class ArtworkCacheManagerTest {

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    @After
    public void tearDown() {
        NativeArchiveExtractor.setBackendForTesting(null);
    }

    @Test
    public void normalizeCacheFileName_canonicalizesValid0xAndRejectsArbitraryModels() {
        assertEquals("0x12AB.dff", ArtworkCacheManager.normalizeCacheFileName("0x0012ab.DFF"));
        assertEquals("0x99BAE090.txd", ArtworkCacheManager.normalizeCacheFileName("0X99bae090.txd"));
        assertEquals("0x1.dff", ArtworkCacheManager.normalizeCacheFileName("0x1.dff"));
        assertEquals("0x99BAE090.dff", ArtworkCacheManager.normalizeCacheFileName("0x99BAE090 (1).dff"));
        assertEquals("0xCAFEBABE.txd", ArtworkCacheManager.normalizeCacheFileName("0xcafebabe(12).TXD"));

        // Strictly reject non-0x filenames (Modloader / single-player models) and invalid hashes
        assertNull(ArtworkCacheManager.normalizeCacheFileName(null));
        assertNull(ArtworkCacheManager.normalizeCacheFileName(""));
        assertNull(ArtworkCacheManager.normalizeCacheFileName("0x0.dff"));
        assertNull(ArtworkCacheManager.normalizeCacheFileName("0x123456789.dff")); // > 8 hex digits
        assertNull(ArtworkCacheManager.normalizeCacheFileName("0x1234.col"));
        assertNull(ArtworkCacheManager.normalizeCacheFileName("model.dff"));
        assertNull(ArtworkCacheManager.normalizeCacheFileName("infernus.txd"));
        assertNull(ArtworkCacheManager.normalizeCacheFileName("cafe.dff"));
        assertNull(ArtworkCacheManager.normalizeCacheFileName("99BAE090.dff")); // missing 0x prefix
        assertNull(ArtworkCacheManager.normalizeCacheFileName("0xGGGG.txd"));
    }

    @Test
    public void resolveZipEntryTarget_unifiesAnyPcHierarchyIntoLocalAndBlocksZipSlip() {
        assertEquals("local/0x99BAE090.txd",
                ArtworkCacheManager.resolveZipEntryTarget("cache/116.62.52.151.7777/0x99bae090.TXD"));
        assertEquals("local/0x99BAE090.txd",
                ArtworkCacheManager.resolveZipEntryTarget("中文包/116.62.52.151:7777/0x99bae090.TXD"));
        assertEquals("local/0x99BAE090.dff",
                ArtworkCacheManager.resolveZipEntryTarget(
                        "GTA San Andreas User Files\\SAMP\\cache\\Play.MyServer.COM.7777\\0x99BAE090 (1).dff"));
        assertEquals("local/0xDEADBEEF.dff",
                ArtworkCacheManager.resolveZipEntryTarget("0.3.DL/v2.1/models/0xdeadbeef.dff"));
        assertEquals("local/0xDEADBEEF.dff",
                ArtworkCacheManager.resolveZipEntryTarget("SAMP/cache/local/0xdeadbeef.dff"));
        assertEquals("local/0xCAFEBABE.dff",
                ArtworkCacheManager.resolveZipEntryTarget("0xcafebabe.dff"));

        assertNull(ArtworkCacheManager.resolveZipEntryTarget("../116.62.52.151.7777/0x1234.dff"));
        assertNull(ArtworkCacheManager.resolveZipEntryTarget("cache/../../0x1234.dff"));
        assertNull(ArtworkCacheManager.resolveZipEntryTarget("cache/116.62.52.151.7777/infernus.dff"));
        assertNull(ArtworkCacheManager.resolveZipEntryTarget("cache/116.62.52.151.7777/readme.txt"));
        assertNull(ArtworkCacheManager.resolveZipEntryTarget("cache/116.62.52.151.7777/"));
    }

    @Test
    public void importFromFiles_handlesGbkChinesePathsNestedZipAndExtensionlessZip() throws IOException {
        File cacheRoot = tmp.newFolder("cache_gbk");

        File gbkZip = tmp.newFile("pc_pack.zip");
        try (FileOutputStream fos = new FileOutputStream(gbkZip);
             ZipOutputStream zos = new ZipOutputStream(fos, Charset.forName("GBK"))) {
            putZipEntry(zos, "某服0.3.DL离线包/GTA San Andreas User Files/SAMP/cache/Play.Server.CN.7777/0x99bae090.DFF", 128);
            putZipEntry(zos, "某服0.3.DL离线包/0.3.DL/0x0012ab.txd", 256);
            putZipEntry(zos, "某服0.3.DL离线包/infernus.dff", 512);
            putZipEntry(zos, "某服0.3.DL离线包/说明.txt", 32);
        }

        ByteArrayOutputStream innerBytes = new ByteArrayOutputStream();
        try (ZipOutputStream innerZos = new ZipOutputStream(innerBytes)) {
            putZipEntry(innerZos, "cn1.myserver.com.7777/0xDEADBEEF (1).dff", 64);
        }
        File opaqueZipBlob = tmp.newFile("saf_download_blob_88991");
        try (FileOutputStream fos = new FileOutputStream(opaqueZipBlob);
             ZipOutputStream outerZos = new ZipOutputStream(fos)) {
            outerZos.putNextEntry(new ZipEntry("离线缓存/cache.zip"));
            outerZos.write(innerBytes.toByteArray());
            outerZos.closeEntry();
        }

        ArtworkCacheManager.ImportResult res = ArtworkCacheManager.importFromFiles(
                Arrays.asList(gbkZip, opaqueZipBlob), cacheRoot, null, null);
        assertFalse(res.cancelled);
        assertFalse(res.passwordRequired);
        assertNull(res.archiveError);
        assertEquals(3, res.importedFiles);
        assertEquals(128L + 256L + 64L, res.importedBytes);
        assertEquals(2, res.skippedFiles);

        assertTrue(new File(cacheRoot, "local/0x99BAE090.dff").isFile());
        assertTrue(new File(cacheRoot, "local/0x12AB.txd").isFile());
        assertTrue(new File(cacheRoot, "local/0xDEADBEEF.dff").isFile());
        assertFalse(new File(cacheRoot, "local/infernus.dff").exists());
    }

    @Test
    public void importFromFiles_routes7zAndRarMagicBytesAndNestedZipsAndPasswordStatus() throws IOException {
        File cacheRoot = tmp.newFolder("cache_7z_rar");

        NativeArchiveExtractor.setBackendForTesting((archiveFile, localCacheDir, callback) -> {
            try {
                if (archiveFile.getName().contains("encrypted")) {
                    return new NativeArchiveExtractor.Result(
                            NativeArchiveExtractor.STATUS_PASSWORD_REQUIRED,
                            0, 0, 0L, ".rar 压缩包设有密码保护", Collections.emptyList());
                }
                // Simulate native extractor writing a 0x<CRC>.dff + unpacking a nested .zip
                writeBytes(new File(localCacheDir, "0x7777AAAA.dff"), 180);
                if (callback != null) {
                    callback.onNativeProgress(1, 0, "local/0x7777AAAA.dff");
                }
                File nestedZip = new File(localCacheDir, ".nested_test.zip.tmp");
                try (FileOutputStream fos = new FileOutputStream(nestedZip);
                     ZipOutputStream zos = new ZipOutputStream(fos)) {
                    putZipEntry(zos, "127.0.0.1:7777/0x8888BBBB.txd", 220);
                }
                return new NativeArchiveExtractor.Result(
                        NativeArchiveExtractor.STATUS_OK,
                        1, 1, 180L, "", Collections.singletonList(nestedZip.getAbsolutePath()));
            } catch (IOException e) {
                throw new RuntimeException(e);
            }
        });

        // 1. Chinese-named .7z detected via magic header (even without .7z suffix)
        File chinese7z = tmp.newFile("服务器缓存包_无后缀");
        try (FileOutputStream fos = new FileOutputStream(chinese7z)) {
            fos.write(new byte[]{0x37, 0x7A, (byte) 0xBC, (byte) 0xAF, 0x27, 0x1C, 0x00, 0x04});
        }
        ArtworkCacheManager.ImportResult r7z = ArtworkCacheManager.importFromFiles(
                Collections.singletonList(chinese7z), cacheRoot, null, null);
        assertEquals(2, r7z.importedFiles);
        assertEquals(400L, r7z.importedBytes);
        assertTrue(new File(cacheRoot, "local/0x7777AAAA.dff").isFile());
        assertTrue(new File(cacheRoot, "local/0x8888BBBB.txd").isFile());
        assertFalse(new File(cacheRoot, "local/.nested_test.zip.tmp").exists());

        // 2. Password-protected .rar surfaces passwordRequired cleanly
        File encRar = tmp.newFile("encrypted.rar");
        try (FileOutputStream fos = new FileOutputStream(encRar)) {
            fos.write(new byte[]{'R', 'a', 'r', '!', 0x1A, 0x07, 0x01, 0x00});
        }
        File cacheRoot2 = tmp.newFolder("cache_enc_rar");
        ArtworkCacheManager.ImportResult rEnc = ArtworkCacheManager.importFromFiles(
                Collections.singletonList(encRar), cacheRoot2, null, null);
        assertEquals(0, rEnc.importedFiles);
        assertTrue(rEnc.passwordRequired);

        // 3. Extracted directory with multi-level PC folders
        File extractedDir = tmp.newFolder("extracted_pack");
        File deep = new File(extractedDir, "SAMP/cache/114.66.17.73.7777");
        assertTrue(deep.mkdirs());
        writeBytes(new File(deep, "0xABCDEF01.dff"), 300);
        writeBytes(new File(deep, "0xABCDEF02.txd"), 200);
        writeBytes(new File(deep, "skin.dff"), 100);

        ArtworkCacheManager.ImportResult rDir = ArtworkCacheManager.importFromFiles(
                Collections.singletonList(extractedDir), cacheRoot2, null, null);
        assertEquals(2, rDir.importedFiles);
        assertEquals(500L, rDir.importedBytes);
        assertTrue(new File(cacheRoot2, "local/0xABCDEF01.dff").isFile());
        assertTrue(new File(cacheRoot2, "local/0xABCDEF02.txd").isFile());
    }

    @Test
    public void scanDeleteFolderAndClearAll_workSafelyAndMigrateLooseRootFiles() throws IOException {
        File cacheRoot = tmp.newFolder("cache");
        File localDir = new File(cacheRoot, "local");
        File srvDir = new File(cacheRoot, "127.0.0.1.7777");
        File nestedManualDir = new File(cacheRoot, "manual_pack/Play.Server.COM.7777");
        File emptyDir = new File(cacheRoot, "empty.7777");
        assertTrue(localDir.mkdirs());
        assertTrue(srvDir.mkdirs());
        assertTrue(nestedManualDir.mkdirs());
        assertTrue(emptyDir.mkdirs());

        writeBytes(new File(cacheRoot, "0x4444.dff"), 50);
        writeBytes(new File(cacheRoot, ".stage_import_123.tmp"), 999); // stale stage file auto-cleaned
        writeBytes(new File(localDir, "0x1111.dff"), 100);
        writeBytes(new File(srvDir, "0x2222.dff"), 500);
        writeBytes(new File(srvDir, "0x3333.txd"), 400);
        writeBytes(new File(srvDir, "0x3333.txd.tmp"), 999);
        writeBytes(new File(nestedManualDir, "0x5555.dff"), 250);

        ArtworkCacheManager.CacheSummary sum = ArtworkCacheManager.scan(cacheRoot);
        assertFalse(new File(cacheRoot, ".stage_import_123.tmp").exists());
        assertEquals(5, sum.totalFiles);
        assertEquals(1300L, sum.totalBytes);
        assertEquals(3, sum.folders.size());
        assertEquals("local", sum.folders.get(0).name);
        assertTrue(sum.folders.get(0).isLocal);
        assertEquals(2, sum.folders.get(0).fileCount);
        assertEquals(150L, sum.folders.get(0).totalBytes);
        assertEquals("127.0.0.1.7777", sum.folders.get(1).name);
        assertEquals(2, sum.folders.get(1).fileCount);
        assertEquals(900L, sum.folders.get(1).totalBytes);
        assertEquals("manual_pack", sum.folders.get(2).name);
        assertEquals(1, sum.folders.get(2).fileCount);
        assertEquals(250L, sum.folders.get(2).totalBytes);

        assertFalse(ArtworkCacheManager.deleteFolder(cacheRoot, "../cache"));
        assertFalse(ArtworkCacheManager.deleteFolder(cacheRoot, "a/b"));

        assertTrue(ArtworkCacheManager.deleteFolder(cacheRoot, "127.0.0.1.7777"));
        assertFalse(srvDir.exists());
        assertTrue(localDir.exists());

        ArtworkCacheManager.CacheSummary afterDel = ArtworkCacheManager.scan(cacheRoot);
        assertEquals(3, afterDel.totalFiles);
        assertEquals(400L, afterDel.totalBytes);

        ArtworkCacheManager.clearAll(cacheRoot);
        ArtworkCacheManager.CacheSummary afterClear = ArtworkCacheManager.scan(cacheRoot);
        assertEquals(0, afterClear.totalFiles);
        assertEquals(0L, afterClear.totalBytes);
    }

    private static void putZipEntry(ZipOutputStream zos, String name, int len) throws IOException {
        zos.putNextEntry(new ZipEntry(name));
        zos.write(new byte[len]);
        zos.closeEntry();
    }

    private static void writeBytes(File f, int len) throws IOException {
        try (FileOutputStream fos = new FileOutputStream(f)) {
            fos.write(new byte[len]);
        }
    }
}

package com.sampmobile.launcher.utils;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

public class ServerJoinProfileStoreTest {

    @Rule
    public TemporaryFolder tmpDir = new TemporaryFolder();

    @Test
    public void perServerNicknamesAndFiveSlotHistory_persistIndependentlyAcrossReloads() throws Exception {
        File storeFile = new File(tmpDir.getRoot(), "SAMP/server_profiles.properties");
        ServerJoinProfileStore store = new ServerJoinProfileStore();

        String srvA = ServerJoinProfileStore.normalizeServerKey("Play.Alpha.Example", "7777");
        String srvB = ServerJoinProfileStore.normalizeServerKey("43.251.102.190", 7777);
        String srvNew = ServerJoinProfileStore.normalizeServerKey("203.0.113.9", "7788");

        assertEquals("play.alpha.example:7777", srvA);
        assertEquals("samp_player", store.resolveNicknameForServer(srvA, "samp_player"));
        assertTrue(store.getNicknameHistory("samp_player").isEmpty());

        // First join sets server A's nickname, adds to history, and upgrades default from samp_player
        store.recordJoin(srvA, "XiaoNiao", false, "");
        assertEquals("XiaoNiao", store.resolveNicknameForServer(srvA, "samp_player"));
        assertEquals("XiaoNiao", store.resolveNicknameForServer(srvNew, "samp_player"));

        // Second server uses a completely different RP name; server A keeps "XiaoNiao"
        store.recordJoin(srvB, "Arthur_Pendragon", true, "medieval#2026");
        assertEquals("XiaoNiao", store.resolveNicknameForServer(srvA, "Arthur_Pendragon"));
        assertEquals("Arthur_Pendragon", store.resolveNicknameForServer(srvB, "XiaoNiao"));
        assertEquals("XiaoNiao", store.resolveNicknameForServer(srvNew, "Arthur_Pendragon"));
        assertEquals(Arrays.asList("Arthur_Pendragon", "XiaoNiao"),
                store.getNicknameHistory("samp_player"));

        // Push 4 more names (total 6 distinct names) -> history caps strictly at 5 MRU items
        store.recordJoin("10.0.0.1:7777", "Carl_Johnson", false, "");
        store.recordJoin("10.0.0.2:7777", "Tommy_Vercetti", false, "");
        store.recordJoin("10.0.0.3:7777", "Claude_Speed", false, "");
        store.recordJoin("10.0.0.4:7777", "Niko_Bellic", false, "");

        List<String> expectedFive = Arrays.asList(
                "Niko_Bellic", "Claude_Speed", "Tommy_Vercetti", "Carl_Johnson", "Arthur_Pendragon");
        assertEquals(5, store.getNicknameHistory("XiaoNiao").size());
        assertEquals(expectedFive, store.getNicknameHistory("XiaoNiao"));

        // Re-using an existing name promotes it to #0 without duplicating
        store.recordJoin(srvA, "Tommy_Vercetti", false, "");
        assertEquals(Arrays.asList(
                "Tommy_Vercetti", "Niko_Bellic", "Claude_Speed", "Carl_Johnson", "Arthur_Pendragon"),
                store.getNicknameHistory("samp_player"));

        // Persist to disk and reload in a fresh instance
        store.saveToFile(storeFile);
        ServerJoinProfileStore reloaded = ServerJoinProfileStore.loadFromFile(storeFile);

        assertEquals("Tommy_Vercetti", reloaded.resolveNicknameForServer(srvA, "samp_player"));
        assertEquals("Arthur_Pendragon", reloaded.resolveNicknameForServer(srvB, "samp_player"));
        assertEquals("medieval#2026", reloaded.getSavedPassword(srvB));
        assertEquals("XiaoNiao", reloaded.getDefaultNickname("samp_player"));
        assertEquals(Arrays.asList(
                "Tommy_Vercetti", "Niko_Bellic", "Claude_Speed", "Carl_Johnson", "Arthur_Pendragon"),
                reloaded.getNicknameHistory("samp_player"));
    }

    @Test
    public void lockedServerPasswords_preserveSpecialCharsUpdateCleanlyAndSurviveTemporaryUnlock() throws Exception {
        File storeFile = new File(tmpDir.getRoot(), "SAMP/server_profiles.properties");
        ServerJoinProfileStore store = new ServerJoinProfileStore();

        String lockedKey = ServerJoinProfileStore.normalizeServerKey("198.51.100.42", 7779);
        String trickyPass = " p@ss=w:o\\rd!\"中文 ";

        store.recordJoin(lockedKey, "Tester_One", true, trickyPass);
        store.saveToFile(storeFile);

        ServerJoinProfileStore reloaded = ServerJoinProfileStore.loadFromFile(storeFile);
        assertEquals(trickyPass, reloaded.getSavedPassword(lockedKey));
        assertEquals("Tester_One", reloaded.resolveNicknameForServer(lockedKey, "samp_player"));

        // Joining while server is temporarily unlocked (updatePassword = false) preserves saved password
        reloaded.recordJoin(lockedKey, "Tester_Two", false, "");
        assertEquals(trickyPass, reloaded.getSavedPassword(lockedKey));
        assertEquals("Tester_Two", reloaded.resolveNicknameForServer(lockedKey, "samp_player"));

        // Explicitly clearing password when password box is shown (updatePassword = true) clears it
        reloaded.recordJoin(lockedKey, "Tester_Two", true, "");
        assertEquals("", reloaded.getSavedPassword(lockedKey));
    }

    @Test
    public void legacySeeding_migratesLastServerNicknameAndPasswordWithoutPollutingPlaceholder() {
        ServerJoinProfileStore freshPlaceholder = new ServerJoinProfileStore();
        freshPlaceholder.seedFromLegacySettings("127.0.0.1", 7777, "", "samp_player");
        assertTrue(freshPlaceholder.getNicknameHistory("samp_player").isEmpty());
        assertEquals("", freshPlaceholder.getSavedPassword("127.0.0.1:7777"));

        ServerJoinProfileStore upgraded = new ServerJoinProfileStore();
        upgraded.seedFromLegacySettings("43.251.102.190", 7777, "lock123", "Legacy_Hero");
        assertEquals("Legacy_Hero", upgraded.resolveNicknameForServer("43.251.102.190:7777", "samp_player"));
        assertEquals("lock123", upgraded.getSavedPassword("43.251.102.190:7777"));
        assertEquals(Collections.singletonList("Legacy_Hero"), upgraded.getNicknameHistory("samp_player"));
    }
}

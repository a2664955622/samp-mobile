package com.sampmobile.launcher.utils;

import org.junit.Test;

import static org.junit.Assert.assertEquals;

public class LauncherBackdropManagerTest {

    @Test
    public void clampDimPercentRespectsBounds() {
        assertEquals(0, LauncherBackdropManager.clampDimPercent(-20));
        assertEquals(0, LauncherBackdropManager.clampDimPercent(0));
        assertEquals(35, LauncherBackdropManager.clampDimPercent(35));
        assertEquals(85, LauncherBackdropManager.clampDimPercent(85));
        assertEquals(85, LauncherBackdropManager.clampDimPercent(100));
    }

    @Test
    public void computeSampleSizeHalvesLargeImagesSafely() {
        assertEquals(1, LauncherBackdropManager.computeSampleSize(1920, 1080, 2560));
        assertEquals(1, LauncherBackdropManager.computeSampleSize(2560, 1440, 2560));
        assertEquals(1, LauncherBackdropManager.computeSampleSize(4000, 3000, 2560));
        assertEquals(2, LauncherBackdropManager.computeSampleSize(5120, 2880, 2560));
        assertEquals(2, LauncherBackdropManager.computeSampleSize(8000, 6000, 2560));
        assertEquals(4, LauncherBackdropManager.computeSampleSize(12000, 9000, 2560));
        assertEquals(1, LauncherBackdropManager.computeSampleSize(0, 0, 2560));
    }
}

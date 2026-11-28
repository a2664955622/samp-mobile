package com.sampmobile.launcher.utils;

import org.junit.Test;

import java.io.File;
import java.io.FileInputStream;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

public class ServerDirectoryTest {

    private static GeoIpDatabase.Table loadRealGeoTable() throws Exception {
        File raw = new File("src/main/res/raw/ipv4_country.bin");
        if (!raw.isFile()) {
            raw = new File("app/src/main/res/raw/ipv4_country.bin");
        }
        assertTrue("Missing res/raw/ipv4_country.bin", raw.isFile());
        try (FileInputStream fis = new FileInputStream(raw)) {
            return GeoIpDatabase.loadFromGzipStream(fis);
        }
    }

    @Test
    public void geoIpDatabase_resolvesGlobalAndGreaterChinaCloudServers() throws Exception {
        GeoIpDatabase.Table table = loadRealGeoTable();
        assertTrue(table.size() > 200_000);

        assertEquals("CN", table.lookupHost("111.229.161.230", false)); // Shanghai Tencent Cloud
        assertEquals("CN", table.lookupHost("110.42.109.138", false));  // China mainland
        assertEquals("HK", table.lookupHost("40.83.121.170", false));   // Azure Hong Kong
        assertEquals("TW", table.lookupHost("220.135.101.62", false));  // Taiwan Chunghwa Telecom
        assertEquals("DE", table.lookupHost("141.95.95.184", false));   // OVH Germany
        assertEquals("US", table.lookupHost("8.8.8.8", false));         // Google US

        // Loopback & RFC1918 private networks never report a false country
        assertEquals("", table.lookupHost("127.0.0.1", false));
        assertEquals("", table.lookupHost("192.168.1.50", false));
        assertEquals("", table.lookupHost("10.0.0.5", false));
        assertEquals("", table.lookupHost("not-an-ip.example.org", false));
    }

    @Test
    public void mojibakeRecovery_repairsCp1251AndCp1252ChineseWithoutCorruptingRussianOrEuropean() {
        Charset gbk = Charset.forName("GBK");
        Charset cp1251 = Charset.forName("windows-1251");
        Charset cp1252 = Charset.forName("windows-1252");

        // 1. GBK bytes misdecoded as CP1251 (real SAMonitor pattern: "现代都市 www.mcitygame.com")
        String mcityGarbled = new String("现代都市 www.mcitygame.com".getBytes(gbk), cp1251);
        assertEquals("现代都市 www.mcitygame.com", ServerFilterEngine.recoverMojibake(mcityGarbled, false));

        String zhLangGarbled = new String("中文|Chinese".getBytes(gbk), cp1251);
        assertEquals("中文|Chinese", ServerFilterEngine.recoverMojibake(zhLangGarbled, true));

        // 2. Mixed CP1251 + Latin-1 scraper output ("TSRP 测试服 中文|用TypeScript编写的RP服务器")
        String mixedSam = "TSRP ІвКФ·ю ЦРОД|ÓГTypeScript±аРґµДRP·юОñЖч";
        assertEquals("TSRP 测试服 中文|用TypeScript编写的RP服务器",
                ServerFilterEngine.recoverMojibake(mixedSam, true));

        // 3. UTF-8 bytes misdecoded as CP1251, ISO-8859-1, or CP1252 (where 0x8D in '服'=E6 9C 8D is an undefined hole -> \uFFFD)
        String utf8Via1251 = new String("SA-MP 0.3 無人值守伺服器 (open.mp)".getBytes(StandardCharsets.UTF_8), cp1251);
        assertEquals("SA-MP 0.3 無人值守伺服器 (open.mp)", ServerFilterEngine.recoverMojibake(utf8Via1251, false));

        String utf8ViaLatin1 = new String("SA-MP 0.3 無人值守伺服器 (open.mp)".getBytes(StandardCharsets.UTF_8), StandardCharsets.ISO_8859_1);
        assertEquals("SA-MP 0.3 無人值守伺服器 (open.mp)", ServerFilterEngine.recoverMojibake(utf8ViaLatin1, false));

        String utf8Via1252 = new String("SA-MP 0.3 無人值守伺服器 (open.mp)".getBytes(StandardCharsets.UTF_8), cp1252);
        assertEquals("SA-MP 0.3 無人值守伺\uFFFD器 (open.mp)", ServerFilterEngine.recoverMojibake(utf8Via1252, false));

        // 4. Pure Latin-1 GBK mojibake ("¡ïStarFuture¡ïÐÇ¿ÕÎ´À´ | ×°°çÏµÍ³[037|DL]") recovers even without hint
        assertEquals("★StarFuture★星空未来 | 装扮系统[037|DL]",
                ServerFilterEngine.recoverMojibake("¡ïStarFuture¡ïÐÇ¿ÕÎ´À´ | ×°°çÏµÍ³[037|DL]", false));

        // 5. Russian CP1251 misdecoded as Latin-1 ("Russian/Ðóññêèé" on 185.130.83.87:7892) recovers to Cyrillic, NOT fake GBK!
        String swipeLang = ServerFilterEngine.recoverMojibake("Russian/Ðóññêèé", false, "Russian/Ðóññêèé");
        assertEquals("Russian/Русский", swipeLang);
        assertFalse(ServerFilterEngine.containsHan(swipeLang));

        // 6. Genuine Russian, Ukrainian, Portuguese, Spanish, German, Polish must stay untouched
        String[] genuine = {
                "Nayalsk RolePlay | Jeden | x3 ДЕНЬ",
                "Русский Дрифт Сервер | СНГ",
                "Україна ГТА | Київ",
                "Português - Brasil | Vida Real",
                "FZ Roleplay [S1] Rol en español",
                "Español - Comunidad Latina",
                "German Reallife | Österreich",
                "[PPS] Polski Power Server"
        };
        for (String orig : genuine) {
            assertEquals(orig, ServerFilterEngine.recoverMojibake(orig, false));
        }
    }

    @Test
    public void smartUdpDecodeAndCnFilter_rejectSpanishRussianAndIndonesianFalsePositives() {
        Charset cp1251 = Charset.forName("windows-1251");
        Charset gbk = Charset.forName("GBK");

        // FZ Roleplay (51.79.37.125 / 66.70.203.215 / 66.70.220.66): ISO-8859-1 "Rol en español"
        // Previously GBK turned 0xF1 0x6F ("ño") into fake Chinese '駉', polluting CN Regional!
        byte[] fzHostBytes = "FZ Roleplay [S1] Rol en español".getBytes(StandardCharsets.ISO_8859_1);
        byte[] fzLangBytes = "Español - Spanish".getBytes(StandardCharsets.ISO_8859_1);
        String fzHost = SampQuery.decodeSmart(fzHostBytes, "GBK", null);
        String fzLang = SampQuery.decodeSmart(fzLangBytes, "GBK", null);
        assertEquals("FZ Roleplay [S1] Rol en español", fzHost);
        assertEquals("Español - Spanish", fzLang);
        assertFalse(ServerFilterEngine.containsHan(fzHost));

        ServerModel fzServer = new ServerModel(fzHost, "51.79.37.125", "7777", "0", fzHost, "897", "899", fzLang);
        fzServer.server_country = "CA";
        assertFalse(ServerFilterEngine.matchesCountry(fzServer, "CN"));
        assertTrue(ServerFilterEngine.matchesCountry(fzServer, "CA"));
        assertTrue(ServerFilterEngine.matchesLanguage(fzServer, "es"));

        // Swipe RP (185.130.83.87:7892): CP1251 "Russian/Русский"
        byte[] swipeLangBytes = "Russian/Русский".getBytes(cp1251);
        String decodedSwipe = SampQuery.decodeSmart(swipeLangBytes, "GBK", null);
        assertEquals("Russian/Русский", decodedSwipe);
        assertFalse(ServerFilterEngine.containsHan(decodedSwipe));

        ServerModel swipeServer = new ServerModel("Swipe RP", "185.130.83.87", "7892", "0", "Swipe RP", "750", "1000", decodedSwipe);
        swipeServer.server_country = "RU";
        assertFalse(ServerFilterEngine.matchesCountry(swipeServer, "CN"));
        assertTrue(ServerFilterEngine.matchesLanguage(swipeServer, "ru"));

        // Indonesian server hosted on HK VPS (141.11.78.40:7777) must NOT leak into CN Regional
        ServerModel hkIndo = new ServerModel("Lorvia Roleplay", "141.11.78.40", "7777", "0", "LRP", "10", "100", "Bahasa Indonesia");
        hkIndo.server_country = "HK";
        assertFalse(ServerFilterEngine.matchesCountry(hkIndo, "CN"));
        assertTrue(ServerFilterEngine.matchesLanguage(hkIndo, "id"));

        // Real GBK Chinese UDP response still decodes as clean GBK Chinese even without language hint
        byte[] cnUdpBytes = "现代都市 www.mcitygame.com".getBytes(gbk);
        assertEquals("现代都市 www.mcitygame.com", SampQuery.decodeSmart(cnUdpBytes, "GBK", "-"));
    }

    @Test
    public void countryAndLanguageFilters_decoupleHostingLocationFromCommunityLanguageAndAvoidMisses() {
        // Case A: US-hosted Chinese community server (overseas DDoS IP, Chinese script & language "-")
        ServerModel usHostedChinese = new ServerModel(
                "洛圣都自由城 QQ群 123456", "208.67.222.222", "7777", "0", "RPG", "35", "100", "-");
        usHostedChinese.server_country = "US";

        // Matches BOTH US (physical GeoIP) and CN ("国服" anti-miss OR rule) in Regional tab
        assertTrue(ServerFilterEngine.matchesCountry(usHostedChinese, "US"));
        assertTrue(ServerFilterEngine.matchesCountry(usHostedChinese, "CN"));
        assertFalse(ServerFilterEngine.matchesCountry(usHostedChinese, "DE"));
        // In Internet language filter, matches Chinese ("zh"), NOT English ("en")
        assertTrue(ServerFilterEngine.matchesLanguage(usHostedChinese, "zh"));
        assertFalse(ServerFilterEngine.matchesLanguage(usHostedChinese, "en"));

        // Case B: German-hosted English community server (e.g. OVH/Hetzner DE, language "EN")
        ServerModel deHostedEnglish = new ServerModel(
                "SOLS Roleplay | Tuning Update", "141.95.95.184", "7777", "0", "SOLS v3.4", "50", "200", "EN");
        deHostedEnglish.server_country = "DE";

        assertTrue(ServerFilterEngine.matchesCountry(deHostedEnglish, "DE"));
        assertFalse(ServerFilterEngine.matchesCountry(deHostedEnglish, "US"));
        assertTrue(ServerFilterEngine.matchesLanguage(deHostedEnglish, "en"));
        assertFalse(ServerFilterEngine.matchesLanguage(deHostedEnglish, "de"));

        // Case C: HK VPS server WITHOUT Chinese signal (e.g. 141.11.78.33:7011 "Velloria Mapping") must NOT leak into CN
        ServerModel hkMappingServer = new ServerModel(
                "Velloria Mapping", "141.11.78.33", "7011", "0", "Mapping", "0", "50", "-");
        hkMappingServer.server_country = "HK";
        assertFalse(ServerFilterEngine.matchesCountry(hkMappingServer, "CN"));

        // Case D: Foreign servers using .hk domains (132.243.17.171 blast.hk, 31.56.25.232 & 31.56.81.178 forum.hk) must NOT leak into CN or zh
        ServerModel blastRp = new ServerModel("BLAST RP", "132.243.17.171", "7777", "1", "Blast RP", "1", "100", "EN");
        blastRp.server_country = "RU";
        blastRp.server_website = "blast.hk";
        assertFalse(ServerFilterEngine.matchesCountry(blastRp, "CN"));
        assertFalse(ServerFilterEngine.matchesLanguage(blastRp, "zh"));
        assertTrue(ServerFilterEngine.matchesCountry(blastRp, "RU"));

        ServerModel lostRp = new ServerModel("Lost RolePlay | Season 5", "31.56.81.178", "7777", "0", "Lost:RP", "10", "300", "Arabic/English");
        lostRp.server_country = "NL";
        lostRp.server_website = "https://lostrp.forum.hk";
        assertFalse(ServerFilterEngine.matchesCountry(lostRp, "CN"));
        assertFalse(ServerFilterEngine.matchesLanguage(lostRp, "zh"));
        assertTrue(ServerFilterEngine.matchesCountry(lostRp, "NL"));
        assertTrue(ServerFilterEngine.matchesLanguage(lostRp, "ar"));

        // Case E: Korean EUC-KR server (125.187.179.182:7777 "Àç°ÇÀÇ ¼Õ", la="Korean") must recover to Hangul & stay out of CN/zh
        String krRecovered = ServerFilterEngine.recoverMojibake("Àç°ÇÀÇ ¼Õ", false, "Korean");
        assertEquals("재건의 손", krRecovered);
        ServerModel krServer = new ServerModel(krRecovered, "125.187.179.182", "7777", "0", "Manus Vindictae", "1", "28", "Korean");
        krServer.server_country = "KR";
        assertFalse(ServerFilterEngine.matchesCountry(krServer, "CN"));
        assertFalse(ServerFilterEngine.matchesLanguage(krServer, "zh"));
        assertTrue(ServerFilterEngine.matchesCountry(krServer, "KR"));

        // Case F: Corrupted dead Hangul/Arabic zombie entries on CN IPs (117.72.222.148, 211.154.28.15) rejected until UDP cleans them
        ServerModel hangulZombie = new ServerModel("課랑薑적실\uFFFD곁蘿", "117.72.222.148", "7777", "0", "-", "0", "50", "\uFFFD\uFFFD");
        hangulZombie.server_country = "CN";
        assertFalse(ServerFilterEngine.matchesCountry(hangulZombie, "CN"));

        ServerModel arabicZombie = new ServerModel("\uFFFDدسر\uFFFDاة\uFFFDهر", "211.154.28.15", "31818", "0", "-", "0", "50", "\uFFFDْجمضذخؤ");
        arabicZombie.server_country = "CN";
        assertFalse(ServerFilterEngine.matchesCountry(arabicZombie, "CN"));

        // Case G: Both Taiwan (220.135.101.62:7778) and Japan-hosted Traditional Chinese (162.222.17.43:7777)
        // AND "中国服China" (119.138.226.148:7777) are included in CN Regional & zh Internet!
        ServerModel jpTradServer = new ServerModel("TFM GF Server [0.3DL]", "162.222.17.43", "7777", "0", "TDGF Gen14 OMP", "0", "100", "Traditional Chinese/中文");
        jpTradServer.server_country = "JP";
        assertTrue(ServerFilterEngine.matchesCountry(jpTradServer, "CN"));
        assertTrue(ServerFilterEngine.matchesCountry(jpTradServer, "JP"));
        assertTrue(ServerFilterEngine.matchesLanguage(jpTradServer, "zh"));

        ServerModel twServer = new ServerModel("SA-MP 0.3 無人值守伺服器 (open.mp)", "220.135.101.62", "7778", "0", "2.7.1.9", "0", "55", "Chinese");
        twServer.server_country = "TW";
        assertTrue(ServerFilterEngine.matchesCountry(twServer, "CN"));
        assertTrue(ServerFilterEngine.matchesLanguage(twServer, "zh"));

        ServerModel newFreedom = new ServerModel("[GTA科技]-New Freedom Land新自由城★", "119.138.226.148", "7777", "0", "新自由城V2.1.5", "1", "100", "中国服China");
        newFreedom.server_country = "CN";
        assertTrue(ServerFilterEngine.matchesCountry(newFreedom, "CN"));
        assertTrue(ServerFilterEngine.matchesLanguage(newFreedom, "zh"));

        // Case H: Precomputed O(1) bitmasks match runtime predicates 1:1
        ServerFilterEngine.updateMasks(usHostedChinese);
        int cnMask = ServerFilterEngine.buildCountryMaskForSet(Collections.singleton("CN"));
        int zhMask = ServerFilterEngine.buildLanguageMaskForSet(Collections.singleton("zh"));
        assertTrue((usHostedChinese.country_mask & cnMask) != 0);
        assertTrue((usHostedChinese.lang_mask & zhMask) != 0);

        // Case I: Russian server with empty language field caught by Cyrillic script in hostname
        ServerModel ruBlankLang = new ServerModel(
                "Nayalsk RolePlay | x3 ДЕНЬ", "51.75.44.8", "7777", "0", "RP", "10", "100", "-");
        assertTrue(ServerFilterEngine.matchesLanguage(ruBlankLang, "ru"));
        // Short code "it" must not match lowercase English sentence "Try it out"
        ServerModel enSentence = new ServerModel("Try it out Drift", "1.1.1.1", "7777", "0", "Drift", "1", "50", "try it now");
        assertFalse(ServerFilterEngine.matchesLanguage(enSentence, "it"));
    }

    @Test
    public void deviceDetection_resolvesInitialRegionalCountryAcrossPhonesAndEmulators() {
        assertEquals(18, ServerFilterEngine.LANGUAGES.size());

        // Chinese locale -> CN
        assertEquals("CN", ServerFilterEngine.detectDefaultCountry(
                "", "", Collections.singletonList(Locale.SIMPLIFIED_CHINESE), "UTC"));
        // Traditional Chinese (TW/HK) -> grouped into CN regional tab
        assertEquals("CN", ServerFilterEngine.detectDefaultCountry(
                "tw", "", Collections.singletonList(Locale.TRADITIONAL_CHINESE), "Asia/Taipei"));
        // SIM-less Chinese Android emulator (en_US locale + Asia/Shanghai timezone) -> CN
        assertEquals("CN", ServerFilterEngine.detectDefaultCountry(
                "", "", Collections.singletonList(Locale.US), "Asia/Shanghai"));
        // Real US device (en_US + America/New_York) -> US
        assertEquals("US", ServerFilterEngine.detectDefaultCountry(
                "us", "us", Collections.singletonList(Locale.US), "America/New_York"));
        // Russian, Brazilian, German, Indonesian ("in" legacy Java code) locales
        assertEquals("RU", ServerFilterEngine.detectDefaultCountry(
                "", "", Collections.singletonList(new Locale("ru", "RU")), "Europe/Moscow"));
        assertEquals("BR", ServerFilterEngine.detectDefaultCountry(
                "", "", Collections.singletonList(new Locale("pt", "BR")), "America/Sao_Paulo"));
        assertEquals("DE", ServerFilterEngine.detectDefaultCountry(
                "", "", Collections.singletonList(Locale.GERMANY), "Europe/Berlin"));
        assertEquals("ID", ServerFilterEngine.detectDefaultCountry(
                "", "", Collections.singletonList(new Locale("in", "ID")), "Asia/Jakarta"));
    }

    @Test
    public void aggregator_deduplicatesAndPrefersLosslessSamTextOverOpenMpReplacementChars() throws Exception {
        GeoIpDatabase.Table geo = loadRealGeoTable();

        String ompJson = "[{" +
                "\"core\":{\"ip\":\"43.249.193.238:7778\",\"hn\":\"TSRP \uFFFD\uFFFD\uFFFD\",\"pc\":8,\"pm\":50,\"gm\":\"RP\",\"la\":\"Chinese\",\"pa\":false,\"vn\":\"omp 1.6\",\"omp\":true}," +
                "\"ru\":{\"weburl\":\"bilibili.com\",\"mapname\":\"LS\"}" +
                "}]";
        String samJson = "[{" +
                "\"success\":true,\"ipAddr\":\"43.249.193.238:7778\"," +
                "\"name\":\"TSRP ІвКФ·ю ЦРОД|ÓГTypeScript±аРґµДRP·юОñЖч\"," +
                "\"gameMode\":\"botTest\",\"language\":\"Chinese\",\"playersOnline\":8,\"maxPlayers\":50," +
                "\"requiresPassword\":false,\"isOpenMp\":true,\"website\":\"Unknown\"" +
                "},{" +
                "\"success\":true,\"ipAddr\":\"40.83.121.170:7777\"," +
                "\"name\":\"ПЦґъ¶јКР www.mcitygame.com\",\"gameMode\":\"RPG\",\"language\":\"ЦРОД|Chinese\"," +
                "\"playersOnline\":21,\"maxPlayers\":300,\"requiresPassword\":false,\"isOpenMp\":false,\"website\":\"www.mcitygame.com\"" +
                "},{" +
                "\"success\":false,\"ipAddr\":\"9.9.9.9:7777\",\"name\":\"Offline Dead Server\",\"playersOnline\":0,\"maxPlayers\":50" +
                "}]";
        String favJson = "{\"servers\":[{\"host\":\"127.0.0.1\",\"port\":\"7777\"},{\"host\":\"111.229.161.230\",\"port\":\"7777\"}]}";

        List<ServerModel> merged = ServerListFetcher.aggregateFromJson(
                ompJson, samJson, null, null, favJson, null, geo);

        // 2 online public servers + 1 unverified public favorite candidate (127.0.0.1 & offline SAM row excluded)
        assertEquals(3, merged.size());

        // Sorted by online players descending -> 40.83.121.170:7777 (21 players) first!
        ServerModel first = merged.get(0);
        assertEquals("40.83.121.170:7777", first.address());
        assertEquals("现代都市 www.mcitygame.com", first.server_name);
        assertEquals("中文|Chinese", first.server_language);
        assertEquals("HK", first.server_country);
        assertFalse(first.unverified_candidate);

        // Second: merged open.mp + SAMonitor entry recovered clean Chinese name & kept open.mp rules weburl
        ServerModel second = merged.get(1);
        assertEquals("43.249.193.238:7778", second.address());
        assertEquals("TSRP 测试服 中文|用TypeScript编写的RP服务器", second.server_name);
        assertEquals("bilibili.com", second.server_website);
        assertEquals("CN", second.server_country);
        assertFalse(second.needs_encoding_refresh);

        // Third: unverified favorite candidate stays flagged until UDP confirms online
        ServerModel third = merged.get(2);
        assertEquals("111.229.161.230:7777", third.address());
        assertTrue(third.unverified_candidate);

        Set<String> cnSet = new HashSet<>(Arrays.asList("CN"));
        assertTrue(ServerFilterEngine.matchesAnyCountry(first, cnSet));
        assertTrue(ServerFilterEngine.matchesAnyCountry(second, cnSet));
    }
}

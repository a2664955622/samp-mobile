package com.sampmobile.launcher.utils;

import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.Charset;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.TimeZone;

/**
 * Compact table-driven server filtering, device region/language resolution, and mojibake recovery:
 * <ul>
 *   <li><b>Regional ("国服")</b>: Region-based filtering (offline IPv4 GeoIP + full Chinese community
 *       coverage for {@code CN}, keeping the region switcher for cross-region devices).</li>
 *   <li><b>Internet</b>: Language-based filtering across the 18 official {@code open.mp/servers}
 *       languages via {@code language} column keywords + Unicode script fallback.</li>
 * </ul>
 */
public final class ServerFilterEngine {

    private static final int SCRIPT_NONE = 0;
    private static final int SCRIPT_HAN = 1;
    private static final int SCRIPT_CYRILLIC = 2;
    private static final int SCRIPT_UKRAINIAN = 3;
    private static final int SCRIPT_ARABIC = 4;
    private static final int SCRIPT_VIETNAMESE = 5;

    public static final class FilterOption {
        public final String id;
        public final String zhName;
        public final String enName;
        public final String nativeLabel;
        final int scriptKind;
        final String[] keywords;

        FilterOption(String id, String zhName, String enName, String nativeLabel) {
            this(id, zhName, enName, nativeLabel, SCRIPT_NONE);
        }

        FilterOption(String id, String zhName, String enName, String nativeLabel, int scriptKind, String... keywords) {
            this.id = id;
            this.zhName = zhName;
            this.enName = enName;
            this.nativeLabel = nativeLabel;
            this.scriptKind = scriptKind;
            this.keywords = keywords;
        }

        public String chipLabel(boolean englishUi) {
            return englishUi ? enName : zhName;
        }

        public String dialogLabel(boolean englishUi) {
            String primary = englishUi ? enName : zhName;
            if (nativeLabel == null || nativeLabel.isEmpty() || primary.equalsIgnoreCase(nativeLabel)) {
                return primary;
            }
            return primary + " (" + nativeLabel + ")";
        }
    }

    /** Official 18 languages from https://open.mp/servers (table-driven). */
    public static final List<FilterOption> LANGUAGES = Collections.unmodifiableList(Arrays.asList(
            new FilterOption("zh", "中文", "Chinese", "中文", SCRIPT_HAN,
                    "chinese", "china", "mandarin", "cantonese", "taiwan", "zhcn", "zhtw", "zhhk", "zh", "cn", "chn", "tw", "中文", "中国", "汉语", "繁體"),
            new FilterOption("en", "英语", "English", "English", SCRIPT_NONE,
                    "english", "international", "global", "en", "eng", "us", "usa", "uk"),
            new FilterOption("ru", "俄语", "Russian", "Русский", SCRIPT_CYRILLIC,
                    "russian", "russia", "русский", "рус", "россия", "crmp", "ru", "rus"),
            new FilterOption("pt", "葡萄牙语", "Portuguese", "Português", SCRIPT_NONE,
                    "portugu", "brasil", "brazil", "pt-br", "ptbr", "pt", "br", "por"),
            new FilterOption("es", "西班牙语", "Spanish", "Español", SCRIPT_NONE,
                    "español", "espanol", "spanish", "hispano", "latino", "latam", "es", "esp"),
            new FilterOption("id", "印尼语", "Indonesian", "Bahasa Indonesia", SCRIPT_NONE,
                    "indonesia", "bahasa", "indo", "id", "idn"),
            new FilterOption("de", "德语", "German", "Deutsch", SCRIPT_NONE,
                    "german", "deutsch", "de", "ger", "deu"),
            new FilterOption("vi", "越南语", "Vietnamese", "Tiếng Việt", SCRIPT_VIETNAMESE,
                    "vietnam", "viet nam", "tiếng việt", "tieng viet", "vi", "vn"),
            new FilterOption("tr", "土耳其语", "Turkish", "Türkçe", SCRIPT_NONE,
                    "turkish", "türk", "turk", "türkiye", "tr"),
            new FilterOption("ar", "阿拉伯语", "Arabic", "العربية", SCRIPT_ARABIC,
                    "arabic", "arab", "العربية", "عربي", "ar", "ksa"),
            new FilterOption("ro", "罗马尼亚语", "Romanian", "Română", SCRIPT_NONE,
                    "romanian", "română", "romana", "romania", "moldova", "ro", "rom"),
            new FilterOption("pl", "波兰语", "Polish", "Polski", SCRIPT_NONE,
                    "polish", "polski", "polska", "poland", "pl", "pol"),
            new FilterOption("uk", "乌克兰语", "Ukrainian", "Українська", SCRIPT_UKRAINIAN,
                    "ukrain", "україн", "украин", "uk", "ua", "ukr"),
            new FilterOption("fr", "法语", "French", "Français", SCRIPT_NONE,
                    "french", "français", "francais", "france", "fr", "fra"),
            new FilterOption("it", "意大利语", "Italian", "Italiano", SCRIPT_NONE,
                    "italian", "italiano", "italia", "it", "ita"),
            new FilterOption("cs", "捷克语", "Czech", "Čeština", SCRIPT_NONE,
                    "czech", "čeština", "cestina", "slovak", "cz/sk", "cs", "cz", "sk"),
            new FilterOption("hu", "匈牙利语", "Hungarian", "Magyar", SCRIPT_NONE,
                    "hungarian", "magyar", "hungary", "hu", "hun"),
            new FilterOption("nl", "荷兰语", "Dutch", "Nederlands", SCRIPT_NONE,
                    "dutch", "nederland", "holland", "nl", "nld")
    ));

    /** Curated 24 regions/countries for the Regional ("国服") tab. */
    public static final List<FilterOption> COUNTRIES = Collections.unmodifiableList(Arrays.asList(
            new FilterOption("CN", "中国", "China", "中国"),
            new FilterOption("US", "美国", "United States", "USA"),
            new FilterOption("RU", "俄罗斯", "Russia", "Россия"),
            new FilterOption("DE", "德国", "Germany", "Deutschland"),
            new FilterOption("BR", "巴西", "Brazil", "Brasil"),
            new FilterOption("FR", "法国", "France", "France"),
            new FilterOption("GB", "英国", "United Kingdom", "UK"),
            new FilterOption("ID", "印尼", "Indonesia", "Indonesia"),
            new FilterOption("SG", "新加坡", "Singapore", "Singapore"),
            new FilterOption("PL", "波兰", "Poland", "Polska"),
            new FilterOption("UA", "乌克兰", "Ukraine", "Україна"),
            new FilterOption("RO", "罗马尼亚", "Romania", "România"),
            new FilterOption("TR", "土耳其", "Türkiye", "Türkiye"),
            new FilterOption("VN", "越南", "Vietnam", "Việt Nam"),
            new FilterOption("CA", "加拿大", "Canada", "Canada"),
            new FilterOption("NL", "荷兰", "Netherlands", "Nederland"),
            new FilterOption("FI", "芬兰", "Finland", "Suomi"),
            new FilterOption("ES", "西班牙", "Spain", "España"),
            new FilterOption("IT", "意大利", "Italy", "Italia"),
            new FilterOption("AU", "澳大利亚", "Australia", "Australia"),
            new FilterOption("JP", "日本", "Japan", "日本"),
            new FilterOption("KR", "韩国", "South Korea", "한국"),
            new FilterOption("IN", "印度", "India", "India"),
            new FilterOption("AR", "阿根廷", "Argentina", "Argentina")
    ));

    private static final String CP1251_80_BF =
            "\u0402\u0403\u201A\u0453\u201E\u2026\u2020\u2021\u20AC\u2030\u0409\u2039\u040A\u040C\u040B\u040F" +
            "\u0452\u2018\u2019\u201C\u201D\u2022\u2013\u2014\u0098\u2122\u0459\u203A\u045A\u045C\u045B\u045F" +
            "\u00A0\u040E\u045E\u0408\u00A4\u0490\u00A6\u00A7\u0401\u00A9\u0404\u00AB\u00AC\u00AD\u00AE\u0407" +
            "\u00B0\u00B1\u0406\u0456\u0491\u00B5\u00B6\u00B7\u0451\u2116\u0454\u00BB\u0458\u0405\u0455\u0457";

    private static final String CP1252_80_9F =
            "\u20AC\u0081\u201A\u0192\u201E\u2026\u2020\u2021\u02C6\u2030\u0160\u2039\u0152\u008D\u017D\u008F" +
            "\u0090\u2018\u2019\u201C\u201D\u2022\u2013\u2014\u02DC\u2122\u0161\u203A\u0153\u009D\u017E\u0178";

    private static final String MOJIBAKE_SYMBOLS = "¶°±·µ¬®©¤¦§¨ª¯²³¹º¼½¾¡¿×÷\u0458\u0405\u0455";

    private static final Charset GBK_CS = charsetOrNull("GBK");
    private static final Charset BIG5_CS = charsetOrNull("Big5");
    private static final Charset CP1251_CS = charsetOrNull("windows-1251");
    private static final Charset EUC_KR_CS = charsetOrNull("EUC-KR");

    private ServerFilterEngine() {}

    static Charset charsetOrNull(String name) {
        try { return Charset.forName(name); } catch (Exception e) { return null; }
    }

    public static int countryIndex(String id) {
        if (id == null) return -1;
        String t = id.trim();
        for (int i = 0; i < COUNTRIES.size(); i++) {
            if (COUNTRIES.get(i).id.equalsIgnoreCase(t)) return i;
        }
        return -1;
    }

    public static int languageIndex(String id) {
        if (id == null) return -1;
        String t = id.trim();
        for (int i = 0; i < LANGUAGES.size(); i++) {
            if (LANGUAGES.get(i).id.equalsIgnoreCase(t)) return i;
        }
        return -1;
    }

    public static FilterOption findLanguage(String id) {
        int idx = languageIndex(id);
        return idx >= 0 ? LANGUAGES.get(idx) : null;
    }

    public static FilterOption findCountry(String id) {
        if (id == null) return null;
        int idx = countryIndex(normalizeGreaterChina(id.trim().toUpperCase(Locale.US)));
        return idx >= 0 ? COUNTRIES.get(idx) : null;
    }

    public static String normalizeGreaterChina(String cc) {
        return ("HK".equals(cc) || "MO".equals(cc) || "TW".equals(cc)) ? "CN" : cc;
    }

    public static int buildCountryMaskForSet(Set<String> ids) {
        int mask = 0;
        if (ids != null) {
            for (String id : ids) {
                int idx = countryIndex(id);
                if (idx >= 0) mask |= (1 << idx);
            }
        }
        return mask;
    }

    public static int buildLanguageMaskForSet(Set<String> ids) {
        int mask = 0;
        if (ids != null) {
            for (String id : ids) {
                int idx = languageIndex(id);
                if (idx >= 0) mask |= (1 << idx);
            }
        }
        return mask;
    }

    public static void updateMasks(ServerModel s) {
        if (s == null) return;
        int cMask = 0;
        for (int i = 0; i < COUNTRIES.size(); i++) {
            if (matchesCountry(s, COUNTRIES.get(i).id)) cMask |= (1 << i);
        }
        int lMask = 0;
        for (int i = 0; i < LANGUAGES.size(); i++) {
            if (matchesLanguageOption(s, LANGUAGES.get(i))) lMask |= (1 << i);
        }
        s.country_mask = cMask;
        s.lang_mask = lMask;
    }

    public static List<FilterOption> orderedOptions(List<FilterOption> base, String preferredId) {
        if (preferredId == null || preferredId.isEmpty()) return base;
        List<FilterOption> out = new ArrayList<>(base.size());
        for (FilterOption o : base) if (o.id.equalsIgnoreCase(preferredId)) out.add(o);
        for (FilterOption o : base) if (!o.id.equalsIgnoreCase(preferredId)) out.add(o);
        return out;
    }

    public static Set<String> parseCsvIds(String csv, List<FilterOption> allowed, boolean uppercase) {
        Set<String> out = new LinkedHashSet<>();
        if (csv == null || csv.trim().isEmpty() || "auto".equalsIgnoreCase(csv.trim())) return out;
        for (String p : csv.split(",")) {
            String tok = p.trim();
            if (tok.isEmpty()) continue;
            tok = uppercase ? normalizeGreaterChina(tok.toUpperCase(Locale.US)) : tok.toLowerCase(Locale.US);
            for (FilterOption o : allowed) {
                if (o.id.equalsIgnoreCase(tok)) { out.add(o.id); break; }
            }
        }
        return out;
    }

    public static String joinIds(Set<String> ids) {
        return (ids == null || ids.isEmpty()) ? "" : String.join(",", ids);
    }

    // ------------------------------------------------------------------ Device environment detection

    public static String detectDefaultCountry(String simIso, String netIso, List<Locale> locales, String tzId) {
        String s = matchCountryIso(simIso);
        if (!s.isEmpty()) return s;
        s = matchCountryIso(netIso);
        if (!s.isEmpty()) return s;

        if (locales != null) {
            for (Locale loc : locales) {
                if (loc == null) continue;
                String lang = normLang(loc.getLanguage());
                if ("zh".equals(lang)) return "CN";
                String cc = matchCountryIso(loc.getCountry());
                if (!cc.isEmpty()) return ("US".equals(cc) && isChinaTz(tzId)) ? "CN" : cc;
                String byLang = defaultCountryForLang(lang);
                if (!byLang.isEmpty()) return byLang;
            }
        }
        return isChinaTz(tzId) ? "CN" : "US";
    }

    public static String detectDefaultLanguage(List<Locale> locales, String tzId) {
        if (locales != null) {
            for (Locale loc : locales) {
                if (loc == null) continue;
                String lang = normLang(loc.getLanguage());
                if (findLanguage(lang) != null) return lang;
            }
        }
        return isChinaTz(tzId) ? "zh" : "en";
    }

    private static boolean isChinaTz(String tzId) {
        String t = (tzId == null || tzId.isEmpty()) ? TimeZone.getDefault().getID() : tzId.trim();
        return t != null && ("Asia/Shanghai".equalsIgnoreCase(t) || "Asia/Chongqing".equalsIgnoreCase(t)
                || "Asia/Harbin".equalsIgnoreCase(t) || "Asia/Urumqi".equalsIgnoreCase(t)
                || "Asia/Hong_Kong".equalsIgnoreCase(t) || "Asia/Macau".equalsIgnoreCase(t)
                || "Asia/Taipei".equalsIgnoreCase(t) || "CTT".equalsIgnoreCase(t));
    }

    private static String matchCountryIso(String iso) {
        if (iso == null || iso.trim().length() != 2) return "";
        FilterOption o = findCountry(iso);
        return o != null ? o.id : "";
    }

    private static String normLang(String l) {
        if (l == null) return "";
        String s = l.trim().toLowerCase(Locale.US);
        if ("in".equals(s)) return "id";
        if ("ua".equals(s)) return "uk";
        if ("cz".equals(s)) return "cs";
        return s;
    }

    private static String defaultCountryForLang(String lang) {
        switch (lang) {
            case "zh": return "CN";
            case "ru": case "be": case "kk": return "RU";
            case "pt": return "BR";
            case "de": return "DE";
            case "fr": return "FR";
            case "id": return "ID";
            case "vi": return "VN";
            case "tr": return "TR";
            case "pl": return "PL";
            case "ro": return "RO";
            case "uk": return "UA";
            case "es": return "ES";
            case "it": return "IT";
            case "nl": return "NL";
            case "fi": return "FI";
            case "ja": return "JP";
            case "ko": return "KR";
            case "hi": return "IN";
            case "en": return "US";
            default: return "";
        }
    }

    // ------------------------------------------------------------------ Mojibake recovery

    public static String recoverMojibake(String s, boolean cjkHint) {
        return recoverMojibake(s, cjkHint, null);
    }

    public static String recoverMojibake(String s, boolean cjkHint, String rawLangHint) {
        if (s == null || s.isEmpty() || isPureAscii(s) || containsHan(s)) return s == null ? "" : s;
        if (s.indexOf('\uFFFD') >= 0) return recoverPartialUtf8AroundReplacement(s);

        byte[] raw = toSingleByteStream(s);
        if (raw == null) return s;

        String utf8 = tryStrictDecode(raw, StandardCharsets.UTF_8);
        if (utf8 != null && utf8.length() < s.length() && !hasControlOrReplacement(utf8) && hasHighLetter(utf8)) {
            return utf8;
        }

        String hint = rawLangHint == null ? "" : rawLangHint.toLowerCase(Locale.US);
        if (!cjkHint && isKoreanHint(hint)) {
            String kr = tryStrictDecode(raw, EUC_KR_CS);
            return kr != null ? kr : s;
        }
        if (!cjkHint && isCyrillicHint(hint) && !containsCyrillic(s)) {
            String cyr = tryStrictDecode(raw, CP1251_CS);
            return (cyr != null && containsCyrillic(cyr)) ? cyr : s;
        }
        if (!cjkHint && isExplicitNonChineseForeignLang(hint)) {
            return s;
        }

        if (cjkHint || (hasGbkMojibakeSignature(s) && hasGb2312Level1Density(raw))) {
            String gbk = tryStrictDecode(raw, GBK_CS);
            if (gbk != null && isCredibleChinese(gbk, cjkHint)) return gbk;
            if (cjkHint) {
                String big5 = tryStrictDecode(raw, BIG5_CS);
                if (big5 != null && isCredibleChinese(big5, true)) return big5;
            }
        }
        return s;
    }

    static String tryStrictDecode(byte[] bytes, Charset cs) {
        if (cs == null || bytes == null) return null;
        try {
            return cs.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(ByteBuffer.wrap(bytes)).toString();
        } catch (CharacterCodingException e) {
            return null;
        }
    }

    private static String recoverPartialUtf8AroundReplacement(String s) {
        String[] parts = s.split("\uFFFD", -1);
        if (parts.length <= 1) return s;
        StringBuilder out = new StringBuilder(s.length());
        boolean recovered = false;
        for (int i = 0; i < parts.length; i++) {
            if (i > 0) out.append('\uFFFD');
            String part = parts[i];
            byte[] raw = part.isEmpty() ? null : toSingleByteStream(part);
            if (raw == null) { out.append(part); continue; }
            int start = 0, end = raw.length;
            if (i > 0) while (start < end && (raw[start] & 0xC0) == 0x80) start++;
            if (i + 1 < parts.length) {
                int p = end - 1, trail = 0;
                while (p >= start && (raw[p] & 0xC0) == 0x80 && trail < 3) { p--; trail++; }
                if (p >= start) {
                    int lead = raw[p] & 0xFF;
                    int seq = (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 1;
                    if (seq > 1 && (1 + trail) < seq) end = p;
                }
            }
            String utf8 = (end > start) ? tryStrictDecode(Arrays.copyOfRange(raw, start, end), StandardCharsets.UTF_8) : null;
            if (utf8 != null && utf8.length() < (end - start) && !hasControlOrReplacement(utf8) && hasHighLetter(utf8)) {
                out.append(utf8);
                recovered = true;
            } else {
                out.append(part);
            }
        }
        return recovered ? out.toString() : s;
    }

    private static byte[] toSingleByteStream(String s) {
        boolean preferCp1251 = containsCyrillic(s);
        byte[] out = new byte[s.length()];
        for (int i = 0; i < s.length(); i++) {
            int b = reverseSingleByte(s.charAt(i), preferCp1251);
            if (b < 0) return null;
            out[i] = (byte) b;
        }
        return out;
    }

    private static int reverseSingleByte(char c, boolean preferCp1251) {
        if (c <= 0xFF) return c;
        if (c >= 0x0410 && c <= 0x044F) return 0xC0 + (c - 0x0410);
        if (!preferCp1251) {
            int i2 = CP1252_80_9F.indexOf(c);
            if (i2 >= 0) return 0x80 + i2;
        }
        int i1 = CP1251_80_BF.indexOf(c);
        if (i1 >= 0) return 0x80 + i1;
        int i2 = CP1252_80_9F.indexOf(c);
        return i2 >= 0 ? (0x80 + i2) : -1;
    }

    private static boolean hasGbkMojibakeSignature(String s) {
        boolean cyr = false, lat1 = false;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c >= 0x0400 && c <= 0x04FF) cyr = true;
            if (c >= 0x00C0 && c <= 0x00FF) lat1 = true;
            if (i + 1 < s.length()) {
                char n = s.charAt(i + 1);
                if ((MOJIBAKE_SYMBOLS.indexOf(c) >= 0 && isHighByteChar(n))
                        || (isHighByteChar(c) && MOJIBAKE_SYMBOLS.indexOf(n) >= 0)
                        || (isLowerCyr(c) && isUpperCyr(n))) {
                    return true;
                }
            }
        }
        return cyr && lat1;
    }

    static boolean hasGb2312Level1Density(byte[] raw) {
        int level1 = 0, pairs = 0;
        for (int i = 0; i < raw.length; ) {
            int b0 = raw[i] & 0xFF;
            if (b0 < 0x80) { i++; continue; }
            if (i + 1 >= raw.length) return false;
            int b1 = raw[i + 1] & 0xFF;
            if (b0 >= 0xC0 && isAsciiAlpha(b1) && i > 0 && isAsciiAlpha(raw[i - 1] & 0xFF)) return false;
            pairs++;
            if ((b0 >= 0xB0 && b0 <= 0xD7 && b1 >= 0xA1 && b1 <= 0xFE) || b0 == 0xA1 || b0 == 0xA3) level1++;
            i += 2;
        }
        return pairs > 0 && level1 * 2 >= pairs;
    }

    private static boolean isCredibleChinese(String s, boolean cjkHint) {
        if (hasControlOrReplacement(s)) return false;
        int han = 0, nonAscii = 0;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c >= 0x80) {
                nonAscii++;
                if (c >= 0x4E00 && c <= 0x9FA5) han++;
            }
        }
        return han > 0 && (cjkHint ? (han * 2 >= nonAscii) : (han >= 2 && han * 4 >= nonAscii * 3));
    }

    // ------------------------------------------------------------------ Filtering predicates

    public static boolean matchesAnyLanguage(ServerModel s, Set<String> langIds) {
        if (langIds == null || langIds.isEmpty()) return true;
        for (String id : langIds) if (matchesLanguage(s, id)) return true;
        return false;
    }

    public static boolean matchesLanguage(ServerModel s, String langId) {
        FilterOption opt = findLanguage(langId);
        return opt != null && matchesLanguageOption(s, opt);
    }

    private static boolean matchesLanguageOption(ServerModel s, FilterOption opt) {
        if (s == null || opt == null) return false;
        String laRaw = safe(s.server_language);
        String la = laRaw.toLowerCase(Locale.US);
        String rawText = safe(s.server_name) + " " + safe(s.server_gamemode) + " " + laRaw + " " + safe(s.server_mapname);

        if ("zh".equals(opt.id)) {
            if (containsHangul(rawText) || containsArabic(rawText) || isExplicitNonChineseForeignLang(la)) {
                return false;
            }
            if ((s.server_name.indexOf('\uFFFD') >= 0 || laRaw.indexOf('\uFFFD') >= 0)
                    && !containsHan(rawText) && !la.contains("chinese") && !la.contains("zh")) {
                return false;
            }
        }

        boolean isLowerSentenceLa = "it".equals(opt.id) && laRaw.trim().contains(" ") && laRaw.equals(la);
        for (String kw : opt.keywords) {
            if (kw.length() <= 3 && isPureAscii(kw)) {
                if (isLowerSentenceLa && "it".equals(kw)) continue;
                if (containsWord(la, kw)) return true;
            } else if (la.contains(kw)) {
                return true;
            }
        }

        switch (opt.scriptKind) {
            case SCRIPT_HAN: return containsHan(rawText);
            case SCRIPT_ARABIC: return containsArabic(rawText);
            case SCRIPT_UKRAINIAN: return containsUkrainianChars(rawText);
            case SCRIPT_VIETNAMESE: return containsVietnameseChars(rawText);
            case SCRIPT_CYRILLIC: return containsCyrillic(rawText) && !isExclusivelyNonRussianCyrillic(la);
            default: return false;
        }
    }

    public static boolean matchesAnyCountry(ServerModel s, Set<String> countryCodes) {
        if (countryCodes == null || countryCodes.isEmpty()) return true;
        for (String cc : countryCodes) if (matchesCountry(s, cc)) return true;
        return false;
    }

    public static boolean matchesCountry(ServerModel s, String countryCode) {
        if (s == null || countryCode == null || countryCode.isEmpty()) return false;
        String cc = normalizeGreaterChina(countryCode.trim().toUpperCase(Locale.US));
        String srvCc = s.server_country == null ? "" : s.server_country.trim().toUpperCase(Locale.US);

        if ("CN".equals(cc)) {
            String rawText = safe(s.server_name) + " " + safe(s.server_gamemode) + " " + safe(s.server_language);
            String la = safe(s.server_language).toLowerCase(Locale.US);
            if (containsHangul(rawText) || containsArabic(rawText) || isExplicitNonChineseForeignLang(la)) {
                return false;
            }
            if (matchesLanguage(s, "zh")) {
                return true;
            }
            if ("CN".equals(srvCc)) {
                return s.server_name.indexOf('\uFFFD') < 0 && s.server_language.indexOf('\uFFFD') < 0;
            }
            return false;
        }
        return !srvCc.isEmpty() && cc.equals(srvCc);
    }

    // ------------------------------------------------------------------ Shared script & hint helpers

    static boolean isKoreanHint(String low) {
        return low.contains("korean") || low.contains("korea") || containsWord(low, "kr") || containsWord(low, "kor");
    }

    static boolean isJapaneseHint(String low) {
        return low.contains("japan") || containsWord(low, "jp") || containsWord(low, "jpn");
    }

    static boolean isCyrillicHint(String low) {
        return low.contains("rus") || low.contains("рус") || containsWord(low, "ru")
                || low.contains("ukrain") || containsWord(low, "ua") || low.contains("balkan")
                || low.contains("bulgar") || low.contains("crmp");
    }

    public static boolean isExplicitNonChineseForeignLang(String low) {
        if (low == null || low.isEmpty()) return false;
        if (low.contains("chin") || low.contains("zh") || containsHan(low)
                || containsWord(low, "cn") || containsWord(low, "tw")) {
            return false;
        }
        return isKoreanHint(low) || isJapaneseHint(low) || isCyrillicHint(low)
                || low.contains("indones") || low.contains("bahasa")
                || low.contains("espa") || low.contains("span") || containsWord(low, "es")
                || low.contains("portugu") || low.contains("brasil") || low.contains("brazil") || containsWord(low, "pt")
                || low.contains("german") || low.contains("deutsch") || containsWord(low, "de")
                || low.contains("polsk") || low.contains("polish") || containsWord(low, "pl")
                || low.contains("roman") || containsWord(low, "ro")
                || low.contains("turk") || low.contains("türk") || containsWord(low, "tr")
                || low.contains("viet") || containsWord(low, "vn")
                || low.contains("arab") || low.contains("tagalog")
                || low.contains("french") || low.contains("fran") || low.contains("ital")
                || low.contains("czech") || low.contains("magyar") || low.contains("dutch");
    }

    public static boolean hasCjkContextHint(String rawLang, String rawWebsite, String ipCountry) {
        String la = rawLang == null ? "" : rawLang.toLowerCase(Locale.US);
        if (isExplicitNonChineseForeignLang(la)) return false;
        if ("CN".equals(ipCountry) || "TW".equals(ipCountry)) return true;
        if (la.contains("chin") || la.contains("zh") || containsWord(la, "cn") || containsWord(la, "tw") || containsHan(la)) {
            return true;
        }
        String web = rawWebsite == null ? "" : rawWebsite.toLowerCase(Locale.US);
        return web.endsWith(".cn") || web.contains(".cn/") || web.contains("qq.com") || web.contains("bilibili.com");
    }

    public static boolean containsWord(String haystack, String word) {
        if (haystack == null || word == null || word.isEmpty()) return false;
        int wLen = word.length(), idx = haystack.indexOf(word);
        while (idx >= 0) {
            boolean lOk = idx == 0 || !Character.isLetter(haystack.charAt(idx - 1));
            int end = idx + wLen;
            boolean rOk = end >= haystack.length() || !Character.isLetter(haystack.charAt(end));
            if (lOk && rOk) return true;
            idx = haystack.indexOf(word, idx + 1);
        }
        return false;
    }

    private static boolean isExclusivelyNonRussianCyrillic(String la) {
        return !la.isEmpty()
                && (la.contains("bulgar") || containsWord(la, "bg") || la.contains("serb") || la.contains("balkan") || la.contains("ex-yu"))
                && !(la.contains("rus") || containsWord(la, "ru") || la.contains("рус"));
    }

    private static String safe(String s) { return s == null ? "" : s; }

    static boolean isPureAscii(String s) {
        for (int i = 0; i < s.length(); i++) if (s.charAt(i) >= 0x80) return false;
        return true;
    }

    static boolean isAsciiAlpha(int b) { return (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z'); }

    private static boolean isHighByteChar(char c) { return (c >= 0x00C0 && c <= 0x00FF) || (c >= 0x0400 && c <= 0x04FF); }

    private static boolean isLowerCyr(char c) { return (c >= 0x0430 && c <= 0x044F) || c == 0x0451 || c == 0x0454 || c == 0x0456 || c == 0x0457 || c == 0x0491; }

    private static boolean isUpperCyr(char c) { return (c >= 0x0410 && c <= 0x042F) || c == 0x0401 || c == 0x0404 || c == 0x0406 || c == 0x0407 || c == 0x0490; }

    private static boolean hasControlOrReplacement(String s) {
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if ((c < 0x20 && c != '\t' && c != '\n' && c != '\r') || c == 0x7F || c == '\uFFFD') return true;
        }
        return false;
    }

    private static boolean hasHighLetter(String s) {
        for (int i = 0; i < s.length(); i++) if (s.charAt(i) >= 0x80 && Character.isLetter(s.charAt(i))) return true;
        return false;
    }

    public static boolean containsHan(String s) {
        if (s == null) return false;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF)) return true;
        }
        return false;
    }

    public static boolean containsCyrillic(String s) {
        if (s == null) return false;
        for (int i = 0; i < s.length(); i++) if (s.charAt(i) >= 0x0400 && s.charAt(i) <= 0x04FF) return true;
        return false;
    }

    private static boolean containsUkrainianChars(String s) {
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '\u0404' || c == '\u0454' || c == '\u0406' || c == '\u0456' || c == '\u0407' || c == '\u0457' || c == '\u0490' || c == '\u0491') return true;
        }
        return false;
    }

    private static boolean containsArabic(String s) {
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if ((c >= 0x0600 && c <= 0x06FF) || (c >= 0x0750 && c <= 0x077F)) return true;
        }
        return false;
    }

    private static boolean containsVietnameseChars(String s) {
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if ((c >= 0x1EA0 && c <= 0x1EF9) || c == '\u01A0' || c == '\u01A1' || c == '\u01AF' || c == '\u01B0') return true;
        }
        return false;
    }

    private static boolean containsHangul(String s) {
        for (int i = 0; i < s.length(); i++) if (s.charAt(i) >= 0xAC00 && s.charAt(i) <= 0xD7AF) return true;
        return false;
    }
}

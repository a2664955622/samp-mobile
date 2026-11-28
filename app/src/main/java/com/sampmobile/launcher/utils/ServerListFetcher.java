package com.sampmobile.launcher.utils;

import android.content.Context;

import com.sampmobile.launcher.R;

import java.io.BufferedReader;
import java.io.File;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;

import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;

/**
 * Multi-source public server directory aggregator ({@code api.open.mp/servers/full} +
 * {@code sam.markski.ar/api/GetAllServers} + optional {@code supplemental_servers.json} + local files).
 */
public final class ServerListFetcher {

    private static final String OMP_FULL_URL = "https://api.open.mp/servers/full";
    private static final String OMP_LITE_URL = "https://api.open.mp/servers";
    private static final String SAM_ALL_HTTPS = "https://sam.markski.ar/api/GetAllServers";
    private static final String SAM_FILTERED_HTTP = "http://sam.markski.ar/api/GetFilteredServers?show_empty=1";
    private static final long CACHE_TTL_MS = 60_000L;

    private static final OkHttpClient CLIENT = new OkHttpClient.Builder()
            .connectTimeout(10, TimeUnit.SECONDS)
            .readTimeout(14, TimeUnit.SECONDS)
            .build();
    private static final ExecutorService FETCH_POOL = Executors.newFixedThreadPool(2);
    private static final Object CACHE_LOCK = new Object();
    private static List<ServerModel> sCachedPool = null;
    private static long sCachedAtMs = 0L;

    private ServerListFetcher() {}

    public static List<ServerModel> fetchDirectory(Context ctx, boolean forceNetwork) {
        synchronized (CACHE_LOCK) {
            if (!forceNetwork && sCachedPool != null && !sCachedPool.isEmpty()
                    && (System.currentTimeMillis() - sCachedAtMs) < CACHE_TTL_MS) {
                return new ArrayList<>(sCachedPool);
            }
        }

        GeoIpDatabase.Table geoTable = GeoIpDatabase.get(ctx);
        String ompJson = null, samJson = null;
        try {
            Future<String> fOmp = FETCH_POOL.submit(() -> {
                String s = fetchUrlQuiet(OMP_FULL_URL);
                return s != null ? s : fetchUrlQuiet(OMP_LITE_URL);
            });
            Future<String> fSam = FETCH_POOL.submit(() -> {
                String s = fetchUrlQuiet(SAM_ALL_HTTPS);
                return s != null ? s : fetchUrlQuiet(SAM_FILTERED_HTTP);
            });
            try { ompJson = fOmp.get(16, TimeUnit.SECONDS); } catch (Exception ignored) {}
            try { samJson = fSam.get(16, TimeUnit.SECONDS); } catch (Exception ignored) {}
        } catch (Exception ignored) {}

        String builtinSupp = readRawQuiet(ctx, R.raw.supplemental_servers);
        String extSupp = null, favJson = null, recentJson = null;
        File extDir = ctx != null ? ctx.getExternalFilesDir(null) : null;
        if (extDir != null) {
            extSupp = readFileQuiet(new File(extDir, "supplemental_servers.json"));
            favJson = readFileQuiet(new File(extDir, "servers.json"));
            recentJson = readFileQuiet(new File(extDir, "recent.json"));
        }

        List<ServerModel> merged = aggregateFromJson(
                ompJson, samJson, builtinSupp, extSupp, favJson, recentJson, geoTable);

        synchronized (CACHE_LOCK) {
            if (merged.isEmpty()) {
                return sCachedPool != null ? new ArrayList<>(sCachedPool) : merged;
            }
            if (sCachedPool != null) {
                Map<String, ServerModel> prev = new LinkedHashMap<>();
                for (ServerModel old : sCachedPool) prev.put(old.normalizedKey(), old);
                for (ServerModel m : merged) {
                    ServerModel old = prev.get(m.normalizedKey());
                    if (old != null && old.ping_queried) {
                        m.server_ping = old.server_ping;
                        m.ping_queried = true;
                        if ((m.needs_encoding_refresh && !old.needs_encoding_refresh)
                                || (m.unverified_candidate && !old.unverified_candidate)) {
                            m.server_name = old.server_name;
                            m.server_gamemode = old.server_gamemode;
                            m.server_language = old.server_language;
                            m.server_players = old.server_players;
                            m.server_maxplayers = old.server_maxplayers;
                            m.server_password = old.server_password;
                            m.needs_encoding_refresh = false;
                            m.unverified_candidate = false;
                            ServerFilterEngine.updateMasks(m);
                        }
                    }
                }
            }
            sCachedPool = new ArrayList<>(merged);
            sCachedAtMs = System.currentTimeMillis();
        }
        return merged;
    }

    public static List<ServerModel> aggregateFromJson(
            String ompJson, String samJson, String builtinSupp, String extSupp,
            String favJson, String recentJson, GeoIpDatabase.Table geo) {

        Map<String, ServerModel> map = new LinkedHashMap<>();
        parsePublicArray(ompJson, true, geo, map);
        parsePublicArray(samJson, false, geo, map);
        parseLocalList(builtinSupp, false, geo, map);
        parseLocalList(extSupp, false, geo, map);
        parseLocalList(favJson, true, geo, map);
        parseLocalList(recentJson, true, geo, map);

        List<ServerModel> online = new ArrayList<>(map.size());
        List<ServerModel> tail = new ArrayList<>();
        for (ServerModel m : map.values()) {
            ServerFilterEngine.updateMasks(m);
            (m.unverified_candidate ? tail : online).add(m);
        }
        Collections.sort(online, (a, b) -> {
            int cmp = Integer.compare(parseInt(b.server_players), parseInt(a.server_players));
            if (cmp != 0) return cmp;
            cmp = Integer.compare(parseInt(b.server_maxplayers), parseInt(a.server_maxplayers));
            return cmp != 0 ? cmp : a.normalizedKey().compareTo(b.normalizedKey());
        });
        online.addAll(tail);
        return online;
    }

    @SuppressWarnings("unchecked")
    private static void parsePublicArray(String json, boolean isOmpSchema, GeoIpDatabase.Table geo, Map<String, ServerModel> out) {
        List<Object> arr = JsonMini.parseArray(json);
        if (arr == null) return;
        for (Object elem : arr) {
            if (!(elem instanceof Map)) continue;
            Map<String, Object> item = (Map<String, Object>) elem;
            if (!isOmpSchema && item.containsKey("success") && !optBool(item, "success")) continue;

            Map<String, Object> src = isOmpSchema && item.get("core") instanceof Map ? (Map<String, Object>) item.get("core") : item;
            Map<String, Object> ru = isOmpSchema && item.get("ru") instanceof Map ? (Map<String, Object>) item.get("ru") : null;
            String[] hp = splitHostPort(optStr(src, isOmpSchema ? "ip" : "ipAddr"));
            if (hp == null) continue;

            String cc = geo != null ? geo.lookupHost(hp[0], false) : "";
            String rawLa = optStr(src, isOmpSchema ? "la" : "language");
            String rawWeb = isOmpSchema
                    ? (ru != null ? firstNonEmpty(optStr(ru, "weburl"), optStr(ru, "url"), optStr(ru, "website")) : "")
                    : optStr(src, "website");
            if ("Unknown".equalsIgnoreCase(rawWeb)) rawWeb = "";

            boolean cjk = ServerFilterEngine.hasCjkContextHint(rawLa, rawWeb, cc);
            String hn = ServerFilterEngine.recoverMojibake(optStr(src, isOmpSchema ? "hn" : "name"), cjk, rawLa);
            String gm = ServerFilterEngine.recoverMojibake(optStr(src, isOmpSchema ? "gm" : "gameMode"), cjk, rawLa);
            String la = ServerFilterEngine.recoverMojibake(rawLa, cjk, rawLa);
            String mapName = ServerFilterEngine.recoverMojibake(
                    isOmpSchema ? (ru != null ? optStr(ru, "mapname") : "") : optStr(src, "mapName"), cjk, rawLa);

            ServerModel m = new ServerModel(
                    hn.isEmpty() ? hp[0] : hn, hp[0], hp[1],
                    optBool(src, isOmpSchema ? "pa" : "requiresPassword") ? "1" : "0",
                    gm.isEmpty() ? "-" : gm,
                    String.valueOf(Math.max(0, optInt(src, isOmpSchema ? "pc" : "playersOnline"))),
                    String.valueOf(Math.max(0, optInt(src, isOmpSchema ? "pm" : "maxPlayers"))),
                    la.isEmpty() ? "-" : la
            );
            m.server_country = cc;
            m.server_website = rawWeb;
            m.server_mapname = mapName;
            m.server_version = optStr(src, isOmpSchema ? "vn" : "version");
            m.is_omp = optBool(src, isOmpSchema ? "omp" : "isOpenMp");
            m.needs_encoding_refresh = hasFffd(hn) || hasFffd(gm) || hasFffd(la);
            mergeInto(out, m);
        }
    }

    @SuppressWarnings("unchecked")
    private static void parseLocalList(String json, boolean forceUnverified, GeoIpDatabase.Table geo, Map<String, ServerModel> out) {
        Map<String, Object> root = JsonMini.parseObject(json);
        if (root == null || !(root.get("servers") instanceof List)) return;
        for (Object elem : (List<Object>) root.get("servers")) {
            if (!(elem instanceof Map)) continue;
            Map<String, Object> item = (Map<String, Object>) elem;
            String host = optStr(item, "host"), port = optStr(item, "port");
            if (host.isEmpty()) {
                String[] hp = splitHostPort(optStr(item, "ip"));
                if (hp != null) { host = hp[0]; port = hp[1]; }
            }
            if (host.isEmpty() || GeoIpDatabase.isNonPublicHost(host)) continue;
            if (port.isEmpty()) port = "7777";

            String name = forceUnverified ? "" : optStr(item, "name");
            String explicitCc = optStr(item, "country").toUpperCase(Locale.US);
            ServerModel m = new ServerModel(
                    name.isEmpty() ? host : name, host, port,
                    optBool(item, "password") ? "1" : "0",
                    forceUnverified || optStr(item, "gamemode").isEmpty() ? "-" : optStr(item, "gamemode"),
                    "0", "0",
                    forceUnverified || optStr(item, "language").isEmpty() ? "-" : optStr(item, "language")
            );
            m.server_country = !explicitCc.isEmpty() ? explicitCc : (geo != null ? geo.lookupHost(host, false) : "");
            m.server_website = optStr(item, "website");
            m.server_discord = optStr(item, "discord");
            m.unverified_candidate = forceUnverified || name.isEmpty();
            out.putIfAbsent(m.normalizedKey(), m);
        }
    }

    private static void mergeInto(Map<String, ServerModel> map, ServerModel in) {
        ServerModel ex = map.get(in.normalizedKey());
        if (ex == null) { map.put(in.normalizedKey(), in); return; }
        ex.server_name = pickClean(ex.server_name, in.server_name, ex.server_host);
        ex.server_gamemode = pickClean(ex.server_gamemode, in.server_gamemode, "-");
        ex.server_language = pickClean(ex.server_language, in.server_language, "-");
        if (ex.server_website.isEmpty()) ex.server_website = in.server_website;
        if (ex.server_mapname.isEmpty()) ex.server_mapname = in.server_mapname;
        if (ex.server_version.isEmpty()) ex.server_version = in.server_version;
        if (ex.server_country.isEmpty()) ex.server_country = in.server_country;
        ex.is_omp = ex.is_omp || in.is_omp;
        ex.unverified_candidate = ex.unverified_candidate && in.unverified_candidate;
        ex.needs_encoding_refresh = hasFffd(ex.server_name) || hasFffd(ex.server_gamemode) || hasFffd(ex.server_language);
        if (parseInt(ex.server_maxplayers) == 0 && parseInt(in.server_maxplayers) > 0) {
            ex.server_players = in.server_players;
            ex.server_maxplayers = in.server_maxplayers;
        }
    }

    private static String pickClean(String cur, String alt, String emptySentinel) {
        if (alt == null || alt.isEmpty() || alt.equals(emptySentinel)) return cur;
        if (cur == null || cur.isEmpty() || cur.equals(emptySentinel) || (hasFffd(cur) && !hasFffd(alt))) return alt;
        return cur;
    }

    private static boolean hasFffd(String s) { return s != null && s.indexOf('\uFFFD') >= 0; }

    private static String[] splitHostPort(String raw) {
        int colon = raw == null ? -1 : raw.lastIndexOf(':');
        if (colon <= 0 || colon + 1 >= raw.length()) return null;
        String h = raw.substring(0, colon).trim();
        int p = parseInt(raw.substring(colon + 1));
        return (h.isEmpty() || p < 1 || p > 65535) ? null : new String[]{h, String.valueOf(p)};
    }

    private static String optStr(Map<String, Object> m, String k) {
        Object v = m != null ? m.get(k) : null;
        String s = v == null ? "" : String.valueOf(v).trim();
        return "null".equalsIgnoreCase(s) ? "" : s;
    }

    private static int optInt(Map<String, Object> m, String k) {
        Object v = m != null ? m.get(k) : null;
        return v instanceof Number ? ((Number) v).intValue() : parseInt(v == null ? "" : String.valueOf(v));
    }

    private static boolean optBool(Map<String, Object> m, String k) {
        Object v = m != null ? m.get(k) : null;
        if (v instanceof Boolean) return (Boolean) v;
        if (v instanceof Number) return ((Number) v).intValue() != 0;
        return v != null && ("true".equalsIgnoreCase(v.toString().trim()) || "1".equals(v.toString().trim()));
    }

    private static String firstNonEmpty(String... vals) {
        for (String v : vals) if (v != null && !v.isEmpty()) return v;
        return "";
    }

    private static int parseInt(String s) {
        try { return Integer.parseInt(s.trim()); } catch (Exception e) { return 0; }
    }

    private static String fetchUrlQuiet(String url) {
        Request req = new Request.Builder().url(url).header("User-Agent", "SAMP-Mobile/1.0").build();
        try (Response resp = CLIENT.newCall(req).execute()) {
            return (resp.isSuccessful() && resp.body() != null) ? resp.body().string() : null;
        } catch (Exception e) { return null; }
    }

    private static String readRawQuiet(Context ctx, int resId) {
        if (ctx == null) return null;
        try (InputStream is = ctx.getResources().openRawResource(resId);
             BufferedReader br = new BufferedReader(new InputStreamReader(is, StandardCharsets.UTF_8))) {
            StringBuilder sb = new StringBuilder();
            String ln;
            while ((ln = br.readLine()) != null) sb.append(ln);
            return sb.toString();
        } catch (Exception e) { return null; }
    }

    private static String readFileQuiet(File f) {
        try { return (f != null && f.isFile() && f.length() > 0) ? Utils.readJSONFromFile(f) : null; }
        catch (Exception e) { return null; }
    }

    /** Compact RFC 8259 JSON parser (runs identically on Android and host JVM unit tests). */
    static final class JsonMini {
        private final String s;
        private final int len;
        private int pos;

        private JsonMini(String s) { this.s = s; this.len = s.length(); }

        @SuppressWarnings("unchecked")
        static List<Object> parseArray(String json) {
            if (json == null || json.isEmpty()) return null;
            try { Object v = new JsonMini(json).val(); return v instanceof List ? (List<Object>) v : null; }
            catch (Exception e) { return null; }
        }

        @SuppressWarnings("unchecked")
        static Map<String, Object> parseObject(String json) {
            if (json == null || json.isEmpty()) return null;
            try { Object v = new JsonMini(json).val(); return v instanceof Map ? (Map<String, Object>) v : null; }
            catch (Exception e) { return null; }
        }

        private void ws() {
            while (pos < len) {
                char c = s.charAt(pos);
                if (c <= ' ' || c == '\uFEFF') pos++; else break;
            }
        }

        private Object val() {
            ws();
            char c = s.charAt(pos);
            if (c == '{') return obj();
            if (c == '[') return arr();
            if (c == '"') return str();
            if (c == 't') { pos += 4; return Boolean.TRUE; }
            if (c == 'f') { pos += 5; return Boolean.FALSE; }
            if (c == 'n') { pos += 4; return null; }
            return num();
        }

        private Map<String, Object> obj() {
            pos++; ws();
            Map<String, Object> m = new LinkedHashMap<>();
            if (s.charAt(pos) == '}') { pos++; return m; }
            while (pos < len) {
                ws(); String k = str(); ws();
                if (s.charAt(pos++) != ':') throw new IllegalArgumentException();
                m.put(k, val()); ws();
                char sep = s.charAt(pos++);
                if (sep == '}') return m;
                if (sep != ',') throw new IllegalArgumentException();
            }
            return m;
        }

        private List<Object> arr() {
            pos++; ws();
            List<Object> l = new ArrayList<>();
            if (s.charAt(pos) == ']') { pos++; return l; }
            while (pos < len) {
                l.add(val()); ws();
                char sep = s.charAt(pos++);
                if (sep == ']') return l;
                if (sep != ',') throw new IllegalArgumentException();
            }
            return l;
        }

        private String str() {
            if (s.charAt(pos++) != '"') throw new IllegalArgumentException();
            int start = pos;
            StringBuilder sb = null;
            while (pos < len) {
                char c = s.charAt(pos++);
                if (c == '"') return sb == null ? s.substring(start, pos - 1) : sb.append(s, start, pos - 1).toString();
                if (c == '\\') {
                    if (sb == null) sb = new StringBuilder(32);
                    sb.append(s, start, pos - 1);
                    char e = s.charAt(pos++);
                    if (e == 'u') { sb.append((char) Integer.parseInt(s.substring(pos, pos + 4), 16)); pos += 4; }
                    else sb.append(e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e == 'b' ? '\b' : e == 'f' ? '\f' : e);
                    start = pos;
                }
            }
            throw new IllegalArgumentException();
        }

        private Number num() {
            int start = pos;
            boolean flt = false;
            while (pos < len) {
                char c = s.charAt(pos);
                if ((c >= '0' && c <= '9') || c == '-' || c == '+') pos++;
                else if (c == '.' || c == 'e' || c == 'E') { flt = true; pos++; }
                else break;
            }
            String sub = s.substring(start, pos);
            if (flt) return Double.parseDouble(sub);
            long lv = Long.parseLong(sub);
            return (lv >= Integer.MIN_VALUE && lv <= Integer.MAX_VALUE) ? (int) lv : lv;
        }
    }
}

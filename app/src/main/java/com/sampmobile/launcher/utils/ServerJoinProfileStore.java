package com.sampmobile.launcher.utils;

import android.content.Context;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Properties;

/**
 * Persistent launcher store for:
 * <ul>
 *   <li>Per-server player nickname and locked-server password ({@code host:port})</li>
 *   <li>Most-recently-used nickname history (capped at {@link #MAX_NICKNAME_HISTORY} = 5)</li>
 *   <li>Global default nickname for newly visited servers</li>
 * </ul>
 *
 * <p>Stored in {@code <externalFilesDir>/SAMP/server_profiles.properties} (UTF-8, atomic
 * temp-file + rename) so {@code :game} process rewrites or settings resets never clobber
 * saved server credentials.</p>
 */
public final class ServerJoinProfileStore {
    public static final int SCHEMA_VERSION = 1;
    public static final int MAX_NICKNAME_HISTORY = 5;
    public static final int MAX_SAVED_SERVERS = 256;
    public static final String DEFAULT_PLACEHOLDER_NICK = "samp_player";
    public static final String REL_PATH = "SAMP/server_profiles.properties";

    private static final String KEY_SCHEMA = "schema_version";
    private static final String KEY_DEFAULT_NICK = "default_nickname";
    private static final String KEY_HISTORY_COUNT = "history.count";
    private static final String KEY_HISTORY_PREFIX = "history.";
    private static final String KEY_SERVER_COUNT = "server.count";
    private static final String KEY_SERVER_PREFIX = "server.";

    private static final Object LOCK = new Object();
    private static ServerJoinProfileStore sCache = null;

    public static final class ServerProfile {
        public final String nickname;
        public final String password;

        public ServerProfile(String nickname, String password) {
            this.nickname = nickname != null ? nickname : "";
            this.password = password != null ? password : "";
        }
    }

    // Insertion-ordered map; most recently used server sits at the tail.
    private final LinkedHashMap<String, ServerProfile> servers = new LinkedHashMap<>();
    // Index 0 = most recently used nickname.
    private final List<String> nicknameHistory = new ArrayList<>();
    private String defaultNickname = "";

    public ServerJoinProfileStore() {
    }

    public static String normalizeServerKey(String host, String portStr) {
        int port = 7777;
        if (portStr != null && !portStr.trim().isEmpty()) {
            try {
                port = Integer.parseInt(portStr.trim());
            } catch (NumberFormatException ignored) {
            }
        }
        return normalizeServerKey(host, port);
    }

    public static String normalizeServerKey(String host, int port) {
        if (host == null) return "";
        String cleanHost = host.trim().toLowerCase(Locale.US);
        if (cleanHost.isEmpty()) return "";
        int cleanPort = (port >= 1 && port <= 65535) ? port : 7777;
        return cleanHost + ":" + cleanPort;
    }

    private static boolean isMeaningfulNick(String nick) {
        if (nick == null) return false;
        String trimmed = nick.trim();
        return !trimmed.isEmpty() && trimmed.length() <= 24;
    }

    private static boolean isCustomNick(String nick) {
        return isMeaningfulNick(nick) && !DEFAULT_PLACEHOLDER_NICK.equals(nick.trim());
    }

    public synchronized String getDefaultNickname(String fallbackGlobalNick) {
        if (isCustomNick(defaultNickname)) {
            return defaultNickname;
        }
        if (isCustomNick(fallbackGlobalNick)) {
            return fallbackGlobalNick.trim();
        }
        if (!nicknameHistory.isEmpty()) {
            return nicknameHistory.get(0);
        }
        if (isMeaningfulNick(fallbackGlobalNick)) {
            return fallbackGlobalNick.trim();
        }
        return DEFAULT_PLACEHOLDER_NICK;
    }

    public synchronized void setDefaultNickname(String nickname) {
        if (!isMeaningfulNick(nickname)) return;
        this.defaultNickname = nickname.trim();
    }

    public synchronized String resolveNicknameForServer(String serverKey, String fallbackGlobalNick) {
        if (serverKey != null && !serverKey.isEmpty()) {
            ServerProfile profile = servers.get(serverKey);
            if (profile != null && isMeaningfulNick(profile.nickname)) {
                return profile.nickname;
            }
        }
        return getDefaultNickname(fallbackGlobalNick);
    }

    public synchronized String getSavedPassword(String serverKey) {
        if (serverKey == null || serverKey.isEmpty()) return "";
        ServerProfile profile = servers.get(serverKey);
        return profile != null ? profile.password : "";
    }

    /**
     * Returns up to {@link #MAX_NICKNAME_HISTORY} (5) historical nicknames, most recent first.
     */
    public synchronized List<String> getNicknameHistory(String fallbackGlobalNick) {
        List<String> out = new ArrayList<>(MAX_NICKNAME_HISTORY);
        for (String item : nicknameHistory) {
            if (isMeaningfulNick(item) && !out.contains(item)) {
                out.add(item);
                if (out.size() >= MAX_NICKNAME_HISTORY) {
                    return Collections.unmodifiableList(out);
                }
            }
        }
        if (isCustomNick(defaultNickname) && !out.contains(defaultNickname) && out.size() < MAX_NICKNAME_HISTORY) {
            out.add(defaultNickname);
        }
        if (isCustomNick(fallbackGlobalNick)) {
            String trimmed = fallbackGlobalNick.trim();
            if (!out.contains(trimmed) && out.size() < MAX_NICKNAME_HISTORY) {
                out.add(trimmed);
            }
        }
        return Collections.unmodifiableList(out);
    }

    /**
     * Pushes a committed nickname to the front of the MRU history list (max 5, deduplicated).
     */
    public synchronized boolean recordNicknameInHistory(String nickname) {
        if (!isMeaningfulNick(nickname)) return false;
        String clean = nickname.trim();
        if (DEFAULT_PLACEHOLDER_NICK.equals(clean) && !nicknameHistory.isEmpty()) {
            // Do not let the built-in placeholder evict real player names when history exists
            return false;
        }
        if (!nicknameHistory.isEmpty() && clean.equals(nicknameHistory.get(0))) {
            return false;
        }
        nicknameHistory.remove(clean);
        nicknameHistory.add(0, clean);
        while (nicknameHistory.size() > MAX_NICKNAME_HISTORY) {
            nicknameHistory.remove(nicknameHistory.size() - 1);
        }
        if (!isCustomNick(defaultNickname) && isCustomNick(clean)) {
            defaultNickname = clean;
        }
        return true;
    }

    /**
     * Records a server join: updates per-server nickname, updates per-server password when
     * {@code updatePassword} is true (leaving any remembered password intact when joining a
     * temporarily unlocked server), and promotes {@code nickname} in the 5-slot history.
     */
    public synchronized void recordJoin(String serverKey, String nickname,
                                        boolean updatePassword, String password) {
        String cleanNick = isMeaningfulNick(nickname) ? nickname.trim() : "";
        if (!cleanNick.isEmpty()) {
            recordNicknameInHistory(cleanNick);
        }

        if (serverKey == null || serverKey.isEmpty()) {
            return;
        }

        ServerProfile prev = servers.remove(serverKey);
        String nextNick = !cleanNick.isEmpty()
                ? cleanNick
                : (prev != null ? prev.nickname : "");
        String nextPass = updatePassword
                ? (password != null ? password : "")
                : (prev != null ? prev.password : "");

        if (!nextNick.isEmpty() || !nextPass.isEmpty()) {
            servers.put(serverKey, new ServerProfile(nextNick, nextPass));
            while (servers.size() > MAX_SAVED_SERVERS) {
                Iterator<String> it = servers.keySet().iterator();
                if (it.hasNext()) {
                    it.next();
                    it.remove();
                }
            }
        }
    }

    public synchronized void seedFromLegacySettings(String lastHost, int lastPort,
                                                    String lastPassword, String globalNick) {
        if (isCustomNick(globalNick)) {
            String trimmed = globalNick.trim();
            if (!isCustomNick(defaultNickname)) {
                defaultNickname = trimmed;
            }
            recordNicknameInHistory(trimmed);
        }
        String cleanHost = lastHost != null ? lastHost.trim() : "";
        if (!cleanHost.isEmpty() && !"127.0.0.1".equals(cleanHost)) {
            String key = normalizeServerKey(cleanHost, lastPort);
            boolean hasPass = lastPassword != null && !lastPassword.isEmpty();
            if (!key.isEmpty() && !servers.containsKey(key) && (isCustomNick(globalNick) || hasPass)) {
                String seedNick = isCustomNick(globalNick) ? globalNick.trim() : "";
                servers.put(key, new ServerProfile(seedNick, hasPass ? lastPassword : ""));
            }
        }
    }

    // ------------------------------------------------------------------ File codec (UTF-8, atomic)

    public synchronized void saveToFile(File file) throws IOException {
        if (file == null) return;
        File parent = file.getParentFile();
        if (parent != null && !parent.exists()) {
            parent.mkdirs();
        }

        Properties props = new Properties();
        props.setProperty(KEY_SCHEMA, String.valueOf(SCHEMA_VERSION));
        if (!defaultNickname.isEmpty()) {
            props.setProperty(KEY_DEFAULT_NICK, defaultNickname);
        }

        int histCount = Math.min(nicknameHistory.size(), MAX_NICKNAME_HISTORY);
        props.setProperty(KEY_HISTORY_COUNT, String.valueOf(histCount));
        for (int i = 0; i < histCount; i++) {
            props.setProperty(KEY_HISTORY_PREFIX + i, nicknameHistory.get(i));
        }

        int idx = 0;
        for (Map.Entry<String, ServerProfile> entry : servers.entrySet()) {
            String prefix = KEY_SERVER_PREFIX + idx + ".";
            ServerProfile p = entry.getValue();
            props.setProperty(prefix + "key", entry.getKey());
            props.setProperty(prefix + "nick", p.nickname);
            props.setProperty(prefix + "pass", p.password);
            idx++;
        }
        props.setProperty(KEY_SERVER_COUNT, String.valueOf(idx));

        File tmp = new File(file.getParentFile(), file.getName() + ".tmp");
        try (OutputStreamWriter writer = new OutputStreamWriter(
                new BufferedOutputStream(new FileOutputStream(tmp)), StandardCharsets.UTF_8)) {
            props.store(writer, null);
        }
        if (!tmp.renameTo(file)) {
            try (OutputStreamWriter writer = new OutputStreamWriter(
                    new BufferedOutputStream(new FileOutputStream(file)), StandardCharsets.UTF_8)) {
                props.store(writer, null);
            }
            tmp.delete();
        }
    }

    public static ServerJoinProfileStore loadFromFile(File file) {
        ServerJoinProfileStore store = new ServerJoinProfileStore();
        if (file == null || !file.isFile()) {
            return store;
        }
        Properties props = new Properties();
        try (InputStreamReader reader = new InputStreamReader(
                new BufferedInputStream(new FileInputStream(file)), StandardCharsets.UTF_8)) {
            props.load(reader);
        } catch (Exception ignored) {
            return store;
        }

        String defNick = props.getProperty(KEY_DEFAULT_NICK, "");
        if (isMeaningfulNick(defNick)) {
            store.defaultNickname = defNick.trim();
        }

        int histCount = parseIntSafe(props.getProperty(KEY_HISTORY_COUNT), 0);
        for (int i = 0; i < histCount && store.nicknameHistory.size() < MAX_NICKNAME_HISTORY; i++) {
            String item = props.getProperty(KEY_HISTORY_PREFIX + i, "");
            if (isMeaningfulNick(item)) {
                String clean = item.trim();
                if (!store.nicknameHistory.contains(clean)) {
                    store.nicknameHistory.add(clean);
                }
            }
        }

        int srvCount = parseIntSafe(props.getProperty(KEY_SERVER_COUNT), 0);
        int startIdx = Math.max(0, srvCount - MAX_SAVED_SERVERS);
        for (int i = startIdx; i < srvCount; i++) {
            String prefix = KEY_SERVER_PREFIX + i + ".";
            String rawKey = props.getProperty(prefix + "key", "").trim().toLowerCase(Locale.US);
            if (rawKey.isEmpty() || !rawKey.contains(":")) continue;
            String nick = props.getProperty(prefix + "nick", "");
            String pass = props.getProperty(prefix + "pass", "");
            String cleanNick = isMeaningfulNick(nick) ? nick.trim() : "";
            if (!cleanNick.isEmpty() || !pass.isEmpty()) {
                store.servers.remove(rawKey);
                store.servers.put(rawKey, new ServerProfile(cleanNick, pass));
            }
        }
        return store;
    }

    private static int parseIntSafe(String s, int fallback) {
        if (s == null) return fallback;
        try {
            return Integer.parseInt(s.trim());
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    // ------------------------------------------------------------------ Android Launcher helpers

    public static File storeFile(Context ctx) {
        if (ctx == null) return null;
        File extDir = ctx.getExternalFilesDir(null);
        return extDir != null ? new File(extDir, REL_PATH) : null;
    }

    public static ServerJoinProfileStore get(Context ctx) {
        if (ctx == null) return new ServerJoinProfileStore();
        Context appCtx = ctx.getApplicationContext();
        synchronized (LOCK) {
            if (sCache != null) {
                return sCache;
            }
            File file = storeFile(appCtx);
            boolean existed = file != null && file.isFile();
            ServerJoinProfileStore loaded = loadFromFile(file);
            if (!existed) {
                ClientSettings settings = ClientSettings.loadLauncher(appCtx);
                loaded.seedFromLegacySettings(
                        settings.getServerHost(),
                        settings.getServerPort(),
                        settings.getServerPassword(),
                        settings.getString("nick_name"));
                if (file != null) {
                    try {
                        loaded.saveToFile(file);
                    } catch (IOException ignored) {
                    }
                }
            }
            sCache = loaded;
            return sCache;
        }
    }

    public static void recordJoinAndSave(Context ctx, String host, int port, String nickname,
                                         boolean updatePassword, String password) {
        if (ctx == null) return;
        Context appCtx = ctx.getApplicationContext();
        synchronized (LOCK) {
            ServerJoinProfileStore store = get(appCtx);
            String key = normalizeServerKey(host, port);
            store.recordJoin(key, nickname, updatePassword, password);
            File file = storeFile(appCtx);
            if (file != null) {
                try {
                    store.saveToFile(file);
                } catch (IOException ignored) {
                }
            }
        }
    }

    public static void updateDefaultNickname(Context ctx, String nickname, boolean commitToHistory) {
        if (ctx == null || !isMeaningfulNick(nickname)) return;
        Context appCtx = ctx.getApplicationContext();
        synchronized (LOCK) {
            ServerJoinProfileStore store = get(appCtx);
            store.setDefaultNickname(nickname);
            if (commitToHistory) {
                store.recordNicknameInHistory(nickname);
            }
            File file = storeFile(appCtx);
            if (file != null) {
                try {
                    store.saveToFile(file);
                } catch (IOException ignored) {
                }
            }
        }
    }
}

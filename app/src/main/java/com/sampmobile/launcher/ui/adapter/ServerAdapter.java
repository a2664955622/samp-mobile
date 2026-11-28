package com.sampmobile.launcher.ui.adapter;

import android.app.Activity;
import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.drawable.Drawable;
import android.os.Handler;
import android.os.Looper;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;

import com.joom.paranoid.Obfuscate;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

import com.sampmobile.launcher.R;
import com.sampmobile.launcher.utils.ClientSettings;
import com.sampmobile.launcher.utils.GeoIpDatabase;
import com.sampmobile.launcher.utils.LauncherFonts;
import com.sampmobile.launcher.utils.LauncherThemeManager;
import com.sampmobile.launcher.utils.SampQuery;
import com.sampmobile.launcher.utils.ServerFilterEngine;
import com.sampmobile.launcher.utils.ServerListFetcher;
import com.sampmobile.launcher.utils.ServerModel;
import com.sampmobile.launcher.utils.Utils;

@Obfuscate
public class ServerAdapter extends RecyclerView.Adapter<ServerAdapter.ViewHolder> {

    public static final int MODE_FAVORITES = 0;
    public static final int MODE_REGIONAL = 1;
    public static final int MODE_INTERNET = 2;
    public static final int MODE_RECENT = 3;

    private static final int QUERY_POOL_SIZE = 8;
    private static final int LOCAL_QUERY_ATTEMPTS = 2;
    private static final int INITIAL_VIEWPORT_PING_BATCH = 32;
    private static final int MAX_ENCODING_RECOVERY_PROBES = 36;
    private static final long DIRECTORY_UI_CACHE_TTL_MS = 60_000L;
    private static final long UI_PING_BATCH_MS = 96L;
    private static final Object PAYLOAD_PING = new Object();
    private static final Object PAYLOAD_SELECTION = new Object();

    public interface OnServerSelectListener {
        void onServerSelected(ServerModel server);
    }

    public interface OnListLoadedListener {
        void onListLoaded();
    }

    static class ViewHolder extends RecyclerView.ViewHolder {
        LinearLayout server_item;
        TextView server_name;
        TextView server_ping;
        ImageView server_password;
        TextView server_gamemode;
        TextView server_players;
        View server_status;
        ProgressBar server_players_bar;

        public ViewHolder(View itemView) {
            super(itemView);
            server_item = itemView.findViewById(R.id.server_item);
            server_name = itemView.findViewById(R.id.server_name);
            server_ping = itemView.findViewById(R.id.server_ping);
            server_password = itemView.findViewById(R.id.server_password);
            server_gamemode = itemView.findViewById(R.id.server_gamemode);
            server_players = itemView.findViewById(R.id.server_players);
            server_status = itemView.findViewById(R.id.server_status);
            server_players_bar = itemView.findViewById(R.id.server_players_bar);
            LauncherFonts.applyCondensed(server_ping, server_players);
        }
    }

    /** Ping quality colour: green up to 120 ms, amber up to 250 ms, red above, faint when unknown. */
    public static int pingColor(Context ctx, String ping) {
        int ms;
        try {
            ms = Integer.parseInt(ping.trim());
        } catch (Exception e) {
            return ctx.getColor(R.color.ui_text_faint);
        }
        if (ms < 0) return ctx.getColor(R.color.ui_text_faint);
        if (ms <= 120) return ctx.getColor(R.color.ui_good);
        if (ms <= 250) return ctx.getColor(R.color.ui_warn);
        return ctx.getColor(R.color.ui_bad);
    }

    /** Server-reported text without a single letter or digit ("-", "..", blank) counts as absent. */
    public static String meaningful(String s) {
        if (s == null) return "";
        String t = s.trim();
        for (int i = 0; i < t.length(); i++) {
            if (Character.isLetterOrDigit(t.charAt(i))) return t;
        }
        return "";
    }

    private static int parseIntSafe(String s) {
        try {
            return Integer.parseInt(s.trim());
        } catch (Exception e) {
            return 0;
        }
    }

    private static boolean isDirectoryMode(int m) {
        return m == MODE_REGIONAL || m == MODE_INTERNET;
    }

    Activity activity;
    public List<ServerModel> serverModel;
    private final List<ServerModel> allServers = new ArrayList<>();
    private final Map<Integer, List<ServerModel>> listCache = new HashMap<>();
    private final Set<String> regionalCountries = new LinkedHashSet<>();
    private final Set<String> internetLanguages = new LinkedHashSet<>();
    private final Set<String> queuedPings = Collections.synchronizedSet(new HashSet<>());

    private int activeCountryMask = 0;
    private int activeLangMask = 0;
    private final int[] countryCounts = new int[ServerFilterEngine.COUNTRIES.size()];
    private final int[] langCounts = new int[ServerFilterEngine.LANGUAGES.size()];
    private int verifiedDirectoryCount = 0;
    private long directoryLoadedAtMs = 0L;
    private boolean favoritesLoaded = false;
    private boolean recentLoaded = false;

    private Drawable cachedSelectedDrawable = null;
    private int cachedSelectedAccent = 0;
    private ColorStateList cachedAccentTint = null;

    private final Handler uiHandler = new Handler(Looper.getMainLooper());
    private final Set<ServerModel> pendingPingItems = new LinkedHashSet<>();
    private boolean pendingStructureRefresh = false;
    private boolean pingFlushScheduled = false;
    private final Runnable flushPendingPingsRunnable = this::flushPendingPings;

    private void flushPendingPings() {
        pingFlushScheduled = false;
        if (activity.isFinishing() || activity.isDestroyed()) {
            pendingPingItems.clear();
            pendingStructureRefresh = false;
            return;
        }
        if (pendingStructureRefresh && isDirectoryMode(mode)) {
            pendingStructureRefresh = false;
            pendingPingItems.clear();
            recomputeDirectoryStats();
            applyFilters();
            if (listLoadedListener != null) listLoadedListener.onListLoaded();
            return;
        }
        pendingStructureRefresh = false;
        if (pendingPingItems.isEmpty()) return;
        for (ServerModel m : pendingPingItems) {
            int idx = indexOf(m);
            if (idx >= 0) {
                notifyItemChanged(idx, PAYLOAD_PING);
            }
        }
        pendingPingItems.clear();
    }

    private int mode = MODE_FAVORITES;
    private String selectedAddress = null;
    private String filterQuery = "";
    private volatile int generation = 0;
    private OnServerSelectListener selectListener;
    private OnListLoadedListener listLoadedListener;

    private final ExecutorService ioExecutor = Executors.newSingleThreadExecutor();
    private final ExecutorService queryPool = Executors.newFixedThreadPool(QUERY_POOL_SIZE);

    public ServerAdapter(Activity activity, List<ServerModel> serverModel, int mode) {
        this.activity = activity;
        this.serverModel = serverModel;
        this.mode = mode;
    }

    public void setOnServerSelectListener(OnServerSelectListener listener) {
        this.selectListener = listener;
    }

    public void setOnListLoadedListener(OnListLoadedListener listener) {
        this.listLoadedListener = listener;
    }

    public int getMode() {
        return mode;
    }

    public boolean hasCachedDataForMode(int targetMode) {
        if (targetMode == MODE_FAVORITES && favoritesLoaded) return true;
        if (targetMode == MODE_RECENT && recentLoaded) return true;
        if (isDirectoryMode(mode) && isDirectoryMode(targetMode) && !allServers.isEmpty()) {
            return true;
        }
        int key = isDirectoryMode(targetMode) ? MODE_INTERNET : targetMode;
        List<ServerModel> c = listCache.get(key);
        return c != null && !c.isEmpty();
    }

    public void invalidateLocalCache(int targetMode) {
        if (targetMode == MODE_FAVORITES) {
            favoritesLoaded = false;
            listCache.remove(MODE_FAVORITES);
        } else if (targetMode == MODE_RECENT) {
            recentLoaded = false;
            listCache.remove(MODE_RECENT);
        }
    }

    public Set<String> getRegionalCountries() {
        return new LinkedHashSet<>(regionalCountries);
    }

    public void setRegionalCountries(Set<String> countries) {
        regionalCountries.clear();
        if (countries != null) regionalCountries.addAll(countries);
        activeCountryMask = ServerFilterEngine.buildCountryMaskForSet(regionalCountries);
        if (mode == MODE_REGIONAL) {
            applyFilters();
            scheduleDirectoryViewportQueries(0, INITIAL_VIEWPORT_PING_BATCH - 1, generation);
        }
    }

    public Set<String> getInternetLanguages() {
        return new LinkedHashSet<>(internetLanguages);
    }

    public void setInternetLanguages(Set<String> languages) {
        internetLanguages.clear();
        if (languages != null) internetLanguages.addAll(languages);
        activeLangMask = ServerFilterEngine.buildLanguageMaskForSet(internetLanguages);
        if (mode == MODE_INTERNET) {
            applyFilters();
            scheduleDirectoryViewportQueries(0, INITIAL_VIEWPORT_PING_BATCH - 1, generation);
        }
    }

    public int countVerifiedDirectoryServers() {
        return verifiedDirectoryCount;
    }

    public int countServersForCountry(String countryCode) {
        int idx = ServerFilterEngine.countryIndex(countryCode);
        return idx >= 0 ? countryCounts[idx] : 0;
    }

    public int countServersForLanguage(String langId) {
        int idx = ServerFilterEngine.languageIndex(langId);
        return idx >= 0 ? langCounts[idx] : 0;
    }

    /** Single O(N) integer bitmask pass (~0.05 ms for 1,500 servers, zero allocations). */
    private void recomputeDirectoryStats() {
        Arrays.fill(countryCounts, 0);
        Arrays.fill(langCounts, 0);
        int verified = 0;
        int nCountries = countryCounts.length;
        int nLangs = langCounts.length;
        for (int sIdx = 0, size = allServers.size(); sIdx < size; sIdx++) {
            ServerModel s = allServers.get(sIdx);
            if (s.unverified_candidate) continue;
            verified++;
            int cm = s.country_mask;
            while (cm != 0) {
                int bit = Integer.numberOfTrailingZeros(cm);
                if (bit < nCountries) countryCounts[bit]++;
                cm &= (cm - 1);
            }
            int lm = s.lang_mask;
            while (lm != 0) {
                int bit = Integer.numberOfTrailingZeros(lm);
                if (bit < nLangs) langCounts[bit]++;
                lm &= (lm - 1);
            }
        }
        verifiedDirectoryCount = verified;
    }

    public ServerModel getSelected() {
        if (selectedAddress == null) return null;
        for (ServerModel s : allServers) {
            if (s.address().equals(selectedAddress)) return s;
        }
        return null;
    }

    public void clearSelection() {
        selectedAddress = null;
        notifyDataSetChanged();
        if (selectListener != null) selectListener.onServerSelected(null);
    }

    public void filter(String query) {
        filterQuery = query == null ? "" : query.trim().toLowerCase(Locale.US);
        applyFilters();
        if (isDirectoryMode(mode)) {
            scheduleDirectoryViewportQueries(0, INITIAL_VIEWPORT_PING_BATCH - 1, generation);
        }
    }

    private void applyFilters() {
        boolean dirMode = isDirectoryMode(mode);
        boolean checkQuery = !filterQuery.isEmpty();
        serverModel.clear();
        for (int i = 0, n = allServers.size(); i < n; i++) {
            ServerModel s = allServers.get(i);
            if (dirMode && s.unverified_candidate) {
                continue;
            }
            if (mode == MODE_REGIONAL && activeCountryMask != 0 && (s.country_mask & activeCountryMask) == 0) {
                continue;
            }
            if (mode == MODE_INTERNET && activeLangMask != 0 && (s.lang_mask & activeLangMask) == 0) {
                continue;
            }
            if (checkQuery && !matchesSearchQuery(s, filterQuery)) {
                continue;
            }
            serverModel.add(s);
        }
        notifyDataSetChanged();
    }

    private static boolean matchesSearchQuery(ServerModel s, String q) {
        return s.server_name.toLowerCase(Locale.US).contains(q)
                || s.server_host.toLowerCase(Locale.US).contains(q)
                || s.server_gamemode.toLowerCase(Locale.US).contains(q)
                || s.server_language.toLowerCase(Locale.US).contains(q)
                || s.server_mapname.toLowerCase(Locale.US).contains(q);
    }

    public boolean deleteSelected() {
        ServerModel selected = getSelected();
        if (selected == null) return false;

        String address = selected.address();
        try {
            if (mode == MODE_FAVORITES) {
                Utils.deleteServerFromFavorites(activity, address);
            } else if (mode == MODE_RECENT) {
                Utils.deleteServerFromRecent(activity, address);
            } else {
                return false;
            }
        } catch (Exception e) {
            e.printStackTrace();
            return false;
        }

        int pos = serverModel.indexOf(selected);
        allServers.remove(selected);
        List<ServerModel> cached = listCache.get(mode);
        if (cached != null) cached.remove(selected);
        if (pos >= 0) {
            serverModel.remove(pos);
            notifyItemRemoved(pos);
        }
        clearSelection();
        return true;
    }

    @NonNull
    public ViewHolder onCreateViewHolder(ViewGroup parent, int i) {
        View view = LayoutInflater.from(parent.getContext()).inflate(R.layout.layout_server_item, parent, false);
        return new ViewHolder(view);
    }

    @Override
    public void onBindViewHolder(@NonNull ViewHolder viewHolder, int position, @NonNull List<Object> payloads) {
        if (payloads.isEmpty()) {
            onBindViewHolder(viewHolder, position);
            return;
        }
        if (position >= serverModel.size()) return;
        ServerModel server = serverModel.get(position);
        int accentColor = LauncherThemeManager.getAccentColor(activity);
        ensureThemeCaches(accentColor);
        for (Object p : payloads) {
            if (p == PAYLOAD_PING) {
                bindServerMetrics(viewHolder, server);
            } else if (p == PAYLOAD_SELECTION) {
                bindRowSelection(viewHolder, server);
            }
        }
    }

    private void ensureThemeCaches(int accentColor) {
        if (cachedSelectedDrawable == null || cachedSelectedAccent != accentColor) {
            cachedSelectedAccent = accentColor;
            cachedSelectedDrawable = LauncherThemeManager.createSelectedRowDrawable(activity, accentColor);
            cachedAccentTint = ColorStateList.valueOf(accentColor);
        }
    }

    private void bindServerMetrics(ViewHolder viewHolder, ServerModel server) {
        viewHolder.server_name.setText(server.server_name);
        String ping = server.server_ping;
        boolean hasPing = !("-".equals(ping) || ping.isEmpty());
        viewHolder.server_ping.setText(hasPing ? ping + " ms" : "—");
        int pingColor = pingColor(activity, ping);
        viewHolder.server_ping.setTextColor(pingColor);
        viewHolder.server_status.setBackgroundTintList(ColorStateList.valueOf(pingColor));

        String gamemode = meaningful(server.server_gamemode);
        String lang = meaningful(server.server_language);
        if (gamemode.isEmpty() && lang.isEmpty()) {
            viewHolder.server_gamemode.setText(server.address());
        } else if (gamemode.isEmpty() || lang.isEmpty()) {
            viewHolder.server_gamemode.setText(gamemode + lang);
        } else {
            viewHolder.server_gamemode.setText(gamemode + "  ·  " + lang);
        }

        int players = parseIntSafe(server.server_players);
        int maxPlayers = parseIntSafe(server.server_maxplayers);
        viewHolder.server_players.setText(server.server_players + "/" + server.server_maxplayers);
        viewHolder.server_players.setTextColor(activity.getColor(players > 0 ? R.color.launcher_text : R.color.launcher_text_dim));
        viewHolder.server_players_bar.setProgress(maxPlayers > 0 ? Math.min(100, players * 100 / maxPlayers) : 0);
        viewHolder.server_password.setVisibility("1".equals(server.server_password) ? View.VISIBLE : View.GONE);
    }

    private void bindRowSelection(ViewHolder viewHolder, ServerModel server) {
        boolean selected = server.address().equals(selectedAddress);
        if (selected) {
            viewHolder.server_item.setBackground(cachedSelectedDrawable);
        } else {
            viewHolder.server_item.setBackgroundResource(R.drawable.ui_row_bg);
        }
    }

    public void onBindViewHolder(ViewHolder viewHolder, int position) {
        if (position >= serverModel.size()) {
            return;
        }

        ServerModel server = serverModel.get(position);
        int accentColor = LauncherThemeManager.getAccentColor(activity);
        ensureThemeCaches(accentColor);

        bindServerMetrics(viewHolder, server);
        viewHolder.server_players_bar.setProgressTintList(cachedAccentTint);
        bindRowSelection(viewHolder, server);

        viewHolder.server_item.setOnClickListener(v -> {
            String address = server.address();
            if (address.equals(selectedAddress)) {
                return;
            }
            String old = selectedAddress;
            selectedAddress = address;
            int count = serverModel.size();
            for (int i = 0; i < count; i++) {
                String a = serverModel.get(i).address();
                if (a.equals(old) || a.equals(address)) {
                    notifyItemChanged(i, PAYLOAD_SELECTION);
                }
            }
            if (selectListener != null) selectListener.onServerSelected(server);
        });
    }

    /**
     * Switch tab with zero UI-thread stutter:
     * - Switching between Regional and Internet reuses the live in-memory pool in <0.1 ms.
     * - Switching back to a warm directory cache within TTL skips redundant background reloads.
     */
    public void selectMode(int newMode) {
        if (newMode == mode) {
            refresh();
            return;
        }

        boolean wasDir = isDirectoryMode(mode);
        boolean nowDir = isDirectoryMode(newMode);

        selectedAddress = null;
        if (selectListener != null) selectListener.onServerSelected(null);

        // Fast path 1: Regional <-> Internet share the exact same in-memory server pool
        if (wasDir && nowDir && !allServers.isEmpty()) {
            mode = newMode;
            applyFilters();
            if (listLoadedListener != null) listLoadedListener.onListLoaded();
            scheduleDirectoryViewportQueries(0, INITIAL_VIEWPORT_PING_BATCH - 1, generation);
            return;
        }

        generation++;
        clearPendingPingBatch();
        queuedPings.clear();
        int prevCacheKey = wasDir ? MODE_INTERNET : mode;
        listCache.put(prevCacheKey, new ArrayList<>(allServers));
        mode = newMode;

        allServers.clear();
        int cacheKey = nowDir ? MODE_INTERNET : mode;
        List<ServerModel> cached = listCache.get(cacheKey);
        if (cached != null) allServers.addAll(cached);
        if (nowDir) {
            recomputeDirectoryStats();
        }
        applyFilters();

        // Fast path 2: Directory pool was already fetched recently — render instantly without re-fetching
        if (nowDir && !allServers.isEmpty()
                && (System.currentTimeMillis() - directoryLoadedAtMs) < DIRECTORY_UI_CACHE_TTL_MS) {
            if (listLoadedListener != null) listLoadedListener.onListLoaded();
            scheduleDirectoryViewportQueries(0, INITIAL_VIEWPORT_PING_BATCH - 1, generation);
            return;
        }

        // Fast path 3: Local Favorites / Recent already loaded and unchanged — instant 0ms switch
        if ((mode == MODE_FAVORITES && favoritesLoaded && cached != null)
                || (mode == MODE_RECENT && recentLoaded && cached != null)) {
            if (listLoadedListener != null) listLoadedListener.onListLoaded();
            return;
        }

        loadAndQuery(false);
    }

    private void clearPendingPingBatch() {
        uiHandler.removeCallbacks(flushPendingPingsRunnable);
        pingFlushScheduled = false;
        pendingPingItems.clear();
        pendingStructureRefresh = false;
    }

    private void schedulePingUiUpdate(ServerModel model, boolean structureChanged) {
        pendingPingItems.add(model);
        if (structureChanged) {
            pendingStructureRefresh = true;
        }
        if (!pingFlushScheduled) {
            pingFlushScheduled = true;
            uiHandler.postDelayed(flushPendingPingsRunnable, UI_PING_BATCH_MS);
        }
    }

    public void setMode(int mode) {
        selectMode(mode);
    }

    /** Reload current mode's source (forcing fresh network pull on directory tabs) and re-query. */
    public void refresh() {
        generation++;
        clearPendingPingBatch();
        queuedPings.clear();
        for (ServerModel s : allServers) {
            s.ping_queried = false;
        }
        loadAndQuery(true);
    }

    public void refreshServers() {
        refresh();
    }

    public void shutdown() {
        clearPendingPingBatch();
        ioExecutor.shutdownNow();
        queryPool.shutdownNow();
    }

    /** Enqueues UDP info/ping queries for visible rows as the user scrolls through large lists. */
    public void onViewportVisibleRange(int firstVisible, int lastVisible) {
        if (!isDirectoryMode(mode) || serverModel.isEmpty()) return;
        int from = Math.max(0, firstVisible - 4);
        int to = Math.min(serverModel.size() - 1, Math.max(firstVisible, lastVisible) + 10);
        scheduleDirectoryViewportQueries(from, to, generation);
    }

    private void scheduleDirectoryViewportQueries(int fromIdx, int toIdx, int gen) {
        if (gen != generation) return;
        int end = Math.min(serverModel.size() - 1, toIdx);
        for (int i = Math.max(0, fromIdx); i <= end; i++) {
            enqueueDirectoryQuery(serverModel.get(i), gen);
        }
    }

    private void enqueueDirectoryQuery(ServerModel model, int gen) {
        if (model == null || model.ping_queried) return;
        String key = model.normalizedKey();
        if (!queuedPings.add(key)) return;
        queryPool.execute(() -> queryServer(model, true, gen));
    }

    private void loadAndQuery(boolean forceNetwork) {
        final int gen = generation;
        final int listMode = mode;
        final boolean dirMode = isDirectoryMode(listMode);
        ioExecutor.execute(() -> {
            List<ServerModel> parsed = dirMode
                    ? ServerListFetcher.fetchDirectory(activity, forceNetwork)
                    : loadLocal(listMode);
            if (gen != generation) return;

            activity.runOnUiThread(() -> {
                if (activity.isFinishing() || activity.isDestroyed()) return;
                if (gen != generation) return;

                int cacheKey = dirMode ? MODE_INTERNET : listMode;
                Map<String, ServerModel> oldByKey = new HashMap<>();
                for (ServerModel s : allServers) oldByKey.put(s.normalizedKey(), s);
                List<ServerModel> cached = listCache.get(cacheKey);
                if (cached != null) {
                    for (ServerModel s : cached) oldByKey.putIfAbsent(s.normalizedKey(), s);
                }

                allServers.clear();
                for (ServerModel s : parsed) {
                    ServerModel old = oldByKey.get(s.normalizedKey());
                    if (old != null && !forceNetwork) {
                        allServers.add(old);
                    } else {
                        if (old != null && !"-".equals(old.server_ping) && !dirMode) {
                            s.server_ping = old.server_ping;
                        }
                        allServers.add(s);
                    }
                }
                listCache.put(cacheKey, new ArrayList<>(allServers));
                if (dirMode) {
                    directoryLoadedAtMs = System.currentTimeMillis();
                    recomputeDirectoryStats();
                } else if (listMode == MODE_FAVORITES) {
                    favoritesLoaded = true;
                } else if (listMode == MODE_RECENT) {
                    recentLoaded = true;
                }
                applyFilters();

                if (listLoadedListener != null) listLoadedListener.onListLoaded();

                if (!dirMode) {
                    for (ServerModel model : allServers) {
                        queryPool.execute(() -> queryServer(model, false, gen));
                    }
                } else {
                    scheduleDirectoryViewportQueries(0, INITIAL_VIEWPORT_PING_BATCH - 1, gen);
                    int recoveryCount = 0;
                    for (ServerModel m : allServers) {
                        if (m.unverified_candidate) {
                            enqueueDirectoryQuery(m, gen);
                        } else if (m.needs_encoding_refresh && recoveryCount < MAX_ENCODING_RECOVERY_PROBES) {
                            recoveryCount++;
                            enqueueDirectoryQuery(m, gen);
                        }
                    }
                }
            });
        });
    }

    private List<ServerModel> loadLocal(int listMode) {
        List<ServerModel> parsed = new ArrayList<>();
        try {
            File extDir = activity.getExternalFilesDir(null);
            if (extDir == null) return parsed;
            File serversFile = new File(extDir,
                    listMode == MODE_RECENT ? "recent.json" : "servers.json");
            String jsonString = (serversFile.exists() && serversFile.length() > 0)
                    ? Utils.readJSONFromFile(serversFile) : "{\"servers\":[]}";
            JSONObject json = new JSONObject(jsonString);
            JSONArray serversArray = json.getJSONArray("servers");

            for (int i = 0; i < serversArray.length(); i++) {
                JSONObject serverJson = serversArray.getJSONObject(i);
                String host = serverJson.getString("host");
                String port = serverJson.getString("port");

                ServerModel model = new ServerModel(activity.getString(R.string.loading), host, port, "0", "-", "0", "0", "-");
                model.server_discord = optStr(serverJson, "discord");
                model.server_website = optStr(serverJson, "website");
                parsed.add(model);
            }
        } catch (Exception e) {
            e.printStackTrace();
        }
        return parsed;
    }

    private static String optStr(JSONObject j, String key) {
        String s = j.optString(key, "");
        return "null".equals(s) ? "" : s;
    }

    private int indexOf(ServerModel model) {
        return serverModel.indexOf(model);
    }

    private void queryServer(ServerModel model, boolean directoryMode, int gen) {
        int attempts = directoryMode ? 1 : LOCAL_QUERY_ATTEMPTS;
        int timeoutMs = directoryMode ? 1200 : 2000;
        boolean done = false;
        for (int a = 0; a < attempts && !done; a++) {
            if (gen != generation) return;
            done = fetchServerInfo(model, directoryMode, timeoutMs, gen);
            if (!done && a + 1 < attempts) {
                try {
                    Thread.sleep(700);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    return;
                }
            }
        }

        if (!done && gen == generation) {
            activity.runOnUiThread(() -> {
                if (activity.isFinishing() || activity.isDestroyed() || gen != generation) return;
                model.ping_queried = true;
                if (!directoryMode) {
                    model.server_name = activity.getString(R.string.server_unreachable);
                    model.server_players = "0";
                    model.server_maxplayers = "0";
                    model.server_gamemode = "-";
                }
                model.server_ping = "-";
                schedulePingUiUpdate(model, false);
            });
        }
    }

    private boolean fetchServerInfo(ServerModel model, boolean directoryMode, int timeoutMs, int gen) {
        if (gen != generation) return true;
        SampQuery sampQuery = null;
        try {
            String charset = ClientSettings.getCharsetForLauncher(activity);
            sampQuery = new SampQuery(model.server_host, Integer.parseInt(model.server_port),
                    charset, timeoutMs, model.server_language, directoryMode);

            String[] infos = sampQuery.getInfo();
            if (infos == null) {
                return false;
            }

            String hostname = infos[3];
            if (hostname == null || hostname.replace(" ", "").isEmpty()) {
                return false;
            }

            long ping = sampQuery.getPing();
            String resolvedCountry = model.server_country;
            if (directoryMode && (resolvedCountry == null || resolvedCountry.isEmpty())) {
                resolvedCountry = GeoIpDatabase.get(activity).lookupHost(model.server_host, true);
            }

            // Precompute updated bitmasks off the UI thread
            int nextCountryMask = model.country_mask;
            int nextLangMask = model.lang_mask;
            if (directoryMode) {
                ServerModel scratch = new ServerModel(hostname, model.server_host, model.server_port,
                        infos[0], infos[4], infos[1], infos[2], infos[5]);
                scratch.server_country = resolvedCountry != null ? resolvedCountry : "";
                scratch.server_website = model.server_website;
                scratch.server_mapname = model.server_mapname;
                ServerFilterEngine.updateMasks(scratch);
                nextCountryMask = scratch.country_mask;
                nextLangMask = scratch.lang_mask;
            }

            if (gen != generation) return true;

            final String fPass = infos[0];
            final String fPlayers = infos[1];
            final String fMaxPlayers = infos[2];
            final String fName = hostname;
            final String fMode = infos[4];
            final String fLang = infos[5];
            final String fPing = ping >= 0 ? String.valueOf(ping) : "-";
            final String fCountry = resolvedCountry;
            final int fCountryMask = nextCountryMask;
            final int fLangMask = nextLangMask;

            activity.runOnUiThread(() -> {
                if (activity.isFinishing() || activity.isDestroyed() || gen != generation) return;
                boolean maskOrVisibilityChanged = model.unverified_candidate
                        || model.needs_encoding_refresh
                        || model.country_mask != fCountryMask
                        || model.lang_mask != fLangMask;

                model.server_name = fName;
                model.server_password = fPass;
                model.server_gamemode = fMode;
                model.server_players = fPlayers;
                model.server_maxplayers = fMaxPlayers;
                model.server_language = fLang;
                model.server_ping = fPing;
                if (fCountry != null && !fCountry.isEmpty()) {
                    model.server_country = fCountry;
                }
                model.country_mask = fCountryMask;
                model.lang_mask = fLangMask;
                model.unverified_candidate = false;
                model.needs_encoding_refresh = false;
                model.ping_queried = true;

                schedulePingUiUpdate(model, maskOrVisibilityChanged && isDirectoryMode(mode));
                if (selectListener != null && model.address().equals(selectedAddress)) {
                    selectListener.onServerSelected(model);
                }
            });
        } catch (Exception e) {
            return false;
        } finally {
            if (sampQuery != null) {
                sampQuery.close();
            }
        }
        return true;
    }

    public int getItemCount() {
        return serverModel.size();
    }
}

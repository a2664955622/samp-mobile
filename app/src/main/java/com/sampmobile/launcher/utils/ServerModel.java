package com.sampmobile.launcher.utils;

import com.joom.paranoid.Obfuscate;

@Obfuscate
public class ServerModel {
    public String server_name = "Loading...";
    public String server_host = "127.0.0.1";
    public String server_port = "7777";
    public String server_password = "0";
    public String server_gamemode = "Mobile / PC";
    public String server_players = "0";
    public String server_maxplayers = "0";
    public String server_language = "English";
    public String server_ping = "-";
    public String server_discord = "";
    public String server_website = "";
    public String server_mapname = "";
    public String server_version = "";
    public String server_country = "";
    public int country_mask = 0;
    public int lang_mask = 0;
    public boolean is_omp = false;
    public boolean needs_encoding_refresh = false;
    public boolean unverified_candidate = false;
    public volatile boolean ping_queried = false;
    private String cachedAddress;
    private String cachedNormalizedKey;

    public ServerModel(String server_name, String server_host, String server_port, String server_password, String server_gamemode, String server_players, String server_maxplayers, String server_language) {
        this.server_name = server_name;
        this.server_host = server_host;
        this.server_port = server_port;
        this.server_password = server_password;
        this.server_gamemode = server_gamemode;
        this.server_players = server_players;
        this.server_maxplayers = server_maxplayers;
        this.server_language = server_language;
    }

    public String address() {
        String a = cachedAddress;
        if (a == null) {
            a = server_host + ":" + server_port;
            cachedAddress = a;
        }
        return a;
    }

    public String normalizedKey() {
        String k = cachedNormalizedKey;
        if (k == null) {
            String h = server_host == null ? "" : server_host.trim().toLowerCase(java.util.Locale.US);
            String p = server_port == null ? "7777" : server_port.trim();
            k = h + ":" + p;
            cachedNormalizedKey = k;
        }
        return k;
    }
}

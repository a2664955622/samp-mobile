package com.sampmobile.launcher.utils;

import android.content.Context;

import com.sampmobile.launcher.R;

import java.io.BufferedInputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.util.zip.GZIPInputStream;

/**
 * Compact offline IPv4-to-country (ISO 3166-1 alpha-2) lookup table backed by
 * {@code res/raw/ipv4_country.bin} (CC0-1.0 Public Domain dataset from
 * {@code @ip-location-db/geo-whois-asn-country}, combining RFC 8805 GeoFeed, Whois, and ASN).
 *
 * <p>Binary format {@code IPC1} (gzip-compressed):
 * <ul>
 *   <li>4B ASCII magic {@code "IPC1"}</li>
 *   <li>4B big-endian {@code count}</li>
 *   <li>{@code count * 4B} big-endian delta-encoded unsigned IPv4 start boundaries</li>
 *   <li>{@code count * 2B} big-endian packed two-letter ASCII country codes ({@code 0} = gap)</li>
 * </ul>
 */
public final class GeoIpDatabase {

    public static final class Table {
        final int[] starts;
        final short[] codes;

        Table(int[] starts, short[] codes) {
            this.starts = starts;
            this.codes = codes;
        }

        public int size() {
            return starts.length;
        }

        public String lookupHost(String host, boolean allowDnsResolve) {
            if (host == null || host.isEmpty()) return "";
            String trimmed = host.trim();
            long ip = parseIpv4Literal(trimmed);
            if (ip < 0 && allowDnsResolve) {
                try {
                    InetAddress addr = InetAddress.getByName(trimmed);
                    if (addr instanceof Inet4Address) {
                        byte[] b = addr.getAddress();
                        ip = ((b[0] & 0xFFL) << 24)
                                | ((b[1] & 0xFFL) << 16)
                                | ((b[2] & 0xFFL) << 8)
                                | (b[3] & 0xFFL);
                    }
                } catch (Exception ignored) {
                }
            }
            if (ip < 0 || isPrivateOrLoopbackIpv4(ip)) {
                return "";
            }
            return lookupUnsignedIpv4((int) ip);
        }

        public String lookupUnsignedIpv4(int targetUnsigned) {
            int n = starts.length;
            if (n == 0) return "";
            int targetKey = targetUnsigned ^ 0x80000000;
            int lo = 0;
            int hi = n - 1;
            int idx = -1;
            while (lo <= hi) {
                int mid = (lo + hi) >>> 1;
                int midKey = starts[mid] ^ 0x80000000;
                if (midKey <= targetKey) {
                    idx = mid;
                    lo = mid + 1;
                } else {
                    hi = mid - 1;
                }
            }
            if (idx < 0) return "";
            int packed = codes[idx] & 0xFFFF;
            if (packed == 0) return "";
            char c0 = (char) ((packed >>> 8) & 0xFF);
            char c1 = (char) (packed & 0xFF);
            if (c0 < 'A' || c0 > 'Z' || c1 < 'A' || c1 > 'Z') return "";
            return new String(new char[]{c0, c1});
        }
    }

    private static final Table EMPTY_TABLE = new Table(new int[0], new short[0]);
    private static volatile Table sCached = null;

    private GeoIpDatabase() {}

    public static void preloadAsync(Context context) {
        if (sCached != null || context == null) return;
        final Context appCtx = context.getApplicationContext();
        Thread t = new Thread(() -> get(appCtx), "geoip-preload");
        t.setPriority(Thread.MIN_PRIORITY + 1);
        t.setDaemon(true);
        t.start();
    }

    public static Table get(Context context) {
        Table local = sCached;
        if (local != null) return local;
        if (context == null) return EMPTY_TABLE;
        synchronized (GeoIpDatabase.class) {
            if (sCached != null) return sCached;
            try (InputStream raw = context.getApplicationContext().getResources().openRawResource(R.raw.ipv4_country)) {
                sCached = loadFromGzipStream(raw);
            } catch (Exception e) {
                e.printStackTrace();
                sCached = EMPTY_TABLE;
            }
            return sCached;
        }
    }

    public static Table loadFromGzipStream(InputStream gzipStream) throws IOException {
        try (DataInputStream dis = new DataInputStream(
                new BufferedInputStream(new GZIPInputStream(gzipStream, 16384), 32768))) {
            byte b0 = dis.readByte();
            byte b1 = dis.readByte();
            byte b2 = dis.readByte();
            byte b3 = dis.readByte();
            if (b0 != 'I' || b1 != 'P' || b2 != 'C' || b3 != '1') {
                throw new IOException("Invalid GeoIP header magic");
            }
            int count = dis.readInt();
            if (count < 0 || count > 2_000_000) {
                throw new IOException("Out-of-bounds GeoIP entry count: " + count);
            }
            int[] starts = new int[count];
            short[] codes = new short[count];
            int acc = 0;
            for (int i = 0; i < count; i++) {
                acc += dis.readInt();
                starts[i] = acc;
            }
            for (int i = 0; i < count; i++) {
                codes[i] = dis.readShort();
            }
            return new Table(starts, codes);
        }
    }

    /** Fast zero-allocation dotted-quad IPv4 parser. Returns unsigned 32-bit value or -1L if not literal IPv4. */
    public static long parseIpv4Literal(String s) {
        if (s == null) return -1L;
        int len = s.length();
        if (len < 7 || len > 15) return -1L;
        long result = 0L;
        int acc = 0;
        int digits = 0;
        int dots = 0;
        for (int i = 0; i < len; i++) {
            char c = s.charAt(i);
            if (c >= '0' && c <= '9') {
                acc = acc * 10 + (c - '0');
                digits++;
                if (digits > 3 || acc > 255) return -1L;
            } else if (c == '.') {
                if (digits == 0 || dots >= 3) return -1L;
                result = (result << 8) | acc;
                acc = 0;
                digits = 0;
                dots++;
            } else {
                return -1L;
            }
        }
        if (dots != 3 || digits == 0) return -1L;
        return (result << 8) | acc;
    }

    public static boolean isPrivateOrLoopbackIpv4(long ip) {
        if (ip < 0 || ip > 0xFFFFFFFFL) return true;
        int b0 = (int) ((ip >>> 24) & 0xFF);
        int b1 = (int) ((ip >>> 16) & 0xFF);
        if (b0 == 0 || b0 == 10 || b0 == 127 || b0 >= 224) return true;
        if (b0 == 172 && b1 >= 16 && b1 <= 31) return true;
        if (b0 == 192 && b1 == 168) return true;
        return b0 == 169 && b1 == 254;
    }

    public static boolean isNonPublicHost(String host) {
        if (host == null) return true;
        String h = host.trim().toLowerCase(java.util.Locale.US);
        if (h.isEmpty() || "localhost".equals(h) || h.endsWith(".local") || h.endsWith(".lan")) {
            return true;
        }
        long ip = parseIpv4Literal(h);
        return ip >= 0 && isPrivateOrLoopbackIpv4(ip);
    }
}

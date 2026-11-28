package com.sampmobile.launcher.utils;

import android.content.Context;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.os.LocaleList;
import android.telephony.TelephonyManager;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.TimeZone;

/**
 * Fixed in-app language selection shared by the launcher and the isolated :game process.
 *
 * The first activity resolve turns the internal "auto" sentinel into a concrete zh/en
 * choice from the pre-override system locale list. From then on settings.json is the
 * only source of truth, so later system locale changes never override the user's choice.
 */
public final class LocaleManager {
    public static final String LANG_AUTO = "auto";
    public static final String LANG_ZH = "zh";
    public static final String LANG_EN = "en";

    private LocaleManager() {}

    public static Context wrap(Context base) {
        String language = currentLanguage(base);
        I18n.setLanguage(language);
        Locale locale = localeFor(language);
        Locale.setDefault(locale);

        Configuration config = new Configuration(base.getResources().getConfiguration());
        config.setLocale(locale);
        config.setLayoutDirection(locale);
        return base.createConfigurationContext(config);
    }

    /** Returns the persisted concrete language, initializing it once from the device locale when needed. */
    public static String currentLanguage(Context context) {
        ClientSettings settings = ClientSettings.loadLauncher(context);
        String language = normalize(settings.getString("app_language"));
        if (LANG_AUTO.equals(language)) {
            language = detectSystemLanguage(context);
            ClientSettings.setSync(context, "app_language", language);
        }
        String reg = settings.getString("regional_countries");
        if (reg == null || reg.trim().isEmpty() || LANG_AUTO.equalsIgnoreCase(reg.trim())) {
            String detectedCountry = detectDeviceCountry(context);
            ClientSettings.setAsync(context, "regional_countries", detectedCountry);
        }
        return language;
    }

    /** Detects the user's country from SIM/Network ISO, OS system LocaleList, and TimeZone. */
    public static String detectDeviceCountry(Context context) {
        String simIso = "";
        String netIso = "";
        if (context != null) {
            try {
                TelephonyManager tm = (TelephonyManager) context.getSystemService(Context.TELEPHONY_SERVICE);
                if (tm != null) {
                    simIso = tm.getSimCountryIso();
                    netIso = tm.getNetworkCountryIso();
                }
            } catch (Exception ignored) {
            }
        }
        List<Locale> sysLocales = collectSystemLocales(context);
        return ServerFilterEngine.detectDefaultCountry(simIso, netIso, sysLocales, TimeZone.getDefault().getID());
    }

    /** Detects the user's preferred server language (among the 18 open.mp languages) from OS LocaleList. */
    public static String detectDeviceServerLanguage(Context context) {
        return ServerFilterEngine.detectDefaultLanguage(collectSystemLocales(context), TimeZone.getDefault().getID());
    }

    private static List<Locale> collectSystemLocales(Context context) {
        List<Locale> out = new ArrayList<>();
        try {
            LocaleList sys = Resources.getSystem().getConfiguration().getLocales();
            for (int i = 0; i < sys.size(); i++) {
                if (sys.get(i) != null) out.add(sys.get(i));
            }
        } catch (Exception ignored) {
        }
        if (context != null) {
            try {
                LocaleList ctxLocs = context.getResources().getConfiguration().getLocales();
                for (int i = 0; i < ctxLocs.size(); i++) {
                    Locale l = ctxLocs.get(i);
                    if (l != null && !out.contains(l)) out.add(l);
                }
            } catch (Exception ignored) {
            }
        }
        return out;
    }

    public static void setLanguage(Context context, String language) {
        String normalized = normalizeConcrete(language, LANG_ZH);
        ClientSettings.setSync(context, "app_language", normalized);
        I18n.setLanguage(normalized);
    }

    /** Detects the pre-override device locale preference order; unsupported locales fall back to English. */
    public static String detectSystemLanguage(Context context) {
        LocaleList locales = context.getResources().getConfiguration().getLocales();
        for (int i = 0; i < locales.size(); i++) {
            Locale locale = locales.get(i);
            if (locale != null && "zh".equalsIgnoreCase(locale.getLanguage())) {
                return LANG_ZH;
            }
        }
        return LANG_EN;
    }

    public static Locale localeFor(String language) {
        return LANG_ZH.equals(normalizeConcrete(language, LANG_EN)) ? new Locale("zh") : Locale.ENGLISH;
    }

    public static String normalize(String language) {
        if (LANG_ZH.equalsIgnoreCase(language) || LANG_EN.equalsIgnoreCase(language)) {
            return language.toLowerCase(Locale.US);
        }
        return LANG_AUTO;
    }

    private static String normalizeConcrete(String language, String fallback) {
        String normalized = normalize(language);
        return LANG_AUTO.equals(normalized) ? fallback : normalized;
    }
}

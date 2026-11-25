package com.sampmobile.launcher.ui.fragment;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.graphics.BitmapFactory;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.StatFs;
import android.provider.MediaStore;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.CompoundButton;
import android.widget.EditText;
import android.widget.Filter;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.RelativeLayout;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.annotation.NonNull;
import androidx.appcompat.app.AlertDialog;
import androidx.core.content.FileProvider;
import androidx.fragment.app.Fragment;
import androidx.recyclerview.widget.GridLayoutManager;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.bumptech.glide.Glide;
import com.google.android.material.textfield.MaterialAutoCompleteTextView;
import com.joom.paranoid.Obfuscate;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

import com.sampmobile.launcher.BuildConfig;
import com.sampmobile.launcher.R;
import com.sampmobile.launcher.download.GamedataInstallerTask;
import com.sampmobile.launcher.download.GamedataState;
import com.sampmobile.launcher.ui.GameDataImportDialog;
import com.sampmobile.launcher.ui.widget.GtaOptionView;
import com.sampmobile.launcher.ui.widget.GtaSaSliderView;
import com.sampmobile.launcher.ui.widget.GtaTabStrip;
import com.sampmobile.launcher.utils.GameDataImporter;
import com.sampmobile.launcher.utils.GameDataVerifier;
import com.sampmobile.launcher.utils.LauncherThemeManager;
import com.sampmobile.launcher.utils.Utils;

@Obfuscate
public class SettingsPageFragment extends Fragment implements LauncherThemeManager.ThemeListener {

    private static final int CAT_MULTIPLAYER = 0;
    private static final int CAT_GAMEDATA    = 1;
    private static final int CAT_GRAPHICS    = 2;
    private static final int CAT_CAMERA      = 3;
    private static final int CAT_CONTROLS    = 4;
    private static final int CAT_AUDIO       = 5;
    private static final int CAT_HUD_CHAT    = 6;
    private static final int CAT_SCREENSHOTS = 7;
    private static final int CAT_LOGS        = 8;
    private static final int CAT_HELP_ABOUT  = 9;

    private static final String[] CATEGORY_TITLES = {
            "联机",
            "文件验证",
            "画面",
            "镜头",
            "操作",
            "音频",
            "界面与聊天",
            "截图库",
            "问题反馈",
            "帮助与关于"
    };

    private static final String[] GAMEDATA_EDITION_ITEMS = {
            "精简版（推荐）",
            "完整版（2.10 GiB）"
    };

    private int currentCategory = CAT_MULTIPLAYER;
    private final ExecutorService ioExecutor = Executors.newSingleThreadExecutor();
    private View rootView;
    private GtaTabStrip settingsTabs;

    private View[] categoryPanels;

    private EditText et_settings_nickname;
    private GtaOptionView samp_version, charset, fps_limit;
    private MaterialAutoCompleteTextView game_version;
    private GtaOptionView dropdown_aspect_ratio, dropdown_visual_fx, dropdown_shadows, dropdown_car_reflections, dropdown_traffic_mode;
    private GtaOptionView dropdown_ped_cam_zoom, dropdown_veh_cam_height;
    private GtaOptionView dropdown_targeting_mode, dropdown_touch_steering, dropdown_touch_layout, dropdown_accelerometer;
    private GtaOptionView dropdown_radio_station, dropdown_voice_mic_mode;
    private GtaOptionView dropdown_crosshair_style, dropdown_font_family;

    private TextView btn_gfx_preset_low, btn_gfx_preset_med, btn_gfx_preset_high, btn_gfx_preset_ultra;
    private TextView btn_open_hud_editor, btn_reset_hud_editor;

    // 文件验证与资源管理选项卡
    private GtaOptionView option_gd_edition;
    private TextView tv_gd_summary, tv_gd_storage_meta, tv_gd_list_summary, tv_gd_list_empty;
    private TextView btn_gd_import, btn_gd_download, btn_gd_verify, btn_gd_filter_all, btn_gd_filter_issues;
    private View layout_gd_download_progress;
    private TextView tv_gd_dl_status, tv_gd_dl_bytes;
    private android.widget.ProgressBar pb_gd_download;
    private RecyclerView rv_gd_files;
    private final List<GameDataVerifier.FileCheckItem> allGameDataItems = new ArrayList<>();
    private final List<GameDataVerifier.FileCheckItem> displayedGameDataItems = new ArrayList<>();
    private GameDataFileAdapter gameDataFileAdapter;
    private GameDataVerifier.Edition selectedGameDataEdition = GameDataVerifier.Edition.LITE;
    private GameDataVerifier.Report lastGameDataReport;
    private boolean gameDataFilterIssuesOnly = false;
    private final AtomicBoolean gdDownloading = new AtomicBoolean(false);
    private volatile GamedataInstallerTask activeDownloadTask;
    private long lastDownloadProgressPostMs = 0L;

    private final ActivityResultLauncher<String[]> gameDataZipPicker =
            registerForActivityResult(new ActivityResultContracts.OpenDocument(), uri -> {
                if (uri != null && isAdded()) {
                    GameDataImportDialog.startImport(requireActivity(), uri, this::refreshGameDataVerification);
                }
            });

    private Switch voice, timestamp, fullscreen, displayfps, systemkeyboard;
    private Switch modify_files, fastconnect;
    private Switch headmove_switch, dlmodels_switch, switch_invert_look, switch_auto_climb;
    private Switch switch_frame_limiter, switch_free_cam, switch_first_person_look;
    private Switch switch_bike_steering, switch_vibration, switch_radio_autotune;
    private Switch switch_km_enabled, switch_joypad_invert_x, switch_joypad_invert_y;
    private Switch switch_voice_mic_enable, switch_voice_speaker_list, switch_voice_balancer, switch_voice_filter;
    private Switch switch_show_gta_hud, switch_show_radar, switch_radar_tap_map, switch_show_nametag_weapon;
    private Switch switch_show_death_window, switch_show_nametags, switch_show_button_panel;
    private Switch switch_show_screenshot_btn, switch_gta_subtitles, switch_chat_shadow;

    private View restore_button;
    private View chat_minus, chat_plus;
    private TextView chat_strings;

    private GtaSaSliderView slider_theme_hue;
    private TextView tv_theme_hue_val;

    private GtaSaSliderView font_size_slider, chatposx_slider, chatposy_slider, chatsizex_slider, chatsizey_slider;
    private TextView font_size_value, chatposx_value, chatposy_value, chatsizex_value, chatsizey_value;

    private GtaSaSliderView slider_resolution, slider_draw_distance, slider_brightness;
    private TextView tv_resolution_val, tv_draw_distance_val, tv_brightness_val;

    private GtaSaSliderView slider_cam_sens_x, slider_cam_sens_y, slider_cam_sens_foot, slider_cam_sens_veh, slider_cam_sens_aim;
    private TextView tv_cam_sens_x_val, tv_cam_sens_y_val, tv_cam_sens_foot_val, tv_cam_sens_veh_val, tv_cam_sens_aim_val;

    private GtaSaSliderView slider_joystick_size, slider_shoot_threshold, slider_analog_sens, slider_flick_sens;
    private TextView tv_joystick_size_val, tv_shoot_threshold_val, tv_analog_sens_val, tv_flick_sens_val;

    private GtaSaSliderView slider_sfx_volume, slider_music_volume, slider_voice_volume, slider_voice_mic_volume, slider_voice_btn_size;
    private TextView tv_sfx_volume_val, tv_music_volume_val, tv_voice_volume_val, tv_voice_mic_volume_val, tv_voice_btn_size_val;

    private GtaSaSliderView slider_nametag_size, slider_textlabel_size;
    private TextView tv_nametag_size_val, tv_textlabel_size_val;

    // 截图库
    private TextView btn_ss_mode_hud, btn_ss_mode_game, tv_screenshots_summary, tv_screenshots_empty;
    private RecyclerView rv_screenshots;
    private final List<File> screenshotFiles = new ArrayList<>();
    private ScreenshotAdapter screenshotAdapter;

    // 问题反馈
    private TextView tv_diag_status_title, tv_diag_status_desc;

    // 帮助与关于
    private TextView tv_help_mods_body, tv_help_faq_body, tv_help_commands;
    private TextView tv_about_version_line, tv_about_summary;

    private SharedPreferences settings_prefs;
    private RelativeLayout game_version_layout;

    private static final String[] SAMP_VERSION_ITEMS = {"0.3.7", "0.3.7-R4", "0.3.7-R5", "0.3.DL-R1"};
    private static final String[] CHARSET_ITEMS = {"GBK", "UTF-8", "BIG5", "CP1251"};
    private static final String[] FPS_ITEMS = {"无限制", "30", "45", "60", "75", "90", "120", "144"};
    private static final int[] FPS_VALUES = {0, 30, 45, 60, 75, 90, 120, 144};
    private static final String[] ASPECT_RATIO_ITEMS = {"自动适配屏幕", "16:9 标准宽屏", "20:9 全面屏", "21:9 超宽屏", "16:10 平板比例", "4:3 经典比例"};
    private static final String[] VISUAL_FX_ITEMS = {"低", "中", "高", "极高"};
    private static final String[] SHADOWS_ITEMS = {"关闭", "经典", "高级实时"};
    private static final String[] CAR_REFL_ITEMS = {"关闭", "低", "中", "高"};
    private static final String[] TRAFFIC_ITEMS = {"稀少", "适中", "密集"};
    private static final String[] PED_ZOOM_ITEMS = {"近距", "标准", "远距"};
    private static final String[] VEH_CAM_ITEMS = {"低视角", "高视角"};
    private static final String[] TARGETING_ITEMS = {"锁定辅助", "自由准星"};
    private static final String[] STEERING_ITEMS = {"虚拟摇杆", "左右按键", "滑动转向"};
    private static final String[] LAYOUT_ITEMS = {"经典布局", "双侧自适应"};
    private static final String[] ACCEL_ITEMS = {"关闭", "轻度", "重度"};
    private static final String[] RADIO_STATION_ITEMS = {
            "关闭电台", "Playback FM", "K-Rose", "K-DST", "Bounce FM",
            "SF-UR", "Radio Los Santos", "Radio X", "CSR 103.9",
            "K-Jah West", "Master Sounds", "WCTR", "自定义音乐"
    };
    private static final String[] VOICE_MIC_MODE_ITEMS = {"按住说话", "点击切换"};
    private static final String[] CROSSHAIR_STYLE_ITEMS = {"原版经典准星", "精简十字准星", "中心高亮圆点", "十字加圆点", "隐藏准星"};
    private static final String[] FONT_FAMILY_ITEMS = {"默认中文黑体", "经典西文粗体", "自定义字体"};

    private static final int[] PANEL_LAYOUTS = {
            R.layout.settings_panel_multiplayer,
            R.layout.settings_panel_gamedata,
            R.layout.settings_panel_graphics,
            R.layout.settings_panel_camera,
            R.layout.settings_panel_controls,
            R.layout.settings_panel_audio,
            R.layout.settings_panel_hud_chat,
            R.layout.settings_panel_screenshots,
            R.layout.settings_panel_logs,
            R.layout.settings_panel_help_about
    };

    /**
     * Inflates the settings page piecewise: the frame first, then one panel per {@link #step()}.
     * The launcher runs a step per idle slot, so pre-warming 600+ views never blocks a frame for long.
     */
    public static final class ViewBuilder {
        private final LayoutInflater inflater;
        private final View root;
        private final ViewGroup panels;
        private int next;

        public ViewBuilder(LayoutInflater inflater, ViewGroup parent) {
            this.inflater = inflater;
            this.root = inflater.inflate(R.layout.fragment_settings_page, parent, false);
            this.panels = root.findViewById(R.id.settings_panels);
        }

        /** Inflates the next panel; returns true while panels remain. */
        public boolean step() {
            if (next < PANEL_LAYOUTS.length) {
                inflater.inflate(PANEL_LAYOUTS[next++], panels, true);
            }
            return next < PANEL_LAYOUTS.length;
        }

        public View finish() {
            while (step()) {
            }
            return root;
        }
    }

    private View prebuiltView;

    public SettingsPageFragment() {
    }

    /** Hands over a page view built ahead of time by {@link ViewBuilder}. */
    public void setPrebuiltView(View view) {
        this.prebuiltView = view;
    }

    @Override
    public View onCreateView(LayoutInflater inflater, ViewGroup container, Bundle savedInstanceState) {
        View view = prebuiltView != null ? prebuiltView : new ViewBuilder(inflater, container).finish();
        prebuiltView = null;
        this.rootView = view;
        settings_prefs = requireActivity().getSharedPreferences("samp_settings", Context.MODE_PRIVATE);

        initViews(view);
        setupCategoryNavigation(view);
        setupListeners(view);
        setWidgetsForSettings();
        populateHelpAndAbout();

        LauncherThemeManager.addListener(this);
        onThemeColorChanged(LauncherThemeManager.getAccentColor(requireContext()));

        return view;
    }

    public void onPageShown() {
        if (!isAdded() || settings_prefs == null) return;
        setWidgetsForSettings();
        if (currentCategory == CAT_GAMEDATA) {
            refreshGameDataVerification();
        } else if (currentCategory == CAT_SCREENSHOTS) {
            refreshScreenshotGallery();
        } else if (currentCategory == CAT_LOGS) {
            refreshDiagnosticStatus();
        }
    }

    /** 供 MainActivity 在缺少游戏数据时一键切到「文件验证」选项卡。 */
    public void openGameDataTab() {
        if (categoryPanels == null) return;
        if (settingsTabs != null) {
            settingsTabs.setSelectedTab(CAT_GAMEDATA, false);
        }
        switchCategory(CAT_GAMEDATA);
    }

    @Override
    public void onDestroyView() {
        cancelActiveGameDataDownload();
        LauncherThemeManager.removeListener(this);
        super.onDestroyView();
    }

    @Override
    public void onDestroy() {
        cancelActiveGameDataDownload();
        super.onDestroy();
        ioExecutor.shutdownNow();
    }

    @Override
    public void onThemeColorChanged(int accentColor) {
        if (rootView == null || !isAdded()) return;
        LauncherThemeManager.applyThemeToTree(rootView, accentColor);
        updateGraphicsPresetUI();
        updateScreenshotModeUI();
        updateHudEditorButtonUI();
        updateGameDataFilterUI();
        updateGameDataDownloadButtonUI();
        if (screenshotAdapter != null) {
            screenshotAdapter.notifyDataSetChanged();
        }
    }

    private void initViews(View view) {

        et_settings_nickname = view.findViewById(R.id.et_settings_nickname);
        samp_version = view.findViewById(R.id.samp_version);
        game_version = view.findViewById(R.id.game_version);
        charset = view.findViewById(R.id.charset);
        game_version_layout = view.findViewById(R.id.game_version_layout);

        slider_theme_hue = view.findViewById(R.id.slider_theme_hue);
        tv_theme_hue_val = view.findViewById(R.id.tv_theme_hue_val);
        if (slider_theme_hue != null) {
            slider_theme_hue.setRainbowMode(true);
        }

        btn_gfx_preset_low = view.findViewById(R.id.btn_gfx_preset_low);
        btn_gfx_preset_med = view.findViewById(R.id.btn_gfx_preset_med);
        btn_gfx_preset_high = view.findViewById(R.id.btn_gfx_preset_high);
        btn_gfx_preset_ultra = view.findViewById(R.id.btn_gfx_preset_ultra);

        btn_open_hud_editor = view.findViewById(R.id.btn_open_hud_editor);
        btn_reset_hud_editor = view.findViewById(R.id.btn_reset_hud_editor);

        dropdown_aspect_ratio = view.findViewById(R.id.dropdown_aspect_ratio);
        dropdown_visual_fx = view.findViewById(R.id.dropdown_visual_fx);
        dropdown_shadows = view.findViewById(R.id.dropdown_shadows);
        dropdown_car_reflections = view.findViewById(R.id.dropdown_car_reflections);
        dropdown_traffic_mode = view.findViewById(R.id.dropdown_traffic_mode);
        dropdown_ped_cam_zoom = view.findViewById(R.id.dropdown_ped_cam_zoom);
        dropdown_veh_cam_height = view.findViewById(R.id.dropdown_veh_cam_height);
        dropdown_targeting_mode = view.findViewById(R.id.dropdown_targeting_mode);
        dropdown_touch_steering = view.findViewById(R.id.dropdown_touch_steering);
        dropdown_touch_layout = view.findViewById(R.id.dropdown_touch_layout);
        dropdown_accelerometer = view.findViewById(R.id.dropdown_accelerometer);
        dropdown_radio_station = view.findViewById(R.id.dropdown_radio_station);
        dropdown_voice_mic_mode = view.findViewById(R.id.dropdown_voice_mic_mode);
        dropdown_crosshair_style = view.findViewById(R.id.dropdown_crosshair_style);
        dropdown_font_family = view.findViewById(R.id.dropdown_font_family);

        voice = view.findViewById(R.id.voice_switch);
        timestamp = view.findViewById(R.id.timestamp_switch);
        fullscreen = view.findViewById(R.id.fullscreen_switch);
        displayfps = view.findViewById(R.id.displayfps_switch);
        systemkeyboard = view.findViewById(R.id.systemkeyboard_switch);
        modify_files = view.findViewById(R.id.modify_switch);
        fastconnect = view.findViewById(R.id.fastconnect_switch);

        headmove_switch = view.findViewById(R.id.headmove_switch);
        dlmodels_switch = view.findViewById(R.id.dlmodels_switch);
        switch_invert_look = view.findViewById(R.id.switch_invert_look);
        switch_auto_climb = view.findViewById(R.id.switch_auto_climb);
        switch_frame_limiter = view.findViewById(R.id.switch_frame_limiter);
        switch_free_cam = view.findViewById(R.id.switch_free_cam);
        switch_first_person_look = view.findViewById(R.id.switch_first_person_look);
        switch_bike_steering = view.findViewById(R.id.switch_bike_steering);
        switch_vibration = view.findViewById(R.id.switch_vibration);
        switch_radio_autotune = view.findViewById(R.id.switch_radio_autotune);

        switch_km_enabled = view.findViewById(R.id.switch_km_enabled);
        switch_joypad_invert_x = view.findViewById(R.id.switch_joypad_invert_x);
        switch_joypad_invert_y = view.findViewById(R.id.switch_joypad_invert_y);

        switch_voice_mic_enable = view.findViewById(R.id.switch_voice_mic_enable);
        switch_voice_speaker_list = view.findViewById(R.id.switch_voice_speaker_list);
        switch_voice_balancer = view.findViewById(R.id.switch_voice_balancer);
        switch_voice_filter = view.findViewById(R.id.switch_voice_filter);

        switch_show_gta_hud = view.findViewById(R.id.switch_show_gta_hud);
        switch_show_radar = view.findViewById(R.id.switch_show_radar);
        switch_radar_tap_map = view.findViewById(R.id.switch_radar_tap_map);
        switch_show_nametag_weapon = view.findViewById(R.id.switch_show_nametag_weapon);
        switch_show_death_window = view.findViewById(R.id.switch_show_death_window);
        switch_show_nametags = view.findViewById(R.id.switch_show_nametags);
        switch_show_button_panel = view.findViewById(R.id.switch_show_button_panel);
        switch_show_screenshot_btn = view.findViewById(R.id.switch_show_screenshot_btn);
        switch_gta_subtitles = view.findViewById(R.id.switch_gta_subtitles);
        switch_chat_shadow = view.findViewById(R.id.switch_chat_shadow);

        restore_button = view.findViewById(R.id.restore_button);
        chat_minus = view.findViewById(R.id.chat_minus);
        chat_strings = view.findViewById(R.id.chat_strings);
        chat_plus = view.findViewById(R.id.chat_plus);
        fps_limit = view.findViewById(R.id.fps_limit);

        font_size_slider = view.findViewById(R.id.fontsize_slider);
        font_size_value = view.findViewById(R.id.fontsize_value);
        chatposx_slider = view.findViewById(R.id.chatposx_slider);
        chatposx_value = view.findViewById(R.id.chatposx_value);
        chatposy_slider = view.findViewById(R.id.chatposy_slider);
        chatposy_value = view.findViewById(R.id.chatposy_value);
        chatsizex_slider = view.findViewById(R.id.chatsizex_slider);
        chatsizex_value = view.findViewById(R.id.chatsizex_value);
        chatsizey_slider = view.findViewById(R.id.chatsizey_slider);
        chatsizey_value = view.findViewById(R.id.chatsizey_value);

        slider_resolution = view.findViewById(R.id.slider_resolution);
        tv_resolution_val = view.findViewById(R.id.tv_resolution_val);
        slider_draw_distance = view.findViewById(R.id.slider_draw_distance);
        tv_draw_distance_val = view.findViewById(R.id.tv_draw_distance_val);
        slider_brightness = view.findViewById(R.id.slider_brightness);
        tv_brightness_val = view.findViewById(R.id.tv_brightness_val);

        slider_cam_sens_x = view.findViewById(R.id.slider_cam_sens_x);
        tv_cam_sens_x_val = view.findViewById(R.id.tv_cam_sens_x_val);
        slider_cam_sens_y = view.findViewById(R.id.slider_cam_sens_y);
        tv_cam_sens_y_val = view.findViewById(R.id.tv_cam_sens_y_val);
        slider_cam_sens_foot = view.findViewById(R.id.slider_cam_sens_foot);
        tv_cam_sens_foot_val = view.findViewById(R.id.tv_cam_sens_foot_val);
        slider_cam_sens_veh = view.findViewById(R.id.slider_cam_sens_veh);
        tv_cam_sens_veh_val = view.findViewById(R.id.tv_cam_sens_veh_val);
        slider_cam_sens_aim = view.findViewById(R.id.slider_cam_sens_aim);
        tv_cam_sens_aim_val = view.findViewById(R.id.tv_cam_sens_aim_val);

        slider_joystick_size = view.findViewById(R.id.slider_joystick_size);
        tv_joystick_size_val = view.findViewById(R.id.tv_joystick_size_val);
        slider_shoot_threshold = view.findViewById(R.id.slider_shoot_threshold);
        tv_shoot_threshold_val = view.findViewById(R.id.tv_shoot_threshold_val);
        slider_analog_sens = view.findViewById(R.id.slider_analog_sens);
        tv_analog_sens_val = view.findViewById(R.id.tv_analog_sens_val);
        slider_flick_sens = view.findViewById(R.id.slider_flick_sens);
        tv_flick_sens_val = view.findViewById(R.id.tv_flick_sens_val);

        slider_sfx_volume = view.findViewById(R.id.slider_sfx_volume);
        tv_sfx_volume_val = view.findViewById(R.id.tv_sfx_volume_val);
        slider_music_volume = view.findViewById(R.id.slider_music_volume);
        tv_music_volume_val = view.findViewById(R.id.tv_music_volume_val);
        slider_voice_volume = view.findViewById(R.id.slider_voice_volume);
        tv_voice_volume_val = view.findViewById(R.id.tv_voice_volume_val);
        slider_voice_mic_volume = view.findViewById(R.id.slider_voice_mic_volume);
        tv_voice_mic_volume_val = view.findViewById(R.id.tv_voice_mic_volume_val);
        slider_voice_btn_size = view.findViewById(R.id.slider_voice_btn_size);
        tv_voice_btn_size_val = view.findViewById(R.id.tv_voice_btn_size_val);

        slider_nametag_size = view.findViewById(R.id.slider_nametag_size);
        tv_nametag_size_val = view.findViewById(R.id.tv_nametag_size_val);
        slider_textlabel_size = view.findViewById(R.id.slider_textlabel_size);
        tv_textlabel_size_val = view.findViewById(R.id.tv_textlabel_size_val);

        // 截图库
        btn_ss_mode_hud = view.findViewById(R.id.btn_ss_mode_hud);
        btn_ss_mode_game = view.findViewById(R.id.btn_ss_mode_game);
        tv_screenshots_summary = view.findViewById(R.id.tv_screenshots_summary);
        tv_screenshots_empty = view.findViewById(R.id.tv_screenshots_empty);
        rv_screenshots = view.findViewById(R.id.rv_screenshots);
        rv_screenshots.setLayoutManager(new GridLayoutManager(requireContext(), 2));
        screenshotAdapter = new ScreenshotAdapter();
        rv_screenshots.setAdapter(screenshotAdapter);

        // 问题反馈
        tv_diag_status_title = view.findViewById(R.id.tv_diag_status_title);
        tv_diag_status_desc = view.findViewById(R.id.tv_diag_status_desc);

        // 帮助与关于
        tv_help_mods_body = view.findViewById(R.id.tv_help_mods_body);
        tv_help_faq_body = view.findViewById(R.id.tv_help_faq_body);
        tv_help_commands = view.findViewById(R.id.tv_help_commands);
        tv_about_version_line = view.findViewById(R.id.tv_about_version_line);
        tv_about_summary = view.findViewById(R.id.tv_about_summary);

        // 文件验证与游戏资源选项卡
        option_gd_edition = view.findViewById(R.id.option_gd_edition);
        tv_gd_summary = view.findViewById(R.id.tv_gd_summary);
        tv_gd_storage_meta = view.findViewById(R.id.tv_gd_storage_meta);
        tv_gd_list_summary = view.findViewById(R.id.tv_gd_list_summary);
        tv_gd_list_empty = view.findViewById(R.id.tv_gd_list_empty);
        btn_gd_import = view.findViewById(R.id.btn_gd_import);
        btn_gd_download = view.findViewById(R.id.btn_gd_download);
        btn_gd_verify = view.findViewById(R.id.btn_gd_verify);
        btn_gd_filter_all = view.findViewById(R.id.btn_gd_filter_all);
        btn_gd_filter_issues = view.findViewById(R.id.btn_gd_filter_issues);
        layout_gd_download_progress = view.findViewById(R.id.layout_gd_download_progress);
        tv_gd_dl_status = view.findViewById(R.id.tv_gd_dl_status);
        tv_gd_dl_bytes = view.findViewById(R.id.tv_gd_dl_bytes);
        pb_gd_download = view.findViewById(R.id.pb_gd_download);
        rv_gd_files = view.findViewById(R.id.rv_gd_files);
        if (rv_gd_files != null) {
            rv_gd_files.setLayoutManager(new LinearLayoutManager(requireContext()));
            gameDataFileAdapter = new GameDataFileAdapter();
            rv_gd_files.setAdapter(gameDataFileAdapter);
        }
        setupGameDataControls();
        refreshGameDataVerification();
    }

    private void setupCategoryNavigation(View view) {
        categoryPanels = new View[]{
                view.findViewById(R.id.panel_multiplayer),
                view.findViewById(R.id.panel_gamedata),
                view.findViewById(R.id.panel_graphics),
                view.findViewById(R.id.panel_camera),
                view.findViewById(R.id.panel_controls),
                view.findViewById(R.id.panel_audio),
                view.findViewById(R.id.panel_hud_chat),
                view.findViewById(R.id.panel_screenshots),
                view.findViewById(R.id.panel_logs),
                view.findViewById(R.id.panel_help_about)
        };

        settingsTabs = view.findViewById(R.id.settings_tabs);
        settingsTabs.setTabs(CATEGORY_TITLES);
        settingsTabs.setOnTabSelectedListener((index, reselected) -> {
            if (reselected) {
                if (categoryPanels[index] instanceof android.widget.ScrollView) {
                    ((android.widget.ScrollView) categoryPanels[index]).smoothScrollTo(0, 0);
                }
                return;
            }
            switchCategory(index);
            View panel = categoryPanels[index];
            panel.animate().cancel();
            panel.setAlpha(0f);
            panel.setTranslationY(8f * getResources().getDisplayMetrics().density);
            panel.animate().alpha(1f).translationY(0f).setDuration(180).start();
        });
        switchCategory(CAT_MULTIPLAYER);
    }

    private void switchCategory(int catIdx) {
        currentCategory = catIdx;

        for (int i = 0; i < categoryPanels.length; i++) {
            categoryPanels[i].setVisibility((i == catIdx) ? View.VISIBLE : View.GONE);
        }

        if (catIdx == CAT_GAMEDATA) {
            refreshGameDataVerification();
        } else if (catIdx == CAT_SCREENSHOTS) {
            refreshScreenshotGallery();
        } else if (catIdx == CAT_LOGS) {
            refreshDiagnosticStatus();
        } else if (catIdx == CAT_HELP_ABOUT) {
            populateHelpAndAbout();
        }
    }

    private void setupListeners(View view) {
        et_settings_nickname.setText(settings_prefs.getString("nick_name", "samp_player"));
        et_settings_nickname.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {}

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {}

            @Override
            public void afterTextChanged(Editable s) {
                String nick = s.toString().trim();
                if (!nick.isEmpty()) {
                    savePreference("nick_name", nick);
                }
            }
        });

        // 启动器主题配色预设与自定义滑条
        int[][] themePresets = {
                {R.id.btn_theme_gold, 38},
                {R.id.btn_theme_cyan, 186},
                {R.id.btn_theme_green, 145},
                {R.id.btn_theme_red, 355},
                {R.id.btn_theme_purple, 271},
                {R.id.btn_theme_blue, 216}
        };
        for (int[] preset : themePresets) {
            View btn = view.findViewById(preset[0]);
            final int hue = preset[1];
            if (btn != null) {
                btn.setOnClickListener(v -> {
                    if (slider_theme_hue != null) slider_theme_hue.setValue(hue);
                    if (tv_theme_hue_val != null) tv_theme_hue_val.setText(hue + "°");
                    LauncherThemeManager.setHue(requireContext(), hue);
                });
            }
        }
        if (slider_theme_hue != null) {
            int savedHue = LauncherThemeManager.getHue(requireContext());
            slider_theme_hue.setValue(savedHue);
            if (tv_theme_hue_val != null) tv_theme_hue_val.setText(savedHue + "°");
            slider_theme_hue.addOnChangeListener((s, value, fromUser) -> {
                int hue = Math.round(value);
                if (tv_theme_hue_val != null) tv_theme_hue_val.setText(hue + "°");
                if (fromUser) {
                    LauncherThemeManager.setHue(requireContext(), hue);
                }
            });
        }

        bindIndexedDropdown(samp_version, SAMP_VERSION_ITEMS, "samp_version", 0);

        ArrayAdapter<String> charsetAdapter = createDropdownAdapter(CHARSET_ITEMS);
        charset.setAdapter(charsetAdapter);
        charset.setThreshold(0);
        charset.setText(settings_prefs.getString("charset", "GBK"), false);
        charset.setOnClickListener(v -> charset.showDropDown());
        charset.setOnItemClickListener((adapterView, v, i, l) -> savePreference("charset", CHARSET_ITEMS[i]));

        // 帧率上限（默认 0 = 无限制）
        ArrayAdapter<String> fpsAdapter = createDropdownAdapter(FPS_ITEMS);
        fps_limit.setAdapter(fpsAdapter);
        fps_limit.setThreshold(0);
        fps_limit.setText(fpsValueToLabel(settings_prefs.getInt("fps_limit", 0)), false);
        fps_limit.setOnClickListener(v -> fps_limit.showDropDown());
        fps_limit.setOnItemClickListener((adapterView, v, i, l) -> {
            if (i >= 0 && i < FPS_VALUES.length) {
                savePreference("fps_limit", FPS_VALUES[i]);
            }
        });

        savePreference("game_version", 0);
        if (game_version_layout != null) {
            game_version_layout.setVisibility(View.GONE);
        }

        bindIndexedDropdown(dropdown_aspect_ratio, ASPECT_RATIO_ITEMS, "aspect_ratio", 0);
        bindIndexedDropdownWithPresetSync(dropdown_visual_fx, VISUAL_FX_ITEMS, "gta_visual_fx", 2);
        bindIndexedDropdownWithPresetSync(dropdown_shadows, SHADOWS_ITEMS, "gta_shadows", 1);
        bindIndexedDropdownWithPresetSync(dropdown_car_reflections, CAR_REFL_ITEMS, "gta_car_reflections", 2);
        bindIndexedDropdown(dropdown_traffic_mode, TRAFFIC_ITEMS, "gta_traffic_mode", 2);
        bindIndexedDropdown(dropdown_veh_cam_height, VEH_CAM_ITEMS, "veh_cam_height", 1);
        bindIndexedDropdown(dropdown_targeting_mode, TARGETING_ITEMS, "targeting_mode", 0);
        bindIndexedDropdown(dropdown_touch_steering, STEERING_ITEMS, "touch_steering", 0);
        bindIndexedDropdown(dropdown_touch_layout, LAYOUT_ITEMS, "touch_layout", 0);
        bindIndexedDropdown(dropdown_accelerometer, ACCEL_ITEMS, "accelerometer", 0);
        bindIndexedDropdown(dropdown_radio_station, RADIO_STATION_ITEMS, "radio_station", 0);
        bindIndexedDropdown(dropdown_voice_mic_mode, VOICE_MIC_MODE_ITEMS, "voice_mic_mode", 0);
        bindIndexedDropdown(dropdown_crosshair_style, CROSSHAIR_STYLE_ITEMS, "crosshair_style", 0);
        bindIndexedDropdown(dropdown_font_family, FONT_FAMILY_ITEMS, "font_family", 0);

        // 步战视角距离 1..3
        ArrayAdapter<String> pedZoomAdapter = createDropdownAdapter(PED_ZOOM_ITEMS);
        dropdown_ped_cam_zoom.setAdapter(pedZoomAdapter);
        dropdown_ped_cam_zoom.setThreshold(0);
        int zoomIdx = Math.min(Math.max(settings_prefs.getInt("ped_cam_zoom", 2) - 1, 0), PED_ZOOM_ITEMS.length - 1);
        dropdown_ped_cam_zoom.setText(PED_ZOOM_ITEMS[zoomIdx], false);
        dropdown_ped_cam_zoom.setOnClickListener(v -> dropdown_ped_cam_zoom.showDropDown());
        dropdown_ped_cam_zoom.setOnItemClickListener((av, v, i, l) -> savePreference("ped_cam_zoom", i + 1));

        // 四档画质预设
        btn_gfx_preset_low.setOnClickListener(v -> applyGraphicsPreset(0, 0, 0, 60, 40, "省电流畅"));
        btn_gfx_preset_med.setOnClickListener(v -> applyGraphicsPreset(1, 1, 1, 80, 60, "均衡标准"));
        btn_gfx_preset_high.setOnClickListener(v -> applyGraphicsPreset(2, 1, 2, 100, 75, "高清画质"));
        btn_gfx_preset_ultra.setOnClickListener(v -> applyGraphicsPreset(3, 2, 3, 100, 100, "极致画质"));

        // 自定义游戏按键位置 (Change Button Position)
        if (btn_open_hud_editor != null) {
            btn_open_hud_editor.setOnClickListener(v -> {
                boolean next = !settings_prefs.getBoolean("open_hud_edit_on_start", false);
                savePreference("open_hud_edit_on_start", next);
                updateHudEditorButtonUI();
                if (next) {
                    Toast.makeText(requireContext(), "已开启：下次进入游戏将自动打开按键位置调整界面（游戏内也可随时点击底部 HUD 键或输入 /hudedit 打开）", Toast.LENGTH_LONG).show();
                } else {
                    Toast.makeText(requireContext(), "已取消进服自动打开按键位置调整", Toast.LENGTH_SHORT).show();
                }
            });
        }
        if (btn_reset_hud_editor != null) {
            btn_reset_hud_editor.setOnClickListener(v -> {
                showAlertDialog("确定要删除自定义按键布局文件 (Adjustable.cfg)，恢复 GTA:SA 原版默认按键位置吗？", (d, w) -> {
                    File adjFile = new File(requireContext().getExternalFilesDir(null), "Adjustable.cfg");
                    if (adjFile.exists()) {
                        adjFile.delete();
                    }
                    savePreference("open_hud_edit_on_start", false);
                    updateHudEditorButtonUI();
                    Toast.makeText(requireContext(), "已恢复默认游戏按键位置", Toast.LENGTH_SHORT).show();
                    d.dismiss();
                }, (d, w) -> d.dismiss());
            });
        }

        // 重置聊天框布局
        view.findViewById(R.id.btn_reset_chat_layout).setOnClickListener(v -> {
            savePreference("font_size", 25.0f);
            savePreference("chat_strings", 8);
            savePreference("chat_posx", 100);
            savePreference("chat_posy", 20);
            savePreference("chat_sizex", 360);
            savePreference("chat_sizey", 110);
            setWidgetsForSettings();
            Toast.makeText(requireContext(), "聊天窗口布局与字号已恢复默认", Toast.LENGTH_SHORT).show();
        });

        // 截图模式切换
        btn_ss_mode_hud.setOnClickListener(v -> {
            savePreference("screenshot_mode", 0);
            updateScreenshotModeUI();
            Toast.makeText(requireContext(), "已设为：含界面与聊天", Toast.LENGTH_SHORT).show();
        });
        btn_ss_mode_game.setOnClickListener(v -> {
            savePreference("screenshot_mode", 1);
            updateScreenshotModeUI();
            Toast.makeText(requireContext(), "已设为：仅游戏画面", Toast.LENGTH_SHORT).show();
        });
        view.findViewById(R.id.btn_ss_refresh).setOnClickListener(v -> refreshScreenshotGallery());
        view.findViewById(R.id.btn_ss_clear_all).setOnClickListener(v -> confirmClearScreenshots());

        // 问题反馈操作按钮
        view.findViewById(R.id.btn_log_refresh).setOnClickListener(v -> {
            refreshDiagnosticStatus();
            Toast.makeText(requireContext(), "运行状态检测已刷新", Toast.LENGTH_SHORT).show();
        });
        view.findViewById(R.id.btn_log_copy_report).setOnClickListener(v -> copyDiagnosticReport());
        view.findViewById(R.id.btn_log_share).setOnClickListener(v -> shareDiagnosticLogFile());
        view.findViewById(R.id.btn_log_clear).setOnClickListener(v -> confirmClearLogs());

        setupSwitchListeners();
        setupRestoreButton();
        setupChatButtons();
        setupSliders();
        updateGraphicsPresetUI();
        updateScreenshotModeUI();
        updateHudEditorButtonUI();
    }

    private String fpsValueToLabel(int fps) {
        if (fps <= 0) return "无限制";
        return String.valueOf(fps);
    }

    private ArrayAdapter<String> createDropdownAdapter(final String[] items) {
        return new ArrayAdapter<String>(requireContext(), R.layout.layout_setting_version, new ArrayList<>(Arrays.asList(items))) {
            private final Filter noOpFilter = new Filter() {
                @Override
                protected FilterResults performFiltering(CharSequence constraint) {
                    FilterResults results = new FilterResults();
                    results.values = Arrays.asList(items);
                    results.count = items.length;
                    return results;
                }

                @Override
                protected void publishResults(CharSequence constraint, FilterResults results) {
                    clear();
                    addAll(items);
                    notifyDataSetChanged();
                }
            };

            @NonNull
            @Override
            public Filter getFilter() {
                return noOpFilter;
            }
        };
    }

    private void bindIndexedDropdown(GtaOptionView view, String[] items, String key, int defIdx) {
        if (view == null) return;
        ArrayAdapter<String> adapter = createDropdownAdapter(items);
        view.setAdapter(adapter);
        view.setThreshold(0);
        int idx = Math.min(Math.max(settings_prefs.getInt(key, defIdx), 0), items.length - 1);
        view.setText(items[idx], false);
        view.setOnClickListener(v -> view.showDropDown());
        view.setOnItemClickListener((av, v, i, l) -> savePreference(key, i));
    }

    private void bindIndexedDropdownWithPresetSync(GtaOptionView view, String[] items, String key, int defIdx) {
        if (view == null) return;
        ArrayAdapter<String> adapter = createDropdownAdapter(items);
        view.setAdapter(adapter);
        view.setThreshold(0);
        int idx = Math.min(Math.max(settings_prefs.getInt(key, defIdx), 0), items.length - 1);
        view.setText(items[idx], false);
        view.setOnClickListener(v -> view.showDropDown());
        view.setOnItemClickListener((av, v, i, l) -> {
            savePreference(key, i);
            updateGraphicsPresetUI();
        });
    }

    private void applyGraphicsPreset(int vfx, int shadows, int carRefl, int res, int dd, String label) {
        savePreference("gta_visual_fx", vfx);
        savePreference("gta_shadows", shadows);
        savePreference("gta_car_reflections", carRefl);
        savePreference("gta_resolution", res);
        savePreference("gta_draw_distance", dd);
        setWidgetsForSettings();
        Toast.makeText(requireContext(), "已应用画质预设：" + label, Toast.LENGTH_SHORT).show();
    }

    private void updateGraphicsPresetUI() {
        if (btn_gfx_preset_low == null || !isAdded()) return;
        int vfx = settings_prefs.getInt("gta_visual_fx", 2);
        int shd = settings_prefs.getInt("gta_shadows", 1);
        int refl = settings_prefs.getInt("gta_car_reflections", 2);
        int res = settings_prefs.getInt("gta_resolution", 100);
        int dd = settings_prefs.getInt("gta_draw_distance", 70);

        int activePreset = -1;
        if (vfx == 0 && shd == 0 && refl == 0 && res == 60 && dd == 40) activePreset = 0;
        else if (vfx == 1 && shd == 1 && refl == 1 && res == 80 && dd == 60) activePreset = 1;
        else if (vfx == 2 && shd == 1 && refl == 2 && res == 100 && dd == 75) activePreset = 2;
        else if (vfx == 3 && shd == 2 && refl == 3 && res == 100 && dd == 100) activePreset = 3;

        TextView[] btns = {btn_gfx_preset_low, btn_gfx_preset_med, btn_gfx_preset_high, btn_gfx_preset_ultra};
        int accentColor = LauncherThemeManager.getAccentColor(requireContext());
        int colorOnAccent = requireContext().getResources().getColor(R.color.launcher_on_accent);
        int colorText = requireContext().getResources().getColor(R.color.launcher_text);

        for (int i = 0; i < btns.length; i++) {
            boolean selected = (i == activePreset);
            btns[i].setBackgroundResource(selected ? R.drawable.launcher_btn_primary : R.drawable.launcher_btn_outline);
            btns[i].setBackgroundTintList(selected ? ColorStateList.valueOf(accentColor) : null);
            btns[i].setTextColor(selected ? colorOnAccent : colorText);
        }
    }

    private void updateHudEditorButtonUI() {
        if (btn_open_hud_editor == null || !isAdded()) return;
        boolean scheduled = settings_prefs.getBoolean("open_hud_edit_on_start", false);
        int accentColor = LauncherThemeManager.getAccentColor(requireContext());
        btn_open_hud_editor.setBackgroundTintList(ColorStateList.valueOf(accentColor));
        btn_open_hud_editor.setText(scheduled
                ? "✓ 已预约：下次进入游戏将自动打开按键调整"
                : "进入游戏时自动打开按键位置调整");
    }

    private void setupSliders() {
        bindSlider(font_size_slider, font_size_value, "font_size", 25, true, "", false);
        bindSlider(chatposx_slider, chatposx_value, "chat_posx", 100, false, "", false);
        bindSlider(chatposy_slider, chatposy_value, "chat_posy", 20, false, "", false);
        bindSlider(chatsizex_slider, chatsizex_value, "chat_sizex", 360, false, "", false);
        bindSlider(chatsizey_slider, chatsizey_value, "chat_sizey", 110, false, "", false);

        bindSlider(slider_resolution, tv_resolution_val, "gta_resolution", 100, false, "%", true);
        bindSlider(slider_draw_distance, tv_draw_distance_val, "gta_draw_distance", 70, false, "%", true);
        bindSlider(slider_brightness, tv_brightness_val, "gta_brightness", 60, false, "%", false);

        bindSlider(slider_cam_sens_x, tv_cam_sens_x_val, "cam_sens_x", 50, false, "%", false);
        bindSlider(slider_cam_sens_y, tv_cam_sens_y_val, "cam_sens_y", 50, false, "%", false);
        bindSlider(slider_cam_sens_foot, tv_cam_sens_foot_val, "cam_sens_foot", 100, false, "%", false);
        bindSlider(slider_cam_sens_veh, tv_cam_sens_veh_val, "cam_sens_veh", 100, false, "%", false);
        bindSlider(slider_cam_sens_aim, tv_cam_sens_aim_val, "cam_sens_aim", 70, false, "%", false);

        bindSlider(slider_joystick_size, tv_joystick_size_val, "joystick_size", 100, false, "%", false);
        bindSlider(slider_shoot_threshold, tv_shoot_threshold_val, "shoot_threshold", 50, false, "%", false);
        bindSlider(slider_analog_sens, tv_analog_sens_val, "analog_sensitivity", 50, false, "%", false);
        bindSlider(slider_flick_sens, tv_flick_sens_val, "flick_sensitivity", 50, false, "%", false);

        bindSlider(slider_sfx_volume, tv_sfx_volume_val, "sfx_volume", 80, false, "%", false);
        bindSlider(slider_music_volume, tv_music_volume_val, "music_volume", 80, false, "%", false);
        bindSlider(slider_voice_volume, tv_voice_volume_val, "voice_volume", 100, false, "%", false);
        bindSlider(slider_voice_mic_volume, tv_voice_mic_volume_val, "voice_mic_volume", 75, false, "%", false);
        bindSlider(slider_voice_btn_size, tv_voice_btn_size_val, "voice_btn_size", 100, false, "%", false);

        bindSlider(slider_nametag_size, tv_nametag_size_val, "nametag_size", 100, false, "%", false);
        bindSlider(slider_textlabel_size, tv_textlabel_size_val, "textlabel_size", 100, false, "%", false);
    }

    private void bindSlider(GtaSaSliderView slider, TextView valueView, String key, int def,
                            boolean asFloat, String suffix, boolean syncPreset) {
        if (slider == null || valueView == null) return;
        slider.clearOnChangeListeners();
        slider.addOnChangeListener((s, value, fromUser) -> {
            valueView.setText(Math.round(value) + suffix);
            if (fromUser) {
                if (asFloat) savePreference(key, value);
                else savePreference(key, Math.round(value));
                if (syncPreset) {
                    updateGraphicsPresetUI();
                }
            }
        });
        syncSlider(slider, valueView, key, def, asFloat, suffix);
    }

    private void syncSlider(GtaSaSliderView slider, TextView valueView, String key, int def, boolean asFloat, String suffix) {
        if (slider == null || valueView == null) return;
        float current = asFloat ? settings_prefs.getFloat(key, def) : settings_prefs.getInt(key, def);
        float clamped = Math.min(Math.max(current, slider.getValueFrom()), slider.getValueTo());
        float step = slider.getStepSize();
        if (step > 0) {
            clamped = slider.getValueFrom() + Math.round((clamped - slider.getValueFrom()) / step) * step;
            clamped = Math.min(Math.max(clamped, slider.getValueFrom()), slider.getValueTo());
        }
        slider.setValue(clamped);
        valueView.setText(Math.round(clamped) + suffix);
    }

    private boolean getDefaultBool(String key) {
        switch (key) {
            case "system_keyboard":
            case "head_move":
            case "auto_climb":
            case "dl_custom_models":
            case "first_person_look":
            case "vibration":
            case "km_enabled":
            case "radio_autotune":
            case "voice_mic_enable":
            case "voice_filter":
            case "voice_speaker_list":
            case "show_gta_hud":
            case "show_radar":
            case "radar_tap_map":
            case "show_death_window":
            case "show_nametags":
            case "show_button_panel":
            case "show_screenshot_btn":
            case "gta_subtitles":
            case "chat_shadow":
                return true;
            default:
                return false;
        }
    }

    private Map<CompoundButton, String> buildSwitchMap() {
        Map<CompoundButton, String> map = new HashMap<>();
        map.put(systemkeyboard, "system_keyboard");
        map.put(timestamp, "timestamp");
        map.put(fullscreen, "fullscreen");
        map.put(displayfps, "display_fps");
        map.put(voice, "voice_chat");
        map.put(fastconnect, "fast_connect");
        map.put(headmove_switch, "head_move");
        map.put(dlmodels_switch, "dl_custom_models");
        map.put(switch_invert_look, "invert_look");
        map.put(switch_auto_climb, "auto_climb");
        map.put(switch_frame_limiter, "frame_limiter");
        map.put(switch_free_cam, "free_cam");
        map.put(switch_first_person_look, "first_person_look");
        map.put(switch_bike_steering, "bike_steering");
        map.put(switch_vibration, "vibration");
        map.put(switch_radio_autotune, "radio_autotune");
        map.put(switch_km_enabled, "km_enabled");
        map.put(switch_joypad_invert_x, "joypad_invert_x");
        map.put(switch_joypad_invert_y, "joypad_invert_y");
        map.put(switch_voice_mic_enable, "voice_mic_enable");
        map.put(switch_voice_speaker_list, "voice_speaker_list");
        map.put(switch_voice_balancer, "voice_balancer");
        map.put(switch_voice_filter, "voice_filter");
        map.put(switch_show_gta_hud, "show_gta_hud");
        map.put(switch_show_radar, "show_radar");
        map.put(switch_radar_tap_map, "radar_tap_map");
        map.put(switch_show_nametag_weapon, "show_nametag_weapon");
        map.put(switch_show_death_window, "show_death_window");
        map.put(switch_show_nametags, "show_nametags");
        map.put(switch_show_button_panel, "show_button_panel");
        map.put(switch_show_screenshot_btn, "show_screenshot_btn");
        map.put(switch_gta_subtitles, "gta_subtitles");
        map.put(switch_chat_shadow, "chat_shadow");
        return map;
    }

    private void setupSwitchListeners() {
        for (Map.Entry<CompoundButton, String> entry : buildSwitchMap().entrySet()) {
            setupSwitch(entry.getKey(), entry.getValue());
        }
        setupSwitchWithWarning(modify_files);
    }

    private void setupSwitch(CompoundButton switchButton, String key) {
        if (switchButton == null) return;
        switchButton.setOnCheckedChangeListener(null);
        switchButton.setChecked(settings_prefs.getBoolean(key, getDefaultBool(key)));
        switchButton.setOnCheckedChangeListener((buttonView, isChecked) -> savePreference(key, isChecked));
    }

    private void setupSwitchWithWarning(CompoundButton switchButton) {
        if (switchButton == null) return;
        switchButton.setOnCheckedChangeListener(null);
        switchButton.setChecked(settings_prefs.getBoolean("modify_files", false));

        switchButton.setOnCheckedChangeListener((buttonView, isChecked) -> {
            if (isChecked && buttonView.isPressed()) {
                showAlertDialog("开启此选项后，启动器将跳过原始文件完整性检查。若因手工替换数据包导致游戏异常，请关闭此选项。",
                        (dialog, which) -> dialog.dismiss(), (dialog, which) -> {
                            switchButton.setChecked(false);
                            dialog.dismiss();
                        });
            }
            savePreference("modify_files", isChecked);
        });
    }

    private void setupRestoreButton() {
        restore_button.setOnClickListener(v -> showAlertDialog("确定要将所有联机、画面、镜头、操作、音频与界面设置恢复为默认值吗？", (dialog, which) -> {
            Utils.restoreSettings(requireContext());
            LauncherThemeManager.setHue(requireContext(), LauncherThemeManager.DEFAULT_HUE);
            setWidgetsForSettings();
            Toast.makeText(requireContext(), "已恢复全部默认设置", Toast.LENGTH_SHORT).show();
            dialog.dismiss();
        }, (dialog, which) -> dialog.dismiss()));
    }

    private void setupChatButtons() {
        chat_minus.setOnClickListener(v -> {
            adjustChatStrings(-1);
        });
        chat_plus.setOnClickListener(v -> {
            adjustChatStrings(1);
        });
    }

    private void adjustChatStrings(int delta) {
        int currentValue = settings_prefs.getInt("chat_strings", 8);
        int newValue = currentValue + delta;
        if (newValue < 5 || newValue > 20) return;

        chat_strings.setText(String.valueOf(newValue));
        savePreference("chat_strings", newValue);
    }

    private void setWidgetsForSettings() {
        et_settings_nickname.setText(settings_prefs.getString("nick_name", "samp_player"));

        for (Map.Entry<CompoundButton, String> entry : buildSwitchMap().entrySet()) {
            if (entry.getKey() != null) {
                entry.getKey().setChecked(settings_prefs.getBoolean(entry.getValue(), getDefaultBool(entry.getValue())));
            }
        }
        modify_files.setChecked(settings_prefs.getBoolean("modify_files", false));
        chat_strings.setText(String.valueOf(settings_prefs.getInt("chat_strings", 8)));

        int hue = LauncherThemeManager.getHue(requireContext());
        if (slider_theme_hue != null) slider_theme_hue.setValue(hue);
        if (tv_theme_hue_val != null) tv_theme_hue_val.setText(hue + "°");

        samp_version.setText(SAMP_VERSION_ITEMS[Math.min(Math.max(settings_prefs.getInt("samp_version", 0), 0), SAMP_VERSION_ITEMS.length - 1)], false);
        charset.setText(settings_prefs.getString("charset", "GBK"), false);
        fps_limit.setText(fpsValueToLabel(settings_prefs.getInt("fps_limit", 0)), false);

        setDropdownIdxText(dropdown_aspect_ratio, ASPECT_RATIO_ITEMS, "aspect_ratio", 0);
        setDropdownIdxText(dropdown_visual_fx, VISUAL_FX_ITEMS, "gta_visual_fx", 2);
        setDropdownIdxText(dropdown_shadows, SHADOWS_ITEMS, "gta_shadows", 1);
        setDropdownIdxText(dropdown_car_reflections, CAR_REFL_ITEMS, "gta_car_reflections", 2);
        setDropdownIdxText(dropdown_traffic_mode, TRAFFIC_ITEMS, "gta_traffic_mode", 2);
        dropdown_ped_cam_zoom.setText(PED_ZOOM_ITEMS[Math.min(Math.max(settings_prefs.getInt("ped_cam_zoom", 2) - 1, 0), PED_ZOOM_ITEMS.length - 1)], false);
        setDropdownIdxText(dropdown_veh_cam_height, VEH_CAM_ITEMS, "veh_cam_height", 1);
        setDropdownIdxText(dropdown_targeting_mode, TARGETING_ITEMS, "targeting_mode", 0);
        setDropdownIdxText(dropdown_touch_steering, STEERING_ITEMS, "touch_steering", 0);
        setDropdownIdxText(dropdown_touch_layout, LAYOUT_ITEMS, "touch_layout", 0);
        setDropdownIdxText(dropdown_accelerometer, ACCEL_ITEMS, "accelerometer", 0);
        setDropdownIdxText(dropdown_radio_station, RADIO_STATION_ITEMS, "radio_station", 0);
        setDropdownIdxText(dropdown_voice_mic_mode, VOICE_MIC_MODE_ITEMS, "voice_mic_mode", 0);
        setDropdownIdxText(dropdown_crosshair_style, CROSSHAIR_STYLE_ITEMS, "crosshair_style", 0);
        setDropdownIdxText(dropdown_font_family, FONT_FAMILY_ITEMS, "font_family", 0);

        syncSlider(font_size_slider, font_size_value, "font_size", 25, true, "");
        syncSlider(chatposx_slider, chatposx_value, "chat_posx", 100, false, "");
        syncSlider(chatposy_slider, chatposy_value, "chat_posy", 20, false, "");
        syncSlider(chatsizex_slider, chatsizex_value, "chat_sizex", 360, false, "");
        syncSlider(chatsizey_slider, chatsizey_value, "chat_sizey", 110, false, "");

        syncSlider(slider_resolution, tv_resolution_val, "gta_resolution", 100, false, "%");
        syncSlider(slider_draw_distance, tv_draw_distance_val, "gta_draw_distance", 70, false, "%");
        syncSlider(slider_brightness, tv_brightness_val, "gta_brightness", 60, false, "%");

        syncSlider(slider_cam_sens_x, tv_cam_sens_x_val, "cam_sens_x", 50, false, "%");
        syncSlider(slider_cam_sens_y, tv_cam_sens_y_val, "cam_sens_y", 50, false, "%");
        syncSlider(slider_cam_sens_foot, tv_cam_sens_foot_val, "cam_sens_foot", 100, false, "%");
        syncSlider(slider_cam_sens_veh, tv_cam_sens_veh_val, "cam_sens_veh", 100, false, "%");
        syncSlider(slider_cam_sens_aim, tv_cam_sens_aim_val, "cam_sens_aim", 70, false, "%");

        syncSlider(slider_joystick_size, tv_joystick_size_val, "joystick_size", 100, false, "%");
        syncSlider(slider_shoot_threshold, tv_shoot_threshold_val, "shoot_threshold", 50, false, "%");
        syncSlider(slider_analog_sens, tv_analog_sens_val, "analog_sensitivity", 50, false, "%");
        syncSlider(slider_flick_sens, tv_flick_sens_val, "flick_sensitivity", 50, false, "%");

        syncSlider(slider_sfx_volume, tv_sfx_volume_val, "sfx_volume", 80, false, "%");
        syncSlider(slider_music_volume, tv_music_volume_val, "music_volume", 80, false, "%");
        syncSlider(slider_voice_volume, tv_voice_volume_val, "voice_volume", 100, false, "%");
        syncSlider(slider_voice_mic_volume, tv_voice_mic_volume_val, "voice_mic_volume", 75, false, "%");
        syncSlider(slider_voice_btn_size, tv_voice_btn_size_val, "voice_btn_size", 100, false, "%");

        syncSlider(slider_nametag_size, tv_nametag_size_val, "nametag_size", 100, false, "%");
        syncSlider(slider_textlabel_size, tv_textlabel_size_val, "textlabel_size", 100, false, "%");

        updateGraphicsPresetUI();
        updateScreenshotModeUI();
        updateHudEditorButtonUI();
    }

    private void setDropdownIdxText(GtaOptionView view, String[] items, String key, int defIdx) {
        if (view == null) return;
        int idx = Math.min(Math.max(settings_prefs.getInt(key, defIdx), 0), items.length - 1);
        view.setText(items[idx], false);
    }

    private void updateScreenshotModeUI() {
        if (btn_ss_mode_hud == null || btn_ss_mode_game == null || !isAdded()) return;
        int mode = settings_prefs.getInt("screenshot_mode", 0);
        int accentColor = LauncherThemeManager.getAccentColor(requireContext());
        int colorOnAccent = requireContext().getResources().getColor(R.color.launcher_on_accent);
        int colorText = requireContext().getResources().getColor(R.color.launcher_text);

        boolean isHud = (mode == 0);
        btn_ss_mode_hud.setBackgroundResource(isHud ? R.drawable.launcher_btn_primary : R.drawable.launcher_btn_outline);
        btn_ss_mode_hud.setBackgroundTintList(isHud ? ColorStateList.valueOf(accentColor) : null);
        btn_ss_mode_hud.setTextColor(isHud ? colorOnAccent : colorText);

        btn_ss_mode_game.setBackgroundResource(!isHud ? R.drawable.launcher_btn_primary : R.drawable.launcher_btn_outline);
        btn_ss_mode_game.setBackgroundTintList(!isHud ? ColorStateList.valueOf(accentColor) : null);
        btn_ss_mode_game.setTextColor(!isHud ? colorOnAccent : colorText);
    }

    // ════════════════════════════════════════════════════════════════════════
    //  截图库
    // ════════════════════════════════════════════════════════════════════════

    private File getScreenshotsDir() {
        File dir = new File(requireContext().getExternalFilesDir(null), "SAMP/screens");
        if (!dir.exists()) dir.mkdirs();
        return dir;
    }

    private void refreshScreenshotGallery() {
        Context ctx = getContext();
        if (ctx == null) return;
        final Context appCtx = ctx.getApplicationContext();
        ioExecutor.execute(() -> {
            File extDir = appCtx.getExternalFilesDir(null);
            if (extDir == null) return;
            File dir = new File(extDir, "SAMP/screens");
            if (!dir.exists()) dir.mkdirs();
            File[] files = dir.listFiles((d, name) -> {
                String lower = name.toLowerCase(Locale.US);
                return lower.endsWith(".png") || lower.endsWith(".jpg") || lower.endsWith(".jpeg");
            });

            List<File> list = new ArrayList<>();
            long totalBytes = 0;
            if (files != null) {
                Arrays.sort(files, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
                for (File f : files) {
                    list.add(f);
                    totalBytes += f.length();
                }
            }
            final long finalBytes = totalBytes;
            androidx.fragment.app.FragmentActivity act = getActivity();
            if (act != null && isAdded()) {
                act.runOnUiThread(() -> {
                    if (!isAdded() || getView() == null) return;
                    screenshotFiles.clear();
                    screenshotFiles.addAll(list);
                    screenshotAdapter.notifyDataSetChanged();
                    tv_screenshots_empty.setVisibility(list.isEmpty() ? View.VISIBLE : View.GONE);
                    tv_screenshots_summary.setText(String.format(Locale.US,
                            "共 %d 张截图 (%.2f MB) · 点击图片可放大预览或保存到手机相册",
                            list.size(), finalBytes / (1024.0 * 1024.0)));
                });
            }
        });
    }

    private void confirmClearScreenshots() {
        if (screenshotFiles.isEmpty()) {
            Toast.makeText(requireContext(), "截图库已经是空的", Toast.LENGTH_SHORT).show();
            return;
        }
        showAlertDialog("确定要清空截图库中的所有 " + screenshotFiles.size() + " 张截图吗？", (d, w) -> {
            List<File> toDelete = new ArrayList<>(screenshotFiles);
            ioExecutor.execute(() -> {
                for (File f : toDelete) {
                    f.delete();
                }
                androidx.fragment.app.FragmentActivity act = getActivity();
                if (act != null && isAdded()) {
                    act.runOnUiThread(() -> {
                        if (!isAdded() || getContext() == null) return;
                        refreshScreenshotGallery();
                        Toast.makeText(getContext(), "已清空全部截图", Toast.LENGTH_SHORT).show();
                    });
                }
            });
            d.dismiss();
        }, (d, w) -> d.dismiss());
    }

    private void previewScreenshot(File file) {
        Context ctx = requireContext();
        LinearLayout box = new LinearLayout(ctx);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(24, 20, 24, 12);
        box.setBackgroundResource(R.color.launcher_surface);

        ImageView iv = new ImageView(ctx);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                (int) (ctx.getResources().getDisplayMetrics().heightPixels * 0.62f));
        iv.setLayoutParams(lp);
        iv.setScaleType(ImageView.ScaleType.FIT_CENTER);
        Glide.with(this).load(file).into(iv);
        box.addView(iv);

        BitmapFactory.Options opts = new BitmapFactory.Options();
        opts.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(file.getAbsolutePath(), opts);

        TextView meta = new TextView(ctx);
        meta.setPadding(0, 12, 0, 4);
        meta.setGravity(Gravity.CENTER);
        meta.setTextColor(ctx.getResources().getColor(R.color.launcher_text_dim));
        meta.setTextSize(12);
        boolean isClean = file.getName().contains("_game");
        meta.setText(String.format(Locale.US, "%dx%d  ·  %.0f KB  ·  %s",
                opts.outWidth, opts.outHeight, file.length() / 1024.0,
                isClean ? "仅游戏画面" : "含界面与聊天"));
        box.addView(meta);

        new AlertDialog.Builder(ctx)
                .setView(box)
                .setPositiveButton("保存到手机相册", (d, w) -> saveScreenshotToSystemGallery(file))
                .setNeutralButton("分享截图", (d, w) -> shareFile(file, "image/png", "分享游戏截图"))
                .setNegativeButton("关闭", (d, w) -> d.dismiss())
                .show();
    }

    private void saveScreenshotToSystemGallery(File file) {
        Context ctx = getContext();
        if (ctx == null) return;
        final Context appCtx = ctx.getApplicationContext();
        ioExecutor.execute(() -> {
            try {
                ContentResolver resolver = appCtx.getContentResolver();
                ContentValues values = new ContentValues();
                values.put(MediaStore.Images.Media.DISPLAY_NAME, file.getName());
                values.put(MediaStore.Images.Media.MIME_TYPE, "image/png");
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    values.put(MediaStore.Images.Media.RELATIVE_PATH, Environment.DIRECTORY_PICTURES + "/SAMP-Mobile");
                    values.put(MediaStore.Images.Media.IS_PENDING, 1);
                }
                Uri uri = resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values);
                if (uri == null) throw new Exception("相册写入失败");

                try (InputStream in = new FileInputStream(file);
                     OutputStream out = resolver.openOutputStream(uri)) {
                    if (out == null) throw new Exception("相册输出流打开失败");
                    byte[] buf = new byte[8192];
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        out.write(buf, 0, n);
                    }
                }
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    values.clear();
                    values.put(MediaStore.Images.Media.IS_PENDING, 0);
                    resolver.update(uri, values, null, null);
                }
                androidx.fragment.app.FragmentActivity act = getActivity();
                if (act != null && isAdded()) {
                    act.runOnUiThread(() -> {
                        if (!isAdded() || getContext() == null) return;
                        Toast.makeText(getContext(), "已保存到手机相册", Toast.LENGTH_LONG).show();
                    });
                }
            } catch (Exception e) {
                androidx.fragment.app.FragmentActivity act = getActivity();
                if (act != null && isAdded()) {
                    act.runOnUiThread(() -> {
                        if (!isAdded() || getContext() == null) return;
                        Toast.makeText(getContext(), "保存相册失败: " + e.getMessage(), Toast.LENGTH_SHORT).show();
                    });
                }
            }
        });
    }

    private class ScreenshotAdapter extends RecyclerView.Adapter<ScreenshotAdapter.VH> {
        class VH extends RecyclerView.ViewHolder {
            ImageView ivThumb;
            TextView tvTitle, tvBadge, tvMeta, btnSave, btnShare, btnDelete;
            VH(View itemView, ImageView ivThumb, TextView tvTitle, TextView tvBadge,
               TextView tvMeta, TextView btnSave, TextView btnShare, TextView btnDelete) {
                super(itemView);
                this.ivThumb = ivThumb;
                this.tvTitle = tvTitle;
                this.tvBadge = tvBadge;
                this.tvMeta = tvMeta;
                this.btnSave = btnSave;
                this.btnShare = btnShare;
                this.btnDelete = btnDelete;
            }
        }

        @NonNull
        @Override
        public VH onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            Context ctx = parent.getContext();
            float dp = ctx.getResources().getDisplayMetrics().density;

            LinearLayout card = new LinearLayout(ctx);
            card.setOrientation(LinearLayout.VERTICAL);
            card.setBackgroundResource(R.drawable.launcher_row_bg);
            int pad = (int) (8 * dp);
            card.setPadding(pad, pad, pad, pad);
            RecyclerView.LayoutParams lp = new RecyclerView.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            int m = (int) (4 * dp);
            lp.setMargins(m, m, m, m);
            card.setLayoutParams(lp);

            ImageView iv = new ImageView(ctx);
            iv.setLayoutParams(new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, (int) (105 * dp)));
            iv.setScaleType(ImageView.ScaleType.CENTER_CROP);
            iv.setBackgroundResource(R.drawable.launcher_img_clip);
            iv.setClipToOutline(true);
            card.addView(iv);

            LinearLayout titleRow = new LinearLayout(ctx);
            titleRow.setOrientation(LinearLayout.HORIZONTAL);
            titleRow.setGravity(Gravity.CENTER_VERTICAL);
            titleRow.setPadding(0, (int) (6 * dp), 0, (int) (2 * dp));

            TextView tvTitle = new TextView(ctx);
            tvTitle.setLayoutParams(new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            tvTitle.setSingleLine(true);
            tvTitle.setTextColor(ctx.getResources().getColor(R.color.launcher_text));
            tvTitle.setTextSize(11);
            tvTitle.setTypeface(Typeface.DEFAULT_BOLD);
            titleRow.addView(tvTitle);

            TextView tvBadge = new TextView(ctx);
            tvBadge.setBackgroundResource(R.drawable.launcher_btn_outline);
            tvBadge.setPadding((int) (6 * dp), (int) (2 * dp), (int) (6 * dp), (int) (2 * dp));
            tvBadge.setTextColor(LauncherThemeManager.getAccentColor(ctx));
            tvBadge.setTextSize(9);
            tvBadge.setTypeface(Typeface.DEFAULT_BOLD);
            titleRow.addView(tvBadge);
            card.addView(titleRow);

            TextView tvMeta = new TextView(ctx);
            tvMeta.setTextColor(ctx.getResources().getColor(R.color.launcher_text_dim));
            tvMeta.setTextSize(10);
            card.addView(tvMeta);

            LinearLayout btnRow = new LinearLayout(ctx);
            btnRow.setOrientation(LinearLayout.HORIZONTAL);
            btnRow.setPadding(0, (int) (6 * dp), 0, 0);

            TextView btnSave = makeMiniButton(ctx, "存相册", R.drawable.launcher_btn_outline, R.color.launcher_accent);
            TextView btnShare = makeMiniButton(ctx, "分享", R.drawable.launcher_btn_outline, R.color.launcher_text);
            TextView btnDel = makeMiniButton(ctx, "删除", R.drawable.launcher_btn_danger, R.color.launcher_danger);

            btnRow.addView(btnSave);
            btnRow.addView(btnShare);
            btnRow.addView(btnDel);
            card.addView(btnRow);

            return new VH(card, iv, tvTitle, tvBadge, tvMeta, btnSave, btnShare, btnDel);
        }

        private TextView makeMiniButton(Context ctx, String text, int bgRes, int colorRes) {
            float dp = ctx.getResources().getDisplayMetrics().density;
            TextView tv = new TextView(ctx);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, (int) (26 * dp), 1f);
            lp.setMargins((int) (2 * dp), 0, (int) (2 * dp), 0);
            tv.setLayoutParams(lp);
            tv.setGravity(Gravity.CENTER);
            tv.setBackgroundResource(bgRes);
            tv.setTextColor(ctx.getResources().getColor(colorRes));
            tv.setTextSize(10);
            tv.setTypeface(Typeface.DEFAULT_BOLD);
            tv.setText(text);
            return tv;
        }

        @Override
        public void onBindViewHolder(@NonNull VH h, int position) {
            File file = screenshotFiles.get(position);
            int accentColor = LauncherThemeManager.getAccentColor(h.itemView.getContext());
            Glide.with(SettingsPageFragment.this).load(file).centerCrop().into(h.ivThumb);
            String dateStr = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date(file.lastModified()));
            h.tvTitle.setText(dateStr);
            boolean isClean = file.getName().contains("_game");
            h.tvBadge.setText(isClean ? "仅游戏画面" : "含界面与聊天");
            h.tvBadge.setTextColor(accentColor);
            h.btnSave.setTextColor(accentColor);
            h.tvMeta.setText(String.format(Locale.US, "文件大小：%.0f KB", file.length() / 1024.0));

            h.itemView.setOnClickListener(v -> previewScreenshot(file));
            h.ivThumb.setOnClickListener(v -> previewScreenshot(file));
            h.btnSave.setOnClickListener(v -> saveScreenshotToSystemGallery(file));
            h.btnShare.setOnClickListener(v -> shareFile(file, "image/png", "分享游戏截图"));
            h.btnDelete.setOnClickListener(v -> {
                file.delete();
                refreshScreenshotGallery();
                Toast.makeText(requireContext(), "截图已删除", Toast.LENGTH_SHORT).show();
            });
        }

        @Override
        public int getItemCount() {
            return screenshotFiles.size();
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    //  问题反馈与后台诊断（不向用户直接裸露代码日志）
    // ════════════════════════════════════════════════════════════════════════

    private void refreshDiagnosticStatus() {
        Context ctx = getContext();
        if (ctx == null) return;
        final Context appCtx = ctx.getApplicationContext();
        ioExecutor.execute(() -> {
            File baseDir = appCtx.getExternalFilesDir(null);
            if (baseDir == null) return;
            File logcatDir = new File(baseDir, "logcat");
            int logFileCount = 0;
            int crashHits = 0;

            if (logcatDir.exists()) {
                File[] all = logcatDir.listFiles((d, n) -> n.endsWith(".log"));
                if (all != null) {
                    logFileCount = all.length;
                    Arrays.sort(all, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
                    for (int i = 0; i < Math.min(all.length, 3); i++) {
                        crashHits += extractCrashAndErrorLines(all[i]).size();
                    }
                }
            }

            final int finalLogCount = logFileCount;
            final boolean hasCrash = crashHits > 0;
            androidx.fragment.app.FragmentActivity act = getActivity();
            if (act != null && isAdded()) {
                act.runOnUiThread(() -> {
                    if (!isAdded() || getView() == null) return;
                    if (hasCrash) {
                        tv_diag_status_title.setText("运行诊断状态：检测到历史异常记录");
                        tv_diag_status_desc.setText(String.format(Locale.US,
                                "系统已在后台自动保存 %d 份运行诊断记录。如您刚遇到闪退或卡顿，可直接点击下方「一键复制故障反馈信息」发送给客服或开发者协助排查。",
                                finalLogCount));
                    } else if (finalLogCount > 0) {
                        tv_diag_status_title.setText("运行诊断状态：正常");
                        tv_diag_status_desc.setText(String.format(Locale.US,
                                "客户端运行环境自检通过，当前已归档 %d 份运行记录，近期未发现崩溃异常。",
                                finalLogCount));
                    } else {
                        tv_diag_status_title.setText("运行诊断状态：就绪");
                        tv_diag_status_desc.setText("当前尚无历史故障记录。进入游戏后系统将在后台自动守护并记录运行状态。");
                    }
                });
            }
        });
    }

    private String buildDiagnosticReportText(Context appCtx) {
        File baseDir = appCtx.getExternalFilesDir(null);
        File logcatDir = baseDir != null ? new File(baseDir, "logcat") : null;
        StringBuilder sb = new StringBuilder();
        sb.append("========== SA-MP Mobile 故障反馈诊断信息 ==========\n");
        sb.append(buildDeviceSummaryLine(appCtx)).append("\n\n");

        File latest = findLatestClientLog(logcatDir);
        if (latest != null && latest.exists()) {
            List<String> crashes = extractCrashAndErrorLines(latest);
            if (!crashes.isEmpty()) {
                sb.append("--- 异常摘要 (").append(latest.getName()).append(") ---\n");
                for (String line : crashes) {
                    sb.append(line).append("\n");
                }
                sb.append("\n");
            }
            sb.append("--- 最近一次客户端运行记录 (").append(latest.getName()).append(") ---\n");
            sb.append(readFileTail(latest, 160));
        } else {
            sb.append("尚未生成客户端运行日志记录。\n");
        }
        return sb.toString();
    }

    private void copyDiagnosticReport() {
        Context ctx = getContext();
        if (ctx == null) return;
        final Context appCtx = ctx.getApplicationContext();
        ioExecutor.execute(() -> {
            String report = buildDiagnosticReportText(appCtx);
            androidx.fragment.app.FragmentActivity act = getActivity();
            if (act != null && isAdded()) {
                act.runOnUiThread(() -> {
                    if (!isAdded() || getContext() == null) return;
                    copyTextToClipboard("SA-MP Mobile 故障反馈", report);
                    Toast.makeText(getContext(), "故障反馈信息已复制到剪贴板，可直接粘贴发送！", Toast.LENGTH_LONG).show();
                });
            }
        });
    }

    private void shareDiagnosticLogFile() {
        Context ctx = getContext();
        if (ctx == null) return;
        final Context appCtx = ctx.getApplicationContext();
        ioExecutor.execute(() -> {
            try {
                File baseDir = appCtx.getExternalFilesDir(null);
                if (baseDir == null) throw new IllegalStateException("存储目录不可用");
                File dir = new File(baseDir, "logcat");
                if (!dir.exists()) dir.mkdirs();
                File reportFile = new File(dir, "SAMP_Feedback_Report.log");
                String report = buildDiagnosticReportText(appCtx);
                try (FileOutputStream fos = new FileOutputStream(reportFile)) {
                    fos.write(report.getBytes(StandardCharsets.UTF_8));
                }
                androidx.fragment.app.FragmentActivity act = getActivity();
                if (act != null && isAdded()) {
                    act.runOnUiThread(() -> {
                        if (!isAdded() || getContext() == null) return;
                        shareFile(reportFile, "text/plain", "导出故障反馈日志文件");
                    });
                }
            } catch (Exception e) {
                androidx.fragment.app.FragmentActivity act = getActivity();
                if (act != null && isAdded()) {
                    act.runOnUiThread(() -> {
                        if (!isAdded() || getContext() == null) return;
                        Toast.makeText(getContext(), "导出反馈文件失败: " + e.getMessage(), Toast.LENGTH_SHORT).show();
                    });
                }
            }
        });
    }

    private void confirmClearLogs() {
        Context ctx = getContext();
        if (ctx == null) return;
        final Context appCtx = ctx.getApplicationContext();
        showAlertDialog("确定要清理所有历史日志缓存文件吗？", (d, w) -> {
            ioExecutor.execute(() -> {
                File baseDir = appCtx.getExternalFilesDir(null);
                File logcatDir = baseDir != null ? new File(baseDir, "logcat") : null;
                if (logcatDir != null && logcatDir.exists()) {
                    File[] files = logcatDir.listFiles();
                    if (files != null) {
                        for (File f : files) f.delete();
                    }
                }
                androidx.fragment.app.FragmentActivity act = getActivity();
                if (act != null && isAdded()) {
                    act.runOnUiThread(() -> {
                        if (!isAdded() || getContext() == null) return;
                        refreshDiagnosticStatus();
                        Toast.makeText(getContext(), "日志缓存已清理", Toast.LENGTH_SHORT).show();
                    });
                }
            });
            d.dismiss();
        }, (d, w) -> d.dismiss());
    }

    private File findLatestClientLog(File logcatDir) {
        if (logcatDir == null || !logcatDir.exists()) return null;
        File[] files = logcatDir.listFiles((d, name) -> name.startsWith("client_") && name.endsWith(".log"));
        if (files == null || files.length == 0) return null;
        Arrays.sort(files, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
        return files[0];
    }

    private String readFileTail(File file, int maxLines) {
        if (file == null || !file.exists()) return "";
        List<String> lines = new ArrayList<>();
        try (BufferedReader br = new BufferedReader(new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line;
            while ((line = br.readLine()) != null) {
                lines.add(line);
                if (lines.size() > maxLines * 2) {
                    lines.subList(0, maxLines).clear();
                }
            }
        } catch (Exception ignored) {}
        int start = Math.max(0, lines.size() - maxLines);
        StringBuilder sb = new StringBuilder();
        for (int i = start; i < lines.size(); i++) {
            sb.append(lines.get(i)).append('\n');
        }
        return sb.toString();
    }

    private List<String> extractCrashAndErrorLines(File file) {
        List<String> result = new ArrayList<>();
        try (BufferedReader br = new BufferedReader(new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line;
            int contextAfterHit = 0;
            while ((line = br.readLine()) != null) {
                String lower = line.toLowerCase(Locale.US);
                boolean hit = lower.contains("crashed") || lower.contains("fatal signal")
                        || lower.contains("backtrace") || lower.contains("[error]")
                        || lower.contains("[critical]") || lower.contains("sigsegv")
                        || lower.contains("sigabrt") || lower.contains("last script command")
                        || lower.contains("last rendered object");
                if (hit) {
                    result.add(line);
                    contextAfterHit = 10;
                } else if (contextAfterHit > 0) {
                    result.add(line);
                    contextAfterHit--;
                }
                if (result.size() > 150) break;
            }
        } catch (Exception ignored) {}
        return result;
    }

    private String buildDeviceSummaryLine(Context appCtx) {
        boolean is64 = android.os.Process.is64Bit();
        String abi = is64 ? "64位" : "32位";
        File extDir = appCtx.getExternalFilesDir(null);
        long freeMB = 0;
        if (extDir != null && extDir.exists()) {
            StatFs stat = new StatFs(extDir.getAbsolutePath());
            freeMB = (stat.getAvailableBlocksLong() * stat.getBlockSizeLong()) / (1024 * 1024);
        }
        return String.format(Locale.US,
                "版本: v%s (%s) | 设备: %s %s | 系统: Android %s | 剩余空间: %d MB\n"
                + "模组开关: Modloader=%b, AML=%b, JPatch=%b, MonetLoader=%b, CLEO=%b",
                BuildConfig.VERSION_NAME, abi,
                Build.MANUFACTURER, Build.MODEL, Build.VERSION.RELEASE, freeMB,
                settings_prefs.getBoolean("modloader", false),
                settings_prefs.getBoolean("aml_scripts", false),
                settings_prefs.getBoolean("jpatch_enabled", false),
                settings_prefs.getBoolean("monet_scripts", false),
                settings_prefs.getBoolean("cleo_scripts", false));
    }

    // ════════════════════════════════════════════════════════════════════════
    //  帮助与关于
    // ════════════════════════════════════════════════════════════════════════

    private void populateHelpAndAbout() {
        tv_help_mods_body.setText(
                "1. 在哪里开启和管理模组？\n"
                + "请返回启动器主界面，点击最左侧导航栏的「模组」板块。您可以在模组板块中统一开启或关闭各类加载器，并单独管理每一个模组包与脚本。\n\n"
                + "2. 如何安装车包、人物皮肤、武器与贴图音效模组？\n"
                + "客户端支持免打包文件替换。只需将下载好的模组文件夹（内含 .dff 模型、.txd 贴图或音效文件）放入游戏数据目录的 modloader/ 文件夹下，随后在主界面「模组」板块开启资源加载器即可自动生效，不会破坏原始数据包。\n\n"
                + "3. 如何使用 Lua 辅助脚本与原生扩展插件？\n"
                + "• Lua 脚本：将 .lua 脚本文件放入 monetloader/ 目录，并在主界面「模组」板块中启用脚本加载器；\n"
                + "• 原生插件与画质补丁：在主界面「模组」板块中可一键开启扩展插件与引擎修复补丁，点击「画质补丁配置」还可按需开关各项渲染优化。\n\n"
                + "4. 安装新模组后进游戏闪退怎么办？\n"
                + "不同模组之间可能存在模型 ID 冲突或旧版脚本不兼容。只需在主界面「模组」板块中暂时关闭最近新增的模组开关，即可立即恢复正常进服；也可在本页「问题反馈」中一键复制故障信息交由开发者排查。");

        tv_help_faq_body.setText(
                "1. 如何使用自由镜头、调整摇杆大小或自定义游戏按键位置？\n"
                + "• 自由镜头：在「镜头设置」中开启「自由镜头（镜头不自动回正）」，行走或驾车时镜头将始终停留在您旋转到的方向；\n"
                + "• 分段灵敏度：在「镜头设置」中可分别调节步行、载具与开镜瞄准时的独立镜头灵敏度；\n"
                + "• 自定义按键位置：在「操作设置」中点击「进入游戏时自动打开按键位置调整」（或在游戏内展开底部快捷栏点击 HUD 键 / 输入 /hudedit），即可像单机版一样自由拖拽和缩放屏幕按键。\n\n"
                + "2. 步战时如何切换视角远近、开镜瞄准、格挡或乘坐副驾驶？\n"
                + "• 切换视角：点击游戏画面右上角的镜头图标即可在近距、标准、远距视角间切换，也可在「镜头设置」中设定默认距离；\n"
                + "• 瞄准与射击：持枪时点击右侧独立准星按钮进入瞄准状态，轻点开火键单发点射、按住连续射击；\n"
                + "• 近战格挡：徒手或持近战武器时按住格挡键即可保持防御姿态；\n"
                + "• 乘坐副驾驶：靠近载具时点击屏幕右侧出现的 G 键图标，或展开底部功能键盘点击 G。\n\n"
                + "3. 进入服务器后中文显示为乱码怎么解决？\n"
                + "在「联机设置」中修改「服务器文字编码」：国内大多数服务器使用 GBK 编码，部分新版服务器使用 UTF-8，港台服务器常使用 BIG5。\n\n"
                + "4. 如何拍摄不含聊天框和界面按钮的纯净游戏截图？\n"
                + "在「截图库」中将截图范围设为「仅游戏画面」，或在游戏聊天框直接输入 /ss game，即可拍摄隐藏所有界面与按键的纯净游戏画面。");

        tv_help_commands.setText(
                "/ss 或 /screenshot      立即截图（也可点击底部快捷键盘 F8）\n"
                + "/ss game               强制拍摄一张不含界面与聊天的纯净画面\n"
                + "/hudedit               打开游戏内触控按键位置与大小编辑器\n"
                + "/freecam               实时开关自由镜头（镜头不自动回正）\n"
                + "/headmove              开关角色头部随视角方向转动\n"
                + "/pagesize [5-20]       修改聊天窗口同时显示的消息行数\n"
                + "/timestamp             开关聊天消息前方的接收时间显示\n"
                + "/clearmychat           清空当前聊天窗口的历史消息\n"
                + "/save [备注]           记录当前人物或车辆坐标\n"
                + "/dl                    开关周围载具编号与距离坐标显示\n"
                + "/dw                    开关右侧击杀通报列表\n"
                + "/q 或 /quit            断开服务器连接并退出游戏");

        tv_about_version_line.setText(String.format(Locale.US,
                "SA-MP Mobile 移动端联机客户端  ·  版本 v%s",
                BuildConfig.VERSION_NAME));

        tv_about_summary.setText(
                "专为移动端打造的圣安地列斯多人联机启动器与游戏客户端，全面支持 0.3.7 与 0.3.DL 服务器协议，集成原生画质调节、纯净截图库与模块化扩展支持。\n\n"
                + "开源致谢：基于 Alyn SA-MP Mobile 二次开发，致谢 AML / MonetLoader / CLEO 等社区开源项目。");
    }

    // ════════════════════════════════════════════════════════════════════════
    //  文件验证与游戏资源管理（手动导入 / 手动下载精简版 / 逐项文件状态）
    // ════════════════════════════════════════════════════════════════════════

    private void setupGameDataControls() {
        if (option_gd_edition != null) {
            option_gd_edition.setAdapter(createDropdownAdapter(GAMEDATA_EDITION_ITEMS));
            option_gd_edition.setText(GAMEDATA_EDITION_ITEMS[0], false);
            option_gd_edition.setOnItemClickListener((parent, v, pos, id) -> {
                selectedGameDataEdition = (pos == 1)
                        ? GameDataVerifier.Edition.FULL
                        : GameDataVerifier.Edition.LITE;
                refreshGameDataVerification();
            });
        }
        if (btn_gd_import != null) {
            btn_gd_import.setOnClickListener(v -> {
                if (gdDownloading.get()) {
                    Toast.makeText(requireContext(), "正在下载数据包，请先取消后再导入", Toast.LENGTH_SHORT).show();
                    return;
                }
                gameDataZipPicker.launch(new String[]{"application/zip", "application/octet-stream", "*/*"});
            });
        }
        if (btn_gd_download != null) {
            btn_gd_download.setOnClickListener(v -> {
                if (gdDownloading.get()) {
                    cancelActiveGameDataDownload();
                } else {
                    onClickManualGameDataDownload();
                }
            });
        }
        if (btn_gd_verify != null) {
            btn_gd_verify.setOnClickListener(v -> {
                refreshGameDataVerification();
                Toast.makeText(requireContext(), "正在重新验证游戏文件…", Toast.LENGTH_SHORT).show();
            });
        }
        if (btn_gd_filter_all != null) {
            btn_gd_filter_all.setOnClickListener(v -> {
                gameDataFilterIssuesOnly = false;
                updateGameDataFilterUI();
                applyGameDataListFilter();
            });
        }
        if (btn_gd_filter_issues != null) {
            btn_gd_filter_issues.setOnClickListener(v -> {
                gameDataFilterIssuesOnly = true;
                updateGameDataFilterUI();
                applyGameDataListFilter();
            });
        }
        updateGameDataFilterUI();
        updateGameDataDownloadButtonUI();
    }

    private void updateGameDataFilterUI() {
        if (btn_gd_filter_all == null || btn_gd_filter_issues == null || !isAdded()) return;
        int accentColor = LauncherThemeManager.getAccentColor(requireContext());
        int colorOnAccent = requireContext().getResources().getColor(R.color.launcher_on_accent);
        int colorText = requireContext().getResources().getColor(R.color.launcher_text);

        boolean all = !gameDataFilterIssuesOnly;
        btn_gd_filter_all.setBackgroundResource(all ? R.drawable.launcher_btn_primary : R.drawable.launcher_btn_outline);
        btn_gd_filter_all.setBackgroundTintList(all ? ColorStateList.valueOf(accentColor) : null);
        btn_gd_filter_all.setTextColor(all ? colorOnAccent : colorText);

        btn_gd_filter_issues.setBackgroundResource(!all ? R.drawable.launcher_btn_primary : R.drawable.launcher_btn_outline);
        btn_gd_filter_issues.setBackgroundTintList(!all ? ColorStateList.valueOf(accentColor) : null);
        btn_gd_filter_issues.setTextColor(!all ? colorOnAccent : colorText);
    }

    private void updateGameDataDownloadButtonUI() {
        if (btn_gd_download == null || !isAdded()) return;
        if (gdDownloading.get()) {
            btn_gd_download.setBackgroundResource(R.drawable.launcher_btn_danger);
            btn_gd_download.setBackgroundTintList(null);
            btn_gd_download.setTextColor(requireContext().getResources().getColor(R.color.launcher_danger));
            btn_gd_download.setText("取消下载");
        } else {
            int accentColor = LauncherThemeManager.getAccentColor(requireContext());
            btn_gd_download.setBackgroundResource(R.drawable.launcher_btn_primary);
            btn_gd_download.setBackgroundTintList(ColorStateList.valueOf(accentColor));
            btn_gd_download.setTextColor(requireContext().getResources().getColor(R.color.launcher_on_accent));
            btn_gd_download.setText("启动器下载");
        }
    }

    private void refreshGameDataVerification() {
        Context ctx = getContext();
        if (ctx == null || tv_gd_summary == null) return;
        final Context appCtx = ctx.getApplicationContext();
        final GameDataVerifier.Edition edition = selectedGameDataEdition;
        ioExecutor.execute(() -> {
            GameDataVerifier.Report report = GameDataVerifier.verify(appCtx, edition);
            androidx.fragment.app.FragmentActivity act = getActivity();
            if (act != null && isAdded()) {
                act.runOnUiThread(() -> {
                    if (!isAdded() || getView() == null) return;
                    renderGameDataReport(report);
                });
            }
        });
    }

    private void renderGameDataReport(GameDataVerifier.Report report) {
        lastGameDataReport = report;
        String editionLabel = (report.edition == GameDataVerifier.Edition.FULL)
                ? "完整版校验（2.10 GiB）"
                : "精简版校验";
        String detectedPack = report.hasFullAudio ? "已含完整版音频" : "精简版结构";
        if (report.problemCount() == 0) {
            tv_gd_summary.setText(String.format(Locale.US,
                    "状态：已就绪（%s · %s）", editionLabel, detectedPack));
            tv_gd_summary.setTextColor(requireContext().getResources().getColor(R.color.ui_good));
        } else if (report.coreReady && report.edition == GameDataVerifier.Edition.FULL) {
            tv_gd_summary.setText(String.format(Locale.US,
                    "状态：核心可进服，缺少完整版音频项 %d 处（切回精简版即可正常游玩）", report.problemCount()));
            tv_gd_summary.setTextColor(requireContext().getResources().getColor(R.color.ui_warn));
        } else {
            tv_gd_summary.setText(String.format(Locale.US,
                    "状态：未就绪（缺失 %d 项，异常 %d 项）· 请导入或下载游戏数据",
                    report.missingCount, report.invalidCount));
            tv_gd_summary.setTextColor(requireContext().getResources().getColor(R.color.ui_bad));
        }

        String verText = (report.installedVersion != null && !report.installedVersion.isEmpty())
                ? " · 清单版本 " + report.installedVersion
                : "";
        tv_gd_storage_meta.setText(String.format(Locale.US,
                "当前占用 %s · 可用空间 %s%s · 统一提供精简版下载（完整版 2.10 GiB 暂仅支持本地导入与校验）",
                GameDataVerifier.formatBytes(report.totalDataBytes),
                GameDataVerifier.formatBytes(report.freeSpaceBytes),
                verText));

        String skipHint = report.skippedCount > 0
                ? String.format(Locale.US, " · 精简版跳过 %d 项", report.skippedCount)
                : "";
        tv_gd_list_summary.setText(String.format(Locale.US,
                "已通过 %d/%d · 缺失 %d · 异常 %d%s",
                report.okCount, report.requiredCount(),
                report.missingCount, report.invalidCount, skipHint));

        allGameDataItems.clear();
        allGameDataItems.addAll(report.items);
        applyGameDataListFilter();
    }

    private void applyGameDataListFilter() {
        displayedGameDataItems.clear();
        for (GameDataVerifier.FileCheckItem item : allGameDataItems) {
            if (!gameDataFilterIssuesOnly || item.isProblem()) {
                displayedGameDataItems.add(item);
            }
        }
        if (gameDataFileAdapter != null) {
            gameDataFileAdapter.notifyDataSetChanged();
        }
        if (tv_gd_list_empty != null && rv_gd_files != null) {
            boolean empty = displayedGameDataItems.isEmpty();
            tv_gd_list_empty.setVisibility(empty ? View.VISIBLE : View.GONE);
            rv_gd_files.setVisibility(empty ? View.GONE : View.VISIBLE);
        }
    }

    private void onClickManualGameDataDownload() {
        if (!isAdded()) return;
        if (selectedGameDataEdition == GameDataVerifier.Edition.FULL) {
            new AlertDialog.Builder(requireActivity())
                    .setTitle("完整版暂未开放下载")
                    .setMessage("当前启动器统一提供精简版游戏资源下载，完整版（2.10 GiB）暂仅支持通过「本地导入」安装与文件校验。\n\n是否切换至精简版并开始下载？")
                    .setPositiveButton("切换精简版并下载", (d, w) -> {
                        selectedGameDataEdition = GameDataVerifier.Edition.LITE;
                        if (option_gd_edition != null) {
                            option_gd_edition.setText(GAMEDATA_EDITION_ITEMS[0], false);
                        }
                        refreshGameDataVerification();
                        proceedLiteManualDownload();
                    })
                    .setNegativeButton("取消", null)
                    .show();
            return;
        }
        proceedLiteManualDownload();
    }

    private void proceedLiteManualDownload() {
        if (!isAdded()) return;
        String manifestUrl = GamedataState.resolveManifestUrl(requireContext());
        if (manifestUrl.isEmpty()) {
            new AlertDialog.Builder(requireActivity())
                    .setTitle("未配置下载源")
                    .setMessage("当前安装包未内置精简版数据下载源。\n\n请先准备好游戏数据包 .zip，点击「本地导入」直接导入到启动器中。")
                    .setPositiveButton("去本地导入", (d, w) ->
                            gameDataZipPicker.launch(new String[]{"application/zip", "application/octet-stream", "*/*"}))
                    .setNegativeButton("知道了", null)
                    .show();
            return;
        }

        boolean coreReady = GameDataImporter.isGameDataReady(requireContext().getExternalFilesDir(null));
        boolean hasProblems = lastGameDataReport != null && lastGameDataReport.problemCount() > 0;
        if (coreReady && !hasProblems) {
            new AlertDialog.Builder(requireActivity())
                    .setTitle("本地资源已完整")
                    .setMessage("检测到当前精简版核心文件已全部通过校验。是否仍要重新下载并覆盖修复？（您的设置与服务器列表将完整保留）")
                    .setPositiveButton("强制重新下载", (d, w) -> startManualGameDataDownload(manifestUrl, true))
                    .setNeutralButton("仅检查更新", (d, w) -> startManualGameDataDownload(manifestUrl, false))
                    .setNegativeButton("取消", null)
                    .show();
            return;
        }

        startManualGameDataDownload(manifestUrl, true);
    }

    private void startManualGameDataDownload(String manifestUrl, boolean forceRedownload) {
        if (!gdDownloading.compareAndSet(false, true)) return;
        lastDownloadProgressPostMs = 0L;

        if (layout_gd_download_progress != null) {
            layout_gd_download_progress.setVisibility(View.VISIBLE);
        }
        if (pb_gd_download != null) {
            pb_gd_download.setProgress(0);
        }
        if (tv_gd_dl_status != null) {
            tv_gd_dl_status.setText("正在连接下载服务…");
            tv_gd_dl_status.setTextColor(requireContext().getResources().getColor(R.color.launcher_text));
        }
        if (tv_gd_dl_bytes != null) {
            tv_gd_dl_bytes.setText("0%");
        }
        updateGameDataDownloadButtonUI();

        final Context appCtx = requireContext().getApplicationContext();
        final GamedataInstallerTask task = new GamedataInstallerTask(appCtx, manifestUrl, forceRedownload);
        activeDownloadTask = task;

        Thread worker = new Thread(() -> task.run(new GamedataInstallerTask.Listener() {
            @Override
            public void onStage(GamedataInstallerTask.Stage stage) {
                postToUi(() -> {
                    if (tv_gd_dl_status == null) return;
                    switch (stage) {
                        case FETCHING_MANIFEST:
                            tv_gd_dl_status.setText("正在获取精简版数据清单…");
                            break;
                        case CHECKING:
                            tv_gd_dl_status.setText("正在检查数据版本…");
                            break;
                        case DOWNLOADING:
                            tv_gd_dl_status.setText("正在下载游戏数据（支持断点续传）…");
                            break;
                        case VERIFYING:
                            tv_gd_dl_status.setText("正在校验数据包完整性（SHA-256）…");
                            break;
                        case EXTRACTING:
                            tv_gd_dl_status.setText("正在解压安装游戏文件…");
                            break;
                        case ACTIVATING:
                            tv_gd_dl_status.setText("正在启用已安装资源…");
                            break;
                        case READY:
                            break;
                        default:
                            break;
                    }
                });
            }

            @Override
            public void onProgress(long bytesDone, long bytesTotal) {
                long now = System.currentTimeMillis();
                int perMille = bytesTotal > 0
                        ? (int) Math.min(1000L, (bytesDone * 1000L) / bytesTotal)
                        : 0;
                boolean force = perMille >= 1000 || lastDownloadProgressPostMs == 0L;
                if (!force && (now - lastDownloadProgressPostMs) < 200L) return;
                lastDownloadProgressPostMs = now;
                postToUi(() -> {
                    if (pb_gd_download != null && bytesTotal > 0) {
                        pb_gd_download.setProgress(perMille);
                    }
                    if (tv_gd_dl_bytes != null && bytesTotal > 0) {
                        tv_gd_dl_bytes.setText(String.format(Locale.US, "%d%%  (%s / %s)",
                                perMille / 10,
                                GameDataVerifier.formatBytes(bytesDone),
                                GameDataVerifier.formatBytes(bytesTotal)));
                    }
                });
            }

            @Override
            public void onStatus(String message) {
                if (message == null || message.isEmpty()) return;
                postToUi(() -> {
                    if (tv_gd_dl_status != null) {
                        tv_gd_dl_status.setText(message);
                    }
                });
            }

            @Override
            public void onError(String message, Throwable cause) {
                gdDownloading.set(false);
                activeDownloadTask = null;
                postToUi(() -> {
                    updateGameDataDownloadButtonUI();
                    if (tv_gd_dl_status != null) {
                        tv_gd_dl_status.setText(message != null ? message : "下载失败");
                        tv_gd_dl_status.setTextColor(requireContext().getResources().getColor(R.color.ui_bad));
                    }
                    refreshGameDataVerification();
                });
            }

            @Override
            public void onDone(boolean installedNow) {
                gdDownloading.set(false);
                activeDownloadTask = null;
                if (installedNow) {
                    try {
                        Utils.saveSettings(appCtx);
                    } catch (Exception ignored) {
                    }
                }
                postToUi(() -> {
                    updateGameDataDownloadButtonUI();
                    if (pb_gd_download != null) {
                        pb_gd_download.setProgress(1000);
                    }
                    if (tv_gd_dl_bytes != null) {
                        tv_gd_dl_bytes.setText("100%");
                    }
                    if (tv_gd_dl_status != null) {
                        tv_gd_dl_status.setText(installedNow ? "精简版资源下载并安装完成" : "本地数据已是最新，无需重新下载");
                        tv_gd_dl_status.setTextColor(requireContext().getResources().getColor(R.color.ui_good));
                    }
                    refreshGameDataVerification();
                });
            }
        }), "gamedata-manual-download");
        worker.start();
    }

    private void cancelActiveGameDataDownload() {
        GamedataInstallerTask task = activeDownloadTask;
        if (task != null) {
            task.cancel();
            if (isAdded() && tv_gd_dl_status != null) {
                tv_gd_dl_status.setText("正在取消下载…");
            }
        }
    }

    private void postToUi(Runnable action) {
        androidx.fragment.app.FragmentActivity act = getActivity();
        if (act != null && isAdded()) {
            act.runOnUiThread(() -> {
                if (isAdded() && getView() != null) {
                    action.run();
                }
            });
        }
    }

    private class GameDataFileAdapter extends RecyclerView.Adapter<GameDataFileAdapter.VH> {
        class VH extends RecyclerView.ViewHolder {
            final View statusDot;
            final TextView tvPath, tvDesc, tvSize, tvBadge;

            VH(View itemView) {
                super(itemView);
                statusDot = itemView.findViewById(R.id.v_gd_status_dot);
                tvPath = itemView.findViewById(R.id.tv_gd_file_path);
                tvDesc = itemView.findViewById(R.id.tv_gd_file_desc);
                tvSize = itemView.findViewById(R.id.tv_gd_file_size);
                tvBadge = itemView.findViewById(R.id.tv_gd_file_badge);
            }
        }

        @NonNull
        @Override
        public VH onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            View v = LayoutInflater.from(parent.getContext())
                    .inflate(R.layout.item_gamedata_file, parent, false);
            return new VH(v);
        }

        @Override
        public void onBindViewHolder(@NonNull VH h, int position) {
            GameDataVerifier.FileCheckItem item = displayedGameDataItems.get(position);
            Context ctx = h.itemView.getContext();
            h.tvPath.setText(item.path);
            h.tvDesc.setText(item.group + " · " + item.label);

            if (item.status == GameDataVerifier.Status.OK) {
                String sizeText = item.directory
                        ? String.format(Locale.US, "%d 个文件 · %s", item.fileCount, GameDataVerifier.formatBytes(item.bytes))
                        : GameDataVerifier.formatBytes(item.bytes);
                h.tvSize.setText(sizeText);
            } else if (item.status == GameDataVerifier.Status.INVALID) {
                h.tvSize.setText(GameDataVerifier.formatBytes(item.bytes));
            } else {
                h.tvSize.setText("—");
            }

            int colorRes;
            String badgeText;
            switch (item.status) {
                case OK:
                    colorRes = R.color.ui_good;
                    badgeText = "正常";
                    break;
                case INVALID:
                    colorRes = R.color.ui_warn;
                    badgeText = "文件异常";
                    break;
                case SKIPPED_LITE:
                    colorRes = R.color.launcher_text_dim;
                    badgeText = "精简版跳过";
                    break;
                case MISSING:
                default:
                    colorRes = R.color.ui_bad;
                    badgeText = "缺失";
                    break;
            }
            int color = ctx.getResources().getColor(colorRes);
            h.statusDot.setBackgroundTintList(ColorStateList.valueOf(color));
            h.tvBadge.setText(badgeText);
            h.tvBadge.setTextColor(color);
        }

        @Override
        public int getItemCount() {
            return displayedGameDataItems.size();
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    //  通用工具方法
    // ════════════════════════════════════════════════════════════════════════

    private void shareFile(File file, String mimeType, String chooserTitle) {
        if (file == null || !file.exists()) {
            Toast.makeText(requireContext(), "文件不存在", Toast.LENGTH_SHORT).show();
            return;
        }
        try {
            Uri uri = FileProvider.getUriForFile(requireContext(),
                    requireContext().getPackageName() + ".provider", file);
            Intent intent = new Intent(Intent.ACTION_SEND);
            intent.setType(mimeType);
            intent.putExtra(Intent.EXTRA_STREAM, uri);
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(intent, chooserTitle));
        } catch (Exception e) {
            Toast.makeText(requireContext(), "分享失败: " + e.getMessage(), Toast.LENGTH_SHORT).show();
        }
    }

    private void copyTextToClipboard(String label, String text) {
        ClipboardManager cm = (ClipboardManager) requireContext().getSystemService(Context.CLIPBOARD_SERVICE);
        if (cm != null) {
            cm.setPrimaryClip(ClipData.newPlainText(label, text));
        }
    }

    private void savePreference(String key, Object value) {
        SharedPreferences.Editor editor = settings_prefs.edit();

        if (value instanceof Integer) editor.putInt(key, (Integer) value);
        else if (value instanceof Boolean) editor.putBoolean(key, (Boolean) value);
        else if (value instanceof Float) editor.putFloat(key, (Float) value);
        else if (value instanceof String) editor.putString(key, (String) value);

        editor.apply();
        final Context appCtx = requireContext().getApplicationContext();
        ioExecutor.execute(() -> Utils.saveSettings(appCtx));
    }

    private void showAlertDialog(String message, DialogInterface.OnClickListener positiveListener, DialogInterface.OnClickListener negativeListener) {
        new AlertDialog.Builder(requireActivity())
                .setTitle("提示")
                .setMessage(message)
                .setPositiveButton("确定", positiveListener)
                .setNegativeButton("取消", negativeListener)
                .show();
    }
}

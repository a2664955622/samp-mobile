package com.sampmobile.game;

import android.content.Context;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.MotionEvent;
import android.view.WindowManager;
import android.widget.Toast;

import com.joom.paranoid.Obfuscate;
import com.rockstargames.gtasa.GTASA;

import java.io.File;
import java.util.Objects;

import com.sampmobile.game.ui.UI;
import com.sampmobile.game.ui.cef.CefManager;
import com.sampmobile.launcher.utils.ModManager;
import com.sampmobile.launcher.utils.SignatureChecker;
import com.sampmobile.launcher.utils.Utils;

@Obfuscate
public class SAMP extends GTASA {
    private static final String TAG = "SAMP";
    private static boolean sAmlLoaded = false;
    private static boolean sMonetLoaded = false;

    public UI ui;

    private native void initializeSAMP(UI ui, String gameDir, boolean isOffline);

    private native boolean multiTouchEvent4Ex(int action, int count, int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3);

    private native void onHardwareKeyEvent(int keyCode, boolean isDown);

    private void loadLibraries() {
        Log.i(TAG, "**** Loading libraries");

        // check if arch is arm32 and load ImmEmulatorJ
        // Process bitness is the right signal: on translated/emulated devices the
        // SUPPORTED_ABIS list reflects the host (e.g. x86_64), not the loaded ABI.
        boolean is_64 = android.os.Process.is64Bit();
        Log.i(TAG, "is64BitProcess: " + is_64);

        if (!is_64) {
            System.loadLibrary("ImmEmulatorJ");
        }

        System.loadLibrary("GTASA");
        System.loadLibrary("SAMPMOBILE");
    }

    private void loadModLibraries() {
        android.content.SharedPreferences prefs = getSharedPreferences("samp_settings", Context.MODE_PRIVATE);
        boolean amlEnabled = prefs.getBoolean("aml_scripts", false);
        boolean jpatchEnabled = prefs.getBoolean("jpatch_enabled", false);
        boolean monetEnabled = prefs.getBoolean("monet_scripts", false);

        ModManager.ensureDirectories(this);

        if (amlEnabled || jpatchEnabled) {
            // Sync JPatch .so enabled/disabled filename before AML scans mods/
            ModManager.syncJPatchBinaryState(this, jpatchEnabled);
            ModManager.prepareAmlConfig(this);

            if (!sAmlLoaded) {
                try {
                    // Preload libc++_shared so third-party AML plugins linked dynamically against it can dlopen cleanly
                    try {
                        System.loadLibrary("c++_shared");
                    } catch (Throwable ignored) {
                    }
                    Log.i(TAG, "Loading AndroidModLoader (AML)...");
                    System.loadLibrary("AML");
                    sAmlLoaded = true;
                    Log.i(TAG, "AndroidModLoader (AML) loaded");
                } catch (Throwable t) {
                    Log.e(TAG, "Failed to load AML: " + t.getMessage(), t);
                }
            }
        }

        if (monetEnabled && !sMonetLoaded) {
            try {
                ModManager.ensureMonetLibsInstalled(this, false);
                Log.i(TAG, "Loading MonetLoader (LuaJIT 5.1 + libmonetloader)...");
                System.loadLibrary("luajit-5.1");
                System.loadLibrary("monetloader");
                sMonetLoaded = true;
                Log.i(TAG, "MonetLoader loaded");
            } catch (Throwable t) {
                Log.e(TAG, "Failed to load MonetLoader: " + t.getMessage(), t);
            }
        }
    }

    @Override
    public void onCreate(Bundle savedInstanceState) {
        Log.i(TAG, "**** onCreate");

        android.content.Intent launchIntent = getIntent();
        if (launchIntent == null || !Objects.equals(launchIntent.getStringExtra("extra_check"), "sampmobile1337")) {
            Log.e(TAG, "Not joined from launcher!");
            supportPauseResume = false;
            super.onCreate(savedInstanceState);
            finish();
            return;
        }

        loadLibraries();

        super.onCreate(savedInstanceState);
        Toast.makeText(this, "SAMP 已启动", Toast.LENGTH_LONG).show();

        File extDir = getExternalFilesDir(null);
        if (extDir == null) {
            Log.e(TAG, "External files directory is unavailable!");
            Toast.makeText(this, "游戏数据存储目录不可用", Toast.LENGTH_LONG).show();
            finish();
            return;
        }

        // delete MINFO.BIN (both root and models/MINFO.BIN) and CINFO.BIN if they exist
        File minfo = new File(extDir, "MINFO.BIN");
        File minfoModels = new File(extDir, "models/MINFO.BIN");
        File cinfo = new File(extDir, "CINFO.BIN");

        if (minfo.exists()) {
            Log.i(TAG, "Deleting MINFO.BIN");
            minfo.delete();
        }
        if (minfoModels.exists()) {
            Log.i(TAG, "Deleting models/MINFO.BIN");
            minfoModels.delete();
        }

        if (cinfo.exists()) {
            Log.i(TAG, "Deleting CINFO.BIN");
            cinfo.delete();
        }

        ui = new UI(this);
        ui.initializeUI();

        // omp-cef 浏览器层：装上 DecorView，服务端 CreateBrowser 事件到达时可立即建窗
        CefManager.install(this);

        // check signature
        if (!SignatureChecker.isSignatureValid(this, getPackageName())) {
            Log.e("SplashActivity", "No remake idiot!");
            finish();
            return;
        } else {
            Log.i("SplashActivity", "Using original client!");
        }

        // Ensure SAMP/settings.json matches SharedPreferences (even on offline or direct launches)
        Utils.saveSettings(this);
        getSharedPreferences("samp_settings", Context.MODE_PRIVATE)
                .edit()
                .putBoolean("open_hud_edit_on_start", false)
                .commit();

        String gameDir = extDir.toString().concat("/");
        Log.i(TAG, "Game directory: " + gameDir);

        try {
            initializeSAMP(ui, gameDir, getSharedPreferences("samp_settings", Context.MODE_PRIVATE).getBoolean("offline_mode", false));
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, e.getMessage() != null ? e.getMessage() : "UnsatisfiedLinkError in initializeSAMP");
        }

        // Load optional mod frameworks AFTER initializeSAMP (so our crash handler,
        // logger, hooks, and ModLoader are ready) and BEFORE onCreate returns
        // (so surfaceCreated / NVEventAppMain has not started yet).
        loadModLibraries();

        // fullscreen
        if (getSharedPreferences("samp_settings", Context.MODE_PRIVATE).getBoolean("fullscreen", false)) {
            if (Build.VERSION.SDK_INT >= 30) {
                getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
            } else if (Build.VERSION.SDK_INT >= 28) {
                getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            }
        }
    }

    private boolean mConsumedBackKey = false;

    @Override
    public void onBackPressed() {
        // 页面正在接管输入时，返回键先把焦点还给游戏（不销毁页面——生命周期属于服务端）
        if (CefManager.onBackPressed()) {
            return;
        }
        if (ui != null && ui.onBackPressed()) {
            return;
        }
        super.onBackPressed();
    }

    @Override
    public boolean dispatchKeyEvent(android.view.KeyEvent event) {
        // 按键上行（PressKey）：必须在 super 之前，否则被输入法/WebView 消费掉就看不到。
        CefManager.handleKeyEvent(event);

        // BACK 在页面持有焦点时可能被 Chromium 自己吃掉，Activity.onKeyDown 就收不到、
        // onBackPressed 也不会被调用。所以在派发进视图树之前截住：先释放页面焦点，
        // 把手势交还给游戏；页面本身留给服务端销毁。
        if (event.getKeyCode() == android.view.KeyEvent.KEYCODE_BACK
                && event.getAction() == android.view.KeyEvent.ACTION_UP
                && CefManager.isAnyBrowserFocused()
                && CefManager.onBackPressed()) {
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean onKeyDown(int keyCode, android.view.KeyEvent event) {
        if (keyCode == android.view.KeyEvent.KEYCODE_BACK) {
            if (event.getRepeatCount() == 0 && ui != null && ui.onBackPressed()) {
                mConsumedBackKey = true;
                return true;
            }
            if (mConsumedBackKey) {
                return true;
            }
        }
        // CEF 页面持有焦点时不要把按键喂给引擎（否则会同时触发游戏动作）
        if (ui != null && !ui.isInputFocused() && !CefManager.isAnyBrowserFocused()) {
            try {
                onHardwareKeyEvent(keyCode, true);
            } catch (UnsatisfiedLinkError ignored) {
            }
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, android.view.KeyEvent event) {
        if (keyCode == android.view.KeyEvent.KEYCODE_BACK && mConsumedBackKey) {
            mConsumedBackKey = false;
            return true;
        }
        // 保留本地修复：Java 输入框弹出时不吞 keyUp（防卡键），仅在 CEF 页面持有焦点时阻断
        if (ui != null && !CefManager.isAnyBrowserFocused()) {
            try {
                onHardwareKeyEvent(keyCode, false);
            } catch (UnsatisfiedLinkError ignored) {
            }
        }
        return super.onKeyUp(keyCode, event);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (ui != null) {
            if (ui.onTouchEvent(event)) {
                cancelAllNativeTouches();
                return true;
            }
            if (ui.isDialogVisible()) {
                return true;
            }
        }

        int x1 = 0, y1 = 0, x2 = 0, y2 = 0, x3 = 0, y3 = 0, x4 = 0, y4 = 0;
        int presentMask = 0;
        int numEvents = event.getPointerCount();

        for (int i = 0; i < numEvents; i++) {
            int pointerId = event.getPointerId(i);

            if (pointerId == 0) {
                x1 = (int) event.getX(i);
                y1 = (int) event.getY(i);
                presentMask |= (1 << 0);
            } else if (pointerId == 1) {
                x2 = (int) event.getX(i);
                y2 = (int) event.getY(i);
                presentMask |= (1 << 1);
            } else if (pointerId == 2) {
                x3 = (int) event.getX(i);
                y3 = (int) event.getY(i);
                presentMask |= (1 << 2);
            } else if (pointerId == 3) {
                x4 = (int) event.getX(i);
                y4 = (int) event.getY(i);
                presentMask |= (1 << 3);
            }
        }

        int pointerId = event.getPointerId(event.getActionIndex());
        int action = event.getActionMasked();
        int packedPointer = (pointerId & 0xFF) | (presentMask << 8);

        try {
            multiTouchEvent4Ex(action, packedPointer, x1, y1, x2, y2, x3, y3, x4, y4);
        } catch (UnsatisfiedLinkError e) {
            e.printStackTrace();
        }
        return super.onTouchEvent(event);
    }

    public void cancelAllNativeTouches() {
        try {
            multiTouchEvent4Ex(MotionEvent.ACTION_CANCEL, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

/*
    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (getSharedPreferences("samp_settings", Context.MODE_PRIVATE).getInt("game_version", 0) == 0) {
            if (ui != null) {
                ui.onTouchEvent(event);
            }

            int x1 = 0, y1 = 0, x2 = 0, y2 = 0, x3 = 0, y3 = 0, x4 = 0, y4 = 0;
            int numEvents = event.getPointerCount();

            for (int i = 0; i < numEvents; i++) {
                int pointerId = event.getPointerId(i);

                if (pointerId == 0) {
                    x1 = (int) event.getX(i);
                    y1 = (int) event.getY(i);
                } else if (pointerId == 1) {
                    x2 = (int) event.getX(i);
                    y2 = (int) event.getY(i);
                } else if (pointerId == 2) {
                    x3 = (int) event.getX(i);
                    y3 = (int) event.getY(i);
                } else if (pointerId == 3) {
                    x4 = (int) event.getX(i);
                    y4 = (int) event.getY(i);
                }
            }

            int pointerId = event.getPointerId(event.getActionIndex());
            int action = event.getActionMasked();

            try {
                multiTouchEvent4Ex(action, pointerId, x1, y1, x2, y2, x3, y3, x4, y4);
            } catch (UnsatisfiedLinkError e) {
                e.printStackTrace();
            }
            return super.onTouchEvent(event);
        }
        // arz version
        else {
            int pointerCount = event.getPointerCount();
            int i = 0;
            int i2 = 0;
            int i3 = 0;
            int i4 = 0;
            int i5 = 0;
            int i6 = 0;
            int i7 = 0;
            int i8 = 0;
            for (int i9 = 0; i9 < pointerCount; i9++) {
                int pointerId = event.getPointerId(i9);
                if (pointerId == 0) {
                    i = (int) event.getX(i9);
                    i2 = (int) event.getY(i9);
                } else if (pointerId == 1) {
                    i3 = (int) event.getX(i9);
                    i4 = (int) event.getY(i9);
                } else if (pointerId == 2) {
                    i5 = (int) event.getX(i9);
                    i6 = (int) event.getY(i9);
                } else if (pointerId == 3) {
                    i7 = (int) event.getX(i9);
                    i8 = (int) event.getY(i9);
                }
            }
            multiTouchEvent4(event.getActionMasked(), event.getPointerId(event.getActionIndex()), i, i2, i3, i4, i5, i6, i7, i8, event);
            return true;
        }
    }
*/

    @Override
    protected void onNewIntent(android.content.Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
    }

    @Override
    public void onStart() {
        Log.i(TAG, "**** onStart");
        super.onStart();
    }

    @Override
    public void onRestart() {
        Log.i(TAG, "**** onRestart");
        super.onRestart();
    }

    @Override
    public void onResume() {
        Log.i(TAG, "**** onResume");
        super.onResume();

        if (ui != null) {
            ui.onResumeEvent();
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus) {
            cancelAllNativeTouches();
        }
    }

    @Override
    public void onPause() {
        Log.i(TAG, "**** onPause");
        // 切后台：收起输入法并让出游戏输入，否则回来时焦点还在页面上、摇杆失效
        CefManager.releaseAllFocus();
        cancelAllNativeTouches();
        super.onPause();
    }

    @Override
    public void onStop() {
        Log.i(TAG, "**** onStop");
        super.onStop();
    }

    @Override
    public void onDestroy() {
        Log.i(TAG, "**** onDestroy");
        if (ui != null) {
            ui.destroy();
        }
        // 拆除全部 omp-cef WebView，避免跨 Activity 生命周期泄漏
        CefManager.shutdown();
        // Kill the isolated :game process BEFORE NvEventQueueActivity.onDestroy() invokes
        // quitAndWait() (10s Main-Looper wait + OS_ApplicationEvent UAF) & systemCleanup().
        android.os.Process.killProcess(android.os.Process.myPid());
        super.onDestroy();
    }
}

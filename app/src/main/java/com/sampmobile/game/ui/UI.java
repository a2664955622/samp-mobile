package com.sampmobile.game.ui;

import android.content.Context;
import android.graphics.Color;
import android.text.Spannable;
import android.text.SpannableString;
import android.text.style.ForegroundColorSpan;
import android.util.Log;
import android.view.MotionEvent;

import com.google.firebase.crashlytics.FirebaseCrashlytics;
import com.joom.paranoid.Obfuscate;

import java.io.UnsupportedEncodingException;
import java.util.Arrays;
import java.util.Objects;

import com.sampmobile.game.SAMP;
import com.sampmobile.game.ui.cef.CefManager;
import com.sampmobile.game.ui.widgets.ButtonPanel;
import com.sampmobile.game.ui.widgets.Chat;
import com.sampmobile.game.ui.widgets.Dialog;
import com.sampmobile.game.ui.widgets.EditObject;
import com.sampmobile.game.ui.widgets.HeightProvider;
import com.sampmobile.game.ui.widgets.Keyboard;
import com.sampmobile.game.ui.widgets.LoadingScreen;
import com.sampmobile.game.ui.widgets.Scoreboard;
import com.sampmobile.game.ui.widgets.Voice;
import com.sampmobile.game.ui.widgets.WantedLevel;

@Obfuscate
public class UI implements ButtonPanel.Listener, Voice.Listener, Dialog.Listener, Scoreboard.Listener, Keyboard.Listener, Chat.Listener, HeightProvider.Listener {
    private static final String TAG = "UI";

    private final SAMP samp;

    private LoadingScreen loadingScreen;
    private ButtonPanel buttonPanel;
    private Voice voice;
    private Dialog dialog;
    private Keyboard keyboard;
    private Scoreboard scoreboard;
    private WantedLevel wantedLevel;
    private Chat chat = null;
    private EditObject editObject = null;
    private HeightProvider heightProvider = null;

    private native void sendButtonPanelKey(int id);

    private native void voicePressed();

    private native void keyboardSend(byte[] str);

    private native void sendDialogResponse(int dialog_id, int button_id, int list_item, byte[] input);

    private native void setJavaDialogVisible(boolean visible);

    private native void setJavaKeyboardVisible(boolean visible);

    private native void sendScoreboardClick(int player_id);

    private native void chatSend(String str);

    private native boolean handleNativeBackPressed();

    public UI(SAMP samp) {
        this.samp = samp;
    }

    public void initializeUI() {
        heightProvider = new HeightProvider(samp).init(samp.GetMainView()).setHeightListener(this);
        loadingScreen = new LoadingScreen(samp);
        buttonPanel = new ButtonPanel(samp, this);
        voice = new Voice(samp, this);
        chat = new Chat(samp, this);
        editObject = new EditObject(samp);
        dialog = new Dialog(samp, this);
        scoreboard = new Scoreboard(samp, this);
        keyboard = new Keyboard(samp, this);
        wantedLevel = new WantedLevel(samp);

        if (samp.getSharedPreferences("samp_settings", Context.MODE_PRIVATE).getInt("game_version", 0) == 0) {
            showLoadingScreen(true);
            showWantedLevel(true);
        }
    }

    public boolean onBackPressed() {
        if (keyboard != null && keyboard.isVisible()) {
            showKeyboard(false);
            return true;
        }
        if (isDialogVisible()) {
            dialog.SendDialogResponse(0, dialog.mCurrentListitem, dialog.mCurrentInputtext);
            return true;
        }
        try {
            if (handleNativeBackPressed()) {
                return true;
            }
        } catch (UnsatisfiedLinkError ignored) {
        }
        return false;
    }

    public void destroy() {
        if (heightProvider != null) {
            heightProvider.destroy();
            heightProvider = null;
        }
        if (editObject != null) {
            editObject.show(false);
        }
        _onDialogVisibilityChanged(false);
        _onKeyboardVisibilityChanged(false);
    }

    public boolean isDialogVisible() {
        return dialog != null && dialog.isShow && dialog.mMainLayout != null && dialog.mMainLayout.getVisibility() == android.view.View.VISIBLE;
    }

    @Override
    public void _onDialogVisibilityChanged(boolean visible) {
        try {
            setJavaDialogVisible(visible);
        } catch (UnsatisfiedLinkError ignored) {
        }
        if (visible && samp != null) {
            samp.cancelAllNativeTouches();
        }
    }

    @Override
    public void _onKeyboardVisibilityChanged(boolean visible) {
        try {
            setJavaKeyboardVisible(visible);
        } catch (UnsatisfiedLinkError ignored) {
        }
        if (visible && samp != null) {
            samp.cancelAllNativeTouches();
        }
        // 游戏聊天输入开/关 -> omp-cef：本地事件发给每个页面，同时上行
        // ClientEmitEvent(ChatInputState)，对应 PC ChatHook::SetChatInputState。
        CefManager.onGameChatStateChanged(visible);
    }

    public boolean isInputFocused() {
        if (keyboard != null && keyboard.isVisible()) {
            return true;
        }
        if (dialog != null && dialog.isShow) {
            return true;
        }
        return false;
    }

    public boolean onTouchEvent(MotionEvent event) {
        if (keyboard != null) {
            return keyboard.onTouchEvent(event);
        }
        return false;
    }

    public void exitGame() {
        try {
            FirebaseCrashlytics.getInstance().setCrashlyticsCollectionEnabled(false);
        } catch (Throwable ignored) {
            // FirebaseInitProvider only runs in the main launcher process, not :game.
        }
        Thread exitWatchdog = new Thread(() -> {
            try {
                Thread.sleep(150);
            } catch (InterruptedException ignored) {
            }
            android.os.Process.killProcess(android.os.Process.myPid());
        }, "samp-exit-watchdog");
        exitWatchdog.setDaemon(true);
        exitWatchdog.start();
        samp.runOnUiThread(samp::finish);
    }

    public void onResumeEvent() {
        if (heightProvider != null) {
            heightProvider.init(samp.GetMainView());
        }
    }

    private void showLoadingScreen(boolean z) {
        samp.runOnUiThread(() -> loadingScreen.show(z));
    }

    public void showEditObject(boolean z) {
        samp.runOnUiThread(() -> editObject.show(z));
    }

    public void setPauseState(boolean pause) {
        samp.runOnUiThread(() -> {
            if (pause) {
                keyboard.show(false);
                wantedLevel.show(false);
                dialog.showWithoutReset(false);
                editObject.showWithoutReset(false);
            } else {
                wantedLevel.show(true);
                dialog.showWithoutReset(true);
                editObject.showWithoutReset(true);
            }
        });
    }

    private void showButtonPanel(boolean z) {
        // Log.i(TAG, "**** showButtonPanel " + z);
        samp.runOnUiThread(() -> buttonPanel.show(z));
    }

    private void showVoice(boolean z) {
        // Log.i(TAG, "**** showVoice " + z);
        samp.runOnUiThread(() -> voice.show(z));
    }

    private void showDialog(int dialog_id, int dialog_style, String title, String message, String button1, String button2) {
        Log.i(TAG, "**** showDialog " + dialog_id + " " + title + " " + message + " " + button1 + " " + button2);
        samp.runOnUiThread(() -> dialog.show(dialog_id, dialog_style, title, message, button1, button2));
    }

    private void hideDialog() {
        Log.i(TAG, "**** hideDialog");
        samp.runOnUiThread(() -> {
            if (dialog != null && dialog.isShow) {
                dialog.Hide();
            }
        });
    }

    private void showScoreboard(boolean z) {
        Log.i(TAG, "**** showScoreboard " + z);
        samp.runOnUiThread(() -> scoreboard.show(z));
    }

    private void setScoreboardStats(String server, int players) {
        Log.i(TAG, "**** setScoreboardStats " + server + " " + players);
        samp.runOnUiThread(() -> scoreboard.setStats(server, players));
    }

    private void addScoreboardPlayer(int id, String name, int score, int ping, String color) {
        Log.i(TAG, "**** addScoreboardPlayer " + id + " " + name + " " + score + " " + ping + " " + color);
        samp.runOnUiThread(() -> scoreboard.addPlayer(id, name, score, ping, color));
    }

    private void showWantedLevel(boolean z) {
        // Log.i(TAG, "**** showWantedLevel " + z);
        samp.runOnUiThread(() -> wantedLevel.show(z));
    }

    private void setWantedLevel(int level) {
        Log.i(TAG, "**** setWantedLevel " + level);
        samp.runOnUiThread(() -> wantedLevel.setLevel(level));
    }

    public void showKeyboard(boolean z) {
        Log.i(TAG, "**** showKeyboard " + z);
        // 页面输入框正拿着键盘且该浏览器 controlsChat=true 时，游戏聊天框让位
        // （PC ChatHook::Hook_OpenChatInput -> FocusManager::ShouldBlockChat）。
        if (z && CefManager.shouldBlockChat()) {
            Log.i(TAG, "**** showKeyboard blocked - focused CEF page owns chat input");
            return;
        }
        samp.runOnUiThread(() -> keyboard.show(z));
    }

    private void showChat(boolean z) {
        Log.i(TAG, "**** showChat " + z);
        samp.runOnUiThread(() -> chat.show(z));
    }

    @Override
    public void _sendButtonPanelKey(int id) {
        Log.i(TAG, "**** sendButtonPanelKey " + id);
        sendButtonPanelKey(id);
    }

    @Override
    public void _voicePressed() {
        Log.i(TAG, "**** voicePressed");
        voicePressed();
    }

    @Override
    public void _sendDialogResponse(int dialog_id, int button_id, int list_item, byte[] input) {
        Log.i(TAG, "**** sendDialogResponse " + dialog_id + " " + button_id + " " + list_item + " " + Arrays.toString(input));
        sendDialogResponse(dialog_id, button_id, list_item, input);
    }

    @Override
    public void _sendScoreboardClick(int player_id) {
        Log.i(TAG, "**** _sendScoreboardClick " + player_id);
        sendScoreboardClick(player_id);
    }

    @Override
    public void _keyboardSend(String str) {
        if (str == null) str = "";
        byte[] ret;

        try {
            String charset = samp.getSharedPreferences("samp_settings", Context.MODE_PRIVATE).getString("charset", "GBK");
            ret = str.getBytes(charset);
        } catch (UnsupportedEncodingException e) {
            ret = str.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        }

        try {
            keyboardSend(ret);
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, e.getMessage() != null ? e.getMessage() : "UnsatisfiedLinkError in keyboardSend");
        }
    }

    private static int safeParseColor(String color, int fallback) {
        try {
            return Color.parseColor(color);
        } catch (IllegalArgumentException | NullPointerException e) {
            return fallback;
        }
    }

    public void addChatMessage(String message, String nick, String nickColor) {
        if (message == null) message = "";
        if (nick == null) nick = "";
        String formattedMessage = nick + ": " + message;
        SpannableString spannableString = new SpannableString(formattedMessage);
        spannableString.setSpan(new ForegroundColorSpan(safeParseColor(nickColor, Color.WHITE)), 0, nick.length(), Spannable.SPAN_EXCLUSIVE_EXCLUSIVE);
        spannableString.setSpan(new ForegroundColorSpan(Color.parseColor("#FFFFFF")), nick.length(), formattedMessage.length(), Spannable.SPAN_EXCLUSIVE_EXCLUSIVE);
        samp.runOnUiThread(() -> chat.addMessage(spannableString));
    }

    public void addInfoMessage(String message) {
        if (message == null) message = "";
        SpannableString spannableString = new SpannableString(message);
        spannableString.setSpan(new ForegroundColorSpan(Color.parseColor("#00C8C8")), 0, message.length(), Spannable.SPAN_EXCLUSIVE_EXCLUSIVE);
        samp.runOnUiThread(() -> chat.addMessage(spannableString));
    }

    public void addDebugMessage(String message) {
        if (message == null) message = "";
        SpannableString spannableString = new SpannableString(message);
        spannableString.setSpan(new ForegroundColorSpan(Color.parseColor("#F4A419")), 0, message.length(), Spannable.SPAN_EXCLUSIVE_EXCLUSIVE);
        samp.runOnUiThread(() -> chat.addMessage(spannableString));
    }

    public void addClientMessage(String message, String color) {
        if (message == null) message = "";
        SpannableString spannableString = new SpannableString(message);
        spannableString.setSpan(new ForegroundColorSpan(safeParseColor(color, Color.WHITE)), 0, message.length(), Spannable.SPAN_EXCLUSIVE_EXCLUSIVE);
        samp.runOnUiThread(() -> chat.addMessage(spannableString));
    }

    @Override
    public void chatShowKeyboard(boolean z) {

    }

    @Override
    public void _chatSend(String str) {
        Log.i(TAG, "**** chatSend " + str);
        chatSend(str);
    }

    @Override
    public void onHeightChanged(int orientation, int height) {
        if (keyboard != null) {
            keyboard.onHeightChanged(height);
        }

        if (dialog != null) {
            dialog.onHeightChanged(height);
        }
    }
}

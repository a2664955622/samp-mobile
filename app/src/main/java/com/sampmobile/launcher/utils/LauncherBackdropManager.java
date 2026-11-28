package com.sampmobile.launcher.utils;

import android.content.ContentResolver;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Matrix;
import android.media.ExifInterface;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.ImageView;

import com.bumptech.glide.Glide;
import com.bumptech.glide.load.engine.DiskCacheStrategy;
import com.bumptech.glide.signature.ObjectKey;
import com.sampmobile.launcher.R;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * Manages the user-customizable launcher background image and dimming scrim overlay.
 * Images picked from the system document/photo picker are safely downscaled, EXIF-rotated,
 * and saved atomically into app-private storage so they never require runtime storage permissions
 * or lose URI grants across reboots.
 */
public final class LauncherBackdropManager {
    private static final String PREFS_NAME = "samp_settings";
    private static final String PREF_BACKDROP_VERSION = "launcher_backdrop_version";
    private static final String PREF_BACKDROP_DIM = "launcher_backdrop_dim";
    private static final String BACKDROP_FILE_NAME = "launcher_backdrop.jpg";

    public static final int DEFAULT_DIM_PERCENT = 35; // matches @color/ui_backdrop_scrim (#59000000)
    public static final int MIN_DIM_PERCENT = 0;
    public static final int MAX_DIM_PERCENT = 85;
    private static final int MAX_EDGE_PX = 2560;

    private static final List<BackdropListener> sListeners = new ArrayList<>();
    private static volatile Handler sMainHandler;

    public interface BackdropListener {
        void onBackdropChanged();
    }

    private LauncherBackdropManager() {}

    public static void addListener(BackdropListener listener) {
        if (listener != null && !sListeners.contains(listener)) {
            sListeners.add(listener);
        }
    }

    public static void removeListener(BackdropListener listener) {
        sListeners.remove(listener);
    }

    public static void notifyBackdropChanged() {
        Runnable r = () -> {
            for (BackdropListener listener : new ArrayList<>(sListeners)) {
                listener.onBackdropChanged();
            }
        };
        if (Looper.myLooper() == Looper.getMainLooper()) {
            r.run();
        } else {
            Handler h = sMainHandler;
            if (h == null) {
                h = new Handler(Looper.getMainLooper());
                sMainHandler = h;
            }
            h.post(r);
        }
    }

    public static File getBackdropFile(Context context) {
        return new File(context.getFilesDir(), BACKDROP_FILE_NAME);
    }

    public static boolean hasCustomBackdrop(Context context) {
        if (context == null) return false;
        File f = getBackdropFile(context);
        return f.isFile() && f.length() > 0L;
    }

    public static long getVersion(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        return prefs.getLong(PREF_BACKDROP_VERSION, 0L);
    }

    public static int getDimPercent(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        return clampDimPercent(prefs.getInt(PREF_BACKDROP_DIM, DEFAULT_DIM_PERCENT));
    }

    public static void setDimPercent(Context context, int percent) {
        int clamped = clampDimPercent(percent);
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
                .edit()
                .putInt(PREF_BACKDROP_DIM, clamped)
                .apply();
        notifyBackdropChanged();
    }

    public static int clampDimPercent(int percent) {
        return Math.max(MIN_DIM_PERCENT, Math.min(MAX_DIM_PERCENT, percent));
    }

    public static int scrimColorForDimPercent(int percent) {
        int clamped = clampDimPercent(percent);
        int alpha = Math.round(clamped * 255f / 100f);
        return Color.argb(alpha, 0, 0, 0);
    }

    static int computeSampleSize(int srcWidth, int srcHeight, int maxEdgePx) {
        if (srcWidth <= 0 || srcHeight <= 0 || maxEdgePx <= 0) return 1;
        int sample = 1;
        int maxDim = Math.max(srcWidth, srcHeight);
        while ((maxDim / (sample * 2)) >= maxEdgePx) {
            sample *= 2;
        }
        return sample;
    }

    private static long bumpVersion(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        long next = prefs.getLong(PREF_BACKDROP_VERSION, 0L) + 1L;
        prefs.edit().putLong(PREF_BACKDROP_VERSION, next).apply();
        return next;
    }

    /**
     * Imports an image from a SAF/Gallery URI on a background thread, applying EXIF rotation
     * and capping the long edge to {@value #MAX_EDGE_PX}px to avoid OOM on large camera photos.
     */
    public static void importFromUri(Context context, Uri uri) throws IOException {
        if (context == null || uri == null) {
            throw new IOException("Invalid context or image URI");
        }
        ContentResolver resolver = context.getContentResolver();

        // 1. Probe dimensions without allocating pixel memory
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        try (InputStream is = resolver.openInputStream(uri)) {
            if (is == null) {
                throw new IOException("Unable to open selected image");
            }
            BitmapFactory.decodeStream(is, null, bounds);
        }
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) {
            throw new IOException("Unsupported or corrupted image file");
        }

        // 2. Read EXIF orientation if present (JPG/WEBP/etc.)
        int exifOrientation = ExifInterface.ORIENTATION_NORMAL;
        try (InputStream exifStream = resolver.openInputStream(uri)) {
            if (exifStream != null) {
                ExifInterface exif = new ExifInterface(exifStream);
                exifOrientation = exif.getAttributeInt(
                        ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL);
            }
        } catch (Throwable ignored) {
            // PNG and non-EXIF streams are completely fine.
        }

        // 3. Decode downsampled bitmap
        BitmapFactory.Options decodeOpts = new BitmapFactory.Options();
        decodeOpts.inSampleSize = computeSampleSize(bounds.outWidth, bounds.outHeight, MAX_EDGE_PX);
        decodeOpts.inPreferredConfig = Bitmap.Config.ARGB_8888;
        Bitmap decoded;
        try (InputStream is = resolver.openInputStream(uri)) {
            if (is == null) {
                throw new IOException("Unable to read selected image");
            }
            decoded = BitmapFactory.decodeStream(is, null, decodeOpts);
        }
        if (decoded == null) {
            throw new IOException("Failed to decode image");
        }

        Bitmap finalBitmap = decoded;
        try {
            // 4. Apply EXIF transform and exact max-edge scaling in one transform pass
            Matrix matrix = buildTransformMatrix(decoded.getWidth(), decoded.getHeight(), exifOrientation);
            if (!matrix.isIdentity()) {
                Bitmap transformed = Bitmap.createBitmap(
                        decoded, 0, 0, decoded.getWidth(), decoded.getHeight(), matrix, true);
                if (transformed != decoded) {
                    decoded.recycle();
                    finalBitmap = transformed;
                }
            }

            // 5. Write atomically to app-private storage
            File dst = getBackdropFile(context);
            File tmp = new File(context.getFilesDir(), BACKDROP_FILE_NAME + ".tmp");
            boolean saved = false;
            try (FileOutputStream fos = new FileOutputStream(tmp)) {
                if (!finalBitmap.compress(Bitmap.CompressFormat.JPEG, 92, fos)) {
                    throw new IOException("Failed to encode backdrop JPEG");
                }
                fos.flush();
                if (dst.exists() && !dst.delete()) {
                    // Rename may still replace on POSIX; fall through to rename check.
                }
                if (!tmp.renameTo(dst)) {
                    throw new IOException("Failed to commit backdrop image");
                }
                saved = true;
            } finally {
                if (!saved && tmp.exists()) {
                    tmp.delete();
                }
            }
        } finally {
            if (finalBitmap != null && !finalBitmap.isRecycled()) {
                finalBitmap.recycle();
            }
        }

        bumpVersion(context);
        notifyBackdropChanged();
    }

    private static Matrix buildTransformMatrix(int w, int h, int exifOrientation) {
        Matrix m = new Matrix();
        switch (exifOrientation) {
            case ExifInterface.ORIENTATION_FLIP_HORIZONTAL:
                m.postScale(-1f, 1f);
                break;
            case ExifInterface.ORIENTATION_ROTATE_180:
                m.postRotate(180f);
                break;
            case ExifInterface.ORIENTATION_FLIP_VERTICAL:
                m.postScale(1f, -1f);
                break;
            case ExifInterface.ORIENTATION_TRANSPOSE:
                m.postRotate(90f);
                m.postScale(-1f, 1f);
                break;
            case ExifInterface.ORIENTATION_ROTATE_90:
                m.postRotate(90f);
                break;
            case ExifInterface.ORIENTATION_TRANSVERSE:
                m.postRotate(-90f);
                m.postScale(-1f, 1f);
                break;
            case ExifInterface.ORIENTATION_ROTATE_270:
                m.postRotate(270f);
                break;
            default:
                break;
        }
        int maxDim = Math.max(w, h);
        if (maxDim > MAX_EDGE_PX) {
            float scale = (float) MAX_EDGE_PX / (float) maxDim;
            m.postScale(scale, scale);
        }
        return m;
    }

    public static void clearBackdrop(Context context) {
        if (context == null) return;
        File f = getBackdropFile(context);
        if (f.exists()) {
            f.delete();
        }
        bumpVersion(context);
        notifyBackdropChanged();
    }

    public static void resetDefaults(Context context) {
        if (context == null) return;
        File f = getBackdropFile(context);
        if (f.exists()) {
            f.delete();
        }
        SharedPreferences prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        long next = prefs.getLong(PREF_BACKDROP_VERSION, 0L) + 1L;
        prefs.edit()
                .putLong(PREF_BACKDROP_VERSION, next)
                .putInt(PREF_BACKDROP_DIM, DEFAULT_DIM_PERCENT)
                .apply();
        notifyBackdropChanged();
    }

    /**
     * Applies the active launcher backdrop (custom user wallpaper or default dark gradient)
     * and dimming scrim to the given views. Safe to call repeatedly when settings change.
     */
    public static void applyToViews(Context context, ImageView backdropView, View scrimView) {
        if (context == null) return;
        if (scrimView != null) {
            scrimView.setBackgroundColor(scrimColorForDimPercent(getDimPercent(context)));
        }
        if (backdropView == null) return;
        if (hasCustomBackdrop(context)) {
            File file = getBackdropFile(context);
            long ver = getVersion(context);
            Glide.with(backdropView)
                    .load(file)
                    .signature(new ObjectKey(ver))
                    .diskCacheStrategy(DiskCacheStrategy.NONE)
                    .centerCrop()
                    .error(R.drawable.launcher_backdrop)
                    .into(backdropView);
        } else {
            Glide.with(backdropView).clear(backdropView);
            backdropView.setImageResource(R.drawable.launcher_backdrop);
        }
    }
}

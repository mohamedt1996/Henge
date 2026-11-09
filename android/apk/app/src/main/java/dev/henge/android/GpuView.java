package dev.henge.android;

import android.content.Context;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

import java.io.File;

/**
 * The Mac's screen drawn by the app's own Vulkan renderer (libhengegpu.so, android/gpu/replay), which
 * takes macOS's GPU commands from the engine through gpu.link (Graphics: Vulkan). Touch and keys still
 * go over VNC, through the RfbView laid over this one.
 */
public class GpuView extends SurfaceView implements SurfaceHolder.Callback {
    static boolean loaded;
    static {
        try { System.loadLibrary("hengegpu"); loaded = true; } catch (Throwable t) { loaded = false; }
    }

    private long handle;

    GpuView(Context c) {
        super(c);
        getHolder().addCallback(this);
    }

    /** Where macOS's shaders are kept once translated: Android/data/dev.henge.android/files/spirv. */
    static File cacheDir(Context c) {
        File ext = c.getExternalFilesDir(null);
        return new File(ext != null ? ext : c.getFilesDir(), "spirv");
    }

    void start() {
        if (!loaded || handle != 0) return;
        Context c = getContext();
        String link = new File(VmService.macDir(c), "gpu.link").getPath();
        String tool = c.getApplicationInfo().nativeLibraryDir + "/libair2spv.so";
        handle = nativeStart(link, tool, cacheDir(c).getPath());
        if (getHolder().getSurface() != null && getHolder().getSurface().isValid()) nativeSurface(handle, getHolder().getSurface());
    }

    void stop() {
        if (handle == 0) return;
        nativeStop(handle);
        handle = 0;
    }

    @Override public void surfaceCreated(SurfaceHolder h) { if (handle != 0) nativeSurface(handle, h.getSurface()); }
    @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int hh) { if (handle != 0) nativeSurface(handle, h.getSurface()); }
    @Override public void surfaceDestroyed(SurfaceHolder h) { if (handle != 0) nativeSurface(handle, null); }

    static native long nativeStart(String linkPath, String air2spvPath, String cacheDir);
    static native void nativeSurface(long handle, Object surface);
    static native void nativeStop(long handle);
    /**
     * Graphics: reims: the paravirtual GPU device itself (reims-vgpu), for the VM VmService runs. homeDir is a
     * folder of the app's that becomes the process's HOME: metal2vulkan keeps its translated shaders in
     * $HOME/.cache and refuses to make a device without one (an app process has no HOME of its own).
     */
    static native boolean nativeReimsStart(String reimsPath, String logPath, String homeDir);
    static native void nativeReimsStop();
}

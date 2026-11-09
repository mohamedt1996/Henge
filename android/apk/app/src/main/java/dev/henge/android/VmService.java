package dev.henge.android;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Environment;
import android.os.IBinder;
import android.os.PowerManager;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

/** Runs the engine (libhengevm.so, an executable) in the foreground so Android keeps it alive. */
public class VmService extends Service {
    public static final String EXTRA_MODE = "mode"; // "install" or "run"
    public static final String EXTRA_RAM = "ram";
    public static final String EXTRA_CPUS = "cpus";
    public static final String EXTRA_RELEASE = "release";
    private static final String CHANNEL = "vm";

    /** "idle", "installing", "running", or how the last run ended. */
    public static volatile String state = "idle";
    private static volatile Process process;
    private PowerManager.WakeLock wake;

    public static File macDir(Context c) { return new File(c.getFilesDir(), "mac"); }
    /** Shader captures go where a computer can copy them over USB: Android/data/dev.henge.android/files/shaders. */
    public static File shaderDir(Context c) {
        File ext = c.getExternalFilesDir(null);
        return new File(ext != null ? ext : c.getFilesDir(), "shaders");
    }
    public static File logFile(Context c) { return new File(c.getFilesDir(), "henge.log"); }

    /** Where downloads are kept (restore images, the boot ROM): Henge/ on the tablet's storage when the app has
     *  all-files access, so they outlive an uninstall; else the app's own folder, which outlives updates. */
    public static File libraryDir(Context c) {
        File d = sharedLibrary() != null ? sharedLibrary() : appLibrary(c);
        d.mkdirs();
        return d;
    }
    static File sharedLibrary() {
        return android.os.Build.VERSION.SDK_INT >= 30 && Environment.isExternalStorageManager() ? new File(Environment.getExternalStorageDirectory(), "Henge") : null;
    }
    static File appLibrary(Context c) {
        File ext = c.getExternalFilesDir("downloads");
        return ext != null ? ext : new File(c.getFilesDir(), "downloads");
    }
    /** Every place a download may be, newest home first (older builds kept it inside the Mac). */
    static List<File> imageDirs(Context c) {
        List<File> l = new ArrayList<>();
        if (sharedLibrary() != null) l.add(sharedLibrary());
        l.add(appLibrary(c));
        l.add(macDir(c));
        return l;
    }
    /** A complete restore image of this release (its name holds the version, as Apple's and Henge's do). */
    public static File findImage(Context c, String release) {
        File best = null;
        for (File d : imageDirs(c)) {
            File[] f = d.listFiles((x, n) -> n.endsWith(".ipsw") && n.contains(release));
            if (f != null) for (File x : f) if (x.length() > (1L << 30) && (best == null || x.length() > best.length())) best = x;
        }
        return best;
    }
    /** Moves downloads from older homes into the library (a rename: same storage, so instant), else leaves them. */
    static void gatherImages(Context c) {
        File lib = libraryDir(c);
        for (File d : imageDirs(c)) {
            if (d.equals(lib)) continue;
            File[] f = d.listFiles((x, n) -> n.endsWith(".ipsw") || n.endsWith(".ipsw.part") || n.equals("AVPBooter.vmapple2.bin"));
            if (f == null) continue;
            for (File x : f) {
                if (d.equals(macDir(c)) && x.getName().equals("AVPBooter.vmapple2.bin")) continue; // the Mac's own copy stays
                File to = new File(lib, x.getName());
                if (!to.exists()) x.renameTo(to);
            }
        }
    }
    public static boolean installed(Context c) {
        return new File(macDir(c), "machine.json").exists() && new File(c.getFilesDir(), "installed").exists();
    }
    public static boolean busy() { Process p = process; return p != null && p.isAlive(); }

    @Override public IBinder onBind(Intent i) { return null; }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null || busy()) return START_NOT_STICKY;
        String mode = intent.getStringExtra(EXTRA_MODE);
        int ram = intent.getIntExtra(EXTRA_RAM, 4096), cpus = intent.getIntExtra(EXTRA_CPUS, 2);
        String release = intent.getStringExtra(EXTRA_RELEASE);
        startForeground(1, notification("install".equals(mode) ? "Installing macOS" : "Mac running"),
                ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        wake = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "henge:vm");
        wake.acquire();
        boolean deleteImage = intent.getBooleanExtra("deleteImage", false);
        String gpu = intent.getStringExtra("gpu");
        new Thread(() -> run(mode, ram, cpus, release == null ? "15.6.1" : release, deleteImage,
                gpu == null ? "on" : gpu), "engine").start();
        return START_NOT_STICKY;
    }

    private void run(String mode, int ram, int cpus, String release, boolean deleteImage, String gpu) {
        boolean install = "install".equals(mode);
        state = install ? "installing" : "running";
        File files = getFilesDir(), mac = macDir(this), log = logFile(this), pidFile = new File(files, "engine.pid");
        gatherImages(this);
        File lib = libraryDir(this), image = findImage(this, release);
        String engine = getApplicationInfo().nativeLibraryDir + "/libhengevm.so";
        List<String> cmd = new ArrayList<>();
        // sh: a bigger stack (each guest CPU keeps a large structure on it) and the engine's pid for Stop.
        cmd.add("/system/bin/sh");
        cmd.add("-c");
        cmd.add("ulimit -s 65520 2>/dev/null; echo $$ > \"$HENGE_PID\"; exec \"$0\" \"$@\"");
        cmd.add(engine);
        if (install) {
            cmd.add("--install"); cmd.add(mac.getPath());
            cmd.add("--release"); cmd.add(release);
            if (image != null) { cmd.add("--ipsw"); cmd.add(image.getPath()); }
            cmd.add("--cache"); cmd.add(lib.getPath());
            cmd.add("--vnc"); cmd.add("5900");
        } else {
            cmd.add("--mac"); cmd.add(mac.getPath());
            cmd.add("--vnc"); cmd.add("5900");
            cmd.add("--gpu"); cmd.add(gpu);
            cmd.add("--cache"); cmd.add(lib.getPath()); // a missing boot ROM comes back from here
            if ("capture".equals(gpu)) { cmd.add("--shader-dir"); cmd.add(shaderDir(this).getPath()); }
        }
        cmd.add("--ram"); cmd.add(Integer.toString(ram));
        cmd.add("--cpus"); cmd.add(Integer.toString(cpus));
        int code = -1;
        try {
            ProcessBuilder pb = new ProcessBuilder(cmd);
            pb.directory(files);
            pb.environment().put("HOME", files.getPath());
            pb.environment().put("TMPDIR", getCacheDir().getPath());
            pb.environment().put("HENGE_PID", pidFile.getPath());
            // Android's seccomp filter kills glibc's per-thread rseq registration on many devices.
            pb.environment().put("GLIBC_TUNABLES", "glibc.pthread.rseq=0");
            pb.redirectErrorStream(true);
            pb.redirectOutput(ProcessBuilder.Redirect.appendTo(log));
            try (FileOutputStream o = new FileOutputStream(log, true)) {
                o.write(("\n=== " + String.join(" ", cmd.subList(3, cmd.size())) + "\n").getBytes(StandardCharsets.UTF_8));
            }
            process = pb.start();
            code = process.waitFor();
        } catch (Exception e) {
            appendLog(log, "henge app: could not start the engine: " + e);
        }
        if (install) {
            // The restore tools' own log sits in the app's private files; put a copy where a computer
            // sees it over USB: Android/data/dev.henge.android/files/install.log.
            File rlog = new File(mac, "restore/install.log"), ext = getExternalFilesDir(null);
            if (rlog.exists() && ext != null) {
                try { Files.copy(rlog.toPath(), new File(ext, "install.log").toPath(),
                        java.nio.file.StandardCopyOption.REPLACE_EXISTING); } catch (Exception ignored) { }
            }
        }
        if (install && code == 0) {
            try { new File(files, "installed").createNewFile(); } catch (Exception ignored) { }
            // The restore image is 17 GB; kept by default so a reinstall needs no download.
            File done = findImage(this, release);
            if (deleteImage && done != null) done.delete();
        }
        // Graphics: Vulkan keeps guest RAM as a named file for the renderer, which removes the name once it
        // has it mapped; if it never attached, the file (as big as the Mac's memory) goes now.
        new File(mac, "disk.img.ram").delete();
        if (code > 128) appendLog(log, "henge app: the engine was killed by " + signalName(code - 128)
                + (code - 128 == 31 ? " (a syscall Android's seccomp filter does not allow)" : ""));
        state = install ? (code == 0 ? "installed" : "install failed (" + code + ")")
                        : (code == 0 ? "stopped" : "stopped (" + code + ")");
        process = null;
        if (wake != null && wake.isHeld()) wake.release();
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
    }

    /** SIGTERM: the engine stops the Mac at the next block and flushes its disks. */
    public static void stop(Context c) {
        try {
            String pid = new String(Files.readAllBytes(new File(c.getFilesDir(), "engine.pid").toPath())).trim();
            android.os.Process.sendSignal(Integer.parseInt(pid), 15);
        } catch (Exception e) {
            Process p = process;
            if (p != null) p.destroy();
        }
    }

    static String signalName(int sig) {
        switch (sig) {
            case 4: return "SIGILL"; case 6: return "SIGABRT"; case 7: return "SIGBUS"; case 9: return "SIGKILL";
            case 11: return "SIGSEGV"; case 15: return "SIGTERM"; case 31: return "SIGSYS";
            default: return "signal " + sig;
        }
    }

    private static void appendLog(File log, String line) {
        try (FileOutputStream o = new FileOutputStream(log, true)) {
            o.write((line + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (Exception ignored) { }
    }

    private Notification notification(String text) {
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Virtual Mac", NotificationManager.IMPORTANCE_LOW));
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class),
                PendingIntent.FLAG_IMMUTABLE);
        return new Notification.Builder(this, CHANNEL)
                .setContentTitle("Henge")
                .setContentText(text)
                .setSmallIcon(android.R.drawable.ic_media_play)
                .setContentIntent(open)
                .setOngoing(true)
                .build();
    }
}

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

    /** The verbose log where a computer sees it over USB: Android/data/dev.henge.android/files/verbose.log. */
    static void exportLog(Context c) {
        File ext = c.getExternalFilesDir(null), log = logFile(c);
        if (ext == null || !log.exists()) return;
        try { Files.copy(log.toPath(), new File(ext, "verbose.log").toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING); }
        catch (Exception ignored) { }
    }

    /** Copies a file, or a folder as a zip, into the tablet's Download folder (MediaStore: no permission needed).
     *  Returns the name it got, or null if there was nothing to copy or it failed. */
    static String toDownloads(Context c, File src, String name, String mime) {
        if (src == null || !src.exists()) return null;
        android.content.ContentValues v = new android.content.ContentValues();
        v.put(android.provider.MediaStore.Downloads.DISPLAY_NAME, name);
        v.put(android.provider.MediaStore.Downloads.MIME_TYPE, mime);
        v.put(android.provider.MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS + "/Henge");
        android.net.Uri uri = c.getContentResolver().insert(android.provider.MediaStore.Downloads.EXTERNAL_CONTENT_URI, v);
        if (uri == null) return null;
        try (java.io.OutputStream out = c.getContentResolver().openOutputStream(uri)) {
            if (src.isDirectory()) {
                try (java.util.zip.ZipOutputStream z = new java.util.zip.ZipOutputStream(out)) { zipInto(z, src, ""); }
            } else {
                Files.copy(src.toPath(), out);
            }
            return name;
        } catch (Exception e) {
            c.getContentResolver().delete(uri, null, null);
            return null;
        }
    }

    private static void zipInto(java.util.zip.ZipOutputStream z, File dir, String prefix) throws java.io.IOException {
        File[] fs = dir.listFiles();
        if (fs == null) return;
        for (File f : fs) {
            String n = prefix + f.getName();
            if (f.isDirectory()) { zipInto(z, f, n + "/"); continue; }
            z.putNextEntry(new java.util.zip.ZipEntry(n));
            Files.copy(f.toPath(), z);
            z.closeEntry();
        }
    }

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
        engineMode = "gunyah".equals(intent.getStringExtra("engine")) ? "gunyah" : "jit";
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
        cmd.add("--ram"); cmd.add(Integer.toString(gunyah() ? Math.min(ram, 3072) : ram));
        cmd.add("--cpus"); cmd.add(Integer.toString(cpus));
        if (gunyah()) { cmd.add("--engine"); cmd.add("gunyah"); }
        // Graphics: reims: the GPU device runs in this process (libhengegpu.so), up before the engine needs it.
        boolean reims = !install && "reims".equals(gpu);
        // reims-vgpu views guest memory a 4 KB mapping at a time where Android has no /dev/udmabuf: at the default
        // vm.max_map_count (65530) that covers ~190 MB of the Mac's RAM. Raise it where root is there (best effort).
        if (reims) {
            try {
                Process p = new ProcessBuilder("su", "-c", "sysctl -w vm.max_map_count=2097152").redirectErrorStream(true).start();
                StringBuilder sb = new StringBuilder();
                try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8))) {
                    for (String l; (l = r.readLine()) != null; ) sb.append(l).append(' ');
                }
                String out = sb.toString().trim();
                appendLog(log, "henge app: " + (p.waitFor() == 0 ? out : "could not raise vm.max_map_count (no root?): " + out));
            } catch (Exception e) { appendLog(log, "henge app: could not raise vm.max_map_count: " + e); }
        }
        // The last run's gpu.reims is stale: the app must attach only to the one this engine is about to make.
        if (reims) new File(mac, "gpu.reims").delete();
        // A run whose app process died (native crash, low-memory kill) writes no exit line: say so on the next start.
        File active = new File(files, "run.active");
        if (active.exists()) appendLog(log, "henge app: the last run ended with this app killed (a crash or Android's "
                + "low-memory killer) and the engine with it; no exit code was seen");
        try { active.createNewFile(); } catch (Exception ignored) { }
        if (reims && !(GpuView.loaded && GpuView.nativeReimsStart(new File(mac, "gpu.reims").getPath(), log.getPath())))
            appendLog(log, "henge app: this build has no reims-vgpu; the GPU will not answer");
        int code = -1;
        try {
            // Gunyah: /dev/gunyah is closed to apps, so the engine runs as root through su; afterwards everything it
            // made in the app's folder is handed back to the app.
            List<String> launch = cmd;
            if (gunyah()) {
                StringBuilder q = new StringBuilder();
                q.append("export HOME=").append(sq(files.getPath())).append(" TMPDIR=").append(sq(getCacheDir().getPath()))
                 .append(" HENGE_PID=").append(sq(pidFile.getPath()))
                 .append(" HENGE_APP_UID=").append(android.os.Process.myUid()) // files the renderer opens while it runs
                 .append(" GLIBC_TUNABLES=glibc.pthread.rseq=0; cd ")
                 .append(sq(files.getPath())).append(" && ulimit -s 65520 2>/dev/null; touch engine.root; ");
                for (String a : cmd.subList(3, cmd.size())) q.append(sq(a)).append(' ');
                int uid = android.os.Process.myUid();
                // In the background so the shell can tidy up after it; the pid file names the engine itself.
                q.append("& echo $! > \"$HENGE_PID\"; wait $!; code=$?; chown -R ").append(uid).append(':').append(uid).append(' ').append(sq(files.getPath()))
                 .append(' ').append(sq(getCacheDir().getPath())).append("; restorecon -RF ").append(sq(files.getPath()))
                 .append(" 2>/dev/null; rm -f engine.root; exit $code");
                launch = new ArrayList<>();
                launch.add("su"); launch.add("-c"); launch.add(q.toString());
            }
            ProcessBuilder pb = new ProcessBuilder(launch);
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
        if (reims && GpuView.loaded) GpuView.nativeReimsStop();
        active.delete();
        if (!install) appendLog(log, "henge app: the engine exited (" + code + ")");
        exportLog(this);
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

    static volatile String engineMode = "jit";
    static boolean gunyah() { return "gunyah".equals(engineMode); }
    static String sq(String s) { return "'" + s.replace("'", "'\\''") + "'"; }

    /** SIGTERM: the engine stops the Mac at the next block and flushes its disks. */
    public static void stop(Context c) {
        try {
            String pid = new String(Files.readAllBytes(new File(c.getFilesDir(), "engine.pid").toPath())).trim();
            if (gunyah() || new File(c.getFilesDir(), "engine.root").exists()) { // a root process: only root may signal it
                new ProcessBuilder("su", "-c", "kill -15 " + Integer.parseInt(pid)).start();
                return;
            }
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

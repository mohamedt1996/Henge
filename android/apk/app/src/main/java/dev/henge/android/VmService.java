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
    /** Graphics: reims: HOME for reims-vgpu in this process; its shader cache is $HOME/.cache/reims-vgpu-vulkan. */
    public static File reimsHomeDir(Context c) {
        File d = new File(c.getFilesDir(), "reims-home");
        d.mkdirs();
        return d;
    }

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
        // A run whose app process died (native crash, low-memory kill) writes no exit line, and the next start
        // must not walk into the same crash: "run.active" holds the GPU mode of a run still going.
        // The file holds "<mode>@<versionCode>": a new build of the app may well have fixed the crash, so only a
        // crash of this same build sends the run to Graphics: GPU (the first start after an update otherwise
        // fell back silently, and the fix went untested).
        File active = new File(files, "run.active");
        String build = Integer.toString(buildNumber(this));
        // Graphics: reims on Turnip: asked for, bundled, and not the driver that took this build down before.
        // Two Turnips may be bundled: Henge's own build (I/O-coherent dma-buf imports: the Mac's memory in place,
        // GpuView.turnipHenge) and the stock one. Henge's first; if it took this build down, the stock one; if
        // that did too, the system's driver.
        android.content.SharedPreferences prefs = getSharedPreferences("henge", MODE_PRIVATE);
        boolean turnipAsked = "reims".equals(gpu) && !install && "turnip".equals(prefs.getString("reimsDriver", "turnip"));
        boolean ours = turnipAsked && GpuView.turnipHenge(this) != null && !"stock".equals(prefs.getString("turnipBuild", "henge"))
                && !build.equals(prefs.getString("turnipHengeCrashed", ""));
        boolean turnip = ours || (turnipAsked && GpuView.turnip(this) != null && !build.equals(prefs.getString("turnipCrashed", "")));
        if (active.exists()) {
            int[] death = logLastCrash(log);
            // Android ending the app (memory short, the user, an update) is no fault of the GPU's: only a death
            // Android records as a crash, or one it has no record of, changes what the next run uses. A SIGKILL
            // (status 9) is the low-memory killer's on a device that reports it as "signalled".
            boolean crashed = death == null || death[0] == android.app.ApplicationExitInfo.REASON_CRASH_NATIVE
                    || death[0] == android.app.ApplicationExitInfo.REASON_CRASH
                    || death[0] == android.app.ApplicationExitInfo.REASON_INITIALIZATION_FAILURE
                    || (death[0] == android.app.ApplicationExitInfo.REASON_SIGNALED && death[1] != 9);
            String last = "";
            try { last = new String(Files.readAllBytes(active.toPath()), StandardCharsets.UTF_8).trim(); } catch (Exception ignored) { }
            String lastMode = last.contains("@") ? last.substring(0, last.indexOf('@')) : last;
            String lastBuild = last.contains("@") ? last.substring(last.indexOf('@') + 1) : "";
            appendLog(log, "henge app: the last run ended with this app killed (a crash or Android's low-memory killer) "
                    + "and the engine with it; no exit code was seen");
            // A run on Gunyah that ended so: the tablet reset. What the kernel said last (host/gunyah.c kept it).
            File kmsg = new File(files, "gunyah-kmsg.txt");
            if (kmsg.exists()) {
                String tail = DeviceCheck.sh(true, "tail -n 80 " + sq(kmsg.getPath()) + "; true");
                appendLog(log, "henge app: that run was on Gunyah; the kernel's last messages in it:\n"
                        + (tail.trim().isEmpty() ? "  (none were kept)" : tail));
                File ext = getExternalFilesDir(null);
                if (ext != null) DeviceCheck.sh(true, "cp " + sq(kmsg.getPath()) + " " + sq(new File(ext, "gunyah-kmsg-last.txt").getPath())
                        + "; rm -f " + sq(kmsg.getPath()) + "; true");
            }
            if (!crashed && lastMode.startsWith("reims") && "reims".equals(gpu) && !"install".equals(mode)) {
                appendLog(log, "henge app: Android ended the app (" + exitReason(death[0]) + "), which is not a crash of the "
                        + "GPU's: reims and its Vulkan driver are used as before. If it was memory, close other apps or lower "
                        + "the Mac's memory or the JIT cache (Settings)");
            } else if ("reims-turnip-henge".equals(lastMode) && "reims".equals(gpu) && !"install".equals(mode)
                    && new File(reimsHomeDir(this), "render-in-place").exists()) {
                // The run that died had the framebuffer in place on (Settings, an experiment that had not run on a
                // tablet when it shipped): that is the first suspect, and the driver keeps its place.
                new File(reimsHomeDir(this), "render-in-place").delete();
                appendLog(log, "henge app: the run that took the app down had Settings > Framebuffer in place on (an "
                        + "experiment): it is off again, and Henge's Turnip is used as before. Its \"reims:\" and "
                        + "\"metal2vulkan:\" lines above say where it died");
            } else if ("reims-turnip-henge".equals(lastMode) && "reims".equals(gpu) && !"install".equals(mode) && build.equals(lastBuild)) {
                // Henge's own Turnip died: the stock one next, on copies, until the app is updated.
                prefs.edit().putString("turnipHengeCrashed", build).apply();
                ours = false;
                turnip = turnipAsked && GpuView.turnip(this) != null && !build.equals(prefs.getString("turnipCrashed", ""));
                appendLog(log, "henge app: reims on Henge's Turnip took the app down last time (its \"reims:\" lines above say "
                        + "where); this build uses " + (turnip ? "the stock Turnip" : "the system's Vulkan driver")
                        + " from now on. An update of the app tries Henge's Turnip again");
            } else if ("reims-turnip".equals(lastMode) && "reims".equals(gpu) && !"install".equals(mode) && build.equals(lastBuild)) {
                // Turnip's death is not reims's: the same screen on the system's driver, until the app is updated.
                prefs.edit().putString("turnipCrashed", build).apply();
                turnip = ours;
                appendLog(log, "henge app: reims on Turnip took the app down last time (its \"reims:\" lines above say where); "
                        + "this build uses the system's Vulkan driver from now on. An update of the app tries Turnip again");
            } else if (lastMode.startsWith("reims") && "reims".equals(gpu) && !"install".equals(mode)) {
                if (build.equals(lastBuild)) {
                    gpu = "on";
                    appendLog(log, "henge app: reims took the app down last time (its \"reims:\" lines above say where); "
                            + "this run uses Graphics: GPU. The next start tries reims again");
                } else {
                    appendLog(log, "henge app: that was build " + (lastBuild.isEmpty() ? "?" : lastBuild) + "; this is build "
                            + build + ", so reims is tried again");
                }
            }
        }
        try (FileOutputStream o = new FileOutputStream(active)) {
            o.write((("install".equals(mode) ? "install" : turnip && "reims".equals(gpu) ? (ours ? "reims-turnip-henge" : "reims-turnip") : gpu)
                    + "@" + build).getBytes(StandardCharsets.UTF_8));
        } catch (Exception ignored) { }
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
        boolean sound = !install && !"off".equals(getSharedPreferences("henge", MODE_PRIVATE).getString("sound", "on"));
        cmd.add("--sound"); cmd.add(sound ? "1" : "0");
        cmd.add("--ram"); cmd.add(Integer.toString(gunyah() ? Math.min(ram, 3072) : ram));
        cmd.add("--cpus"); cmd.add(Integer.toString(cpus));
        // Settings > JIT cache: the translated code's room, 768 MB where nothing is set (host/vm.c).
        int jitMb = prefs.getInt("jitMb", 0);
        if (!install && !gunyah() && jitMb >= 64) { cmd.add("--jit-mb"); cmd.add(Integer.toString(jitMb)); }
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
        if (reims) udmabuf(log, "on".equals(prefs.getString("zerocopy", "off")));
        // The last run's gpu.reims is stale: the app must attach only to the one this engine is about to make.
        if (reims) new File(mac, "gpu.reims").delete();

        // reims-vgpu's shader cache (translated Metal shaders and the driver's pipelines) lives under HOME, which this
        // process does not have: give it a folder that outlives the process, so a second boot finds the first's work.
        // The Vulkan driver reims-vgpu runs on: the system's, or Turnip where the build bundles it and the setting asks.
        String vulkanLib = null;
        if (reims) {
            File lib2 = ours ? GpuView.turnipHenge(this) : GpuView.turnip(this);
            if (turnip && lib2 != null) {
                vulkanLib = lib2.getPath();
                if (!ours && GpuView.turnipHenge(this) != null)
                    appendLog(log, "henge app: the stock Turnip is used (" + ("stock".equals(prefs.getString("turnipBuild", "henge"))
                            ? "Settings" : "Henge's own took this build of the app down before") + "): the GPU works on copies of the Mac's memory");
                else if (!ours)
                    appendLog(log, "henge app: this build has no Turnip of Henge's own (its CI step failed): the stock one, on copies of the Mac's memory");
            }
            else if (turnipAsked && lib2 == null) appendLog(log, "henge app: this build has no Turnip; the system's Vulkan driver is used");
            else if (turnipAsked) appendLog(log, "henge app: Turnip took this build of the app down before; the system's Vulkan driver is used");
        }
        if (reims && !(GpuView.loaded && GpuView.nativeReimsStart(new File(mac, "gpu.reims").getPath(), log.getPath(),
                reimsHomeDir(this).getPath(), vulkanLib)))
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
                 .append(" HV_GUNYAH_KMSG=").append(sq(new File(files, "gunyah-kmsg.txt").getPath())) // host/gunyah.c
                 .append(gunyahFound()) // what the Gunyah test found this tablet's resource manager takes
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
            // The Mac's RAM in memory, not in a file on the tablet's storage (devices/vmapple.c): unless Settings
            // asks for storage, or the app could not open it on this build (the GPU's note, android/gpu/reims/host.c).
            File memfdNote = new File(reimsHomeDir(this), "ram-memfd-failed");
            if (memfdNote.exists()) { prefs.edit().putString("ramFileBuild", build).apply(); memfdNote.delete(); }
            boolean ramInMemory = !install && !"storage".equals(prefs.getString("ram", "memory"))
                    && !build.equals(prefs.getString("ramFileBuild", ""));
            if (ramInMemory) pb.environment().put("HV_RAM_MEMFD", "1");
            // Huge pages: On asks for them as the Mac touches its memory; Eager also touches all of it at the start,
            // while the tablet still has 2 MB runs of free memory to give (devices/vmapple.c).
            String huge = prefs.getString("hugepages", "off");
            if (!install && hugePages(log, ramInMemory && !gunyah() && !"off".equals(huge)))
                pb.environment().put("HV_RAM_THP", "eager".equals(huge) ? "2" : "1");
            // The Mac's disk: the data is flushed at each of macOS's flushes (fdatasync), not the file's times too.
            pb.environment().put("HV_FLUSH", "2");
            // Graphics: reims: the Mac's screen size (Settings), every pixel of which is copied on this driver.
            if (reims) pb.environment().put("HV_REIMS_MODE", getSharedPreferences("henge", MODE_PRIVATE).getString("reimsMode", "1280x800"));
            pb.redirectErrorStream(true);
            pb.redirectOutput(ProcessBuilder.Redirect.appendTo(log));
            try (FileOutputStream o = new FileOutputStream(log, true)) {
                o.write(("\n=== " + String.join(" ", cmd.subList(3, cmd.size())) + "\n").getBytes(StandardCharsets.UTF_8));
            }
            process = pb.start();
            if (sound) new Thread(() -> playSound(log), "sound").start();
            if (!install) new Thread(() -> favourEngine(pidFile, log), "favour").start();
            if (!install) fullSpeed(log, "on".equals(getSharedPreferences("henge", MODE_PRIVATE).getString("fullspeed", "off")));
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
        if (!install) fullSpeed(log, false);
        if (!install) hugePages(log, false);
        if (gunyah()) DeviceCheck.sh(true, "rm -f " + sq(new File(files, "gunyah-kmsg.txt").getPath()) + "; true"); // it ended: no reset
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

    /** Android's record of this app's last death (ApplicationExitInfo, Android 11+), with the tombstone's
     *  backtrace from Android 12: the native crash the app's own handler may not have caught. Answers its reason
     *  and status, or null where Android keeps no record. */
    private int[] logLastCrash(File log) {
        if (android.os.Build.VERSION.SDK_INT < 30) return null;
        int[] death = null;
        try {
            android.app.ActivityManager am = (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
            java.util.List<android.app.ApplicationExitInfo> exits = am.getHistoricalProcessExitReasons(null, 0, 1);
            if (exits.isEmpty()) return null;
            android.app.ApplicationExitInfo e = exits.get(0);
            death = new int[]{e.getReason(), e.getStatus()};
            appendLog(log, "henge app: Android's record of the last death: reason " + e.getReason() + " (" + exitReason(e.getReason())
                    + "), status " + e.getStatus() + ", " + e.getDescription() + ", at " + new java.util.Date(e.getTimestamp()));
            if (android.os.Build.VERSION.SDK_INT >= 31 && e.getReason() == android.app.ApplicationExitInfo.REASON_CRASH_NATIVE) {
                try (java.io.InputStream in = e.getTraceInputStream()) {
                    if (in == null) { appendLog(log, "henge app: no tombstone kept"); return death; }
                    java.io.BufferedReader r = new java.io.BufferedReader(new java.io.InputStreamReader(in, StandardCharsets.UTF_8));
                    StringBuilder sb = new StringBuilder("henge app: tombstone:\n");
                    int lines = 0;
                    boolean inFrames = false;
                    for (String l; (l = r.readLine()) != null && lines < 120; ) {
                        // The header (signal, fault address, abort message), the registers' first lines and the backtrace.
                        if (l.startsWith("signal ") || l.startsWith("Abort message") || l.startsWith("Cause") || l.startsWith("pid:"))
                            { sb.append("  ").append(l).append('\n'); lines++; }
                        if (l.startsWith("backtrace:")) inFrames = true;
                        else if (inFrames && l.trim().startsWith("#")) { sb.append("  ").append(l.trim()).append('\n'); lines++; }
                        else if (inFrames && l.trim().isEmpty()) inFrames = false;
                    }
                    appendLog(log, sb.toString());
                }
            }
        } catch (Exception ex) {
            appendLog(log, "henge app: could not read Android's record of the last death: " + ex);
        }
        return death;
    }

    private static String exitReason(int r) {
        switch (r) {
            case android.app.ApplicationExitInfo.REASON_CRASH_NATIVE: return "native crash";
            case android.app.ApplicationExitInfo.REASON_CRASH: return "Java crash";
            case android.app.ApplicationExitInfo.REASON_LOW_MEMORY: return "low memory";
            case android.app.ApplicationExitInfo.REASON_ANR: return "ANR";
            case android.app.ApplicationExitInfo.REASON_SIGNALED: return "signalled";
            case android.app.ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "excessive resource usage";
            case android.app.ApplicationExitInfo.REASON_USER_REQUESTED: return "user requested";
            case android.app.ApplicationExitInfo.REASON_OTHER: return "other";
            case android.app.ApplicationExitInfo.REASON_USER_STOPPED: return "stopped by the user";
            case android.app.ApplicationExitInfo.REASON_DEPENDENCY_DIED: return "a process it depends on died";
            case android.app.ApplicationExitInfo.REASON_PERMISSION_CHANGE: return "a permission changed";
            case android.app.ApplicationExitInfo.REASON_EXIT_SELF: return "it exited by itself";
            case android.app.ApplicationExitInfo.REASON_INITIALIZATION_FAILURE: return "it failed to start";
            case 14: return "frozen too long";   // REASON_FREEZER, API 34
            case 15: return "its package changed state"; // REASON_PACKAGE_STATE_CHANGE, API 34
            case 16: return "the app was updated";       // REASON_PACKAGE_UPDATED, API 34
            default: return "?";
        }
    }

    /**
     * The engine among Android's foreground work. Android schedules by group: a process outside "top-app"
     * may be kept off the fastest cores and held to lower CPU frequencies, and the engine is a child process
     * Android knows nothing about. With root, it is put into the top-app groups (which cores it may use, and
     * how eagerly the CPU speeds up for it); without root nothing changes, and the log says so.
     */
    private void favourEngine(File pidFile, File log) {
        try {
            String pid = "";
            for (int i = 0; i < 20 && pid.isEmpty(); i++) {
                Thread.sleep(500);
                try { pid = new String(Files.readAllBytes(pidFile.toPath()), StandardCharsets.UTF_8).trim(); } catch (Exception ignored) { }
            }
            if (!pid.matches("[0-9]+") || !busy()) return;
            String sh = "n=0; for d in /dev/cpuset/top-app /dev/cpuctl/top-app /dev/stune/top-app; do "
                    + "[ -e $d/cgroup.procs ] && echo " + pid + " > $d/cgroup.procs && n=$((n+1)); done; echo $n";
            Process p = new ProcessBuilder("su", "-c", sh).redirectErrorStream(true).start();
            StringBuilder sb = new StringBuilder();
            try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8))) {
                for (String l; (l = r.readLine()) != null; ) sb.append(l).append(' ');
            }
            String out = sb.toString().trim();
            appendLog(log, p.waitFor() == 0 && out.matches("[1-9]")
                    ? "henge app: the engine runs among Android's foreground work (top-app, " + out + " groups)"
                    : "henge app: could not put the engine among Android's foreground work (no root?): " + out);
        } catch (Exception e) {
            appendLog(log, "henge app: could not put the engine among Android's foreground work: " + e);
        }
    }

    /**
     * Settings > Full speed (root): every CPU's frequency governor set to `performance` while the Mac runs, so
     * the cores the JIT runs on stay at their highest frequency instead of climbing to it after each pause.
     * The governors as they were are kept in the preferences and put back when the Mac stops; a stop that
     * never got here (the app killed) is put right at the next start, on or off.
     */
    private void fullSpeed(File log, boolean on) {
        android.content.SharedPreferences p = getSharedPreferences("henge", MODE_PRIVATE);
        String saved = p.getString("governors", "");
        if (!saved.isEmpty()) {
            StringBuilder sh = new StringBuilder();
            for (String s : saved.split(" ")) {
                String[] kv = s.split("=");
                if (kv.length == 2 && kv[0].matches("policy[0-9]+") && kv[1].matches("[a-z_]+"))
                    sh.append("echo ").append(kv[1]).append(" > /sys/devices/system/cpu/cpufreq/").append(kv[0]).append("/scaling_governor; ");
            }
            DeviceCheck.sh(true, sh + "true");
            p.edit().remove("governors").commit();
            appendLog(log, "henge app: full speed off: the CPU governors are as they were (" + saved + ")");
        }
        if (!on) return;
        String was = DeviceCheck.sh(true, "for p in /sys/devices/system/cpu/cpufreq/policy*; do "
                + "grep -qw performance $p/scaling_available_governors || continue; "
                + "echo -n \"${p##*/}=$(cat $p/scaling_governor) \"; done; true").trim();
        if (was.isEmpty()) {
            appendLog(log, "henge app: full speed: no root, or this kernel has no `performance` governor");
            return;
        }
        p.edit().putString("governors", was).commit();
        DeviceCheck.sh(true, "for p in /sys/devices/system/cpu/cpufreq/policy*; do echo performance > $p/scaling_governor; done; true");
        appendLog(log, "henge app: full speed: CPU governors set to performance (were " + was + "); now "
                + DeviceCheck.sh(true, "cat /sys/devices/system/cpu/cpufreq/policy*/scaling_governor | tr '\\n' ' '; true").trim());
    }

    /**
     * Settings > Huge pages (root): the kernel's huge pages for shared memory set to `advise` while the Mac
     * runs, so the Mac's RAM (a memfd the engine asks huge pages for, devices/vmapple.c) is in 2 MB pages.
     * The setting as it was is kept and put back when the Mac stops, or at the next start. Answers whether
     * the kernel now gives them.
     *
     * 0.3.53: asking was not enough. On the tablet (build 57) On showed no huge pages in the log and Eager 14 to
     * 18 MB of 4096. The kernel hands one out only where 2 MB of memory lie free in one piece at that moment; the
     * likely cause (not measured until this build's log lines) is free memory in crumbs after hours of use, and
     * kernel settings that do not make room. So, while the Mac runs (each put back after): the free memory is
     * compacted once before the engine starts ({@link #hugeCompact}), and the kernel's own settings for huge
     * pages are set so that it tries for one when the Mac touches its memory and gathers the Mac's small pages
     * into huge ones in the background ({@link #hugeKnobs}). The log says what each step found.
     */
    private boolean hugePages(File log, boolean on) {
        android.content.SharedPreferences p = getSharedPreferences("henge", MODE_PRIVATE);
        final String file = THP + "shmem_enabled";
        String saved = p.getString("shmemHuge", "");
        if (saved.matches("[a-z_]+")) {
            DeviceCheck.sh(true, "echo " + saved + " > " + file + "; true");
            p.edit().remove("shmemHuge").commit();
            appendLog(log, "henge app: huge pages off: shared memory's are `" + saved + "` again");
        }
        hugeKnobsBack(log, p);
        if (!on) return false;
        // The file reads like "always within_size advise [never] deny force": the one in brackets is set.
        String now = DeviceCheck.sh(true, "cat " + file + "; true").trim();
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("\\[([a-z_]+)\\]").matcher(now);
        if (!m.find() || !now.contains("advise")) {
            appendLog(log, "henge app: huge pages: no root, or this kernel has none for shared memory (" + now + ")");
            return false;
        }
        String was = m.group(1);
        if (was.equals("never") || was.equals("deny")) {
            p.edit().putString("shmemHuge", was).commit();
            DeviceCheck.sh(true, "echo advise > " + file + "; true");
        }
        String after = DeviceCheck.sh(true, "cat " + file + "; true").trim();
        appendLog(log, "henge app: huge pages: shared memory's were `" + was + "`, now: " + after);
        boolean gives = after.contains("[advise]") || after.contains("[always]") || after.contains("[within_size]") || after.contains("[force]");
        if (gives) {
            try { hugeKnobs(log, p); } catch (Exception e) { appendLog(log, "henge app: huge pages: the kernel's settings were left as they are: " + e); }
            try { hugeCompact(log); } catch (Exception e) { appendLog(log, "henge app: huge pages: memory was not compacted: " + e); }
        }
        return gives;
    }

    private static final String THP = "/sys/kernel/mm/transparent_hugepage/";

    /** The value a sysfs file of the kernel's shows as set: the word in brackets, or the file's one word. */
    private static String setting(String text) {
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("\\[([a-z+_]+)\\]").matcher(text);
        return m.find() ? m.group(1) : text.trim();
    }

    /**
     * The kernel's huge-page settings while the Mac runs (root), those that differ kept in the preference
     * `thpKnobs` (lines of file=value) for {@link #hugeKnobsBack}:
     * - `enabled` never -> madvise: khugepaged, the kernel thread that gathers small pages into huge ones, runs
     *   only while this is not `never` (as Linux 6.1's source reads; not checked on a tablet); `madvise` gives huge pages
     *   to memory that asks, as the Mac's RAM does, and to nothing else.
     * - `defrag` never -> madvise, defer -> defer+madvise: where memory that asked is first touched and no 2 MB
     *   piece is free, the kernel makes one (moves pages aside) instead of giving small pages at once.
     * - khugepaged: 16384 pages looked at every 2 s (the kernel's default, 4096 every 10 s, takes 43 minutes
     *   to pass over 4 GB once; this takes about 2), and a 2 MB range gathered only where at most 64 of its
     *   512 pages are untouched (default 511: one touched page makes 2 MB, which would grow the Mac's memory).
     * Its pause after a huge page it could not get stays the kernel's (a minute): where the tablet has no
     * memory for them, it does not keep trying.
     */
    private void hugeKnobs(File log, android.content.SharedPreferences p) {
        final String[] files = {"enabled", "defrag", "khugepaged/pages_to_scan", "khugepaged/scan_sleep_millisecs", "khugepaged/max_ptes_none"};
        StringBuilder read = new StringBuilder("cd " + THP + " || exit 0; ");
        for (String f : files) read.append("echo \"").append(f).append("=$(cat ").append(f).append(" 2>/dev/null)\"; ");
        java.util.Map<String, String> now = new java.util.LinkedHashMap<>();
        for (String line : DeviceCheck.sh(true, read + "true").split("\n")) {
            int eq = line.indexOf('=');
            if (eq > 0) now.put(line.substring(0, eq).trim(), setting(line.substring(eq + 1)));
        }
        java.util.Map<String, String> want = new java.util.LinkedHashMap<>();
        String enabled = now.get("enabled"), defrag = now.get("defrag");
        if ("never".equals(enabled)) want.put("enabled", "madvise");
        if ("never".equals(defrag)) want.put("defrag", "madvise");
        else if ("defer".equals(defrag)) want.put("defrag", "defer+madvise");
        long scan = number(now.get("khugepaged/pages_to_scan")), sleep = number(now.get("khugepaged/scan_sleep_millisecs")),
             none = number(now.get("khugepaged/max_ptes_none"));
        if (scan >= 0 && scan < 16384) want.put("khugepaged/pages_to_scan", "16384");
        if (sleep > 2000) want.put("khugepaged/scan_sleep_millisecs", "2000");
        if (none > 64) want.put("khugepaged/max_ptes_none", "64");
        StringBuilder keep = new StringBuilder(), write = new StringBuilder("cd " + THP + " || exit 0; "), said = new StringBuilder();
        for (java.util.Map.Entry<String, String> e : want.entrySet()) {
            String was = now.get(e.getKey());
            if (was == null || !was.matches("[a-z0-9+_]+")) continue; // not a value this could put back
            keep.append(e.getKey()).append('=').append(was).append('\n');
            write.append("echo ").append(e.getValue()).append(" > ").append(e.getKey()).append("; ");
            said.append(said.length() > 0 ? ", " : "").append(e.getKey()).append(' ').append(was).append(" -> ").append(e.getValue());
        }
        if (keep.length() > 0) {
            p.edit().putString("thpKnobs", keep.toString()).commit(); // before the writes: a kill between them still puts them back
            DeviceCheck.sh(true, write + "true");
        }
        appendLog(log, "henge app: huge pages: the kernel's settings while the Mac runs: "
                + (said.length() > 0 ? said : "left as they are") + " (they were: " + now + ")");
    }

    private static long number(String s) {
        try { return s == null ? -1 : Long.parseLong(s.trim()); } catch (NumberFormatException e) { return -1; }
    }

    /** Puts back what {@link #hugeKnobs} changed (when the Mac stops, or at the next start after a kill). */
    private void hugeKnobsBack(File log, android.content.SharedPreferences p) {
        String kept = p.getString("thpKnobs", "");
        if (kept.isEmpty()) return;
        StringBuilder write = new StringBuilder("cd " + THP + " || exit 0; ");
        int n = 0;
        for (String line : kept.split("\n")) {
            int eq = line.indexOf('=');
            if (eq <= 0) continue;
            String f = line.substring(0, eq), v = line.substring(eq + 1);
            if (!f.matches("[a-z_/]+") || !v.matches("[a-z0-9+_]+")) continue;
            write.append("echo ").append(v).append(" > ").append(f).append("; ");
            n++;
        }
        if (n > 0) DeviceCheck.sh(true, write + "true");
        p.edit().remove("thpKnobs").commit();
        appendLog(log, "henge app: huge pages off: " + n + " of the kernel's settings are as they were again");
    }

    /**
     * The tablet's free memory gathered into whole pieces before the engine takes the Mac's RAM (root:
     * /proc/sys/vm/compact_memory), with how much of it lay in pieces of 2 MB or more before and after
     * (/proc/buddyinfo): that figure is the most the Mac's first touches can get as huge pages.
     */
    private void hugeCompact(File log) {
        long before = freeInHugePieces();
        long t0 = android.os.SystemClock.elapsedRealtime();
        DeviceCheck.sh(true, "echo 1 > /proc/sys/vm/compact_memory; true", 30);
        long ms = android.os.SystemClock.elapsedRealtime() - t0, after = freeInHugePieces();
        appendLog(log, "henge app: huge pages: " + (before < 0 ? "?" : String.valueOf(before)) + " MB of the tablet's free memory lay in pieces of 2 MB or more; "
                + (after < 0 ? "?" : String.valueOf(after)) + " MB after compacting it (" + ms + " ms)");
    }

    /** Free memory in pieces of 2 MB or more, in MB, from /proc/buddyinfo (a count per order after each zone's name); -1 unread. */
    private static long freeInHugePieces() {
        String text = DeviceCheck.sh(true, "cat /proc/buddyinfo; true");
        long page = android.system.Os.sysconf(android.system.OsConstants._SC_PAGESIZE), bytes = 0;
        boolean any = false;
        for (String line : text.split("\n")) {
            int zone = line.indexOf("zone");
            if (zone < 0) continue;
            String[] t = line.substring(zone + 4).trim().split("\\s+");
            for (int order = 0; order + 1 < t.length; order++) { // t[0] is the zone's name
                long size = page << order, count = number(t[order + 1]);
                if (count > 0 && size >= (2L << 20)) bytes += count * size;
                any = true;
            }
        }
        return any ? bytes >> 20 : -1;
    }

    /**
     * Settings > Zero-copy (root, a kernel with /dev/udmabuf: android/kernel/README.md). The device is root's
     * and of a kind Android lets no app open, so with the setting on it is made this app's alone to open: a
     * SELinux type of its own that app domains may open and send requests to (live policy, until reboot), and
     * this app's UID as owner with mode 600, so every other app is still refused. Its limits are raised
     * (pieces and MB a dma-buf) so a view of the Mac's scattered pages is few dma-bufs. reims-vgpu then maps
     * views of the Mac's memory as dma-bufs, and a Vulkan driver that imports dma-bufs (Turnip) uses the Mac's
     * pages in place of copies. Off: the device is root's alone again (owner, mode and SELinux type), and
     * reims-vgpu works as without it.
     * reims-vgpu opens the device once a process, so a change takes hold when the app is started afresh.
     */
    private void udmabuf(File log, boolean on) {
        if (!new File("/dev/udmabuf").exists()) {
            if (on) appendLog(log, "henge app: zero-copy: this kernel has no /dev/udmabuf (android/kernel/README.md)");
            return;
        }
        if (!on) {
            DeviceCheck.sh(true, "chown 0:0 /dev/udmabuf; chmod 600 /dev/udmabuf; chcon u:object_r:device:s0 /dev/udmabuf; true");
            return;
        }
        int uid = android.os.Process.myUid();
        String out = DeviceCheck.sh(true, "magiskpolicy --live 'type henge_udmabuf' 'typeattribute henge_udmabuf dev_type' "
                + "'typeattribute henge_udmabuf mlstrustedobject' 'allow henge_udmabuf tmpfs filesystem associate' "
                + "'allow henge_udmabuf device filesystem associate' "
                + "'allow untrusted_app_all henge_udmabuf chr_file { getattr open read write ioctl map }' >/dev/null 2>&1 "
                + "&& chcon u:object_r:henge_udmabuf:s0 /dev/udmabuf || echo 'own SELinux type failed;'; "
                + "chown " + uid + ":" + uid + " /dev/udmabuf; chmod 600 /dev/udmabuf; "
                + "echo 8192 > /sys/module/udmabuf/parameters/list_limit; echo 256 > /sys/module/udmabuf/parameters/size_limit_mb; "
                + "ls -lZ /dev/udmabuf; echo limits $(cat /sys/module/udmabuf/parameters/list_limit) pieces, "
                + "$(cat /sys/module/udmabuf/parameters/size_limit_mb) MB; true").trim().replace('\n', ' ');
        String opens;
        try {
            java.io.FileDescriptor fd = android.system.Os.open("/dev/udmabuf", android.system.OsConstants.O_RDWR, 0);
            android.system.Os.close(fd);
            opens = "this app opens it";
        } catch (Exception e) {
            opens = "this app CANNOT open it (" + e.getMessage() + "): reims-vgpu will work as without it";
        }
        appendLog(log, "henge app: zero-copy: " + (out.isEmpty() ? "no root" : out) + "; " + opens);
    }

    private static int le32(byte[] b, int at) {
        return (b[at] & 0xff) | (b[at + 1] & 0xff) << 8 | (b[at + 2] & 0xff) << 16 | (b[at + 3] & 0xff) << 24;
    }

    /**
     * The Mac's sound. The engine has none of Android's audio libraries: it serves the guest's PCM on
     * 127.0.0.1:5901 (android/audio_android.c: messages of 'H','S',type,0,length; type 1 the format,
     * type 2 PCM), and this plays it. In the service, so the Mac is heard with its screen closed too.
     */
    private void playSound(File log) {
        byte[] head = new byte[8], body = new byte[1 << 16];
        java.nio.ByteBuffer out = java.nio.ByteBuffer.allocateDirect(1 << 16).order(java.nio.ByteOrder.nativeOrder());
        boolean said = false;
        while (busy()) {
            android.media.AudioTrack track = null;
            try (java.net.Socket s = new java.net.Socket()) {
                s.connect(new java.net.InetSocketAddress("127.0.0.1", 5901), 1000);
                s.setTcpNoDelay(true);
                java.io.DataInputStream in = new java.io.DataInputStream(s.getInputStream());
                int format = 0;
                while (true) {
                    in.readFully(head);
                    int len = le32(head, 4);
                    if (head[0] != 'H' || head[1] != 'S' || len < 0 || len > body.length) throw new java.io.IOException("not the engine's sound");
                    in.readFully(body, 0, len);
                    if (head[2] == 1 && len >= 12) {
                        int channels = le32(body, 0), rate = le32(body, 8);
                        format = le32(body, 4);
                        if (track != null) { track.release(); track = null; }
                        try {
                            track = soundTrack(channels, format, rate);
                            track.play();
                            if (!said) appendLog(log, "henge app: sound: playing " + channels + " channels at " + rate + " Hz");
                        } catch (Exception e) {
                            if (!said) appendLog(log, "henge app: sound: Android refused " + channels + " channels, format " + format + ", " + rate + " Hz: " + e);
                            track = null;
                        }
                        said = true;
                    } else if (head[2] == 2 && track != null) {
                        out.clear();
                        if (format == 17) { // 32-bit integers: as floats, which every Android plays
                            for (int i = 0; i + 4 <= len; i += 4) out.putFloat(le32(body, i) / 2147483648f);
                        } else {
                            out.put(body, 0, len);
                        }
                        out.flip();
                        // Never waits: what the track has no room for is dropped, so the sound stays on time
                        // (the Mac's clock and the tablet's drift apart by a little, for ever).
                        track.write(out, out.remaining(), android.media.AudioTrack.WRITE_NON_BLOCKING);
                    }
                }
            } catch (Exception e) {
                // no engine yet, or it left: again in a moment, while there is one
            } finally {
                if (track != null) { try { track.release(); } catch (Exception ignored) { } }
            }
            try { Thread.sleep(1000); } catch (InterruptedException e) { return; }
        }
    }

    /** format: virtio-snd's number (5: 16-bit, 17: 32-bit, 19: float). */
    private static android.media.AudioTrack soundTrack(int channels, int format, int rate) {
        int encoding = format == 5 ? android.media.AudioFormat.ENCODING_PCM_16BIT : android.media.AudioFormat.ENCODING_PCM_FLOAT;
        android.media.AudioFormat.Builder f = new android.media.AudioFormat.Builder().setEncoding(encoding).setSampleRate(rate);
        if (channels == 1) f.setChannelMask(android.media.AudioFormat.CHANNEL_OUT_MONO);
        else if (channels == 2) f.setChannelMask(android.media.AudioFormat.CHANNEL_OUT_STEREO);
        else f.setChannelIndexMask((1 << channels) - 1);
        int frame = channels * (format == 5 ? 2 : 4);
        int least = android.media.AudioTrack.getMinBufferSize(rate,
                channels == 1 ? android.media.AudioFormat.CHANNEL_OUT_MONO : android.media.AudioFormat.CHANNEL_OUT_STEREO, encoding);
        int size = Math.max(least > 0 ? least : 0, rate / 10 * frame); // a tenth of a second, or what the device needs
        return new android.media.AudioTrack.Builder()
                .setAudioAttributes(new android.media.AudioAttributes.Builder()
                        .setUsage(android.media.AudioAttributes.USAGE_MEDIA)
                        .setContentType(android.media.AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setAudioFormat(f.build())
                .setBufferSizeInBytes(size)
                .setTransferMode(android.media.AudioTrack.MODE_STREAM)
                .build();
    }

    static int buildNumber(Context c) {
        try { return (int) c.getPackageManager().getPackageInfo(c.getPackageName(), 0).getLongVersionCode(); }
        catch (Exception e) { return 0; }
    }

    static volatile String engineMode = "jit";
    static boolean gunyah() { return "gunyah".equals(engineMode); }

    /**
     * How the engine gives its VM to Gunyah (host/gunyah.c), from what the Gunyah test found: the first region lent
     * if only a lent VM started, and the way of naming memory whose memory the guest could write. Nothing found:
     * the engine's own choices.
     */
    private String gunyahFound() {
        String layout = GunyahTest.layout(this), ram = GunyahTest.ram(this);
        StringBuilder b = new StringBuilder();
        if (layout != null && layout.contains("--lend")) b.append(" HV_GUNYAH_LEND_BOOT=1");
        if (ram != null && ram.contains("resmem")) b.append(" HV_GUNYAH_RAM=resmem");
        else if (ram != null && ram.contains("plain")) b.append(" HV_GUNYAH_RAM=plain");
        return b.toString();
    }
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

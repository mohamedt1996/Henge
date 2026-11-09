package dev.henge.android;

import android.app.ActivityManager;
import android.content.Context;
import android.content.pm.FeatureInfo;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.StatFs;
import android.system.Os;
import android.system.OsConstants;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.TimeUnit;

/** What this tablet offers Henge (android/tools/device_check.sh, in the app). Root, when granted, reads what the
 *  app alone cannot: the hypervisor devices and kernel settings. */
final class DeviceCheck {
    private DeviceCheck() { }

    static String run(Context c) {
        StringBuilder b = new StringBuilder();
        boolean root = "0".equals(sh(true, "id -u").trim());
        line(b, "device", Build.MANUFACTURER + " " + Build.MODEL + " (" + Build.DEVICE + "), Android " + Build.VERSION.RELEASE
                + " / API " + Build.VERSION.SDK_INT);
        line(b, "chip", (Build.VERSION.SDK_INT >= 31 ? Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL + " / " : "")
                + prop("ro.board.platform"));
        line(b, "kernel", System.getProperty("os.version"));
        line(b, "root", root ? "yes (su granted)" : "no (or su was denied)");
        for (String d : new String[]{"/dev/kvm", "/dev/gunyah", "/dev/gzvm"}) {
            String r = root ? sh(true, "ls -lZ " + d).trim() : "";
            line(b, d, !r.isEmpty() ? r : new File(d).exists() ? "present (no root to read it)" : "absent");
        }
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        c.getSystemService(ActivityManager.class).getMemoryInfo(mi);
        line(b, "memory", (mi.totalMem >> 20) + " MB, " + (mi.availMem >> 20) + " MB free now");
        line(b, "storage", MainActivity.bytes(new StatFs(c.getFilesDir().getPath()).getAvailableBytes()) + " free");
        line(b, "page size", Long.toString(Os.sysconf(OsConstants._SC_PAGESIZE)));
        line(b, "Vulkan", vulkan(c) + ", driver " + orDash(prop("ro.hardware.vulkan")));
        line(b, "GPU", orDash(read("/sys/class/kgsl/kgsl-3d0/gpu_model")));
        line(b, "SELinux", orDash(sh(false, "getenforce").trim()));
        line(b, "huge pages", root ? orDash(sh(true, "cat /sys/kernel/mm/transparent_hugepage/enabled").trim()) : "needs root");
        line(b, "governor", orDash(read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")));
        StringBuilder cores = new StringBuilder();
        for (int i = 0; i < Runtime.getRuntime().availableProcessors(); i++) {
            String f = read("/sys/devices/system/cpu/cpu" + i + "/cpufreq/cpuinfo_max_freq");
            String mhz;
            try { mhz = (Long.parseLong(f) / 1000) + "MHz"; } catch (NumberFormatException e) { mhz = "?"; }
            cores.append("cpu").append(i).append('=').append(mhz).append(' ');
        }
        line(b, "cores", cores.toString().trim());
        line(b, "all-files", VmService.sharedLibrary() != null ? "granted: downloads outlive an uninstall" : "not granted");
        line(b, "downloads", VmService.libraryDir(c).getPath());
        String lib = c.getApplicationInfo().nativeLibraryDir;
        line(b, "engine", new File(lib, "libhengevm.so").canExecute() ? "ok" : "missing");
        line(b, "air2spv", new File(lib, "libair2spv.so").canExecute() ? "ok" : "missing");
        String out = b.toString();
        File ext = c.getExternalFilesDir(null);
        if (ext != null) try (FileOutputStream o = new FileOutputStream(new File(ext, "device-check.txt"))) {
            o.write(out.getBytes(StandardCharsets.UTF_8));
        } catch (Exception ignored) { }
        return out;
    }

    private static String vulkan(Context c) {
        int version = 0, level = -1;
        for (FeatureInfo f : c.getPackageManager().getSystemAvailableFeatures()) {
            if (PackageManager.FEATURE_VULKAN_HARDWARE_VERSION.equals(f.name)) version = f.version;
            if (PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL.equals(f.name)) level = f.version;
        }
        if (version == 0) return "none";
        return (version >> 22) + "." + ((version >> 12) & 0x3ff) + " (level " + level + ")";
    }

    private static void line(StringBuilder b, String k, String v) {
        b.append(String.format(java.util.Locale.US, "%-11s %s%n", k + ":", v));
    }
    private static String orDash(String s) { return s == null || s.isEmpty() ? "—" : s; }
    private static String prop(String name) { return sh(false, "getprop " + name).trim(); }
    private static String read(String path) {
        try { return new String(Files.readAllBytes(new File(path).toPath()), StandardCharsets.UTF_8).trim(); }
        catch (Exception e) { return ""; }
    }

    /** A shell command's output (as root through su when asked), given up after ten seconds. */
    static String sh(boolean su, String cmd) {
        try {
            Process p = new ProcessBuilder(su ? new String[]{"su", "-c", cmd} : new String[]{"/system/bin/sh", "-c", cmd})
                    .redirectErrorStream(true).start();
            StringBuilder out = new StringBuilder();
            Thread t = new Thread(() -> {
                try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()))) {
                    String l;
                    while ((l = r.readLine()) != null) out.append(l).append('\n');
                } catch (Exception ignored) { }
            });
            t.start();
            if (!p.waitFor(10, TimeUnit.SECONDS)) { p.destroy(); return ""; }
            t.join(1000);
            return p.exitValue() == 0 ? out.toString() : "";
        } catch (Exception e) {
            return "";
        }
    }
}

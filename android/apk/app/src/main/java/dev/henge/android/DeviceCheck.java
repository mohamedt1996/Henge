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
        line(b, "udmabuf", udmabuf(root));
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
        // Gunyah itself is not tried here: making a VM can reset this tablet. "Test Gunyah" (GunyahTest) does,
        // a step a tap.
        line(b, "Gunyah", new File(lib, "libghprobe.so").canExecute() ? "tested a step at a time, under \"Test Gunyah\"" : "probe missing");
        if (root) {
            // Where Gunyah's resource manager might say why it refuses a VM: its log does not reach dmesg.
            String found = sh(true, "mount -t debugfs none /sys/kernel/debug 2>/dev/null; "
                    + "echo '-- debugfs:'; ls /sys/kernel/debug 2>/dev/null | grep -iE 'gunyah|^gh|hyp|rsc|vmm|haven'; "
                    + "for d in /sys/kernel/debug/gunyah* /sys/kernel/debug/gh* /sys/kernel/debug/hyp*; do [ -e $d ] && ls -la $d 2>/dev/null | head -20; done; "
                    + "echo '-- devices:'; ls /dev 2>/dev/null | grep -iE 'gunyah|^gh|hyp|vm_|rm_'; "
                    + "echo '-- device tree /hypervisor:'; ls /sys/firmware/devicetree/base/hypervisor 2>/dev/null | head -40; "
                    + "echo '-- modules:'; lsmod 2>/dev/null | grep -iE 'gunyah|^gh_|hyp|crosvm|vm_' | head -30; "
                    + "echo '-- VMs of Android itself (AVF):'; getprop | grep -iE 'hypervisor|virtualization|pvmfw|protected_vm' | head -12; "
                    + "ls /apex/com.android.virt/bin 2>/dev/null | head -12; ls -la /dev/block/by-name 2>/dev/null | grep -iE 'pvmfw|vm-|hyp' | head -8; "
                    + "ls /vendor/vm-system /vendor/bin 2>/dev/null | grep -iE 'vm|crosvm|qcrosvm|gunyah' | head -12; "
                    // The resource manager's own messages, from the hypervisor image: which version's rules it has
                    // (android/tools/gunyah_probe.c reads Qualcomm's source; these say how far that source is its).
                    + "echo \"-- the resource manager's own words (hyp partition):\"; "
                    + "h=/dev/block/by-name/hyp$(getprop ro.boot.slot_suffix); [ -e $h ] && strings -n 8 $h 2>/dev/null | grep -iE "
                    + "'neither shared nor exclusive|must be private|parse device tree|map device tree|accept image memparcel|"
                    + "ACL validation disabled|invalid base tag|only one memparcel|memory node|allow-unprotected|"
                    + "address space limits|gunyah-[0-9a-f]{6,}|RM version|resource-manager.*(20[0-9]{2}|v[0-9])' | sort -u | head -40; "
                    +"echo '-- the kernel about Gunyah and KVM since boot:'; dmesg 2>/dev/null | grep -iE 'gunyah|gh_rsc|gh_|hypervisor|kvm|HYP mode|EL2' | head -40; true", 20);
            if (!found.trim().isEmpty()) b.append("\nGunyah, what this system has:\n").append(found);
        }
        String gunyah = GunyahTest.report(c);
        if (!gunyah.isEmpty()) b.append("\nGunyah test so far:\n").append(gunyah);
        String out = b.toString();
        File ext = c.getExternalFilesDir(null);
        if (ext != null) try (FileOutputStream o = new FileOutputStream(new File(ext, "device-check.txt"))) {
            o.write(out.getBytes(StandardCharsets.UTF_8));
        } catch (Exception ignored) { }
        return out;
    }

    /**
     * /dev/udmabuf: the kernel device that makes a dma-buf of memfd pages. With it reims-vgpu views the Mac's
     * memory a mapping a view, not a mapping a run of pages, and a Vulkan driver that imports dma-bufs can use
     * the Mac's memory in place, with no copies (android/gpu/REIMS.md). Is it there; if not, is it in this
     * kernel's configuration, and is there a module of it that could be loaded?
     */
    private static String udmabuf(boolean root) {
        if (new File("/dev/udmabuf").exists()) {
            String ls = root ? sh(true, "ls -lZ /dev/udmabuf").trim() : "";
            String limits = read("/sys/module/udmabuf/parameters/list_limit");
            String size = read("/sys/module/udmabuf/parameters/size_limit_mb");
            return "present" + (ls.isEmpty() ? "" : ": " + ls)
                    + (limits.isEmpty() ? "" : "; at most " + limits + " pieces and " + size + " MB a buffer");
        }
        if (!root) return "absent (/dev/udmabuf); root would say whether the kernel has it";
        // Each command ends in true: a grep or find that matches nothing is an answer, not a failure.
        String config = sh(true, "zcat /proc/config.gz 2>/dev/null | grep -i UDMABUF; true").trim().replace('\n', ' ');
        String module = sh(true, "find /vendor_dlkm /system_dlkm /vendor/lib/modules /system/lib/modules /odm/lib/modules "
                + "-name '*udmabuf*' 2>/dev/null | head -3; true").trim().replace('\n', ' ');
        String loaded = sh(true, "ls -d /sys/module/udmabuf /sys/class/misc/udmabuf 2>/dev/null; true").trim().replace('\n', ' ');
        StringBuilder s = new StringBuilder("absent (/dev/udmabuf). kernel config: ");
        s.append(config.isEmpty() ? "no answer (/proc/config.gz unreadable or silent on it)" : config);
        s.append("; module file: ").append(module.isEmpty() ? "none found" : module);
        if (!loaded.isEmpty()) s.append("; in the running kernel: ").append(loaded);
        s.append(". ");
        if (!module.isEmpty()) s.append("Verdict: a module exists and is not loaded; it could be loaded with root.");
        else if (!loaded.isEmpty() || config.contains("=y")) s.append("Verdict: the kernel has it, and no device node was made for it.");
        else if (config.contains("=m")) s.append("Verdict: built as a module, and its file was not found.");
        else s.append("Verdict: this kernel does not have it; it would take a kernel module built for this kernel.");
        return s.toString();
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
    static String sh(boolean su, String cmd) { return sh(su, cmd, 10); }

    static String sh(boolean su, String cmd, int seconds) {
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
            if (!p.waitFor(seconds, TimeUnit.SECONDS)) { p.destroy(); return ""; }
            t.join(1000);
            return p.exitValue() == 0 ? out.toString() : "";
        } catch (Exception e) {
            return "";
        }
    }
}

package dev.henge.android;

import android.content.Context;
import android.content.SharedPreferences;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;

/**
 * Gunyah, a step at a time. Starting the Mac on Gunyah resets the Xiaomi Pad 7 and leaves nothing to read, so
 * this walks from the smallest thing (opening /dev/gunyah) towards the engine's VM, one step a tap, each a run
 * of the probe (android/tools/gunyah_probe.c) as root. Before a step its number is saved, and the probe forces
 * a line to storage before each thing it asks of Gunyah: if the tablet resets, the next start of the app finds
 * the step unfinished and says which step, and which request inside it, the tablet did not survive.
 */
final class GunyahTest {
    private GunyahTest() { }

    /**
     * {what the step does, the probe's arguments, seconds the guest may run, kind}.
     * "pre": before the VM starts. "start": one way of laying the VM out, started and not run; a refusal goes on
     * to the next way, and the first that starts is the layout the "use" steps run with (%L in their arguments).
     * The ways are in the order of how little they resemble what resets the tablet: the engine's own is last.
     */
    static final String[][] STEPS = {
            {"Open /dev/gunyah", "--until open", "6", "pre"},
            {"Create an empty VM", "--until create", "6", "pre"},
            {"Give it 2 MB of memory", "--until memory", "6", "pre"},
            {"Give it its device tree", "--until tree", "6", "pre"},
            {"Add a CPU", "--until vcpu", "6", "pre"},
            {"Start: 2 MB at 0x80000000 (refused on the Pad 7: memory invalid)", "", "6", "start"},
            {"Start: everything at or above the base address (base 0)", "--layout zero", "6", "start"},
            {"Start: 16 MB at 0x80000000", "--image-mb 16", "6", "start"},
            {"Start: 2 MB at 0x80000000, memory lent", "--lend", "6", "start"},
            {"Start: base 0, memory lent", "--layout zero --lend", "6", "start"},
            {"Start: the engine's layout (ROM below the base address; may reset)", "--layout engine", "6", "start"},
            {"Run a tiny program in it", "%L", "6", "use"},
            {"256 MB of memory", "%L --mem 256", "6", "use"},
            {"256 MB, every page written", "%L --mem 256 --touch", "20", "use"},
            {"1 GB, every page written", "%L --mem 1024 --touch", "40", "use"},
            {"3 GB, every page written", "%L --mem 3072 --touch", "90", "use"},
            {"2 CPUs", "%L --mem 256 --cpus 2", "10", "use"},
            {"4 CPUs", "%L --mem 256 --cpus 4", "10", "use"},
            {"The Mac's interrupt doorbells", "%L --mem 256 --cpus 4 --bells", "10", "use"},
            {"All of it: 3 GB, 4 CPUs, doorbells", "%L --mem 3072 --cpus 4 --bells --touch", "90", "use"},
    };
    private static final int VERSION = 3; // of STEPS: progress saved by another list starts over

    static volatile boolean running;

    private static SharedPreferences prefs(Context c) {
        SharedPreferences p = c.getSharedPreferences("gunyah-test", Context.MODE_PRIVATE);
        if (p.getInt("version", VERSION) != VERSION || (!p.contains("version") && p.contains("next"))) {
            String old = p.getString("report", "");
            p.edit().clear().putInt("version", VERSION)
                    .putString("report", old.isEmpty() ? "" : old + "(The steps changed with this version of Henge: starting from step 1.)\n\n").commit();
        } else if (!p.contains("version")) {
            p.edit().putInt("version", VERSION).commit();
        }
        return p;
    }
    private static File kmsg(Context c) { return new File(c.getFilesDir(), "gunyah-test-kmsg.txt"); }

    /** The first layout that started: what the later steps run with. Empty: none has yet. */
    static String layout(Context c) { return prefs(c).getString("layout", null); }
    private static File crumbs(Context c) { return new File(c.getFilesDir(), "gunyah-crumbs.txt"); }

    /** The step a tap runs next: the first not passed. STEPS.length when all passed. */
    static int next(Context c) { return prefs(c).getInt("next", 0); }

    /** Everything said so far, newest last. */
    static String report(Context c) { return prefs(c).getString("report", ""); }

    static void startOver(Context c) {
        prefs(c).edit().clear().commit();
        crumbs(c).delete();
    }

    /**
     * A step that began and never ended, with this process not running it: the tablet reset (or the app was
     * killed) during it. Noted in the report, once. Answers whether there was one.
     */
    static boolean noteReset(Context c) {
        SharedPreferences p = prefs(c);
        int k = p.getInt("began", -1);
        if (k < 0 || running) return false;
        String last = "";
        try {
            List<String> lines = Files.readAllLines(crumbs(c).toPath(), StandardCharsets.UTF_8);
            StringBuilder b = new StringBuilder();
            for (int i = Math.max(0, lines.size() - 4); i < lines.size(); i++) b.append("    ").append(lines.get(i)).append('\n');
            last = b.toString();
        } catch (Exception ignored) { }
        String title = k < STEPS.length ? STEPS[k][0] : "?";
        String kernel = "";
        try {
            List<String> lines = Files.readAllLines(kmsg(c).toPath(), StandardCharsets.UTF_8);
            StringBuilder b = new StringBuilder();
            for (int i = Math.max(0, lines.size() - 40); i < lines.size(); i++) b.append("    ").append(lines.get(i)).append('\n');
            kernel = b.toString();
        } catch (Exception ignored) { }
        String said = "Step " + (k + 1) + " (" + title + ") DID NOT END: the tablet reset, or the app was killed, during it.\n"
                + (last.isEmpty() ? "  Nothing was written before it stopped.\n"
                : "  The last lines forced to storage (a `try:` with no `ok:` after it is where it stopped):\n" + last)
                + (kernel.isEmpty() ? "  No kernel messages were kept from it.\n" : "  The kernel's last messages before it stopped:\n" + kernel);
        p.edit().putInt("began", -1).putInt("reset", k).putString("report", p.getString("report", "") + said + "\n").commit();
        save(c);
        return true;
    }

    /** Past a step that was refused cleanly: the steps after it may not depend on it. */
    static void skip(Context c) {
        SharedPreferences p = prefs(c);
        int k = next(c);
        if (k >= STEPS.length) return;
        p.edit().putInt("next", k + 1).putInt("reset", -1)
                .putString("report", p.getString("report", "") + "Step " + (k + 1) + " (" + STEPS[k][0] + ") skipped.\n\n").commit();
        save(c);
    }

    /** The step the tablet last reset in, -1 if none: running it again is the user's choice, said plainly. */
    static int resetAt(Context c) { return prefs(c).getInt("reset", -1); }

    /** Runs the next step (blocking; seconds). Answers what it said. */
    static String runNext(Context c) {
        SharedPreferences p = prefs(c);
        int k = next(c);
        if (k >= STEPS.length) return "Every step passed.";
        File probe = new File(c.getApplicationInfo().nativeLibraryDir, "libghprobe.so");
        String said;
        String kind = STEPS[k][3], layout = layout(c);
        if (kind.equals("use") && layout == null) {
            return "None of the layouts started a VM: there is nothing to run the remaining steps with.";
        } else if (!"0".equals(DeviceCheck.sh(true, "id -u").trim())) {
            said = "No root (su was denied): the test needs it.";
        } else if (!new File("/dev/gunyah").exists() && DeviceCheck.sh(true, "ls /dev/gunyah").trim().isEmpty()) {
            said = "This tablet has no /dev/gunyah.";
        } else if (!probe.canExecute()) {
            said = "The probe is missing from this build.";
        } else {
            running = true;
            try {
                File crumb = crumbs(c);
                // Made here, so the app owns it and can read what root appends.
                try (FileOutputStream o = new FileOutputStream(crumb)) {
                    o.write(("step " + (k + 1) + ": " + STEPS[k][0] + "\n").getBytes(StandardCharsets.UTF_8));
                    o.getFD().sync();
                } catch (Exception ignored) { }
                try (FileOutputStream o = new FileOutputStream(kmsg(c))) { o.getFD().sync(); } catch (Exception ignored) { }
                String args = STEPS[k][1].replace("%L", layout == null ? "" : layout);
                p.edit().putInt("began", k).commit(); // commit: on storage before the step
                // A mark in the kernel's log: what the kernel says after it is this step's.
                String mark = "henge-gunyah-step-" + (k + 1) + "-" + System.currentTimeMillis();
                DeviceCheck.sh(true, "echo " + mark + " > /dev/kmsg; sync");
                int seconds = Integer.parseInt(STEPS[k][2]);
                String out = DeviceCheck.sh(true, probe.getPath() + " " + args + (kind.equals("start") ? " --until start" : "")
                        + " --seconds " + seconds + " --crumb " + crumb.getPath() + " --kmsg " + kmsg(c).getPath() + " 2>&1; true", seconds + 30);
                boolean pass = out.contains("RESULT: PASS");
                said = "Step " + (k + 1) + " of " + STEPS.length + " (" + STEPS[k][0] + "): "
                        + (pass ? "passed" : out.isEmpty() ? "no answer in " + (seconds + 30) + " s" : "FAILED") + "\n" + indent(out);
                if (!pass) {
                    // Why the kernel or Gunyah's resource manager refused: the kernel's lines since the mark.
                    String kernel = DeviceCheck.sh(true, "dmesg | sed -n '/" + mark + "/,$p' | grep -v " + mark + " | tail -n 60; true");
                    said += kernel.trim().isEmpty() ? "  The kernel said nothing during it (or dmesg is not readable).\n"
                            : "  The kernel during it:\n" + indent(kernel);
                }
                SharedPreferences.Editor e = p.edit().putInt("began", -1);
                if (pass && kind.equals("start") && layout == null) {
                    e.putString("layout", STEPS[k][1]);
                    said += "  This layout starts: the steps after the layouts use it.\n";
                }
                // A layout that is refused is an answer: on to the next one.
                if (pass || kind.equals("start")) e.putInt("next", k + 1).putInt("reset", -1);
                e.putString("report", p.getString("report", "") + said + "\n").commit();
                save(c);
                return said;
            } finally {
                running = false;
            }
        }
        p.edit().putString("report", p.getString("report", "") + said + "\n\n").commit();
        return said;
    }

    private static String indent(String s) {
        StringBuilder b = new StringBuilder();
        for (String l : s.split("\n")) if (!l.isEmpty()) b.append("    ").append(l).append('\n');
        return b.toString();
    }

    /** The report where a file manager reaches it, beside device-check.txt. */
    private static void save(Context c) {
        File ext = c.getExternalFilesDir(null);
        if (ext != null) try (FileOutputStream o = new FileOutputStream(new File(ext, "gunyah-test.txt"))) {
            o.write(report(c).getBytes(StandardCharsets.UTF_8));
            o.getFD().sync();
        } catch (Exception ignored) { }
    }
}

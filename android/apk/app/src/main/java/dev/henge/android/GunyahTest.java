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

    /** {what the step does, the probe's arguments, seconds the guest may run}. Each adds one thing to the last. */
    static final String[][] STEPS = {
            {"Open /dev/gunyah", "--until open", "6"},
            {"Create an empty VM", "--until create", "6"},
            {"Give it 2 MB of memory", "--until memory", "6"},
            {"Give it its device tree", "--until tree", "6"},
            {"Add a CPU", "--until vcpu", "6"},
            {"Start it, without running it", "--until start", "6"},
            {"Run a tiny program in it", "", "6"},
            {"256 MB of memory", "--mem 256", "6"},
            {"256 MB, every page written", "--mem 256 --touch", "20"},
            {"1 GB, every page written", "--mem 1024 --touch", "40"},
            {"3 GB, every page written", "--mem 3072 --touch", "90"},
            {"2 CPUs", "--mem 256 --cpus 2", "10"},
            {"4 CPUs", "--mem 256 --cpus 4", "10"},
            {"The Mac's interrupt doorbells", "--mem 256 --cpus 4 --bells", "10"},
            {"The Mac's memory layout, 256 MB", "--layout engine --mem 256 --cpus 4 --bells --touch", "20"},
            {"The Mac's memory layout, 3 GB", "--layout engine --mem 3072 --cpus 4 --bells --touch", "90"},
    };

    static volatile boolean running;

    private static SharedPreferences prefs(Context c) { return c.getSharedPreferences("gunyah-test", Context.MODE_PRIVATE); }
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
        String said = "Step " + (k + 1) + " (" + title + ") DID NOT END: the tablet reset, or the app was killed, during it.\n"
                + (last.isEmpty() ? "  Nothing was written before it stopped.\n"
                : "  The last lines forced to storage (a `try:` with no `ok:` after it is where it stopped):\n" + last);
        p.edit().putInt("began", -1).putInt("reset", k).putString("report", p.getString("report", "") + said + "\n").commit();
        save(c);
        return true;
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
        if (!"0".equals(DeviceCheck.sh(true, "id -u").trim())) {
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
                p.edit().putInt("began", k).commit(); // commit: on storage before the step
                DeviceCheck.sh(true, "sync");
                int seconds = Integer.parseInt(STEPS[k][2]);
                String out = DeviceCheck.sh(true, probe.getPath() + " " + STEPS[k][1] + " --seconds " + seconds + " --crumb "
                        + crumb.getPath() + " 2>&1; true", seconds + 30);
                boolean pass = out.contains("RESULT: PASS");
                said = "Step " + (k + 1) + " of " + STEPS.length + " (" + STEPS[k][0] + "): "
                        + (pass ? "passed" : out.isEmpty() ? "no answer in " + (seconds + 30) + " s" : "FAILED") + "\n" + indent(out);
                SharedPreferences.Editor e = p.edit().putInt("began", -1);
                if (pass) e.putInt("next", k + 1).putInt("reset", -1);
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

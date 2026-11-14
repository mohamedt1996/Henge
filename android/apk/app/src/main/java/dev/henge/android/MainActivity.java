package dev.henge.android;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.net.Uri;
import android.os.StatFs;
import android.system.Os;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Henge's iOS look (the Desk) on Android: Macs, Install and Settings tabs over the engine service. */
public class MainActivity extends Activity {
    // The Desk: a dark, quiet page, cards on it with a hairline edge, and one blue for what you press.
    static final int PAGE = 0xFF0B0C10, CARD = 0xFF16181F, RAISED = 0xFF23262F, TEXT = 0xFFF2F3F7,
            DIM = 0xFF9095A4, FAINT = 0xFF6A6F7E, ACCENT = 0xFF2E6BFF, ACCENT_LIGHT = 0xFF5B8DFF,
            GOOD = 0xFF39D98A, WARN = 0xFFFFBC2E, BAD = 0xFFFF7A70, HAIRLINE = 0x1AFFFFFF;

    static final String[] RELEASES = {"15.6.1", "26.6.2", "27.2"};
    static final String[] NAMES = {"macOS Sequoia 15.6.1", "macOS Tahoe 26.6.2", "macOS 27.2"};
    static final long[] MB = {16_035, 18_856, 24_409};
    static final String[] NOTES = {"Recommended: the version Henge is tested with.", "Not tested with Henge yet.",
            "The newest. Not tested with Henge yet."};
    static final String[] STAGES = {"Download", "Start installer", "Write macOS", "Check macOS", "Finish"};
    static final String[] STEP_NAMES = {"detecting", "preparing", "sending the system", "verifying",
            "flashing firmware", "flashing", "updating", "finishing"};

    enum Tab { MACS, INSTALL, VNC, SETTINGS }

    private final Handler ui = new Handler(Looper.getMainLooper());
    private SharedPreferences prefs;
    private Tab tab = Tab.MACS;
    private FrameLayout content;
    private TextView title;
    private LinearLayout tabBar;
    private boolean details;
    private String lastSignature = "";
    private long stageStarted;
    private int stageSeen = -1;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().setStatusBarColor(PAGE);
        getWindow().setNavigationBarColor(CARD);
        if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, 1);
        prefs = getSharedPreferences("henge", MODE_PRIVATE);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(PAGE);
        root.setFitsSystemWindows(true);

        title = text("Henge", 34, TEXT, true);
        title.setPadding(dp(20), dp(18), dp(20), dp(6));
        root.addView(title);

        content = new FrameLayout(this);
        root.addView(content, new LinearLayout.LayoutParams(-1, 0, 1));

        View line = new View(this);
        line.setBackgroundColor(HAIRLINE);
        root.addView(line, new LinearLayout.LayoutParams(-1, 1));
        tabBar = new LinearLayout(this);
        tabBar.setBackgroundColor(CARD);
        tabBar.setPadding(0, dp(6), 0, dp(6));
        tabBar.addView(tabButton("▭", "Macs", Tab.MACS), new LinearLayout.LayoutParams(0, -2, 1));
        tabBar.addView(tabButton("↓", "Install", Tab.INSTALL), new LinearLayout.LayoutParams(0, -2, 1));
        tabBar.addView(tabButton("▣", "VNC", Tab.VNC), new LinearLayout.LayoutParams(0, -2, 1));
        tabBar.addView(tabButton("⚙", "Settings", Tab.SETTINGS), new LinearLayout.LayoutParams(0, -2, 1));
        root.addView(tabBar);
        setContentView(root);
        if (VmService.state.equals("installing")) tab = Tab.INSTALL;
        if (!VmService.busy()) new Thread(() -> VmService.gatherImages(this)).start();
    }

    @Override protected void onResume() { super.onResume(); lastSignature = ""; ui.post(refresh); }
    @Override protected void onPause() { super.onPause(); ui.removeCallbacks(refresh); vncOff(); }

    // MARK: VNC

    /** The engine's screen (127.0.0.1:5900) inside the app, also while macOS installs: one view, kept across
     *  page rebuilds, connected only while this tab shows. */
    private ScreenActivity.RfbView vnc;
    private boolean vncConnected;

    private void vncOff() {
        if (vnc != null && vncConnected) { vnc.disconnect(); vncConnected = false; }
    }

    private View vncPage() {
        if (vnc == null) vnc = new ScreenActivity.RfbView(this);
        if (vnc.getParent() != null) ((android.view.ViewGroup) vnc.getParent()).removeView(vnc);
        if (!vncConnected) { vnc.connect(); vncConnected = true; }
        LinearLayout page = column();
        page.setPadding(dp(12), dp(4), dp(12), dp(10));
        FrameLayout frame = new FrameLayout(this);
        frame.setBackgroundColor(0xff000000);
        frame.addView(vnc, new FrameLayout.LayoutParams(-1, -1));
        page.addView(frame, new LinearLayout.LayoutParams(-1, 0, 1));
        page.addView(gap(8));
        TextView note = text(VmService.busy()
                ? (installing() ? "The installing Mac's screen. Tap clicks, drag moves, two fingers scroll."
                                : "The Mac's screen. Tap clicks, drag moves, two fingers scroll.")
                : "Nothing is running. Start a Mac or an install and its screen shows here.", 13, DIM, false);
        page.addView(note);
        page.addView(gap(6));
        page.addView(link("Full screen with keyboard", v -> startActivity(new Intent(this, ScreenActivity.class))));
        return page;
    }

    private final Runnable refresh = new Runnable() {
        @Override public void run() {
            render(false);
            ui.postDelayed(this, 1000);
        }
    };

    private void show(Tab t) { tab = t; details = false; render(true); }

    /** Rebuilds the page when what it shows changes; the install progress page redraws every second. */
    private void render(boolean force) {
        String sig = tab + "|" + VmService.state + "|" + VmService.busy() + "|" + VmService.installed(this) + "|" + details;
        boolean live = (tab == Tab.INSTALL && installing()) || (tab == Tab.SETTINGS) || (tab == Tab.INSTALL && failed());
        if (!force && sig.equals(lastSignature) && !live) return;
        int scrollY = 0;
        if (content.getChildCount() > 0 && content.getChildAt(0) instanceof ScrollView)
            scrollY = content.getChildAt(0).getScrollY();
        boolean keepScroll = sig.equals(lastSignature);
        lastSignature = sig;
        for (int i = 0; i < tabBar.getChildCount(); i++) {
            boolean on = i == tab.ordinal();
            LinearLayout b = (LinearLayout) tabBar.getChildAt(i);
            for (int j = 0; j < b.getChildCount(); j++) ((TextView) b.getChildAt(j)).setTextColor(on ? ACCENT_LIGHT : FAINT);
        }
        if (tab == Tab.VNC) { /* no scrolling: drags go to the Mac */
            title.setText("VNC");
            content.removeAllViews();
            content.addView(vncPage());
            return;
        }
        vncOff();
        LinearLayout page = column();
        page.setPadding(dp(16), dp(4), dp(16), dp(24));
        switch (tab) {
            case MACS: title.setText("Henge"); macs(page); break;
            case INSTALL: title.setText(installing() ? "Installing" : "Install macOS"); install(page); break;
            case SETTINGS: title.setText("Settings"); settings(page); break;
        }
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(Math.min(dp(680), getResources().getDisplayMetrics().widthPixels), -2,
                Gravity.CENTER_HORIZONTAL);
        /* A refresh of the same page swaps the page inside the existing scroller: a new ScrollView every second
           jumped to the top and back (the flicker while the log was being written). */
        if (keepScroll && content.getChildCount() == 1 && content.getChildAt(0) instanceof ScrollView
                && ((ScrollView) content.getChildAt(0)).getChildAt(0) instanceof FrameLayout) {
            FrameLayout centre = (FrameLayout) ((ScrollView) content.getChildAt(0)).getChildAt(0);
            centre.removeAllViews();
            centre.addView(page, lp);
            return;
        }
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        FrameLayout centre = new FrameLayout(this);
        centre.addView(page, lp);
        scroll.addView(centre);
        content.removeAllViews();
        content.addView(scroll);
        if (keepScroll) { int y = scrollY; scroll.post(() -> scroll.scrollTo(0, y)); }
    }

    // MARK: Macs

    private void macs(LinearLayout page) {
        boolean installed = VmService.installed(this), busy = VmService.busy(), running = busy && !installing();
        if (installing()) {
            page.addView(notice("↓", "Installing " + macName() + "…", ACCENT_LIGHT, "See", v -> show(Tab.INSTALL)));
            page.addView(gap(14));
        } else if (failed()) {
            page.addView(notice("⚠", "The last install did not work.", BAD, "See", v -> show(Tab.INSTALL)));
            page.addView(gap(14));
        }
        if (!installed) {
            LinearLayout card = card(30);
            card.setGravity(Gravity.CENTER_HORIZONTAL);
            card.addView(preview(false), new LinearLayout.LayoutParams(-1, dp(190)));
            card.addView(gap(14));
            card.addView(text("No Mac yet", 24, TEXT, true));
            card.addView(gap(6));
            int saved = savedRelease();
            TextView t = text(saved >= 0 ? NAMES[saved] + " is already downloaded here, so installing needs no download."
                                         : "Install macOS on this tablet. It downloads from Apple and installs here.", 15, DIM, false);
            t.setGravity(Gravity.CENTER);
            card.addView(t);
            card.addView(gap(14));
            if (saved >= 0) card.addView(primary("Install from the saved download", !busy, v -> {
                prefs.edit().putInt("release", saved).apply(); confirmInstall(); }));
            else card.addView(primary("Install macOS", true, v -> show(Tab.INSTALL)));
            page.addView(card);
            return;
        }
        LinearLayout hero = card(30);
        hero.addView(preview(running), new LinearLayout.LayoutParams(-1, dp(220)));
        LinearLayout row = row();
        row.setPadding(dp(5), dp(15), dp(5), 0);
        TextView name = text(macName(), 24, TEXT, true);
        name.setSingleLine(true);
        name.setEllipsize(TextUtils.TruncateAt.END);
        row.addView(name, new LinearLayout.LayoutParams(0, -2, 1));
        row.addView(pill(running ? "Running" : "Stopped", running ? GOOD : WARN));
        hero.addView(row);
        TextView summary = text(releaseName() + " · " + ramMb() / 1024 + " GB · " + cpus() + " CPUs", 15, DIM, false);
        summary.setPadding(dp(5), dp(3), dp(5), 0);
        hero.addView(summary);
        hero.addView(gap(15));
        if (running) {
            hero.addView(primary("▶  Open Screen", true, v -> startActivity(new Intent(this, ScreenActivity.class))));
            hero.addView(gap(10));
            hero.addView(secondary("Shut Down", BAD, v -> VmService.stop(this)));
        } else {
            hero.addView(primary("▶  Start", !busy, v -> launch("run")));
        }
        page.addView(hero);
        page.addView(gap(10));
        LinearLayout facts = row();
        facts.addView(fact("Disk", bytes(allocated(new File(VmService.macDir(this), "disk.img")))), new LinearLayout.LayoutParams(0, -2, 1));
        facts.addView(gapW(10));
        facts.addView(fact("Free here", bytes(free())), new LinearLayout.LayoutParams(0, -2, 1));
        page.addView(facts);
    }

    private View preview(boolean running) {
        FrameLayout f = new FrameLayout(this);
        f.setBackground(round(0x8023262F, 18, 0x24FFFFFF));
        TextView glyph = text(running ? "macOS is running" : "⌘", running ? 15 : 46, running ? GOOD : FAINT, false);
        f.addView(glyph, new FrameLayout.LayoutParams(-2, -2, Gravity.CENTER));
        return f;
    }

    // MARK: Install

    private boolean installing() { return VmService.state.equals("installing") && VmService.busy(); }
    private boolean failed() { return VmService.state.startsWith("install failed"); }

    private void install(LinearLayout page) {
        if (installing()) progress(page);
        else if (failed()) failedPage(page);
        else if (VmService.state.equals("installed")) {
            finished(page);
        } else chooser(page);
    }

    private void chooser(LinearLayout page) {
        page.addView(heading("Version"));
        LinearLayout versions = card(22);
        versions.setPadding(0, 0, 0, 0);
        int chosen = releaseIndex();
        for (int i = 0; i < RELEASES.length; i++) {
            final int idx = i;
            LinearLayout r = row();
            r.setPadding(dp(17), dp(12), dp(17), dp(12));
            LinearLayout col = column();
            col.addView(text(NAMES[i], 16, TEXT, false, Typeface.create("sans-serif-medium", Typeface.NORMAL)));
            long have = imageSize(i);
            col.addView(have > 0 ? text("Downloaded, " + bytes(have), 13, GOOD, false)
                                 : text(bytes(MB[i] << 20) + " download", 13, DIM, false));
            col.addView(text(NOTES[i], 13, DIM, false));
            r.addView(col, new LinearLayout.LayoutParams(0, -2, 1));
            if (i == chosen) r.addView(text("✓", 17, ACCENT_LIGHT, true));
            r.setOnClickListener(v -> { prefs.edit().putInt("release", idx).apply(); render(true); });
            r.setBackground(ripple());
            versions.addView(r);
            if (i < RELEASES.length - 1) versions.addView(hairline());
        }
        page.addView(versions);

        page.addView(heading("New Mac"));
        LinearLayout mac = card(22);
        mac.setPadding(0, 0, 0, 0);
        int maxGb = Math.max(2, (int) (totalRamMb() / 1024) - 2);
        mac.addView(stepper("Memory", ramMb() / 1024 + " GB", () -> setRam(ramMb() - 1024), () -> setRam(ramMb() + 1024),
                ramMb() > 2048, ramMb() / 1024 < maxGb));
        if (ramMb() / 1024 >= maxGb) {
            TextView note = text("The most this tablet can spare; Android keeps the rest.", 13, DIM, false);
            note.setPadding(dp(17), 0, dp(17), dp(10));
            mac.addView(note);
        }
        mac.addView(hairline());
        mac.addView(stepper("CPUs", Integer.toString(cpus()), () -> setCpus(cpus() - 1), () -> setCpus(cpus() + 1),
                cpus() > 1, cpus() < Math.min(8, Runtime.getRuntime().availableProcessors())));
        mac.addView(hairline());
        LinearLayout toggle = row();
        toggle.setPadding(dp(17), dp(14), dp(17), dp(14));
        toggle.addView(text("Delete the download after", 16, TEXT, false), new LinearLayout.LayoutParams(0, -2, 1));
        boolean del = prefs.getBoolean("deleteAfter", false);
        toggle.addView(pill(del ? "On" : "Off", del ? GOOD : FAINT));
        toggle.setOnClickListener(v -> { prefs.edit().putBoolean("deleteAfter", !del).apply(); render(true); });
        toggle.setBackground(ripple());
        mac.addView(toggle);
        page.addView(mac);

        page.addView(gap(20));
        long need = (imageSize(chosen) > 0 ? 0 : MB[chosen] << 20) + (30L << 30);
        boolean room = free() >= need;
        page.addView(primary(imageSize(chosen) > 0 ? "Install" : "Download and Install", room && !VmService.busy(),
                v -> confirmInstall()));
        TextView foot = text(room ? "Keep Henge open and the tablet plugged in. Needs about " + bytes(need) + " free; this takes hours."
                                  : "Needs about " + bytes(need) + " free; there is " + bytes(free()) + ".", 13, room ? DIM : BAD, false);
        foot.setGravity(Gravity.CENTER);
        foot.setPadding(dp(8), dp(10), dp(8), 0);
        page.addView(foot);
    }

    private int stage(String log) {
        if (!lastMatch(log, "henge: install: ").isEmpty()) {
            int step = installStep(log);
            if (step <= 1) return 1;
            if (step == 2) return 2;
            if (step == 3) return 3;
            return 4;
        }
        if (log.contains("henge: install finished")) return 4;
        if (log.contains("hengevm: display on") || log.contains(" CPUs, ")) return 1;
        return 0;
    }

    private int installStep(String log) {
        String line = lastMatch(log, "henge: install: ");
        for (int i = STEP_NAMES.length - 1; i >= 0; i--) if (line.startsWith(STEP_NAMES[i] + " ")) return i;
        return 0;
    }

    private double fraction(String log, int stage) {
        Matcher m;
        if (stage == 0) {
            String line = romPhase(log) ? lastMatch(log, "henge: boot ROM ") : lastMatch(log, "henge: downloading macOS ");
            m = Pattern.compile("\\(([0-9]+)%\\)").matcher(line);
            return m.find() ? Integer.parseInt(m.group(1)) / 100.0 : 0;
        }
        m = Pattern.compile(" ([0-9]+)%").matcher(lastMatch(log, "henge: install: "));
        return m.find() ? Integer.parseInt(m.group(1)) / 100.0 : 0;
    }

    /** Fetching the small boot ROM, before the macOS download starts. */
    private static boolean romPhase(String log) {
        return log.lastIndexOf("henge: downloading macOS ") < Math.max(log.lastIndexOf("henge: boot ROM "),
                log.lastIndexOf("henge: fetching the boot ROM"));
    }

    /** What the download is doing right now, in a line. */
    private static String downloadDetail(String log) {
        if (romPhase(log)) {
            Matcher m = Pattern.compile("([0-9]+) / ([0-9]+) KB").matcher(lastMatch(log, "henge: boot ROM "));
            return "Fetching the boot ROM from Apple" + (m.find() ? " · " + m.group(1) + " / " + m.group(2) + " KB" : "");
        }
        Matcher m = Pattern.compile("([0-9.]+) / ([0-9.]+) GB \\([0-9]+%\\) ([0-9.]+) MB/s").matcher(lastMatch(log, "henge: downloading macOS "));
        Matcher n = Pattern.compile("over ([0-9]+) connections").matcher(log);
        String conns = "";
        while (n.find()) conns = " · " + n.group(1) + " connections";
        if (m.find()) return "macOS · " + m.group(1) + " of " + m.group(2) + " GB · " + m.group(3) + " MB/s" + conns;
        String r = lastMatch(log, "henge: download interrupted");
        return r.isEmpty() ? "Starting the macOS download" : "Reconnecting…";
    }

    private void progress(LinearLayout page) {
        String log = installLog();
        int stage = stage(log);
        double f = Math.min(1, fraction(log, stage));
        if (stage != stageSeen) { stageSeen = stage; stageStarted = System.currentTimeMillis(); }

        TextView lead = text(releaseName() + " on this tablet. Keep Henge open.", 16, DIM, false);
        lead.setPadding(dp(4), 0, dp(4), dp(16));
        page.addView(lead);

        LinearLayout big = card(30);
        big.setPadding(dp(20), dp(24), dp(20), dp(24));
        big.addView(text((int) (f * 100) + "%", 66, TEXT, true));
        double elapsed = (System.currentTimeMillis() - stageStarted) / 1000.0;
        String left = "Step " + (stage + 1) + " of " + STAGES.length;
        if (f > 0.03 && f < 1 && elapsed > 10) {
            int min = (int) Math.ceil(elapsed / f * (1 - f) / 60);
            left = min <= 1 ? "About a minute left in this step" : "About " + min + " minutes left in this step";
        }
        TextView sub = text(left, 15, DIM, false);
        sub.setPadding(0, dp(2), 0, stage == 0 ? 0 : dp(16));
        big.addView(sub);
        if (stage == 0) {
            TextView what = text(downloadDetail(log), 15, TEXT, false);
            what.setPadding(0, dp(4), 0, dp(16));
            big.addView(what);
        }
        big.addView(bar(f, 11));
        page.addView(big);

        page.addView(heading("Steps"));
        LinearLayout steps = card(22);
        steps.setPadding(0, 0, 0, 0);
        for (int i = 0; i < STAGES.length; i++) {
            LinearLayout r = row();
            r.setPadding(dp(17), dp(13), dp(17), dp(13));
            r.addView(text(STAGES[i], 16, i > stage ? FAINT : TEXT, i == stage), new LinearLayout.LayoutParams(0, -2, 1));
            if (i < stage) r.addView(text("✓", 16, GOOD, true));
            else if (i == stage) {
                ProgressBar spin = new ProgressBar(this);
                spin.setIndeterminate(true);
                r.addView(spin, new LinearLayout.LayoutParams(dp(18), dp(18)));
            }
            steps.addView(r);
            if (i < STAGES.length - 1) steps.addView(hairline());
        }
        page.addView(steps);

        TextView toggle = link(details ? "Hide details" : "Show details", v -> { details = !details; render(true); });
        toggle.setPadding(0, dp(14), 0, 0);
        page.addView(toggle);
        if (details) {
            page.addView(gap(10));
            page.addView(logCard(14, false));
        }
    }

    private void failedPage(LinearLayout page) {
        LinearLayout c = card(30);
        c.setPadding(dp(18), dp(18), dp(18), dp(18));
        c.addView(text("It did not work", 22, TEXT, true));
        c.addView(gap(10));
        c.addView(text(failureReason(), 15, TEXT, false));
        c.addView(gap(10));
        c.addView(text("Downloads are kept and resume where they stopped, so trying again does not start over.", 13, DIM, false));
        c.addView(gap(16));
        c.addView(primary("Back", true, v -> { VmService.state = "idle"; render(true); }));
        page.addView(c);
        page.addView(heading("Log"));
        page.addView(logCard(14, false));
    }

    private void finished(LinearLayout page) {
        LinearLayout c = card(30);
        c.setGravity(Gravity.CENTER_HORIZONTAL);
        c.setPadding(dp(20), dp(20), dp(20), dp(20));
        c.addView(text("✓", 54, GOOD, true));
        c.addView(text("Installed", 24, TEXT, true));
        c.addView(gap(6));
        TextView t = text(releaseName() + " is on " + macName() + ". Start it from Macs.", 15, DIM, false);
        t.setGravity(Gravity.CENTER);
        c.addView(t);
        c.addView(gap(16));
        c.addView(primary("Go to Macs", true, v -> { VmService.state = "idle"; show(Tab.MACS); }));
        page.addView(c);
    }

    /** The engine's own words for why it stopped, else what the exit code means. */
    private String failureReason() {
        String log = installLog();
        String[] lines = log.split("\n");
        for (int i = lines.length - 1; i >= 0; i--) {
            String l = lines[i].trim();
            if (l.startsWith("henge app:") || l.contains("failed") || l.contains("FAILED") || l.contains("cannot")
                    || l.contains("unavailable") || l.contains("wasn't")) return l.replaceFirst("^henge( app)?: ", "");
        }
        Matcher m = Pattern.compile("\\(([0-9]+)\\)").matcher(VmService.state);
        if (m.find()) {
            int code = Integer.parseInt(m.group(1));
            if (code > 128) return "The engine was killed by " + VmService.signalName(code - 128) + ".";
            return "The engine stopped with code " + code + ".";
        }
        return "The engine stopped.";
    }

    private void confirmInstall() {
        if (!VmService.installed(this)) { startInstall(); return; }
        new AlertDialog.Builder(this, android.R.style.Theme_Material_Dialog_Alert)
                .setTitle("Replace this Mac?")
                .setMessage("Installing again erases " + macName() + " and everything on it.")
                .setPositiveButton("Replace", (d, w) -> startInstall())
                .setNegativeButton("Cancel", null).show();
    }

    private void startInstall() {
        new File(getFilesDir(), "installed").delete();
        File[] files = VmService.macDir(this).listFiles();
        if (files != null) for (File f : files)
            if (!f.getName().endsWith(".ipsw") && !f.getName().endsWith(".part") && f.isFile()) f.delete();
        installStart = VmService.logFile(this).length();
        prefs.edit().putLong("installStart", installStart).apply();
        launch("install");
        stageSeen = -1;
        ui.postDelayed(() -> render(true), 300);
    }

    private long installStart = -1;

    /** The log since this install began. */
    private String installLog() {
        if (installStart < 0) installStart = prefs.getLong("installStart", 0);
        File f = VmService.logFile(this);
        long from = Math.max(installStart, f.length() - 64 * 1024);
        return read(f, from);
    }

    // MARK: Settings

    private void settings(LinearLayout page) {
        page.addView(heading("This Mac"));
        LinearLayout c = card(22);
        c.setPadding(0, 0, 0, 0);
        c.addView(stepper("CPUs", Integer.toString(cpus()), () -> setCpus(cpus() - 1), () -> setCpus(cpus() + 1),
                cpus() > 1, cpus() < Math.min(8, Runtime.getRuntime().availableProcessors())));
        c.addView(hairline());
        c.addView(stepper("Memory", ramMb() / 1024 + " GB", () -> setRam(ramMb() - 1024), () -> setRam(ramMb() + 1024),
                ramMb() > 2048, ramMb() / 1024 < Math.max(2, (int) (totalRamMb() / 1024) - 2)));
        c.addView(hairline());
        c.addView(choiceRow("Graphics", GPU_LABELS, java.util.Arrays.asList(GPU_MODES).indexOf(gpuMode()),
                i -> { prefs.edit().putString("gpu", GPU_MODES[i]).apply(); render(true); }));
        TextView gnote = text(GPU_NOTES[Math.max(0, java.util.Arrays.asList(GPU_MODES).indexOf(gpuMode()))], 13, DIM, false);
        gnote.setPadding(dp(17), 0, dp(17), dp(12));
        c.addView(gnote);
        if ("reims".equals(gpuMode()) || "vulkan".equals(gpuMode())) {
            c.addView(hairline());
            final String scale = prefs.getString("screenScale", "compositor");
            final String[] scales = {"compositor", "blit", "sharp"};
            c.addView(choiceRow("Screen scaling", new String[]{"Compositor", "Blit", "Sharp"},
                    Math.max(0, java.util.Arrays.asList(scales).indexOf(scale)),
                    i -> { prefs.edit().putString("screenScale", scales[i]).apply(); render(true); }));
            TextView snote = text("sharp".equals(scale)
                    ? "Each of the Mac's pixels is drawn as exactly 2x2 of the tablet's (or 3x3...) by the GPU, with no "
                        + "blur, where the Mac's screen fits the tablet's a whole number of times: pick Mac screen: Half. "
                        + "Any other size is stretched smoothly, as Blit. Costs the GPU a full-resolution copy each frame."
                    : "blit".equals(scale)
                    ? "As before 0.3.53: every frame is stretched to the tablet's full resolution by the GPU before it is shown."
                    : "The Mac's picture is handed to Android at its own size and Android's compositor stretches it to the "
                        + "screen: much less work for the GPU at every frame, and less memory. If the picture is missing, "
                        + "stretched wrongly or touches land off target, pick Blit and tell us.", 13, DIM, false);
            snote.setPadding(dp(17), 0, dp(17), dp(12));
            c.addView(snote);
        }
        if ("reims".equals(gpuMode())) {
            c.addView(hairline());
            c.addView(choiceRow("Mac screen", REIMS_MODE_LABELS, Math.max(0, java.util.Arrays.asList(REIMS_MODES).indexOf(reimsMode())),
                    i -> { prefs.edit().putString("reimsMode", REIMS_MODES[i]).apply(); render(true); }));
            TextView rnote = text("half".equals(reimsMode())
                    ? "Half the tablet's own resolution (" + VmService.halfScreen(this) + "), so each of the Mac's pixels is "
                        + "exactly 2x2 of the tablet's: with Screen scaling: Sharp, crisp. Takes effect at the next start."
                    : "Every pixel of every surface is copied and compared on the CPU on this GPU driver, so a smaller "
                        + "screen is much faster. Takes effect at the next start.", 13, DIM, false);
            rnote.setPadding(dp(17), 0, dp(17), dp(12));
            c.addView(rnote);
            c.addView(hairline());
            boolean hasTurnip = GpuView.turnip(this) != null;
            c.addView(choiceRow("Vulkan driver", DRIVER_LABELS, Math.max(0, java.util.Arrays.asList(DRIVERS).indexOf(reimsDriver())),
                    i -> { prefs.edit().putString("reimsDriver", DRIVERS[i]).apply(); render(true); }));
            TextView dnote = text("turnip".equals(reimsDriver())
                    ? (hasTurnip ? "Turnip, Mesa's open Adreno driver, bundled in this build: it has the ordered framebuffer fetch "
                        + "macOS's compositor needs, and its bugs can be fixed. Falls back to the system's driver if it does not start."
                        : "This build has no Turnip; the system's driver is used. Pick System to hide this.")
                    : "Qualcomm's own Vulkan driver. It lacks ordered framebuffer fetch, raster order groups and host memory imports; "
                        + "the app works around each, less exactly.", 13, DIM, false);
            dnote.setPadding(dp(17), 0, dp(17), dp(12));
            c.addView(dnote);
            if ("turnip".equals(reimsDriver()) && GpuView.turnipHenge(this) != null) {
                c.addView(hairline());
                c.addView(choiceRow("Turnip build", new String[]{"Henge's", "Stock"}, "stock".equals(prefs.getString("turnipBuild", "henge")) ? 1 : 0,
                        i -> { prefs.edit().putString("turnipBuild", i == 1 ? "stock" : "henge").apply(); render(true); }));
                TextView bnote = text("stock".equals(prefs.getString("turnipBuild", "henge"))
                        ? "The Turnip release as it is published. It cannot share memory with the Mac safely, so the GPU works on copies."
                        : "The same Turnip with one change of Henge's: memory it is handed by the Mac is shared with the CPU properly "
                            + "(I/O-coherent), so with Zero-copy on the GPU draws from and into the Mac's own memory. If it does not "
                            + "start, the stock one is used.", 13, DIM, false);
                bnote.setPadding(dp(17), 0, dp(17), dp(12));
                c.addView(bnote);
            }
        }
        c.addView(hairline());
        c.addView(choiceRow("Touch", new String[]{"Direct", "Trackpad"}, "trackpad".equals(prefs.getString("touch", "direct")) ? 1 : 0,
                i -> { prefs.edit().putString("touch", i == 0 ? "direct" : "trackpad").apply(); render(true); }));
        TextView tnote = text("trackpad".equals(prefs.getString("touch", "direct"))
                ? "The screen is a trackpad: slide a finger to move the Mac's pointer, tap to click where it is, hold to drag, two fingers scroll."
                : "The Mac's pointer goes where the finger is: tap to click there, hold to drag, two fingers scroll.", 13, DIM, false);
        tnote.setPadding(dp(17), 0, dp(17), dp(12));
        c.addView(tnote);
        c.addView(hairline());
        final boolean captured = "capture".equals(prefs.getString("mouse", "android"));
        c.addView(choiceRow("Mouse", new String[]{"Android's pointer", "Captured"}, captured ? 1 : 0,
                i -> { prefs.edit().putString("mouse", i == 0 ? "android" : "capture").apply(); render(true); }));
        TextView mousenote = text(captured
                ? "A mouse's raw movement moves the Mac's pointer, with the Mac's own speed and acceleration; Android's pointer is hidden. Takes effect at the next start."
                : "A mouse moves Android's own pointer, drawn by the tablet's display as fast as it refreshes and dressed as the Mac's (arrow, text beam, hand), and the Mac's pointer is put exactly under it, so it moves like a real mouse and clicks land where you see them. Uses Android's pointer speed. Takes effect at the next start.",
                13, DIM, false);
        mousenote.setPadding(dp(17), 0, dp(17), dp(12));
        c.addView(mousenote);
        c.addView(hairline());
        if ("reims".equals(prefs.getString("gpu", "on"))) {
            final java.io.File copyAll = new java.io.File(VmService.reimsHomeDir(this), "copy-in-all");
            c.addView(choiceRow("GPU memory checks", new String[]{"Used", "Everything"}, copyAll.exists() ? 1 : 0,
                    i -> {
                        try { if (i == 1) copyAll.createNewFile(); else copyAll.delete(); } catch (Exception ignored) { }
                        render(true);
                    }));
            TextView cnote = text(copyAll.exists()
                    ? "Before each batch of drawing, every piece of the Mac's memory the GPU holds a copy of is checked for changes: slow, but nothing can be missed."
                    : "Before each batch of drawing, only the memory that batch uses is checked for changes: much faster. If a window shows old "
                        + "contents, pick Everything. Takes effect when the app is next started.", 13, DIM, false);
            cnote.setPadding(dp(17), 0, dp(17), dp(12));
            c.addView(cnote);
            c.addView(hairline());
        }
        c.addView(choiceRow("Mac's memory", new String[]{"In RAM", "On storage"}, "storage".equals(prefs.getString("ram", "memory")) ? 1 : 0,
                i -> { prefs.edit().putString("ram", i == 0 ? "memory" : "storage").apply(); render(true); }));
        TextView mnote = text("storage".equals(prefs.getString("ram", "memory"))
                ? "The Mac's RAM is a file on the tablet's storage: slower, and it writes to the flash all the time, but Android "
                    + "can make room by itself when memory is short."
                : "The Mac's RAM is held in the tablet's memory: faster, nothing written to storage. If Android closes the app "
                    + "under memory pressure, lower RAM or pick On storage.", 13, DIM, false);
        mnote.setPadding(dp(17), 0, dp(17), dp(12));
        c.addView(mnote);
        c.addView(hairline());
        c.addView(choiceRow("Sound", new String[]{"On", "Off"}, "off".equals(prefs.getString("sound", "on")) ? 1 : 0,
                i -> { prefs.edit().putString("sound", i == 0 ? "on" : "off").apply(); render(true); }));
        c.addView(hairline());
        c.addView(choiceRow("Full speed", new String[]{"Off", "On"}, "on".equals(prefs.getString("fullspeed", "off")) ? 1 : 0,
                i -> { prefs.edit().putString("fullspeed", i == 1 ? "on" : "off").apply(); render(true); }));
        TextView fnote = text("Needs root. Keeps every CPU core at its highest frequency while the Mac runs: fewer slowdowns, "
                + "more heat and battery use; the tablet may throttle sooner when hot. Put back when the Mac stops.", 13, DIM, false);
        fnote.setPadding(dp(17), 0, dp(17), dp(12));
        c.addView(fnote);
        c.addView(hairline());
        final boolean coresOn = !"off".equals(prefs.getString("coreTuning", "on"));
        c.addView(choiceRow("Cores for the Mac", new String[]{"On", "Off"}, coresOn ? 0 : 1,
                i -> { prefs.edit().putString("coreTuning", i == 0 ? "on" : "off").apply(); render(true); }));
        TextView coreNote = text(coresOn
                ? "Each of the Mac's CPUs gets a fast core of the tablet's to itself, the fastest for the first, and the engine's "
                    + "helpers (disk, network, sound) stay on the slow cores. With root, Android's background work is kept off the "
                    + "Mac's cores too while the Mac runs, and put back after. Other apps are slower in the background meanwhile."
                : "The Mac's CPUs share the tablet's fast cores with everything else, as before 0.3.56.", 13, DIM, false);
        coreNote.setPadding(dp(17), 0, dp(17), dp(12));
        c.addView(coreNote);
        c.addView(hairline());
        if (coresOn) {
            c.addView(choiceRow("Fast cores awake", new String[]{"On", "Off"}, "off".equals(prefs.getString("coreAwake", "on")) ? 1 : 0,
                    i -> { prefs.edit().putString("coreAwake", i == 0 ? "on" : "off").apply(); render(true); }));
            TextView awakeNote = text("Needs root. The fast cores do not go into deep sleep while the Mac runs, so work handed between "
                    + "the Mac's CPUs and the GPU is picked up sooner. Costs a little power and heat: if the tablet slows down "
                    + "sooner with this on, turn it off.", 13, DIM, false);
            awakeNote.setPadding(dp(17), 0, dp(17), dp(12));
            c.addView(awakeNote);
            c.addView(hairline());
        }
        if (new java.io.File("/dev/udmabuf").exists()) {
            c.addView(choiceRow("Zero-copy", new String[]{"Off", "On"}, "on".equals(prefs.getString("zerocopy", "off")) ? 1 : 0,
                    i -> { prefs.edit().putString("zerocopy", i == 1 ? "on" : "off").apply(); render(true); }));
            TextView znote = text("Needs root and Graphics: reims. The GPU side reaches the Mac's memory through /dev/udmabuf; with "
                    + "Henge's build of the Turnip driver the Mac's buffers are used in place, not copied. Experimental. Close Henge "
                    + "from recents and open it again after changing this.", 13, DIM, false);
            znote.setPadding(dp(17), 0, dp(17), dp(12));
            c.addView(znote);
            c.addView(hairline());
            if ("on".equals(prefs.getString("zerocopy", "off")) && "reims".equals(prefs.getString("gpu", "on"))) {
                final java.io.File inOff = new java.io.File(VmService.reimsHomeDir(this), "in-place-off");
                final java.io.File inForce = new java.io.File(VmService.reimsHomeDir(this), "in-place-force");
                c.addView(choiceRow("Mac's memory in place", new String[]{"Auto", "Off", "Force"}, inOff.exists() ? 1 : inForce.exists() ? 2 : 0,
                        i -> {
                            try {
                                inOff.getParentFile().mkdirs();
                                if (i == 1) inOff.createNewFile(); else inOff.delete();
                                if (i == 2) inForce.createNewFile(); else inForce.delete();
                            } catch (Exception ignored) { }
                            render(true);
                        }));
                TextView inote = text(inOff.exists()
                        ? "The GPU always works on copies of the Mac's memory, checked for changes at each batch of drawing."
                        : inForce.exists()
                            ? "The GPU uses the Mac's memory in place even on a driver that does not share it safely: expect stale or "
                                + "torn pixels. Only to see what a driver does."
                            : "The GPU uses the Mac's memory in place where the driver shares it safely with the CPU (Henge's Turnip, "
                                + "and the kernel keeps it so), and copies otherwise. If windows show stale or torn contents, pick Off.",
                        13, DIM, false);
                inote.setPadding(dp(17), 0, dp(17), dp(12));
                c.addView(inote);
                c.addView(hairline());
                if (!inOff.exists()) {
                    final java.io.File renderOff = new java.io.File(VmService.reimsHomeDir(this), "render-in-place-off");
                    c.addView(choiceRow("Framebuffer in place", new String[]{"On", "Off"}, renderOff.exists() ? 1 : 0,
                            i -> {
                                try {
                                    renderOff.getParentFile().mkdirs();
                                    if (i == 1) renderOff.createNewFile(); else renderOff.delete();
                                } catch (Exception ignored) { }
                                render(true);
                            }));
                    TextView rnote = text("With Henge's Turnip only. On: the GPU draws the Mac's screen straight into the Mac's "
                            + "memory. Off: it draws into a copy and copies the screen in and back at every frame. In the one "
                            + "comparison so far (a Mi Pad 7) On took about a seventh off the GPU's time per frame. If the picture is wrong, turn it off; after a "
                            + "crash Henge turns it off by itself. Close Henge from recents and open it again after changing this.",
                            13, DIM, false);
                    rnote.setPadding(dp(17), 0, dp(17), dp(12));
                    c.addView(rnote);
                    c.addView(hairline());
                }
            }
        }
        final int[] jitSizes = {1024, 0, 512, 384, 256};
        int jitNow = 1;
        for (int i = 0; i < jitSizes.length; i++) if (jitSizes[i] == prefs.getInt("jitMb", 0)) jitNow = i;
        c.addView(choiceRow("JIT cache", new String[]{"1 GB", "768 MB", "512", "384", "256"}, jitNow,
                i -> { prefs.edit().putInt("jitMb", jitSizes[i]).apply(); render(true); }));
        TextView jnote = text("Room for the Mac's code as the CPU engine translates it. When it fills, all of it is dropped and its "
                + "memory given back to Android, then code is translated again as it runs. Smaller leaves the tablet more memory "
                + "and re-translates more often: at 384 the Mi Pad 7 dropped it one to three times every 30 seconds with four "
                + "Mac apps open. If the tablet is short of memory (the log's \"memory available\"), try 512 first. "
                + "1 GB: at 768 the Mi Pad 7 still dropped it every minute and a half, and over half of what it then translated "
                + "was code it had before; 1 GB drops it less often for about 350 MB more of the tablet's memory.", 13, DIM, false);
        jnote.setPadding(dp(17), 0, dp(17), dp(12));
        c.addView(jnote);
        c.addView(hairline());
        c.addView(choiceRow("JIT cache memory", new String[]{"Give back", "Keep"}, "keep".equals(prefs.getString("jitMemory", "give")) ? 1 : 0,
                i -> { prefs.edit().putString("jitMemory", i == 1 ? "keep" : "give").apply(); render(true); }));
        TextView knote = text("Give back: when the cache is dropped its memory goes back to Android until code fills it again. "
                + "Keep: the engine holds on to it, so the tablet has that much less but does not have to find it again every "
                + "time. Which is faster on a tablet short of memory is not known yet: the log's swap figures say.", 13, DIM, false);
        knote.setPadding(dp(17), 0, dp(17), dp(12));
        c.addView(knote);
        c.addView(hairline());
        c.addView(choiceRow("Engine statistics", new String[]{"Off", "On"}, "on".equals(prefs.getString("engineStats", "off")) ? 1 : 0,
                i -> { prefs.edit().putString("engineStats", i == 1 ? "on" : "off").apply(); render(true); }));
        TextView statsNote = text("On: once a minute the CPU engine writes where its time goes into the log (some thirty lines: "
                + "memory lookups that missed, slow memory accesses, interrupts, how idle the Mac is). For finding what to "
                + "make faster; it costs a little speed while on.", 13, DIM, false);
        statsNote.setPadding(dp(17), 0, dp(17), dp(12));
        c.addView(statsNote);
        c.addView(hairline());
        final String hugeNow = prefs.getString("hugepages", "off");
        c.addView(choiceRow("Huge pages", new String[]{"Off", "On", "Eager"}, "eager".equals(hugeNow) ? 2 : "on".equals(hugeNow) ? 1 : 0,
                i -> { prefs.edit().putString("hugepages", i == 2 ? "eager" : i == 1 ? "on" : "off").apply(); render(true); }));
        TextView hnote = text("Needs root. The Mac's RAM in 2 MB pages (JIT engine, Mac's memory: In RAM): the CPU engine finds "
                + "the Mac's memory faster. Before the Mac starts, Henge has the tablet gather its free memory into whole pieces "
                + "(a second or two) and sets the kernel to hand out 2 MB pages and to gather the Mac's small pages into them as "
                + "it runs. Eager also takes the Mac's memory as it starts, for as long as the tablet has memory to spare. How "
                + "much ends up huge depends on how full the tablet is: the log's \"MB in huge pages\" says. Experimental; "
                + "every setting is put back when the Mac stops.", 13, DIM, false);
        hnote.setPadding(dp(17), 0, dp(17), dp(12));
        c.addView(hnote);
        c.addView(hairline());
        c.addView(choiceRow("CPU engine", ENGINE_LABELS, java.util.Arrays.asList(ENGINE_MODES).indexOf(engineMode()),
                i -> { prefs.edit().putString("engine", ENGINE_MODES[i]).apply(); render(true); }));
        TextView enote = text(ENGINE_NOTES[Math.max(0, java.util.Arrays.asList(ENGINE_MODES).indexOf(engineMode()))], 13, DIM, false);
        enote.setPadding(dp(17), 0, dp(17), dp(12));
        c.addView(enote);
        c.addView(hairline());
        c.addView(infoRow("Display", "VNC on 127.0.0.1:5900"));
        c.addView(hairline());
        c.addView(infoRow("Engine", VmService.busy() ? "Running" : VmService.state));
        page.addView(c);

        tablet(page);
        downloads(page);

        page.addView(heading("Log"));
        page.addView(logCard(40, true));
    }

    // MARK: Downloads

    private static final int PICK_IMAGE = 7;
    private static volatile String importing, importError;
    private static volatile long importDone, importTotal;

    /** The kept downloads: where they live, each one (tap to delete), and bringing in an .ipsw from elsewhere. */
    private void downloads(LinearLayout page) {
        page.addView(heading("Downloads"));
        LinearLayout c = card(22);
        c.setPadding(0, 0, 0, 0);
        File lib = VmService.libraryDir(this);
        boolean shared = VmService.sharedLibrary() != null;
        c.addView(infoRow("Kept in", shared ? "Henge/ on this tablet" : "the app's folder"));
        TextView where = text(shared ? "Outlives uninstalling Henge. Put an .ipsw here yourself and Henge uses it."
                : "Kept across updates, but uninstalling Henge deletes it. Allow all-files access to keep it in Henge/ on "
                + "the tablet's storage instead.", 13, DIM, false);
        where.setPadding(dp(17), 0, dp(17), dp(12));
        c.addView(where);
        if (!shared && android.os.Build.VERSION.SDK_INT >= 30) {
            c.addView(hairline());
            LinearLayout r = row();
            r.setPadding(dp(17), dp(14), dp(17), dp(14));
            r.addView(text("Keep after uninstall", 16, ACCENT_LIGHT, false), new LinearLayout.LayoutParams(0, -2, 1));
            r.setBackground(ripple());
            r.setOnClickListener(v -> {
                try {
                    startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                            Uri.parse("package:" + getPackageName())));
                } catch (Exception e) {
                    startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                }
            });
            c.addView(r);
        }
        java.util.List<File> found = new java.util.ArrayList<>();
        for (File d : VmService.imageDirs(this)) {
            File[] f = d.listFiles((x, n) -> n.endsWith(".ipsw") || n.endsWith(".ipsw.part")
                    || (!d.equals(VmService.macDir(this)) && n.equals("AVPBooter.vmapple2.bin")));
            if (f != null) found.addAll(java.util.Arrays.asList(f));
        }
        for (File f : found) {
            c.addView(hairline());
            LinearLayout r = row();
            r.setPadding(dp(17), dp(12), dp(17), dp(12));
            String n = f.getName();
            String label = n.endsWith(".part") ? n.replace(".ipsw.part", "") + " (unfinished: resumes on Install)"
                    : n.equals("AVPBooter.vmapple2.bin") ? "Boot ROM" : n;
            TextView name = text(label, 15, TEXT, false);
            name.setSingleLine(true);
            name.setEllipsize(TextUtils.TruncateAt.MIDDLE);
            r.addView(name, new LinearLayout.LayoutParams(0, -2, 1));
            r.addView(text(bytes(f.length()), 14, DIM, false));
            r.setBackground(ripple());
            r.setOnClickListener(v -> new AlertDialog.Builder(this, android.R.style.Theme_Material_Dialog_Alert)
                    .setTitle("Delete " + label + "?")
                    .setMessage("Installing again will download it again.")
                    .setPositiveButton("Delete", (d, w) -> { f.delete(); render(true); })
                    .setNegativeButton("Cancel", null).show());
            c.addView(r);
        }
        c.addView(hairline());
        if (importing != null) {
            LinearLayout r = column();
            r.setPadding(dp(17), dp(12), dp(17), dp(12));
            r.addView(text("Copying " + importing + " · " + bytes(importDone) + " of " + bytes(importTotal), 14, TEXT, false));
            r.addView(gap(8));
            r.addView(bar(importTotal > 0 ? importDone / (double) importTotal : 0, 7));
            c.addView(r);
        } else {
            LinearLayout r = row();
            r.setPadding(dp(17), dp(14), dp(17), dp(14));
            r.addView(text("Add a restore image (.ipsw)…", 16, ACCENT_LIGHT, false), new LinearLayout.LayoutParams(0, -2, 1));
            r.setBackground(ripple());
            r.setOnClickListener(v -> {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType("*/*");
                startActivityForResult(i, PICK_IMAGE);
            });
            c.addView(r);
            if (importError != null) {
                TextView e = text(importError, 13, BAD, false);
                e.setPadding(dp(17), 0, dp(17), dp(12));
                c.addView(e);
            }
        }
        page.addView(c);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_IMAGE || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        String name = displayName(uri);
        int rel = -1;
        for (int i = 0; i < RELEASES.length; i++) if (name.contains(RELEASES[i])) rel = i;
        if (!name.toLowerCase(java.util.Locale.US).endsWith(".ipsw")) { importError = name + " is not an .ipsw."; render(true); return; }
        if (rel < 0) rel = releaseIndex(); // a name without the version: taken as the chosen release
        File to = new File(VmService.libraryDir(this), RELEASES[rel] + ".ipsw"), part = new File(to.getPath() + ".import");
        importing = name; importError = null; importDone = 0; importTotal = 0;
        render(true);
        new Thread(() -> {
            try (android.os.ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r");
                 java.io.FileInputStream in = new java.io.FileInputStream(pfd.getFileDescriptor());
                 java.io.FileOutputStream out = new java.io.FileOutputStream(part)) {
                importTotal = pfd.getStatSize();
                in.getChannel().transferTo(0, Long.MAX_VALUE, out.getChannel()); // progress: the copy's size, polled
            } catch (Exception e) {
                importError = "Could not copy it: " + e.getMessage();
            }
            if (importError == null && !part.renameTo(to)) importError = "Could not save it in " + to.getParent();
            part.delete();
            importing = null;
        }, "import").start();
        ui.post(importProgress = () -> {
            importDone = part.length();
            if (importing != null) ui.postDelayed(importProgress, 500);
        });
    }
    private Runnable importProgress;

    private String displayName(Uri uri) {
        try (android.database.Cursor q = getContentResolver().query(uri, new String[]{android.provider.OpenableColumns.DISPLAY_NAME},
                null, null, null)) {
            if (q != null && q.moveToFirst()) return q.getString(0);
        } catch (Exception ignored) { }
        String p = uri.getLastPathSegment();
        return p == null ? "image.ipsw" : p;
    }

    // MARK: This tablet

    private static volatile String checkResult;
    private static volatile boolean checking;

    /** The device check, run on demand: root (if granted) reads the hypervisor devices and kernel settings. */
    private void tablet(LinearLayout page) {
        page.addView(heading("This tablet"));
        LinearLayout c = card(22);
        if (checkResult != null) {
            TextView t = text(checkResult.trim(), 11, DIM, false, Typeface.MONOSPACE);
            t.setTextIsSelectable(true);
            c.addView(t);
            c.addView(gap(10));
            c.addView(link("Open full screen", v -> showCheck()));
            c.addView(gap(10));
            c.addView(link("Copy", v -> {
                getSystemService(android.content.ClipboardManager.class)
                        .setPrimaryClip(android.content.ClipData.newPlainText("Henge device check", checkResult));
                android.widget.Toast.makeText(this, "Copied", android.widget.Toast.LENGTH_SHORT).show();
            }));
            c.addView(gap(10));
        }
        c.addView(secondary(checking ? "Checking…" : checkResult == null ? "Check this tablet" : "Check again", ACCENT_LIGHT, v -> {
            if (checking) return;
            checking = true;
            render(true);
            new Thread(() -> {
                String r = DeviceCheck.run(getApplicationContext());
                checkResult = r;
                checking = false;
                ui.post(() -> { if (!isFinishing()) { render(true); showCheck(); } });
            }, "device-check").start();
        }));
        TextView note = text("Asks for root (su) to read the hypervisor devices; saved to Android/data/dev.henge.android/files/device-check.txt.",
                13, DIM, false);
        note.setPadding(0, dp(10), 0, 0);
        c.addView(note);
        page.addView(c);
        gunyah(page);
    }

    /** Gunyah, a step a tap (GunyahTest): each step adds one thing to the last, and a reset names its step. */
    private void gunyah(LinearLayout page) {
        if (!new java.io.File("/dev/gunyah").exists() && GunyahTest.report(this).isEmpty()) return;
        GunyahTest.noteReset(this);
        LinearLayout c = card(22);
        c.addView(text("Test Gunyah", 16, TEXT, true));
        c.addView(gap(6));
        int next = GunyahTest.next(this), reset = GunyahTest.resetAt(this), n = GunyahTest.STEPS.length;
        TextView about = text("Starting the Mac on Gunyah resets this tablet. This tries Gunyah from the smallest request up, one "
                + "step a tap, and records each before it is made: after a reset, open Henge again and it says which step did it. "
                + "A step can reset the tablet: close what you have not saved.", 13, DIM, false);
        c.addView(about);
        c.addView(gap(10));
        String report = GunyahTest.report(this);
        if (!report.isEmpty()) {
            TextView t = text(report.trim(), 11, DIM, false, Typeface.MONOSPACE);
            t.setTextIsSelectable(true);
            c.addView(t);
            c.addView(gap(10));
            c.addView(link("Copy", v -> {
                getSystemService(android.content.ClipboardManager.class)
                        .setPrimaryClip(android.content.ClipData.newPlainText("Henge Gunyah test", GunyahTest.report(this)));
                android.widget.Toast.makeText(this, "Copied", android.widget.Toast.LENGTH_SHORT).show();
            }));
            c.addView(gap(10));
        }
        boolean stuck = next < n && !"pre".equals(GunyahTest.STEPS[next][3]) && !"start".equals(GunyahTest.STEPS[next][3])
                && GunyahTest.layout(this) == null;
        if (next >= n) {
            c.addView(text("Every step was run. Layout that works: " + GunyahTest.layout(this), 13, TEXT, false));
        } else if (stuck) {
            c.addView(text("None of the layouts started a VM, so the remaining steps cannot run. Copy the report.", 13, TEXT, false));
        } else {
            String label = GunyahTest.running ? "Running step " + (next + 1) + "…"
                    : (reset == next ? "Run step " + (next + 1) + " again (it reset the tablet): " : "Run step " + (next + 1) + " of " + n + ": ")
                    + GunyahTest.STEPS[next][0];
            c.addView(secondary(label, ACCENT_LIGHT, v -> {
                if (GunyahTest.running) return;
                GunyahTest.running = true;
                render(true);
                new Thread(() -> {
                    try { GunyahTest.runNext(getApplicationContext()); } finally { GunyahTest.running = false; }
                    ui.post(() -> { if (!isFinishing()) render(true); });
                }, "gunyah-test").start();
            }));
        }
        if (!report.isEmpty() && !GunyahTest.running) {
            c.addView(gap(10));
            if (next < n - 1) {
                c.addView(link("Skip step " + (next + 1), v -> { GunyahTest.skip(this); render(true); }));
                c.addView(gap(10));
            }
            c.addView(link("Start over", v -> { GunyahTest.startOver(this); render(true); }));
        }
        page.addView(gap(14));
        page.addView(c);
    }

    /** The check's result over the app, scrollable and selectable, with Copy. */
    private void showCheck() {
        TextView t = text(checkResult.trim(), 12, TEXT, false, Typeface.MONOSPACE);
        t.setTextIsSelectable(true);
        t.setPadding(dp(20), dp(12), dp(20), dp(12));
        android.widget.HorizontalScrollView h = new android.widget.HorizontalScrollView(this);
        h.addView(t);
        ScrollView v = new ScrollView(this);
        v.addView(h);
        new AlertDialog.Builder(this, android.R.style.Theme_Material_Dialog_Alert)
                .setTitle("This tablet")
                .setView(v)
                .setPositiveButton("Copy", (d, w) -> {
                    getSystemService(android.content.ClipboardManager.class)
                            .setPrimaryClip(android.content.ClipData.newPlainText("Henge device check", checkResult));
                    android.widget.Toast.makeText(this, "Copied", android.widget.Toast.LENGTH_SHORT).show();
                })
                .setNegativeButton("Close", null).show();
    }

    // MARK: Engine

    private void launch(String mode) {
        Intent i = new Intent(this, VmService.class);
        i.putExtra(VmService.EXTRA_MODE, mode);
        i.putExtra(VmService.EXTRA_RAM, ramMb());
        i.putExtra(VmService.EXTRA_CPUS, cpus());
        i.putExtra(VmService.EXTRA_RELEASE, RELEASES[releaseIndex()]);
        i.putExtra("deleteImage", prefs.getBoolean("deleteAfter", false));
        i.putExtra("gpu", gpuMode());
        i.putExtra("engine", engineMode());
        if ("install".equals(mode)) prefs.edit().putInt("installedRelease", releaseIndex()).apply();
        startForegroundService(i);
        if ("run".equals(mode)) ui.postDelayed(() -> startActivity(new Intent(this, ScreenActivity.class)), 1500);
    }

    private int releaseIndex() { return Math.max(0, Math.min(RELEASES.length - 1, prefs.getInt("release", 0))); }
    private String releaseName() {
        int i = VmService.installed(this) && !installing() ? prefs.getInt("installedRelease", releaseIndex()) : releaseIndex();
        return NAMES[Math.max(0, Math.min(NAMES.length - 1, i))];
    }
    private String macName() { return "Mac"; }
    private int ramMb() { return prefs.getInt("ramMB", 4096); }
    private int cpus() { return prefs.getInt("cpus", 2); }
    private void setRam(int mb) { prefs.edit().putInt("ramMB", Math.max(2048, mb)).apply(); render(true); }
    private void setCpus(int n) { prefs.edit().putInt("cpus", Math.max(1, n)).apply(); render(true); }
    private long free() { return new StatFs(getFilesDir().getPath()).getAvailableBytes(); }
    private long totalRamMb() {
        android.app.ActivityManager.MemoryInfo mi = new android.app.ActivityManager.MemoryInfo();
        getSystemService(android.app.ActivityManager.class).getMemoryInfo(mi);
        return mi.totalMem >> 20;
    }
    private long imageSize(int i) {
        File f = VmService.findImage(this, RELEASES[i]);
        return f == null ? 0 : f.length();
    }
    /** The release whose download is here (the chosen one first), or -1. */
    private int savedRelease() {
        if (imageSize(releaseIndex()) > 0) return releaseIndex();
        for (int i = 0; i < RELEASES.length; i++) if (imageSize(i) > 0) return i;
        return -1;
    }
    private static long allocated(File f) {
        try { return Os.stat(f.getPath()).st_blocks * 512; } catch (Exception e) { return 0; }
    }

    // MARK: Pieces

    private LinearLayout card(int radius) {
        LinearLayout c = column();
        c.setPadding(dp(14), dp(14), dp(14), dp(14));
        c.setBackground(round(CARD, radius, HAIRLINE));
        c.setClipToOutline(true);
        return c;
    }

    private GradientDrawable round(int fill, int radius, int stroke) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill);
        g.setCornerRadius(dp(radius));
        if (stroke != 0) g.setStroke(Math.max(1, dp(1) / 2), stroke);
        return g;
    }

    private android.graphics.drawable.Drawable ripple() {
        TypedValue tv = new TypedValue();
        getTheme().resolveAttribute(android.R.attr.selectableItemBackground, tv, true);
        return getDrawable(tv.resourceId);
    }

    private TextView primary(String label, boolean enabled, View.OnClickListener l) {
        TextView b = text(label, 19, enabled ? Color.WHITE : DIM, true);
        b.setGravity(Gravity.CENTER);
        b.setBackground(round(enabled ? ACCENT : RAISED, 29, 0));
        b.setLayoutParams(new LinearLayout.LayoutParams(-1, dp(58)));
        b.setEnabled(enabled);
        if (enabled) { b.setOnClickListener(l); pressable(b); }
        return b;
    }

    private TextView secondary(String label, int color, View.OnClickListener l) {
        TextView b = text(label, 17, color, true);
        b.setGravity(Gravity.CENTER);
        b.setBackground(round(RAISED, 26, 0));
        b.setLayoutParams(new LinearLayout.LayoutParams(-1, dp(52)));
        b.setOnClickListener(l);
        pressable(b);
        return b;
    }

    private void pressable(View v) {
        v.setOnTouchListener((view, e) -> {
            switch (e.getActionMasked()) {
                case android.view.MotionEvent.ACTION_DOWN:
                    view.animate().scaleX(0.985f).scaleY(0.985f).alpha(0.85f).setDuration(120).start(); break;
                case android.view.MotionEvent.ACTION_UP:
                case android.view.MotionEvent.ACTION_CANCEL:
                    view.animate().scaleX(1).scaleY(1).alpha(1).setDuration(120).start(); break;
            }
            return false;
        });
    }

    private TextView pill(String label, int color) {
        TextView p = text(label, 13, color, true);
        p.setPadding(dp(11), dp(5), dp(11), dp(5));
        p.setBackground(round((color & 0x00FFFFFF) | 0x21000000, 20, 0));
        return p;
    }

    private TextView heading(String t) {
        TextView h = text(t, 14, DIM, true);
        h.setPadding(dp(18), dp(18), dp(18), dp(8));
        return h;
    }

    private LinearLayout notice(String symbol, String msg, int tint, String trailing, View.OnClickListener l) {
        LinearLayout c = card(22);
        c.setOrientation(LinearLayout.HORIZONTAL);
        c.setGravity(Gravity.CENTER_VERTICAL);
        c.addView(text(symbol, 17, tint, true));
        c.addView(gapW(11));
        c.addView(text(msg, 15, TEXT, false), new LinearLayout.LayoutParams(0, -2, 1));
        if (trailing != null) c.addView(text(trailing, 15, ACCENT_LIGHT, true));
        if (l != null) c.setOnClickListener(l);
        return c;
    }

    private LinearLayout fact(String title, String value) {
        LinearLayout c = card(22);
        c.setPadding(dp(15), dp(15), dp(15), dp(15));
        c.addView(text(title, 13, DIM, false));
        TextView v = text(value, 21, TEXT, true);
        v.setSingleLine(true);
        c.addView(v);
        return c;
    }

    private LinearLayout stepper(String label, String value, Runnable minus, Runnable plus, boolean canMinus, boolean canPlus) {
        LinearLayout r = row();
        r.setPadding(dp(17), dp(8), dp(12), dp(8));
        r.addView(text(label, 16, TEXT, false), new LinearLayout.LayoutParams(0, -2, 1));
        TextView v = text(value, 16, TEXT, true);
        v.setPadding(0, 0, dp(12), 0);
        r.addView(v);
        LinearLayout seg = row();
        seg.setBackground(round(RAISED, 9, 0));
        seg.addView(stepButton("−", canMinus, minus));
        View div = new View(this);
        div.setBackgroundColor(HAIRLINE);
        seg.addView(div, new LinearLayout.LayoutParams(1, dp(18)));
        seg.addView(stepButton("+", canPlus, plus));
        r.addView(seg);
        return r;
    }

    private TextView stepButton(String t, boolean enabled, Runnable r) {
        TextView b = text(t, 20, enabled ? TEXT : FAINT, false);
        b.setGravity(Gravity.CENTER);
        b.setLayoutParams(new LinearLayout.LayoutParams(dp(46), dp(34)));
        if (enabled) { b.setOnClickListener(v -> r.run()); b.setBackground(ripple()); }
        return b;
    }

    static final String[] ENGINE_MODES = {"jit", "gunyah"};
    static final String[] ENGINE_LABELS = {"JIT", "Gunyah"};
    static final String[] ENGINE_NOTES = {
            "Translates the Mac's instructions: works on any tablet, but many times slower than a real Mac.",
            "Experimental: runs the Mac's instructions on this tablet's CPU through Qualcomm's Gunyah hypervisor. "
                    + "Needs root (su). Uses the CPUs setting. The log says how far it gets."};
    private String engineMode() { return prefs.getString("engine", "jit"); }
    static final String[] GPU_MODES = {"on", "off", "capture", "vulkan", "reims", "applegfx"};
    static final String[] GPU_LABELS = {"GPU", "Off", "Capture", "Vulkan", "reims", "applegfx"};
    static final String[] GPU_NOTES = {
            "macOS's GPU driver runs, but Android has no renderer for it yet: expect the boot screen, then black.",
            "Experiment: no GPU device, so macOS may fall back to a plain framebuffer and show a slow desktop.",
            "Saves every shader and pipeline macOS uses to Android/data/dev.henge.android/files/shaders, "
                    + "the inventory for the Vulkan renderer. Copy it to a computer over USB.",
            "Experimental: the app's Vulkan renderer shows the screen and draws on the GPU, translating macOS's shaders "
                    + "on the tablet into Android/data/dev.henge.android/files/spirv. Blending and some draws are still guesses.",
            "Experimental: the whole GPU device is reims-vgpu running in this app, drawing macOS's Metal work on the "
                    + "tablet's GPU through metal2vulkan, as a Mac's own paravirtual GPU would. Keep the app open while the Mac runs.",
            "Experimental: a second, independent GPU host (libapplegfx-vulkan), chosen instead of reims so the GPU is "
                    + "not bet on one project. It loads and initialises on the tablet (its version is in the log), but does "
                    + "not drive the screen yet, so the picture stays on Graphics: GPU. Pick reims for the working GPU."};

    private String gpuMode() { return prefs.getString("gpu", "on"); }
    static final String[] REIMS_MODES = {"1024x768", "1280x800", "1600x1000", "half", "1920x1080"};
    static final String[] REIMS_MODE_LABELS = {"1024", "1280", "1600", "Half", "1920"};
    private String reimsMode() { return prefs.getString("reimsMode", "1280x800"); }
    static final String[] DRIVERS = {"turnip", "system"};
    static final String[] DRIVER_LABELS = {"Turnip", "System"};
    private String reimsDriver() { return prefs.getString("reimsDriver", "turnip"); }

    /** A row with a segmented choice, the way iOS settings pick one of a few. */
    private LinearLayout choiceRow(String label, String[] options, int selected, java.util.function.IntConsumer pick) {
        LinearLayout r = row();
        r.setPadding(dp(17), dp(10), dp(12), dp(10));
        r.addView(text(label, 16, TEXT, false), new LinearLayout.LayoutParams(0, -2, 1));
        LinearLayout seg = row();
        seg.setBackground(round(RAISED, 9, 0));
        seg.setPadding(dp(2), dp(2), dp(2), dp(2));
        for (int i = 0; i < options.length; i++) {
            final int idx = i;
            TextView o = text(options[i], 14, i == selected ? Color.WHITE : DIM, i == selected);
            o.setGravity(Gravity.CENTER);
            o.setPadding(dp(12), dp(6), dp(12), dp(6));
            if (i == selected) o.setBackground(round(ACCENT, 7, 0));
            o.setOnClickListener(v -> pick.accept(idx));
            seg.addView(o);
        }
        r.addView(seg);
        return r;
    }

    private LinearLayout infoRow(String label, String value) {
        LinearLayout r = row();
        r.setPadding(dp(17), dp(14), dp(17), dp(14));
        r.addView(text(label, 16, TEXT, false), new LinearLayout.LayoutParams(0, -2, 1));
        r.addView(text(value, 15, DIM, false));
        return r;
    }

    private View bar(double fraction, int height) {
        FrameLayout f = new FrameLayout(this);
        f.setBackground(round(RAISED, height, 0));
        View fill = new View(this);
        fill.setBackground(round(ACCENT, height, 0));
        f.addView(fill, new FrameLayout.LayoutParams(dp(height), dp(height)));
        f.setLayoutParams(new LinearLayout.LayoutParams(-1, dp(height)));
        f.post(() -> {
            ViewGroup.LayoutParams lp = fill.getLayoutParams();
            lp.width = Math.max(dp(height), (int) (f.getWidth() * Math.min(Math.max(fraction, 0), 1)));
            fill.setLayoutParams(lp);
        });
        return f;
    }

    private static final Pattern PROGRESS = Pattern.compile("\\s*(.*?):\\s*\\d{1,3}(?:\\.\\d)?%\\s*");

    /** Two views of one file: henge.log keeps everything the engine prints (the verbose log); the normal view leaves
     *  out the machinery (iBoot's trace, debug lines, plist dumps, PSCI calls, heartbeats), which verbose shows. */
    private boolean verboseLog() { return prefs.getBoolean("logVerbose", false); }

    private static final Pattern NOISE = Pattern.compile(
            "^(?:[0-9a-f]{10,16}:\\d+"                           // iBoot's trace: hash:line
            + "|psci 0x.*"                                         // PSCI calls (HV_PSCI_LOG)
            + "|.*<Debug>.*"                                       // the restore tools' debug lines
            + "|\\s*<(?:\\?xml|!DOCTYPE|/?(?:plist|dict|key|string|integer|data|array|true|false|real|date)\\b).*" // plist dumps
            + "|henge: guest \\d+ s:.*"                            // the 30 s heartbeat (installs and boots)
            + "|gunyah: (?:\\d+ )?device accesses.*"
            + "|hengevm: (?:read of|write to) implementation-defined.*"
            + "|hengevm: unknown hypervisor call.*"
            + "|mmio (?:read|write) .*"
            + ")$");

    private LinearLayout logCard(int lines, boolean clear) {
        LinearLayout l = card(22);
        l.setPadding(0, dp(4), 0, dp(14));
        l.addView(choiceRow("Log", new String[]{"Normal", "Verbose"}, verboseLog() ? 1 : 0,
                i -> { prefs.edit().putBoolean("logVerbose", i == 1).apply(); render(true); }));
        LinearLayout body = column();
        body.setPadding(dp(17), dp(4), dp(17), 0);
        body.addView(logView(verboseLog() ? lines * 3 : lines, verboseLog()));
        body.addView(gap(10));
        body.addView(link("Copy the verbose log", v -> {
            File f = VmService.logFile(this);
            String all = read(f, Math.max(0, f.length() - 256 * 1024));
            getSystemService(android.content.ClipboardManager.class)
                    .setPrimaryClip(android.content.ClipData.newPlainText("Henge verbose log", all));
            VmService.exportLog(this);
            android.widget.Toast.makeText(this, "Copied the last 256 KB. The whole log is in "
                    + "Android/data/dev.henge.android/files/verbose.log", android.widget.Toast.LENGTH_LONG).show();
        }));
        body.addView(gap(10));
        body.addView(link("Save the full log to Downloads", v -> saveToDownloads(false)));
        body.addView(gap(10));
        body.addView(link("Export captured shaders to Downloads", v -> saveToDownloads(true)));
        if (clear) {
            body.addView(gap(10));
            body.addView(link("Clear log", v -> { VmService.logFile(this).delete(); prefs.edit().putLong("installStart", 0).apply();
                installStart = 0; render(true); }));
        }
        l.addView(body);
        return l;
    }

    /* The log is read and filtered off the UI thread, and only when the file grew: reading up to 96 KB and running
       the filters on the UI thread every second stalled the app's drawing while the engine wrote. */
    private static final java.util.concurrent.ExecutorService LOG_READER = java.util.concurrent.Executors.newSingleThreadExecutor();
    private static volatile String logText = "";
    private static volatile String logKey = "";
    private static volatile boolean logReading;

    /** Download/Henge/: the whole verbose log and the install log, or the shader capture as a zip. */
    private void saveToDownloads(boolean shaders) {
        android.widget.Toast.makeText(this, shaders ? "Zipping the shaders…" : "Saving the log…", android.widget.Toast.LENGTH_SHORT).show();
        String stamp = new java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US).format(new java.util.Date());
        android.content.Context c = getApplicationContext();
        new Thread(() -> {
            List<String> saved = new ArrayList<>();
            if (shaders) {
                String n = VmService.toDownloads(c, VmService.shaderDir(c), "henge-shaders-" + stamp + ".zip", "application/zip");
                if (n != null) saved.add(n);
            } else {
                String n = VmService.toDownloads(c, VmService.logFile(c), "henge-verbose-" + stamp + ".log", "text/plain");
                if (n != null) saved.add(n);
                n = VmService.toDownloads(c, new File(VmService.macDir(c), "restore/install.log"), "henge-install-" + stamp + ".log", "text/plain");
                if (n != null) saved.add(n);
            }
            String msg = saved.isEmpty() ? (shaders ? "No shaders captured yet." : "No log to save yet.")
                    : "Saved to Download/Henge: " + TextUtils.join(", ", saved);
            ui.post(() -> android.widget.Toast.makeText(c, msg, android.widget.Toast.LENGTH_LONG).show());
        }).start();
    }

    private TextView logView(int lines, boolean verbose) {
        File f = VmService.logFile(this);
        String key = f.length() + "|" + lines + "|" + verbose;
        String shown = key.equals(logKey) || !logText.isEmpty() ? logText : "";
        TextView t = text(shown.isEmpty() ? "Nothing yet." : shown, 10, DIM, false, Typeface.MONOSPACE);
        t.setTextIsSelectable(true);
        if (!key.equals(logKey) && !logReading) {
            logReading = true;
            LOG_READER.execute(() -> {
                String text = filterLog(read(f, Math.max(0, f.length() - (verbose ? 96 : 48) * 1024)), lines, verbose);
                logText = text;
                logKey = key;
                logReading = false;
                ui.post(() -> { if (t.isAttachedToWindow() && !text.equals(t.getText().toString()))
                    t.setText(text.isEmpty() ? "Nothing yet." : text); });
            });
        }
        return t;
    }

    private static String filterLog(String all, int lines, boolean verbose) {
        // A progress bar redrawn with \r, or printed as "Label: 93.1%" line after line, shows as its latest value.
        List<String> ls = new ArrayList<>();
        for (String raw : all.split("\n")) {
            int cr = raw.lastIndexOf('\r', raw.length() - 2);
            String line = (cr >= 0 ? raw.substring(cr + 1) : raw).replace("\r", "").replaceAll("\u001B\\[[0-9;]*[A-Za-z]", "");
            if (line.trim().isEmpty()) continue;
            if (!verbose && NOISE.matcher(line).matches()) continue;
            Matcher pm = PROGRESS.matcher(line);
            if (pm.matches() && !ls.isEmpty()) {
                Matcher prev = PROGRESS.matcher(ls.get(ls.size() - 1));
                if (prev.matches() && prev.group(1).equals(pm.group(1))) { ls.set(ls.size() - 1, line); continue; }
            }
            ls.add(line);
        }
        StringBuilder b = new StringBuilder();
        for (int i = Math.max(0, ls.size() - lines); i < ls.size(); i++) b.append(ls.get(i)).append('\n');
        return b.toString().trim();
    }

    private TextView link(String t, View.OnClickListener l) {
        TextView v = text(t, 15, ACCENT_LIGHT, true);
        v.setGravity(Gravity.CENTER);
        v.setOnClickListener(l);
        return v;
    }

    private View hairline() {
        View v = new View(this);
        v.setBackgroundColor(HAIRLINE);
        v.setLayoutParams(new LinearLayout.LayoutParams(-1, Math.max(1, dp(1) / 2)));
        return v;
    }

    private LinearLayout tabButton(String glyph, String label, Tab t) {
        LinearLayout b = column();
        b.setGravity(Gravity.CENTER);
        TextView g = text(glyph, 20, FAINT, false);
        g.setGravity(Gravity.CENTER);
        TextView l = text(label, 11, FAINT, true);
        l.setGravity(Gravity.CENTER);
        b.addView(g);
        b.addView(l);
        b.setOnClickListener(v -> show(t));
        return b;
    }

    private TextView text(String s, int sp, int color, boolean bold) {
        return text(s, sp, color, bold, Typeface.create("sans-serif", bold ? Typeface.BOLD : Typeface.NORMAL));
    }

    private TextView text(String s, int sp, int color, boolean bold, Typeface face) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        t.setTypeface(face);
        return t;
    }

    private LinearLayout row() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setGravity(Gravity.CENTER_VERTICAL);
        return l;
    }

    private LinearLayout column() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        return l;
    }

    private View gap(int h) { View v = new View(this); v.setLayoutParams(new LinearLayout.LayoutParams(1, dp(h))); return v; }
    private View gapW(int w) { View v = new View(this); v.setLayoutParams(new LinearLayout.LayoutParams(dp(w), 1)); return v; }

    private static String lastMatch(String log, String prefix) {
        int i = log.lastIndexOf(prefix);
        if (i < 0) return "";
        int end = log.indexOf('\n', i);
        return log.substring(i + prefix.length(), end < 0 ? log.length() : end).trim();
    }

    private static String read(File f, long from) {
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
            long len = r.length();
            from = Math.max(0, Math.min(from, len));
            byte[] b = new byte[(int) (len - from)];
            r.seek(from);
            r.readFully(b);
            return new String(b, "UTF-8").replace('\r', '\n');
        } catch (Exception e) {
            return "";
        }
    }

    static String bytes(long b) {
        if (b >= 1L << 30) return String.format(java.util.Locale.US, "%.1f GB", b / (double) (1L << 30));
        return Math.max(0, b >> 20) + " MB";
    }

    private int dp(int v) { return Math.round(v * getResources().getDisplayMetrics().density); }
}

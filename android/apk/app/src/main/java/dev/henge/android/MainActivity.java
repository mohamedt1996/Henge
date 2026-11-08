package dev.henge.android;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.StatFs;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;

import java.io.File;
import java.io.RandomAccessFile;

/** Install macOS, start and stop the Mac, open its screen, and watch the engine's log. */
public class MainActivity extends Activity {
    private static final String[] RELEASES = {"15.6.1", "26.6.2", "27.2"};
    private static final String[] RELEASE_NAMES = {"macOS Sequoia 15.6.1", "macOS Tahoe 26.6.2 (untested)",
            "macOS 27.2 (untested)"};
    private final Handler ui = new Handler(Looper.getMainLooper());
    private TextView status, log;
    private ScrollView logScroll;
    private EditText ram, cpus;
    private Spinner release;
    private Button install, start, stop, screen;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, 1);
        int pad = dp(16);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("Henge — macOS on Android");
        title.setTextSize(22);
        root.addView(title);

        status = new TextView(this);
        status.setPadding(0, dp(8), 0, dp(8));
        root.addView(status);

        LinearLayout settings = row();
        settings.addView(label("RAM (MB)"));
        ram = number("4096");
        settings.addView(ram);
        settings.addView(label("  CPUs"));
        cpus = number("2");
        settings.addView(cpus);
        root.addView(settings);

        release = new Spinner(this);
        release.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, RELEASE_NAMES));
        root.addView(release);

        LinearLayout buttons = row();
        install = button("Install macOS", v -> confirmInstall());
        start = button("Start", v -> launch("run"));
        stop = button("Stop", v -> VmService.stop(this));
        screen = button("Screen", v -> startActivity(new Intent(this, ScreenActivity.class)));
        buttons.addView(install);
        buttons.addView(start);
        buttons.addView(stop);
        buttons.addView(screen);
        root.addView(buttons);

        log = new TextView(this);
        log.setTypeface(Typeface.MONOSPACE);
        log.setTextSize(11);
        log.setTextIsSelectable(true);
        logScroll = new ScrollView(this);
        logScroll.addView(log);
        root.addView(logScroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        setContentView(root);
    }

    @Override protected void onResume() { super.onResume(); ui.post(refresh); }
    @Override protected void onPause() { super.onPause(); ui.removeCallbacks(refresh); }

    private final Runnable refresh = new Runnable() {
        @Override public void run() {
            boolean busy = VmService.busy(), installed = VmService.installed(MainActivity.this);
            long freeGb = new StatFs(getFilesDir().getPath()).getAvailableBytes() >> 30;
            String s = "State: " + VmService.state + (installed ? " · macOS installed" : " · macOS not installed")
                    + " · " + freeGb + " GB free";
            status.setText(s);
            install.setEnabled(!busy);
            start.setEnabled(!busy && installed);
            stop.setEnabled(busy);
            screen.setEnabled(busy);
            boolean atBottom = !logScroll.canScrollVertically(1);
            log.setText(tail(VmService.logFile(MainActivity.this), 24 * 1024));
            if (atBottom) logScroll.post(() -> logScroll.fullScroll(View.FOCUS_DOWN));
            ui.postDelayed(this, 1000);
        }
    };

    private void confirmInstall() {
        long free = new StatFs(getFilesDir().getPath()).getAvailableBytes() >> 30;
        String msg = "Henge downloads the boot ROM and macOS (17 GB) from Apple and installs it here. "
                + "It needs about 45 GB free (you have " + free + " GB) and takes hours: keep the tablet plugged in."
                + (VmService.installed(this) ? "\n\nThis replaces the Mac already installed." : "")
                + "\n\nIf a download stops, tap Install again to resume.";
        new AlertDialog.Builder(this).setTitle("Install macOS").setMessage(msg)
                .setPositiveButton("Install", (d, w) -> {
                    new File(getFilesDir(), "installed").delete();
                    if (VmService.installed(this)) deleteMac();
                    launch("install");
                })
                .setNegativeButton("Cancel", null).show();
    }

    private void deleteMac() {
        File[] files = VmService.macDir(this).listFiles();
        if (files != null) for (File f : files) if (!f.getName().endsWith(".ipsw") && !f.getName().endsWith(".part")) f.delete();
    }

    private void launch(String mode) {
        Intent i = new Intent(this, VmService.class);
        i.putExtra(VmService.EXTRA_MODE, mode);
        i.putExtra(VmService.EXTRA_RAM, parse(ram, 4096));
        i.putExtra(VmService.EXTRA_CPUS, parse(cpus, 2));
        i.putExtra(VmService.EXTRA_RELEASE, RELEASES[release.getSelectedItemPosition()]);
        startForegroundService(i);
        if ("run".equals(mode)) ui.postDelayed(() -> startActivity(new Intent(this, ScreenActivity.class)), 1500);
    }

    private static int parse(EditText e, int dflt) {
        try { return Integer.parseInt(e.getText().toString().trim()); } catch (Exception x) { return dflt; }
    }

    private static String tail(File f, int bytes) {
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
            long len = r.length(), from = Math.max(0, len - bytes);
            byte[] b = new byte[(int) (len - from)];
            r.seek(from);
            r.readFully(b);
            return new String(b, "UTF-8").replace('\r', '\n');
        } catch (Exception e) {
            return "";
        }
    }

    private LinearLayout row() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setGravity(Gravity.CENTER_VERTICAL);
        return l;
    }

    private TextView label(String t) { TextView v = new TextView(this); v.setText(t); return v; }

    private EditText number(String t) {
        EditText e = new EditText(this);
        e.setText(t);
        e.setInputType(InputType.TYPE_CLASS_NUMBER);
        e.setMinEms(4);
        return e;
    }

    private Button button(String t, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(t);
        b.setAllCaps(false);
        b.setOnClickListener(l);
        return b;
    }

    private int dp(int v) { return Math.round(v * getResources().getDisplayMetrics().density); }
}

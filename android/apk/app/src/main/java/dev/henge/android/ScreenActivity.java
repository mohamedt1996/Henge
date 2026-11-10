package dev.henge.android;

import android.app.Activity;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.os.Bundle;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;

import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;

/** The Mac's screen: an RFB client of the engine's VNC server on 127.0.0.1:5900, with touch and keyboard. */
public class ScreenActivity extends Activity {
    private RfbView view;
    private EditText keys;
    private GpuView gpu; /* Graphics: Vulkan: the app draws the screen; VNC only carries input */
    private static final String FILLER = " ";

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        String gpuMode = getSharedPreferences("henge", MODE_PRIVATE).getString("gpu", "on");
        boolean vulkan = ("vulkan".equals(gpuMode) || "reims".equals(gpuMode)) && GpuView.loaded;
        if (vulkan) {
            gpu = new GpuView(this);
            root.addView(gpu);
            gpu.start();
        }
        view = new RfbView(this);
        view.overlay = vulkan;
        if (vulkan) view.setPointerIcon(android.view.PointerIcon.getSystemIcon(this, android.view.PointerIcon.TYPE_NULL));
        root.addView(view);

        // A hidden field takes the soft keyboard's text; its characters go to the Mac as keys.
        keys = new EditText(this);
        keys.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD);
        keys.setAlpha(0f);
        // No full-screen "extract" editor in landscape: it would cover the Mac.
        keys.setImeOptions(android.view.inputmethod.EditorInfo.IME_FLAG_NO_EXTRACT_UI
                | android.view.inputmethod.EditorInfo.IME_FLAG_NO_FULLSCREEN
                | android.view.inputmethod.EditorInfo.IME_ACTION_NONE);
        // The field always holds at least one character (a space), so Backspace always has something to
        // delete and the keyboard keeps reporting it: an empty field swallows it on many keyboards.
        keys.setText(FILLER);
        keys.setSelection(FILLER.length());
        keys.addTextChangedListener(new TextWatcher() {
            private String removed = "";
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {
                removed = s.subSequence(start, start + count).toString();
            }
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {
                // A keyboard that composes replaces the whole word at each letter: only what differs is typed.
                String added = s.subSequence(start, start + count).toString();
                int same = 0;
                while (same < removed.length() && same < added.length() && removed.charAt(same) == added.charAt(same)) same++;
                for (int i = same; i < removed.length(); i++) view.tapKey(0xff08, false); // deletions: Backspace
                for (int i = same; i < added.length(); i++) view.typeChar(added.charAt(i));
            }
            @Override public void afterTextChanged(Editable e) {
                if (e.length() == 0 || e.length() > 64) {
                    keys.removeTextChangedListener(this);
                    keys.setText(FILLER);
                    keys.setSelection(FILLER.length());
                    keys.addTextChangedListener(this);
                }
            }
        });
        keys.setOnKeyListener((v, code, ev) -> {
            if (code != KeyEvent.KEYCODE_ENTER) return false; // Backspace goes through the field's text
            if (ev.getAction() == KeyEvent.ACTION_DOWN) view.tapKey(0xff0d, false);
            return true;
        });
        keys.setOnEditorActionListener((v, action, ev) -> {
            if (ev == null) view.tapKey(0xff0d, false); // the keyboard's own Return; a key event came through above
            return true;
        });
        root.addView(keys, new FrameLayout.LayoutParams(1, 1));

        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setBackgroundColor(0x80000000);
        bar.addView(barButton("⌨", v -> toggleKeyboard()));
        bar.addView(barButton("esc", v -> view.tapKey(0xff1b, false)));
        bar.addView(barButton("tab", v -> view.tapKey(0xff09, false)));
        bar.addView(barButton("⌘", v -> view.toggleModifier(0xffe7)));
        bar.addView(barButton("⌥", v -> view.toggleModifier(0xffe9)));
        bar.addView(barButton("⌃", v -> view.toggleModifier(0xffe3)));
        bar.addView(barButton("←", v -> view.tapKey(0xff51, false)));
        bar.addView(barButton("↑", v -> view.tapKey(0xff52, false)));
        bar.addView(barButton("↓", v -> view.tapKey(0xff54, false)));
        bar.addView(barButton("→", v -> view.tapKey(0xff53, false)));
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.END);
        root.addView(bar, lp);
        setContentView(root);
    }

    private Button barButton(String t, View.OnClickListener l) {
        Button b = new Button(this, null, android.R.attr.borderlessButtonStyle);
        b.setText(t);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setMinWidth(0);
        b.setMinimumWidth(0);
        b.setOnClickListener(l);
        return b;
    }

    private void toggleKeyboard() {
        keys.requestFocus();
        InputMethodManager im = getSystemService(InputMethodManager.class);
        im.toggleSoftInput(InputMethodManager.SHOW_FORCED, 0);
    }

    @Override protected void onResume() { super.onResume(); view.connect(); }
    @Override protected void onPause() { super.onPause(); view.disconnect(); }
    @Override protected void onDestroy() { if (gpu != null) gpu.stop(); super.onDestroy(); }

    /** Hardware keyboards: keys go straight through as keysyms; the soft keyboard goes through the text field. */
    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        boolean hardware = e.getDevice() != null && !e.getDevice().isVirtual();
        if (!hardware || e.getKeyCode() == KeyEvent.KEYCODE_BACK) return super.dispatchKeyEvent(e);
        int sym = RfbView.keysym(e);
        if (sym == 0) return super.dispatchKeyEvent(e);
        if (e.getAction() == KeyEvent.ACTION_DOWN || e.getAction() == KeyEvent.ACTION_UP)
            view.key(sym, e.getAction() == KeyEvent.ACTION_DOWN);
        return true;
    }

    /** Raw-encoding RFB client. Touch is direct: tap clicks, drag moves, long press drags, two fingers scroll. */
    static class RfbView extends View {
        private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);
        private Bitmap frame;
        private int fbW, fbH;
        private Socket sock;
        private OutputStream out;
        private Thread reader;
        private String message = "Connecting to the Mac…";
        private int modifiers; // bit per sticky modifier
        private final int[] modSyms = {0xffe7, 0xffe9, 0xffe3};
        private float downX, downY, lastY;
        private long downAt;
        private boolean dragging, moved, scrolling;
        private int buttons;
        /* Over a GpuView: the frame is the GPU's; this view only places the pointer and says why there is none. */
        boolean overlay;

        RfbView(Context c) {
            super(c);
            setFocusable(true);
        }

        void connect() {
            synchronized (outbox) { outbox.clear(); }
            reader = new Thread(this::loop, "rfb");
            reader.start();
            writer = new Thread(this::writeLoop, "rfb-input");
            writer.start();
        }

        void disconnect() {
            try { if (sock != null) sock.close(); } catch (IOException ignored) { }
            if (reader != null) reader.interrupt();
            if (writer != null) writer.interrupt();
        }

        private void loop() {
            while (!Thread.currentThread().isInterrupted()) {
                try {
                    session();
                } catch (Exception e) {
                    message = VmService.busy() ? "Waiting for the Mac's screen…" : "The Mac isn't running.";
                    postInvalidate();
                }
                try { Thread.sleep(1000); } catch (InterruptedException e) { return; }
            }
        }

        private void session() throws IOException {
            Socket s = new Socket();
            s.connect(new InetSocketAddress("127.0.0.1", 5900), 2000);
            s.setTcpNoDelay(true);
            sock = s;
            DataInputStream in = new DataInputStream(s.getInputStream());
            out = s.getOutputStream();
            byte[] ver = new byte[12];
            in.readFully(ver);
            send("RFB 003.008\n".getBytes("US-ASCII"));
            int n = in.readUnsignedByte();
            in.skipBytes(n);
            send(new byte[]{1});                       // security: None
            if (in.readInt() != 0) throw new IOException("refused");
            send(new byte[]{1});                       // shared
            fbW = in.readUnsignedShort();
            fbH = in.readUnsignedShort();
            in.skipBytes(16);                          // pixel format: we keep the server's (BGRA)
            in.skipBytes(in.readInt());                // name
            send(new byte[]{2, 0, 0, 2, 0, 0, 0, 0, (byte) 0xff, (byte) 0xff, (byte) 0xff, 0x21}); // raw, DesktopSize
            allocate(fbW, fbH);
            message = null;
            request(false);
            int[] row = new int[0];
            byte[] bytes = new byte[0];
            while (true) {
                int type = in.readUnsignedByte();
                if (type == 0) {
                    in.readUnsignedByte();
                    int rects = in.readUnsignedShort();
                    for (int r = 0; r < rects; r++) {
                        int x = in.readUnsignedShort(), y = in.readUnsignedShort();
                        int w = in.readUnsignedShort(), h = in.readUnsignedShort(), enc = in.readInt();
                        if (enc == -223) { fbW = w; fbH = h; allocate(w, h); continue; }
                        if (enc != 0) throw new IOException("encoding " + enc);
                        if (row.length < w) { row = new int[w]; bytes = new byte[w * 4]; }
                        for (int yy = 0; yy < h; yy++) {
                            in.readFully(bytes, 0, w * 4);
                            for (int i = 0; i < w; i++) {
                                int b = bytes[i * 4] & 0xff, g = bytes[i * 4 + 1] & 0xff, rr = bytes[i * 4 + 2] & 0xff;
                                row[i] = 0xff000000 | rr << 16 | g << 8 | b;
                            }
                            synchronized (this) { if (frame != null) frame.setPixels(row, 0, w, x, y + yy, w, 1); }
                        }
                    }
                    postInvalidate();
                    request(true);
                } else if (type == 2) {
                    // bell
                } else if (type == 3) {
                    in.readUnsignedByte(); in.readUnsignedShort();
                    in.skipBytes(in.readInt());
                } else {
                    throw new IOException("message " + type);
                }
            }
        }

        private synchronized void allocate(int w, int h) {
            if (frame == null || frame.getWidth() != w || frame.getHeight() != h)
                frame = Bitmap.createBitmap(Math.max(w, 1), Math.max(h, 1), Bitmap.Config.ARGB_8888);
        }

        private void request(boolean incremental) throws IOException {
            send(new byte[]{3, (byte) (incremental ? 1 : 0), 0, 0, 0, 0,
                    (byte) (fbW >> 8), (byte) fbW, (byte) (fbH >> 8), (byte) fbH});
        }

        private synchronized void send(byte[] b) throws IOException {
            if (out == null) throw new IOException("closed");
            out.write(b);
            out.flush();
        }

        /* Input goes out from one thread, in the order it happened. (A thread per message let a key's
           release overtake its press, which left the key held on the Mac, repeating; and let pointer
           positions arrive out of order, which shook the pointer.) */
        private final java.util.ArrayDeque<byte[]> outbox = new java.util.ArrayDeque<>();
        private Thread writer;

        private void sendAsync(byte[] b) {
            synchronized (outbox) {
                // A move with the same buttons as the move still waiting replaces it: only the latest matters.
                byte[] last = outbox.peekLast();
                if (b[0] == 5 && last != null && last[0] == 5 && last[1] == b[1]) outbox.pollLast();
                if (outbox.size() < 4096) outbox.addLast(b);
                outbox.notify();
            }
        }

        private void writeLoop() {
            while (true) {
                byte[] b;
                synchronized (outbox) {
                    while (outbox.isEmpty()) {
                        try { outbox.wait(); } catch (InterruptedException e) { return; }
                    }
                    b = outbox.pollFirst();
                }
                try { send(b); } catch (IOException ignored) { } // no Mac to hear it: dropped
            }
        }

        // ---- drawing ----

        private final Rect dst = new Rect();

        /** Where the Mac's screen lies in this view: the framebuffer letterboxed, as the GPU view letterboxes its
         *  frame (screen_vk.c). From the size alone, so touches map before (or without) a VNC frame. */
        private void layoutScreen() {
            if (fbW == 0 || fbH == 0 || getWidth() == 0 || getHeight() == 0) return;
            float s = Math.min(getWidth() / (float) fbW, getHeight() / (float) fbH);
            int w = Math.round(fbW * s), h = Math.round(fbH * s);
            int x = (getWidth() - w) / 2, y = (getHeight() - h) / 2;
            dst.set(x, y, x + w, y + h);
        }

        @Override protected void onDraw(Canvas c) {
            if (!overlay) c.drawColor(Color.BLACK);
            Bitmap f;
            synchronized (this) { f = frame; }
            layoutScreen();
            if (f != null && message == null) {
                if (!overlay) synchronized (this) { c.drawBitmap(f, null, dst, paint); }
            } else if (overlay && message == null) {
                // nothing yet: the GPU view shows what there is
            } else {
                Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
                p.setColor(Color.LTGRAY);
                p.setTextSize(42);
                p.setTextAlign(Paint.Align.CENTER);
                c.drawText(message == null ? "" : message, getWidth() / 2f, getHeight() / 2f, p);
            }
        }

        // ---- pointer ----

        private int fx(float x) { return dst.width() == 0 ? 0 : clamp((int) ((x - dst.left) * fbW / dst.width()), fbW); }
        private int fy(float y) { return dst.height() == 0 ? 0 : clamp((int) ((y - dst.top) * fbH / dst.height()), fbH); }
        private static int clamp(int v, int max) { return Math.max(0, Math.min(max - 1, v)); }

        private void pointer(int mask, float x, float y) {
            int px = fx(x), py = fy(y);
            sendAsync(new byte[]{5, (byte) mask, (byte) (px >> 8), (byte) px, (byte) (py >> 8), (byte) py});
        }

        private final Runnable longPress = () -> {
            if (!moved && !scrolling) { dragging = true; buttons = 1; pointer(1, downX, downY); performHapticFeedback(0); }
        };

        /** A mouse (or a trackpad in pointer mode): its position, buttons and wheel go through as they are;
         *  the finger heuristics (tap, long press, two-finger scroll) are for touch alone. */
        private static boolean isMouse(MotionEvent e) {
            return (e.getSource() & android.view.InputDevice.SOURCE_MOUSE) == android.view.InputDevice.SOURCE_MOUSE
                    || e.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE;
        }

        private int mouseButtons(MotionEvent e) {
            int st = e.getButtonState(), m = 0;
            if ((st & MotionEvent.BUTTON_PRIMARY) != 0) m |= 1;
            if ((st & MotionEvent.BUTTON_TERTIARY) != 0) m |= 2;
            if ((st & MotionEvent.BUTTON_SECONDARY) != 0) m |= 4;
            return m;
        }

        @Override public boolean onGenericMotionEvent(MotionEvent e) {
            if (!isMouse(e)) return super.onGenericMotionEvent(e);
            layoutScreen();
            switch (e.getActionMasked()) {
            case MotionEvent.ACTION_HOVER_MOVE:
            case MotionEvent.ACTION_HOVER_ENTER:
                buttons = mouseButtons(e);
                pointer(buttons, e.getX(), e.getY());
                return true;
            case MotionEvent.ACTION_SCROLL: {
                float v = e.getAxisValue(MotionEvent.AXIS_VSCROLL), hz = e.getAxisValue(MotionEvent.AXIS_HSCROLL);
                int b = buttons | (v > 0 ? 8 : v < 0 ? 16 : 0) | (hz < 0 ? 32 : hz > 0 ? 64 : 0);
                if (b != buttons) { pointer(b, e.getX(), e.getY()); pointer(buttons, e.getX(), e.getY()); }
                return true;
            }
            case MotionEvent.ACTION_BUTTON_PRESS:
            case MotionEvent.ACTION_BUTTON_RELEASE:
                buttons = mouseButtons(e);
                pointer(buttons, e.getX(), e.getY());
                return true;
            }
            return super.onGenericMotionEvent(e);
        }

        @Override public boolean onTouchEvent(MotionEvent e) {
            layoutScreen();
            if (isMouse(e)) {
                // Android sends a mouse's button presses as touch down/up too: the button state is the truth.
                buttons = mouseButtons(e);
                pointer(buttons, e.getX(), e.getY());
                return true;
            }
            switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                downX = e.getX(); downY = e.getY(); downAt = e.getEventTime();
                moved = dragging = scrolling = false;
                buttons = 0;
                pointer(0, downX, downY);
                postDelayed(longPress, 500);
                return true;
            case MotionEvent.ACTION_POINTER_DOWN:
                if (e.getPointerCount() == 2) { scrolling = true; lastY = e.getY(1); removeCallbacks(longPress); }
                return true;
            case MotionEvent.ACTION_MOVE:
                if (scrolling && e.getPointerCount() >= 2) {
                    float dy = e.getY(1) - lastY;
                    if (Math.abs(dy) > 24) {
                        int b = dy < 0 ? 16 : 8; // wheel down / up
                        pointer(b, e.getX(0), e.getY(0));
                        pointer(0, e.getX(0), e.getY(0));
                        lastY = e.getY(1);
                    }
                    return true;
                }
                if (Math.hypot(e.getX() - downX, e.getY() - downY) > 12) { moved = true; removeCallbacks(longPress); }
                if (moved || dragging) pointer(buttons, e.getX(), e.getY());
                return true;
            case MotionEvent.ACTION_UP:
                removeCallbacks(longPress);
                if (scrolling) return true;
                if (dragging) {
                    pointer(0, e.getX(), e.getY());
                } else if (!moved && e.getEventTime() - downAt < 500) {
                    pointer(1, e.getX(), e.getY());  // click
                    pointer(0, e.getX(), e.getY());
                }
                return true;
            }
            return true;
        }

        // ---- keys ----

        void key(int sym, boolean down) {
            sendAsync(new byte[]{4, (byte) (down ? 1 : 0), 0, 0,
                    (byte) (sym >> 24), (byte) (sym >> 16), (byte) (sym >> 8), (byte) sym});
        }

        void tapKey(int sym, boolean shift) {
            for (int i = 0; i < modSyms.length; i++) if ((modifiers >> i & 1) != 0) key(modSyms[i], true);
            if (shift) key(0xffe1, true);
            key(sym, true);
            key(sym, false);
            if (shift) key(0xffe1, false);
            for (int i = 0; i < modSyms.length; i++) if ((modifiers >> i & 1) != 0) key(modSyms[i], false);
            modifiers = 0; // sticky for one key, as on the iOS app
        }

        void toggleModifier(int sym) {
            for (int i = 0; i < modSyms.length; i++) if (modSyms[i] == sym) modifiers ^= 1 << i;
        }

        void typeChar(char c) {
            if (c == '\n') { tapKey(0xff0d, false); return; }
            if (c < 32 || c > 126) return;
            boolean shift = needsShift(c);
            tapKey(shift ? unshift(c) : c, shift);
        }

        private static boolean needsShift(char c) {
            return Character.isUpperCase(c) || "~!@#$%^&*()_+{}|:\"<>?".indexOf(c) >= 0;
        }

        private static int unshift(char c) {
            if (Character.isUpperCase(c)) return Character.toLowerCase(c);
            String from = "~!@#$%^&*()_+{}|:\"<>?", to = "`1234567890-=[]\\;',./";
            int i = from.indexOf(c);
            return i >= 0 ? to.charAt(i) : c;
        }

        static int keysym(KeyEvent e) {
            switch (e.getKeyCode()) {
            case KeyEvent.KEYCODE_DEL: return 0xff08;
            case KeyEvent.KEYCODE_FORWARD_DEL: return 0xffff;
            case KeyEvent.KEYCODE_TAB: return 0xff09;
            case KeyEvent.KEYCODE_ENTER: case KeyEvent.KEYCODE_NUMPAD_ENTER: return 0xff0d;
            case KeyEvent.KEYCODE_ESCAPE: return 0xff1b;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 0xff51;
            case KeyEvent.KEYCODE_DPAD_UP: return 0xff52;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 0xff53;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 0xff54;
            case KeyEvent.KEYCODE_MOVE_HOME: return 0xff50;
            case KeyEvent.KEYCODE_MOVE_END: return 0xff57;
            case KeyEvent.KEYCODE_PAGE_UP: return 0xff55;
            case KeyEvent.KEYCODE_PAGE_DOWN: return 0xff56;
            case KeyEvent.KEYCODE_SHIFT_LEFT: return 0xffe1;
            case KeyEvent.KEYCODE_SHIFT_RIGHT: return 0xffe2;
            case KeyEvent.KEYCODE_CTRL_LEFT: return 0xffe3;
            case KeyEvent.KEYCODE_CTRL_RIGHT: return 0xffe4;
            case KeyEvent.KEYCODE_ALT_LEFT: return 0xffe9;
            case KeyEvent.KEYCODE_ALT_RIGHT: return 0xffea;
            case KeyEvent.KEYCODE_META_LEFT: return 0xffe7;
            case KeyEvent.KEYCODE_META_RIGHT: return 0xffe8;
            case KeyEvent.KEYCODE_CAPS_LOCK: return 0xffe5;
            }
            if (e.getKeyCode() >= KeyEvent.KEYCODE_F1 && e.getKeyCode() <= KeyEvent.KEYCODE_F12)
                return 0xffbe + (e.getKeyCode() - KeyEvent.KEYCODE_F1);
            int ch = e.getUnicodeChar(0); // unshifted: the Shift key travels on its own
            return ch >= 32 && ch < 127 ? ch : 0;
        }
    }
}

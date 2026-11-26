package io.github.spikeoutrecomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.util.SparseIntArray;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import org.libsdl.app.SDLActivity;
import java.util.HashSet;
import java.util.Set;

/**
 * On-screen stick and buttons. They send the game's default keyboard keys:
 * arrows, Z beat, X charge, C jump, V shift, 5 coin, 1 start, Esc menu.
 * Touches outside the controls go on to the game (SDL), so its menus can be tapped.
 */
public class TouchControls extends View {
    private static final class Button {
        final String label; final int key; float x, y, r;
        Button(String label, int key) { this.label = label; this.key = key; }
    }

    private final Button beat = new Button("SOCO", KeyEvent.KEYCODE_Z);
    private final Button charge = new Button("CARGA", KeyEvent.KEYCODE_X);
    private final Button jump = new Button("PULO", KeyEvent.KEYCODE_C);
    private final Button shift = new Button("SHIFT", KeyEvent.KEYCODE_V);
    private final Button coin = new Button("FICHA", KeyEvent.KEYCODE_5);
    private final Button start = new Button("START", KeyEvent.KEYCODE_1);
    private final Button menu = new Button("MENU", KeyEvent.KEYCODE_ESCAPE);
    private final Button ok = new Button("OK", KeyEvent.KEYCODE_ENTER);           // the menu: choose
    private final Button back = new Button("VOLTAR", KeyEvent.KEYCODE_DEL);       // the menu: back (Backspace)
    private final Button[] buttons = {beat, charge, jump, shift, coin, start, menu, ok, back};
    private final Button toggle = new Button("TOQUE", 0); // shows or hides the rest

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Set<Integer> held = new HashSet<>();
    private final SparseIntArray owners = new SparseIntArray(); // pointer id -> STICK or BUTTON
    private static final int STICK = 1, BUTTON = 2;
    private float stickX, stickY, stickR, knobX, knobY;
    private boolean visible = true;

    public TouchControls(Context context) { super(context); }

    private static void place(Button b, float x, float y, float r) { b.x = x; b.y = y; b.r = r; }

    @Override
    protected void onSizeChanged(int w, int h, int oldW, int oldH) {
        releaseAll();
        final float u = h / 100f; // one percent of the height (landscape)
        stickR = 18 * u; stickX = 34 * u; stickY = h - 28 * u;
        knobX = stickX; knobY = stickY;
        final float cx = w - 36 * u, cy = h - 30 * u, r = 9 * u;
        place(beat, cx - 15 * u, cy, r);
        place(charge, cx, cy - 15 * u, r);
        place(jump, cx, cy + 15 * u, r);
        place(shift, cx + 15 * u, cy, r);
        place(coin, w * 0.5f - 13 * u, h - 7 * u, 5.5f * u);
        place(start, w * 0.5f + 13 * u, h - 7 * u, 5.5f * u);
        place(menu, w - 24 * u, 8 * u, 5.5f * u);
        place(toggle, w - 10 * u, 8 * u, 5.5f * u);
        place(ok, w - 38 * u, 8 * u, 5.5f * u);
        place(back, w - 52 * u, 8 * u, 5.5f * u);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        circle(canvas, toggle, false);
        if (!visible) return;
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0x33FFFFFF);
        canvas.drawCircle(stickX, stickY, stickR, paint);
        paint.setColor(0x77FFFFFF);
        canvas.drawCircle(knobX, knobY, stickR * 0.42f, paint);
        for (Button b : buttons) circle(canvas, b, held.contains(b.key));
    }

    private void circle(Canvas canvas, Button b, boolean down) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(down ? 0x88FFCF3A : 0x33FFFFFF);
        canvas.drawCircle(b.x, b.y, b.r, paint);
        paint.setColor(0xCCFFFFFF);
        paint.setTextAlign(Paint.Align.CENTER);
        paint.setTextSize(b.r * 0.46f);
        canvas.drawText(b.label, b.x, b.y + b.r * 0.16f, paint);
    }

    private static boolean inside(float x, float y, float cx, float cy, float r) {
        return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
    }

    private Button buttonAt(float x, float y) {
        for (Button b : buttons) if (inside(x, y, b.x, b.y, b.r * 1.15f)) return b;
        return null;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        final int action = e.getActionMasked(), index = e.getActionIndex();
        if (action == MotionEvent.ACTION_CANCEL) { releaseAll(); return true; }
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            final float x = e.getX(index), y = e.getY(index);
            final int id = e.getPointerId(index);
            if (inside(x, y, toggle.x, toggle.y, toggle.r * 1.15f)) {
                releaseAll();
                visible = !visible;
                invalidate();
                return true;
            }
            if (visible && inside(x, y, stickX, stickY, stickR * 1.4f)) owners.put(id, STICK);
            else if (visible && buttonAt(x, y) != null) owners.put(id, BUTTON);
            else if (action == MotionEvent.ACTION_DOWN) return false;
        }
        // the keys wanted now, from every finger still down
        final Set<Integer> wanted = new HashSet<>();
        final boolean lifting = action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP;
        knobX = stickX; knobY = stickY;
        for (int i = 0; i < e.getPointerCount(); i++) {
            final int id = e.getPointerId(i);
            if (lifting && i == index) { owners.delete(id); continue; }
            final int owner = owners.get(id);
            if (owner == STICK) stick(e.getX(i), e.getY(i), wanted);
            else if (owner == BUTTON) {
                final Button b = buttonAt(e.getX(i), e.getY(i)); // sliding between buttons works
                if (b != null) wanted.add(b.key);
            }
        }
        send(wanted);
        return true;
    }

    /** An 8-way lever: a dead zone, then eight slices of 45 degrees. */
    private void stick(float x, float y, Set<Integer> wanted) {
        final float dx = x - stickX, dy = y - stickY;
        final float distance = (float) Math.hypot(dx, dy);
        final float scale = distance > stickR ? stickR / distance : 1;
        knobX = stickX + dx * scale; knobY = stickY + dy * scale;
        if (distance < stickR * 0.25f) return;
        final int slice = ((int) Math.round(Math.atan2(dy, dx) / (Math.PI / 4)) + 8) % 8; // 0 right, 2 down
        if (slice == 7 || slice == 0 || slice == 1) wanted.add(KeyEvent.KEYCODE_DPAD_RIGHT);
        if (slice >= 1 && slice <= 3) wanted.add(KeyEvent.KEYCODE_DPAD_DOWN);
        if (slice >= 3 && slice <= 5) wanted.add(KeyEvent.KEYCODE_DPAD_LEFT);
        if (slice >= 5 && slice <= 7) wanted.add(KeyEvent.KEYCODE_DPAD_UP);
    }

    private void send(Set<Integer> wanted) {
        for (int key : held) if (!wanted.contains(key)) SDLActivity.onNativeKeyUp(key);
        for (int key : wanted) if (!held.contains(key)) SDLActivity.onNativeKeyDown(key);
        held.clear();
        held.addAll(wanted);
        invalidate();
    }

    void releaseAll() {
        owners.clear();
        knobX = stickX; knobY = stickY;
        send(new HashSet<>());
    }
}

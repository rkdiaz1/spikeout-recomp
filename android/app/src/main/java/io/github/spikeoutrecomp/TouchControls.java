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
    private enum Icon { NONE, PUNCH, KICK, JUMP, SHIFT, SPECIAL, COMBO }

    private static final class Button {
        final String label; final Icon icon; final int color; final int[] keys; float x, y, r;
        Button(String label, int... keys) { this.label = label; this.icon = Icon.NONE; this.color = 0xFFFFFFFF; this.keys = keys; }
        Button(Icon icon, int color, int... keys) { this.label = ""; this.icon = icon; this.color = color; this.keys = keys; }
    }

    private static final int Z = KeyEvent.KEYCODE_Z, X = KeyEvent.KEYCODE_X, C = KeyEvent.KEYCODE_C;
    private final Button beat = new Button(Icon.PUNCH, 0xFFE5484D, Z);
    private final Button charge = new Button(Icon.KICK, 0xFF3E8EED, X);
    private final Button jump = new Button(Icon.JUMP, 0xFF35B779, C);
    private final Button shift = new Button(Icon.SHIFT, 0xFF9B6CE0, KeyEvent.KEYCODE_V);
    private final Button special = new Button(Icon.SPECIAL, 0xFFF2A900, Z, X, C); // beat + charge + jump
    private final Button combo = new Button(Icon.COMBO, 0xFFEE7B30, Z, X);        // beat + charge
    private final Button coin = new Button("FICHA", KeyEvent.KEYCODE_5);
    private final Button start = new Button("START", KeyEvent.KEYCODE_1);
    private final Button menu = new Button("MENU", KeyEvent.KEYCODE_ESCAPE);
    private final Button ok = new Button("OK", KeyEvent.KEYCODE_ENTER);           // the menu: choose
    private final Button back = new Button("VOLTAR", KeyEvent.KEYCODE_DEL);       // the menu: back (Backspace)
    private final Button[] buttons = {beat, charge, jump, shift, special, combo, coin, start, menu, ok, back};
    private final Button toggle = new Button("TOQUE"); // shows or hides the rest
    private final Set<Button> pressed = new HashSet<>();

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
        place(beat, w - 38 * u, h - 22 * u, 10 * u);
        place(charge, w - 17 * u, h - 28 * u, 10 * u);
        place(jump, w - 14 * u, h - 52 * u, 9 * u);
        place(combo, w - 36 * u, h - 46 * u, 9 * u);
        place(special, w - 58 * u, h - 38 * u, 9 * u);
        place(shift, w - 60 * u, h - 14 * u, 9 * u);
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
        // the stick: a dark base with a ring and four arrows, a glossy knob
        fill(canvas, stickX, stickY, stickR, 0x55101218);
        ring(canvas, stickX, stickY, stickR, stickR * 0.03f, 0x99FFFFFF);
        paint.setColor(0x99FFFFFF);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeCap(Paint.Cap.ROUND);
        paint.setStrokeWidth(stickR * 0.05f);
        for (int i = 0; i < 4; i++) {
            final float dx = i == 0 ? 1 : i == 2 ? -1 : 0, dy = i == 1 ? 1 : i == 3 ? -1 : 0;
            final float tx = stickX + dx * stickR * 0.86f, ty = stickY + dy * stickR * 0.86f;
            final float bx = tx - dx * stickR * 0.12f, by = ty - dy * stickR * 0.12f;
            canvas.drawLine(bx - dy * stickR * 0.11f, by - dx * stickR * 0.11f, tx, ty, paint);
            canvas.drawLine(bx + dy * stickR * 0.11f, by + dx * stickR * 0.11f, tx, ty, paint);
        }
        fill(canvas, knobX, knobY, stickR * 0.45f, 0x66000000);
        fill(canvas, knobX, knobY, stickR * 0.40f, 0xBBD8DCE6);
        fill(canvas, knobX - stickR * 0.08f, knobY - stickR * 0.10f, stickR * 0.20f, 0x66FFFFFF);
        for (Button b : buttons) circle(canvas, b, pressed.contains(b));
    }

    private void fill(Canvas canvas, float x, float y, float r, int color) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(color);
        canvas.drawCircle(x, y, r, paint);
    }

    private void ring(Canvas canvas, float x, float y, float r, float width, int color) {
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(width);
        paint.setColor(color);
        canvas.drawCircle(x, y, r, paint);
    }

    private void circle(Canvas canvas, Button b, boolean down) {
        final float r = down ? b.r * 0.93f : b.r;
        if (b.icon == Icon.NONE) {
            fill(canvas, b.x, b.y, r, down ? 0xAAFFCF3A : 0x66101218);
            ring(canvas, b.x, b.y, r, b.r * 0.06f, 0x99FFFFFF);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(0xEEFFFFFF);
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setTextSize(b.r * 0.46f);
            canvas.drawText(b.label, b.x, b.y + b.r * 0.16f, paint);
            return;
        }
        final int rgb = b.color & 0x00FFFFFF;
        fill(canvas, b.x, b.y, r, (down ? 0xE6000000 : 0x80000000) | rgb);
        fill(canvas, b.x, b.y, r * 0.80f, 0x30000000);
        ring(canvas, b.x, b.y, r, r * 0.08f, down ? 0xFFFFFFFF : 0xCCFFFFFF);
        figure(canvas, b.icon, b.x, b.y, r / 50f);
    }

    // The figures, in a circle of radius 50 around (0, 0). Each row: a limb as a line of points x,y,x,y...
    private static final float[][] PUNCH = {{-8,-8, -10,12}, {-10,12, -22,22, -24,35}, {-10,12, 3,21, 7,35},
            {-8,-6, 10,-9, 30,-10}, {-8,-6, -21,1, -13,-12}};
    private static final float[][] KICK = {{-14,-10, -8,10}, {-8,10, -13,23, -12,37}, {-8,10, 11,6, 31,-2},
            {-13,-6, -27,-2, -31,-14}, {-13,-6, 1,1, 8,-9}};
    private static final float[][] JUMP = {{0,-13, 0,5}, {0,5, -15,9, -12,22}, {0,5, 15,12, 7,23},
            {0,-9, -15,-12, -23,-27}, {0,-9, 15,-12, 23,-27}};
    private static final float[][] SHIFT = {{0,-10, 0,9}, {0,9, -9,21, -12,35}, {0,9, 9,21, 12,35},
            {0,-6, -11,1, -4,-11}, {0,-6, 11,1, 4,-11}};
    private static final float[][] SPECIAL = {{0,-9, 0,8}, {0,8, -10,18, -17,29}, {0,8, 10,18, 17,29},
            {0,-5, -14,-8, -21,-18}, {0,-5, 14,-8, 21,-18}};
    private static final float[][] COMBO = {{-13,-11, -9,10}, {-9,10, -14,23, -13,36}, {-9,10, 9,12, 29,13},
            {-12,-8, 7,-12, 29,-13}, {-12,-8, -26,-3, -29,-15}};

    private void figure(Canvas canvas, Icon icon, float cx, float cy, float s) {
        final float[][] limbs;
        float headX = 0, headY = -24;
        switch (icon) {
            case PUNCH: limbs = PUNCH; headX = -7; headY = -22; break;
            case KICK: limbs = KICK; headX = -17; headY = -23; break;
            case JUMP: limbs = JUMP; headY = -27; break;
            case SHIFT: limbs = SHIFT; break;
            case SPECIAL: limbs = SPECIAL; headY = -23; break;
            default: limbs = COMBO; headX = -15; headY = -24; break;
        }
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeCap(Paint.Cap.ROUND);
        paint.setColor(0xFFFFFFFF);
        paint.setStrokeWidth(7 * s);
        for (float[] limb : limbs) lines(canvas, cx, cy, s, limb);
        fill(canvas, cx + headX * s, cy + headY * s, 8 * s, 0xFFFFFFFF);
        // fists, impact marks, motion lines, arrows, the burst
        if (icon == Icon.PUNCH) fill(canvas, cx + 30 * s, cy - 10 * s, 6 * s, 0xFFFFFFFF);
        if (icon == Icon.COMBO) fill(canvas, cx + 29 * s, cy - 13 * s, 5.5f * s, 0xFFFFFFFF);
        paint.setStyle(Paint.Style.STROKE);
        paint.setColor(0xFFFFFFFF);
        paint.setStrokeWidth(3 * s);
        if (icon == Icon.PUNCH) {
            lines(canvas, cx, cy, s, 38,-20, 43,-25); lines(canvas, cx, cy, s, 40,-10, 46,-10); lines(canvas, cx, cy, s, 38,0, 43,5);
        } else if (icon == Icon.KICK) {
            lines(canvas, cx, cy, s, 37,-12, 41,-18); lines(canvas, cx, cy, s, 39,-2, 45,-2); lines(canvas, cx, cy, s, 36,8, 41,13);
        } else if (icon == Icon.COMBO) {
            lines(canvas, cx, cy, s, 38,-17, 44,-21); lines(canvas, cx, cy, s, 39,0, 46,0); lines(canvas, cx, cy, s, 38,17, 44,21);
        } else if (icon == Icon.JUMP) {
            lines(canvas, cx, cy, s, -14,34, -14,40); lines(canvas, cx, cy, s, 0,31, 0,41); lines(canvas, cx, cy, s, 14,34, 14,40);
        } else if (icon == Icon.SHIFT) {
            lines(canvas, cx, cy, s, -29,2, -39,12, -29,22); lines(canvas, cx, cy, s, 29,2, 39,12, 29,22);
        } else if (icon == Icon.SPECIAL) {
            for (int i = 0; i < 8; i++) {
                final double a = Math.PI / 8 + i * Math.PI / 4;
                final float ax = (float) Math.cos(a), ay = (float) Math.sin(a);
                lines(canvas, cx, cy, s, ax * 34, ay * 34, ax * 44, ay * 44);
            }
        }
    }

    private void lines(Canvas canvas, float cx, float cy, float s, float... p) {
        paint.setStyle(Paint.Style.STROKE);
        for (int i = 0; i + 3 < p.length; i += 2)
            canvas.drawLine(cx + p[i] * s, cy + p[i + 1] * s, cx + p[i + 2] * s, cy + p[i + 3] * s, paint);
    }

    private static boolean inside(float x, float y, float cx, float cy, float r) {
        return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
    }

    /** The nearest button the finger is on (their touch areas may overlap a little). */
    private Button buttonAt(float x, float y) {
        Button best = null;
        double nearest = 1.15;
        for (Button b : buttons) {
            final double d = Math.hypot(x - b.x, y - b.y) / b.r;
            if (d <= nearest) { best = b; nearest = d; }
        }
        return best;
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
        pressed.clear();
        final boolean lifting = action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP;
        knobX = stickX; knobY = stickY;
        for (int i = 0; i < e.getPointerCount(); i++) {
            final int id = e.getPointerId(i);
            if (lifting && i == index) { owners.delete(id); continue; }
            final int owner = owners.get(id);
            if (owner == STICK) stick(e.getX(i), e.getY(i), wanted);
            else if (owner == BUTTON) {
                final Button b = buttonAt(e.getX(i), e.getY(i)); // sliding between buttons works
                if (b != null) {
                    pressed.add(b);
                    for (int key : b.keys) wanted.add(key);
                }
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
        pressed.clear();
        knobX = stickX; knobY = stickY;
        send(new HashSet<>());
    }
}

package io.github.spikeoutrecomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
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
        place(beat, w - 36 * u, h - 20 * u, 8.5f * u);
        place(charge, w - 16 * u, h - 26 * u, 8.5f * u);
        place(jump, w - 13 * u, h - 47 * u, 7.5f * u);
        place(combo, w - 33 * u, h - 41 * u, 7.5f * u);
        place(special, w - 53 * u, h - 34 * u, 7.5f * u);
        place(shift, w - 55 * u, h - 13 * u, 7.5f * u);
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
        // an arcade cabinet's button of the nineties: a black bezel, a domed face, a glint
        final int rgb = b.color & 0x00FFFFFF, alpha = down ? 0xF2000000 : 0xC8000000;
        fill(canvas, b.x, b.y, r * 1.06f, 0xC0101014);
        ring(canvas, b.x, b.y, r * 0.97f, r * 0.05f, down ? 0xFFFFE45C : 0xD0E8E8F0);
        fill(canvas, b.x, b.y, r * 0.90f, alpha | darker(rgb));
        fill(canvas, b.x, b.y - r * 0.05f, r * 0.82f, alpha | (down ? lighter(rgb) : rgb));
        glint(canvas, b.x, b.y - r * 0.05f, r * 0.82f);
        figure(canvas, b.icon, b.x, b.y - r * 0.03f, r / 62f);
    }

    private static int darker(int rgb) { return (rgb >> 1) & 0x7F7F7F; }

    private static int lighter(int rgb) { return rgb + ((0xFFFFFF - rgb) >> 2 & 0x3F3F3F); }

    private void glint(Canvas canvas, float x, float y, float r) {
        final Path p = new Path();
        p.moveTo(x - r * 0.72f, y - r * 0.34f);
        p.quadTo(x - r * 0.52f, y - r * 0.92f, x + r * 0.16f, y - r * 0.90f);
        p.quadTo(x - r * 0.40f, y - r * 0.72f, x - r * 0.72f, y - r * 0.34f);
        p.close();
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0x80FFFFFF);
        canvas.drawPath(p, paint);
    }

    // The icons, drawn as comic stickers in a circle of radius 50 around (0, 0): each piece has a hard
    // shadow, a thick black outline and a flat colour.
    private static final int CREASE = 0xE0121014;
    private static final int INK = 0xFF121014, WHITE = 0xFFFFFFFF, YELLOW = 0xFFFFE03A, RED = 0xFFE8291C,
            ORANGE = 0xFFFF8A14, SKIN_SHADE = 0xFFBFD0EA;
    private boolean inking;      // drawing a piece's shadow or outline
    private int tone = WHITE;    // the piece's colour

    private void sticker(Canvas canvas, int color, Runnable piece) {
        inking = true;
        canvas.save();
        canvas.translate(3.5f, 4.5f);
        piece.run();
        canvas.restore();
        piece.run();
        inking = false;
        tone = color;
        piece.run();
    }

    private void figure(Canvas canvas, Icon icon, float cx, float cy, float s) {
        canvas.save();
        canvas.translate(cx, cy);
        canvas.scale(s, s);
        switch (icon) {
            case PUNCH: // a fist from the left, hitting
                sticker(canvas, YELLOW, () -> shape(canvas, 6,-17, 17,-39, 22,-19, 44,-27, 31,-8, 47,1,
                        31,9, 43,28, 22,20, 17,39, 6,17));
                sticker(canvas, WHITE, () -> {
                    canvas.save();
                    canvas.translate(1, 0);
                    canvas.scale(1.08f, 1.08f);
                    fist(canvas);
                    canvas.restore();
                });
                sticker(canvas, WHITE, () -> { // speed streaks, pointed behind
                    shape(canvas, -46,-23, -15,-27, -15,-20); shape(canvas, -44,25, -17,21, -17,28);
                });
                break;
            case KICK: // a leg kicking up and forward, its sole first
                sticker(canvas, YELLOW, () -> shape(canvas, 14,-22, 19,-46, 27,-27, 47,-35, 36,-15, 50,-5,
                        35,-2, 40,17, 24,5));
                sticker(canvas, WHITE, () -> {
                    shape(canvas, -42,15, -14,6, 8,-15, 20,0, -4,20, -28,35);
                    canvas.save();
                    canvas.translate(15, -10);
                    canvas.rotate(-122);
                    canvas.scale(0.78f, 0.78f);
                    boot(canvas);
                    canvas.restore();
                });
                stroke(canvas, 2.6f, CREASE, -15, 8, -5, 21); // the knee
                break;
            case JUMP: // a boot springing off the ground, an arrow up
                sticker(canvas, YELLOW, () -> shape(canvas, 0,-46, 24,-21, 10,-21, 10,-6, -10,-6, -10,-21, -24,-21));
                sticker(canvas, WHITE, () -> {
                    canvas.save();
                    canvas.translate(-2, 10);
                    canvas.scale(0.76f, 0.76f);
                    canvas.rotate(-14);
                    boot(canvas);
                    canvas.restore();
                });
                sticker(canvas, WHITE, () -> { // the dust
                    dot(canvas, -27, 33, 6); dot(canvas, -17, 35, 4.5f); dot(canvas, 27, 33, 6); dot(canvas, 17, 35, 4.5f);
                    line(canvas, 4, -9,38, 9,38);
                });
                break;
            case SHIFT: // a boot stepping to either side
                sticker(canvas, WHITE, () -> {
                    canvas.save();
                    canvas.translate(0, -12);
                    canvas.scale(0.86f, 0.86f);
                    boot(canvas);
                    canvas.restore();
                });
                sticker(canvas, YELLOW, () -> shape(canvas, -42,24, -24,10, -24,19, 24,19, 24,10, 42,24,
                        24,38, 24,29, -24,29, -24,38));
                break;
            case SPECIAL: // a fist wrapped in flames
                sticker(canvas, RED, () -> flames(canvas));
                sticker(canvas, ORANGE, () -> {
                    canvas.save();
                    canvas.translate(0, 6);
                    canvas.scale(0.72f, 0.72f);
                    flames(canvas);
                    canvas.restore();
                });
                sticker(canvas, WHITE, () -> {
                    canvas.save();
                    canvas.translate(0, 3);
                    canvas.scale(0.94f, 0.94f);
                    frontFist(canvas);
                    canvas.restore();
                });
                break;
            default: // a punch above, a kick below, at once
                sticker(canvas, YELLOW, () -> shape(canvas, 9,-5, 25,-22, 24,-6, 47,-9, 32,3, 48,15,
                        27,13, 30,30, 13,13));
                sticker(canvas, WHITE, () -> {
                    canvas.save();
                    canvas.translate(-6, -21);
                    canvas.scale(0.74f, 0.74f);
                    fist(canvas);
                    canvas.restore();
                });
                sticker(canvas, WHITE, () -> {
                    shape(canvas, -40,23, -12,11, -3,25, -29,40);
                    canvas.save();
                    canvas.translate(7, 17);
                    canvas.rotate(-102);
                    canvas.scale(0.58f, 0.58f);
                    boot(canvas);
                    canvas.restore();
                });
                break;
        }
        canvas.restore();
    }

    private void shape(Canvas canvas, float... p) {
        final Path path = new Path();
        path.moveTo(p[0], p[1]);
        for (int i = 2; i + 1 < p.length; i += 2) path.lineTo(p[i], p[i + 1]);
        path.close();
        filled(canvas, path);
    }

    private void filled(Canvas canvas, Path path) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(inking ? INK : tone);
        canvas.drawPath(path, paint);
        if (!inking) return;
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeJoin(Paint.Join.ROUND);
        paint.setStrokeWidth(7);
        canvas.drawPath(path, paint);
    }

    private void dot(Canvas canvas, float x, float y, float r) {
        fill(canvas, x, y, inking ? r + 3 : r, inking ? INK : tone);
    }

    private void box(Canvas canvas, float left, float top, float right, float bottom, float corner) {
        final RectF r = new RectF(left, top, right, bottom);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(inking ? INK : tone);
        canvas.drawRoundRect(r, corner, corner, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(inking ? 7 : 2.6f);
        paint.setColor(inking ? INK : CREASE);
        canvas.drawRoundRect(r, corner, corner, paint);
    }

    private void line(Canvas canvas, float width, float x0, float y0, float x1, float y1) {
        draw(canvas, inking ? width + 6 : width, inking ? INK : tone, x0, y0, x1, y1);
    }

    /** A crease, drawn only over a piece's colour. */
    private void stroke(Canvas canvas, float width, int color, float x0, float y0, float x1, float y1) {
        if (!inking) draw(canvas, width, color, x0, y0, x1, y1);
    }

    private void draw(Canvas canvas, float width, int color, float x0, float y0, float x1, float y1) {
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeCap(Paint.Cap.ROUND);
        paint.setStrokeWidth(width);
        paint.setColor(color);
        canvas.drawLine(x0, y0, x1, y1, paint);
    }

    /** A closed fist seen from the side, its knuckles to the right. */
    private void fist(Canvas canvas) {
        shape(canvas, -34,-10, -13,-13, -13,13, -34,11); // the wrist
        stroke(canvas, 2.6f, CREASE, -23, -11, -23, 11); // the glove's cuff
        box(canvas, -15,-18, 10,17, 8);                   // the hand
        for (int i = 0; i < 4; i++) box(canvas, 1, -19 + i * 9.4f, 20, -9.6f + i * 9.4f, 4.7f); // the fingers
        box(canvas, -11,3, 9,14, 5.5f);                   // the thumb across them
    }

    /** A boot in profile, its toe to the right. */
    private void boot(Canvas canvas) {
        final Path p = new Path();
        p.moveTo(-18, -26);
        p.lineTo(4, -26);
        p.lineTo(5, -6);
        p.quadTo(9, 0, 22, 2);
        p.quadTo(31, 5, 30, 15);
        p.lineTo(-19, 15);
        p.quadTo(-22, 5, -17, -3);
        p.close();
        filled(canvas, p);
        if (inking) return;
        final int was = tone;
        tone = SKIN_SHADE; // the sole, in the second tone
        shape(canvas, -19.5f,8, 29,8, 30,15, -19,15);
        tone = was;
        stroke(canvas, 3, CREASE, -19, 8, 29, 8);
        stroke(canvas, 2.6f, CREASE, -17, -18, 4, -18);
        stroke(canvas, 2.4f, CREASE, -3, -6, 6, -9);    // the laces
        stroke(canvas, 2.4f, CREASE, 0, 0, 10, -3);
    }

    private void flames(Canvas canvas) {
        final Path p = new Path();
        p.moveTo(-21, 28);
        p.quadTo(-46, 8, -31, -22);
        p.quadTo(-29, -9, -20, -8);
        p.quadTo(-27, -29, -4, -46);
        p.quadTo(-9, -28, 6, -19);
        p.quadTo(8, -33, 21, -38);
        p.quadTo(15, -23, 25, -13);
        p.quadTo(28, -21, 35, -24);
        p.quadTo(47, 7, 21, 28);
        p.close();
        filled(canvas, p);
    }

    /** A fist coming at the viewer. */
    private void frontFist(Canvas canvas) {
        box(canvas, -12,20, 13,35, 4);                   // the wrist
        for (int i = 0; i < 4; i++) box(canvas, -22 + i * 11, -9, -11 + i * 11, 15, 5.5f); // the knuckles
        box(canvas, -19,10, 20,27, 8);                   // the palm
        box(canvas, -21,9, 7,21, 6);                     // the thumb
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

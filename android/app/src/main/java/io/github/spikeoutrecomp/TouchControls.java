package io.github.spikeoutrecomp;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.util.SparseIntArray;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import org.libsdl.app.SDLActivity;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/**
 * On-screen stick and buttons. They send the game's default keyboard keys:
 * arrows, Z beat, X charge, C jump, V shift, 5 coin, 1 start, Esc menu.
 * With the game's menu open: a D-pad instead of the stick, OK, back, save (F9) and load (F10); on its list of
 * saves, delete too.
 * With a controller connected they go away (but save and load in the menu, which a pad has no button for).
 * Touches outside the controls go on to the game (SDL), so its menus can be tapped.
 * The game menu's "Touch buttons" opens the editor: the play controls dragged, resized and made see-through, kept
 * between starts.
 * To spare the battery each button is drawn once into a picture, and the screen again only when something shows.
 */
public class TouchControls extends View {
    private enum Icon { NONE, PUNCH, KICK, JUMP, SHIFT, SPECIAL, COMBO }

    private static final class Button {
        final String label; final Icon icon; final int color; final int[] keys; float x, y, r;
        String id;                  // the name its place is kept under (null: not movable)
        float homeX, homeY, homeR;  // its default place
        Bitmap up, down;            // its pictures, drawn when first wanted, at the size artR
        float artR;
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
    private final Button save = new Button("SALVAR", KeyEvent.KEYCODE_F9);
    private final Button load = new Button("CARREGAR", KeyEvent.KEYCODE_F10);
    private final Button delete = new Button("APAGAR", KeyEvent.KEYCODE_FORWARD_DEL); // the Saves page (Delete)
    private final Button[] playButtons = {beat, charge, jump, shift, special, combo, coin, start, menu};
    private final Button[] menuButtons = {menu, ok, back, save, load};
    private final Button[] savesButtons = {menu, ok, back, save, load, delete};
    private final Button[] padMenuButtons = {save, load}; // the menu, with a controller
    private final Button[] none = {};
    private final Set<Button> pressed = new HashSet<>();

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Set<Integer> held = new HashSet<>();
    private final SparseIntArray owners = new SparseIntArray(); // pointer id -> STICK or BUTTON
    private static final int STICK = 1, BUTTON = 2;
    private float stickX, stickY, stickR, knobX, knobY;
    private boolean menuOpen, savesPage, controller;

    // The player's layout: for a moved or resized control, its centre (fractions of the screen) and its size
    // (times the default). Used in play and in the editor; the menu's controls stay where they are.
    private final Button lever = new Button("");
    private final Button[] movable = {lever, beat, charge, jump, shift, special, combo, coin, start, menu};
    private final Map<String, float[]> custom = new HashMap<>();
    private SharedPreferences store;
    private int width, height;

    // How solid the play controls are: 1 as designed, down to 1 - MAX_CLEAR (the editor's bar).
    private static final float MAX_CLEAR = 0.8f;
    private static final int DPAD_ALPHA = 0x78; // the menu's D-pad, over the menu: see-through
    private float opacity = 1, fade = 1;        // fade: applied to the colours being drawn
    private final Paint art = new Paint(Paint.FILTER_BITMAP_FLAG);
    private String shownState = "";

    // the editor
    private RectF clearBar;
    private float trackLeft, trackRight;
    private int sliderId = -1;
    private static final float MIN_SCALE = 0.6f, MAX_SCALE = 1.8f;
    private final String[] tools = {"\u2212", "+", "PADR\u00C3O", "PRONTO"};
    private final RectF[] toolBoxes = new RectF[4];
    private float toolsLeft, toolsTop, toolsRight, toolsBottom;
    private boolean editing;
    private Button selected;
    private int dragId = -1, pinchId = -1;
    private float grabX, grabY, pinchDistance, pinchScale;

    public TouchControls(Context context) {
        super(context);
        final String[] ids = {"stick", "beat", "charge", "jump", "shift", "special", "combo", "coin", "start", "menu"};
        for (int i = 0; i < movable.length; i++) movable[i].id = ids[i];
        if (context == null) return;
        store = context.getSharedPreferences("touch_layout", Context.MODE_PRIVATE);
        opacity = clamp(store.getFloat("opacity", 1), 1 - MAX_CLEAR, 1);
        for (String id : ids) {
            if (store.contains(id + ".x"))
                custom.put(id, new float[] {store.getFloat(id + ".x", 0.5f), store.getFloat(id + ".y", 0.5f),
                        store.getFloat(id + ".size", 1)});
        }
    }

    private static void place(Button b, float x, float y, float r) { b.homeX = x; b.homeY = y; b.homeR = r; }

    private static float clamp(float v, float low, float high) { return Math.max(low, Math.min(high, v)); }

    /** Every control put where it belongs now. */
    private void applyLayout() {
        final Button[] all = {lever, beat, charge, jump, shift, special, combo, coin, start, menu, ok, back, save, load, delete};
        for (Button b : all) {
            final float[] own = (editing || !menuOpen) && b.id != null ? custom.get(b.id) : null;
            b.r = own == null ? b.homeR : b.homeR * own[2];
            b.x = own == null ? b.homeX : clamp(own[0] * width, b.r, width - b.r);
            b.y = own == null ? b.homeY : clamp(own[1] * height, b.r, height - b.r);
        }
        stickX = lever.x; stickY = lever.y; stickR = lever.r;
        knobX = stickX; knobY = stickY;
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldW, int oldH) {
        releaseAll();
        width = w; height = h;
        final float u = h / 100f; // one percent of the height (landscape)
        place(lever, 34 * u, h - 28 * u, 18 * u);
        // the editor's row of tools, in the upper middle, clear of the default controls
        final float[] wide = {14 * u, 14 * u, 30 * u, 30 * u};
        toolsTop = 19 * u; toolsBottom = 31 * u;
        toolsLeft = (w - (88 * u + 3 * 3 * u)) / 2;
        float at = toolsLeft;
        for (int i = 0; i < 4; i++) {
            toolBoxes[i] = new RectF(at, toolsTop, at + wide[i], toolsBottom);
            at += wide[i] + 3 * u;
        }
        toolsRight = at - 3 * u;
        // under them, the transparency's bar
        clearBar = new RectF(toolsLeft, 34 * u, toolsRight, 45 * u);
        trackLeft = toolsLeft + 47 * u; trackRight = toolsRight - 6 * u;
        place(beat, w - 36 * u, h - 20 * u, 8.5f * u);
        place(charge, w - 16 * u, h - 26 * u, 8.5f * u);
        place(jump, w - 13 * u, h - 47 * u, 7.5f * u);
        place(combo, w - 33 * u, h - 41 * u, 7.5f * u);
        place(special, w - 53 * u, h - 34 * u, 7.5f * u);
        place(shift, w - 55 * u, h - 13 * u, 7.5f * u);
        place(coin, w * 0.5f - 13 * u, h - 7 * u, 5.5f * u);
        place(start, w * 0.5f + 13 * u, h - 7 * u, 5.5f * u);
        place(menu, w - 10 * u, 8 * u, 5.5f * u);
        place(ok, w - 16 * u, h - 22 * u, 9 * u);
        place(back, w - 38 * u, h - 15 * u, 8 * u);
        place(save, w - 13 * u, h - 52 * u, 8.5f * u);
        place(load, w - 33 * u, h - 52 * u, 8.5f * u);
        place(delete, w - 13 * u, h - 71 * u, 8 * u);
        applyLayout();
        send(new HashSet<>());
    }

    private Button[] shown() {
        if (editing) return playButtons;
        if (menuOpen) return controller ? padMenuButtons : savesPage ? savesButtons : menuButtons; // (a pad deletes with Y)
        return controller ? none : playButtons;
    }

    /** The game's menu opened or closed, or went to or from its list of saves. */
    void setMenu(boolean open, boolean saves) {
        if (!open && editing) stopEditing(); // (the menu closed under the editor)
        final boolean changed = open != menuOpen;
        if (changed) releaseAll();
        menuOpen = open;
        savesPage = saves;
        if (changed) applyLayout();
        invalidate();
    }

    boolean isEditing() { return editing; }

    /** The editor, over the game's menu. */
    void startEditing() {
        releaseAll();
        editing = true;
        selected = null;
        dragId = pinchId = sliderId = -1;
        applyLayout();
        invalidate();
    }

    void stopEditing() {
        editing = false;
        selected = null;
        dragId = pinchId = sliderId = -1;
        saveLayout();
        applyLayout();
        invalidate();
    }

    private void saveLayout() {
        if (store == null) return;
        final SharedPreferences.Editor e = store.edit();
        e.clear();
        e.putFloat("opacity", opacity);
        for (Map.Entry<String, float[]> c : custom.entrySet()) {
            e.putFloat(c.getKey() + ".x", c.getValue()[0]);
            e.putFloat(c.getKey() + ".y", c.getValue()[1]);
            e.putFloat(c.getKey() + ".size", c.getValue()[2]);
        }
        e.apply();
    }

    /** A control moved to (x, y) at this size: kept on the screen and out from under the tools. */
    private void move(Button b, float x, float y, float scale) {
        scale = clamp(scale, MIN_SCALE, MAX_SCALE);
        final float r = b.homeR * scale, margin = height * 0.03f;
        x = clamp(x, r, width - r);
        y = clamp(y, r, height - r);
        if (x > toolsLeft - margin && x < toolsRight + margin && y > toolsTop - margin && y < clearBar.bottom + margin)
            y = clearBar.bottom + margin;
        custom.put(b.id, new float[] {x / width, y / height, scale});
        applyLayout();
    }

    private float scaleOf(Button b) { return b.r / b.homeR; }

    /** The control under the finger in the editor: the one whose centre is nearest, for its size. */
    private Button movableAt(float x, float y) {
        Button best = null;
        double nearest = 1.2;
        for (Button b : movable) {
            final double d = Math.hypot(x - b.x, y - b.y) / b.r;
            if (d <= nearest) { best = b; nearest = d; }
        }
        return best;
    }

    /** One finger drags a control; the tools or a second finger (a pinch) resize it. */
    private void edit(MotionEvent e) {
        final int action = e.getActionMasked(), index = e.getActionIndex();
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            final float x = e.getX(index), y = e.getY(index);
            if (sliderId >= 0) return;
            if (dragId < 0 && clearBar.contains(x, y)) {
                sliderId = e.getPointerId(index);
                slide(x);
                return;
            }
            if (dragId >= 0) { // a second finger: pinch the dragged control
                if (pinchId >= 0) return;
                for (int i = 0; i < e.getPointerCount(); i++) {
                    if (e.getPointerId(i) != dragId) continue;
                    pinchId = e.getPointerId(index);
                    pinchDistance = (float) Math.max(1, Math.hypot(x - e.getX(i), y - e.getY(i)));
                    pinchScale = scaleOf(selected);
                }
                return;
            }
            for (int i = 0; i < 4; i++) {
                if (!toolBoxes[i].contains(x, y)) continue;
                if (i == 3) { stopEditing(); return; }
                if (i == 2) { custom.clear(); opacity = 1; selected = null; applyLayout(); }
                else if (selected != null) move(selected, selected.x, selected.y, scaleOf(selected) + (i == 0 ? -0.1f : 0.1f));
                saveLayout();
                invalidate();
                return;
            }
            final Button hit = movableAt(x, y);
            if (hit != null) {
                selected = hit;
                dragId = e.getPointerId(index);
                grabX = hit.x - x; grabY = hit.y - y;
                invalidate();
            }
            return;
        }
        if (action == MotionEvent.ACTION_MOVE && sliderId >= 0) {
            for (int i = 0; i < e.getPointerCount(); i++)
                if (e.getPointerId(i) == sliderId) slide(e.getX(i));
            return;
        }
        if (action == MotionEvent.ACTION_MOVE && dragId >= 0) {
            int first = -1, second = -1;
            for (int i = 0; i < e.getPointerCount(); i++) {
                if (e.getPointerId(i) == dragId) first = i;
                if (e.getPointerId(i) == pinchId) second = i;
            }
            if (first < 0) return;
            if (second >= 0) {
                final double d = Math.hypot(e.getX(second) - e.getX(first), e.getY(second) - e.getY(first));
                move(selected, selected.x, selected.y, (float) (pinchScale * d / pinchDistance));
            } else if (pinchId < 0) {
                move(selected, e.getX(first) + grabX, e.getY(first) + grabY, scaleOf(selected));
            }
            invalidate();
            return;
        }
        final boolean cancelled = action == MotionEvent.ACTION_CANCEL;
        if (cancelled || action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            final int id = e.getPointerId(index);
            if (cancelled || id == sliderId) {
                sliderId = -1;
                saveLayout();
            }
            if (cancelled || id == dragId || id == pinchId) { // (after a pinch, lift and touch again to drag)
                dragId = pinchId = -1;
                saveLayout();
            }
        }
    }

    /** The finger at x on the transparency's bar. */
    private void slide(float x) {
        final float clear = clamp((x - trackLeft) / (trackRight - trackLeft), 0, 1) * MAX_CLEAR;
        opacity = 1 - Math.round(clear * 20) / 20f; // (steps of five percent)
        invalidate();
    }

    private void drawClearBar(Canvas canvas) {
        final float u = height / 100f, middle = (clearBar.top + clearBar.bottom) / 2;
        final float at = trackLeft + (1 - opacity) / MAX_CLEAR * (trackRight - trackLeft);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0xFF2C2F38);
        canvas.drawRoundRect(clearBar, 2.5f * u, 2.5f * u, paint);
        paint.setColor(0xFFFFFFFF);
        paint.setTextAlign(Paint.Align.CENTER);
        paint.setTextSize(3.8f * u);
        canvas.drawText("Transpar\u00EAncia: " + Math.round((1 - opacity) * 100) + "%", clearBar.left + 22 * u, middle + 1.4f * u, paint);
        paint.setColor(0xFF6A7080);
        canvas.drawRoundRect(new RectF(trackLeft, middle - u, trackRight, middle + u), u, u, paint);
        paint.setColor(0xFFFFE03A);
        canvas.drawRoundRect(new RectF(trackLeft, middle - u, at, middle + u), u, u, paint);
        fill(canvas, at, middle, 3.6f * u, 0xFFFFE03A);
    }

    private void drawEditor(Canvas canvas) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0xB8101218);
        canvas.drawRect(0, 0, width, height, paint);
        fade = opacity;
        drawStick(canvas);
        fade = 1;
        for (Button b : playButtons) button(canvas, b, false, opacity);
        drawClearBar(canvas);
        if (selected != null) ring(canvas, selected.x, selected.y, selected.r * 1.16f, height * 0.008f, 0xFFFFE03A);
        final float u = height / 100f;
        paint.setStyle(Paint.Style.FILL);
        paint.setTextAlign(Paint.Align.CENTER);
        paint.setColor(0xFFFFFFFF);
        paint.setTextSize(3.8f * u);
        canvas.drawText("Arraste um bot\u00E3o para mover.", width / 2f, 8 * u, paint);
        canvas.drawText("Toque nele e use \u2212 e + (ou pin\u00E7a) para mudar o tamanho.", width / 2f, 14 * u, paint);
        for (int i = 0; i < 4; i++) {
            final RectF box = toolBoxes[i];
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(i == 3 ? 0xFF1F7A4D : i == 2 ? 0xFFA8242C : selected == null ? 0xFF2C2F38 : 0xFF3C4250);
            canvas.drawRoundRect(box, 2.5f * u, 2.5f * u, paint);
            paint.setColor(i < 2 && selected == null ? 0x70FFFFFF : 0xFFFFFFFF);
            paint.setTextSize((i < 2 ? 8 : 4.6f) * u);
            canvas.drawText(tools[i], (box.left + box.right) / 2, (box.top + box.bottom) / 2 + (i < 2 ? 2.8f : 1.7f) * u, paint);
        }
    }

    /** A controller connected, or the last one gone. */
    void setController(boolean connected) {
        if (connected == controller) return;
        releaseAll();
        controller = connected;
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        if (editing) { drawEditor(canvas); return; }
        if (!controller) {
            if (menuOpen) {
                canvas.saveLayerAlpha(new RectF(stickX - stickR * 1.1f, stickY - stickR * 1.1f,
                        stickX + stickR * 1.1f, stickY + stickR * 1.1f), DPAD_ALPHA);
                drawDpad(canvas);
                canvas.restore();
            } else {
                fade = opacity;
                drawStick(canvas);
                fade = 1;
            }
        }
        for (Button b : shown()) button(canvas, b, pressed.contains(b), menuOpen ? 1 : opacity);
    }

    /** A button's picture put on the screen (drawn the first time, or after its size changes). */
    private void button(Canvas canvas, Button b, boolean down, float solid) {
        if (b.artR != b.r) {
            b.up = b.down = null;
            b.artR = b.r;
        }
        final int half = (int) Math.ceil(b.r * 1.2f);
        Bitmap picture = down ? b.down : b.up;
        if (picture == null) {
            picture = Bitmap.createBitmap(2 * half, 2 * half, Bitmap.Config.ARGB_8888);
            final Canvas into = new Canvas(picture);
            into.translate(half - b.x, half - b.y);
            circle(into, b, down);
            if (down) b.down = picture;
            else b.up = picture;
        }
        art.setAlpha(Math.round(255 * solid));
        canvas.drawBitmap(picture, b.x - half, b.y - half, art);
    }

    private void drawStick(Canvas canvas) {
        // the stick: a dark base with a ring and four arrows, a glossy knob
        fill(canvas, stickX, stickY, stickR, 0x55101218);
        ring(canvas, stickX, stickY, stickR, stickR * 0.03f, 0x99FFFFFF);
        paint.setColor(faded(0x99FFFFFF));
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
    }

    /** The menu's D-pad: a black cross, each arm with its arrow, lit while held. */
    private void drawDpad(Canvas canvas) {
        final float arm = stickR * 0.34f, corner = stickR * 0.08f, edge = stickR * 0.045f;
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0xD0E8E8F0);
        canvas.drawRoundRect(new RectF(stickX - stickR - edge, stickY - arm - edge, stickX + stickR + edge, stickY + arm + edge), corner, corner, paint);
        canvas.drawRoundRect(new RectF(stickX - arm - edge, stickY - stickR - edge, stickX + arm + edge, stickY + stickR + edge), corner, corner, paint);
        paint.setColor(0xF0181A20);
        canvas.drawRoundRect(new RectF(stickX - stickR, stickY - arm, stickX + stickR, stickY + arm), corner, corner, paint);
        canvas.drawRoundRect(new RectF(stickX - arm, stickY - stickR, stickX + arm, stickY + stickR), corner, corner, paint);
        final int[] keys = {KeyEvent.KEYCODE_DPAD_RIGHT, KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_UP};
        for (int i = 0; i < 4; i++) {
            final float dx = i == 0 ? 1 : i == 2 ? -1 : 0, dy = i == 1 ? 1 : i == 3 ? -1 : 0;
            final float tip = stickR * 0.86f, base = stickR * 0.56f, half = stickR * 0.19f;
            final Path p = new Path();
            p.moveTo(stickX + dx * tip, stickY + dy * tip);
            p.lineTo(stickX + dx * base - dy * half, stickY + dy * base + dx * half);
            p.lineTo(stickX + dx * base + dy * half, stickY + dy * base - dx * half);
            p.close();
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(held.contains(keys[i]) ? 0xFFFFE03A : 0xFFE8E8F0);
            canvas.drawPath(p, paint);
        }
        fill(canvas, stickX, stickY, arm * 0.45f, 0xFF30333C);
    }

    private int faded(int color) { return (Math.round((color >>> 24) * fade) << 24) | (color & 0x00FFFFFF); }

    private void fill(Canvas canvas, float x, float y, float r, int color) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(faded(color));
        canvas.drawCircle(x, y, r, paint);
    }

    private void ring(Canvas canvas, float x, float y, float r, float width, int color) {
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(width);
        paint.setColor(faded(color));
        canvas.drawCircle(x, y, r, paint);
    }

    private void circle(Canvas canvas, Button b, boolean down) {
        final float r = down ? b.r * 0.93f : b.r;
        if (b.icon == Icon.NONE) {
            final int base = b == save ? 0xB01F7A4D : b == load ? 0xB0245FA8 : b == delete ? 0xB0A8242C : 0x66101218;
            fill(canvas, b.x, b.y, r, down ? 0xAAFFCF3A : base);
            ring(canvas, b.x, b.y, r, b.r * 0.06f, 0x99FFFFFF);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(0xEEFFFFFF);
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setTextSize(b.r * (b.label.length() > 6 ? 0.34f : 0.46f));
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
            case PUNCH: // a fist thrown at the viewer: four knuckles leading, the thumb tucked behind
                sticker(canvas, YELLOW, () -> shape(canvas, 14,-48, 20,-33, 31,-40, 30,-27, 49,-31, 36,-17, 49,-11, 36,-5, 50,10, 30,5, 31,18, 20,11, 14,28, 8,11, -2,18, -2,5, -17,7, -8,-5, -20,-11, -8,-17, -18,-30, -2,-27, -3,-40, 8,-33));
                canvas.save();
                canvas.translate(-4, 4);
                canvas.scale(0.92f, 0.92f);
                sticker(canvas, WHITE, () -> punchThumb(canvas));
                sticker(canvas, WHITE, () -> punchHand(canvas));
                stroke(canvas, 4.2f, INK, -8, -13, -21, 21); // between the fingers
                stroke(canvas, 4.2f, INK, 8, -8, -6, 27);
                stroke(canvas, 4.2f, INK, 23, -1, 9, 29);
                canvas.restore();
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

    private void punchThumb(Canvas canvas) {
        final Path p = new Path();
        p.moveTo(-28, -5);
        p.quadTo(-41, 12, -40, 25);
        p.quadTo(-25, 39, -9, 35);
        p.lineTo(-18, 22);
        p.close();
        filled(canvas, p);
    }

    private void punchHand(Canvas canvas) {
        final Path p = new Path();
        p.moveTo(-35, 15);
        p.lineTo(-21, -24);
        p.quadTo(-18, -34, -10, -33);
        p.quadTo(-3, -33, -2, -24);   // the first knuckle
        p.quadTo(5, -31, 11, -27);
        p.quadTo(15, -25, 15, -18);   // the second
        p.quadTo(22, -24, 27, -19);
        p.quadTo(30, -17, 30, -12);   // the third
        p.quadTo(42, -12, 40, -1);    // the fourth
        p.quadTo(36, 17, 24, 30);
        p.quadTo(10, 36, -6, 31);
        p.quadTo(-23, 26, -35, 15);
        p.close();
        filled(canvas, p);
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
        for (Button b : shown()) {
            final double d = Math.hypot(x - b.x, y - b.y) / b.r;
            if (d <= nearest) { best = b; nearest = d; }
        }
        return best;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (editing) { edit(e); return true; } // (every touch is the editor's)
        final int action = e.getActionMasked(), index = e.getActionIndex();
        if (action == MotionEvent.ACTION_CANCEL) { releaseAll(); return true; }
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            final float x = e.getX(index), y = e.getY(index);
            final int id = e.getPointerId(index);
            final Button under = buttonAt(x, y);
            final boolean onButton = under != null && inside(x, y, under.x, under.y, under.r); // (one moved by the stick)
            if (!controller && !onButton && inside(x, y, stickX, stickY, stickR * 1.4f)) owners.put(id, STICK);
            else if (buttonAt(x, y) != null) owners.put(id, BUTTON);
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
        if (menuOpen) { // the D-pad: one direction at a time, so a menu's row isn't changed while moving down
            if (Math.abs(dx) > Math.abs(dy)) wanted.add(dx > 0 ? KeyEvent.KEYCODE_DPAD_RIGHT : KeyEvent.KEYCODE_DPAD_LEFT);
            else wanted.add(dy > 0 ? KeyEvent.KEYCODE_DPAD_DOWN : KeyEvent.KEYCODE_DPAD_UP);
            return;
        }
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
        // The screen again only when what shows has changed (a finger resting sends moves up to 240 times a second;
        // the stick's knob is redrawn each time it has gone a little way).
        final float step = Math.max(1, stickR * 0.04f);
        final StringBuilder state = new StringBuilder().append(held).append(Math.round(knobX / step))
                .append(',').append(Math.round(knobY / step));
        for (Button b : shown()) state.append(pressed.contains(b) ? '1' : '0');
        final String now = state.toString();
        if (!now.equals(shownState)) {
            shownState = now;
            invalidate();
        }
    }

    void releaseAll() {
        owners.clear();
        pressed.clear();
        knobX = stickX; knobY = stickY;
        send(new HashSet<>());
    }
}

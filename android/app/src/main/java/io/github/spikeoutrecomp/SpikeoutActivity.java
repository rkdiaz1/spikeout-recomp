package io.github.spikeoutrecomp;

import android.content.pm.ActivityInfo;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import org.libsdl.app.SDLActivity;

/** SDL's activity plus the on-screen controls. */
public class SpikeoutActivity extends SDLActivity implements InputManager.InputDeviceListener {
    /** Sent by the game (tools/spikeout.cpp) as its menu changes: 0 closed, 1 open, 2 on the list of saves. */
    private static final int MESSAGE_MENU = COMMAND_USER;

    /** The game menu's "Touch buttons": the editor of their places and sizes. */
    private static final int MESSAGE_EDIT_CONTROLS = COMMAND_USER + 1;

    private TouchControls controls;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (mLayout == null) return; // (SDL failed to load: it shows its own error)
        controls = new TouchControls(this);
        mLayout.addView(controls, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    @Override
    protected boolean onUnhandledMessage(int command, Object param) {
        if (command == MESSAGE_EDIT_CONTROLS) {
            if (controls != null) controls.startEditing();
            return true;
        }
        if (command != MESSAGE_MENU || !(param instanceof Integer)) return false;
        final int shows = (Integer) param;
        if (controls != null) controls.setMenu(shows != 0, shows == 2);
        return true;
    }

    /**
     * Asks for the screen at 60 Hz: the game makes 57.5 pictures a second, and a 120 Hz screen showing them
     * only takes more power (and heat). The system may keep its own rate.
     */
    private void askFor60Hz() {
        final WindowManager.LayoutParams window = getWindow().getAttributes();
        window.preferredRefreshRate = 60;
        getWindow().setAttributes(window);
    }

    /** The touch controls go away while a real controller (USB or Bluetooth) is connected. */
    private void lookForControllers() {
        if (controls == null) return;
        boolean found = false;
        for (int id : InputDevice.getDeviceIds()) {
            final InputDevice d = InputDevice.getDevice(id);
            if (d != null && !d.isVirtual() && d.isExternal() && d.supportsSource(InputDevice.SOURCE_GAMEPAD)) found = true;
        }
        controls.setController(found);
    }

    @Override public void onInputDeviceAdded(int id) { lookForControllers(); }
    @Override public void onInputDeviceRemoved(int id) { lookForControllers(); }
    @Override public void onInputDeviceChanged(int id) { lookForControllers(); }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK && controls != null && controls.isEditing()) {
            if (event.getAction() == KeyEvent.ACTION_UP) controls.stopEditing(); // the back gesture: done
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        // The game closed (its menu's Quit): its process ends too, so the next start is a fresh one.
        if (isFinishing()) android.os.Process.killProcess(android.os.Process.myPid());
    }

    /** The system's bars hidden (swipe from an edge to show them for a moment). */
    private void hideSystemBars() {
        final WindowInsetsController bars = getWindow().getInsetsController();
        if (bars == null) return;
        bars.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        bars.hide(WindowInsets.Type.systemBars());
    }

    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
        askFor60Hz();
        final InputManager inputs = getSystemService(InputManager.class);
        inputs.registerInputDeviceListener(this, null);
        lookForControllers(); // (one may have come or gone while the game was away)
    }

    @Override
    protected void onPause() {
        if (controls != null) controls.releaseAll(); // no key left held
        getSystemService(InputManager.class).unregisterInputDeviceListener(this);
        super.onPause();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        if (!hasFocus && controls != null) controls.releaseAll();
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }
}

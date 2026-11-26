package io.github.spikeoutrecomp;

import android.content.pm.ActivityInfo;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.view.InputDevice;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import org.libsdl.app.SDLActivity;

/** SDL's activity plus the on-screen controls. */
public class SpikeoutActivity extends SDLActivity implements InputManager.InputDeviceListener {
    /** Sent by the game (tools/spikeout.cpp) as its menu opens (1) or closes (0). */
    private static final int MESSAGE_MENU = COMMAND_USER;

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
        if (command != MESSAGE_MENU || !(param instanceof Integer)) return false;
        if (controls != null) controls.setMenuOpen((Integer) param != 0);
        return true;
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

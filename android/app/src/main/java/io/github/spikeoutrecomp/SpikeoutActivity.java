package io.github.spikeoutrecomp;

import android.os.Bundle;
import android.view.ViewGroup;
import org.libsdl.app.SDLActivity;

/** SDL's activity plus the on-screen controls. */
public class SpikeoutActivity extends SDLActivity {
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
    protected void onPause() {
        if (controls != null) controls.releaseAll(); // no key left held
        super.onPause();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        if (!hasFocus && controls != null) controls.releaseAll();
        super.onWindowFocusChanged(hasFocus);
    }
}

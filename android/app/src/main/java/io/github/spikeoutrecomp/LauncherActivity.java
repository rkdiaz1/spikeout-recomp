package io.github.spikeoutrecomp;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Test builds' start screen: "open the game", and a report of why it last closed. The game runs in its own
 * process (":game"), so this screen is still here after it crashes. Nothing is sent anywhere: the player copies it.
 */
public class LauncherActivity extends Activity {
    private TextView text;
    private String report = "";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        final LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setFitsSystemWindows(true);
        final LinearLayout row = new LinearLayout(this);
        final Button play = new Button(this);
        play.setText("Abrir jogo");
        play.setOnClickListener(v -> startActivity(new Intent(this, SpikeoutActivity.class)));
        final Button copy = new Button(this);
        copy.setText("Copiar relatório");
        copy.setOnClickListener(v -> {
            final ClipboardManager board = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
            board.setPrimaryClip(ClipData.newPlainText("Spikeout", report));
            Toast.makeText(this, "Copiado", Toast.LENGTH_SHORT).show();
        });
        row.addView(play);
        row.addView(copy);
        page.addView(row);
        text = new TextView(this);
        text.setTextSize(11);
        text.setTextIsSelectable(true);
        final ScrollView scroll = new ScrollView(this);
        scroll.addView(text);
        page.addView(scroll);
        setContentView(page);
    }

    @Override
    protected void onResume() {
        super.onResume();
        report = buildReport();
        text.setText(report);
    }

    private String buildReport() {
        final StringBuilder out = new StringBuilder();
        out.append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
           .append(", Android ").append(Build.VERSION.RELEASE).append('\n');
        try {
            final ActivityManager manager = (ActivityManager) getSystemService(Context.ACTIVITY_SERVICE);
            final List<ApplicationExitInfo> exits = manager.getHistoricalProcessExitReasons(null, 0, 10);
            boolean found = false;
            for (ApplicationExitInfo exit : exits) {
                if (!exit.getProcessName().endsWith(":game")) continue;
                found = true;
                out.append("\n== Último fechamento do jogo ==\nmotivo=").append(exit.getReason())
                   .append(" status=").append(exit.getStatus())
                   .append("\ndescrição=").append(exit.getDescription()).append('\n');
                try (InputStream trace = exit.getTraceInputStream()) {
                    if (trace != null) out.append(readable(trace));
                }
                break;
            }
            if (!found) out.append("\nNenhum fechamento registrado ainda.\n");
        } catch (Exception e) {
            out.append("Motivo indisponível: ").append(e).append('\n');
        }
        appendLog(out, "spikeout.log");
        return out.toString();
    }

    /** The trace is a binary tombstone (or Java text): keep the readable names that point at the fault. */
    private static String readable(InputStream in) throws IOException {
        final ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        final byte[] buffer = new byte[65536];
        for (int n; bytes.size() < 8_000_000 && (n = in.read(buffer)) > 0; ) bytes.write(buffer, 0, n);
        final Matcher m = Pattern.compile("[\\x20-\\x7e]{4,}")
                .matcher(new String(bytes.toByteArray(), StandardCharsets.ISO_8859_1));
        final Set<String> lines = new LinkedHashSet<>();
        int size = 0;
        while (size < 9000 && m.find()) {
            final String s = m.group();
            if (s.matches("(?s).*(libmain|libSDL3|libvulkan|vulkan\\.|libc\\.so|SIG|SEGV|bort|atal|xception|::|_Z|SDL_).*")
                    && lines.add(s)) size += s.length() + 1;
        }
        return String.join("\n", lines) + "\n";
    }

    private void appendLog(StringBuilder out, String name) {
        final File file = new File(getFilesDir(), name);
        out.append("\n== ").append(name).append(" ==\n");
        if (!file.isFile()) { out.append("(não existe)\n"); return; }
        try (RandomAccessFile f = new RandomAccessFile(file, "r")) {
            final byte[] tail = new byte[(int) Math.min(f.length(), 6000)];
            f.seek(f.length() - tail.length);
            f.readFully(tail);
            out.append(new String(tail, StandardCharsets.UTF_8));
        } catch (IOException e) {
            out.append(e).append('\n');
        }
    }
}

package org.matonos.caption;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.graphics.Color;
import android.os.RemoteException;
import org.matonos.appsettings.IEmbeddedSettingsHost;
import android.util.TypedValue;
import android.view.Display;
import android.view.Gravity;
import android.view.SurfaceControlViewHost;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

final class CaptionSettingsSession {
    private final Context context;
    private final IEmbeddedSettingsHost host;
    private final SurfaceControlViewHost surfaceHost;
    private final LinearLayout content;
    private int width;
    private int height;

    CaptionSettingsSession(Context context, Display display, android.os.IBinder token,
            IEmbeddedSettingsHost host) {
        this.context = context;
        this.host = host;
        surfaceHost = new SurfaceControlViewHost(context, display, token);
        boolean dark = (context.getResources().getConfiguration().uiMode
                & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES;
        int bg = dark ? Color.rgb(28, 28, 30) : Color.rgb(250, 249, 252);
        int fg = dark ? Color.WHITE : Color.rgb(25, 27, 31);
        ScrollView scroll = new ScrollView(context);
        scroll.setBackgroundColor(bg);
        content = new LinearLayout(context);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(20), dp(12), dp(20), dp(20));
        scroll.addView(content, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        TextView heading = new TextView(context);
        heading.setText("Window title bars");
        heading.setTextColor(fg);
        heading.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
        heading.setPadding(0, 0, 0, dp(8));
        content.addView(heading);
        addChoice("Button side", CaptionTheme.SIDE, new String[]{"left", "right"},
                new String[]{"Left", "Right"}, fg);
        addChoice("Button style", CaptionTheme.STYLE, new String[]{"icons", "circles"},
                new String[]{"Icons", "Circles"}, fg);
        addChoice("Title alignment", CaptionTheme.ALIGNMENT, new String[]{"centre", "start"},
                new String[]{"Centre", "Start"}, fg);
        addChoice("Accent colour", CaptionTheme.ACCENT, new String[]{"wallpaper", "neutral"},
                new String[]{"Wallpaper", "Neutral"}, fg);
        surfaceHost.setView(scroll, 1, 1);
        try {
            host.setSurface(surfaceHost.getSurfacePackage());
            host.setPreferredHeight(dp(330));
        } catch (RemoteException e) {
            LogFailure("settings surface callback failed", e);
        }
    }

    void resize(int widthPx, int heightPx) {
        width = Math.max(1, widthPx);
        height = Math.max(1, heightPx);
        surfaceHost.relayout(width, height);
    }

    void release() { surfaceHost.release(); }

    private void addChoice(String label, String key, String[] values, String[] names, int fg) {
        SharedPreferences prefs = context.getSharedPreferences(CaptionTheme.PREFS, Context.MODE_PRIVATE);
        LinearLayout row = new LinearLayout(context);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(4), 0, dp(4));
        TextView name = new TextView(context);
        name.setText(label);
        name.setTextColor(fg);
        name.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        row.addView(name, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        Button choice = new Button(context);
        row.addView(choice, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        Runnable refresh = () -> {
            String value = prefs.getString(key, values[0]);
            int index = 0;
            for (int i = 0; i < values.length; i++) if (values[i].equals(value)) index = i;
            choice.setText(names[index]);
            choice.setOnClickListener(v -> {
                String next = values[(indexOf(values, prefs.getString(key, values[0])) + 1) % values.length];
                prefs.edit().putString(key, next).apply();
                CaptionTheme.notifyChanged();
                choice.setText(names[indexOf(values, next)]);
            });
        };
        refresh.run();
        content.addView(row, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
    }

    private int indexOf(String[] values, String value) {
        for (int i = 0; i < values.length; i++) if (values[i].equals(value)) return i;
        return 0;
    }

    private int dp(int value) { return Math.round(value * context.getResources().getDisplayMetrics().density); }

    private void LogFailure(String message, RemoteException e) {
        android.util.Log.e(CaptionProviderService.TAG, message, e);
    }
}

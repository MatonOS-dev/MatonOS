package org.matonos.caption;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Color;
import java.util.concurrent.CopyOnWriteArrayList;

final class CaptionTheme {
    static final String PREFS = "caption_settings";
    static final String SIDE = "button_side";
    static final String STYLE = "button_style";
    static final String ALIGNMENT = "title_alignment";
    static final String ACCENT = "accent_source";
    private static final CopyOnWriteArrayList<Runnable> LISTENERS = new CopyOnWriteArrayList<>();
    final boolean right;
    final boolean circles;
    final boolean startTitle;
    final boolean neutralAccent;
    final boolean dark;
    final boolean focused;
    final int accent;
    final int foreground;
    final int background;

    CaptionTheme(Context context, boolean dark, boolean focused) {
        SharedPreferences p = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        right = "right".equals(p.getString(SIDE, "left"));
        circles = !"icons".equals(p.getString(STYLE, "circles"));
        startTitle = "start".equals(p.getString(ALIGNMENT, "centre"));
        neutralAccent = "neutral".equals(p.getString(ACCENT, "wallpaper"));
        this.dark = dark;
        this.focused = focused;
        accent = resource(context, neutralAccent ? android.R.color.system_neutral1_600 : android.R.color.system_accent1_600,
                dark ? Color.rgb(210, 210, 210) : Color.rgb(70, 90, 150));
        foreground = focused
                ? resource(context, dark ? android.R.color.system_neutral1_50 : android.R.color.system_neutral1_900,
                        dark ? Color.WHITE : Color.BLACK)
                : resource(context, dark ? android.R.color.system_neutral2_200 : android.R.color.system_neutral2_700,
                        dark ? Color.LTGRAY : Color.DKGRAY);
        background = focused
                ? resource(context, dark ? android.R.color.system_neutral1_800 : android.R.color.system_neutral1_50,
                        dark ? Color.rgb(50, 50, 50) : Color.rgb(240, 240, 240))
                : resource(context, dark ? android.R.color.system_neutral1_900 : android.R.color.system_neutral1_50,
                        dark ? Color.rgb(32, 32, 32) : Color.rgb(250, 250, 250));
    }

    private static int resource(Context c, int id, int fallback) {
        try { return c.getColor(id); } catch (RuntimeException e) {
            android.util.Log.e(CaptionProviderService.TAG, "Theme color lookup failed resource=" + id, e);
            return fallback;
        }
    }
    static void addListener(Runnable r) { LISTENERS.add(r); }
    static void removeListener(Runnable r) { LISTENERS.remove(r); }
    static void notifyChanged() { for (Runnable r : LISTENERS) r.run(); }
}

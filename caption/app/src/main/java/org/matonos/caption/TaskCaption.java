package org.matonos.caption;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Rect;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.os.Bundle;
import android.os.RemoteException;
import android.view.Display;
import android.view.Gravity;
import android.view.SurfaceControlViewHost;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.util.ArrayList;

final class TaskCaption {
    private final Context context;
    private final int taskId;
    private final Display display;
    private final IBinderToken token;
    private final ICaptionHost host;
    private SurfaceControlViewHost surfaceHost;
    private FrameLayout root;
    private TextView title;
    private ImageView icon;
    private LinearLayout buttons;
    private boolean transparent;
    private boolean maximized;
    private boolean surfacePublished;
    private int width;
    private int height;
    private Bundle state = new Bundle();
    private final Runnable preferenceListener = this::render;

    TaskCaption(Context context, int taskId, Display display, android.os.IBinder hostToken,
            ICaptionHost host) {
        this.context = context;
        this.taskId = taskId;
        this.display = display;
        this.token = new IBinderToken(hostToken);
        this.host = host;
        CaptionTheme.addListener(preferenceListener);
    }

    void update(int widthPx, int heightPx, Bundle newState) {
        width = Math.max(1, widthPx);
        height = Math.max(1, heightPx);
        state = newState == null ? new Bundle() : new Bundle(newState);
        transparent = state.getBoolean("clientDecorated") || !state.getBoolean("buttonsAllowed");
        maximized = state.getBoolean("maximized");
        if (surfaceHost == null) {
            surfaceHost = new SurfaceControlViewHost(context, display, token.binder);
            root = new FrameLayout(context);
            root.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> sendRegions());
        }
        render();
        if (!surfacePublished) {
            surfaceHost.setView(root, width, height);
            try {
                host.setSurface(taskId, surfaceHost.getSurfacePackage());
                surfacePublished = true;
            } catch (RemoteException e) { throw new IllegalStateException("setSurface failed", e); }
        } else {
            surfaceHost.relayout(width, height);
        }
    }

    private void render() {
        if (root == null) return;
        root.removeAllViews();
        root.setBackgroundColor(android.graphics.Color.TRANSPARENT);
        if (transparent) {
            sendRegions();
            return;
        }
        boolean dark = state.getBoolean("darkMode");
        CaptionTheme theme = new CaptionTheme(context, dark, state.getBoolean("focused"));
        root.setBackgroundColor(theme.background);
        int heightDp = Math.max(1, Math.round(height / context.getResources().getDisplayMetrics().density));
        int buttonDp = Math.max(1, Math.round(heightDp * 0.75f));
        // Same gap beside the first circle as above and below it, as in the reference.
        int pad = Math.max(1, (heightDp - buttonDp) / 2);
        FrameLayout.LayoutParams buttonsLp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.MATCH_PARENT,
                theme.right ? Gravity.RIGHT : Gravity.LEFT);
        buttonsLp.leftMargin = theme.right ? 0 : dp(pad);
        buttonsLp.rightMargin = theme.right ? dp(pad) : 0;
        buttons = new LinearLayout(context);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.CENTER_VERTICAL);
        root.addView(buttons, buttonsLp);

        int maxGlyph = maximized ? R.drawable.caption_restore : R.drawable.caption_maximize;
        int[] glyphs = theme.right
                ? new int[]{R.drawable.caption_minimize, maxGlyph, R.drawable.caption_close}
                : new int[]{R.drawable.caption_close, maxGlyph, R.drawable.caption_minimize};
        for (int i = 0; i < 3; i++) {
            final int action = theme.right ? 2 - i : i;
            ImageView button = button(glyphs[i], theme, theme.circles);
            LinearLayout.LayoutParams buttonLp = new LinearLayout.LayoutParams(dp(buttonDp), dp(buttonDp));
            if (i > 0) buttonLp.leftMargin = dp(BUTTON_GAP_DP);
            buttons.addView(button, buttonLp);
            button.setOnClickListener(v -> invoke(action));
        }

        LinearLayout identity = new LinearLayout(context);
        identity.setOrientation(LinearLayout.HORIZONTAL);
        identity.setGravity(Gravity.CENTER_VERTICAL);
        int side = dp(26);
        icon = new ImageView(context);
        icon.setScaleType(ImageView.ScaleType.FIT_CENTER);
        Bitmap bitmap = state.getParcelable("icon", Bitmap.class);
        if (bitmap != null) icon.setImageBitmap(bitmap);
        else icon.setVisibility(View.GONE);
        identity.addView(icon, new LinearLayout.LayoutParams(side, side));
        title = new TextView(context);
        title.setSingleLine(true);
        title.setEllipsize(android.text.TextUtils.TruncateAt.END);
        title.setText(state.getCharSequence("title", ""));
        title.setTextColor(theme.foreground);
        title.setTextSize(18);
        LinearLayout.LayoutParams titleLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        titleLp.leftMargin = bitmap != null ? dp(8) : 0;
        identity.addView(title, titleLp);
        FrameLayout.LayoutParams identityLp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.MATCH_PARENT,
                theme.startTitle ? Gravity.LEFT | Gravity.CENTER_VERTICAL : Gravity.CENTER);
        if (theme.startTitle) {
            int controlsWidth = dp(buttonDp * 3 + 2 * BUTTON_GAP_DP + pad);
            identityLp.leftMargin = theme.right ? dp(12) : controlsWidth + dp(12);
            identityLp.rightMargin = theme.right ? controlsWidth + dp(12) : dp(12);
        }
        root.addView(identity, identityLp);
        identity.setOnLongClickListener(v -> { sendMenuAnchor(identity); return true; });
        root.setFocusable(false);
        root.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> sendRegions());
        root.post(this::sendRegions);
    }

    private ImageView button(int glyph, CaptionTheme theme, boolean circle) {
        ImageView view = new ImageView(context);
        // The caption_* drawables are 18 dp with a 2.6667-unit stroke on Lucide's 24 grid:
        // 2 px at 160 dpi, 3 px at 240, 4 px at 320, so every line has the same width.
        // Centred at its own size, never scaled, so nothing resamples the stroke.
        view.setScaleType(ImageView.ScaleType.CENTER);
        android.graphics.drawable.Drawable drawable = context.getDrawable(glyph).mutate();
        drawable.setTint(theme.foreground);
        view.setImageDrawable(drawable);
        // Neutral tones deepen in light mode and lighten in dark mode; no accent fill.
        int rest = circle ? 0x0C : 0x00;
        StateListDrawable states = new StateListDrawable();
        states.addState(new int[]{android.R.attr.state_pressed},
                shape(theme, 0x24, circle));
        states.addState(new int[]{android.R.attr.state_hovered},
                shape(theme, 0x18, circle));
        states.addState(new int[]{}, shape(theme, rest, circle));
        view.setBackground(states);
        view.setClickable(true);
        view.setFocusable(true);
        return view;
    }

    private static int tone(int color, int alpha) { return (color & 0x00FFFFFF) | (alpha << 24); }

    private GradientDrawable shape(CaptionTheme theme, int alpha, boolean circle) {
        GradientDrawable d = new GradientDrawable();
        int tonalColor = theme.dark ? android.graphics.Color.WHITE : android.graphics.Color.BLACK;
        d.setColor(tone(tonalColor, alpha));
        d.setShape(circle ? GradientDrawable.OVAL : GradientDrawable.RECTANGLE);
        if (circle) d.setStroke(dp(1), tone(tonalColor, alpha + 0x10));
        else d.setCornerRadius(dp(5));
        return d;
    }

    private void invoke(int action) {
        try {
            if (action == 0) host.close(taskId);
            else if (action == 1) host.toggleMaximize(taskId);
            else host.minimize(taskId);
        } catch (RemoteException e) { android.util.Log.e(CaptionProviderService.TAG, "button callback failed taskId=" + taskId + " action=" + action, e); }
    }

    private void sendMenuAnchor(View v) {
        int[] loc = new int[2];
        v.getLocationOnScreen(loc);
        try { host.openAppMenu(taskId, new Rect(loc[0], loc[1], loc[0] + v.getWidth(), loc[1] + v.getHeight())); }
        catch (RemoteException e) { android.util.Log.e(CaptionProviderService.TAG, "openAppMenu failed taskId=" + taskId, e); }
    }

    private void sendRegions() {
        if (root == null) return;
        ArrayList<Rect> regions = new ArrayList<>();
        if (!transparent) {
            addRegion(regions, buttons);
            if (title != null && title.getParent() instanceof View) addRegion(regions, (View) title.getParent());
        }
        try { host.setInteractiveRegions(taskId, regions.toArray(new Rect[0])); }
        catch (RemoteException e) { android.util.Log.e(CaptionProviderService.TAG, "setInteractiveRegions failed taskId=" + taskId, e); }
    }

    private void addRegion(ArrayList<Rect> regions, View v) {
        int[] rootPos = new int[2];
        int[] viewPos = new int[2];
        root.getLocationOnScreen(rootPos);
        v.getLocationOnScreen(viewPos);
        regions.add(new Rect(viewPos[0] - rootPos[0], viewPos[1] - rootPos[1],
                viewPos[0] - rootPos[0] + v.getWidth(), viewPos[1] - rootPos[1] + v.getHeight()));
    }

    void release() {
        CaptionTheme.removeListener(preferenceListener);
        if (surfaceHost != null) {
            surfaceHost.release();
            surfaceHost = null;
        }
        root = null;
    }

    private static final int BUTTON_GAP_DP = 6;

    private int dp(int value) { return Math.round(value * context.getResources().getDisplayMetrics().density); }

    private static final class IBinderToken {
        final android.os.IBinder binder;
        IBinderToken(android.os.IBinder binder) { this.binder = binder; }
    }
}

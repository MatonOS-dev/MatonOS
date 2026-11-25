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
        int pad = Math.max(4, heightDp / 8);
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
            buttons.addView(button, new LinearLayout.LayoutParams(dp(Math.max(32, heightDp - 2 * pad)),
                    ViewGroup.LayoutParams.MATCH_PARENT));
            button.setOnClickListener(v -> invoke(action));
        }

        LinearLayout identity = new LinearLayout(context);
        identity.setOrientation(LinearLayout.HORIZONTAL);
        identity.setGravity(Gravity.CENTER_VERTICAL);
        int side = dp(Math.max(18, heightDp - 2 * pad));
        icon = new ImageView(context);
        icon.setScaleType(ImageView.ScaleType.FIT_CENTER);
        Bitmap bitmap = state.getParcelable("icon", Bitmap.class);
        if (bitmap != null) icon.setImageBitmap(bitmap);
        identity.addView(icon, new LinearLayout.LayoutParams(side, side));
        title = new TextView(context);
        title.setSingleLine(true);
        title.setEllipsize(android.text.TextUtils.TruncateAt.END);
        title.setText(state.getCharSequence("title", ""));
        title.setTextColor(theme.foreground);
        title.setTextSize(14);
        LinearLayout.LayoutParams titleLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        titleLp.leftMargin = dp(8);
        identity.addView(title, titleLp);
        FrameLayout.LayoutParams identityLp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.MATCH_PARENT,
                theme.startTitle ? Gravity.LEFT | Gravity.CENTER_VERTICAL : Gravity.CENTER);
        if (theme.startTitle) {
            int controlsWidth = dp(Math.max(32, heightDp - 2 * pad) * 3 + pad);
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
        view.setScaleType(ImageView.ScaleType.CENTER);
        android.graphics.drawable.Drawable drawable = context.getDrawable(glyph).mutate();
        drawable.setTint(theme.foreground);
        view.setImageDrawable(drawable);
        if (circle) {
            StateListDrawable states = new StateListDrawable();
            states.addState(new int[]{android.R.attr.state_pressed}, shape(theme.accent, true));
            states.addState(new int[]{android.R.attr.state_hovered}, shape(theme.accent, true));
            states.addState(new int[]{}, shape(android.graphics.Color.TRANSPARENT, true));
            view.setBackground(states);
        } else {
            StateListDrawable states = new StateListDrawable();
            states.addState(new int[]{android.R.attr.state_pressed}, shape(theme.accent, false));
            states.addState(new int[]{android.R.attr.state_hovered}, shape(theme.accent, false));
            states.addState(new int[]{}, shape(android.graphics.Color.TRANSPARENT, false));
            view.setBackground(states);
        }
        view.setClickable(true);
        view.setFocusable(true);
        return view;
    }

    private GradientDrawable shape(int color, boolean circle) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(circle ? dp(100) : dp(5));
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

    private int dp(int value) { return Math.round(value * context.getResources().getDisplayMetrics().density); }

    private static final class IBinderToken {
        final android.os.IBinder binder;
        IBinderToken(android.os.IBinder binder) { this.binder = binder; }
    }
}

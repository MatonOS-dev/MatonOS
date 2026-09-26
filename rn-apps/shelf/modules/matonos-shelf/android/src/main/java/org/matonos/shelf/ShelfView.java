package org.matonos.shelf;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.app.WallpaperColors;
import android.app.WallpaperManager;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.PopupMenu;
import android.widget.TextView;
import android.os.Handler;
import android.os.Looper;
import android.os.Build;

import org.matonos.systembridge.ISystemBridge;

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

/** Back, drawer, pinned/running icons, and recents in one persistent shelf. */
final class ShelfView extends LinearLayout {
    private static final int SCRIM_ALPHA = 238;
    private final ShelfService service;
    private final LinearLayout appButtons;
    private final List<View> controls = new ArrayList<>();
    private final View spacer;
    private final TextView compactHandle;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable collapse;
    private boolean homeVisible = true;
    private final Runnable refresh = new Runnable() {
        @Override public void run() { refreshApps(); handler.postDelayed(this, 4000); }
    };

    ShelfView(ShelfService owner) {
        super(owner); service = owner;
        collapse = () -> service.collapseShelfTemporarily();
        setOrientation(HORIZONTAL); setGravity(Gravity.CENTER_VERTICAL);
        setPadding(owner.dp(10), owner.dp(5), owner.dp(10), owner.dp(5));
        int foreground = foregroundForWallpaper(owner);
        int scrim = foreground == Color.WHITE
                ? Color.argb(SCRIM_ALPHA, 0, 0, 0) : Color.argb(SCRIM_ALPHA, 255, 255, 255);
        GradientDrawable bg = new GradientDrawable(); bg.setColor(scrim);
        setBackground(bg);
        Button backButton = addButton("‹", "Back", foreground, v -> service.navigate("back"));
        backButton.setOnLongClickListener(v -> service.navigate("back", true));
        boolean threeButton = ShelfService.isThreeButtonMode(owner);
        Button appsButton = threeButton ? null : addButton("▦", "Apps", foreground, v -> ShelfService.openPanel(service, "drawer"));
        Button settings = addButton("⚙", "Shelf settings", foreground, v -> {
            PopupMenu menu = new PopupMenu(service, v);
            menu.getMenu().add(threeButton ? "Use default shelf layout" : "Use 3-button navigation")
                    .setOnMenuItemClickListener(item -> {
                        ShelfService.setThreeButtonMode(service, !ShelfService.isThreeButtonMode(service));
                        return true;
                    });
            menu.show();
        });
        settings.setTextSize(16);
        if (appsButton != null) {
        appsButton.setTooltipText("Apps · hold for Home");
        appsButton.setOnLongClickListener(v -> {
            if (!service.navigate("home")) ShelfService.goHome(service);
            return true;
        });
        boolean[] longHomeSent = {false};
        appsButton.setOnKeyListener((v, keyCode, event) -> {
            if (keyCode != android.view.KeyEvent.KEYCODE_ENTER
                    && keyCode != android.view.KeyEvent.KEYCODE_SPACE) return false;
            if (event.getAction() == android.view.KeyEvent.ACTION_DOWN && event.getRepeatCount() > 0) {
                if (!longHomeSent[0]) { longHomeSent[0] = true; v.performLongClick(); }
                return true;
            }
            if (event.getAction() == android.view.KeyEvent.ACTION_UP && longHomeSent[0]) {
                longHomeSent[0] = false;
                return true;
            }
            return false;
        });
        }
        appButtons = new LinearLayout(owner); appButtons.setOrientation(HORIZONTAL);
        appButtons.setGravity(Gravity.CENTER_VERTICAL);
        controls.add(appButtons);
        addView(appButtons, new LayoutParams(LayoutParams.WRAP_CONTENT, -1));
        spacer = new Space(owner); addView(spacer, new LayoutParams(0, 1, 1));
        if (threeButton) {
            Space center = new Space(owner); addView(center, new LayoutParams(0, 1, 1));
            Button home = addButton("⌂", "Home", foreground, v -> {
                if (!service.navigate("home")) ShelfService.goHome(service);
            });
            home.setOnLongClickListener(v -> { ShelfService.openPanel(service, "drawer"); return true; });
            addView(new Space(owner), new LayoutParams(0, 1, 1));
        }
        addButton("▣", "Recents", foreground, v -> {
            if (!service.navigate("recents")) ShelfService.openPanel(service, "recents");
        });
        compactHandle = new TextView(owner);
        compactHandle.setText("⌃"); compactHandle.setTextColor(foreground); compactHandle.setTextSize(18);
        compactHandle.setGravity(Gravity.CENTER); compactHandle.setContentDescription("Expand shelf");
        compactHandle.setBackgroundResource(android.R.drawable.btn_default);
        compactHandle.setBackgroundTintList(android.content.res.ColorStateList.valueOf(
                foreground == Color.WHITE ? Color.argb(48, 255, 255, 255) : Color.argb(48, 0, 0, 0)));
        compactHandle.setOnClickListener(v -> service.expandShelfTemporarily());
        addView(compactHandle, new LayoutParams(service.dp(64), service.dp(28)));
        setExpanded(true);
        setOnClickListener(v -> {
            if (!homeVisible) { handler.removeCallbacks(collapse); service.expandShelfTemporarily(); }
        });
        setOnHoverListener((v, event) -> {
            if (!homeVisible && event.getAction() == android.view.MotionEvent.ACTION_HOVER_ENTER) {
                handler.removeCallbacks(collapse); service.expandShelfTemporarily();
            } else if (!homeVisible && event.getAction() == android.view.MotionEvent.ACTION_HOVER_EXIT) {
                handler.removeCallbacks(collapse); handler.postDelayed(collapse, 1200);
            }
            return false;
        });
    }

    private static int foregroundForWallpaper(Context context) {
        try {
            if (Build.VERSION.SDK_INT >= 27) {
                WallpaperColors colors = WallpaperManager.getInstance(context)
                        .getWallpaperColors(WallpaperManager.FLAG_SYSTEM);
                if (colors != null && colors.getPrimaryColor() != null
                        && colors.getPrimaryColor().toArgb() != Color.TRANSPARENT) {
                    return Color.luminance(colors.getPrimaryColor().toArgb()) > 0.5f
                            ? Color.BLACK : Color.WHITE;
                }
            }
        } catch (RuntimeException ignored) { }
        return Color.WHITE;
    }

    void setHomeVisible(boolean visible) {
        homeVisible = visible;
        handler.removeCallbacks(collapse);
        setExpanded(visible);
    }

    void setExpanded(boolean expanded) {
        for (View control : controls) control.setVisibility(expanded ? VISIBLE : GONE);
        spacer.setVisibility(VISIBLE);
        compactHandle.setVisibility(expanded ? GONE : VISIBLE);
        setPadding(getPaddingLeft(), expanded ? service.dp(5) : 0,
                getPaddingRight(), expanded ? service.dp(5) : 0);
    }

    private void refreshApps() {
        appButtons.removeAllViews();
        Set<String> pinned = service.getSharedPreferences("shelf", Context.MODE_PRIVATE)
                .getStringSet("pinned", java.util.Collections.emptySet());
        LinkedHashSet<String> packages = new LinkedHashSet<>(pinned);
        List<ShellTask> tasks = recentTasks();
        for (ShellTask task : tasks) {
            if (!task.packageName.equals(service.getPackageName())
                    && !task.packageName.equals("com.android.systemui")
                    && !task.packageName.equals("android")) packages.add(task.packageName);
        }
        int availableDp = (int) (service.getResources().getDisplayMetrics().widthPixels
                / service.getResources().getDisplayMetrics().density) - 176;
        int maxIcons = Math.max(0, availableDp / 48);
        int count = 0;
        for (String pkg : packages) {
            if (count++ >= maxIcons) break;
            addAppIcon(pkg, findTask(tasks, pkg));
        }
    }

    private List<ShellTask> recentTasks() {
        ISystemBridge bridge = service.systemBridge();
        if (bridge == null) return new ArrayList<>();
        try { return ShellTask.parse(bridge.getRecentTasks(32)); }
        catch (Exception ignored) { return new ArrayList<>(); }
    }

    private ShellTask findTask(List<ShellTask> tasks, String pkg) {
        for (ShellTask task : tasks) if (task.packageName.equals(pkg)) return task;
        return null;
    }

    private void addAppIcon(String packageName, ShellTask task) {
        try {
            ImageButton app = new ImageButton(service);
            app.setImageDrawable(service.getPackageManager().getApplicationIcon(packageName));
            app.setContentDescription(service.getPackageManager().getApplicationLabel(
                    service.getPackageManager().getApplicationInfo(packageName, 0)));
            app.setScaleType(android.widget.ImageView.ScaleType.FIT_CENTER);
            app.setBackgroundColor(Color.TRANSPARENT);
            app.setPadding(service.dp(7), service.dp(7), service.dp(7), service.dp(7));
            app.setOnClickListener(v -> activate(packageName, task));
            app.setOnLongClickListener(v -> { showTaskMenu(v, task); return task != null; });
            appButtons.addView(app, new LayoutParams(service.dp(48), service.dp(46)));
        } catch (Exception ignored) { }
    }

    private void activate(String packageName, ShellTask task) {
        ISystemBridge bridge = service.systemBridge();
        if (task != null && bridge != null) {
            try { if (bridge.moveTaskToFront(task.taskId)) return; }
            catch (Exception ignored) { }
        }
        Intent launch = service.getPackageManager().getLaunchIntentForPackage(packageName);
        if (launch != null) service.startShellActivity(launch);
    }

    private void showTaskMenu(View anchor, ShellTask task) {
        if (task == null) return;
        PopupMenu menu = new PopupMenu(service, anchor);
        menu.getMenu().add("Bring to front").setOnMenuItemClickListener(item -> {
            activate(task.packageName, task); return true;
        });
        menu.getMenu().add("Move to fullscreen").setOnMenuItemClickListener(item -> {
            ISystemBridge bridge = service.systemBridge();
            if (bridge != null) try {
                bridge.setTaskFullscreen(task.taskId);
                bridge.moveTaskToFront(task.taskId);
            } catch (Exception ignored) { }
            return true;
        });
        menu.show();
    }

    private Button addButton(String glyph, String description, int foreground, OnClickListener click) {
        Button b = new Button(service); b.setText(glyph); b.setTextColor(foreground); b.setTextSize(22);
        b.setContentDescription(description); b.setAllCaps(false); b.setOnClickListener(click);
        b.setMinWidth(service.dp(48)); b.setMinHeight(service.dp(44)); b.setPadding(0, 0, 0, 0);
        b.setBackgroundTintList(android.content.res.ColorStateList.valueOf(
                foreground == Color.WHITE ? Color.argb(36, 255, 255, 255) : Color.argb(36, 0, 0, 0)));
        controls.add(b);
        addView(b, new LayoutParams(service.dp(52), service.dp(46)));
        return b;
    }

    @Override protected void onDetachedFromWindow() {
        handler.removeCallbacks(refresh);
        super.onDetachedFromWindow();
    }

    @Override protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        refreshApps();
        handler.removeCallbacks(refresh);
        handler.postDelayed(refresh, 1000);
    }

    private static final class Space extends View { Space(Context c) { super(c); } }
}

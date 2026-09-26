package org.matonos.shell;

import android.content.Context;
import android.content.Intent;
import android.content.pm.ResolveInfo;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.widget.EditText;
import android.widget.GridLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;

/** Minimal native desktop shown if the React home surface cannot start. */
final class HomeFallbackView extends LinearLayout {
    private final Context context;
    private final GridLayout apps;
    private final List<ResolveInfo> launchables = new ArrayList<>();
    private final int columns;
    private String filter = "";

    HomeFallbackView(Context context) {
        super(context);
        this.context = context;
        setOrientation(VERTICAL);
        setPadding(dp(24), dp(24), dp(24), dp(76));
        setFocusable(true);

        TextView title = new TextView(context);
        title.setText("MatonOS");
        title.setTextColor(Color.WHITE);
        title.setTextSize(28);
        title.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        title.setShadowLayer(3, 1, 2, Color.BLACK);
        addView(title, new LayoutParams(LayoutParams.WRAP_CONTENT, dp(44)));

        EditText search = new EditText(context);
        search.setSingleLine(true);
        search.setHint("Search apps");
        search.setTextColor(Color.WHITE);
        search.setHintTextColor(0xffdddddd);
        search.setTextSize(16);
        GradientDrawable searchBackground = new GradientDrawable();
        searchBackground.setColor(0x99000000);
        searchBackground.setCornerRadius(dp(12));
        search.setBackground(searchBackground);
        search.setPadding(dp(14), 0, dp(14), 0);
        addView(search, new LayoutParams(LayoutParams.MATCH_PARENT, dp(48)));

        ScrollView scroll = new ScrollView(context);
        scroll.setFillViewport(false);
        apps = new GridLayout(context);
        apps.setColumnCount(6);
        apps.setPadding(0, dp(14), 0, 0);
        scroll.addView(apps, new ScrollView.LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT));
        addView(scroll, new LayoutParams(LayoutParams.MATCH_PARENT, 0, 1));

        int widthDp = (int) (context.getResources().getDisplayMetrics().widthPixels
                / context.getResources().getDisplayMetrics().density);
        columns = Math.max(3, Math.min(9, widthDp / 144));
        apps.setColumnCount(columns);
        Intent query = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
        launchables.addAll(context.getPackageManager().queryIntentActivities(query, 0));
        launchables.removeIf(item -> item.activityInfo.packageName.equals(context.getPackageName())
                || item.activityInfo.packageName.equals("com.android.systemui"));
        launchables.sort(Comparator.comparing(item -> item.loadLabel(context.getPackageManager())
                .toString().toLowerCase(Locale.ROOT)));
        renderApps();
        search.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) { }
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {
                filter = s.toString().trim().toLowerCase(Locale.ROOT);
                renderApps();
            }
            @Override public void afterTextChanged(Editable s) { }
        });
    }

    private void renderApps() {
        apps.removeAllViews();
        int index = 0;
        for (ResolveInfo item : launchables) {
            String label = item.loadLabel(context.getPackageManager()).toString();
            if (!label.toLowerCase(Locale.ROOT).contains(filter)) continue;
            LinearLayout tile = new LinearLayout(context);
            tile.setOrientation(VERTICAL);
            tile.setGravity(Gravity.CENTER);
            tile.setPadding(dp(4), dp(6), dp(4), dp(6));
            GradientDrawable background = new GradientDrawable();
            background.setColor(0x55000000);
            background.setCornerRadius(dp(12));
            tile.setBackground(background);
            tile.setFocusable(true);
            ImageView icon = new ImageView(context);
            icon.setImageDrawable(item.loadIcon(context.getPackageManager()));
            tile.addView(icon, new LayoutParams(dp(44), dp(44)));
            TextView name = new TextView(context);
            name.setText(label);
            name.setTextColor(Color.WHITE);
            name.setTextSize(12);
            name.setGravity(Gravity.CENTER);
            name.setMaxLines(2);
            name.setShadowLayer(2, 1, 1, Color.BLACK);
            tile.addView(name, new LayoutParams(LayoutParams.MATCH_PARENT, dp(34)));
            tile.setContentDescription(label);
            tile.setOnClickListener(view -> {
                Intent launch = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                        .setClassName(item.activityInfo.packageName, item.activityInfo.name)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                try { context.startActivity(launch); }
                catch (RuntimeException error) { android.util.Log.e("MatonOSShell", "Fallback launch failed: " + label, error); }
            });
            GridLayout.LayoutParams params = new GridLayout.LayoutParams(
                    GridLayout.spec(GridLayout.UNDEFINED), GridLayout.spec(index % columns, 1, 1f));
            params.width = 0;
            params.height = dp(92);
            params.setMargins(dp(4), dp(4), dp(4), dp(4));
            apps.addView(tile, params);
            index++;
        }
    }

    private int dp(int value) {
        return (int) (value * getResources().getDisplayMetrics().density + 0.5f);
    }
}

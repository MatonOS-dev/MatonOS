package org.matonos.pointertest;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.os.Bundle;
import android.util.Log;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

/** Click to capture the pointer (Android pointer capture); Esc releases. Shows raw relative motion. */
public class PointerActivity extends Activity {
    private static final String TAG = "MatonPointerTest";

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        CaptureView view = new CaptureView(this);
        setContentView(view);
        view.requestFocus();
    }

    static final class CaptureView extends View {
        private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint dot = new Paint(Paint.ANTI_ALIAS_FLAG);
        private float x, y, sumX, sumY, lastDx, lastDy, scroll;
        private int buttons;
        private long events;
        private String status = "Click to capture the pointer. Esc releases.";

        CaptureView(Context context) {
            super(context);
            setFocusable(true);
            setFocusableInTouchMode(true);
            text.setTextSize(28);
            dot.setColor(Color.rgb(0xb3, 0x26, 0x1e));
        }

        @Override public boolean onTouchEvent(MotionEvent event) {
            if (event.getActionMasked() == MotionEvent.ACTION_DOWN && !hasPointerCapture()) {
                x = getWidth() / 2f; y = getHeight() / 2f;
                requestPointerCapture();
                status = "Requested capture…";
                invalidate();
            }
            return true;
        }

        @Override public void onPointerCaptureChange(boolean captured) {
            status = captured ? "CAPTURED — move the mouse, click, scroll. Esc releases." : "Released. Click to capture again.";
            Log.i(TAG, "pointer capture " + captured);
            invalidate();
        }

        @Override public boolean onCapturedPointerEvent(MotionEvent event) {
            events++;
            buttons = event.getButtonState();
            if (event.getActionMasked() == MotionEvent.ACTION_SCROLL) scroll += event.getAxisValue(MotionEvent.AXIS_VSCROLL);
            for (int h = 0; h < event.getHistorySize(); h++) move(event.getHistoricalX(h), event.getHistoricalY(h));
            move(event.getX(), event.getY());
            invalidate();
            return true;
        }

        private void move(float dx, float dy) {
            lastDx = dx; lastDy = dy; sumX += dx; sumY += dy;
            x = Math.max(0, Math.min(getWidth(), x + dx));
            y = Math.max(0, Math.min(getHeight(), y + dy));
        }

        @Override public boolean onKeyDown(int keyCode, KeyEvent event) {
            if (keyCode == KeyEvent.KEYCODE_ESCAPE && hasPointerCapture()) { releasePointerCapture(); return true; }
            return super.onKeyDown(keyCode, event);
        }

        @Override protected void onDraw(Canvas canvas) {
            canvas.drawColor(hasPointerCapture() ? Color.rgb(0xe8, 0xf5, 0xe9) : Color.WHITE);
            float line = 40;
            canvas.drawText(status, 24, line, text);
            canvas.drawText(String.format("events %d   last dx %.1f dy %.1f   total dx %.0f dy %.0f", events, lastDx, lastDy, sumX, sumY), 24, line * 2, text);
            canvas.drawText(String.format("buttons 0x%x (1 left, 2 right, 4 middle)   scroll %.0f", buttons, scroll), 24, line * 3, text);
            if (hasPointerCapture()) canvas.drawCircle(x, y, 12, dot);
        }
    }
}

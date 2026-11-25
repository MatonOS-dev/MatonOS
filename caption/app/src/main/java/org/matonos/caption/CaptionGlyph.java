package org.matonos.caption;

import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;

/** Window control glyph drawn as anti-aliased strokes (font glyphs render unevenly). */
final class CaptionGlyph extends Drawable {
    static final int CLOSE = 0, MAXIMIZE = 1, RESTORE = 2, MINIMIZE = 3;

    private final int kind;
    private final float size;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

    /** @param size glyph edge in px; @param stroke line width in px */
    CaptionGlyph(int kind, int color, float size, float stroke) {
        this.kind = kind;
        this.size = size;
        paint.setColor(color);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(stroke);
        paint.setStrokeCap(Paint.Cap.ROUND);
        paint.setStrokeJoin(Paint.Join.ROUND);
    }

    @Override
    public void draw(Canvas canvas) {
        Rect b = getBounds();
        float l = b.exactCenterX() - size / 2, t = b.exactCenterY() - size / 2;
        float r = l + size, bottom = t + size;
        switch (kind) {
            case CLOSE:
                canvas.drawLine(l, t, r, bottom, paint);
                canvas.drawLine(l, bottom, r, t, paint);
                break;
            case MAXIMIZE:
                canvas.drawRoundRect(l, t, r, bottom, size / 8, size / 8, paint);
                break;
            case RESTORE: {
                float o = size / 4;
                canvas.drawRoundRect(l, t + o, r - o, bottom, size / 10, size / 10, paint);
                canvas.drawLine(l + o, t + o, l + o, t, paint);
                canvas.drawLine(l + o, t, r, t, paint);
                canvas.drawLine(r, t, r, bottom - o, paint);
                canvas.drawLine(r, bottom - o, r - o, bottom - o, paint);
                break;
            }
            default:
                canvas.drawLine(l, b.exactCenterY(), r, b.exactCenterY(), paint);
        }
    }

    @Override public void setAlpha(int alpha) { paint.setAlpha(alpha); invalidateSelf(); }
    @Override public void setColorFilter(ColorFilter filter) { paint.setColorFilter(filter); invalidateSelf(); }
    @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
}

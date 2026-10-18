package com.tt2kgames.Borderlands2.touch;

import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import static com.tt2kgames.Borderlands2.touch.TouchContract.*;

/**
 * Vector icons for the touch buttons (BUTTON ICONS), drawn in a 24x24 design
 * box centred on the button. Plain strokes and fills only: no bitmaps, no
 * language, crisp at every control size.
 */
public final class TouchIcons {
    private TouchIcons() {}
    // UI thread only; created on the first draw so host checks of has() need no graphics.
    private static Path path;
    private static RectF oval;

    /** True when the action has an icon; then it is drawn, else nothing is. */
    public static boolean has(int action) {
        switch (action) {
            case FIRE: case AIM: case JUMP: case CROUCH: case RELOAD: case USE: case MELEE: case GRENADE:
            case SKILL: case NEXT_WEAPON: case PREV_WEAPON: case INVENTORY: case MAP: case PAUSE: case BACK:
            case CONFIRM: case SKIP: case VEHICLE_BRAKE: case VEHICLE_BOOST: case VEHICLE_HORN:
            case UI_UP: case UI_DOWN: case UI_LEFT: case UI_RIGHT: case CHAT: case EXIT_VEHICLE:
            case SWITCH_SEATS: case DRIVER_FIRE_TURRET: case LOOK_BACK: case SUICIDE:
            case TouchLayout.MORE_BUTTON: case -3:
                return true;
            default: return false;
        }
    }

    /**
     * Draws the icon of action at (cx,cy), size being the side of the 24-unit
     * design box. The paint's style and stroke are restored afterwards.
     */
    public static boolean draw(Canvas canvas, Paint paint, int action, float cx, float cy, float size, int color, int alpha) {
        if (!has(action) || !(size > 0)) return false;
        if (path == null) { path = new Path(); oval = new RectF(); }
        Paint.Style style = paint.getStyle();
        float stroke = paint.getStrokeWidth();
        Paint.Cap cap = paint.getStrokeCap();
        Paint.Join join = paint.getStrokeJoin();
        float s = size / 24f;
        canvas.save();
        canvas.translate(cx - 12 * s, cy - 12 * s);
        canvas.scale(s, s);
        paint.setColor(color); paint.setAlpha(alpha);
        paint.setStrokeWidth(2f); paint.setStrokeCap(Paint.Cap.ROUND); paint.setStrokeJoin(Paint.Join.ROUND);
        paint.setStyle(Paint.Style.STROKE);
        switch (action) {
            case FIRE: bullet(canvas, paint); break;
            case AIM: aim(canvas, paint); break;
            case JUMP: arrowUp(canvas, paint, 12, 19, 5); line(canvas, paint, 5, 22, 19, 22); break;
            case CROUCH: arrowDown(canvas, paint, 12, 3, 15); line(canvas, paint, 5, 20, 19, 20); break;
            case RELOAD: reload(canvas, paint); break;
            case USE: interact(canvas, paint); break;
            case MELEE: knife(canvas, paint); break;
            case GRENADE: grenade(canvas, paint); break;
            case SKILL: bolt(canvas, paint); break;
            case NEXT_WEAPON: swap(canvas, paint); break;
            case PREV_WEAPON: previous(canvas, paint); break;
            case INVENTORY: backpack(canvas, paint); break;
            case MAP: map(canvas, paint); break;
            case CHAT: chat(canvas, paint); break;
            case SUICIDE: flag(canvas, paint); break;
            case PAUSE: fill(paint); canvas.drawRect(6.5f, 5, 10, 19, paint); canvas.drawRect(14, 5, 17.5f, 19, paint); break;
            case TouchLayout.MORE_BUTTON: fill(paint); for (int i = 0; i < 3; ++i) canvas.drawCircle(5.5f + 6.5f * i, 12, 2.2f, paint); break;
            case -3: case CONFIRM: check(canvas, paint); break;
            case BACK: case UI_LEFT: arrowLeft(canvas, paint); break;
            case UI_RIGHT: canvas.save(); canvas.rotate(180, 12, 12); arrowLeft(canvas, paint); canvas.restore(); break;
            case UI_UP: arrowUp(canvas, paint, 12, 20, 4); break;
            case UI_DOWN: arrowDown(canvas, paint, 12, 4, 20); break;
            case SKIP: skip(canvas, paint); break;
            case VEHICLE_BOOST: flame(canvas, paint); break;
            case VEHICLE_BRAKE: brake(canvas, paint); break;
            case EXIT_VEHICLE: exit(canvas, paint); break;
            case VEHICLE_HORN: horn(canvas, paint); break;
            case SWITCH_SEATS: seats(canvas, paint); break;
            case LOOK_BACK: eye(canvas, paint); break;
            case DRIVER_FIRE_TURRET: turret(canvas, paint); break;
            default: break;
        }
        canvas.restore();
        paint.setStyle(style); paint.setStrokeWidth(stroke); paint.setStrokeCap(cap); paint.setStrokeJoin(join);
        return true;
    }
    private static void fill(Paint paint) { paint.setStyle(Paint.Style.FILL); }
    private static void line(Canvas c, Paint p, float x0, float y0, float x1, float y1) { c.drawLine(x0, y0, x1, y1, p); }
    private static void bullet(Canvas c, Paint p) {
        fill(p);
        path.reset();
        path.moveTo(8.5f, 21); path.lineTo(8.5f, 10.5f);
        path.cubicTo(8.5f, 6.5f, 10, 3.5f, 12, 2);
        path.cubicTo(14, 3.5f, 15.5f, 6.5f, 15.5f, 10.5f);
        path.lineTo(15.5f, 21); path.close();
        c.drawPath(path, p);
        // The case mouth: a dark notch would vanish on black, so a gap instead.
        p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(1.4f);
        int color = p.getColor(), alpha = p.getAlpha();
        p.setColor(0xFF000000); p.setAlpha(alpha);
        c.drawLine(8.5f, 16.5f, 15.5f, 16.5f, p);
        p.setColor(color); p.setAlpha(alpha); p.setStrokeWidth(2f);
    }
    private static void aim(Canvas c, Paint p) {
        c.drawCircle(12, 12, 7, p);
        line(c, p, 12, 1.5f, 12, 6); line(c, p, 12, 18, 12, 22.5f);
        line(c, p, 1.5f, 12, 6, 12); line(c, p, 18, 12, 22.5f, 12);
        fill(p); c.drawCircle(12, 12, 1.6f, p);
    }
    private static void arrowUp(Canvas c, Paint p, float x, float bottom, float top) {
        line(c, p, x, bottom, x, top);
        path.reset(); path.moveTo(x - 6, top + 6); path.lineTo(x, top); path.lineTo(x + 6, top + 6);
        c.drawPath(path, p);
    }
    private static void arrowDown(Canvas c, Paint p, float x, float top, float bottom) {
        line(c, p, x, top, x, bottom);
        path.reset(); path.moveTo(x - 6, bottom - 6); path.lineTo(x, bottom); path.lineTo(x + 6, bottom - 6);
        c.drawPath(path, p);
    }
    private static void arrowLeft(Canvas c, Paint p) {
        line(c, p, 20, 12, 5, 12);
        path.reset(); path.moveTo(11, 6); path.lineTo(5, 12); path.lineTo(11, 18);
        c.drawPath(path, p);
    }
    private static void reload(Canvas c, Paint p) {
        oval.set(4.5f, 4.5f, 19.5f, 19.5f);
        c.drawArc(oval, -30, 290, false, p);
        // Arrowhead at the arc's open end (angle -30 degrees on the ring).
        path.reset(); path.moveTo(20.6f, 3.4f); path.lineTo(19.3f, 8.6f); path.lineTo(14.2f, 7.3f);
        c.drawPath(path, p);
    }
    /** An open hand, the usual "interact" sign (the game's own pickup prompt shows one too). */
    private static void interact(Canvas c, Paint p) {
        p.setStrokeWidth(2.6f);
        line(c, p, 9, 13, 9, 5.5f);
        line(c, p, 12.2f, 13, 12.2f, 3.5f);
        line(c, p, 15.4f, 13, 15.4f, 4.5f);
        line(c, p, 18.4f, 14, 18.4f, 7.5f);
        line(c, p, 8.5f, 16.5f, 4.5f, 12);
        p.setStrokeWidth(2f);
        fill(p);
        oval.set(7.5f, 11.5f, 19.8f, 22); c.drawRoundRect(oval, 4.5f, 4.5f, p);
    }
    private static void knife(Canvas c, Paint p) {
        fill(p);
        path.reset();
        path.moveTo(9, 15); path.lineTo(20.5f, 3.5f); path.lineTo(17.5f, 11); path.lineTo(11, 17); path.close();
        c.drawPath(path, p);
        p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(2.6f);
        line(c, p, 3.5f, 20.5f, 8, 16);
        p.setStrokeWidth(2f);
        line(c, p, 6.5f, 13.5f, 10.5f, 17.5f);
    }
    private static void grenade(Canvas c, Paint p) {
        c.drawCircle(12, 14.5f, 6.5f, p);
        path.reset(); path.moveTo(9.5f, 8.5f); path.lineTo(9.5f, 5.5f); path.lineTo(14.5f, 5.5f); path.lineTo(14.5f, 8.5f);
        c.drawPath(path, p);
        path.reset(); path.moveTo(14.5f, 6); path.lineTo(18.5f, 8.5f); path.lineTo(18, 12.5f);
        c.drawPath(path, p);
        c.drawCircle(7, 5, 2, p);
    }
    private static void bolt(Canvas c, Paint p) {
        fill(p);
        path.reset();
        path.moveTo(13.5f, 1.5f); path.lineTo(5, 13.5f); path.lineTo(11, 13.5f); path.lineTo(9.5f, 22.5f);
        path.lineTo(19, 9.5f); path.lineTo(13, 9.5f); path.close();
        c.drawPath(path, p);
    }
    private static void swap(Canvas c, Paint p) {
        line(c, p, 4, 8, 19, 8);
        path.reset(); path.moveTo(15, 4); path.lineTo(19, 8); path.lineTo(15, 12); c.drawPath(path, p);
        line(c, p, 20, 16, 5, 16);
        path.reset(); path.moveTo(9, 12); path.lineTo(5, 16); path.lineTo(9, 20); c.drawPath(path, p);
    }
    private static void previous(Canvas c, Paint p) {
        path.reset(); path.moveTo(8.5f, 3.5f); path.lineTo(4, 8); path.lineTo(8.5f, 12.5f); c.drawPath(path, p);
        path.reset(); path.moveTo(4, 8); path.lineTo(14, 8);
        oval.set(8, 8, 20, 20); path.arcTo(oval, -90, 180);
        path.lineTo(8, 20);
        c.drawPath(path, p);
    }
    private static void backpack(Canvas c, Paint p) {
        oval.set(5, 7, 19, 21.5f); c.drawRoundRect(oval, 3.5f, 3.5f, p);
        path.reset(); path.moveTo(9, 7); path.lineTo(9, 4.5f); path.lineTo(15, 4.5f); path.lineTo(15, 7); c.drawPath(path, p);
        oval.set(8.5f, 13, 15.5f, 18.5f); c.drawRoundRect(oval, 1.5f, 1.5f, p);
    }
    private static void map(Canvas c, Paint p) {
        path.reset();
        path.moveTo(3, 6); path.lineTo(9, 3.5f); path.lineTo(15, 6); path.lineTo(21, 3.5f);
        path.lineTo(21, 18); path.lineTo(15, 20.5f); path.lineTo(9, 18); path.lineTo(3, 20.5f); path.close();
        c.drawPath(path, p);
        line(c, p, 9, 3.5f, 9, 18); line(c, p, 15, 6, 15, 20.5f);
    }
    private static void chat(Canvas c, Paint p) {
        path.reset();
        oval.set(3, 4, 21, 16);
        path.addRoundRect(oval, 3.5f, 3.5f, Path.Direction.CW);
        c.drawPath(path, p);
        path.reset(); path.moveTo(8, 16); path.lineTo(7, 21); path.lineTo(12.5f, 16); c.drawPath(path, p);
    }
    private static void flag(Canvas c, Paint p) {
        line(c, p, 6, 3, 6, 21.5f);
        fill(p);
        path.reset(); path.moveTo(6, 4); path.lineTo(19, 4); path.lineTo(15.5f, 8.5f); path.lineTo(19, 13); path.lineTo(6, 13); path.close();
        c.drawPath(path, p);
    }
    private static void check(Canvas c, Paint p) {
        p.setStrokeWidth(2.6f);
        path.reset(); path.moveTo(4.5f, 12.5f); path.lineTo(9.5f, 17.5f); path.lineTo(19.5f, 6.5f);
        c.drawPath(path, p);
        p.setStrokeWidth(2f);
    }
    private static void skip(Canvas c, Paint p) {
        fill(p);
        path.reset(); path.moveTo(3, 5.5f); path.lineTo(11, 12); path.lineTo(3, 18.5f); path.close();
        path.moveTo(11, 5.5f); path.lineTo(19, 12); path.lineTo(11, 18.5f); path.close();
        c.drawPath(path, p);
        c.drawRect(19, 5.5f, 21.5f, 18.5f, p);
    }
    private static void flame(Canvas c, Paint p) {
        path.reset();
        path.moveTo(12, 2);
        path.cubicTo(16.5f, 6.5f, 19, 10, 19, 14.5f);
        path.cubicTo(19, 18.5f, 15.8f, 22, 12, 22);
        path.cubicTo(8.2f, 22, 5, 18.5f, 5, 14.5f);
        path.cubicTo(5, 11.5f, 6.5f, 9.5f, 8.5f, 7.5f);
        path.cubicTo(8.5f, 10.5f, 9.8f, 12.5f, 12, 13);
        path.cubicTo(11.5f, 9, 11, 5.5f, 12, 2);
        path.close();
        c.drawPath(path, p);
    }
    private static void brake(Canvas c, Paint p) {
        c.drawCircle(12, 12, 6, p);
        oval.set(2, 2, 22, 22);
        c.drawArc(oval, 135, 90, false, p);
        c.drawArc(oval, -45, 90, false, p);
        line(c, p, 12, 8.5f, 12, 12.5f);
        fill(p); c.drawCircle(12, 15.3f, 1.1f, p);
    }
    private static void exit(Canvas c, Paint p) {
        path.reset(); path.moveTo(13, 7); path.lineTo(13, 3.5f); path.lineTo(4, 3.5f); path.lineTo(4, 20.5f);
        path.lineTo(13, 20.5f); path.lineTo(13, 17); c.drawPath(path, p);
        line(c, p, 9, 12, 21, 12);
        path.reset(); path.moveTo(17, 8); path.lineTo(21, 12); path.lineTo(17, 16); c.drawPath(path, p);
    }
    private static void horn(Canvas c, Paint p) {
        fill(p);
        path.reset(); path.moveTo(3.5f, 9); path.lineTo(7.5f, 9); path.lineTo(12.5f, 5); path.lineTo(12.5f, 19);
        path.lineTo(7.5f, 15); path.lineTo(3.5f, 15); path.close();
        c.drawPath(path, p);
        p.setStyle(Paint.Style.STROKE);
        oval.set(9.5f, 8, 17.5f, 16); c.drawArc(oval, -50, 100, false, p);
        oval.set(7, 4.5f, 21.5f, 19.5f); c.drawArc(oval, -50, 100, false, p);
    }
    private static void seats(Canvas c, Paint p) {
        arrowUp(c, p, 8, 20, 4);
        arrowDown(c, p, 16, 4, 20);
    }
    private static void eye(Canvas c, Paint p) {
        path.reset(); path.moveTo(2, 12); path.quadTo(12, 2.5f, 22, 12); path.quadTo(12, 21.5f, 2, 12); path.close();
        c.drawPath(path, p);
        fill(p); c.drawCircle(12, 12, 3, p);
    }
    private static void turret(Canvas c, Paint p) {
        c.drawCircle(12, 12, 8.5f, p);
        c.drawCircle(12, 12, 4, p);
        fill(p); c.drawCircle(12, 12, 1.5f, p);
    }
}

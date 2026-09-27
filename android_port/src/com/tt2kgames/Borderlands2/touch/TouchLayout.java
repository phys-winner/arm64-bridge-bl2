package com.tt2kgames.Borderlands2.touch;

import java.util.List;

/** Deterministic nearest free placement, independent of Android for geometry tests. */
public final class TouchLayout {
    private TouchLayout() {}
    public static boolean overlaps(float[] a,float[] b,float gap) {
        return a[0]<b[2]+gap && a[2]+gap>b[0] && a[1]<b[3]+gap && a[3]+gap>b[1];
    }
    private static boolean fits(float[] r,List<float[]> occupied,float gap) {
        for(float[] other:occupied) if(overlaps(r,other,gap)) return false;
        return true;
    }
    public static float[] place(float cx,float cy,float size,float left,float top,
            float right,float bottom,float step,float gap,List<float[]> occupied) {
        if(right-left<size || bottom-top<size) return null;
        cx=Math.max(left+size/2,Math.min(right-size/2,cx));
        cy=Math.max(top+size/2,Math.min(bottom-size/2,cy));
        float[] preferred={cx-size/2,cy-size/2,cx+size/2,cy+size/2};
        if(fits(preferred,occupied,gap)) return preferred;
        float[] best=null; float score=Float.POSITIVE_INFINITY;
        for(float y=top;y<=bottom-size+.01f;y+=step) for(float x=left;x<=right-size+.01f;x+=step) {
            float distance=(x+size/2-cx)*(x+size/2-cx)+(y+size/2-cy)*(y+size/2-cy);
            if(distance>=score) continue;
            float[] candidate={x,y,x+size,y+size};
            if(fits(candidate,occupied,gap)) { best=candidate; score=distance; }
        }
        return best;
    }
}

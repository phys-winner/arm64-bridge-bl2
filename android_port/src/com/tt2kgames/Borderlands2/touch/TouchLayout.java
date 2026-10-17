package com.tt2kgames.Borderlands2.touch;

import java.util.List;
import static com.tt2kgames.Borderlands2.touch.TouchContract.*;

/** Deterministic nearest free placement, independent of Android for geometry tests. */
public final class TouchLayout {
    private TouchLayout() {}
    /** The gameplay More button (NativeTouchControls' own id; no native action). */
    public static final int MORE_BUTTON = -2;
    /** Where the layout editor puts an on-foot action: a button, a More sector, nowhere. */
    public static final int PLACE_SCREEN = 0, PLACE_MORE = 1, PLACE_HIDDEN = 2;
    /** Every on-foot control the editor's button panel lists, in panel order. */
    public static final int[] EDITABLE = {FIRE, AIM, GRENADE, JUMP, RELOAD, USE, MELEE, CROUCH, SKILL,
            NEXT_WEAPON, PREV_WEAPON, INVENTORY, MAP, CHAT, SUICIDE, PAUSE, MORE_BUTTON};
    /** More's sectors keep this order whatever the player moved in or out. */
    public static final int[] RADIAL_ORDER = {NEXT_WEAPON, SKILL, CROUCH, MELEE, GRENADE, INVENTORY, MAP,
            PREV_WEAPON, RELOAD, SUICIDE, CHAT};
    /** The shipped layout: six permanent buttons, contextual Reload and Action, the rest in More. */
    public static int defaultPlacement(int action) {
        switch (action) {
            case FIRE: case AIM: case GRENADE: case JUMP: case RELOAD: case USE: case PAUSE: case MORE_BUTTON:
                return PLACE_SCREEN;
            case MELEE: case CROUCH: case SKILL: case NEXT_WEAPON: case PREV_WEAPON: case INVENTORY:
            case MAP: case CHAT: case SUICIDE:
                return PLACE_MORE;
            default: return PLACE_HIDDEN;
        }
    }
    /** Held controls (Fire, Aim, Jump, Action) and the two utilities never become a sector. */
    public static boolean placementAllowed(int action, int place) {
        boolean editable = false;
        for (int a : EDITABLE) editable |= a == action;
        if (!editable || place < PLACE_SCREEN || place > PLACE_HIDDEN) return false;
        if (place != PLACE_MORE) return true;
        for (int a : RADIAL_ORDER) if (a == action) return true;
        return false;
    }
    /** Pause and the menu buttons the editor adds share the top row. */
    public static boolean topRow(int action) {
        return action == PAUSE || action == INVENTORY || action == MAP || action == CHAT;
    }
    /** Utilities (More, Pause, the menu buttons) keep a fixed size and stay unmirrored in left-hand mode. */
    public static boolean utility(int action) { return action < 0 || topRow(action); }
    /** Remove badge of a round button: on its rim at one o'clock-thirty. {x,y,radius} */
    public static float[] removeBadge(float cx,float cy,float radius,float density) {
        float offset=radius*.72f;
        return new float[]{cx+offset,cy-offset,10*density};
    }
    /**
     * Editor toolbar pills {left,top,right,bottom} centred at the top of the safe
     * area for the given widths, 8dp apart. Null when they do not fit.
     */
    public static float[][] toolbar(float left,float top,float right,float[] widths,float density) {
        if (widths == null || widths.length == 0 || !(right > left) || !Float.isFinite(density) || density <= 0) return null;
        float gap=8*density,total=-gap;
        for (float w:widths) { if (!(w > 0) || !Float.isFinite(w)) return null; total+=w+gap; }
        if (total > right-left-16*density) return null;
        float x=(left+right-total)/2,y=top+8*density,h=40*density;
        float[][] pills=new float[widths.length][];
        for (int i=0;i<widths.length;++i) { pills[i]=new float[]{x,y,x+widths[i],y+h}; x+=widths[i]+gap; }
        return pills;
    }
    /**
     * The button panel: a centred card with a title row and a grid of cells, as
     * many columns of 236dp as fit (2 or 3) and rows that shrink from 44dp to
     * 32dp to fit. Index 0 is the card, 1 the close pill, 2.. the cells. Null without room.
     */
    public static float[][] palette(float left,float top,float right,float bottom,int count,float density) {
        if (count <= 0 || !Float.isFinite(density) || density <= 0 || !(right > left) || !(bottom > top)) return null;
        float margin=12*density,cell=236*density,gap=6*density,title=44*density;
        int columns=(int)Math.min(3,Math.floor((right-left-2*margin+gap)/(cell+gap)));
        if (columns < 2) return null;
        int rows=(count+columns-1)/columns;
        float row=Math.min(44*density,(bottom-top-2*margin-title-margin-(rows-1)*gap)/rows);
        if (row < 32*density) return null;
        float width=columns*cell+(columns-1)*gap+2*margin,height=title+rows*row+(rows-1)*gap+2*margin;
        float x=(left+right-width)/2,y=(top+bottom-height)/2;
        float[][] out=new float[count+2][];
        out[0]=new float[]{x,y,x+width,y+height};
        out[1]=new float[]{x+width-margin-96*density,y+margin,x+width-margin,y+margin+32*density};
        for (int i=0;i<count;++i) {
            float cx=x+margin+(i%columns)*(cell+gap),cy=y+margin+title+(i/columns)*(row+gap);
            out[i+2]=new float[]{cx,cy,cx+cell,cy+row};
        }
        return out;
    }
    /** The three placement segments at the right of a panel cell: Screen, More, None. */
    public static float[] segment(float[] cell,int place,float density) {
        float width=44*density,right=cell[2]-4*density,top=cell[1]+4*density,bottom=cell[3]-4*density;
        float x=right-(3-place)*width;
        return new float[]{x+1*density,top,x+width-1*density,bottom};
    }
    public static boolean circleContains(float x,float y,float cx,float cy,float radius,float slop) {
        return Float.isFinite(x) && Float.isFinite(y) && Float.isFinite(radius) && radius>0
                && Float.isFinite(slop) && slop>=0 && Math.hypot(x-cx,y-cy)<=radius+slop;
    }
    public static float gameplayHitSlop(int action,float density) { return (action==USE?14:2)*density; }
    /** Reference FPS layout in safe-area coordinates; utility controls stay on the left. */
    public static float[] gameplayAnchor(int action) {
        switch (action) {
            case FIRE: return new float[]{.858f,.784f};
            case AIM: return new float[]{.957f,.934f};
            // A vehicle seat replaces the cluster: Boost, Brake and Exit take
            // the Grenade, Jump and Use places.
            case GRENADE: case VEHICLE_BOOST: return new float[]{.957f,.784f};
            case JUMP: case VEHICLE_BRAKE: return new float[]{.858f,.944f};
            case USE: case EXIT_VEHICLE: return new float[]{.75f,.784f};
            case RELOAD: return new float[]{.75f,.944f};
            case PAUSE: return new float[]{.035f,.055f};
            // Buttons the editor adds: a row above the cluster, weapons left of
            // it, menus beside Pause; placement then finds the nearest free spot.
            case SKILL: return new float[]{.858f,.62f};
            case CROUCH: return new float[]{.957f,.62f};
            case MELEE: return new float[]{.75f,.62f};
            case NEXT_WEAPON: return new float[]{.65f,.784f};
            case PREV_WEAPON: return new float[]{.65f,.944f};
            case SUICIDE: return new float[]{.65f,.62f};
            case INVENTORY: return new float[]{.25f,.055f};
            case MAP: return new float[]{.32f,.055f};
            case CHAT: return new float[]{.39f,.055f};
            default: return new float[]{.13f,.32f};
        }
    }
    public static float gameplaySize(int action,float height,float density,float scale) {
        if (utility(action)) return 44*density;
        float base = Math.max((action == FIRE ? 56 : 44)*density, height*(action == FIRE ? .115f : .09f));
        return Math.max(44*density,base*scale);
    }
    public static float[] placeGameplay(int action,float nx,float ny,float size,float left,float top,
            float right,float bottom,float density,boolean leftHanded,boolean custom,
            List<float[]> occupied,List<float[]> reserved) {
        boolean utility = utility(action);
        if (!utility && leftHanded) nx = 1-nx;
        float x = left+nx*(right-left), y = top+ny*(bottom-top);
        float areaLeft=left,areaRight=right,areaTop=top,areaBottom=bottom;
        if (!utility && !custom) {
            areaTop += 60*density;
            if (leftHanded) areaRight=left+(right-left)*.65f;
            else areaLeft=right-(right-left)*.65f;
        } else if (topRow(action) && !custom) areaBottom=Math.min(bottom,top+56*density);
        return place(x,y,size,areaLeft,areaTop,areaRight,areaBottom,4*density,4*density,occupied,custom?null:reserved);
    }
    /** Move the four default FPS controls together so HUD clearance keeps their relationships. */
    public static float[][] gameplayCluster(float left,float top,float right,float bottom,float density,float scale,
            boolean leftHanded,long actions,List<float[]> occupied,List<float[]> reserved) {
        return gameplayCluster(left,top,right,bottom,density,scale,leftHanded,new int[]{FIRE,AIM,GRENADE,JUMP},
                actions,occupied,reserved);
    }
    /**
     * The same places for other ids: primary, Aim, Grenade and Jump slots in that
     * order, and optionally a fifth in the Use place left of the primary, so a
     * seat's Exit keeps its distance from Fire whenever HUD clearance moves the group.
     */
    public static float[][] gameplayCluster(float left,float top,float right,float bottom,float density,float scale,
            boolean leftHanded,int[] ids,long actions,List<float[]> occupied,List<float[]> reserved) {
        if (!Float.isFinite(density) || density <= 0 || !Float.isFinite(scale) || scale <= 0
                || !Float.isFinite(left) || !Float.isFinite(top) || !Float.isFinite(right) || !Float.isFinite(bottom)
                || right <= left || bottom <= top || ids == null || (ids.length != 4 && ids.length != 5)) return null;
        float[][] base=new float[ids.length][];
        // Leave subpixel room so common float translations preserve the minimum finger gap.
        float primary=gameplaySize(ids[0],bottom-top,density,scale),secondary=gameplaySize(AIM,bottom-top,density,scale),gap=4*density+.05f;
        float xRight=left+(right-left)*.957f,xLeft=Math.min(left+(right-left)*.858f,xRight-(primary+secondary)/2-gap);
        float aimY=top+(bottom-top)*.934f,jumpY=top+(bottom-top)*.944f;
        float upperY=Math.min(top+(bottom-top)*.784f,Math.min(aimY-secondary-gap,jumpY-(primary+secondary)/2-gap));
        float minX=Float.POSITIVE_INFINITY,minY=Float.POSITIVE_INFINITY,maxX=Float.NEGATIVE_INFINITY,maxY=Float.NEGATIVE_INFINITY;
        float useX=Math.min(xLeft-(right-left)*(.858f-.75f),xLeft-(primary+secondary)/2-gap);
        for (int i=0;i<ids.length;++i) if (ids[i]>=0 && (actions&bit(ids[i]))!=0) {
            float size=i==0?primary:secondary,cx=i==4?useX:i==0||i==3?xLeft:xRight,cy=i==1?aimY:i==3?jumpY:upperY;
            if (leftHanded) cx=left+right-cx;
            base[i]=new float[]{cx-size/2,cy-size/2,cx+size/2,cy+size/2};
            minX=Math.min(minX,base[i][0]);maxX=Math.max(maxX,base[i][2]);minY=Math.min(minY,base[i][1]);maxY=Math.max(maxY,base[i][3]);
        }
        if (!Float.isFinite(minX)) return base;
        float areaLeft=leftHanded?left:right-(right-left)*.65f,areaRight=leftHanded?left+(right-left)*.65f:right;
        float minDx=areaLeft-minX,maxDx=areaRight-maxX,minDy=top+60*density-minY,maxDy=bottom-maxY;
        if (minDx>maxDx || minDy>maxDy) return null;
        float dx=Math.max(minDx,Math.min(maxDx,0)),dy=Math.max(minDy,Math.min(maxDy,0)),step=4*density;
        if (fitsGroup(base,dx,dy,occupied,reserved,gap)) return shiftGroup(base,dx,dy);
        float bestX=dx,bestY=0,score=Float.POSITIVE_INFINITY;
        // Prefer a shared vertical correction; only then search a shared sideways correction.
        for(float y=minDy;y<=maxDy+.01f;y+=step) if (y*y<score && fitsGroup(base,dx,y,occupied,reserved,gap)) {score=y*y;bestY=y;}
        if (Float.isFinite(score)) return shiftGroup(base,bestX,bestY);
        for(float y=minDy;y<=maxDy+.01f;y+=step) for(float x=minDx;x<=maxDx+.01f;x+=step) {
            float distance=x*x*4+y*y;
            if (distance<score && fitsGroup(base,x,y,occupied,reserved,gap)) {score=distance;bestX=x;bestY=y;}
        }
        return Float.isFinite(score)?shiftGroup(base,bestX,bestY):null;
    }
    private static boolean fitsGroup(float[][] group,float dx,float dy,List<float[]> occupied,List<float[]> reserved,float gap) {
        for (float[] r:group) if(r!=null) {
            for(float[] other:occupied) if(r[0]+dx<other[2]+gap && r[2]+dx+gap>other[0] && r[1]+dy<other[3]+gap && r[3]+dy+gap>other[1]) return false;
            if(reserved!=null) for(float[] other:reserved) if(r[0]+dx<other[2]+gap && r[2]+dx+gap>other[0] && r[1]+dy<other[3]+gap && r[3]+dy+gap>other[1]) return false;
        }
        return true;
    }
    private static float[][] shiftGroup(float[][] group,float dx,float dy) {
        float[][] result=new float[group.length][];
        for(int i=0;i<group.length;++i) if(group[i]!=null) result[i]=new float[]{group[i][0]+dx,group[i][1]+dy,group[i][2]+dx,group[i][3]+dy};
        return result;
    }
    /** Eight stable sectors, clockwise from twelve o'clock. Center/outside cancels. */
    public static int radialSector(float x, float y, float cx, float cy, float radius) {
        return radialSector(x,y,cx,cy,radius,8);
    }
    /**
     * Horizontal span {left,right} of a hint pill of the wanted width, centred at
     * centerX inside [minX,maxX] and clear of every HUD rect {left,top,right,bottom}
     * crossing its band [top,bottom] (minimap, objectives): a rect right of the
     * centre caps the right edge, one left of it the left edge. Null without room.
     */
    public static float[] hintSpan(float centerX,float wanted,float minX,float maxX,float top,float bottom,
                                   float gap,float[]... avoid) {
        float low=minX,high=maxX;
        for (float[] r:avoid) {
            if (r==null || r.length!=4 || !(r[2]>r[0]) || !(r[3]>r[1]) || r[3]<=top || r[1]>=bottom) continue;
            if ((r[0]+r[2])/2>=centerX) high=Math.min(high,r[0]-gap); else low=Math.max(low,r[2]+gap);
        }
        if (!(high-low>0) || !(wanted>0)) return null;
        float width=Math.min(wanted,high-low);
        float left=Math.max(low,Math.min(high-width,centerX-width/2));
        return new float[]{left,left+width};
    }
    /** The adaptive radial has one to twelve equal sectors, the first at twelve o'clock. */
    public static int radialSector(float x, float y, float cx, float cy, float radius,int sectors) {
        double dx=x-cx, dy=y-cy, distance=Math.hypot(dx,dy);
        if (sectors<1 || sectors>12 || !Double.isFinite(distance) || !Float.isFinite(radius)
                || radius<=0 || distance<radius*.48 || distance>radius*1.65) return -1;
        double width=Math.PI*2/sectors;
        double angle=Math.atan2(dy,dx)+Math.PI/2+width/2;
        if(angle<0) angle+=Math.PI*2;
        return (int)Math.floor(angle/width)%sectors;
    }
    /**
     * Four transient weapon-slot cells, Slot1..Slot4 from left to right, in one
     * row just above the ammo strip and inside the safe area. Null without room.
     */
    public static float[][] slotCells(float[] ammo,float left,float top,float right,float bottom,float density) {
        if (ammo == null || ammo.length != 4 || !Float.isFinite(density) || density <= 0) return null;
        for (float value:ammo) if (!Float.isFinite(value)) return null;
        float width=72*density,height=56*density,gap=6*density,row=4*width+3*gap;
        if (!(right-left >= row) || !(bottom-top >= height) || ammo[2] <= ammo[0]) return null;
        float cx=Math.max(left+row/2,Math.min(right-row/2,(ammo[0]+ammo[2])/2));
        float y=Math.max(top,Math.min(bottom,ammo[1]-8*density)-height);
        float[][] cells=new float[4][];
        for (int i=0;i<4;++i) {
            float x=cx-row/2+i*(width+gap);
            cells[i]=new float[]{x,y,x+width,y+height};
        }
        return cells;
    }
    /** The cell column a finger is over within the row's band, or -1 (cancel). */
    public static int slotAt(float x,float y,float[][] cells,float density) {
        if (cells == null || !Float.isFinite(x) || !Float.isFinite(y)) return -1;
        for (int i=0;i<cells.length;++i) {
            float[] c=cells[i];
            if (c != null && x >= c[0]-3*density && x <= c[2]+3*density && y >= c[1]-24*density && y <= c[3]+4*density) return i;
        }
        return -1;
    }
    public static boolean overlaps(float[] a,float[] b,float gap) {
        return a[0]<b[2]+gap && a[2]+gap>b[0] && a[1]<b[3]+gap && a[3]+gap>b[1];
    }
    private static boolean fits(float[] r,List<float[]> occupied,float gap) {
        for(float[] other:occupied) if(overlaps(r,other,gap)) return false;
        return true;
    }
    /** Original 1280x720 HUD stage, including all six enlarged objective lines. */
    public static void reserveGameplayHud(float left,float top,float right,float bottom,List<float[]> reserved) {
        reserveGameplayHud(left,top,right,bottom,reserved,false);
    }
    public static void reserveGameplayHud(float left,float top,float right,float bottom,List<float[]> reserved,boolean nativeAmmo) {
        reserveGameplayHud(left,top,right,bottom,reserved,nativeAmmo,false);
    }
    public static void reserveGameplayHud(float left,float top,float right,float bottom,List<float[]> reserved,
            boolean skipCoarseAmmo,boolean replaceCoarseQuest) {
        float scale=Math.min((right-left)/1280f,(bottom-top)/720f);
        if(scale<=0 || !Float.isFinite(scale)) return;
        float x=left+((right-left)-1280*scale)/2, y=top+((bottom-top)-720*scale)/2;
        if (!replaceCoarseQuest) reserved.add(new float[]{x+940*scale,y+35*scale,x+1230*scale,y+455*scale});
        reserved.add(new float[]{x+55*scale,y+580*scale,x+580*scale,y+720*scale});
        if (!skipCoarseAmmo) reserved.add(new float[]{x+940*scale,y+600*scale,x+1240*scale,y+720*scale});
    }
    /** Passive HUD occupancy has no finger target expansion or left-hand mirroring. */
    public static void reserveSurfaceRect(float[] rect,float left,float top,float right,float bottom,List<float[]> reserved) {
        if (rect==null || rect.length!=4 || right<=left || bottom<=top) return;
        for (float value:rect) if (!Float.isFinite(value) || value<0 || value>1) return;
        if (rect[2]<=rect[0] || rect[3]<=rect[1]) return;
        reserved.add(new float[]{left+rect[0]*(right-left),top+rect[1]*(bottom-top),
                left+rect[2]*(right-left),top+rect[3]*(bottom-top)});
    }
    public static float[] place(float cx,float cy,float size,float left,float top,
            float right,float bottom,float step,float gap,List<float[]> occupied) {
        return place(cx,cy,size,left,top,right,bottom,step,gap,occupied,null);
    }
    public static float[] place(float cx,float cy,float size,float left,float top,
            float right,float bottom,float step,float gap,List<float[]> occupied,List<float[]> reserved) {
        if(right-left<size || bottom-top<size) return null;
        cx=Math.max(left+size/2,Math.min(right-size/2,cx));
        cy=Math.max(top+size/2,Math.min(bottom-size/2,cy));
        float[] preferred={cx-size/2,cy-size/2,cx+size/2,cy+size/2};
        if(fits(preferred,occupied,gap) && (reserved==null || fits(preferred,reserved,gap))) return preferred;
        float[] best=null; float score=Float.POSITIVE_INFINITY;
        for(float y=top;y<=bottom-size+.01f;y+=step) for(float x=left;x<=right-size+.01f;x+=step) {
            float distance=(x+size/2-cx)*(x+size/2-cx)+(y+size/2-cy)*(y+size/2-cy);
            if(distance>=score) continue;
            float[] candidate={x,y,x+size,y+size};
            if(fits(candidate,occupied,gap) && (reserved==null || fits(candidate,reserved,gap))) { best=candidate; score=distance; }
        }
        return best;
    }
}

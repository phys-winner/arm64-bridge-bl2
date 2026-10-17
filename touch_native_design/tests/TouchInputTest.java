package com.tt2kgames.Borderlands2.touch;

import java.util.ArrayList;
import static com.tt2kgames.Borderlands2.touch.TouchContract.*;

public final class TouchInputTest {
    private static final class Packet {
        float x, y, yaw, pitch; long held, epoch;
        Packet(long epoch,float x,float y,float yaw,float pitch,long held) {
            this.epoch=epoch; this.x=x; this.y=y; this.yaw=yaw; this.pitch=pitch; this.held=held;
        }
    }
    private static final class Sink implements TouchInput.Sink {
        final ArrayList<Packet> packets=new ArrayList<>(); int resets;
        public void submit(long epoch,float x,float y,float yaw,float pitch,long held) {
            packets.add(new Packet(epoch,x,y,yaw,pitch,held));
        }
        public void reset() { resets++; }
        Packet last() { return packets.get(packets.size()-1); }
        float yaw() { float sum=0; for(Packet p:packets) sum+=p.yaw; return sum; }
    }
    private static void check(boolean ok,String message) { if(!ok) throw new AssertionError(message); }
    private static void near(float actual,float expected,String message) {
        check(Math.abs(actual-expected)<.0002f,message+": "+actual+" != "+expected);
    }
    private static Context play(long epoch) { return new Context(epoch,GAMEPLAY,GAMEPLAY_INPUT|RELATIVE_LOOK,(1L<<26)-1,1); }
    private static void checkHudLayout(float width,float height,float density,float insetL,float insetT,
            float insetR,float insetB,float requestedScale,boolean leftHanded) {
        float left=insetL,top=insetT,right=width-insetR,bottom=height-insetB;
        float scale=Math.min(requestedScale,Math.max(.75f,((bottom-top)/density-64)/260));
        ArrayList<float[]> occupied=new ArrayList<>(),hud=new ArrayList<>();
        TouchLayout.reserveGameplayHud(0,0,width,height,hud);
        int[] actions={PAUSE,-2,FIRE,AIM,GRENADE,JUMP,RELOAD,USE};float[][] cluster=null;
        for(int i=0;i<actions.length;++i) {
            float[] anchor=TouchLayout.gameplayAnchor(actions[i]);
            float size=TouchLayout.gameplaySize(actions[i],bottom-top,density,scale);
            if(i==2) {
                cluster=TouchLayout.gameplayCluster(left,top,right,bottom,density,scale,leftHanded,(1L<<26)-1,occupied,hud);
                check(cluster!=null,"HUD layout must fit the four primary buttons as one cluster "+width+"x"+height+" scale="+scale+" left="+leftHanded);
            }
            float[] r=i>=2&&i<6?cluster[i-2]:TouchLayout.placeGameplay(actions[i],anchor[0],anchor[1],size,left,top,right,bottom,
                    density,leftHanded,false,occupied,hud);
            String description="HUD layout "+width+"x"+height+" scale="+requestedScale+" left="+leftHanded+" action="+i;
            check(r!=null,description+" must retain every combat and utility button");
            check(r[0]>=left && r[1]>=top && r[2]<=right && r[3]<=bottom,description+" stays in safe bounds");
            check(r[2]-r[0]>=44*density-.01f && r[3]-r[1]>=44*density-.01f,description+" retains touch target size");
            for(float[] prior:occupied) check(!TouchLayout.overlaps(r,prior,4*density),description+" avoids other controls");
            for(float[] reserved:hud) check(!TouchLayout.overlaps(r,reserved,4*density),description+" leaves original HUD visible");
            occupied.add(r);
        }
    }
    private static void checkReferenceCluster() {
        near(TouchLayout.gameplayHitSlop(USE,3),42,"Action gains 14dp of finger margin");
        near(TouchLayout.gameplayHitSlop(JUMP,3),6,"neighbouring buttons retain their existing margin");
        check(TouchLayout.circleContains(58,22,22,22,22,TouchLayout.gameplayHitSlop(USE,1)),
                "Action accepts a tap 14dp outside its visible circle");
        check(!TouchLayout.circleContains(58.1f,22,22,22,22,TouchLayout.gameplayHitSlop(USE,1)),
                "expanded Action margin remains bounded");
        check(TouchLayout.circleContains(45,22,22,22,22,2),"round button permits small finger slop outside its visible side");
        check(!TouchLayout.circleContains(1,1,22,22,22,2),"transparent square corners cannot steal taps from the native ammo bar");
        check(!TouchLayout.circleContains(46.1f,22,22,22,22,2),"circular finger slop remains bounded");
        ArrayList<float[]> occupied=new ArrayList<>(),clear=new ArrayList<>();
        long all=(1L<<26)-1;
        float[][] original=TouchLayout.gameplayCluster(0,0,2780,1264,3,1,false,all,occupied,clear);
        check(original!=null,"FPS reference cluster fits the current phone aspect");
        int[] ids={FIRE,AIM,GRENADE,JUMP};
        for(int i=0;i<4;++i) {
            float[] anchor=TouchLayout.gameplayAnchor(ids[i]);
            near((original[i][0]+original[i][2])/2,anchor[0]*2780,"reference horizontal anchor "+ids[i]);
            near((original[i][1]+original[i][3])/2,anchor[1]*1264,"reference vertical anchor "+ids[i]);
        }
        clear.add(new float[]{2200,1130,2780,1264});
        float[][] adjusted=TouchLayout.gameplayCluster(0,0,2780,1264,3,1,false,all,occupied,clear);
        check(adjusted!=null,"native bottom ammo can be exposed by a common vertical correction");
        float dy=adjusted[0][1]-original[0][1];check(dy<0,"ammo clearance moves the cluster upward");
        for(int i=0;i<4;++i) {
            near(adjusted[i][0],original[i][0],"HUD clearance never moves Jump alone sideways");
            near(adjusted[i][1]-original[i][1],dy,"all four controls retain their relative heights");
            check(!TouchLayout.overlaps(adjusted[i],clear.get(0),12),"all primary hit circles leave ammo exposed");
        }
        float[][] mirrored=TouchLayout.gameplayCluster(0,0,2780,1264,3,1,true,all,occupied,new ArrayList<float[]>());
        for(int i=0;i<4;++i)near(mirrored[i][0],2780-original[i][2],"left handed controls mirror without mirroring the native HUD");
        float[] anchor=TouchLayout.gameplayAnchor(PAUSE);
        float[] pause=TouchLayout.placeGameplay(PAUSE,anchor[0],anchor[1],132,0,0,2780,1264,3,true,false,occupied,null);
        check((pause[0]+pause[2])/2<2780*.08f,"Pause remains top left in left handed mode");
        float[] custom=TouchLayout.placeGameplay(AIM,.12f,.28f,132,0,0,2780,1264,3,false,true,occupied,clear);
        near((custom[0]+custom[2])/2,.12f*2780,"an explicit custom position can use the other side of the screen");
    }
    private static long packed(float first,float second) {
        return (Float.floatToIntBits(first)&0xffffffffL)|((long)Float.floatToIntBits(second)<<32);
    }
    private static Context hud(long epoch,int weapons,long known,long actions) {
        return new Context(epoch,GAMEPLAY,GAMEPLAY_INPUT|RELATIVE_LOOK,actions,1,known,weapons,0,3,
                new float[]{.75f,.05f,.96f,.3f},new float[]{.7f,.88f,.94f,.98f});
    }
    private static boolean contains(int[] values,int wanted) {
        for(int value:values) if(value==wanted) return true;
        return false;
    }
    /** Stage 7: a second stick-zone finger turns the camera; hints keep off the HUD. */
    private static void checkStageSeven() {
        Sink sink=new Sink();TouchInput input=new TouchInput(sink);
        input.configure(2,60,.22f,.5f,false);input.setContext(play(70));
        input.down(1,TouchInput.MOVEMENT,-1,100,500);input.move(1,100,420);
        check(sink.last().y>0,"the first stick-zone finger is the stick");
        input.down(2,TouchInput.MOVEMENT,-1,300,600);input.move(2,320,600);
        near(sink.yaw(),2.2f,"a second stick-zone finger turns the camera by its own distance");
        check(sink.last().y>0,"while the stick keeps moving");
        input.up(1,100,420);input.move(2,340,600);
        check(sink.last().x==0 && sink.last().y==0,"the camera finger never inherits the stick");
        near(sink.yaw(),4.4f,"and keeps turning after the stick lifts");
        Sink busy=new Sink();TouchInput both=new TouchInput(busy);
        both.configure(2,60,.22f,.5f,false);both.setContext(play(71));
        both.down(1,TouchInput.MOVEMENT,-1,100,500);both.down(3,TouchInput.LOOK,-1,1500,400);
        both.down(2,TouchInput.MOVEMENT,-1,300,600);both.move(2,340,600);
        near(busy.yaw(),0,"a third finger cannot take the camera from its owner");
        both.move(3,1520,400);
        near(busy.yaw(),2.2f,"the camera owner keeps turning");
        Sink menuSink=new Sink();TouchInput menu=new TouchInput(menuSink);
        menu.configure(2,60,.22f,.5f,false);menu.setContext(new Context(72,MENU,UI_NAVIGATION,bit(BACK),5));
        menu.down(4,TouchInput.MOVEMENT,-1,100,500);menu.move(4,140,500);
        near(menuSink.yaw(),0,"menus still ignore stick-zone fingers");
        float[] free=TouchLayout.hintSpan(1200,600,40,2380,100,190,20);
        check(free!=null && free[0]==900 && free[1]==1500,"without HUD rects the hint stays centred");
        float[] map={1790,40,2040,300};
        float[] shifted=TouchLayout.hintSpan(1200,1400,40,2380,100,190,20,map);
        check(shifted!=null && shifted[1]==1770 && shifted[1]-shifted[0]==1400,"a wide hint moves left of the minimap");
        float[] squeezed=TouchLayout.hintSpan(1200,2400,40,2380,100,190,20,map);
        check(squeezed!=null && squeezed[0]==40 && squeezed[1]==1770,"a hint wider than the room narrows to it");
        float[] objectives={1600,330,2050,400};
        float[] below=TouchLayout.hintSpan(1200,600,40,2380,100,190,20,objectives);
        check(below!=null && below[0]==900 && below[1]==1500,"a HUD rect outside the hint band changes nothing");
        float[] leftSide={10,120,300,170};
        float[] right=TouchLayout.hintSpan(400,600,40,2380,100,190,20,leftSide);
        check(right!=null && right[0]==320 && right[1]==920,"a rect left of the centre caps the left edge");
        check(TouchLayout.hintSpan(1200,600,40,2380,100,190,20,new float[]{0,0,2400,1080})==null,"no room, no hint");
        check(TouchLayout.hintSpan(1200,600,40,2380,100,190,20,(float[])null)!=null,"an absent HUD rect is ignored");
    }
    /** Settings are written off the UI thread after SAVE_DELAY_MS, coalesced, and flushed on demand. */
    private static void checkDeferredSettingsWrite() throws Exception {
        java.io.File dir=java.nio.file.Files.createTempDirectory("bl2-touch-settings").toFile();
        try {
            java.io.File first=new java.io.File(dir,"first.properties");
            TouchPreferences prefs=new TouchPreferences(first);
            prefs.opacity=.5f;prefs.save();
            check(prefs.writes==0 && !first.exists(),"a change is not written on the calling thread");
            prefs.flush();
            check(prefs.writes==1 && first.isFile(),"flush commits the pending change and waits for it");
            check(new TouchPreferences(first).opacity==.5f,"the committed file reads back");
            prefs.flush();
            check(prefs.writes==1,"flush without a pending change writes nothing");
            java.io.File second=new java.io.File(dir,"second.properties");
            TouchPreferences burst=new TouchPreferences(second);
            for(int i=0;i<5;i++) { burst.look=.1f+i*.1f;burst.save(); }
            burst.flush();
            check(burst.writes==1 && Math.abs(new TouchPreferences(second).look-.5f)<1e-6f,
                    "a slider burst becomes one write of the last value");
            java.io.File third=new java.io.File(dir,"third.properties");
            TouchPreferences later=new TouchPreferences(third);
            later.ads=.7f;later.save();
            long deadline=System.currentTimeMillis()+TouchPreferences.SAVE_DELAY_MS+3000;
            while(later.writes==0 && System.currentTimeMillis()<deadline) Thread.sleep(20);
            check(later.writes==1 && third.isFile(),"the writer commits on its own after the delay");
            check(new TouchPreferences().writes==0,"in-memory host preferences never write");
        } finally {
            java.io.File[] files=dir.listFiles();
            if(files!=null) for(java.io.File file:files) file.delete();
            dir.delete();
        }
    }
    /** Protocol 6: native menu X/Y and LB/RB keys; the More panel holds only what no gesture expresses. */
    private static void checkStageSix() {
        check(VERSION==6 && ACTION_COUNT==43 && UI_ACTION==39 && UI_OTHER==40 && UI_PREVIOUS_TAB==41 && UI_NEXT_TAB==42,
                "protocol 6 appends the menu X/Y and LB/RB keys after the status tabs");
        long ui=bit(BACK)|bit(CONFIRM)|bit(UI_UP)|bit(UI_DOWN)|bit(UI_LEFT)|bit(UI_RIGHT)
                |bit(UI_ACTION)|bit(UI_OTHER)|bit(UI_PREVIOUS_TAB)|bit(UI_NEXT_TAB);
        long[] words=new long[25];
        long[] head={5,90,MENU,UI_NAVIGATION|UI_TAP_TEST,ui,60,0,-1,-1,0};
        System.arraycopy(head,0,words,0,head.length);
        check(Context.decode(words).epoch==0,"a protocol-5 producer without the menu keys is rejected");
        words[0]=VERSION;
        Context menu=Context.decode(words);
        check(menu.epoch==90 && menu.menu() && menu.usable() && menu.allows(UI_ACTION) && menu.allows(UI_NEXT_TAB),
                "v6 menus publish the four native keys");
        int[] compact=NativeTouchControls.menuCommands(menu,false,false);
        check(java.util.Arrays.equals(compact,new int[]{UI_ACTION,UI_OTHER}),
                "a native menu keeps only Action and Other: drags scroll and sideways swipes change tabs");
        int[] talkBack=NativeTouchControls.menuCommands(menu,false,true);
        check(java.util.Arrays.equals(talkBack,new int[]{UI_UP,UI_DOWN,UI_LEFT,UI_RIGHT,UI_ACTION,UI_OTHER,UI_PREVIOUS_TAB,UI_NEXT_TAB}),
                "TalkBack consumes swipes, so it keeps every key, all native");
        Context debug=new Context(91,MENU,UI_NAVIGATION|DEBUG_CANVAS,(1L<<25)-1,61);
        check(java.util.Arrays.equals(NativeTouchControls.menuCommands(debug,false,false),new int[]{RELOAD,NEXT_WEAPON,SKILL,SPRINT}),
                "the Debug canvas keeps X/Y/R/L; its own swipes scroll rows and change tabs");
        check(NativeTouchControls.menuCommands(debug,false,true).length==10,"Debug with TalkBack keeps directions and tabs");
        Context legacy=new Context(92,MENU,UI_NAVIGATION,(1L<<25)-1,62);
        check(java.util.Arrays.equals(NativeTouchControls.menuCommands(legacy,true,false),new int[]{1099,1100,1102,1103}),
                "the fallback keeps the virtual pad's Action, Other and shoulder keys; its swipes are the D-pad");
        check(NativeTouchControls.menuCommands(legacy,true,true)[0]==UI_UP,"fallback with TalkBack adds the D-pad keys");
        float[] action=NativeTouchControls.menuCommandPlace(compact,0),other=NativeTouchControls.menuCommandPlace(compact,1);
        check(action[0]==176 && action[1]==36 && other[0]==176 && other[1]==96,"two keys stack beside Select");
        float[] r=NativeTouchControls.menuCommandPlace(new int[]{RELOAD,NEXT_WEAPON,SKILL,SPRINT},2);
        check(r[0]==236 && r[1]==36,"a third and fourth key form the next column");
        float[] up=NativeTouchControls.menuCommandPlace(talkBack,0),right=NativeTouchControls.menuCommandPlace(talkBack,3);
        float[] x=NativeTouchControls.menuCommandPlace(talkBack,4),next=NativeTouchControls.menuCommandPlace(talkBack,7);
        check(up[0]==176 && up[1]==96 && right[0]==116 && right[1]==36 && x[0]==300 && x[1]==36 && next[0]==360 && next[1]==96,
                "the TalkBack panel keeps the earlier places");
        int[] debugFull=NativeTouchControls.menuCommands(debug,false,true);
        float[] tab=NativeTouchControls.menuCommandPlace(debugFull,8),tabNext=NativeTouchControls.menuCommandPlace(debugFull,9);
        check(tab[0]==236 && tab[1]==156 && tabNext[0]==176 && tabNext[1]==156,"Debug tab keys keep their upper row");
        // A key is one held-bit edge pair: a press packet, then its release.
        Sink sink=new Sink();TouchInput input=new TouchInput(sink);
        input.configure(2,60,.22f,.5f,false);input.setContext(menu);
        for (int key:new int[]{UI_ACTION,UI_OTHER,UI_PREVIOUS_TAB,UI_NEXT_TAB}) {
            int before=sink.packets.size();
            input.pulse(key);
            check(sink.packets.size()==before+2 && (sink.packets.get(before).held&bit(key))!=0
                    && (sink.last().held&bit(key))==0,"a native menu key presses and releases: "+key);
        }
        input.setContext(play(93));int count=sink.packets.size();input.pulse(UI_ACTION);
        check(sink.packets.size()==count,"gameplay never sends a menu key");
    }
    /** Protocol 5: slot cells over the ammo strip, HUD status taps, give-up and the adaptive radial. */
    private static void checkStageFive() {
        check(VERSION>=5 && ACTION_COUNT>=39 && SLOT1==31 && SLOT2==32 && SLOT3==33 && SLOT4==34
                && SUICIDE==35 && SKILLS==36 && LOGS==37 && CHALLENGES==38,
                "protocol 5 appends four slots, the give-up and three status tabs after the seat actions");
        long actions=((1L<<27)-1)|bit(SLOT1)|bit(SLOT2)|bit(SLOT4)|bit(LOGS)|bit(SKILLS)|bit(CHALLENGES);
        // Slots 1, 2 and 4 hold an SMG, a pistol and a launcher; slot 2 is equipped.
        int slots=WEAPON_SLOTS_KNOWN|0b1011|(2<<8)|(3<<16)|(1<<20)|(6<<28);
        long[] words=new long[25];
        long[] head={VERSION,80,GAMEPLAY,GAMEPLAY_INPUT|RELATIVE_LOOK,actions,3,
                bit(SLOT1)|bit(SLOT2)|bit(SLOT3)|bit(SLOT4)|bit(RELOAD),3,2,2|4|32|64|128};
        System.arraycopy(head,0,words,0,head.length);
        words[12]=packed(.75f,.88f);words[13]=packed(.98f,.96f);
        words[14]=packed(.72f,.2f);words[15]=packed(.97f,.4f);
        words[18]=packed(.05f,.78f);words[19]=packed(.11f,.84f);
        words[20]=packed(.34f,.88f);words[21]=packed(.66f,.92f);
        words[22]=packed(.06f,.68f);words[23]=packed(.26f,.75f);
        words[24]=slots&0xFFFFFFFFL;
        Context c=Context.decode(words);
        check(c.epoch==80 && c.hudSkill() && c.hudExperience() && c.hudChallenge() && c.hudObjectives() && c.hudAmmo(),
                "v5 decodes the skill icon, XP bar and challenge notice beside the earlier regions");
        near(c.skillRect[1],.78f,"skill icon top");near(c.experienceRect[2],.66f,"XP bar right");near(c.challengeRect[3],.75f,"challenge bottom");
        check(c.slotsKnown() && c.slotOccupied(0) && c.slotOccupied(1) && !c.slotOccupied(2) && c.slotOccupied(3)
                && c.equippedSlot()==2 && c.slotType(0)==3 && c.slotType(1)==1 && c.slotType(2)==0 && c.slotType(3)==6,
                "the slot word names occupied slots, the equipped one and each weapon type");
        check(NativeTouchControls.slotTypeLabel(3,true).equals("ПП") && NativeTouchControls.slotTypeLabel(6,false).equals("Launcher")
                && NativeTouchControls.slotTypeLabel(0,true).isEmpty(),"slot cells use the game's own weapon category names");
        words[19]=packed(Float.NaN,.84f);
        check(!Context.decode(words).hudSkill() && Context.decode(words).hudExperience() && Context.decode(words).hudChallenge(),
                "one malformed tap target leaves the others live");
        words[19]=packed(.11f,.84f);words[24]=0b1011|(2<<8);
        check(!Context.decode(words).slotsKnown() && Context.decode(words).equippedSlot()==0 && !Context.decode(words).slotOccupied(0),
                "a slot word without its known bit is ignored");
        words[24]=slots&0xFFFFFFFFL;

        // Hold the ammo strip: four transient cells; lift over one to pick its slot.
        TouchInput.HudGesture gesture=new TouchInput.HudGesture();
        gesture.down(3,TouchInput.HudGesture.AMMO_ZONE,1900,980,1000,80,14);
        check(!gesture.slotHoldDue(1399,c,14) && !gesture.slotsOpen(),"a press under 400ms keeps tap, double-tap and swipe");
        check(gesture.slotHoldDue(1400,c,14) && gesture.slotsOpen() && !gesture.slotHoldDue(1500,c,14),
                "a still 400ms press opens the cells exactly once");
        check(TouchInput.HudGesture.slotSelectable(c,0) && !TouchInput.HudGesture.slotSelectable(c,1)
                && !TouchInput.HudGesture.slotSelectable(c,2) && TouchInput.HudGesture.slotSelectable(c,3)
                && !TouchInput.HudGesture.slotSelectable(c,-1) && !TouchInput.HudGesture.slotSelectable(c,4),
                "only an occupied slot other than the equipped weapon's can be chosen");
        check(gesture.up(3,1900,700,1700,c,true,14,3)==SLOT4 && !gesture.slotsOpen(),"lifting over cell 4 selects Slot4 and closes the cells");
        gesture.down(4,TouchInput.HudGesture.AMMO_ZONE,1900,980,2000,80,14);
        check(gesture.slotHoldDue(2400,c,14),"the next hold opens the cells again");
        check(gesture.up(4,1900,980,2500,c,true,14,-1)==TouchInput.HudGesture.NONE && gesture.poll(3000,c)==TouchInput.HudGesture.NONE,
                "lifting off the cells cancels without a reload");
        gesture.down(5,TouchInput.HudGesture.AMMO_ZONE,1900,980,4000,80,14);gesture.slotHoldDue(4400,c,14);
        check(gesture.up(5,1900,700,4500,c,true,14,1)==TouchInput.HudGesture.NONE,"the equipped weapon's cell does nothing");
        gesture.down(6,TouchInput.HudGesture.AMMO_ZONE,1900,980,5000,80,14);gesture.move(6,1990,982);
        check(!gesture.slotHoldDue(5500,c,14),"a swipe never turns into the slot hold");
        check(gesture.up(6,1990,982,5500,c,true,14)==NEXT_WEAPON,"a slow swipe still switches to the next weapon");
        gesture.down(7,TouchInput.HudGesture.AMMO_ZONE,1900,980,6000,80,14);gesture.down(8,TouchInput.HudGesture.AMMO_ZONE,1950,990,6100,80,14);
        check(!gesture.slotHoldDue(6500,c,14),"two fingers on the strip never open the cells");
        gesture.cancel();
        long[] lone=words.clone();lone[4]=actions&~(bit(SLOT2)|bit(SLOT4));lone[24]=WEAPON_SLOTS_KNOWN|1|(1<<8)|(3<<16);
        gesture.down(9,TouchInput.HudGesture.AMMO_ZONE,1900,980,7000,80,14);
        check(!gesture.slotHoldDue(7500,Context.decode(lone),14),"a single weapon has no slot to switch to");
        gesture.cancel();
        Context vehicle=new Context(80,VEHICLE,GAMEPLAY_INPUT|RELATIVE_LOOK,actions,1);
        gesture.down(10,TouchInput.HudGesture.AMMO_ZONE,1900,980,8000,80,14);
        check(!gesture.slotHoldDue(8500,vehicle,14),"a vehicle seat has no slot cells");
        gesture.cancel();
        float[][] cells=TouchLayout.slotCells(new float[]{1820,950,2380,1030},0,0,2424,1080,2.6f);
        check(cells!=null && cells.length==4,"four slot cells");
        for(int i=0;i<4;++i) {
            check(cells[i][3]<=950-8*2.6f+.01f && cells[i][0]>=0 && cells[i][2]<=2424+.01f,"cell "+i+" sits above the strip inside the screen");
            if(i>0) check(cells[i][0]>=cells[i-1][2]+6*2.6f-.01f,"cells keep a finger gap in slot order "+i);
            check(cells[i][2]-cells[i][0]>=48*2.6f && cells[i][3]-cells[i][1]>=48*2.6f,"cell "+i+" is a full touch target");
        }
        float middle=(cells[2][0]+cells[2][2])/2;
        check(TouchLayout.slotAt(middle,cells[2][1]+5,cells,2.6f)==2 && TouchLayout.slotAt(middle,cells[2][1]-20*2.6f,cells,2.6f)==2,
                "a finger over or just above cell 3 picks it");
        check(TouchLayout.slotAt(middle,990,cells,2.6f)==-1 && TouchLayout.slotAt(Float.NaN,700,cells,2.6f)==-1,
                "a finger still on the strip, or an invalid one, cancels");
        check(TouchLayout.slotCells(new float[]{1820,950,2380,1030},0,0,600,1080,2.6f)==null
                && TouchLayout.slotCells(new float[]{1820,950,Float.NaN,1030},0,0,2424,1080,2.6f)==null,"no room or no strip, no cells");

        // Tap-only targets: a still tap acts, a drag becomes the stick or camera.
        TouchInput.HudTaps taps=new TouchInput.HudTaps();
        taps.down(11,TouchInput.HudTaps.SKILL_ZONE,TouchInput.MOVEMENT,200,860,9000,80);
        check(taps.owns(11) && taps.move(11,205,862,14)==null,"a still finger on the skill icon remains a tap");
        check(taps.up(11,205,862,9300,c,14)==SKILL && !taps.owns(11),"tapping the skill icon uses the action skill");
        taps.down(12,TouchInput.HudTaps.SKILL_ZONE,TouchInput.MOVEMENT,200,860,9400,80);
        float[] drag=taps.move(12,260,860,14);
        check(drag!=null && drag[0]==TouchInput.MOVEMENT && drag[1]==200 && drag[2]==860 && !taps.owns(12),
                "dragging off the icon hands the touch to the stick from where it went down");
        Sink sink=new Sink();TouchInput input=new TouchInput(sink);input.configure(2.6f,60,.22f,.55f,false);input.setContext(c);
        input.down(12,(int)drag[0],-1,drag[1],drag[2]);input.move(12,330,860);
        check(sink.last().x>.5f && Math.abs(sink.last().y)<.01f,"the handed-off stick moves at once");
        int[][] zones={{TouchInput.HudTaps.OBJECTIVES_ZONE,LOGS},{TouchInput.HudTaps.EXPERIENCE_ZONE,SKILLS},
                {TouchInput.HudTaps.CHALLENGE_ZONE,CHALLENGES}};
        for(int[] zone:zones) {
            taps.down(13,zone[0],TouchInput.LOOK,1900,300,10000,80);
            check(taps.up(13,1903,300,10200,c,14)==zone[1],"a still tap on HUD zone "+zone[0]+" opens its status tab");
        }
        taps.down(14,TouchInput.HudTaps.OBJECTIVES_ZONE,TouchInput.LOOK,1900,300,11000,80);
        check(taps.up(14,1900,300,11400,c,14)==TouchInput.HudTaps.NONE,"a 400ms press is not a tap");
        taps.down(15,TouchInput.HudTaps.OBJECTIVES_ZONE,TouchInput.LOOK,1900,300,12000,79);
        check(taps.up(15,1900,300,12100,c,14)==TouchInput.HudTaps.NONE,"a tap from a stale epoch does nothing");
        long[] noTabs=words.clone();noTabs[4]&=~bit(LOGS);
        taps.down(16,TouchInput.HudTaps.OBJECTIVES_ZONE,TouchInput.LOOK,1900,300,13000,80);
        check(taps.up(16,1900,300,13100,Context.decode(noTabs),14)==TouchInput.HudTaps.NONE,"an unavailable status tab does nothing");
        taps.down(17,TouchInput.HudTaps.EXPERIENCE_ZONE,TouchInput.LOOK,1900,300,14000,80);
        check(taps.up(17,1900,300,14100,vehicle,14)==TouchInput.HudTaps.NONE,"HUD taps act only on foot");
        taps.down(18,99,TouchInput.LOOK,1900,300,15000,80);
        check(!taps.owns(18),"an unknown zone is never captured");
        taps.down(19,TouchInput.HudTaps.SKILL_ZONE,TouchInput.MOVEMENT,200,860,16000,80);taps.cancel();
        check(!taps.owns(19),"cancel drops every pending tap");

        // The adaptive radial (review section 5).
        long foot=(1L<<27)-1;
        Context walk=new Context(5,GAMEPLAY,GAMEPLAY_INPUT|RELATIVE_LOOK,foot,1,foot,3,2,0,new float[4],new float[4]);
        int[] all=NativeTouchControls.radialItems(walk,false,false,false);
        check(contains(all,MAP) && contains(all,RELOAD) && contains(all,CHAT) && !contains(all,SUICIDE),
                "without other routes the radial keeps Map and Reload, and no Give up on foot");
        int[] lean=NativeTouchControls.radialItems(walk,false,true,true);
        check(!contains(lean,MAP) && !contains(lean,RELOAD) && lean.length==all.length-2,
                "a working minimap tap and Reload button remove their radial duplicates");
        long injured=bit(FIRE)|bit(MELEE)|bit(RELOAD)|bit(NEXT_WEAPON)|bit(PREV_WEAPON)|bit(GRENADE)|bit(SUICIDE)|bit(CHAT)|bit(PAUSE);
        Context ffyl=new Context(6,GAMEPLAY,GAMEPLAY_INPUT|RELATIVE_LOOK,injured,1,injured,3,2,0,new float[4],new float[4]);
        check(java.util.Arrays.equals(NativeTouchControls.radialItems(ffyl,false,true,true),
                new int[]{NEXT_WEAPON,MELEE,PREV_WEAPON,SUICIDE,CHAT}),"the FFYL radial: Weapon, Melee, Previous, Give up and Chat");
        check(NativeTouchControls.radialConfirm(SUICIDE) && !NativeTouchControls.radialConfirm(MELEE)
                && !NativeTouchControls.radialHold(SUICIDE) && NativeTouchControls.radialLabel(SUICIDE,true).equals("Сдаться"),
                "only Give up waits for a held sector");
        long seatActions=bit(FIRE)|bit(VEHICLE_HORN)|bit(SWITCH_SEATS)|bit(RELOAD)|bit(INVENTORY)|bit(MAP)|bit(SUICIDE);
        Context seat=new Context(7,VEHICLE,GAMEPLAY_INPUT|RELATIVE_LOOK,seatActions,1);
        int[] ride=NativeTouchControls.radialItems(seat,false,false,true);
        check(ride[0]==VEHICLE_HORN && ride[1]==SWITCH_SEATS && contains(ride,MAP) && !contains(ride,RELOAD) && !contains(ride,SUICIDE),
                "a seat keeps its own items and drops the Reload duplicate");
        float nine=NativeTouchControls.radialItemRadius(94*2.6f,9,2.6f),five=NativeTouchControls.radialItemRadius(94*2.6f,5,2.6f);
        check(nine>=25*2.6f && five>nine && five<=32*2.6f+.01f && NativeTouchControls.radialItemRadius(94*2.6f,12,2.6f)>=25*2.6f-.01f,
                "fewer sectors make larger targets, from 25dp up to 32dp");
    }
    private static void checkObjectiveHud() {
        long all=(1L<<26)-1;
        long[] words={2,90,GAMEPLAY,GAMEPLAY_INPUT|RELATIVE_LOOK,all,12,0,3,0,7,
                packed(.75f,.05f),packed(.96f,.3f),packed(.7f,.88f),packed(.94f,.98f),
                packed(.76f,.31f),packed(.97f,.34f)};
        Context visible=Context.decode(words);
        check(visible.hudMap() && visible.hudAmmo() && visible.hudObjectives() && visible.hudObjectivesKnown(),
                "objective float pairs extend the same native snapshot without replacing map or ammo metadata");
        near(visible.objectiveRect[0],.76f,"objective low-word left");near(visible.objectiveRect[3],.34f,"objective high-word bottom");
        words[14]=packed(Float.NaN,.31f);
        Context invalid=Context.decode(words);
        check(!invalid.hudObjectivesKnown() && invalid.hudMap() && invalid.hudAmmo(),
                "an invalid objective projection keeps both interactive HUD targets and the conservative quest fallback");
        words[14]=packed(.76f,.31f);words[15]=packed(.74f,.34f);invalid=Context.decode(words);
        check(!invalid.hudObjectivesKnown() && invalid.hudMap() && invalid.hudAmmo(),"inverted objective bounds cannot clear valid map and ammo");
        words[9]=11;words[14]=Long.MAX_VALUE;words[15]=Long.MIN_VALUE;
        Context empty=Context.decode(words);
        check(empty.hudObjectivesKnown() && !empty.hudObjectives() && empty.hudMap() && empty.hudAmmo(),
                "proven empty objectives ignore reserved-word bits and differ from a failed projection");
        words[9]=15;words[14]=packed(.76f,.31f);words[15]=packed(.97f,.34f);
        Context conflict=Context.decode(words);
        check(!conflict.hudObjectivesKnown() && conflict.hudMap() && conflict.hudAmmo(),"conflicting visible and empty flags invalidate only objectives");
        words[9]=3;words[14]=123456;words[15]=0;
        check(!Context.decode(words).hudObjectivesKnown(),"old version2 telemetry tags never become fabricated objective geometry");

        Context unarmed=new Context(90,GAMEPLAY,GAMEPLAY_INPUT|RELATIVE_LOOK,all&~bit(FIRE)&~bit(AIM)&~bit(RELOAD),12,
                bit(FIRE)|bit(AIM)|bit(RELOAD),0,0,9,visible.mapRect,new float[4]);
        check(NativeTouchControls.confirmedUnarmed(unarmed),"native known Fire-unavailable plus count zero proves empty ammo occupancy");
        Context special=new Context(90,GAMEPLAY,GAMEPLAY_INPUT|RELATIVE_LOOK,all,12,0,0,0,9,visible.mapRect,new float[4]);
        check(!NativeTouchControls.confirmedUnarmed(special),"an exceptional count-zero equipped state keeps usable routes and the ammo fallback");
        check(!NativeTouchControls.confirmedUnarmed(hud(90,-1,bit(FIRE),all&~bit(FIRE))),"unknown inventory cannot masquerade as confirmed unarmed");

        ArrayList<float[]> passive=new ArrayList<>();
        TouchLayout.reserveSurfaceRect(new float[]{.76f,.31f,.97f,.32f},0,-140,2780,1124,passive);
        near(passive.get(0)[3]-passive.get(0)[1],12.64f,"passive captions retain their actual height instead of a 44dp finger target");
        near(passive.get(0)[1],.31f*1264-140,"passive HUD mapping preserves the full Surface origin relative to the overlay");
        TouchLayout.reserveSurfaceRect(new float[]{.7f,.2f,.6f,.3f},0,0,2780,1264,passive);
        check(passive.size()==1,"an inverted passive region never poisons placement");

        // Current phone dimensions/density and measured minimap vicinity; the
        // bottom ammo fixture exercises a valid region needing vertical clearance.
        ArrayList<float[]> occupied=new ArrayList<>(),known=new ArrayList<>(),unknown=new ArrayList<>();
        float[][] reference=TouchLayout.gameplayCluster(140,0,2780,1264,3.5f,1,false,all,occupied,new ArrayList<float[]>());
        TouchLayout.reserveGameplayHud(0,0,2780,1264,known,true,true);
        TouchLayout.reserveSurfaceRect(new float[]{2100/2780f,112/1264f,2370/2780f,345/1264f},0,0,2780,1264,known);
        float[][] emptyCluster=TouchLayout.gameplayCluster(140,0,2780,1264,3.5f,1,false,all,occupied,known);
        check(emptyCluster!=null,"known empty objectives and ammo retain the reference cluster at the right edge");
        for(int i=0;i<4;++i) {near(emptyCluster[i][0],reference[i][0],"empty HUD keeps reference x "+i);near(emptyCluster[i][1],reference[i][1],"empty HUD keeps reference y "+i);}
        TouchLayout.reserveSurfaceRect(new float[]{2100/2780f,380/1264f,2630/2780f,420/1264f},0,0,2780,1264,known);
        TouchLayout.reserveSurfaceRect(new float[]{2200/2780f,1130/1264f,1,1},0,0,2780,1264,known);
        float[][] actual=TouchLayout.gameplayCluster(140,0,2780,1264,3.5f,1,false,all,occupied,known);
        check(actual!=null,"small valid quest bounds leave a real vertical corridor above bottom ammo");
        float dy=actual[0][1]-reference[0][1];check(dy<0,"known ammo shifts the cluster upward");
        for(int i=0;i<4;++i) {
            near(actual[i][0],reference[i][0],"accurate occupancy keeps every reference horizontal relationship "+i);
            near(actual[i][1]-reference[i][1],dy,"accurate occupancy translates all four controls by the same vertical amount "+i);
            for(float[] region:known)check(!TouchLayout.overlaps(actual[i],region,14),"known passive and interactive HUD remain exposed");
        }
        TouchLayout.reserveGameplayHud(0,0,2780,1264,unknown,true,false);
        TouchLayout.reserveSurfaceRect(new float[]{2200/2780f,1130/1264f,1,1},0,0,2780,1264,unknown);
        float[][] fallback=TouchLayout.gameplayCluster(140,0,2780,1264,3.5f,1,false,all,occupied,unknown);
        check(fallback!=null && fallback[0][0]<actual[0][0]-200,"unknown objectives preserve a conservative reserve rather than assuming an empty HUD");
    }
    private static void checkHudGestures() {
        long actions=(1L<<26)-1,known=bit(RELOAD)|bit(LAST_WEAPON)|bit(GRENADE);
        Context c=hud(70,3,known,actions);
        long[] words={2,70,GAMEPLAY,GAMEPLAY_INPUT|RELATIVE_LOOK,actions,9,known,3,0,3,
                packed(.75f,.05f),packed(.96f,.3f),packed(.7f,.88f),packed(.94f,.98f),0,0};
        Context decoded=Context.decode(words);
        check(decoded.hudMap() && decoded.hudAmmo() && decoded.weaponCount==3 && decoded.grenadeCount==0,
                "Version2 packed native Surface rects and actual counts decode independently of render scale");
        near(decoded.mapRect[0],.75f,"packed low-word X");near(decoded.ammoRect[3],.98f,"packed high-word bottom");
        long[] prompt=java.util.Arrays.copyOf(words,25);prompt[0]=VERSION;prompt[6]|=bit(USE);prompt[9]|=16;
        prompt[16]=packed(.38f,.49f);prompt[17]=packed(.65f,.55f);
        Context promptContext=Context.decode(prompt);
        check(promptContext.hudUse() && promptContext.hudMap() && promptContext.hudAmmo() && promptContext.known(USE),
                "v3 appends true native interaction-hint geometry without moving map/ammo fields");
        long[] stale=java.util.Arrays.copyOf(prompt,18);stale[0]=4;
        check(VERSION==6 && Context.decode(stale).epoch==0,"a protocol-4 producer without the slot, give-up and tab fields is rejected");
        near(promptContext.useRect[1],.49f,"native Use hint is normalized to the full Surface");
        prompt[16]=packed(Float.NaN,.49f);promptContext=Context.decode(prompt);
        check(!promptContext.hudUse() && promptContext.hudMap() && promptContext.hudAmmo() && promptContext.allows(USE),
                "bad prompt projection preserves independent native HUD and the usable Action fallback");
        prompt[0]=2;
        check(Context.decode(prompt).epoch==0,"an extended snapshot cannot masquerade as version2");
        words[10]=packed(Float.NaN,.05f);decoded=Context.decode(words);
        check(!decoded.hudMap() && decoded.hudAmmo(),"one invalid native rectangle leaves the other live and preserves fallback");
        words[10]=packed(.75f,.05f);words[13]=packed(.69f,.98f);decoded=Context.decode(words);
        check(decoded.hudMap() && !decoded.hudAmmo(),"inverted ammo rectangle cannot steal touch from fallback controls");
        check(Context.decode(java.util.Arrays.copyOf(words,8)).epoch==0,"v2 truncated as old snapshot is rejected");
        Context old=Context.decode(new long[]{1,71,GAMEPLAY,GAMEPLAY_INPUT,actions,1,0,0});
        check(old.gameplay() && old.weaponCount==-1 && !old.hudAmmo() && !old.known(RELOAD),"v1 compatibility preserves unknown availability without inventing HUD geometry");
        TouchInput.HudGesture gesture=new TouchInput.HudGesture();
        gesture.down(7,TouchInput.HudGesture.MAP_ZONE,100,100,0,70,14);
        check(gesture.up(7,100,100,80,c,true,14)==MAP,"native minimap tap opens the real map once");
        gesture.down(8,TouchInput.HudGesture.MAP_ZONE,100,100,100,70,14);gesture.move(8,130,100);
        check(gesture.up(8,100,100,180,c,true,14)==-1,"a minimap drag returning to start never becomes a tap");
        gesture.down(9,TouchInput.HudGesture.MAP_ZONE,100,100,200,70,14);
        check(gesture.up(9,100,100,280,hud(71,3,known,actions),true,14)==-1,"old epoch HUD release cannot activate in a new scene");
        for(int direction:new int[]{-1,1}) {
            gesture.down(12,TouchInput.HudGesture.AMMO_ZONE,200,100,300,70,14);gesture.move(12,200+direction*100,100);
            check(gesture.up(12,200+direction*100,100,400,c,true,14)==(direction>0?NEXT_WEAPON:PREV_WEAPON),
                    "ammo horizontal swipe changes exactly one weapon with correct direction");
        }
        gesture.down(12,TouchInput.HudGesture.AMMO_ZONE,200,100,500,70,14);
        check(gesture.up(12,205,200,600,c,true,14)==-1,"vertical ammo drag does not cycle weapons or reload");
        gesture.down(12,TouchInput.HudGesture.AMMO_ZONE,200,100,700,70,14);
        check(gesture.up(12,300,100,800,hud(70,-1,known,actions),true,14)==-1,"unknown weapon count cannot promise a switch gesture");
        gesture.down(12,TouchInput.HudGesture.AMMO_ZONE,200,100,900,70,14);
        check(gesture.up(12,200,100,950,c,true,14)==-1 && gesture.poll(1229,c)==-1,"reload waits for the double-tap decision");
        gesture.down(91,TouchInput.HudGesture.AMMO_ZONE,201,101,1100,70,14);
        check(gesture.up(91,201,101,1150,c,true,14)==LAST_WEAPON && gesture.poll(2000,c)==-1,
                "double-tap selects original last-used action with three weapons and cancels pending reload");
        gesture.down(12,TouchInput.HudGesture.AMMO_ZONE,200,100,2100,70,14);
        gesture.up(12,200,100,2150,c,true,14);
        check(gesture.poll(2430,c)==RELOAD && gesture.poll(2500,c)==-1,"single tap reloads once after the deadline");
        gesture.down(12,TouchInput.HudGesture.AMMO_ZONE,200,100,2600,70,14);
        check(gesture.up(12,200,100,2650,hud(70,1,known,actions),true,14)==RELOAD,"one weapon keeps useful immediate reload");
        gesture.down(12,TouchInput.HudGesture.AMMO_ZONE,200,100,2700,70,14);
        check(gesture.up(12,200,100,2750,hud(70,1,0,actions),true,14)==-1,"unknown reload need does not invent an ammo action");
        gesture.down(12,TouchInput.HudGesture.AMMO_ZONE,200,100,2800,70,14);gesture.up(12,200,100,2850,c,true,14);gesture.cancel();
        check(gesture.poll(3200,c)==-1,"lifecycle cancellation removes delayed reload");
        gesture.down(12,TouchInput.HudGesture.MAP_ZONE,200,100,3300,70,14);gesture.down(55,TouchInput.HudGesture.AMMO_ZONE,200,100,3310,70,14);
        check(gesture.up(12,200,100,3350,c,true,14)==-1 && gesture.up(55,200,100,3360,c,true,14)==-1,
                "competing HUD fingers cancel both intentions without a weapon or map edge");
        gesture.down(12,TouchInput.HudGesture.AMMO_ZONE,200,100,3400,70,14);gesture.up(12,200,100,3450,c,true,14);
        check(gesture.poll(4000,hud(71,3,known,actions))==-1,"pending single tap cannot cross an epoch");
        Context unavailable=hud(70,3,known,actions&~bit(RELOAD)&~bit(LAST_WEAPON));
        gesture.down(12,TouchInput.HudGesture.AMMO_ZONE,200,100,4100,70,14);
        check(gesture.up(12,200,100,4150,unavailable,true,14)==-1 && gesture.poll(4500,c)==-1,
                "a known full/unreloadable magazine does not queue a later reload");
        Context physical=new Context(70,GAMEPLAY,GAMEPLAY_INPUT|RELATIVE_LOOK|PHYSICAL_OWNS_INPUT,actions,1,known,3,0,3,c.mapRect,c.ammoRect);
        gesture.down(12,TouchInput.HudGesture.MAP_ZONE,200,100,4600,70,14);
        check(gesture.up(12,200,100,4650,physical,true,14)==-1,"a physical controller owner cannot receive a touch HUD edge");
        Context fullMagazine=hud(70,3,known,actions&~bit(RELOAD));
        gesture.down(12,TouchInput.HudGesture.AMMO_ZONE,200,100,4700,70,14);gesture.up(12,200,100,4750,fullMagazine,true,14);
        check(gesture.poll(5100,c)==-1,"a full magazine tap cannot invent a delayed reload after another finger begins firing");
        Context noMap=new Context(c.epoch,c.mode,c.capabilities,c.actions,c.frame,c.knownActions,c.weaponCount,c.grenadeCount,2,c.mapRect,c.ammoRect);
        Context noAmmo=new Context(c.epoch,c.mode,c.capabilities,c.actions,c.frame,c.knownActions,c.weaponCount,c.grenadeCount,1,c.mapRect,c.ammoRect);
        gesture.down(13,TouchInput.HudGesture.MAP_ZONE,100,100,5200,70,14);
        check(gesture.up(13,100,100,5250,noMap,true,14)==-1,"a same-epoch vanished minimap cannot activate through its old captured hit box");
        gesture.down(14,TouchInput.HudGesture.AMMO_ZONE,200,100,5300,70,14);
        check(gesture.up(14,270,100,5350,noAmmo,true,14)==-1,"a same-epoch vanished ammo bar cannot switch weapons despite an available native route");
        gesture.down(15,TouchInput.HudGesture.AMMO_ZONE,200,100,5400,70,14);gesture.up(15,200,100,5450,c,true,14);
        check(gesture.poll(5500,noAmmo)==-1 && gesture.poll(5800,c)==-1,"ammo target loss before the tap timer expires discards reload rather than reviving it after HUD return");
        // Native availability already withholds Reload for a full magazine and
        // while reloading; the preference only decides the extra button.
        check(NativeTouchControls.reloadButtonShown(c,true,false,true),"a possible reload shows the button by default");
        check(!NativeTouchControls.reloadButtonShown(c,true,false,false),"Reload button OFF keeps the ammo bar as the reload route");
        check(NativeTouchControls.reloadButtonShown(c,false,false,false),"without a native ammo bar the button stays the only route");
        check(NativeTouchControls.reloadButtonShown(hud(70,3,0,actions),true,false,false),"unknown reload need keeps the button");
        check(NativeTouchControls.reloadButtonShown(c,true,true,false),"the layout editor always shows Reload");
        Context vehicle=new Context(70,VEHICLE,GAMEPLAY_INPUT|RELATIVE_LOOK,actions,1,known,3,0,3,c.mapRect,c.ammoRect);
        check(NativeTouchControls.reloadButtonShown(vehicle,true,false,false),"vehicles keep their Reload button");
    }
    private static void checkAutoRun() {
        Sink manualSink=new Sink();TouchInput manual=new TouchInput(manualSink);
        manual.configure(1,60,.22f,.55f,false);manual.setContext(play(79));
        manual.down(1,TouchInput.MOVEMENT,-1,50,150);manual.move(1,50,54);
        check(!manual.sprintLatched() && manualSink.last().held==bit(SPRINT) && manualSink.last().y==1,
                "default-off latch still permits ordinary far-up held sprint");
        manual.down(2,TouchInput.BUTTON,FIRE,500,300);
        check(manualSink.last().held==bit(FIRE),"firing suppresses manual sprint without losing its movement owner");
        manual.up(2,500,300);manual.up(1,50,54);manual.heartbeat();
        check(!manual.sprintLatched() && manualSink.last().held==0 && manualSink.last().y==0,
                "default-off latch never keeps movement or sprint after stick release");
        manual.down(1,TouchInput.MOVEMENT,-1,50,150);manual.move(1,50,54);manual.stopRunning();
        manual.up(1,50,54);
        check(manualSink.last().held==0 && manualSink.last().y==0,"pause stops held sprint before its final pointer sample");
        Sink sink=new Sink();TouchInput run=new TouchInput(sink);run.configure(1,60,.22f,.55f,false);run.autoSprint(true);run.setContext(play(80));
        run.down(19,TouchInput.MOVEMENT,-1,50,150);run.move(19,50,90);
        check(!run.sprintLatched() && sink.last().y==1 && (sink.last().held&bit(SPRINT))==0,"ordinary full forward is below the deliberate auto-run threshold");
        run.move(19,100,50);check(!run.sprintLatched(),"a far diagonal cannot accidentally latch straight auto-run");
        run.move(19,50,54);check(run.sprintLatched() && sink.last().held==bit(SPRINT),"far straight upward travel latches native sprint");
        run.up(19,50,54);run.heartbeat();check(sink.last().x==0 && sink.last().y==1 && run.moving(),"auto-run survives stick release with visible stop target");
        run.down(20,TouchInput.BUTTON,FIRE,500,300);run.move(20,510,300);
        check(sink.last().held==bit(FIRE) && sink.last().y==1,"fire/look suppresses sprint while preserving latched forward movement");
        int count=sink.packets.size();run.pulse(LAST_WEAPON);
        check(sink.packets.get(count).held==(bit(FIRE)|bit(LAST_WEAPON)) && sink.last().held==bit(FIRE) && sink.last().y==1,
                "HUD impulse preserves another finger's fire, look owner and auto-run axis");
        run.down(21,TouchInput.MOVEMENT,-1,50,150);
        check(!run.sprintLatched() && sink.last().y==0 && sink.last().held==bit(FIRE),"tap stick stops auto-run without releasing another fire owner");
        run.up(20,510,300);run.move(21,50,54);run.move(21,50,140);run.up(21,50,140);
        check(!run.sprintLatched() && sink.last().y==0,"pulling back into the stick center stops the latch");
        run.down(21,TouchInput.MOVEMENT,-1,50,150);run.move(21,50,54);run.up(21,50,54);run.autoSprint(false);run.heartbeat();
        check(sink.last().y==0 && sink.last().held==0,"disabling automatic sprint clears a released-stick latch");
        run.autoSprint(true);run.down(21,TouchInput.MOVEMENT,-1,50,150);run.move(21,50,54);run.up(21,50,54);
        run.setContext(new Context(81,MENU,UI_NAVIGATION,bit(BACK),2));
        check(!run.sprintLatched() && sink.last().y==0,"pause context stops auto-run before menu input");
        run.setContext(play(82));run.down(21,TouchInput.MOVEMENT,-1,50,150);run.move(21,50,54);run.stopRunning();
        check(!run.sprintLatched() && sink.last().y==0,"Pause activation can stop the latch before the native menu snapshot arrives");
        run.move(21,50,54);run.up(21,50,54);
        check(!run.sprintLatched() && sink.last().y==0,"a stopped far-up owner cannot relatch on its final UP sample");
        run.down(21,TouchInput.MOVEMENT,-1,50,150);run.move(21,50,54);run.stopRunning();run.move(21,50,140);run.move(21,50,54);
        check(run.sprintLatched() && sink.last().y==1,"returning the stopped owner to neutral permits a deliberate new auto-run gesture");
        run.up(21,50,54);run.cancel();run.setContext(play(83));
        check(!run.sprintLatched() && sink.last().y==0 && sink.last().held==0,"focus/lifecycle cancellation never resumes automatic movement");
        run.setContext(new Context(84,VEHICLE,GAMEPLAY_INPUT|RELATIVE_LOOK,(1L<<26)-1,1));
        run.down(21,TouchInput.MOVEMENT,-1,50,150);run.move(21,50,20);run.up(21,50,20);
        check(!run.sprintLatched() && sink.last().y==0,"vehicle throttle never inherits on-foot auto-run");
    }
    private static void checkVehicleSeat() {
        check(ACTION_COUNT==43 && EXIT_VEHICLE==27 && SWITCH_SEATS==28 && DRIVER_FIRE_TURRET==29 && LOOK_BACK==30,
                "protocol 4 appends the four shipped seat actions after Chat");
        long seat=bit(FIRE)|bit(VEHICLE_BOOST)|bit(VEHICLE_BRAKE)|bit(VEHICLE_HORN)|bit(EXIT_VEHICLE)
                |bit(SWITCH_SEATS)|bit(DRIVER_FIRE_TURRET)|bit(LOOK_BACK)|bit(MAP)|bit(INVENTORY);
        Context driver=new Context(9,VEHICLE,GAMEPLAY_INPUT|RELATIVE_LOOK,seat,1);
        check(driver.gameplay() && driver.usable() && !driver.legacyFallback(),"a routed seat owns touch input natively");
        Sink sink=new Sink(); TouchInput input=new TouchInput(sink);
        input.configure(2,60,.22f,.5f,false); input.aimToggle(true); input.setContext(driver);
        input.down(1,TouchInput.MOVEMENT,-1,0,0); input.move(1,0,-120);
        check(sink.last().y>.99f,"the stick reaches the seat as throttle");
        input.down(2,TouchInput.LOOK,-1,500,300); input.move(2,520,300);
        check(sink.last().yaw>0,"a look drag turns the seat camera (camera steering)");
        input.down(3,TouchInput.BUTTON,EXIT_VEHICLE,700,500);
        check((sink.last().held&bit(EXIT_VEHICLE))!=0,"Exit is a native held button");
        input.up(3,700,500);
        input.down(4,TouchInput.BUTTON,DRIVER_FIRE_TURRET,700,500);
        check((sink.last().held&bit(DRIVER_FIRE_TURRET))!=0,"a second finger holds Turret fire");
        input.up(4,700,500);
        check((sink.last().held&bit(DRIVER_FIRE_TURRET))==0,"lifting it stops Turret fire");
        input.down(5,TouchInput.BUTTON,GRENADE,700,500);input.down(6,TouchInput.BUTTON,ACTION_COUNT,700,500);
        check((sink.last().held&(bit(GRENADE)|bit(ACTION_COUNT)))==0,"on-foot and unknown actions stay out of a seat");
        int packets=sink.packets.size(); input.pulse(SWITCH_SEATS);
        check(sink.packets.size()==packets+2 && (sink.packets.get(packets).held&bit(SWITCH_SEATS))!=0
                && (sink.last().held&bit(SWITCH_SEATS))==0,"Switch seat is one press/release pulse from the radial");
        Context turret=new Context(10,VEHICLE,GAMEPLAY_INPUT|RELATIVE_LOOK,bit(FIRE)|bit(AIM)|bit(EXIT_VEHICLE)|bit(SWITCH_SEATS),1);
        Sink turretSink=new Sink(); TouchInput gunner=new TouchInput(turretSink);
        gunner.configure(2,60,.22f,.5f,false); gunner.aimToggle(true); gunner.setContext(turret);
        gunner.down(7,TouchInput.BUTTON,AIM,700,500);
        check((turretSink.last().held&bit(AIM))!=0,"a turret's Aim is held alternate fire even with Aim toggle on");
        gunner.up(7,700,500);
        check((turretSink.last().held&bit(AIM))==0,"lifting releases the turret's alternate fire: never latched");
        int[] seatRadial=NativeTouchControls.radialActions(driver),footRadial=NativeTouchControls.radialActions(play(9));
        check(seatRadial.length==9 && seatRadial[0]==VEHICLE_HORN && seatRadial[1]==SWITCH_SEATS
                && seatRadial[2]==LOOK_BACK && seatRadial[3]==DRIVER_FIRE_TURRET && seatRadial[8]==CHAT,
                "the seat radial carries Horn and Switch seat, then Look back and Turret");
        for(int action:footRadial) check(action<VEHICLE_BRAKE || action==CHAT || action==SUICIDE,"the on-foot radial has no seat items");
        check(NativeTouchControls.radialHold(LOOK_BACK) && NativeTouchControls.radialHold(DRIVER_FIRE_TURRET)
                && !NativeTouchControls.radialHold(VEHICLE_HORN) && !NativeTouchControls.radialHold(SWITCH_SEATS),
                "only Look back and Turret are held radial items");
        check(java.util.Arrays.equals(TouchLayout.gameplayAnchor(VEHICLE_BOOST),TouchLayout.gameplayAnchor(GRENADE))
                && java.util.Arrays.equals(TouchLayout.gameplayAnchor(VEHICLE_BRAKE),TouchLayout.gameplayAnchor(JUMP))
                && java.util.Arrays.equals(TouchLayout.gameplayAnchor(EXIT_VEHICLE),TouchLayout.gameplayAnchor(USE)),
                "the seat replaces the cluster: Boost, Brake and Exit take Grenade, Jump and Action places");
        ArrayList<float[]> occupied=new ArrayList<>(),hud=new ArrayList<>();
        TouchLayout.reserveGameplayHud(0,0,2424,1080,hud);
        float[][] foot=TouchLayout.gameplayCluster(0,0,2424,1080,2.6f,1,false,bit(FIRE)|bit(GRENADE)|bit(JUMP),occupied,hud);
        float[][] car=TouchLayout.gameplayCluster(0,0,2424,1080,2.6f,1,false,
                new int[]{FIRE,AIM,VEHICLE_BOOST,VEHICLE_BRAKE},seat,occupied,hud);
        check(foot!=null && car!=null && car[1]==null && java.util.Arrays.equals(car[0],foot[0])
                && java.util.Arrays.equals(car[2],foot[2]) && java.util.Arrays.equals(car[3],foot[3]),
                "the driver's Fire, Boost and Brake sit exactly where Fire, Grenade and Jump were; no Aim");
        float[][] seatGroup=TouchLayout.gameplayCluster(0,0,2424,1080,2.6f,1,false,
                new int[]{FIRE,AIM,VEHICLE_BOOST,VEHICLE_BRAKE,EXIT_VEHICLE},seat,occupied,hud);
        check(seatGroup!=null && seatGroup.length==5 && seatGroup[4]!=null && seatGroup[1]==null,
                "Exit joins the seat group in the Action place");
        check(seatGroup[4][2]+4*2.6f<=seatGroup[0][0]
                && Math.abs((seatGroup[4][1]+seatGroup[4][3])/2-(seatGroup[0][1]+seatGroup[0][3])/2)<.01f,
                "Exit stays left of Fire on its row, never beside Boost or Brake");
        for(int i:new int[]{2,3}) check(!TouchLayout.overlaps(seatGroup[4],seatGroup[i],4*2.6f),"Exit keeps a finger gap");
        float[][] mirrored=TouchLayout.gameplayCluster(0,0,2424,1080,2.6f,1,true,
                new int[]{FIRE,AIM,VEHICLE_BOOST,VEHICLE_BRAKE,EXIT_VEHICLE},seat,new ArrayList<>(),new ArrayList<>());
        check(mirrored!=null && mirrored[4][0]>=mirrored[0][2]+4*2.6f,"left-handed seats mirror Exit to Fire's right");
    }
    private static void checkChatEntry() {
        long oldActions=(1L<<26)-1;
        long[] words=new long[25];
        long[] head={VERSION,91,GAMEPLAY,GAMEPLAY_INPUT|RELATIVE_LOOK,oldActions,50,0,-1,-1};
        System.arraycopy(head,0,words,0,head.length);
        Context old=Context.decode(words);
        check(old.usable() && !old.allows(CHAT) && !contains(NativeTouchControls.radialItems(old,false,false,false),CHAT),
                "a native snapshot without Say has no Chat sector");
        words[4]|=bit(CHAT);
        Context chat=Context.decode(words);
        check(chat.usable() && chat.allows(CHAT) && contains(NativeTouchControls.radialItems(chat,false,false,false),CHAT),
                "the additive Say bit adds a Chat sector without changing the packet length or HUD metadata");
        check(!contains(NativeTouchControls.radialItems(chat,true,false,false),CHAT),"legacy fallback cannot invent the original Say entry");
        check(NativeTouchControls.radialItems(Context.unknown(),false,false,false).length==0,"unknown native availability opens no radial");
        Context physical=new Context(91,GAMEPLAY,GAMEPLAY_INPUT|RELATIVE_LOOK|PHYSICAL_OWNS_INPUT,words[4],50);
        check(NativeTouchControls.radialItems(physical,false,false,false).length==0,"physical ownership cannot expose a usable Chat gesture");
        Context menu=new Context(92,MENU,DIRECT_UI|UI_NAVIGATION,bit(BACK)|bit(CHAT),51);
        check(!contains(NativeTouchControls.radialItems(menu,false,false,false),CHAT),"native menu actions do not become a gameplay Chat sector");
        for(int sectors=1;sectors<=12;++sectors) {
            for(int i=0;i<sectors;++i) {
                double angle=i*Math.PI*2/sectors-Math.PI/2;
                float x=220+(float)Math.cos(angle)*100,y=180+(float)Math.sin(angle)*100;
                check(TouchLayout.radialSector(x,y,220,180,100,sectors)==i,"every radial sector matches its drawn control: "+sectors+"/"+i);
                if(sectors==8) check(TouchLayout.radialSector(x,y,220,180,100)==i,"legacy radial API preserves eight-sector geometry");
            }
            check(TouchLayout.radialSector(220,180,220,180,100,sectors)==-1,"radial center cancels for "+sectors+" sectors");
            check(TouchLayout.radialSector(220,350,220,180,100,sectors)==-1,"leaving the radial outer ring cancels for "+sectors+" sectors");
        }
        check(TouchLayout.radialSector(220,80,220,180,100,0)==-1 && TouchLayout.radialSector(220,80,220,180,100,13)==-1,
                "sector counts outside the adaptive 1..12 fail closed");
        check(TouchLayout.radialSector(220,80,220,180,Float.NaN,9)==-1,"invalid radius cannot dispatch an appended action");

        Sink sink=new Sink();TouchInput input=new TouchInput(sink);input.configure(3,60,.22f,.55f,false);input.setContext(chat);
        input.down(7,TouchInput.MOVEMENT,-1,100,500);input.move(7,100,320);
        input.down(8,TouchInput.BUTTON,FIRE,1000,500);input.down(9,TouchInput.BUTTON,AIM,1100,500);
        long existing=bit(FIRE)|bit(AIM);int count=sink.packets.size();input.pulse(CHAT);
        check(sink.packets.size()==count+2 && sink.packets.get(count).held==(existing|bit(CHAT)) && sink.last().held==existing,
                "Chat dispatches one original begin/end pair without releasing independent Fire or Aim");
        near(sink.packets.get(count).y,1,"Chat pulse preserves the movement owner");
        near(sink.last().yaw,0,"Chat entry never submits synthetic camera displacement");
        input.down(10,TouchInput.BUTTON,CHAT,1200,500);
        check(sink.last().held==(existing|bit(CHAT)),"appended action passes the real pointer-down bound");
        count=sink.packets.size();input.pulse(CHAT);check(sink.packets.size()==count,"an already owned Chat action cannot retrigger");
        input.setContext(old);
        check(sink.last().held==existing && sink.last().y==1,"live Say revocation releases only Chat and retains other owners");
        input.up(10,1200,500);check(sink.last().held==existing,"a revoked pointer-up cannot restore the old Chat hold");
        count=sink.packets.size();input.pulse(CHAT);input.pulse(CHAT+1);
        check(sink.packets.size()==count,"unavailable Say and the next unknown action cannot dispatch");
        input.setContext(chat);input.down(11,TouchInput.BUTTON,CHAT,1200,500);
        input.setContext(new Context(92,TEXT_INPUT,0,0,51));
        check(sink.last().held==0 && sink.last().y==0,"original chat focus transition retires all gameplay touch owners");
        count=sink.packets.size();input.pulse(CHAT);check(sink.packets.size()==count,"text input cannot reopen or send Chat");
        input.setContext(physical);count=sink.packets.size();input.pulse(CHAT);input.down(12,TouchInput.BUTTON,CHAT,1200,500);
        check(sink.packets.size()==count,"physical takeover rejects both Chat transport entry points");
        input.cancel();count=sink.packets.size();input.pulse(CHAT);
        check(sink.resets==1 && sink.packets.size()==count,"lifecycle cancellation requires a fresh native context before Chat");
    }
    /** Layout editor: placements, More's content, the toolbar, the button panel and Fire's melee. */
    private static void checkLayoutEditor() {
        // The shipped layout: six permanent buttons, contextual Reload and Action, the rest in More.
        for (int a:new int[]{FIRE,AIM,GRENADE,JUMP,RELOAD,USE,PAUSE,TouchLayout.MORE_BUTTON})
            check(TouchLayout.defaultPlacement(a)==TouchLayout.PLACE_SCREEN,"on the screen by default: "+a);
        for (int a:new int[]{MELEE,CROUCH,SKILL,NEXT_WEAPON,PREV_WEAPON,INVENTORY,MAP,CHAT,SUICIDE})
            check(TouchLayout.defaultPlacement(a)==TouchLayout.PLACE_MORE,"inside More by default: "+a);
        for (int a:new int[]{FIRE,AIM,JUMP,USE,PAUSE,TouchLayout.MORE_BUTTON})
            check(!TouchLayout.placementAllowed(a,TouchLayout.PLACE_MORE) && TouchLayout.placementAllowed(a,TouchLayout.PLACE_HIDDEN),
                    "held controls and utilities never become a sector but can be removed: "+a);
        check(TouchLayout.placementAllowed(GRENADE,TouchLayout.PLACE_MORE) && TouchLayout.placementAllowed(RELOAD,TouchLayout.PLACE_MORE),
                "Grenade and Reload may go into More");
        check(!TouchLayout.placementAllowed(VEHICLE_HORN,TouchLayout.PLACE_SCREEN) && !TouchLayout.placementAllowed(FIRE,3),
                "seat actions and unknown places are not editable");
        TouchPreferences p=new TouchPreferences();
        int[] defaultMore={NEXT_WEAPON,SKILL,CROUCH,MELEE,INVENTORY,MAP,PREV_WEAPON,SUICIDE,CHAT};
        check(java.util.Arrays.equals(p.moreItems(),defaultMore),"More keeps its shipped items and order");
        check(java.util.Arrays.equals(NativeTouchControls.footRadial(p),
                new int[]{NEXT_WEAPON,SKILL,CROUCH,MELEE,INVENTORY,MAP,PREV_WEAPON,RELOAD,SUICIDE,CHAT}),
                "the default foot radial equals the earlier fixed radial, Reload as its fallback");
        check(!p.placement(TouchLayout.MORE_BUTTON,TouchLayout.PLACE_HIDDEN)
                && p.placement(TouchLayout.MORE_BUTTON)==TouchLayout.PLACE_SCREEN,"More cannot go while buttons are inside it");
        check(!p.placement(FIRE,TouchLayout.PLACE_MORE) && p.placement(FIRE)==TouchLayout.PLACE_SCREEN,"Fire never moves into More");
        check(p.placement(MELEE,TouchLayout.PLACE_SCREEN) && p.placement(MELEE)==TouchLayout.PLACE_SCREEN
                && !contains(p.moreItems(),MELEE),"Melee becomes a button and leaves More");
        check(p.placement(FIRE,TouchLayout.PLACE_HIDDEN) && p.placement(FIRE)==TouchLayout.PLACE_HIDDEN,"Fire can be removed");
        for (int a:defaultMore) if (a!=MELEE) check(p.placement(a,TouchLayout.PLACE_HIDDEN),"emptying More: "+a);
        check(p.moreItemCount()==0 && p.placement(TouchLayout.MORE_BUTTON,TouchLayout.PLACE_HIDDEN)
                && p.placement(TouchLayout.MORE_BUTTON)==TouchLayout.PLACE_HIDDEN,"an empty More can be removed");
        check(p.placement(GRENADE,TouchLayout.PLACE_MORE) && p.placement(TouchLayout.MORE_BUTTON)==TouchLayout.PLACE_SCREEN
                && java.util.Arrays.equals(p.moreItems(),new int[]{GRENADE}),"a button put into a removed More brings More back");
        check(java.util.Arrays.equals(NativeTouchControls.footRadial(p),new int[]{GRENADE,RELOAD}),
                "the foot radial follows the editor; Reload on the screen stays its fallback");
        p.placement(RELOAD,TouchLayout.PLACE_HIDDEN);
        check(java.util.Arrays.equals(NativeTouchControls.footRadial(p),new int[]{GRENADE}),"a removed Reload is no fallback either");
        java.util.Properties store;
        try {
            java.lang.reflect.Field storeField=TouchPreferences.class.getDeclaredField("store");
            storeField.setAccessible(true);
            store=(java.util.Properties)storeField.get(p);
        } catch (ReflectiveOperationException error) { throw new AssertionError(error); }
        store.setProperty("place_"+AIM,"more");store.setProperty("place_"+JUMP,"sideways");
        check(p.placement(AIM)==TouchLayout.PLACE_SCREEN && p.placement(JUMP)==TouchLayout.PLACE_SCREEN,
                "a forbidden or unknown stored place falls back to the default");
        p.position(MELEE,.5f,.5f);p.position(TouchLayout.MORE_BUTTON,.2f,.4f);
        check(NativeTouchControls.customPosition(p,TouchLayout.MORE_BUTTON),"More keeps a dragged position");
        p.resetLayout();
        check(java.util.Arrays.equals(p.moreItems(),defaultMore) && p.placement(FIRE)==TouchLayout.PLACE_SCREEN
                && !NativeTouchControls.customPosition(p,MELEE) && !NativeTouchControls.customPosition(p,TouchLayout.MORE_BUTTON),
                "the editor's reset restores places and positions");
        // More's radial follows the foot list on foot; a seat keeps its own items.
        long foot=(1L<<27)-1;
        Context walk=new Context(5,GAMEPLAY,GAMEPLAY_INPUT|RELATIVE_LOOK,foot,1,foot,3,2,0,new float[4],new float[4]);
        check(java.util.Arrays.equals(NativeTouchControls.radialItems(walk,false,false,false,new int[]{MELEE,GRENADE}),
                new int[]{MELEE,GRENADE}),"an edited More shows exactly its items");
        Context seat=new Context(7,VEHICLE,GAMEPLAY_INPUT|RELATIVE_LOOK,bit(VEHICLE_HORN)|bit(SWITCH_SEATS),1);
        check(java.util.Arrays.equals(NativeTouchControls.radialItems(seat,false,false,false,new int[0]),
                new int[]{VEHICLE_HORN,SWITCH_SEATS}),"a seat ignores the on-foot More");
        // An empty equipped weapon: Fire's place melees on foot only.
        int slots=WEAPON_SLOTS_KNOWN|0b0011|(2<<8)|(3<<16)|(1<<20);
        long known=bit(RELOAD);
        Context loaded=new Context(9,GAMEPLAY,GAMEPLAY_INPUT|RELATIVE_LOOK,foot,1,known,2,1,0,null,null,null,null,null,null,null,slots);
        Context empty=new Context(9,GAMEPLAY,GAMEPLAY_INPUT|RELATIVE_LOOK,foot,1,known,2,1,0,null,null,null,null,null,null,null,
                slots|WEAPON_EQUIPPED_EMPTY);
        check(!loaded.equippedEmpty() && empty.equippedEmpty() && empty.equippedSlot()==2,"the empty bit belongs to the equipped weapon");
        check(!NativeTouchControls.fireShowsMelee(loaded,false) && NativeTouchControls.fireShowsMelee(empty,false),
                "Fire's place melees only while the weapon is empty");
        check(!NativeTouchControls.fireShowsMelee(empty,true),"the editor always shows Fire");
        Context noMelee=new Context(9,GAMEPLAY,GAMEPLAY_INPUT|RELATIVE_LOOK,foot&~bit(MELEE),1,known,2,1,0,null,null,null,null,null,null,null,
                slots|WEAPON_EQUIPPED_EMPTY);
        check(!NativeTouchControls.fireShowsMelee(noMelee,false),"without Melee (FFYL) Fire stays");
        Context riding=new Context(9,VEHICLE,GAMEPLAY_INPUT|RELATIVE_LOOK,foot,1,known,2,1,0,null,null,null,null,null,null,null,
                slots|WEAPON_EQUIPPED_EMPTY);
        check(!NativeTouchControls.fireShowsMelee(riding,false),"a seat's Fire is the vehicle weapon");
        Context unequipped=new Context(9,GAMEPLAY,GAMEPLAY_INPUT|RELATIVE_LOOK,foot,1,known,2,1,0,null,null,null,null,null,null,null,
                (slots&~(7<<8))|WEAPON_EQUIPPED_EMPTY);
        check(!unequipped.equippedEmpty(),"no equipped slot, no empty weapon");
        long[] words=new long[25];
        long[] head={VERSION,80,GAMEPLAY,GAMEPLAY_INPUT|RELATIVE_LOOK,foot,3,known,2,1,0};
        System.arraycopy(head,0,words,0,head.length);
        words[24]=(slots|WEAPON_EQUIPPED_EMPTY)&0xFFFFFFFFL;
        check(Context.decode(words).equippedEmpty(),"the empty bit decodes from the snapshot's slot word");
        // Utilities and the menu buttons keep a fixed size, the top row and no mirroring.
        for (int a:new int[]{PAUSE,INVENTORY,MAP,CHAT}) check(TouchLayout.topRow(a) && TouchLayout.utility(a),"top row: "+a);
        check(TouchLayout.utility(TouchLayout.MORE_BUTTON) && !TouchLayout.topRow(TouchLayout.MORE_BUTTON)
                && !TouchLayout.utility(MELEE) && !TouchLayout.utility(FIRE),"More is a utility; Melee and Fire are not");
        check(TouchLayout.gameplaySize(INVENTORY,1080,3,1.4f)==44*3,"menu buttons keep 44dp like Pause");
        java.util.ArrayList<float[]> occupied=new java.util.ArrayList<>();
        float[] anchor=TouchLayout.gameplayAnchor(MAP);
        float[] map=TouchLayout.placeGameplay(MAP,anchor[0],anchor[1],44*2,0,0,1560,720,2,true,false,occupied,null);
        check(map!=null && map[3]<=56*2+.01f && Math.abs((map[0]+map[2])/2-anchor[0]*1560)<1,
                "a Map button sits in the top row, unmirrored in left-hand mode");
        for (int a:new int[]{SKILL,CROUCH,MELEE,NEXT_WEAPON,PREV_WEAPON,SUICIDE}) {
            float[] at=TouchLayout.gameplayAnchor(a);
            check(at[0]>=.6f && at[1]>=.6f,"an added combat button starts beside the cluster: "+a);
        }
        // Remove badge on the rim, toolbar pills centred and spaced, the panel fits phones.
        float[] badge=TouchLayout.removeBadge(100,100,40,2);
        check(Math.abs(Math.hypot(badge[0]-100,badge[1]-100)-40*Math.sqrt(2)*.72f)<.01f && badge[0]>100 && badge[1]<100 && badge[2]==20,
                "the remove badge sits on the button's upper right rim");
        float[][] pills=TouchLayout.toolbar(0,96,1560,new float[]{200,300,180,160},2);
        check(pills!=null && pills.length==4 && Math.abs((pills[0][0]+pills[3][2])/2-780)<.01f && pills[0][1]==96+16
                && Math.abs(pills[1][0]-pills[0][2]-16)<.01f,"toolbar pills are centred with 8dp gaps");
        check(TouchLayout.toolbar(0,0,300,new float[]{200,300},2)==null,"a toolbar that does not fit is not drawn");
        for (float[] screen:new float[][]{{1560,720,2},{2424,1080,2.625f},{2340,1080,2.75f}}) {
            float[][] r=TouchLayout.palette(0,0,screen[0],screen[1],TouchLayout.EDITABLE.length,screen[2]);
            check(r!=null && r.length==TouchLayout.EDITABLE.length+2,"the button panel fits "+screen[0]+"x"+screen[1]);
            check(r[0][0]>=0 && r[0][1]>=0 && r[0][2]<=screen[0] && r[0][3]<=screen[1],"the card stays on screen");
            for (int i=2;i<r.length;++i) {
                check(r[i][0]>=r[0][0] && r[i][2]<=r[0][2] && r[i][1]>=r[0][1] && r[i][3]<=r[0][3],"cells stay in the card");
                check(r[i][3]-r[i][1]>=32*screen[2]-.01f,"cells keep a 32dp touch height");
                float[] previous=null;
                for (int place=0;place<3;++place) {
                    float[] s=TouchLayout.segment(r[i],place,screen[2]);
                    check(s[0]>=r[i][0] && s[2]<=r[i][2] && s[2]-s[0]>=40*screen[2],"segments stay in the cell, 40dp+ wide");
                    if (previous!=null) check(s[0]>=previous[2],"segments do not overlap");
                    previous=s;
                }
            }
        }
        check(TouchLayout.palette(0,0,400,300,17,2)==null,"no panel without room");
        for (int a:TouchLayout.EDITABLE) check(TouchIcons.has(a),"every editable control has an icon: "+a);
        for (int a:new int[]{VEHICLE_BOOST,VEHICLE_BRAKE,EXIT_VEHICLE,VEHICLE_HORN,SWITCH_SEATS,LOOK_BACK,DRIVER_FIRE_TURRET,BACK,CONFIRM,SKIP})
            check(TouchIcons.has(a),"seat, menu and cinematic buttons have icons: "+a);
        check(!TouchIcons.has(UI_ACTION) && !TouchIcons.has(UI_OTHER),"screen-specific menu keys keep their captions");
        check(NativeTouchControls.tapButton(INVENTORY) && NativeTouchControls.tapButton(MAP) && NativeTouchControls.tapButton(CHAT)
                && !NativeTouchControls.tapButton(MELEE) && !NativeTouchControls.tapButton(FIRE),"menu buttons act on a tap");
    }
    public static void main(String[] args) throws Exception {
        check(TouchContract.MOVIE_PLAYING==1 && TouchContract.MOVIE_STARTUP==2,
                "movie state bits match kMovie* in bl2_touch_input.h");
        checkChatEntry();checkStageFive();checkStageSix();checkStageSeven();checkDeferredSettingsWrite();checkLayoutEditor();
        checkHudGestures();checkAutoRun();checkReferenceCluster();checkObjectiveHud();
        TouchPreferences hints=new TouchPreferences();
        check(!hints.mapTapHintSeen && !hints.weaponGestureHintSeen,"one-shot HUD hints begin unseen");
        hints.markHintSeen(false);hints.save();hints.reload();
        check(hints.mapTapHintSeen && !hints.weaponGestureHintSeen,"map hint acknowledgement persists without consuming the weapon hint");
        hints.markHintSeen(true);hints.resetTouch();hints.reload();
        check(hints.mapTapHintSeen && hints.weaponGestureHintSeen,"layout reset preserves both one-shot acknowledgements");
        for(int kind=TouchPreferences.HINT_GAME_MORE;kind<=TouchPreferences.HINT_STATUS_TAPS;++kind) {
            check(!hints.hintSeen(kind),"new tutorial begins unseen independently: "+kind);
            hints.markHintSeen(kind);hints.save();hints.reload();
            check(hints.hintSeen(kind),"tutorial acknowledgement survives settings save and reload: "+kind);
            if(kind<TouchPreferences.HINT_STATUS_TAPS) check(!hints.hintSeen(kind+1),"showing one tutorial does not consume the next");
        }
        hints.resetTouch();hints.reload();
        for(int kind=TouchPreferences.HINT_MAP;kind<=TouchPreferences.HINT_STATUS_TAPS;++kind)
            check(hints.hintSeen(kind),"Reset Controls retains every presented tutorial: "+kind);
        Context mapMenu=new Context(1,MENU,DIRECT_UI|UI_NAVIGATION|MAP_GESTURES,bit(BACK),1);
        check(NativeTouchControls.menuPointerAllowed(mapMenu,-1,0,false),"map first pointer is admitted");
        check(NativeTouchControls.menuPointerAllowed(mapMenu,7,1,false),"map second native UI pointer reaches the real pinch handler");
        check(!NativeTouchControls.menuPointerAllowed(mapMenu,7,1,true),"map pinch cannot press an overlay navigation button");
        check(!NativeTouchControls.menuPointerAllowed(mapMenu,7,0,false),"overlay-owned first pointer cannot become a pinch");
        check(!NativeTouchControls.menuPointerAllowed(mapMenu,7,2,false),"third map pointer cancels the sequence");
        check(!NativeTouchControls.menuPointerAllowed(new Context(1,MENU,DIRECT_UI,bit(BACK),1),7,1,false),
                "other native menus retain second-pointer cancellation");
        for (int percent=20; percent<=150; percent+=10)
            check(TouchPreferences.renderPercent(percent)==percent,"render scale preserves ten-percent steps");
        check(TouchPreferences.renderPercent(54)==50 && TouchPreferences.renderPercent(55)==60,
                "render scale rounds to nearest ten percent");
        check(TouchPreferences.renderPercent(-1)==20 && TouchPreferences.renderPercent(14)==20
                && TouchPreferences.renderPercent(16)==20 && TouchPreferences.renderPercent(999)==150,
                "render scale bounds");
        check(TouchPreferences.renderPercent(Float.NaN)==50 && TouchPreferences.renderPercent(Float.POSITIVE_INFINITY)==50,
                "invalid render scale restores native default");
        for(int percent=100;percent<=200;percent+=10)
            check(TouchPreferences.uiPercent(percent)==percent,"interface scale preserves ten-percent steps");
        check(TouchPreferences.uiPercent(125)==125 && TouchPreferences.uiPercent(124)==120 && TouchPreferences.uiPercent(126)==130,
                "interface preserves its special default and rounds other values to the grid");
        check(TouchPreferences.uiPercent(-1)==100 && TouchPreferences.uiPercent(999)==200,
                "interface scale is bounded independently of render scale");
        check(TouchPreferences.uiPercent(Float.NaN)==125 && TouchPreferences.uiPercent(Float.POSITIVE_INFINITY)==125,
                "invalid interface scale restores the 125-percent default");
        int[] settings={10,1,0,100,Float.floatToIntBits(.42f),Float.floatToIntBits(1),
            Float.floatToIntBits(.22f),Float.floatToIntBits(.55f),0,0,0,0,1,1,
            Float.floatToIntBits(1),Float.floatToIntBits(.4f),0,1,1,0,125,0,0};
        settings=java.util.Arrays.copyOf(settings,44);
        java.util.Arrays.fill(settings,24,35,-1);
        settings[38]=1;
        settings[41]=1; // v9: icons ON; inset margin and hidden gamepad images OFF.
        settings[43]=1; // v10: look acceleration ON.
        check(TouchPreferences.validSettingsWire(settings),"native settings protocol admits bounded full vector");
        int[] defaults = settings.clone();
        settings[0]=1;
        check(!TouchPreferences.validSettingsWire(settings),"legacy version cannot be accepted with a new-sized vector");
        settings[0]=2;
        check(!TouchPreferences.validSettingsWire(settings),"v2 cannot silently accept FPS fields");
        settings[0]=3;
        check(!TouchPreferences.validSettingsWire(settings),"v3 cannot silently accept graphics fields");
        settings[0]=4;
        check(!TouchPreferences.validSettingsWire(settings),"v4 cannot silently accept performance fields");
        settings[0]=5;
        check(!TouchPreferences.validSettingsWire(settings),"v5 cannot silently accept the frame pacing field");
        settings[0]=6;
        check(!TouchPreferences.validSettingsWire(settings),"v6 cannot silently accept the Reload button field");
        settings[0]=7;
        check(!TouchPreferences.validSettingsWire(settings),"v7 cannot silently accept the radial tap field");
        settings[0]=8;
        check(!TouchPreferences.validSettingsWire(settings),"v8 cannot silently accept the inset, icon and gamepad image fields");
        settings[0]=9;
        check(!TouchPreferences.validSettingsWire(settings),"v9 cannot silently accept the look acceleration field");
        settings[0]=10;
        check(!TouchPreferences.validSettingsWire(java.util.Arrays.copyOf(settings,43)),"the v9 field count cannot be accepted as v10");
        check(!TouchPreferences.validSettingsWire(java.util.Arrays.copyOf(settings,42)),"old field count cannot be accepted as v10");
        check(!TouchPreferences.validSettingsWire(java.util.Arrays.copyOf(settings,45)),"extra fields cannot be accepted as v10");
        for(float sensitivity:new float[]{1.5f,2.5f,3}) {
            settings[6]=Float.floatToIntBits(sensitivity);settings[7]=Float.floatToIntBits(sensitivity);
            check(TouchPreferences.validSettingsWire(settings),"look and aim sensitivity reach 3x: "+sensitivity);
        }
        settings[6]=Float.floatToIntBits(3.01f);
        check(!TouchPreferences.validSettingsWire(settings),"look sensitivity stays within 3x");
        settings[6]=Float.floatToIntBits(.22f);settings[7]=Float.floatToIntBits(3.01f);
        check(!TouchPreferences.validSettingsWire(settings),"aim sensitivity stays within 3x");
        settings[7]=Float.floatToIntBits(.55f);
        for(int fps:new int[]{0,30,60,90}) {
            settings[22]=fps;
            check(TouchPreferences.validSettingsWire(settings),"exact selectable FPS mode admitted: "+fps);
        }
        for(int fps:new int[]{-1,1,29,45,59,120,Float.floatToIntBits(60)}) {
            settings[22]=fps;
            check(!TouchPreferences.validSettingsWire(settings),"arbitrary FPS cap rejected: "+fps);
        }
        settings[22]=0;settings[21]=2;
        check(!TouchPreferences.validSettingsWire(settings),"showFPS must be a boolean");settings[21]=0;
        for(int invalidPercent:new int[]{0,99,126,199,201,Float.floatToIntBits(125)}) {
            settings[20]=invalidPercent;
            check(!TouchPreferences.validSettingsWire(settings),"malformed global UI percent rejected: "+invalidPercent);
        }
        settings[20]=125;
        settings[6]=Float.floatToIntBits(Float.NaN);
        check(!TouchPreferences.validSettingsWire(settings),"native settings reject nonfinite slider values");
        settings[6]=Float.floatToIntBits(.22f);settings[11]=2;
        check(!TouchPreferences.validSettingsWire(settings),"native settings reject malformed microphone flag");
        settings[11]=0;settings[2]=16;
        check(TouchPreferences.validSettingsWire(settings),"native video options page state admitted");
        settings[2]=32;
        check(!TouchPreferences.validSettingsWire(settings),"native settings reject unknown commands");
        settings[2]=8;
        check(TouchPreferences.validSettingsWire(settings),"native debug preference explicitly edited flag admitted");
        settings[2]=0;settings[19]=-1;
        check(!TouchPreferences.validSettingsWire(settings),"native debug default must resolve to a boolean before persistence");
        TouchPreferences preferences = new TouchPreferences();
        check(preferences.uiPercent==125 && preferences.renderPercent==50 && !preferences.potatoGraphics,
                "new graphics default is render50 and Potato OFF; UI remains125");
        for (int i=0;i<preferences.advancedGraphics.length;++i)
            check(preferences.advancedGraphics[i]==(i==1?0:-1),"Motion Blur stays OFF; other absent graphics preferences inherit the runtime default");
        check(preferences.performance[0] && preferences.performance[1] && preferences.performance[2],
                "absent performance and frame pacing choices default ON");
        check(preferences.reloadButton && preferences.settingsWire(1)[37]==1,"absent Reload button choice defaults ON");
        check(!preferences.radialTap && preferences.settingsWire(1)[38]==0,"absent radial tap choice defaults OFF");
        check(!preferences.screenInsets && preferences.settingsWire(1)[39]==0,"controls use the whole screen unless the inset margin is chosen");
        check(preferences.buttonIcons && preferences.settingsWire(1)[40]==1,"absent icon choice defaults ON");
        check(!preferences.hideGamepadGlyphs && preferences.settingsWire(1)[41]==0,"gamepad button images stay unless hidden");
        check(preferences.lookAcceleration && preferences.settingsWire(1)[42]==1,"absent look acceleration choice defaults ON");
        settings[19]=0;
        check(TouchPreferences.validSettingsWire(settings),"toggle checks start from a valid vector");
        for(int invalid:new int[]{-1,2,Float.floatToIntBits(1)}) {
            settings[35]=invalid;check(!TouchPreferences.validSettingsWire(settings),"visibility optimization must be boolean");
            settings[35]=0;settings[36]=invalid;check(!TouchPreferences.validSettingsWire(settings),"audio optimization must be boolean");
            settings[36]=0;settings[37]=invalid;check(!TouchPreferences.validSettingsWire(settings),"frame pacing must be boolean");
            settings[37]=0;settings[38]=invalid;check(!TouchPreferences.validSettingsWire(settings),"Reload button must be boolean");
            settings[38]=1;settings[39]=invalid;check(!TouchPreferences.validSettingsWire(settings),"radial tap must be boolean");
            settings[39]=0;settings[40]=invalid;check(!TouchPreferences.validSettingsWire(settings),"inset margin must be boolean");
            settings[40]=0;settings[41]=invalid;check(!TouchPreferences.validSettingsWire(settings),"icons must be boolean");
            settings[41]=1;settings[42]=invalid;check(!TouchPreferences.validSettingsWire(settings),"hidden gamepad images must be boolean");
            settings[42]=0;settings[43]=invalid;check(!TouchPreferences.validSettingsWire(settings),"look acceleration must be boolean");
            settings[43]=1;
        }
        settings[43]=0;
        TouchPreferences v10=new TouchPreferences();
        check(v10.applySettingsWire(settings) && !v10.lookAcceleration && v10.settingsWire(1)[42]==0,
                "look acceleration OFF travels from the native page");
        settings[43]=1;settings[6]=Float.floatToIntBits(2.5f);
        check(v10.applySettingsWire(settings) && v10.lookAcceleration && Math.abs(v10.look-2.5f)<1e-6f
                && v10.settingsWire(1)[5]==Float.floatToIntBits(2.5f),"look acceleration ON and a 2.5x look travel both ways");
        settings[6]=Float.floatToIntBits(.22f);
        settings[40]=1;settings[41]=0;settings[42]=1;
        TouchPreferences v9=new TouchPreferences();
        check(v9.applySettingsWire(settings) && v9.screenInsets && !v9.buttonIcons && v9.hideGamepadGlyphs
                && v9.settingsWire(1)[39]==1 && v9.settingsWire(1)[40]==0 && v9.settingsWire(1)[41]==1,
                "the three v9 choices travel both ways");
        settings[40]=0;settings[41]=1;settings[42]=0;
        settings[23]=2;check(!TouchPreferences.validSettingsWire(settings),"Potato must be boolean");settings[23]=0;
        settings[24]=2;check(!TouchPreferences.validSettingsWire(settings),"graphics toggle must be boolean or original default");settings[24]=-1;
        settings[34]=5;check(!TouchPreferences.validSettingsWire(settings),"anisotropy index bounded to proven choices");settings[34]=-1;
        java.lang.reflect.Field storeField=TouchPreferences.class.getDeclaredField("store");
        storeField.setAccessible(true);
        java.util.Properties store=(java.util.Properties)storeField.get(preferences);
        store.setProperty("motionBlur","1");preferences.reload();
        check(preferences.advancedGraphics[1]==0,"older saved Motion Blur ON is ignored");
        preferences.advancedGraphics[1]=1;
        check(preferences.settingsWire(1)[24]==0,"Motion Blur cannot be enabled through the host wire");
        preferences.reload();
        java.util.Properties hintStore=(java.util.Properties)storeField.get(hints);
        java.io.ByteArrayOutputStream hintBytes=new java.io.ByteArrayOutputStream();
        try { hintStore.store(hintBytes,"cold tutorial preference check"); }
        catch(java.io.IOException error) { throw new AssertionError(error); }
        TouchPreferences coldHints=new TouchPreferences();
        java.util.Properties coldStore=(java.util.Properties)storeField.get(coldHints);
        try { coldStore.load(new java.io.ByteArrayInputStream(hintBytes.toByteArray())); }
        catch(java.io.IOException error) { throw new AssertionError(error); }
        coldHints.reload();coldHints.leftHanded=true;coldHints.save();coldHints.reload();
        for(int kind=TouchPreferences.HINT_MAP;kind<=TouchPreferences.HINT_LAYOUT;++kind)
            check(coldHints.hintSeen(kind),"fresh preferences retain every acknowledged tutorial through an unrelated control edit: "+kind);
        coldHints.markHintSeen(99);
        check(!coldHints.hintSeen(99),"unknown tutorial IDs cannot create acknowledgements");
        check(!preferences.autoSprint && !preferences.showFPS && preferences.fpsLimit==0,
                "absent optional latch and FPS preferences use OFF/OFF/display");
        store.setProperty("autoSprint","true");preferences.reload();
        check(!preferences.autoSprint,"old materialized autoSprint default is not an opt-in to the new latch");
        store.setProperty("autoRunLatch","true");preferences.reload();preferences.save();preferences.reload();
        check(preferences.autoSprint,"explicit new latch opt-in survives ordinary settings save and restart");
        preferences.resetTouch();preferences.reload();
        check(!preferences.autoSprint,"Reset Controls restores optional latch OFF");
        for(int fps:new int[]{0,30,60,90}) {
            preferences.fpsLimit=fps;preferences.showFPS=fps!=0;preferences.save();preferences.reload();
            check(preferences.fpsLimit==fps && preferences.showFPS==(fps!=0),"FPS mode and counter persist independently: "+fps);
        }
        for(String bad:new String[]{"29","45","120","NaN","Infinity","not a number"}) {
            store.setProperty("fpsLimit",bad);preferences.reload();
            check(preferences.fpsLimit==0,"bad persisted FPS limit restores display mode: "+bad);
        }
        String[] corruptUi={"99","126","201","NaN","Infinity","not a number"};
        int[] normalizedUi={100,130,200,125,125,125};
        for(int i=0;i<corruptUi.length;++i) {
            store.setProperty("uiPercent",corruptUi[i]);preferences.reload();
            check(preferences.uiPercent==normalizedUi[i],"loading malformed UI preference produces a bounded wire value");
        }
        preferences.renderPercent=90;preferences.uiPercent=126;preferences.save();preferences.reload();
        check(preferences.uiPercent==130 && preferences.renderPercent==90,"saved UI percentage normalizes independently of rendering");
        preferences.uiPercent=125;preferences.save();preferences.reload();
        check(preferences.uiPercent==125 && "125".equals(store.getProperty("uiPercent")),"125-percent default survives save and reload exactly");
        check(preferences.settingsWire(1).length==43 && preferences.settingsWire(1)[0]==10
                && preferences.settingsWire(1)[19]==125,"new FPS fields leave the UI and Debug IDs unchanged");
        int[] edited = defaults.clone();
        edited[1]=2; edited[2]=8; edited[3]=110;
        edited[4]=Float.floatToIntBits(.8f); edited[5]=Float.floatToIntBits(1.4f);
        edited[6]=Float.floatToIntBits(.8f); edited[7]=Float.floatToIntBits(.8f);
        edited[8]=1; edited[9]=1; edited[10]=0; edited[12]=0; edited[13]=0;
        edited[14]=Float.floatToIntBits(3); edited[15]=Float.floatToIntBits(3);
        edited[16]=1; edited[17]=0; edited[18]=0; edited[20]=190; edited[38]=0; edited[39]=1;
        edited[25]=1;
        check(preferences.applySettingsWire(edited),"native control edits admitted before reset");
        check(preferences.advancedGraphics[1]==0 && "0".equals(store.getProperty("motionBlur")),
                "native Motion Blur ON is normalized to OFF before persistence");
        check(!preferences.reloadButton && preferences.settingsWire(2)[37]==0,"Reload button OFF is applied from the native page");
        check(preferences.radialTap && preferences.settingsWire(2)[38]==1,"radial tap ON is applied from the native page");
        preferences.reload();
        check(!preferences.reloadButton,"Reload button OFF persists through save and reload");
        preferences.position(FIRE,.15f,.28f);
        int[] editedAndReset=edited.clone();
        editedAndReset[1]=3; editedAndReset[2]=2|8;
        editedAndReset[3]=150; editedAndReset[11]=1; editedAndReset[19]=1; editedAndReset[20]=200;
        editedAndReset[21]=1;editedAndReset[22]=60;
        editedAndReset[23]=1;editedAndReset[24]=0;editedAndReset[34]=2;
        editedAndReset[35]=0;editedAndReset[36]=1;editedAndReset[37]=0;editedAndReset[38]=0;
        check(preferences.applySettingsWire(editedAndReset),"one poll admits both newer values and Reset");
        int[] expected=defaults.clone(); expected[3]=150; expected[11]=1; expected[19]=1; expected[20]=200;
        expected[21]=1;expected[22]=60;
        expected[23]=1;expected[24]=0;expected[25]=0;expected[34]=2;
        expected[35]=0;expected[36]=1;expected[37]=0;
        int[] applied=preferences.settingsWire(3);
        for(int i=3;i<expected.length;++i)
            check(applied[i-1]==expected[i],"combined Reset restores control defaults and retains non-touch choices field "+(i-3));
        near(preferences.x(FIRE,.82f),.82f,"Reset removes custom horizontal layout position");
        near(preferences.y(FIRE,.75f),.75f,"Reset removes custom vertical layout position");
        preferences.reload();
        check(preferences.uiPercent==200 && preferences.renderPercent==150 && preferences.microphoneEnabled && preferences.debugMenuVisible
                && preferences.showFPS && preferences.fpsLimit==60 && preferences.potatoGraphics
                && preferences.advancedGraphics[0]==0 && preferences.advancedGraphics[10]==2,
                "Reset persists global UI, graphics, microphone, explicit Debug and FPS while clearing combat preferences");
        check(!preferences.performance[0] && preferences.performance[1] && !preferences.performance[2],
                "performance and frame pacing OFF/ON choices persist across touch reset and reload");
        int[] invalidReset=editedAndReset.clone(); invalidReset[6]=Float.floatToIntBits(Float.NaN);
        preferences.position(FIRE,.2f,.3f);
        check(!preferences.applySettingsWire(invalidReset),"malformed combined packet cannot reset preferences");
        near(preferences.x(FIRE,.82f),.2f,"rejected Reset preserves existing layout");
        TouchPreferences inheritedDebug=new TouchPreferences();
        int[] inheritedReset=defaults.clone(); inheritedReset[2]=2; inheritedReset[19]=1;
        check(inheritedDebug.applySettingsWire(inheritedReset),"Reset accepts the resolved build default");
        check(inheritedDebug.debugMenuVisible && inheritedDebug.settingsWire(1)[18]==-1,
                "Reset preserves resolved Debug without persisting an unedited build default");
        check(!Context.unknown().legacyFallback(), "unknown context hides fallback");
        check(!new Context(1,LOADING,0,0,1).legacyFallback(), "loading hides fallback");
        check(!new Context(1,CINEMATIC,0,0,1).legacyFallback(), "cinematic without skip hides fallback");
        Context skippable=new Context(4,CINEMATIC,CAN_SKIP,bit(SKIP)|bit(PAUSE),1);
        Context pauseOnly=new Context(4,CINEMATIC,CAN_SKIP,bit(PAUSE),1);
        check(skippable.usable() && NativeTouchControls.cinematicBackAction(skippable,true)==SKIP,"Back skips a skippable cinematic");
        check(NativeTouchControls.cinematicBackAction(pauseOnly,true)==PAUSE,"Back pauses an unskippable cinematic");
        check(NativeTouchControls.cinematicBackAction(new Context(4,CINEMATIC,CAN_SKIP,0,1),true)==-1
                && NativeTouchControls.cinematicBackAction(new Context(4,CINEMATIC,0,bit(SKIP),1),true)==-1
                && NativeTouchControls.cinematicBackAction(skippable,false)==-1,
                "an unrouted or ineligible cinematic leaves Back to the game");
        check(NativeTouchControls.cinematicBackAction(play(4),true)==-1,"gameplay Back is not a cinematic action");
        Sink cinematicSink=new Sink(); TouchInput cinematicInput=new TouchInput(cinematicSink);
        cinematicInput.configure(2,60,.22f,.5f,false); cinematicInput.setContext(skippable);
        cinematicInput.down(5,TouchInput.BUTTON,SKIP,10,10);
        check((cinematicSink.last().held&bit(SKIP))!=0,"held Skip reaches the native route");
        cinematicInput.down(6,TouchInput.MOVEMENT,-1,0,0); cinematicInput.move(6,120,-120);
        cinematicInput.down(7,TouchInput.LOOK,-1,0,0); cinematicInput.move(7,140,40);
        check(cinematicSink.last().x==0 && cinematicSink.last().y==0 && cinematicSink.last().yaw==0
                && cinematicSink.last().pitch==0,"a cinematic never produces movement or look");
        cinematicInput.up(5,10,10);
        check((cinematicSink.last().held&bit(SKIP))==0,"lifting the finger releases Skip");
        checkVehicleSeat();
        check(!NativeTouchControls.legacyUse(play(4),false,USE),"native Use never needs the virtual controller");
        check(NativeTouchControls.legacyUse(play(4),true,USE),"the legacy fallback keeps virtual key X");
        check(!NativeTouchControls.legacyUse(play(4),true,FIRE)
                && !NativeTouchControls.legacyUse(new Context(4,MENU,UI_NAVIGATION,(1L<<26)-1,1),true,USE),
                "only gameplay Use takes the fallback key");
        Sink useSink=new Sink(); TouchInput useInput=new TouchInput(useSink);
        useInput.configure(2,60,.22f,.5f,false); useInput.setContext(play(5));
        useInput.down(8,TouchInput.BUTTON,USE,10,10);
        check((useSink.last().held&bit(USE))!=0,"a held Action finger holds the native Use bit (hold to equip)");
        useInput.move(8,40,10);
        check((useSink.last().held&bit(USE))!=0 && useSink.last().yaw==0,"sliding on Action keeps holding without camera look");
        useInput.up(8,40,10);
        check((useSink.last().held&bit(USE))==0,"lifting the finger releases Use (tap = pickup)");
        check(new Context(1,FRONTEND,0,0,1).legacyFallback(), "press start keeps stock confirm");
        check(new Context(1,MENU,0,0,1).legacyFallback(), "native canvas menu keeps fallback");
        check(new Context(1,GAMEPLAY,0,0,1).legacyFallback(), "unsupported gameplay keeps stock input");
        check(!play(1).legacyFallback(), "native gameplay owns input");
        check(!new Context(1,MENU,PHYSICAL_OWNS_INPUT,0,1).legacyFallback(), "physical owner hides fallback");
        Sink sink=new Sink(); TouchInput input=new TouchInput(sink);
        input.configure(2,60,.22f,.5f,false); input.setContext(play(1));
        input.down(47,TouchInput.MOVEMENT,-1,0,0);
        input.move(47,120,-120);
        near((float)Math.hypot(sink.last().x,sink.last().y),1,"radial clamp avoids fast diagonal");
        check(sink.last().y>0,"forward semantic axis is positive");
        input.down(8,TouchInput.BUTTON,FIRE,500,300);
        input.move(8,520,300);
        near(sink.yaw(),2.2f,"fire drag rotates by distance");
        input.down(99,TouchInput.BUTTON,FIRE,600,300);
        input.up(8,520,300);
        check((sink.last().held&bit(FIRE))!=0,"second fire owner survives first release");
        input.up(99,600,300);
        check(sink.last().held==0,"last fire owner releases");
        check(sink.last().x!=0,"fire release preserves movement");
        input.heartbeat(); input.heartbeat();
        near(sink.yaw(),2.2f,"stationary heartbeat never repeats camera delta");
        input.setContext(play(2));
        check(sink.last().held==0 && sink.last().x==0,"epoch change rearms neutral");
        input.move(47,999,999);
        check(sink.last().x==0,"old pointer cannot revive after epoch");
        input.down(3,TouchInput.LOOK,-1,100,100);
        input.move(3,Float.NaN,Float.POSITIVE_INFINITY);
        check(Float.isFinite(sink.last().yaw),"bad coordinates rejected");
        input.down(5,TouchInput.BUTTON,AIM,0,0);
        input.move(3,120,120);
        near(sink.last().yaw,1.1f,"ADS gain applied once");
        near(sink.last().pitch,-1.1f,"pitch convention up-positive");
        input.relativeLook(2,-4);
        near(sink.last().yaw,1,"gyro shares ADS gain exactly once");
        near(sink.last().pitch,-2,"gyro uses independent vertical sign");
        check((sink.last().held&bit(AIM))!=0,"gyro preserves held aim owner");
        input.cancel();
        int count=sink.packets.size(); input.move(3,300,300); input.down(3,TouchInput.LOOK,-1,0,0);
        check(sink.resets==1 && sink.packets.size()==count,"lifecycle clears and rejects stale input");
        input.setContext(new Context(3,MENU,GAMEPLAY_INPUT|RELATIVE_LOOK|DIRECT_UI,bit(BACK),2));
        input.down(2,TouchInput.MOVEMENT,-1,0,0); input.move(2,100,100);
        check(sink.last().x==0 && sink.last().yaw==0,"menu cannot move even with bad capability publication");
        input.down(4,TouchInput.BUTTON,BACK,0,0);
        check(sink.last().held==bit(BACK),"menu semantic action allowed");
        count=sink.packets.size();input.relativeLook(10,10);
        check(sink.packets.size()==count,"menu suppresses sensor look despite invalid relative capability");
        input.setContext(new Context(4,GAMEPLAY,GAMEPLAY_INPUT|PHYSICAL_OWNS_INPUT,(1L<<25)-1,3));
        count=sink.packets.size(); input.down(7,TouchInput.BUTTON,FIRE,0,0);
        check(sink.packets.size()==count,"physical takeover rejects touch");
        Sink batched=new Sink(); TouchInput other=new TouchInput(batched);
        other.configure(2,60,.22f,.5f,false); other.setContext(play(1));
        other.down(9,TouchInput.LOOK,-1,0,0);
        for(int i=1;i<=10;i++) other.move(9,i*2,0);
        near(batched.yaw(),2.2f,"history splitting preserves integrated camera displacement");
        // LOOK ACCELERATION (density 2, .22 deg/dp): linear below .5 dp/ms, 2.5x from 2.5 dp/ms.
        near(TouchInput.accelerationGain(.2f),1,"slow drags stay linear");
        near(TouchInput.accelerationGain(1.5f),1.75f,"the gain rises evenly between the thresholds");
        near(TouchInput.accelerationGain(9),2.5f,"fast flicks are capped");
        check(TouchInput.accelerationGain(Float.NaN)==1,"a bad speed stays linear");
        Sink slow=new Sink(); TouchInput precise=new TouchInput(slow);
        precise.configure(2,60,.22f,.5f,false); precise.setContext(play(1));
        precise.down(9,TouchInput.LOOK,-1,0,0,1000);
        for(int i=1;i<=20;i++) precise.move(9,i*4,0,1000+i*8);
        near(slow.yaw(),40*.22f,"a slow timed drag (.25 dp/ms) turns exactly as before");
        Sink fast=new Sink(); TouchInput flick=new TouchInput(fast);
        flick.configure(2,60,.22f,.5f,false); flick.setContext(play(1));
        flick.down(9,TouchInput.LOOK,-1,0,0,1000);
        for(int i=1;i<=20;i++) flick.move(9,i*64,0,1000+i*8);
        check(fast.yaw()>640*.22f*1.8f && fast.yaw()<640*.22f*2.5f+.001f,"a 4 dp/ms flick turns about 2.4 times further: "+fast.yaw());
        near(fast.last().yaw,32*.22f*2.5f,"the smoothed speed reaches the full gain");
        Sink off=new Sink(); TouchInput linear=new TouchInput(off);
        linear.configure(2,60,.22f,.5f,false); linear.lookAcceleration(false); linear.setContext(play(1));
        linear.down(9,TouchInput.LOOK,-1,0,0,1000);
        for(int i=1;i<=20;i++) linear.move(9,i*64,0,1000+i*8);
        near(off.yaw(),640*.22f,"LOOK ACCELERATION OFF keeps the linear camera");
        Sink same=new Sink(); TouchInput burst=new TouchInput(same);
        burst.configure(2,60,.22f,.5f,false); burst.setContext(play(1));
        burst.down(9,TouchInput.LOOK,-1,0,0,1000);
        burst.move(9,64,0,1000); burst.move(9,128,0,1000); burst.move(9,192,0,1016);
        check(Float.isFinite(same.yaw()) && same.yaw()>=96*.22f-.001f && same.yaw()<=96*.22f*2.5f,
                "samples sharing a millisecond are timed with the next one: "+same.yaw());
        Sink moving=new Sink(); TouchInput stick=new TouchInput(moving);
        stick.configure(2,60,.22f,.5f,false); stick.setContext(play(1));
        stick.down(10,TouchInput.MOVEMENT,-1,0,0,1000);
        for(int i=1;i<=10;i++) stick.move(10,0,-i*64,1000+i*8);
        near(moving.yaw(),0,"a fast stick never turns the camera");
        Sink wide=new Sink(); TouchInput turn=new TouchInput(wide);
        turn.configure(2,60,5,5,false); turn.setContext(play(1));
        turn.down(11,TouchInput.LOOK,-1,0,0); turn.move(11,20,0);
        near(wide.yaw(),30,"look sensitivity is clamped to 3x (3 deg/dp)");
        turn.down(12,TouchInput.BUTTON,AIM,0,0); turn.move(11,40,0);
        near(wide.last().yaw,90,"aim sensitivity is clamped to 3x");
        other.down(12,TouchInput.BUTTON,JUMP,0,0); other.up(12,0,0);
        check(batched.packets.get(batched.packets.size()-3).held==bit(JUMP)
                && batched.last().held==0,"quick tap sends both states without 600ms dwell");
        other.down(17,TouchInput.MOVEMENT,-1,0,0); other.move(17,0,-120);
        other.down(18,TouchInput.BUTTON,FIRE,0,0);
        other.setContext(new Context(1,GAMEPLAY,GAMEPLAY_INPUT|RELATIVE_LOOK,bit(FIRE),1));
        check(batched.last().y==1 && batched.last().held==bit(FIRE),"action-mask changes preserve movement and valid hold");
        other.setContext(new Context(1,GAMEPLAY,GAMEPLAY_INPUT|RELATIVE_LOOK,0,1));
        check(batched.last().y==1 && batched.last().held==0,"revoked action releases alone");
        ArrayList<float[]> rects=new ArrayList<>();
        check(TouchLayout.radialSector(0,-100,0,0,100)==0,"top sector stable");
        check(TouchLayout.radialSector(100,0,0,0,100)==2,"right sector stable");
        check(TouchLayout.radialSector(0,0,0,0,100)==-1,"center cancels");
        check(TouchLayout.radialSector(Float.NaN,0,0,0,100)==-1,"invalid radial coordinates cancel");
        Sink sprintSink=new Sink();TouchInput sprint=new TouchInput(sprintSink);
        sprint.autoSprint(true);sprint.configure(1,60,.22f,.5f,false);sprint.setContext(play(20));
        sprint.down(33,TouchInput.MOVEMENT,-1,0,0);sprint.move(33,0,-60);
        check((sprintSink.last().held&bit(SPRINT))==0,"ordinary full forward does not latch automatic running");
        sprint.down(44,TouchInput.BUTTON,AIM,500,300);
        check((sprintSink.last().held&bit(SPRINT))==0,"aim stops automatic sprint");
        sprint.move(44,520,300);near(sprintSink.last().yaw,2.2f,"aim drag looks with ADS gain");
        sprint.up(33,0,-60);check(sprintSink.last().x==0 && sprintSink.last().y==0,"movement owner release stops movement");
        sprint.cancel();check(sprintSink.resets==1,"cancel releases aim and sprint");
        Sink toggleSink=new Sink();TouchInput toggle=new TouchInput(toggleSink);
        toggle.aimToggle(true);toggle.setContext(play(30));
        toggle.down(6,TouchInput.BUTTON,AIM,100,100);
        toggle.up(6,100,100);
        check(toggleSink.last().held==bit(AIM),"tap ADS latches aim for two-thumb movement and fire");
        toggle.down(42,TouchInput.BUTTON,FIRE,500,300);toggle.move(42,510,300);
        check(toggleSink.last().held==(bit(AIM)|bit(FIRE)),"ADS latch permits simultaneous fire and drag-look");
        toggle.down(6,TouchInput.BUTTON,AIM,100,100);toggle.down(7,TouchInput.BUTTON,AIM,100,100);
        check(toggleSink.last().held==bit(FIRE),"second simultaneous ADS owner cannot retrigger toggle");
        toggle.up(6,100,100);toggle.up(7,100,100);toggle.up(42,510,300);
        check(toggleSink.last().held==0,"second ADS tap ends the latched aim");
        toggle.down(6,TouchInput.BUTTON,AIM,100,100);toggle.up(6,100,100);
        toggle.setContext(new Context(30,GAMEPLAY,GAMEPLAY_INPUT|RELATIVE_LOOK,bit(FIRE),1));
        check(toggleSink.last().held==0,"revoking Aim clears the latch alone");
        toggle.setContext(play(31));toggle.down(6,TouchInput.BUTTON,AIM,100,100);toggle.up(6,100,100);
        toggle.cancel();toggle.setContext(play(32));
        check(toggleSink.last().held==0,"lifecycle cancellation never restores toggled ADS");
        TouchGyro gyro=new TouchGyro();
        long sample=1_000_000_000L;
        gyro.orientation(1,0,0,0,sample,0,1,0,false);
        near(gyro.delta[0],0,"first sensor sample establishes a neutral baseline");
        float half=(float)Math.toRadians(5);
        gyro.orientation((float)Math.cos(half),0,(float)Math.sin(half),0,sample+20_000_000,0,1,0,false);
        near(gyro.delta[0],10,"ten degree physical yaw maps to ten semantic degrees");
        near(gyro.delta[1],0,"pure yaw does not rotate pitch");
        gyro.orientation(-(float)Math.cos(half),0,-(float)Math.sin(half),0,sample+40_000_000,0,1,0,false);
        near(gyro.delta[0],0,"quaternion sign discontinuity cannot turn the camera");
        gyro.reset();gyro.orientation(1,0,0,0,sample,0,1,0,false);
        gyro.orientation((float)Math.cos(half),(float)Math.sin(half),0,0,sample+20_000_000,0,1,0,false);
        near(gyro.delta[1],-10,"physical pitch uses up-positive semantic camera convention");
        near(gyro.delta[0],0,"pure pitch does not rotate yaw");
        gyro.orientation(1,0,0,0,sample+300_000_000,0,1,0,false);
        near(gyro.delta[0],0,"sensor suspension discards accumulated physical rotation");
        gyro.reset();
        gyro.velocity(1,0,sample,1,1,0,false);
        gyro.velocity(1,0,sample+20_000_000,1,1,0,false);
        near(gyro.delta[0],-(float)Math.toDegrees(.02),"landscape gyro remaps device X to screen yaw");
        near(gyro.delta[1],0,"landscape yaw has no pitch cross-axis");
        gyro.velocity(1,0,sample+40_000_000,3,1,0,false);
        near(gyro.delta[0],0,"display rotation rebaselines sensor");
        gyro.velocity(1,0,sample+60_000_000,3,1,0,false);
        near(gyro.delta[0],(float)Math.toDegrees(.02),"reverse landscape has the matching horizontal sign");
        gyro.reset();gyro.velocity(.005f,0,sample,0,1,.4f,false);
        gyro.velocity(.005f,0,sample+20_000_000,0,1,.4f,false);
        near(gyro.delta[1],0,"stationary gyro noise stays inside velocity deadzone");
        gyro.velocity(1,0,sample+40_000_000,0,1,0,true);
        near(gyro.delta[1],(float)Math.toDegrees(.02),"gyro inversion changes only its own vertical direction");
        gyro.velocity(Float.NaN,0,sample+60_000_000,0,1,0,false);
        near(gyro.delta[1],0,"invalid sensor sample is neutral and resets tracking");
        gyro.velocity(1,0,sample+80_000_000,0,1,0,false);
        near(gyro.delta[1],0,"valid sample after sensor fault establishes a baseline");
        float splitYaw=0;gyro.reset();gyro.velocity(0,1,sample,0,1,.4f,false);
        for (int i=1; i<=5; ++i) {
            gyro.velocity(0,1,sample+i*10_000_000L,0,1,.4f,false);splitYaw+=gyro.delta[0];
        }
        gyro.reset();gyro.velocity(0,1,sample,0,1,.4f,false);
        gyro.velocity(0,1,sample+50_000_000L,0,1,.4f,false);
        near(gyro.delta[0],splitYaw,"sensor batching and frame rate cannot change integrated aim distance");
        for(int i=0;i<14;i++) {
            float[] r=TouchLayout.place(550,150,48,210,64,600,270,4,4,rects);
            check(r!=null,"compact layout must place all actions");
            check(r[0]>=210 && r[1]>=64 && r[2]<=600 && r[3]<=270,"layout inside safe bounds");
            for(float[] prior:rects) check(!TouchLayout.overlaps(r,prior,4),"no overlapping touch targets");
            rects.add(r);
        }
        for(float scale:new float[]{.85f,1,1.4f}) for(boolean leftHanded:new boolean[]{false,true}) {
            checkHudLayout(2424,1080,2.625f,0,63,0,82,scale,leftHanded);
            checkHudLayout(1920,1080,2.625f,0,63,0,82,scale,leftHanded);
            checkHudLayout(1280,720,2,24,24,24,32,scale,leftHanded);
            checkHudLayout(1024,768,2,0,24,0,32,scale,leftHanded);
        }
        ArrayList<float[]> hud=new ArrayList<>();
        TouchLayout.reserveGameplayHud(0,0,2424,1080,hud);
        float[] oldAim={2025,357,2151,483};
        check(TouchLayout.overlaps(oldAim,hud.get(0),0),"regression fixture reproduces Aim covering the408 objective checkbox");
        TouchPreferences custom=new TouchPreferences();
        check(!NativeTouchControls.customPosition(custom,AIM),"default positions receive HUD reservations");
        custom.position(AIM,2088f/2424,(420-63f)/(998-63));
        check(NativeTouchControls.customPosition(custom,AIM),"explicitly saved former default remains a custom choice");
        float[] chosen=TouchLayout.place(2088,420,126,0,0,2424,1080,10.5f,10.5f,new ArrayList<float[]>());
        near(chosen[0],2025,"custom placement can retain its chosen HUD-overlapping position");
        near(custom.x(AIM,0),2088f/2424,"layout placement does not rewrite saved coordinates");
        custom.position(AIM,0,1);
        check(NativeTouchControls.customPosition(custom,AIM),"saved edge coordinates are not mistaken for absent positions");
        custom.position(PAUSE,.25f,.55f);custom.save();custom.reload();
        float[] pause=TouchLayout.placeGameplay(PAUSE,custom.x(PAUSE,0),custom.y(PAUSE,0),88,
                0,0,1200,800,2,true,true,new ArrayList<float[]>(),new ArrayList<float[]>());
        check(pause!=null,"configured Pause remains a movable control");
        near((pause[0]+pause[2])/2,300,"saved Pause follows its selected horizontal position without left-hand mirroring");
        near((pause[1]+pause[3])/2,440,"saved Pause is not forced back to the default top strip");
        ArrayList<float[]> offsetHud=new ArrayList<>();
        TouchLayout.reserveGameplayHud(22,10,2446,1090,offsetHud);
        for(int i=0;i<hud.size();++i) for(int axis=0;axis<4;++axis)
            near(offsetHud.get(i)[axis],hud.get(i)[axis]+(axis%2==0?22:10),"HUD reservation follows Surface origin independently of safe insets");
        System.out.println("TouchInputTest: HUD gestures, held action margins, pointer lifecycle, tutorials, settings v10 (look acceleration, 3x sensitivity), layout editor placements, Fire-to-Melee, deferred writer, stick-zone camera, hint span, render50, graphics/performance/pacing/Reload button persistence and reset passed");
    }
}

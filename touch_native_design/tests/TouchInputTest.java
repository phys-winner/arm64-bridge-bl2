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
    private static Context play(long epoch) { return new Context(epoch,GAMEPLAY,GAMEPLAY_INPUT|RELATIVE_LOOK,(1L<<25)-1,1); }
    public static void main(String[] args) {
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
        input.cancel();
        int count=sink.packets.size(); input.move(3,300,300); input.down(3,TouchInput.LOOK,-1,0,0);
        check(sink.resets==1 && sink.packets.size()==count,"lifecycle clears and rejects stale input");
        input.setContext(new Context(3,MENU,GAMEPLAY_INPUT|RELATIVE_LOOK|DIRECT_UI,bit(BACK),2));
        input.down(2,TouchInput.MOVEMENT,-1,0,0); input.move(2,100,100);
        check(sink.last().x==0 && sink.last().yaw==0,"menu cannot move even with bad capability publication");
        input.down(4,TouchInput.BUTTON,BACK,0,0);
        check(sink.last().held==bit(BACK),"menu semantic action allowed");
        input.setContext(new Context(4,GAMEPLAY,GAMEPLAY_INPUT|PHYSICAL_OWNS_INPUT,(1L<<25)-1,3));
        count=sink.packets.size(); input.down(7,TouchInput.BUTTON,FIRE,0,0);
        check(sink.packets.size()==count,"physical takeover rejects touch");
        Sink batched=new Sink(); TouchInput other=new TouchInput(batched);
        other.configure(2,60,.22f,.5f,false); other.setContext(play(1));
        other.down(9,TouchInput.LOOK,-1,0,0);
        for(int i=1;i<=10;i++) other.move(9,i*2,0);
        near(batched.yaw(),2.2f,"history splitting preserves integrated camera displacement");
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
        for(int i=0;i<14;i++) {
            float[] r=TouchLayout.place(550,150,48,210,64,600,270,4,4,rects);
            check(r!=null,"compact layout must place all actions");
            check(r[0]>=210 && r[1]>=64 && r[2]<=600 && r[3]<=270,"layout inside safe bounds");
            for(float[] prior:rects) check(!TouchLayout.overlaps(r,prior,4),"no overlapping touch targets");
            rects.add(r);
        }
        System.out.println("TouchInputTest: pointer, lifecycle, look, admission and 14-target geometry checks passed");
    }
}

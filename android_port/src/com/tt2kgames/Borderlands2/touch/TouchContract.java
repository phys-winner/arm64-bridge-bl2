package com.tt2kgames.Borderlands2.touch;

/** Versioned ABI shared with bl2_touch_input.h. No engine offsets belong here. */
public final class TouchContract {
    private TouchContract() {}
    public static final int VERSION = 1;
    public static final int UNKNOWN = 0, LOADING = 1, CINEMATIC = 2, FRONTEND = 3,
            GAMEPLAY = 4, VEHICLE = 5, MENU = 6, DIALOG = 7, TEXT_INPUT = 8, DEAD = 9;
    public static final int GAMEPLAY_INPUT = 1, RELATIVE_LOOK = 2, DIRECT_UI = 4,
            PHYSICAL_OWNS_INPUT = 8, CAN_SKIP = 16, UI_NAVIGATION = 32, UI_TAP_TEST = 64;
    public static final int FIRE = 0, AIM = 1, JUMP = 2, CROUCH = 3, RELOAD = 4,
            USE = 5, MELEE = 6, GRENADE = 7, SKILL = 8, SPRINT = 9,
            NEXT_WEAPON = 10, PREV_WEAPON = 11, INVENTORY = 12, MAP = 13,
            PAUSE = 14, BACK = 15, CONFIRM = 16, SKIP = 17,
            VEHICLE_BRAKE = 18, VEHICLE_BOOST = 19, VEHICLE_HORN = 20,
            UI_UP = 21, UI_DOWN = 22, UI_LEFT = 23, UI_RIGHT = 24;
    public static final int DOWN = 0, MOVE = 1, UP = 2, CANCEL = 3;
    public static long bit(int action) { return 1L << action; }

    public static final class Context {
        public final long epoch, actions, frame;
        public final int mode, capabilities;
        public Context(long epoch, int mode, int caps, long actions, long frame) {
            this.epoch = epoch; this.mode = mode; capabilities = caps;
            this.actions = actions; this.frame = frame;
        }
        public boolean has(int cap) { return (capabilities & cap) != 0; }
        public boolean allows(int action) { return (actions & bit(action)) != 0; }
        public boolean gameplay() {
            return (mode == GAMEPLAY || mode == VEHICLE) && has(GAMEPLAY_INPUT);
        }
        public boolean menu() {
            return mode == FRONTEND || mode == MENU || mode == DIALOG || mode == DEAD;
        }
        public boolean usable() {
            return epoch > 0 && !has(PHYSICAL_OWNS_INPUT)
                    && (gameplay() || menu() || (mode == CINEMATIC && has(CAN_SKIP)));
        }
        public static Context unknown() { return new Context(0, UNKNOWN, 0, 0, 0); }
        public static Context decode(long[] words) {
            if (words == null || words.length != 8 || words[0] != VERSION
                    || words[2] < UNKNOWN || words[2] > DEAD) return unknown();
            return new Context(words[1], (int) words[2], (int) words[3], words[4], words[5]);
        }
    }
}

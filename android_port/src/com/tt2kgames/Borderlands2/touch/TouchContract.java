package com.tt2kgames.Borderlands2.touch;

/** Versioned ABI shared with bl2_touch_input.h. No engine offsets belong here. */
public final class TouchContract {
    private TouchContract() {}
    public static final int VERSION = 6;
    public static final int UNKNOWN = 0, LOADING = 1, CINEMATIC = 2, FRONTEND = 3,
            GAMEPLAY = 4, VEHICLE = 5, MENU = 6, DIALOG = 7, TEXT_INPUT = 8, DEAD = 9;
    public static final int GAMEPLAY_INPUT = 1, RELATIVE_LOOK = 2, DIRECT_UI = 4,
            PHYSICAL_OWNS_INPUT = 8, CAN_SKIP = 16, UI_NAVIGATION = 32, UI_TAP_TEST = 64,
            UI_WIDGETS = 128, PAUSE_BACKDROP = 256, DEBUG_CANVAS = 512, MAP_GESTURES = 1024;
    public static final int FIRE = 0, AIM = 1, JUMP = 2, CROUCH = 3, RELOAD = 4,
            USE = 5, MELEE = 6, GRENADE = 7, SKILL = 8, SPRINT = 9,
            NEXT_WEAPON = 10, PREV_WEAPON = 11, INVENTORY = 12, MAP = 13,
            PAUSE = 14, BACK = 15, CONFIRM = 16, SKIP = 17,
            VEHICLE_BRAKE = 18, VEHICLE_BOOST = 19, VEHICLE_HORN = 20,
            UI_UP = 21, UI_DOWN = 22, UI_LEFT = 23, UI_RIGHT = 24, LAST_WEAPON = 25, CHAT = 26,
            EXIT_VEHICLE = 27, SWITCH_SEATS = 28, DRIVER_FIRE_TURRET = 29, LOOK_BACK = 30,
            SLOT1 = 31, SLOT2 = 32, SLOT3 = 33, SLOT4 = 34, SUICIDE = 35, SKILLS = 36, LOGS = 37, CHALLENGES = 38,
            UI_ACTION = 39, UI_OTHER = 40, UI_PREVIOUS_TAB = 41, UI_NEXT_TAB = 42,
            ACTION_COUNT = 43;
    public static final int DOWN = 0, MOVE = 1, UP = 2, CANCEL = 3;
    /** Context.weaponSlots layout is documented beside kWeaponSlotsKnown in bl2_touch_input.h. */
    public static final int WEAPON_SLOTS_KNOWN = 1 << 15;
    public static long bit(int action) { return 1L << action; }

    public static final class Context {
        public final long epoch, actions, frame, knownActions;
        public final int mode, capabilities;
        public final int weaponCount, grenadeCount, hudValidMask, weaponSlots;
        public final float[] mapRect, ammoRect, objectiveRect, useRect, skillRect, experienceRect, challengeRect;
        public Context(long epoch, int mode, int caps, long actions, long frame) {
            this(epoch,mode,caps,actions,frame,0,-1,-1,0,new float[4],new float[4]);
        }
        public Context(long epoch, int mode, int caps, long actions, long frame,
                       long knownActions, int weaponCount, int grenadeCount, int hudValidMask,
                       float[] mapRect, float[] ammoRect) {
            this(epoch,mode,caps,actions,frame,knownActions,weaponCount,grenadeCount,hudValidMask,
                    mapRect,ammoRect,new float[4]);
        }
        public Context(long epoch, int mode, int caps, long actions, long frame,
                       long knownActions, int weaponCount, int grenadeCount, int hudValidMask,
                       float[] mapRect, float[] ammoRect, float[] objectiveRect) {
            this(epoch,mode,caps,actions,frame,knownActions,weaponCount,grenadeCount,hudValidMask,
                    mapRect,ammoRect,objectiveRect,new float[4]);
        }
        public Context(long epoch, int mode, int caps, long actions, long frame,
                       long knownActions, int weaponCount, int grenadeCount, int hudValidMask,
                       float[] mapRect, float[] ammoRect, float[] objectiveRect, float[] useRect) {
            this(epoch,mode,caps,actions,frame,knownActions,weaponCount,grenadeCount,hudValidMask,
                    mapRect,ammoRect,objectiveRect,useRect,null,null,null,0);
        }
        public Context(long epoch, int mode, int caps, long actions, long frame,
                       long knownActions, int weaponCount, int grenadeCount, int hudValidMask,
                       float[] mapRect, float[] ammoRect, float[] objectiveRect, float[] useRect,
                       float[] skillRect, float[] experienceRect, float[] challengeRect, int weaponSlots) {
            this.epoch = epoch; this.mode = mode; capabilities = caps;
            this.actions = actions; this.frame = frame;
            this.knownActions = knownActions;
            this.weaponCount = weaponCount;
            this.grenadeCount = grenadeCount;
            this.mapRect = mapRect == null ? new float[4] : mapRect.clone();
            this.ammoRect = ammoRect == null ? new float[4] : ammoRect.clone();
            this.objectiveRect = objectiveRect == null ? new float[4] : objectiveRect.clone();
            this.useRect = useRect == null ? new float[4] : useRect.clone();
            this.skillRect = skillRect == null ? new float[4] : skillRect.clone();
            this.experienceRect = experienceRect == null ? new float[4] : experienceRect.clone();
            this.challengeRect = challengeRect == null ? new float[4] : challengeRect.clone();
            int validMask = hudValidMask & 255;
            if (!validRect(this.mapRect)) validMask &= ~1;
            if (!validRect(this.ammoRect)) validMask &= ~2;
            if ((validMask & 12) == 12) validMask &= ~12;
            if (!validRect(this.objectiveRect)) validMask &= ~4;
            if (!validRect(this.useRect)) validMask &= ~16;
            if (!validRect(this.skillRect)) validMask &= ~32;
            if (!validRect(this.experienceRect)) validMask &= ~64;
            if (!validRect(this.challengeRect)) validMask &= ~128;
            this.hudValidMask = validMask;
            this.weaponSlots = (weaponSlots & WEAPON_SLOTS_KNOWN) != 0 ? weaponSlots : 0;
        }
        public boolean has(int cap) { return (capabilities & cap) != 0; }
        public boolean allows(int action) { return (actions & bit(action)) != 0; }
        public boolean known(int action) { return (knownActions & bit(action)) != 0; }
        public boolean hudMap() { return (hudValidMask & 1) != 0; }
        public boolean hudAmmo() { return (hudValidMask & 2) != 0; }
        public boolean hudObjectives() { return (hudValidMask & 4) != 0; }
        public boolean hudObjectivesKnown() { return (hudValidMask & 12) != 0; }
        public boolean hudUse() { return (hudValidMask & 16) != 0; }
        public boolean hudSkill() { return (hudValidMask & 32) != 0; }
        public boolean hudExperience() { return (hudValidMask & 64) != 0; }
        public boolean hudChallenge() { return (hudValidMask & 128) != 0; }
        public boolean slotsKnown() { return (weaponSlots & WEAPON_SLOTS_KNOWN) != 0; }
        /** Slot 0..3 holds a readied weapon; an unknown word answers false. */
        public boolean slotOccupied(int slot) { return slotsKnown() && slot >= 0 && slot < 4 && (weaponSlots & (1 << slot)) != 0; }
        /** 1..4 for the equipped weapon's slot, 0 when none or unknown. */
        public int equippedSlot() { return slotsKnown() ? (weaponSlots >> 8) & 7 : 0; }
        /** EWeaponType + 1 (pistol 1 .. launcher 6), 0 when unknown. */
        public int slotType(int slot) { return slot >= 0 && slot < 4 ? (weaponSlots >>> (16 + 4 * slot)) & 15 : 0; }
        private static boolean validRect(float[] rect) {
            if (rect.length != 4) return false;
            for (float value : rect) if (!Float.isFinite(value) || value < 0 || value > 1) return false;
            return rect[2] > rect[0] && rect[3] > rect[1];
        }
        private static float[] rect(long first, long second) {
            return new float[]{Float.intBitsToFloat((int)first),Float.intBitsToFloat((int)(first >>> 32)),
                    Float.intBitsToFloat((int)second),Float.intBitsToFloat((int)(second >>> 32))};
        }
        public boolean gameplay() {
            return (mode == GAMEPLAY || mode == VEHICLE) && has(GAMEPLAY_INPUT);
        }
        public boolean menu() {
            return mode == FRONTEND || mode == MENU || mode == DIALOG || mode == DEAD;
        }
        public boolean usable() {
            return epoch > 0 && !has(PHYSICAL_OWNS_INPUT)
                    && (gameplay() || (menu() && has(DIRECT_UI | UI_NAVIGATION | UI_TAP_TEST | UI_WIDGETS))
                    || (mode == CINEMATIC && has(CAN_SKIP)));
        }
        public boolean legacyFallback() {
            return !usable() && !has(PHYSICAL_OWNS_INPUT)
                    && (menu() || mode == GAMEPLAY || mode == VEHICLE);
        }
        public static Context unknown() { return new Context(0, UNKNOWN, 0, 0, 0); }
        public static Context decode(long[] words) {
            if (words == null || !((words.length == 8 && words[0] == 1)
                    || (words.length == 16 && words[0] == 2)
                    || (words.length == 25 && words[0] == VERSION))
                    || words[2] < UNKNOWN || words[2] > DEAD) return unknown();
            if (words.length == 8) return new Context(words[1], (int) words[2], (int) words[3], words[4], words[5]);
            if (words[7] < -1 || words[7] > 64 || words[8] < -1 || words[8] > 65535) return unknown();
            boolean current = words.length == 25;
            return new Context(words[1],(int)words[2],(int)words[3],words[4],words[5],
                    words[6],(int)words[7],(int)words[8],(int)words[9],
                    rect(words[10],words[11]),rect(words[12],words[13]),
                    (words[9] & 4) != 0 ? rect(words[14],words[15]) : new float[4],
                    current && (words[9] & 16) != 0 ? rect(words[16],words[17]) : new float[4],
                    current && (words[9] & 32) != 0 ? rect(words[18],words[19]) : new float[4],
                    current && (words[9] & 64) != 0 ? rect(words[20],words[21]) : new float[4],
                    current && (words[9] & 128) != 0 ? rect(words[22],words[23]) : new float[4],
                    current && words[24] >= 0 && words[24] <= 0xFFFFFFFFL ? (int) words[24] : 0);
        }
    }
}

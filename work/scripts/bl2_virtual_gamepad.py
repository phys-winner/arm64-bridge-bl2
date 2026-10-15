#!/usr/bin/env python3
"""Virtual HID gamepad for device QA (uhid + Android `hid` tool, no root).

Registers one USB game pad with 15 buttons, a hat switch, four D-pad buttons,
two sticks and two analog triggers. Linux hid-input and Android Generic.kl make
it a real controller (SOURCE_GAMEPAD | SOURCE_JOYSTICK | SOURCE_DPAD, its own
controller number), so the game sees it exactly like a Bluetooth pad.

Two D-pad flavours exist because real pads differ: `hat:<dir>` moves the hat
switch (AXIS_HAT_X/Y in joystick MotionEvents, as Xbox/most BT pads do) and
`dpad:<dir>` presses D-pad buttons (KEYCODE_DPAD_*, as some pads do).

Buttons (Generic.kl): A B C X Y Z L1 R1 L2 R2 SELECT START MODE THUMBL THUMBR.

The `hid` JVM needs a few seconds to register the device; the scenario starts
after --settle seconds. Reports are streamed live, so host-side waits are real.

Steps (one at a time, in order):
  wait:<ms>                 sleep on the host
  tap:<button>[:<ms>]       press, hold <ms> (default 120), release
  hold:<button> / up:<button>
  hat:<up|down|left|right|upleft|...>[:<ms>]   hat pressed then centred
  dpad:<up|down|left|right>[:<ms>]             D-pad button press/release
  stick:<lx>,<ly>[,<rx>,<ry>]   -127..127, stays until changed
  trig:<l>,<r>              0..255, stays until changed
  shot:<path.png>           screenshot of the device to a host path
Example: python -I work/scripts/bl2_virtual_gamepad.py hat:down wait:400 tap:A shot:a.png
"""
import argparse
import json
import subprocess
import sys
import time

ADB = r"C:\Users\Xilonen\AppData\Local\Android\Sdk\platform-tools\adb.exe"
BUTTONS = ["A", "B", "C", "X", "Y", "Z", "L1", "R1", "L2", "R2", "SELECT", "START", "MODE", "THUMBL", "THUMBR"]
HAT = {"up": 0, "upright": 1, "right": 2, "downright": 3, "down": 4, "downleft": 5, "left": 6, "upleft": 7}
DPAD = {"up": 0, "down": 1, "right": 2, "left": 3}


def descriptor():
    return [
        0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01,          # Game Pad, report 1
        0x05, 0x09, 0x19, 0x01, 0x29, 0x0F, 0x15, 0x00, 0x25, 0x01,  # Buttons 1..15
        0x75, 0x01, 0x95, 0x0F, 0x81, 0x02,
        0x75, 0x01, 0x95, 0x01, 0x81, 0x03,                      # 1 pad bit
        0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07,          # Hat switch 0..7, null otherwise
        0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14,
        0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x65, 0x00,
        0x75, 0x04, 0x95, 0x01, 0x81, 0x03,                      # 4 pad bits
        0x09, 0x90, 0x09, 0x91, 0x09, 0x92, 0x09, 0x93,          # D-pad Up/Down/Right/Left buttons
        0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02,
        0x75, 0x04, 0x95, 0x01, 0x81, 0x03,                      # 4 pad bits
        0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35,          # X Y Z Rz sticks
        0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02,
        0x05, 0x02, 0x09, 0xC5, 0x09, 0xC4,                      # Brake, Accelerator triggers
        0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
        0xC0]


class Pad:
    def __init__(self, serial, settle):
        self.serial = serial
        self.buttons = 0
        self.hat = 15
        self.dpad = 0
        self.sticks = [0, 0, 0, 0]
        self.triggers = [0, 0]
        subprocess.run([ADB, "-s", serial, "shell", "pkill -f '[c]om.android.commands.hid'"], capture_output=True)
        self.proc = subprocess.Popen([ADB, "-s", serial, "shell", "hid", "-"], stdin=subprocess.PIPE,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.send({"id": 1, "command": "register", "name": "BL2 QA Gamepad", "vid": 0x045E, "pid": 0x028E,
                   "bus": "usb", "descriptor": descriptor()})
        time.sleep(settle)
        self.report()

    def send(self, event):
        self.proc.stdin.write((json.dumps(event) + "\n").encode())
        self.proc.stdin.flush()

    def report(self):
        data = [1, self.buttons & 255, (self.buttons >> 8) & 255, self.hat & 15, self.dpad & 15]
        data += [value & 255 for value in self.sticks] + list(self.triggers)
        self.send({"id": 1, "command": "report", "report": data})

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=5)
        except Exception:  # noqa: BLE001
            self.proc.kill()
        subprocess.run([ADB, "-s", self.serial, "shell", "pkill -f '[c]om.android.commands.hid'"], capture_output=True)


def run(pad, step):
    kind, _, argument = step.partition(":")
    parts = argument.split(":") if argument else []
    if kind == "wait":
        time.sleep(int(argument) / 1000)
    elif kind in ("tap", "hold", "up"):
        bit = 1 << BUTTONS.index(parts[0].upper())
        if kind in ("tap", "hold"):
            pad.buttons |= bit
            pad.report()
        if kind == "tap":
            time.sleep((int(parts[1]) if len(parts) > 1 else 120) / 1000)
        if kind in ("tap", "up"):
            pad.buttons &= ~bit
            pad.report()
    elif kind == "hat":
        pad.hat = HAT[parts[0]]
        pad.report()
        time.sleep((int(parts[1]) if len(parts) > 1 else 120) / 1000)
        pad.hat = 15
        pad.report()
    elif kind == "dpad":
        pad.dpad |= 1 << DPAD[parts[0]]
        pad.report()
        time.sleep((int(parts[1]) if len(parts) > 1 else 120) / 1000)
        pad.dpad = 0
        pad.report()
    elif kind == "stick":
        values = [int(v) for v in argument.split(",")]
        pad.sticks[:len(values)] = [max(-127, min(127, v)) for v in values]
        pad.report()
    elif kind == "trig":
        pad.triggers = [max(0, min(255, int(v))) for v in argument.split(",")]
        pad.report()
    elif kind == "shot":
        with open(argument, "wb") as out:
            out.write(subprocess.run([ADB, "-s", pad.serial, "exec-out", "screencap", "-p"],
                                     check=True, capture_output=True).stdout)
    else:
        raise SystemExit(f"unknown step {step!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--serial", default="61151JEA326613")
    parser.add_argument("--settle", type=float, default=7.0)
    parser.add_argument("--gap", type=int, default=250, help="ms between steps")
    parser.add_argument("steps", nargs="*")
    args = parser.parse_args()
    pad = Pad(args.serial, args.settle)
    try:
        for step in args.steps:
            run(pad, step)
            time.sleep(args.gap / 1000)
            print("done", step, flush=True)
    finally:
        pad.close()


if __name__ == "__main__":
    sys.exit(main())

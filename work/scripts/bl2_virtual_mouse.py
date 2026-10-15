#!/usr/bin/env python3
"""Virtual HID mouse for device QA (uhid + Android `hid` tool, no root).

Registers a 5-button wheel mouse (BTN_LEFT/RIGHT/MIDDLE/SIDE/EXTRA, REL_X/Y,
REL_WHEEL). Android's CursorInputMapper makes it a real SOURCE_MOUSE device
with the system arrow, hover events, clicks, the wheel and pointer capture.

Movement is relative. `home` pushes the arrow into the top-left corner; `to`
then moves slowly (below the velocity-control threshold, so no acceleration)
to display pixel x,y of the landscape screen.

Steps (in order):
  wait:<ms> | home | to:<x>,<y> | move:<dx>,<dy>
  click:<left|right|middle|back|forward>[:<ms>] | down:<button> | up:<button>
  drag:<x>,<y>   (left button held while moving to x,y)
  wheel:<+n|-n>  (positive = away from the user)
  shot:<path.png>
Example: python -I work/scripts/bl2_virtual_mouse.py home to:600,300 wait:300 click:left
"""
import argparse
import json
import subprocess
import sys
import time

ADB = r"C:\Users\Xilonen\AppData\Local\Android\Sdk\platform-tools\adb.exe"
BUTTONS = {"left": 1, "right": 2, "middle": 4, "back": 8, "forward": 16}
STEP = 6          # counts per report while positioning
STEP_MS = 15      # 400 counts/s stays under the default 500 px/s threshold


def descriptor():
    return [
        0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01,          # Mouse, report 1
        0x09, 0x01, 0xA1, 0x00,                                   # Pointer, physical
        0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01,
        0x95, 0x05, 0x75, 0x01, 0x81, 0x02,                       # 5 buttons
        0x95, 0x01, 0x75, 0x03, 0x81, 0x03,                       # 3 pad bits
        0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,           # X, Y, wheel
        0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,  # relative
        0xC0, 0xC0]


class Mouse:
    def __init__(self, serial, settle):
        self.serial = serial
        self.buttons = 0
        self.gain = 1.0
        self.x = self.y = None
        subprocess.run([ADB, "-s", serial, "shell", "pkill -f '[c]om.android.commands.hid'"], capture_output=True)
        self.proc = subprocess.Popen([ADB, "-s", serial, "shell", "hid", "-"], stdin=subprocess.PIPE,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.send({"id": 1, "command": "register", "name": "BL2 QA Mouse", "vid": 0x046D, "pid": 0xC077,
                   "bus": "usb", "descriptor": descriptor()})
        time.sleep(settle)

    def send(self, event):
        self.proc.stdin.write((json.dumps(event) + "\n").encode())
        self.proc.stdin.flush()

    def report(self, dx=0, dy=0, wheel=0):
        self.send({"id": 1, "command": "report", "report": [1, self.buttons, dx & 255, dy & 255, wheel & 255]})

    def move(self, dx, dy, step=STEP, pause=STEP_MS):
        while dx or dy:
            sx = max(-step, min(step, dx))
            sy = max(-step, min(step, dy))
            self.report(sx, sy)
            dx -= sx
            dy -= sy
            if self.x is not None:
                self.x += sx
                self.y += sy
            time.sleep(pause / 1000)

    def home(self):
        self.move(-3000, -3000, step=127, pause=8)
        self.x = self.y = 0

    def to(self, x, y):
        if self.x is None:
            self.home()
        # Android's pointer speed turns counts into more display pixels.
        tx, ty = round(x / self.gain), round(y / self.gain)
        self.move(tx - self.x, ty - self.y)

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=5)
        except Exception:  # noqa: BLE001
            self.proc.kill()
        subprocess.run([ADB, "-s", self.serial, "shell", "pkill -f '[c]om.android.commands.hid'"], capture_output=True)


def run(mouse, step):
    kind, _, argument = step.partition(":")
    parts = argument.split(":") if argument else []
    if kind == "wait":
        time.sleep(int(argument) / 1000)
    elif kind == "home":
        mouse.home()
    elif kind in ("to", "drag"):
        x, y = (int(v) for v in parts[0].split(","))
        if kind == "drag":
            mouse.buttons |= 1
            mouse.report()
        mouse.to(x, y)
        if kind == "drag":
            mouse.buttons &= ~1
            mouse.report()
    elif kind == "move":
        dx, dy = (int(v) for v in parts[0].split(","))
        mouse.move(dx, dy, step=127, pause=8)
    elif kind in ("click", "down", "up"):
        bit = BUTTONS[parts[0]]
        if kind in ("click", "down"):
            mouse.buttons |= bit
            mouse.report()
        if kind == "click":
            time.sleep((int(parts[1]) if len(parts) > 1 else 90) / 1000)
        if kind in ("click", "up"):
            mouse.buttons &= ~bit
            mouse.report()
    elif kind == "wheel":
        notches = int(argument)
        for _ in range(abs(notches)):
            mouse.report(wheel=1 if notches > 0 else -1)
            time.sleep(0.12)
    elif kind == "shot":
        with open(argument, "wb") as out:
            out.write(subprocess.run([ADB, "-s", mouse.serial, "exec-out", "screencap", "-p"],
                                     check=True, capture_output=True).stdout)
    else:
        raise SystemExit(f"unknown step {step!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--serial", default="61151JEA326613")
    parser.add_argument("--settle", type=float, default=7.0)
    parser.add_argument("--gap", type=int, default=200, help="ms between steps")
    parser.add_argument("--gain", type=float, default=2.7,
                        help="display px per count at the slow `to` speed (2.7 on the Pixel 10a, Android 17)")
    parser.add_argument("steps", nargs="*")
    args = parser.parse_args()
    mouse = Mouse(args.serial, args.settle)
    mouse.gain = args.gain
    try:
        for step in args.steps:
            run(mouse, step)
            time.sleep(args.gap / 1000)
            print("done", step, flush=True)
    finally:
        mouse.close()


if __name__ == "__main__":
    sys.exit(main())

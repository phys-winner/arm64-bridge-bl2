#!/usr/bin/env python3
"""Virtual HID keyboard + mouse for device QA (uhid + Android `hid` tool, no root).

One `hid` session registers two devices: a 5-button wheel mouse (id 1, the
same report as bl2_virtual_mouse.py) and a boot-protocol keyboard (id 2).
Android gives both controller number 0, like a real Bluetooth keyboard and
mouse, so keys reach UE3JavaApp.onKeyDown while the mouse is captured.

Steps (in order):
  wait:<ms> | shot:<path.png>
  key:<name>[:<ms>]            press and release
  keydown:<name> | keyup:<name> | keyup:all
  move:<dx>,<dy>[:<ms>]        relative mouse counts, spread over <ms> (default 0)
  click:<left|right|middle>[:<ms>] | down:<button> | up:<button>
  wheel:<+n|-n>
Key names: a-z, 0-9, space, enter, esc, tab, shift, ctrl, alt, up, down, left, right.
Example (hold W, turn, fire):
  python -I work/scripts/bl2_virtual_kbm.py --serial SERIAL keydown:w wait:400 move:300,0:400 click:left:300 keyup:w
"""
import argparse
import json
import subprocess
import sys
import time

ADB = "adb"
BUTTONS = {"left": 1, "right": 2, "middle": 4, "back": 8, "forward": 16}
MODIFIERS = {"ctrl": 0x01, "shift": 0x02, "alt": 0x04}
KEYS = {chr(ord("a") + i): 0x04 + i for i in range(26)}
KEYS.update({str(i): 0x1E + i - 1 for i in range(1, 10)})
KEYS.update({"0": 0x27, "enter": 0x28, "esc": 0x29, "tab": 0x2B, "space": 0x2C,
             "right": 0x4F, "left": 0x50, "down": 0x51, "up": 0x52})


def mouse_descriptor():
    return [
        0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01,
        0x09, 0x01, 0xA1, 0x00,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01,
        0x95, 0x05, 0x75, 0x01, 0x81, 0x02,
        0x95, 0x01, 0x75, 0x03, 0x81, 0x03,
        0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
        0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
        0xC0, 0xC0]


def keyboard_descriptor():
    return [
        0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,                          # Keyboard
        0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,  # 8 modifier bits
        0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
        0x95, 0x01, 0x75, 0x08, 0x81, 0x01,                          # reserved byte
        0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,              # 6 key slots
        0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
        0xC0]


class Devices:
    def __init__(self, serial, settle):
        self.serial = serial
        self.buttons = 0
        self.modifiers = 0
        self.keys = []
        subprocess.run([ADB, "-s", serial, "shell", "pkill -f '[c]om.android.commands.hid'"], capture_output=True)
        self.proc = subprocess.Popen([ADB, "-s", serial, "shell", "hid", "-"], stdin=subprocess.PIPE,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.send({"id": 1, "command": "register", "name": "BL2 QA Mouse", "vid": 0x046D, "pid": 0xC077,
                   "bus": "usb", "descriptor": mouse_descriptor()})
        self.send({"id": 2, "command": "register", "name": "BL2 QA Keyboard", "vid": 0x046D, "pid": 0xC31C,
                   "bus": "usb", "descriptor": keyboard_descriptor()})
        time.sleep(settle)

    def send(self, event):
        self.proc.stdin.write((json.dumps(event) + "\n").encode())
        self.proc.stdin.flush()

    def mouse(self, dx=0, dy=0, wheel=0):
        self.send({"id": 1, "command": "report", "report": [1, self.buttons, dx & 255, dy & 255, wheel & 255]})

    def keyboard(self):
        slots = (self.keys + [0] * 6)[:6]
        self.send({"id": 2, "command": "report", "report": [self.modifiers, 0] + slots})

    def press(self, name, down):
        if name in MODIFIERS:
            bit = MODIFIERS[name]
            self.modifiers = self.modifiers | bit if down else self.modifiers & ~bit
        else:
            code = KEYS[name]
            if down and code not in self.keys:
                self.keys.append(code)
            elif not down and code in self.keys:
                self.keys.remove(code)
        self.keyboard()

    def move(self, dx, dy, ms=0):
        reports = max(1, max(abs(dx), abs(dy)) // 20 + (1 if max(abs(dx), abs(dy)) % 20 else 0))
        pause = ms / 1000 / reports
        sent_x = sent_y = 0
        for i in range(1, reports + 1):
            x = round(dx * i / reports) - sent_x
            y = round(dy * i / reports) - sent_y
            while x or y:
                sx, sy = max(-127, min(127, x)), max(-127, min(127, y))
                self.mouse(sx, sy)
                x -= sx; y -= sy
                sent_x += sx; sent_y += sy
            if pause:
                time.sleep(pause)

    def close(self):
        try:
            self.modifiers = 0
            self.keys = []
            self.keyboard()
            self.buttons = 0
            self.mouse()
            self.proc.stdin.close()
            self.proc.wait(timeout=5)
        except Exception:  # noqa: BLE001
            self.proc.kill()
        subprocess.run([ADB, "-s", self.serial, "shell", "pkill -f '[c]om.android.commands.hid'"], capture_output=True)


def run(devices, step):
    kind, _, argument = step.partition(":")
    parts = argument.split(":") if argument else []
    if kind == "wait":
        time.sleep(int(argument) / 1000)
    elif kind == "key":
        devices.press(parts[0], True)
        time.sleep((int(parts[1]) if len(parts) > 1 else 90) / 1000)
        devices.press(parts[0], False)
    elif kind == "keydown":
        devices.press(parts[0], True)
    elif kind == "keyup":
        if parts[0] == "all":
            devices.modifiers = 0
            devices.keys = []
            devices.keyboard()
        else:
            devices.press(parts[0], False)
    elif kind == "move":
        dx, dy = (int(v) for v in parts[0].split(","))
        devices.move(dx, dy, int(parts[1]) if len(parts) > 1 else 0)
    elif kind in ("click", "down", "up"):
        bit = BUTTONS[parts[0]]
        if kind in ("click", "down"):
            devices.buttons |= bit
            devices.mouse()
        if kind == "click":
            time.sleep((int(parts[1]) if len(parts) > 1 else 90) / 1000)
        if kind in ("click", "up"):
            devices.buttons &= ~bit
            devices.mouse()
    elif kind == "wheel":
        notches = int(argument)
        for _ in range(abs(notches)):
            devices.mouse(wheel=1 if notches > 0 else -1)
            time.sleep(0.12)
    elif kind == "shot":
        with open(argument, "wb") as out:
            out.write(subprocess.run([ADB, "-s", devices.serial, "exec-out", "screencap", "-p"],
                                     check=True, capture_output=True).stdout)
    else:
        raise SystemExit(f"unknown step {step!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--serial", required=True, help="adb device serial")
    parser.add_argument("--settle", type=float, default=7.0)
    parser.add_argument("--gap", type=int, default=150, help="ms between steps")
    parser.add_argument("steps", nargs="*")
    args = parser.parse_args()
    devices = Devices(args.serial, args.settle)
    try:
        for step in args.steps:
            run(devices, step)
            time.sleep(args.gap / 1000)
            print("done", step, flush=True)
    finally:
        devices.close()


if __name__ == "__main__":
    sys.exit(main())

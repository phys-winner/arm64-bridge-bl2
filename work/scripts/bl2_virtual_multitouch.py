#!/usr/bin/env python3
"""Real two-finger touch input for device QA through a virtual HID touchscreen.

`input` injects one pointer and SELinux denies the shell `sendevent` writes to the
panel (/dev/input/event*). The shell user may use /dev/uhid, so Android's `hid`
tool registers a two-contact HID digitizer; hid-multitouch binds it and
InputReader maps it to the internal display ("fallback to internal one").

Coordinates are display pixels of the landscape game (2424x1080 Pixel 10a). The
device reports raw portrait coordinates and the display rotation applies:
rotation 90 (dumpsys input `orientation=1`) maps display (x, y) to raw
(1079 - y, x); rotation 270 maps it to (y, 2423 - x).

The tool spends its first ~5 s starting a JVM and registering the device while
already consuming `delay` commands, so reports queued earlier arrive at once.
Every scenario therefore starts with a 7 s wait.

Steps: wait:<ms> | f1:<x>,<y> | f2:<x>,<y> | up1 | up2   (one report per step)
Example, a stick held by finger 1 while finger 2 drags:
  python -I work/scripts/bl2_virtual_multitouch.py f1:500,850 wait:400 f2:730,700 f2:1000,700 up2 up1
"""
import argparse
import json
import pathlib
import subprocess
import tempfile

ADB = r"C:\Users\Xilonen\AppData\Local\Android\Sdk\platform-tools\adb.exe"
WIDTH, HEIGHT = 1080, 2424  # natural portrait panel


def descriptor():
    finger = [0x09, 0x22, 0xA1, 0x02,                       # Finger, logical collection
              0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,  # Tip Switch
              0x95, 0x07, 0x81, 0x03,                       # 7 padding bits
              0x09, 0x51, 0x25, 0x0F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,  # Contact Identifier
              0x05, 0x01, 0x26, 0x37, 0x04, 0x75, 0x10, 0x95, 0x01, 0x09, 0x30, 0x81, 0x02,  # X 0..1079
              0x26, 0x77, 0x09, 0x09, 0x31, 0x81, 0x02,     # Y 0..2423
              0x05, 0x0D, 0xC0]
    return ([0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01, 0x85, 0x01] + finger + finger +
            [0x09, 0x54, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,  # Contact Count
             0x85, 0x02, 0x09, 0x55, 0x25, 0x02, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x02,  # Contact Count Maximum
             0xC0])


def raw(x, y, rotation):
    return (WIDTH - 1 - y, x) if rotation == 90 else (y, HEIGHT - 1 - x)


def scenario(steps, rotation):
    events = [{"id": 1, "command": "register", "name": "BL2 QA Touch", "vid": 4660, "pid": 22137,
               "bus": "usb", "descriptor": descriptor(), "feature_reports": [{"id": 2, "data": [2, 2]}]},
              {"id": 1, "command": "delay", "duration": 7000}]
    contacts = [None, None]

    def report():
        data = [1]
        for index, contact in enumerate(contacts):
            if contact is None:
                data += [0, index, 0, 0, 0, 0]
            else:
                rx, ry = contact
                data += [1, index, rx & 255, rx >> 8, ry & 255, ry >> 8]
        data.append(sum(contact is not None for contact in contacts))
        events.append({"id": 1, "command": "report", "report": data})

    for step in steps:
        kind, _, argument = step.partition(":")
        if kind == "wait":
            events.append({"id": 1, "command": "delay", "duration": int(argument)})
        elif kind in ("f1", "f2"):
            x, y = (int(value) for value in argument.split(","))
            contacts[int(kind[1]) - 1] = raw(x, y, rotation)
            report()
        elif kind in ("up1", "up2"):
            contacts[int(kind[2]) - 1] = None
            report()
        else:
            raise SystemExit(f"unknown step {step!r}")
    events.append({"id": 1, "command": "delay", "duration": 300})
    return "".join(json.dumps(event) + "\n" for event in events)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--serial", default="61151JEA326613")
    parser.add_argument("--rotation", type=int, choices=(90, 270), default=90)
    parser.add_argument("steps", nargs="+")
    args = parser.parse_args()
    remote = "/data/local/tmp/bl2-virtual-touch.json"
    with tempfile.TemporaryDirectory() as folder:
        local = pathlib.Path(folder) / "scenario.json"
        local.write_text(scenario(args.steps, args.rotation), encoding="utf8")
        subprocess.run([ADB, "-s", args.serial, "push", str(local), remote], check=True, capture_output=True)
    # The bracket keeps pkill from matching this shell's own command line.
    subprocess.run([ADB, "-s", args.serial, "shell",
                    f"pkill -f '[c]om.android.commands.hid'; cat {remote} | hid - >/dev/null 2>&1; rm -f {remote}"],
                   check=True)


if __name__ == "__main__":
    main()

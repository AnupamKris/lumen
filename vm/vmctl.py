#!/usr/bin/env python3
"""Drive the test VM over QMP: screenshots, mouse and keyboard.

  vmctl.py shot out.png            capture the VM display
  vmctl.py move X Y                move pointer (screen pixels)
  vmctl.py click X Y [left|right|middle]
  vmctl.py drag X1 Y1 X2 Y2 [mod]  press at 1, move to 2, release (mod e.g. meta)
  vmctl.py key meta-ret            press a chord (qcodes joined by '-')
  vmctl.py type "hello world"      type text
  vmctl.py scroll X Y up|down [N]
"""
import json, os, socket, sys, time

SOCK = os.path.join(os.path.dirname(os.path.abspath(__file__)), "qmp.sock")
W, H = 1440, 900  # must match virtio-gpu xres/yres in start.sh


class QMP:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(SOCK)
        self.f = self.s.makefile("rw")
        self._read()
        self.cmd("qmp_capabilities")

    def _read(self):
        while True:
            msg = json.loads(self.f.readline())
            if "event" not in msg:
                return msg

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}) + "\n")
        self.f.flush()
        r = self._read()
        if "error" in r:
            sys.exit(f"QMP {name}: {r['error']}")
        return r.get("return")

    def events(self, evs):
        self.cmd("input-send-event", events=evs)


def absmove(x, y):
    return [
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (W - 1))}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (H - 1))}},
    ]


def btn(b, down):
    return {"type": "btn", "data": {"button": b, "down": down}}


def keyev(q, down):
    return {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": q}}}


CHARS = {" ": "spc", "\n": "ret", "-": "minus", "=": "equal", ".": "dot", ",": "comma",
         "/": "slash", ";": "semicolon", "'": "apostrophe", "[": "bracket_left",
         "]": "bracket_right", "\\": "backslash", "`": "grave_accent", "\t": "tab"}
SHIFTED = {"_": "minus", "+": "equal", ">": "dot", "<": "comma", "?": "slash", ":": "semicolon",
           '"': "apostrophe", "{": "bracket_left", "}": "bracket_right", "|": "backslash",
           "~": "grave_accent", "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6",
           "&": "7", "*": "8", "(": "9", ")": "0"}
ALIAS = {"super": "meta_l", "meta": "meta_l", "ctrl": "ctrl", "alt": "alt", "shift": "shift",
         "enter": "ret", "esc": "esc", "space": "spc"}


def chord(q, keys):
    keys = [ALIAS.get(k, k) for k in keys]
    for k in keys:
        q.events([keyev(k, True)])
        time.sleep(0.03)
    for k in reversed(keys):
        q.events([keyev(k, False)])
        time.sleep(0.03)


def main(a):
    q = QMP()
    c = a[0]
    if c == "shot":
        out = os.path.abspath(a[1])
        q.cmd("screendump", filename=out, format="png")
        print(out)
    elif c == "move":
        q.events(absmove(float(a[1]), float(a[2])))
    elif c == "click":
        b = a[3] if len(a) > 3 else "left"
        q.events(absmove(float(a[1]), float(a[2])))
        time.sleep(0.05)
        q.events([btn(b, True)]); time.sleep(0.05); q.events([btn(b, False)])
    elif c == "drag":
        mod = ALIAS.get(a[5], a[5]) if len(a) > 5 else None
        x1, y1, x2, y2 = map(float, a[1:5])
        q.events(absmove(x1, y1)); time.sleep(0.05)
        if mod:
            q.events([keyev(mod, True)]); time.sleep(0.05)
        q.events([btn("left", True)])
        for i in range(1, 21):
            t = i / 20
            q.events(absmove(x1 + (x2 - x1) * t, y1 + (y2 - y1) * t)); time.sleep(0.02)
        q.events([btn("left", False)])
        if mod:
            q.events([keyev(mod, False)])
    elif c == "key":
        chord(q, a[1].split("-") if a[1] != "-" else ["minus"])
    elif c == "type":
        for ch in a[1]:
            if ch in SHIFTED:
                chord(q, ["shift", SHIFTED[ch]])
            elif ch.isupper():
                chord(q, ["shift", ch.lower()])
            else:
                chord(q, [CHARS.get(ch, ch)])
    elif c == "scroll":
        n = int(a[4]) if len(a) > 4 else 1
        q.events(absmove(float(a[1]), float(a[2])))
        b = "wheel-up" if a[3] == "up" else "wheel-down"
        for _ in range(n):
            q.events([btn(b, True)]); q.events([btn(b, False)]); time.sleep(0.03)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:] or ["help"])

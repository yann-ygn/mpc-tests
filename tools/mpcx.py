#!/usr/bin/env python3
"""mpcx: run discovery experiments on an MPC with privtap and the MPC Remote addin installed.

An experiment marks privtap's log, performs actions (button presses through the remote addin, which reach MPC through
privtap's FIFO; screen taps; raw MIDI into the FIFO), waits for MPC to settle, marks the log again, and prints what
MPC sent the hardware in between, decoded: the button LEDs by name, the pad colours, other sysex. Events are ordered by
their place in the log, not by timestamp, so the marks can't drift against privtap's clock.

    mpcx.py exp LABEL ACTION...     run an experiment (actions below), print what changed
    mpcx.py state [MARK]            the LED and pad state at a mark (default: now)
    mpcx.py events [FROM [TO]]      the decoded events between two marks
    mpcx.py marks                   the marks in the log
    mpcx.py oled [N] [MARK]         the MPC X's Q-Link displays as text art, as MPC last drew them (privtap identity=X)
    mpcx.py shot FILE               save a screenshot

Actions: press:NAME (hold:NAME:MS), down:NAME, up:NAME, tap:X,Y, inj:HEX (raw MIDI into the FIFO), dial:N (the data
wheel, N steps, negative to the left), wait:MS,
shot:FILE. Button names are the remote addin's profile (privtap/profiles/mpc-key-37-buttons.conf).

The log is copied incrementally to build/mpcx/ (it holds the device's serial number: keep it out of git).
MPC_HOST sets the device (default mpc-key-37.lan.arpa).
"""
import os
import re
import subprocess
import sys
import time
import urllib.parse
import urllib.request

HOST = os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa")
HTTP = f"http://{HOST}:6720"
LOG = "/tmp/privtap.log"
FIFO = "/dev/midi_inject"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, "build", "mpcx")
PROFILE = os.path.join(ROOT, "privtap", "profiles", "mpc-key-37-buttons.conf")
SSH = ["ssh", "-o", "BatchMode=yes", "-o", "ControlMaster=auto", "-o", f"ControlPath={CACHE}/ssh-%C",
       "-o", "ControlPersist=10m", f"root@{HOST}"]
BLINKERS = {0x35, 0x71}   # LEDs MPC blinks in tempo (TAP, and 113, which has no light on a Key 37)
KEEPALIVE = (0x52, 0x53)  # Akai sysex commands MPC and the hardware exchange every 4 s


def ssh(cmd, data=None):
    os.makedirs(CACHE, exist_ok=True)
    r = subprocess.run(SSH + [cmd], input=data, capture_output=True, check=True)
    return r.stdout


def http(path, **query):
    url = f"{HTTP}{path}" + ("?" + urllib.parse.urlencode(query) if query else "")
    with urllib.request.urlopen(url, timeout=10) as r:
        return r.read()


# ---- the profile: button and pad names

def load_profile():
    names = {}
    for line in open(PROFILE):
        line = line.split("#", 1)[0].strip()
        m = re.match(r"([A-Z0-9-]+)\s*=\s*(\d+)(?:\s+(\d+))?$", line)
        if m:
            names[(int(m.group(3) or 1), int(m.group(2)))] = m.group(1)
    return names


NAMES = load_profile()


def button(note, ch=1):
    return NAMES.get((ch, note), f"note{note}" if ch == 1 else f"ch{ch}:{note}")


# ---- the log

def fetch_log():
    """The device's log, copied incrementally (refetched whole if it was rotated or restarted)."""
    os.makedirs(CACHE, exist_ok=True)
    local = os.path.join(CACHE, "privtap.log")
    have = os.path.getsize(local) if os.path.exists(local) else 0
    size = int(ssh(f"stat -c %s {LOG}").strip())
    if size < have:
        have = 0
    if size > have:
        new = ssh(f"tail -c +{have + 1} {LOG}")
        with open(local, "r+b" if have else "wb") as f:
            f.seek(have)
            f.write(new)
            f.truncate()
    return open(local, encoding="utf-8", errors="replace").read().splitlines()


LINE = re.compile(r"^(\d+\.\d+) (in|out|inj) +(.+?): ((?:[0-9A-F]{2} ?)+)$")
EVENT = re.compile(r"^(\S+) # (.*)$")


def data_len(s):
    hi = s & 0xF0
    if hi in (0xC0, 0xD0):
        return 1
    if hi == 0xF0:
        return 2 if s == 0xF2 else 1 if s in (0xF1, 0xF3) else 0
    return 2


class Stream:
    """Turns one port's bytes, split however the reads split them, into whole MIDI messages."""

    def __init__(self):
        self.msg, self.need, self.run, self.sysex = [], 0, 0, False

    def feed(self, b):
        out = []
        for x in b:
            if x >= 0xF8:
                out.append([x])
            elif self.sysex:
                self.msg.append(x)
                if x == 0xF7:
                    out.append(self.msg)
                    self.msg, self.sysex = [], False
                elif x >= 0x80:   # a status byte ends a sysex
                    self.msg, self.sysex = [], False
                    out += self.feed([x])
            elif x == 0xF0:
                self.msg, self.sysex, self.run = [x], True, 0
            elif x >= 0x80:
                self.run = x if x < 0xF0 else 0
                self.msg, self.need = [x], data_len(x)
                if not self.need:
                    out.append(self.msg)
                    self.msg = []
            else:
                if not self.msg:
                    if not self.run:
                        continue
                    self.msg, self.need = [self.run], data_len(self.run)
                self.msg.append(x)
                if len(self.msg) == 1 + self.need:
                    out.append(self.msg)
                    self.msg = []
        return out


def parse(lines):
    """Yield (time, kind, ...) for each line: ("mark", label), ("event", text), ("msg", dir, port, bytes)."""
    streams = {}
    for line in lines:
        m = LINE.match(line)
        if m:
            key = (m.group(2), m.group(3))
            for msg in streams.setdefault(key, Stream()).feed(bytes.fromhex(m.group(4))):
                yield float(m.group(1)), "msg", m.group(2), m.group(3), msg
            continue
        e = EVENT.match(line)
        if e:
            text = e.group(2)
            yield (float(e.group(1)), "mark", text[1:].strip()) if text.startswith("@") else \
                (float(e.group(1)), "event", text)


# ---- decoding

def akai(msg):
    """(command, payload) of an Akai sysex (F0 47 dev family cmd lenH lenL payload F7), else None. The family is the
    Key 37's 57, or another model's when privtap's identity= is set (the log keeps MPC's bytes)."""
    if len(msg) >= 8 and msg[0] == 0xF0 and msg[1] == 0x47 and msg[-1] == 0xF7:
        return msg[4], msg[7:-1]
    return None


def hexs(b):
    return " ".join(f"{x:02X}" for x in b)


BLINK_WINDOW = 1.5   # an LED or pad that changed twice or more in this many seconds is blinking


def label(key):
    return f"LED {button(key[2], key[1])}" if key[0] == "led" else f"pad[{key[1]}]"


class State:
    """What MPC last showed on the hardware: LEDs ("led", channel, note) and pads ("pad", index), with recent changes."""

    def __init__(self):
        self.val, self.lit, self.sysex, self.changed = {}, {}, {}, {}
        self.now = 0.0

    def blinking(self, key):
        return len([t for t in self.changed.get(key, []) if self.now - t <= BLINK_WINDOW]) >= 2

    def show(self, key):
        if key not in self.val:
            return "?"
        if self.blinking(key):
            lit = self.lit.get(key)
            return "blinking" + (f" {hexs(lit)}" if key[0] == "pad" and lit else "")
        v = self.val[key]
        return f"{v:02X}" if key[0] == "led" else hexs(v)

    def set(self, t, key, v, show):
        old = self.val.get(key)
        self.val[key] = v
        if v != (0, 0, 0) and v != 0:
            self.lit[key] = v
        if old == v:
            return None
        self.changed[key] = (self.changed.get(key, []) + [t])[-4:]
        if key[0] == "led" and key[2] in BLINKERS:
            return None
        return (key, f"{label(key)} {'?' if old is None else show(old)} -> {show(v)}")

    def apply(self, t, d, port, msg):
        """Fold an outgoing message into the state; return what is worth listing, else None."""
        self.now = t
        if d != "out":
            return None
        if msg[0] & 0xF0 == 0xB0 and len(msg) == 3:
            return self.set(t, ("led", (msg[0] & 15) + 1, msg[1]), msg[2], lambda v: f"{v:02X}")
        a = akai(msg)
        if a and a[0] == 0x65 and len(a[1]) == 4:
            return self.set(t, ("pad", a[1][0]), tuple(a[1][1:]), hexs)
        if a and a[0] in KEEPALIVE:
            return None
        key = (port, a[0]) if a else (port, "raw")
        changed = self.sysex.get(key) != msg
        self.sysex[key] = msg
        if not changed:
            return None
        return f"{port}: sysex {a[0]:02X}: {hexs(a[1])}" if a else f"{port}: {hexs(msg)}"

    def keys(self):
        return sorted(self.val, key=lambda k: (k[0] != "led",) + k[1:])

    def copy(self):
        s = State()
        s.val, s.lit, s.sysex, s.changed, s.now = dict(self.val), dict(self.lit), dict(self.sysex), \
            dict(self.changed), self.now
        return s


def describe_input(d, msg):
    if len(msg) == 3 and msg[0] & 0xF0 in (0x80, 0x90):
        ch, on = (msg[0] & 15) + 1, msg[0] & 0xF0 == 0x90 and msg[2] > 0
        return f"{'inj' if d == 'inj' else 'hw '} {button(msg[1], ch)} {'down' if on else 'up'}" + \
            (f" v{msg[2]}" if on and ch != 1 else "")
    a = akai(msg)
    if a and a[0] in KEEPALIVE:
        return None
    return f"{'inj' if d == 'inj' else 'hw '} {hexs(msg)}"


def fold_blinks(events):
    """An LED or pad that changes three times or more in the list is shown once, as blinking."""
    counts = {}
    for e in events:
        if isinstance(e, tuple):
            counts[e[0]] = counts.get(e[0], 0) + 1
    out, shown = [], set()
    for e in events:
        if not isinstance(e, tuple):
            out.append(e)
        elif counts[e[0]] < 3:
            out.append(e[1])
        elif e[0] not in shown:
            shown.add(e[0])
            out.append(f"{label(e[0])} blinking ({counts[e[0]]} changes)")
    return out


def scan(lines, start=None, end=None):
    """The state at the start mark, the state at the end mark, and the events listed in between."""
    st, before, events, inside = State(), None, [], start is None
    for item in parse(lines):
        t, kind = item[0], item[1]
        if kind == "mark":
            if item[2] == start:
                st.now = max(st.now, t)
                before, inside, events = st.copy(), True, []
            elif item[2] == end and inside:
                st.now = max(st.now, t)
                return before, st, fold_blinks(events)
            continue
        if kind == "event":
            if inside:
                events.append(f"# {item[2]}")
            continue
        d, port, msg = item[2], item[3], item[4]
        text = st.apply(t, d, port, msg) if d == "out" else describe_input(d, msg)
        if inside and text:
            events.append(text)
    return before or State(), st, fold_blinks(events)


def diff(a, b):
    return [f"{label(k)}: {a.show(k)} -> {b.show(k)}" for k in sorted(set(a.val) | set(b.val), key=lambda k: (k[0] != "led",) + k[1:])
            if not (k[0] == "led" and k[2] in BLINKERS) and a.show(k) != b.show(k)]


# ---- the MPC X's Q-Link displays (sysex 0B, sent when privtap passes the device off as an MPC X)

OLED_W, OLED_PAGES = 128, 4   # 128 x 32 pixels, monochrome, in pages of 8 rows (SSD1306 style)


def unpack7(data):
    """8-bit bytes sent as a stream of 7-bit MIDI data bytes, most significant bit first."""
    bits = "".join(f"{b & 0x7F:07b}" for b in data)
    return [int(bits[i:i + 8], 2) for i in range(0, len(bits) - 7, 8)]


def oled_screens(lines, until=None):
    """Display number -> its pages of columns, from every 0B message (display, first page, last page, first column,
    last column, then the pixels: one byte per column of 8 rows, bit 0 on top) up to the mark until."""
    screens = {}
    for item in parse(lines):
        if item[1] == "mark" and item[2] == until:
            break
        if item[1] != "msg" or item[2] != "out":
            continue
        a = akai(item[4])
        if not a or a[0] != 0x0B or len(a[1]) < 5:
            continue
        d, ps, pe, cs, ce = a[1][:5]
        raw, i = unpack7(a[1][5:]), 0
        fb = screens.setdefault(d, [[0] * OLED_W for _ in range(OLED_PAGES)])
        for page in range(ps, pe + 1):
            for col in range(cs, ce + 1):
                if i < len(raw) and page < OLED_PAGES and col < OLED_W:
                    fb[page][col] = raw[i]
                i += 1
    return screens


def oled_text(fb):
    return ["".join("#" if fb[page][c] >> bit & 1 else " " for c in range(OLED_W)).rstrip()
            for page in range(OLED_PAGES) for bit in range(8)]


# ---- actions

def mark(label):
    ssh(f'echo "$(cut -d" " -f1 /proc/uptime) # @{label}" >> {LOG}')


def act(a):
    kind, _, arg = a.partition(":")
    if kind == "press":
        http("/button", name=arg)
    elif kind == "hold":
        name, _, ms = arg.partition(":")
        http("/button", name=name, hold=ms or "500")
    elif kind in ("down", "up"):
        http("/button", name=arg, action=kind)
    elif kind == "tap":
        x, _, y = arg.partition(",")
        http("/tap", x=x, y=y)
    elif kind == "inj":
        ssh(f"cat > {FIFO}", bytes.fromhex(arg))
    elif kind == "dial":   # the data wheel: relative CC 100 on channel 1, one message per step
        n = int(arg)
        ssh(f"cat > {FIFO}", bytes([0xB0, 0x64, 1 if n > 0 else 0x7F]) * abs(n))
    elif kind == "wait":
        time.sleep(int(arg) / 1000)
    elif kind == "shot":
        with open(arg, "wb") as f:
            f.write(http("/screen.png"))
    else:
        sys.exit(f"unknown action {a}")


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    cmd = argv[1]
    if cmd == "exp":
        title, actions = argv[2], argv[3:]
        settle = int(os.environ.get("MPCX_SETTLE", "1500")) / 1000
        stamp = time.strftime("%H%M%S")
        start, end = f"{title} {stamp} start", f"{title} {stamp} end"
        mark(start)
        time.sleep(0.3)
        for a in actions:
            act(a)
        time.sleep(settle)
        mark(end)
        before, after, events = scan(fetch_log(), start, end)
        print(f"== {title}: {' '.join(actions)}")
        changes = diff(before, after)
        print("changed:" if changes else "changed: nothing", *[f"  {c}" for c in changes], sep="\n")
        if events:
            print("events:", *[f"  {e}" for e in events], sep="\n")
    elif cmd == "state":
        lines = fetch_log()
        before, st, _ = scan(lines, argv[2] if len(argv) > 2 else None, None)
        st = before if len(argv) > 2 else st
        for key in st.keys():
            print(f"{label(key)} = {st.show(key)}")
    elif cmd == "events":
        _, _, events = scan(fetch_log(), argv[2] if len(argv) > 2 else None, argv[3] if len(argv) > 3 else None)
        print(*events, sep="\n")
    elif cmd == "oled":
        screens = oled_screens(fetch_log(), argv[3] if len(argv) > 3 else None)
        for d in sorted(screens):
            if len(argv) > 2 and argv[2] != "all" and int(argv[2], 0) != d:
                continue
            print(f"--- display {d:02X}", *oled_text(screens[d]), sep="\n")
    elif cmd == "marks":
        print(*[item[2] for item in parse(fetch_log()) if item[1] == "mark"], sep="\n")
    elif cmd == "shot":
        act(f"shot:{argv[2]}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)

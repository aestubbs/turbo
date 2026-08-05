#!/usr/bin/env python3
"""PTY runtime test: the mouse wheel acts on the window UNDER THE CURSOR, not the
focused one (D27).

Two windows are tiled: an editor (left) and the file tree (right). The wheel is
sent over one while the other holds focus, both ways round:

  1. editor focused, wheel over the tree   -> the tree scrolls, the editor stays;
  2. tree focused,   wheel over the editor -> the editor scrolls, the tree stays.

This is the cross-window case; the intra-window case (two panes in one window)
lives in spec_scroll_test.py. Driven under a real pty with SGR wheel events.

Usage: python3 test/pty/wheel_test.py [path-to-turboIDE]
"""
import fcntl, os, pty, re, select, signal, struct, sys, termios, time, \
       shutil, tempfile

_CUP = re.compile(rb"\x1b\[(\d*);(\d*)H")
def build_grid(raw):
    g = {}; row = col = 1; i = 0; n = len(raw)
    while i < n:
        b = raw[i]
        if b == 0x1b:
            m = _CUP.match(raw, i)
            if m: row = int(m.group(1) or 1); col = int(m.group(2) or 1); i = m.end(); continue
            m2 = re.match(rb"\x1b\[[0-9;:?]*[A-Za-z]", raw[i:])
            if m2: i += m2.end(); continue
            m3 = re.match(rb"\x1b\][^\x07]*\x07", raw[i:])
            if m3: i += m3.end(); continue
            i += 2; continue
        if b == 0x0a: row += 1; col = 1; i += 1; continue
        if b == 0x0d: col = 1; i += 1; continue
        if b < 0x80: ch = chr(b); i += 1
        elif b >= 0xe0: ch = raw[i:i+3].decode("utf-8", "replace"); i += 3
        elif b >= 0xc0: ch = raw[i:i+2].decode("utf-8", "replace"); i += 2
        else: i += 1; continue
        g[(row, col)] = ch; col += 1
    return g
def rows(g, w=140):
    rr = {}
    for (r, c), ch in g.items(): rr.setdefault(r, {})[c] = ch
    return {r: "".join(cs.get(c, " ") for c in range(1, w + 1)) for r, cs in rr.items()}
def row_of(g, needle):
    for r, l in rows(g).items():
        if needle in l: return r
    return -1
def col_of(g, needle):
    for r, l in rows(g).items():
        i = l.find(needle)
        if i >= 0: return i + 1
    return -1
def wheel(down, col, row):
    return ("\x1b[<%d;%d;%dM" % (65 if down else 64, col, row)).encode()
def click(col, row):
    return ("\x1b[<0;%d;%dM\x1b[<0;%d;%dm" % (col, row, col, row)).encode()

REPO = os.path.realpath(os.path.join(os.path.dirname(__file__), "..", ".."))
TURBO = os.path.realpath(sys.argv[1] if len(sys.argv) > 1
                         else os.path.join(REPO, "turboIDE"))
PROJ = os.path.realpath(tempfile.mkdtemp(prefix="turbo-wheel-"))
os.makedirs(PROJ + "/.turbo")
for i in range(40):
    open(PROJ + "/file_%02d.txt" % i, "w").write("x\n")
open(PROJ + "/LONGDOC.txt", "w").write("\n".join("EDITLINE %03d" % i for i in range(120)) + "\n")

pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"; os.environ["COLORTERM"] = "truecolor"
    os.environ.pop("TURBO_TEST", None); os.environ["TURBO_NO_AUTO_SPEC_AGENT"] = "1"
    os.chdir(PROJ); os.execv(TURBO, [TURBO, PROJ]); os._exit(1)
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 140, 0, 0))
out = bytearray()
def drain(s):
    e = time.time() + s
    while time.time() < e:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try: out.extend(os.read(fd, 65536))
            except OSError: break
def send(d, g=0.08):
    for ch in d:
        os.write(fd, ch if isinstance(ch, bytes) else ch.encode()); drain(g)

results = []
def check(name, ok):
    results.append((name, ok)); print(("PASS " if ok else "FAIL ") + name)

drain(3.0)
# Open the long doc -> editor focused, tiled left of the tree.
send(["\x10"]); drain(0.6); send(list("LONGDOC")); drain(0.8); send(["\r"]); drain(2.0)
g = build_grid(bytes(out))
tcol = col_of(g, "file_")               # a column inside the tree
ecol = max(6, col_of(g, "EDITLINE") if col_of(g, "EDITLINE") > 0 else 20)
tree_top0 = next(("file_%02d" % i for i in range(40) if row_of(g, "file_%02d" % i) > 0), None)
ed_000_row0 = row_of(g, "EDITLINE 000")
check("setup: tree and editor both visible", tcol > 0 and ed_000_row0 > 0)

# (1) editor focused, wheel over the TREE.
for _ in range(5):
    send([wheel(True, tcol, 15)]); drain(0.15)
drain(0.4)
g1 = build_grid(bytes(out))
tree_top1 = next(("file_%02d" % i for i in range(40) if row_of(g1, "file_%02d" % i) > 0), None)
check("wheel over tree scrolls the tree (not the focused editor)",
      tree_top1 is not None and tree_top1 != tree_top0)
check("wheel over tree leaves the focused editor put",
      row_of(g1, "EDITLINE 000") == ed_000_row0)

# (2) focus the tree (click a row), then wheel over the EDITOR.
send([click(tcol, 12)]); drain(0.6)
g2 = build_grid(bytes(out))
ed_000_row2 = row_of(g2, "EDITLINE 000")
tree_top2 = next(("file_%02d" % i for i in range(40) if row_of(g2, "file_%02d" % i) > 0), None)
for _ in range(5):
    send([wheel(True, ecol, 15)]); drain(0.15)
drain(0.4)
g3 = build_grid(bytes(out))
check("wheel over editor scrolls the editor (not the focused tree)",
      ed_000_row2 > 0 and row_of(g3, "EDITLINE 000") != ed_000_row2)
check("wheel over editor leaves the focused tree put",
      tree_top2 is not None and
      next(("file_%02d" % i for i in range(40) if row_of(g3, "file_%02d" % i) > 0), None) == tree_top2)

try:
    send(["\x11"]); drain(0.4); send(["n"]); drain(0.4)
except OSError: pass
os.kill(pid, signal.SIGKILL)
try: os.waitpid(pid, 0)
except ChildProcessError: pass
os.close(fd); shutil.rmtree(PROJ, ignore_errors=True)
failed = [n for n, ok in results if not ok]
print()
print("%d/%d passed" % (len(results) - len(failed), len(results)))
sys.exit(1 if failed else 0)

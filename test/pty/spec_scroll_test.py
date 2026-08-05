#!/usr/bin/env python3
"""PTY runtime test: mouse-wheel routing (D27).

The wheel must act on the window/control *under the cursor*, not the focused
one. Two things are proven, both in a Spec Workbench window (document left,
agent conversation right, one window, two scroll regions):

  1. With focus in the document, wheeling over the *conversation* scrolls the
     conversation and leaves the document put -- i.e. the wheel followed the
     cursor across the divider, and the two panes' scrollbars are told apart.
  2. Wheeling over the document scrolls the document.

Driven against a scripted fake agent (no LLM). Harness conventions match
spec_workbench_test.py: a real pty, discrete key/SGR writes, a reconstructed
screen grid (tvision repaints differentially, so the grid is fed the cumulative
stream and later writes win).

Usage: python3 test/pty/spec_scroll_test.py [path-to-turboIDE]
"""
import fcntl, os, pty, re, select, signal, struct, sys, termios, time, \
       shutil, tempfile

_CUP = re.compile(rb"\x1b\[(\d*);(\d*)H")
def build_grid(raw):
    grid = {}
    row = col = 1
    i, n = 0, len(raw)
    while i < n:
        b = raw[i]
        if b == 0x1b:
            m = _CUP.match(raw, i)
            if m:
                row = int(m.group(1) or 1); col = int(m.group(2) or 1)
                i = m.end(); continue
            m2 = re.match(rb"\x1b\[[0-9;:?]*[A-Za-z]", raw[i:])
            if m2: i += m2.end(); continue
            m3 = re.match(rb"\x1b\][^\x07]*\x07", raw[i:])
            if m3: i += m3.end(); continue
            i += 2; continue
        if b == 0x0a: row += 1; col = 1; i += 1; continue
        if b == 0x0d: col = 1; i += 1; continue
        if b < 0x80: ch = chr(b); i += 1
        elif b >= 0xf0: ch = raw[i:i+4].decode("utf-8", "replace"); i += 4
        elif b >= 0xe0: ch = raw[i:i+3].decode("utf-8", "replace"); i += 3
        elif b >= 0xc0: ch = raw[i:i+2].decode("utf-8", "replace"); i += 2
        else: i += 1; continue
        grid[(row, col)] = ch; col += 1
    return grid

def rows_of(grid, width):
    rows = {}
    for (r, c), ch in grid.items():
        rows.setdefault(r, {})[c] = ch
    return {r: "".join(cols.get(c, " ") for c in range(1, width + 1))
            for r, cols in rows.items()}

def vbar_run(grid, col):
    return sum(1 for (r, c), ch in grid.items() if c == col and ch == "│")

# The Workbench divider: the tall interior vertical bar with conversation text
# to its right (window frames have no pane text beside them).
_PANE_TOKENS = ("REPLY", "Agent ready", "Resuming", "brief")
def divider_col(grid, width, height=40):
    rws = rows_of(grid, width)
    pane_left = width + 1
    for r, line in rws.items():
        if r < 3 or r > height - 2:
            continue
        for tok in _PANE_TOKENS:
            idx = line.find(tok)
            if idx >= 0:
                pane_left = min(pane_left, idx + 1)
    if pane_left > width:
        return -1
    best = -1
    for c in range(2, pane_left):
        if vbar_run(grid, c) >= 20 and c > best:
            best = c
    return best

# Squashed text of a screen region (rows x cols), whitespace-free so tvision's
# differential repaint (unchanged space cells are never rewritten) doesn't skew
# the comparison. Scrolling a pane changes its region's text.
def region_text(grid, width, c0, c1, r0=3, r1=39):
    rws = rows_of(grid, width)
    out = []
    for r in range(r0, r1):
        out.append(rws.get(r, "")[c0:c1])
    return re.sub(r"\s+", "", "".join(out))

# The conversation sits right of the divider.
def chat_text(grid, width, split):
    return region_text(grid, width, split, width)

# The screen row a needle sits on, or -1. A body line (unique to the document,
# absent from the conversation) whose row is stable proves the document did not
# scroll, and whose row moved proves it did -- robust to the differential-repaint
# reconstruction, unlike an exact region-string compare.
def anchor_row(grid, width, needle):
    for r, line in rows_of(grid, width).items():
        if needle in line:
            return r
    return -1

def sgr_wheel(down, col, row):
    # SGR mouse: button 64 = wheel up, 65 = wheel down; one-shot 'press'.
    return ("\x1b[<%d;%d;%dM" % (65 if down else 64, col, row)).encode()

REPO = os.path.realpath(os.path.join(os.path.dirname(__file__), "..", ".."))
TURBO = os.path.realpath(sys.argv[1] if len(sys.argv) > 1
                         else os.path.join(REPO, "turboIDE"))
PROJ = os.path.realpath(tempfile.mkdtemp(prefix="turbo-scroll-"))

# A unique marker near the top of the document body -- present on first open,
# and scrolled off once the document scrolls down. Distinct from its neighbours
# so the differential repaint rewrites its whole row (unlike a run of look-alike
# lines, where only the digits change and stale text lingers in the grid).
DOC_ANCHOR = "ZZDOCANCHORZZ"

FAKE_AGENT = '''#!/usr/bin/env python3
import json, sys
def emit(o):
    sys.stdout.write(json.dumps(o) + "\\n"); sys.stdout.flush()
emit({"type": "system", "subtype": "init",
      "session_id": "fake", "model": "fake"})
for line in sys.stdin:
    if not line.strip():
        continue
    # A long, multi-line reply so the transcript is comfortably scrollable.
    for k in range(40):
        emit({"type": "assistant",
              "message": {"content": [{"type": "text",
                                       "text": "REPLYLINE %02d ................" % k}]}})
    emit({"type": "result", "subtype": "success", "is_error": False,
          "result": "done"})
'''

SPEC = "---\ntitle: Scroll Spec\nstatus: draft\ndomain: test\nupdated: 2026-07-20\n---\n\n# Background\n\n" \
       + "ZZDOCANCHORZZ marks the top of the body.\n" \
       + "\n".join("source line %02d ................." % i for i in range(80)) + "\n"

def setup():
    os.makedirs(PROJ + "/specs", exist_ok=True)
    os.makedirs(PROJ + "/.turbo", exist_ok=True)
    agent = PROJ + "/fake_agent.py"
    with open(agent, "w") as f:
        f.write(FAKE_AGENT)
    os.chmod(agent, 0o755)
    with open(PROJ + "/.turbo/config.json", "w") as f:
        f.write('{"agent": "%s"}\n' % agent)
    with open(PROJ + "/specs/scroll.md", "w") as f:
        f.write(SPEC)

def run_session(actions):
    try:
        os.remove(PROJ + "/.turbo/session")
    except OSError:
        pass
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.environ["COLORTERM"] = "truecolor"
        os.environ.pop("TURBO_TEST", None)
        os.chdir(PROJ)
        os.execv(TURBO, [TURBO, PROJ])
        os._exit(1)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 140, 0, 0))
    out = bytearray()

    def drain(secs):
        end = time.time() + secs
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try:
                    out.extend(os.read(fd, 65536))
                except OSError:
                    break

    def send(data, gap=0.08):
        for ch in data:
            os.write(fd, ch if isinstance(ch, bytes) else ch.encode())
            drain(gap)

    drain(3.0)
    try:
        actions(send, drain, out)
    finally:
        try:
            send(["\x11"]); drain(0.5); send(["n"]); drain(0.5)
        except OSError:
            pass
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            os.waitpid(pid, 0)
        except ChildProcessError:
            pass
        os.close(fd)
    return bytes(out)

results = []
def check(name, ok):
    results.append((name, ok))
    print(("PASS " if ok else "FAIL ") + name)

setup()
marks = {}

def _scroll_actions(send, drain, out):
    # Open the spec -> the Workbench docks itself (D25). Focus starts in the
    # document (it is primary).
    send(["\x10"]); drain(0.6)
    send(list("scroll")); drain(0.8)
    send(["\r"]); drain(4.5)
    g0 = build_grid(bytes(out))
    split = divider_col(g0, 140)
    marks["split"] = split
    marks["doc0"] = anchor_row(g0, 140, DOC_ANCHOR) if split > 0 else -1
    marks["chat0"] = chat_text(g0, 140, split) if split > 0 else ""
    if split > 0:
        chat_col = split + 8            # a column well inside the conversation
        doc_col = max(8, split // 2)    # a column inside the document
        row = 12
        # (1) Wheel over the conversation while the *document* holds focus.
        for _ in range(5):
            send([sgr_wheel(False, chat_col, row)]); drain(0.15)   # wheel up
        drain(0.4)
        marks["after_chat_wheel"] = len(out)
        # (2) Wheel over the document.
        for _ in range(4):
            send([sgr_wheel(True, doc_col, row)]); drain(0.15)     # wheel down
        drain(0.4)
        marks["after_doc_wheel"] = len(out)

raw = run_session(_scroll_actions)

split = marks.get("split", -1)
check("scroll: workbench docked and divider found", split > 0)

g_chat = build_grid(raw[:marks.get("after_chat_wheel", len(raw))])
g_doc = build_grid(raw[:marks.get("after_doc_wheel", len(raw))])

chat0 = marks.get("chat0", "")
doc0 = marks.get("doc0", -1)
chat_after_chatwheel = chat_text(g_chat, 140, split) if split > 0 else ""
doc_after_chatwheel = anchor_row(g_chat, 140, DOC_ANCHOR) if split > 0 else -1

# (1) Wheeling over the conversation (document focused) scrolled the
# conversation -- the wheel followed the cursor, not the focus.
check("scroll: wheel over conversation scrolls the conversation",
      bool(chat0) and chat_after_chatwheel != chat0)
# ...and left the document exactly where it was (the two panes' scrollbars were
# told apart; the wheel did not leak to the document's bar).
check("scroll: wheel over conversation leaves the document put",
      doc0 > 0 and doc_after_chatwheel == doc0)

# (2) Wheeling over the document scrolls the document (the anchor line moved).
doc_after_docwheel = anchor_row(g_doc, 140, DOC_ANCHOR) if split > 0 else -1
check("scroll: wheel over the document scrolls the document",
      doc0 > 0 and doc_after_docwheel != doc0)

shutil.rmtree(PROJ, ignore_errors=True)
failed = [n for n, ok in results if not ok]
print()
print("%d/%d passed" % (len(results) - len(failed), len(results)))
sys.exit(1 if failed else 0)

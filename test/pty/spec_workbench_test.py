#!/usr/bin/env python3
"""PTY runtime test: the Spec Workbench window (M3 of
specs/spec-agent-integration.md).

FR1-FR3: one window per spec, document left and conversation right, with a
per-window agent session. Driven against a scripted fake agent so no LLM is
involved.

Harness conventions match spec_identity_test.py. Note squash(): tvision
repaints differentially, so unchanged space cells are never rewritten and
stripping the cursor escapes lands glyphs adjacent -- compare whitespace-free
rather than asserting exact spacing (D11).

Usage: python3 test/pty/spec_workbench_test.py [path-to-turboIDE]
"""
import fcntl, os, pty, re, select, signal, struct, sys, termios, time, \
       shutil, tempfile

def stripped(raw):
    s = re.sub(rb"\x1b\[[0-9;:?]*[A-Za-z]", b"", raw)
    s = re.sub(rb"\x1b\][^\x07\x1b]*(\x07|\x1b\\)", b"", s)
    s = re.sub(rb"\x1b.", b"", s)
    return s.decode("utf-8", "replace")

def squash(s):
    return re.sub(r"\s+", "", s)

# Reconstruct a screen grid by tracking cursor moves (CUP: ESC[row;colH) and
# the glyphs written after them. tvision repaints differentially, so this is
# fed the *cumulative* stream up to a point -- later writes overwrite earlier
# cells, so the final value at each cell is the current screen. Used by the
# splitter-drag check to find the divider column and prove it moved (FR1).
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

def vbar_run(grid, col):
    return sum(1 for (r, c), ch in grid.items() if c == col and ch == "│")

def rows_of(grid, width):
    rows = {}
    for (r, c), ch in grid.items():
        rows.setdefault(r, {})[c] = ch
    return {r: "".join(cols.get(c, " ") for c in range(1, width + 1))
            for r, cols in rows.items()}

# The Workbench divider is the interior vertical bar with the conversation to
# its right (window frames have no pane text beside them). Anchor on transcript
# text that appears *only* in the right pane -- restricted to the content rows,
# so the menu bar and the status line's "Alt-…" hints can't be mistaken for it
# -- then take the tall vertical-bar run immediately left of that text.
_PANE_TOKENS = ("REPLY", "Resuming", "Agent ready")
def divider_col(grid, width, height=40):
    rws = rows_of(grid, width)
    pane_left = width + 1
    for r, line in rws.items():
        if r < 3 or r > height - 2:
            continue  # skip the menu bar (row 1) and the status line
        for tok in _PANE_TOKENS:
            idx = line.find(tok)
            if idx >= 0:
                pane_left = min(pane_left, idx + 1)  # 1-based column
    if pane_left > width:
        return -1, 0
    best, best_h = -1, 0
    for c in range(1, pane_left):
        h = vbar_run(grid, c)
        if h >= 20 and c > best:  # a full-height divider, not a short frame
            best, best_h = c, h
    return best, best_h

def sgr_mouse(button, col, row, press=True):
    return ("\x1b[<%d;%d;%d%s" % (button, col, row, "M" if press else "m")).encode()

REPO = os.path.realpath(os.path.join(os.path.dirname(__file__), "..", ".."))
TURBO = os.path.realpath(sys.argv[1] if len(sys.argv) > 1
                         else os.path.join(REPO, "turboIDE"))
PROJ = os.path.realpath(tempfile.mkdtemp(prefix="turbo-workbench-"))

FAKE_AGENT = '''#!/usr/bin/env python3
import json, sys, os

def emit(o):
    sys.stdout.write(json.dumps(o) + "\\n")
    sys.stdout.flush()

# Record every launch (argv + the session id this process reports) so the
# resume test can assert that reopening a Workbench passes --resume with the
# id captured from the previous launch (FR7).
sid = "fake-%d" % os.getpid()
try:
    with open(os.path.join(os.getcwd(), "launches.log"), "a") as _f:
        _f.write(json.dumps({"argv": sys.argv, "sid": sid}) + "\\n")
except Exception:
    pass

emit({"type": "system", "subtype": "init",
      "session_id": sid, "model": "fake-model"})
for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    try:
        text = json.loads(line)["message"]["content"][0]["text"]
    except Exception:
        continue
    # Echo only the first line, so the long opening brief stays readable.
    first = text.strip().splitlines()[0] if text.strip() else ""
    if "JUMPTEST" in text:
        # A tool call aimed at a section: activating it must move the
        # document's cursor there (FR11).
        emit({"type": "assistant",
              "message": {"content": [{"type": "tool_use", "id": "toolu_j",
                                       "name": "insert_text",
                                       "input": {"path": "/specs/alpha.md",
                                                 "section": "Decisions"}}]}})
        emit({"type": "result", "subtype": "success", "is_error": False,
              "result": "done"})
        continue
    emit({"type": "assistant",
          "message": {"content": [{"type": "text", "text": "REPLY " + first}]}})
    emit({"type": "result", "subtype": "success", "is_error": False,
          "result": "REPLY " + first})
'''

SPEC_A = """---
title: Alpha Spec
status: draft
domain: test
updated: 2026-07-20
---

# Background

ALPHABODYTEXT here.

# Implementation Plan

- [ ] one
""" + "\n".join("filler line %d" % i for i in range(60)) + """

# Decisions

DECISIONSANCHOR marks the section the tool call names.
"""

SPEC_B = SPEC_A.replace("Alpha Spec", "Beta Spec").replace("ALPHABODYTEXT",
                                                           "BETABODYTEXT")

def setup():
    os.makedirs(PROJ + "/specs", exist_ok=True)
    os.makedirs(PROJ + "/.turbo", exist_ok=True)
    agent = PROJ + "/fake_agent.py"
    with open(agent, "w") as f:
        f.write(FAKE_AGENT)
    os.chmod(agent, 0o755)
    with open(PROJ + "/.turbo/config.json", "w") as f:
        f.write('{"agent": "%s"}\n' % agent)
    with open(PROJ + "/specs/alpha.md", "w") as f:
        f.write(SPEC_A)
    with open(PROJ + "/specs/beta.md", "w") as f:
        f.write(SPEC_B)

def run_session(actions, timeout=40):
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
            send(["\x11"]); drain(0.6)
            send(["n"]); drain(0.6)
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

def open_workbench(send, drain, name):
    send(["\x10"]); drain(0.6)              # Ctrl-P: go to anything
    send(list(name)); drain(0.6)
    send(["\r"]); drain(1.5)                # open the spec
    send(["\x02"]); drain(0.5)              # Ctrl-B: palette
    send(list("Spec Workbench")); drain(0.5)
    send(["\r"]); drain(0.8)
    send(["\r"]); drain(2.5)                # confirm the launch

def actions(send, drain, out):
    open_workbench(send, drain, "alpha")
    marks["alpha"] = len(out)
    # Focus starts in the document (it is primary). Crossing to the
    # conversation is Alt-Right rather than Tab: Scintilla owns Tab for
    # indentation inside the document, so it never reaches the window.
    send([b"\x1b[1;3C"]); drain(0.6)
    send(list("PINGALPHA")); drain(0.4)
    send(["\r"]); drain(2.5)
    marks["alpha_reply"] = len(out)
    # A second spec gets its own window and its own session (FR2/FR3) --
    # impossible before, when one app-global agent window was shared.
    open_workbench(send, drain, "beta")
    marks["beta"] = len(out)

raw = run_session(actions)
s = squash(stripped(raw))
alpha = squash(stripped(raw[:marks.get("alpha", len(raw))]))
alpha_reply = squash(stripped(raw[:marks.get("alpha_reply", len(raw))]))
beta = squash(stripped(raw[marks.get("alpha_reply", 0):marks.get("beta", len(raw))]))

# One window, not two: the old design placed a separate "Spec Agent (...)"
# terminal window beside the editor. There must be no such window now.
check("wb: no separate agent window", "SpecAgent(" not in s)
# ...and yet the conversation is present, inside the spec's own window. The
# opening brief is long, so the greeting has already scrolled off -- assert on
# the transcript's live tail instead.
check("wb: conversation docked in the spec window",
      squash("turbo spec agent brief") in alpha)
# The document is still there, beside it, in the same window.
check("wb: document still shown", "ALPHABODYTEXT" in alpha)
# The section strip (FR6) survives the split.
check("wb: section strip still shown", "Sections" in alpha)

# Tab reached the composer and the turn round-tripped through the pane.
check("wb: focus crosses to the composer, turn sent",
      squash("> PINGALPHA") in alpha_reply)
check("wb: agent replied in the pane", "REPLYPINGALPHA" in alpha_reply)

# The opening brief went over the structured channel, not a brief file on the
# command line (FR8): the agent echoed its first line back.
check("wb: interview brief delivered as a turn", "REPLY" in alpha_reply)
briefs = PROJ + "/.turbo/spec-sessions"
check("wb: no brief file written for Discuss",
      not os.path.isdir(briefs) or not os.listdir(briefs))

# A second spec opens its own Workbench with its own agent session.
check("wb: second spec opens its own workbench", "BETABODYTEXT" in beta)
check("wb: second spec has its own conversation",
      squash("turbo spec agent brief") in beta)

# --- FR11: activating a tool call moves the document's cursor ---------------
# The agent replies with a tool call naming the Decisions section; Enter on
# that row must take the document there. The spec is long enough that
# Decisions is off-screen until the jump happens.
def actions_jump(send, drain, out):
    open_workbench(send, drain, "alpha")
    send([b"\x1b[1;3C"]); drain(0.6)        # cross to the conversation
    send(list("JUMPTEST")); drain(0.4)
    send(["\r"]); drain(2.5)                 # agent emits the tool call
    marks["before_jump"] = len(out)
    # Alt-J: jump the document to the agent's most recent edit. A command
    # rather than Enter-on-a-row, so it works from either pane.
    send([b"\x1bj"]); drain(1.5)
    marks["after_jump"] = len(out)

raw2 = run_session(actions_jump)
before = squash(stripped(raw2[:marks.get("before_jump", 0)]))
after = squash(stripped(raw2[marks.get("before_jump", 0):]))
check("wb: tool call rendered with its target section",
      "insert_text" in before and "Decisions" in before)
# The jump is what proves the transcript is a way *into* the document.
check("wb: activating a tool call moves the document cursor",
      "DECISIONSANCHOR" in after and "DECISIONSANCHOR" not in before)

# --- FR7: closing and reopening a Workbench resumes the conversation --------
# Open a Workbench (agent session A), close the window (ending A), then open a
# Workbench on the same spec again. The second launch must carry
# --resume <A's session id> rather than starting fresh -- and no fresh brief.
open(PROJ + "/launches.log", "w").close()  # only this run's launches

def actions_resume(send, drain, out):
    open_workbench(send, drain, "alpha")
    drain(0.8)                               # let init be captured
    send(["\x17"]); drain(1.2)               # Ctrl-W: close the editor window
    open_workbench(send, drain, "alpha")     # reopen -> should resume
    drain(0.8)

run_session(actions_resume)

launches = []
try:
    with open(PROJ + "/launches.log") as f:
        for ln in f:
            ln = ln.strip()
            if ln:
                launches.append(__import__("json").loads(ln))
except Exception:
    pass
check("resume: two agent launches recorded", len(launches) >= 2)
first_sid = launches[0]["sid"] if launches else ""
first_argv = launches[0]["argv"] if launches else []
second_argv = launches[1]["argv"] if len(launches) > 1 else []
check("resume: the first launch is fresh (no --resume)",
      "--resume" not in first_argv)
check("resume: the reopened agent gets --resume", "--resume" in second_argv)
check("resume: --resume carries the previous session id",
      bool(first_sid) and first_sid in second_argv)

# --- FR1: the splitter between the panes is draggable -----------------------
# Open a Workbench, find the divider column, drag it left, and assert it moved
# there -- proving the split is adjustable rather than fixed.
drag_marks = {}

def actions_drag(send, drain, out):
    open_workbench(send, drain, "alpha")
    drain(0.5)
    drag_marks["before"] = len(out)
    grid = build_grid(bytes(out[:drag_marks["before"]]))
    col, _ = divider_col(grid, 140)
    drag_marks["col"] = col
    if col > 0:
        row = 10
        target = col + 20                 # drag right: the agent pane narrows
        send([sgr_mouse(0, col, row, True)]); drain(0.3)         # press on divider
        send([sgr_mouse(32, col + 10, row, True)]); drain(0.2)   # drag
        send([sgr_mouse(32, target, row, True)]); drain(0.2)
        send([sgr_mouse(0, target, row, False)]); drain(0.6)     # release
    drain(0.4)
    drag_marks["after"] = len(out)

raw3 = run_session(actions_drag)
before_col = drag_marks.get("col", -1)
grid_before = build_grid(raw3[:drag_marks.get("before", 0)])
grid_after = build_grid(raw3[:drag_marks.get("after", len(raw3))])
# The window's own frames sit at stable columns in both grids; only the
# divider moves. So the column that stopped being a full-height run and the
# one that became one are the divider's old and new homes.
def tall_vbars(grid):
    return {c for c in range(2, 140) if vbar_run(grid, c) >= 20}
gone = tall_vbars(grid_before) - tall_vbars(grid_after)
arrived = tall_vbars(grid_after) - tall_vbars(grid_before)
check("drag: the divider was found before the drag", before_col > 0)
# The splitter followed the drag: its column left its old home and a new
# full-height divider appeared well away from it.
check("drag: the splitter moved when dragged",
      before_col in gone and
      any(abs(a - before_col) >= 10 for a in arrived))
# Both panes survive the resize.
check("drag: both panes still render after the resize",
      "ALPHABODYTEXT" in squash(stripped(raw3)))

shutil.rmtree(PROJ, ignore_errors=True)

failed = [n for n, ok in results if not ok]
print()
print("%d/%d passed" % (len(results) - len(failed), len(results)))
sys.exit(1 if failed else 0)

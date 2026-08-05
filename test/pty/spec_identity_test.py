#!/usr/bin/env python3
"""PTY runtime test: spec purple background + cmNewSpec template (M1 of
specs/spec-workbench.md).

Drives the real turboIDE binary under a pty. Conventions this harness
enforces (learned the hard way):
  - a real pty, not piped stdin (piped stdin gives a blank screen);
  - TIOCSWINSZ before the app starts (a fresh pty is 0x0: nothing paints);
  - discrete key writes with a gap (bursts parse as a bracketed paste);
  - a real, non-symlinked project dir (macOS FSEvents reports /private
    paths, so /tmp-rooted symlinked paths never match watcher events);
  - COLORTERM=truecolor, or tvision won't emit 24-bit SGR at all.

Usage: python3 test/pty/spec_identity_test.py [path-to-turboIDE]
"""
import fcntl, json, os, pty, re, select, struct, subprocess, sys, termios, \
       time, shutil, signal, tempfile

def stripped(raw):
    """Remove escape sequences so text assertions see contiguous glyphs."""
    s = re.sub(rb"\x1b\[[0-9;:?]*[A-Za-z]", b"", raw)
    s = re.sub(rb"\x1b\][^\x07\x1b]*(\x07|\x1b\\)", b"", s)
    s = re.sub(rb"\x1b.", b"", s)
    return s.decode("utf-8", "replace")

def has_sgr(raw, *nums):
    """True if an SGR sequence containing the given number run appears."""
    pat = ("[;:]".join(str(n) for n in nums)).encode()
    return re.search(rb"\x1b\[[0-9;:]*" + pat + rb"[0-9;:]*m", raw) is not None

def colors_at(raw, needle, occurrence=-1):
    """(fg, bg) in effect where 'needle' is written, as ('rgb',r,g,b) or
    ('bios',n) or None.

    Replaying the SGR state is the only way to assert what a *particular*
    glyph run is painted with: a bare has_sgr() proves a colour appears
    somewhere on screen, not that the frame is using it. That weaker check
    is what let a blue active frame sit behind a purple body unnoticed.
    """
    occ = list(re.finditer(re.escape(needle), raw))
    if not occ:
        return (None, None)
    end = occ[occurrence].start()
    fg = bg = None
    for m in re.finditer(rb"\x1b\[([0-9;:]*)m", raw[:end]):
        parts = [int(p) for p in re.split(rb"[;:]", m.group(1)) if p != b""]
        i = 0
        while i < len(parts):
            p = parts[i]
            if p in (38, 48) and i + 1 < len(parts):
                target = "fg" if p == 38 else "bg"
                if parts[i + 1] == 2 and i + 4 < len(parts):
                    val = ("rgb",) + tuple(parts[i + 2:i + 5]); i += 5
                elif parts[i + 1] == 5 and i + 2 < len(parts):
                    val = ("idx", parts[i + 2]); i += 3
                else:
                    i += 2; continue
                if target == "fg": fg = val
                else: bg = val
                continue
            if p == 0: fg = bg = None
            elif 30 <= p <= 37: fg = ("bios", p - 30)
            elif 90 <= p <= 97: fg = ("bios", p - 90 + 8)
            elif p == 39: fg = None
            elif 40 <= p <= 47: bg = ("bios", p - 40)
            elif 100 <= p <= 107: bg = ("bios", p - 100 + 8)
            elif p == 49: bg = None
            i += 1
    return (fg, bg)

REPO = os.path.realpath(os.path.join(os.path.dirname(__file__), "..", ".."))
TURBO = os.path.realpath(sys.argv[1] if len(sys.argv) > 1
                         else os.path.join(REPO, "turboIDE"))
# realpath: FSEvents reports resolved paths, so the project dir must be one.
PROJ = os.path.realpath(tempfile.mkdtemp(prefix="turbo-spec-pty-"))

SPEC_BODY = """---
title: Test Spec
status: draft
domain: test
created: 2026-07-19
updated: 2026-07-19
---

# Background

Some background text for rendering.

# Objective

# Implementation Plan

- [ ] first thing
"""

# Opening a spec interactively now auto-docks the agent Workbench (D25). These
# surface/manager/ask/loop sessions are about the purple identity, not the
# agent, so suppress the auto-dock (TURBO_NO_AUTO_SPEC_AGENT) and test the plain
# spec editor. Sessions 5 and 7 clear it in env_extra to exercise the Workbench.
os.environ["TURBO_NO_AUTO_SPEC_AGENT"] = "1"

def setup():
    shutil.rmtree(PROJ, ignore_errors=True)
    os.makedirs(PROJ + "/specs")
    with open(PROJ + "/specs/test-spec.md", "w") as f:
        f.write(SPEC_BODY)
    with open(PROJ + "/readme.txt", "w") as f:
        f.write("plain file\n")

def run_session(env_extra, actions, timeout=25, width=120):
    """Spawn turbo on PROJ, run actions(send), return all raw output bytes."""
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.environ["COLORTERM"] = "truecolor"
        os.environ.pop("TURBO_TEST", None)
        for k, v in env_extra.items():
            os.environ[k] = v
        os.chdir(PROJ)
        os.execv(TURBO, [TURBO, PROJ])
        os._exit(1)

    # A fresh pty is 0x0; tvision would paint into an empty screen. Give the
    # child a real terminal size before it initializes.
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, width, 0, 0))
    out = bytearray()
    deadline = time.time() + timeout

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
        # Discrete keys: one write per key, small gap, so the input parser
        # sees key presses rather than a bracketed paste.
        for ch in data:
            os.write(fd, ch if isinstance(ch, bytes) else ch.encode())
            drain(gap)

    drain(3.0)  # let it start and scan the project
    try:
        actions(send, drain)
    finally:
        # Quit: Ctrl-Q, then answer any "save?" prompt with n. Either write
        # may hit an already-closed pty if the app exits promptly.
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

def open_via_palette(send, drain, label):
    # Ctrl-B opens the command palette; type the label; Enter runs it.
    send(["\x02"]); drain(0.5)
    send(list(label)); drain(0.4)
    send(["\r"]); drain(1.0)

results = []
def check(name, ok):
    results.append((name, ok))
    print(("PASS " if ok else "FAIL ") + name)

setup()

# --- Session 1: full-colour mode -------------------------------------------
def actions_full(send, drain):
    # Open the spec via Go to Anything (Ctrl-P).
    send(["\x10"]); drain(0.6)
    send(list("test-spec")); drain(0.5)
    send(["\r"]); drain(1.5)
    # New Spec via the command palette (Ctrl-B).
    open_via_palette(send, drain, "New Spec")
    # Dialog: Title -> Tab -> Domain -> Tab -> Goal -> Enter.
    send(list("Payment Flow")); send(["\t"])
    send(list("saas")); send(["\t"])
    send(list("Take payments simply.")); drain(0.3)
    send(["\r"]); drain(2.0)

out1 = run_session({}, actions_full)
s1 = stripped(out1)

# Purple active background: RGB 0x2A1B4D -> SGR 48;2;42;27;77 (or colon form)
check("full: active purple bg escape present", has_sgr(out1, 48, 2, 42, 27, 77))
# The spec body text actually rendered
check("full: spec text rendered", "background text for rendering" in s1)
# Template file created by cmNewSpec
new_path = PROJ + "/specs/payment-flow.md"
created = os.path.exists(new_path)
check("full: cmNewSpec created specs/payment-flow.md", created)
if created:
    body = open(new_path).read()
    check("full: template has frontmatter title", "title: Payment Flow" in body)
    check("full: template status draft", "status: draft" in body)
    check("full: template domain", "domain: saas" in body)
    check("full: goal seeded into Objective",
          "# Objective\n\nTake payments simply." in body)
    check("full: canonical headings present",
          all(("# " + h) in body for h in
              ["Background", "Objective", "Non-Goals", "Functional Requirements",
               "UX Considerations", "Security Considerations",
               "Auditability and Observability", "Test Strategy", "References",
               "Decisions", "Implementation Plan", "Summary"]))
# The new spec window opened -> its purple bg present again (already covered),
# and the tree shows the specs entry
check("full: tree shows specs dir", "specs" in s1)
# Spec files are tinted with the spec purple accent 0x9D7CD8 in the tree.
check("full: spec file tinted in tree", has_sgr(out1, 38, 2, 157, 124, 216))
# The frame is part of the purple surface (D29): active frame text 0xE6DFF5.
check("full: purple frame chrome", has_sgr(out1, 38, 2, 230, 223, 245))
# ...and the active frame really *is* the purple one -- assert the attributes
# the title bar is painted with, not merely that the colour exists on screen.
# A blue title bar over a purple body is the exact pre-D29 look.
fg1, bg1 = colors_at(out1, b"test-spec.md")
check("full: active frame bg is spec purple", bg1 == ("rgb", 42, 27, 77))
check("full: active frame fg is lavender", fg1 == ("rgb", 230, 223, 245))

# --- Session 2: classic 16-colour mode -------------------------------------
def actions_classic(send, drain):
    send(["\x10"]); drain(0.6)
    send(list("test-spec")); drain(0.5)
    send(["\r"]); drain(1.5)

# Classic mode is the *setting* theme.colors=16, not TVISION_COLORS: the env
# var only caps Turbo Vision's output depth (turbo sets it itself from the
# setting), so capping alone leaves the RGB schemes active and the violet is
# merely downconverted -- to blue, which is exactly what the purple is meant
# to be distinguishable from. Drive it through a throwaway HOME/.turborc.
CLASSIC_HOME = os.path.realpath(tempfile.mkdtemp(prefix="turbo-spec-home-"))
with open(CLASSIC_HOME + "/.turborc", "w") as f:
    f.write("theme.colors=16\n")

out2 = run_session({"HOME": CLASSIC_HOME}, actions_classic)
# BIOS magenta background renders as SGR 45 (bg magenta) in 16-colour output.
check("classic: magenta bg escape present", has_sgr(out2, 45))
check("classic: no RGB purple leaks", not has_sgr(out2, 48, 2, 42, 27, 77))
# The 16-colour branch of specPurpleScheme()/applyActiveStateTheme: magenta
# chrome and body, not the default blue.
fg2, bg2 = colors_at(out2, b"test-spec.md")
check("classic: active frame bg is magenta", bg2 == ("bios", 5))
_, bodybg2 = colors_at(out2, b"background text for rendering")
check("classic: spec body bg is magenta", bodybg2 == ("bios", 5))
shutil.rmtree(CLASSIC_HOME, ignore_errors=True)

# --- Session 3: Spec Manager (Alt-P) ---------------------------------------
setup()  # reset the fixture so test-spec is the only (and focused) row

def actions_manager(send, drain):
    send(["\x1bp"]); drain(1.2)   # Alt-P -> Spec Manager
    send(["r"]); drain(0.8)       # Mark reviewed...
    send(["\r"]); drain(1.5)      # confirm (Yes is the default)

out3 = run_session({}, actions_manager)
s3 = stripped(out3)
check("mgr: column headers shown",
      all(h in s3 for h in ("Title", "Status", "Domain", "Updated", "Plan")))
# The Manager is spec material, so it sits on the same violet surface as a
# spec editor -- chrome and content together, not a blue frame around a
# purple table (D29's "one surface" rule, extended to the Manager).
fg_mgr, bg_mgr = colors_at(out3, b"Specs")       # the window's frame title
check("mgr: frame is the spec violet", bg_mgr == ("rgb", 42, 27, 77))
check("mgr: frame text is lavender", fg_mgr == ("rgb", 230, 223, 245))
fg_hdr, bg_hdr = colors_at(out3, b"Title")       # the column-header row
check("mgr: header sits on the violet surface", bg_hdr == ("rgb", 42, 27, 77))
# Gold, not purple: a purple accent would vanish on a purple ground.
check("mgr: header accent is gold", fg_hdr == ("rgb", 232, 192, 125))
check("mgr: spec row shown with title", "Test Spec" in s3)
check("mgr: gate line shows blocked reason", "Blocked: not reviewed" in s3)
check("mgr: key legend shown", "R review" in s3)
body3 = open(PROJ + "/specs/test-spec.md").read()
check("mgr: mark reviewed wrote status", "status: reviewed" in body3)
check("mgr: review recorded in Decisions",
      "Reviewed by the user via the Spec Manager." in body3)
check("mgr: override recorded (was draft)",
      "Override: status was 'draft'" in body3)

# --- Session 4: ask_user via the real MCP bridge (a fake agent) -------------
# The fake client launches the same `turboIDE mcp` bridge that .mcp.json
# points coding agents at, and speaks newline-delimited JSON-RPC over it.

def bridge_send(proc, obj):
    proc.stdin.write((json.dumps(obj) + "\n").encode())
    proc.stdin.flush()

def bridge_read(proc, timeout=12):
    r, _, _ = select.select([proc.stdout], [], [], timeout)
    if not r:
        return None
    line = proc.stdout.readline()
    return json.loads(line) if line else None

ask = {}

def actions_ask(send, drain):
    deadline = time.time() + 10
    while time.time() < deadline and not os.path.exists(PROJ + "/.mcp.json"):
        drain(0.3)
    cfg = json.load(open(PROJ + "/.mcp.json"))
    srv = cfg["mcpServers"]["turboide"]
    proc = subprocess.Popen([srv["command"]] + srv.get("args", []),
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, cwd=PROJ)
    try:
        bridge_send(proc, {"jsonrpc": "2.0", "id": 1, "method": "initialize",
                           "params": {"protocolVersion": "2024-11-05",
                                      "clientInfo": {"name": "pty-fake-agent",
                                                     "version": "1"},
                                      "capabilities": {}}})
        ask["init"] = bridge_read(proc)
        bridge_send(proc, {"jsonrpc": "2.0",
                           "method": "notifications/initialized"})
        bridge_send(proc, {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                           "params": {"name": "ask_user", "arguments": {
                               "questions": [
                                   {"prompt": "Pick a colour",
                                    "options": ["red", "green", "blue"]},
                                   {"prompt": "Say something",
                                    "free_text": True}]}}})
        drain(2.5)                    # wizard page 1 appears
        send(["\x1b[B"]); drain(0.5)  # Down: red -> green
        send(["\r"]); drain(1.2)      # Next
        send(list("hello")); drain(0.4)
        send(["\r"]); drain(1.2)      # Finish
        ask["call"] = bridge_read(proc)
        bridge_send(proc, {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                           "params": {"name": "ask_user", "arguments": {
                               "questions": [{"prompt": "Cancel me",
                                              "free_text": True}]}}})
        drain(2.0)
        send(["\x1b"]); drain(1.2)    # Esc -> cancel
        ask["cancel"] = bridge_read(proc)
    finally:
        try:
            proc.stdin.close()
        except OSError:
            pass
        proc.terminate()

out4 = run_session({}, actions_ask, timeout=45)
s4 = stripped(out4)

def tool_text(resp):
    try:
        return json.loads(resp["result"]["content"][0]["text"])
    except (KeyError, TypeError, IndexError, ValueError):
        return {}

data = tool_text(ask.get("call") or {})
check("ask: wizard finished (not cancelled)", data.get("cancelled") is False)
answers = data.get("answers") or []
check("ask: radio answer is green",
      bool(answers) and answers[0].get("selected") == ["green"])
check("ask: free-text answer round-trips",
      len(answers) > 1 and answers[1].get("text") == "hello")
cancelled = tool_text(ask.get("cancel") or {})
check("ask: cancel sentinel explicit", cancelled.get("cancelled") is True)
check("ask: attribution line shown", "From: pty-fake-agent" in s4)
check("ask: wizard pages numbered", "Agent Question (1/2)" in s4)

# --- Session 5: Spec Workbench (strip + docked conversation pane) -----------
# The Workbench is now ONE window -- document left, conversation right (M3 of
# specs/spec-agent-integration.md) -- rather than two windows placed side by
# side, and the interview brief travels as a structured turn instead of a
# brief file named on the command line.
setup()
os.makedirs(PROJ + "/.turbo", exist_ok=True)
# A scripted stand-in speaking the stream-json protocol. The test must never
# launch a real agent CLI. (/bin/cat was the old stand-in and cannot serve
# here: with stdout on a pipe it block-buffers at 4096 bytes, and the brief is
# just under that, so it never flushes.) It records what it receives so the
# brief's *content* can still be asserted now that no brief file is written.
SPY_AGENT = '''#!/usr/bin/env python3
import sys, json
log = open(sys.argv[1], "w")
def emit(o):
    sys.stdout.write(json.dumps(o) + "\\n"); sys.stdout.flush()
emit({"type": "system", "subtype": "init",
      "session_id": "spy-session", "model": "spy-model"})
for line in sys.stdin:
    log.write(line); log.flush()
    try:
        json.loads(line)["message"]["content"][0]["text"]
    except Exception:
        continue
    emit({"type": "assistant",
          "message": {"content": [{"type": "text", "text": "AGENTREPLIED"}]}})
    emit({"type": "result", "subtype": "success", "is_error": False,
          "result": "AGENTREPLIED"})
'''
spy_path = PROJ + "/spy_agent.py"
spy_log = PROJ + "/spy.log"
with open(spy_path, "w") as f:
    f.write(SPY_AGENT)
os.chmod(spy_path, 0o755)
with open(PROJ + "/.turbo/config.json", "w") as f:
    json.dump({"agent": spy_path + " " + spy_log}, f)

def actions_workbench(send, drain):
    # Opening a spec interactively auto-docks the Workbench (D25) -- no palette
    # command and no launch confirmation.
    send(["\x10"]); drain(0.6)
    send(list("test-spec")); drain(0.8)
    send(["\r"]); drain(3.5)   # open -> pane docks, strip appears, brief sent

# A wider terminal so the section strip -- clamped to the (now narrower) editor
# once the pane docks -- still has room to name the empty section.
out5 = run_session({"TURBO_NO_AUTO_SPEC_AGENT": ""}, actions_workbench, width=170)
s5 = stripped(out5)
# The fixture spec: Background (drafted), Objective (empty), Implementation
# Plan (has a checkbox), so the strip reads "Sections 2/3 drafted | empty:
# Objective".
check("wb: section strip counts", "Sections 2/3 drafted" in s5)
check("wb: strip names the empty section", "empty: Objective" in s5)
check("wb: spec text still visible", "background text for rendering" in s5)
# One window, two panes: no separate "Spec Agent (...)" terminal window is
# opened any more, and the conversation lives inside the spec's own window.
check("wb: no separate agent window", "Spec Agent (" not in s5)
check("wb: conversation docked in the same window", "AGENTREPLIED" in s5)
# The brief now travels over the structured channel (FR8), so there is no
# brief file and no shell quoting -- but its content must still be right.
brief_path = PROJ + "/.turbo/spec-sessions/test-spec-discuss.md"
check("wb: no brief file written for Discuss", not os.path.exists(brief_path))
check("wb: interview brief delivered to the agent", os.path.exists(spy_log))
if os.path.exists(spy_log):
    received = open(spy_log).read()
    brief = ""
    try:
        brief = json.loads(received.splitlines()[0])["message"]["content"][0]["text"]
    except Exception:
        pass
    # The prompt arrives as ONE intact argument-free turn, not split into
    # argv fragments the way the old shell-quoted command line did.
    check("wb: brief carries the rubric", "Readiness rubric" in brief)
    check("wb: brief carries the contract", "Write-back contract" in brief)
    check("wb: brief names the spec", PROJ + "/specs/test-spec.md" in brief)

# --- Session 6: agent write-back reaches the open editor (FR5) --------------
setup()

def actions_agentloop(send, drain):
    send(["\x10"]); drain(0.6)
    send(list("test-spec")); drain(0.5)
    send(["\r"]); drain(1.5)
    # An external "spec agent" writes a distilled answer into the file on
    # disk; the clean editor buffer must silently reload it (FR5).
    body = open(PROJ + "/specs/test-spec.md").read()
    body = body.replace("# Objective\n",
                        "# Objective\n\nDistilled by the spec agent.\n")
    with open(PROJ + "/specs/test-spec.md", "w") as f:
        f.write(body)
    drain(3.0)  # FSEvents latency
    send(["\x1b[B"])  # any key: nudges the event loop through an idle tick
    drain(4.0)

out6 = run_session({}, actions_agentloop)
s6 = stripped(out6)
# Single-token match: the terminal may replace runs of spaces with cursor
# moves, so multi-word phrases are not reliable across a redraw.
check("loop: agent-written text reloads into the open editor",
      "Distilled" in s6)

# --- Session 7: Implement handoff, gate refusal then gated launch -----------
setup()
os.makedirs(PROJ + "/.turbo", exist_ok=True)
impl_log = PROJ + "/impl.log"
with open(spy_path, "w") as f:      # same scripted stand-in as session 5
    f.write(SPY_AGENT)
os.chmod(spy_path, 0o755)
with open(PROJ + "/.turbo/config.json", "w") as f:
    json.dump({"agent": spy_path + " " + impl_log}, f)
with open(PROJ + "/specs/ship-it.md", "w") as f:
    f.write("---\ntitle: Ship It\nstatus: reviewed\nupdated: 2026-07-19\n"
            "---\n\n# Objective\n\nShip.\n\n# Implementation Plan\n\n"
            "- [ ] do it\n")

def actions_implement(send, drain):
    send(["\x1bp"]); drain(1.2)   # Spec Manager; ship-it focused (path sort)
    send(["\x1b[B"]); drain(0.5)  # Down -> test-spec (draft)
    send(["i"]); drain(1.2)       # Implement -> gate refusal box
    send(["\r"]); drain(0.8)      # dismiss
    send(["\x1b[A"]); drain(0.5)  # Up -> ship-it (reviewed, gate passes)
    send(["i"]); drain(3.0)       # Implement -> workbench docks (no confirm, D25)

out7 = run_session({"TURBO_NO_AUTO_SPEC_AGENT": ""}, actions_implement)
s7 = stripped(out7)
check("impl: gate refusal names the blocker", "Cannot implement" in s7)
# The gated launch now opens the Workbench on the spec rather than a
# separate terminal window -- all three modes share one path (M4).
check("impl: gated launch opens the workbench, not a terminal",
      "Spec Agent (" not in s7)
check("impl: implement conversation is live", "AGENTREPLIED" in s7)
impl_brief = PROJ + "/.turbo/spec-sessions/ship-it-implement.md"
check("impl: no brief file written", not os.path.exists(impl_brief))
check("impl: implement brief delivered to the agent", os.path.exists(impl_log))
if os.path.exists(impl_log):
    got = ""
    try:
        got = json.loads(open(impl_log).read().splitlines()[0])
        got = got["message"]["content"][0]["text"]
    except Exception:
        pass
    check("impl: brief carries the implement mission", "Mission: implement" in got)
    check("impl: brief carries the write-back contract",
          "Write-back contract" in got)

# --- Session 8: D20 — warn when specs/ is gitignored ------------------------
setup()
subprocess.run(["git", "init", "-q"], cwd=PROJ, check=False)
with open(PROJ + "/.gitignore", "w") as f:
    f.write("specs\n")

def actions_ignored(send, drain):
    open_via_palette(send, drain, "New Spec")
    send(list("Hidden Feature")); drain(0.3)
    send(["\r"]); drain(2.0)   # OK -> file created -> D20 warning box
    send(["\r"]); drain(1.0)   # dismiss

out8 = run_session({}, actions_ignored)
s8 = stripped(out8)
check("ignored: D20 warning shown for gitignored specs/", "gitignore" in s8)

print()
fails = [n for n, ok in results if not ok]
print(f"{len(results) - len(fails)}/{len(results)} passed")
shutil.rmtree(PROJ, ignore_errors=True)
sys.exit(1 if fails else 0)

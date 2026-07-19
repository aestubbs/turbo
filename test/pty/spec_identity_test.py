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

def setup():
    shutil.rmtree(PROJ, ignore_errors=True)
    os.makedirs(PROJ + "/specs")
    with open(PROJ + "/specs/test-spec.md", "w") as f:
        f.write(SPEC_BODY)
    with open(PROJ + "/readme.txt", "w") as f:
        f.write("plain file\n")

def run_session(env_extra, actions, timeout=25):
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
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
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

# --- Session 2: classic 16-colour mode -------------------------------------
def actions_classic(send, drain):
    send(["\x10"]); drain(0.6)
    send(list("test-spec")); drain(0.5)
    send(["\r"]); drain(1.5)

out2 = run_session({"TVISION_COLORS": "16"}, actions_classic)
# BIOS magenta background renders as SGR 45 (bg magenta) in 16-colour output.
check("classic: magenta bg escape present", has_sgr(out2, 45))
check("classic: no RGB purple leaks", not has_sgr(out2, 48, 2, 42, 27, 77))

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
check("mgr: spec row shown with title", "Test Spec" in s3)
check("mgr: gate line shows blocked reason", "needs review" in s3)
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

print()
fails = [n for n, ok in results if not ok]
print(f"{len(results) - len(fails)}/{len(results)} passed")
shutil.rmtree(PROJ, ignore_errors=True)
sys.exit(1 if fails else 0)

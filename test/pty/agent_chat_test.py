#!/usr/bin/env python3
"""PTY runtime test: the structured agent conversation pane (M2 of
specs/spec-agent-integration.md).

Drives the real turboIDE binary against a *fake* agent that speaks the
stream-json protocol, so the whole M1 transport + M2 rendering path is
exercised end to end with no LLM and no cost. The fake is configured through
the project's .turbo/config.json "agent" key, exactly as a real one would be.

Harness conventions (same as spec_identity_test.py): a real pty, TIOCSWINSZ
before start, discrete key writes, a real non-symlinked project dir.

Usage: python3 test/pty/agent_chat_test.py [path-to-turboIDE]
"""
import fcntl, os, pty, re, select, signal, struct, sys, termios, time, \
       shutil, tempfile

def stripped(raw):
    s = re.sub(rb"\x1b\[[0-9;:?]*[A-Za-z]", b"", raw)
    s = re.sub(rb"\x1b\][^\x07\x1b]*(\x07|\x1b\\)", b"", s)
    s = re.sub(rb"\x1b.", b"", s)
    return s.decode("utf-8", "replace")

def squash(s):
    """Drop all whitespace.

    tvision repaints differentially: cells that already hold the right glyph
    are not rewritten, and a run of spaces that was already blank produces no
    output at all. Once the cursor-positioning escapes are stripped, the
    glyphs that *were* written land adjacent to each other, so "ECHOED hello"
    can read as "ECHOEDhello" even though the screen is correct. Compare
    whitespace-free on both sides rather than asserting on exact spacing.
    """
    return re.sub(r"\s+", "", s)

REPO = os.path.realpath(os.path.join(os.path.dirname(__file__), "..", ".."))
TURBO = os.path.realpath(sys.argv[1] if len(sys.argv) > 1
                         else os.path.join(REPO, "turboIDE"))
PROJ = os.path.realpath(tempfile.mkdtemp(prefix="turbo-agentchat-"))

# A minimal agent speaking the stream-json protocol: one init event, then per
# user turn an assistant text block, a tool call, and a result. Enough to prove
# every event kind reaches the pane with its own rendering.
FAKE_AGENT = '''#!/usr/bin/env python3
import json, sys

def emit(o):
    sys.stdout.write(json.dumps(o) + "\\n")
    sys.stdout.flush()

emit({"type": "system", "subtype": "init",
      "session_id": "fake-session-1", "model": "fake-model"})
for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    try:
        msg = json.loads(line)
    except Exception:
        continue
    try:
        text = msg["message"]["content"][0]["text"]
    except Exception:
        continue
    emit({"type": "assistant", "session_id": "fake-session-1",
          "message": {"content": [{"type": "text", "text": "ECHOED " + text}]}})
    emit({"type": "assistant", "session_id": "fake-session-1",
          "message": {"content": [{"type": "tool_use", "id": "toolu_1",
                                   "name": "file_text",
                                   "input": {"path": "/spec.md"}}]}})
    emit({"type": "user", "session_id": "fake-session-1",
          "message": {"content": [{"type": "tool_result",
                                   "tool_use_id": "toolu_1",
                                   "content": "SPECBODY"}]}})
    emit({"type": "result", "subtype": "success", "is_error": False,
          "result": "ECHOED " + text, "session_id": "fake-session-1"})
'''

def setup():
    os.makedirs(PROJ + "/.turbo", exist_ok=True)
    agent = PROJ + "/fake_agent.py"
    with open(agent, "w") as f:
        f.write(FAKE_AGENT)
    os.chmod(agent, 0o755)
    # Point the project's agent at the fake (buildconfig's "agent" key).
    with open(PROJ + "/.turbo/config.json", "w") as f:
        f.write('{"agent": "%s"}\n' % agent)
    with open(PROJ + "/readme.txt", "w") as f:
        f.write("hello\n")

def run_session(actions, timeout=30):
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.environ["COLORTERM"] = "truecolor"
        os.environ.pop("TURBO_TEST", None)
        os.chdir(PROJ)
        os.execv(TURBO, [TURBO, PROJ])
        os._exit(1)

    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
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

def actions(send, drain, out):
    # Ctrl-B: command palette -> "Agent Conversation" -> Enter.
    send(["\x02"]); drain(0.5)
    send(list("Agent Conv")); drain(0.5)
    send(["\r"]); drain(1.0)
    # Launch confirmation (Yes is the default).
    send(["\r"]); drain(2.0)
    marks["opened"] = len(out)
    # Type a turn into the composer and send it.
    send(list("hello agent")); drain(0.4)
    send(["\r"]); drain(2.5)
    marks["answered"] = len(out)

raw = run_session(actions)
s = squash(stripped(raw))
opened = squash(stripped(raw[:marks.get("opened", len(raw))]))

# The window opened and greeted before any turn was sent.
check("chat: window titled Agent", "Agent" in opened)
check("chat: startup notice shown", squash("Type a message and press Enter") in opened)

# The user's turn is echoed into the transcript with its marker.
check("chat: user turn rendered", squash("> hello agent") in s)
# The agent's prose came back through the structured transport.
check("chat: assistant text rendered", squash("ECHOED hello agent") in s)
# A tool call is rendered as a tool call, not as opaque prose (FR10).
check("chat: tool call rendered", "file_text" in s)
check("chat: tool call shows its input", "/spec.md" in s)
# And its result is shown beneath it.
check("chat: tool result rendered", "SPECBODY" in s)

# The pane is not a terminal: the agent's raw protocol never reaches the
# screen -- turbo parsed it and drew the transcript itself.
check("chat: no raw json leaked into the pane", squash('"type":"assistant"') not in s)

shutil.rmtree(PROJ, ignore_errors=True)

failed = [n for n, ok in results if not ok]
print()
print("%d/%d passed" % (len(results) - len(failed), len(results)))
sys.exit(1 if failed else 0)

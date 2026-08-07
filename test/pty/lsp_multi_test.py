#!/usr/bin/env python3
"""PTY runtime test: MORE THAN ONE language server per language.

Configures two servers for `php` -- a per-language primary (`lsp.server.php`) and
an extra (`lsp.extra.<name>`) -- and proves the client fans out to both and MERGES
their results rather than letting one clobber the other:

  * both servers start and receive textDocument/didOpen;
  * both servers' diagnostics render together (per-source, not overwritten);
  * completion results from both servers are merged in the popup.

Uses the mock_lsp.py stand-in (no real server). Config is supplied through a
throwaway HOME/.turborc, exactly how a user would set it up.

Usage: python3 test/pty/lsp_multi_test.py [path-to-turboIDE]
"""
import fcntl, os, pty, re, select, signal, struct, sys, termios, time, \
       shutil, tempfile

def stripped(raw):
    s = re.sub(rb"\x1b\[[0-9;:?]*[A-Za-z]", b"", raw)
    s = re.sub(rb"\x1b\][^\x07\x1b]*(\x07|\x1b\\)", b"", s)
    s = re.sub(rb"\x1b.", b"", s)
    return s.decode("utf-8", "replace")

REPO = os.path.realpath(os.path.join(os.path.dirname(__file__), "..", ".."))
TURBO = os.path.realpath(sys.argv[1] if len(sys.argv) > 1
                         else os.path.join(REPO, "turboIDE"))
MOCK = os.path.join(REPO, "test", "pty", "mock_lsp.py")
PY = sys.executable  # absolute, no spaces on macOS/Linux CI

PROJ = os.path.realpath(tempfile.mkdtemp(prefix="turbo-lspm-"))
HOME = os.path.realpath(tempfile.mkdtemp(prefix="turbo-lspm-home-"))
ALOG = PROJ + "/alpha.log"
BLOG = PROJ + "/beta.log"

def setup():
    with open(PROJ + "/test.php", "w") as f:
        f.write("<?php\n\necho 'hello world';\n")
    # Two servers for php: the per-language primary + an extra one.
    with open(HOME + "/.turborc", "w") as f:
        f.write("lsp.enabled=1\n")
        f.write("lsp.server.php=%s %s ALPHA %s\n" % (PY, MOCK, ALOG))
        f.write("lsp.extra.beta.command=%s %s BETA %s\n" % (PY, MOCK, BLOG))
        f.write("lsp.extra.beta.langs=php\n")

def run(actions, timeout=30):
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.environ["COLORTERM"] = "truecolor"
        os.environ.pop("TURBO_TEST", None)
        os.environ["HOME"] = HOME
        os.chdir(PROJ)
        os.execv(TURBO, [TURBO, PROJ])
        os._exit(1)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 130, 0, 0))
    out = bytearray()
    def drain(secs):
        end = time.time() + secs
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try: out.extend(os.read(fd, 65536))
                except OSError: break
    def send(data, gap=0.08):
        for ch in data:
            os.write(fd, ch if isinstance(ch, bytes) else ch.encode()); drain(gap)
    drain(3.0)
    try:
        actions(send, drain, out)
    finally:
        try:
            send(["\x11"]); drain(0.5); send(["n"]); drain(0.5)
        except OSError: pass
        try: os.kill(pid, signal.SIGKILL)
        except ProcessLookupError: pass
        try: os.waitpid(pid, 0)
        except ChildProcessError: pass
        os.close(fd)
    return bytes(out)

results = []
def check(name, ok):
    results.append((name, ok)); print(("PASS " if ok else "FAIL ") + name)

setup()

def actions(send, drain, out):
    # Open test.php via Go to Anything -> didOpen fans out to both servers.
    send(["\x10"]); drain(0.6)
    send(list("test.php")); drain(0.6)
    send(["\r"]); drain(2.5)
    # Trigger completion by typing a '.' (auto-trigger). The caret is in the
    # editor after opening; both servers get the request and results merge.
    send(["."]); drain(1.5)

raw = run(actions)
s = stripped(raw)

def readlog(p):
    try:
        return open(p).read()
    except OSError:
        return ""

alog, blog = readlog(ALOG), readlog(BLOG)

# 1. Both servers were started and initialized.
check("both servers started", "start ALPHA" in alog and "start BETA" in blog)
check("both servers initialized", "recv initialize" in alog and "recv initialize" in blog)
# 2. Both received the document open (fan-out).
check("both servers got didOpen",
      "recv textDocument/didOpen" in alog and "recv textDocument/didOpen" in blog)
# 3. Both servers' diagnostics render together (per-source merge, no clobber).
check("alpha diagnostic rendered", "ALPHAdiag" in s)
check("beta diagnostic rendered",  "BETAdiag" in s)
# 4. Completion fanned out to both and merged in the popup.
check("both servers got completion",
      "recv textDocument/completion" in alog and "recv textDocument/completion" in blog)
check("merged completions shown (alpha + beta)",
      "ALPHAone" in s and "BETAone" in s)

# --- Scenario 2: a server that refuses to initialize must not stall others ---
GLOG = PROJ + "/good.log"
XLOG = PROJ + "/bad.log"
with open(HOME + "/.turborc", "w") as f:
    f.write("lsp.enabled=1\n")
    f.write("lsp.server.php=%s %s GOOD %s\n" % (PY, MOCK, GLOG))
    f.write("lsp.extra.bad.command=%s %s BADX %s\n" % (PY, MOCK, XLOG))
    f.write("lsp.extra.bad.langs=php\n")

raw2 = run(actions)
s2 = stripped(raw2)
glog, xlog = readlog(GLOG), readlog(XLOG)
check("bad server was contacted (got initialize)", "recv initialize" in xlog)
check("good server's diagnostic still renders", "GOODdiag" in s2)
# The bad server never becomes ready, so it must not hold back the merge: the
# good server's completions still pop.
check("good server's completions still shown despite the bad server",
      "GOODone" in s2)

shutil.rmtree(PROJ, ignore_errors=True)
shutil.rmtree(HOME, ignore_errors=True)
failed = [n for n, ok in results if not ok]
print()
print("%d/%d passed" % (len(results) - len(failed), len(results)))
sys.exit(1 if failed else 0)

#!/usr/bin/env python3
"""PTY runtime test: Laravel project auto-detection for the language server.

Opening a project that looks like Laravel (an `artisan` file at the root) should,
with NO user config, enable the Laravel LSP for php + blade -- found via the
standard composer-global bin dir (COMPOSER_HOME) even when it isn't on PATH. A
non-Laravel project must not enable it; a Laravel project with the server missing
must warn.

The Laravel LSP is stood in for by mock_lsp.py (tagged LARAVEL) behind a
`laravel-lsp` wrapper in a fake COMPOSER_HOME. HOME is a throwaway so neither the
user's ~/.turborc nor their real composer install interferes.

Usage: python3 test/pty/lsp_laravel_test.py [path-to-turboIDE]
"""
import fcntl, os, pty, re, select, signal, struct, sys, termios, time, \
       shutil, stat, tempfile

def stripped(raw):
    s = re.sub(rb"\x1b\[[0-9;:?]*[A-Za-z]", b"", raw)
    s = re.sub(rb"\x1b\][^\x07\x1b]*(\x07|\x1b\\)", b"", s)
    s = re.sub(rb"\x1b.", b"", s)
    return s.decode("utf-8", "replace")

REPO = os.path.realpath(os.path.join(os.path.dirname(__file__), "..", ".."))
TURBO = os.path.realpath(sys.argv[1] if len(sys.argv) > 1
                         else os.path.join(REPO, "turboIDE"))
MOCK = os.path.join(REPO, "test", "pty", "mock_lsp.py")
PY = sys.executable

def make_composer_home(with_lsp):
    ch = os.path.realpath(tempfile.mkdtemp(prefix="turbo-composer-"))
    log = ch + "/laravel.log"
    if with_lsp:
        bindir = ch + "/vendor/bin"
        os.makedirs(bindir)
        wrapper = bindir + "/laravel-lsp"
        with open(wrapper, "w") as f:
            f.write("#!/bin/sh\nexec %s %s LARAVEL %s\n" % (PY, MOCK, log))
        os.chmod(wrapper, os.stat(wrapper).st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    return ch, log

def run(is_laravel, with_lsp, actions):
    proj = os.path.realpath(tempfile.mkdtemp(prefix="turbo-lar-"))
    home = os.path.realpath(tempfile.mkdtemp(prefix="turbo-lar-home-"))
    ch, log = make_composer_home(with_lsp)
    if is_laravel:
        open(proj + "/artisan", "w").write("#!/usr/bin/env php\n<?php\n")  # Laravel marker
    open(proj + "/index.php", "w").write("<?php\n\necho 'hi';\n")
    open(proj + "/welcome.blade.php", "w").write("<div>{{ $x }}</div>\n")
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"; os.environ["COLORTERM"] = "truecolor"
        os.environ.pop("TURBO_TEST", None)
        os.environ["HOME"] = home
        os.environ["COMPOSER_HOME"] = ch
        os.chdir(proj); os.execv(TURBO, [TURBO, proj]); os._exit(1)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 130, 0, 0))
    out = bytearray()
    def drain(secs):
        end = time.time() + secs
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try: out.extend(os.read(fd, 65536))
                except OSError: break
    def send(data, gap=0.06):
        for c in data:
            os.write(fd, c if isinstance(c, bytes) else c.encode()); drain(gap)
    drain(3.0)
    try:
        actions(send, drain, out)
    finally:
        try:
            send(["\x11"]); drain(0.4); send(["n"]); drain(0.4)
        except OSError: pass
        try: os.kill(pid, signal.SIGKILL)
        except ProcessLookupError: pass
        try: os.waitpid(pid, 0)
        except ChildProcessError: pass
        os.close(fd)
    try: mocklog = open(log).read()
    except OSError: mocklog = ""
    for d in (proj, home, ch):
        shutil.rmtree(d, ignore_errors=True)
    return bytes(out), mocklog

results = []
def check(name, ok):
    results.append((name, ok)); print(("PASS " if ok else "FAIL ") + name)

def open_files(send, drain, out):
    send(["\x10"]); drain(0.6); send(list("index.php")); drain(0.7); send(["\r"]); drain(2.5)
    send(["\x10"]); drain(0.6); send(list("welcome")); drain(0.7); send(["\r"]); drain(2.5)

# --- A. Laravel project + laravel-lsp available: auto-enabled, zero config ----
rawA, logA = run(True, True, open_files)
sA = stripped(rawA)
check("A: laravel-lsp auto-started for a Laravel project (no config)",
      "start LARAVEL" in logA and "recv initialize" in logA)
check("A: php file routed to laravel-lsp", "recv textDocument/didOpen" in logA)
check("A: laravel-lsp diagnostic rendered on the php file", "LARAVELdiag" in sA)
# both php and blade opened -> two didOpen
check("A: both php and blade routed to laravel-lsp",
      logA.count("recv textDocument/didOpen") >= 2)

# --- B. Not a Laravel project: laravel-lsp must NOT start ---------------------
rawB, logB = run(False, True, open_files)
check("B: laravel-lsp NOT started for a non-Laravel project", "start LARAVEL" not in logB)

# --- C. Laravel project, laravel-lsp missing: warn ---------------------------
def just_wait(send, drain, out):
    drain(1.5)  # the warning box pops on project open, before any action
rawC, logC = run(True, False, just_wait)
sC = stripped(rawC)
check("C: warns that this is a Laravel project", "Laravel project" in sC)
# The box wraps text, so assert on tokens with no internal spaces (survive wraps).
check("C: warning names the server and the install target",
      "laravel-lsp" in sC and "laravel/lsp" in sC)

failed = [n for n, ok in results if not ok]
print()
print("%d/%d passed" % (len(results) - len(failed), len(results)))
sys.exit(1 if failed else 0)

#!/usr/bin/env python3
"""PTY runtime test: the Language Servers settings dialog can configure the
multi-server LSP setup -- including the extra servers -- from the UI.

  1. an extra server already in ~/.turborc is loaded, shown, and round-trips
     unchanged on OK;
  2. a new extra server typed into the dialog is persisted to ~/.turborc.

Config is read/written through a throwaway HOME/.turborc.

Usage: python3 test/pty/lsp_dialog_test.py [path-to-turboIDE]
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

def run(torc, actions):
    proj = os.path.realpath(tempfile.mkdtemp(prefix="turbo-lspd-"))
    home = os.path.realpath(tempfile.mkdtemp(prefix="turbo-lspd-home-"))
    open(proj + "/readme.txt", "w").write("x\n")
    open(home + "/.turborc", "w").write(torc)
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"; os.environ["COLORTERM"] = "truecolor"
        os.environ.pop("TURBO_TEST", None); os.environ["HOME"] = home
        os.chdir(proj); os.execv(TURBO, [TURBO, proj]); os._exit(1)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
    out = bytearray()
    def drain(secs):
        end = time.time() + secs
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try: out.extend(os.read(fd, 65536))
                except OSError: break
    def send(data, gap=0.05):
        for ch in data:
            os.write(fd, ch if isinstance(ch, bytes) else ch.encode()); drain(gap)
    drain(2.5)
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
    torc_after = open(home + "/.turborc").read()
    shutil.rmtree(proj, ignore_errors=True); shutil.rmtree(home, ignore_errors=True)
    return bytes(out), torc_after

results = []
def check(name, ok):
    results.append((name, ok)); print(("PASS " if ok else "FAIL ") + name)

def open_dialog(send, drain):
    send(["\x02"]); drain(0.6)               # Ctrl-B: command palette
    send(list("Language Serv")); drain(0.6)
    send(["\r"]); drain(1.0)                 # run -> dialog opens

# --- 1. An existing extra server loads, shows, and round-trips on OK ----------
TORC1 = ("lsp.enabled=1\n"
         "lsp.extra.myls.command=/opt/myls --stdio\n"
         "lsp.extra.myls.langs=php blade\n")

def act1(send, drain, out):
    open_dialog(send, drain)
    globals()["_dlg1"] = len(out)
    drain(0.3)
    send(["\r"]); drain(0.8)                 # Enter -> default OK

raw1, torc1 = run(TORC1, act1)
dlg1 = stripped(raw1[:globals().get("_dlg1", len(raw1))])
check("dialog shows the Extra servers section", "Extra servers" in dlg1)
check("dialog shows the existing extra server", "myls" in dlg1)
check("existing extra server round-trips on OK",
      "lsp.extra.myls.command=/opt/myls --stdio" in torc1 and
      "lsp.extra.myls.langs=php,blade" in torc1)  # saved canonically comma-joined

# --- 2. Adding an extra server through the dialog persists it -----------------
def act2(send, drain, out):
    open_dialog(send, drain)
    # Focus starts on the Enable checkbox. Tab past it + the 7 per-language
    # inputs to reach the first extra "name" field, then fill name/command/langs.
    for _ in range(8):
        send(["\t"]); drain(0.1)
    send(list("added")); drain(0.2)
    send(["\t"]); drain(0.1); send(list("/opt/added --stdio")); drain(0.2)
    send(["\t"]); drain(0.1); send(list("php")); drain(0.2)
    send(["\r"]); drain(0.8)                 # Enter -> default OK

raw2, torc2 = run("lsp.enabled=1\n", act2)
check("added extra server's command persisted",
      "lsp.extra.added.command=/opt/added --stdio" in torc2)
check("added extra server's languages persisted",
      "lsp.extra.added.langs=php" in torc2)

failed = [n for n, ok in results if not ok]
print()
print("%d/%d passed" % (len(results) - len(failed), len(results)))
sys.exit(1 if failed else 0)
